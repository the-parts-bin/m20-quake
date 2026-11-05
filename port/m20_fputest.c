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

// R_EmitEdge's surface index, as compiled: three crashes had it not shifted
// down, as though the slli read a5 from before the srai, with an fsw between.
static uint32_t hazard(volatile uint32_t *v, uint32_t *edge, int div) {
    uint32_t a, b;
    float one = 1.0f, x = 3.0f;
    if (div)
        asm volatile(
            "fdiv.s %[x],%[one],%[x]\n"
            "lw %[a],0(%[v])\n lw %[b],4(%[v])\n"
            "sub %[a],%[a],%[b]\n srai %[a],%[a],6\n addi %[b],%[e],32\n"
            "fsw %[x],24(%[e])\n slli %[a],%[a],16\n srli %[a],%[a],16\n"
            : [a]"=&r"(a), [b]"=&r"(b), [x]"+f"(x) : [v]"r"(v), [e]"r"(edge), [one]"f"(one) : "memory");
    else
        asm volatile(
            "lw %[a],0(%[v])\n lw %[b],4(%[v])\n"
            "sub %[a],%[a],%[b]\n srai %[a],%[a],6\n addi %[b],%[e],32\n"
            "fsw %[x],24(%[e])\n slli %[a],%[a],16\n srli %[a],%[a],16\n"
            : [a]"=&r"(a), [b]"=&r"(b) : [v]"r"(v), [e]"r"(edge), [x]"f"(x) : "memory");
    return a;
}

#define area ((uint32_t *)0xC00000u)   // 1 MB of the idle hunk: stores that miss the cache

static void hazard_test(void) {
    static volatile uint32_t v[2];
    uint32_t bad[2] = {0, 0}, told = 0;
    for (uint32_t i = 1; i <= 3000000u; i++) {
        int div = i & 1;
        uint32_t idx = rnd() & 0x3FFu;
        v[0] = 0xFE0000u + idx * 64u;
        v[1] = 0xFE0000u;
        uint32_t *e = &area[(rnd() & 0x3FFFFu) & ~7u];
        if (e > &area[(1u << 18) - 32]) e = area;
        uint32_t r = hazard(v, e, div);
        if (r != idx) {
            bad[div]++;
            if (told++ < 8) printf("[hazard div %d: %lx, not %lx]\n", div, (unsigned long)r, (unsigned long)idx);
        }
        if (i % 1000000u == 0) printf("hazard: %lu runs, %lu bad plain, %lu bad after fdiv\n", (unsigned long)i, (unsigned long)bad[0], (unsigned long)bad[1]);
    }
}

// Nothing but shifts and adds in a loop, so that the interrupts land between
// them: each round must end with its input.
static void tight_test(void) {
    uint32_t bad = 0, told = 0;
    for (uint32_t round = 1; round <= 40u; round++) {
        uint32_t n = 4000000u, errs = 0, last = 0;
        uint32_t x = 0x3380u;
        asm volatile(
            "1:\n"
            "li t0,0xCE\n"
            "slli t1,t0,6\n"
            "srai t1,t1,6\n"
            "addi t2,t0,32\n"
            "slli t3,t1,16\n"
            "srli t3,t3,16\n"
            "beq t3,t0,2f\n"
            "addi %[e],%[e],1\n"
            "mv %[l],t3\n"
            "2:\n"
            "addi %[n],%[n],-1\n"
            "bnez %[n],1b\n"
            : [n]"+r"(n), [e]"+r"(errs), [l]"+r"(last) : : "t0", "t1", "t2", "t3");
        bad += errs;
        if (errs && told++ < 8) printf("[tight: %lu wrong, last %lx]\n", (unsigned long)errs, (unsigned long)last);
        if (round % 10u == 0) printf("tight: %lu million loops, %lu wrong\n", (unsigned long)round * 4u, (unsigned long)bad);
    }
}

// Halfword stores to one word back to back, as in R_EmitEdge (sh a3,18(a1);
// sh zero,16(a1)), then read back. variant 0: nothing before; 1: fsw and sw to
// the same line first, as the game does; 2: the stores the other way round;
// 3: bytes.
static void store_test(void) {
    uint32_t bad[4] = {0, 0, 0, 0}, told = 0;
    float x = 1.5f;
    for (uint32_t i = 1; i <= 6000000u; i++) {
        uint32_t var = i & 3u, val = (rnd() & 0xFFFFu) | 1u, want, got;
        uint32_t *e = &area[(rnd() & 0x3FFFFu) & ~7u];
        if (e > &area[(1u << 18) - 32]) e = area;
        uint32_t *w = e + 4;
        *w = 0xDEADBEEFu;
        if (i & 4u) asm volatile("cbo.flush (%0)" :: "r"(e) : "memory");   // half of them miss
        switch (var) {
        case 0:
            asm volatile("sh %1,18(%0)\n sh zero,16(%0)\n" :: "r"(e), "r"(val) : "memory");
            want = val << 16;
            break;
        case 1:
            asm volatile("fsw %2,24(%0)\n sw %1,28(%0)\n sh %1,18(%0)\n sh zero,16(%0)\n" :: "r"(e), "r"(val), "f"(x) : "memory");
            want = val << 16;
            break;
        case 2:
            asm volatile("sh %1,16(%0)\n sh zero,18(%0)\n" :: "r"(e), "r"(val) : "memory");
            want = val;
            break;
        default:
            asm volatile("sb %1,16(%0)\n sb zero,17(%0)\n sb %1,18(%0)\n sb zero,19(%0)\n" :: "r"(e), "r"(val) : "memory");
            want = (val & 255u) | (val & 255u) << 16;
            break;
        }
        got = *(volatile uint32_t *)w;
        if (got != want) {
            bad[var]++;
            if (told++ < 10) printf("[store %lu: %08lx, not %08lx]\n", (unsigned long)var, (unsigned long)got, (unsigned long)want);
        }
        if (i % 2000000u == 0) printf("store: %lu runs, bad %lu %lu %lu %lu\n", (unsigned long)i, (unsigned long)bad[0], (unsigned long)bad[1], (unsigned long)bad[2], (unsigned long)bad[3]);
    }
}

