// <https://devblogs.microsoft.com/oldnewthing/20111216-00/?p=8873>
#define _WIN32_WINNT 0x0600 // For extended process API

#include "util/process.h"

#include <processthreadsapi.h>

#include <assert.h>

#include "util/log.h"
#include "util/str.h"

#define CMD_MAX_LEN 8192

static bool
build_cmd(char *cmd, size_t len, const char *const argv[]) {
    // Windows command-line parsing is WTF:
    // <http://daviddeley.com/autohotkey/parameters/parameters.htm#WINPASS>
    // only make it work for this very specific program
    // (don't handle escaping nor quotes)
    size_t ret = sc_str_join(cmd, argv, ' ', len);
    if (ret >= len) {
        LOGE("Command too long (%" SC_PRIsizet " chars)", len - 1);
        return false;
    }
    return true;
}

enum sc_process_result
sc_process_execute_p(const char *const argv[], HANDLE *handle, unsigned inherit,
                     HANDLE *pin, HANDLE *pout, HANDLE *perr) {
    bool inherit_stdout = inherit & SC_STDOUT;
    bool inherit_stderr = inherit & SC_STDERR;

    // If pout is defined, then inherit MUST NOT contain SC_STDOUT.
    assert(!pout || !inherit_stdout);
    // If perr is defined, then inherit MUST NOT contain SC_STDERR.
    assert(!perr || !inherit_stderr);

    // Add 1 per non-NULL pointer
    unsigned handle_count = !!pin
                          + (pout || inherit_stdout)
                          + (perr || inherit_stderr);

    enum sc_process_result ret = SC_PROCESS_ERROR_GENERIC;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read_handle;
    HANDLE stdout_write_handle;
    HANDLE stderr_write_handle;
    if (pin) {
        if (!CreatePipe(&stdin_read_handle, pin, &sa, 0)) {
            perror("pipe");
            return SC_PROCESS_ERROR_GENERIC;
        }
        if (!SetHandleInformation(*pin, HANDLE_FLAG_INHERIT, 0)) {
            LOGE("SetHandleInformation stdin failed");
            goto error_close_stdin;
        }
    }
    if (pout) {
        if (!CreatePipe(pout, &stdout_write_handle, &sa, 0)) {
            perror("pipe");
            goto error_close_stdin;
        }
        if (!SetHandleInformation(*pout, HANDLE_FLAG_INHERIT, 0)) {
            LOGE("SetHandleInformation stdout failed");
            goto error_close_stdout;
        }
    }
    if (perr) {
        if (!CreatePipe(perr, &stderr_write_handle, &sa, 0)) {
            perror("pipe");
            goto error_close_stdout;
        }
        if (!SetHandleInformation(*perr, HANDLE_FLAG_INHERIT, 0)) {
            LOGE("SetHandleInformation stderr failed");
            goto error_close_stderr;
        }
    }

    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    HANDLE handles[3];

    LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList = NULL;
    // Must be set even if handle_count == 0, so that stdin, stdout and stderr
    // are NOT inherited in that case
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    if (handle_count) {
        unsigned i = 0;
        if (pin) {
            si.StartupInfo.hStdInput = stdin_read_handle;
            handles[i++] = si.StartupInfo.hStdInput;
        }
        if (pout || inherit_stdout) {
            si.StartupInfo.hStdOutput = pout ? stdout_write_handle
                                             : GetStdHandle(STD_OUTPUT_HANDLE);
            handles[i++] = si.StartupInfo.hStdOutput;
        }
        if (perr || inherit_stderr) {
            si.StartupInfo.hStdError = perr ? stderr_write_handle
                                            : GetStdHandle(STD_ERROR_HANDLE);
            handles[i++] = si.StartupInfo.hStdError;
        }

        SIZE_T size;
        // Call it once to know the required buffer size
        BOOL ok =
            InitializeProcThreadAttributeList(NULL, 1, 0, &size)
                || GetLastError() == ERROR_INSUFFICIENT_BUFFER;
        if (!ok) {
            goto error_close_stderr;
        }

        lpAttributeList = malloc(size);
        if (!lpAttributeList) {
            goto error_close_stderr;
        }

        ok = InitializeProcThreadAttributeList(lpAttributeList, 1, 0, &size);
        if (!ok) {
            free(lpAttributeList);
            goto error_close_stderr;
        }

        ok = UpdateProcThreadAttribute(lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles, handle_count * sizeof(HANDLE),
                                       NULL, NULL);
        if (!ok) {
            goto error_free_attribute_list;
        }

        si.lpAttributeList = lpAttributeList;
    }

    char *cmd = malloc(CMD_MAX_LEN);
    if (!cmd || !build_cmd(cmd, CMD_MAX_LEN, argv)) {
        goto error_free_attribute_list;
    }

    wchar_t *wide = sc_str_to_wchars(cmd);
    free(cmd);
    if (!wide) {
        LOGC("Could not allocate wide char string");
        goto error_free_attribute_list;
    }

    BOOL bInheritHandles = handle_count > 0;
    DWORD dwCreationFlags = handle_count > 0 ? EXTENDED_STARTUPINFO_PRESENT : 0;
    BOOL ok = CreateProcessW(NULL, wide, NULL, NULL, bInheritHandles,
                             dwCreationFlags, NULL, NULL, &si.StartupInfo, &pi);
    free(wide);
    if (!ok) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            ret = SC_PROCESS_ERROR_MISSING_BINARY;
        }
        goto error_free_attribute_list;
    }

    if (lpAttributeList) {
        DeleteProcThreadAttributeList(lpAttributeList);
        free(lpAttributeList);
    }

    // These handles are used by the child process, close them for this process
    if (pin) {
        CloseHandle(stdin_read_handle);
    }
    if (pout) {
        CloseHandle(stdout_write_handle);
    }
    if (perr) {
        CloseHandle(stderr_write_handle);
    }

    *handle = pi.hProcess;

    return SC_PROCESS_SUCCESS;

error_free_attribute_list:
    if (lpAttributeList) {
        DeleteProcThreadAttributeList(lpAttributeList);
        free(lpAttributeList);
    }
error_close_stderr:
    if (perr) {
        CloseHandle(*perr);
        CloseHandle(stderr_write_handle);
    }
error_close_stdout:
    if (pout) {
        CloseHandle(*pout);
        CloseHandle(stdout_write_handle);
    }
error_close_stdin:
    if (pin) {
        CloseHandle(*pin);
        CloseHandle(stdin_read_handle);
    }

    return ret;
}

bool
sc_process_terminate(HANDLE handle) {
    return TerminateProcess(handle, 1);
}

sc_exit_code
sc_process_wait(HANDLE handle, bool close) {
    DWORD code;
    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0
            || !GetExitCodeProcess(handle, &code)) {
        // could not wait or retrieve the exit code
        code = SC_EXIT_CODE_NONE;
    }
    if (close) {
        CloseHandle(handle);
    }
    return code;
}

void
sc_process_close(HANDLE handle) {
    bool closed = CloseHandle(handle);
    assert(closed);
    (void) closed;
}

ssize_t
sc_pipe_read(HANDLE pipe, char *data, size_t len) {
    DWORD r;
    if (!ReadFile(pipe, data, len, &r, NULL)) {
        return -1;
    }
    return r;
}

void
sc_pipe_close(HANDLE pipe) {
    if (!CloseHandle(pipe)) {
        LOGW("Cannot close pipe");
    }
}
