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
// 0: Quake's own span drawers, 1 the engine, 2 the C model, 3 the C model
// with lit spans (the texture and the lightmap at draw time, no surface
// cache, for textures with power-of-2 sizes)
cvar_t r_m20span = {"r_m20span", M20_SPAN_DEFAULT};
// With r_m20span 1: 1 lit spans in the engine (below), 0 the surface cache for all
cvar_t r_m20lit = {"r_m20lit", "1"};

// The engine's arithmetic (shared with the RTL's test vectors), writing
// into the frame and z buffers
static uint16_t *pix_fb;   // (RGB565)
static int16_t *pix_zb;
static const uint8_t *pix_tex;
static const uint8_t *pix_light;
extern uint32_t m20_pal888[256];   // the palette (quakegeneric_m20.c)
extern int m20_pal_changed;
#define SC_PIXEL(i, tex_off, z16) do { pix_fb[i] = d_8to16[pix_tex[tex_off]]; pix_zb[i] = (z16); } while (0)
#define SC_LPIXEL(i, tex_off, scale, z16) \
    do { uint8_t t_ = pix_tex[tex_off]; pix_fb[i] = sc_shade(m20_pal888[t_], t_, scale); pix_zb[i] = (z16); } while (0)
#define SC_LIGHTMAP(k) ((int32_t)pix_light[k])
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
    union { float f; uint32_t u; } b = {m};
    int32_t e = 30 - ((int32_t)(b.u >> 23) - 126);   // m < 2^(exponent - 126), as frexpf (m is normal)
    if (e > 60) e = 60;
    return e;
}

