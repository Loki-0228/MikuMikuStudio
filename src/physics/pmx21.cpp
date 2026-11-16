#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/pmx21.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/sequential_file_reader.hpp"
#include "mikumikustudio/pmx21_morph.hpp"
#include "mikumikustudio/qdef_skinning.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/d3d12_skinning.hpp"
#include "mikumikustudio/soft_body_compute.hpp"
#include "btBulletDynamicsCommon.h"
#include "BulletSoftBody/btSoftRigidDynamicsWorld.h"
#include "BulletSoftBody/btSoftBodyInternals.h"
#include "../plugins/scene_resource_budget.hpp"
#include "soft_body_topology.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mikumikustudio::pmx21 {
namespace {
struct Anchor { int rigid = -1; unsigned vertex = 0; unsigned char nearMode = 0; int node = -1; };
struct SoftBody {
    std::wstring name, nameEn;
    unsigned char shape = 0, group = 0, flags = 0;
    unsigned short mask = 0;
    int material = -1, bending = 0, clusters = 0, aero = 0;
    float mass = 0, margin = 0;
    float config[18]{}, stiffness[3]{};
    int iterations[4]{};
    std::vector<Anchor> anchors;
    std::vector<btVector3> bakedAnchorOffsets;
    std::vector<unsigned> pins, vertices;
    std::unique_ptr<btSoftBody> body;
    std::unique_ptr<soft_compute::State> compute;
    bool initialized = false;
};
struct State {
    btSoftRigidDynamicsWorld* world = nullptr;
    bool present = true;
    std::vector<SoftBody> soft;
    ~State() {
        if (world) for (auto& s : soft) if (s.body) world->removeCollisionObject(s.body.get());
        if (world) world->getWorldInfo().m_sparsesdf.Reset();
    }
};
// Loading, physics, and disposal run on the model owner thread.
std::unordered_map<const unsigned char*, std::unique_ptr<State>> states;

bool SolveSoftBody(btSoftBody& body) {
    for(auto& entry:states)for(auto& soft:entry.second->soft)
        if(soft.body.get()==&body && soft.compute)
            return soft.compute->Solve(body,processor::ResolvedCloth());
    return false;
}
bool SolveClusters(btAlignedObjectArray<btSoftBody*>& bodies) {
    return soft_compute::SolveClusters(bodies,processor::ResolvedCloth());
}

void BeforeStep(btDiscreteDynamicsWorld& world) {
    for (auto& entry : states) {
        const auto& model = *mdl::Mdl(entry.first);
        if (entry.second->present && model.loadComplete && model.scenePtr && model.scenePtr->world == &world)
            ApplyImpulses(const_cast<unsigned char*>(entry.first));
    }
}

struct Input {
    fileio::SequentialFileReader& reader;
    std::int64_t end;
    bool utf8;
    bool bytes(void* value, unsigned size) {
        const auto at = reader.Tell();
        return at >= 0 && at <= end && size <= std::uint64_t(end - at) &&
            reader.Read(value, size) == static_cast<int>(size);
    }
    template<class T> bool value(T& v) { return bytes(&v, sizeof v); }
    bool count(int& count, unsigned minimum) {
        return value(count) && count >= 0 && reader.Tell() <= end &&
            std::uint64_t(count)*minimum <= std::uint64_t(end-reader.Tell());
    }
    bool index(int& result, unsigned width, bool vertex = false) {
        unsigned bits = 0;
        if (!bytes(&bits, width)) return false;
        result = width == 1 && !vertex ? static_cast<signed char>(bits) :
            width == 2 && !vertex ? static_cast<short>(bits) : static_cast<int>(bits);
        return true;
    }
    bool text(std::wstring& result) {
        int length = 0;
        if (!count(length, 1) || (!utf8 && (length & 1))) return false;
        if (!length) return true;
        if (!utf8) { result.resize(length/2); return bytes(result.data(), length); }
        std::vector<char> raw(length);
        if (!bytes(raw.data(), length)) return false;
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), length, nullptr, 0);
        if (n <= 0) return false;
        result.resize(n);
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), length, result.data(), n) == n;
    }
};
btVector3 Vector(const float* p) { return {p[0], p[1], p[2]}; }
template<class T> auto SetUid(T* c, int id, int) -> decltype(c->setUid(id), void()) { c->setUid(id); }
template<class T> void SetUid(T* c, int id, long) { c->setUserConstraintId(id); }
bool Finite(const float* p, unsigned count) {
    for (unsigned i = 0; i < count; ++i) if (!std::isfinite(p[i])) return false;
    return true;
}

