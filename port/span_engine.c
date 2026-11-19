// A model of the span engine planned for the m20 (../README.md, "Span
// engine"): Quake's D_DrawSpans8 + D_DrawZSpans for one textured surface,
// split the way the hardware will be.
//
// The driver (M20_SpanSurface, the CPU's part) turns the surface's float
// gradients into the engine's fixed-point registers, once per surface. The
// engine (sc_span, ../core/firmware/span_core.h) then works in integers only, per span:
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
    if (e < 0) e = 0;   // (the engine's exponent is 6 bits, unsigned; origin() wraps)
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

// A linear function's value at the screen's corner, which the engine starts
// from, scaled by 2^e. On a steep surface it can be far beyond what the visible
// pixels reach, the more so on a wider screen: it would saturate. So it's taken
// at the centre, and stepped back in wrapping integers, as the engine's adders
// work: what's right modulo 2^32 is right on screen.
static int32_t origin(float o, float du, float dv, int32_t iu, int32_t iv, int32_t e) {
    int32_t cu = vid.width / 2, cv = vid.height / 2;
    return (int32_t)((uint32_t)fix(o + (float)cu * du + (float)cv * dv, e)
                     - (uint32_t)cu * (uint32_t)iu - (uint32_t)cv * (uint32_t)iv);
}

// ---------------------------------------------------------------------
// The hardware (../core/rtl/span_engine.v): commands into its FIFO at
// 0x490000 + 4 r (r = 0: a span), the buffers' registers, its status
// (SPAN_W, SPAN_R, SDRAM_OFF: span_hw.h)
// ---------------------------------------------------------------------
#include "span_hw.h"
#define SPAN_BUSY    (1u << 31)

// r_m20span, as it was when the frame began: 0 Quake's own drawers, 1 the
// engine, 2 the C model
static int mode;

// Bumped each time the engine is seen idle: a surface cache entry stamped
// with the current epoch (M20_SpanSurface) may still be read by queued spans.
static int epoch = 1;

static void hw_wait(void) {
    while (SPAN_R(20) & SPAN_BUSY) {
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

static int eng_owns;    // the engine may be drawing; the CPU holds none of the buffers' lines (below)
static int ztest_on;    // SPAN_ZTEST as last set
extern cvar_t r_m20alias;

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
        SPAN_W(24, 0);
        for (int k = 0; k < 256; k++) SPAN_W(25, m20_pal888[k]);
        m20_pal_changed = 0;
    }
    // Unlit, which also empties the texel cache: once a frame, so that a
    // new map's textures are read afresh
    SPAN_W(0, LIT_B);
    hw_lit = 0;
    SPAN_W(1, 0);
    // (the engine takes one stride for both, and z a multiple of 2 KB past
    // the frame: its z cache's index bits are the frame address's)
    if ((((uintptr_t)d_pzbuffer - (uintptr_t)d_viewbuffer) & 2047u) || d_zwidth != screenwidth)
        Sys_Error("span engine: z not a multiple of 2 KB past the frame, or another stride");
    // The bases only while the engine is idle: it sends z lines out at
    // their frame line + (z base - frame base), so a new frame base under
    // the last frame's models' z write-backs puts them a display buffer
    // away from the z buffer (into .bss)
    if (eng_owns) {
        M20_PERF_BEGIN(M20_PERF_AWAIT);
        hw_wait();
        M20_PERF_END(M20_PERF_AWAIT);
    }
    SPAN_W(16, SDRAM_OFF(d_viewbuffer));
    SPAN_W(17, (uint32_t)screenwidth);
    SPAN_W(18, SDRAM_OFF(d_pzbuffer));
    SPAN_W(19, (uint32_t)d_zwidth);
    cache_flush(d_viewbuffer, fb_bytes());
    cache_flush(d_pzbuffer, zb_bytes());
    eng_owns = 1;
}

