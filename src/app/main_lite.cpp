// VKey Classic — Lite build entry point
// SPDX-License-Identifier: GPL-3.0-only
//
// Simplified entry point for the Classic (Win32 native) UI build.
// No Sciter dependency. Uses HookEngine + TrayIcon + ClassicSettingsDialog.

#include "core/Version.h"
#include "core/WinStrings.h"
#include "core/config/ConfigManager.h"
#include "core/ipc/SharedState.h"
#include "core/ipc/SharedStateManager.h"
#include "core/ipc/SharedConstants.h"
#include "core/config/ConfigEvent.h"
#include "core/Strings.h"
#include "core/SystemConfig.h"
#include "core/Debug.h"
#include "core/CrashLog.h"
#include "core/Logger.h"
#include "browser_host/Registration.h"

#include "system/HookEngine.h"
#include "system/MainThreadWorker.h"
#include "system/HotkeyManager.h"
#include "system/HotkeyWiring.h"
#include "system/QuickConvert.h"
#include "system/TrayIcon.h"
#include "system/FloatingIcon.h"
#include "system/TsfRegistration.h"
#include "system/StartupHelper.h"
#if defined(VKEY_USE_RUST_ENGINE)
#include "system/AdvancedEngineInstaller.h"
#endif
#include "system/UpdateChecker.h"
#include "system/UpdateInstaller.h"
#include "system/PendingDllApply.h"
#include "system/ToastPopup.h"
#include "system/WatchdogController.h"
#include "system/BrowserExtensionController.h"
#include "helpers/AppHelpers.h"

#include "classic/ClassicSettingsDialog.h"
#include "classic/ClassicMacroTableDialog.h"
#include "classic/ClassicConvertToolDialog.h"
#include "core/config/LexiconTransaction.h"
#include "core/config/LexiconValidation.h"
#include "core/config/SpellExclusionCanonicalizer.h"
#include "core/ipc/LexiconWireManager.h"
#include "core/ipc/SharedStateManager.h"

#include <Windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <timeapi.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

// Common Controls v6 manifest is embedded via VKeyLite.rc + .exe.manifest
// (do NOT add #pragma manifestdependency here — causes duplicate MANIFEST resource)

using namespace NextKey;

// ═══════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};
static TrayIcon g_trayIcon;
static FloatingIcon g_floatingIcon;
static HotkeyManager g_hotkeyManager;
static HookEngine g_hookEngine;
static MainThreadWorker g_mainThreadWorker;  // Sprint 1 D9: drain config-change work off main thread
static SharedStateManager g_sharedState;
static Wire::LexiconWireManager g_wireManager;
static std::unique_ptr<QuickConvert> g_quickConvert;

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
static HotkeyManager::SlotId g_toggleHotkeySlot = 0;
static HotkeyManager::SlotId g_convertHotkeySlot = 0;
static WatchdogController g_watchdog;  // Owns heartbeat + Task Scheduler entry + VKeyWatchdog.exe lifecycle
static BrowserExtensionController g_browserExtension;  // Owns browser native-messaging registration
static HINSTANCE g_hInstance = nullptr;

// Forward declarations
static void SpawnSettingsDialog();
static void OnMenuCommand(TrayMenuId id);
static void EnsureFloatingIconCreated();
static void InitFloatingIcon(HINSTANCE hInstance, const SystemConfig& sc);
static void CleanupFloatingIcon() noexcept;

// ═══════════════════════════════════════════════════════════
// Settings Dialog — in-process (no subprocess)
// ═══════════════════════════════════════════════════════════

/// ClassicSettingsDialog state — one instance at a time.
/// Dialog runs its own modal message loop inside Show().
static bool g_settingsOpen = false;

