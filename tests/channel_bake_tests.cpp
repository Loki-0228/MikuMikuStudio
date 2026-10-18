// CPU regression for the native channel bake.  Everything here runs without a
// device, without a loaded MMV.dll and without any GPU work: the scene tracks
// are the native pools, and the model tracks are a hand-built model record.
//
// MAINTENANCE: regression-keep; channel enumeration, transactional commit and
// the mode split of the bake dialog.
#define NOMINMAX
#include <Windows.h>
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/channel_bake.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/physics_bake.hpp"
#include "mikudancestudio/pmm_extension.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mikudancestudio;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", name);
    }
}
// MMDAppState is 0xA55E0 bytes on x64 (661 KB).  `app.state = MMDAppState{}`
// materialises that whole blob as a temporary on the test thread's stack, and a
// function that does it twice (or nests it under a dialog) overflows the 1 MB
// default stack.  Zero the blob in place instead: it holds only scalars, arrays
// and pointers, so a zero fill is exactly what value-initialisation produces.
void ResetAppState(MMDApp& app) { std::memset(&app.state, 0, sizeof(app.state)); }
bool Near(double a, double b) { return std::fabs(a - b) < 1.0e-6; }
// A rollback runs while a caller may already be unwinding, so an exception that
// escapes it ends the process.  The header declares it noexcept; the property is
// pinned here, where weakening the declaration breaks this build.
static_assert(
    noexcept(channel_bake::Rollback(std::declval<MMDApp&>(), std::declval<channel_bake::Plan&>())),
    "channel_bake::Rollback must stay noexcept");

template <class K>
std::vector<int> NativeFrames(const K* pool, std::size_t capacity, unsigned head) {
    std::vector<int> frames;
    unsigned index = head;
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        if (index >= capacity) break;
        frames.push_back(static_cast<int>(pool[index].frame));
        if (pool[index].next == 0) break;
        index = pool[index].next;
    }
    return frames;
}

std::vector<double> MorphValues(const mdl::MorphKey* pool, int morph) {
    std::vector<double> values;
    unsigned index = static_cast<unsigned>(morph);
    for (std::size_t visited = 0; visited < mdl::kMorphKeyCapacity; ++visited) {
        values.push_back(pool[index].value);
        if (pool[index].next == 0) break;
        index = pool[index].next;
    }
    return values;
}

std::vector<unsigned char> DisplayLane(unsigned char* model, int lane) {
    auto* keys = mdl::DisplayKeys(model);
    std::vector<unsigned char> values;
    unsigned index = 0;
    for (std::size_t visited = 0; visited < mdl::kDisplayKeyCapacity; ++visited) {
        if (lane == 0)
            values.push_back(keys[index].visible != 0 ? 1 : 0);
        else
            values.push_back(mdl::IkStates(keys[index]) ? mdl::IkStates(keys[index])[lane - 1] : 0);
        if (keys[index].next == 0) break;
        index = keys[index].next;
    }
    return values;
}

// ---- scene fixture ---------------------------------------------------------
// An empty scene in camera mode: no model, one .x object, the four native
// global tracks.  This is exactly the state the requirement calls "empty scene,
// camera page, no MMV".
struct Scene {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::CameraKey[]> camera =
        std::make_unique<mdl::CameraKey[]>(mdl::kTimelineKeyCapacity);
    std::unique_ptr<mdl::LightKey[]> light =
        std::make_unique<mdl::LightKey[]>(mdl::kTimelineKeyCapacity);
    std::unique_ptr<mdl::SelfShadowKey[]> shadow =
        std::make_unique<mdl::SelfShadowKey[]>(mdl::kTimelineKeyCapacity);
    std::unique_ptr<mdl::GravityKey[]> gravity =
        std::make_unique<mdl::GravityKey[]>(mdl::kTimelineKeyCapacity);
    std::unique_ptr<mdl::AccessoryKey[]> accessory =
        std::make_unique<mdl::AccessoryKey[]>(mdl::kTimelineKeyCapacity);
    mdl::AccessoryRecord object{};

    Scene() {
        ResetAppState(*app);
        app->CameraKeys() = camera.get();
        app->LightKeys() = light.get();
        app->ShadowKeys() = shadow.get();
        app->GravityKeys() = gravity.get();
        app->AccessorySlot(0) = &object;
        app->AccessoryKeys(0) = accessory.get();
        app->state.optflag[0] = 1;  // camera / light / accessory page
        object.parentModel = -1;
        object.scale = 1;
        object.opacity = 1;
        object.visible = 1;
        object.shadowEnabled = 1;
        // Camera: frames 0, 10 and 40 with eye X equal to the frame number, so
        // a resampled value is predictable: X(frame) == frame.  The default
        // self-shadow/light/gravity tracks keep their own single frame-zero key.
        const int cameraFrames[3] = {0, 10, 40};
        for (int i = 0; i < 3; ++i) {
            camera[i].frame = static_cast<std::uint32_t>(cameraFrames[i]);
            camera[i].previous = static_cast<std::uint32_t>(i ? i - 1 : 0);
            camera[i].next = static_cast<std::uint32_t>(i + 1 < 3 ? i + 1 : 0);
            camera[i].eye[0] = static_cast<float>(cameraFrames[i]);
            camera[i].distance = 45;
            camera[i].fov = 30;
            camera[i].parentModel = -1;
        }
        light[0].frame = 0;
        light[0].next = 1;
        light[0].direction[1] = 1;
        light[0].color[0] = light[0].color[1] = light[0].color[2] = 1;
        light[1].frame = 20;
        light[1].previous = 0;
        light[1].direction[0] = 1;
        light[1].color[0] = light[1].color[1] = light[1].color[2] = 1;
        shadow[0].frame = 0;
        shadow[0].next = 1;
        shadow[0].mode = 1;
        shadow[0].distance = 0.05f;
        shadow[1].frame = 20;
        shadow[1].previous = 0;
        shadow[1].mode = 2;
        shadow[1].distance = 0.05f;
        gravity[0].frame = 0;
        gravity[0].acceleration = 9.8f;
        gravity[0].direction[1] = -1;
        // .x object: position X steps from 0 to 6 at frame 20, holds to frame 60.
        accessory[0].frame = 0;
        accessory[0].next = 1;
        accessory[0].parentModel = -1;
        accessory[0].scale = 1;
        accessory[0].opacity = 1;
        accessory[0].visible = 1;
        accessory[0].shadowEnabled = 1;
        accessory[1].frame = 20;
        accessory[1].previous = 0;
        accessory[1].next = 2;
        accessory[1].position[0] = 6;
        accessory[1].parentModel = -1;
        accessory[1].scale = 1;
        accessory[1].opacity = 1;
        accessory[1].visible = 1;
        accessory[1].shadowEnabled = 1;
        accessory[2].frame = 60;
        accessory[2].previous = 1;
        accessory[2].position[0] = 6;
        accessory[2].parentModel = -1;
        accessory[2].scale = 1;
        accessory[2].opacity = 1;
        accessory[2].visible = 1;
        accessory[2].shadowEnabled = 1;
    }
    ~Scene() {
        curve_host::Reset(app.get());
        if(app->CameraKeys()!=camera.get())mdl::KeyPoolReleaseOwned(app->CameraKeys());
        if(app->LightKeys()!=light.get())mdl::KeyPoolReleaseOwned(app->LightKeys());
        if(app->ShadowKeys()!=shadow.get())mdl::KeyPoolReleaseOwned(app->ShadowKeys());
        if(app->GravityKeys()!=gravity.get())mdl::KeyPoolReleaseOwned(app->GravityKeys());
        if(app->AccessoryKeys(0)!=accessory.get())mdl::KeyPoolReleaseOwned(app->AccessoryKeys(0));
    }
};

std::vector<unsigned char> LightBytes(const Scene& scene) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(scene.light.get());
    return std::vector<unsigned char>(bytes, bytes + sizeof(mdl::LightKey) * mdl::kTimelineKeyCapacity);
}
std::vector<unsigned char> CameraBytes(const Scene& scene) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(scene.camera.get());
    return std::vector<unsigned char>(bytes, bytes + sizeof(mdl::CameraKey) * mdl::kTimelineKeyCapacity);
}
std::vector<unsigned char> AccessoryBytes(const Scene& scene) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(scene.accessory.get());
    return std::vector<unsigned char>(bytes,
                                     bytes + sizeof(mdl::AccessoryKey) * mdl::kTimelineKeyCapacity);
}

void SelectCameraXY(channel_bake::Request& request) {
    request.selection.choices.clear();
    auto& choice = request.selection.Touch(channel_bake::Kind::Camera, 0, 11);
    choice.fields.assign(11, false);
    choice.fields[0] = true;
    choice.fields[1] = true;
}

