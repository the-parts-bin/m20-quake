// A model of the span engine planned for the m20 (../README.md, "Span
// engine"): Quake's D_DrawSpans8 + D_DrawZSpans for one textured surface,
// split the way the hardware will be.
//
// The driver (M20_SpanSurface, the CPU's part) turns the surface's float
// gradients into the engine's fixed-point registers, once per surface. The
// engine (engine_span) then works in integers only, per span:
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

cvar_t r_m20span = {"r_m20span", "1"};   // 0: Quake's own span drawers

#define SEG 16   // pixels between perspective divisions

// ---------------------------------------------------------------------
// The engine's registers
// ---------------------------------------------------------------------
typedef struct {
    // per surface
    const uint8_t *tex;     // surface cache block
    int32_t tex_w;          // its width (cachewidth)
    int32_t s0, su, sv;     // s/z: origin, d/du, d/dv, scaled by 2^es
    int32_t t0, tu, tv;     // t/z, scaled by 2^et
    int32_t z0, zu, zv;     // 1/z, scaled by 2^31
    int32_t es, et;
    int32_t sadj, tadj;     // 16.16
    int32_t smax, tmax;     // bbextents, bbextentt
    // per frame
    uint8_t *fb;            // d_viewbuffer
    int32_t fb_stride;      // screenwidth
    int16_t *zb;            // d_pzbuffer
    int32_t zb_stride;      // d_zwidth (in 16-bit units)
} engine_t;

static engine_t eng;

// 1/m for m in [1, 2): 2^17 / (1 + i/1024) for i = 0..1024, 17-18 bits
static uint32_t recip_tab[1025];
// 1/n for the last segment's step: 65536 / n, n = 1..SEG-1
static uint32_t inv_n[SEG];

static void tables(void) {
    for (int i = 0; i <= 1024; i++) recip_tab[i] = (uint32_t)((1u << 27) / (1024u + (uint32_t)i));
    for (int n = 1; n < SEG; n++) inv_n[n] = 65536u / (uint32_t)n;
}

// s (or t) = value / zi in 16.16, plus adj, clamped to [lo, hi].
// v: s/z * 2^e; izi: 1/z * 2^31.
static int32_t persp(int32_t v, int32_t izi, int32_t e, int32_t adj, int32_t lo, int32_t hi) {
    if (izi < 1) izi = 1;
    int lz = __builtin_clz((uint32_t)izi);
    uint32_t m = (uint32_t)izi << lz;          // top bit set: m = 2^31 .. 2^32 - 1
    uint32_t idx = (m >> 21) & 1023u;          // the 10 bits after the leading 1
    uint32_t frac = (m >> 13) & 255u;          // the next 8
    uint32_t r = recip_tab[idx] - (((recip_tab[idx] - recip_tab[idx + 1u]) * frac) >> 8);
    // r ~ 2^17 * 2^31 / m = 2^48 / m, so 1/izi = r * 2^lz / 2^48, and
    // s = v / 2^e / (izi / 2^31) * 2^16 = v * r * 2^(lz - 48 + 31 + 16 - e)
    int64_t p = (int64_t)v * (int64_t)r;       // 32 x 18 bits
    int sh = 1 + e - lz;                        // 48 - 31 - 16 + e - lz
    int64_t s;
    if (sh >= 63) s = 0;
    else if (sh >= 0) s = p >> sh;
    else if (sh > -16) s = p * ((int64_t)1 << -sh);
    else s = p < 0 ? INT32_MIN : INT32_MAX;     // (out of range either way)
    s += adj;
    if (s > hi) s = hi;
    else if (s < lo) s = lo;
    return (int32_t)s;
}

static void engine_span(int32_t u, int32_t v, int32_t count) {
    int32_t sdz = eng.s0 + v * eng.sv + u * eng.su;
    int32_t tdz = eng.t0 + v * eng.tv + u * eng.tu;
    int32_t izi = eng.z0 + v * eng.zv + u * eng.zu;
    uint8_t *pd = eng.fb + v * eng.fb_stride + u;
    int16_t *pz = eng.zb + v * eng.zb_stride + u;
    int32_t zpix = izi;

    int32_t s = persp(sdz, izi, eng.es, eng.sadj, 0, eng.smax);
    int32_t t = persp(tdz, izi, eng.et, eng.tadj, 0, eng.tmax);
    do {
        int32_t n = count >= SEG ? SEG : count;
        count -= n;
        int32_t s1, t1, ss = 0, ts = 0;
        if (count) {
            sdz += SEG * eng.su;
            tdz += SEG * eng.tu;
            izi += SEG * eng.zu;
            s1 = persp(sdz, izi, eng.es, eng.sadj, 8, eng.smax);
            t1 = persp(tdz, izi, eng.et, eng.tadj, 8, eng.tmax);
            ss = (s1 - s) >> 4;
            ts = (t1 - t) >> 4;
        } else {
            sdz += (n - 1) * eng.su;
            tdz += (n - 1) * eng.tu;
            izi += (n - 1) * eng.zu;
            s1 = persp(sdz, izi, eng.es, eng.sadj, 8, eng.smax);
            t1 = persp(tdz, izi, eng.et, eng.tadj, 8, eng.tmax);
            if (n > 1) {
                ss = (int32_t)(((int64_t)(s1 - s) * inv_n[n - 1]) >> 16);
                ts = (int32_t)(((int64_t)(t1 - t) * inv_n[n - 1]) >> 16);
            }
        }
        for (int32_t i = 0; i < n; i++) {
            *pd++ = eng.tex[(s >> 16) + (t >> 16) * eng.tex_w];
            *pz++ = (int16_t)(zpix >> 16);
            s += ss;
            t += ts;
            zpix += eng.zu;
        }
        s = s1;
        t = t1;
    } while (count > 0);
}

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

void M20_SpanSurface(espan_t *spans) {
    static int ready;
    if (!ready) {
        tables();
        ready = 1;
    }
    eng.fb = (uint8_t *)d_viewbuffer;
    eng.fb_stride = screenwidth;
    eng.zb = d_pzbuffer;
    eng.zb_stride = d_zwidth;
    eng.tex = (const uint8_t *)cacheblock;
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
    for (espan_t *p = spans; p; p = p->pnext) engine_span(p->u, p->v, p->count);
}
