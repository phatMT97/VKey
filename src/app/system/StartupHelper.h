// VKey - Startup Helper (Windows-only)
// SPDX-License-Identifier: GPL-3.0-only
//
// Manages run-on-startup registration via Registry (normal) or
// Task Scheduler (admin/elevated). Pattern from OpenKey.

#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <exdisp.h>       // IShellWindows
// Windows.h defines `ShellExecute` as a macro aliasing `ShellExecuteA/W`.
// If the macro is active when <shldisp.h> is preprocessed, MIDL-declared
// COM method `IShellDispatch2::ShellExecute` gets renamed at header time,
// breaking direct calls. Undo the macro across the interface declaration.
#pragma push_macro("ShellExecute")
#undef ShellExecute
#include <shldisp.h>      // IShellFolderViewDual, IShellDispatch2
#pragma pop_macro("ShellExecute")
#include <servprov.h>     // IServiceProvider
#include <memory>
#include <string>
#include "UpdateSecurity.h"
#include "core/config/ConfigManager.h"
#include "core/SystemConfig.h"
#include "core/Debug.h"

namespace NextKey {

/// Registry key path for user-level startup programs
inline constexpr const wchar_t* STARTUP_REG_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* STARTUP_REG_VALUE = L"VKey";
inline constexpr const wchar_t* STARTUP_TASK_NAME = L"VKey";
inline constexpr const wchar_t* WATCHDOG_TASK_NAME = L"\\VKey\\Watchdog";
inline constexpr const wchar_t* WATCHDOG_TASK_FLAG = L"--watchdog-task";

// Forward declaration — defined below. RemoveScheduledTask() calls this before
// its definition appears in the file.
[[nodiscard]] inline bool IsScheduledTaskRegistered() noexcept;
[[nodiscard]] inline bool IsWatchdogTaskRegistered() noexcept;

/// Check if the current process is running with admin privileges
[[nodiscard]] inline bool IsRunningAsAdmin() noexcept {
    return IsUserAnAdmin() != FALSE;
}

/// Token passed in cmdline when the app is spawned by an admin-mode restart
/// (either self-elevation or toggle-driven elevation/de-elevation). The new
/// instance uses this as a signal to wait on the single-instance mutex rather
/// than exit immediately when the old instance still holds it.
inline constexpr const wchar_t* ADMIN_RESTART_FLAG = L"--admin-restart";

/// Whole-word cmdline flag match: `flag` must be preceded by start-of-string
/// or whitespace, and followed by whitespace or end-of-string. Guards against
/// substring matches (e.g. `--admin-restart` false-positive on a future flag
/// like `--admin-restart-dry-run`).
[[nodiscard]] inline bool HasCmdlineFlag(LPCWSTR cmdLine, LPCWSTR flag) noexcept {
    if (!cmdLine || !flag) return false;
    const size_t flagLen = wcslen(flag);
    if (flagLen == 0) return false;
    const wchar_t* p = cmdLine;
    while ((p = wcsstr(p, flag)) != nullptr) {
        const bool leftOk = (p == cmdLine) || (p[-1] == L' ' || p[-1] == L'\t');
        const wchar_t after = p[flagLen];
        const bool rightOk = (after == L'\0' || after == L' ' || after == L'\t');
        if (leftOk && rightOk) return true;
        p += flagLen;  // skip past this candidate and keep searching
    }
    return false;
}

/// Result of an admin-mode restart attempt.
enum class AdminRestartResult {
    NoRestartNeeded,    ///< Current elevation already matches config — nothing to do
    Restarting,         ///< New instance launched — caller should exit this process
    UacDenied,          ///< User cancelled UAC prompt for elevation; config reverted
    DeElevationFailed,  ///< Shell dispatch unavailable (no explorer, session 0, …)
};

/// Get the full path to the current executable (quoted)
[[nodiscard]] inline std::wstring GetQuotedExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\"";
}

/// Get the directory of the current executable (no trailing slash).
/// Returns empty string if GetModuleFileNameW or path parsing fails.
[[nodiscard]] inline std::wstring GetInstallDirectory() noexcept {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring pathStr(path);
    auto slash = pathStr.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return pathStr.substr(0, slash);
}

/// Remove the registry startup entry (HKCU\...\Run)
inline void RemoveRegistryStartup() noexcept {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, STARTUP_REG_VALUE);
        RegCloseKey(hKey);
    }
}

