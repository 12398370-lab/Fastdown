#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "engine.h"
#include "protocol.h"
#include "themes.h"
#include "performance.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr UINT_PTR RefreshTimer = 10;
constexpr UINT_PTR SearchTimer = 11;
constexpr int WindowMinWidth = 1120;
constexpr int WindowMinHeight = 790;
constexpr wchar_t AppClass[] = L"FASTDOWN_DESKTOP_V1_0";

enum Control : int {
    C_Add = 100, C_Go, C_Resume, C_Pause, C_Stop, C_Retry,
    C_Remove, C_Import, C_Export, C_OpenFolder, C_Url, C_Directory,
    C_BrowseFolder, C_Protocol, C_BrowseTorrent, C_Search,
    C_Themes, C_Layouts, C_Modes, C_List, C_Filter,
    C_NavAll = 200, C_NavActive, C_NavComplete, C_NavPaused, C_NavFailed,
    C_NavExternal, C_NavMusic, C_NavVideos, C_NavArchives,
    C_ShowDetails = 300, C_CopyUrl, C_CopyPath, C_CopyReport,
    C_CopyAll, C_ExportCSV, C_SelectAll, C_ToggleTheme, C_ToggleLayout,
    C_ToggleMode, C_FocusURL, C_FocusSearch, C_Refresh, C_About
};

struct RateData {
    std::int64_t previous = 0;
    double rate = 0;
};

struct App {
    HWND window = nullptr;
    HWND search = nullptr, list = nullptr, url = nullptr, directory = nullptr;
    HWND protocol = nullptr, torrent = nullptr, go = nullptr, browseFolder = nullptr;
    HWND theme = nullptr, layout = nullptr, mode = nullptr;
    HWND add = nullptr, resume = nullptr, pause = nullptr, stop = nullptr;
    HWND retry = nullptr, remove = nullptr, import = nullptr, exportButton = nullptr;
    HWND openFolder = nullptr;
    std::array<HWND, 9> navigation{};
    std::array<int, 9> navCommands{};
    HFONT font = nullptr, fontSmall = nullptr, fontMedium = nullptr;
    HFONT fontTitle = nullptr, fontStat = nullptr;
    HBRUSH editBrush = nullptr;
    HIMAGELIST rowImages = nullptr;
    HWND hovered = nullptr;
    fd::Manager manager;
    std::vector<fd::Snapshot> snapshots;
    std::vector<fd::Snapshot> visible;
    std::vector<unsigned> displayedIds; // Stable row IDs (A16).
    std::unordered_map<unsigned, std::array<std::wstring, 7>> displayedCells; // A17: value diff cache.
    std::unordered_map<unsigned, RateData> rates;
    std::chrono::steady_clock::time_point lastSample = std::chrono::steady_clock::now();
    std::wstring banner = L"Ready to download";
    int themeIndex = 0, layoutIndex = 0;
    int navIndex = 0, protocolIndex = 0;
    int sortColumn = 0;
    bool descending = false;
    bool composer = false;
    UINT dpi = 96;
    double overallSpeed = 0;
    std::wstring diskText = L"Disk space: calculating";
    std::chrono::steady_clock::time_point diskReadAt{};
    int left = 0, contentWidth = 0, listTop = 0, summaryTop = 0;
} g;

int S(int value) { return MulDiv(value, static_cast<int>(g.dpi), 96); }
const fd::Theme& T() { return fd::Themes().at(static_cast<std::size_t>(g.themeIndex)); }
const fd::Layout& L() { return fd::Layouts().at(static_cast<std::size_t>(g.layoutIndex)); }

namespace {
constexpr wchar_t PreferencesKey[] = L"Software\\FASTDOWN1.0";
DWORD ReadPreference(const wchar_t* key, DWORD fallback, DWORD maximum) {
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, PreferencesKey, key,
                     RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return fallback;
    return value <= maximum ? value : fallback;
}
std::wstring ReadStringPreference(const wchar_t* key) {
    wchar_t value[256]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, PreferencesKey, key,
                     RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    value[_countof(value) - 1] = L'\0';
    return value;
}
int LookupSavedTheme(const std::vector<fd::Theme>& values, int fallback) {
    const auto name = ReadStringPreference(L"ThemeName");
    if (!name.empty())
        for (std::size_t i = 0; i < values.size(); ++i)
            if (values[i].name == name) return static_cast<int>(i);
    return fallback;
}
int LookupSavedLayout(const std::vector<fd::Layout>& values, int fallback) {
    const auto name = ReadStringPreference(L"LayoutName");
    if (!name.empty())
        for (std::size_t i = 0; i < values.size(); ++i)
            if (values[i].name == name) return static_cast<int>(i);
    return fallback;
}
void PersistAppearance() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, PreferencesKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const auto writeNumber = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    const auto writeText = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    writeNumber(L"Theme", static_cast<DWORD>(g.themeIndex));
    writeNumber(L"Layout", static_cast<DWORD>(g.layoutIndex));
    writeNumber(L"Mode", static_cast<DWORD>(g.manager.GetMode()));
    writeText(L"ThemeName", T().name);
    writeText(L"LayoutName", L().name);
    RegCloseKey(key);
}
void UpdateTitleContrast() {
    const auto& colors = T();
    const int brightness = (GetRValue(colors.bg) * 299 +
        GetGValue(colors.bg) * 587 + GetBValue(colors.bg) * 114) / 1000;
    const BOOL dark = brightness < 170 ? TRUE : FALSE;
    DwmSetWindowAttribute(g.window, static_cast<DWMWINDOWATTRIBUTE>(20),
                          &dark, sizeof(dark));
}
} // namespace


RECT Rect(int x, int y, int width, int height) {
    return RECT{x, y, x + width, y + height};
}

std::wstring ReadText(HWND control) {
    if (!control) return L"";
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}

void WriteText(HWND control, const std::wstring& value) {
    if (control) SetWindowTextW(control, value.c_str());
}

std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \r\n\t");
    if (first == std::wstring::npos) return L"";
    const auto last = value.find_last_not_of(L" \r\n\t");
    return value.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring FormatNumber(std::uint64_t value) {
    return std::to_wstring(value);
}

bool ClipboardSet(const std::wstring& value) {
    if (!OpenClipboard(g.window)) return false;
    if (!EmptyClipboard()) { CloseClipboard(); return false; }
    const SIZE_T size = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!block) { CloseClipboard(); return false; }
    auto* data = static_cast<wchar_t*>(GlobalLock(block));
    if (!data) { GlobalFree(block); CloseClipboard(); return false; }
    memcpy(data, value.c_str(), size);
    GlobalUnlock(block);
    if (!SetClipboardData(CF_UNICODETEXT, block)) GlobalFree(block);
    CloseClipboard();
    return true;
}

std::wstring ClipboardGet() {
    std::wstring result;
    if (!OpenClipboard(g.window)) return result;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        const auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
        if (data) { result = data; GlobalUnlock(handle); }
    }
    CloseClipboard();
    return result;
}

void Fill(HDC dc, RECT area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

void Round(HDC dc, RECT area, int radius, COLORREF background, COLORREF stroke) {
    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void Gradient(HDC dc, RECT area, COLORREF first, COLORREF last, int radius) {
    if (area.right <= area.left || area.bottom <= area.top) return;
    HRGN clipping = CreateRoundRectRgn(area.left, area.top, area.right + 1,
                                      area.bottom + 1, radius, radius);
    SelectClipRgn(dc, clipping);
    TRIVERTEX vertices[2]{};
    vertices[0].x = area.left;
    vertices[0].y = area.top;
    vertices[0].Red = static_cast<COLOR16>(GetRValue(first) << 8);
    vertices[0].Green = static_cast<COLOR16>(GetGValue(first) << 8);
    vertices[0].Blue = static_cast<COLOR16>(GetBValue(first) << 8);
    vertices[1].x = area.right;
    vertices[1].y = area.bottom;
    vertices[1].Red = static_cast<COLOR16>(GetRValue(last) << 8);
    vertices[1].Green = static_cast<COLOR16>(GetGValue(last) << 8);
    vertices[1].Blue = static_cast<COLOR16>(GetBValue(last) << 8);
    GRADIENT_RECT mesh{0,1};
    GradientFill(dc, vertices, 2, &mesh, 1, GRADIENT_FILL_RECT_H);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clipping);
}

void DrawLabel(HDC dc, const std::wstring& text, RECT area,
               HFONT font, COLORREF color,
               UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ previous = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &area, flags);
    SelectObject(dc, previous);
}

