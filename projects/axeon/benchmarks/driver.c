#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef BENCH_KERNEL
#error BENCH_KERNEL must name the separately compiled kernel.
#endif
extern int64_t BENCH_KERNEL(int64_t n);

static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) exit(2);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static unsigned long image_peak_rss_kib(void) {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) {
        fputs("cannot read /proc/self/status for VmHWM\n", stderr);
        exit(2);
    }
    char line[512];
    unsigned long value;
    while (fgets(line, sizeof line, status)) {
        if (sscanf(line, "VmHWM: %lu kB", &value) == 1 && value > 0) {
            fclose(status);
            return value;
        }
    }
    fclose(status);
    fputs("positive VmHWM missing from /proc/self/status\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char *end;
    errno = 0;
    int64_t n = strtoll(argv[1], &end, 10);
    if (errno || !argv[1][0] || *end || n < 0 || n > 1000000) return 2;
    uint64_t elapsed[9];
    int64_t results[9];
    for (int call = 0; call < 9; ++call) {
        uint64_t begin = now_ns();
        results[call] = BENCH_KERNEL(n);
        elapsed[call] = now_ns() - begin;
    }
    const unsigned long peak_rss_kib = image_peak_rss_kib();
    // All outputs are observed after the timed regions. No LTO is used.
    printf("{\"n\":%" PRId64 ",\"proc_image_peak_rss_kib\":%lu,\"cold_ns\":%" PRIu64 ",\"warmup_ns\":%" PRIu64
           ",\"warm_ns\":[", n, peak_rss_kib, elapsed[0], elapsed[1]);
    for (int i = 2; i < 9; ++i) printf("%s%" PRIu64, i == 2 ? "" : ",", elapsed[i]);
    printf("],\"checksums\":[");
    for (int i = 0; i < 9; ++i) printf("%s%" PRId64, i == 0 ? "" : ",", results[i]);
    puts("]}");
    return 0;
}
