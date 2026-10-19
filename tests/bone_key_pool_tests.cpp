#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/frame_limits.hpp"
// MAINTENANCE: regression-keep; runtime bone-key pool capacity
// (mikumikustudio/bone_key_pool.hpp).
//
// The pool starts at the original capacity and grows on demand; these cases pin
// the capacity bookkeeping itself, so they use a deliberately small pool and
// never touch the GPU, a window or a device.
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/global_key_layout.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, const char* name) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", name);
    }
}

using mikumikustudio::mdl::BoneKey;
using mikumikustudio::mdl::ModelRecord;

struct Fixture {
    std::unique_ptr<ModelRecord> model = std::make_unique<ModelRecord>();
    std::array<mikumikustudio::mdl::BoneRecord, 2> bones{};
    std::array<unsigned char, 2> physics{}, selection{};

    explicit Fixture(std::size_t capacity) {
        for (auto& bone : bones) bone.rotQuat[3] = 1.0f;
        model->boneTable = bones.data();
        model->boneCount = 2;
        model->bonePhysicsState = physics.data();
        model->boneSelection = selection.data();
        model->boneKeys = mikumikustudio::mdl::BoneKeyPoolCreate(capacity);
    }
    ~Fixture() {
        for (auto& ring : model->undoRings)
            for (auto& slot : ring.slots) {
                std::free(slot.bonePose);
                std::free(slot.auxiliaryPose);
            }
        mikumikustudio::mdl::BoneKeyPoolDestroy(model->boneKeys);
    }
    std::size_t Capacity() const {
        return mikumikustudio::mdl::BoneKeyCapacity(*model);
    }
};
void CapacityOfAHandBuiltPool() {
    // A pool the registry never saw keeps reporting the original capacity, which
    // is what keeps hand-built fixtures and any not-yet-migrated caller correct.
    std::vector<BoneKey> pool(8);
    Check(mikumikustudio::mdl::BoneKeyCapacity(pool.data()) ==
              mikumikustudio::mdl::kBoneKeyCapacity,
          "an unregistered pool reports the original capacity");
    Check(mikumikustudio::mdl::BoneKeyCapacity(nullptr) == 0,
          "a null pool reports no capacity");
}

void CreateEnsureAndDestroy() {
    Fixture fixture(64);
    Check(fixture.Capacity() == 64, "a created pool reports the capacity it was made with");
    Check(mikumikustudio::mdl::BoneKeyVisitMapSize(*fixture.model) == 64,
          "the visit map covers the pool capacity");
    Check(mikumikustudio::mdl::BoneKeyVisitMapOf(*fixture.model).bytes ==
              fixture.model->keyVisitMap,
          "a pool below the original capacity uses the inline visit map");

    // Contents survive a growth byte for byte, and the growth is one-way.
    BoneKey* before = fixture.model->boneKeys;
    fixture.model->boneKeys[5].frame = 33;
    fixture.model->boneKeys[5].rotation[3] = 1.0f;
    fixture.model->boneKeys[5].allocated = 1;
    const std::size_t grown = mikumikustudio::mdl::BoneKeyPlanCapacity(
        fixture.Capacity(), 100);
    Check(mikumikustudio::mdl::BoneKeyPoolEnsure(*fixture.model, 100),
          "a request past the end grows the pool");
    Check(fixture.model->boneKeys != before, "growth publishes a new block");
    Check(fixture.Capacity() >= 100 && fixture.Capacity() == grown,
          "the published capacity is the planned one");
    Check(fixture.model->boneKeys[5].frame == 33 &&
              fixture.model->boneKeys[5].allocated == 1,
          "growth copies the existing records");
    Check(fixture.model->boneKeys[99].frame == 0 &&
              fixture.model->boneKeys[99].allocated == 0,
          "growth zero fills the new records");
    // The growth stayed inside the original capacity (64 -> 131072 slots), so
    // the inline visit map still covers every slot and no side buffer was
    // needed.  The crossing case is exercised by VisitClearFollowsTheCapacity
    // and GrowthPreservesVisitMarks below.
    const auto grownVisit = mikumikustudio::mdl::BoneKeyVisitMapOf(*fixture.model);
    Check(fixture.Capacity() <= mikumikustudio::mdl::kBoneKeyCapacity,
          "this growth stays inside the original capacity");
    Check(grownVisit.bytes == fixture.model->keyVisitMap,
          "a pool that stayed inside the original capacity keeps the inline visit map");
    Check(grownVisit.size == fixture.Capacity(),
          "the visit map length matches the capacity it reports");
    Check(mikumikustudio::mdl::BoneKeyVisitMapSize(*fixture.model) ==
              fixture.Capacity(),
          "the visit map tracks the grown capacity");
    // The visit map must be writable up to the last slot of the new capacity.
    grownVisit.bytes[fixture.Capacity() - 1] = 1;
    Check(grownVisit.bytes[fixture.Capacity() - 1] == 1,
          "the visit map covers the last slot");

    Check(mikumikustudio::mdl::BoneKeyPoolEnsure(*fixture.model, 100),
          "an in-range request is a no-op");
    Check(fixture.Capacity() == grown, "a no-op request does not grow again");

    // Destroying the pool drops its record, so the address reports the original
    // capacity again instead of matching a stale entry.
    BoneKey* released = fixture.model->boneKeys;
    mikumikustudio::mdl::BoneKeyPoolDestroy(fixture.model->boneKeys);
    Check(fixture.model->boneKeys == nullptr, "destroy clears the caller's pointer");
    Check(mikumikustudio::mdl::BoneKeyCapacity(released) ==
              mikumikustudio::mdl::kBoneKeyCapacity,
          "a released pool no longer matches the registry");
}