// x 2^e (e in -98..60: a normal float scale), rounded to the nearest
// integer (fcvt saturates what doesn't fit, which the exponent prevents)
static int32_t fix(float x, int32_t e) {
    union { uint32_t u; float f; } scale = {(uint32_t)(e + 127) << 23};
#ifdef __riscv_flen
    int32_t r;
    __asm__("fcvt.w.s %0, %1, rne" : "=r"(r) : "f"(x * scale.f));
    return r;
#else
    return (int32_t)lrintf(x * scale.f);   // (make FPU=0)
#endif
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

// Bumped each time the engine is seen idle: a surface cache entry stamped
// with the current epoch (M20_SpanSurface) may still be read by queued spans.
static int epoch = 1;

static void hw_wait(void) {
    while (SPAN_HW[20] & SPAN_BUSY) {
    }
    epoch++;
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

static uint32_t fb_bytes(void) { return (uint32_t)screenwidth * vid.height * 2u; }
static uint32_t zb_bytes(void) { return (uint32_t)d_zwidth * vid.height * 2u; }

// Before the world's surfaces: the engine writes the frame and z buffers
// in SDRAM, so whatever the CPU has of them in its cache goes back first.
static int hw_lit;          // the engine's lit flag, as last sent (below)
#define LIT_A (1u << 30)   // span command sub-commands (rtl/span_engine.v)
#define LIT_B (2u << 30)

void M20_SpanFrameBegin(void) {
    static int last = -1;
    mode = (int)r_m20span.value;
    // The surface cache's blocks are surfaces or lightmaps depending on
    // the mode: a new mode starts it afresh
    int now = mode * 2 + (r_m20lit.value != 0);
    if (now != last) {
        if (last == 1 * 2 + 1 || last == 1 * 2) hw_wait();
        D_FlushCaches();
        last = now;
    }
    if (mode != 1) return;
    // The palette, when it changed (a flash, a new one)
    if (m20_pal_changed) {
        hw_wait();
        SPAN_HW[24] = 0;
        for (int k = 0; k < 256; k++) SPAN_HW[25] = m20_pal888[k];
        m20_pal_changed = 0;
    }
    // Unlit, which also empties the texel cache: once a frame, so that a
    // new map's textures are read afresh
    SPAN_HW[0] = LIT_B;
    hw_lit = 0;
    SPAN_HW[1] = 0;
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
        cache_flush(d_viewbuffer + p->v * screenwidth + p->u, 2u * (uint32_t)p->count);
        cache_flush(d_pzbuffer + p->v * d_zwidth + p->u, 2u * (uint32_t)p->count);
    }
}

// A surface just built in the surface cache (d_surf.c): to SDRAM, where
// the engine reads it.
void M20_SpanSurfaceBuilt(const void *data, int bytes) {
    if (mode == 1) cache_clean(data, (uint32_t)bytes);
}

// A surface cache entry about to be reused or redrawn (d_surf.c): wait
// only if spans queued since the engine was last idle read from it.
void M20_SpanCacheReuse(surfcache_t *sc) {
    if (mode == 1 && sc->m20epoch == epoch) hw_wait();
}

int M20_SpanOn(void) {
    return mode;
}

// The registers from Quake's gradients (D_CalcGradients)
static void set_regs(void) {
    static int ready;
    if (!ready) {
        sc_tables();
        ready = 1;
    }
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
}

void M20_SpanSurface(espan_t *spans) {
    set_regs();
    pix_tex = (const uint8_t *)cacheblock;
    eng.tex_w = cachewidth;
    if (mode == 1) {
        ((surfcache_t *)((uint8_t *)cacheblock - offsetof(surfcache_t, data)))->m20epoch = epoch;
        if (hw_lit) {
            SPAN_HW[0] = LIT_B;
            hw_lit = 0;
        }
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
            SPAN_HW[0] = (uint32_t)p->count << 19 | (uint32_t)p->v << 10 | (uint32_t)p->u;
        return;
    }
    for (espan_t *p = spans; p; p = p->pnext) {
        pix_fb = d_viewbuffer + p->v * screenwidth + p->u;
        pix_zb = d_pzbuffer + p->v * d_zwidth + p->u;
        sc_span(&eng, p->u, p->v, p->count);
    }
}

// ---------------------------------------------------------------------
// Lit spans: the texture and the lightmap at draw time
// ---------------------------------------------------------------------
// For a surface whose texture has power-of-2 sizes, no surface cache
// block: the engine (or the C model, r_m20span 3) reads the texture
// itself, wrapping, and lights each pixel from the surface's lightmap
// through the colormap. The lightmaps (8 bits a sample: Quake's light
// value >> 6) are cached in the surface cache instead, each in a block of
// its own, rebuilt as D_CacheSurface rebuilds a surface: when a light
// style changed or a dynamic light touches it.


texture_t *R_TextureAnimation(texture_t *base);   // r_surf.c
void R_BuildLightMap(void);
surfcache_t *D_SCAlloc(int width, int size);   // d_surf.c
extern unsigned blocklights[18 * 18];


static int log2_pow2(int x) {
    return (x > 0 && !(x & (x - 1))) ? __builtin_ctz((unsigned)x) : -1;
}

// Whether d_edge.c should draw this surface with lit spans (else through
// the surface cache)
int M20_SpanLit(msurface_t *pface, int miplevel) {
    if (!(mode == 3 || (mode == 1 && r_m20lit.value))) return 0;
    texture_t *mt = pface->texinfo->texture;
    return log2_pow2(mt->width >> miplevel) >= 0 && log2_pow2(mt->height >> miplevel) >= 0;
}

// The surface's lightmap, 64-byte aligned (the engine loads it by lines)
static uint8_t *lightmap(msurface_t *pface, int lw, int lh) {
    int adj[MAXLIGHTMAPS];
    for (int k = 0; k < MAXLIGHTMAPS; k++) adj[k] = d_lightstylevalue[pface->styles[k]];
    surfcache_t *c = pface->cachespots[0];
    if (c && !c->dlight && pface->dlightframe != r_framecount && c->lightadj[0] == adj[0] &&
        c->lightadj[1] == adj[1] && c->lightadj[2] == adj[2] && c->lightadj[3] == adj[3])
        return (uint8_t *)(((uintptr_t)c->data + 63u) & ~(uintptr_t)63);
    if (!c) {
        c = D_SCAlloc(lw, lw * lh + 64);
        pface->cachespots[0] = c;
        c->owner = &pface->cachespots[0];
        c->mipscale = 1.0f;
    } else {
        M20_SpanCacheReuse(c);   // rebuilt in place: queued spans may still read it
    }
    c->dlight = pface->dlightframe == r_framecount;
    for (int k = 0; k < MAXLIGHTMAPS; k++) c->lightadj[k] = r_drawsurf.lightadj[k] = adj[k];
    r_drawsurf.surf = pface;
    R_BuildLightMap();
    uint8_t *lm = (uint8_t *)(((uintptr_t)c->data + 63u) & ~(uintptr_t)63);
    for (int k = 0; k < lw * lh; k++) lm[k] = (uint8_t)(blocklights[k] >> 6);
    if (mode == 1) cache_clean(lm, (uint32_t)(lw * lh));
    return lm;
}

void M20_SpanSurfaceLit(espan_t *spans, msurface_t *pface, int miplevel) {
    texture_t *mt = R_TextureAnimation(pface->texinfo->texture);
    int lw = (pface->extents[0] >> 4) + 1, lh = (pface->extents[1] >> 4) + 1;
    const uint8_t *lm = lightmap(pface, lw, lh);
    set_regs();
    eng.tw_log2 = log2_pow2(mt->width >> miplevel);
    eng.th_log2 = log2_pow2(mt->height >> miplevel);
    eng.soff = (pface->texturemins[0] >> miplevel) & ((1 << eng.tw_log2) - 1);
    eng.toff = (pface->texturemins[1] >> miplevel) & ((1 << eng.th_log2) - 1);
    eng.lshift = 4 - miplevel;
    eng.lw = lw;
    const uint8_t *tex = (const uint8_t *)mt + mt->offsets[miplevel];
    if (mode == 1) {
        if (!mt->m20clean) {
            // the whole miptex (all four levels) into SDRAM, once
            cache_clean(mt, mt->offsets[3] + (mt->width >> 3) * (mt->height >> 3));
            mt->m20clean = 1;
        }
        ((surfcache_t *)pface->cachespots[0])->m20epoch = epoch;
        SPAN_HW[0] = LIT_B | 1u << 27 | (uint32_t)lh << 22 | (uint32_t)lw << 17 | SDRAM_OFF(lm) >> 6;
        SPAN_HW[0] = LIT_A | (uint32_t)eng.lshift << 26 | (uint32_t)eng.th_log2 << 22 |
                     (uint32_t)eng.tw_log2 << 18 | (uint32_t)eng.toff << 9 | (uint32_t)eng.soff;
        hw_lit = 1;
        SPAN_HW[1] = SDRAM_OFF(tex);
        SPAN_HW[2] = (uint32_t)eng.es << 12 | (uint32_t)eng.et << 18;
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
            SPAN_HW[0] = (uint32_t)p->count << 19 | (uint32_t)p->v << 10 | (uint32_t)p->u;
        return;
    }
    pix_tex = tex;
    pix_light = lm;
    for (espan_t *p = spans; p; p = p->pnext) {
        pix_fb = d_viewbuffer + p->v * screenwidth + p->u;
        pix_zb = d_pzbuffer + p->v * d_zwidth + p->u;
        sc_span_lit(&eng, p->u, p->v, p->count);
    }
}
