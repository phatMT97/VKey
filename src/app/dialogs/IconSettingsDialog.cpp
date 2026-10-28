// VKey - Icon Settings Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "IconSettingsDialog.h"

#include <algorithm>
#include <commdlg.h>
#include <string>

#include "core/Debug.h"
#include "core/config/ConfigManager.h"
#include "core/ipc/SharedConstants.h"
#include "sciter/ScaleHelper.h"
#include "sciter-x-dom.hpp"

using namespace sciter::dom;

namespace NextKey {

namespace {

constexpr int kBaseHeight = 356;
constexpr int kCustomColorsHeight = 416;

SubDialogConfig MakeIconSettingsConfig(HWND parent) {
    const SystemConfig config = ConfigManager::LoadSystemConfigOrDefault();
    const bool custom = config.iconStyle == static_cast<uint8_t>(IconStyle::Custom);
    return {
        L"this://app/iconsettings/iconsettings.html",
        L"VKey - Tùy chỉnh icon",
        430, custom ? kCustomColorsHeight : kBaseHeight,
        parent, false, 36, 40, true
    };
}

}  // namespace

IconSettingsDialog::IconSettingsDialog(HWND parent)
    : SciterSubDialog(MakeIconSettingsConfig(parent))
    , systemConfig_(ConfigManager::LoadSystemConfigOrDefault()) {
    customColorsVisible_ =
        systemConfig_.iconStyle == static_cast<uint8_t>(IconStyle::Custom);
    populate();
}

void IconSettingsDialog::setToggleState(const char* id, bool checked) {
    element root = get_root();
    element toggle = root.find_first(("#" + std::string(id)).c_str());
    if (toggle.is_valid()) {
        std::wstring classes = toggle.get_attribute("class");
        const size_t checkedPos = classes.find(L" checked");
        if (checkedPos != std::wstring::npos) {
            classes.erase(checkedPos, 8);
        }
        if (checked) classes += L" checked";
        toggle.set_attribute("class", classes.c_str());
        toggle.set_attribute("aria-checked", checked ? L"true" : L"false");
    }

    element hidden = root.find_first(("#val-" + std::string(id)).c_str());
    if (hidden.is_valid()) hidden.set_value(sciter::value(checked ? 1 : 0));
}

void IconSettingsDialog::setDropdownValue(const char* id, int value) {
    element root(get_root());
    element dropdown = root.find_first(("#" + std::string(id)).c_str());
    if (dropdown.is_valid()) dropdown.set_value(sciter::value(value));
}

void IconSettingsDialog::populate() {
    setDropdownValue("modern-icon", static_cast<int>(systemConfig_.iconStyle));
    setToggleState("tsf-indicator", systemConfig_.showTsfIndicator);
    setToggleState("floating-icon", systemConfig_.showFloatingIcon);
    updateColorSwatches();

    element root(get_root());
    element colorRow = root.find_first("#custom-color-row");
    if (colorRow.is_valid()) {
        colorRow.set_style_attribute("display", customColorsVisible_ ? L"block" : L"none");
    }
}

void IconSettingsDialog::setCustomColorsVisible(bool visible) {
    element root(get_root());
    element colorRow = root.find_first("#custom-color-row");
    if (colorRow.is_valid()) {
        colorRow.set_style_attribute("display", visible ? L"block" : L"none");
    }
    if (visible == customColorsVisible_) return;

    RECT windowRect{};
    if (GetWindowRect(get_hwnd(), &windowRect)) {
        const int delta = ScaleHelper::scale(kCustomColorsHeight - kBaseHeight);
        const int signedDelta = visible ? delta : -delta;
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top + signedDelta;
        SetWindowPos(get_hwnd(), nullptr,
                     windowRect.left, windowRect.top - signedDelta / 2,
                     width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    customColorsVisible_ = visible;
}

void IconSettingsDialog::saveAndNotify(WPARAM iconChangeFlags) {
    // Icon fields live in the same [system] table SettingsDialog's own System
    // tab edits from its own process (modeless — both can be open at once).
    // SaveSystemConfig() writes the WHOLE table, so saving systemConfig_
    // directly would silently revert any run_at_startup/language/theme/...
    // change SettingsDialog made since this dialog's construction-time
    // snapshot. Merge onto a freshly-loaded snapshot instead: only the fields
    // this dialog actually edits are ours to overwrite.
    SystemConfig toSave = ConfigManager::LoadSystemConfigOrDefault();
    toSave.iconStyle = systemConfig_.iconStyle;
    toSave.customColorV = systemConfig_.customColorV;
    toSave.customColorE = systemConfig_.customColorE;
    toSave.showTsfIndicator = systemConfig_.showTsfIndicator;
    toSave.showFloatingIcon = systemConfig_.showFloatingIcon;
    toSave.floatingIconX = systemConfig_.floatingIconX;
    toSave.floatingIconY = systemConfig_.floatingIconY;

    const std::wstring path = ConfigManager::GetConfigPath();
    if (!ConfigManager::SaveSystemConfig(path, toSave)) {
        NEXTKEY_LOG(L"IconSettingsDialog: failed to save system config to %s", path.c_str());
        return;
    }

    // Refresh the running tray/floating icon immediately. Use a bounded
    // synchronous send so the color has been applied before the picker returns;
    // PostMessage could fail silently across an integrity-level boundary.
    if (HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr)) {
        DWORD_PTR messageResult = 0;
        if (!SendMessageTimeoutW(
                trayWnd, WM_VKEY_ICON_CHANGED, iconChangeFlags, 0,
                SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &messageResult)) {
            NEXTKEY_LOG(L"IconSettingsDialog: failed to notify tray icon (error=%lu)",
                        GetLastError());
        }
    } else {
        NEXTKEY_LOG(L"IconSettingsDialog: tray message window not found");
    }

    // SettingsDialog caches the whole SystemConfig. Refresh only that cache so
    // later System-tab saves cannot overwrite these icon values. Do not ask it
    // to rebuild the entire UI: that may dispatch changes for unrelated fields.
    if (HWND settingsWnd = FindWindowW(nullptr, L"VKey Settings")) {
        PostMessageW(settingsWnd, WM_VKEY_ICON_SETTINGS_CHANGED, 0, 0);
    }
}

void IconSettingsDialog::openColorPicker(bool forVietnamese) {
    const COLORREF current = static_cast<COLORREF>(
        forVietnamese ? systemConfig_.GetEffectiveColorV()
                      : systemConfig_.GetEffectiveColorE());
    static COLORREF customColors[16] = {};

    CHOOSECOLORW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = get_hwnd();
    picker.lpCustColors = customColors;
    picker.rgbResult = current;
    picker.Flags = CC_FULLOPEN | CC_RGBINIT;

    if (!ChooseColorW(&picker)) return;

    if (forVietnamese) {
        systemConfig_.customColorV = static_cast<uint32_t>(picker.rgbResult);
    } else {
        systemConfig_.customColorE = static_cast<uint32_t>(picker.rgbResult);
    }
    updateColorSwatches();
    saveAndNotify();
}

void IconSettingsDialog::updateColorSwatches() {
    element root = get_root();
    auto setSwatch = [&](const char* selector, COLORREF color) {
        element swatch = root.find_first(selector);
        if (!swatch.is_valid()) return;

        wchar_t css[64] = {};
        swprintf_s(css, L"rgb(%d,%d,%d)",
                   GetRValue(color), GetGValue(color), GetBValue(color));
        swatch.set_style_attribute("background", css);
        swatch.set_style_attribute("background-color", css);
    };

    setSwatch("#preview-color-v", static_cast<COLORREF>(systemConfig_.GetEffectiveColorV()));
    setSwatch("#preview-color-e", static_cast<COLORREF>(systemConfig_.GetEffectiveColorE()));
}

bool IconSettingsDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    UNREFERENCED_PARAMETER(he);

    if (params.cmd == BUTTON_CLICK) {
        element target(params.heTarget);
        const std::wstring id = target.get_attribute("id");

        if (id == L"btn-close") {
            PostMessageW(get_hwnd(), WM_CLOSE, 0, 0);
        } else if (id == L"btn-color-v") {
            openColorPicker(true);
        } else if (id == L"btn-color-e") {
            openColorPicker(false);
        } else if (id == L"btn-reset-colors") {
            systemConfig_.customColorV = 0;
            systemConfig_.customColorE = 0;
            updateColorSwatches();
            saveAndNotify();
        } else if (id == L"btn-reset-floating-icon") {
            systemConfig_.floatingIconX = INT32_MIN;
            systemConfig_.floatingIconY = INT32_MIN;
            saveAndNotify(1);  // Main process resets the live overlay position.
        } else {
            return sciter::window::handle_event(he, params);
        }
        return true;
    }

    if (params.cmd == VALUE_CHANGED) {
        element target(params.heTarget);
        const std::wstring id = target.get_attribute("id");
        const sciter::value value = target.get_value();

        if (id == L"modern-icon") {
            int style = 0;
            if (value.is_int()) {
                style = value.get<int>();
            } else if (value.is_string()) {
                style = _wtoi(value.get<std::wstring>().c_str());
            }
            systemConfig_.iconStyle = static_cast<uint8_t>(std::clamp(style, 0, 4));
            setCustomColorsVisible(
                systemConfig_.iconStyle == static_cast<uint8_t>(IconStyle::Custom));
            saveAndNotify();
            return true;
        }

        auto readToggle = [&]() {
            if (value.is_bool()) return value.get<bool>();
            if (value.is_int()) return value.get<int>() != 0;
            return value.is_string() && value.get<std::wstring>() != L"0";
        };

        if (id == L"val-tsf-indicator") {
            systemConfig_.showTsfIndicator = readToggle();
            saveAndNotify();
            return true;
        }
        if (id == L"val-floating-icon") {
            systemConfig_.showFloatingIcon = readToggle();
            saveAndNotify();
            return true;
        }
    }

    return sciter::window::handle_event(he, params);
}

}  // namespace NextKey
