#include "mikumikustudio/physics_scale.hpp"
// MAINTENANCE: regression-keep; offline baking, transactional keys and native dialog translations.
// Modes: no argument runs the unit/dialog suite; `--ui <dir>` adds screenshots;
// `<model.pmx> <output-directory>` runs the full scene integration;
// `--capacity` runs the runtime bone-key pool acceptance without a scene;
// `--warnings` drives native and layer-cut warning gates with fixture MMV.dll;
// `--capacity-scene <model.pmx> <output-directory>` runs the scene integration
// with a bone-key pool grown past the original capacity.
#define NOMINMAX
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <btBulletDynamicsCommon.h>
#include <CommCtrl.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace mikumikustudio;
namespace {
int failures=0,dialogs=0,language=0;bool inspect=false;std::filesystem::path images;MMDApp* inspectApp=nullptr;
// One real bake is driven through the hidden dialog to observe the interface
// lock while Compute holds it.  `runBakeStep` is set by the progress bar
// subclass below, which is entered from inside that lock.
bool runBake=false,runBakeCancel=false,runBakeStep=false;HWND runBakeMainWindow=nullptr;WNDPROC runBakeBarProc=nullptr;
int warningMode=0,fxWarnings=0,warningMorphCase=-1;bool warningProgress=false;WNDPROC warningBarProc=nullptr;std::wstring warningText;
LRESULT CALLBACK WarningProgress(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==PBM_SETRANGE32){warningProgress=true;SendMessageW(GetParent(window),WM_COMMAND,IDCANCEL,0);}
    return CallWindowProcW(warningBarProc,window,message,wp,lp);
}
// `--capacity-scene` sets this: the scene run first grows the selected model's
// bone-key pool past the original capacity and links a real key into a track
// there, so the existing PMM and VMD save/reload comparisons have to carry a key
// that only a runtime-capacity bound can see.
bool growCapacity=false;bool scenePhysicsDialog=false,scenePhysicsRun=false;
bool inspectAllChannels=false,selectOnlyBeforePicker=true;
void Check(bool ok,const char* name){if(!ok){++failures;std::printf("FAIL: %s\n",name);}}
template<class T>bool Equal(const std::vector<T>& a,const T* b){return !std::memcmp(a.data(),b,a.size()*sizeof(T));}
void Capture(HWND window,const wchar_t* suffix){
    if(images.empty())return;RECT r{};GetClientRect(window,&r);RECT button{};GetWindowRect(GetDlgItem(window,IDOK),&button);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2);Check(button.bottom<=r.bottom&&button.right<=r.right,"dialog buttons fit client area");HDC source=GetDC(window),dc=CreateCompatibleDC(source);
    BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),r.right,-r.bottom,1,32,BI_RGB};void* pixels=nullptr;
    HBITMAP bmp=CreateDIBSection(source,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bmp);
    RECT outer{};GetWindowRect(window,&outer);POINT origin{};ClientToScreen(window,&origin);SetViewportOrgEx(dc,outer.left-origin.x,outer.top-origin.y,nullptr);
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    BITMAPFILEHEADER header{};header.bfType=0x4D42;header.bfOffBits=sizeof header+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+r.right*r.bottom*4;
    std::ofstream out(images/(std::to_wstring(language)+suffix+L".bmp"),std::ios::binary);
    out.write(reinterpret_cast<char*>(&header),sizeof header);out.write(reinterpret_cast<char*>(&bi.bmiHeader),sizeof bi.bmiHeader);
    out.write(static_cast<char*>(pixels),r.right*r.bottom*4);SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);ReleaseDC(window,source);
}
// True for the child controls that take user input.  Group boxes are
// BUTTON-class decorations and statics never take input, so neither counts.
bool InteractiveControl(HWND child){
    wchar_t cls[32]{};GetClassNameW(child,cls,32);
    if(!_wcsicmp(cls,L"EDIT")||!_wcsicmp(cls,L"COMBOBOX"))return true;
    if(_wcsicmp(cls,L"BUTTON"))return false;
    return (GetWindowLongW(child,GWL_STYLE)&0x0FL)!=BS_GROUPBOX;
}
// The progress bar is subclassed for the duration of one hidden bake: Run's
// progress callback sends PBM_SETRANGE32 while Compute holds the interface
// lock, so this is where the locked state can be observed from the outside.
LRESULT CALLBACK BakeStepHook(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(!runBakeStep&&message==PBM_SETRANGE32){
        runBakeStep=true;
        HWND dialog=GetParent(window);
        Check(bake::UiLocked(),"a running bake reports the interface as locked");
        Check(IsWindowEnabled(dialog),"a running bake keeps its own dialog enabled");
        Check(runBakeMainWindow&&!IsWindowEnabled(runBakeMainWindow),"a running bake disables the main window");
        Check(IsWindowEnabled(GetDlgItem(dialog,IDCANCEL)),"a running bake keeps cancel usable");
        Check((GetWindowLongW(window,GWL_STYLE)&WS_VISIBLE)!=0,"a running bake shows its progress bar");
        unsigned interactive=0,usable=0;
        for(HWND child=GetWindow(dialog,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
            if(!InteractiveControl(child))continue;
            ++interactive;
            if(IsWindowEnabled(child)){++usable;Check(GetDlgCtrlID(child)==IDCANCEL,"only cancel accepts input while a bake runs");}
        }
        Check(interactive>=4&&usable==1,"a running bake leaves exactly one usable control");
        // While a bake runs the dialog must accept cancel and nothing else: a
        // click on "Last" would otherwise rewrite the range of the running bake.
        wchar_t endFrame[32]{};GetDlgItemTextW(dialog,101,endFrame,32);
        SendMessageW(dialog,WM_COMMAND,102/*Last*/,0);
        wchar_t afterCommand[32]{};GetDlgItemTextW(dialog,101,afterCommand,32);
        Check(!wcscmp(endFrame,afterCommand),"a running bake ignores every command except cancel");
        if(runBakeCancel)SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);
    }
    return runBakeBarProc?CallWindowProcW(runBakeBarProc,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
LRESULT CALLBACK DialogHook(int code,WPARAM wp,LPARAM lp){
    if(code>=0){const auto& m=*reinterpret_cast<CWPRETSTRUCT*>(lp);if(m.message==WM_INITDIALOG){
        wchar_t cls[32]{};GetClassNameW(m.hwnd,cls,32);
        if(inspectAllChannels&&!wcscmp(cls,L"#32770")){
            if(GetDlgItem(m.hwnd,160)){
                RECT client{};GetClientRect(m.hwnd,&client);
                for(int id:{161,162,163,IDOK,IDCANCEL}){
                    RECT rect{};GetWindowRect(GetDlgItem(m.hwnd,id),&rect);
                    MapWindowPoints(nullptr,m.hwnd,reinterpret_cast<POINT*>(&rect),2);
                    Check(rect.right<=client.right&&rect.bottom<=client.bottom,"channel picker controls fit client area");
                }
                Check(GetDlgItem(m.hwnd,167)!=nullptr,"picker offers select all channels");
                SendMessageW(m.hwnd,WM_COMMAND,167,0);
                HWND groups=GetDlgItem(m.hwnd,160),fields=GetDlgItem(m.hwnd,161);
                const int count=int(SendMessageW(groups,LB_GETCOUNT,0,0));
                Check(count>1&&SendMessageW(groups,LB_GETSELCOUNT,0,0)==count,"all channel rows selected");
                for(int row=0;row<count;++row){
                    SendMessageW(groups,LB_SETCARETINDEX,row,FALSE);
                    SendMessageW(m.hwnd,WM_COMMAND,MAKEWPARAM(160,LBN_SELCHANGE),reinterpret_cast<LPARAM>(groups));
                    Check(SendMessageW(fields,LB_GETSELCOUNT,0,0)==SendMessageW(fields,LB_GETCOUNT,0,0),"all fields selected across channels");
                }
                SendMessageW(m.hwnd,WM_COMMAND,163,0); // clear current fields
                SendMessageW(m.hwnd,WM_COMMAND,162,0); // reselect current fields
                Check(SendMessageW(groups,LB_GETSEL,count-1,0)>0,"reselecting fields includes their channel");
                SendMessageW(groups,LB_SETSEL,FALSE,count-1);
                SendMessageW(m.hwnd,WM_COMMAND,MAKEWPARAM(160,LBN_SELCHANGE),reinterpret_cast<LPARAM>(groups));
                Check(SendMessageW(groups,LB_GETSEL,count-1,0)==0,"channel can still be deselected directly");
                SendMessageW(m.hwnd,WM_COMMAND,167,0);
                SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);
            }else if(GetDlgItem(m.hwnd,105)){
                if(selectOnlyBeforePicker)CheckDlgButton(m.hwnd,105,BST_CHECKED);
                else Check(IsDlgButtonChecked(m.hwnd,105)==BST_UNCHECKED,"global select all removes selected-bones restriction");
                SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);
            }else if(GetDlgItem(m.hwnd,103)){
                SendMessageW(m.hwnd,WM_COMMAND,108,0);
                SendMessageW(m.hwnd,WM_COMMAND,141,0);
                Check(IsDlgButtonChecked(m.hwnd,104)==BST_CHECKED,"select all enables bone keyframe baking");
                selectOnlyBeforePicker=false;
                SendMessageW(m.hwnd,WM_COMMAND,108,0);
                EndDialog(m.hwnd,IDCANCEL);
            }else{Check(false,"unexpected picker dialog");EndDialog(m.hwnd,IDCANCEL);}
            return CallNextHookEx(nullptr,code,wp,lp);
        }
        if(scenePhysicsDialog&&!wcscmp(cls,L"#32770")&&GetDlgItem(m.hwnd,103)){
            ++dialogs;
            Check(IsWindowEnabled(GetDlgItem(m.hwnd,103)),"scene physics switch is enabled with dynamic models");
            Check(IsWindowEnabled(GetDlgItem(m.hwnd,106)),"scene physics preset is enabled");
            Check(IsDlgButtonChecked(m.hwnd,103)==BST_UNCHECKED,"scene physics starts unchecked");
            CheckDlgButton(m.hwnd,103,BST_CHECKED);
            SendMessageW(m.hwnd,WM_COMMAND,MAKEWPARAM(103,BN_CLICKED),0);
            Check(IsDlgButtonChecked(m.hwnd,103)==BST_CHECKED,"scene physics stays checked after selection");
            if(scenePhysicsRun){CheckDlgButton(m.hwnd,104,BST_UNCHECKED);SetDlgItemTextW(m.hwnd,100,L"1");SetDlgItemTextW(m.hwnd,101,L"4");SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);}
            else EndDialog(m.hwnd,IDCANCEL);
            return CallNextHookEx(nullptr,code,wp,lp);
        }
        if(warningMode&&!wcscmp(cls,L"#32770")){
            const bool mesh=warningMode>=3;
            if(GetDlgItem(m.hwnd,160)){
                HWND groups=GetDlgItem(m.hwnd,160);
                SendMessageW(groups,LB_SETSEL,TRUE,1);SendMessageW(groups,LB_SETSEL,warningMorphCase==1,2);
                SendMessageW(m.hwnd,WM_COMMAND,MAKEWPARAM(160,LBN_SELCHANGE),reinterpret_cast<LPARAM>(groups));
                SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);
            }else if(GetDlgItem(m.hwnd,mesh?127:103)){
                SetDlgItemTextW(m.hwnd,mesh?120:100,L"1");SetDlgItemTextW(m.hwnd,mesh?121:101,L"2");
                const int layers=mesh?133:144;
                Check(SendDlgItemMessageW(m.hwnd,layers,CB_GETCURSEL,0,0)<=0,"ordinary baking is the default with MMV loaded");
                if(warningMode==2||warningMode==4)SendDlgItemMessageW(m.hwnd,layers,CB_SETCURSEL,1,0);
                if(warningMode==2)CheckDlgButton(m.hwnd,104,BST_CHECKED);
                if(warningMode==2&&warningMorphCase>=0)SendMessageW(m.hwnd,WM_COMMAND,141,0);
                if(mesh){
                    CheckDlgButton(m.hwnd,126,BST_UNCHECKED);CheckDlgButton(m.hwnd,127,BST_CHECKED);
                    HWND objects=GetDlgItem(m.hwnd,129);
                    for(int row=1;row<ListView_GetItemCount(objects);++row)ListView_SetCheckState(objects,row,FALSE);
                }
                warningBarProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(GetDlgItem(m.hwnd,mesh?124:109),GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(WarningProgress)));
                SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);if(IsWindow(m.hwnd))EndDialog(m.hwnd,IDCANCEL);
            }else{
                wchar_t text[2048]{};GetDlgItemTextW(m.hwnd,65535,text,2048);
                warningText=text;
                if(wcsstr(text,L"FX")||wcsstr(text,L"MMVGetLayerBakeApi"))++fxWarnings;
                EndDialog(m.hwnd,IDCANCEL);
            }
            return CallNextHookEx(nullptr,code,wp,lp);
        }
        if(runBake){
            // Hidden bake run: frame 0 to frame 0 keeps the simulation short,
            // and every other dialog the run raises (the replace-keys
            // confirmation, an error box) is answered and closed here.
            if(!wcscmp(cls,L"#32770")&&GetDlgItem(m.hwnd,100)){
                ++dialogs;
                SetDlgItemTextW(m.hwnd,100,L"0");SetDlgItemTextW(m.hwnd,101,L"0");
                runBakeStep=false;
                runBakeBarProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(GetDlgItem(m.hwnd,109),GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(BakeStepHook)));
                SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);
                const bool stillOpen=IsWindow(m.hwnd)!=FALSE;
                // EndDialog defers destruction until this WM_INITDIALOG hook
                // returns. A successful run must close itself; cancellation
                // leaves the dialog open so the user can edit and retry.
                if(runBakeCancel){Check(stillOpen,"cancel keeps the bake dialog available");if(stillOpen)EndDialog(m.hwnd,IDCANCEL);}
                Check(runBakeStep,"the running bake reached its first progress step");
                Check(runBakeMainWindow&&IsWindowEnabled(runBakeMainWindow),"the interface is usable again after the bake");
            }else {Check(GetDlgItem(m.hwnd,IDCANCEL)!=nullptr,"a bake run shows only the overwrite confirmation");EndDialog(m.hwnd,IDOK);}
            return CallNextHookEx(nullptr,code,wp,lp);
        }
        if(wcscmp(cls,L"#32770")==0){++dialogs;
            if(inspect && GetDlgItem(m.hwnd,120)){
                wchar_t text[80]{};GetDlgItemTextW(m.hwnd,120,text,80);Check(std::abs(wcstod(text,nullptr)-.2)<1e-7,"ERP starts at Bullet default");
                GetDlgItemTextW(m.hwnd,121,text,80);Check(wcstod(text,nullptr)==0,"CFM starts at Bullet default");
                Check(GetDlgItemInt(m.hwnd,122,nullptr,FALSE)==1,"one default substep");
                Check(GetDlgItemInt(m.hwnd,123,nullptr,FALSE)==1,"default multiplier one");
                Check(GetDlgItemInt(m.hwnd,124,nullptr,FALSE)==60,"default base rate 60");
                Capture(m.hwnd,L"-advanced");SetDlgItemTextW(m.hwnd,123,L"2");GetDlgItemTextW(m.hwnd,125,text,80);Check(!wcscmp(text,L"120 Hz"),"derived frequency updates");
                SetDlgItemTextW(m.hwnd,123,L"100000");SetDlgItemTextW(m.hwnd,124,L"100000");GetDlgItemTextW(m.hwnd,125,text,80);Check(!wcscmp(text,L"—"),"invalid inputs never overflow derived frequency");
                SetDlgItemTextW(m.hwnd,123,L"2");SetDlgItemTextW(m.hwnd,124,L"60");SetDlgItemTextW(m.hwnd,120,L"2e-1");SendMessageW(m.hwnd,WM_COMMAND,IDOK,0);
            }else if(inspect && GetDlgItem(m.hwnd,100)){
                wchar_t title[80]{};GetWindowTextW(m.hwnd,title,80);
                Check(!wcscmp(title,language==2?L"关键帧烧录":language==1?L"Keyframe baking":L"キー焼き込み"),"native dialog language");
                Check(IsDlgButtonChecked(m.hwnd,103)==BST_CHECKED,"physics enabled initially");
                // The main dialog must state its own scope in the active
                // language and lay that text out where the buttons are not.
                // The wording follows the page the main window shows, so the
                // expectation is picked from the scope of the app under test.
                const bool modelPage = inspectApp==nullptr || inspectApp->state.optflag[0]==0;
                wchar_t note[512]{};GetDlgItemTextW(m.hwnd,133,note,512);
                Check(!wcscmp(note,language==2
                    ?(modelPage?L"模型页只烧录当前模型：骨骼、表情与「显示・IK」可在「通道…」中逐项选择。网格烘焙和 ABC 导出请用「烘焙 → 烘焙网格…」。"
                               :L"相机、灯光、附件页烧录场景轨道：相机、灯光、自阴影、重力，以及每个 .x 附件及其子参数。网格烘焙和 ABC 导出请用「烘焙 → 烘焙网格…」。")
                    :language==1
                    ?(modelPage?L"Model page bakes the current model only: bones, morphs and the visibility/IK track are picked item by item in the Channels dialog. Use Baking → Bake mesh for mesh baking and ABC export."
                               :L"The camera/light/accessory page bakes the scene tracks: camera, light, self shadow, gravity, and every .x object with its own sub-parameters. Use Baking → Bake mesh for mesh baking and ABC export.")
                    :(modelPage?L"モデルページは現在のモデルのみを焼き込みます：ボーン・モーフ・「表示・IK」は「チャンネル…」で個別に選びます。メッシュのベイクと ABC 書き出しには「ベイク → メッシュ焼き込み…」を使います。"
                               :L"カメラ・照明・アクセサリのページはシーンのトラックを焼き込みます：カメラ・照明・セルフ影・重力、および各 .x アクセサリとその子パラメータです。メッシュのベイクと ABC 書き出しには「ベイク → メッシュ焼き込み…」を使います。")),
                    "bake scope explanation follows the UI language and the page");
                RECT noteRect{},okRect{},client{};GetClientRect(m.hwnd,&client);
                GetWindowRect(GetDlgItem(m.hwnd,133),&noteRect);GetWindowRect(GetDlgItem(m.hwnd,IDOK),&okRect);
                MapWindowPoints(nullptr,m.hwnd,reinterpret_cast<POINT*>(&noteRect),2);MapWindowPoints(nullptr,m.hwnd,reinterpret_cast<POINT*>(&okRect),2);
                Check(noteRect.left>=client.left&&noteRect.right<=client.right&&noteRect.bottom<=client.bottom,"bake scope explanation fits inside the dialog");
                Check(noteRect.bottom<=okRect.top,"bake scope explanation does not overlap the buttons");
                Capture(m.hwnd,L"-main");SendMessageW(m.hwnd,WM_COMMAND,107,0);EndDialog(m.hwnd,IDCANCEL);
            }else if(inspect && GetDlgItem(m.hwnd,132)){
                wchar_t title[80]{},text[512]{};
                GetWindowTextW(m.hwnd,title,80);
                Check(!wcscmp(title,language==2?L"清除网格烧录":language==1?L"Clear mesh bake":L"ベイク解除"),"clear dialog language");
                GetDlgItemTextW(m.hwnd,132,text,512);
                Check(!wcscmp(text,language==2?L"清除后骨骼、IK、表情与物理重新影响网格；原 FX 保持停用，可手动恢复。":language==1?L"After clearing, bones, IK, morphs and physics drive the mesh again. The original FX stay disabled and can be restored manually.":L"解除するとボーン・IK・モーフ・物理が再びメッシュに反映されます。元の FX は無効のままで、手動で戻せます。"),"clear dialog explanation");
                const bool cached=inspectApp&&mesh_bake_cache::Has(inspectApp->SelectedModel());
                const bool any=inspectApp&&!mesh_bake_cache::Empty();
                Check(IsWindowEnabled(GetDlgItem(m.hwnd,130))==(cached?TRUE:FALSE),"current scope needs a cached current model");
                Check(IsWindowEnabled(GetDlgItem(m.hwnd,131))==(any?TRUE:FALSE),"all-objects scope needs a non-empty cache");
                Check(IsWindowEnabled(GetDlgItem(m.hwnd,IDOK))==(cached||any?TRUE:FALSE),"confirm follows the available scopes");
                EndDialog(m.hwnd,IDCANCEL);
            }else {if(!inspect){wchar_t title[256]{},text[1024]{};GetWindowTextW(m.hwnd,title,256);GetDlgItemTextW(m.hwnd,65535,text,1024);std::fwprintf(stderr,L"unexpected dialog: %ls: %ls\\n",title,text);}EndDialog(m.hwnd,IDCANCEL);}
        }
    }}return CallNextHookEx(nullptr,code,wp,lp);
}
LRESULT CALLBACK HideHook(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_CREATEWND){auto* cs=reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;if(!(cs->style&WS_CHILD))cs->style&=~WS_VISIBLE;}
    if(code==HCBT_ACTIVATE)return 1;return CallNextHookEx(nullptr,code,wp,lp);
}
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,2> bones{};std::array<unsigned char,2> physics{},selection{};
    std::vector<mdl::BoneKey> keys{mdl::kBoneKeyCapacity};
    // Morph and display pools as well: the capacity mode drives SaveVmdFile on
    // this fixture, and the export sweeps all three key arrays.
    std::vector<mdl::MorphKey> morphKeys{mdl::kMorphKeyCapacity};
    std::vector<mdl::DisplayKey> displayKeys{mdl::kDisplayKeyCapacity};
    Fixture(){model->boneTable=bones.data();model->boneCount=2;model->bonePhysicsState=physics.data();model->boneSelection=selection.data();model->boneKeys=keys.data();model->morphKeys=morphKeys.data();model->displayKeys=displayKeys.data();for(int i=0;i<2;++i)keys[i].rotation[3]=bones[i].rotQuat[3]=1;}
    ~Fixture(){bone_scale::Forget(raw());for(auto& ring:model->undoRings)for(auto& slot:ring.slots){std::free(slot.bonePose);std::free(slot.auxiliaryPose);}}
    unsigned char* raw(){return reinterpret_cast<unsigned char*>(model.get());}
};
void ScenePhysicsUnit(){
    Fixture a,b;auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);
    app->ModelSlot(0)=a.raw();app->ModelSlot(1)=b.raw();app->SelectedModelSlot()=0;app->state.optflag[0]=1;
    app->GravityY()=-1;app->GravityMagnitude()=9.8f;
    btDefaultCollisionConfiguration config;btCollisionDispatcher dispatcher(&config);btDbvtBroadphase broadphase;btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world(&dispatcher,&broadphase,&solver,&config);PhysicsScene physics{};physics.world=&world;app->Physics()=&physics;
    btSphereShape shape(1);btVector3 inertia;shape.calculateLocalInertia(1,inertia);
    btDefaultMotionState motionA(btTransform(btQuaternion::getIdentity(),btVector3(0,0,0))),motionB(btTransform(btQuaternion::getIdentity(),btVector3(1,0,0)));
    btRigidBody bodyA(btRigidBody::btRigidBodyConstructionInfo(1,&motionA,&shape,inertia)),bodyB(btRigidBody::btRigidBodyConstructionInfo(1,&motionB,&shape,inertia));
    world.addRigidBody(&bodyA);world.addRigidBody(&bodyB);
    mdl::RigidRecord rigidA{},rigidB{};
    std::array<mdl::BoneOrderEntry,1> orderA{},orderB{};orderA[0].linkedModel=orderB[0].linkedModel=-1;
    mdl::BoneReference pairA{-1,-1},pairB{-1,-1};a.displayKeys[0].selectorStates=&pairA;b.displayKeys[0].selectorStates=&pairB;
    a.model->boneOrderTable=orderA.data();b.model->boneOrderTable=orderB.data();a.model->boneOrderCount=b.model->boneOrderCount=1;
    std::array<unsigned,2> cursorsA{},cursorsB{};std::array<unsigned char,2> activeA{},activeB{};
    a.model->boneKeyCursors=cursorsA.data();b.model->boneKeyCursors=cursorsB.data();a.model->boneTrackActive=activeA.data();b.model->boneTrackActive=activeB.data();
    auto setup=[&](Fixture& f,mdl::RigidRecord& rigid,btRigidBody& body,int slot){
        f.model->rigidTable=&rigid;f.model->rigidCount=1;f.model->scenePtr=&physics;f.model->comboSelIndex2=slot;
        rigid.body=&body;rigid.keyData=body.getMotionState();rigid.boneIndex=0;rigid.mode=1;
        for(int i=0;i<16;++i)rigid.invTransform[i]=i%5==0?1.f:0.f;
        for(auto& bone:f.bones){bone.parent=-1;bone.slotIndex=-1;bone.tailIdx=-1;bone.rigidIdx=-296;bone.rotQuat2[3]=1;bone.type=mdl::BoneType::RotateMove;
            for(int i=0;i<16;++i)bone.matInit[i]=bone.matWorld[i]=bone.matLocal[i]=i%5==0?1.f:0.f;
        }
        f.bones[0].hasRigidBody=1;f.bones[0].rigidIdx=0;
        f.bones[0].position[0]=slot;f.bones[0].matInit[12]=f.bones[0].matWorld[12]=float(slot);
    };
    setup(a,rigidA,bodyA,0);setup(b,rigidB,bodyB,1);
    auto* previous=g_Block;g_Block=app.get();
    const int initialDialogs=dialogs;scenePhysicsDialog=true;bake::ShowDialog(app.get());scenePhysicsDialog=false;
    Check(dialogs==initialDialogs+1,"camera page opens physics bake without scene tracks");
    const auto beforeA=a.keys,beforeB=b.keys;const auto poseA=bodyA.getWorldTransform(),poseB=bodyB.getWorldTransform();
    bake::Request request;request.firstFrame=0;request.lastFrame=4;request.physics=true;request.keyframes=false;
    const auto start=GetTickCount64();auto scene=bake::ComputeScene(*app,request);
    std::printf("scene physics: error=%d models=%zu steps=%llu time_ms=%llu\n",int(scene.error),scene.models.size(),scene.simulationSteps,GetTickCount64()-start);
    Check(scene.error==bake::Error::None&&scene.models.size()==2,"scene bake collects both dynamic models on camera page");
    Check(scene.simulationSteps==10,"scene bake advances one shared world rather than one world per model");
    request.preview=true;
    auto previewed=bake::ComputeScene(*app,request);request.preview=false;
    Check(previewed.error==scene.error&&previewed.simulationSteps==scene.simulationSteps&&previewed.models.size()==scene.models.size(),"preview preserves simulation step count");
    if(previewed.models.size()==scene.models.size())for(size_t i=0;i<scene.models.size();++i){
        const auto& left=scene.models[i].result.changes;const auto& right=previewed.models[i].result.changes;
        Check(left.size()==right.size(),"preview preserves sampled key count");
        if(left.size()==right.size())for(size_t k=0;k<left.size();++k)
            Check(left[k].keyIndex==right[k].keyIndex&&!std::memcmp(&left[k].key,&right[k].key,sizeof(mdl::BoneKey)),"preview option preserves sampled key data");
    }
    Check(Equal(beforeA,a.keys.data())&&Equal(beforeB,b.keys.data())&&poseA==bodyA.getWorldTransform()&&poseB==bodyB.getWorldTransform(),"offline scene bake preserves live keys and rigid transforms");
    bake::Request partial=request;partial.physics=false;partial.keyframes=true;
    partial.boneCoverage=channel_bake::Coverage::Explicit;partial.boneIndices={0};partial.keepLayerEffects=true;
    app->SelectedModelSlot()=0;
    const auto recorded=bake::Compute(*app,partial);
    Check(recorded.error==bake::Error::None&&recorded.boneCount==1,"partial recording targets one bone");
    for(const auto& change:recorded.changes)Check(change.keyIndex!=1,"unselected bone root is not written");
    Check(Equal(beforeA,a.keys.data()),"partial preparation leaves live key pools untouched");
    for(size_t i=0;i<scene.models.size();++i){app->SelectedModelSlot()=static_cast<unsigned char>(i);auto single=bake::Compute(*app,request);const auto& all=scene.models[i].result;
        Check(single.error==bake::Error::None&&single.changes.size()==all.changes.size(),"scene output matches existing selected-model simulation");
        if(single.changes.size()==all.changes.size())for(size_t k=0;k<single.changes.size();++k)
            Check(single.changes[k].keyIndex==all.changes[k].keyIndex&&!std::memcmp(&single.changes[k].key,&all.changes[k].key,sizeof(mdl::BoneKey)),"shared-world sampled keys retain selected-model results");
    }
    world.removeRigidBody(&bodyA);world.removeRigidBody(&bodyB);world.addRigidBody(&bodyA,1,0);world.addRigidBody(&bodyB,1,0);
    auto noCollision=bake::ComputeScene(*app,request);float collisionDifference=0;
    if(noCollision.models.size()==scene.models.size())for(size_t i=0;i<scene.models.size();++i){
        const auto& keys=scene.models[i].result.changes;const auto& isolated=noCollision.models[i].result.changes;
        if(keys.size()==isolated.size())for(size_t k=0;k<keys.size();++k)collisionDifference+=std::abs(keys[k].key.position[0]-isolated[k].key.position[0]);
    }
    Check(collisionDifference>.01f,"cross-model rigid collision changes baked poses");
    world.removeRigidBody(&bodyA);world.removeRigidBody(&bodyB);world.addRigidBody(&bodyA);world.addRigidBody(&bodyB);
    app->SelectedModelSlot()=2;auto unselected=bake::ComputeScene(*app,request);
    Check(unselected.error==bake::Error::None&&unselected.models.size()==2&&app->SelectedModelSlot()==2,"scene bake needs no selected model and preserves selection");
    auto cancelled=bake::ComputeScene(*app,request,[](int frame,int){return frame<1;});
    Check(cancelled.error==bake::Error::Cancelled&&cancelled.models.empty(),"cancelled scene simulation publishes no partial models");
    if(scene.models.size()==2){auto invalid=scene;invalid.models[1].result.changes.push_back({-1,{}});
        const auto scaleBefore=bone_scale::Capture(a.raw());const auto revisionBefore=bone_scale::Revision(a.raw());
        invalid.models[0].result.scales[0]=bone_scale::Matrix{{2,0,0,0,1,0,0,0,1}};
        Check(!bake::CommitScene(*app,invalid,3),"invalid second model refuses the entire scene commit");
        Check(Equal(beforeA,a.keys.data())&&Equal(beforeB,b.keys.data())&&!a.model->undoDirty&&!b.model->undoDirty,"failed scene commit preserves both timelines and undo cursors");
        Check(bone_scale::Capture(a.raw()).keys==scaleBefore.keys&&bone_scale::Revision(a.raw())==revisionBefore,"failed scene commit preserves scale keys and revision");
        Check(bake::CommitScene(*app,scene,3),"scene commit writes all models together");
        const auto afterA=a.keys,afterB=b.keys;
        Check(!Equal(beforeA,a.keys.data())&&!Equal(beforeB,b.keys.data()),"both models receive physical keys");
        int frame=0;UndoModelEdit(a.raw(),&frame);UndoModelEdit(b.raw(),&frame);
        Check(Equal(beforeA,a.keys.data())&&Equal(beforeB,b.keys.data()),"scene results preserve per-model undo");
        RedoModelEdit(a.raw(),&frame);RedoModelEdit(b.raw(),&frame);
        Check(Equal(afterA,a.keys.data())&&Equal(afterB,b.keys.data()),"scene results preserve per-model redo");
    }
    int restoreFrame=0;UndoModelEdit(a.raw(),&restoreFrame);UndoModelEdit(b.raw(),&restoreFrame);
    scenePhysicsDialog=true;scenePhysicsRun=true;bake::ShowDialog(app.get());scenePhysicsRun=scenePhysicsDialog=false;
    Check(!Equal(beforeA,a.keys.data())&&!Equal(beforeB,b.keys.data()),"camera-page physics-only dialog writes both models without scene channels");
    Check(app->SelectedModelSlot()==2&&app->state.optflag[0]==1,"camera-page bake preserves selected page and model slot");
    g_Block=previous;world.removeRigidBody(&bodyA);world.removeRigidBody(&bodyB);
}
void WarningLogic(){
    Fixture f,other;f.model->maxFrame=other.model->maxFrame=30;mdl::AccessoryRecord accessory{};
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};app->ModelSlot(0)=f.raw();app->SelectedModelSlot()=0;
    app->ModelSlot(1)=other.raw();app->AccessorySlot(0)=&accessory;
    app->MainWindow()=CreateWindowExW(0,L"STATIC",L"bake warnings",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    g_Block=app.get();*reinterpret_cast<HWND*>(f.raw())=app->MainWindow();
    btDefaultCollisionConfiguration configuration;btCollisionDispatcher dispatcher(&configuration);btDbvtBroadphase broadphase;btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world(&dispatcher,&broadphase,&solver,&configuration);PhysicsScene physics{};physics.world=&world;app->Physics()=&physics;
    btSphereShape shape(1);btRigidBody::btRigidBodyConstructionInfo info(1,nullptr,&shape);btRigidBody body(info);
    mdl::RigidRecord rigid{};rigid.boneIndex=0;rigid.body=&body;f.model->rigidTable=&rigid;f.model->rigidCount=1;f.bones[0].hasRigidBody=1;
    // The fixture DLL has no assigned FX; run with and without its optional layer API.
    native_mesh::Initialize(app.get());Check((GetModuleHandleW(L"MMV.dll")!=nullptr),"empty-effect fixture MMV is loaded");
    for(const char* support : {"1","missing"}){
        _putenv_s("MMV_TEST_LAYER_BAKE",support);_putenv_s("MMV_TEST_LAYERS","3,1,0");
        for(language=0;language<3;++language){
            app->EnglishUI()=static_cast<unsigned char>(language);
            for(int mode : {1,3,2,4}){
                if(support[0]=='m'&&(mode==2||mode==4))continue;
                warningMode=mode;fxWarnings=0;warningProgress=false;warningText.clear();
                if(mode<3)bake::ShowDialog(app.get());else alembic_export::ShowBakeDialog(app.get());
                if(mode==1||mode==3){Check(fxWarnings==0,"native/range bake never warns about deleting FX or a missing layer API");Check(warningProgress,"ordinary partial bake reaches the sampling path");}
                else{
                    Check(fxWarnings==1&&!warningProgress,"explicit layer cuts retain their genuine loss-of-output warning");
                    if(mode==2)Check(warningText.find(L"表情")==warningText.npos&&warningText.find(L"morph")==warningText.npos&&warningText.find(L"モーフ")==warningText.npos,"a model without morphs does not need a nonexistent morph channel");
                }
            }
        }
    }
    std::array<mdl::MorphRecord,2> morphs{};f.model->morphCount=2;f.model->morphs=morphs.data();
    _putenv_s("MMV_TEST_LAYER_BAKE","1");app->EnglishUI()=2;
    for(int all : {0,1}){
        warningMode=2;warningMorphCase=all;fxWarnings=0;warningProgress=false;warningText.clear();
        bake::ShowDialog(app.get());
        Check(fxWarnings==1&&!warningProgress,"a partial layer cut is refused before sampling");
        Check((warningText.find(L"表情")!=warningText.npos)==(all==0),"layer removal requires all actual morph channels, not merely the first");
    }
    warningMorphCase=-1;f.model->morphCount=0;f.model->morphs=nullptr;
    warningMode=0;native_mesh::Shutdown();DestroyWindow(app->MainWindow());g_Block=nullptr;
    std::puts("warning logic: hidden native physics and range mesh defaults, missing layer API, explicit cuts, three languages");
}
// ---- interface lock --------------------------------------------------------
// The lock covers every enabled top-level window of this GUI thread, leaves the
// progress dialog alone and restores exactly what it disabled.  Every window
// here is created hidden and destroyed again, so nothing reaches the desktop.
void UiLock(){
    auto make=[](const wchar_t* title){return CreateWindowExW(0,L"STATIC",title,0,0,0,120,80,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);};
    HWND enabledWindow=make(L"ui lock enabled"),disabledWindow=make(L"ui lock disabled"),progressWindow=make(L"ui lock progress");
    Check(enabledWindow&&disabledWindow&&progressWindow,"interface lock fixtures created");
    if(!enabledWindow||!disabledWindow||!progressWindow)return;
    EnableWindow(disabledWindow,FALSE);
    Check(!bake::UiLocked(),"no interface lock is reported before one is taken");
    const LONG style=GetWindowLongW(enabledWindow,GWL_STYLE);
    {
        bake::ScopedUiLock lock(progressWindow);
        Check(bake::UiLocked(),"a held interface lock is reported");
        Check(!IsWindowEnabled(enabledWindow),"the lock disables an enabled top-level window");
        Check(IsWindowEnabled(progressWindow),"the lock leaves the progress dialog enabled");
        Check(!IsWindowEnabled(disabledWindow),"the lock leaves an already disabled window disabled");
        Check((GetWindowLongW(enabledWindow,GWL_STYLE)&~WS_DISABLED)==(style&~WS_DISABLED),"the lock changes only the disabled style");
        Check(GetActiveWindow()!=enabledWindow&&GetActiveWindow()!=progressWindow&&GetActiveWindow()!=disabledWindow,"the lock never activates a window");
        Check(GetFocus()!=enabledWindow&&GetFocus()!=progressWindow,"the lock never moves focus");
    }
    Check(!bake::UiLocked(),"the interface lock is released on scope exit");
    Check(IsWindowEnabled(enabledWindow),"releasing the lock re-enables what it disabled");
    Check(!IsWindowEnabled(disabledWindow),"releasing the lock keeps an already disabled window disabled");
    Check(IsWindowEnabled(progressWindow),"releasing the lock leaves the progress dialog enabled");
    {
        bake::ScopedUiLock lock(progressWindow);
        HWND later=make(L"ui lock later");
        Check(later&&IsWindowEnabled(later),"a window created while the lock is held is untouched");
        if(later)DestroyWindow(later);
    }
    Check(IsWindowEnabled(enabledWindow),"a second lock scope restores the same window again");
    DestroyWindow(progressWindow);DestroyWindow(disabledWindow);DestroyWindow(enabledWindow);
    std::puts("interface lock scope checks complete");
}
// Runtime pool capacity: a commit whose changes reach past the pool's end grows
// the pool first (transactionally), and undo still restores those slots to the
// empty record they had before.
void PoolGrowthCommit(){
    std::puts("pool growth commit: start");
    // A registered pool with a small capacity exercises growth without a second
    // full-size allocation.
    auto model=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,2> bones{};std::array<unsigned char,2> physics{},selection{};
    for(auto& bone:bones)bone.rotQuat[3]=1.0f;
    model->boneTable=bones.data();model->boneCount=2;
    model->bonePhysicsState=physics.data();model->boneSelection=selection.data();
    model->boneKeys=mdl::BoneKeyPoolCreate(64);
    model->maxFrame=0;model->searchCursor=2;
    unsigned char* raw=reinterpret_cast<unsigned char*>(model.get());
    Check(mdl::BoneKeyCapacity(*model)==64,"a created pool reports its own capacity");
    const std::size_t grown=mdl::BoneKeyPlanCapacity(mdl::BoneKeyCapacity(*model),70);
    mdl::BoneKey head{};head.frame=4;head.next=70;head.rotation[3]=1.0f;head.allocated=1;
    mdl::BoneKey past{};past.frame=9;past.previous=1;past.rotation[3]=1.0f;past.allocated=1;past.physicsDisabled=1;
    bake::Result growth;growth.maxFrame=9;growth.changes={{1,head},{70,past}};
    Check(bake::Commit(raw,growth,6),"a commit past the pool end grows the pool");
    Check(mdl::BoneKeyCapacity(*model)>=71,"the pool covers the committed slot");
    if(mdl::BoneKeyCapacity(*model)<=70){std::puts("pool growth commit: growth failed, stopping");mdl::BoneKeyPoolDestroy(model->boneKeys);return;}
    Check(mdl::BoneKeyCapacity(*model)==grown,"the pool reaches the planned capacity");
    Check(model->boneKeys[70].frame==9&&model->boneKeys[70].physicsDisabled==1&&model->boneKeys[1].next==70,"the committed key lands in the grown pool with its links");
    Check(model->boneKeys[69].frame==0&&model->boneKeys[69].allocated==0,"slots between the old end and the new key stay empty");
    std::printf("pool growth commit: grown to %zu slots\n",mdl::BoneKeyCapacity(*model));
    int frame=0;UndoModelEdit(raw,&frame);
    Check(frame==6,"undo restores the recorded frame");
    Check(model->boneKeys[70].frame==0&&model->boneKeys[70].allocated==0,"undo restores a grown slot to the empty record");
    Check(model->boneKeys[1].next==0,"undo restores the head link");
    Check(mdl::BoneKeyCapacity(*model)==grown,"the pool stays grown after undo, keeping every slot index stable");
    std::puts("pool growth commit: undo complete");
    RedoModelEdit(raw,&frame);
    Check(model->boneKeys[70].frame==9&&model->boneKeys[1].next==70,"redo restores the grown key");
    std::puts("pool growth commit: redo complete");
    bone_scale::Forget(raw);
    for(auto& ring:model->undoRings)for(auto& slot:ring.slots){std::free(slot.bonePose);std::free(slot.auxiliaryPose);}
    mdl::BoneKeyPoolDestroy(model->boneKeys);
    std::puts("runtime pool capacity: commit growth and undo complete");
}
// ---- MMV layer cutoff bake: expressibility checks --------------------------
// Synthetic layer outputs only: sourceKey identity mapping, MeshOnly refusal,
// the production-skin comparison (BDEF1/2/4, SDEF and the QDEF/unknown rule
// that leaves position and normal untouched) and the stage comparison the mesh
// bake uses.  No plugin and no scene are involved.
namespace layer_unit {
native_mesh::LayerObject Object(const char* key) {
    native_mesh::LayerObject object;
    object.objectId = 7;
    object.key = key;
    object.flags = 1u;
    object.structureSynchronized = true;
    return object;
}
native_mesh::LayerBone Bone(const std::string& objectKey, unsigned id, bool generated,
                         const char* sourceKey, float x) {
    native_mesh::LayerBone bone;
    bone.key = generated ? ("producer/bone/" + std::to_string(id)) : (objectKey + "/bone/" + std::to_string(id));
    bone.sourceKey = sourceKey ? sourceKey : (generated ? std::string() : bone.key);
    bone.generated = generated ? 1u : 0u;
    bone.valid = 1;
    bone.world[0] = bone.world[5] = bone.world[10] = bone.world[15] = 1.0f;
    bone.world[12] = x;
    for (int i = 0; i < 16; ++i) bone.skinMatrix[i] = bone.world[i];
    return bone;
}
}
void LayerUnit() {
    using namespace layer_unit;
    std::puts("layer bake expressibility: start");
    auto app = std::make_unique<MMDApp>();
    Fixture f;
    mdl::MorphRecord morph{};std::strcpy(morph.name,"smile");
    static wchar_t smile[] = L"smile";morph.jpText = smile;
    f.model->morphs = &morph;f.model->morphCount = 1;
    std::wstring message;
    // 1: sourceKey identity mapping, poses and morph values land on the host tables.
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        object.bones.push_back(Bone("unit", 1, false, nullptr, 2.0f));
        object.morphs.push_back({"smile", 0.5f, 1});
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::None,
              "layer pose maps by source key");
        Check(frame.boneValid[0] && frame.boneValid[1], "every host bone is mapped");
        Check(frame.boneWorld[12] == 1.0f && frame.boneWorld[28] == 2.0f, "bone poses land by source key");
        Check(frame.morphValid[0] && frame.morphValues[0] == 0.5f, "morph values map by unique name");
    }
    // 2: a generated bone keeps the identity of the original it replaces.
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, true, "unit/bone/0", 7.0f));
        object.bones.push_back(Bone("unit", 1, false, nullptr, 2.0f));
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::None,
              "a generated bone writes back through its source key");
        Check(frame.boneWorld[12] == 7.0f, "the generated pose replaces the source bone pose");
    }
    // 3..7: the refusals.
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, true, nullptr, 1.0f));
        object.bones.push_back(Bone("unit", 1, false, nullptr, 2.0f));
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::GeneratedItem,
              "a generated bone without a unique source bone is refused");
    }
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, false, "unit/bone/0", 1.0f));
        object.bones.push_back(Bone("unit", 1, true, "unit/bone/0", 2.0f));
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::AmbiguousBone,
              "two layer bones claiming one host bone are refused");
    }
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::MissingBone,
              "a host bone without a layer bone is refused");
    }
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, false, "other/bone/0", 1.0f));
        object.bones.push_back(Bone("unit", 1, false, nullptr, 2.0f));
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::UnmappedBone,
              "a source key of another object is refused, never matched by name or index");
    }
    {
        auto object = Object("unit");
        object.flags = 0;
        object.structureSynchronized = false;
        object.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        object.bones.push_back(Bone("unit", 1, false, nullptr, 2.0f));
        bake::LayerPoseFrame frame;
        Check(bake::MapLayerPose(*app, f.raw(), object, frame, message) == bake::LayerError::NotSynchronized,
              "a MeshOnly layer output is refused for key baking");
    }
    // 8: the production-skin comparison.
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, false, nullptr, 0.0f));
        auto& bone = object.bones[0];
        for (int i = 0; i < 16; ++i) bone.skinMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        mesh_data::Vertex vertex{};mesh_data::Triangle triangle{};
        mesh_data::MeshView mesh{};mesh.vertices = &vertex;mesh.vertexCount = 1;mesh.triangles = &triangle;
        mesh.materialCount = 0;mesh.revision = 1;
        object.mesh = &mesh;
        object.vertexBindings.assign(1, 0);
        native_mesh::LayerBindingVertex binding;
        binding.weightType = 1;  // BDEF2
        binding.bones[0] = binding.bones[1] = 0;
        binding.weights[0] = 1.0f;
        binding.deformedValid = true;
        binding.deformedPosition[0] = 1.0f;binding.deformedPosition[1] = 2.0f;binding.deformedPosition[2] = 3.0f;
        object.bindings.push_back(binding);
        std::memcpy(vertex.position, binding.deformedPosition, sizeof vertex.position);
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::None,
              "a mesh equal to the native skinning result allows key baking");
        vertex.position[0] = 9.0f;
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::ExtraMeshDeform,
              "extra mesh deformation refuses key baking");
        vertex.position[0] = 1.0f;
        object.bindings[0].deformedValid = false;
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::NoDeformedGeometry,
              "a snapshot without morph-deformed geometry is refused, never assumed");
        object.bindings[0].deformedValid = true;
        // QDEF and unknown weight types: the host leaves position and normal
        // untouched, so an unchanged vertex is a native result and a moved one
        // is not.  No QDEF transform is invented.
        object.bindings[0].weightType = 4;
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::None,
              "an untouched QDEF vertex matches the host behaviour");
        vertex.position[2] = 5.0f;
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::ExtraMeshDeform,
              "a moved QDEF vertex cannot come from native keys");
    }
    // 9: SDEF is judged by the SDEF kernel, not by a linear blend.  Two bones
    // rotating 0 and 90 degrees around Z at weight 0.5 give a rotated result,
    // while the linear blend of the two matrices lands elsewhere.
    {
        auto object = Object("unit");
        object.bones.push_back(Bone("unit", 0, false, nullptr, 0.0f));
        object.bones.push_back(Bone("unit", 1, false, nullptr, 0.0f));
        auto& second = object.bones[1];
        for (int i = 0; i < 16; ++i) second.skinMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        second.skinMatrix[0] = 0.0f;second.skinMatrix[1] = 1.0f;
        second.skinMatrix[4] = -1.0f;second.skinMatrix[5] = 0.0f;
        mesh_data::Vertex vertex{};mesh_data::Triangle triangle{};
        mesh_data::MeshView mesh{};mesh.vertices = &vertex;mesh.vertexCount = 1;mesh.triangles = &triangle;
        mesh.revision = 1;
        object.mesh = &mesh;
        object.vertexBindings.assign(1, 0);
        native_mesh::LayerBindingVertex binding;
        binding.weightType = 3;  // SDEF
        binding.bones[0] = 0;binding.bones[1] = 1;
        binding.weights[0] = 0.5f;
        binding.deformedValid = true;
        binding.deformedPosition[0] = 1.0f;
        // The SDEF centre stays at the origin, so the vertex offset rotates
        // through the blended rotation instead of degenerating onto the
        // centre's affine image (which would equal the linear blend).
        object.bindings.push_back(binding);
        // The SDEF result of this configuration, from the same kernel the host
        // production skinning uses.
        affine::BoneAux aux0{}, aux1{};
        affine::PrecomputeBone(object.bones[0].skinMatrix, nullptr, aux0);
        affine::PrecomputeBone(object.bones[1].skinMatrix, nullptr, aux1);
        const affine::BoneRef refs[2] = {{object.bones[0].skinMatrix, &aux0},
                                         {object.bones[1].skinMatrix, &aux1}};
        affine::Skinned skinned;
        affine::SkinSdef(refs[0], refs[1], 0.5f, binding.deformedPosition, binding.deformedNormal,
                         binding.sdefCenter, binding.sdefR0, binding.sdefR1,
                         affine::DefaultRotationOps(), skinned);
        std::memcpy(vertex.position, skinned.position, sizeof vertex.position);
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::None,
              "a legal SDEF result is accepted through the SDEF kernel");
        // The plain linear blend of the two matrices is a different point; a
        // check that used it would reject this vertex.
        float blended[3]{};
        for (int c = 0; c < 3; ++c) {
            const float a = binding.deformedPosition[0] * object.bones[0].skinMatrix[c] +
                            binding.deformedPosition[1] * object.bones[0].skinMatrix[4 + c] +
                            binding.deformedPosition[2] * object.bones[0].skinMatrix[8 + c] +
                            object.bones[0].skinMatrix[12 + c];
            const float b = binding.deformedPosition[0] * object.bones[1].skinMatrix[c] +
                            binding.deformedPosition[1] * object.bones[1].skinMatrix[4 + c] +
                            binding.deformedPosition[2] * object.bones[1].skinMatrix[8 + c] +
                            object.bones[1].skinMatrix[12 + c];
            blended[c] = 0.5f * (a + b);
        }
        const bool differs = std::fabs(blended[0] - skinned.position[0]) > 1e-3f ||
                             std::fabs(blended[1] - skinned.position[1]) > 1e-3f;
        Check(differs, "the SDEF result differs from a linear blend in this case");
        std::memcpy(vertex.position, blended, sizeof vertex.position);
        Check(bake::CheckLayerMeshIsNativeSkin(object, object, message) == bake::LayerError::ExtraMeshDeform,
              "a plain linear blend is not accepted as the SDEF result");
    }
    // 10: the stage comparison the mesh bake uses.
    {
        auto output = Object("unit");
        auto source = Object("unit");
        output.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        source.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        output.morphs.push_back({"smile", 0.5f, 1});
        source.morphs.push_back({"smile", 0.5f, 1});
        Check(bake::CompareLayerStages(output, source).Empty(), "unchanged stages differ nowhere");
        output.bones[0].world[12] = 3.0f;
        Check(bake::CompareLayerStages(output, source).pose, "a moved bone is a pose difference");
        output.bones[0].world[12] = 1.0f;
        output.morphs[0].value = 0.25f;
        Check(bake::CompareLayerStages(output, source).morphs, "a changed morph value is a morph difference");
        output.morphs[0].value = 0.5f;
        output.bindings.push_back({});
        source.bindings.push_back({});
        Check(bake::CompareLayerStages(output, source).Empty(), "equal bindings differ nowhere");
        output.bindings[0].weights[0] = 0.75f;
        Check(bake::CompareLayerStages(output, source).binding, "a re-bound weight is a binding difference");
        output.bones.push_back(Bone("unit", 1, true, "unit/bone/0", 2.0f));
        const auto grown = bake::CompareLayerStages(output, source);
        Check(grown.structure && grown.pose && grown.morphs,
              "an added item is a structure difference and skips the item walk");
        // The mirror case: the source side is longer, which is the shape that
        // used to read past the end of the shorter side.
        auto shorter = Object("unit");
        shorter.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        shorter.morphs.push_back({"smile", 0.5f, 1});
        auto longer = Object("unit");
        longer.bones.push_back(Bone("unit", 0, false, nullptr, 1.0f));
        longer.bones.push_back(Bone("unit", 1, false, nullptr, 2.0f));
        longer.morphs.push_back({"smile", 0.5f, 1});
        longer.morphs.push_back({"extra", 0.0f, 1});
        const auto removed = bake::CompareLayerStages(shorter, longer);
        Check(removed.structure && removed.pose && removed.morphs,
              "a removed item is a structure difference on both sides without reading past the end");
    }
    // 11: the channels a key bake cannot carry stay equal to the SOURCE stage.
    {
        auto output = Object("unit");
        auto source = Object("unit");
        output.bones.push_back(Bone("unit", 0, false, nullptr, 0.0f));
        source.bones.push_back(Bone("unit", 0, false, nullptr, 0.0f));
        for (auto* object : {&output, &source})
            for (int i = 0; i < 16; ++i) object->bones[0].skinMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        mesh_data::Vertex outVertex{}, srcVertex{};
        mesh_data::Triangle outTriangle{}, srcTriangle{};
        mesh_data::MeshView outMesh{}, srcMesh{};
        outMesh.vertices = &outVertex;outMesh.vertexCount = 1;
        outMesh.triangles = &outTriangle;outMesh.triangleCount = 1;outMesh.revision = 1;
        srcMesh.vertices = &srcVertex;srcMesh.vertexCount = 1;
        srcMesh.triangles = &srcTriangle;srcMesh.triangleCount = 1;srcMesh.revision = 1;
        output.mesh = &outMesh;source.mesh = &srcMesh;
        output.vertexBindings.assign(1, 0);source.vertexBindings.assign(1, 0);
        native_mesh::LayerBindingVertex binding;
        binding.weightType = 1;binding.bones[0] = binding.bones[1] = 0;binding.weights[0] = 1.0f;
        binding.deformedValid = true;binding.deformedPosition[0] = 1.0f;
        output.bindings.push_back(binding);source.bindings.push_back(binding);
        outVertex.position[0] = srcVertex.position[0] = 1.0f;
        outVertex.uv[0] = srcVertex.uv[0] = 0.25f;
        Check(bake::CheckLayerMeshIsNativeSkin(output, source, message) == bake::LayerError::None,
              "unchanged UV, material, topology and visibility allow key baking");
        outVertex.uv[0] = 0.75f;
        Check(bake::CheckLayerMeshIsNativeSkin(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a changed UV refuses key baking");
        outVertex.uv[0] = 0.25f;
        outTriangle.material = 1;
        Check(bake::CheckLayerMeshIsNativeSkin(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a changed material assignment refuses key baking");
        outTriangle.material = 0;
        outVertex.flags = 1;
        Check(bake::CheckLayerMeshIsNativeSkin(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a changed vertex flag refuses key baking");
        outVertex.flags = 0;
        output.visible = false;
        Check(bake::CheckLayerMeshIsNativeSkin(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a visibility change refuses key baking");
        output.visible = true;
        outVertex.normal[1] = 1.0f;
        Check(bake::CheckLayerMeshIsNativeSkin(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a changed normal refuses key baking");
    }
    // 12: the mesh cache records positions, normals, uv, indices, the material
    // index and the source-vertex mapping - nothing else.  Every field it
    // cannot store must be unchanged or the bake is refused with the reason.
    {
        auto output = Object("unit");
        auto source = Object("unit");
        mesh_data::Vertex outVertex{}, srcVertex{};
        mesh_data::Triangle outTriangle{}, srcTriangle{};
        mesh_data::MaterialView outMaterial{}, srcMaterial{};
        outMaterial.key = srcMaterial.key = "source:0";
        mesh_data::MeshView outMesh{}, srcMesh{};
        outMesh.vertices = &outVertex;outMesh.vertexCount = 1;
        outMesh.triangles = &outTriangle;outMesh.triangleCount = 1;
        outMesh.materials = &outMaterial;outMesh.materialCount = 1;
        srcMesh.vertices = &srcVertex;srcMesh.vertexCount = 1;
        srcMesh.triangles = &srcTriangle;srcMesh.triangleCount = 1;
        srcMesh.materials = &srcMaterial;srcMesh.materialCount = 1;
        output.mesh = &outMesh;source.mesh = &srcMesh;
        outVertex.sourceVertex = srcVertex.sourceVertex = 0;
        outVertex.uv[0] = 0.5f;
        outVertex.position[0] = 9.0f;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::None,
              "recordable mesh fields may change under a mesh bake");
        outVertex.additionalUv[0] = 1.0f;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "additional UVs the cache cannot store refuse the mesh bake");
        outVertex.additionalUv[0] = 0.0f;
        outVertex.flags = 1;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "vertex flags the cache cannot store refuse the mesh bake");
        outVertex.flags = 0;
        outVertex.edgeScale = 2.0f;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "edge scale the cache cannot store refuses the mesh bake");
        outVertex.edgeScale = 1.0f;
        outMaterial.diffuse[0] = 0.25f;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a changed material refuses the mesh bake instead of replaying an invented default");
        outMaterial.diffuse[0] = srcMaterial.diffuse[0];
        outMesh.materialCount = 2;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a generated material refuses the mesh bake");
        outMesh.materialCount = 1;
        outVertex.sourceVertex = 3;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a vertex mapping outside the source refuses the mesh bake");
        outVertex.sourceVertex = 0xffffffffu;
        outVertex.edgeScale = 2.0f;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a generated vertex with non-neutral unrecordable fields refuses the mesh bake");
        outVertex.edgeScale = 1.0f;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::None,
              "a generated vertex with neutral unrecordable fields is representable");
        output.visible = false;
        Check(bake::CheckLayerMeshCacheExpressible(output, source, message) == bake::LayerError::ExtraMeshDeform,
              "a visibility change refuses the mesh bake too");
        output.visible = true;
    }
    f.model->morphs = nullptr;f.model->morphCount = 0;
    // 13: the host's snapshot weight conversion fills the effective slots by
    // type.  A raw memcpy of the native PMX array dropped BDEF2's implicit
    // second weight (a real model's vertex summed to 0.959561 and the whole
    // snapshot was refused), so the conversion is pinned here.
    {
        const int bones[4]={10,9,7,6};
        const float native[4]={0.959561288f,0.f,0.f,0.f};
        int outBones[4];float outWeights[4];
        native_mesh::EffectiveWeights(0,bones,native,outBones,outWeights);
        Check(outWeights[0]==1.f&&outWeights[1]==0.f&&outWeights[2]==0.f&&outWeights[3]==0.f,"BDEF1 fills 1/0/0/0");
        Check(outBones[0]==10&&outBones[1]==-1&&outBones[2]==-1&&outBones[3]==-1,"BDEF1 clears the unused bones");
        native_mesh::EffectiveWeights(1,bones,native,outBones,outWeights);
        Check(outWeights[0]==0.959561288f&&outWeights[2]==0.f&&outWeights[3]==0.f,"BDEF2 keeps its first native weight");
        Check(outWeights[1]==1.f-0.959561288f,"BDEF2 takes the implicit second weight as exactly 1-w");
        Check(outBones[0]==10&&outBones[1]==9&&outBones[2]==-1&&outBones[3]==-1,"BDEF2 keeps its two bones and clears the rest");
        Check(std::fabs(outWeights[0]+outWeights[1]-1.f)<1e-6f,"the BDEF2 slots sum to one");
        native_mesh::EffectiveWeights(3,bones,native,outBones,outWeights);
        Check(outWeights[1]==1.f-0.959561288f&&outWeights[2]==0.f&&outBones[2]==-1,"SDEF fills the same effective slots");
        const float four[4]={0.1f,0.2f,0.3f,0.4f};
        native_mesh::EffectiveWeights(2,bones,four,outBones,outWeights);
        Check(outWeights[0]==0.1f&&outWeights[1]==0.2f&&outWeights[2]==0.3f&&outWeights[3]==0.4f,"BDEF4 keeps its native weights untouched");
        Check(outBones[0]==10&&outBones[1]==9&&outBones[2]==7&&outBones[3]==6,"BDEF4 keeps its native bones untouched");
    }
    std::puts("layer bake expressibility: complete");
}
void AllChannelPicker(){
    Fixture f;auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
    std::array<mdl::MorphRecord,2> morphs{};
    f.model->morphs=morphs.data();f.model->morphCount=2;f.model->boneOrderCount=1;
    app->ModelSlot(0)=f.raw();app->SelectedModelSlot()=0;
    app->MainWindow()=CreateWindowExW(0,L"STATIC",L"hidden picker",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    MMDApp* previous=g_Block;g_Block=app.get();
    inspectAllChannels=true;
    for(unsigned lang=0;lang<3;++lang){app->EnglishUI()=static_cast<unsigned char>(lang);selectOnlyBeforePicker=true;bake::ShowDialog(app.get());}
    inspectAllChannels=false;g_Block=previous;DestroyWindow(app->MainWindow());
    std::puts("all-channel picker checks complete");
}
void Unit(){
    Fixture f;f.model->maxFrame=7;f.model->searchCursor=77;auto before=f.keys;    const bone_scale::Matrix scale{{2,0,0,0,1.5f,0,0,0,.75f}};
    bone_scale::SetKey(f.raw(),1,scale);bone_scale::SetPose(f.raw(),1,scale);
    bake::Result result;result.maxFrame=12;auto head=f.keys[1];head.next=2;mdl::BoneKey key{};key.frame=12;key.previous=1;key.position[1]=3;key.rotation[3]=1;key.allocated=1;key.physicsDisabled=1;result.changes={{1,head},{2,key}};
    result.scales[2]=bone_scale::Sample(f.raw(),1,12);
    Check(result.scales[2]==scale&&bone_scale::Pose(f.raw(),1)==scale,"read-only scale sampling preserves the live pose");
    Check(bake::Commit(f.raw(),result,5),"commit staged changes");Check(f.keys[1].next==2&&f.keys[2].physicsDisabled==1,"commit keys and links");auto after=f.keys;
    Check(bone_scale::Key(f.raw(),2)==scale,"baked key retains scale");
    int frame=0;UndoModelEdit(f.raw(),&frame);Check(frame==5&&Equal(before,f.keys.data()),"undo restores full key pool byte for byte");Check(f.model->maxFrame==7&&f.model->searchCursor==77,"undo restores frame extent and allocation cursor");
    Check(bone_scale::Identity(bone_scale::Key(f.raw(),2))&&bone_scale::Key(f.raw(),1)==scale,"undo restores scale keys");
    RedoModelEdit(f.raw(),&frame);Check(Equal(after,f.keys.data())&&f.model->maxFrame==12,"redo restores complete transaction");
    Check(bone_scale::Key(f.raw(),2)==scale,"redo restores baked scale key");
    bake::Result unchanged;const auto undoCursor=f.model->undoState[0];Check(bake::Commit(f.raw(),unchanged,0)&&f.model->undoState[0]==undoCursor,"no-op bake succeeds without consuming undo");
    result.error=bake::Error::Cancelled;Check(!bake::Commit(f.raw(),result,0)&&Equal(after,f.keys.data()),"cancelled result cannot commit");
    PoolGrowthCommit();
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};app->ModelSlot(0)=f.raw();app->SelectedModelSlot()=0;
    app->MainWindow()=CreateWindowExW(0,L"STATIC",L"bake tests",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    inspect=true;const int prior=dialogs;for(language=0;language<3;++language){app->EnglishUI()=static_cast<unsigned char>(language);bake::ShowDialog(app.get());}inspect=false;
    Check(dialogs-prior==6,"three languages and advanced dialogs run hidden");DestroyWindow(app->MainWindow());
    UiLock();
    std::puts("unit transaction and localized dialog checks complete");
}
// One key of a linked bone track with the pool slot that owns it.  The slot is
// what the bone-scale side table is keyed by, so a caller that wants a key's
// own scale matrix cannot work from the copied record alone.
struct TrackEntry { unsigned slot;mdl::BoneKey key; };
std::vector<TrackEntry> TrackSlots(const mdl::ModelRecord& m,unsigned bone) {
    // The pool's runtime capacity: a grown pool holds keys past the original
    // slot count, and a walk bounded by the constant would stop at the old end.
    const std::size_t capacity=mdl::BoneKeyCapacity(m);
    std::vector<TrackEntry> out;unsigned slot=bone;do{
        Check(slot<capacity,"key index within capacity");if(slot>=capacity)break;
        auto key=m.boneKeys[slot];if(!out.empty())Check(out.back().key.frame<key.frame,"strictly increasing track");
        if(key.next)Check(m.boneKeys[key.next].previous==slot,"reverse key link");out.push_back({slot,key});slot=key.next;
    }while(slot&&out.size()<capacity);return out;
}
std::vector<mdl::BoneKey> Track(const mdl::ModelRecord& m,unsigned bone) {
    std::vector<mdl::BoneKey> out;for(const auto& entry:TrackSlots(m,bone))out.push_back(entry.key);return out;
}
// A key is identified by the bone and the model frame it belongs to; the pool
// slot it sits in is not stable across a reload, a re-import or a pasted
// motion, so every cross-file comparison is keyed this way.
using KeyPosition=std::pair<unsigned,std::uint32_t>;
struct KeyFact {
    unsigned char physicsDisabled;
    bone_scale::Matrix scale;
    bool operator==(const KeyFact& other) const { return physicsDisabled==other.physicsDisabled&&scale==other.scale; }
};
std::string BoneFieldName(const mdl::BoneRecord& bone) {
    std::size_t length=0;while(length<sizeof bone.name&&bone.name[length])++length;return std::string(bone.name,length);
}
// A VMD stores a 15-byte bone name per key, so an import can only find a bone
// again when its name fits that field, holds no LF and is unique in the model.
// That subset is the population a name-based round trip can be compared over;
// the counts of the comparison are printed so the coverage is visible.
std::set<std::string> UniqueVmdBoneNames(const mdl::ModelRecord& m) {
    std::map<std::string,int> counts;
    for(unsigned b=0;b<m.boneCount;++b)++counts[BoneFieldName(m.boneTable[b])];
    std::set<std::string> out;
    for(const auto& entry:counts)
        if(entry.second==1&&entry.first.size()<=15&&entry.first.find('\n')==std::string::npos)out.insert(entry.first);
    return out;
}
std::map<KeyPosition,bone_scale::Matrix> ScaleKeys(const unsigned char* model) {
    std::map<KeyPosition,bone_scale::Matrix> out;if(!model)return out;const auto& m=*mdl::Mdl(model);
    for(unsigned b=0;b<m.boneCount;++b)for(const auto& entry:TrackSlots(m,b))
        out[{b,entry.key.frame}]=bone_scale::Key(model,int(entry.slot));
    return out;
}
// Every key a motion export would write: only allocated key records reach the
// VMD, so an unallocated record left in a chain by an earlier import must never
// enter the comparison.  `restrict` keeps only bones a VMD can name again.
std::map<KeyPosition,KeyFact> AllocatedKeys(const unsigned char* model,const std::set<std::string>* restrict=nullptr) {
    std::map<KeyPosition,KeyFact> out;if(!model)return out;const auto& m=*mdl::Mdl(model);
    for(unsigned b=0;b<m.boneCount;++b) {
        if(restrict&&!restrict->count(BoneFieldName(m.boneTable[b])))continue;
        for(const auto& entry:TrackSlots(m,b))if(entry.key.allocated)
            out[{b,entry.key.frame}]={entry.key.physicsDisabled,bone_scale::Key(model,int(entry.slot))};
    }
    return out;
}
// Minimum frame over the three model key arrays, exactly as SaveVmdFile
// computes it: every exported frame is rebased by this value, so an import has
// to land a key back at (frame - minFrame) + offset.
std::uint32_t MotionMinFrame(const mdl::ModelRecord& m) {
    std::uint32_t minFrame=0xFFFFFFFFu;
    if(m.boneKeys)for(std::size_t i=0;i<mdl::BoneKeyCapacity(m);++i)if(m.boneKeys[i].allocated&&m.boneKeys[i].frame<minFrame)minFrame=m.boneKeys[i].frame;
    if(m.morphKeys)for(std::size_t i=0;i<20000;++i)if(m.morphKeys[i].allocated&&m.morphKeys[i].frame<minFrame)minFrame=m.morphKeys[i].frame;
    if(m.displayKeys)for(std::size_t i=0;i<1000;++i)if(m.displayKeys[i].allocated&&m.displayKeys[i].frame<minFrame)minFrame=m.displayKeys[i].frame;
    return minFrame;
}
// The state a real VMD import of `model`'s motion must produce at `offset`.
std::map<KeyPosition,KeyFact> ExpectedImport(const unsigned char* model,unsigned offset,const std::set<std::string>& restrict) {
    const auto keys=AllocatedKeys(model,&restrict);std::map<KeyPosition,KeyFact> out;if(keys.empty())return out;
    const std::uint32_t minFrame=MotionMinFrame(*mdl::Mdl(model));
    for(const auto& entry:keys)out[{entry.first.first,offset+entry.first.second-minFrame}]=entry.second;
    return out;
}
// Compares the allocated keys of a model that a real import just filled with
// the physics flag and the complete 9-float scale matrix every one of them had
// in the exported model.
void CompareImportedKeys(const unsigned char* model,const std::map<KeyPosition,KeyFact>& expected,const std::set<std::string>& restrict,const char* label) {
    const auto actual=AllocatedKeys(model,&restrict);
    Check(actual.size()==expected.size(),label);
    if(actual.size()!=expected.size())
        std::printf("%s: imported %zu keys, exported %zu\n",label,actual.size(),expected.size());
    for(const auto& entry:expected) {
        const auto found=actual.find(entry.first);
        if(found==actual.end()){std::printf("%s: bone=%u frame=%u missing after import\n",label,entry.first.first,entry.first.second);Check(false,label);continue;}
        const bool flag=found->second.physicsDisabled==entry.second.physicsDisabled;
        const bool scale=found->second.scale==entry.second.scale;
        if(!flag||!scale) {
            std::printf("%s: bone=%u frame=%u flags=%u/%u scale=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g)\n",label,entry.first.first,entry.first.second,
                static_cast<unsigned>(found->second.physicsDisabled),static_cast<unsigned>(entry.second.physicsDisabled),
                found->second.scale.v[0],found->second.scale.v[1],found->second.scale.v[2],found->second.scale.v[3],found->second.scale.v[4],found->second.scale.v[5],found->second.scale.v[6],found->second.scale.v[7],found->second.scale.v[8],
                entry.second.scale.v[0],entry.second.scale.v[1],entry.second.scale.v[2],entry.second.scale.v[3],entry.second.scale.v[4],entry.second.scale.v[5],entry.second.scale.v[6],entry.second.scale.v[7],entry.second.scale.v[8]);
        }
        Check(flag&&scale,label);
    }
}
bool SameResults(const bake::Result& a,const bake::Result& b){
    if(a.error!=b.error||a.changes.size()!=b.changes.size()||a.scales!=b.scales)return false;
    for(size_t i=0;i<a.changes.size();++i)if(a.changes[i].keyIndex!=b.changes[i].keyIndex||std::memcmp(&a.changes[i].key,&b.changes[i].key,sizeof(mdl::BoneKey)))return false;return true;
}
// ---- hidden menu regression ------------------------------------------------
// The keyframe bake command moved out of the Edit popup into a top-level
// "Baking" popup that sits directly in front of Help.  The fixed indices the rest
// of the port reads (Edit 1, View 2) must not move, while Help is located through
// its About command because the baking popup takes the index Help used to hold.
// The popup is recognised by its own menu id only.  32100/32101 also name the
// first two skinning adapter entries, which live in the View popup, so the
// refresh must never claim or delete them.  These checks run against the
// production menu of the hidden renderer window.
int TopLevelIndexWithId(HMENU menu,unsigned id){
    for(int i=0;i<GetMenuItemCount(menu);++i){MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID;
        if(GetMenuItemInfoW(menu,i,TRUE,&item)&&item.wID==id)return i;}return -1;
}
int TopLevelItemsWithId(HMENU menu,unsigned id){
    int total=0;for(int i=0;i<GetMenuItemCount(menu);++i){MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID;
        if(GetMenuItemInfoW(menu,i,TRUE,&item)&&item.wID==id)++total;}return total;
}
bool DirectChildWithId(HMENU menu,unsigned id){
    for(int i=0;i<GetMenuItemCount(menu);++i){MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID;
        if(GetMenuItemInfoW(menu,i,TRUE,&item)&&item.wID==id)return true;}return false;
}
HMENU DirectSubmenuWithId(HMENU menu,unsigned id){
    for(int i=0;i<GetMenuItemCount(menu);++i){MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID|MIIM_SUBMENU;
        if(GetMenuItemInfoW(menu,i,TRUE,&item)&&item.wID==id)return item.hSubMenu;}return nullptr;
}
// "About" of the original Help popup (201 in res/templates/menu/menus.rc).  The
// Help popup is identified through this command, never through a top-level index:
// the port's baking popup sits directly in front of Help, so Help is index 8 while
// the baking popup holds index 7.
constexpr unsigned kHelpAboutCommand=0xC9;
int TopLevelIndexWithCommand(HMENU menu,unsigned command){
    for(int i=0;i<GetMenuItemCount(menu);++i){MENUITEMINFOW item{sizeof item};item.fMask=MIIM_SUBMENU;
        if(!GetMenuItemInfoW(menu,i,TRUE,&item)||!item.hSubMenu)continue;
        if(GetMenuState(item.hSubMenu,command,MF_BYCOMMAND)!=UINT(-1))return i;}return -1;
}
HMENU SubmenuWithCommand(HMENU menu,unsigned command){
    const int index=TopLevelIndexWithCommand(menu,command);MENUITEMINFOW item{sizeof item};item.fMask=MIIM_SUBMENU;
    return index>=0&&GetMenuItemInfoW(menu,UINT(index),TRUE,&item)?item.hSubMenu:nullptr;
}
int CommandItems(HMENU popup,unsigned command){
    int total=0;for(int i=0;i<GetMenuItemCount(popup);++i){MENUITEMINFOW item{sizeof item};item.fMask=MIIM_ID;
        if(GetMenuItemInfoW(popup,i,TRUE,&item)&&item.wID==command)++total;}return total;
}
bool CommandEnabled(HMENU popup,unsigned command){
    const UINT state=GetMenuState(popup,command,MF_BYCOMMAND);return state!=UINT(-1)&&(state&(MF_DISABLED|MF_GRAYED))==0;
}
bool LabelIs(HMENU menu,UINT id,bool byPosition,const wchar_t* expected){
    wchar_t text[128]{};GetMenuStringW(menu,id,text,128,byPosition?MF_BYPOSITION:MF_BYCOMMAND);return !wcscmp(text,expected);
}
void MenuRegression(MMDApp* app){
    HMENU menu=GetMenu(app->MainWindow());Check(menu!=nullptr,"production menu handle");if(!menu)return;
    HMENU edit=GetSubMenu(menu,1),view=GetSubMenu(menu,2),help=SubmenuWithCommand(menu,kHelpAboutCommand);
    Check(edit&&DirectChildWithId(edit,0x12C),"Edit popup keeps index 1");
    Check(view&&DirectChildWithId(view,0xD4),"View popup keeps index 2");
    // Help is found through its About command, not through a fixed index: the
    // baking popup is inserted directly in front of it.
    Check(help&&DirectChildWithId(help,kHelpAboutCommand),"Help is found through its About command");
    // Plant the entry the previous build appended to Edit, so the removal check
    // below is not vacuous.
    if(edit)AppendMenuW(edit,MF_STRING,32100,L"legacy bake entry");
    // Plant the first two skinning adapter entries in the View popup.  32000 is
    // the skinning popup the port adds to View under the same parent.
    if(view){AppendMenuW(view,MF_STRING,32100,L"adapter 0");AppendMenuW(view,MF_STRING,32101,L"adapter 1");}
    HMENU skinning=view?DirectSubmenuWithId(view,32000):nullptr;const int skinningItems=skinning?GetMenuItemCount(skinning):-1;
    const int before=GetMenuItemCount(menu);
    for(int pass=0;pass<3;++pass)bake::RefreshMenu(app);
    const int after=GetMenuItemCount(menu);
    Check(after-before<=1,"the baking popup is inserted once");
    for(int pass=0;pass<6;++pass)bake::RefreshMenu(app);
    Check(GetMenuItemCount(menu)==after,"repeated refresh never appends another popup");
    Check(edit&&!DirectChildWithId(edit,32100),"RefreshMenu removes the legacy Edit entry");
    Check(view&&DirectChildWithId(view,32100)&&DirectChildWithId(view,32101),"skinning adapter entries survive the refresh");
    Check(view&&DirectChildWithId(view,0xD4),"the View popup itself survives the refresh");
    Check(!skinning||GetMenuItemCount(skinning)==skinningItems,"the skinning popup in View keeps its entries");
    Check(!bake::HandleCommand(app,32100)&&!bake::HandleCommand(app,32101),"the baking handler never claims the adapter ids");
    const int index=TopLevelIndexWithId(menu,bake::kBakingMenuId);
    MENUITEMINFOW found{sizeof found};found.fMask=MIIM_SUBMENU;
    HMENU baking=GetMenuItemInfoW(menu,bake::kBakingMenuId,FALSE,&found)?found.hSubMenu:nullptr;
    Check(TopLevelItemsWithId(menu,bake::kBakingMenuId)==1,"the baking popup carries one unique menu id");
    // The baking popup is not appended behind the original popups: it is inserted
    // directly in front of Help, so Help is located by command and its left
    // neighbour is checked instead.
    const int helpIndex=TopLevelIndexWithCommand(menu,kHelpAboutCommand);
    Check(baking&&index>=0&&helpIndex>=0&&index+1==helpIndex,"the baking popup sits directly in front of Help");
    Check(baking&&GetMenuItemCount(baking)==4&&CommandItems(baking,bake::kBakeMenuCommand)==1&&CommandItems(baking,bake::kExportMenuCommand)==1&&CommandItems(baking,bake::kBakeMeshMenuCommand)==1&&CommandItems(baking,bake::kClearMeshBakeMenuCommand)==1,"each baking command appears exactly once");
    if(baking){MENUITEMINFOW first{sizeof first};first.fMask=MIIM_ID;GetMenuItemInfoW(baking,0,TRUE,&first);
        Check(first.wID==bake::kBakeMenuCommand,"bake keyframes is the first entry");
        // The mesh bake is a peer of the ABC export, never merged into the key
        // bake: the two entries produce different results.
        MENUITEMINFOW second{sizeof second};second.fMask=MIIM_ID;GetMenuItemInfoW(baking,1,TRUE,&second);
        MENUITEMINFOW third{sizeof third};third.fMask=MIIM_ID;GetMenuItemInfoW(baking,2,TRUE,&third);
        Check(second.wID==bake::kExportMenuCommand&&third.wID==bake::kBakeMeshMenuCommand,
            "the mesh bake follows the ABC export");}
    // The clear entry is only offered while the mesh cache holds something, gated
    // by the export guard that also covers an export in flight.  This session
    // never installs a cache.
    Check(mesh_bake_cache::Empty(),"no mesh cache is installed in this session");
    if(baking)Check(CommandEnabled(baking,bake::kClearMeshBakeMenuCommand)==
        (!mesh_bake_cache::Empty()&&alembic_export::CanExport(app)),
        "clear entry tracks the cache and the export guard");
    const wchar_t* titles[]={L"ベイク",L"Baking",L"烘焙"};
    const wchar_t* bakes[]={L"キー焼き込み…",L"Bake keyframes…",L"烧录关键帧…"};
    const wchar_t* exports[]={L"ABC 書き出し…",L"Export ABC…",L"导出 ABC…"};
    const wchar_t* meshes[]={L"メッシュ焼き込み…",L"Bake mesh…",L"烘焙网格…"};
    const wchar_t* clears[]={L"ベイク解除…",L"Clear mesh bake…",L"清除网格烧录…"};
    const unsigned char savedLanguage=app->EnglishUI();
    // Every language runs the pass the program itself uses, so the positional
    // language table and the menu refresh are exercised together.  One warm-up
    // call absorbs any menu work the pass performs only once.
    LocalizeUI(app);
    const int settled=GetMenuItemCount(menu);
    for(unsigned char language=0;language<3;++language){app->EnglishUI()=language;LocalizeUI(app);
        Check(GetMenuItemCount(menu)==settled,"language switch does not add a popup");
        Check(TopLevelIndexWithId(menu,bake::kBakingMenuId)==index,"language switch keeps one baking popup");
        Check(TopLevelIndexWithCommand(menu,kHelpAboutCommand)==index+1,"the baking popup stays directly left of Help in every language");
        Check(view&&DirectChildWithId(view,32100)&&DirectChildWithId(view,32101),"adapter entries stay through language switches");
        if(!baking)continue;
        Check(LabelIs(menu,bake::kBakingMenuId,false,titles[language]),"baking caption follows the UI language");
        Check(LabelIs(baking,bake::kBakeMenuCommand,false,bakes[language]),"bake caption follows the UI language");
        Check(LabelIs(baking,bake::kExportMenuCommand,false,exports[language]),"export caption follows the UI language");
        Check(LabelIs(baking,bake::kBakeMeshMenuCommand,false,meshes[language]),"mesh bake caption follows the UI language");
        Check(LabelIs(baking,bake::kClearMeshBakeMenuCommand,false,clears[language]),"clear caption follows the UI language");}
    app->EnglishUI()=savedLanguage;
    unsigned char* model=app->ModelSlot(0);Check(model!=nullptr,"scene model present for the menu state checks");
    if(baking&&model){
        Check(bake::CanBake(app),"CanBake allows the loaded idle scene with a selected model");
        Check(CommandEnabled(baking,bake::kBakeMenuCommand)==bake::CanBake(app),"the bake entry follows CanBake");
        // Each condition of the shared gate has to refuse a command that arrives
        // without the menu as well, so a shortcut or a stale menu can never start
        // a bake the entry itself would have refused.
        const int quiet=dialogs;
        app->PlaybackActive()=1;bake::RefreshMenu(app);
        Check(!bake::CanBake(app),"playback refuses baking");
        Check(!CommandEnabled(baking,bake::kBakeMenuCommand),"playback disables the bake entry");
        Check(bake::HandleCommand(app,bake::kBakeMenuCommand)&&dialogs==quiet,"playback refuses the direct bake command");
        app->PlaybackActive()=0;
        app->RecordingWindow()=app->MainWindow();bake::RefreshMenu(app);
        Check(!bake::CanBake(app),"a recording window refuses baking");
        Check(!CommandEnabled(baking,bake::kBakeMenuCommand),"a recording window disables the bake entry");
        Check(bake::HandleCommand(app,bake::kBakeMenuCommand)&&dialogs==quiet,"a recording window refuses the direct bake command");
        app->RecordingWindow()=nullptr;
        auto& record=*mdl::Mdl(model);const std::uint32_t boneCount=record.boneCount;mdl::BoneRecord* const boneTable=record.boneTable;
        record.boneCount=0;bake::RefreshMenu(app);
        Check(!bake::CanBake(app),"a model without bones refuses baking");
        Check(!CommandEnabled(baking,bake::kBakeMenuCommand),"a model without bones disables the bake entry");
        Check(bake::HandleCommand(app,bake::kBakeMenuCommand)&&dialogs==quiet,"a model without bones refuses the direct bake command");
        record.boneCount=boneCount;record.boneTable=nullptr;bake::RefreshMenu(app);
        Check(!bake::CanBake(app),"a model without a bone table refuses baking");
        record.boneTable=boneTable;bake::RefreshMenu(app);
        Check(bake::CanBake(app)&&CommandEnabled(baking,bake::kBakeMenuCommand),"restoring the bone data restores the bake entry");
        app->ModelSlot(0)=nullptr;bake::RefreshMenu(app);
        Check(!bake::CanBake(app)&&!CommandEnabled(baking,bake::kBakeMenuCommand),"no selected model refuses baking");
        Check(CommandEnabled(baking,bake::kExportMenuCommand)==alembic_export::CanExport(app),"export state tracks CanExport without a model");
        app->ModelSlot(0)=model;bake::RefreshMenu(app);
        Check(bake::CanBake(app)&&CommandEnabled(baking,bake::kBakeMenuCommand),"a selected model restores the bake entry");
        Check(CommandEnabled(baking,bake::kExportMenuCommand)==alembic_export::CanExport(app),"export state tracks CanExport with a model");}
    // While a bake holds the interface lock every other baking entry stays
    // disabled, and a directly dispatched clear command is refused as well.
    if(baking){
        {
            bake::ScopedUiLock lock(app->MainWindow());
            bake::RefreshMenu(app);
            Check(bake::UiLocked(),"the regression holds the interface lock");
            Check(!bake::CanBake(app),"CanBake refuses a held interface lock");
            Check(!CommandEnabled(baking,bake::kBakeMenuCommand),"the lock disables the bake entry");
            Check(!CommandEnabled(baking,bake::kExportMenuCommand),"the lock disables the export entry");
            Check(!CommandEnabled(baking,bake::kClearMeshBakeMenuCommand),"the lock disables the clear entry");
            const int quiet=dialogs;
            Check(bake::HandleCommand(app,bake::kClearMeshBakeMenuCommand)&&dialogs==quiet,"the lock refuses the direct clear command");
        }
        bake::RefreshMenu(app);
        Check(!bake::UiLocked(),"the menu regression released the interface lock");
        Check(CommandEnabled(baking,bake::kExportMenuCommand)==alembic_export::CanExport(app),"the export entry recovers after the lock");
    }
    // Drive the scope dialog itself: it must mirror the cache state, offer only
    // the scopes that hold data, and clear nothing when it is cancelled.  While
    // the export guard says no it must not open at all.
    Check(alembic_export::CanExport(app),"a scene is exportable before the clear dialog check");
    app->PlaybackActive()=1;
    const int refused=dialogs;
    Check(bake::HandleCommand(app,bake::kClearMeshBakeMenuCommand),"the clear command keeps its id while the guard refuses");
    Check(dialogs==refused,"the clear dialog is refused while export is not allowed");
    app->PlaybackActive()=0;
    language=savedLanguage;inspectApp=app;
    const int dialogsBefore=dialogs;inspect=true;
    Check(bake::HandleCommand(app,bake::kClearMeshBakeMenuCommand),"the clear command opens its own scope dialog");
    inspect=false;inspectApp=nullptr;
    Check(dialogs==dialogsBefore+1,"the clear command opens exactly one dialog");
    Check(mesh_bake_cache::Empty(),"the cancelled clear dialog leaves the cache untouched");
    std::puts("hidden menu regression complete");
}
// Grows the selected model's bone-key pool past the original capacity and links
// one real key into a bone track there.  Everything the scene run does next -
// the bake, the PMM save/reload and the VMD save/reload - then has to carry a
// key that only a runtime-capacity bound can see.  The bone is chosen among
// those a VMD can name again (<= 15 bytes, unique), so the motion round trip
// really compares the grown key instead of filtering it out.
void GrowPoolForCapacity(MMDApp* app){
    auto* raw=app->SelectedModel();if(raw==nullptr)return;
    auto& m=*mdl::Mdl(raw);
    const std::size_t bigSlot=mdl::BoneKeyCapacity(m);
    Check(mdl::BoneKeyPoolEnsure(m,static_cast<std::uint64_t>(bigSlot)+1),"grow the bone-key pool past the original capacity");
    Check(mdl::BoneKeyCapacity(m)>bigSlot,"the grown pool covers the new slot");
    std::map<std::string,int> nameCounts;
    for(unsigned b=0;b<m.boneCount;++b)++nameCounts[BoneFieldName(m.boneTable[b])];
    unsigned bone=0;bool nameable=false;
    for(unsigned b=0;b<m.boneCount;++b){const std::string name=BoneFieldName(m.boneTable[b]);
        if(nameCounts[name]==1&&name.size()<=15&&name.find('\n')==std::string::npos){bone=b;nameable=true;break;}}
    // The record must sit in a real track: the save paths name a key by walking
    // `previous` back to a bone head, and the reload compares whole tracks.
    unsigned tail=bone;std::size_t guard=0;
    while(m.boneKeys[tail].next&&guard++<mdl::BoneKeyCapacity(m))tail=m.boneKeys[tail].next;
    const unsigned frame=m.maxFrame+100;
    mdl::BoneKey key{};key.frame=frame;key.previous=tail;key.next=0;key.rotation[3]=1.0f;
    key.position[0]=1.0f;key.position[1]=2.0f;key.position[2]=3.0f;key.physicsDisabled=1;key.allocated=1;
    m.boneKeys[bigSlot]=key;
    m.boneKeys[tail].next=static_cast<std::uint32_t>(bigSlot);
    if(frame>m.maxFrame)m.maxFrame=frame;
    const bone_scale::Matrix grownScale{{1.25f,0,0,0,1.5f,0,0,0,.75f}};
    bone_scale::SetKey(raw,int(bigSlot),grownScale);
    std::printf("capacity scene: pool grown to %zu slots, key bone=%u frame=%u slot=%zu nameable=%d\n",
                mdl::BoneKeyCapacity(m),bone,frame,bigSlot,nameable?1:0);
}
void Scene(const wchar_t* source,const std::filesystem::path& output){
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};app->InitDefaults();g_Block=app.get();
    Check(InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE),"initialize hidden production renderer");ShowWindow(app->MainWindow(),SW_HIDE);
    int initialDialogs=dialogs;wcscpy_s(app->EnvFileName(),256,source);LoadSceneFile();
    Check(dialogs==initialDialogs&&app->ModelSlot(0),"load supplied scene without dialogs");if(!app->ModelSlot(0))return;
    app->SelectedModelSlot()=0;MenuRegression(app.get());
    initialDialogs=dialogs;  // the menu regression drives its own hidden scope dialog
    auto* raw=app->SelectedModel();auto& m=*mdl::Mdl(raw);
    const bone_scale::Matrix scale{{1.5f,.1f,0,0,.8f,0,0,0,1.2f}};
    for(unsigned b=0;b<m.boneCount;++b)bone_scale::SetKey(raw,int(b),scale);
    if(growCapacity)GrowPoolForCapacity(app.get());
    SeekModelFrame(raw,0,2);ModelApplyMorphs(raw);SetPhysicsMode(raw,0,app->ModelSlots(),2);
    ModelDynamicReseat(raw);ModelKinematicSync(raw);bone_scale::ApplyTransforms(raw);
    std::vector<const btCollisionShape*> collisionShapes;
    for(unsigned i=0;i<m.rigidCount;++i)collisionShapes.push_back(static_cast<btRigidBody*>(m.rigidTable[i].body)->getCollisionShape());
    const auto scaleBefore=bone_scale::Capture(raw);
    std::printf("scene bones=%u rigid=%u joints=%u\n",m.boneCount,m.rigidCount,m.jointCount);
    auto record=std::make_unique<mdl::ModelRecord>(m);
    // Snapshots cover the pool's runtime capacity, so a grown pool (see
    // GrowPoolForCapacity below) is compared completely.
    const std::size_t poolCapacity=mdl::BoneKeyCapacity(m);
    std::vector<mdl::BoneKey> original(m.boneKeys,m.boneKeys+poolCapacity);
    std::vector<mdl::BoneRecord> bones(m.boneTable,m.boneTable+m.boneCount);
    std::vector<btTransform> bodyTransforms;for(unsigned i=0;i<m.rigidCount;++i)bodyTransforms.push_back(static_cast<btRigidBody*>(m.rigidTable[i].body)->getWorldTransform());
    const auto unchanged=[&]{Check(!std::memcmp(record.get(),&m,sizeof m)&&Equal(original,m.boneKeys)&&Equal(bones,m.boneTable),"offline evaluation leaves live model byte-identical");for(unsigned i=0;i<m.rigidCount;++i){auto* body=static_cast<btRigidBody*>(m.rigidTable[i].body);Check(bodyTransforms[i]==body->getWorldTransform(),"offline evaluation leaves live bodies unchanged");Check(collisionShapes[i]==body->getCollisionShape(),"offline evaluation leaves live scaled shapes unchanged");}Check(bone_scale::Capture(raw).keys==scaleBefore.keys&&bone_scale::Capture(raw).pose==scaleBefore.pose,"offline evaluation leaves scale keys and pose unchanged");};
    bake::Request request;request.lastFrame=10;request.settings=bake::BulletDefaults();
    auto cancelled=bake::Compute(*app,request,[](int f,int){return f<4;});Check(cancelled.error==bake::Error::Cancelled&&cancelled.changes.empty(),"mid-simulation cancel is transactional");unchanged();
    auto baked=bake::Compute(*app,request);std::printf("baked error=%d bones=%d changed=%zu steps=%llu\n",int(baked.error),baked.boneCount,baked.changes.size(),static_cast<unsigned long long>(baked.simulationSteps));
    Check(baked.error==bake::Error::None&&!baked.changes.empty(),"compute physics keys");unchanged();if(baked.error!=bake::Error::None){ShutdownCleanup(app.get());g_Block=nullptr;return;}
    Check(!baked.scales.empty(),"offline bake stages scale samples");
    for(const auto& value:baked.scales)Check(bone_scale::Valid(value.second),"staged scale is valid");
    Check(baked.simulationSteps==22,"60 Hz includes one boundary frame");
    Check(SameResults(baked,bake::Compute(*app,request)),"repeated bake is deterministic");unchanged();
    request.settings.rateMultiplier=2;request.settings.substeps=2;auto fine=bake::Compute(*app,request);
    Check(fine.error==bake::Error::None&&fine.simulationSteps==baked.simulationSteps*4,"multiplier and substeps reach solver");Check(!SameResults(baked,fine),"time resolution changes simulated keys");
    request.settings=bake::BulletDefaults();request.settings.erp=.5f;auto erp=bake::Compute(*app,request);Check(erp.error==bake::Error::None&&!SameResults(baked,erp),"ERP changes physical results");
    request.settings=bake::BulletDefaults();request.settings.cfm=.1f;auto soft=bake::Compute(*app,request);Check(soft.error==bake::Error::None&&!SameResults(baked,soft),"CFM changes joint results");unchanged();
    request.settings=bake::BulletDefaults();request.firstFrame=5;request.lastFrame=7;
    auto partial=bake::Compute(*app,request);Check(partial.error==bake::Error::None&&partial.sampledFrames==3,"nonzero bake range succeeds");
    for(const auto& change:partial.changes){const auto& key=change.key;if(key.frame>=4&&key.frame<=8)continue;
        const auto& old=original[change.keyIndex];Check(!std::memcmp(key.position,old.position,12)&&!std::memcmp(key.rotation,old.rotation,16)&&key.physicsDisabled==old.physicsDisabled,"outside range only link fields change");}
    request.firstFrame=request.lastFrame=5;auto single=bake::Compute(*app,request);
    Check(single.error==bake::Error::None&&single.sampledFrames==1,"single frame bake succeeds");
    std::vector<unsigned char> selection(m.boneSelection,m.boneSelection+m.boneCount);std::memset(m.boneSelection,0,m.boneCount);
    unsigned chosen=m.rigidTable[1].boneIndex;for(unsigned i=0;i<m.rigidCount;++i)if(!m.rigidTable[i].staticFlag&&m.rigidTable[i].boneIndex>=0){chosen=m.rigidTable[i].boneIndex;break;}
    m.boneSelection[chosen]=1;request.selectedOnly=true;auto selected=bake::Compute(*app,request);
    Check(selected.error==bake::Error::None&&selected.boneCount==1,"selected-only scope limits physical targets");
    request.physics=false;request.keyframes=true;auto resampled=bake::Compute(*app,request);
    Check(resampled.error==bake::Error::None&&resampled.boneCount==1&&resampled.sampledFrames==1,"ordinary keyframe resampling works independently");
    request.physics=true;request.keyframes=false;request.selectedOnly=false;
    request.boneCoverage=channel_bake::Coverage::Explicit;request.boneIndices={int(chosen)};
    request.firstFrame=2;request.lastFrame=8;request.step=3;
    const auto sparse=bake::Compute(*app,request);
    Check(sparse.error==bake::Error::None&&sparse.boneCount==1&&sparse.sampledFrames==3,"explicit bone range and sparse sampling reach physics compute");
    auto sparsePool=original;for(const auto& change:sparse.changes){if(size_t(change.keyIndex)>=sparsePool.size())sparsePool.resize(size_t(change.keyIndex)+1);sparsePool[change.keyIndex]=change.key;}
    std::vector<unsigned> sampledFrames;
    for(unsigned slot=chosen;;){const auto& k=sparsePool[slot];if(k.frame>=2&&k.frame<=8){sampledFrames.push_back(k.frame);Check(k.physicsDisabled==1,"every sparse baked key disables repeated physics");}if(!k.next)break;slot=k.next;}
    Check(sampledFrames==std::vector<unsigned>({2,5,8}),"sparse bake replaces old interior keys and retains the requested sample grid");
    request.boneIndices={int(m.boneCount)};Check(bake::Compute(*app,request).error==bake::Error::InvalidRequest,"invalid bone selection never widens to whole model");
    // The fixed "frames * bones > 600000" precheck is gone: a bake is grown into
    // the pool on demand and only refused when its slot count cannot be
    // represented at all (kBoneKeyCapacityLimit); running out of memory is a
    // transactional failure (`Error::Allocation`) instead.  An oversized range
    // therefore reaches the first progress check now, which is where a caller
    // stops it before any pool copy or simulation.
    std::memcpy(m.boneSelection,selection.data(),selection.size());request={};request.settings=bake::BulletDefaults();request.lastFrame=100000;Check(bake::Compute(*app,request,[](int,int){return false;}).error==bake::Error::Cancelled,"an oversized range is cancelled at the first check, not refused by a fixed capacity");unchanged();
    Check(bake::Commit(raw,baked,app->CurrentFrame()),"commit real baked result");std::vector<mdl::BoneKey> committed(m.boneKeys,m.boneKeys+poolCapacity);
    for(const auto& value:baked.scales)Check(bone_scale::Key(raw,value.first)==value.second,"commit keeps sampled source scale");
    std::vector<std::vector<mdl::BoneKey>> tracks;unsigned disabled=0;for(unsigned b=0;b<m.boneCount;++b){tracks.push_back(Track(m,b));for(const auto& k:tracks.back())if(k.frame<=10&&k.physicsDisabled)++disabled;}
    Check(disabled>=unsigned(baked.boneCount*11),"baked physical keys disable repeated physics");
    int frame=0;UndoModelEdit(raw,&frame);Check(Equal(original,m.boneKeys)&&m.maxFrame==record->maxFrame,"real bake undo restores all original keys");Check(bone_scale::Capture(raw).keys==scaleBefore.keys,"real bake undo restores original scale");RedoModelEdit(raw,&frame);Check(Equal(committed,m.boneKeys),"real bake redo restores all keys");
    for(const auto& value:baked.scales)Check(bone_scale::Key(raw,value.first)==value.second,"real bake redo restores scale samples");
    for(int sample:{2,7,10}){SeekModelFrame(raw,sample,2);ModelApplyMorphs(raw);SetPhysicsMode(raw,1,app->ModelSlots(),2);
        for(unsigned b=0;b<m.boneCount;++b){for(const auto& key:tracks[b])if(key.frame==unsigned(sample)&&key.physicsDisabled){Check(m.boneTable[b].physicsDisabled==1,"baked playback disables the solver pose");break;}}}
    // Every key the baked model holds, with its complete scale matrix, before
    // the project is written; the reload gets a new model pointer, so this is
    // the only value the post-reload comparison may be based on.
    const auto scaleBeforePmm=ScaleKeys(raw);
    std::filesystem::create_directories(output);auto pmm=output/L"baked-roundtrip.pmm",vmd=output/L"baked-motion.vmd";
    wcscpy_s(app->EnvFileName(),256,pmm.c_str());app->SceneModified()=1;SaveSceneFile(app.get());Check(app->SceneModified()==0&&std::filesystem::exists(pmm),"save baked PMM");
    app->state.optflag[0]=0;SaveVmdFile(vmd.c_str());Check(std::filesystem::exists(vmd)&&std::filesystem::file_size(vmd)>54,"export baked VMD");
    std::ifstream motion(vmd,std::ios::binary);motion.seekg(50);unsigned count=0;motion.read(reinterpret_cast<char*>(&count),4);unsigned flags=0;
    for(unsigned i=0;i<count;++i){unsigned char key[111]{};motion.read(reinterpret_cast<char*>(key),sizeof key);if(key[49]==0x63&&key[50]==0x0f)++flags;}
    Check(flags>0,"VMD exports the native physics disabled marker");
    wcscpy_s(app->EnvFileName(),256,pmm.c_str());LoadSceneFile();Check(app->ModelSlot(0)!=nullptr,"reload baked PMM");auto& loaded=*mdl::Mdl(app->ModelSlot(0));
    for(unsigned b=0;b<loaded.boneCount;++b){const auto keys=Track(loaded,b);Check(keys.size()==tracks[b].size(),"PMM retains complete baked track");if(keys.size()!=tracks[b].size())continue;
        for(size_t i=0;i<keys.size();++i){const auto& a=keys[i];const auto& old=tracks[b][i];
            const bool equal=a.frame==old.frame&&a.physicsDisabled==old.physicsDisabled&&std::memcmp(a.position,old.position,12)==0&&std::memcmp(a.rotation,old.rotation,16)==0;
            if(!equal)std::printf("roundtrip bone=%u frame=%u flags=%u/%u p=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g) q=(%.9g,%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g,%.9g)\n",b,a.frame,a.physicsDisabled,old.physicsDisabled,a.position[0],a.position[1],a.position[2],old.position[0],old.position[1],old.position[2],a.rotation[0],a.rotation[1],a.rotation[2],a.rotation[3],old.rotation[0],old.rotation[1],old.rotation[2],old.rotation[3]);
            Check(equal,"PMM retains baked pose and physics flag");}}
    // The reload allocated a new model: every scale query below goes through
    // the freshly selected pointer, never through the one the project was saved
    // from.
    const auto scaleAfterPmm=ScaleKeys(app->ModelSlot(0));
    Check(scaleAfterPmm.size()==scaleBeforePmm.size(),"PMM reload keeps every scaled key");
    for(const auto& entry:scaleBeforePmm){const auto found=scaleAfterPmm.find(entry.first);
        const bool kept=found!=scaleAfterPmm.end()&&found->second==entry.second;
        if(!kept)std::printf("PMM scale bone=%u frame=%u missing or changed\n",entry.first.first,entry.first.second);
        Check(kept,"PMM reload keeps the complete 9-float scale matrix of every bone and frame");}
    // ---- physics flags and scale through a real VMD save/load -------------
    // SaveVmdFile exports only allocated key records and rebases every frame by
    // the minimum frame of the whole motion; LoadVmdFile registers them back at
    // (frame - minFrame) + the current frame and then applies the scale tail to
    // exactly those imported keys.
    app->SelectedModelSlot()=0;auto* exported=app->SelectedModel();
    Check(exported!=nullptr,"the reloaded project keeps model slot 0 selected");
    const auto named=UniqueVmdBoneNames(*mdl::Mdl(exported));
    const auto expectedZero=ExpectedImport(exported,0,named);
    const auto expectedOffset=ExpectedImport(exported,37,named);
    const auto allocated=AllocatedKeys(exported);
    Check(!expectedZero.empty(),"the reloaded model has keys a motion export can carry");
    unsigned expectedDisabled=0,expectedScaled=0;
    for(const auto& entry:expectedZero){if(entry.second.physicsDisabled)++expectedDisabled;if(!bone_scale::Identity(entry.second.scale))++expectedScaled;}
    Check(expectedDisabled>0,"the exported motion carries the physics disabled flag on real keys");
    Check(expectedScaled>0,"the exported motion carries non-identity scale on real keys");
    const std::filesystem::path roundtrip=output/L"baked-motion-roundtrip.vmd";
    app->state.optflag[0]=0;SaveVmdFile(roundtrip.c_str());
    Check(std::filesystem::exists(roundtrip)&&std::filesystem::file_size(roundtrip)>54,"export the motion for the VMD roundtrip");
    {std::ifstream stream(roundtrip,std::ios::binary);char name[21]{};unsigned declared=0;
     stream.seekg(30);stream.read(name,20);stream.seekg(50);stream.read(reinterpret_cast<char*>(&declared),4);
     Check(!std::strcmp(name,mdl::Mdl(exported)->name),"the exported motion names the model it came from");
     Check(declared==static_cast<unsigned>(allocated.size()),"the motion carries exactly the allocated bone keys");}
    app->state.currentFrame=0;LoadVmdFile(roundtrip.c_str());
    auto* importedZero=app->SelectedModel();Check(importedZero!=nullptr,"VMD import keeps the scene model");
    std::printf("VMD roundtrip compares %zu of %zu allocated keys by unique bone name\n",expectedZero.size(),allocated.size());
    CompareImportedKeys(importedZero,expectedZero,named,"VMD import restores every physics flag and scale matrix");
    Check(expectedOffset.size()==expectedZero.size()&&expectedOffset!=expectedZero,"an offset shifts every exported frame");
    app->state.currentFrame=37;LoadVmdFile(roundtrip.c_str());
    auto* importedOffset=app->SelectedModel();
    Check(importedOffset==exported,"an import does not replace the model the motion is loaded into");
    CompareImportedKeys(importedOffset,expectedOffset,named,"VMD import at an offset lands every key at the offset frame");
    Check(dialogs==initialDialogs,"integration never needs a modal prompt");
    // ---- a real bake locks the interface and leaves only Cancel usable -----
    // The hook drives the hidden bake dialog through one real run at frame 0,
    // and BakeStepHook inspects the interface from inside the lock.
    runBakeMainWindow=app->MainWindow();runBake=true;runBakeCancel=false;runBakeStep=false;
    const int lockedDialogs=dialogs;
    Check(bake::HandleCommand(app.get(),bake::kBakeMenuCommand),"the bake command starts a locked run");
    runBake=false;
    Check(runBakeStep,"the locked run reached the simulation");
    Check(dialogs>lockedDialogs,"the running bake opened its own dialog");
    Check(IsWindowEnabled(runBakeMainWindow)==TRUE,"the interface is usable again after the bake");
    // Cancelling the same run through the dialog releases the lock and writes
    // nothing: the bake dialog stays open instead of closing itself.
    const auto undoBefore=mdl::Mdl(app->ModelSlot(0))->undoState[0];
    runBake=true;runBakeCancel=true;runBakeStep=false;
    Check(bake::HandleCommand(app.get(),bake::kBakeMenuCommand),"the bake command starts a cancellable run");
    runBake=false;runBakeCancel=false;
    Check(runBakeStep&&!bake::UiLocked(),"cancelling a locked run releases the interface lock");
    Check(IsWindowEnabled(runBakeMainWindow)==TRUE,"the interface is usable again after a cancelled bake");
    Check(mdl::Mdl(app->ModelSlot(0))->undoState[0]==undoBefore,"a cancelled bake writes no keys");
    ShutdownCleanup(app.get());g_Block=nullptr;
    std::printf("scene integration complete: physical_keys=%u vmd_flags=%u\n",disabled,flags);
}
}
// `physics_bake_tests --capacity`: the runtime bone-key pool acceptance without
// a scene, a device or a modal prompt.
//   1. a real bake::Commit lands a key in a slot past the original pool;
//   2. Undo/Redo restore that slot byte for byte while the pool stays grown;
//   3. a real SaveVmdFile writes it (the file's key count proves it);
//   4. the loader's own registrar, RegisterBoneKey, inserts the next key into a
//      slot past the original pool as well.
// The full VMD reload (LoadVmdFile) is deliberately not called here: its tail
// unconditionally paints the curve panel, re-evaluates the selection and seeks
// the frame, which a model without a scene cannot service.  The end-to-end
// save/reload of a grown pool runs in the scene mode instead:
//   physics_bake_tests --capacity-scene <model.pmx> <output-directory>
void CapacityMode(){
    std::puts("capacity mode: start");
    const std::size_t initial=mdl::kBoneKeyCapacity;
    const unsigned bigSlot=static_cast<unsigned>(initial)+7;

    // ---- 1. a real commit past the original pool -------------------------
    Fixture f;
    std::puts("capacity mode: fixture ready");
    std::strcpy(f.bones[0].name,"boneA");
    std::strcpy(f.bones[1].name,"boneB");
    f.model->maxFrame=20;f.model->searchCursor=2;
    Check(mdl::BoneKeyCapacity(*f.model)==initial,"a borrowed vector pool reports the original capacity");
    mdl::BoneKey head{};head.frame=10;head.next=bigSlot;head.rotation[3]=1.0f;head.allocated=1;
    mdl::BoneKey big{};big.frame=20;big.previous=0;big.rotation[3]=1.0f;big.position[1]=7.0f;big.physicsDisabled=1;big.allocated=1;
    bake::Result grown;grown.maxFrame=20;grown.changes={{0,head},{static_cast<int>(bigSlot),big}};
    Check(bake::Commit(f.raw(),grown,5),"commit a key past the original pool");
    const std::size_t grownCapacity=mdl::BoneKeyCapacity(*f.model);
    Check(grownCapacity>bigSlot,"the pool covers the committed slot");
    std::printf("capacity mode: committed, pool capacity=%zu\n",grownCapacity);
    if(grownCapacity<=bigSlot){std::puts("capacity mode: growth failed, stopping");g_Block=nullptr;return;}
    Check(f.model->boneKeys!=f.keys.data(),"growth publishes a new block");
    mdl::BoneKey* pool=f.model->boneKeys;
    Check(pool[bigSlot].frame==20&&pool[bigSlot].position[1]==7.0f&&pool[bigSlot].physicsDisabled==1,
          "the committed key lands in the grown block");
    Check(pool[0].next==bigSlot,"the bone head links to the grown key");
    Check(f.keys[0].next==0&&f.keys[7].frame==0,
          "the borrowed vector the fixture handed in is adopted, never released");

    // ---- 2. undo and redo keep the grown slot stable ---------------------
    int frame=0;UndoModelEdit(f.raw(),&frame);
    Check(frame==5,"undo restores the recorded frame");
    Check(pool[bigSlot].frame==0&&pool[bigSlot].allocated==0,
          "undo restores the grown slot to the empty record");
    Check(pool[0].next==0,"undo restores the head link");
    Check(mdl::BoneKeyCapacity(*f.model)==grownCapacity,"the pool stays grown after undo");
    RedoModelEdit(f.raw(),&frame);
    Check(pool[bigSlot].frame==20&&pool[0].next==bigSlot,"redo restores the grown key");
    std::puts("capacity mode: undo/redo complete");
    // A scale key on the grown slot as well: the VMD tail extension walks the
    // pool by slot, so its grown-capacity scan is exercised by the export below.
    bone_scale::SetKey(f.raw(),static_cast<int>(bigSlot),
                       bone_scale::Matrix{{1.25f,0,0,0,1.5f,0,0,0,.75f}});

    // ---- 3. a real VMD export carries the grown key ----------------------
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
    app->ModelSlot(0)=f.raw();app->SelectedModelSlot()=0;
    app->MainWindow()=CreateWindowExW(0,L"STATIC",L"capacity mode",WS_OVERLAPPED,
                                      0,0,320,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    app->Hwnd()=f.model->hwnd=app->MainWindow();
    app->state.currentFrame=30;app->state.optflag[0]=0;
    auto* previousApp=g_Block;g_Block=app.get();
    const auto directory=std::filesystem::temp_directory_path()/L"mmd-bone-key-capacity";
    std::filesystem::create_directories(directory);
    const auto vmdPath=directory/L"capacity-motion.vmd";
    std::filesystem::remove(vmdPath);
    std::puts("capacity mode: exporting VMD");
    SaveVmdFile(vmdPath.c_str());
    std::puts("capacity mode: VMD export returned");
    Check(std::filesystem::exists(vmdPath)&&std::filesystem::file_size(vmdPath)>54,
          "the VMD export wrote a file");
    {
        std::ifstream stream(vmdPath,std::ios::binary);
        unsigned declared=0;stream.seekg(50);stream.read(reinterpret_cast<char*>(&declared),4);
        Check(declared==2,"the VMD export carries the key written from a grown slot");
        std::printf("capacity: VMD declared bone keys=%u (1 would mean the grown slot was skipped)\n",declared);
    }

    // ---- 4. the loader's registrar inserts past the pool end -------------
    {
        std::puts("capacity mode: registrar insert");
        auto& ring=f.model->undoRings[0].slots[f.model->undoState[0]];
        std::free(ring.auxiliaryPose);
        ring.auxiliaryPose=std::malloc(4*sizeof(mdl::BoneKeySnapshot));
        ring.dirty=0;
        const unsigned insertSlot=bigSlot+1000;
        Check(insertSlot<mdl::BoneKeyCapacity(*f.model),"the insertion slot is inside the grown pool");
        f.model->searchCursor=static_cast<int>(insertSlot);
        // The parse record the VMD loader fills: name at +0, frame at +32,
        // quaternion at +36, position at +52, the 3939 physics marker at +64.
        unsigned char rec[96]={};
        std::strcpy(reinterpret_cast<char*>(rec),"boneA");
        const unsigned fileFrame=77;std::memcpy(rec+32,&fileFrame,4);
        const float position[3]={1.0f,2.0f,3.0f};std::memcpy(rec+52,position,sizeof position);
        const float quaternion[4]={0.0f,0.0f,0.0f,1.0f};std::memcpy(rec+36,quaternion,sizeof quaternion);
        rec[64]=1;
        const int offset=30;
        Check(RegisterBoneKey(f.raw(),rec,offset,0),
              "the loader registrar inserts a key past the original pool");
        const unsigned expectedFrame=fileFrame+static_cast<unsigned>(offset);
        const std::size_t capacity=mdl::BoneKeyCapacity(*f.model);
        unsigned slot=0,found=0xFFFFFFFFu;std::size_t guard=0;
        while(slot<capacity&&guard++<capacity){
            if(f.model->boneKeys[slot].frame==expectedFrame){found=slot;break;}
            if(f.model->boneKeys[slot].next==0)break;
            slot=f.model->boneKeys[slot].next;
        }
        Check(found!=0xFFFFFFFFu,"the imported key is in the bone track");
        Check(found>mdl::kBoneKeyCapacity,"the imported key sits in a slot past the original pool");
        Check(found==insertSlot,"the registrar took the grown slot the cursor pointed at");
        if(found<capacity)Check(f.model->boneKeys[found].physicsDisabled==1&&
                                f.model->boneKeys[found].position[1]==2.0f,
                                "the imported key keeps its pose and physics marker");
    }

    g_Block=previousApp;
    DestroyWindow(app->MainWindow());
    f.model->hwnd=nullptr;
    std::filesystem::remove(vmdPath);
    mdl::BoneKeyPoolDestroy(f.model->boneKeys);
    std::puts("runtime pool capacity: commit, undo/redo, VMD export and registrar insert complete");
}
#include "dense_keyframe_checks.hpp"
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);INITCOMMONCONTROLSEX cc{sizeof cc,ICC_WIN95_CLASSES};InitCommonControlsEx(&cc);
    auto hide=SetWindowsHookExW(WH_CBT,HideHook,nullptr,GetCurrentThreadId());auto hook=SetWindowsHookExW(WH_CALLWNDPROCRET,DialogHook,nullptr,GetCurrentThreadId());
    if(argc==2&&!wcscmp(argv[1],L"--dense-playback"))DenseKeyframeChecks();
    else if(argc==2&&!wcscmp(argv[1],L"--select-all"))AllChannelPicker();
    else if(argc==2&&!wcscmp(argv[1],L"--scene-physics"))ScenePhysicsUnit();
    else if(argc==2&&!wcscmp(argv[1],L"--capacity"))CapacityMode();
    else if(argc==2&&!wcscmp(argv[1],L"--warnings"))WarningLogic();
    else if(argc==4&&!wcscmp(argv[1],L"--capacity-scene")){growCapacity=true;Scene(argv[2],argv[3]);}
    else if(argc==3&&!wcscmp(argv[1],L"--ui")){images=argv[2];std::filesystem::create_directories(images);Unit();}
    else if(argc==3)Scene(argv[1],argv[2]);else {LayerUnit();Unit();}
    UnhookWindowsHookEx(hook);UnhookWindowsHookEx(hide);std::printf("failures=%d\n",failures);return failures?1:0;
}
