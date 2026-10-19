// MAINTENANCE: regression-keep; MMS native expression (morph) curve host entry
// (include/mikudancestudio/animation_curve_host.hpp,
//  src/window/animation_curve_host_ui.cpp).
//
// Covered here, on the CPU, with hidden windows only and without a renderer or
// a GPU:
//   * the channel build through the frozen morph_curve MMDApp overloads: only
//     the model's four user panels are offered, so a panel-0 system morph can
//     never reach the editor, and every channel takes its group from
//     morph_curve::PanelGroup,
//   * ChangedMorphTracks: an untouched list yields no track at all, one edited
//     channel yields exactly that one, so looking at the window can never
//     register every native curve as authored,
//   * the identity gate: a deleted or no-longer-selected model is refused with
//     a three-language message and nothing is written, and a different model at
//     the captured address is refused too - the morph table rewritten in place,
//     a changed panel, a changed resolved path, a changed bone count and a
//     swapped morph table are each caught, which a bare pointer comparison
//     would all pass,
//   * the comparison mode: the INI round-trip, the same-group default for an
//     absent or out-of-range value, and a refused write that keeps the stored
//     value,
//   * the lane selection: MorphLaneInitial answers only for a selector that
//     names a morph of that very panel, and rejects an unset, out-of-range or
//     foreign selection, so the launcher is disabled and OpenMorph refuses
//     instead of falling back to the first channel,
//   * the four launcher buttons: creation during WM_CREATE, the 22 px launcher
//     plus the 70 px selector with both arrows unmoved, idempotence over
//     repeated relayout, the model-mode expression-band visibility rule, the
//     per-lane enable rule and the three-language caption,
//   * OpenMorph builds its hidden window without activating the host.
//
// The baked-morph editing regression additionally supplies full key pools,
// a bone and a display track. It drives the curve bake button and the channel
// bake transaction, then verifies that mouse moves preserve manual bone edits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/channel_bake.hpp"
#include "mikudancestudio/bone_scale.hpp"

namespace mc = mikudancestudio::morph_curve;
namespace ac = mikudancestudio::animation_curve;
namespace mdl = mikudancestudio::mdl;
namespace ch = mikudancestudio::curve_host;
namespace ce = mikudancestudio::curve_editor;

using mikudancestudio::MMDApp;
using mikudancestudio::curve_editor::Channel;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

// ---------------------------------------------------------------------------
// A model block with a morph table and a morph key pool, and nothing else: the
// host bridge only ever looks at those two.
// ---------------------------------------------------------------------------

struct FakeModel {
    std::unique_ptr<mdl::ModelRecord> storage;
    std::vector<mdl::MorphRecord> morphs;
    std::vector<mdl::MorphKey> keys;

    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(storage.get()); }
    mdl::ModelRecord* record() const { return storage.get(); }
};

void SetName(mdl::MorphRecord& morph, const std::string& jp, const std::string& en) {
    std::memcpy(morph.name, jp.data(), jp.size() < 20 ? jp.size() : 20);
    std::memcpy(morph.nameEn, en.data(), en.size() < 20 ? en.size() : 20);
}

FakeModel MakeModel(const std::vector<std::pair<std::string, std::uint8_t>>& definitions,
                    const std::string& modelName) {
    FakeModel model;
    model.storage = std::make_unique<mdl::ModelRecord>();
    model.morphs.assign(definitions.size(), mdl::MorphRecord{});
    model.keys.assign(mdl::kMorphKeyCapacity, mdl::MorphKey{});
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        mdl::MorphRecord& morph = model.morphs[index];
        morph.type = 1;  // vertex morph: the bridge never inspects the type
        morph.panel = static_cast<mdl::MorphPanel>(definitions[index].second);
        SetName(morph, definitions[index].first, definitions[index].first);
    }
    std::memcpy(model.storage->name, modelName.data(),
                modelName.size() < 20 ? modelName.size() : 20);
    model.storage->morphCount = static_cast<std::uint32_t>(definitions.size());
    model.storage->morphs = model.morphs.data();
    model.storage->morphKeys = model.keys.data();
    return model;
}

int AllocSlot(FakeModel& model) {
    for (int index = static_cast<int>(model.storage->morphCount);
         index < static_cast<int>(mdl::kMorphKeyCapacity); ++index) {
        if (model.keys[static_cast<std::size_t>(index)].frame == 0) return index;
    }
    return -1;
}

// Mimics the registrar: ascending chain, allocated marks, max frame bump.
void NativeAdd(FakeModel& model, int morph, int frame, float value) {
    mdl::MorphKey* keys = model.keys.data();
    int current = morph;
    while (keys[current].next != 0 && static_cast<int>(keys[keys[current].next].frame) < frame) {
        current = static_cast<int>(keys[current].next);
    }
    const int next = static_cast<int>(keys[current].next);
    if (next != 0 && static_cast<int>(keys[next].frame) == frame) {
        keys[next].value = value;
        keys[next].allocated = 1;
    } else {
        const int fresh = AllocSlot(model);
        if (fresh < 0) std::abort();
        keys[fresh].frame = static_cast<std::uint32_t>(frame);
        keys[fresh].value = value;
        keys[fresh].allocated = 1;
        keys[fresh].previous = static_cast<std::uint32_t>(current);
        keys[fresh].next = static_cast<std::uint32_t>(next);
        keys[current].next = static_cast<std::uint32_t>(fresh);
        if (next != 0) keys[next].previous = static_cast<std::uint32_t>(fresh);
    }
    if (static_cast<std::uint32_t>(frame) > model.storage->maxFrame) {
        model.storage->maxFrame = static_cast<std::uint32_t>(frame);
    }
}

