#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace fd {
struct Theme {
    std::wstring name;
    COLORREF bg, panel, raised, accent, text, muted, border;
    COLORREF success, warning, danger, hover, sidebar;
};
struct Layout {
    std::wstring name;
    int sidebar = 240;
    int rowHeight = 45;
    int radius = 14;
    int toolbarHeight = 62;
    int gap = 12;
    bool compact = false;
    bool wide = false;
};
const std::vector<Theme>& Themes();
const std::vector<Layout>& Layouts();
COLORREF Blend(COLORREF first, COLORREF second, int percentage);
}
