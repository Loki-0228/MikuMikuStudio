// ===========================================================================
// D3D9 vertex-shader output capture
// ===========================================================================
// MME/MMV effects (and MMD's own GPU skinning path) deform vertices inside the
// vertex shader: the POSITION the rasterizer receives never exists in the input
// vertex buffer.  Alembic export therefore has to read the *deformed* position
// back from the GPU; re-reading the input vertex buffer would export the
// undeformed mesh but look like a successful capture.
//
// Capture() reads the POSITION that the currently bound vertex shader computes
// for vertex indices [firstVertex, firstVertex + vertexCount).  It is meant to
// be called while the native D3D9 device carries the real draw's input streams,
// vertex declaration, vertex shader, constants and vertex textures, with a
// BeginScene already open.  Only the shader matters to the caller: the device
// state is saved and put back, so the open scene continues unchanged.
//
// The result is the homogeneous clip-space position (x, y, z, w) that the
// shader's POSITION output receives, i.e. before the viewport transform and
// before the perspective divide.  Callers that need view/model space divide by
// w and apply the inverse matrices themselves; that keeps the capture
// independent of what the shader does with the transform.
//
// Returns false and fills `error` with the concrete reason when:
//   * `device` is null or the vertex range overflows unsigned
//   * no vertex shader is bound (the caller's fixed-function path handles it)
//   * no vertex declaration is bound (an FVF input layout cannot be extended)
//   * the shader version is not vs_1_1 / vs_2_0 / vs_3_0
//   * a vs_3_0 shader declares no POSITION output, or more than one
//   * the shader writes no POSITION output at all
//   * no free output register, input register, temporary register or TEXCOORD
//     semantic is left to carry the capture
//   * the shader uses subroutines (call / callnz / ret / label)
//   * the bytecode is truncated or has unbalanced flow control
//   * the vertex streams are instanced, or no free stream is left
//   * the device cannot provide a float render target or the readback fails
// Every one of those cases is reported, never silently downgraded to a partial
// or unvalidated result.
//
// A vertexCount of 0 is accepted and yields an empty result.  On failure the
// output vector is cleared, so a caller can never mistake stale data for a
// capture.
// ===========================================================================
#pragma once

#include <d3d9.h>

#include <array>
#include <string>
#include <vector>
#include <memory>
#include <wrl/client.h>

namespace mikumikustudio {
namespace vertex_capture {

// Reads the POSITION output of the bound vertex shader for vertexCount vertices
// starting at firstVertex.  `clipPositions[i]` receives the clip-space position
// of vertex (firstVertex + i).
bool Capture(IDirect3DDevice9* device, unsigned firstVertex, unsigned vertexCount,
             std::vector<std::array<float, 4>>& clipPositions, std::string& error);

// Immutable while a caller holds the result. Each vertex occupies one RGBA32F
// texel; rasterPositions supplies that texel's clip-space centre. No readback.
struct GpuPositions {
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture;
    Microsoft::WRL::ComPtr<IDirect3DVertexBuffer9> rasterPositions;
    unsigned firstVertex=0, vertexCount=0, width=0, height=0;
};
class GpuCapture {
public:
    GpuCapture();
    ~GpuCapture();
    GpuCapture(const GpuCapture&)=delete;
    GpuCapture& operator=(const GpuCapture&)=delete;
    bool Capture(IDirect3DDevice9*, unsigned firstVertex, unsigned vertexCount,
                 std::shared_ptr<const GpuPositions>&, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vertex_capture
}  // namespace mikumikustudio
