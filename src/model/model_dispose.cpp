#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/physics_scale.hpp"
// ===========================================================================
// VA 0x0048F830 - ModelDispose  (original: sub_48F830, 2099 bytes)
// ===========================================================================
// Teardown of every heap block owned by a 0x4CCF4 model object, in the
// original's exact order:
//   joints  (m+12748 array, count m+12756, stride 140) - physics constraint
//           release via 0x4013A0, then +20/+24, then the array itself;
//   rigids  (m+12744 array, count m+12752, stride 172) - physics body
//           release via 0x4068D0, then +20/+24, then the array;
//   30x28B morph-slot block at m+9980: per slot free +4 then +0, and the
//           twin slot 840 bytes later free +4 then +0;
//   post-load bone/morph lookup tables;
//   1000x28B animation pool m+9960: two sweeps freeing record +16 then
//           record +24, then the pool and pools m+9956 / m+9952;
//   singles m+11668, m+11672, m+9944, m+9936, m+9948;
//   IK records (m+9924, count m+11648, stride 136): 11 pointers per record
//           (+40 +44 +92 +104 +108 +112 +116 +120 +96 +100 +124);
//   11 contiguous pointers m+8724..m+8764, then the IK array;
//   IK chains (m+9920, count m+11656, stride 24): pointer +12, then array;
//   singles m+44, m+48, m+36, m+40 (vertex shadow buffers);
//   bones (m+9916, count m+11652, stride 604): +40 +44 +564, then array;
//   singles m+32, m+24, m+9928, m+9932, m+314596;
//   Release() (vtable+8) on the three D3D pool objects m+12, m+8, m+16;
//   singles m+9388, m+9392, m+9396, m+9400.
//
// Helpers (both take the scene pointer stored at model+0x3C by 0x4BF3E0):
//   0x004013A0 ReleasePhysConstraint(scene, constraintId):
//       world = scene+64 member; world vtable byte offsets +80 = count,
//       +88 = item(i), +40 = remove(item); the item whose user constraint
//       id (constraint+96 in the binary's Bullet build) matches the id
//       returned by CreatePhysJoint (0x406010, scene+56 counter) is removed
//       and deleted via its scalar deleting destructor (slot 0, flag 1).
//   0x004068D0 ReleasePhysRigid(scene, key):
//       world = scene+64; collision object list = world+8 count / world+16
//       array, walked in reverse; objects tagged +244==2 whose +548
//       (btRigidBody::m_debugBodyId, returned as CreateRigidBody out[0])
//       matches the key get their +516 (motion state) and +204 (collision
//       shape) released (slot-0 deleting dtor), are removed from the world
//       (vtable +20) and deleted (vtable +4).  (The original decompiles as a
//       crash-prone ternary - `v6 = tag!=2 ? 0 : obj; *(v6+0x224)` - which is
//       branch fusion of `tag==2 && *(obj+0x224)==key`; ported as the fused
//       conjunction.  The bullet275 package reproduces this layout exactly -
//       probe-verified: btRigidBody 560 bytes, m_debugBodyId@+548,
//       m_optionalMotionState@+516, damping@+480/484, friction/restitution
//       @+232/236, activation state@+224.)
//
// The scene/scene+64 dereferences are unconditional like the original -
// the physics world is created by SceneConstruct during WM_CREATE, before
// any model can exist.  The phase-15 null guards were reclaimed in
// phase 19 together with the timeline guards.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include <Windows.h>
#include <unknwn.h>

#include <cstdint>
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include <cstdlib>

#include "btBulletDynamicsCommon.h"

