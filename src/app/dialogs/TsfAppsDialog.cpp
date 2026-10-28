// VKey - TSF Apps Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "TsfAppsDialog.h"
#include "DialogUtils.h"
#include "core/config/ConfigManager.h"
#include "helpers/AppHelpers.h"
#include "core/Strings.h"
#include "core/WinStrings.h"
#include "core/PathUtil.h"
#include "sciter-x-dom.hpp"
#include <algorithm>
#include <fstream>
#include <vector>

using namespace sciter::dom;

namespace NextKey {

TsfAppsDialog::TsfAppsDialog(HWND parent)
    : WindowPickerDialog({
        L"this://app/tsfapps/tsfapps.html",
        L"VKey - TSF Apps",
        360, 420, parent, false, 36, 40, true
    }) {
    appList_ = ConfigManager::LoadTsfApps(ConfigManager::GetConfigPath());
    populateList();
}

void TsfAppsDialog::persistAndSignal() {
    (void)ConfigManager::SaveTsfApps(ConfigManager::GetConfigPath(), appList_);
    SignalConfigChange();
}

bool TsfAppsDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    // Handle BUTTON_CLICK for close button
    if (params.cmd == BUTTON_CLICK) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"btn-close") {
            PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
            return true;
        }
    }

    // Handle VALUE_CHANGED for #val-action (triggered by JS triggerAction)
    if (params.cmd == VALUE_CHANGED) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"val-action") {
            sciter::value val = el.get_value();
            std::wstring action = val.is_string() ? val.get<std::wstring>() : L"";
            if (!action.empty()) {
                // Read app name from hidden input
                sciter::dom::element root = get_root();
                sciter::dom::element nameInput = root.find_first("#val-app-name");
                std::wstring appName;
                if (nameInput.is_valid()) {
                    sciter::value nv = nameInput.get_value();
                    appName = nv.is_string() ? nv.get<std::wstring>() : L"";
                }

                if (action == L"add-manual") {
                    if (!appName.empty()) {
                        addApp(appName);
                    }
                } else if (action == L"add-current") {
                    startWindowPicking();
                } else if (action == L"add-browse") {
                    // Pick any exe — including system/UWP apps not in the running
                    // list (Windows Search, Task Manager, etc.). addApp() normalizes
                    // the path to its basename (#109 follow-up: add native apps).
                    std::wstring picked = ShowOpenFileDialogW(
                        get_hwnd(),
                        L"Ứng dụng (*.exe)\0*.exe\0Tất cả (*.*)\0*.*\0",
                        L"exe");
                    if (!picked.empty()) addApp(picked);
                } else if (action == L"delete") {
                    if (!appName.empty()) {
                        removeApp(appName);
                    }
                } else if (action == L"get-running-apps") {
                    auto apps = getRunningApps();
                    sciter::value arr;
                    for (size_t i = 0; i < apps.size(); ++i) {
                        arr.set_item(static_cast<int>(i), sciter::value(apps[i].c_str()));
                    }
                    call_function("setRunningApps", arr);
                } else if (action == L"import") {
                    importApps();
                } else if (action == L"export") {
                    exportApps();
                } else if (action == L"close") {
                    PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
                }

                // Clear the action value to allow re-triggering
                el.set_value(sciter::value(L""));
            }
            return true;
        }
    }

    return sciter::window::handle_event(he, params);
}

void TsfAppsDialog::onWindowPicked(const std::wstring& exeName) {
    if (exeName == L"vkey.exe") {
        MessageBoxW(get_hwnd(), L"Không thể thêm VKey vào danh sách.",
                    L"VKey", MB_OK | MB_ICONWARNING);
    } else {
        addApp(exeName);
    }
}

void TsfAppsDialog::populateList() {
    call_function("clearAppList");
    for (auto& app : appList_) {
        call_function("addAppToList", sciter::value(app.c_str()));
    }
    call_function("forceRefresh");
}

void TsfAppsDialog::addApp(const std::wstring& name) {
    // Match key is the exe basename (the classifier compares cls->exeName). A
    // browsed/typed full path ("C:\\Windows\\System32\\Taskmgr.exe") must be
    // reduced to its basename or it would never match. No-op for a plain name.
    std::wstring lower = ToLowerAscii(PathBasename(name));

    // Never add VKey to its own TSF list — covers every path (manual, browse,
    // window-picker, import). The picker also shows a message; skip silently here
    // so a typed/browsed/imported VKey exe can't slip through. Match the Classic
    // dialogs: block all three VKey exe names (Sciter + Lite + Classic builds).
    if (lower == L"vkey.exe" || lower == L"vkeylite.exe" || lower == L"vkeyclassic.exe") return;

    // Check for duplicates
    if (std::find(appList_.begin(), appList_.end(), lower) != appList_.end()) return;

    appList_.push_back(lower);
    call_function("addAppToList", sciter::value(lower.c_str()));
    call_function("forceRefresh");
    persistAndSignal();
}

void TsfAppsDialog::removeApp(const std::wstring& name) {
    std::wstring lower = ToLowerAscii(name);

    auto it = std::find(appList_.begin(), appList_.end(), lower);
    if (it != appList_.end()) {
        appList_.erase(it);
        call_function("removeAppFromList", sciter::value(lower.c_str()));
        persistAndSignal();
    }
}

void TsfAppsDialog::importApps() {
    std::wstring path = ShowOpenFileDialogW(
        get_hwnd(),
        L"Text file (*.txt)\0*.txt\0All (*.*)\0*.*\0",
        L"txt"
    );
    if (path.empty()) return;

    int msgboxID = MessageBoxW(
        get_hwnd(),
        L"B\u1EA1n c\u00F3 mu\u1ED1n gi\u1EEF l\u1EA1i danh s\u00E1ch hi\u1EC7n t\u1EA1i kh\u00F4ng?",
        L"Danh s\u00E1ch TSF",
        MB_ICONEXCLAMATION | MB_YESNO
    );

    bool append = (msgboxID == IDYES);

    std::ifstream infile(path);
    if (!infile.is_open()) return;

    if (!append) {
        appList_.clear();
    }

    ParseConfigLines(infile, [&](const std::string& line) {
        std::wstring wName = ToLowerAscii(Utf8ToWide(line));
        if (wName.empty()) return;
        if (std::find(appList_.begin(), appList_.end(), wName) == appList_.end()) {
            appList_.push_back(wName);
        }
    });

    populateList();
    persistAndSignal();
}

void TsfAppsDialog::exportApps() {
    std::wstring path = ShowSaveFileDialogW(
        get_hwnd(),
        L"Text file (*.txt)\0*.txt\0",
        L"txt",
        L"VKeyTsfApps"
    );
    if (path.empty()) return;

    std::ofstream outfile(path);
    if (!outfile.is_open()) return;

    outfile << ";VKey TSF Apps\n";

    std::vector<std::wstring> sorted = appList_;
    std::sort(sorted.begin(), sorted.end());
    for (auto& app : sorted) {
        outfile << WideToUtf8(app) << "\n";
    }
}

}  // namespace NextKey
