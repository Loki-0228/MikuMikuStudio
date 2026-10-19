// MAINTENANCE: regression-keep; exercise the production PhysicsInput callback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <btBulletDynamicsCommon.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mmv/mesh.hpp"

using namespace mikumikustudio;

namespace {
MMDApp* host=nullptr;
uint64_t frameNumber=0;

// The surrounding host supplies this conversion; the callback itself is
// included unchanged, including its production cache and error handling.
void Matrix(const btTransform& t,float* out) {
    mmv::Identity(out);const auto& b=t.getBasis();
    for(int row=0;row<3;++row)for(int col=0;col<3;++col)out[row*4+col]=float(b[col][row]);
    for(int i=0;i<3;++i)out[12+i]=float(t.getOrigin()[i]);
}
#include "../src/mmv4mms/physics_input.inl"

void Require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
bool Near(float a,float b) {return std::fabs(a-b)<1e-5f;}

struct Fixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    btDefaultCollisionConfiguration configuration;
    btCollisionDispatcher dispatcher{&configuration};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world{&dispatcher,&broadphase,&solver,&configuration};
    btBoxShape shape{btVector3(1,1,1)};
    PhysicsScene scene{};
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    std::vector<std::unique_ptr<btGeneric6DofConstraint>> constraints;
    std::vector<mdl::PmxVertex> pmx{2};
    std::vector<mdl::PmdVertex> pmd{2};
    std::vector<mdl::RigidRecord> rigids;
    std::vector<mdl::JointRecord> joints;

    Fixture() {
        physicsSnapshots.clear();frameNumber=1;snapshotNativePhysics=1;
        app->ClearModelSlots();app->PlaybackActive()=0;app->FrameStepPlayback()=0;app->DeltaTime()=0;
        app->ModelSlot(0)=reinterpret_cast<unsigned char*>(model.get());host=app.get();
        model->physicsMode=2;scene.world=&world;model->scenePtr=&scene;
        world.setGravity(btVector3(0,-98,0));
        pmx[0].normal[1]=pmx[1].normal[1]=1;
        pmd[0].normal[1]=pmd[1].normal[1]=1;
        AddBody();AddBody();AddJoint(0,1,41);Sync();
    }
    ~Fixture() {
        host=nullptr;physicsSnapshots.clear();
        while(world.getNumConstraints())world.removeConstraint(world.getConstraint(world.getNumConstraints()-1));
        while(world.getNumCollisionObjects())world.removeCollisionObject(world.getCollisionObjectArray()[world.getNumCollisionObjects()-1]);
    }
    uint64_t Id() const {return reinterpret_cast<uintptr_t>(model.get());}
    void Sync() {
        model->pmxVertices=pmx.empty()?nullptr:pmx.data();
        model->rawVertices=pmd.empty()?nullptr:pmd.data();
        model->vertexCount=uint32_t(model->physicsMode==2?pmx.size():pmd.size());
        model->rigidTable=rigids.empty()?nullptr:rigids.data();model->rigidCount=uint32_t(rigids.size());
        model->jointTable=joints.empty()?nullptr:joints.data();model->jointCount=uint32_t(joints.size());
    }
    void AddBody() {
        bodies.push_back(std::make_unique<btRigidBody>(2.f,nullptr,&shape,btVector3(3,4,5)));
        world.addRigidBody(bodies.back().get());
        mdl::RigidRecord record{};record.body=bodies.back().get();record.mass=2.f;
        rigids.push_back(record);
    }
    void AddJoint(size_t a,size_t b,int id) {
        btTransform frameA=btTransform::getIdentity(),frameB=btTransform::getIdentity();
        frameA.setOrigin(btVector3(1,2,3));frameB.setOrigin(btVector3(4,5,6));
        constraints.push_back(std::make_unique<btGeneric6DofConstraint>(*bodies[a],*bodies[b],frameA,frameB,true));
        constraints.back()->setUserConstraintId(id);world.addConstraint(constraints.back().get());
        mdl::JointRecord record{};record.constraint=id;record.rigidA=int(a);record.rigidB=int(b);joints.push_back(record);
    }
    MmvPhysicsInput Query() const {
        MmvPhysicsInput result{};result.structureSize=sizeof(result);
        Require(PhysicsInput(nullptr,Id(),&result)==MMV_OK,"PhysicsInput succeeds");
        return result;
    }
};

