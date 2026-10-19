#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_pose_safety.hpp"
#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/scene_sampling.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace mikumikustudio::bake {
namespace {
struct SceneError {};
template<class T> void Copy(std::vector<T>& dst, T*& target, const T* source, std::size_t count) {
    if (count && !source) throw SceneError{};
    if (count) dst.assign(source, source + count);
    target = dst.empty() ? nullptr : dst.data();
}
struct ModelCopy {
    std::unique_ptr<mdl::ModelRecord> model;
    std::vector<mdl::BoneRecord> bones;
    std::vector<mdl::MorphRecord> morphs;
    std::vector<mdl::IkChain> ik;
    std::vector<mdl::BoneOrderEntry> order;
    std::vector<mdl::BoneMorphOffsetRecord> boneMorphs;
    std::vector<mdl::RigidRecord> rigids;
    std::vector<mdl::JointRecord> joints;
    std::vector<unsigned> boneCursors, morphCursors;
    std::vector<unsigned char> boneActive, morphActive, selection, physicsState;
    std::vector<mdl::PmdVertex> pmdVertices;
    std::vector<mdl::PmxVertex> pmxVertices;
    std::vector<mdl::ModelMaterialRecord> materials;
    std::vector<mdl::MaterialMorphPool> materialAdd, materialMul;
    std::vector<unsigned> indices;
    unsigned char* raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    explicit ModelCopy(const mdl::ModelRecord& source, bool geometry = false) : model(new mdl::ModelRecord(source)) {
        auto& m = *model;
        if (!source.boneKeys || !source.displayKeys ||
            source.boneCount > mdl::BoneKeyCapacity(source)) throw SceneError{};
        Copy(bones, m.boneTable, source.boneTable, m.boneCount);
        Copy(morphs, m.morphs, source.morphs, m.morphCount);
        Copy(ik, m.ikChains, source.ikChains, m.ikChainCount);
        auto* ordered = static_cast<mdl::BoneOrderEntry*>(source.boneOrderTable);
        mdl::BoneOrderEntry* newOrder = nullptr;
        Copy(order, newOrder, ordered, m.boneOrderCount); m.boneOrderTable = newOrder;
        Copy(boneMorphs, m.boneMorphTable, source.boneMorphTable, m.boneMorphCount);
        Copy(rigids, m.rigidTable, source.rigidTable, m.rigidCount);
        Copy(joints, m.jointTable, source.jointTable, m.jointCount);
        Copy(boneCursors, m.boneKeyCursors, source.boneKeyCursors, m.boneCount);
        Copy(morphCursors, m.morphKeyCursors, source.morphKeyCursors, m.morphCount);
        Copy(boneActive, m.boneTrackActive, source.boneTrackActive, m.boneCount);
        Copy(morphActive, m.morphTrackActive, source.morphTrackActive, m.morphCount);
        Copy(selection, m.boneSelection, source.boneSelection, m.boneCount);
        Copy(physicsState, m.bonePhysicsState, source.bonePhysicsState, m.boneCount);
        // Morph payloads and motion keys are immutable. Rendering material and
        // pose-trace writes are unnecessary in an offline bone evaluation.
        m.hwnd = nullptr; m.poseTraceEnabled = 0;
        if (geometry) {
            if (m.physicsMode == 2) Copy(pmxVertices, m.pmxVertices, source.pmxVertices, m.vertexCount);
            else Copy(pmdVertices, m.rawVertices, source.rawVertices, m.vertexCount);
            mdl::ModelMaterialRecord* newMaterials = nullptr;
            Copy(materials, newMaterials, static_cast<const mdl::ModelMaterialRecord*>(source.materials), m.materialCount);
            m.materials = newMaterials;
            if (m.physicsMode == 2) {
                Copy(materialAdd,m.materialMorphPools.additive,source.materialMorphPools.additive,m.materialCount);
                Copy(materialMul,m.materialMorphPools.multiplicative,source.materialMorphPools.multiplicative,m.materialCount);
            }
            unsigned* newIndices = nullptr;
            Copy(indices,newIndices,static_cast<const unsigned*>(source.indices),m.indexCount);
            m.indices = newIndices;
        } else m.materialCount = 0;
        const auto* original = reinterpret_cast<const unsigned char*>(&source);
        for (unsigned j=0;j<m.jointCount;++j) joints[j].radiusBound=physics_scale::JointRadiusBaseline(original,j);
        bone_scale::CopyState(original, raw());
        morph_curve::CopyState(original, raw());
    }
    ~ModelCopy() { physics_scale::Forget(raw()); bone_scale::Forget(raw()); morph_curve::Forget(raw()); }
};
class BakeJoint final : public btGeneric6DofSpringConstraint {
    btScalar erp_, cfm_;
public:
    BakeJoint(btRigidBody& a, btRigidBody& b, const btTransform& fa, const btTransform& fb,
              const mdl::JointRecord& joint, const Settings& settings)
        : btGeneric6DofSpringConstraint(a, b, fa, fb, true), erp_(settings.erp), cfm_(settings.cfm) {
        const auto* v = joint.limits;
        setLinearUpperLimit(btVector3(v[0],v[1],v[2]));
        setLinearLowerLimit(btVector3(v[3],v[4],v[5]));
        setAngularUpperLimit(btVector3(v[6],v[7],v[8]));
        setAngularLowerLimit(btVector3(v[9],v[10],v[11]));
        for (int i=0;i<6;++i) if (joint.springs[i]>0) {
            enableSpring(i,true); setStiffness(i,joint.springs[i]);
        }
        for(int i=0;i<3;++i)getRotationalLimitMotor(i)->m_ERP=settings.erp;
        getTranslationalLimitMotor()->m_restitution=settings.erp;
        // Loader joints meet at the same model-space point at rest.
        for (int i=0;i<6;++i) m_equilibriumPoint[i] = 0;
    }
    void getInfo2(btConstraintInfo2* info) override {
        info->erp = erp_;
        btGeneric6DofSpringConstraint::getInfo2(info);
        btConstraintInfo1 rows{}; getInfo1(&rows);
        for (int i=0;i<rows.m_numConstraintRows;++i) info->cfm[i*info->rowskip] = cfm_;
    }
};
struct WorldCopy {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
    PhysicsScene scene{};
    std::vector<std::unique_ptr<btDefaultMotionState>> motions;
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    std::vector<std::unique_ptr<BakeJoint>> joints;
    std::unordered_map<const btRigidBody*,btRigidBody*> bodyMap;
    explicit WorldCopy(btDiscreteDynamicsWorld& original,const Settings& settings) {
        scene.world = &world;
        world.getSolverInfo() = btContactSolverInfo();
        world.getSolverInfo().m_erp = settings.erp;
        world.getSolverInfo().m_globalCfm = settings.cfm;
        world.setGravity(original.getGravity()); world.ResetLocalTime();
        for (int i=0;i<original.getNumCollisionObjects();++i) {
            auto* src = btRigidBody::upcast(original.getCollisionObjectArray()[i]);
            if (!src) throw SceneError{};
            const btScalar mass = src->getInvMass() > 0 ? 1/src->getInvMass() : 0;
            btVector3 inertia(0,0,0);
            for (int axis=0;axis<3;++axis) if (src->getInvInertiaDiagLocal()[axis]>0)
                inertia[axis] = 1/src->getInvInertiaDiagLocal()[axis];
            auto* shape=const_cast<btCollisionShape*>(physics_scale::BaseShape(src));
            if(shape!=src->getCollisionShape())shape->calculateLocalInertia(mass,inertia);
            motions.emplace_back(new btDefaultMotionState(src->getWorldTransform()));
            btRigidBody::btRigidBodyConstructionInfo ci(mass,motions.back().get(),shape,inertia);
            ci.m_linearDamping=src->getLinearDamping(); ci.m_angularDamping=src->getAngularDamping();
            ci.m_friction=src->getFriction(); ci.m_restitution=src->getRestitution();
            bodies.emplace_back(new btRigidBody(ci)); auto* dst=bodies.back().get();
            dst->setCollisionFlags(src->getCollisionFlags()); dst->setActivationState(DISABLE_DEACTIVATION);
            auto* proxy=src->getBroadphaseHandle();
            world.addRigidBody(dst,proxy ? proxy->m_collisionFilterGroup : 1,proxy ? proxy->m_collisionFilterMask : -1);
            bodyMap[src]=dst;
        }
    }
    ~WorldCopy() {
        for (auto& c:joints) world.removeConstraint(c.get());
        for (auto& b:bodies) world.removeRigidBody(b.get());
    }
    void Connect(ModelCopy& copy,const mdl::ModelRecord& original,const Settings& settings) {
        copy.model->scenePtr=&scene;
        for (auto& r:copy.rigids) {
            if (!r.body) continue;
            auto found=bodyMap.find(static_cast<btRigidBody*>(r.body));
            if (found==bodyMap.end()) throw SceneError{};
            r.body=found->second; r.keyData=found->second->getMotionState();
        }
        for (auto& j:copy.joints) {
            if (j.constraint<0) continue;
            auto* ow=original.scenePtr ? original.scenePtr->world : nullptr;
            btGeneric6DofSpringConstraint* source=nullptr;
            if (ow) for(int i=0;i<ow->getNumConstraints();++i) {
                auto* c=ow->getConstraint(i);
                if(c->getUid()==j.constraint) { source=dynamic_cast<btGeneric6DofSpringConstraint*>(c); break; }
            }
            if (!source || j.rigidA<0 || j.rigidB<0 || j.rigidA>=static_cast<int>(copy.rigids.size()) ||
                j.rigidB>=static_cast<int>(copy.rigids.size())) { j.constraint=-1; continue; }
            auto* a=static_cast<btRigidBody*>(copy.rigids[j.rigidA].body);
            auto* b=static_cast<btRigidBody*>(copy.rigids[j.rigidB].body);
            if(!a||!b) { j.constraint=-1; continue; }
            btTransform fa,fb; physics_scale::JointBaseline(*source,fa,fb);
            joints.emplace_back(new BakeJoint(*a,*b,fa,fb,j,settings));
            joints.back()->setUserConstraintId(j.constraint);
            world.addConstraint(joints.back().get(),false);
        }
    }
};
bool DynamicBone(const mdl::ModelRecord& m,unsigned bone) {
    if (!m.boneTable[bone].hasRigidBody) return false;
    for(unsigned i=0;i<m.rigidCount;++i)
        if(m.rigidTable[i].boneIndex==static_cast<int>(bone) && !m.rigidTable[i].staticFlag && m.rigidTable[i].body) return true;
    return false;
}
std::vector<int> Targets(const mdl::ModelRecord& m,const Request& request) {
    std::vector<int> out;
    for(unsigned b=0;b<m.boneCount;++b) {
        if((request.selectedOnly || request.boneCoverage==channel_bake::Coverage::Selected) && (!m.boneSelection || !m.boneSelection[b])) continue;
        if(request.boneCoverage==channel_bake::Coverage::Explicit &&
           std::find(request.boneIndices.begin(),request.boneIndices.end(),int(b))==request.boneIndices.end())continue;
        if((request.physics && DynamicBone(m,b)) || (request.keyframes && m.boneTable[b].type!=mdl::BoneType::InertTip)) out.push_back(b);
    }
    return out;
}
void Evaluate(std::vector<std::unique_ptr<ModelCopy>>& models,const std::array<unsigned char*,kModelSlotCount>& slots,double seconds,bool init) {
    for(auto& m:models) {
        if(init) InitModelTrackCursors(m->raw(),seconds,2);
        AdvanceModelKeyframes(m->raw(),seconds,2);
        for(auto& bone:m->bones) RepairBonePose(bone.trans,bone.rotQuat);
    }
    // Preserve model transform dependency order.
    for(int order=0;order<kModelSlotCount;++order) for(auto& m:models) if(m->model->comboSelIndex2==order) {
        ModelApplyMorphs(m->raw()); SetPhysicsMode(m->raw(),0,slots.data(),init?0:2);
    }
}
void Readback(std::vector<std::unique_ptr<ModelCopy>>& models,const std::array<unsigned char*,kModelSlotCount>& slots) {
    for(auto& m:models) ModelPhysicsReadback(m->raw());
    for(int order=0;order<kModelSlotCount;++order) for(auto& m:models) if(m->model->comboSelIndex2==order)
        { ModelApplyMorphs(m->raw()); SetPhysicsMode(m->raw(),1,slots.data(),2); }
}
void SetGravity(MMDApp& app,btDiscreteDynamicsWorld& world,double frame) {
    const auto* keys=app.GravityKeys();
    btVector3 direction(app.GravityX(),app.GravityY(),app.GravityZ()); float magnitude=app.GravityMagnitude();
    int noise=app.GravityNoise(); bool enabled=app.GravityNoiseEnabled()!=0;
    if(keys) {
        unsigned i=0; int guard=0;
        while(keys[i].next) {
            if(keys[i].next>=mdl::KeyPoolCapacity(keys)) throw SceneError{};
            if(keys[keys[i].next].frame>frame) break;
            i=keys[i].next; if(i>=mdl::KeyPoolCapacity(keys) || ++guard>=mdl::KeyPoolCapacity(keys)) throw SceneError{};
        }
        const auto& a=keys[i]; float t=0;
        if(a.next && keys[a.next].frame>a.frame) t=float(std::clamp((frame-a.frame)/(keys[a.next].frame-a.frame),0.0,1.0));
        const auto& b=keys[a.next ? a.next : i];
        direction=btVector3(a.direction[0],a.direction[1],a.direction[2]).lerp(btVector3(b.direction[0],b.direction[1],b.direction[2]),t);
        magnitude=a.acceleration+(b.acceleration-a.acceleration)*t; noise=a.noise; enabled=a.noiseEnabled!=0;
    }
    if(direction.length2()==0) direction=btVector3(0,.1f,0);
    direction.normalize();
    if(enabled && noise) {
        // Local deterministic wind; never consumes the editor's random state.
        std::uint32_t state=0x9e3779b9u^static_cast<unsigned>(frame/15.f);
        for(int c=0;c<3;++c) { state=1664525u*state+1013904223u; direction[c]+=(static_cast<int>((state>>16)&32767)-16384)/16384.f*noise*.1f; }
    }
    world.setGravity(direction*(magnitude*10.f));
}
// Private key pool for one bake.  It starts as a copy of the live pool at its
// runtime capacity and grows on demand as the bake appends keys, so a request
// is never charged the worst-case frames*bones allocation.  The live model is
// not touched here; bake::Commit grows it by exactly the slots the result
// needs.
struct KeyBuilder {
    std::vector<mdl::BoneKey> keys;
    std::vector<unsigned> cursors;
    unsigned freeSlot, maxFrame;
    bone_scale::Values scales;
    explicit KeyBuilder(const mdl::ModelRecord& m):keys(m.boneKeys,m.boneKeys+mdl::BoneKeyCapacity(m)),cursors(m.boneCount),freeSlot(m.boneCount),maxFrame(m.maxFrame) {
        for(unsigned i=0;i<m.boneCount;++i)cursors[i]=i;
    }
    // Enlarges the private pool when the free-slot scan reached its end.
    // Returns false at the structural slot ceiling or when the allocation
    // fails, which the caller reports as Error::Capacity / Error::Allocation.
    bool Grow() {
        const std::size_t current = keys.size();
        if (current == 0) return false;
        const std::size_t target = mdl::BoneKeyPlanCapacity(current, current + 1);
        if (target <= current) return false;
        try { keys.resize(target); } catch (const std::bad_alloc&) { return false; }
        catch (...) { return false; }
        return true;
    }
    void EraseRange(unsigned bone,unsigned first,unsigned last) {
        unsigned previous=bone,slot=keys[bone].next;size_t guard=0;
        while(slot){
            if(slot>=keys.size()||++guard>=keys.size())throw SceneError{};
            const unsigned next=keys[slot].next;
            if(keys[slot].frame>=first&&keys[slot].frame<=last){
                keys[previous].next=next;if(next){if(next>=keys.size())throw SceneError{};keys[next].previous=previous;}
                keys[slot]={};scales[int(slot)]={};freeSlot=std::min(freeSlot,slot);
            }else previous=slot;
            slot=next;
        }
    }
    bool Put(unsigned bone,unsigned frame,const float* p,const float* q,unsigned char disabled,bool onlyMissing=false, const bone_scale::Matrix& scale={}) {
        unsigned current=cursors[bone]; unsigned guard=0;
        while(keys[current].next) {
            if(keys[current].next>=keys.size()) throw SceneError{};
            if(keys[keys[current].next].frame>frame) break;
            current=keys[current].next; if(current>=keys.size() || ++guard>=keys.size()) throw SceneError{};
        }
        if(keys[current].frame>frame) throw SceneError{};
        unsigned slot=current;
        if(keys[current].frame!=frame) {
            while(freeSlot<keys.size() && keys[freeSlot].frame)++freeSlot;
            // Growth moves the pool, so the records below are indexed again
            // rather than referenced.
            if(freeSlot==keys.size() && !Grow())return false;
            slot=freeSlot++; auto& fresh=keys[slot]; fresh={}; fresh.frame=frame;
            fresh.previous=current; fresh.next=keys[current].next;
            if(fresh.next) keys[fresh.next].previous=slot;
            keys[current].next=slot;
        } else if(onlyMissing) { cursors[bone]=slot; return true; }
        scales[int(slot)]=scale;
        auto& key=keys[slot];
        std::memcpy(key.position,p,sizeof key.position);std::memcpy(key.rotation,q,sizeof key.rotation);
        for(int i=0;i<4;++i){key.interpolation[i]=key.interpolation[i+4]=20;key.interpolation[i+8]=key.interpolation[i+12]=107;}
        key.physicsDisabled=disabled;key.allocated=1;cursors[bone]=slot;maxFrame=std::max(maxFrame,frame);return true;
    }
};
bool Finite(const float* a,int n) { for(int i=0;i<n;++i)if(!std::isfinite(a[i]))return false; return true; }
}

