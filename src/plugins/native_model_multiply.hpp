#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <array>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct BoneMultiplyOptions {
    // Native position XYZ, Euler XYZ (degree offsets), then MMS scale XYZ.
    std::array<float, 9> scale{1, 1, 1, 1, 1, 1, 1, 1, 1};
    std::array<float, 9> offset{};
};
struct MorphMultiplyOptions { float scale = 1, offset = 0; };
MmsResult PlanBoneKeyMultiply(MMDApp&, unsigned modelSlot, const BoneMultiplyOptions&,
    std::shared_ptr<HistoryChange>&) noexcept;
MmsResult PlanMorphKeyMultiply(MMDApp&, unsigned modelSlot, const MorphMultiplyOptions&,
    std::shared_ptr<HistoryChange>&) noexcept;
enum class ModelMultiplyKind { Bone, Morph };
// Capture before the modal, read privately on OK, then take one explicit
// history participant after closing. Cancellation/no-op never touches redo.
// The plan retains its model and rejects changes to its modal source. Replay
// uses the same model identity, independently of the current selection.
class NativeModelMultiplyPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&, ModelMultiplyKind) noexcept;
    MmsResult Read(const BoneMultiplyOptions&) noexcept;
    MmsResult Read(const MorphMultiplyOptions&) noexcept;
    MmsResult ReadDialog(HWND) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
}
#endif
