/* Independent C caller for the private hosted ABI. Run each mode separately
 * with captured stdout/stderr bytes under the native Windows validation guard.
 * This is not a public language-export ABI qualification. */
#ifndef _WIN32
#error This fixture requires a native Windows CRT.
#endif

#include "runtime.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

static void cleanup(void *context) {
    int *count = context;
    ++*count;
    printf("cleanup:%d active:%d\n", *count, ax_rt_cleanup_active());
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 9;
    }
    if (strcmp(argv[1], "values") == 0) {
        ax_rt_print_i64(INT64_MIN, 1, 1);
        ax_rt_print_i64(INT64_MAX, 1, 1);
        ax_rt_print_f64(-0.0, 1, 1);
        ax_rt_print_bool(1, 1, 1);
        ax_rt_print_bool(0, 1, 1);
        ax_rt_result(42);
        (void)fflush(stdout);
        (void)fflush(stderr);
        /* The previous modes prove both streams were initialized. */
        return _setmode(_fileno(stdout), _O_BINARY) != _O_BINARY ||
               _setmode(_fileno(stderr), _O_BINARY) != _O_BINARY;
    }
    if (strcmp(argv[1], "result") == 0) {
        ax_rt_result(42);
        return 0;
    }
    int count = 0;
    if (ax_rt_register_cleanup(cleanup, &count) != 0) {
        return 9;
    }
    if (strcmp(argv[1], "trap") == 0) {
        ax_rt_trap(AX_RT_INVALID_UNION_TAG, 12, 7);
    }
    if (strcmp(argv[1], "module-trap") == 0) {
        ax_rt_trap_v2(AX_RT_INVALID_UNION_TAG, "values.ax", 12, 7);
    }
    if (strcmp(argv[1], "mismatch") == 0) {
        ax_rt_tensor_runtime_mismatch();
    }
    (void)ax_rt_unregister_cleanup(&count);
    return 9;
}