bool HasExistingKeys(const unsigned char* bytes,const Request& request) {
    if(!bytes)return false; const auto& m=*mdl::Mdl(bytes);
    // One capacity lookup for the whole probe, never one per chain step.
    const std::size_t capacity=mdl::BoneKeyCapacity(m);
    if(capacity==0||m.boneKeys==nullptr)return false;
    for(int b:Targets(m,request)) { unsigned i=b; std::size_t guard=0;
        do {const auto& k=m.boneKeys[i];if(k.frame>=static_cast<unsigned>(request.firstFrame)&&k.frame<=static_cast<unsigned>(request.lastFrame))return true;
            i=k.next;if(i>=capacity || ++guard>=capacity)return true;
        }while(i);
    }return false;
}
static SceneResult ComputeBatch(MMDApp& app,const Request& request,bool wholeScene,const Progress& progress) {
    SceneResult result; SettingsError settingError;
    if(request.firstFrame<0 || request.lastFrame<request.firstFrame || request.lastFrame>frame_limits::maximum || request.step<1 || request.step>frame_limits::maximum ||
       (!request.physics&&!request.keyframes) || !ValidateSettings(request.settings,settingError)) { result.error=Error::InvalidRequest;return result; }
    auto* selected=wholeScene?nullptr:app.SelectedModel();
    if((!wholeScene&&!selected) || !app.Physics() || !app.Physics()->world) {result.error=Error::NoModel;return result;}
    try {
        const std::uint64_t frames=1u+unsigned(request.lastFrame-request.firstFrame)/unsigned(request.step)+((request.lastFrame-request.firstFrame)%request.step?1u:0u);
        if(progress&&!progress(0,request.lastFrame)){result.error=Error::Cancelled;return result;}
        WorldCopy world(*app.Physics()->world,request.settings);
        std::array<unsigned char*,kModelSlotCount> slots{};
        std::vector<std::unique_ptr<ModelCopy>> models;
        struct Target {unsigned char* live;ModelCopy* copy;std::vector<int> bones;std::unique_ptr<KeyBuilder> builder;};
        std::vector<Target> targets;
        for(int i=0;i<kModelSlotCount;++i) if(auto* raw=app.ModelSlot(i)) {
            models.emplace_back(new ModelCopy(*mdl::Mdl(raw),request.preview)); auto* copy=models.back().get();
            slots[i]=copy->raw();world.Connect(*copy,*mdl::Mdl(raw),request.settings);
            if(wholeScene||raw==selected){auto& source=*mdl::Mdl(raw);
                for(int index:request.boneIndices)if(index<0||unsigned(index)>=source.boneCount){result.error=Error::InvalidRequest;return result;}
                auto bones=Targets(source,request);if(bones.empty())continue;
                if(frames>mdl::kBoneKeyCapacityLimit/bones.size()){result.error=Error::Capacity;return result;}
                auto builder=std::make_unique<KeyBuilder>(source);
                if(request.step>1)for(int bone:bones)builder->EraseRange(bone,unsigned(request.firstFrame),unsigned(request.lastFrame));
                targets.push_back({raw,copy,std::move(bones),std::move(builder)});
            }
        }
        if(targets.empty()){result.error=Error::NoBones;return result;}
        Evaluate(models,slots,0,true);
        for(auto& m:models){ModelDynamicReseat(m->raw());ModelKinematicSync(m->raw());}
        for(auto& m:models)for(auto& r:m->rigids)if(auto* b=static_cast<btRigidBody*>(r.body)) {
            if(r.mode==0)b->getMotionState()->getWorldTransform(b->getWorldTransform());
            else b->getMotionState()->setWorldTransform(b->getWorldTransform());
            b->setInterpolationWorldTransform(b->getWorldTransform());b->setLinearVelocity(btVector3(0,0,0));b->setAngularVelocity(btVector3(0,0,0));
        }
        world.world.updateAabbs();Readback(models,slots);
        const int ticks=request.settings.baseRateHz/30*request.settings.rateMultiplier;
        const int subdivisions=request.settings.substeps;
        const float dt=1.f/(request.settings.baseRateHz*request.settings.rateMultiplier*subdivisions);
        const int finalFrame=request.lastFrame+(request.physics?1:0);
        BakePreview preview(&app);
        for(std::int64_t frame=0;frame<=finalFrame;++frame) {
            if(frame>0)for(int tick=0;tick<ticks;++tick)for(int sub=0;sub<subdivisions;++sub) {
                const double seconds=(frame-1)/30.0+(tick*subdivisions+sub+1)/static_cast<double>(request.settings.baseRateHz*request.settings.rateMultiplier*subdivisions);
                Evaluate(models,slots,seconds,false);
                for(auto& m:models)ModelKinematicSync(m->raw());
                SetGravity(app,world.world,seconds*30);
                world.world.stepSimulation(dt,1,dt);++result.simulationSteps;
                Readback(models,slots);
            }
            const bool inside=frame>=request.firstFrame&&frame<=request.lastFrame;
            const bool sample=inside&&((frame-request.firstFrame)%request.step==0||frame==request.lastFrame);
            const bool guard=request.physics&&(frame==request.firstFrame-1 || frame==request.lastFrame+1);
            if(sample||guard)for(auto& entry:targets){auto* target=entry.copy;auto& source=*mdl::Mdl(entry.live);auto& builder=*entry.builder;
            for(int bone:entry.bones) {
                const auto& b=target->bones[bone];
                const bool physical=request.physics&&DynamicBone(source,bone);
                if(guard&&!physical)continue;
                const bool simulated=inside&&physical&&!b.physicsDisabled;
                float p[3],q[4];
                std::memcpy(p,simulated?b.ikBackup:b.trans,sizeof p);std::memcpy(q,simulated?b.ikBackup+3:b.rotQuat,sizeof q);
                RepairBonePose(p,q,b.trans,b.rotQuat);
                if(!Finite(p,3)||!Finite(q,4))throw SceneError{};
                if(!builder.Put(bone,frame,p,q,inside&&physical?1:b.physicsDisabled,guard,
                    simulated ? bone_scale::Pose(target->raw(),bone) : bone_scale::Sample(entry.live,bone,frame))){result.error=Error::Capacity;return result;}
            }
            }
            if(request.preview&&sample&&preview.Due(frame==request.lastFrame)){
                // Render the completed private pose. Restore all simulation
                // bones and live slot identities even if a renderer fails.
                struct Restore {
                    MMDApp& app;std::array<unsigned char*,kModelSlotCount> live{};
                    int frame;double cursor;decltype(app.state.messageSeen) seen;
                    std::vector<std::pair<ModelCopy*,std::vector<mdl::BoneRecord>>> bones;
                    explicit Restore(MMDApp& a):app(a),frame(a.CurrentFrame()),cursor(a.PlaybackCursorSeconds()),seen(a.state.messageSeen){
                        for(int i=0;i<kModelSlotCount;++i)live[i]=app.ModelSlot(i);
                    }
                    ~Restore(){for(auto& entry:bones)std::copy(entry.second.begin(),entry.second.end(),entry.first->bones.begin());
                        for(int i=0;i<kModelSlotCount;++i)app.ModelSlot(i)=live[i];
                        app.CurrentFrame()=frame;app.PlaybackCursorSeconds()=cursor;app.state.messageSeen=seen;}
                } restore(app);
                for(auto& model:models)restore.bones.emplace_back(model.get(),model->bones);
                for(int i=0;i<kModelSlotCount;++i)app.ModelSlot(i)=slots[i];
                app.CurrentFrame()=frame;app.PlaybackCursorSeconds()=frame/30.0;app.state.messageSeen=1;
                preview.RenderNative();
            }
            if(progress&&!progress(frame,finalFrame)){result.error=Error::Cancelled;return result;}
        }
        const auto simulationSteps=result.simulationSteps;auto& batch=result;
        for(auto& entry:targets){auto& source=*mdl::Mdl(entry.live);auto& builder=*entry.builder;
        Result output;auto& result=output;
        // The builder may have grown past the live pool, so every slot past the
        // live capacity is compared against the empty key record.  That makes a
        // key in a fresh slot a change like any other, which is what lets
        // bake::Commit carry it into the undo entry and restore it as empty.
        const std::size_t liveCapacity=mdl::BoneKeyCapacity(source);
        const mdl::BoneKey empty{};
        std::vector<bool> changed(builder.keys.size());
        for(unsigned i=0;i<builder.keys.size();++i) {
            const mdl::BoneKey& live=i<liveCapacity?source.boneKeys[i]:empty;
            if(std::memcmp(&builder.keys[i],&live,sizeof(mdl::BoneKey)) ||
               (builder.scales.count(int(i)) && builder.scales.at(int(i))!=bone_scale::Key(entry.live,int(i)))) {
                result.changes.push_back({static_cast<int>(i),builder.keys[i]});
                if(!builder.keys[i].frame&&i>=source.boneCount)result.scales[int(i)]={};
                changed[i]=true;
            }
        }
        // Simulation can resolve parent deformation into a new local stretch.
        // Save that sampled stretch alongside the corresponding rigid pose.
        for(int bone:entry.bones) {
            unsigned slot=bone,guard=0;
            do {
                if(slot>=builder.keys.size()||++guard>builder.keys.size())throw SceneError{};
                if(changed[slot]) {
                    auto scale=builder.scales.find(int(slot));
                    result.scales[int(slot)]=scale==builder.scales.end() ? bone_scale::Sample(entry.live,bone,builder.keys[slot].frame) : scale->second;
                }
                slot=builder.keys[slot].next;
            } while(slot);
        }
        result.maxFrame=builder.maxFrame;result.boneCount=static_cast<int>(entry.bones.size());result.sampledFrames=static_cast<int>(frames);
        output.simulationSteps=simulationSteps;
        batch.models.push_back({entry.live,std::move(output)});}
    }catch(const std::bad_alloc&){result.error=Error::Allocation;result.models.clear();}
    catch(...){result.error=Error::InvalidScene;result.models.clear();}
    return result;
}

