#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../plugins/native_pose_scope.hpp"
#endif
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/ui_language.hpp"
// ===========================================================================
// Timeline-driven Light / Self Shadow panel refresh.
//   0x00411070  RefreshLightPanel (was Sub411070): evaluate the light key
//               list and synchronize controls 455..466
//   0x00411B90  RefreshSelfShadowPanel (was Sub411B90): evaluate the shadow
//               key list and synchronize controls 560..564
//   0x004134E0  SyncAccessoryEditPanel (was Sub4134E0): reload the accessory
//               edit controls 474..486 from the selected accessory
//   0x00411DF0  RegisterSelfShadowState (was Sub411DF0): insert/overwrite the
//               self-shadow key at a frame with the current mode + interval
//               (the SelfShadow sibling of RegisterCameraState/RegisterLightState)
// ===========================================================================
#include "mikumikustudio/animation_curve_host.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include "mikumikustudio/ported_funcs.hpp"

#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_control_readout.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "accessory_bone_combo_cache.hpp"

namespace mikumikustudio {
namespace {

// Porting-era trace under MIKUMIKUSTUDIO_LIGHT_TRACE_DIR (CMake option
// MIKUMIKUSTUDIO_DIAG, default OFF); the OFF stub keeps the call sites
// (including the cross-TU TraceSceneLightState) valid and inlines away to
// nothing.
#ifdef MIKUMIKUSTUDIO_DIAG
void WriteLightTrace(MMDApp* app, const char* stage) {
    const char* directory = std::getenv("MIKUMIKUSTUDIO_LIGHT_TRACE_DIR");
    if (directory == nullptr || directory[0] == '\0')
        return;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\light_state.jsonl", directory);
    FILE* stream = nullptr;
    if (fopen_s(&stream, path, "ab") != 0 || stream == nullptr)
        return;
    const D3DLIGHT9& light = app->SceneLight();
    std::fprintf(stream,
        "{\"stage\":\"%s\",\"type\":%u,\"direction\":[%.9g,%.9g,%.9g],"
        "\"specular\":[%.9g,%.9g,%.9g],\"ambient\":[%.9g,%.9g,%.9g]}\\n",
        stage, static_cast<unsigned>(light.Type), light.Direction.x,
        light.Direction.y, light.Direction.z, light.Specular.r,
        light.Specular.g, light.Specular.b, light.Ambient.r,
        light.Ambient.g, light.Ambient.b);
    fclose(stream);
}
#else
inline void WriteLightTrace(MMDApp*, const char*) {}
#endif

bool KeepPendingBoundEdit(MMDApp* app, HWND edit) noexcept {
#if !MMD_ORIGINAL_PROFILE
    return app && plugins::BoundNativePoseHistory(app) && edit &&
        SendMessageW(edit, EM_GETMODIFY, 0, 0) != 0;
#else
    (void)app;
    (void)edit;
    return false;
#endif
}

void ReplaceEditText(MMDApp* app, HWND main, int id, const char* text) {
    HWND edit = GetDlgItem(main, id);
    if (KeepPendingBoundEdit(app, edit) || ui::ReadoutTextMatchesA(edit, text)) return;
    const LRESULT length = GetWindowTextLengthA(edit);
    SendMessageA(edit, EM_SETSEL, 0, length);
    SendMessageA(edit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text));
#if !MMD_ORIGINAL_PROFILE
    // EM_REPLACESEL marks the edit as modified even for a host readout.
    // Mark the completed host write as a readout, including initialization
    // before binding. A real pending bound edit returned before this write.
    if (ui::ReadoutTextMatchesA(edit, text))
        SendMessageW(edit, EM_SETMODIFY, FALSE, 0);
#endif
}

void FormatLegacyOneDecimal(char* text, std::size_t size, float value) {
    const double magnitude = std::floor(std::fabs(
        static_cast<double>(value)) * 10.0 + 0.5) / 10.0;
    const double rounded = std::signbit(value) ? -magnitude : magnitude;
    sprintf_s(text, size, "%+3.1f", rounded);
}

// Compare the actual list as well as the parent row. Model slots and combo
// rows have different identities, and labels/bone filtering can change while
// the selected parent row stays the same. The optional cache is window-owned,
// observes all list mutations, and still compares live source contents.
bool AccessoryBoneComboMatches(HWND combo, const mdl::ModelRecord* model,
                               bool english) {
    if (combo == nullptr)
        return true;
    auto cache = accessory_bone_combo::Find(combo, true);
    if (!cache || cache->busy) return false;
    if (cache && cache->valid && cache->source.Matches(model, english)) return true;
    accessory_bone_combo::Guard guard(cache);
    accessory_bone_combo::Source source;
    const bool captured = cache && accessory_bone_combo::Source::Capture(model, english, source);
    const auto mutations = cache ? cache->mutations : 0;
    LRESULT row = 0;
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    if (count < 0 || !guard.Alive())
        return false;
    if (model != nullptr && model->boneTable != nullptr) {
        for (std::uint32_t bone = 0; bone < model->boneCount; ++bone) {
            const auto& record = model->boneTable[bone];
            if (record.type >= mdl::BoneType::InertTip &&
                record.type != mdl::BoneType::FixedAxis)
                continue;
            const auto label = text_encoding::BoneLabel(record, english);
            if (row >= count ||
                SendMessageW(combo, CB_GETITEMDATA, row, 0) != bone || !guard.Alive())
                return false;
            accessory_bone_combo::ReadGuard read(cache);
            if (SendMessageW(combo, CB_GETLBTEXTLEN, row, 0) !=
                static_cast<LRESULT>(label.size()) || !guard.Alive())
                return false;
            std::wstring actual(label.size() + 1, L'\0');
            if (SendMessageW(combo, CB_GETLBTEXT, row,
                    reinterpret_cast<LPARAM>(actual.data())) !=
                    static_cast<LRESULT>(label.size()) ||
                !guard.Alive() || cache->mutations != mutations ||
                actual.compare(0, label.size(), label) != 0)
                return false;
            if (captured && (model->boneTable != source.table || model->boneCount != source.count)) return false;
            ++row;
        }
    }
    if (!guard.Alive() || row != count || (cache && cache->mutations != mutations)) return false;
    if (captured) {
        if (!source.Matches(model, english)) return false;
        cache->Remember(std::move(source), mutations);
    }
    return true;
}

bool RebuildAccessoryBoneCombo(HWND combo, const mdl::ModelRecord* model,
                                bool english) {
    if (combo == nullptr)
        return true;
    auto cache = accessory_bone_combo::Find(combo, true);
    if (!cache || cache->busy) return false;
    accessory_bone_combo::Guard guard(cache);
    struct Row { std::wstring label; std::uint32_t bone; };
    std::vector<Row> rows;
    if (model != nullptr && model->boneTable != nullptr) {
        rows.reserve(model->boneCount);
        for (std::uint32_t bone = 0; bone < model->boneCount; ++bone) {
            const auto& record = model->boneTable[bone];
            if (record.type >= mdl::BoneType::InertTip &&
                record.type != mdl::BoneType::FixedAxis)
                continue;
            rows.push_back({text_encoding::BoneLabel(record, english), bone});
        }
    }
    accessory_bone_combo::Source source;
    const bool captured = cache && accessory_bone_combo::Source::Capture(model, english, source);
    // Complete all C++ allocation before retiring the displayed list. A native
    // control can still run out of space while publishing; leave its selection
    // empty and retry on the next content/parent mismatch in that case.
    std::size_t longest = 0;
    for (const auto& row : rows) longest = (std::max)(longest, row.label.size());
    std::wstring actual(longest + 1, L'\0');
    if (SendMessageW(combo, CB_RESETCONTENT, 0, 0) == CB_ERR || !guard.Alive())
        return false;
    for (const auto& row : rows) {
        const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(row.label.c_str()));
        if (!guard.Alive() || index == CB_ERR || index == CB_ERRSPACE ||
            SendMessageW(combo, CB_SETITEMDATA, index, row.bone) == CB_ERR || !guard.Alive())
            return false;
    }
    // Native callbacks can change the list during publication. Only cache a
    // complete, verified result; any failed/partial attempt remains invalid.
    const auto mutations = cache ? cache->mutations : 0;
    if (SendMessageW(combo, CB_GETCOUNT, 0, 0) != static_cast<LRESULT>(rows.size()) || !guard.Alive()) return false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (SendMessageW(combo, CB_GETITEMDATA, i, 0) != row.bone || !guard.Alive()) return false;
        accessory_bone_combo::ReadGuard read(cache);
        if (SendMessageW(combo, CB_GETLBTEXTLEN, i, 0) != static_cast<LRESULT>(row.label.size()) || !guard.Alive()) return false;
        if (SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(actual.data())) != static_cast<LRESULT>(row.label.size()) ||
            !guard.Alive() || cache->mutations != mutations ||
            actual.compare(0, row.label.size(), row.label) != 0) return false;
    }
    if (!guard.Alive() || (cache && cache->mutations != mutations)) return false;
    if (captured) {
        if (!source.Matches(model, english)) return false;
        cache->Remember(std::move(source), mutations);
    }
    return true;
}

