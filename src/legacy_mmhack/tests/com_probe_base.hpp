// Synthetic COM interfaces for CPU-only lifetime regressions.
#pragma once
#include "../runtime.hpp"
namespace mme_re::mmhack::test {
class Direct3DProbeBase : public IDirect3D9 {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { return E_NOTIMPL; }
  ULONG STDMETHODCALLTYPE AddRef() override { return {}; }
  ULONG STDMETHODCALLTYPE Release() override { return {}; }
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return E_NOTIMPL; }
  UINT STDMETHODCALLTYPE GetAdapterCount() override { return {}; }
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return E_NOTIMPL; }
  UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return {}; }
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return E_NOTIMPL; }
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return {}; }
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) override { return E_NOTIMPL; }
};
class MeshProbeBase : public ID3DXMesh {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override { return E_NOTIMPL; }
  ULONG STDMETHODCALLTYPE AddRef() override { return {}; }
  ULONG STDMETHODCALLTYPE Release() override { return {}; }
  HRESULT STDMETHODCALLTYPE DrawSubset(DWORD AttribId) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE GetNumFaces() override { return {}; }
  DWORD STDMETHODCALLTYPE GetNumVertices() override { return {}; }
  DWORD STDMETHODCALLTYPE GetFVF() override { return {}; }
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 Declaration[MAX_FVF_DECL_SIZE]) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE GetNumBytesPerVertex() override { return {}; }
  DWORD STDMETHODCALLTYPE GetOptions() override { return {}; }
  HRESULT STDMETHODCALLTYPE GetDevice(LPDIRECT3DDEVICE9* ppDevice) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CloneMeshFVF(DWORD Options, DWORD FVF, LPDIRECT3DDEVICE9 pD3DDevice, LPD3DXMESH* ppCloneMesh) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CloneMesh(DWORD Options, CONST D3DVERTEXELEMENT9 *pDeclaration, LPDIRECT3DDEVICE9 pD3DDevice, LPD3DXMESH* ppCloneMesh) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVertexBuffer(LPDIRECT3DVERTEXBUFFER9* ppVB) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIndexBuffer(LPDIRECT3DINDEXBUFFER9* ppIB) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockVertexBuffer(DWORD Flags, LPVOID *ppData) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockVertexBuffer() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockIndexBuffer(DWORD Flags, LPVOID *ppData) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockIndexBuffer() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAttributeTable(D3DXATTRIBUTERANGE *pAttribTable, DWORD* pAttribTableSize) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ConvertPointRepsToAdjacency(CONST DWORD* pPRep, DWORD* pAdjacency) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ConvertAdjacencyToPointReps(CONST DWORD* pAdjacency, DWORD* pPRep) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GenerateAdjacency(FLOAT Epsilon, DWORD* pAdjacency) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UpdateSemantics(D3DVERTEXELEMENT9 Declaration[MAX_FVF_DECL_SIZE]) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockAttributeBuffer(DWORD Flags, DWORD** ppData) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnlockAttributeBuffer() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Optimize(DWORD Flags, CONST DWORD* pAdjacencyIn, DWORD* pAdjacencyOut, DWORD* pFaceRemap, LPD3DXBUFFER *ppVertexRemap, LPD3DXMESH* ppOptMesh) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE OptimizeInplace(DWORD Flags, CONST DWORD* pAdjacencyIn, DWORD* pAdjacencyOut, DWORD* pFaceRemap, LPD3DXBUFFER *ppVertexRemap) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetAttributeTable(CONST D3DXATTRIBUTERANGE *pAttribTable, DWORD cAttribTableSize) override { return E_NOTIMPL; }
};
}
