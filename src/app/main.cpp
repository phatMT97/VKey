// VKey - Core Application Entry Point
// SPDX-License-Identifier: GPL-3.0-only

#include "system/TrayIcon.h"
#include "system/FloatingIcon.h"
#include "system/SubprocessHelper.h"
#include "system/SubprocessRunners.h"
#if defined(VKEY_USE_RUST_ENGINE)
#include "system/AdvancedEngineInstaller.h"
#endif
#include "system/UpdateChecker.h"
#include "system/UpdateInstaller.h"
#include "system/PendingDllApply.h"
#include "system/ToastPopup.h"
#include "core/Version.h"
#include "core/config/TypingConfig.h"
#include "core/config/ConfigManager.h"
#include "core/config/ConfigEvent.h"
#include "core/ipc/SharedState.h"
#include "core/Strings.h"
#include "core/Debug.h"
#include "core/CrashLog.h"
#include "core/Logger.h"
#include "browser_host/Registration.h"

#include "system/TsfRegistration.h"
#include "system/StartupHelper.h"
#include "helpers/AppHelpers.h"

#include "system/HotkeyManager.h"
#include "system/HotkeyWiring.h"
#include "system/WatchdogController.h"
#include "system/BrowserExtensionController.h"
#include "core/config/LexiconTransaction.h"
#include "core/config/LexiconValidation.h"
#include "core/config/SpellExclusionCanonicalizer.h"
#include "core/ipc/LexiconWireManager.h"
#include "core/ipc/SharedStateManager.h"
#ifdef VKEY_HOOK_ENGINE
#include "system/HookEngine.h"
#include "system/MainThreadWorker.h"
#include "system/QuickConvert.h"
#endif

#include <Windows.h>
#include <ole2.h>
#include <timeapi.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

// Common Controls v6 is declared in VKey.exe.manifest (DPI awareness + CC v6)

using namespace NextKey;

// Forward declarations
void SpawnSettingsSubprocess();

// Global state
static std::atomic<bool> g_running{true};
static TrayIcon g_trayIcon;
static FloatingIcon g_floatingIcon;
static HINSTANCE g_hInstance = nullptr;

static SharedStateManager g_sharedState;  // Shared memory for Settings subprocess IPC
static Wire::LexiconWireManager g_wireManager;  // Shared memory wire manager for lexicon
static HotkeyManager g_hotkeyManager;
static WatchdogController g_watchdog;  // Owns heartbeat + Task Scheduler entry + VKeyWatchdog.exe lifecycle
static BrowserExtensionController g_browserExtension;  // Owns browser native-messaging registration

static bool PublishCurrentLexiconToWire() {
    if (!g_wireManager.IsWritable()) return false;
    const std::wstring configPath = ConfigManager::GetConfigPath();
    LexiconSyncLock snapshotLock;
    if (!snapshotLock.IsLocked() || !LexiconRecovery::RecoverIfNeeded(configPath)) {
        NEXTKEY_LOG(L"PublishCurrentLexiconToWire: paired snapshot lock/recovery failed");
        return false;
    }
    auto diskConfig = ConfigManager::LoadFromFile(configPath);
    if (!diskConfig) return false;

    std::vector<std::wstring> dictWords;
    if (!LexiconReader::LoadUserDictionaryWordsLocked(configPath, dictWords)) {
        NEXTKEY_LOG(L"PublishCurrentLexiconToWire: dictionary read failed; retaining prior snapshot");
        return false;
    }

    uint64_t wireGen = ConfigManager::LoadWireGeneration(configPath);
    if (wireGen <= g_wireManager.GetWireGeneration()) {
        wireGen = g_wireManager.GetWireGeneration() + 1;
        if (!ConfigManager::SaveWireGeneration(configPath, wireGen)) {
            NEXTKEY_LOG(L"PublishCurrentLexiconToWire: failed to persist generation");
            return false;
        }
    }
    g_wireManager.SetWireGeneration(wireGen);

    std::string err;
    if (!g_wireManager.Publish(diskConfig->spellExclusions, dictWords, wireGen, diskConfig->spellSuggestEnabled, &err)) {
        NEXTKEY_LOG(L"PublishCurrentLexiconToWire failed: %hs", err.c_str());
        return false;
    } else {
        NEXTKEY_LOG(L"PublishCurrentLexiconToWire: published gen=%llu dictWords=%zu exclusions=%zu",
                    static_cast<unsigned long long>(wireGen), dictWords.size(), diskConfig->spellExclusions.size());
    }
    return true;
}

static void InitLexiconWireMapping() {
    if (g_wireManager.Create()) {
        LexiconWriter::SetWireManager(&g_wireManager);
        const std::wstring configPath = ConfigManager::GetConfigPath();
        LexiconSyncLock snapshotLock;
        if (!snapshotLock.IsLocked() || !LexiconRecovery::RecoverIfNeeded(configPath)) {
            NEXTKEY_LOG(L"InitLexiconWireMapping: paired snapshot lock/recovery failed");
            return;
        }
        uint64_t wireGen = ConfigManager::LoadWireGeneration(configPath);
        g_wireManager.SetWireGeneration(wireGen);

        std::vector<std::wstring> initialDict;
        if (!LexiconReader::LoadUserDictionaryWordsLocked(configPath, initialDict)) {
            NEXTKEY_LOG(L"InitLexiconWireMapping: dictionary read failed; wire remains unpublished");
            return;
        }
        std::string err;
        const auto diskConfig = ConfigManager::LoadFromFile(configPath);
        if (!diskConfig) {
            NEXTKEY_LOG(L"InitLexiconWireMapping: config read failed; wire remains unpublished");
            return;
        }
        if (!g_wireManager.Publish(diskConfig->spellExclusions, initialDict, wireGen, diskConfig->spellSuggestEnabled, &err)) {
            NEXTKEY_LOG(L"InitLexiconWireMapping: failed to publish initial wire snapshot: %hs", err.c_str());
        } else {
            NEXTKEY_LOG(L"InitLexiconWireMapping: published wire snapshot gen=%llu dictWords=%zu exclusions=%zu",
                        static_cast<unsigned long long>(wireGen), initialDict.size(), diskConfig->spellExclusions.size());
        }
    } else {
        NEXTKEY_LOG(L"InitLexiconWireMapping: failed to create wire manager");
    }
}

static void ShutdownLexiconWireMapping() noexcept {
    LexiconWriter::SetWireManager(nullptr);
    g_wireManager.Close();
    NEXTKEY_LOG(L"LexiconWireMapping closed");
}

#ifdef VKEY_HOOK_ENGINE
static HookEngine g_hookEngine;
static MainThreadWorker g_mainThreadWorker;  // Sprint 1 D9: drain config-change work off main thread
static std::unique_ptr<QuickConvert> g_quickConvert;
static HotkeyManager::SlotId g_toggleHotkeySlot = 0;
static HotkeyManager::SlotId g_convertHotkeySlot = 0;
#endif

// Forward declaration
void OnMenuCommand(TrayMenuId id);

// Settings window helpers — search by title (subprocess may or may not be open)
static HWND GetSettingsHwnd() noexcept {
    return FindWindowW(nullptr, L"VKey Settings");
}
static void NotifySettingsMode(bool vietnamese) noexcept {
    if (HWND h = GetSettingsHwnd()) {
        PostMessageW(h, WM_VKEY_MODE_CHANGED, vietnamese ? 1 : 0, 0);
    }
}

