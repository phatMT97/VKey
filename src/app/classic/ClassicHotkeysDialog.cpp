// VKey Classic — Unified Hotkey Rebind Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ClassicHotkeysDialog.h"

#ifdef _WIN32

#include "ClassicHotkeyCapture.h"
#include "core/config/ConfigManager.h"
#include "core/CrashLog.h"
#include "core/Debug.h"
#include "app/helpers/AppHelpers.h"

#include <windowsx.h>
#include <exception>
#include <string>
#include <unordered_map>

namespace NextKey::Classic {

namespace {

constexpr UINT IDC_ENABLE_CANCEL = 4101;
constexpr UINT IDC_LIST_CANCEL   = 4102;
constexpr UINT IDC_ADD_CANCEL    = 4103;
constexpr UINT IDC_DEL_CANCEL    = 4104;
constexpr UINT IDC_RESET_CANCEL  = 4105;

constexpr UINT IDC_ENABLE_SKIP   = 4111;
constexpr UINT IDC_LIST_SKIP     = 4112;
constexpr UINT IDC_ADD_SKIP      = 4113;
constexpr UINT IDC_DEL_SKIP      = 4114;
constexpr UINT IDC_RESET_SKIP    = 4115;

constexpr UINT IDC_ENABLE_TOGGLE = 4121;
constexpr UINT IDC_LIST_TOGGLE   = 4122;
constexpr UINT IDC_ADD_TOGGLE    = 4123;
constexpr UINT IDC_DEL_TOGGLE    = 4124;
constexpr UINT IDC_RESET_TOGGLE  = 4125;

constexpr UINT IDC_ENABLE_GAME   = 4131;
constexpr UINT IDC_LIST_GAME     = 4132;
constexpr UINT IDC_ADD_GAME      = 4133;
constexpr UINT IDC_DEL_GAME      = 4134;
constexpr UINT IDC_RESET_GAME    = 4135;

constexpr UINT IDC_BTN_CLOSE_HK  = 4150;

// ── VK → display name table (mirrors HotkeysDialog.cpp kVkNames) ──
const std::unordered_map<uint32_t, const wchar_t*> kVkNames = {
    {0x08, L"Backspace"}, {0x09, L"Tab"},   {0x0D, L"Enter"},
    {0x10, L"Shift"},     {0x11, L"Ctrl"},  {0x12, L"Alt"},
    {0x13, L"Pause"},     {0x14, L"Caps"},  {0x1B, L"Esc"},
    {0x20, L"Space"},
    {0x21, L"PgUp"},      {0x22, L"PgDn"},  {0x23, L"End"},  {0x24, L"Home"},
    {0x25, L"←"},    {0x26, L"↑"},{0x27, L"→"},{0x28, L"↓"},
    {0x2C, L"PrtSc"},     {0x2D, L"Insert"}, {0x2E, L"Del"},
    {0x5B, L"Win"},       {0x5C, L"Win"},   {0x5D, L"Menu"},
    {0xBA, L";"}, {0xBB, L"="}, {0xBC, L","}, {0xBD, L"-"}, {0xBE, L"."}, {0xBF, L"/"},
    {0xC0, L"`"}, {0xDB, L"["}, {0xDC, L"\\"}, {0xDD, L"]"}, {0xDE, L"'"},
};

[[nodiscard]] std::wstring FormatTriggerLabel(const Trigger& t) {
    std::wstring prefix;
    if (t.mods & kModCtrl)  prefix += L"Ctrl+";
    if (t.mods & kModShift) prefix += L"Shift+";
    if (t.mods & kModAlt)   prefix += L"Alt+";
    if (t.mods & kModWin)   prefix += L"Win+";

    // Modifier-alone (no other mods + vk is itself a modifier)
    if (t.mods == 0 && !t.doubleTap && IsModifierKey(t.vk)) {
        switch (t.vk) {
        case 0x11: return L"Ctrl (giữ-thả)";
        case 0x12: return L"Alt (giữ-thả)";
        case 0x10: return L"Shift (giữ-thả)";
        case 0x5B: case 0x5C: return L"Win (giữ-thả)";
        }
    }

    std::wstring keyName;
    if (auto it = kVkNames.find(t.vk); it != kVkNames.end()) {
        keyName = it->second;
    } else if (t.vk >= 0x60 && t.vk <= 0x69) {
        keyName = L"Num" + std::to_wstring(t.vk - 0x60);
    } else if (t.vk >= 0x70 && t.vk <= 0x87) {
        keyName = L"F" + std::to_wstring(t.vk - 0x6F);
    } else if ((t.vk >= 'A' && t.vk <= 'Z') || (t.vk >= '0' && t.vk <= '9')) {
        keyName.push_back(static_cast<wchar_t>(t.vk));
    } else {
        keyName = L"VK_" + std::to_wstring(t.vk);
    }

    if (t.doubleTap) {
        // doubleTap implies mods=0 — strip any prefix accidentally accumulated.
        return L"2×" + keyName;
    }
    return prefix + keyName;
}

[[nodiscard]] bool IsModifierVk(uint32_t vk) noexcept {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU
        || vk == VK_LWIN  || vk == VK_RWIN;
}

[[nodiscard]] UINT IntentToIndex(Intent intent) noexcept {
    return static_cast<UINT>(intent);  // 0/1/2 — matches kAllIntents order
}

// ── Section labels ──
struct SectionMeta {
    Intent           intent;
    const wchar_t*   title;
    UINT             idEnable;
    UINT             idList;
    UINT             idAdd;
    UINT             idDelete;
    UINT             idReset;
};

constexpr SectionMeta kSectionMeta[4] = {
    { Intent::CancelComposition, L"Hủy phiên đang gõ",
      IDC_ENABLE_CANCEL, IDC_LIST_CANCEL, IDC_ADD_CANCEL, IDC_DEL_CANCEL, IDC_RESET_CANCEL },
    { Intent::SkipMacro,         L"Bỏ qua gõ tắt",
      IDC_ENABLE_SKIP,   IDC_LIST_SKIP,   IDC_ADD_SKIP,   IDC_DEL_SKIP,   IDC_RESET_SKIP   },
    { Intent::ToggleEnabled,     L"Tạm tắt bộ gõ",
      IDC_ENABLE_TOGGLE, IDC_LIST_TOGGLE, IDC_ADD_TOGGLE, IDC_DEL_TOGGLE, IDC_RESET_TOGGLE },
    { Intent::ToggleGameMode,    L"Bật / tắt chế độ game cho app đang mở",
      IDC_ENABLE_GAME,   IDC_LIST_GAME,   IDC_ADD_GAME,   IDC_DEL_GAME,   IDC_RESET_GAME   },
};

}  // namespace

// ════════════════════════════════════════════════════════════
// Public entry
// ════════════════════════════════════════════════════════════

bool ClassicHotkeysDialog::Show(HINSTANCE hInstance, HWND parent, bool forceLightTheme) {
    ClassicHotkeysDialog dlg;
    if (!dlg.Init(hInstance, parent, forceLightTheme)) return false;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg.hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return dlg.modified_;
}

int ClassicHotkeysDialog::Dpi(int value) const noexcept {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

// ════════════════════════════════════════════════════════════
// Init / Layout
// ════════════════════════════════════════════════════════════

bool ClassicHotkeysDialog::Init(HINSTANCE hInstance, HWND parent, bool forceLightTheme) {
    hInstance_ = hInstance;
    registry_  = ConfigManager::LoadHotkeyRegistryOrDefault();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.cbWndExtra    = sizeof(void*);
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    hwnd_ = CreateWindowExW(0, kClassName, L"Cấu hình phím tắt",
        style, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
        parent, nullptr, hInstance, this);
    if (!hwnd_) return false;

    dpi_ = Classic::GetWindowDpi(hwnd_);
    theme_.Init(hwnd_, forceLightTheme);
    theme_.ApplyWindowAttributes(hwnd_);

    const int w = Dpi(kWidth), h = Dpi(kHeight);
    RECT rc{0, 0, w, h};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int aw = rc.right - rc.left, ah = rc.bottom - rc.top;
    POINT pt = NextKey::GetCenteredPos(hwnd_, aw, ah);
    SetWindowPos(hwnd_, nullptr, pt.x, pt.y, aw, ah, SWP_NOZORDER);

    CreateControls();
    RepopulateAll();

    EnumChildWindows(hwnd_, [](HWND h, LPARAM lp) -> BOOL {
        auto* self = reinterpret_cast<ClassicHotkeysDialog*>(lp);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(self->theme_.Fonts().body), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    theme_.ThemeAllChildren(hwnd_);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void ClassicHotkeysDialog::CreateControls() {
    const int pad      = Dpi(kPadding);
    const int gap      = Dpi(kSectionGap);
    const int sectionH = Dpi(kSectionH);
    const int headerH  = Dpi(kHeaderH);
    const int listH    = Dpi(kListH);
    const int btnH     = Dpi(kBtnH);
    const int checkW   = Dpi(kCheckW);
    const int addW     = Dpi(kAddW);
    const int deleteW  = Dpi(kDeleteW);
    const int resetW   = Dpi(kResetW);
    const int btnGap   = Dpi(6);
    const int contentW = Dpi(kWidth) - pad * 2;

    int y = pad;
    for (const auto& meta : kSectionMeta) {
        SectionUI& s = sections_[IntentToIndex(meta.intent)];

        // Header label (left) — section title
        s.label = CreateWindowExW(0, L"STATIC", meta.title,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, y, contentW - checkW - Dpi(8), headerH,
            hwnd_, nullptr, hInstance_, nullptr);

        // Enable checkbox (right)
        s.enable = CreateWindowExW(0, L"BUTTON", L"Bật",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            pad + contentW - checkW, y, checkW, headerH,
            hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(meta.idEnable)),
            hInstance_, nullptr);

        const int listY = y + headerH + Dpi(4);

        // Chip list (single-column LVS_REPORT)
        s.listView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL
                | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
            pad, listY, contentW, listH,
            hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(meta.idList)),
            hInstance_, nullptr);
        ListView_SetExtendedListViewStyle(s.listView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(L"");
        col.cx = contentW - Dpi(24);  // leave space for scrollbar
        ListView_InsertColumn(s.listView, 0, &col);

        const int btnY = listY + listH + Dpi(6);

        // Buttons row: [+ Thêm] [Xóa] [Mặc định]
        int bx = pad;
        s.btnAdd = CreateWindowExW(0, L"BUTTON", L"+ Thêm",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            bx, btnY, addW, btnH,
            hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(meta.idAdd)),
            hInstance_, nullptr);
        bx += addW + btnGap;

        s.btnDelete = CreateWindowExW(0, L"BUTTON", L"Xóa",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            bx, btnY, deleteW, btnH,
            hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(meta.idDelete)),
            hInstance_, nullptr);
        bx += deleteW + btnGap;

        s.btnReset = CreateWindowExW(0, L"BUTTON", L"Mặc định",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            bx, btnY, resetW, btnH,
            hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(meta.idReset)),
            hInstance_, nullptr);

        y += sectionH + gap;
    }

