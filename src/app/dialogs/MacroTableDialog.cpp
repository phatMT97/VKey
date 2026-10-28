// VKey - Macro Table Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "MacroTableDialog.h"
#include "DialogUtils.h"
#include "core/config/ConfigManager.h"
#include "core/Strings.h"
#include "core/WinStrings.h"
#include "helpers/AppHelpers.h"
#include "sciter-x-dom.hpp"
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>

using namespace sciter::dom;

namespace NextKey {

MacroTableDialog::MacroTableDialog(HWND parent)
    : SciterSubDialog({
        L"this://app/macro/macro.html",
        L"VKey - Macro Table",
        kBaseWidth, kBaseHeight, parent, false, 36, 40, true
    }) {
    macros_ = ConfigManager::LoadMacros(ConfigManager::GetConfigPath());
    populateList();

    // Set trigger states in UI via DOM attributes (safe during init — no JS dependency)
    TypingConfig cfg = ConfigManager::LoadOrDefault();
    sciter::dom::element root = get_root();
    auto setChecked = [&](const char* id, bool val) {
        sciter::dom::element el = root.find_first(id);
        if (el.is_valid()) el.set_value(sciter::value(val));
    };
    setChecked("#cfg-macro_trigger_space", cfg.macroTriggerSpace);
    setChecked("#cfg-macro_trigger_enter", cfg.macroTriggerEnter);
    setChecked("#cfg-macro_trigger_tab", cfg.macroTriggerTab);
    setChecked("#cfg-macro_trigger_dir", cfg.macroTriggerDir);
}


void MacroTableDialog::persistAndSignal() {
    (void)ConfigManager::SaveMacros(ConfigManager::GetConfigPath(), macros_);
    SignalConfigChange();
}

bool MacroTableDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
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
                // Read macro name and content from hidden inputs
                sciter::dom::element root = get_root();
                sciter::dom::element oldNameInput = root.find_first("#val-old-macro-name");
                sciter::dom::element nameInput = root.find_first("#val-macro-name");
                sciter::dom::element contentInput = root.find_first("#val-macro-content");

                std::wstring oldMacroName;
                std::wstring macroName;
                std::wstring macroContent;
                if (oldNameInput.is_valid()) {
                    sciter::value ov = oldNameInput.get_value();
                    oldMacroName = ov.is_string() ? ov.get<std::wstring>() : L"";
                }
                if (nameInput.is_valid()) {
                    sciter::value nv = nameInput.get_value();
                    macroName = nv.is_string() ? nv.get<std::wstring>() : L"";
                }
                if (contentInput.is_valid()) {
                    sciter::value cv = contentInput.get_value();
                    macroContent = cv.is_string() ? cv.get<std::wstring>() : L"";
                }

