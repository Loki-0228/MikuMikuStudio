#include "ui_registry.hpp"
#include "common.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mikumikustudio::plugins {
namespace {

constexpr std::uint64_t kMaxEntries = 16384;
constexpr std::uint64_t kMaxSnapshotBytes = 8ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxLeasesPerPlugin = 16;
constexpr std::uint64_t kMaxLeasesGlobal = 128;
constexpr std::uint64_t kMaxRetainedBytesPerPlugin = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxRetainedBytesGlobal = 64ull * 1024ull * 1024ull;
constexpr std::size_t kMaxLabelBytes = 65536;
// Three vector allocations can each carry MSVC alignment slack (>= 4 KiB
// blocks lose about 39 bytes), and a map node plus ownerId's trailing NUL add
// more. 256 is a conservative per-lease bookkeeping charge.
constexpr std::uint64_t kLeaseBookkeeping = 256;
constexpr char kHostOwner[] = "mms";
constexpr std::uint64_t kHostOwnerBytes = 3;

constexpr std::uint32_t kNativeScopes =
    MMS_UI_CATALOG_MAIN_MENU | MMS_UI_CATALOG_MAIN_CONTROLS;
constexpr std::uint32_t kAllScopes = MMS_UI_CATALOG_REGISTRY | kNativeScopes;
constexpr std::uint32_t kKnownFields =
    MMS_UI_CATALOG_LABEL | MMS_UI_CATALOG_ENABLED | MMS_UI_CATALOG_VISIBLE |
    MMS_UI_CATALOG_CHECKED | MMS_UI_CATALOG_BOUNDS | MMS_UI_CATALOG_PRESENTATION;
constexpr std::uint32_t kStateFields =
    MMS_UI_CATALOG_STATE_ENABLED | MMS_UI_CATALOG_STATE_VISIBLE | MMS_UI_CATALOG_STATE_CHECKED;
constexpr std::uint32_t kCoverageFields =
    MMS_UI_CATALOG_REGISTRY_COMPLETE | MMS_UI_CATALOG_MATERIALIZED_MENU |
    MMS_UI_CATALOG_DECLARED_CONTROLS | MMS_UI_CATALOG_INCOMPLETE_NATIVE;

bool AddBytes(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += value;
    return true;
}

bool AddProduct(std::uint64_t& total, std::uint64_t count, std::uint64_t size) noexcept {
    if (count && size > std::numeric_limits<std::uint64_t>::max() / count) return false;
    return AddBytes(total, count * size);
}

bool ValidUtf8NoNul(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) return false;
    if (text.empty()) return true;
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0) != 0;
}

bool ValidEntryScope(std::uint32_t scope) noexcept {
    return scope != 0 && (scope & ~kNativeScopes) == 0;
}

bool ValidKind(std::uint32_t kind) noexcept {
    return kind >= MMS_UI_MENU && kind <= MMS_UI_GROUP;
}

bool ValidRole(std::uint32_t role) noexcept {
    return role <= MMS_UI_CATALOG_ROLE_SEPARATOR;
}

std::uint32_t RoleForKind(std::uint32_t kind) noexcept {
    switch (kind) {
    case MMS_UI_BUTTON: return MMS_UI_CATALOG_ROLE_BUTTON;
    case MMS_UI_INPUT: return MMS_UI_CATALOG_ROLE_INPUT;
    case MMS_UI_MENU: return MMS_UI_CATALOG_ROLE_MENU;
    case MMS_UI_MENU_ITEM: return MMS_UI_CATALOG_ROLE_MENU_ITEM;
    case MMS_UI_SEPARATOR: return MMS_UI_CATALOG_ROLE_SEPARATOR;
    default: return MMS_UI_CATALOG_ROLE_UNKNOWN;
    }
}

bool FiniteBounds(const MmsUiDirtyRectV1& bounds) noexcept {
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
        std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
        bounds.width >= 0.0f && bounds.height >= 0.0f;
}

std::uint32_t KnownStateFields(std::uint32_t known) noexcept {
    std::uint32_t result = 0;
    if (known & MMS_UI_CATALOG_ENABLED) result |= MMS_UI_CATALOG_STATE_ENABLED;
    if (known & MMS_UI_CATALOG_VISIBLE) result |= MMS_UI_CATALOG_STATE_VISIBLE;
    if (known & MMS_UI_CATALOG_CHECKED) result |= MMS_UI_CATALOG_STATE_CHECKED;
    return result;
}

bool SameText(MmsString text, std::string_view value) noexcept {
    return text.bytes == value.size() &&
        (text.bytes == 0 || (text.data && std::memcmp(text.data, value.data(),
            static_cast<std::size_t>(text.bytes)) == 0));
}

bool SameBounds(const MmsUiDirtyRectV1& a, const MmsUiDirtyRectV1& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

struct PublishedAncestor {
    std::string id;
    MmsHandle handle = 0;
};

struct PublishedEntry {
    MmsHandle handle = 0;
    bool observation = false;
    bool alias = false;
    std::string id, parentId, label;
    MmsHandle parentHandle = 0;
    std::vector<PublishedAncestor> ancestors;
    std::uint64_t instance = 0;
    MmsUiDirtyRectV1 bounds{};
    std::uint32_t kind = MMS_UI_GROUP;
    std::uint32_t role = MMS_UI_CATALOG_ROLE_UNKNOWN;
    std::uint32_t scope = 0, knownFields = 0, state = 0, ordinal = 0;
};

bool SamePublished(const PublishedEntry& a, const PublishedEntry& b) noexcept {
    if (!(a.handle == b.handle && a.observation == b.observation && a.alias == b.alias &&
        a.id == b.id && a.parentId == b.parentId && a.label == b.label &&
        a.parentHandle == b.parentHandle && a.instance == b.instance &&
        SameBounds(a.bounds, b.bounds) && a.kind == b.kind && a.role == b.role &&
        a.scope == b.scope && a.knownFields == b.knownFields && a.state == b.state &&
        a.ordinal == b.ordinal && a.ancestors.size() == b.ancestors.size()))
        return false;
    for (std::size_t i = 0; i < a.ancestors.size(); ++i) {
        if (a.ancestors[i].id != b.ancestors[i].id ||
            a.ancestors[i].handle != b.ancestors[i].handle)
            return false;
    }
    return true;
}

bool SamePublication(const std::map<MmsHandle, PublishedEntry>& a,
    const std::map<MmsHandle, PublishedEntry>& b) noexcept {
    if (a.size() != b.size()) return false;
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (ia->first != ib->first || !SamePublished(ia->second, ib->second)) return false;
    }
    return true;
}