static void SpawnSettingsDialog() {
    if (g_settingsOpen) {
        FocusExistingWindow(FindWindowW(L"VKeyClassicSettings", nullptr));
        return;
    }

    g_settingsOpen = true;

    // Run in a separate thread so the main message loop keeps running
    // (tray icon, hook engine callbacks, etc. must remain responsive).
    std::thread([]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        try {
            NextKey::Classic::ClassicSettingsDialog dialog;
            dialog.Show(g_hInstance);
            g_settingsOpen = false;

            // After dialog closes, reload config in case settings changed
            auto config = ConfigManager::LoadOrDefault();

            // Reload convert + toggle hotkeys (mirrors HotkeyWiring callback)
            {
                auto cc = ConfigManager::LoadConvertConfigOrDefault();
                if (g_quickConvert) g_quickConvert->UpdateConfig(cc);
                g_hotkeyManager.UpdateHotkey(g_convertHotkeySlot, cc.hotkey);
                g_trayIcon.RefreshConvertHotkeyCache(cc);

                auto hk = ConfigManager::LoadHotkeyConfigOrDefault();
                g_hotkeyManager.UpdateHotkey(g_toggleHotkeySlot, hk);
            }

            // Refresh floating icon config
            auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
            if (sysConfig.showFloatingIcon) {
                EnsureFloatingIconCreated();
                g_floatingIcon.SetVisible(true);
            } else {
                g_floatingIcon.Destroy();
            }

            // Refresh tray icon style
            g_trayIcon.SetIconConfig(sysConfig.iconStyle, sysConfig.customColorV, sysConfig.customColorE,
                                     sysConfig.showTsfIndicator);
        } catch (const std::exception& e) {
            CrashLog(L"SpawnSettingsDialog::thread", e.what());
            g_settingsOpen = false;
        } catch (...) {
            CrashLog(L"SpawnSettingsDialog::thread", "(non-std exception)");
            g_settingsOpen = false;
        }
        CoUninitialize();
    }).detach();
}

// ═══════════════════════════════════════════════════════════
// Floating Icon Helpers
// ═══════════════════════════════════════════════════════════

static void EnsureFloatingIconCreated() {
    if (g_floatingIcon.IsCreated()) return;
    (void)g_floatingIcon.Create(g_hInstance, g_hookEngine.IsVietnameseMode());
}

static void InitFloatingIcon(HINSTANCE hInstance, const SystemConfig& sc) {
    bool startVietnamese = (sc.startupMode != 1);
    if (sc.showFloatingIcon) {
        if (g_floatingIcon.Create(hInstance, startVietnamese)) {
            g_floatingIcon.SetPosition(sc.floatingIconX, sc.floatingIconY);
            g_floatingIcon.SetVisible(true);
        }
    }

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

static void CleanupFloatingIcon() noexcept {
    if (g_floatingIcon.GetPosX() != INT32_MIN) {
        auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
        sysConfig.floatingIconX = g_floatingIcon.GetPosX();
        sysConfig.floatingIconY = g_floatingIcon.GetPosY();
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), sysConfig);
    }
    g_floatingIcon.Destroy();
}

// ═══════════════════════════════════════════════════════════
// Config Change Helper
// ═══════════════════════════════════════════════════════════

/// Save config to TOML + sync SharedState + signal ConfigEvent.
/// Used by tray menu toggle handlers to propagate changes.
static void ApplyConfigChange(const TypingConfig& config) {
    (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);

    SharedState state = g_sharedState.Read();
    if (state.IsValid()) {
        state.inputMethod = static_cast<uint8_t>(config.inputMethod);
        state.spellCheck = static_cast<uint8_t>(config.GetSpellCheckLevel());
        state.codeTable = static_cast<uint8_t>(config.codeTable);
        state.SetFeatureFlags(EncodeFeatureFlags(config));
        state.configGeneration++;
        g_sharedState.Write(state);
    }

    ConfigEvent event;
    if (event.Initialize()) {
        event.Signal();
    }

    // Notify Classic settings dialog (if open) to refresh UI
    if (HWND settingsWnd = FindWindowW(L"VKeyClassicSettings", nullptr)) {
        PostMessageW(settingsWnd, WM_VKEY_CONFIG_CHANGED, 0, 0);
    }

    // Update lexicon shared memory wire mapping
    PublishCurrentLexiconToWire();
}

