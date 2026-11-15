#pragma once

namespace mikumikustudio::project_recovery {

// The caller has already verified and pinned every existing regular stage.
// Leave the journal recoverable through any interrupted or failed removal.
template<class Stages, class RemoveStage, class FinishStages, class RemoveJournal>
void CompleteCleanup(Stages& stages, RemoveStage&& removeStage, FinishStages&& finishStages,
                     RemoveJournal&& removeJournal) {
    for (auto& stage : stages) removeStage(stage);
    finishStages();
    removeJournal();
}

}
