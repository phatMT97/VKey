// VKey - Focus Owner Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "FocusOwner.h"
#include "PerfHistogram.h"  // PERF_SCOPE
#include "helpers/AppHelpers.h"  // ToLowerAscii, GetFocusedChildHwnd
#include "core/AutoCapDecision.h"
#include "core/CrashLog.h"
#include "core/Debug.h"
#include "core/FocusTargetDecision.h"
#include "core/Logger.h"
#include "core/PerAppModeDecision.h"
#include "core/WebView2CacheDecision.h"

#include <tlhelp32.h>
#include <cstdint>
#include <iterator>
#include <utility>

namespace NextKey {

// Same prefix-only pattern as HookEngine.cpp / HookLifecycle.cpp: arg side
// effects are not evaluated when the runtime logger is off.
#define FOCUS_LOG(fmt, ...) do {                                              \
    if (::NextKey::Logger::IsEnabled())                                       \
        ::NextKey::Logger::Log(L"[Hook] " fmt, ##__VA_ARGS__);                \
} while (0)

std::atomic<FocusOwner*> FocusOwner::s_instance{nullptr};

// Per-app "send method = compatibility split" sleep budgets (ms). Resolved
// from AppOverrideEntry::sendMethod in Classify and carried in
// FocusClassification::localForcedSplitSleepMs → WindowClassification.
//   sendMethod 2 — Firefox-family / local Gecko renderer drain (~one frame).
//   sendMethod 3 — cloud / remote desktop: the gap must outlast the RDP/Citrix
//                  round-trip so the BS batch lands before the char batch
//                  (issue #178 — typing over cloud desktop drops/misplaces tones).
static constexpr int kCompatSplitFirefoxMs = 6;
static constexpr int kCompatSplitRemoteMs  = 25;
static constexpr int kCompatSplitWebViewMs = 6;
static constexpr UINT kControlProbeTimeoutMs = 20;
static constexpr UINT kSciGetLength = 2006;
static constexpr UINT kSciGetCurrentPos = 2008;

// ─────────────────────────────────────────────────────────────────────────
// File-scope helpers (moved from HookEngine.cpp). Kept anonymous-namespace-
// free for symmetry with the originals and to keep diffs small if a future
// PR needs to reorder them.
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] static bool TrySendControlQuery(HWND control,
                                               UINT message,
                                               DWORD_PTR& result) noexcept {
    result = 0;
    return SendMessageTimeoutW(
        control, message, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
        kControlProbeTimeoutMs, &result) != 0;
}

/// Query document length + caret only for controls with documented scalar
/// messages. This runs on the worker focus-classification path, never inside
/// LowLevelKeyboardProc. Unsupported/custom controls fail closed. `focused`
/// is resolved once by the caller (Classify) and shared with
/// IsFocusedControlPassword to avoid two AttachThreadInput round-trips.
[[nodiscard]] static bool IsKnownEmptyFocusedDocument(HWND foreground, HWND focused) noexcept {
    if (foreground == nullptr || GetForegroundWindow() != foreground) return false;
    if (focused == nullptr || GetAncestor(focused, GA_ROOT) != foreground) return false;

    wchar_t className[64] = {};
    if (GetClassNameW(focused, className, static_cast<int>(std::size(className))) <= 0) {
        return false;
    }

    AutoCapControlProbe probe{};
    DWORD_PTR textLength = 0;
    DWORD_PTR caretPosition = 0;
    UINT lengthMessage = 0;
    UINT caretMessage = 0;
    if (_wcsicmp(className, L"Edit") == 0
        || _wcsnicmp(className, L"RichEdit", 8) == 0) {
        if ((GetWindowLongPtrW(focused, GWL_STYLE) & ES_PASSWORD) != 0) {
            return false;
        }
        probe.isSupportedControl = true;
        lengthMessage = WM_GETTEXTLENGTH;
        caretMessage = EM_GETSEL;
    } else if (wcsstr(className, L"Scintilla") != nullptr) {
        probe.isSupportedControl = true;
        lengthMessage = kSciGetLength;
        caretMessage = kSciGetCurrentPos;
    }

    if (!probe.isSupportedControl) return false;
    probe.textLengthKnown = TrySendControlQuery(
        focused, lengthMessage, textLength);
    probe.textLength = static_cast<std::uint64_t>(textLength);
    if (!probe.textLengthKnown || probe.textLength != 0) return false;

    probe.caretPositionKnown = TrySendControlQuery(
        focused, caretMessage, caretPosition);
    probe.caretPosition = static_cast<std::uint64_t>(caretPosition);
    return GetForegroundWindow() == foreground
        && ComputeShouldAutoCapFromControlProbe(probe);
}

