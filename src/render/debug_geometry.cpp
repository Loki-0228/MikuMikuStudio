#include "mikumikustudio/physics_scale.hpp"
// ===========================================================================
// Debug geometry and operation-axis renderer
//   0x00401400..0x00401D93  primitive helpers
//   0x00406950              physics collision debug shapes
//   0x00420F30              accessory/attachment debug shapes
//   0x0042DB10              selected-bone operation axis
//   0x004C4760/0x004C5150   embedded axis.x owner and loader
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "btBulletDynamicsCommon.h"

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_edit_axes.hpp"
#include "mikumikustudio/bone_projection.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"

namespace mikumikustudio {

// Shared with bone dragging so the gizmo and the actual edit operation use
// the same view-space basis.
void ViewWorldAxis(MMDApp* app, int axis, float out[3]);

namespace {

using Matrix = d3dx::D3DXMATRIXF;

// D3DXMATERIAL is { D3DMATERIAL9 MatD3D; LPSTR pTextureFilename; }.  The
// material itself is 68 bytes on both architectures, but the trailing pointer
// pads the record to 72 bytes on x86 and to 80 on x64 (4 bytes of padding
// after the 68-byte material so the 8-byte pointer is aligned).  The x86
// original loops with the 72-byte record size (RVA 0x4C5150); keeping that
// constant on x64 shifted every material after the first by eight bytes, so
// the Z arm (material 1) and the Y arm (material 2) read another material's
// tail as their diffuse colour while the red X arm stayed correct.
// Evidence: res/assets/axis.x - material 0 red (1.0, 0.0, 0.035) spans +X,
// material 1 blue (0.0, 0.043, 1.0) spans -Z, material 2 green
// (0.039, 1.0, 0.0) spans +Y, material 3 yellow is the origin box and
// materials 4/5 are the black outline shells.
#if defined(_WIN64) || defined(_M_X64) || defined(__x86_64__)
constexpr std::size_t kD3DXMaterialStride = 80;
#else
constexpr std::size_t kD3DXMaterialStride = 72;
#endif
static_assert(sizeof(D3DMATERIAL9) == 68, "D3DMATERIAL9 layout");

IDirect3DDevice9* DeviceFromScene(PhysicsScene* scene) {
    D3DRenderer* sub = scene->owner;   // slot 0
    return sub->device;
}

void Identity(Matrix* m) {
    std::memset(m, 0, sizeof(*m));
    m->m[0][0] = 1.0f;
    m->m[1][1] = 1.0f;
    m->m[2][2] = 1.0f;
    m->m[3][3] = 1.0f;
}

void SetDebugColor(PhysicsScene* scene, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b) {
    const DWORD color = D3DCOLOR_ARGB(255, r, g, b);
    // gizmo vertex buffers (scene slots 4/12/20/28)
    IDirect3DVertexBuffer9* const vbs[] = {
        scene->gizmoSphereVB,   // 4
        scene->gizmoCubeVB,     // 12
        scene->gizmoSphere33VB, // 20
        scene->gizmoArrowVB};   // 28
    const UINT lengths[] = {928, 128, 528, 256};
    for (int n = 0; n < 4; ++n) {
        auto* vb = vbs[n];
        unsigned char* data = nullptr;
        // If Lock is refused, the data pointer is null; writing the colour DWORD
        // through it would fault on a lost device. Skip that buffer instead.
        // Defect RB-11.
        if (FAILED(vb->Lock(0, lengths[n], reinterpret_cast<void**>(&data), 0)) ||
            data == nullptr)
            continue;
        for (UINT offset = 12; offset < lengths[n]; offset += 16)
            *reinterpret_cast<DWORD*>(data + offset) = color;
        vb->Unlock();
    }
}

void DrawIndexedLines(PhysicsScene* scene, IDirect3DVertexBuffer9* vb,
                      IDirect3DIndexBuffer9* ib, UINT vertices,
                      UINT primitives) {
    auto* device = DeviceFromScene(scene);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    device->SetStreamSource(0, vb, 0, 16);
    device->SetIndices(ib);
    device->DrawIndexedPrimitive(D3DPT_LINELIST, 0, 0, vertices, 0,
                                 primitives);
}

void DrawSphere(PhysicsScene* scene, float scale, const Matrix& transform) {
    auto& api = d3dx::Get();
    auto* device = DeviceFromScene(scene);
    Matrix oldWorld;
    Matrix world;
    api.scaling(&world, scale, scale, scale);
    api.multiply(&world, &world, &transform);
    device->GetTransform(D3DTS_WORLD,
                         reinterpret_cast<D3DMATRIX*>(&oldWorld));
    api.multiply(&world, &world, &oldWorld);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));
    device->SetTexture(0, nullptr);
    DrawIndexedLines(scene, scene->gizmoSphereVB,   // 4
                     scene->gizmoSphereIB, 58, 120); // 8
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&oldWorld));
}

void DrawBox(PhysicsScene* scene, float x, float y, float z,
             const Matrix& transform) {
    auto& api = d3dx::Get();
    auto* device = DeviceFromScene(scene);
    Matrix oldWorld;
    Matrix world;
    api.scaling(&world, x * 2.0f, y * 2.0f, z * 2.0f);
    api.multiply(&world, &world, &transform);
    device->GetTransform(D3DTS_WORLD,
                         reinterpret_cast<D3DMATRIX*>(&oldWorld));
    api.multiply(&world, &world, &oldWorld);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    device->SetStreamSource(0, scene->gizmoCubeVB, 0, 16);  // 12
    device->SetIndices(scene->gizmoCubeIB);                 // 16
    device->DrawIndexedPrimitive(D3DPT_LINELIST, 0, 0, 8, 0, 12);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&oldWorld));
}

void DrawCapsule(PhysicsScene* scene, float radius, float halfHeight,
                 const Matrix& transform) {
    auto& api = d3dx::Get();
    auto* device = DeviceFromScene(scene);
    Matrix oldWorld;
    Matrix base;
    device->GetTransform(D3DTS_WORLD,
                         reinterpret_cast<D3DMATRIX*>(&oldWorld));
    api.multiply(&base, &transform, &oldWorld);

    Matrix scale;
    Matrix translation;
    Matrix world;
    api.scaling(&scale, -radius, -radius, -radius);
    api.translation(&translation, 0.0f, -halfHeight, 0.0f);
    api.multiply(&world, &scale, &translation);
    api.multiply(&world, &world, &base);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));
    DrawIndexedLines(scene, scene->gizmoSphere33VB,    // 20
                     scene->gizmoSphere33IB, 33, 64);   // 24

    api.scaling(&scale, radius, radius, radius);
    api.translation(&translation, 0.0f, halfHeight, 0.0f);
    api.multiply(&world, &scale, &translation);
    api.multiply(&world, &world, &base);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));
    DrawIndexedLines(scene, scene->gizmoSphere33VB,    // 20
                     scene->gizmoSphere33IB, 33, 64);   // 24

    api.scaling(&world, radius, halfHeight * 2.0f, radius);
    api.multiply(&world, &world, &base);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));
    DrawIndexedLines(scene, scene->gizmoArrowVB,       // 28
                     scene->gizmoIdentityIB, 16, 8);    // 32
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&oldWorld));
}

void DrawSelectionBox(PhysicsScene* scene) {
    auto* device = DeviceFromScene(scene);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    device->SetStreamSource(0, scene->gizmoBoxSelVB, 0, 16);  // 36
    device->SetIndices(scene->gizmoBoxSelIB);                 // 40
    // The 72 was the draw's index count, not its vertex count: the box
    // table holds 16 vertices (kGizmoBoxSel16, a 256-byte buffer). The
    // original passes the index count here too (x64 0x140007713 sets
    // NumVertices to 0x48), so the port deliberately corrects it.
    // Defect RB-08.
    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 16, 0, 24);
}

void BulletTransformToMatrix(const btTransform& source, Matrix* out) {
    Identity(out);
    const btMatrix3x3& basis = source.getBasis();
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out->m[row][column] = basis[column][row];
    const btVector3& origin = source.getOrigin();
    out->m[3][0] = origin.x();
    out->m[3][1] = origin.y();
    out->m[3][2] = origin.z();
}

using BufferPointer = void*(WINAPI*)(void*);
using MeshDrawSubset = HRESULT(WINAPI*)(void*, DWORD);

void ReleaseCom(void* object) {
    if (object != nullptr)
        reinterpret_cast<IUnknown*>(object)->Release();  // vtable slot 2
}

void DrawAxisMesh(void* axis, D3DRenderer* sub) {
    auto* device = sub->device;
    mdl::AccessoryRecord& gizmo = *mdl::Accessory(axis);
    const DWORD count = gizmo.materialCount;
    auto* materials = static_cast<D3DMATERIAL9*>(gizmo.materials);
    for (DWORD i = 0; i < count; ++i) {
        device->SetMaterial(&materials[i]);
        device->SetTexture(0, nullptr);
        reinterpret_cast<MeshDrawSubset>(
            (*reinterpret_cast<void***>(gizmo.mesh))[3])(gizmo.mesh, i);
    }
}

unsigned char* CurrentModel(MMDApp* app) {
    return app->SelectedModel();
}

Matrix* BoneMatrix(mikumikustudio::mdl::BoneRecord* bone) {
    return reinterpret_cast<Matrix*>(bone->matInit);
}

void OrthonormalizeGizmoBasis(Matrix* world) {
    float x[3] = {world->m[0][0], world->m[0][1], world->m[0][2]};
    float y[3] = {world->m[1][0], world->m[1][1], world->m[1][2]};
    const float originalZ[3] = {world->m[2][0], world->m[2][1], world->m[2][2]};
    const auto normalize = [](float value[3]) {
        const float length = std::sqrt(value[0] * value[0] +
                                       value[1] * value[1] +
                                       value[2] * value[2]);
        if (!(length > 1.0e-6f))
            return false;
        for (int i = 0; i < 3; ++i)
            value[i] /= length;
        return true;
    };
    if (!normalize(x)) {
        x[0] = 1.0f; x[1] = 0.0f; x[2] = 0.0f;
    }
    const float projection = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
    for (int i = 0; i < 3; ++i)
        y[i] -= projection * x[i];
    if (!normalize(y)) {
        y[0] = std::fabs(x[0]) < 0.8f ? 1.0f : 0.0f;
        y[1] = std::fabs(x[0]) < 0.8f ? 0.0f : 1.0f;
        y[2] = 0.0f;
        const float fallbackProjection = y[0] * x[0] + y[1] * x[1];
        for (int i = 0; i < 3; ++i)
            y[i] -= fallbackProjection * x[i];
        if (!normalize(y))
            return;
    }
    float z[3] = {x[1] * y[2] - x[2] * y[1],
                  x[2] * y[0] - x[0] * y[2],
                  x[0] * y[1] - x[1] * y[0]};
    const float handedness = z[0] * originalZ[0] +
                             z[1] * originalZ[1] +
                             z[2] * originalZ[2];
    if (handedness < 0.0f)
        for (int i = 0; i < 3; ++i)
            z[i] = -z[i];
    for (int i = 0; i < 3; ++i) {
        world->m[0][i] = x[i];
        world->m[1][i] = y[i];
        world->m[2][i] = z[i];
    }
    world->m[0][3] = world->m[1][3] = world->m[2][3] = 0.0f;
    world->m[3][3] = 1.0f;
}

