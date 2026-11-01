// VKey - Runtime-gated debug logger
// SPDX-License-Identifier: GPL-3.0-only

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "core/Version.h"

#include <clocale>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#include <share.h>
#include "core/WinStrings.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#endif

namespace NextKey {

std::atomic<bool> Logger::enabled_{false};

namespace {

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

// Lazy state behind Mutex().
std::wstring& InstallDir() { static std::wstring s; return s; }
std::wstring& PathOverride() { static std::wstring s; return s; }
std::wstring& ResolvedPath() { static std::wstring s; return s; }
std::wstring& RoleTag() { static std::wstring s; return s; }
// Frozen by OpenFileUnlocked() at file-creation time so all log lines within
// one enable-session land in the same file. Cleared whenever the open file
// closes (SetEnabled(false), SetInstallDir, SetRoleTag, SetLogPathForTesting).
// ResolvePathUnlocked() is a pure preview when this is empty — it reports
// what the next OpenFileUnlocked() *would* create without storing anything.
std::wstring& CachedTimestamp() { static std::wstring s; return s; }
std::FILE*& File() { static std::FILE* f = nullptr; return f; }

void ResetCachedPathStateUnlocked() {
    ResolvedPath().clear();
    CachedTimestamp().clear();
}

#ifdef _WIN32

std::wstring CurrentTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[24] = {0};
    _snwprintf_s(buf, 24, _TRUNCATE, L"%02u%02u%04u_%02u%02u",
                 st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    return std::wstring(buf);
}

bool FileExistsW(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// First line of every log file. A bug report that arrives without it costs a
// round trip asking which build and which Windows the reporter is on — and the
// answers routinely disagree with what the issue template says. The host exe
// matters too: the same log line means different things coming from VKey.exe
// and from the TIP loaded inside a browser.
void WriteSessionBannerUnlocked() {
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    // GetVersionEx lies without a matching manifest; RtlGetVersion does not.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        using RtlGetVersionPtr = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion == nullptr || rtlGetVersion(&os) != 0) os = {};
    }
    wchar_t host[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, host, MAX_PATH);
    const wchar_t* hostName = wcsrchr(host, L'\\');
    hostName = (hostName != nullptr) ? hostName + 1 : host;

    std::wstring buildVer = GetBuildVersion(__DATE__, __TIME__);
    if (VKEY_CI_RUN_WSTR[0] != L'\0') {
        fwprintf(File(),
                 L"=== VKey v%hs (build %ls, run #%ls, commit %hs) | Windows %lu.%lu.%lu | host %ls | PID %lu ===\n",
                 VKEY_VERSION_STR, buildVer.c_str(), VKEY_CI_RUN_WSTR,
                 VKEY_GIT_HASH_STR, os.dwMajorVersion, os.dwMinorVersion,
                 os.dwBuildNumber, hostName, GetCurrentProcessId());
    } else {
        fwprintf(File(),
                 L"=== VKey v%hs (build %ls, commit %hs) | Windows %lu.%lu.%lu | host %ls | PID %lu ===\n",
                 VKEY_VERSION_STR, buildVer.c_str(),
                 VKEY_GIT_HASH_STR, os.dwMajorVersion, os.dwMinorVersion,
                 os.dwBuildNumber, hostName, GetCurrentProcessId());
    }
    std::fflush(File());
}

bool DirectoryWritable(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    // Probe with a temp file (CREATE_ALWAYS + FILE_FLAG_DELETE_ON_CLOSE).
    wchar_t probe[MAX_PATH] = {0};
    _snwprintf_s(probe, MAX_PATH, _TRUNCATE, L"%ls\\.vkey_probe_%lu",
                 dir.c_str(), GetCurrentProcessId());
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS,
                           FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::wstring AppDataLogFolder() {
    wchar_t buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = std::wstring(buf) + L"\\VKey\\logs";
        // Ensure folder exists. SHCreateDirectoryExW creates intermediate dirs.
        int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS) {
            return dir;
        }
    }
    return L"";
}

std::wstring ProcessTag() {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0) return L"unknown";
    std::wstring base = PathBasename(path);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    return base;
}