bool CreateSoftBody(mdl::ModelRecord& model, SoftBody& soft, btSoftRigidDynamicsWorld& world) {
    if (!model.pmxVertices || !model.materials || !model.indices) return false;
    const auto* materials = static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    std::uint64_t first = 0;
    for (int i = 0; i < soft.material; ++i) {
        if (materials[i].faceVertexCount < 0) return false;
        first += materials[i].faceVertexCount;
    }
    const int count = materials[soft.material].faceVertexCount;
    if (count < 0 || first + count > model.indexCount || count % 3) return false;
    if (!count) return true;
    const auto* indices = static_cast<const std::int32_t*>(model.indices);
    std::unordered_map<unsigned, int> nodes;
    std::vector<int> faces;
    soft.vertices.clear();
    for (int i = 0; i < count; ++i) {
        const int vertex = indices[first + i];
        if (vertex < 0 || unsigned(vertex) >= model.vertexCount) return false;
        const auto found = nodes.emplace(unsigned(vertex), int(soft.vertices.size()));
        if (found.second) soft.vertices.push_back(unsigned(vertex));
        faces.push_back(found.first->second);
    }
    std::vector<btVector3> positions;
    std::vector<btScalar> masses(soft.vertices.size(), 1);
    for (unsigned index : soft.vertices) positions.push_back(Vector(model.pmxVertices[index].position));
    auto body = std::make_unique<btSoftBody>(&world.getWorldInfo(), int(positions.size()), positions.data(), masses.data());
    auto* material = body->m_materials[0];
    material->m_kLST = soft.stiffness[0]; material->m_kAST = soft.stiffness[1]; material->m_kVST = soft.stiffness[2];
    if (soft.shape == 1) {
        for (int i = 1; i < int(positions.size()); ++i) body->appendLink(i-1, i, material);
    } else {
        topology::AppendFaces(*body,faces,material);
    }
    body->setTotalMass(soft.mass == 0 ? 1 : soft.mass);
    body->getCollisionShape()->setMargin(soft.margin);
    body->m_cfg.aeromodel = static_cast<btSoftBody::eAeroModel::_>(soft.aero);
    static_assert(offsetof(btSoftBody::Config, kSS_SPLT_CL)-offsetof(btSoftBody::Config, kVCF) == 17*sizeof(float));
    std::memcpy(&body->m_cfg.kVCF, soft.config, sizeof soft.config);
    body->m_cfg.viterations = soft.iterations[0]; body->m_cfg.piterations = soft.iterations[1];
    body->m_cfg.diterations = soft.iterations[2]; body->m_cfg.citerations = soft.iterations[3];
    body->m_cfg.collisions |= btSoftBody::fCollision::VF_SS;
    if ((soft.flags & 1) && soft.bending > 0) topology::GenerateBending(*body,soft.bending,material);
    if (soft.flags & 2) { body->generateClusters(soft.clusters); body->m_cfg.collisions = 0x22; }
    if (soft.flags & 4) body->randomizeConstraints();
    for (auto& anchor : soft.anchors) {
        const auto found = nodes.find(anchor.vertex);
        if (found == nodes.end() || anchor.rigid < 0 || unsigned(anchor.rigid) >= model.rigidCount) continue;
        anchor.node=found->second;
        auto* rigid = static_cast<btRigidBody*>(model.rigidTable[anchor.rigid].body);
        if (rigid) {
            const int index=body->m_anchors.size();
            body->appendAnchor(found->second, rigid);
            if (static_cast<std::size_t>(index)<soft.bakedAnchorOffsets.size())
                body->m_anchors[index].m_local=soft.bakedAnchorOffsets[index];
        }
    }
    for (unsigned pin : soft.pins) {
        const auto found = nodes.find(pin);
        if (found != nodes.end()) body->setMass(found->second, 0);
    }
    body->updateNormals();
    world.addSoftBody(body.get(), static_cast<short>(1u << soft.group), static_cast<short>(~soft.mask));
    soft.body = std::move(body);
    soft.compute = std::make_unique<soft_compute::State>();
    return true;
}