#ifndef VKEY_HOOK_ENGINE
// V/E icon sync: 250ms poll of SharedState flags (atomic read, no IPC)
static constexpr UINT_PTR TIMER_ID_ICON_POLL = 100;

static void CALLBACK IconPollTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    try {
        uint32_t flags = g_sharedState.ReadFlags();
        bool vietnamese = (flags & SharedFlags::VIETNAMESE_MODE) != 0;
        g_trayIcon.SetVietnameseMode(vietnamese);  // no-op if unchanged
        g_floatingIcon.SetVietnameseMode(vietnamese);  // no-op if unchanged
    } catch (const std::exception& e) {
        CrashLog(L"IconPollTimerProc", e.what());
    } catch (...) {
        CrashLog(L"IconPollTimerProc", "(non-std exception)");
    }
}
#endif

/// Ensure floating icon window exists (lazy-create on first use).
static void EnsureFloatingIconCreated() {
    if (g_floatingIcon.IsCreated()) return;
#ifdef VKEY_HOOK_ENGINE
    const bool vietnamese = g_hookEngine.IsVietnameseMode();
#else
    const bool vietnamese =
        (g_sharedState.ReadFlags() & SharedFlags::VIETNAMESE_MODE) != 0;
#endif
    (void)g_floatingIcon.Create(g_hInstance, vietnamese);
}

/// Initialize floating icon overlay + config callbacks.
/// Shared by both HookEngine and TSF modes.
static void InitFloatingIcon(HINSTANCE hInstance, const SystemConfig& sc) {
    bool startVietnamese = (sc.startupMode != 1);
    // Only create resources if actually showing — saves RAM when disabled
    if (sc.showFloatingIcon) {
        if (g_floatingIcon.Create(hInstance, startVietnamese)) {
            g_floatingIcon.SetPosition(sc.floatingIconX, sc.floatingIconY);
            g_floatingIcon.SetVisible(true);
        }
    }

    // No TOML save on drag — position lives in g_floatingIcon.posX_/posY_,
    // survives Destroy()/Create() cycles. Persisted to TOML at app exit only.

    // Re-read config when Settings changes icon/system config
    g_trayIcon.SetIconConfigChangedCallback([](WPARAM wParam) {
        auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
        if (sysConfig.showFloatingIcon) {
            EnsureFloatingIconCreated();
            if (wParam == 1) {
                g_floatingIcon.SetPosition(INT32_MIN, INT32_MIN);
            }
            g_floatingIcon.SetVisible(true);
        } else {
            g_floatingIcon.Destroy();
        }
    });
}

/// Save floating icon position to TOML for next launch, then destroy.
static void CleanupFloatingIcon() noexcept {
    if (g_floatingIcon.GetPosX() != INT32_MIN) {
        auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
        sysConfig.floatingIconX = g_floatingIcon.GetPosX();
        sysConfig.floatingIconY = g_floatingIcon.GetPosY();
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), sysConfig);
    }
    g_floatingIcon.Destroy();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    g_hInstance = hInstance;
    // Brand the log file before anything writes to it. Modern build = Sciter UI.
    ::NextKey::Logger::SetRoleTag(L"Modern");
    InstallCursorCrashHandler();  // Restore system cursors if we crash during window picking

    // Last-resort catch: if a C++ throw ever escapes all try/catch at thread boundaries
    // (shouldn't happen after issue #103 fix, but belt-and-suspenders), log before dying
    // so the next crash report shows WHAT escaped, not a silent std::terminate.
    std::set_terminate([]() noexcept {
        CrashLog(L"std::terminate", "uncaught exception reached std::terminate");
        std::abort();
    });

    // ═══════════════════════════════════════════════════════════
    // Command-line Router
    // ═══════════════════════════════════════════════════════════

    if (BrowserHost::IsNativeMessagingInvocation()) {
        return BrowserHost::RunTrustedNativeMessagingHost();
    }

    if (HasCmdlineFlag(lpCmdLine, WATCHDOG_TASK_FLAG)) {
        return WatchdogController::RunScheduledTask();
    }

    // TSF Unregistration (runs elevated, then exits)
    // NOTE: Must check --unregister-tsf BEFORE --register-tsf
    // because "--register-tsf" is a substring of "--unregister-tsf"
    if (lpCmdLine && wcsstr(lpCmdLine, L"--unregister-tsf") != nullptr) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = UnregisterTsf();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    // TSF Registration (runs elevated, then exits)
    if (lpCmdLine && wcsstr(lpCmdLine, L"--register-tsf") != nullptr) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = RegisterTsf();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    // Diagnostics mode (shows HKL/TSF/SharedState info in MessageBox)
    if (lpCmdLine && wcsstr(lpCmdLine, L"--diag") != nullptr) {
        RunDiagnostics();
        return 0;
    }

    // Self-update installer mode (MUST be before Sciter dialog routes — sciter.dll not loaded yet)
    if (lpCmdLine && wcsstr(lpCmdLine, L"--install-update") != nullptr) {
        const wchar_t* arg = wcsstr(lpCmdLine, L"--install-update");
        arg += wcslen(L"--install-update");  // Skip "--install-update"
        // Skip whitespace
        while (*arg == L' ' || *arg == L'\t') arg++;
        // Strip surrounding quotes if present
        std::wstring zipPath(arg);
        if (zipPath.size() >= 2 && zipPath.front() == L'"' && zipPath.back() == L'"') {
            zipPath = zipPath.substr(1, zipPath.size() - 2);
        }
        if (!zipPath.empty()) {
            // Validate that the zip is inside %TEMP% — reject arbitrary paths to prevent
            // local malware from replacing the update archive with a crafted DLL payload.
            // Normalize first to defeat path traversal (e.g. C:\Temp\..\evil.zip).
            wchar_t resolvedPath[MAX_PATH] = {};
            if (!GetFullPathNameW(zipPath.c_str(), MAX_PATH, resolvedPath, nullptr)) {
                NEXTKEY_LOG(L"[main] --install-update rejected: path resolution failed");
                return 1;
            }
            wchar_t tempDir[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tempDir);  // Guarantees trailing backslash
            int cmpLen = static_cast<int>(wcslen(tempDir));
            int resolvedLen = static_cast<int>(wcslen(resolvedPath));
            if (resolvedLen < cmpLen ||
                CompareStringOrdinal(resolvedPath, cmpLen, tempDir, cmpLen, TRUE) != CSTR_EQUAL) {
                NEXTKEY_LOG(L"[main] --install-update rejected: '%s' not inside TEMP", resolvedPath);
                return 1;
            }
            RunUpdateInstaller(resolvedPath);  // [[noreturn]]
        }
        return 1;  // Missing zip path
    }

    // Subprocess dialog routes
    if (lpCmdLine && wcsstr(lpCmdLine, L"--settings") != nullptr) {
        RunSettingsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--excludedapps") != nullptr) {
        RunExcludedAppsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--tsfapps") != nullptr) {
        RunTsfAppsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--macro") != nullptr) {
        RunMacroSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--convert") != nullptr) {
        RunConvertToolSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--about") != nullptr) {
        RunAboutSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--appoverrides") != nullptr) {
        RunAppOverridesSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && (wcsstr(lpCmdLine, L"--spellexclusions") != nullptr ||
                      wcsstr(lpCmdLine, L"--lexicon") != nullptr)) {
        RunSpellExclusionsSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--userdefined") != nullptr) {
        RunUserDefinedSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--hotkeys") != nullptr) {
        RunHotkeysSubprocess();  // [[noreturn]]
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--icon-settings") != nullptr) {
        RunIconSettingsSubprocess();  // [[noreturn]]
    }

    // ═══════════════════════════════════════════════════════════
    // Main Process
    // ═══════════════════════════════════════════════════════════

    const bool isAdminRestart = HasCmdlineFlag(lpCmdLine, ADMIN_RESTART_FLAG);

    // Self-elevate if "Run as Admin" is enabled but we're not elevated.
    // Must be before mutex — the elevated instance will acquire the mutex instead.
    {
        auto preConfig = ConfigManager::LoadSystemConfigOrDefault();
        if (SelfElevateIfNeeded(ConfigManager::GetConfigPath(), preConfig.runAsAdmin)) {
            return 0;  // Elevated instance launching, exit this one
        }
    }

    // Ensure only one background instance of VKey runs at a time.
    // We check this AFTER subprocess routing so settings/macro dialogs
    // can spawn freely, but a second background process cannot.
    // The mutex name is SHARED with the Classic/Lite build (main_lite.cpp) on
    // purpose: the Sciter and Classic editions are mutually exclusive — running
    // one blocks the other. Do NOT rename this without updating main_lite.cpp.
    // NOTE: Use default DACL (nullptr). MakeCreatorOnlySecurityAttributes() uses
    // CO (Creator Owner) SID which does NOT resolve for non-container objects like
    // mutexes — second instance gets ERROR_ACCESS_DENIED instead of ERROR_ALREADY_EXISTS.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\VKey_Main_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (isAdminRestart) {
            // Admin-restart path: old instance is in the middle of cleanup.
            // Wait for ownership transfer (either clean release or WAIT_ABANDONED
            // if old is killed). This closes the restart race: by the time we
            // hold the mutex, old has unhooked and released shared resources.
            DWORD r = WaitForSingleObject(hMutex, 10000);
            if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
                NEXTKEY_LOG(L"Admin-restart: timeout waiting for old instance mutex (r=%lu)", r);
                CloseHandle(hMutex);
                return 1;
            }
            NEXTKEY_LOG(L"Admin-restart: mutex ownership acquired (%s)",
                         r == WAIT_ABANDONED ? L"abandoned" : L"released");
        } else {
            // Another background instance is already running.
            // Surface the settings dialog of the existing instance to indicate the app is active.
            HWND existingTrayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
            if (existingTrayWnd) {
                // Hand our foreground privilege to the existing instance (and to
                // the settings subprocess it spawns) before asking it to show the
                // dialog — it is a background process and cannot take foreground
                // on its own. Focusing the dialog from here instead would race:
                // the posted message has not been handled yet, so the window
                // usually does not exist at this point.
                DWORD existingPid = 0;
                GetWindowThreadProcessId(existingTrayWnd, &existingPid);
                if (existingPid != 0) {
                    AllowSetForegroundWindow(existingPid);
                }
                PostMessageW(existingTrayWnd, WM_VKEY_SHOW_SETTINGS, 0, 0);
            }

            NEXTKEY_LOG(L"Another instance is already running. Exiting.");
            CloseHandle(hMutex);
            return 0;
        }
    }

    // Remove any HKCU CLSID override that malware may have planted to hijack TSF DLL loading
    CleanupHkcuClsidOverride();

    // Load config
    auto config = ConfigManager::LoadOrDefault();
    auto hotkeyConfig = ConfigManager::LoadHotkeyConfigOrDefault();

    // Initialize UI language from system config
    auto systemConfig = ConfigManager::LoadSystemConfigOrDefault();
    SetLanguage(static_cast<Language>(systemConfig.language));

