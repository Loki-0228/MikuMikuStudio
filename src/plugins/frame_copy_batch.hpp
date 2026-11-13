#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/frame_clipboard.hpp"
#include <mms/plugin.h>
#include <cstdint>
#include <memory>

namespace mikumikustudio::negative_frames { struct CopySource; }
namespace mikumikustudio::plugins {
struct ModelCopySnapshot;
struct FrameCopyTicket {
    const MMDApp* owner = nullptr;
    frame_clipboard::CopyMode mode = frame_clipboard::CopyMode::Global;
    std::uint64_t attempt = 0, sceneGeneration = 0;
    std::uint64_t previousGlobalAttempt = 0;
};
// A production 420 receipt owns both portions from the same copy. Native
// records retain relative frames; first is their combined signed origin.
// Captured means owned raw sources, not semantic paste admission.
struct FrameCopyBatch {
    FrameCopyBatch() = default;
    FrameCopyBatch(const FrameCopyBatch& other)
        : ticket(other.ticket), hasNative(other.hasNative), nativeFirst(other.nativeFirst), first(other.first),
          global(other.global), model(other.model), negative(other.negative), sourceRetired(other.sourceRetired) {}
    FrameCopyBatch& operator=(const FrameCopyBatch&) = delete;
    ~FrameCopyBatch();
    FrameCopyTicket ticket;
    bool hasNative = false;
    std::uint32_t nativeFirst = 0;
    std::int64_t first = 0;
    std::shared_ptr<const GlobalCopySnapshot> global;
    std::shared_ptr<const ModelCopySnapshot> model;
    std::shared_ptr<const negative_frames::CopySource> negative;
    // Lifecycle invalidation changes availability, never the owned payload.
    mutable bool sourceRetired = false;
private:
    friend struct FrameCopyRegistry;
    mutable const FrameCopyBatch* previous = nullptr;
    mutable const FrameCopyBatch* next = nullptr;
    mutable bool observed = false;
};
bool BeginFrameCopy(MMDApp&, frame_clipboard::CopyMode, FrameCopyTicket&) noexcept;
bool FrameCopyPreparing(const MMDApp&, const FrameCopyTicket&) noexcept;
// False means superseded: the caller must leave the newer attempt alone.
bool FailFrameCopy(MMDApp&, const FrameCopyTicket&, MmsResult) noexcept;
MmsResult CaptureFrameCopyBatch(MMDApp&, const FrameCopyTicket&, std::uint32_t nativeFirst) noexcept;
frame_clipboard::CopyBatchStatus ReadFrameCopyBatch(const MMDApp&, frame_clipboard::CopyMode,
    std::shared_ptr<const FrameCopyBatch>&) noexcept;
bool FrameCopyBatchCurrent(const MMDApp&, const FrameCopyBatch&) noexcept;
void ForgetFrameCopyModel(const unsigned char*) noexcept;
}
#endif
