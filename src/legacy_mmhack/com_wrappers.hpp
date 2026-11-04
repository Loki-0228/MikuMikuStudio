// Generated from standard Microsoft COM declarations. Behavioral overrides are hand-written.
#pragma once
#include "runtime.hpp"
#include "scene_router.hpp"
namespace mme_re::mmhack {
class Direct3DWrapper final : public IDirect3D9 {
public:
  Direct3DWrapper(IDirect3D9* original, Runtime& runtime);
  ~Direct3DWrapper();
  IDirect3D9* underlying() const noexcept { return original_; }
  // Original slot 0: 0x180001970
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { return original_->QueryInterface(riid, ppvObj); }
  // Original slot 1: 0x180001980
  ULONG STDMETHODCALLTYPE AddRef() override { return original_->AddRef(); }
  // Original slot 2: 0x180003e40
  ULONG STDMETHODCALLTYPE Release() override;
  // Original slot 3: 0x180003e90
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return original_->RegisterSoftwareDevice(pInitializeFunction); }
  // Original slot 4: 0x1800019e0
  UINT STDMETHODCALLTYPE GetAdapterCount() override { return original_->GetAdapterCount(); }
  // Original slot 5: 0x180003ea0
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return original_->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
  // Original slot 6: 0x1800019f0
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return original_->GetAdapterModeCount(Adapter, Format); }
  // Original slot 7: 0x180003eb0
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return original_->EnumAdapterModes(Adapter, Format, Mode, pMode); }
  // Original slot 8: 0x180001a10
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return original_->GetAdapterDisplayMode(Adapter, pMode); }
  // Original slot 9: 0x180003ee0
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return original_->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
  // Original slot 10: 0x180003f20
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return original_->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
  // Original slot 11: 0x180003f60
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return original_->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
  // Original slot 12: 0x180003fb0
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return original_->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
  // Original slot 13: 0x180003ff0
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return original_->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
  // Original slot 14: 0x180004020
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return original_->GetDeviceCaps(Adapter, DeviceType, pCaps); }
  // Original slot 15: 0x180003ed0
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return original_->GetAdapterMonitor(Adapter); }
  // Original slot 16: 0x180004040
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) override;
private:
  IDirect3D9* original_;
  Runtime& runtime_;
};
class DeviceWrapper final : public IDirect3DDevice9 {
public:
  DeviceWrapper(IDirect3DDevice9* original, Runtime& runtime);
  ~DeviceWrapper();
  IDirect3DDevice9* underlying() const noexcept { return original_; }
  // Original slot 0: 0x180001970
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { return original_->QueryInterface(riid, ppvObj); }
  // Original slot 1: 0x180001d40
  ULONG STDMETHODCALLTYPE AddRef() override;
  // Original slot 2: 0x180001d50
  ULONG STDMETHODCALLTYPE Release() override;
  // Original slot 3: 0x180003e90
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return original_->TestCooperativeLevel(); }
  // Original slot 4: 0x1800019e0
  UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return original_->GetAvailableTextureMem(); }
  // Original slot 5: 0x180003ea0
  HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return original_->EvictManagedResources(); }
  // Original slot 6: 0x180001e90
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override;
  // Original slot 7: 0x180001a00
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { return original_->GetDeviceCaps(pCaps); }
  // Original slot 8: 0x180001a10
  HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override { return original_->GetDisplayMode(iSwapChain, pMode); }
  // Original slot 9: 0x180001a20
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { return original_->GetCreationParameters(pParameters); }
  // Original slot 10: 0x180001ec0
  HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override { return original_->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
  // Original slot 11: 0x180001ed0
  void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override { return original_->SetCursorPosition(X, Y, Flags); }
  // Original slot 12: 0x180001ee0
  BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return original_->ShowCursor(bShow); }
  // Original slot 13: 0x180001a90
  HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override { return original_->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
  // Original slot 14: 0x180001aa0
  HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override { return original_->GetSwapChain(iSwapChain, pSwapChain); }
  // Original slot 15: 0x180003ed0
  UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return original_->GetNumberOfSwapChains(); }
  // Original slot 16: 0x180003d30
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override;
  // Original slot 17: 0x180003cf0
  HRESULT STDMETHODCALLTYPE Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) override;
  // Original slot 18: 0x180001ef0
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override { return original_->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
  // Original slot 19: 0x180001ae0
  HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { return original_->GetRasterStatus(iSwapChain, pRasterStatus); }
  // Original slot 20: 0x180001af0
  HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { return original_->SetDialogBoxMode(bEnableDialogs); }
  // Original slot 21: 0x180001f20
  void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) override { return original_->SetGammaRamp(iSwapChain, Flags, pRamp); }
  // Original slot 22: 0x180001b00
  void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override { return original_->GetGammaRamp(iSwapChain, pRamp); }
  // Original slot 23: 0x180001f30
  HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override { return original_->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }
  // Original slot 24: 0x180001f90
  HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override { return original_->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
  // Original slot 25: 0x180001ff0
  HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override { return original_->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
  // Original slot 26: 0x180002040
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override { return original_->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
  // Original slot 27: 0x180002d40
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override;
  // Original slot 28: 0x180002080
  HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return original_->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
  // Original slot 29: 0x1800020e0
  HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return original_->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
  // Original slot 30: 0x180003bd0
  HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) override;
  // Original slot 31: 0x180002140
  HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override { return original_->UpdateTexture(pSourceTexture, pDestinationTexture); }
  // Original slot 32: 0x180003c50
  HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override;
  // Original slot 33: 0x180002150
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override { return original_->GetFrontBufferData(iSwapChain, pDestSurface); }
  // Original slot 34: 0x180003b50
  HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override;
  // Original slot 35: 0x180002160
  HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) override { return original_->ColorFill(pSurface, pRect, color); }
  // Original slot 36: 0x180002170
  HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return original_->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
  // Original slot 37: 0x1800021b0
  HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override { return original_->SetRenderTarget(RenderTargetIndex, pRenderTarget); }
  // Original slot 38: 0x1800021c0
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override { return original_->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
  // Original slot 39: 0x1800021d0
  HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { return original_->SetDepthStencilSurface(pNewZStencil); }
  // Original slot 40: 0x1800021e0
  HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { return original_->GetDepthStencilSurface(ppZStencilSurface); }
  // Original slot 41: 0x180002de0
  HRESULT STDMETHODCALLTYPE BeginScene() override;
  // Original slot 42: 0x180003cb0
  HRESULT STDMETHODCALLTYPE EndScene() override;
  // Original slot 43: 0x180003a20
  HRESULT STDMETHODCALLTYPE Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
  // Original slot 44: 0x1800021f0
  HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override { return original_->SetTransform(State, pMatrix); }
  // Original slot 45: 0x180002200
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override { return original_->GetTransform(State, pMatrix); }
  // Original slot 46: 0x180002210
  HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE argument_0, CONST D3DMATRIX* argument_1) override { return original_->MultiplyTransform(argument_0, argument_1); }
  // Original slot 47: 0x180002220
  HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9* pViewport) override { return original_->SetViewport(pViewport); }
  // Original slot 48: 0x180002230
  HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { return original_->GetViewport(pViewport); }
  // Original slot 49: 0x180002240
  HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9* pMaterial) override { return original_->SetMaterial(pMaterial); }
  // Original slot 50: 0x180002250
  HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { return original_->GetMaterial(pMaterial); }
  // Original slot 51: 0x180002260
  HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, CONST D3DLIGHT9* argument_1) override { return original_->SetLight(Index, argument_1); }
  // Original slot 52: 0x180002270
  HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9* argument_1) override { return original_->GetLight(Index, argument_1); }
  // Original slot 53: 0x180002280
  HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { return original_->LightEnable(Index, Enable); }
  // Original slot 54: 0x180002290
  HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable) override { return original_->GetLightEnable(Index, pEnable); }
  // Original slot 55: 0x1800022a0
  HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, CONST float* pPlane) override { return original_->SetClipPlane(Index, pPlane); }
  // Original slot 56: 0x1800022b0
  HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override { return original_->GetClipPlane(Index, pPlane); }
  // Original slot 57: 0x1800022c0
  HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { return original_->SetRenderState(State, Value); }
  // Original slot 58: 0x1800022d0
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override { return original_->GetRenderState(State, pValue); }
  // Original slot 59: 0x1800022e0
  HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override { return original_->CreateStateBlock(Type, ppSB); }
  // Original slot 60: 0x1800022f0
  HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return original_->BeginStateBlock(); }
  // Original slot 61: 0x180002300
  HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { return original_->EndStateBlock(ppSB); }
  // Original slot 62: 0x180002310
  HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) override { return original_->SetClipStatus(pClipStatus); }
  // Original slot 63: 0x180002320
  HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { return original_->GetClipStatus(pClipStatus); }
  // Original slot 64: 0x180002330
  HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override { return original_->GetTexture(Stage, ppTexture); }
  // Original slot 65: 0x180002340
  HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override { return original_->SetTexture(Stage, pTexture); }
  // Original slot 66: 0x180002350
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override { return original_->GetTextureStageState(Stage, Type, pValue); }
  // Original slot 67: 0x180002360
  HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { return original_->SetTextureStageState(Stage, Type, Value); }
  // Original slot 68: 0x180002370
  HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override { return original_->GetSamplerState(Sampler, Type, pValue); }
  // Original slot 69: 0x180002380
  HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override { return original_->SetSamplerState(Sampler, Type, Value); }
  // Original slot 70: 0x180002390
  HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { return original_->ValidateDevice(pNumPasses); }
  // Original slot 71: 0x1800023a0
  HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) override { return original_->SetPaletteEntries(PaletteNumber, pEntries); }
  // Original slot 72: 0x1800023b0
  HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override { return original_->GetPaletteEntries(PaletteNumber, pEntries); }
  // Original slot 73: 0x1800023c0
  HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return original_->SetCurrentTexturePalette(PaletteNumber); }
  // Original slot 74: 0x1800023d0
  HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { return original_->GetCurrentTexturePalette(PaletteNumber); }
  // Original slot 75: 0x1800023e0
  HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT* pRect) override { return original_->SetScissorRect(pRect); }
  // Original slot 76: 0x1800023f0
  HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { return original_->GetScissorRect(pRect); }
  // Original slot 77: 0x180002400
  HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { return original_->SetSoftwareVertexProcessing(bSoftware); }
  // Original slot 78: 0x180002410
  BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return original_->GetSoftwareVertexProcessing(); }
  // Original slot 79: 0x180002420
  HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return original_->SetNPatchMode(nSegments); }
  // Original slot 80: 0x180002430
  float STDMETHODCALLTYPE GetNPatchMode() override { return original_->GetNPatchMode(); }
  // Original slot 81: 0x180002780
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
  // Original slot 82: 0x1800027f0
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE argument_0, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override;
  // Original slot 83: 0x180002440
  HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return original_->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
  // Original slot 84: 0x180002460
  HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return original_->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
  // Original slot 85: 0x1800024c0
  HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { return original_->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
  // Original slot 86: 0x180002500
  HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override { return original_->CreateVertexDeclaration(pVertexElements, ppDecl); }
  // Original slot 87: 0x180002510
  HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { return original_->SetVertexDeclaration(pDecl); }
  // Original slot 88: 0x180002520
  HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { return original_->GetVertexDeclaration(ppDecl); }
  // Original slot 89: 0x180002530
  HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return original_->SetFVF(FVF); }
  // Original slot 90: 0x180002540
  HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { return original_->GetFVF(pFVF); }
  // Original slot 91: 0x180002550
  HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override { return original_->CreateVertexShader(pFunction, ppShader); }
  // Original slot 92: 0x180002560
  HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override { return original_->SetVertexShader(pShader); }
  // Original slot 93: 0x180002570
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { return original_->GetVertexShader(ppShader); }
  // Original slot 94: 0x180002580
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override { return original_->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
  // Original slot 95: 0x180002590
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return original_->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
  // Original slot 96: 0x1800025a0
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override { return original_->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
  // Original slot 97: 0x1800025b0
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return original_->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
  // Original slot 98: 0x1800025c0
  HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) override { return original_->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
  // Original slot 99: 0x1800025d0
  HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return original_->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
  // Original slot 100: 0x1800025e0
  HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override { return original_->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
  // Original slot 101: 0x180002600
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) override { return original_->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
  // Original slot 102: 0x180002630
  HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override { return original_->SetStreamSourceFreq(StreamNumber, Setting); }
  // Original slot 103: 0x180002640
  HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) override { return original_->GetStreamSourceFreq(StreamNumber, pSetting); }
  // Original slot 104: 0x180002650
  HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { return original_->SetIndices(pIndexData); }
  // Original slot 105: 0x180002660
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { return original_->GetIndices(ppIndexData); }
  // Original slot 106: 0x180002670
  HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override { return original_->CreatePixelShader(pFunction, ppShader); }
  // Original slot 107: 0x180002680
  HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override { return original_->SetPixelShader(pShader); }
  // Original slot 108: 0x180002690
  HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { return original_->GetPixelShader(ppShader); }
  // Original slot 109: 0x1800026a0
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override { return original_->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
  // Original slot 110: 0x1800026b0
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return original_->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
  // Original slot 111: 0x1800026c0
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override { return original_->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
  // Original slot 112: 0x1800026d0
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return original_->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
  // Original slot 113: 0x1800026e0
  HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) override { return original_->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
  // Original slot 114: 0x1800026f0
  HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return original_->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
  // Original slot 115: 0x180002700
  HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) override { return original_->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
  // Original slot 116: 0x180002710
  HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) override { return original_->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
  // Original slot 117: 0x180002720
  HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return original_->DeletePatch(Handle); }
  // Original slot 118: 0x180002730
  HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override { return original_->CreateQuery(Type, ppQuery); }
private:
  IDirect3DDevice9* original_;
  Runtime& runtime_;
  ULONG local_references_ = 1;
  SceneRouter router_;
};
class MeshWrapper final : public ID3DXMesh {
public:
  MeshWrapper(ID3DXMesh* original, Runtime& runtime);
  ~MeshWrapper();
  ID3DXMesh* underlying() const noexcept { return original_; }
  // Original slot 0: 0x180001970
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override { return original_->QueryInterface(iid, ppv); }
  // Original slot 1: 0x180001980
  ULONG STDMETHODCALLTYPE AddRef() override { return original_->AddRef(); }
  // Original slot 2: 0x180001990
  ULONG STDMETHODCALLTYPE Release() override;
  // Original slot 3: 0x180003e90
  HRESULT STDMETHODCALLTYPE DrawSubset(DWORD AttribId) override { return original_->DrawSubset(AttribId); }
  // Original slot 4: 0x1800019e0
  DWORD STDMETHODCALLTYPE GetNumFaces() override { return original_->GetNumFaces(); }
  // Original slot 5: 0x180003ea0
  DWORD STDMETHODCALLTYPE GetNumVertices() override { return original_->GetNumVertices(); }
  // Original slot 6: 0x1800019f0
  DWORD STDMETHODCALLTYPE GetFVF() override { return original_->GetFVF(); }
  // Original slot 7: 0x180001a00
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 Declaration[MAX_FVF_DECL_SIZE]) override { return original_->GetDeclaration(Declaration); }
  // Original slot 8: 0x180001a10
  DWORD STDMETHODCALLTYPE GetNumBytesPerVertex() override { return original_->GetNumBytesPerVertex(); }
  // Original slot 9: 0x180001a20
  DWORD STDMETHODCALLTYPE GetOptions() override { return original_->GetOptions(); }
  // Original slot 10: 0x180001ec0
  HRESULT STDMETHODCALLTYPE GetDevice(LPDIRECT3DDEVICE9* ppDevice) override { return original_->GetDevice(ppDevice); }
  // Original slot 11: 0x180001a30
  HRESULT STDMETHODCALLTYPE CloneMeshFVF(DWORD Options, DWORD FVF, LPDIRECT3DDEVICE9 pD3DDevice, LPD3DXMESH* ppCloneMesh) override;
  // Original slot 12: 0x180001a60
  HRESULT STDMETHODCALLTYPE CloneMesh(DWORD Options, CONST D3DVERTEXELEMENT9 *pDeclaration, LPDIRECT3DDEVICE9 pD3DDevice, LPD3DXMESH* ppCloneMesh) override;
  // Original slot 13: 0x180001a90
  HRESULT STDMETHODCALLTYPE GetVertexBuffer(LPDIRECT3DVERTEXBUFFER9* ppVB) override { return original_->GetVertexBuffer(ppVB); }
  // Original slot 14: 0x180001aa0
  HRESULT STDMETHODCALLTYPE GetIndexBuffer(LPDIRECT3DINDEXBUFFER9* ppIB) override { return original_->GetIndexBuffer(ppIB); }
  // Original slot 15: 0x180003ed0
  HRESULT STDMETHODCALLTYPE LockVertexBuffer(DWORD Flags, LPVOID *ppData) override { return original_->LockVertexBuffer(Flags, ppData); }
  // Original slot 16: 0x180001ab0
  HRESULT STDMETHODCALLTYPE UnlockVertexBuffer() override { return original_->UnlockVertexBuffer(); }
  // Original slot 17: 0x180001ac0
  HRESULT STDMETHODCALLTYPE LockIndexBuffer(DWORD Flags, LPVOID *ppData) override { return original_->LockIndexBuffer(Flags, ppData); }
  // Original slot 18: 0x180001ad0
  HRESULT STDMETHODCALLTYPE UnlockIndexBuffer() override { return original_->UnlockIndexBuffer(); }
  // Original slot 19: 0x180001ae0
  HRESULT STDMETHODCALLTYPE GetAttributeTable(D3DXATTRIBUTERANGE *pAttribTable, DWORD* pAttribTableSize) override { return original_->GetAttributeTable(pAttribTable, pAttribTableSize); }
  // Original slot 20: 0x180001af0
  HRESULT STDMETHODCALLTYPE ConvertPointRepsToAdjacency(CONST DWORD* pPRep, DWORD* pAdjacency) override { return original_->ConvertPointRepsToAdjacency(pPRep, pAdjacency); }
  // Original slot 21: 0x180001f20
  HRESULT STDMETHODCALLTYPE ConvertAdjacencyToPointReps(CONST DWORD* pAdjacency, DWORD* pPRep) override { return original_->ConvertAdjacencyToPointReps(pAdjacency, pPRep); }
  // Original slot 22: 0x180001b00
  HRESULT STDMETHODCALLTYPE GenerateAdjacency(FLOAT Epsilon, DWORD* pAdjacency) override { return original_->GenerateAdjacency(Epsilon, pAdjacency); }
  // Original slot 23: 0x180001b10
  HRESULT STDMETHODCALLTYPE UpdateSemantics(D3DVERTEXELEMENT9 Declaration[MAX_FVF_DECL_SIZE]) override { return original_->UpdateSemantics(Declaration); }
  // Original slot 24: 0x180001b20
  HRESULT STDMETHODCALLTYPE LockAttributeBuffer(DWORD Flags, DWORD** ppData) override { return original_->LockAttributeBuffer(Flags, ppData); }
  // Original slot 25: 0x180001b30
  HRESULT STDMETHODCALLTYPE UnlockAttributeBuffer() override { return original_->UnlockAttributeBuffer(); }
  // Original slot 26: 0x180001b40
  HRESULT STDMETHODCALLTYPE Optimize(DWORD Flags, CONST DWORD* pAdjacencyIn, DWORD* pAdjacencyOut, DWORD* pFaceRemap, LPD3DXBUFFER *ppVertexRemap, LPD3DXMESH* ppOptMesh) override;
  // Original slot 27: 0x180001b90
  HRESULT STDMETHODCALLTYPE OptimizeInplace(DWORD Flags, CONST DWORD* pAdjacencyIn, DWORD* pAdjacencyOut, DWORD* pFaceRemap, LPD3DXBUFFER *ppVertexRemap) override { return original_->OptimizeInplace(Flags, pAdjacencyIn, pAdjacencyOut, pFaceRemap, ppVertexRemap); }
  // Original slot 28: 0x180001bc0
  HRESULT STDMETHODCALLTYPE SetAttributeTable(CONST D3DXATTRIBUTERANGE *pAttribTable, DWORD cAttribTableSize) override { return original_->SetAttributeTable(pAttribTable, cAttribTableSize); }
private:
  ID3DXMesh* original_;
  Runtime& runtime_;
};
}
