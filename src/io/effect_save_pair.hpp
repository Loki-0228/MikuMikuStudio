#pragma once

#include "effect_save_capture.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace mikumikustudio::effect_save {

struct ArtifactFingerprint {
    std::uint64_t size = 0;
    EffectDigest sha256{};

    bool operator==(const ArtifactFingerprint& other) const noexcept {
        return size == other.size && sha256 == other.sha256;
    }
};

struct SidecarFingerprint {
    std::wstring path;
    ArtifactFingerprint file;

    bool operator==(const SidecarFingerprint& other) const noexcept {
        return path == other.path && file == other.file;
    }
};

// Trusted host evidence for the closed PMM stage and any plugin sidecar the
// save profile includes. The request must be the one bound to those exact
// captured stages, not an identity re-read after staging. The host must
// resolve paths and rule out filesystem aliases before using this protocol.
struct StagedProjectEvidence {
    Request request;
    ArtifactFingerprint pmm;
    std::optional<SidecarFingerprint> sidecar;
};

namespace detail {
inline bool SameRequest(const Request& left, const Request& right) noexcept {
    return left.requestId == right.requestId && left.identity == right.identity &&
        left.pmmPath == right.pmmPath && left.emmPath == right.emmPath &&
        left.kind == right.kind && left.saveEffects == right.saveEffects;
}
} // namespace detail

// Worker/receipt value data only: no callable or live scene/provider pointer.
struct PairSeal {
    Request request;
    ArtifactFingerprint pmm;
    ArtifactFingerprint emm;
    std::optional<SidecarFingerprint> sidecar;

    bool operator==(const PairSeal& other) const noexcept {
        return detail::SameRequest(request, other.request) && pmm == other.pmm &&
            emm == other.emm && sidecar == other.sidecar;
    }
};

enum class PublicationStatus { NotCommitted, Committed, NeedsRecovery };
struct PublicationReceipt {
    // Only the trusted writer may report Committed, after verifying every
    // required final member against the staged bytes. A PMM-only success, a
    // return from a legacy save, or echoing the input seal is not evidence.
    // Report NotCommitted only when the prior destination state is known to
    // be intact. Report NeedsRecovery after any uncertain/partial outcome.
    PublicationStatus status = PublicationStatus::NeedsRecovery;
    PairSeal seal;
};

struct LiveSaveState {
    CaptureIdentity identity;
    bool providerAvailable = false;
};

struct AcknowledgementCallbacks {
    // Only the provider that accepts publication of its frozen snapshot may
    // return a positive receipt. Never synthesize one from a void notification.
    // This is synchronous; the provider may not retain the passed seal view.
    std::function<std::optional<PairSeal>(const PairSeal&)> provider;
    // Host: read the current attachment and full identity after the callback.
    std::function<LiveSaveState()> observe;
};

// Main-thread completion for one same-snapshot publication attempt. Reuse the
// existing completion stages and authorization rules, but never pass them a
// success without both the host's write receipt and the provider's own receipt.
// Use a fresh, never-reused request id and keep this attempt alive through the
// callback. This class does no I/O, EMM parsing, or interruption recovery.
class PairTransaction final {
public:
    PairTransaction(const CapturedEffects& effects, const StagedProjectEvidence& project)
        : seal_(MakeSeal(effects, project)), completion_(seal_.request) {}

    const PairSeal& GetSeal() const noexcept { return seal_; }
    Stage GetStage() const noexcept { return completion_.GetStage(); }
    bool DiskCommitted() const noexcept { return completion_.DiskCommitted(); }

    bool StartWrite(std::uint64_t id) noexcept { return completion_.StartWrite(id); }
    bool CancelBeforeWrite(std::uint64_t id) noexcept { return completion_.CancelBeforeWrite(id); }

    bool FinishWrite(const PublicationReceipt& receipt, const CaptureIdentity& current,
        bool providerAvailable) noexcept {
        const auto id = receipt.seal.request.requestId;
        if (id != seal_.request.requestId) return false;
        // Same id with contradictory/malformed evidence is not a safe abort.
        if (!(receipt.seal == seal_)) return completion_.FinishWriteUncertain(id);
        switch (receipt.status) {
        case PublicationStatus::NotCommitted:
            return completion_.FinishWrite(id, false, current, providerAvailable);
        case PublicationStatus::Committed:
            return completion_.FinishWrite(id, true, current, providerAvailable);
        default:
            return completion_.FinishWriteUncertain(id);
        }
    }

    // `before` must be freshly observed. The tracker gates entry before any
    // callbacks can re-enter. Reobserve after the provider even if it throws.
    // Apply side effects only once, after true with Completed, and recheck
    // live identity at application time.
    bool Acknowledge(std::uint64_t id, const LiveSaveState& before,
        const AcknowledgementCallbacks& callbacks) noexcept {
        if (!completion_.BeginAcknowledgement(id, before.identity, before.providerAvailable)) return false;
        if (!callbacks.observe) return completion_.FailUnverifiedAcknowledgement(id);
        bool acknowledged = false;
        try {
            if (callbacks.provider) {
                const auto receipt = callbacks.provider(seal_);
                acknowledged = receipt && *receipt == seal_;
            }
        } catch (...) { /* No positive receipt. */ }
        try {
            const auto current = callbacks.observe();
            return completion_.FinishAcknowledgement(id, acknowledged, current.identity, current.providerAvailable);
        } catch (...) {
            return completion_.FailUnverifiedAcknowledgement(id);
        }
    }

    bool AllowsApplySceneCompletion(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return completion_.AllowsApplySceneCompletion(current, providerAvailable);
    }
    bool AllowsClearCapturedDirty(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return completion_.AllowsClearCapturedDirty(current, providerAvailable);
    }
    bool AllowsCloseAfterSave(const CaptureIdentity& current, bool providerAvailable) const noexcept {
        return completion_.AllowsCloseAfterSave(current, providerAvailable);
    }

private:
    static PairSeal MakeSeal(const CapturedEffects& effects, const StagedProjectEvidence& project) {
        const auto& request = effects.GetRequest();
        if (!detail::SameRequest(project.request, request) || !project.pmm.size)
            throw std::invalid_argument("effect_save: staged PMM does not match captured request");
        if (project.sidecar) {
            const auto& sidecar = *project.sidecar;
            if (!sidecar.file.size || sidecar.path.empty() || sidecar.path.find(L'\0') != std::wstring::npos ||
                sidecar.path == request.pmmPath || sidecar.path == request.emmPath)
                throw std::invalid_argument("effect_save: invalid staged sidecar evidence");
        }
        return {request, project.pmm, {effects.Bytes().size, effects.Digest()}, project.sidecar};
    }

    const PairSeal seal_;
    CompletionTracker completion_;
};

} // namespace mikumikustudio::effect_save