void NativeTrack(FakeModel& model, int morph, const std::vector<std::pair<int, float>>& track) {
    model.keys[static_cast<std::size_t>(morph)] = mdl::MorphKey{};
    model.keys[static_cast<std::size_t>(morph)].frame = 0;
    model.keys[static_cast<std::size_t>(morph)].allocated = 1;
    for (std::size_t index = 0; index < track.size(); ++index) {
        if (index == 0) {
            if (track[index].first == 0) {
                model.keys[static_cast<std::size_t>(morph)].value = track[index].second;
                continue;
            }
            model.keys[static_cast<std::size_t>(morph)].value = 0.0f;
        }
        NativeAdd(model, morph, track[index].first, track[index].second);
    }
}

std::vector<mdl::MorphKey> PoolSnapshot(const FakeModel& model) { return model.keys; }

bool PoolUnchanged(const FakeModel& model, const std::vector<mdl::MorphKey>& snapshot) {
    if (snapshot.size() != model.keys.size()) return false;
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        const mdl::MorphKey& left = snapshot[index];
        const mdl::MorphKey& right = model.keys[index];
        if (left.frame != right.frame || left.value != right.value ||
            left.allocated != right.allocated || left.previous != right.previous ||
            left.next != right.next) {
            return false;
        }
    }
    return true;
}

std::size_t ChannelOf(const std::vector<Channel>& channels, const std::string& id) {
    for (std::size_t index = 0; index < channels.size(); ++index)
        if (channels[index].id == id) return index;
    return channels.size();
}

// MMDApp's own constructor builds only a trivial subobject: WinMain memsets the
// whole state right after construction, so a test has to do the same before it
// can rely on empty model and accessory slots.  Without this the empty-slot
// checks would read whatever the stack happened to hold.
void ResetApp(MMDApp& app) {
    std::memset(static_cast<void*>(&app.state), 0, sizeof(app.state));
    app.EnglishUI() = 2;
}

// ---------------------------------------------------------------------------
// Hidden window helpers.
// ---------------------------------------------------------------------------

HWND Control(HWND parent, int id) { return GetDlgItem(parent, id); }

RECT ControlRect(HWND parent, int id) {
    RECT rect{};
    GetWindowRect(Control(parent, id), &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

// The launcher buttons are children of a window this test never shows, so
// IsWindowVisible is always false: the window's own WS_VISIBLE bit is the fact
// ShowMorphControls controls.
bool Shown(HWND parent, int id) {
    HWND control = Control(parent, id);
    return control != nullptr && (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
}

HWND MakeControl(HWND parent, const wchar_t* cls, DWORD style, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, cls, L"", WS_CHILD | style, x, y, w, h, parent,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), 0);
    return control;
}

// One expression lane exactly as the option band places it at bandXBase2 == 0:
// the selector 16 px right of its own previous arrow, the forward arrow 92 px
// right of that.
struct Lane {
    int combo;
    int prevArrow;
    int nextArrow;
    int comboX;
    int arrowX;
    int comboY;
    int arrowY;
};

const Lane kLanes[4] = {
    {504, 507, 508, 618, 602, 650, 649},
    {509, 512, 513, 618, 602, 580, 579},
    {514, 517, 518, 749, 733, 580, 579},
    {519, 522, 523, 749, 733, 650, 649},
};

void CreateLaneControls(HWND parent) {
    for (const Lane& lane : kLanes) {
        MakeControl(parent, L"COMBOBOX", CBS_DROPDOWNLIST, lane.combo, lane.comboX, lane.comboY,
                    92, 500);
        MakeControl(parent, L"BUTTON", 0, lane.prevArrow, lane.arrowX, lane.arrowY, 16, 22);
        MakeControl(parent, L"BUTTON", 0, lane.nextArrow, lane.arrowX + 108, lane.arrowY, 16, 22);
    }
}

// ---------------------------------------------------------------------------
// Curve-editor settings: the same path the host derives, so this test also pins
// the file name and the section key.
// ---------------------------------------------------------------------------

std::wstring ExeDirectory() {
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    if (count == 0 || count >= 32768) return {};
    std::wstring text(path, count);
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash + 1);
}

std::wstring SettingsPath() { return ExeDirectory() + L"Data\\curve_editor.ini"; }

void WriteRawSetting(const std::wstring& path, const wchar_t* value) {
    WritePrivateProfileStringW(L"CurveEditor", L"MorphComparison", value, path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

void ComparisonMode() {
    const std::wstring path = SettingsPath();
    Check(!path.empty(), "the settings path resolves");
    CreateDirectoryW((ExeDirectory() + L"Data").c_str(), nullptr);
    DeleteFileW(path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    Check(ch::ReadMorphComparisonMode() == ce::kCompareSameGroup,
          "an absent setting reads as the same-group default");

    std::wstring error;
    Check(ch::WriteMorphComparisonMode(2, ce::kCompareFree, error), "the free mode is stored");
    Check(error.empty(), "a stored mode reports no error");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareFree, "the free mode round-trips");
    Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
          "the setting lands in Data/curve_editor.ini");

    Check(ch::WriteMorphComparisonMode(1, ce::kCompareAllGroups, error), "four groups is stored");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareAllGroups, "four groups round-trips");
    Check(ch::WriteMorphComparisonMode(2, ce::kCompareSameGroup, error), "the default is stored");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareSameGroup, "the default round-trips");

    Check(!ch::WriteMorphComparisonMode(0, 0, error), "the off mode is refused");
    Check(!error.empty(), "a refused mode carries a message");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareSameGroup,
          "a refused write leaves the stored mode alone");
    Check(!ch::WriteMorphComparisonMode(2, ce::kCompareModeCount, error),
          "a mode above the range is refused");

    WriteRawSetting(path, L"9");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareSameGroup,
          "an out-of-range stored byte reads as the default");
    WriteRawSetting(path, L"0");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareSameGroup,
          "a stored zero reads as the default");
    WriteRawSetting(path, L"2");
    Check(ch::ReadMorphComparisonMode() == ce::kCompareAllGroups,
          "a stored in-range byte is honoured");
}

