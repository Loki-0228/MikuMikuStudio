// ===========================================================================
// d3dx9_43.dll entry points (shared)
// ===========================================================================
// The original imports d3dx9_32.dll (x86) / d3dx9_43.dll (x64); the port
// resolves the exact entry points at runtime and needs no build-time D3DX
// dependency.  On x64 the entry points are additionally declared as static
// imports (res/imports/d3dx9_43.def, MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS):
// MikuMikuEffect's MMHack hooks the host by rewriting the host's import
// address table, so the shipped exe must carry real IAT slots for them.
// =========================================================================//
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdint>
#include <type_traits>

namespace mikumikustudio::d3dx {

struct D3DXMATRIXF { float m[4][4]; };

using FnCreateTexInMemEx = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
    D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**);
using FnCreateTexFromFileExA = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**);
using FnCreateTexFromFileExW = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCWSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**);
using FnCreateTexture = HRESULT(WINAPI*)(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DTexture9**);
using FnCreateEffectFromResA = HRESULT(WINAPI*)(
    IDirect3DDevice9*, HMODULE, LPCSTR, const void*, const void*, DWORD,
    void*, void**, void*);
using FnCreateEffect = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCVOID, UINT, const void*, const void*, DWORD,
    void*, void**, void**);
using FnMatrixPerspectiveFovLH = D3DXMATRIXF*(WINAPI*)(
    D3DXMATRIXF*, float, float, float, float);
using FnMatrixOp1 = D3DXMATRIXF*(WINAPI*)(D3DXMATRIXF*, float);
using FnMatrixOp2 = D3DXMATRIXF*(WINAPI*)(
    D3DXMATRIXF*, const D3DXMATRIXF*, const D3DXMATRIXF*);
using FnMatrixTranslation = D3DXMATRIXF*(WINAPI*)(
    D3DXMATRIXF*, float, float, float);
using FnMatrixLookAtLH = D3DXMATRIXF*(WINAPI*)(
    D3DXMATRIXF*, const float*, const float*, const float*);
using FnMatrixInverse = D3DXMATRIXF*(WINAPI*)(
    D3DXMATRIXF*, float*, const D3DXMATRIXF*);
using FnVec3Transform = float*(WINAPI*)(
    float out[4], const float src[3], const D3DXMATRIXF*);
using FnLoadMeshFromXInMemory = HRESULT(WINAPI*)(
    LPCVOID, DWORD, DWORD, IDirect3DDevice9*, void**, void**, void**, DWORD*,
    void**);
using FnLoadMeshFromXW = HRESULT(WINAPI*)(
    LPCWSTR, DWORD, IDirect3DDevice9*, void**, void**, void**, DWORD*, void**);
using FnComputeNormals = HRESULT(WINAPI*)(void*, const DWORD*);
using FnVecQuat = float*(WINAPI*)(float out[4], const D3DXMATRIXF* m);
using FnVec3Normalize = float*(WINAPI*)(float out[3], const float* src);
using FnQuatMultiply = float*(WINAPI*)(float out[4], const float* q1,
                                       const float* q2);
using FnMatrixQuat = D3DXMATRIXF*(WINAPI*)(D3DXMATRIXF* out,
                                           const float q[4]);
using FnQuatToAxisAngle = float*(WINAPI*)(const float q[4],
                                             float axis[3], float* angle);
using FnQuatNormalize = float*(WINAPI*)(float out[4], const float q[4]);
using FnSaveSurfaceToFileW = HRESULT(WINAPI*)(
    const wchar_t*, int /*D3DXIMAGE_FILEFORMAT*/, IDirect3DSurface9*,
    const void* /*PALETTEENTRY*/, const RECT*);

