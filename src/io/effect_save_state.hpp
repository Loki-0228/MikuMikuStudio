#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mikumikustudio::effect_save {

// SAVE-02 private save-completion protocol, first isolated step.
//
// This header models capture identity and completion bookkeeping only. The
// request it tracks must already have been sealed safely by the caller outside
// this protocol. It stores no effect bytes, validates no EMM format, performs
// no budget admission, proves no capture success, carries no provider callback
// or live effect pointer, and is not connected to production saving.
//
// The real MME-RE currently offers realtime serialization and void
// notifications only; neither is freeze plus acknowledgement evidence and
// neither may be reported here as a completed acknowledgement.

struct CaptureIdentity {
    // sceneGeneration and providerInstance must be non-zero on a valid request.
    // historyBinding/historyRevision stay zero on the original configuration,
    // which has no shared history binding.
    std::uint64_t sceneGeneration = 0;
    std::uint64_t sceneRevision = 0;
    std::uint64_t historyBinding = 0;
    std::uint64_t historyRevision = 0;
    std::uint64_t providerInstance = 0;
    std::uint64_t effectRevision = 0;
    // Stable object identities supplied by a future adapter, in capture order.
    // Slot indices and previous-frame draw order are not stable identities.
    std::vector<std::uint64_t> objectIds;

    // Strict equality: every field and the ordered object set must match.
    bool operator==(const CaptureIdentity& other) const noexcept {
        if (sceneGeneration != other.sceneGeneration ||
            sceneRevision != other.sceneRevision ||
            historyBinding != other.historyBinding ||
            historyRevision != other.historyRevision ||
            providerInstance != other.providerInstance ||
            effectRevision != other.effectRevision ||
            objectIds.size() != other.objectIds.size()) {
            return false;
        }
        for (std::size_t index = 0; index < objectIds.size(); ++index) {
            if (objectIds[index] != other.objectIds[index]) return false;
        }
        return true;
    }

    bool operator!=(const CaptureIdentity& other) const noexcept {
        return !(*this == other);
    }
};

enum class SaveKind { Manual, SaveAs, Incremental, Backup };

struct Request {
    std::uint64_t requestId = 0;
    CaptureIdentity identity;
    // Final targets, already resolved by the caller. This protocol performs no
    // file system normalization and no I/O.
    std::wstring pmmPath;
    std::wstring emmPath;
    SaveKind kind = SaveKind::Manual;
    bool saveEffects = false;
};

namespace detail {
inline const char* RequestError(const Request& request) noexcept {
    if (!request.requestId) return "effect_save: requestId must be non-zero";
    if (!request.identity.sceneGeneration) return "effect_save: sceneGeneration must be non-zero";
    if (!request.identity.providerInstance) return "effect_save: providerInstance must be non-zero";
    if (!request.saveEffects) return "effect_save: tracker requires saveEffects";
    switch (request.kind) {
    case SaveKind::Manual:
    case SaveKind::SaveAs:
    case SaveKind::Incremental:
    case SaveKind::Backup:
        break;
    default:
        return "effect_save: unknown save kind";
    }
    const auto pathValid = [](const std::wstring& path) {
        return !path.empty() && path.find(L'\0') == std::wstring::npos;
    };
    if (!pathValid(request.pmmPath)) return "effect_save: pmmPath must be resolved, non-empty and NUL-free";
    if (!pathValid(request.emmPath)) return "effect_save: emmPath must be resolved, non-empty and NUL-free";
    return nullptr;
}
} // namespace detail

enum class Stage {
    Ready,
    Writing,
    AwaitingAcknowledgement,
    Acknowledging,
    NotCommitted,
    CancelledBeforeWrite,
    CommittedAcknowledgementFailed,
    CommittedStaleScene,
    CommittedProviderRetired,
    Completed,
};

// Main-thread only. Non-copyable and non-movable, with an immutable request.
// It tracks completion of an already sealed request; it does not prove the
// captured bytes were frozen or legal.
class CompletionTracker {
public:
    // Throws std::invalid_argument when the request cannot be tracked: zero
    // requestId, zero sceneGeneration or providerInstance, saveEffects off, an
    // unknown save kind, or a final path that is empty or contains an embedded
    // NUL.
    explicit CompletionTracker(Request request) : request_(std::move(request)) {
        if (const auto* error = detail::RequestError(request_)) throw std::invalid_argument(error);
    }

    ~CompletionTracker() = default;

    CompletionTracker(const CompletionTracker&) = delete;
    CompletionTracker& operator=(const CompletionTracker&) = delete;
    CompletionTracker(CompletionTracker&&) = delete;
    CompletionTracker& operator=(CompletionTracker&&) = delete;

    const Request& GetRequest() const noexcept { return request_; }
    Stage GetStage() const noexcept { return stage_; }

    // Disk commit fact, independent of provider acknowledgement. It stays true
    // in every committed stage, including failed, stale and retired outcomes.
    bool DiskCommitted() const noexcept {
        switch (stage_) {
        case Stage::AwaitingAcknowledgement:
        case Stage::Acknowledging:
        case Stage::CommittedAcknowledgementFailed:
        case Stage::CommittedStaleScene:
        case Stage::CommittedProviderRetired:
        case Stage::Completed:
            return true;
        default:
            return false;
        }
    }

