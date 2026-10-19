// MAINTENANCE: integration-manual; hidden-morph interaction contract of
// docs/records/animation/editing.md (a PMX morph whose panel byte is 0 stays out
// of the eyebrow/eye/mouth/other lanes while the timeline label list still
// carries it, and clicking it there is safe and side-effect free).
//
// This test drives the real production window procedure with real mouse
// messages in a hidden window. It never writes to the model file.
//
// Usage: hidden_morph_tests --model PATH [--mode contract|bounds]
// PATH must be the separately supplied Rou PMX regression sample described in
// docs/records/animation/editing.md. No copyrighted model is bundled here.
// Contract covers timeline selection, four-lane isolation, ordinary controls,
// registered animation and vertex/UV/material results of the group morphs.
// Bounds only repeats the click checks after in-memory panel changes 0/5/255;
// these values are never written back to the PMX.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"

using namespace mikumikustudio;

namespace {

int checks = 0;
int failures = 0;
int dialogs = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

// UTF-8 for console output: the model and morph names are not representable in
// every console code page, and printf's %ls would depend on the C locale.
std::string Utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0')
        return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 1)
        return std::string();
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
    out.pop_back();
    return out;
}

// ---- window lifecycle ------------------------------------------------------

// Keeps every window the production startup creates hidden and non-activated,
// so a background run never takes the desktop.
LRESULT CALLBACK HideHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD))
            create->style &= ~WS_VISIBLE;
    }
    if (code == HCBT_ACTIVATE)
        return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Cancels any modal box the production code opens (the PMX loader warns about
// malformed joint or morph data), so the run cannot block on a hidden dialog.
LRESULT CALLBACK DialogHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (message.message == WM_INITDIALOG) {
            wchar_t className[32]{};
            GetClassNameW(message.hwnd, className, 32);
            if (wcscmp(className, L"#32770") == 0) {
                ++dialogs;
                EndDialog(message.hwnd, IDCANCEL);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---- model helpers ---------------------------------------------------------

int PanelNumber(const mdl::MorphRecord& morph) {
    return static_cast<int>(morph.panel);
}

bool MorphInRange(const mdl::ModelRecord& model, int morphIndex) {
    return morphIndex >= 0 &&
           static_cast<std::uint32_t>(morphIndex) < model.morphCount;
}

void SetAllMorphValues(mdl::ModelRecord& model, float value) {
    for (std::uint32_t i = 0; i < model.morphCount; ++i)
        model.morphs[i].value = value;
}

// Row index of `morphIndex` in the label table the production sweep just built.
// Morph rows encode the index as -1-idx; -999 is the "no record" sentinel.
int FindMorphRow(const mdl::ModelRecord& model, int morphIndex) {
    for (int row = 0; row < 200; ++row) {
        const std::int32_t record = model.boneListRowRecord[row];
        if (record < 0 && record != -999 && -1 - record == morphIndex)
            return row;
    }
    return -1;
}

int FindRowOfType(const mdl::ModelRecord& model, unsigned char type) {
    for (int row = 0; row < 200; ++row)
        if (model.boneListRowType[row] == type)
            return row;
    return -1;
}

int RowY(int row) {
    return 0xA0 + 14 * row + 7;
}

// Mirrors the user scrolling the label list: puts `fullRow` on a small visible
// line and re-runs the production sweep. Returns the visible row index, or -1
// when the client area is too short for any row to pass the click gate.
int ScrollToVisibleRow(MMDApp* app, unsigned char* raw, int fullRow) {
    RECT rc{};
    GetClientRect(app->MainWindow(), &rc);
    int limit = -1;
    for (int row = 0; row < 200; ++row) {
        if (RowY(row) < rc.bottom - 0xF8)
            limit = row;
        else
            break;
    }
    if (limit < 0) {
        std::printf("note: client area %dx%d is too short for the label row click "
                    "gate (needs a bottom above %d)\n",
                    static_cast<int>(rc.right), static_cast<int>(rc.bottom),
                    RowY(0) + 0xF8);
        return -1;
    }
    int target = limit < 2 ? limit : 2;
    if (fullRow < target)
        target = fullRow;
    mdl::Mdl(raw)->boneListPos = fullRow - target;
    PostLanguageSweep(app);
    return target;
}

// The facial display group only lists its morph rows once the user expands it,
// which the production margin-column click does.
void EnsureFacialGroupExpanded(MMDApp* app, unsigned char* raw) {
    char message[256];
    mdl::DisplayGroup* groups = mdl::DisplayGroups(raw);
    if (groups == nullptr || mdl::Mdl(raw)->groupCount < 3)
        return;
    if (groups[2].flags != 0)
        return;
    const int facialRow = FindRowOfType(*mdl::Mdl(raw), 2);
    std::snprintf(message, sizeof message,
                  "facial display group row is present in the label table");
    Check(facialRow >= 0, message);
    if (facialRow < 0)
        return;
    const int row = ScrollToVisibleRow(app, raw, facialRow);
    if (row < 0)
        return;
    SendMessageW(app->MainWindow(), WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(12, RowY(row)));
    SendMessageW(app->MainWindow(), WM_LBUTTONUP, 0, MAKELPARAM(12, RowY(row)));
    std::snprintf(message, sizeof message,
                  "margin click on row %d expands the facial display group", row);
    Check(groups[2].flags != 0, message);
}

// ---- real mouse messages ---------------------------------------------------

constexpr int kNameColumnX = 40;  // inside the name column band X in (20, 95]

void SendMouse(HWND window, UINT message, int x, int y, bool shift) {
    WPARAM flags = message == WM_LBUTTONUP ? 0 : MK_LBUTTON;
    if (shift)
        flags |= MK_SHIFT;
    SendMessageW(window, message, flags, MAKELPARAM(x, y));
}

// The message order Windows produces for one user double click.
void SendClickSequence(MMDApp* app, int x, int y, bool shift) {
    HWND window = app->MainWindow();
    const auto savedShift = app->ShiftModifierState();
    app->ShiftModifierState() = shift ? 3 : 0;
    SendMouse(window, WM_LBUTTONDOWN, x, y, shift);
    SendMouse(window, WM_LBUTTONUP, x, y, shift);
    SendMouse(window, WM_LBUTTONDBLCLK, x, y, shift);
    SendMouse(window, WM_LBUTTONUP, x, y, shift);
    app->ShiftModifierState() = savedShift;
}

// ---- four-lane snapshots ---------------------------------------------------

constexpr int kMorphCombo[4] = {panel::kMorphCombo0, panel::kMorphCombo1,
                                panel::kMorphCombo2, panel::kMorphCombo3};
constexpr int kMorphSlider[4] = {panel::kMorphSlider0, panel::kMorphSlider1,
                                 panel::kMorphSlider2, panel::kMorphSlider3};
constexpr int kMorphEdit[4] = {panel::kMorphEdit0, panel::kMorphEdit1,
                               panel::kMorphEdit2, panel::kMorphEdit3};

struct LaneState {
    int count = 0;
    LRESULT cursel = CB_ERR;
    LONG itemData = -1;
    std::wstring text;
    std::string edit;
    LRESULT slider = -1;
    std::vector<LONG> items;
};

LaneState CaptureLane(MMDApp* app, int lane) {
    LaneState state;
    HWND window = app->MainWindow();
    HWND combo = GetDlgItem(window, kMorphCombo[lane]);
    state.count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    state.cursel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    for (int i = 0; i < state.count; ++i)
        state.items.push_back(
            static_cast<LONG>(SendMessageW(combo, CB_GETITEMDATA, i, 0)));
    if (state.cursel != CB_ERR &&
        state.cursel < static_cast<LRESULT>(state.count)) {
        state.itemData = state.items[static_cast<std::size_t>(state.cursel)];
        const auto length = SendMessageW(combo, CB_GETLBTEXTLEN, state.cursel, 0);
        if (length != CB_ERR) {
            std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1);
            SendMessageW(combo, CB_GETLBTEXT, state.cursel,
                         reinterpret_cast<LPARAM>(text.data()));
            state.text = text.data();
        }
    }
    state.slider =
        SendMessageW(GetDlgItem(window, kMorphSlider[lane]), TBM_GETPOS, 0, 0);
    char edit[128]{};
    GetWindowTextA(GetDlgItem(window, kMorphEdit[lane]), edit, sizeof edit);
    state.edit = edit;
    return state;
}

bool SameLane(const LaneState& a, const LaneState& b) {
    return a.count == b.count && a.cursel == b.cursel &&
           a.itemData == b.itemData && a.text == b.text && a.edit == b.edit &&
           a.slider == b.slider && a.items == b.items;
}

struct ContractState {
    LaneState lane[4];
    int selectedMorphs[4]{};
    std::vector<float> values;
};

ContractState CaptureContractState(MMDApp* app, unsigned char* raw) {
    ContractState state;
    for (int lane = 0; lane < 4; ++lane)
        state.lane[lane] = CaptureLane(app, lane);
    auto& model = *mdl::Mdl(raw);
    std::memcpy(state.selectedMorphs, model.selectedMorphs,
                sizeof state.selectedMorphs);
    state.values.reserve(model.morphCount);
    for (std::uint32_t i = 0; i < model.morphCount; ++i)
        state.values.push_back(model.morphs[i].value);
    return state;
}

bool SameLanes(const ContractState& a, const ContractState& b) {
    for (int lane = 0; lane < 4; ++lane)
        if (!SameLane(a.lane[lane], b.lane[lane]))
            return false;
    return true;
}

// The hidden morph row click contract: the production message sequence must not
// touch the four lanes, selectedMorphs, or any morph value, and the row must
// still be part of the table the production sweep built.
void CheckHiddenMorphClick(MMDApp* app, unsigned char* raw, int morphIndex,
                           const char* context, bool shift, int repeats) {
    char message[256];
    mdl::ModelRecord& model = *mdl::Mdl(raw);

    std::snprintf(message, sizeof message, "%s: hidden morph %d exists", context,
                  morphIndex);
    Check(MorphInRange(model, morphIndex), message);
    if (!MorphInRange(model, morphIndex))
        return;

    model.boneListPos = 0;
    PostLanguageSweep(app);
    const int fullRow = FindMorphRow(model, morphIndex);
    std::snprintf(message, sizeof message,
                  "%s: hidden morph %d is in the timeline row table", context,
                  morphIndex);
    Check(fullRow >= 0, message);
    if (fullRow < 0)
        return;

    const int row = ScrollToVisibleRow(app, raw, fullRow);
    std::snprintf(message, sizeof message,
                  "%s: hidden morph %d reaches a clickable row", context,
                  morphIndex);
    Check(row >= 0, message);
    if (row < 0)
        return;
    std::snprintf(message, sizeof message,
                  "%s: hidden morph %d stays on that row after the scroll",
                  context, morphIndex);
    Check(FindMorphRow(model, morphIndex) == row, message);
    if (FindMorphRow(model, morphIndex) != row)
        return;

    mdl::FrameGroup* face = nullptr;
    for (unsigned f = 0; f < model.facialFrameCount; ++f)
        if (model.displayFrames[f].targetIndex == morphIndex)
            face = &model.displayFrames[f];
    Check(face != nullptr, "clicked morph has a facial display-frame record");
    if (face == nullptr)
        return;
    const ContractState before = CaptureContractState(app, raw);
    for (int i = 0; i < repeats; ++i) {
        const bool wasSelected = face->selected != 0;
        const auto savedShift = app->ShiftModifierState();
        app->ShiftModifierState() = shift ? 3 : 0;
        SendMouse(app->MainWindow(), WM_LBUTTONDOWN, kNameColumnX, RowY(row), shift);
        SendMouse(app->MainWindow(), WM_LBUTTONUP, kNameColumnX, RowY(row), shift);
        const auto afterSingle = CaptureContractState(app, raw);
        Check(SameLanes(before, afterSingle) && before.values == afterSingle.values &&
                  std::memcmp(before.selectedMorphs, afterSingle.selectedMorphs,
                              sizeof before.selectedMorphs) == 0,
              "single click preserves controls, selected lanes and all weights");
        Check((face->selected != 0) == (shift ? !wasSelected : true),
              "single click updates timeline selection");
        SendMouse(app->MainWindow(), WM_LBUTTONDBLCLK, kNameColumnX, RowY(row), shift);
        SendMouse(app->MainWindow(), WM_LBUTTONUP, kNameColumnX, RowY(row), shift);
        app->ShiftModifierState() = savedShift;
        Check((face->selected != 0) == (shift ? !wasSelected : true),
              "click reaches the timeline selection handler; Shift toggles selection");
    }
    const ContractState after = CaptureContractState(app, raw);

    std::snprintf(message, sizeof message,
                  "%s: %s click on hidden morph %d keeps selectedMorphs",
                  context, shift ? "Shift" : "plain", morphIndex);
    Check(std::memcmp(before.selectedMorphs, after.selectedMorphs,
                      sizeof before.selectedMorphs) == 0,
          message);
    std::snprintf(message, sizeof message,
                  "%s: %s click on hidden morph %d keeps the four morph lanes",
                  context, shift ? "Shift" : "plain", morphIndex);
    Check(SameLanes(before, after), message);
    std::snprintf(message, sizeof message,
                  "%s: %s click on hidden morph %d keeps every morph value",
                  context, shift ? "Shift" : "plain", morphIndex);
    Check(before.values == after.values, message);
    std::snprintf(message, sizeof message,
                  "%s: hidden morph %d stays in the row table after the click",
                  context, morphIndex);
    Check(FindMorphRow(model, morphIndex) >= 0, message);
}

// ---- lane content contract -------------------------------------------------

// Every combo item must name a morph of that lane's panel, and no morph whose
// panel is outside 1..4 may appear in any lane. The lane count must equal the
// number of morphs carrying that panel.
void CheckLaneContents(MMDApp* app, unsigned char* raw, const char* context) {
    char message[256];
    auto& model = *mdl::Mdl(raw);
    int expected[4] = {0, 0, 0, 0};
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        const int panel = PanelNumber(model.morphs[i]);
        if (panel >= 1 && panel <= 4)
            ++expected[panel - 1];
    }
    for (int lane = 0; lane < 4; ++lane) {
        const LaneState state = CaptureLane(app, lane);
        std::snprintf(message, sizeof message,
                      "%s: lane %d item count equals the panel %d morph count",
                      context, lane, lane + 1);
        Check(state.count == expected[lane], message);
        bool clean = true;
        for (LONG index : state.items) {
            if (index < 0 ||
                static_cast<std::uint32_t>(index) >= model.morphCount) {
                clean = false;
                break;
            }
            if (PanelNumber(model.morphs[index]) != lane + 1) {
                clean = false;
                break;
            }
        }
        std::snprintf(message, sizeof message,
                      "%s: lane %d lists only panel %d morphs, no hidden morph",
                      context, lane, lane + 1);
        Check(clean, message);
    }
}

bool LaneContainsMorph(MMDApp* app, int lane, int morphIndex) {
    const LaneState state = CaptureLane(app, lane);
    for (LONG index : state.items)
        if (index == morphIndex)
            return true;
    return false;
}

// ---- group-driven evaluation ----------------------------------------------

constexpr int kGeometryFloats = 3 + 2 + 4 * 4;  // position, uv, four extra UVs
constexpr int kMaterialFloats = 16;             // see ReadMaterialChannels

// Observable material channels written by ModelApplyMorphs, in a fixed order:
// 0..2 diffuseMirror, 3 diffuse alpha, 4..6 specular, 7..9 ambient,
// 10..13 edgeColor, 14 specularPower, 15 edgeSize.
void ReadMaterialChannels(const mdl::ModelMaterialRecord& material,
                          float* out) {
    std::memcpy(out + 0, material.diffuseMirror, 3 * sizeof(float));
    out[3] = material.diffuse[3];
    std::memcpy(out + 4, material.specular, 3 * sizeof(float));
    std::memcpy(out + 7, material.ambient, 3 * sizeof(float));
    std::memcpy(out + 10, material.edgeColor, 4 * sizeof(float));
    out[14] = material.specularPower;
    out[15] = material.edgeSize;
}

// One observable material channel of a morph delta or of the load-time pool.
float MaterialChannel(const mdl::MaterialMorphChannels& channels, int index) {
    if (index < 3)
        return channels.diffuse[index];
    if (index == 3)
        return channels.diffuse[3];
    if (index < 7)
        return channels.specular[index - 4];
    if (index < 10)
        return channels.ambient[index - 7];
    if (index < 14)
        return channels.edgeColor[index - 10];
    if (index == 14)
        return channels.specularPower;
    return channels.edgeSize;
}

// Vertex/UV result of the production vertex-morph pass.
bool CaptureGeometry(MMDApp* app, unsigned char* raw, std::vector<float>& out) {
    float world[16]{};
    world[0] = world[5] = world[10] = world[15] = 1.0f;
    GpuModelInput input;
    if (!PrepareGpuModelInput(app, raw, world, input, true))
        return false;
    out.assign(input.vertices.size() * kGeometryFloats, 0.0f);
    for (std::size_t i = 0; i < input.vertices.size(); ++i) {
        float* dst = &out[i * kGeometryFloats];
        const gpu::Vertex& vertex = input.vertices[i];
        std::memcpy(dst + 0, vertex.position, 3 * sizeof(float));
        std::memcpy(dst + 3, vertex.uv, 2 * sizeof(float));
        for (int family = 0; family < 4; ++family)
            std::memcpy(dst + 5 + 4 * family, vertex.additionalUv[family],
                        4 * sizeof(float));
    }
    return true;
}

// Material result of the production material-morph pass.
void CaptureMaterials(unsigned char* raw, std::vector<float>& out) {
    ModelApplyMorphs(raw);
    auto* materials = mdl::Materials(raw);
    const std::uint32_t count = mdl::Mdl(raw)->materialCount;
    out.assign(static_cast<std::size_t>(count) * kMaterialFloats, 0.0f);
    for (std::uint32_t i = 0; i < count; ++i)
        ReadMaterialChannels(materials[i], &out[i * kMaterialFloats]);
}

// Independent expectation for driving one group morph at `groupValue`: every
// direct group reference of a vertex or UV morph adds
// entry.offset * groupValue * referenceWeight, and every direct reference of a
// material morph composes through the load-time pool the same way
// ApplyMaterialMorphChannels does. Neither production pass recurses into a group
// referenced by a group, and neither does this.
void BuildGroupExpectation(unsigned char* raw, int group, float groupValue,
                           const std::vector<float>& baseGeometry,
                           const std::vector<float>& baseMaterials,
                           std::vector<float>& geometry,
                           std::vector<float>& materials) {
    auto& model = *mdl::Mdl(raw);
    geometry = baseGeometry;
    materials = baseMaterials;

    const std::size_t materialCount = model.materialCount;
    std::vector<float> additive(materialCount * kMaterialFloats, 0.0f);
    std::vector<float> multiplicative(materialCount * kMaterialFloats, 1.0f);
    bool hasMaterialReference = false;

    const mdl::MorphRecord& driver = model.morphs[group];
    for (int k = 0; k < driver.groupCount; ++k) {
        const mdl::PmxGroupMorphEntry& reference = driver.groupEntries[k];
        if (reference.morphIndex < 0 ||
            reference.morphIndex >= static_cast<int>(model.morphCount))
            continue;
        const mdl::MorphRecord& target = model.morphs[reference.morphIndex];
        const float weight = groupValue * reference.weight;
        if (target.type == 1) {
            for (int e = 0; e < target.offsetCount; ++e) {
                const mdl::PmdVertexMorphEntry& entry = target.vertexEntries[e];
                if (entry.vertexIndex >= model.vertexCount)
                    continue;
                float* dst = &geometry[static_cast<std::size_t>(
                                          entry.vertexIndex) *
                                      kGeometryFloats];
                for (int c = 0; c < 3; ++c)
                    dst[c] += entry.offset[c] * weight;
            }
        } else if (target.type >= 3 && target.type <= 7) {
            const int family = target.type - 3;
            const int count = target.uvCounts[family];
            const mdl::PmxUvMorphEntry* entries = target.uvEntries[family];
            for (int e = 0; e < count; ++e) {
                const mdl::PmxUvMorphEntry& entry = entries[e];
                if (entry.vertexIndex < 0 ||
                    static_cast<std::uint32_t>(entry.vertexIndex) >=
                        model.vertexCount)
                    continue;
                float* dst = &geometry[static_cast<std::size_t>(
                                          entry.vertexIndex) *
                                      kGeometryFloats];
                if (family == 0) {
                    dst[3] += entry.offset[0] * weight;
                    dst[4] += entry.offset[1] * weight;
                } else {
                    for (int c = 0; c < 4; ++c)
                        dst[5 + 4 * (family - 1) + c] += entry.offset[c] * weight;
                }
            }
        } else if (target.type == 8) {
            if (mdl::MaterialMorphBase(raw) == nullptr)
                continue;
            hasMaterialReference = true;
            for (int e = 0; e < target.materialCount; ++e) {
                const mdl::PmxMaterialMorphEntry& entry =
                    target.materialEntries[e];
                const int last = entry.materialIndex < 0
                                     ? static_cast<int>(materialCount) - 1
                                     : entry.materialIndex;
                const int first = entry.materialIndex < 0 ? 0 : last;
                for (int mi = first;
                     mi >= 0 && mi <= last && mi < static_cast<int>(materialCount);
                     ++mi) {
                    float* add =
                        &additive[static_cast<std::size_t>(mi) * kMaterialFloats];
                    float* mul = &multiplicative[static_cast<std::size_t>(mi) *
                                                 kMaterialFloats];
                    for (int c = 0; c < kMaterialFloats; ++c) {
                        const float delta = MaterialChannel(entry.channels, c);
                        if (entry.operation != 0)
                            add[c] += delta * weight;
                        else
                            mul[c] = (1.0f - (1.0f - delta) * weight) * mul[c];
                    }
                }
            }
        }
    }

    if (hasMaterialReference) {
        auto* basePool = mdl::MaterialMorphBase(raw);
        for (std::size_t mi = 0; mi < materialCount; ++mi) {
            for (int c = 0; c < kMaterialFloats; ++c) {
                materials[mi * kMaterialFloats + c] =
                    multiplicative[mi * kMaterialFloats + c] *
                        MaterialChannel(basePool[mi].channels, c) +
                    additive[mi * kMaterialFloats + c];
            }
        }
    }
}

int CountDifferent(const std::vector<float>& a, const std::vector<float>& b,
                   std::size_t* firstIndex) {
    if (a.size() != b.size())
        return -1;
    int different = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const float magnitude =
            std::fabs(a[i]) > std::fabs(b[i]) ? std::fabs(a[i]) : std::fabs(b[i]);
        const float tolerance = 1.0e-5f * (magnitude > 1.0f ? magnitude : 1.0f);
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
            std::fabs(a[i] - b[i]) > tolerance) {
            if (different == 0 && firstIndex != nullptr)
                *firstIndex = i;
            ++different;
        }
    }
    return different;
}