void MorphVertices() {
    Fixture f;
    const auto initial=f.Query();
    f.pmx[0].position[0]=2.5f;f.pmx[0].normal[0]=.6f;f.pmx[0].normal[1]=.8f;
    const auto changed=f.Query();
    Require(Near(changed.vertices[0].position[0],2.5f)&&Near(changed.vertices[0].normal[0],.6f),"same-frame PMX morph position and normal refresh");
    Require(Near(initial.vertices[0].position[0],0)&&Near(initial.vertices[0].normal[0],0),"previous same-frame PMX view remains valid and unchanged");
    f.pmx[0].position[0]=0;f.pmx[0].normal[0]=0;f.pmx[0].normal[1]=1;
    const auto restored=f.Query();
    Require(Near(restored.vertices[0].position[0],0)&&Near(restored.vertices[0].normal[1],1),"morph reset refreshes unskinned geometry");
    Require(Near(changed.vertices[0].position[0],2.5f),"third query preserves second borrowed view");
    ++frameNumber;f.pmx[0].position[0]=-3;
    Require(Near(f.Query().vertices[0].position[0],-3),"next frame uses current PMX geometry");
    Require(Near(f.pmx[0].position[0],-3),"read-only callback preserves source geometry");

    f.model->physicsMode=0;f.Sync();
    const auto pmdInitial=f.Query();f.pmd[1].position[2]=7;f.pmd[1].normal[1]=.5f;
    const auto pmdChanged=f.Query();
    Require(Near(pmdChanged.vertices[1].position[2],7)&&Near(pmdChanged.vertices[1].normal[1],.5f),"PMD morph position and normal refresh");
    Require(Near(pmdInitial.vertices[1].position[2],0),"PMD borrowed view remains unchanged");
}

void SameCountEdits() {
    Fixture f;
    const auto initial=f.Query();
    Require(initial.rigidBindings[0].valid&&initial.jointFrames[0].valid,"initial body and constraint are valid");
    f.rigids[0].position[0]=9;f.rigids[0].rotation[2]=1.5707963267948966f;
    f.rigids[0].mass=4;f.bodies[0]->setMassProps(4,btVector3(6,8,10));
    f.constraints[0]->getFrameOffsetA().setOrigin(btVector3(11,12,13));
    f.constraints[0]->getFrameOffsetB().setOrigin(btVector3(14,15,16));
    f.constraints[0]->getFrameOffsetB().setRotation(btQuaternion(btVector3(0,0,1),1.5707963267948966f));
    const auto changed=f.Query();
    Require(changed.rigidCount==initial.rigidCount&&changed.jointCount==initial.jointCount,"editing keeps counts unchanged");
    Require(Near(changed.rigidBindings[0].boneToBody[12],9)&&Near(changed.rigidBindings[0].boneToBody[0],0)&&Near(changed.rigidBindings[0].boneToBody[1],1),"same-count body translation and rotation refresh");
    Require(Near(changed.rigidBindings[0].inertia[0],6)&&Near(changed.rigidBindings[0].inertia[1],8)&&Near(changed.rigidBindings[0].inertia[2],10),"same-count mass/inertia update refreshes binding");
    Require(Near(changed.jointFrames[0].frameA[12],11)&&Near(changed.jointFrames[0].frameB[14],16)&&Near(changed.jointFrames[0].frameB[1],1),"same-count joint anchors and orientation refresh");
    Require(Near(initial.rigidBindings[0].inertia[0],3)&&Near(initial.rigidBindings[0].boneToBody[12],0)&&Near(initial.jointFrames[0].frameA[12],1),"previous binding and joint arrays remain unchanged");
    Require(Near(f.bodies[0]->getInvMass(),.25f)&&Near(f.rigids[0].position[0],9)&&Near(f.constraints[0]->getFrameOffsetA().getOrigin().x(),11),"callback leaves model and Bullet state unchanged");
    f.bodies[0]->setMassProps(0,btVector3(0,0,0));
    const auto fixed=f.Query();
    Require(fixed.rigidBindings[0].valid&&Near(fixed.rigidBindings[0].inertia[0],0),"static body clears previous nonzero inertia");
    f.rigids[0].body=nullptr;f.world.removeConstraint(f.constraints[0].get());
    const auto missing=f.Query();
    Require(!missing.rigidBindings[0].valid&&Near(missing.rigidBindings[0].inertia[0],0)&&!missing.jointFrames[0].valid,"removed native body/constraint clears previous valid fields");
    f.rigids[0].body=f.bodies[0].get();f.world.addConstraint(f.constraints[0].get());
    Require(f.Query().jointFrames[0].valid,"constraint reconstruction becomes visible");
}

