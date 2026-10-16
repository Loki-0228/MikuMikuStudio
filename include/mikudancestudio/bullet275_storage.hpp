#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include <cstddef>

// This adapter is specific to the headers and static libraries of Bullet 2.75.
// An upgrade must re-audit both member names/types and the allocation tests.
// It changes no Bullet declaration, object layout, allocation or library code.
static_assert(BT_BULLET_VERSION == 275, "Re-audit the Bullet constraint storage adapter before upgrading Bullet");

namespace mikudancestudio::bullet275_storage {
struct Allocation {
    const void* identity = nullptr;
    std::size_t capacity = 0;
};
namespace detail {
namespace {
using References = btAlignedObjectArray<btTypedConstraint*>;
struct BodyReferences {
    using Member = References btRigidBody::*;
    friend Member ReadMember(BodyReferences) noexcept;
};
struct ReferenceData {
    using Member = btTypedConstraint** References::*;
    friend Member ReadMember(ReferenceData) noexcept;
};
// Explicit instantiation does not apply access checking to its template
// arguments ([temp.explicit]). The resulting pointers name real typed members;
// no guessed offsets, class redefinitions, casts or mutable references are used.
// Internal linkage gives each including TU its own instantiations and avoids
// multiple explicit-instantiation definitions for one program entity.
template<class Tag, typename Tag::Member member> struct ReadOnlyMember {
    friend typename Tag::Member ReadMember(Tag) noexcept { return member; }
};
template struct ReadOnlyMember<BodyReferences, &btRigidBody::m_constraintRefs>;
template struct ReadOnlyMember<ReferenceData, &References::m_data>;
}
}
// Reads storage, not elements: removal can leave capacity > 0 with size == 0.
// A const body is sufficient, and zero capacity never reads the data member.
// The caller must keep the body alive and prevent concurrent physics edits.
// False leaves out unchanged; unknown derived types need a separate size guard.
[[maybe_unused]] static bool ConstraintReferences(const btRigidBody& body, Allocation& out) noexcept {
    const auto& references = body.*ReadMember(detail::BodyReferences{});
    const int capacity = references.capacity(), size = references.size();
    if (capacity < 0 || size < 0 || size > capacity) return false;
    if (!capacity) { out = {}; return true; }
    const void* const data = references.*ReadMember(detail::ReferenceData{});
    if (!data) return false;
    out = {data, static_cast<std::size_t>(capacity)};
    return true;
}
}
#endif