Result Compute(MMDApp& app,const Request& request,const Progress& progress){
    auto batch=ComputeBatch(app,request,false,progress);
    if(batch.error!=Error::None){Result out;out.error=batch.error;return out;}
    return std::move(batch.models.front().result);
}
SceneResult ComputeScene(MMDApp& app,const Request& request,const Progress& progress){
    Request scene=request;scene.physics=true;scene.keyframes=false;scene.selectedOnly=false;
    scene.boneCoverage=channel_bake::Coverage::Whole;scene.boneIndices.clear();
    return ComputeBatch(app,scene,true,progress);
}

// ---------------------------------------------------------------------------
// MMV layer cutoff bake
// ---------------------------------------------------------------------------
// The layered path never runs its own physics and never recomposes the layer
// chain: every pose comes from the cutoff layer's OUTPUT snapshot, mapped back
// to the host bones through sourceKey identity.  Expressibility checks follow
// .local/artifacts/mmv-layer-bake/cutoff-cache-plan.md and the host's real
// skinning (affine_skinning.hpp), so a legal SDEF/QDEF result is never judged
// by a plain linear blend.
namespace {
bool layerPhysicsSuppressed=false;

// Decodes an original item key "<objectKey>/<type>/<sourceId>" and re-encodes
// it to prove the round trip.  Identity is the stable key; bone names and array
// positions are never used to guess a mapping.
bool LayerOriginalKeyId(const std::string& key,const std::string& objectKey,
                        const char* type,std::uint32_t& id) {
    const std::string head=objectKey+"/"+type+"/";
    if(key.size()<=head.size()||key.compare(0,head.size(),head)!=0)return false;
    const std::string local=key.substr(head.size());
    if(local.empty()||local.find('/')!=std::string::npos)return false;
    try{
        std::size_t used=0;
        const unsigned long value=std::stoul(local,&used);
        if(used!=local.size()||value>0xfffffffful)return false;
        id=static_cast<std::uint32_t>(value);
    }catch(...){return false;}
    return head+std::to_string(id)==key;
}

// UTF-16 -> UTF-8 for the morph name round trip (the snapshot stores the name
// the host itself supplied in UTF-8).
std::string LayerUtf8(const std::wstring& text) {
    std::string out;
    for(std::size_t i=0;i<text.size();++i){
        unsigned code=text[i]&0xffffu;
        if(code>=0xd800u&&code<0xdc00u&&i+1<text.size()){
            const unsigned low=text[i+1]&0xffffu;
            if(low>=0xdc00u&&low<0xe000u){code=0x10000u+((code-0xd800u)<<10)+(low-0xdc00u);++i;}
        }
        if(code<0x80u)out.push_back(char(code));
        else if(code<0x800u){out.push_back(char(0xc0u|(code>>6)));out.push_back(char(0x80u|(code&0x3fu)));}
        else if(code<0x10000u){out.push_back(char(0xe0u|(code>>12)));out.push_back(char(0x80u|((code>>6)&0x3fu)));out.push_back(char(0x80u|(code&0x3fu)));}
        else{out.push_back(char(0xf0u|(code>>18)));out.push_back(char(0x80u|((code>>12)&0x3fu)));out.push_back(char(0x80u|((code>>6)&0x3fu)));out.push_back(char(0x80u|(code&0x3fu)));}
    }
    return out;
}

// Missing-bone precomputation, identical to the production skinning tables.
constexpr affine::BoneAux kLayerMissingBoneAux = {
    {1.0f,0.0f,0.0f,0.0f,1.0f,0.0f,0.0f,0.0f,1.0f},
    {0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f},
    {0.5f,0.0f,0.0f,0.0f},
    0u,
};
const float kLayerZeroMatrix[16]={};
}

