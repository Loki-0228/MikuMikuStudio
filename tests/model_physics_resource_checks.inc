// Include after physics_import_checks.inc. Uses actual CPU Bullet worlds,
// native ModelDispose, scene-object presence and the resource-version publisher.
#include "../src/plugins/model_physics_resources.hpp"
#include "mikumikustudio/model_physics_resource_owner.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/physics_scale.hpp"
namespace model_physics_resource_checks {
using namespace plugins;
void MatrixIdentity(float* m){for(unsigned i=0;i<16;++i)m[i]=i%5==0?1.f:0.f;}
void Fill(mdl::ModelRecord& m,PhysicsScene& scene,float radius) {
    m.scenePtr=&scene;m.boneCount=2;
    m.boneTable=static_cast<mdl::BoneRecord*>(std::calloc(2,sizeof(mdl::BoneRecord)));
    m.rigidCount=2;m.jointCount=1;
    m.rigidTable=static_cast<mdl::RigidRecord*>(std::calloc(2,sizeof(mdl::RigidRecord)));
    m.jointTable=static_cast<mdl::JointRecord*>(std::calloc(1,sizeof(mdl::JointRecord)));
    Check(m.boneTable&&m.rigidTable&&m.jointTable,"allocate complete privately owned native physics tables");
    for(unsigned i=0;i<2;++i) {
        auto& b=m.boneTable[i];b.parent=b.tailIdx=b.slotIndex=-1;b.type=mdl::BoneType::RotateMove;
        b.rotQuat[3]=b.rotQuat2[3]=b.ikBackup[6]=1;b.position[1]=float(i*3);
        MatrixIdentity(b.matInit);MatrixIdentity(b.matWorld);MatrixIdentity(b.matLocal);MatrixIdentity(b.matExtra);
        b.matInit[13]=b.matWorld[13]=b.matLocal[13]=float(i*3);
        auto& r=m.rigidTable[i];r.boneIndex=int(i);r.mode=1;r.mass=1;r.size[0]=radius;r.position[1]=float(i*3);
        MatrixIdentity(r.invTransform);r.invTransform[13]=-float(i*3);
        r.jpText=owned_text::AllocateBytes(128);std::memset(r.jpText,0x35,128);r.jpText[0]=wchar_t(L'A'+i);r.jpText[1]=0;
        std::memcpy(r.name,"native",6);
        auto* shape=new btSphereShape(radius);btVector3 inertia;shape->calculateLocalInertia(1,inertia);
        auto transform=btTransform::getIdentity();transform.setOrigin(btVector3(0,float(i*3),0));
        auto* motion=new btDefaultMotionState(transform);auto* body=new btRigidBody(1,motion,shape,inertia);
        body->setWorldTransform(transform);body->setInterpolationWorldTransform(transform);body->forceActivationState(DISABLE_DEACTIVATION);
        scene.world->addRigidBody(body,1,-1);r.body=body;
    }
    auto* joint=new btGeneric6DofConstraint(*static_cast<btRigidBody*>(m.rigidTable[0].body),*static_cast<btRigidBody*>(m.rigidTable[1].body),
        btTransform::getIdentity(),btTransform::getIdentity(),true);
    physics_import_replace_checks::SetUid(joint,scene.constraintId++,0);scene.world->addConstraint(joint,false);
    m.jointTable[0].constraint=joint->getUid();m.jointTable[0].rigidA=0;m.jointTable[0].rigidB=1;m.jointTable[0].radiusBound=17;
    m.jointTable[0].jpText=owned_text::AllocateBytes(96);std::memset(m.jointTable[0].jpText,0x63,96);m.jointTable[0].jpText[0]=L'J';m.jointTable[0].jpText[1]=0;
}
struct Fixture {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    history_physics::Broadphase broadphase{btVector3(-100,-100,-100),btVector3(100,100,100),128};
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
    PhysicsScene scene{};
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    mdl::ModelRecord* model=static_cast<mdl::ModelRecord*>(std::calloc(1,sizeof(mdl::ModelRecord)));
    Fixture() {
        ::new(static_cast<void*>(&app->state))MMDAppState{};
        scene.world=&world;scene.broadphase=&broadphase;scene.dispatcher=&dispatcher;scene.solver=&solver;scene.collisionConfig=&config;scene.constraintId=3;
        Fill(*model,scene,.5f);app->Physics()=&scene;app->ModelSlot(0)=raw();
        std::wcscpy(model->path,L"missing-model-resource-source.pmx");
    }
    unsigned char* raw(){return reinterpret_cast<unsigned char*>(model);}
    ~Fixture(){app->ModelSlot(0)=nullptr;ModelDispose(raw());std::free(model);}
};
struct Private {
    std::shared_ptr<history_physics::ImportScene> scene;
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    Private(Fixture& live,float radius):scene(std::make_shared<history_physics::ImportScene>(live.scene)) {
        Fill(*model,scene->Scene(),radius);
        std::memcpy(model->boneTable,live.model->boneTable,2*sizeof(mdl::BoneRecord));
        // This fixture's bones have no owned name/IK allocations to alias.
    }
    ~Private(){model_import::DisposePrivate(reinterpret_cast<unsigned char*>(model.get()));}
};
void Publish(std::unique_ptr<PreparedHistoryChange>& candidate) {
    Check(candidate&&candidate->IsCurrent(),"complete native resource candidate is current before commit");
    bulletAllocations=0;trackBulletAlloc=true;failAfter=0;candidate->Publish();failAfter=-1;trackBulletAlloc=false;
    Check(!bulletAllocations,"world, scale state and model table publication make no Bullet allocation");candidate.reset();
}
void CaptureAndLifetime() {
    const auto texts=owned_text::Count();
    {
        Fixture f;const auto* table=f.model->rigidTable;auto* body=f.model->rigidTable[0].body;
        {
            auto owner=ModelPhysicsResources::Capture(f.raw());auto version=owner->Current();
            Check(owner==ModelPhysicsResources::Capture(f.raw())&&owner->IsCurrent(version),"one resource owner observes the exact current model version");
        }
        Check(!ModelPhysicsResources::Find(f.raw())&&f.model->rigidTable==table&&f.model->rigidTable[0].body==body&&f.world.getNumCollisionObjects()==2,
            "releasing a merely captured owner leaves the model-owned live baseline intact");
    }
    Check(owned_text::Count()==texts,"ordinary ModelDispose retains the original ownership and full-name release behavior");
}
void DirectionsAndDeletion() {
    const auto texts=owned_text::Count();
    {
        Fixture f;auto object=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
        const bone_scale::Matrix scale{{2,0,0,0,2,0,0,0,2}};
        Check(bone_scale::SetPose(f.raw(),0,scale),"seed the live author's scaled physics baseline");physics_scale::Sync(f.raw(),true);
        // The resource owner was captured before scale-derived shapes existed.
        // Base ownership remains stable while the complete scale node grows.
        auto owner=ModelPhysicsResources::Capture(f.raw());auto before=owner->Current();
        std::array<unsigned char,128> fullName{};std::memcpy(fullName.data(),before->Rigids()[0].jpText,fullName.size());
        auto* originalShape=static_cast<btRigidBody*>(before->Rigids()[0].body)->getCollisionShape();
        const auto originalRadius=physics_scale::JointRadiusBaseline(f.raw(),0);
        Private scratch(f,.8f);physics_scale::ResourceState::PreparePrivate(f.raw(),reinterpret_cast<unsigned char*>(scratch.model.get()));
        auto after=owner->AdoptPrivate(*scratch.model,scratch.scene);
        Check(!scratch.model->rigidTable&&!scratch.model->jointTable&&!scratch.model->rigidCount&&!scratch.model->jointCount,
            "private resource adoption transfers tables exactly once without changing live membership");
        auto prepared=owner->PrepareExchange(before,after);uint64_t charge=0;
        Check(prepared->TargetRetainedBytes(charge)&&charge>sizeof(mdl::RigidRecord)*4+sizeof(btRigidBody)*4,
            "candidate charges both complete resource versions and their retained membership bookkeeping");
        auto stalePresence=object->PreparePresence(false);
        f.model->boneTable[0].trans[0]=123;
        Publish(prepared);
        Check(owner->IsCurrent(after)&&!owner->IsCurrent(before)&&!stalePresence->IsCurrent()&&f.model->boneTable[0].trans[0]==123,
            "resource publication preserves unregistered pose and invalidates an older object-presence candidate");stalePresence.reset();
        auto* editedShape=static_cast<btRigidBody*>(after->Rigids()[0].body)->getCollisionShape();
        for(unsigned iteration=0;iteration<12;++iteration) {
            prepared=owner->PrepareExchange(after,before);Publish(prepared);
            Check(owner->IsCurrent(before)&&f.model->rigidTable==before->Rigids()&&static_cast<btRigidBody*>(f.model->rigidTable[0].body)->getCollisionShape()==originalShape&&
                physics_scale::JointRadiusBaseline(f.raw(),0)==originalRadius&&!std::memcmp(f.model->rigidTable[0].jpText,fullName.data(),fullName.size()),
                "undo restores exact tables, complete raw name capacity, affine shape and joint baseline without file reads");
            prepared=owner->PrepareExchange(before,after);Publish(prepared);
            Check(owner->IsCurrent(after)&&static_cast<btRigidBody*>(f.model->rigidTable[0].body)->getCollisionShape()==editedShape&&f.world.getNumCollisionObjects()==2,
                "redo restores the retained edited native resources without rebuilding shapes or joints");
        }
        prepared=object->PreparePresence(false);Publish(prepared);
        Check(!object->Present()&&!f.app->ModelSlot(0)&&!f.world.getNumCollisionObjects()&&!f.world.getNumConstraints(),
            "same-object deletion uses the edited resource version rather than the initially captured tables");
        prepared=object->PreparePresence(true);Publish(prepared);
        Check(object->Present()&&f.app->ModelSlot(0)==f.raw()&&f.model->rigidTable==after->Rigids()&&f.world.getNumCollisionObjects()==2,
            "same-object restoration restores the current edited version and stable model identity");
        object.reset();owner.reset();before.reset();after.reset();
        Check(ModelPhysicsResources::Find(f.raw())&&f.world.getNumCollisionObjects()==2,
            "the model's adopted sidecar keeps its current resources after temporary plans and holders disappear");
    }
    Check(owned_text::Count()==texts,"disposing an edited model releases every old/current full-name allocation once");
}
void FailureAndStale() {
    Fixture f;auto owner=ModelPhysicsResources::Capture(f.raw());auto before=owner->Current();
    Private scratch(f,.9f);auto after=owner->AdoptPrivate(*scratch.model,scratch.scene);
    const auto* table=f.model->rigidTable;auto* cache=f.broadphase.Cache();
    bool reached=false;unsigned failures=0;
    for(long limit=0;limit<2048&&!reached;++limit) {
        std::unique_ptr<PreparedHistoryChange> candidate;failAfter=limit;
        try{candidate=owner->PrepareExchange(before,after);reached=true;}catch(const std::bad_alloc&){++failures;}
        failAfter=-1;
        Check(owner->IsCurrent(before)&&f.model->rigidTable==table&&f.broadphase.Cache()==cache&&f.world.getNumCollisionObjects()==2&&scratch.scene->Scene().world->getNumCollisionObjects()==2,
            "every resource-version allocation failure preserves live/private worlds, tables and exact source ownership");
    }
    Check(reached&&failures>20,"resource-version preparation fault sweep reaches complete scale/world/ownership and budget stages");
    auto candidate=owner->PrepareExchange(before,after);auto* body=static_cast<btRigidBody*>(before->Rigids()[0].body);
    body->setLinearVelocity(btVector3(1,2,3));Check(!candidate->IsCurrent()&&owner->IsCurrent(before)&&f.model->rigidTable==table,
        "a changed actual native body rejects the complete version candidate before any publication");candidate.reset();body->setLinearVelocity(btVector3(0,0,0));
    auto replacement=ModelPhysicsResources::Capture(f.raw());Check(replacement==owner,"failed candidates retain the same reusable live ownership");
    Private other(f,1.1f);auto alternative=owner->AdoptPrivate(*other.model,other.scene);
    candidate=owner->PrepareExchange(before,alternative);Publish(candidate);
    Check(!owner->IsCurrent(before)&&owner->IsCurrent(alternative),"a fresh alternate private version remains publishable after cancellation and stale failure");
}
}
void ModelPhysicsResourceChecks() {
    btAlignedAllocSetCustom(HistoryAlloc,HistoryFree);btAlignedAllocSetCustomAligned(HistoryAlignedAlloc,HistoryAlignedFree);
    model_physics_resource_checks::CaptureAndLifetime();model_physics_resource_checks::DirectionsAndDeletion();model_physics_resource_checks::FailureAndStale();
    btAlignedAllocSetCustomAligned(nullptr,nullptr);btAlignedAllocSetCustom(nullptr,nullptr);
}