// ---- scene page: enumeration -----------------------------------------------
void SceneEnumeration() {
    auto scene = std::make_unique<Scene>();
    Check(channel_bake::CurrentScope(*scene->app) == channel_bake::Scope::Scene,
          "the camera page selects the scene scope");
    std::vector<channel_bake::Group> groups;
    std::wstring message;
    Check(channel_bake::Enumerate(*scene->app, channel_bake::Scope::Scene, groups, message) ==
              channel_bake::Error::None,
          "an empty scene in camera mode enumerates");
    Check(groups.size() == 5, "camera, light, self shadow, gravity and the .x object are offered");
    Check(groups.size() == 5 && groups[0].kind == channel_bake::Kind::Camera &&
              groups[0].fields.size() == 11,
          "camera offers its own sub-parameters");
    Check(groups.size() == 5 && groups[3].kind == channel_bake::Kind::Gravity &&
              groups[3].fields.size() == 6,
          "gravity offers its own sub-parameters");
    Check(groups.size() == 5 && groups[4].kind == channel_bake::Kind::Accessory && groups[4].slot == 0 &&
              groups[4].fields.size() == 12,
          "each .x object offers its own sub-parameters");
    scene->app->EnglishUI() = 1;
    Check(channel_bake::Enumerate(*scene->app, channel_bake::Scope::Scene, groups, message) ==
                  channel_bake::Error::None &&
              groups[0].label == L"Camera" && groups[0].fields[7].label == L"View angle",
          "English labels");
    scene->app->EnglishUI() = 0;
    Check(channel_bake::Enumerate(*scene->app, channel_bake::Scope::Scene, groups, message) ==
                  channel_bake::Error::None &&
              groups[0].label == L"カメラ" && groups[0].fields[7].label == L"視野角",
          "Japanese labels");
    scene->app->EnglishUI() = 2;
    Check(channel_bake::Enumerate(*scene->app, channel_bake::Scope::Scene, groups, message) ==
                  channel_bake::Error::None &&
              groups[0].label == L"摄像机" && groups[0].fields[7].label == L"视角",
          "Chinese labels");
    scene->app->EnglishUI() = 2;
    // The model page needs a model, and the scene page must never claim one.
    Check(channel_bake::Enumerate(*scene->app, channel_bake::Scope::Model, groups, message) ==
              channel_bake::Error::Unavailable,
          "the model page refuses a scene without a model");
    std::puts("scene page enumeration complete");
}

// ---- scene page: bake, step, sub-parameters, keys outside the range --------
void CameraBake() {
    auto scene = std::make_unique<Scene>();
    const auto lightBefore = LightBytes(*scene);
    const auto accessoryBefore = AccessoryBytes(*scene);
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Scene;
    request.firstFrame = 5;
    request.lastFrame = 25;
    request.step = 10;
    SelectCameraXY(request);
    std::wstring message;
    std::vector<curve_editor::Channel> source;
    Check(curve_host::Read(*scene->app, curve_host::Kind::Camera, 0, source, message), "read source camera before bake");
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None,
          "prepare an empty scene camera bake");
    Check(plan.Valid(), "a prepared plan is valid");
    Check(channel_bake::Commit(*scene->app, plan, message) == channel_bake::Error::None,
          "commit an empty scene camera bake");
    std::vector<curve_editor::Channel> channels;
    std::wstring error;
    Check(curve_host::Read(*scene->app, curve_host::Kind::Camera, 0, channels, error),
          "read the baked camera");
    std::vector<int> frames;
    for (const auto& key : channels[0].track.keys) frames.push_back(key.frame);
    Check(frames == std::vector<int>({0, 5, 15, 25, 40}),
          "the selected range is resampled and every key outside it is kept");
    bool xValues = true;
    for (const auto& key : channels[0].track.keys) xValues = xValues && Near(key.value, curve_editor::EvaluateChannel(source[0], key.frame).value);
    bool yValues = true;
    for (const auto& key : channels[1].track.keys) yValues = yValues && Near(key.value, 0);
    Check(xValues, "camera X is resampled from the source curve");
    Check(yValues, "camera Y keeps the value of its own keys");
    Check(curve_host::Read(*scene->app, curve_host::Kind::Camera, 0, channels, error) &&
              channels[8].track.discrete && channels[8].track.keys.front().value == 0,
          "a discrete camera sub-parameter keeps its Step keys");
    Check(LightBytes(*scene) == lightBefore, "an unselected scene track is byte-identical");
    Check(AccessoryBytes(*scene) == accessoryBefore, "an unselected .x track is byte-identical");
    // curve_host mirrors the whole channel set, so the native pool holds the
    // union of every channel's frames, not just the resampled one.
    Check(NativeFrames(scene->camera.get(), mdl::kTimelineKeyCapacity, 0) ==
              std::vector<int>({0, 5, 10, 15, 25, 40}),
          "the native camera pool holds the union of the channel frames");
    Check(scene->app->SceneModified() == 1, "a committed bake marks the scene modified");
    std::puts("camera bake complete");
}

void NativeCameraBakeCompatibility() {
    for(const std::array<double,4> controls:{std::array<double,4>{100,10,20,115},{0,127,127,0}}) {
        auto scene=std::make_unique<Scene>();
        for(int i=0;i<2;++i) {
            auto& key=scene->camera[i];key.frame=i*100;key.previous=0;key.next=i?0:1;key.eye[0]=float(i*10);
            for(int lane=0;lane<6;++lane)for(int c=0;c<4;++c)key.interpolation[c][lane]=static_cast<unsigned char>(controls[c]);
        }
        scene->camera[2]={};
        std::vector<curve_editor::Channel> before,after;std::wstring message;
        Check(curve_host::Read(*scene->app,curve_host::Kind::Camera,0,before,message),"read native camera bake timing");
        channel_bake::Request request;request.scope=channel_bake::Scope::Scene;
        request.firstFrame=0;request.lastFrame=100;request.step=1;
        auto& choice=request.selection.Touch(channel_bake::Kind::Camera,0,before.size());
        choice.fields.assign(before.size(),false);choice.fields[0]=true;
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app,request,plan,message)==channel_bake::Error::None &&
              channel_bake::Commit(*scene->app,plan,message)==channel_bake::Error::None,"bake native crossed/endpoint camera timing");
        Check(curve_host::Read(*scene->app,curve_host::Kind::Camera,0,after,message),"read native camera bake result");
        for(int frame=0;frame<=100;++frame)
            Check(Near(curve_editor::EvaluateChannel(before[0],frame).value,
                       curve_editor::EvaluateChannel(after[0],frame).value),"native camera bake samples the actual player at every frame");
        // Preparing a selected rotation lane splits its linked group locally;
        // only the requested lane is allowed into the committed bake result.
        request.firstFrame=25;request.lastFrame=75;choice.fields[0]=false;choice.fields[3]=true;
        const auto untouched=after[4].track.keys.size();channel_bake::Plan rotation;
        Check(channel_bake::Prepare(*scene->app,request,rotation,message)==channel_bake::Error::None &&
              channel_bake::Commit(*scene->app,rotation,message)==channel_bake::Error::None,"bake one selected native rotation lane");
        Check(curve_host::Read(*scene->app,curve_host::Kind::Camera,0,after,message) &&
              after[4].track.keys.size()==untouched,"bake keeps unselected linked rotation author keys unchanged");
    }
}

// ---- a prepared or refused bake writes nothing ------------------------------
void Transactional() {
    auto scene = std::make_unique<Scene>();
    const auto cameraBefore = CameraBytes(*scene);
    const auto lightBefore = LightBytes(*scene);
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Scene;
    request.firstFrame = 5;
    request.lastFrame = 25;
    request.step = 10;
    SelectCameraXY(request);
    std::wstring message;
    // Prepare only reads: the pools are byte-identical before and after it.
    {
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None,
              "prepare for the cancel check");
        Check(CameraBytes(*scene) == cameraBefore && LightBytes(*scene) == lightBefore,
              "prepare writes nothing");
        channel_bake::Discard(plan);
        Check(!plan.Valid(), "discarding a prepared bake drops it");
        Check(CameraBytes(*scene) == cameraBefore, "a discarded bake leaves the camera untouched");
    }
    // An invalid step is refused before anything is read.
    {
        channel_bake::Request bad = request;
        bad.step = 0;
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, bad, plan, message) == channel_bake::Error::InvalidRequest,
              "a zero step is refused");
        Check(CameraBytes(*scene) == cameraBefore, "a refused step leaves the camera untouched");
    }
    // Expanded capacity is automatic, and rollback must retain every original byte.
    {
        auto scene = std::make_unique<Scene>();
        channel_bake::Request dense = request;
        dense.firstFrame = 0;
        dense.lastFrame = 10050;
        dense.step = 1;
        dense.selection.choices.clear();
        auto& lightChoice = dense.selection.Touch(channel_bake::Kind::Light, 0, 6);
        lightChoice.fields.assign(6, false);
        lightChoice.fields[0] = true;  // one lane is enough to exceed the capacity
        dense.selection.Touch(channel_bake::Kind::Camera, 0, 11);
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, dense, plan, message) == channel_bake::Error::None,
              "bake preparation accepts automatic growth beyond the former native capacity");
        Check(LightBytes(*scene) == lightBefore && CameraBytes(*scene) == cameraBefore,
              "preparing an expanded bake leaves selected tracks untouched");
        Check(channel_bake::Commit(*scene->app, plan, message) == channel_bake::Error::None,
              "commit expands scene pools automatically");
        Check(mdl::KeyPoolCapacity(scene->app->LightKeys())>10000 &&
              NativeFrames(scene->app->LightKeys(),mdl::KeyPoolCapacity(scene->app->LightKeys()),0).size()==10051,
              "expanded light bake retains every requested frame");
        Check(channel_bake::Rollback(*scene->app,plan),"rollback expanded scene bake");
        Check(std::memcmp(scene->app->LightKeys(),lightBefore.data(),lightBefore.size())==0 &&
              std::memcmp(scene->app->CameraKeys(),cameraBefore.data(),cameraBefore.size())==0,
              "expanded rollback restores source records exactly");
    }
    // A value the track cannot carry is refused in the same way: the light track
    // is selected first and must survive the camera's refusal byte for byte.
    {
        scene->camera[0].fov = 0;  // outside the supported 1..179 range
        channel_bake::Request invalid = request;
        invalid.selection.choices.clear();
        invalid.selection.Touch(channel_bake::Kind::Light, 0, 6);
        invalid.selection.Touch(channel_bake::Kind::Camera, 0, 11);
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, invalid, plan, message) == channel_bake::Error::InvalidRequest,
              "an unsupported value is refused");
        Check(LightBytes(*scene) == lightBefore, "the first selected track is untouched after a refusal");
        scene->camera[0].fov = 30;
    }
    // A successful bake followed by a rollback restores the exact bytes.
    {
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None &&
                  channel_bake::Commit(*scene->app, plan, message) == channel_bake::Error::None,
              "commit before the rollback check");
        Check(CameraBytes(*scene) != cameraBefore, "the commit changed the camera track");
        Check(channel_bake::Rollback(*scene->app, plan), "rollback reports success");
        Check(CameraBytes(*scene) == cameraBefore, "rollback restores the camera pool byte for byte");
        Check(!channel_bake::Rollback(*scene->app, plan), "a rolled back plan cannot roll back twice");
    }
    std::puts("transaction checks complete");
}