// ---------------------------------------------------------------------------
// Channels, the changed-track diff and the identity gate.
// ---------------------------------------------------------------------------

void ChannelBuild() {
    FakeModel model = MakeModel({{"hide", 0},
                                 {"brow1", 1}, {"brow2", 1},
                                 {"eye1", 2}, {"mouth1", 3}, {"other1", 4}},
                                "morph-host");
    NativeTrack(model, 1, {{0, 0.0f}, {10, 0.5f}});
    NativeTrack(model, 4, {{0, 0.25f}});

    MMDApp app;
    ResetApp(app);
    app.ModelSlot(0) = model.raw();
    app.SetSelectedModelSlot(0);

    std::vector<Channel> channels;
    std::wstring error;
    Check(ch::ReadMorphChannels(app, channels, error), "the model's channels read");
    Check(channels.size() == 5, "only the five panel 1..4 expressions are offered");
    Check(ChannelOf(channels, mc::StableId(0, model.morphs[0].name)) == channels.size(),
          "the panel-0 system morph is not offered");
    bool groups = true;
    bool fields = true;
    for (const Channel& channel : channels) {
        groups = groups && channel.group >= 0 && channel.group <= 3;
        fields = fields && channel.minimum == 0.0 && channel.maximum == 1.0 &&
                 !channel.label.empty() && !channel.track.keys.empty() &&
                 channel.track.keys.front().frame == 0;
    }
    Check(groups, "every channel takes its group from morph_curve::PanelGroup");
    Check(fields, "every channel has a label, the weight range and a frame-zero key");
    const std::size_t brow = ChannelOf(channels, mc::StableId(1, model.morphs[1].name));
    Check(brow != channels.size() && channels[brow].group == 0, "the eyebrow group is zero");
    Check(brow != channels.size() && channels[brow].track.keys.size() == 2,
          "the eyebrow channel carries its native keys");
    const std::size_t eye = ChannelOf(channels, mc::StableId(3, model.morphs[3].name));
    Check(eye != channels.size() && channels[eye].group == 1, "the eye group is one");
    const std::size_t other = ChannelOf(channels, mc::StableId(5, model.morphs[5].name));
    Check(other != channels.size() && channels[other].group == 3, "the other group is three");

    // A model whose expressions are all system morphs has nothing to edit.
    FakeModel system = MakeModel({{"hide", 0}}, "system-only");
    std::vector<Channel> none;
    app.ModelSlot(0) = system.raw();
    Check(!ch::ReadMorphChannels(app, none, error),
          "a model without user expressions is refused");
    Check(none.empty(), "a refused read yields no channel");
    Check(!error.empty(), "a refused read carries a message");

    // Every message of this source is three-language.
    for (unsigned language = 0; language < 3; ++language) {
        app.EnglishUI() = static_cast<std::uint8_t>(language);
        std::wstring text;
        app.ModelSlot(0) = nullptr;
        Check(!ch::ReadMorphChannels(app, none, text) && !text.empty(),
              "a missing model is reported in every language");
        app.ModelSlot(0) = system.raw();
        Check(!ch::ReadMorphChannels(app, none, text) && !text.empty(),
              "an empty expression list is reported in every language");
    }
    app.ModelSlot(0) = model.raw();
    app.EnglishUI() = 2;
}

void ChangedTracks() {
    FakeModel model = MakeModel({{"brow1", 1}, {"eye1", 2}}, "diff");
    NativeTrack(model, 0, {{0, 0.0f}, {10, 0.5f}});

    MMDApp app;
    ResetApp(app);
    app.ModelSlot(0) = model.raw();
    app.SetSelectedModelSlot(0);

    std::vector<Channel> baseline;
    std::wstring error;
    Check(ch::ReadMorphChannels(app, baseline, error), "the diff baseline reads");

    Check(ch::ChangedMorphTracks(baseline, baseline).empty(),
          "an untouched list yields no track at all");

    std::vector<Channel> edited = baseline;
    const std::size_t brow = ChannelOf(edited, mc::StableId(0, model.morphs[0].name));
    Check(brow != edited.size(), "the eyebrow channel exists");
    edited[brow].track.keys.back().value = 0.75;
    auto changed = ch::ChangedMorphTracks(edited, baseline);
    Check(changed.size() == 1, "one edited channel yields exactly one track");
    Check(changed.size() == 1 && changed[0].id == edited[brow].id,
          "the reported track is the edited channel");

    std::vector<Channel> added = baseline;
    ac::Key extra;
    extra.frame = 24;
    extra.value = 1.0;
    extra.interpolation = ac::Interpolation::Linear;
    extra.tangent = ac::Tangent::Free;
    added[brow].track.keys.push_back(extra);
    changed = ch::ChangedMorphTracks(added, baseline);
    Check(changed.size() == 1 && changed[0].track.keys.size() == 3,
          "an inserted key is reported with the whole track");

    std::vector<Channel> both = edited;
    both[ChannelOf(both, mc::StableId(1, model.morphs[1].name))].track.keys.back().value = 0.2;
    changed = ch::ChangedMorphTracks(both, baseline);
    Check(changed.size() == 2, "two edited channels yield two tracks");
}