// IsFocusedControlPassword moved to helpers/AppHelpers.h — HandleAlphaKey
// (HookEngine.cpp) needs the same cheap check for a fresh, synchronous
// re-validation right before auto-capitalizing (see its call site comment).

/// Check if a filename (without path) is a known Electron app executable.
/// Electron apps use Chrome_WidgetWin window class (same as Chromium browsers).
/// Unknown Chrome_WidgetWin apps default to "browser" — safer because:
///   - Browser miss → double text (visible, user reports immediately)
///   - Electron miss → slightly slower input (split delay absent, usually OK)
/// This list covers the most popular Electron apps. Add new ones as needed.
static bool IsKnownElectronExe(const wchar_t* filename) noexcept {
    return _wcsnicmp(filename, L"code", 4) == 0 ||       // VS Code
           _wcsnicmp(filename, L"cursor", 6) == 0 ||     // Cursor (AI code editor)
           _wcsnicmp(filename, L"discord", 7) == 0 ||    // Discord
           _wcsnicmp(filename, L"slack", 5) == 0 ||      // Slack
           _wcsnicmp(filename, L"notion", 6) == 0 ||     // Notion
           _wcsnicmp(filename, L"obsidian", 8) == 0 ||   // Obsidian
           _wcsnicmp(filename, L"figma", 5) == 0 ||      // Figma
           _wcsnicmp(filename, L"postman", 7) == 0 ||    // Postman
           _wcsnicmp(filename, L"insomnia", 8) == 0 ||   // Insomnia
           _wcsnicmp(filename, L"signal", 6) == 0 ||     // Signal
           _wcsnicmp(filename, L"1password", 9) == 0 ||  // 1Password
           _wcsnicmp(filename, L"bitwarden", 9) == 0 ||  // Bitwarden
           _wcsnicmp(filename, L"gitkraken", 9) == 0 ||  // GitKraken
           _wcsnicmp(filename, L"hyper", 5) == 0 ||      // Hyper terminal
           _wcsnicmp(filename, L"spotify", 7) == 0 ||    // Spotify
           _wcsnicmp(filename, L"whatsapp", 8) == 0 ||   // WhatsApp Desktop
           _wcsnicmp(filename, L"telegram", 8) == 0 ||   // Telegram (some forks are Electron; native is Qt, caught earlier)
           _wcsnicmp(filename, L"logseq", 6) == 0 ||     // Logseq
           _wcsnicmp(filename, L"linear", 6) == 0 ||     // Linear
           _wcsnicmp(filename, L"lark", 4) == 0 ||       // Lark/Feishu
           _wcsnicmp(filename, L"legcord", 7) == 0 ||    // Legcord
           _wcsnicmp(filename, L"vesktop", 7) == 0 ||    // Vesktop
           _wcsnicmp(filename, L"zalo", 4) == 0;         // Zalo PC
}

/// Returns the full exe path (original case) for the process owning `hwnd`.
/// Empty on failure. Needed by IsWebView2App() to locate sibling DLLs.
[[nodiscard]] static std::wstring GetExeFullPathForHwnd(HWND hwnd) noexcept {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        result.assign(exePath, size);
    }
    CloseHandle(hProc);
    return result;
}

/// Classify window into app type — called from Classify().
/// Reads window class ONCE and determines: Browser, Electron, Qt, Console, or Normal.
/// Results are written directly to the caller's output variables.
///
/// Priority order:
///   1. Console (by window class: ConsoleWindowClass, CASCADIA, mintty, PuTTY)
///   2. Firefox-based browser (by window class: MozillaWindowClass)
///   3. Chrome_WidgetWin → Known Electron list or default to browser
///   4. Qt app (by window class: Qt5*, Qt6*, QWidget)
///   5. VB6 app (by window class: ThunderRT6*) — needs clipboard paste
///   6. Normal Win32 app
static void ClassifyWindow(HWND hwnd,
                           bool& outIsBrowser,
                           bool& outIsElectron,
                           bool& outIsQtApp,
                           bool& outIsConsole,
                           bool& outIsVB6) noexcept {
    outIsBrowser = outIsElectron = outIsQtApp = outIsConsole = outIsVB6 = false;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;

    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, 64);

    // Console includes modern Windows Terminal. Cascadia's renderer accepts a
    // SendInput batch, but ConPTY-backed TUIs such as Codex/Claude can still
    // reorder physical WM_KEYDOWN against synthetic VK_PACKET input.
    if (_wcsicmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0 ||
        _wcsicmp(className, L"ConsoleWindowClass") == 0 ||
        _wcsicmp(className, L"tty") == 0 ||                            // Cygwin/MSYS
        _wcsicmp(className, L"mintty") == 0 ||                         // Git Bash
        _wcsicmp(className, L"PuTTY") == 0) {
        outIsConsole = true;
        return;
    }

    // 2. Firefox-based browsers (covers Firefox, Floorp, Tor, LibreWolf, Waterfox, Pale Moon).
    //    Classified as a browser so the bait-char prefix applies. Gecko's content
    //    process can re-render React contenteditable mid-batch and drop the trailing
    //    VK_PACKET char (Mattermost-style chat race) — users hit by that enable the
    //    per-app "Tương thích Firefox" send-method override (split dispatch).
    if (_wcsicmp(className, L"MozillaWindowClass") == 0) {
        outIsBrowser = true;
        return;
    }

    // 3. Chrome_WidgetWin: Chromium browser OR Electron app
    //    Disambiguate by known Electron exe list. Unknown → browser (safer default).
    if (wcsstr(className, L"Chrome_WidgetWin")) {
        std::wstring exeName = FocusOwner::GetExeNameForHwnd(hwnd);
        if (!exeName.empty() && IsKnownElectronExe(exeName.c_str())) {
            outIsElectron = true;
        } else {
            outIsBrowser = true;  // Unknown Chrome_WidgetWin → assume browser
        }
        return;
    }

    // 4. Qt apps (Telegram native, KeePassXC, etc.)
    if (wcsstr(className, L"Qt5") || wcsstr(className, L"Qt6") ||
        wcsstr(className, L"QWidget")) {
        outIsQtApp = true;
        return;
    }

    // 5. VB6 apps (XYplorer, etc.): register Unicode window classes but process
    //    messages as ANSI internally — KEYEVENTF_UNICODE / VK_PACKET chars become '?'.
    if (_wcsnicmp(className, L"ThunderRT6", 10) == 0) {
        outIsVB6 = true;
        return;
    }

    // 6. Normal Win32 app (Notepad, Word, etc.) — no flags set
}

