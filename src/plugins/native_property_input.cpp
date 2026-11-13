#include "native_property_input.hpp"
#include "ui_property_draft.hpp"
#include "mikumikustudio/plugin_input_focus.hpp"
#include <commctrl.h>
#include <imm.h>
#include <array>
#include <atomic>
#include <limits>

namespace mikumikustudio::plugins {
struct NativePropertyInput::Impl {
    static constexpr UINT kCheckFocus = WM_APP + 0x4b1;
    static constexpr unsigned kFirstField = 0x7201;
    UiRegistry& registry;
    std::string elementId;
    std::wstring cachedLabel, stateLabel;
    std::function<void(bool)> nativeWork;
    Log log;
    DWORD thread = GetCurrentThreadId();
    HWND window = nullptr, label = nullptr;
    std::vector<HWND> fields;
    HFONT font = nullptr;
    uint32_t dpi = 0;
    PropertyInputSnapshot snapshot, draftOrigin;
    UiPropertyDraft draft;
    std::shared_ptr<std::atomic<bool>> intent;
    std::array<bool, 4> invalid{};
    bool haveSnapshot = false, destroying = false, updating = false, oversized = false;
    bool refreshing = false, readingBack = false, submitting = false, visible = true, imeEnter = false, focusQueued = false;
    bool applyingState = false, haveState = false, stateEnabled = true, stateReadOnly = false, stateVisible = true;
    bool layoutSuspended = false;
    uint64_t layoutSuspendSerial = 0;
    UINT_PTR focusSerial = 0;
    uint64_t displayGeneration = 1;

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
        ++layoutSuspendSerial;
        if (intent) intent->store(false, std::memory_order_relaxed);
        intent.reset();
        draft.Cancel(); draftOrigin = {}; invalid.fill(false); imeEnter = false; focusQueued = false; ++focusSerial; ++displayGeneration;
    }
    void SuspendLayoutDraft() noexcept {
        if (layoutSuspended) return;
        layoutSuspended = true;
        const auto serial = ++layoutSuspendSerial;
        if (intent) intent->store(false, std::memory_order_relaxed);
        intent.reset(); focusQueued = false; ++focusSerial; ++displayGeneration;
        // Cancel conversion, never accept it as a side effect of hiding.
        // The EDIT retains already entered text and its existing selection.
        if (draft.Composing()) {
            Flag writing(updating);
            const auto root = window;
            const auto count = fields.size();
            if (count > 4) { CancelInternal(); Report(MMS_E_FAILED); return; }
            std::array<HWND, 4> original{};
            std::copy(fields.begin(), fields.end(), original.begin());
            const auto current = [&]() noexcept {
                if (destroying || !layoutSuspended || layoutSuspendSerial != serial || window != root ||
                    !IsWindow(root) || fields.size() != count) return false;
                for (size_t i = 0; i < count; ++i)
                    if (fields[i] != original[i] || !IsWindow(original[i]) || GetParent(original[i]) != root) return false;
                const auto element = registry.elements.find(elementId);
                return element != registry.elements.end() &&
                    registry.ControlVisibility(element->second) == ControlLayoutVisibility::TemporaryHidden;
            };
            bool cancelled = true;
            for (size_t i = 0; i < count; ++i) {
                if (!current()) return;
                const auto field = original[i];
                if (const auto context = ImmGetContext(field)) {
                    if (!current()) { ImmReleaseContext(field, context); return; }
                    cancelled = (ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0) != FALSE) && cancelled;
                    ImmReleaseContext(field, context);
                    if (!current()) return;
                }
            }
            if (!current()) return;
            draft.SetComposing(false); imeEnter = false;
            if (!cancelled) { CancelInternal(); Report(MMS_E_FAILED); }
        }
    }
    bool SuspendHiddenLayout() noexcept {
        const auto found = registry.elements.find(elementId);
        const auto visibility = found == registry.elements.end() ? ControlLayoutVisibility::PersistentHidden :
            registry.ControlVisibility(found->second);
        if (visibility == ControlLayoutVisibility::TemporaryHidden) { SuspendLayoutDraft(); return true; }
        if (visibility == ControlLayoutVisibility::PersistentHidden) { CancelInternal(); return true; }
        return false;
    }
    bool Contains(HWND focus) const noexcept { return focus && (focus == window || IsChild(window, focus)); }
    const std::wstring& DisplayLabel() const noexcept { return haveState ? stateLabel : cachedLabel; }
    bool StateEditable() const noexcept { return stateEnabled && !stateReadOnly && stateVisible; }
    bool EffectiveReadOnly(bool base) const noexcept { return base || !StateEditable(); }
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
        readOnly = EffectiveReadOnly(readOnly);
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
        std::wstring actual(std::max(size_t(std::max(0, count)), text.size()) + 1, L'\0');
        if (!SendMessageW(target, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str())) || !IsWindow(target) ||
            GetWindowTextLengthW(target) != int(text.size()) ||
            GetWindowTextW(target, actual.data(), int(actual.size())) != int(text.size()) ||
            std::wstring_view(actual.data(), text.size()) != text) throw std::runtime_error("property input caption");
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
        fields.swap(next); invalid.fill(false); ++layoutSuspendSerial;
        ++displayGeneration;
        for (HWND field : fields) ShowWindow(field, SW_SHOWNA);
        const HWND afterDestroy = GetFocus();
        if (restoreFocus && (!afterDestroy || Contains(afterDestroy)) && !destroying && visible &&
            IsWindowVisible(window) && IsWindowEnabled(window)) SetFocus(fields[std::min(focusIndex, fields.size() - 1)]);
    }
    static std::vector<std::wstring> FormatFields(const PropertyInputSnapshot& value, bool& tooLong) {
        std::vector<std::string> text;
        const auto canonical = value.value.View();
        tooLong = (canonical.type == MMS_VALUE_TEXT && canonical.value.text.bytes > UiPropertyDraft::kMaxFieldBytes) ||
            (canonical.type == MMS_VALUE_BYTES && canonical.value.bytes.bytes > UiPropertyDraft::kMaxFieldBytes / 2);
        if (!tooLong) {
            const auto result = value.value.Format(text);
            if (result == MMS_E_MEMORY) throw std::bad_alloc();
            if (result) throw std::invalid_argument("property canonical format");
            for (const auto& field : text) if (field.size() > UiPropertyDraft::kMaxFieldBytes) tooLong = true;
        }
        if (tooLong) { text.clear(); text.emplace_back(u8"值过长（默认输入上限 1 MiB），请使用自定义编辑器"); }
        std::vector<std::wstring> wide; wide.reserve(text.size());
        for (const auto& field : text) wide.push_back(Wide(field));
        return wide;
    }
    void WriteCached() {
        if (!haveSnapshot || destroying) return;
        bool tooLong = false; auto text = FormatFields(snapshot, tooLong);
        oversized = tooLong;
        Flag guard(updating); Build(text.size());
        for (size_t n = 0; n < fields.size(); ++n) {
            Caption(fields[n], text[n]);
        }
        Caption(label, DisplayLabel());
        invalid.fill(false); Layout();
    }
    void CancelInternal() noexcept {
        ClearDraft();
        const auto result = Boundary([&] {
            Flag writing(updating);
            WriteCached(); ReadOnly(!haveSnapshot || !snapshot.editable || oversized); return MMS_OK;
        }); if (result) Report(result);
    }
    static size_t FieldCount(const PropertyInputSnapshot& binding) noexcept {
        if (!binding.property) return 0;
        const auto& info = binding.property->desc.info;
        return info.valueType == MMS_VALUE_VECTOR ? info.components : 1;
    }
    MmsResult PrepareEmptyBinding() {
        const auto element = registry.elements.find(elementId);
        if (element == registry.elements.end()) return MMS_E_NOT_FOUND;
        if (element->second.desc.kind != MMS_UI_INPUT) return MMS_E_ARGUMENT;
        PropertyInputSnapshot binding;
        binding.elementId = elementId; binding.element = element->second.handle;
        binding.propertyId = String(element->second.desc.propertyId);
        const auto property = registry.properties.find(binding.propertyId);
        if (property == registry.properties.end()) return MMS_E_NOT_FOUND;
        binding.property = property->second;
        if (!registry.IsPropertyInputCurrent(binding)) return MMS_E_STALE;
        auto labelText = String(element->second.desc.label);
        if (labelText.empty()) labelText = String(binding.property->desc.info.label);
        auto caption = Wide(labelText);
        const auto count = FieldCount(binding);
        if (!count || count > 4) return MMS_E_ARGUMENT;
        // A registered type determines the native shape before the first
        // successful read. This binding owns no canonical value or revision.
        ClearDraft(); haveSnapshot = false; snapshot = std::move(binding); oversized = false;
        Flag writing(updating); Build(count);
        for (HWND field : fields) Caption(field, std::wstring{});
        Caption(label, haveState ? stateLabel : caption); cachedLabel.swap(caption); ReadOnly(true); Layout();
        return registry.IsPropertyInputCurrent(snapshot) ? MMS_OK : MMS_E_STALE;
    }
    void Refresh(uint32_t value) {
        Work guard(*this); if (destroying || refreshing || readingBack || applyingState || !window) return; Flag refresh(refreshing);
        const auto result = Boundary([&]() -> MmsResult {
            Font(value);
            if (haveSnapshot && !registry.IsPropertyInputCurrent(snapshot)) {
                ClearDraft(); haveSnapshot = false; snapshot = {};
            }
            if (!haveSnapshot) if (const auto prepared = PrepareEmptyBinding()) return prepared;
            PropertyInputSnapshot next;
            const auto captured = registry.CapturePropertyInput(elementId, next);
            if (captured) return captured;
            if (destroying || !window || next.property != snapshot.property || next.element != snapshot.element ||
                !registry.IsPropertyInputCurrent(snapshot)) return MMS_E_STALE;
            const auto element = registry.elements.find(elementId);
            if (element == registry.elements.end()) return MMS_E_STALE;
            auto labelText = String(element->second.desc.label);
            if (labelText.empty()) labelText = String(next.property->desc.info.label);
            auto caption = Wide(labelText);
            Caption(label, haveState ? stateLabel : caption); cachedLabel.swap(caption);
            if (draft.Active() && next.property == snapshot.property && next.element == snapshot.element && next.editable && StateEditable()) {
                // Advance the latest canonical cache without replacing the
                // draft's original value/revision or its unfinished raw text.
                Caption(label, DisplayLabel()); ReadOnly(false); Layout();
                snapshot = std::move(next); ++displayGeneration; return MMS_OK;
            }
            ClearDraft(); snapshot = std::move(next); haveSnapshot = true;
            WriteCached(); ReadOnly(!snapshot.editable || oversized);
            return MMS_OK;
        });
        if (result) {
            if (draft.Active() && haveSnapshot && registry.IsPropertyInputCurrent(snapshot)) {
                // A temporary provider/format failure must not turn a pure
                // refresh into loss of unsubmitted text. Revoke an in-flight
                // request, but retain raw fields and their original baseline.
                // The next explicit submit creates a fresh intent and reads
                // the source again before it can publish anything.
                if (intent) intent->store(false, std::memory_order_relaxed);
                intent.reset();
            } else {
                CancelInternal(); ReadOnly(true);
            }
            Report(result);
        }
    }
    MmsResult Readback(const std::function<MmsResult()>& revalidate, uint64_t& sourceRevision) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        Work work(*this);
        if (destroying || !window || !snapshot.property || !snapshot.element ||
            fields.size() != FieldCount(snapshot)) return MMS_E_STALE;
        if (readingBack || refreshing || updating || submitting || applyingState) return MMS_E_BUSY;
        Flag reading(readingBack);
        return Boundary([&]() -> MmsResult {
            const auto generation = displayGeneration;
            const auto requested = intent;
            const bool active = draft.Active();
            const bool hadCanonical = haveSnapshot;
            const auto original = snapshot;
            const auto windows = fields;
            const auto current = [&]() noexcept {
                if (destroying || !IsWindow(window) || displayGeneration != generation || intent != requested ||
                    draft.Active() != active || haveSnapshot != hadCanonical || fields != windows ||
                    !registry.IsPropertyInputCurrent(original)) return false;
                for (HWND field : windows) if (!IsWindow(field) || GetParent(field) != window) return false;
                return true;
            };
            const auto validate = [&]() -> MmsResult {
                if (!current()) return MMS_E_STALE;
                if (revalidate) { const auto result = revalidate(); if (result) return result; }
                return current() ? MMS_OK : MMS_E_STALE;
            };
            if (const auto result = validate()) return result;
            PropertyInputSnapshot next;
            if (const auto result = registry.CapturePropertyInput(elementId, next)) return result;
            if (const auto result = validate()) return result;
            if (next.property != original.property || next.element != original.element ||
                (hadCanonical && next.editable != original.editable))
                return MMS_E_STALE;
            bool tooLong = false; auto text = FormatFields(next, tooLong);
            if (text.size() != windows.size() || text.empty()) return MMS_E_STALE;
            struct Field {
                HWND window;
                std::wstring before, after, scratch;
                DWORD begin = 0, end = 0;
                bool readonly = false;
            };
            std::vector<Field> prepared; prepared.reserve(windows.size());
            for (size_t index = 0; index < windows.size(); ++index) {
                Field field; field.window = windows[index]; field.after = std::move(text[index]);
                const int length = GetWindowTextLengthW(field.window);
                if (length < 0 || size_t(length) > UiPropertyDraft::kMaxFieldBytes) return MMS_E_ARGUMENT;
                field.before.resize(size_t(length) + 1);
                const int read = GetWindowTextW(field.window, field.before.data(), int(field.before.size()));
                if (read != length) return MMS_E_STALE;
                field.before.resize(size_t(read));
                field.scratch.resize(std::max(field.before.size(), field.after.size()) + 1);
                SendMessageW(field.window, EM_GETSEL, reinterpret_cast<WPARAM>(&field.begin), reinterpret_cast<LPARAM>(&field.end));
                field.readonly = (GetWindowLongPtrW(field.window, GWL_STYLE) & ES_READONLY) != 0;
                prepared.push_back(std::move(field));
            }
            if (const auto result = validate()) return result;
            // The active session owns its raw fields, including invalid or IME
            // text. DATA advances only the latest canonical cache in that case.
            if (active) {
                if (!next.editable || !StateEditable()) return MMS_E_STALE;
                if (const auto result = validate()) return result;
                const auto revision = next.sceneRevision;
                snapshot = std::move(next); ++displayGeneration; sourceRevision = revision; return MMS_OK;
            }
            const auto sameText = [](Field& field, const std::wstring& expected) noexcept {
                const int length = GetWindowTextLengthW(field.window);
                if (length < 0 || size_t(length) != expected.size()) return false;
                const int read = GetWindowTextW(field.window, field.scratch.data(), int(field.scratch.size()));
                return read == length && std::wstring_view(field.scratch.data(), size_t(read)) == expected;
            };
            for (auto& field : prepared) if (!sameText(field, field.before)) return MMS_E_STALE;
            if (const auto result = validate()) return result;
            Flag writing(updating);
            size_t attempted = 0;
            const bool readonly = EffectiveReadOnly(!next.editable || tooLong);
            auto result = Boundary([&]() -> MmsResult {
                for (auto& field : prepared) {
                    if (!current()) return MMS_E_STALE;
                    ++attempted;
                    const bool changed = field.before != field.after;
                    const bool style = field.readonly != readonly;
                    if (changed && !SetWindowTextW(field.window, field.after.c_str())) return MMS_E_FAILED;
                    if (style && !SendMessageW(field.window, EM_SETREADONLY, readonly ? TRUE : FALSE, 0)) return MMS_E_FAILED;
                    if (changed || style) SendMessageW(field.window, EM_SETSEL, field.begin, field.end);
                    if (!current()) return MMS_E_STALE;
                    if (!sameText(field, field.after)) return MMS_E_FAILED;
                }
                if (const auto result = validate()) return result;
                for (auto& field : prepared) if (!sameText(field, field.after)) return MMS_E_STALE;
                return current() ? MMS_OK : MMS_E_STALE;
            });
            if (result) {
                // Every rollback caption/selection was allocated before the
                // first WM_SETTEXT. Never overwrite a newer reentrant draft.
                bool restored = current();
                if (restored) for (size_t index = attempted; index != 0;) {
                    auto& field = prepared[--index];
                    if (!sameText(field, field.before) && !SetWindowTextW(field.window, field.before.c_str())) restored = false;
                    if (bool(GetWindowLongPtrW(field.window, GWL_STYLE) & ES_READONLY) != field.readonly &&
                        !SendMessageW(field.window, EM_SETREADONLY, field.readonly ? TRUE : FALSE, 0)) restored = false;
                    SendMessageW(field.window, EM_SETSEL, field.begin, field.end);
                    if (!current() || !sameText(field, field.before)) { restored = false; break; }
                }
                if (!restored) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); return MMS_E_FAILED; }
                return result;
            }
            const auto revision = next.sceneRevision;
            snapshot = std::move(next); haveSnapshot = true; oversized = tooLong; ++displayGeneration; sourceRevision = revision;
            return MMS_OK;
        });
    }
    MmsResult ApplyStateContent(const std::wstring& caption, bool enabled, bool readOnly, bool shown,
        const std::function<MmsResult()>& revalidate) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        Work work(*this);
        if (destroying || !window || !label || !snapshot.property || !snapshot.element) return MMS_E_STALE;
        if (applyingState || refreshing || readingBack || updating) return MMS_E_BUSY;
        Flag applying(applyingState);
        return Boundary([&]() -> MmsResult {
            const auto binding = snapshot;
            const HWND root = window, target = label;
            const auto windows = fields;
            auto generation = displayGeneration;
            auto requested = intent;
            const auto current = [&]() noexcept {
                if (destroying || window != root || label != target || fields != windows ||
                    displayGeneration != generation || intent != requested || !IsWindow(root) || !IsWindow(target) ||
                    GetParent(target) != root || !registry.IsPropertyInputCurrent(binding)) return false;
                for (HWND field : windows) if (!IsWindow(field) || GetParent(field) != root) return false;
                return true;
            };
            const auto validate = [&]() -> MmsResult {
                if (!current()) return MMS_E_STALE;
                if (revalidate) if (const auto result = revalidate()) return result;
                return current() ? MMS_OK : MMS_E_STALE;
            };
            if (const auto result = validate()) return result;
            const auto read = [&](HWND field) {
                const int length = GetWindowTextLengthW(field);
                if (length < 0 || size_t(length) > UiPropertyDraft::kMaxFieldBytes) throw std::invalid_argument("property state caption extent");
                std::wstring text(size_t(length) + 1, L'\0');
                if (GetWindowTextW(field, text.data(), int(text.size())) != length) throw std::runtime_error("property state caption read");
                text.resize(size_t(length)); return text;
            };
            auto beforeLabel = read(target); auto nextLabel = caption;
            std::wstring labelScratch(std::max(beforeLabel.size(), nextLabel.size()) + 1, L'\0');
            const bool cancelling = (!enabled || readOnly || !shown) && (draft.Active() || draft.Composing() || intent);
            UiPropertyDraft savedDraft;
            PropertyInputSnapshot savedOrigin;
            std::shared_ptr<std::atomic<bool>> restoredIntent;
            const auto savedInvalid = invalid; const bool savedImeEnter = imeEnter;
            if (cancelling) {
                savedDraft = draft; savedOrigin = draftOrigin;
                if (draft.Active()) restoredIntent = std::make_shared<std::atomic<bool>>(true);
            }
            bool tooLong = oversized;
            std::vector<std::wstring> canonical;
            if (cancelling && haveSnapshot) canonical = FormatFields(snapshot, tooLong);
            if (cancelling && haveSnapshot && canonical.size() != windows.size()) return MMS_E_STALE;
            const bool readonly = !haveSnapshot || !snapshot.editable || tooLong || readOnly || !enabled || !shown;
            struct Field {
                HWND window; bool readonly, text = false;
                std::wstring before, after, scratch;
                DWORD begin = 0, end = 0;
            };
            std::vector<Field> changes; changes.reserve(windows.size());
            for (size_t n = 0; n < windows.size(); ++n) {
                const bool previous = (GetWindowLongPtrW(windows[n], GWL_STYLE) & ES_READONLY) != 0;
                if (!cancelling && previous == readonly) continue;
                Field field{windows[n], previous};
                if (cancelling && haveSnapshot) {
                    field.text = true; field.before = read(field.window); field.after = std::move(canonical[n]);
                    field.scratch.resize(std::max(field.before.size(), field.after.size()) + 1);
                }
                SendMessageW(field.window, EM_GETSEL, reinterpret_cast<WPARAM>(&field.begin), reinterpret_cast<LPARAM>(&field.end));
                if (const auto result = validate()) return result;
                changes.push_back(std::move(field));
            }
            if (const auto result = validate()) return result;
            const auto same = [](HWND field, const std::wstring& text, std::wstring& scratch) noexcept {
                const int length = GetWindowTextLengthW(field);
                return length >= 0 && size_t(length) == text.size() &&
                    GetWindowTextW(field, scratch.data(), int(scratch.size())) == length &&
                    std::wstring_view(scratch.data(), size_t(length)) == text;
            };
            const auto write = [&](HWND field, const std::wstring& text, std::wstring& scratch) -> MmsResult {
                const auto written = SendMessageW(field, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
                if (const auto result = validate()) return result;
                if (!written) return MMS_E_FAILED;
                return same(field, text, scratch) ? validate() : MMS_E_FAILED;
            };
            Flag writing(updating);
            bool labelAttempted = false, cancelled = false;
            size_t attempted = 0;
            const auto result = Boundary([&]() -> MmsResult {
                if (beforeLabel != nextLabel) {
                    labelAttempted = true;
                    if (const auto result = write(target, nextLabel, labelScratch)) return result;
                }
                // Revoke the old request before any native operation can
                // transfer focus or end IME composition. A rollback can create
                // a fresh intent, but can never resurrect that old request.
                if (cancelling) { ClearDraft(); generation = displayGeneration; requested = intent; cancelled = true; }
                for (auto& field : changes) {
                    ++attempted;
                    if (field.text && field.before != field.after)
                        if (const auto result = write(field.window, field.after, field.scratch)) return result;
                    if (field.readonly != readonly) {
                        const auto written = SendMessageW(field.window, EM_SETREADONLY, readonly ? TRUE : FALSE, 0);
                        if (const auto result = validate()) return result;
                        if (!written) return MMS_E_FAILED;
                        if (bool(GetWindowLongPtrW(field.window, GWL_STYLE) & ES_READONLY) != readonly) return MMS_E_FAILED;
                    }
                }
                for (auto& field : changes) {
                    if ((field.text && !same(field.window, field.after, field.scratch)) ||
                        bool(GetWindowLongPtrW(field.window, GWL_STYLE) & ES_READONLY) != readonly) return MMS_E_FAILED;
                    if (const auto result = validate()) return result;
                }
                if (!same(target, nextLabel, labelScratch)) return MMS_E_FAILED;
                return validate();
            });
            if (result) {
                bool restored = current();
                if (restored) for (size_t n = attempted; n;) {
                    auto& field = changes[--n];
                    if (field.text && !same(field.window, field.before, field.scratch)) {
                        if (!same(field.window, field.after, field.scratch) ||
                            !SendMessageW(field.window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(field.before.c_str()))) { restored = false; break; }
                    }
                    if (bool(GetWindowLongPtrW(field.window, GWL_STYLE) & ES_READONLY) != field.readonly &&
                        !SendMessageW(field.window, EM_SETREADONLY, field.readonly ? TRUE : FALSE, 0)) { restored = false; break; }
                    SendMessageW(field.window, EM_SETSEL, field.begin, field.end);
                    if (!current() || (field.text && !same(field.window, field.before, field.scratch)) ||
                        bool(GetWindowLongPtrW(field.window, GWL_STYLE) & ES_READONLY) != field.readonly) { restored = false; break; }
                }
                if (restored && labelAttempted && !same(target, beforeLabel, labelScratch)) {
                    restored = SendMessageW(target, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(beforeLabel.c_str())) &&
                        current() && same(target, beforeLabel, labelScratch);
                }
                if (!restored) { registry.SuspendGeometry(); registry.Refresh().SuspendTargets(); return MMS_E_FAILED; }
                if (cancelled) {
                    draft = std::move(savedDraft); draftOrigin = std::move(savedOrigin); intent = std::move(restoredIntent);
                    invalid = savedInvalid; imeEnter = savedImeEnter; focusQueued = false; ++focusSerial; ++displayGeneration;
                }
                return result;
            }
            stateLabel.swap(nextLabel); haveState = true;
            stateEnabled = enabled; stateReadOnly = readOnly; stateVisible = shown; oversized = tooLong;
            ++displayGeneration;
            return MMS_OK;
        });
    }
    bool Start() {
        if (destroying || updating || applyingState || layoutSuspended || !StateEditable() || !haveSnapshot || !snapshot.editable || oversized || !visible || !IsWindowEnabled(window)) return false;
        if (draft.Active()) return true;
        const auto result = Boundary([&]() -> MmsResult {
            auto origin = snapshot;
            auto next = std::make_shared<std::atomic<bool>>(true);
            if (const auto begun = draft.Begin(snapshot.value.View(), snapshot.sceneRevision)) return begun;
            draftOrigin = std::move(origin); intent = std::move(next); ++displayGeneration; ++layoutSuspendSerial; return MMS_OK;
        });
        if (result) Report(result);
        return !result;
    }
    void NewInputIntent() noexcept {
        ++displayGeneration; ++layoutSuspendSerial;
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
        invalid[index] = result != MMS_OK; ++displayGeneration;
    }
    void Submit() {
        Work guard(*this);
        if (destroying || submitting || updating || applyingState) return;
        // Effective layout changes precede the queued native refresh. A blur
        // already in the queue must observe that change before it can edit.
        if (SuspendHiddenLayout()) return;
        if (!visible || layoutSuspended || !StateEditable() || !draft.Active() || draft.Composing()) return;
        Flag submission(submitting);
        if (!intent || !intent->load(std::memory_order_relaxed)) NewInputIntent();
        const auto request = intent;
        if (!CurrentIntent(request)) return;
        // A read callback can pump messages that refresh, hide or cancel this
        // view. Keep the original broker input independent of mutable members.
        auto origin = draftOrigin; origin.intent = request;
        for (HWND field : fields) Changed(field, false);
        for (bool failure : invalid) if (failure) { Report(MMS_E_ARGUMENT); return; }
        PropertyInputSnapshot current;
        auto result = registry.CapturePropertyInput(elementId, current);
        if (SuspendHiddenLayout()) return;
        if (!CurrentIntent(request)) { Report(MMS_E_STALE); return; }
        if (!result && (current.property != origin.property || current.element != origin.element)) result = MMS_E_STALE;
        UiPropertyValue candidate; bool changed = false;
        if (!result) result = draft.Prepare(current.value.View(), current.sceneRevision, candidate, changed);
        if (SuspendHiddenLayout()) return;
        if (!CurrentIntent(request)) { Report(MMS_E_STALE); return; }
        if (!result && changed) result = registry.CommitPropertyInput(origin, candidate.View());
        if (result && SuspendHiddenLayout()) return;
        // Escape/hide may already have cancelled, or a reentrant EN_CHANGE
        // may own a replacement draft. Neither belongs to this old request.
        if (!CurrentIntent(request)) { if (result) Report(result); return; }
        if (!result) { ClearDraft(); Refresh(dpi); return; }
        if (result == MMS_E_STALE || result == MMS_E_NOT_FOUND || result == MMS_E_NOT_READY) { ClearDraft(); Refresh(dpi); }
        Report(result);
    }
    void QueueFocus() noexcept {
        if (!destroying && !updating && visible && !layoutSuspended && draft.Active() && !focusQueued) {
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
            else if (message == WM_SHOWWINDOW) {
                self.visible = wp != 0;
                if (!wp && !self.layoutSuspended && (self.draft.Active() || self.draft.Composing() || self.intent) &&
                    !self.SuspendHiddenLayout()) self.CancelInternal();
            }
            else if (message == WM_ENABLE) {
                if (!wp && (self.draft.Active() || self.draft.Composing() || self.intent)) self.CancelInternal();
                for (HWND field : self.fields) EnableWindow(field, wp != 0);
            }
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
        if (message == WM_NCDESTROY) { ++self.displayGeneration; RemoveWindowSubclass(hwnd, FieldProc, id); return DefSubclassProc(hwnd, message, wp, lp); }
        if (self.destroying) return DefSubclassProc(hwnd, message, wp, lp);
        if (message == WM_GETDLGCODE) {
            const auto* key = reinterpret_cast<const MSG*>(lp);
            const bool wanted = wp == VK_RETURN || wp == VK_ESCAPE || (key && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE));
            const bool compositionTab = self.draft.Composing() && (wp == VK_TAB || (key && key->wParam == VK_TAB));
            return DefSubclassProc(hwnd, message, wp, lp) | (wanted || compositionTab ? DLGC_WANTMESSAGE : 0) |
                (compositionTab ? DLGC_WANTTAB : 0);
        }
        if (message == WM_IME_ENDCOMPOSITION) {
            const auto root = self.window;
            const auto serial = self.layoutSuspendSerial;
            const auto answer = DefSubclassProc(hwnd, message, wp, lp);
            if (self.destroying || self.window != root || self.layoutSuspendSerial != serial || !IsWindow(hwnd) ||
                std::find(self.fields.begin(), self.fields.end(), hwnd) == self.fields.end()) return answer;
            if (self.draft.Active() || self.draft.Composing()) {
                self.imeEnter = self.imeEnter || (GetKeyState(VK_RETURN) & 0x8000) != 0;
                self.draft.SetComposing(false); ++self.displayGeneration; self.QueueFocus();
            }
            return answer;
        }
        bool handled = false;
        const auto result = Boundary([&]() -> MmsResult {
            if (message == WM_IME_STARTCOMPOSITION) {
                if (self.Start()) {
                    if (self.submitting) self.NewInputIntent();
                    self.draft.SetComposing(true); ++self.displayGeneration;
                }
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
MmsResult NativePropertyInput::Readback(const std::function<MmsResult()>& revalidate, uint64_t& sourceRevision) {
    return impl_->Readback(revalidate, sourceRevision);
}
MmsResult NativePropertyInput::ApplyStateContent(const std::wstring& label, bool enabled, bool readOnly,
    bool visible, const std::function<MmsResult()>& revalidate) {
    return impl_->ApplyStateContent(label, enabled, readOnly, visible, revalidate);
}
const std::wstring& NativePropertyInput::DisplayLabel() const noexcept { return impl_->DisplayLabel(); }
MmsResult NativePropertyInput::RepairLabel() {
    Impl::Work work(*impl_);
    if (impl_->destroying || !impl_->window || !impl_->label) return MMS_E_STALE;
    return Boundary([&]() -> MmsResult { const auto caption = impl_->DisplayLabel(); Impl::Caption(impl_->label, caption); return MMS_OK; });
}
void NativePropertyInput::Cancel() noexcept { Impl::Work work(*impl_); impl_->CancelInternal(); }
bool NativePropertyInput::LayoutSuspended() const noexcept { return impl_->layoutSuspended; }
void NativePropertyInput::LayoutVisible(bool visible, bool preserveDraft) noexcept {
    Impl::Work work(*impl_); impl_->visible = visible;
    if (!visible && preserveDraft) impl_->SuspendLayoutDraft();
    else {
        if (impl_->layoutSuspended) ++impl_->layoutSuspendSerial;
        impl_->layoutSuspended = false;
        if (!visible) impl_->CancelInternal();
    }
}

}