/// Applies a spell-check level chosen from the tray menu. Entering Advanced
/// installs the trusted Rust engine if needed; either crossing of the Advanced
/// boundary needs a restart to load or release it (Yes/No prompt). The level
/// change itself always applies — declining the restart just defers loading the
/// engine into hosts that are already running (VKey itself picks it up live).
static void ApplySpellCheckLevel(SpellCheckLevel newLevel) {
    auto config = ConfigManager::LoadOrDefault();
#if !defined(VKEY_USE_RUST_ENGINE)
    // Defense in depth: official Classic hides the Advanced menu item, but an
    // unexpected internal command must still never persist a Rust-only mode.
    if (newLevel == SpellCheckLevel::Advanced) {
        newLevel = SpellCheckLevel::Standard;
    }
#endif
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

// ═══════════════════════════════════════════════════════════
// Tray Menu Handler
// ═══════════════════════════════════════════════════════════

static void OnMenuCommand(TrayMenuId id) {
    switch (id) {
        case TrayMenuId::Settings:
            SpawnSettingsDialog();
            break;

        case TrayMenuId::About: {
            // Lite build: simple MessageBox about dialog
            std::wstring aboutText = L"VKey Classic v" VKEY_VERSION_WSTR L"\n"
                                     L"Vietnamese Input Method Editor\n"
                                     L"Build: " + GetBuildVersion(__DATE__, __TIME__) + L"\n\n"
                                     L"https://github.com/phatMT97/VKey\n\n";
#if defined(VKEY_USE_RUST_ENGINE)
            aboutText += L"Bản Classic + Rust dành riêng cho kiểm thử; không phải artifact phát hành và không được SignPath ký.";
#else
            aboutText += L"VKey Classic v4.3 được ký số miễn phí bởi SignPath.io, chứng chỉ bởi SignPath Foundation.";
#endif
            MessageBoxW(nullptr, aboutText.c_str(), L"VKey", MB_ICONINFORMATION);
            break;
        }

        case TrayMenuId::ToggleMode:
            g_hookEngine.ToggleVietnameseMode();
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
        case TrayMenuId::ConvertTool: {
            bool forceLight = ConfigManager::LoadSystemConfigOrDefault().forceLightTheme;
            if (id == TrayMenuId::MacroTable)
                Classic::ClassicMacroTableDialog::Show(g_hInstance, nullptr, forceLight);
            else
                Classic::ClassicConvertToolDialog::Show(g_hInstance, nullptr, forceLight);
            break;
        }

        case TrayMenuId::QuickConvert:
            if (g_quickConvert) {
                g_quickConvert->Execute();
            }
            break;

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
                g_hookEngine.SetCodeTable(ct);

                SharedState state = g_sharedState.Read();
                if (state.IsValid()) {
                    state.codeTable = static_cast<uint8_t>(ct);
                    g_sharedState.Write(state);
                }

                auto config = ConfigManager::LoadOrDefault();
                config.codeTable = ct;
                (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);
            }
            break;
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Entry Point
// ═══════════════════════════════════════════════════════════

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    g_hInstance = hInstance;
    // Brand the log file before anything writes to it. Classic build = Win32 UI.
    ::NextKey::Logger::SetRoleTag(L"Classic");
    InstallCursorCrashHandler();  // Restore system cursors if we crash during window picking

    // Last-resort catch: if a C++ throw ever escapes all try/catch at thread boundaries
    // (shouldn't happen after issue #103 fix, but belt-and-suspenders), log before dying
    // so the next crash report shows WHAT escaped, not a silent std::terminate.
    std::set_terminate([]() noexcept {
        CrashLog(L"std::terminate", "uncaught exception reached std::terminate");
        std::abort();
    });

    // ── Command-line routes (shared with main build) ──

    if (BrowserHost::IsNativeMessagingInvocation()) {
        return BrowserHost::RunTrustedNativeMessagingHost();
    }

    if (HasCmdlineFlag(lpCmdLine, WATCHDOG_TASK_FLAG)) {
        return WatchdogController::RunScheduledTask();
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--unregister-tsf") != nullptr) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = UnregisterTsf();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--register-tsf") != nullptr) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = RegisterTsf();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (lpCmdLine && wcsstr(lpCmdLine, L"--diag") != nullptr) {
        RunDiagnostics();
        return 0;
    }

    // Self-update installer mode
    if (lpCmdLine && wcsstr(lpCmdLine, L"--install-update") != nullptr) {
        const wchar_t* arg = wcsstr(lpCmdLine, L"--install-update");
        arg += wcslen(L"--install-update");
        while (*arg == L' ' || *arg == L'\t') arg++;
        std::wstring zipPath(arg);
        if (zipPath.size() >= 2 && zipPath.front() == L'"' && zipPath.back() == L'"') {
            zipPath = zipPath.substr(1, zipPath.size() - 2);
        }
        if (!zipPath.empty()) {
            wchar_t resolvedPath[MAX_PATH] = {};
            if (!GetFullPathNameW(zipPath.c_str(), MAX_PATH, resolvedPath, nullptr)) {
                return 1;
            }
            wchar_t tempDir[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tempDir);
            int cmpLen = static_cast<int>(wcslen(tempDir));
            int resolvedLen = static_cast<int>(wcslen(resolvedPath));
            if (resolvedLen < cmpLen ||
                CompareStringOrdinal(resolvedPath, cmpLen, tempDir, cmpLen, TRUE) != CSTR_EQUAL) {
                return 1;
            }
            RunUpdateInstaller(resolvedPath);
        }
        return 1;
    }

    const bool isAdminRestart = HasCmdlineFlag(lpCmdLine, ADMIN_RESTART_FLAG);

    // Self-elevate if "Run as Admin" is enabled but we're not elevated.
    {
        auto preConfig = ConfigManager::LoadSystemConfigOrDefault();
        if (SelfElevateIfNeeded(ConfigManager::GetConfigPath(), preConfig.runAsAdmin)) {
            return 0;
        }
    }

    // ── Single instance check ──

    // Shared mutex name with the Sciter build (main.cpp) so the Classic and
    // Sciter editions are mutually exclusive — only ONE VKey of EITHER edition
    // may run at a time (both register the same "VKeyTrayClass" tray window, so
    // the existing-instance popup below still targets whichever is running).
    // NOTE: Use default DACL (nullptr). CO SID doesn't resolve for non-container objects.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\VKey_Main_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (isAdminRestart) {
            // See main.cpp for rationale — wait for old instance to release.
            DWORD r = WaitForSingleObject(hMutex, 10000);
            if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
                NEXTKEY_LOG(L"Admin-restart: timeout waiting for old mutex (r=%lu)", r);
                CloseHandle(hMutex);
                return 1;
            }
            NEXTKEY_LOG(L"Admin-restart: mutex ownership acquired (%s)",
                         r == WAIT_ABANDONED ? L"abandoned" : L"released");
        } else {
            // Another VKey (Classic or Sciter) is already running. Surface the
            // running instance's settings, then exit silently — mirrors main.cpp.
            HWND existingTrayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
            if (existingTrayWnd) {
                DWORD existingPid = 0;
                GetWindowThreadProcessId(existingTrayWnd, &existingPid);
                if (existingPid != 0) {
                    AllowSetForegroundWindow(existingPid);
                }
                PostMessageW(existingTrayWnd, WM_VKEY_SHOW_SETTINGS, 0, 0);
            }
            NEXTKEY_LOG(L"Another VKey instance is already running. Exiting.");
            CloseHandle(hMutex);
            return 0;
        }
    }

    // ── Initialization ──

    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Remove any HKCU CLSID override that malware may have planted
    CleanupHkcuClsidOverride();

    // Load config
    auto config = ConfigManager::LoadOrDefault();
    auto hotkeyConfig = ConfigManager::LoadHotkeyConfigOrDefault();

    // Initialize UI language
    auto systemConfig = ConfigManager::LoadSystemConfigOrDefault();
    SetLanguage(static_cast<Language>(systemConfig.language));

