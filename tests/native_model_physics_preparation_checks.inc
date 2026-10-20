// Include after native_model_physics_checks.inc. Faults affect C++ new only;
// native malloc and Bullet aligned allocations retain their normal behavior.
#include "mikumikustudio/affine_collision_shape.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>

namespace model_physics_preparation_checks {
using namespace plugins;
using model_physics_checks::Fixture;

bool Near(btScalar a, btScalar b) {
    return btFabs(a-b) <= btScalar(.002) * (std::max)(btScalar(1),btFabs(b));
}
void Identity(float* out) {
    std::fill(out,out+16,0.f);out[0]=out[5]=out[10]=out[15]=1.f;
}
bone_scale::Matrix Scale(unsigned slot,unsigned bone) {
    if(slot)return bone?bone_scale::Matrix{{.9f,0,0,0,1.2f,0,0,0,1.4f}}:
        bone_scale::Matrix{{1.3f,0,0,0,.85f,0,0,0,1.1f}};
    return bone?bone_scale::Matrix{{1.1f,0,0,0,1.75f,0,0,0,.9f}}:
        bone_scale::Matrix{{2.f,0,0,0,1.25f,0,0,0,.75f}};
}
btGeneric6DofConstraint* Joint(Fixture& f,unsigned slot) {
    const auto id=mdl::Mdl(f.app->ModelSlot(slot))->jointTable[0].constraint;
    for(int i=0;i<f.physics.world.getNumConstraints();++i) {
        auto* joint=f.physics.world.getConstraint(i);
        if(joint->getUid()==id)return dynamic_cast<btGeneric6DofConstraint*>(joint);
    }
    return nullptr;
}
void Seed(Fixture& f) {
    for(unsigned slot=0;slot<2;++slot) {
        auto* raw=f.app->ModelSlot(slot);auto& m=*mdl::Mdl(raw);
        // The existing fixture retains deliberately unregistered trans[0].
        // Give its physics geometry a separate, well formed rigid baseline.
        for(unsigned i=0;i<2;++i) {
            auto& b=m.boneTable[i];b.position[0]=float(slot)*.5f;b.position[1]=float(i*2);
            Identity(b.matInit);b.matInit[12]=b.position[0];b.matInit[13]=b.position[1];
            std::memcpy(b.matWorld,b.matInit,sizeof(b.matInit));
            Identity(b.matLocal);Identity(b.matExtra);
            Check(bone_scale::SetPose(raw,int(i),Scale(slot,i)),"seed independent nonuniform source and peer bone scales");
        }
        auto* joint=Joint(f,slot);Check(joint!=nullptr,"find real source six-axis constraint for scale preparation");
        joint->getFrameOffsetA().setOrigin(btVector3(.25f,.5f,.75f));
        joint->getFrameOffsetB().setOrigin(btVector3(-.5f,.25f,-.125f));
        joint->setLinearLowerLimit(btVector3(-.5f,-.75f,-1.f));
        joint->setLinearUpperLimit(btVector3(.5f,.75f,1.f));m.jointTable[0].radiusBound=17.f;
        physics_scale::Sync(raw,true);
        std::wcscpy(m.path,L"Z:\\MMS-missing-physics-preparation-source\\offline.pmx");
    }
    f.physics.world.performDiscreteCollisionDetection();
    bool algorithm=false;const auto& pairs=f.physics.broadphase.Cache()->getOverlappingPairArray();
    for(int i=0;i<pairs.size();++i)algorithm=algorithm||pairs[i].m_algorithm!=nullptr;
    Check(algorithm,"seed a real cross-model contact algorithm before testing preparation cancellation");
    Check(f.Stable(),"nonuniform physics fixture keeps current frame and unregistered bone pose");
}
struct TextState {
    const wchar_t* pointer=nullptr;
    owned_text::Allocation allocation{};
    std::vector<unsigned char> bytes;
    explicit TextState(const wchar_t* p):pointer(p) {
        if(!p)return;
        Check(owned_text::Find(p,allocation),"capture full physics text allocation rather than its visible prefix");
        bytes.resize(allocation.bytes);std::memcpy(bytes.data(),p,bytes.size());
    }
    bool Same(const wchar_t* p)const {
        if(p!=pointer)return false;if(!p)return true;
        owned_text::Allocation now{};
        return owned_text::Find(p,now)&&now.bytes==allocation.bytes&&now.registryIdentity==allocation.registryIdentity&&
            !std::memcmp(bytes.data(),p,bytes.size());
    }
};
struct BodyState {
    btRigidBody* body;
    const btCollisionShape* shape;
    const btCollisionShape* base;
    btMotionState* motion;
    btBroadphaseProxy* proxy;
    btTransform world,interpolation,motionWorld;
    btVector3 linear,angular,inertia;
    std::array<btVector3,3> support;
    btScalar inverseMass;
    int activation,flags;
    short group,mask;
    explicit BodyState(btRigidBody* value):body(value),shape(body->getCollisionShape()),base(physics_scale::BaseShape(body)),
        motion(body->getMotionState()),proxy(body->getBroadphaseHandle()),world(body->getWorldTransform()),
        interpolation(body->getInterpolationWorldTransform()),linear(body->getLinearVelocity()),angular(body->getAngularVelocity()),
        inertia(body->getInvInertiaDiagLocal()),inverseMass(body->getInvMass()),activation(body->getActivationState()),
        flags(body->getCollisionFlags()),group(proxy->m_collisionFilterGroup),mask(proxy->m_collisionFilterMask) {
        motionWorld=btTransform::getIdentity();if(motion)motion->getWorldTransform(motionWorld);
        const auto* convex=dynamic_cast<const btConvexShape*>(shape);Check(convex!=nullptr,"scaled fixture has convex shape support");
        for(int axis=0;axis<3;++axis){btVector3 direction(0,0,0);direction[axis]=1;support[axis]=convex->localGetSupportingVertex(direction);}
    }
    bool Same()const {
        if(body->getCollisionShape()!=shape||physics_scale::BaseShape(body)!=base||body->getMotionState()!=motion||
            body->getBroadphaseHandle()!=proxy||!(body->getWorldTransform()==world)||!(body->getInterpolationWorldTransform()==interpolation)||
            body->getLinearVelocity()!=linear||body->getAngularVelocity()!=angular||body->getInvInertiaDiagLocal()!=inertia||
            body->getInvMass()!=inverseMass||body->getActivationState()!=activation||body->getCollisionFlags()!=flags||
            proxy->m_collisionFilterGroup!=group||proxy->m_collisionFilterMask!=mask)return false;
        btTransform now=btTransform::getIdentity();if(motion)motion->getWorldTransform(now);if(!(now==motionWorld))return false;
        const auto* convex=static_cast<const btConvexShape*>(shape);
        for(int axis=0;axis<3;++axis){btVector3 direction(0,0,0);direction[axis]=1;if(convex->localGetSupportingVertex(direction)!=support[axis])return false;}
        return true;
    }
};
struct ModelState {
    unsigned char* raw;
    mdl::RigidRecord* rigidTable;
    mdl::JointRecord* jointTable;
    mdl::BoneRecord* boneTable;
    std::vector<mdl::RigidRecord> rigids;
    std::vector<mdl::JointRecord> joints;
    std::vector<mdl::BoneRecord> bones;
    std::vector<TextState> texts;
    std::vector<BodyState> bodies;
    bone_scale::Values pose;
    std::uint64_t poseRevision;
    btGeneric6DofConstraint* joint;
    btTransform frameA,frameB,baseA,baseB;
    btVector3 lower,upper;
    float radius;
    explicit ModelState(Fixture& f,unsigned slot):raw(f.app->ModelSlot(slot)),rigidTable(mdl::Mdl(raw)->rigidTable),
        jointTable(mdl::Mdl(raw)->jointTable),boneTable(mdl::Mdl(raw)->boneTable),
        rigids(rigidTable,rigidTable+mdl::Mdl(raw)->rigidCount),joints(jointTable,jointTable+mdl::Mdl(raw)->jointCount),
        bones(boneTable,boneTable+mdl::Mdl(raw)->boneCount),pose(bone_scale::PoseValues(raw)),poseRevision(bone_scale::Revision(raw)),
        joint(Joint(f,slot)),frameA(joint->getFrameOffsetA()),frameB(joint->getFrameOffsetB()),
        lower(joint->getTranslationalLimitMotor()->m_lowerLimit),upper(joint->getTranslationalLimitMotor()->m_upperLimit),
        radius(physics_scale::JointRadiusBaseline(raw,0)) {
        physics_scale::JointBaseline(*joint,baseA,baseB);
        for(const auto& r:rigids){texts.emplace_back(r.jpText);texts.emplace_back(r.enText);bodies.emplace_back(static_cast<btRigidBody*>(r.body));}
        for(const auto& j:joints){texts.emplace_back(j.jpText);texts.emplace_back(j.enText);}
    }
    bool Same()const {
        const auto& m=*mdl::Mdl(raw);
        if(m.rigidTable!=rigidTable||m.jointTable!=jointTable||m.boneTable!=boneTable||m.rigidCount!=rigids.size()||
            m.jointCount!=joints.size()||m.boneCount!=bones.size()||!bone_scale::PoseValuesEqual(raw,pose)||
            bone_scale::Revision(raw)!=poseRevision||std::memcmp(rigidTable,rigids.data(),rigids.size()*sizeof(rigids[0]))||
            std::memcmp(jointTable,joints.data(),joints.size()*sizeof(joints[0]))||std::memcmp(boneTable,bones.data(),bones.size()*sizeof(bones[0])))return false;
        for(const auto& body:bodies)if(!body.Same())return false;
        std::size_t at=0;
        for(const auto& r:rigids)if(!texts[at++].Same(r.jpText)||!texts[at++].Same(r.enText))return false;
        for(const auto& j:joints)if(!texts[at++].Same(j.jpText)||!texts[at++].Same(j.enText))return false;
        btTransform a,b;physics_scale::JointBaseline(*joint,a,b);
        return joint->getFrameOffsetA()==frameA&&joint->getFrameOffsetB()==frameB&&a==baseA&&b==baseB&&
            joint->getTranslationalLimitMotor()->m_lowerLimit==lower&&joint->getTranslationalLimitMotor()->m_upperLimit==upper&&
            physics_scale::JointRadiusBaseline(raw,0)==radius;
    }
};
struct WorldState {
    struct Array {const void* data;int size,capacity;};
    Array objects,moving,constraints,pairs;
    const history_physics::PairCache* cache;
    std::vector<btCollisionObject*> objectValues;
    std::vector<btRigidBody*> movingValues;
    std::vector<btTypedConstraint*> constraintValues;
    std::vector<btBroadphasePair> pairValues;
    int manifolds,constraintId;
    template<class T> static Array Read(const btAlignedObjectArray<T>& a){return {a.size()?&a[0]:nullptr,a.size(),a.capacity()};}
    template<class T> static std::vector<T> Copy(const btAlignedObjectArray<T>& a){return a.size()?std::vector<T>(&a[0],&a[0]+a.size()):std::vector<T>{};}
    template<class T> static bool SameArray(const Array& before,const btAlignedObjectArray<T>& a,const std::vector<T>& values){
        const auto now=Read(a);if(before.data!=now.data||before.size!=now.size||before.capacity!=now.capacity)return false;
        for(int i=0;i<a.size();++i)if(a[i]!=values[std::size_t(i)])return false;return true;
    }
    explicit WorldState(Fixture& f):objects(Read(f.physics.world.getCollisionObjectArray())),moving(Read(f.physics.world.HistoryBodies())),
        constraints(Read(f.physics.world.HistoryConstraints())),pairs(Read(f.physics.broadphase.Cache()->getOverlappingPairArray())),
        cache(f.physics.broadphase.Cache()),objectValues(Copy(f.physics.world.getCollisionObjectArray())),
        movingValues(Copy(f.physics.world.HistoryBodies())),constraintValues(Copy(f.physics.world.HistoryConstraints())),
        pairValues(Copy(f.physics.broadphase.Cache()->getOverlappingPairArray())),manifolds(f.physics.dispatcher.getNumManifolds()),constraintId(f.physics.scene.constraintId){}
    bool Same(Fixture& f)const {
        if(cache!=f.physics.broadphase.Cache()||manifolds!=f.physics.dispatcher.getNumManifolds()||constraintId!=f.physics.scene.constraintId||
            !SameArray(objects,f.physics.world.getCollisionObjectArray(),objectValues)||!SameArray(moving,f.physics.world.HistoryBodies(),movingValues)||
            !SameArray(constraints,f.physics.world.HistoryConstraints(),constraintValues))return false;
        const auto& now=f.physics.broadphase.Cache()->getOverlappingPairArray();const auto storage=Read(now);
        if(pairs.data!=storage.data||pairs.size!=storage.size||pairs.capacity!=storage.capacity)return false;
        for(int i=0;i<now.size();++i){const auto& p=now[i];const auto& old=pairValues[std::size_t(i)];
            if(p.m_pProxy0!=old.m_pProxy0||p.m_pProxy1!=old.m_pProxy1||p.m_algorithm!=old.m_algorithm||p.m_internalInfo1!=old.m_internalInfo1)return false;}
        return true;
    }
};
ModelPhysicsDraft Edited(Fixture& f) {
    auto after=f.Read();after.rigids[0].mass=2.75f;after.rigids[0].size[0]=.8f;
    after.rigids[1].shape=1;after.rigids[1].size[0]=.35f;after.rigids[1].size[1]=.65f;after.rigids[1].size[2]=.45f;
    after.joints[0].position[0]=.4f;after.joints[0].position[1]=.2f;after.joints[0].limits[0]=-.5f;after.joints[0].limits[3]=.75f;
    after.rigidNames[0][0]={L'新',L'形',L'状',0,L'全',L'配',L'列',0};return after;
}
void Scaled(Fixture& f) {
    const auto& m=*mdl::Mdl(f.app->ModelSlot(0));
    for(unsigned i=0;i<2;++i) {
        auto* body=static_cast<btRigidBody*>(m.rigidTable[i].body);const auto* shape=dynamic_cast<const AffineConvexShape*>(body->getCollisionShape());
        Check(shape&&physics_scale::BaseShape(body)!=shape,"private preparation preserves nonuniform affine wrappers for both physics modes");
        for(int axis=0;axis<3;++axis){btVector3 direction(0,0,0);direction[axis]=1;
            Check(Near(shape->localGetSupportingVertex(direction)[axis],shape->BaseShape().localGetSupportingVertex(direction)[axis]*Scale(0,i).v[axis*3+axis]),
                "collision support applies the authored nonuniform scale exactly once");}
    }
    auto* joint=Joint(f,0);btTransform a,b;physics_scale::JointBaseline(*joint,a,b);
    for(int axis=0;axis<3;++axis) {
        Check(Near(joint->getFrameOffsetA().getOrigin()[axis],a.getOrigin()[axis]*Scale(0,0).v[axis*3+axis])&&
            Near(joint->getFrameOffsetB().getOrigin()[axis],b.getOrigin()[axis]*Scale(0,1).v[axis*3+axis]),
            "joint frames use their own body's nonuniform geometry rather than a repeated scale");
    }
}
enum class Stage {Begin,Set,Take};
void AllocationFailures(Stage stage) {
    const auto initialTexts=owned_text::Count();
    {
        Fixture f;Seed(f);NativePoseTestBinding binding(*f.app,f.history);
        const auto after=Edited(f);const ModelState source(f,0),peer(f,1);const WorldState world(f);
        const auto texts=owned_text::Count();std::uint64_t revision=0;
        Check(f.history.Revision(revision)==MMS_OK,"capture physics preparation history revision");
        unsigned failures=0;bool reached=false;long completedAt=-1;
        for(long limit=0;limit<4096&&!reached;++limit) {
            {
                NativeModelPhysicsPlan plan;std::shared_ptr<HistoryChange> result;
                if(stage!=Stage::Begin)Check(plan.Begin(*f.app,0)==MMS_OK,"prepare plan before isolated allocation sweep");
                if(stage==Stage::Take)Check(plan.Set(after)==MMS_OK,"stage full physics draft before private-build allocation sweep");
                failAfter=limit;
                const auto status=stage==Stage::Begin?plan.Begin(*f.app,0):stage==Stage::Set?plan.Set(after):plan.Take(result);
                failAfter=-1;
                Check(status==MMS_OK||status==MMS_E_MEMORY,"allocation sweep reports only success or a bounded allocation failure");
                if(status==MMS_E_MEMORY) {
                    ++failures;Check(!result&&owned_text::Count()==texts,"failed private preparation releases every new text allocation before returning");
                    Check(source.Same()&&peer.Same()&&world.Same(f),"partial preparation failure preserves exact live/peer resources and world backing storage");
                    if(stage==Stage::Begin)Check(plan.Begin(*f.app,0)==MMS_OK,"same plan can retry a failed Begin");
                    Check(plan.IsCurrent(),"failed Set or Take leaves its original plan current and reusable");
                    if(stage!=Stage::Take)Check(plan.Set(after)==MMS_OK,"same plan can retry its unchanged staged input after allocation failure");
                    Check(plan.Take(result)==MMS_OK&&result,"same plan can retry private scale preparation after partial cleanup");
                } else {
                    reached=true;completedAt=limit;
                    if(stage!=Stage::Take){Check(plan.Set(after)==MMS_OK,"complete successful preparation sweep input");Check(plan.Take(result)==MMS_OK&&result,"finish successful preparation sweep without publication");}
                }
                Check(source.Same()&&peer.Same()&&world.Same(f),"complete private replacement still cannot alter the live source or peer");
            }
            std::uint64_t now=0;
            Check(owned_text::Count()==texts&&source.Same()&&peer.Same()&&world.Same(f),"discarding a retry releases its private tables, shapes and scale indexes once");
            Check(f.history.Revision(now)==MMS_OK&&now==revision&&!f.history.UndoCount()&&!f.history.RedoCount()&&
                !f.app->SceneModified()&&!f.app->EnhancedModelDirty()&&f.Stable(),"failed and discarded preparation never writes scene data, history, playback or save reminders");
        }
        Check(reached&&failures>(stage==Stage::Begin?10u:stage==Stage::Set?5u:20u),"fault sweep reaches all C++ allocations through its first complete successful path");
        std::fprintf(stderr,"MODEL_PHYSICS_PREPARATION stage=%s failures=%u firstSuccess=%ld\n",stage==Stage::Begin?"begin":stage==Stage::Set?"set":"take",failures,completedAt);
    }
    Check(owned_text::Count()==initialTexts,"preparation sweep teardown leaves no complete-name registrations behind");
}
void ScaledOfflineReplay() {
    const auto initialTexts=owned_text::Count();
    {
        Fixture f;Seed(f);NativePoseTestBinding binding(*f.app,f.history);const ModelState before(f,0),peer(f,1);
        Scaled(f);const auto after=Edited(f);auto change=f.Change(after);model_physics_checks::Publish(f,change);
        const auto& m=*mdl::Mdl(f.app->ModelSlot(0));
        Check(m.rigidTable!=before.rigidTable&&m.jointTable!=before.jointTable&&m.rigidTable[0].mass==2.75f&&
            m.rigidTable[1].shape==1&&m.rigidTable[1].size[1]==.65f&&m.jointTable[0].position[0]==.4f&&f.history.UndoCount()==1,
            "mass, shape, table contents, joint geometry and names enter one common-history node");
        owned_text::Allocation name;
        Check(owned_text::Find(m.rigidTable[0].jpText,name)&&name.bytes==after.rigidNames[0][0].size()*sizeof(wchar_t)&&
            !std::memcmp(m.rigidTable[0].jpText,after.rigidNames[0][0].data(),name.bytes),
            "the same geometry publication retains the entire edited name including its raw tail after NUL");
        Check(Near(static_cast<btRigidBody*>(m.rigidTable[0].body)->getInvMass(),btScalar(1)/btScalar(2.75))&&
            Near(static_cast<const btConvexShape*>(physics_scale::BaseShape(static_cast<btRigidBody*>(m.rigidTable[0].body)))->localGetSupportingVertex(btVector3(1,0,0)).x(),.8f),
            "the edited native body receives the requested mass and base geometry as well as its scaled display shape");
        Check(bone_scale::PoseValuesEqual(f.app->ModelSlot(0),before.pose)&&peer.Same()&&f.Stable(),"publishing scaled geometry retains current unregistered bone pose and peer physics");
        Scaled(f);const ModelState edited(f,0);
        for(unsigned i=0;i<12;++i) {
            Check(f.history.History(false)==MMS_OK&&before.Same()&&peer.Same()&&f.Stable(),"offline undo restores exact old scaled shapes, joint frames, tables and full text capacity");
            Scaled(f);
            Check(f.history.History(true)==MMS_OK&&edited.Same()&&peer.Same()&&f.Stable(),"offline redo restores the retained edited geometry without rereading a model or changing pose");
            Scaled(f);
        }
        Check(!f.history.LastPublicationRequiresPhysicsReset()&&f.history.UndoCount()==1&&!f.history.RedoCount(),"repeated scaled replay remains one node without resetting peer physics");
    }
    Check(owned_text::Count()==initialTexts,"scaled offline replay releases all source and replacement text allocations");
}
}
void NativeModelPhysicsPreparationChecks() {
    using namespace model_physics_preparation_checks;
    Check(d3dx::Get().Load(),"prewarm D3DX before allocation-fault preparation tests");
    btAlignedAllocSetCustom(HistoryAlloc,HistoryFree);btAlignedAllocSetCustomAligned(HistoryAlignedAlloc,HistoryAlignedFree);
    AllocationFailures(Stage::Begin);AllocationFailures(Stage::Set);AllocationFailures(Stage::Take);ScaledOfflineReplay();
    btAlignedAllocSetCustomAligned(nullptr,nullptr);btAlignedAllocSetCustom(nullptr,nullptr);
}
