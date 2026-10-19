// Real MMS controls and DX9 render-target readback, kept entirely hidden.
#define NOMINMAX
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_scale_gizmo.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/frame_rate_correction.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <set>
using namespace mikudancestudio;
namespace fs=std::filesystem;
namespace {
int failures=0;
MMDApp* app=nullptr;
fs::path output;
void Check(bool ok,const char* what){std::printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok)++failures;}
LRESULT CALLBACK HiddenWindow(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR){
    // Later production ShowWindow calls must stay hidden as well as creation.
    if(message==WM_WINDOWPOSCHANGING){auto* position=reinterpret_cast<WINDOWPOS*>(l);position->flags&=~SWP_SHOWWINDOW;position->flags|=SWP_NOACTIVATE;}
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,HiddenWindow,id);
    return DefSubclassProc(window,message,w,l);
}
LRESULT CALLBACK Hide(int code,WPARAM w,LPARAM l){
    if(code==HCBT_CREATEWND){auto* c=reinterpret_cast<CBT_CREATEWNDW*>(l)->lpcs;if(!(c->style&WS_CHILD)){c->style&=~WS_VISIBLE;SetWindowSubclass(reinterpret_cast<HWND>(w),HiddenWindow,1,0);}}
    if(code==HCBT_ACTIVATE)return 1;
    return CallNextHookEx(nullptr,code,w,l);
}
void Frame(){app->state.messageSeen=1;app->DeltaTime()=1.f/30;FrameDriver(app);}
void Capture(HWND w,const fs::path& path){
    RECT r{};GetWindowRect(w,&r);int width=r.right-r.left,height=r.bottom-r.top;
    BITMAPINFO b{};b.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);b.bmiHeader.biWidth=width;b.bmiHeader.biHeight=-height;b.bmiHeader.biPlanes=1;b.bmiHeader.biBitCount=32;
    void* p=nullptr;HBITMAP bitmap=CreateDIBSection(nullptr,&b,DIB_RGB_COLORS,&p,nullptr,0);HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,bitmap);
    SendMessageW(w,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);GdiFlush();
    BITMAPFILEHEADER f{};f.bfType=0x4d42;f.bfOffBits=sizeof(f)+sizeof(b.bmiHeader);f.bfSize=f.bfOffBits+width*height*4;
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<char*>(&f),sizeof(f));file.write(reinterpret_cast<char*>(&b.bmiHeader),sizeof(b.bmiHeader));file.write(static_cast<char*>(p),width*height*4);
    Check(bool(file),"native window WM_PRINT capture");SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
