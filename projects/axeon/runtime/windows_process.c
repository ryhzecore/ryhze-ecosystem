/* Bounded native Windows process adapter. See docs/windows-process-design.md. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum { AX_COMMAND_CHARS = 32767, AX_CLEANUP_MS = 5000, AX_POLL_MS = 50 };
#define AX_MEMORY_BYTES ((SIZE_T)2 * 1024 * 1024 * 1024)
#define AX_TICKS_PER_SECOND 10000000LL

typedef struct {
    wchar_t *text;
    size_t used;
} CommandLine;

static BOOL append(CommandLine *line, wchar_t value) {
    if (line->used >= AX_COMMAND_CHARS - 1) return FALSE;
    line->text[line->used++] = value;
    return TRUE;
}

/* CRT argv rules: double backslashes before the closing quote; before an
   embedded quote double them and add the quote's escape. No shell is used. */
static BOOL argument(CommandLine *line, const wchar_t *value) {
    if (!append(line, L'"')) return FALSE;
    while (*value != L'\0') {
        size_t slashes = 0;
        while (*value == L'\\') { ++slashes; ++value; }
        size_t copies = (*value == L'"' || *value == L'\0') ? slashes * 2 : slashes;
        while (copies-- != 0) if (!append(line, L'\\')) return FALSE;
        if (*value == L'\0') break;
        if (*value == L'"' && !append(line, L'\\')) return FALSE;
        if (!append(line, *value++)) return FALSE;
    }
    return append(line, L'"');
}

static BOOL timeout_value(const wchar_t *text, DWORD *milliseconds) {
    DWORD seconds = 0;
    if (*text == L'\0') return FALSE;
    for (; *text != L'\0'; ++text) {
        if (*text < L'0' || *text > L'9' || seconds > 360) return FALSE;
        seconds = seconds * 10 + (DWORD)(*text - L'0');
    }
    if (seconds == 0 || seconds > 3600) return FALSE;
    *milliseconds = seconds * 1000;
    return TRUE;
}

static BOOL executable_path(const wchar_t *name, wchar_t *resolved) {
    DWORD length;
    if (*name == L'\0' || wcschr(name, L'"') != NULL) {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    if (wcschr(name, L'\\') || wcschr(name, L'/') || wcschr(name, L':')) {
        length = GetFullPathNameW(name, AX_COMMAND_CHARS, resolved, NULL);
    } else {
        length = SearchPathW(NULL, name, L".exe", AX_COMMAND_CHARS, resolved, NULL);
    }
    if (length == 0) return FALSE;
    if (length >= AX_COMMAND_CHARS) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    return TRUE;
}

static HANDLE standard_handle(DWORD kind, DWORD access) {
    HANDLE source = GetStdHandle(kind), copy = NULL;
    if (source != NULL && source != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                             &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
        return copy;
    }
    SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
    copy = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &security, OPEN_EXISTING, 0, NULL);
    return copy == INVALID_HANDLE_VALUE ? NULL : copy;
}

static void report(const wchar_t *operation, DWORD code) {
    (void)fwprintf(stderr, L"axeon-process: %ls failed (Windows error %lu)\n",
                   operation, (unsigned long)code);
}

/* Only a fresh ordinary disk file is an accounting destination. The handle is
   private, non-inherited and held until the final bounded record is written. */
