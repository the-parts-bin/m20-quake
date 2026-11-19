// m20: per-region CPU counters, see m20_perf.h.
#ifdef M20_PERF
#include <stdint.h>
#include <stdio.h>

#include "m20_perf.h"

// The events counted by mhpmcounter3..6 (vexiiriscv.misc.PerformanceCounterService)
#define EV_DCACHE_WAITING   0x1A    // cycles a load or store waits for the data cache
#define EV_ICACHE_WAITING   0x12    // cycles the fetch waits for the instruction cache
#define EV_BRANCH_MISS      0x02    // mispredicted branches and jumps
#define EV_STALL_BACKEND    0x05    // cycles an instruction is ready but can't issue (dependencies, the FPU, the LSU)

#define NC 6
static const char *const names[M20_PERF_N] = {
    "view", "world", "bmodels", "scanedges", "drawsurfs", "surfbuild", "entities", "viewmodel", "particles",
    "aliasdraw", "aliaswait", "cpuwait"};
static const char *const cnames[NC] = {"cycles", "instret", "dc_wait", "ic_wait", "br_miss", "be_stall"};

static uint64_t total[M20_PERF_N][NC];
static uint32_t worst[M20_PERF_N];   // the most cycles in one call
// The frame with the most cycles in the view: each region's cycles in it
static uint32_t frame_c[M20_PERF_N], worst_frame[M20_PERF_N];
static uint32_t start[M20_PERF_N][NC];
static uint32_t calls[M20_PERF_N];
static int ready;

#define CSRR(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })

#ifdef M20_PERF_CLOCK
// (EXTRA=-DM20_PERF_CLOCK: a CPU without the counters; cycles from the
// SoC's CLOCK_CNT, the rest 0)
static inline void sample(uint32_t *c) {
    c[0] = *(volatile uint32_t *)0x44000Cu;
    for (int k = 1; k < NC; k++) c[k] = 0;
}
#else
static inline void sample(uint32_t *c) {
    c[0] = CSRR(mcycle);
    c[1] = CSRR(minstret);
    c[2] = CSRR(mhpmcounter3);
    c[3] = CSRR(mhpmcounter4);
    c[4] = CSRR(mhpmcounter5);
    c[5] = CSRR(mhpmcounter6);
}
#endif

void M20_PerfReset(void) {
#ifndef M20_PERF_CLOCK
    __asm__ volatile("csrw mhpmevent3, %0" :: "r"(EV_DCACHE_WAITING));
    __asm__ volatile("csrw mhpmevent4, %0" :: "r"(EV_ICACHE_WAITING));
    __asm__ volatile("csrw mhpmevent5, %0" :: "r"(EV_BRANCH_MISS));
    __asm__ volatile("csrw mhpmevent6, %0" :: "r"(EV_STALL_BACKEND));
    __asm__ volatile("csrw mcountinhibit, zero");
#endif
    for (int r = 0; r < M20_PERF_N; r++) {
        calls[r] = 0;
        worst[r] = 0;
        worst_frame[r] = 0;
        for (int k = 0; k < NC; k++) total[r][k] = 0;
    }
    ready = 1;
}

void M20_PerfBegin(int r) {
    if (!ready) return;
    if (r == M20_PERF_VIEW)
        for (int k = 0; k < M20_PERF_N; k++) frame_c[k] = 0;
    sample(start[r]);
}

void M20_PerfEnd(int r) {
    if (!ready) return;
    uint32_t c[NC];
    sample(c);
    for (int k = 0; k < NC; k++) total[r][k] += c[k] - start[r][k];   // (32-bit deltas: a call is < 79 s)
    if (c[0] - start[r][0] > worst[r]) worst[r] = c[0] - start[r][0];
    frame_c[r] += c[0] - start[r][0];
    if (r == M20_PERF_ENTS && c[0] - start[r][0] > 2000000u)   // (the slow ones, as they happen)
        printf("M20: slow entities %lu cycles, call %lu\n", (unsigned long)(c[0] - start[r][0]),
               (unsigned long)calls[r]);
    if (r == M20_PERF_VIEW && frame_c[r] >= worst_frame[r])
        for (int k = 0; k < M20_PERF_N; k++) worst_frame[k] = frame_c[k];
    calls[r]++;
}

// (A pause after each line: the BL616 bridge drops bytes from long bursts)
static void pace(void) {
    fflush(stdout);
    uint32_t t0 = *(volatile uint32_t *)0x44000Cu;
    while (*(volatile uint32_t *)0x44000Cu - t0 < 3000000u) {
    }
}

void M20_PerfReport(void) {
    if (!ready) return;
    printf("perf begin\n");
    pace();
    printf("%-10s %6s", "region", "calls");
    for (int k = 0; k < NC; k++) printf(" %12s", cnames[k]);
    printf(" %12s\n", "worst_call");
    for (int r = 0; r < M20_PERF_N; r++) {
        printf("%-10s %6lu", names[r], (unsigned long)calls[r]);
        for (int k = 0; k < NC; k++) printf(" %12llu", (unsigned long long)total[r][k]);
        printf(" %12lu\n", (unsigned long)worst[r]);
        pace();
    }
    printf("worst frame:");
    for (int r = 0; r < M20_PERF_N; r++) {
        printf(" %s %lu", names[r], (unsigned long)worst_frame[r]);
        if (r % 4 == 3) pace();
    }
    printf("\nperf end\n");
}
#endif
