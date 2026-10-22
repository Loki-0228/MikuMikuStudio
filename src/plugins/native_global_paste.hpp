#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/clipboard_layout.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Internal participants never own the shared selection snapshot or last frame.
// The aggregate validates every participant before publishing any of them.
class PreparedGlobalPastePart {
public:
    virtual ~PreparedGlobalPastePart() = default;
    virtual bool IsCurrent() const noexcept = 0;
    virtual void PublishOwned() noexcept = 0;
    // Called after all pool growth and GlobalKeySelection::Publish. Restore
    // only this participant's pasted/linked slots, preserving the snapshot's
    // restored marks on undo within the originally captured pool extent.
    virtual void PublishSelection() noexcept = 0;
    virtual bool TargetRetainedBytes(std::uint64_t&) const noexcept { return false; }
};
class GlobalPastePart {
public:
    virtual ~GlobalPastePart() = default;
    virtual MmsResult Prepare(bool forward, std::unique_ptr<PreparedGlobalPastePart>&) = 0;
    virtual MmsResult PrepareInitial(SharedHistory&, bool, std::unique_ptr<PreparedGlobalPastePart>& out) { return Prepare(true, out); }
    virtual std::uint64_t RetainedBytes() const noexcept = 0;
};
struct GlobalPasteSource {
    std::vector<mdl::CameraClipboardRecord> cameras;
    std::vector<mdl::LightClipboardRecord> lights;
    std::vector<mdl::ShadowClipboardRecord> shadows;
    std::vector<mdl::GravityClipboardRecord> gravity;
    std::vector<mdl::AccessoryClipboardKey> accessories;
    std::uint32_t frameOffset = 0;
};
struct GlobalPasteFamilyResult {
    std::shared_ptr<GlobalPastePart> part;
    std::uint32_t maximumInsertedFrame = 0;
    bool dataChanged = false;
};
MmsResult PlanCameraKeyPastePart(MMDApp&, const GlobalPasteSource&, GlobalPasteFamilyResult&) noexcept;
MmsResult PlanLightKeyPastePart(MMDApp&, const GlobalPasteSource&, GlobalPasteFamilyResult&) noexcept;
MmsResult PlanShadowKeyPastePart(MMDApp&, const GlobalPasteSource&, GlobalPasteFamilyResult&) noexcept;
MmsResult PlanGravityKeyPastePart(MMDApp&, const GlobalPasteSource&, GlobalPasteFamilyResult&) noexcept;
MmsResult PlanAccessoryKeyPastePart(MMDApp&, unsigned slot, const GlobalPasteSource&, GlobalPasteFamilyResult&) noexcept;
MmsResult PlanGlobalKeyPaste(MMDApp&, std::shared_ptr<HistoryChange>&) noexcept;
// Immutable decoded source, independent of application clipboard storage.
// frameOffset locates source keys; planning and first preparation retain the
// current real playhead, which is never moved to obtain the offset.
MmsResult PlanGlobalKeyPaste(MMDApp&, const GlobalPasteSource&,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
