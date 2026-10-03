// VKey - TSF Registration & Diagnostics
// SPDX-License-Identifier: GPL-3.0-only

// Windows/COM headers MUST come first — <msctf.h> includes <comcat.h>
// which requires COM base types from <ole2.h>/<objbase.h>
#include <Windows.h>
#include <ole2.h>
#include <msctf.h>

#include "TsfRegistration.h"
#include "core/ipc/SharedState.h"
#include "core/ipc/SharedStateManager.h"
#include "core/Debug.h"
#include "core/security/ExtensionTrust.h"
#include "tsf/Globals.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace NextKey {

using DllRegisterServerFn = HRESULT(STDAPICALLTYPE*)();

namespace {

// "0x0409:{CLSID}{profile GUID}" — must match RegisterTIP()/Globals and the
// CLSID_NK / GUID_NK_Profile GUIDs in ActivateVKeyTsfProfile() below. Shared
// by ActivateVKeyTsfProfile (install) and RemoveVKeyTsfFromInputList
// (uninstall) so the two can't drift apart.
constexpr wchar_t kVKeyTipId[] =
    L"0x0409:{DEB18BD1-2331-4F2A-B030-DA9EB0093683}"
    L"{2FE17DA4-D8E2-4B28-8566-C30E8F04BFD4}";

// Not defined in a public header (see InstallLayoutOrTip docs) — same as
// ILOT_DISABLED, removes the layout/TIP from the user's enabled input list.
constexpr DWORD kIlotUninstall = 0x00000001;

using InstallLayoutOrTipFn = BOOL(WINAPI*)(LPCWSTR, DWORD);

}  // namespace

std::wstring GetTsfDllPath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        path = path.substr(0, pos + 1);
    }
    return path + L"VKeyTSF.dll";
}

bool IsTsfRegistered() noexcept {
    // 1. Check if the CLSID key exists
    std::wstring clsidPath = L"CLSID\\";
    clsidPath += TSF::CLSID_TEXTSERVICE_STRING;

    HKEY hKeyClsid = nullptr;
    LSTATUS lsClsid = RegOpenKeyExW(HKEY_CLASSES_ROOT, clsidPath.c_str(), 0, KEY_READ, &hKeyClsid);
    if (lsClsid != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(hKeyClsid);

    // 2. Check if the profile is registered under the correct language (0x0409)
    // The profile key is Software\Microsoft\CTF\TIP\{CLSID}\LanguageProfile\0x00000409\{ProfileGUID}
    std::wstring profilePath = L"Software\\Microsoft\\CTF\\TIP\\";
    profilePath += TSF::CLSID_TEXTSERVICE_STRING;
    profilePath += L"\\LanguageProfile\\0x00000409\\";
    profilePath += TSF::GUID_PROFILE_STRING;

    // Check HKEY_LOCAL_MACHINE
    HKEY hKeyProfile = nullptr;
    LSTATUS lsProfile = RegOpenKeyExW(HKEY_LOCAL_MACHINE, profilePath.c_str(), 0, KEY_READ, &hKeyProfile);
    if (lsProfile == ERROR_SUCCESS) {
        RegCloseKey(hKeyProfile);
        return true;
    }

    // Check HKEY_CURRENT_USER
    lsProfile = RegOpenKeyExW(HKEY_CURRENT_USER, profilePath.c_str(), 0, KEY_READ, &hKeyProfile);
    if (lsProfile == ERROR_SUCCESS) {
        RegCloseKey(hKeyProfile);
        return true;
    }

    return false;
}

bool RegisterTsf() {
    std::wstring dllPath = GetTsfDllPath();
    OutputDebugStringW((L"RegisterTsf: DLL path = " + dllPath + L"\n").c_str());

    if (!Security::IsExtensionTrusted(dllPath, Security::ExtensionBinary::TsfDll)) {
        NEXTKEY_LOG(L"RegisterTsf: VKeyTSF.dll failed integrity verification");
        return false;
    }

    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) {
        wchar_t buf[128];
        swprintf_s(buf, L"RegisterTsf: LoadLibrary failed, error=%lu\n", GetLastError());
        OutputDebugStringW(buf);
        return false;
    }

    auto pRegister = reinterpret_cast<DllRegisterServerFn>(
        GetProcAddress(hDll, "DllRegisterServer")
    );

    bool success = false;
    if (pRegister) {
        HRESULT hr = pRegister();
        success = SUCCEEDED(hr);
        wchar_t buf[128];
        swprintf_s(buf, L"RegisterTsf: DllRegisterServer hr=0x%08X\n", hr);
        OutputDebugStringW(buf);
    } else {
        OutputDebugStringW(L"RegisterTsf: DllRegisterServer export not found\n");
    }

    FreeLibrary(hDll);
    return success;
}

