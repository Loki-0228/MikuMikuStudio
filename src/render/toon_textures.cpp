// ===========================================================================
// VA 0x00424DC0 - InitToonTextures  (original: sub_424DC0, 0x2FA bytes)
// ===========================================================================
// Toon-shading gradient loader (NOT the font system - reclassified; the
// translated file's "font init" comment was wrong):
//   1. release the 11 texture slots at this+650720..+650760
//   2. texture slot 0: embedded PNG resource 0x67 via
//      D3DXCreateTextureFromFileInMemoryEx (A8R8G8B8, MANAGED pool)
//   3. edge-colour table at this+655632 (14 floats, exact values below)
//   4. slots 1..10: data\toon%02d.bmp via D3DXCreateTextureFromFileExA
//      (fmt 21 = A8R8G8B8, pool 1 = MANAGED); on failure fall back to the
//      embedded PNG resource (id+103); on success read the bottom-left
//      pixel via LockRect and store R/G/B * (1/256) into entries
//      [3*(N-1) .. 3*(N-1)+2] of the same table (stride three floats
//      per toon texture; x64 0x7FF7CB4BA522..0x7FF7CB4BA567).
//
// D3DX fidelity: the original specifically imports d3dx9_32.dll; the port
// obtains the two entry points from the shared loader in d3dx_dyn.hpp, which
// reports them from the exe's import slots.  MMHack rewrites those slots and
// registers every texture the host creates through them; the ten toon bitmaps
// are registered the same way in the original.  A private LoadLibrary plus
// GetProcAddress would bypass the rewrite, leaving MMHack without a toon
// texture to hand to the effect.
// Device: *(this+657092)+120032 (guard: null until the D3D init is ported).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../model/texture_reload_preparation.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <algorithm>
#include <climits>
#include <limits>
#include <new>
#include <string>
#endif

