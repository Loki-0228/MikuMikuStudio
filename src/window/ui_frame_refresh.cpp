#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/key_pool.hpp"
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
#include "mikumikustudio/ported_funcs.hpp"

#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/panel_controls.hpp"

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

void ReplaceEditText(HWND main, int id, const char* text) {
    HWND edit = GetDlgItem(main, id);
    const LRESULT length = GetWindowTextLengthA(edit);
    SendMessageA(edit, EM_SETSEL, 0, length);
    SendMessageA(edit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text));
}

void FormatLegacyOneDecimal(char* text, std::size_t size, float value) {
    const double magnitude = std::floor(std::fabs(
        static_cast<double>(value)) * 10.0 + 0.5) / 10.0;
    const double rounded = std::signbit(value) ? -magnitude : magnitude;
    sprintf_s(text, size, "%+3.1f", rounded);
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
        SendMessageA(GetDlgItem(main, 455 + lane), TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(static_cast<int>(color[lane] * 256.0f)));
        SendMessageA(GetDlgItem(main, 458 + lane), TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(static_cast<int>(direction[lane] * 100.0f)));

        sprintf_s(text, sizeof(text), "%3d",
                  static_cast<int>(color[lane] * 256.0f));
        ReplaceEditText(main, 461 + lane, text);
        FormatLegacyOneDecimal(text, sizeof(text), direction[lane]);
        ReplaceEditText(main, 464 + lane, text);
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
    if(std::strcmp(text,previous)!=0)SetWindowTextA(edit,text);
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
    if(std::strcmp(text,previous)!=0)SetWindowTextA(edit,text);

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
    if (oldModel != parentSlot) {
        HWND boneCombo = GetDlgItem(main, panel::kAttachBoneCombo);
        SendMessageA(boneCombo, CB_RESETCONTENT, 0, 0);
        if (parentSlot >= 0) {
            unsigned char* model = app->ModelSlot(parentSlot);
            if (model != nullptr) {
                const mdl::ModelRecord& record = *mdl::Mdl(model);
                FillBoneCombo(boneCombo, &record, UiUsesEnglishNames(app->EnglishUI()));
                SendMessageA(modelCombo, CB_SETCURSEL,
                             static_cast<WPARAM>(record.comboSelIndex), 0);
            }
        } else {
            SendMessageA(modelCombo, CB_SETCURSEL, 0, 0);
        }
    }

    if (parentSlot >= 0) {
        unsigned char* model = app->ModelSlot(parentSlot);
        if (model != nullptr) {
            SelectBoneCombo(GetDlgItem(main, panel::kAttachBoneCombo), accessory->parentBone);
        }
    }

    SendMessageA(GetDlgItem(main, panel::kAccessoryVisibleCheckbox), BM_SETCHECK,
                 accessory->visible != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(GetDlgItem(main, panel::kAccessoryAddBlendCheckbox), BM_SETCHECK,
                 accessory->additiveBlend != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(GetDlgItem(main, panel::kAccessoryShadowCheckbox), BM_SETCHECK,
                 accessory->shadowEnabled != 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    char text[104];
    for (int lane = 0; lane < 3; ++lane) {
        sprintf_s(text, sizeof(text), "%3.4f",
                  static_cast<double>(accessory->position[lane]));
        ReplaceEditText(main, 478 + lane, text);
        sprintf_s(text, sizeof(text), "%3.4f",
                  static_cast<double>(accessory->rotation[lane]) /
                      3.141592025756836 * 180.0);
        ReplaceEditText(main, 481 + lane, text);
    }
    sprintf_s(text, sizeof(text), "%3.4f",
              static_cast<double>(accessory->scale));
    ReplaceEditText(main, 484, text);
    sprintf_s(text, sizeof(text), "%3.2f",
              static_cast<double>(accessory->opacity));
    ReplaceEditText(main, 485, text);
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
        curve_host::NotifyRegistered(app, curve_host::Kind::Shadow, 0, frameValue);
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
    curve_host::NotifyRegistered(app, curve_host::Kind::Shadow, 0, frameValue);
}

}  // namespace mikumikustudio
