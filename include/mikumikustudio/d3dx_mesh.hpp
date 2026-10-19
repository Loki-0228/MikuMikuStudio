#pragma once
#include <d3d9.h>

namespace mikumikustudio::d3dx {
// ID3DXBaseMesh ABI, from d3dx9mesh.h. Slots are architecture independent.
// 7 = GetDeclaration (writes an ARRAY), 10 = GetDevice, 13/14 = VB/IB.
// Reference: https://github.com/wine-mirror/wine/blob/master/include/d3dx9mesh.h
template<class Fn, unsigned Slot> Fn MeshMethod(void* mesh) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(mesh))[Slot]);
}
inline DWORD MeshFvf(void* mesh) {
    return MeshMethod<DWORD(WINAPI*)(void*), 6>(mesh)(mesh);
}
inline DWORD MeshOptions(void* mesh) {
    return MeshMethod<DWORD(WINAPI*)(void*), 9>(mesh)(mesh);
}
inline HRESULT MeshDevice(void* mesh, IDirect3DDevice9** device) {
    return MeshMethod<HRESULT(WINAPI*)(void*, IDirect3DDevice9**), 10>(mesh)(mesh, device);
}
inline HRESULT MeshVertexBuffer(void* mesh, IDirect3DVertexBuffer9** buffer) {
    return MeshMethod<HRESULT(WINAPI*)(void*, IDirect3DVertexBuffer9**), 13>(mesh)(mesh, buffer);
}
inline HRESULT MeshIndexBuffer(void* mesh, IDirect3DIndexBuffer9** buffer) {
    return MeshMethod<HRESULT(WINAPI*)(void*, IDirect3DIndexBuffer9**), 14>(mesh)(mesh, buffer);
}
inline HRESULT CloneMeshFvf(void* mesh, DWORD options, DWORD fvf,
                            IDirect3DDevice9* device, void** clone) {
    return MeshMethod<HRESULT(WINAPI*)(void*, DWORD, DWORD, IDirect3DDevice9*, void**), 11>(mesh)(
        mesh, options, fvf, device, clone);
}
}
