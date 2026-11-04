#pragma once
#include <array>
#include <cstdint>
namespace mme_re::mmhack {
struct TechniqueDescriptor { const char* name; std::uint32_t flags; };
// Read directly from MMHack .rdata, preferred VA 0x18005bdf0, 12 x 16 bytes.
// The last original spelling is DiffuseBSSphiaTexTec.
inline constexpr std::array<TechniqueDescriptor, 12> technique_table{{
  {"ColorRenderTec", 0}, {"ZValuePlotTec", 0}, {"BufferShadowTec", 0},
  {"BShadowTextureTec", 0x1}, {"BShadowSphiaTec", 0x100},
  {"BShadowSphiaTextureTec", 0x101}, {"BShadowTexCd2Tec", 0x10100},
  {"BShadowTextureTexCd2Tec", 0x10101}, {"DiffuseBufferShadowTec", 0x1000000},
  {"DiffuseBSTextureTec", 0x1000001}, {"DiffuseBSSphiaTec", 0x1000100},
  {"DiffuseBSSphiaTexTec", 0x1000101}
}};
}
