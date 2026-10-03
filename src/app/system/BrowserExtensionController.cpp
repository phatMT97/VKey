// VKey - Browser Extension Controller implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "system/BrowserExtensionController.h"

#ifdef _WIN32

#include "browser_host/Registration.h"
#include "core/Debug.h"
#include "core/Strings.h"
#include "core/SystemConfig.h"
#include "core/config/ConfigManager.h"
#include "system/ExtensionInstaller.h"

namespace NextKey {

namespace {
constexpr const wchar_t* kAppTitle = L"VKey";
}  // namespace

void BrowserExtensionController::Init(const SystemConfig& systemConfig) {
    if (!systemConfig.browserExtensionEnabled) {
        (void)BrowserHost::UnregisterNativeMessagingHost();
        enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    if (BrowserHost::RegisterNativeMessagingHost()) {
        enabled_.store(true, std::memory_order_relaxed);
    } else {
        (void)BrowserHost::UnregisterNativeMessagingHost();
        NEXTKEY_LOG(L"BrowserExtensionController::Init: registration failed or host missing");
        enabled_.store(false, std::memory_order_relaxed);
    }
}

void BrowserExtensionController::Toggle(HWND notifyHwnd) {
    auto cfg = ConfigManager::LoadSystemConfigOrDefault();

    if (enabled_.load(std::memory_order_relaxed)) {
        (void)BrowserHost::UnregisterNativeMessagingHost();
        cfg.browserExtensionEnabled = false;
        (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), cfg);
        enabled_.store(false, std::memory_order_relaxed);

        MessageBoxW(notifyHwnd,
                    S(StringId::BROWSER_EXT_STOPPED_BODY),
                    kAppTitle, MB_OK | MB_ICONINFORMATION);
    } else {
        if (!ExtensionInstaller::EnsureInstalledWithUi(ExtensionType::BrowserHost, notifyHwnd)) {
            return;
        }

        if (BrowserHost::RegisterNativeMessagingHost()) {
            cfg.browserExtensionEnabled = true;
            (void)ConfigManager::SaveSystemConfig(ConfigManager::GetConfigPath(), cfg);
            enabled_.store(true, std::memory_order_relaxed);

            MessageBoxW(notifyHwnd,
                        S(StringId::BROWSER_EXT_ENABLED_BODY),
                        kAppTitle, MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(notifyHwnd,
                        L"Registration failed for browser native-messaging host.",
                        kAppTitle, MB_OK | MB_ICONERROR);
        }
    }
}

}  // namespace NextKey

#endif  // _WIN32
