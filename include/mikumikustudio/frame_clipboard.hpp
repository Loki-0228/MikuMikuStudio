#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
class MMDApp;
namespace plugins { struct GlobalCopySnapshot; struct FrameCopyBatch; }

namespace frame_clipboard {
enum class GlobalCopyStatus { Unavailable, Empty, Captured, Failed };
// Native host state, outside the compatibility layout and public plugin ABI.
// The state itself needs no allocation, so even a first-copy OOM is observable.
struct GlobalCopyState {
    GlobalCopyStatus status = GlobalCopyStatus::Unavailable;
    std::uint64_t attempt = 0, sceneGeneration = 0;
    std::int32_t error = 0;
    bool preparing = false;
    std::shared_ptr<const plugins::GlobalCopySnapshot> snapshot;
};
enum class CopyMode { Model, Global };
enum class CopyBatchStatus { Unavailable, Empty, Captured, Failed };
struct CopyBatchState {
    CopyBatchStatus status = CopyBatchStatus::Unavailable;
    std::uint64_t attempt = 0, sceneGeneration = 0;
    std::int32_t error = 0;
    bool preparing = false;
    std::shared_ptr<const plugins::FrameCopyBatch> snapshot;
};
// Identity is available, but its occurrence does not exist in the destination.
inline constexpr int kMissingTarget = -2;
// Returns the original same-model track, or matches cross-model names by their
// occurrence in the complete model table. -1 requests legacy name resolution.
int BoneTarget(const MMDApp* app, const unsigned char* model, std::size_t index);
int MorphTarget(const MMDApp* app, const unsigned char* model, std::size_t index);
void Reset(const MMDApp* app);
void ForgetModel(const unsigned char* model);
}
}