void IdentityGate() {
    FakeModel first = MakeModel({{"brow1", 1}, {"eye1", 2}}, "first");
    // Different names, so a track id of the second model cannot name a morph of
    // the first one.
    FakeModel second = MakeModel({{"browB", 1}, {"eyeB", 2}}, "second");
    NativeTrack(first, 0, {{0, 0.0f}, {10, 0.5f}});
    NativeTrack(second, 0, {{0, 0.1f}, {10, 0.6f}});

    MMDApp app;
    ResetApp(app);
    app.ModelSlot(0) = first.raw();
    app.ModelSlot(1) = second.raw();
    app.SetSelectedModelSlot(0);

    const ch::MorphModelIdentity firstIdentity = ch::MorphIdentityOf(app, first.raw());
    Check(firstIdentity.model == first.raw(), "the identity captures the model");
    Check(ch::MorphIdentityCurrent(app, firstIdentity), "the selected model is current");
    // A model that is loaded but not selected is not current: the write path
    // targets the selected one, so it must never be redirected to another.
    const ch::MorphModelIdentity otherIdentity = ch::MorphIdentityOf(app, second.raw());
    Check(otherIdentity.model == second.raw(), "the other model's identity captures it");
    Check(!ch::MorphIdentityCurrent(app, otherIdentity), "another model is not current");
    const ch::MorphModelIdentity unknownIdentity = ch::MorphIdentityOf(app, nullptr);
    Check(unknownIdentity.model == nullptr, "a null model yields no identity");
    Check(!ch::MorphIdentityCurrent(app, unknownIdentity), "an empty identity is never current");

    std::vector<Channel> channels;
    std::wstring error;
    Check(ch::ReadMorphChannels(app, channels, error), "the first model reads");
    const auto firstPool = PoolSnapshot(first);
    const auto secondPool = PoolSnapshot(second);

    // The untouched list must not register anything: this is the whole point of
    // writing only the tracks that really changed.
    Check(ch::WriteMorphChannels(app, firstIdentity, channels, channels, error),
          "an unchanged list is accepted");
    Check(mc::CurveCount(first.raw()) == 0, "an unchanged list registers no authored curve");
    Check(PoolUnchanged(first, firstPool), "an unchanged list leaves the native pool alone");

    // A second model is selected: the captured object must not be written.
    app.SetSelectedModelSlot(1);
    error.clear();
    Check(!ch::WriteMorphChannels(app, firstIdentity, channels, {}, error),
          "a no-longer-selected model is refused");
    Check(!error.empty(), "the refusal carries a message");
    const std::wstring movedError = error;
    Check(PoolUnchanged(first, firstPool) && PoolUnchanged(second, secondPool),
          "a refused write touches neither model");
    Check(mc::CurveCount(first.raw()) == 0 && mc::CurveCount(second.raw()) == 0,
          "a refused write registers no authored curve");

    // The captured model is deleted: nothing may be written either.
    app.ModelSlot(0) = nullptr;
    error.clear();
    Check(!ch::MorphIdentityCurrent(app, firstIdentity), "a deleted model is not current");
    Check(!ch::WriteMorphChannels(app, firstIdentity, channels, {}, error),
          "a deleted model is refused");
    Check(!error.empty(), "the deleted-model refusal carries a message");
    const std::wstring goneError = error;
    Check(movedError != goneError, "a switched and a deleted model are reported differently");
    Check(PoolUnchanged(first, firstPool) && PoolUnchanged(second, secondPool),
          "the deletion refusal touches no pool");

    // A track that names a morph of another model is refused by the backend as
    // well, so a recycled address cannot inherit a curve even then.
    app.ModelSlot(0) = first.raw();
    app.SetSelectedModelSlot(0);
    const ac::Track foreign = channels.empty() ? ac::Track{} : channels.front().track;
    std::string detail;
    Check(!mc::Write(first.raw(), {{mc::StableId(0, second.morphs[0].name), foreign}}, detail),
          "a track naming another model's morph is refused");
    Check(PoolUnchanged(first, firstPool), "the foreign-id refusal leaves the pool alone");

    app.ModelSlot(0) = nullptr;
    app.ModelSlot(1) = nullptr;
}

// The hole a bare pointer cannot see: the captured address is still in a slot
// and still selected, but a different model now lives there.  Every mutation
// below keeps the address, so only the matching identity field can catch it.
void IdentityReuse() {
    FakeModel model = MakeModel({{"brow1", 1}, {"eye1", 2}}, "reuse");
    NativeTrack(model, 0, {{0, 0.0f}, {10, 0.5f}});

    MMDApp app;
    ResetApp(app);
    app.ModelSlot(0) = model.raw();
    app.SetSelectedModelSlot(0);

    const ch::MorphModelIdentity identity = ch::MorphIdentityOf(app, model.raw());
    Check(ch::MorphIdentityCurrent(app, identity), "the pristine identity is current");

    // 1. A morph name rewritten in place: the morph table pointer, the key pool
    //    pointer and the count all stay the same, so only the digest notices.
    const char savedName = model.record()->morphs[0].name[0];
    model.record()->morphs[0].name[0] = 'X';
    Check(!ch::MorphIdentityCurrent(app, identity),
          "a morph table rewritten in place is not the captured model");
    model.record()->morphs[0].name[0] = savedName;
    Check(ch::MorphIdentityCurrent(app, identity), "restoring the name restores the identity");

    // 2. The same in-place case through the digest's second field.
    const mdl::MorphPanel savedPanel = model.record()->morphs[0].panel;
    model.record()->morphs[0].panel = mdl::MorphPanel::mouth;
    Check(!ch::MorphIdentityCurrent(app, identity), "a changed panel is not the captured model");
    model.record()->morphs[0].panel = savedPanel;

    // 3. The resolved model path, the identity the backend does not keep and
    //    the host adds.
    const wchar_t savedPath = model.record()->path[0];
    model.record()->path[0] = savedPath == L'Z' ? L'Y' : L'Z';
    Check(!ch::MorphIdentityCurrent(app, identity),
          "a changed model path is not the captured model");
    model.record()->path[0] = savedPath;
    Check(ch::MorphIdentityCurrent(app, identity), "restoring the path restores the identity");

    // 4. The bone table.
    const std::uint32_t savedBones = model.record()->boneCount;
    model.record()->boneCount = savedBones + 1;
    Check(!ch::MorphIdentityCurrent(app, identity),
          "a changed bone count is not the captured model");
    model.record()->boneCount = savedBones;
    Check(ch::MorphIdentityCurrent(app, identity), "restoring the bones restores the identity");

    // 5. A whole new morph table at the same address: different pointers and a
    //    different count, which is what a recycled allocation looks like.
    std::vector<mdl::MorphRecord> replacement(3, mdl::MorphRecord{});
    for (mdl::MorphRecord& morph : replacement) morph.panel = mdl::MorphPanel::other;
    model.record()->morphs = replacement.data();
    model.record()->morphCount = static_cast<std::uint32_t>(replacement.size());
    Check(!ch::MorphIdentityCurrent(app, identity),
          "a new morph table at the same address is refused");

    // The refusal really blocks the write, and an empty diff proves the identity
    // check is what refused: without it the call would succeed with nothing to do.
    std::vector<Channel> channels;
    std::wstring error;
    const auto pool = PoolSnapshot(model);
    Check(!ch::WriteMorphChannels(app, identity, channels, {}, error),
          "a recycled address cannot be written");
    Check(!error.empty(), "the recycled-address refusal carries a message");
    Check(PoolUnchanged(model, pool), "the recycled-address refusal leaves the pool alone");

    app.ModelSlot(0) = nullptr;
}