// R_ViewChanged's a5 turned into a float right after fdiv.s fa5: does a long FPU
// operation reach the integer register of the same number? fdiv or fsqrt, then
// a load through that register (a miss, or not), after 0..n instructions.
void M20_RegTest(void) {
    static volatile uint32_t mem[2048] __attribute__((aligned(64)));
    uint32_t bad[2][2] = {{0, 0}, {0, 0}}, told = 0, runs = 0;
    float one = 1.0f, three = 3.0f;
    printf("reg test\n");
    for (uint32_t i = 1; i <= 4000000u; i++) {
        volatile uint32_t *p = &mem[(rnd() & 0x7FFu) & ~15u];
        uint32_t miss = i & 1u, sq = (i >> 1) & 1u, want = (uint32_t)p, got;
        if (miss) asm volatile("cbo.flush (%0)" :: "r"(p) : "memory");
        register uint32_t a5 asm("a5") = want;
        register float fa5 asm("fa5") = one;
        if (sq)
            asm volatile("fsqrt.s fa5,%2\n flw fa3,0(a5)\n flw fa4,4(a5)\n"
                         : "+r"(a5), "+f"(fa5) : "f"(three) : "fa3", "fa4", "memory");
        else
            asm volatile("fdiv.s fa5,fa5,%2\n flw fa3,0(a5)\n flw fa4,4(a5)\n"
                         : "+r"(a5), "+f"(fa5) : "f"(three) : "fa3", "fa4", "memory");
        got = a5;
        runs++;
        if (got != want) {
            bad[sq][miss]++;
            if (told++ < 8) printf("[reg sq %lu miss %lu: a5 %08lx, not %08lx]\n", (unsigned long)sq, (unsigned long)miss, (unsigned long)got, (unsigned long)want);
        }
        if (i % 1000000u == 0)
            printf("reg: %lu runs, bad fdiv %lu/%lu fsqrt %lu/%lu\n", (unsigned long)runs, (unsigned long)bad[0][0], (unsigned long)bad[0][1], (unsigned long)bad[1][0], (unsigned long)bad[1][1]);
    }
}

// The game data, read twice: a sum for each megabyte, to compare with the
// host's copy and between the passes.
static void file_test(void) {
    static uint8_t buf[65536] __attribute__((aligned(64)));
    for (int pass = 0; pass < 2; pass++) {
        FILE *f = fopen("QUAKE/id1/pak0.pak", "r");
        if (!f) { printf("no pak\n"); return; }
        uint32_t sum = 0, block = 0, k = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            for (size_t i = 0; i + 4 <= n; i += 4) {
                uint32_t w; memcpy(&w, buf + i, 4);
                sum = (sum << 1 | sum >> 31) ^ w;
            }
            if (++k == 16u) { printf("sum %d %lu %08lx\n", pass, (unsigned long)block++, (unsigned long)sum); sum = 0; k = 0; }
        }
        if (k) printf("sum %d %lu %08lx\n", pass, (unsigned long)block, (unsigned long)sum);
        fclose(f);
    }
}

// 4 MB of the idle hunk, far more than the caches: written with stores, with
// memset and with memcpy, each read back.
static void mem_test(void) {
    uint32_t *m = (uint32_t *)0xA00000u;
    const uint32_t n = (4u << 20) / 4u;
    for (uint32_t pass = 0; pass < 3; pass++) {
        uint32_t bad = 0, told = 0;
        for (uint32_t i = 0; i < n; i++) m[i] = (i + pass) * 2654435761u;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w = (i + pass) * 2654435761u;
            if (m[i] != w) { bad++; if (told++ < 4) printf("[mem %p: %08lx, not %08lx]\n", &m[i], (unsigned long)m[i], (unsigned long)w); }
        }
        printf("stores pass %lu: %lu bad\n", (unsigned long)pass, (unsigned long)bad);
        bad = 0; told = 0;
        memset(m, 0, 4u << 20);
        for (uint32_t i = 0; i < n; i++) if (m[i]) { bad++; if (told++ < 4) printf("[zero %p: %08lx]\n", &m[i], (unsigned long)m[i]); }
        printf("memset 0 pass %lu: %lu bad\n", (unsigned long)pass, (unsigned long)bad);
        bad = 0; told = 0;
        for (uint32_t i = 0; i < n / 2; i++) m[i] = i * 40503u + pass;
        memcpy(m + n / 2, m, 2u << 20);
        for (uint32_t i = 0; i < n / 2; i++) if (m[n / 2 + i] != i * 40503u + pass) { bad++; if (told++ < 4) printf("[copy %p: %08lx]\n", &m[n / 2 + i], (unsigned long)m[n / 2 + i]); }
        printf("memcpy pass %lu: %lu bad\n", (unsigned long)pass, (unsigned long)bad);
    }
}

void M20_FpuTest(void) {
    mem_test();
    file_test();
    store_test();
    tight_test();
    hazard_test();
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