// Finds the group morph that references `target` and reports its reference
// weight. The loaded morph tables decide this, not a hardcoded assumption.
bool FindGroupReference(const mdl::ModelRecord& model, int target, int* group,
                        float* weight) {
    for (std::uint32_t g = 0; g < model.morphCount; ++g) {
        const mdl::MorphRecord& morph = model.morphs[g];
        if (morph.type != 0)
            continue;
        for (int k = 0; k < morph.groupCount; ++k) {
            if (morph.groupEntries[k].morphIndex == target) {
                *group = static_cast<int>(g);
                *weight = morph.groupEntries[k].weight;
                return true;
            }
        }
    }
    return false;
}

// Driving the group must reach the hidden morph and match the expectation built
// from the morph tables.
void CheckGroupDrives(MMDApp* app, unsigned char* raw, int morphIndex,
                      const char* context) {
    char message[256];
    auto& model = *mdl::Mdl(raw);
    std::snprintf(message, sizeof message, "%s: hidden morph %d exists", context,
                  morphIndex);
    Check(MorphInRange(model, morphIndex), message);
    if (!MorphInRange(model, morphIndex))
        return;

    int group = -1;
    float weight = 0.0f;
    std::snprintf(message, sizeof message,
                  "%s: hidden morph %d is referenced by a group morph", context,
                  morphIndex);
    const bool referenced =
        FindGroupReference(model, morphIndex, &group, &weight) && weight != 0.0f;
    Check(referenced, message);
    if (!referenced)
        return;

    const float contribution = 0.6f;  // the value the reference contributes
    const float groupValue = contribution / weight;

    SetAllMorphValues(model, 0.0f);
    std::vector<float> baseGeometry;
    std::vector<float> baseMaterials;
    if (!CaptureGeometry(app, raw, baseGeometry)) {
        std::snprintf(message, sizeof message,
                      "%s: production geometry pass available for hidden morph %d",
                      context, morphIndex);
        Check(false, message);
        return;
    }
    CaptureMaterials(raw, baseMaterials);

    model.morphs[group].value = groupValue;  // the hidden morph stays at zero
    std::vector<float> drivenGeometry;
    std::vector<float> drivenMaterials;
    if (!CaptureGeometry(app, raw, drivenGeometry)) {
        std::snprintf(message, sizeof message,
                      "%s: production geometry pass available while driving "
                      "group %d",
                      context, group);
        Check(false, message);
        SetAllMorphValues(model, 0.0f);
        return;
    }
    CaptureMaterials(raw, drivenMaterials);

    std::vector<float> expectedGeometry;
    std::vector<float> expectedMaterials;
    BuildGroupExpectation(raw, group, groupValue, baseGeometry, baseMaterials,
                          expectedGeometry, expectedMaterials);
    SetAllMorphValues(model, 0.0f);

    // The driven pass must differ from the idle pass, otherwise an unchanged
    // table could pass the comparison below.
    const int hiddenType = model.morphs[morphIndex].type;
    std::size_t firstDelta = 0;
    if (hiddenType == 8) {
        const int moved =
            CountDifferent(baseMaterials, drivenMaterials, &firstDelta);
        std::snprintf(message, sizeof message,
                      "%s: group %d changes the material result of hidden morph %d",
                      context, group, morphIndex);
        Check(moved > 0, message);
    } else {
        const int moved =
            CountDifferent(baseGeometry, drivenGeometry, &firstDelta);
        std::snprintf(message, sizeof message,
                      "%s: group %d changes the vertex/UV result of hidden morph "
                      "%d",
                      context, group, morphIndex);
        Check(moved > 0, message);
    }

    std::size_t firstGeometryMismatch = 0;
    const int geometryMismatch =
        CountDifferent(expectedGeometry, drivenGeometry, &firstGeometryMismatch);
    std::snprintf(message, sizeof message,
                  "%s: group %d vertex/UV result matches the morph-table "
                  "expectation for hidden morph %d",
                  context, group, morphIndex);
    Check(geometryMismatch == 0, message);
    if (geometryMismatch != 0)
        std::printf("geometry mismatches=%d first=%llu\n", geometryMismatch,
                    static_cast<unsigned long long>(firstGeometryMismatch));

    std::size_t firstMaterialMismatch = 0;
    const int materialMismatch = CountDifferent(expectedMaterials,
                                                drivenMaterials,
                                                &firstMaterialMismatch);
    std::snprintf(message, sizeof message,
                  "%s: group %d material result matches the morph-table "
                  "expectation for hidden morph %d",
                  context, group, morphIndex);
    Check(materialMismatch == 0, message);
    if (materialMismatch != 0)
        std::printf("material mismatches=%d first=%llu\n", materialMismatch,
                    static_cast<unsigned long long>(firstMaterialMismatch));

    // Drive the same result through registered motion and the playback cursor,
    // not only a direct write to the live group weight. Put keys beyond any
    // ordinary-control keys above, so earlier tracks cannot contaminate it.
    for (unsigned i = 0; i < model.morphCount; ++i) {
        model.morphs[i].value = 0;
        RegisterMorphKeyCurrent(raw, i, 40);
        RegisterMorphKeyCurrent(raw, i, 60);
    }
    model.morphs[group].value = 2 * groupValue;
    RegisterMorphKeyCurrent(raw, group, 60);
    InitModelTrackCursors(raw, 40.0f / 30.0f, 0);
    AdvanceModelKeyframes(raw, 50.0f / 30.0f, 0);
    Check(std::fabs(model.morphs[group].value - groupValue) < 1e-6f &&
              model.morphs[morphIndex].value == 0,
          "playback interpolates the group track while the hidden base weight stays zero");
    Check(CaptureGeometry(app, raw, drivenGeometry), "playback geometry available");
    CaptureMaterials(raw, drivenMaterials);
    Check(CountDifferent(expectedGeometry, drivenGeometry, nullptr) == 0 &&
              CountDifferent(expectedMaterials, drivenMaterials, nullptr) == 0,
          "playback preserves expected vertex/UV/material group effects");
    SetAllMorphValues(model, 0.0f);
    std::printf("group=%d hidden=%d direct/playback results checked\n", group, morphIndex);
}