// ─────────────────────────────────────────────────────────────────────────
// FocusOwner — public interface
// ─────────────────────────────────────────────────────────────────────────

FocusOwner::FocusOwner() = default;

FocusOwner::~FocusOwner() {
    Uninstall();
}

bool FocusOwner::Install(FocusChangedFn onFocusChanged) {
    if (focusHook_) return true;  // already installed — idempotent

    onFocusChanged_ = std::move(onFocusChanged);
    s_instance.store(this, std::memory_order_release);

    // Two separate hooks for exact event targeting (avoids receiving ~20
    // unrelated events in the 0x0003..0x0017 range).
    focusHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // MINIMIZEEND: restoring a window from the taskbar may not fire FOREGROUND
    // (taskbar gets the foreground event, filtered as Shell_TrayWnd).
    minimizeHook_ = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND,
        nullptr, WinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!focusHook_) {
        s_instance.store(nullptr, std::memory_order_release);
        onFocusChanged_ = nullptr;
        if (minimizeHook_) {
            UnhookWinEvent(minimizeHook_);
            minimizeHook_ = nullptr;
        }
        return false;
    }
    return true;
}

void FocusOwner::Uninstall() {
    if (focusHook_) {
        UnhookWinEvent(focusHook_);
        focusHook_ = nullptr;
    }
    if (minimizeHook_) {
        UnhookWinEvent(minimizeHook_);
        minimizeHook_ = nullptr;
    }
    if (s_instance.load(std::memory_order_acquire) == this) {
        s_instance.store(nullptr, std::memory_order_release);
    }
    onFocusChanged_ = nullptr;
}