#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {
namespace {

template <typename T>
inline void FreeOwned(T*& pointer) {
    std::free(pointer);
    pointer = nullptr;
}

inline void ReleaseCom(void*& storage) {
    IUnknown*& object = mdl::ResourceAs<IUnknown>(storage);
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

// Remove scene objects through Bullet's public API.  The former x86 port
// walked Bullet's private vectors and virtual-table slots; those layouts do
// not survive the x64 ABI and left freed bodies in the world.
void ReleasePhysConstraint(void* scene, void* constraint) {
    auto* physics = static_cast<PhysicsScene*>(scene);
    if (physics == nullptr || physics->world == nullptr)
        return;
    btDiscreteDynamicsWorld* const world = physics->world;
    const int wantId = static_cast<int>(
        reinterpret_cast<std::intptr_t>(constraint));
    for (int i = world->getNumConstraints() - 1; i >= 0; --i) {
        btTypedConstraint* const item = world->getConstraint(i);
        if (item != nullptr && item->getUid() == wantId) {
            world->removeConstraint(item);
            delete item;
            return;
        }
    }
}

void ReleasePhysRigid(void* scene, void* bodyPointer) {
    auto* physics = static_cast<PhysicsScene*>(scene);
    auto* body = static_cast<btRigidBody*>(bodyPointer);
    if (physics == nullptr || physics->world == nullptr || body == nullptr)
        return;
    physics->world->removeRigidBody(body);
    delete body->getMotionState();
    delete body->getCollisionShape();
    delete body;
}

}  // namespace

// VA 0x0048F830
void ModelDispose(unsigned char* m) {
    frame_clipboard::ForgetModel(m);
    text_encoding::ForgetMaterialNames(*mdl::Mdl(m));
    mdl::ForgetDisplayCapacity(*mdl::Mdl(m));
    text_encoding::ForgetDisplayGroupNames(*mdl::Mdl(m));
    mmv4mms::Forget(m);
    native_mesh::Forget(m);
    physics_scale::Forget(m);
    bone_scale::Forget(m);
    morph_curve::Forget(m);
    skinning::ForgetModel(m);
    ShutdownD3D12Native();
    mdl::ModelRecord& model = *mdl::Mdl(m);
    void* scene = model.scenePtr;

    // ---- joints -----------------------------------------------------------
    if (model.jointCount > 0) {
        for (std::uint32_t i = 0; i < model.jointCount; ++i) {
            mdl::JointRecord* jt = &model.jointTable[i];
            ReleasePhysConstraint(
                scene, reinterpret_cast<void*>(
                           static_cast<std::intptr_t>(jt->constraint)));
            FreeOwned(jt->jpText);
            FreeOwned(jt->enText);
        }
    }
    FreeOwned(model.jointTable);

    // ---- rigid bodies -----------------------------------------------------
    if (model.rigidCount > 0) {
        for (std::uint32_t i = 0; i < model.rigidCount; ++i) {
            mdl::RigidRecord* rb = &model.rigidTable[i];
            ReleasePhysRigid(scene, rb->body);
            rb->body = nullptr;
            FreeOwned(rb->jpText);
            FreeOwned(rb->enText);
        }
    }
    FreeOwned(model.rigidTable);

    // ---- two consecutive 30-slot undo rings -------------------------------
    for (auto& ring : mikumikustudio::mdl::Mdl(m)->undoRings) {
        for (auto& undo : ring.slots) {
            if (undo.auxiliaryPose != nullptr) {
                std::free(undo.auxiliaryPose);
                undo.auxiliaryPose = nullptr;
            }
            if (undo.bonePose != nullptr) {
                std::free(undo.bonePose);
                undo.bonePose = nullptr;
            }
        }
    }

    FreeOwned(mdl::MorphKeyIndices(m));
    FreeOwned(mdl::BoneKeyIndices(m));

    // ---- animation pools --------------------------------------------------
    if (mdl::DisplayKeys(m) != nullptr) {
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(mdl::DisplayKeys(m)); i < capacity; ++i) {
            FreeOwned(mdl::IkStates(mdl::DisplayKeys(m)[i]));
            FreeOwned(mdl::SelectorStates(mdl::DisplayKeys(m)[i]));
        }
    }
    mdl::KeyPoolDestroy(mdl::DisplayKeys(m));
    mdl::DisplayKeys(m) = nullptr;
    mdl::MorphKeyPoolDestroy(mdl::MorphKeys(m));
    // Registered pool: releases the storage this port allocated (possibly grown
    // past the original capacity) and drops its capacity record.  The record is
    // dropped before the model block itself is freed, so a recycled model
    // address cannot match a stale entry.
    mdl::BoneKeyPoolDestroy(mdl::BoneKeys(m));

    FreeOwned(mdl::Mdl(m)->boneSelection);
    FreeOwned(mdl::Mdl(m)->bonePhysicsState);
    FreeOwned(mdl::Mdl(m)->rbGroups);
    FreeOwned(mdl::Mdl(m)->groupNames);
    FreeOwned(mdl::Mdl(m)->displayFrames);

