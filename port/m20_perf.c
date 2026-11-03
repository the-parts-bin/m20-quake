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
    "view", "world", "bmodels", "scanedges", "drawsurfs", "surfbuild", "entities", "viewmodel", "particles"};
static const char *const cnames[NC] = {"cycles", "instret", "dc_wait", "ic_wait", "br_miss", "be_stall"};

static uint64_t total[M20_PERF_N][NC];
static uint32_t start[M20_PERF_N][NC];
static uint32_t calls[M20_PERF_N];
static int ready;

#define CSRR(csr) ({ uint32_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })

static inline void sample(uint32_t *c) {
    c[0] = CSRR(mcycle);
    c[1] = CSRR(minstret);
    c[2] = CSRR(mhpmcounter3);
    c[3] = CSRR(mhpmcounter4);
    c[4] = CSRR(mhpmcounter5);
    c[5] = CSRR(mhpmcounter6);
}

void M20_PerfReset(void) {
    __asm__ volatile("csrw mhpmevent3, %0" :: "r"(EV_DCACHE_WAITING));
    __asm__ volatile("csrw mhpmevent4, %0" :: "r"(EV_ICACHE_WAITING));
    __asm__ volatile("csrw mhpmevent5, %0" :: "r"(EV_BRANCH_MISS));
    __asm__ volatile("csrw mhpmevent6, %0" :: "r"(EV_STALL_BACKEND));
    __asm__ volatile("csrw mcountinhibit, zero");
    for (int r = 0; r < M20_PERF_N; r++) {
        calls[r] = 0;
        for (int k = 0; k < NC; k++) total[r][k] = 0;
    }
    ready = 1;
}

void M20_PerfBegin(int r) {
    if (ready) sample(start[r]);
}

void M20_PerfEnd(int r) {
    if (!ready) return;
    uint32_t c[NC];
    sample(c);
    for (int k = 0; k < NC; k++) total[r][k] += c[k] - start[r][k];   // (32-bit deltas: a call is < 79 s)
    calls[r]++;
}

void M20_PerfReport(void) {
    if (!ready) return;
    printf("perf begin\n");
    printf("%-10s %6s", "region", "calls");
    for (int k = 0; k < NC; k++) printf(" %12s", cnames[k]);
    printf("\n");
    for (int r = 0; r < M20_PERF_N; r++) {
        printf("%-10s %6lu", names[r], (unsigned long)calls[r]);
        for (int k = 0; k < NC; k++) printf(" %12llu", (unsigned long long)total[r][k]);
        printf("\n");
    }
    printf("perf end\n");
}
#endif
