// Checked C++ baseline: same initialization, operation order, and traps as Axeon.
#include <stdint.h>
#include <stddef.h>

extern "C" {
void ax_rt_enter(int64_t line, int64_t column);
void ax_rt_leave(void);
[[noreturn]] void ax_rt_trap(int32_t kind, int64_t line, int64_t column);
}

static inline int64_t add(int64_t a, int64_t b) {
    int64_t result;
    if (__builtin_add_overflow(a, b, &result)) ax_rt_trap(1, 1, 1);
    return result;
}
static inline int64_t multiply(int64_t a, int64_t b) {
    int64_t result;
    if (__builtin_mul_overflow(a, b, &result)) ax_rt_trap(1, 1, 1);
    return result;
}
static inline int64_t remainder(int64_t a, int64_t b) {
    if (b == 0) ax_rt_trap(2, 1, 1);
    if (a == INT64_MIN && b == -1) ax_rt_trap(1, 1, 1);
    return a % b;
}
static inline size_t index(int64_t value) {
    if (value < 0 || value >= 32) ax_rt_trap(7, 1, 1);
    return static_cast<size_t>(value);
}

extern "C" __attribute__((noinline)) int64_t cpp_bench_kernel(int64_t n) {
    ax_rt_enter(1, 1);
    int64_t state[32] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                         16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
    int64_t acc = 17;
    int64_t i = 0;
    while (i < n) {
        const int64_t idx = remainder(i, 32);
        const int64_t j = remainder(add(idx, 1), 32);
        const int64_t product = multiply(state[index(idx)], 1664525);
        const int64_t with_neighbor = add(product, state[index(j)]);
        acc = remainder(add(with_neighbor, acc), 1000003);
        state[index(idx)] = acc;
        i = add(i, 1);
    }
    int64_t checksum = 0;
    int64_t k = 0;
    while (k < 32) {
        checksum = add(checksum, state[index(k)]);
        k = add(k, 1);
    }
    ax_rt_leave();
    return checksum;
}