btVector3 SkinPosition(const mdl::ModelRecord& model, unsigned vertex, const std::vector<affine::BoneAux>& aux) {
    const auto& v = model.pmxVertices[vertex];
    const auto* bones = model.boneTable;
    static const float zero[16]{};
    static const affine::BoneAux zeroAux{};
    auto ref = [&](int i) -> affine::BoneRef {
        const int b = v.bone[i];
        return b >= 0 && unsigned(b) < model.boneCount ? affine::BoneRef{bones[b].matInit, &aux[b]} : affine::BoneRef{zero, &zeroAux};
    };
    affine::Skinned result{};
    switch (v.weightType) {
    case mdl::PmxWeightType::bdef1: affine::SkinBdef1(ref(0), v.position, v.normal, result); break;
    case mdl::PmxWeightType::bdef2: affine::SkinBdef2(ref(0), ref(1), v.weight[0], v.position, v.normal, result); break;
    case mdl::PmxWeightType::bdef4: {
        const affine::BoneRef refs[4]{ref(0), ref(1), ref(2), ref(3)};
        affine::SkinBdef4(refs, v.weight, v.position, v.normal, result); break;
    }
    case mdl::PmxWeightType::sdef:
        affine::SkinSdef(ref(0), ref(1), v.weight[0], v.position, v.normal,
            v.sdef.center, v.sdef.r0Offset, v.sdef.r1Offset, affine::DefaultRotationOps(), result); break;
    case mdl::PmxWeightType::qdef:
        affine::SkinQdef(v.position, v.normal, v.bone, v.weight, int(model.boneCount),
            [bones](int b) { return bones[b].matInit; }, aux.data(), result.position, result.normal); break;
    default: std::memcpy(result.position, v.position, 12); break;
    }
    return Vector(result.position);
}
} // namespace

