#pragma once

// C++17 conveniences for the existing MMS C ABI. There is no separate UI
// registry: every reference below contains the host's original kind/handle.
// All owning objects and their borrowed services stay on the host UI thread.
// Keep host/plugin contexts and callbacks alive through their host lifecycle.
// Close() only unregisters items: outstanding leases may still need release
// callbacks. The plugin normally retains its context until the host calls
// destroy, after its registrations and every outstanding lease are retired.
#include <mms/ui.h>
#include <mms/surface.h>
#include <mms/layout.h>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mms::sdk {

template<class T> constexpr MmsHeader Header() noexcept {
    static_assert(sizeof(T) <= UINT32_MAX, "MMS structure size");
    return {static_cast<std::uint32_t>(sizeof(T)), MMS_ABI_MAJOR, MMS_ABI_MINOR};
}

// Borrowed UTF-8 bytes. Registration copies strings in the host; Text itself
// neither allocates nor extends the input's lifetime.
inline MmsString Text(std::string_view value) noexcept {
    return {value.data(), static_cast<std::uint64_t>(value.size())};
}

namespace detail {
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
template<class T> bool TableHeader(const T& table) noexcept {
    // A newer minor can append fields. The host still receives sizeof(T) as
    // outBytes and must copy only that prefix; this wrapper never reads a tail.
    return table.h.size >= sizeof(T) && table.h.major == 1;
}
struct ActiveCall {
    bool& active;
    explicit ActiveCall(bool& value) noexcept : active(value) { active = true; }
    ~ActiveCall() { active = false; }
};
} // namespace detail

class UiServices {
    MmsHostV1 host_{};
    MmsUiApiV1 ui_{};
    MmsUiSurfaceApiV1 surface_{};
    MmsUiLayoutApiV1 layout_{};
    std::thread::id thread_{};
    bool ready_ = false, surfaceReady_ = false, layoutReady_ = false;

    template<class T> MmsResult QueryTable(std::string_view name, T& table) const noexcept {
        return detail::Call([&]() -> MmsResult {
            T candidate{};
            const auto result = host_.queryService(host_.context, Text(name), 1, 0,
                static_cast<std::uint32_t>(sizeof(T)), &candidate);
            if (result != MMS_OK) return result;
            if (!detail::TableHeader(candidate)) return MMS_E_VERSION;
            table = candidate;
            return MMS_OK;
        });
    }
public:
    // Only mms.ui is required here. Optional services are queried on demand.
    // A failed query leaves out unchanged; no service table owns registrations.
    static MmsResult Query(const MmsHostV1& host, UiServices& out) noexcept {
        if (host.h.size < sizeof(MmsHostV1) || host.h.major != 1)
            return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        UiServices candidate;
        candidate.host_ = host;
        const auto result = candidate.QueryTable("mms.ui", candidate.ui_);
        if (result != MMS_OK) return result;
        const auto& ui = candidate.ui_;
        if (!ui.registerCommand || !ui.registerElement || !ui.registerProperty || !ui.unregisterItem)
            return MMS_E_UNSUPPORTED;
        candidate.thread_ = std::this_thread::get_id();
        candidate.ready_ = true;
        out = candidate;
        return MMS_OK;
    }
    bool ready() const noexcept { return ready_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    MmsResult RequireSurface() noexcept {
        if (const auto result = CheckThread()) return result;
        if (surfaceReady_) return MMS_OK;
        MmsUiSurfaceApiV1 candidate{};
        if (const auto result = QueryTable("mms.ui.surface", candidate)) return result;
        if (!candidate.registerSurface || !candidate.unregisterSurface) return MMS_E_UNSUPPORTED;
        surface_ = candidate; surfaceReady_ = true;
        return MMS_OK;
    }
    MmsResult RequireLayout() noexcept {
        if (const auto result = CheckThread()) return result;
        if (layoutReady_) return MMS_OK;
        MmsUiLayoutApiV1 candidate{};
        if (const auto result = QueryTable("mms.ui.layout", candidate)) return result;
        if (!candidate.setDefaultHints) return MMS_E_UNSUPPORTED;
        layout_ = candidate; layoutReady_ = true;
        return MMS_OK;
    }
    // Borrowed original tables. Only functions used by this header are checked;
    // check other optional function pointers before direct low-level use.
    const MmsUiApiV1& ui() const noexcept { return ui_; }
    const MmsUiSurfaceApiV1& surface() const noexcept { return surface_; }
    const MmsUiLayoutApiV1& layout() const noexcept { return layout_; }
};

enum class RegistrationKind { Command, Element, Property, Surface };
struct RegistrationRef {
    RegistrationKind kind = RegistrationKind::Element;
    MmsHandle handle = 0;
    explicit operator bool() const noexcept { return handle != 0; }
};
inline bool operator==(RegistrationRef a, RegistrationRef b) noexcept {
    return a.kind == b.kind && a.handle == b.handle;
}
inline bool operator!=(RegistrationRef a, RegistrationRef b) noexcept { return !(a == b); }

class RegistrationBundle {
    UiServices services_;
    std::vector<RegistrationRef> registrations_;
    bool active_ = false;

