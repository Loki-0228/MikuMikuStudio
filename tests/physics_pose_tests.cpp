// MAINTENANCE: regression-keep; invalid physics poses must not poison later frames.
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../src/io/pmm_io_common.hpp"
#include <btBulletDynamicsCommon.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <memory>
#include <vector>
using namespace mikumikustudio;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
bool Finite(const float* values, int count) {
    for (int i = 0; i < count; ++i) if (!std::isfinite(values[i])) return false;
    return true;
}
struct TemporaryFile {
    wchar_t path[MAX_PATH]{}; int fd = -1;
    TemporaryFile() {
        wchar_t directory[MAX_PATH]{}; GetTempPathW(MAX_PATH, directory);
        GetTempFileNameW(directory, L"phy", 0, path);
        fd = _wopen(path, _O_BINARY | _O_RDWR | _O_TRUNC);
        if (fd < 0) std::exit(2);
    }
    ~TemporaryFile() { _close(fd); DeleteFileW(path); }
};
void ReadPoseCase(const float position[3], const float rotation[4], bool preserve) {
    for (bool v2 : {false, true}) {
        TemporaryFile file;
        unsigned char record[58]{};
        std::memcpy(record + 28, position, 12);
        std::memcpy(record + 40, rotation, 16);
        record[57] = 1;
        _write(file.fd, record, v2 ? 58 : 57); _lseek(file.fd, 0, SEEK_SET);
        mdl::BoneKey key{};
        if (v2) pmm_io::ReadPmmBoneKey<pmm_io::PmmStream::V2>(file.fd, key);
        else pmm_io::ReadPmmBoneKey<pmm_io::PmmStream::V1>(file.fd, key);
        Check(Finite(key.position, 3) && Finite(key.rotation, 4), "PMM key contains finite pose");
        double norm = 0;
        for (float value : key.rotation) norm += double(value) * value;
        if (!preserve) Check(std::abs(norm - 1) < .001, "PMM rotation remains a unit quaternion");
        if (preserve) Check(std::memcmp(key.position, position, 12) == 0 &&
            std::memcmp(key.rotation, rotation, 16) == 0, "valid scientific values remain bit exact");
        Check(_tell(file.fd) == (v2 ? 58 : 57), "pose repair preserves PMM stream position");
        if (v2) Check(key.physicsDisabled == 1, "pose repair preserves physics key flags");
    }
}
void ReadPoses() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float ordinary[3] = {1e-8f, -1e-20f, 3e20f}, identity[4] = {0,0,0,1};
    ReadPoseCase(ordinary, identity, true);
    const float nonUnit[4] = {0, 0, .25f, 1.f};
    ReadPoseCase(ordinary, nonUnit, true);
    const float invalidPosition[3] = {nan, inf, -inf}, invalidRotation[4] = {nan,nan,nan,nan};
    ReadPoseCase(invalidPosition, invalidRotation, false);
    for (float scale : {0.f, 1e-30f, 1e30f}) {
        const float rotation[4] = {0, scale, 0, scale};
        ReadPoseCase(ordinary, rotation, false);
    }
}
void RegisterPoses() {
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    unsigned char selected = 1, physics = 0;
    std::vector<mdl::BoneKey> keys(mdl::kBoneKeyCapacity);
    std::vector<mdl::MorphKey> morphs(mdl::kMorphKeyCapacity);
    std::vector<mdl::DisplayKey> display(mdl::kDisplayKeyCapacity);
    model->boneCount = 1; model->boneTable = &bone; model->boneSelection = &selected;
    model->bonePhysicsState = &physics; model->boneKeys = keys.data();
    model->morphKeys = morphs.data(); model->displayKeys = display.data();
    bone.hasRigidBody = 1; bone.slotIndex = -1; bone.rotQuat[3] = 1;
    keys[0].rotation[3] = 1;
    bone.trans[0] = 1e-8f; bone.trans[1] = -2.f; bone.trans[2] = 3.f;
    for (float& value : bone.ikBackup) value = std::numeric_limits<float>::quiet_NaN();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    RegisterSelectedBoneKeys(raw, 15, 1);
    const int key = static_cast<int>(keys[0].next);
    Check(key > 0, "register a physics key at frame 15");
    Check(Finite(keys[key].position, 3) && Finite(keys[key].rotation, 4), "registration rejects broken physics pose");
    Check(keys[key].position[0] == 1e-8f && keys[key].position[1] == -2.f,
          "broken physics pose uses current finite bone position");
    Check(keys[key].rotation[3] == 1.f, "broken physics rotation uses current rotation");
    bone.ikBackup[0] = 1e-9f; bone.ikBackup[1] = 2e-8f; bone.ikBackup[2] = -3e-7f;
    bone.ikBackup[3] = bone.ikBackup[4] = bone.ikBackup[5] = 0; bone.ikBackup[6] = 1;
    RegisterSelectedBoneKeys(raw, 25, 1);
    const auto& later = keys[keys[key].next];
    Check(std::memcmp(later.position, bone.ikBackup, 12) == 0, "small scientific physics position registers exactly");
    for (auto& ring : model->undoRings) for (auto& slot : ring.slots) {
        ::operator delete(slot.bonePose); ::operator delete(slot.auxiliaryPose);
    }
}
struct World {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
    PhysicsScene scene{};
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::RigidRecord, 2> rigid{};
    mdl::JointRecord joint{};
    World() {
        scene.world = &world; world.setGravity(btVector3(0,0,0));
        model->rigidCount = 2; model->rigidTable = rigid.data();
        model->jointCount = 1; model->jointTable = &joint;
        joint.rigidA = 0; joint.rigidB = 1; joint.radiusBound = 2.f;
        float matrix[16]{}; matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
        for (auto& rec : rigid) {
            rec.boneIndex = -1; rec.size[0] = 1;
            void* output[2]{};
            CreateRigidBody(&scene, output, 0, 1,1,1, matrix, 1,1,0,0,0,0,0,0);
            rec.body = output[1];
        }
    }
    ~World() {
        for (auto& rec : rigid) {
            auto* body = static_cast<btRigidBody*>(rec.body);
            world.removeRigidBody(body); delete body->getMotionState();
            delete body->getCollisionShape(); delete body;
        }
    }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    btRigidBody* Body(int i) { return static_cast<btRigidBody*>(rigid[i].body); }
};
void RigidPoses() {
    World f;
    auto pose = btTransform::getIdentity(); pose.setOrigin(btVector3(3e20f,4e20f,0));
    f.Body(1)->setWorldTransform(pose);
    ModelPhysicsReadback(f.Raw());
    auto p = f.Body(1)->getWorldTransform().getOrigin();
    Check(std::abs(p.x()-1.2f)<1e-5f && std::abs(p.y()-1.6f)<1e-5f,
          "large joint separation clamps to radius without float-square overflow");
    f.model->jointCount = 0;
    f.rigid[1].position[0] = 7; f.rigid[1].position[1] = 8;
    pose.setOrigin(btVector3(std::numeric_limits<float>::quiet_NaN(), 0,0));
    f.Body(1)->setWorldTransform(pose);
    f.Body(1)->setLinearVelocity(btVector3(std::numeric_limits<float>::infinity(), 0,0));
    ModelPhysicsReadback(f.Raw());
    p = f.Body(1)->getWorldTransform().getOrigin();
    Check(p.x() == 7 && p.y() == 8 && p.z() == 0, "damaged body returns to finite follow pose");
    Check(f.Body(1)->getLinearVelocity().length2() == 0, "recovery clears invalid velocity");
    f.world.stepSimulation(1.f/60,1,1.f/60);
    p = f.Body(1)->getWorldTransform().getOrigin();
    Check(std::isfinite(p.x()) && std::isfinite(p.y()), "physics continues after body recovery");
    f.rigid[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    btTransform before; f.Body(0)->getMotionState()->getWorldTransform(before);
    ModelKinematicSync(f.Raw());
    btTransform after; f.Body(0)->getMotionState()->getWorldTransform(after);
    Check(after.getOrigin() == before.getOrigin(), "invalid kinematic pose retains last finite state");
    f.rigid[0].mode = 1;
    const auto beforeBody = f.Body(0)->getWorldTransform();
    ModelDynamicReseat(f.Raw());
    Check(f.Body(0)->getWorldTransform().getOrigin() == beforeBody.getOrigin(), "invalid dynamic reseat retains last finite state");
}
void UserFile(const wchar_t* path) {
    const int fd = _wopen(path, _O_BINARY | _O_RDONLY);
    if (fd < 0) { Check(false,"open supplied PMM"); return; }
    auto readByte = [&]() { unsigned char v=0; _read(fd,&v,1); return v; };
    auto readInt = [&]() { int v=0; _read(fd,&v,4); return v; };
    auto skipName = [&]() { const int n=readByte(); _lseek(fd,n,SEEK_CUR); };
    // One-model v2 fixture. Decode its tables instead of matching arbitrary floats.
    _lseek(fd,54,SEEK_SET); Check(readByte()==1,"fixture has one model");
    readByte(); skipName(); skipName(); _lseek(fd,256,SEEK_CUR);
    const int groups=readByte(), bones=readInt();
    if (bones < 1 || bones > 1000) { Check(false,"fixture bone count"); _close(fd); return; }
    for(int i=0;i<bones;++i)skipName();
    const int morphs=readInt(); for(int i=0;i<morphs;++i)skipName();
    const int ik=readInt(); _lseek(fd,4*ik,SEEK_CUR);
    const int rigids=readInt(); _lseek(fd,4*rigids+2+20+1+groups+8,SEEK_CUR);
    int invalid=0,total=0;
    auto checkKey=[&]() { mdl::BoneKey key{};
        pmm_io::ReadPmmBoneKey<pmm_io::PmmStream::V2>(fd,key); ++total;
        if(!Finite(key.position,3)||!Finite(key.rotation,4))++invalid;
    };
    for(int i=0;i<bones;++i)checkKey();
    const int extra=readInt();
    if(extra<0 || extra>1000){Check(false,"fixture sparse count");_close(fd);return;}
    for(int i=0;i<extra;++i){readInt();checkKey();}
    std::printf("user_pmm_keys=%d nonfinite_keys=%d\n",total,invalid);
    Check(invalid==0,"supplied PMM key poses are finite after loading"); _close(fd);
}
void Benchmark() {
    World f; f.model->jointCount=0;
    constexpr int samples=200000;
    auto start=std::chrono::steady_clock::now();
    for(int i=0;i<samples;++i) ModelPhysicsReadback(f.Raw());
    auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    std::printf("readback_us=%.6f iterations=%d bodies=2\n",elapsed/samples,samples);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc>1 && wcscmp(argv[1],L"--benchmark")==0) { Benchmark(); return 0; }
    ReadPoses(); RegisterPoses(); RigidPoses();
    if(argc>1)UserFile(argv[1]);
    std::printf("physics_pose_failures=%d\n",failures); return failures?1:0;
}