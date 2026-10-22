/* Synthetic native tool for Windows driver failure contracts only.
 * This does not simulate or qualify an actual missing SDK or linker. */
#ifndef _WIN32
#error This fixture requires native Windows.
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

static int record_compilation(void) {
    wchar_t path[32768];
    DWORD length = GetEnvironmentVariableW(L"AXEON_TEST_MOCK_CLANG_MARKER", path, 32768);
    if (length == 0 || length >= 32768) return 0;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    static const char bytes[] = "compile\n";
    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes, (DWORD)(sizeof bytes - 1), &written, NULL);
    BOOL closed = CloseHandle(file);
    return ok && closed && written == sizeof bytes - 1;
}

int main(int argc, char **argv) {
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) return 91;
    char mode[32];
    DWORD length = GetEnvironmentVariableA("AXEON_TEST_MOCK_CLANG_MODE", mode,
                                           (DWORD)sizeof mode);
    if (length == 0 || length >= sizeof mode) return 92;
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        if (strcmp(mode, "wrong-major") == 0) {
            (void)fputs("clang version 20.0.0\n", stdout);
        } else if (strcmp(mode, "version-suffix") == 0) {
            (void)fputs("clang version 19.1.70\n", stdout);
        } else if (strcmp(mode, "malformed-version") == 0) {
            (void)fputs("not a supported compiler\n", stdout);
        } else if (strcmp(mode, "embedded-version") == 0) {
            (void)fputs("clang version 20.0.0 (compatibility 19.1.7 )\n", stdout);
        } else if (strcmp(mode, "probe-failure") == 0) {
            (void)fputs("clang version 19.1.7\n", stdout);
            (void)fputs("synthetic probe failure\n", stderr);
            return 1;
        } else if (strcmp(mode, "compiler-failure") == 0) {
            (void)fputs("clang version 19.1.7\n", stdout);
        } else {
            return 92;
        }
        return 0;
    }
    if (!record_compilation()) return 94;
    if (strcmp(mode, "compiler-failure") != 0) {
        (void)fputs("unexpected compilation after rejected probe\n", stderr);
        return 93;
    }
    (void)fputs("synthetic compiler stdout\n", stdout);
    (void)fputs("synthetic compiler stderr\n", stderr);
    return 9;
}
