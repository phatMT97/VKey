// VKey - Convert Tool Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ConvertToolDialog.h"
#include "core/engine/CodeTableConverter.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyLabel.h"
#include "helpers/AppHelpers.h"
#include "core/Strings.h"
#include "sciter/ScaleHelper.h"
#include "sciter-x-dom.hpp"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <vector>

using namespace sciter::dom;

namespace NextKey {

// Measure after construction and after Sciter has applied document/mode changes.
static constexpr UINT WM_CONVERT_FIT_CONTENT = WM_APP + 3;
static constexpr UINT_PTR TIMER_CONVERT_FIT_CONTENT = 1003;

ConvertToolDialog::ConvertToolDialog(HWND parent)
    : SciterSubDialog({
        L"this://app/convert-tool/convert-tool.html",
        L"VKey - Convert Tool",
        kBaseWidth, kBaseHeight, parent, false, 36, 70, true
    }) {
    // Load saved config (UI will be populated in DOCUMENT_COMPLETE)
    config_ = ConfigManager::LoadConvertConfigOrDefault();
    PostMessageW(get_hwnd(), WM_CONVERT_FIT_CONTENT, 0, 0);
}

LRESULT ConvertToolDialog::onCustomMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM) {
    if (msg == WM_CONVERT_FIT_CONTENT) {
        // Let Sciter commit CSS/layout before reading geometry. Coalesce mode
        // and document notifications into one measurement after the next frame.
        if (!SetTimer(hwnd, TIMER_CONVERT_FIT_CONTENT, 30, nullptr)) fitContent();
        return 0;
    }
    if (msg == WM_TIMER && wParam == TIMER_CONVERT_FIT_CONTENT) {
        KillTimer(hwnd, TIMER_CONVERT_FIT_CONTENT);
        fitContent();
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        PostMessageW(hwnd, WM_CONVERT_FIT_CONTENT, 0, 0);
    }
    return -1;
}

void ConvertToolDialog::fitContent() {
    HWND hwnd = get_hwnd();
    if (!hwnd || IsIconic(hwnd)) return;

    element rootEl = get_root();
    element container = rootEl.find_first("#main-container");
    element content = rootEl.find_first(".content-section");
    if (!container.is_valid() || !content.is_valid()) return;

    // Re-measure natural height each time: delta-based resizing drifts on mode
    // changes and cannot account for translated text or a different monitor DPI.
    content.set_style_attribute("height", L"auto");
    container.set_style_attribute("height", L"auto");
    rootEl.update(true);
    const RECT bounds = container.get_location_ppx(MARGIN_BOX);
    const int width = bounds.right - bounds.left;
    const int naturalHeight = bounds.bottom - bounds.top;
    if (width <= 0 || naturalHeight <= 0) return;

    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT& work = monitor.rcWork;
    const int height = (std::min)(naturalHeight, static_cast<int>(work.bottom - work.top));
    if (height < naturalHeight) {
        // Keep the title visible and scroll the content on short work areas.
        const RECT border = container.get_location_ppx(BORDER_BOX);
        const int borderHeight = border.bottom - border.top - (naturalHeight - height);
        wchar_t cssHeight[32];
        swprintf_s(cssHeight, L"%.2fpx", borderHeight / ScaleHelper::getDpiScaleForWindow(hwnd));
        container.set_style_attribute("height", cssHeight);
        content.set_style_attribute("height", L"*");
    }

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return;
    const int x = (std::max)(static_cast<int>(work.left),
        (std::min)(static_cast<int>(windowRect.left), static_cast<int>(work.right) - width));
    const int y = (std::max)(static_cast<int>(work.top),
        (std::min)(static_cast<int>(windowRect.top), static_cast<int>(work.bottom) - height));
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    rootEl.update(true);
}