/// Set the registry startup entry (HKCU\...\Run)
[[nodiscard]] inline bool SetRegistryStartup() noexcept {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0,
                      KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    std::wstring exePath = GetQuotedExePath();

    // Idempotent: skip the write when the value already matches. Rewriting the
    // same HKCU\Run value on every toggle/launch reads as autorun-key churn to
    // AV behavior heuristics — a persistence signal we don't need to emit.
    {
        std::wstring current(exePath.size() + 1, L'\0');
        DWORD cb = static_cast<DWORD>(current.size() * sizeof(wchar_t));
        DWORD type = 0;
        LSTATUS q = RegQueryValueExW(hKey, STARTUP_REG_VALUE, nullptr, &type,
            reinterpret_cast<BYTE*>(current.data()), &cb);
        if (q == ERROR_SUCCESS && type == REG_SZ) {
            current.resize(wcslen(current.c_str()));
            if (current == exePath) {
                RegCloseKey(hKey);
                return true;  // already correct — no write
            }
        }
    }

    LSTATUS status = RegSetValueExW(
        hKey, STARTUP_REG_VALUE, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(exePath.c_str()),
        static_cast<DWORD>((exePath.size() + 1) * sizeof(wchar_t))
    );
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

/// Run schtasks.exe elevated (UAC prompt) and return success/failure
[[nodiscard]] inline bool RunSchtasksElevated(const wchar_t* args, DWORD timeoutMs = 5000) noexcept {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = L"schtasks";
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei)) return false;

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, timeoutMs);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        return exitCode == 0;
    }
    return true;
}

/// Remove the scheduled task (requires elevation for /rl highest tasks).
/// Returns true if the task was successfully deleted or didn't exist.
[[nodiscard]] inline bool RemoveScheduledTask() noexcept {
    if (!IsScheduledTaskRegistered()) return true;  // Nothing to remove
    std::wstring args = L"/delete /tn " + std::wstring(STARTUP_TASK_NAME) + L" /f";
    bool ok = RunSchtasksElevated(args.c_str());
    // Verify removal — schtasks may return 0 even on partial failure
    return ok && !IsScheduledTaskRegistered();
}

/// Create a scheduled task to run at logon with highest privileges (UAC prompt)
[[nodiscard]] inline bool CreateScheduledTaskElevated() noexcept {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Get current username BEFORE elevation — ensures task triggers for the
    // logged-in user, not the admin account used for UAC elevation.
    wchar_t username[256] = {};
    DWORD usernameSize = 256;
    GetUserNameW(username, &usernameSize);

    std::wstring exeStr(exePath);
    std::wstring dirStr = exeStr.substr(0, exeStr.find_last_of(L"\\/"));

    // Secure memory-only PowerShell script string (No %TEMP% XML files required -> 100% secure from TOCTOU)
    // Uses Cmdlets to fully configure Triggers, Actions, and disabled Battery constraints natively.
    std::wstring ps1Args = L"-NoProfile -WindowStyle Hidden -Command \"";
    // -Execute takes the BARE path. Task Scheduler stores it in the structured
    // <Command> field (launched as lpApplicationName, not a command line), so
    // embedded quotes would become literal path chars → 0x2 "cannot find the
    // file" at logon (task fires but launches nothing). Spaces are handled by
    // Task Scheduler itself. Do NOT wrap exeStr in \" \" (issue #210).
    ps1Args += L"$A = New-ScheduledTaskAction -Execute '" + EscapePowerShellSingleQuote(exeStr) + L"' -WorkingDirectory '" + EscapePowerShellSingleQuote(dirStr) + L"'; ";
    ps1Args += L"$T = New-ScheduledTaskTrigger -AtLogOn; ";
    ps1Args += L"$T.Delay = 'PT5S'; ";
    ps1Args += L"$S = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit 0; ";
    ps1Args += L"$P = New-ScheduledTaskPrincipal -UserId '" + EscapePowerShellSingleQuote(username) + L"' -LogonType Interactive -RunLevel Highest; ";
    ps1Args += L"Register-ScheduledTask -TaskName '" + std::wstring(STARTUP_TASK_NAME) + L"' -Action $A -Trigger $T -Settings $S -Principal $P -Force\"";

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = ps1Args.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei)) return false;

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 10000);  // PowerShell + Register-ScheduledTask can be slow
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        return exitCode == 0;
    }
    return true;
}

