#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"

#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mikumikustudio::mdl {
namespace {

struct Entry {
    std::size_t capacity = 0;
    // One byte per slot, allocated by growth only.  Empty while the capacity is
    // the original one, because ModelRecord::keyVisitMap already covers that.
    std::vector<unsigned char> visit;
};

std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<const BoneKey*, Entry>& Registry() {
    static std::unordered_map<const BoneKey*, Entry> registry;
    return registry;
}

// True when the registry owns this pool, i.e. BoneKeyPoolCreate allocated it.
bool Registered(const BoneKey* pool) {
    if (pool == nullptr) return false;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    return Registry().find(pool) != Registry().end();
}

}  // namespace

BoneKey* BoneKeyPoolCreate(std::size_t capacity) {
    if (capacity == 0) return nullptr;
    if (capacity > kBoneKeyCapacityLimit) throw std::bad_alloc();

    BoneKey* pool =
        static_cast<BoneKey*>(std::calloc(capacity, sizeof(BoneKey)));
    if (pool == nullptr) throw std::bad_alloc();

    Entry entry;
    entry.capacity = capacity;
    if (capacity > kBoneKeyCapacity) {
        // Keep the invariant "capacity above the original implies the side visit
        // map exists" true from the first allocation.
        try {
            entry.visit.assign(capacity, 0);
        } catch (...) {
            std::free(pool);
            throw;
        }
    }

    try {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        Registry()[pool] = std::move(entry);
    } catch (...) {
        std::free(pool);
        throw;
    }
    return pool;
}

void BoneKeyPoolDestroy(BoneKey*& pool) {
    if (pool == nullptr) return;
    BoneKey* const released = pool;
    pool = nullptr;
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        const auto found = Registry().find(released);
        if (found != Registry().end()) {
            Registry().erase(found);
            owned = true;
        }
    }
    // Only the storage that BoneKeyPoolCreate allocated is released.  A pool the
    // registry does not own came from another owner, which keeps the duty to
    // free it; the entry above is still erased so a recycled address cannot
    // match a stale record.
    if (owned) std::free(released);
}

std::size_t BoneKeyCapacity(const BoneKey* pool) {
    if (pool == nullptr) return 0;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(pool);
    return found == Registry().end() ? kBoneKeyCapacity : found->second.capacity;
}

std::size_t BoneKeyCapacity(const ModelRecord& model) {
    return BoneKeyCapacity(model.boneKeys);
}

std::size_t BoneKeyPlanCapacity(std::size_t currentCapacity,
                                std::uint64_t slotsNeeded) {
    if (slotsNeeded <= currentCapacity) return currentCapacity;
    if (slotsNeeded > kBoneKeyCapacityLimit) return 0;

    std::size_t target = static_cast<std::size_t>(slotsNeeded);
    // Headroom for the next interactive insertions, clamped to the ceiling.
    const std::size_t geometric =
        currentCapacity + currentCapacity / 2 + kBoneKeyGrowthQuantum;
    if (geometric > target && geometric <= kBoneKeyCapacityLimit)
        target = geometric;
    if (target > kBoneKeyCapacityLimit) target = kBoneKeyCapacityLimit;

    const std::size_t remainder = target % kBoneKeyGrowthQuantum;
    if (remainder != 0) {
        const std::size_t bump = kBoneKeyGrowthQuantum - remainder;
        target = target <= kBoneKeyCapacityLimit - bump
                     ? target + bump
                     : kBoneKeyCapacityLimit;
    }
    return target;
}

std::size_t BoneKeyPlanCapacity(const ModelRecord& model,
                                std::uint64_t slotsNeeded) {
    return BoneKeyPlanCapacity(BoneKeyCapacity(model.boneKeys), slotsNeeded);
}