// The launcher opens on the selector's current morph, so a lane is usable only
// when that selector names a morph of that very panel.
//
// The real four-column mapping is lane L <-> morph panel L + 1, which is
// morph_curve::PanelGroup L, and it comes straight from production: combos
// 504/509/514/519 are lanes 0..3 (src/window/command_control_500.cpp:462-501),
// each row's step filter is `panel == MorphPanel(lane + 1)` (same file, line
// 281), and PostLoadInit builds 504 with eyebrow, 509 with eye, 514 with mouth
// and 519 with other (src/model/post_load_init.cpp:161-164).  Lane 1 is
// therefore the eye lane: a panel-1 (eyebrow) morph in it must be refused.
void LaneSelection() {
    Check(mc::PanelGroup(1) == 0 && mc::PanelGroup(2) == 1 &&
              mc::PanelGroup(3) == 2 && mc::PanelGroup(4) == 3 &&
              mc::PanelGroup(0) == -1,
          "the four panels map to lane 0..3 and the system panel to no lane");

    FakeModel model = MakeModel({{"hide", 0}, {"brow1", 1}, {"brow2", 1},
                                 {"eye1", 2}, {"mouth1", 3}, {"other1", 4}},
                                "lane-select");
    MMDApp app;
    ResetApp(app);

    Check(ch::MorphLaneInitial(app, 0).empty(), "an unloaded project has no lane morph");
    Check(ch::MorphLaneInitial(app, -1).empty(), "a negative lane is refused");
    Check(ch::MorphLaneInitial(app, ch::kMorphLaneCount).empty(), "a lane past the four is refused");

    app.ModelSlot(0) = model.raw();
    app.SetSelectedModelSlot(0);
    for (int lane = 0; lane < ch::kMorphLaneCount; ++lane) {
        model.record()->selectedMorphs[lane] = -1;
        Check(ch::MorphLaneInitial(app, lane).empty(), "an unset selector has nothing to open");
    }

    // Every lane accepts a morph of its own panel: brow2, eye1, mouth1, other1.
    const std::int32_t ownMorph[4] = {2, 3, 4, 5};
    const char* const ownLane[4] = {
        "the eyebrow lane opens on its own morph",
        "the eye lane opens on its own morph",
        "the mouth lane opens on its own morph",
        "the other lane opens on its own morph"};
    for (int lane = 0; lane < ch::kMorphLaneCount; ++lane) {
        const std::size_t index = static_cast<std::size_t>(ownMorph[lane]);
        model.record()->selectedMorphs[lane] = ownMorph[lane];
        Check(ch::MorphLaneInitial(app, lane) ==
                  mc::StableId(ownMorph[lane], model.morphs[index].name),
              ownLane[lane]);
    }

    // The eye lane refuses a morph of another panel, and takes its own back.
    model.record()->selectedMorphs[1] = 1;  // brow1 is panel 1, so group 0
    Check(ch::MorphLaneInitial(app, 1).empty(),
          "a selector that names another panel is refused");
    model.record()->selectedMorphs[1] = 3;  // eye1 is panel 2, this lane's own
    Check(ch::MorphLaneInitial(app, 1) == mc::StableId(3, model.morphs[3].name),
          "a selector back on its own panel is accepted again");

    model.record()->selectedMorphs[1] = static_cast<std::int32_t>(model.record()->morphCount);
    Check(ch::MorphLaneInitial(app, 1).empty(), "an out-of-range selector is refused");
    model.record()->selectedMorphs[1] = 0;
    Check(ch::MorphLaneInitial(app, 1).empty(),
          "a selector on the hidden system morph is refused");

    app.ModelSlot(0) = nullptr;
}

// ---------------------------------------------------------------------------
// The four launcher buttons.
// ---------------------------------------------------------------------------


struct WheelNotifications { int changes[4]{}; int escaped = 0; };

LRESULT CALLBACK ObserveMorphWheel(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR data) {
    auto& state = *reinterpret_cast<WheelNotifications*>(data);
    if (message == WM_COMMAND && HIWORD(wp) == CBN_SELCHANGE) {
        for (int lane = 0; lane < 4; ++lane)
            if (LOWORD(wp) == kLanes[lane].combo) ++state.changes[lane];
    }
    if (message == WM_MOUSEWHEEL) { ++state.escaped; return 0; }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ObserveMorphWheel, id);
    return DefSubclassProc(hwnd, message, wp, lp);
}