/// Create the watchdog scheduled task at \VKey\Watchdog.
/// Differences from CreateScheduledTaskElevated():
///   - Action: current VKey executable with --watchdog-task. The app verifies
///     the companion before launching it; Task Scheduler must not bypass that gate.
///   - Trigger delay: 10s (let VKey come up first; main task uses 5s)
///   - Settings: RestartCount=3, RestartInterval=1min if the launch fails.
///   - Principal RunLevel: Limited (NOT Highest) — process supervisor doesn't
///     need elevation. Keeps AV calm, no UAC needed at logon.
///   - Task path: \VKey\Watchdog (user-root folder, visible in Task
///     Scheduler MMC for user debug).
/// Requires UAC to register under \VKey\ folder.
[[nodiscard]] inline bool CreateWatchdogScheduledTask() noexcept {
    std::wstring dirStr = GetInstallDirectory();
    if (dirStr.empty()) return false;
    wchar_t appPath[MAX_PATH] = {};
    const DWORD appLength = GetModuleFileNameW(nullptr, appPath, MAX_PATH);
    if (appLength == 0 || appLength >= MAX_PATH) return false;

    // Get current username BEFORE elevation — ensures task triggers for the
    // logged-in user, not the admin account used for UAC elevation.
    wchar_t username[256] = {};
    DWORD usernameSize = 256;
    GetUserNameW(username, &usernameSize);

    // PowerShell registers the task. Watchdog runs at LIMITED RunLevel
    // (NOT Highest) — keeps AV calm, no UAC needed at logon.
    std::wstring ps1Args = L"-NoProfile -WindowStyle Hidden -Command \"";
    // Bare path — see CreateScheduledTaskElevated() (issue #210): quotes in the
    // <Command> field cause 0x2 "cannot find the file" at logon.
    ps1Args += L"$A = New-ScheduledTaskAction -Execute '" + EscapePowerShellSingleQuote(appPath) + L"' -Argument '" + WATCHDOG_TASK_FLAG + L"' -WorkingDirectory '" + EscapePowerShellSingleQuote(dirStr) + L"'; ";
    ps1Args += L"$T = New-ScheduledTaskTrigger -AtLogOn; ";
    ps1Args += L"$T.Delay = 'PT10S'; ";  // 10s after logon — let VKey come up first
    ps1Args += L"$S = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit 0 -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1); ";
    ps1Args += L"$P = New-ScheduledTaskPrincipal -UserId '" + EscapePowerShellSingleQuote(username) + L"' -LogonType Interactive -RunLevel Limited; ";
    ps1Args += L"Register-ScheduledTask -TaskName '" + std::wstring(WATCHDOG_TASK_NAME) + L"' -Action $A -Trigger $T -Settings $S -Principal $P -Force\"";

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";  // UAC required to write \VKey\ Task Scheduler folder
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = ps1Args.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei)) return false;

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 10000);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        return exitCode == 0;
    }
    return true;
}

/// Remove the watchdog scheduled task at \VKey\Watchdog.
/// No-op if task doesn't exist. Requires UAC.
inline void RemoveWatchdogScheduledTask() noexcept {
    if (!IsWatchdogTaskRegistered()) return;
    std::wstring args = L"/delete /tn \"" + std::wstring(WATCHDOG_TASK_NAME) + L"\" /f";
    (void)RunSchtasksElevated(args.c_str());
}

/// Register or unregister run-on-startup.
///
/// - enable + !asAdmin → Registry entry (normal startup)
/// - enable + asAdmin  → Task Scheduler with highest privileges (UAC prompt)
/// - !enable           → Remove both registry entry and scheduled task
inline void RegisterRunOnStartup(bool enable, bool asAdmin) {
    if (!enable) {
        // Remove both to be safe
        RemoveRegistryStartup();
        (void)RemoveScheduledTask();
        return;
    }

    if (asAdmin) {
        // Create elevated scheduled task, remove registry to avoid duplicate
        if (CreateScheduledTaskElevated()) {
            RemoveRegistryStartup();
        }
    } else {
        // Normal registry startup, remove any elevated task
        (void)RemoveScheduledTask();
        (void)SetRegistryStartup();
    }
}

/// Get the Desktop folder path for the current user
[[nodiscard]] inline std::wstring GetDesktopPath() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path))) {
        return std::wstring(path);
    }
    return {};
}

/// Get the shortcut (.lnk) path on Desktop
[[nodiscard]] inline std::wstring GetDesktopShortcutPath() {
    std::wstring desktop = GetDesktopPath();
    if (desktop.empty()) return {};
    return desktop + L"\\VKey.lnk";
}

