#pragma once

#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_paste.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include <mms/plugin.h>
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio::mdl { struct BoneRecord; }

namespace mikumikustudio::plugins {

enum class GlobalCopySourceMode { Global };

struct GlobalCopyAccessoryReference {
    std::int32_t slot = -1;
    const mdl::AccessoryRecord* pointer = nullptr;
    MmsObjectRef identity{};
    bool referenced = false;
    bool available = false;
};

struct GlobalCopyModelReference {
    std::int32_t slot = -1;
    const unsigned char* pointer = nullptr;
    MmsObjectRef identity{};
    const mdl::BoneRecord* boneTable = nullptr;
    std::uint32_t boneCount = 0;
    bool referenced = false;
    bool available = false;
};

// Only const shared ownership escapes capture. Raw records own their storage;
// object pointers are identity tokens, never owners of scene objects.
struct GlobalCopySnapshot {
    GlobalPasteSource payload;
    // Complete effective authors at their original absolute frames. Adjacent
    // unselected keys and exact camera intervals describe the copied shape;
    // only payload records determine which keys a later paste may insert.
    std::vector<curve_host::CopyAuthoring> authoring;
    std::uint32_t nativeFirst = 0;
    std::uint64_t attempt = 0;
    std::uint64_t sceneGeneration = 0;
    const GlobalCopySourceMode sourceMode = GlobalCopySourceMode::Global;

    // Indexed by slot, recorded once even if many keys share the object.
    // Accessory slot 255 records an unavailable out-of-range reference.
    // Models use the maximum host capacity; capture checks kModelSlotCount
    // before accessing the actual (possibly smaller x86) app slot array.
    std::array<GlobalCopyAccessoryReference, 256> accessoryReferences{};
    std::array<GlobalCopyModelReference, 255> modelReferences{};
    // One permanently unavailable witness suffices for any invalid model
    // slots. It retains the first such slot; all original slots stay in payload.
    // parentModel == -1 denotes no model and creates no reference.
    GlobalCopyModelReference invalidModelReference;
};

// Invalidates the previous copy without allocation, even on counter exhaustion.
// A false return must abort the copy; Complete/Capture require a successful Begin.
bool BeginGlobalCopy(MMDApp&) noexcept;
void CompleteEmptyGlobalCopy(MMDApp&) noexcept;
MmsResult CaptureGlobalCopySnapshot(MMDApp&, std::uint32_t nativeFirst) noexcept;

// Returns the status, reporting an incoherent Captured envelope as Failed.
// Clears out unless the Captured envelope matches
// the current attempt and scene. Reference availability is checked separately:
// a raw Captured snapshot can be readable while Current returns false.
frame_clipboard::GlobalCopyStatus ReadGlobalCopySnapshot(
    const MMDApp&, std::shared_ptr<const GlobalCopySnapshot>& out) noexcept;

// Checks publication, object identities and model table/count structure only.
// This does not validate parent-bone contents, raw values or paste admission.
bool GlobalCopySnapshotCurrent(const MMDApp&, const GlobalCopySnapshot&) noexcept;

}
#endif
