// MAINTENANCE: integration regression; hidden real renderer and Ogawa roundtrip.
#define NOMINMAX
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <MinHook.h>
#include <CommCtrl.h>
using namespace mikumikustudio;
namespace mikumikustudio { LRESULT CALLBACK EditSubclassProc(HWND,UINT,WPARAM,LPARAM); }
namespace Abc=Alembic::Abc;
namespace Geom=Alembic::AbcGeom;
namespace {
void Check(bool ok,const char* why){if(!ok){std::printf("FAIL: %s\n",why);std::exit(1);}}
std::filesystem::path uiOutput;
void Capture(HWND window,const std::filesystem::path& path){
    RECT r{};GetWindowRect(window,&r);const int w=r.right-r.left,h=r.bottom-r.top;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    GdiFlush();BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+w*h*4;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&file),sizeof(file));out.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));out.write(static_cast<char*>(pixels),w*h*4);
    Check(bool(out),"capture actual window memory target");SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
bool inspectExport=false;int warningAnswer=IDNO,warningCount=0,unexpectedDialogs=0,clearScope=-1;
decltype(&ShowWindow) realShowWindow=nullptr;
int restoredRecordingWindows=0;
BOOL WINAPI RecordingWindowRestore(HWND window,int show){
    if(window==g_Block->MainWindow()&&show==SW_SHOW){
        ++restoredRecordingWindows;
        Check(g_Block->FrameStepPlayback()!=0,"recording guard covers window focus restoration");
        const auto edit=GetDlgItem(window,panel::kCurrentFrameEdit);
        SetWindowTextA(edit,"0");
        EditSubclassProc(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(window),0);
        return FALSE;  // Keep the production host hidden during the test.
    }
    return realShowWindow(window,show);
}
bool inspectLayout=false;int workWidth=0,workHeight=0,layoutFailures=0;
decltype(&GetMonitorInfoW) realMonitorInfo=nullptr;
BOOL WINAPI SmallWorkArea(HMONITOR monitor,LPMONITORINFO info){
    const BOOL result=realMonitorInfo(monitor,info);
    if(result&&workWidth){info->rcWork.right=info->rcWork.left+workWidth;info->rcWork.bottom=info->rcWork.top+workHeight;}
    return result;
}
LRESULT CALLBACK PrintClippedChild(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(message!=WM_PRINT)return DefSubclassProc(window,message,wp,lp);
    // WM_PRINT's child traversal bypasses the normal window DC clipping region.
    const auto dc=reinterpret_cast<HDC>(wp);const int saved=SaveDC(dc);const auto region=CreateRectRgn(0,0,0,0);
    if(GetWindowRgn(window,region)!=ERROR){POINT origin{};LPtoDP(dc,&origin,1);OffsetRgn(region,origin.x,origin.y);ExtSelectClipRgn(dc,region,RGN_AND);}
    const auto result=DefSubclassProc(window,message,wp,lp);RestoreDC(dc,saved);DeleteObject(region);return result;
}
void CheckLayout(HWND window){
    RECT outer{},client{};GetWindowRect(window,&outer);GetClientRect(window,&client);
    MONITORINFO monitor{sizeof monitor};Check(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"layout monitor query");
    const auto contains=[](const RECT& a,const RECT& b){return b.left>=a.left&&b.top>=a.top&&b.right<=a.right&&b.bottom<=a.bottom;};
    if(!contains(monitor.rcWork,outer)){++layoutFailures;std::printf("FAIL: dialog outside %dx%d work area\n",workWidth,workHeight);}
    RECT buttons[3]{};int index=0;
    for(int id:{IDOK,IDCANCEL,128}){
        auto& bounds=buttons[index++];Check(GetWindowRect(GetDlgItem(window,id),&bounds)!=FALSE,"layout button exists");
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
        if(!contains(client,bounds)){++layoutFailures;std::printf("FAIL: button %d outside client\n",id);}
    }
    for(int i=0;i<3;++i)for(int j=i+1;j<3;++j){RECT overlap{};if(IntersectRect(&overlap,&buttons[i],&buttons[j])){++layoutFailures;std::puts("FAIL: footer buttons overlap");}}
    SCROLLINFO scroll{sizeof scroll,SIF_ALL};GetScrollInfo(window,SB_VERT,&scroll);
    Check(scroll.nMax>=int(scroll.nPage),"small work area exposes content scrolling");
    SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0);
    RECT status{};GetWindowRect(GetDlgItem(window,125),&status);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&status),2);
    Check(status.top>=0&&status.bottom<buttons[0].top,"scroll reaches status without covering footer");
    for(int i=0;i<3;++i){RECT bounds{};GetWindowRect(GetDlgItem(window,i==0?IDOK:i==1?IDCANCEL:128),&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);Check(EqualRect(&bounds,&buttons[i]),"footer stays fixed while scrolling");}
    SendMessageW(GetDlgItem(window,120),WM_SETFOCUS,0,0);
    RECT first{};GetWindowRect(GetDlgItem(window,120),&first);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&first),2);
    Check(contains(client,first)&&first.bottom<buttons[0].top,"keyboard focus reveals the first frame edit");
    SendMessageW(GetDlgItem(window,131),WM_SETFOCUS,0,0);
    RECT edit{};GetWindowRect(GetDlgItem(window,131),&edit);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&edit),2);
    Check(contains(client,edit)&&edit.bottom<buttons[0].top,"keyboard focus reveals lower and right-hand fields");
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SetWindowSubclass(child,PrintClippedChild,2,0);
    Capture(window,uiOutput/(L"layout-"+std::to_wstring(workWidth)+L"-"+std::to_wstring(g_Block->EnglishUI())+L".bmp"));
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))RemoveWindowSubclass(child,PrintClippedChild,2);
    std::printf("layout work=%dx%d language=%d window=%ldx%ld failures=%d\n",workWidth,workHeight,g_Block->EnglishUI(),outer.right-outer.left,outer.bottom-outer.top,layoutFailures);
}
bool rejectKeyframeDialog=false;int rejectedKeyframeDialogs=0,keyframeGuardFailures=0;
int inspectBakeRun=0,lockedProgress=0;HWND enabledEditor=nullptr,disabledEditor=nullptr;
LRESULT CALLBACK Dialogs(int code,WPARAM wp,LPARAM lp){
    if(code>=0){const auto& event=*reinterpret_cast<CWPRETSTRUCT*>(lp);
        if(inspectBakeRun&&event.message==WM_SETTEXT&&GetDlgCtrlID(event.hwnd)==125){
            const HWND d=GetParent(event.hwnd);
            if(!IsWindowEnabled(GetDlgItem(d,IDOK))){
                ++lockedProgress;
                Check(!IsWindowEnabled(enabledEditor)&&!IsWindowEnabled(disabledEditor),"baking disables other editing windows");
                Check(IsWindowEnabled(d)&&IsWindowEnabled(GetDlgItem(d,IDCANCEL)),"progress and cancellation remain enabled");
                for(int id:{120,121,122,123,126,127,128,IDOK})Check(!IsWindowEnabled(GetDlgItem(d,id)),"baking disables dialog inputs");
                const int frame=g_Block->CurrentFrame(),dialogsBefore=unexpectedDialogs;
                SendMessageW(g_Block->MainWindow(),WM_KEYDOWN,VK_RIGHT,0);
                SendMessageW(g_Block->MainWindow(),WM_COMMAND,bake::kBakeMenuCommand,0);
                SendMessageW(g_Block->MainWindow(),WM_CLOSE,0,0);
                Check(g_Block->CurrentFrame()==frame&&unexpectedDialogs==dialogsBefore&&IsWindow(g_Block->MainWindow()),"queued main-window commands and navigation cannot edit or close the scene");
                const int warnings=warningCount;
                SendMessageW(d,WM_COMMAND,MAKEWPARAM(127,BN_CLICKED),0);
                SendMessageW(d,WM_COMMAND,IDOK,0);
                Check(warningCount==warnings,"queued editing commands are ignored while baking");
                if(inspectBakeRun==1)SendMessageW(d,WM_COMMAND,IDCANCEL,0);
            }
        }
        if(event.message==WM_INITDIALOG){
            const auto window=event.hwnd;
            if(inspectBakeRun&&GetDlgItem(window,126)){
                CheckDlgButton(window,126,BST_UNCHECKED);CheckDlgButton(window,127,BST_CHECKED);
                SendMessageW(window,WM_COMMAND,IDOK,0);
                Check(IsWindowEnabled(enabledEditor)&&!IsWindowEnabled(disabledEditor),"baking restores only previously enabled windows");
                Check(IsWindowEnabled(GetDlgItem(window,IDOK))&&IsWindowEnabled(GetDlgItem(window,120)),"baking restores its controls after completion or cancellation");
                EndDialog(window,IDCANCEL);
            }else if(rejectKeyframeDialog&&GetDlgItem(window,100)){
                ++rejectedKeyframeDialogs;EndDialog(window,IDCANCEL);
            }else if(inspectExport&&GetDlgItem(window,126)){
                if(inspectLayout){CheckLayout(window);EndDialog(window,IDCANCEL);return CallNextHookEx(nullptr,code,wp,lp);}
                const int before=warningCount;
                Check(IsDlgButtonChecked(window,126)==BST_CHECKED&&IsDlgButtonChecked(window,127)==BST_UNCHECKED,"export defaults to ABC only");
                SendDlgItemMessageW(window,126,BM_CLICK,0,0);SendDlgItemMessageW(window,126,BM_CLICK,0,0);
                Check(warningCount==before,"ABC selection never shows project-bake warnings");
                SendDlgItemMessageW(window,127,BM_CLICK,0,0);
                Check(IsDlgButtonChecked(window,127)==BST_CHECKED&&warningCount==before,"native range bake is selected without changing MME assignments");
                Check(GetDlgItem(window,129)&&GetDlgItem(window,130)&&GetDlgItem(window,131),"mesh source list and material/vertex controls are present");
                Capture(window,uiOutput/(L"mesh-selector-"+std::to_wstring(g_Block->EnglishUI())+L".bmp"));
                SendDlgItemMessageW(window,127,BM_CLICK,0,0);
                Check(IsDlgButtonChecked(window,127)==BST_UNCHECKED,"project baking can be deselected");
                EndDialog(window,IDCANCEL);
            }else if(inspectExport&&GetDlgItem(window,IDYES)&&GetDlgItem(window,IDNO)){
                ++warningCount;std::wstring text;
                for(auto child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){wchar_t value[4096]{};GetWindowTextW(child,value,4096);text+=value;}
                Check(text.find(L".mdsmesh.abc")!=text.npos&&text.find(L"IK")!=text.npos&&text.find(L"FX")!=text.npos,"warning covers cache files, animation and effects");
                EndDialog(window,warningAnswer);
            }else if(clearScope>=0&&GetDlgItem(window,132)){
                const int id=clearScope?131:130;Check(IsWindowEnabled(GetDlgItem(window,id)),"requested clear scope is enabled");
                SendDlgItemMessageW(window,id,BM_CLICK,0,0);SendMessageW(window,WM_COMMAND,IDOK,0);
            }else{++unexpectedDialogs;wchar_t title[256]{},body[2048]{};GetWindowTextW(window,title,256);GetDlgItemTextW(window,65535,body,2048);std::printf("unexpected dialog: %ls: %ls\n",title,body);EndDialog(window,IDCANCEL);}
        }
    }return CallNextHookEx(nullptr,code,wp,lp);
}
LRESULT CALLBACK Hidden(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_CREATEWND)reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style&=~WS_VISIBLE;
    if(code==HCBT_ACTIVATE){ShowWindow(reinterpret_cast<HWND>(wp),SW_HIDE);return 1;}
    return CallNextHookEx(nullptr,code,wp,lp);
}
template<class T>void Put(std::ofstream& f,T value){f.write(reinterpret_cast<char*>(&value),sizeof value);}
void Text(std::ofstream& f,const wchar_t* s){Put(f,uint32_t(wcslen(s)*2));f.write(reinterpret_cast<const char*>(s),wcslen(s)*2);}
void Fixture(const std::filesystem::path& path){
    std::ofstream f(path,std::ios::binary);f.write("PMX ",4);Put(f,2.f);
    for(uint8_t v:{8,0,0,1,1,1,1,1,1})Put(f,v);
    Text(f,L"ABC triangle");Text(f,L"ABC triangle");Text(f,L"");Text(f,L"");
    Put(f,3u);
    for(int i=0;i<3;++i){
        Put(f,i==1?1.f:0.f);Put(f,i==2?11.f:10.f);Put(f,0.f);
        Put(f,0.f);Put(f,0.f);Put(f,-1.f);Put(f,i==1?1.f:0.f);Put(f,i==2?1.f:0.f);
        Put(f,uint8_t(0));Put(f,int8_t(0));Put(f,1.f);
    }
    Put(f,3u);Put(f,uint8_t(0));Put(f,uint8_t(1));Put(f,uint8_t(2));Put(f,0u);
    Put(f,1u);Text(f,L"surface");Text(f,L"surface");
    for(float v:{.8f,.8f,.8f,1.f,0.f,0.f,0.f,1.f,.4f,.4f,.4f})Put(f,v);
    Put(f,uint8_t(1));for(int i=0;i<4;++i)Put(f,0.f);Put(f,0.f);
    Put(f,int8_t(-1));Put(f,int8_t(-1));Put(f,uint8_t(0));Put(f,uint8_t(1));Put(f,uint8_t(0));Text(f,L"");Put(f,3u);
    Put(f,1u);Text(f,L"root");Text(f,L"root");for(int i=0;i<3;++i)Put(f,0.f);
    Put(f,int8_t(-1));Put(f,0u);Put(f,uint16_t(0x1a));for(int i=0;i<3;++i)Put(f,0.f);
    Put(f,0u);Put(f,1u);Text(f,L"Root");Text(f,L"Root");Put(f,uint8_t(1));Put(f,1u);Put(f,uint8_t(0));Put(f,int8_t(0));
    Put(f,0u);Put(f,0u);Check(bool(f),"write fixture PMX");
}
void AttributeFixture(const std::filesystem::path& path){
    std::ofstream f(path,std::ios::binary);f.write("PMX ",4);Put(f,2.f);
    for(uint8_t v:{8,0,4,1,1,1,1,1,1})Put(f,v); // Four additional UV sets.
    Text(f,L"Mapped attributes");Text(f,L"Mapped attributes");Text(f,L"");Text(f,L"");
    Put(f,4u);
    for(int i=0;i<4;++i){
        Put(f,float(i%2));Put(f,10.f+float(i/2));Put(f,0.f);
        Put(f,0.f);Put(f,0.f);Put(f,-1.f);Put(f,float(i%2));Put(f,float(i/2));
        for(int c=0;c<16;++c)Put(f,float(i*16+c+1)*.125f);
        Put(f,uint8_t(0));Put(f,int8_t(0));Put(f,.25f*float(i+1));
    }
    Put(f,6u);for(uint8_t i:{2,0,1,2,3,0})Put(f,i);Put(f,0u);
    Put(f,2u);
    for(int material=0;material<2;++material){
        Text(f,L"surface");Text(f,L"surface");
        for(float v:{.8f,.8f,.8f,1.f,0.f,0.f,0.f,1.f,.4f,.4f,.4f})Put(f,v);
        Put(f,uint8_t(0x11));for(float v:{0.f,0.f,0.f,1.f})Put(f,v);Put(f,1.f);
        Put(f,int8_t(-1));Put(f,int8_t(-1));Put(f,uint8_t(0));Put(f,uint8_t(1));Put(f,uint8_t(0));Text(f,L"");Put(f,3u);
    }
    Put(f,1u);Text(f,L"root");Text(f,L"root");for(int i=0;i<3;++i)Put(f,0.f);
    Put(f,int8_t(-1));Put(f,0u);Put(f,uint16_t(0x1a));for(int i=0;i<3;++i)Put(f,0.f);
    Put(f,0u);Put(f,1u);Text(f,L"Root");Text(f,L"Root");Put(f,uint8_t(1));Put(f,1u);Put(f,uint8_t(0));Put(f,int8_t(0));
    Put(f,0u);Put(f,0u);Check(bool(f),"write mapped attribute PMX");
}
struct Sample{std::vector<Abc::V3f> positions;size_t faces=0;};
std::vector<Sample> Read(const std::filesystem::path& path,int samples,double start,double step){
    std::ifstream stream(path,std::ios::binary);std::vector<std::istream*> streams{&stream};
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(streams),"stream");
    std::vector<Sample> output(samples);auto root=archive.getTop();Check(root.getNumChildren()>0,"ABC contains meshes");
    for(size_t child=0;child<root.getNumChildren();++child){
        Geom::IPolyMesh mesh(root,root.getChildHeader(child).getName());auto& schema=mesh.getSchema();
        Check(schema.getNumSamples()==samples,"mesh sample count");
        for(int i=0;i<samples;++i){
            Check(std::abs(schema.getTimeSampling()->getSampleTime(i)-(start+i*step))<1e-9,"sample time");
            auto sample=schema.getValue(Abc::ISampleSelector(Abc::index_t(i)));
            const auto p=sample.getPositions();
            const auto n=schema.getNormalsParam().getExpandedValue(Abc::ISampleSelector(Abc::index_t(i))).getVals();
            Check(n&&n->size()==p->size(),"normals cover exported vertices");
            auto uv=schema.getUVsParam().getExpandedValue(Abc::ISampleSelector(Abc::index_t(i))).getVals();
            Check(uv&&uv->size()==p->size(),"UVs cover exported vertices");
            const auto indices=sample.getFaceIndices(),counts=sample.getFaceCounts();
            for(size_t j=0;j<indices->size();++j)Check((*indices)[j]>=0&&size_t((*indices)[j])<p->size(),"face index bounds");
            for(size_t j=0;j<counts->size();++j)Check((*counts)[j]==3,"triangle topology");
            output[i].faces+=sample.getFaceCounts()->size();
            for(size_t j=0;j<p->size();++j)output[i].positions.push_back((*p)[j]);
        }
    }
    for(auto& sample:output)std::sort(sample.positions.begin(),sample.positions.end(),[](const auto& a,const auto& b){
        if(std::abs(a.y-b.y)>.001f)return a.y<b.y;if(std::abs(a.z-b.z)>.001f)return a.z<b.z;return a.x<b.x;});
    return output;
}
void Same(const std::vector<Sample>& actual,const std::vector<Sample>& expected,const char* why){
    Check(actual.size()==expected.size(),why);
    for(size_t frame=0;frame<actual.size();++frame){Check(actual[frame].faces==expected[frame].faces&&actual[frame].positions.size()==expected[frame].positions.size(),why);
        for(size_t i=0;i<actual[frame].positions.size();++i){
            const auto& a=actual[frame].positions[i];const auto& e=expected[frame].positions[i];
            if((a-e).length()>=.02f)std::printf("mismatch frame=%zu vertex=%zu actual=(%g,%g,%g) expected=(%g,%g,%g)\n",frame,i,a.x,a.y,a.z,e.x,e.y,e.z);
            Check((a-e).length()<.02f,why);
        }}
}
void Frame(MMDApp& app){app.DeltaTime()=1.f/30;app.state.messageSeen=1;FrameDriver(&app);}
std::vector<float> VertexFloats(void* pointer,unsigned count,unsigned floats){
    auto* buffer=static_cast<IDirect3DVertexBuffer9*>(pointer);Check(buffer!=nullptr,"vertex buffer exists");
    D3DVERTEXBUFFER_DESC description{};Check(SUCCEEDED(buffer->GetDesc(&description)),"vertex buffer size");
    Check(size_t(count)*floats*sizeof(float)<=description.Size,"vertex readback bounds");
    void* bytes=nullptr;Check(SUCCEEDED(buffer->Lock(0,0,&bytes,D3DLOCK_READONLY)),"lock vertex buffer for attribute check");
    std::vector<float> result(size_t(count)*floats);std::memcpy(result.data(),bytes,result.size()*sizeof(float));buffer->Unlock();return result;
}
void CheckVertexValue(float actual,float expected,size_t row,size_t component,const char* why){
    const bool equal=std::isfinite(actual)&&std::abs(actual-expected)<.0001f;
    if(!equal)std::printf("vertex mismatch row=%zu component=%zu actual=%.9g expected=%.9g: %s\n",row,component,actual,expected,why);
    Check(equal,why);
}
void AttributeRoundtrip(MMDApp& app,const std::filesystem::path& out,bool benchmark){
    const auto modelPath=out/L"attributes.pmx";AttributeFixture(modelPath);LoadModelFile(&app,modelPath.c_str(),false);
    auto* raw=app.ModelSlot(0);Check(raw!=nullptr,"load mapped attributes fixture");auto& model=*mdl::Mdl(raw);
    Check(model.vertexCount==4&&model.materialCount==2&&model.pmxAdditionalUvCount==4,"attribute fixture structure");
    for(unsigned i=0;i<4;++i)model.pmxVertices[i].hasPmxEdgeData=i%2?1:0;
    // FrameDriver evaluates the loaded pose after rendering. The following
    // capture frame skins that pose and uploads both buffers before readback.
    Frame(app);
    native_mesh::SetBakeCapture(true);Frame(app);
    std::vector<deformed_mesh_capture::Mesh> captured;std::string error;
    Check(native_mesh::ReadBakeCapture(captured,error),"read production capture");native_mesh::SetBakeCapture(false);
    const auto original=VertexFloats(model.vertexBuffer,4,24),originalEdges=VertexFloats(model.vertexBuffer2,4,4);
    for(unsigned i=0;i<4;++i){
        const float position[]{float(i%2),10.f+float(i/2),0.f},normal[]{0.f,0.f,-1.f};
        for(unsigned c=0;c<3;++c){
            CheckVertexValue(original[i*24+c],position[c],i,c,"fixture main buffer contains the evaluated pose");
            CheckVertexValue(original[i*24+3+c],normal[c],i,c,"fixture main buffer contains skinned normals");
        }
        for(unsigned c=0;c<2;++c)
            CheckVertexValue(originalEdges[i*4+c],position[c],i,c,"fixture outline contains the evaluated pose");
        if(i%2)Check(std::isfinite(originalEdges[i*4+2])&&originalEdges[i*4+2]<0,"fixture enabled outline is expanded");
        else CheckVertexValue(originalEdges[i*4+2],.005f,i,2,"fixture disabled outline is inset");
    }
    CheckVertexValue(originalEdges[3*4+2],2*originalEdges[1*4+2],3,2,"fixture outline uses the non-unit edge scales");
    const std::vector<std::vector<uint32_t>> expected{{2,1,0},{2,0,3}};
    Check(captured.size()==expected.size(),"capture preserves two material partitions");
    for(size_t i=0;i<expected.size();++i)Check(captured[i].sourceVertices==expected[i],"capture records reordered and repeated original rows");

    using Clock=std::chrono::steady_clock;
    alembic_export::Options options;options.lastFrame=benchmark?29:2;options.bakeIntoProject=true;
    const unsigned warmup=benchmark?1:0,iterations=benchmark?3:1;
    double exportMs=0;std::filesystem::path archive;
    for(unsigned iteration=0;iteration<warmup+iterations;++iteration){
        native_mesh::ClearMeshBake(&app,false);
        archive=out/(L"attributes-"+std::to_wstring(iteration)+L".abc");
        const auto begin=Clock::now();const auto result=alembic_export::Export(app,archive,options);
        const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        Check(result.success&&result.baked&&result.abcWritten,"real export captures, writes and installs complete bake");
        if(iteration>=warmup)exportMs+=elapsed;
    }
    unsigned explicitMaps=0;
    {
        std::ifstream stream(archive,std::ios::binary);std::vector<std::istream*> streams{&stream};
        Abc::IArchive abc(Alembic::AbcCoreOgawa::ReadArchive(streams),"stream");
        for(size_t i=0;i<abc.getTop().getNumChildren();++i){
            Geom::IPolyMesh mesh(abc.getTop(),abc.getTop().getChildHeader(i).getName());
            const auto properties=mesh.getSchema().getUserProperties();
            if(properties.valid()&&properties.getPropertyHeader("mdsSourceVertices")){
                ++explicitMaps;
                const auto ids=Abc::IUInt32ArrayProperty(properties,"mdsSourceVertices").getValue();
                if(!benchmark)Check(ids&&ids->size()==expected[i].size()&&std::equal(expected[i].begin(),expected[i].end(),ids->get()),"ABC retains explicit material source rows");
            }
        }
    }
    std::vector<deformed_mesh_capture::Mesh> samples;
    Check(mesh_bake_cache::Sample(raw,0,samples,error),"read the installed production archive");
    if(!benchmark){
        Check(explicitMaps==2,"complete project bake writes source properties for both materials");
        Check(samples.size()==expected.size(),"cache restores two material partitions");
        for(size_t i=0;i<expected.size();++i)Check(samples[i].sourceVertices==expected[i],"cache retains explicit source rows");
    }
    const unsigned replayWarmup=10,replayIterations=benchmark?100:1;
    for(unsigned i=0;i<replayWarmup;++i)Frame(app);
    const auto begin=Clock::now();
    for(unsigned i=0;i<replayIterations;++i)Frame(app);
    const auto replayMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    auto* render=native_mesh::RenderModel(raw);Check(render!=raw,"complete cache publishes real render proxy");
    const auto& proxy=*mdl::Mdl(render);Check(proxy.vertexCount==6,"material partition duplicates shared original vertices");
    const auto actual=VertexFloats(proxy.vertexBuffer,6,24),actualEdges=VertexFloats(proxy.vertexBuffer2,6,4);
    double attributeChecksum=0,expectedChecksum=0;size_t output=0;
    for(const auto& material:expected)for(auto source:material){
        for(size_t c=8;c<24;++c){
            attributeChecksum+=actual[output*24+c];expectedChecksum+=original[source*24+c];
            if(!benchmark)Check(actual[output*24+c]==original[source*24+c],"replayed additional UV follows original vertex across materials");
        }
        if(!benchmark)for(size_t c=0;c<3;++c)
            CheckVertexValue(actualEdges[output*4+c],originalEdges[source*4+c],output,c,"real outline buffer preserves mapped edge scale and disabled flag");
        ++output;
    }
    Check(VertexFloats(model.vertexBuffer,4,24)==original,"capture and replay never overwrite host vertices");
    std::printf("BAKE_EXPORT_BENCHMARK warmup=%u iterations=%u samples=%d meshes=2 vertices=6 export_ms=%.6f mean_export_ms=%.6f replay_frames=%u replay_cpu_ms=%.6f abc_bytes=%llu explicit_maps=%u attribute_checksum=%.6f expected_checksum=%.6f\n",
        warmup,iterations,options.lastFrame+1,exportMs,exportMs/iterations,replayIterations,replayMs,
        static_cast<unsigned long long>(std::filesystem::file_size(archive)),explicitMaps,attributeChecksum,expectedChecksum);
    native_mesh::ClearMeshBake(&app,false);
}
void KeyframeMenuState(MMDApp& app,bool enabled,const char* context){
    bake::RefreshMenu(&app);
    MENUITEMINFOW popup{sizeof popup};popup.fMask=MIIM_SUBMENU;
    Check(GetMenuItemInfoW(GetMenu(app.MainWindow()),bake::kBakingMenuId,FALSE,&popup)!=FALSE,"baking menu exists");
    const auto state=GetMenuState(popup.hSubMenu,bake::kBakeMenuCommand,MF_BYCOMMAND);
    const bool actual=state!=UINT(-1)&&!(state&(MF_DISABLED|MF_GRAYED));
    if(actual!=enabled){++keyframeGuardFailures;std::printf("FAIL: keyframe menu state: %s\n",context);}
    if(!enabled){
        const int before=rejectedKeyframeDialogs;rejectKeyframeDialog=true;
        Check(bake::HandleCommand(&app,bake::kBakeMenuCommand),"keyframe command remains routed");
        rejectKeyframeDialog=false;
        if(rejectedKeyframeDialogs!=before){++keyframeGuardFailures;std::printf("FAIL: keyframe command opened: %s\n",context);}
    }
}
}
int wmain(int argc,wchar_t** argv){
    if(argc<3)return 2;std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto out=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(out);
    uiOutput=out;
    const std::wstring mode=argv[2];const bool mme=mode==L"mme"||mode==L"combined",mmv=mode==L"combined",varying=mode==L"varying",extra=mode==L"extra"||varying;
    auto hide=SetWindowsHookExW(WH_CBT,Hidden,nullptr,GetCurrentThreadId());auto dialogs=SetWindowsHookExW(WH_CALLWNDPROCRET,Dialogs,nullptr,GetCurrentThreadId());_putenv_s("MMV_TEST_MODE",varying?"varying":extra?"extra":"identity");
    auto* app=new MMDApp;g_Block=app;app->state=MMDAppState{};app->InitDefaults();
    Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"hidden renderer initialization");
    Check(!IsWindowVisible(app->MainWindow()),"test remains hidden");app->EnglishUI()=1;
    Check(bool(GetModuleHandleW(L"MMEffect.dll"))==mme,"expected MME runtime");Check((GetModuleHandleW(L"MMV.dll")!=nullptr)==(mmv||extra),"expected MMV runtime");Frame(*app);
    if(mode==L"attributes"||mode==L"attributes-benchmark"){
        AttributeRoundtrip(*app,out,mode==L"attributes-benchmark");
        Check(unexpectedDialogs==0,"attribute export requires no modal interaction");
        ShutdownCleanup(app);g_Block=nullptr;delete app;UnhookWindowsHookEx(hide);UnhookWindowsHookEx(dialogs);
        std::puts("PASS: native bake attribute capture, ABC, cache and replay");return 0;
    }
    const auto modelPath=out/L"triangle.pmx";Fixture(modelPath);LoadModelFile(app,modelPath.c_str(),false);
    auto* raw=app->ModelSlot(0);Check(raw!=nullptr,"load triangle fixture");auto& m=*mdl::Mdl(raw);
    auto& key=m.boneKeys[1];key=m.boneKeys[0];key.frame=2;key.position[0]=2;key.previous=0;key.next=0;key.allocated=1;
    m.boneKeys[0].next=1;m.maxFrame=2;
    if(mode==L"native"){
        // Resampling must carry animated stretch/shear at the inserted frame,
        // not repeat an endpoint or take only the three diagonal components.
        const auto originalScale=bone_scale::Capture(raw);
        const bone_scale::Matrix endScale{{3,.4f,0,0,2,0,0,0,.75f}};
        bone_scale::SetKey(raw,1,endScale);
        const auto expected=bone_scale::Sample(raw,0,1.f);
        bake::Request request;request.physics=false;request.keyframes=true;request.lastFrame=2;request.settings=bake::BulletDefaults();
        const auto sampled=bake::Compute(*app,request);
        Check(sampled.error==bake::Error::None,"resample animated bone scale without physics baking");
        bool inserted=false;
        for(const auto& entry:sampled.changes)if(entry.key.frame==1){
            const auto found=sampled.scales.find(entry.keyIndex);
            Check(found!=sampled.scales.end()&&found->second==expected,"inserted key retains all nine interpolated scale components");
            Check(std::abs(entry.key.position[0]-1.f)<.001f,"inserted key retains interpolated translation");inserted=true;
        }
        Check(inserted&&expected!=endScale&&!bone_scale::Identity(expected),"animated scale test exercises an intermediate key");
        Check(bone_scale::Key(raw,1)==endScale,"resampling leaves source scale unchanged");
        bone_scale::Restore(raw,originalScale);
    }
    for(int i=0;i<3;++i)Frame(*app);
    if(mode==L"recording-focus"){
        const auto edit=GetDlgItem(app->MainWindow(),panel::kCurrentFrameEdit);
        Check(edit!=nullptr,"real frame edit exists");
        for(bool pending:{false,true}){
            SetWindowTextA(edit,"0");app->CurrentFrame()=60;app->state.enterKeyState=0;
            app->FrameStepPlayback()=pending?0:1;app->state.recordPlaybackStartPending=pending?1:0;
            EditSubclassProc(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);
            Check(app->CurrentFrame()==60,"recording focus restore does not seek to stale frame zero");
            Check(app->state.enterKeyState!=1,"recording focus restore does not manufacture Enter registration");
        }
        app->FrameStepPlayback()=app->state.recordPlaybackStartPending=0;
        SetWindowTextA(edit,"25");app->state.enterKeyState=0;
        EditSubclassProc(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);
        Check(app->CurrentFrame()==25,"ordinary frame edits still commit outside recording");
        Check(app->state.enterKeyState!=1,"committing an edit consumes Enter rather than registers a key");
        const auto initialized=MH_Initialize();Check(initialized==MH_OK||initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize recording restore hook");
        Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(&RecordingWindowRestore),reinterpret_cast<void**>(&realShowWindow))==MH_OK,"hook recording window restoration");
        Check(MH_EnableHook(reinterpret_cast<void*>(&ShowWindow))==MH_OK,"enable recording window restoration hook");
        for(bool stopAtEnd:{false,true}){
            app->state.playbackReturnsToStartFrame=stopAtEnd?1:0;
            app->CurrentFrame()=17;app->state.recordSavedFrame=17;
            app->AviRecordStartFrame()=10;app->AviRecordEndFrame()=60;
            app->PlaybackCursorSeconds()=2.f;app->FrameStepPlayback()=1;app->state.enterKeyState=0;
            app->state.recRTW=app->Renderer()->screenWidth;app->state.recRTH=app->Renderer()->screenHeight;
            FinishAviRecord(app,true);
            Check(app->CurrentFrame()==(stopAtEnd?60:17),"recording completion preserves the chosen stop frame");
            Check(app->state.enterKeyState!=1&&app->FrameStepPlayback()==0,"recording completion creates no Enter press and exits recording");
        }
        Check(restoredRecordingWindows==2,"both stop-position choices exercise production window restoration");
        MH_DisableHook(reinterpret_cast<void*>(&ShowWindow));MH_RemoveHook(reinterpret_cast<void*>(&ShowWindow));
        Check(!IsWindowVisible(app->MainWindow())&&m.boneKeys[0].next==1&&m.boneKeys[1].next==0,"focus regression stays hidden and creates no bone keyframes");
        ShutdownCleanup(app);g_Block=nullptr;delete app;UnhookWindowsHookEx(hide);UnhookWindowsHookEx(dialogs);
        std::puts("PASS: Recording focus restoration");return 0;
    }
    if(mode==L"layout"){
        const auto initialized=MH_Initialize();Check(initialized==MH_OK||initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize layout work-area hook");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetMonitorInfoW),reinterpret_cast<void*>(&SmallWorkArea),reinterpret_cast<void**>(&realMonitorInfo))==MH_OK,"create process-local work-area hook");
        Check(MH_EnableHook(reinterpret_cast<void*>(&GetMonitorInfoW))==MH_OK,"enable process-local work-area hook");
        inspectExport=inspectLayout=true;
        for(const auto size:std::vector<std::pair<int,int>>{{800,600},{640,480}}){workWidth=size.first;workHeight=size.second;for(int language=0;language<3;++language){app->EnglishUI()=language;alembic_export::ShowDialog(app);alembic_export::ShowBakeDialog(app);}}
        inspectExport=inspectLayout=false;workWidth=workHeight=0;
        MH_DisableHook(reinterpret_cast<void*>(&GetMonitorInfoW));MH_RemoveHook(reinterpret_cast<void*>(&GetMonitorInfoW));
        ShutdownCleanup(app);g_Block=nullptr;delete app;UnhookWindowsHookEx(hide);UnhookWindowsHookEx(dialogs);
        std::printf("layout failures=%d\n",layoutFailures);return layoutFailures?1:0;
    }
    inspectExport=true;for(int language=0;language<3;++language){app->EnglishUI()=language;alembic_export::ShowDialog(app);}inspectExport=false;app->EnglishUI()=1;
    Check(warningCount==0,"native range controls verified without global FX-disable warnings in three languages");
    auto saved=std::make_unique<mdl::ModelRecord>(m);std::vector<mdl::BoneRecord> bones(m.boneTable,m.boneTable+m.boneCount);
    const auto scale=bone_scale::Capture(raw);const int frame=app->CurrentFrame(),modified=app->SceneModified();
    const auto unchanged=[&]{Check(!std::memcmp(saved.get(),&m,sizeof m),"model record restored");Check(!std::memcmp(bones.data(),m.boneTable,bones.size()*sizeof bones[0]),"bone pose restored");Check(bone_scale::Capture(raw).pose==scale.pose,"scale restored");Check(app->CurrentFrame()==frame&&app->SceneModified()==modified&&!app->PlaybackActive(),"timeline and modified state restored");};
    alembic_export::Options options;options.lastFrame=2;
    app->CameraMode()=0;
    KeyframeMenuState(*app,true,"ordinary animated model");
    bool checkedBusy=false;
    const auto base=out/L"基础网格.abc";auto result=alembic_export::Export(*app,base,options,[&](int,int){
        if(!checkedBusy){KeyframeMenuState(*app,false,"ABC export in progress");checkedBusy=true;}
        return true;
    });
    std::printf("export success=%d samples=%u meshes=%u error=%s\n",result.success,result.samples,result.meshes,result.error.c_str());Check(result.success,"base export succeeds");unchanged();
    Check(checkedBusy,"export progress exercises command guard");KeyframeMenuState(*app,true,"export completed");
    auto data=Read(base,3,0,1.0/30);Check(data[0].faces==(extra&&!varying?2:1),"source topology including generated MMV material");
    Check(data[0].positions.size()==(extra&&!varying?6:3),"source vertex count including generated MMV vertices");
    if(varying)Check(data[1].faces==2&&data[1].positions.size()==6&&data[2].faces==1&&data[2].positions.size()==3,"generated topology appears and disappears with explicit empty samples");
    if(!extra){Check(std::abs(data[0].positions[0].x)<.005f,"base geometry position");Check(std::abs(data[2].positions[0].x-2)<.005f,"animated bone reaches exported positions");}
    std::ofstream(out/L"cancel.abc")<<"keep";auto cancel=alembic_export::Export(*app,out/L"cancel.abc",options,[](int f,int){return f<1;});
    Check(cancel.cancelled&&!cancel.success,"cancel export");std::ifstream old(out/L"cancel.abc");std::string value;old>>value;Check(value=="keep","cancel preserves previous file");unchanged();
    options.firstFrame=1;options.framesPerSecond=60;result=alembic_export::Export(*app,out/L"range.abc",options);Check(result.success,"range export");Read(out/L"range.abc",3,1.0/30,1.0/60);unchanged();options.firstFrame=0;options.framesPerSecond=30;
    if(mode==L"native"){
        alembic_export::Options patch;patch.firstFrame=1;patch.lastFrame=1;patch.writeAbc=false;patch.bakeIntoProject=true;patch.explicitSelection=true;
        alembic_export::Selection selection;selection.slot=0;selection.vertices={0};patch.selection={selection};
        result=alembic_export::Export(*app,{},patch);std::printf("partial bake success=%d error=%s\n",result.success,result.error.c_str());Check(result.success,"bake one source vertex only");
        Check(mesh_bake_cache::RangeLimited(raw)&&!mesh_bake_cache::ActiveAt(raw,0)&&mesh_bake_cache::ActiveAt(raw,1./30.)&&!mesh_bake_cache::ActiveAt(raw,2./30.),"partial cache follows selected frame range");
        m.boneKeys[1].position[0]=12;
        auto verification=alembic_export::Export(*app,out/L"partial-vertices.abc",options);Check(verification.success,"render a partially baked mesh");
        const auto partial=Read(out/L"partial-vertices.abc",3,0,1./30.);
        Check(std::abs(partial[1].positions[0].x-1)<.02f&&std::abs(partial[1].positions[1].x-7)<.02f&&std::abs(partial[1].positions[2].x-6)<.02f,"only selected vertex freezes; other vertices still follow source bones");
        Check(std::abs(partial[0].positions[0].x)<.02f&&std::abs(partial[2].positions[0].x-12)<.02f,"native animation remains live outside bake range");
        native_mesh::ClearMeshBake(app,false);m.boneKeys[1].position[0]=2;
        Check(mesh_bake_cache::Empty(),"partial test removes only its own cache");
    }
    if(mme){
        auto pmm=out/L"effects.pmm",emm=out/L"effects.emm",fx=out/L"offset.fx";
        wcscpy_s(app->EnvFileName(),256,pmm.c_str());SaveSceneFile(app);
        for(int i=0;i<4;++i)Frame(*app); // consume MME's queued autosave before replacing the fixture EMM
        std::ofstream(fx)<<R"FX(float4x4 WVP:WORLDVIEWPROJECTION;
float4 VS(float4 p:POSITION):POSITION{p.x+=3;return mul(p,WVP);}float4 PS():COLOR{return float4(.8,.2,.1,1);}
technique Main<string MMDPass="object";>{pass P{CullMode=NONE;VertexShader=compile vs_3_0 VS();PixelShader=compile ps_3_0 PS();}}
technique MainSS<string MMDPass="object_ss";>{pass P{CullMode=NONE;VertexShader=compile vs_3_0 VS();PixelShader=compile ps_3_0 PS();}})FX";
        std::ofstream(emm)<<"[Info]\nVersion = 3\n[Object]\nPmd1 = "<<modelPath.string()<<"\n[Effect]\nDefault = none\nPmd1 = "<<fx.string()<<"\n";
        NotifyEffectFileOpen(pmm.c_str());
        for(int i=0;i<4;++i)Frame(*app);
        if(mmv){
            auto meshFx=out/L"mesh.fx";std::ofstream(meshFx)<<R"FX(#include "mmv.fxh"
float MMVConfig<string MMVBackend="DX12";>;
[numthreads(64,1,1)]void CS(uint id:SV_DispatchThreadID){if(id>=MMVVertexCount())return;MMVVertex v=MMVVertices[id];v.position.x+=2;MMVStoreVertex(id,v);}
technique Main<string MMVType="Mesh";>{pass P{ComputeShader=compile cs_5_1 CS();}})FX";
            std::ofstream(out/L"effects.vmm")<<"[Info]\nType=MMV\nVersion=1\nKind=Project\nAutoLoad=true\nAutoSave=true\n[Binding.0]\nObjectKind=0\nObjectPath="<<modelPath.generic_u8string()<<"\nInstance=0\nOwner=\nTarget=Main\nMaterial=\nEffect="<<meshFx.generic_u8string()<<"\nEnabled=true\nController=false\n";
            native_mesh::ProjectOpened(app,pmm.c_str());
        }
        for(int i=0;i<4;++i)Frame(*app);
        auto effectResult=alembic_export::Export(*app,out/L"effect.abc",options);std::printf("effect success=%d error=%s\n",effectResult.success,effectResult.error.c_str());Check(effectResult.success,"effect export succeeds");
        auto effect=Read(out/L"effect.abc",3,0,1.0/30);
        for(int sample=0;sample<3;++sample){Check(effect[sample].positions.size()==data[sample].positions.size(),"effect vertex count");for(size_t i=0;i<data[sample].positions.size();++i){
            const auto delta=effect[sample].positions[i]-data[sample].positions[i];
            std::printf("delta frame=%d vertex=%zu (%g,%g,%g)\n",sample,i,delta.x,delta.y,delta.z);
            Check(std::abs(delta.x-(mmv?5.f:3.f))<.01f&&std::abs(delta.y)<.01f&&std::abs(delta.z)<.01f,"ABC contains post MME/MMV deformation");}}
    }
    {
        const auto expected=Read(mme?out/L"effect.abc":base,3,0,1.0/30);
        options.writeAbc=false;options.bakeIntoProject=true;
        auto cancelled=alembic_export::Export(*app,{},options,[](int f,int){return f<1;});
        Check(cancelled.cancelled&&!cancelled.baked&&mesh_bake_cache::Empty(),"cancelled bake changes neither cache nor effects");
        result=alembic_export::Export(*app,{},options);
        std::printf("bake success=%d baked=%d error=%s\n",result.success,result.baked,result.error.c_str());
        Check(result.success&&result.baked&&!result.abcWritten&&mesh_bake_cache::Has(raw),"bake-only installs cache without exporting a user ABC");
        KeyframeMenuState(*app,false,"current model has a mesh bake");
        for(int i=0;i<4;++i)Frame(*app);
        Check(native_mesh::RenderModel(raw)!=raw,"baked mesh uses a renderer proxy");
        m.boneKeys[1].position[0]=12;
        options.writeAbc=true;options.bakeIntoProject=false;
        result=alembic_export::Export(*app,out/L"baked-output.abc",options);
        std::printf("cached export success=%d error=%s\n",result.success,result.error.c_str());Check(result.success,"export cached geometry");
        Same(Read(out/L"baked-output.abc",3,0,1.0/30),expected,"bone edits and disabled FX do not deform the cached mesh again");
        options.bakeIntoProject=true;
        result=alembic_export::Export(*app,out/L"both.abc",options);
        Check(result.success&&result.baked&&result.abcWritten,"both output options execute together");
        const auto bakedProject=out/L"baked-project.pmm";wcscpy_s(app->EnvFileName(),256,bakedProject.c_str());SaveSceneFile(app);
        Check(app->SceneModified()==0,"save cached project and companion file");
        for(int i=0;i<4;++i)Frame(*app);
        if(mme)std::filesystem::copy_file(out/L"effects.emm",out/L"baked-project.emm",std::filesystem::copy_options::overwrite_existing);
        LoadSceneFile();Check(app->ModelSlot(0)&&mesh_bake_cache::Has(app->ModelSlot(0)),"PMM reload binds companion mesh cache");
        for(int i=0;i<4;++i)Frame(*app);
        options.bakeIntoProject=false;result=alembic_export::Export(*app,out/L"reopened.abc",options);
        std::printf("reopen export success=%d error=%s\n",result.success,result.error.c_str());Check(result.success,"export reopened cache");
        Same(Read(out/L"reopened.abc",3,0,1.0/30),expected,"PMM reload preserves the bake even with stale EMM assignments");
        clearScope=1;Check(bake::HandleCommand(app,bake::kClearMeshBakeMenuCommand),"clear command routed from Baking menu");clearScope=-1;
        Check(mesh_bake_cache::Empty(),"clear all mesh bakes through menu dialog");
        KeyframeMenuState(*app,true,"mesh bake cleared");
        result=alembic_export::Export(*app,out/L"cleared.abc",options);Check(result.success,"export after clearing cache");
        auto cleared=Read(out/L"cleared.abc",3,0,1.0/30);
        std::printf("cleared frame2 x=%g\n",cleared[2].positions[0].x);
        Check(std::abs(cleared[2].positions[0].x-(mme?15.f:12.f))<.02f,"clearing restores edited bone animation and preserves MME shading");
    }
    if(mode==L"native"){
        const auto path=out/L"triangle.x";
        std::ofstream(path)<<R"X(xof 0303txt 0032
Mesh {3; 0;20;0;, 1;20;0;, 0;21;0;; 1; 3;0,1,2;;
MeshNormals {3; 0;0;-1;,0;0;-1;,0;0;-1;; 1;3;0,1,2;;}
MeshTextureCoords {3;0;0;,1;0;,0;1;;}
MeshMaterialList {1;1;0;; Material {1;1;1;1;; 1; 0;0;0;; 0;0;0;;}}})X";
        LoadAccessoryFile(path.c_str());auto* accessory=app->AccessorySlot(0);Check(accessory&&app->AccessoryKeys(0),"load generated accessory fixture");
        auto* keys=app->AccessoryKeys(0);keys[0].position[0]=4;keys[0].scale=2;
        options.writeAbc=true;options.bakeIntoProject=true;
        result=alembic_export::Export(*app,out/L"accessory.abc",options);Check(result.success,"bake transformed accessory and model together");
        const auto expected=Read(out/L"accessory.abc",3,0,1.0/30);Check(expected[0].positions.size()==6,"accessory geometry included");
        keys[0].position[0]=50;keys[0].scale=.5f;options.bakeIntoProject=false;
        result=alembic_export::Export(*app,out/L"accessory-cached.abc",options);Check(result.success,"sample accessory after transform edit");
        Same(Read(out/L"accessory-cached.abc",3,0,1.0/30),expected,"cached accessory cancels edited world transform");
        clearScope=0;bake::HandleCommand(app,bake::kClearMeshBakeMenuCommand);clearScope=-1;
        Check(!mesh_bake_cache::Has(app->SelectedModel())&&mesh_bake_cache::Has(accessory),"menu current-model clearing preserves accessory cache");
        KeyframeMenuState(*app,true,"only another object's mesh is cached");
        clearScope=1;bake::HandleCommand(app,bake::kClearMeshBakeMenuCommand);clearScope=-1;Check(mesh_bake_cache::Empty(),"menu all-objects clearing removes remaining accessory cache");
        enabledEditor=CreateWindowExW(0,L"STATIC",L"modeless editor",WS_OVERLAPPED,0,0,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        disabledEditor=CreateWindowExW(0,L"STATIC",L"disabled editor",WS_OVERLAPPED|WS_DISABLED,0,0,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(enabledEditor&&disabledEditor,"create hidden editor witnesses");
        for(int action:{1,2}){
            inspectBakeRun=action;lockedProgress=0;alembic_export::ShowDialog(app);inspectBakeRun=0;
            Check(lockedProgress>0,"real mesh bake progress exercises UI lock");
            Check(mesh_bake_cache::Empty()==(action==1),"cancel remains transactional and completion installs cache");
        }
        native_mesh::ClearMeshBake(app,false);DestroyWindow(enabledEditor);DestroyWindow(disabledEditor);
    }
    Check(keyframeGuardFailures==0,"keyframe bake menu and direct command respect export and cache state");
    Check(unexpectedDialogs==0,"operation requires no unexpected modal dialogs");
    ShutdownCleanup(app);g_Block=nullptr;delete app;UnhookWindowsHookEx(hide);UnhookWindowsHookEx(dialogs);std::puts("PASS: Alembic deformed mesh roundtrip");return 0;
}