/// Create a desktop shortcut (.lnk) pointing to the current executable.
/// Uses COM IShellLink + IPersistFile.
inline bool CreateDesktopShortcut() {
    std::wstring lnkPath = GetDesktopShortcutPath();
    if (lnkPath.empty()) return false;

    // Get unquoted exe path
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Working directory = exe directory
    std::wstring workDir(exePath);
    size_t lastSlash = workDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        workDir = workDir.substr(0, lastSlash);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellLinkW* pShellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&pShellLink));
    if (FAILED(hr) || !pShellLink) {
        CoUninitialize();
        return false;
    }

    pShellLink->SetPath(exePath);
    pShellLink->SetWorkingDirectory(workDir.c_str());
    pShellLink->SetDescription(L"VKey Vietnamese Input");
    pShellLink->SetIconLocation(exePath, 0);

    IPersistFile* pPersistFile = nullptr;
    hr = pShellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pPersistFile));
    bool ok = false;
    if (SUCCEEDED(hr) && pPersistFile) {
        hr = pPersistFile->Save(lnkPath.c_str(), TRUE);
        ok = SUCCEEDED(hr);
        pPersistFile->Release();
    }

    pShellLink->Release();
    CoUninitialize();
    return ok;
}

/// Remove the desktop shortcut if it exists
inline bool RemoveDesktopShortcut() {
    std::wstring lnkPath = GetDesktopShortcutPath();
    if (lnkPath.empty()) return false;
    return DeleteFileW(lnkPath.c_str()) != FALSE;
}

/// Create or remove the desktop shortcut
inline void SetDesktopShortcut(bool enable) {
    if (enable) {
        CreateDesktopShortcut();
    } else {
        RemoveDesktopShortcut();
    }
}

/// Check if the scheduled task exists (non-elevated query, no UAC prompt)
[[nodiscard]] inline bool IsScheduledTaskRegistered() noexcept {
    std::wstring cmdLine = L"schtasks.exe /query /tn \"" + std::wstring(STARTUP_TASK_NAME) + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return exitCode == 0;
}

/// Check if the watchdog scheduled task exists at \VKey\Watchdog
/// (non-elevated query, no UAC prompt).
[[nodiscard]] inline bool IsWatchdogTaskRegistered() noexcept {
    std::wstring cmdLine = L"schtasks.exe /query /tn \"" +
                            std::wstring(WATCHDOG_TASK_NAME) + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return exitCode == 0;
}

/// Ensure startup registration is intact. Call on app startup. Never prompts UAC.
/// If the Task Scheduler task was lost (Windows Update, antivirus, etc.),
/// silently falls back to registry and syncs config so Settings UI matches reality.
/// When elevated + admin + task missing: recreates the task (no UAC needed — already elevated).
/// Returns true if config was modified (caller should save).
[[nodiscard]] inline bool EnsureStartupRegistration(bool runAtStartup, bool& runAsAdmin) {
    if (!runAtStartup) return false;

    if (runAsAdmin) {
        // ALWAYS forcefully remove the registry startup to make sure it doesn't conflict
        // with the Scheduled Task. This prevents lingering non-elevated auto-start entries
        // from causing UAC prompts on every logon.
        RemoveRegistryStartup();

        if (IsScheduledTaskRegistered()) return false;  // Task exists, all good

        // Task missing. Only an elevated process can (re)create it without a
        // silent-startup UAC prompt.
        if (IsRunningAsAdmin()) {
            // Create, then VERIFY by re-query rather than trusting the bool.
            // PowerShell's exit code is unreliable when Register-ScheduledTask is
            // slow (the 10s WaitForSingleObject can return STILL_ACTIVE while the
            // task is still being written) — that false-negative is what flipped
            // runAsAdmin off and surfaced as "ticked admin, but it un-ticks
            // itself" (issue #210).
            (void)CreateScheduledTaskElevated();
            if (IsScheduledTaskRegistered()) {
                RemoveRegistryStartup();
                return false;
            }
            // Elevated but the task still isn't there. Keep runAsAdmin=true — the
            // user explicitly chose it, and silently flipping it off (forcing a
            // re-toggle) is the worse bug. A later elevated launch retries; don't
            // add a registry entry (it would reintroduce the double-launch UAC
            // prompt cfb4ec1 fixed). Just log so the cause is visible in the file.
            NEXTKEY_LOG(L"EnsureStartupRegistration: elevated but scheduled-task creation failed; keeping runAsAdmin=true, will retry next launch");
            return false;
        }

        // Not elevated and task missing — can't create the elevated task without a
        // UAC prompt we must not show at silent startup. Fall back to registry so
        // the app at least auto-starts (non-elevated), and sync config so Settings
        // reflects reality.
        NEXTKEY_LOG(L"EnsureStartupRegistration: not elevated and scheduled task missing; falling back to registry startup, disabling admin mode");
        (void)SetRegistryStartup();
        runAsAdmin = false;
        return true;  // Config changed, caller should save
    }

    // Non-admin mode: ensure registry entry points to current EXE
    (void)SetRegistryStartup();
    return false;
}

