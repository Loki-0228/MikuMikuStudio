#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "BulletDynamics/Dynamics/btDynamicsWorld.h"
#include <type_traits>
#include <utility>
#include <vector>
#include <limits>
#include <stdexcept>
#include <new>

// Project-local Bullet 2.75 adapter. Explicit instantiation names actual typed
// members without changing Bullet's declarations, layout or linked library.
// Re-audit this file and the import allocation tests when upgrading Bullet.
static_assert(BT_BULLET_VERSION == 275, "Re-audit the Bullet import storage adapter");
namespace mikudancestudio::history_physics::import_storage {
namespace {
template<class Tag, typename Tag::Member member> struct MemberAccess {
    friend typename Tag::Member Access(Tag) noexcept { return member; }
};
#define MMS_IMPORT_ARRAY_MEMBERS(Name, Element) \
using Name = btAlignedObjectArray<Element>; \
struct Name##Size { using Member = int Name::*; friend Member Access(Name##Size) noexcept; }; \
struct Name##Capacity { using Member = int Name::*; friend Member Access(Name##Capacity) noexcept; }; \
struct Name##Data { using Member = Element* Name::*; friend Member Access(Name##Data) noexcept; }; \
struct Name##Owns { using Member = bool Name::*; friend Member Access(Name##Owns) noexcept; }; \
template struct MemberAccess<Name##Size, &Name::m_size>; \
template struct MemberAccess<Name##Capacity, &Name::m_capacity>; \
template struct MemberAccess<Name##Data, &Name::m_data>; \
template struct MemberAccess<Name##Owns, &Name::m_ownsMemory>; \
inline void Exchange(Name& a, Name& b) noexcept { \
    static_assert(std::is_empty<btAlignedAllocator<Element, 16>>::value, "Allocator must be stateless"); \
    using std::swap; swap(a.*Access(Name##Size{}), b.*Access(Name##Size{})); \
    swap(a.*Access(Name##Capacity{}), b.*Access(Name##Capacity{})); \
    swap(a.*Access(Name##Data{}), b.*Access(Name##Data{})); \
    swap(a.*Access(Name##Owns{}), b.*Access(Name##Owns{})); \
} \
inline void Prepare(Name& out, const std::vector<Element>& values) { \
    if (out.capacity() || values.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) || \
        values.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(Element)) \
        throw std::invalid_argument("invalid import array"); \
    if (values.empty()) return; \
    auto* data = static_cast<Element*>(btAlignedAlloc(values.size() * sizeof(Element), 16)); \
    if (!data) throw std::bad_alloc(); \
    for (std::size_t i = 0; i < values.size(); ++i) data[i] = values[i]; \
    out.*Access(Name##Data{}) = data; out.*Access(Name##Size{}) = static_cast<int>(values.size()); \
    out.*Access(Name##Capacity{}) = static_cast<int>(values.size()); out.*Access(Name##Owns{}) = true; \
}
MMS_IMPORT_ARRAY_MEMBERS(Objects, btCollisionObject*)
MMS_IMPORT_ARRAY_MEMBERS(Bodies, btRigidBody*)
MMS_IMPORT_ARRAY_MEMBERS(Constraints, btTypedConstraint*)
#undef MMS_IMPORT_ARRAY_MEMBERS
struct Tick { using Member = btInternalTickCallback btDynamicsWorld::*; friend Member Access(Tick) noexcept; };
struct PreTick { using Member = btInternalTickCallback btDynamicsWorld::*; friend Member Access(PreTick) noexcept; };
template struct MemberAccess<Tick, &btDynamicsWorld::m_internalTickCallback>;
template struct MemberAccess<PreTick, &btDynamicsWorld::m_internalPreTickCallback>;
inline bool HasTickCallbacks(const btDynamicsWorld& world) noexcept {
    return world.*Access(Tick{}) || world.*Access(PreTick{});
}
}
}
#endif