#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
// Imported entry points of d3dx9_43.dll (res/imports/d3dx9_43.def).  They must
// be declared as FUNCTIONS: declaring them as function-pointer objects makes
// MSVC apply data-import semantics (an extra dereference through the slot),
// which reads the function's first bytes instead of its address.  The
// static_asserts below pin every prototype to the typedef it is used with.
extern "C" {
__declspec(dllimport) HRESULT WINAPI D3DXCreateTextureFromFileInMemoryEx(
    IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
    D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**);
__declspec(dllimport) HRESULT WINAPI D3DXCreateTextureFromFileExA(
    IDirect3DDevice9*, LPCSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**);
__declspec(dllimport) HRESULT WINAPI D3DXCreateTextureFromFileExW(
    IDirect3DDevice9*, LPCWSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**);
__declspec(dllimport) HRESULT WINAPI D3DXCreateTexture(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DTexture9**);
__declspec(dllimport) HRESULT WINAPI D3DXCreateEffectFromResourceA(
    IDirect3DDevice9*, HMODULE, LPCSTR, const void*, const void*, DWORD,
    void*, void**, void*);
__declspec(dllimport) HRESULT WINAPI D3DXCreateEffect(
    IDirect3DDevice9*, LPCVOID, UINT, const void*, const void*, DWORD,
    void*, void**, void**);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixPerspectiveFovLH(
    D3DXMATRIXF*, float, float, float, float);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixRotationX(
    D3DXMATRIXF*, float);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixRotationY(
    D3DXMATRIXF*, float);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixRotationZ(
    D3DXMATRIXF*, float);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixMultiply(
    D3DXMATRIXF*, const D3DXMATRIXF*, const D3DXMATRIXF*);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixTranslation(
    D3DXMATRIXF*, float, float, float);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixScaling(
    D3DXMATRIXF*, float, float, float);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixLookAtLH(
    D3DXMATRIXF*, const float*, const float*, const float*);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixInverse(
    D3DXMATRIXF*, float*, const D3DXMATRIXF*);
__declspec(dllimport) float* WINAPI D3DXVec3Transform(
    float out[4], const float src[3], const D3DXMATRIXF*);
__declspec(dllimport) HRESULT WINAPI D3DXLoadMeshFromXInMemory(
    LPCVOID, DWORD, DWORD, IDirect3DDevice9*, void**, void**, void**, DWORD*,
    void**);
__declspec(dllimport) HRESULT WINAPI D3DXLoadMeshFromXW(
    LPCWSTR, DWORD, IDirect3DDevice9*, void**, void**, void**, DWORD*, void**);
__declspec(dllimport) HRESULT WINAPI D3DXComputeNormals(void*, const DWORD*);
__declspec(dllimport) float* WINAPI D3DXQuaternionRotationMatrix(
    float out[4], const D3DXMATRIXF*);
__declspec(dllimport) float* WINAPI D3DXVec3Normalize(
    float out[3], const float*);
__declspec(dllimport) float* WINAPI D3DXQuaternionMultiply(
    float out[4], const float*, const float*);
__declspec(dllimport) D3DXMATRIXF* WINAPI D3DXMatrixRotationQuaternion(
    D3DXMATRIXF*, const float q[4]);
__declspec(dllimport) float* WINAPI D3DXQuaternionToAxisAngle(
    const float q[4], float axis[3], float* angle);
__declspec(dllimport) float* WINAPI D3DXQuaternionNormalize(
    float out[4], const float q[4]);
__declspec(dllimport) HRESULT WINAPI D3DXSaveSurfaceToFileW(
    const wchar_t*, int, IDirect3DSurface9*, const void*, const RECT*);
}

static_assert(std::is_same<decltype(&D3DXCreateTextureFromFileInMemoryEx),
                           FnCreateTexInMemEx>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXCreateTextureFromFileExA),
                           FnCreateTexFromFileExA>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXCreateTextureFromFileExW),
                           FnCreateTexFromFileExW>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXCreateTexture), FnCreateTexture>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXCreateEffectFromResourceA),
                           FnCreateEffectFromResA>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXCreateEffect),
                           FnCreateEffect>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixPerspectiveFovLH),
                           FnMatrixPerspectiveFovLH>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixRotationX), FnMatrixOp1>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixMultiply), FnMatrixOp2>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixTranslation),
                           FnMatrixTranslation>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixLookAtLH),
                           FnMatrixLookAtLH>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixInverse), FnMatrixInverse>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXVec3Transform), FnVec3Transform>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXLoadMeshFromXInMemory),
                           FnLoadMeshFromXInMemory>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXLoadMeshFromXW), FnLoadMeshFromXW>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXComputeNormals), FnComputeNormals>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXQuaternionRotationMatrix),
                           FnVecQuat>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXVec3Normalize), FnVec3Normalize>::value,
              "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXQuaternionMultiply),
                           FnQuatMultiply>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXMatrixRotationQuaternion),
                           FnMatrixQuat>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXQuaternionToAxisAngle),
                           FnQuatToAxisAngle>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXQuaternionNormalize),
                           FnQuatNormalize>::value, "d3dx import signature");
static_assert(std::is_same<decltype(&D3DXSaveSurfaceToFileW),
                           FnSaveSurfaceToFileW>::value, "d3dx import signature");
#endif

