#pragma once

#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mikumikustudio {
enum class ModelAlphaPass { Original, Opaque, Translucent };

// Partition materials, not pixels: submit every material exactly once, including
// draws intercepted by MME. Preserve authored order within each group and the
// depth writes which keep inverted-hull outlines behind transparent hair.
inline bool ModelMaterialMatchesPass(ModelAlphaPass pass, bool usesAlpha) {
    return pass == ModelAlphaPass::Original ||
        (pass == ModelAlphaPass::Translucent) == usesAlpha;
}

// The alpha that reaches the blend stage. The translucent-display override
// forces 0.5 on every material of the model; otherwise the authored diffuse
// alpha is used unchanged.
inline float EffectiveMaterialAlpha(float diffuseAlpha, bool translucentDisplay) {
    return translucentDisplay ? 0.5f : diffuseAlpha;
}

// Whether an effective alpha can contribute to transparent compositing at
// all. Exactly zero cannot: the engine's alpha test is GREATEREQUAL against
// reference 1, so an alpha-0 fragment is rejected before blending and writes
// neither colour nor depth. A hidden helper material authored with alpha 0
// therefore never needs transparent sorting or peeling, even when its texture
// carries alpha, while its draw is still submitted exactly once through the
// opaque pass. Any positive alpha, however small, still blends and stays
// translucent.
inline bool MaterialAlphaVisible(float effectiveAlpha) {
    return effectiveAlpha > 0.0f;
}

// resourcePool tag's low three bytes store the toon color. The fourth byte is
// host-only alpha metadata, computed once from the actual loaded texture.
constexpr unsigned char kTextureAlphaKnown = 1;
constexpr unsigned char kTextureUsesAlpha = 2;

inline bool TexturePixelsUseAlpha(const D3DLOCKED_RECT& pixels, UINT width,
                                 UINT height) {
    for (UINT y = 0; y < height; ++y) {
        const auto* row = static_cast<const unsigned char*>(pixels.pBits) +
            static_cast<std::size_t>(y) * pixels.Pitch;
        for (UINT x = 0; x < width; ++x)
            if (row[4 * x + 3] != 255)
                return true;
    }
    return false;
}

inline void CacheTextureColorAndAlpha(IDirect3DTexture9* texture,
                                     unsigned char* tag) {
    // Unknown metadata is not evidence of transparency. Keep these materials
    // in their authored pass instead of sorting opaque DDS/RGB surfaces.
    tag[3] = 0;
    D3DSURFACE_DESC desc{};
    D3DLOCKED_RECT pixels{};
    if (texture == nullptr || FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height)
        return;
    switch(desc.Format) {
    case D3DFMT_X8R8G8B8:case D3DFMT_X8B8G8R8:case D3DFMT_R8G8B8:
    case D3DFMT_R5G6B5:case D3DFMT_X1R5G5B5:case D3DFMT_L8:case D3DFMT_L16:
    case D3DFMT_R16F:case D3DFMT_R32F:case D3DFMT_G16R16:case D3DFMT_G16R16F:case D3DFMT_G32R32F:
        tag[3]=kTextureAlphaKnown;return;
    default:break;
    }
    const bool rgba=desc.Format==D3DFMT_A8R8G8B8 || desc.Format==D3DFMT_A8B8G8R8;
    const bool dxt1=desc.Format==D3DFMT_DXT1;
    const bool dxt3=desc.Format==D3DFMT_DXT2 || desc.Format==D3DFMT_DXT3;
    const bool dxt5=desc.Format==D3DFMT_DXT4 || desc.Format==D3DFMT_DXT5;
    if((!rgba && !dxt1 && !dxt3 && !dxt5) || FAILED(texture->LockRect(0,&pixels,nullptr,D3DLOCK_READONLY)))return;
    if(!rgba) {
        bool alpha=false;
        for(UINT y=0;y<desc.Height && !alpha;y+=4)for(UINT x=0;x<desc.Width && !alpha;x+=4) {
            const auto* block=static_cast<const unsigned char*>(pixels.pBits)+(y/4)*pixels.Pitch+(x/4)*(dxt1?8:16);
            std::uint16_t c0=0,c1=0;std::uint32_t colors=0;
            if(dxt1){std::memcpy(&c0,block,2);std::memcpy(&c1,block+2,2);std::memcpy(&colors,block+4,4);}
            unsigned values[8]{block[0],block[1]};std::uint64_t selectors=0;
            if(dxt5) {
                if(values[0]>values[1])for(unsigned i=2;i<8;++i)values[i]=((8-i)*values[0]+(i-1)*values[1])/7;
                else {for(unsigned i=2;i<6;++i)values[i]=((6-i)*values[0]+(i-1)*values[1])/5;values[6]=0;values[7]=255;}
                for(unsigned i=0;i<6;++i)selectors|=std::uint64_t(block[2+i])<<(8*i);
            }
            for(UINT by=0;by<4 && y+by<desc.Height;++by)for(UINT bx=0;bx<4 && x+bx<desc.Width;++bx) {
                const unsigned pixel=by*4+bx;
                alpha|=dxt1 ? c0<=c1 && ((colors>>(2*pixel))&3)==3 : dxt3 ?
                    ((block[pixel/2]>>(4*(pixel%2)))&15)!=15 : values[(selectors>>(3*pixel))&7]!=255;
            }
        }
        texture->UnlockRect(0);tag[3]=kTextureAlphaKnown|(alpha?kTextureUsesAlpha:0);return;
    }
    const auto* lastRow = static_cast<const unsigned char*>(pixels.pBits) +
        static_cast<std::size_t>(desc.Height - 1) * pixels.Pitch;
    tag[0] = lastRow[desc.Format==D3DFMT_A8R8G8B8?2:0];
    tag[1] = lastRow[1];
    tag[2] = lastRow[desc.Format==D3DFMT_A8R8G8B8?0:2];
    tag[3] = kTextureAlphaKnown |
        (TexturePixelsUseAlpha(pixels, desc.Width, desc.Height) ? kTextureUsesAlpha : 0);
    texture->UnlockRect(0);
}
}
