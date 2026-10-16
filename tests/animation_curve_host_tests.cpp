#define NOMINMAX
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/frame_presentation.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <commctrl.h>
namespace { thread_local long failAfter = -1; }
void* operator new(std::size_t size) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
using namespace mikudancestudio;
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
    const auto previousViewport = app.ViewportRect();
    app.ViewportRect().bottom = 700;
    const auto panels = FramePanelLayout(&app);
    const HWND label=CreateWindowExW(0,L"STATIC",L"Scale",WS_CHILD,640,panels.bottomBand.top+4,60,18,parent,
        reinterpret_cast<HMENU>(570),GetModuleHandleW(nullptr),nullptr);
    Check(label!=nullptr,"hidden scale caption");
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
    g_Block=previousApp;app.ViewportRect()=previousViewport;SelectObject(dc,oldFont);SelectObject(dc,oldImage);
    DeleteObject(image);DeleteDC(dc);DestroyWindow(label);
}
int main(){
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
    CheckScaleLabelRepaint(*app,parent);
    curve_host::LayoutControls(app.get());
    for(unsigned lang=0;lang<3;++lang){app->EnglishUI()=uint8_t(lang);curve_host::Localize(app.get());
        wchar_t text[64]{};GetWindowTextW(GetDlgItem(parent,472),text,64);Check(wcslen(text)>0,"localized load label");
        Check(GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)!=UINT(-1),"explicit accessory VMD menu exists");
    }
    curve_host::Open(app.get(),false);
    Check(GetForegroundWindow()!=parent,"hidden integration does not activate host");
    curve_editor::CloseOwned(parent);
    DestroyWindow(parent);app->Hwnd()=nullptr;
    curve_host::ForgetAccessory(app.get(),0);Check(!curve_host::Has(app.get(),curve_host::Kind::Accessory,0),"deleted object clears curves");
    curve_host::Reset(app.get());
    std::puts("animation_curve_host_tests: all checks passed");
}
