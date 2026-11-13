#pragma once

// Internal logical media identities. A ticket survives D3D device reset but
// never survives a media replacement, scene reset, or application teardown.
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <utility>
#include "mikumikustudio/background_picture_asset.hpp"

namespace mikumikustudio::media { class BackgroundPictureSource; }

namespace mikumikustudio::plugins::background_identity {
enum class Kind : unsigned char { Picture, Video };
// New-project reset hides but retains the native picture texture. Preserve
// its immutable asset too, while always invalidating old activation tickets.
enum class PictureRetention { Discard, RetainForSceneReset };
struct Key {
    const void* app;
    Kind kind;
    bool operator==(const Key& other) const noexcept {
        return app == other.app && kind == other.kind;
    }
};
struct Hash {
    std::size_t operator()(const Key& key) const noexcept {
        return std::hash<const void*>{}(key.app) ^ static_cast<unsigned>(key.kind);
    }
};
struct Registry {
    struct Owner {
        std::uint64_t serial;
        DWORD thread;
        media::BackgroundPictureAssetPtr pictureAsset;
        std::uint64_t pictureTicket = 0;
    };
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<Key, std::uint64_t, Hash> entries;
    std::unordered_map<const void*, Owner> registered;
    std::unordered_set<const void*> closing;
    std::uint64_t last = 0;
    std::uint64_t activePicture = 0, activeVideo = 0;
    std::uint64_t mutationSerial = 0;
    std::uint64_t pictureMutationSerial = 0;
    bool poisoned = false;
};
inline Registry& GetRegistry() {
    static Registry value;
    return value;
}
struct Guard {
    SRWLOCK& lock;
    explicit Guard(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~Guard() { ReleaseSRWLockExclusive(&lock); }
};
// Zero signals invalid input, exhaustion or allocation failure. A failed
// acquisition cannot make an earlier ticket current again.
inline std::uint64_t Acquire(const void* app, Kind kind) noexcept {
    if (!app) return 0;
    auto& registry = GetRegistry();
    Guard guard(registry.lock);
    if (registry.poisoned || !registry.registered.count(app) || registry.closing.count(app) ||
        (kind == Kind::Picture ? registry.activePicture : registry.activeVideo)) return 0;
    if (kind == Kind::Picture) {
        auto& ticket = registry.registered.find(app)->second.pictureTicket;
        if (ticket) return ticket;
        if (registry.last == (std::numeric_limits<std::uint64_t>::max)()) return 0;
        ticket = ++registry.last;
        return ticket;
    }
    const Key key{app, kind};
    auto found = registry.entries.find(key);
    if (found != registry.entries.end()) return found->second;
    if (registry.last == (std::numeric_limits<std::uint64_t>::max)()) return 0;
    const auto next = registry.last + 1;
    try { registry.entries.emplace(key, next); }
    catch (...) { return 0; }
    registry.last = next;
    return next;
}
inline bool IsCurrent(const void* app, Kind kind, std::uint64_t ticket) noexcept {
    if (!app || !ticket) return false;
    auto& registry = GetRegistry();
    Guard guard(registry.lock);
    if (registry.poisoned || !registry.registered.count(app) || registry.closing.count(app) ||
        (kind == Kind::Picture ? registry.activePicture : registry.activeVideo)) return false;
    if (kind == Kind::Picture) return registry.registered.find(app)->second.pictureTicket == ticket;
    const auto found = registry.entries.find(Key{app, kind});
    return found != registry.entries.end() && found->second == ticket;
}
inline void Invalidate(const void* app, Kind kind) noexcept {
    if (!app) return;
    media::BackgroundPictureAssetPtr retired;
    auto& registry = GetRegistry();
    Guard guard(registry.lock);
    const auto owner = registry.registered.find(app);
    if (kind == Kind::Picture) {
        if (owner != registry.registered.end()) {
            if (registry.pictureMutationSerial == (std::numeric_limits<std::uint64_t>::max)()) registry.poisoned = true;
            else ++registry.pictureMutationSerial;
            owner->second.pictureTicket = 0;
            retired.swap(owner->second.pictureAsset);
        }
    } else registry.entries.erase(Key{app, kind});
}
inline void Forget(const void* app) noexcept {
    if (!app) return;
    media::BackgroundPictureAssetPtr retired;
    auto& registry = GetRegistry();
    Guard guard(registry.lock);
    registry.entries.erase(Key{app, Kind::Video});
    const auto owner = registry.registered.find(app);
    if (owner != registry.registered.end()) {
        if (registry.pictureMutationSerial == (std::numeric_limits<std::uint64_t>::max)()) registry.poisoned = true;
        else ++registry.pictureMutationSerial;
        owner->second.pictureTicket = 0;
        retired.swap(owner->second.pictureAsset);
    }
}
// Register only after construction and outside every media mutation. A live
// owner cannot register again. A closed address receives fresh media tickets;
// tickets are process-unique and never reissued, including after address reuse.
inline bool Register(const void* app) noexcept {
    if (!app) return false;
    const DWORD thread = GetCurrentThreadId();
    media::BackgroundPictureAssetPtr retired;
    auto& r = GetRegistry();
    Guard guard(r.lock);
    if (r.poisoned) return false;
    const bool known = r.registered.count(app) != 0;
    if (known && !r.closing.count(app)) {
        // This may be an old object destroyed without ShutdownCleanup and a
        // new object at the same address. Never retain the old identity when
        // the caller ignores the failure. Tombstone allocation failure must
        // disable the entire registry instead of reviving the old tickets.
        r.entries.erase(Key{app, Kind::Video});
        r.registered.find(app)->second.pictureTicket = 0;
        retired.swap(r.registered.find(app)->second.pictureAsset);
        try { r.closing.insert(app); } catch (...) { r.poisoned = true; }
        return false;
    }
    if (r.activePicture || r.activeVideo) return false;
    if (r.last == (std::numeric_limits<std::uint64_t>::max)()) return false;
    const auto instance = r.last + 1;
    if (!known) {
        try { r.registered.emplace(app, Registry::Owner{instance, thread}); } catch (...) { return false; }
    } else {
        retired.swap(r.registered.find(app)->second.pictureAsset);
        r.registered.find(app)->second = Registry::Owner{instance, thread};
    }
    r.last = instance;
    r.entries.erase(Key{app, Kind::Video});
    r.closing.erase(app);
    return true;
}
inline std::uint64_t Instance(const void* app) noexcept {
    if (!app) return 0;
    auto& r = GetRegistry();
    Guard guard(r.lock);
    if (r.poisoned || r.closing.count(app)) return 0;
    const auto found = r.registered.find(app);
    return found == r.registered.end() ? 0 : found->second.serial;
}
inline bool IsOwner(const void* app, std::uint64_t instance) noexcept {
    if (!app || !instance) return false;
    const DWORD thread = GetCurrentThreadId();
    auto& r = GetRegistry(); Guard guard(r.lock);
    if (r.poisoned || r.closing.count(app)) return false;
    const auto found = r.registered.find(app);
    return found != r.registered.end() && found->second.serial == instance && found->second.thread == thread;
}

// Immutable encoded bytes belong to the currently published picture. A lease
// survives retirement, but a new activation must never discover the old source.
// This is an internal resource prerequisite, not an undo record or public ABI.
inline media::BackgroundPictureAssetPtr CurrentPictureAsset(
    const void* app, std::uint64_t instance, std::uint64_t ticket) noexcept {
    if (!app || !instance || !ticket) return {};
    const DWORD thread = GetCurrentThreadId();
    auto& r = GetRegistry(); Guard guard(r.lock);
    const auto owner = r.registered.find(app);
    if (r.poisoned || r.activePicture || r.closing.count(app) || owner == r.registered.end() ||
        owner->second.serial != instance || owner->second.thread != thread ||
        owner->second.pictureTicket != ticket) return {};
    return owner->second.pictureAsset;
}

inline media::BackgroundPictureSourcePtr CurrentPictureSource(
    const void* app, std::uint64_t instance, std::uint64_t ticket) noexcept {
    const auto asset = CurrentPictureAsset(app, instance, ticket);
    return asset ? asset->Source() : media::BackgroundPictureSourcePtr{};
}

// Read-only lookup used by long-lived history records. Unlike Acquire it never
// installs an activation while a record is merely checking its current side.
inline std::uint64_t CurrentPictureTicket(const void* app, std::uint64_t instance, bool* mutating = nullptr) noexcept {
    if (mutating) *mutating = false;
    if (!app || !instance) return 0;
    auto& r = GetRegistry(); Guard guard(r.lock);
    const auto owner = r.registered.find(app);
    if (r.poisoned || r.closing.count(app) || owner == r.registered.end() ||
        owner->second.serial != instance || owner->second.thread != GetCurrentThreadId()) return 0;
    if (r.activePicture) { if (mutating) *mutating = true; return 0; }
    return owner->second.pictureTicket;
}

// Observe a complete logical picture without activating an unqueried slot.
// Scene reset can retain an asset/texture with ticket zero; both that state
// and an empty never-activated picture are valid snapshots. Failure leaves
// out unchanged, including its asset lease; no allocation or ticket is used.
struct PictureSnapshot {
    std::uint64_t ticket = 0, pictureMutationSerial = 0;
    media::BackgroundPictureAssetPtr asset;
};
inline bool QueryPictureSnapshot(const void* app, std::uint64_t instance,
    PictureSnapshot& out, bool* mutating = nullptr) noexcept {
    if (mutating) *mutating = false;
    if (!app || !instance) return false;
    PictureSnapshot next;
    {
        auto& r = GetRegistry(); Guard guard(r.lock);
        const auto owner = r.registered.find(app);
        if (r.poisoned || r.closing.count(app) || owner == r.registered.end() ||
            owner->second.serial != instance || owner->second.thread != GetCurrentThreadId()) return false;
        if (r.activePicture) { if (mutating) *mutating = true; return false; }
        next.ticket = owner->second.pictureTicket;
        next.pictureMutationSerial = r.pictureMutationSerial;
        next.asset = owner->second.pictureAsset;
    }
    // A caller's previous lease may be the final reference. Release it only
    // after the registry lock has been dropped.
    out = std::move(next);
    return true;
}

// A private preparation reserves an identity without making it active. The
// owner slot already exists, so successful publication never inserts a node.
// Copies are still single-use: changing the active ticket invalidates every
// competing receipt, including copies of the successful one.
struct PicturePreparation {
    const void* app = nullptr;
    std::uint64_t instance = 0, expectedTicket = 0, reservedTicket = 0;
    std::uint64_t pictureMutationSerial = 0;
    media::BackgroundPictureAssetPtr expectedAsset;
};
inline bool PreparePictureActivation(const void* app, std::uint64_t instance,
    PicturePreparation& out) noexcept {
    if (!app || !instance) return false;
    const DWORD thread = GetCurrentThreadId();
    PicturePreparation next;
    {
        auto& r = GetRegistry(); Guard guard(r.lock);
        const auto owner = r.registered.find(app);
        if (r.poisoned || r.activePicture || r.closing.count(app) || owner == r.registered.end() ||
            owner->second.serial != instance || owner->second.thread != thread ||
            r.last == (std::numeric_limits<std::uint64_t>::max)() ||
            r.pictureMutationSerial == (std::numeric_limits<std::uint64_t>::max)()) return false;
        next.app = app; next.instance = instance; next.expectedTicket = owner->second.pictureTicket;
        next.reservedTicket = ++r.last; next.pictureMutationSerial = r.pictureMutationSerial;
        next.expectedAsset = owner->second.pictureAsset;
    }
    // Release the caller's former receipt/asset only after unlocking.
    out = std::move(next);
    return true;
}
namespace detail {
// The caller holds Registry::lock. This helper never calls out or allocates.
inline bool PicturePreparationCurrentLocked(const Registry& r, const PicturePreparation& receipt,
    DWORD thread) noexcept {
    if (!receipt.app || !receipt.instance ||
        receipt.reservedTicket <= receipt.expectedTicket || receipt.reservedTicket > r.last) return false;
    const auto owner = r.registered.find(receipt.app);
    return !r.poisoned && !r.activePicture && r.pictureMutationSerial != (std::numeric_limits<std::uint64_t>::max)() &&
        !r.closing.count(receipt.app) && owner != r.registered.end() &&
        owner->second.serial == receipt.instance && owner->second.thread == thread &&
        owner->second.pictureTicket == receipt.expectedTicket && r.pictureMutationSerial == receipt.pictureMutationSerial &&
        owner->second.pictureAsset == receipt.expectedAsset;
}
}
inline bool PicturePreparationCurrent(const PicturePreparation& receipt) noexcept {
    const DWORD thread = GetCurrentThreadId();
    auto& r = GetRegistry(); Guard guard(r.lock);
    return detail::PicturePreparationCurrentLocked(r, receipt, thread);
}
template<class T>
bool PublishPreparedPicture(PicturePreparation& receipt, T*& texture, T* candidate, T*& previous,
    wchar_t* path, const wchar_t* nextPath, std::int32_t& width, std::int32_t nextWidth,
    std::int32_t& height, std::int32_t nextHeight, std::int32_t& x, std::int32_t& y,
    float& scale, std::int32_t nextX, std::int32_t nextY, float nextScale,
    std::uint8_t& enabled, std::uint8_t nextEnabled, media::BackgroundPictureAssetPtr asset) noexcept {
    if (!path || !nextPath || nextEnabled > 1) return false;
    std::size_t length = 0;
    while (length < 256 && nextPath[length]) ++length;
    if (length == 256) return false;
    if (candidate || asset) {
        if (!candidate || !asset || !length || nextWidth != asset->Width() || nextHeight != asset->Height() ||
            std::wcscmp(nextPath, asset->Path()) != 0) return false;
    } else if (length || nextWidth || nextHeight || nextEnabled) return false;
    const DWORD thread = GetCurrentThreadId();
    auto& r = GetRegistry(); Guard guard(r.lock);
    if (!detail::PicturePreparationCurrentLocked(r, receipt, thread)) return false;
    auto& owner = r.registered.find(receipt.app)->second;
    previous = texture; texture = candidate;
    for (std::size_t i = 0; i <= length; ++i) path[i] = nextPath[i];
    width = nextWidth; height = nextHeight; x = nextX; y = nextY;
    std::memcpy(&scale, &nextScale, sizeof(float)); enabled = nextEnabled;
    owner.pictureAsset.swap(asset);
    owner.pictureTicket = receipt.reservedTicket;
    ++r.pictureMutationSerial;
    receipt.reservedTicket = 0;
    // The by-value old asset is released after Guard, never under the lock.
    return true;
}

// Callers pass only the owning app's direct placement fields. The caller is
// responsible for their lifetime and must not publish pointers from plugins.
// No callback, allocation, or window operation runs while the lock is held.
struct PlacementFields { std::int32_t* x; std::int32_t* y; float* scale; };
struct Placement { std::int32_t x, y; float scale; };
enum class PlacementResult { Ok, Invalid, WrongThread, Stale, Conflict };
inline PlacementResult PlacementAccess(const void* app, Kind kind,
    std::uint64_t instance, std::uint64_t ticket, PlacementFields fields,
    Placement& observed, const Placement* expected = nullptr,
    const Placement* replacement = nullptr, unsigned mask = 0) noexcept {
    if (!app || !instance || !ticket || !fields.x || !fields.y || !fields.scale ||
        (mask & ~7u) || ((expected == nullptr) != (replacement == nullptr)) ||
        (!replacement && mask)) return PlacementResult::Invalid;
    const DWORD thread = GetCurrentThreadId();
    auto& r = GetRegistry();
    Guard guard(r.lock);
    const auto owner = r.registered.find(app);
    if (r.poisoned || owner == r.registered.end() || r.closing.count(app) ||
        owner->second.serial != instance ||
        (kind == Kind::Picture ? r.activePicture : r.activeVideo)) return PlacementResult::Stale;
    if (owner->second.thread != thread) return PlacementResult::WrongThread;
    if (kind == Kind::Picture) {
        if (owner->second.pictureTicket != ticket) return PlacementResult::Stale;
    } else {
        const auto entry = r.entries.find(Key{app, kind});
        if (entry == r.entries.end() || entry->second != ticket) return PlacementResult::Stale;
    }
    // Inputs may alias the output. Snapshot them before publishing observed.
    const Placement expectedValue = expected ? *expected : Placement{};
    const Placement replacementValue = replacement ? *replacement : Placement{};
    Placement current{*fields.x, *fields.y, *fields.scale};
    observed = current;
    if (!replacement) return PlacementResult::Ok;
    if (((mask & 1) && current.x != expectedValue.x) ||
        ((mask & 2) && current.y != expectedValue.y) ||
        ((mask & 4) && std::memcmp(&current.scale, &expectedValue.scale, sizeof(float))))
        return PlacementResult::Conflict;
    if (mask & 1) *fields.x = replacementValue.x;
    if (mask & 2) *fields.y = replacementValue.y;
    if (mask & 4) std::memcpy(fields.scale, &replacementValue.scale, sizeof(float));
    return PlacementResult::Ok;
}
// Scopes use conservative process-wide per-kind gates. This requires no
// allocation at mutation time and makes nested message loops fail closed.
// Closing owners remain tombstoned until a new owner explicitly registers.
class Mutation final {
public:
    Mutation(const void* app, bool picture, bool video, bool closing = false,
        bool preservePictureUntilPublish = false,
        PictureRetention retention = PictureRetention::Discard) noexcept
        : picture_(picture || closing), video_(video || closing), app_(app), preservePicture_(preservePictureUntilPublish) {
        // Closing always ends both resource domains, even if a caller only
        // names one kind; a retention preference must never survive teardown.
        picture = picture_; video = video_;
        auto& r = GetRegistry();
        Guard guard(r.lock);
        const auto initialOwner = r.registered.find(app);
        if (initialOwner != r.registered.end()) ownerInstance_ = initialOwner->second.serial;
        // Scene reset always retires activation, even if a caller also asks
        // to keep the old picture until a later publication.
        if (picture && (!preservePicture_ || closing || retention == PictureRetention::RetainForSceneReset)) {
            const auto owner = r.registered.find(app);
            if (owner != r.registered.end()) {
                owner->second.pictureTicket = 0;
                if (closing || retention != PictureRetention::RetainForSceneReset)
                    retiredPictureAsset_.swap(owner->second.pictureAsset);
            }
        }
        if (video) r.entries.erase(Key{app, Kind::Video});
        if (closing) {
            try { r.closing.insert(app); } catch (...) { r.poisoned = true; }
        }
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (r.mutationSerial == maximum || (picture && (r.activePicture == maximum || r.pictureMutationSerial == maximum)) ||
            (video && r.activeVideo == maximum)) {
            r.poisoned = true;
            picture_ = video_ = false;
            return;
        }
        if (picture) { ++r.activePicture; pictureSerial_ = ++r.pictureMutationSerial; }
        if (video) ++r.activeVideo;
        serial_ = ++r.mutationSerial;
    }
    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;
    bool Current() const noexcept {
        auto& r = GetRegistry(); Guard guard(r.lock);
        return serial_ && !r.poisoned && r.mutationSerial == serial_;
    }
    // Project loading may perform legitimate nested AVI mutations before its
    // picture stage. This authorizes a new nested picture scope, not a direct
    // publication through this older scope and never an ungated Acquire. Any
    // newer picture scope, owner replacement or completed publication ends it.
    bool AllowsProjectPicture(const void* app) const noexcept {
        if (!app || app != app_ || !picture_ || !serial_ || !ownerInstance_ || published_) return false;
        const DWORD thread = GetCurrentThreadId();
        auto& r = GetRegistry(); Guard guard(r.lock);
        const auto owner = r.registered.find(app);
        return !r.poisoned && r.activePicture && r.pictureMutationSerial == pictureSerial_ && !r.closing.count(app) &&
            owner != r.registered.end() && owner->second.serial == ownerInstance_ && owner->second.thread == thread;
    }
    // Called only for a fully prepared picture, with stable native fields.
    // All publication is raw data swapping under the same lock as Mutation.
    // The caller releases the old texture and refreshes UI after unlocking.
    template<class T>
    bool PublishPicture(const void* app, T*& texture, T* candidate, T*& previous,
        wchar_t* path, const wchar_t* nextPath, std::int32_t& width, std::int32_t nextWidth,
        std::int32_t& height, std::int32_t nextHeight, std::int32_t& x, std::int32_t& y,
        float& scale, std::int32_t nextY, float nextScale, std::uint8_t& enabled,
        media::BackgroundPictureAssetPtr asset = {}, std::int32_t nextX = 0,
        std::uint8_t nextEnabled = 1) noexcept {
        if (!app || app != app_ || !picture_ || !serial_ || !candidate || !path || !nextPath || nextEnabled > 1) return false;
        const DWORD thread = GetCurrentThreadId();
        auto& r = GetRegistry(); Guard guard(r.lock);
        const auto owner = r.registered.find(app);
        if (r.poisoned || published_ || r.mutationSerial != serial_ || owner == r.registered.end() ||
            owner->second.thread != thread || r.closing.count(app)) return false;
        std::size_t length = 0;
        while (length < 256 && nextPath[length]) ++length;
        if (length == 256) return false;
        if (asset && (asset->Width() != nextWidth || asset->Height() != nextHeight ||
            std::wcscmp(asset->Path(), nextPath) != 0)) return false;
        if (preservePicture_) owner->second.pictureTicket = 0;
        previous = texture; texture = candidate;
        for (std::size_t i = 0; i <= length; ++i) path[i] = nextPath[i];
        width = nextWidth; height = nextHeight; x = nextX; y = nextY;
        scale = nextScale; enabled = nextEnabled;
        // The old asset's final release is after Guard destruction. Keeping
        // it in the by-value parameter also leaves failed publication intact.
        owner->second.pictureAsset.swap(asset);
        published_ = true;
        return true;
    }
    // Clear native data atomically, handing the old COM reference to the
    // caller without invoking it. Asset destruction occurs after unlocking.
    template<class T>
    bool ClearPicture(const void* app, T*& texture, T*& previous, wchar_t* path,
        std::int32_t& width, std::int32_t& height, std::int32_t& x, std::int32_t& y,
        float& scale, std::uint8_t& enabled) noexcept {
        if (!app || app != app_ || !picture_ || !serial_ || !path) return false;
        const DWORD thread = GetCurrentThreadId();
        media::BackgroundPictureAssetPtr retired;
        auto& r = GetRegistry(); Guard guard(r.lock);
        const auto owner = r.registered.find(app);
        if (r.poisoned || published_ || r.mutationSerial != serial_ || owner == r.registered.end() ||
            owner->second.thread != thread || r.closing.count(app)) return false;
        owner->second.pictureTicket = 0;
        previous = texture; texture = nullptr;
        path[0] = L'\0'; width = height = x = y = 0; scale = 1.f; enabled = 0;
        retired.swap(owner->second.pictureAsset);
        published_ = true;
        return true;
    }
    ~Mutation() noexcept {
        auto& r = GetRegistry();
        Guard guard(r.lock);
        if (picture_) --r.activePicture;
        if (video_) --r.activeVideo;
    }
private:
    bool picture_, video_;
    const void* app_;
    bool preservePicture_;
    bool published_ = false;
    // This member is destroyed after the destructor body releases the lock.
    media::BackgroundPictureAssetPtr retiredPictureAsset_;
    std::uint64_t serial_ = 0;
    std::uint64_t pictureSerial_ = 0, ownerInstance_ = 0;
};
}