void StructuralEdits() {
    Fixture f;
    const auto initial=f.Query();
    f.pmx.resize(4);f.pmx[3].position[2]=19;f.AddBody();f.AddJoint(1,2,42);f.Sync();
    const auto grown=f.Query();
    Require(grown.vertexCount==4&&grown.rigidCount==3&&grown.jointCount==2,"added vertex/body/joint counts refresh");
    Require(Near(grown.vertices[3].position[2],19)&&grown.rigidBindings[2].valid&&grown.jointFrames[1].valid,"new structures have current data");
    Require(initial.vertexCount==2&&initial.rigidCount==2&&initial.jointCount==1&&Near(initial.jointFrames[0].frameA[12],1),"growth preserves previously borrowed arrays");
    f.world.removeConstraint(f.constraints[1].get());f.joints.pop_back();
    f.world.removeRigidBody(f.bodies[2].get());f.rigids.pop_back();f.pmx.resize(1);f.Sync();
    const auto shrunk=f.Query();
    Require(shrunk.vertexCount==1&&shrunk.rigidCount==2&&shrunk.jointCount==1,"removed structures disappear from current counts");
    Require(grown.jointFrames[1].valid&&Near(grown.vertices[3].position[2],19),"shrink preserves prior borrowed arrays");
    f.pmx.clear();f.rigids.clear();f.joints.clear();f.Sync();
    const auto empty=f.Query();
    Require(empty.vertexCount==0&&empty.rigidCount==0&&empty.jointCount==0,"empty tables accept null backing pointers");
}

void FailureAndRebuild() {
    Fixture f;
    const auto initial=f.Query();
    auto failed=[&]() {
        MmvPhysicsInput output;std::memset(&output,0x5a,sizeof(output));output.structureSize=sizeof(output);
        std::array<unsigned char,sizeof(output)> before{};std::memcpy(before.data(),&output,sizeof(output));
        Require(PhysicsInput(nullptr,f.Id(),&output)==MMV_FAILED,"nonempty table with missing storage fails");
        Require(std::memcmp(before.data(),&output,sizeof(output))==0,"failed query leaves caller output unchanged");
        Require(Near(initial.vertices[0].normal[1],1)&&initial.rigidBindings[0].valid&&initial.jointFrames[0].valid,"failure preserves earlier borrowed arrays");
    };
    f.model->pmxVertices=nullptr;failed();f.Sync();
    f.model->physicsMode=0;f.model->rawVertices=nullptr;failed();f.model->physicsMode=2;f.Sync();
    f.model->rigidTable=nullptr;failed();f.Sync();
    f.model->jointTable=nullptr;failed();f.Sync();
    f.pmx[0].position[1]=21;Require(Near(f.Query().vertices[0].position[1],21),"query recovers after storage is restored");
    ++frameNumber;f.model->pmxVertices=nullptr;failed();f.Sync();
    // Old arrays remain valid after a failed new-frame request; a successful
    // new-frame request ends their lifetime, so do not access them after this.
    Require(Near(f.Query().vertices[0].position[1],21),"new frame recovers after failed refresh");
    f.app->ModelSlot(0)=nullptr;
    MmvPhysicsInput output{};output.structureSize=sizeof(output);
    Require(PhysicsInput(nullptr,f.Id(),&output)==MMV_UNSUPPORTED,"removed object ID is rejected");
    physicsSnapshots.erase(f.Id());
    f.app->ModelSlot(0)=reinterpret_cast<unsigned char*>(f.model.get());f.pmx[0].position[1]=31;
    Require(Near(f.Query().vertices[0].position[1],31),"object rebuilt at the same address receives new data");
    physicsSnapshots.clear();f.app->ClearModelSlots();
    Require(PhysicsInput(nullptr,f.Id(),&output)==MMV_UNSUPPORTED,"empty scene rejects old object ID");
    f.app->ModelSlot(0)=reinterpret_cast<unsigned char*>(f.model.get());f.pmx[0].position[1]=41;
    Require(Near(f.Query().vertices[0].position[1],41),"scene rebuild receives new data");
}

