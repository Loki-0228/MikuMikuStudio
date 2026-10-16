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
class ModelEdit {
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
