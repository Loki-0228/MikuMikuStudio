#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/model.hpp"
#include <array>
#include <vector>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Compact model-space records, with native body/key handles ignored on input.
// Active joints address this draft's compact rigid indices. Their constraint
// is nonnegative (the UID is regenerated); -1 preserves a disabled source joint
// without constructing a native constraint or repairing its raw endpoints.
// Masks use the
// model's no-collapse representation, not the dialog's inverted edit mask.
// Each nonempty UTF-16 allocation includes a terminator and preserves any raw
// tail after it. Empty vectors represent absent pointers. Record jpText/enText
// are not borrowed; the corresponding names arrays are the source of truth.
using PhysicsDraftNames = std::array<std::vector<wchar_t>, 2>;
struct ModelPhysicsDraft {
    std::vector<mdl::RigidRecord> rigids;
    std::vector<mdl::JointRecord> joints;
    std::vector<PhysicsDraftNames> rigidNames, jointNames;
};
class NativeModelPhysicsPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeModelPhysicsPlan() noexcept = default;
    NativeModelPhysicsPlan(const NativeModelPhysicsPlan&) = delete;
    NativeModelPhysicsPlan& operator=(const NativeModelPhysicsPlan&) = delete;
    MmsResult Begin(MMDApp&, unsigned slot) noexcept;
    bool IsCurrent() const noexcept;
    MmsResult Read(ModelPhysicsDraft&) const noexcept;
    MmsResult Set(const ModelPhysicsDraft&) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
// The real editor owns its private draft until one explicit OK. Bound failure,
// cancellation and a stale source must never fall through to legacy writers.
bool TryEditBoundModelPhysics(MMDApp*, void* ownerWindow) noexcept;
bool BoundPhysicsModelDraft(const MMDApp*) noexcept;
MmsResult CommitBoundModelPhysics(MMDApp&, SharedHistory&, std::shared_ptr<HistoryChange>) noexcept;
MmsResult CommitUnboundModelPhysics(MMDApp&, std::shared_ptr<HistoryChange>) noexcept;
}
#endif