void SetLayerPhysicsSuppressed(bool suppressed){layerPhysicsSuppressed=suppressed;}
bool LayerPhysicsSuppressed(){return layerPhysicsSuppressed;}

KeyPreImage CaptureKeys(unsigned char* model,const Result& result) {
    KeyPreImage image;
    if(!model)return image;
    auto& m=*mdl::Mdl(model);
    if(!m.boneKeys)return image;
    const std::size_t capacity=mdl::BoneKeyCapacity(m);
    try{
        image.keys.reserve(result.changes.size());
        for(const auto& change:result.changes){
            if(change.keyIndex<0){image.keys.clear();return image;}
            const std::size_t slot=std::size_t(change.keyIndex);
            image.keys.push_back({change.keyIndex,slot<capacity?m.boneKeys[slot]:mdl::BoneKey{}});
        }
    }catch(...){image.keys.clear();return image;}
    image.maxFrame=m.maxFrame;
    image.valid=true;
    return image;
}

const wchar_t* LayerErrorText(MMDApp& app,LayerError error) {
    const bool chinese=app.EnglishUI()==2,english=app.EnglishUI()==1;
    switch(error){
    case LayerError::None:return L"";
    case LayerError::NotSynchronized:return chinese?
        L"该层输出是 MeshOnly（结构与输出网格不同步），只能烘焙网格，不能烘焙骨骼/物理/表情关键帧。":
        english?L"This layer output is MeshOnly (structure not synchronized with the mesh): it can be baked as a mesh but never as bone, physics or morph keys."
               :L"この層の出力は MeshOnly（構造非同期）です。メッシュのみベイクでき、ボーン・物理・モーフのキーにはベイクできません。";
    case LayerError::GeneratedItem:return chinese?
        L"该层生成了没有唯一来源骨骼的结构，无法忠实写入原生关键帧。请改用网格烘焙。":
        english?L"This layer generated structure without a unique source bone, which the native key pools cannot represent. Use the mesh bake instead."
               :L"この層は一意な元ボーンを持たない構造を生成しました。ネイティブキーには保存できません。メッシュのベイクを使ってください。";
    case LayerError::UnmappedBone:return chinese?
        L"该层的骨骼来源键无法定位宿主骨骼，不按同名或序号猜测对应。请改用网格烘焙。":
        english?L"A layer bone source key does not locate a host bone; names and indices are never used to guess a mapping. Use the mesh bake instead."
               :L"層ボーンの来源キーがホストのボーンを特定できません。名前や番号での推定は行いません。メッシュのベイクを使ってください。";
    case LayerError::AmbiguousBone:return chinese?
        L"两个层骨骼指向同一个宿主骨骼，无法一一对应写入原生关键帧。请改用网格烘焙。":
        english?L"Two layer bones claim the same host bone, so the native keys cannot be written 1:1. Use the mesh bake instead."
               :L"2 つの層ボーンが同じホストボーンを指しており、1対1でキーを書けません。メッシュのベイクを使ってください。";
    case LayerError::MissingBone:return chinese?
        L"宿主骨骼在该层输出中没有对应骨骼，删除/改动结构无法写成原生关键帧。请改用网格烘焙。":
        english?L"A host bone has no counterpart in the layer output; removed or changed structure cannot be written as native keys. Use the mesh bake instead."
               :L"ホストのボーンに対応する層ボーンがありません。構造変更はキーにできません。メッシュのベイクを使ってください。";
    case LayerError::BindingChanged:return chinese?
        L"该层重绑了权重或绑定几何，无法写入原生关键帧。请改用网格烘焙。":
        english?L"This layer re-bound the weights or binding geometry, which native keys cannot represent. Use the mesh bake instead."
               :L"この層はウェイトまたはバインド形状を再構成しました。キーには保存できません。メッシュのベイクを使ってください。";
    case LayerError::ExtraMeshDeform:return chinese?
        L"该层网格不只是骨骼/表情的原生蒙皮结果，含额外网格变形。取消 FX 会丢失该变形，已拒绝；请改用或配合网格烘焙。":
        english?L"The layer mesh is not just the native skinning of the bone/morph state: it carries extra mesh deformation that removing the FX would lose. Use the mesh bake instead."
               :L"この層のメッシュはボーン・モーフのスキニング結果ではなく、追加のメッシュ変形を含みます。FX を外すと失われるため中止しました。メッシュのベイクを使ってください。";
    case LayerError::NoDeformedGeometry:return chinese?
        L"快照缺少蒙皮校验所需的几何数据，无法证明网格可由关键帧复现；为免丢数据已拒绝。请改用网格烘焙。":
        english?L"The snapshot lacks the geometry needed to prove the mesh is reproducible from keys; the bake was refused rather than risking data loss. Use the mesh bake instead."
               :L"スナップショットに検証用の形状データがなく、キーで再現できることを証明できません。データ喪失を避けるため中止しました。";
    case LayerError::MissingObject:return chinese?
        L"采样到的层输出不含被烘焙对象，不以回退来源冒充成功输出。":
        english?L"The sampled layer output does not contain the baked object; a fallback source is never shown as a successful output."
               :L"サンプリングした層出力に対象オブジェクトがありません。代替ソースを成功出力として扱いません。";
    default:return chinese?
        L"层输出数据无效，烘焙已拒绝。":
        english?L"The layer output data is invalid; the bake was refused."
               :L"層出力のデータが無効です。ベイクを中止しました。";
    }
}