// After them: the engine keeps the buffers for the alias models
// (r_m20alias; the CPU takes them when it draws, M20_SpanCpuOwn), else
// they're the CPU's from here: the engine waited for, the CPU's (stale)
// copies dropped, so that the models, particles and the rest see what it
// drew.
void M20_SpanCpuOwn(void);
void M20_SpanFrameEnd(void) {
    if (mode != 1 || r_m20alias.value) return;
    M20_SpanCpuOwn();
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
    eng.su = fix(d_sdivzstepu, eng.es);
    eng.sv = fix(d_sdivzstepv, eng.es);
    eng.s0 = origin(d_sdivzorigin, d_sdivzstepu, d_sdivzstepv, eng.su, eng.sv, eng.es);
    eng.tu = fix(d_tdivzstepu, eng.et);
    eng.tv = fix(d_tdivzstepv, eng.et);
    eng.t0 = origin(d_tdivzorigin, d_tdivzstepu, d_tdivzstepv, eng.tu, eng.tv, eng.et);
    eng.zu = fix(d_zistepu, 31);
    eng.zv = fix(d_zistepv, 31);
    eng.z0 = origin(d_ziorigin, d_zistepu, d_zistepv, eng.zu, eng.zv, 31);
    eng.sadj = sadjust;
    eng.tadj = tadjust;
    eng.smax = bbextents;
    eng.tmax = bbextentt;
}

#ifdef M20_SPAN_CHECK
// After each surface: the engine's spans against the C model's, from SDRAM.
// (printf per span found wrong, up to a limit a frame; the second read tells
// a wrong read from wrong memory.)
static uint16_t chk_fb[1024];
static int16_t chk_zb[1024];
static uint32_t chk_spans, chk_bad;
static void check(espan_t *spans, int lit, const char *name, int mip) {
    static int printed, frame = -1;
    if (frame != r_framecount) {
        frame = r_framecount;
        printed = 0;
    }
    hw_wait();
    uint16_t *fb = pix_fb;
    for (espan_t *p = spans; p; p = p->pnext) {
        pix_fb = chk_fb;
        pix_zb = chk_zb;
        if (lit) sc_span_lit(&eng, p->u, p->v, p->count);
        else sc_span(&eng, p->u, p->v, p->count);
        uint16_t *f = d_viewbuffer + p->v * screenwidth + p->u;
        int16_t *z = (int16_t *)d_pzbuffer + p->v * d_zwidth + p->u;
        cache_flush(f, 2u * (uint32_t)p->count);
        cache_flush(z, 2u * (uint32_t)p->count);
        int nf = 0, nz = 0, first = -1, last = -1;
        for (int i = 0; i < p->count; i++) {
            int bf = f[i] != chk_fb[i], bz = z[i] != chk_zb[i];
            nf += bf;
            nz += bz;
            if (bf || bz) {
                if (first < 0) first = i;
                last = i;
            }
        }
        chk_spans++;
        if (nf || nz) {
            chk_bad++;
            if (printed++ < 6) {
                uint16_t gf = f[first], gz = (uint16_t)z[first];
                cache_flush(f, 2u * (uint32_t)p->count);
                cache_flush(z, 2u * (uint32_t)p->count);
                int again = 0;
                for (int i = 0; i < p->count; i++) again += f[i] != chk_fb[i] || z[i] != chk_zb[i];
                printf("chk f%d %s m%d lit%d u%d v%d n%d badf %d badz %d at %d..%d got %04x/%04x want %04x/%04x again %d tex %08lx\n",
                       r_framecount, name, mip, lit, p->u, p->v, p->count, nf, nz, first, last,
                       gf, gz, chk_fb[first], (uint16_t)chk_zb[first], again, (unsigned long)SDRAM_OFF(pix_tex));
            }
        }
        cache_flush(f, 2u * (uint32_t)p->count);
        cache_flush(z, 2u * (uint32_t)p->count);
    }
    pix_fb = fb;
}
void M20_SpanCheckReport(void) {
    printf("chk total spans %lu bad %lu\n", (unsigned long)chk_spans, (unsigned long)chk_bad);
}
#endif

