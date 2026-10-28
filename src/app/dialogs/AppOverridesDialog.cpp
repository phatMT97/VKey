// VKey - App Overrides Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "AppOverridesDialog.h"
#include "helpers/AppHelpers.h"
#include "sciter-x-dom.hpp"

using namespace sciter::dom;

namespace NextKey {

AppOverridesDialog::AppOverridesDialog(HWND parent)
    : WindowPickerDialog({
        L"this://app/appoverrides/appoverrides.html",
        L"VKey - App Overrides",
        400, 460, parent, false, 36, 40, true
    }) {
    entries_ = ConfigManager::LoadAppOverrides(ConfigManager::GetConfigPath());
    populateList();
}

void AppOverridesDialog::persistAndSignal() {
    (void)ConfigManager::SaveAppOverrides(ConfigManager::GetConfigPath(), entries_);
    SignalConfigChange();
}

bool AppOverridesDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    if (params.cmd == BUTTON_CLICK) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");
        if (id == L"btn-close") {
            PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
            return true;
        }
    }

    if (params.cmd == VALUE_CHANGED) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"val-action") {
            sciter::value val = el.get_value();
            std::wstring action = val.is_string() ? val.get<std::wstring>() : L"";

            if (!action.empty()) {
                sciter::dom::element root = get_root();
                auto readInput = [&](const char* inputId) -> std::wstring {
                    sciter::dom::element inp = root.find_first(inputId);
                    if (!inp.is_valid()) return L"";
                    sciter::value v = inp.get_value();
                    return v.is_string() ? v.get<std::wstring>() : L"";
                };
                auto readInt = [&](const char* inputId, int def) -> int {
                    auto s = readInput(inputId);
                    if (s.empty()) return def;
                    try { return std::stoi(s); } catch (...) { return def; }
                };

                std::wstring appName = readInput("#val-app-name");

                if (action == L"add-app") {
                    if (!appName.empty()) {
                        int8_t enc = static_cast<int8_t>(readInt("#val-encoding-override", -1));
                        int8_t mth = static_cast<int8_t>(readInt("#val-input-method", -1));
                        int8_t snd = static_cast<int8_t>(readInt("#val-send-method", -1));
                        addEntry(appName, enc, mth, snd);
                    }
                } else if (action == L"delete-app") {
                    if (!appName.empty()) {
                        removeEntry(appName);
                    }
                } else if (action == L"get-running-apps") {
                    auto apps = getRunningApps();
                    sciter::value arr;
                    for (size_t i = 0; i < apps.size(); ++i) {
                        arr.set_item(static_cast<int>(i), sciter::value(apps[i].c_str()));
                    }
                    call_function("setRunningApps", arr);
                } else if (action == L"pick-window") {
                    startWindowPicking();
                }

                el.set_value(sciter::value(L""));
            }
            return true;
        }
    }

    return sciter::window::handle_event(he, params);
}

void AppOverridesDialog::onWindowPicked(const std::wstring& exeName) {
    // Set the picked exe name into the app-name input field
    sciter::dom::element root = get_root();
    sciter::dom::element input = root.find_first("#app-name");
    if (input.is_valid()) {
        input.set_value(sciter::value(exeName.c_str()));
    }
}

void AppOverridesDialog::populateList() {
    call_function("clearAppList");
    for (auto& [name, entry] : entries_) {
        call_function("addAppToList",
            sciter::value(name.c_str()),
            sciter::value(static_cast<int>(entry.inputMethod)),
            sciter::value(static_cast<int>(entry.encodingOverride)),
            sciter::value(static_cast<int>(entry.sendMethod)));
    }
    call_function("forceRefresh");
}

void AppOverridesDialog::addEntry(const std::wstring& name, int8_t encoding, int8_t inputMethod, int8_t sendMethod) {
    std::wstring lower = ToLowerAscii(name);
    AppOverrideEntry entry;
    entry.encodingOverride = encoding;
    entry.inputMethod = inputMethod;
    entry.sendMethod = sendMethod;

    bool isUpdate = (entries_.find(lower) != entries_.end());
    entries_[lower] = entry;

    if (isUpdate) {
        call_function("removeAppFromList", sciter::value(lower.c_str()));
    }
    call_function("addAppToList",
        sciter::value(lower.c_str()),
        sciter::value(static_cast<int>(inputMethod)),
        sciter::value(static_cast<int>(encoding)),
        sciter::value(static_cast<int>(sendMethod)));
    call_function("clearInput");
    call_function("forceRefresh", sciter::value(true));
    persistAndSignal();
}

void AppOverridesDialog::removeEntry(const std::wstring& name) {
    std::wstring lower = ToLowerAscii(name);
    auto it = entries_.find(lower);
    if (it != entries_.end()) {
        entries_.erase(it);
        call_function("removeAppFromList", sciter::value(lower.c_str()));
        persistAndSignal();
    }
}

}  // namespace NextKey