    // Footer — Close button right-aligned
    const int closeW = Dpi(kCloseW);
    const int closeY = Dpi(kHeight) - pad - btnH;
    btnClose_ = CreateWindowExW(0, L"BUTTON", L"Đóng",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
        pad + contentW - closeW, closeY, closeW, btnH,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_CLOSE_HK)),
        hInstance_, nullptr);
}

// ════════════════════════════════════════════════════════════
// Populate
// ════════════════════════════════════════════════════════════

void ClassicHotkeysDialog::RepopulateAll() {
    for (Intent intent : kAllIntents) RepopulateSection(intent);
}

void ClassicHotkeysDialog::RepopulateSection(Intent intent) {
    SectionUI& s = sections_[IntentToIndex(intent)];

    SendMessageW(s.enable, BM_SETCHECK,
        registry_.IsEnabled(intent) ? BST_CHECKED : BST_UNCHECKED, 0);

    ListView_DeleteAllItems(s.listView);
    int i = 0;
    for (const Trigger& t : registry_.TriggersFor(intent)) {
        std::wstring label = FormatTriggerLabel(t);
        LVITEMW item{};
        item.mask     = LVIF_TEXT;
        item.iItem    = i++;
        item.pszText  = label.data();
        ListView_InsertItem(s.listView, &item);
    }

    const bool enabled = registry_.IsEnabled(intent);
    const COLORREF textCol = enabled ? theme_.Colors().text : theme_.Colors().textSecondary;
    const COLORREF bkCol = theme_.IsDark() ? theme_.Colors().background : GetSysColor(COLOR_WINDOW);

    ListView_SetTextColor(s.listView, textCol);
    ListView_SetBkColor(s.listView, bkCol);
    ListView_SetTextBkColor(s.listView, bkCol);

    EnableWindow(s.btnAdd, enabled);
    EnableWindow(s.btnDelete, enabled);
    EnableWindow(s.btnReset, enabled);

    EnableWindow(s.listView, TRUE);
    InvalidateRect(s.listView, nullptr, TRUE);
}

