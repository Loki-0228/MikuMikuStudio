#include "native_ui_catalog.hpp"
#include "native_menu_snapshot.hpp"
#include "ui_registry.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "../window/ui_controls.inc"
#include <commctrl.h>
#include <array>
#include <cwchar>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
constexpr char kMenuRoot[] = "mms.catalog.main.menu";
constexpr char kControlsRoot[] = "mms.catalog.main.controls";

MmsResult MenuResult(native_menu_snapshot::Status status) noexcept {
    using Status = native_menu_snapshot::Status;
    switch (status) {
    case Status::ok: return MMS_OK;
    case Status::invalid_argument: return MMS_E_ARGUMENT;
    case Status::invalid_window: return MMS_E_STALE;
    case Status::wrong_thread: return MMS_E_THREAD;
    case Status::limit: case Status::no_memory: return MMS_E_MEMORY;
    case Status::changed: return MMS_E_STALE;
    default: return MMS_E_FAILED;
    }
}

bool SameStructure(const native_menu_snapshot::Snapshot& a,
                   const native_menu_snapshot::Snapshot& b) noexcept {
    if (a.rootMenu != b.rootMenu || a.items.size() != b.items.size()) return false;
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        const auto& x = a.items[i]; const auto& y = b.items[i];
        if (x.menu != y.menu || x.subMenu != y.subMenu || x.position != y.position ||
            x.parent != y.parent || x.type != y.type || x.commandId != y.commandId) return false;
    }
    return true;
}

std::uint32_t WindowDpi(HWND window) noexcept {
    // The same coordinate-space rule as NativeUi::Impl::WindowDpi. In
    // particular, do not scale DPI-virtualized coordinates a second time.
    using Function = UINT(WINAPI*)(HWND);
    static const auto getDpi = reinterpret_cast<Function>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    std::uint32_t value = 96;
    if (getDpi) value = getDpi(window);
    else {
        const HDC dc = GetDC(window);
        if (dc) { value = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(window, dc); }
    }
    return value < 48 || value > 960 ? 96 : value;
}

bool OwnedWindow(HWND window, DWORD thread) noexcept {
    if (!window || !IsWindow(window)) return false;
    DWORD process = 0;
    return GetWindowThreadProcessId(window, &process) == thread && process == GetCurrentProcessId();
}

MmsResult Bounds(HWND window, HWND main, std::uint32_t dpi, MmsUiDirtyRectV1& out) noexcept {
    const WindowDpiScope scope(main);
    RECT rectangle{};
    if (!GetWindowRect(window, &rectangle)) return MMS_E_FAILED;
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(nullptr, main, reinterpret_cast<POINT*>(&rectangle), 2) &&
        GetLastError() != ERROR_SUCCESS) return MMS_E_FAILED;
    if (rectangle.right < rectangle.left || rectangle.bottom < rectangle.top) return MMS_E_FAILED;
    const double scale = 96.0 / dpi;
    out = {float(double(rectangle.left) * scale), float(double(rectangle.top) * scale),
        float((double(rectangle.right) - rectangle.left) * scale),
        float((double(rectangle.bottom) - rectangle.top) * scale)};
    return MMS_OK;
}

std::uint32_t ControlRole(const wchar_t* name) noexcept {
    if (!_wcsicmp(name, L"BUTTON")) return MMS_UI_CATALOG_ROLE_BUTTON;
    if (!_wcsicmp(name, L"EDIT")) return MMS_UI_CATALOG_ROLE_INPUT;
    if (!_wcsicmp(name, L"COMBOBOX")) return MMS_UI_CATALOG_ROLE_COMBO;
    if (!_wcsicmp(name, TRACKBAR_CLASSW)) return MMS_UI_CATALOG_ROLE_SLIDER;
    if (!_wcsicmp(name, L"STATIC")) return MMS_UI_CATALOG_ROLE_LABEL;
    if (!_wcsicmp(name, L"SCROLLBAR")) return MMS_UI_CATALOG_ROLE_SCROLLBAR;
    if (!_wcsicmp(name, WC_TABCONTROLW)) return MMS_UI_CATALOG_ROLE_TABS;
    return MMS_UI_CATALOG_ROLE_UNKNOWN;
}

std::uint32_t ControlKind(std::uint32_t role) noexcept {
    if (role == MMS_UI_CATALOG_ROLE_BUTTON) return MMS_UI_BUTTON;
    if (role == MMS_UI_CATALOG_ROLE_INPUT || role == MMS_UI_CATALOG_ROLE_COMBO ||
        role == MMS_UI_CATALOG_ROLE_SLIDER) return MMS_UI_INPUT;
    return MMS_UI_GROUP;
}

