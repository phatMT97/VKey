// VKey - Sciter SubDialog Base Class
// SPDX-License-Identifier: GPL-3.0-only
//
// Base class for simple Sciter subdialogs that run as subprocesses.
// Extracts all common boilerplate: resource loading, window setup,
// SubclassProc, drag handling, ExitProcess(0) on close.
//
// NOT suitable for SettingsDialog (SOM passports, custom messages, complex init).

#pragma once

// Undefine Windows macros that conflict with Sciter enums
#ifdef KEY_DOWN
#undef KEY_DOWN
#endif
#ifdef KEY_UP
#undef KEY_UP
#endif

#include "SubDialogConfig.h"
#include "sciter-x-window.hpp"
#include <string>

namespace NextKey {

/// Base class for simple Sciter subdialogs running as subprocesses.
/// Handles: resource loading, window chrome, drag, blur, ExitProcess on close.
class SciterSubDialog : public sciter::window {
public:
    explicit SciterSubDialog(const SubDialogConfig& config);
    virtual ~SciterSubDialog();

    /// Show the dialog (blocks until closed via message loop)
    void Show();

    /// Override resource loading (Debug: file system, Release: packed archive)
    LRESULT on_load_data(LPSCN_LOAD_DATA pnmld) override;

protected:
    /// Called before ExitProcess(0) on WM_CLOSE. Override for save/cleanup.
    virtual void onBeforeClose() {}

    /// Handle custom WM_ messages in SubclassProc. Return -1 if unhandled.
    virtual LRESULT onCustomMessage(HWND, UINT, WPARAM, LPARAM) { return -1; }

    /// Access the config
    const SubDialogConfig& config() const { return config_; }

private:
    static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR uIdSubclass,
                                         DWORD_PTR dwRefData);

    SubDialogConfig config_;
    std::wstring uiBasePath_;
    bool wasMinimized_ = false;

    // Single instance per process (subdialogs run as separate processes)
    static SciterSubDialog* s_instance;
};

}  // namespace NextKey
