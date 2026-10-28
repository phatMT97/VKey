// VKey - Subprocess Helper
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared initialization for Sciter subprocess dialogs + subprocess spawning.
// Tracks child process handles for cleanup on exit (TerminateProcess).

#pragma once

#include "sciter/SciterArchive.h"
#include "system/DarkModeHelper.h"
#include "helpers/AppHelpers.h"
#include "core/config/ConfigManager.h"
#include "core/Strings.h"
#include "sciter-x.h"
#include "sciter-x-window.hpp"
#include <windows.h>
#include <ole2.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

namespace NextKey {

/// Job Object for automatic child-process cleanup.
/// When the main process exits (even via crash/TerminateProcess), Windows
/// automatically terminates all processes assigned to this job.
/// Returns a singleton handle; first call creates the job.
inline HANDLE GetJobObject() noexcept {
    static HANDLE hJob = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return h;
    }();
    return hJob;
}

/// Per-process list of child process handles (for TerminateProcess on exit)
inline std::vector<HANDLE>& GetChildProcessHandles() {
    static std::vector<HANDLE> handles;
    return handles;
}

/// Terminate all tracked child processes and close handles.
/// Call from exit handler before PostQuitMessage / ExitProcess.
inline void TerminateAllSubprocesses() {
    for (HANDLE h : GetChildProcessHandles()) {
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
    }
    GetChildProcessHandles().clear();
}

/// Track a child process handle (caller retains ownership until TerminateAllSubprocesses)
inline void TrackChildProcess(HANDLE hProcess) {
    if (!hProcess) return;
    auto& handles = GetChildProcessHandles();
    // Purge handles to already-exited processes
    std::erase_if(handles, [](HANDLE h) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            return true;
        }
        return false;
    });
    handles.push_back(hProcess);

    // Also assign to job object so Windows auto-kills on main process exit
    HANDLE hJob = GetJobObject();
    if (hJob) {
        AssignProcessToJobObject(hJob, hProcess);
    }
}

/// Initialize Sciter runtime for a subprocess dialog.
/// Call once at the start of RunXxxSubprocess() before creating any dialog.
inline void InitSciterSubprocess() {
    OleInitialize(nullptr);
    DarkModeHelper::ApplyDarkModeForApp();  // Enable dark mode for native controls

    // Load UI language from config (subprocess starts with default=Vietnamese)
    auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
    SetLanguage(static_cast<Language>(sysConfig.language));

    // FIX: ClearType corrupts alpha channel on Win10 DWM surfaces.
    // SCITER_SET_GFX_LAYER must be set BEFORE application::start() so the
    // renderer is initialized with the correct backend.
    if (DarkModeHelper::IsWindows11OrGreater()) {
        SciterSetOption(nullptr, SCITER_SET_GFX_LAYER, GFX_LAYER_D2D);
    } else {
        // Win10 needs SKIA to fix alpha channel issues with DWM
        SciterSetOption(nullptr, SCITER_SET_GFX_LAYER, GFX_LAYER_SKIA);
    }

    sciter::application::start();

    // CRITICAL: Disable DirectComposition on Win11 when using D2D to avoid black/blank window issues.
    // Must be set after start() (window system must be initialized).
    if (DarkModeHelper::IsWindows11OrGreater()) {
        SciterSetOption(nullptr, SCITER_SET_UX_THEMING, TRUE);
    }

    SciterSetOption(nullptr, SCITER_SET_SCRIPT_RUNTIME_FEATURES,
        ALLOW_FILE_IO | ALLOW_SOCKET_IO | ALLOW_EVAL | ALLOW_SYSINFO);
    BindSciterResources();
}

/// Spawn a subprocess (single-instance by window title).
/// If a window with the given title already exists, brings it to front.
/// Tracks the process handle for cleanup via TerminateAllSubprocesses().
inline void SpawnSubprocess(const wchar_t* windowTitle, const wchar_t* cliArgs) {
    using ProcessHandle = std::unique_ptr<void, decltype(&CloseHandle)>;
    static std::unordered_map<std::wstring, ProcessHandle> pending;
    if (HWND existing = FindWindowW(nullptr, windowTitle)) {
        pending.erase(windowTitle);
        FocusExistingWindow(existing);
        return;
    }

    // Keep one startup in flight until its window appears or its process exits.
    // A slow disk/first launch may take longer than a fixed debounce interval.
    if (auto it = pending.find(windowTitle); it != pending.end()) {
        if (WaitForSingleObject(it->second.get(), 0) == WAIT_TIMEOUT) return;
        pending.erase(it);
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 128];
    swprintf_s(cmdLine, L"\"%s\" %s", exePath, cliArgs);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        HANDLE startupHandle = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(),
                            &startupHandle, SYNCHRONIZE, FALSE, 0)) {
            pending.emplace(windowTitle, ProcessHandle(startupHandle, &CloseHandle));
        }
        TrackChildProcess(pi.hProcess);  // Track for cleanup, don't close
    }
}

}  // namespace NextKey