namespace mikumikustudio {
namespace {

// Layout binary-compatible with D3DXIMAGE_INFO. D3DX is resolved at runtime, so
// the legacy D3DX SDK header cannot be included directly.
struct D3dxImageInfo {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    UINT ImageFileFormat;
};

// D3DX entry points come from the shared loader in d3dx_dyn.hpp. It reads
// them directly from the exe's import slots on every call, which is what allows
// MMHack to register the toon bitmaps that this function creates; resolving them with
// a private LoadLibrary and GetProcAddress would bypass that rewrite.

IDirect3DDevice9* DeviceOf(MMDApp* app) {
    D3DRenderer* sub = app->Renderer();                           // 657092
    if (sub == nullptr)
        return nullptr;
    return sub->device;                                           // +0x1D4E0
}

}  // namespace

bool InitToonTextures(MMDApp* app) {
    auto& s = *app;
    IDirect3DDevice9* device = DeviceOf(app);
    if (device == nullptr)
        return false;   // the original fails in the resource-path call chain
    auto* d3dx = &d3dx::Get();

    SetCurrentDirectoryW(app->ExeDir());                          // 657102

    // release the 11 toon texture slots (vtable+8 = Release)
    for (int i = 0; i < 11; ++i) {
        IDirect3DTexture9* tex = s.ToonTexture(i);                // 650720..
        if (tex != nullptr) {
            static_cast<IUnknown*>(tex)->Release();
            s.ToonTexture(i) = nullptr;
        }
    }

    // slot 0: embedded PNG (resource 0x67, type "PNG")
    HRSRC res = FindResourceA(static_cast<HMODULE>(s.HInstance()),
                              MAKEINTRESOURCEA(0x67), "PNG");
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL glob = LoadResource(static_cast<HMODULE>(s.HInstance()), res);
    void* data = LockResource(glob);
    IDirect3DTexture9* tex0 = nullptr;
    if (!d3dx->Load())
        return false;
    if (FAILED(d3dx->fromMemEx(device, data, size,
                                static_cast<UINT>(-1), static_cast<UINT>(-1),
                                1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
                                static_cast<DWORD>(-1), static_cast<DWORD>(-1),
                                0, nullptr, nullptr, &tex0)))
        return false;
    s.ToonTexture(0) = tex0;

    // edge-colour table defaults (this+655632, exact original values)
    const float defaults[14] = {
        0.80078101f, 0.80078101f, 0.80078101f, 0.95703101f, 0.87890601f,
        0.87890601f, 0.60156298f, 0.60156298f, 0.60156298f, 0.96875f,
        0.93359399f, 0.91796899f, 1.0f, 0.90234399f,
    };
    for (int i = 0; i < 14; ++i)
        s.state.toonEdgeTable[i] = defaults[i];
    // Trailing table (dwords 0x28052..0x28061): [14..17] pull the four
    // rdata floats 0x52c0c0/c0bc/c0b8/c0b4 (0.863281, 0.761719, 0.671875,
    // 0.011719); [18..29] are all 1.0.
    const float tail[4] = {0.86328101f, 0.76171899f, 0.671875f,
                           0.011719f};
    for (int i = 0; i < 4; ++i)
        s.state.toonEdgeTable[14 + i] = tail[i];
    for (int i = 18; i < 30; ++i)
        s.state.toonEdgeTable[i] = 1.0f;

    // slots 1..10: toonNN.bmp with embedded PNG fallback, colour from pixel
    for (int i = 1; i < 11; ++i) {
        char path[256];
        sprintf_s(path, 0x100, "data\\toon%02d.bmp", i);
        IDirect3DTexture9** slot = &s.ToonTexture(i);
        D3dxImageInfo info{};
        if (!d3dx->Load())
            return false;
        if (FAILED(d3dx->fromFileExA(device, path,
                                      static_cast<UINT>(-1),
                                      static_cast<UINT>(-1), 1, 0,
                                      D3DFMT_A8R8G8B8 /*21*/,
                                      D3DPOOL_MANAGED /*1*/,
                                      static_cast<DWORD>(-1),
                                      static_cast<DWORD>(-1), 0,
                                      &info, nullptr, slot))) {
            HRSRC r = FindResourceA(static_cast<HMODULE>(s.HInstance()),
                                    MAKEINTRESOURCEA(i + 103), "PNG");
            DWORD sz = SizeofResource(nullptr, r);
            HGLOBAL g = LoadResource(static_cast<HMODULE>(s.HInstance()), r);
            void* d = LockResource(g);
            d3dx->fromMemEx(device, d, sz, static_cast<UINT>(-1),
                             static_cast<UINT>(-1), 1, 0, D3DFMT_UNKNOWN,
                             D3DPOOL_MANAGED, static_cast<DWORD>(-1),
                             static_cast<DWORD>(-1), 0, nullptr, nullptr,
                             slot);
        } else {
            // x64 sub_7FF7CB4BA130+0x3F2..0x437 (0x7FF7CB4BA522..0x7FF7CB4BA567):
            // the bottom-left pixel of toon%02d.bmp (A8R8G8B8 lock order
            // B,G,R,A) fills table entry 3*(N-1)..3*(N-1)+2 as R,G,B
            // (*entry = row[2]/256, entry[1] = row[1]/256, entry[2] =
            // row[0]/256); the entry pointer then advances by three floats
            // per texture (add r12,0Ch @0x7FF7CB4BA563).
            D3DLOCKED_RECT rect;
            if (SUCCEEDED((*slot)->LockRect(0, &rect, nullptr, 0))) {
                auto* row = static_cast<unsigned char*>(rect.pBits) +
                            rect.Pitch * (info.Height - 1);
                float* out = &s.state.toonEdgeTable[3 * (i - 1)];
                out[0] = row[2] * 0.00390625f;     // R
                out[1] = row[1] * 0.00390625f;     // G
                out[2] = row[0] * 0.00390625f;     // B
                (*slot)->UnlockRect(0);
            }
        }
    }
    return true;
}

#if !MMD_ORIGINAL_PROFILE
namespace texture_reload {
namespace {
bool FatalToonLoad(HRESULT r) noexcept {
    return r == E_OUTOFMEMORY || r == D3DERR_OUTOFVIDEOMEMORY || r == D3DERR_DEVICELOST ||
        r == D3DERR_DEVICENOTRESET || r == D3DERR_INVALIDCALL || r == D3DERR_NOTAVAILABLE ||
        r == E_NOINTERFACE || r == E_ABORT || r == E_UNEXPECTED ||
        r == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}
MmsResult ToonLoadError(HRESULT r) noexcept {
    return r == E_OUTOFMEMORY || r == D3DERR_OUTOFVIDEOMEMORY ? MMS_E_MEMORY : MMS_E_NOT_READY;
}
}
MmsResult PrepareSharedToon(MMDApp& app, PreparedSharedToon& out) noexcept {
    try {
        auto* renderer = app.Renderer(); auto* device = renderer ? renderer->device : nullptr;
        if (!device) return MMS_E_NOT_READY;
        const auto module = static_cast<HMODULE>(app.HInstance());
        if (!module) return MMS_E_NOT_READY;
        // Freeze every path and its exact ACP spelling before a D3DX hook can
        // run. Absolute paths avoid changing the process CWD; preserving the A
        // import where lossless keeps that original hook boundary observable.
        // This does not promise an external hook's relative-key equivalence.
        const auto length = wcsnlen_s(app.ExeDir(), 256);
        if (!length || length == 256) return MMS_E_NOT_READY;
        const std::wstring capturedDirectory(app.ExeDir(), length);
        std::array<wchar_t, 32768> absolute{};
        const auto absoluteLength = GetFullPathNameW(capturedDirectory.c_str(), DWORD(absolute.size()), absolute.data(), nullptr);
        if (!absoluteLength || absoluteLength >= absolute.size()) return MMS_E_NOT_READY;
        std::wstring directory(absolute.data(), absoluteLength);
        if (directory.back() != L'\\' && directory.back() != L'/') directory.push_back(L'\\');
        struct Path { std::wstring wide; std::string ansi; bool exact = false; };
        std::array<Path, 10> paths;
        for (unsigned i = 0; i < paths.size(); ++i) {
            wchar_t name[32]{}; swprintf_s(name, L"data\\toon%02u.bmp", i + 1);
            paths[i].wide = directory + name;
            paths[i].exact = text_encoding::EncodeExact(paths[i].wide.c_str(), CP_ACP, paths[i].ansi);
        }
        auto& api = d3dx::Get();
        PreparedSharedToon candidate;
        const float defaults[18] = {
            0.80078101f, 0.80078101f, 0.80078101f, 0.95703101f, 0.87890601f,
            0.87890601f, 0.60156298f, 0.60156298f, 0.60156298f, 0.96875f,
            0.93359399f, 0.91796899f, 1.0f, 0.90234399f,
            0.86328101f, 0.76171899f, 0.671875f, 0.011719f
        };
        std::copy_n(defaults, 18, candidate.edge.begin());
        std::fill(candidate.edge.begin() + 18, candidate.edge.end(), 1.0f);
        const auto embedded = [&](unsigned id, Texture& texture) -> MmsResult {
            const auto resource = FindResourceA(module, MAKEINTRESOURCEA(id), "PNG");
            if (!resource) return MMS_E_NOT_READY;
            const auto bytes = SizeofResource(module, resource); const auto handle = LoadResource(module, resource);
            const auto data = handle ? LockResource(handle) : nullptr;
            if (!data || !bytes) return MMS_E_NOT_READY;
            // Re-read the shared import slots for this call too: a file hook
            // may have installed a different memory hook before fallback.
            if (!api.Load() || !api.fromMemEx) return MMS_E_NOT_READY;
            IDirect3DTexture9* raw = nullptr; HRESULT result;
            try { result = api.fromMemEx(device, data, bytes, UINT(-1), UINT(-1), 1, 0, D3DFMT_UNKNOWN,
                D3DPOOL_MANAGED, DWORD(-1), DWORD(-1), 0, nullptr, nullptr, &raw); }
            catch (...) { if (raw) raw->Release(); throw; }
            Texture next(raw); if (FAILED(result)) return ToonLoadError(result);
            if (!next) return MMS_E_NOT_READY; texture = std::move(next); return MMS_OK;
        };
        if (const auto result = embedded(0x67, candidate.textures[0])) return result;
        for (unsigned i = 1; i < 11; ++i) {
            const auto& path = paths[i - 1];
            IDirect3DTexture9* raw = nullptr; D3dxImageInfo info{}; HRESULT result;
            if (!api.Load() || (path.exact ? !api.fromFileExA : !api.fromFileExW)) return MMS_E_NOT_READY;
            try {
                result = path.exact ? api.fromFileExA(device, path.ansi.c_str(), UINT(-1), UINT(-1), 1, 0,
                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, DWORD(-1), DWORD(-1), 0, &info, nullptr, &raw) :
                    api.fromFileExW(device, path.wide.c_str(), UINT(-1), UINT(-1), 1, 0,
                        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, DWORD(-1), DWORD(-1), 0, &info, nullptr, &raw);
            }
            catch (...) { if (raw) raw->Release(); throw; }
            Texture texture(raw);
            if (FAILED(result)) {
                if (FatalToonLoad(result)) return ToonLoadError(result);
                texture.reset(); if (const auto r = embedded(i + 103, texture)) return r;
            } else {
                if (!texture) return MMS_E_NOT_READY;
                D3DSURFACE_DESC desc{}; D3DLOCKED_RECT pixels{};
                if (FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height ||
                    !info.Height || info.Height > desc.Height || desc.Format != D3DFMT_A8R8G8B8)
                    return MMS_E_NOT_READY;
                if (SUCCEEDED(texture->LockRect(0, &pixels, nullptr, D3DLOCK_READONLY))) {
                    struct Unlock { IDirect3DTexture9* texture; ~Unlock() { texture->UnlockRect(0); } } unlock{texture.get()};
                    if (!pixels.pBits || pixels.Pitch <= 0 || uint64_t(pixels.Pitch) < uint64_t(desc.Width) * 4 ||
                        uint64_t(info.Height - 1) * uint64_t(pixels.Pitch) > uint64_t(PTRDIFF_MAX)) return MMS_E_NOT_READY;
                    // The original uses D3DXIMAGE_INFO.Height (the source
                    // image), which can differ from a rounded-up texture.
                    const auto* row = static_cast<const unsigned char*>(pixels.pBits) + size_t(info.Height - 1) * size_t(pixels.Pitch);
                    candidate.edge[3 * (i - 1)] = row[2] * 0.00390625f;
                    candidate.edge[3 * (i - 1) + 1] = row[1] * 0.00390625f;
                    candidate.edge[3 * (i - 1) + 2] = row[0] * 0.00390625f;
                }
            }
            candidate.textures[i] = std::move(texture);
        }
        out = std::move(candidate); return MMS_OK;
    } catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
}
#endif

}  // namespace mikumikustudio