std::string StaticLabel(const ui::ControlSpec& spec) {
    if (spec.wtext) return Utf8(std::wstring_view(spec.wtext));
    if (!spec.atext || !*spec.atext) return {};
    // The generated ANSI creation table contains ASCII captions. Refuse an
    // unexpected encoding rather than publishing malformed UTF-8 or reading
    // the current text/value of an EDIT or COMBOBOX.
    std::string result(spec.atext);
    if (!std::all_of(result.begin(), result.end(), [](unsigned char c) { return c < 0x80; }))
        throw std::invalid_argument("native control static caption encoding");
    return result;
}

const char* SemanticLabel(int id) noexcept {
    switch (id) {
    case panel::kCurrentFrameEdit: return "Current frame";
    case panel::kFovSlider: case panel::kFovEdit: return "Camera field of view";
    case panel::kLightColorSliderR: case panel::kLightColorEditR: return "Light red";
    case panel::kLightColorSliderG: case panel::kLightColorEditG: return "Light green";
    case panel::kLightColorSliderB: case panel::kLightColorEditB: return "Light blue";
    case panel::kLightDirSliderX: case panel::kLightDirEditX: return "Light direction X";
    case panel::kLightDirSliderY: case panel::kLightDirEditY: return "Light direction Y";
    case panel::kLightDirSliderZ: case panel::kLightDirEditZ: return "Light direction Z";
    default: return nullptr;
    }
}

struct ExtraControl { int id; const char* label; };
constexpr ExtraControl kExtraControls[] = {
    {panel::kForcePasteButton, "Force paste"},
    {panel::kShadowFilterCheckbox, "Shadow filtering"},
    {int(curve_editor::kAccessoryCurve), "Accessory curve editor"},
    {int(curve_editor::kInterpolationCurve), "Interpolation curve editor"},
    {int(curve_editor::kMorphCurve), "Eyebrow curve editor"},
    // These lane IDs are declared by curve_host::kMorphCurveLane.
    {32904, "Eye curve editor"}, {32905, "Mouth curve editor"}, {32906, "Other morph curve editor"},
};
}