void AttachToBone(MMDApp* app, Matrix* world, int boneIndex) {
    auto& api = d3dx::Get();
    unsigned char* model = CurrentModel(app);
    auto* bones = mikumikustudio::mdl::Bones(model);
    // The bone index on a rigid-body record comes from the loaded file, and the
    // original code clamped only the lower bound, so an index past the model's own
    // table read beyond it. Both bounds now fall back to the first bone.
    // Defect RB-12.
    const int boneCount =
        static_cast<int>(mikumikustudio::mdl::Mdl(model)->boneCount);
    if (bones == nullptr || boneCount <= 0)
        return;
    mikumikustudio::mdl::BoneRecord* bone =
        &bones[(boneIndex < 0 || boneIndex >= boneCount) ? 0 : boneIndex];
    Matrix translation;
    api.translation(&translation, bone->position[0], bone->position[1],
                    bone->position[2]);
    api.multiply(world, world, &translation);
    api.multiply(world, world, BoneMatrix(bone));
}

}  // namespace

void Sub04B0Init(void* object) {
    mdl::AccessoryRecord& accessory = *mdl::Accessory(object);
    accessory.mesh = nullptr;
    accessory.materials = nullptr;
    accessory.texturePaths = nullptr;
    accessory.textureTypes = nullptr;
    accessory.visible = 1;
    accessory.position[0] = accessory.position[1] =
        accessory.position[2] = 0.0f;
    accessory.rotation[0] = accessory.rotation[1] =
        accessory.rotation[2] = 0.0f;
    accessory.scale = 1.0f;
    accessory.parentModel = -1;
    accessory.parentBone = 0;
    accessory.shadowEnabled = 0;
    accessory.order = 0;
    accessory.additiveBlend = 0;
    accessory.materialCount = 0;
    accessory.opacity = 1.0f;
    accessory.currentMaterial = -1;
    accessory.rowSelected = 0;
}

bool InitAxisMesh(MMDApp* app) {
    mdl::AccessoryRecord& gizmo = *mdl::Accessory(app->state.axisMeshObject);
    ReleaseCom(gizmo.mesh);
    gizmo.mesh = nullptr;

    HRSRC resource = FindResourceA(nullptr, MAKEINTRESOURCEA(0x73), "XFILE");
    DWORD size = SizeofResource(nullptr, resource);
    HGLOBAL loaded = LoadResource(nullptr, resource);
    void* data = LockResource(loaded);
    void* materialBuffer = nullptr;
    DWORD materialCount = 0;
    void* mesh = nullptr;
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    auto& api = d3dx::Get();
    if (!api.Load() || api.loadMeshFromXInMemory(
            data, size, 544, device, nullptr, &materialBuffer, nullptr,
            &materialCount, &mesh) != D3D_OK) {
        MessageBoxA(nullptr, "failed load axis.x from memory!", "", MB_OK);
        return false;
    }

    gizmo.mesh = mesh;
    gizmo.materialCount = materialCount;
    auto* materials = static_cast<D3DMATERIAL9*>(
        ::operator new(68 * materialCount));
    gizmo.materials = materials;
    if (materials == nullptr)
        return false;
    std::memset(materials, 0, 68 * static_cast<std::size_t>(materialCount));
    auto getBufferPointer = reinterpret_cast<BufferPointer>(
        (*reinterpret_cast<void***>(materialBuffer))[3]);
    auto* source = static_cast<unsigned char*>(
        getBufferPointer(materialBuffer));
    for (DWORD i = 0; i < materialCount; ++i)
        std::memcpy(&materials[i], source + kD3DXMaterialStride * i, 68);
    ReleaseCom(materialBuffer);
    return true;
}

void DrawPhysicsCollisionDebug(PhysicsScene* scene) {
    auto* world = scene->world;   // slot 64 / 0x40
    SetDebugColor(scene, 255, 0, 0);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            SetDebugColor(scene, 155, 255, 0);
        const int count = world->getNumCollisionObjects();
        for (int i = 0; i < count; ++i) {
            btCollisionObject* object = world->getCollisionObjectArray()[i];
            btRigidBody* body = btRigidBody::upcast(object);
            if (body == nullptr ||
                ((body->getInvMass() != 0.0f) != (pass == 0)))
                continue;
            const btCollisionShape* shape = physics_scale::BaseShape(body);
            Matrix transform;
            if (!physics_scale::CollisionTransform(body, reinterpret_cast<float*>(&transform)))
                BulletTransformToMatrix(body->getWorldTransform(), &transform);
            if (shape->getShapeType() == SPHERE_SHAPE_PROXYTYPE) {
                auto* sphere = static_cast<const btSphereShape*>(shape);
                DrawSphere(scene, sphere->getRadius(), transform);
            } else if (shape->getShapeType() == BOX_SHAPE_PROXYTYPE) {
                const btVector3 extents =
                    static_cast<const btBoxShape*>(shape)->getHalfExtentsWithMargin();
                DrawBox(scene, extents.x(), extents.y(), extents.z(),
                        transform);
            } else if (shape->getShapeType() == CAPSULE_SHAPE_PROXYTYPE) {
                auto* capsule = static_cast<const btCapsuleShape*>(shape);
                DrawCapsule(scene, capsule->getRadius(),
                            capsule->getHalfHeight(), transform);
            }
        }
    }
}

// Physics-editor scratch-array capacity (must track the dialog's own
// kMaxEditRecords: x86 original 10,000 slots, x64 recompile 100,000).
#if defined(_M_X64) || defined(__x86_64__)
constexpr int kDebugRecordCapacity = 100000;
#else
constexpr int kDebugRecordCapacity = 10000;
#endif

void DrawAccessoryDebug(MMDApp* app) {
    auto& api = d3dx::Get();
    PhysicsScene* scene = app->Physics();   // +650672 (kPtrSub048)
    // The physics-model editor's scratch arrays double as the draw source:
    // rigidScratchArray = rigid bodies (mdl::RigidRecord), jointScratchArray =
    // joints (mdl::JointRecord).  While the dialog is open the records'
    // keyData / constraint slots hold the combo index (-1 = deleted slot).
    auto* bodies =
        static_cast<mikumikustudio::mdl::RigidRecord*>(
            app->state.rigidScratchArray);
    auto* joints =
        static_cast<mikumikustudio::mdl::JointRecord*>(
            app->state.jointScratchArray);
    const bool dialogSelection = app->state.physicsEditorJointPage != 0;
    for (int index = 0; index < kDebugRecordCapacity; ++index) {
        mikumikustudio::mdl::RigidRecord* record = &bodies[index];
        const int comboSlot =
            static_cast<int>(reinterpret_cast<std::intptr_t>(record->keyData));
        if (comboSlot >= 0) {
            Matrix world;
            Matrix temp;
            api.rotZ(&world, record->rotation[2]);
            api.rotX(&temp, record->rotation[0]);
            api.multiply(&world, &world, &temp);
            api.rotY(&temp, record->rotation[1]);
            api.multiply(&world, &world, &temp);
            api.translation(&temp, record->position[0],
                            record->position[1], record->position[2]);
            api.multiply(&world, &world, &temp);
            AttachToBone(app, &world, record->boneIndex);

            bool selected = false;
            if (dialogSelection) {
                const int linkIndex = app->state.selectedJointIndex;
                if (linkIndex >= 0) {
                    const mikumikustudio::mdl::JointRecord* link =
                        &joints[linkIndex];
                    selected = index == link->rigidA ||
                               index == link->rigidB;
                }
            } else {
                selected = index == app->state.selectedRigidIndex;
            }
            if (selected)
                SetDebugColor(scene, 255, dialogSelection ? 75 : 0,
                              dialogSelection ? 75 : 0);

            const std::uint8_t type = record->shape;
            if (type == 0)
                DrawSphere(scene, record->size[0], world);
            else if (type == 1)
                DrawBox(scene, record->size[0], record->size[1],
                        record->size[2], world);
            else
                DrawCapsule(scene, record->size[0],
                            record->size[1] * 0.5f, world);
            if (selected)
                SetDebugColor(scene, 155, 255, 0);
        }

        mikumikustudio::mdl::JointRecord* link = &joints[index];
        if (link->constraint < 0)
            continue;
        Matrix world;
        Matrix temp;
        api.rotZ(&world, link->rotation[2]);
        api.rotX(&temp, link->rotation[0]);
        api.multiply(&world, &world, &temp);
        api.scaling(&temp, 1.5f, 1.5f, 1.5f);
        api.multiply(&world, &temp, &world);
        api.rotY(&temp, link->rotation[1]);
        api.multiply(&world, &world, &temp);
        api.translation(&temp, link->position[0], link->position[1],
                        link->position[2]);
        api.multiply(&world, &world, &temp);
        auto* device = DeviceFromScene(scene);
        Matrix oldWorld;
        device->GetTransform(D3DTS_WORLD,
                             reinterpret_cast<D3DMATRIX*>(&oldWorld));
        api.multiply(&world, &world, &oldWorld);
        device->SetTransform(D3DTS_WORLD,
                             reinterpret_cast<const D3DMATRIX*>(&world));
        if (dialogSelection && index == app->state.selectedJointIndex) {
            device->SetRenderState(D3DRS_ZENABLE, TRUE);
            device->SetRenderState(D3DRS_LIGHTING, TRUE);
            device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0x00FFFFFF, 1.0f, 0);
            DrawAxisMesh(app->state.axisMeshObject,
                         app->Renderer());
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_LIGHTING, FALSE);
        } else {
            DrawSelectionBox(scene);
        }
        device->SetTransform(D3DTS_WORLD,
                             reinterpret_cast<const D3DMATRIX*>(&oldWorld));
    }
}

