// Runtime capacity of the per-model bone-key pool.
//
// The pool is still allocated at the original size (kBoneKeyCapacity: 600000
// slots on x64, 300000 on x86) and only grows when a caller actually needs more
// slots.  A model's capacity is therefore a property of the pool, not a
// compile-time constant, and it is recorded in a side registry keyed by the pool
// pointer.
//
// A side registry is used instead of a ModelRecord field because the record's
// field offsets are a cross-module ABI - MME reads the model by fixed offsets -
// so no field may be appended.  Keying by the pool pointer has two further
// effects: the ModelRecord copies that bake builds share the pool pointer and
// resolve to the same capacity, and any pool the registry does not know about
// falls back to kBoneKeyCapacity, which keeps hand-built fixtures working.
//
// Lookups take a mutex and search a map, so they must never run inside a
// per-record loop.  Hoist the value once per operation, as in
// `const std::size_t capacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));`.
//
// Growth is one-way: a pool never shrinks.  Slot indices are stored in the key
// records themselves (BoneKey::previous / BoneKey::next are absolute slot
// numbers), so a stable index space keeps every chain, undo snapshot and scale
// side table valid across a growth.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "mikumikustudio/model.hpp"

namespace mikumikustudio::mdl {

// Slots added by one interactive growth step (a single key registration).
constexpr std::size_t kBoneKeyGrowthQuantum = 65536;

// Structural ceiling, not a policy number.  A slot index is stored in a signed
// 32-bit field and compared as `int` throughout the tree, so the pool may not
// exceed INT32_MAX slots; a size_t must also be able to hold the pool bytes and
// the one-byte-per-slot visit map.  Whichever bound is smaller wins.  Running
// out of memory below the ceiling is reported as an allocation failure by
// BoneKeyPoolEnsure, which leaves the model untouched.
constexpr std::size_t kBoneKeyCapacityIndexLimit =
    static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
constexpr std::size_t kBoneKeyCapacityByteLimit =
    ((std::numeric_limits<std::size_t>::max)() - 1) / (sizeof(BoneKey) + 1);
constexpr std::size_t kBoneKeyCapacityLimit =
    kBoneKeyCapacityIndexLimit < kBoneKeyCapacityByteLimit
        ? kBoneKeyCapacityIndexLimit
        : kBoneKeyCapacityByteLimit;
static_assert(kBoneKeyCapacityLimit >= kBoneKeyCapacity,
              "the structural ceiling must admit the original pool size");

// Allocates a zero-filled pool and records its capacity.  Throws
// std::bad_alloc when the pool or its registry entry cannot be allocated, which
// is the contract of the operator new call this replaces in the loaders.
// Returns nullptr only for a zero capacity.  Release with BoneKeyPoolDestroy.
BoneKey* BoneKeyPoolCreate(std::size_t capacity);

// Unregisters and releases a pool created by BoneKeyPoolCreate, then clears the
// caller's pointer.  A pool the registry does not own was not allocated here and
// is not freed; the pointer is only cleared.  Erasing the registry entry before
// the free keeps a recycled address from matching a stale entry.
void BoneKeyPoolDestroy(BoneKey*& pool);

// Capacity of a pool.  A null pool reports 0; a pool the registry does not know
// reports kBoneKeyCapacity, the original allocation size.
std::size_t BoneKeyCapacity(const BoneKey* pool);
std::size_t BoneKeyCapacity(const ModelRecord& model);

// Capacity to grow to so that `slotsNeeded` slots fit, or 0 when the request
// exceeds kBoneKeyCapacityLimit or the arithmetic would overflow.  Grows
// geometrically so that repeated interactive insertions stay amortised O(1),
// rounded up to kBoneKeyGrowthQuantum.
std::size_t BoneKeyPlanCapacity(std::size_t currentCapacity,
                                std::uint64_t slotsNeeded);
std::size_t BoneKeyPlanCapacity(const ModelRecord& model,
                                std::uint64_t slotsNeeded);

// Transactional growth.  Returns true when the pool already holds `slotsNeeded`
// slots.  Otherwise allocates the new block, copies the old contents, prepares
// the visit map (carrying the visited marks of every existing slot over, so a
// growth in the middle of a batch registration or an undo transaction does not
// lose them), commits, and only then releases the old block.  Any failure leaves
// model.boneKeys, its contents, the undo ring and the scale side table exactly
// as they were, and returns false.
bool BoneKeyPoolEnsure(ModelRecord& model, std::uint64_t slotsNeeded);

// Visit map for the model's current capacity, one byte per slot, returned as a
// pointer together with the length that pointer really has.  The two must never
// be taken from different sources: while the capacity is the original one the
// map is the inline ModelRecord array, and a grown pool uses the side buffer the
// growth prepared.  A caller that asked for a capacity the registry cannot cover
// (a capacity that did not come from this pool) gets the inline array with its
// own safe length instead of a short buffer under a larger capacity.
struct BoneKeyVisit {
    unsigned char* bytes;
    std::size_t size;
};

// The two-argument form takes a capacity the caller already resolved, which is
// how a per-record loop avoids a registry lookup per iteration: hoist
// `const std::size_t capacity = mdl::BoneKeyCapacity(model);` once, then call
// this form inside the loop.  The caller must refresh both values after any
// BoneKeyPoolEnsure.
BoneKeyVisit BoneKeyVisitMapOf(ModelRecord& model);
BoneKeyVisit BoneKeyVisitMapOf(ModelRecord& model, std::size_t capacity);
std::size_t BoneKeyVisitMapSize(const ModelRecord& model);
// Clears the whole visit map.  Use this instead of a memset with sizeof(...),
// which is bound to the inline array and is too short after a growth.
void BoneKeyVisitClear(ModelRecord& model);

}  // namespace mikumikustudio::mdl
