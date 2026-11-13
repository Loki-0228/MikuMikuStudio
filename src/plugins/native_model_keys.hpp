#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "native_bone_registration.hpp"
#include "native_display_keys.hpp"
#include "native_morph_keys.hpp"
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// One atomic model-key deletion across bone, morph and display/IK/parent
// tracks. Keyboard fallback is considered only when all families are unmarked.
// A null successful result is a no-op. The caller owns the serialized editing
// scope and budgets retained model resources separately from these records.
struct ModelKeySelection {
    std::vector<unsigned> bones, morphs, displays;
};
class PreparedModelKeyDeletion : public PreparedHistoryChange {
public:
    virtual MMDApp* OwnerApp() const noexcept = 0;
    virtual unsigned char* ModelObject() const noexcept = 0;
    // Borrowed from child candidates owned by this aggregate. The display
    // target is absent when this deletion does not alter the display family.
    virtual const std::vector<BoneDeletionTarget>& BoneZeroTargets() const noexcept = 0;
    virtual const std::vector<MorphDeletionTarget>& MorphZeroTargets() const noexcept = 0;
    virtual const DisplayDeletionTarget* DisplayZeroTarget() const noexcept = 0;
    // Visits precisely the edited model and leased source/target selector
    // parents, including unchangedDisplay. Duplicates are intentional: feed
    // every visit to one shared resource collector. The visitor may throw;
    // traversal itself allocates nothing and never visits unrelated models.
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
};
class ModelKeyDeletion : public HistoryChange {
public:
    // Visit holders already owned by every child record and metadata, before
    // preparing candidates. No allocation, new retain or unrelated scene scan;
    // feed duplicates to one collector. The visitor may throw. This does not
    // include candidate-only leases or change RetainedBytes payload accounting.
    virtual void VisitRetainedModels(void*, ModelDeletionOwnerVisitor) const = 0;
    // Prepare display zero windows before bone targets. At negative playheads
    // no child publishes a live pose; the enclosing signed transaction must
    // supply its final pose after validating both signed halves together.
    virtual MmsResult PrepareSigned(bool forward,
        std::unique_ptr<PreparedModelKeyDeletion>&) noexcept = 0;
    // One positive-half transient allowance across layout, children, read
    // dependencies and metadata. Failure nulls out and preserves the balance;
    // success leaves the remainder available to the negative half.
    virtual MmsResult PrepareSignedBudgeted(bool forward, std::uint64_t& remainingBytes,
        std::unique_ptr<PreparedModelKeyDeletion>&) noexcept = 0;
};
MmsResult PlanModelKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, bool selectedAtCurrentFrame = false,
    const ModelKeySelection* privateSelection = nullptr) noexcept;
MmsResult PlanSignedModelKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<ModelKeyDeletion>&, bool selectedAtCurrentFrame = false,
    const ModelKeySelection* privateSelection = nullptr) noexcept;
// Finds redundant interior keys without writing live selection marks. The
// detached selection, authoring and all three key families publish together.
MmsResult PlanUnusedModelKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, std::uint32_t& deleted) noexcept;
struct ModelRangeScaleOptions {
    std::uint32_t start = 0, end = 0;
    double scale = 1;
    bool bones = false, morphs = false, display = false;
    bool automaticInterpolation = false;
};
MmsResult PlanModelRangeScale(MMDApp&, unsigned modelSlot, const ModelRangeScaleOptions&,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
