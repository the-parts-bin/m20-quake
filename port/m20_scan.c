// m20: make EXTRA=-DM20_SCAN: while the game runs, after each surface given to
// the span engine (so with the engine, the video and the CPU all at the SDRAM):
//   memory  lines of a pattern area checked, rewritten and flushed. A wrong word
//           is read again: right the second time is a bad read, still wrong
//           is bad memory (a bad write, or a bad cell)
//   arith   FPU and integer results against a table made at the start
//   fetch   every so often, code bigger than the instruction cache
// Tells a fault of the memory path from one of the CPU's.
#ifdef M20_SCAN
#include <stdint.h>
#include <stdio.h>

#define SCAN_LINES 4096u   // 256 KB
#define TAB 256u
static uint32_t area[SCAN_LINES * 16u] __attribute__((aligned(64)));
static uint32_t pos, pass, ready, steps;
static uint32_t lines, bad_read, bad_mem, bad_arith, bad_fetch, arith_ops, fetches, told, orbits;

static uint32_t want_at(uint32_t w, uint32_t gen) {
    uint32_t x = (w + gen * 0x9E3779B9u) * 2654435761u;
    return x ^ x >> 15 ^ (gen & 1u ? 0xAAAAAAAAu : 0x55555555u);
}
static inline void flush_line(const void *p) { asm volatile("cbo.flush (%0)" :: "r"(p) : "memory"); }

static struct { uint32_t a, b, add, mul, div, sqrt, imul, idiv; } tab[TAB];
static inline uint32_t f_add(uint32_t a, uint32_t b) { uint32_t r; asm volatile("fmv.w.x ft0,%1\n fmv.w.x ft1,%2\n fadd.s ft2,ft0,ft1\n fmv.x.w %0,ft2" : "=r"(r) : "r"(a), "r"(b) : "ft0", "ft1", "ft2"); return r; }
static inline uint32_t f_mul(uint32_t a, uint32_t b) { uint32_t r; asm volatile("fmv.w.x ft0,%1\n fmv.w.x ft1,%2\n fmul.s ft2,ft0,ft1\n fmv.x.w %0,ft2" : "=r"(r) : "r"(a), "r"(b) : "ft0", "ft1", "ft2"); return r; }
static inline uint32_t f_div(uint32_t a, uint32_t b) { uint32_t r; asm volatile("fmv.w.x ft0,%1\n fmv.w.x ft1,%2\n fdiv.s ft2,ft0,ft1\n fmv.x.w %0,ft2" : "=r"(r) : "r"(a), "r"(b) : "ft0", "ft1", "ft2"); return r; }
static inline uint32_t f_sqrt(uint32_t a) { uint32_t r; asm volatile("fmv.w.x ft0,%1\n fsqrt.s ft2,ft0\n fmv.x.w %0,ft2" : "=r"(r) : "r"(a) : "ft0", "ft2"); return r; }
static inline uint32_t i_mul(uint32_t a, uint32_t b) { uint32_t r; asm volatile("mul %0,%1,%2" : "=r"(r) : "r"(a), "r"(b)); return r; }
static inline uint32_t i_div(uint32_t a, uint32_t b) { uint32_t r; asm volatile("divu %0,%1,%2" : "=r"(r) : "r"(a), "r"(b)); return r; }

__attribute__((noinline, aligned(64))) static uint32_t fetch_block(uint32_t x) {
    asm volatile(".rept 2048\n addi %0,%0,0x555\n xori %0,%0,0x2aa\n .endr\n" : "+r"(x));   // 16 KB
    return x;
}
static uint32_t fetch_want;

static void init(void) {
    uint32_t s = 2463534242u;
    for (uint32_t i = 0; i < TAB; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        uint32_t a = 0x3F800000u + (s & 0x047FFFFFu);
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        uint32_t b = 0x3F800000u + (s & 0x047FFFFFu);
        tab[i].a = a; tab[i].b = b;
        tab[i].add = f_add(a, b); tab[i].mul = f_mul(a, b); tab[i].div = f_div(a, b); tab[i].sqrt = f_sqrt(a);
        tab[i].imul = i_mul(a, b); tab[i].idiv = i_div(a, (b >> 9) | 1u);
    }
    for (uint32_t i = 0; i < SCAN_LINES * 16u; i++) area[i] = want_at(i, 0);
    for (uint32_t l = 0; l < SCAN_LINES; l++) flush_line(&area[l * 16u]);
    fetch_want = 0x1234u;
    for (uint32_t i = 0; i < 2048u; i++) fetch_want = (fetch_want + 0x555u) ^ 0x2AAu;
    ready = 1;
}

static void memory_line(void) {
    volatile uint32_t *p = &area[pos * 16u];
    uint32_t base = pos * 16u, nbad = 0, first = 0, got = 0, want = 0;
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t g = p[i], w = want_at(base + i, pass);
        if (g != w) {
            if (!nbad) { first = i; got = g; want = w; }
            nbad++;
            orbits |= g ^ w;
        }
    }
    lines++;
    if (nbad) {
        flush_line((const void *)p);
        uint32_t still = 0;
        for (uint32_t i = 0; i < 16u; i++) still += p[i] != want_at(base + i, pass);
        if (still) bad_mem++; else bad_read++;
        if (told++ < 40u)
            printf("scan mem line %lu word %lu: %08lx, not %08lx (xor %08lx), %lu of 16 wrong, %lu on the second read, addr %08lx\n",
                   (unsigned long)pos, (unsigned long)first, (unsigned long)got, (unsigned long)want,
                   (unsigned long)(got ^ want), (unsigned long)nbad, (unsigned long)still, (unsigned long)(uintptr_t)&p[first]);
    }
    for (uint32_t i = 0; i < 16u; i++) p[i] = want_at(base + i, pass + 1u);
    flush_line((const void *)p);
    if (++pos == SCAN_LINES) { pos = 0; pass++; }
}

static void arith(uint32_t i) {
    uint32_t a = tab[i].a, b = tab[i].b, r[6];
    r[0] = f_add(a, b); r[1] = f_mul(a, b); r[2] = f_div(a, b); r[3] = f_sqrt(a);
    r[4] = i_mul(a, b); r[5] = i_div(a, (b >> 9) | 1u);
    const uint32_t *g = &tab[i].add;
    arith_ops += 6u;
    for (uint32_t k = 0; k < 6u; k++) if (r[k] != g[k]) {
        bad_arith++;
        if (told++ < 40u) printf("scan arith op %lu entry %lu: %08lx, not %08lx\n", (unsigned long)k, (unsigned long)i, (unsigned long)r[k], (unsigned long)g[k]);
    }
}

void M20_ScanStep(void) {
    if (!ready) init();
    for (uint32_t k = 0; k < 4u; k++) memory_line();
    for (uint32_t k = 0; k < 2u; k++) arith(steps * 2u + k & (TAB - 1u));
    if (++steps % 32u == 0) {
        uint32_t g = fetch_block(0x1234u);
        fetches++;
        if (g != fetch_want) {
            bad_fetch++;
            if (told++ < 40u) printf("scan fetch %08lx, not %08lx\n", (unsigned long)g, (unsigned long)fetch_want);
        }
    }
}

void M20_ScanReport(void) {
    printf("scan lines %lu badread %lu badmem %lu bits %08lx | arith %lu bad %lu | fetch %lu bad %lu\n",
           (unsigned long)lines, (unsigned long)bad_read, (unsigned long)bad_mem, (unsigned long)orbits,
           (unsigned long)arith_ops, (unsigned long)bad_arith, (unsigned long)fetches, (unsigned long)bad_fetch);
}
#endif