void LauncherWheel() {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    auto app = std::make_unique<MMDApp>(); ResetApp(*app);
    HWND parent = CreateWindowExW(0, L"STATIC", L"hidden morph wheel test", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1200, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "wheel test creates a hidden host");
    app->Hwnd() = parent; app->state.optflag[5] = 1;
    WheelNotifications observed;
    SetWindowSubclass(parent, ObserveMorphWheel, 1, reinterpret_cast<DWORD_PTR>(&observed));
    CreateLaneControls(parent); ch::CreateControls(app.get(), parent);
    for (int lane = 0; lane < 4; ++lane) {
        HWND combo = Control(parent, kLanes[lane].combo);
        HWND button = Control(parent, ch::kMorphCurveLane[lane]);
        for (auto label : {L"first", L"middle", L"last"}) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        MoveWindow(button, 50 + lane * 100, 50, 22, 22, FALSE);
        ShowWindow(button, SW_SHOWNA); EnableWindow(button, TRUE);
    }
    for (int lane = 0; lane < 4; ++lane) {
        HWND combo = Control(parent, kLanes[lane].combo);
        HWND button = Control(parent, ch::kMorphCurveLane[lane]);
        RECT rect{}; GetWindowRect(button, &rect);
        LPARAM point = MAKELPARAM(static_cast<short>(rect.left + 10), static_cast<short>(rect.top + 10));
        auto wheel = [&](HWND target, int delta) { SendMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), point); };
        const HWND focus = GetFocus();
        wheel(button, -WHEEL_DELTA);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 2, "launcher wheel down selects the next expression");
        wheel(button, -WHEEL_DELTA);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 2, "launcher wheel stops at the last expression like the combo");
        wheel(button, WHEEL_DELTA);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "launcher wheel up selects the previous expression");
        Check(observed.changes[lane] == 2, "launcher delivers native selection notifications exactly once");
        wheel(parent, WHEEL_DELTA);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0, "host wheel over the launcher also selects an expression");
        wheel(parent, WHEEL_DELTA);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0, "host wheel stops at the first expression");
        EnableWindow(combo, FALSE); wheel(button, -WHEEL_DELTA); wheel(parent, -WHEEL_DELTA); EnableWindow(combo, TRUE);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0, "a disabled selector cannot be changed through its launcher");
        EnableWindow(button, FALSE); wheel(button, -WHEEL_DELTA); wheel(parent, -WHEEL_DELTA); EnableWindow(button, TRUE);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0, "a disabled launcher cannot change its expression");
        wheel(button, 0);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0, "zero wheel delta does not switch expressions");
        for (WORD modifier : {WORD(MK_CONTROL), WORD(MK_SHIFT)})
            SendMessageW(button, WM_MOUSEWHEEL, MAKEWPARAM(modifier, WORD(-WHEEL_DELTA)), point);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0, "modified wheel input cannot recurse through the combo and parent");
        Check(observed.changes[lane] == 3, "boundaries and disabled controls emit no selection changes");
        Check(GetFocus() == focus, "wheel switching does not steal keyboard focus");
        for (int other = lane + 1; other < 4; ++other)
            Check(SendMessageW(Control(parent, kLanes[other].combo), CB_GETCURSEL, 0, 0) == 1,
                  "wheel switching leaves other expression lanes unchanged");
    }
    Check(observed.escaped == 0, "launcher wheel messages never reach viewport or timeline scrolling");
    SendMessageW(parent, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(0, 0));
    Check(observed.escaped == 1, "wheel outside the launchers keeps the host's original behavior");
    DestroyWindow(parent); app->Hwnd() = nullptr;
}

