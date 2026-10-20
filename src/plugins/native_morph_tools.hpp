#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
enum class NativeMorphTool { MouthShift, RandomBlink };
// Capture before the modal loop. Read generates only a private candidate;
// Take is committed once, after the dialog closes, as ExplicitCommit. Random
// samples are consumed here only, never during Prepare/undo/redo. A null change
// is a true no-op. Dirty weights and unrelated model pose are retained.
class NativeMorphToolsPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&, NativeMorphTool) noexcept;
    MmsResult ReadShift(int frames) noexcept;
    // Optional internal deterministic source for native contract tests. Each
    // result must follow Windows rand's 0..32767 range; it is not retained.
    MmsResult ReadBlink(int start, int end, int (*random)() = nullptr) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
    unsigned BlinkCount() const noexcept;
};
}
#endif