bool ReadSoftBodies(unsigned char* m, fileio::SequentialFileReader& reader, std::int64_t end, bool utf8) {
    auto& model = *mdl::Mdl(m);
    Input input{reader, end, utf8};
    int count = 0;
    if (!input.count(count, 141)) return false;
    auto state = std::make_unique<State>();
    state->soft.resize(count);
    for (auto& soft : state->soft) {
        if (!input.text(soft.name) || !input.text(soft.nameEn) || !input.value(soft.shape) ||
            !input.index(soft.material, model.pmxMaterialIndexSize) || !input.value(soft.group) ||
            !input.value(soft.mask) || !input.value(soft.flags) || !input.value(soft.bending) ||
            !input.value(soft.clusters) || !input.value(soft.mass) || !input.value(soft.margin) ||
            !input.value(soft.aero) || !input.value(soft.config) || !input.value(soft.iterations) ||
            !input.value(soft.stiffness)) return false;
        if (soft.shape > 1 || soft.group > 15 || soft.flags > 7 || soft.material < 0 ||
            unsigned(soft.material) >= model.materialCount || soft.bending < 0 ||
            unsigned(soft.bending) > model.vertexCount || soft.clusters < 0 ||
            unsigned(soft.clusters) > model.vertexCount || soft.aero < 0 || soft.aero > 4 ||
            !std::isfinite(soft.mass) || soft.mass < 0 || !std::isfinite(soft.margin) || soft.margin < 0 ||
            !Finite(soft.config, 18) || !Finite(soft.stiffness, 3)) return false;
        for (int i : soft.iterations) if (i < 0 || i > 8192) return false;
        int anchors = 0;
        if (!input.count(anchors, model.pmxRigidIndexSize + model.pmxVertexIndexSize + 1)) return false;
        soft.anchors.resize(anchors);
        for (auto& anchor : soft.anchors) {
            int vertex = 0;
            if (!input.index(anchor.rigid, model.pmxRigidIndexSize) ||
                !input.index(vertex, model.pmxVertexIndexSize, true) || !input.value(anchor.nearMode) ||
                anchor.rigid < 0 || unsigned(anchor.rigid) >= model.rigidCount || vertex < 0 ||
                unsigned(vertex) >= model.vertexCount || anchor.nearMode > 1) return false;
            anchor.vertex = unsigned(vertex);
        }
        int pins = 0;
        if (!input.count(pins, model.pmxVertexIndexSize)) return false;
        soft.pins.resize(pins);
        for (auto& pin : soft.pins) {
            int vertex = 0;
            if (!input.index(vertex, model.pmxVertexIndexSize, true) || vertex < 0 || unsigned(vertex) >= model.vertexCount) return false;
            pin = unsigned(vertex);
        }
        // Validate the mesh before publication; a malformed material must not
        // become a silently missing body when the live scene starts stepping.
        const auto* materials = static_cast<const mdl::ModelMaterialRecord*>(model.materials);
        const auto* indices = static_cast<const std::int32_t*>(model.indices);
        std::uint64_t first = 0;
        for (int i = 0; i < soft.material; ++i) {
            if (materials[i].faceVertexCount < 0) return false;
            first += materials[i].faceVertexCount;
        }
        const int faces = materials[soft.material].faceVertexCount;
        if (faces < 0 || faces % 3 || first + faces > model.indexCount) return false;
        std::vector<bool> used(model.vertexCount, false);
        for (int i = 0; i < faces; ++i) {
            const int v = indices[first+i];
            if (v < 0 || unsigned(v) >= model.vertexCount) return false;
            used[v] = true;
        }
        for (auto pin : soft.pins) if (!used[pin]) return false;
        for (const auto& anchor : soft.anchors) if (!used[anchor.vertex]) return false;
    }
    // Defer Bullet objects until the model is in its final scene. Transactional
    // imports parse in a temporary world which must never be retained here.
    states[m] = std::move(state);
    DynamicsWorld::beforeStep = BeforeStep;
    DynamicsWorld::solveSoftBody = SolveSoftBody;
    DynamicsWorld::solveClusters = SolveClusters;
    return true;
}