void LauncherButtons() {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);

    auto app = std::make_unique<MMDApp>();
    ResetApp(*app);
    HWND parent = CreateWindowExW(0, L"STATIC", L"morph host test", WS_OVERLAPPEDWINDOW, 0, 0,
                                  1200, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "hidden host window");
    HMENU menu = CreateMenu(), file = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    SetMenu(parent, menu);
    for (int id = 471; id <= 487; ++id) {
        const bool combo = id == 471 || id == 474 || id == 475;
        const bool edit = id >= 478 && id <= 485;
        MakeControl(parent, combo ? L"COMBOBOX" : edit ? L"EDIT" : L"BUTTON",
                    combo ? CBS_DROPDOWNLIST : 0, id, 868, 620, 100, 22);
    }
    CreateLaneControls(parent);

    ch::CreateControls(app.get(), parent);
    for (int lane = 0; lane < ch::kMorphLaneCount; ++lane)
        Check(GetParent(Control(parent, ch::kMorphCurveLane[lane])) == parent,
              "a launcher is created before MMDApp::Hwnd is assigned");

    app->Hwnd() = parent;
    app->EnglishUI() = 2;
    app->state.optflag[0] = 0;
    app->state.optflag[5] = 1;

    // Geometry: 22 px launcher, 70 px selector, both arrows unmoved.
    const int comboId[4] = {504, 509, 514, 519};
    const int launcher[4] = {ch::kMorphCurveLane[0], ch::kMorphCurveLane[1],
                             ch::kMorphCurveLane[2], ch::kMorphCurveLane[3]};
    ch::LayoutMorphControls(app.get());
    for (int lane = 0; lane < 4; ++lane) {
        const RECT button = ControlRect(parent, launcher[lane]);
        const RECT combo = ControlRect(parent, comboId[lane]);
        const RECT previous = ControlRect(parent, kLanes[lane].prevArrow);
        const RECT forward = ControlRect(parent, kLanes[lane].nextArrow);
        Check(button.left == kLanes[lane].comboX && button.right - button.left == 22,
              "the launcher takes the selector's first 22 px");
        Check(combo.left == kLanes[lane].comboX + 22 && combo.right - combo.left == 70,
              "the selector keeps 70 px and starts after the launcher");
        Check(previous.left == kLanes[lane].arrowX && previous.right - previous.left == 16 &&
                  previous.top == combo.top && previous.bottom == combo.bottom,
              "the previous arrow keeps its X position and aligns with the selector");
        Check(forward.left == kLanes[lane].arrowX + 108 && forward.right - forward.left == 16,
              "the forward arrow is not moved");
    }

    // Idempotence: the second pass must produce the same rectangles, not narrow
    // the selector again.
    RECT before[4];
    for (int lane = 0; lane < 4; ++lane) before[lane] = ControlRect(parent, comboId[lane]);
    ch::LayoutMorphControls(app.get());
    ch::LayoutMorphControls(app.get());
    for (int lane = 0; lane < 4; ++lane) {
        const RECT after = ControlRect(parent, comboId[lane]);
        Check(before[lane].left == after.left && before[lane].right == after.right &&
                  before[lane].top == after.top && before[lane].bottom == after.bottom,
              "a repeated relayout cannot narrow the selector again");
    }

    // Visibility: the expression band is a model-mode band with its own expand
    // flag, and ShowControls is the notification the host already sends.
    app->state.optflag[0] = 0;
    app->state.optflag[5] = 1;
    ch::ShowControls(app.get(), false);
    for (int lane = 0; lane < 4; ++lane)
        Check(Shown(parent, launcher[lane]), "model mode with the band open shows the launchers");
    app->state.optflag[0] = 1;
    ch::ShowControls(app.get(), true);
    for (int lane = 0; lane < 4; ++lane)
        Check(!Shown(parent, launcher[lane]), "a camera panel hides the launchers");
    app->state.optflag[0] = 0;
    app->state.optflag[5] = 0;
    ch::ShowControls(app.get(), false);
    for (int lane = 0; lane < 4; ++lane)
        Check(!Shown(parent, launcher[lane]), "a collapsed expression band hides the launchers");
    app->state.optflag[5] = 1;
    ch::ShowControls(app.get(), false);

    // Enablement needs a valid current selection for that very panel, not just
    // some expression inside the panel.
    for (int lane = 0; lane < 4; ++lane)
        Check(!IsWindowEnabled(Control(parent, launcher[lane])),
              "without a model every launcher is disabled");

    FakeModel model = MakeModel({{"brow1", 1}, {"mouth1", 3}}, "lanes");
    app->ModelSlot(0) = model.raw();
    app->SetSelectedModelSlot(0);
    for (int lane = 0; lane < 4; ++lane) model.record()->selectedMorphs[lane] = -1;
    ch::ShowControls(app.get(), false);
    for (int lane = 0; lane < 4; ++lane)
        Check(!IsWindowEnabled(Control(parent, launcher[lane])),
              "an unset selector disables its launcher even when the panel has an expression");

    model.record()->selectedMorphs[0] = 0;  // brow1 is panel 1
    model.record()->selectedMorphs[2] = 1;  // mouth1 is panel 3
    ch::ShowControls(app.get(), false);
    Check(IsWindowEnabled(Control(parent, launcher[0])), "the eyebrow launcher is enabled");
    Check(!IsWindowEnabled(Control(parent, launcher[1])),
          "a panel without an expression disables its launcher");
    Check(IsWindowEnabled(Control(parent, launcher[2])), "the mouth launcher is enabled");
    Check(!IsWindowEnabled(Control(parent, launcher[3])),
          "the empty other panel disables its launcher");

    // The mouth panel does hold an expression, which is exactly the case a
    // panel-wide test would wrongly enable.
    model.record()->selectedMorphs[2] = 0;  // a panel-1 morph in the panel-3 selector
    ch::ShowControls(app.get(), false);
    Check(!IsWindowEnabled(Control(parent, launcher[2])),
          "a selector naming another panel disables its launcher");
    model.record()->selectedMorphs[2] = 1;
    ch::ShowControls(app.get(), false);
    Check(IsWindowEnabled(Control(parent, launcher[2])),
          "the launcher comes back with a selector of its own panel");

    // Three-language captions.
    std::wstring captions[3];
    for (unsigned language = 0; language < 3; ++language) {
        app->EnglishUI() = static_cast<std::uint8_t>(language);
        ch::Localize(app.get());
        wchar_t text[128]{};
        GetWindowTextW(Control(parent, launcher[0]), text, 128);
        captions[language] = text;
        Check(!captions[language].empty(), "the launcher caption is never empty");
        Check(GetMenuState(file, ce::kAccessoryMotionMenu, MF_BYCOMMAND) != UINT(-1),
              "the accessory VMD menu item still exists");
    }
    Check(captions[0] != captions[1] && captions[1] != captions[2] && captions[0] != captions[2],
          "the launcher caption follows the three UI languages");
    app->EnglishUI() = 2;

    // OpenMorph builds its hidden window without activating the host.
    ch::OpenMorph(app.get(), 2, false);
    Check(GetForegroundWindow() != parent, "hidden integration does not activate host");
    ce::CloseOwned(parent);

    DestroyWindow(parent);
    app->Hwnd() = nullptr;
    app->ModelSlot(0) = nullptr;
    app->EnglishUI() = 2;
}