struct Api {
    HMODULE module = nullptr;
    FnCreateTexInMemEx fromMemEx = nullptr;
    FnCreateTexFromFileExA fromFileExA = nullptr;
    FnCreateTexFromFileExW fromFileExW = nullptr;
    FnCreateTexture createTexture = nullptr;
    FnCreateEffectFromResA createEffectFromResA = nullptr;
    FnCreateEffect createEffect = nullptr;
    FnMatrixPerspectiveFovLH perspectiveFovLH = nullptr;
    FnMatrixOp1 rotX = nullptr;
    FnMatrixOp1 rotY = nullptr;
    FnMatrixOp1 rotZ = nullptr;
    FnMatrixOp2 multiply = nullptr;
    FnMatrixTranslation translation = nullptr;
    FnMatrixTranslation scaling = nullptr;
    FnMatrixLookAtLH lookAtLH = nullptr;
    FnMatrixInverse inverse = nullptr;
    FnVec3Transform vec3Transform = nullptr;
    FnLoadMeshFromXInMemory loadMeshFromXInMemory = nullptr;
    FnLoadMeshFromXW loadMeshFromXW = nullptr;
    FnComputeNormals computeNormals = nullptr;
    FnVecQuat quatFromMatrix = nullptr;
    FnVec3Normalize vec3Normalize = nullptr;
    FnQuatMultiply quatMultiply = nullptr;
    FnMatrixQuat matrixRotationQuaternion = nullptr;
    FnQuatToAxisAngle quatToAxisAngle = nullptr;
    FnQuatNormalize quatNormalize = nullptr;
    FnSaveSurfaceToFileW saveSurfaceToFileW = nullptr;