void PlanningAndTheStructuralLimit() {
    using mikumikustudio::mdl::BoneKeyPlanCapacity;
    Check(BoneKeyPlanCapacity(64, 64) == 64, "a fitted request keeps the capacity");
    const std::size_t planned = BoneKeyPlanCapacity(64, 65);
    Check(planned > 65, "a request past the end leaves growth headroom");
    Check(planned % mikumikustudio::mdl::kBoneKeyGrowthQuantum == 0,
          "the planned capacity is a multiple of the growth quantum");
    Check(BoneKeyPlanCapacity(64, UINT64_MAX) == 0,
          "a request beyond the structural limit is refused");
    Check(BoneKeyPlanCapacity(mikumikustudio::mdl::kBoneKeyCapacityLimit,
                              mikumikustudio::mdl::kBoneKeyCapacityLimit) ==
              mikumikustudio::mdl::kBoneKeyCapacityLimit,
          "the structural limit itself is representable");
    // The limit is the smaller of the 32-bit index range and the size_t byte
    // range, so growing to it can never wrap an index or a byte count.
    Check(mikumikustudio::mdl::kBoneKeyCapacityLimit <=
              static_cast<std::size_t>(INT32_MAX),
          "the ceiling keeps every slot index inside int32");
    Check(mikumikustudio::mdl::kBoneKeyCapacityLimit <=
              SIZE_MAX / (sizeof(BoneKey) + 1),
          "the ceiling keeps the pool and its visit map inside size_t");
}

void RefusedGrowthLeavesTheModelAlone() {
    Fixture fixture(64);
    BoneKey* before = fixture.model->boneKeys;
    fixture.model->boneKeys[3].frame = 9;
    fixture.model->maxFrame = 9;
    Check(!mikumikustudio::mdl::BoneKeyPoolEnsure(
              *fixture.model,
              static_cast<std::uint64_t>(
                  mikumikustudio::mdl::kBoneKeyCapacityLimit) + 1),
          "a request beyond the ceiling is refused");
    Check(fixture.model->boneKeys == before &&
              fixture.Capacity() == 64 &&
              fixture.model->boneKeys[3].frame == 9,
          "a refused growth leaves the pointer, capacity and contents untouched");
}

