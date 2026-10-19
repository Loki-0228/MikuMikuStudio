#include "mikumikustudio/perf_timing.hpp"
// ===========================================================================
// VA 0x004970B0 - ModelApplyMorphs  (original: sub_4970B0, 0x288D bytes)
// ===========================================================================
// Morph application pass, __thiscall on the model block.  Called per model
// (slot order) right before SetPhysicsMode (0x4A9220) in the frame-driver
// physics section (0x46FCB1 / 0x46FE93) and from several UI refresh paths
// (0x430F20, 0x4312E0, 0x432FA0, 0x4341E0, 0x446A70, 0x44AAA0).  Physics
// models only: model+14590 == 2, otherwise the function returns without
// touching anything.
//
// NOTE: earlier port comments called this the "IK solver" - it is not.  The
// CCD IK solve lives in BoneFrameTransform (0x493A60) phase D and is driven
// by SetPhysicsMode; this function only accumulates morph offsets.
//
//   1. Bone morphs (0x4970C3..0x49767A): the 32-byte work records at
//      model+8728 (count model+8708) { int boneIdx; vec3 pos@+4; quat@+16 }
//      are reset to pos 0 / quat identity, then every morph record (136 B,
//      model+9924, count model+11648) with weight (morph+48) != 0 adds in:
//        - type 2 (bone morph): entries at morph+96, count morph+76, 32 B
//          each { int boneIdx; vec3 translation; quat rotation }.
//        - type 0 (group morph): 8-byte refs at morph+100, count morph+80
//          { int morphIdx; float weight }; refs pointing at type-2 morphs
//          contribute their entries scaled by group weight * ref weight.
//      The entry quat is rescaled by the weight through axis-angle: w is
//      clamped to [-1,1], angle = acos(w) (w<0 subtracts the
//      3.140000104904175 literal - same quirk as the 0x4B25D0 rotation
//      limit), zero angle or sub-epsilon axis -> identity; else angle *=
//      weight and work.quat = work.quat * q (D3DXQuaternionMultiply),
//      work.pos += entry translation * weight.  SetPhysicsMode later adds
//      these records onto bone->physicsOffset/+376 before the transform/IK update.
//   2. Material morphs (0x497696..0x4996BF): two 128-byte-per-material
//      pools are reset - model+8760 additive (memset 0) and model+8764
//      multiplicative (the 28 blend channels at offsets 8..124 skipping
//      36/56 set to 1.0).  Type 8 morphs (and type-0 groups referring to
//      them, weight * ref weight) contribute 128-byte entries at morph+124,
//      count morph+84 { int matIdx (-1 = all materials); byte calcMode@+4
//      (1 = additive, 0 = multiplicative); channel deltas at the same
//      offsets as the pools }:
//        additive:       add[ch] += delta[ch] * w
//        multiplicative: mul[ch]  = (1 - (1 - delta[ch]) * w) * mul[ch]
//   3. Compose (0x4996C9..0x4999D0): for every material (count model+28,
//      2292-byte records at model+32):  mat[dst] = mul[ch]*base[ch] + add[ch]
//      with base = the load-time pool at model+8756.  Only 16 of the 28
//      channels are written back; the other 12 accumulate in the pools but
//      are never consumed - faithful to the original.
//
// Deviation: the original unrolls every channel access per code path; the
// port loops over the same offset sets (arithmetic identical, order of
// independent channel updates irrelevant).
// =========================================================================//
#include <cmath>
#include <cstdint>
#include <cstring>

#include "mikumikustudio/model.hpp"

