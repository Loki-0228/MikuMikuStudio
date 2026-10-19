#ifndef MIKUMIKUSTUDIO_RENDER_VIEWPORT_CURSOR_STYLE_HPP_
#define MIKUMIKUSTUDIO_RENDER_VIEWPORT_CURSOR_STYLE_HPP_
#include <cstdint>
struct IDirect3DTexture9;
namespace mikumikustudio {
// Rebuild the eight rotate/move cursor cells once after loading the HUD sheet.
// Only x >= 256 changes; operation buttons, UVs and input regions stay intact.
// Flat red/green/blue fills, black outlines and a light rim replace the old
// shading. The ring uses 70% opacity; its red/blue axes remain opaque.
// Move cursors have a square origin instead of a hand.
// Exactly 512x512 A8R8G8B8 is accepted; invalid textures are left untouched.
// Managed textures preserve the generated pixels across a device reset.
bool RestyleViewportCursorAtlas(IDirect3DTexture9* texture);
// CPU rasterizer. Padded row strides are supported; non-target bytes stay intact.
void RestyleViewportCursorAtlasPixels(std::uint32_t* pixels, int width,
                                      int height, int stridePixels);
}  // namespace mikumikustudio
#endif
