#pragma once

#include "mikumikustudio/d3dx_dyn.hpp"

namespace mikumikustudio::d3dx {

// ID3DXEffect slots from d3dx9effect.h (June 2010 SDK). COM slot indices
// are identical on x86/x64; byte offsets from the x86 binary are not.
template <typename Fn, unsigned Slot>
Fn EffectMethod(void* effect) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(effect))[Slot]);
}

inline HRESULT FxSetTechnique(void* effect, const char* name) {
    using Fn = HRESULT(WINAPI*)(void*, const char*);
    return EffectMethod<Fn, 58>(effect)(effect, name);
}

inline HRESULT FxSetInt(void* effect, const char* name, int value) {
    using Fn = HRESULT(WINAPI*)(void*, const char*, int);
    return EffectMethod<Fn, 26>(effect)(effect, name, value);
}

inline HRESULT FxSetFloatArray(void* effect, const char* name,
                               const float* values, UINT count) {
    using Fn = HRESULT(WINAPI*)(void*, const char*, const float*, UINT);
    return EffectMethod<Fn, 32>(effect)(effect, name, values, count);
}

inline HRESULT FxSetMatrix(void* effect, const char* name,
                           const D3DXMATRIXF* value) {
    using Fn = HRESULT(WINAPI*)(void*, const char*, const D3DXMATRIXF*);
    return EffectMethod<Fn, 38>(effect)(effect, name, value);
}

inline HRESULT FxSetMatrixTranspose(void* effect, const char* name,
                                    const D3DXMATRIXF* value) {
    using Fn = HRESULT(WINAPI*)(void*, const char*, const D3DXMATRIXF*);
    return EffectMethod<Fn, 44>(effect)(effect, name, value);
}

inline HRESULT FxBegin(void* effect, UINT* passes) {
    using Fn = HRESULT(WINAPI*)(void*, UINT*, DWORD);
    return EffectMethod<Fn, 63>(effect)(effect, passes, 0);
}

inline HRESULT FxBeginPass(void* effect, UINT pass) {
    using Fn = HRESULT(WINAPI*)(void*, UINT);
    return EffectMethod<Fn, 64>(effect)(effect, pass);
}

inline HRESULT FxCommit(void* effect) {
    using Fn = HRESULT(WINAPI*)(void*);
    return EffectMethod<Fn, 65>(effect)(effect);
}

inline HRESULT FxEndPass(void* effect) {
    using Fn = HRESULT(WINAPI*)(void*);
    return EffectMethod<Fn, 66>(effect)(effect);
}

inline HRESULT FxEnd(void* effect) {
    using Fn = HRESULT(WINAPI*)(void*);
    return EffectMethod<Fn, 67>(effect)(effect);
}

inline HRESULT FxOnLostDevice(void* effect) {
    using Fn = HRESULT(WINAPI*)(void*);
    return EffectMethod<Fn, 69>(effect)(effect);
}

inline HRESULT FxOnResetDevice(void* effect) {
    using Fn = HRESULT(WINAPI*)(void*);
    return EffectMethod<Fn, 70>(effect)(effect);
}

}  // namespace mikumikustudio::d3dx
