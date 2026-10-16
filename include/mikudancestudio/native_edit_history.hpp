#pragma once
#include "mikudancestudio/build_profile.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace mikudancestudio {
// Success means native authoring and the history cursor both changed. A failed
// preparation leaves them untouched, including the availability/dirty flags.
bool TryUndoModelEdit(unsigned char*, std::int32_t*, int physicsMode = 2);
bool TryRedoModelEdit(unsigned char*, std::int32_t*, int physicsMode = 2);

#if !MMD_ORIGINAL_PROFILE
namespace native_history {
struct RingStep { unsigned slot; bool redo; };
// Internal prepared side-table exchange. Callers retain the live model and
// serialize edits from preparation through publication; this is not a C ABI.
class SideState {
public:
    virtual ~SideState() = default;
    virtual bool IsCurrent() const noexcept = 0;
    virtual void Publish() noexcept = 0;
};
enum class Result { Ready, Empty, Invalid, Memory };
class OwnedPoseEdit;
class ModelEdit {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class OwnedPoseEdit;
public:
    ModelEdit();
    ~ModelEdit();
    ModelEdit(ModelEdit&&) noexcept;
    ModelEdit& operator=(ModelEdit&&) noexcept;
    ModelEdit(const ModelEdit&) = delete;
    ModelEdit& operator=(const ModelEdit&) = delete;
    Result Prepare(unsigned char*, std::int32_t frame, bool redo) noexcept;
    // The model must still be alive. Object lifetime/generation checks belong
    // to the host before calling this internal pointer-based helper.
    bool IsCurrent(std::int32_t frame) const noexcept;
    // Revalidates, then only copies and swaps prepared data. No allocation,
    // window messages, plugin callbacks, evaluation or physics notifications.
    bool Publish(std::int32_t& frame) noexcept;
    // Post-publication UI/physics refresh, kept outside the atomic exchange.
    void Refresh(int physicsMode);
};
// A type-1 pose edit owns both directions independently of the legacy ring.
// The host retains the model and verifies its scene identity before Prepare.
// This first implementation covers position, rotation, unregistered-pose flags
// and scale pose only. It never restores keyframes, curve tracks or ring state.
// Position components and dirty flags are individual deltas; the quaternion is
// one rotation value. Fields untouched by the edit retain their live values.
class OwnedPoseEdit {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class PoseEditCapture;
public:
    OwnedPoseEdit();
    ~OwnedPoseEdit();
    OwnedPoseEdit(OwnedPoseEdit&&) noexcept;
    OwnedPoseEdit& operator=(OwnedPoseEdit&&) noexcept;
    OwnedPoseEdit(const OwnedPoseEdit&) = delete;
    OwnedPoseEdit& operator=(const OwnedPoseEdit&) = delete;
    Result Prepare(unsigned char*, std::int32_t frame, bool forward, ModelEdit&) const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
};
class PoseEditCapture {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PoseEditCapture();
    ~PoseEditCapture();
    PoseEditCapture(PoseEditCapture&&) noexcept;
    PoseEditCapture& operator=(PoseEditCapture&&) noexcept;
    PoseEditCapture(const PoseEditCapture&) = delete;
    PoseEditCapture& operator=(const PoseEditCapture&) = delete;
    Result Begin(unsigned char*) noexcept;
    // Begin allocates the owned record and both delta capacities. This exact
    // upper charge stays valid through Finish; zero means no active capture.
    std::uint64_t MaximumRetainedBytes() const noexcept;
    // Only operation 1 is supported. Finish leaves the capture available for
    // Rollback if the host cannot adopt the edit. Empty means no authored change.
    Result Finish(int operation, std::unique_ptr<OwnedPoseEdit>&) noexcept;
    // Restores captured type-1 pose authoring without allocation. Use only while
    // the same edit scope excludes other authored edits. A changed physical mode
    // is unsupported and rejects the entire rollback. Ring/UI/journal metadata
    // belongs to the host. Destruction itself never changes the model.
    bool Rollback() noexcept;
};
}
namespace bone_scale {
std::unique_ptr<native_history::SideState> PrepareHistory(unsigned char*, const std::vector<native_history::RingStep>&);
}
namespace bone_curve {
std::unique_ptr<native_history::SideState> PrepareHistory(unsigned char*, const std::vector<native_history::RingStep>&);
}
namespace mdl { struct MorphKey; }
namespace sequence_scatter {
bool PrepareHistory(unsigned char*, const void*, std::vector<mdl::MorphKey>&,
                    std::unique_ptr<native_history::SideState>&, std::uint32_t&, std::int32_t&);
}
#endif
}
