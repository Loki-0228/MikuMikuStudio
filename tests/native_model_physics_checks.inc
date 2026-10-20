// Include after ObjectCommandFixture and the CPU physics/retention fixtures.
#include "../src/plugins/native_model_physics.hpp"
#include "../src/plugins/model_physics_resources.hpp"
#include <limits>
namespace model_physics_checks {
using namespace plugins;
struct World {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    history_physics::Broadphase broadphase{btVector3(-100,-100,-100),btVector3(100,100,100),256};
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
    PhysicsScene scene{};
    World() { scene.world=&world;scene.broadphase=&broadphase;scene.dispatcher=&dispatcher;scene.solver=&solver;scene.collisionConfig=&config; }
};
wchar_t* Text(const std::vector<wchar_t>& value) {
    auto* out=static_cast<wchar_t*>(std::malloc(value.size()*sizeof(wchar_t)));
    Check(out!=nullptr,"allocate model physics UTF16 text");
    std::memcpy(out,value.data(),value.size()*sizeof(wchar_t));owned_text::Remember(out,value.size()*sizeof(wchar_t));return out;
}
struct Fixture : ObjectCommandFixture {
    World physics;
    Fixture() {
        app->HInstance()=GetModuleHandleW(nullptr);app->Physics()=&physics.scene;
        for(unsigned slot=0;slot<2;++slot) {
            auto& m=*mdl::Mdl(app->ModelSlot(slot));m.scenePtr=&physics.scene;
            m.boneCount=2;m.boneTable=static_cast<mdl::BoneRecord*>(std::calloc(2,sizeof(mdl::BoneRecord)));
            m.rigidTable=static_cast<mdl::RigidRecord*>(std::calloc(2,sizeof(mdl::RigidRecord)));m.rigidCount=2;
            m.jointTable=static_cast<mdl::JointRecord*>(std::calloc(1,sizeof(mdl::JointRecord)));m.jointCount=1;
            Check(m.boneTable&&m.rigidTable&&m.jointTable,"allocate complete native physics tables");
            for(unsigned i=0;i<2;++i) {
                auto& b=m.boneTable[i];b.parent=-1;b.rotQuat[3]=1;b.trans[0]=float(7+slot+i);b.hasRigidBody=i?0:1;
                for(unsigned k:{0u,5u,10u,15u})b.matInit[k]=1;
                auto& r=m.rigidTable[i];std::strcpy(r.name,i?"body-static":"body-dynamic");r.boneIndex=int(i);
                r.jpText=Text({L'物',L'理',wchar_t(L'A'+i),0,L'尾',L'部',0});r.enText=Text({L'B',wchar_t(L'0'+i),0});
                r.group=1;r.noCollapse=0xffff;r.shape=0;r.size[0]=r.size[1]=r.size[2]=.5f;
                r.mass=1;r.mode=i?0:1;r.staticFlag=i?1:0;r.position[1]=float(3+i);r.linearDamping=.1234567f;r.friction=.5f;
                float matrix[16]{};for(unsigned k:{0u,5u,10u,15u})matrix[k]=1;matrix[12]=float(slot*10);matrix[13]=float(3+i);
                void* out[2]{};CreateRigidBody(&physics.scene,out,r.shape,r.size[0],r.size[1],r.size[2],matrix,r.mode,r.mass,
                    r.linearDamping,r.angularDamping,r.restitution,r.friction,char(r.group),r.noCollapse);
                Check(out[1]!=nullptr,"construct real CPU source body");r.keyData=out[0];r.body=out[1];
                for(unsigned k:{0u,5u,10u,15u})r.invTransform[k]=1;
            }
            auto& j=m.jointTable[0];std::strcpy(j.name,"joint-source");j.rigidA=0;j.rigidB=1;
            j.jpText=Text({L'関',L'節',0,L'余',0});
            j.constraint=CreatePhysJoint(&physics.scene,m.rigidTable[0].body,m.rigidTable[1].body,
                0,0,0,0,0,0,1,0,0,0,0,0,0,1,
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);
        }
        app->PhysicsResetPending()=0;app->SceneModified()=0;app->EnhancedModelDirty()=0;
    }
    ~Fixture() {
        Check(history.Reset()==MMS_OK,"release physics journal before its world");history.Collect();
        for(unsigned slot=0;slot<2;++slot)if(auto* raw=app->ModelSlot(slot)){DeleteModel(raw,1);app->ModelSlot(slot)=nullptr;}
        app->Physics()=nullptr;
        Check(physics.world.getNumCollisionObjects()==0&&physics.world.getNumConstraints()==0,"model disposal releases every version exactly once before world teardown");
    }
    ModelPhysicsDraft Read(unsigned slot=0) {
        NativeModelPhysicsPlan plan;ModelPhysicsDraft value;
        Check(plan.Begin(*app,slot)==MMS_OK&&plan.Read(value)==MMS_OK,"capture full native model physics draft");return value;
    }
    std::shared_ptr<HistoryChange> Change(const ModelPhysicsDraft& draft,unsigned slot=0) {
        NativeModelPhysicsPlan plan;std::shared_ptr<HistoryChange> out;
        Check(plan.Begin(*app,slot)==MMS_OK&&plan.Set(draft)==MMS_OK&&plan.Take(out)==MMS_OK,"construct private replacement before common-history admission");return out;
    }
    bool Stable() const {
        return Unmoved()&&!app->PhysicsResetPending()&&mdl::Mdl(app->ModelSlot(0))->boneTable[0].trans[0]==7.f;
    }
};
void Publish(Fixture& f,const std::shared_ptr<HistoryChange>& change) {
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("model physics test",change,token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"prepare complete model resource version and history node");
    bulletAllocations=0;trackBulletAlloc=true;failAfter=0;
    const auto result=f.history.CommitNative(std::move(token));failAfter=-1;trackBulletAlloc=false;
    Check(result==MMS_OK&&!bulletAllocations,"model physics formal publication allocates neither C++ nor Bullet storage");
}
void RoundTripAndNoop() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"model physics recording mode");
        auto before=f.Read(),after=before;auto* raw=f.app->ModelSlot(0);auto& m=*mdl::Mdl(raw);
        auto* oldTable=m.rigidTable;auto* oldBody=oldTable[0].body;auto* peer=static_cast<btRigidBody*>(mdl::Mdl(f.app->ModelSlot(1))->rigidTable[0].body);
        const auto peerPose=peer->getWorldTransform();const auto id=SceneObjectIdentity(raw).id;
        after.rigids[0].mass=2.25f;after.rigids[0].size[0]=.75f;after.joints[0].springs[2]=7.5f;
        after.rigidNames[0][0]={L'新',L'物',L'理',0,L'保',L'留',0};
        {NativeModelPhysicsPlan cancelled;Check(cancelled.Begin(*f.app,0)==MMS_OK&&cancelled.Set(after)==MMS_OK&&m.rigidTable==oldTable&&
            m.rigidTable[0].mass==1&&!f.history.UndoCount()&&f.Stable(),"discarding staged physics changes leaves both native worlds and unregistered pose untouched");}
        auto change=f.Change(after);Check(m.rigidTable==oldTable&&f.physics.world.getNumCollisionObjects()==4,"building private physics adds no live member before commit");
        Publish(f,change);auto* newTable=m.rigidTable;auto* newBody=newTable[0].body;
        Check(newTable!=oldTable&&newBody!=oldBody&&m.rigidTable[0].mass==2.25f&&f.history.UndoCount()==1&&f.Stable(),
            "one explicit physics version replaces tables and native bodies in either history mode");
        Check(peer->getWorldTransform()==peerPose&&f.physics.world.getNumCollisionObjects()==4&&f.physics.world.getNumConstraints()==2,
            "peer model pose and world membership stay intact");
        m.boneTable[0].trans[2]=.375f;m.boneTable[0].matInit[12]=.375f;
        Check(f.history.History(false)==MMS_OK&&m.rigidTable==oldTable&&m.rigidTable[0].body==oldBody&&f.Stable(),"physics undo restores the original allocations without reading a model file");
        Check(m.boneTable[0].trans[2]==.375f&&m.boneTable[0].matInit[12]==.375f,
            "physics replay accepts the current evaluated bone matrix and preserves a later unregistered editing pose");
        Check(!f.Change(before)&&f.history.RedoCount()==1,"unchanged physics confirmation preserves redo and original float bits");
        for(unsigned n=0;n<4;++n) {
            Check(f.history.History(true)==MMS_OK&&m.rigidTable==newTable&&m.rigidTable[0].body==newBody&&f.Stable(),"physics redo reuses the retained replacement resources");
            Check(f.history.History(false)==MMS_OK&&m.rigidTable==oldTable&&f.Stable(),"repeated physics undo retains original resources and unregistered pose");
        }
        Check(f.history.History(true)==MMS_OK&&SceneObjectPresent(raw,id)&&!f.history.LastPublicationRequiresPhysicsReset(),"physics resource edits preserve the model identity and do not reset peer physics");
        owned_text::Allocation text;
        Check(owned_text::Find(m.rigidTable[0].jpText,text)&&text.bytes==after.rigidNames[0][0].size()*sizeof(wchar_t)&&
            !std::memcmp(m.rigidTable[0].jpText,after.rigidNames[0][0].data(),text.bytes),"replacement retains complete Unicode allocation including bytes after NUL");
    }
}
void InvalidBudgetAndStale() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);auto before=f.Read();auto& m=*mdl::Mdl(f.app->ModelSlot(0));
    NativeModelPhysicsPlan plan;Check(plan.Begin(*f.app,0)==MMS_OK,"begin physics validation plan");auto invalid=before;
    invalid.joints[0].rigidB=99;Check(plan.Set(invalid)==MMS_E_ARGUMENT&&m.rigidTable[0].mass==1,"invalid compact joint endpoints fail without publishing old removal");
    invalid=before;invalid.rigids[0].mass=std::numeric_limits<float>::quiet_NaN();Check(plan.Set(invalid)==MMS_E_ARGUMENT,"nonfinite physics draft is rejected");
    invalid=before;invalid.rigidNames[0][0]={L'x'};Check(plan.Set(invalid)==MMS_E_ARGUMENT,"unterminated draft Unicode allocation is rejected");
    auto after=before;after.rigids[0].mass=3;auto change=f.Change(after);auto* old=m.rigidTable;
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,1)==MMS_OK&&
        f.history.PrepareNative("physics budget",change,token)==MMS_E_MEMORY&&!token&&m.rigidTable==old&&f.physics.world.getNumCollisionObjects()==4,
        "complete resource budget refusal leaves old resources and live capacity unchanged");
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,16*1024*1024)==MMS_OK&&
        f.history.PrepareNative("physics stale",change,token)==MMS_OK,"prepare reusable private replacement after budget refusal");
    const auto original=m.rigidTable[0].mass;m.rigidTable[0].mass=4;
    Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&m.rigidTable==old&&!f.history.UndoCount(),"authored source mutation invalidates the whole candidate");
    m.rigidTable[0].mass=original;
    Publish(f,change);
    auto removal=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0)->CaptureRemoval(1024*1024);
    std::unique_ptr<SharedHistory::NativeTransaction> deletion;
    Check(f.history.PrepareNative("remove edited model",removal,deletion)==MMS_OK&&f.history.CommitNative(std::move(deletion))==MMS_OK&&
        !f.app->ModelSlot(0)&&f.physics.world.getNumCollisionObjects()==2,"model deletion parks the current replacement version");
    Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)&&mdl::Mdl(f.app->ModelSlot(0))->rigidTable[0].mass==3,
        "object restoration follows the current physics version instead of fixed initial tables");
    Check(f.history.History(false)==MMS_OK&&mdl::Mdl(f.app->ModelSlot(0))->rigidTable==old,"older physics history remains valid across object removal and restoration");
}
void UnboundLifetime() {
    Fixture f;auto before=f.Read(),after=before;after.rigids[0].friction=.85f;
    Check(CommitUnboundModelPhysics(*f.app,f.Change(after))==MMS_OK&&!f.history.UndoCount()&&f.app->SceneModified()&&
        mdl::Mdl(f.app->ModelSlot(0))->rigidTable[0].friction==.85f&&f.Stable(),"ordinary full-profile physics uses the same private publication without a bound journal");
    after=f.Read();after.rigids[0].friction=.65f;
    Check(CommitUnboundModelPhysics(*f.app,f.Change(after))==MMS_OK&&mdl::Mdl(f.app->ModelSlot(0))->rigidTable[0].friction==.65f,
        "a second ordinary edit never falls through to legacy immediate-free resources");
}
void DisabledSourceJoint() {
    Fixture f;auto& m=*mdl::Mdl(f.app->ModelSlot(0));
    auto* replacement=static_cast<mdl::JointRecord*>(std::calloc(2,sizeof(mdl::JointRecord)));
    Check(replacement!=nullptr,"allocate retained disabled-joint source record");replacement[0]=m.jointTable[0];
    replacement[1].rigidA=-1;replacement[1].rigidB=INT_MAX;replacement[1].constraint=-1;
    std::memcpy(replacement[1].name,"disabled",9);replacement[1].jpText=Text({L'未',L'接',L'続',0,L'尾',0});
    std::free(m.jointTable);m.jointTable=replacement;m.jointCount=2;
    NativePoseTestBinding binding(*f.app,f.history);auto before=f.Read(),after=before;after.rigids[0].mass=2;
    Check(before.joints[0].constraint==0&&before.joints[1].constraint==-1,"draft distinguishes native joint UIDs from preserved disabled source records");
    Publish(f,f.Change(after));
    Check(m.jointCount==2&&m.jointTable[1].constraint==-1&&m.jointTable[1].rigidA==-1&&m.jointTable[1].rigidB==INT_MAX&&
        f.physics.world.getNumConstraints()==2&&std::wcscmp(m.jointTable[1].jpText,L"未接続")==0,
        "editing a rigid body preserves disabled source endpoints and Unicode instead of rejecting or silently deleting the joint");
    Check(f.history.History(false)==MMS_OK&&m.jointTable==replacement&&m.jointCount==2,"undo restores the exact table containing the original disabled source joint");
}
}
void NativeModelPhysicsChecks() {
    btAlignedAllocSetCustom(HistoryAlloc,HistoryFree);btAlignedAllocSetCustomAligned(HistoryAlignedAlloc,HistoryAlignedFree);
    model_physics_checks::RoundTripAndNoop();model_physics_checks::InvalidBudgetAndStale();model_physics_checks::UnboundLifetime();model_physics_checks::DisabledSourceJoint();
    btAlignedAllocSetCustomAligned(nullptr,nullptr);btAlignedAllocSetCustom(nullptr,nullptr);
}
