#define NOMINMAX
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/viewport_bounds.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <commctrl.h>
#include <algorithm>
#include <exception>
namespace { thread_local long failAfter = -1; thread_local unsigned allocationFailures = 0; }
void* operator new(std::size_t size) {
    if (failAfter == 0) { ++allocationFailures; throw std::bad_alloc(); }
    if (failAfter > 0) --failAfter;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
using namespace mikumikustudio;
static void Check(bool b,const char*s){if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);}}
static bool PublishWithoutAllocation(curve_host::AccessoryAuthoring::Exchange& exchange) {
    failAfter = 0;
    const bool result = exchange.Publish();
    failAfter = -1;
    return result;
}
static void CheckAccessoryAuthoringRetention() {
    struct Fixture {
        std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
        mdl::AccessoryRecord objects[3]{};
        Fixture() {
            // Initialize in place: an assignment creates a ~680 KB temporary
            // and nested fixtures exceed the executable's default stack.
            ::new (static_cast<void*>(&app->state)) MMDAppState{};
            app->EnglishUI() = 2;
            app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(16);
            app->CameraKeys()[0].parentModel = -1;
            app->CameraKeys()[0].fov = 45;
            for (int slot = 0; slot < 3; ++slot) {
                app->AccessorySlot(slot) = &objects[slot];
                auto*& keys = app->AccessoryKeys(slot);
                keys = mdl::KeyPoolCreate<mdl::AccessoryKey>(16);
                keys[0].parentModel = -1; keys[0].scale = keys[0].opacity = 1;
            }
        }
        ~Fixture() {
            curve_host::Reset(app.get());
            mdl::KeyPoolDestroy(app->CameraKeys());
            for (int slot = 0; slot < 3; ++slot) mdl::KeyPoolDestroy(app->AccessoryKeys(slot));
        }
    } fixture, other;
    auto& app = *fixture.app;
    const auto author = [](MMDApp& target, curve_host::Kind kind, int slot, double value) {
        std::vector<curve_editor::Channel> channels; std::wstring error;
        Check(curve_host::Read(target, kind, slot, channels, error), "read retention fixture");
        auto& track = channels[0].track;
        track.keys[0].tangent = animation_curve::Tangent::Free;
        track.keys[0].outSlope = 0.75; track.keys[0].outWeight = 0.2;
        auto key = track.keys[0]; key.frame = 20; key.value = value;
        key.inSlope = -0.5; key.inWeight = 0.4; track.keys.push_back(key);
        channels[0].selected = false;
        channels[0].collections.push_back({"fixture", L"fixture"});
        Check(curve_host::Write(target, kind, slot, channels, error), "author retention fixture");
    };
    author(app, curve_host::Kind::Accessory, 0, 10);
    author(app, curve_host::Kind::Accessory, 1, 20);
    author(app, curve_host::Kind::Camera, 0, 30);
    author(*other.app, curve_host::Kind::Accessory, 0, 40);
    curve_host::AuthoringTable baseline, foreign;
    std::wstring error;
    Check(curve_host::CaptureAuthoring(app, baseline, error) &&
          curve_host::CaptureAuthoring(*other.app, foreign, error), "capture retention baseline");
    auto retained = std::make_unique<curve_host::AccessoryAuthoring>(app, 0);
    retained->Prepare(false).reset();
    Check(curve_host::AuthoringUnchanged(app, baseline), "cancelled accessory removal preserves authoring");
    bool rejected = false;
    try { retained->Prepare(true); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "same accessory presence rejected");
    unsigned failures = 0;
    for (long limit = 0; limit < 16; ++limit) {
        std::unique_ptr<curve_host::AccessoryAuthoring::Exchange> prepared;
        failAfter = limit;
        try { prepared = retained->Prepare(false); } catch (const std::bad_alloc&) { ++failures; }
        failAfter = -1;
        Check(curve_host::AuthoringUnchanged(app, baseline), "failed preparation preserves authored curves");
        if (prepared) break;
        Check(limit < 15, "accessory preparation failure sweep terminates");
    }
    Check(failures >= 2, "failure sweep covers exchange allocations");
    auto remove = retained->Prepare(false);
    auto superseded = retained->Prepare(false);
    Check(PublishWithoutAllocation(*remove), "accessory authoring removal allocates nothing");
    app.AccessorySlot(0) = nullptr;
    Check(!remove->IsCurrent() && !superseded->IsCurrent() && !PublishWithoutAllocation(*remove),
          "accessory exchanges are single use and invalidate competing preparation");
    Check(!curve_host::Has(&app, curve_host::Kind::Accessory, 0) &&
          curve_host::Has(&app, curve_host::Kind::Accessory, 1) &&
          curve_host::Has(&app, curve_host::Kind::Camera), "removal only detaches one accessory");
    Check(curve_host::AuthoringUnchanged(*other.app, foreign), "removal leaves another app unchanged");
    std::vector<curve_editor::Channel> peerChannels;
    Check(curve_host::Read(app, curve_host::Kind::Accessory, 1, peerChannels, error), "read peer while accessory is parked");
    peerChannels[0].selected = true;
    Check(curve_host::Write(app, curve_host::Kind::Accessory, 1, peerChannels, error), "edit peer while accessory is parked");
    auto restore = retained->Prepare(true);
    restore.reset();
    Check(!curve_host::Has(&app, curve_host::Kind::Accessory, 0), "cancelled restore leaves authoring parked");
    restore = retained->Prepare(true);
    Check(PublishWithoutAllocation(*restore), "accessory authoring restoration allocates nothing");
    app.AccessorySlot(0) = &fixture.objects[0];
    Check(curve_host::AuthoringUnchanged(app, baseline), "restoration preserves tangents and native baseline");
    Check(curve_host::Read(app, curve_host::Kind::Accessory, 1, peerChannels, error) && peerChannels[0].selected,
          "restoration preserves peer edits made while accessory was parked");
    std::vector<curve_editor::Channel> channels;
    Check(curve_host::Read(app, curve_host::Kind::Accessory, 0, channels, error) &&
          !channels[0].selected && channels[0].collections.size() == 1,
          "restoration preserves editor metadata");
    auto stale = retained->Prepare(false);
    channels[0].selected = true; channels[0].collections[0].label = L"changed metadata";
    Check(curve_host::Write(app, curve_host::Kind::Accessory, 0, channels, error), "metadata-only authoring write");
    Check(!stale->IsCurrent() && !PublishWithoutAllocation(*stale), "metadata-only write invalidates pending removal");
    stale = retained->Prepare(false);
    const auto oldValue = app.AccessoryKeys(0)[1].position[0];
    app.AccessoryKeys(0)[1].position[0] += 1;
    Check(!stale->IsCurrent() && !PublishWithoutAllocation(*stale), "unreconciled native edit invalidates removal");
    app.AccessoryKeys(0)[1].position[0] = oldValue;
    auto* originalPool = app.AccessoryKeys(0);
    app.AccessoryKeys(0) = app.AccessoryKeys(1);
    Check(!stale->IsCurrent(), "native pool replacement invalidates removal before reading old storage");
    app.AccessoryKeys(0) = originalPool;
    app.AccessorySlot(0) = &fixture.objects[1];
    Check(!stale->IsCurrent(), "native object replacement invalidates removal");
    app.AccessorySlot(0) = &fixture.objects[0];
    Check(curve_host::CaptureAuthoring(app, baseline, error), "capture edited retention baseline");
    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        remove = retained->Prepare(false);
        Check(PublishWithoutAllocation(*remove), "repeat accessory authoring removal");
        app.AccessorySlot(0) = nullptr;
        restore = retained->Prepare(true);
        Check(PublishWithoutAllocation(*restore), "repeat accessory authoring restore");
        app.AccessorySlot(0) = &fixture.objects[0];
        Check(curve_host::AuthoringUnchanged(app, baseline), "repeat removal/restore is lossless");
    }
    remove = retained->Prepare(false); Check(PublishWithoutAllocation(*remove), "park before slot reuse");
    app.AccessorySlot(0) = &fixture.objects[2];
    // A newer object may occupy the same slot until its own import is undone.
    Check(curve_host::Write(app, curve_host::Kind::Accessory, 0, channels, error), "new occupant authoring");
    rejected = false;
    try { retained->Prepare(true); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected && curve_host::Has(&app, curve_host::Kind::Accessory, 0), "restore does not overwrite another occupant");
    curve_host::ForgetAccessory(&app, 0); app.AccessorySlot(0) = nullptr;
    restore = retained->Prepare(true);
    remove.reset(); superseded.reset(); stale.reset(); retained.reset();
    Check(restore->IsCurrent() && PublishWithoutAllocation(*restore), "prepared restore retains its owner");
    app.AccessorySlot(0) = &fixture.objects[0];
    Check(curve_host::AuthoringUnchanged(app, baseline), "owner release preserves restored authoring");
    Check(curve_host::Read(app, curve_host::Kind::Accessory, 0, channels, error) &&
          channels[0].selected && channels[0].collections[0].label == L"changed metadata",
          "latest editor metadata survives slot reuse and owner release");
    curve_host::AccessoryAuthoring empty(app, 2);
    remove = empty.Prepare(false);
    Check(PublishWithoutAllocation(*remove), "accessory without authored curves can be removed");
    app.AccessorySlot(2) = nullptr;
    restore = empty.Prepare(true);
    Check(PublishWithoutAllocation(*restore), "accessory without authored curves can be restored");
    app.AccessorySlot(2) = &fixture.objects[2];
    Check(!curve_host::Has(&app, curve_host::Kind::Accessory, 2), "empty authoring remains empty");
}
static void CheckScaleLabelRepaint(MMDApp& app,HWND parent){
    const auto panels=FramePanelLayout(&app);
    // Use the same bottom-row coordinates as LayoutViewportPanels. The enhanced
    // profile derives this band from the client area, not ViewportRect.bottom.
    const int x=panels.BottomX(640), y=panels.BottomY(640,17)-9;
    const HWND label=CreateWindowExW(0,L"STATIC",L"Scale",WS_CHILD,x,y,60,18,parent,
        reinterpret_cast<HMENU>(570),GetModuleHandleW(nullptr),nullptr);
    Check(label!=nullptr,"hidden scale caption");
    RECT position{},client{};
    Check(GetWindowRect(label,&position)!=FALSE&&GetClientRect(parent,&client)!=FALSE,"scale caption coordinates");
    MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&position),2);
    Check(position.left==x&&position.top==y&&position.right==x+60&&position.bottom==y+18,
        "scale caption uses the actual parent client coordinates");
    Check(position.left>=panels.bottomBand.left&&position.right<=panels.bottomBand.right&&
        position.top>=panels.bottomBand.top&&position.bottom<=panels.bottomBand.bottom&&
        position.bottom<=client.bottom,"scale repaint fixture lies wholly inside the production bottom band");
    HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO bitmap{};bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth=60;bitmap.bmiHeader.biHeight=-18;
    bitmap.bmiHeader.biPlanes=1;bitmap.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP image=CreateDIBSection(dc,&bitmap,DIB_RGB_COLORS,&pixels,nullptr,0);
    Check(dc && image && pixels, "scale caption repaint surface");
    auto oldImage=SelectObject(dc,image);auto oldFont=SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
    MMDApp* previousApp=g_Block;g_Block=&app;
    const auto paint=[&](const wchar_t* text){HandleCtlColor(label,dc);TextOutW(dc,0,0,text,lstrlenW(text));GdiFlush();};
    std::memset(pixels,0xA0,60*18*4);paint(L"Scale");paint(L"\u7f29\u653e");
    std::vector<unsigned char> repeated(60*18*4);std::memcpy(repeated.data(),pixels,repeated.size());
    std::memset(pixels,0xA0,60*18*4);paint(L"\u7f29\u653e");
    Check(std::memcmp(repeated.data(),pixels,repeated.size())==0,"scale caption clears old glyphs on repaint");
    g_Block=previousApp;SelectObject(dc,oldFont);SelectObject(dc,oldImage);
    DeleteObject(image);DeleteDC(dc);DestroyWindow(label);
}
static void CheckCrossedCameraEndpoints() {
    auto app = std::make_unique<MMDApp>();
    ::new (static_cast<void*>(&app->state)) MMDAppState{};
    auto camera = std::make_unique<mdl::CameraKey[]>(mdl::kTimelineKeyCapacity);
    app->CameraKeys() = camera.get();
    for (unsigned i = 0; i < 3; ++i) {
        auto& key = app->CameraKeys()[i];
        key.frame = i == 0 ? 0 : i == 1 ? 44 : 61;
        key.previous = i ? i - 1 : 0; key.next = i < 2 ? i + 1 : 0;
        key.parentModel = -1; key.fov = 15; key.distance = -38.4f;
        key.eye[0] = i == 2 ? -1.2559141f : 0.f;
        for (unsigned lane = 0; lane < 6; ++lane) {
            key.interpolation[0][lane] = i == 2 ? 57 : 20;
            key.interpolation[1][lane] = i == 2 ? 0 : 20;
            key.interpolation[2][lane] = i == 2 ? 44 : 107;
            key.interpolation[3][lane] = i == 2 ? 127 : 107;
        }
    }
    std::vector<curve_editor::Channel> channels; std::wstring error;
    Check(curve_host::Read(*app, curve_host::Kind::Camera, 0, channels, error), "read crossed-X camera fixture");
    const auto value = channels[0].sample(channels[0], 44).value;
    std::printf("camera exact key expected=0 actual=%.17g\n", value);
    Check(value == 0.0, "native camera sampling returns the exact interior key value");
    const auto before = channels[0].sample(channels[0], 50).value;
    Check(curve_host::Write(*app, curve_host::Kind::Camera, 0, channels, error), "write unchanged crossed-X camera fixture");
    Check(app->CameraKeys()[1].eye[0] == 0.f, "unchanged editor commit does not perturb a key value");
    Check(app->CameraKeys()[2].interpolation[0][0] == 57 &&
          app->CameraKeys()[2].interpolation[2][0] == 44, "unchanged editor commit preserves crossed-X controls");
    pmm_extension::Document document;
    curve_host::Capture(*app, document); curve_host::Reset(app.get()); curve_host::Load(*app, document);
    Check(curve_host::Read(*app, curve_host::Kind::Camera, 0, channels, error), "reload crossed-X camera authoring");
    Check(channels[0].sample(channels[0], 44).value == 0.0 &&
          channels[0].sample(channels[0], 50).value == before, "PMM curve extension preserves native camera evaluation");
    curve_host::Reset(app.get()); app->CameraKeys() = nullptr;
}
static void CheckCameraHoldSamples() {
    auto app = std::make_unique<MMDApp>();
    ::new (static_cast<void*>(&app->state)) MMDAppState{};
    auto camera = std::make_unique<mdl::CameraKey[]>(mdl::kTimelineKeyCapacity);
    app->CameraKeys() = camera.get();
    const unsigned frames[] = {0, 12, 13, 30};
    const float positions[] = {2.f, 5.f, 11.f, 17.f};
    for (unsigned i = 0; i < 4; ++i) {
        auto& key = app->CameraKeys()[i];
        key.frame = frames[i]; key.previous = i ? i - 1 : 0; key.next = i < 3 ? i + 1 : 0;
        key.parentModel = -1; key.fov = 40 + int(i) * 5; key.distance = -30.f - float(i);
        for (unsigned axis = 0; axis < 3; ++axis) {
            key.eye[axis] = positions[i] + float(axis);
            key.target[axis] = float(i + axis) * .125f;
        }
        for (unsigned lane = 0; lane < 6; ++lane) {
            key.interpolation[0][lane] = 57; key.interpolation[1][lane] = 0;
            key.interpolation[2][lane] = 44; key.interpolation[3][lane] = 127;
        }
    }
    std::vector<curve_editor::Channel> channels; std::wstring error;
    Check(curve_host::Read(*app, curve_host::Kind::Camera, 0, channels, error), "read one-frame camera hold fixture");
    const auto check = [&]() {
        for (unsigned c = 0; c < 8; ++c) {
            const auto& channel = channels[c];
            for (unsigned i = 0; i < 4; ++i)
                Check(channel.sample(channel, frames[i]).value == channel.track.keys[i].value,
                    "all native camera lanes return exact first interior and terminal keys");
            for (double frame : {12.0, 12.25, 12.5, 12.999})
                Check(channel.sample(channel, frame).value == channel.track.keys[1].value,
                    "native one-frame camera interval holds the earlier value until the next key");
            Check(channel.sample(channel, 12.5).derivative == 0.0,
                "native one-frame hold has zero derivative away from its jump");
        }
        Check(channels[0].sample(channels[0], 13).value == 11.0 &&
              channels[0].sample(channels[0], 13.5).value > 11.0,
            "the one-frame cut jumps at its key then resumes the following native curve");
    };
    check();
    Check(curve_host::Write(*app, curve_host::Kind::Camera, 0, channels, error), "write one-frame camera hold fixture");
    app->PlaybackActive() = 1;
    curve_host::Apply(app.get(), curve_host::Kind::Camera, 12.5);
    Check(app->CameraPositionAxis(0) == 5.f, "default playback holds an adjacent camera interval");
    app->PlaybackTiming().interpolate = true;
    curve_host::Apply(app.get(), curve_host::Kind::Camera, 12.5);
    Check(app->CameraPositionAxis(0) > 5.f && app->CameraPositionAxis(0) < 11.f,
          "continuous playback samples the authored native timing between adjacent camera frames");
    app->PlaybackActive() = 0;
    app->PlaybackTiming().interpolate = false;
    for (unsigned i = 0; i < 4; ++i)
        Check(app->CameraKeys()[i].frame == frames[i] && app->CameraKeys()[i].eye[0] == positions[i],
            "unchanged curve write preserves both sides of the native camera cut");
    pmm_extension::Document document;
    curve_host::Capture(*app, document); curve_host::Reset(app.get()); curve_host::Load(*app, document);
    Check(curve_host::Read(*app, curve_host::Kind::Camera, 0, channels, error), "reload one-frame camera hold authoring");
    check();
    // An edited Step interval uses the authored evaluator, independently of
    // native one-frame holds and of the exact native-key shortcut.
    auto step = channels[0];
    step.track.keys[3].interpolation = animation_curve::Interpolation::Step;
    for (double frame : {13.0, 13.5, 29.999})
        Check(step.sample(step, frame).value == 11.0, "authored camera Step retains its discontinuous hold");
    Check(step.sample(step, 30).value == 17.0, "authored camera Step reaches the terminal key exactly");
    curve_host::Reset(app.get()); app->CameraKeys() = nullptr;
}
#include "native_camera_insert_checks.hpp"
static void CheckLongCameraSpeed() {
    auto app=std::make_unique<MMDApp>();::new(static_cast<void*>(&app->state)) MMDAppState{};
    auto camera=std::make_unique<mdl::CameraKey[]>(mdl::kTimelineKeyCapacity);app->CameraKeys()=camera.get();
    for(int index=0;index<2;++index){auto& key=camera[index];key.frame=index*100000;key.previous=0;key.next=index?0:1;
        key.parentModel=-1;key.fov=45;key.distance=-30;key.eye[0]=index?90.f:-10.f;
        for(int lane=0;lane<6;++lane){key.interpolation[0][lane]=20;key.interpolation[1][lane]=20;
            key.interpolation[2][lane]=107;key.interpolation[3][lane]=107;}}
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"read long native camera interval");
    for(double frame:{0.0,12500.0,25000.0,50000.0,75000.0,87500.0,100000.0}){
        const auto sample=channels[0].sample(channels[0],frame);
        Check(std::abs(sample.derivative-.001)<1e-10,"long linear native camera speed stays nonzero and constant");
    }
    camera[1].interpolation[1][0]=0;camera[1].interpolation[3][0]=127;
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"read long eased camera interval");
    double speeds[3]{};int index=0;
    for(double frame:{25000.0,50000.0,75000.0}){
        const auto sample=channels[0].sample(channels[0],frame);speeds[index++]=sample.derivative;
        Check(sample.derivative>.0001&&sample.derivative<.01,"long eased camera speed has no quantized zero plateaus or spikes");
        const double low=channels[0].sample(channels[0],frame-100).value;
        const double high=channels[0].sample(channels[0],frame+100).value;
        std::printf("CAMERA_SPEED_QUANTIZATION frame=%.0f analytic=%.12g nativeSecant=%.12g difference=%.12g low=%.12g high=%.12g\n",
            frame,sample.derivative,(high-low)/200,sample.derivative-(high-low)/200,low,high);
    }
    // Independent parametric reference: choose the Bezier parameter first,
    // compute its frame by the forward polynomial, then compare dy/dx. There
    // is no inverse solver or quantized playback value in this oracle.
    for(double u:{.25,.5,.75}){
        const double v=1-u;
        const double x=(3*v*v*u*20+3*v*u*u*107+u*u*u*127)/127;
        const double dx=(3*v*v*20+6*v*u*87+3*u*u*20)/127;
        const double dy=6*v*u;
        const double expected=.001*dy/dx;
        const auto actual=channels[0].sample(channels[0],x*100000).derivative;
        Check(std::abs(actual-expected)<1e-10,"native eased speed matches independent forward-parametric Bezier derivative");
    }
    Check(speeds[1]>speeds[0]&&std::abs(speeds[0]-speeds[2])<1e-10,"eased native speed has the expected symmetric peak");
    curve_host::Reset(app.get());app->CameraKeys()=nullptr;
}
namespace {
namespace pmm_parent_checks {
namespace ac = animation_curve;
using Channels = std::vector<curve_editor::Channel>;
constexpr char recordKey[] = "org.mikumikustudio.animation_curves.v1";
constexpr char intervalKey[] = "org.mikumikustudio.camera_intervals.v1";

struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> first = std::make_unique<mdl::ModelRecord>();
    std::unique_ptr<mdl::ModelRecord> second = std::make_unique<mdl::ModelRecord>();
    mdl::AccessoryRecord accessory{};
    Fixture() {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->EnglishUI() = 1;
        first->boneCount = 6; second->boneCount = 5;
        app->ModelSlot(3) = reinterpret_cast<unsigned char*>(first.get());
        app->ModelSlot(7) = reinterpret_cast<unsigned char*>(second.get());
        app->AccessorySlot(0) = &accessory;
        app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(16);
        app->AccessoryKeys(0) = mdl::KeyPoolCreate<mdl::AccessoryKey>(16);
        Check(app->CameraKeys() && app->AccessoryKeys(0), "parent remap fixture owns registered native pools");
        for (unsigned i = 0; i < 3; ++i) {
            auto& camera = app->CameraKeys()[i]; auto& object = app->AccessoryKeys(0)[i];
            camera.frame = object.frame = i * 20;
            camera.previous = object.previous = i ? i - 1 : 0;
            camera.next = object.next = i < 2 ? i + 1 : 0;
            camera.parentModel = object.parentModel = -1;
            camera.fov = 45; camera.distance = -30; camera.eye[0] = float(i * 20);
            object.scale = object.opacity = 1; object.visible = 1; object.position[0] = float(i * 4);
            for (unsigned lane = 0; lane < 6; ++lane) {
                camera.interpolation[0][lane] = 57; camera.interpolation[1][lane] = 0;
                camera.interpolation[2][lane] = 44; camera.interpolation[3][lane] = 127;
            }
        }
    }
    ~Fixture() {
        curve_host::Reset(app.get());
        mdl::KeyPoolDestroy(app->CameraKeys()); mdl::KeyPoolDestroy(app->AccessoryKeys(0));
        app->CameraKeys() = nullptr; app->AccessoryKeys(0) = nullptr;
        app->AccessorySlot(0) = nullptr; app->ModelSlot(3) = app->ModelSlot(7) = nullptr;
    }
};
Channels Read(Fixture& f, curve_host::Kind kind) {
    Channels channels; std::wstring error;
    Check(curve_host::Read(*f.app, kind, 0, channels, error), "read real loaded parent-remap authoring");
    return channels;
}
std::vector<std::uint8_t> OtherTracks(const Channels& channels) {
    std::vector<ac::NamedTrack> tracks;
    for (const auto& c : channels) if (c.id != "parentModel" && c.id != "parentBone") tracks.push_back({c.id,c.track});
    std::vector<std::uint8_t> bytes; std::string error;
    Check(ac::Encode(tracks, bytes, error), "encode all unrelated authored track fields for exact comparison");
    return bytes;
}
ac::Track Step(std::initializer_list<std::pair<int, double>> values) {
    ac::Track result; result.discrete = true;
    for (const auto& value : values) {
        ac::Key key; key.frame = value.first; key.value = value.second;
        key.interpolation = ac::Interpolation::Step; key.tangent = ac::Tangent::Free;
        result.keys.push_back(key);
    }
    return result;
}
pmm_extension::Document Document(Fixture& f) {
    auto camera = Read(f, curve_host::Kind::Camera); std::wstring error; std::size_t inserted = 0;
    Check(curve_host::InsertPreservingShape(camera, 0, 10, inserted, error), "prepare a real exact native camera slice alongside parents");
    Check(curve_host::Write(*f.app, curve_host::Kind::Camera, 0, camera, error), "retain exact camera interval metadata through the normal writer");
    auto accessory = Read(f, curve_host::Kind::Accessory);
    Check(curve_host::Write(*f.app, curve_host::Kind::Accessory, 0, accessory, error), "seed accessory authoring through the normal writer");
    // The standard prefix has fewer/different parent events than the extension.
    // No native-key writer is called when the extension-only events are added.
    for (std::size_t i = 0; i < mdl::KeyPoolCapacity(f.app->CameraKeys()); ++i) {
        auto& key = f.app->CameraKeys()[i]; key.parentModel = key.frame < 20 ? 3 : key.frame < 40 ? 7 : 3;
        key.parentBone = key.frame < 40 ? 1 : 8;
    }
    for (std::size_t i = 0; i < mdl::KeyPoolCapacity(f.app->AccessoryKeys(0)); ++i) {
        auto& key = f.app->AccessoryKeys(0)[i]; key.parentModel = key.frame < 20 ? 3 : key.frame < 40 ? 7 : 3;
        key.parentBone = key.frame < 40 ? 1 : 8;
    }
    pmm_extension::Document document; curve_host::Capture(*f.app, document);
    const auto* exact = pmm_extension::Find(document, intervalKey);
    Check(exact && !exact->value.empty(), "parent-remap fixture includes independently retained exact camera intervals");
    const auto* record = pmm_extension::Find(document, recordKey); Check(record != nullptr, "capture actual PMM curve record");
    std::vector<ac::NamedTrack> tracks; std::string reason;
    Check(ac::Decode(record->value.data(), record->value.size(), tracks, reason), "decode the real source curve record");
    for (auto& track : tracks) {
        if (track.id == "0/0/parentModel" || track.id == "4/0/parentModel")
            track.track = Step({{0,3},{5,7},{12,3},{30,7},{40,9},{45,3},{50,-1}});
        if (track.id == "0/0/parentBone" || track.id == "4/0/parentBone") {
            track.track = Step({{0,1},{15,2},{18,3},{25,99}});
            track.track.keys[0].outWeight = .8; track.track.keys[1].inWeight = .1;
            // A terminal handle has no adjacent sum bound until the remap must
            // append a step at a later parent-model event.
            track.track.keys.back().outWeight = .9999;
        }
    }
    std::vector<std::uint8_t> bytes;
    Check(ac::Encode(tracks, bytes, reason), "encode non-mirrored parent events and terminal handle boundary");
    pmm_extension::Set(document, recordKey, std::move(bytes));
    return document;
}
std::vector<PmmModelAdaptation> Adaptations() {
    return {{3,true,{2,4,-1,99}}, {7,true,{1,3,4}}, {9,false,{0}}};
}
struct Pools {
    std::vector<mdl::CameraKey> camera;
    std::vector<mdl::AccessoryKey> accessory;
    explicit Pools(const Fixture& f)
        : camera(f.app->CameraKeys(), f.app->CameraKeys() + mdl::KeyPoolCapacity(f.app->CameraKeys())),
          accessory(f.app->AccessoryKeys(0), f.app->AccessoryKeys(0) + mdl::KeyPoolCapacity(f.app->AccessoryKeys(0))) {}
    void Unchanged(const Fixture& f) const {
        Check(camera.size() == mdl::KeyPoolCapacity(f.app->CameraKeys()) &&
            accessory.size() == mdl::KeyPoolCapacity(f.app->AccessoryKeys(0)) &&
            !std::memcmp(camera.data(), f.app->CameraKeys(), camera.size() * sizeof(camera[0])) &&
            !std::memcmp(accessory.data(), f.app->AccessoryKeys(0), accessory.size() * sizeof(accessory[0])),
            "PMM parent adaptation never writes or replaces either native pool");
    }
};
void ExpectedParents(const Channels& channels, bool mapped) {
    const std::size_t model = channels.size() == 11 ? 9 : 10, bone = model + 1;
    const std::pair<double,double> samples[] = {{0,4},{4.5,4},{5,3},{11.5,3},{12,4},{14.5,4},
        {15,0},{18,0},{25,0},{30,0},{39.5,0},{40,99},{44.5,99},{45,0},{49.5,0},{50,99},{80,99}};
    const auto originalModel = Step({{0,3},{5,7},{12,3},{30,7},{40,9},{45,3},{50,-1}});
    const auto originalBone = Step({{0,1},{15,2},{18,3},{25,99}});
    for (const auto& sample : samples) {
        Check(ac::Evaluate(channels[model].track,sample.first).value == ac::Evaluate(originalModel,sample.first).value,
            "model-parent track is preserved at keys, between keys and outside its range");
        Check(ac::Evaluate(channels[bone].track,sample.first).value ==
            (mapped ? sample.second : ac::Evaluate(originalBone,sample.first).value),
            "bone-parent step semantics use the original pair, both mappings and native zero fallback");
    }
    const auto& keys = channels[bone].track.keys;
    if (mapped) {
        const int expectedFrames[] = {0,5,12,15,18,25,40,45,50};
        Check(keys.size() == sizeof(expectedFrames) / sizeof(expectedFrames[0]), "only value-changing model events add dependent bone steps");
        for (std::size_t i = 0; i < keys.size(); ++i)
            Check(keys[i].frame == expectedFrames[i], "parent-step union retains authored keys without redundant model-only events");
    }
    std::string error; Check(ac::Validate(channels[bone].track,error), "inserted parent steps retain codec-valid handle bounds");
}
void SimulateNativeSuccessTail(Fixture& f) {
    const auto remap = [](int model, int bone) {
        // Independent expected final native values for the deliberately sparse
        // prefix. This does not invoke or mirror the production remap helper.
        if (model == 3) return bone == 1 ? 4 : 0;
        if (model == 7) return bone == 1 ? 3 : 0;
        return bone;
    };
    for (std::size_t i = 0; i < mdl::KeyPoolCapacity(f.app->CameraKeys()); ++i) {
        auto& key = f.app->CameraKeys()[i]; key.parentBone = remap(key.parentModel,key.parentBone);
    }
    for (std::size_t i = 0; i < mdl::KeyPoolCapacity(f.app->AccessoryKeys(0)); ++i) {
        auto& key = f.app->AccessoryKeys(0)[i]; key.parentBone = remap(key.parentModel,key.parentBone);
    }
}
void CheckLoad(bool unavailable) {
    Fixture f; const auto document = Document(f); const auto adaptations = Adaptations();
    curve_host::Load(*f.app,document);
    const auto originalCamera = Read(f,curve_host::Kind::Camera), originalAccessory = Read(f,curve_host::Kind::Accessory);
    ExpectedParents(originalCamera,false); ExpectedParents(originalAccessory,false);
    if (unavailable) f.app->CameraKeys()[1].interpolation[0][0] = 255;
    const Pools untouched(f);
    curve_host::Load(*f.app,document,adaptations);
    auto camera = Read(f,curve_host::Kind::Camera), accessory = Read(f,curve_host::Kind::Accessory);
    ExpectedParents(camera,true); ExpectedParents(accessory,true); untouched.Unchanged(f);
    Check(OtherTracks(camera) == OtherTracks(originalCamera) && OtherTracks(accessory) == OtherTracks(originalAccessory),
        "parent remapping preserves all other curve values, interpolation, slopes and weights exactly");
    pmm_extension::Document captured; curve_host::Capture(*f.app,captured); untouched.Unchanged(f);
    const auto* oldExact = pmm_extension::Find(document,intervalKey);
    const auto* newExact = pmm_extension::Find(captured,intervalKey);
    Check(oldExact && newExact && oldExact->value == newExact->value, "parent adaptation preserves the exact native-camera interval record");
    const auto* encoded = pmm_extension::Find(captured,recordKey); std::vector<ac::NamedTrack> decoded; std::string reason;
    Check(encoded && ac::Decode(encoded->value.data(),encoded->value.size(),decoded,reason),
        "Capture and codec decoding accept parent steps inserted after an unrestricted terminal handle");
    SimulateNativeSuccessTail(f); const Pools finalNative(f);
    for (unsigned read = 0; read < 3; ++read) {
        camera = Read(f,curve_host::Kind::Camera); accessory = Read(f,curve_host::Kind::Accessory);
        ExpectedParents(camera,true); ExpectedParents(accessory,true); finalNative.Unchanged(f);
        Check(OtherTracks(camera) == OtherTracks(originalCamera) && OtherTracks(accessory) == OtherTracks(originalAccessory),
            "the first and repeated native reconciliation do not reinterpret adaptation as an edit");
    }
    // This newly saved record belongs to the now adapted standard prefix. A
    // normal reopen has no adaptation context and must not map a second time.
    curve_host::Load(*f.app,captured);
    ExpectedParents(Read(f,curve_host::Kind::Camera),true);
    ExpectedParents(Read(f,curve_host::Kind::Accessory),true); finalNative.Unchanged(f);
}
void CheckAllocationFailure() {
    Fixture f; const auto document = Document(f); const auto adaptations = Adaptations(); const Pools untouched(f);
    bool success = false, partialMapped = false; unsigned faults = 0;
    for (long budget = 0; budget < 2048 && !success; ++budget) {
        const auto failures = allocationFailures; bool threw = false;
        failAfter = budget;
        try { curve_host::Load(*f.app,document,adaptations); }
        catch (const std::bad_alloc&) { threw = true; }
        failAfter = -1;
        const bool failed = threw || allocationFailures != failures;
        if (failed) ++faults;
        const bool camera = curve_host::Has(f.app.get(),curve_host::Kind::Camera);
        const bool accessory = curve_host::Has(f.app.get(),curve_host::Kind::Accessory,0);
        // Decode may safely decline the record; a later preparation may throw.
        // Any group already published must always contain transformed parents.
        if (camera) ExpectedParents(Read(f,curve_host::Kind::Camera),true);
        if (accessory) ExpectedParents(Read(f,curve_host::Kind::Accessory),true);
        if (failed && camera && !accessory) partialMapped = true;
        untouched.Unchanged(f);
        success = !failed && camera && accessory;
    }
    Check(success && faults > 8 && partialMapped,
        "real Load allocation sweep reaches private preparation and never publishes unmapped parent groups");
}
void CheckInvalidContext() {
    Fixture f; const auto document = Document(f); const Pools untouched(f);
    curve_host::Load(*f.app,document,Adaptations());
    for (const auto& invalid : std::vector<std::vector<PmmModelAdaptation>>{
        {{-1,true,{0}}}, {{255,true,{0}}}, {{3,true,{0}},{3,false,{}}}}) {
        bool rejected = false;
        try { curve_host::Load(*f.app,document,invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "invalid transient adaptation context reaches the PMM runtime-error boundary");
        ExpectedParents(Read(f,curve_host::Kind::Camera),true);
        ExpectedParents(Read(f,curve_host::Kind::Accessory),true);
        untouched.Unchanged(f);
    }
}
}
}
int main(){
    std::set_terminate([]() noexcept {
        std::fprintf(stderr,"TERMINATE animation curve host failAfter=%ld failures=%u\n",failAfter,allocationFailures);
        if(auto pending=std::current_exception())try{std::rethrow_exception(pending);}
            catch(const std::exception& error){std::fprintf(stderr,"TERMINATE exception: %s\n",error.what());}
            catch(...){std::fputs("TERMINATE non-standard exception\n",stderr);}
        std::fflush(stderr);std::abort();
    });
    const auto stage=[](const char* name){std::fprintf(stderr,"STAGE %s\n",name);std::fflush(stderr);};
    stage("PMM adapted parent tracks");pmm_parent_checks::CheckLoad(false);
    stage("PMM adapted parents without native interpolation");pmm_parent_checks::CheckLoad(true);
    stage("PMM parent adaptation allocation failures");pmm_parent_checks::CheckAllocationFailure();
    stage("PMM invalid adaptation context");pmm_parent_checks::CheckInvalidContext();
    stage("native camera speed");CheckLongCameraSpeed();
    stage("crossed camera endpoints");
    CheckCrossedCameraEndpoints();
    stage("camera one-frame holds");
    CheckCameraHoldSamples();
    stage("exact camera subdivision");
    CheckNativeCameraInsertion();
    stage("existing host and UI regressions");
    auto app=std::make_unique<MMDApp>();
    ::new (static_cast<void*>(&app->state)) MMDAppState{};
    auto cam=std::make_unique<mdl::CameraKey[]>(mdl::kTimelineKeyCapacity);
    auto light=std::make_unique<mdl::LightKey[]>(mdl::kTimelineKeyCapacity);
    auto shadow=std::make_unique<mdl::SelfShadowKey[]>(mdl::kTimelineKeyCapacity);
    auto gravity=std::make_unique<mdl::GravityKey[]>(mdl::kTimelineKeyCapacity);
    auto acc=std::make_unique<mdl::AccessoryKey[]>(mdl::kTimelineKeyCapacity);
    mdl::AccessoryRecord object{};
    app->CameraKeys()=cam.get();app->LightKeys()=light.get();app->ShadowKeys()=shadow.get();app->GravityKeys()=gravity.get();
    app->AccessorySlot(0)=&object;app->AccessoryKeys(0)=acc.get();app->EnglishUI()=2;
    cam[0].fov=45;cam[0].parentModel=-1;acc[0].parentModel=-1;acc[0].scale=1;acc[0].opacity=1;
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*app,curve_host::Kind::Accessory,0,channels,error),"read accessory");
    auto key=channels[0].track.keys[0];key.frame=20;key.value=10;key.interpolation=animation_curve::Interpolation::Linear;
    channels[0].track.keys.push_back(key);
    Check(curve_host::Write(*app,curve_host::Kind::Accessory,0,channels,error),"add native accessory key");
    Check(acc[0].next==1&&acc[1].frame==20&&acc[1].position[0]==10,"native timeline receives new frame");
    curve_host::Apply(app.get(),curve_host::Kind::Accessory,10,0);Check(object.position[0]==5,"accessory playback interpolates");
    curve_host::Apply(app.get(),curve_host::Kind::Accessory,10,0);Check(object.position[0]==5,"no accumulated deformation");
    pmm_extension::Document doc;curve_host::Capture(*app,doc);curve_host::Reset(app.get());curve_host::Load(*app,doc);
    curve_host::Apply(app.get(),curve_host::Kind::Accessory,10,0);Check(object.position[0]==5,"PMM curve reload");
    // A native edit must not be lost when the floating editor refreshes.
    acc[1].position[0]=14;
    Check(curve_host::Read(*app,curve_host::Kind::Accessory,0,channels,error),"refresh external edit");
    Check(channels[0].track.keys.back().value==14,"native changes reconcile");
    channels[0].track.keys.pop_back();
    Check(curve_host::Write(*app,curve_host::Kind::Accessory,0,channels,error),"delete key");
    Check(acc[0].next==0,"last nonzero native frame deleted");
    channels[0].track.keys.clear();Check(!curve_host::Write(*app,curve_host::Kind::Accessory,0,channels,error),"reject removing sentinel transactionally");
    Check(acc[0].next==0,"failed edit leaves native data intact");
    for(auto k:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow,curve_host::Kind::Gravity}){
        Check(curve_host::Read(*app,k,0,channels,error),"read global track");
        size_t c=k==curve_host::Kind::Shadow?1:0;
        key=channels[c].track.keys[0];key.frame=30;key.value=k==curve_host::Kind::Shadow?0.05:10;
        key.interpolation=animation_curve::Interpolation::Linear;channels[c].track.keys.push_back(key);
        Check(curve_host::Write(*app,k,0,channels,error),"global key insertion");curve_host::Apply(app.get(),k,15);
    }
    Check(app->CameraPositionAxis(0)==5,"camera curve evaluation");
    Check(app->LightDirection()[0]==5,"light curve evaluation");
    Check(std::fabs(app->ShadowDistance()-0.025)<1.e-6,"shadow curve evaluation");
    Check(app->GravityMagnitude()==5,"gravity curve evaluation");
    // Authored curves are PMM extension data, independent of whether the
    // legacy camera interpolation can currently be decoded by the editor.
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"read authored camera");
    channels[0].track.keys[0].outSlope=0.75;
    channels[0].track.keys[0].outWeight=0.3;
    channels[0].track.keys.back().inSlope=-0.25;
    channels[0].track.keys.back().interpolation=animation_curve::Interpolation::Bezier;
    Check(curve_host::Write(*app,curve_host::Kind::Camera,0,channels,error),"write custom camera curve");
    const auto authored=channels[0].track;
    const auto originalControl=cam[1].interpolation[0][0];
    cam[1].interpolation[0][0]=255;
    try{curve_host::Capture(*app,doc);}
    catch(const std::exception&){Check(false,"capture authored data despite unavailable native interpolation");}
    curve_host::Reset(app.get());curve_host::Load(*app,doc);
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"reload custom data despite unavailable native interpolation");
    Check(channels[0].track.keys[0].outSlope==authored.keys[0].outSlope&&
          channels[0].track.keys[0].outWeight==authored.keys[0].outWeight&&
          channels[0].track.keys.back().inSlope==authored.keys.back().inSlope,
          "custom tangent data survives PMM roundtrip");
    cam[1].interpolation[0][0]=originalControl;
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"native reconciliation resumes");
    CheckAccessoryAuthoringRetention();
    // Hidden real parent/control integration: the launcher must be parented
    // during WM_CREATE even before MMDApp::Hwnd is assigned by window_init.
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    HWND parent=CreateWindowExW(0,L"STATIC",L"curve host test",WS_OVERLAPPEDWINDOW,0,0,1100,760,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(parent!=nullptr,"hidden host window");
    HMENU menu=CreateMenu(),file=CreatePopupMenu();AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"File");SetMenu(parent,menu);
    for(int id=471;id<=487;++id){
        const bool combo=id==471||id==474||id==475;
        auto control=CreateWindowExW(0,combo?L"COMBOBOX":id>=478&&id<=485?L"EDIT":L"BUTTON",L"",WS_CHILD|(combo?CBS_DROPDOWNLIST:0),868,620,100,22,parent,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),0);
    }
    curve_host::CreateControls(app.get(),parent);
    Check(GetParent(GetDlgItem(parent,curve_editor::kAccessoryCurve))==parent,"launcher created before app HWND assignment");
    app->Hwnd()=parent;app->state.optflag[0]=1;app->state.optflag[3]=1;
    {
        // This CPU/Win32 fixture has no device to publish a viewport. Supply
        // the same valid scene geometry from its actual client before asking
        // the production panel layout for the scale caption's coordinates.
        const WindowDpiScope dpi(parent);RECT client{};FrameViewportGeometry geometry{};
        app->RenderWidth()=1280;app->RenderHeight()=720;
        Check(GetClientRect(parent,&client)&&CalculateFrameViewport(client,app->SidebarWidth(),0,
            FrameViewportMode::Docked,app->RenderWidth(),app->RenderHeight(),geometry),
            "scale repaint fixture has positive output dimensions and actual-client scene geometry");
        app->ViewportRect()=geometry.scene;
        CheckScaleLabelRepaint(*app,parent);
    }
    curve_host::LayoutControls(app.get());
    for(unsigned lang=0;lang<3;++lang){app->EnglishUI()=uint8_t(lang);curve_host::Localize(app.get());
        wchar_t text[64]{};GetWindowTextW(GetDlgItem(parent,472),text,64);Check(wcslen(text)>0,"localized load label");
        Check(GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)!=UINT(-1),"explicit accessory VMD menu exists");
        const int previousMode=app->CameraMode();
        for(int mode:{0,1,0,1}) {
            app->CameraMode()=mode;curve_host::Localize(app.get());
            Check((GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)&MF_GRAYED)==(mode?0:MF_GRAYED),
                "accessory motion menu is available only in camera mode");
            SendMessageW(parent,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(file),0);
            Check((GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)&MF_GRAYED)==(mode?0:MF_GRAYED),
                "opening the menu preserves its mode-dependent availability");
        }
        app->CameraMode()=previousMode;
    }
    curve_host::Open(app.get(),false);
    Check(GetForegroundWindow()!=parent,"hidden integration does not activate host");
    const auto failuresBefore=allocationFailures;
    failAfter=0;
    curve_editor::NotifyOwnedChanged(parent);
    failAfter=-1;
    Check(allocationFailures>failuresBefore&&IsWindow(parent),
        "deferred curve notification allocation failure never escapes command completion");
    curve_editor::CloseOwned(parent);
    DestroyWindow(parent);app->Hwnd()=nullptr;
    curve_host::ForgetAccessory(app.get(),0);Check(!curve_host::Has(app.get(),curve_host::Kind::Accessory,0),"deleted object clears curves");
    curve_host::Reset(app.get());
    std::puts("animation_curve_host_tests: all checks passed");
}
