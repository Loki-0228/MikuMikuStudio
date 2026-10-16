#include "scene_resource_budget.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/affine_collision_shape.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/d3dx_mesh.hpp"
#include "mikudancestudio/display_capacity.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/owned_text.hpp"
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>
#include <cwchar>
#include <memory>
#include <typeinfo>

namespace mikudancestudio::plugins::resource_budget {
namespace {
struct Release { template<class T> void operator()(T* p) const noexcept { if(p) p->Release(); } };
const void* ComIdentity(IUnknown* resource, Collector& out) {
    IUnknown* identity = nullptr;
    if (FAILED(resource->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity) {
        out.MissingResource(RenderQuery); return resource;
    }
    const void* result = identity; identity->Release(); return result;
}
template<class T> void Table(Collector& out, const T* p, std::int64_t count) {
    if (!p) return;
    if (count < 0) { out.MissingResource(NativeOpaque); return; }
    // Several importers allocate one sentinel element for an empty table.
    out.Array(p, (std::max)(std::uint64_t(1), static_cast<std::uint64_t>(count)));
}
template<class Key> void KeyRegistry(const Key* pool, Collector& out) {
    std::lock_guard<std::mutex> lock(mdl::KeyPoolStorage<Key>::mutex);
    const auto& registry = mdl::KeyPoolStorage<Key>::capacities;
    const auto found = registry.find(pool);
    if (found != registry.end()) out.Add(&*found, 1, sizeof(*found) + 4 * sizeof(void*));
}
void DisplayRegistry(const mdl::ModelRecord& model, Collector& out) {
    std::lock_guard<std::mutex> lock(mdl::displayCapacityMutex);
    const auto found = mdl::displayCapacities.find(model.groupNames);
    const auto faces = found == mdl::displayCapacities.end() ? model.facialFrameCount : found->second.faces;
    if (found != mdl::displayCapacities.end()) out.Add(&*found, 1, sizeof(*found) + 4 * sizeof(void*));
    const auto row = [&](const mdl::FrameGroup* p) {
        const auto entry = mdl::displayIndices.find(p);
        if (entry != mdl::displayIndices.end()) out.Add(&*entry, 1, sizeof(*entry) + 4 * sizeof(void*));
    };
    if (model.displayFrames) for (std::uint32_t i = 0; i < faces; ++i) row(model.displayFrames + i);
    if (model.rbGroups) for (std::uint32_t i = 0; i < model.rigidBodyCount; ++i) row(static_cast<const mdl::FrameGroup*>(model.rbGroups) + i);
}
}
void Collector::RawText(const wchar_t* text) {
    if (!text) return;
    owned_text::Allocation allocation;
    if (!owned_text::Find(text, allocation)) {
        // An external pointer is not permission to scan its contents, nor
        // evidence of a minimum allocation. Keep the incomplete flag explicit.
        MissingResource(RawTextCapacity); return;
    }
    Add(text, allocation.bytes, 1);
    Add(allocation.registryIdentity, allocation.registryBytes, 1);
}
void Collector::VertexBuffer(void* value) {
    if (!value) return;
    auto* buffer = static_cast<IDirect3DVertexBuffer9*>(value);
    D3DVERTEXBUFFER_DESC desc{};
    if (FAILED(buffer->GetDesc(&desc))) { MissingResource(RenderQuery); return; }
    // Managed resources may retain both system and device copies.
    Add(ComIdentity(buffer, *this), desc.Size, desc.Pool == D3DPOOL_MANAGED ? 2 : 1, true);
}
void Collector::IndexBuffer(void* value) {
    if (!value) return;
    auto* buffer = static_cast<IDirect3DIndexBuffer9*>(value);
    D3DINDEXBUFFER_DESC desc{};
    if (FAILED(buffer->GetDesc(&desc))) { MissingResource(RenderQuery); return; }
    Add(ComIdentity(buffer, *this), desc.Size, desc.Pool == D3DPOOL_MANAGED ? 2 : 1, true);
}
void Collector::Mesh(void* mesh) {
    if (!mesh) return;
    // ID3DXBaseMesh::GetNumFaces, followed by the two public buffer queries.
    const auto faces = d3dx::MeshMethod<DWORD(WINAPI*)(void*), 4>(mesh)(mesh);
    Add(ComIdentity(static_cast<IUnknown*>(mesh), *this), faces, sizeof(DWORD), true);
    IDirect3DVertexBuffer9* vb = nullptr;
    const auto vertexResult = d3dx::MeshVertexBuffer(mesh, &vb);
    std::unique_ptr<IDirect3DVertexBuffer9, Release> vertices(vb);
    if (FAILED(vertexResult) || !vb) MissingResource(RenderQuery); else VertexBuffer(vb);
    IDirect3DIndexBuffer9* ib = nullptr;
    const auto indexResult = d3dx::MeshIndexBuffer(mesh, &ib);
    std::unique_ptr<IDirect3DIndexBuffer9, Release> indices(ib);
    if (FAILED(indexResult) || !ib) MissingResource(RenderQuery); else IndexBuffer(ib);
}
void Shape(const btCollisionShape* shape, Collector& out) {
    if (!shape) return;
    // Exact dynamic types: an arbitrary derived type may own more storage.
    if (typeid(*shape) == typeid(btSphereShape)) out.Add(shape, 1, sizeof(btSphereShape));
    else if (typeid(*shape) == typeid(btBoxShape)) out.Add(shape, 1, sizeof(btBoxShape));
    else if (typeid(*shape) == typeid(btCapsuleShape)) out.Add(shape, 1, sizeof(btCapsuleShape));
    else if (typeid(*shape) == typeid(AffineConvexShape)) {
        auto& affine = static_cast<const AffineConvexShape&>(*shape);
        out.Array(&affine, 1); Shape(&affine.BaseShape(), out);
    } else { out.Array(shape, 1); out.MissingResource(PhysicsStorage); }
}
void Body(const btRigidBody* body, Collector& out) {
    if (!body) return;
    out.Array(body, 1);
    if (typeid(*body) != typeid(btRigidBody)) out.MissingResource(PhysicsStorage);
    if (const auto* motion = body->getMotionState()) {
        if (typeid(*motion) == typeid(btDefaultMotionState)) out.Add(motion, 1, sizeof(btDefaultMotionState));
        else { out.Array(motion, 1); out.MissingResource(PhysicsStorage); }
    }
    Shape(body->getCollisionShape(), out);
    // Bullet's private constraint-reference array exposes size, not retained
    // capacity; removing constraints can leave its allocation alive.
    out.MissingResource(PhysicsStorage);
}
void Joint(const btTypedConstraint* joint, Collector& out) {
    if (!joint) return;
    if (typeid(*joint) == typeid(btGeneric6DofSpringConstraint)) out.Add(joint, 1, sizeof(btGeneric6DofSpringConstraint));
    else if (typeid(*joint) == typeid(btGeneric6DofConstraint)) out.Add(joint, 1, sizeof(btGeneric6DofConstraint));
    else if (typeid(*joint) == typeid(btPoint2PointConstraint)) out.Add(joint, 1, sizeof(btPoint2PointConstraint));
    else { out.Array(joint, 1); out.MissingResource(PhysicsStorage); }
}
void Model(const mdl::ModelRecord& m, Collector& out) {
    out.Array(&m, 1);
    // PMD stores each WORD at an even WORD offset; PMX uses uint32 indices.
    out.Add(m.indices, m.indexCount, sizeof(std::uint32_t));
    Table(out, static_cast<const mdl::ModelMaterialRecord*>(m.materials), m.materialCount);
    Table(out, m.rawVertices, m.vertexCount); Table(out, m.pmxVertices, m.vertexCount);
    Table(out, m.boneTable, m.boneCount); Table(out, m.morphs, m.morphCount);
    Table(out, m.ikChains, m.ikChainCount);
    Table(out, static_cast<const mdl::BoneOrderEntry*>(m.boneOrderTable), m.boneOrderCount);
    Table(out, m.boneKeyCursors, m.boneCount); Table(out, m.boneTrackActive, m.boneCount);
    Table(out, m.morphKeyCursors, m.morphCount); Table(out, m.morphTrackActive, m.morphCount);
    Table(out, m.boneSelection, m.boneCount); Table(out, m.bonePhysicsState, m.boneCount);
    Table(out, m.boneKeyIndices, m.boneCount); Table(out, m.morphKeyIndices, m.morphCount);
    Table(out, static_cast<const mdl::DisplayGroup*>(m.groupNames), mdl::DisplayGroupCount(m));
    Table(out, static_cast<const mdl::FrameGroup*>(m.rbGroups), m.rigidBodyCount);
    Table(out, m.displayFrames, mdl::FacialFrameCount(m));
    Table(out, m.morph0Table, m.morph0Count); Table(out, m.boneMorphTable, m.boneMorphCount);
    for (unsigned i = 0; i < 5; ++i) Table(out, m.uvMorphTables.byFamily[i], m.uvMorphCounts.byFamily[i]);
    Table(out, m.materialMorphPools.base, m.materialCount);
    Table(out, m.materialMorphPools.additive, m.materialCount);
    Table(out, m.materialMorphPools.multiplicative, m.materialCount);
    if (m.morphTableGap) out.MissingResource(NativeOpaque);
    if (m.poseTraceBuffer) { out.Add(m.poseTraceBuffer, 1, 0x3B3760); out.MissingResource(NativeOpaque); }
    for (auto* text : m.pmxTextBuffers) out.RawText(text);
    if (m.boneTable) for (std::uint32_t i = 0; i < m.boneCount; ++i) {
        const auto& b = m.boneTable[i]; out.RawText(b.jpText); out.RawText(b.enText); Table(out, b.ikLinks, b.ikLinkCount);
    }
    if (m.ikChains) for (std::uint32_t i = 0; i < m.ikChainCount; ++i) Table(out, m.ikChains[i].links, m.ikChains[i].linkCount);
    if (m.morphs) for (std::uint32_t i = 0; i < m.morphCount; ++i) {
        const auto& v = m.morphs[i]; out.RawText(v.jpText); out.RawText(v.enText);
        Table(out, v.vertexEntries, v.offsetCount); Table(out, v.boneEntries, v.boneCount);
        Table(out, v.groupEntries, v.groupCount); Table(out, v.materialEntries, v.materialCount);
        for (unsigned j = 0; j < 5; ++j) Table(out, v.uvEntries[j], v.uvCounts[j]);
        if (v.impulseEntries || v.impulse2Entries) out.MissingResource(NativeOpaque);
    }
    out.Array(m.boneKeys, mdl::BoneKeyCapacity(m)); out.Array(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys));
    const auto displays = mdl::KeyPoolCapacity(m.displayKeys); out.Array(m.displayKeys, displays);
    if (m.displayKeys) for (std::size_t i = 0; i < displays; ++i) {
        Table(out, m.displayKeys[i].ikStates, m.ikChainCount);
        Table(out, m.displayKeys[i].selectorStates, m.boneOrderCount);
    }
    Table(out, m.rigidTable, m.rigidCount); Table(out, m.jointTable, m.jointCount);
    if (m.rigidTable) for (std::uint32_t i = 0; i < m.rigidCount; ++i) {
        const auto& r = m.rigidTable[i]; out.RawText(r.jpText); out.RawText(r.enText);
        Body(static_cast<const btRigidBody*>(r.body), out);
    }
    if (m.jointTable) for (std::uint32_t i = 0; i < m.jointCount; ++i) {
        out.RawText(m.jointTable[i].jpText); out.RawText(m.jointTable[i].enText);
    }
    out.VertexBuffer(m.vertexBuffer); out.VertexBuffer(m.vertexBuffer2); out.IndexBuffer(m.indexBuffer);
    native_history::MeasureResources(m, out); mdl::MeasureBoneKeyStorage(m, out);
    KeyRegistry(m.displayKeys, out); DisplayRegistry(m, out);
    const auto* p = reinterpret_cast<const unsigned char*>(&m);
    bone_scale::MeasureResources(p, out); bone_curve::MeasureResources(p, out);
    morph_curve::MeasureResources(p, out); model_vertex_cache::MeasureResources(p, out);
    text_encoding::MeasureResources(m, out); native_mesh::MeasureResources(&m, out);
    physics_scale::MeasureResources(p, out); skinning::MeasureResources(p, out);
    mmv4mms::MeasureResources(p, out);
}
void Accessory(const mdl::AccessoryRecord& a, const mdl::AccessoryKey* keys, Collector& out) {
    out.Array(&a, 1); Table(out, static_cast<const D3DMATERIAL9*>(a.materials), a.materialCount);
    Table(out, a.texturePaths, a.materialCount); Table(out, a.textureTypes, a.materialCount);
    out.Array(keys, mdl::KeyPoolCapacity(keys)); out.Mesh(a.mesh);
    KeyRegistry(keys, out); native_mesh::MeasureResources(&a, out); mmv4mms::MeasureResources(&a, out);
}
}
#endif
