#include "mikudancestudio/bone_key_pool.hpp"
#include "../plugins/scene_resource_budget.hpp"
#include "mikudancestudio/key_pool.hpp"

#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mikudancestudio::mdl {
namespace {

struct Entry {
    std::size_t capacity = 0;
    std::uint64_t identity = 0;
    bool visible = true;
    const void* token = nullptr;
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

// Called with RegistryMutex held. A fresh allocation consumes an identity only
// after its entry is installed. Zero remains reserved for borrowed pools; after
// exhaustion, new ownership is refused rather than recycling an old identity.
void RegisterEntry(const BoneKey* pool, Entry entry) {
    static std::uint64_t nextIdentity = 1;
    const bool fresh = entry.identity == 0;
    if (fresh) {
        if (!nextIdentity) throw std::bad_alloc();
        entry.identity = nextIdentity;
    }
    if (!Registry().emplace(pool, std::move(entry)).second)
        throw std::bad_alloc();
    if (fresh) ++nextIdentity;
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
        RegisterEntry(pool, std::move(entry));
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
        if (found != Registry().end() && found->second.visible) {
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
    return found == Registry().end() ? kBoneKeyCapacity :
        found->second.visible ? found->second.capacity : 0;
}

std::uint64_t BoneKeyPoolIdentity(const BoneKey* pool) {
    if (pool == nullptr) return 0;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(pool);
    return found == Registry().end() || !found->second.visible ? 0 : found->second.identity;
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

struct BoneKeyPoolGrowth::Impl {
    enum class Phase { Prepared, Published, Done };
    ModelRecord* model = nullptr;
    BoneKey* source = nullptr;
    BoneKey* candidate = nullptr;
    std::size_t sourceCapacity = 0, candidateCapacity = 0;
    std::uint64_t sourceIdentity = 0, candidateIdentity = 0;
    bool grows = false, registered = false;
    Phase phase = Phase::Prepared;
    std::vector<unsigned char> beforeVisit;

    bool SourceCurrent(bool published) const noexcept {
        const auto found = Registry().find(source);
        if (!sourceIdentity)
            return found == Registry().end() && sourceCapacity == (source ? kBoneKeyCapacity : 0);
        return found != Registry().end() && found->second.capacity == sourceCapacity &&
            found->second.identity == sourceIdentity &&
            found->second.visible == !(published && grows) &&
            found->second.token == (published ? this : nullptr);
    }
    bool CandidateCurrent(bool published) const noexcept {
        if (!grows) return true;
        const auto found = Registry().find(candidate);
        return registered && found != Registry().end() && found->second.token == this &&
            found->second.identity == candidateIdentity && found->second.capacity == candidateCapacity &&
            found->second.visible == published;
    }
    unsigned char* SourceVisit() const noexcept {
        if (sourceCapacity <= kBoneKeyCapacity) return model->keyVisitMap;
        const auto found = Registry().find(source);
        return found != Registry().end() && found->second.visit.size() >= sourceCapacity
            ? found->second.visit.data() : nullptr;
    }
    bool Current() const noexcept { // RegistryMutex held.
        if (!model || phase == Phase::Done) return false;
        const bool published = phase == Phase::Published;
        if (model->boneKeys != (published ? candidate : source) ||
            !SourceCurrent(published) || !CandidateCurrent(published)) return false;
        if (published || beforeVisit.empty()) return true;
        const auto* visit = SourceVisit();
        return visit && !std::memcmp(visit, beforeVisit.data(), beforeVisit.size());
    }
    void RestoreVisit() noexcept {
        if (!beforeVisit.empty())
            std::memcpy(SourceVisit(), beforeVisit.data(), beforeVisit.size());
    }
    // Never overwrite a replacement model pointer. If a caller made one of our
    // allocations current directly, leave its ownership with that model.
    void Abandon() noexcept {
        if (phase == Phase::Done) return;
        BoneKey* releaseSource = nullptr;
        BoneKey* releaseCandidate = nullptr;
        try {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            const auto retainOrRelease = [&](BoneKey* pool, std::uint64_t identity, BoneKey*& release) {
                const auto found = Registry().find(pool);
                if (found == Registry().end() || found->second.token != this || found->second.identity != identity) return;
                if (model && model->boneKeys == pool) {
                    found->second.visible = true; found->second.token = nullptr;
                } else { Registry().erase(found); release = pool; }
            };
            if (grows && candidate) {
                if (registered) retainOrRelease(candidate, candidateIdentity, releaseCandidate);
                else releaseCandidate = candidate;
            }
            if (phase == Phase::Published && sourceIdentity) {
                if (grows) retainOrRelease(source, sourceIdentity, releaseSource);
                else {
                    const auto found = Registry().find(source);
                    if (found != Registry().end() && found->second.token == this)
                        found->second.token = nullptr;
                }
            }
            phase = Phase::Done;
        } catch (...) { return; }
        std::free(releaseCandidate); std::free(releaseSource);
    }
    bool Rollback() noexcept {
        if (phase == Phase::Done) return false;
        if (phase == Phase::Prepared) { Abandon(); return phase == Phase::Done; }
        BoneKey* release = nullptr;
        try {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            if (!Current()) return false;
            RestoreVisit();
            if (sourceIdentity) {
                auto& entry = Registry().find(source)->second;
                entry.visible = true; entry.token = nullptr;
            }
            model->boneKeys = source;
            if (grows) { Registry().erase(candidate); release = candidate; }
            phase = Phase::Done;
        } catch (...) { return false; }
        std::free(release); return true;
    }
    ~Impl() { if (phase != Phase::Done && !Rollback()) Abandon(); }
};

BoneKeyPoolGrowth::BoneKeyPoolGrowth(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
BoneKeyPoolGrowth::~BoneKeyPoolGrowth() = default;

bool BoneKeyPoolGrowth::Prepare(ModelRecord& model, std::uint64_t needed,
                               std::unique_ptr<BoneKeyPoolGrowth>& out) noexcept {
    if (out) return false;
    try {
        auto next = std::make_unique<Impl>();
        next->model = &model; next->source = model.boneKeys;
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            const auto found = Registry().find(next->source);
            if (found != Registry().end()) {
                if (!found->second.visible || found->second.token) return false;
                next->sourceCapacity = found->second.capacity;
                next->sourceIdentity = found->second.identity;
            } else next->sourceCapacity = next->source ? kBoneKeyCapacity : 0;
        }
        next->candidateCapacity = BoneKeyPlanCapacity(next->sourceCapacity, needed);
        if (needed && !next->candidateCapacity) return false;
        next->grows = next->candidateCapacity > next->sourceCapacity;
        next->candidate = next->grows ? nullptr : next->source;
        next->candidateIdentity = next->sourceIdentity;
        // Preserve the newly exposed inline range too, so rollback restores
        // every visit byte that publication can touch.
        const auto visitSize = next->candidateCapacity <= kBoneKeyCapacity
            ? next->candidateCapacity : next->sourceCapacity;
        {
            std::lock_guard<std::mutex> lock(RegistryMutex());
            if (!next->SourceCurrent(false)) return false;
            const auto* visit = next->SourceVisit();
            if (visitSize && !visit) return false;
            if (visitSize) next->beforeVisit.assign(visit, visit + visitSize);
        }
        if (next->grows) {
            next->candidate = static_cast<BoneKey*>(std::calloc(next->candidateCapacity, sizeof(BoneKey)));
            if (!next->candidate) return false;
            if (next->sourceCapacity)
                std::memcpy(next->candidate, next->source, next->sourceCapacity * sizeof(BoneKey));
            Entry entry; entry.capacity = next->candidateCapacity; entry.identity = next->sourceIdentity;
            entry.visible = false; entry.token = next.get();
            if (entry.capacity > kBoneKeyCapacity) {
                entry.visit.assign(entry.capacity, 0);
                if (next->sourceCapacity)
                    std::memcpy(entry.visit.data(), next->beforeVisit.data(), next->sourceCapacity);
            }
            {
                std::lock_guard<std::mutex> lock(RegistryMutex());
                RegisterEntry(next->candidate, std::move(entry));
                next->registered = true;
                next->candidateIdentity = Registry().find(next->candidate)->second.identity;
            }
        }
        auto token = std::unique_ptr<BoneKeyPoolGrowth>(new BoneKeyPoolGrowth(std::move(next)));
        if (!token->IsCurrent()) return false;
        out = std::move(token); return true;
    } catch (...) { return false; }
}

BoneKey* BoneKeyPoolGrowth::CandidatePool() const noexcept {
    return impl_->phase == Impl::Phase::Done ? nullptr : impl_->candidate;
}
std::size_t BoneKeyPoolGrowth::CandidateCapacity() const noexcept {
    return impl_->phase == Impl::Phase::Done ? 0 : impl_->candidateCapacity;
}
std::uint64_t BoneKeyPoolGrowth::CandidateIdentity() const noexcept {
    return impl_->phase == Impl::Phase::Done ? 0 : impl_->candidateIdentity;
}
bool BoneKeyPoolGrowth::Grows() const noexcept { return impl_->grows; }
bool BoneKeyPoolGrowth::IsCurrent() const noexcept {
    try { std::lock_guard<std::mutex> lock(RegistryMutex()); return impl_->Current(); }
    catch (...) { return false; }
}
bool BoneKeyPoolGrowth::Publish() noexcept {
    try {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto& p = *impl_;
        if (p.phase != Impl::Phase::Prepared || !p.Current()) return false;
        if (p.grows) {
            if (p.sourceCapacity) std::memcpy(p.candidate, p.source, p.sourceCapacity * sizeof(BoneKey));
            if (p.candidateCapacity <= kBoneKeyCapacity)
                std::memset(p.model->keyVisitMap + p.sourceCapacity, 0, p.candidateCapacity - p.sourceCapacity);
            Registry().find(p.candidate)->second.visible = true;
        }
        if (p.sourceIdentity) {
            auto& source = Registry().find(p.source)->second;
            source.visible = !p.grows; source.token = &p;
        }
        p.model->boneKeys = p.candidate; p.phase = Impl::Phase::Published;
        return true;
    } catch (...) { return false; }
}
bool BoneKeyPoolGrowth::Rollback() noexcept { return impl_->Rollback(); }
bool BoneKeyPoolGrowth::Accept() noexcept {
    BoneKey* release = nullptr;
    try {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto& p = *impl_;
        if (p.phase != Impl::Phase::Published || !p.Current()) return false;
        if (p.grows && p.sourceIdentity) { Registry().erase(p.source); release = p.source; }
        const auto candidate = Registry().find(p.candidate);
        if (candidate != Registry().end()) candidate->second.token = nullptr;
        p.phase = Impl::Phase::Done;
    } catch (...) { return false; }
    std::free(release); return true;
}

bool BoneKeyPoolEnsure(ModelRecord& model, std::uint64_t slotsNeeded) {
    // Preserve the allocation-free no-op used by native registration loops.
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        const auto found = Registry().find(model.boneKeys);
        if (found != Registry().end() && !found->second.visible) return false;
        const auto capacity = found == Registry().end()
            ? (model.boneKeys ? kBoneKeyCapacity : 0) : found->second.capacity;
        if (slotsNeeded <= capacity) return true;
    }
    std::unique_ptr<BoneKeyPoolGrowth> token;
    return BoneKeyPoolGrowth::Prepare(model, slotsNeeded, token) && token->Publish() && token->Accept();
}
BoneKeyVisit BoneKeyVisitMapOf(ModelRecord& model) {
    return BoneKeyVisitMapOf(model, BoneKeyCapacity(model.boneKeys));
}

BoneKeyVisit BoneKeyVisitMapOf(ModelRecord& model, std::size_t capacity) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(model.boneKeys);
    if (found != Registry().end() && !found->second.visible) return {nullptr, 0};
    if (capacity <= kBoneKeyCapacity) return {model.keyVisitMap, capacity};
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
    KeyPoolCommitGrowth(pool, grown);
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

#if !MMD_ORIGINAL_PROFILE
void MeasureBoneKeyStorage(const ModelRecord& model, plugins::resource_budget::Collector& out) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto found = Registry().find(model.boneKeys);
    if (found != Registry().end()) {
        out.Add(&*found, 1, sizeof(*found) + 4 * sizeof(void*));
        out.Vector(found->second.visit);
    }
    const auto morph = MorphRegistry().find(model.morphKeys);
    if (morph != MorphRegistry().end()) out.Add(&*morph, 1, sizeof(*morph) + 4 * sizeof(void*));
}
#endif
}  // namespace mikudancestudio::mdl
