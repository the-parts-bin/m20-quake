// m20: make EXTRA=-DM20_FPUTEST: instead of the game, checks the FPU forever
// against integer arithmetic, with the game's interrupts running. Quake's
// edges showed wrong divides on the board that no simulation had (R_EmitEdge).
#ifdef M20_FPUTEST
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static uint32_t seed = 2463534242u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

// Round to nearest even; m has the 24 result bits above `low` extra bits.
static uint32_t pack(uint32_t s, int e, uint64_t m, int low, int sticky) {
    uint64_t q = m >> low, rem = m & (((uint64_t)1 << low) - 1), half = (uint64_t)1 << (low - 1);
    if (rem > half || (rem == half && (sticky || (q & 1u)))) q++;
    if (q >> 24) { q >>= 1; e++; }
    return s << 31 | (uint32_t)(e + 127) << 23 | ((uint32_t)q & 0x7FFFFFu);
}

// Normal numbers, with a normal result
static uint32_t ref_div(uint32_t a, uint32_t b) {
    uint32_t s = (a ^ b) >> 31;
    int e = (int)(a >> 23 & 255) - (int)(b >> 23 & 255);
    uint64_t ma = (a & 0x7FFFFFu) | 0x800000u, mb = (b & 0x7FFFFFu) | 0x800000u;
    uint64_t n = ma << 26, q = n / mb;
    int st = n % mb != 0;
    if (!(q >> 26)) { n <<= 1; q = n / mb; st = n % mb != 0; e--; }
    return pack(s, e, q, 3, st);        // q: 27 bits
}

static uint32_t ref_mul(uint32_t a, uint32_t b) {
    uint32_t s = (a ^ b) >> 31;
    int e = (int)(a >> 23 & 255) + (int)(b >> 23 & 255) - 254;
    uint64_t m = (uint64_t)((a & 0x7FFFFFu) | 0x800000u) * ((b & 0x7FFFFFu) | 0x800000u);
    if (m >> 47) { e++; return pack(s, e, m, 24, 0); }
    return pack(s, e, m, 23, 0);
}

void M20_FpuTest(void) {
    uint32_t ops = 0, bad_div = 0, bad_mul = 0, told = 0, t0 = 0;
    printf("fpu test\n");
    for (;;) {
        float a = flt(0x3F800000u + (rnd() & 0x047FFFFFu)), b = flt(0x3F800000u + (rnd() & 0x047FFFFFu));
        float c = flt(0x3F800000u + (rnd() & 0x047FFFFFu)), d = flt(0x3F800000u + (rnd() & 0x047FFFFFu));
        float n = a - b, m = c - d, q = n / m, p = n * m;
        uint32_t bn = bits(n), bm = bits(m), bq = bits(q), bp = bits(p);
        ops++;
        if (bn << 1 && bm << 1 && (bn >> 23 & 255) > 90 && (bm >> 23 & 255) > 90) {
            uint32_t rq = ref_div(bn, bm), rp = ref_mul(bn, bm);
            if (bq != rq) {
                bad_div++;
                if (told++ < 12) printf("[div %08lx / %08lx = %08lx, not %08lx]\n", (unsigned long)bn, (unsigned long)bm, (unsigned long)bq, (unsigned long)rq);
            }
            if (bp != rp) {
                bad_mul++;
                if (told++ < 12) printf("[mul %08lx * %08lx = %08lx, not %08lx]\n", (unsigned long)bn, (unsigned long)bm, (unsigned long)bp, (unsigned long)rp);
            }
        }
        if (++t0 == 400000u) {
            t0 = 0;
            printf("fpu: %lu ops, %lu bad divides, %lu bad multiplies\n", (unsigned long)ops, (unsigned long)bad_div, (unsigned long)bad_mul);
        }
    }
}
#endif