bool ConvertToolDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    // Document fully loaded — populate UI from saved config
    if (params.cmd == DOCUMENT_COMPLETE) {
        sciter::dom::element root = get_root();
        sciter::dom::element pinBtn = root.find_first("#btn-pin");
        if (pinBtn.is_valid() && config().topmost) {
            pinBtn.set_attribute("class", L"btn-pin pinned");
        }

        // Upload canonical VK → name table to JS before any label rendering
        // so the capture preview shows "Space" instead of falling back to
        // "VK_32" (the bare-decimal-fallback in shared/hotkey-capture.js).
        {
            sciter::value pairs;
            int i = 0;
            for (const auto& [vk, name] : GetVkDisplayNames()) {
                sciter::value pair;
                pair.set_item(0, sciter::value(static_cast<int>(vk)));
                pair.set_item(1, sciter::value(name));
                pairs.set_item(i++, pair);
            }
            call_function("setVkNames", pairs);
        }

        // Set toggle states
        setToggleUI("#toggle-all-caps", "#val-toggle-all-caps", config_.allCaps);
        setToggleUI("#toggle-non-caps", "#val-toggle-non-caps", config_.allLower);
        setToggleUI("#toggle-caps-first", "#val-toggle-caps-first", config_.capsFirst);
        setToggleUI("#toggle-caps-each", "#val-toggle-caps-each", config_.capsEach);
        setToggleUI("#toggle-remove-mark", "#val-toggle-remove-mark", config_.removeMark);
        setToggleUI("#toggle-alert", "#val-toggle-alert", config_.alertDone);
        setToggleUI("#toggle-auto-paste", "#val-toggle-auto-paste", config_.autoPaste);
        setToggleUI("#toggle-sequential", "#val-toggle-sequential", config_.sequential);
        setToggleUI("#toggle-enable-log", "#val-toggle-enable-log", config_.enableLog);

        // Set encoding dropdowns
        setDropdownUI("#source-encoding", config_.sourceEncoding);
        setDropdownUI("#dest-encoding", config_.destEncoding);

        // Push existing hotkey binding into the record-button display + hidden
        // inputs. JS reads `val-hotkey-vk` / `val-hotkey-mods` only when the
        // user records a new one — we seed them so the dialog reflects the
        // persisted binding from boot and a no-op close doesn't clobber it.
        {
            uint32_t vk   = config_.hotkey.vk;
            uint32_t mods = config_.hotkey.ToMods();
            sciter::dom::element root2 = get_root();
            sciter::dom::element vkEl   = root2.find_first("#val-hotkey-vk");
            sciter::dom::element modsEl = root2.find_first("#val-hotkey-mods");
            std::wstring vkStr   = std::to_wstring(vk);
            std::wstring modsStr = std::to_wstring(mods);
            if (vkEl.is_valid())   vkEl.set_value(sciter::value(vkStr.c_str()));
            if (modsEl.is_valid()) modsEl.set_value(sciter::value(modsStr.c_str()));

            std::wstring label = FormatHotkeyLabel(vk, mods);
            const wchar_t* labelText = label.empty() ? L"— Chưa đặt —" : label.c_str();
            root2.call_function("setHotkeyDisplay", sciter::value(labelText));
        }

        // Sync sequential toggle enabled/disabled state from the actual autoPaste value.
        // JS initializeToggles() runs before DOCUMENT_COMPLETE, so it cannot see the real
        // registry value yet. We call the JS function directly after all toggles are set.
        {
            sciter::dom::element root2 = get_root();
            sciter::value autoPasteArg(config_.autoPaste);
            root2.call_function("updateSequentialToggleState", autoPasteArg);
        }

        PostMessageW(get_hwnd(), WM_CONVERT_FIT_CONTENT, 0, 0);
        return true;
    }

    // Handle BUTTON_CLICK for close and convert buttons
    if (params.cmd == BUTTON_CLICK) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"btn-close") {
            PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
            return true;
        }
        if (id == L"btn-pin") {
            togglePin();
            return true;
        }
        if (id == L"btn-convert") {
            doConvert();
            return true;
        }
    }

    // Handle VALUE_CHANGED for toggles, encodings, hotkeys, and actions
    if (params.cmd == VALUE_CHANGED) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"val-action") {
            sciter::value val = el.get_value();
            std::wstring action = val.is_string() ? val.get<std::wstring>() : L"";

            if (action == L"close") {
                PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
            } else if (action == L"select-source-file") {
                browseFile(true);
            } else if (action == L"select-dest-file") {
                browseFile(false);
            }

            el.set_value(sciter::value(L""));
            return true;
        }

        // Mode switch (clipboard ↔ file): fit the complete updated document.
        if (id == L"val-ui-mode") {
            PostMessageW(get_hwnd(), WM_CONVERT_FIT_CONTENT, 0, 0);
            return true;
        }

        // Toggle options — save on change
        bool needSave = false;
        if (id == L"val-toggle-all-caps") { config_.allCaps = getToggleValue("#val-toggle-all-caps"); needSave = true; }
        else if (id == L"val-toggle-non-caps") { config_.allLower = getToggleValue("#val-toggle-non-caps"); needSave = true; }
        else if (id == L"val-toggle-caps-first") { config_.capsFirst = getToggleValue("#val-toggle-caps-first"); needSave = true; }
        else if (id == L"val-toggle-caps-each") { config_.capsEach = getToggleValue("#val-toggle-caps-each"); needSave = true; }
        else if (id == L"val-toggle-remove-mark") { config_.removeMark = getToggleValue("#val-toggle-remove-mark"); needSave = true; }
        else if (id == L"val-toggle-alert") { config_.alertDone = getToggleValue("#val-toggle-alert"); needSave = true; }
        else if (id == L"val-toggle-auto-paste") { config_.autoPaste = getToggleValue("#val-toggle-auto-paste"); needSave = true; }
        else if (id == L"val-toggle-sequential") { config_.sequential = getToggleValue("#val-toggle-sequential"); needSave = true; }
        else if (id == L"val-toggle-enable-log") { config_.enableLog = getToggleValue("#val-toggle-enable-log"); needSave = true; }
        // Encoding dropdowns
        else if (id == L"source-encoding") { config_.sourceEncoding = static_cast<uint8_t>(getDropdownValue("#source-encoding")); needSave = true; }
        else if (id == L"dest-encoding") { config_.destEncoding = static_cast<uint8_t>(getDropdownValue("#dest-encoding")); needSave = true; }
        // Hotkey: shared/hotkey-capture.js writes both `val-hotkey-vk` and
        // `val-hotkey-mods` on commit, then fires change on `val-hotkey-vk`.
        // We read both at once to keep the binding consistent.
        else if (id == L"val-hotkey-vk") {
            try {
                config_.hotkey.vk = static_cast<uint32_t>(std::stoul(getHiddenValue("#val-hotkey-vk")));
            } catch (...) { config_.hotkey.vk = 0; }
            uint32_t mods = 0;
            try {
                mods = static_cast<uint32_t>(std::stoul(getHiddenValue("#val-hotkey-mods")));
            } catch (...) { mods = 0; }
            config_.hotkey.SetModsFromMask(mods);
            needSave = true;
        }

        if (needSave) {
            saveConvertConfig();
        }
    }

    return sciter::window::handle_event(he, params);
}

