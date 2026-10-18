#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/clipboard_layout.hpp"
#include <vector>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
struct ModelBonePasteRecord {
    mdl::BoneClipboardRecord key{};
    bone_scale::Matrix scale{};
    bone_scale::Curve scaleCurve=bone_scale::LinearCurve();
    // Resolved -1 means skip this source; false uses the encoded key name.
    bool targetResolved=false;
    int targetIndex=-1;
};
struct ModelMorphPasteRecord {
    mdl::MorphClipboardRecord key{};
    bool targetResolved=false;
    int targetIndex=-1;
};
struct ModelDisplayPasteRecord {
    std::uint32_t frame=0;
    std::uint8_t visible=1;
    std::vector<mdl::IkClipboardState> ik;
    std::vector<mdl::SelectorClipboardState> selectors;
    // When resolved, each source IK entry names a destination bone index.
    // -1 skips that entry. This never installs importedIkTargets thread state.
    bool ikTargetsResolved=false;
    std::vector<int> ikTargets;
};
struct ModelPasteSource {
    std::vector<ModelBonePasteRecord> bones;
    std::vector<ModelMorphPasteRecord> morphs;
    std::vector<ModelDisplayPasteRecord> displays;
    std::uint32_t frameOffset=0;
    // VMD import advances the shared registration extent; ordinary paste does
    // not. Both values remain part of the same metadata history participant.
    bool advanceLastRegisteredFrame=false;
    // Early VMD writers can end before the optional display section. Their
    // final allocation cursor belongs to Morph rather than Display.
    bool resetDisplayCursor=true;
    // Optional model VMD author blocks, kept byte-exact after envelope checks.
    // Families consume them against the SAME private pool as standard keys.
    std::vector<std::uint8_t> boneCurveBlock,morphCurveBlock;
    bool curveTargetsResolved=false;
    // Unicode dictionary owner index -> destination index. Resolved empty
    // tables are intentional and must never fall back to CP932 matching.
    std::vector<int> boneCurveTargets,morphCurveTargets;
};
enum ModelPasteFamilyMask : unsigned {ModelPasteBones=1,ModelPasteMorphs=2,ModelPasteDisplays=4,ModelPasteAll=7};
// Internal snapshot helpers; family wrappers capture only their own source so
// an unused malformed clipboard family cannot change their legacy admission.
MmsResult CaptureModelPasteSource(MMDApp&,ModelPasteSource&,unsigned families=ModelPasteAll) noexcept;
MmsResult ValidateModelPasteSource(const ModelPasteSource&,unsigned families=ModelPasteAll) noexcept;
// Family output for one model clipboard aggregate. The participant owns only
// its key pool, authoring and derived values; the aggregate owns all marks,
// cursor, maximum frame and last-registration metadata.
struct ModelPasteFamilyResult {
    std::shared_ptr<HistoryChange> change;
    std::uint32_t maxFrame = 0;
    int searchCursor = 0;
    bool hasKeys = false;
    bool dataChanged = false;
    std::vector<unsigned> markedSlots;
    bool overridesSearchCursor = false;
    bool curveBlockSkipped = false;
    // The legacy loader advances the registration extent BEFORE author tails.
    // A tail may alter model.maxFrame without advancing that earlier marker.
    bool hasRegistrationExtent = false;
    std::uint32_t registrationMaxFrame = 0;
};
struct ModelPasteReport { bool boneCurvesSkipped=false; };
MmsResult PlanModelKeyPaste(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>&) noexcept;
MmsResult PlanModelKeyPaste(MMDApp&,unsigned modelSlot,const ModelPasteSource&,
    std::shared_ptr<HistoryChange>&,ModelPasteReport* = nullptr) noexcept;
}
#endif