int CreateJoint(PhysicsScene& scene, mdl::ModelRecord& model, mdl::JointRecord& joint, unsigned type) {
    auto& a = *static_cast<btRigidBody*>(model.rigidTable[joint.rigidA].body);
    auto& b = *static_cast<btRigidBody*>(model.rigidTable[joint.rigidB].body);
    btTransform pose;
    pose.setIdentity();
    // PMX rotation order is Z, X, Y, matching the existing rigid-body loader.
    const btQuaternion qz(btVector3(0,0,1), joint.rotation[2]);
    const btQuaternion qx(btVector3(1,0,0), joint.rotation[0]);
    const btQuaternion qy(btVector3(0,1,0), joint.rotation[1]);
    pose.setRotation(qy*qx*qz); pose.setOrigin(Vector(joint.position));
    const btTransform fa = a.getWorldTransform().inverse()*pose, fb = b.getWorldTransform().inverse()*pose;
    const auto lo = Vector(joint.limits+3), hi = Vector(joint.limits);
    const auto alo = Vector(joint.limits+9), ahi = Vector(joint.limits+6);
    const auto sp = Vector(joint.springs), sa = Vector(joint.springs+3);
    std::unique_ptr<btTypedConstraint> result;
    switch (type) {
    case 1: {
        auto c = std::make_unique<btGeneric6DofConstraint>(a,b,fa,fb,true);
        c->setLinearLowerLimit(lo); c->setLinearUpperLimit(hi);
        c->setAngularLowerLimit(alo); c->setAngularUpperLimit(ahi); result = std::move(c); break;
    }
    case 2: result = std::make_unique<btPoint2PointConstraint>(a,b,fa.getOrigin(),fb.getOrigin()); break;
    case 3: {
        auto c = std::make_unique<btConeTwistConstraint>(a,b,fa,fb);
        c->setLimit(alo.z(),alo.y(),alo.x(),sp.x(),sp.y(),sp.z());
        c->setDamping(lo.x()); c->setFixThresh(hi.x()); c->enableMotor(lo.z() >= 1);
        if (lo.z() >= 1) { c->setMaxMotorImpulse(hi.z()); btQuaternion q; q.setEuler(sa.y(),sa.x(),sa.z()); c->setMotorTarget(q); }
        result = std::move(c); break;
    }
    case 4: {
        auto c = std::make_unique<btSliderConstraint>(a,b,fa,fb,true);
        c->setLowerLinLimit(lo.x()); c->setUpperLinLimit(hi.x());
        c->setLowerAngLimit(alo.x()); c->setUpperAngLimit(ahi.x());
        c->setPoweredLinMotor(sp.x() >= 1); c->setPoweredAngMotor(sa.x() >= 1);
        c->setTargetLinMotorVelocity(sp.y()); c->setMaxLinMotorForce(sp.z());
        c->setTargetAngMotorVelocity(sa.y()); c->setMaxAngMotorForce(sa.z()); result = std::move(c); break;
    }
    case 5: {
        auto c = std::make_unique<btHingeConstraint>(a,b,fa,fb,true);
        c->setLimit(alo.x(),ahi.x(),sp.x(),sp.y(),sp.z());
        c->enableAngularMotor(sa.x() >= 1,sa.y(),sa.z()); result = std::move(c); break;
    }
    default: return -1;
    }
    const int id = scene.constraintId++;
    SetUid(result.get(), id, 0);
    scene.world->addConstraint(result.get(), false);
    result.release();
    // The legacy distance clamp is specific to spring 6DOF joints. Applying
    // it to a free slider or point constraint would override Bullet's result.
    joint.radiusBound = -1;
    return id;
}

void Forget(unsigned char* model) noexcept { states.erase(model); }
void ExchangeRenderState(unsigned char* live, unsigned char* sampled) noexcept {
    const auto editor = states.find(live), bake = states.find(sampled);
    // ModelCopy creates a private state for every live PMX 2.1 state.
    if (editor != states.end() && bake != states.end())
        std::swap(editor->second, bake->second);
}