    template<class F> MmsResult Add(RegistrationKind kind, RegistrationRef* out, F&& call) noexcept {
        if (out) *out = {};
        if (active_) return MMS_E_BUSY;
        if (const auto result = services_.CheckThread()) return result;
        detail::ActiveCall guard(active_);
        return detail::Call([&]() -> MmsResult {
            // Reserve and append BEFORE the C call. A successful registration
            // must never be orphaned by a later vector allocation failure.
            if (registrations_.size() == registrations_.max_size()) return MMS_E_MEMORY;
            registrations_.reserve(registrations_.size() + 1);
            registrations_.push_back({kind, 0});
            MmsHandle handle = 0;
            const auto result = detail::Call([&] { return call(&handle); });
            if (result != MMS_OK || !handle) {
                registrations_.pop_back();
                return result != MMS_OK ? result : MMS_E_FAILED;
            }
            registrations_.back().handle = handle;
            if (out) *out = registrations_.back();
            return MMS_OK;
        });
    }
    MmsResult Remove(std::size_t index) noexcept {
        const auto reference = registrations_[index];
        const auto result = detail::Call([&] {
            return reference.kind == RegistrationKind::Surface
                ? services_.surface().unregisterSurface(services_.surface().context, reference.handle)
                : services_.ui().unregisterItem(services_.ui().context, reference.handle);
        });
        if (result != MMS_OK && result != MMS_E_NOT_FOUND) return result;
        registrations_.erase(registrations_.begin() + static_cast<std::ptrdiff_t>(index));
        return MMS_OK;
    }
public:
    explicit RegistrationBundle(UiServices services) noexcept : services_(std::move(services)) {}
    RegistrationBundle(const RegistrationBundle&) = delete;
    RegistrationBundle& operator=(const RegistrationBundle&) = delete;
    RegistrationBundle(RegistrationBundle&& other) noexcept
        : services_(std::move(other.services_)), registrations_(std::move(other.registrations_)) {
        // Moving is an ownership operation on the same UI thread, outside a
        // registration/Close callback. Callback context addresses never move.
        other.registrations_.clear();
    }
    RegistrationBundle& operator=(RegistrationBundle&&) = delete;
    // Best effort only. In stop(), call Close(), return its failure and retain
    // this object/context for retry. Destruction cannot report a failed Close.
    ~RegistrationBundle() { (void)Close(); }

