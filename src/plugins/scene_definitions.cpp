#include "scene_definitions.hpp"
#include "scene_digest.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <btBulletDynamicsCommon.h>
#include <cmath>

namespace mikumikustudio::plugins {
namespace {
template<class T, size_t N> void Copy(T (&out)[N], const T (&in)[N]) { std::copy(in, in + N, out); }
void Identity(float* out) { std::fill(out, out + 16, 0.f); out[0] = out[5] = out[10] = out[15] = 1; }
void Multiply(float* out, const float* a, const float* b) {
    float result[16]{};
    for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col)
        for (unsigned n = 0; n < 4; ++n) result[row * 4 + col] += a[row * 4 + n] * b[n * 4 + col];
    std::copy(result, result + 16, out);
}
void InverseRigid(float* out, const float* value) {
    Identity(out);
    for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 3; ++col) out[row * 4 + col] = value[col * 4 + row];
    for (unsigned col = 0; col < 3; ++col)
        out[12 + col] = -(value[12] * out[col] + value[13] * out[4 + col] + value[14] * out[8 + col]);
}
void Transform(float* out, const float* position, const float* rotation) {
    float x[16], y[16], z[16]; Identity(x); Identity(y); Identity(z);
    const auto sx = std::sin(rotation[0]), cx = std::cos(rotation[0]);
    const auto sy = std::sin(rotation[1]), cy = std::cos(rotation[1]);
    const auto sz = std::sin(rotation[2]), cz = std::cos(rotation[2]);
    x[5] = x[10] = cx; x[6] = sx; x[9] = -sx;
    y[0] = y[10] = cy; y[2] = -sy; y[8] = sy;
    z[0] = z[5] = cz; z[1] = sz; z[4] = -sz;
    Multiply(out, z, x); Multiply(out, out, y); std::copy(position, position + 3, out + 12);
}
template<class T> void Publish(SceneData& data, SceneData::Channels& record, GeometryDigests& hashes,
    uint32_t channel, std::vector<T> values) {
    Digest digest; digest.Add(values); hashes[channel] = digest.Finish();
    record.values[channel] = data.Keep(std::move(values));
}
MmsResult Physics(SceneIdentity& ids, SceneData& data, SceneData::Channels& record,
    const void* token, const mdl::ModelRecord* model, uint64_t channels, GeometryDigests& hashes) {
    const uint32_t count = model ? model->rigidCount : 0;
    if (count && !model->rigidTable) return MMS_E_NOT_READY;
    std::vector<MmsRigidV1> rigids(count); std::vector<MmsShapeV1> shapes(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& source = model->rigidTable[i]; auto& rigid = rigids[i]; auto& shape = shapes[i];
        if (source.shape > MMS_SHAPE_CAPSULE) return MMS_E_UNSUPPORTED;
        if (!std::isfinite(source.mass) || source.mass < 0 || !std::isfinite(source.size[0]) || source.size[0] < 0 ||
            (source.shape != MMS_SHAPE_SPHERE && (!std::isfinite(source.size[1]) || source.size[1] < 0)) ||
            (source.shape == MMS_SHAPE_BOX && (!std::isfinite(source.size[2]) || source.size[2] < 0))) return MMS_E_FAILED;
        rigid.h = Header<MmsRigidV1>(); rigid.id = rigid.sourceId = ids.Element(token, MMS_CHANNEL_RIGIDS, i);
        shape.h = Header<MmsShapeV1>(); shape.id = shape.sourceId = ids.Element(token, MMS_CHANNEL_SHAPES, i);
        rigid.shapeId = shape.id; shape.kind = source.shape; Copy(shape.size, source.size);
        rigid.mode = source.staticFlag ? 0u : source.kinematicFlag ? 2u : 1u;
        rigid.group = source.group; rigid.collisionMask = source.noCollapse;
        rigid.mass = source.mass; rigid.damping[0] = source.linearDamping; rigid.damping[1] = source.angularDamping;
        rigid.friction = source.friction; rigid.restitution = source.restitution;
        const bool linked = source.boneIndex >= 0 && uint32_t(source.boneIndex) < model->boneCount;
        if ((linked || (model->physicsMode != 2 && model->boneCount)) && !model->boneTable) return MMS_E_NOT_READY;
        if (linked) rigid.boneId = ids.Element(token, MMS_CHANNEL_BONES, uint32_t(source.boneIndex));
        Transform(rigid.world, source.position, source.rotation);
        InverseRigid(rigid.bodyToBone, rigid.world);
        // PMD unlinked bodies use bone zero's rest origin in the native loader.
        // PMX unlinked bodies use the object origin. Neither invents a bone ID.
        const auto* bone = linked ? &model->boneTable[source.boneIndex] :
            model->physicsMode != 2 && model->boneCount ? &model->boneTable[0] : nullptr;
        if (bone) for (unsigned axis = 0; axis < 3; ++axis) rigid.world[12 + axis] += bone->position[axis];
        if (!linked) InverseRigid(rigid.bodyToBone, rigid.world);
        for (float value : rigid.world) if (!std::isfinite(value)) return MMS_E_FAILED;
        std::unique_ptr<btCollisionShape> native;
        if (shape.kind == MMS_SHAPE_SPHERE) native = std::make_unique<btSphereShape>(source.size[0]);
        else if (shape.kind == MMS_SHAPE_BOX) native = std::make_unique<btBoxShape>(btVector3(source.size[0], source.size[1], source.size[2]));
        else native = std::make_unique<btCapsuleShape>(source.size[0], source.size[1]);
        btVector3 inertia(0,0,0); native->calculateLocalInertia(rigid.mode ? rigid.mass : 0.f, inertia);
        for (unsigned axis = 0; axis < 3; ++axis) rigid.inertia[axis * 4] = float(inertia[axis]);
    }
    if (channels & MMS_CHANNEL_JOINTS) {
        const uint32_t countJoints = model ? model->jointCount : 0;
        if (countJoints && !model->jointTable) return MMS_E_NOT_READY;
        std::vector<MmsJointV1> joints(countJoints);
        for (uint32_t i = 0; i < countJoints; ++i) {
            const auto& source = model->jointTable[i]; auto& joint = joints[i];
            if (source.rigidA < 0 || source.rigidB < 0 || uint32_t(source.rigidA) >= count || uint32_t(source.rigidB) >= count) return MMS_E_NOT_READY;
            joint.h = Header<MmsJointV1>(); joint.id = joint.sourceId = ids.Element(token, MMS_CHANNEL_JOINTS, i);
            joint.objectA = joint.objectB = record.object.object.id;
            joint.bodyA = rigids[source.rigidA].id; joint.bodyB = rigids[source.rigidB].id;
            float world[16], inverse[16]; Transform(world, source.position, source.rotation);
            InverseRigid(inverse, rigids[source.rigidA].world); Multiply(joint.frameA, world, inverse);
            InverseRigid(inverse, rigids[source.rigidB].world); Multiply(joint.frameB, world, inverse);
            std::copy(source.limits + 3, source.limits + 6, joint.linearLower); std::copy(source.limits, source.limits + 3, joint.linearUpper);
            std::copy(source.limits + 9, source.limits + 12, joint.angularLower); std::copy(source.limits + 6, source.limits + 9, joint.angularUpper);
            Copy(joint.springs, source.springs);
            // Native CreatePhysJoint uses Bullet 2.75's constructor damping (1)
            // and addConstraint(con, false), allowing collision of linked bodies.
            std::fill(joint.damping, joint.damping + 6, 1.f);
            joint.kind = MMS_JOINT_SPRING6DOF; joint.collideConnected = 1;
        }
        Publish(data, record, hashes, MMS_CHANNEL_JOINTS, std::move(joints));
    }
    if (channels & MMS_CHANNEL_RIGIDS) Publish(data, record, hashes, MMS_CHANNEL_RIGIDS, std::move(rigids));
    if (channels & MMS_CHANNEL_SHAPES) Publish(data, record, hashes, MMS_CHANNEL_SHAPES, std::move(shapes));
    return MMS_OK;
}
template<class T> bool Available(int32_t count, const T* pointer) { return count >= 0 && (!count || pointer); }
template<class T> void Elements(SceneData& data, MmsMorphV1& morph, Digest& digest, std::vector<T> values) {
    digest.Add(values); morph.elements = data.Keep(std::move(values));
}
MmsResult Morphs(SceneIdentity& ids, SceneData& data, SceneData::Channels& record,
    const void* token, const mdl::ModelRecord* model, GeometryDigests& hashes) {
    const uint32_t count = model ? model->morphCount : 0;
    if (count && !model->morphs) return MMS_E_NOT_READY;
    std::vector<MmsMorphV1> morphs(count); Digest digest; digest.Add(&count, sizeof(count));
    for (uint32_t i = 0; i < count; ++i) {
        const auto& source = model->morphs[i]; auto& morph = morphs[i]; const bool pmx = model->physicsMode == 2;
        morph.h = Header<MmsMorphV1>(); morph.id = morph.sourceId = ids.Element(token, MMS_CHANNEL_MORPHS, i);
        morph.name = data.Keep(Utf8(text_encoding::MorphName(source, false))); morph.kind = pmx ? source.type : MMS_MORPH_VERTEX;
        morph.flags = uint32_t(source.panel) << MMS_MORPH_PANEL_SHIFT;
        if (!pmx && !i) morph.flags |= MMS_MORPH_BASE;
        digest.Add(&morph.id, sizeof(morph.id)); digest.Add(&morph.kind, sizeof(morph.kind)); digest.Add(&morph.flags, sizeof(morph.flags));
        digest.Add(&morph.name.bytes, sizeof(morph.name.bytes)); digest.Add(morph.name.data, size_t(morph.name.bytes));
        if (morph.kind == MMS_MORPH_GROUP) {
            if (!Available(source.groupCount, source.groupEntries)) return MMS_E_FAILED;
            std::vector<MmsMorphGroupElementV1> elements(size_t(source.groupCount));
            for (size_t n = 0; n < elements.size(); ++n) {
                const auto& e = source.groupEntries[n]; if (e.morphIndex < 0 || uint32_t(e.morphIndex) >= count) return MMS_E_FAILED;
                elements[n].morph = ids.Element(token, MMS_CHANNEL_MORPHS, uint32_t(e.morphIndex)); elements[n].weight = e.weight;
            }
            Elements(data, morph, digest, std::move(elements));
        } else if (morph.kind == MMS_MORPH_VERTEX) {
            if (!Available(source.offsetCount, source.vertexEntries)) return MMS_E_FAILED;
            std::vector<MmsMorphVertexElementV1> elements(size_t(source.offsetCount));
            for (size_t n = 0; n < elements.size(); ++n) {
                const auto& e = source.vertexEntries[n]; if (e.vertexIndex >= model->vertexCount) return MMS_E_FAILED;
                elements[n].vertex = ids.Element(token, MMS_CHANNEL_MESH, e.vertexIndex); Copy(elements[n].offset, e.offset);
            }
            Elements(data, morph, digest, std::move(elements));
        } else if (morph.kind == MMS_MORPH_BONE) {
            if (!Available(source.boneCount, source.boneEntries)) return MMS_E_FAILED;
            std::vector<MmsMorphBoneElementV1> elements(size_t(source.boneCount));
            for (size_t n = 0; n < elements.size(); ++n) {
                const auto& e = source.boneEntries[n]; if (e.boneIndex < 0 || uint32_t(e.boneIndex) >= model->boneCount) return MMS_E_FAILED;
                elements[n].bone = ids.Element(token, MMS_CHANNEL_BONES, uint32_t(e.boneIndex));
                Copy(elements[n].translation, e.translation); Copy(elements[n].rotation, e.rotation);
            }
            Elements(data, morph, digest, std::move(elements));
        } else if (morph.kind >= MMS_MORPH_UV && morph.kind <= MMS_MORPH_UV4) {
            const auto family = morph.kind - MMS_MORPH_UV;
            if (!Available(source.uvCounts[family], source.uvEntries[family])) return MMS_E_FAILED;
            std::vector<MmsMorphUvElementV1> elements(size_t(source.uvCounts[family]));
            for (size_t n = 0; n < elements.size(); ++n) {
                const auto& e = source.uvEntries[family][n]; if (e.vertexIndex < 0 || uint32_t(e.vertexIndex) >= model->vertexCount) return MMS_E_FAILED;
                elements[n].vertex = ids.Element(token, MMS_CHANNEL_MESH, uint32_t(e.vertexIndex)); Copy(elements[n].offset, e.offset);
            }
            Elements(data, morph, digest, std::move(elements));
        } else if (morph.kind == MMS_MORPH_MATERIAL) {
            if (!Available(source.materialCount, source.materialEntries)) return MMS_E_FAILED;
            std::vector<MmsMorphMaterialElementV1> elements(size_t(source.materialCount));
            for (size_t n = 0; n < elements.size(); ++n) {
                const auto& e = source.materialEntries[n]; auto& v = elements[n];
                if (e.materialIndex < -1 || (e.materialIndex >= 0 && uint32_t(e.materialIndex) >= model->materialCount) || e.operation > MMS_MORPH_ADD) return MMS_E_FAILED;
                if (e.materialIndex >= 0) v.material = ids.Element(token, MMS_CHANNEL_MATERIALS, uint32_t(e.materialIndex));
                v.operation = e.operation; const auto& c = e.channels;
                Copy(v.diffuse, c.diffuse); Copy(v.specular, c.specular); v.power = c.specularPower;
                Copy(v.ambient, c.ambient); Copy(v.edgeColor, c.edgeColor); v.edgeSize = c.edgeSize;
                Copy(v.texture, c.textureTint); Copy(v.sphere, c.sphereTint); Copy(v.toon, c.toonTint);
            }
            Elements(data, morph, digest, std::move(elements));
        } else return MMS_E_UNSUPPORTED;
    }
    hashes[MMS_CHANNEL_MORPHS] = digest.Finish(); record.values[MMS_CHANNEL_MORPHS] = data.Keep(std::move(morphs)); return MMS_OK;
}
}
MmsResult CaptureBindDefinitions(SceneIdentity& ids, SceneData& data, SceneData::Channels& record,
    const void* token, uint32_t kind, uint64_t channels, GeometryDigests& hashes) {
    const auto* model = kind == MMS_OBJECT_MODEL ? mdl::Mdl(static_cast<const unsigned char*>(token)) : nullptr;
    if (channels & (MMS_CHANNEL_RIGIDS | MMS_CHANNEL_JOINTS | MMS_CHANNEL_SHAPES)) {
        const auto result = Physics(ids, data, record, token, model, channels, hashes); if (result != MMS_OK) return result;
    }
    if (channels & MMS_CHANNEL_MORPHS) return Morphs(ids, data, record, token, model, hashes);
    return MMS_OK;
}
}