// ═══════════════════════════════════════════════════════════
// UI reading helpers
// ═══════════════════════════════════════════════════════════

bool ConvertToolDialog::getToggleValue(const char* id) {
    sciter::dom::element root = get_root();
    sciter::dom::element el = root.find_first(id);
    if (!el.is_valid()) return false;
    sciter::value val = el.get_value();
    return val.is_string() && val.get<std::wstring>() == L"1";
}

int ConvertToolDialog::getDropdownValue(const char* id) {
    sciter::dom::element root = get_root();
    sciter::dom::element el = root.find_first(id);
    if (!el.is_valid()) return 0;
    sciter::value val = el.get_value();
    if (val.is_string()) {
        std::wstring s = val.get<std::wstring>();
        if (!s.empty()) {
            try { return std::stoi(s); } catch (...) { return 0; }
        }
    } else if (val.is_int()) {
        return val.get<int>();
    }
    return 0;
}

std::wstring ConvertToolDialog::getHiddenValue(const char* id) {
    sciter::dom::element root = get_root();
    sciter::dom::element el = root.find_first(id);
    if (!el.is_valid()) return L"";
    sciter::value val = el.get_value();
    return val.is_string() ? val.get<std::wstring>() : L"";
}

// ═══════════════════════════════════════════════════════════
// Clipboard I/O
// ═══════════════════════════════════════════════════════════

std::wstring ConvertToolDialog::readClipboardText() {
    if (!OpenClipboard(nullptr)) return L"";
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return L"";
    }
    auto* pText = static_cast<const wchar_t*>(GlobalLock(hData));
    if (!pText) {
        CloseClipboard();
        return L"";
    }
    std::wstring text(pText);
    GlobalUnlock(hData);
    CloseClipboard();
    return text;
}

bool ConvertToolDialog::writeClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }
    auto* pDst = static_cast<wchar_t*>(GlobalLock(hMem));
    if (!pDst) {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }
    memcpy(pDst, text.c_str(), bytes);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
}

// ═══════════════════════════════════════════════════════════
// File I/O
// ═══════════════════════════════════════════════════════════