struct NativeUiCatalog::Impl {
    struct Observer {
        Impl* owner = nullptr;
        HWND window = nullptr;
        std::uint64_t instance = 0;
        bool main = false, installed = false;
        Observer(Impl& value, HWND target, std::uint64_t generation, bool root)
            : owner(&value), window(target), instance(generation), main(root) {}
        // If native subclass removal unexpectedly fails, keep the callback
        // context alive but disconnected until that window reaches NCDESTROY.
        // A failed cleanup must never leave a freed context in comctl32.
        static void Release(Observer* self) noexcept {
            if (self->installed && self->window) {
                DWORD_PTR value = 0;
                if (GetWindowSubclass(self->window, Procedure, reinterpret_cast<UINT_PTR>(self), &value) &&
                    value == reinterpret_cast<DWORD_PTR>(self) &&
                    !RemoveWindowSubclass(self->window, Procedure, reinterpret_cast<UINT_PTR>(self))) {
                    self->owner = nullptr;
                    return;
                }
            }
            delete self;
        }
        static std::shared_ptr<Observer> Create(Impl& owner, HWND window, std::uint64_t instance, bool main) {
            return std::shared_ptr<Observer>(new Observer(owner, window, instance, main), Release);
        }
        MmsResult Install() noexcept {
            if (!SetWindowSubclass(window, Procedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
                return MMS_E_FAILED;
            installed = true;
            return MMS_OK;
        }
        bool Current() const noexcept {
            if (!owner || !installed || !window || !OwnedWindow(window, owner->thread)) return false;
            DWORD_PTR value = 0;
            return GetWindowSubclass(window, Procedure, reinterpret_cast<UINT_PTR>(this), &value) &&
                value == reinterpret_cast<DWORD_PTR>(this);
        }
        static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR data) noexcept {
            auto* self = reinterpret_cast<Observer*>(data);
            if (message == WM_NCDESTROY) {
                RemoveWindowSubclass(window, Procedure, id);
                self->installed = false; self->window = nullptr;
                if (self->owner && self->main) {
                    self->owner->main = nullptr;
                    self->owner->registry.Catalog().Suspend();
                } else if (self->owner) self->owner->registry.Catalog().RetireInstance(self->instance);
                if (!self->owner) {
                    const auto result = DefSubclassProc(window, message, wp, lp);
                    delete self;
                    return result;
                }
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };

    UiRegistry& registry;
    const DWORD thread = GetCurrentThreadId();
    HWND main;
    std::shared_ptr<Observer> mainObserver;
    std::map<int, std::shared_ptr<Observer>> controls;
    native_menu_snapshot::Snapshot menu;
    bool haveMenu = false;
    std::uint64_t menuStructure = 0;
    std::vector<std::uint64_t> menuInstances;

    Impl(UiRegistry& value, HWND window) : registry(value), main(window) {
        if (!OwnedWindow(main, thread)) throw std::invalid_argument("native catalog owner window/thread");
        mainObserver = Observer::Create(*this, main, registry.NewPresentationInstance(), true);
        if (mainObserver->Install() != MMS_OK) throw std::runtime_error("native catalog owner lifetime observer");
    }
    ~Impl() {
        // The owning NativeUi adapter creates and destroys this module on its
        // UI thread. Detach observers before invalidating the published data.
        controls.clear(); mainObserver.reset();
        registry.Catalog().Suspend();
    }

    HostUiCatalogEntry Root(const char* id, const char* label, std::uint32_t scope) const {
        HostUiCatalogEntry entry;
        entry.id = id; entry.label = label; entry.scope = scope;
        entry.instance = mainObserver->instance;
        entry.knownFields = MMS_UI_CATALOG_LABEL | MMS_UI_CATALOG_PRESENTATION;
        return entry;
    }

    MmsResult CaptureControls(HostUiCatalogPublication& publication,
        std::map<int, std::shared_ptr<Observer>>& candidate, std::uint32_t dpi) {
        std::set<int> declared;
        std::uint32_t ordinal = 0;
        const auto add = [&](int id, std::string label) -> MmsResult {
            if (!declared.insert(id).second) return MMS_E_CONFLICT;
            const std::uint32_t order = ordinal++;
            const HWND window = GetDlgItem(main, id);
            if (!window) return MMS_OK; // A declared but absent control is not materialized.
            if (!OwnedWindow(window, thread) || GetParent(window) != main) return MMS_E_STALE;
            std::shared_ptr<Observer> observer;
            const auto previous = controls.find(id);
            if (previous != controls.end() && previous->second->window == window && previous->second->Current())
                observer = previous->second;
            else {
                observer = Observer::Create(*this, window, registry.NewPresentationInstance(), false);
                if (const auto result = observer->Install()) return result;
            }
            wchar_t name[256]{};
            const int length = GetClassNameW(window, name, int(std::size(name)));
            if (length <= 0 || length >= int(std::size(name)) - 1) return MMS_E_FAILED;
            HostUiCatalogEntry entry;
            entry.id = "mms.catalog.main.control." + std::to_string(id);
            entry.parentId = kControlsRoot; entry.label = std::move(label);
            entry.instance = observer->instance; entry.scope = MMS_UI_CATALOG_MAIN_CONTROLS;
            entry.ordinal = order; entry.role = ControlRole(name); entry.kind = ControlKind(entry.role);
            entry.knownFields = MMS_UI_CATALOG_LABEL | MMS_UI_CATALOG_ENABLED | MMS_UI_CATALOG_VISIBLE |
                MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS;
            if (IsWindowEnabled(window)) entry.state |= MMS_UI_CATALOG_STATE_ENABLED;
            if (IsWindowVisible(window)) entry.state |= MMS_UI_CATALOG_STATE_VISIBLE;
            if (const auto result = Bounds(window, main, dpi, entry.bounds)) return result;
            if (!observer->Current() || GetDlgItem(main, id) != window) return MMS_E_STALE;
            candidate.emplace(id, std::move(observer));
            publication.entries.push_back(std::move(entry));
            return MMS_OK;
        };
        for (const auto& spec : ui::kControls) {
            std::string label = StaticLabel(spec);
            if (label.empty()) if (const char* semantic = SemanticLabel(spec.id)) label = semantic;
            if (const auto result = add(spec.id, std::move(label))) return result;
        }
        for (const auto& extra : kExtraControls)
            if (const auto result = add(extra.id, extra.label)) return result;
        return MMS_OK;
    }

    MmsResult Observe(const std::vector<NativeCatalogMenuAlias>& aliases) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (!main || !mainObserver->Current()) return MMS_E_STALE;
        native_menu_snapshot::Snapshot captured;
        if (const auto result = MenuResult(native_menu_snapshot::Capture(main, {}, captured))) return result;
        const bool replaced = !haveMenu || !SameStructure(menu, captured);
        const std::uint64_t structure = replaced ? registry.NewPresentationInstance() : menuStructure;
        std::vector<std::uint64_t> instances;
        instances.reserve(captured.items.size());
        HostUiCatalogPublication publication;
        publication.availableScopes = MMS_UI_CATALOG_MAIN_MENU | MMS_UI_CATALOG_MAIN_CONTROLS;
        publication.coverageFlags = MMS_UI_CATALOG_MATERIALIZED_MENU | MMS_UI_CATALOG_DECLARED_CONTROLS |
            MMS_UI_CATALOG_INCOMPLETE_NATIVE;
        publication.entries.reserve(captured.items.size() + std::size(ui::kControls) + std::size(kExtraControls) + 2);
        publication.entries.push_back(Root(kMenuRoot, "Main menu (materialized)", MMS_UI_CATALOG_MAIN_MENU));
        publication.entries.push_back(Root(kControlsRoot, "Main controls (declared)", MMS_UI_CATALOG_MAIN_CONTROLS));
        std::vector<std::string> rowIds;
        rowIds.reserve(captured.items.size());
        std::set<MmsHandle> usedAliases;
        for (std::size_t i = 0; i < captured.items.size(); ++i) {
            const auto& item = captured.items[i];
            if (item.parent != UINT32_MAX && item.parent >= i) return MMS_E_FAILED;
            const NativeCatalogMenuAlias* alias = nullptr;
            for (const auto& possible : aliases) {
                if (possible.menu != item.menu || possible.subMenu != item.subMenu ||
                    (possible.commandId == UINT32_MAX ? !item.subMenu : possible.commandId != item.commandId)) continue;
                if (alias || !possible.element || !ValidId(possible.id)) return MMS_E_CONFLICT;
                alias = &possible;
            }
            HostUiCatalogEntry entry;
            if (alias) {
                if (!usedAliases.insert(alias->element).second) return MMS_E_CONFLICT;
                entry.id = alias->id; entry.reuseElement = alias->element;
                // The backend resolves the original refresh presentation.
                // An unpresented registered alias never receives a fabricated instance.
                entry.instance = 0;
            } else {
                entry.id = "mms.catalog.main.menu.g" + std::to_string(structure) + ".row." + std::to_string(i);
                entry.instance = replaced || !menuInstances.at(i) ? registry.NewPresentationInstance() : menuInstances.at(i);
                entry.knownFields |= MMS_UI_CATALOG_PRESENTATION;
            }
            instances.push_back(entry.instance);
            entry.parentId = item.parent == UINT32_MAX ? kMenuRoot : rowIds[item.parent];
            entry.scope = MMS_UI_CATALOG_MAIN_MENU; entry.ordinal = item.position;
            entry.knownFields |= MMS_UI_CATALOG_ENABLED | MMS_UI_CATALOG_VISIBLE | MMS_UI_CATALOG_CHECKED;
            entry.state = MMS_UI_CATALOG_STATE_VISIBLE;
            if (!(item.state & (MFS_DISABLED | MFS_GRAYED))) entry.state |= MMS_UI_CATALOG_STATE_ENABLED;
            if (item.state & MFS_CHECKED) entry.state |= MMS_UI_CATALOG_STATE_CHECKED;
            if (item.textKnown) { entry.label = Utf8(std::wstring_view(item.text)); entry.knownFields |= MMS_UI_CATALOG_LABEL; }
            if (item.type & MFT_SEPARATOR) { entry.kind = MMS_UI_SEPARATOR; entry.role = MMS_UI_CATALOG_ROLE_SEPARATOR; }
            else if (item.subMenu) { entry.kind = MMS_UI_MENU; entry.role = MMS_UI_CATALOG_ROLE_MENU; }
            else { entry.kind = MMS_UI_MENU_ITEM; entry.role = MMS_UI_CATALOG_ROLE_MENU_ITEM; }
            rowIds.push_back(entry.id);
            publication.entries.push_back(std::move(entry));
        }
        std::map<int, std::shared_ptr<Observer>> controlCandidate;
        if (const auto result = CaptureControls(publication, controlCandidate, WindowDpi(main))) return result;
        if (!mainObserver->Current() || GetMenu(main) != reinterpret_cast<HMENU>(captured.rootMenu)) return MMS_E_STALE;
        for (const auto& [id, observer] : controlCandidate)
            if (!observer->Current() || GetDlgItem(main, id) != observer->window) return MMS_E_STALE;
        if (const auto result = registry.Catalog().Publish(publication)) return result;
        // All allocations, observer installation and validation precede the
        // backend publication. These swaps cannot fail or invoke providers.
        menu = std::move(captured); haveMenu = true; menuStructure = structure;
        menuInstances.swap(instances); controls.swap(controlCandidate);
        return MMS_OK;
    }
};

NativeUiCatalog::NativeUiCatalog(UiRegistry& registry, HWND main)
    : impl_(std::make_unique<Impl>(registry, main)) {}
NativeUiCatalog::~NativeUiCatalog() = default;
MmsResult NativeUiCatalog::Observe(const std::vector<NativeCatalogMenuAlias>& aliases) noexcept {
    return Boundary([&] { return impl_->Observe(aliases); });
}
}