namespace mikumikustudio {
namespace {

// D3DXQuaternionMultiply(out, a, b): out = a * b (Hamilton product).
void QuatMul(float out[4], const float a[4], const float b[4]) {
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    out[3] = r[3];
}

// One bone-morph entry (0x497452 / 0x4971B7 pattern): rescale the entry
// quaternion by `w` through axis-angle and accumulate into the 32-byte work
// record selected by the entry's bone index.
void AccumBoneEntry(mdl::BoneMorphOffsetRecord* work,
                    int workCount, const mdl::PmxBoneMorphEntry& ent, float w) {
    if (!work || ent.boneIndex < 0 || ent.boneIndex >= workCount) return;
    mdl::BoneMorphOffsetRecord& rec = work[ent.boneIndex];
    float x = ent.rotation[0];
    float y = ent.rotation[1];
    float z = ent.rotation[2];
    float qw = ent.rotation[3];
    if (qw > 1.0f)
        qw = 1.0f;
    else if (qw < -1.0f)
        qw = -1.0f;
    double angle = std::acos(static_cast<double>(qw));
    if (qw < 0.0f)
        angle -= 3.140000104904175;
    const double len =
        std::sqrt(static_cast<double>(x * x + y * y + z * z));
    float q[4];
    if (angle == 0.0 || len < 0.00000011920929) {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
    } else {
        const double scaled = angle * w;
        const double s = std::sin(scaled) / len;
        q[0] = static_cast<float>(s * x);
        q[1] = static_cast<float>(s * y);
        q[2] = static_cast<float>(s * z);
        q[3] = static_cast<float>(std::cos(scaled));
    }
    QuatMul(rec.rotation, rec.rotation, q);
    rec.translation[0] += ent.translation[0] * w;
    rec.translation[1] += ent.translation[1] * w;
    rec.translation[2] += ent.translation[2] * w;
}

template <std::size_t Count>
void ApplyMaterialMorphChannels(float (&add)[Count], float (&mul)[Count],
                                const float (&delta)[Count], float weight,
                                bool additive) {
    for (std::size_t i = 0; i < Count; ++i) {
        if (additive)
            add[i] += delta[i] * weight;
        else
            mul[i] = (1.0f - (1.0f - delta[i]) * weight) * mul[i];
    }
}

void ApplyMaterialMorphChannels(float& add, float& mul, float delta,
                                float weight, bool additive) {
    if (additive)
        add += delta * weight;
    else
        mul = (1.0f - (1.0f - delta) * weight) * mul;
}

// One material-morph entry applied to one named pool pair (add + multiply).
void ApplyMatSlot(mdl::MaterialMorphPool& add, mdl::MaterialMorphPool& mul,
                  const mdl::PmxMaterialMorphEntry& entry, float weight) {
    const bool additive = entry.operation != 0;
    const mdl::MaterialMorphChannels& delta = entry.channels;
    mdl::MaterialMorphChannels& addChannels = add.channels;
    mdl::MaterialMorphChannels& mulChannels = mul.channels;
    ApplyMaterialMorphChannels(addChannels.diffuse, mulChannels.diffuse,
                               delta.diffuse, weight, additive);
    ApplyMaterialMorphChannels(addChannels.specular, mulChannels.specular,
                               delta.specular, weight, additive);
    ApplyMaterialMorphChannels(addChannels.specularPower,
                               mulChannels.specularPower,
                               delta.specularPower, weight, additive);
    ApplyMaterialMorphChannels(addChannels.ambient, mulChannels.ambient,
                               delta.ambient, weight, additive);
    ApplyMaterialMorphChannels(addChannels.edgeColor, mulChannels.edgeColor,
                               delta.edgeColor, weight, additive);
    ApplyMaterialMorphChannels(addChannels.edgeSize, mulChannels.edgeSize,
                               delta.edgeSize, weight, additive);
    ApplyMaterialMorphChannels(addChannels.textureTint,
                               mulChannels.textureTint, delta.textureTint,
                               weight, additive);
    ApplyMaterialMorphChannels(addChannels.sphereTint, mulChannels.sphereTint,
                               delta.sphereTint, weight, additive);
    ApplyMaterialMorphChannels(addChannels.toonTint, mulChannels.toonTint,
                               delta.toonTint, weight, additive);
}

// All entries of one material morph (type 8 record), weight `w`.
void AccumMatMorph(mdl::MaterialMorphPool* addP,
                   mdl::MaterialMorphPool* mulP, int matCount,
                   const mdl::MorphRecord& morph, float w) {
    const int n = morph.materialCount;
    for (int k = 0; k < n; ++k) {
        const mdl::PmxMaterialMorphEntry& ent = morph.materialEntries[k];
        const std::int32_t mi = ent.materialIndex;
        if (mi == -1) {
            for (int s = 0; s < matCount; ++s)
                ApplyMatSlot(addP[s], mulP[s], ent, w);
        } else if (mi >= 0 && mi < matCount) {
            ApplyMatSlot(addP[mi], mulP[mi], ent, w);
        }
    }
}

}  // namespace

// Entries retain PMX bone indices. The accumulator must therefore be indexed
// by bone, not by the number/order of morph entries (which can be sparse and
// can reference the same bone several times). Apply each bone's total once.
void InitializeBoneMorphOffsets(unsigned char* m) {
    auto& model = *mdl::Mdl(m);
    auto*& work = mdl::BoneMorphOffsets(m);
    operator delete(work);
    work = nullptr;
    mdl::BoneMorphOffsetCount(m) = 0;
    bool hasEntries = false;
    for (std::uint32_t i = 0; i < model.morphCount; ++i)
        hasEntries |= model.morphs[i].type == 2 && model.morphs[i].boneCount > 0;
    if (!hasEntries || model.boneCount == 0) return;
    work = static_cast<mdl::BoneMorphOffsetRecord*>(
        operator new(sizeof(*work) * model.boneCount));
    mdl::BoneMorphOffsetCount(m) = static_cast<int>(model.boneCount);
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        work[i] = {};
        work[i].boneIndex = static_cast<int>(i);
        work[i].rotation[3] = 1.f;
    }
}

