#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <algorithm>
#include <cstdint>
#include <map>
#include <new>
#include <string>
#include <vector>

struct IUnknown;
class btRigidBody;
class btTypedConstraint;
class btCollisionShape;
namespace mikumikustudio {
namespace mdl { struct ModelRecord; struct AccessoryRecord; struct AccessoryKey; }
namespace plugins::resource_budget {
// This is a retained-payload estimate, not a process/VRAM measurement. Global
// renderer caches, allocator metadata and opaque driver storage are outside it.
// Missing flags must be checked before treating TotalBytes as a complete charge.
enum Missing : std::uint32_t {
    RawTextCapacity = 1u << 0, NativeOpaque = 1u << 1,
    UndoCapacity = 1u << 2, PhysicsStorage = 1u << 3,
    RenderQuery = 1u << 4, MeshSidecar = 1u << 5,
    CurveCallback = 1u << 6, ExternalRenderer = 1u << 7,
};
struct Report {
    std::uint64_t cpuBytes = 0, d3d9Bytes = 0, totalBytes = 0;
    std::uint32_t missing = 0;
    bool Complete() const noexcept { return missing == 0; }
};
// One collector spans the entire batch. Equal allocation/COM identities are
// charged once at their largest observed size; equal content is not deduped.
// Checked operations throw bad_alloc, translated to MMS_E_MEMORY by the caller.
class Collector {
    struct Key {
        const void* pointer;
        unsigned domain;
        bool operator<(const Key& other) const noexcept {
            if (domain != other.domain) return domain < other.domain;
            return std::less<const void*>{}(pointer, other.pointer);
        }
    };
    std::map<Key, std::uint64_t> allocations_;
    Report report_;
    bool verifying_ = false, mismatch_ = false;
    struct Changed {};
public:
    static std::uint64_t Sum(std::uint64_t a, std::uint64_t b) {
        if (a > UINT64_MAX - b) throw std::bad_alloc();
        return a + b;
    }
    static std::uint64_t Product(std::uint64_t count, std::uint64_t stride) {
        if (stride && count > UINT64_MAX / stride) throw std::bad_alloc();
        return count * stride;
    }
    void MissingResource(Missing reason) noexcept {
        if (verifying_) mismatch_ = true;
        else report_.missing |= reason;
    }
    const Report& Result() const noexcept { return report_; }
    // Storage owned by this temporary ledger, excluding sizeof(Collector).
    // Coordinators charge it in addition to the resources it describes.
    // The tree nodes and sentinel hold no separately allocated payload.
    std::uint64_t StorageBytes() const {
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L && !_HAS_CXX23
        using Node=std::_Tree_node<decltype(allocations_)::value_type,void*>;
        auto bytes=Product(Sum(allocations_.size(),1),sizeof(Node));
#if _ITERATOR_DEBUG_LEVEL != 0
        bytes=Sum(bytes,sizeof(std::_Container_proxy));
#endif
        return bytes;
#else
        throw std::bad_alloc();
#endif
    }
    // Reuse the prepared identity/capacity ledger without inserting nodes.
    // Removed or smaller allocations remain conservatively charged. New or
    // larger allocations and incomplete queries invalidate the candidate.
    // This is validation before publication, never part of resource mutation.
    template<class Measure> bool Covers(Measure&& measure) noexcept {
        if (verifying_ || report_.missing) return false;
        verifying_ = true; mismatch_ = false;
        try { measure(*this); } catch (...) { mismatch_ = true; }
        verifying_ = false; return !mismatch_;
    }
    void Add(const void* identity, std::uint64_t count, std::uint64_t stride, bool d3d9 = false) {
        if (!identity) return;
        const auto bytes = Product(count, stride);
        if (verifying_) {
            const auto found = allocations_.find(Key{identity, d3d9 ? 1u : 0u});
            if (found == allocations_.end() || found->second < bytes) throw Changed{};
            return;
        }
        auto found = allocations_.try_emplace(Key{identity, d3d9 ? 1u : 0u}, 0).first;
        if (found->second >= bytes) return;
        const auto delta = bytes - found->second;
        const auto total = Sum(report_.totalBytes, delta);
        auto& category = d3d9 ? report_.d3d9Bytes : report_.cpuBytes;
        category = Sum(category, delta); report_.totalBytes = total; found->second = bytes;
    }
    template<class T> void Array(const T* p, std::uint64_t n) { Add(p, n, sizeof(T)); }
    template<class T> void Vector(const std::vector<T>& v) { Array(v.data(), v.capacity()); }
    template<class C, class T, class A> void String(const std::basic_string<C,T,A>& s) {
        // Charging SSO storage twice is a small conservative allowance.
        Add(s.data(), Sum(s.capacity(), 1), sizeof(C));
    }
    template<class Map> void Nodes(const Map& map) {
        // Payload plus a conservative tree/list node allowance. Shared hash
        // bucket arrays belong to the global registry, not to one object.
        for (const auto& item : map) Add(&item, 1, sizeof(item) + 4 * sizeof(void*));
    }
    void RawText(const wchar_t*);
    void VertexBuffer(void*);
    void IndexBuffer(void*);
    void Mesh(void*);
};
void Model(const mdl::ModelRecord&, Collector&);
void Accessory(const mdl::AccessoryRecord&, const mdl::AccessoryKey*, Collector&);
void Shape(const btCollisionShape*, Collector&);
void Body(const btRigidBody*, Collector&);
void Joint(const btTypedConstraint*, Collector&);
}
// Read-only hooks live beside the allocations they describe. None refreshes
// authoring state, seeks, evaluates physics, or opens a graphics device.
namespace native_history { void MeasureResources(const mdl::ModelRecord&, plugins::resource_budget::Collector&); }
namespace sequence_scatter { void MeasureSnapshot(const void*, plugins::resource_budget::Collector&); }
namespace bone_scale { void MeasureResources(const unsigned char*, plugins::resource_budget::Collector&); }
namespace bone_curve { void MeasureResources(const unsigned char*, plugins::resource_budget::Collector&); }
namespace morph_curve { void MeasureResources(const unsigned char*, plugins::resource_budget::Collector&); }
namespace model_vertex_cache { void MeasureResources(const unsigned char*, plugins::resource_budget::Collector&); }
namespace text_encoding { void MeasureResources(const mdl::ModelRecord&, plugins::resource_budget::Collector&); }
namespace native_mesh { void MeasureResources(const void*, plugins::resource_budget::Collector&); }
namespace mesh_bake_cache { void MeasureResources(const void*, plugins::resource_budget::Collector&); }
namespace physics_scale { void MeasureResources(const unsigned char*, plugins::resource_budget::Collector&); }
namespace skinning { void MeasureResources(const unsigned char*, plugins::resource_budget::Collector&); }
namespace mmv4mms { void MeasureResources(const void*, plugins::resource_budget::Collector&); }
namespace mdl { void MeasureBoneKeyStorage(const ModelRecord&, plugins::resource_budget::Collector&); }
}
#endif
