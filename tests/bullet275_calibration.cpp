// Native Bullet 2.75 versus the production Bullet3 C ABI. No Bullet3 C++
// symbols are linked into this executable. Animation/IK use the MMS loader.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "../../MMV/plugins/bullet3/include/mmv_bullet3.h"
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <chrono>
using namespace mikumikustudio;
namespace fs=std::filesystem;
void Require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
struct Profile {
    const char* name; float margin,springLinear,springAngular,springDamping,rigidDamping,erp;
    bool nativeInertia; unsigned iterations=10; void* world=nullptr;
    std::vector<MmvB3State> states;
    double position2=0,angle2=0,maximum=0,cpuMs=0; uint64_t count=0;
    bool collideConnected=true;
};
static MmvB3State State(btRigidBody& body,uint64_t id,bool target=false){
    MmvB3State s{};s.id=id;auto t=body.getWorldTransform();
    if(target&&body.getMotionState())body.getMotionState()->getWorldTransform(t);
    t.getOpenGLMatrix(s.world);
    for(int a=0;a<3;++a){s.linearVelocity[a]=body.getLinearVelocity()[a];s.angularVelocity[a]=body.getAngularVelocity()[a];}
    return s;
}
// The user authorizes name-matched CH4NGE import. Give the background loader
// an artifact copy with only its 20-byte model annotation changed. All motion
// records stay byte-identical; the supplied VMD is never modified.
static void LoadAuthorizedMotion(MMDApp& app,const fs::path& source,const fs::path& output){
    const auto fixture=output/L"CH4NGE-import.vmd";fs::copy_file(source,fixture,fs::copy_options::overwrite_existing);
    std::fstream stream(fixture,std::ios::binary|std::ios::in|std::ios::out);
    stream.seekp(30);stream.write(mdl::Mdl(app.SelectedModel())->name,20);stream.close();
    app.CameraMode()=0;LoadVmdFile(fixture.c_str());
}
int wmain(int argc,wchar_t** argv){try{
    Require(argc>=5,"usage: model.pmx motion.vmd MMVBullet3.dll output [ticks]");
    std::setvbuf(stdout,nullptr,_IONBF,0);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    fs::path output=fs::absolute(argv[4]);fs::create_directories(output);
    auto dll=LoadLibraryW(fs::absolute(argv[3]).c_str());Require(dll!=nullptr,"load Bullet3 DLL");
    auto get=reinterpret_cast<MmvB3GetApiFn>(GetProcAddress(dll,"MmvBullet3GetApi"));
    MmvB3Api api{};api.structureSize=sizeof(api);Require(get&&get(1,&api)==0,"Bullet3 API");
    Require(btGetVersion()==275&&api.bulletVersion>=300,"distinct real Bullet versions");
    auto* app=new MMDApp;g_Block=app;app->state=MMDAppState{};app->InitDefaults();
    Require(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"hidden native host");
    Require(!IsWindowVisible(app->MainWindow()),"hidden window");
    LoadModelFile(app,fs::absolute(argv[1]).c_str(),false);app->SetSelectedModelSlot(0);
    auto* raw=app->ModelSlot(0);Require(raw!=nullptr,"PMX loaded");LoadAuthorizedMotion(*app,fs::absolute(argv[2]),output);
    auto& model=*mdl::Mdl(raw);Require(model.rigidCount&&model.jointCount,"physics PMX");
    if(argc>6&&wcscmp(argv[6],L"make-pmm")==0){
        LoadModelFile(app,fs::absolute(argv[1]).c_str(),false);app->SetSelectedModelSlot(1);app->CameraMode()=0;
        LoadAuthorizedMotion(*app,fs::absolute(argv[2]),output);
        for(unsigned slot=0;slot<2;++slot){auto* r=app->ModelSlot(slot);auto& m=*mdl::Mdl(r);unsigned roots=0;
            // Models may have an unused operation root before the skeleton,
            // and disconnected weapon roots. Translate every root component.
            for(unsigned root=0;root<m.boneCount;++root)if(m.boneTable[root].parent<0){++roots;
                unsigned key=root,guard=0;m.boneKeys[root].allocated=1;m.boneTrackActive[root]=1;
                do{m.boneKeys[key].position[0]+=slot==0?-12.f:12.f;key=m.boneKeys[key].next;Require(++guard<mdl::BoneKeyCapacity(m),"root key chain");}while(key);
            }
            Require(roots>0,"model root components");
            InitModelTrackCursors(r,0,2);AdvanceModelKeyframes(r,0,2);ModelApplyMorphs(r);SetPhysicsMode(r,0,app->ModelSlots(),0);ModelDynamicReseat(r);ModelKinematicSync(r);
            std::printf("scene slot=%u root_components=%u\n",slot,roots);
        }
        app->PlaybackPhysicsMode()=2;app->CurrentFrame()=0;app->PlaybackCursorSeconds()=0;
        const auto pmm=output/L"comparison.pmm";wcscpy_s(app->EnvFileName(),256,pmm.c_str());SaveSceneFile(app);
        Require(fs::exists(pmm)&&fs::file_size(pmm)>100000,"two-model PMM saved");std::puts("PASS two models with CH4NGE, root offsets -12/+12");return 0;
    }
    auto* native=app->Physics()->world;
    static_cast<DynamicsWorld*>(native)->ResetLocalTime();
    const auto evaluate=[&](float seconds,bool initial){
        if(initial)InitModelTrackCursors(raw,seconds,2);
        AdvanceModelKeyframes(raw,seconds,2);ModelApplyMorphs(raw);
        SetPhysicsMode(raw,0,app->ModelSlots(),initial?0:2);ModelKinematicSync(raw);
    };
    evaluate(0,true);ModelDynamicReseat(raw);ModelKinematicSync(raw);
    std::vector<btRigidBody*> originals;
    std::vector<MmvB3Body> bodies;std::vector<MmvB3State> initial;
    std::unordered_map<btRigidBody*,unsigned> indices;
    for(int i=0;i<native->getNumCollisionObjects();++i){
        auto* b=btRigidBody::upcast(native->getCollisionObjectArray()[i]);Require(b!=nullptr,"rigid world");
        if(b->isKinematicObject()&&b->getMotionState())b->getMotionState()->getWorldTransform(b->getWorldTransform());
        b->setInterpolationWorldTransform(b->getWorldTransform());b->setLinearVelocity(btVector3(0,0,0));b->setAngularVelocity(btVector3(0,0,0));
        indices[b]=unsigned(bodies.size());originals.push_back(b);
        MmvB3Body d{};d.id=bodies.size()+1;d.motion=b->isKinematicObject()?MMVB3_KINEMATIC:(b->getInvMass()>0?MMVB3_DYNAMIC:MMVB3_STATIC);
        d.mass=b->getInvMass()>0?1/b->getInvMass():0;
        auto* shape=b->getCollisionShape();d.margin=shape->getMargin();
        if(auto* s=dynamic_cast<btSphereShape*>(shape)){d.shape=MMVB3_SPHERE;d.size[0]=s->getRadius();}
        else if(auto* s=dynamic_cast<btBoxShape*>(shape)){d.shape=MMVB3_BOX;auto v=s->getHalfExtentsWithMargin();for(int a=0;a<3;++a)d.size[a]=v[a];}
        else if(auto* s=dynamic_cast<btCapsuleShape*>(shape)){d.shape=MMVB3_CAPSULE_Y;d.size[0]=s->getRadius();d.size[1]=2*s->getHalfHeight();}
        else if(auto* s=dynamic_cast<btStaticPlaneShape*>(shape)){d.shape=MMVB3_PLANE;for(int a=0;a<3;++a)d.plane[a]=s->getPlaneNormal()[a];d.plane[3]=s->getPlaneConstant();}
        else throw std::runtime_error("unsupported native shape");
        for(int a=0;a<3;++a)d.inertia[a]=b->getInvInertiaDiagLocal()[a]>0?1/b->getInvInertiaDiagLocal()[a]:0;
        d.linearDamping=b->getLinearDamping();d.angularDamping=b->getAngularDamping();d.friction=b->getFriction();d.restitution=b->getRestitution();
        d.collisionGroup=b->getBroadphaseHandle()->m_collisionFilterGroup;d.collisionMask=b->getBroadphaseHandle()->m_collisionFilterMask;
        bodies.push_back(d);initial.push_back(State(*b,d.id));
    }
    std::vector<MmvB3Joint> joints;
    for(unsigned i=0;i<model.jointCount;++i){const auto& j=model.jointTable[i];if(j.constraint<0)continue;
        btGeneric6DofSpringConstraint* c=nullptr;
        for(int k=0;k<native->getNumConstraints();++k)if(native->getConstraint(k)->getUserConstraintId()==j.constraint)c=dynamic_cast<btGeneric6DofSpringConstraint*>(native->getConstraint(k));
        Require(c!=nullptr,"native spring constraint");MmvB3Joint d{};
        d.bodyA=indices.at(&c->getRigidBodyA());d.bodyB=indices.at(&c->getRigidBodyB());d.useLinearReferenceFrameA=1;
        c->getFrameOffsetA().getOpenGLMatrix(d.frameA);c->getFrameOffsetB().getOpenGLMatrix(d.frameB);
        for(int a=0;a<3;++a){d.linearUpper[a]=j.limits[a];d.linearLower[a]=j.limits[a+3];d.angularUpper[a]=j.limits[a+6];d.angularLower[a]=j.limits[a+9];}
        for(int a=0;a<6;++a){d.springEnabled[a]=j.springs[a]>0;d.stiffness[a]=j.springs[a];d.damping[a]=1;}
        joints.push_back(d);
    }
    std::vector<Profile> profiles={
        {"baseline",0,1,1,0,1,.2f,false},
        {"joint275",0,1,1,1,1,.2f,false},
        {"margin275",.04f,1,1,1,1,.2f,false},
        {"inertia275",0,1,1,1,1,.2f,true},
        {"geometry275",.04f,1,1,1,1,.2f,true},
        {"linear075",.04f,.75f,1,1,1,.2f,true},
        {"linear125",.04f,1.25f,1,1,1,.2f,true},
        {"angular075",.04f,1,.75f,1,1,.2f,true},
        {"angular125",.04f,1,1.25f,1,1,.2f,true},
        {"spring050",.04f,.5f,.5f,1,1,.2f,true},
        {"spring150",.04f,1.5f,1.5f,1,1,.2f,true},
        {"jointDamping080",.04f,1,1,.8f,1,.2f,true},
        {"rigidDamping075",.04f,1,1,1,.75f,.2f,true},
        {"rigidDamping125",.04f,1,1,1,1.25f,.2f,true},
        {"erp010",.04f,1,1,1,1,.1f,true},
        {"erp030",.04f,1,1,1,1,.3f,true},
        {"iterations20",.04f,1,1,1,1,.2f,true,20}
    };
    profiles.front().collideConnected=false;
    for(auto& p:profiles){auto b=bodies;auto j=joints;
        for(auto& d:b){if(d.shape==MMVB3_BOX)d.margin=p.margin;if(!p.nativeInertia)for(auto& v:d.inertia)v=0;
            d.linearDamping=std::min(1.f,d.linearDamping*p.rigidDamping);d.angularDamping=std::min(1.f,d.angularDamping*p.rigidDamping);}
        for(auto& d:j){d.disableLinkedCollision=p.collideConnected?0:1;for(int a=0;a<6;++a){d.stiffness[a]*=a<3?p.springLinear:p.springAngular;d.damping[a]=p.springDamping;}}
        MmvB3Scene scene{sizeof(scene),unsigned(b.size()),unsigned(j.size()),MMVB3_SCENE_MMD275,b.data(),initial.data(),j.data()};
        Require(api.create(&scene,&p.world)==0,api.lastError());p.states=initial;
    }
    native->updateAabbs();
    const unsigned ticks=argc>5?_wtoi(argv[5]):1800;Require(ticks>0,"positive duration");
    std::ofstream csv(output/L"trajectory.csv");csv<<"tick,seconds,profile,position_rmse,angle_rmse_deg,max_position,cpu_ms\n";
    std::ofstream binary(output/L"states.f32",std::ios::binary);
    unsigned springs=0,motionKeys=0;for(const auto& j:joints)for(unsigned enabled:j.springEnabled)springs+=enabled;
    for(unsigned i=0;i<mdl::BoneKeyCapacity(model);++i)if(model.boneKeys[i].allocated)++motionKeys;
    double kinematicMotion=0;
    std::ofstream manifest(output/L"manifest.txt");manifest<<"native_version="<<btGetVersion()<<"\nbullet3_version="<<api.bulletVersion<<"\nmodel="<<fs::path(argv[1]).u8string()<<"\nmotion="<<fs::path(argv[2]).u8string()<<"\nbodies="<<bodies.size()<<"\njoints="<<joints.size()<<"\nspring_axes="<<springs<<"\nallocated_motion_keys="<<motionKeys<<"\nticks="<<ticks<<"\nHz=60\nprofiles="<<profiles.size()<<"\nstate_layout=tick every 6; native then profiles in summary order; each body 16 row-major floats\n";
    for(unsigned tick=1;tick<=ticks;++tick){evaluate(tick/60.f,false);
        std::vector<MmvB3State> targets;for(unsigned i=0;i<bodies.size();++i)targets.push_back(State(*originals[i],bodies[i].id,true));
        for(unsigned i=0;i<bodies.size();++i)if(bodies[i].motion==MMVB3_KINEMATIC)for(int a=12;a<15;++a)kinematicMotion=std::max(kinematicMotion,std::abs(double(targets[i].world[a]-initial[i].world[a])));
        native->stepSimulation(1.f/60,1,1.f/60);
        if(tick%6==0)for(auto* b:originals){float m[16];b->getWorldTransform().getOpenGLMatrix(m);binary.write(reinterpret_cast<char*>(m),sizeof(m));}
        for(auto& p:profiles){auto in=p.states;
            for(unsigned i=0;i<bodies.size();++i)if(bodies[i].motion!=MMVB3_DYNAMIC)in[i]=targets[i];
            MmvB3Step step{sizeof(step),unsigned(bodies.size()),1,p.iterations,1.f/60,1.f/60,{0,0,0},p.erp,0};
            for(int a=0;a<3;++a)step.gravity[a]=native->getGravity()[a];
            auto start=std::chrono::steady_clock::now();Require(api.step(p.world,&step,in.data(),p.states.data(),nullptr)==0,api.lastError());
            double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();p.cpuMs+=ms;
            double e2=0,a2=0,mx=0;unsigned n=0;
            for(unsigned i=0;i<bodies.size();++i){const auto& s=p.states[i];
                for(float f:s.world)Require(std::isfinite(f),"finite output");
                if(tick%6==0)binary.write(reinterpret_cast<const char*>(s.world),sizeof(s.world));
                if(bodies[i].motion!=MMVB3_DYNAMIC)continue;
                auto t=originals[i]->getWorldTransform();double e=0;for(int a=0;a<3;++a)e+=std::pow(s.world[12+a]-t.getOrigin()[a],2);
                btTransform b;b.setFromOpenGLMatrix(s.world);auto q=t.getRotation().inverse()*b.getRotation();double angle=2*std::acos(std::min(1.,std::abs(double(q.w()))))*180/3.141592653589793;
                e2+=e;a2+=angle*angle;mx=std::max(mx,std::sqrt(e));++n;
            }
            p.position2+=e2;p.angle2+=a2;p.count+=n;p.maximum=std::max(p.maximum,mx);
            csv<<tick<<','<<tick/60.0<<','<<p.name<<','<<std::sqrt(e2/n)<<','<<std::sqrt(a2/n)<<','<<mx<<','<<ms<<'\n';
        }
        ModelPhysicsReadback(raw);ModelApplyMorphs(raw);SetPhysicsMode(raw,1,app->ModelSlots(),2);
        if(tick%300==0)std::printf("tick=%u/%u\n",tick,ticks);
    }
    Require(kinematicMotion>1e-3&&motionKeys>model.boneCount,"CH4NGE actually drives animated kinematics");manifest<<"kinematic_motion="<<kinematicMotion<<'\n';
    std::ofstream summary(output/L"summary.csv");summary<<"profile,box_margin,native_inertia,linear_spring,angular_spring,joint_damping,rigid_damping,erp,iterations,collide_connected,position_rmse,angle_rmse_deg,max_position,solver_cpu_ms_per_tick\n";
    for(auto& p:profiles){summary<<p.name<<','<<p.margin<<','<<p.nativeInertia<<','<<p.springLinear<<','<<p.springAngular<<','<<p.springDamping<<','<<p.rigidDamping<<','<<p.erp<<','<<p.iterations<<','<<p.collideConnected<<','<<std::sqrt(p.position2/p.count)<<','<<std::sqrt(p.angle2/p.count)<<','<<p.maximum<<','<<p.cpuMs/ticks<<'\n';api.destroy(p.world);}
    std::printf("PASS native275=%d bullet3=%u bodies=%zu joints=%zu ticks=%u\n",btGetVersion(),api.bulletVersion,bodies.size(),joints.size(),ticks);
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
