// m20: profile-guided builds. make PGO=gen counts every branch; at the
// timedemo's end this prints the counters as hex over the UART (GCC's
// freestanding gcov: no files on the target), between the markers tdrun.py's
// --perf saves. tools: xxd -r -p | gcov-tool merge-stream writes the .gcda
// files that make PGO=use reads.
#ifdef M20_PGO_GEN
#include <gcov.h>
#include <stdio.h>
#include <stdlib.h>

#define QUIET __attribute__((no_profile_instrument_function))

extern const struct gcov_info *const __start_gcov_info[];
extern const struct gcov_info *const __stop_gcov_info[];

QUIET static void dump(const void *d, unsigned n, void *arg) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *c = d;
    for (unsigned i = 0; i < n; i++) {
        putchar(hex[c[i] >> 4]);
        putchar(hex[c[i] & 15]);
    }
}

QUIET static void filename(const char *f, void *arg) {
    __gcov_filename_to_gcfn(f, dump, arg);
}

QUIET static void *allocate(unsigned n, void *arg) {
    return malloc(n);
}

QUIET void M20_PgoDump(void) {
    const struct gcov_info *const *info = __start_gcov_info;
    const struct gcov_info *const *end = __stop_gcov_info;
    __asm__("" : "+r"(info));
    printf("perf begin\n");
    for (; info != end; info++) {
        __gcov_info_to_gcda(*info, filename, dump, allocate, NULL);
        putchar('\n');
    }
    printf("perf end\n");
}
#endif