static HANDLE accounting_file(const wchar_t *path) {
    if (*path == L'\0' || wcslen(path) >= AX_COMMAND_CHARS ||
        (path[0] == L'\\' && path[1] == L'\\') ||
        (path[0] == L'/' && path[1] == L'/')) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    for (size_t index = 0; path[index] != L'\0'; ++index) {
        if (path[index] == L':' && !(index == 1 &&
            ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))) {
            SetLastError(ERROR_INVALID_NAME);
            return INVALID_HANDLE_VALUE;
        }
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return file;
    if (GetFileType(file) != FILE_TYPE_DISK) {
        (void)CloseHandle(file);
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

static BOOL write_accounting(HANDLE file, const JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *accounting,
    BOOL valid, BOOL cleanup_ok, BOOL limits_applied, BOOL created, DWORD child_pid,
    DWORD cpu_seconds, DWORD timeout_ms, DWORD code, const char *status) {
    char bytes[1024];
    DWORD written = 0;
    int length = snprintf(bytes, sizeof bytes,
        "{\"schema_version\":1,\"user_cpu_ticks\":%llu,\"kernel_cpu_ticks\":%llu,"
        "\"cpu_seconds\":%lu,\"wall_seconds\":%lu,\"memory_bytes\":2147483648,"
        "\"active_process_limit\":256,\"exit_code\":%lu,\"cleanup_succeeded\":%s,"
        "\"status\":\"%s\",\"accounting_valid\":%s,\"limits_applied\":%s,"
        "\"child_created\":%s,\"supervisor_pid\":%lu,\"child_pid\":%lu,"
        "\"total_processes\":%lu,\"active_processes\":%lu,\"limit_terminated_processes\":%lu}\n",
        (unsigned long long)accounting->TotalUserTime.QuadPart,
        (unsigned long long)accounting->TotalKernelTime.QuadPart,
        (unsigned long)cpu_seconds, (unsigned long)(timeout_ms / 1000), (unsigned long)code,
        cleanup_ok ? "true" : "false", status, valid ? "true" : "false",
        limits_applied ? "true" : "false", created ? "true" : "false",
        (unsigned long)GetCurrentProcessId(), (unsigned long)child_pid,
        (unsigned long)accounting->TotalProcesses, (unsigned long)accounting->ActiveProcesses,
        (unsigned long)accounting->TotalTerminatedProcesses);
    if (length <= 0 || (size_t)length >= sizeof bytes) {
        report(L"accounting record size", ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (!WriteFile(file, bytes, (DWORD)length, &written, NULL)) {
        report(L"accounting write", GetLastError());
        return FALSE;
    }
    if (written != (DWORD)length) {
        report(L"accounting short write", ERROR_WRITE_FAULT);
        return FALSE;
    }
    return TRUE;
}

/* Terminate only this private job and wait for its accounting to become empty.
   The non-inherited kill-on-close handle is also the abrupt-exit backstop. */
static BOOL finish_job(HANDLE job, DWORD code) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
    if (!TerminateJobObject(job, code)) {
        report(L"TerminateJobObject", GetLastError());
        return FALSE;
    }
    ULONGLONG started = GetTickCount64();
    for (;;) {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                       &accounting, sizeof accounting, NULL)) {
            report(L"cleanup accounting", GetLastError());
            return FALSE;
        }
        if (accounting.ActiveProcesses == 0) return TRUE;
        if (GetTickCount64() - started >= AX_CLEANUP_MS) {
            report(L"job cleanup deadline", WAIT_TIMEOUT);
            return FALSE;
        }
        Sleep(10);
    }
}

int wmain(int argc, wchar_t **argv) {
    DWORD timeout_ms = 0, cpu_seconds = 180, result = 125;
    LONGLONG cpu_ticks = 180 * AX_TICKS_PER_SECOND;
    HANDLE job = NULL, streams[3] = {NULL, NULL, NULL};
    HANDLE accounting_output = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process = {0};
    STARTUPINFOEXW startup = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    SIZE_T attribute_bytes = 0;
    BOOL attributes_ready = FALSE, created = FALSE, limits_applied = FALSE;
    BOOL cleanup_ok = TRUE, accounting_valid = FALSE, cpu_supplied = FALSE;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION final_accounting = {0};
    const wchar_t *accounting_path = NULL;
    const char *status = "adapter_error";
    int program = 3;
    wchar_t *application = NULL;
    CommandLine line = {NULL, 0};
    if (argc < 5 || wcscmp(argv[1], L"--timeout-seconds") != 0 ||
        !timeout_value(argv[2], &timeout_ms)) goto usage;
    while (program < argc && wcscmp(argv[program], L"--") != 0) {
        if (program + 1 >= argc) goto usage;
        if (wcscmp(argv[program], L"--cpu-seconds") == 0 && !cpu_supplied) {
            DWORD cpu_ms = 0;
            if (!timeout_value(argv[program + 1], &cpu_ms) || cpu_ms > 180000) goto usage;
            cpu_seconds = cpu_ms / 1000;
            cpu_ticks = cpu_seconds * AX_TICKS_PER_SECOND;
            cpu_supplied = TRUE;
        } else if (wcscmp(argv[program], L"--accounting") == 0 && accounting_path == NULL &&
                   *argv[program + 1] != L'\0') {
            accounting_path = argv[program + 1];
        } else {
            goto usage;
        }
        program += 2;
    }
    if (program >= argc - 1) goto usage;
    ++program;
    if (accounting_path != NULL) {
        accounting_output = accounting_file(accounting_path);
        if (accounting_output == INVALID_HANDLE_VALUE) {
            report(L"accounting create", GetLastError());
            goto cleanup;
        }
    }
    application = HeapAlloc(GetProcessHeap(), 0, AX_COMMAND_CHARS * sizeof(wchar_t));
    line.text = HeapAlloc(GetProcessHeap(), 0, AX_COMMAND_CHARS * sizeof(wchar_t));
    if (!application || !line.text) {
        report(L"command allocation", ERROR_NOT_ENOUGH_MEMORY);
        goto cleanup;
    }
    if (!executable_path(argv[program], application)) {
        report(L"executable lookup", GetLastError());
        goto cleanup;
    }
    if (!argument(&line, application)) goto command_limit;
    for (int index = program + 1; index < argc; ++index) {
        if (!append(&line, L' ') || !argument(&line, argv[index])) goto command_limit;
    }
    line.text[line.used] = L'\0';
    job = CreateJobObjectW(NULL, NULL);
    if (job == NULL) { report(L"CreateJobObjectW", GetLastError()); goto cleanup; }
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_TIME |
        JOB_OBJECT_LIMIT_JOB_TIME | JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 256;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = cpu_ticks;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = cpu_ticks;
    limits.JobMemoryLimit = AX_MEMORY_BYTES;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
        report(L"job limits", GetLastError()); goto cleanup;
    }
    limits_applied = TRUE;
    streams[0] = standard_handle(STD_INPUT_HANDLE, GENERIC_READ);
    streams[1] = standard_handle(STD_OUTPUT_HANDLE, GENERIC_WRITE);
    streams[2] = standard_handle(STD_ERROR_HANDLE, GENERIC_WRITE);
    if (!streams[0] || !streams[1] || !streams[2]) {
        report(L"standard handle inheritance", GetLastError()); goto cleanup;
    }
    (void)InitializeProcThreadAttributeList(NULL, 2, 0, &attribute_bytes);
    if (attribute_bytes == 0) {
        report(L"attribute sizing", GetLastError()); goto cleanup;
    }
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attribute_bytes);
    if (startup.lpAttributeList == NULL) {
        report(L"attribute allocation", ERROR_NOT_ENOUGH_MEMORY); goto cleanup;
    }
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0, &attribute_bytes)) {
        report(L"attribute initialization", GetLastError()); goto cleanup;
    }
    attributes_ready = TRUE;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, streams, sizeof streams, NULL, NULL) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof job, NULL, NULL)) {
        report(L"process containment attributes", GetLastError()); goto cleanup;
    }
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = streams[0];
    startup.StartupInfo.hStdOutput = streams[1];
    startup.StartupInfo.hStdError = streams[2];
    if (!CreateProcessW(application, line.text, NULL, NULL, TRUE,
            CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            NULL, NULL, &startup.StartupInfo, &process)) {
        report(L"CreateProcessW", GetLastError()); goto cleanup;
    }
    created = TRUE;
    BOOL in_job = FALSE;
    BOOL checked_job = IsProcessInJob(process.hProcess, job, &in_job);
    if (!checked_job || !in_job) {
        report(L"child job verification", checked_job ? ERROR_INVALID_DATA : GetLastError());
        /* The exact owned handle is safe even if the containment query failed. */
        if (!TerminateProcess(process.hProcess, 125)) report(L"suspended child termination", GetLastError());
        (void)WaitForSingleObject(process.hProcess, AX_CLEANUP_MS);
        goto cleanup;
    }
    ULONGLONG started = GetTickCount64();
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        report(L"ResumeThread", GetLastError()); goto cleanup;
    }
    for (;;) {
        ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeout_ms) { result = 124; status = "wall_limit"; break; }
        DWORD remaining = timeout_ms - (DWORD)elapsed;
        DWORD waited = WaitForSingleObject(process.hProcess,
            remaining < AX_POLL_MS ? remaining : AX_POLL_MS);
        if (waited == WAIT_OBJECT_0) {
            if (!GetExitCodeProcess(process.hProcess, &result)) {
                report(L"child exit code", GetLastError()); result = 125;
            } else {
                status = "exited";
            }
            break;
        }
        if (waited != WAIT_TIMEOUT) {
            report(L"child wait", GetLastError()); break;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                &accounting, sizeof accounting, NULL)) {
            report(L"resource accounting", GetLastError()); break;
        }
        if (accounting.TotalUserTime.QuadPart + accounting.TotalKernelTime.QuadPart >= cpu_ticks) {
            (void)fputws(L"axeon-process: job CPU limit exceeded\n", stderr);
            result = 124; status = "cpu_limit"; break;
        }
        if (GetTickCount64() - started >= timeout_ms) { result = 124; status = "wall_limit"; break; }
    }
    goto cleanup;