    const UiServices& services() const noexcept { return services_; }
    std::size_t size() const noexcept { return registrations_.size(); }
    RegistrationRef get(std::size_t index) const noexcept {
        return index < registrations_.size() ? registrations_[index] : RegistrationRef{};
    }
    bool owns(RegistrationRef reference) const noexcept {
        if (!reference) return false;
        for (const auto& value : registrations_) if (value == reference) return true;
        return false;
    }
    // Explicitly transfer the ORIGINAL registration. The caller assumes its
    // cleanup and dependency ordering. Failure returns an empty reference.
    RegistrationRef detach(RegistrationRef reference) noexcept {
        if (active_ || services_.CheckThread() != MMS_OK) return {};
        for (std::size_t i = 0; i < registrations_.size(); ++i) if (registrations_[i] == reference) {
            registrations_.erase(registrations_.begin() + static_cast<std::ptrdiff_t>(i));
            return reference;
        }
        return {};
    }
    MmsResult AddCommand(const MmsCommandDescV1& desc, RegistrationRef* out = nullptr) noexcept {
        return Add(RegistrationKind::Command, out, [&](MmsHandle* handle) {
            return services_.ui().registerCommand(services_.ui().context, &desc, handle);
        });
    }
    MmsResult AddElement(const MmsUiElementV1& desc, RegistrationRef* out = nullptr) noexcept {
        return Add(RegistrationKind::Element, out, [&](MmsHandle* handle) {
            return services_.ui().registerElement(services_.ui().context, &desc, handle);
        });
    }
    MmsResult AddProperty(const MmsPropertyDescV1& desc, RegistrationRef* out = nullptr) noexcept {
        return Add(RegistrationKind::Property, out, [&](MmsHandle* handle) {
            return services_.ui().registerProperty(services_.ui().context, &desc, handle);
        });
    }
    MmsResult AddSurface(const MmsUiSurfaceDescV1& desc, RegistrationRef* out = nullptr) noexcept {
        if (out) *out = {};
        if (active_) return MMS_E_BUSY;
        if (const auto result = services_.RequireSurface()) return result;
        return Add(RegistrationKind::Surface, out, [&](MmsHandle* handle) {
            return services_.surface().registerSurface(services_.surface().context, &desc, handle);
        });
    }
    MmsResult SetDefaultHints(std::string_view id, const MmsUiLayoutHintsV1& hints) noexcept {
        if (active_) return MMS_E_BUSY;
        if (const auto result = services_.RequireLayout()) return result;
        detail::ActiveCall guard(active_);
        return detail::Call([&] {
            return services_.layout().setDefaultHints(services_.layout().context, Text(id), &hints);
        });
    }
    MmsResult Close() noexcept {
        if (active_) return MMS_E_BUSY;
        if (registrations_.empty()) return MMS_OK;
        if (const auto result = services_.CheckThread()) return result;
        detail::ActiveCall guard(active_);
        // Direct C ABI edits may reparent an older leaf into a newer group.
        // Retry BUSY elements after removing other leaves instead of becoming
        // permanently stuck on registration order. External dependencies keep
        // the remaining handles alive and produce BUSY when no progress occurs.
        for (;;) {
            bool elements = false, progress = false;
            for (std::size_t i = registrations_.size(); i != 0;) {
                --i;
                if (registrations_[i].kind != RegistrationKind::Element) continue;
                elements = true;
                const auto result = Remove(i);
                if (result == MMS_OK) progress = true;
                else if (result != MMS_E_BUSY) return result;
            }
            if (!elements) break;
            if (!progress) return MMS_E_BUSY;
        }
        // No command, property or surface dependency is released while any
        // owned UI element remains. Stop at failure and keep the rest for retry.
        for (std::size_t i = registrations_.size(); i != 0;) {
            --i;
            if (registrations_[i].kind != RegistrationKind::Surface)
                if (const auto result = Remove(i)) return result;
        }
        for (std::size_t i = registrations_.size(); i != 0;) {
            --i;
            if (const auto result = Remove(i)) return result;
        }
        return MMS_OK;
    }
    MmsResult reset() noexcept { return Close(); }
};

struct UiPlacementSpec {
    std::string_view id, parent, label;
    // Menu items do not support layout hints. Toolbar button hints are copied
    // synchronously by the host; they need only live through Register().
    const MmsUiLayoutHintsV1* hints = nullptr;
};
struct UiGroupSpec {
    std::string_view id, parent, label;
    const MmsUiLayoutHintsV1* hints = nullptr;
};
struct CommandUiSpec {
    std::string_view id, owner, label, description;
    void* context = nullptr;
    decltype(MmsCommandDescV1::getState) getState = nullptr;
    decltype(MmsCommandDescV1::invoke) invoke = nullptr;
    std::uint32_t flags = MMS_COMMAND_INVOKE_ONLY;
    UiPlacementSpec menu, button;
    UiGroupSpec group;
    // Applies to toolbar button/group hints only. Menus retain the C ABI's
    // registration order. A nonzero value overrides hints.order.
    std::int32_t order = 0;
};

class CommandUi {
    RegistrationBundle registrations_;
    RegistrationRef command_{}, menu_{}, button_{}, group_{};
    MmsResult cleanupResult_ = MMS_OK;
    RegistrationRef Borrow(RegistrationRef reference) const noexcept {
        return registrations_.owns(reference) ? reference : RegistrationRef{};
    }
    MmsResult Fail(MmsResult result) noexcept {
        cleanupResult_ = registrations_.Close();
        return result;
    }
    MmsResult Hints(std::string_view id, const MmsUiLayoutHintsV1* hints, std::int32_t order) noexcept {
        if (!hints && !order) return MMS_OK;
        auto value = hints ? *hints : MmsUiLayoutHintsV1{};
        if (!hints) value.h = Header<MmsUiLayoutHintsV1>();
        if (order) value.order = order;
        return registrations_.SetDefaultHints(id, value);
    }
public:
    explicit CommandUi(UiServices services) noexcept : registrations_(std::move(services)) {}
    CommandUi(const CommandUi&) = delete;
    CommandUi& operator=(const CommandUi&) = delete;
    CommandUi(CommandUi&&) noexcept = default;
    CommandUi& operator=(CommandUi&&) = delete;