// ---- .x only scene, and the stored curve data ---------------------------------
void AccessoryAndStorage() {
    auto scene = std::make_unique<Scene>();
    const auto cameraBefore = CameraBytes(*scene);
    const auto lightBefore = LightBytes(*scene);
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Scene;
    request.firstFrame = 5;
    request.lastFrame = 45;
    request.step = 10;
    {
        auto& choice = request.selection.Touch(channel_bake::Kind::Accessory, 0, 12);
        choice.fields.assign(12, false);
        choice.fields[0] = true;  // position X only
    }
    std::wstring message;
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None &&
              channel_bake::Commit(*scene->app, plan, message) == channel_bake::Error::None,
          "a .x scene bakes without a model and without MMV");
    std::vector<curve_editor::Channel> channels;
    std::wstring error;
    Check(curve_host::Read(*scene->app, curve_host::Kind::Accessory, 0, channels, error),
          "read the baked .x track");
    std::vector<int> frames;
    for (const auto& key : channels[0].track.keys) frames.push_back(key.frame);
    Check(frames == std::vector<int>({0, 5, 15, 25, 35, 45, 60}),
          "the .x range is resampled and its outside keys survive");
    bool stepped = true;
    for (const auto& key : channels[0].track.keys)
        stepped = stepped && Near(key.value, key.frame < 20 ? 0 : 6);
    Check(stepped, "a Step sub-parameter of the .x track holds its value");
    Check(channels[0].track.keys.front().interpolation == animation_curve::Interpolation::Step,
          "the .x bake keeps Step keys instead of turning the hold into a ramp");
    Check(CameraBytes(*scene) == cameraBefore && LightBytes(*scene) == lightBefore,
          "baking one .x object leaves the other scene tracks untouched");
    // The baked track is stored by the existing curve_host storage, exactly like
    // an editor edit: the PMM extension payload round trip must return it.
    std::vector<double> baked;
    for (const auto& key : channels[0].track.keys) baked.push_back(key.value);
    pmm_extension::Document document;
    curve_host::Capture(*scene->app, document);
    curve_host::Reset(scene->app.get());
    curve_host::Load(*scene->app, document);
    std::vector<curve_editor::Channel> restored;
    Check(curve_host::Read(*scene->app, curve_host::Kind::Accessory, 0, restored, error),
          "read the restored .x track");
    std::vector<double> after;
    for (const auto& key : restored[0].track.keys) after.push_back(key.value);
    Check(baked == after, "the PMM curve payload restores the baked .x track");
    std::puts(".x bake and stored curve data complete");
}

// ---- model fixture ---------------------------------------------------------
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    // Parentheses, not braces: these records are aggregates, so `bones{2}` would
    // pick the initializer_list overload and build a ONE-element vector, and
    // every bones[1] below would write past its end.
    std::vector<mdl::BoneRecord> bones = std::vector<mdl::BoneRecord>(2);
    std::vector<mdl::MorphRecord> morphs = std::vector<mdl::MorphRecord>(2);
    std::vector<mdl::IkChain> chains = std::vector<mdl::IkChain>(1);
    std::vector<mdl::BoneOrderEntry> order = std::vector<mdl::BoneOrderEntry>(1);
    std::vector<mdl::BoneKey> boneKeys = std::vector<mdl::BoneKey>(mdl::kBoneKeyCapacity);
    std::unique_ptr<mdl::MorphKey[]> morphKeys =
        std::make_unique<mdl::MorphKey[]>(mdl::kMorphKeyCapacity);
    std::unique_ptr<mdl::DisplayKey[]> displayKeys =
        std::make_unique<mdl::DisplayKey[]>(mdl::kDisplayKeyCapacity);
    std::vector<std::unique_ptr<unsigned char[]>> ikOwned;
    std::vector<std::unique_ptr<mdl::BoneReference[]>> selectorOwned;

    Model() {
        auto& model = *record;
        model.boneTable = bones.data();
        model.boneCount = 2;
        model.boneKeys = boneKeys.data();
        std::snprintf(bones[0].name, sizeof bones[0].name, "%s", "center");
        std::snprintf(bones[1].name, sizeof bones[1].name, "%s", "leg");
        model.morphs = morphs.data();
        model.morphCount = 2;
        model.morphKeys = morphKeys.get();
        std::snprintf(morphs[0].name, sizeof morphs[0].name, "%s", "smile");
        std::snprintf(morphs[1].name, sizeof morphs[1].name, "%s", "blink");
        model.ikChains = chains.data();
        model.ikChainCount = 1;
        chains[0].boneIndex = 1;
        chains[0].targetBone = 0;
        chains[0].iterations = 1;
        chains[0].enabled = 1;
        model.boneOrderTable = order.data();
        model.boneOrderCount = 1;
        order[0].boneIndex = 1;
        model.displayKeys = displayKeys.get();
        model.maxFrame = 40;  // the display track already reaches frame 40
        // Every display slot owns its two state arrays, exactly like the loader.
        for (std::size_t i = 0; i < mdl::kDisplayKeyCapacity; ++i) {
            auto ik = std::make_unique<unsigned char[]>(1);
            ik[0] = 1;
            mdl::IkStates(displayKeys[i]) = ik.get();
            ikOwned.push_back(std::move(ik));
            auto pairs = std::make_unique<mdl::BoneReference[]>(1);
            pairs[0].modelIndex = -1;
            pairs[0].boneIndex = 0;
            mdl::SelectorStates(displayKeys[i]) = pairs.get();
            selectorOwned.push_back(std::move(pairs));
        }
        for (std::size_t i = 0; i < mdl::kDisplayKeyCapacity; ++i) displayKeys[i].visible = 1;
        // Morph 0: frame 0 -> 0, frame 20 -> 0.8.  Morph 1 stays at frame 0 only.
        morphKeys[0].frame = 0;
        morphKeys[0].allocated = 1;
        morphKeys[0].next = 2;
        morphKeys[2].frame = 20;
        morphKeys[2].previous = 0;
        morphKeys[2].value = 0.8f;
        morphKeys[2].allocated = 1;
        morphKeys[1].frame = 0;
        morphKeys[1].allocated = 1;
        // Display: visible and IK on at frame 0, both off at frame 40.
        displayKeys[0].frame = 0;
        displayKeys[0].allocated = 1;
        displayKeys[0].next = 3;
        mdl::IkStates(displayKeys[0])[0] = 1;
        mdl::SelectorStates(displayKeys[0])[0].modelIndex = 7;
        mdl::SelectorStates(displayKeys[0])[0].boneIndex = 3;
        displayKeys[3].frame = 40;
        displayKeys[3].previous = 0;
        displayKeys[3].allocated = 1;
        displayKeys[3].visible = 0;
        mdl::IkStates(displayKeys[3])[0] = 0;
        mdl::SelectorStates(displayKeys[3])[0].modelIndex = 5;
        mdl::SelectorStates(displayKeys[3])[0].boneIndex = 0;
    }
    ~Model() { if (record->morphKeys != morphKeys.get()) mdl::MorphKeyPoolDestroy(record->morphKeys); }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
};

std::vector<unsigned char> MorphPoolBytes(const Model& model) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(model.morphKeys.get());
    return std::vector<unsigned char>(bytes,
                                     bytes + sizeof(mdl::MorphKey) * mdl::kMorphKeyCapacity);
}

std::vector<unsigned char> DisplayPoolBytes(const Model& model) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(model.displayKeys.get());
    return std::vector<unsigned char>(bytes,
                                      bytes + sizeof(mdl::DisplayKey) * mdl::kDisplayKeyCapacity);
}

std::vector<mdl::BoneReference> SelectorsOf(unsigned char* raw, int slot) {
    const auto* pairs = mdl::SelectorStates(mdl::DisplayKeys(raw)[slot]);
    return std::vector<mdl::BoneReference>(pairs, pairs + 1);
}