bool BoneKeyPoolEnsure(ModelRecord& model, std::uint64_t slotsNeeded) {
    BoneKey* const current = model.boneKeys;
    const bool owned = Registered(current);
    const std::size_t capacity = BoneKeyCapacity(current);
    if (slotsNeeded <= capacity) return true;

    const std::size_t target = BoneKeyPlanCapacity(capacity, slotsNeeded);
    if (target == 0 || target <= capacity) return false;  // over the ceiling

    BoneKey* const grown =
        static_cast<BoneKey*>(std::calloc(target, sizeof(BoneKey)));
    if (grown == nullptr) return false;
    if (current != nullptr && capacity != 0)
        std::memcpy(grown, current, capacity * sizeof(BoneKey));

    Entry entry;
    entry.capacity = target;
    if (target > kBoneKeyCapacity) {
        try {
            entry.visit.assign(target, 0);
        } catch (...) {
            std::free(grown);
            return false;
        }
        // Carry the visited marks of every existing slot over: a growth in the
        // middle of a batch registration or an undo transaction must not lose
        // them, or the same slot could be appended twice.  Slots the growth adds
        // start unvisited, which is what assign(..., 0) left for them.  The
        // previous map is the inline array while the previous capacity was the
        // original one, and the side buffer above it.
        if (capacity != 0) {
            const unsigned char* previous = nullptr;
            if (capacity <= kBoneKeyCapacity) {
                previous = model.keyVisitMap;
            } else {
                std::lock_guard<std::mutex> lock(RegistryMutex());
                const auto found = Registry().find(current);
                if (found != Registry().end() &&
                    found->second.visit.size() >= capacity)
                    previous = found->second.visit.data();
            }
            if (previous != nullptr)
                std::memcpy(entry.visit.data(), previous, capacity);
        }
    }

    // Commit: publish the new block and its capacity.  Everything above may
    // fail, everything below cannot, so a failure never touches the model.
    try {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        Registry()[grown] = std::move(entry);
        Registry().erase(current);
    } catch (...) {
        std::free(grown);
        return false;
    }
    model.boneKeys = grown;
    if (owned) std::free(current);
    return true;
}

BoneKeyVisit BoneKeyVisitMapOf(ModelRecord& model) {
    return BoneKeyVisitMapOf(model, BoneKeyCapacity(model.boneKeys));
}

BoneKeyVisit BoneKeyVisitMapOf(ModelRecord& model, std::size_t capacity) {
    if (capacity <= kBoneKeyCapacity) return {model.keyVisitMap, capacity};
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(model.boneKeys);
    if (found == Registry().end() ||
        found->second.capacity != capacity ||
        found->second.visit.size() < capacity) {
        // The requested capacity does not belong to this pool (it came from a
        // stale value or another model): handing back the shorter inline array
        // under a larger capacity is exactly what would make the caller clear or
        // index past its end.  Report the inline array with the length it really
        // has instead, so pointer and length always agree.
        assert(false && "bone-key visit map does not cover the requested capacity");
        return {model.keyVisitMap, kBoneKeyCapacity};
    }
    // Valid until the same pool grows again; callers must not hold it across a
    // BoneKeyPoolEnsure call, the same rule as for any cached pool pointer.
    return {found->second.visit.data(), capacity};
}

std::size_t BoneKeyVisitMapSize(const ModelRecord& model) {
    const std::size_t capacity = BoneKeyCapacity(model.boneKeys);
    if (capacity <= kBoneKeyCapacity) return capacity;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(model.boneKeys);
    if (found == Registry().end() || found->second.capacity != capacity ||
        found->second.visit.size() < capacity)
        return kBoneKeyCapacity;  // the length of the inline array it falls back to
    return capacity;
}

void BoneKeyVisitClear(ModelRecord& model) {
    const BoneKeyVisit visit = BoneKeyVisitMapOf(model);
    if (visit.size != 0) std::memset(visit.bytes, 0, visit.size);
}


// Morph pools retain the model ABI and all slot indices across growth.
// Borrowed test/SDK pools are copied but never released by this owner.
namespace {
struct MorphEntry { std::size_t capacity; std::uint64_t identity; };
std::unordered_map<const MorphKey*, MorphEntry>& MorphRegistry() {
    static std::unordered_map<const MorphKey*, MorphEntry> registry;
    return registry;
}
}

MorphKey* MorphKeyPoolCreate(std::size_t capacity) {
    if (!capacity) return nullptr;
    if (capacity > kMorphKeyCapacityLimit) throw std::bad_alloc();
    auto* pool = static_cast<MorphKey*>(std::calloc(capacity, sizeof(MorphKey)));
    if (!pool) throw std::bad_alloc();
    try {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        static std::uint64_t nextIdentity = 0;
        MorphRegistry().emplace(pool, MorphEntry{capacity, ++nextIdentity});
    } catch (...) { std::free(pool); throw; }
    return pool;
}

std::size_t MorphKeyCapacity(const MorphKey* pool) {
    if (!pool) return 0;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = MorphRegistry().find(pool);
    return found == MorphRegistry().end() ? kMorphKeyCapacity : found->second.capacity;
}

