#include "frame_copy_batch.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_copy.hpp"
#include "native_model_copy.hpp"
#include "../animation/negative_copy_source.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <algorithm>
#include <limits>
#include <new>

namespace mikumikustudio::plugins {
// Intrusive, non-owning observations disappear with the snapshot. Registration
// cannot allocate, retain a control block, or change the OOM path on later copies.
struct FrameCopyRegistry {
    static const FrameCopyBatch* head;
    static void Attach(const FrameCopyBatch& batch) noexcept {
        batch.next = head; batch.previous = nullptr; batch.observed = true;
        if(head) head->previous = &batch;
        head = &batch;
    }
    static void Detach(const FrameCopyBatch& batch) noexcept {
        if(!batch.observed) return;
        if(batch.previous) batch.previous->next = batch.next; else head = batch.next;
        if(batch.next) batch.next->previous = batch.previous;
        batch.previous = batch.next = nullptr; batch.observed = false;
    }
    static const FrameCopyBatch* Next(const FrameCopyBatch& batch) noexcept { return batch.next; }
};
const FrameCopyBatch* FrameCopyRegistry::head = nullptr;
FrameCopyBatch::~FrameCopyBatch() { FrameCopyRegistry::Detach(*this); }
namespace frame_copy_detail {
using Status = frame_clipboard::CopyBatchStatus;
using Mode = frame_clipboard::CopyMode;
void Retire(frame_clipboard::CopyBatchState& state, MmsResult error) noexcept {
    state.snapshot.reset(); state.status = Status::Failed;
    state.error = error; state.preparing = false;
}
bool TicketMatches(const MMDApp& app, const FrameCopyTicket& ticket) noexcept {
    if (ticket.owner != &app || !ticket.attempt || ticket.sceneGeneration != app.SceneGeneration()) return false;
    const auto& state = app.FrameClipboardCopy(ticket.mode);
    return state.attempt == ticket.attempt && state.sceneGeneration == ticket.sceneGeneration;
}
bool Published(const MMDApp& app, const FrameCopyBatch& batch) noexcept {
    if (!TicketMatches(app, batch.ticket)) return false;
    const auto& state = app.FrameClipboardCopy(batch.ticket.mode);
    return state.status == Status::Captured && !state.preparing && state.error == MMS_OK &&
        state.snapshot.get() == &batch;
}
}

bool BeginFrameCopy(MMDApp& app, frame_clipboard::CopyMode mode, FrameCopyTicket& ticket) noexcept {
    ticket = {};
    auto& state = app.FrameClipboardCopy(mode);
    frame_copy_detail::Retire(state, MMS_E_NOT_READY);
    state.sceneGeneration = app.SceneGeneration();
    if (state.attempt == (std::numeric_limits<std::uint64_t>::max)()) return false;
    ++state.attempt; state.preparing = true;
    ticket = {&app, mode, state.attempt, state.sceneGeneration, app.GlobalClipboardCopy().attempt};
    return true;
}
bool FrameCopyPreparing(const MMDApp& app, const FrameCopyTicket& ticket) noexcept {
    if (!frame_copy_detail::TicketMatches(app, ticket)) return false;
    const auto& state = app.FrameClipboardCopy(ticket.mode);
    return state.preparing && state.status == frame_copy_detail::Status::Failed &&
        state.error == MMS_E_NOT_READY && !state.snapshot;
}
bool FailFrameCopy(MMDApp& app, const FrameCopyTicket& ticket, MmsResult error) noexcept {
    if (!FrameCopyPreparing(app, ticket)) return false;
    frame_copy_detail::Retire(app.FrameClipboardCopy(ticket.mode), error);
    return true;
}
MmsResult CaptureFrameCopyBatch(MMDApp& app, const FrameCopyTicket& ticket,
                              std::uint32_t nativeFirst) noexcept {
    using namespace frame_copy_detail;
    if (!FrameCopyPreparing(app, ticket)) return MMS_E_STALE;
    const auto fail = [&](MmsResult error) noexcept { FailFrameCopy(app, ticket, error); return error; };
    if ((app.CameraMode() != 0) != (ticket.mode == Mode::Global)) return fail(MMS_E_STALE);
    std::shared_ptr<const GlobalCopySnapshot> global;
    bool hasNative = false;
    if(ticket.mode == Mode::Global) {
        const auto& native = app.GlobalClipboardCopy();
        if(ticket.previousGlobalAttempt == (std::numeric_limits<std::uint64_t>::max)() ||
            native.attempt != ticket.previousGlobalAttempt + 1 || native.preparing ||
            native.sceneGeneration != ticket.sceneGeneration) return fail(MMS_E_STALE);
        const auto status = ReadGlobalCopySnapshot(app, global);
        if(status == frame_clipboard::GlobalCopyStatus::Captured) {
            if(!global || global->nativeFirst != nativeFirst) return fail(MMS_E_STALE);
            hasNative = true;
        } else if(status != frame_clipboard::GlobalCopyStatus::Empty) return fail(MMS_E_NOT_READY);
    } else {
        if(app.SelectedModelSlot() >= kModelSlotCount || !app.SelectedModel()) return fail(MMS_E_NOT_READY);
        const auto& counts = app.ClipboardCounts();
        hasNative = counts.bones || counts.morphs || counts.displays;
    }
    // Empty is decided from raw selected rows, not the filtering legacy adapter.
    // No descriptor allocation is required for an empty clipboard operation.
    if(!hasNative && !negative_frames::HasSelectedCopyRows(app)) {
        auto& state=app.FrameClipboardCopy(ticket.mode);
        state.snapshot.reset(); state.status=Status::Empty; state.error=MMS_OK; state.preparing=false;
        return MMS_OK;
    }
    try {
        auto batch = std::make_shared<FrameCopyBatch>(); batch->ticket = ticket;
        batch->hasNative = hasNative; batch->global = std::move(global);
        if (ticket.mode == Mode::Model) {
            if (hasNative) {
                if (const auto result = CaptureModelCopySnapshot(app, nativeFirst, batch->model)) return fail(result);
            }
        }
        if (const auto result = negative_frames::CaptureCopySource(app, batch->negative)) return fail(result);
        if (!batch->negative ||
            (batch->negative->mode == negative_frames::CopySourceMode::Global) != (ticket.mode == Mode::Global))
            return fail(MMS_E_STALE);
        if (!FrameCopyPreparing(app, ticket)) return MMS_E_STALE;
        auto& state = app.FrameClipboardCopy(ticket.mode);
        const bool hasNegative = !batch->negative->keys.empty();
        if (!batch->hasNative && !hasNegative) {
            state.snapshot.reset(); state.status = Status::Empty;
        } else {
            if (batch->hasNative) {
                batch->nativeFirst = nativeFirst;
                batch->first = nativeFirst;
            }
            if (hasNegative) batch->first = batch->hasNative ?
                (std::min)(batch->first, batch->negative->first) : batch->negative->first;
            FrameCopyRegistry::Attach(*batch);
            state.snapshot = std::move(batch); state.status = Status::Captured;
        }
        state.error = MMS_OK; state.preparing = false;
        return MMS_OK;
    } catch (const std::bad_alloc&) { return fail(MMS_E_MEMORY); }
      catch (...) { return fail(MMS_E_FAILED); }
}
frame_clipboard::CopyBatchStatus ReadFrameCopyBatch(const MMDApp& app, frame_clipboard::CopyMode mode,
    std::shared_ptr<const FrameCopyBatch>& out) noexcept {
    const auto& state = app.FrameClipboardCopy(mode); const auto snapshot = state.snapshot;
    if (snapshot && snapshot->ticket.mode == mode && frame_copy_detail::Published(app, *snapshot)) out = snapshot;
    else {
        out.reset();
        if (state.status == frame_clipboard::CopyBatchStatus::Captured) return frame_clipboard::CopyBatchStatus::Failed;
    }
    return state.status;
}
bool FrameCopyBatchCurrent(const MMDApp& app, const FrameCopyBatch& batch) noexcept {
    if (!frame_copy_detail::Published(app, batch) || batch.sourceRetired || !batch.negative ||
        !negative_frames::CopySourceCurrent(app, *batch.negative)) return false;
    if (!batch.hasNative) return !batch.global && !batch.model;
    if (batch.ticket.mode == frame_clipboard::CopyMode::Global)
        return batch.global && !batch.model && GlobalCopySnapshotCurrent(app, *batch.global);
    return batch.model && !batch.global && ModelCopySnapshotCurrent(app, *batch.model);
}
void ForgetFrameCopyModel(const unsigned char* model) noexcept {
    if (!model) return;
    for (auto batch = FrameCopyRegistry::head; batch; batch = FrameCopyRegistry::Next(*batch)) {
        const auto mentions = [model](const auto& refs) {
            return std::any_of(refs.begin(), refs.end(), [model](const auto& ref) {
                return ref.referenced && ref.pointer == model;
            });
        };
        if ((batch->model && mentions(batch->model->models)) ||
            (batch->global && mentions(batch->global->modelReferences)) ||
            (batch->negative && mentions(batch->negative->models))) batch->sourceRetired = true;
    }
}
}
#endif