// ---- sample anchors --------------------------------------------------------

struct HiddenAnchor {
    int index;
    const wchar_t* name;
    std::uint8_t type;
    int group;
};

const HiddenAnchor kReportedHidden[5] = {
    {17, L"\u6f70\u308c1", 1, 16},  // name_local 潰れ1, vertex morph
    {18, L"\u6f70\u308c2", 8, 16},  // name_local 潰れ2, material morph
    {20, L"\u6697\u76ee2", 3, 19},  // name_local 暗目2, UV morph
    {24, L"HL1_R", 3, 22},
    {25, L"HL2_R", 8, 22},
};

void CheckSampleAnchors(unsigned char* raw, const char* context) {
    char message[256];
    auto& model = *mdl::Mdl(raw);
    for (const HiddenAnchor& anchor : kReportedHidden) {
        std::printf("anchor morph=%d name=%s type=%u panel=%d group=%d\n",
                    anchor.index, Utf8(anchor.name).c_str(),
                    static_cast<unsigned>(anchor.type),
                    static_cast<std::uint32_t>(anchor.index) < model.morphCount
                        ? PanelNumber(model.morphs[anchor.index])
                        : -1,
                    anchor.group);

        std::snprintf(message, sizeof message, "%s: morph %d exists", context,
                      anchor.index);
        Check(static_cast<std::uint32_t>(anchor.index) < model.morphCount,
              message);
        if (static_cast<std::uint32_t>(anchor.index) >= model.morphCount)
            continue;
        const mdl::MorphRecord& morph = model.morphs[anchor.index];
        std::snprintf(message, sizeof message,
                      "%s: morph %d keeps the reported name", context,
                      anchor.index);
        Check(text_encoding::MorphName(morph, false) == anchor.name, message);
        std::snprintf(message, sizeof message,
                      "%s: morph %d keeps the reported panel 0", context,
                      anchor.index);
        Check(PanelNumber(morph) == 0, message);
        std::snprintf(message, sizeof message,
                      "%s: morph %d keeps the reported deform type %u", context,
                      anchor.index, static_cast<unsigned>(anchor.type));
        Check(morph.type == anchor.type, message);

        int group = -1;
        float weight = 0.0f;
        std::snprintf(message, sizeof message,
                      "%s: group morph %d references hidden morph %d", context,
                      anchor.group, anchor.index);
        Check(FindGroupReference(model, anchor.index, &group, &weight) &&
                  group == anchor.group,
              message);

        bool inDisplayFrame = false;
        for (unsigned f = 0; f < model.facialFrameCount; ++f)
            if (model.displayFrames[f].targetIndex == anchor.index)
                inDisplayFrame = true;
        std::snprintf(message, sizeof message,
                      "%s: morph %d is referenced by a display frame", context,
                      anchor.index);
        Check(inDisplayFrame, message);
    }
}