command_limit:
    report(L"command line budget", ERROR_FILENAME_EXCED_RANGE);
cleanup:
    if (created) {
        cleanup_ok = finish_job(job, result);
        if (!cleanup_ok) { result = 125; status = "cleanup_error"; }
    }
    if (accounting_output != INVALID_HANDLE_VALUE) {
        if (job != NULL) {
            accounting_valid = QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                &final_accounting, sizeof final_accounting, NULL);
            if (!accounting_valid) {
                report(L"final accounting", GetLastError());
            } else if (final_accounting.TotalUserTime.QuadPart < 0 ||
                       final_accounting.TotalKernelTime.QuadPart < 0) {
                report(L"final accounting values", ERROR_INVALID_DATA);
                accounting_valid = FALSE;
            }
            if (!accounting_valid) {
                memset(&final_accounting, 0, sizeof final_accounting);
                result = 125; status = "accounting_error";
            } else if (final_accounting.ActiveProcesses != 0) {
                report(L"final accounting active processes", ERROR_INVALID_DATA);
                cleanup_ok = FALSE; result = 125; status = "cleanup_error";
            }
        }
        if (!write_accounting(accounting_output, &final_accounting, accounting_valid, cleanup_ok,
                limits_applied, created, process.dwProcessId, cpu_seconds, timeout_ms, result, status)) {
            result = 125;
        }
        if (!CloseHandle(accounting_output)) {
            report(L"accounting close", GetLastError()); result = 125;
        }
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    if (attributes_ready) DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (startup.lpAttributeList) HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    for (int index = 0; index < 3; ++index) if (streams[index]) CloseHandle(streams[index]);
    if (line.text) HeapFree(GetProcessHeap(), 0, line.text);
    if (application) HeapFree(GetProcessHeap(), 0, application);
    /* Preserve the full Windows exit status, including a child's exception code. */
    ExitProcess(result);
usage:
    (void)fputws(L"usage: axeon-process --timeout-seconds 1..3600 [--cpu-seconds 1..180] "
                 L"[--accounting PATH] -- PROGRAM [ARG ...]\n", stderr);
    return 125;
}