void OutlineCircle(HDC dc, int centerX, int centerY, int radius, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Ellipse(dc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

bool IsPrimary(int id) { return id == C_Add || id == C_Go || id == C_BrowseTorrent; }

void DrawButton(const DRAWITEMSTRUCT* item) {
    HDC dc = item->hDC;
    const RECT area = item->rcItem;
    const bool active = item->hwndItem == g.hovered;
    const bool down = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool primary = IsPrimary(static_cast<int>(item->CtlID));
    const int radius = S(L().radius);
    COLORREF base = primary ? T().accent : T().raised;
    if (down) base = fd::Blend(base, T().bg, 24);
    else if (active) base = fd::Blend(base, T().accent, primary ? 10 : 15);
    if (disabled) base = fd::Blend(base, T().panel, 70);
    const bool navigation = item->CtlID >= C_NavAll && item->CtlID <= C_NavArchives;
    if (navigation) {
        const int index = static_cast<int>(item->CtlID) - C_NavAll;
        base = index == g.navIndex ? T().hover : (active ? T().raised : T().sidebar);
    }
    if (primary && !disabled) {
        Gradient(dc, area, base, fd::Blend(T().accent, RGB(192,81,245), 48), radius);
    } else {
        Round(dc, area, radius, base, navigation ? base : T().border);
    }
    if (navigation && static_cast<int>(item->CtlID) - C_NavAll == g.navIndex) {
        Fill(dc, RECT{area.left,area.top + S(7),area.left + S(3),area.bottom - S(7)}, T().accent);
    }
    wchar_t caption[100]{};
    GetWindowTextW(item->hwndItem, caption, static_cast<int>(_countof(caption)));
    RECT label = area;
    if (navigation) label.left += S(32);
    if (item->CtlID == C_Add) {
        OutlineCircle(dc, area.left + S(20), (area.top + area.bottom) / 2,
                      S(10), RGB(255,255,255), S(2));
        RECT vertical = Rect(area.left + S(19), (area.top + area.bottom) / 2 - S(5), S(2), S(10));
        RECT horizontal = Rect(area.left + S(15), (area.top + area.bottom) / 2 - S(1), S(10), S(2));
        Fill(dc, vertical, RGB(255,255,255));
        Fill(dc, horizontal, RGB(255,255,255));
        label.left += S(24);
    }
    if (navigation) {
        RECT glyph = Rect(area.left + S(13), (area.top + area.bottom) / 2 - S(5), S(10), S(10));
        Round(dc, glyph, S(5),
              static_cast<int>(item->CtlID) - C_NavAll == g.navIndex ? T().accent : T().muted,
              base);
    }
    DrawLabel(dc, caption, label, g.fontMedium,
              disabled ? T().muted : primary ? RGB(255,255,255) : T().text,
              navigation ? DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS
                         : DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (item->itemState & ODS_FOCUS) {
        RECT inner = area;
        InflateRect(&inner, -S(4), -S(4));
        DrawFocusRect(dc, &inner);
    }
}

LRESULT CALLBACK ButtonSubclass(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        if (g.hovered != window) {
            HWND previous = g.hovered;
            g.hovered = window;
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window;
        TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE) {
        if (g.hovered == window) g.hovered = nullptr;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ButtonSubclass, 1);
    }
    return DefSubclassProc(window, message, wp, lp);
}

HWND ControlWindow(const wchar_t* type, const wchar_t* caption, DWORD style, int id,
                   DWORD ex = 0) {
    HWND control = CreateWindowExW(ex, type, caption, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 80, 30, g.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

HWND Button(const wchar_t* caption, int id) {
    HWND control = ControlWindow(L"BUTTON", caption, BS_OWNERDRAW | WS_TABSTOP, id);
    if (control) SetWindowSubclass(control, ButtonSubclass, 1, 0);
    return control;
}

HWND Combo(int id) {
    return ControlWindow(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
}

HWND Edit(const wchar_t* initial, int id) {
    return ControlWindow(L"EDIT", initial, ES_AUTOHSCROLL | WS_TABSTOP, id, WS_EX_CLIENTEDGE);
}

void SetFont(HWND control, HFONT font) {
    if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void AddCombo(HWND control, const wchar_t* value) {
    SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
}

void CreateFonts() {
    auto make = [](int size, int weight) -> HFONT {
        return CreateFontW(-S(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    g.font = make(15, FW_NORMAL);
    g.fontSmall = make(12, FW_NORMAL);
    g.fontMedium = make(14, FW_SEMIBOLD);
    g.fontTitle = make(25, FW_BOLD);
    g.fontStat = make(24, FW_BOLD);
}

void RebuildListRowHeight() {
    if (!g.list) return;
    HIMAGELIST next = ImageList_Create(S(1), S(std::max(36, L().rowHeight)), ILC_COLOR32, 1, 1);
    if (!next) return;
    HIMAGELIST previous = ListView_SetImageList(g.list, next, LVSIL_SMALL);
    g.rowImages = next;
    if (previous) ImageList_Destroy(previous);
}

void ApplyTheme() {
    if (g.editBrush) DeleteObject(g.editBrush);
    g.editBrush = CreateSolidBrush(T().raised);
    if (g.list) {
        ListView_SetBkColor(g.list, T().panel);
        ListView_SetTextBkColor(g.list, T().panel);
        ListView_SetTextColor(g.list, T().text);
    }
    for (HWND control : {g.search,g.list,g.url,g.directory,g.protocol,g.theme,g.layout,g.mode})
        if (control) InvalidateRect(control, nullptr, TRUE);
    for (HWND control : g.navigation) if (control) InvalidateRect(control, nullptr, FALSE);
    if (g.window) InvalidateRect(g.window, nullptr, FALSE);
}

void ShowComposer(bool show) {
    g.composer = show;
    for (HWND control : {g.url,g.directory,g.protocol,g.browseFolder,g.torrent,g.go}) {
        if (control) ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    }
    if (show && g.torrent) ShowWindow(g.torrent,
        g.protocolIndex == 5 ? SW_SHOW : SW_HIDE);
    if (g.window) {
        UpdateTitleContrast();
        SendMessageW(g.window, WM_SIZE, 0, 0);
    }
}

void LayoutWindows() {
    if (!g.window || !g.list) return;
    RECT screen{};
    GetClientRect(g.window, &screen);
    const int width = screen.right;
    const int height = screen.bottom;
    const int sidebar = S(L().sidebar);
    const int left = sidebar + S(20);
    const int right = width - S(22);
    const int available = std::max(S(480), right - left);
    g.left = left;
    g.contentWidth = available;
    const int gap = S(8);
    const int buttonY = S(90);
    const int buttonH = S(43);
    const int widths[] = {112,79,73,76,75,119,78,78,92};
    const HWND toolbar[] = {g.add,g.resume,g.pause,g.stop,g.retry,
                            g.remove,g.import,g.exportButton,g.openFolder};
    const int count = static_cast<int>(_countof(toolbar));
    int required = gap * (count - 1);
    for (int x : widths) required += S(x);
    // One-row primary actions on wide windows; secondary operations wrap below.
    const bool wrap = required > available;
    int x = left;
    for (int i = 0; i < count; ++i) {
        if (wrap && i == 5) x = left;
        const int y = buttonY + ((wrap && i >= 5) ? S(49) : 0);
        const int buttonWidth = S(widths[i]);
        MoveWindow(toolbar[i], x, y, buttonWidth, buttonH, TRUE);
        x += buttonWidth + gap;
    }
    const int toolbarBottom = buttonY + buttonH + (wrap ? S(49) : 0);

    // Sidebar navigation. Keep the familiar download categories visible.
    const int navStart = S(156);
    for (int index = 0; index < static_cast<int>(g.navigation.size()); ++index) {
        MoveWindow(g.navigation[index], S(17), navStart + index * S(40),
                   sidebar - S(34), S(35), TRUE);
    }
    const int settingsTop = height - S(224);
    MoveWindow(g.theme, S(21), settingsTop + S(38), sidebar - S(42), S(260), TRUE);
    MoveWindow(g.layout, S(21), settingsTop + S(105), sidebar - S(42), S(260), TRUE);
    MoveWindow(g.mode, S(21), settingsTop + S(172), sidebar - S(42), S(190), TRUE);

    MoveWindow(g.search, right - S(280), S(20), S(276), S(37), TRUE);
    const int composerTop = toolbarBottom + S(18);
    if (g.composer) {
        MoveWindow(g.url, left + S(20), composerTop + S(36), available - S(194), S(33), TRUE);
        MoveWindow(g.go, right - S(154), composerTop + S(36), S(134), S(35), TRUE);
        const int protocolW = S(148);
        const int torrentW = S(130);
        const int folderW = available - S(478);
        MoveWindow(g.directory, left + S(20), composerTop + S(94), folderW, S(32), TRUE);
        MoveWindow(g.browseFolder, left + S(28) + folderW, composerTop + S(94), S(88), S(32), TRUE);
        MoveWindow(g.protocol, right - S(308), composerTop + S(94), protocolW, S(250), TRUE);
        MoveWindow(g.torrent, right - S(151), composerTop + S(94), torrentW, S(32), TRUE);
    }
    g.summaryTop = composerTop + (g.composer ? S(157) : S(10));
    g.listTop = g.summaryTop + S(112);
    MoveWindow(g.list, left, g.listTop, available,
               std::max(S(120), height - g.listTop - S(65)), TRUE);
    const int proportions[] = {31, 11, 13, 14, 12, 10, 9};
    for (int i = 0; i < 7; ++i)
        ListView_SetColumnWidth(g.list, i, available * proportions[i] / 100);
    InvalidateRect(g.window, nullptr, FALSE);
}

void UpdateDiskSpace(bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && std::chrono::duration<double>(now - g.diskReadAt).count() < 5.0) return;
    g.diskReadAt = now;
    const std::wstring path = ReadText(g.directory);
    ULARGE_INTEGER available{}, total{};
    if (path.empty() || !GetDiskFreeSpaceExW(path.c_str(), &available, &total, nullptr)) {
        g.diskText = L"Disk space: unavailable";
        return;
    }
    const int used = total.QuadPart
        ? static_cast<int>(100.0 * (total.QuadPart - available.QuadPart) / total.QuadPart)
        : 0;
    g.diskText = L"Disk used: " + std::to_wstring(std::clamp(used,0,100)) +
                 L"%  |  Free: " + fd::HumanBytes(static_cast<std::int64_t>(available.QuadPart));
}

void DrawStat(HDC dc, int x, int y, int width, int height,
              const std::wstring& title, const std::wstring& number,
              COLORREF accent) {
    const RECT area = Rect(x, y, width, height);
    Round(dc, area, S(L().radius), T().panel, T().border);
    const RECT accentBar = Rect(x + S(16), y + S(18), S(3), S(43));
    Fill(dc, accentBar, accent);
    DrawLabel(dc, title, Rect(x + S(30), y + S(13), width - S(44), S(23)),
              g.fontSmall, T().muted);
    DrawLabel(dc, number, Rect(x + S(30), y + S(37), width - S(44), S(35)),
              g.fontStat, T().text);
}

void Paint(HWND window) {
    PAINTSTRUCT ps{};
    HDC screenDC = BeginPaint(window, &ps);
    RECT client{};
    GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0) { EndPaint(window, &ps); return; }
    HDC dc = CreateCompatibleDC(screenDC);
    HBITMAP buffer = CreateCompatibleBitmap(screenDC,
        std::max(1, static_cast<int>(client.right)), std::max(1, static_cast<int>(client.bottom)));
    if (!dc || !buffer) {
        if (buffer) DeleteObject(buffer);
        if (dc) DeleteDC(dc);
        EndPaint(window, &ps);
        return;
    }
    HGDIOBJ old = SelectObject(dc, buffer);
    Fill(dc, client, T().bg);
    const int sidebar = S(L().sidebar);
    Fill(dc, Rect(0,0,sidebar,client.bottom), T().sidebar);
    Fill(dc, Rect(sidebar - S(1),0,S(1),client.bottom), T().border);
    Fill(dc, Rect(0, S(70), client.right, S(1)), T().border);
    Round(dc, Rect(S(22),S(18),S(36),S(36)), S(12), T().accent, T().accent);
    DrawLabel(dc, L"F", Rect(S(23),S(18),S(34),S(36)), g.fontTitle, RGB(255,255,255),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawLabel(dc, L"FASTDOWN", Rect(S(68),S(15),S(175),S(35)),
              g.fontTitle, T().text);
    DrawLabel(dc, L"DOWNLOAD STUDIO 1.0", Rect(S(69),S(47),S(185),S(18)),
              g.fontSmall, T().muted);
    DrawLabel(dc, L"Downloads", Rect(S(22),S(113),S(175),S(32)),
              g.fontMedium, T().muted);
    // Settings remain anchored to the bottom of the sidebar.
    const int settingsTop = client.bottom - S(224);
    Fill(dc, Rect(S(20),settingsTop-S(15),sidebar-S(40),S(1)), T().border);
    DrawLabel(dc, L"APPEARANCE & ENGINE", Rect(S(21),settingsTop-S(6),sidebar-S(40),S(26)),
              g.fontSmall, T().muted);
    DrawLabel(dc, L"Color theme", Rect(S(21),settingsTop+S(20),sidebar-S(40),S(20)),
              g.fontSmall, T().text);
    DrawLabel(dc, L"Layout style", Rect(S(21),settingsTop+S(87),sidebar-S(40),S(20)),
              g.fontSmall, T().text);
    DrawLabel(dc, L"Download mode", Rect(S(21),settingsTop+S(154),sidebar-S(40),S(20)),
              g.fontSmall, T().text);

    const int left = g.left, width = g.contentWidth;
    DrawLabel(dc, L"Download manager", Rect(left,S(21),S(325),S(34)),
              g.fontTitle, T().text);
    DrawLabel(dc, L"A17  |  HTTP  /  HTTPS  /  FTP  /  FTPS  /  TORRENT  /  MAGNET", Rect(left,S(55),S(650),S(18)),
              g.fontSmall, T().muted);
    const int summaryTop = g.summaryTop;
    if (g.composer) {
        const int composerTop = summaryTop - S(157);
        Round(dc, Rect(left,composerTop,width,S(140)), S(L().radius), T().panel, T().border);
        DrawLabel(dc, L"NEW DOWNLOAD", Rect(left+S(20),composerTop+S(9),S(220),S(23)),
                  g.fontMedium, T().text);
        DrawLabel(dc, L"Destination", Rect(left+S(20),composerTop+S(73),S(200),S(18)),
                  g.fontSmall, T().muted);
        DrawLabel(dc, L"Protocol", Rect(left+width-S(308),composerTop+S(73),S(130),S(18)),
                  g.fontSmall, T().muted);
    }
    int active=0, complete=0, errors=0;
    std::int64_t transferred=0;
    for (const auto& item : g.snapshots) {
        if (item.state == fd::State::Downloading || item.state == fd::State::Probing) ++active;
        if (item.state == fd::State::Complete) ++complete;
        if (item.state == fd::State::Failed) ++errors;
        transferred += item.done;
    }
    const int statGap = S(12);
    const int statWidth = (width - 3 * statGap) / 4;
    DrawStat(dc,left,summaryTop,statWidth,S(87),L"TOTAL TASKS",
             FormatNumber(g.snapshots.size()), T().accent);
    DrawStat(dc,left+statWidth+statGap,summaryTop,statWidth,S(87),L"ACTIVE",
             std::to_wstring(active), T().success);
    DrawStat(dc,left+(statWidth+statGap)*2,summaryTop,statWidth,S(87),L"COMPLETED",
             std::to_wstring(complete), T().warning);
    DrawStat(dc,left+(statWidth+statGap)*3,summaryTop,statWidth,S(87),L"TRANSFER RATE",
             fd::HumanRate(g.overallSpeed), T().accent);

    DrawLabel(dc, L"TRANSFER QUEUE  |  LIVE ACTIVITY", Rect(left,g.listTop-S(31),S(400),S(27)),
              g.fontMedium,T().text);
    DrawLabel(dc, std::to_wstring(g.visible.size()) + L" shown",
              Rect(left+width-S(120),g.listTop-S(28),S(120),S(24)),
              g.fontSmall,T().muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    Fill(dc, Rect(sidebar,client.bottom-S(49),client.right-sidebar,S(49)), T().panel);
    Fill(dc, Rect(sidebar,client.bottom-S(49),client.right-sidebar,S(1)), T().border);
    DrawLabel(dc, g.banner, Rect(left,client.bottom-S(44),S(300),S(36)),
              g.fontSmall,T().text);
    DrawLabel(dc, L"Errors: " + std::to_wstring(errors) +
              L"   |   Transferred: " + fd::HumanBytes(transferred) +
              L"   |   " + g.diskText,
              Rect(left+S(255),client.bottom-S(44),width-S(265),S(36)),
              g.fontSmall,T().muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    BitBlt(screenDC,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old);
    DeleteObject(buffer);
    DeleteDC(dc);
    EndPaint(window,&ps);
}

bool ShowFileDialog(std::wstring& output, const wchar_t* title,
                    const wchar_t* filter, bool save) {
    wchar_t filename[MAX_PATH]{};
    OPENFILENAMEW file{};
    file.lStructSize = sizeof(file);
    file.hwndOwner = g.window;
    file.lpstrTitle = title;
    file.lpstrFilter = filter;
    file.lpstrFile = filename;
    file.nMaxFile = _countof(filename);
    file.nFilterIndex = 1;
    file.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                 (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&file) : GetOpenFileNameW(&file))) return false;
    output = filename;
    return true;
}

std::wstring SelectDirectory() {
    BROWSEINFOW browse{};
    browse.hwndOwner = g.window;
    browse.lpszTitle = L"Select the download destination";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&browse);
    if (!id) return L"";
    wchar_t filename[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(id, filename);
    CoTaskMemFree(id);
    return ok ? filename : L"";
}

void BrowseTorrent() {
    std::wstring filename;
    if (!ShowFileDialog(filename, L"Select a BitTorrent file",
                        L"BitTorrent files (*.torrent)\0*.torrent\0All files (*.*)\0*.*\0", false)) return;
    if (!fd::IsLocalTorrentFile(filename)) {
        MessageBoxW(g.window, L"Please select an existing .torrent file.",
                    L"FASTDOWN", MB_ICONWARNING);
        return;
    }
    WriteText(g.url, filename);
    g.protocolIndex = 5;
    SendMessageW(g.protocol, CB_SETCURSEL, 5, 0);
    WriteText(g.go, L"Start download");
    g.banner = L"Torrent selected - FASTDOWN internal BT engine";
    InvalidateRect(g.window,nullptr,FALSE);
}

std::wstring DefaultDownloadFolder() {
    wchar_t profile[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(g.window, CSIDL_PROFILE, nullptr, 0, profile))) {
        return std::wstring(profile) + L"\\Downloads";
    }
    return L"C:\\Users\\Public\\Downloads";
}

unsigned SelectedID() {
    if (!g.list) return 0;
    const int row = ListView_GetNextItem(g.list,-1,LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(g.visible.size())) return 0;
    return g.visible[static_cast<std::size_t>(row)].id;
}

const fd::Snapshot* SelectedTask() {
    const unsigned id = SelectedID();
    for (const auto& item : g.snapshots) if (item.id == id) return &item;
    return nullptr;
}

double Rate(unsigned id) {
    auto found = g.rates.find(id);
    return found == g.rates.end() ? 0.0 : found->second.rate;
}

std::wstring Category(const fd::Snapshot& snapshot) {
    const std::wstring file = Lower(fd::FilenameFromUrl(snapshot.url));
    const auto dot = file.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"Other";
    const std::wstring ext = file.substr(dot);
    if (ext == L".mp3" || ext == L".wav" || ext == L".flac" || ext == L".ogg" ||
        ext == L".aac" || ext == L".m4a") return L"Music";
    if (ext == L".mp4" || ext == L".mkv" || ext == L".mov" || ext == L".avi" ||
        ext == L".webm") return L"Videos";
    if (ext == L".zip" || ext == L".7z" || ext == L".rar" || ext == L".tar" ||
        ext == L".gz" || ext == L".iso") return L"Archives";
    if (ext == L".pdf" || ext == L".docx" || ext == L".txt" || ext == L".xlsx")
        return L"Documents";
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp")
        return L"Images";
    return L"Other";
}

bool InFilter(const fd::Snapshot& item) {
    switch (g.navIndex) {
    case 1: return item.state == fd::State::Downloading || item.state == fd::State::Probing ||
                   item.state == fd::State::Queued;
    case 2: return item.state == fd::State::Complete;
    case 3: return item.state == fd::State::Paused;
    case 4: return item.state == fd::State::Failed || item.state == fd::State::Cancelled;
    case 5: return item.state == fd::State::HandedOff;
    case 6: return Category(item) == L"Music";
    case 7: return Category(item) == L"Videos";
    case 8: return Category(item) == L"Archives";
    default: return true;
    }
}

std::wstring Eta(const fd::Snapshot& task) {
    if (task.state == fd::State::Complete) return L"Done";
    if (task.total <= 0 || task.done >= task.total || Rate(task.id) < 1.0) return L"--";
    const double seconds = (task.total - task.done) / Rate(task.id);
    if (seconds > 60 * 60 * 24 * 30) return L"--";
    const auto rounded = static_cast<int>(seconds);
    if (rounded < 60) return std::to_wstring(rounded) + L"s";
    if (rounded < 3600) return std::to_wstring(rounded/60) + L"m " +
                              std::to_wstring(rounded%60) + L"s";
    return std::to_wstring(rounded/3600) + L"h " + std::to_wstring((rounded/60)%60) + L"m";
}

int Compare(const fd::Snapshot& a, const fd::Snapshot& b, int column) {
    auto textcmp = [](const std::wstring& x, const std::wstring& y) -> int {
        const int result = _wcsicmp(x.c_str(), y.c_str());
        return result < 0 ? -1 : result > 0 ? 1 : 0;
    };
    switch (column) {
    case 0: return textcmp(fd::FilenameFromUrl(a.url), fd::FilenameFromUrl(b.url));
    case 1: return (a.total > b.total) - (a.total < b.total);
    case 2: return (a.state > b.state) - (a.state < b.state);
    case 3: {
        const double pa = a.total ? static_cast<double>(a.done) / a.total : 0;
        const double pb = b.total ? static_cast<double>(b.done) / b.total : 0;
        return (pa > pb) - (pa < pb);
    }
    case 4: return (Rate(a.id) > Rate(b.id)) - (Rate(a.id) < Rate(b.id));
    case 5: return textcmp(fd::ProtocolName(a.url), fd::ProtocolName(b.url));
    case 6: return (a.id > b.id) - (a.id < b.id);
    default: return 0;
    }
}

void SetCell(int row, int column, const std::wstring& value) {
    ListView_SetItemText(g.list, row, column, const_cast<LPWSTR>(value.c_str()));
}

// A16: separate identity/order changes from progress updates.
// Retain selected row, top index and selection focus while in-flight progress changes.
bool SameDisplayedTaskOrder() {
    if (g.displayedIds.size() != g.visible.size()) return false;
    for (std::size_t i = 0; i < g.visible.size(); ++i)
        if (g.displayedIds[i] != g.visible[i].id) return false;
    return true;
}

// A17: update only cells whose visible contents changed. This avoids sending
// repeated Win32 list notifications while the queue is idle.
void SetCellCached(unsigned id, int row, int column, const std::wstring& value) {
    auto& values = g.displayedCells[id];
    if (values[static_cast<std::size_t>(column)] == value) return;
    values[static_cast<std::size_t>(column)] = value;
    SetCell(row, column, value);
}
void UpdateDisplayedRow(int row, const fd::Snapshot& item) {
    SetCellCached(item.id,row,1,item.total > 0 ? fd::HumanBytes(item.total) : L"--");
    SetCellCached(item.id,row,2,fd::StateName(item.state));
    if (item.total > 0)
        SetCellCached(item.id,row,3,std::to_wstring(fd::performance::CompletionBasisPoints(item.done,item.total) / 100u) + L"%");
    else
        SetCellCached(item.id,row,3,item.state == fd::State::HandedOff ? L"External" : L"--");
    SetCellCached(item.id,row,4,item.state == fd::State::Downloading ? fd::HumanRate(Rate(item.id)) : L"--");
    SetCellCached(item.id,row,5,fd::ProtocolName(item.url));
    SetCellCached(item.id,row,6,Eta(item));
}

void PopulateQueue() {
    const unsigned oldSelection = SelectedID();
    const int oldTop = ListView_GetTopIndex(g.list);
    const std::wstring search = Lower(Trim(ReadText(g.search)));
    g.visible.clear();
    g.visible.reserve(g.snapshots.size());
    for (const auto& item : g.snapshots) {
        if (!InFilter(item)) continue;
        if (!search.empty()) {
            const std::wstring haystack = Lower(item.url + L" " + item.destination + L" " + item.message);
            if (haystack.find(search) == std::wstring::npos) continue;
        }
        g.visible.push_back(item);
    }
    std::stable_sort(g.visible.begin(),g.visible.end(),[](const auto& a,const auto& b) {
        const int compare = Compare(a,b,g.sortColumn);
        if (compare == 0) return a.id < b.id;
        return g.descending ? compare > 0 : compare < 0;
    });
    const bool rebuild = !SameDisplayedTaskOrder();
    SendMessageW(g.list,WM_SETREDRAW,FALSE,0);
    if (rebuild) {
        ListView_DeleteAllItems(g.list);
        g.displayedIds.clear();
        g.displayedCells.clear();
        g.displayedIds.reserve(g.visible.size());
    }
    int selectedRow = -1;
    for (int row=0; row<static_cast<int>(g.visible.size()); ++row) {
        const auto& item = g.visible[static_cast<std::size_t>(row)];
        if (rebuild) {
            LVITEMW lv{};
            lv.mask = LVIF_TEXT | LVIF_PARAM;
            lv.iItem = row;
            lv.lParam = static_cast<LPARAM>(item.id);
            std::wstring name = fd::FilenameFromUrl(item.url);
            lv.pszText = name.data();
            ListView_InsertItem(g.list, &lv);
            g.displayedIds.push_back(item.id);
        }
        UpdateDisplayedRow(row,item);
        if (item.id == oldSelection) selectedRow = row;
    }
    if (rebuild) {
        if (selectedRow >= 0)
            ListView_SetItemState(g.list, selectedRow, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        if (!g.visible.empty()) ListView_EnsureVisible(g.list,
            std::min(oldTop, static_cast<int>(g.visible.size()) - 1), FALSE);
    }
    SendMessageW(g.list,WM_SETREDRAW,TRUE,0);
    InvalidateRect(g.list,nullptr,FALSE);
}

void Refresh(bool force = false) {
    auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now-g.lastSample).count();
    if (!force && dt < 0.3) return;
    const auto current = g.manager.List();
    double total = 0;
    for (const auto& item : current) {
        auto& rate = g.rates[item.id];
        if (dt >= 0.3) {
            const double instantaneous = std::max<std::int64_t>(0, item.done - rate.previous) / dt;
            rate.rate = item.state == fd::State::Downloading
                ? (rate.rate <= 1.0 ? instantaneous : 0.5*rate.rate + 0.5*instantaneous)
                : 0;
        }
        if (dt >= 0.3) rate.previous = item.done;
        total += rate.rate;
    }
    if (dt >= 0.3) g.lastSample = now;
    g.overallSpeed = total;
    UpdateDiskSpace();
    g.snapshots = current;
    PopulateQueue();
    InvalidateRect(g.window,nullptr,FALSE);
}

bool ProtocolMatches(const std::wstring& link) {
    const fd::Protocol detected = fd::Detect(link);
    const std::wstring lower = Lower(link);
    switch (g.protocolIndex) {
    case 0: return detected != fd::Protocol::Unsupported;
    case 1: return lower.rfind(L"http://",0) == 0;
    case 2: return lower.rfind(L"https://",0) == 0;
    case 3: return detected == fd::Protocol::Magnet;
    case 4: return detected == fd::Protocol::Ed2k;
    case 5: return detected == fd::Protocol::Torrent;
    case 6: return lower.rfind(L"ftp://",0) == 0;
    case 7: return lower.rfind(L"ftps://",0) == 0;
    default: return false;
    }
}

void AddSourcesText(const std::wstring& input) {
    const std::wstring folder = Trim(ReadText(g.directory));
    if (Trim(input).empty()) {
        MessageBoxW(g.window,L"Enter a URL, Magnet link or select a .torrent file.",
                    L"FASTDOWN",MB_OK|MB_ICONINFORMATION);
        return;
    }
    if (folder.empty()) {
        MessageBoxW(g.window,L"Select a download destination first.",L"FASTDOWN",MB_ICONWARNING);
        return;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(std::filesystem::path(folder), directoryError);
    if (directoryError) {
        MessageBoxW(g.window,L"Cannot create the selected download directory.",L"FASTDOWN",MB_ICONERROR);
        return;
    }
    std::size_t position = 0;
    unsigned added = 0, rejected = 0;
    while (position < input.size()) {
        const std::size_t end = input.find_first_of(L"\r\n", position);
        const std::wstring link = Trim(input.substr(position,
            end == std::wstring::npos ? end : end-position));
        if (!link.empty()) {
            if (!ProtocolMatches(link)) { ++rejected; }
            else {
                const auto protocol = fd::Detect(link);
                std::wstring destination;
                if (protocol == fd::Protocol::Http)
                    destination = (std::filesystem::path(folder) / fd::FilenameFromUrl(link)).wstring();
                else if (protocol == fd::Protocol::Magnet || protocol == fd::Protocol::Torrent)
                    destination = folder; // libtorrent expects a directory.
                // FTP and ED2K use the explicitly configured external handler.
                if (protocol == fd::Protocol::Ftp)
                    destination = fd::IsFtpsUrl(link) ? folder :
                        (std::filesystem::path(folder) / fd::FilenameFromUrl(link)).wstring();
                if (protocol == fd::Protocol::Ed2k) destination = folder;
                if (g.manager.Add(link,destination)) ++added;
                else ++rejected;
            }
        }
        if (end == std::wstring::npos) break;
        position = end + 1;
        while (position < input.size() && (input[position] == L'\r' || input[position] == L'\n')) ++position;
    }
    if (added) {
        g.banner = std::to_wstring(added) + L" task(s) added to FASTDOWN";
        WriteText(g.url,L"");
        ShowComposer(false);
        UpdateDiskSpace(true);
        Refresh(true);
    }
    if (rejected) {
        MessageBoxW(g.window,
            (std::to_wstring(rejected) + L" item(s) could not be added. Check protocol, duplicates and destination.").c_str(),
            L"FASTDOWN - Some items not added",MB_ICONWARNING);
    }
}

void AddSources() {
    AddSourcesText(ReadText(g.url));
}

void OpenFolder() {
    const std::wstring path = ReadText(g.directory);
    if (!path.empty()) ShellExecuteW(g.window,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}

void OpenSelectedFile(bool containing) {
    const auto* task = SelectedTask();
    if (!task || task->destination.empty()) return;
    if (containing) {
        const std::wstring parameters = L"/select,\"" + task->destination + L"\"";
        ShellExecuteW(g.window,L"open",L"explorer.exe",parameters.c_str(),nullptr,SW_SHOWNORMAL);
    } else if (task->state == fd::State::Complete) {
        ShellExecuteW(g.window,L"open",task->destination.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
}

void ShowSelectedDetails() {
    const auto* task = SelectedTask();
    if (!task) return;
    const std::wstring report =
        L"Task ID: " + std::to_wstring(task->id) +
        L"\nFilename: " + fd::FilenameFromUrl(task->url) +
        L"\nProtocol: " + fd::ProtocolName(task->url) +
        L"\nStatus: " + fd::StateName(task->state) +
        L"\nDownloaded: " + fd::HumanBytes(task->done) +
        L"\nTotal: " + fd::HumanBytes(task->total) +
        L"\nCurrent speed: " + fd::HumanRate(Rate(task->id)) +
        L"\nTorrent download speed: " + fd::HumanRate(static_cast<double>(task->downloadRate)) +
        L"\nTorrent upload speed: " + fd::HumanRate(static_cast<double>(task->uploadRate)) +
        L"\nSeeds: " + std::to_wstring(task->seeds) +
        L"\nPeers: " + std::to_wstring(task->peers) +
        L"\nEstimated time: " + Eta(*task) +
        L"\n\nURL: " + task->url +
        L"\n\nDestination: " + task->destination +
        L"\n\nDetails: " + task->message;
    MessageBoxW(g.window,report.c_str(),L"FASTDOWN - Transfer details",MB_OK|MB_ICONINFORMATION);
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8,0,text.data(),
        static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),
                        result.data(),size,nullptr,nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return L"";
    const int length = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
        text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!length) return L"";
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),
        static_cast<int>(text.size()),result.data(),length);
    return result;
}

std::wstring CSVField(std::wstring text) {
    std::wstring output = L"\"";
    for (wchar_t character : text) {
        if (character == L'\"') output += L"\"\"";
        else output += character;
    }
    return output + L"\"";
}

void ExportLinks(bool csv) {
    std::wstring path;
    if (!ShowFileDialog(path,csv ? L"Export transfer report" : L"Export URLs",
        csv ? L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0"
            : L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0",true)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file,path.c_str(),L"wb") != 0 || !file) {
        MessageBoxW(g.window,L"Cannot create the export file.",L"FASTDOWN",MB_ICONERROR);
        return;
    }
    const unsigned char bom[] = {0xEF,0xBB,0xBF};
    fwrite(bom,1,sizeof(bom),file);
    auto write = [&](const std::wstring& line) {
        const std::string utf = Utf8(line);
        if (!utf.empty()) fwrite(utf.data(),1,utf.size(),file);
    };
    if (csv) write(L"ID,Filename,Status,DownloadedBytes,TotalBytes,Protocol,URL,Destination,Message\r\n");
    for (const auto& item : g.snapshots) {
        if (!csv) { write(item.url + L"\r\n"); continue; }
        write(std::to_wstring(item.id) + L"," +
              CSVField(fd::FilenameFromUrl(item.url)) + L"," +
              CSVField(fd::StateName(item.state)) + L"," +
              std::to_wstring(item.done) + L"," +
              std::to_wstring(item.total) + L"," +
              CSVField(fd::ProtocolName(item.url)) + L"," +
              CSVField(item.url) + L"," +
              CSVField(item.destination) + L"," +
              CSVField(item.message) + L"\r\n");
    }
    fclose(file);
    g.banner = L"Queue exported";
    InvalidateRect(g.window,nullptr,FALSE);
}

bool ReadFileText(const std::wstring& path,std::wstring& content) {
    FILE* file = nullptr;
    if (_wfopen_s(&file,path.c_str(),L"rb") != 0 || !file) return false;
    if (fseek(file,0,SEEK_END) != 0) { fclose(file); return false; }
    const long length = ftell(file);
    if (length < 0 || length > 4*1024*1024) { fclose(file); return false; }
    rewind(file);
    std::string bytes(static_cast<std::size_t>(length),'\0');
    const std::size_t got = fread(bytes.data(),1,bytes.size(),file);
    fclose(file);
    bytes.resize(got);
    if (bytes.size()>=2 && static_cast<unsigned char>(bytes[0])==0xFF &&
        static_cast<unsigned char>(bytes[1])==0xFE) {
        if ((bytes.size()-2)%2) return false;
        content.clear();
        for (std::size_t index=2;index+1<bytes.size();index+=2) {
            const unsigned a=static_cast<unsigned char>(bytes[index]);
            const unsigned b=static_cast<unsigned char>(bytes[index+1]);
            content.push_back(static_cast<wchar_t>(a|(b<<8)));
        }
        return true;
    }
    if (bytes.size()>=3 && static_cast<unsigned char>(bytes[0])==0xEF &&
        static_cast<unsigned char>(bytes[1])==0xBB && static_cast<unsigned char>(bytes[2])==0xBF)
        bytes.erase(0,3);
    content=FromUtf8(bytes);
    return bytes.empty()||!content.empty();
}

void ImportLinks() {
    std::wstring path;
    if (!ShowFileDialog(path,L"Import URLs from text",
        L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0",false)) return;
    std::wstring text;
    if (!ReadFileText(path,text)) {
        MessageBoxW(g.window,L"Unsupported file encoding or file exceeds 4 MiB.",
                    L"FASTDOWN",MB_ICONERROR);
        return;
    }
    // A text import can contain many URLs: enqueue directly instead of
    // placing multiple lines in a single-line edit control.
    g.protocolIndex = 0;
    SendMessageW(g.protocol,CB_SETCURSEL,0,0);
    AddSourcesText(text);
}

void ChangeFilter(int index) {
    g.navIndex=std::clamp(index,0,static_cast<int>(g.navigation.size())-1);
    for (HWND button : g.navigation) InvalidateRect(button,nullptr,FALSE);
    PopulateQueue();
    InvalidateRect(g.window,nullptr,FALSE);
}

void CycleTheme() {
    g.themeIndex=(g.themeIndex+1)%static_cast<int>(fd::Themes().size());
    SendMessageW(g.theme,CB_SETCURSEL,g.themeIndex,0);
    ApplyTheme();
}
void CycleLayout() {
    g.layoutIndex=(g.layoutIndex+1)%static_cast<int>(fd::Layouts().size());
    SendMessageW(g.layout,CB_SETCURSEL,g.layoutIndex,0);
    RebuildListRowHeight();
    LayoutWindows();
}
void CycleMode() {
    const int next=(static_cast<int>(g.manager.GetMode())+1)%4;
    g.manager.SetMode(static_cast<fd::Mode>(next));
    SendMessageW(g.mode,CB_SETCURSEL,next,0);
    g.banner=L"Mode: "+fd::ModeName(g.manager.GetMode());
    InvalidateRect(g.window,nullptr,FALSE);
}

void CopySummary() {
    std::wstring report=L"FASTDOWN1.0 TRANSFER REPORT\r\n";
    for (const auto& item:g.snapshots)
        report += std::to_wstring(item.id)+L" | "+fd::StateName(item.state)+
                  L" | "+fd::HumanBytes(item.done)+L" | "+item.url+L"\r\n";
    ClipboardSet(report);
}

void ShowContextMenu(POINT point) {
    HMENU menu=CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu,MF_STRING,C_ShowDetails,L"Transfer details");
    AppendMenuW(menu,MF_STRING,C_CopyUrl,L"Copy URL");
    AppendMenuW(menu,MF_STRING,C_CopyPath,L"Copy destination");
    AppendMenuW(menu,MF_STRING,C_OpenFolder,L"Open containing folder");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,C_Resume,L"Resume selected");
    AppendMenuW(menu,MF_STRING,C_Pause,L"Pause selected");
    AppendMenuW(menu,MF_STRING,C_Stop,L"Cancel selected");
    AppendMenuW(menu,MF_STRING,C_Retry,L"Retry selected");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,C_CopyReport,L"Copy queue report");
    AppendMenuW(menu,MF_STRING,C_ExportCSV,L"Export as CSV");
    TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,g.window,nullptr);
    DestroyMenu(menu);
}

void DrawProgress(NMLVCUSTOMDRAW* custom) {
    const int row=static_cast<int>(custom->nmcd.dwItemSpec);
    if (row<0 || row>=static_cast<int>(g.visible.size())) return;
    RECT area{};
    ListView_GetSubItemRect(g.list,row,3,LVIR_BOUNDS,&area);
    const auto& item=g.visible[static_cast<std::size_t>(row)];
    HDC dc=custom->nmcd.hdc;
    const bool selected=(ListView_GetItemState(g.list,row,LVIS_SELECTED)&LVIS_SELECTED)!=0;
    Fill(dc,area,selected ? T().hover : T().panel);
    const int trackWidth=std::max<int>(S(20),static_cast<int>(area.right-area.left)-S(30));
    const int barHeight=S(7);
    const int center=(area.top+area.bottom)/2;
    RECT track=Rect(area.left+S(12),center-S(4),trackWidth,barHeight);
    Round(dc,track,S(7),T().raised,T().raised);
    const double progress=fd::performance::CompletionBasisPoints(item.done,item.total) / 10000.0;
    if (progress>0) {
        RECT filled=track;
        filled.right=filled.left+std::max(S(3),static_cast<int>(progress*trackWidth));
        const COLORREF primary = item.state == fd::State::Failed ? T().danger :
            item.state == fd::State::Paused ? T().warning : T().success;
        Gradient(dc,filled,primary,T().accent,S(7));
    }
    std::wstring label=item.total>0 ?
        std::to_wstring(fd::performance::CompletionBasisPoints(item.done,item.total)/100u) + L"%"
                                     : item.state==fd::State::HandedOff ? L"External" : L"--";
    DrawLabel(dc,label,Rect(track.left,area.top,trackWidth,center-area.top-S(6)),
              g.fontSmall,T().muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
}

LRESULT OnHeaderCustomDraw(NMCUSTOMDRAW* custom) {
    if (custom->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (custom->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND header = ListView_GetHeader(g.list);
    const int itemIndex = static_cast<int>(custom->dwItemSpec);
    wchar_t caption[128]{};
    HDITEMW item{};
    item.mask = HDI_TEXT;
    item.pszText = caption;
    item.cchTextMax = static_cast<int>(_countof(caption));
    SendMessageW(header, HDM_GETITEMW, static_cast<WPARAM>(itemIndex), reinterpret_cast<LPARAM>(&item));
    RECT r = custom->rc;
    Fill(custom->hdc, r, T().raised);
    Fill(custom->hdc, Rect(r.left, r.bottom-S(1), r.right-r.left, S(1)), T().border);
    r.left += S(12);
    r.right -= S(8);
    DrawLabel(custom->hdc, caption, r, g.fontSmall, T().muted);
    return CDRF_SKIPDEFAULT;
}

LRESULT CALLBACK ListSubclass(HWND control, UINT message, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    if (message == WM_PAINT) {
        const LRESULT result = DefSubclassProc(control,message,wp,lp);
        if (ListView_GetItemCount(control) == 0) {
            RECT r{};
            GetClientRect(control,&r);
            HDC dc = GetDC(control);
            if (dc) {
                const int center = (r.top+r.bottom)/2;
                DrawLabel(dc, L"No downloads in this view",
                    Rect(r.left+S(20),center-S(42),r.right-r.left-S(40),S(43)),
                    g.fontTitle,T().text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                DrawLabel(dc, L"Use Add URL to create a task or change the category filter.",
                    Rect(r.left+S(20),center+S(5),r.right-r.left-S(40),S(29)),
                    g.font,T().muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                ReleaseDC(control,dc);
            }
        }
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control,ListSubclass,1);
    return DefSubclassProc(control,message,wp,lp);
}

LRESULT OnListCustomDraw(NMLVCUSTOMDRAW* custom) {
    switch (custom->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const int row=static_cast<int>(custom->nmcd.dwItemSpec);
        const bool selected=(ListView_GetItemState(g.list,row,LVIS_SELECTED)&LVIS_SELECTED)!=0;
        custom->clrText=selected ? T().text :
            (custom->iSubItem==2 && row>=0 && row<static_cast<int>(g.visible.size()) &&
             g.visible[static_cast<std::size_t>(row)].state==fd::State::Failed)
             ? T().danger : T().text;
        custom->clrTextBk=selected ? T().hover : T().panel;
        if (custom->iSubItem==3) {
            DrawProgress(custom);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

void DropFiles(HDROP drop) {
    const UINT count=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
    if (count>0) {
        wchar_t path[MAX_PATH]{};
        DragQueryFileW(drop,0,path,_countof(path));
        const std::wstring name=Lower(path);
        if (fd::IsLocalTorrentFile(path)) {
            ShowComposer(true);
            WriteText(g.url,path);
            g.protocolIndex=5;
            SendMessageW(g.protocol,CB_SETCURSEL,5,0);
            WriteText(g.go,L"Start download");
            ShowWindow(g.torrent,SW_SHOW);
        } else if (name.size()>=4 && name.substr(name.size()-4)==L".txt") {
            std::wstring content;
            if (ReadFileText(path,content)) {
                g.protocolIndex=0;
                SendMessageW(g.protocol,CB_SETCURSEL,0,0);
                AddSourcesText(content);
            }
        }
    }
    DragFinish(drop);
}

void InitializeControls() {
    CreateFonts();
    g.add=Button(L"Add URL",C_Add);
    g.resume=Button(L"Resume",C_Resume);
    g.pause=Button(L"Pause",C_Pause);
    g.stop=Button(L"Stop",C_Stop);
    g.retry=Button(L"Retry",C_Retry);
    g.remove=Button(L"Clear finished",C_Remove);
    g.import=Button(L"Import",C_Import);
    g.exportButton=Button(L"Export",C_Export);
    g.openFolder=Button(L"Open folder",C_OpenFolder);
    g.search=Edit(L"",C_Search);
    SendMessageW(g.search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search transfers..."));
    g.url=Edit(L"",C_Url);
    SendMessageW(g.url,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"https://example.com/file.zip"));
    g.directory=Edit(DefaultDownloadFolder().c_str(),C_Directory);
    g.go=Button(L"Start download",C_Go);
    g.browseFolder=Button(L"Browse",C_BrowseFolder);
    g.protocol=Combo(C_Protocol);
    g.torrent=Button(L"Browse .torrent",C_BrowseTorrent);
    const wchar_t* protocols[]={L"Auto Detect",L"HTTP",L"HTTPS",L"Magnet",L"ED2K",
                                 L"BitTorrent",L"FTP",L"FTPS"};
    for (const auto* value:protocols) AddCombo(g.protocol,value);
    SendMessageW(g.protocol,CB_SETCURSEL,0,0);
    const wchar_t* navs[]={L"All downloads",L"Active",L"Completed",L"Paused",L"Failed",
                           L"External clients",L"Music",L"Videos",L"Archives"};
    for (int i=0;i<static_cast<int>(g.navigation.size());++i)
        g.navigation[static_cast<std::size_t>(i)]=Button(navs[i],C_NavAll+i);
    g.theme=Combo(C_Themes);
    g.layout=Combo(C_Layouts);
    g.mode=Combo(C_Modes);
    for (const auto& theme:fd::Themes()) AddCombo(g.theme,theme.name.c_str());
    for (const auto& layout:fd::Layouts()) AddCombo(g.layout,layout.name.c_str());
    AddCombo(g.mode,L"Eco - Low Impact");
    AddCombo(g.mode,L"Balanced - Recommended");
    AddCombo(g.mode,L"Turbo - High Throughput");
    AddCombo(g.mode,L"Maximum - Aggressive");
    g.themeIndex = static_cast<int>(ReadPreference(L"Theme", 0,
        static_cast<DWORD>(fd::Themes().size() - 1)));
    g.layoutIndex = static_cast<int>(ReadPreference(L"Layout", 0,
        static_cast<DWORD>(fd::Layouts().size() - 1)));
    g.themeIndex = LookupSavedTheme(fd::Themes(), g.themeIndex);
    g.layoutIndex = LookupSavedLayout(fd::Layouts(), g.layoutIndex);
    const DWORD modeChoice = ReadPreference(L"Mode", 1, 3);
    SendMessageW(g.theme, CB_SETCURSEL, g.themeIndex, 0);
    SendMessageW(g.layout, CB_SETCURSEL, g.layoutIndex, 0);
    SendMessageW(g.mode, CB_SETCURSEL, modeChoice, 0);
    g.manager.SetMode(static_cast<fd::Mode>(modeChoice));

    g.list=CreateWindowExW(0,WC_LISTVIEWW,L"",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL,
        0,0,100,100,g.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(C_List)),
        GetModuleHandleW(nullptr),nullptr);
    ListView_SetExtendedListViewStyle(g.list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_INFOTIP|
                                             LVS_EX_LABELTIP|LVS_EX_HEADERDRAGDROP);
    SetWindowSubclass(g.list,ListSubclass,1,0);
    SetFont(g.list,g.font);
    const wchar_t* headings[]={L"Name",L"Size",L"Status",L"Progress",L"Speed",L"Protocol",L"Time left"};
    for (int col=0;col<static_cast<int>(_countof(headings));++col) {
        LVCOLUMNW column{};
        column.mask=LVCF_WIDTH|LVCF_TEXT|LVCF_FMT;
        column.cx=120;
        column.fmt=col==1 || col==4 || col==6 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        column.pszText=const_cast<LPWSTR>(headings[col]);
        ListView_InsertColumn(g.list,col,&column);
    }
    RebuildListRowHeight();
    ApplyTheme();
    ShowComposer(false);
    LayoutWindows();
    UpdateDiskSpace(true);
}

LRESULT CALLBACK WndProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        g.window=window;
        g.dpi=GetDpiForWindow(window);
        InitializeControls();
        DragAcceptFiles(window,TRUE);
        SetTimer(window,RefreshTimer,650,nullptr);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits=reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMinTrackSize.x=S(WindowMinWidth);
        limits->ptMinTrackSize.y=S(WindowMinHeight);
        return 0;
    }
    case WM_SIZE:
        LayoutWindows();
        return 0;
    case WM_DPICHANGED: {
        g.dpi=HIWORD(wp);
        const std::array<HFONT,5> oldFonts{{g.font,g.fontSmall,g.fontMedium,g.fontTitle,g.fontStat}};
        CreateFonts();
        for (HWND control : {g.search,g.url,g.directory,g.protocol,g.theme,g.layout,g.mode,g.list,
                             g.add,g.resume,g.pause,g.stop,g.retry,g.remove,g.import,g.exportButton,
                             g.openFolder,g.go,g.browseFolder,g.torrent}) SetFont(control,g.font);
        for (HWND control : g.navigation) SetFont(control,g.font);
        for (HFONT font : oldFonts) if (font) DeleteObject(font);
        RebuildListRowHeight();
        const RECT* suggested=reinterpret_cast<RECT*>(lp);
        SetWindowPos(window,nullptr,suggested->left,suggested->top,
                     suggested->right-suggested->left,suggested->bottom-suggested->top,
                     SWP_NOZORDER|SWP_NOACTIVATE);
        LayoutWindows();
        InvalidateRect(window,nullptr,TRUE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(window); return 0;
    case WM_DRAWITEM:
        if (const auto* item=reinterpret_cast<const DRAWITEMSTRUCT*>(lp)) {
            if (item->CtlType==ODT_BUTTON) { DrawButton(item); return TRUE; }
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,T().text);
        SetBkColor(dc,T().raised);
        return reinterpret_cast<LRESULT>(g.editBrush);
    }
    case WM_NOTIFY: {
        const auto* notice=reinterpret_cast<NMHDR*>(lp);
        if (!notice) break;
        if (notice->hwndFrom==ListView_GetHeader(g.list) && notice->code==NM_CUSTOMDRAW)
            return OnHeaderCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(lp));
        if (notice->hwndFrom!=g.list) break;
        if (notice->code==NM_CUSTOMDRAW)
            return OnListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(lp));
        if (notice->code==NM_DBLCLK) { ShowSelectedDetails(); return 0; }
        if (notice->code==LVN_GETINFOTIPW) {
            auto* tip=reinterpret_cast<NMLVGETINFOTIPW*>(lp);
            if (tip->iItem>=0 && static_cast<std::size_t>(tip->iItem)<g.visible.size() &&
                tip->pszText && tip->cchTextMax>0) {
                const auto& item=g.visible[static_cast<std::size_t>(tip->iItem)];
                // Avoid exposing full URL credentials in tooltips.
                const std::wstring details=fd::FilenameFromUrl(item.url) + L"\n" +
                    fd::StateName(item.state) + L" | " + item.message + L"\n" + item.destination;
                wcsncpy_s(tip->pszText,static_cast<std::size_t>(tip->cchTextMax),
                          details.c_str(),_TRUNCATE);
            }
            return 0;
        }
        if (notice->code==LVN_COLUMNCLICK) {
            const auto* click=reinterpret_cast<const NMLISTVIEW*>(lp);
            if (g.sortColumn==click->iSubItem) g.descending=!g.descending;
            else { g.sortColumn=click->iSubItem; g.descending=false; }
            PopulateQueue();
            return 0;
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp)==g.list) {
            POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if (point.x<0 && point.y<0) GetCursorPos(&point);
            ShowContextMenu(point);
            return 0;
        }
        break;
    case WM_DROPFILES:
        DropFiles(reinterpret_cast<HDROP>(wp));
        return 0;
    case WM_TIMER:
        if (wp==RefreshTimer) { Refresh(); return 0; }
        if (wp==SearchTimer) {
            KillTimer(window, SearchTimer);
            PopulateQueue();
            InvalidateRect(window,nullptr,FALSE);
            return 0;
        }
        break;
    case WM_COMMAND: {
        const int id=LOWORD(wp);
        const int notify=HIWORD(wp);
        if (id>=C_NavAll && id<=C_NavArchives) {
            ChangeFilter(id-C_NavAll);
            return 0;
        }
        switch (id) {
        case C_Add: ShowComposer(!g.composer); if (g.composer) SetFocus(g.url); break;
        case C_Go: AddSources(); break;
        case C_BrowseFolder: {
            const std::wstring selected=SelectDirectory();
            if (!selected.empty()) WriteText(g.directory,selected);
            break;
        }
        case C_BrowseTorrent: BrowseTorrent(); break;
        case C_Resume: if (SelectedID()) g.manager.Resume(SelectedID()); else g.manager.ResumeAll(); Refresh(true); break;
        case C_Pause: if (SelectedID()) g.manager.Pause(SelectedID()); else g.manager.PauseAll(); Refresh(true); break;
        case C_Stop:
            if (SelectedID()) g.manager.Cancel(SelectedID());
            else if (MessageBoxW(window,L"Cancel all active transfers?",L"FASTDOWN",
                                 MB_YESNO|MB_ICONWARNING)==IDYES) g.manager.CancelAll();
            Refresh(true); break;
        case C_Retry: if (SelectedID()) g.manager.Retry(SelectedID()); else g.manager.RetryFailed(); Refresh(true); break;
        case C_Remove: g.manager.RemoveFinished(); Refresh(true); break;
        case C_Import: ImportLinks(); break;
        case C_Export: ExportLinks(false); break;
        case C_ExportCSV: ExportLinks(true); break;
        case C_OpenFolder: {
            if (SelectedID()) OpenSelectedFile(true);
            else OpenFolder();
            break;
        }
        case C_Protocol:
            if (notify==CBN_SELCHANGE) {
                g.protocolIndex=static_cast<int>(SendMessageW(g.protocol,CB_GETCURSEL,0,0));
                ShowWindow(g.torrent,g.composer && g.protocolIndex==5 ? SW_SHOW : SW_HIDE);
                WriteText(g.go,L"Start download");
            }
            break;
        case C_Search:
            if (notify==EN_CHANGE) SetTimer(window,SearchTimer,180,nullptr);
            break;
        case C_Directory: if (notify==EN_KILLFOCUS) UpdateDiskSpace(true); break;
        case C_Themes:
            if (notify==CBN_SELCHANGE) {
                g.themeIndex=static_cast<int>(SendMessageW(g.theme,CB_GETCURSEL,0,0));
                ApplyTheme();
            }
            break;
        case C_Layouts:
            if (notify==CBN_SELCHANGE) {
                g.layoutIndex=static_cast<int>(SendMessageW(g.layout,CB_GETCURSEL,0,0));
                RebuildListRowHeight();
                LayoutWindows();
            }
            break;
        case C_Modes:
            if (notify==CBN_SELCHANGE) {
                const int selection=static_cast<int>(SendMessageW(g.mode,CB_GETCURSEL,0,0));
                if (selection>=0 && selection<=3) {
                    g.manager.SetMode(static_cast<fd::Mode>(selection));
                    g.banner=L"Mode: "+fd::ModeName(g.manager.GetMode());
                    InvalidateRect(window,nullptr,FALSE);
                }
            }
            break;
        case C_ShowDetails: ShowSelectedDetails(); break;
        case C_CopyUrl: if (const auto* item=SelectedTask()) ClipboardSet(item->url); break;
        case C_CopyPath: if (const auto* item=SelectedTask()) ClipboardSet(item->destination); break;
        case C_CopyReport: CopySummary(); break;
        case C_CopyAll: {
            std::wstring links;
            for (const auto& item:g.snapshots) links+=item.url+L"\r\n";
            ClipboardSet(links);
            break;
        }
        case C_ToggleTheme: CycleTheme(); break;
        case C_ToggleLayout: CycleLayout(); break;
        case C_ToggleMode: CycleMode(); break;
        case C_FocusURL: ShowComposer(true); SetFocus(g.url); break;
        case C_FocusSearch: SetFocus(g.search); break;
        case C_Refresh: Refresh(true); break;
        case C_About:
            MessageBoxW(window,L"FASTDOWN1.0 A17\n\nNative Windows download manager.\n"
                L"HTTP/HTTPS downloads are handled internally with WinHTTP.\n"
                L"Magnet and .torrent use libtorrent; FTP uses WinINet.\n"
                L"FTPS uses libcurl with TLS certificate verification.\n"
                L"ED2K requires a registered external client.\n"
                L"Maximum mode increases concurrency but cannot guarantee a speed multiplier.",
                L"About FASTDOWN",MB_OK|MB_ICONINFORMATION);
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        PersistAppearance();
        KillTimer(window,RefreshTimer);
        KillTimer(window,SearchTimer);
        g.manager.CancelAll();
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        if (g.rowImages) { ImageList_Destroy(g.rowImages); g.rowImages=nullptr; }
        for (HFONT f:{g.font,g.fontSmall,g.fontMedium,g.fontTitle,g.fontStat})
            if (f) DeleteObject(f);
        if (g.editBrush) { DeleteObject(g.editBrush); g.editBrush=nullptr; }
        break;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{};
    common.dwSize=sizeof(common);
    common.dwICC=ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common);
    WNDCLASSEXW cls{};
    cls.cbSize=sizeof(cls);
    cls.lpfnWndProc=WndProc;
    cls.hInstance=instance;
    cls.lpszClassName=AppClass;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    cls.hbrBackground=nullptr;
    if (!RegisterClassExW(&cls)) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    HWND window=CreateWindowExW(0,AppClass,L"FASTDOWN1.0 | Download Studio",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,1420,910,
        nullptr,nullptr,instance,nullptr);
    if (!window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 2;
    }
    const BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(20),
                          &darkTitle, sizeof(darkTitle));
    ShowWindow(window,show);
    UpdateWindow(window);
    ACCEL shortcuts[] = {
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'L',C_FocusURL},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'N',C_Add},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'P',C_Pause},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'R',C_Resume},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'F',C_FocusSearch},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'O',C_Import},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'E',C_Export},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'T',C_ToggleTheme},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'M',C_ToggleMode},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL),'D',C_ShowDetails},
        {static_cast<BYTE>(FVIRTKEY),VK_F5,C_Refresh},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL|FSHIFT),'S',C_ExportCSV},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL|FSHIFT),'L',C_ToggleLayout},
        {static_cast<BYTE>(FVIRTKEY|FCONTROL|FSHIFT),'R',C_Retry},
        {static_cast<BYTE>(FVIRTKEY),VK_F1,C_About}
    };
    HACCEL accelerator=CreateAcceleratorTableW(shortcuts,static_cast<int>(_countof(shortcuts)));
    MSG msg{};
    while (GetMessageW(&msg,nullptr,0,0)>0) {
        if (!accelerator || !TranslateAcceleratorW(window,accelerator,&msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (accelerator) DestroyAcceleratorTable(accelerator);
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
