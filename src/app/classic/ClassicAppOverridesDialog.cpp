// VKey Classic — App Overrides Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ClassicAppOverridesDialog.h"
#include "core/CrashLog.h"
#include "app/helpers/AppHelpers.h"

#include <windowsx.h>
#include <algorithm>
#include <exception>
#include <vector>

namespace NextKey::Classic {

enum {
    IDC_LIST_OVERRIDES = 3201,
    IDC_COMBO_APP,
    IDC_COMBO_METHOD,
    IDC_COMBO_ENCODING,
    IDC_COMBO_SENDMETHOD,
    IDC_BTN_ADD,
    IDC_BTN_PICK,
    IDC_BTN_DELETE,
};

// ════════════════════════════════════════════════════════════

bool ClassicAppOverridesDialog::Show(HINSTANCE hInstance, HWND parent, bool forceLightTheme) {
    ClassicAppOverridesDialog dlg;
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

bool ClassicAppOverridesDialog::Init(HINSTANCE hInstance, HWND parent, bool forceLightTheme) {
    hInstance_ = hInstance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbWndExtra = sizeof(void*);
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"Tuỳ chỉnh theo ứng dụng",
        style, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
        parent, nullptr, hInstance, this);
    if (!hwnd_) return false;

    dpi_ = Classic::GetWindowDpi(hwnd_);

    int w = Dpi(kWidth), h = Dpi(kHeight);
    RECT rc = {0, 0, w, h};
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOPMOST);
    int aw = rc.right - rc.left, ah = rc.bottom - rc.top;
    POINT pt = NextKey::GetCenteredPos(hwnd_, aw, ah);
    SetWindowPos(hwnd_, nullptr, pt.x, pt.y, aw, ah, SWP_NOZORDER);

    theme_.Init(hwnd_, forceLightTheme);
    theme_.ApplyWindowAttributes(hwnd_);

    LoadData();
    CreateControls();
    PopulateList();