/// Launch `exePath` (with optional `params`) via the shell's dispatch.
/// Because explorer.exe runs at medium integrity, `ShellExecute` through its
/// dispatch produces a non-elevated child even when the caller is elevated.
/// Used to de-elevate when the user turns OFF "Run as Admin" while the app is
/// currently running with admin rights.
///
/// Returns false on any failure (session 0 with no shell, explorer killed,
/// COM failure, etc.). Initializes COM on the calling thread if needed.
///
/// Reference: Raymond Chen, "How can I launch an unelevated process from my
/// elevated process?" (https://devblogs.microsoft.com/oldnewthing/20131118-00/).
[[nodiscard]] inline bool LaunchViaShellUnelevated(const wchar_t* exePath,
                                                    const wchar_t* params) noexcept {
    // RAII: COM uninit happens AFTER all COM unique_ptrs destruct (declared below,
    // so they destruct first). Pairs S_OK/S_FALSE with CoUninitialize.
    struct ComGuard {
        bool owned;
        ComGuard() noexcept {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            owned = (hr == S_OK || hr == S_FALSE);
        }
        ~ComGuard() noexcept { if (owned) CoUninitialize(); }
        ComGuard(const ComGuard&) = delete;
        ComGuard& operator=(const ComGuard&) = delete;
    } comGuard;

    auto release = [](IUnknown* p) noexcept { if (p) p->Release(); };

    IShellWindows* shellWindowsRaw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                  IID_IShellWindows,
                                  reinterpret_cast<void**>(&shellWindowsRaw))) ||
        !shellWindowsRaw) {
        return false;
    }
    std::unique_ptr<IShellWindows, decltype(release)> shellWindows(shellWindowsRaw, release);

    VARIANT vLoc = {};   vLoc.vt = VT_I4;   vLoc.lVal = CSIDL_DESKTOP;
    VARIANT vEmpty = {};
    long hwnd = 0;
    IDispatch* dispRaw = nullptr;
    if (FAILED(shellWindows->FindWindowSW(&vLoc, &vEmpty, SWC_DESKTOP, &hwnd,
                                            SWFO_NEEDDISPATCH, &dispRaw)) ||
        !dispRaw) {
        return false;
    }
    std::unique_ptr<IDispatch, decltype(release)> disp(dispRaw, release);

    IServiceProvider* providerRaw = nullptr;
    if (FAILED(disp->QueryInterface(IID_IServiceProvider,
                                      reinterpret_cast<void**>(&providerRaw))) ||
        !providerRaw) {
        return false;
    }
    std::unique_ptr<IServiceProvider, decltype(release)> provider(providerRaw, release);

    IShellBrowser* browserRaw = nullptr;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                        reinterpret_cast<void**>(&browserRaw))) ||
        !browserRaw) {
        return false;
    }
    std::unique_ptr<IShellBrowser, decltype(release)> browser(browserRaw, release);

    IShellView* viewRaw = nullptr;
    if (FAILED(browser->QueryActiveShellView(&viewRaw)) || !viewRaw) {
        return false;
    }
    std::unique_ptr<IShellView, decltype(release)> view(viewRaw, release);

    IDispatch* viewDispRaw = nullptr;
    if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch,
                                     reinterpret_cast<void**>(&viewDispRaw))) ||
        !viewDispRaw) {
        return false;
    }
    std::unique_ptr<IDispatch, decltype(release)> viewDisp(viewDispRaw, release);

    IShellFolderViewDual* folderRaw = nullptr;
    if (FAILED(viewDisp->QueryInterface(IID_IShellFolderViewDual,
                                           reinterpret_cast<void**>(&folderRaw))) ||
        !folderRaw) {
        return false;
    }
    std::unique_ptr<IShellFolderViewDual, decltype(release)> folder(folderRaw, release);

    IDispatch* appRaw = nullptr;
    if (FAILED(folder->get_Application(&appRaw)) || !appRaw) {
        return false;
    }
    std::unique_ptr<IDispatch, decltype(release)> app(appRaw, release);

    // We only need IDispatch::Invoke — skip querying IShellDispatch2 entirely.
    // SDK versions disagree on whether IShellDispatch2 declares `ShellExecute`
    // as a direct C++ method (some ship only the IDispatch dual path). Going
    // through `Invoke` via GetIDsOfNames works on every SDK + every Windows.

    // IDispatch::Invoke receives arguments in REVERSE declaration order.
    // ShellExecute signature: (File, vArgs, vDir, vVerb, vShow).
    VARIANT invokeArgs[5] = {};
    invokeArgs[0].vt = VT_I4;   invokeArgs[0].lVal    = SW_SHOWNORMAL;     // vShow
    invokeArgs[1].vt = VT_BSTR; invokeArgs[1].bstrVal = SysAllocString(L"open");
    invokeArgs[2].vt = VT_BSTR; invokeArgs[2].bstrVal = SysAllocString(L"");
    invokeArgs[3].vt = VT_BSTR; invokeArgs[3].bstrVal = SysAllocString(params ? params : L"");
    invokeArgs[4].vt = VT_BSTR; invokeArgs[4].bstrVal = SysAllocString(exePath);

    DISPID dispid = 0;
    LPOLESTR methodName = const_cast<LPOLESTR>(L"ShellExecute");
    HRESULT hr = app->GetIDsOfNames(IID_NULL, &methodName, 1,
                                      LOCALE_USER_DEFAULT, &dispid);

    if (SUCCEEDED(hr)) {
        DISPPARAMS dp = {};
        dp.cArgs  = 5;
        dp.rgvarg = invokeArgs;
        hr = app->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                          DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
    }

    for (VARIANT& v : invokeArgs) VariantClear(&v);
    return SUCCEEDED(hr);
}