// ---- model page: morph and 表示/IK ------------------------------------------
void ModelBake() {
    auto first = std::make_unique<Model>();
    auto second = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = first->raw();
    app->ModelSlot(1) = second->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;  // model page
    Check(channel_bake::CurrentScope(*app) == channel_bake::Scope::Model,
          "the model page selects the model scope");
    const auto otherMorphs = MorphPoolBytes(*second);
    const auto otherDisplay = NativeFrames(second->displayKeys.get(), mdl::kDisplayKeyCapacity, 0);
    std::vector<channel_bake::Group> groups;
    std::wstring message;
    Check(channel_bake::Enumerate(*app, channel_bake::Scope::Model, groups, message) ==
              channel_bake::Error::None,
          "the model page enumerates");
    bool bones = false, morph = false, display = false;
    for (const auto& group : groups) {
        bones = bones || group.kind == channel_bake::Kind::Bones;
        morph = morph || (group.kind == channel_bake::Kind::Morph && group.slot == 0);
        display = display || group.kind == channel_bake::Kind::Display;
    }
    Check(bones && morph && display, "the model page offers bones, morphs and the visibility/IK track");
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Model;
    request.firstFrame = 5;
    request.lastFrame = 25;
    request.step = 10;
    request.selection.Touch(channel_bake::Kind::Morph, 0, 1);
    request.selection.Touch(channel_bake::Kind::Display, 0, 2);
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
          "prepare a model bake");
    Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::None,
          "commit a model bake");
    Check(NativeFrames(first->morphKeys.get(), mdl::kMorphKeyCapacity, 0) ==
              std::vector<int>({0, 5, 15, 25}),
          "the morph range is resampled and frame zero survives");
    const auto values = MorphValues(first->morphKeys.get(), 0);
    Check(values.size() == 4 && Near(values[0], 0) && Near(values[1], 0.2) && Near(values[2], 0.6) &&
              Near(values[3], 0.8),
          "the morph weight follows the source curve");
    Check(NativeFrames(first->morphKeys.get(), mdl::kMorphKeyCapacity, 1) == std::vector<int>({0}),
          "an unselected morph keeps its own track");
    Check(NativeFrames(first->displayKeys.get(), mdl::kDisplayKeyCapacity, 0) ==
              std::vector<int>({0, 5, 15, 25, 40}),
          "the display range is resampled and frame 40 survives");
    Check(DisplayLane(first->raw(), 0) == std::vector<unsigned char>({1, 1, 1, 1, 0}),
          "visibility is baked with Step keys");
    Check(DisplayLane(first->raw(), 1) == std::vector<unsigned char>({1, 1, 1, 1, 0}),
          "the IK flag is baked with Step keys");
    const auto selector0 = SelectorsOf(first->raw(), 0);
    const auto selector4 = SelectorsOf(first->raw(), 4);
    Check(selector0.size() == 1 && selector0[0].modelIndex == 7 && selector0[0].boneIndex == 3,
          "an existing display frame keeps its selector state");
    Check(selector4.size() == 1 && selector4[0].modelIndex == 7 && selector4[0].boneIndex == 3,
          "a new display frame inherits the selector state in effect");
    Check(mdl::Mdl(first->raw())->maxFrame == 40, "the model frame extent covers the baked display key");
    Check(MorphPoolBytes(*second) == otherMorphs &&
              NativeFrames(second->displayKeys.get(), mdl::kDisplayKeyCapacity, 0) == otherDisplay,
          "model mode never touches another object of the scene");
    std::puts("model bake complete");
}

// ---- model page: bones only, identity, capacity -----------------------------
void ModelTransactional() {
    auto first = std::make_unique<Model>();
    auto second = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = first->raw();
    app->ModelSlot(1) = second->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    const auto morphsBefore = MorphPoolBytes(*first);
    std::wstring message;
    // Selecting the bone channel alone writes nothing from this module: the
    // bone pools stay with bake::Compute/Commit.
    {
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Model;
        request.lastFrame = 10;
        request.selection.Touch(channel_bake::Kind::Bones, 0, 1);
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None &&
                  channel_bake::Commit(*app, plan, message) == channel_bake::Error::None,
              "the bone channel alone prepares and commits");
        Check(MorphPoolBytes(*first) == morphsBefore, "the bone channel writes no morph key");
    }
    // A model that is switched or deleted while the selection is alive can never
    // receive the baked keys.
    {
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Model;
        request.firstFrame = 5;
        request.lastFrame = 25;
        request.step = 10;
        request.selection.Touch(channel_bake::Kind::Morph, 0, 1);
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
              "prepare for the identity check");
        app->SetSelectedModelSlot(1);
        Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::Identity,
              "a switched model is refused");
        Check(MorphPoolBytes(*first) == morphsBefore, "the refused bake wrote no morph key");
        app->SetSelectedModelSlot(0);
        app->ModelSlot(0) = nullptr;
        Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::Identity,
              "a deleted model is refused");
        app->ModelSlot(0) = first->raw();
    }
    // A full pool is grown only at commit, and rollback covers the new slots.
    {
        for (std::size_t index = 2; index + 5 < mdl::kMorphKeyCapacity; ++index) {
            first->morphKeys[index].frame = 1;
            first->morphKeys[index].allocated = 1;
        }
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Model;
        request.firstFrame = 0;
        request.lastFrame = 100;
        request.step = 1;
        request.selection.Touch(channel_bake::Kind::Morph, 0, 1);
        channel_bake::Plan plan;
        const auto before = MorphPoolBytes(*first);
        Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
              "a full morph pool prepares a growing bake");
        Check(MorphPoolBytes(*first) == before, "preparing growth writes nothing");
        Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::None,
              "a full morph pool grows during bake commit");
        Check(mdl::MorphKeyCapacity(*first->record) > mdl::kMorphKeyCapacity,
              "bake expands beyond the former limit");
        Check(NativeFrames(first->record->morphKeys, mdl::MorphKeyCapacity(*first->record), 0).size() == 101,
              "every requested frame is baked into the grown pool");
        Check(channel_bake::Rollback(*app, plan), "grown bake can be rolled back");
        const auto* restored = reinterpret_cast<const unsigned char*>(first->record->morphKeys);
        Check(std::memcmp(restored, before.data(), before.size()) == 0,
              "rollback restores every original morph slot");
        const auto capacity = mdl::MorphKeyCapacity(*first->record);
        bool empty = true;
        for (std::size_t i = mdl::kMorphKeyCapacity; i < capacity; ++i)
            if (first->record->morphKeys[i].frame || first->record->morphKeys[i].allocated) empty = false;
        Check(empty, "rollback clears all newly allocated slots");
    }
    std::puts("model transaction checks complete");
}


void DenseMorphBake() {
    auto model = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = model->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Model;
    request.firstFrame = 0;
    request.lastFrame = 22000;
    request.step = 1;
    request.selection.Touch(channel_bake::Kind::Morph, 0, 1);
    channel_bake::Plan plan;
    std::wstring message;
    Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
          "22001-frame morph bake prepares beyond the former key limit");
    Check(model->record->morphKeys == model->morphKeys.get(),
          "preparation does not move the live pool");
    Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::None,
          "22001-frame morph bake commits");
    const auto capacity = mdl::MorphKeyCapacity(*model->record);
    const auto frames = NativeFrames(model->record->morphKeys, capacity, 0);
    Check(frames.size() == 22001 && frames.front() == 0 && frames.back() == 22000,
          "all dense baked frames survive in the grown pool");
    Check(NativeFrames(model->record->morphKeys, capacity, 1) == std::vector<int>{0},
          "dense bake preserves the unselected morph");
    Check(channel_bake::Rollback(*app, plan), "dense bake rolls back");
    Check(NativeFrames(model->record->morphKeys, capacity, 0) == std::vector<int>({0,20}),
          "dense bake rollback restores the original chain");
}

// ---- a commit that fails halfway leaves nothing behind ----------------------
// Prepare accepts a request and snapshots its restore data, but the native pools
// stay live: a key that appears between Prepare and Commit makes the write phase
// run out of room after the first keys are already written.  A failed commit has
// to undo what it wrote, so every case here compares the pool against the state
// the plan was prepared from.
void MorphCommitFailure() {
    auto first = std::make_unique<Model>();
    auto second = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = first->raw();
    app->ModelSlot(1) = second->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    const auto otherMorphs = MorphPoolBytes(*second);
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Model;
    request.firstFrame = 5;
    request.lastFrame = 25;
    request.step = 10;
    request.selection.Touch(channel_bake::Kind::Morph, 0, 1);
    std::wstring message;
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
          "prepare the morph bake that is later squeezed out");
    const auto prepared = MorphPoolBytes(*first);
    // Fill every slot Prepare counted as free, then hand back exactly one: the
    // range needs three new keys, so the first two land and the third fails, and
    // the failure happens with writes already done.  The frame extent and the
    // shared allocation cursor are moved too, to prove they come back with it.
    auto* keys = mdl::MorphKeys(first->raw());
    for (std::size_t index = mdl::Mdl(first->raw())->morphCount; index < mdl::kMorphKeyCapacity;
         ++index) {
        if (keys[index].frame != 0) continue;
        keys[index].frame = 1;
        keys[index].allocated = 1;
    }
    keys[mdl::kMorphKeyCapacity - 1] = mdl::MorphKey{};
    mdl::Mdl(first->raw())->maxFrame = 999;
    mdl::Mdl(first->raw())->searchCursor = 4242;
    Check(MorphPoolBytes(*first) != prepared, "the morph pool really changed after prepare");
    Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::Capacity,
          "a morph pool that filled up between prepare and commit fails the commit");
    Check(!message.empty(), "the failed morph commit reports why");
    Check(MorphPoolBytes(*first) == prepared,
          "a failed morph commit puts the whole pool back to the prepared state");
    Check(NativeFrames(first->morphKeys.get(), mdl::kMorphKeyCapacity, 0) == std::vector<int>({0, 20}),
          "a failed morph commit leaves only the original keys of the morph");
    Check(NativeFrames(first->morphKeys.get(), mdl::kMorphKeyCapacity, 1) == std::vector<int>({0}),
          "a failed morph commit leaves the other morph alone");
    Check(mdl::Mdl(first->raw())->maxFrame == 40 && mdl::Mdl(first->raw())->searchCursor == 0,
          "a failed morph commit restores the frame extent and the allocation cursor");
    Check(MorphPoolBytes(*second) == otherMorphs,
          "a failed morph commit never touches another model of the scene");
    // The failure was already undone, but the plan stays applied until a rollback
    // runs, so a caller that unwinds through it can still finish the undo.
    Check(channel_bake::Rollback(*app, plan), "a failed morph commit stays rollback-able");
    Check(MorphPoolBytes(*first) == prepared,
          "the rollback of a failed morph commit keeps the prepared pool");
    Check(!channel_bake::Rollback(*app, plan),
          "the second rollback of a failed morph commit has nothing left to undo");
    std::puts("morph commit failure checks complete");
}