std::wstring ConvertToolDialog::readFileContent(const std::wstring& path, CodeTable sourceTable) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return L"";
    }

    std::vector<BYTE> buffer(fileSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), fileSize, &bytesRead, nullptr)) {
        CloseHandle(hFile);
        return L"";
    }
    CloseHandle(hFile);

    if (sourceTable == CodeTable::Unicode) {
        // Read as UTF-8
        int wideLen = MultiByteToWideChar(CP_UTF8, 0,
            reinterpret_cast<const char*>(buffer.data()), static_cast<int>(bytesRead),
            nullptr, 0);
        if (wideLen <= 0) return L"";
        std::wstring result(wideLen, 0);
        MultiByteToWideChar(CP_UTF8, 0,
            reinterpret_cast<const char*>(buffer.data()), static_cast<int>(bytesRead),
            result.data(), wideLen);
        return result;
    }

    // Legacy encodings: widen each byte to wchar_t
    std::wstring result;
    result.reserve(bytesRead);
    for (DWORD i = 0; i < bytesRead; ++i) {
        result += static_cast<wchar_t>(buffer[i]);
    }
    return result;
}

bool ConvertToolDialog::writeFileContent(const std::wstring& path, const std::wstring& text,
                                         CodeTable destTable) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    bool ok = false;

    if (destTable == CodeTable::Unicode) {
        // Write as UTF-8
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::vector<char> utf8(utf8Len);
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                utf8.data(), utf8Len, nullptr, nullptr);
            ok = WriteFile(hFile, utf8.data(), utf8Len, &bytesWritten, nullptr) != FALSE;
        }
    } else {
        // Legacy encodings: narrow each wchar_t to byte
        std::vector<BYTE> bytes;
        bytes.reserve(text.size());
        for (wchar_t ch : text) {
            bytes.push_back(static_cast<BYTE>(ch & 0xFF));
        }
        ok = WriteFile(hFile, bytes.data(), static_cast<DWORD>(bytes.size()),
                       &bytesWritten, nullptr) != FALSE;
    }

    CloseHandle(hFile);
    return ok;
}

// ═══════════════════════════════════════════════════════════
// Conversion pipeline
// ═══════════════════════════════════════════════════════════