LayerError MapLayerPose(MMDApp& app,unsigned char* model,const native_mesh::LayerObject& object,
                        LayerPoseFrame& out,std::wstring& message) {
    out=LayerPoseFrame{};
    if(!model){message=LayerErrorText(app,LayerError::InvalidData);return LayerError::InvalidData;}
    auto& m=*mdl::Mdl(model);
    if(!object.structureSynchronized){
        message=LayerErrorText(app,LayerError::NotSynchronized);return LayerError::NotSynchronized;}
    out.structureSynchronized=true;
    out.boneWorld.assign(std::size_t(m.boneCount)*16,0.f);
    out.boneValid.assign(m.boneCount,0);
    out.morphValues.assign(m.morphCount,0.f);
    out.morphValid.assign(m.morphCount,0);
    std::vector<unsigned char> claimed(m.boneCount,0);
    for(const auto& bone:object.bones){
        std::uint32_t index=0;
        if(bone.sourceKey.empty()){
            message=LayerErrorText(app,LayerError::GeneratedItem);return LayerError::GeneratedItem;}
        if(!LayerOriginalKeyId(bone.sourceKey,object.key,"bone",index)){
            message=LayerErrorText(app,LayerError::UnmappedBone);return LayerError::UnmappedBone;}
        if(index>=m.boneCount){
            message=LayerErrorText(app,LayerError::UnmappedBone);return LayerError::UnmappedBone;}
        if(claimed[index]){
            message=LayerErrorText(app,LayerError::AmbiguousBone);return LayerError::AmbiguousBone;}
        claimed[index]=1;
        std::memcpy(&out.boneWorld[std::size_t(index)*16],bone.world,sizeof(float)*16);
        out.boneValid[index]=1;
    }
    for(unsigned b=0;b<m.boneCount;++b)if(!claimed[b]){
        message=LayerErrorText(app,LayerError::MissingBone);return LayerError::MissingBone;}
    // The snapshot carries no morph identity key, so the morph name the host
    // itself supplied is matched back with a uniqueness check; a duplicate or
    // missing entry leaves that morph unmapped and the caller refuses it.
    std::map<std::string,std::vector<std::uint32_t>> byName;
    for(std::uint32_t i=0;i<object.morphs.size();++i)byName[object.morphs[i].name].push_back(i);
    for(unsigned morph=0; morph<m.morphCount; ++morph){
        const auto found=byName.find(LayerUtf8(text_encoding::MorphName(m.morphs[morph],false)));
        if(found==byName.end()||found->second.size()!=1)continue;
        out.morphValues[morph]=object.morphs[found->second[0]].value;
        out.morphValid[morph]=1;
    }
    return LayerError::None;
}

// Everything a bone/morph key bake cannot carry must equal the SOURCE stage:
// UVs, edge data, materials, topology and visibility.  Only position and
// normal may differ, and only by exactly what the host's own skinning does.
bool LayerNonSkinChannelsEqual(const native_mesh::LayerObject& output,
                               const native_mesh::LayerObject& source, std::wstring& message) {
    static const mesh_data::MeshView kEmpty{};
    const mesh_data::MeshView& out = output.mesh ? *output.mesh : kEmpty;
    const mesh_data::MeshView& src = source.mesh ? *source.mesh : kEmpty;
    if (output.visible != source.visible) {
        message = L"The layer changed object visibility."; return false;
    }
    if (out.vertexCount != src.vertexCount || out.triangleCount != src.triangleCount ||
        out.materialCount != src.materialCount) {
        message = L"The layer changed the mesh size."; return false;
    }
    for (std::uint32_t v = 0; v < out.vertexCount; ++v) {
        const auto& x = out.vertices[v];
        const auto& y = src.vertices[v];
        if (x.sourceVertex != y.sourceVertex || x.flags != y.flags || x.edgeScale != y.edgeScale ||
            std::memcmp(x.uv, y.uv, sizeof x.uv) ||
            std::memcmp(x.additionalUv, y.additionalUv, sizeof x.additionalUv)) {
            message = L"The layer changed UV, edge or source-vertex data."; return false;
        }
    }
    for (std::uint32_t t = 0; t < out.triangleCount; ++t) {
        const auto& x = out.triangles[t];
        const auto& y = src.triangles[t];
        if (x.material != y.material || std::memcmp(x.vertex, y.vertex, sizeof x.vertex)) {
            message = L"The layer changed the topology or material assignment."; return false;
        }
    }
    const auto sameTexture = [](const mesh_data::TextureView& x, const mesh_data::TextureView& y) {
        const bool xPath = x.path && *x.path, yPath = y.path && *y.path;
        if (xPath != yPath) return false;
        if (xPath && wcscmp(x.path, y.path) != 0) return false;
        if ((x.rgba != nullptr) != (y.rgba != nullptr) || x.width != y.width || x.height != y.height)
            return false;
        return !x.rgba || !x.width || !x.height ||
               std::memcmp(x.rgba, y.rgba, std::size_t(x.width) * x.height * 4 * sizeof(float)) == 0;
    };
    for (std::uint32_t m = 0; m < out.materialCount; ++m) {
        const mesh_data::MaterialView& x = out.materials[m];
        const mesh_data::MaterialView& y = src.materials[m];
        const std::string xKey = x.key ? x.key : "", yKey = y.key ? y.key : "";
        const std::string xName = x.name ? x.name : "", yName = y.name ? y.name : "";
        const std::string xNameEn = x.nameEn ? x.nameEn : "", yNameEn = y.nameEn ? y.nameEn : "";
        if (xKey != yKey || xName != yName || xNameEn != yNameEn || x.sourceMaterial != y.sourceMaterial ||
            x.flags != y.flags || x.sphereMode != y.sphereMode ||
            std::memcmp(x.diffuse, y.diffuse, sizeof x.diffuse) ||
            std::memcmp(x.ambient, y.ambient, sizeof x.ambient) ||
            std::memcmp(x.specular, y.specular, sizeof x.specular) ||
            std::memcmp(x.edgeColor, y.edgeColor, sizeof x.edgeColor) ||
            x.power != y.power || x.edgeSize != y.edgeSize ||
            !sameTexture(x.texture, y.texture) || !sameTexture(x.sphere, y.sphere) ||
            !sameTexture(x.toon, y.toon)) {
            message = L"The layer changed a material."; return false;
        }
    }
    return true;
}