/// Self-elevate if config says runAsAdmin but process is not elevated.
/// Call early in WinMain (before mutex). Returns true if re-launching elevated
/// (caller should return 0 immediately). Returns false to continue normally.
/// If UAC is denied, sets runAsAdmin=false in config so we don't prompt again.
[[nodiscard]] inline bool SelfElevateIfNeeded(const std::wstring& configPath, bool runAsAdmin) {
    if (!runAsAdmin || IsRunningAsAdmin()) return false;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = ADMIN_RESTART_FLAG;
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        return true;  // Elevated instance launching — caller should exit
    }

    // UAC denied — disable admin mode so we don't prompt on every start
    SystemConfig sysConfig;
    auto loaded = ConfigManager::LoadSystemConfig(configPath);
    if (loaded) sysConfig = *loaded;
    sysConfig.runAsAdmin = false;
    (void)ConfigManager::SaveSystemConfig(configPath, sysConfig);
    return false;
}

/// Restart the app with a new admin mode. Handles both directions:
///   - Elevation     (runAsAdmin ON, not yet elevated)   → ShellExecute(runas) + UAC
///   - De-elevation  (runAsAdmin OFF, currently elevated) → shell-dispatch trick
/// Returns `AdminRestartResult` describing the outcome so the caller can
/// decide whether to exit, notify the user, or do nothing.
[[nodiscard]] inline AdminRestartResult RestartWithNewAdminMode() {
    auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
    const bool nowElevated = IsRunningAsAdmin();

    if (sysConfig.runAsAdmin == nowElevated) {
        return AdminRestartResult::NoRestartNeeded;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // The new instance uses this flag to wait on the single-instance mutex
    // for ownership transfer (instead of exiting) — closes the restart race.
    LPCWSTR params = ADMIN_RESTART_FLAG;

    if (sysConfig.runAsAdmin && !nowElevated) {
        // Need elevation — ShellExecute with runas (shows UAC)
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.lpParameters = params;
        sei.nShow = SW_SHOWNORMAL;

        if (ShellExecuteExW(&sei)) {
            return AdminRestartResult::Restarting;
        }
        // UAC denied — revert config so we don't retry on every startup
        sysConfig.runAsAdmin = false;
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), sysConfig);
        return AdminRestartResult::UacDenied;
    }

    // De-elevation: !runAsAdmin && nowElevated.
    // CreateProcessW / ShellExecute inherit our elevated token; route through
    // explorer.exe's dispatch so the child inherits explorer's medium-IL token.
    return LaunchViaShellUnelevated(exePath, params)
               ? AdminRestartResult::Restarting
               : AdminRestartResult::DeElevationFailed;
}

}  // namespace NextKey

#endif  // _WIN32