void CopyState(const unsigned char* source, unsigned char* target) {
    const auto found = states.find(source);
    if (found == states.end()) return;
    auto copy = std::make_unique<State>();
    for (const auto& src : found->second->soft) {
        SoftBody dst;
        dst.name = src.name; dst.nameEn = src.nameEn;
        dst.shape = src.shape; dst.group = src.group; dst.flags = src.flags; dst.mask = src.mask;
        dst.material = src.material; dst.bending = src.bending; dst.clusters = src.clusters; dst.aero = src.aero;
        dst.mass = src.mass; dst.margin = src.margin;
        std::copy_n(src.config, 18, dst.config); std::copy_n(src.stiffness, 3, dst.stiffness);
        std::copy_n(src.iterations, 4, dst.iterations);
        dst.anchors = src.anchors; dst.pins = src.pins;
        // A reconstructed anchor must retain the bind-space offset created when
        // the live body was loaded, even if the rigid has since moved.
        if (src.body) for (int i=0;i<src.body->m_anchors.size();++i)
            dst.bakedAnchorOffsets.push_back(src.body->m_anchors[i].m_local);
        copy->soft.push_back(std::move(dst));
    }
    states[target] = std::move(copy);
}
void MeasureResources(const unsigned char* model, plugins::resource_budget::Collector& out) {
    const auto found = states.find(model);
    if (found == states.end()) return;
    out.Add(&*found, 1, sizeof(*found)+4*sizeof(void*));
    const auto& state = *found->second;
    out.Array(&state, 1); out.Vector(state.soft);
    // These Bullet arrays are privately owned. Their containing member is a
    // stable allocation identity, including size-zero arrays retaining capacity.
    const auto array = [&](const auto& values) {
        using Element = std::remove_reference_t<decltype(values[0])>;
        out.Add(&values, values.capacity(), sizeof(Element));
    };
    const auto nodes = [&](auto&& self, const btDbvtNode* node) -> void {
        if (!node) return;
        out.Array(node, 1);
        if (node->isinternal()) { self(self, node->childs[0]); self(self, node->childs[1]); }
    };
    const auto tree = [&](const btDbvt& value) {
        nodes(nodes, value.m_root); out.Array(value.m_free, 1); array(value.m_stkStack);
    };
    for (const auto& soft : state.soft) {
        out.String(soft.name); out.String(soft.nameEn);
        out.Vector(soft.bakedAnchorOffsets);
        out.Vector(soft.anchors); out.Vector(soft.pins); out.Vector(soft.vertices);
        if (!soft.body) continue;
        if (soft.compute) soft.compute->Measure(out);
        const auto& body = *soft.body; out.Array(&body, 1);
        out.Array(static_cast<const btSoftBodyCollisionShape*>(body.getCollisionShape()), 1);
        array(body.m_cfg.m_vsequence); array(body.m_cfg.m_psequence); array(body.m_cfg.m_dsequence);
        array(body.m_pose.m_pos); array(body.m_pose.m_wgh);
        array(body.m_notes); array(body.m_nodes); array(body.m_links); array(body.m_faces); array(body.m_tetras);
        array(body.m_anchors); array(body.m_rcontacts); array(body.m_scontacts); array(body.m_joints);
        array(body.m_materials); array(body.m_clusters); array(body.m_clusterConnectivity); array(body.m_userIndexMapping);
        for (int i = 0; i < body.m_materials.size(); ++i) out.Array(body.m_materials[i], 1);
        for (int i = 0; i < body.m_clusters.size(); ++i) {
            const auto* cluster = body.m_clusters[i]; out.Array(cluster, 1);
            array(cluster->m_nodes); array(cluster->m_masses); array(cluster->m_framerefs);
        }
        for (int i = 0; i < body.m_joints.size(); ++i) {
            const auto* joint = body.m_joints[i];
            switch (joint->Type()) {
            case btSoftBody::Joint::eType::Linear: out.Array(static_cast<const btSoftBody::LJoint*>(joint), 1); break;
            case btSoftBody::Joint::eType::Angular: out.Array(static_cast<const btSoftBody::AJoint*>(joint), 1); break;
            case btSoftBody::Joint::eType::Contact: out.Array(static_cast<const btSoftBody::CJoint*>(joint), 1); break;
            }
        }
        tree(body.m_ndbvt); tree(body.m_fdbvt); tree(body.m_cdbvt);
    }
}
void SuspendSoftBodies(unsigned char* model) noexcept {
    const auto found = states.find(model);
    if (found == states.end()) return;
    auto& state = *found->second;
    for (auto& soft : state.soft) {
        if (state.world && soft.body) state.world->removeCollisionObject(soft.body.get());
        soft.compute.reset();soft.body.reset(); soft.initialized = false; soft.vertices.clear();
    }
    if (state.world) state.world->getWorldInfo().m_sparsesdf.Reset();
    state.world = nullptr;
}
void SetPresent(unsigned char* model, bool present) noexcept {
    const auto found = states.find(model);
    if (found == states.end()) return;
    if (!present) SuspendSoftBodies(model);
    found->second->present = present;
}
bool HasSoftBodies(const unsigned char* model) {
    return SoftBodyCount(model)!=0;
}
std::size_t SoftBodyCount(const unsigned char* model) {
    const auto found = states.find(model);
    return found != states.end() ? found->second->soft.size() : 0;
}
void SyncSoftBodies(unsigned char* m, bool reset) {
    const auto found = states.find(m);
    if (found == states.end() || !found->second->present || found->second->soft.empty()) return;
    auto& state = *found->second;
    auto& model = *mdl::Mdl(m);
    if (!model.scenePtr || !model.scenePtr->world || !model.boneTable) return;
    auto* world = dynamic_cast<btSoftRigidDynamicsWorld*>(model.scenePtr->world);
    if (!world) return;
    if (state.world && state.world != world) SuspendSoftBodies(m);
    if (!state.world) {
        world->getWorldInfo().m_gravity = world->getGravity();
        if (!world->getWorldInfo().m_sparsesdf.cells.size()) world->getWorldInfo().m_sparsesdf.Initialize();
        state.world = world;
        for (auto& soft : state.soft) CreateSoftBody(model, soft, *world);
    }
    RefreshVertexMorphs(m);
    std::vector<affine::BoneAux> aux(model.boneCount);
    for (unsigned i = 0; i < model.boneCount; ++i) {
        float rotation[4];
        affine::DefaultQuatFromMatrix(rotation, model.boneTable[i].matInit);
        affine::PrecomputeBone(model.boneTable[i].matInit, rotation, aux[i]);
    }
    for (auto& soft : state.soft) {
        if (!soft.body) continue;
        auto& body = *soft.body;
        for (int i = 0; i < body.m_nodes.size(); ++i) {
            auto& node = body.m_nodes[i];
            if (!reset && soft.initialized && node.m_im != 0) continue;
            const btVector3 position = SkinPosition(model, soft.vertices[i], aux);
            if (!std::isfinite(position.x()) || !std::isfinite(position.y()) || !std::isfinite(position.z())) continue;
            node.m_x = position; node.m_q = position;
            node.m_v.setZero(); node.m_f.setZero();
        }
        body.updateNormals(); body.updateBounds(); body.activate(true);
        if (!reset && soft.initialized) for (const auto& anchor : soft.anchors) {
            if (!anchor.nearMode || unsigned(anchor.rigid) >= model.rigidCount) continue;
            auto* rigid = static_cast<btRigidBody*>(model.rigidTable[anchor.rigid].body);
            if (!rigid || !rigid->getMotionState()) continue;
            if(anchor.node<0 || anchor.node>=body.m_nodes.size())continue;
            auto& node = body.m_nodes[anchor.node];
            btTransform pose; rigid->getMotionState()->getWorldTransform(pose);
            if (rigid->isStaticOrKinematicObject()) node.m_v += pose.getOrigin()-node.m_x;
            else { pose.setOrigin(node.m_x); rigid->getMotionState()->setWorldTransform(pose); }
        }
        soft.initialized = true;
    }
}

