#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/build_profile.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#include "pmm_read_buffer.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/skinning_menu.hpp"
#include "mikumikustudio/view_options.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <algorithm>
#include <utility>

namespace mikumikustudio::pmm_settings {
namespace {
// Precision is one stable byte: Low=0 .. Off=4. Scope is a separate boolean
// record. Unknown bytes stay untouched until that setting is explicitly edited.
static_assert(static_cast<std::uint8_t>(TransparentSortPrecision::Low) == 0 &&
                  static_cast<std::uint8_t>(TransparentSortPrecision::Standard) == 1 &&
                  static_cast<std::uint8_t>(TransparentSortPrecision::High) == 2 &&
                  static_cast<std::uint8_t>(TransparentSortPrecision::Maximum) == 3 &&
                  static_cast<std::uint8_t>(TransparentSortPrecision::Off) == 4,
              "PMM v2 transparent_sort_precision bytes are stable");
constexpr std::uint8_t kHighestTransparentSortTier =
    static_cast<std::uint8_t>(TransparentSortPrecision::Off);

// A tier record is usable when it is one byte inside the enum range.  Anything
// else is a value from another writer or a damaged record: the running state
// falls back to Off and the raw bytes are kept for the next save.
bool KnownTier(const pmm_extension::Record* record) {
    return record != nullptr && record->value.size() == 1 &&
           record->value[0] <= kHighestTransparentSortTier;
}

bool KnownAntialias(const pmm_extension::Record* record) {
    return record && record->value.size() == 3 && record->value[0] <= 1 &&
        record->value[1] < static_cast<unsigned>(antialias::Algorithm::Count) &&
        record->value[2] >= static_cast<unsigned>(antialias::Algorithm::Smaa) &&
        record->value[2] < static_cast<unsigned>(antialias::Algorithm::Count);
}
std::vector<std::uint8_t> AntialiasBytes(const MMDApp& app) {
    return {std::uint8_t(app.antialiasing.enabled),
        std::uint8_t(app.antialiasing.algorithm),
        std::uint8_t(app.antialiasing.lastSmaaAlgorithm)};
}

void Refresh(MMDApp& app) {
    if (!app.MainWindow()) return;
    // Refresh both the nested strength selection and the independent scope.
    RefreshTransparentSortPrecisionMenu(&app);
    RefreshSkinningMenu(&app);
    antialias::RefreshMenu(&app);
    InvalidateRect(app.MainWindow(), nullptr, FALSE);
}
}

Processor ClassifyProcessor(const skinning::Preference& preference, const std::vector<gpu::Adapter>& adapters) {
    if (preference.mode == skinning::Mode::Cpu) return Processor::Cpu;
    if (preference.mode != skinning::Mode::Adapter) return Processor::Automatic;
    for (const auto& adapter : adapters)
        if (adapter.id == preference.adapterId)
            return adapter.integrated ? Processor::Integrated : Processor::Discrete;
    return Processor::Automatic;
}

skinning::Preference ResolveProcessor(Processor processor, const std::vector<gpu::Adapter>& adapters) {
    if (processor == Processor::Automatic) return {};
    if (processor == Processor::Cpu) return {skinning::Mode::Cpu};
    const bool integrated = processor == Processor::Integrated;
    for (const auto& adapter : adapters) {
        if (adapter.integrated != integrated) continue;
        skinning::Preference preference{skinning::Mode::Adapter, adapter.id, adapter.name};
        preference.adapterClass = integrated ? skinning::AdapterClass::Integrated : skinning::AdapterClass::Discrete;
        return preference;
    }
    // Keep the requested type in State::document. A temporary automatic fallback must
    // not replace it when the project is saved on another computer.
    return {};
}

void Reset(MMDApp& app) {
    negative_frames::Reset(&app);
#if !MMD_ORIGINAL_PROFILE
    curve_host::Reset(&app);
    morph_curve::Reset(&app);
    bone_curve::Reset();
#endif
    app.PmmProjectSettings() = {};
    app.TransparentMaterialSortPrecision() = kDefaultTransparentSortPrecision;
    app.TransparentSortCrossObjects() = false;
    // Processor settings are per machine user, not per project: creating a new
    // scene never resets them (docs/records/performance/devices.md).
#if !MMD_ORIGINAL_PROFILE
    antialias::ReloadUserSettings(&app);
    bone_scale_io::ResetPmm();
#endif
    Refresh(app);
}

void Load(MMDApp& app, int fd, const std::vector<int>& adaptedModelSlots) {
    negative_frames::Reset(&app);
#if MMD_ORIGINAL_PROFILE
    // Original MMD stops at the standard body. Do not parse, retain or apply
    // even unknown extension records from a full-profile project.
    (void)fd;
    app.PmmProjectSettings() = {};
    app.TransparentMaterialSortPrecision() = kDefaultTransparentSortPrecision;
    app.TransparentSortCrossObjects() = false;
#else
    curve_host::Reset(&app);
    morph_curve::Reset(&app);
    bone_curve::Reset();
    // Every model slot is loaded anew with the standard PMM data, so the
    // edit baselines of the previous project must not survive this call.
    bone_scale_io::ResetPmm();
    pmm_extension::Document document;
    std::uint64_t standardSize = 0;
    // The standard body is read through the bounded read-ahead window, so the
    // CRT descriptor sits past the consumed byte.  Flush it to the logical
    // position and drop the window before the trailer reader seeks the file
    // end and back: its standard-data boundary must be the consumed position,
    // not the read-ahead position.  A failed sync refuses the handoff instead
    // of letting the trailer reader work from the read-ahead position.
    if (!pmm_io::SyncExternalPosition(fd)) {
        runtime_log::Write("PMM extension ignored: standard-body position cannot be synchronised");
        return;
    }
    const auto consumed = pmm_io::Position(fd);
    const auto result = pmm_extension::Read(fd, document, standardSize);
    if (result != pmm_extension::ReadResult::Loaded || consumed < 0 ||
        standardSize != static_cast<std::uint64_t>(consumed)) {
        if (result != pmm_extension::ReadResult::Absent)
            runtime_log::Write("PMM extension ignored: invalid footer, checksum or standard-data boundary");
        return;
    }
    app.PmmProjectSettings().document = std::move(document);
    const auto& loaded = app.PmmProjectSettings().document;
    if (const auto* aa = pmm_extension::Find(loaded, kAntialiasKey); KnownAntialias(aa)) {
        antialias::ApplyProjectSettings(&app, aa->value[0] != 0,
            static_cast<antialias::Algorithm>(aa->value[1]),
            static_cast<antialias::Algorithm>(aa->value[2]));
    }
    // The old processor record (kProcessorKey) is never applied: the processor
    // choice lives in the per-user processor.ini and opening a project must
    // not change it.  An old record stays untouched in the document until the
    // next save removes it.
    // The tier field stores strength, Off included; scope has its own record.  A file
    // without the field runs Off; a file whose byte this reader does not
    // know also runs Off, and that raw byte stays in the document.  The
    // retired toggle key is never looked up: it is an ordinary unknown record
    // that is carried over untouched and never affects the running tier.
    const auto* tier = pmm_extension::Find(loaded, kSortingPrecisionKey);
    app.TransparentMaterialSortPrecision() = KnownTier(tier)
        ? static_cast<TransparentSortPrecision>(tier->value[0])
        : kDefaultTransparentSortPrecision;
    const auto* cross = pmm_extension::Find(loaded, kSortingCrossObjectsKey);
    app.TransparentSortCrossObjects() = cross && cross->value == std::vector<std::uint8_t>{1};
    // Bone scaling rides the same document
    // ("org.mikudancestudio.model.<slot>.boneScale.v1"); it is applied after
    // the document itself, on the models already loaded.
    extended_names::LoadPmm(app, loaded, adaptedModelSlots);
    bone_scale_io::LoadPmm(app, loaded);
    curve_host::Load(app, loaded);
    // Model morph curves ride the same document
    // ("org.mikudancestudio.model.<slot>.morphCurve.v1"), on the models the
    // standard body already loaded.  The exact curves take over the native
    // linear interpolation from the next frame seek on.
    morph_curve::LoadPmm(app, loaded);
    bone_curve::LoadPmm(app, loaded);
    negative_frames::Load(app, loaded);
    std::string meshError;
    if(!mesh_bake_cache::Load(app,app.EnvFileName(),loaded,meshError)){
        runtime_log::Write("Mesh cache load failed: %s",meshError.c_str());
        LegacyUiMessageBoxA(app.MainWindow(),meshError.c_str(),"Mesh cache / PMM",MB_OK|MB_ICONWARNING);
    }
    Refresh(app);
#endif
}

pmm_extension::Document Capture(const MMDApp& app) {
#if MMD_ORIGINAL_PROFILE
    (void)app;
    return {};
#else
    const auto& state = app.PmmProjectSettings();
    auto document = state.document;
    // The processor choice is per machine user (processor.ini), never project
    // data: the old record is removed on save and nothing else changes.
    // Unchanged known fields also retain their raw representation: a future
    // value and an unavailable GPU type must survive an older reader's save.
    pmm_extension::Erase(document, kProcessorKey);
    // An unsupported device can temporarily use a spatial fallback. Preserve
    // the project's requested algorithm (including future values) until the
    // user explicitly chooses a setting on this machine.
    if (state.antialiasEdited || !pmm_extension::Find(document, kAntialiasKey))
        pmm_extension::Set(document, kAntialiasKey, AntialiasBytes(app));

    // Only the selected tier byte is written.  The per-frame layer count of that
    // tier is not stored anywhere and Off is stored as the Off byte, so no
    // earlier tier is remembered.
    const auto selected = static_cast<std::uint8_t>(app.TransparentMaterialSortPrecision());
    const auto* tier = pmm_extension::Find(document, kSortingPrecisionKey);
    if (state.sortingPrecisionEdited) {
        // The user picked a tier, so this field is authoritative now.
        pmm_extension::Set(document, kSortingPrecisionKey, {selected});
    } else if (tier == nullptr) {
        // Project without the field: start it at the running tier, which Load
        // left at Off.  A retired toggle record elsewhere in the document
        // is not a tier statement and is left alone.
        pmm_extension::Set(document, kSortingPrecisionKey, {selected});
    } else if (KnownTier(tier) && tier->value[0] != selected) {
        pmm_extension::Set(document, kSortingPrecisionKey, {selected});
    }
    // A tier byte this reader does not know is carried over exactly as read,
    // next to the retired toggle record, until the user picks a tier.
    // Per-model bone scaling is appended last so the slot walk below sees the
    // final state of every model slot.
    const auto* cross = pmm_extension::Find(document, kSortingCrossObjectsKey);
    if (state.sortingCrossObjectsEdited || !cross ||
        (cross->value.size() == 1 && cross->value[0] <= 1))
        pmm_extension::Set(document, kSortingCrossObjectsKey, {std::uint8_t(app.TransparentSortCrossObjects())});
    extended_names::CapturePmm(app, document);
    bone_scale_io::CapturePmm(app, document);
    morph_curve::CapturePmm(app, document);
    bone_curve::CapturePmm(app, document);
    negative_frames::Capture(app, document);
    curve_host::Capture(const_cast<MMDApp&>(app), document);
    return document;
#endif
}

void ProcessorChanged(MMDApp& app) {
    app.PmmProjectSettings().processorEdited = true;
    app.SceneModified() = 1;
}
void SortingPrecisionChanged(MMDApp& app) {
    app.PmmProjectSettings().sortingPrecisionEdited = true;
    app.SceneModified() = 1;
}
void SortingCrossObjectsChanged(MMDApp& app) {
    app.PmmProjectSettings().sortingCrossObjectsEdited = true;
    app.SceneModified() = 1;
}
void AntialiasChanged(MMDApp& app) {
    const auto* record = pmm_extension::Find(app.PmmProjectSettings().document, kAntialiasKey);
    if (record && record->value == AntialiasBytes(app)) return;
    app.PmmProjectSettings().antialiasEdited = true;
    app.SceneModified() = 1;
}
}
