#pragma once
#include "mikumikustudio/build_profile.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mikumikustudio {
class PhysicsScene;
namespace mdl { struct ModelRecord; struct JointRecord; struct EdgeVertex; }
namespace fileio { class SequentialFileReader; }
namespace plugins::resource_budget { class Collector; }
namespace gpu { struct Vertex; }
namespace pmx21 {
#if !MMD_ORIGINAL_PROFILE
bool ReadSoftBodies(unsigned char* model, fileio::SequentialFileReader& reader,
                    std::int64_t fileLength, bool utf8);
int CreateJoint(PhysicsScene& scene, mdl::ModelRecord& model,
                mdl::JointRecord& joint, unsigned type);
void Forget(unsigned char* model) noexcept;
void SuspendSoftBodies(unsigned char* model) noexcept;
void SetPresent(unsigned char* model, bool present) noexcept;
void CopyState(const unsigned char* source, unsigned char* target);
void ExchangeRenderState(unsigned char* live, unsigned char* sampled) noexcept;
void MeasureResources(const unsigned char* model, plugins::resource_budget::Collector& out);
bool HasSoftBodies(const unsigned char* model);
std::size_t SoftBodyCount(const unsigned char* model);
void SyncSoftBodies(unsigned char* model, bool reset);
void RefreshVertexMorphs(unsigned char* model);
void ApplyImpulses(unsigned char* model);
void WriteGpuSoftVertices(unsigned char* model, std::vector<gpu::Vertex>& vertices);
void WriteSoftVertices(unsigned char* model, void* vertices, std::size_t stride,
                       mdl::EdgeVertex* edges, float edgeDistance);
#else
inline void Forget(unsigned char*) noexcept {}
inline void SuspendSoftBodies(unsigned char*) noexcept {}
inline void SetPresent(unsigned char*, bool) noexcept {}
inline void CopyState(const unsigned char*, unsigned char*) {}
inline void ExchangeRenderState(unsigned char*, unsigned char*) noexcept {}
inline bool HasSoftBodies(const unsigned char*) { return false; }
inline std::size_t SoftBodyCount(const unsigned char*) { return 0; }
inline void SyncSoftBodies(unsigned char*, bool) {}
inline void ApplyImpulses(unsigned char*) {}
inline void WriteGpuSoftVertices(unsigned char*, std::vector<gpu::Vertex>&) {}
inline void WriteSoftVertices(unsigned char*, void*, std::size_t, mdl::EdgeVertex*, float) {}
#endif
} // namespace pmx21
} // namespace mikumikustudio