void VisitClearFollowsTheCapacity() {
    Fixture fixture(64);
    mikumikustudio::mdl::BoneKeyVisitClear(*fixture.model);
    fixture.model->keyVisitMap[10] = 1;
    mikumikustudio::mdl::BoneKeyVisitClear(*fixture.model);
    Check(fixture.model->keyVisitMap[10] == 0,
          "clearing an original-size pool clears the inline map");
    // Only a capacity above the original one needs the side buffer, so the
    // growth below asks for more slots than the original pool had.
    Check(mikumikustudio::mdl::BoneKeyPoolEnsure(
              *fixture.model,
              static_cast<std::uint64_t>(
                  mikumikustudio::mdl::kBoneKeyCapacity) + 1),
          "grow the pool past the original capacity");
    Check(fixture.Capacity() > mikumikustudio::mdl::kBoneKeyCapacity,
          "the grown capacity really crosses the original one");
    const auto visited = mikumikustudio::mdl::BoneKeyVisitMapOf(*fixture.model);
    Check(visited.bytes != fixture.model->keyVisitMap,
          "a pool grown past the original capacity uses the side visit map");
    Check(visited.size == fixture.Capacity(),
          "the side visit map covers the whole capacity");
    visited.bytes[10] = 1;
    visited.bytes[150] = 1;
    mikumikustudio::mdl::BoneKeyVisitClear(*fixture.model);
    Check(visited.bytes[10] == 0 && visited.bytes[150] == 0,
          "clearing a grown pool clears the side map, not just the inline array");
}

// A growth in the middle of a batch transaction must not lose the marks that
// dedup the undo records already collected for this pool.
void GrowthPreservesVisitMarks() {
    Fixture fixture(64);
    const auto before = mikumikustudio::mdl::BoneKeyVisitMapOf(*fixture.model);
    before.bytes[3] = 1;
    before.bytes[63] = 1;
    const std::size_t firstTarget = mikumikustudio::mdl::BoneKeyPlanCapacity(
        fixture.Capacity(),
        static_cast<std::uint64_t>(mikumikustudio::mdl::kBoneKeyCapacity) + 1);
    Check(mikumikustudio::mdl::BoneKeyPoolEnsure(
              *fixture.model,
              static_cast<std::uint64_t>(
                  mikumikustudio::mdl::kBoneKeyCapacity) + 1),
          "grow the pool past the original capacity");
    Check(fixture.Capacity() > mikumikustudio::mdl::kBoneKeyCapacity,
          "the grown capacity really crosses the original one");
    const auto grown = mikumikustudio::mdl::BoneKeyVisitMapOf(*fixture.model);
    Check(grown.bytes != fixture.model->keyVisitMap,
          "the grown pool reads through the side visit map");
    Check(grown.size == fixture.Capacity() && grown.size == firstTarget,
          "the side visit map covers the planned capacity");
    Check(grown.bytes[3] == 1 && grown.bytes[63] == 1,
          "growth carries the marks of the existing slots over");
    Check(grown.bytes[64] == 0 && grown.bytes[firstTarget - 1] == 0,
          "the slots the growth adds start unvisited");

    grown.bytes[150] = 1;
    Check(mikumikustudio::mdl::BoneKeyPoolEnsure(*fixture.model, grown.size + 1),
          "grow the pool a second time");
    const auto again = mikumikustudio::mdl::BoneKeyVisitMapOf(*fixture.model);
    Check(again.size > grown.size, "the second growth publishes a larger capacity");
    Check(again.bytes[3] == 1 && again.bytes[150] == 1,
          "a second growth carries the side marks over");
    Check(again.bytes[grown.size] == 0,
          "the region the second growth adds starts unvisited");
}