// ---- the same for a display-only bake ---------------------------------------
// No morph is selected here, so the plan holds no morph snapshot at all.  The
// frame extent moves for display writes as well, and it still has to come back.
void DisplayCommitFailure() {
    auto first = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = first->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Model;
    request.firstFrame = 5;
    request.lastFrame = 25;
    request.step = 10;
    request.selection.Touch(channel_bake::Kind::Display, 0, 2);
    std::wstring message;
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
          "prepare the display bake that is later squeezed out");
    // Slots 1, 2 and 4 upward were free for Prepare; leave only 1 and 2, so the
    // third new display frame finds no slot and the commit fails mid-write.  Slot
    // 0 is the head and slot 3 holds frame 40, so the fill starts at slot 4.
    auto* keys = mdl::DisplayKeys(first->raw());
    for (std::size_t index = 4; index < mdl::kDisplayKeyCapacity; ++index) {
        if (keys[index].frame != 0) continue;
        keys[index].frame = 1;
        keys[index].allocated = 1;
    }
    mdl::Mdl(first->raw())->maxFrame = 999;
    mdl::Mdl(first->raw())->searchCursor = 4242;
    const auto squeezed = DisplayPoolBytes(*first);
    Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::Capacity,
          "a display pool that filled up between prepare and commit fails the commit");
    Check(!message.empty(), "the failed display commit reports why");
    Check(DisplayPoolBytes(*first) == squeezed,
          "a failed display commit puts every slot it touched back");
    Check(NativeFrames(first->displayKeys.get(), mdl::kDisplayKeyCapacity, 0) ==
              std::vector<int>({0, 40}),
          "a failed display commit leaves the original display track");
    Check(DisplayLane(first->raw(), 0) == std::vector<unsigned char>({1, 0}),
          "a failed display commit keeps the original visibility keys");
    Check(mdl::Mdl(first->raw())->maxFrame == 40 && mdl::Mdl(first->raw())->searchCursor == 0,
          "a display-only failure restores the frame extent and the cursor without a morph snapshot");
    Check(channel_bake::Rollback(*app, plan), "a failed display commit stays rollback-able");
    Check(DisplayPoolBytes(*first) == squeezed,
          "the rollback of a failed display commit changes nothing more");
    Check(!channel_bake::Rollback(*app, plan),
          "the second rollback of a failed display commit has nothing left to undo");
    std::puts("display commit failure checks complete");
}

// ---- rollback always answers, and never terminates --------------------------
void RollbackSafety() {
    // A plan that was prepared but never committed has nothing to undo.
    {
        auto scene = std::make_unique<Scene>();
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Scene;
        request.firstFrame = 5;
        request.lastFrame = 25;
        request.step = 10;
        SelectCameraXY(request);
        std::wstring message;
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None,
              "prepare a plan for the uncommitted rollback guard");
        Check(!channel_bake::Rollback(*scene->app, plan),
              "a prepared but uncommitted plan cannot roll back");
    }
    // A scene plan whose .x object was removed must still restore the tracks that
    // are left: the failed target comes first, so the camera restore only happens
    // if the rollback keeps going after a target it cannot put back.
    {
        auto scene = std::make_unique<Scene>();
        const auto cameraBefore = CameraBytes(*scene);
        const auto accessoryBefore = AccessoryBytes(*scene);
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Scene;
        request.firstFrame = 5;
        request.lastFrame = 45;
        request.step = 10;
        request.selection.Touch(channel_bake::Kind::Accessory, 0, 12);  // rolled back first
        request.selection.Touch(channel_bake::Kind::Camera, 0, 11);     // ... after the failure
        std::wstring message;
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None &&
                  channel_bake::Commit(*scene->app, plan, message) == channel_bake::Error::None,
              "commit the accessory and camera tracks");
        Check(AccessoryBytes(*scene) != accessoryBefore && CameraBytes(*scene) != cameraBefore,
              "the scene commit changed both tracks");
        scene->app->AccessorySlot(0) = nullptr;
        scene->app->AccessoryKeys(0) = nullptr;
        Check(channel_bake::Rollback(*scene->app, plan),
              "a rollback with a removed .x object still reports success");
        Check(CameraBytes(*scene) == cameraBefore,
              "a rollback restores the track that follows the unavailable one");
        Check(scene->app->SceneModified() == 0 && scene->app->LastRegisteredFrame() == 0,
              "a rollback restores the dirty flag and the registered frame");
        Check(!channel_bake::Rollback(*scene->app, plan),
              "the rolled back scene plan cannot roll back twice");
    }
    // A model plan stays applied while its model is unreachable, and finishes the
    // undo once the model is selected again - or simply back in its slot.
    {
        auto first = std::make_unique<Model>();
        auto second = std::make_unique<Model>();
        auto app = std::make_unique<MMDApp>();
        ResetAppState(*app);
        app->ModelSlot(0) = first->raw();
        app->ModelSlot(1) = second->raw();
        app->SetSelectedModelSlot(0);
        app->state.optflag[0] = 0;
        const auto prepared = MorphPoolBytes(*first);
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Model;
        request.firstFrame = 5;
        request.lastFrame = 25;
        request.step = 10;
        request.selection.Touch(channel_bake::Kind::Morph, 0, 1);
        std::wstring message;
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None &&
                  channel_bake::Commit(*app, plan, message) == channel_bake::Error::None,
              "commit a morph bake for the identity guard");
        const auto baked = MorphPoolBytes(*first);
        Check(baked != prepared, "the committed morph pool holds the baked keys");
        app->SetSelectedModelSlot(1);
        Check(!channel_bake::Rollback(*app, plan),
              "a rollback refuses a model that is no longer selected");
        Check(MorphPoolBytes(*first) == baked, "the refused rollback leaves the baked model alone");
        app->SetSelectedModelSlot(0);
        Check(channel_bake::Rollback(*app, plan), "the rollback finishes once the model is selected again");
        Check(MorphPoolBytes(*first) == prepared, "the finished rollback restores the morph pool");
        channel_bake::Plan deleted;
        Check(channel_bake::Prepare(*app, request, deleted, message) == channel_bake::Error::None &&
                  channel_bake::Commit(*app, deleted, message) == channel_bake::Error::None,
              "commit a second morph bake for the deleted-model guard");
        const auto bakedAgain = MorphPoolBytes(*first);
        app->ModelSlot(0) = nullptr;
        Check(!channel_bake::Rollback(*app, deleted), "a rollback refuses a deleted model");
        Check(MorphPoolBytes(*first) == bakedAgain,
              "the refused rollback of the deleted model leaves the pool as it was");
        app->ModelSlot(0) = first->raw();
        Check(channel_bake::Rollback(*app, deleted),
              "the rollback finishes once the model is back in its slot");
        Check(MorphPoolBytes(*first) == prepared, "the finished rollback of the deleted model restores the pool");
    }
    std::puts("rollback safety checks complete");
}

// ---- the range boundary keeps the curve outside it --------------------------
// Splicing keys is not enough: the interval that ends at the first baked key and
// the interval that starts at the last one have to keep the original shape, or
// the curve outside the range changes.  The check samples that curve densely.
animation_curve::Track AsymmetricBezier(double from, double to) {
    animation_curve::Track track;
    animation_curve::Key first;
    first.frame = 0;
    first.value = from;
    first.interpolation = animation_curve::Interpolation::Linear;
    first.tangent = animation_curve::Tangent::Free;
    // A cubic that is nowhere near its own chord: one handle short with a steep
    // slope, the other long with a flat one, so a relabelled Linear piece is
    // obvious.  The weights also satisfy the key format's weight-sum rule
    // (outWeight + (1 - inWeight) <= 0.9999), which the curve core validates.
    first.outWeight = 0.35;
    first.outSlope = (to - from) / 20.0 * 1.9;
    animation_curve::Key second;
    second.frame = 20;
    second.value = to;
    second.interpolation = animation_curve::Interpolation::Bezier;
    second.tangent = animation_curve::Tangent::Free;
    second.inWeight = 0.45;
    second.inSlope = (to - from) / 20.0 * 0.3;
    track.keys.push_back(first);
    track.keys.push_back(second);
    return track;
}

