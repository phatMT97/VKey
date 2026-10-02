// VKey - Watchdog Controller implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "system/WatchdogController.h"

#ifdef _WIN32

#include "core/SystemConfig.h"
#include "core/Strings.h"
#include "core/Debug.h"
#include "core/config/ConfigManager.h"
#include "core/security/ExtensionTrust.h"
#include "system/StartupHelper.h"
#include "system/ExtensionInstaller.h"

#include <string>

namespace NextKey {

namespace {
constexpr const wchar_t* kAppTitle = L"VKey";
}  // namespace

void WatchdogController::Init(const SystemConfig& systemConfig) {
    enabled_.store(systemConfig.watchdogEnabled, std::memory_order_relaxed);

    if (!systemConfig.watchdogEnabled) {
        return;
    }

    // Watchdog has its own single-instance mutex — duplicate launch is a
    // no-op. Re-launch on every startup means the user doesn't have to log
    // out/in to recover the watchdog after enabling.
    LaunchWatchdogProcess();

    if (!heartbeat_.Start()) {
        NEXTKEY_LOG(L"HeartbeatPublisher start failed — watchdog auto-respawn disabled");
    }
}

void WatchdogController::Toggle(HWND notifyHwnd) {
    auto cfg = ConfigManager::LoadSystemConfigOrDefault();

    if (cfg.watchdogEnabled) {
        heartbeat_.SignalGracefulShutdown();
        KillWatchdogProcess();
        RemoveWatchdogScheduledTask();

        cfg.watchdogEnabled = false;
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), cfg);
        enabled_.store(false, std::memory_order_relaxed);

        // Heartbeat consumer is gone — release named events + thread.
        // Stop() joins within ≤100 ms (Sleep granularity in Publisher::Run).
        heartbeat_.Stop();

        MessageBoxW(notifyHwnd,
                    S(StringId::WATCHDOG_STOPPED_BODY),
                    kAppTitle, MB_OK | MB_ICONINFORMATION);
    } else {
        // Ensure VKeyWatchdog.exe is installed and verified
        if (!ExtensionInstaller::EnsureInstalledWithUi(ExtensionType::Watchdog, notifyHwnd)) {
            return;
        }

        // Task Scheduler register prompts UAC. If user denies, leave the
        // config flag false (menu still reads "Bật…") and bail.
        if (!CreateWatchdogScheduledTask()) {
            NEXTKEY_LOG(L"Watchdog enable: Task Scheduler register failed (UAC denied?)");
            return;
        }

        cfg.watchdogEnabled = true;
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), cfg);
        enabled_.store(true, std::memory_order_relaxed);

        if (!heartbeat_.Start()) {
            NEXTKEY_LOG(L"HeartbeatPublisher start failed on toggle ON — watchdog auto-respawn disabled");
        }

        LaunchWatchdogProcess();

        MessageBoxW(notifyHwnd,
                    S(StringId::WATCHDOG_ENABLED_BODY),
                    kAppTitle, MB_OK | MB_ICONINFORMATION);
    }
}

void WatchdogController::SignalGracefulShutdown() noexcept {
    heartbeat_.SignalGracefulShutdown();
}

void WatchdogController::LaunchWatchdogProcess() noexcept {
    std::wstring dir = GetInstallDirectory();
    if (dir.empty()) {
        return;
    }
    std::wstring wdPath = dir + L"\\VKeyWatchdog.exe";
    if (!Security::IsExtensionTrusted(wdPath)) {
        NEXTKEY_LOG(L"LaunchWatchdogProcess: untrusted or invalid binary at %ls — aborted",
                    wdPath.c_str());
        return;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(wdPath.c_str(), nullptr, nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void WatchdogController::KillWatchdogProcess() noexcept {
    // Same-user same-session — no UAC required for taskkill.
    // TODO(perf): WaitForSingleObject(3000) blocks the tray-menu thread —
    // up to 3 s UI freeze on toggle-off. Move to a worker thread when the
    // UI-thread budget on tray menus becomes a priority.
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"taskkill /F /IM VKeyWatchdog.exe";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

}  // namespace NextKey

#endif  // _WIN32