// Existing fixtures hand the model a std::vector<BoneKey> pool ("borrowed").
// Nothing in this module may release memory it did not allocate, and the inline
// visit map keeps working for such a pool.
void BorrowedPoolIsNeverFreed() {
    const std::size_t initial = mikumikustudio::mdl::kBoneKeyCapacity;
    std::vector<BoneKey> pool(initial);
    pool[7].frame = 42;
    pool[7].rotation[3] = 1.0f;
    auto model = std::make_unique<ModelRecord>();
    model->boneKeys = pool.data();
    Check(mikumikustudio::mdl::BoneKeyCapacity(*model) == initial,
          "a borrowed pool reports the original capacity");
    model->keyVisitMap[9] = 1;
    mikumikustudio::mdl::BoneKeyVisitClear(*model);
    Check(model->keyVisitMap[9] == 0, "clearing a borrowed pool uses the inline map");
    Check(mikumikustudio::mdl::BoneKeyVisitMapOf(*model).size == initial,
          "a borrowed pool's visit map length is its capacity");

    // Destroying a pool this module did not allocate only clears the handle.
    BoneKey* borrowed = model->boneKeys;
    mikumikustudio::mdl::BoneKeyPoolDestroy(borrowed);
    Check(borrowed == nullptr, "destroy clears the caller's pointer");
    Check(pool[7].frame == 42 && pool[7].rotation[3] == 1.0f,
          "a borrowed pool's storage is still valid after destroy");

    // Growth adopts a borrowed pool: it copies the records and leaves the
    // caller's storage alone, which is what the vector-based fixtures rely on.
    model->boneKeys = pool.data();
    Check(mikumikustudio::mdl::BoneKeyPoolEnsure(
              *model, static_cast<std::uint64_t>(initial) + 1),
          "a borrowed pool can be adopted by growth");
    Check(model->boneKeys != pool.data(), "adoption publishes a new block");
    Check(pool[7].frame == 42, "adoption leaves the caller's storage untouched");
    Check(model->boneKeys[7].frame == 42, "adoption copies the records");
    Check(mikumikustudio::mdl::BoneKeyCapacity(*model) > initial,
          "the adopted pool reports the grown capacity");
    mikumikustudio::mdl::BoneKeyPoolDestroy(model->boneKeys);
}


void MorphPoolGrowth() {
    namespace mdl = mikumikustudio::mdl;
    auto model = std::make_unique<ModelRecord>();
    model->morphKeys = mdl::MorphKeyPoolCreate(mdl::kMorphKeyCapacity);
    model->morphKeys[3].frame = 71;
    model->morphKeys[3].next = 19999;
    model->morphKeys[19999].previous = 3;
    model->morphKeys[19999].value = .75f;
    const auto* before = model->morphKeys;
    Check(mdl::MorphKeyPoolEnsure(*model, 90001), "morph pool grows beyond 20000");
    Check(model->morphKeys != before && mdl::MorphKeyCapacity(*model) >= 90001,
          "morph growth publishes its runtime capacity");
    Check(model->morphKeys[3].frame == 71 && model->morphKeys[3].next == 19999 &&
              model->morphKeys[19999].previous == 3 && model->morphKeys[19999].value == .75f,
          "growth preserves morph records and chain indices");
    Check(model->morphKeys[90000].frame == 0 && model->morphKeys[90000].next == 0,
          "new morph slots are empty");
    before = model->morphKeys;
    Check(!mdl::MorphKeyPoolEnsure(*model, std::uint64_t(mdl::kMorphKeyCapacityLimit) + 1),
          "structural overflow is refused");
    Check(model->morphKeys == before && model->morphKeys[3].frame == 71,
          "failed morph growth leaves the model untouched");
    mdl::MorphKeyPoolDestroy(model->morphKeys);
    Check(model->morphKeys == nullptr, "morph destruction clears the handle");
    std::vector<mdl::MorphKey> borrowed(mdl::kMorphKeyCapacity);
    borrowed[7].frame = 42;
    model->morphKeys = borrowed.data();
    Check(mdl::MorphKeyPoolEnsure(*model, mdl::kMorphKeyCapacity + 1),
          "morph growth can adopt a borrowed pool");
    Check(borrowed[7].frame == 42 && model->morphKeys[7].frame == 42,
          "adoption preserves both borrowed and adopted storage");
    mdl::MorphKeyPoolDestroy(model->morphKeys);
}

}  // namespace

