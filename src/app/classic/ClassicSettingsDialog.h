// VKey Classic — Settings Dialog
// Compact (Unikey-style) + Advanced (EVKey-style) modes
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "ClassicTheme.h"
#include "ClassicDialogUtils.h"
#include "resource.h"
#include "core/config/TypingConfig.h"
#include "core/config/SettingMetadata.h"
#include "core/SystemConfig.h"
#include "core/UIConfig.h"
#include "core/ipc/SharedStateManager.h"
#include "core/config/ConfigEvent.h"
#include <Windows.h>
#include <commctrl.h>
#include <string>
#include <functional>

namespace NextKey::Classic {

/// Win32 settings dialog with two layout modes:
/// - Compact (320x280): dropdowns + "Mo rong" checkbox + buttons (Unikey-like)
/// - Advanced (500x460): compact top + tabbed 2-column checkboxes (EVKey-like)
///
/// Reads/writes settings via SettingMetadata mapping table + offsetof.
/// Syncs to SharedState immediately; deferred TOML save via 30s timer.
class ClassicSettingsDialog {
public:
    ClassicSettingsDialog() = default;
    ~ClassicSettingsDialog();

    // Non-copyable
    ClassicSettingsDialog(const ClassicSettingsDialog&) = delete;
    ClassicSettingsDialog& operator=(const ClassicSettingsDialog&) = delete;

    /// Create and show the dialog. Runs a modal message loop.
    /// Returns true if the dialog was shown successfully.
    bool Show(HINSTANCE hInstance, HWND parent = nullptr);

    [[nodiscard]] HWND GetHwnd() const noexcept { return hwnd_; }
    [[nodiscard]] const ClassicTheme& theme() const noexcept { return theme_; }

private:
    friend LRESULT CALLBACK TabSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // -- Window setup --
    bool RegisterWindowClass(HINSTANCE hInstance);
    void CreateCompactControls();
    void CreateAdvancedControls();

    // -- Settings I/O --
    void LoadSettings();
    void SaveSettings();
    void SyncToSharedState();
    void SaveToToml();
    void PopulateControls();
    void ReadControlValues();

    // -- Event handlers --
    void OnCommand(WPARAM wParam, LPARAM lParam);
    void OnActionButton(uint16_t controlId);
    void OnSystemToggle(const wchar_t* id, bool value);
    /// Side-effect handler for the TSF-apps checkbox.
    /// Registers/unregisters the TSF DLL to match the requested state.
    /// Returns true if the DLL state now matches; false means the caller
    /// must revert the checkbox + config (user denied UAC, regsvr32 failed, etc).
    [[nodiscard]] bool OnTsfAppsToggle(bool wantsEnabled);
    void OnPickIconColors();
    void UpdateSpellCheckChildren();
    void UpdateTsfChildren();
    void UpdateMacroChildren();
    void UpdateMacroCapsChildren();
    /// Shared CBN_DROPDOWN/CBN_CLOSEUP combo-box animation suppression
    /// (dark-mode-only Win32 visual glitch workaround), used by every
    /// hand-wired combo case in OnCommand.
    void HandleComboDarkModeAnimation(UINT code, LPARAM lParam);
    void RefreshLabels();
    void OnTabChange();
    void ShowTabPage(int tabIndex);
    void UpdateCustomKeyMapButtonVisibility();

    // -- Helpers --
    HWND CreateLabel(const wchar_t* text, int x, int y, int w, int h, UINT id);
    HWND CreateCombo(int x, int y, int w, int h, UINT id);
    HWND CreateCheck(const wchar_t* text, int x, int y, int w, int h, UINT id);
    HWND CreateEdit(int x, int y, int w, int h, UINT id);
    HWND CreateBtn(const wchar_t* text, int x, int y, int w, int h, UINT id, bool isPrimary = false);
    void SetupTooltips();
    void ThemeTooltip();
    void SetFontOnAllChildren();
    static BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lParam);
    int Dpi(int value) const noexcept;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // -- Layout constants (pixels at 96 DPI, scaled by Dpi()) --
    // All values are multiples of 4 for consistent visual rhythm
    static constexpr int kAdvancedWidth  = 490;
    // Must clear the tallest tab column (tab 0 / column 1) plus the "Báo cáo lỗi"
    // link, which is anchored to the tab control's bottom edge — one row too short
    // and the link lands on top of the last button.
    static constexpr int kAdvancedHeight = 456;
    static constexpr int kPadding       = 16;
    static constexpr int kControlHeight = 24;
    static constexpr int kComboHeight   = 24;
    static constexpr int kButtonHeight  = 32;
    static constexpr int kRowGap        = 5;
    static constexpr int kSectionGap    = 8;
    static constexpr int kLabelHeight   = 16;
    static constexpr int kTabHeight     = 28;

    // -- State --
    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    ClassicTheme theme_;
    UINT dpi_ = 96;

    // Compact controls
    HWND comboMethod_   = nullptr;
    HWND comboEncoding_ = nullptr;
    HWND btnClose_      = nullptr;
    HWND btnExit_       = nullptr;
    HWND editHotkey_    = nullptr;
    HWND tooltip_       = nullptr;
    HFONT fontSmall_    = nullptr;
    HWND btnCustomKeymap_ = nullptr;

    // Advanced controls
    HWND tabControl_    = nullptr;
    HWND linkReportBug_ = nullptr;
    HWND lblSpellCheckLevel_   = nullptr;
    HWND comboSpellCheckLevel_ = nullptr;  // Off / Standard; Advanced only in Rust dev builds
                                            // like comboMethod_/comboEncoding_, not
                                            // table-driven (see SpellCheckLevel design
                                            // note in TypingConfig.h)
    bool advancedCreated_ = false;
    int currentTab_     = 0;

    // Tab icons (color icons replacing emoji text)
    HICON tabIcons_[3]{};
    int   iconSize_ = 16;

    // Per-tab checkbox HWNDs (indexed by kSettings array index)
    static constexpr size_t kMaxControls = 64;
    static_assert(kSettingsCount <= kMaxControls, "kMaxControls too small for kSettingsCount");
    HWND checkControls_[kMaxControls] = {};
    HWND extraControls_[kMaxControls] = {};

    // Settings data
    TypingConfig config_{};
    HotkeyConfig hotkeyConfig_{};
    SystemConfig systemConfig_{};
    SharedStateManager sharedState_;
    ConfigEvent configEvent_;
    bool configDirty_ = false;

    static constexpr UINT_PTR kTimerDeferredSave = 1001;
    static constexpr DWORD kDeferredSaveDelayMs  = 30000;  // 30 seconds

    static constexpr const wchar_t* kClassName = L"VKeyClassicSettings";
};

} // namespace NextKey::Classic
