// Correctness only: no clocks, warmups, or benchmark timing protocol.
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef BENCH_KERNEL
#error BENCH_KERNEL must name the checked kernel
#endif
extern int64_t BENCH_KERNEL(int64_t n);
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char *end = NULL;
    errno = 0;
    const int64_t n = strtoll(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0') return 2;
    printf("%" PRId64 "\n", BENCH_KERNEL(n));
    return 0;
}
