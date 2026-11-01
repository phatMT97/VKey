// VKey - Always-on crash exception logger
// SPDX-License-Identifier: GPL-3.0-only

#include "CrashLog.h"

#include "Version.h"

#ifdef _WIN32
#include <Windows.h>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace NextKey {

#ifdef _WIN32

namespace {

HANDLE OpenLogFile() noexcept {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return INVALID_HANDLE_VALUE;

    wchar_t* lastSep = nullptr;
    for (DWORD i = 0; i < len; ++i) {
        if (exePath[i] == L'\\' || exePath[i] == L'/') lastSep = &exePath[i];
    }
    if (!lastSep) return INVALID_HANDLE_VALUE;

    *(lastSep + 1) = L'\0';
    wchar_t logPath[MAX_PATH] = {};
    if (swprintf_s(logPath, L"%ls_vkey_crash.log", exePath) < 0) {
        return INVALID_HANDLE_VALUE;
    }

    return CreateFileW(logPath,
                       FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr,
                       OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

}  // namespace

void CrashLog(const wchar_t* context, const char* what) noexcept {
    HANDLE h = OpenLogFile();
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[1024];
    // context is widechar — convert to narrow via %ls (MSVC-specific but
    // portable across the Windows builds we target). Truncation is fine:
    // crash context strings are short class/method names.
    int n = _snprintf_s(line, _TRUNCATE,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] v" VKEY_VERSION_STR " (" VKEY_GIT_HASH_STR ") PID=%lu TID=%lu %ls: %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId(),
        context ? context : L"(null)",
        what ? what : "(no message)");

    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(h);
    }
    CloseHandle(h);
}

void WriteCrashDump(::_EXCEPTION_POINTERS* ep, const wchar_t* tag) noexcept {
    // Build "<exe_dir>\<tag>_<pid>_<time>.dmp" (same dir as the crash log).
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    wchar_t* lastSep = nullptr;
    for (DWORD i = 0; i < len; ++i) {
        if (exePath[i] == L'\\' || exePath[i] == L'/') lastSep = &exePath[i];
    }
    if (!lastSep) return;
    *(lastSep + 1) = L'\0';

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t dumpPath[MAX_PATH] = {};
    if (swprintf_s(dumpPath, L"%ls%ls_%lu_%04u%02u%02u_%02u%02u%02u.dmp",
            exePath, tag ? tag : L"vkey_crash", GetCurrentProcessId(),
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond) < 0) {
        return;
    }

    HANDLE h = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                      MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(h);
}

#else

void CrashLog(const wchar_t*, const char*) noexcept {
    // No-op on non-Windows (tests build)
}

#endif

}  // namespace NextKey