// ---- panel substitution contract -------------------------------------------

// Relabels a morph the file puts in a normal lane as panel 0 / 5 / 255 in
// memory, rebuilds the four combos through the production PostLoadInit, and
// repeats the hidden-morph click contract. The file is never touched.
void CheckPanelSubstitution(MMDApp* app, unsigned char* raw) {
    char message[256];
    auto& model = *mdl::Mdl(raw);
    int subject = -1;
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        if (PanelNumber(model.morphs[i]) == 2) {
            subject = static_cast<int>(i);
            break;
        }
    }
    std::snprintf(message, sizeof message,
                  "panel substitution: a panel 2 morph is available as subject");
    Check(subject >= 0, message);
    if (subject < 0)
        return;

    const int originalPanel = PanelNumber(model.morphs[subject]);
    for (int panel : {0, 5, 255}) {
        model.morphs[subject].panel = static_cast<mdl::MorphPanel>(panel);
        PostLoadInit(raw);

        std::snprintf(message, sizeof message,
                      "panel %d: subject morph %d leaves the four morph lanes",
                      panel, subject);
        bool inAnyLane = false;
        for (int lane = 0; lane < 4; ++lane)
            inAnyLane |= LaneContainsMorph(app, lane, subject);
        Check(!inAnyLane, message);

        CheckLaneContents(app, raw, "panel substitution");

        // The rebuilt combos must still be intact after the real click sequence
        // on a morph they no longer carry.
        model.morphs[subject].value = 0.5f;
        CheckHiddenMorphClick(app, raw, subject, "panel substitution", false, 2);
        std::snprintf(message, sizeof message,
                      "panel %d: subject morph %d keeps its value", panel,
                      subject);
        Check(model.morphs[subject].value == 0.5f, message);
        model.morphs[subject].value = 0.0f;

        model.morphs[subject].panel = static_cast<mdl::MorphPanel>(originalPanel);
        PostLoadInit(raw);
    }
}

