#ifndef MMS_DX12_VIEWPORT_H
#define MMS_DX12_VIEWPORT_H

#include "plugin.h"
#include <d3d12.h>

#define MMS_DX12_VIEWPORT_SERVICE "mms.render.dx12.viewport"

#pragma pack(push, 8)
typedef struct MmsDx12ViewportFrameV1 {
    MmsHeader h;
    uint64_t deviceGeneration;
    uint32_t slot, width, height;
    ID3D12Device* device;
    ID3D12GraphicsCommandList* commands;
    ID3D12Resource* color;
    ID3D12Resource* depth;
    D3D12_CPU_DESCRIPTOR_HANDLE colorRtv;
    ID3D12Resource* vertices;
    ID3D12Resource* indices;
    uint32_t vertexCount, vertexStride, indexCount;
    float worldViewProjection[16], lightDirection[3];
    const wchar_t* projectPath;
    void* lifetime;
    MmsResult (MMS_CALL *retain)(void* lifetime, IUnknown* object);
} MmsDx12ViewportFrameV1;

typedef MmsResult (MMS_CALL *MmsDx12ViewportRecordV1)(void*, const MmsDx12ViewportFrameV1*);
typedef void (MMS_CALL *MmsDx12ViewportRetireV1)(void*, uint64_t deviceGeneration);

typedef struct MmsDx12ViewportApiV1 {
    MmsHeader h;
    void* context;
    MmsResult (MMS_CALL *setCallbacks)(void*, MmsDx12ViewportRecordV1, MmsDx12ViewportRetireV1, void*);
} MmsDx12ViewportApiV1;
#pragma pack(pop)

#endif