bool UnregisterTsf() {
    std::wstring dllPath = GetTsfDllPath();

    if (!Security::IsExtensionTrusted(dllPath, Security::ExtensionBinary::TsfDll)) {
        NEXTKEY_LOG(L"UnregisterTsf: refusing to load untrusted VKeyTSF.dll");
        RemoveVKeyTsfFromInputList();
        return false;
    }

    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) {
        OutputDebugStringW(L"UnregisterTsf: LoadLibrary failed\n");
        return false;
    }

    auto pUnregister = reinterpret_cast<DllRegisterServerFn>(
        GetProcAddress(hDll, "DllUnregisterServer")
    );

    if (pUnregister) {
        pUnregister();
    }

    FreeLibrary(hDll);

    // DllUnregisterServer only removes the CLSID/profile registration — it
    // never touches the user's enabled-input-list entry that
    // ActivateVKeyTsfProfile's InstallLayoutOrTip added. Without this, VKey
    // stays selectable (and re-selectable via Win+Space / the OS input-switch
    // hotkey) as a Windows input method even after the user turns TSF off.
    RemoveVKeyTsfFromInputList();

    // Don't trust DllUnregisterServer return value — it always returns S_OK.
    // Check actual registry state instead.
    bool gone = !IsTsfRegistered();
    OutputDebugStringW(gone
        ? L"UnregisterTsf: succeeded\n"
        : L"UnregisterTsf: CLSID still in registry (needs elevation)\n");
    return gone;
}

bool RegisterTsfElevated() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    OutputDebugStringW(L"RegisterTsfElevated: requesting elevation\n");

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = L"--register-tsf";
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 10000);
            CloseHandle(sei.hProcess);
        }
        bool registered = IsTsfRegistered();
        OutputDebugStringW(registered
            ? L"RegisterTsfElevated: succeeded\n"
            : L"RegisterTsfElevated: failed\n");
        return registered;
    }
    wchar_t buf[128];
    swprintf_s(buf, L"RegisterTsfElevated: ShellExecuteEx failed, error=%lu\n", GetLastError());
    OutputDebugStringW(buf);
    return false;
}

bool UnregisterTsfElevated() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = L"--unregister-tsf";
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 10000);
            CloseHandle(sei.hProcess);
        }
        return !IsTsfRegistered();
    }
    return false;
}