void GlobalAndDisplayGrowth() {
    namespace mdl = mikumikustudio::mdl;
    auto* camera = mdl::KeyPoolCreate<mdl::CameraKey>();
    for (unsigned i = 1; i < 10000; ++i) {
        camera[i].frame = i;
        camera[i].previous = i - 1;
        camera[i].next = i + 1 < 10000 ? i + 1 : 0;
    }
    camera[0].next = 1;
    camera[9999].distance = -71;
    std::uint32_t slot = 0;
    Check(mdl::KeyPoolFreeSlot(camera, slot) && slot == 10000,
          "global pool accepts the first key beyond 10000 slots");
    Check(camera[9999].distance == -71 && camera[9999].previous == 9998 &&
          camera[0].next == 1 && camera[slot].frame == 0,
          "global growth preserves values and absolute chain indices");
    auto* before = camera;
    Check(!mdl::KeyPoolEnsure(camera, std::uint64_t(mdl::KeyPoolLimit<mdl::CameraKey>()) + 1) &&
          camera == before && camera[9999].distance == -71,
          "impossible growth leaves the allocation and keys unchanged");
    mdl::KeyPoolDestroy(camera);

    auto model = std::make_unique<mdl::ModelRecord>();
    model->ikChainCount = 3;
    model->boneOrderCount = 2;
    Check(mdl::DisplayKeyPoolEnsure(*model, 1000), "display pool initializes owned payloads");
    const auto initial = mdl::KeyPoolCapacity(model->displayKeys);
    for (std::size_t i = 1; i < initial; ++i) model->displayKeys[i].frame = static_cast<unsigned>(i);
    auto* originalIk = model->displayKeys[1].ikStates;
    originalIk[2] = 0;
    model->displayKeys[1].selectorStates[1].modelIndex = 17;
    Check(mdl::DisplayKeyFreeSlot(*model, slot) && slot == initial,
          "display pool grows after all original slots are occupied");
    Check(model->displayKeys[1].ikStates == originalIk && originalIk[2] == 0 &&
          model->displayKeys[1].selectorStates[1].modelIndex == 17,
          "display growth preserves ownership and existing IK parent values");
    Check(model->displayKeys[slot].ikStates[2] == 1 &&
          model->displayKeys[slot].selectorStates[1].modelIndex == -1,
          "new display slots have usable initialized payload arrays");
    auto* displayBefore = model->displayKeys;
    Check(!mdl::DisplayKeyPoolEnsure(*model, std::uint64_t(mdl::KeyPoolLimit<mdl::DisplayKey>()) + 1) &&
          model->displayKeys == displayBefore && model->displayKeys[1].ikStates == originalIk,
          "failed display growth preserves all prior state");
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(model->displayKeys); i < capacity; ++i) {
        std::free(model->displayKeys[i].ikStates);
        std::free(model->displayKeys[i].selectorStates);
    }
    mdl::KeyPoolDestroy(model->displayKeys);
}

void ExpandedDisplayAndFrames() {
    namespace mdl=mikumikustudio::mdl;
    auto model=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::DisplayGroup> groups(700);model->groupNames=groups.data();
    mdl::SetDisplayCapacity(*model,700,900);
    Check(model->groupCount==255&&model->facialFrameCount==255,"plugin count mirrors saturate instead of wrapping");
    Check(mdl::DisplayGroupCount(*model)==700&&mdl::FacialFrameCount(*model)==900,"display counts retain their complete values");
    mdl::SetDisplayRowType(*model,12,699);
    Check(mdl::DisplayRowType(*model,12)==699,"visible row selects a group above byte range");
    std::array<mdl::FrameGroup,1> rows{};model->rbGroups=rows.data();model->rigidBodyCount=1;
    mdl::SetFrameIndices(rows[0],70000,90000);
    Check(mdl::FrameGroupIndex(rows[0])==70000&&mdl::FrameTargetIndex(rows[0])==90000,"display row indices retain values beyond 16 bits");
    mdl::ForgetDisplayCapacity(*model);
    mdl::SetFrameIndices(rows[0],1,2);
    Check(mdl::FrameGroupIndex(rows[0])==1&&mdl::FrameTargetIndex(rows[0])==2,"reused storage does not inherit wide indices");
    std::uint32_t result=0;
    Check(mikumikustudio::frame_limits::Add(10000000,37,result)&&result==10000037,"frame arithmetic extends beyond the former cap");
    result=77;
    Check(!mikumikustudio::frame_limits::Add(mikumikustudio::frame_limits::maximum,1,result)&&result==77,"structural frame overflow preserves the output");
}

int main() {
    ExpandedDisplayAndFrames();
    GlobalAndDisplayGrowth();
    MorphPoolGrowth();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CapacityOfAHandBuiltPool();
    CreateEnsureAndDestroy();
    PlanningAndTheStructuralLimit();
    RefusedGrowthLeavesTheModelAlone();
    VisitClearFollowsTheCapacity();
    GrowthPreservesVisitMarks();
    BorrowedPoolIsNeverFreed();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