std::wstring DefaultInstallDir() {
    // Caller-provided install dir wins (DLL sets this from its own module).
    if (!InstallDir().empty()) return InstallDir();
    // Else: directory of the host process exe.
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L"";
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : s.substr(0, slash);
}

std::wstring ResolveFolderUnlocked() {
    if (!PathOverride().empty()) {
        std::wstring p = PathOverride();
        size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
    }
    std::wstring install = DefaultInstallDir();
    if (DirectoryWritable(install)) return install;
    return AppDataLogFolder();
}

std::wstring ResolvePathUnlocked() {
    if (!PathOverride().empty()) return PathOverride();
    std::wstring folder = ResolveFolderUnlocked();
    if (folder.empty()) return L"";

    wchar_t pidBuf[16] = {0};
    _snwprintf_s(pidBuf, 16, _TRUNCATE, L"%lu", GetCurrentProcessId());

    // Legacy fallback when SetRoleTag was never called (tests, older callers).
    if (RoleTag().empty()) {
        return folder + L"\\VKey_" + ProcessTag() + L"_" + pidBuf + L".log";
    }

    // Pure read: if the timestamp is already frozen (file opened in this
    // enable-session), reuse it so the path stays stable. Otherwise compute
    // a transient preview — OpenFileUnlocked() is the only place that *stores*
    // the timestamp, so previews never mutate module state.
    const std::wstring stamp = CachedTimestamp().empty()
                                   ? CurrentTimestamp()
                                   : CachedTimestamp();

    std::wstring base = folder + L"\\VKey_" + RoleTag() + L"_" + stamp;
    // PID suffix: TSF-* roles always need it (many DLL hosts share one timestamp
    // minute). For other roles, only attach as a collision tiebreaker.
    bool needPid = (RoleTag().compare(0, 4, L"TSF-") == 0);
    std::wstring candidate = base + L".log";
    if (!needPid && FileExistsW(candidate)) {
        needPid = true;
    }
    if (needPid) {
        candidate = base + L"_p" + pidBuf + L".log";
    }
    return candidate;
}

void OpenFileUnlocked() {
    if (File()) return;
    // Freeze the timestamp before resolving the path. This is the single
    // writer for CachedTimestamp() — any GetCurrentLogPath() previews stay
    // pure reads. RoleTag-empty (legacy) callers ignore the stamp anyway.
    if (CachedTimestamp().empty() && !RoleTag().empty()) {
        CachedTimestamp() = CurrentTimestamp();
    }
    std::wstring path = ResolvePathUnlocked();
    if (path.empty()) return;
    ResolvedPath() = path;
    // Append mode + UTF-8 transcoding. Filename uniqueness (role tag + minute
    // timestamp, plus _p<PID> for TSF-* roles or same-minute collisions) means
    // one writer per file. The handle stays open for the whole enable-session
    // (closed by SetEnabled(false)/SetInstallDir/SetRoleTag/SetLogPathForTesting
    // /Shutdown) — opening and closing it per line was a per-keystroke open/close
    // syscall storm in the LL keyboard hook path (root cause of debug-log typing
    // lag). _SH_DENYWR keeps the single-writer guarantee while still letting the
    // user open/read/copy the log while VKey appends. We still fflush after each
    // line so a crash doesn't lose the tail — issue-#108 bug reports need the
    // last lines.
    File() = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYWR);
    if (File()) {
        setvbuf(File(), nullptr, _IOLBF, 4096);
        WriteSessionBannerUnlocked();
    }
}

#else  // POSIX (tests only)

bool DirectoryWritable(const std::wstring& /*dir*/) { return false; }
std::wstring AppDataLogFolder() { return L""; }
std::wstring DefaultInstallDir() { return L""; }
std::wstring ProcessTag() { return L"test"; }

std::wstring CurrentTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    wchar_t buf[24] = {0};
    swprintf(buf, 24, L"%02d%02d%04d_%02d%02d",
             tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900,
             tmv.tm_hour, tmv.tm_min);
    return std::wstring(buf);
}

