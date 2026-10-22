#include "checked.hpp"

extern "C" __attribute__((noinline)) int64_t cpp_bench_kernel(int64_t n) {
    ax_rt_enter(1, 1);
    const double matrix[16] = {
        0.5, 0.125, 0.0625, 0.03125,
        0.03125, 0.5, 0.125, 0.0625,
        0.0625, 0.03125, 0.5, 0.125,
        0.125, 0.0625, 0.03125, 0.5,
    };
    const double forcing[4] = {0.03125, -0.015625, 0.0078125, -0.00390625};
    double state[4] = {0.125, -0.25, 0.375, -0.5};
    double next[4] = {0.0, 0.0, 0.0, 0.0};
    int64_t i = 0;
    while (i < n) {
        const double drive = static_cast<double>(remainder(i, 97)) / 97.0 - 0.5;
        int64_t row = 0;
        while (row < 4) {
            double acc = 0.0;
            int64_t col = 0;
            while (col < 4) {
                acc = acc + matrix[index(add(multiply(row, 4), col), 16)]
                    * state[index(col, 4)];
                col = add(col, 1);
            }
            // Check the destination before evaluating the scalar RHS.
            const size_t target = index(row, 4);
            next[target] = acc + forcing[index(row, 4)] * drive;
            row = add(row, 1);
        }
        int64_t copy = 0;
        while (copy < 4) {
            const size_t target = index(copy, 4);
            state[target] = next[index(copy, 4)];
            copy = add(copy, 1);
        }
        i = add(i, 1);
    }
    double checksum = 0.0;
    int64_t element = 0;
    while (element < 4) {
        checksum = checksum + state[index(element, 4)];
        element = add(element, 1);
    }
    const int64_t result = bits(checksum);
    ax_rt_leave();
    return result;
}
