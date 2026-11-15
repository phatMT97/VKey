// VKey Classic — Native Windows Theme Engine
// SPDX-License-Identifier: GPL-3.0-only

#include "ClassicTheme.h"
#include "DarkModeHelper.h"
#include <CommCtrl.h>
#include <uxtheme.h>
#include <vssym32.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// Undocumented uxtheme.dll APIs for dark mode (Win10 1903+)
using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);

namespace NextKey::Classic {

// -- Lifecycle --

ClassicTheme::~ClassicTheme() {
    Destroy();
}

void ClassicTheme::Init(HWND hwnd, bool forceLightTheme) {
    hwnd_ = hwnd;
    forceLightTheme_ = forceLightTheme;
    DetectDarkMode();
    RefreshColors();

    // Enable dark mode at app level (affects native combobox, scrollbar, etc.)
    DarkModeHelper::ApplyDarkModeForApp();

    // Destroy old fonts before recreating them to prevent GDI leak
    DestroyFonts();
    dpi_ = Classic::GetWindowDpi(hwnd);
    CreateFonts(dpi_);
    CreateBrushes();
    ApplyWindowAttributes(hwnd);
}

void ClassicTheme::Destroy() {
    DestroyBrushes();
    DestroyFonts();
}

void ClassicTheme::DetectDarkMode() {
    if (forceLightTheme_) {
        isDark_ = false;
        return;
    }
    isDark_ = DarkModeHelper::IsWindowsDarkMode();
}

void ClassicTheme::RefreshColors() {
    // GetSysColor() does NOT change in dark mode for Win32 apps — only UWP/WinUI
    // gets automatic dark system colors. So we use system colors for light mode
    // and provide our own neutral dark palette.
    if (isDark_) {
        colors_.background    = RGB( 32,  32,  32);  // #202020
        colors_.surface       = RGB( 43,  43,  43);  // #2B2B2B
        colors_.text          = RGB(255, 255, 255);   // #FFFFFF
        colors_.textSecondary = RGB(170, 170, 170);   // #AAAAAA
        colors_.accent        = GetSysColor(COLOR_HIGHLIGHT);  // system accent works
        colors_.accentText    = GetSysColor(COLOR_HIGHLIGHTTEXT);
        colors_.border        = RGB( 64,  64,  64);   // #404040
    } else {
        colors_.background    = GetSysColor(COLOR_WINDOW);
        colors_.surface       = RGB( 242, 242, 242);  // softer than COLOR_BTNFACE usually
        colors_.text          = GetSysColor(COLOR_WINDOWTEXT);
        colors_.textSecondary = RGB( 90,  90,  90);
        colors_.accent        = GetSysColor(COLOR_HIGHLIGHT);
        colors_.accentText    = GetSysColor(COLOR_HIGHLIGHTTEXT);
        colors_.border        = RGB( 210, 210, 210);  // #D2D2D2
    }
}

bool ClassicTheme::OnSettingChange(LPARAM lParam) {
    if (forceLightTheme_) return false;  // Ignore system theme changes
    if (lParam && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
        bool wasDark = isDark_;
        DetectDarkMode();
        if (wasDark != isDark_) {
            RefreshColors();
            DestroyBrushes();
            CreateBrushes();
            DarkModeHelper::ApplyDarkModeForApp();
            ApplyWindowAttributes(hwnd_);
            return true;
        }
    }
    return false;
}

void ClassicTheme::ApplyWindowAttributes(HWND hwnd) {
    // Dark mode caption bar (Win10 build 18985+)
    BOOL darkBool = isDark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkBool, sizeof(darkBool));

    // Win11: rounded corners (no custom caption color — let Windows decide)
    if (DarkModeHelper::IsWindows11OrGreater()) {
        auto corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    }
}