bool FileExistsW(const std::wstring& path) {
    std::string narrow; narrow.reserve(path.size());
    for (wchar_t c : path) narrow.push_back(static_cast<char>(c & 0x7F));
    struct stat st;
    return ::stat(narrow.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string ToNarrow(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0x7F));
    return s;
}

std::wstring ResolveFolderUnlocked() {
    if (!PathOverride().empty()) {
        std::wstring p = PathOverride();
        size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
    }
    return L"/tmp/vkey-logs";
}

std::wstring ResolvePathUnlocked() {
    if (!PathOverride().empty()) return PathOverride();
    std::wstring folder = ResolveFolderUnlocked();
    if (folder.empty()) return L"";

    wchar_t pidBuf[16] = {0};
    swprintf(pidBuf, 16, L"%d", static_cast<int>(getpid()));

    if (RoleTag().empty()) {
        // Legacy POSIX path — keep stable name for older tests.
        return folder + L"/vkey.log";
    }

    // Preview path stays pure — see Win32 branch / CachedTimestamp() doc above.
    const std::wstring stamp = CachedTimestamp().empty()
                                   ? CurrentTimestamp()
                                   : CachedTimestamp();

    std::wstring base = folder + L"/VKey_" + RoleTag() + L"_" + stamp;
    bool needPid = (RoleTag().compare(0, 4, L"TSF-") == 0);
    std::wstring candidate = base + L".log";
    if (!needPid && FileExistsW(candidate)) {
        needPid = true;
    }
    if (needPid) {
        candidate = base + L"_p" + pidBuf + L".log";
    }
    return candidate;
}

void OpenFileUnlocked() {
    if (File()) return;
    // Single writer for CachedTimestamp() — keeps GetCurrentLogPath() preview
    // pure. See Win32 branch comment above.
    if (CachedTimestamp().empty() && !RoleTag().empty()) {
        CachedTimestamp() = CurrentTimestamp();
    }
    std::wstring path = ResolvePathUnlocked();
    if (path.empty()) return;
    // Ensure parent dir exists (mkdir -p for /tmp/vkey-logs).
    std::wstring folder = ResolveFolderUnlocked();
    if (!folder.empty()) {
        std::string fnarrow = ToNarrow(folder);
        mkdir(fnarrow.c_str(), 0755);
    }
    ResolvedPath() = path;
    std::string narrow = ToNarrow(path);
    // Default "C" locale can't transcode wide → multibyte. Switch once so
    // fwprintf(L"...") works. Only matters in test/Linux builds.
    static bool localeSet = false;
    if (!localeSet) {
        if (std::setlocale(LC_ALL, "C.UTF-8") == nullptr) {
            std::setlocale(LC_ALL, "en_US.UTF-8");
        }
        localeSet = true;
    }
    File() = std::fopen(narrow.c_str(), "a");
    if (File()) {
        // Orient stream to wide before any non-wide write happens.
        fwide(File(), 1);
        setvbuf(File(), nullptr, _IOLBF, 4096);
    }
}

#endif

void CloseFileUnlocked() {
    if (File()) {
        std::fflush(File());
        std::fclose(File());
        File() = nullptr;
    }
}