    // ---- IK records --------------------------------------------------------
    if (model.morphs != nullptr && model.morphCount > 0) {
        const std::uint32_t n = model.morphCount;
        for (std::uint32_t i = 0; i < n; ++i) {
            mdl::MorphRecord& morph = model.morphs[i];
            FreeOwned(morph.jpText);
            FreeOwned(morph.enText);
            FreeOwned(morph.vertexEntries);
            for (auto& entries : morph.uvEntries)
                FreeOwned(entries);
            FreeOwned(morph.boneEntries);
            FreeOwned(morph.groupEntries);
            FreeOwned(morph.materialEntries);
        }
    }

    FreeOwned(mdl::BaseVertexMorphTable(m));
    for (auto& table : mdl::UvMorphTables(m).byFamily)
        FreeOwned(table);
    FreeOwned(mdl::BoneMorphOffsets(m));
    // Defect MB-29: the x64 destructor releases the slot at model+0x2270
    // (read at 0x1400A9311, cleared at 0x1400A9323); the old code instead freed the
    // x86 literal 8732 (0x221C), which falls in padding on both ABIs. The
    // original never writes the slot, so this release is also a no-op there.
    FreeOwned(mdl::Mdl(m)->morphTableGap);
    FreeOwned(mdl::MaterialMorphBase(m));
    FreeOwned(mdl::MaterialMorphAdd(m));
    FreeOwned(mdl::MaterialMorphMul(m));

    FreeOwned(model.morphs);

    // ---- IK chains ---------------------------------------------------------
    if (mdl::Mdl(m)->ikChains != nullptr &&
        mikumikustudio::mdl::Mdl(m)->ikChainCount > 0) {
        const int n = mikumikustudio::mdl::Mdl(m)->ikChainCount;
        for (int i = 0; i < n; ++i)
            FreeOwned(mdl::IkChains(m)[i].links);
    }
    FreeOwned(mdl::Mdl(m)->ikChains);

    FreeOwned(mdl::Mdl(m)->morphKeyCursors);
    FreeOwned(mdl::Mdl(m)->morphTrackActive);
    FreeOwned(mdl::Mdl(m)->boneKeyCursors);
    FreeOwned(mdl::Mdl(m)->boneTrackActive);

    // ---- bones -------------------------------------------------------------
    if (mikumikustudio::mdl::Mdl(m)->boneCount > 0) {
        mikumikustudio::mdl::BoneRecord* bones = mdl::Mdl(m)->boneTable;
        for (std::uint32_t i = 0; i < mikumikustudio::mdl::Mdl(m)->boneCount; ++i) {
            mikumikustudio::mdl::BoneRecord* bone = &bones[i];
            FreeOwned(bone->jpText);
            FreeOwned(bone->enText);
            FreeOwned(bone->ikLinks);
        }
    }
    FreeOwned(mdl::Mdl(m)->boneTable);

    FreeOwned(mdl::Mdl(m)->materials);
    FreeOwned(mdl::Mdl(m)->indices);
    FreeOwned(mdl::Mdl(m)->rawVertices);
    FreeOwned(mdl::Mdl(m)->pmxVertices);
    FreeOwned(mdl::Mdl(m)->boneOrderTable);

    // ---- D3D pool objects (vertex/index buffers) ---------------------------
    ReleaseCom(mdl::Mdl(m)->vertexBuffer2);
    ReleaseCom(mdl::Mdl(m)->vertexBuffer);
    ReleaseCom(mdl::Mdl(m)->indexBuffer);

    FreeOwned(mdl::PmxTextBuffer(m, mdl::PmxTextBufferSlot::japaneseName));
    FreeOwned(mdl::PmxTextBuffer(m, mdl::PmxTextBufferSlot::englishName));
    FreeOwned(mdl::PmxTextBuffer(m, mdl::PmxTextBufferSlot::japaneseComment));
    FreeOwned(mdl::PmxTextBuffer(m, mdl::PmxTextBufferSlot::englishComment));
}

// ---------------------------------------------------------------------------
// VA 0x0040A710 - DeleteModel (was Sub40A710)(model, freeFlag): the
// dispose-and-maybe-free wrapper used by the model-delete command
// (0x47FEF1).  Returns the model pointer in the original; no caller reads
// it, kept void.
// ---------------------------------------------------------------------------
void DeleteModel(unsigned char* model, int freeFlag) {
    ModelDispose(model);                                    // 0x40A713
    if ((freeFlag & 1) != 0)
        std::free(model);                                   // 0x40A720
}

}  // namespace mikumikustudio