// Subclass to prevent the white flash during the slide-down animation of ComboLBox
static LRESULT CALLBACK DarkListSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* theme = reinterpret_cast<ClassicTheme*>(dwRefData);
    if ((msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT) && theme && theme->IsDark()) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, theme->BrushBackground());
        if (msg == WM_ERASEBKGND) return 1;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ListViewSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* theme = reinterpret_cast<ClassicTheme*>(dwRefData);
    if (msg == WM_NOTIFY && theme && theme->IsDark()) {
        auto* nmhdr = reinterpret_cast<LPNMHDR>(lParam);
        if (nmhdr->code == NM_CUSTOMDRAW) {
            auto* pnmcd = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
            HWND hHeader = ListView_GetHeader(hWnd);
            if (hHeader && pnmcd->hdr.hwndFrom == hHeader) {
                if (pnmcd->dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (pnmcd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    SetTextColor(pnmcd->hdc, theme->Colors().text);
                    SetBkMode(pnmcd->hdc, TRANSPARENT);

                    RECT rc = pnmcd->rc;
                    FillRect(pnmcd->hdc, &rc, theme->BrushBackground());

                    wchar_t buf[256] = {0};
                    HDITEMW hdi = { HDI_TEXT };
                    hdi.pszText = buf;
                    hdi.cchTextMax = 256;
                    SendMessageW(hHeader, HDM_GETITEMW, pnmcd->dwItemSpec, reinterpret_cast<LPARAM>(&hdi));

                    rc.left += DpiScale(6, theme->Dpi()); // padding
                    DrawTextW(pnmcd->hdc, buf, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    int count = Header_GetItemCount(hHeader);
                    if (static_cast<int>(pnmcd->dwItemSpec) < count - 1) {
                        // Draw separator line
                        int inset = DpiScale(4, theme->Dpi());
                        HPEN pen = CreatePen(PS_SOLID, 1, theme->Colors().border);
                        HGDIOBJ oldPen = SelectObject(pnmcd->hdc, pen);
                        MoveToEx(pnmcd->hdc, pnmcd->rc.right - 1, pnmcd->rc.top + inset, nullptr);
                        LineTo(pnmcd->hdc, pnmcd->rc.right - 1, pnmcd->rc.bottom - inset);
                        SelectObject(pnmcd->hdc, oldPen);
                        DeleteObject(pen);
                    }

                    return CDRF_SKIPDEFAULT;
                }
            }
        }
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// Checkbox/Radio custom-draw subclass proc
static LRESULT CALLBACK CheckboxSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR /*uId*/, DWORD_PTR dwRef) {
    auto* th = reinterpret_cast<ClassicTheme*>(dwRef);
    if (msg == WM_PAINT && th) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        th->DrawCheckbox(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0; // Handled
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ComboBox full custom-draw subclass proc (draws Fluent Design style box + arrow + text)
static LRESULT CALLBACK ComboSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uId*/, DWORD_PTR dwRef) {
    auto* th = reinterpret_cast<ClassicTheme*>(dwRef);
    if (!th) return DefSubclassProc(hWnd, msg, wParam, lParam);

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);

        bool isFocused = (GetFocus() == hWnd);

        LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        bool hasEdit = ((style & CBS_DROPDOWN) == CBS_DROPDOWN) && ((style & CBS_DROPDOWNLIST) != CBS_DROPDOWNLIST);
        
        if (hasEdit) {
            HWND hEdit = FindWindowExW(hWnd, nullptr, L"Edit", nullptr);
            if (hEdit && GetFocus() == hEdit) isFocused = true;
        }

        // 1. Draw rounded background and border
        COLORREF bg = th->Colors().surface;
        COLORREF border = isFocused ? th->Colors().accent : th->Colors().border;
        
        HBRUSH bgBr = CreateSolidBrush(bg);
        HPEN bgPen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBr = SelectObject(hdc, bgBr);
        HGDIOBJ oldPen = SelectObject(hdc, bgPen);

        UINT dpi = Classic::GetWindowDpi(hWnd);
        int r = Classic::DpiScale(6, dpi); 

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);

        // 2. Draw standard chevron (arrow)
        int cx = rc.right - Classic::DpiScale(16, dpi);
        int cy = rc.top + (rc.bottom - rc.top) / 2;
        HPEN arrowPen = CreatePen(PS_SOLID, Classic::DpiScale(1, dpi) < 2 ? 2 : Classic::DpiScale(1, dpi), th->Colors().textSecondary);
        SelectObject(hdc, arrowPen);
        int aw = Classic::DpiScale(4, dpi);
        MoveToEx(hdc, cx - aw, cy - aw / 2 + 1, nullptr);
        LineTo(hdc, cx, cy + aw / 2 + 1);
        LineTo(hdc, cx + aw, cy - aw / 2 + 1);

        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(bgBr);
        DeleteObject(bgPen);
        DeleteObject(arrowPen);

        // 3. Draw text (only if there's no child Edit control overlapping)
        if (!hasEdit) {
            wchar_t text[256] = {};
            GetWindowTextW(hWnd, text, 256);
            RECT textRc = rc;
            textRc.left += Classic::DpiScale(10, dpi);
            textRc.right -= Classic::DpiScale(28, dpi); // make room for arrow
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, th->Colors().text);
            SelectObject(hdc, th->Fonts().body);
            DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hWnd, &ps);
        return 0; // Completely bypass system drawing!
    }
    
    // Completely ignore WM_ERASEBKGND so the system doesn't draw a grey box behind our rounded corners
    if (msg == WM_ERASEBKGND) {
        return 1;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// Edit custom border subclass proc
static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR /*uId*/, DWORD_PTR dwRef) {
    auto* th = reinterpret_cast<ClassicTheme*>(dwRef);
    if (!th) return DefSubclassProc(hWnd, msg, wParam, lParam);

    if (msg == WM_NCCALCSIZE && wParam) {
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
        LPNCCALCSIZE_PARAMS p = (LPNCCALCSIZE_PARAMS)lParam;
        int windowH = p->rgrc[0].bottom - p->rgrc[0].top;

        // Measure actual font height
        HFONT hFont = (HFONT)SendMessageW(hWnd, WM_GETFONT, 0, 0);
        int textH = Classic::DpiScale(18, th->Dpi()); // fallback
        if (hFont) {
            HDC hdc = GetDC(hWnd);
            HFONT old = (HFONT)SelectObject(hdc, hFont);
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            textH = tm.tmHeight;
            SelectObject(hdc, old);
            ReleaseDC(hWnd, hdc);
        }

        int vPad = (windowH - textH) / 2;
        if ((GetWindowLongPtrW(hWnd, GWL_STYLE) & ES_MULTILINE) != 0) {
            vPad = Classic::DpiScale(8, th->Dpi()); // Fixed top/bottom padding for multiline
        }
        if (vPad < 1) vPad = 1;
        int hPad = Classic::DpiScale(8, th->Dpi());

        p->rgrc[0].top    += vPad;
        p->rgrc[0].bottom -= vPad;
        p->rgrc[0].left   += hPad;
        p->rgrc[0].right  -= hPad;
        return res;
    }

    if (msg == WM_NCPAINT) {
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
        // Fill the non-client padding area with the edit background color
        HDC hdc = GetWindowDC(hWnd);
        if (hdc) {
            RECT wrBase;
            GetWindowRect(hWnd, &wrBase);
            RECT wr = wrBase;
            OffsetRect(&wr, -wr.left, -wr.top);
            
            // Exclude the client area so we don't erase text when unfocused
            RECT cr;
            GetClientRect(hWnd, &cr);
            POINT pt = {0, 0};
            ClientToScreen(hWnd, &pt);
            OffsetRect(&cr, pt.x - wrBase.left, pt.y - wrBase.top);
            ExcludeClipRect(hdc, cr.left, cr.top, cr.right, cr.bottom);

            if (GetWindowLongPtrW(hWnd, GWL_STYLE) & WS_VSCROLL) {
                int cxVScroll = Classic::DpiScale(GetSystemMetrics(SM_CXVSCROLL), th->Dpi());
                // Exclude the scrollbar area to preserve the native scrollbar
                ExcludeClipRect(hdc, wr.right - cxVScroll, wr.top, wr.right, wr.bottom);
            }

            HBRUSH bg = th->IsDark() ? th->BrushBackground()
                                     : GetSysColorBrush(COLOR_WINDOW);
            FillRect(hdc, &wr, bg);
            // Draw border over the full rect (will be clipped properly by scrollbar exclusion)
            th->DrawEditBorder(hdc, wr, GetFocus() == hWnd);
            ReleaseDC(hWnd, hdc);
        }
        return res;
    }

    if (msg == WM_PAINT) {
        // Let the system draw the text in the (shrunken) client area
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);

        // Overlay our rounded border on the full window rect
        HDC hdc = GetWindowDC(hWnd);
        if (hdc) {
            RECT wrBase;
            GetWindowRect(hWnd, &wrBase);
            RECT wr = wrBase;
            OffsetRect(&wr, -wr.left, -wr.top);

            if (GetWindowLongPtrW(hWnd, GWL_STYLE) & WS_VSCROLL) {
                int cxVScroll = Classic::DpiScale(GetSystemMetrics(SM_CXVSCROLL), th->Dpi());
                // Ensure we don't overlay the border onto the native scrollbar
                ExcludeClipRect(hdc, wr.right - cxVScroll, wr.top, wr.right, wr.bottom);
            }

            th->DrawEditBorder(hdc, wr, GetFocus() == hWnd);
            ReleaseDC(hWnd, hdc);
        }
        return res;
    }

    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        return res;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