RECT Rect(int id){RECT r{};GetWindowRect(GetDlgItem(app->MainWindow(),id),&r);return r;}
void Readback(const wchar_t* name){
    auto* device=app->Renderer()->device;IDirect3DSurface9 *target=nullptr,*read=nullptr;D3DSURFACE_DESC desc{};
    Check(SUCCEEDED(device->GetRenderTarget(0,&target)),"actual DX9 render target");if(!target)return;target->GetDesc(&desc);
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&read,nullptr)),"readback allocation");
    if(read){
        IDirect3DSurface9* resolved=nullptr;auto* source=target;
        if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE){Check(SUCCEEDED(device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)),"resolve surface");if(resolved){Check(SUCCEEDED(device->StretchRect(target,nullptr,resolved,nullptr,D3DTEXF_NONE)),"resolve multisampled frame");source=resolved;}}
        const HRESULT hr=device->GetRenderTargetData(source,read);Check(SUCCEEDED(hr),"synchronous GPU readback");if(SUCCEEDED(hr))Check(SUCCEEDED(d3dx::Get().saveSurfaceToFileW((output/name).c_str(),3,read,nullptr,nullptr)),"save rendered frame");if(resolved)resolved->Release();read->Release();}target->Release();
}
void Grid(){
    const int ids[2][3]={{490,494,501},{492,493,491}};
    for(int row=0;row<2;++row)for(int col=0;col<3;++col){auto r=Rect(ids[row][col]);
        if(col){auto l=Rect(ids[row][col-1]);Check(r.left>=l.right+2&&r.top==l.top,"actual bone button row alignment and gap");}
        if(row){auto p=Rect(ids[0][col]);Check(r.top>p.bottom&&r.left==p.left,"actual bone tool column alignment");}
        wchar_t text[128]{};auto w=GetDlgItem(app->MainWindow(),ids[row][col]);GetWindowTextW(w,text,128);HDC dc=GetDC(w);auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(w,WM_GETFONT,0,0)));SIZE s{};GetTextExtentPoint32W(dc,text,int(wcslen(text)),&s);SelectObject(dc,old);ReleaseDC(w,dc);Check(s.cx+4<=r.right-r.left,"button caption leaves inner horizontal margins");
    }
}
void Gizmo(){
    auto* raw=app->SelectedModel();auto* model=mdl::Mdl(raw);if(!model)return;
    int chosen=0;for(unsigned b=0;b<model->boneCount;++b)if(model->boneTable[b].position[1]>7&&model->boneTable[b].position[1]<13){chosen=b;break;}
    model->selectedBone=chosen;for(unsigned b=0;b<model->boneCount;++b)model->boneSelection[b]=b==unsigned(chosen);
    app->state.coordinateSystem=1;app->CameraYaw()=0;app->CameraPitch()=0;app->CameraRoll()=0;
    SendMessageW(GetDlgItem(app->MainWindow(),491),BM_CLICK,0,0);Check(app->EditMode()==ViewportEditMode::Scale,"scale button selects native scale mode");
    Frame();auto g=bone_scale::gizmo::Published(app);std::printf("gizmo %d origin %.1f %.1f plane %.1f %.1f axes %d %d %d\n",g.valid,g.originX,g.originY,g.planeX,g.planeY,g.axisAvailable[0],g.axisAvailable[1],g.axisAvailable[2]);
    Check(g.valid&&g.axisAvailable[0]&&g.axisAvailable[1]&&g.axisAvailable[2]&&
          std::hypot(g.tipX[2]-g.originX,g.tipY[2]-g.originY)>=8.0f,
          "actual rendering keeps the view-aligned scale axis visible");Readback(L"scale-front.png");
    if(g.valid){
        for(int axis=0;axis<2;++axis){
            Check(bone_scale::gizmo::HitTest(app,g.tipX[axis],g.tipY[axis])==bone_scale::gizmo::HandleOfAxis(axis),"painted square maps to matching axis hit region");
            bone_scale::SetPose(raw,chosen,{});app->PreviousMouseX()=int(g.tipX[axis]);app->PreviousMouseY()=int(g.tipY[axis]);app->MouseX()=app->PreviousMouseX()+(axis==0?12:0);app->MouseY()=app->PreviousMouseY()+(axis==1?-12:0);
            bone_scale::DragScreen(app,axis);auto value=bone_scale::Pose(raw,chosen);Check(value.v[axis*4]>1.01f,"real rendered axis responds to its projected drag direction");
        }
        bone_scale::SetPose(raw,chosen,{});app->PreviousMouseX()=int(g.originX);app->PreviousMouseY()=int(g.originY);app->MouseX()=app->PreviousMouseX()+12;app->MouseY()=app->PreviousMouseY();bone_scale::DragScreenPlane(app);auto value=bone_scale::Pose(raw,chosen);Check(value.v[0]>1.01f&&value.v[4]>1.01f&&std::abs(value.v[8]-1)<1.e-5f,"white plane horizontal drag scales two view axes only");
        bone_scale::SetPose(raw,chosen,{});
    }
    app->CameraYaw()=.55f;app->CameraPitch()=.3f;Frame();g=bone_scale::gizmo::Published(app);Check(g.valid&&g.axisAvailable[2],"turned camera reveals depth scale handle");Readback(L"scale-angled.png");
}
void Morphs(){
    auto* raw=app->SelectedModel();auto* model=mdl::Mdl(raw);std::vector<animation_curve::NamedTrack> authored;std::string reason;std::wstring error;
    int chosen[4]={-1,-1,-1,-1};
    for(const auto& info:morph_curve::Enumerate(raw,2))if(info.panel>=1&&info.panel<=4&&chosen[info.panel-1]<0){
        chosen[info.panel-1]=info.index;model->selectedMorphs[info.panel-1]=info.index;
        animation_curve::Track track;animation_curve::Key first,last;first.frame=0;first.value=0;first.tangent=animation_curve::Tangent::Free;last=first;last.frame=30;last.value=1;track.keys={first,last};
        authored.push_back({info.id,track});
    }
    Check(authored.size()==4,"real model exposes four expression panels");
    Check(morph_curve::Write(*app,authored,error),"author four native Bezier expression tracks");
    app->state.optflag[5]=1;curve_host::LayoutMorphControls(app);curve_host::ShowControls(app,false);
    const int combo[4]={504,509,514,519};
    for(int lane=0;lane<4;++lane){
        auto br=Rect(curve_host::kMorphCurveLane[lane]),cr=Rect(combo[lane]);
        Check(br.right<=cr.left&&br.top>=cr.top-2&&br.bottom<=cr.bottom+2,"expression curve launcher fits immediately before selector");
        Check(cr.right-cr.left>=60,"expression selector keeps readable width");
        Check(IsWindowEnabled(GetDlgItem(app->MainWindow(),curve_host::kMorphCurveLane[lane]))!=0,"valid current expression enables launcher");
    }
    Check(curve_host::WriteMorphComparisonMode(2,curve_editor::kCompareSameGroup,error),"store initial comparison preference");
    std::vector<curve_editor::Channel> channels;Check(curve_host::ReadMorphChannels(*app,channels,error),"read actual model expression channels");
    for(int lane=0;lane<4;++lane){
        SendMessageW(GetDlgItem(app->MainWindow(),curve_host::kMorphCurveLane[lane]),BM_CLICK,0,0);
        HWND curve=FindWindowW(L"MikuMikuStudioCurveEditorWindow",nullptr);Check(curve!=nullptr&&!IsWindowVisible(curve),"real expression launcher opens hidden production editor");if(!curve)continue;
        int info[12]{};SendMessageW(curve,WM_APP+308,0,reinterpret_cast<LPARAM>(info));
        const auto wanted=morph_curve::StableId(chosen[lane],model->morphs[chosen[lane]].name);
        Check(info[1]>=0&&info[1]<int(channels.size())&&channels[info[1]].id==wanted,"editor starts on lane current expression");
        for(auto pair:{std::pair<int,int>{1082,1},{1087,3},{1086,2}}){
            SendMessageW(GetDlgItem(curve,pair.first),BM_CLICK,0,0);SendMessageW(curve,WM_APP+308,0,reinterpret_cast<LPARAM>(info));
            Check(info[0]==pair.second&&SendMessageW(GetDlgItem(curve,pair.first),BM_GETCHECK,0,0)==BST_CHECKED,"clicking a segment immediately selects its comparison mode");
            Check(curve_host::ReadMorphComparisonMode()==pair.second,"comparison mode persists in native settings");
        }
        if(lane==0){
            for(unsigned language=0;language<3;++language){curve_editor::SetLanguage(app->MainWindow(),language);for(int width:{1000,836}){SetWindowPos(curve,nullptr,50,50,width,720,SWP_NOZORDER|SWP_NOACTIVATE);Capture(curve,output/(L"expression-"+std::to_wstring(language)+L"-"+std::to_wstring(width)+L".bmp"));}}
            HWND list=GetDlgItem(curve,1015);SendMessageW(list,WM_MOUSEMOVE,0,MAKELPARAM(4,4));wchar_t hint[512]{};GetWindowTextW(GetDlgItem(curve,1085),hint,512);Check(*hint!=0,"hovering expression list explains operation below");SendMessageW(list,WM_MOUSELEAVE,0,0);GetWindowTextW(GetDlgItem(curve,1085),hint,512);Check(*hint==0,"leaving clears hover instructions");
        }
        SendMessageW(curve,WM_CLOSE,0,0);
    }
    // Normal model VMD entry point, with all authored keys selected for export.
    const auto path=output/L"native-expression.vmd";app->CurrentFrame()=0;SaveVmdFile(path.c_str());Check(fs::exists(path)&&fs::file_size(path)>100,"normal model VMD includes expression motion");
    morph_curve::Forget(raw);Check(!morph_curve::AnyCurve(raw),"drop live curves before import");LoadVmdFile(path.c_str());
    Check(morph_curve::CurveCount(raw)>=4,"normal model VMD reader restores native expression curves");
    animation_curve::Track restored;Check(morph_curve::Read(raw,chosen[0],restored,reason),"read imported curve");
    Check(std::abs(animation_curve::Evaluate(restored,7.5).value-animation_curve::Evaluate(authored[0].track,7.5).value)<1.e-5,"VMD roundtrip preserves non-linear expression sampling");
}
int saveDialogs = 0;
std::set<HWND> saveDialogWindows;
BOOL CALLBACK CancelSaveDialog(HWND window, LPARAM) {
    wchar_t type[80]{}; GetClassNameW(window, type, 80);
    if(wcscmp(type,L"#32770")!=0)return TRUE;
    // The production GetSaveFileNameW is active here. Cancel it through its
    // normal button; the WH_CBT hook keeps its window off the user's desktop.
    const bool firstSeen=saveDialogWindows.insert(window).second;
    if(firstSeen)++saveDialogs;
    wchar_t title[256]{};GetWindowTextW(window,title,256);
    std::fwprintf(stderr,L"Save-dialog HWND=%p first=%d title=%ls\n",window,firstSeen?1:0,title);
    Check(!IsWindowVisible(window),"automatic native Save As dialog remains hidden in test");
    SendMessageW(window,WM_COMMAND,IDCANCEL,0);
    return FALSE;
}
void CALLBACK SaveDialogTimer(HWND,UINT,UINT_PTR,DWORD) {
    EnumThreadWindows(GetCurrentThreadId(),CancelSaveDialog,0);
}
std::wstring ControlText(HWND window,int id){
    auto control=GetDlgItem(window,id);int n=GetWindowTextLengthW(control);
    std::wstring text(n+1,L'\0');GetWindowTextW(control,text.data(),n+1);text.resize(n);return text;
}
void FrameRate(){
    namespace frc=frame_rate_correction;
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::ReadMorphChannels(*app,channels,error)&&!channels.empty(),"read live morphs for compression");
    if(channels.empty())return;
    animation_curve::Track source;
    for(auto pair:{std::pair<int,double>{0,0},{1,1},{2,.3},{30,.9}}){animation_curve::Key key;key.frame=pair.first;key.value=pair.second;key.interpolation=animation_curve::Interpolation::Bezier;source.keys.push_back(key);}
    std::vector<animation_curve::NamedTrack> authored{{channels.front().id,source}};std::string detail;
    Check(morph_curve::Write(app->SelectedModel(),authored,detail),"author close keys that collide under FPS compression");
    app->EnglishUI()=2;LocalizeUI(app);app->CurrentFrame()=0;
    frc::RefreshMenu(app);
    Check(GetMenuState(GetSubMenu(GetMenu(app->MainWindow()),1),frc::kMenuCommand,MF_BYCOMMAND)!=UINT(-1),"Edit menu contains native frame-rate correction");
    SendMessageW(app->MainWindow(),WM_COMMAND,frc::kMenuCommand,0);
    HWND window=FindWindowW(L"MikuMikuStudioFrameRateCorrection",nullptr);
    if(!window)window=FindWindowW(L"MikuMikuStudioFrameRateCorrectionWindow",nullptr);
    Check(window!=nullptr&&!IsWindowVisible(window),"Edit command opens production FPS window in background");
    if(!window)return;
    SetDlgItemTextW(window,1302,L"120");SetDlgItemTextW(window,1304,L"30");
    SendMessageW(GetDlgItem(window,1305),BM_CLICK,0,0);
    Check(IsWindowEnabled(GetDlgItem(window,1306))!=0,"real preview validates complete project for compression");
    if(!IsWindowEnabled(GetDlgItem(window,1306))){std::fwprintf(stderr,L"FPS status: %ls\n",ControlText(window,1310).c_str());SendMessageW(window,WM_CLOSE,0,0);return;}
    Capture(window,output/L"frame-rate-preview.bmp");
    const auto timer=SetTimer(nullptr,0,40,SaveDialogTimer);
    SendMessageW(GetDlgItem(window,1306),BM_CLICK,0,0);
    KillTimer(nullptr,timer);
    Check(saveDialogs==1,"successful correction automatically opens exactly one real Save As dialog");
    animation_curve::Track restored;
    const auto* model=mdl::Mdl(app->SelectedModel());
    int morph=0;for(unsigned i=0;i<model->morphCount;++i)if(morph_curve::StableId(i,model->morphs[i].name)==channels.front().id){morph=int(i);break;}
    Check(morph_curve::Read(app->SelectedModel(),morph,restored,detail),"read converted native morph after cancelling Save As");
    Check(restored.keys.size()==3&&restored.keys[1].frame==1&&restored.keys.back().frame==8,"cancel Save As keeps converted compressed animation in memory");
    const auto report=ControlText(window,1309);
    Check(report.find(L"0.25")!=std::wstring::npos&&report.find(L"30")!=std::wstring::npos,"report retains specific rounded and omitted frame details after Save As cancellation");
    Capture(window,output/L"frame-rate-report.bmp");
    std::ofstream file(output/L"frame-rate-report.txt",std::ios::binary);
    int n=WideCharToMultiByte(CP_UTF8,0,report.data(),int(report.size()),nullptr,0,nullptr,nullptr);std::string utf8(n,'\0');WideCharToMultiByte(CP_UTF8,0,report.data(),int(report.size()),utf8.data(),n,nullptr,nullptr);file<<utf8;
    SendMessageW(window,WM_CLOSE,0,0);
    Frame();Readback(L"frame-rate-after.png");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3)return 2;std::setvbuf(stdout,nullptr,_IONBF,0);output=fs::absolute(argv[2]);fs::create_directories(output);
    auto hook=SetWindowsHookExW(WH_CBT,Hide,nullptr,GetCurrentThreadId());
    app=new MMDApp;g_Block=app;app->state=MMDAppState{};app->InitDefaults();
    if(!InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE))return 3;
    Check(!IsWindowVisible(app->MainWindow()),"production MMS remains hidden");Frame();LoadModelFile(app,fs::absolute(argv[1]).c_str(),false);Check(app->SelectedModel()!=nullptr,"real PMX loaded");Frame();
    for(unsigned language=0;language<3;++language){app->EnglishUI()=static_cast<unsigned char>(language);LocalizeUI(app);Frame();Grid();Capture(app->MainWindow(),output/(L"host-"+std::to_wstring(language)+L".bmp"));}
    app->EditMode()=ViewportEditMode::None;
    for(auto expected:{ViewportEditMode::Bone,ViewportEditMode::None,ViewportEditMode::Bone}){SendMessageW(app->MainWindow(),WM_COMMAND,490,0);Check(app->EditMode()==expected,"selection button toggles select on and off");}
    Gizmo();Morphs();FrameRate();
    ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(hook);
    std::printf("native editor integration failures=%d\n",failures);return failures?1:0;
}