#if defined(VKEY_USE_RUST_ENGINE)
    // Repair before HookEngine can cache a failed first load for this process.
    // Ready is the only state allowed to keep Advanced persisted. Decline,
    // manual install, cancellation, and install failures all return to Standard
    // so startup never repeats the prompt without another explicit selection.
    if (config.GetSpellCheckLevel() == SpellCheckLevel::Advanced) {
        // ShowManualInstallWithUi reaches ShellExecuteW, which needs an
        // initialized apartment. The tray path only calls CoInitializeEx inside
        // the TSF branch below, so hook builds would otherwise have none.
        (void)::OleInitialize(nullptr);
        const AdvancedEngineStatus status = AdvancedEngineInstaller::EnsureInstalledWithUi(nullptr);
        if (status != AdvancedEngineStatus::Ready) {
            config.SetSpellCheckLevel(SpellCheckLevel::Standard);
            (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);
            if (status == AdvancedEngineStatus::ManualRequested) {
                AdvancedEngineInstaller::ShowManualInstallWithUi(nullptr);
            }
        }
    }
#endif

    // Apply any deferred TSF DLL swap before the generic update-file cleanup
    // (which removes _old_version/ and would delete the parked copy if the
    // order were reversed). Only runs in the main process; subprocess routes
    // returned above. Result published into SharedState flags after Create().
    PendingDllState pendingDllState = ApplyPendingDllUpdate();

    // Clean up leftover files from a previous update.
    // If files were cleaned up, it means we just finished an update.
    bool updateJustCompleted = CleanupOldUpdateFiles();
    if (updateJustCompleted) {
        NEXTKEY_LOG(L"Update completed successfully, old files cleaned up");
    }

    // Ensure startup registration is intact (never prompts UAC).
    // If admin task was lost, falls back to registry and syncs config.
    if (EnsureStartupRegistration(systemConfig.runAtStartup, systemConfig.runAsAdmin)) {
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), systemConfig);
        NEXTKEY_LOG(L"Startup task missing — fell back to registry, disabled admin mode in config");
    }

    // Watchdog is opt-in (default OFF). Init mirrors the config flag, and —
    // if previously enabled — launches VKeyWatchdog.exe and starts the
    // heartbeat thread (single-instance mutex inside watchdog dedups against
    // the logon-trigger task, so re-launch is safe).
    g_watchdog.Init(systemConfig);
    g_browserExtension.Init(systemConfig);

    // Check for update failure marker (installer failed and relaunched us)
    bool updateJustFailed = false;
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir(exePath);
        auto pos = exeDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) exeDir = exeDir.substr(0, pos);
        std::wstring markerPath = exeDir + L"\\_update_failed";
        if (DeleteFileW(markerPath.c_str())) {
            updateJustFailed = true;
            NEXTKEY_LOG(L"Update failed marker found and cleaned up");
        }
    }