    bool Load() {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
        // The entry points are static imports (the exe's IAT carries them so
        // that MME can hook them).  The slots are re-read on every call: MME
        // installs its hooks into exactly these slots right after the first
        // Direct3DCreate9, so caching the raw addresses too early would
        // bypass the effect system.
        // Static imports keep this DLL loaded for the process lifetime.
        // Cache only its module handle, not the IAT entry points below: those
        // must still observe hooks installed after Direct3DCreate9.
        if (module == nullptr)
            module = GetModuleHandleA("d3dx9_43.dll");
        if (module == nullptr)
            return false;
        fromMemEx = D3DXCreateTextureFromFileInMemoryEx;
        fromFileExA = D3DXCreateTextureFromFileExA;
        fromFileExW = D3DXCreateTextureFromFileExW;
        createTexture = D3DXCreateTexture;
        createEffectFromResA = D3DXCreateEffectFromResourceA;
        createEffect = D3DXCreateEffect;
        perspectiveFovLH = D3DXMatrixPerspectiveFovLH;
        rotX = D3DXMatrixRotationX;
        rotY = D3DXMatrixRotationY;
        rotZ = D3DXMatrixRotationZ;
        multiply = D3DXMatrixMultiply;
        translation = D3DXMatrixTranslation;
        scaling = D3DXMatrixScaling;
        lookAtLH = D3DXMatrixLookAtLH;
        inverse = D3DXMatrixInverse;
        vec3Transform = D3DXVec3Transform;
        loadMeshFromXInMemory = D3DXLoadMeshFromXInMemory;
        loadMeshFromXW = D3DXLoadMeshFromXW;
        computeNormals = D3DXComputeNormals;
        quatFromMatrix = D3DXQuaternionRotationMatrix;
        vec3Normalize = D3DXVec3Normalize;
        quatMultiply = D3DXQuaternionMultiply;
        matrixRotationQuaternion = D3DXMatrixRotationQuaternion;
        quatToAxisAngle = D3DXQuaternionToAxisAngle;
        quatNormalize = D3DXQuaternionNormalize;
        saveSurfaceToFileW = D3DXSaveSurfaceToFileW;
        return fromMemEx && fromFileExA && fromFileExW && createTexture &&
               createEffectFromResA && perspectiveFovLH && rotX && rotY &&
               rotZ && multiply && translation && scaling && lookAtLH &&
               inverse && vec3Transform &&
               loadMeshFromXInMemory && loadMeshFromXW && computeNormals &&
               quatFromMatrix &&
               vec3Normalize && quatMultiply && matrixRotationQuaternion &&
               quatToAxisAngle && quatNormalize;
#else
        if (module != nullptr)
            return true;
        // original import name: x86 links d3dx9_32, the x64 rebuild d3dx9_43
        module = LoadLibraryA(sizeof(void*) == 8 ? "d3dx9_43.dll"
                                                 : "d3dx9_32.dll");
        if (module == nullptr)
            return false;
        fromMemEx = reinterpret_cast<FnCreateTexInMemEx>(GetProcAddress(
            module, "D3DXCreateTextureFromFileInMemoryEx"));
        fromFileExA = reinterpret_cast<FnCreateTexFromFileExA>(GetProcAddress(
            module, "D3DXCreateTextureFromFileExA"));
        fromFileExW = reinterpret_cast<FnCreateTexFromFileExW>(GetProcAddress(
            module, "D3DXCreateTextureFromFileExW"));
        createTexture = reinterpret_cast<FnCreateTexture>(GetProcAddress(
            module, "D3DXCreateTexture"));
        createEffectFromResA = reinterpret_cast<FnCreateEffectFromResA>(
            GetProcAddress(module, "D3DXCreateEffectFromResourceA"));
        createEffect = reinterpret_cast<FnCreateEffect>(
            GetProcAddress(module, "D3DXCreateEffect"));
        perspectiveFovLH = reinterpret_cast<FnMatrixPerspectiveFovLH>(
            GetProcAddress(module, "D3DXMatrixPerspectiveFovLH"));
        rotX = reinterpret_cast<FnMatrixOp1>(
            GetProcAddress(module, "D3DXMatrixRotationX"));
        rotY = reinterpret_cast<FnMatrixOp1>(
            GetProcAddress(module, "D3DXMatrixRotationY"));
        rotZ = reinterpret_cast<FnMatrixOp1>(
            GetProcAddress(module, "D3DXMatrixRotationZ"));
        multiply = reinterpret_cast<FnMatrixOp2>(
            GetProcAddress(module, "D3DXMatrixMultiply"));
        translation = reinterpret_cast<FnMatrixTranslation>(
            GetProcAddress(module, "D3DXMatrixTranslation"));
        scaling = reinterpret_cast<FnMatrixTranslation>(
            GetProcAddress(module, "D3DXMatrixScaling"));
        lookAtLH = reinterpret_cast<FnMatrixLookAtLH>(
            GetProcAddress(module, "D3DXMatrixLookAtLH"));
        inverse = reinterpret_cast<FnMatrixInverse>(
            GetProcAddress(module, "D3DXMatrixInverse"));
        vec3Transform = reinterpret_cast<FnVec3Transform>(
            GetProcAddress(module, "D3DXVec3Transform"));
        loadMeshFromXInMemory = reinterpret_cast<FnLoadMeshFromXInMemory>(
            GetProcAddress(module, "D3DXLoadMeshFromXInMemory"));
        loadMeshFromXW = reinterpret_cast<FnLoadMeshFromXW>(
            GetProcAddress(module, "D3DXLoadMeshFromXW"));
        computeNormals = reinterpret_cast<FnComputeNormals>(
            GetProcAddress(module, "D3DXComputeNormals"));
        quatFromMatrix = reinterpret_cast<FnVecQuat>(
            GetProcAddress(module, "D3DXQuaternionRotationMatrix"));
        vec3Normalize = reinterpret_cast<FnVec3Normalize>(
            GetProcAddress(module, "D3DXVec3Normalize"));
        quatMultiply = reinterpret_cast<FnQuatMultiply>(
            GetProcAddress(module, "D3DXQuaternionMultiply"));
        matrixRotationQuaternion = reinterpret_cast<FnMatrixQuat>(
            GetProcAddress(module, "D3DXMatrixRotationQuaternion"));
        quatToAxisAngle = reinterpret_cast<FnQuatToAxisAngle>(
            GetProcAddress(module, "D3DXQuaternionToAxisAngle"));
        quatNormalize = reinterpret_cast<FnQuatNormalize>(
            GetProcAddress(module, "D3DXQuaternionNormalize"));
        saveSurfaceToFileW = reinterpret_cast<FnSaveSurfaceToFileW>(
            GetProcAddress(module, "D3DXSaveSurfaceToFileW"));
        return fromMemEx && fromFileExA && fromFileExW && createTexture &&
               createEffectFromResA && perspectiveFovLH && rotX && rotY &&
               rotZ && multiply && translation && scaling && lookAtLH &&
               inverse && vec3Transform &&
               loadMeshFromXInMemory && loadMeshFromXW && computeNormals &&
               quatFromMatrix &&
               vec3Normalize && quatMultiply && matrixRotationQuaternion &&
               quatToAxisAngle && quatNormalize;
#endif
    }
};

inline Api& Get() {
    static Api api;
    return api;
}

}  // namespace mikumikustudio::d3dx