    // Consumers apply completion side effects at most once, and only when
    // FinishAcknowledgement returned true with Stage::Completed. A false result
    // and a duplicate or wrong-request receipt must not apply them again. Each
    // side effect still rechecks the caller's current identity through the
    // queries below; there is no separate consumption state.
    //
    // Every authorization rechecks the caller's current identity against the
    // capture; an old authorization is never cached as a permanent bool.
    //
    // Path completion needs only the scene generation, history binding and
    // provider instance to remain attached while the provider is available.
    // Revisions and object edits made during saving keep the disk commit and
    // the path result; this one still requires Completed. A backup never
    // changes the current project path.
    bool AllowsApplySceneCompletion(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return stage_ == Stage::Completed && request_.kind != SaveKind::Backup &&
            SceneAttached(current) && ProviderAttached(current, providerAvailable);
    }

    // Dirty clearing and successful close require Completed and the full
    // capture identity, including every revision and the ordered object set.
    // Later edits must not clear dirty or report success, and a backup never
    // clears dirty or closes as a successful save.
    bool AllowsClearCapturedDirty(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return stage_ == Stage::Completed && request_.kind != SaveKind::Backup &&
            providerAvailable && current == request_.identity;
    }

    bool AllowsCloseAfterSave(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return AllowsClearCapturedDirty(current, providerAvailable);
    }

    // Every mutator is serialized by requestId. A different id, a repeated
    // event or an out-of-order event returns false without changing the stage.
    bool StartWrite(std::uint64_t requestId) noexcept {
        if (requestId != request_.requestId || stage_ != Stage::Ready) return false;
        stage_ = Stage::Writing;
        return true;
    }

    bool CancelBeforeWrite(std::uint64_t requestId) noexcept {
        if (requestId != request_.requestId || stage_ != Stage::Ready) return false;
        stage_ = Stage::CancelledBeforeWrite;
        return true;
    }

    // Only Writing may finish. Uncommitted writes end in NotCommitted. A
    // committed write keeps the committed fact even when the scene or provider
    // moved. Later sceneRevision/historyRevision/effectRevision/object edits do
    // not revoke the disk commit; sceneGeneration or historyBinding changes are
    // CommittedStaleScene and provider retirement is CommittedProviderRetired.
    bool FinishWrite(std::uint64_t requestId, bool committed,
        const CaptureIdentity& current, bool providerAvailable) noexcept {
        if (requestId != request_.requestId || stage_ != Stage::Writing) return false;
        if (!committed) {
            stage_ = Stage::NotCommitted;
            return true;
        }
        if (!SceneAttached(current)) {
            stage_ = Stage::CommittedStaleScene;
            return true;
        }
        if (!ProviderAttached(current, providerAvailable)) {
            stage_ = Stage::CommittedProviderRetired;
            return true;
        }
        stage_ = Stage::AwaitingAcknowledgement;
        return true;
    }

    // The caller runs the real provider callback only after this returns true.
    // A callback that re-enters with the same request sees Acknowledging and is
    // refused. When the scene or provider no longer belongs to the capture, the
    // committed fact is preserved as a stale/retired terminal stage and false
    // is returned without entering Acknowledging.
    bool BeginAcknowledgement(std::uint64_t requestId,
        const CaptureIdentity& current, bool providerAvailable) noexcept {
        if (requestId != request_.requestId || stage_ != Stage::AwaitingAcknowledgement) return false;
        if (!SceneAttached(current)) {
            stage_ = Stage::CommittedStaleScene;
            return false;
        }
        if (!ProviderAttached(current, providerAvailable)) {
            stage_ = Stage::CommittedProviderRetired;
            return false;
        }
        stage_ = Stage::Acknowledging;
        return true;
    }

    // Rechecks scene and provider after the callback, because the callback may
    // have replaced the scene or unloaded the provider. A throwing adapter must
    // call this with false; this protocol never invokes a callback itself. Only
    // a true result ending in Stage::Completed authorizes one application of the
    // completion side effects; false results authorize nothing.
    bool FinishAcknowledgement(std::uint64_t requestId, bool acknowledged,
        const CaptureIdentity& current, bool providerAvailable) noexcept {
        if (requestId != request_.requestId || stage_ != Stage::Acknowledging) return false;
        if (!SceneAttached(current)) {
            stage_ = Stage::CommittedStaleScene;
        } else if (!ProviderAttached(current, providerAvailable)) {
            stage_ = Stage::CommittedProviderRetired;
        } else if (!acknowledged) {
            stage_ = Stage::CommittedAcknowledgementFailed;
        } else {
            stage_ = Stage::Completed;
        }
        return true;
    }

private:
    // A scene generation or history binding change means the committed result
    // belongs to a scene that no longer exists.
    bool SceneAttached(const CaptureIdentity& current) const noexcept {
        return current.sceneGeneration == request_.identity.sceneGeneration &&
            current.historyBinding == request_.identity.historyBinding;
    }

    // Availability plus instance equality keeps a retired instance's result
    // from passing to a newly loaded provider at the same paths.
    bool ProviderAttached(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return providerAvailable && current.providerInstance == request_.identity.providerInstance;
    }

    const Request request_;
    Stage stage_ = Stage::Ready;
};

}