void ModelApplyMorphs(unsigned char* m) {
    MMD_PERF_SCOPE(BoneMorph);
    mdl::ModelRecord& model = *mdl::Mdl(m);
    if (model.physicsMode != 2)
        return;

    mdl::MorphRecord* morphs = model.morphs;
    const int morphCount = static_cast<int>(model.morphCount);
    mdl::BoneMorphOffsetRecord* work = mdl::BoneMorphOffsets(m);
    const int workCount = mdl::BoneMorphOffsetCount(m);

    // ---- 1. bone morph offsets into the work records ----------------------
    for (int i = 0; i < workCount; ++i) {
        mdl::BoneMorphOffsetRecord& record = work[i];
        record.translation[0] = record.translation[1] =
            record.translation[2] = 0.0f;
        record.rotation[0] = record.rotation[1] =
            record.rotation[2] = 0.0f;
        record.rotation[3] = 1.0f;
    }
    for (int i = 0; i < morphCount; ++i) {
        const mdl::MorphRecord& morph = morphs[i];
        const float w = morph.value;
        if (w == 0.0f)
            continue;
        if (morph.type != 0) {
            if (morph.type == 2) {                    // bone morph
                const int n = morph.boneCount;
                for (int k = 0; k < n; ++k)
                    AccumBoneEntry(work, workCount, morph.boneEntries[k], w);
            }
        } else {                                      // group morph
            for (int k = 0; k < morph.groupCount; ++k) {
                const mdl::PmxGroupMorphEntry& ref = morph.groupEntries[k];
                if (ref.morphIndex < 0 || ref.morphIndex >= morphCount) continue;
                const mdl::MorphRecord& target = morphs[ref.morphIndex];
                if (target.type == 2) {
                    const int en = target.boneCount;
                    const float w2 = w * ref.weight;
                    for (int e = 0; e < en; ++e)
                        AccumBoneEntry(work, workCount, target.boneEntries[e], w2);
                }
            }
        }
    }

    // ---- 2. material morph pools ------------------------------------------
    const int matCount = static_cast<int>(model.materialCount);
    auto* addP = mdl::MaterialMorphAdd(m);
    auto* mulP = mdl::MaterialMorphMul(m);
    if (matCount > 0)
        std::memset(addP, 0, sizeof(*addP) * static_cast<std::size_t>(matCount));
    for (int i = 0; i < matCount; ++i) {
        mdl::MaterialMorphChannels& slot = mulP[i].channels;
        for (float& value : slot.diffuse) value = 1.0f;
        for (float& value : slot.specular) value = 1.0f;
        slot.specularPower = 1.0f;
        for (float& value : slot.ambient) value = 1.0f;
        for (float& value : slot.edgeColor) value = 1.0f;
        slot.edgeSize = 1.0f;
        for (float& value : slot.textureTint) value = 1.0f;
        for (float& value : slot.sphereTint) value = 1.0f;
        for (float& value : slot.toonTint) value = 1.0f;
    }
    for (int i = 0; i < morphCount; ++i) {
        const mdl::MorphRecord& morph = morphs[i];
        const float w = morph.value;
        if (w == 0.0f)
            continue;
        if (morph.type != 0) {
            if (morph.type == 8)                       // material morph
                AccumMatMorph(addP, mulP, matCount, morph, w);
        } else {                                      // group morph
            for (int k = 0; k < morph.groupCount; ++k) {
                const mdl::PmxGroupMorphEntry& ref = morph.groupEntries[k];
                if (ref.morphIndex < 0 || ref.morphIndex >= morphCount) continue;
                const mdl::MorphRecord& target = morphs[ref.morphIndex];
                if (target.type == 8)
                    AccumMatMorph(addP, mulP, matCount, target,
                                  w * ref.weight);
            }
        }
    }

    // ---- 3. compose into the material records ------------------------------
    auto* mats = static_cast<mdl::ModelMaterialRecord*>(model.materials);
    auto* baseP = mdl::MaterialMorphBase(m);
    for (int i = 0; i < matCount; ++i) {
        mdl::ModelMaterialRecord& mat = mats[i];
        const mdl::MaterialMorphChannels& add = addP[i].channels;
        const mdl::MaterialMorphChannels& mul = mulP[i].channels;
        const mdl::MaterialMorphChannels& base = baseP[i].channels;
        const auto compose = [](float multiplier, float original, float added) {
            return multiplier * original + added;
        };
        for (int c = 0; c < 3; ++c) {
            mat.diffuseMirror[c] = compose(mul.diffuse[c], base.diffuse[c],
                                            add.diffuse[c]);
            mat.specular[c] = compose(mul.specular[c], base.specular[c],
                                      add.specular[c]);
            mat.ambient[c] = compose(mul.ambient[c], base.ambient[c],
                                     add.ambient[c]);
            mat.edgeColor[c] = compose(mul.edgeColor[c], base.edgeColor[c],
                                       add.edgeColor[c]);
        }
        mat.diffuse[3] = compose(mul.diffuse[3], base.diffuse[3],
                                 add.diffuse[3]);
        mat.specularPower = compose(mul.specularPower, base.specularPower,
                                    add.specularPower);
        mat.edgeColor[3] = compose(mul.edgeColor[3], base.edgeColor[3],
                                   add.edgeColor[3]);
        mat.edgeSize = compose(mul.edgeSize, base.edgeSize, add.edgeSize);
    }
}

}  // namespace mikumikustudio