void AbiAndMetadata() {
    Fixture f;
    f.app->PlaybackActive()=1;f.app->FrameStepPlayback()=1;f.app->DeltaTime()=.125f;
    f.world.setGravity(btVector3(1,-2,3));snapshotNativePhysics=0;
    const auto current=f.Query();
    Require(current.objectId==f.Id()&&current.structureSize==sizeof(current)&&!current.nativePhysicsEnabled,"current ABI and native physics flag");
    Require(current.flags==(MMV_PHYSICS_TIMING_VALID|MMV_PHYSICS_PLAYING|MMV_PHYSICS_FRAME_STEP)&&current.frameDeltaSeconds==.125,"timing flags and delta");
    Require(Near(current.gravity[0],1)&&Near(current.gravity[1],-2)&&Near(current.gravity[2],3),"live gravity");
    f.app->DeltaTime()=-1;f.model->scenePtr=nullptr;
    const auto noScene=f.Query();
    Require(noScene.frameDeltaSeconds==0&&Near(noScene.gravity[1],-98)&&!noScene.jointFrames[0].valid,"missing physics scene uses defaults without stale joints");

    MmvPhysicsInput legacy{};legacy.structureSize=uint32_t(offsetof(MmvPhysicsInput,frameDeltaSeconds));legacy.frameDeltaSeconds=9876;
    Require(PhysicsInput(nullptr,f.Id(),&legacy)==MMV_OK,"legacy ABI succeeds");
    Require(legacy.structureSize==offsetof(MmvPhysicsInput,frameDeltaSeconds)&&legacy.flags==0&&legacy.frameDeltaSeconds==9876,"legacy ABI preserves optional tail and omits timing flags");
    struct Extended {MmvPhysicsInput input;std::array<unsigned char,16> tail;};
    Extended extended{};extended.input.structureSize=sizeof(extended);extended.tail.fill(0x5a);
    Require(PhysicsInput(nullptr,f.Id(),&extended.input)==MMV_OK&&extended.input.structureSize==sizeof(MmvPhysicsInput),"larger ABI is bounded to host structure");
    Require(std::all_of(extended.tail.begin(),extended.tail.end(),[](unsigned char value){return value==0x5a;}),"larger ABI tail is untouched");
    MmvPhysicsInput bad{};bad.structureSize=uint32_t(offsetof(MmvPhysicsInput,frameDeltaSeconds)-1);
    Require(PhysicsInput(nullptr,f.Id(),&bad)==MMV_INVALID_ARGUMENT,"undersized ABI is rejected");
    Require(PhysicsInput(nullptr,f.Id(),nullptr)==MMV_INVALID_ARGUMENT,"null output is rejected");
    bad.structureSize=sizeof(bad);
    Require(PhysicsInput(nullptr,0,&bad)==MMV_UNSUPPORTED&&PhysicsInput(nullptr,f.Id()+1,&bad)==MMV_UNSUPPORTED,"null and unknown object IDs are rejected");
    host=nullptr;Require(PhysicsInput(nullptr,f.Id(),&bad)==MMV_INVALID_ARGUMENT,"shutdown host is rejected");host=f.app.get();
}

void ObjectIsolation() {
    Fixture f;
    auto other=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::PmxVertex,1> vertices{};vertices[0].position[0]=57;
    other->physicsMode=2;other->vertexCount=1;other->pmxVertices=vertices.data();
    f.app->ModelSlot(1)=reinterpret_cast<unsigned char*>(other.get());
    const auto first=f.Query();
    MmvPhysicsInput second{};second.structureSize=sizeof(second);
    Require(PhysicsInput(nullptr,reinterpret_cast<uintptr_t>(other.get()),&second)==MMV_OK,"second model query succeeds");
    f.pmx[0].position[0]=67;const auto updated=f.Query();
    Require(Near(updated.vertices[0].position[0],67)&&Near(first.vertices[0].position[0],0)&&Near(second.vertices[0].position[0],57),"models and repeated borrowed views stay independent");
}