void ApplyImpulses(unsigned char* m) {
    const auto& model = *mdl::Mdl(m);
    if (!model.morphs || !model.rigidTable) return;
    const MorphWeights weights(model.morphs, int(model.morphCount));
    auto apply = [&](const mdl::MorphRecord& morph, float weight) {
        if (morph.type != 10 || !morph.impulse2Entries || weight == 0 || !std::isfinite(weight)) return;
        const auto* entries = reinterpret_cast<const Impulse*>(morph.impulse2Entries);
        for (int i = 0; i < morph.offsetCount; ++i) {
            const auto& entry = entries[i];
            if (entry.rigid < 0 || unsigned(entry.rigid) >= model.rigidCount ||
                !Finite(entry.velocity, 3) || !Finite(entry.torque, 3)) continue;
            auto* body = static_cast<btRigidBody*>(model.rigidTable[entry.rigid].body);
            if (!body || body->isStaticOrKinematicObject() || body->getActivationState() == DISABLE_SIMULATION) continue;
            btVector3 velocity = Vector(entry.velocity)*weight, torque = Vector(entry.torque)*weight;
            if (Vector(entry.velocity).length2() == 0 && Vector(entry.torque).length2() == 0) {
                body->clearForces(); body->setLinearVelocity(btVector3(0,0,0)); body->setAngularVelocity(btVector3(0,0,0));
            } else {
                if (entry.local) { velocity = body->getWorldTransform().getBasis()*velocity; torque = body->getWorldTransform().getBasis()*torque; }
                body->applyCentralImpulse(velocity); body->applyTorqueImpulse(torque);
            }
            body->activate(true);
        }
    };
    for (unsigned i = 0; i < model.morphCount; ++i) {
        const auto& morph = model.morphs[i]; const float weight = weights.Get(int(i), morph.value);
        apply(morph, weight);
        if (morph.type == 0 && morph.groupEntries) for (int j = 0; j < morph.groupCount; ++j) {
            const auto& entry = morph.groupEntries[j];
            if (entry.morphIndex >= 0 && unsigned(entry.morphIndex) < model.morphCount)
                apply(model.morphs[entry.morphIndex], weight*entry.weight);
        }
    }
}

