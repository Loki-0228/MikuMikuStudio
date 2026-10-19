// Retained unbound command regression: one paste/undo covers every key family.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <new>

namespace { thread_local long allocationLimit = -1; }
void* operator new(std::size_t size) {
    if (allocationLimit == 0) throw std::bad_alloc();
    if (allocationLimit > 0) --allocationLimit;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace mikumikustudio {
void CmdControl400(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}
namespace {
using namespace mikumikustudio;
unsigned checks = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
template<class T> bool Same(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
struct Fixture {
    static constexpr unsigned capacity = 16;
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 3> bones{};
    std::array<unsigned char, 3> selection{}, physics{};
    std::array<mdl::MorphRecord, 2> morphs{};
    std::array<mdl::IkChain, 1> ik{};
    std::array<mdl::BoneOrderEntry, 2> order{};
    std::array<std::array<unsigned char, 1>, capacity> ikStates{};
    std::array<std::array<mdl::BoneReference, 2>, capacity> parents{};
    Fixture() {
        auto& m = *model;
        m.boneCount = bones.size(); m.boneTable = bones.data();
        m.boneSelection = selection.data(); m.bonePhysicsState = physics.data();
        m.morphCount = morphs.size(); m.morphs = morphs.data();
        m.boneKeys = mdl::BoneKeyPoolCreate(capacity);
        m.morphKeys = mdl::MorphKeyPoolCreate(capacity);
        m.displayKeys = mdl::KeyPoolCreate<mdl::DisplayKey>(capacity);
        m.ikChains = ik.data(); m.ikChainCount = ik.size(); ik[0].boneIndex = 2;
        m.boneOrderTable = order.data(); m.boneOrderCount = order.size();
        order[0].boneIndex = -1; order[1].boneIndex = 1;
        m.physicsMode = 2; m.facialFrameCount = 0;
        std::strcpy(bones[0].name, "sentinel");
        std::strcpy(bones[1].name, "bone"); std::strcpy(bones[2].name, "ik");
        std::strcpy(morphs[0].name, "face"); std::strcpy(morphs[1].name, "untouched");
        for (unsigned i = 0; i < bones.size(); ++i) {
            bones[i].rotQuat[3] = m.boneKeys[i].rotation[3] = 1;
            bones[i].slotIndex = -1; m.boneKeys[i].position[0] = float(i + 2);
        }
        m.boneKeys[1].next = 3;
        m.boneKeys[3].frame = 5; m.boneKeys[3].previous = 1; m.boneKeys[3].next = 4;
        m.boneKeys[4].frame = 20; m.boneKeys[4].previous = 3;
        for (int i : {3, 4}) { m.boneKeys[i].rotation[3] = 1; m.boneKeys[i].position[0] = float(i); }
        m.morphKeys[0].value = .2f; m.morphKeys[0].next = 2;
        m.morphKeys[2].frame = 5; m.morphKeys[2].value = .3f; m.morphKeys[2].next = 3;
        m.morphKeys[3].frame = 20; m.morphKeys[3].previous = 2; m.morphKeys[3].value = .4f;
        m.displayKeys[0].next = 1;
        m.displayKeys[1].frame = 5; m.displayKeys[1].next = 2;
        m.displayKeys[2].frame = 20; m.displayKeys[2].previous = 1;
        for (unsigned i = 0; i < capacity; ++i) {
            ikStates[i][0] = 1; parents[i][0] = {-1, 3}; parents[i][1] = {-1, 4};
            m.displayKeys[i].visible = 1;
            m.displayKeys[i].ikStates = ikStates[i].data();
            m.displayKeys[i].selectorStates = parents[i].data();
        }
        m.maxFrame = 20;
    }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    ~Fixture() {
#if !MMD_ORIGINAL_PROFILE
        bone_curve::Forget(Raw());
#endif
        bone_scale::Forget(Raw());
        for (auto& ring : model->undoRings) for (auto& e : ring.slots) {
            undo_storage::Delete(e.bonePose); undo_storage::Delete(e.auxiliaryPose);
        }
        mdl::BoneKeyPoolDestroy(model->boneKeys); mdl::MorphKeyPoolDestroy(model->morphKeys);
        mdl::KeyPoolDestroy(model->displayKeys);
    }
};
struct Image {
    std::array<mdl::BoneKey, Fixture::capacity> bones;
    std::array<mdl::MorphKey, Fixture::capacity> morphs;
    std::array<mdl::DisplayKey, Fixture::capacity> display;
    decltype(Fixture::ikStates) ik;
    decltype(Fixture::parents) parents;
    explicit Image(const Fixture& f) : ik(f.ikStates), parents(f.parents) {
        std::memcpy(bones.data(), f.model->boneKeys, sizeof bones);
        std::memcpy(morphs.data(), f.model->morphKeys, sizeof morphs);
        std::memcpy(display.data(), f.model->displayKeys, sizeof display);
    }
    void Verify(const Fixture& f, const char* label) const {
        Check(!std::memcmp(bones.data(), f.model->boneKeys, sizeof bones), label);
        Check(!std::memcmp(morphs.data(), f.model->morphKeys, sizeof morphs), label);
        Check(!std::memcmp(display.data(), f.model->displayKeys, sizeof display), label);
        Check(Same(ik, f.ikStates), "undo/redo restores owned IK bytes");
        Check(Same(parents, f.parents), "undo/redo restores owned parent pairs");
    }
};
template<class K> unsigned At(const K* keys, unsigned root, unsigned frame) {
    unsigned current = root;
    for (unsigned n = 0; n < Fixture::capacity; ++n) {
        if (keys[current].frame == frame) return current;
        if (!keys[current].next) break;
        current = keys[current].next;
        Check(current < Fixture::capacity, "paste links stay within pool");
    }
    Check(false, "find pasted key"); return 0;
}
void Scenario(unsigned mask, int frame) {
    Fixture f;
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>(); std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get(); app->ModelSlot(0) = f.Raw();
    app->SelectedModelSlot() = 0; app->CurrentFrame() = frame; app->PlaybackPhysicsMode() = 2;
    mdl::BoneClipboardRecord bone{}; std::strcpy(bone.name, "bone");
    bone.rotation[3] = 1; bone.position[0] = 14;
    for (int lane = 0; lane < 4; ++lane) {
        bone.interpolation[lane] = bone.interpolation[lane + 4] = 20;
        bone.interpolation[lane + 8] = bone.interpolation[lane + 12] = 107;
    }
    mdl::MorphClipboardRecord morph{}; std::strcpy(morph.name, "face"); morph.value = .75f;
    mdl::IkClipboardState ik{}; std::strcpy(ik.boneName, "ik"); ik.enabled = 0;
    std::array<mdl::SelectorClipboardState, 2> parents{};
    parents[0].modelIndex = -1; parents[0].boneIndex = 8;
    std::strcpy(parents[1].boneName, "bone"); parents[1].modelIndex = -1; parents[1].boneIndex = 9;
    mdl::DisplayClipboardRecord display{}; display.visible = 0;
    display.ikCount = 1; display.ikStates = &ik; display.selectorCount = 2; display.selectorStates = parents.data();
    app->ClipboardCounts().bones = mask & 1 ? 1 : 0;
    app->ClipboardCounts().morphs = mask & 2 ? 1 : 0;
    app->ClipboardCounts().displays = mask & 4 ? 1 : 0;
    app->BoneClipboard() = &bone; app->MorphClipboard() = &morph; app->DisplayClipboard() = &display;
    auto* previous = g_Block; g_Block = app.get();
    const Image before(f);
    CmdControl400(app.get(), nullptr, 421, 0);
    Check(f.model->undoState[0] == 1, "each nonempty family combination advances history once");
    Check(f.model->undoRings[0].slots[1].operation == (mask & 6 ? 7 : 2), "paste uses reversible family payload");
    Check(Same(before.bones[0], f.model->boneKeys[0]), "bone zero sentinel is untouched");
    if (mask & 1) Check(f.model->boneKeys[At(f.model->boneKeys, 1, frame)].position[0] == 14, "bone paste applied");
    if (mask & 2) Check(f.model->morphKeys[At(f.model->morphKeys, 0, frame)].value == .75f, "morph paste applied");
    if (mask & 4) {
        const auto index = At(f.model->displayKeys, 0, frame);
        Check(f.model->displayKeys[index].visible == 0 && f.ikStates[index][0] == 0, "display paste applies visibility and IK");
        Check(f.parents[index][0].boneIndex == 8 && f.parents[index][1].boneIndex == 9, "display paste applies root and named parent");
    }
    const Image after(f);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        CmdControl400(app.get(), nullptr, 400, 0); before.Verify(f, "undo restores all key pools");
        CmdControl400(app.get(), nullptr, 401, 0); after.Verify(f, "redo restores all key pools");
    }
    if (!(mask & 4)) {
        CmdControl400(app.get(), nullptr, 400, 0);
        const auto undoState = f.model->undoState[0];
        const auto limit = f.model->undoState[1];
        const auto redo = f.model->undoRings[1].slots[(undoState + 1) % 30];
        const auto selected = f.selection;
        app->SceneModified() = 0;
        const auto bones = app->ClipboardCounts().bones, morphs = app->ClipboardCounts().morphs;
        app->ClipboardCounts().bones = app->ClipboardCounts().morphs = 0;
        CmdControl400(app.get(), nullptr, 421, 0);
        Check(f.model->undoState[0] == undoState && f.model->undoState[1] == limit && !app->SceneModified(),
              "empty clipboard leaves history and dirty state unchanged");
        app->ClipboardCounts().bones = bones; app->ClipboardCounts().morphs = morphs;
        std::strcpy(bone.name, "foreign bone"); std::strcpy(morph.name, "foreign morph");
        CmdControl400(app.get(), nullptr, 421, 0);
        Check(f.model->undoState[0] == undoState && f.model->undoState[1] == limit && !app->SceneModified(),
              "unmatched clipboard leaves history and dirty state unchanged");
        Check(Same(redo, f.model->undoRings[1].slots[(undoState + 1) % 30]) && Same(selected, f.selection),
              "unmatched clipboard preserves redo payload and selection");
        before.Verify(f, "unmatched clipboard preserves all key pools");
        CmdControl400(app.get(), nullptr, 401, 0); after.Verify(f, "redo still works after unmatched paste");
    }
    app->BoneClipboard() = nullptr; app->MorphClipboard() = nullptr; app->DisplayClipboard() = nullptr;
    g_Block = previous;
}
void HistoryPreparationFailures() {
    unsigned failures = 0;
    // MSVC allocates a sentinel for each empty map in all 30 undo/redo slots.
    constexpr long maxBudget = 1024;
    for (long budget = 0; budget < maxBudget; ++budget) {
        Fixture f;
        auto& slot = f.model->undoRings[0].slots[1];
        slot.operation = 1; slot.frame = 37; slot.dirty = 1;
        slot.bonePose = static_cast<mdl::BonePoseSnapshot*>(undo_storage::Allocate(sizeof(mdl::BonePoseSnapshot)));
        slot.auxiliaryPose = undo_storage::Allocate(11);
        std::memset(slot.bonePose, 0x35, sizeof(mdl::BonePoseSnapshot));
        std::memset(slot.auxiliaryPose, 0x71, 11);
        const auto original = slot;
        const auto pose = *slot.bonePose;
        std::array<unsigned char, 11> aux{}; std::memcpy(aux.data(), slot.auxiliaryPose, aux.size());
        const Image before(f);
        Check(!BeginModelFramePasteUndo(f.Raw(), 10, UINT32_MAX, true, true), "unrepresentable history size is refused");
        allocationLimit = budget;
        const bool prepared = BeginModelFramePasteUndo(f.Raw(), 10, 1, true, true);
        allocationLimit = -1;
        if (prepared) {
            std::printf("model paste history preparation: %u rejected allocation budgets, success at %ld\n", failures, budget);
            Check(f.model->undoState[0] == 1 && slot.operation == 7, "history preparation remains usable after allocation retries");
            break;
        }
        ++failures;
        Check(f.model->undoState[0] == 0 && f.model->undoState[1] == 0 && Same(slot, original),
              "failed before-image preparation preserves cursor and recycled slot");
        Check(Same(pose, *slot.bonePose) && !std::memcmp(aux.data(), slot.auxiliaryPose, aux.size()),
              "failed preparation preserves old owned snapshot bytes");
        before.Verify(f, "failed preparation leaves all native pools unchanged");
    }
    std::printf("model paste history preparation rejected budgets=%u\n", failures);
    Check(failures >= 2 && failures < maxBudget, "allocation fault sweep reaches both native preparation buffers and then succeeds");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (unsigned mask = 1; mask < 8; ++mask) for (int frame : {0, 10, 20, 30}) {
        std::printf("model paste mask=%u frame=%d\n", mask, frame); Scenario(mask, frame);
    }
    HistoryPreparationFailures();
    std::printf("legacy model paste: %u checks passed\n", checks);
}
