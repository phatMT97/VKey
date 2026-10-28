// VKey - Convert Tool Dialog Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "SciterSubDialog.h"
#include "core/config/TypingConfig.h"
#include <string>

namespace NextKey {

/// Dialog for Vietnamese text encoding conversion (Sciter subdialog)
class ConvertToolDialog : public SciterSubDialog {
public:
    static constexpr int kBaseWidth  = 420;
    static constexpr int kBaseHeight = 520;

    ConvertToolDialog(HWND parent);

    // Override event handler for VALUE_CHANGED, BUTTON_CLICK, and DOCUMENT_COMPLETE
    bool handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) override;

private:
    void doConvert();
    void togglePin();
    void browseFile(bool isSource);
    [[nodiscard]] bool getToggleValue(const char* id);
    [[nodiscard]] int getDropdownValue(const char* id);
    [[nodiscard]] std::wstring getHiddenValue(const char* id);
    [[nodiscard]] static std::wstring readClipboardText();
    [[nodiscard]] static bool writeClipboardText(const std::wstring& text);
    [[nodiscard]] static std::wstring readFileContent(const std::wstring& path, CodeTable sourceTable);
    [[nodiscard]] static bool writeFileContent(const std::wstring& path, const std::wstring& text, CodeTable destTable);

    void saveConvertConfig();
    void setToggleUI(const char* toggleId, const char* hiddenId, bool value);
    void setDropdownUI(const char* id, int value);

    ConvertConfig config_;
    bool isPinned_ = false;
};

}  // namespace NextKey
