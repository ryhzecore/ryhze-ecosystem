#ifndef AXEON_RUNTIME_H
#define AXEON_RUNTIME_H

#include <stdint.h>

enum ax_rt_trap_kind {
    AX_RT_OVERFLOW = 1,
    AX_RT_DIVISION_BY_ZERO = 2,
    AX_RT_SHIFT_COUNT = 3,
    AX_RT_CALL_DEPTH = 4,
    AX_RT_FUEL_EXHAUSTED = 5,
    AX_RT_OUTPUT_LIMIT = 6,
    AX_RT_BOUNDS = 7,
    AX_RT_INVALID_UNION_TAG = 8
};

void ax_rt_enter(int64_t line, int64_t col);
void ax_rt_leave(void);
void ax_rt_print_i64(int64_t value, int64_t line, int64_t col);
void ax_rt_print_f64(double value, int64_t line, int64_t col);
void ax_rt_print_bool(int32_t value, int64_t line, int64_t col);
void ax_rt_result(int64_t value);
_Noreturn void ax_rt_trap(int32_t kind, int64_t line, int64_t col);

/* Hosted ABI 2 location entry points. file is a compiler-owned, immutable,
 * NUL-terminated logical UTF-8 path valid for the entire call. NULL selects
 * the legacy ABI 1 location format; no process-global file state is retained. */
void ax_rt_enter_v2(const char *file, int64_t line, int64_t col);
void ax_rt_print_i64_v2(int64_t value, const char *file,
                        int64_t line, int64_t col);
void ax_rt_print_f64_v2(double value, const char *file,
                        int64_t line, int64_t col);
void ax_rt_print_bool_v2(int32_t value, const char *file,
                         int64_t line, int64_t col);
_Noreturn void ax_rt_trap_v2(int32_t kind, const char *file,
                            int64_t line, int64_t col);

/* Internal hosted-runtime extension. Only one synchronous tensor scope may
 * register cleanup; source programs cannot name or retain this ABI. */
typedef void (*ax_rt_cleanup_fn)(void *context);
int32_t ax_rt_register_cleanup(ax_rt_cleanup_fn cleanup, void *context);
int32_t ax_rt_unregister_cleanup(void *context);
int32_t ax_rt_cleanup_active(void);
_Noreturn void ax_rt_tensor_runtime_mismatch(void);

#endif
