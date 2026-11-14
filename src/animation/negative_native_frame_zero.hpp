#pragma once

#include "negative_key_data.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <variant>

namespace mikumikustudio::negative_frames::native_zero {
// Test seam for a caller-owned, exact frame-zero row. No live pool, lookup,
// pose mutation, or author reconciliation. A future caller must read both
// the row and any effective data under the same serialized ownership guard.
using Values = std::array<double,16>;
using Family = authoring::Family;

struct BoneHeadAuthor {
    std::array<std::uint8_t,16> controls{};
    bone_scale::Curve scaleControls{};
    std::uint8_t physicsDisabled = 0;
};
struct CameraHeadAuthor {
    std::array<std::uint8_t,24> controls{};
};
using HeadAuthor = std::variant<std::monostate,BoneHeadAuthor,CameraHeadAuthor>;

struct Head {
    Family family;
    Values values;
    // First-key controls have no native incoming interval. Preserve the
    // stored bytes even when they are not admissible as signed curve controls.
    HeadAuthor author;
};

inline std::optional<Head> Read(const mdl::CameraKey& row) noexcept {
    if (row.frame != 0) return std::nullopt;
    Head out{Family::Camera,{},CameraHeadAuthor{}};
    for (unsigned i=0;i<3;++i) {out.values[i]=row.eye[i];out.values[i+3]=row.target[i];}
    out.values[6]=row.distance;out.values[7]=float(row.fov);out.values[8]=row.perspective;
    out.values[9]=row.parentModel;out.values[10]=row.parentBone;
    auto& controls=std::get<CameraHeadAuthor>(out.author).controls;
    for (unsigned i=0;i<4;++i) std::copy_n(row.interpolation[i],6,controls.begin()+6*i);
    return out;
}
inline std::optional<Head> Read(const mdl::LightKey& row) noexcept {
    if (row.frame != 0) return std::nullopt;
    Head out{Family::Light,{},{}};
    std::copy_n(row.direction,3,out.values.begin());
    std::copy_n(row.color,3,out.values.begin()+3);
    return out;
}
inline std::optional<Head> Read(const mdl::SelfShadowKey& row) noexcept {
    if (row.frame != 0) return std::nullopt;
    return Head{Family::Shadow,{double(row.mode),row.distance},{}};
}
inline std::optional<Head> Read(const mdl::GravityKey& row) noexcept {
    if (row.frame != 0) return std::nullopt;
    Head out{Family::Gravity,{},{}};
    out.values[0]=row.acceleration;
    std::copy_n(row.direction,3,out.values.begin()+1);
    // The signed pose owns only the enabled byte. Selection and reserved
    // bytes stay in the native row even in a detached target.
    out.values[4]=row.noise;out.values[5]=row.noiseEnabled;
    return out;
}
inline std::optional<Head> Read(const mdl::AccessoryKey& row) noexcept {
    if (row.frame != 0) return std::nullopt;
    Head out{Family::Accessory,{},{}};
    std::copy_n(row.position,3,out.values.begin());
    std::copy_n(row.rotation,3,out.values.begin()+3);
    out.values[6]=row.scale;out.values[7]=row.opacity;out.values[8]=row.visible;
    out.values[9]=row.shadowEnabled;out.values[10]=row.parentModel;out.values[11]=row.parentBone;
    // These are the key's pose parent fields, not the logical attachment.
    return out;
}
inline std::optional<Head> Read(const mdl::BoneKey& row, const bone_scale::Matrix& registeredScale,
    const bone_scale::Curve& scaleControls) noexcept {
    if (row.frame != 0) return std::nullopt;
    Head out{Family::Bone,{},BoneHeadAuthor{}};
    std::copy_n(row.position,3,out.values.begin());
    std::copy_n(row.rotation,4,out.values.begin()+3);
    std::copy(registeredScale.v.begin(),registeredScale.v.end(),out.values.begin()+7);
    auto& author=std::get<BoneHeadAuthor>(out.author);
    std::copy_n(row.interpolation,16,author.controls.begin());
    author.scaleControls=scaleControls;author.physicsDisabled=row.physicsDisabled;
    return out;
}
inline std::optional<Head> Read(const mdl::MorphKey& row) noexcept {
    if (row.frame != 0) return std::nullopt;
    return Head{Family::Morph,{row.value},{}};
}

struct Projection {
    Head head;
    Values effective;
    // Authored context is diagnostic only. A positive-only track does not
    // establish ownership of the negative incoming interval.
    authoring::CurvesPtr frozenAuthor;

    detail::Key LegacyEndpoint() const noexcept {
        detail::Key zero;
        zero.values=effective;
        return zero;
    }
};

// Callers must supply the value actually used for the seam; do not silently
// fall back to the raw row for model selectors, authored overrides or drafts.
// This is a projection, not proof the supplied value matches ordinary seek.
inline std::optional<Projection> Project(const Head& head, const Values& effective,
    const authoring::CurvesPtr& frozenAuthor = {}) noexcept {
    if (frozenAuthor && frozenAuthor->Data().family != head.family) return std::nullopt;
    for (double value : effective)
        if (!std::isfinite(value) || std::abs(value)>(std::numeric_limits<float>::max)()) return std::nullopt;
    return Projection{head,effective,frozenAuthor};
}
}
