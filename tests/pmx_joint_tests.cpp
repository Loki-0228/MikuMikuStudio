// MAINTENANCE: regression-keep; missing PMX joint endpoints disable only that joint.
#define NOMINMAX
#include "mikumikustudio/pmx_joint_validation.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <btBulletDynamicsCommon.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <vector>
using namespace mikumikustudio;
namespace mikumikustudio { void InitPhysicsModelDialog(HWND); void ReleasePhysicsEditScratch(MMDApp*); }
namespace {
using Bytes=std::vector<unsigned char>;
int failures=0,dialogs=0;
std::wstring expectedText,expectedTitle;
void Check(bool ok,const char* label){if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
template<class T>void Put(Bytes& b,T v){const auto* p=reinterpret_cast<const unsigned char*>(&v);b.insert(b.end(),p,p+sizeof v);}
void Text(Bytes& b,const wchar_t* value){const auto n=unsigned(wcslen(value)*2);Put(b,n);const auto* p=reinterpret_cast<const unsigned char*>(value);b.insert(b.end(),p,p+n);}
void Index(Bytes& b,int value,int width){if(width==1)Put(b,static_cast<signed char>(value));else if(width==2)Put(b,static_cast<short>(value));else Put(b,value);}
struct Link { int a,b; };
Bytes Model(int width,int bodies,const std::vector<Link>& links,bool badShape=false,bool truncated=false){
    Bytes b{'P','M','X',' '};Put(b,2.f);
    for(unsigned char v:{8,0,0,1,1,1,1,1})Put(b,v);Put(b,static_cast<unsigned char>(width));
    Text(b,L"joint regression");Text(b,L"joint regression");Text(b,L"");Text(b,L"");
    for(int i=0;i<4;++i)Put(b,0u); // vertices, indices, textures, materials
    Put(b,1u);Text(b,L"root");Text(b,L"root");
    for(int i=0;i<3;++i)Put(b,0.f);
    Index(b,-1,1);Put(b,0u);Put(b,static_cast<unsigned short>(0));
    for(int i=0;i<3;++i)Put(b,0.f);
    Put(b,0u);Put(b,0u); // morphs, display frames
    Put(b,static_cast<unsigned>(bodies));
    for(int i=0;i<bodies;++i){
        Text(b,L"body");Text(b,L"body");Index(b,0,1);
        Put(b,static_cast<unsigned char>(0));Put(b,static_cast<unsigned short>(0));
        Put(b,static_cast<unsigned char>(badShape && i==0 ? 255 : 0));
        for(int c=0;c<3;++c)Put(b,1.f);
        for(int c=0;c<3;++c)Put(b,c==0?float(i):0.f);
        for(int c=0;c<3;++c)Put(b,0.f);
        for(float v:{1.f,.5f,.5f,0.f,.5f})Put(b,v);
        Put(b,static_cast<unsigned char>(1));
    }
    Put(b,truncated?3u:static_cast<unsigned>(links.size()));
    if(truncated){Text(b,L"broken");Put(b,1u);return b;}
    for(auto link:links){
        Text(b,L"左腕接点_测试");Text(b,L"Left arm joint");Put(b,static_cast<unsigned char>(0));
        Index(b,link.a,width);Index(b,link.b,width);
        for(int i=0;i<24;++i)Put(b,0.f);
    }
    return b;
}
struct World {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world{&dispatcher,&broadphase,&solver,&config};PhysicsScene scene{};
    World(){scene.world=&world;world.setGravity(btVector3(0,0,0));}
};
std::unique_ptr<mdl::ModelRecord> Load(World& world,const Bytes& bytes,unsigned char language=0,HWND owner=nullptr,bool success=true){
    wchar_t temp[MAX_PATH]{},path[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);GetTempFileNameW(temp,L"jnt",0,path);
    int fd=_wopen(path,_O_BINARY|_O_WRONLY|_O_TRUNC);_write(fd,bytes.data(),unsigned(bytes.size()));_close(fd);
    fd=_wopen(path,_O_BINARY|_O_RDONLY);_lseek(fd,3,SEEK_SET);
    auto model=std::make_unique<mdl::ModelRecord>();model->scenePtr=&world.scene;model->hwnd=owner;
    PathResolutionWorkspace paths{};
    Check(LoadPMX(reinterpret_cast<unsigned char*>(model.get()),nullptr,0,language,paths,fd)==success,"real PMX load result");
    Check(DeleteFileW(path)!=FALSE,"PMX loader closes fixture");return model;
}
void Dispose(std::unique_ptr<mdl::ModelRecord>& model){ModelDispose(reinterpret_cast<unsigned char*>(model.get()));model.reset();}
LRESULT CALLBACK InspectDialog(int code,WPARAM wp,LPARAM lp){
    if(code>=0){
        const auto& message=*reinterpret_cast<CWPRETSTRUCT*>(lp);
        if(message.message==WM_INITDIALOG){
            wchar_t kind[32]{};GetClassNameW(message.hwnd,kind,32);
            if(wcscmp(kind,L"#32770")==0){
                ++dialogs;wchar_t caption[256]{},text[4096]{};
                GetWindowTextW(message.hwnd,caption,256);GetDlgItemTextW(message.hwnd,65535,text,4096);
                Check(expectedTitle==caption,"real warning dialog uses current UI language");
                Check(expectedText==text,"real warning contains every endpoint and Unicode name");
                // End during WM_INITDIALOG, before the modal window is shown.
                EndDialog(message.hwnd,IDOK);
            }
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
void IndexWidths(){
    for(int width:{1,2,4}){
        World w;auto other=Load(w,Model(width,2,{{0,1}}));
        Check(w.world.getNumConstraints()==1,"unrelated model owns constraint zero");
        const std::vector<Link> links={{-1,0},{0,-1},{-1,-1},{2,0},{0,2},{0,1}};
        auto model=Load(w,Model(width,2,links));
        Check(model->jointCount==links.size(),"retain original joint numbering");
        if(model->jointCount==links.size()){
            for(int i=0;i<5;++i)Check(model->jointTable[i].constraint==-1,"invalid joint has no constraint ID");
            Check(model->jointTable[5].constraint>=0,"valid joint after bad joints remains active");
            Check(MissingJointBodies(*model,model->jointTable[0])==1,"identify missing A");
            Check(MissingJointBodies(*model,model->jointTable[1])==2,"identify missing B");
            Check(MissingJointBodies(*model,model->jointTable[2])==3,"identify both missing bodies");
            auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};g_Block=app.get();
            app->ModelSlot(0)=reinterpret_cast<unsigned char*>(model.get());app->SelectedModelSlot()=0;
            if(model->jointTable[0].constraint==-1) {
            InitPhysicsModelDialog(nullptr);
            const auto* scratch=static_cast<const mdl::JointRecord*>(app->state.jointScratchArray);
            Check(scratch[0].constraint==-1 && scratch[5].constraint==0,"editor keeps invalid joints disabled and compacts combo indices");
            Check(app->state.selectedJointIndex==5,"editor selects first valid original joint");
            ReleasePhysicsEditScratch(app.get());
            }
            g_Block=nullptr;
        }
        Check(w.world.getNumConstraints()==2,"only valid constraints enter Bullet");
        Dispose(model);Check(w.world.getNumConstraints()==1,"disposing damaged model preserves other model constraint");
        auto empty=Load(w,Model(width,0,{{-1,-1},{0,1}}));
        Check(empty->jointCount==2,"read joints even with zero rigid bodies");
        if(empty->jointCount==2)Check(empty->jointTable[0].constraint==-1 && empty->jointTable[1].constraint==-1,"zero-body joints remain disabled");
        Dispose(empty);Check(w.world.getNumConstraints()==1,"zero-body disposal preserves other model");
        auto broken=Load(w,Model(width,2,{},false,true),0,nullptr,false);Dispose(broken);
        Check(w.world.getNumConstraints()==1,"partial joint table cannot delete constraint zero");
        Dispose(other);Check(w.world.getNumConstraints()==0 && w.world.getNumCollisionObjects()==0,"all resources released");
    }
}
void Languages(){
    World w;const auto fixture=Model(4,2,{{-1,0},{0,-1},{-1,-1}});
    auto model=Load(w,fixture);HWND owner=CreateWindowExW(0,L"STATIC",L"",0,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    for(unsigned char language:{kUiJapanese,kUiEnglish,kUiChinese}){
        expectedText=PmxJointWarnings(*model,language);expectedTitle=PmxJointWarningTitle(language);
        Check(expectedText.find(language==kUiEnglish?L"Left arm joint":L"左腕接点_测试")!=std::wstring::npos,"localized name selection preserves Unicode");
        Check(expectedText.find(language==kUiJapanese?L"剛体 A":language==kUiChinese?L"刚体 A":L"Rigid body A")!=std::wstring::npos,"localized missing A text");
        Check(expectedText.find(language==kUiJapanese?L"剛体 B":language==kUiChinese?L"刚体 B":L"Rigid body B")!=std::wstring::npos,"localized missing B text");
        const int count=dialogs;HHOOK hook=SetWindowsHookExW(WH_CALLWNDPROCRET,InspectDialog,nullptr,GetCurrentThreadId());
        Check(hook!=nullptr,"install hidden warning hook");
        auto shown=Load(w,fixture,language,owner);UnhookWindowsHookEx(hook);
        Check(dialogs==count+1,"show one summary warning even when showInfo is false");Dispose(shown);
    }
    DestroyWindow(owner);Dispose(model);
    auto missingBody=Load(w,Model(4,2,{{0,1}},true));
    Check(missingBody->jointTable[0].constraint==-1,"unsupported rigid shape cannot pass a null body to Bullet");
    Check(MissingJointBodies(*missingBody,missingBody->jointTable[0])==1,"report rigid A when body creation failed");Dispose(missingBody);
}
}
int main(int argc,char** argv){std::setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);IndexWidths();if(argc==1 || std::strcmp(argv[1],"--indices-only")!=0)Languages();std::printf("pmx_joint_failures=%d warning_dialogs=%d\n",failures,dialogs);return failures?1:0;}