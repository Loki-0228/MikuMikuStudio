#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <cstddef>
#include <cstdint>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
enum class ModelOrderKind { Draw, Calculate };
// Capture before opening the editor. Read its slot+1 scratch permutation
// without publishing; Take is consumed only after the modal has closed.
class NativeModelOrderPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&, ModelOrderKind) noexcept;
    MmsResult Read(const std::int32_t* oneBasedSlots, std::size_t count) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
}
#endif
