#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <memory>
#include <mms/edit.h>
#include "../plugins/shared_history.hpp"
namespace mikumikustudio {
class MMDApp;
namespace plugins {class PreparedGlobalKeyDeletion;class PreparedModelKeyDeletion;}
namespace negative_frames {
struct DeletionSelection {
    bool negative=false;
    bool positive=false;
};
// Does not acquire/check the journal gate; safe while the caller holds it.
// Checks UI/save/drag, signed frame, selected model slot and managed pools.
// Replay may pass only the real history publisher's own UI/scene lock; asking
// for replay alone does not grant that authorization or bypass other checks.
bool DeletionInteractionReady(MMDApp&, bool historyTraversal=false) noexcept;
// Read-only, allocation-free admission and explicit selection snapshot for the
// currently bound history/active timeline, including signed playhead bounds.
// UI/binding refusal returns MMS_E_NOT_READY; history errors are propagated.
// Every failure leaves out unchanged. Success may report both flags false;
// it does not prepare/reserve a deletion or enable current-frame fallback.
// The caller must still prepare and validate its plan.
MmsResult QueryDeletionSelection(MMDApp&,plugins::SharedHistory&,DeletionSelection&) noexcept;
struct DeletionTargets {
    const plugins::PreparedGlobalKeyDeletion* globals=nullptr;
    const plugins::PreparedModelKeyDeletion* model=nullptr;
    unsigned modelSlot=0;
};
class SelectedDeletion : public plugins::HistoryChange {
public:
    // Target candidates are borrowed only during preparation. The coordinator
    // must retain and validate BOTH sides before publishing native keys first,
    // then this negative key/pose candidate. This does not admit a UI command.
    // The caller charges positive preparation separately; remainingBytes is
    // reduced only on success by this participant's transient preparation.
    virtual MmsResult PrepareSigned(bool forward,bool initial,const DeletionTargets&,
        std::uint64_t& remainingBytes,std::unique_ptr<plugins::PreparedHistoryChange>&) noexcept=0;
};
// includeUnchanged supports positive-only changes and zero dependencies even
// when no negative storage exists. Planning/preparing never creates live state.
MmsResult PlanSignedSelectedDeletion(MMDApp&,std::shared_ptr<SelectedDeletion>&,
    bool includeUnchanged=false) noexcept;
// Internal detached deletion for selected keys in the active timeline. A null
// success is empty. Signed command coordination uses PlanSignedSelectedDeletion
// to reconcile shared selection and target zero-frame baselines instead.
// This never deletes positive keys or changes current selection/pose itself.
MmsResult PlanSelectedDeletion(MMDApp&,std::shared_ptr<plugins::HistoryChange>&) noexcept;
}
}
#endif
