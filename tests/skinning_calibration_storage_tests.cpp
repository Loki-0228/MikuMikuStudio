// MAINTENANCE: regression-keep; call the real startup cache-selection path.
// No D3D device, adapter enumeration, GPU pipeline or hardware benchmark runs.
// A typed COM device rejects the first real startup VB allocation on a miss.
#include "../src/model/skinning_backend.cpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <new>
#include <iterator>
#include <cstdlib>
#include <type_traits>

namespace {
namespace fs = std::filesystem;
namespace skin = mikumikustudio::skinning;
namespace cache = skin::calibration_cache;
using mikumikustudio::MMDApp;
using mikumikustudio::MMDAppState;
namespace mdl = mikumikustudio::mdl;
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", message); std::exit(1); }
}
[[noreturn]] void Unexpected(const char* method) {
    std::printf("FAIL: unexpected COM device method: %s\n", method); std::exit(1);
}
struct RejectDevice final : IDirect3DDevice9 {
    unsigned allocations = 0;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
        IDirect3DVertexBuffer9** buffer, HANDLE* shared) override {
        Check(length == 1024u * 32u && usage == 0 && fvf == 0 && pool == D3DPOOL_MANAGED &&
            buffer && shared == nullptr, "real startup calibration requested its first PMD vertex buffer");
        *buffer = nullptr; ++allocations; return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { Unexpected("QueryInterface"); }
    ULONG STDMETHODCALLTYPE AddRef() override { Unexpected("AddRef"); }
    ULONG STDMETHODCALLTYPE Release() override { Unexpected("Release"); }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { Unexpected("TestCooperativeLevel"); }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { Unexpected("GetAvailableTextureMem"); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { Unexpected("EvictManagedResources"); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override { Unexpected("GetDirect3D"); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { Unexpected("GetDeviceCaps"); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain,D3DDISPLAYMODE* pMode) override { Unexpected("GetDisplayMode"); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override { Unexpected("GetCreationParameters"); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot,UINT YHotSpot,IDirect3DSurface9* pCursorBitmap) override { Unexpected("SetCursorProperties"); }
    void STDMETHODCALLTYPE SetCursorPosition(int X,int Y,DWORD Flags) override { Unexpected("SetCursorPosition"); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { Unexpected("ShowCursor"); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DSwapChain9** pSwapChain) override { Unexpected("CreateAdditionalSwapChain"); }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain,IDirect3DSwapChain9** pSwapChain) override { Unexpected("GetSwapChain"); }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { Unexpected("GetNumberOfSwapChains"); }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override { Unexpected("Reset"); }
    HRESULT STDMETHODCALLTYPE Present(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion) override { Unexpected("Present"); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer) override { Unexpected("GetBackBuffer"); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain,D3DRASTER_STATUS* pRasterStatus) override { Unexpected("GetRasterStatus"); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { Unexpected("SetDialogBoxMode"); }
    void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp) override { Unexpected("SetGammaRamp"); }
    void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain,D3DGAMMARAMP* pRamp) override { Unexpected("GetGammaRamp"); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle) override { Unexpected("CreateTexture"); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture9** ppVolumeTexture,HANDLE* pSharedHandle) override { Unexpected("CreateVolumeTexture"); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture9** ppCubeTexture,HANDLE* pSharedHandle) override { Unexpected("CreateCubeTexture"); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer9** ppIndexBuffer,HANDLE* pSharedHandle) override { Unexpected("CreateIndexBuffer"); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { Unexpected("CreateRenderTarget"); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { Unexpected("CreateDepthStencilSurface"); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestinationSurface,CONST POINT* pDestPoint) override { Unexpected("UpdateSurface"); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture,IDirect3DBaseTexture9* pDestinationTexture) override { Unexpected("UpdateTexture"); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget,IDirect3DSurface9* pDestSurface) override { Unexpected("GetRenderTargetData"); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain,IDirect3DSurface9* pDestSurface) override { Unexpected("GetFrontBufferData"); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter) override { Unexpected("StretchRect"); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface,CONST RECT* pRect,D3DCOLOR color) override { Unexpected("ColorFill"); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width,UINT Height,D3DFORMAT Format,D3DPOOL Pool,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle) override { Unexpected("CreateOffscreenPlainSurface"); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget) override { Unexpected("SetRenderTarget"); }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex,IDirect3DSurface9** ppRenderTarget) override { Unexpected("GetRenderTarget"); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { Unexpected("SetDepthStencilSurface"); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { Unexpected("GetDepthStencilSurface"); }
    HRESULT STDMETHODCALLTYPE BeginScene() override { Unexpected("BeginScene"); }
    HRESULT STDMETHODCALLTYPE EndScene() override { Unexpected("EndScene"); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil) override { Unexpected("Clear"); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix) override { Unexpected("SetTransform"); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,D3DMATRIX* pMatrix) override { Unexpected("GetTransform"); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE,CONST D3DMATRIX*) override { Unexpected("MultiplyTransform"); }
    HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9* pViewport) override { Unexpected("SetViewport"); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { Unexpected("GetViewport"); }
    HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9* pMaterial) override { Unexpected("SetMaterial"); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { Unexpected("GetMaterial"); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index,CONST D3DLIGHT9*) override { Unexpected("SetLight"); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index,D3DLIGHT9*) override { Unexpected("GetLight"); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index,BOOL Enable) override { Unexpected("LightEnable"); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index,BOOL* pEnable) override { Unexpected("GetLightEnable"); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index,CONST float* pPlane) override { Unexpected("SetClipPlane"); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index,float* pPlane) override { Unexpected("GetClipPlane"); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,DWORD Value) override { Unexpected("SetRenderState"); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,DWORD* pValue) override { Unexpected("GetRenderState"); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type,IDirect3DStateBlock9** ppSB) override { Unexpected("CreateStateBlock"); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { Unexpected("BeginStateBlock"); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { Unexpected("EndStateBlock"); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) override { Unexpected("SetClipStatus"); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { Unexpected("GetClipStatus"); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage,IDirect3DBaseTexture9** ppTexture) override { Unexpected("GetTexture"); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage,IDirect3DBaseTexture9* pTexture) override { Unexpected("SetTexture"); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD* pValue) override { Unexpected("GetTextureStageState"); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value) override { Unexpected("SetTextureStageState"); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD* pValue) override { Unexpected("GetSamplerState"); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD Value) override { Unexpected("SetSamplerState"); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { Unexpected("ValidateDevice"); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber,CONST PALETTEENTRY* pEntries) override { Unexpected("SetPaletteEntries"); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,PALETTEENTRY* pEntries) override { Unexpected("GetPaletteEntries"); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { Unexpected("SetCurrentTexturePalette"); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { Unexpected("GetCurrentTexturePalette"); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT* pRect) override { Unexpected("SetScissorRect"); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { Unexpected("GetScissorRect"); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { Unexpected("SetSoftwareVertexProcessing"); }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { Unexpected("GetSoftwareVertexProcessing"); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { Unexpected("SetNPatchMode"); }
    float STDMETHODCALLTYPE GetNPatchMode() override { Unexpected("GetNPatchMode"); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount) override { Unexpected("DrawPrimitive"); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount) override { Unexpected("DrawIndexedPrimitive"); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride) override { Unexpected("DrawPrimitiveUP"); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride) override { Unexpected("DrawIndexedPrimitiveUP"); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer9* pDestBuffer,IDirect3DVertexDeclaration9* pVertexDecl,DWORD Flags) override { Unexpected("ProcessVertices"); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements,IDirect3DVertexDeclaration9** ppDecl) override { Unexpected("CreateVertexDeclaration"); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { Unexpected("SetVertexDeclaration"); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { Unexpected("GetVertexDeclaration"); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { Unexpected("SetFVF"); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { Unexpected("GetFVF"); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader) override { Unexpected("CreateVertexShader"); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override { Unexpected("SetVertexShader"); }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { Unexpected("GetVertexShader"); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) override { Unexpected("SetVertexShaderConstantF"); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister,float* pConstantData,UINT Vector4fCount) override { Unexpected("GetVertexShaderConstantF"); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount) override { Unexpected("SetVertexShaderConstantI"); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister,int* pConstantData,UINT Vector4iCount) override { Unexpected("GetVertexShaderConstantI"); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister,CONST BOOL* pConstantData,UINT  BoolCount) override { Unexpected("SetVertexShaderConstantB"); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister,BOOL* pConstantData,UINT BoolCount) override { Unexpected("GetVertexShaderConstantB"); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride) override { Unexpected("SetStreamSource"); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer9** ppStreamData,UINT* pOffsetInBytes,UINT* pStride) override { Unexpected("GetStreamSource"); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber,UINT Setting) override { Unexpected("SetStreamSourceFreq"); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber,UINT* pSetting) override { Unexpected("GetStreamSourceFreq"); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { Unexpected("SetIndices"); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { Unexpected("GetIndices"); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader) override { Unexpected("CreatePixelShader"); }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override { Unexpected("SetPixelShader"); }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { Unexpected("GetPixelShader"); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount) override { Unexpected("SetPixelShaderConstantF"); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister,float* pConstantData,UINT Vector4fCount) override { Unexpected("GetPixelShaderConstantF"); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount) override { Unexpected("SetPixelShaderConstantI"); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister,int* pConstantData,UINT Vector4iCount) override { Unexpected("GetPixelShaderConstantI"); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister,CONST BOOL* pConstantData,UINT  BoolCount) override { Unexpected("SetPixelShaderConstantB"); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister,BOOL* pConstantData,UINT BoolCount) override { Unexpected("GetPixelShaderConstantB"); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo) override { Unexpected("DrawRectPatch"); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo) override { Unexpected("DrawTriPatch"); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { Unexpected("DeletePatch"); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type,IDirect3DQuery9** ppQuery) override { Unexpected("CreateQuery"); }
};

// Only actual CPU Lock/Unlock is supported. The buffer is a live typed COM
// object with owned memory, never a fabricated interface pointer or vtable.
struct CpuBuffer final : IDirect3DVertexBuffer9 {
    std::vector<float> values;
    unsigned locks = 0, unlocks = 0;
    bool locked = false;
    explicit CpuBuffer(unsigned floats) : values(floats, -999.f) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { if (out) *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { Unexpected("buffer GetDevice"); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { Unexpected("buffer SetPrivateData"); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { Unexpected("buffer GetPrivateData"); }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { Unexpected("buffer FreePrivateData"); }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { Unexpected("buffer SetPriority"); }
    DWORD STDMETHODCALLTYPE GetPriority() override { Unexpected("buffer GetPriority"); }
    void STDMETHODCALLTYPE PreLoad() override { Unexpected("buffer PreLoad"); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void** out, DWORD flags) override {
        Check(!locked && out && offset == 0 && size == values.size() * sizeof(float) && flags == 0,
            "real CPU worker locks the complete owned vertex buffer");
        locked = true; ++locks; *out = values.data(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        Check(locked, "real CPU worker unlocks only its locked buffer");
        locked = false; ++unlocks; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC*) override { Unexpected("buffer GetDesc"); }
};
static_assert(!std::is_abstract<RejectDevice>::value && !std::is_abstract<CpuBuffer>::value,
    "storage probes must implement complete typed COM interfaces");
std::vector<unsigned char> Bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(bool(in), "read an existing owned evidence file");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Check(!in.bad(), "read complete evidence file bytes"); return bytes;
}
void NewFile(const fs::path& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create only a previously absent fixture file");
    DWORD written = 0; const auto ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    const auto closed = CloseHandle(file);
    Check(ok && written == bytes.size() && closed, "write complete fixture bytes");
}
fs::path InstallDirectory() {
    wchar_t name[32768]{}; const auto size = GetModuleFileNameW(nullptr, name, 32768);
    Check(size && size < 32768, "identify the actual copied executable");
    return fs::path(name).parent_path();
}
struct EnvironmentValue {
    const wchar_t* name;
    std::wstring value;
    bool existed = false;
    explicit EnvironmentValue(const wchar_t* key) : name(key) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size) {
            value.resize(size);
            Check(GetEnvironmentVariableW(name, value.data(), size) == size - 1, "capture own inherited environment");
            value.resize(size - 1); existed = true;
        }
    }
    void Set(const std::wstring& text) { Check(_wputenv_s(name, text.c_str()) == 0, "set only this process's environment"); }
    ~EnvironmentValue() { Check(_wputenv_s(name, existed ? value.c_str() : L"") == 0, "restore own inherited environment"); }
};
struct Environment {
    fs::path original = fs::current_path();
    fs::path cwd, temp, local;
    EnvironmentValue tempValue{L"TEMP"}, tmpValue{L"TMP"}, localValue{L"LOCALAPPDATA"}, workers{L"OMP_NUM_THREADS"};
    explicit Environment(const fs::path& out) : cwd(out / L"foreign-cwd"), temp(out / L"foreign-temp"), local(out / L"legacy-profile") {
        Check(fs::create_directory(cwd) && fs::create_directory(temp) && fs::create_directory(local), "create new isolated process directories");
        tempValue.Set(temp.wstring()); tmpValue.Set(temp.wstring()); localValue.Set(local.wstring()); workers.Set(L"1");
        Check(SetCurrentDirectoryW(cwd.c_str()) != FALSE, "use an independent cwd");
        wchar_t actual[32768]{}; auto size = GetTempPathW(32768, actual);
        Check(size && size < 32768 && fs::equivalent(actual, temp), "Win32 TEMP resolves only to owned test storage");
        size = GetEnvironmentVariableW(L"LOCALAPPDATA", actual, 32768);
        Check(size && size < 32768 && fs::equivalent(actual, local), "legacy lookup can see only the owned fake profile");
    }
    ~Environment() { Check(SetCurrentDirectoryW(original.c_str()) != FALSE, "restore own initial cwd"); }
};
std::vector<skin::StartupMeasurement> Profiles(double base) {
    std::vector<skin::StartupMeasurement> profiles;
    for (bool pmx : {false, true}) for (unsigned count : {1024u, 32768u, 262144u}) {
        skin::StartupMeasurement sample; sample.pmx = pmx; sample.vertexCount = count;
        sample.report.measurements.push_back({0, base + profiles.size(), true});
        sample.report.selectedAdapterId = 0; profiles.push_back(std::move(sample));
    }
    return profiles;
}
void SameProfiles(const std::vector<skin::StartupMeasurement>& actual, const std::vector<skin::StartupMeasurement>& expected) {
    Check(actual.size() == expected.size(), "startup loaded exactly the expected profile count");
    for (size_t i = 0; i < actual.size(); ++i) {
        Check(actual[i].pmx == expected[i].pmx && actual[i].vertexCount == expected[i].vertexCount &&
            actual[i].report.selectedAdapterId == 0 && actual[i].report.measurements.size() == 1,
            "startup loaded the CPU-only PMD/PMX profile structure");
        const auto& value = actual[i].report.measurements[0];
        Check(value.valid && value.adapterId == 0 && value.milliseconds == expected[i].report.measurements[0].milliseconds,
            "startup used this installation's exact measurements rather than legacy timings");
    }
}
void Reset(RejectDevice& device) {
    skin::Shutdown(); skin::SetPreference({});
    // Same-TU access only replaces the unavailable hardware inventory. All
    // cache loading, startup control flow, and failure handling stay real.
    skin::adapters.clear(); skin::enumerated = true;
    device.allocations = 0;
    Check(skin::AvailableAdapters().empty() && skin::startupDevices.empty(), "controlled adapter inventory contains no GPU");
}
void CpuFallback(MMDApp& app, bool cacheHit) {
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{}; mdl::ModelMaterialRecord material{}; mdl::PmdVertex vertex{};
    CpuBuffer main(8), edge(4);
    float world[16]{}; world[0] = world[5] = world[10] = world[15] = 1.f;
    std::memcpy(bone.matInit, world, sizeof world);
    bone.matInit[12] = 7.f; bone.matInit[13] = 8.f; bone.matInit[14] = 9.f;
    vertex.position[0] = 1.f; vertex.position[1] = 2.f; vertex.position[2] = 3.f;
    vertex.normal[1] = 1.f; vertex.weightPercent = 100; vertex.edgeDisabled = 1;
    vertex.bone[0] = vertex.bone[1] = 0;
    model->loadComplete = 1; model->vertexCount = model->boneCount = model->materialCount = 1;
    model->physicsMode = 0; model->edgeScale = 1.f;
    model->boneTable = &bone; model->materials = &material; model->rawVertices = &vertex;
    model->vertexBuffer = &main; model->vertexBuffer2 = &edge;
    app.CameraDistance() = 10.f; app.CameraFov() = 30.f;
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    const auto fallbackBefore = skin::GetStatus().cpuFallbacks;
    const auto cpuBefore = skin::GetStatus().cpuUpdates;
    mikumikustudio::UpdateModelVertexBuffers(&app, raw, world);
    Check(main.locks == 1 && main.unlocks == 1 && edge.locks == 1 && edge.unlocks == 1,
        "real model update reached the CPU worker and released both owned buffers");
    Check(std::abs(main.values[0] - 8.f) < .0001f && std::abs(main.values[1] - 10.f) < .0001f &&
        std::abs(main.values[2] - 12.f) < .0001f && std::abs(main.values[4] - 1.f) < .0001f,
        "real CPU fallback writes the known translated position and normal");
    const auto& status = skin::GetStatus();
    Check(status.gpuUpdates == 0 && status.automaticCalibrations == 0, "CPU update does not start GPU work or a new benchmark");
    if (cacheHit) Check(status.result == skin::Result::Cpu && status.cpuUpdates == cpuBefore + 1 && status.cpuFallbacks == fallbackBefore,
        "a valid CPU profile selects CPU directly");
    else Check(status.result == skin::Result::Unavailable && status.cpuFallbacks == fallbackBefore + 1,
        "missing startup measurements use the real unavailable-to-CPU fallback");
    skin::ForgetModel(raw); mikumikustudio::model_vertex_cache::Forget(raw);
}
void Miss(MMDApp& app, RejectDevice& device) {
    Reset(device); skin::InitializeAutomatic(&app, &device);
    Check(device.allocations == 1 && skin::startupAttempted && skin::GetStartupMeasurements().empty(),
        "cache miss reaches and handles the real startup VB allocation failure without importing legacy measurements");
    Check(skin::startupDevices.empty() && skin::GetStatus().automaticCalibrations == 0,
        "cache miss starts no GPU device and records no successful benchmark");
    skin::InitializeAutomatic(&app, &device);
    Check(device.allocations == 1 && skin::GetStartupMeasurements().empty(), "repeated initialization does not retry the failed startup benchmark");
    CpuFallback(app, false);
}
void NoFallback(const Environment& environment) {
    Check(fs::is_empty(environment.cwd) && fs::is_empty(environment.temp), "calibration never writes cwd or TEMP fallback files");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3 || wcscmp(argv[1], L"--storage") != 0) {
        std::puts("Use --storage <absolute fresh output directory> in a marked copied installation."); return 2;
    }
    const auto install = InstallDirectory();
    const auto marker = Bytes(install / L"calibration-test-installation.txt");
    Check(std::string(marker.begin(), marker.end()) == "MMS calibration test installation v1\n", "require explicit copied-installation marker");
    const auto data = install / L"data", local = data / L"processor-config-MMS-v1.bin";
    Check(!fs::exists(data), "copied installation must have no preexisting data directory");
    const fs::path supplied(argv[2]);
    Check(supplied.is_absolute(), "storage output must be an absolute path");
    const auto out = supplied.lexically_normal();
    Check(!fs::exists(out) && fs::is_directory(out.parent_path()) && out != install && out != data,
        "output must be a fresh directory separate from installation data");
    Check(fs::create_directory(out), "create only new evidence output");
    Environment environment(out);
    Check(!fs::equivalent(environment.cwd, install) && !fs::equivalent(environment.temp, install), "runtime directories differ from installation");
    const auto legacy = environment.local / L"MikuMikuStudio" / L"skinning-MMS-v1.bin";
    const auto cpu = cache::CpuKey();
    const auto legacyProfiles = Profiles(100.), localProfiles = Profiles(10.);
    Check(cache::Save(legacy, cpu, {}, legacyProfiles), "write a valid owned legacy-profile witness");
    const auto legacyBytes = Bytes(legacy);
    std::vector<skin::StartupMeasurement> loaded;
    Check(cache::Load(legacy, cpu, {}, loaded), "legacy witness is actually valid for the current CPU and controlled inventory");
    SameProfiles(loaded, legacyProfiles);
    auto app = std::make_unique<MMDApp>();
    new (&app->state) MMDAppState{};
    RejectDevice device;
    Reset(device);
    skin::InitializeAutomatic(nullptr, &device); skin::InitializeAutomatic(app.get(), nullptr);
    Check(!skin::startupAttempted && device.allocations == 0 && skin::GetStartupMeasurements().empty(), "null arguments do not consume startup or load any cache");
    skin::SetPreference({skin::Mode::Cpu}); skin::InitializeAutomatic(app.get(), &device);
    Check(!skin::startupAttempted && device.allocations == 0 && skin::GetStartupMeasurements().empty(), "explicit CPU preference does not calibrate or import cache");
    Check(!fs::exists(data) && Bytes(legacy) == legacyBytes, "startup guards leave installation and legacy witness unchanged");

    Miss(*app, device);
    Check(!fs::exists(local) && !fs::exists(data) && Bytes(legacy) == legacyBytes, "missing local cache never migrates the valid legacy witness");
    Check(cache::Path(L"MMS") == local, "production cache path uses this executable's lowercase data directory");
    NoFallback(environment);

    const std::string obstruction = "MMS owned calibration data obstruction\n";
    NewFile(data, obstruction);
    Miss(*app, device);
    const auto blocked = Bytes(data);
    Check(std::string(blocked.begin(), blocked.end()) == obstruction && Bytes(legacy) == legacyBytes,
        "blocked local storage preserves obstruction and does not borrow legacy measurements");
    Check(DeleteFileW(data.c_str()) != FALSE, "remove only the verified owned obstruction file");
    NoFallback(environment);

    Check(fs::create_directory(data), "create new installation data for the corrupt-cache case");
    NewFile(local, "MMS deliberately truncated calibration\n");
    const auto corrupt = Bytes(local);
    Miss(*app, device);
    Check(Bytes(local) == corrupt && Bytes(legacy) == legacyBytes, "corrupt local cache remains untouched on failed calibration without legacy replacement");
    NoFallback(environment);
    Check(DeleteFileW(local.c_str()) != FALSE, "remove only the verified owned corrupt fixture");

    Check(cache::Save(local, cpu, {}, localProfiles), "write the distinct valid installation-local fixture");
    const auto localBytes = Bytes(local);
    Reset(device); skin::InitializeAutomatic(app.get(), &device);
    Check(skin::startupAttempted && device.allocations == 0 && skin::GetStatus().automaticCalibrations == 0,
        "valid local startup cache loads without any calibration buffer or benchmark");
    SameProfiles(skin::GetStartupMeasurements(), localProfiles);
    CpuFallback(*app, true);
    skin::InitializeAutomatic(app.get(), &device);
    Check(device.allocations == 0, "repeat initialization after a cache hit starts no calibration");
    SameProfiles(skin::GetStartupMeasurements(), localProfiles);
    Check(Bytes(local) == localBytes && Bytes(legacy) == legacyBytes, "hit and repeat initialization preserve both exact cache files");
    NoFallback(environment);
    skin::Shutdown();
    std::printf("install=%ls\nlocal=%ls\nlegacy_witness=%ls\n", install.c_str(), local.c_str(), legacy.c_str());
    std::printf("PASS: %u checks; real startup cache selection, failed allocation, CPU fallback and isolated data; no GPU enumeration or hardware benchmark\n", checks);
    return 0;
}