void RunDiagnostics() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring out;
    out += L"=== VKey Diagnostics ===\n\n";

    // 1. TSF Registration check
    out += IsTsfRegistered() ? L"[OK] TSF registered\n" : L"[FAIL] TSF NOT registered\n";

    // 2. Enumerate all keyboard layouts (HKLs)
    out += L"\n--- Keyboard Layouts (HKLs) ---\n";
    int count = GetKeyboardLayoutList(0, nullptr);
    if (count > 0) {
        std::vector<HKL> hklList(count);
        GetKeyboardLayoutList(count, hklList.data());
        for (int i = 0; i < count; i++) {
            auto hklVal = reinterpret_cast<DWORD_PTR>(hklList[i]);
            bool isTip = (HIWORD(hklVal) >= 0xF000);
            wchar_t buf[128];
            swprintf_s(buf, L"  HKL[%d] = 0x%08IX  LOWORD=0x%04X  HIWORD=0x%04X  %s\n",
                        i, hklVal, LOWORD(hklVal), HIWORD(hklVal),
                        isTip ? L"(TIP substitute)" : L"(keyboard layout)");
            out += buf;
        }
    } else {
        out += L"  (none found)\n";
    }

    // 3. Current thread HKL
    {
        HKL cur = GetKeyboardLayout(0);
        wchar_t buf[128];
        swprintf_s(buf, L"\nCurrent thread HKL: 0x%08IX\n",
                    reinterpret_cast<DWORD_PTR>(cur));
        out += buf;
    }

    // 4. Foreground thread HKL
    {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD tid = GetWindowThreadProcessId(fg, nullptr);
            HKL fgHkl = GetKeyboardLayout(tid);
            wchar_t buf[128];
            swprintf_s(buf, L"Foreground thread HKL: 0x%08IX (tid=%lu)\n",
                        reinterpret_cast<DWORD_PTR>(fgHkl), tid);
            out += buf;
        }
    }

    // 5. TSF Active Profile
    // RAII guard for COM pointers (ATL/CComPtr not available in EXE build)
    {
        auto comRelease = [](IUnknown* p) { if (p) p->Release(); };

        out += L"\n--- TSF Active Profile ---\n";
        ITfInputProcessorProfileMgr* pProfileMgrRaw = nullptr;
        HRESULT hr = CoCreateInstance(
            CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfileMgr,
            reinterpret_cast<void**>(&pProfileMgrRaw));
        std::unique_ptr<ITfInputProcessorProfileMgr, decltype(comRelease)>
            pProfileMgr(SUCCEEDED(hr) ? pProfileMgrRaw : nullptr, comRelease);

        if (pProfileMgr) {
            TF_INPUTPROCESSORPROFILE activeProfile = {};
            hr = pProfileMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &activeProfile);
            if (SUCCEEDED(hr)) {
                wchar_t buf[256];
                swprintf_s(buf, L"  Type=%lu  LangID=0x%04X  HKL=0x%08IX\n",
                            activeProfile.dwProfileType, activeProfile.langid,
                            reinterpret_cast<DWORD_PTR>(activeProfile.hkl));
                out += buf;

                // Check CLSID
                wchar_t clsidStr[64];
                StringFromGUID2(activeProfile.clsid, clsidStr, 64);
                out += L"  CLSID=";
                out += clsidStr;
                out += L"\n";

                // VKey CLSID for comparison
                static const GUID CLSID_NK = {
                    0xDEB18BD1, 0x2331, 0x4F2A,
                    {0xB0, 0x30, 0xDA, 0x9E, 0xB0, 0x09, 0x36, 0x83}
                };
                out += IsEqualCLSID(activeProfile.clsid, CLSID_NK)
                    ? L"  → This IS VKey\n"
                    : L"  → This is NOT VKey\n";
            } else {
                out += L"  GetActiveProfile failed\n";
            }

            // 6. Enumerate all profiles for VKey's registered langid (English-US 0x0409)
            out += L"\n--- VKey LangID Profiles ---\n";
            IEnumTfInputProcessorProfiles* pEnumRaw = nullptr;
            hr = pProfileMgr->EnumProfiles(TSF::TEXTSERVICE_LANGID, &pEnumRaw);
            std::unique_ptr<IEnumTfInputProcessorProfiles, decltype(comRelease)>
                pEnum(SUCCEEDED(hr) ? pEnumRaw : nullptr, comRelease);

            if (pEnum) {
                TF_INPUTPROCESSORPROFILE profile;
                ULONG fetched = 0;
                int idx = 0;
                while (pEnum->Next(1, &profile, &fetched) == S_OK && fetched == 1) {
                    wchar_t clsidStr2[64];
                    StringFromGUID2(profile.clsid, clsidStr2, 64);
                    wchar_t buf2[256];
                    swprintf_s(buf2, L"  [%d] type=%lu  hkl=0x%08IX  clsid=%s\n",
                                idx++, profile.dwProfileType,
                                reinterpret_cast<DWORD_PTR>(profile.hkl), clsidStr2);
                    out += buf2;
                }
            }
        } else {
            out += L"  Failed to create ITfInputProcessorProfileMgr\n";
        }
    }

    // 7. SharedState check
    out += L"\n--- SharedState ---\n";
    {
        SharedStateManager sm;
        if (sm.Open()) {
            SharedState state = sm.Read();
            if (state.IsValid()) {
                wchar_t buf[256];
                swprintf_s(buf, L"  magic=0x%08X  epoch=%u  flags=0x%08X\n"
                                L"  VIETNAMESE_MODE=%d  ENGINE_ENABLED=%d\n"
                                L"  inputMethod=%d  spellCheck=%d\n",
                            state.magic, state.epoch, state.flags,
                            (state.flags & SharedFlags::VIETNAMESE_MODE) ? 1 : 0,
                            (state.flags & SharedFlags::ENGINE_ENABLED) ? 1 : 0,
                            state.inputMethod, state.spellCheck);
                out += buf;
            } else {
                out += L"  SharedState invalid (magic mismatch)\n";
            }
        } else {
            out += L"  SharedState not available (EXE not running?)\n";
        }
    }

    CoUninitialize();
    MessageBoxW(nullptr, out.c_str(), L"VKey Diagnostics", MB_OK | MB_ICONINFORMATION);
}

