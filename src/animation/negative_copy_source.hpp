#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <mms/scene.h>
#include "negative_authoring.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::negative_frames {
enum class CopySourceMode { Global, Model };
struct CopyObjectReference {
    int slot=-1;
    const void* pointer=nullptr;
    MmsObjectRef identity{};
    bool referenced=false, available=false;
};
struct CopyModelReference : CopyObjectReference {
    bool tracks=false;
    const void* bones=nullptr;
    const void* morphs=nullptr;
    const void* ikChains=nullptr;
    const void* selectors=nullptr;
    std::uint32_t boneCount=0, morphCount=0, ikCount=0, selectorCount=0;
};
struct CopySourceKey {
    const unsigned char* model=nullptr;
    const void* identity=nullptr;
    std::int32_t target=0, frame=0;
    bool morph=false, selected=false;
    std::array<double,16> values{};
    std::int64_t clipboardOffset=0;
    // Copy-time track metadata; missing/stale tracks keep the raw key above.
    std::array<char,20> name{};
    std::uint32_t occurrence=0;
    // IK/selector mapping at capture; selector zero may mean the whole model (-1).
    std::int32_t mappedBone=-1;
    bool trackAvailable=false;
    // Shared const ownership keeps full historical fields alive independently
    // of the source state. Null is LegacyLinear, not native default controls.
    authoring::Ptr author;
};
// Native implementation detail, not a public plugin ABI. Owns all copied
// values/names. Object pointers are tokens and never keep scene objects alive.
struct CopySource {
    const MMDApp* owner=nullptr;
    CopySourceMode mode=CopySourceMode::Global;
    int modelSlot=-1;
    std::uint64_t sceneGeneration=0;
    bool hadState=false, referencesAvailable=true;
    std::weak_ptr<const char> lifetime;
    std::int64_t first=0;
    std::vector<CopySourceKey> keys;
    std::array<CopyObjectReference,255> accessories{};
    std::array<CopyModelReference,255> models{};
};
// Captures selected live-storage rows in the active timeline, without using
// NativeClipboard or filtering stale rows. Empty is a successful empty source.
// Invalid current model context returns NOT_READY. Every failure clears out.
// Reference availability is separate from successful raw capture/semantics.
MmsResult CaptureCopySource(MMDApp&,std::shared_ptr<const CopySource>& out) noexcept;
bool HasSelectedCopyRows(MMDApp&) noexcept;
// Allocation-free identity/structure check, independent of current playhead,
// selection and later value edits. Checks selected IK/selector mappings, but
// not parent-bone values or compatibility fallbacks used by paste admission.
bool CopySourceCurrent(const MMDApp&,const CopySource&) noexcept;
}
#endif