void CALLBACK FocusOwner::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                        LONG, LONG, DWORD, DWORD) {
    try {
        FocusOwner* self = s_instance.load(std::memory_order_relaxed);
        if (!self || !self->onFocusChanged_) return;

        // Per WINEVENT_OUTOFCONTEXT semantics, this callback fires on the
        // INSTALLER thread (main, where SetWinEventHook was called) — NOT
        // the hook thread. Wave 3 PR 3.6 (worker-thread doctrine §12.4):
        // onFocusChanged_ on this thread is produce-only — it latches the
        // trigger HWND + signals the worker. The full chain is then
        // `HookEngine::OnFocusChanged` (main) → latch + signal worker →
        // worker `DrainClassifyOnWorker` → `focus_.Classify` →
        // `Mailbox().Post` (worker → hook). State writes still land on the
        // hook thread via the mailbox; Classify itself now runs only on
        // the worker, restoring single-writer for the plain
        // appProfileCache_ + webView2PositiveCache_ containers.
        if (event == EVENT_SYSTEM_MINIMIZEEND) {
            // Window restored from taskbar — re-evaluate focus with the actual foreground window.
            // Don't use hwnd directly: the restored window may not be foreground yet.
            FOCUS_LOG(L"MINIMIZEEND (hwnd=%p) — re-evaluating focus", hwnd);
            self->onFocusChanged_(nullptr);  // nullptr → uses GetForegroundWindow()
            return;
        }

        FOCUS_LOG(L"FOCUS changed (hwnd=%p) — classifying + posting", hwnd);
        self->onFocusChanged_(hwnd);
    } catch (const std::exception& e) {
        CrashLog(L"FocusOwner::WinEventProc", e.what());
    } catch (...) {
        CrashLog(L"FocusOwner::WinEventProc", "(non-std exception)");
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Cache helpers (hook-thread only — same invariant as pre-PR-3.2: only
// Classify (main/worker) and ApplyFocusOnHookThread (hook) touch the maps.
// Pre-existing single-thread guarantee carried over verbatim.)
// ─────────────────────────────────────────────────────────────────────────

const FocusOwner::AppProfile* FocusOwner::LookupAppProfile(HWND hwnd) noexcept {
    auto it = appProfileCache_.find(hwnd);
    if (it == appProfileCache_.end()) return nullptr;

    // Validate: HWND values can be reused after the owning process dies.
    // GetWindowThreadProcessId is one cheap syscall; on hit it still saves
    // the much pricier ClassifyWindow + GetExeNameForHwnd + IsWebView2App
    // child-window walk that we'd otherwise rerun.
    DWORD currentPid = 0;
    GetWindowThreadProcessId(hwnd, &currentPid);
    if (currentPid == 0 || currentPid != it->second.pid) {
        appProfileCache_.erase(it);
        return nullptr;
    }
    return &it->second;
}

void FocusOwner::StoreAppProfile(HWND hwnd, AppProfile profile) noexcept {
    profile.cachedAt = GetTickCount64();

    // Bounded cache: LRU-evict the oldest entry when at capacity. O(N) scan
    // is fine — N is capped at kMaxAppProfileCache (64).
    if (appProfileCache_.size() >= kMaxAppProfileCache) {
        auto oldest = appProfileCache_.begin();
        for (auto it = std::next(appProfileCache_.begin());
             it != appProfileCache_.end(); ++it) {
            if (it->second.cachedAt < oldest->second.cachedAt) oldest = it;
        }
        appProfileCache_.erase(oldest);
    }

    appProfileCache_[hwnd] = std::move(profile);
}

bool FocusOwner::IsTrayOrTaskbarWindow(HWND hwnd) noexcept {
    if (!hwnd) return false;

    // Ignore focus switches to our own process (Settings, Menu, Tray)
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == GetCurrentProcessId()) {
        return true;
    }

    // GetAncestor is a no-op when hwnd is already a root (e.g. from GetForegroundWindow),
    // but needed when called with a child HWND (e.g. from WindowFromPoint).
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||            // main taskbar
           _wcsicmp(cls, L"TrayNotifyWnd") == 0 ||            // notification area
           _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0 ||  // overflow (^) Win 10
           _wcsicmp(cls, L"TopLevelWindowForOverflowTray") == 0 || // overflow (^) Win 11
           _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||   // secondary taskbar
           _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0 || // Win 11 tray popups (volume, network)
           _wcsicmp(cls, L"#32768") == 0 ||                   // standard popup menu (right-click tray apps)
           _wcsicmp(cls, L"MSTaskSwWClass") == 0 ||            // taskbar app buttons
           _wcsicmp(cls, L"Start") == 0 ||                     // Start button
           _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0 || // Start Menu / Action Center (Win 10/11)
           _wcsicmp(cls, L"VKeyTrayClass") == 0;           // VKey own tray window
           // Note: SetForegroundWindow(hwndMessage_) in ShowContextMenu fires
           // EVENT_SYSTEM_FOREGROUND synchronously, but WinEventProc is WINEVENT_OUTOFCONTEXT
           // so it's delivered asynchronously — this filter still catches it correctly.
}

std::wstring FocusOwner::GetExeNameForHwnd(HWND hwnd) noexcept {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        const wchar_t* filename = wcsrchr(exePath, L'\\');
        result = ToLowerAscii(filename ? filename + 1 : exePath);
    }
    CloseHandle(hProc);
    return result;
}

// ── Auto-detect WebView2 apps via process inspection ──────────────────
// Tauri apps (Dorion), Office WebView2 add-ins, and any Win32 app that
// hosts a WebView2 control needs the Electron input treatment (skip
// reinjectVk + split dispatch) — Chromium's untrusted-input filter drops
// the unpaired synthetic VK keydown under some conditions.
//
// Two-pass detection:
//   1. Module check: Win32 apps that load WebView2 inline into the host
//      process will have `WebView2Loader.dll` or `embeddedbrowserwebview.dll`
//      loaded. Cheap (~3ms) and catches most hybrid Win32 apps.
//   2. Child-process check: Tauri v2 + modern WebView2 runtime isolate
//      the browser into `msedgewebview2.exe` — spawned as a child of the
//      host. The host itself may not load any WebView2 DLL. Walk the
//      process table and look for a child with that exe name. Slower
//      (~5-10ms over ~200 processes), so we only run it as a fallback.
//
// Cache strategy: positive-only. Both checks race with WebView2 runtime
// initialization (Tauri delay-loads on first embed), so a false at app-
// launch time must not poison subsequent checks.
//
// Threading: `webView2PositiveCache_` is plain (not atomic). Called only
// from Classify, which post Wave 3 PR 3.6 runs exclusively on the
// MainThreadWorker thread per worker-thread doctrine §12.4
// (docs/CODING_RULES/12-worker-thread-doctrine.md): WinEventProc's main-
// thread handler is produce-only (latches HWND + signals worker), and
// OnTickPoll already runs on the worker. Single-writer = no race on the
// plain unordered_set.
bool FocusOwner::IsWebView2App(HWND topLevel,
                                const std::wstring& exeFullPath) noexcept {
    if (!topLevel || exeFullPath.empty()) return false;

    if (webView2PositiveCache_.count(exeFullPath)) return true;

    DWORD pid = 0;
    GetWindowThreadProcessId(topLevel, &pid);
    if (!pid) return false;

    const ULONGLONG now = GetTickCount64();

    // Negative cache: a recent "not WebView2" verdict for this exe lets us skip
    // the ~150 ms snapshot below. Re-focusing explorer.exe etc. otherwise paid
    // the full cross-process scan every time (only positives were cached). The
    // TTL bounds staleness so a lazily-initialized WebView2 host is re-checked.
    if (auto it = webView2NegativeCache_.find(exeFullPath);
        it != webView2NegativeCache_.end()) {
        if (IsWebView2NegativeCacheFresh(now, it->second, kWebView2NegativeTtlMs)) {
            FOCUS_LOG(L"  IsWebView2App: pid=%u neg-cache hit (skip snapshot)", pid);
            return false;
        }
        // Expired — fall through and re-scan; the store below refreshes the tick.
    }

    // Instrumentation: snapshot APIs below cross process boundaries (loader lock +
    // potential AV hook). Logged so 1-off user reports of post-unlock CPU spikes
    // can be triaged with evidence instead of speculation. See docs/TODO.md
    // "IsWebView2App perf instrumentation".
    const ULONGLONG t0 = now;
    const wchar_t* slash = wcsrchr(exeFullPath.c_str(), L'\\');
    const wchar_t* exeBase = slash ? slash + 1 : exeFullPath.c_str();
    const wchar_t* pass1Result = L"snap_fail";
    const wchar_t* pass2Result = L"skip";

    bool found = false;

    // Pass 1 — loaded modules in the host process.
    // TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32 covers both native + WoW64.
    // INVALID_HANDLE_VALUE on cross-IL / AppContainer targets → fall through.
    if (HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        modSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        for (BOOL ok = Module32FirstW(modSnap, &me); ok; ok = Module32NextW(modSnap, &me)) {
            if (_wcsicmp(me.szModule, L"WebView2Loader.dll") == 0 ||
                _wcsicmp(me.szModule, L"embeddedbrowserwebview.dll") == 0) {
                found = true;
                break;
            }
        }
        CloseHandle(modSnap);
        pass1Result = found ? L"module_found" : L"module_notfound";
    }

    // Pass 2 — `msedgewebview2.exe` spawned as a child process.
    // Covers modern Tauri where the host doesn't load WebView2 DLLs itself.
    if (!found) {
        pass2Result = L"snap_fail";
        if (HANDLE procSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            procSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            for (BOOL ok = Process32FirstW(procSnap, &pe); ok; ok = Process32NextW(procSnap, &pe)) {
                if (pe.th32ParentProcessID == pid &&
                    _wcsicmp(pe.szExeFile, L"msedgewebview2.exe") == 0) {
                    found = true;
                    break;
                }
            }
            CloseHandle(procSnap);
            pass2Result = found ? L"proc_found" : L"proc_notfound";
        }
    }

    if (found) {
        webView2PositiveCache_.insert(exeFullPath);
        webView2NegativeCache_.erase(exeFullPath);  // promote out of the miss set
    } else {
        if (webView2NegativeCache_.size() >= kMaxWebView2NegativeCache)
            webView2NegativeCache_.clear();          // bound growth (see header)
        webView2NegativeCache_[exeFullPath] = now;   // remember the miss (TTL'd)
    }

    FOCUS_LOG(L"  IsWebView2App: pid=%u exe=\"%s\" pass1=%s pass2=%s result=%d dur=%llums",
             pid, exeBase, pass1Result, pass2Result, found ? 1 : 0,
             GetTickCount64() - t0);
    return found;
}

void FocusOwner::RefreshFocusCache(HWND foreground) noexcept {
    HWND focused = ::NextKey::GetFocusedChildHwnd(foreground);
    cachedFocusedHwnd_.store(focused, std::memory_order_relaxed);
    if (focused) {
        wchar_t cls[64] = {};
        GetClassNameW(focused, cls, 64);
        cachedFocusedClass_.assign(cls);
    } else {
        cachedFocusedClass_.clear();
    }
}

void FocusOwner::InvalidateFocusCache() noexcept {
    cachedFocusedHwnd_.store(nullptr, std::memory_order_relaxed);
    cachedFocusedClass_.clear();  // tuple race benign — see header doc
}

// ─────────────────────────────────────────────────────────────────────────
// Phase 2b classify (moved from HookEngine::ClassifyFocusedWindow).
//
// Caller thread: main (WinEventProc) or worker (OnTickPoll). Heavy Win32
// inspection lives here: ClassifyWindow + GetExeNameForHwnd +
// IsWebView2App + cache lookup/store + override-map reads. None of these
// are safe to run from the LL hook callback (Rule 11.2:
// CreateToolhelp32Snapshot ≥ 3 ms blows the 30 ms p99 Tier-2 budget).
//
// The result is a `FocusClassification` POD which HookEngine posts to the
// mailbox; the hook thread consumes it in ApplyFocusOnHookThread.
// ─────────────────────────────────────────────────────────────────────────
FocusClassification FocusOwner::Classify(HWND triggerHwnd,
                                          const ConfigContext& ctx) noexcept {
    PERF_SCOPE(::NextKey::Perf::Stage::FocusClassify);
    FocusClassification cls;

    // Hidden helpers, tray, zero-size, tool windows — still classify (so
    // dispatch flags stay consistent when focus transits through one) but
    // skip the smart-switch / currentExe_ update.
    const auto isHelper = [](HWND hwnd) noexcept {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsTrayOrTaskbarWindow(hwnd)) return true;
        RECT rect;
        if (GetWindowRect(hwnd, &rect) &&
            (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0 || rect.left <= -20000)) {
            return true;  // trick message-pump windows (IDM et al.)
        }
        return (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;  // tooltips, menus
    };

    HWND fg = GetForegroundWindow();
    const bool fgIsHelper = fg && isHelper(fg);
    HWND activeHwnd = reinterpret_cast<HWND>(ChooseFocusTarget(
        reinterpret_cast<std::uintptr_t>(triggerHwnd),
        reinterpret_cast<std::uintptr_t>(fg), fgIsHelper));
    if (!activeHwnd) return cls;  // empty cls = "nothing to apply" sentinel
    if (triggerHwnd && activeHwnd != triggerHwnd) {
        FOCUS_LOG(L"  Classify: stale trigger %p — using foreground %p", triggerHwnd, fg);
    }

    cls.hwndOpaque = reinterpret_cast<std::uintptr_t>(activeHwnd);
    cls.skipAppTracking = activeHwnd == fg ? fgIsHelper : isHelper(activeHwnd);

    // Cache lookup: skip ClassifyWindow + GetExeNameForHwnd + IsWebView2App
    // when (HWND, PID) already classified. PID re-check inside LookupAppProfile
    // catches HWND reuse after a process dies.
    const AppProfile* cached = LookupAppProfile(activeHwnd);
    bool isBrowser{}, isElectron{}, isQtApp{}, isVB6{}, localConsole{}, isWebView2{};
    if (cached) {
        isBrowser    = cached->isBrowser;
        isElectron   = cached->isElectron;
        isQtApp      = cached->isQtApp;
        localConsole = cached->isConsole;
        isVB6        = cached->isVB6;
        isWebView2   = cached->isWebView2;
        cls.exeName  = cached->exeName;
    } else {
        ClassifyWindow(activeHwnd, isBrowser, isElectron, isQtApp, localConsole, isVB6);
        cls.exeName = GetExeNameForHwnd(activeHwnd);
    }
    cls.isBrowser  = isBrowser;
    cls.isElectron = isElectron;
    cls.isQtApp    = isQtApp;
    cls.isVB6      = isVB6;
    cls.isConsole  = localConsole;

    // Per-app send-method override. Reads the snapshot's
    // `appSendMethodOverrides` map — RCU-published from the worker thread, so
    // this main-thread lookup is lock-free and immune to torn reads during a
    // TOML rebuild. Values: 1=Clipboard, 2=Firefox-compat split, 3=Cloud/Remote
    // compat split, 5=game re-inject (0/absent = default Win32 batch path).
    bool forceNoBait = false;
    if (!cls.exeName.empty() && ctx.snap) {
        auto it = ctx.snap->appSendMethodOverrides.find(cls.exeName);
        if (it != ctx.snap->appSendMethodOverrides.end()) {
            switch (it->second) {
                case 1: cls.localUseClipboardInjector = true;                 break;
                case 2: cls.localForcedSplitSleepMs = kCompatSplitFirefoxMs;  break;
                case 3: cls.localForcedSplitSleepMs = kCompatSplitRemoteMs;   break;
                case 4:
                    cls.localForceEmReplaceSel = true;
                    cls.localEditMsg = true;
                    break;
                // Mutually exclusive with 1-4 by construction (one value per
                // app), which is what keeps "game + clipboard" and
                // "game + EM_REPLACESEL" — combinations whose re-inject
                // accounting has no sensible answer — unreachable.
                case 5: cls.localGameReinject = true;                         break;
                case 6:
                    cls.localForcedSplitSleepMs = kCompatSplitWebViewMs;
                    forceNoBait = true;
                    break;
                default: break;  // 0=SendInput / unknown → default Win32 path
            }
        }
    }

    // Dispatch-shape derivation. See OnFocusChanged's pre-Phase-2b comments
    // for the per-host rationale (bait char, split dispatch, edit message
    // path, clipboard fallback). Preserved verbatim — this is data the
    // hook hot path will read via the IOutputInjector picked in Apply.
    cls.localSkipEmpty = cls.isElectron || cls.isConsole || forceNoBait;
    cls.localNeedBait  = forceNoBait ? false : cls.isBrowser;
    cls.localClipboard = cls.isVB6;
    if (!cls.localSkipEmpty && !cls.localNeedBait && !cls.localClipboard) {
        if (!cls.exeName.empty()) {
            if (_wcsicmp(cls.exeName.c_str(), L"zed.exe") == 0) {
                cls.localSkipEmpty = true;
            } else if (_wcsicmp(cls.exeName.c_str(), L"notepad.exe") == 0) {
                // Win11 WinUI 3 Notepad: RichEditBox async on compositor; EM_REPLACESEL
                // on the child Edit is atomic and avoids the flicker that SendInput
                // batching causes here. Classic Notepad benefits too (single undo entry).
                cls.localEditMsg = true;
            } else {
                const bool isOutlook = cls.exeName.find(L"outlook") != std::wstring::npos;
                const bool isExcel   = cls.exeName.find(L"excel") != std::wstring::npos;
                // #252: Excel formula suggestions are dropdowns; the U+202F
                // inline-suggestion bait can leak into the cell as a tofu glyph.
                // Keep Excel out of both Office bait and WebView2 promotion.
                cls.localNeedBait = isOutlook;
                if (!isExcel && !isOutlook) {
                    if (!cached) {
                        std::wstring exeFullPath = GetExeFullPathForHwnd(activeHwnd);
                        isWebView2 = IsWebView2App(activeHwnd, exeFullPath);
                    }
                    if (isWebView2) {
                        cls.localNeedBait = false;
                        cls.localSkipEmpty = true;
                    }
                }
            }
        }
    }
    cls.isWebView2 = isWebView2;
    cls.localElectronApp = (cls.isElectron || cls.isWebView2 || forceNoBait) && !cls.isConsole;

    // Cache miss path: persist for next focus event. Skipped when
    // GetWindowThreadProcessId fails — invariant requires PID for re-check.
    DWORD pid = 0;
    GetWindowThreadProcessId(activeHwnd, &pid);
    cls.pid = static_cast<std::uint32_t>(pid);
    if (!cached && pid != 0) {
        AppProfile profile;
        profile.pid = pid;
        profile.exeName = cls.exeName;
        profile.isBrowser  = isBrowser;
        profile.isElectron = isElectron;
        profile.isQtApp    = isQtApp;
        profile.isConsole  = localConsole;
        profile.isVB6      = isVB6;
        profile.isWebView2 = isWebView2;
        StoreAppProfile(activeHwnd, std::move(profile));
    }

    // exeName fallback: triggerHwnd may have died by the time the async
    // event dispatches; fall back to current foreground.
    if (cls.exeName.empty() && activeHwnd != fg && fg) {
        cls.exeName = GetExeNameForHwnd(fg);
    }

    // Per-app excluded / TSF / encoding / method overrides — captured here
    // so ApplyFocusOnHookThread doesn't need to touch the maps OR read
    // `global{CodeTable,InputMethod}_` (both written from main; the new
    // cross-thread read would be a race). Phase 3 RCU-snapshots the maps
    // and the global values come in via ConfigContext (HookEngine reads
    // the atomics on the caller thread and hands the resolved values in).
    cls.targetCodeTable = ctx.globalCodeTable;
    cls.targetMethod    = ctx.globalInputMethod;
    // No skipAppTracking gate on the excluded / TSF resolution: those two decide
    // which engine owns the app, and ApplyFocusOnHookThread publishes them for
    // every focus event including helper windows (#242 — a skipped focus event
    // used to leave TSF_ACTIVE pointing at the previous app). The per-app
    // encoding / method overrides stay gated: they feed the typing state, which
    // is exactly what must not flip on a transient helper.
    if (!cls.exeName.empty() && ctx.snap && ctx.cfg) {
        // Per-app mode lock (hard-E / hard-V) shares the excludeApps gate. The
        // two snapshot sets are disjoint (excluded wins, enforced at build time);
        // DecidePerAppMode is the belt-and-suspenders precedence. Forced-V apps
        // are NOT excluded/TSF, so they still flow into the encoding/method
        // override path below (only the smart-switch restore is overridden on
        // the hook thread — see HookEngine::ApplyFocusOnHookThread).
        if (ctx.cfg->excludeApps) {
            const bool inExcl = !ctx.snap->excludedAppSet.empty()
                                && ctx.snap->excludedAppSet.count(cls.exeName) > 0;
            const bool inVn   = !ctx.snap->forcedVietnameseAppSet.empty()
                                && ctx.snap->forcedVietnameseAppSet.count(cls.exeName) > 0;
            const PerAppMode m = DecidePerAppMode(inExcl, inVn);
            cls.isExcluded         = (m == PerAppMode::ForceEnglish);
            cls.isForcedVietnamese = (m == PerAppMode::ForceVietnamese);
        }
        if (!cls.isExcluded && ctx.cfg->tsfApps && !ctx.snap->tsfAppSet.empty()) {
            cls.isTsf = ctx.snap->tsfAppSet.count(cls.exeName) > 0;
        }
        if (!cls.isExcluded && !cls.isTsf && !cls.skipAppTracking) {
            auto itEnc = ctx.snap->appEncodingOverrides.find(cls.exeName);
            if (itEnc != ctx.snap->appEncodingOverrides.end()) {
                cls.targetCodeTable = static_cast<int>(itEnc->second);
            }
            auto itIm = ctx.snap->appInputMethodOverrides.find(cls.exeName);
            if (itIm != ctx.snap->appInputMethodOverrides.end()) {
                cls.targetMethod = static_cast<int>(itIm->second);
            }
        }
    }

    // Hook-only auto-cap fallback: ask only when the result can be consumed.
    // Custom controls, TSF/excluded apps, helper windows, disabled auto-cap,
    // and stale foreground identities all remain conservative. The focused
    // child is resolved once here and carried in the classification so the
    // hook thread can re-check it's still current before trusting this
    // verdict — the worker can run before or after the app that owns
    // `activeHwnd` finishes moving focus to it (WH_MOUSE_LL fires before
    // the click is delivered), so the control probed here may not be the
    // one actually focused by the time this classification is applied.
    if (!cls.skipAppTracking && !cls.isExcluded && !cls.isTsf
        && ctx.cfg && ctx.cfg->autoCaps) {
        const HWND focusedChild = ::NextKey::GetFocusedChildHwnd(activeHwnd);
        cls.focusedChildHwndOpaque = reinterpret_cast<std::uintptr_t>(focusedChild);
        cls.isPasswordFieldFocused = IsFocusedControlPassword(activeHwnd, focusedChild);
        if (!cls.isPasswordFieldFocused) {
            const std::uint64_t probeStartedAtMs = GetTickCount64();
            cls.isKnownEmptyDocument = IsKnownEmptyFocusedDocument(activeHwnd, focusedChild);
            if (cls.isKnownEmptyDocument) {
                cls.emptyDocumentProbeStartedAtMs = probeStartedAtMs;
            }
        }
    }

    return cls;
}

// ─────────────────────────────────────────────────────────────────────────
// Smart-switch RCU snapshot — hook-thread publish, worker-thread load.
// See SnapshotAppModes() rationale in FocusOwner.h header doc.
// ─────────────────────────────────────────────────────────────────────────

void FocusOwner::PublishAppModesSnapshot() noexcept {
    // Hook-thread side of the RCU publish. Allocates a fresh shared_ptr
    // holding an immutable copy of the live appModeMap_, then publishes
    // it via std::atomic_store. The previous snapshot's refcount drops;
    // worker threads holding it via SnapshotAppModes() keep it alive.
    //
    // Cost: 1 alloc + map copy (~1.5 KB at cap 200). Fires only on focus
    // changes / mode toggles → cold path, well within Pillar 1 budget.
    try {
        auto fresh = std::make_shared<
            const std::unordered_map<std::wstring, bool>>(appModeMap_);
        appModesSnap_.store(std::move(fresh), std::memory_order_release);
    } catch (...) {
        // Allocation failure (OOM): leave previous snapshot in place.
        // Worst case is a stale write window the next mutation will fix.
    }
}

std::shared_ptr<const std::unordered_map<std::wstring, bool>>
FocusOwner::SnapshotAppModes() const noexcept {
    return appModesSnap_.load(std::memory_order_acquire);
}

}  // namespace NextKey
