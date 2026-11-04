// ===========================================================================
// VA 0x004A8DC0 - ModelInitDefaults  (original: sub_4A8DC0, 1117 bytes)
// VA 0x004A89B0 - ModelInitMorphSlots (original: sub_4A89B0, 522 bytes)
// ===========================================================================
// Default-state initializer of the freshly allocated 0x4CCF4 model block
// (called from 0x460430 add-model, 0x450000 and 0x458F80 scene load); the
// caller has already zeroed the block (operator new + memset).
// Every store below mirrors the original one-to-one; only the 21 identity
// matrices and the 30x23 morph-slot zeroing loops are expressed as loops
// instead of unrolled stores (deviation noted in docs/ARCHITECTURE.md).
//
// Runtime accesses use the named, architecture-pinned ModelRecord fields.
// Pose-history storage is shared with the saved PMM state block on both ABIs.
// =========================================================================//
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include <cstdint>
#include <cstring>

#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"

namespace mikumikustudio {

void ModelInitMorphSlots(unsigned char* m);                  // 0x4A89B0

void ModelInitDefaults(unsigned char* m) {                   // 0x4A8DC0
    frame_clipboard::ForgetModel(m);
    model_vertex_cache::Forget(m);
    bone_scale::Forget(m);
    morph_curve::Forget(m);
    mdl::ModelRecord& model = *mdl::Mdl(m);
    text_encoding::ForgetMaterialNames(model);
    text_encoding::ForgetDisplayGroupNames(model);
    model.physicsMode = 0;
    // 9388..9404: the four PMX text buffers (name/comment JP/EN).
    for (wchar_t*& text : model.pmxTextBuffers)
        text = nullptr;
    for (std::int32_t& count : mdl::UvMorphCounts(m).byFamily)
        count = 0;
    for (auto& table : mdl::UvMorphTables(m).byFamily)
        table = nullptr;
    mdl::MaterialMorphBase(m) = nullptr;
    mdl::MaterialMorphAdd(m) = nullptr;
    mdl::MaterialMorphMul(m) = nullptr;
    mdl::BaseVertexMorphCount(m) = 0;
    mdl::BaseVertexMorphTable(m) = nullptr;
    mdl::BoneMorphOffsetCount(m) = 0;
    mdl::BoneMorphOffsets(m) = nullptr;
    model.rbGroups = nullptr;
    model.groupNames = nullptr;
    model.displayFrames = nullptr;
    model.morphs = nullptr;
    model.ikChains = nullptr;
    model.boneTable = nullptr;
    model.materials = nullptr;
    model.indices = nullptr;
    model.rawVertices = nullptr;
    model.pmxVertices = nullptr;
    model.vertexBuffer2 = nullptr;
    model.vertexBuffer = nullptr;
    mdl::Mdl(m)->boneKeyCursors = nullptr;
    mdl::Mdl(m)->boneTrackActive = nullptr;
    mdl::Mdl(m)->morphKeyCursors = nullptr;
    mdl::Mdl(m)->morphTrackActive = nullptr;
    mdl::Mdl(m)->displayKeyCursor = 0;
    mdl::Mdl(m)->displayTrackActive = 0;
    mikumikustudio::mdl::Mdl(m)->displayState = 0;
    mikumikustudio::mdl::Mdl(m)->selectedBone = 0;
    mdl::Mdl(m)->boneSelection = nullptr;
    mdl::Mdl(m)->bonePhysicsState = nullptr;
    mikumikustudio::mdl::Mdl(m)->loadComplete = 1;
    for (std::int32_t& selectedMorph : mdl::Mdl(m)->selectedMorphs)
        selectedMorph = -1;
    mikumikustudio::mdl::Mdl(m)->edgeScale = 1.0f;
    mdl::BoneKeys(m) = nullptr;
    mdl::MorphKeys(m) = nullptr;
    mdl::DisplayKeys(m) = nullptr;
    mdl::BoneKeyIndices(m) = nullptr;
    mdl::MorphKeyIndices(m) = nullptr;
    mikumikustudio::mdl::Mdl(m)->boneListPos = 0;
    mikumikustudio::mdl::Mdl(m)->maxFrame = 0;
    mikumikustudio::mdl::Mdl(m)->undoState[0] = 0;
    mikumikustudio::mdl::Mdl(m)->undoState[1] = 0;
    mikumikustudio::mdl::Mdl(m)->postLoadFlag2 = 0;
    std::memset(mikumikustudio::mdl::Mdl(m)->undoRings, 0,
                sizeof(mikumikustudio::mdl::Mdl(m)->undoRings));
    // 10804 (0x348 bytes = the x86 ring size): the second undo ring, i.e.
    // undoRings[1] on both architectures.
    std::memset(&model.undoRings[1], 0, sizeof(model.undoRings[1]));
    mdl::Mdl(m)->undoDirty = 0;
    mdl::Mdl(m)->redoDirty = 0;
    mikumikustudio::mdl::Mdl(m)->physicsFlags = 0;
    mdl::Mdl(m)->rigidTable = nullptr;
    mikumikustudio::mdl::Mdl(m)->rigidCount = 0;
    mikumikustudio::mdl::Mdl(m)->jointCount = 0;
    mdl::Mdl(m)->jointTable = nullptr;
    mdl::Mdl(m)->indexBuffer = nullptr;
    mikumikustudio::mdl::Mdl(m)->toonFlag = 1;
    mikumikustudio::mdl::Mdl(m)->toonShared = 0xFFFFFFFFu;
    model.displayKeyframesPresent = 0;
    model.morph0Count = 0;
    model.uvMorphCounts = {};
    model.boneMorphCount = 0;
    model.morph0Table = nullptr;
    model.boneMorphTable = nullptr;
    model.uvMorphTables = {};
    model.materialMorphPools = {};
    mikumikustudio::mdl::Mdl(m)->matFloat2 = -999.0f;
    mikumikustudio::mdl::Mdl(m)->boneCount = 0;
    mikumikustudio::mdl::Mdl(m)->matColumn[0] = -999.0f;
    mikumikustudio::mdl::Mdl(m)->morphCount = 0;
    mikumikustudio::mdl::Mdl(m)->matColumn[3] = -999.0f;
    mikumikustudio::mdl::Mdl(m)->ikChainCount = 0;
    mikumikustudio::mdl::Mdl(m)->matColumn[6] = -999.0f;
    mikumikustudio::mdl::Mdl(m)->displayRootBone = 0;
    mikumikustudio::mdl::Mdl(m)->matColumn[9] = -999.0f;
    mikumikustudio::mdl::Mdl(m)->pmxAdditionalUvCount = 0;
    mikumikustudio::mdl::Mdl(m)->matColumn[12] = -999.0f;
    mdl::Mdl(m)->maxBoneLayer = 0;
    mikumikustudio::mdl::Mdl(m)->matColumn[15] = -999.0f;
    mdl::Mdl(m)->boneOrderTable = nullptr;
    mikumikustudio::mdl::Mdl(m)->centerBone = 0;
    mdl::Mdl(m)->frameRegistrationSelection = 3;
    for (auto& position : model.standardBonePositions)
        position[1] = -999.0f;
    for (std::size_t i = 0; i < 17; ++i) {
        auto* q = model.localTransforms + 4 * i;
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }

    ModelInitMorphSlots(m);                                // 0x4A89B0

    mdl::PoseTraceFlag(m) = 0;
    mikumikustudio::mdl::Mdl(m)->lightDir[0] = -1.0f;
    mikumikustudio::mdl::Mdl(m)->lightDir[1] = 90.0f;
    mikumikustudio::mdl::Mdl(m)->lightDir[2] = 10.0f;
    mdl::Mdl(m)->legIkXOffset = 1.0f;
    mdl::PoseTraceBuffer(m) = nullptr;
    mikumikustudio::mdl::Mdl(m)->matMisc = 0;
}

void ModelInitMorphSlots(unsigned char* m) {               // 0x4A89B0
    for (auto& channel : mdl::Mdl(m)->poseHistory)
        for (auto& sample : channel) {
            sample[0] = sample[2] = 0.0f;
            sample[1] = -999.0f;
        }
    mikumikustudio::mdl::Mdl(m)->lightDir[0] = -1.0f;
    mdl::PoseTraceFlag(m) = 0;  // 8616; x64 0x21E0
    mikumikustudio::mdl::Mdl(m)->lightDir[1] = 90.0f;
    mikumikustudio::mdl::Mdl(m)->lightDir[2] = 10.0f;
    mdl::Mdl(m)->legIkXOffset = 1.0f;
}

}  // namespace mikumikustudio
