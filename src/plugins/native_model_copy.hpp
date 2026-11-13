#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_model_paste.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include <mms/scene.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mikumikustudio::plugins {

struct ModelCopyModelReference {
    int slot = -1;
    const unsigned char* pointer = nullptr;
    MmsObjectRef identity{};
    const void* bones = nullptr;
    const void* morphs = nullptr;
    const void* ikChains = nullptr;
    const void* selectors = nullptr;
    std::uint32_t boneCount = 0, morphCount = 0, ikCount = 0, selectorCount = 0;
    bool referenced = false, available = false, tracks = false;
};

struct ModelCopyTrack {
    // Original source table index, never a name-matched fallback destination.
    int index = -1;
    std::array<char,20> name{};
    std::uint32_t occurrence = 0;
    bool available = false;
};

struct ModelCopyMapping {
    std::uint32_t tableIndex = 0;
    int mappedBone = -1;
    std::array<char,20> name{};
    std::uint32_t occurrence = 0;
    // Selector zero with mappedBone == -1 is an available whole-model entry.
    bool available = false;
};

struct ModelCopyDisplaySource {
    std::vector<ModelCopyMapping> ik, selectors;
};

// Internal preparation only: no publication/batch state or public plugin ABI.
// Metadata vectors are parallel to payload records, including unavailable ones.
// Borrowed object/table pointers are tokens and never keep scene storage alive.
struct ModelCopySnapshot {
    const MMDApp* owner = nullptr;
    std::uint64_t sceneGeneration = 0;
    std::uint32_t nativeFirst = 0;
    int modelSlot = -1;
    ModelPasteSource payload;
    bool referencesAvailable = true;
    // The source has tracks=true; selector parents need only bone structure.
    // A self-parent retains the source's stronger four-table stamp.
    std::array<ModelCopyModelReference,255> models{};
    std::vector<ModelCopyTrack> bones, morphs;
    std::vector<ModelCopyDisplaySource> displays;
    // Effective authoring for selected source tracks, captured at copy time.
    // Absolute frames and unselected neighbours are retained so later signed
    // admission can preserve handle geometry without guessing from live pose.
    // Absence means native interpolation; these never create a default author.
    std::vector<bone_curve::CopyAuthoring> boneAuthoring;
    std::vector<morph_curve::CopyAuthoring> morphAuthoring;
};

// Call immediately after successful Cmd400_FrameCopy has installed its source
// mappings, with the same selected model. Owns raw payload even if dependencies
// are unavailable. Missing model is NOT_READY; empty payload may succeed.
// Only finds existing identities. Every failure clears out.
MmsResult CaptureModelCopySnapshot(MMDApp&, std::uint32_t nativeFirst,
    std::shared_ptr<const ModelCopySnapshot>& out) noexcept;
// Allocation-free source dependency guard, independent of selection, playhead,
// mode, later values/names and clipboard replacement. Not batch validation or
// full semantic paste admission (including parent-bone index/value validity).
bool ModelCopySnapshotCurrent(const MMDApp&, const ModelCopySnapshot&) noexcept;

}
#endif
