#include "runtime.h"

#include <inttypes.h>
#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

_Static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 &&
               DBL_MAX_EXP == 1024 && DBL_MIN_EXP == -1021,
               "Axeon requires IEEE binary64 double storage");

enum { AX_RT_MAX_DEPTH = 64, AX_RT_MAX_OUTPUT = 1024 * 1024 };

static unsigned int active_depth;
static size_t printed_bytes;
static ax_rt_cleanup_fn active_cleanup;
static void *active_cleanup_context;

int32_t ax_rt_register_cleanup(ax_rt_cleanup_fn cleanup, void *context) {
    if (active_cleanup != NULL || active_cleanup_context != NULL ||
        cleanup == NULL || context == NULL) {
        return 1;
    }
    active_cleanup = cleanup;
    active_cleanup_context = context;
    return 0;
}

int32_t ax_rt_unregister_cleanup(void *context) {
    if (context == NULL || active_cleanup == NULL ||
        active_cleanup_context != context) {
        return 1;
    }
    active_cleanup = NULL;
    active_cleanup_context = NULL;
    return 0;
}

int32_t ax_rt_cleanup_active(void) {
    return active_cleanup != NULL || active_cleanup_context != NULL;
}

static void run_cleanup(void) {
    ax_rt_cleanup_fn cleanup = active_cleanup;
    void *context = active_cleanup_context;
    /* Clear before invoking: a fatal path cannot call the same cleanup twice. */
    active_cleanup = NULL;
    active_cleanup_context = NULL;
    if (cleanup != NULL) {
        cleanup(context);
    }
}

#ifdef _WIN32
static void ensure_binary_stdio(void) {
    static int initialized;
    if (initialized) {
        return;
    }
    /* Narrow runtime writes are UTF-8 bytes with LF on every hosted target.
     * Change both CRT streams before the first write, including fatal cleanup. */
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) {
        run_cleanup();
        (void)fputs("configuration[stdio]: cannot set binary output mode\n", stderr);
        exit(2);
    }
    initialized = 1;
}
#else
#define ensure_binary_stdio() ((void)0)
#endif

_Noreturn void ax_rt_tensor_runtime_mismatch(void) {
    ensure_binary_stdio();
    run_cleanup();
    (void)fflush(stdout);
    (void)fputs("configuration[tensor_runtime]: incompatible internal ABI/version\n", stderr);
    exit(2);
}

static void print_bytes(const char *bytes, size_t count,
                        const char *file, int64_t line, int64_t col) {
    ensure_binary_stdio();
    if (count > (size_t)AX_RT_MAX_OUTPUT - printed_bytes) {
        ax_rt_trap_v2(AX_RT_OUTPUT_LIMIT, file, line, col);
    }
    printed_bytes += count;
    (void)fwrite(bytes, 1, count, stdout);
}

void ax_rt_enter(int64_t line, int64_t col) {
    ax_rt_enter_v2(NULL, line, col);
}

void ax_rt_enter_v2(const char *file, int64_t line, int64_t col) {
    if (active_depth >= AX_RT_MAX_DEPTH) {
        ax_rt_trap_v2(AX_RT_CALL_DEPTH, file, line, col);
    }
    ++active_depth;
}

void ax_rt_leave(void) {
    if (active_depth != 0) {
        --active_depth;
    }
}

void ax_rt_print_i64(int64_t value, int64_t line, int64_t col) {
    ax_rt_print_i64_v2(value, NULL, line, col);
}

void ax_rt_print_i64_v2(int64_t value, const char *file,
                        int64_t line, int64_t col) {
    /* A signed 64-bit decimal, newline, and terminator require 22 bytes. */
    char buffer[32];
    int count = snprintf(buffer, sizeof buffer, "%" PRId64 "\n", value);
    print_bytes(buffer, (size_t)count, file, line, col);
}

void ax_rt_print_f64(double value, int64_t line, int64_t col) {
    ax_rt_print_f64_v2(value, NULL, line, col);
}

void ax_rt_print_f64_v2(double value, const char *file,
                        int64_t line, int64_t col) {
    uint64_t bits;
    /* memcpy preserves the representation without violating C aliasing rules. */
    memcpy(&bits, &value, sizeof bits);
    if ((bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000)) {
        bits = UINT64_C(0x7ff8000000000000);
    }
    char buffer[32];
    int count = snprintf(buffer, sizeof buffer, "f64:0x%016" PRIx64 "\n", bits);
    print_bytes(buffer, (size_t)count, file, line, col);
}

void ax_rt_print_bool(int32_t value, int64_t line, int64_t col) {
    ax_rt_print_bool_v2(value, NULL, line, col);
}

void ax_rt_print_bool_v2(int32_t value, const char *file,
                         int64_t line, int64_t col) {
    if (value != 0) {
        print_bytes("true\n", 5, file, line, col);
    } else {
        print_bytes("false\n", 6, file, line, col);
    }
}

void ax_rt_result(int64_t value) {
    ensure_binary_stdio();
    char buffer[64];
    int count = snprintf(buffer, sizeof buffer, "result: %" PRId64 "\n", value);
    (void)fwrite(buffer, 1, (size_t)count, stdout);
}

_Noreturn void ax_rt_trap(int32_t kind, int64_t line, int64_t col) {
    ax_rt_trap_v2(kind, NULL, line, col);
}

_Noreturn void ax_rt_trap_v2(int32_t kind, const char *file,
                            int64_t line, int64_t col) {
    ensure_binary_stdio();
    static const char *const names[] = {
        "unknown", "overflow", "division_by_zero", "shift_count",
        "call_depth", "fuel_exhausted", "output_limit", "bounds", "invalid_union_tag"
    };
    const char *name = "unknown";
    if (kind >= AX_RT_OVERFLOW && kind <= AX_RT_INVALID_UNION_TAG) {
        name = names[kind];
    }
    run_cleanup();
    (void)fflush(stdout);
    if (file == NULL) {
        (void)fprintf(stderr, "trap[%s] at %" PRId64 ":%" PRId64 "\n",
                      name, line, col);
    } else {
        (void)fprintf(stderr, "trap[%s] at %s:%" PRId64 ":%" PRId64 "\n",
                      name, file, line, col);
    }
    exit(70);
}
