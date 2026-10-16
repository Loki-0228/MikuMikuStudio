#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// All global and 255 accessory key-selection flags. The app and its scene
// owners outlive this record. Pool growth keeps identity; replacement is stale.
class GlobalKeySelection {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    explicit GlobalKeySelection(MMDApp&);
    ~GlobalKeySelection();
    class Candidate {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        explicit Candidate(std::unique_ptr<Impl>);
        friend class GlobalKeySelection;
    public:
        ~Candidate();
        bool IsCurrent() const noexcept;
        // Caller validates every candidate before any write. Resolve current
        // pools again, so this may run after identity-preserving pool growth.
        // Clear captured ranges; undo restores their original nonzero marks.
        // Later-grown tails remain owned by their own edits. An inserting
        // record separately restores the mark of its newly grown target slot.
        void Publish() noexcept;
    };
    std::unique_ptr<Candidate> Prepare(bool forward, bool initial = false) const;
    std::uint64_t RetainedBytes() const noexcept;
};
MmsResult PlanCameraRegistration(MMDApp&, int frame, bool resetInterpolation,
    std::shared_ptr<HistoryChange>& out) noexcept;
MmsResult PlanLightRegistration(MMDApp&, int frame,
    std::shared_ptr<HistoryChange>& out) noexcept;
// The caller parses/clamps the slider without changing live state. The first
// publication records and installs distance together; undo/redo sample shadow.
MmsResult PlanShadowRegistration(MMDApp&, int frame, float distance,
    std::shared_ptr<HistoryChange>& out) noexcept;
}
#endif