void SelectAccessoryBoneIfChanged(HWND combo, int bone) {
    const LRESULT current = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (bone >= 0 && current >= 0 && SendMessageW(combo, CB_GETITEMDATA, current, 0) == bone) return;
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    LRESULT target = -1;
    for (LRESULT row = 0; row < count; ++row) {
        if (SendMessageW(combo, CB_GETITEMDATA, row, 0) == bone) {
            target = row;
            break;
        }
    }
    if (current != target)
        SendMessageW(combo, CB_SETCURSEL, target, 0);
}

template <typename Key>
std::uint32_t FindKeyAtOrAfter(const Key* keys,
                               std::uint32_t frame) {
    std::uint32_t index = 0;
    if (keys[0].frame < frame) {
        for (;;) {
            const std::uint32_t next = keys[index].next;
            if (next == 0)
                break;
            index = next;
            if (keys[index].frame >= frame)
                break;
        }
    }
    return index;
}

}  // namespace

void TraceSceneLightState(MMDApp* app, const char* stage) {
    if (app != nullptr)
        WriteLightTrace(app, stage);
}

// was Sub411070, VA 0x00411070
void RefreshLightPanel(MMDApp* app) {
    if(app && app->CurrentFrame()<0) {
        const auto frame=app->CurrentFrame();app->CurrentFrame()=0;RefreshLightPanel(app);app->CurrentFrame()=frame;
        negative_frames::EvaluateGlobals(app,frame);return;
    }
    if (app == nullptr)
        return;
    auto* keys = app->LightKeys();
    if (keys == nullptr)
        return;

    const std::uint32_t frame = app->state.currentFrame;
    const std::uint32_t index = FindKeyAtOrAfter(keys, frame);
    const auto& right = keys[index];

    float direction[3];
    float color[3];
    // When the walk reaches the end of the chain, the original copies the
    // terminal key verbatim.  Interpolating it against its own prev index
    // produces a zero denominator (and poisoned the light register payload
    // with NaNs in the previous approximation).
    if (right.frame <= frame) {
        direction[0] = right.direction[0];
        direction[1] = right.direction[1];
        direction[2] = right.direction[2];
        color[0] = right.color[0];
        color[1] = right.color[1];
        color[2] = right.color[2];
    } else {
        const auto& left = keys[right.previous];
        const double amount =
            static_cast<double>(frame - left.frame) /
            static_cast<double>(right.frame - left.frame);
        for (int lane = 0; lane < 3; ++lane) {
            direction[lane] = static_cast<float>(
                (right.direction[lane] - left.direction[lane]) * amount + left.direction[lane]);
            color[lane] = static_cast<float>(
                (right.color[lane] - left.color[lane]) * amount + left.color[lane]);
        }
    }

    std::memcpy(app->LightDirection(), direction, sizeof(direction));
    std::memcpy(app->LightColor(), color, sizeof(color));
    curve_host::Apply(app, curve_host::Kind::Light, frame);
    std::memcpy(direction, app->LightDirection(), sizeof(direction));
    std::memcpy(color, app->LightColor(), sizeof(color));
    app->ApplyTimelineLightState();

    // Original 0x411201..0x41121F: IDirect3DDevice9::SetLight(0,
    // reinterpret_cast<D3DLIGHT9*>(app+0x9E180)).
    D3DRenderer* wrapper = app->Renderer();
    if (wrapper != nullptr) {
        IDirect3DDevice9* device = wrapper->device;
        if (device != nullptr)
            device->SetLight(0, &app->SceneLight());
    }
    TraceSceneLightState(app, "timeline-light-refresh");

    const HWND main = app->state.hwnd;
    char text[0x34];
    for (int lane = 0; lane < 3; ++lane) {
        ui::SetTrackbarPositionIfChanged(GetDlgItem(main, 455 + lane),
            static_cast<int>(color[lane] * 256.0f));
        ui::SetTrackbarPositionIfChanged(GetDlgItem(main, 458 + lane),
            static_cast<int>(direction[lane] * 100.0f));

        sprintf_s(text, sizeof(text), "%3d",
                  static_cast<int>(color[lane] * 256.0f));
        ReplaceEditText(app, main, 461 + lane, text);
        FormatLegacyOneDecimal(text, sizeof(text), direction[lane]);
        ReplaceEditText(app, main, 464 + lane, text);
    }
}