std::size_t MorphKeyCapacity(const ModelRecord& model) {
    return MorphKeyCapacity(model.morphKeys);
}

std::uint64_t MorphKeyPoolIdentity(const MorphKey* pool) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = MorphRegistry().find(pool);
    return found == MorphRegistry().end() ? 0 : found->second.identity;
}

void MorphKeyPoolDestroy(MorphKey*& pool) {
    if (!pool) return;
    bool owned;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        owned = MorphRegistry().erase(pool) != 0;
    }
    if (owned) std::free(pool);
    pool = nullptr;
}

bool MorphKeyPoolEnsure(ModelRecord& model, std::uint64_t slotsNeeded) {
    const std::size_t capacity = MorphKeyCapacity(model);
    if (slotsNeeded <= capacity) return true;
    if (slotsNeeded > kMorphKeyCapacityLimit) return false;
    const std::uint64_t geometric = std::uint64_t(capacity) + capacity / 2 + 4096;
    const std::size_t target = static_cast<std::size_t>(
        (std::min)(std::uint64_t(kMorphKeyCapacityLimit), (std::max)(slotsNeeded, geometric)));
    MorphKey* grown;
    try { grown = MorphKeyPoolCreate(target); }
    catch (const std::bad_alloc&) { return false; }
    if (capacity) std::memcpy(grown, model.morphKeys, capacity * sizeof(MorphKey));
    const auto identity = MorphKeyPoolIdentity(model.morphKeys);
    if (identity) {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        MorphRegistry().at(grown).identity = identity;
    }
    MorphKeyPoolDestroy(model.morphKeys);
    model.morphKeys = grown;
    return true;
}

bool DisplayKeyPoolEnsure(ModelRecord& model, std::uint64_t required) noexcept {
    auto*& pool = model.displayKeys;
    const auto capacity = KeyPoolCapacity(pool);
    if (required <= capacity) return true;
    if (required > KeyPoolLimit<DisplayKey>()) return false;
    const auto target = static_cast<std::size_t>((std::min)(
        std::uint64_t(KeyPoolLimit<DisplayKey>()),
        (std::max)(required, std::uint64_t(capacity) + capacity / 2 + 256)));
    DisplayKey* grown = nullptr;
    try {
        grown = KeyPoolCreate<DisplayKey>(target);
        for (std::size_t i = capacity; i < target; ++i) {
            auto& key = grown[i];
            key.visible = 1;
            if (model.ikChainCount) {
                key.ikStates = static_cast<std::uint8_t*>(std::malloc(model.ikChainCount));
                if (!key.ikStates) throw std::bad_alloc();
                std::memset(key.ikStates, 1, model.ikChainCount);
            }
            if (model.boneOrderCount) {
                const auto count = std::size_t(model.boneOrderCount);
                if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(BoneReference))
                    throw std::bad_alloc();
                key.selectorStates = static_cast<BoneReference*>(std::calloc(count, sizeof(BoneReference)));
                if (!key.selectorStates) throw std::bad_alloc();
                auto* parents = static_cast<BoneReference*>(key.selectorStates);
                for (std::size_t p = 0; p < count; ++p) parents[p].modelIndex = -1;
            }
        }
    } catch (...) {
        if (grown) {
            for (std::size_t i = capacity; i < target; ++i) {
                std::free(grown[i].ikStates);
                std::free(grown[i].selectorStates);
            }
            KeyPoolDestroy(grown);
        }
        return false;
    }
    if (capacity) std::memcpy(grown, pool, capacity * sizeof(DisplayKey));
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(KeyPoolStorage<DisplayKey>::mutex);
        owned = KeyPoolStorage<DisplayKey>::capacities.erase(pool) != 0;
    }
    auto* previous = pool;
    pool = grown;
    if (owned) ::operator delete(previous);
    return true;
}

bool DisplayKeyFreeSlot(ModelRecord& model, std::uint32_t& slot) noexcept {
    const auto capacity = KeyPoolCapacity(model.displayKeys);
    std::size_t index = 1;
    while (index < capacity && model.displayKeys[index].frame) ++index;
    if (index >= capacity && !DisplayKeyPoolEnsure(model, std::uint64_t(index) + 1)) return false;
    slot = static_cast<std::uint32_t>(index);
    return true;
}

}  // namespace mikumikustudio::mdl