    // Composition is not an atomic host transaction: registrations may be
    // observed between calls. On failure, the result is the original error;
    // cleanup_result() separately reports rollback failure. A blocked rollback
    // remains owned here, and Register() cannot overwrite it. Keep the caller's
    // callback context alive and explicitly retry Close() before plugin stop.
    // No INPUT, overflow menu, new anchor, shortcut or refresh API is invented.
    MmsResult Register(const CommandUiSpec& spec) noexcept {
        if (registrations_.size()) return MMS_E_BUSY;
        if (const auto result = registrations_.services().CheckThread()) return result;
        if (spec.id.empty() || spec.owner.empty() || !spec.getState || !spec.invoke ||
            spec.menu.hints || (!spec.group.id.empty() && spec.button.id.empty())) return MMS_E_ARGUMENT;
        cleanupResult_ = MMS_OK;
        command_ = menu_ = button_ = group_ = {};
        MmsCommandDescV1 command{};
        command.info.h = Header<MmsCommandDescV1>();
        command.info.id = Text(spec.id); command.info.ownerPlugin = Text(spec.owner);
        command.info.label = Text(spec.label); command.info.description = Text(spec.description);
        command.info.flags = spec.flags; command.context = spec.context;
        command.getState = spec.getState; command.invoke = spec.invoke;
        if (const auto result = registrations_.AddCommand(command, &command_)) return Fail(result);

        if (!spec.group.id.empty()) {
            MmsUiElementV1 group{}; group.h = Header<MmsUiElementV1>();
            group.id = Text(spec.group.id); group.ownerPlugin = Text(spec.owner);
            group.parentId = Text(spec.group.parent.empty() ? "mms.toolbar.main.extensions" : spec.group.parent);
            group.label = Text(spec.group.label.empty() ? spec.label : spec.group.label);
            group.kind = MMS_UI_GROUP;
            group.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_CONTAINER | MMS_UI_LAYOUT;
            if (const auto result = registrations_.AddElement(group, &group_)) return Fail(result);
            if (const auto result = Hints(spec.group.id, spec.group.hints, spec.order)) return Fail(result);
        }
        if (!spec.menu.id.empty()) {
            MmsUiElementV1 menu{}; menu.h = Header<MmsUiElementV1>();
            menu.id = Text(spec.menu.id); menu.ownerPlugin = Text(spec.owner);
            menu.parentId = Text(spec.menu.parent.empty() ? "mms.menu.plugins" : spec.menu.parent);
            menu.label = Text(spec.menu.label.empty() ? spec.label : spec.menu.label);
            menu.commandId = Text(spec.id); menu.kind = MMS_UI_MENU_ITEM;
            menu.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_SHORTCUT;
            if (const auto result = registrations_.AddElement(menu, &menu_)) return Fail(result);
        }
        if (!spec.button.id.empty()) {
            MmsUiElementV1 button{}; button.h = Header<MmsUiElementV1>();
            button.id = Text(spec.button.id); button.ownerPlugin = Text(spec.owner);
            const auto defaultParent = spec.group.id.empty() ? std::string_view("mms.toolbar.main.extensions") : spec.group.id;
            button.parentId = Text(spec.button.parent.empty() ? defaultParent : spec.button.parent);
            button.label = Text(spec.button.label.empty() ? spec.label : spec.button.label);
            button.commandId = Text(spec.id); button.kind = MMS_UI_BUTTON;
            button.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_SHORTCUT | MMS_UI_LAYOUT;
            if (const auto result = registrations_.AddElement(button, &button_)) return Fail(result);
            if (const auto result = Hints(spec.button.id, spec.button.hints, spec.order)) return Fail(result);
        }
        return MMS_OK;
    }
    MmsResult Close() noexcept { return cleanupResult_ = registrations_.Close(); }
    MmsResult cleanup_result() const noexcept { return cleanupResult_; }
    RegistrationRef command() const noexcept { return Borrow(command_); }
    RegistrationRef menu() const noexcept { return Borrow(menu_); }
    RegistrationRef button() const noexcept { return Borrow(button_); }
    RegistrationRef group() const noexcept { return Borrow(group_); }
    RegistrationBundle& registrations() noexcept { return registrations_; }
    const RegistrationBundle& registrations() const noexcept { return registrations_; }
};

} // namespace mms::sdk