void WriteLineUnlocked(const wchar_t* fmt, va_list args) {
    if (!File()) OpenFileUnlocked();
    if (!File()) return;

    // Format the whole line into a stack buffer first — single point of
    // truth for both the file sink and (in _DEBUG) the OutputDebugStringW
    // sink. Doing the formatting twice would double-evaluate variadic args
    // that have side effects.
    wchar_t line[1024];
    int prefixLen = 0;
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    prefixLen = _snwprintf_s(line, 1024, _TRUNCATE,
                             L"[%02u:%02u:%02u.%03u] [PID:%lu] ",
                             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                             GetCurrentProcessId());
#else
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    prefixLen = swprintf(line, 1024,
                         L"[%02d:%02d:%02d] [PID:%d] ",
                         tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                         static_cast<int>(getpid()));
#endif
    if (prefixLen < 0) prefixLen = 0;
    if (prefixLen >= 1024) prefixLen = 1023;

    int bodyLen = 0;
    const int remaining = 1024 - prefixLen - 2;  // leave room for "\n\0"
    if (remaining > 0) {
#ifdef _WIN32
        bodyLen = _vsnwprintf_s(line + prefixLen, remaining + 2, _TRUNCATE,
                                fmt, args);
#else
        bodyLen = vswprintf(line + prefixLen, remaining + 2, fmt, args);
#endif
    }
    if (bodyLen < 0) bodyLen = remaining;  // truncated — fill to room
    int totalLen = prefixLen + bodyLen;
    if (totalLen > 1022) totalLen = 1022;
    line[totalLen]     = L'\n';
    line[totalLen + 1] = L'\0';

    fputws(line, File());
    // Per-line flush keeps the tail crash-safe (a crash loses at most the line
    // in flight — issue-#108 bug reports need the last lines). The file is NOT
    // closed here: it stays open for the whole enable-session. Closing it after
    // every line meant each keystroke paid an open/close syscall pair (plus an
    // antivirus scan-on-close) inside the LL keyboard hook — the root cause of
    // debug-log typing lag. Session-end teardown closes it.
    //
    // TODO(perf, Rule 12.2): when debug_log is ON, this fflush (a WriteFile) and
    // the Mutex() guarding this function still put file-I/O + a lock on the LL-
    // hook path — a Pillar #1 violation that only the OFF-by-default gate keeps
    // tolerable. The full fix is async: the hook thread enqueues formatted lines
    // to a lock-free MPSC queue drained by a background writer. This commit only
    // removes the per-line open/close storm; async logging is left as a follow-up.
    std::fflush(File());

#if defined(_DEBUG) || defined(NEXTKEY_DEBUG)
#ifdef _WIN32
    OutputDebugStringW(line);
#endif
#endif
}

}  // namespace

void Logger::SetEnabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    bool wasEnabled = enabled_.load(std::memory_order_acquire);
    if (wasEnabled == enabled) return;
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        CloseFileUnlocked();
        // Drop the cached timestamp so the next enable-session resolves to a
        // fresh filename (HHMM at that moment) — keeps "one file per session"
        // for user-driven toggling.
        ResetCachedPathStateUnlocked();
    }
    // On enable: defer file open until first Log() (lazy).
}

void Logger::SetInstallDir(const std::wstring& dir) noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    if (InstallDir() == dir) return;
    InstallDir() = dir;
    // Always close + clear so the next Log() re-resolves at the new path.
    // Previous "skip if enabled" branch left the open handle pointing at the
    // host-process directory when the DLL was loaded with the toggle on.
    CloseFileUnlocked();
    ResetCachedPathStateUnlocked();
}

void Logger::SetRoleTag(const std::wstring& tag) noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    if (RoleTag() == tag) return;
    RoleTag() = tag;
    CloseFileUnlocked();
    ResetCachedPathStateUnlocked();
}

void Logger::SetLogPathForTesting(const std::wstring& path) noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    PathOverride() = path;
    CloseFileUnlocked();
    ResetCachedPathStateUnlocked();
}

std::wstring Logger::GetCurrentLogPath() noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    if (!ResolvedPath().empty()) return ResolvedPath();
    return ResolvePathUnlocked();
}

std::wstring Logger::GetCurrentLogFolder() noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    return ResolveFolderUnlocked();
}

void Logger::Log(const wchar_t* fmt, ...) noexcept {
    if (!enabled_.load(std::memory_order_acquire)) return;
    va_list args;
    va_start(args, fmt);
    LogV(fmt, args);
    va_end(args);
}

void Logger::LogV(const wchar_t* fmt, va_list args) noexcept {
    if (!enabled_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(Mutex());
    if (!enabled_.load(std::memory_order_acquire)) return;  // re-check after lock
    WriteLineUnlocked(fmt, args);
}

void Logger::Shutdown() noexcept {
    std::lock_guard<std::mutex> lock(Mutex());
    CloseFileUnlocked();
}

}  // namespace NextKey