#if defined(VKEY_USE_RUST_ENGINE)
    // Repair before HookEngine can cache a failed first load for this process.
    // Ready is the only state allowed to keep Advanced persisted. Decline,
    // manual install, cancellation, and install failures all return to Standard
    // so startup never repeats the prompt without another explicit selection.
    if (config.GetSpellCheckLevel() == SpellCheckLevel::Advanced) {
        const AdvancedEngineStatus status = AdvancedEngineInstaller::EnsureInstalledWithUi(nullptr);
        if (status != AdvancedEngineStatus::Ready) {
            config.SetSpellCheckLevel(SpellCheckLevel::Standard);
            (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);
            if (status == AdvancedEngineStatus::ManualRequested) {
                AdvancedEngineInstaller::ShowManualInstallWithUi(nullptr);
            }
        }
    }
#else
    // A preference may have been left by Sciter or a development build. The
    // official Classic product boundary is C++-only, so normalize it before
    // HookEngine can create an engine in this process.
    if (config.GetSpellCheckLevel() == SpellCheckLevel::Advanced) {
        config.SetSpellCheckLevel(SpellCheckLevel::Standard);
        (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);
    }
#endif

    // Apply any deferred TSF DLL swap before CleanupOldUpdateFiles removes
    // _old_version/ (the parking dir used by ApplyPendingDllUpdate).
    PendingDllState pendingDllState = ApplyPendingDllUpdate();

    // Clean up leftover update files
    bool updateJustCompleted = CleanupOldUpdateFiles();
    if (updateJustCompleted) {
        NEXTKEY_LOG(L"Update completed successfully, old files cleaned up");
    }

    // Ensure startup registration is intact (never prompts UAC, same logic as main.cpp)
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

    // Check for update failure marker
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

    // ── SharedState ──

    bool startVietnamese = (systemConfig.startupMode != 1);

    if (g_sharedState.Create()) {
        SharedState state;
        state.InitDefaults();
        state.flags |= SharedFlags::CLASSIC_MODE;
        state.inputMethod = static_cast<uint8_t>(config.inputMethod);
        state.spellCheck = static_cast<uint8_t>(config.GetSpellCheckLevel());
        state.optimizeLevel = config.optimizeLevel;
        state.codeTable = static_cast<uint8_t>(config.codeTable);
        state.SetFeatureFlags(EncodeFeatureFlags(config));
        state.SetHotkey(hotkeyConfig);
        if (!startVietnamese) {
            state.flags &= ~SharedFlags::VIETNAMESE_MODE;
        }
        g_sharedState.Write(state);
        NEXTKEY_LOG(L"SharedState created for Lite mode");

        // Publish the startup DLL-swap outcome ONLY if TSF is registered —
        // users who haven't opted into TSF (or toggled it off) don't care
        // about DLL-host sync and shouldn't be nagged to reboot.
        const bool tsfInUse = IsTsfRegistered();
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_PENDING_DLL_SWAP,
            tsfInUse && pendingDllState == PendingDllState::SwapFailed);
        g_sharedState.SetOrClearFlag(SharedFlags::TSF_POST_UPDATE_REBOOT,
            tsfInUse && pendingDllState == PendingDllState::SwapDoneNeedsReboot);

        InitLexiconWireMapping();
    }

    // ── Tray Icon ──

    if (!g_trayIcon.Create(hInstance, startVietnamese)) {
        MessageBoxW(nullptr, L"Failed to create tray icon", L"VKey", MB_ICONERROR);
        OleUninitialize();
        CloseHandle(hMutex);
        return 1;
    }
    g_trayIcon.SetMenuCallback(OnMenuCommand);
    g_trayIcon.SetSharedState(&g_sharedState);  // for TSF-update restart menu item

    // Wire mode change callback: HookEngine -> tray icon + SharedState + floating icon.
    // sharedMode = logical V/E (→ SharedState, drives DLL + OnTickPoll sync);
    // displayMode = what the icon shows (TSF-icon override, issue #209).
    g_hookEngine.SetModeChangeCallback([](bool sharedMode, bool displayMode) {
        g_sharedState.SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, sharedMode);
        HWND trayWnd = g_trayIcon.GetMessageWindow();
        if (trayWnd) {
            PostMessageW(trayWnd, WM_VKEY_TRAY_MODE_SYNC, displayMode ? 1 : 0, 0);
        }
        g_floatingIcon.SetVietnameseMode(displayMode);
    });

    // Wire TSF mode callback: HookEngine -> SharedState flags for DLL.
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
            // #209: re-assert TIP selection on focus into a new TSF target, NOT
            // gated on V/E — selection recovery matters in both modes (the TIP
            // passes through in E). Same-HWND poll classifications are filtered
            // by HookEngine. See main.cpp for the full rationale.
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

    // Wire settings dialog -> HookEngine mode set
    g_trayIcon.SetModeRequestCallback([](bool vietnamese) {
        if (g_hookEngine.IsVietnameseMode() != vietnamese) {
            g_hookEngine.ToggleVietnameseMode();
        }
    });

    // Wire hook-reload callback: sub-dialog subprocess → main EXE eager sync.
    // Without this, new lists (TSF apps, excluded apps, macros, …) only apply
    // on the next keystroke / focus change in the target app.
    //
    // Sprint 1 D9: route the work onto MainThreadWorker (see main.cpp for the
    // full rationale — keeps SyncConfig + potential ReloadFromToml off the
    // tray-window thread, pre-empts hook QuickSync slow path).
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

    // Wire menu state getter
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

    // ── Timer resolution ──

    // Set 1ms timer resolution for smooth Vietnamese input
    timeBeginPeriod(1);

    // ── HookEngine ──

    g_hookEngine.SetSharedStateReader(&g_sharedState);
    g_hookEngine.SetHotkeyManager(&g_hotkeyManager);

    // Wave 3 PR 3.6 — wire the worker-signal callback BEFORE HookEngine::Start.
    // The LL hook thread (spawned inside Start) reads `workerSignalFn_` from
    // QuickSync's hook-bail branch — std::function assignment is NOT atomic,
    // so pre-Start init is required to publish it via the thread-creation
    // happens-before relation. Mirror of main.cpp.
    g_hookEngine.SetWorkerSignalFn([]() { g_mainThreadWorker.Signal(); });

    // Register both application-hotkey slots before HookEngine seals the
    // passive matcher topology and installs the sole keyboard hook.
    WireHotkeys(g_hotkeyManager, g_hookEngine, g_trayIcon, g_sharedState, g_quickConvert,
                g_toggleHotkeySlot, g_convertHotkeySlot, hotkeyConfig);

    // Wire live rebinding before Start so the hook thread never races
    // std::function assignment during startup.
    g_hookEngine.SetHotkeyChangedCallback([](const HotkeyConfig& hk) {
        g_hotkeyManager.UpdateHotkey(g_toggleHotkeySlot, hk);
    });

    if (!g_hookEngine.Start(hInstance, config, startVietnamese)) {
        timeEndPeriod(1);
        MessageBoxW(nullptr, L"Failed to install keyboard hook", L"VKey", MB_ICONERROR);
        OleUninitialize();
        CloseHandle(hMutex);
        return 1;
    }

    // Sprint 1 D9: launch worker after HookEngine so the first Signal it
    // observes lands on a fully-initialised engine.
    //
    // Wave 3 PR 3.6 (worker-thread doctrine §12.4): workHandler also drains
    // pending focus-classify requests latched by WinEventProc. Mirror of
    // main.cpp wiring — Lite mode has the same race surface (same
    // HookEngine + FocusOwner), so the doctrine applies identically.
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
        // Adaptive-tick (plan 2026-05-27): retune cadence after Signal-driven
        // wake. Same wiring as main.cpp.
        g_hookEngine.RetuneCadenceIfNeeded();
    });
    // (SetWorkerSignalFn already wired above, BEFORE HookEngine::Start —
    //  see Wave 3 PR 3.6 comment there for the std::function race rationale.)
    // Sprint 1 D10: 200 ms tick replaces the retired SetTimer focus/CJK
    // poll that lived inside HookEngine::Start.
    g_mainThreadWorker.SetTickHandler([]() {
        g_hookEngine.OnTickPoll();
    });
    // Adaptive-tick retune callback — see main.cpp for full rationale.
    g_hookEngine.SetTickRetuneFn([](std::chrono::milliseconds ms) noexcept {
        g_mainThreadWorker.SetTickInterval(ms);
    });
    g_mainThreadWorker.SetTickInterval(std::chrono::milliseconds(NextKey::kTickActiveMs));
    g_mainThreadWorker.Start();

    NEXTKEY_LOG(L"HookEngine started (Lite mode), entering message loop");

    // ── Icon config + floating icon ──

    g_trayIcon.SetIconConfig(systemConfig.iconStyle, systemConfig.customColorV, systemConfig.customColorE,
                             systemConfig.showTsfIndicator);
    InitFloatingIcon(hInstance, systemConfig);

    // ── Show settings on startup if configured ──

    if (systemConfig.showOnStartup) {
        SpawnSettingsDialog();
    }

    // ── Post-update notifications ──

    if (updateJustCompleted) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_SUCCESS), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"PostUpdateToast::thread", e.what());
            } catch (...) {
                CrashLog(L"PostUpdateToast::thread", "(non-std exception)");
            }
        }).detach();
    } else if (updateJustFailed) {
        std::thread([]() {
            try {
                Sleep(1000);
                ToastPopup::Show(S(StringId::UPDATE_INSTALL_FAILED), 3000);
            } catch (const std::exception& e) {
                CrashLog(L"UpdateFailedToast::thread", e.what());
            } catch (...) {
                CrashLog(L"UpdateFailedToast::thread", "(non-std exception)");
            }
        }).detach();
    }

    // ── Auto-check for updates ──

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
                CrashLog(L"AutoUpdateCheck::thread", e.what());
            } catch (...) {
                CrashLog(L"AutoUpdateCheck::thread", "(non-std exception)");
            }
            CoUninitialize();
        }).detach();
    }

    // ── Message Loop ──

    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ── Cleanup ──

    // Catch graceful exits that didn't go through the tray-Exit branch (e.g.
    // WM_CLOSE from the updater handover) so the watchdog skips respawn.
    // Idempotent — safe even if SignalGracefulShutdown was already called.
    g_watchdog.SignalGracefulShutdown();
    // Sprint 1 D9: stop the worker before HookEngine — handler captures
    // g_hookEngine, so any in-flight SyncConfigFromSharedState must finish
    // before HookEngine teardown.
    g_mainThreadWorker.Stop();
    g_hookEngine.Stop();
    // The hook thread is joined before destroying either hotkey callback
    // target. No deferred slot can race tray/QuickConvert teardown.
    g_quickConvert.reset();
    CleanupFloatingIcon();
    timeEndPeriod(1);
    g_trayIcon.Destroy();
    ShutdownLexiconWireMapping();

    NEXTKEY_LOG(L"Exiting (Lite mode)");

    OleUninitialize();
    CloseHandle(hMutex);

    return 0;
}