struct Projection {
    const ElementRecord* record = nullptr;
    MmsString id{}, ownerPlugin{}, parentId{}, label{}, commandId{}, propertyId{};
    std::uint64_t descriptionGeneration = 0;
    MmsUiTargetRefV1 target{};
    MmsUiDirtyRectV1 bounds{};
    std::uint32_t kind = 0, origin = 0, scope = 0, knownFields = 0;
    std::uint32_t state = 0, role = 0, ordinal = 0;
};

struct Lease {
    UiClient* owner = nullptr;
    std::string ownerId;
    bool entryOnly = false;
    std::uint64_t charge = 0;
    MmsUiCatalogSnapshotV1 snapshot{};
    std::vector<MmsUiCatalogEntryV1> elements;
    std::vector<MmsUiCatalogRegistrationV1> registrations;
    std::vector<char> text;
};

struct Usage {
    std::uint32_t leases = 0;
    std::uint64_t bytes = 0;
};

std::uint64_t LeaseCharge(const Lease& lease) noexcept {
    std::uint64_t charge = sizeof(Lease) + kLeaseBookkeeping;
    if (!AddBytes(charge, lease.ownerId.capacity())) return std::numeric_limits<std::uint64_t>::max();
    if (!AddProduct(charge, lease.elements.capacity(), sizeof(MmsUiCatalogEntryV1)))
        return std::numeric_limits<std::uint64_t>::max();
    if (!AddProduct(charge, lease.registrations.capacity(), sizeof(MmsUiCatalogRegistrationV1)))
        return std::numeric_limits<std::uint64_t>::max();
    if (!AddBytes(charge, lease.text.capacity())) return std::numeric_limits<std::uint64_t>::max();
    return charge;
}

bool AddProjectionBytes(const Projection& projection, std::uint64_t& total) noexcept {
    return AddBytes(total, projection.id.bytes) &&
        AddBytes(total, projection.ownerPlugin.bytes) &&
        AddBytes(total, projection.parentId.bytes) &&
        AddBytes(total, projection.label.bytes) &&
        AddBytes(total, projection.commandId.bytes) &&
        AddBytes(total, projection.propertyId.bytes);
}

bool AddRegistrationBytes(std::uint64_t& total, MmsString id, MmsString owner,
    MmsString label) noexcept {
    return AddBytes(total, id.bytes) && AddBytes(total, owner.bytes) &&
        AddBytes(total, label.bytes);
}

MmsString CopyText(std::vector<char>& text, std::size_t& offset, MmsString source) {
    if (source.bytes && !source.data) throw std::invalid_argument("catalog string");
    if (offset > text.size() || source.bytes > text.size() - offset)
        throw std::invalid_argument("catalog text");
    if (!source.bytes) return MmsString{};
    MmsString result{text.data() + offset, source.bytes};
    std::memcpy(text.data() + offset, source.data, static_cast<std::size_t>(source.bytes));
    offset += static_cast<std::size_t>(source.bytes);
    return result;
}

MmsUiCatalogEntryV1 BuildEntry(const Projection& projection, std::vector<char>& text,
    std::size_t& offset) {
    MmsUiCatalogEntryV1 entry{};
    entry.h = Header<MmsUiCatalogEntryV1>();
    entry.element = projection.record->handle;
    entry.id = CopyText(text, offset, projection.id);
    entry.ownerPlugin = CopyText(text, offset, projection.ownerPlugin);
    entry.parentId = CopyText(text, offset, projection.parentId);
    entry.label = CopyText(text, offset, projection.label);
    entry.commandId = CopyText(text, offset, projection.commandId);
    entry.propertyId = CopyText(text, offset, projection.propertyId);
    entry.descriptionGeneration = projection.descriptionGeneration;
    entry.target = projection.target;
    entry.bounds = projection.bounds;
    entry.kind = projection.kind;
    entry.origin = projection.origin;
    entry.scope = projection.scope;
    entry.knownFields = projection.knownFields;
    entry.state = projection.state;
    entry.role = projection.role;
    entry.ordinal = projection.ordinal;
    entry.reserved = 0;
    return entry;
}

MmsUiCatalogRegistrationV1 BuildRegistration(MmsHandle handle, MmsString id,
    MmsString owner, MmsString label, std::uint32_t kind, std::uint32_t flags,
    std::vector<char>& text, std::size_t& offset) {
    MmsUiCatalogRegistrationV1 registration{};
    registration.h = Header<MmsUiCatalogRegistrationV1>();
    registration.handle = handle;
    registration.id = CopyText(text, offset, id);
    registration.ownerPlugin = CopyText(text, offset, owner);
    registration.label = CopyText(text, offset, label);
    registration.kind = kind;
    registration.flags = flags;
    return registration;
}

void ValidatePublicationEntry(const HostUiCatalogEntry& entry) {
    if (!ValidId(entry.id)) throw std::invalid_argument("catalog id");
    if (!entry.parentId.empty() && !ValidId(entry.parentId))
        throw std::invalid_argument("catalog parent");
    if (!entry.parentId.empty() && entry.parentId == entry.id)
        throw std::invalid_argument("catalog parent cycle");
    if (entry.label.size() > kMaxLabelBytes || !ValidUtf8NoNul(entry.label))
        throw std::invalid_argument("catalog label");
    if (entry.reuseElement == 0) {
        static constexpr std::string_view kPrefix = "mms.catalog.";
        if (entry.id.size() <= kPrefix.size() ||
            entry.id.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0)
            throw std::invalid_argument("catalog observation id");
    }
    if (!ValidKind(entry.kind)) throw std::invalid_argument("catalog kind");
    if (!ValidRole(entry.role)) throw std::invalid_argument("catalog role");
    if (!ValidEntryScope(entry.scope)) throw std::invalid_argument("catalog scope");
    if (entry.knownFields & ~kKnownFields) throw std::invalid_argument("catalog known fields");
    if (entry.state & ~kStateFields) throw std::invalid_argument("catalog state");
    if (!FiniteBounds(entry.bounds)) throw std::invalid_argument("catalog bounds");
    if ((entry.knownFields & MMS_UI_CATALOG_BOUNDS) &&
        (entry.scope & MMS_UI_CATALOG_MAIN_MENU))
        throw std::invalid_argument("menu bounds");
    const bool presentation = (entry.knownFields & MMS_UI_CATALOG_PRESENTATION) != 0;
    if (presentation != (entry.instance != 0))
        throw std::invalid_argument("catalog presentation");
    if (entry.reuseElement && (entry.instance != 0 || presentation))
        throw std::invalid_argument("catalog alias presentation");
}

