#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace mikumikustudio::gpu {
// Float4-aligned structured records; shared with d3d12_skinning.hlsl.
struct Vertex {
    float position[3], edgeScale;
    float normal[3]; std::uint32_t type; // 0 BDEF1, 1 BDEF2/PMD, 2 BDEF4, 3 SDEF
    float uv[2]; std::uint32_t edgeEnabled, material;
    std::int32_t bones[4];
    float weights[4];
    float center[4], r0[4], r1[4];
    float additionalUv[4][4];
};
// Bone transform record shared with d3d12_skinning.hlsl. matrix is the full
// affine bone transform (row-vector convention, translation in row 3).
// quaternion is always a legal rotation quaternion: D3DXQuaternionRotationMatrix's
// value for rigid bones, the polar-decomposition rotation (A = H * R) for
// affine ones - see affine_skinning.hpp. The affine extension adds stretch
// (the positive-definite H rows), normalMatrix (inverse transpose of the
// linear part rows) and flags; flags bit 0 marks those valid. A record with
// flags 0 - the default {} - takes the legacy rigid path in the shader, so
// older submissions that fill only matrix and quaternion behave exactly as
// before. PrepareGpuModelInput fills every field.
struct Bone {
    float matrix[16];
    float quaternion[4];
    float stretch[3][4];       // H rows in xyz, w unused
    float normalMatrix[3][4];  // (linear)^-T rows in xyz, w unused
    std::uint32_t flags;       // bit0: affine extension valid
    std::uint32_t reserved[3];
};
struct Material { float edgeColor[4], edgeSize, edgeDistance; std::uint32_t pmd, opaqueEdge; };
struct Output {
    float position[3], normal[3], uv[2], additionalUv[4][4];
    float edgePosition[3]; std::uint32_t edgeColor;
};
static_assert(sizeof(Vertex)==192 && sizeof(Bone)==192 && sizeof(Material)==32 && sizeof(Output)==112);
// The HLSL struct lays the same fields out at the same offsets; keep the
// 16-byte rows of the affine extension pinned for the shader.
static_assert(offsetof(Bone, quaternion) == 64 && offsetof(Bone, stretch) == 80 &&
              offsetof(Bone, normalMatrix) == 128 && offsetof(Bone, flags) == 176,
              "bone record ABI shared with d3d12_skinning.hlsl");
struct Parameters {
    float edgeDistance; bool opaqueEdge; bool pmd; bool perVertex = false;
    // Nonzero revision promises unchanged geometry until the revision changes.
    // Zero always uploads, for callers without an invalidation contract.
    std::uint64_t geometryRevision = 0;
};
struct Ticket { std::uint64_t fence = 0; unsigned slot = 0; std::uint32_t count = 0; };

// One adapter usable for D3D12 compute skinning.
// id packs the DXGI LUID losslessly: the high 32 bits hold LUID.HighPart and the
// low 32 bits hold LUID.LowPart, so an id survives a round trip through a
// configuration file unchanged.
struct Adapter {
    std::uint64_t id = 0;
    std::wstring name;
    bool integrated = false;
    std::wstring hardwareKey; // Stable GPU identity and driver; excludes reboot-dependent LUID.
};
// Hardware adapters that run D3D12 at feature level 11_0; software and remote
// adapters are excluded. Discrete adapters come first, then integrated ones, and
// each group keeps the DXGI high-performance order.
std::vector<Adapter> EnumerateAdapters();

// Direct queue, three reusable submission slots. The native preview consumes
// OutputResource directly. The complete D3D9/MME renderer uses SubmitAndReadback
// to copy the result into its existing vertex buffers.
class Skinning {
public:
    Skinning();
    ~Skinning();
    Skinning(const Skinning&) = delete;
    Skinning& operator=(const Skinning&) = delete;
    // debug enables the D3D12 debug layer before any device is created.
    // adapterId 0 selects automatically and walks every enumerated adapter in order
    // (discrete first) onto the next one when an initialisation fails. A nonzero
    // adapterId is the exact id from EnumerateAdapters: it initialises on that
    // adapter or returns false, never silently switching to another card.
    bool Initialize(bool debug = false, std::uint64_t adapterId = 0);
    bool Submit(const std::vector<Vertex>& vertices, const std::vector<Bone>& bones,
                const std::vector<Material>& materials, Parameters parameters, Ticket& ticket);
    // Same work as Submit, with the output copy appended to the same command list:
    // one ExecuteCommandLists and one fence wait for the results. ticket is optional
    // and still addresses the output resource afterwards.
    bool SubmitAndReadback(const std::vector<Vertex>& vertices, const std::vector<Bone>& bones,
                           const std::vector<Material>& materials, Parameters parameters,
                           std::vector<Output>& output, Ticket* ticket = nullptr);
    // D3D9 layout: 32 + 16 * additionalUvCount body bytes, then 16 outline
    // bytes per vertex. Packs existing output bits on the GPU before readback;
    // the normal OutputResource layout and native rendering stay unchanged.
    bool SubmitAndReadbackCompact(const std::vector<Vertex>& vertices, const std::vector<Bone>& bones,
                                  const std::vector<Material>& materials, Parameters parameters,
                                  unsigned additionalUvCount, std::vector<unsigned char>& output);
    // Copies an already submitted output to CPU. Usable by the production D3D9
    // bridge as well as by validation; rendering keeps consuming OutputResource.
    bool Readback(Ticket ticket, std::vector<Output>& output);
    bool Wait(Ticket ticket);
    ID3D12Resource* OutputResource(Ticket ticket) const;
    ID3D12Device* Device() const;
    ID3D12CommandQueue* Queue() const;
    // Call after submitting graphics work on Queue() that consumes this output.
    bool RetainForQueuedGraphics(Ticket ticket);
    // Wait for in-flight work and release model buffers, keeping the device
    // and compiled pipeline. All existing output tickets become invalid.
    bool ReleaseWorkingBuffers();
    const char* Error() const;
    const wchar_t* AdapterName() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
