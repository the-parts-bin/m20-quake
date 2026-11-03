// A model of the span engine planned for the m20 (../README.md, "Span
// engine"): Quake's D_DrawSpans8 + D_DrawZSpans for one textured surface,
// split the way the hardware will be.
//
// The driver (M20_SpanSurface, the CPU's part) turns the surface's float
// gradients into the engine's fixed-point registers, once per surface. The
// engine (sc_span, ../m20/firmware/span_core.h) then works in integers only, per span:
//   - s/z, t/z and 1/z at the span's start by multiply-add from the
//     surface's origin and u, v gradients;
//   - every 16 pixels (Quake: 8) the perspective division: 1/zi from a
//     1024-entry reciprocal table with linear interpolation, then
//     s = s/z * (1/zi) + sadjust (a 32 x 18 multiply), clamped as Quake
//     does; the last, shorter segment's step uses a 1/n table instead of a
//     divide;
//   - per pixel: the texel from the surface cache, and 1/z for the
//     z-buffer (as D_DrawZSpans: 2^31 / z, top 16 bits), stepped linearly.
// It isn't pixel-exact with Quake's (16-pixel segments, the table's
// precision), which is fine.
//
// Fixed-point formats (all int32):
//   1/z:       zi * 2^31 (Quake's own z-buffer scale: zi < 1)
//   s/z, t/z:  value * 2^E, E per surface (es, et): the largest the value
//              gets over the screen just fits 30 bits
//   s, t:      16.16 texels, as Quake
#include <math.h>
#include <stdint.h>

#include "quakedef.h"
#include "d_local.h"

#ifndef M20_SPAN_DEFAULT
#define M20_SPAN_DEFAULT "1"
#endif
cvar_t r_m20span = {"r_m20span", M20_SPAN_DEFAULT};   // 0: Quake's own span drawers, 1 the engine, 2 the C model

// The engine's arithmetic (shared with the RTL's test vectors), writing
// into the frame and z buffers
static uint8_t *pix_fb;
static int16_t *pix_zb;
static const uint8_t *pix_tex;
#define SC_PIXEL(i, tex_off, z16) do { pix_fb[i] = pix_tex[tex_off]; pix_zb[i] = (z16); } while (0)
#include "span_core.h"

static sc_regs_t eng;

// ---------------------------------------------------------------------
// The driver: Quake's float state -> the engine's registers
// ---------------------------------------------------------------------

// The exponent that fits the largest |origin + u du + v dv| over the
// screen into 30 bits (a linear function: its extremes are at the corners).
static int32_t exponent(float o, float du, float dv) {
    float w = (float)(vid.width - 1), h = (float)(vid.height - 1);
    float m = fabsf(o);
    float c;
    c = fabsf(o + w * du); if (c > m) m = c;
    c = fabsf(o + h * dv); if (c > m) m = c;
    c = fabsf(o + w * du + h * dv); if (c > m) m = c;
    if (m < 1e-30f) return 60;
    int e;
    frexpf(m, &e);          // m < 2^e
    e = 30 - e;
    if (e > 60) e = 60;
    return e;
}

static int32_t fix(float x, int32_t e) {
    return (int32_t)lrintf(ldexpf(x, e));
}

// ---------------------------------------------------------------------
// The hardware (../m20/rtl/span_engine.v): commands into its FIFO at
// 0x490000 + 4 r (r = 0: a span), the buffers' registers, its status
// ---------------------------------------------------------------------
#define SPAN_HW      ((volatile uint32_t *)0x490000u)
#define SPAN_BUSY    (1u << 31)
#define SDRAM_OFF(p) ((uint32_t)(uintptr_t)(p) - 0x800000u)

// r_m20span, as it was when the frame began: 0 Quake's own drawers, 1 the
// engine, 2 the C model
static int mode;

static void hw_wait(void) {
    while (SPAN_HW[20] & SPAN_BUSY) {
    }
}

// The data cache over [p, p + n): written back and kept (clean), written
// back and dropped (flush), or dropped (inval)
static void cache_clean(const void *p, uint32_t n) {
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}
static void cache_flush(const void *p, uint32_t n) {
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n; a += 64u)
        asm volatile("cbo.flush (%0)" :: "r"(a) : "memory");
}
// (Only lines wholly inside the range are dropped: a line shared with
// other data is written back instead, or that data's unsaved stores would
// be lost. The frame and z buffers are line-aligned, vid_null.c.)
static void cache_inval(const void *p, uint32_t n) {
    uintptr_t lo = ((uintptr_t)p + 63u) & ~(uintptr_t)63, hi = ((uintptr_t)p + n) & ~(uintptr_t)63;
    if (lo > hi) lo = hi;
    if ((uintptr_t)p & 63u) cache_flush(p, 1);
    if (((uintptr_t)p + n) & 63u) cache_flush((const uint8_t *)p + n - 1, 1);
    for (uintptr_t a = lo; a < hi; a += 64u)
        asm volatile("cbo.inval (%0)" :: "r"(a) : "memory");
}

