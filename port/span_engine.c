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

cvar_t r_m20span = {"r_m20span", "1"};   // 0: Quake's own span drawers

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
    for (espan_t *p = spans; p; p = p->pnext) {
        pix_fb = (uint8_t *)d_viewbuffer + p->v * screenwidth + p->u;
        pix_zb = d_pzbuffer + p->v * d_zwidth + p->u;
        sc_span(&eng, p->u, p->v, p->count);
    }
}