// ---- MMS addition: scale-mode handles (no original VA) --------------------
//
namespace {

// Placement shared by the selected-bone operation axis mesh and the scale
// handles.  `world` comes back as the unscaled gizmo transform (bone origin,
// orientation for the active coordinate state, then the frame matrix) and
// `unitScale` is the view-distance factor the original applied to it, so both
// gizmos keep one on-screen size and one placement rule.
bool BoneGizmoTransform(MMDApp* app, const float frameMatrix[16],
                        mikumikustudio::mdl::BoneRecord* bone,
                        Matrix& world, float& unitScale,
                        bool viewAxes = false) {
    if (bone == nullptr)
        return false;
    auto& api = d3dx::Get();
    Matrix translation;
    api.translation(&translation, bone->position[0], bone->position[1],
                    bone->position[2]);
    const int coordinateState = viewAxes ? 2 : EffectiveCoordinateState(app);
    if (coordinateState == 1) {
        // In Global mode, the gizmo keeps its own axes and uses the bone only to
        // position its origin, preserving the red X / green Y / blue Z
        // material order from axis.x.
        api.multiply(&world, &translation, BoneMatrix(bone));
        const float x = world.m[3][0];
        const float y = world.m[3][1];
        const float z = world.m[3][2];
        Identity(&world);
        world.m[3][0] = x;
        world.m[3][1] = y;
        world.m[3][2] = z;
    } else if (coordinateState == 2) {
        // View mode is screen-oriented.  Build rows because D3DX's vector
        // transform treats each matrix row as the destination direction for
        // an axis mesh basis vector.
        Matrix positioned;
        api.multiply(&positioned, &translation, BoneMatrix(bone));
        const float x = positioned.m[3][0];
        const float y = positioned.m[3][1];
        const float z = positioned.m[3][2];
        float axes[3][3]{};
        for (int axis = 0; axis < 3; ++axis)
            ViewWorldAxis(app, axis, axes[axis]);
        Identity(&world);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                world.m[row][column] = axes[row][column];
        world.m[3][0] = x;
        world.m[3][1] = y;
        world.m[3][2] = z;
    } else {
        Matrix basis;
        auto* model = CurrentModel(app);
        auto* bones = model ? mdl::Bones(model) : nullptr;
        BoneLocalAxesForBone(model, bones ? static_cast<int>(bone - bones) : -1,
                             &basis.m[0][0]);
        api.multiply(&world, &basis, &translation);
        api.multiply(&world, &world, BoneMatrix(bone));
    }
    const auto* frame = reinterpret_cast<const Matrix*>(frameMatrix);
    api.multiply(&world, &world, frame);
    // Operation handles show orientation, never the selected bone's scale or
    // shear. Keep the bone origin and view-sized unitScale while making their
    // three axes a rigid, orthogonal basis.
    OrthonormalizeGizmoBasis(&world);

    const float px = bone->matInit[0] * bone->position[0] +
                     bone->matInit[4] * bone->position[1] +
                     bone->matInit[8] * bone->position[2] +
                     bone->matInit[12];
    const float py = bone->matInit[1] * bone->position[0] +
                     bone->matInit[5] * bone->position[1] +
                     bone->matInit[9] * bone->position[2] +
                     bone->matInit[13];
    const float pz = bone->matInit[2] * bone->position[0] +
                     bone->matInit[6] * bone->position[1] +
                     bone->matInit[10] * bone->position[2] +
                     bone->matInit[14];
    const float pw = bone->matInit[3] * bone->position[0] +
                     bone->matInit[7] * bone->position[1] +
                     bone->matInit[11] * bone->position[2] +
                     bone->matInit[15];
    const float tx = px * frame->m[0][0] + py * frame->m[1][0] +
                     pz * frame->m[2][0] + pw * frame->m[3][0];
    const float ty = px * frame->m[0][1] + py * frame->m[1][1] +
                     pz * frame->m[2][1] + pw * frame->m[3][1];
    const float tz = px * frame->m[0][2] + py * frame->m[1][2] +
                     pz * frame->m[2][2] + pw * frame->m[3][2];
    const float dx = tx - app->ViewOffsetX();
    const float dy = ty - app->ViewOffsetY();
    const float dz = tz - app->CameraDistance();
    unitScale = std::sqrt(dx * dx + dy * dy + dz * dz) *
                (app->CameraFov() * 0.001f);
    return true;
}

// Gizmo units (the same unit the axis mesh uses; unitScale maps them to world
// units).  The rod reaches past the white square so the square handles sit
// outside it, which is the rule the hit test relies on.
constexpr float kScaleRodLength = 1.0f;
constexpr float kScaleHandleHalf = 0.13f;
// Half extent of a square handle in device pixels at HUD scale 1. The square
// is intentionally larger than before while the complete scale gizmo stays
// compact beside the move gizmo.
constexpr float kScaleHandlePixels = 6.0f;
// Length of a scale rod in device pixels at HUD scale 1 (the HUD preview and
// the fallback when an axis cannot be projected).
constexpr float kScaleRodPixels = 70.0f;
// Distance of the move arrow's tip from the origin in axis.x units (the cone
// apex at x = 2.546, drawn at unitScale).
constexpr float kMoveArmLength = 2.546f;
// Shorten the scale rods relative to the move arrow while giving their terminal
// squares more visual weight. This keeps the overall scale handle compact.
constexpr float kScaleReachRatio = 0.70f;
constexpr float kScalePlaneSide = 0.50f;
constexpr float kScaleRodHalf = 0.02f;
// Keep projected rods and their published hit geometry tied to the same scale.
constexpr float kScaleGizmoSize = 1.5f;
// axis.x draws its blue arm along -Z (see the material table at the top of this
// file), so the Z handle points the same way and the two gizmos line up.
constexpr float kScaleAxisDirection[3][3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, -1.0f}};
constexpr D3DCOLOR kScaleAxisColor[3] = {
    0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu};

struct ScreenHandleVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

// Triangle-list order for one quad {corner0, corner1, corner2, corner3}.
constexpr int kQuadTriangles[6] = {0, 1, 2, 0, 2, 3};