// ---- modes -----------------------------------------------------------------

void ContractMode(MMDApp* app, unsigned char* raw) {
    char message[256];
    auto& model = *mdl::Mdl(raw);

    // 1. the reported sample anchors
    const int anchorFailures = failures;
    CheckSampleAnchors(raw, "sample anchors");
    if (failures != anchorFailures)
        return;

    // 2. the four lanes only carry their own panel, so no hidden morph is there
    CheckLaneContents(app, raw, "loaded sample");

    // 3. the facial display group must be expanded before its morph rows exist
    mdl::DisplayGroup* groups = mdl::DisplayGroups(raw);
    std::snprintf(message, sizeof message,
                  "display group 2 is the facial group (name 表情)");
    Check(groups != nullptr && model.groupCount >= 3 &&
              text_encoding::DisplayGroupName(groups[2], false) ==
                  L"\u8868\u60c5",
          message);
    EnsureFacialGroupExpanded(app, raw);

    // 4. every reported hidden morph is a row of the production table
    for (const HiddenAnchor& anchor : kReportedHidden) {
        if (!MorphInRange(model, anchor.index))
            continue;
        std::snprintf(message, sizeof message,
                      "hidden morph %d is listed in the label table",
                      anchor.index);
        Check(FindMorphRow(model, anchor.index) >= 0, message);
    }

    for (const auto& anchor : kReportedHidden)
        if (MorphInRange(model, anchor.index))
            model.morphs[anchor.index].value = 0.37f;

    // 5. click contract on each hidden morph: a plain and a Shift sequence for
    //    every morph, plus a repeated sequence on the reported 潰れ1 row
    for (const HiddenAnchor& anchor : kReportedHidden) {
        if (!MorphInRange(model, anchor.index))
            continue;
        CheckHiddenMorphClick(app, raw, anchor.index, "reported sample", false, 1);
        CheckHiddenMorphClick(app, raw, anchor.index, "reported sample", true, 1);
    }
    if (MorphInRange(model, 17))
        CheckHiddenMorphClick(app, raw, 17, "reported sample repeated", false, 3);

    // Follow hidden selections with real timeline/slider/register actions in
    // all four ordinary lanes. Each lane must retain the other three controls.
    constexpr int registerButton[4] = {525, 524, 527, 526};
    for (int lane = 0; lane < 4; ++lane) {
        const auto initialLane = CaptureLane(app, lane);
        Check(!initialLane.items.empty(), "ordinary lane has a morph");
        if (initialLane.items.empty())
            continue;
        const int ordinary = initialLane.items.back();
        for (int i = 0; i < 5; ++i)
            model.morphs[kReportedHidden[i].index].value = 0.5f + 0.05f * i;
        model.morphs[ordinary].value = 0.25f;
        model.boneListPos = 0;
        PostLanguageSweep(app);
        const int fullRow = FindMorphRow(model, ordinary);
        const int row = fullRow >= 0 ? ScrollToVisibleRow(app, raw, fullRow) : -1;
        Check(row >= 0 && FindMorphRow(model, ordinary) == row,
              "ordinary morph reaches a clickable row");
        if (row < 0 || FindMorphRow(model, ordinary) != row)
            continue;
        const ContractState before = CaptureContractState(app, raw);
        SendClickSequence(app, kNameColumnX, RowY(row), false);
        const LaneState after = CaptureLane(app, lane);
        Check(model.selectedMorphs[lane] == ordinary && after.itemData == ordinary,
              "timeline click selects ordinary morph in its own lane");
        Check(after.edit == "0.2500" && after.slider == 25,
              "timeline click synchronizes ordinary weight controls");
        for (int other = 0; other < 4; ++other)
            if (other != lane)
                Check(SameLane(before.lane[other], CaptureLane(app, other)) &&
                          model.selectedMorphs[other] == before.selectedMorphs[other],
                      "ordinary click preserves other lanes");
        const HWND slider = GetDlgItem(app->MainWindow(), kMorphSlider[lane]);
        SendMessageW(slider, TBM_SETPOS, TRUE, 65);
        SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBTRACK,
                     reinterpret_cast<LPARAM>(slider));
        Check(std::fabs(model.morphs[ordinary].value - 0.65f) < 1e-6f &&
                  CaptureLane(app, lane).edit == "0.6500",
              "ordinary slider updates the selected morph weight");
        for (const auto& anchor : kReportedHidden)
            Check(model.morphs[anchor.index].value == before.values[anchor.index],
                  "ordinary selection and weight editing preserve hidden weights");
        app->CurrentFrame() = 10 + lane;
        SendMessageW(app->MainWindow(), WM_COMMAND,
                     MAKEWPARAM(registerButton[lane], BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(app->MainWindow(), registerButton[lane])));
        model.morphs[ordinary].value = 0;
        SeekModelFrame(raw, app->CurrentFrame(), 0);
        Check(std::fabs(model.morphs[ordinary].value - 0.65f) < 1e-6f,
              "ordinary register button stores a key recoverable by frame seek");
        std::printf("ordinary panel=%d morph=%d selection/slider/key checked\n", lane + 1, ordinary);
    }

    // 7. group-driven vertex / UV / material evaluation
    for (const HiddenAnchor& anchor : kReportedHidden)
        CheckGroupDrives(app, raw, anchor.index, "group evaluation");

    // 8. panel substitution 0 / 5 / 255
    CheckPanelSubstitution(app, raw);
}

