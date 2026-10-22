#include "checked.hpp"

extern "C" __attribute__((noinline)) int64_t cpp_bench_kernel(int64_t n) {
    ax_rt_enter(1, 1);
    double x = 0.125;
    double y = 0.5;
    int64_t i = 0;
    while (i < n) {
        x = (x * (1.0 - x)) * 3.75;
        y = (y + x) / (1.5 + x * 0.5);
        i = add(i, 1);
    }
    const int64_t checksum = bits(x + y);
    ax_rt_leave();
    return checksum;
}
