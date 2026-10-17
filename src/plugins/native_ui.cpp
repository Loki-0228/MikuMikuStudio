#include "native_ui.hpp"
#include <native_ui/layout.hpp>
#include <native_ui/win32_host.hpp>
#include <cmath>
#include <set>
#include <commctrl.h>

namespace mikudancestudio::plugins {
struct NativeUi::Impl {
    UiRegistry& registry;
    UiClient client;
    Log log;
    HWND main, toolbar = nullptr, workspace = nullptr, tabs = nullptr, body = nullptr, failure = nullptr;
    HMENU root = nullptr, popup = nullptr;
    unsigned nextId = 0xa000;
    uint64_t revision = 0;
    int x = 0, width = 0, maximum = 120, height = 0, scroll = 0, contentHeight = 0;
    bool visible = true, refreshing = false;
    std::map<std::string, unsigned> ids;
    std::map<unsigned, std::pair<std::string, uint64_t>> dispatch;
    struct Control { HWND window = nullptr; MmsHandle element = 0; uint64_t instance = 0; };
    std::map<std::string, Control> controls;
    std::vector<std::string> toolbarOrder;
    std::vector<std::string> panelOrder;
    std::string activePanel;
    std::map<std::string, MmsResult> panelErrors;
    uint32_t dpi = 96;
    native_ui::PanelBuffer toolbarBuffer, workspaceBuffer, bodyBuffer;
    int workspaceHeight = 0, panelScrollX = 0, panelScrollY = 0, panelWidth = 0, panelHeight = 0;
    Impl(UiRegistry& r, HWND window, Log output): registry(r), client{&r, "mms"}, log(std::move(output)), main(window) {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MMS.PluginAbi.Toolbar.V1"; wc.lpfnWndProc = Window; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("plugin toolbar class");
        toolbar = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"插件工具栏", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 0, 0, main, nullptr, wc.hInstance, this);
        if (!toolbar) throw std::runtime_error("plugin toolbar window");
        wc.lpszClassName = L"MMS.PluginAbi.Workspace.V1";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { DestroyWindow(toolbar); throw std::runtime_error("plugin workspace class"); }
        workspace = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"插件工作区", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, main, nullptr, wc.hInstance, this);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
        if (workspace) tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, workspace, nullptr, wc.hInstance, nullptr);
        if (workspace) body = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"插件面板容器", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL, 0, 0, 0, 0, workspace, nullptr, wc.hInstance, this);
        if (body) failure = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 0, 0, body, nullptr, wc.hInstance, nullptr);
        if (!workspace || !tabs || !body || !failure) { if (workspace) DestroyWindow(workspace); DestroyWindow(toolbar); throw std::runtime_error("plugin workspace tabs"); }
        SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        SendMessageW(failure, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    }
    ~Impl() {
        registry.Refresh().SuspendTargets();
        Boundary([&] { registry.Refresh().SyncTargets({}); return MMS_OK; });
        Boundary([&] { registry.DestroyViews(); return MMS_OK; });
        for (auto& [id, control] : controls) DestroyControl(control);
        Detach(); if (IsWindow(workspace)) DestroyWindow(workspace); if (IsWindow(toolbar)) DestroyWindow(toolbar);
    }
    void DestroyControl(Control& control) noexcept {
        registry.Refresh().InvalidateTarget(control.element);
        if (control.window) DestroyWindow(control.window);
        control = {};
    }
    static LRESULT CALLBACK Window(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_COMMAND && lp && HIWORD(wp) == BN_CLICKED) { Boundary([&] { self->Command(LOWORD(wp)); return MMS_OK; }); return 0; }
        if (msg == WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->hwndFrom == self->tabs && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) {
            int index = TabCtrl_GetCurSel(self->tabs); if (index >= 0 && size_t(index) < self->panelOrder.size()) {
                self->activePanel = self->panelOrder[index]; Boundary([&] { self->Layout(); return self->registry.FocusSurface(self->activePanel); });
            } return 0;
        }
        if (hwnd == self->body && (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL)) {
            const bool horizontal = msg == WM_HSCROLL; int& position = horizontal ? self->panelScrollX : self->panelScrollY;
            SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, horizontal ? SB_HORZ : SB_VERT, &info); int to = position;
            if (msg == WM_MOUSEWHEEL) to -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 28;
            else switch (LOWORD(wp)) { case SB_LINEUP: to -= 28; break; case SB_LINEDOWN: to += 28; break; case SB_PAGEUP: to -= info.nPage; break; case SB_PAGEDOWN: to += info.nPage; break; case SB_THUMBTRACK: to = info.nTrackPos; break; default: break; }
            position = std::clamp(to, 0, std::max(0, info.nMax - int(info.nPage) + 1)); Boundary([&] { self->Layout(); return MMS_OK; }); return 0;
        }
        if (hwnd == self->toolbar && (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL)) {
            SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &info);
            int to = self->scroll;
            if (msg == WM_MOUSEWHEEL) to -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 28;
            else switch (LOWORD(wp)) { case SB_LINEUP: to -= 28; break; case SB_LINEDOWN: to += 28; break; case SB_PAGEUP: to -= self->height; break; case SB_PAGEDOWN: to += self->height; break; case SB_THUMBTRACK: to = info.nTrackPos; break; default: break; }
            self->scroll = std::clamp(to, 0, std::max(0, self->contentHeight - self->height));
            Boundary([&] { self->Layout(); return MMS_OK; }); return 0;
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); RECT rect{}; GetClientRect(hwnd, &rect);
            auto& buffer = hwnd == self->toolbar ? self->toolbarBuffer : hwnd == self->body ? self->bodyBuffer : self->workspaceBuffer;
            const auto color = GetSysColor(COLOR_BTNFACE); uint64_t key = (uint64_t(self->dpi) << 32) | color;
            HDC memory = buffer.Begin(dc, rect.right, rect.bottom, key, color);
            if (memory) { FillRect(memory, &ps.rcPaint, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1)); if (!buffer.Copy(dc, ps.rcPaint)) FillRect(dc, &ps.rcPaint, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1)); }
            else FillRect(dc, &ps.rcPaint, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
            EndPaint(hwnd, &ps); return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    int Position() const {
        if (!IsMenu(root)) return -1;
        for (int i = 0; i < GetMenuItemCount(root); ++i) {
            MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_DATA | MIIM_SUBMENU;
            if (GetMenuItemInfoW(root, i, TRUE, &item) && item.dwItemData == reinterpret_cast<ULONG_PTR>(this) && item.hSubMenu == popup) return i;
        }
        return -1;
    }
    void Detach() { if (Position() >= 0) { RemoveMenu(root, Position(), MF_BYPOSITION); DestroyMenu(popup); } root = popup = nullptr; }
    unsigned Id(const std::string& id, const ElementRecord& e) {
        auto it = ids.find(id);
        if (it != ids.end()) {
            if (dispatch.at(it->second).second == e.handle) return it->second;
            auto control = controls.find(id); if (control != controls.end()) { DestroyControl(control->second); controls.erase(control); }
            dispatch.erase(it->second); ids.erase(it);
        }
        HMENU menu = GetMenu(main);
        while (nextId < 0xc000 && menu && GetMenuState(menu, nextId, MF_BYCOMMAND) != UINT(-1)) ++nextId;
        if (nextId >= 0xc000) throw std::runtime_error("plugin menu command range exhausted");
        auto value = nextId++; ids.emplace(id, value); dispatch[value] = {id, e.handle}; return value;
    }
    std::vector<std::string> Children(const std::string& parent) const {
        std::vector<std::string> out;
        for (const auto& [id, e] : registry.elements) if (String(e.desc.parentId) == parent) out.push_back(id);
        std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) { return registry.elements.at(a).effectiveHints.order < registry.elements.at(b).effectiveHints.order; }); return out;
    }
    bool State(const ElementRecord& e, MmsCommandStateV1& out) {
        out = {}; out.h = Header<MmsCommandStateV1>();
        if (!e.visible) return false;
        if (!e.desc.commandId.bytes) { out.enabled = out.visible = 1; return true; }
        auto api = registry.Api(client); MmsHandle lease = 0; auto result = api.getCommandState(api.context, e.desc.commandId, {}, &out, &lease);
        api.releaseValue(api.context, lease); out.disabledReason = {};
        if (result) { out.enabled = 0; out.visible = 1; }
        return out.visible != 0;
    }
    void Menu(HMENU parent, const std::string& parentId, unsigned depth = 0) {
        if (depth > 64) return;
        for (const auto& id : Children(parentId)) {
            const auto& e = registry.elements.at(id); MmsCommandStateV1 state{}; if (!State(e, state)) continue;
            if (e.desc.kind == MMS_UI_SEPARATOR) { AppendMenuW(parent, MF_SEPARATOR, 0, nullptr); continue; }
            auto caption = Wide(String(e.desc.label));
            if (e.desc.kind == MMS_UI_MENU) { HMENU sub = CreatePopupMenu(); Menu(sub, id, depth + 1); if (!AppendMenuW(parent, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), caption.c_str())) DestroyMenu(sub); }
            else AppendMenuW(parent, MF_STRING | (state.enabled ? MF_ENABLED : MF_GRAYED) | (state.checked ? MF_CHECKED : MF_UNCHECKED), Id(id, e), caption.c_str());
        }
    }
    void Gather(const std::string& parent, std::vector<std::string>& out, unsigned depth = 0) {
        if (depth > 64) return;
        for (const auto& id : Children(parent)) { const auto& e = registry.elements.at(id); if (!e.visible) continue;
            if (e.desc.kind == MMS_UI_GROUP) Gather(id, out, depth + 1); else out.push_back(id); }
    }
    void Refresh() {
        if (refreshing || !IsWindow(main)) return;
        refreshing = true; struct Guard { bool& v; ~Guard() { v = false; } } guard{refreshing};
        HMENU menu = GetMenu(main); bool changed = false;
        const bool hasMenu = !Children("mms.menu.plugins").empty();
        if (menu != root || Position() < 0) { Detach(); if (menu && hasMenu) {
            root = menu; popup = CreatePopupMenu(); MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_DATA;
            item.hSubMenu = popup; item.dwTypeData = const_cast<wchar_t*>(L"插件 (&P)"); item.dwItemData = reinterpret_cast<ULONG_PTR>(this);
            if (!InsertMenuItemW(root, GetMenuItemCount(root), TRUE, &item)) { DestroyMenu(popup); root = popup = nullptr; } else changed = true;
        } }
        if (!hasMenu && popup) { Detach(); changed = true; }
        if (popup) { while (GetMenuItemCount(popup) > 0) DeleteMenu(popup, 0, MF_BYPOSITION); Menu(popup, "mms.menu.plugins"); }
        if (changed) DrawMenuBar(main);
        std::vector<std::string> wanted; Gather("mms.toolbar.main.extensions", wanted);
        std::set<std::string> keep(wanted.begin(), wanted.end());
        for (auto it = controls.begin(); it != controls.end();) if (!keep.count(it->first)) { DestroyControl(it->second); it = controls.erase(it); } else ++it;
        for (auto it = ids.begin(); it != ids.end();) {
            auto element = registry.elements.find(it->first);
            if (element == registry.elements.end() || dispatch.at(it->second).second != element->second.handle) {
                auto control = controls.find(it->first);
                if (control != controls.end()) { DestroyControl(control->second); controls.erase(control); }
                dispatch.erase(it->second); it = ids.erase(it);
            } else ++it;
        }
        for (const auto& id : wanted) {
            const auto& e = registry.elements.at(id); MmsCommandStateV1 state{}; bool show = State(e, state); auto caption = Wide(String(e.desc.label));
            const auto commandId = Id(id, e);
            auto& control = controls[id];
            if (!control.window) {
                const bool separator = e.desc.kind == MMS_UI_SEPARATOR;
                control.element = e.handle; control.instance = registry.NewPresentationInstance();
                control.window = CreateWindowExW(0, separator ? L"STATIC" : L"BUTTON", caption.c_str(), WS_CHILD | (separator ? SS_ETCHEDVERT : WS_TABSTOP | BS_PUSHBUTTON), 0, 0, 0, 0,
                    toolbar, reinterpret_cast<HMENU>(UINT_PTR(commandId)), GetModuleHandleW(nullptr), nullptr);
                if (!control.window) throw std::runtime_error("cannot create plugin button");
                SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
            }
            SetWindowTextW(control.window, caption.c_str()); EnableWindow(control.window, state.enabled); ShowWindow(control.window, show ? SW_SHOWNA : SW_HIDE);
        }
        toolbarOrder = std::move(wanted);
        if (revision != registry.Revision()) {
            panelOrder.clear(); TabCtrl_DeleteAllItems(tabs);
            for (const auto& id : Children("mms.workspace.tabs.extensions")) if (registry.elements.at(id).visible) {
                auto caption = Wide(String(registry.elements.at(id).desc.label)); TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = caption.data();
                SendMessageW(tabs, TCM_INSERTITEMW, panelOrder.size(), reinterpret_cast<LPARAM>(&item)); panelOrder.push_back(id);
            }
            if (std::find(panelOrder.begin(), panelOrder.end(), activePanel) == panelOrder.end()) activePanel = panelOrder.empty() ? "" : panelOrder.front();
            if (!activePanel.empty()) TabCtrl_SetCurSel(tabs, int(std::find(panelOrder.begin(), panelOrder.end(), activePanel) - panelOrder.begin()));
        }
        revision = registry.Revision(); Layout();
    }
    void LayoutPanels(std::map<std::string, UiRegistry::Geometry>& geometry, bool& applied) {
        const int header = MulDiv(30, dpi, 96);
        RECT client{}; GetClientRect(workspace, &client);
        applied = MoveWindow(tabs, 0, 0, client.right, header, TRUE) && applied;
        applied = MoveWindow(body, 0, header, client.right, std::max(0L, client.bottom - header), TRUE) && applied;
        GetClientRect(body, &client); int availableWidth = std::max(0L, client.right), availableHeight = std::max(0L, client.bottom);
        for (int pass = 0; pass < 2; ++pass) {
            SCROLLINFO h{sizeof(h), SIF_RANGE | SIF_PAGE | SIF_POS}; h.nMax = std::max(0, panelWidth - 1); h.nPage = availableWidth; h.nPos = panelScrollX; SetScrollInfo(body, SB_HORZ, &h, TRUE);
            h.nMax = std::max(0, panelHeight - 1); h.nPage = availableHeight; h.nPos = panelScrollY; SetScrollInfo(body, SB_VERT, &h, TRUE);
            GetClientRect(body, &client); availableWidth = std::max(0L, client.right); availableHeight = std::max(0L, client.bottom);
        }
        panelScrollX = std::clamp(panelScrollX, 0, std::max(0, panelWidth - availableWidth)); panelScrollY = std::clamp(panelScrollY, 0, std::max(0, panelHeight - availableHeight));
        bool failed = false;
        for (const auto& id : Children("mms.workspace.tabs.extensions")) {
            bool shown = workspaceHeight > 0 && id == activePanel && registry.elements.at(id).visible;
            auto size = PanelSize(registry.elements.at(id));
            MmsUiRectV1 rect{-panelScrollX, -panelScrollY, uint32_t(std::max(int(std::ceil(size.width * dpi / 96)), availableWidth)), uint32_t(std::max(int(std::ceil(size.height * dpi / 96)), availableHeight))};
            auto result = registry.ShowSurface(id, body, rect, dpi, shown);
            const float scale = 96.0f / dpi;
            geometry[id] = {(x + rect.x) * scale, (height + header + rect.y) * scale, rect.width * scale, rect.height * scale, shown, rect.width > uint32_t(availableWidth) || rect.height > uint32_t(availableHeight)};
            if (result != MMS_OK) {
                if (panelErrors[id] != result && log) log("surface " + id + " returned " + std::to_string(result));
                panelErrors[id] = result;
                if (shown) { failed = true; SetWindowTextW(failure, (L"无法打开插件面板（" + std::to_wstring(result) + L"），切换页签后重试。").c_str()); }
            } else panelErrors.erase(id);
        }
        MoveWindow(failure, 8, 8, std::max(0, availableWidth - 16), std::max(0, availableHeight - 16), TRUE);
        ShowWindow(failure, failed ? SW_SHOWNA : SW_HIDE);
    }
    native_ui::Size PanelSize(const ElementRecord& e) {
        const auto factory = registry.surfaces.find(String(e.desc.surfaceId)); if (factory == registry.surfaces.end()) return {};
        const auto& d = factory->second->desc; const auto& h = e.effectiveHints;
        float minimumWidth = std::max(d.minimumWidth, h.minimum[0]), minimumHeight = std::max(d.minimumHeight, h.minimum[1]);
        float w = e.width ? e.width : h.preferred[0] ? h.preferred[0] : d.preferredWidth;
        float v = e.height ? e.height : h.preferred[1] ? h.preferred[1] : d.preferredHeight ? d.preferredHeight : 160;
        return {std::clamp(w, minimumWidth, std::max(minimumWidth, h.maximum[0] ? h.maximum[0] : 32768)),
                std::clamp(v, minimumHeight, std::max(minimumHeight, h.maximum[1] ? h.maximum[1] : 32768))};
    }
    native_ui::Node LayoutNode(const std::string& id, HDC dc) {
        const auto& e = registry.elements.at(id); const auto& hints = e.effectiveHints;
        native_ui::Node node; node.id = id; MmsCommandStateV1 state{}; node.visible = State(e, state);
        auto& h = node.hints; h.flow = native_ui::Flow(hints.flow); h.columns = hints.columns; h.align = hints.align;
        h.minimum = {hints.minimum[0], hints.minimum[1]}; h.preferred = {hints.preferred[0], hints.preferred[1]}; h.maximum = {hints.maximum[0], hints.maximum[1]};
        h.grow = {hints.grow[0], hints.grow[1]}; h.shrink = {hints.shrink[0], hints.shrink[1]};
        h.keepTogether = (hints.flags & MMS_HINT_KEEP_TOGETHER) != 0; h.preserveHidden = (hints.flags & MMS_HINT_PRESERVE_HIDDEN_SLOT) != 0;
        if (hints.flags & MMS_HINT_EXPLICIT_SPACING) { std::copy_n(hints.gap, 2, h.gap); std::copy_n(hints.padding, 4, h.padding); }
        if (hints.overflow == 1) h.flow = native_ui::Flow::Wrap;
        if (e.width) h.preferred.width = e.width; if (e.height) h.preferred.height = e.height;
        if (e.desc.capabilities & MMS_UI_CONTAINER) for (const auto& child : Children(id)) node.children.push_back(LayoutNode(child, dc));
        else {
            const bool separator = e.desc.kind == MMS_UI_SEPARATOR; auto label = Wide(String(e.desc.label)); SIZE size{};
            if (dc) GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &size);
            node.measuredMinimum = {separator ? 5.0f : 60.0f, 24}; node.measuredPreferred = {separator ? 5.0f : float(size.cx) * 96 / dpi + 20, 24};
        }
        return node;
    }
    static uint32_t WindowDpi(HWND window) noexcept {
        // GetDpiForWindow returns the window's coordinate-space DPI (96 for
        // the current DPI-unaware MMS). Never multiply virtualized pixels by
        // the monitor scale a second time. Win7 falls back to the window DC.
        using DpiFn = UINT(WINAPI*)(HWND);
        static auto getDpi = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        uint32_t value = 96;
        if (getDpi) value = getDpi(window); else { HDC dc = GetDC(window); value = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96; if (dc) ReleaseDC(window, dc); }
        return value < 48 || value > 960 ? 96 : value;
    }
    bool Presentation(const ElementRecord& element, HWND window, uint64_t instance, bool shown,
        std::vector<RefreshPresentation>& out) const {
        RECT client{}; POINT origin{};
        if (!window || !instance || !IsWindow(window) || !GetClientRect(window, &client)) return false;
        SetLastError(ERROR_SUCCESS);
        if (!MapWindowPoints(window, main, &origin, 1) && GetLastError() != ERROR_SUCCESS) return false;
        RefreshPresentation item; item.element = element.handle; item.owner = String(element.desc.ownerPlugin);
        item.instance = instance; item.window = window; item.dpi = WindowDpi(window);
        item.x = origin.x * 96.0f / dpi; item.y = origin.y * 96.0f / dpi;
        item.width = (client.right - client.left) * 96.0f / item.dpi;
        item.height = (client.bottom - client.top) * 96.0f / item.dpi; item.visible = shown;
        out.push_back(std::move(item)); return true;
    }
    bool Presentations(const std::map<std::string, UiRegistry::Geometry>& geometry,
        std::vector<RefreshPresentation>& out) const {
        for (const auto& [id, element] : registry.elements) {
            const auto bounds = geometry.find(id);
            if (bounds == geometry.end() || String(element.desc.ownerPlugin) == "mms") continue;
            if (element.desc.kind == MMS_UI_BUTTON) {
                const auto control = controls.find(id);
                if (control == controls.end() || control->second.element != element.handle || !control->second.window) continue;
                if (!Presentation(element, control->second.window, control->second.instance, bounds->second.visible, out)) return false;
            } else if (element.desc.kind == MMS_UI_PANEL && !panelErrors.count(id)) {
                UiRegistry::SurfacePresentation view;
                if (!registry.ExistingSurface(id, view)) continue;
                if (!Presentation(element, view.window, view.instance, view.visible && bounds->second.visible, out)) return false;
            }
        }
        return true;
    }
    int Layout() {
        dpi = WindowDpi(main);
        const uint64_t uiRevision = registry.Revision(); bool applied = true; std::map<std::string, UiRegistry::Geometry> geometry;
        HDC dc = GetDC(toolbar); HGDIOBJ old = dc ? SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT)) : nullptr;
        auto tree = LayoutNode("mms.toolbar.main.extensions", dc);
        if (dc) { SelectObject(dc, old); ReleaseDC(toolbar, dc); }
        auto flow = native_ui::Arrange(tree, {float(std::max(0, width - GetSystemMetrics(SM_CXVSCROLL))) * 96 / dpi, 0});
        contentHeight = int(std::ceil(flow.content.height * dpi / 96)); height = visible && width > 0 ? std::min(contentHeight, maximum) : 0;
        scroll = std::clamp(scroll, 0, std::max(0, contentHeight - height));
        // A partial move or allocation failure must not leave old rectangles
        // advertised as ready. Only the final successful snapshot resumes them.
        registry.Refresh().SuspendTargets();
        applied = MoveWindow(toolbar, x, 0, std::max(0, width), height, TRUE) && applied;
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS}; info.nMin = 0; info.nMax = std::max(0, contentHeight - 1); info.nPage = height; info.nPos = scroll; SetScrollInfo(toolbar, SB_VERT, &info, TRUE);
        std::vector<native_ui::WindowMove> moves;
        for (const auto& item : flow.items) {
            auto rect = item.rect; auto control = controls.find(item.id);
            int left = int(std::lround(rect.x * dpi / 96)), top = int(std::lround(rect.y * dpi / 96)) - scroll;
            int right = int(std::lround((rect.x + rect.width) * dpi / 96)), bottom = int(std::lround((rect.y + rect.height) * dpi / 96)) - scroll;
            if (control != controls.end()) {
                moves.push_back({control->second.window, {left, top, right, bottom}});
                ShowWindow(control->second.window, item.visible ? SW_SHOWNA : SW_HIDE);
            }
            geometry[item.id] = {(x + left) * 96.0f / dpi, top * 96.0f / dpi, (right - left) * 96.0f / dpi, (bottom - top) * 96.0f / dpi, item.visible && height > 0, item.overflowed || top < 0 || bottom > height};
        }
        applied = native_ui::ApplyWindows(moves) == native_ui::ApplyResult::Applied && applied;
        ShowWindow(toolbar, height ? SW_SHOWNA : SW_HIDE);
        workspaceHeight = panelWidth = panelHeight = 0;
        if (visible && width > 0 && !activePanel.empty() && registry.elements.count(activePanel)) {
            const auto& e = registry.elements.at(activePanel); const auto factory = registry.surfaces.find(String(e.desc.surfaceId));
            if (factory != registry.surfaces.end()) {
                auto size = PanelSize(e);
                panelWidth = int(std::ceil(size.width * dpi / 96));
                panelHeight = int(std::ceil(size.height * dpi / 96));
                workspaceHeight = std::min(std::max(0, maximum - height), panelHeight + MulDiv(30, dpi, 96));
            }
        }
        applied = MoveWindow(workspace, x, height, std::max(0, width), workspaceHeight, TRUE) && applied; ShowWindow(workspace, workspaceHeight ? SW_SHOWNA : SW_HIDE);
        geometry["mms.toolbar.main.extensions"] = {x * 96.0f / dpi, 0, width * 96.0f / dpi, height * 96.0f / dpi, height > 0, contentHeight > height};
        geometry["mms.workspace.tabs.extensions"] = {x * 96.0f / dpi, height * 96.0f / dpi, width * 96.0f / dpi, workspaceHeight * 96.0f / dpi, workspaceHeight > 0, false};
        LayoutPanels(geometry, applied);
        if (applied) {
            std::vector<RefreshPresentation> presentations;
            if (Presentations(geometry, presentations) && registry.PublishGeometry(geometry, uiRevision))
                registry.Refresh().SyncTargets(presentations);
        }
        return height + workspaceHeight;
    }
    bool Command(unsigned id) {
        auto item = dispatch.find(id); if (item == dispatch.end()) return false;
        auto it = registry.elements.find(item->second.first);
        if (it == registry.elements.end() || it->second.handle != item->second.second) return true;
        const auto command = String(it->second.desc.commandId); auto api = registry.Api(client);
        auto result = api.invoke(api.context, Text(command), {}, {}); if (result && log) log("UI command " + command + " returned " + std::to_string(result));
        Refresh(); return true;
    }
};
NativeUi::NativeUi(UiRegistry& r, HWND window, Log log): impl_(std::make_unique<Impl>(r, window, std::move(log))) {}
NativeUi::~NativeUi() = default;
void NativeUi::Refresh() { impl_->Refresh(); }
bool NativeUi::Command(unsigned id) { return impl_->Command(id); }
bool NativeUi::Shortcut(unsigned key, unsigned modifiers, bool viewportFocused) {
    for (const auto& [id, e] : impl_->registry.elements) if (e.key == key && e.modifiers == modifiers && (!e.scope || viewportFocused) && e.visible) return impl_->Command(impl_->Id(id, e));
    return false;
}
int NativeUi::Layout(int x, int width, int maximum, bool visible) { impl_->x = x; impl_->width = width; impl_->maximum = std::max(0, maximum); impl_->visible = visible; return impl_->Layout(); }
HWND NativeUi::Toolbar() const { return impl_->toolbar; }
bool NativeUi::DialogMessage(MSG* message) {
    if (!message || (message->message != WM_KEYDOWN && message->message != WM_CHAR)) return false;
    for (HWND window : {impl_->toolbar, impl_->workspace})
        if (message->hwnd == window || IsChild(window, message->hwnd)) return IsDialogMessageW(window, message) != FALSE;
    return false;
}
}