double Chord(const animation_curve::Track& track, double frame) {
    const double span = static_cast<double>(track.keys.back().frame - track.keys.front().frame);
    return track.keys.front().value +
           (track.keys.back().value - track.keys.front().value) * (frame / span);
}

bool SameSamples(const animation_curve::Track& before, const animation_curve::Track& after,
                 double first, double last, double step) {
    bool same = true;
    for (double frame = first; frame <= last + 1e-9; frame += step)
        same = same && Near(animation_curve::Evaluate(before, frame).value,
                            animation_curve::Evaluate(after, frame).value);
    return same;
}

std::vector<curve_editor::Channel> CameraChannels(Scene& scene) {
    std::vector<curve_editor::Channel> channels;
    std::wstring error;
    if (!curve_host::Read(*scene.app, curve_host::Kind::Camera, 0, channels, error)) channels.clear();
    return channels;
}

void BakeCameraField(Scene& scene, std::size_t field, int first, int last, int step) {
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Scene;
    request.firstFrame = first;
    request.lastFrame = last;
    request.step = step;
    auto& choice = request.selection.Touch(channel_bake::Kind::Camera, 0, 11);
    choice.fields.assign(11, false);
    choice.fields[field] = true;
    std::wstring message;
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*scene.app, request, plan, message) == channel_bake::Error::None,
          "prepare a boundary bake");
    Check(channel_bake::Commit(*scene.app, plan, message) == channel_bake::Error::None,
          "commit a boundary bake");
}

bool SameChannelSamples(const curve_editor::Channel& before,const curve_editor::Channel& after,
                        double first,double last,double step,double tolerance=1e-6) {
    for(double frame=first;frame<=last;frame+=step)
        if(std::abs(curve_editor::EvaluateChannel(before,frame).value-
                    curve_editor::EvaluateChannel(after,frame).value)>tolerance)return false;
    return true;
}

void BoundarySplit() {
    // A cubic from frame 0 to 20, baked over 6..12 only.
    {
        auto scene = std::make_unique<Scene>();
        auto channels = CameraChannels(*scene);
        Check(channels.size() == 11, "the camera channels are readable");
        channels[6].track = AsymmetricBezier(45, 60);  // distance, a plain cubic lane
        std::wstring error;
        Check(curve_host::Write(*scene->app, curve_host::Kind::Camera, 0, channels, error),
              "write the asymmetric cubic");
        const auto before = CameraChannels(*scene);
        Check(before.size() == 11 && before[6].track.keys.size() == 2, "the cubic survives the write");
        Check(std::fabs(animation_curve::Evaluate(before[6].track, 4).value - Chord(before[6].track, 4)) >
                  1.0e-6,
              "the source curve really is a cubic, not its chord");
        BakeCameraField(*scene, 6, 6, 12, 1);
        const auto after = CameraChannels(*scene);
        Check(SameSamples(before[6].track, after[6].track, 0, 6, 0.25),
              "the interval before the range keeps the exact cubic");
        Check(SameSamples(before[6].track, after[6].track, 12, 20, 0.25),
              "the interval after the range keeps the exact cubic");
        Check(SameSamples(before[6].track, after[6].track, 6, 12, 1),
              "the baked interior keeps every integer sample");
        // The boundary keys carry real handles, not the sampled Linear default.
        const auto find = [](const animation_curve::Track& track, int frame) {
            for (const auto& key : track.keys)
                if (key.frame == frame) return key;
            return animation_curve::Key{};
        };
        const auto firstKey = find(after[6].track, 6);
        const auto lastKey = find(after[6].track, 12);
        Check(firstKey.interpolation == animation_curve::Interpolation::Bezier &&
                  std::fabs(firstKey.inSlope) > 1.0e-9,
              "the first baked key keeps the incoming handle");
        Check(std::fabs(lastKey.outSlope) > 1.0e-9, "the last baked key keeps the outgoing handle");
        Check(after[6].track.keys.front().outWeight != before[6].track.keys.front().outWeight,
              "the outside key before the range takes the split handle");
    }
    // The same range on a Linear lane must stay exactly the same line.
    {
        auto scene = std::make_unique<Scene>();
        const auto before = CameraChannels(*scene);
        Check(before.size() == 11 && before[0].track.keys.size() == 3, "the linear lane has three keys");
        BakeCameraField(*scene, 0, 6, 12, 2);
        const auto after = CameraChannels(*scene);
        Check(SameChannelSamples(before[0], after[0], 0, 6, 0.25) &&
                  SameChannelSamples(before[0], after[0], 12, 20, 0.25),
              "a Linear lane keeps its exact shape outside the range");
        Check(SameChannelSamples(before[0], after[0], 6, 12, 2),
              "a Linear lane keeps its step samples");
    }
    // A discrete lane holds its value on both sides of the range.
    {
        auto scene = std::make_unique<Scene>();
        std::vector<curve_editor::Channel> before;
        std::wstring error;
        Check(curve_host::Read(*scene->app, curve_host::Kind::Shadow, 0, before, error),
              "read the discrete lane before the bake");
        channel_bake::Request request;
        request.scope = channel_bake::Scope::Scene;
        request.firstFrame = 6;
        request.lastFrame = 12;
        request.step = 2;
        auto& choice = request.selection.Touch(channel_bake::Kind::SelfShadow, 0, 2);
        choice.fields.assign(2, false);
        choice.fields[0] = true;  // the self-shadow mode is discrete
        std::wstring message;
        channel_bake::Plan plan;
        Check(channel_bake::Prepare(*scene->app, request, plan, message) == channel_bake::Error::None &&
                  channel_bake::Commit(*scene->app, plan, message) == channel_bake::Error::None,
              "bake a discrete lane");
        std::vector<curve_editor::Channel> after;
        Check(curve_host::Read(*scene->app, curve_host::Kind::Shadow, 0, after, error),
              "read the baked discrete lane");
        Check(SameSamples(before[0].track, after[0].track, 0, 20, 0.5),
              "a discrete lane holds the same value across the whole domain");
        Check(animation_curve::Evaluate(after[0].track, 20).value == 2,
              "the discrete lane keeps its outside key");
    }
    std::puts("boundary split checks complete");
}

// ---- a boundary the key format cannot hold --------------------------------
// A refused exact split must never fall back to sampled keys: the curve outside
// the range would change while the bake reported success.  This cubic's handles
// sit on the weight floor, so the piece before frame 972 would need an outgoing
// weight below it.
void BoundaryUnrepresentable() {
    auto scene = std::make_unique<Scene>();
    auto channels = CameraChannels(*scene);
    Check(channels.size() == 11, "the camera channels are readable");
    animation_curve::Track track;
    animation_curve::Key first;
    first.frame = 0;
    first.value = 45;
    first.interpolation = animation_curve::Interpolation::Linear;
    first.tangent = animation_curve::Tangent::Free;
    first.outWeight = 0.0001;
    first.outSlope = 0.05;
    animation_curve::Key second;
    second.frame = 1000;
    second.value = 60;
    second.interpolation = animation_curve::Interpolation::Bezier;
    second.tangent = animation_curve::Tangent::Free;
    second.inWeight = 0.0001;
    second.inSlope = 0.05;
    track.keys.push_back(first);
    track.keys.push_back(second);
    channels[6].track = track;
    std::wstring error;
    Check(curve_host::Write(*scene->app, curve_host::Kind::Camera, 0, channels, error),
          "the floor-weight cubic is a valid track");
    const auto before = CameraBytes(*scene);
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Scene;
    request.firstFrame = 972;
    request.lastFrame = 990;
    request.step = 1;
    auto& choice = request.selection.Touch(channel_bake::Kind::Camera, 0, 11);
    choice.fields.assign(11, false);
    choice.fields[6] = true;
    channel_bake::Plan plan;
    std::wstring message;
    Check(channel_bake::Prepare(*scene->app, request, plan, message) ==
              channel_bake::Error::Unrepresentable,
          "a boundary the key format cannot hold is refused");
    Check(!plan.Valid() && !message.empty(), "the refusal explains itself and holds no plan");
    Check(CameraBytes(*scene) == before, "the refused boundary writes nothing");
    std::puts("unrepresentable boundary checks complete");
}