void BakedMorphKeepsBoneEditing() {
    for (bool curveBake : {true, false}) {
        auto model = MakeModel({{"smile", 1}}, "bake-edit");
        NativeTrack(model, 0, {{0, 0}, {20, 1}});
        auto app = std::make_unique<MMDApp>();
        std::memset(&app->state, 0, sizeof(app->state));
        mdl::BoneRecord bone{};
        auto boneKeys = std::make_unique<mdl::BoneKey[]>(mdl::kBoneKeyCapacity);
        auto displayKeys = std::make_unique<mdl::DisplayKey[]>(mdl::kDisplayKeyCapacity);
        auto& boneKey = boneKeys[0];
        auto& display = displayKeys[0];
        unsigned char selected = 1, dirty = 0;
        bone.type = mdl::BoneType::Move;
        bone.parent = bone.slotIndex = bone.tailIdx = -1;
        bone.rotQuat[3] = boneKey.rotation[3] = 1;
        for (int i : {0, 5, 10, 15}) bone.matInit[i] = bone.matWorld[i] = 1;
        boneKey.allocated = 1;
        display.visible = 1;
        auto& record = *model.record();
        record.boneCount = 1; record.boneTable = &bone; record.boneKeys = &boneKey;
        record.boneSelection = &selected; record.bonePhysicsState = &dirty;
        record.displayKeys = &display; record.selectedBone = 0; record.selectedMorphs[0] = 0;
        record.physicsMode = 2;
        HWND parent = CreateWindowW(L"STATIC", L"Hidden morph bake regression", WS_POPUP,
            0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        app->Hwnd() = parent; app->ModelSlot(0) = model.raw(); app->EnglishUI() = 2;
        MMDApp* previousApp = mikudancestudio::g_Block;
        mikudancestudio::g_Block = app.get();
        ch::OpenMorph(app.get(), 0, false);
        HWND editor = FindWindowW(L"MikuMikuStudioCurveEditorWindow", nullptr);
        Check(editor != nullptr, "the expression bake editor opens hidden");
        HWND canvas = GetDlgItem(editor, 1020);
        const LPARAM key = SendMessageW(editor, WM_APP + 300, 1, 0);
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, key);
        SendMessageW(canvas, WM_LBUTTONUP, 0, key);
        Check(app->SelectionBoxDragging() == 0, "curve selection owns no timeline drag");
        if (curveBake) {
            SetDlgItemTextW(editor, 1071, L"0");
            SetDlgItemTextW(editor, 1073, L"20");
            SetDlgItemTextW(editor, 1075, L"1");
            SendMessageW(editor, WM_COMMAND, MAKEWPARAM(1076, BN_CLICKED),
                         reinterpret_cast<LPARAM>(GetDlgItem(editor, 1076)));
        } else {
            namespace cb = mikudancestudio::channel_bake;
            cb::Request request; request.scope = cb::Scope::Model;
            request.firstFrame = 0; request.lastFrame = 20; request.step = 1;
            request.selection.Touch(cb::Kind::Morph, 0, 1);
            cb::Plan plan; std::wstring error;
            Check(cb::Prepare(*app, request, plan, error) == cb::Error::None,
                  "prepare expression-only channel bake");
            Check(cb::Commit(*app, plan, error) == cb::Error::None,
                  "commit expression-only channel bake");
            mikudancestudio::SeekSelectedModelToCurrentFrame(app.get());
        }
        ac::Track baked; std::string error;
        Check(mc::Read(model.raw(), 0, baked, error) && baked.keys.size() == 21,
              "expression bake really writes all 21 samples");
        Check(app->TimelineSelectionChanged() == 0,
              "expression selection and bake do not arm a native timeline drag");
        auto renderer = std::make_unique<mikudancestudio::D3DRenderer>();
        std::memset(renderer.get(), 0, sizeof(*renderer));
        app->Renderer() = renderer.get();
        for (int mode : {4, 10}) {
            app->InteractionDragMode() = static_cast<mikudancestudio::ViewportDragMode>(mode);
            app->MouseY() = 200; app->PreviousMouseY() = 210;
            mikudancestudio::BoneEditModes(app.get());
            const float rotation = bone.rotQuat[0], translation = bone.trans[0];
            Check(mode == 4 ? std::fabs(rotation) > 0.01f : std::fabs(translation) > 0.01f,
                  "bone rotation and translation respond after expression baking");
            mikudancestudio::HandleMouseMove(500 + mode, 270);
            Check(bone.rotQuat[0] == rotation && bone.trans[0] == translation,
                  "mouse movement after expression baking preserves manual bone edits");
        }
        // Defense against a stale flag from older editors or another callback.
        app->TimelineSelectionChanged() = 1;
        const auto pose = bone;
        mikudancestudio::HandleMouseMove(520, 271);
        Check(bone.rotQuat[0] == pose.rotQuat[0] && bone.trans[0] == pose.trans[0],
              "a selection flag without a timeline gesture cannot re-seek bones");
        app->SelectionBoxDragging() = 1;
        mikudancestudio::HandleMouseMove(540, 272); // Cross a frame cell; sub-cell motion is a no-op.
        Check(bone.rotQuat[0] == boneKey.rotation[0] && bone.trans[0] == boneKey.position[0],
              "a real timeline drag still applies the native frame");
        app->SelectionBoxDragging() = app->TimelineSelectionChanged() = 0;
        app->Renderer() = nullptr;
        ce::CloseOwned(parent); DestroyWindow(parent);
        app->ModelSlot(0) = nullptr; app->Hwnd() = nullptr;
        mc::Forget(model.raw()); mikudancestudio::bone_scale::Forget(model.raw());
        mikudancestudio::g_Block = previousApp;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if(argc>1 && std::string(argv[1])=="--morph-wheel") {
        LauncherWheel();
        std::printf("morph launcher wheel: %d checks, %d failures\n",g_checks,g_failures);
        return g_failures ? 1 : 0;
    }
    LauncherWheel();
    if(argc>1 && std::string(argv[1])=="--editor-input") {
        BakedMorphKeepsBoneEditing();
        std::printf("baked morph editing: %d checks, %d failures\n",g_checks,g_failures);
        return g_failures ? 1 : 0;
    }
    BakedMorphKeepsBoneEditing();
    ComparisonMode();
    ChannelBuild();
    ChangedTracks();
    IdentityGate();
    IdentityReuse();
    LaneSelection();
    LauncherButtons();
    std::printf("morph_curve_host_ui_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
