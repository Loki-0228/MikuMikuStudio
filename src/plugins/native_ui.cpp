#include "native_ui.hpp"
#include "native_ui_overflow.hpp"
#include "native_property_input.hpp"
#include <native_ui/layout.hpp>
#include <native_ui/win32_host.hpp>
#include <cmath>
#include <set>
#include <commctrl.h>
#include <cstdio>
#include "mikumikustudio/ui_control_layout.hpp"

namespace mikumikustudio::plugins {
struct NativeUi::Impl {
    UiRegistry& registry;
    UiClient client;
    Log log;
    HWND main, toolbar = nullptr, properties = nullptr, workspace = nullptr, tabs = nullptr, body = nullptr, failure = nullptr, tooltip = nullptr, overflowButton = nullptr;
    HMENU root = nullptr, popup = nullptr;
    unsigned nextId = 0xa000;
    uint64_t revision = 0;
    uint64_t layoutContext = 1;
    int x = 0, width = 0, maximum = 120, height = 0, scroll = 0, contentHeight = 0;
    int propertyBottom = 0, propertyMaximum = 0, propertyHeight = 0, propertyScroll = 0, propertyContentHeight = 0;
    bool propertyVisible = false;
    bool visible = true, refreshing = false, presentationPending = false;
    bool refreshLayout = false;
    MmsResult layoutResult = MMS_E_NOT_READY;
    std::function<MmsResult(int)> layoutPublished;
    std::map<std::string, bool> retainedVisibility;
    std::map<std::string, unsigned> ids;
    std::map<unsigned, std::pair<std::string, uint64_t>> dispatch;
    struct Control {
        HWND window = nullptr, parent = nullptr; MmsHandle element = 0; uint64_t instance = 0;
        bool stateVisible = true, layoutVisible = true;
        bool tooltipEnabled = true;
        std::shared_ptr<const ButtonStateContent> content, tooltipContent;
        std::unique_ptr<NativePropertyInput> input;
        std::vector<HWND> inputTipWindows;
        bool inputTips = false;
    };
    std::map<std::string, Control> controls;
    std::vector<std::string> toolbarOrder;
    std::set<std::string> overflowIds;
    HMENU overflowPopup = nullptr;
    std::map<unsigned, std::pair<HMENU, MmsHandle>> overflowMenuItems;
    bool overflowOpen = false, overflowLayout = false;
    unsigned nativeWorkDepth = 0;
    bool closePending = false;
    WPARAM closeWParam = 0; LPARAM closeLParam = 0;
    std::vector<std::string> panelOrder;
    std::string activePanel;
    std::map<std::string, MmsResult> panelErrors;
    uint32_t dpi = 96;
    native_ui::PanelBuffer toolbarBuffer, propertyBuffer, workspaceBuffer, bodyBuffer;
    int workspaceHeight = 0, panelScrollX = 0, panelScrollY = 0, panelWidth = 0, panelHeight = 0;
    Impl(UiRegistry& r, HWND window, Log output): registry(r), client{&r, "mms"}, log(std::move(output)), main(window) {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MMS.PluginAbi.Toolbar.V1"; wc.lpfnWndProc = Window; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("plugin toolbar class");
        toolbar = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"插件工具栏", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 0, 0, main, nullptr, wc.hInstance, this);
        if (!toolbar) throw std::runtime_error("plugin toolbar window");
        wc.lpszClassName = L"MMS.PluginAbi.Workspace.V1";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { DestroyWindow(toolbar); throw std::runtime_error("plugin workspace class"); }
        workspace = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"插件工作区", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, main, nullptr, wc.hInstance, this);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        if (workspace) tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, workspace, nullptr, wc.hInstance, nullptr);
        if (workspace) body = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"插件面板容器", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL, 0, 0, 0, 0, workspace, nullptr, wc.hInstance, this);
        if (body) failure = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 0, 0, body, nullptr, wc.hInstance, nullptr);
        if (!workspace || !tabs || !body || !failure) { if (workspace) DestroyWindow(workspace); DestroyWindow(toolbar); throw std::runtime_error("plugin workspace tabs"); }
        SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        SendMessageW(failure, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, main, nullptr, wc.hInstance, nullptr);
        if (!tooltip) { DestroyWindow(workspace); DestroyWindow(toolbar); throw std::runtime_error("plugin tooltip window"); }
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 480);
        overflowButton = CreateWindowExW(0, L"BUTTON", L"更多", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, toolbar, nullptr, wc.hInstance, nullptr);
        if (!overflowButton) { DestroyWindow(tooltip); DestroyWindow(workspace); DestroyWindow(toolbar); throw std::runtime_error("plugin overflow button"); }
        SendMessageW(overflowButton, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        wc.lpszClassName = L"MMS.PluginAbi.Properties.V1";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            DestroyWindow(tooltip); DestroyWindow(workspace); DestroyWindow(toolbar); throw std::runtime_error("plugin property strip class");
        }
        properties = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"属性区扩展", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
            0, 0, 0, 0, main, nullptr, wc.hInstance, this);
        if (!properties) { DestroyWindow(tooltip); DestroyWindow(workspace); DestroyWindow(toolbar); throw std::runtime_error("plugin property strip"); }
        registry.Refresh().SetLayout([this](MmsHandle element) { return Relayout(element); });
        registry.Refresh().SetValidatedStateContent([this](MmsHandle element, const std::shared_ptr<const ButtonStateContent>& content,
            bool enabled, const std::function<MmsResult()>& revalidate) {
            return ApplyStateContent(element, content, enabled, revalidate);
        });
        registry.Refresh().SetPropertyData([this](MmsHandle element, const std::function<MmsResult()>& revalidate, uint64_t& revision) {
            return ReadPropertyData(element, revalidate, revision);
        });
        if (!SetWindowSubclass(main, Lifetime, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
            registry.Refresh().SetLayout({}); registry.Refresh().SetValidatedStateContent({}); registry.Refresh().SetPropertyData({});
            DestroyWindow(tooltip); DestroyWindow(properties); DestroyWindow(workspace); DestroyWindow(toolbar);
            throw std::runtime_error("plugin popup lifetime hook");
        }
    }
    ~Impl() {
        if (IsWindow(main)) RemoveWindowSubclass(main, Lifetime, reinterpret_cast<UINT_PTR>(this));
        registry.Refresh().SetLayout({});
        registry.Refresh().SetValidatedStateContent({});
        registry.Refresh().SetPropertyData({});
        registry.SuspendGeometry();
        registry.Refresh().SuspendTargets();
        Boundary([&] { registry.Refresh().SyncTargets({}); return MMS_OK; });
        Boundary([&] { registry.DestroyViews(); return MMS_OK; });
        for (auto& [id, control] : controls) DestroyControl(control);
        if (IsWindow(tooltip)) DestroyWindow(tooltip);
        Detach(); if (IsWindow(properties)) DestroyWindow(properties); if (IsWindow(workspace)) DestroyWindow(workspace); if (IsWindow(toolbar)) DestroyWindow(toolbar);
    }
    void ReplayClose() noexcept {
        if (nativeWorkDepth || overflowOpen || !closePending) return;
        if (!IsWindow(main)) { closePending = false; return; }
        if (PostMessageW(main, WM_CLOSE, closeWParam, closeLParam)) closePending = false;
    }
    struct NativeWork {
        Impl& owner;
        explicit NativeWork(Impl& value) noexcept: owner(value) { ++owner.nativeWorkDepth; }
        ~NativeWork() noexcept { --owner.nativeWorkDepth; owner.ReplayClose(); }
        NativeWork(const NativeWork&) = delete;
        NativeWork& operator=(const NativeWork&) = delete;
    };
    bool DeferClose(WPARAM wp, LPARAM lp) noexcept {
        if (!nativeWorkDepth && !overflowOpen) return false;
        if (!closePending) { closePending = true; closeWParam = wp; closeLParam = lp; }
        if (overflowOpen) EndMenu();
        return true;
    }
    static LRESULT CALLBACK Lifetime(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_CLOSE && self->DeferClose(wp, lp)) return 0;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Lifetime, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    void DestroyControl(Control& control) noexcept {
        registry.Refresh().InvalidateTarget(control.element);
        if (control.input) control.input->Cancel();
        const HWND focus = GetFocus();
        if (focus && (focus == control.window || IsChild(control.window, focus))) SetFocus(main);
        if (tooltip && control.window) {
            for (bool rectangle : {false, true}) {
                auto tool = Tool(control, rectangle);
                SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
            for (HWND child : control.inputTipWindows) if (child != control.window) {
                auto tool = Tool(control, false, child);
                SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        if (control.input) { control.input.reset(); control.window = nullptr; }
        if (control.window) DestroyWindow(control.window);
        control = {};
    }
    TOOLINFOW Tool(const Control& control, bool rectangle, HWND child = nullptr) const noexcept {
        // v5 (unmanifested hosts) rejects the modern lpReserved tail. We only
        // use the common prefix through lpszText; keep Unicode in both hosts.
        TOOLINFOW tool{}; tool.cbSize = TTTOOLINFOW_V1_SIZE; tool.hwnd = control.parent ? control.parent : toolbar;
        tool.uId = rectangle ? UINT_PTR(control.element) : reinterpret_cast<UINT_PTR>(child ? child : control.window);
        tool.uFlags = TTF_SUBCLASS | (rectangle ? 0 : TTF_IDISHWND);
        return tool;
    }
    bool AddTips(const Control& control) {
        for (bool rectangle : {false, true}) {
            auto tool = Tool(control, rectangle); tool.lpszText = const_cast<wchar_t*>(L"");
            if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) return false;
        }
        return true;
    }
    MmsResult SyncInputTips(Control& control, const std::function<MmsResult()>& revalidate = {}) {
        if (!control.input || !IsWindow(control.window) || !IsWindow(tooltip)) return MMS_E_STALE;
        const HWND root = control.window;
        const auto check = [&]() -> MmsResult {
            if (!IsWindow(root) || control.window != root || !control.input) return MMS_E_STALE;
            return revalidate ? revalidate() : MMS_OK;
        };
        std::vector<HWND> next{root}, added;
        for (HWND child = GetWindow(root, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) next.push_back(child);
        added.reserve(next.size());
        bool rectangleAdded = false;
        const auto result = Boundary([&]() -> MmsResult {
            for (HWND child : next) if (std::find(control.inputTipWindows.begin(), control.inputTipWindows.end(), child) == control.inputTipWindows.end()) {
                auto tool = Tool(control, false, child); tool.lpszText = LPSTR_TEXTCALLBACKW;
                if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) return MMS_E_FAILED;
                added.push_back(child);
                if (const auto result = check()) return result;
            }
            if (!control.inputTips) {
                auto tool = Tool(control, true); tool.lpszText = LPSTR_TEXTCALLBACKW;
                if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) return MMS_E_FAILED;
                rectangleAdded = true;
                if (const auto result = check()) return result;
            }
            return MMS_OK;
        });
        if (result) {
            for (HWND child : added) { auto tool = Tool(control, false, child); SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool)); }
            if (rectangleAdded) { auto tool = Tool(control, true); SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool)); }
            return result;
        }
        control.inputTipWindows.swap(next); control.inputTips = true;
        for (HWND child : next) if (std::find(control.inputTipWindows.begin(), control.inputTipWindows.end(), child) == control.inputTipWindows.end()) {
            auto tool = Tool(control, false, child); SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            if (const auto result = check()) return result;
        }
        return check();
    }
    bool TipsRect(const Control& control, int logicalVisible = -1) {
        auto tool = Tool(control, true);
        if (!GetWindowRect(control.window, &tool.rect)) return false;
        SetLastError(ERROR_SUCCESS);
        if (!MapWindowPoints(nullptr, tool.hwnd, reinterpret_cast<POINT*>(&tool.rect), 2) && GetLastError()) return false;
        if (logicalVisible == 0 || (logicalVisible < 0 && !(GetWindowLongPtrW(control.window, GWL_STYLE) & WS_VISIBLE))) SetRectEmpty(&tool.rect);
        auto previous = Tool(control, true); wchar_t existing[8194]{}; previous.lpszText = existing;
        if (!SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&previous))) return false;
        if (!EqualRect(&previous.rect, &tool.rect)) SendMessageW(tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        return true;
    }
    static std::wstring Caption(HWND window) {
        const int count = GetWindowTextLengthW(window);
        if (count < 0 || count > 1024 * 1024) throw std::invalid_argument("plugin button caption extent");
        std::wstring text(size_t(count) + 1, L'\0');
        const int copied = GetWindowTextW(window, text.data(), int(text.size()));
        text.resize(size_t(std::max(0, copied))); return text;
    }
    std::wstring EffectiveCaption(const ElementRecord& element, const Control* control) const {
        if (control && control->input) return control->input->DisplayLabel();
        return control && control->content && (control->content->fields & MMS_UI_STATE_LABEL)
            ? control->content->label : Wide(String(element.desc.label));
    }
    MmsResult WriteCaption(HWND window, const std::wstring& caption) {
        const auto before = Caption(window);
        if (before == caption) return MMS_OK;
        // Allocate verification storage before any native mutation.
        std::wstring actual(std::max(before.size(), caption.size()) + 1, L'\0');
        const bool written = SetWindowTextW(window, caption.c_str()) != FALSE;
        if (!IsWindow(window)) return MMS_E_STALE;
        const int count = GetWindowTextLengthW(window);
        const int copied = GetWindowTextW(window, actual.data(), int(actual.size()));
        actual.resize(size_t(std::max(0, copied)));
        if (!written || count != int(caption.size()) || actual != caption) {
            if (count != int(before.size()) || actual != before) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); }
            return MMS_E_FAILED;
        }
        return MMS_OK;
    }
    MmsResult ReadPropertyData(MmsHandle handle, const std::function<MmsResult()>& revalidate, uint64_t& revision) {
        const NativeWork work(*this);
        if (!revalidate) return MMS_E_ARGUMENT;
        if (const auto result = revalidate()) return result;
        const auto found = std::find_if(controls.begin(), controls.end(), [&](const auto& item) { return item.second.element == handle; });
        if (found == controls.end() || !found->second.input || !IsWindow(found->second.window)) return MMS_E_STALE;
        const auto element = registry.elements.find(found->first);
        if (element == registry.elements.end() || element->second.handle != handle || element->second.desc.kind != MMS_UI_INPUT) return MMS_E_STALE;
        return found->second.input->Readback(revalidate, revision);
    }
    MmsResult ApplyStateContent(MmsHandle handle, const std::shared_ptr<const ButtonStateContent>& content, bool enabled,
        const std::function<MmsResult()>& revalidate) {
        const NativeWork work(*this);
        const WindowDpiScope stateDpi(main);
        auto found = std::find_if(controls.begin(), controls.end(), [&](const auto& item) { return item.second.element == handle; });
        if (found == controls.end() || !IsWindow(found->second.window) || !content || !IsWindow(tooltip)) return MMS_E_STALE;
        auto element = registry.elements.find(found->first);
        if (element == registry.elements.end() || element->second.handle != handle ||
            (element->second.desc.kind != MMS_UI_BUTTON && element->second.desc.kind != MMS_UI_INPUT)) return MMS_E_STALE;
        auto& control = found->second; const HWND window = control.window;
        const auto instance = control.instance;
        const auto id = found->first;
        const auto check = [&]() -> MmsResult {
            const auto live = controls.find(id);
            const auto registered = registry.elements.find(id);
            if (live == controls.end() || live->second.element != handle || live->second.instance != instance ||
                live->second.window != window || !IsWindow(window) || registered == registry.elements.end() ||
                registered->second.handle != handle) return MMS_E_STALE;
            return revalidate ? revalidate() : MMS_OK;
        };
        if (const auto result = check()) return result;
        if (control.input) {
            if (element->second.desc.kind != MMS_UI_INPUT || !content->input) return MMS_E_ARGUMENT;
            if (const auto result = SyncInputTips(control, check)) return result;
            // Callback tools read only the published host snapshot. Hide an
            // already displayed tip before releasing its former text owner.
            SendMessageW(tooltip, TTM_POP, 0, 0);
            if (const auto result = check()) return result;
            // The rectangle is a candidate too: a rejected STATIC caption must
            // not hide the old disabled tip or expose a hidden target's tip.
            const HWND tips = tooltip;
            const auto* input = control.input.get();
            const auto previousContent = control.content, previousTips = control.tooltipContent;
            const bool previousVisible = control.stateVisible;
            const auto identity = Tool(control, true);
            const auto sameToolOwner = [&]() noexcept {
                const auto live = controls.find(id);
                return tooltip == tips && IsWindow(tips) && live != controls.end() &&
                    live->second.element == handle && live->second.instance == instance &&
                    live->second.window == window && IsWindow(window) && live->second.input.get() == input &&
                    live->second.content == previousContent && live->second.tooltipContent == previousTips &&
                    live->second.stateVisible == previousVisible;
            };
            const auto readRectangle = [&](RECT& rectangle) {
                if (!sameToolOwner()) return false;
                auto tool = identity; wchar_t text[8194]{}; tool.lpszText = text;
                if (!SendMessageW(tips, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) || !sameToolOwner()) return false;
                if (tool.hwnd != identity.hwnd || tool.uId != identity.uId || (tool.uFlags & TTF_IDISHWND)) return false;
                rectangle = tool.rect; return true;
            };
            RECT before{}, candidate{};
            if (!readRectangle(before)) return MMS_E_FAILED;
            if (const auto result = check()) return result;
            if (!GetWindowRect(window, &candidate)) return MMS_E_FAILED;
            SetLastError(ERROR_SUCCESS);
            if (!MapWindowPoints(nullptr, identity.hwnd, reinterpret_cast<POINT*>(&candidate), 2) && GetLastError()) return MMS_E_FAILED;
            if (!(content->visible && control.layoutVisible && HostVisible(control))) SetRectEmpty(&candidate);
            if (const auto result = check()) return result;
            bool attempted = false;
            const auto result = Boundary([&]() -> MmsResult {
                if (!EqualRect(&before, &candidate)) {
                    auto tool = identity; tool.rect = candidate; attempted = true;
                    SendMessageW(tips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
                }
                if (const auto current = check()) return current;
                RECT actual{};
                if (!readRectangle(actual)) return MMS_E_FAILED;
                if (const auto current = check()) return current;
                if (!EqualRect(&actual, &candidate)) return MMS_E_FAILED;
                return control.input->ApplyStateContent(content->label, content->enabled,
                    content->readOnly, content->visible, check);
            });
            if (result) {
                if (attempted && sameToolOwner()) {
                    RECT actual{};
                    if (readRectangle(actual) && (EqualRect(&actual, &candidate) || EqualRect(&actual, &before))) {
                        // An expired provider cannot restore a formerly visible
                        // region. Clear only this still-owned candidate; never
                        // touch a replacement control or a newer published tip.
                        const auto current = check();
                        if (!sameToolOwner()) return result;
                        const RECT restore = current ? RECT{} : before;
                        if (!EqualRect(&actual, &restore)) {
                            auto tool = identity; tool.rect = restore;
                            SendMessageW(tips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
                        }
                        if (!sameToolOwner()) return result;
                        if (!readRectangle(actual) || !EqualRect(&actual, &restore)) {
                            registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); return MMS_E_FAILED;
                        }
                        // TOOL queries and writes may themselves reenter. If
                        // the source expired during restoration, remove the
                        // just-restored region while its exact owner still
                        // matches, rather than leaving a stale visible tool.
                        const auto after = check();
                        if (!sameToolOwner()) return result;
                        if (after && !IsRectEmpty(&actual)) {
                            auto tool = identity; SetRectEmpty(&tool.rect);
                            SendMessageW(tips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
                            if (!sameToolOwner()) return result;
                            if (!readRectangle(actual) || !IsRectEmpty(&actual)) {
                                registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); return MMS_E_FAILED;
                            }
                        }
                        if (current || after) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); }
                    } else {
                        if (!sameToolOwner()) return result;
                        registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); return MMS_E_FAILED;
                    }
                }
                return result;
            }
            // The view verified its real STATIC before either cache changes.
            // No native call or allocation follows this publication.
            control.content = content; control.tooltipContent = content;
            control.tooltipEnabled = content->enabled && !content->readOnly;
            control.stateVisible = content->visible;
            return MMS_OK;
        }
        // Tool registration and allocations precede caption mutation. Both the
        // child tool and its parent rectangle are needed for disabled controls.
        for (bool rectangle : {false, true}) {
            auto tool = Tool(control, rectangle);
            wchar_t existing[8194]{}; tool.lpszText = existing;
            if (!SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool))) return MMS_E_FAILED;
        }
        auto caption = content->fields & MMS_UI_STATE_LABEL ? content->label : Wide(String(element->second.desc.label));
        if (const auto result = WriteCaption(window, caption)) return result;
        if (const auto result = check()) return result;
        if (!control.content || control.content->fields != content->fields || control.content->label != content->label ||
            control.content->description != content->description || control.content->disabledReason != content->disabledReason)
            control.content = content;
        // Only verified captions reach retained measurement. Keep the tooltip's
        // backing snapshot alive and avoid same-value native tooltip rewrites.
        const auto& text = enabled ? content->description : content->help;
        const auto previous = control.tooltipContent;
        const bool sameTip = previous && previous->description == content->description &&
            previous->disabledReason == content->disabledReason && text == (control.tooltipEnabled ? previous->description : previous->help);
        if (!sameTip) {
            control.tooltipContent = content; control.tooltipEnabled = enabled;
            for (bool rectangle : {false, true}) {
                auto tool = Tool(control, rectangle); tool.lpszText = const_cast<wchar_t*>(text.c_str());
                SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
                if (const auto result = check()) return result;
            }
        }
        if (!IsWindow(window)) return MMS_E_STALE;
        if (!TipsRect(control)) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); return MMS_E_FAILED; }
        if (const auto result = check()) return result;
        return UpdateOverflowItem(found->first);
    }
    static LRESULT CALLBACK Window(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        const NativeWork work(*self);
        if (msg == WM_NOTIFY && lp && reinterpret_cast<NMHDR*>(lp)->hwndFrom == self->tooltip &&
            reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW) {
            auto& info = *reinterpret_cast<NMTTDISPINFOW*>(lp);
            info.lpszText = const_cast<wchar_t*>(L"");
            for (const auto& [id, control] : self->controls) if (control.input &&
                ((info.uFlags & TTF_IDISHWND) ?
                    std::find(control.inputTipWindows.begin(), control.inputTipWindows.end(), reinterpret_cast<HWND>(info.hdr.idFrom)) != control.inputTipWindows.end() :
                    info.hdr.idFrom == UINT_PTR(control.element))) {
                if (control.tooltipContent) info.lpszText = const_cast<wchar_t*>((control.tooltipEnabled ?
                    control.tooltipContent->description : control.tooltipContent->help).c_str());
                break;
            }
            return 0;
        }
        // Overflow uses TPM_RETURNCMD: only OpenOverflow's validated return
        // dispatches. Never treat menu notification as a second button click.
        if ((hwnd == self->toolbar || hwnd == self->properties) && msg == WM_COMMAND && !lp) return 0;
        if (hwnd == self->toolbar && (msg == WM_ENTERMENULOOP || msg == WM_EXITMENULOOP ||
            msg == WM_INITMENU || msg == WM_INITMENUPOPUP || msg == WM_UNINITMENUPOPUP || msg == WM_MENUSELECT)) return 0;
        if (msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT) {
            const HDC dc = reinterpret_cast<HDC>(wp);
            // Our PAINT fills every dirty pixel through PanelBuffer. Avoid a
            // standalone display erase before that transaction, while still
            // providing the same opaque background to child/theme print DCs.
            if (dc && (msg == WM_PRINTCLIENT || GetObjectType(dc) != OBJ_DC || WindowFromDC(dc) != hwnd)) {
                RECT client{}; GetClientRect(hwnd, &client); FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
            }
            return msg == WM_ERASEBKGND ? 1 : 0;
        }
        if (msg == WM_COMMAND && lp && HIWORD(wp) == BN_CLICKED) { Boundary([&] {
            if (reinterpret_cast<HWND>(lp) == self->overflowButton) self->OpenOverflow();
            else self->Command(LOWORD(wp));
            return MMS_OK; }); return 0; }
        if (msg == WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->hwndFrom == self->tabs && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) {
            int index = TabCtrl_GetCurSel(self->tabs); if (index >= 0 && size_t(index) < self->panelOrder.size()) {
                if (self->activePanel != self->panelOrder[index]) ++self->layoutContext;
                self->activePanel = self->panelOrder[index]; Boundary([&] { self->Layout(); return self->registry.FocusSurface(self->activePanel); });
            } return 0;
        }
        if (hwnd == self->body && (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL)) {
            const bool horizontal = msg == WM_HSCROLL; int& position = horizontal ? self->panelScrollX : self->panelScrollY;
            SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, horizontal ? SB_HORZ : SB_VERT, &info); int to = position;
            if (msg == WM_MOUSEWHEEL) to -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 28;
            else switch (LOWORD(wp)) { case SB_LINEUP: to -= 28; break; case SB_LINEDOWN: to += 28; break; case SB_PAGEUP: to -= info.nPage; break; case SB_PAGEDOWN: to += info.nPage; break; case SB_THUMBTRACK: to = info.nTrackPos; break; default: break; }
            const int next = std::clamp(to, 0, std::max(0, info.nMax - int(info.nPage) + 1));
            if (position != next) ++self->layoutContext;
            position = next; Boundary([&] { self->Layout(); return MMS_OK; }); return 0;
        }
        if ((hwnd == self->toolbar || hwnd == self->properties) && (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL)) {
            SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &info);
            const bool property = hwnd == self->properties;
            int& position = property ? self->propertyScroll : self->scroll;
            const int extent = property ? self->propertyHeight : self->height;
            const int content = property ? self->propertyContentHeight : self->contentHeight;
            int to = position;
            if (msg == WM_MOUSEWHEEL) to -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 28;
            else switch (LOWORD(wp)) { case SB_LINEUP: to -= 28; break; case SB_LINEDOWN: to += 28; break; case SB_PAGEUP: to -= extent; break; case SB_PAGEDOWN: to += extent; break; case SB_THUMBTRACK: to = info.nTrackPos; break; default: break; }
            const int next = std::clamp(to, 0, std::max(0, content - extent));
            if (position != next) ++self->layoutContext;
            position = next;
            Boundary([&] { self->Layout(); return MMS_OK; }); return 0;
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); RECT rect{}; GetClientRect(hwnd, &rect);
            auto& buffer = hwnd == self->toolbar ? self->toolbarBuffer : hwnd == self->properties ? self->propertyBuffer : hwnd == self->body ? self->bodyBuffer : self->workspaceBuffer;
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
        if (e.desc.kind == MMS_UI_INPUT) {
            const auto control = controls.find(String(e.desc.id));
            const auto content = control != controls.end() ? control->second.content : nullptr;
            out.enabled = !content || !content->input || content->enabled;
            out.visible = registry.Refresh().CachedStateVisible(e.handle, !content || !content->input || content->visible);
            return out.visible != 0;
        }
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
    HWND ControlHost(const ElementRecord& element) const {
        auto parent = String(element.desc.parentId);
        for (size_t depth = 0; depth <= registry.elements.size(); ++depth) {
            if (parent == "mms.toolbar.main.extensions") return toolbar;
            if (parent == "mms.properties.main.extensions") return properties;
            const auto found = registry.elements.find(parent);
            if (found == registry.elements.end() || found->second.desc.kind != MMS_UI_GROUP) return nullptr;
            parent = String(found->second.desc.parentId);
        }
        return nullptr;
    }
    bool HostVisible(const Control& control) const noexcept {
        return visible && (control.parent == properties ? propertyVisible && propertyHeight > 0 : height > 0);
    }
    bool AvailableAncestors(const ElementRecord& element) const {
        auto parent = String(element.desc.parentId);
        for (unsigned depth = 0; depth <= 64; ++depth) {
            if (NativeControlAnchor(parent)) return true;
            const auto found = registry.elements.find(parent);
            if (found == registry.elements.end() || !found->second.visible) return false;
            parent = String(found->second.desc.parentId);
        }
        return false;
    }
    struct HostLayoutPaint {
        struct Snapshot { HWND window, parent; RECT bounds; bool visible, force = false; };
        Impl& owner;
        std::vector<Snapshot> snapshots;
        std::vector<ui::LayoutChange> changes;
        std::vector<ui::layout_detail::PendingPaint> guards;
        bool deferred;
        explicit HostLayoutPaint(Impl& value): owner(value), guards(value.controls.size() + 7), deferred(ui::LayoutBatch::IsCollecting()) {
            const auto count = owner.controls.size() + 7;
            snapshots.reserve(count); changes.reserve(count * 2);
            ui::ReserveLayoutChanges(count * 2);
            for (HWND window : {owner.toolbar, owner.properties, owner.workspace, owner.tabs, owner.body, owner.failure, owner.overflowButton}) Add(window);
            for (const auto& [id, control] : owner.controls) Add(control.window);
            for (size_t i = 0; i < snapshots.size(); ++i) if (!guards[i].Attach(snapshots[i].window))
                throw std::runtime_error("plugin layout paint guard");
        }
        void Add(HWND window) {
            if (!IsWindow(window)) return;
            const HWND parent = GetParent(window);
            snapshots.push_back({window, parent, ui::layout_detail::ActualRect(parent, window),
                (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0});
        }
        Snapshot* Find(HWND window) noexcept {
            for (auto& item : snapshots) if (item.window == window) return &item;
            return nullptr;
        }
        bool Move(HWND window, int x, int y, int width, int height, bool nativeConstraints = false) {
            auto* snapshot = Find(window);
            if (!snapshot || !IsWindow(window) || GetParent(window) != snapshot->parent) {
                std::fprintf(stderr, "PLUGIN_LAYOUT_MOVE target=%p id=%d invalid_snapshot=1\n", window, GetDlgCtrlID(window)); return false;
            }
            const WindowDpiScope dpi(snapshot->parent);
            const RECT before = ui::layout_detail::ActualRect(snapshot->parent, window);
            const RECT requested{x, y, x + width, y + height};
            if (EqualRect(&before, &requested) && !nativeConstraints) return true;
            snapshot->force = true;
            const bool moved = SetWindowPos(window, nullptr, x, y, width, height,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS) != FALSE;
            const RECT after = ui::layout_detail::ActualRect(snapshot->parent, window);
            const bool matched = moved && IsWindow(window) && GetParent(window) == snapshot->parent &&
                (nativeConstraints || EqualRect(&after, &requested));
            if (!matched) std::fprintf(stderr, "PLUGIN_LAYOUT_MOVE target=%p id=%d moved=%d request=%ld,%ld,%ld,%ld actual=%ld,%ld,%ld,%ld error=%lu\n",
                window, GetDlgCtrlID(window), int(moved), requested.left, requested.top, requested.right, requested.bottom,
                after.left, after.top, after.right, after.bottom, GetLastError());
            return matched;
        }
        bool Visible(HWND window, bool shown) {
            auto* snapshot = Find(window); if (!snapshot || !IsWindow(window)) return false;
            if (bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == shown) return true;
            snapshot->force = true;
            const bool changed = SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)) != FALSE;
            const bool matched = changed && IsWindow(window) && bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) == shown;
            if (!matched) std::fprintf(stderr, "PLUGIN_LAYOUT_VISIBLE target=%p id=%d changed=%d request=%d actual=%d error=%lu\n",
                window, GetDlgCtrlID(window), int(changed), int(shown), int(bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)), GetLastError());
            return matched;
        }
        bool Scroll(HWND window, int bar, SCROLLINFO requested) {
            auto* snapshot = Find(window); if (!snapshot || !IsWindow(window)) return false;
            // USER32 clamps page/position; compare that effective state so a
            // repeated layout does not redraw the same nonclient scrollbar.
            const auto range = uint64_t(int64_t(requested.nMax) - requested.nMin) + 1;
            requested.nPage = UINT(std::min<uint64_t>(requested.nPage, range));
            const int last = requested.nMax - int(requested.nPage ? requested.nPage - 1 : 0);
            requested.nPos = std::clamp(requested.nPos, requested.nMin, last);
            const auto same = [&](const SCROLLINFO& current) {
                return current.nMin == requested.nMin && current.nMax == requested.nMax &&
                    current.nPage == requested.nPage && current.nPos == requested.nPos;
            };
            const auto absentEmptyBar = [&](DWORD error) {
                // USER32 may discard the standard scrollbar state entirely
                // after an empty range hides it. That exact state has nothing
                // to scroll; do not turn it into a failed geometry publication
                // or recreate the same absent bar on every empty layout.
                const LONG_PTR style = bar == SB_VERT ? WS_VSCROLL : bar == SB_HORZ ? WS_HSCROLL : 0;
                return error == ERROR_NO_SCROLLBARS && style && IsWindow(window) &&
                    !(GetWindowLongPtrW(window, GWL_STYLE) & style) && requested.nMin == 0 && requested.nMax == 0 &&
                    requested.nPos == 0 && requested.nPage <= 1;
            };
            SCROLLINFO current{sizeof(current), SIF_RANGE | SIF_PAGE | SIF_POS};
            SetLastError(ERROR_SUCCESS);
            const bool readBefore = GetScrollInfo(window, bar, &current) != FALSE;
            const DWORD beforeError = readBefore ? ERROR_SUCCESS : GetLastError();
            if ((readBefore && same(current)) || (!readBefore && absentEmptyBar(beforeError))) return true;
            snapshot->force = true;
            SetScrollInfo(window, bar, &requested, FALSE);
            current = {sizeof(current), SIF_RANGE | SIF_PAGE | SIF_POS};
            SetLastError(ERROR_SUCCESS);
            const bool queried = GetScrollInfo(window, bar, &current) != FALSE;
            const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
            const bool matched = (queried && same(current)) || (!queried && absentEmptyBar(error));
            if (!matched) std::fprintf(stderr, "PLUGIN_LAYOUT_SCROLL target=%p bar=%d queried=%d request=%d,%d,%u,%d actual=%d,%d,%u,%d error=%lu\n",
                window, bar, int(queried), requested.nMin, requested.nMax, requested.nPage, requested.nPos,
                current.nMin, current.nMax, current.nPage, current.nPos, error);
            return matched;
        }
        bool Queue(std::vector<native_ui::WindowMove>& moves, HWND window, RECT requested) {
            auto* snapshot = Find(window);
            if (!snapshot || !IsWindow(window) || GetParent(window) != snapshot->parent) return false;
            const RECT current = ui::layout_detail::ActualRect(snapshot->parent, window);
            if (EqualRect(&current, &requested)) return true;
            snapshot->force = true;
            moves.push_back({window, requested}); return true;
        }
        void Finish() {
            for (auto& guard : guards) guard.Release();
            for (const auto& saved : snapshots) {
                const bool alive = IsWindow(saved.window) != FALSE;
                const HWND parent = alive ? GetParent(saved.window) : nullptr;
                const RECT after = alive ? ui::layout_detail::ActualRect(parent, saved.window) : RECT{};
                const bool visible = alive && (GetWindowLongPtrW(saved.window, GWL_STYLE) & WS_VISIBLE);
                if (parent != saved.parent) {
                    changes.push_back({nullptr, saved.parent, saved.bounds, {}});
                    if (alive) changes.push_back({saved.window, parent, {}, after});
                } else if (saved.force || saved.visible != visible || !EqualRect(&saved.bounds, &after))
                    changes.push_back({saved.window, parent, saved.bounds, after});
            }
            // Deferred changes are painted by the outer layout batch. Only an
            // independent (or unrecordable) transaction invalidates here, so
            // the outer batch keeps one repaint and owns the ready state.
            for (const auto& change : changes) {
                bool recorded = true;
                if (deferred) {
                    recorded = ui::RecordLayoutChange(change);
                    if (!recorded) {
                        std::fprintf(stderr, "PLUGIN_LAYOUT_RECORD target=%p parent=%p failed=1\n", change.window, change.parent);
                        owner.layoutResult = MMS_E_FAILED; owner.registry.SuspendGeometry(); owner.registry.Refresh().SuspendTargets();
                    }
                }
                if (deferred && recorded) continue;
                if (IsWindow(change.parent)) {
                    const WindowDpiScope dpi(change.parent);
                    RECT damage{}; UnionRect(&damage, &change.before, &change.after);
                    RedrawWindow(change.parent, &damage, nullptr, RDW_INVALIDATE | RDW_NOCHILDREN);
                }
                if (IsWindow(change.window)) {
                    const WindowDpiScope dpi(change.window);
                    RedrawWindow(change.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
                }
            }
            if (!deferred && !changes.empty()) ui::RepaintLayout(owner.main, changes, {});
        }
        ~HostLayoutPaint() noexcept {
            try { Finish(); }
            catch (...) { std::fprintf(stderr, "PLUGIN_LAYOUT_FINISH exception=1\n");
                owner.layoutResult = MMS_E_FAILED; owner.registry.SuspendGeometry(); owner.registry.Refresh().SuspendTargets(); }
        }
    };
    static void PruneOverflow(HMENU menu, unsigned depth = 0) {
        if (!menu || depth > 64) return;
        for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i) if (const HMENU sub = GetSubMenu(menu, i)) {
            PruneOverflow(sub, depth + 1);
            if (!GetMenuItemCount(sub)) DeleteMenu(menu, i, MF_BYPOSITION);
        }
        bool separator = true;
        for (int i = 0; i < GetMenuItemCount(menu);) {
            MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_FTYPE;
            if (!GetMenuItemInfoW(menu, i, TRUE, &item)) break;
            const bool current = (item.fType & MFT_SEPARATOR) != 0;
            if (current && (separator || i == GetMenuItemCount(menu) - 1)) DeleteMenu(menu, i, MF_BYPOSITION);
            else { separator = current; ++i; }
        }
    }
    MmsResult UpdateOverflowItem(const std::string& id) {
        const auto commandId = ids.find(id);
        if (commandId == ids.end()) return MMS_OK;
        const auto menu = overflowMenuItems.find(commandId->second);
        if (menu == overflowMenuItems.end()) return MMS_OK;
        const auto element = registry.elements.find(id);
        const auto control = controls.find(id);
        const bool current = element != registry.elements.end() && element->second.handle == menu->second.second &&
            control != controls.end() && control->second.element == element->second.handle && overflowIds.count(id) &&
            element->second.visible && AvailableAncestors(element->second);
        const bool shown = current && registry.Refresh().CachedStateVisible(element->second.handle, control->second.stateVisible);
        if (!shown) {
            const bool removed = DeleteMenu(menu->second.first, commandId->second, MF_BYCOMMAND) != FALSE;
            overflowMenuItems.erase(menu); PruneOverflow(overflowPopup); return removed ? MMS_OK : MMS_E_FAILED;
        }
        auto caption = EffectiveCaption(element->second, &control->second);
        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_STATE | MIIM_STRING;
        item.fState = (IsWindowEnabled(control->second.window) ? MFS_ENABLED : MFS_DISABLED) |
            (SendMessageW(control->second.window, BM_GETCHECK, 0, 0) == BST_CHECKED ? MFS_CHECKED : MFS_UNCHECKED);
        item.dwTypeData = caption.data();
        return SetMenuItemInfoW(menu->second.first, commandId->second, FALSE, &item) ? MMS_OK : MMS_E_FAILED;
    }
    void OverflowMenu(HMENU menu, const std::string& parent, unsigned depth = 0) {
        if (depth > 64) throw std::invalid_argument("plugin overflow menu depth");
        bool emitted = false, separator = false;
        for (const auto& id : Children(parent)) {
            const auto& e = registry.elements.at(id);
            if (!e.visible) continue;
            if (e.desc.kind == MMS_UI_GROUP) {
                struct OwnedMenu {
                    HMENU handle = CreatePopupMenu();
                    ~OwnedMenu() { if (handle) DestroyMenu(handle); }
                    void Release() noexcept { handle = nullptr; }
                } sub;
                if (!sub.handle) throw std::runtime_error("plugin overflow submenu");
                OverflowMenu(sub.handle, id, depth + 1);
                if (!GetMenuItemCount(sub.handle)) continue;
                auto caption = Wide(String(e.desc.label));
                if (separator && emitted && !AppendMenuW(menu, MF_SEPARATOR, 0, nullptr)) throw std::runtime_error("plugin overflow separator");
                if (!AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(sub.handle), caption.c_str()))
                    throw std::runtime_error("plugin overflow group");
                sub.Release();
                emitted = true; separator = false; continue;
            }
            if (!overflowIds.count(id)) continue;
            if (e.desc.kind == MMS_UI_SEPARATOR) { separator = emitted; continue; }
            if (e.desc.kind != MMS_UI_BUTTON) continue;
            MmsCommandStateV1 state{}; if (!State(e, state)) continue;
            const auto control = controls.find(id);
            if (control == controls.end() || control->second.element != e.handle) continue;
            const auto commandId = Id(id, e); auto caption = EffectiveCaption(e, &control->second);
            if (separator && emitted && !AppendMenuW(menu, MF_SEPARATOR, 0, nullptr)) throw std::runtime_error("plugin overflow separator");
            if (!AppendMenuW(menu, MF_STRING | (state.enabled ? MF_ENABLED : MF_GRAYED) |
                (state.checked ? MF_CHECKED : MF_UNCHECKED), commandId, caption.c_str())) throw std::runtime_error("plugin overflow command");
            overflowMenuItems.emplace(commandId, std::make_pair(menu, e.handle));
            emitted = true; separator = false;
        }
    }
    void OpenOverflow() {
        const NativeWork work(*this);
        if (overflowOpen || overflowLayout || registry.InCallback() || !IsWindowVisible(overflowButton) || overflowIds.empty()) return;
        overflowOpen = true; HMENU menu = CreatePopupMenu(); overflowPopup = menu;
        struct Guard { Impl& self; HMENU menu; ~Guard() { self.overflowMenuItems.clear(); self.overflowPopup = nullptr;
            if (menu) DestroyMenu(menu); self.overflowOpen = false;
        } } guard{*this, menu};
        if (!menu) throw std::runtime_error("plugin overflow menu");
        OverflowMenu(menu, "mms.toolbar.main.extensions");
        if (overflowMenuItems.empty() || closePending || !IsWindow(main)) return;
        RECT anchor{}; if (!GetWindowRect(overflowButton, &anchor)) return;
        // Own the native menu loop here. Sending menu notifications to main
        // would run its unrelated recent-file/antialias menu handlers and its
        // WM_INITMENU refresh path while this popup snapshot is being tracked.
        const unsigned selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
            anchor.left, anchor.bottom, toolbar, nullptr);
        if (closePending || !IsWindow(main)) return;
        const auto snapshot = overflowMenuItems.find(selected);
        const auto dispatchItem = dispatch.find(selected);
        if (snapshot == overflowMenuItems.end() || dispatchItem == dispatch.end() || dispatchItem->second.second != snapshot->second.second) return;
        const auto element = registry.elements.find(dispatchItem->second.first);
        if (element == registry.elements.end() || element->second.handle != snapshot->second.second || !element->second.visible ||
            !AvailableAncestors(element->second) || !overflowIds.count(element->first)) return;
        // Invoke rechecks command state and owner lifetime at the ordinary gate.
        Command(selected);
    }
    void Refresh() {
        const NativeWork work(*this);
        if (registry.InCallback()) { DeferPresentation(); return; }
        if (refreshing || !IsWindow(main)) return;
        presentationPending = false;
        refreshing = true; struct Guard { bool& v; ~Guard() { v = false; } } guard{refreshing};
        for (auto it = retainedVisibility.begin(); it != retainedVisibility.end();)
            if (registry.elements.find(it->first) == registry.elements.end()) it = retainedVisibility.erase(it); else ++it;
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
        std::vector<std::string> wanted; Gather("mms.toolbar.main.extensions", wanted); Gather("mms.properties.main.extensions", wanted);
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
            const auto& e = registry.elements.at(id); MmsCommandStateV1 state{}; bool show = State(e, state);
            const auto commandId = Id(id, e);
            auto& control = controls[id];
            const bool input = e.desc.kind == MMS_UI_INPUT;
            const HWND parent = ControlHost(e);
            if (!parent) throw std::runtime_error("unavailable plugin control mount");
            if (control.window && (input != bool(control.input) || control.parent != parent)) DestroyControl(control);
            control.parent = parent;
            if (input) {
                if (!control.input) {
                    control.element = e.handle; control.instance = registry.NewPresentationInstance();
                    control.input = std::make_unique<NativePropertyInput>(registry, id, parent, commandId,
                        [this](bool enter) { if (enter) ++nativeWorkDepth; else { --nativeWorkDepth; ReplayClose(); } }, log);
                    control.window = control.input->Window();
                }
                control.input->Refresh(dpi);
                if (const auto result = SyncInputTips(control)) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); throw std::runtime_error("cannot create plugin input tips"); }
                control.input->LayoutVisible(show && HostVisible(control) && !overflowIds.count(id));
                if (bool(IsWindowEnabled(control.window)) != bool(state.enabled)) EnableWindow(control.window, state.enabled);
                ShowWindow(control.window, show && !overflowIds.count(id) ? SW_SHOWNA : SW_HIDE);
                control.stateVisible = state.visible != 0;
                continue;
            }
            auto caption = EffectiveCaption(e, &control);
            if (!control.window) {
                const bool separator = e.desc.kind == MMS_UI_SEPARATOR;
                control.element = e.handle; control.instance = registry.NewPresentationInstance();
                control.window = CreateWindowExW(0, separator ? L"STATIC" : L"BUTTON", caption.c_str(), WS_CHILD | (separator ? SS_ETCHEDVERT : WS_TABSTOP | BS_CHECKBOX | BS_PUSHLIKE), 0, 0, 0, 0,
                    parent, reinterpret_cast<HMENU>(UINT_PTR(commandId)), GetModuleHandleW(nullptr), nullptr);
                if (!control.window) throw std::runtime_error("cannot create plugin button");
                SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
                if (!separator && !AddTips(control)) { DestroyControl(control); throw std::runtime_error("cannot create plugin button tips"); }
            }
            if (Caption(control.window) != caption) SetWindowTextW(control.window, caption.c_str());
            EnableWindow(control.window, state.enabled); ShowWindow(control.window, show && !overflowIds.count(id) ? SW_SHOWNA : SW_HIDE);
            if (control.tooltipContent) {
                const auto& text = state.enabled ? control.tooltipContent->description : control.tooltipContent->help;
                const auto& previous = control.tooltipEnabled ? control.tooltipContent->description : control.tooltipContent->help;
                if (text != previous) for (bool rectangle : {false, true}) {
                    auto tool = Tool(control, rectangle); tool.lpszText = const_cast<wchar_t*>(text.c_str());
                    SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
                }
                control.tooltipEnabled = state.enabled != 0;
            }
            if (e.desc.kind == MMS_UI_BUTTON) SendMessageW(control.window, BM_SETCHECK, state.checked ? BST_CHECKED : BST_UNCHECKED, 0);
            control.stateVisible = state.visible != 0;
        }
        toolbarOrder = std::move(wanted);
        // TrackPopupMenu runs a nested native loop. A plugin can retire while
        // it is open; stale menu choices must disappear before dispatch.
        std::vector<std::string> menuIds;
        for (auto item = overflowMenuItems.begin(); item != overflowMenuItems.end();) {
            const auto dispatched = dispatch.find(item->first);
            if (dispatched == dispatch.end()) {
                DeleteMenu(item->second.first, item->first, MF_BYCOMMAND); item = overflowMenuItems.erase(item);
            } else { menuIds.push_back(dispatched->second.first); ++item; }
        }
        for (const auto& id : menuIds) if (const auto result = UpdateOverflowItem(id))
            if (log) log("overflow state returned " + std::to_string(result));
        PruneOverflow(overflowPopup);
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
    void LayoutPanels(std::map<std::string, UiRegistry::Geometry>& geometry, bool& applied, HostLayoutPaint& paint) {
        const int header = MulDiv(30, dpi, 96);
        RECT client{}; GetClientRect(workspace, &client);
        applied = paint.Move(tabs, 0, 0, client.right, header) && applied;
        applied = paint.Move(body, 0, header, client.right, std::max(0L, client.bottom - header)) && applied;
        GetClientRect(body, &client); int availableWidth = std::max(0L, client.right), availableHeight = std::max(0L, client.bottom);
        for (int pass = 0; pass < 2; ++pass) {
            SCROLLINFO h{sizeof(h), SIF_RANGE | SIF_PAGE | SIF_POS}; h.nMax = std::max(0, panelWidth - 1); h.nPage = availableWidth; h.nPos = panelScrollX;
            applied = paint.Scroll(body, SB_HORZ, h) && applied;
            h.nMax = std::max(0, panelHeight - 1); h.nPage = availableHeight; h.nPos = panelScrollY;
            applied = paint.Scroll(body, SB_VERT, h) && applied;
            GetClientRect(body, &client); availableWidth = std::max(0L, client.right); availableHeight = std::max(0L, client.bottom);
        }
        panelScrollX = std::clamp(panelScrollX, 0, std::max(0, panelWidth - availableWidth)); panelScrollY = std::clamp(panelScrollY, 0, std::max(0, panelHeight - availableHeight));
        bool failed = false;
        for (const auto& id : Children("mms.workspace.tabs.extensions")) {
            bool shown = workspaceHeight > 0 && id == activePanel && registry.elements.at(id).visible;
            auto size = PanelSize(registry.elements.at(id));
            MmsUiRectV1 rect{-panelScrollX, -panelScrollY, uint32_t(std::max(int(std::ceil(size.width * dpi / 96)), availableWidth)), uint32_t(std::max(int(std::ceil(size.height * dpi / 96)), availableHeight))};
            MmsResult result = MMS_OK;
            UiRegistry::SurfacePresentation existing;
            const bool attempted = !refreshLayout || registry.ExistingSurface(id, existing);
            if (attempted)
                result = registry.ShowSurface(id, body, rect, dpi, shown);
            else if (const auto error = panelErrors.find(id); error != panelErrors.end()) result = error->second;
            const float scale = 96.0f / dpi;
            geometry[id] = {(x + rect.x) * scale, (height + header + rect.y) * scale, rect.width * scale, rect.height * scale, shown, rect.width > uint32_t(availableWidth) || rect.height > uint32_t(availableHeight)};
            if (result != MMS_OK) {
                if (refreshLayout && attempted) { applied = false; layoutResult = result; }
                if (panelErrors[id] != result && log) log("surface " + id + " returned " + std::to_string(result));
                panelErrors[id] = result;
                if (shown) { failed = true; SetWindowTextW(failure, (L"无法打开插件面板（" + std::to_wstring(result) + L"），切换页签后重试。").c_str()); }
            } else if (attempted) panelErrors.erase(id);
        }
        applied = paint.Move(failure, 8, 8, std::max(0, availableWidth - 16), std::max(0, availableHeight - 16)) && applied;
        applied = paint.Visible(failure, failed) && applied;
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
    native_ui::Node LayoutNode(const std::string& id, HDC dc, bool ancestorsVisible = true) {
        const auto& e = registry.elements.at(id); const auto& hints = e.effectiveHints;
        native_ui::Node node; node.id = id; MmsCommandStateV1 state{};
        if (refreshLayout) {
            const auto cached = retainedVisibility.find(id);
            if (cached == retainedVisibility.end()) throw std::invalid_argument("unpublished retained layout");
            state.visible = registry.Refresh().CachedStateVisible(e.handle, cached->second) ? 1 : 0;
            node.visible = e.visible && state.visible;
        } else {
            node.visible = State(e, state);
            retainedVisibility[id] = state.visible != 0;
        }
        if (auto control = controls.find(id); control != controls.end()) {
            control->second.stateVisible = state.visible != 0;
            control->second.layoutVisible = ancestorsVisible && e.visible;
        }
        auto& h = node.hints; h.flow = native_ui::Flow(hints.flow); h.columns = hints.columns; h.align = hints.align;
        h.minimum = {hints.minimum[0], hints.minimum[1]}; h.preferred = {hints.preferred[0], hints.preferred[1]}; h.maximum = {hints.maximum[0], hints.maximum[1]};
        h.grow = {hints.grow[0], hints.grow[1]}; h.shrink = {hints.shrink[0], hints.shrink[1]};
        h.keepTogether = (hints.flags & MMS_HINT_KEEP_TOGETHER) != 0; h.preserveHidden = (hints.flags & MMS_HINT_PRESERVE_HIDDEN_SLOT) != 0;
        if (hints.flags & MMS_HINT_EXPLICIT_SPACING) { std::copy_n(hints.gap, 2, h.gap); std::copy_n(hints.padding, 4, h.padding); }
        if (hints.overflow == 1) h.flow = native_ui::Flow::Wrap;
        if (e.width) h.preferred.width = e.width; if (e.height) h.preferred.height = e.height;
        if (e.desc.capabilities & MMS_UI_CONTAINER) for (const auto& child : Children(id)) node.children.push_back(LayoutNode(child, dc, ancestorsVisible && node.visible));
        else {
            const bool separator = e.desc.kind == MMS_UI_SEPARATOR;
            const auto control = controls.find(id);
            auto label = EffectiveCaption(e, control == controls.end() ? nullptr : &control->second); SIZE size{};
            if (dc) GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &size);
            node.measuredMinimum = {separator ? 5.0f : 60.0f, 24}; node.measuredPreferred = {separator ? 5.0f : float(size.cx) * 96 / dpi + 20, 24};
            if (e.desc.kind == MMS_UI_INPUT) {
                const auto property = registry.properties.find(String(e.desc.propertyId));
                const auto fields = property == registry.properties.end() ? 1u : std::max(1u, property->second->desc.info.components);
                node.measuredMinimum = {48.0f + fields * 32.0f, 28};
                node.measuredPreferred = {std::max(220.0f, float(size.cx) * 96 / dpi + 16 + fields * 76.0f), 28};
            }
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
        item.coordinateDpi = dpi; item.coordinateWindow = main;
        item.pixelX = origin.x; item.pixelY = origin.y; item.pixelOrigin = true;
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
            if (element.desc.kind == MMS_UI_BUTTON || element.desc.kind == MMS_UI_SEPARATOR || element.desc.kind == MMS_UI_INPUT) {
                const auto control = controls.find(id);
                if (control == controls.end() || control->second.element != element.handle || !control->second.window) continue;
                if (!Presentation(element, control->second.window, control->second.instance, bounds->second.visible, out)) return false;
                out.back().stateRefresh = element.desc.kind == MMS_UI_BUTTON; out.back().stateVisible = control->second.stateVisible;
                if (element.desc.kind == MMS_UI_INPUT && control->second.input) {
                    const auto property = registry.properties.find(String(element.desc.propertyId));
                    if (property != registry.properties.end()) {
                        out.back().stateRefresh = true;
                        out.back().propertyDataRefresh = true; out.back().propertyRecord = property->second;
                    }
                }
                out.back().stateVisibility = (element.effectiveHints.flags & MMS_HINT_PRESERVE_HIDDEN_SLOT) != 0;
                out.back().layoutVisible = control->second.layoutVisible && HostVisible(control->second);
                out.back().layoutRefresh = true;
            } else if (element.desc.kind == MMS_UI_GROUP) {
                // A retained group is an addressable registration, not another
                // HWND. Its SUBTREE expands to the existing descendant views.
                RefreshPresentation item; item.element = element.handle;
                item.owner = String(element.desc.ownerPlugin); item.instance = element.handle;
                item.window = ControlHost(element); item.dpi = dpi; item.container = true;
                if (!item.window) return false;
                item.coordinateDpi = dpi; item.coordinateWindow = main;
                item.pixelX = LONG(std::lround(double(bounds->second.x) * dpi / 96));
                item.pixelY = LONG(std::lround(double(bounds->second.y) * dpi / 96)); item.pixelOrigin = true;
                item.x = bounds->second.x; item.y = bounds->second.y;
                item.width = bounds->second.width; item.height = bounds->second.height;
                item.visible = bounds->second.visible; item.layoutRefresh = true;
                out.push_back(std::move(item));
            } else if (element.desc.kind == MMS_UI_PANEL && !panelErrors.count(id)) {
                UiRegistry::SurfacePresentation view;
                if (!registry.ExistingSurface(id, view)) continue;
                if (!Presentation(element, view.window, view.instance, view.visible && bounds->second.visible, out)) return false;
                out.back().layoutRefresh = true;
            }
        }
        return true;
    }
    int Layout(bool fromRefresh = false) {
        // Declared before every layout guard: Finish can synchronously paint a
        // plugin HWND and pump WM_CLOSE while this adapter still owns a stack.
        const NativeWork work(*this);
        const WindowDpiScope layoutDpi(main);
        const auto context = layoutContext;
        const int priorHeight = height + workspaceHeight;
        const int priorPropertyHeight = propertyHeight;
        layoutResult = MMS_E_NOT_READY;
        if (registry.InCallback() && !fromRefresh) { DeferPresentation(); return height + workspaceHeight; }
        if (overflowLayout) { layoutResult = MMS_E_NOT_READY; return height + workspaceHeight; }
        overflowLayout = true;
        struct OverflowLayoutGuard { bool& value; ~OverflowLayoutGuard() { value = false; } } overflowGuard{overflowLayout};
        layoutResult = MMS_E_FAILED;
        presentationPending = false;
        dpi = WindowDpi(main);
        const uint64_t uiRevision = registry.Revision(); bool applied = true; std::map<std::string, UiRegistry::Geometry> geometry;
        const auto measureTree = [&](const char* root, HWND host) {
            struct MeasureDc {
                HWND window; HDC dc; HGDIOBJ old;
                explicit MeasureDc(HWND value): window(value), dc(GetDC(value)), old(dc ? SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT)) : nullptr) {}
                ~MeasureDc() { if (dc) { if (old && old != HGDI_ERROR) SelectObject(dc, old); ReleaseDC(window, dc); } }
            } measure{host};
            return LayoutNode(root, measure.dc);
        };
        auto tree = measureTree("mms.toolbar.main.extensions", toolbar);
        std::map<std::string, toolbar_overflow::Policy> policies;
        for (const auto& [id, e] : registry.elements) policies[id] = {e.effectiveHints.overflow == 3,
            e.desc.kind == MMS_UI_BUTTON, e.desc.kind == MMS_UI_SEPARATOR, (e.desc.capabilities & MMS_UI_CONTAINER) != 0};
        const int layoutWidth = std::max(0, width - GetSystemMetrics(SM_CXVSCROLL));
        auto overflow = toolbar_overflow::Arrange(tree, policies, float(layoutWidth) * 96 / dpi, float(maximum) * 96 / dpi);
        auto& flow = overflow.layout;
        overflowIds = std::move(overflow.hidden);
        contentHeight = int(std::ceil(std::max(flow.content.height, overflow.menu ? 32.0f : 0.0f) * dpi / 96));
        height = visible && width > 0 ? std::min(contentHeight, maximum) : 0;
        scroll = std::clamp(scroll, 0, std::max(0, contentHeight - height));
        // Both mounts use the same tree measurement and retained objects. The
        // property strip does not opt into the toolbar-only overflow menu.
        auto propertyTree = measureTree("mms.properties.main.extensions", properties);
        auto propertyFlow = native_ui::Arrange(propertyTree, {float(layoutWidth) * 96 / dpi, 0});
        const bool propertySlots = std::any_of(propertyFlow.items.begin(), propertyFlow.items.end(), [&](const auto& item) {
            const auto control = controls.find(item.id);
            return control != controls.end() && control->second.parent == properties && item.rect.height > 0 &&
                (item.visible || (registry.elements.at(item.id).effectiveHints.flags & MMS_HINT_PRESERVE_HIDDEN_SLOT));
        });
        propertyContentHeight = propertySlots ? int(std::ceil(propertyFlow.content.height * dpi / 96)) : 0;
        propertyHeight = visible && propertyVisible && width > 0 ?
            std::min({propertyContentHeight, propertyMaximum, propertyBottom}) : 0;
        propertyScroll = std::clamp(propertyScroll, 0, std::max(0, propertyContentHeight - propertyHeight));
        const int propertyTop = propertyBottom - propertyHeight;
        // A partial move or allocation failure must not leave old rectangles
        // advertised as ready. Only the final successful snapshot resumes them.
        registry.Refresh().SuspendTargets();
        registry.SuspendGeometry();
        struct PublicationGuard {
            UiRegistry& registry; bool completed = false;
            ~PublicationGuard() { if (!completed) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); } }
        } publication{registry};
        HostLayoutPaint paint(*this);
        std::vector<native_ui::WindowMove> moves; moves.reserve(controls.size() + 1);
        applied = paint.Move(toolbar, x, 0, std::max(0, width), height) && applied;
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS}; info.nMin = 0; info.nMax = std::max(0, contentHeight - 1); info.nPage = height; info.nPos = scroll;
        applied = paint.Scroll(toolbar, SB_VERT, info) && applied;
        applied = paint.Move(properties, x, propertyTop, std::max(0, width), propertyHeight) && applied;
        SCROLLINFO propertyInfo{sizeof(propertyInfo), SIF_RANGE | SIF_PAGE | SIF_POS};
        propertyInfo.nMin = 0; propertyInfo.nMax = std::max(0, propertyContentHeight - 1);
        propertyInfo.nPage = propertyHeight; propertyInfo.nPos = propertyScroll;
        applied = paint.Scroll(properties, SB_VERT, propertyInfo) && applied;
        RECT toolbarClient{}; applied = GetClientRect(toolbar, &toolbarClient) && applied;
        const int menuRight = std::max(0L, toolbarClient.right);
        const int menuWidth = std::min(menuRight, MulDiv(32, dpi, 96));
        const int menuHeight = std::min(height, MulDiv(24, dpi, 96));
        const bool menuVisible = overflow.menu && visible && menuWidth > 0 && menuHeight > 0;
        applied = paint.Queue(moves, overflowButton, {menuRight - menuWidth, 0, menuRight, menuHeight}) && applied;
        if (!menuVisible && GetFocus() == overflowButton) SetFocus(main);
        applied = paint.Visible(overflowButton, menuVisible) && applied;
        const auto place = [&](const native_ui::TreeResult& arranged, HWND host, int hostTop, int extent, int offset, bool property) {
        for (const auto& item : arranged.items) {
            auto rect = item.rect; auto control = controls.find(item.id);
            // The hidden HWND is still the STATE/provider target. Keep its
            // measured client extent so refresh can update the menu without
            // manufacturing a second presentation or a second command object.
            if (!property && overflowIds.count(item.id)) {
                rect = overflow.retained.at(item.id);
                rect.width = std::max(1.0f, rect.width); rect.height = std::max(1.0f, rect.height);
            }
            int left = int(std::lround(rect.x * dpi / 96)), top = int(std::lround(rect.y * dpi / 96)) - offset;
            int right = int(std::lround((rect.x + rect.width) * dpi / 96)), bottom = int(std::lround((rect.y + rect.height) * dpi / 96)) - offset;
            if (control != controls.end()) {
                if (control->second.parent != host) { applied = false; continue; }
                const bool hidden = !property && overflowIds.count(item.id) != 0;
                const bool shown = item.visible && !hidden && visible && extent > 0;
                if (control->second.input) control->second.input->LayoutVisible(shown);
                if (hidden) control->second.layoutVisible = false;
                const HWND focused = GetFocus();
                if (!shown && focused && (focused == control->second.window || IsChild(control->second.window, focused)))
                    SetFocus(hidden && menuVisible ? overflowButton : main);
                applied = paint.Queue(moves, control->second.window, {left, top, right, bottom}) && applied;
                applied = paint.Visible(control->second.window, item.visible && !hidden) && applied;
            }
            geometry[item.id] = {(x + left) * 96.0f / dpi, (hostTop + top) * 96.0f / dpi, (right - left) * 96.0f / dpi, (bottom - top) * 96.0f / dpi,
                item.visible && extent > 0 && (property || !overflowIds.count(item.id)),
                (!property && overflowIds.count(item.id)) || item.overflowed || top < 0 || bottom > extent};
        }
        };
        place(flow, toolbar, 0, height, scroll, false);
        place(propertyFlow, properties, propertyTop, propertyHeight, propertyScroll, true);
        {
            const WindowDpiScope toolbarDpi(toolbar);
            applied = native_ui::ApplyWindows(moves) == native_ui::ApplyResult::Applied && applied;
        }
        for (const auto& [id, control] : controls) if (registry.elements.at(id).desc.kind == MMS_UI_BUTTON || control.input)
            applied = TipsRect(control) && applied;
        const auto leaveHiddenHost = [&](HWND host, int extent) {
            const HWND focus = GetFocus();
            if (!extent && focus && (focus == host || IsChild(host, focus))) SetFocus(main);
        };
        leaveHiddenHost(toolbar, height); leaveHiddenHost(properties, propertyHeight);
        applied = paint.Visible(toolbar, height > 0) && applied;
        applied = paint.Visible(properties, propertyHeight > 0) && applied;
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
        // An explicit LAYOUT request re-runs native workspace constraints even
        // when desired bounds are unchanged. Surface sizing consumes the
        // resulting actual client, preserving its callback/error semantics.
        applied = paint.Move(workspace, x, height, std::max(0, width), workspaceHeight, fromRefresh) && applied;
        applied = paint.Visible(workspace, workspaceHeight > 0) && applied;
        geometry["mms.toolbar.main.extensions"] = {x * 96.0f / dpi, 0, width * 96.0f / dpi, height * 96.0f / dpi, height > 0, overflow.menu || contentHeight > height};
        geometry["mms.properties.main.extensions"] = {x * 96.0f / dpi, propertyTop * 96.0f / dpi,
            width * 96.0f / dpi, propertyHeight * 96.0f / dpi, propertyHeight > 0, propertyContentHeight > propertyHeight};
        const RECT workspaceBounds = ui::layout_detail::ActualRect(main, workspace);
        geometry["mms.workspace.tabs.extensions"] = {workspaceBounds.left * 96.0f / dpi, workspaceBounds.top * 96.0f / dpi,
            (workspaceBounds.right - workspaceBounds.left) * 96.0f / dpi, (workspaceBounds.bottom - workspaceBounds.top) * 96.0f / dpi, workspaceHeight > 0, false};
        LayoutPanels(geometry, applied, paint);
        if (fromRefresh && context != layoutContext) { layoutResult = MMS_E_STALE; applied = false; }
        if (applied && fromRefresh && layoutPublished && (height + workspaceHeight != priorHeight || propertyHeight != priorPropertyHeight)) {
            const auto dependencyResult = Boundary([&] { return layoutPublished(height + workspaceHeight); });
            if (dependencyResult) { layoutResult = dependencyResult; applied = false; }
            if (context != layoutContext) { layoutResult = MMS_E_STALE; applied = false; }
        }
        // A failed WM_SETTEXT can have changed HWND text while its retained
        // content stayed old. Direct resize/layout must repair that mismatch
        // before advertising measured geometry as ready. No provider is read.
        if (applied) for (const auto& [id, control] : controls) {
            const auto element = registry.elements.find(id);
            if (element == registry.elements.end() || element->second.handle != control.element) { layoutResult = MMS_E_STALE; applied = false; break; }
            if (element->second.desc.kind != MMS_UI_BUTTON && !control.input) continue;
            const auto result = control.input ? control.input->RepairLabel() : WriteCaption(control.window, EffectiveCaption(element->second, &control));
            if (result) { layoutResult = result; applied = false; break; }
        }
        if (applied) {
            std::vector<std::string> menuIds;
            for (const auto& [id, control] : controls) if (overflowMenuItems.count(GetDlgCtrlID(control.window))) menuIds.push_back(id);
            for (const auto& id : menuIds) if (const auto result = UpdateOverflowItem(id)) { layoutResult = result; applied = false; break; }
        }
        if (applied) {
            std::vector<RefreshPresentation> presentations;
            if (Presentations(geometry, presentations) && registry.PublishGeometry(geometry, uiRevision)) {
                registry.Refresh().SyncTargets(presentations);
                layoutResult = MMS_OK;
                publication.completed = true;
            }
        }
        return height + workspaceHeight;
    }
    MmsResult Relayout(MmsHandle element) {
        const NativeWork work(*this);
        if (refreshLayout || refreshing || !IsWindow(main) || revision != registry.Revision()) return MMS_E_NOT_READY;
        bool present = false;
        for (const auto& [id, e] : registry.elements) if (e.handle == element) {
            if (e.desc.kind == MMS_UI_BUTTON || e.desc.kind == MMS_UI_SEPARATOR || e.desc.kind == MMS_UI_INPUT) {
                const auto c = controls.find(id); present = c != controls.end() && c->second.element == element && IsWindow(c->second.window);
            } else if (e.desc.kind == MMS_UI_PANEL) { UiRegistry::SurfacePresentation view; present = registry.ExistingSurface(id, view); }
            else if (e.desc.kind == MMS_UI_GROUP) present = IsWindow(ControlHost(e));
            break;
        }
        if (!present) return MMS_E_STALE;
        refreshLayout = true; struct Guard { bool& value; ~Guard() { value = false; } } guard{refreshLayout};
        Layout(true);
        return layoutResult;
    }
    void DeferPresentation() noexcept {
        if (presentationPending) return;
        presentationPending = true; registry.RequestPresentation();
    }
    bool Command(unsigned id) {
        const NativeWork work(*this);
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
    const Impl::NativeWork work(*impl_);
    for (const auto& [id, e] : impl_->registry.elements) if (e.key == key && e.modifiers == modifiers && (!e.scope || viewportFocused) && e.visible) return impl_->Command(impl_->Id(id, e));
    return false;
}
int NativeUi::Layout(int x, int width, int maximum, bool visible) {
    const Impl::NativeWork work(*impl_);
    maximum = std::max(0, maximum);
    if (impl_->x != x || impl_->width != width || impl_->maximum != maximum || impl_->visible != visible) ++impl_->layoutContext;
    impl_->x = x; impl_->width = width; impl_->maximum = maximum; impl_->visible = visible;
    return impl_->Layout();
}
void NativeUi::SetPropertyArea(int bottom, int maximumHeight, bool visible) {
    const Impl::NativeWork work(*impl_);
    const WindowDpiScope dpi(impl_->main);
    RECT client{}; GetClientRect(impl_->main, &client);
    bottom = std::clamp(bottom, 0, std::max(0, int(client.bottom)));
    maximumHeight = std::clamp(maximumHeight, 0, bottom);
    if (impl_->propertyBottom != bottom || impl_->propertyMaximum != maximumHeight || impl_->propertyVisible != visible) ++impl_->layoutContext;
    impl_->propertyBottom = bottom; impl_->propertyMaximum = maximumHeight; impl_->propertyVisible = visible;
}
int NativeUi::PropertyAreaHeight() const noexcept { return impl_->propertyHeight; }
HWND NativeUi::PropertyArea() const noexcept { return impl_->properties; }
void NativeUi::SetLayoutPublished(std::function<MmsResult(int)> callback) { impl_->layoutPublished = std::move(callback); }
HWND NativeUi::Toolbar() const { return impl_->toolbar; }
bool NativeUi::PopupActive() const noexcept { return impl_->overflowOpen; }
bool NativeUi::DeferClose(WPARAM wp, LPARAM lp) noexcept { return impl_->DeferClose(wp, lp); }
bool NativeUi::DialogMessage(MSG* message) {
    const Impl::NativeWork work(*impl_);
    if (!message || (message->message != WM_KEYDOWN && message->message != WM_CHAR)) return false;
    // Push-like check buttons retain command activation with Enter; their
    // checked state remains exclusively controlled by getState, not by clicks.
    if (message->wParam == VK_RETURN && GetFocus() == message->hwnd) {
        if (message->hwnd == impl_->overflowButton && IsWindowEnabled(message->hwnd) && IsWindowVisible(message->hwnd)) {
            Boundary([&] { impl_->OpenOverflow(); return MMS_OK; }); return true;
        }
        for (const auto& [id, control] : impl_->controls) if (control.window == message->hwnd) {
            auto element = impl_->registry.elements.find(id);
            if (element != impl_->registry.elements.end() && element->second.desc.kind == MMS_UI_BUTTON) {
                if (IsWindowEnabled(control.window) && IsWindowVisible(control.window)) impl_->Command(GetDlgCtrlID(control.window));
                return true;
            }
        }
    }
    for (HWND window : {impl_->toolbar, impl_->properties, impl_->workspace})
        if (message->hwnd == window || IsChild(window, message->hwnd)) return IsDialogMessageW(window, message) != FALSE;
    return false;
}
}