// ---- the bone channel carries its own scope ---------------------------------
// A source the caller ticked must travel as an explicit bone list or as a named
// scope, never as an implicit "the whole model": the caller drives the bone
// transaction and the per-bone FX 0/1 keys from exactly this set.
void BoneScope() {
    auto model = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = model->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    std::vector<int> bones;
    std::wstring message;
    channel_bake::Selection none;
    Check(channel_bake::ResolveBones(*app, none, bones, message) == channel_bake::Error::None &&
              bones.empty(),
          "a selection without a bone channel covers no bone");
    // The model page exposes every bone as its own sub-parameter, so a caller
    // ticks bones instead of only switching the model on or off.
    std::vector<channel_bake::Group> groups;
    Check(channel_bake::Enumerate(*app, channel_bake::Scope::Model, groups, message) ==
              channel_bake::Error::None,
          "the model page enumerates for the bone list check");
    const channel_bake::Group* boneGroup = nullptr;
    for (const auto& group : groups)
        if (group.kind == channel_bake::Kind::Bones) boneGroup = &group;
    Check(boneGroup && boneGroup->fields.size() == 2,
          "every bone of the model is its own sub-parameter");
    Check(boneGroup && boneGroup->fields[0].label == L"center" &&
              boneGroup->fields[1].label == L"leg",
          "each bone sub-parameter carries the bone name");
    channel_bake::Selection whole;
    whole.Touch(channel_bake::Kind::Bones, 0, 1);
    Check(channel_bake::ResolveBones(*app, whole, bones, message) == channel_bake::Error::None &&
              bones == std::vector<int>({0, 1}),
          "the whole scope covers every bone of the model");
    std::vector<unsigned char> marked(2, 0);  // two bones, none marked
    mdl::Mdl(model->raw())->boneSelection = marked.data();
    channel_bake::Selection selected;
    selected.Touch(channel_bake::Kind::Bones, 0, 1).coverage = channel_bake::Coverage::Selected;
    Check(channel_bake::ResolveBones(*app, selected, bones, message) == channel_bake::Error::None &&
              bones.empty(),
          "the selected scope covers no bone while none is marked");
    marked[1] = 1;
    Check(channel_bake::ResolveBones(*app, selected, bones, message) == channel_bake::Error::None &&
              bones == std::vector<int>({1}),
          "the selected scope covers only the marked bones");
    channel_bake::Selection explicitList;
    auto& explicitChoice = explicitList.Touch(channel_bake::Kind::Bones, 0, 1);
    explicitChoice.coverage = channel_bake::Coverage::Explicit;
    explicitChoice.indices = {1, 0, 1};
    Check(channel_bake::ResolveBones(*app, explicitList, bones, message) ==
                  channel_bake::Error::None &&
              bones == std::vector<int>({0, 1}),
          "an explicit list is sorted and deduplicated");
    explicitChoice.indices = {0, 5};
    Check(channel_bake::ResolveBones(*app, explicitList, bones, message) ==
              channel_bake::Error::InvalidRequest,
          "a bone index outside the model is refused instead of clamped");
    explicitChoice.indices = {1};
    channel_bake::Request request;
    request.scope = channel_bake::Scope::Model;
    request.firstFrame = 0;
    request.lastFrame = 10;
    request.step = 1;
    request.selection = explicitList;
    channel_bake::Plan plan;
    Check(channel_bake::Prepare(*app, request, plan, message) == channel_bake::Error::None,
          "prepare an explicit bone scope");
    std::vector<int> prepared;
    Check(channel_bake::PreparedBones(plan, prepared) && prepared == std::vector<int>({1}),
          "the prepared plan reports exactly the checked bones");
    Check(channel_bake::Commit(*app, plan, message) == channel_bake::Error::None,
          "commit an explicit bone scope");
    Check(NativeFrames(model->morphKeys.get(), mdl::kMorphKeyCapacity, 0) ==
              std::vector<int>({0, 20}),
          "a bone-only bake writes no morph key");
    // The dialog line names the checked bone count, so a narrowed scope is
    // visible instead of reading like the whole model.
    app->EnglishUI() = 2;
    const std::wstring line =
        channel_bake::Summary(*app, channel_bake::Scope::Model, groups, request.selection);
    Check(line.find(L"（骨骼已勾选 1 根）") != std::wstring::npos,
          "the summary names the checked bone count");
    std::puts("bone scope checks complete");
}

// ---- the Baking popup carries the mesh bake entry ---------------------------
// Mesh baking follows key baking; its dialog also offers ABC export.  A hidden host window is enough: no
// device and no dialog is opened here.
void MeshMenuEntry() {
    auto scene = std::make_unique<Scene>();
    HWND window = CreateWindowExW(0, L"STATIC", L"bake menu host", 0, 0, 0, 160, 100, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "a hidden menu host window");
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    SetMenu(window, menu);
    scene->app->MainWindow() = window;
    bake::RefreshMenu(scene->app.get());
    MENUITEMINFOW entry{sizeof entry};
    entry.fMask = MIIM_SUBMENU;
    HMENU baking = GetMenuItemInfoW(menu, bake::kBakingMenuId, FALSE, &entry) ? entry.hSubMenu : nullptr;
    Check(baking != nullptr, "the baking popup is created");
    const auto commandAt = [&](int position) {
        MENUITEMINFOW item{sizeof item};
        item.fMask = MIIM_ID;
        if (!baking || !GetMenuItemInfoW(baking, UINT(position), TRUE, &item)) return 0u;
        return item.wID;
    };
    const auto positionOf = [&](unsigned command) {
        const int count = baking ? GetMenuItemCount(baking) : 0;
        for (int i = 0; i < count; ++i)
            if (commandAt(i) == command) return i;
        return -1;
    };
    Check(commandAt(0) == bake::kBakeMenuCommand, "bake keyframes stays the first entry");
    Check(positionOf(bake::kBakeMeshMenuCommand) == positionOf(bake::kBakeMenuCommand) + 1 && positionOf(bake::kExportMenuCommand) == -1,
          "mesh baking follows key baking without the retired duplicate ABC command");
    const auto caption = [&] {
        wchar_t text[128]{};
        GetMenuStringW(baking, bake::kBakeMeshMenuCommand, text, 128, MF_BYCOMMAND);
        return std::wstring(text);
    };
    const auto enabled = [&] {
        const UINT state = GetMenuState(baking, bake::kBakeMeshMenuCommand, MF_BYCOMMAND);
        return state != UINT(-1) && (state & (MF_DISABLED | MF_GRAYED)) == 0;
    };
    Check(enabled(), "an idle scene with an object enables the mesh bake entry");
    for (unsigned char language = 0; language < 3; ++language) {
        scene->app->EnglishUI() = language;
        bake::RefreshMenu(scene->app.get());
        Check(caption() == (language == 2   ? std::wstring(L"烘焙网格…")
                            : language == 1 ? std::wstring(L"Bake mesh…")
                                            : std::wstring(L"メッシュ焼き込み…")),
              "the mesh bake caption follows the UI language");
        Check(positionOf(bake::kBakeMeshMenuCommand) == positionOf(bake::kBakeMenuCommand) + 1 && positionOf(bake::kExportMenuCommand) == -1 &&
                  commandAt(0) == bake::kBakeMenuCommand,
              "the language change keeps the entry order");
    }
    scene->app->EnglishUI() = 2;
    scene->app->PlaybackActive() = 1;
    bake::RefreshMenu(scene->app.get());
    Check(!enabled(), "playback disables the mesh bake entry");
    scene->app->PlaybackActive() = 0;
    HWND progress = CreateWindowExW(0, L"STATIC", L"mesh bake progress", 0, 0, 0, 80, 40, nullptr,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        bake::ScopedUiLock lock(progress);
        bake::RefreshMenu(scene->app.get());
        Check(!enabled(), "the interface lock disables the mesh bake entry");
    }
    bake::RefreshMenu(scene->app.get());
    Check(enabled(), "the mesh bake entry recovers after the lock");
    scene->app->MainWindow() = nullptr;
    DestroyWindow(progress);
    DestroyWindow(window);
    DestroyMenu(menu);
    std::puts("mesh bake menu entry checks complete");
}