// Echo the evaluated angle without writing the rounded slider value back.
void SyncCameraFovControls(MMDApp* app) {
    if(app==nullptr||!std::isfinite(app->CameraFov()))return;
    const HWND main=app->MainWindow();
    char text[32],previous[32]{};
    sprintf_s(text,"%.6g",static_cast<double>(app->CameraFov()));
    const HWND edit=GetDlgItem(main,panel::kFovEdit);
    GetWindowTextA(edit,previous,sizeof(previous));
    if(!KeepPendingBoundEdit(app,edit)&&std::strcmp(text,previous)!=0)SetWindowTextA(edit,text);
    const HWND slider=GetDlgItem(main,panel::kFovSlider);
    const int position=static_cast<int>(std::lround(std::clamp(app->CameraFov(),1.f,125.f)));
    if(SendMessageA(slider,TBM_GETPOS,0,0)!=position)
        SendMessageA(slider,TBM_SETPOS,TRUE,position);
}

// was Sub411B90, VA 0x00411B90
void RefreshSelfShadowPanel(MMDApp* app) {
    if(app && app->CurrentFrame()<0) {
        const auto frame=app->CurrentFrame();app->CurrentFrame()=0;RefreshSelfShadowPanel(app);app->CurrentFrame()=frame;
        negative_frames::EvaluateGlobals(app,frame);return;
    }
    if (app == nullptr)
        return;
    auto* keys = app->ShadowKeys();
    if (keys == nullptr)
        return;

    const std::uint32_t frame = app->state.currentFrame;
    const std::uint32_t index = FindKeyAtOrAfter(keys, frame);
    const auto* record = &keys[index];
    if (record->frame > frame)
        record = &keys[record->previous];

    app->SelfShadowMode() =
        static_cast<std::int8_t>(record->mode);
    app->state.physicsInterval =
        record->distance;
    curve_host::Apply(app, curve_host::Kind::Shadow, frame);
    SyncSelfShadowControls(app);
}