void DrawBoneScaleHandles(MMDApp* app,
                          mikumikustudio::mdl::BoneRecord* bone,
                          const float frameMatrix[16]) {
    Matrix world;
    float unitScale = 1.0f;
    // Bone scale is a view-axis tool (bone_scale::AxisDirection), so its
    // handles always show the screen axes, whatever the caption says.
    if (!BoneGizmoTransform(app, frameMatrix, bone, world, unitScale, true))
        return;
    D3DRenderer* sub = app->Renderer();
    if (sub == nullptr || sub->device == nullptr)
        return;
    auto* device = sub->device;

    // Gizmo-space images of the camera axes: the i-th row of the unscaled
    // transform is the world image of the i-th unit axis, so the dot of a
    // normalized row with a world direction is that direction's component on
    // that axis.  The white square uses these two, which is what keeps it in
    // the view plane in every coordinate state.
    float cameraRight[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 0.0f, 0.0f};
    ViewWorldAxis(app, 0, cameraRight);
    ViewWorldAxis(app, 1, cameraUp);
    float localRight[3] = {0.0f, 0.0f, 0.0f};
    float localUp[3] = {0.0f, 0.0f, 0.0f};
    for (int axis = 0; axis < 3; ++axis) {
        const float* row = world.m[axis];
        const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] +
                                       row[2] * row[2]);
        if (!(length > 1.0e-6f))
            continue;
        localRight[axis] = (row[0] * cameraRight[0] +
                            row[1] * cameraRight[1] +
                            row[2] * cameraRight[2]) / length;
        localUp[axis] = (row[0] * cameraUp[0] + row[1] * cameraUp[1] +
                         row[2] * cameraUp[2]) / length;
    }

    auto& api = d3dx::Get();
    Matrix scaleMatrix;
    Matrix finalWorld;
    const float gizmoScale = unitScale * kScaleGizmoSize;
    api.scaling(&scaleMatrix, gizmoScale, gizmoScale, gizmoScale);
    api.multiply(&finalWorld, &scaleMatrix, &world);

    // Project first: when the origin is behind the eye there is nothing to
    // publish and no render state to disturb.
    D3DMATRIX view{};
    D3DMATRIX projection{};
    device->GetTransform(D3DTS_VIEW, &view);
    device->GetTransform(D3DTS_PROJECTION, &projection);
    const RECT viewport = app->ViewportRect();
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    const D3DMATRIX finalMatrix =
        *reinterpret_cast<const D3DMATRIX*>(&finalWorld);
    const auto project = [&](const float point[3], float* screenX,
                             float* screenY) {
        int x = 0;
        int y = 0;
        float clipW = 0.0f;
        if (!ProjectPoint(point, identity, finalMatrix, view, projection,
                          viewport, &x, &y, &clipW))
            return false;
        *screenX = static_cast<float>(x);
        *screenY = static_cast<float>(y);
        return true;
    };

    bone_scale::gizmo::ScreenGeometry geometry;
    float originX = 0.0f;
    float originY = 0.0f;
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    if (!project(origin, &originX, &originY)) {
        bone_scale::gizmo::Publish(app, geometry);
        return;
    }
    geometry.originX = originX;
    geometry.originY = originY;
    const float planeRight[3] = {kScalePlaneSide * localRight[0],
                                  kScalePlaneSide * localRight[1],
                                  kScalePlaneSide * localRight[2]};
    const float planeUp[3] = {kScalePlaneSide * localUp[0],
                              kScalePlaneSide * localUp[1],
                              kScalePlaneSide * localUp[2]};
    float planeRightX = originX, planeRightY = originY;
    float planeUpX = originX, planeUpY = originY;
    const bool projectedPlaneRight =
        project(planeRight, &planeRightX, &planeRightY);
    const bool projectedPlaneUp = project(planeUp, &planeUpX, &planeUpY);
    const float halfPoint[3] = {kScaleHandleHalf * localRight[0],
                                kScaleHandleHalf * localRight[1],
                                kScaleHandleHalf * localRight[2]};
    // The square handles keep one fixed on-screen size (scaled only by the
    // HUD scale) so they never grow or shrink with the camera angle, the view
    // distance or the hovered state.
    (void)halfPoint;
    geometry.half = std::max(4.0f, kScaleHandlePixels * sub->viewScale);
    const float planeSidePixels = std::max(8.0f,
        geometry.half * (kScalePlaneSide / kScaleHandleHalf));
    if (!projectedPlaneRight) {
        planeRightX = originX + planeSidePixels;
        planeRightY = originY;
    }
    if (!projectedPlaneUp) {
        planeUpX = originX;
        planeUpY = originY - planeSidePixels;
    }
    geometry.planeX = planeRightX + planeUpX - originX;
    geometry.planeY = planeRightY + planeUpY - originY;

    // The visible line is defined by a screen-space start and end point. Keep
    // its nominal on-screen length when an axis points toward/away from the
    // camera instead of letting perspective collapse it into a dot or a strip.
    const float rightTip[3] = {kScaleRodLength * localRight[0],
                               kScaleRodLength * localRight[1],
                               kScaleRodLength * localRight[2]};
    const float upTip[3] = {kScaleRodLength * localUp[0],
                            kScaleRodLength * localUp[1],
                            kScaleRodLength * localUp[2]};
    float rightX = originX, rightY = originY;
    float upX = originX, upY = originY;
    const float rightLength = project(rightTip, &rightX, &rightY)
        ? std::hypot(rightX - originX, rightY - originY) : 0.0f;
    const float upLength = project(upTip, &upX, &upY)
        ? std::hypot(upX - originX, upY - originY) : 0.0f;
    const float minimumLength = std::max({2.0f * geometry.half, 8.0f,
                                          rightLength, upLength});
    float screenDirection[3][2]{};
    float screenLength[3]{};
    float rawLength[3]{};
    bool fallbackAxis[3]{};
    for (int axis = 0; axis < 3; ++axis) {
        const float* direction = kScaleAxisDirection[axis];
        const float endpoint[3] = {direction[0] * kScaleRodLength,
                                    direction[1] * kScaleRodLength,
                                    direction[2] * kScaleRodLength};
        float endX = originX, endY = originY;
        if (project(endpoint, &endX, &endY)) {
            const float dx = endX - originX;
            const float dy = endY - originY;
            const float length = std::hypot(dx, dy);
            rawLength[axis] = length;
            if (length > 0.5f) {
                screenDirection[axis][0] = dx / length;
                screenDirection[axis][1] = dy / length;
                screenLength[axis] = std::max(minimumLength, length);
            } else {
                fallbackAxis[axis] = true;
            }
        } else {
            fallbackAxis[axis] = true;
        }
    }
    static constexpr float kFallbackDirections[8][2] = {
        {1.0f, 0.0f}, {0.7071068f, -0.7071068f}, {0.0f, -1.0f},
        {-0.7071068f, -0.7071068f}, {-1.0f, 0.0f},
        {-0.7071068f, 0.7071068f}, {0.0f, 1.0f},
        {0.7071068f, 0.7071068f}};
    for (int axis = 0; axis < 3; ++axis) {
        if (!fallbackAxis[axis])
            continue;
        int best = 0;
        float bestScore = -1.0f;
        for (int candidate = 0; candidate < 8; ++candidate) {
            float score = 1.0f;
            for (int other = 0; other < 3; ++other) {
                if (other == axis || fallbackAxis[other])
                    continue;
                const float dot = kFallbackDirections[candidate][0] *
                                      screenDirection[other][0] +
                                  kFallbackDirections[candidate][1] *
                                      screenDirection[other][1];
                score = std::min(score, 1.0f - dot);
            }
            if (score > bestScore) {
                bestScore = score;
                best = candidate;
            }
        }
        screenDirection[axis][0] = kFallbackDirections[best][0];
        screenDirection[axis][1] = kFallbackDirections[best][1];
        screenLength[axis] = minimumLength;
        fallbackAxis[axis] = false;
    }
    // Scale rods use a shorter reach than the move arrows. Project the rod and
    // its target with one ratio so camera distance and angle stay consistent.
    // An axis without a usable projection falls back to the longest in-view
    // axis, or to the fixed length.
    const float scaleReach = kMoveArmLength * kScaleReachRatio;
    const float toMoveArm = scaleReach / (kScaleRodLength * kScaleGizmoSize);
    const float nominalProjected = std::max(rightLength, upLength);
    const float fallbackLength = nominalProjected > 0.5f
        ? nominalProjected * toMoveArm
        : kScaleRodPixels * sub->viewScale * kScaleReachRatio;
    // Pull the rod back by the square half-width so the square stays within
    // the scale handle's reduced reach.
    const float squareInset = std::max(4.0f, kScaleHandlePixels * sub->viewScale);
    for (int axis = 0; axis < 3; ++axis)
        screenLength[axis] = std::max(2.0f * squareInset,
            (rawLength[axis] > 0.5f ? rawLength[axis] * toMoveArm : fallbackLength) -
                squareInset);
    // An axis pointing at (or away from) the camera has no meaningful screen
    // direction, so it is drawn as its square alone on the origin, without a
    // rod; its drag falls back to vertical (bone_scale::ScreenDragAmount).
    const float nominalLength = std::max(rightLength, upLength);
    bool endOn[3]{};
    for (int axis = 0; axis < 3; ++axis) {
        endOn[axis] = rawLength[axis] < 0.25f * nominalLength;
        const float length = endOn[axis] ? 0.0f : screenLength[axis];
        geometry.tipX[axis] = geometry.endX[axis] = originX +
            screenDirection[axis][0] * length;
        geometry.tipY[axis] = geometry.endY[axis] = originY +
            screenDirection[axis][1] * length;
        geometry.axisAvailable[axis] = true;
    }
    // The white square spans the coordinate selector's X and Y axes, so it
    // turns with Local / Global / View exactly like the rods and scales that
    // same plane (bone_scale::DragScreenPlane).  Its edges reuse the rods'
    // screen directions, which already carry the end-on fallback, so the
    // square never collapses into a line.
    const float planeRatio = kScalePlaneSide / kScaleRodLength;
    planeRightX = originX + screenDirection[0][0] * screenLength[0] * planeRatio;
    planeRightY = originY + screenDirection[0][1] * screenLength[0] * planeRatio;
    planeUpX = originX + screenDirection[1][0] * screenLength[1] * planeRatio;
    planeUpY = originY + screenDirection[1][1] * screenLength[1] * planeRatio;
    geometry.planeSkewed = true;
    geometry.planeRightX = planeRightX;
    geometry.planeRightY = planeRightY;
    geometry.planeUpX = planeUpX;
    geometry.planeUpY = planeUpY;
    geometry.planeX = planeRightX + planeUpX - originX;
    geometry.planeY = planeRightY + planeUpY - originY;
    // Keep the line hit band wider than its one-pixel painted stroke so it stays
    // clickable when the gizmo is small.
    geometry.rod = std::max(2.0f, geometry.half *
                                      (kScaleRodHalf / kScaleHandleHalf));
    geometry.valid = true;

    DWORD previousLighting = 0;
    DWORD previousBlend = 0;
    DWORD previousSourceBlend = 0;
    DWORD previousDestinationBlend = 0;
    DWORD previousCull = 0;
    DWORD previousZEnable = 0;
    device->GetRenderState(D3DRS_LIGHTING, &previousLighting);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &previousBlend);
    device->GetRenderState(D3DRS_SRCBLEND, &previousSourceBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &previousDestinationBlend);
    device->GetRenderState(D3DRS_CULLMODE, &previousCull);
    device->GetRenderState(D3DRS_ZENABLE, &previousZEnable);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetTexture(0, nullptr);
    const auto vertex = [](float x, float y, D3DCOLOR color) {
        return ScreenHandleVertex{x, y, 0.0f, 1.0f, color};
    };
    const auto drawLine = [&](float x0, float y0, float x1, float y1,
                              D3DCOLOR color) {
        const ScreenHandleVertex line[2] = {vertex(x0, y0, color),
                                            vertex(x1, y1, color)};
        device->DrawPrimitiveUP(D3DPT_LINELIST, 1, line,
                                sizeof(ScreenHandleVertex));
    };
    const auto drawSquare = [&](float centerX, float centerY, float half,
                                float axisX, float axisY, D3DCOLOR color) {
        const float axisLength = std::hypot(axisX, axisY);
        if (!(axisLength > 1.0e-5f)) {
            axisX = 1.0f;
            axisY = 0.0f;
        } else {
            axisX /= axisLength;
            axisY /= axisLength;
        }
        const float sideX = -axisY;
        const float sideY = axisX;
        const float corners[4][2] = {
            {centerX - half * (axisX + sideX),
             centerY - half * (axisY + sideY)},
            {centerX + half * (axisX - sideX),
             centerY + half * (axisY - sideY)},
            {centerX + half * (axisX + sideX),
             centerY + half * (axisY + sideY)},
            {centerX + half * (-axisX + sideX),
             centerY + half * (-axisY + sideY)}};
        ScreenHandleVertex fill[6];
        for (int i = 0; i < 6; ++i)
            fill[i] = vertex(corners[kQuadTriangles[i]][0],
                             corners[kQuadTriangles[i]][1], color);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, fill,
                                sizeof(ScreenHandleVertex));
        for (int i = 0; i < 4; ++i) {
            const int next = (i + 1) % 4;
            drawLine(corners[i][0], corners[i][1], corners[next][0],
                     corners[next][1], 0xFF000000u);
        }
    };

    // Draw the plane and axis lines directly from their projected endpoints.
    // Their orientation follows the rotated coordinate basis, but perspective
    // can no longer turn a line into a thin 3D sheet or cull an axis end-on.
    // The handle under the pointer - or the one the hovered HUD scale cell
    // drives - is painted in the HUD's yellow highlight with a heavier stroke,
    // as the rotate/move rows light their axis.
    const bone_scale::gizmo::Highlight lit =
        bone_scale::gizmo::HighlightOf(app);
    constexpr D3DCOLOR kHighlight = 0xFFFFE000u;
    const auto drawBoldLine = [&](float x0, float y0, float x1, float y1,
                                  D3DCOLOR color) {
        const float length = std::hypot(x1 - x0, y1 - y0);
        const float nx = length > 1.0e-5f ? -(y1 - y0) / length : 0.0f;
        const float ny = length > 1.0e-5f ? (x1 - x0) / length : 1.0f;
        for (int offset = -1; offset <= 1; ++offset)
            drawLine(x0 + nx * offset, y0 + ny * offset,
                     x1 + nx * offset, y1 + ny * offset, color);
    };
    const float planeCorners[4][2] = {
        {originX, originY}, {planeRightX, planeRightY},
        {geometry.planeX, geometry.planeY}, {planeUpX, planeUpY}};
    ScreenHandleVertex planeFill[6];
    for (int i = 0; i < 6; ++i)
        planeFill[i] = vertex(planeCorners[kQuadTriangles[i]][0],
                              planeCorners[kQuadTriangles[i]][1],
                              lit.plane ? 0x88FFE000u : 0x66FFFFFFu);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, planeFill,
                            sizeof(ScreenHandleVertex));
    for (int i = 0; i < 4; ++i) {
        const int next = (i + 1) % 4;
        drawLine(planeCorners[i][0], planeCorners[i][1],
                 planeCorners[next][0], planeCorners[next][1],
                 0xFF000000u);
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (endOn[axis])
            continue;
        if (lit.axis[axis])
            drawBoldLine(originX, originY, geometry.endX[axis],
                         geometry.endY[axis], kHighlight);
        else
            drawLine(originX, originY, geometry.endX[axis],
                     geometry.endY[axis], kScaleAxisColor[axis]);
    }
    for (int axis = 0; axis < 3; ++axis)
        drawSquare(geometry.tipX[axis], geometry.tipY[axis], geometry.half,
                   screenDirection[axis][0], screenDirection[axis][1],
                   lit.axis[axis] ? kHighlight : kScaleAxisColor[axis]);

    device->SetRenderState(D3DRS_LIGHTING, previousLighting);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, previousBlend);
    device->SetRenderState(D3DRS_SRCBLEND, previousSourceBlend);
    device->SetRenderState(D3DRS_DESTBLEND, previousDestinationBlend);
    device->SetRenderState(D3DRS_CULLMODE, previousCull);
    device->SetRenderState(D3DRS_ZENABLE, previousZEnable);
    bone_scale::gizmo::Publish(app, geometry);
}