LayerError CheckLayerMeshCacheExpressible(const native_mesh::LayerObject& output,
                                          const native_mesh::LayerObject& source,
                                          std::wstring& message) {
    message.clear();
    static const mesh_data::MeshView kEmpty{};
    const mesh_data::MeshView& out = output.mesh ? *output.mesh : kEmpty;
    const mesh_data::MeshView& src = source.mesh ? *source.mesh : kEmpty;
    if (output.visible != source.visible) {
        message = L"The layer changed object visibility, which the mesh cache cannot store.";
        return LayerError::ExtraMeshDeform;
    }
    if (out.materialCount != src.materialCount) {
        message = L"The layer generated or removed a material, which the mesh cache cannot store.";
        return LayerError::ExtraMeshDeform;
    }
    // The cache stores no material parameters at all: replay rebuilds every
    // material from the live source object, so every material the layers
    // changed would be lost with the removed FX.
    const auto sameTexture = [](const mesh_data::TextureView& x, const mesh_data::TextureView& y) {
        const bool xPath = x.path && *x.path, yPath = y.path && *y.path;
        if (xPath != yPath) return false;
        if (xPath && wcscmp(x.path, y.path) != 0) return false;
        if ((x.rgba != nullptr) != (y.rgba != nullptr) || x.width != y.width || x.height != y.height)
            return false;
        return !x.rgba || !x.width || !x.height ||
               std::memcmp(x.rgba, y.rgba, std::size_t(x.width) * x.height * 4 * sizeof(float)) == 0;
    };
    for (std::uint32_t m = 0; m < out.materialCount; ++m) {
        const mesh_data::MaterialView& x = out.materials[m];
        const mesh_data::MaterialView& y = src.materials[m];
        const std::string xKey = x.key ? x.key : "", yKey = y.key ? y.key : "";
        const std::string xName = x.name ? x.name : "", yName = y.name ? y.name : "";
        const std::string xNameEn = x.nameEn ? x.nameEn : "", yNameEn = y.nameEn ? y.nameEn : "";
        if (xKey != yKey || xName != yName || xNameEn != yNameEn || x.sourceMaterial != y.sourceMaterial ||
            x.flags != y.flags || x.sphereMode != y.sphereMode ||
            std::memcmp(x.diffuse, y.diffuse, sizeof x.diffuse) ||
            std::memcmp(x.ambient, y.ambient, sizeof x.ambient) ||
            std::memcmp(x.specular, y.specular, sizeof x.specular) ||
            std::memcmp(x.edgeColor, y.edgeColor, sizeof x.edgeColor) ||
            x.power != y.power || x.edgeSize != y.edgeSize ||
            !sameTexture(x.texture, y.texture) || !sameTexture(x.sphere, y.sphere) ||
            !sameTexture(x.toon, y.toon)) {
            message = L"The layer changed a material, which the mesh cache cannot store.";
            return LayerError::ExtraMeshDeform;
        }
    }
    // Per render vertex: positions, normals and uv[2] are recorded, everything
    // else is rebuilt from the source row the vertex maps to.  The check is by
    // source-vertex identity, so a reordered mesh is judged correctly.
    for (std::uint32_t v = 0; v < out.vertexCount; ++v) {
        const auto& x = out.vertices[v];
        if (x.sourceVertex == 0xffffffffu) {
            // A generated render vertex has no source row: replay can only give
            // it the neutral values the cache cannot record.
            if (x.flags != 0 || x.edgeScale != 1.0f) {
                message = L"A generated vertex carries edge or flag data, which the mesh cache cannot store.";
                return LayerError::ExtraMeshDeform;
            }
            for (int k = 0; k < 16; ++k) if (x.additionalUv[k] != 0.0f) {
                message = L"A generated vertex carries additional UVs, which the mesh cache cannot store.";
                return LayerError::ExtraMeshDeform;
            }
            continue;
        }
        if (x.sourceVertex >= src.vertexCount) {
            message = L"A layer vertex maps to a source vertex the object does not have.";
            return LayerError::ExtraMeshDeform;
        }
        const auto& y = src.vertices[x.sourceVertex];
        if (x.flags != y.flags || x.edgeScale != y.edgeScale ||
            std::memcmp(x.additionalUv, y.additionalUv, sizeof x.additionalUv)) {
            message = L"The layer changed additional UV, edge or flag data, which the mesh cache cannot store.";
            return LayerError::ExtraMeshDeform;
        }
    }
    return LayerError::None;
}

LayerError CheckLayerMeshIsNativeSkin(const native_mesh::LayerObject& object,
                                      const native_mesh::LayerObject& source, std::wstring& message) {
    message.clear();
    if(!object.mesh){message=L"The layer output carries no mesh.";return LayerError::InvalidData;}
    // UVs, materials, topology and visibility are not expressible as bone or
    // morph keys; when the layers changed any of them the bake is refused
    // instead of losing that change with the removed FX.
    if(!LayerNonSkinChannelsEqual(object,source,message))return LayerError::ExtraMeshDeform;
    const mesh_data::MeshView& mesh=*object.mesh;
    if(mesh.vertexCount!=std::uint32_t(object.vertexBindings.size())){
        message=L"The layer mesh and its vertex bindings disagree.";return LayerError::InvalidData;}    // Per-bone affine precomputation, exactly like the production skinning's
    // BuildPmxBoneAux, with the header's reference rotation ops (this path has
    // no D3DX).  The layer skin matrix is the host matInit equivalent:
    // world = T(origin) * matInit, so T(-origin) * world is the matrix the host
    // itself skins with after the keys are baked.
    std::vector<affine::BoneAux> aux(object.bones.size());
    for(std::size_t b=0;b<object.bones.size();++b){
        float rotation[4];
        affine::DefaultQuatFromMatrix(rotation,object.bones[b].skinMatrix);
        affine::PrecomputeBone(object.bones[b].skinMatrix,rotation,aux[b]);
    }
    const auto refFor=[&](std::int32_t index)->affine::BoneRef{
        if(index>=0&&std::size_t(index)<object.bones.size())
            return {object.bones[std::size_t(index)].skinMatrix,&aux[std::size_t(index)]};
        return {kLayerZeroMatrix,&kLayerMissingBoneAux};
    };
    const auto operations=affine::DefaultRotationOps();
    for(std::uint32_t v=0;v<mesh.vertexCount;++v){
        const std::uint32_t row=object.vertexBindings[v];
        if(row==0xffffffffu||row>=object.bindings.size()){
            message=L"A render vertex has no layer binding.";return LayerError::NoDeformedGeometry;}
        const auto& bind=object.bindings[row];
        if(!bind.deformedValid){
            message=L"The snapshot carried no morph-deformed geometry.";return LayerError::NoDeformedGeometry;}
        affine::Skinned skinned;
        switch(bind.weightType){
        case 0:affine::SkinBdef1(refFor(bind.bones[0]),bind.deformedPosition,bind.deformedNormal,skinned);break;
        case 1:affine::SkinBdef2(refFor(bind.bones[0]),refFor(bind.bones[1]),bind.weights[0],
                                 bind.deformedPosition,bind.deformedNormal,skinned);break;
        case 2:{const affine::BoneRef refs[4]={refFor(bind.bones[0]),refFor(bind.bones[1]),
                                               refFor(bind.bones[2]),refFor(bind.bones[3])};
            affine::SkinBdef4(refs,bind.weights,bind.deformedPosition,bind.deformedNormal,skinned);break;}
        case 3:affine::SkinSdef(refFor(bind.bones[0]),refFor(bind.bones[1]),bind.weights[0],
                                bind.deformedPosition,bind.deformedNormal,bind.sdefCenter,
                                bind.sdefR0,bind.sdefR1,operations,skinned);break;
        default:
            // QDEF and unknown weight types: the host skinning leaves position
            // and normal untouched (model_skinning.cpp dispatch default), so
            // the native result is the untransformed input itself.  No new
            // QDEF behaviour is invented here.
            std::memcpy(skinned.position,bind.deformedPosition,sizeof skinned.position);
            std::memcpy(skinned.normal,bind.deformedNormal,sizeof skinned.normal);
            break;
        }
        const auto& expected=mesh.vertices[v].position;
        const auto& expectedNormal=mesh.vertices[v].normal;
        for(int c=0;c<3;++c){
            const float tolerance=1e-3f+1e-3f*std::fabs(expected[c]);
            if(!(std::fabs(skinned.position[c]-expected[c])<=tolerance)){
                message=L"The layer mesh is not the native skinning result of its own state.";
                return LayerError::ExtraMeshDeform;}
            // The normal runs through the same kernel (and the same affine
            // normal matrix) the production skinning uses.
            const float normalTolerance=1e-2f+1e-2f*std::fabs(expectedNormal[c]);
            if(!(std::fabs(skinned.normal[c]-expectedNormal[c])<=normalTolerance)){
                message=L"The layer mesh normals are not the native skinning result of their own state.";
                return LayerError::ExtraMeshDeform;}
        }
    }
    return LayerError::None;
}

