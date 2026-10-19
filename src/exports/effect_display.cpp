#define NOMINMAX
#include "mikumikustudio/effect_display.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <commctrl.h>
#include <cwchar>

namespace mikumikustudio {
namespace {
constexpr UINT_PTR kDisplaySubclass = 0x4D4D454C;
std::wstring Item(HWND list, int row, LPARAM* identity = nullptr) {
    wchar_t text[2048]{};
    LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row; item.pszText = text; item.cchTextMax = 2048;
    if (!SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))) return {};
    if (identity) *identity = item.lParam;
    return text;
}
// MME's expanded rows use the same opaque item identity as their model row.
// Never dereference that identity or modify it, the text, or an MME name query.
int MaterialIndex(const std::wstring& text, std::size_t& prefix) {
    if (text.compare(0, 6, L"  |-- ") && text.compare(0, 6, L"  `-- ")) return -1;
    wchar_t* end = nullptr;
    const long index = std::wcstol(text.c_str() + 6, &end, 10);
    if (end == text.c_str() + 6 || index < 0 || index > 1000000 || end[0] != L':' || end[1] != L' ') return -1;
    prefix = end + 2 - text.c_str();
    return int(index);
}
void PaintLabel(MMDApp* app, NMLVCUSTOMDRAW& draw) {
    const HWND list = draw.nmcd.hdr.hwndFrom;
    const int row = int(draw.nmcd.dwItemSpec);
    const auto label = EffectListDisplayLabel(app, list, row);
    if (label == Item(list, row)) return;
    RECT rect{}, bounds{};
    if (!ListView_GetItemRect(list, row, &rect, LVIR_LABEL) ||
        !ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS)) return;
    rect.right = (std::min)(rect.right, bounds.left + ListView_GetColumnWidth(list, 0));
    if (rect.right <= rect.left) return;
    const UINT state = ListView_GetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED);
    const bool focus = GetFocus() == list;
    const bool selected = (state & LVIS_SELECTED) && (focus || (GetWindowLongPtrW(list, GWL_STYLE) & LVS_SHOWSELALWAYS));
    COLORREF background = selected ? GetSysColor(focus ? COLOR_HIGHLIGHT : COLOR_BTNFACE) : ListView_GetTextBkColor(list);
    COLORREF foreground = selected && focus ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ListView_GetTextColor(list);
    if (background == CLR_NONE || background == CLR_DEFAULT) background = GetSysColor(COLOR_WINDOW);
    if (foreground == CLR_DEFAULT) foreground = GetSysColor(COLOR_WINDOWTEXT);
    const int saved = SaveDC(draw.nmcd.hdc);
    IntersectClipRect(draw.nmcd.hdc, rect.left, rect.top, rect.right, rect.bottom);
    HBRUSH brush = CreateSolidBrush(background); FillRect(draw.nmcd.hdc, &rect, brush); DeleteObject(brush);
    SetBkMode(draw.nmcd.hdc, TRANSPARENT); SetTextColor(draw.nmcd.hdc, foreground);
    if (app->state.hFontUI) SelectObject(draw.nmcd.hdc, app->state.hFontUI);
    RECT text = rect; text.left += 2;
    DrawTextW(draw.nmcd.hdc, label.c_str(), int(label.size()), &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (focus && (state & LVIS_FOCUSED)) DrawFocusRect(draw.nmcd.hdc, &rect);
    RestoreDC(draw.nmcd.hdc, saved);
}
LRESULT CALLBACK DisplayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, DisplayProc, id);
    const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if (message != WM_NOTIFY || !lp) return result;
    const auto* header = reinterpret_cast<const NMHDR*>(lp);
    if (header->code != NM_CUSTOMDRAW) return result;
    wchar_t cls[64]{}; GetClassNameW(header->hwndFrom, cls, 64);
    if (_wcsicmp(cls, WC_LISTVIEWW)) return result;
    auto& draw = *reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return result | CDRF_NOTIFYITEMDRAW;
    if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return result | CDRF_NOTIFYSUBITEMDRAW;
    if (draw.iSubItem == 0) {
        if (draw.nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) return result | CDRF_NOTIFYPOSTPAINT;
        if (draw.nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPOSTPAINT))
            PaintLabel(reinterpret_cast<MMDApp*>(ref), draw);
    }
    return result;
}
}
std::wstring EffectListDisplayLabel(MMDApp* app, HWND list, int row) {
    LPARAM identity = 0;
    const auto original = Item(list, row, &identity);
    if (!app || !identity) return original;
    std::size_t prefix = 0;
    const int material = MaterialIndex(original, prefix);
    std::wstring file = original;
    if (material >= 0) {
        for (int parent = row - 1; parent >= 0; --parent) {
            LPARAM parentIdentity = 0;
            const auto text = Item(list, parent, &parentIdentity);
            if (identity != parentIdentity) return original;
            std::size_t ignored = 0;
            if (MaterialIndex(text, ignored) < 0) { file = text; break; }
        }
    }
    const bool english = UiUsesEnglishNames(app->EnglishUI());
    std::wstring label;
    bool matched = false;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* raw = native_mesh::RenderModel(app->ModelSlot(slot));
        if (!raw) continue;
        const auto& model = *mdl::Mdl(raw);
        std::wstring path(model.path);
        const auto slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) path.erase(0, slash + 1);
        if (path != file) continue;
        const auto candidate = material >= 0 ? text_encoding::MaterialLabel(model, material, english)
                                             : text_encoding::ModelLabel(model, english);
        // Identically named files with different metadata are ambiguous. Leave
        // the plugin's label intact rather than display another model's name.
        if (matched && candidate != label) return original;
        label = candidate; matched = true;
    }
    if (!matched || label.empty()) return original;
    return material >= 0 ? original.substr(0, prefix) + label : label;
}
void InstallEffectListDisplay(HWND editor, MMDApp* app) {
    SetWindowSubclass(editor, DisplayProc, kDisplaySubclass, reinterpret_cast<DWORD_PTR>(app));
}
}