void BoundsMode(MMDApp* app, unsigned char* raw) {
    CheckLaneContents(app, raw, "bounds baseline");
    EnsureFacialGroupExpanded(app, raw);
    CheckPanelSubstitution(app, raw);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);

    std::wstring modelPath;
    std::wstring mode = L"contract";
    bool modelGiven = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--model") == 0 && i + 1 < argc) {
            modelPath = argv[++i];
            modelGiven = true;
        } else if (wcscmp(argv[i], L"--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else {
            std::printf("usage: hidden_morph_tests --model PATH "
                        "[--mode contract|bounds]\n");
            return 2;
        }
    }
    if (mode != L"contract" && mode != L"bounds") {
        std::printf("unknown mode: %s\n", Utf8(mode.c_str()).c_str());
        return 2;
    }
    if (!modelGiven || GetFileAttributesW(modelPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("FAIL: model sample not found: %s\n",
                    Utf8(modelPath.c_str()).c_str());
        std::printf("pass --model PATH to point at a PMX sample\n");
        return 1;
    }
    std::printf("model=%s%s\n", Utf8(modelPath.c_str()).c_str(),
                modelGiven ? "" : " (default)");
    std::printf("mode=%s\n", Utf8(mode.c_str()).c_str());

    const auto hideHook =
        SetWindowsHookExW(WH_CBT, HideHook, nullptr, GetCurrentThreadId());
    const auto dialogHook = SetWindowsHookExW(WH_CALLWNDPROCRET, DialogHook,
                                              nullptr, GetCurrentThreadId());
    if (hideHook == nullptr || dialogHook == nullptr) {
        std::printf("FAIL: background window hooks unavailable\n");
        return 3;
    }

    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->InitDefaults();
    g_Block = app.get();
    if (!InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE)) {
        std::printf("FAIL: production startup failed\n");
        g_Block = nullptr;
        return 3;
    }
    ShowWindow(app->MainWindow(), SW_HIDE);
    const int startupDialogs = dialogs;

    LoadModelFile(app.get(), modelPath.c_str(), false);
    app->SetSelectedModelSlot(0);
    unsigned char* raw = app->ModelSlot(0);
    std::printf("load dialogs=%d\n", dialogs - startupDialogs);
    Check(dialogs == 0, "startup and sample loading need no modal dialogs");
    if (raw == nullptr || mdl::Mdl(raw)->vertexCount == 0 ||
        mdl::Mdl(raw)->morphCount == 0) {
        std::printf("FAIL: the sample did not load into slot 0\n");
        ShutdownCleanup(app.get());
        g_Block = nullptr;
        return 4;
    }
    std::printf("morphs=%u faces=%u groups=%u vertices=%u materials=%u\n",
                mdl::Mdl(raw)->morphCount,
                static_cast<unsigned>(mdl::Mdl(raw)->facialFrameCount),
                static_cast<unsigned>(mdl::Mdl(raw)->groupCount),
                mdl::Mdl(raw)->vertexCount, mdl::Mdl(raw)->materialCount);

    // Deterministic focus: the production press handler re-focuses the main
    // window, and a control that loses focus would commit its edit text first.
    SetFocus(app->MainWindow());
    Check(app->state.optflag[0] == 0,
          "bone-edit panel active, so the press takes the name column path");

    RECT client{};
    GetClientRect(app->MainWindow(), &client);
    std::printf("client=%dx%d\n", static_cast<int>(client.right),
                static_cast<int>(client.bottom));

    if (mode == L"contract")
        ContractMode(app.get(), raw);
    else
        BoundsMode(app.get(), raw);

    std::printf("checks=%d failures=%d\n", checks, failures);
    std::printf("hidden_morph %s: %s\n", Utf8(mode.c_str()).c_str(),
                failures == 0 ? "PASS" : "FAIL");

    ShutdownCleanup(app.get());
    g_Block = nullptr;
    if (dialogHook != nullptr)
        UnhookWindowsHookEx(dialogHook);
    if (hideHook != nullptr)
        UnhookWindowsHookEx(hideHook);
    return failures == 0 ? 0 : 1;
}
