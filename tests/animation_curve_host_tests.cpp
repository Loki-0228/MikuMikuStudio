#define NOMINMAX
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cmath>
#include <commctrl.h>
using namespace mikumikustudio;
static void Check(bool b,const char*s){if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);}}
static void CheckScaleLabelRepaint(MMDApp& app,HWND parent){
    const auto panels=FramePanelLayout(&app);
    // Use the same bottom-row coordinates as LayoutViewportPanels. The main
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
#include "camera_subdivision_checks.hpp"
int main(){
    CheckCameraSubdivision();
    CheckCrossedCameraEndpoints();
    CheckCameraHoldSamples();
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
    for(auto kind:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow,curve_host::Kind::Gravity,curve_host::Kind::Accessory}){
        Check(curve_host::Read(*app,kind,0,channels,error),"read non-bone insertion fixture");
        const std::size_t lane=kind==curve_host::Kind::Shadow?1:0;
        const auto before=channels[lane].track;
        if(kind==curve_host::Kind::Camera)Check(curve_host::InsertCameraKey(channels,lane,15,error),"insert native camera authored key");
        else {std::size_t index=0;std::string detail;
            Check(animation_curve::InsertPreservingShape(channels[lane].track,15,index,detail)==animation_curve::InsertResult::Inserted,"insert non-bone key preserving shape");
        }
        Check(curve_host::Write(*app,kind,0,channels,error),"commit non-bone inserted key");
        for(int frame=0;frame<=30;++frame)
            Check(std::fabs(animation_curve::Evaluate(channels[lane].track,frame).value-animation_curve::Evaluate(before,frame).value)<1e-9,"non-bone insertion keeps the evaluated curve");
    }
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
        wchar_t text[128]{};GetWindowTextW(GetDlgItem(parent,472),text,128);Check(wcslen(text)>0,"localized load label");
        Check(GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)!=UINT(-1),"explicit accessory VMD menu exists");
        GetMenuStringW(file,curve_editor::kAccessoryMotionMenu,text,128,MF_BYCOMMAND);
        Check(std::wstring(text)==(lang==2?L"载入当前附件动作（VMD）…":lang==1?
            L"Load selected accessory motion (VMD)…":L"選択中のアクセサリのモーション読込（VMD）…"),"accessory motion menu uses the load caption");
        for(int mode:{0,1,0,1}){
            app->CameraMode()=mode;curve_host::Localize(app.get());
            Check((GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)&MF_GRAYED)==(mode?0:MF_GRAYED),
                "accessory motion menu is available only in camera mode");
            SendMessageW(parent,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(file),0);
            Check((GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)&MF_GRAYED)==(mode?0:MF_GRAYED),
                "opening the menu preserves its mode-dependent availability");
        }
        auto* selected=app->AccessorySlot(0);app->AccessorySlot(0)=nullptr;curve_host::Localize(app.get());
        Check((GetMenuState(file,curve_editor::kAccessoryMotionMenu,MF_BYCOMMAND)&MF_GRAYED)!=0,
            "camera mode with no selected accessory cannot import accessory motion");
        app->AccessorySlot(0)=selected;app->CameraMode()=0;curve_host::Localize(app.get());
        SendMessageW(parent,WM_COMMAND,curve_editor::kAccessoryMotionMenu,0);
    }
    curve_host::Open(app.get(),false);
    Check(GetForegroundWindow()!=parent,"hidden integration does not activate host");
    curve_editor::CloseOwned(parent);
    DestroyWindow(parent);app->Hwnd()=nullptr;
    curve_host::ForgetAccessory(app.get(),0);Check(!curve_host::Has(app.get(),curve_host::Kind::Accessory,0),"deleted object clears curves");
    curve_host::Reset(app.get());
    std::puts("animation_curve_host_tests: all checks passed");
}
