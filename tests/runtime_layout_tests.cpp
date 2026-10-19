#include "mikumikustudio/frame_limits.hpp"
// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include <array>
#include <algorithm>
#include <string>
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "btBulletDynamicsCommon.h"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <commctrl.h>
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {
void ModelInitDefaults(unsigned char*);
void ModelVertexHistoryPush(unsigned char*, int);
char ModelStandardPoseSetup(unsigned char*, bool, unsigned char, unsigned char);
void ModeCameraAdjust(MMDApp*, int);
void ModeAngleAdjust(MMDApp*, int);
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void RefreshLightPanel(MMDApp*);
void RefreshSelfShadowPanel(MMDApp*);
void RegisterSelfShadowState(MMDApp*, int);
void RegisterCameraState(MMDApp*, int);
void StepFrame(MMDApp*, bool);
}

namespace {
using namespace mikumikustudio;
static_assert(alignof(btVector3) == 16, "Match the Windows Bullet library ABI");
static_assert(alignof(btRigidBody) == 16, "Match the Windows Bullet library ABI");
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

void StandardPoseBoneTraversal() {
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    ModelInitDefaults(raw);
    std::array<mdl::BoneRecord, 4> bones{};
    model->boneTable = bones.data();
    model->boneCount = static_cast<int>(bones.size());
    // Neither sought bone is first: a byte stride on BoneRecord* skips them.
    std::memcpy(bones[2].name, "\x8D\xB6\x91\xAB", 5); // left leg
    std::memcpy(bones[3].name, "\x8D\xB6\x91\xAB\x8E\xF1", 7); // left ankle
    bones[2].position[1] = 10.f;
    bones[3].position[1] = 2.f;
    model->lightDir[0] = 20.f;
    model->matColumn2[1] = 4.f;
    ModelStandardPoseSetup(raw, false, 1, 0);
    Check(Near(model->lightDir[1], 2.f), "standard pose finds a non-first left leg");
    Check(Near(model->lightDir[2], -1.f) && Near(model->legIkXOffset, 40.f),
          "standard pose finds a non-first ankle without crossing the bone array");
    model->boneCount = 0;
    model->boneTable = nullptr;
    model->lightDir[1] = 90.f;
    ModelStandardPoseSetup(raw, false, 1, 0);
    Check(model->lightDir[1] == 90.f, "empty skeleton never dereferences a bone");
}

void SparseGlobalSelection() {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    std::vector<mdl::CameraKey> camera(mdl::kTimelineKeyCapacity);
    std::vector<mdl::LightKey> light(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadow(mdl::kTimelineKeyCapacity);
    std::vector<mdl::GravityKey> gravity(mdl::kTimelineKeyCapacity);
    std::vector<mdl::AccessoryKey> accessory(mdl::kTimelineKeyCapacity);
    app->CameraKeys() = camera.data();
    app->LightKeys() = light.data();
    app->ShadowKeys() = shadow.data();
    app->GravityKeys() = gravity.data();
    app->AccessoryKeys(254) = accessory.data();
    // Exercise both ends of the pool, with 254 absent accessory tracks.
    for (auto i : {std::size_t(0), mdl::kTimelineKeyCapacity - 1}) {
        camera[i].selected = light[i].selected = shadow[i].selected =
            gravity[i].selected = accessory[i].selected = 1;
        accessory[i].frame = 47;
        accessory[i].position[2] = 12.f;
        gravity[i].noiseEnabled = 1;
    }
    ClearGlobalFrameSelection(app.get());
    for (auto i : {std::size_t(0), mdl::kTimelineKeyCapacity - 1}) {
        Check(!camera[i].selected && !light[i].selected && !shadow[i].selected &&
              !gravity[i].selected && !accessory[i].selected,
              "selection reset reaches every track and the last key");
        Check(accessory[i].frame == 47 && accessory[i].position[2] == 12.f &&
              gravity[i].noiseEnabled == 1, "selection reset preserves adjacent key fields");
    }
    std::memset(&app->state, 0, sizeof(app->state));
    ClearGlobalFrameSelection(app.get());
}

void ModelInitializationAndHistory() {
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    ModelInitDefaults(raw);
    for (int q = 0; q < 17; ++q)
        Check(model->localTransforms[q * 4 + 3] == 1.f,
              "all standard quaternions include the final shoulder W component");
    for (const auto& channel : model->poseHistory)
        for (const auto& sample : channel)
            Check(sample[0] == 0.f && sample[1] == -999.f && sample[2] == 0.f,
                  "every history channel starts empty");
    // PMX header writes must never modify the native-width trace pointer.
    std::array<float, 77> trace{};
    mdl::PoseTraceBuffer(raw) = trace.data();
    model->pmxTextEncoding = 1;
    model->pmxBoneIndexSize = 4;
    model->pmxAdditionalUvCount = 4;
    Check(mdl::PoseTraceBuffer(raw) == trace.data(), "PMX metadata and trace pointer are independent");
    mdl::BaseVertexMorphCount(raw) = 17;
    mdl::UvMorphCounts(raw).byFamily[4] = 19;
    mdl::BoneMorphOffsetCount(raw) = 23;
    for (int c = 0; c < 23; ++c) {
        model->standardBonePositions[c][0] = float(c);
        model->standardBonePositions[c][1] = float(c + 100);
        model->standardBonePositions[c][2] = float(c + 200);
    }
    ModelVertexHistoryPush(raw, 0);
    Check(model->poseHistory[0][0][1] == -999.f, "zero samples never writes before the history");
    ModelVertexHistoryPush(raw, 99);
    for (int c = 0; c < 23; ++c) {
        Check(model->poseHistory[c][29][0] == float(c), "history advances each joint independently");
        model->standardBonePositions[c][0] += 10.f;
    }
    ModelVertexHistoryPush(raw, 30);
    for (int c = 0; c < 23; ++c)
        Check(Near(model->standardBonePositions[c][0], float(c + 5)),
              "joint smoothing averages only its own valid samples");
    Check(model->morph0Count == 17 && model->uvMorphCounts.byFamily[4] == 19 &&
          model->boneMorphCount == 23 && model->poseTraceBuffer == trace.data(),
          "history smoothing preserves following counts and pointers");
    Check(model->boneTable == nullptr && model->boneKeyCursors == nullptr,
          "initialization never writes pose values into model pointers");
}

void NativeRuntimeFields() {
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    mdl::AccessoryRecord accessory{};
    accessory.parentModel = -1;
    accessory.scale = 2.f;
    accessory.opacity = 0.75f;
    app->AccessorySlot(0) = &accessory;
    app->state.coordinateSystem = 2;
    app->state.optflag[0] = 1;
    app->MouseY() = 10;
    app->PreviousMouseY() = 0;
    for (int axis = 0; axis < 3; ++axis) {
        ModeCameraAdjust(app.get(), axis);
        ModeAngleAdjust(app.get(), axis);
        Check(accessory.position[axis] != 0.f && accessory.rotation[axis] != 0.f,
              "all accessory position and angle drags reach their actual fields");
    }
    Check(accessory.parentModel == -1 && accessory.scale == 2.f && accessory.opacity == .75f,
          "accessory movement preserves attachment and material settings");

    auto model = std::make_unique<mdl::ModelRecord>();
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
    app->state.optflag[0] = 0;

    auto scene = std::make_unique<PhysicsScene>();
    btStaticPlaneShape plane(btVector3(0, 1, 0), 0);
    btRigidBody::btRigidBodyConstructionInfo info(0, nullptr, &plane);
    btRigidBody ground(info);
    ground.setDeactivationTime(12.f);
    scene->groundBody = &ground;
    app->Physics() = scene.get();
    app->state.floorVisible = 1;
    CmdViewMenu(app.get(), nullptr, 285, 0);
    Check(!ground.hasContactResponse(), "disabling ground turns off contact response");
    CmdViewMenu(app.get(), nullptr, 285, 0);
    Check(ground.hasContactResponse() && ground.isStaticObject(),
          "enabling ground restores a static collision body");
    Check(ground.getDeactivationTime() == 12.f, "ground toggle never overwrites deactivation time");
}


int TimelineParameterReadouts() {
    int checks=0, failures=0;
    auto verify=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",message);}};
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);
    HWND window=CreateWindowExW(0,L"STATIC",L"hidden timeline readouts",WS_OVERLAPPEDWINDOW,
        0,0,400,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    auto control=[&](int id,const wchar_t* type,DWORD style){return CreateWindowExW(0,type,L"",WS_CHILD|style,
        0,0,150,22,window,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);};
    HWND fovEdit=control(panel::kFovEdit,L"EDIT",0),fovSlider=control(panel::kFovSlider,TRACKBAR_CLASSW,0);
    HWND shadowEdit=control(panel::kSelfShadowRangeEdit,L"EDIT",0),shadowSlider=control(panel::kSelfShadowRangeSlider,TRACKBAR_CLASSW,0);
    for(int id:{panel::kEditOffCheckbox,panel::kEditMode1Checkbox,panel::kEditMode2Checkbox})control(id,L"BUTTON",BS_CHECKBOX);
    SendMessageW(fovSlider,TBM_SETRANGE,FALSE,MAKELPARAM(1,125));
    SendMessageW(shadowSlider,TBM_SETRANGE,FALSE,MAKELPARAM(0,9999));
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};app->Hwnd()=window;
    std::vector<mdl::CameraKey> cameras(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadows(mdl::kTimelineKeyCapacity);
    app->CameraKeys()=cameras.data();app->ShadowKeys()=shadows.data();app->CameraParentModel()=-1;
    for(int frame:{0,20}) {
        app->CurrentFrame()=frame;app->CameraFov()=frame?73.f:30.f;
        RegisterCameraState(app.get(),frame);
        app->SelfShadowMode()=frame?2:1;app->ShadowDistance()=frame?.03f:.01125f;
        RegisterSelfShadowState(app.get(),frame);
    }
    const auto savedCameras=cameras;const auto savedShadows=shadows;
    auto value=[](HWND edit){char text[64]{};GetWindowTextA(edit,text,sizeof(text));return std::strtod(text,nullptr);};
    for(int frame:{0,20,10,25,0,7}) {
        SetWindowTextA(fovEdit,"999");SendMessageW(fovSlider,TBM_SETPOS,TRUE,120);
        app->CurrentFrame()=frame;ReloadModels(app.get());
        const double expected=30.0+43.0*(std::min)(frame,20)/20.0;
        verify(std::fabs(app->CameraFov()-expected)<.0001,"camera seek evaluates keyed and intermediate FOV");
        verify(std::fabs(value(fovEdit)-app->CameraFov())<.0001,"FOV edit echoes the evaluated value including fractional frames");
        verify(SendMessageW(fovSlider,TBM_GETPOS,0,0)==std::lround(app->CameraFov()),"FOV slider follows the evaluated angle");
    }
    for(int frame:{20,25,10,0}) {
        SetWindowTextA(shadowEdit,"999");SendMessageW(shadowSlider,TBM_SETPOS,TRUE,999);
        app->CurrentFrame()=frame;RefreshSelfShadowPanel(app.get());
        const int expected=frame>=20?7000:8875,mode=frame>=20?2:1;
        verify(std::lround(10000.0-app->ShadowDistance()*100000.0)==expected,"shadow seek retains the last key after its frame");
        verify(value(shadowEdit)==expected,"shadow range edit echoes the sought value");
        verify(SendMessageW(shadowSlider,TBM_GETPOS,0,0)==expected,"shadow slider echoes the sought value");
        verify(app->SelfShadowMode()==mode&&IsDlgButtonChecked(window,mode==1?panel::kEditMode1Checkbox:panel::kEditMode2Checkbox)==BST_CHECKED,
            "shadow mode controls match the sought key");
    }
    verify(std::memcmp(cameras.data(),savedCameras.data(),cameras.size()*sizeof(cameras[0]))==0,"camera readouts never rewrite registered keys");
    verify(std::memcmp(shadows.data(),savedShadows.data(),shadows.size()*sizeof(shadows[0]))==0,"shadow readouts never rewrite registered keys");

    std::vector<curve_editor::Channel> channels;std::wstring error;
    verify(curve_host::Read(*app,curve_host::Kind::Shadow,0,channels,error),"read shadow curve channels");
    for(auto& key:channels[1].track.keys)key.interpolation=animation_curve::Interpolation::Linear;
    verify(curve_host::Write(*app,curve_host::Kind::Shadow,0,channels,error),"author a continuous shadow range curve");
    app->CurrentFrame()=10;RefreshSelfShadowPanel(app.get());
    verify(Near(app->ShadowDistance(),.020625f),"shadow curve computes an intermediate distance");
    verify(value(shadowEdit)==7938&&SendMessageW(shadowSlider,TBM_GETPOS,0,0)==7938,"shadow curve's intermediate value reaches both controls");

    // Playback uses the evaluated state, not the stale integral CurrentFrame.
    // Disable camera-track rendering so this remains a CPU-only UI test.
    app->CameraMode()=0;app->CameraTrackActive()=0;app->CameraFov()=62.5f;
    app->CurrentFrame()=0;app->PlaybackCursorSeconds()=15.0/30.0;
    SetWindowTextA(fovEdit,"999");SetWindowTextA(shadowEdit,"999");
    PlaybackPoseAdvance(app.get(),0);
    verify(value(fovEdit)==62.5&&SendMessageW(fovSlider,TBM_GETPOS,0,0)==63,"playback echoes live FOV without rereading frame zero");
    verify(Near(app->ShadowDistance(),.0253125f),"playback preserves the fractional cursor's shadow value");
    verify(value(shadowEdit)==7469&&SendMessageW(shadowSlider,TBM_GETPOS,0,0)==7469,"playback echoes the evaluated shadow range");
    curve_host::Reset(app.get());DestroyWindow(window);app->Hwnd()=nullptr;
    std::printf("timeline parameter readouts: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}

void GlobalKeyTracks() {
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    std::vector<mdl::LightKey> light(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadow(mdl::kTimelineKeyCapacity);
    app->LightKeys() = light.data();
    app->ShadowKeys() = shadow.data();
    light[0].next = 7;
    light[0].direction[1] = -2.f;
    light[7].frame = 10;
    light[7].direction[1] = -4.f;
    light[7].color[2] = 1.f;
    app->CurrentFrame() = 5;
    RefreshLightPanel(app.get());
    Check(Near(app->LightDirection()[1], -3.f) && Near(app->LightColor()[2], .5f),
          "light interpolation follows sparse keys with the typed record stride");
    for (int frame : {10, 20, 5, 10}) {
        app->SelfShadowMode() = 2;
        app->state.physicsInterval = float(frame) / 1000.f;
        RegisterSelfShadowState(app.get(), frame);
    }
    unsigned key = 0;
    for (int frame : {0, 5, 10, 20}) {
        Check(shadow[key].frame == frame, "shadow insertion/overwrite keeps a sorted linked list");
        const unsigned next = shadow[key].next;
        if (next) Check(shadow[next].previous == key, "shadow predecessor link matches");
        key = next;
    }
    app->CurrentFrame() = 15;
    RefreshSelfShadowPanel(app.get());
    Check(app->SelfShadowMode() == 2 && Near(app->state.physicsInterval, .01f),
          "shadow refresh reads the key before the current frame");
}
}

void LargeFramePlayback() {
    auto app=std::make_unique<MMDApp>();
    app->CurrentFrame()=frame_limits::maximum;
    StepFrame(app.get(),true);
    Check(app->CurrentFrame()==frame_limits::maximum,"next frame refuses structural overflow without wrapping");
    const int first=frame_limits::maximum-10;
    app->PlaybackCursorSeconds()=first/30.0;
    const double before=app->PlaybackCursorSeconds();
    app->PlaybackCursorSeconds()+=1.0/60.0;
    Check(app->PlaybackCursorSeconds()>before && std::abs(app->PlaybackCursorSeconds()*30.0-first-.5)<.00001,
          "large-frame playback advances by a half frame without float rounding");
    auto model=std::make_unique<mdl::ModelRecord>();mdl::BoneRecord bone{};
    std::vector<mdl::BoneKey> keys(mdl::kBoneKeyCapacity);mdl::DisplayKey display{};
    std::uint8_t active=0;std::uint32_t cursor=0;
    model->boneCount=1;model->boneTable=&bone;model->boneKeys=keys.data();model->displayKeys=&display;
    model->boneTrackActive=&active;model->boneKeyCursors=&cursor;
    bone.type=mdl::BoneType::RotateMove;bone.rotQuat[3]=1;bone.slotIndex=-1;bone.parent=-1;
    keys[0].next=1;keys[1].frame=first;keys[1].next=2;keys[2].previous=1;keys[2].frame=first+1;
    for(auto& k:keys){k.rotation[3]=1;for(int c=0;c<4;++c){k.interpolation[c]=k.interpolation[c+4]=20;k.interpolation[c+8]=k.interpolation[c+12]=107;}}
    keys[1].position[0]=2;keys[2].position[0]=8;
    auto* raw=reinterpret_cast<unsigned char*>(model.get());
    InitModelTrackCursors(raw,first/30.0,0);
    AdvanceModelKeyframes(raw,app->PlaybackCursorSeconds(),0);
    Check(std::abs(bone.trans[0]-5.f)<.001f,"adjacent keys near the structural frame limit interpolate correctly");
}

int main(int argc,char** argv) {
    LargeFramePlayback();
    if(argc>1&&std::string(argv[1])=="--timeline-readouts")return TimelineParameterReadouts();
    Check(TimelineParameterReadouts()==0,"timeline parameter readouts");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("runtime layouts: model initialization and joint history");
    ModelInitializationAndHistory();
    std::puts("runtime layouts: standard-pose bone traversal");
    StandardPoseBoneTraversal();
    std::puts("runtime layouts: sparse global selection");
    SparseGlobalSelection();
    std::puts("runtime layouts: accessory movement and ground collision");
    NativeRuntimeFields();
    std::puts("runtime layouts: light interpolation and shadow registration");
    GlobalKeyTracks();
    std::puts("Runtime layout regressions passed");
}