// ---- MMS addition: single-axis preview of a hovered HUD cell ---------------
// Hovering (or dragging) one of the nine rotate / move / scale X/Y/Z cells of
// the viewport HUD shows only the axis that cell drives on the selected bone,
// in the HUD's yellow highlight with a black outline: a ring around the axis
// for rotate, an arrow along it for move, and a rod ending in a square for
// scale.  All three follow the local / global / view caption: the HUD scale
// row is a three-mode tool, unlike the 缩放 edit mode's view-only handles.
// Sizes are fixed in screen pixels so the preview never grows or shrinks
// with the camera distance.
enum class HudPreviewKind { Rotate, Move, Scale };

// Keep the authored joint increment separate from movement inherited through
// a selected parent. The latter moves the ring, but must not fill its sector.
struct RotateSectorState {
    bool initialized = false;
    float previousRotation[4]{};
    float previousAxis[3]{};
    float previousStart[3]{};
    float swept = 0.0f;
    float drift = 0.0f;
};
struct RotatePreviewState {
    const MMDApp* app = nullptr;
    const void* model = nullptr;
    const void* bones = nullptr;
    int axis = -1;
    int coordinates = -1;
    bool dragging = false;
    std::vector<int> selection;
    std::vector<unsigned char> selectionMask;
    std::vector<RotateSectorState> sectors;
};
RotatePreviewState g_rotatePreviews;

void ClearRotatePreviews() {
    g_rotatePreviews = {};
}

void PrepareRotatePreviews(MMDApp* app, unsigned char* model,
                          const std::vector<int>& selection, int axis,
                          bool dragging) {
    auto& state = g_rotatePreviews;
    const int coordinates = EffectiveCoordinateState(app);
    const auto* record = mdl::Mdl(model);
    const bool sameMask = state.selectionMask.size() == record->boneCount &&
        (record->boneSelection
            ? std::equal(state.selectionMask.begin(), state.selectionMask.end(),
                         record->boneSelection)
            : std::all_of(state.selectionMask.begin(), state.selectionMask.end(),
                          [](unsigned char value) { return value == 0; }));
    if (state.app != app || state.model != model ||
        state.bones != mdl::Bones(model) || state.axis != axis ||
        state.coordinates != coordinates || state.selection != selection || !sameMask ||
        (state.dragging && !dragging)) {
        ClearRotatePreviews();
        state.app = app;
        state.model = model;
        state.bones = mdl::Bones(model);
        state.axis = axis;
        state.coordinates = coordinates;
        state.selection = selection;
        if (record->boneSelection)
            state.selectionMask.assign(record->boneSelection,
                record->boneSelection + record->boneCount);
        else
            state.selectionMask.assign(record->boneCount, 0);
        state.sectors.resize(selection.size());
    }
    state.dragging = dragging;
}

bool NormalizePreviewVector(float v[3]) {
    const double length2 = double(v[0]) * v[0] + double(v[1]) * v[1] +
                           double(v[2]) * v[2];
    if (!(length2 > 1.0e-12) || !std::isfinite(length2))
        return false;
    const double inverse = 1.0 / std::sqrt(length2);
    for (int c = 0; c < 3; ++c)
        v[c] = static_cast<float>(v[c] * inverse);
    return true;
}

// Preserve the existing ring's start direction while aligning its normal to
// the exact production edit axis, including fixed-axis and PMX local axes.
bool AlignRotationPreview(MMDApp* app, unsigned char* model, int index,
                          int axis, const float frameMatrix[16], Matrix& world,
                          float parentAxis[3]) {
    float direction[3]{};
    if (!BoneRotationAxisWorld(app, model, index, axis, direction))
        return false;
    const auto& bone = mdl::Bones(model)[index];
    for (int row = 0; row < 3; ++row)
        parentAxis[row] = bone.matWorld[row * 4] * direction[0] +
            bone.matWorld[row * 4 + 1] * direction[1] +
            bone.matWorld[row * 4 + 2] * direction[2];
    if (!NormalizePreviewVector(parentAxis))
        return false;
    float normal[3]{};
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k)
            normal[c] += direction[k] * frameMatrix[k * 4 + c];
    if (!NormalizePreviewVector(normal))
        return false;
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    float start[3] = {world.m[u][0], world.m[u][1], world.m[u][2]};
    const float along = start[0] * normal[0] + start[1] * normal[1] +
                        start[2] * normal[2];
    for (int c = 0; c < 3; ++c)
        start[c] -= along * normal[c];
    if (!NormalizePreviewVector(start)) {
        int least = 0;
        for (int c = 1; c < 3; ++c)
            if (std::fabs(normal[c]) < std::fabs(normal[least]))
                least = c;
        for (int c = 0; c < 3; ++c)
            start[c] = (c == least ? 1.0f : 0.0f) - normal[least] * normal[c];
        if (!NormalizePreviewVector(start))
            return false;
    }
    const float other[3] = {
        normal[1] * start[2] - normal[2] * start[1],
        normal[2] * start[0] - normal[0] * start[2],
        normal[0] * start[1] - normal[1] * start[0]};
    for (int c = 0; c < 3; ++c) {
        world.m[axis][c] = normal[c];
        world.m[u][c] = start[c];
        world.m[v][c] = other[c];
    }
    return true;
}

float JointRotationIncrement(const float previous[4], const float current[4],
                             const float axis[3]) {
    double p[4], q[4], pLength2 = 0.0, qLength2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        pLength2 += double(previous[i]) * previous[i];
        qLength2 += double(current[i]) * current[i];
    }
    if (!(pLength2 > 1.0e-12) || !(qLength2 > 1.0e-12) ||
        !std::isfinite(pLength2) || !std::isfinite(qLength2))
        return 0.0f;
    for (int i = 0; i < 4; ++i) {
        p[i] = previous[i] / std::sqrt(pLength2);
        q[i] = current[i] / std::sqrt(qLength2);
    }
    // Production applies q = dq * previous (Hamilton product). Recover dq
    // in the parent-local frame, whose previous edit axis is saved per joint.
    const double x = -q[3] * p[0] + q[0] * p[3] - q[1] * p[2] + q[2] * p[1];
    const double y = -q[3] * p[1] + q[0] * p[2] + q[1] * p[3] - q[2] * p[0];
    const double z = -q[3] * p[2] - q[0] * p[1] + q[1] * p[0] + q[2] * p[3];
    const double w = q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] * p[3];
    const double sign = w < 0.0 ? -1.0 : 1.0;
    return static_cast<float>(2.0 * std::atan2(
        sign * (x * axis[0] + y * axis[1] + z * axis[2]), sign * w));
}