MmsString InternString(Arena& arena, const std::string& value) {
    arena.strings.emplace_back(value);
    const std::string& stored = arena.strings.back();
    return MmsString{stored.data(), static_cast<std::uint64_t>(stored.size())};
}

} // namespace

struct UiCatalog::Impl {
    struct ApiContext {
        Impl* owner = nullptr;
        UiClient* client = nullptr;
    };

    UiRegistry& registry_;
    std::uint64_t revision_ = 1;
    std::uint64_t description_ = 1;
    std::uint32_t availableScopes_ = 0;
    std::uint32_t coverageFlags_ =
        MMS_UI_CATALOG_REGISTRY_COMPLETE | MMS_UI_CATALOG_INCOMPLETE_NATIVE;
    std::map<MmsHandle, PublishedEntry> publication_;
    std::map<MmsHandle, std::unique_ptr<Lease>> leases_;
    std::map<std::string, Usage> usage_;
    std::uint32_t globalLeases_ = 0;
    std::uint64_t retainedGlobal_ = 0;
    std::map<UiClient*, std::unique_ptr<ApiContext>> contexts_;

    explicit Impl(UiRegistry& registry) : registry_(registry) {}

    ApiContext* Context(UiClient& client) {
        auto found = contexts_.find(&client);
        if (found != contexts_.end()) return found->second.get();
        auto context = std::make_unique<ApiContext>();
        context->owner = this;
        context->client = &client;
        ApiContext* raw = context.get();
        contexts_.emplace(&client, std::move(context));
        return raw;
    }

    MmsResult CheckClient(UiClient& client, bool allowStopping) const noexcept {
        if (GetCurrentThreadId() != registry_.thread_) return MMS_E_THREAD;
        if (!client.registry || client.registry != &registry_) return MMS_E_ARGUMENT;
        if (!allowStopping && client.stopping) return MMS_E_NOT_READY;
        return MMS_OK;
    }

    MmsResult ReserveSharedHandle(MmsHandle& handle) noexcept {
        const std::uint64_t next = registry_.next_;
        if (!next || next == std::numeric_limits<std::uint64_t>::max()) return MMS_E_NOT_READY;
        handle = next;
        registry_.next_ = next + 1;
        return MMS_OK;
    }

    const ElementRecord* FindElement(MmsHandle handle) const noexcept {
        for (const auto& item : registry_.elements) {
            if (item.second.handle == handle) return &item.second;
        }
        return nullptr;
    }

    bool NativeBranchValid(const PublishedEntry& published) const {
        if (!published.observation) return false;
        if (published.parentId.empty())
            return published.parentHandle == 0 && published.ancestors.empty();
        if (!published.parentHandle || published.ancestors.empty() ||
            published.ancestors.front().handle != published.parentHandle ||
            published.ancestors.front().id != published.parentId)
            return false;
        for (const auto& ancestor : published.ancestors) {
            const auto found = registry_.elements.find(ancestor.id);
            if (found == registry_.elements.end() || found->second.handle != ancestor.handle)
                return false;
        }
        return true;
    }