void ConvertToolDialog::doConvert() {
    // 1. Read toggle states
    bool allCaps      = getToggleValue("#val-toggle-all-caps");
    bool allLower     = getToggleValue("#val-toggle-non-caps");
    bool capsFirst    = getToggleValue("#val-toggle-caps-first");
    bool capsEach     = getToggleValue("#val-toggle-caps-each");
    bool removeMark   = getToggleValue("#val-toggle-remove-mark");
    bool alertDone    = getToggleValue("#val-toggle-alert");

    // 2. Read encodings
    int srcIdx = getDropdownValue("#source-encoding");
    int dstIdx = getDropdownValue("#dest-encoding");
    auto srcTable = static_cast<CodeTable>(srcIdx);
    auto dstTable = static_cast<CodeTable>(dstIdx);

    // 3. Read source type and input text
    std::wstring sourceType = getHiddenValue("#val-source-type");
    std::wstring input;

    if (sourceType == L"file") {
        std::wstring srcPath = getHiddenValue("#val-source-file");
        if (srcPath.empty()) {
            MessageBoxW(get_hwnd(), S(StringId::CONVERT_NO_SOURCE_FILE), L"VKey", MB_OK | MB_ICONWARNING);
            return;
        }
        input = readFileContent(srcPath, srcTable);
        if (input.empty()) {
            MessageBoxW(get_hwnd(), S(StringId::CONVERT_READ_ERROR), L"VKey", MB_OK | MB_ICONERROR);
            return;
        }
    } else {
        input = readClipboardText();
        if (input.empty()) {
            MessageBoxW(get_hwnd(), S(StringId::CONVERT_CLIPBOARD_EMPTY), L"VKey", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    // 4. Decode: source encoding → Unicode
    std::wstring unicode = CodeTableConverter::DecodeString(input, srcTable);

    // 5. Apply text transformations (on Unicode)
    if (removeMark) {
        unicode = CodeTableConverter::RemoveDiacritics(unicode);
    }
    if (allCaps) {
        unicode = CodeTableConverter::ToUpper(unicode);
    } else if (allLower) {
        unicode = CodeTableConverter::ToLower(unicode);
    } else if (capsFirst) {
        unicode = CodeTableConverter::ToSentenceCase(unicode);
    } else if (capsEach) {
        unicode = CodeTableConverter::ToTitleCase(unicode);
    }

    // 6. Encode: Unicode → dest encoding
    std::wstring output = CodeTableConverter::EncodeString(unicode, dstTable);

    // 7. Write output
    if (sourceType == L"file") {
        std::wstring dstPath = getHiddenValue("#val-dest-file");
        if (dstPath.empty()) {
            MessageBoxW(get_hwnd(), S(StringId::CONVERT_NO_DEST_FILE), L"VKey", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!writeFileContent(dstPath, output, dstTable)) {
            MessageBoxW(get_hwnd(), S(StringId::CONVERT_WRITE_ERROR), L"VKey", MB_OK | MB_ICONERROR);
            return;
        }
    } else {
        if (!writeClipboardText(output)) {
            MessageBoxW(get_hwnd(), S(StringId::CONVERT_CLIPBOARD_WRITE_ERROR), L"VKey", MB_OK | MB_ICONERROR);
            return;
        }
    }

    // 8. Alert on completion
    if (alertDone) {
        MessageBoxW(get_hwnd(), S(StringId::CONVERT_SUCCESS), L"VKey", MB_OK | MB_ICONINFORMATION);
    }
}

// ═══════════════════════════════════════════════════════════
// Config persistence
// ═══════════════════════════════════════════════════════════

void ConvertToolDialog::browseFile(bool isSource) {
    WCHAR szFile[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = get_hwnd();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0"
                      L"Rich Text Format (*.rtf)\0*.rtf\0"
                      L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;

    BOOL result = FALSE;
    if (isSource) {
        ofn.lpstrTitle = L"Ch\x1ECDn file ngu\x1ED3n";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        result = GetOpenFileNameW(&ofn);
    } else {
        ofn.lpstrTitle = L"Ch\x1ECDn file \x0111\x00EDch";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        result = GetSaveFileNameW(&ofn);
    }

    if (!result) return;

    std::wstring path(szFile);
    sciter::dom::element root = get_root();

    // Update hidden value for C++ doConvert()
    const char* hiddenId = isSource ? "#val-source-file" : "#val-dest-file";
    sciter::dom::element hidden = root.find_first(hiddenId);
    if (hidden.is_valid()) {
        hidden.set_value(sciter::value(path));
    }

    // Update visible path display
    const char* displayId = isSource ? "#source-file-path" : "#dest-file-path";
    sciter::dom::element display = root.find_first(displayId);
    if (display.is_valid()) {
        display.set_value(sciter::value(path));
        display.set_attribute("title", path.c_str());
    }
}

void ConvertToolDialog::saveConvertConfig() {
    (void)ConfigManager::SaveConvertConfig(ConfigManager::GetConfigPath(), config_);
    SignalConfigChange();
}

void ConvertToolDialog::setToggleUI(const char* toggleId, const char* hiddenId, bool value) {
    sciter::dom::element root = get_root();

    // Set visual toggle class
    sciter::dom::element toggle = root.find_first(toggleId);
    if (toggle.is_valid()) {
        std::wstring cls = toggle.get_attribute("class");
        if (value && cls.find(L"checked") == std::wstring::npos) {
            toggle.set_attribute("class", (cls + L" checked").c_str());
        } else if (!value && cls.find(L"checked") != std::wstring::npos) {
            // Remove "checked" from class string
            auto pos = cls.find(L" checked");
            if (pos != std::wstring::npos) cls.erase(pos, 8);
            pos = cls.find(L"checked ");
            if (pos != std::wstring::npos) cls.erase(pos, 8);
            pos = cls.find(L"checked");
            if (pos != std::wstring::npos) cls.erase(pos, 7);
            toggle.set_attribute("class", cls.c_str());
        }
    }

    // Set hidden input value
    sciter::dom::element hidden = root.find_first(hiddenId);
    if (hidden.is_valid()) {
        hidden.set_value(sciter::value(value ? L"1" : L"0"));
    }
}

void ConvertToolDialog::setDropdownUI(const char* id, int value) {
    sciter::dom::element root = get_root();
    sciter::dom::element dropdown = root.find_first(id);
    if (dropdown.is_valid()) {
        dropdown.set_value(sciter::value(value));
    }
}

void ConvertToolDialog::togglePin() {
    HWND hwnd = get_hwnd();
    if (!hwnd) return;

    setPinned(!config().topmost);

    sciter::dom::element root = get_root();
    sciter::dom::element pinBtn = root.find_first("#btn-pin");
    if (pinBtn.is_valid()) {
        pinBtn.set_attribute("class", config().topmost ? L"btn-pin pinned" : L"btn-pin");
    }
}

}  // namespace NextKey