// ════════════════════════════════════════════════════════════
// Actions
// ════════════════════════════════════════════════════════════

void ClassicHotkeysDialog::OnAddClicked(Intent intent) {
    Trigger captured{};
    if (!CaptureTrigger(captured)) return;

    // Skip duplicates within the same intent.
    for (const Trigger& existing : registry_.TriggersFor(intent)) {
        if (existing == captured) return;
    }
    registry_.AddTrigger(intent, captured);
    RepopulateSection(intent);
    PersistAndSignal();
}

void ClassicHotkeysDialog::OnResetClicked(Intent intent) {
    // Replace this intent's triggers with the factory defaults for that
    // intent. Other intents and their enabled state are untouched.
    HotkeyRegistry def = HotkeyRegistry::Defaults();

    HotkeyRegistry rebuilt;
    for (Intent i : kAllIntents) {
        rebuilt.SetEnabled(i, registry_.IsEnabled(i));
        if (i == intent) {
            rebuilt.SetEnabled(i, true);  // reset implies enable
            for (const Trigger& t : def.TriggersFor(i)) rebuilt.AddTrigger(i, t);
        } else {
            for (const Trigger& t : registry_.TriggersFor(i)) rebuilt.AddTrigger(i, t);
        }
    }
    registry_ = std::move(rebuilt);
    RepopulateSection(intent);
    PersistAndSignal();
}