void M20_SpanSurface(espan_t *spans) {
    set_regs();
    pix_tex = (const uint8_t *)cacheblock;
    eng.tex_w = cachewidth;
    if (mode == 1) {
        ((surfcache_t *)((uint8_t *)cacheblock - offsetof(surfcache_t, data)))->m20epoch = epoch;
        if (hw_lit) {
            SPAN_W(0, LIT_B);
            hw_lit = 0;
        }
        // The registers in the engine's order, then the spans
        SPAN_W(1, SDRAM_OFF(cacheblock));
        SPAN_W(2, (uint32_t)eng.tex_w | (uint32_t)eng.es << 12 | (uint32_t)eng.et << 18);
        SPAN_W(3, (uint32_t)eng.s0);
        SPAN_W(4, (uint32_t)eng.su);
        SPAN_W(5, (uint32_t)eng.sv);
        SPAN_W(6, (uint32_t)eng.t0);
        SPAN_W(7, (uint32_t)eng.tu);
        SPAN_W(8, (uint32_t)eng.tv);
        SPAN_W(9, (uint32_t)eng.z0);
        SPAN_W(10, (uint32_t)eng.zu);
        SPAN_W(11, (uint32_t)eng.zv);
        SPAN_W(12, (uint32_t)eng.sadj);
        SPAN_W(13, (uint32_t)eng.tadj);
        SPAN_W(14, (uint32_t)eng.smax);
        SPAN_W(15, (uint32_t)eng.tmax);
        for (espan_t *p = spans; p; p = p->pnext)
            SPAN_W(0, (uint32_t)p->count << 19 | (uint32_t)p->v << 10 | (uint32_t)p->u);
#ifdef M20_SCAN   // (port/m20_scan.c: with the engine at work)
        { extern void M20_ScanStep(void); M20_ScanStep(); }
#endif
#ifdef M20_SPAN_SERIAL   // no overlap with the CPU, to look for races
        hw_wait();
#endif
#ifdef M20_SPAN_CHECK
        check(spans, 0, "cache", 0);
#endif
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
        SPAN_W(0, LIT_B | 1u << 27 | (uint32_t)lh << 22 | (uint32_t)lw << 17 | SDRAM_OFF(lm) >> 6);
        SPAN_W(0, LIT_A | (uint32_t)eng.lshift << 26 | (uint32_t)eng.th_log2 << 22 |
                     (uint32_t)eng.tw_log2 << 18 | (uint32_t)eng.toff << 9 | (uint32_t)eng.soff);
        hw_lit = 1;
        SPAN_W(1, SDRAM_OFF(tex));
        SPAN_W(2, (uint32_t)eng.es << 12 | (uint32_t)eng.et << 18);
        SPAN_W(3, (uint32_t)eng.s0);
        SPAN_W(4, (uint32_t)eng.su);
        SPAN_W(5, (uint32_t)eng.sv);
        SPAN_W(6, (uint32_t)eng.t0);
        SPAN_W(7, (uint32_t)eng.tu);
        SPAN_W(8, (uint32_t)eng.tv);
        SPAN_W(9, (uint32_t)eng.z0);
        SPAN_W(10, (uint32_t)eng.zu);
        SPAN_W(11, (uint32_t)eng.zv);
        SPAN_W(12, (uint32_t)eng.sadj);
        SPAN_W(13, (uint32_t)eng.tadj);
        SPAN_W(14, (uint32_t)eng.smax);
        SPAN_W(15, (uint32_t)eng.tmax);
        for (espan_t *p = spans; p; p = p->pnext)
            SPAN_W(0, (uint32_t)p->count << 19 | (uint32_t)p->v << 10 | (uint32_t)p->u);
#ifdef M20_SCAN   // (port/m20_scan.c: with the engine at work)
        { extern void M20_ScanStep(void); M20_ScanStep(); }
#endif
#ifdef M20_SPAN_SERIAL   // no overlap with the CPU, to look for races
        hw_wait();
#endif
#ifdef M20_SPAN_CHECK
        pix_tex = tex;
        pix_light = lm;
        check(spans, 1, mt->name, miplevel);
#endif
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

// ---------------------------------------------------------------------
// After the world: who has the frame and z buffers
// ---------------------------------------------------------------------
// With r_m20alias the engine goes on drawing after the world (the alias
// models), so the CPU doesn't wait for it there: whatever the CPU draws
// into the 3D view next (sprites, particles, a model the engine doesn't
// take) waits first and drops its stale lines of both buffers
// (M20_SpanCpuOwn); an engine model after that has the CPU's lines
// written back first (eng_own). The depth test is a direct register, as
// is the z base: both changed only while the engine is idle (writing the
// z base drops the z cache, dirty or not).
cvar_t r_m20alias = {"r_m20alias", "1"};

void M20_SpanCpuOwn(void) {
    if (mode != 1 || !eng_owns) return;
    M20_PERF_BEGIN(M20_PERF_CPUWAIT);
    hw_wait();
    M20_PERF_END(M20_PERF_CPUWAIT);
    if (ztest_on) {
        SPAN_W(26, 0);
        ztest_on = 0;
    }
    cache_inval(d_viewbuffer, fb_bytes());
    cache_inval(d_pzbuffer, zb_bytes());
    eng_owns = 0;
}

static void eng_own(void) {
    if (eng_owns) return;
    cache_flush(d_viewbuffer, fb_bytes());
    cache_flush(d_pzbuffer, zb_bytes());
    eng_owns = 1;
}

static void ztest(void) {
    if (ztest_on) return;
    M20_PERF_BEGIN(M20_PERF_AWAIT);
    hw_wait();                             // (the world's spans done: no depth test for them)
    M20_PERF_END(M20_PERF_AWAIT);
    SPAN_W(18, SDRAM_OFF(d_pzbuffer));   // the z cache emptied
    SPAN_W(26, 1);
    ztest_on = 1;
}

// ---------------------------------------------------------------------
// Alias models (the monsters, the weapon) in the engine
// ---------------------------------------------------------------------
// D_PolysetDraw's triangles (r_affinetridesc), as TRX draws its faces
// (trx/src/specific/s_hwr_span.h): per triangle the planes of 1/z, s/z and
// t/z on the screen from its three vertices (exact: the engine maps the
// skin with perspective, where Quake's own drawer is affine), the light
// (Gouraud: each vertex's, as the engine's brightness) fitted as
// a + b s + c t, exact for a triangle, and sampled into a small lightmap
// (bilinear gives the plane back), and the rows by the engine's edge walk
// (EDGE). The skin is a lit texture of any width (LIT_A's tw_log2 15: rows
// tex_w apart, s and t not wrapped), read in place in the model (model.c
// cleans it to SDRAM when it's loaded, zone.c when the cache moves it),
// from the triangle's corner. (Padding each skin's rows to a power of 2
// instead cost 0.7 MB, which pushed models out of the cache.) The depth
// test on, against the world's z.
// Models drawn by Quake's subdivision (small, wholly on screen) and with
// a player's colour translation stay on the CPU.
extern void *acolormap;   // r_alias.c

// r_m20far 1: the models Quake would subdivide (small, wholly on screen:
// far monsters, torches) to the engine too (per triangle setup on the CPU
// either way: which is faster depends on the scene)
cvar_t r_m20far = {"r_m20far", "0"};

int M20_AliasEngine(void) {
    return mode == 1 && r_m20alias.value && (!r_affinetridesc.drawtype || r_m20far.value) &&
           acolormap == (void *)vid.colormap && r_affinetridesc.skinwidth < 4096;   // (tex_w: 12 bits)
}

// While the engine draws the models, what the CPU would draw among them
// (sprites, the models above it doesn't take) waits for the end of the
// list, after the view model (r_main.c, M20_DrawDeferred): one hand-over
// of the buffers a frame, before the particles, instead of one for each
// of them and back. All of them test and write z, so the order doesn't
// change the picture.
int m20_deferring;                       // set by R_DrawEntitiesOnList
static entity_t *m20_later[MAX_VISEDICTS];
static int m20_nlater;

int M20_DeferOn(void) {
    m20_nlater = 0;
    return mode == 1 && r_m20alias.value;
}

void M20_Defer(entity_t *e) {
    if (m20_nlater < MAX_VISEDICTS) m20_later[m20_nlater++] = e;
}

// R_AliasDrawModel, its setup done: a model for the CPU, put off?
int M20_AliasDefer(void) {
    if (!m20_deferring || M20_AliasEngine()) return 0;
    M20_Defer(currententity);
    return 1;
}

int M20_Deferred(entity_t ***list) {   // (and empties the list: taken once)
    int n = m20_nlater;
    m20_nlater = 0;
    *list = m20_later;
    return n;
}

#define LM_RING 16384
static uint8_t lm_ring[LM_RING] __attribute__((aligned(64)));
static uint32_t lm_pos;
static int32_t eng_clip_l = -1, eng_clip_r = -1;   // EDGE's window, as last sent

typedef struct { int32_t px, py; float s, t, w, l; } avert_t;
typedef struct { int32_t y0, y1, x, dx; } aedge_t;

static inline int32_t floor_i(float f) {
    int32_t i = (int32_t)f;
    return i - (f < (float)i);
}

static void alias_tri(const avert_t *v, uint32_t skin, uint32_t skin_w) {
    float x1 = (float)(v[1].px - v[0].px), y1 = (float)(v[1].py - v[0].py);
    float x2 = (float)(v[2].px - v[0].px), y2 = (float)(v[2].py - v[0].py);
    float det = x1 * y2 - x2 * y1;
    if (fabsf(det) < 1.0f) return;   // (under half a pixel)
    float smin = v[0].s, smax = v[0].s, tmin = v[0].t, tmax = v[0].t;
    for (int i = 1; i < 3; i++) {
        smin = v[i].s < smin ? v[i].s : smin;
        smax = v[i].s > smax ? v[i].s : smax;
        tmin = v[i].t < tmin ? v[i].t : tmin;
        tmax = v[i].t > tmax ? v[i].t : tmax;
    }
    int32_t umin = floor_i(smin), vmin = floor_i(tmin), umax = floor_i(smax), vmax = floor_i(tmax);
    if (umin < 0) umin = 0;
    if (vmin < 0) vmin = 0;
    if (umax < umin) umax = umin;
    if (vmax < vmin) vmax = vmin;
    float id = 1.0f / det;
#define PLANE(q0, q1, q2, o, dx, dy) do { \
        float d1_ = (q1) - (q0), d2_ = (q2) - (q0); \
        dx = (d1_ * y2 - d2_ * y1) * id; \
        dy = (x1 * d2_ - x2 * d1_) * id; \
        o = (q0) - (float)v[0].px * dx - (float)v[0].py * dy; \
    } while (0)
    float zo, zdx, zdy, so, sdx, sdy, to, tdx, tdy;
    PLANE(v[0].w, v[1].w, v[2].w, zo, zdx, zdy);
    PLANE((v[0].s - umin) * v[0].w, (v[1].s - umin) * v[1].w, (v[2].s - umin) * v[2].w, so, sdx, sdy);
    PLANE((v[0].t - vmin) * v[0].w, (v[1].t - vmin) * v[1].w, (v[2].t - vmin) * v[2].w, to, tdx, tdy);
#undef PLANE
    eng.es = exponent(so, sdx, sdy);
    eng.et = exponent(to, tdx, tdy);
    eng.su = fix(sdx, eng.es);
    eng.sv = fix(sdy, eng.es);
    eng.s0 = origin(so, sdx, sdy, eng.su, eng.sv, eng.es);
    eng.tu = fix(tdx, eng.et);
    eng.tv = fix(tdy, eng.et);
    eng.t0 = origin(to, tdx, tdy, eng.tu, eng.tv, eng.et);
    eng.zu = fix(zdx, 31);
    eng.zv = fix(zdy, 31);
    eng.z0 = origin(zo, zdx, zdy, eng.zu, eng.zv, 31);
    eng.smax = ((umax - umin) << 16) | 0xFFFF;
    eng.tmax = ((vmax - vmin) << 16) | 0xFFFF;
    eng.soff = 0;   // (the texture from the triangle's corner: tex_base)
    eng.toff = 0;

    // The light: l = a + b s + c t through the three vertices (s, t from
    // umin, vmin); a line in the skin: their mean
    float la, lb = 0.0f, lc = 0.0f;
    {
        float s1 = v[1].s - v[0].s, t1 = v[1].t - v[0].t, s2 = v[2].s - v[0].s, t2 = v[2].t - v[0].t;
        float dd = s1 * t2 - s2 * t1;
        if (fabsf(dd) > 1e-3f) {
            float l1 = v[1].l - v[0].l, l2 = v[2].l - v[0].l, idd = 1.0f / dd;
            lb = (l1 * t2 - l2 * t1) * idd;
            lc = (s1 * l2 - s2 * l1) * idd;
            la = v[0].l - lb * (v[0].s - umin) - lc * (v[0].t - vmin);
        } else {
            la = (v[0].l + v[1].l + v[2].l) * (1.0f / 3.0f);
        }
    }
    // The lightmap: the plane every 2^k texels (bilinear gives it back), the
    // coarsest grid that keeps it in 0..255, else a finer one, clamped
    int32_t ext = umax - umin > vmax - vmin ? umax - umin : vmax - vmin, k = 0;
    while ((ext >> k) > 1 && k < 7) k++;
    int32_t lw, lh, q0, qs, qt, clamp = 0;
    for (;;) {
        lw = ((umax - umin) >> k) + 2;
        lh = ((vmax - vmin) >> k) + 2;
        q0 = (int32_t)(la * 65536.0f) + 32768;
        qs = (int32_t)(lb * (float)(65536 << k));
        qt = (int32_t)(lc * (float)(65536 << k));
        int32_t c1 = q0 + qs * (lw - 1), c2 = q0 + qt * (lh - 1), c3 = c1 + qt * (lh - 1);
        int32_t qmin = q0 < c1 ? q0 : c1, qmax = q0 < c1 ? c1 : q0;
        qmin = c2 < qmin ? c2 : qmin;
        qmax = c2 > qmax ? c2 : qmax;
        qmin = c3 < qmin ? c3 : qmin;
        qmax = c3 > qmax ? c3 : qmax;
        if (qmin >= 0 && qmax < (256 << 16) || clamp) break;
        k = 0;   // a finer grid (up to 8 x 8), clamped
        while ((ext >> k) > 6) k++;
        clamp = 1;
    }
    uint32_t bytes = (uint32_t)(lw * lh + 63) & ~63u;
    if (lm_pos + bytes > LM_RING) {   // the ring full: reused once the engine has loaded them all
        M20_PERF_BEGIN(M20_PERF_AWAIT);
        hw_wait();
        M20_PERF_END(M20_PERF_AWAIT);
        lm_pos = 0;
    }
    uint8_t *lm = lm_ring + lm_pos;
    lm_pos += bytes;
    uint8_t *row = lm;
    for (int32_t j = 0; j < lh; j++, q0 += qt, row += lw) {
        int32_t q = q0;
        for (int32_t i = 0; i < lw; i++, q += qs) {
            int32_t s = q >> 16;
            row[i] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
        }
    }
    cache_clean(lm, (uint32_t)(lw * lh));

    SPAN_W(0, LIT_B | 1u << 27 | (uint32_t)lh << 22 | (uint32_t)lw << 17 | SDRAM_OFF(lm) >> 6);
    SPAN_W(0, LIT_A | (uint32_t)k << 26 | 15u << 22 | 15u << 18);   // (any width, no wrap; soff, toff 0)
    SPAN_W(1, skin + (uint32_t)vmin * skin_w + (uint32_t)umin);
    SPAN_W(2, skin_w | (uint32_t)eng.es << 12 | (uint32_t)eng.et << 18);
    SPAN_W(3, (uint32_t)eng.s0);
    SPAN_W(4, (uint32_t)eng.su);
    SPAN_W(5, (uint32_t)eng.sv);
    SPAN_W(6, (uint32_t)eng.t0);
    SPAN_W(7, (uint32_t)eng.tu);
    SPAN_W(8, (uint32_t)eng.tv);
    SPAN_W(9, (uint32_t)eng.z0);
    SPAN_W(10, (uint32_t)eng.zu);
    SPAN_W(11, (uint32_t)eng.zv);
    SPAN_W(12, 0);
    SPAN_W(13, 0);
    SPAN_W(14, (uint32_t)eng.smax);
    SPAN_W(15, (uint32_t)eng.tmax);

    // The rows: the edges going down on one side, up on the other; EDGE
    // per pair, an edge the engine already has going on where it got to.
    // (Rows y0 .. y1 - 1, each from x to x of the other edge, exclusive:
    // neighbouring triangles meet without gaps or overlaps.)
    aedge_t el[3], er[3];
    int32_t nl = 0, nr = 0, ytop = 1 << 20, ybot = -(1 << 20);
    for (int i = 0; i < 3; i++) {
        const avert_t *a = &v[i], *b = &v[i + 1 < 3 ? i + 1 : 0];
        if (a->py == b->py) continue;
        aedge_t *e;
        if (a->py < b->py) {
            e = &er[nr++];
        } else {
            const avert_t *t = a;
            a = b;
            b = t;
            e = &el[nl++];
        }
        e->y0 = a->py;
        e->y1 = b->py;
        e->dx = ((b->px - a->px) << 16) / (b->py - a->py);
        e->x = (a->px << 16) + 0x8000;
        ytop = a->py < ytop ? a->py : ytop;
        ybot = b->py > ybot ? b->py : ybot;
    }
    int32_t clip_t = r_refdef.vrect.y, clip_b = r_refdef.vrect.y + r_refdef.vrect.height - 1;
    int32_t clip_l = r_refdef.vrect.x, clip_r = r_refdef.vrect.x + r_refdef.vrect.width - 1;
    int32_t ys = ytop < clip_t ? clip_t : ytop, ye = ybot > clip_b + 1 ? clip_b + 1 : ybot;
    if (!nl || !nr || ys >= ye) return;
    for (int i = 1; i < nl; i++)   // (by y0: two at most)
        if (el[i].y0 < el[0].y0) { aedge_t t = el[0]; el[0] = el[i]; el[i] = t; }
    for (int i = 1; i < nr; i++)
        if (er[i].y0 < er[0].y0) { aedge_t t = er[0]; er[0] = er[i]; er[i] = t; }
    const aedge_t *l = el, *le = el + nl, *r = er, *re = er + nr, *sl = NULL, *sr = NULL;
    for (int32_t y = ys; y < ye;) {
        while (y >= l->y1 && ++l < le) {
        }
        while (y >= r->y1 && ++r < re) {
        }
        if (l >= le || r >= re) break;
        if (y < l->y0 || y < r->y0) {   // (not a triangle's: can't happen)
            y++;
            sl = sr = NULL;
            continue;
        }
        int32_t yend = l->y1 < r->y1 ? l->y1 : r->y1;
        yend = yend > ye ? ye : yend;
        uint32_t fc = clip_l != eng_clip_l || clip_r != eng_clip_r, fl = l != sl, fr = r != sr;
        SPAN_W(0, 3u << 30 | fl << 29 | fr << 28 | fc << 27 | (uint32_t)y << 9 | (uint32_t)yend);
        if (fc) {
            SPAN_W(0, (uint32_t)clip_r << 16 | (uint32_t)clip_l);
            eng_clip_l = clip_l;
            eng_clip_r = clip_r;
        }
        if (fl) {
            SPAN_W(0, (uint32_t)(l->x + l->dx * (y - l->y0)));
            SPAN_W(0, (uint32_t)l->dx);
        }
        if (fr) {
            SPAN_W(0, (uint32_t)(r->x + r->dx * (y - r->y0)));
            SPAN_W(0, (uint32_t)r->dx);
        }
        sl = l;
        sr = r;
        y = yend;
    }
}

// D_PolysetDraw's triangles, when M20_AliasEngine says so
void M20_AliasDraw(void) {
    eng_own();
    ztest();
    M20_PERF_BEGIN(M20_PERF_ADRAW);
    hw_lit = 1;   // (the next unlit surface sends LIT_B again)
    // The skin in the model (in SDRAM since it was loaded: model.c)
    uint32_t skin = SDRAM_OFF(r_affinetridesc.pskin), skin_w = (uint32_t)r_affinetridesc.skinwidth;
    finalvert_t *pfv = r_affinetridesc.pfinalverts;
    mtriangle_t *ptri = r_affinetridesc.ptriangles;
    for (int n = 0; n < r_affinetridesc.numtriangles; n++, ptri++) {
        finalvert_t *f[3] = {pfv + ptri->vertindex[0], pfv + ptri->vertindex[1], pfv + ptri->vertindex[2]};
        int xdenom = (f[0]->v[1] - f[1]->v[1]) * (f[0]->v[0] - f[2]->v[0]) -
                     (f[0]->v[0] - f[1]->v[0]) * (f[0]->v[1] - f[2]->v[1]);
        if (xdenom >= 0) continue;   // (facing away: as D_DrawNonSubdiv)
        avert_t v[3];
        for (int i = 0; i < 3; i++) {
            int s = f[i]->v[2];
            if (!ptri->facesfront && (f[i]->flags & ALIAS_ONSEAM)) s += r_affinetridesc.seamfixupX16;
            v[i].px = f[i]->v[0];
            v[i].py = f[i]->v[1];
            v[i].s = (float)s * (1.0f / 65536.0f);
            v[i].t = (float)f[i]->v[3] * (1.0f / 65536.0f);
            float w = (float)f[i]->v[5] * (1.0f / 2147483648.0f);   // (the weapon's z is x 3: kept under 1)
            v[i].w = w < 0.9999f ? w : 0.9999f;
            float l = (float)f[i]->v[4] * (1.0f / 64.0f);   // (v[4] is a darkness: colormap row 32 = 128)
            v[i].l = l < 0.0f ? 0.0f : l > 255.0f ? 255.0f : l;
        }
        alias_tri(v, skin, skin_w);
    }
    M20_PERF_END(M20_PERF_ADRAW);
}