void ClassicTheme::DrawEditBorder(HDC hdc, const RECT& rc, bool isFocused) {
    COLORREF borderCol = isFocused ? colors_.accent : colors_.border;
    
    HPEN pen = CreatePen(PS_SOLID, 1, borderCol);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    
    int r = Classic::DpiScale(12, dpi_); // Pill style

    // Draw exactly inside the client area
    ::RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, r, r);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(pen);
}

void ClassicTheme::DrawHotkeyEditBorder(HDC hdc, HWND hwndParent, HWND hwndEdit, int controlHeight) {
    if (!hwndEdit) return;

    RECT rcE;
    GetWindowRect(hwndEdit, &rcE);
    MapWindowPoints(HWND_DESKTOP, hwndParent, reinterpret_cast<LPPOINT>(&rcE), 2);

    int padY = (controlHeight - (rcE.bottom - rcE.top)) / 2;
    rcE.top -= (padY + 1);
    rcE.bottom += (padY + 1);

    int hInf = Classic::DpiScale(6, dpi_);
    rcE.left -= hInf;
    rcE.right += hInf;

    HPEN pen = CreatePen(PS_SOLID, 1, colors_.border);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    int r = Classic::DpiScale(12, dpi_);
    RoundRect(hdc, rcE.left, rcE.top, rcE.right, rcE.bottom, r, r);

    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void ClassicTheme::ThemeChildControl(HWND hwndCtrl) {
    if (!hwndCtrl) return;

    BOOL darkBool = isDark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwndCtrl, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkBool, sizeof(darkBool));

    fnAllowDarkModeForWindow allow = nullptr;
    HMODULE hUxTheme = GetModuleHandleW(L"uxtheme.dll");
    if (hUxTheme) {
        allow = reinterpret_cast<fnAllowDarkModeForWindow>(
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133)));
        if (allow) allow(hwndCtrl, isDark_);
    }

    wchar_t className[32] = {0};
    if (GetClassNameW(hwndCtrl, className, 32)) {
        if (wcscmp(className, L"ComboBox") == 0) {
            SetWindowTheme(hwndCtrl, isDark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            
            // Apply full custom paint to the combobox body
            SetWindowSubclass(hwndCtrl, ComboSubclassProc, 201, reinterpret_cast<DWORD_PTR>(this));

            COMBOBOXINFO info = { sizeof(COMBOBOXINFO) };
            if (GetComboBoxInfo(hwndCtrl, &info) && info.hwndList) {
                DwmSetWindowAttribute(info.hwndList, 20, &darkBool, sizeof(darkBool));
                if (hUxTheme && allow) allow(info.hwndList, isDark_);

                // DarkMode_Explorer is crucial for getting dark scrollbars on Win10/11
                SetWindowTheme(info.hwndList, isDark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                SetClassLongPtrW(info.hwndList, GCLP_HBRBACKGROUND,
                    reinterpret_cast<LONG_PTR>(isDark_ ? brBackground_ : GetSysColorBrush(COLOR_WINDOW)));
                
                // Subclass the dropdown list
                SetWindowSubclass(info.hwndList, DarkListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
            }
        } else if (wcscmp(className, WC_LISTVIEWW) == 0) {
            SetWindowTheme(hwndCtrl, isDark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            ListView_SetBkColor(hwndCtrl, isDark_ ? colors_.background : GetSysColor(COLOR_WINDOW));
            ListView_SetTextBkColor(hwndCtrl, isDark_ ? colors_.background : GetSysColor(COLOR_WINDOW));
            ListView_SetTextColor(hwndCtrl, isDark_ ? colors_.text : GetSysColor(COLOR_WINDOWTEXT));
            SetWindowSubclass(hwndCtrl, ListViewSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
        } else if (wcscmp(className, WC_HEADER) == 0) {
            SetWindowTheme(hwndCtrl, isDark_ ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
        } else if (wcscmp(className, L"Button") == 0) {
            // Detect checkbox / radio button for custom draw
            LONG_PTR style = GetWindowLongPtrW(hwndCtrl, GWL_STYLE);
            LONG_PTR type = style & BS_TYPEMASK;
            if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
                type == BS_AUTO3STATE || type == BS_3STATE ||
                type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) {
                SetWindowSubclass(hwndCtrl, CheckboxSubclassProc, 200, reinterpret_cast<DWORD_PTR>(this));
            }
            SetWindowTheme(hwndCtrl, isDark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        } else {
            SetWindowTheme(hwndCtrl, isDark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
    }
}

void ClassicTheme::ThemeAllChildren(HWND parent) {
    EnumChildWindows(parent, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* th = reinterpret_cast<ClassicTheme*>(lParam);
        th->ThemeChildControl(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
}

int ClassicTheme::ModernHeight() const {
    return DpiScale(32, dpi_);
}

void ClassicTheme::ApplyModernEntryStyle(HWND hwndCtrl) {
    if (!hwndCtrl) return;

    // 1. Theme + remove 3D border + subclass
    SetWindowTheme(hwndCtrl, isDark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowLongPtrW(hwndCtrl, GWL_EXSTYLE, GetWindowLongPtrW(hwndCtrl, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowSubclass(hwndCtrl, EditSubclassProc, 202, reinterpret_cast<DWORD_PTR>(this));

    // 2. Set font FIRST — WM_NCCALCSIZE needs to measure this font
    SendMessageW(hwndCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.entry), TRUE);

    // 3. NOW trigger frame recalculation — WM_NCCALCSIZE will read the correct font
    SetWindowPos(hwndCtrl, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    InvalidateRect(hwndCtrl, nullptr, TRUE);
}


// -- Brushes --

void ClassicTheme::CreateBrushes() {
    brBackground_ = CreateSolidBrush(colors_.background);
    brSurface_    = CreateSolidBrush(colors_.surface);
}

void ClassicTheme::DestroyBrushes() {
    auto del = [](HBRUSH& br) { if (br) { DeleteObject(br); br = nullptr; } };
    del(brBackground_); del(brSurface_);
}

// -- Fonts --

void ClassicTheme::CreateFonts(UINT dpi) {
    auto make = [&](int pt, int weight, const wchar_t* face) -> HFONT {
        int height = -MulDiv(pt, static_cast<int>(dpi), 72);
        return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    // Use plain "Segoe UI" — it has dedicated per-weight font files with
    // proper ClearType hinting for GDI. "Segoe UI Variable" is a variable font
    // designed for DirectWrite and causes GDI to synthetic-bold → blurry text.
    fonts_.header   = make(11, FW_SEMIBOLD, L"Segoe UI Semibold");
    fonts_.body     = make(10, FW_NORMAL,   L"Segoe UI");
    fonts_.bodyBold = make(10, FW_SEMIBOLD, L"Segoe UI Semibold");
    fonts_.entry    = make(11, FW_NORMAL,   L"Segoe UI");
}

void ClassicTheme::DestroyFonts() {
    auto del = [](HFONT& f) { if (f) { DeleteObject(f); f = nullptr; } };
    del(fonts_.header); del(fonts_.body); del(fonts_.bodyBold); del(fonts_.entry);
}

// -- WM_CTLCOLOR Handlers --

HBRUSH ClassicTheme::OnCtlColorDlg(HDC) {
    return brBackground_;
}

HBRUSH ClassicTheme::OnCtlColorStatic(HDC hdc, HWND) {
    SetTextColor(hdc, colors_.text);
    SetBkColor(hdc, colors_.background);
    SetBkMode(hdc, OPAQUE);
    return brBackground_;
}

HBRUSH ClassicTheme::OnCtlColorBtn(HDC hdc, HWND) {
    SetTextColor(hdc, colors_.text);
    SetBkColor(hdc, colors_.background);
    SetBkMode(hdc, OPAQUE);
    return brBackground_;
}

HBRUSH ClassicTheme::OnCtlColorEdit(HDC hdc, HWND) {
    SetTextColor(hdc, colors_.text);
    SetBkColor(hdc, colors_.surface);
    return brSurface_;
}

HBRUSH ClassicTheme::OnCtlColorListBox(HDC hdc, HWND) {
    SetTextColor(hdc, colors_.text);
    SetBkColor(hdc, colors_.background);
    return brBackground_;
}

// -- Owner-Draw: Tab Item --

void ClassicTheme::DrawTabItem(DRAWITEMSTRUCT* dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;

    // Background
    FillRect(hdc, &rc, brBackground_);

    HPEN pen = CreatePen(PS_SOLID, 1, colors_.border);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);

    int r = CornerRadius();
    if (selected) {
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom + r, r, r);
    } else {
        // Unselected tabs also get rounded top
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    }
    
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    wchar_t text[64]{};
    TCITEMW tci = {};
    tci.mask = TCIF_TEXT;
    tci.pszText = text;
    tci.cchTextMax = 64;
    SendMessageW(dis->hwndItem, TCM_GETITEMW, dis->itemID, reinterpret_cast<LPARAM>(&tci));

    SetTextColor(hdc, selected ? colors_.text : colors_.textSecondary);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, selected ? fonts_.header : fonts_.body);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// -- Divider --

void ClassicTheme::DrawDivider(HDC hdc, int x, int y, int width) {
    RECT line = { x, y, x + width, y + 1 };
    HBRUSH br = CreateSolidBrush(colors_.border);
    FillRect(hdc, &line, br);
    DeleteObject(br);
}

// -- Owner-Draw: Checkbox / Radio --

void ClassicTheme::DrawCheckbox(HWND hWnd, HDC hdc) {
    RECT rc;
    GetClientRect(hWnd, &rc);

    // Fill background
    FillRect(hdc, &rc, brBackground_);

    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    LONG_PTR type = style & BS_TYPEMASK;
    bool isRadio = (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON);
    bool isChecked = (SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool isDisabled = !IsWindowEnabled(hWnd);

    // DPI-aware box size
    UINT dpi = Classic::GetWindowDpi(hWnd);
    int boxSize = Classic::DpiScale(14, dpi);
    int boxY = rc.top + (rc.bottom - rc.top - boxSize) / 2;
    int boxX = rc.left + 1;
    RECT boxRc = { boxX, boxY, boxX + boxSize, boxY + boxSize };

    if (isRadio) {
        // Use native theme rendering for radio button indicator (perfect circle)
        HTHEME hTheme = OpenThemeData(hWnd, L"BUTTON");
        if (hTheme) {
            int stateId;
            if (isDisabled)
                stateId = isChecked ? RBS_CHECKEDDISABLED : RBS_UNCHECKEDDISABLED;
            else
                stateId = isChecked ? RBS_CHECKEDNORMAL : RBS_UNCHECKEDNORMAL;
            DrawThemeBackground(hTheme, hdc, BP_RADIOBUTTON, stateId, &boxRc, nullptr);
            CloseThemeData(hTheme);
        }
    } else {
        int radius = Classic::DpiScale(4, dpi);
        if (isChecked) {
            // Filled accent box
            COLORREF fill = isDisabled ? colors_.textSecondary : colors_.accent;
            HBRUSH fillBr = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, 1, fill);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBr = SelectObject(hdc, fillBr);
            RoundRect(hdc, boxRc.left, boxRc.top, boxRc.right, boxRc.bottom, radius, radius);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            DeleteObject(fillBr);

            // Checkmark: thin L-shaped polyline
            int s = Classic::DpiScale(1, dpi); // base scale
            // Force 1px thickness on 100-125% DPI for a thinner look, 2px for >=150%
            int thickness = s < 2 ? 1 : 2; 
            HPEN checkPen = CreatePen(PS_SOLID, thickness, RGB(255, 255, 255));
            HPEN oldP = (HPEN)SelectObject(hdc, checkPen);
            int cx = (boxRc.left + boxRc.right) / 2;
            int cy = (boxRc.top + boxRc.bottom) / 2;
            int q = boxSize / 5;
            MoveToEx(hdc, cx - q - 1, cy - 1, nullptr);
            LineTo(hdc, cx - 1, cy + q);
            LineTo(hdc, cx + q + 2, cy - q - 1);
            SelectObject(hdc, oldP);
            DeleteObject(checkPen);
        } else {
            // Empty box with border and surface background
            COLORREF bdr = colors_.border;
            HBRUSH bgBr = CreateSolidBrush(colors_.surface);
            HPEN pen = CreatePen(PS_SOLID, 1, bdr);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBr = SelectObject(hdc, bgBr);
            RoundRect(hdc, boxRc.left, boxRc.top, boxRc.right, boxRc.bottom, radius, radius);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            DeleteObject(bgBr);
        }
    }

    // Draw label text
    wchar_t text[256] = {};
    int textLen = GetWindowTextW(hWnd, text, 256);
    if (textLen > 0) {
        int textX = boxRc.right + Classic::DpiScale(6, dpi);
        RECT textRc = { textX, rc.top, rc.right, rc.bottom };
        
        COLORREF txtCol = isDisabled ? colors_.textSecondary : colors_.text;
        // Safety: if dark mode but text color is somehow black, force it to white
        if (isDark_ && txtCol == RGB(0, 0, 0)) txtCol = RGB(255, 255, 255);
        
        SetTextColor(hdc, txtCol);
        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(hdc, fonts_.body);
        DrawTextW(hdc, text, textLen, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, oldFont);
    }
}

} // namespace NextKey::Classic