                if (action == L"add") {
                    if (!macroName.empty() && !macroContent.empty()) {
                        addMacro(macroName, macroContent);
                    }
                } else if (action == L"edit") {
                    if (!macroName.empty() && !macroContent.empty()) {
                        if (!oldMacroName.empty() && oldMacroName != macroName) {
                            macros_.erase(oldMacroName);
                        }
                        addMacro(macroName, macroContent);
                    }
                } else if (action == L"delete") {
                    if (!macroName.empty()) {
                        // Newline-delimited, not ';' — a shortcut may legally contain
                        // any printable char (HookEngine feeds punctuation into the
                        // macro buffer too), so ";;"-style names would split apart.
                        // An <input> can't hold a newline, so '\n' is collision-free.
                        std::vector<std::wstring> toDelete;
                        std::wstringstream ss(macroName);
                        std::wstring item;
                        while (std::getline(ss, item)) {
                            if (!item.empty()) {
                                toDelete.push_back(item);
                            }
                        }

                        if (!toDelete.empty()) {
                            std::wstring msg;
                            if (toDelete.size() == 1) {
                                msg = S(StringId::MACRO_CONFIRM_DELETE);
                                msg += L"\n\n";
                                msg += toDelete[0];
                            } else {
                                wchar_t buf[256];
                                swprintf_s(buf, S(StringId::MACRO_CONFIRM_DELETE_MULTI),
                                           static_cast<int>(toDelete.size()));
                                msg = buf;
                            }

                            int msgboxID = MessageBoxW(
                                get_hwnd(),
                                msg.c_str(),
                                L"VKey",
                                MB_ICONQUESTION | MB_YESNO
                            );
                            if (msgboxID == IDYES) {
                                for (const auto& name : toDelete) {
                                    macros_.erase(name);
                                }
                                populateList();
                                persistAndSignal();
                            }
                        }
                    }
                } else if (action == L"import") {
                    importMacros();
                } else if (action == L"export") {
                    exportMacros();
                } else if (action == L"toggle_trigger") {
                    sciter::dom::element triggerIdEl = root.find_first("#val-trigger-id");
                    sciter::dom::element triggerValEl = root.find_first("#val-trigger-val");
                    if (triggerIdEl.is_valid() && triggerValEl.is_valid()) {
                        std::wstring triggerId = triggerIdEl.get_value().get<std::wstring>();
                        bool triggerVal = triggerValEl.get_value().get<std::wstring>() == L"1";
                        
                        TypingConfig cfg = ConfigManager::LoadOrDefault();
                        if (triggerId == L"cfg-macro_trigger_space") cfg.macroTriggerSpace = triggerVal;
                        else if (triggerId == L"cfg-macro_trigger_enter") cfg.macroTriggerEnter = triggerVal;
                        else if (triggerId == L"cfg-macro_trigger_tab") cfg.macroTriggerTab = triggerVal;
                        else if (triggerId == L"cfg-macro_trigger_dir") cfg.macroTriggerDir = triggerVal;
                        
                        (void)ConfigManager::SaveToFile(ConfigManager::GetConfigPath(), cfg);
                        SignalConfigChange();
                    }
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

void MacroTableDialog::populateList() {
    call_function("clearMacroList");

    // Sort entries by key for consistent display
    std::vector<std::pair<std::wstring, std::wstring>> sorted(macros_.begin(), macros_.end());
    std::sort(sorted.begin(), sorted.end());

    for (auto& [name, content] : sorted) {
        call_function("addMacroToList", sciter::value(name.c_str()), sciter::value(content.c_str()));
    }

    call_function("finishMacroList");
}

void MacroTableDialog::addMacro(const std::wstring& name, const std::wstring& content) {
    macros_[name] = content;
    populateList();
    persistAndSignal();
}

void MacroTableDialog::importMacros() {
    std::wstring path = ShowOpenFileDialogW(
        get_hwnd(),
        L"Text file (*.txt)\0*.txt\0All (*.*)\0*.*\0",
        L"txt"
    );
    if (path.empty()) return;

    int msgboxID = MessageBoxW(
        get_hwnd(),
        S(StringId::IMPORT_KEEP_EXISTING),
        L"VKey",
        MB_ICONEXCLAMATION | MB_YESNO
    );

    bool append = (msgboxID == IDYES);
    if (!append) {
        macros_.clear();
    }

    // Read UTF-8 file
    std::ifstream infile(path);
    if (!infile.is_open()) return;

    ParseConfigLines(infile, [&](const std::string& line) {
        // Split on first ':' (key:value format).
        auto pos = line.find(':');
        if (pos == std::string::npos || pos == 0) return;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        if (value.empty()) return;

        std::wstring wKey = Utf8ToWide(key);
        std::wstring wVal = Utf8ToWide(value);
        if (wKey.empty() || wVal.empty()) return;

        macros_[wKey] = wVal;
    });

    populateList();
    persistAndSignal();
}

void MacroTableDialog::exportMacros() {
    std::wstring path = ShowSaveFileDialogW(
        get_hwnd(),
        L"Text file (*.txt)\0*.txt\0",
        L"txt",
        L"VKeyMacro"
    );
    if (path.empty()) return;

    // Sort entries for consistent output
    std::vector<std::pair<std::wstring, std::wstring>> sorted(macros_.begin(), macros_.end());
    std::sort(sorted.begin(), sorted.end());

    std::ofstream outfile(path);
    if (!outfile.is_open()) return;

    // Write OpenKey-compatible header
    outfile << ";Compatible OpenKey Macro Data file*** version=1 ***\n";

    for (auto& [key, value] : sorted) {
        outfile << WideToUtf8(key) << ":" << WideToUtf8(value) << "\n";
    }
}

}  // namespace NextKey