static uint32_t fb_bytes(void) { return (uint32_t)screenwidth * vid.height; }
static uint32_t zb_bytes(void) { return (uint32_t)d_zwidth * vid.height * 2u; }

// Before the world's surfaces: the engine writes the frame and z buffers
// in SDRAM, so whatever the CPU has of them in its cache goes back first.
void M20_SpanFrameBegin(void) {
    mode = (int)r_m20span.value;
    if (mode != 1) return;
    SPAN_HW[16] = SDRAM_OFF(d_viewbuffer);
    SPAN_HW[17] = (uint32_t)screenwidth;
    SPAN_HW[18] = SDRAM_OFF(d_pzbuffer);
    SPAN_HW[19] = (uint32_t)d_zwidth;
    cache_flush(d_viewbuffer, fb_bytes());
    cache_flush(d_pzbuffer, zb_bytes());
}

// After them: wait for the engine, and drop the CPU's (stale) copies, so
// that the models, particles and the rest see what it drew.
void M20_SpanFrameEnd(void) {
    if (mode != 1) return;
    hw_wait();
    cache_inval(d_viewbuffer, fb_bytes());
    cache_inval(d_pzbuffer, zb_bytes());
}

// Around a surface the CPU draws itself (sky, water, the background): the
// engine done first, the CPU's lines written back after, so that the two
// never hold different parts of a line.
void M20_SpanCpuBegin(void) {
    if (mode == 1) hw_wait();
}

void M20_SpanCpuEnd(espan_t *spans) {
    if (mode != 1) return;
    for (espan_t *p = spans; p; p = p->pnext) {
        cache_flush((uint8_t *)d_viewbuffer + p->v * screenwidth + p->u, (uint32_t)p->count);
        cache_flush(d_pzbuffer + p->v * d_zwidth + p->u, 2u * (uint32_t)p->count);
    }
}

// A surface just built in the surface cache (d_surf.c): to SDRAM, where
// the engine reads it.
void M20_SpanSurfaceBuilt(const void *data, int bytes) {
    if (mode == 1) cache_clean(data, (uint32_t)bytes);
}

// Surface cache memory about to be reused (d_surf.c, D_SCAlloc): spans
// still queued may read from it.
void M20_SpanCacheAlloc(void) {
    if (mode == 1) hw_wait();
}

int M20_SpanOn(void) {
    return mode;
}

void M20_SpanSurface(espan_t *spans) {
    static int ready;
    if (!ready) {
        sc_tables();
        ready = 1;
    }
    pix_tex = (const uint8_t *)cacheblock;
    eng.tex_w = cachewidth;
    eng.es = exponent(d_sdivzorigin, d_sdivzstepu, d_sdivzstepv);
    eng.et = exponent(d_tdivzorigin, d_tdivzstepu, d_tdivzstepv);
    eng.s0 = fix(d_sdivzorigin, eng.es);
    eng.su = fix(d_sdivzstepu, eng.es);
    eng.sv = fix(d_sdivzstepv, eng.es);
    eng.t0 = fix(d_tdivzorigin, eng.et);
    eng.tu = fix(d_tdivzstepu, eng.et);
    eng.tv = fix(d_tdivzstepv, eng.et);
    eng.z0 = fix(d_ziorigin, 31);
    eng.zu = fix(d_zistepu, 31);
    eng.zv = fix(d_zistepv, 31);
    eng.sadj = sadjust;
    eng.tadj = tadjust;
    eng.smax = bbextents;
    eng.tmax = bbextentt;
    if (mode == 1) {
        // The registers in the engine's order, then the spans
        SPAN_HW[1] = SDRAM_OFF(cacheblock);
        SPAN_HW[2] = (uint32_t)eng.tex_w | (uint32_t)eng.es << 12 | (uint32_t)eng.et << 18;
        SPAN_HW[3] = (uint32_t)eng.s0;
        SPAN_HW[4] = (uint32_t)eng.su;
        SPAN_HW[5] = (uint32_t)eng.sv;
        SPAN_HW[6] = (uint32_t)eng.t0;
        SPAN_HW[7] = (uint32_t)eng.tu;
        SPAN_HW[8] = (uint32_t)eng.tv;
        SPAN_HW[9] = (uint32_t)eng.z0;
        SPAN_HW[10] = (uint32_t)eng.zu;
        SPAN_HW[11] = (uint32_t)eng.zv;
        SPAN_HW[12] = (uint32_t)eng.sadj;
        SPAN_HW[13] = (uint32_t)eng.tadj;
        SPAN_HW[14] = (uint32_t)eng.smax;
        SPAN_HW[15] = (uint32_t)eng.tmax;
        for (espan_t *p = spans; p; p = p->pnext)
            SPAN_HW[0] = (uint32_t)p->count << 17 | (uint32_t)p->v << 9 | (uint32_t)p->u;
        return;
    }
    for (espan_t *p = spans; p; p = p->pnext) {
        pix_fb = (uint8_t *)d_viewbuffer + p->v * screenwidth + p->u;
        pix_zb = d_pzbuffer + p->v * d_zwidth + p->u;
        sc_span(&eng, p->u, p->v, p->count);
    }
}