    MmsResult Project(const ElementRecord& record, const PublishedEntry* published,
        std::uint64_t epoch, Projection& projection) const {
        projection.record = &record;
        projection.descriptionGeneration = record.desc.generation;
        projection.target.h = Header<MmsUiTargetRefV1>();
        projection.target.element = record.handle;
        projection.target.projectEpoch = epoch;
        if (record.nativeObserved) {
            if (!published || !published->observation) return MMS_E_STALE;
            projection.id = Text(published->id);
            projection.ownerPlugin = MmsString{kHostOwner, kHostOwnerBytes};
            projection.parentId = Text(published->parentId);
            projection.label = Text(published->label);
            projection.commandId = MmsString{};
            projection.propertyId = MmsString{};
            projection.kind = published->kind;
            projection.origin = MMS_UI_CATALOG_NATIVE_OBSERVATION;
            projection.scope = published->scope;
            projection.knownFields = published->knownFields & kKnownFields;
            projection.state = published->state;
            projection.role = published->role;
            projection.ordinal = published->ordinal;
            projection.target.instanceGeneration = published->instance;
            projection.bounds = published->bounds;
            if (published->instance) projection.knownFields |= MMS_UI_CATALOG_PRESENTATION;
            else projection.knownFields &= ~MMS_UI_CATALOG_PRESENTATION;
            if (projection.scope & MMS_UI_CATALOG_MAIN_MENU)
                projection.knownFields &= ~MMS_UI_CATALOG_BOUNDS;
            if (!(projection.knownFields & MMS_UI_CATALOG_BOUNDS))
                projection.bounds = MmsUiDirtyRectV1{};
            projection.knownFields &= kKnownFields;
            projection.state &= KnownStateFields(projection.knownFields);
            return MMS_OK;
        }

        projection.id = record.desc.id;
        projection.ownerPlugin = record.desc.ownerPlugin;
        projection.parentId = record.desc.parentId;
        projection.label = record.desc.label;
        projection.commandId = record.desc.commandId;
        projection.propertyId = record.desc.propertyId;
        projection.kind = record.desc.kind;
        projection.origin = SameText(record.desc.ownerPlugin, "mms") ?
            MMS_UI_CATALOG_HOST_ANCHOR : MMS_UI_CATALOG_REGISTERED;
        projection.scope = MMS_UI_CATALOG_REGISTRY;
        projection.knownFields = MMS_UI_CATALOG_LABEL;
        projection.state = 0;
        projection.role = RoleForKind(record.desc.kind);
        projection.ordinal = 0;

        const bool alias = published && published->alias;
        if (alias) {
            if (!published->parentId.empty()) projection.parentId = Text(published->parentId);
            if (published->knownFields & MMS_UI_CATALOG_LABEL)
                projection.label = Text(published->label);
            projection.kind = published->kind;
            projection.scope |= published->scope;
            projection.knownFields |= published->knownFields &
                (MMS_UI_CATALOG_ENABLED | MMS_UI_CATALOG_VISIBLE |
                    MMS_UI_CATALOG_CHECKED);
            projection.state = published->state;
            projection.role = published->role;
            projection.ordinal = published->ordinal;
        }

        MmsUiTargetRefV1 refreshTarget{};
        MmsUiDirtyRectV1 refreshBounds{};
        bool refreshVisible = false;
        bool menu = false;
        const bool presented = registry_.Refresh().CatalogTarget(record.handle, refreshTarget,
            refreshBounds, refreshVisible, menu);
        if (presented) {
            projection.target = refreshTarget;
            projection.knownFields |= MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_VISIBLE;
            if (menu) {
                projection.knownFields &= ~MMS_UI_CATALOG_BOUNDS;
                projection.bounds = MmsUiDirtyRectV1{};
            } else {
                projection.knownFields |= MMS_UI_CATALOG_BOUNDS;
                projection.bounds = refreshBounds;
            }
            projection.state = (projection.state & ~MMS_UI_CATALOG_STATE_VISIBLE) |
                (refreshVisible ? MMS_UI_CATALOG_STATE_VISIBLE : 0);
        }

        projection.target.h = Header<MmsUiTargetRefV1>();
        projection.target.element = record.handle;
        projection.target.projectEpoch = epoch;
        if (!projection.target.instanceGeneration)
            projection.knownFields &= ~MMS_UI_CATALOG_PRESENTATION;
        else
            projection.knownFields |= MMS_UI_CATALOG_PRESENTATION;
        if (projection.scope & MMS_UI_CATALOG_MAIN_MENU) {
            projection.knownFields &= ~MMS_UI_CATALOG_BOUNDS;
        }
        if (!(projection.knownFields & MMS_UI_CATALOG_BOUNDS))
            projection.bounds = MmsUiDirtyRectV1{};
        projection.knownFields &= kKnownFields;
        projection.state &= KnownStateFields(projection.knownFields);
        return MMS_OK;
    }

    template<class F>
    MmsResult ForEachProjectedElement(std::uint32_t scopes, std::uint64_t epoch,
        F&& visit) const {
        for (const auto& item : registry_.elements) {
            const ElementRecord& record = item.second;
            const PublishedEntry* published = nullptr;
            const auto found = publication_.find(record.handle);
            if (found != publication_.end()) published = &found->second;
            if (record.nativeObserved) {
                if (!published || !published->observation) return MMS_E_STALE;
                if (!(published->scope & scopes)) continue;
                if (!NativeBranchValid(*published)) continue;
            } else {
                const bool alias = published && published->alias;
                if (alias) {
                    if (!(scopes & MMS_UI_CATALOG_REGISTRY) &&
                        !(published->scope & scopes)) continue;
                } else if (!(scopes & MMS_UI_CATALOG_REGISTRY)) {
                    continue;
                }
            }
            Projection projection;
            const MmsResult result = Project(record, published, epoch, projection);
            if (result) return result;
            const MmsResult visited = visit(projection);
            if (visited) return visited;
        }
        return MMS_OK;
    }

    MmsResult CheckLeaseBudget(UiClient& client, std::uint64_t charge,
        std::map<std::string, Usage>::iterator& usage) {
        usage = usage_.find(client.id);
        if (usage == usage_.end()) usage = usage_.emplace(client.id, Usage{}).first;
        if (globalLeases_ >= kMaxLeasesGlobal ||
            usage->second.leases >= kMaxLeasesPerPlugin)
            return MMS_E_MEMORY;
        if (charge > kMaxSnapshotBytes) return MMS_E_MEMORY;
        if (usage->second.bytes > kMaxRetainedBytesPerPlugin ||
            charge > kMaxRetainedBytesPerPlugin - usage->second.bytes)
            return MMS_E_MEMORY;
        if (retainedGlobal_ > kMaxRetainedBytesGlobal ||
            charge > kMaxRetainedBytesGlobal - retainedGlobal_)
            return MMS_E_MEMORY;
        return MMS_OK;
    }

    void AddLeaseUsage(std::map<std::string, Usage>::iterator usage,
        std::uint64_t charge) noexcept {
        ++usage->second.leases;
        usage->second.bytes += charge;
        ++globalLeases_;
        retainedGlobal_ += charge;
    }

    void RemoveLeaseUsage(const Lease& lease) noexcept {
        auto usage = usage_.find(lease.ownerId);
        if (usage != usage_.end()) {
            if (usage->second.leases) --usage->second.leases;
            if (usage->second.bytes >= lease.charge) usage->second.bytes -= lease.charge;
            else usage->second.bytes = 0;
        }
        if (globalLeases_) --globalLeases_;
        if (retainedGlobal_ >= lease.charge) retainedGlobal_ -= lease.charge;
        else retainedGlobal_ = 0;
    }

    MmsResult Capabilities(UiClient& client, MmsUiCatalogCapabilitiesV1* out) {
        const MmsResult check = CheckClient(client, false);
        if (check) return check;
        if (!Valid(out) || out->reserved) return MMS_E_ARGUMENT;
        MmsUiCatalogCapabilitiesV1 result{};
        result.h = Header<MmsUiCatalogCapabilitiesV1>();
        result.supportedScopes = kAllScopes;
        result.maxLeasesPerPlugin = kMaxLeasesPerPlugin;
        result.maxLeasesGlobal = kMaxLeasesGlobal;
        result.maxSnapshotBytes = kMaxSnapshotBytes;
        result.maxRetainedBytesPerPlugin = kMaxRetainedBytesPerPlugin;
        result.maxRetainedBytesGlobal = kMaxRetainedBytesGlobal;
        result.maxEntries = static_cast<std::uint32_t>(kMaxEntries);
        result.reserved = 0;
        *out = result;
        return MMS_OK;
    }