void SyncSelfShadowControls(MMDApp* app) {
    if(app==nullptr)return;

    const int range = ShadowSliderPosition(app);

    const HWND main = app->state.hwnd;
    const HWND slider=GetDlgItem(main,panel::kSelfShadowRangeSlider);
    if(SendMessageA(slider,TBM_GETPOS,0,0)!=range) {
        SendMessageA(slider,TBM_SETPOS,FALSE,range);
        // Timeline seeking can keep the render/message pump busy. Repaint the
        // whole track immediately, including the old thumb's background.
        RedrawWindow(slider,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
    }
    char text[0x34];
    sprintf_s(text, sizeof(text), "%d", range);
    const HWND edit=GetDlgItem(main,panel::kSelfShadowRangeEdit);
    char previous[sizeof(text)]{};GetWindowTextA(edit,previous,sizeof(previous));
    if(!KeepPendingBoundEdit(app,edit)&&std::strcmp(text,previous)!=0)SetWindowTextA(edit,text);

    const int mode = app->SelfShadowMode();
    const int ids[]={panel::kEditOffCheckbox,panel::kEditMode1Checkbox,panel::kEditMode2Checkbox};
    for(int index=0;index<3;++index){
        const bool checked=index==0?mode==0:index==1?mode==1:mode!=0&&mode!=1;
        const HWND button=GetDlgItem(main,ids[index]);
        const WPARAM state=checked?BST_CHECKED:BST_UNCHECKED;
        if(SendMessageA(button,BM_GETCHECK,0,0)!=state)SendMessageA(button,BM_SETCHECK,state,0);
    }
}

// was Sub4134E0, VA 0x004134E0 - reload the accessory edit panel
// (combos 474/475 + checkboxes 476/477/486 + pos/rot/scale/opacity edits)
// from the selected accessory slot.
void SyncAccessoryEditPanel(MMDApp* app) {
    if (app == nullptr)
        return;
    const std::uint8_t selected = app->state.selectedObjectSlot;
    mdl::AccessoryRecord* accessory = app->AccessorySlot(selected);
    if (accessory == nullptr)
        return;

    const HWND main = app->state.hwnd;
    HWND modelCombo = GetDlgItem(main, panel::kMainComboGround);
    const LRESULT oldModel = SendMessageA(modelCombo, CB_GETCURSEL, 0, 0);
    const std::int32_t parentSlot = accessory->parentModel;
    const auto* model = parentSlot >= 0 && parentSlot < kModelSlotCount
        ? mdl::Mdl(app->ModelSlot(parentSlot)) : nullptr;
    const auto order = model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
        const auto* object = app->ModelSlot(static_cast<int>(slot));
        return object ? int(mdl::Mdl(object)->comboSelIndex) : -1;
    });
    const LRESULT parentRow = model != nullptr ? model_order::FindRow(order, unsigned(parentSlot),
        SendMessageW(modelCombo, CB_GETCOUNT, 0, 0),
        [&](std::size_t row) noexcept { return SendMessageW(modelCombo, CB_GETITEMDATA, row, 0); }) : parentSlot < 0 ? 0 : -1;
    const HWND boneCombo = GetDlgItem(main, panel::kAttachBoneCombo);
    const bool english = UiUsesEnglishNames(app->EnglishUI());
    try {
        const bool rebuild = oldModel != parentRow ||
            !AccessoryBoneComboMatches(boneCombo, model, english);
        bool listReady = !rebuild || RebuildAccessoryBoneCombo(boneCombo, model, english);
        if (listReady && oldModel != parentRow && modelCombo != nullptr) {
            const LRESULT selectedRow = SendMessageW(modelCombo, CB_SETCURSEL, parentRow, 0);
            listReady = parentRow < 0 || selectedRow != CB_ERR;
        }
        if (listReady && model != nullptr)
            SelectAccessoryBoneIfChanged(boneCombo, accessory->parentBone);
    } catch (...) {
        // Readout restoration after a rejected/cancelled edit must continue
        // even if a label comparison or private list preparation runs out of
        // memory. Preparation has not changed either combo or its selection.
    }

    const int checkIds[] = {panel::kAccessoryVisibleCheckbox,
        panel::kAccessoryAddBlendCheckbox, panel::kAccessoryShadowCheckbox};
    const bool checkValues[] = {accessory->visible != 0,
        accessory->additiveBlend != 0, accessory->shadowEnabled != 0};
    for (int index = 0; index < 3; ++index) {
        const HWND checkbox = GetDlgItem(main, checkIds[index]);
        const WPARAM state = checkValues[index] ? BST_CHECKED : BST_UNCHECKED;
        if (SendMessageW(checkbox, BM_GETCHECK, 0, 0) != state)
            SendMessageW(checkbox, BM_SETCHECK, state, 0);
    }

    char text[104];
    for (int lane = 0; lane < 3; ++lane) {
        sprintf_s(text, sizeof(text), "%3.4f",
                  static_cast<double>(accessory->position[lane]));
        ReplaceEditText(app, main, 478 + lane, text);
        sprintf_s(text, sizeof(text), "%3.4f",
                  static_cast<double>(accessory->rotation[lane]) /
                      3.141592025756836 * 180.0);
        ReplaceEditText(app, main, 481 + lane, text);
    }
    sprintf_s(text, sizeof(text), "%3.4f",
              static_cast<double>(accessory->scale));
    ReplaceEditText(app, main, 484, text);
    sprintf_s(text, sizeof(text), "%3.2f",
              static_cast<double>(accessory->opacity));
    ReplaceEditText(app, main, 485, text);
}

