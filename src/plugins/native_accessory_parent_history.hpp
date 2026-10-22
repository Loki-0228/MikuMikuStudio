#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <Windows.h>
#include <cstdint>
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct AccessoryAttachmentValue {
    std::int32_t model = -1, bone = 0;
    bool operator==(const AccessoryAttachmentValue& other) const noexcept { return model == other.model && bone == other.bone; }
};
// One private source/provenance handle shared by the three migrated accessory
// participants. Receipts contain values/identities only; active receipt storage
// is bounded to 255 slots. Node-owned handles and retained owner bookkeeping
// are charged by Prepared::MeasuredBytes, never omitted from history admission.
class AccessoryAttachmentHistory {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    class Prepared {
        struct State;
        std::unique_ptr<State> state_;
        explicit Prepared(std::unique_ptr<State>);
        friend class AccessoryAttachmentHistory;
    public:
        ~Prepared();
        bool IsCurrent() const noexcept;
        void Publish() noexcept;
        std::uint64_t MeasuredBytes() const noexcept;
    };
    MmsResult Begin(MMDApp&, SharedHistory&, unsigned slot) noexcept;
    AccessoryAttachmentValue Logical() const noexcept;
    bool Matches(AccessoryAttachmentValue expected) const noexcept;
    // Captures actual+target+logical parent owners, source generations and
    // receipt serial before any publication. Destruction is cancellation.
    MmsResult Prepare(AccessoryAttachmentValue expected, AccessoryAttachmentValue target,
        AccessoryAttachmentValue logical, std::unique_ptr<Prepared>&) const noexcept;
};
// Trusted native evaluation captures provenance before writing. Complete must
// receive the exact pair computed by that evaluator; it never grants trust to
// an arbitrary post-callback live value and never installs a missing receipt.
class AccessoryAttachmentEvaluation {
    MMDApp* app_ = nullptr;
    SharedHistory* history_ = nullptr;
    std::shared_ptr<NativeHistoryDomain> domain_;
    unsigned slot_ = 0;
    void* object_ = nullptr;
    void* cell_ = nullptr;
    MmsId id_{};
    std::uint64_t parentGeneration_ = 0;
    std::uint64_t scene_ = 0, pool_ = 0, historyEpoch_ = 0, domainEpoch_ = 0;
    std::uint64_t serial_ = 0, slotSerial_ = 0, objectGeneration_ = 0, bindingGeneration_ = 0;
    std::uint64_t revision_ = 0;
    bool valid_ = true;
    bool observerOnly_ = true;
    AccessoryAttachmentValue before_{};
public:
    AccessoryAttachmentEvaluation(MMDApp*, unsigned slot) noexcept;
    // Validate before applying a sampled pose as callbacks may replace the
    // scene/history, even if no receipt existed when sampling began.
    bool CanApply() const noexcept;
    void Complete(AccessoryAttachmentValue computed) noexcept;
};
// Directional metadata belonging to the real model-removal participant.
// Model/object resource and presence proofs remain in that outer participant;
// restoring an absent parent is never treated as already-present here.
class AccessoryAttachmentRemoval {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    class Prepared {
        struct State;
        std::unique_ptr<State> state_;
        explicit Prepared(std::unique_ptr<State>);
        friend class AccessoryAttachmentRemoval;
    public:
        ~Prepared();
        bool IsCurrent() const noexcept;
        // Caller validates the entire binding/presence aggregate first. This
        // metadata publishes before raw patches and cannot recheck old fields.
        void Publish() noexcept;
        std::uint64_t MeasuredBytes() const noexcept;
    };
    MmsResult Capture(MMDApp&,const std::vector<unsigned>& models,const std::vector<unsigned>& accessories) noexcept;
    bool Affects(unsigned slot) const noexcept;
    bool OwnsPair(unsigned slot) const noexcept;
    MmsResult Prepare(bool forward,std::unique_ptr<Prepared>&) const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
};
// Private authoring routes. Both fields belong to one history participant;
// model selection resets the stored bone to zero, as native control 474 does.
MmsResult PlanAccessoryParentModel(MMDApp&, std::int32_t, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult PlanAccessoryParentBone(MMDApp&, std::int32_t, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult ApplyBoundAccessoryParentModel(MMDApp&, std::int32_t, bool& changed) noexcept;
MmsResult ApplyBoundAccessoryParentBone(MMDApp&, std::int32_t, bool& changed) noexcept;
// Bound notifications are consumed on refusal too. Never fall back to an
// unjournalled legacy write after stale, busy, allocation or budget failure.
bool TrySelectBoundAccessoryParentModel(MMDApp*, HWND) noexcept;
bool TrySelectBoundAccessoryParentBone(MMDApp*, HWND) noexcept;
// Readout of these two combos only. Numeric EDIT drafts are never touched.
void RefreshBoundAccessoryParentReadout(MMDApp&) noexcept;
}
#endif