    MmsResult BuildSnapshot(UiClient& client, std::uint32_t scopes, std::uint64_t epoch,
        MmsUiCatalogSnapshotV1& result) {
        std::uint64_t elementCount = 0;
        std::uint64_t registrationCount = 0;
        std::uint64_t textBytes = 0;
        MmsResult status = ForEachProjectedElement(scopes, epoch,
            [&](const Projection& projection) -> MmsResult {
                if (elementCount >= kMaxEntries) return MMS_E_MEMORY;
                ++elementCount;
                if (!AddProjectionBytes(projection, textBytes)) return MMS_E_MEMORY;
                return MMS_OK;
            });
        if (status) return status;
        if (scopes & MMS_UI_CATALOG_REGISTRY) {
            for (const auto& item : registry_.commands) {
                const auto& command = item.second;
                if (!command) return MMS_E_STALE;
                if (elementCount + registrationCount >= kMaxEntries) return MMS_E_MEMORY;
                ++registrationCount;
                if (!AddRegistrationBytes(textBytes, command->desc.info.id,
                    command->desc.info.ownerPlugin, command->desc.info.label))
                    return MMS_E_MEMORY;
            }
            for (const auto& item : registry_.properties) {
                const auto& property = item.second;
                if (!property) return MMS_E_STALE;
                if (elementCount + registrationCount >= kMaxEntries) return MMS_E_MEMORY;
                ++registrationCount;
                if (!AddRegistrationBytes(textBytes, property->desc.info.id,
                    property->desc.info.ownerPlugin, property->desc.info.label))
                    return MMS_E_MEMORY;
            }
        }
        const std::uint64_t totalCount = elementCount + registrationCount;
        if (totalCount > kMaxEntries || textBytes > kMaxSnapshotBytes) return MMS_E_MEMORY;
        if (elementCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            registrationCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            textBytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return MMS_E_MEMORY;

        std::vector<MmsUiCatalogEntryV1> elements(static_cast<std::size_t>(elementCount));
        std::vector<MmsUiCatalogRegistrationV1> registrations(
            static_cast<std::size_t>(registrationCount));
        std::vector<char> text(static_cast<std::size_t>(textBytes));
        std::size_t offset = 0;
        std::size_t elementIndex = 0;
        std::size_t registrationIndex = 0;
        status = ForEachProjectedElement(scopes, epoch,
            [&](const Projection& projection) -> MmsResult {
                if (elementIndex >= elements.size()) return MMS_E_FAILED;
                elements[elementIndex++] = BuildEntry(projection, text, offset);
                return MMS_OK;
            });
        if (status) return status;
        if (scopes & MMS_UI_CATALOG_REGISTRY) {
            for (const auto& item : registry_.commands) {
                const auto& command = item.second;
                if (registrationIndex >= registrations.size()) return MMS_E_FAILED;
                registrations[registrationIndex++] = BuildRegistration(command->handle,
                    command->desc.info.id, command->desc.info.ownerPlugin, command->desc.info.label,
                    MMS_UI_CATALOG_COMMAND, command->desc.info.flags, text, offset);
            }
            for (const auto& item : registry_.properties) {
                const auto& property = item.second;
                if (registrationIndex >= registrations.size()) return MMS_E_FAILED;
                registrations[registrationIndex++] = BuildRegistration(property->handle,
                    property->desc.info.id, property->desc.info.ownerPlugin, property->desc.info.label,
                    MMS_UI_CATALOG_PROPERTY, property->desc.info.flags, text, offset);
            }
        }
        if (elementIndex != elements.size() || registrationIndex != registrations.size() ||
            offset != text.size())
            return MMS_E_FAILED;

        auto lease = std::make_unique<Lease>();
        lease->owner = &client;
        lease->ownerId = client.id;
        lease->entryOnly = false;
        lease->elements = std::move(elements);
        lease->registrations = std::move(registrations);
        lease->text = std::move(text);
        lease->snapshot = MmsUiCatalogSnapshotV1{};
        lease->snapshot.h = Header<MmsUiCatalogSnapshotV1>();
        lease->snapshot.revision = revision_;
        lease->snapshot.registryRevision = registry_.Revision();
        lease->snapshot.projectEpoch = epoch;
        lease->snapshot.requestedScopes = scopes;
        lease->snapshot.availableScopes = MMS_UI_CATALOG_REGISTRY | availableScopes_;
        lease->snapshot.coverageFlags = coverageFlags_ | MMS_UI_CATALOG_INCOMPLETE_NATIVE;
        lease->snapshot.elements = MmsSpan{lease->elements.data(),
            static_cast<std::uint64_t>(lease->elements.size()),
            static_cast<std::uint32_t>(sizeof(MmsUiCatalogEntryV1)), 1};
        lease->snapshot.registrations = MmsSpan{lease->registrations.data(),
            static_cast<std::uint64_t>(lease->registrations.size()),
            static_cast<std::uint32_t>(sizeof(MmsUiCatalogRegistrationV1)), 1};

        const std::uint64_t charge = LeaseCharge(*lease);
        if (charge == std::numeric_limits<std::uint64_t>::max()) return MMS_E_MEMORY;
        lease->charge = charge;
        std::map<std::string, Usage>::iterator usage;
        status = CheckLeaseBudget(client, charge, usage);
        if (status) return status;
        MmsHandle handle = 0;
        status = ReserveSharedHandle(handle);
        if (status) return status;
        lease->snapshot.handle = handle;
        const auto inserted = leases_.emplace(handle, std::move(lease));
        if (!inserted.second) return MMS_E_NOT_READY;
        const Lease& stored = *inserted.first->second;
        AddLeaseUsage(usage, charge);
        result = stored.snapshot;
        return MMS_OK;
    }

    MmsResult AcquireSnapshot(UiClient& client, std::uint32_t scopes,
        MmsUiCatalogSnapshotV1* out) {
        const MmsResult check = CheckClient(client, false);
        if (check) return check;
        if (!scopes || (scopes & ~kAllScopes)) return MMS_E_ARGUMENT;
        if (!Valid(out) || out->reserved) return MMS_E_ARGUMENT;
        const std::uint64_t epoch = registry_.Refresh().ProjectEpoch();
        if (!epoch) return MMS_E_THREAD;
        MmsUiCatalogSnapshotV1 result{};
        const MmsResult status = BuildSnapshot(client, scopes, epoch, result);
        if (status) return status;
        *out = result;
        return MMS_OK;
    }

    MmsResult Resolve(UiClient& client, const MmsUiTargetRefV1* expected,
        MmsUiCatalogEntryV1* out, MmsHandle* leaseOut) {
        const MmsResult check = CheckClient(client, false);
        if (check) return check;
        if (!expected || !Valid(expected) || !expected->element || !out ||
            !Valid(out) || out->reserved || !leaseOut)
            return MMS_E_ARGUMENT;
        const std::uint64_t epoch = registry_.Refresh().ProjectEpoch();
        if (!epoch) return MMS_E_THREAD;
        const ElementRecord* record = FindElement(expected->element);
        if (!record) return MMS_E_STALE;
        const auto found = publication_.find(record->handle);
        const PublishedEntry* published =
            found == publication_.end() ? nullptr : &found->second;
        if (record->nativeObserved && (!published || !NativeBranchValid(*published)))
            return MMS_E_STALE;
        Projection projection;
        MmsResult status = Project(*record, published, epoch, projection);
        if (status) return status;
        if (expected->projectEpoch != epoch ||
            expected->instanceGeneration != projection.target.instanceGeneration)
            return MMS_E_STALE;

        std::uint64_t textBytes = 0;
        if (!AddProjectionBytes(projection, textBytes)) return MMS_E_MEMORY;
        if (textBytes > kMaxSnapshotBytes) return MMS_E_MEMORY;
        if (textBytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return MMS_E_MEMORY;

        auto lease = std::make_unique<Lease>();
        lease->owner = &client;
        lease->ownerId = client.id;
        lease->entryOnly = true;
        lease->elements.resize(1);
        lease->text.resize(static_cast<std::size_t>(textBytes));
        std::size_t offset = 0;
        lease->elements[0] = BuildEntry(projection, lease->text, offset);
        if (offset != lease->text.size()) return MMS_E_FAILED;

        const std::uint64_t charge = LeaseCharge(*lease);
        if (charge == std::numeric_limits<std::uint64_t>::max()) return MMS_E_MEMORY;
        lease->charge = charge;
        std::map<std::string, Usage>::iterator usage;
        status = CheckLeaseBudget(client, charge, usage);
        if (status) return status;
        MmsHandle handle = 0;
        status = ReserveSharedHandle(handle);
        if (status) return status;
        const auto inserted = leases_.emplace(handle, std::move(lease));
        if (!inserted.second) return MMS_E_NOT_READY;
        const Lease& stored = *inserted.first->second;
        AddLeaseUsage(usage, charge);
        *out = stored.elements[0];
        *leaseOut = handle;
        return MMS_OK;
    }

    MmsResult ReleaseLease(UiClient& client, MmsHandle handle) {
        const MmsResult check = CheckClient(client, true);
        if (check) return check;
        if (!handle) return MMS_E_ARGUMENT;
        auto found = leases_.find(handle);
        if (found == leases_.end()) return MMS_E_NOT_FOUND;
        const Lease& lease = *found->second;
        if (lease.owner != &client) return MMS_E_ARGUMENT;
        RemoveLeaseUsage(lease);
        leases_.erase(found);
        return MMS_OK;
    }

    void SaturateRevision() noexcept {
        if (revision_ != std::numeric_limits<std::uint64_t>::max()) ++revision_;
    }

    MmsResult Publish(const HostUiCatalogPublication& publication) {
        if (GetCurrentThreadId() != registry_.thread_) return MMS_E_THREAD;
        if (registry_.InCallback()) return MMS_E_BUSY;
        if (publication.availableScopes & ~kAllScopes)
            throw std::invalid_argument("catalog available scopes");
        if (publication.coverageFlags & ~kCoverageFields)
            throw std::invalid_argument("catalog coverage");
        if (publication.entries.size() > kMaxEntries) return MMS_E_MEMORY;
        std::uint64_t textBytes = 0;
        for (const auto& entry : publication.entries) {
            ValidatePublicationEntry(entry);
            if (!AddBytes(textBytes, entry.id.size()) ||
                !AddBytes(textBytes, entry.parentId.size()) ||
                !AddBytes(textBytes, entry.label.size()))
                return MMS_E_MEMORY;
        }
        if (textBytes > kMaxSnapshotBytes) return MMS_E_MEMORY;

        std::map<std::string, MmsHandle> previousObservations;
        for (const auto& item : publication_) {
            if (item.second.observation)
                previousObservations.emplace(item.second.id, item.second.handle);
        }

        std::map<std::string, ElementRecord> candidate = registry_.elements;
        for (auto it = candidate.begin(); it != candidate.end();) {
            if (it->second.nativeObserved) it = candidate.erase(it);
            else ++it;
        }

        std::shared_ptr<Arena> arena;
        std::map<MmsHandle, PublishedEntry> next;
        std::set<std::string> newIds;
        std::uint64_t localNext = registry_.next_;
        std::uint64_t localDescription = description_;
        auto allocateHandle = [&](MmsHandle& handle) noexcept -> bool {
            if (!localNext || localNext == std::numeric_limits<std::uint64_t>::max())
                return false;
            handle = localNext++;
            return true;
        };
        auto intern = [&](const std::string& value) -> MmsString {
            if (!arena) arena = std::make_shared<Arena>();
            return InternString(*arena, value);
        };

        for (const auto& entry : publication.entries) {
            if (entry.reuseElement) continue;
            if (candidate.find(entry.id) != candidate.end() ||
                !newIds.insert(entry.id).second)
                throw std::invalid_argument("catalog observation conflict");
            MmsHandle handle = 0;
            const auto previous = previousObservations.find(entry.id);
            if (previous != previousObservations.end()) handle = previous->second;
            else if (!allocateHandle(handle)) return MMS_E_NOT_READY;
            if (!next.emplace(handle, PublishedEntry{}).second)
                throw std::invalid_argument("catalog handle conflict");

            std::uint64_t generation = 0;
            const auto previousElement = registry_.elements.find(entry.id);
            if (previousElement != registry_.elements.end() &&
                previousElement->second.nativeObserved &&
                previousElement->second.handle == handle) {
                generation = previousElement->second.desc.generation;
            } else {
                if (localDescription == std::numeric_limits<std::uint64_t>::max())
                    return MMS_E_NOT_READY;
                generation = localDescription++;
            }

            ElementRecord element;
            element.nativeObserved = true;
            element.handle = handle;
            element.desc.h = Header<MmsUiElementV1>();
            element.desc.id = intern(entry.id);
            element.desc.ownerPlugin = MmsString{kHostOwner, kHostOwnerBytes};
            element.desc.parentId = intern(entry.parentId);
            element.desc.label = intern(entry.label);
            element.desc.commandId = MmsString{};
            element.desc.propertyId = MmsString{};
            element.desc.surfaceId = MmsString{};
            element.desc.kind = entry.kind;
            element.desc.capabilities = 0;
            element.desc.generation = generation;
            element.arena = arena;
            element.defaultParent = entry.parentId;
            element.visible = true;
            if (!candidate.emplace(entry.id, std::move(element)).second)
                throw std::invalid_argument("catalog observation conflict");

            PublishedEntry& stored = next[handle];
            stored.handle = handle;
            stored.observation = true;
            stored.alias = false;
            stored.id = entry.id;
            stored.parentId = entry.parentId;
            stored.label = entry.label;
            stored.instance = entry.instance;
            stored.bounds = entry.bounds;
            stored.kind = entry.kind;
            stored.role = entry.role;
            stored.scope = entry.scope;
            stored.knownFields = entry.knownFields;
            stored.state = entry.state;
            stored.ordinal = entry.ordinal;
        }

        std::map<MmsHandle, const ElementRecord*> byHandle;
        for (const auto& item : candidate) {
            if (!byHandle.emplace(item.second.handle, &item.second).second)
                return MMS_E_STALE;
        }
        for (const auto& entry : publication.entries) {
            if (!entry.reuseElement) continue;
            const auto found = byHandle.find(entry.reuseElement);
            if (found == byHandle.end() || found->second->nativeObserved)
                throw std::invalid_argument("catalog reuse element");
            if (!SameText(found->second->desc.id, entry.id))
                throw std::invalid_argument("catalog reuse id");
            if (!next.emplace(entry.reuseElement, PublishedEntry{}).second)
                throw std::invalid_argument("catalog reuse conflict");
            PublishedEntry& stored = next[entry.reuseElement];
            stored.handle = entry.reuseElement;
            stored.observation = false;
            stored.alias = true;
            stored.id = entry.id;
            stored.parentId = entry.parentId;
            stored.label = entry.label;
            stored.instance = entry.instance;
            stored.bounds = entry.bounds;
            stored.kind = entry.kind;
            stored.role = entry.role;
            stored.scope = entry.scope;
            stored.knownFields = entry.knownFields;
            stored.state = entry.state;
            stored.ordinal = entry.ordinal;
        }

        std::map<std::string, std::string> effectiveParents;
        for (const auto& item : candidate) {
            const MmsString parent = item.second.desc.parentId;
            std::string value;
            if (parent.bytes) {
                if (!parent.data) throw std::invalid_argument("catalog parent");
                value.assign(parent.data, static_cast<std::size_t>(parent.bytes));
            }
            effectiveParents.emplace(item.first, std::move(value));
        }
        for (const auto& item : next) {
            const PublishedEntry& published = item.second;
            if (published.observation) {
                effectiveParents[published.id] = published.parentId;
            } else if (published.alias && !published.parentId.empty()) {
                effectiveParents[published.id] = published.parentId;
            }
        }
        // Bound all repeated parent walks, including validation before storage.
        // This is a work quota, not a claim about process peak memory.
        constexpr std::uint64_t kMaxParentSteps =
            kMaxSnapshotBytes / (2 * sizeof(PublishedAncestor) + 1);
        std::uint64_t parentSteps = 0;
        for (const auto& item : next) {
            std::set<std::string> seen;
            std::string current = item.second.id;
            std::uint64_t steps = 0;
            while (!current.empty()) {
                if (++parentSteps > kMaxParentSteps) return MMS_E_MEMORY;
                if (!seen.insert(current).second) throw std::invalid_argument("catalog cycle");
                const auto parent = effectiveParents.find(current);
                if (parent == effectiveParents.end() || parent->second.empty()) break;
                current = parent->second;
                if (++steps > effectiveParents.size()) throw std::invalid_argument("catalog cycle");
            }
        }
        std::uint64_t ancestryBudget = textBytes;
        for (auto& item : next) {
            PublishedEntry& entry = item.second;
            if (!entry.observation) continue;
            entry.parentHandle = 0;
            entry.ancestors.clear();
            if (entry.parentId.empty()) continue;
            std::string currentId = entry.parentId;
            std::uint64_t steps = 0;
            while (!currentId.empty()) {
                if (++parentSteps > kMaxParentSteps) return MMS_E_MEMORY;
                if (++steps > effectiveParents.size()) throw std::invalid_argument("catalog cycle");
                const auto parent = candidate.find(currentId);
                if (parent == candidate.end()) throw std::invalid_argument("catalog parent");
                PublishedAncestor ancestor;
                ancestor.id = currentId;
                ancestor.handle = parent->second.handle;
                // Pre-charge growth slack before push_back can allocate. All
                // ancestor copies share this publication-wide text/storage cap.
                if (!AddProduct(ancestryBudget, 2, sizeof(PublishedAncestor)) ||
                    !AddBytes(ancestryBudget, ancestor.id.capacity()) ||
                    !AddBytes(ancestryBudget, 1) || ancestryBudget > kMaxSnapshotBytes)
                    return MMS_E_MEMORY;
                entry.ancestors.push_back(std::move(ancestor));
                if (entry.ancestors.size() == 1) entry.parentHandle = parent->second.handle;
                const auto nextParent = effectiveParents.find(currentId);
                if (nextParent == effectiveParents.end() || nextParent->second.empty()) break;
                currentId = nextParent->second;
            }
        }
        std::uint64_t actualAncestryBytes = textBytes;
        for (const auto& item : next) {
            if (!AddProduct(actualAncestryBytes, item.second.ancestors.capacity(), sizeof(PublishedAncestor)))
                return MMS_E_MEMORY;
            for (const auto& ancestor : item.second.ancestors)
                if (!AddBytes(actualAncestryBytes, ancestor.id.capacity()) || !AddBytes(actualAncestryBytes, 1))
                    return MMS_E_MEMORY;
        }
        if (actualAncestryBytes > kMaxSnapshotBytes) return MMS_E_MEMORY;
        for (const auto& entry : publication.entries) {
            if (entry.reuseElement && !entry.parentId.empty() &&
                candidate.find(entry.parentId) == candidate.end())
                throw std::invalid_argument("catalog parent");
        }

        std::uint32_t available = publication.availableScopes & kNativeScopes;
        for (const auto& entry : publication.entries) available |= entry.scope;
        available &= kNativeScopes;
        const std::uint32_t coverage = publication.coverageFlags |
            MMS_UI_CATALOG_REGISTRY_COMPLETE | MMS_UI_CATALOG_INCOMPLETE_NATIVE;
        if (SamePublication(next, publication_) && available == availableScopes_ &&
            coverage == coverageFlags_)
            return MMS_OK;
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) return MMS_E_NOT_READY;

        registry_.elements.swap(candidate);
        registry_.next_ = localNext;
        description_ = localDescription;
        publication_.swap(next);
        availableScopes_ = available;
        coverageFlags_ = coverage;
        ++revision_;
        return MMS_OK;
    }

    void RetireInstance(std::uint64_t instance) noexcept {
        if (GetCurrentThreadId() != registry_.thread_) return;
        bool changed = false;
        for (auto it = registry_.elements.begin(); it != registry_.elements.end();) {
            const auto published = publication_.find(it->second.handle);
            if (published != publication_.end() && published->second.observation &&
                published->second.instance == instance) {
                it = registry_.elements.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        for (auto it = publication_.begin(); it != publication_.end();) {
            if (it->second.instance == instance) {
                it = publication_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) SaturateRevision();
    }

    void Suspend() noexcept {
        if (GetCurrentThreadId() != registry_.thread_) return;
        bool changed = false;
        for (auto it = registry_.elements.begin(); it != registry_.elements.end();) {
            if (it->second.nativeObserved) {
                it = registry_.elements.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (!publication_.empty()) {
            publication_.clear();
            changed = true;
        }
        if (availableScopes_) {
            availableScopes_ = 0;
            changed = true;
        }
        const std::uint32_t coverage =
            (coverageFlags_ & MMS_UI_CATALOG_REGISTRY_COMPLETE) |
            MMS_UI_CATALOG_INCOMPLETE_NATIVE;
        if (coverage != coverageFlags_) {
            coverageFlags_ = coverage;
            changed = true;
        }
        if (changed) SaturateRevision();
    }

    bool HasLeases(const UiClient& client) const noexcept {
        for (const auto& item : leases_) {
            if (item.second && item.second->owner == &client) return true;
        }
        return false;
    }
};

UiCatalog::UiCatalog(UiRegistry& registry)
    : impl_(std::make_unique<Impl>(registry)) {}

UiCatalog::~UiCatalog() = default;

MmsUiCatalogApiV1 UiCatalog::Api(UiClient& client) {
    Impl::ApiContext* apiContext = impl_->Context(client);
    MmsUiCatalogApiV1 api{};
    api.h = Header<MmsUiCatalogApiV1>();
    api.context = apiContext;
    api.getCapabilities = +[](void* raw, MmsUiCatalogCapabilitiesV1* out) -> MmsResult {
        auto* context = static_cast<Impl::ApiContext*>(raw);
        if (!context || !context->owner || !context->client) return MMS_E_ARGUMENT;
        if (!context->client->registry || context->client->registry != &context->owner->registry_)
            return MMS_E_ARGUMENT;
        return Boundary([&] { return context->owner->Capabilities(*context->client, out); });
    };
    api.acquireSnapshot = +[](void* raw, std::uint32_t scopes,
        MmsUiCatalogSnapshotV1* out) -> MmsResult {
        auto* context = static_cast<Impl::ApiContext*>(raw);
        if (!context || !context->owner || !context->client) return MMS_E_ARGUMENT;
        if (!context->client->registry || context->client->registry != &context->owner->registry_)
            return MMS_E_ARGUMENT;
        return Boundary([&] {
            return context->owner->AcquireSnapshot(*context->client, scopes, out);
        });
    };
    api.resolveElement = +[](void* raw, const MmsUiTargetRefV1* expected,
        MmsUiCatalogEntryV1* out, MmsHandle* lease) -> MmsResult {
        auto* context = static_cast<Impl::ApiContext*>(raw);
        if (!context || !context->owner || !context->client) return MMS_E_ARGUMENT;
        if (!context->client->registry || context->client->registry != &context->owner->registry_)
            return MMS_E_ARGUMENT;
        return Boundary([&] {
            return context->owner->Resolve(*context->client, expected, out, lease);
        });
    };
    api.releaseSnapshot = +[](void* raw, MmsHandle handle) -> MmsResult {
        auto* context = static_cast<Impl::ApiContext*>(raw);
        if (!context || !context->owner || !context->client) return MMS_E_ARGUMENT;
        if (!context->client->registry || context->client->registry != &context->owner->registry_)
            return MMS_E_ARGUMENT;
        return Boundary([&] {
            return context->owner->ReleaseLease(*context->client, handle);
        });
    };
    return api;
}

MmsResult UiCatalog::Publish(const HostUiCatalogPublication& publication) noexcept {
    return Boundary([&] { return impl_->Publish(publication); });
}

void UiCatalog::RetireInstance(std::uint64_t instance) noexcept {
    impl_->RetireInstance(instance);
}

void UiCatalog::Suspend() noexcept {
    impl_->Suspend();
}

bool UiCatalog::HasLeases(const UiClient& client) const noexcept {
    return impl_->HasLeases(client);
}

} // namespace mikumikustudio::plugins