void DrawHudAxisPreview(MMDApp* app, mikumikustudio::mdl::BoneRecord* bone,
                        const float frameMatrix[16], HudPreviewKind kind,
                        int axis, bool rotateDragging,
                        RotateSectorState* sector = nullptr,
                        bool secondary = false) {
    const auto invalidateSector = [&] {
        if (sector != nullptr)
            *sector = {};
    };
    if (axis < 0 || axis > 2 || bone == nullptr) {
        invalidateSector();
        return;
    }
    Matrix world;
    float unitScale = 1.0f;
    if (!BoneGizmoTransform(app, frameMatrix, bone, world, unitScale)) {
        invalidateSector();
        return;
    }
    float parentAxis[3]{};
    if (kind == HudPreviewKind::Rotate) {
        auto* model = CurrentModel(app);
        if (!model || !AlignRotationPreview(app, model,
            static_cast<int>(bone - mdl::Bones(model)), axis, frameMatrix,
            world, parentAxis)) {
            invalidateSector();
            return;
        }
    }
    D3DRenderer* sub = app->Renderer();
    if (sub == nullptr || sub->device == nullptr) {
        invalidateSector();
        return;
    }
    auto* device = sub->device;

    auto& api = d3dx::Get();
    Matrix scaleMatrix;
    Matrix finalWorld;
    api.scaling(&scaleMatrix, unitScale, unitScale, unitScale);
    api.multiply(&finalWorld, &scaleMatrix, &world);
    D3DMATRIX view{};
    D3DMATRIX projection{};
    device->GetTransform(D3DTS_VIEW, &view);
    device->GetTransform(D3DTS_PROJECTION, &projection);
    const RECT viewport = app->ViewportRect();
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    const D3DMATRIX finalMatrix =
        *reinterpret_cast<const D3DMATRIX*>(&finalWorld);
    const auto project = [&](const float point[3], float* screenX,
                             float* screenY) {
        int x = 0;
        int y = 0;
        float clipW = 0.0f;
        if (!ProjectPoint(point, identity, finalMatrix, view, projection,
                          viewport, &x, &y, &clipW))
            return false;
        *screenX = static_cast<float>(x);
        *screenY = static_cast<float>(y);
        return true;
    };

    const float origin[3] = {0.0f, 0.0f, 0.0f};
    float originX = 0.0f;
    float originY = 0.0f;
    if (!project(origin, &originX, &originY)) {
        invalidateSector();
        return;
    }
    // Axis screen vectors come from the camera's rotation alone: each axis
    // direction is rotated into view space and its x / y parts are taken
    // (screen y points down).  Projecting probe points instead mixed in the
    // perspective divide and ProjectPoint's whole-pixel rounding, so the ring
    // still changed size slightly as the camera turned.  This orthographic
    // shape is exact; every size below is then a fixed pixel length.
    float vector[3][2]{};
    float longest = 0.0f;
    for (int a = 0; a < 3; ++a) {
        float frame[3];
        for (int c = 0; c < 3; ++c)
            frame[c] = kScaleAxisDirection[a][0] * world.m[0][c] +
                       kScaleAxisDirection[a][1] * world.m[1][c] +
                       kScaleAxisDirection[a][2] * world.m[2][c];
        const float n = std::sqrt(frame[0] * frame[0] + frame[1] * frame[1] +
                                  frame[2] * frame[2]);
        if (!(n > 1.0e-6f))
            continue;
        const float vx = (frame[0] * view._11 + frame[1] * view._21 +
                          frame[2] * view._31) / n;
        const float vy = (frame[0] * view._12 + frame[1] * view._22 +
                          frame[2] * view._32) / n;
        vector[a][0] = vx;
        vector[a][1] = -vy;
        longest = std::max(longest, std::hypot(vx, vy));
    }
    if (!(longest > 1.0e-4f)) {
        invalidateSector();
        return;
    }
    const float pixel = sub->viewScale > 0.0f ? sub->viewScale : 1.0f;
    // Unit axes: a full-length axis spans kScaleRodPixels, a foreshortened
    // one proportionally less, independent of which axis is longest.
    const float factor = kScaleRodPixels * pixel;
    for (auto& v : vector) {
        v[0] *= factor;
        v[1] *= factor;
    }

    DWORD previousLighting = 0;
    DWORD previousBlend = 0;
    DWORD previousSourceBlend = 0;
    DWORD previousDestinationBlend = 0;
    DWORD previousCull = 0;
    DWORD previousZEnable = 0;
    device->GetRenderState(D3DRS_LIGHTING, &previousLighting);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &previousBlend);
    device->GetRenderState(D3DRS_SRCBLEND, &previousSourceBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &previousDestinationBlend);
    device->GetRenderState(D3DRS_CULLMODE, &previousCull);
    device->GetRenderState(D3DRS_ZENABLE, &previousZEnable);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetTexture(0, nullptr);

    const D3DCOLOR kYellow = secondary ? 0xC0FFE000u : 0xFFFFE000u;
    const D3DCOLOR kOutline = secondary ? 0xC0000000u : 0xFF000000u;
    std::vector<ScreenHandleVertex> triangles;
    triangles.reserve(1024);
    bool directTriangle = false;
    const auto vertex = [](float x, float y, D3DCOLOR color) {
        return ScreenHandleVertex{x, y, 0.0f, 1.0f, color};
    };
    const auto triangle = [&](float x0, float y0, float x1, float y1,
                              float x2, float y2, D3DCOLOR color) {
        const ScreenHandleVertex v[3] = {vertex(x0, y0, color),
                                         vertex(x1, y1, color),
                                         vertex(x2, y2, color)};
        if (directTriangle)
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v,
                                    sizeof(ScreenHandleVertex));
        else
            triangles.insert(triangles.end(), v, v + 3);
    };
    // A stroke of half width `half`, extended by `half` past both ends so
    // consecutive ring segments join without gaps.
    const auto stroke = [&](float x0, float y0, float x1, float y1,
                            float half, D3DCOLOR color) {
        const float length = std::hypot(x1 - x0, y1 - y0);
        if (!(length > 1.0e-3f))
            return;
        const float dx = (x1 - x0) / length;
        const float dy = (y1 - y0) / length;
        const float nx = -dy * half;
        const float ny = dx * half;
        const float ax = x0 - dx * half, ay = y0 - dy * half;
        const float bx = x1 + dx * half, by = y1 + dy * half;
        triangle(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, color);
        triangle(ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny, color);
    };
    const auto square = [&](float cx, float cy, float half, D3DCOLOR color) {
        triangle(cx - half, cy - half, cx + half, cy - half,
                 cx + half, cy + half, color);
        triangle(cx - half, cy - half, cx + half, cy + half,
                 cx - half, cy + half, color);
    };
    const float line = (secondary ? 1.0f : 1.5f) * pixel;
    const float edge = (secondary ? 0.75f : 1.5f) * pixel;

    const float axisX = vector[axis][0];
    const float axisY = vector[axis][1];
    const float axisLength = std::hypot(axisX, axisY);
    // An axis facing the camera has no usable screen direction.
    const bool endOn = axisLength < 0.25f * kScaleRodPixels * pixel;

    if (kind == HudPreviewKind::Rotate) {
        // The circle in the plane of the two other axes, projected through
        // their screen vectors, with a short tick along the rotation axis.
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        constexpr int kSegments = 64;
        // The ring keeps one fixed on-screen size: its projected ellipse is
        // rescaled so the major semi-axis is always kRingPixels, whatever the
        // camera angle, and only the ellipse's tilt and flattening change.
        // Major semi-axis of c*U + s*V: sqrt of the larger eigenvalue of
        // [[U.U, U.V], [U.V, V.V]].
        const float kRingPixels = 56.0f * (secondary ? 0.65f : 1.0f);
        const float uu = vector[u][0] * vector[u][0] + vector[u][1] * vector[u][1];
        const float vv = vector[v][0] * vector[v][0] + vector[v][1] * vector[v][1];
        const float uv = vector[u][0] * vector[v][0] + vector[u][1] * vector[v][1];
        const float half = 0.5f * (uu - vv);
        const float major = std::sqrt(std::max(0.0f,
            0.5f * (uu + vv) + std::sqrt(half * half + uv * uv)));
        const float kRadius = major > 1.0e-3f ? kRingPixels * pixel / major : 0.0f;
        float ring[kSegments + 1][2];
        for (int i = 0; i <= kSegments; ++i) {
            const float angle = 6.2831853f * static_cast<float>(i) / kSegments;
            const float c = std::cos(angle) * kRadius;
            const float s = std::sin(angle) * kRadius;
            ring[i][0] = originX + c * vector[u][0] + s * vector[v][0];
            ring[i][1] = originY + c * vector[u][1] + s * vector[v][1];
        }

        // Ring axes in frame space: the orthonormal gizmo rows mapped through
        // the same axis.x directions the ring is drawn with.
        const auto frameDirection = [&](int a, float out[3]) {
            for (int c = 0; c < 3; ++c)
                out[c] = kScaleAxisDirection[a][0] * world.m[0][c] +
                         kScaleAxisDirection[a][1] * world.m[1][c] +
                         kScaleAxisDirection[a][2] * world.m[2][c];
            const float n = std::sqrt(out[0] * out[0] + out[1] * out[1] +
                                      out[2] * out[2]);
            if (n > 1.0e-6f)
                for (int c = 0; c < 3; ++c)
                    out[c] /= n;
        };
        float A[3], U[3], V[3];
        frameDirection(axis, A);
        frameDirection(u, U);
        frameDirection(v, V);
        const auto dot = [](const float* a, const float* b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        const auto cross = [](const float* a, const float* b, float* o) {
            o[0] = a[1] * b[2] - a[2] * b[1];
            o[1] = a[2] * b[0] - a[0] * b[2];
            o[2] = a[0] * b[1] - a[1] * b[0];
        };
        // Ring parameter theta runs from U toward V; a positive angle about A
        // runs from U toward A x U, so orient by their agreement.
        float AxU[3];
        cross(A, U, AxU);
        const float orientation = dot(AxU, V) >= 0.0f ? 1.0f : -1.0f;
        // Forward (mouse moved up / away) rotation direction.  The drag
        // (frame_modes_bone.cpp, modes 4..6) post-multiplies the bone
        // quaternion by (sin a * axis, cos a) through D3DXQuaternionMultiply:
        // a rotation of 2a about the coordinate axis, a > 0 when the mouse
        // moves up, and D3DX turns a positive angle from u toward axis x u.
        // The ring's A is that axis for X and Y, but axis.x draws Z along -Z,
        // so forward is +A for X/Y and -A for Z.
        const float forward = (axis == 2 ? -1.0f : 1.0f) * orientation;
        const auto ringPoint = [&](float theta, float* x, float* y) {
            const float c = std::cos(theta) * kRadius;
            const float s = std::sin(theta) * kRadius;
            *x = originX + c * vector[u][0] + s * vector[v][0];
            *y = originY + c * vector[u][1] + s * vector[v][1];
        };

        // ---- swept-angle sector while the rotate drag runs ----------------
        if (sector != nullptr) {
            // Signed angle from `from` to `to` about A, after removing the A
            // component of both.
            const auto angleAbout = [&](const float* from, const float* to) {
                float f[3], t[3];
                const float fa = dot(from, A), ta = dot(to, A);
                for (int c = 0; c < 3; ++c) {
                    f[c] = from[c] - fa * A[c];
                    t[c] = to[c] - ta * A[c];
                }
                float x[3];
                cross(f, t, x);
                return std::atan2(dot(x, A), dot(f, t));
            };
            if (rotateDragging && sector->initialized) {
                const float directionSign = axis == 2 ? -1.0f : 1.0f;
                sector->swept += directionSign * JointRotationIncrement(
                    sector->previousRotation, bone->rotQuat,
                    sector->previousAxis);
                // The ring's own start direction turning with the bone.
                sector->drift += angleAbout(sector->previousStart, U);
            } else {
                sector->swept = 0.0f;
                sector->drift = 0.0f;
            }
            sector->initialized = true;
            std::memcpy(sector->previousRotation, bone->rotQuat,
                        sizeof(sector->previousRotation));
            std::memcpy(sector->previousAxis, parentAxis, sizeof(parentAxis));
            std::memcpy(sector->previousStart, U, sizeof(U));

            if (rotateDragging) {
                const float start = -sector->drift * orientation;
                float sweep = sector->swept * orientation;
                constexpr float kTau = 6.2831853f;
                sweep = std::max(-kTau, std::min(kTau, sweep));
                const int steps = std::max(1, static_cast<int>(
                    std::fabs(sweep) / kTau * kSegments + 0.5f));
                if (std::fabs(sweep) > 1.0e-3f) {
                    float previousX = 0.0f, previousY = 0.0f;
                    ringPoint(start, &previousX, &previousY);
                    for (int i = 1; i <= steps; ++i) {
                        float x = 0.0f, y = 0.0f;
                        ringPoint(start + sweep * static_cast<float>(i) / steps,
                                  &x, &y);
                        triangle(originX, originY, previousX, previousY, x, y,
                                 0x70FFE000u);
                        previousX = x;
                        previousY = y;
                    }
                }
                // Start and current edges of the sector.
                float startX = 0.0f, startY = 0.0f, endX = 0.0f, endY = 0.0f;
                ringPoint(start, &startX, &startY);
                ringPoint(start + sweep, &endX, &endY);
                stroke(originX, originY, startX, startY, 1.0f * pixel + edge,
                       kOutline);
                stroke(originX, originY, startX, startY, 1.0f * pixel,
                       0xFFFFFFFFu);
                stroke(originX, originY, endX, endY, line + edge, kOutline);
                stroke(originX, originY, endX, endY, line, kYellow);
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            const float half = pass == 0 ? line + edge : line;
            const D3DCOLOR color = pass == 0 ? kOutline : kYellow;
            for (int i = 0; i < kSegments; ++i)
                stroke(ring[i][0], ring[i][1], ring[i + 1][0], ring[i + 1][1],
                       half, color);
            if (!endOn)
                stroke(originX - axisX * 0.3f, originY - axisY * 0.3f,
                       originX + axisX * 0.3f, originY + axisY * 0.3f,
                       half * 0.7f, color);
        }
        // Two arrowheads on opposite sides of the ring point the way the bone
        // turns when the mouse moves forward (up).  The tangent is taken from
        // the projected ellipse, so the heads follow its tilt and flattening.
        for (int side = 0; side < 2; ++side) {
            const float theta = 0.785398f + 3.1415927f * side;
            float px = 0.0f, py = 0.0f, qx = 0.0f, qy = 0.0f;
            ringPoint(theta, &px, &py);
            ringPoint(theta + 0.05f * forward, &qx, &qy);
            float tx = qx - px, ty = qy - py;
            const float tl = std::hypot(tx, ty);
            if (!(tl > 1.0e-4f))
                continue;
            tx /= tl;
            ty /= tl;
            const float nx = -ty, ny = tx;
            const auto head = [&](float size, D3DCOLOR color) {
                const float length = size * 1.6f;
                triangle(px + tx * length, py + ty * length,
                         px + nx * size, py + ny * size,
                         px - nx * size, py - ny * size, color);
            };
            const float headScale = secondary ? 0.65f : 1.0f;
            head(9.0f * pixel * headScale, kOutline);
            head(6.5f * pixel * headScale, kYellow);
        }
    } else if (endOn) {
        // Move / scale along an axis that points at the camera: a marker on
        // the origin instead of a rod with no direction.
        square(originX, originY, 7.0f * pixel, kOutline);
        square(originX, originY, 5.0f * pixel, kYellow);
    } else {
        const float dx = axisX / axisLength;
        const float dy = axisY / axisLength;
        const float tipX = originX + axisX;
        const float tipY = originY + axisY;
        if (kind == HudPreviewKind::Move) {
            const float head = 14.0f * pixel;
            const float wing = 7.0f * pixel;
            const float baseX = tipX - dx * head;
            const float baseY = tipY - dy * head;
            const float nx = -dy;
            const float ny = dx;
            stroke(originX, originY, baseX, baseY, line + edge, kOutline);
            triangle(tipX + dx * 2.0f * pixel, tipY + dy * 2.0f * pixel,
                     baseX - dx * pixel + nx * (wing + 2.0f * pixel),
                     baseY - dy * pixel + ny * (wing + 2.0f * pixel),
                     baseX - dx * pixel - nx * (wing + 2.0f * pixel),
                     baseY - dy * pixel - ny * (wing + 2.0f * pixel),
                     kOutline);
            stroke(originX, originY, baseX, baseY, line, kYellow);
            triangle(tipX, tipY, baseX + nx * wing, baseY + ny * wing,
                     baseX - nx * wing, baseY - ny * wing, kYellow);
        } else {
            stroke(originX, originY, tipX, tipY, line + edge, kOutline);
            stroke(originX, originY, tipX, tipY, line, kYellow);
            square(tipX, tipY, 7.0f * pixel, kOutline);
            square(tipX, tipY, 5.0f * pixel, kYellow);
        }
    }
    // The bone origin, so the preview reads as attached to the bone.
    square(originX, originY, 3.0f * pixel, 0xFF000000u);
    if (!triangles.empty())
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(triangles.size() / 3), triangles.data(),
            sizeof(ScreenHandleVertex));
    // Keep the two origin triangles separate: the production scene observer
    // uses these final draws to verify the bone anchor at every render scale.
    directTriangle = true;
    square(originX, originY, 2.0f * pixel, 0xFFFFE000u);

    device->SetRenderState(D3DRS_LIGHTING, previousLighting);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, previousBlend);
    device->SetRenderState(D3DRS_SRCBLEND, previousSourceBlend);
    device->SetRenderState(D3DRS_DESTBLEND, previousDestinationBlend);
    device->SetRenderState(D3DRS_CULLMODE, previousCull);
    device->SetRenderState(D3DRS_ZENABLE, previousZEnable);
}

}  // namespace

