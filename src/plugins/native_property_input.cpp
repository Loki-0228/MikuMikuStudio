#include "native_property_input.hpp"
#include "ui_property_draft.hpp"
#include "mikumikustudio/plugin_input_focus.hpp"
#include <commctrl.h>
#include <array>
#include <atomic>
#include <limits>

namespace mikumikustudio::plugins {
struct NativePropertyInput::Impl {
    static constexpr UINT kCheckFocus = WM_APP + 0x4b1;
    static constexpr unsigned kFirstField = 0x7201;
    UiRegistry& registry;
    std::string elementId;
    std::wstring cachedLabel;
    std::function<void(bool)> nativeWork;
    Log log;
    HWND window = nullptr, label = nullptr;
    std::vector<HWND> fields;
    HFONT font = nullptr;
    uint32_t dpi = 0;
    PropertyInputSnapshot snapshot;
    UiPropertyDraft draft;
    std::shared_ptr<std::atomic<bool>> intent;
    std::array<bool, 4> invalid{};
    bool haveSnapshot = false, destroying = false, updating = false, oversized = false;
    bool refreshing = false, submitting = false, visible = true, imeEnter = false, focusQueued = false;
    UINT_PTR focusSerial = 0;

    struct Work {
        Impl& owner; bool entered = false;
        explicit Work(Impl& value) noexcept: owner(value) {
            if (owner.nativeWork) try { owner.nativeWork(true); entered = true; } catch (...) {}
        }
        ~Work() noexcept { if (entered) try { owner.nativeWork(false); } catch (...) {} }
    };
    struct Flag {
        bool& value; bool previous;
        explicit Flag(bool& target): value(target), previous(target) { value = true; }
        ~Flag() { value = previous; }
    };
    Impl(UiRegistry& r, std::string id, HWND parent, unsigned controlId,
        std::function<void(bool)> work, Log output): registry(r), elementId(std::move(id)),
        nativeWork(std::move(work)), log(std::move(output)) {
        Work guard(*this);
        try {
            window = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"",
                WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
                reinterpret_cast<HMENU>(UINT_PTR(controlId)), GetModuleHandleW(nullptr), nullptr);
            if (!window || !SetWindowSubclass(window, RootProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
                throw std::runtime_error("property input wrapper");
            if (!SetPropW(window, kPropertyInputWindowMarker, this))
                throw std::runtime_error("property input focus marker");
            label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!label) throw std::runtime_error("property input label");
        } catch (...) { Destroy(); throw; }
    }
    ~Impl() { Work guard(*this); Destroy(); }
    void Report(MmsResult result) noexcept {
        if (log) try { log(elementId + ": property input result=" + std::to_string(result)); } catch (...) {}
    }
    void Destroy() noexcept {
        destroying = true; ClearDraft(); haveSnapshot = false;
        snapshot = {};
        for (HWND field : fields) if (IsWindow(field)) RemoveWindowSubclass(field, FieldProc, reinterpret_cast<UINT_PTR>(this));
        fields.clear();
        if (IsWindow(window)) { RemovePropW(window, kPropertyInputWindowMarker); RemoveWindowSubclass(window, RootProc, reinterpret_cast<UINT_PTR>(this)); DestroyWindow(window); }
        window = label = nullptr;
        if (font) DeleteObject(font); font = nullptr;
    }
    void ClearDraft() noexcept {
        if (intent) intent->store(false, std::memory_order_relaxed);
        intent.reset();
        draft.Cancel(); invalid.fill(false); imeEnter = false; focusQueued = false; ++focusSerial;
    }
    bool Contains(HWND focus) const noexcept { return focus && (focus == window || IsChild(window, focus)); }
    int Scale(int value) const noexcept { return MulDiv(value, int(dpi ? dpi : 96), 96); }
    void Font(uint32_t value) {
        value = value ? std::min(value, 960u) : 96;
        if (dpi != value || !font) {
            LOGFONTW description{};
            if (!GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(description), &description)) return;
            description.lfHeight = MulDiv(description.lfHeight, int(value), 96);
            HFONT next = CreateFontIndirectW(&description); if (!next) return;
            HFONT old = font; font = next; dpi = value;
            SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            if (old) DeleteObject(old);
        }
    }
    void Layout() noexcept {
        if (!window || !label) return;
        RECT rect{}; if (!GetClientRect(window, &rect)) return;
        const int width = std::max(0L, rect.right), height = std::max(0L, rect.bottom);
        const int marginX = std::min(std::max(1, Scale(2)), width / 2);
        const int marginY = std::min(std::max(1, Scale(2)), height / 2);
        const int inside = std::max(0, width - marginX * 2);
        const int labelWidth = width < Scale(80) ? 0 : std::min(inside / 3, Scale(110));
        const int labelGap = labelWidth ? std::min(std::max(1, Scale(3)), inside - labelWidth) : 0;
        const int rowHeight = std::max(0, std::min(height - marginY * 2, Scale(25)));
        const int top = std::max(0, (height - rowHeight) / 2);
        const int labelOffset = std::min(rowHeight, std::max(0, Scale(3)));
        SetWindowPos(label, nullptr, marginX, top + labelOffset,
            labelWidth, rowHeight - labelOffset, SWP_NOZORDER | SWP_NOACTIVATE);
        const int count = int(fields.size()); if (!count) return;
        const int start = marginX + labelWidth + labelGap;
        const int available = inside - labelWidth - labelGap;
        const int gap = count > 1 ? std::min(std::max(1, Scale(3)), available / (count - 1)) : 0;
        const int space = std::max(0, available - gap * (count - 1));
        for (int n = 0; n < count; ++n) {
            const int left = start + space * n / count + gap * n;
            const int right = start + space * (n + 1) / count + gap * n;
            SetWindowPos(fields[size_t(n)], nullptr, left, top, std::max(0, right - left), rowHeight,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    void ReadOnly(bool readOnly) noexcept {
        if (readOnly && draft.Active()) CancelInternal();
        for (HWND field : fields) if (bool(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY) != readOnly)
            SendMessageW(field, EM_SETREADONLY, readOnly ? TRUE : FALSE, 0);
    }
    static void Caption(HWND target, const std::wstring& text) {
        const int count = GetWindowTextLengthW(target);
        if (count >= 0 && size_t(count) == text.size()) {
            std::wstring current(size_t(count) + 1, L'\0');
            const int read = GetWindowTextW(target, current.data(), int(current.size()));
            if (read == count && std::wstring_view(current.data(), size_t(read)) == text) return;
        }
        if (!SetWindowTextW(target, text.c_str())) throw std::runtime_error("property input caption");
    }
    void Build(size_t count) {
        if (count == fields.size()) return;
        if (!count || count > 4) throw std::invalid_argument("property field count");
        // Build the replacement first; a failed creation leaves the old HWNDs
        // intact. Old focus-loss messages cannot submit while updating.
        const HWND originalFocus = GetFocus();
        auto focused = std::find(fields.begin(), fields.end(), originalFocus);
        const bool originallyFocused = focused != fields.end();
        size_t focusIndex = originallyFocused ? size_t(focused - fields.begin()) : 0;
        std::vector<HWND> next; next.reserve(count);
        try {
            for (size_t n = 0; n < count; ++n) {
                HWND field = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window,
                    reinterpret_cast<HMENU>(UINT_PTR(kFirstField + unsigned(n))), GetModuleHandleW(nullptr), nullptr);
                if (!field) throw std::runtime_error("property field window");
                next.push_back(field);
                if (!SetWindowSubclass(field, FieldProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
                    throw std::runtime_error("property field subclass");
                SendMessageW(field, EM_SETLIMITTEXT, UiPropertyDraft::kMaxFieldBytes, 0);
                SendMessageW(field, WM_SETFONT, font ? reinterpret_cast<WPARAM>(font) : reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
            }
        } catch (...) {
            for (HWND field : next) { RemoveWindowSubclass(field, FieldProc, reinterpret_cast<UINT_PTR>(this)); DestroyWindow(field); }
            throw;
        }
        // Do not steal focus if creation hooks or nested messages moved it to
        // another control. A null focus is eligible only after our own destroy.
        const HWND beforeDestroy = GetFocus();
        const bool restoreFocus = originallyFocused && Contains(beforeDestroy);
        focused = std::find(fields.begin(), fields.end(), beforeDestroy);
        if (focused != fields.end()) focusIndex = size_t(focused - fields.begin());
        for (HWND field : fields) { RemoveWindowSubclass(field, FieldProc, reinterpret_cast<UINT_PTR>(this)); DestroyWindow(field); }
        fields.swap(next); invalid.fill(false);
        for (HWND field : fields) ShowWindow(field, SW_SHOWNA);
        const HWND afterDestroy = GetFocus();
        if (restoreFocus && (!afterDestroy || Contains(afterDestroy)) && !destroying && visible &&
            IsWindowVisible(window) && IsWindowEnabled(window)) SetFocus(fields[std::min(focusIndex, fields.size() - 1)]);
    }
    void WriteCached() {
        if (!haveSnapshot || destroying) return;
        std::vector<std::string> text;
        const auto canonical = snapshot.value.View();
        bool tooLong = (canonical.type == MMS_VALUE_TEXT && canonical.value.text.bytes > UiPropertyDraft::kMaxFieldBytes) ||
            (canonical.type == MMS_VALUE_BYTES && canonical.value.bytes.bytes > UiPropertyDraft::kMaxFieldBytes / 2);
        if (!tooLong) {
            const auto result = snapshot.value.Format(text);
            if (result == MMS_E_MEMORY) throw std::bad_alloc();
            if (result) throw std::invalid_argument("property canonical format");
            for (const auto& field : text) if (field.size() > UiPropertyDraft::kMaxFieldBytes) tooLong = true;
        }
        if (tooLong) { text.clear(); text.emplace_back(u8"值过长（默认输入上限 1 MiB），请使用自定义编辑器"); }
        oversized = tooLong;
        Flag guard(updating); Build(text.size());
        for (size_t n = 0; n < fields.size(); ++n) {
            Caption(fields[n], Wide(text[n]));
        }
        Caption(label, cachedLabel);
        invalid.fill(false); Layout();
    }
    void CancelInternal() noexcept {
        ClearDraft();
        const auto result = Boundary([&] { WriteCached(); return MMS_OK; }); if (result) Report(result);
    }
    void Refresh(uint32_t value) {
        Work guard(*this); if (destroying || refreshing || !window) return; Flag refresh(refreshing);
        const auto result = Boundary([&]() -> MmsResult {
            Font(value);
            if (haveSnapshot && !registry.IsPropertyInputCurrent(snapshot)) {
                ClearDraft(); haveSnapshot = false; snapshot = {};
            }
            PropertyInputSnapshot next;
            const auto captured = registry.CapturePropertyInput(elementId, next);
            if (captured) return captured;
            const auto element = registry.elements.find(elementId);
            if (element == registry.elements.end()) return MMS_E_STALE;
            auto labelText = String(element->second.desc.label);
            if (labelText.empty()) labelText = String(next.property->desc.info.label);
            auto caption = Wide(labelText);
            cachedLabel.swap(caption);
            if (draft.Active() && next.property == snapshot.property && next.element == snapshot.element && next.editable) {
                // Preserve both the original canonical value/revision and raw text.
                // A changed source is detected at submission, never overwritten by refresh.
                Caption(label, cachedLabel); Layout(); return MMS_OK;
            }
            ClearDraft(); snapshot = std::move(next); haveSnapshot = true;
            WriteCached(); ReadOnly(!snapshot.editable || oversized);
            return MMS_OK;
        });
        if (result) { CancelInternal(); ReadOnly(true); Report(result); }
    }
    bool Start() {
        if (destroying || updating || !haveSnapshot || !snapshot.editable || oversized || !visible || !IsWindowEnabled(window)) return false;
        if (draft.Active()) return true;
        const auto result = Boundary([&]() -> MmsResult {
            auto next = std::make_shared<std::atomic<bool>>(true);
            if (const auto begun = draft.Begin(snapshot.value.View(), snapshot.sceneRevision)) return begun;
            intent = std::move(next); return MMS_OK;
        });
        if (result) Report(result);
        return !result;
    }
    void NewInputIntent() noexcept {
        if (intent) intent->store(false, std::memory_order_relaxed);
        // Allocation failure leaves the existing request invalid. The raw
        // fields remain available and the next explicit submit can retry.
        const auto result = Boundary([&]() -> MmsResult {
            auto next = std::make_shared<std::atomic<bool>>(true); intent = std::move(next); return MMS_OK;
        });
        if (result) Report(result);
    }
    bool CurrentIntent(const std::shared_ptr<std::atomic<bool>>& request) const noexcept {
        return request && intent == request && request->load(std::memory_order_relaxed);
    }
    void Changed(HWND field, bool external = true) {
        if (destroying || updating) return;
        const auto found = std::find(fields.begin(), fields.end(), field); if (found == fields.end()) return;
        if (!Start()) return;
        if (submitting && external) NewInputIntent();
        const size_t index = size_t(found - fields.begin());
        const auto result = Boundary([&]() -> MmsResult {
            const int count = GetWindowTextLengthW(field);
            if (count < 0 || size_t(count) > UiPropertyDraft::kMaxFieldBytes) return MMS_E_ARGUMENT;
            std::wstring text(size_t(count) + 1, L'\0');
            const int read = GetWindowTextW(field, text.data(), int(text.size()));
            if (read < 0 || read > count) return MMS_E_ARGUMENT;
            text.resize(size_t(read));
            return draft.SetField(index, Utf8(text));
        });
        invalid[index] = result != MMS_OK;
    }
    void Submit() {
        Work guard(*this);
        if (destroying || submitting || updating || !draft.Active() || draft.Composing()) return;
        Flag submission(submitting);
        if (!intent || !intent->load(std::memory_order_relaxed)) NewInputIntent();
        const auto request = intent;
        if (!CurrentIntent(request)) return;
        // A read callback can pump messages that refresh, hide or cancel this
        // view. Keep the original broker input independent of mutable members.
        auto origin = snapshot; origin.intent = request;
        for (HWND field : fields) Changed(field, false);
        for (bool failure : invalid) if (failure) { Report(MMS_E_ARGUMENT); return; }
        PropertyInputSnapshot current;
        auto result = registry.CapturePropertyInput(elementId, current);
        if (!CurrentIntent(request)) { Report(MMS_E_STALE); return; }
        if (!result && (current.property != origin.property || current.element != origin.element)) result = MMS_E_STALE;
        UiPropertyValue candidate; bool changed = false;
        if (!result) result = draft.Prepare(current.value.View(), current.sceneRevision, candidate, changed);
        if (!CurrentIntent(request)) { Report(MMS_E_STALE); return; }
        if (!result && changed) result = registry.CommitPropertyInput(origin, candidate.View());
        // Escape/hide may already have cancelled, or a reentrant EN_CHANGE
        // may own a replacement draft. Neither belongs to this old request.
        if (!CurrentIntent(request)) { if (result) Report(result); return; }
        if (!result) { ClearDraft(); Refresh(dpi); return; }
        if (result == MMS_E_STALE || result == MMS_E_NOT_FOUND || result == MMS_E_NOT_READY) { ClearDraft(); Refresh(dpi); }
        Report(result);
    }
    void QueueFocus() noexcept {
        if (!destroying && !updating && draft.Active() && !focusQueued) {
            const auto serial = ++focusSerial;
            if (PostMessageW(window, kCheckFocus, serial, 0)) focusQueued = true;
        }
    }
    static LRESULT CALLBACK RootProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data); Work work(self);
        if (message == WM_NCDESTROY) {
            RemovePropW(hwnd, kPropertyInputWindowMarker);
            self.destroying = true; self.ClearDraft(); self.haveSnapshot = false; self.snapshot = {};
            self.window = self.label = nullptr; self.fields.clear(); RemoveWindowSubclass(hwnd, RootProc, id);
            return DefSubclassProc(hwnd, message, wp, lp);
        }
        if (self.destroying) return DefSubclassProc(hwnd, message, wp, lp);
        LRESULT answer = 0; bool handled = false;
        const auto result = Boundary([&]() -> MmsResult {
            if (message == WM_COMMAND) {
                const HWND field = reinterpret_cast<HWND>(lp);
                if (std::find(self.fields.begin(), self.fields.end(), field) != self.fields.end()) {
                    if (HIWORD(wp) == EN_CHANGE) self.Changed(field);
                    if (HIWORD(wp) == EN_SETFOCUS) {
                        if (!self.updating && !self.draft.Active()) self.Refresh(self.dpi);
                        if (self.Contains(GetFocus())) self.Start();
                    }
                    if (HIWORD(wp) == EN_KILLFOCUS) self.QueueFocus();
                    handled = true;
                }
            } else if (message == kCheckFocus) {
                if (wp == self.focusSerial) { self.focusQueued = false; if (!self.Contains(GetFocus())) self.Submit(); }
                handled = true;
            } else if (message == WM_SIZE) self.Layout();
            else if (message == WM_SHOWWINDOW) { self.visible = wp != 0; if (!wp) self.CancelInternal(); }
            else if (message == WM_ENABLE) { if (!wp) self.CancelInternal(); for (HWND field : self.fields) EnableWindow(field, wp != 0); }
            else if (message == WM_SETFONT) {
                if (self.label) SendMessageW(self.label, WM_SETFONT, wp, lp);
                for (HWND field : self.fields) SendMessageW(field, WM_SETFONT, wp, lp);
            }
            return MMS_OK;
        });
        if (result) self.Report(result);
        return handled ? answer : DefSubclassProc(hwnd, message, wp, lp);
    }
    static LRESULT CALLBACK FieldProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data); Work work(self);
        if (message == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, FieldProc, id); return DefSubclassProc(hwnd, message, wp, lp); }
        if (self.destroying) return DefSubclassProc(hwnd, message, wp, lp);
        if (message == WM_GETDLGCODE) {
            const auto* key = reinterpret_cast<const MSG*>(lp);
            const bool wanted = wp == VK_RETURN || wp == VK_ESCAPE || (key && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE));
            const bool compositionTab = self.draft.Composing() && (wp == VK_TAB || (key && key->wParam == VK_TAB));
            return DefSubclassProc(hwnd, message, wp, lp) | (wanted || compositionTab ? DLGC_WANTMESSAGE : 0) |
                (compositionTab ? DLGC_WANTTAB : 0);
        }
        if (message == WM_IME_ENDCOMPOSITION) {
            const auto answer = DefSubclassProc(hwnd, message, wp, lp);
            self.imeEnter = self.imeEnter || (GetKeyState(VK_RETURN) & 0x8000) != 0;
            self.draft.SetComposing(false); self.QueueFocus(); return answer;
        }
        bool handled = false;
        const auto result = Boundary([&]() -> MmsResult {
            if (message == WM_IME_STARTCOMPOSITION) {
                if (self.Start() && self.submitting) self.NewInputIntent();
                self.draft.SetComposing(true);
            }
            if (message == WM_IME_KEYDOWN && wp == VK_RETURN && self.draft.Composing()) self.imeEnter = true;
            if (message == WM_KEYDOWN && wp == VK_RETURN) {
                if (self.draft.Composing()) self.imeEnter = true;
                else { if (!self.imeEnter) self.Submit(); handled = true; }
            }
            if (message == WM_KEYDOWN && wp == VK_ESCAPE && !self.draft.Composing()) { self.CancelInternal(); handled = true; }
            if (message == WM_KEYUP && wp == VK_RETURN) { self.imeEnter = false; handled = true; }
            if (message == WM_CHAR && wp == L'\r') handled = true;
            if (message == WM_CHAR && wp == VK_ESCAPE && !self.draft.Composing()) handled = true;
            return MMS_OK;
        });
        if (result) self.Report(result);
        return handled ? 0 : DefSubclassProc(hwnd, message, wp, lp);
    }
};

NativePropertyInput::NativePropertyInput(UiRegistry& registry, std::string elementId, HWND parent,
    unsigned controlId, std::function<void(bool)> nativeWork, Log log):
    impl_(std::make_unique<Impl>(registry, std::move(elementId), parent, controlId, std::move(nativeWork), std::move(log))) {}
NativePropertyInput::~NativePropertyInput() = default;
HWND NativePropertyInput::Window() const { return impl_->window; }
void NativePropertyInput::Refresh(uint32_t dpi) { impl_->Refresh(dpi); }
void NativePropertyInput::Cancel() noexcept { Impl::Work work(*impl_); impl_->CancelInternal(); }
void NativePropertyInput::LayoutVisible(bool visible) noexcept {
    Impl::Work work(*impl_); impl_->visible = visible; if (!visible) impl_->CancelInternal();
}

}
