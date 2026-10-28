// VKey - Subprocess Runner Functions
// SPDX-License-Identifier: GPL-3.0-only

#include "SubprocessRunners.h"
#include "SubprocessHelper.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/ExcludedAppsDialog.h"
#include "dialogs/TsfAppsDialog.h"
#include "dialogs/MacroTableDialog.h"
#include "dialogs/ConvertToolDialog.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/AppOverridesDialog.h"
#include "dialogs/SpellExclusionsDialog.h"
#include "dialogs/UserDefinedDialog.h"
#include "dialogs/HotkeysDialog.h"
#include "dialogs/IconSettingsDialog.h"
#include "core/Debug.h"

#include <Windows.h>

namespace NextKey {

[[noreturn]] void RunSettingsSubprocess() {
    NEXTKEY_LOG(L"Running settings subprocess");

    // Single control-panel guard (#209). SpawnSettingsSubprocess() already dedups
    // via FindWindow("VKey Settings"), but that title only exists AFTER the child
    // creates its window — a double tray/taskbar click spawns two children that
    // both find nothing and open two panels. The mutex is acquired before window
    // creation, so the loser focuses the live window (best-effort) and exits.
    // ponytail: handle leaked deliberately — ExitProcess frees it with the process.
    HANDLE hSettingsMutex = CreateMutexW(nullptr, TRUE, L"Local\\VKey_Settings_Mutex");
    if (hSettingsMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        FocusExistingWindow(FindWindowW(nullptr, L"VKey Settings"));
        NEXTKEY_LOG(L"Settings already open — focusing existing, exiting");
        ExitProcess(0);
    }

    InitSciterSubprocess();
    NEXTKEY_LOG(L"Sciter initialized, creating dialog...");

    // Parse initial V/E mode from command line (--mode 0 or --mode 1)
    bool initialVietnamese = true;
    const wchar_t* cmdLine = GetCommandLineW();
    const wchar_t* modeArg = wcsstr(cmdLine, L"--mode ");
    if (modeArg) {
        initialVietnamese = (modeArg[7] != L'0');
    }

    // Create and show dialog
    SettingsDialog dialog;
    dialog.SetVietnameseMode(initialVietnamese);
    dialog.SetOnSettingsChanged([]() {
        NEXTKEY_LOG(L"Settings changed");
    });

    dialog.Show();

    // Kill any child processes (ExcludedApps, TsfApps, Macro, AppOverrides)
    // that Settings may have spawned and are still open.
    TerminateAllSubprocesses();

    // INTENTIONAL: ExitProcess() is required here because Sciter's internal
    // cleanup triggers assertion failures on normal process exit. This is a
    // known Sciter issue. Since this is a subprocess with no shared state
    // to flush, ExitProcess() is safe and avoids the assertion.
    NEXTKEY_LOG(L"Settings subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunExcludedAppsSubprocess() {
    NEXTKEY_LOG(L"Running excluded apps subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    ExcludedAppsDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"Excluded apps subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunTsfAppsSubprocess() {
    NEXTKEY_LOG(L"Running TSF apps subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    TsfAppsDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"TSF apps subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunMacroSubprocess() {
    NEXTKEY_LOG(L"Running macro table subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    MacroTableDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"Macro table subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunConvertToolSubprocess() {
    NEXTKEY_LOG(L"Running convert tool subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    ConvertToolDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"Convert tool subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunAboutSubprocess() {
    NEXTKEY_LOG(L"Running about subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    AboutDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"About subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunAppOverridesSubprocess() {
    NEXTKEY_LOG(L"Running app overrides subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    AppOverridesDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"App overrides subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunSpellExclusionsSubprocess() {
    NEXTKEY_LOG(L"Running spell exclusions subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    SpellExclusionsDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"Spell exclusions subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunUserDefinedSubprocess() {
    NEXTKEY_LOG(L"Running user defined input subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    UserDefinedDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"User defined input subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunHotkeysSubprocess() {
    NEXTKEY_LOG(L"Running hotkeys rebind subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    HotkeysDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"Hotkeys subprocess exiting");
    ExitProcess(0);
}

[[noreturn]] void RunIconSettingsSubprocess() {
    NEXTKEY_LOG(L"Running icon settings subprocess");

    InitSciterSubprocess();

    HWND parent = FindWindowW(nullptr, L"VKey Settings");
    IconSettingsDialog dialog(parent);
    dialog.Show();

    NEXTKEY_LOG(L"Icon settings subprocess exiting");
    ExitProcess(0);
}

}  // namespace NextKey