void DrawBoneOperationAxis(MMDApp* app, const float frameMatrix[16]) {
    unsigned char* model = CurrentModel(app);
    // Deliberate hardening: the ledger records no VA for this access, and the
    // original call site was never located, so the port adds this guard intentionally
    // rather than assuming the original had none. The scene is drawn every frame
    // even with no model loaded, while the sibling call sites reading the same
    // selection (sprite_overlay.cpp, frame_driver.cpp) already check the model
    // pointer.
    // Defect RB-03.
    if (model == nullptr) {
        ClearRotatePreviews();
        bone_scale::gizmo::Publish(app, {});
        return;
    }
    const int selected = mikumikustudio::mdl::Mdl(model)->selectedBone;
    if (selected < 0) {
        ClearRotatePreviews();
        bone_scale::gizmo::Publish(app, {});
        return;
    }
    auto* bones = mikumikustudio::mdl::Bones(model);
    // pmm_load_v1 reads selectedBone directly from the file without validating it,
    // so an index beyond the model's own table can reach this draw; keep the
    // lookup within that table.
    // Defect RB-12.
    const int boneCount =
        static_cast<int>(mikumikustudio::mdl::Mdl(model)->boneCount);
    if (bones == nullptr || selected >= boneCount) {
        ClearRotatePreviews();
        bone_scale::gizmo::Publish(app, {});
        return;
    }
    mikumikustudio::mdl::BoneRecord* bone = &bones[selected];

    // MMS addition: scale mode draws its own handles instead of the operation
    // axis mesh.  Every bone type scales (BONE_SCALING.md), so the FixedAxis
    // filters below do not apply, and the arrow mesh is replaced outright.
    // Anything that disables scaling - the camera/light/accessory panel,
    // playback, no valid selection - publishes an unusable geometry, which
    // turns the handles off for the input path as well.  A hovered or dragged
    // X/Y/Z cell of the HUD's rotate / move / scale rows instead shows only
    // the axis that cell drives (DrawHudAxisPreview), in every edit mode.
    const int hoveredOperation = static_cast<int>(app->ViewportToolOperation());
    const int activeDrag = static_cast<int>(app->InteractionDragMode());
    int previewAxis = -1;
    HudPreviewKind previewKind = HudPreviewKind::Move;
    if (activeDrag >= 4 && activeDrag <= 6) {
        previewKind = HudPreviewKind::Rotate;
        previewAxis = activeDrag - 4;
    } else if (activeDrag >= 10 && activeDrag <= 12) {
        previewKind = HudPreviewKind::Move;
        previewAxis = activeDrag - 10;
    } else if (!MMD_ORIGINAL_PROFILE && activeDrag >= 20 && activeDrag <= 22) {
        previewKind = HudPreviewKind::Scale;
        previewAxis = activeDrag - 20;
    } else if (activeDrag == 0) {
        if (hoveredOperation >= 9 && hoveredOperation <= 11) {
            previewKind = HudPreviewKind::Rotate;
            previewAxis = hoveredOperation - 9;
        } else if (hoveredOperation >= 12 && hoveredOperation <= 14) {
            previewKind = HudPreviewKind::Move;
            previewAxis = hoveredOperation - 12;
        } else if (!MMD_ORIGINAL_PROFILE && hoveredOperation >= 23 && hoveredOperation <= 25 &&
                   bone_scale::Enabled(app)) {
            previewKind = HudPreviewKind::Scale;
            previewAxis = hoveredOperation - 23;
        }
    }
    if (previewAxis >= 0) {
        bone_scale::gizmo::Publish(app, bone_scale::gizmo::ScreenGeometry{});
        if (previewKind == HudPreviewKind::Rotate) {
            std::vector<int> targets;
            const auto* selection = mdl::Mdl(model)->boneSelection;
            for (int i = 0; selection != nullptr && i < boneCount; ++i)
                if (i != selected && selection[i] && BoneRotatesInPlace(bones[i].type))
                    targets.push_back(i);
            // Keep the active joint's full-size preview on top, and retain the
            // final origin marker used by the production anchoring regression.
            targets.push_back(selected);
            const bool dragging = activeDrag >= 4 && activeDrag <= 6;
            PrepareRotatePreviews(app, model, targets, previewAxis, dragging);
            for (std::size_t i = 0; i < targets.size(); ++i)
                DrawHudAxisPreview(app, &bones[targets[i]], frameMatrix,
                    previewKind, previewAxis, dragging,
                    &g_rotatePreviews.sectors[i], targets[i] != selected);
            return;
        }
        ClearRotatePreviews();
        // Move keeps its original fixed-axis filter; every bone scales.
        const bool fixedAxis =
            bone->type == mdl::BoneType::FixedAxis ||
            (bone->type == mdl::BoneType::UnderIk &&
             (bone->flags & mdl::kBoneFlagFixedAxis) ==
                 mdl::kBoneFlagFixedAxis);
        if (previewKind == HudPreviewKind::Scale || !fixedAxis)
            DrawHudAxisPreview(app, bone, frameMatrix, previewKind,
                               previewAxis, false);
        return;
    }
    // No rotate preview this frame: drop the sector's drag state.
    ClearRotatePreviews();
    // The HUD uniform scale cell drives every axis at once, so its preview is
    // the whole scale gizmo, fully highlighted (gizmo::HighlightOf), in every
    // edit mode.  Outside scale mode its published geometry stays inert:
    // gizmo::Active also requires the scale edit mode.
    const bool uniformPreview = !MMD_ORIGINAL_PROFILE &&
        (activeDrag == static_cast<int>(ViewportDragMode::ScaleUniform) ||
         (activeDrag == 0 &&
          hoveredOperation ==
              static_cast<int>(ViewportToolAction::ScaleUniform))) &&
        bone_scale::Enabled(app);
    if (uniformPreview) {
        DrawBoneScaleHandles(app, bone, frameMatrix);
        return;
    }
    if (!MMD_ORIGINAL_PROFILE && app->EditMode() == ViewportEditMode::Scale && bone_scale::Enabled(app)) {
        DrawBoneScaleHandles(app, bone, frameMatrix);
        return;
    }
    bone_scale::gizmo::Publish(app, bone_scale::gizmo::ScreenGeometry{});

    const mdl::BoneType type = bone->type;
    const int operation = static_cast<int>(app->ViewportToolOperation());
    const int mode = static_cast<int>(app->InteractionDragMode());
    const bool scaling = !MMD_ORIGINAL_PROFILE && ((operation >= 22 && operation <= 25) || (mode >= 19 && mode <= 22));
    if (!scaling && (type == mdl::BoneType::FixedAxis ||
        (type == mdl::BoneType::UnderIk &&
         (bone->flags & mdl::kBoneFlagFixedAxis) == mdl::kBoneFlagFixedAxis)))
        return;
    if (!(scaling || (operation >= 9 && operation <= 14) ||
          (mode >= 4 && mode <= 7) || (mode >= 10 && mode <= 12)))
        return;

    auto& api = d3dx::Get();
    Matrix world;
    float unitScale = 1.0f;
    if (!BoneGizmoTransform(app, frameMatrix, bone, world, unitScale))
        return;
    Matrix scaleMatrix;
    api.scaling(&scaleMatrix, unitScale, unitScale, unitScale);
    api.multiply(&world, &scaleMatrix, &world);
    auto* device = DeviceFromScene(app->Physics());
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));
    device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0x00FFFFFF, 1.0f, 0);
    DrawAxisMesh(app->state.axisMeshObject, app->Renderer());
}