void WriteGpuSoftVertices(unsigned char* m, std::vector<gpu::Vertex>& vertices) {
    const auto found=states.find(m);
    if(found==states.end()) return;
    for(const auto& soft:found->second->soft) {
        if(!soft.body || !soft.initialized) continue;
        for(int i=0;i<soft.body->m_nodes.size();++i) {
            const auto& node=soft.body->m_nodes[i];
            if(!std::isfinite(node.m_x.x()) || !std::isfinite(node.m_x.y()) || !std::isfinite(node.m_x.z())) continue;
            auto& v=vertices[soft.vertices[i]];
            if(node.m_n.length2()>SIMD_EPSILON) {
                v.type=5;
                for(int c=0;c<3;++c){v.position[c]=node.m_x[c];v.normal[c]=node.m_n[c];}
            } else {
                // Rope nodes have no face normal. Retain their ordinary GPU
                // normal skinning, replacing only the simulated position.
                v.type|=0x100;
                v.center[3]=node.m_x.x();v.r0[3]=node.m_x.y();v.r1[3]=node.m_x.z();
            }
        }
    }
}

void WriteSoftVertices(unsigned char* m, void* vertices, std::size_t stride, mdl::EdgeVertex* edges, float distance) {
    const auto found = states.find(m);
    if (found == states.end()) return;
    const auto& model = *mdl::Mdl(m);
    const auto* materials = static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    for (const auto& soft : found->second->soft) {
        if (!soft.body || !soft.initialized) continue;
        for (int i = 0; i < soft.body->m_nodes.size(); ++i) {
            const unsigned index = soft.vertices[i];
            const auto& node = soft.body->m_nodes[i];
            if (!std::isfinite(node.m_x.x()) || !std::isfinite(node.m_x.y()) || !std::isfinite(node.m_x.z())) continue;
            auto& vertex = *reinterpret_cast<mdl::SkinnedVertexBase*>(static_cast<unsigned char*>(vertices) + stride*index);
            for (int c = 0; c < 3; ++c) {
                vertex.position[c] = node.m_x[c];
                if (node.m_n.length2() > SIMD_EPSILON) vertex.normal[c] = node.m_n[c];
                const auto& source = model.pmxVertices[index];
                const float edge = source.materialIndex >= 0 && unsigned(source.materialIndex) < model.materialCount ? materials[source.materialIndex].edgeSize : 0;
                edges[index].position[c] = vertex.position[c] + distance*source.edgeScale*edge*vertex.normal[c];
            }
        }
    }
}
} // namespace mikumikustudio::pmx21
#endif