// was Sub411DF0, VA 0x00411DF0 - self-shadow track's "register" backend:
// exact-hit overwrite or free-slot splice of the 24-byte shadow key carrying
// the current shadow mode (this+0xA0B10) and interval (this+0xA0B0C).
void RegisterSelfShadowState(MMDApp* app, int frameValue) {
    if(frameValue<0){negative_frames::RegisterGlobal(app,negative_frames::Global::Shadow,frameValue);return;}
    if (app == nullptr || frameValue < 0)
        return;
    auto* keys = app->ShadowKeys();
    if (keys == nullptr)
        return;
    const std::uint32_t frame = static_cast<std::uint32_t>(frameValue);
    const std::uint32_t current = FindKeyAtOrAfter(keys, frame);
    auto* currentRecord = &keys[current];

    if (currentRecord->frame == frame) {
        currentRecord->mode =
            static_cast<std::uint8_t>(app->SelfShadowMode());
        currentRecord->distance =
            app->state.physicsInterval;
        currentRecord->selected = 1;
        curve_host::NotifyLegacyRegistered(app, curve_host::Kind::Shadow, 0, frameValue);
        return;
    }

    std::uint32_t freeIndex = 0;
    if (!mdl::KeyPoolFreeSlot(app->ShadowKeys(), freeIndex)) {
        MessageBoxW(static_cast<HWND>(app->Hwnd()),
            app->EnglishUI() == 2 ? L"无法扩展关键帧存储，本次注册已取消。" :
            app->EnglishUI() == 0 ? L"メモリを確保できないため、登録を中止しました。" :
            L"Unable to allocate keyframe storage. Registration was cancelled.",
            L"MikuMikuStudio", MB_OK | MB_ICONERROR);
        return;
    }
    keys = app->ShadowKeys();
    currentRecord = &keys[current];
    auto* added = &keys[freeIndex];
    if (currentRecord->frame < frame) {
        currentRecord->next = freeIndex;
        added->previous = current;
    } else {
        const std::uint32_t previous = currentRecord->previous;
        keys[previous].next = freeIndex;
        added->previous = previous;
        currentRecord->previous = freeIndex;
        added->next = current;
    }
    added->frame = frame;
    added->mode =
        static_cast<std::uint8_t>(app->SelfShadowMode());
    added->distance = app->state.physicsInterval;
    added->selected = 1;
    if (frame > app->state.lastRegisteredFrame)
        app->state.lastRegisteredFrame = frame;
    curve_host::NotifyLegacyRegistered(app, curve_host::Kind::Shadow, 0, frameValue);
}

}  // namespace mikumikustudio