namespace {
bool CameraTrackView(MMDApp* app) {
    // Video/image-sequence playback samples camera keys even in model mode.
    // A single-image capture and manual stepping keep the editing viewpoint.
    const bool recording = app->RecordingWindow() != nullptr &&
        (app->FrameStepPlayback() != 0 || app->state.recordPlaybackStartPending != 0);
    return recording || app->CameraMode() != 0 || app->state.followCameraEnabled != 0;
}
}

// The world pivot comes from the camera track during recording, camera-panel
// editing or tracking. Free model editing uses its selected model/bone
// reference, or the world origin when no reference is selected.
FrameFocusSource ResolveFrameFocus(MMDApp* app, int referenceMode,
                                   float focus[3]) {
    focus[0] = focus[1] = focus[2] = 0.0f;
    const bool cameraView = CameraTrackView(app);
    const bool attachedView = cameraView && app->CameraParentModel() >= 0;
    if (!cameraView && referenceMode != 0) {
        const auto* model = mdl::Mdl(app->SelectedModel());
        if (model && model->boneTable && model->boneCount > 0) {
            const int bone = referenceMode == 1 ? 0 : model->selectedBone;
            if (bone >= 0 && bone < static_cast<int>(model->boneCount)) {
                ModelViewReferencePoint(app, referenceMode, focus);
                return FrameFocusSource::ModelReference;
            }
        }
    }
    if (attachedView) {
        const int targetModel = app->CameraParentModel();
        const int targetBone = app->CameraParentBone();
        if (targetModel >= 0 && targetModel < kModelSlotCount) {
            auto* model = mdl::Mdl(app->ModelSlot(targetModel));
            if (model && model->boneTable && targetBone >= 0 &&
                targetBone < static_cast<int>(model->boneCount)) {
                const auto& bone = model->boneTable[targetBone];
                const float x = bone.position[0];
                const float y = bone.position[1];
                const float z = bone.position[2];
                focus[0] = bone.matInit[0] * x + bone.matInit[4] * y +
                           bone.matInit[8] * z + bone.matInit[12];
                focus[1] = bone.matInit[1] * x + bone.matInit[5] * y +
                           bone.matInit[9] * z + bone.matInit[13];
                focus[2] = bone.matInit[2] * x + bone.matInit[6] * y +
                           bone.matInit[10] * z + bone.matInit[14];
                return FrameFocusSource::AttachmentBone;
            }
        }
    }
    if (cameraView) {
        focus[0] = app->CameraPositionX();
        focus[1] = app->CameraPositionY();
        focus[2] = app->CameraPositionZ();
        return FrameFocusSource::CameraPosition;
    }
    // Free model view. The world remains at the origin, and the entire viewpoint
    // lives in the eye, which the frame builds as (viewOffsetX, viewOffsetY,
    // cameraDistance). This matches the original's translation for reference mode
    // None: x64 0x14002797A writes MatrixTranslation(0, 0, 0) before the fixed
    // +Z look-at, whose eye is read from app+0x340/+0x344/+0xA1900
    // (0x140027FB0..0x140027FDE). Translating the world by the view offsets as
    // well - as the port did - counts every pan twice, since the eye and the
    // pivot then read the same pair of fields, and it turns the panel switch
    // into a depth jump: the camera panel rotates about cameraPos, while this
    // branch keeps the fixed world origin.
    focus[0] = focus[1] = focus[2] = 0.0f;
    return FrameFocusSource::FreeView;
}

void SetupFrameWorldTransform(MMDApp* app) {
    auto& api = d3dx::Get();
    Matrix translation;
    Matrix rotationX;
    Matrix rotationY;
    Matrix rotationZ;
    Matrix rotation;
    Matrix attachment;
    Identity(&attachment);

    // The camera panel owns the rendered viewpoint (measured on the reference
    // 9.26 x64 image at startup: cameraPos (0, 10, 0), distance -45, zero
    // angles, and the ground origin projecting `atan(10/45)` below the
    // viewport centre - exactly the camera view, with playback stopped).  The
    // free view is therefore a MODEL-EDITING view only, and every viewport
    // gesture in the camera panel must act on cameraPos / cameraDistance,
    // which is what ConsumeRightButtonDrag / ConsumeMiddleButtonPan already
    // do.  Gating the camera view on playback left those gestures writing
    // state that was not rendered, so panning and dollying the camera looked
    // dead while only the shared yaw/pitch still moved the image.
    // An explicitly parented camera applies its attachment, whether or not the
    // follow switch is on.  The original tests exactly this: the pan block at
    // 0x14002DAF0 branches on `cameraParentModel >= 0` (app+0xa1404) and then
    // rotates the screen delta through the parent bone's basis into cameraPos
    // (app+0x36c/0x370), never moving cameraPos.z.  Gating on `tracking` alone
    // left a parented camera in the camera panel rendering as if it had no
    // parent: SetupFrameWorldTransform fell through to the camera-position
    // pivot, which takes the point straight from cameraPos.
    //
    // The free view (model editing, no following) still refuses to resolve the
    // parent: the parent stays stored after switching to model editing, and
    // using it there snaps the viewport to that bone and overwrites every
    // pan/dolly gesture.
    // Export uses the sampled camera's parent regardless of the editor panel.
    // The untracked model editing view still ignores a retained camera binding.
    const bool attachedView = CameraTrackView(app) && app->CameraParentModel() >= 0;
    const int targetModel = attachedView ? app->CameraParentModel() : -1;
    const int targetBone = app->CameraParentBone();
    mdl::BoneRecord* target = nullptr;
    if (attachedView && targetModel >= 0 && targetModel < kModelSlotCount) {
        auto* model = mdl::Mdl(app->ModelSlot(targetModel));
        if (model && model->boneTable && targetBone >= 0 &&
            targetBone < static_cast<int>(model->boneCount))
            target = &model->boneTable[targetBone];
    }
    // The pivot comes from ResolveFrameFocus, the same function read by the
    // reference-mode re-anchor, so a reference switch and the following frame
    // agree on which point moved. The attached eye is the camera's relative
    // offset; the parent point belongs only to this world translation.
    float focus[3] = {0.0f, 0.0f, 0.0f};
    ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focus);
    api.translation(&translation, -focus[0], -focus[1], -focus[2]);

    api.rotY(&rotationY, app->CameraYaw());
    api.rotX(&rotationX, app->CameraPitch());
    api.multiply(&rotation, &rotationY, &rotationX);
    api.rotZ(&rotationZ, app->CameraRoll());
    api.multiply(&rotation, &rotation, &rotationZ);

    if (attachedView) {
        if (target != nullptr &&
            app->CameraAttachmentTransformSuppressed() == 0) {
            // Inverse of the bone's rotation (9.26 x64 RVA 0x27A70).
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    attachment.m[row][column] = target->matInit[column * 4 + row];
            attachment.m[0][3] = 0.0f;
            attachment.m[1][3] = 0.0f;
            attachment.m[2][3] = 0.0f;
            attachment.m[3][0] = 0.0f;
            attachment.m[3][1] = 0.0f;
            attachment.m[3][2] = 0.0f;
            attachment.m[3][3] = 1.0f;
        }
    }
    api.multiply(&rotation, &attachment, &rotation);
    std::memcpy(&app->ViewRotationTransform(), &rotation, sizeof(rotation));

    // Scene lighting belongs to the scene, including the free model view.
    // Transform its direction with the same view rotation used by geometry.
    app->ApplyTimelineLightState();
    const float* const timelineDirection = app->LightDirection();
    D3DVECTOR& deviceDirection = app->SceneLight().Direction;
    deviceDirection.x = timelineDirection[0] * rotation.m[0][0] +
                        timelineDirection[1] * rotation.m[1][0] +
                        timelineDirection[2] * rotation.m[2][0];
    deviceDirection.y = timelineDirection[0] * rotation.m[0][1] +
                        timelineDirection[1] * rotation.m[1][1] +
                        timelineDirection[2] * rotation.m[2][1];
    deviceDirection.z = timelineDirection[0] * rotation.m[0][2] +
                        timelineDirection[1] * rotation.m[1][2] +
                        timelineDirection[2] * rotation.m[2][2];

    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    device->SetLight(0, &app->SceneLight());
    Matrix world;
    api.multiply(&world, &translation, &rotation);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&world));

    // 0x46BC20..0x46BC9F / x64 0x140027E96..0x140027F1F: the projection the
    // frame installs.  The original rebuilds ONE of the two matrices here -
    // the orthographic one while the パース byte (cameraPerspective, x64
    // app+0x354) is set, and otherwise leaves the perspective matrix that the
    // camera panels installed.  The port rebuilds BOTH from the live state:
    // since the FOV slider (447 / 0x14003ED20) is a one-shot device write, a
    // stale matrix - left behind by a device-state reset, by an ortho toggle
    // or by any other SetTransform writer - used to survive indefinitely and
    // the slider looked dead until something else rebuilt it.  Deriving the
    // matrix from state.cameraFov every frame makes "drag = next frame shows
    // it" an invariant (defect 5).
    ApplyFrameCameraProjection(app);

    // 0x46BCAA..0x46BD72: MMD keeps camera translation in the world matrix
    // and still installs this fixed +Z look-at view.  Omitting it leaves the
    // D3D identity view active, moving the horizon and clipping the Y axis.
    const bool relativeEye = target != nullptr &&
        app->CameraAttachmentTransformSuppressed() == 0;
    const float eye[3] = {
        relativeEye ? app->CameraPositionX() : app->ViewOffsetX(),
        relativeEye ? app->CameraPositionY() : app->ViewOffsetY(),
        relativeEye ? app->CameraPositionZ() : app->CameraDistance()};
    const float at[3] = {eye[0], eye[1], eye[2] + 80.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    Matrix view;
    api.lookAtLH(&view, eye, at, up);
    device->SetTransform(D3DTS_VIEW,
                         reinterpret_cast<const D3DMATRIX*>(&view));
}

}  // namespace mikumikustudio
