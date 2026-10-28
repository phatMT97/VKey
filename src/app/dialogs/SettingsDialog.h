// VKey - Settings Dialog Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Undefine Windows macros that conflict with Sciter enums
#ifdef KEY_DOWN
#undef KEY_DOWN
#endif
#ifdef KEY_UP
#undef KEY_UP
#endif

#include "sciter-x-window.hpp"
#include "core/config/TypingConfig.h"
#include "core/SystemConfig.h"
#include "core/ipc/SharedStateManager.h"
#include "core/config/ConfigEvent.h"
#include "system/UpdateChecker.h"
#include <functional>
#include <string>

namespace NextKey {

/// Callback for when settings change
using SettingsChangedCallback = std::function<void()>;

/// Settings dialog using Sciter for modern UI
class SettingsDialog : public sciter::window {
public:
    SettingsDialog();
    ~SettingsDialog();

    /// Show the settings dialog (blocks until closed)
    void Show();

    /// Set Vietnamese mode state (updates toggle UI if window exists)
    void SetVietnameseMode(bool vietnamese);

    /// Set callback for settings changes
    void SetOnSettingsChanged(SettingsChangedCallback callback) { onSettingsChanged_ = callback; }

    // Override to avoid sciter::application dependency
    HINSTANCE get_resource_instance() const { return nullptr; }

    // Override resource loading to support both Debug (file system) and Release (packed)
    virtual LRESULT on_load_data(LPSCN_LOAD_DATA pnmld) override;

    // Override event handler for BUTTON_CLICK, VALUE_CHANGED events
    // Note: Must use handle_event, not on_event, because sciter::window::handle_event
    // doesn't call the base class, so on_event is never invoked.
    virtual bool handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) override;

    // SOM functions exposed to JavaScript
    void onInputMethodChange(int method);  // 0=Telex, 1=VNI
    void onExpandChange(bool expanded);
    void onToggleChange(sciter::string id, bool checked);  // Direct toggle → C++ (bypasses DOM events)
    void onClose();
    void requestRestartWindows();  // Restart-banner button → ExitWindowsEx

    // SOM passport for JavaScript binding
    SOM_PASSPORT_BEGIN(SettingsDialog)
        SOM_FUNCS(
            SOM_FUNC(onInputMethodChange),
            SOM_FUNC(onExpandChange),
            SOM_FUNC(onToggleChange),
            SOM_FUNC(onClose),
            SOM_FUNC(requestRestartWindows)
        )
    SOM_PASSPORT_END

private:
    static constexpr UINT_PTR TIMER_DEFERRED_SAVE = 1002;
    static constexpr DWORD DEFERRED_SAVE_DELAY_MS = 30000;  // 30 seconds

    void loadSettings();
    void saveSettings();       // Immediate SharedState + deferred TOML
    void syncToSharedState();  // Immediate: SharedState + ConfigEvent
    void saveToToml();         // Deferred: TOML file write
    void saveUISettings();
    void saveSystemSettings();
    void applyDocumentSettings();
    void initializeWindow();
    void initializeUI();

    bool uiInitialized_ = false;
    bool configDirty_ = false;

    // Event handlers for specific settings
    void handleToggleChange(const std::wstring& id, bool value);
    void handleDropdownChange(const std::wstring& id, int value);
    void handleButtonClick(const std::wstring& id);

    // Window control
    void togglePin();
    void notifySubdialogPinState();

    // UI helpers
    void setToggleState(const std::wstring& id, bool checked);
    void setDropdownValue(const std::wstring& id, int value);
    void recalcWindowSize();  // Measure DOM and resize window to fit content
    int measureMaxAdvancedContentHeight(double dpiScale);  // #226: max height across all tab panels (fixed window height)

    // Notify main process to re-read system settings (theme/language changes).
    void notifySystemConfigChanged(WPARAM wParam = 0);

    // Update helpers
    void startUpdateCheck();            // Check for updates with progress dialog
    void startUpdate(const UpdateInfo& info);  // Download + launch updater + exit
    void setUpdateButtonEnabled(bool enabled);

    // Subclass procedure for window dragging and close
    static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR uIdSubclass,
                                         DWORD_PTR dwRefData);

    SettingsChangedCallback onSettingsChanged_;

    // Settings state
    TypingConfig config_;          // Typing config (inputMethod, spellCheck, features, etc.)
    HotkeyConfig hotkeyConfig_;    // Hotkey modifiers + key
    SystemConfig systemConfig_;    // System settings (startup, admin, show on startup)
    bool vietnameseMode_ = true;   // V/E mode (synced with main process, not persisted)
    bool modeSyncPending_ = false; // Coalescing flag: deferred toggle-language DOM update in flight
    // Re-entrancy guard for handleToggleChange(). A toggle branch may open a modal
    // MessageBox / UAC prompt (debug-log warning, TSF register/unregister, run-admin);
    // its nested message loop re-delivers the SAME Sciter VALUE_CHANGED event back into
    // handle_event → handleToggleChange before config_ is updated, cascading duplicate
    // dialogs. Incident 2026-06-24 (debug-log + tsf-apps box spam). Main-thread only, so
    // a plain bool suffices. Do NOT remove: it keeps the whole handler non-reentrant.
    bool handlingToggle_ = false;
    bool isExpanded_ = false;
    bool isPinned_ = false;
    bool forceLightTheme_ = false;
    bool wasMinimized_ = false; // Track minimization to force repaint on restore

    // #226: cached result of measureMaxAdvancedContentHeight(), computed once the
    // first time the Advanced panel is actually shown (measuring needs the panel's
    // display:block, so it can't happen for free while collapsed). Keeps the window
    // height fixed across tab switches instead of resizing per tab. Invalidated
    // (set back to 0) on language switch; scoped to cachedAdvancedHeightDpiScale_
    // so moving the dialog to a different-DPI monitor forces a fresh measurement.
    int cachedMaxAdvancedHeight_ = 0;
    double cachedAdvancedHeightDpiScale_ = 0.0;

    // UI settings (saved to config)
    uint8_t backgroundOpacity_ = 80;  // 0-100

    // Switch key display string (HotkeyConfig.key is wchar_t, UI needs wstring)
    std::wstring switchKeyChar_ = L"~";

    // UI base path for file system loading (Debug mode)
    std::wstring uiBasePath_;

    // Cached IPC handles (opened once, reused for every toggle)
    SharedStateManager sharedState_;
    ConfigEvent configEvent_;
};

}  // namespace NextKey
