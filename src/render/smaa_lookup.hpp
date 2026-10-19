#pragma once
#include <d3d9.h>
#include <wrl/client.h>
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"

namespace mikumikustudio::antialias {
// Expand the upstream R8G8/R8 tables to universally filterable DX9 RGBA8.
// DEFAULT + UpdateTexture also works on D3D9Ex, which rejects MANAGED textures.
inline HRESULT CreateSmaaLookup(IDirect3DDevice9* device,unsigned width,unsigned height,
    unsigned channels,const unsigned char* bytes,Microsoft::WRL::ComPtr<IDirect3DTexture9>& texture) {
    Microsoft::WRL::ComPtr<IDirect3DTexture9> upload;
    HRESULT hr=device->CreateTexture(width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&upload,nullptr);
    if(FAILED(hr))return hr;
    D3DLOCKED_RECT lock{};
    if(FAILED(hr=upload->LockRect(0,&lock,nullptr,0)))return hr;
    for(unsigned y=0;y<height;++y) {
        auto* row=reinterpret_cast<DWORD*>(static_cast<unsigned char*>(lock.pBits)+y*lock.Pitch);
        for(unsigned x=0;x<width;++x) {
            const auto* value=bytes+(y*width+x)*channels;
            row[x]=D3DCOLOR_ARGB(255,value[0],channels==2?value[1]:value[0],0);
        }
    }
    upload->UnlockRect(0);
    if(FAILED(hr=device->CreateTexture(width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,nullptr)))return hr;
    return device->UpdateTexture(upload.Get(),texture.Get());
}
inline HRESULT CreateSmaaLookups(IDirect3DDevice9* device,
    Microsoft::WRL::ComPtr<IDirect3DTexture9>& area,Microsoft::WRL::ComPtr<IDirect3DTexture9>& search) {
    HRESULT hr=CreateSmaaLookup(device,AREATEX_WIDTH,AREATEX_HEIGHT,2,areaTexBytes,area);
    return FAILED(hr)?hr:CreateSmaaLookup(device,SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,1,searchTexBytes,search);
}
}