void FrameBufferReuse() {
    Fixture f;
    const auto storage=[](const MmvPhysicsInput& input){return std::array<uintptr_t,3>{
        reinterpret_cast<uintptr_t>(input.vertices),reinterpret_cast<uintptr_t>(input.rigidBindings),reinterpret_cast<uintptr_t>(input.jointFrames)};};
    const auto first=f.Query();
    f.pmx[0].position[0]=2;const auto second=f.Query();
    Require(first.vertices!=second.vertices&&first.rigidBindings!=second.rigidBindings&&first.jointFrames!=second.jointFrames,"same-frame requests own distinct array storage");
    const auto firstStorage=storage(first),secondStorage=storage(second);
    ++frameNumber;f.pmx[0].position[0]=3;const auto next=f.Query();
    const auto nextStorage=storage(next);
    Require(nextStorage[0]!=firstStorage[0]&&nextStorage[0]!=secondStorage[0],"new frame prepares separate storage before retiring prior views");
    // Save addresses only. The prior views expire after a successful new-frame
    // query, so all subsequent content checks use newly returned views.
    ++frameNumber;f.pmx[0].position[0]=4;f.rigids[0].body=nullptr;
    f.world.removeConstraint(f.constraints[0].get());
    const auto reused=f.Query();
    Require(storage(reused)==firstStorage,"first slot reuses all array capacities after two frames");
    Require(Near(reused.vertices[0].position[0],4)&&!reused.rigidBindings[0].valid&&Near(reused.rigidBindings[0].inertia[0],0)&&!reused.jointFrames[0].valid,"reused arrays replace geometry and clear absent body/joint fields");
    f.pmx[0].position[0]=5;f.rigids[0].body=f.bodies[0].get();f.bodies[0]->setMassProps(0,btVector3(0,0,0));
    const auto reusedSecond=f.Query();
    Require(storage(reusedSecond)==secondStorage,"second same-frame slot reuses its retired arrays");
    Require(Near(reused.vertices[0].position[0],4)&&Near(reusedSecond.vertices[0].position[0],5),"reused same-frame slots preserve independent borrowed values");
    Require(reusedSecond.rigidBindings[0].valid&&Near(reusedSecond.rigidBindings[0].inertia[0],0),"reused second slot clears former dynamic inertia");
    ++frameNumber;const auto alternate=f.Query();
    Require(storage(alternate)==nextStorage,"alternate bank also reuses all array capacities");
    Require(alternate.rigidBindings[0].valid&&Near(alternate.rigidBindings[0].inertia[0],0)&&!alternate.jointFrames[0].valid,"alternate bank clears stale physical fields");
}

int Benchmark() {
    Fixture f;
    constexpr uint32_t vertices=50000,rigids=128,joints=127,iterations=300,warmup=20;
    f.pmx.resize(vertices);
    while(f.rigids.size()<rigids)f.AddBody();
    while(f.joints.size()<joints){const auto i=f.joints.size();f.AddJoint(i,i+1,int(100+i));}
    f.Sync();
    for(uint32_t i=0;i<vertices;++i){f.pmx[i].position[0]=float(i)*.01f;f.pmx[i].normal[1]=1;}
    double elapsedMs=0,checksum=0;
    for(uint32_t i=0;i<warmup+iterations;++i){
        ++frameNumber;f.pmx[0].position[0]=float(i);f.rigids[0].position[1]=float(i);
        f.bodies[0]->setMassProps(2,btVector3(float(i+1),4,5));
        f.constraints[0]->getFrameOffsetA().setOrigin(btVector3(float(i),2,3));
        const auto start=std::chrono::steady_clock::now();
        const auto input=f.Query();
        const auto end=std::chrono::steady_clock::now();
        if(i>=warmup){elapsedMs+=std::chrono::duration<double,std::milli>(end-start).count();
            checksum+=input.vertices[0].position[0]+input.rigidBindings[0].inertia[0]+input.jointFrames[0].frameA[12];}
    }
    // Baseline and fixed builds deliberately need not have matching checksums:
    // the baseline keeps stale inputs. Timing covers the callback only.
    std::printf("physics_input_benchmark iterations=%u warmup=%u vertices=%u rigids=%u joints=%u elapsed_ms=%.6f mean_us=%.6f checksum=%.6f\n",
        iterations,warmup,vertices,rigids,joints,elapsedMs,elapsedMs*1000/iterations,checksum);
    return 0;
}
} // namespace

int main(int argc,char** argv) {
    if(argc==2&&std::strcmp(argv[1],"--benchmark")==0){
        try{return Benchmark();}catch(const std::exception& error){std::printf("FAIL benchmark: %s\n",error.what());return 1;}
    }
    if(argc!=1){std::fprintf(stderr,"Usage: mmv4mms_physics_input_tests [--benchmark]\n");return 2;}
    struct Test {const char* name;void (*run)();};
    const Test tests[]={{"morph vertices",MorphVertices},{"same-count edits",SameCountEdits},
        {"structural edits",StructuralEdits},{"failure and rebuild",FailureAndRebuild},
        {"ABI and metadata",AbiAndMetadata},{"object isolation",ObjectIsolation},
        {"frame buffer reuse",FrameBufferReuse}};
    int failures=0;
    for(const auto& test:tests){
        try{test.run();std::printf("PASS %s\n",test.name);}
        catch(const std::exception& error){++failures;std::printf("FAIL %s: %s\n",test.name,error.what());}
    }
    std::printf("physics input: %d failures\n",failures);
    return failures?1:0;
}