void ClassicHotkeysDialog::OnDeleteSelected(Intent intent) {
    SectionUI& s = sections_[IntentToIndex(intent)];
    int sel = ListView_GetNextItem(s.listView, -1, LVNI_SELECTED);
    if (sel < 0) return;

    const auto& triggers = registry_.TriggersFor(intent);
    if (sel >= static_cast<int>(triggers.size())) return;
    const Trigger victim = triggers[sel];

    HotkeyRegistry rebuilt;
    for (Intent i : kAllIntents) {
        rebuilt.SetEnabled(i, registry_.IsEnabled(i));  // preserve toggles
        for (const Trigger& existing : registry_.TriggersFor(i)) {
            if (i == intent && existing == victim) continue;
            rebuilt.AddTrigger(i, existing);
        }
    }
    registry_ = std::move(rebuilt);
    RepopulateSection(intent);
    PersistAndSignal();
}

void ClassicHotkeysDialog::OnEnableToggled(Intent intent, bool enabled) {
    registry_.SetEnabled(intent, enabled);
    NEXTKEY_LOG(L"ClassicHotkeysDialog: SetEnabled intent=%d enabled=%d",
                static_cast<int>(intent), enabled);
    RepopulateSection(intent);
    PersistAndSignal();
}

void ClassicHotkeysDialog::OnCloseClicked() {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

bool ClassicHotkeysDialog::CaptureTrigger(Trigger& outTrigger) {
    HotkeyCaptureOptions opts;  // defaults: allowDoubleTap=true, allowBareModifier=true
    auto r = ShowHotkeyCaptureDialog(hInstance_, hwnd_, theme_, dpi_, opts);
    if (!r) return false;
    outTrigger = Trigger{r->vk, r->mods, r->doubleTap};
    return true;
}

// ════════════════════════════════════════════════════════════
// Persistence
// ════════════════════════════════════════════════════════════

void ClassicHotkeysDialog::PersistAndSignal() {
    modified_ = true;
    auto path = ConfigManager::GetConfigPath();
    if (!ConfigManager::SaveHotkeyRegistry(path, registry_)) {
        NEXTKEY_LOG(L"ClassicHotkeysDialog: SaveHotkeyRegistry failed for %s", path.c_str());
    }
    SignalConfigChange();
}

// ════════════════════════════════════════════════════════════
// WndProc
// ════════════════════════════════════════════════════════════

LRESULT CALLBACK ClassicHotkeysDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) try {
    ClassicHotkeysDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<ClassicHotkeysDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<ClassicHotkeysDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        const UINT code = HIWORD(wParam);

        // Enable checkboxes
        if (code == BN_CLICKED) {
            if (id == IDC_ENABLE_CANCEL || id == IDC_ENABLE_SKIP
                || id == IDC_ENABLE_TOGGLE || id == IDC_ENABLE_GAME) {
                const HWND ck = reinterpret_cast<HWND>(lParam);
                const bool checked = SendMessageW(ck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Intent intent =
                    (id == IDC_ENABLE_CANCEL) ? Intent::CancelComposition :
                    (id == IDC_ENABLE_SKIP)   ? Intent::SkipMacro :
                    (id == IDC_ENABLE_TOGGLE) ? Intent::ToggleEnabled :
                                                 Intent::ToggleGameMode;
                self->OnEnableToggled(intent, checked);
                return 0;
            }
        }

        switch (id) {
        case IDC_ADD_CANCEL:   self->OnAddClicked(Intent::CancelComposition); return 0;
        case IDC_ADD_SKIP:     self->OnAddClicked(Intent::SkipMacro);         return 0;
        case IDC_ADD_TOGGLE:   self->OnAddClicked(Intent::ToggleEnabled);     return 0;
        case IDC_ADD_GAME:     self->OnAddClicked(Intent::ToggleGameMode);    return 0;
        case IDC_DEL_CANCEL:   self->OnDeleteSelected(Intent::CancelComposition); return 0;
        case IDC_DEL_SKIP:     self->OnDeleteSelected(Intent::SkipMacro);         return 0;
        case IDC_DEL_TOGGLE:   self->OnDeleteSelected(Intent::ToggleEnabled);     return 0;
        case IDC_DEL_GAME:     self->OnDeleteSelected(Intent::ToggleGameMode);    return 0;
        case IDC_RESET_CANCEL: self->OnResetClicked(Intent::CancelComposition); return 0;
        case IDC_RESET_SKIP:   self->OnResetClicked(Intent::SkipMacro);         return 0;
        case IDC_RESET_TOGGLE: self->OnResetClicked(Intent::ToggleEnabled);     return 0;
        case IDC_BTN_CLOSE_HK: self->OnCloseClicked(); return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        // Del on a focused ListView removes the selected chip.
        if (wParam == VK_DELETE) {
            HWND focus = GetFocus();
            for (size_t i = 0; i < self->sections_.size(); ++i) {
                if (focus == self->sections_[i].listView) {
                    self->OnDeleteSelected(static_cast<Intent>(i));
                    return 0;
                }
            }
        }
        break;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (nm->code == LVN_KEYDOWN) {
            auto* keyEvent = reinterpret_cast<NMLVKEYDOWN*>(lParam);
            if (keyEvent->wVKey == VK_DELETE) {
                for (size_t i = 0; i < self->sections_.size(); ++i) {
                    if (nm->hwndFrom == self->sections_[i].listView) {
                        self->OnDeleteSelected(static_cast<Intent>(i));
                        return 0;
                    }
                }
            }
        } else if (nm->code == LVN_ITEMCHANGING) {
            auto* pnm = reinterpret_cast<NMLISTVIEW*>(lParam);
            for (size_t i = 0; i < self->sections_.size(); ++i) {
                if (nm->hwndFrom == self->sections_[i].listView) {
                    if (!self->registry_.IsEnabled(static_cast<Intent>(i))) {
                        if (pnm->uNewState & (LVIS_SELECTED | LVIS_FOCUSED)) {
                            return TRUE; // Prevent selection/focus when visually disabled
                        }
                    }
                    break;
                }
            }
        }
        break;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, self->theme_.BrushBackground());
        return 1;
    }

    case WM_CTLCOLORSTATIC:
        return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorStatic(
            reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorBtn(
            reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorListBox(
            reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
    case WM_CTLCOLOREDIT:
        return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorEdit(
            reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        self->theme_.Destroy();
        self->hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
} catch (const std::exception& e) {
    NextKey::CrashLog(L"ClassicHotkeysDialog::WndProc", e.what());
    return DefWindowProcW(hwnd, msg, wParam, lParam);
} catch (...) {
    NextKey::CrashLog(L"ClassicHotkeysDialog::WndProc", "(non-std exception)");
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace NextKey::Classic

#endif  // _WIN32