#ifdef VKEY_HOOK_ENGINE
    // ═══════════════════════════════════════════════════════════
    // Hook Engine Mode — single process, no DLL/COM needed
    // ═══════════════════════════════════════════════════════════

    bool startVietnamese = (systemConfig.startupMode != 1);

    // Create SharedState for Settings subprocess IPC
    if (g_sharedState.Create()) {
        SharedState state;
        state.InitDefaults();
        state.inputMethod = static_cast<uint8_t>(config.inputMethod);
        state.spellCheck = static_cast<uint8_t>(config.GetSpellCheckLevel());
        state.optimizeLevel = config.optimizeLevel;
        state.codeTable = static_cast<uint8_t>(config.codeTable);
        state.SetFeatureFlags(EncodeFeatureFlags(config));
        // Phase 1 perf histogram: surface the hidden TOML toggle through the
        // SharedState diagFlags byte so the hook thread can react via
        // QuickSyncFromSharedState without a TOML re-parse.
        state.diagFlags = config.perfHistogramEnabled ? DiagFlags::PERF_HISTOGRAM : 0;
        if (!startVietnamese) {
            state.flags &= ~SharedFlags::VIETNAMESE_MODE;
        }
        state.SetHotkey(hotkeyConfig);
        g_sharedState.Write(state);
        NEXTKEY_LOG(L"SharedState created for HookEngine mode");

        // Publish the startup DLL-swap outcome ONLY if TSF is currently
        // registered — users who haven't opted into TSF don't care about
        // DLL-host synchronization and shouldn't be nagged to reboot.
        // Bits clear on reboot (SharedState is recreated fresh).
        const bool tsfInUse = IsTsfRegistered();
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_PENDING_DLL_SWAP,
            tsfInUse && pendingDllState == PendingDllState::SwapFailed);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_POST_UPDATE_REBOOT,
            tsfInUse && pendingDllState == PendingDllState::SwapDoneNeedsReboot);
        // TSF_ABI_MISMATCH is set cross-process by the DLL itself and only if
        // a host process loaded our DLL. DLL can't load without registration,
        // so this flag implicitly requires TSF-in-use. No gate needed.

        InitLexiconWireMapping();
    }

    // Tray Icon
    if (!g_trayIcon.Create(hInstance, startVietnamese)) {
        // MessageBox acceptable: fatal startup error, app cannot function without tray icon.
        // No matching StringId — using English string (language config not yet applied to UI).
        MessageBoxW(nullptr, L"Failed to create tray icon", L"VKey", MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }
    g_trayIcon.SetMenuCallback(OnMenuCommand);
    g_trayIcon.SetSharedState(&g_sharedState);  // for TSF-update restart menu item

    // Wire mode change callback: HookEngine → defer icon update via PostMessage.
    // sharedMode = logical V/E (→ SharedState, drives DLL + OnTickPoll sync);
    // displayMode = what the icon shows (TSF-icon override, issue #209).
    g_hookEngine.SetModeChangeCallback([](bool sharedMode, bool displayMode) {
        g_sharedState.SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, sharedMode);
        WPARAM wp = displayMode ? 1 : 0;
        HWND trayWnd = g_trayIcon.GetMessageWindow();
        if (trayWnd) {
            PostMessageW(trayWnd, WM_VKEY_TRAY_MODE_SYNC, wp, 0);
        }
        // Floating icon: direct update (same thread, no PostMessage needed)
        g_floatingIcon.SetVietnameseMode(displayMode);
        // Notify settings dialog directly (1 hop instead of 2 via TRAY_MODE_SYNC).
        // This keeps the toggle in sync with the tray icon even on rapid clicks.
        NotifySettingsMode(displayMode);
    });

    // Wire TSF mode callback: HookEngine → SharedState flags for DLL.
    // TSF_ACTIVE   = DLL consumes keys (foreground app in TSF list).
    // TSF_READONLY = DLL publishes HookContextAnchor for auto-cap (ordinary app).
    g_hookEngine.SetTsfModeCallback([](
            bool tsfActive, bool tsfReadonly, bool shouldActivateProfile) {
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_ACTIVE, tsfActive);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_READONLY, tsfReadonly);
        // Tray icon: show the colored "T" indicator while a TSF app is focused,
        // and revert to V/E when it isn't. Deferred to the tray message thread.
        if (HWND tsfTrayWnd = g_trayIcon.GetMessageWindow()) {
            PostMessageW(tsfTrayWnd, WM_VKEY_TRAY_TSF_SYNC, tsfActive ? 1 : 0, 0);
            // #209 (Shzr0 2026-07-04): re-assert TIP selection on focus INTO a
            // new TSF target. Startup-only activation left selection unrecoverable
            // when Windows/the user flipped the Input Indicator mid-session
            // (manual Win+Space, Edge losing selection after copying an image) —
            // VKey stayed on "US Keyboard" until restart. Re-asserting per focus
            // is safe now that ActivateVKeyTsfProfile() also runs
            // InstallLayoutOrTip (the TIP is always selectable — the pre-bb7e3ad
            // fragility is gone). Same-HWND poll classifications are filtered by
            // HookEngine before reaching this branch. Deliberately NOT gated on
            // V/E mode: the TIP passes
            // through in E mode, and per-app E/V lock must restore the TIP either
            // way. An in-place Input Indicator switch (no focus change) stands
            // until the next refocus — matches tester-expected semantics.
            if (tsfActive && shouldActivateProfile) {
                PostMessageW(tsfTrayWnd, WM_VKEY_ACTIVATE_TSF, 0, 0);
            }
        }
    });

    g_hookEngine.SetFocusAppContextCallback([](
            std::wstring_view exe, std::wstring_view rule,
            bool isTsf, bool isRust) {
        g_trayIcon.QueueAppContext(exe, rule, isTsf, isRust);
    });

    // #109: activate VKey's TIP at startup (standard-IME model, like
    // Unikey/Mozc). The handler runs ActivateVKeyTsfProfile() which (1) adds
    // VKey to the user's input list via InstallLayoutOrTip so it is selectable +
    // survives reboot, then (2) selects it for the session. Gated on
    // config.tsfApps AND TSF being registered — users who never enabled TSF
    // are not forced into it. Checking only IsTsfRegistered() (pre-#221 fix)
    // silently re-opted tsf_apps=false users into VKey as their OS input
    // method whenever registry remnants from an earlier enable/disable cycle
    // were still present (UnregisterTsf never removed the input-list entry —
    // now fixed below). The tsfModeCallback_ above additionally re-asserts
    // selection on each focus into a TSF app (#209 mid-session selection
    // loss); this startup call covers the window before any TSF-app focus
    // happens.
    if (config.tsfApps && IsTsfRegistered()) {
        if (HWND tsfTrayWnd = g_trayIcon.GetMessageWindow()) {
            PostMessageW(tsfTrayWnd, WM_VKEY_ACTIVATE_TSF, 0, 0);
        }
    } else if (!config.tsfApps && IsTsfRegistered()) {
        // #221: clean up a phantom input-list entry left by an earlier
        // enable/disable cycle (e.g. non-admin UnregisterTsf couldn't remove
        // the CLSID, or an older build auto-registered TSF) so Windows stops
        // offering VKey as a selectable input method the user didn't ask for.
        RemoveVKeyTsfFromInputList();
    }

    // Wire hook-reload callback: sub-dialog subprocess → main EXE eager sync.
    // Without this, new lists (TSF apps, excluded apps, macros, …) only apply on the next
    // keystroke / focus change in the target app. SyncConfigFromSharedState reads
    // configGeneration; it must not touch the Named Event (auto-reset, reserved for TSF DLL).
    //
    // Sprint 1 D9: route the actual work onto MainThreadWorker so SyncConfig
    // (which acquires stateMutex_ + may ReloadFromToml — file I/O) runs on
    // a dedicated thread, not the tray-window message thread. The worker
    // pre-empts the hook thread's QuickSync slow path: by the time the next
    // hook key arrives, lastConfigGeneration_ already matches and ProcessKeyDown
    // hits the no-op fast-path inside QuickSyncFromSharedState.
    g_trayIcon.SetHookReloadCallback([]() {
        g_mainThreadWorker.Signal();
    });
    g_trayIcon.SetLexiconCommittedCallback([]() {
        if (!PublishCurrentLexiconToWire()) return false;
        const auto committedConfig = ConfigManager::LoadFromFile(ConfigManager::GetConfigPath());
        if (!committedConfig) return false;
        SignalConfigChange(false, committedConfig->GetSpellCheckLevel());
        g_mainThreadWorker.Signal();
        return true;
    });

    // Wire settings dialog → HookEngine mode set (cross-process)
    g_trayIcon.SetModeRequestCallback([](bool vietnamese) {
        if (g_hookEngine.IsVietnameseMode() != vietnamese) {
            g_hookEngine.ToggleVietnameseMode();
        }
    });

    // Wire menu state getter — reads from SharedState + g_watchdog (no TOML)
    g_trayIcon.SetMenuStateGetter([]() -> TrayMenuState {
        SharedState state = g_sharedState.Read();
        uint32_t ff = state.GetFeatureFlags();
        return {
            g_hookEngine.IsVietnameseMode(),
            static_cast<SpellCheckLevel>(state.spellCheck),
            (ff & FeatureFlags::SMART_SWITCH) != 0,
            (ff & FeatureFlags::MACRO_ENABLED) != 0,
            state.inputMethod,
            static_cast<CodeTable>(state.codeTable),
            g_watchdog.IsEnabled(),
            g_browserExtension.IsEnabled()
        };
    });

    // Set 1ms timer resolution so Sleep(1) actually sleeps ~1ms instead of ~15ms.
    // Required for smooth Vietnamese input — backspace-then-retype needs a short gap
    // between SendInput calls for Electron/Console apps, but 15ms (default) is noticeable.
    timeBeginPeriod(1);

    // Share g_sharedState with HookEngine for direct reading (same process, no Open needed)
    g_hookEngine.SetSharedStateReader(&g_sharedState);

    // Attach the passive matcher before registering its startup-only slots.
    g_hookEngine.SetHotkeyManager(&g_hotkeyManager);

    // Wave 3 PR 3.6 — wire the worker-signal callback BEFORE HookEngine::Start.
    // Otherwise the LL hook thread (spawned inside Start) could read
    // `workerSignalFn_` from its QuickSync slow-path bail-out while main is
    // still mid-assign — std::function copy-assignment is NOT atomic, so a
    // concurrent read on hook = data race = UB. Pre-Start init means the
    // C++ thread-creation happens-before relation publishes the assigned
    // function to the new thread safely. MainThreadWorker::Signal is safe
    // before its own Start (latches, dispatches on first wake — see header
    // doc), so wiring it pre-everything is fine.
    g_hookEngine.SetWorkerSignalFn([]() { g_mainThreadWorker.Signal(); });

    // Register both application-hotkey slots before HookEngine seals the
    // passive matcher topology and installs the sole keyboard hook.
    WireHotkeys(g_hotkeyManager, g_hookEngine, g_trayIcon, g_sharedState, g_quickConvert,
                g_toggleHotkeySlot, g_convertHotkeySlot, hotkeyConfig);

    // Live toggle-hotkey propagation must also be wired before Start so the
    // hook thread never races std::function assignment during startup.
    g_hookEngine.SetHotkeyChangedCallback([](const HotkeyConfig& hk) {
        g_hotkeyManager.UpdateHotkey(g_toggleHotkeySlot, hk);
    });

    if (!g_hookEngine.Start(hInstance, config, startVietnamese)) {
        // MessageBox acceptable: fatal startup error, app cannot function without keyboard hook.
        // No matching StringId — using English string (language config not yet applied to UI).
        timeEndPeriod(1);
        MessageBoxW(nullptr, L"Failed to install keyboard hook", L"VKey", MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    // Sprint 1 D9: launch MainThreadWorker after the hook engine is up so the
    // first config-change Signal it sees has a fully-initialised HookEngine
    // to call into. Handler runs on the worker's own thread.
    //
    // Wave 3 PR 3.6 (worker-thread doctrine §12.4): workHandler also drains
    // any pending focus-classify request latched by WinEventProc. Wiring
    // both signal targets through the same handler keeps Signal()
    // coalescing intact — a burst of signals collapses into one wake that
    // drains both queues.
    g_mainThreadWorker.SetWorkHandler([]() {
        // Game-mode hotkey drains FIRST: the hook thread only latched a flag,
        // and the TOML read/modify/write happens here off the 1 ms hook budget.
        // Ordering matters — it bumps the config generation, so running it
        // after SyncConfigFromSharedState would leave the change unseen until
        // some later worker wake (seconds, once the idle backoff kicks in).
        g_hookEngine.DrainGameModeToggleOnWorker();
        g_hookEngine.SyncConfigFromSharedState();
        g_hookEngine.DrainClassifyOnWorker();
        PublishCurrentLexiconToWire();
        // Adaptive-tick (plan 2026-05-27): when MarkActivity wakes the worker
        // via Signal, this is where the cadence gets retuned back to active.
        // The Signal path doesn't run the tick handler — it runs this work
        // handler — so RetuneCadenceIfNeeded must be invoked here too.
        g_hookEngine.RetuneCadenceIfNeeded();
    });
    // (SetWorkerSignalFn already wired above, BEFORE HookEngine::Start —
    //  see Wave 3 PR 3.6 comment there for the std::function race rationale.)
    // Sprint 1 D10: 200 ms periodic tick — replaces the retired
    // SetTimer(nullptr, 0, 200, FocusPollTimerProc) inside HookEngine::Start.
    // Drives CJK layout poll + foreground-PID fallback off the worker thread.
    g_mainThreadWorker.SetTickHandler([]() {
        g_hookEngine.OnTickPoll();
    });
    // Adaptive-tick retune callback — invoked by HookEngine when the desired
    // cadence changes (active -> idle-short -> idle-long -> active). Updates
    // tickInterval_ inside MainThreadWorker; the next wait_for picks up the
    // new value at the next loop iteration.
    g_hookEngine.SetTickRetuneFn([](std::chrono::milliseconds ms) noexcept {
        g_mainThreadWorker.SetTickInterval(ms);
    });
    g_mainThreadWorker.SetTickInterval(std::chrono::milliseconds(NextKey::kTickActiveMs));
    g_mainThreadWorker.Start();

    NEXTKEY_LOG(L"HookEngine started, entering message loop");

    // Apply system config (icon style, show-on-startup)
    g_trayIcon.SetIconConfig(systemConfig.iconStyle, systemConfig.customColorV, systemConfig.customColorE,
                             systemConfig.showTsfIndicator);

    InitFloatingIcon(hInstance, systemConfig);

    if (systemConfig.showOnStartup) {
        SpawnSettingsSubprocess();
    }

    // Show post-update notification (after tray icon is ready)
    if (updateJustCompleted) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_SUCCESS), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::PostUpdateToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::PostUpdateToast::thread", "(non-std exception)");
            }
        }).detach();
    } else if (updateJustFailed) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_INSTALL_FAILED), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::UpdateFailedToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::UpdateFailedToast::thread", "(non-std exception)");
            }
        }).detach();
    }

    // Auto-check for updates on startup (background thread, 3s delay)
    if (systemConfig.autoCheckUpdate) {
        std::thread([]() {
            // URLDownloadToFileW (used by CheckForUpdate) requires COM init on the
            // calling thread — sibling threads in this file all call it; match them.
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                Sleep(30000);
                auto info = UpdateChecker::CheckForUpdate();
                if (info.available) {
                    HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
                    if (trayWnd) {
                        auto* pInfo = new (std::nothrow) UpdateInfo(std::move(info));
                        if (pInfo) {
                            // WndProc returns true (1) on success and takes ownership of pInfo.
                            // If window was destroyed, SendMessageW returns 0 — we still own pInfo.
                            if (!SendMessageW(trayWnd, WM_VKEY_UPDATE_AVAILABLE, 0,
                                              reinterpret_cast<LPARAM>(pInfo))) {
                                delete pInfo;
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                CrashLog(L"main::AutoUpdateCheck::thread", e.what());
            } catch (...) {
                CrashLog(L"main::AutoUpdateCheck::thread", "(non-std exception)");
            }
            CoUninitialize();
        }).detach();
    }

    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    TerminateAllSubprocesses();
    // Sprint 1 D9: stop the worker before HookEngine — handler captures
    // g_hookEngine, so the worker thread must finish any in-flight
    // SyncConfigFromSharedState before HookEngine teardown begins.
    g_mainThreadWorker.Stop();
    g_hookEngine.Stop();
    // The hook thread is joined before destroying either hotkey callback
    // target. No deferred slot can race tray/QuickConvert teardown.
    g_quickConvert.reset();
    CleanupFloatingIcon();
    g_trayIcon.Destroy();
    ShutdownLexiconWireMapping();
    timeEndPeriod(1);

#else
    // ═══════════════════════════════════════════════════════════
    // TSF Mode — SharedState IPC + DLL
    // ═══════════════════════════════════════════════════════════

    constexpr int kLegacyToggleHotkeyId = 1;
    bool legacyToggleHotkeyRegistered = false;

    // Initialize COM for TSF registration check
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Check and register TSF if needed
    if (!IsTsfRegistered()) {
        NEXTKEY_LOG(L"TSF not registered, attempting registration...");

        if (!RegisterTsf()) {
            // MessageBox acceptable: requires user consent before UAC elevation prompt.
            // No matching StringId — using English strings (first-run scenario, language not configured yet).
            int result = MessageBoxW(
                nullptr,
                L"VKey needs to register its input method.\n\n"
                L"This requires administrator privileges.\n"
                L"Click OK to continue with elevation.",
                L"VKey Setup",
                MB_OKCANCEL | MB_ICONINFORMATION
            );

            if (result == IDOK) {
                if (!RegisterTsfElevated()) {
                    MessageBoxW(nullptr,
                        L"Failed to register input method.\n"
                        L"Please run as administrator.",
                        L"VKey", MB_ICONERROR);
                }
            }
        }
    } else {
        NEXTKEY_LOG(L"TSF already registered");
    }

    // Initialize shared state for Engine IPC
    bool sharedStateOk = g_sharedState.Create();
    if (!sharedStateOk) {
        NEXTKEY_LOG(L"Failed to create shared memory, TSF will use TOML fallback");
    } else {
        SharedState state;
        state.InitDefaults();
        // TSF-only mode: DLL is the only engine, always active
        state.flags |= SharedFlags::TSF_ACTIVE;
        state.inputMethod = static_cast<uint8_t>(config.inputMethod);
        state.spellCheck = static_cast<uint8_t>(config.GetSpellCheckLevel());
        state.optimizeLevel = config.optimizeLevel;
        state.codeTable = static_cast<uint8_t>(config.codeTable);
        state.SetFeatureFlags(EncodeFeatureFlags(config));
        state.diagFlags = config.perfHistogramEnabled ? DiagFlags::PERF_HISTOGRAM : 0;
        g_sharedState.Write(state);
        NEXTKEY_LOG(L"SharedState created and initialized (TSF_ACTIVE=1, TSF-only mode)");

        // Publish startup DLL-swap outcome. We're in the TSF-only mode branch,
        // so TSF is registered by construction (earlier `if (!IsTsfRegistered())`
        // path handled the register prompt) — no IsTsfRegistered() gate needed.
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_PENDING_DLL_SWAP,
            pendingDllState == PendingDllState::SwapFailed);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_POST_UPDATE_REBOOT,
            pendingDllState == PendingDllState::SwapDoneNeedsReboot);

        InitLexiconWireMapping();
    }

    // Tray Icon
    if (!g_trayIcon.Create(hInstance)) {
        // MessageBox acceptable: fatal startup error, app cannot function without tray icon.
        // No matching StringId — using English string (language config not yet applied to UI).
        MessageBoxW(nullptr, L"Failed to create tray icon", L"VKey", MB_ICONERROR);
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }
    g_trayIcon.SetMenuCallback(OnMenuCommand);
    g_trayIcon.SetSharedState(&g_sharedState);  // for TSF-update restart menu item
    g_trayIcon.SetHookReloadCallback([]() {
        static_cast<void>(PublishCurrentLexiconToWire());
    });
    g_trayIcon.SetLexiconCommittedCallback([]() {
        if (!PublishCurrentLexiconToWire()) return false;
        const auto committedConfig = ConfigManager::LoadFromFile(ConfigManager::GetConfigPath());
        if (!committedConfig) return false;
        SignalConfigChange(false, committedConfig->GetSpellCheckLevel());
        return true;
    });

    // Wire settings dialog → TSF mode set (cross-process)
    g_trayIcon.SetModeRequestCallback([](bool vietnamese) {
        g_sharedState.SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, vietnamese);
        g_trayIcon.SetVietnameseMode(vietnamese);
    });

    // Wire menu state getter — reads from SharedState + g_watchdog (no TOML)
    g_trayIcon.SetMenuStateGetter([]() -> TrayMenuState {
        SharedState state = g_sharedState.Read();
        uint32_t ff = state.GetFeatureFlags();
        return {
            (state.flags & SharedFlags::VIETNAMESE_MODE) != 0,
            static_cast<SpellCheckLevel>(state.spellCheck),
            (ff & FeatureFlags::SMART_SWITCH) != 0,
            (ff & FeatureFlags::MACRO_ENABLED) != 0,
            state.inputMethod,
            static_cast<CodeTable>(state.codeTable),
            g_watchdog.IsEnabled(),
            g_browserExtension.IsEnabled()
        };
    });

    // Poll SharedState flags every 250ms to sync icon V/E state
    SetTimer(g_trayIcon.GetMessageWindow(), TIMER_ID_ICON_POLL, 250, IconPollTimerProc);

    // Legacy TSF-only builds have no HookEngine hook to feed the passive
    // matcher. Keep this branch compileable with a narrowly scoped native
    // fallback; the normal application targets always define
    // VKEY_HOOK_ENGINE and use the sole HookLifecycle keyboard hook.
    auto hotkeyOpt = ConfigManager::LoadHotkeyConfig(ConfigManager::GetConfigPath());
    if (hotkeyOpt && hotkeyOpt->vk != 0) {
        HWND trayWnd = g_trayIcon.GetMessageWindow();
        UINT modifiers = MOD_NOREPEAT;
        if (hotkeyOpt->ctrl) modifiers |= MOD_CONTROL;
        if (hotkeyOpt->shift) modifiers |= MOD_SHIFT;
        if (hotkeyOpt->alt) modifiers |= MOD_ALT;
        if (hotkeyOpt->win) modifiers |= MOD_WIN;
        legacyToggleHotkeyRegistered = RegisterHotKey(
            trayWnd, kLegacyToggleHotkeyId, modifiers, hotkeyOpt->vk) != FALSE;
        NEXTKEY_LOG(L"Legacy toggle hotkey %ls (ctrl=%d, shift=%d, alt=%d, win=%d, vk=0x%02X)",
                    legacyToggleHotkeyRegistered ? L"registered" : L"failed",
                    hotkeyOpt->ctrl, hotkeyOpt->shift, hotkeyOpt->alt,
                    hotkeyOpt->win, hotkeyOpt->vk);
    } else {
        NEXTKEY_LOG(L"No legacy RegisterHotKey-compatible toggle configured");
    }

    NEXTKEY_LOG(L"Tray icon created, entering message loop");

    // Apply system config (icon style, show-on-startup)
    g_trayIcon.SetIconConfig(systemConfig.iconStyle, systemConfig.customColorV, systemConfig.customColorE,
                             systemConfig.showTsfIndicator);

    InitFloatingIcon(hInstance, systemConfig);

    if (systemConfig.showOnStartup) {
        SpawnSettingsSubprocess();
    }

    // Show post-update notification (after tray icon is ready)
    if (updateJustCompleted) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_SUCCESS), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::PostUpdateToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::PostUpdateToast::thread", "(non-std exception)");
            }
        }).detach();
    } else if (updateJustFailed) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_INSTALL_FAILED), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"main::UpdateFailedToast::thread", e.what());
            } catch (...) {
                CrashLog(L"main::UpdateFailedToast::thread", "(non-std exception)");
            }
        }).detach();
    }

    // Auto-check for updates on startup (background thread, 3s delay)
    if (systemConfig.autoCheckUpdate) {
        std::thread([]() {
            // URLDownloadToFileW (used by CheckForUpdate) requires COM init on the
            // calling thread — sibling threads in this file all call it; match them.
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                Sleep(30000);
                auto info = UpdateChecker::CheckForUpdate();
                if (info.available) {
                    HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
                    if (trayWnd) {
                        auto* pInfo = new (std::nothrow) UpdateInfo(std::move(info));
                        if (pInfo) {
                            // WndProc returns true (1) on success and takes ownership of pInfo.
                            // If window was destroyed, SendMessageW returns 0 — we still own pInfo.
                            if (!SendMessageW(trayWnd, WM_VKEY_UPDATE_AVAILABLE, 0,
                                              reinterpret_cast<LPARAM>(pInfo))) {
                                delete pInfo;
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                CrashLog(L"main::AutoUpdateCheck::thread", e.what());
            } catch (...) {
                CrashLog(L"main::AutoUpdateCheck::thread", "(non-std exception)");
            }
            CoUninitialize();
        }).detach();
    }

    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    CleanupFloatingIcon();
    KillTimer(g_trayIcon.GetMessageWindow(), TIMER_ID_ICON_POLL);
    if (legacyToggleHotkeyRegistered) {
        UnregisterHotKey(g_trayIcon.GetMessageWindow(), kLegacyToggleHotkeyId);
    }

    // Disable engine in SharedState so TSF stops processing
    if (sharedStateOk) {
        SharedState state = g_sharedState.Read();
        if (state.IsValid()) {
            state.flags &= ~SharedFlags::ENGINE_ENABLED;
            g_sharedState.Write(state);
            NEXTKEY_LOG(L"ENGINE_ENABLED cleared in SharedState");
        }
    }

    TerminateAllSubprocesses();
    g_trayIcon.Destroy();
    ShutdownLexiconWireMapping();
    CoUninitialize();
#endif

    // Catch graceful exits that didn't go through the tray-Exit branch (e.g.
    // WM_CLOSE from the updater handover) so the watchdog skips respawn.
    // Idempotent — safe even if SignalGracefulShutdown was already called.
    // Heartbeat thread is stopped by ~WatchdogController via ~HeartbeatPublisher.
    ShutdownLexiconWireMapping();
    g_watchdog.SignalGracefulShutdown();

    NEXTKEY_LOG(L"Exiting");
    CloseHandle(hMutex);
    return 0;
}

/// Save config to TOML + sync SharedState + signal ConfigEvent.
/// Used by tray menu toggle handlers to propagate changes.
static void ApplyConfigChange(const TypingConfig& config) {
    // 1. Save to TOML
    (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);

    // 2. Sync to SharedState + bump configGeneration
    SharedStateManager sm;
    if (sm.OpenReadWrite()) {
        SharedState state = sm.Read();
        if (state.IsValid()) {
            state.inputMethod = static_cast<uint8_t>(config.inputMethod);
            state.spellCheck = static_cast<uint8_t>(config.GetSpellCheckLevel());
            state.codeTable = static_cast<uint8_t>(config.codeTable);
            state.SetFeatureFlags(EncodeFeatureFlags(config));
            state.diagFlags = config.perfHistogramEnabled ? DiagFlags::PERF_HISTOGRAM : 0;
            state.configGeneration++;  // HookEngine detects on next keystroke
            sm.Write(state);
        }
    }

    // 3. Signal ConfigEvent for TSF DLL (still uses Named Event)
    ConfigEvent event;
    if (event.Initialize()) {
        event.Signal();
    }

    // 4. Notify Settings dialog (if open) to refresh UI
    if (HWND settingsWnd = GetSettingsHwnd()) {
        PostMessageW(settingsWnd, WM_VKEY_CONFIG_CHANGED, 0, 0);
    }

    // 5. Update lexicon shared memory wire mapping
    PublishCurrentLexiconToWire();
}

/// Applies a spell-check level chosen from the tray menu. Entering Advanced
/// installs the trusted Rust engine if needed; either crossing of the Advanced
/// boundary needs a restart to load or release it (Yes/No prompt). The level
/// change itself always applies — declining the restart just defers loading the
/// engine into hosts that are already running (VKey itself picks it up live).
static void ApplySpellCheckLevel(SpellCheckLevel newLevel) {
    auto config = ConfigManager::LoadOrDefault();
    SpellCheckLevel oldLevel = config.GetSpellCheckLevel();
    const bool enteringAdvanced =
        newLevel == SpellCheckLevel::Advanced && oldLevel != SpellCheckLevel::Advanced;

#if defined(VKEY_USE_RUST_ENGINE)
    if (enteringAdvanced) {
        const HWND parent = g_trayIcon.GetMessageWindow();
        const AdvancedEngineStatus status = AdvancedEngineInstaller::EnsureInstalledWithUi(parent);
        if (status != AdvancedEngineStatus::Ready) {
            // oldLevel, not Standard: the guard above allows Off here, and a
            // declined download is not a request to switch spell check on.
            config.SetSpellCheckLevel(oldLevel);
            ApplyConfigChange(config);
            if (status == AdvancedEngineStatus::ManualRequested) {
                AdvancedEngineInstaller::ShowManualInstallWithUi(parent);
            }
            return;
        }
    }
#endif
    if (enteringAdvanced ||
        (oldLevel == SpellCheckLevel::Advanced && newLevel != SpellCheckLevel::Advanced)) {
        int result = MessageBoxW(g_trayIcon.GetMessageWindow(),
            S(enteringAdvanced ? StringId::SPELL_ADVANCED_LOAD_APP : StringId::SPELL_ADVANCED_CLOSE_APP),
            L"VKey", MB_YESNO | MB_ICONQUESTION);
        if (result == IDYES) {
            config.SetSpellCheckLevel(newLevel);
            ApplyConfigChange(config);
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            wchar_t cmdLine[MAX_PATH + 64] = {};
            swprintf_s(cmdLine, L"\"%s\" %s", exePath, ADMIN_RESTART_FLAG);
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE,
                               CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            PostMessageW(g_trayIcon.GetMessageWindow(), WM_CLOSE, 0, 0);
            return;
        }
    }

    config.SetSpellCheckLevel(newLevel);
    ApplyConfigChange(config);
}

void OnMenuCommand(TrayMenuId id) {
    switch (id) {
        case TrayMenuId::Settings:
            SpawnSettingsSubprocess();
            break;

        case TrayMenuId::About:
            SpawnSubprocess(L"VKey - About", L"--about");
            break;

        case TrayMenuId::ToggleMode:
#ifdef VKEY_HOOK_ENGINE
            g_hookEngine.ToggleVietnameseMode();
#else
            g_sharedState.ToggleFlag(SharedFlags::VIETNAMESE_MODE);
            g_trayIcon.SetVietnameseMode(
                (g_sharedState.ReadFlags() & SharedFlags::VIETNAMESE_MODE) != 0);
#endif
            break;

        case TrayMenuId::SpellCheckOff:
            ApplySpellCheckLevel(SpellCheckLevel::Off);
            break;

        case TrayMenuId::SpellCheckStandard:
            ApplySpellCheckLevel(SpellCheckLevel::Standard);
            break;

        case TrayMenuId::SpellCheckAdvanced:
            ApplySpellCheckLevel(SpellCheckLevel::Advanced);
            break;

        case TrayMenuId::SmartSwitch: {
            auto config = ConfigManager::LoadOrDefault();
            config.smartSwitch = !config.smartSwitch;
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::MacroEnabled: {
            auto config = ConfigManager::LoadOrDefault();
            config.macroEnabled = !config.macroEnabled;
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::MacroTable:
            SpawnSubprocess(L"VKey - Macro Table", L"--macro");
            break;

        case TrayMenuId::ConvertTool:
            SpawnSubprocess(L"VKey - Convert Tool", L"--convert");
            break;

#ifdef VKEY_HOOK_ENGINE
        case TrayMenuId::QuickConvert:
            if (g_quickConvert) {
                g_quickConvert->Execute();
            }
            break;
#endif

        case TrayMenuId::InputTelex:
        case TrayMenuId::InputVNI:
        case TrayMenuId::InputSimpleTelex:
        case TrayMenuId::InputCombined:
        case TrayMenuId::InputUserDefined: {
            auto config = ConfigManager::LoadOrDefault();
            int method = static_cast<int>(id) - static_cast<int>(TrayMenuId::InputTelex);
            config.inputMethod = static_cast<InputMethod>(method);
            ApplyConfigChange(config);
            break;
        }

        case TrayMenuId::Exit:
            ShutdownLexiconWireMapping();
            TerminateAllSubprocesses();
            // Tell watchdog this is a user-initiated quit — skip respawn.
            g_watchdog.SignalGracefulShutdown();
            g_running.store(false, std::memory_order_relaxed);
            PostQuitMessage(0);
            break;

        case TrayMenuId::RestartWindows:
            RestartWindowsWithPrompt(g_trayIcon.GetMessageWindow());
            break;

        case TrayMenuId::ExtensionWatchdog:
        case TrayMenuId::ToggleWatchdog:
            g_watchdog.Toggle(g_trayIcon.GetMessageWindow());
            break;

        case TrayMenuId::ExtensionBrowser:
            g_browserExtension.Toggle(g_trayIcon.GetMessageWindow());
            break;

        default: {
            // Code table menu items (1010-1014)
            auto rawId = static_cast<UINT>(id);
            if (rawId >= static_cast<UINT>(TrayMenuId::CodeTableUnicode) &&
                rawId <= static_cast<UINT>(TrayMenuId::CodeTableCP1258)) {
                auto ct = static_cast<CodeTable>(rawId - static_cast<UINT>(TrayMenuId::CodeTableUnicode));
#ifdef VKEY_HOOK_ENGINE
                g_hookEngine.SetCodeTable(ct);
#endif
                // Update SharedState immediately (source of truth at runtime)
                {
                    SharedState state = g_sharedState.Read();
                    if (state.IsValid()) {
                        state.codeTable = static_cast<uint8_t>(ct);
                        g_sharedState.Write(state);
                    }
                }

                // Persist to TOML for next startup
                auto config = ConfigManager::LoadOrDefault();
                config.codeTable = ct;
                (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);

                // Notify Settings dialog to refresh UI
                if (HWND settingsWnd = GetSettingsHwnd()) {
                    PostMessageW(settingsWnd, WM_VKEY_CONFIG_CHANGED, 0, 0);
                }
                NEXTKEY_LOG(L"Code table changed via tray menu: %d", static_cast<int>(ct));
            }
            break;
        }
    }
}

void SpawnSettingsSubprocess() {
    // Check if already open (single-instance)
    HWND existing = GetSettingsHwnd();
    if (existing) {
        FocusExistingWindow(existing);
#ifdef VKEY_HOOK_ENGINE
        NotifySettingsMode(g_hookEngine.IsVietnameseMode());
#endif
        return;
    }

    // Get exe path
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Build command line (pass current V/E mode to subprocess)
    wchar_t cmdLine[MAX_PATH + 64];
#ifdef VKEY_HOOK_ENGINE
    int mode = g_hookEngine.IsVietnameseMode() ? 1 : 0;
#else
    int mode = (g_sharedState.ReadFlags() & SharedFlags::VIETNAMESE_MODE) ? 1 : 0;
#endif
    swprintf_s(cmdLine, L"\"%s\" --settings --mode %d", exePath, mode);

    // Spawn subprocess
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    NEXTKEY_LOG(L"Spawning settings with cmdLine: %s", cmdLine);

    if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        TrackChildProcess(pi.hProcess);
        NEXTKEY_LOG(L"Settings subprocess spawned successfully");
    } else {
        NEXTKEY_LOG(L"Failed to spawn settings subprocess (error=%lu, path=%s)", GetLastError(), exePath);
    }
}