// ---- the dialog note follows the page ---------------------------------------
// The model page is covered by the existing physics bake regression; the scene
// page is checked here, because its note has to name the scene tracks and send
// the MMV mesh and physics to the mesh entry instead of promising bones.
HHOOK g_closeHook = nullptr;
bool g_sceneNote = false;
bool g_sceneKeyControls=false,g_sceneKeyToggle=false,g_sceneRange=false,g_sceneSettings=false;
LRESULT CALLBACK CloseDialogHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (message.message == WM_INITDIALOG) {
            wchar_t cls[32]{};
            GetClassNameW(message.hwnd, cls, 32);
            if (!wcscmp(cls, L"#32770")) {
                if(GetDlgItem(message.hwnd,160)){
                    g_sceneSettings=SendDlgItemMessageW(message.hwnd,160,LB_GETCOUNT,0,0)==5;
                    EndDialog(message.hwnd,IDOK);
                    return CallNextHookEx(nullptr,code,wp,lp);
                }
                if (GetDlgItem(message.hwnd, 133)) {
                    wchar_t note[512]{};
                    GetDlgItemTextW(message.hwnd, 133, note, 512);
                    g_sceneNote =
                        !wcscmp(note,
                                L"相机、灯光、附件页烧录场景轨道：相机、灯光、自阴影、重力，以及每个 .x 附件及其子参数。网格烘焙和 ABC 导出请用「烘焙 → 烘焙网格…」。");
                    g_sceneKeyControls=IsWindowEnabled(GetDlgItem(message.hwnd,104))&&
                        IsWindowEnabled(GetDlgItem(message.hwnd,108))&&
                        !IsWindowEnabled(GetDlgItem(message.hwnd,103));
                    g_sceneRange=GetDlgItemInt(message.hwnd,101,nullptr,FALSE)==4052;
                    SetDlgItemTextW(message.hwnd,101,L"0");
                    SendMessageW(message.hwnd,WM_COMMAND,102,0);
                    g_sceneRange=g_sceneRange&&GetDlgItemInt(message.hwnd,101,nullptr,FALSE)==4052;
                    SendDlgItemMessageW(message.hwnd,104,BM_CLICK,0,0);
                    const bool off=IsDlgButtonChecked(message.hwnd,104)==BST_UNCHECKED&&
                        !IsWindowEnabled(GetDlgItem(message.hwnd,108));
                    SendDlgItemMessageW(message.hwnd,104,BM_CLICK,0,0);
                    g_sceneKeyToggle=off&&IsDlgButtonChecked(message.hwnd,104)==BST_CHECKED&&
                        IsWindowEnabled(GetDlgItem(message.hwnd,108));
                    if(IsWindowEnabled(GetDlgItem(message.hwnd,108)))
                        SendMessageW(message.hwnd,WM_COMMAND,108,0);
                    g_sceneKeyToggle=g_sceneKeyToggle&&IsDlgButtonChecked(message.hwnd,104)==BST_CHECKED;
                }
                EndDialog(message.hwnd, IDCANCEL);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void SceneDialogNote() {
    auto scene = std::make_unique<Scene>();
    auto model=std::make_unique<Model>();
    mdl::Mdl(model->raw())->maxFrame=4052;
    scene->app->ModelSlot(3)=model->raw();
    HWND window = CreateWindowExW(0, L"STATIC", L"scene note host", 0, 0, 0, 160, 100, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "a hidden host for the scene page dialog");
    scene->app->MainWindow() = window;
    scene->app->EnglishUI() = 2;
    Check(bake::CanBake(scene->app.get()), "the scene page can open the dialog without a model");
    g_sceneNote = false;
    g_sceneKeyControls=g_sceneKeyToggle=g_sceneRange=g_sceneSettings=false;
    g_closeHook = SetWindowsHookExW(WH_CALLWNDPROCRET, CloseDialogHook, nullptr, GetCurrentThreadId());
    Check(g_closeHook != nullptr, "the dialog close hook installs");
    if (g_closeHook) {
        bake::ShowDialog(scene->app.get());
        UnhookWindowsHookEx(g_closeHook);
        g_closeHook = nullptr;
    }
    Check(g_sceneNote, "the scene page note names the scene tracks and the mesh entry");
    Check(g_sceneKeyControls,"scene keyframe switch and settings are enabled while physics remains model-only");
    Check(g_sceneKeyToggle,"scene keyframes can be unchecked and checked without losing the channel selection");
    Check(g_sceneRange,"default end and Last include an unselected model beyond the camera timeline");
    Check(g_sceneSettings,"scene keyframe settings open the scene channel picker");
    scene->app->ModelSlot(3)=nullptr;
    scene->app->MainWindow() = nullptr;
    DestroyWindow(window);
    std::puts("scene page dialog note checks complete");
}

// ---- the bone picker lists and selects individual bones ---------------------
// The dialog must let a user tick single bones, not only switch the model on or
// off, so the channel dialog's field list is the model's bone list and a partly
// ticked list travels as an explicit scope.
HHOOK g_pickerHook = nullptr;
bool g_pickerOpened = false, g_pickerListed = false, g_pickerSummary = false;
int g_pickerFields = 0;
LRESULT CALLBACK PickerHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (message.message == WM_INITDIALOG) {
            if (GetDlgItem(message.hwnd, 160) && GetDlgItem(message.hwnd, 161)) {
                g_pickerOpened = true;
                g_pickerFields =
                    static_cast<int>(SendDlgItemMessageW(message.hwnd, 161, LB_GETCOUNT, 0, 0));
                wchar_t name[64]{};
                SendDlgItemMessageW(message.hwnd, 161, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(name));
                g_pickerListed = !wcscmp(name, L"center");
                // Keep the bone row and tick exactly one bone, then accept the
                // dialog.  The picker starts with every bone ticked (the model
                // default), so the rest has to be cleared first.
                SendDlgItemMessageW(message.hwnd, 160, LB_SETSEL, TRUE, 0);
                SendDlgItemMessageW(message.hwnd, 161, LB_SETSEL, FALSE, -1);
                SendDlgItemMessageW(message.hwnd, 161, LB_SETSEL, TRUE, 1);
                SendMessageW(message.hwnd, WM_COMMAND, IDOK, 0);
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            if (GetDlgItem(message.hwnd, 141)) {
                SendMessageW(message.hwnd, WM_COMMAND, 141, 0);  // open the bone picker
                wchar_t summary[256]{};
                GetDlgItemTextW(message.hwnd, 142, summary, 256);
                g_pickerSummary = std::wcsstr(summary, L"骨骼已勾选 1 根") != nullptr;
                EndDialog(message.hwnd, IDCANCEL);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void BonePicker() {
    auto model = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = model->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    app->EnglishUI() = 2;
    HWND window = CreateWindowExW(0, L"STATIC", L"bone picker host", 0, 0, 0, 160, 100, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "a hidden host for the bone picker");
    app->MainWindow() = window;
    Check(bake::CanBake(app.get()), "the model page can open the bake dialog");
    g_pickerOpened = g_pickerListed = g_pickerSummary = false;
    g_pickerFields = 0;
    g_pickerHook = SetWindowsHookExW(WH_CALLWNDPROCRET, PickerHook, nullptr, GetCurrentThreadId());
    Check(g_pickerHook != nullptr, "the picker hook installs");
    if (g_pickerHook) {
        bake::ShowDialog(app.get());
        UnhookWindowsHookEx(g_pickerHook);
        g_pickerHook = nullptr;
    }
    Check(g_pickerOpened, "the channel dialog opens from the bake dialog");
    Check(g_pickerFields == 2, "the bone picker lists every bone of the model");
    Check(g_pickerListed, "the bone picker shows bone names");
    Check(g_pickerSummary, "a partly ticked bone list shows its count in the summary");
    app->MainWindow() = nullptr;
    DestroyWindow(window);
    std::puts("bone picker checks complete");
}

// ---- the dialog gate follows the page --------------------------------------
void Gate() {
    auto scene = std::make_unique<Scene>();
    Check(bake::CanBake(scene->app.get()), "an empty scene in camera mode can bake its scene tracks");
    scene->app->state.optflag[0] = 0;
    Check(!bake::CanBake(scene->app.get()), "the model page needs a model");
    scene->app->state.optflag[0] = 1;
    scene->app->PlaybackActive() = 1;
    Check(!bake::CanBake(scene->app.get()), "playback refuses the scene bake");
    scene->app->PlaybackActive() = 0;
    auto model = std::make_unique<Model>();
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    app->ModelSlot(0) = model->raw();
    app->SetSelectedModelSlot(0);
    app->state.optflag[0] = 0;
    Check(bake::CanBake(app.get()), "the model page bakes the selected model");
    app->ModelSlot(0) = nullptr;
    Check(!bake::CanBake(app.get()), "the model page refuses a removed model");
    std::puts("page gate checks complete");
}
}  // namespace

// One marker before every stage, written unbuffered: a crash must not swallow
// the markers, so the run log names the stage that failed.  The same marker
// starts a 30-second budget for that stage: a stage that hangs (a dialog that
// never closes, a loop that never ends) is reported by name and the process
// exits with code 3 instead of sitting until the harness kills it.
std::atomic<unsigned long long> g_stageStarted{0};
std::atomic<const char*> g_stageName{"startup"};
constexpr unsigned long long kStageBudgetMs = 30000;

void Watchdog() {
    for (;;) {
        Sleep(500);
        const unsigned long long started = g_stageStarted.load();
        if (started == 0 || GetTickCount64() - started <= kStageBudgetMs) continue;
        std::printf("TIMEOUT: stage %s exceeded %llu ms\n", g_stageName.load(), kStageBudgetMs);
        std::fflush(stdout);
        std::_Exit(3);
    }
}

void Stage(const char* name) {
    g_stageName = name;
    g_stageStarted = GetTickCount64();
    std::printf("== %s\n", name);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::thread(Watchdog).detach();
    if (argc == 2 && std::strcmp(argv[1], "--morph-capacity") == 0) {
        Stage("DenseMorphBake()"); DenseMorphBake();
        Stage("ModelTransactional()"); ModelTransactional();
        Stage("MorphCommitFailure()"); MorphCommitFailure();
        Stage("DisplayCommitFailure()"); DisplayCommitFailure();
        Stage("RollbackSafety()"); RollbackSafety();
        std::printf("morph capacity bake: failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    Stage("DenseMorphBake()"); DenseMorphBake();
    Stage("SceneEnumeration()");
    SceneEnumeration();
    Stage("CameraBake()");
    CameraBake();
    NativeCameraBakeCompatibility();
    Stage("BoundarySplit()");
    BoundarySplit();
    Stage("BoundaryUnrepresentable()");
    BoundaryUnrepresentable();
    Stage("Transactional()");
    Transactional();
    Stage("AccessoryAndStorage()");
    AccessoryAndStorage();
    Stage("ModelBake()");
    ModelBake();
    Stage("ModelTransactional()");
    ModelTransactional();
    Stage("MorphCommitFailure()");
    MorphCommitFailure();
    Stage("DisplayCommitFailure()");
    DisplayCommitFailure();
    Stage("RollbackSafety()");
    RollbackSafety();
    Stage("BoneScope()");
    BoneScope();
    Stage("MeshMenuEntry()");
    MeshMenuEntry();
    Stage("SceneDialogNote()");
    SceneDialogNote();
    Stage("BonePicker()");
    BonePicker();
    Stage("Gate()");
    Gate();
    std::printf("channel_bake_tests: failures=%d\n", failures);
    return failures ? 1 : 0;
}