LayerDifference CompareLayerStages(const native_mesh::LayerObject& output,
                                   const native_mesh::LayerObject& source) {
    LayerDifference difference;
    if(output.bones.size()!=source.bones.size()||output.morphs.size()!=source.morphs.size()||
       output.rigids.size()!=source.rigids.size()||
       output.structureVersion!=source.structureVersion){
        // An added or removed item is a structural change; the arrays no longer
        // line up, so the pose and morph channels count as changed too and the
        // per-item comparison below is skipped instead of reading past the end
        // of the shorter side.
        difference.structure=true;
        difference.pose=true;
        difference.morphs=true;
    }
    if(output.vertexBindings!=source.vertexBindings||output.bindings.size()!=source.bindings.size())
        difference.binding=true;
    else for(std::size_t i=0;i<output.bindings.size();++i){
        const auto& a=output.bindings[i];const auto& b=source.bindings[i];
        if(a.weightType!=b.weightType||std::memcmp(a.bones,b.bones,sizeof a.bones)||
           std::memcmp(a.weights,b.weights,sizeof a.weights)||
           std::memcmp(a.bindPosition,b.bindPosition,sizeof a.bindPosition)||
           std::memcmp(a.bindNormal,b.bindNormal,sizeof a.bindNormal)||
           std::memcmp(a.sdefCenter,b.sdefCenter,sizeof a.sdefCenter)||
           std::memcmp(a.sdefR0,b.sdefR0,sizeof a.sdefR0)||
           std::memcmp(a.sdefR1,b.sdefR1,sizeof a.sdefR1)){
            difference.binding=true;break;}
    }
    for(std::size_t i=0;i<output.bones.size()&&!difference.pose;++i)
        if(output.bones[i].valid!=source.bones[i].valid||
           std::memcmp(output.bones[i].world,source.bones[i].world,sizeof(float)*16))
            difference.pose=true;
    for(std::size_t i=0;i<output.morphs.size()&&!difference.morphs;++i)
        if(output.morphs[i].value!=source.morphs[i].value)difference.morphs=true;
    return difference;
}

Result ComputeLayered(MMDApp& app,const Request& request,const native_mesh::LayerSampling& sampling,
                      std::vector<LayerPoseFrame>& frames,const Progress& progress) {
    Result result;
    frames.clear();
    if(!sampling.Active()||request.firstFrame<0||request.lastFrame<request.firstFrame||
       request.lastFrame>frame_limits::maximum||request.step<1||request.step>frame_limits::maximum||
       (!request.physics&&!request.keyframes)){result.error=Error::InvalidRequest;return result;}
    auto* selected=app.SelectedModel();
    if(!selected){result.error=Error::NoModel;return result;}
    try{
        auto& source=*mdl::Mdl(selected);
        for(int index:request.boneIndices)
            if(index<0||unsigned(index)>=source.boneCount){result.error=Error::InvalidRequest;return result;}
        const auto targetBones=Targets(source,request);
        if(targetBones.empty()){result.error=Error::NoBones;return result;}
        const std::uint64_t samples=1u+unsigned(request.lastFrame-request.firstFrame)/unsigned(request.step)+
            ((request.lastFrame-request.firstFrame)%request.step?1u:0u);
        if(samples>mdl::kBoneKeyCapacityLimit/targetBones.size()){result.error=Error::Capacity;return result;}
        if(progress&&!progress(0,request.lastFrame)){result.error=Error::Cancelled;return result;}
        // The physics boundary keys of the non-layered bake extend the sampled
        // range by one frame on each side.
        const int firstGuard=std::max(0,request.firstFrame-1);
        const int lastGuard=std::min(frame_limits::maximum,request.lastFrame+1);
        // An MMV solver that owns the physics (native physics switched off by
        // the solver takeover) must never be advanced a second time by this
        // offline world; with no solver active the world steps as before, and
        // its first step always follows one freshly evaluated animation input.
        struct Suppression {
            bool previous;
            explicit Suppression(bool on):previous(layerPhysicsSuppressed){layerPhysicsSuppressed=on;}
            ~Suppression(){layerPhysicsSuppressed=previous;}
        } suppression(false);
        const std::uint64_t objectId=reinterpret_cast<std::uint64_t>(selected);
        std::string samplingError;
        LayerError layerFailure=LayerError::None;
        std::wstring layerMessage;
        bool cancelled=false;
        BakePreview preview(&app);
        const bool complete=scene_sampling::Visit(
            app,firstGuard,lastGuard,30,
            [&](double seconds,bool output){
                const int frame=int(std::floor(seconds*30+1e-6));
                if(progress&&!progress(frame,request.lastFrame)){cancelled=true;return false;}
                RenderFrameScene(&app);
                if(!output)return true;
                native_mesh::LayerOutput layerOutput,layerSource;
                std::string queryError;
                if(!sampling.Query(layerOutput,queryError,&layerSource))throw std::runtime_error(queryError);
                const native_mesh::LayerObject* object=nullptr;
                const native_mesh::LayerObject* sourceStage=nullptr;
                for(const auto& candidate:layerOutput.objects)
                    if(candidate.objectId==objectId){object=&candidate;break;}
                for(const auto& candidate:layerSource.objects)
                    if(candidate.objectId==objectId){sourceStage=&candidate;break;}
                if(!object||!sourceStage){layerFailure=LayerError::MissingObject;
                    layerMessage=LayerErrorText(app,LayerError::MissingObject);return false;}
                // The mesh must be exactly the native skinning result of this
                // state (and every channel the keys cannot carry must be
                // unchanged), or removing the FX would lose the difference.
                std::wstring detail;
                const LayerError skinCheck=CheckLayerMeshIsNativeSkin(*object,*sourceStage,detail);
                if(!request.keepLayerEffects&&skinCheck!=LayerError::None){layerFailure=skinCheck;
                    layerMessage=detail.empty()?LayerErrorText(app,skinCheck):detail;return false;}
                LayerPoseFrame pose;
                const LayerError mapped=MapLayerPose(app,selected,*object,pose,layerMessage);
                if(mapped!=LayerError::None){layerFailure=mapped;return false;}
                pose.frame=frame;
                frames.push_back(std::move(pose));
                if(request.preview&&frame>=request.firstFrame&&frame<=request.lastFrame&&preview.Due(frame==request.lastFrame))preview.Present();
                return true;
            },samplingError);
        if(layerFailure!=LayerError::None){
            result.error=Error::LayerRefused;result.layerError=layerFailure;result.layerMessage=layerMessage;
            return result;}
        if(!complete||cancelled){
            result.error=cancelled||progress&&!progress(request.lastFrame,request.lastFrame)
                ?Error::Cancelled:Error::InvalidScene;
            return result;}
        if(frames.empty()){result.error=Error::InvalidScene;return result;}
        // Every sampled pose becomes a native key exactly where the non-layered
        // bake writes one; the pose source is the layer OUTPUT, nothing else.
        KeyBuilder builder(source);
        if(request.step>1)for(int bone:targetBones)
            builder.EraseRange(bone,unsigned(request.firstFrame),unsigned(request.lastFrame));
        for(const auto& pose:frames){
            const bool inside=pose.frame>=request.firstFrame&&pose.frame<=request.lastFrame;
            const bool sample=inside&&((pose.frame-request.firstFrame)%request.step==0||
                                       pose.frame==request.lastFrame);
            const bool guard=request.physics&&(pose.frame==request.firstFrame-1||
                                               pose.frame==request.lastFrame+1);
            if(!sample&&!guard)continue;
            for(int bone:targetBones){
                const bool physical=request.physics&&DynamicBone(source,bone);
                if(guard&&!physical)continue;
                if(!pose.boneValid[bone]){
                    result.error=Error::LayerRefused;result.layerError=LayerError::InvalidData;
                    result.layerMessage=LayerErrorText(app,LayerError::InvalidData);return result;}
                float p[3],q[4];
                std::memcpy(p,&pose.boneWorld[std::size_t(bone)*16+12],sizeof p);
                affine::DefaultQuatFromMatrix(q,&pose.boneWorld[std::size_t(bone)*16]);
                RepairBonePose(p,q,source.boneTable[bone].trans,source.boneTable[bone].rotQuat);
                if(!Finite(p,3)||!Finite(q,4))throw SceneError{};
                const unsigned char disabled=physical?1u:
                    (source.bonePhysicsState?source.bonePhysicsState[bone]:0u);
                if(!builder.Put(bone,unsigned(pose.frame),p,q,disabled,guard,
                                bone_scale::Sample(selected,bone,pose.frame))){
                    result.error=Error::Capacity;return result;}
            }
        }
        const std::size_t liveCapacity=mdl::BoneKeyCapacity(source);
        const mdl::BoneKey empty{};
        std::vector<bool> changed(builder.keys.size());
        for(unsigned i=0;i<builder.keys.size();++i){
            const mdl::BoneKey& live=i<liveCapacity?source.boneKeys[i]:empty;
            if(std::memcmp(&builder.keys[i],&live,sizeof(mdl::BoneKey))||
               (builder.scales.count(int(i))&&builder.scales.at(int(i))!=bone_scale::Key(selected,int(i)))){
                result.changes.push_back({static_cast<int>(i),builder.keys[i]});
                if(!builder.keys[i].frame&&i>=source.boneCount)result.scales[int(i)]={};
                changed[i]=true;
            }
        }
        for(int bone:targetBones){
            unsigned slot=bone,guard=0;
            do{
                if(slot>=builder.keys.size()||++guard>builder.keys.size())throw SceneError{};
                if(changed[slot]){
                    auto scale=builder.scales.find(int(slot));
                    result.scales[int(slot)]=scale==builder.scales.end()
                        ?bone_scale::Sample(selected,bone,builder.keys[slot].frame):scale->second;
                }
                slot=builder.keys[slot].next;
            }while(slot);
        }
        result.maxFrame=builder.maxFrame;
        result.boneCount=static_cast<int>(targetBones.size());
        result.sampledFrames=static_cast<int>(samples);
    }catch(const std::bad_alloc&){result.error=Error::Allocation;result.changes.clear();result.scales.clear();}
    catch(...){result.error=Error::InvalidScene;result.changes.clear();result.scales.clear();}
    return result;
}
}