    EnumChildWindows(hwnd_, [](HWND h, LPARAM lp) -> BOOL {
        auto* self = reinterpret_cast<ClassicAppOverridesDialog*>(lp);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(self->theme_.Fonts().body), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    theme_.ThemeAllChildren(hwnd_);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

// ════════════════════════════════════════════════════════════

void ClassicAppOverridesDialog::CreateControls() {
    int x = Dpi(kPadding), y = Dpi(kPadding);
    int cw = Dpi(kWidth - kPadding * 2);
    int gap = Dpi(kBtnGap);

    // ListView: app | method | encoding
    int listH = Dpi(200);
    listView_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        x, y, cw, listH, hwnd_, reinterpret_cast<HMENU>(IDC_LIST_OVERRIDES), hInstance_, nullptr);
    ListView_SetExtendedListViewStyle(listView_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Ứng dụng");
    col.cx = Dpi(120);
    ListView_InsertColumn(listView_, 0, &col);
    col.pszText = const_cast<wchar_t*>(L"Kiểu gõ");
    col.cx = Dpi(80);
    ListView_InsertColumn(listView_, 1, &col);
    col.pszText = const_cast<wchar_t*>(L"Bảng mã");
    col.cx = Dpi(100);
    ListView_InsertColumn(listView_, 2, &col);
    col.pszText = const_cast<wchar_t*>(L"Kiểu gửi");
    col.cx = cw - Dpi(120 + 80 + 100 + 24);
    ListView_InsertColumn(listView_, 3, &col);
    y += listH + gap;

    // Row 1: app combobox + add + pick button
    int rowH = Dpi(26);  // compact row height for this dialog
    int comboInnerH = rowH - Dpi(6); // inner selection field height
    int pickW = Dpi(110);
    int addW = Dpi(60);
    int appComboW = cw - addW - pickW - gap * 2;
    comboApp_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
        x, y, appComboW, Dpi(200), hwnd_, reinterpret_cast<HMENU>(IDC_COMBO_APP), hInstance_, nullptr);
    SendMessageW(comboApp_, CB_SETITEMHEIGHT, (WPARAM)-1, comboInnerH);

    auto running = GetRunningApps();
    for (auto& app : running) {
        ComboBox_AddString(comboApp_, app.c_str());
    }

    btnAdd_ = CreateWindowExW(0, L"BUTTON", L"Thêm",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + appComboW + gap, y, addW, rowH,
        hwnd_, reinterpret_cast<HMENU>(IDC_BTN_ADD), hInstance_, nullptr);

    btnPick_ = CreateWindowExW(0, L"BUTTON", L"Chọn cửa sổ",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + appComboW + addW + gap * 2, y, pickW, rowH,
        hwnd_, reinterpret_cast<HMENU>(IDC_BTN_PICK), hInstance_, nullptr);
    y += rowH + gap * 2;  // breathing room before row 2

    // Row 2: method combo + encoding combo + delete button
    int lblW = Dpi(55);
    int comboW = Dpi(120);
    int lblH = Dpi(20);
    int labelY = y + (rowH - lblH) / 2;

    CreateWindowExW(0, L"STATIC", L"Kiểu gõ:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, labelY, lblW, lblH, hwnd_, nullptr, hInstance_, nullptr);
    comboMethod_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        x + lblW, y, comboW, Dpi(120), hwnd_, reinterpret_cast<HMENU>(IDC_COMBO_METHOD), hInstance_, nullptr);
    SendMessageW(comboMethod_, CB_SETITEMHEIGHT, (WPARAM)-1, comboInnerH);
    ComboBox_AddString(comboMethod_, L"Theo mặc định");
    ComboBox_AddString(comboMethod_, L"Telex");
    ComboBox_AddString(comboMethod_, L"VNI");
    ComboBox_AddString(comboMethod_, L"Simple Telex");
    ComboBox_AddString(comboMethod_, L"Telex + VNI");
    ComboBox_AddString(comboMethod_, L"Tự định nghĩa");
    ComboBox_SetCurSel(comboMethod_, 0);

    int col2X = x + lblW + comboW + gap;
    CreateWindowExW(0, L"STATIC", L"Bảng mã:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        col2X, labelY, lblW, lblH, hwnd_, nullptr, hInstance_, nullptr);
    comboEncoding_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        col2X + lblW, y, comboW, Dpi(120), hwnd_, reinterpret_cast<HMENU>(IDC_COMBO_ENCODING), hInstance_, nullptr);
    SendMessageW(comboEncoding_, CB_SETITEMHEIGHT, (WPARAM)-1, comboInnerH);
    ComboBox_AddString(comboEncoding_, L"Theo mặc định");
    ComboBox_AddString(comboEncoding_, L"Unicode");
    ComboBox_AddString(comboEncoding_, L"TCVN3");
    ComboBox_AddString(comboEncoding_, L"VNI Windows");
    ComboBox_AddString(comboEncoding_, L"Unicode tổ hợp");
    ComboBox_AddString(comboEncoding_, L"Việt CP1258");
    ComboBox_SetCurSel(comboEncoding_, 0);

    // Row 3: Send Method combo + Delete button
    int y3 = y + rowH + gap;
    int labelY3 = y3 + (rowH - lblH) / 2;

    CreateWindowExW(0, L"STATIC", L"Kiểu gửi:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, labelY3, lblW, lblH, hwnd_, nullptr, hInstance_, nullptr);
    comboSendMethod_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        x + lblW, y3, comboW, Dpi(120), hwnd_, reinterpret_cast<HMENU>(IDC_COMBO_SENDMETHOD), hInstance_, nullptr);
    SendMessageW(comboSendMethod_, CB_SETITEMHEIGHT, (WPARAM)-1, comboInnerH);
    SendMessageW(comboSendMethod_, CB_SETDROPPEDWIDTH, Dpi(280), 0);
    ComboBox_AddString(comboSendMethod_, L"Theo mặc định");
    ComboBox_AddString(comboSendMethod_, L"Clipboard");
    ComboBox_AddString(comboSendMethod_, L"Tương thích Firefox");
    ComboBox_AddString(comboSendMethod_, L"Tương thích Cloud/Remote");
    ComboBox_AddString(comboSendMethod_, L"Thay thế trực tiếp (EM_REPLACESEL)");
    ComboBox_AddString(comboSendMethod_, L"Chế độ game (giữ phím cho game)");
    ComboBox_AddString(comboSendMethod_, L"Tương thích WebView App");
    ComboBox_SetCurSel(comboSendMethod_, 0);

    int deleteW = Dpi(70);
    btnDelete_ = CreateWindowExW(0, L"BUTTON", L"Xoá",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x + cw - deleteW, y3, deleteW, rowH, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_DELETE), hInstance_, nullptr);

    y = y3 + rowH + gap * 2;
}

static const wchar_t* MethodName(int8_t m) {
    switch (m) {
        case 0: return L"Telex";
        case 1: return L"VNI";
        case 2: return L"Simple Telex";
        case 3: return L"Telex + VNI";
        case 4: return L"Tự định nghĩa";
        default: return L"Mặc định";
    }
}

static const wchar_t* EncodingName(int8_t e) {
    switch (e) {
        case 0: return L"Unicode";
        case 1: return L"TCVN3";
        case 2: return L"VNI Windows";
        case 3: return L"Unicode tổ hợp";
        case 4: return L"Việt CP1258";
        default: return L"Mặc định";
    }
}

static const wchar_t* SendMethodName(int8_t m) {
    switch (m) {
        case 1: return L"Clipboard";
        case 2: return L"Firefox";
        case 3: return L"Cloud/Remote";
        case 4: return L"Thay thế trực tiếp";
        case 5: return L"Chế độ game";
        case 6: return L"WebView App";
        default: return L"Mặc định";
    }
}

void ClassicAppOverridesDialog::PopulateList() {
    ListView_DeleteAllItems(listView_);

    std::vector<std::pair<std::wstring, AppOverrideEntry>> sorted(entries_.begin(), entries_.end());
    std::sort(sorted.begin(), sorted.end(),
        [](auto& a, auto& b) { return a.first < b.first; });

    for (int i = 0; i < static_cast<int>(sorted.size()); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(sorted[i].first.c_str());
        ListView_InsertItem(listView_, &item);

        ListView_SetItemText(listView_, i, 1, const_cast<wchar_t*>(MethodName(sorted[i].second.inputMethod)));
        ListView_SetItemText(listView_, i, 2, const_cast<wchar_t*>(EncodingName(sorted[i].second.encodingOverride)));
        ListView_SetItemText(listView_, i, 3, const_cast<wchar_t*>(SendMethodName(sorted[i].second.sendMethod)));
    }
}

// ════════════════════════════════════════════════════════════

void ClassicAppOverridesDialog::AddOverride() {
    wchar_t buf[256] = {};
    GetWindowTextW(comboApp_, buf, 256);
    std::wstring app = ToLowerAscii(buf);
    if (app.empty()) return;

    int methodSel = ComboBox_GetCurSel(comboMethod_);
    int encodingSel = ComboBox_GetCurSel(comboEncoding_);
    int sendSel = ComboBox_GetCurSel(comboSendMethod_);

    AppOverrideEntry entry;
    entry.inputMethod = static_cast<int8_t>(methodSel - 1);   // 0="default"→-1, 1=Telex→0, etc.
    entry.encodingOverride = static_cast<int8_t>(encodingSel - 1);
    // Combo index lines up 1:1 with the stored value (1=Clipboard,
    // 2=Firefox-compat, 3=Cloud/Remote-compat); index 0 ("Theo mặc định") → -1.
    entry.sendMethod = static_cast<int8_t>((sendSel >= 1) ? sendSel : -1);

    entries_[app] = entry;
    PopulateList();
    SaveData();

    SetWindowTextW(comboApp_, L"");
    ComboBox_SetCurSel(comboMethod_, 0);
    ComboBox_SetCurSel(comboEncoding_, 0);
    ComboBox_SetCurSel(comboSendMethod_, 0);
}

void ClassicAppOverridesDialog::DeleteSelected() {
    int sel = ListView_GetNextItem(listView_, -1, LVNI_SELECTED);
    if (sel < 0) return;

    wchar_t key[256] = {};
    ListView_GetItemText(listView_, sel, 0, key, 256);
    entries_.erase(key);
    PopulateList();
    SaveData();
}

void ClassicAppOverridesDialog::OnPickWindow() {
    picker_.Start(hwnd_, [this](const std::wstring& exeName) {
        SetWindowTextW(comboApp_, exeName.c_str());
    });
}

void ClassicAppOverridesDialog::LoadData() {
    entries_ = ConfigManager::LoadAppOverrides(ConfigManager::GetConfigPath());
}

void ClassicAppOverridesDialog::SaveData() {
    modified_ = true;
    (void)ConfigManager::SaveAppOverrides(ConfigManager::GetConfigPath(), entries_);
    SignalConfigChange();
}

int ClassicAppOverridesDialog::Dpi(int value) const noexcept {
    return Classic::DpiScale(value, dpi_);
}

// ════════════════════════════════════════════════════════════

LRESULT CALLBACK ClassicAppOverridesDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) try {
    ClassicAppOverridesDialog* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<ClassicAppOverridesDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<ClassicAppOverridesDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (self->picker_.HandleMessage(msg, wParam)) return 0;

    switch (msg) {
        case WM_COMMAND: {
            UINT id = LOWORD(wParam);
            switch (id) {
                case IDC_BTN_ADD:       self->AddOverride();    return 0;
                case IDC_BTN_PICK:      self->OnPickWindow();   return 0;
                case IDC_BTN_DELETE:    self->DeleteSelected(); return 0;
            }
            break;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, self->theme_.BrushBackground());
            return 1;
        }

        case WM_CTLCOLORSTATIC:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorStatic(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
        case WM_CTLCOLOREDIT:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorEdit(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
        case WM_CTLCOLORLISTBOX:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorListBox(
                reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(self->theme_.OnCtlColorBtn(
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
    NextKey::CrashLog(L"ClassicAppOverridesDialog::WndProc", e.what());
    return DefWindowProcW(hwnd, msg, wParam, lParam);
} catch (...) {
    NextKey::CrashLog(L"ClassicAppOverridesDialog::WndProc", "(non-std exception)");
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace NextKey::Classic