void CleanupHkcuClsidOverride() noexcept {
    wchar_t keyPath[256];
    swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", NextKey::TSF::CLSID_TEXTSERVICE_STRING);

    // Only clean up HKCU override if the TSF DLL is registered in HKLM
    HKEY hKeyHklm = nullptr;
    LSTATUS lsHklm = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_READ, &hKeyHklm);
    if (lsHklm == ERROR_SUCCESS) {
        RegCloseKey(hKeyHklm);

        LSTATUS ls = RegDeleteTreeW(HKEY_CURRENT_USER, keyPath);
        if (ls == ERROR_SUCCESS) {
            NEXTKEY_LOG(L"[TsfRegistration] Removed HKCU CLSID override for VKey TSF");
        } else if (ls != ERROR_FILE_NOT_FOUND) {
            NEXTKEY_LOG(L"[TsfRegistration] Warning: could not remove HKCU CLSID override (error=%ld)", ls);
        }
    }
}

void RemoveVKeyTsfFromInputList() noexcept {
    if (HMODULE hInput = ::LoadLibraryExW(L"input.dll", nullptr,
                                          LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (auto pInstall = reinterpret_cast<InstallLayoutOrTipFn>(
                ::GetProcAddress(hInput, "InstallLayoutOrTip"))) {
            // No-op if the entry was never added (e.g. user turned tsf_apps
            // off without ever having it selected) — InstallLayoutOrTip
            // simply returns FALSE for an absent entry, nothing to clean up.
            const BOOL removed = pInstall(kVKeyTipId, kIlotUninstall);
            NEXTKEY_LOG(L"[TsfRegistration] InstallLayoutOrTip(uninstall) -> %d", removed ? 1 : 0);
        }
        ::FreeLibrary(hInput);
    }
}

bool ActivateVKeyTsfProfile() {
    // Throttle (issue #209): WM_VKEY_ACTIVATE_TSF is posted on every toggle-to-V
    // in a TSF app, so holding/mashing Ctrl+Shift posted it dozens of times a
    // second — each doing a full CoCreateInstance + ActivateProfile COM round
    // trip. When the profile cannot activate (e.g. a non-admin session that
    // cannot session-activate the VIE TIP, hr=0x80004005) the failing call was
    // retried on every keystroke, flooding the tray message thread ("not
    // responding"). Back off: a failed activation cools down 2s, a successful
    // one 200ms (re-activation while already active is a no-op for TSF anyway).
    // Only hr == S_OK counts as success — see the S_FALSE note at the return.
    // GetActiveProfile would itself need COM init, so a tick-based gate is the
    // cheap guard that runs before any COM call.
    static std::atomic<ULONGLONG> lastAttemptTick{0};
    static std::atomic<bool> lastResult{false};
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG prev = lastAttemptTick.load(std::memory_order_acquire);
    const ULONGLONG cooldownMs = lastResult.load(std::memory_order_acquire) ? 200ULL : 2000ULL;
    if (prev != 0 && now - prev < cooldownMs) {
        return lastResult.load(std::memory_order_acquire);
    }
    lastAttemptTick.store(now, std::memory_order_release);

    // #250 instrumentation: this per-focus re-assertion is the leading suspect
    // for the visible Alt+Tab / Start-menu stall in TSF mode, but nothing has
    // measured WHICH of its four stages costs the 55-90 ms seen in reporter
    // logs — and the Start-menu symptom persisted after the reporter removed
    // StartMenuExperienceHost.exe from tsf_apps, which this path should not
    // even run for. Time each stage so a user-supplied debug log decides it
    // instead of another guess. steady_clock, not GetTickCount64: the latter's
    // 15.6 ms resolution cannot resolve stages inside a 55 ms budget.
    // ponytail: plain per-stage timestamps, not a PerfHistogram stage — delete
    // this block once #250 is attributed rather than growing it into telemetry.
    const auto stageClockStart = std::chrono::steady_clock::now();
    auto elapsedUs = [](std::chrono::steady_clock::time_point from,
                        std::chrono::steady_clock::time_point to) {
        return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
    };
    long long registryUs = 0;
    long long installUs = 0;
    long long comInitUs = 0;
    long long activateUs = 0;

    // Never install/activate an unregistered TIP: if the DLL registration was
    // removed externally (regsvr32 /u, failed update) while the TOML tsf_apps
    // flag stayed on, InstallLayoutOrTip below would add a phantom input-list
    // entry for a dead CLSID. Fail fast; the 2s fail cooldown keeps the
    // per-focus retries cheap (one registry read).
    const bool registered = IsTsfRegistered();
    const auto afterRegistry = std::chrono::steady_clock::now();
    registryUs = elapsedUs(stageClockStart, afterRegistry);
    if (!registered) {
        NEXTKEY_LOG(L"[TsfRegistration] ActivateVKeyTsfProfile skipped: TSF not "
                    L"registered (registry read %lldus)", registryUs);
        lastResult.store(false, std::memory_order_release);
        return false;
    }

    if (!Security::IsExtensionTrusted(GetTsfDllPath(), Security::ExtensionBinary::TsfDll)) {
        NEXTKEY_LOG(L"[TsfRegistration] ActivateVKeyTsfProfile skipped: untrusted VKeyTSF.dll");
        lastResult.store(false, std::memory_order_release);
        return false;
    }

    // #109/#209: ensure VKey's TIP is in the user's ENABLED input list (HKCU,
    // per-user, NO admin) so Windows can actually select it. Registration
    // (RegisterProfile + EnableLanguageProfile) only makes the TIP *available*;
    // it does not add it to Control Panel\International\User Profile. Without
    // this, after the old reset script purged that list (or on a fresh profile)
    // the TIP existed but was UNSELECTABLE → Windows stuck on US Keyboard and the
    // V/E toggle was dead. InstallLayoutOrTip is exported by input.dll with no
    // import lib, so load it dynamically. Idempotent — re-adding is a no-op.
    {
        if (HMODULE hInput = ::LoadLibraryExW(L"input.dll", nullptr,
                                              LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            if (auto pInstall = reinterpret_cast<InstallLayoutOrTipFn>(
                    ::GetProcAddress(hInput, "InstallLayoutOrTip"))) {
                // flags = 0: install + enable for the current user (adds it to the
                // input list). Session SELECTION is done by ActivateProfile below.
                const BOOL added = pInstall(kVKeyTipId, 0);
                NEXTKEY_LOG(L"[TsfRegistration] InstallLayoutOrTip -> %d", added ? 1 : 0);
            } else {
                NEXTKEY_LOG(L"[TsfRegistration] InstallLayoutOrTip missing from input.dll");
            }
            ::FreeLibrary(hInput);
        }
        installUs = elapsedUs(afterRegistry, std::chrono::steady_clock::now());
    }

    const auto afterInstall = std::chrono::steady_clock::now();

    // RAII COM lifetime: pairs S_OK/S_FALSE with CoUninitialize and, crucially, does
    // NOT call CoUninitialize when CoInitializeEx failed (e.g. RPC_E_CHANGED_MODE when
    // the calling GUI thread was already initialized with a different apartment model).
    // An unconditional CoUninitialize on the failure path would over-decrement and tear
    // down COM on that thread. Declared first so it destructs LAST — after the COM
    // interface unique_ptr below releases. Mirrors StartupHelper.h ComGuard.
    struct ComGuard {
        bool owned;
        ComGuard() noexcept {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            owned = (hr == S_OK || hr == S_FALSE);
        }
        ~ComGuard() noexcept { if (owned) CoUninitialize(); }
        ComGuard(const ComGuard&) = delete;
        ComGuard& operator=(const ComGuard&) = delete;
    } comGuard;

    auto comRelease = [](IUnknown* p) { if (p) p->Release(); };

    ITfInputProcessorProfileMgr* pProfileMgrRaw = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr,
        reinterpret_cast<void**>(&pProfileMgrRaw));

    std::unique_ptr<ITfInputProcessorProfileMgr, decltype(comRelease)>
        pProfileMgr(SUCCEEDED(hr) ? pProfileMgrRaw : nullptr, comRelease);

    const auto afterComInit = std::chrono::steady_clock::now();
    comInitUs = elapsedUs(afterInstall, afterComInit);
    if (!pProfileMgr) {
        NEXTKEY_LOG(L"[TsfRegistration] CoCreateInstance failed for "
                    L"ITfInputProcessorProfileMgr (hr=0x%08X, registry=%lldus, "
                    L"install=%lldus, com=%lldus)",
                    hr, registryUs, installUs, comInitUs);
        lastResult.store(false, std::memory_order_release);
        return false;
    }

    static const GUID CLSID_NK = {
        0xDEB18BD1, 0x2331, 0x4F2A,
        {0xB0, 0x30, 0xDA, 0x9E, 0xB0, 0x09, 0x36, 0x83}
    };
    static const GUID GUID_NK_Profile = {
        0x2FE17DA4, 0xD8E2, 0x4B28,
        {0x85, 0x66, 0xC3, 0x0E, 0x8F, 0x04, 0xBF, 0xD4}
    };

    hr = pProfileMgr->ActivateProfile(
        TF_PROFILETYPE_INPUTPROCESSOR,
        TSF::TEXTSERVICE_LANGID,  // English-US (0x0409) — must match RegisterTIP()'s langid
        CLSID_NK,
        GUID_NK_Profile,
        nullptr,
        TF_IPPMF_FORSESSION
    );
    activateUs = elapsedUs(afterComInit, std::chrono::steady_clock::now());

    if (FAILED(hr)) {
        NEXTKEY_LOG(L"[TsfRegistration] ActivateProfile failed for VKey TSF profile (hr=0x%08X)", hr);
    } else if (hr != S_OK) {
        // S_FALSE = "language profile is not enabled": the call returned without
        // selecting the TIP. Logged separately because it is indistinguishable
        // from success in the old log, which is exactly the state a #109-style
        // report ("VKey does nothing") needs to show.
        NEXTKEY_LOG(L"[TsfRegistration] ActivateProfile returned S_FALSE — VKey TSF "
                    L"profile is not enabled, the TIP will not receive keys");
    }

    // pProfileMgr (Release) then comGuard (CoUninitialize) destruct here, in that order.
    //
    // S_FALSE is NOT success: the profile is not enabled and the TIP will not
    // receive keys, so the caller is in exactly the #109 state the activation
    // exists to repair. Counting it as success picked the 200 ms success
    // cooldown, which re-attempted a call that cannot work five times a second
    // and reported a dead TIP as a live one. It now takes the 2 s failure
    // cooldown like every other non-working outcome.
    const bool ok = (hr == S_OK);
    NEXTKEY_LOG(L"[TsfRegistration] ActivateVKeyTsfProfile hr=0x%08X "
                L"registry=%lldus install=%lldus com=%lldus activate=%lldus "
                L"total=%lldus",
                hr, registryUs, installUs, comInitUs, activateUs,
                elapsedUs(stageClockStart, std::chrono::steady_clock::now()));
    lastResult.store(ok, std::memory_order_release);
    return ok;
}

}  // namespace NextKey
