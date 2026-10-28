// VKey - User Defined Input Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "UserDefinedDialog.h"
#include "DialogUtils.h"
#include "core/config/ConfigManager.h"
#include "core/WinStrings.h"
#include "helpers/AppHelpers.h"
#include "sciter-x-dom.hpp"

namespace NextKey {

UserDefinedDialog::UserDefinedDialog(HWND parent)
    : SciterSubDialog({
        L"this://app/userdefined/userdefined.html",
        L"VKey - User Defined Input",
        460, 550, parent, false, 36, 40, true
    }) {
    TypingConfig config = ConfigManager::LoadOrDefault();
    keyMap_ = config.customKeyMap;
    populateList();
}

void UserDefinedDialog::persistAndSignal() {
    auto config = ConfigManager::LoadOrDefault();
    config.customKeyMap = keyMap_;
    (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), config);
    SignalConfigChange();
}

void UserDefinedDialog::populateList() {
    call_function("clearKeyMap");

    for (size_t i = 0; i < 128; ++i) {
        TypingAction action = keyMap_[i];
        if (action == TypingAction::None) continue;

        std::wstring keyStr(1, static_cast<wchar_t>(i));
        std::wstring actionName = Utf8ToWide(std::string(TypingActionToString(action)));

        // JS resolves the localized label via t("ud.act." + action) — see userdefined.js.
        call_function("addKeyToMap", sciter::value(keyStr), sciter::value(actionName));
    }

    // Refresh key field to reflect the currently-selected action's mapping.
    // Needed because populateList() runs in the constructor before document.ready
    // has wired up listeners, so the initial key display would otherwise be empty.
    call_function("syncSelectedActionKey");
}

void UserDefinedDialog::applyAction(TypingAction action, wchar_t newKey) noexcept {
    if (action == TypingAction::None) return;
    for (auto& slot : keyMap_) {
        if (slot == action) slot = TypingAction::None;
    }
    if (newKey < 128) {
        keyMap_[static_cast<uint8_t>(newKey)] = action;
    }
}

void UserDefinedDialog::clearAction(TypingAction action) noexcept {
    if (action == TypingAction::None) return;
    for (auto& slot : keyMap_) {
        if (slot == action) slot = TypingAction::None;
    }
}

void UserDefinedDialog::loadTemplate(bool telex) {
    keyMap_.fill(TypingAction::None);

    // Common keys for both (a-z, 0-9, and punctuation used in Telex/VNI)
    std::string chars = "abcdefghijklmnopqrstuvwxyz0123456789[]";
    for (char c : chars) {
        TypingAction action = ClassifyKey(static_cast<wchar_t>(c), telex, !telex);
        if (action != TypingAction::None) {
            keyMap_[static_cast<uint8_t>(c)] = action;
        }
    }

    populateList();
    persistAndSignal();
}

bool UserDefinedDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
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
                
                if (action == L"apply") {
                    sciter::dom::element keyInput = root.find_first("#val-key");
                    sciter::dom::element actionInput = root.find_first("#val-key-action");

                    if (keyInput.is_valid() && actionInput.is_valid()) {
                        std::wstring keyStr = keyInput.get_value().get<std::wstring>();
                        std::wstring actionName = actionInput.get_value().get<std::wstring>();

                        if (!keyStr.empty() && !actionName.empty()) {
                            wchar_t k = towlower(keyStr[0]);
                            TypingAction newAction = StringToTypingAction(WideToUtf8(actionName));
                            if (k < 128 && newAction != TypingAction::None) {
                                applyAction(newAction, k);
                                populateList();
                                persistAndSignal();
                            }
                        }
                    }
                } else if (action == L"clear_action") {
                    sciter::dom::element actionInput = root.find_first("#val-key-action");
                    if (actionInput.is_valid()) {
                        std::wstring actionName = actionInput.get_value().get<std::wstring>();
                        if (!actionName.empty()) {
                            TypingAction target = StringToTypingAction(WideToUtf8(actionName));
                            if (target != TypingAction::None) {
                                clearAction(target);
                                populateList();
                                persistAndSignal();
                            }
                        }
                    }
                } else if (action == L"load_telex") {
                    loadTemplate(true);
                } else if (action == L"load_vni") {
                    loadTemplate(false);
                } else if (action == L"import") {
                    importKeyMap();
                } else if (action == L"export") {
                    exportKeyMap();
                }

                // Reset val-action to allow re-triggering same action
                el.set_value(sciter::value(L""));
                return true;
            }
        }
    }

    return sciter::window::handle_event(he, params);
}

void UserDefinedDialog::importKeyMap() {
    std::wstring path = ShowOpenFileDialogW(get_hwnd(), L"Keymap files (*.keymap)\0*.keymap\0All files (*.*)\0*.*\0", L"keymap");
    if (path.empty()) return;

    TypingConfig imported;
    if (ConfigManager::ImportCustomKeyMap(path, imported)) {
        keyMap_ = imported.customKeyMap;
        populateList();
        persistAndSignal();
    }
}

void UserDefinedDialog::exportKeyMap() {
    std::wstring path = ShowSaveFileDialogW(get_hwnd(), L"Keymap files (*.keymap)\0*.keymap\0", L"keymap", L"custom.keymap");
    if (path.empty()) return;

    TypingConfig out;
    out.customKeyMap = keyMap_;
    (void)ConfigManager::ExportCustomKeyMap(path, out);
}

}  // namespace NextKey