namespace mikumikustudio {
void InitExportPlaybackTracks(MMDApp*, double);
void SampleGlobalPlaybackTracks(MMDApp*);
namespace scene_sampling {
bool Visit(MMDApp& app, int firstFrame, int lastFrame, int fps,
           const std::function<bool(double,bool)>& visitor, std::string& error) {
    using namespace bake;
    if (firstFrame<0 || lastFrame<firstFrame || lastFrame>frame_limits::maximum || fps<1 || fps>120 || !visitor) {
        error="Invalid frame range or sampling rate"; return false;
    }
    if (!app.Physics() || !app.Physics()->world) { error="The physics scene is unavailable"; return false; }
    try {
        const auto settings=BulletDefaults();
        WorldCopy world(*app.Physics()->world,settings);
        std::array<unsigned char*,kModelSlotCount> slots{};
        std::vector<std::unique_ptr<ModelCopy>> models;
        std::vector<unsigned char*> live;
        for(int i=0;i<kModelSlotCount;++i) if(auto* raw=app.ModelSlot(i)) {
            models.emplace_back(new ModelCopy(*mdl::Mdl(raw),true));
            auto& copy=*models.back(); slots[i]=copy.raw();
            world.Connect(copy,*mdl::Mdl(raw),settings);live.push_back(raw);
        }
        struct AppRestore {
            MMDApp& app;
            std::unique_ptr<MMDAppState> saved;
            double cursor, start, end;
            std::vector<std::pair<mdl::AccessoryRecord*,mdl::AccessoryRecord>> accessories;
            explicit AppRestore(MMDApp& a):app(a),saved(new MMDAppState(a.state)), cursor(a.PlaybackCursorSeconds()), start(a.PlaybackStartSeconds()), end(a.PlaybackEndSeconds()) {
                for(int i=0;i<255;++i)if(auto* p=a.AccessorySlot(i)) accessories.emplace_back(p,*p);
            }
            ~AppRestore(){for(auto& a:accessories)*a.first=a.second;app.state=*saved;app.PlaybackCursorSeconds()=cursor;app.PlaybackStartSeconds()=start;app.PlaybackEndSeconds()=end;}
        } restore(app);
        Evaluate(models,slots,0,true);
        for(auto& m:models){ModelDynamicReseat(m->raw());ModelKinematicSync(m->raw());}
        for(auto& m:models)for(auto& r:m->rigids)if(auto* b=static_cast<btRigidBody*>(r.body)) {
            if(r.mode==0)b->getMotionState()->getWorldTransform(b->getWorldTransform());
            else b->getMotionState()->setWorldTransform(b->getWorldTransform());
            b->setInterpolationWorldTransform(b->getWorldTransform());
            b->setLinearVelocity(btVector3(0,0,0));b->setAngularVelocity(btVector3(0,0,0));
        }
        world.world.updateAabbs();Readback(models,slots);
        double time=0;
        const auto render=[&](double target,bool output) {
            while(time+1e-9<target) {
                const double next=std::min(target,time+1.0/60.0);
                for(auto& m:models)bone_scale::RestoreTransforms(m->raw());
                Evaluate(models,slots,next,false);
                for(auto& m:models)ModelKinematicSync(m->raw());
                SetGravity(app,world.world,next*30);
                const float dt=float(next-time);
                // A layered bake with an MMV solver owning the physics must not
                // advance the same bodies a second time here.
                if(!layerPhysicsSuppressed)world.world.stepSimulation(dt,1,dt);
                Readback(models,slots);time=next;
            }
            app.PlaybackCursorSeconds()=target;app.CurrentFrame()=int(std::floor(target*30+1e-6));
            app.PlaybackActive()=1;app.state.optflag[0]=1;app.state.messageSeen=1;
            InitExportPlaybackTracks(&app,target*30);
            SampleGlobalPlaybackTracks(&app);
            // Preserve object addresses for MME while borrowing private pose,
            // morph and mesh storage only for this synchronous render.
            struct Swap {
                unsigned char* live;ModelCopy& copy;
                std::unique_ptr<mdl::ModelRecord> original;
                Swap(unsigned char* p,ModelCopy& c):live(p),copy(c),original(new mdl::ModelRecord(*mdl::Mdl(p))) {
                    *mdl::Mdl(p)=*c.model;bone_scale::ExchangeState(c.raw(),p);
                }
                ~Swap(){*copy.model=*mdl::Mdl(live);*mdl::Mdl(live)=*original;bone_scale::ExchangeState(copy.raw(),live);}
            };
            std::vector<std::unique_ptr<Swap>> swaps;
            for(size_t i=0;i<models.size();++i)swaps.emplace_back(new Swap(live[i],*models[i]));
            return visitor(target,output);
        };
        const double begin=firstFrame/30.0,end=lastFrame/30.0;
        // Effects with a previous-frame dependency receive the same preroll.
        for(std::uint64_t n=0;double(n)/fps+1e-9<begin;++n)
            if(!render(double(n)/fps,false))return false;
        for(std::uint64_t n=0;begin+double(n)/fps<=end+1e-9;++n)
            if(!render(begin+double(n)/fps,true))return false;
        return true;
    } catch(const std::exception& e) {error=e.what();}
      catch(...) {error="Invalid scene data during geometry sampling";}
    return false;
}
}
}
