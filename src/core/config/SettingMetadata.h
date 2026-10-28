// VKey - Setting Metadata Table
// Shared mapping for dual-UI (Sciter + Classic Win32) settings binding
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/config/TypingConfig.h"
#include "core/SystemConfig.h"

namespace NextKey {

/// What kind of control this setting maps to
enum class SettingType : uint8_t {
    Toggle,    // Checkbox (bool)
    Dropdown,  // Combo box (enum)
    Action     // Button (no backing field)
};

/// Which config struct owns this field
enum class SettingOwner : uint8_t {
    Typing,  // TypingConfig
    Hotkey,  // HotkeyConfig
    System,  // SystemConfig
    UI       // UI-only (no persistent field)
};

/// Metadata for one user-configurable setting
struct SettingMeta {
    const wchar_t* id;         // Sciter DOM id (e.g. L"spell-check")
    SettingType    type;        // Toggle, Dropdown, Action
    SettingOwner   owner;       // Typing, Hotkey, System, UI
    ptrdiff_t      offset;      // offsetof(OwnerStruct, field)
    const wchar_t* label;       // Vietnamese label
    const wchar_t* labelEn;     // English label
    const wchar_t* tooltip;     // Vietnamese tooltip (nullptr = none)
    const wchar_t* tooltipEn;   // English tooltip (nullptr = none)
    uint16_t       win32Id;     // IDC_ control ID (0 = no control)
    uint8_t        tab;         // 0=Cơ bản, 1=Phím tắt, 2=Hệ thống
    uint8_t        column;      // 0=left, 1=right
    // Dropdown items, null-terminated parallel arrays. nullptr for non-dropdowns.
    const wchar_t* const* itemsVi = nullptr;
    const wchar_t* const* itemsEn = nullptr;
    const wchar_t* parentId = nullptr;  // Non-null for an indented child toggle
};

// ── Dropdown item arrays (null-terminated) ─────────────────────────
inline constexpr const wchar_t* kIconStyleItemsVi[] = {
    L"Màu mặc định", L"Nền tối", L"Nền sáng", L"Tự chọn", L"Tự động", nullptr
};
inline constexpr const wchar_t* kIconStyleItemsEn[] = {
    L"Default color", L"Dark", L"Light", L"Custom", L"Auto", nullptr
};
inline constexpr const wchar_t* kStartupModeItemsVi[] = {
    L"Tiếng Việt", L"Tiếng Anh", L"Ghi nhớ", nullptr
};
inline constexpr const wchar_t* kStartupModeItemsEn[] = {
    L"Vietnamese", L"English", L"Remember", nullptr
};
// ── Helper macros to reduce verbosity ──────────────────────────────
// Master macro — every setting kind funnels through here.
#define NK_SETTING(type_, owner_, cfg, id, field, vi, en, tip, tipE, idc, tab, col, itemsV, itemsE) \
    { L##id, ::NextKey::SettingType::type_, ::NextKey::SettingOwner::owner_, \
      static_cast<ptrdiff_t>(offsetof(::NextKey::cfg, field)),                \
      L##vi, L##en, tip, tipE, idc, tab, col, itemsV, itemsE }

#define NK_TYPING(id, field, vi, en, tip, tipE, idc, tab, col) \
    NK_SETTING(Toggle, Typing, TypingConfig, id, field, vi, en, tip, tipE, idc, tab, col, nullptr, nullptr)

#define NK_TYPING_CHILD(id, field, vi, en, tip, tipE, idc, tab, col, parent) \
    { L##id, ::NextKey::SettingType::Toggle, ::NextKey::SettingOwner::Typing, \
      static_cast<ptrdiff_t>(offsetof(::NextKey::TypingConfig, field)),       \
      L##vi, L##en, tip, tipE, idc, tab, col, nullptr, nullptr, L##parent }

#define NK_HOTKEY(id, field, vi, en, tip, tipE, idc, tab, col) \
    NK_SETTING(Toggle, Hotkey, HotkeyConfig, id, field, vi, en, tip, tipE, idc, tab, col, nullptr, nullptr)

#define NK_SYSTEM(id, field, vi, en, tip, tipE, idc, tab, col) \
    NK_SETTING(Toggle, System, SystemConfig, id, field, vi, en, tip, tipE, idc, tab, col, nullptr, nullptr)

#define NK_DROPDOWN(id, field, vi, en, tip, tipE, idc, tab, col, itemsV, itemsE) \
    NK_SETTING(Dropdown, System, SystemConfig, id, field, vi, en, tip, tipE, idc, tab, col, itemsV, itemsE)

#define NK_TYPING_DROPDOWN(id, field, vi, en, tip, tipE, idc, tab, col, itemsV, itemsE) \
    NK_SETTING(Dropdown, Typing, TypingConfig, id, field, vi, en, tip, tipE, idc, tab, col, itemsV, itemsE)

#define NK_ACTION(id, vi, en, tip, tipE, idc, tab, col) \
    { L##id, ::NextKey::SettingType::Action, ::NextKey::SettingOwner::UI, \
      0, L##vi, L##en, tip, tipE, idc, tab, col, nullptr, nullptr }

/// All toggle settings, ordered by tab → column → visual position
inline constexpr SettingMeta kSettings[] = {

    // ── Tab 0: Bảng gõ — Left column (col 0) ──
    NK_TYPING("modern-ortho",         modernOrtho,
              "Đặt dấu kiểu mới (oà, uý)", "New diacritics style (oà, uý)",
              nullptr, nullptr,                                          2202, 0, 0),
    NK_TYPING("allow-english-bypass", allowEnglishBypass,
              "Gõ tự do",             "Bypass English blocking",
              L"Cho phép đặt dấu tiếng Việt trên các từ tiếng Anh hoặc ngoại lệ",
              L"Allow Vietnamese tones on English words or exceptions",  2208, 0, 0),
    NK_TYPING("auto-caps",            autoCaps,
              "Viết hoa chữ cái đầu", "Auto capitalize",
              nullptr, nullptr,                                          2203, 0, 0),
    NK_TYPING("allow-zwjf",           allowZwjf,
              "Cho phép z, w, j, f",  "Allow z, w, j, f",
              L"Mặc định đã cho phép. Chỉ cần bật khi kiểm tra chính tả được bật",
              L"Allowed by default. Only needed when spell check is on",
                                                                         2204, 0, 0),
    NK_TYPING("restore-key",          autoRestoreEnabled,
              "Tự khôi phục phím sai","Restore key on invalid",
              nullptr, nullptr,                                          2205, 0, 0),
    NK_TYPING("cjk-auto-switch",      cjkAutoSwitch,
              "Tự tắt khi bàn phím CJK", "Auto-disable on CJK layout",
              L"Tự sang E khi đang dùng bàn phím Trung/Nhật/Hàn",
              L"Switch to E when a Chinese/Japanese/Korean layout is active",
                                                                         2209, 0, 0),

    // ── Tab 0: Bảng gõ — Right column (col 1) ──
    NK_TYPING("beep-sound",           beepOnSwitch,
              "Tiếng bíp khi chuyển",   "Beep on switch",
              L"Phát âm báo khi chuyển đổi ngôn ngữ",
              L"Play sound when switching language",                     2211, 0, 1),
    NK_TYPING("suggest-keep-chars",   suggestKeepChars,
              "BS giữ chữ khi có gợi ý", "BS keeps chars on suggest",
              L"Khi tắt gợi ý của trình duyệt bằng Backspace, giữ chữ đã gõ. Cảnh báo: có thể sai dấu nếu gõ tiếp ngay sau BS.",
              L"When dismissing a browser suggestion via Backspace, preserve typed chars. Warning: tone placement may be wrong if you keep typing right after BS.",
                                                                         2220, 0, 1),
    NK_TYPING("smart-switch",         smartSwitch,
              "Lưu chế độ gõ theo app",   "Smart input switch",
              L"Tự động ghi nhớ chế độ gõ cho từng ứng dụng",
              L"Auto-remember input mode per app",                       2206, 0, 1),
    NK_TYPING("exclude-apps",         excludeApps,
              "Khoá chế độ E/V theo app",        "Exclude apps",
              L"Khoá cứng chế độ gõ E hoặc V cho từng ứng dụng (vô hiệu hoá phím chuyển khi ở trong app)",
              L"Lock E or V typing mode for each application (disables hotkeys inside the app)",                     2207, 0, 1),
    NK_ACTION("btn-exclude-apps",     "...", "",
              nullptr, nullptr,                                          2502, 0, 1),
    NK_TYPING("tsf-apps",             tsfApps,
              "Dùng TSF cho danh sách", "Use TSF for listed apps",
              L"Các app trong danh sách sẽ dùng engine TSF, các app khác dùng Hook",
              L"Listed apps use TSF engine; others use the Hook",        2210, 0, 1),
    NK_ACTION("btn-tsf-apps",         "...", "",
              nullptr, nullptr,                                          2507, 0, 1),
    NK_TYPING_CHILD("hide-preedit-underline", hidePreeditUnderline,
              "Tắt gạch chân preedit", "Hide preedit underline",
              L"Không đảm bảo hoạt động 100% trên mọi ứng dụng.",
              L"Not guaranteed to work in every application.",          2221, 0, 1,
              "tsf-apps"),
    // ── Action buttons (grouped at bottom) ──
    NK_ACTION("btn-app-overrides",    "Cấu hình từng ứng dụng", "Per-app config",
              nullptr, nullptr,                                          2500, 0, 1),
    NK_ACTION("btn-spell-exclusions", "Loại trừ chính tả...", "Spell exclusions...",
              nullptr, nullptr,                                          2801, 0, 1),
    NK_ACTION("btn-hotkeys",          "Cấu hình phím tắt", "Configure hotkeys",
              L"Quản lý phím gán cho 3 thao tác: hủy đang gõ, bỏ qua gõ tắt, bật/tắt bộ gõ",
              L"Manage triggers for 3 intents: cancel composition, skip macro, toggle IME",
                                                                         2506, 0, 1),

    // ── Tab 1: Gõ tắt (col 0) ──
    NK_TYPING("use-macro",            macroEnabled,
              "Cho phép gõ tắt",      "Enable macros",
              nullptr, nullptr,                                          2212, 1, 0),
    NK_TYPING("macro-english",        macroInEnglish,
              "Gõ tắt khi tắt tiếng việt", "Macros in English mode",
              nullptr, nullptr,                                          2213, 1, 0),
    NK_TYPING("auto-caps-macro",      autoCapsMacro,
              "Tự động viết hoa theo phím", "Auto capitalize macros",
              nullptr, nullptr,                                          2219, 1, 0),
    NK_TYPING("macro-undo-backspace", macroUndoOnBackspace,
              "Khôi phục gõ tắt bằng Backspace", "Undo macro on Backspace",
              L"Sau khi bung nội dung gõ tắt, nhấn Backspace sẽ khôi phục lại từ gõ tắt ban đầu",
              L"Pressing Backspace right after macro expansion restores the typed shortcut",
                                                                         2223, 1, 0),
    NK_TYPING_CHILD("macro-title-case", macroTitleCase,
              "Viết hoa đầu từng từ", "Capitalize each word",
              L"Gõ Bbh → Bún Bò Huế. Khi tắt: Bbh → Bún bò Huế. Không áp dụng cho viết hoa đầu câu tự động.",
              L"Type Bbh → Bún Bò Huế. When off: Bbh → Bún bò Huế. Does not affect sentence auto-caps.",
                                                                         2222, 1, 0,
              "auto-caps-macro"),
    NK_ACTION("btn-macro-table",      "Bảng gõ tắt", "Macro Table",
              nullptr, nullptr,                                          2503, 1, 0),

    // ── Hotkeys (rendered in Compact mode, logically attached to Settings) ──
    NK_HOTKEY("key-ctrl",             ctrl,
              "Ctrl",                  "Ctrl",
              nullptr, nullptr,                                          2301, 1, 0),
    NK_HOTKEY("key-shift",            shift,
              "Shift",                 "Shift",
              nullptr, nullptr,                                          2302, 1, 0),
    NK_HOTKEY("key-alt",              alt,
              "Alt",                   "Alt",
              nullptr, nullptr,                                          2303, 1, 0),
    NK_HOTKEY("key-win",              win,
              "Win",                   "Win",
              nullptr, nullptr,                                          2304, 1, 0),

    // ── Tab 1: Gõ tắt (col 1) ──
    NK_TYPING("quick-telex",          quickConsonant,
              "Gõ nhanh phụ âm kép",  "Quick double consonant",
              L"cc=ch, gg=gi, kk=kh, nn=ng, qq=qu, pp=ph, tt=th, uu=ươ",
              L"cc=ch, gg=gi, kk=kh, nn=ng, qq=qu, pp=ph, tt=th, uu=ươ", 2214, 1, 1),
    NK_TYPING("quick-start",          quickStartConsonant,
              "Gõ tắt phụ âm đầu",    "Quick start consonant",
              L"f→ph, j→gi, w→qu",
              L"f→ph, j→gi, w→qu",                       2215, 1, 1),
    NK_TYPING("quick-end",            quickEndConsonant,
              "Gõ tắt phụ âm cuối",   "Quick end consonant",
              L"g→ng, h→nh, k→ch",
              L"g→ng, h→nh, k→ch",                       2216, 1, 1),

    // ── Tab 2: Hệ thống — Left column (col 0) ──
    NK_SYSTEM("run-startup",          runAtStartup,
              "Khởi động cùng Windows", "Run at startup",
              nullptr, nullptr,                                          2401, 2, 0),
    NK_DROPDOWN("startup-mode",       startupMode,
              "Chế độ mặc định",      "Default mode",
              L"Chế độ gõ khi khởi động ứng dụng",
              L"Typing mode when app starts",                            2409, 2, 0,
              kStartupModeItemsVi, kStartupModeItemsEn),
    NK_SYSTEM("show-on-startup",      showOnStartup,
              "Bật bảng này khi khởi động", "Show window on startup",
              nullptr, nullptr,                                          2403, 2, 0),
    NK_SYSTEM("run-admin",            runAsAdmin,
              "Chạy với quyền admin",  "Run as admin",
              nullptr, nullptr,                                          2402, 2, 0),
    NK_SYSTEM("desktop-shortcut",     desktopShortcut,
              "Tạo biểu tượng desktop","Desktop shortcut",
              nullptr, nullptr,                                          2404, 2, 0),

    NK_SYSTEM("english-ui",           language,
              "Giao diện tiếng Anh",  "English interface",
              L"Chuyển menu, thông báo sang tiếng Anh",
              L"Switch menus, notifications to English",            2407, 2, 0),

    // ── Tab 2: Hệ thống — Right column (col 1) ──
    NK_SYSTEM("floating-icon",        showFloatingIcon,
              "Icon V/E nổi",          "Floating icon",
              L"Hiện icon V/E nhỏ luôn nổi trên màn hình, hữu ích khi dùng app toàn màn hình",
              L"Show floating V/E indicator on screen, useful for fullscreen apps", 2405, 2, 1),
    NK_ACTION("btn-reset-floating-icon",
              "Đặt lại",              "Reset",
              L"Đặt lại vị trí của icon nổi về góc mặc định",
              L"Reset the floating icon position to default corner",    2508, 2, 1),
    NK_SYSTEM("tsf-indicator",        showTsfIndicator,
              "Hiện icon chữ T cho app TSF", "Show \"T\" icon for TSF apps",
              L"Khi app dùng TSF, hiện icon chữ T (đỏ=V, xanh=E) thay vì icon V/E thường. Tắt để luôn thấy V/E.",
              L"When an app uses TSF, show a \"T\" icon (red=V, blue=E) instead of the usual V/E icon. Off = always V/E.",
                                                                         2412, 2, 1),
    NK_DROPDOWN("custom-icon-style",  iconStyle,
              "Tuỳ chỉnh icon",       "Icon Style",
              nullptr, nullptr,                                          2408, 2, 1,
              kIconStyleItemsVi, kIconStyleItemsEn),
    NK_SYSTEM("check-update",         autoCheckUpdate,
              "Tự kiểm tra cập nhật", "Auto check update",
              L"Tự động kiểm tra phiên bản mới khi khởi động ứng dụng",
              L"Auto-check for new version on startup",                  2406, 2, 1),
    NK_ACTION("btn-check-update",     "Kiểm tra", "Check",
              nullptr, nullptr,                                          2504, 2, 1),
    NK_SYSTEM("force-light-theme",    forceLightTheme,
              "Luôn dùng giao diện sáng", "Always use light theme",
              L"Bỏ qua chế độ tối của Windows, luôn hiển thị giao diện sáng",
              L"Ignore Windows dark mode, always show light theme",     2410, 2, 1),
    NK_TYPING("debug-log",            debugLogEnabled,
              "Bật debug log",        "Enable debug log",
              L"Ghi log chi tiết để gửi kèm khi báo lỗi (VKey_*.log cạnh VKey.exe)",
              L"Write detailed log to attach when reporting bugs (VKey_*.log next to VKey.exe)",
                                                                         2411, 2, 1),
    NK_ACTION("btn-open-log-folder",  "Mở log", "Log folder",
              L"Mở thư mục chứa file log",
              L"Open log folder",                                        2505, 2, 1),
};

#undef NK_SETTING
#undef NK_TYPING
#undef NK_TYPING_CHILD
#undef NK_TYPING_DROPDOWN
#undef NK_HOTKEY
#undef NK_SYSTEM
#undef NK_DROPDOWN
#undef NK_ACTION

/// Total number of entries
inline constexpr size_t kSettingsCount = sizeof(kSettings) / sizeof(kSettings[0]);

// ── Lookup functions ───────────────────────────────────────────────

/// Find setting by Sciter DOM id string. Returns nullptr if not found.
[[nodiscard]] inline constexpr const SettingMeta* FindSetting(const wchar_t* id) noexcept {
    for (size_t i = 0; i < kSettingsCount; ++i) {
        // constexpr-friendly wchar_t comparison
        const wchar_t* a = kSettings[i].id;
        const wchar_t* b = id;
        bool match = true;
        while (*a || *b) {
            if (*a != *b) { match = false; break; }
            ++a; ++b;
        }
        if (match) return &kSettings[i];
    }
    return nullptr;
}

/// Find setting by Win32 control ID. Returns nullptr if not found.
[[nodiscard]] inline constexpr const SettingMeta* FindSettingByControlId(uint16_t controlId) noexcept {
    if (controlId == 0) return nullptr;
    for (size_t i = 0; i < kSettingsCount; ++i) {
        if (kSettings[i].win32Id == controlId) return &kSettings[i];
    }
    return nullptr;
}

}  // namespace NextKey
