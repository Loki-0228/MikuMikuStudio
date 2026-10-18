// ===========================================================================
// VA 0x004089F0 - ResolveUserFilePath (original: sub_4089F0, 0x47D bytes)
// VA 0x00407DA0 - ConvertMaterialName (original: sub_407DA0, 0x280 bytes)
// VA 0x00407490 - LoadTextureShared  (original: sub_407490, 0x1CD bytes)
// ===========================================================================
// Path helpers of the model-load chain.
//
// 0x4089F0: __thiscall(appPathBuf, widePath).  Resolves a user file by
//   probing, in order: "%s%s"(buf+512 dir, tail-from-"UserFile"), the same
//   with buf+0, the raw path, then the UserFile\Model / UserFile\Wave /
//   UserFile\Accessory fallbacks by extension (pmd/wav/x/vac).  Result is
//   stored wide at buf wchar 1512 (byte 3024); 0 on failure.
//
// 0x407DA0: __thiscall(renderSub, ansiName, wideOut, sizeInWords, dirW).
//   Converts a narrow (SJIS) PMD material/texture name to wide, trying the
//   ACP conversion first and then four stored _locale_t handles at
//   renderSub dwords 30001..30004 (bytes 120004..120016), keeping the
//   first variant whose file exists.  NOTE: the original passes the wide
//   buffers to _sopen_s - the CRT entry is actually _wsopen_s (wide);
//   documented as an IDA FLIRT mislabel in docs/ARCHITECTURE.md.
//
// 0x407490: __thiscall(renderSub, widePath).  Shared texture cache: up to
//   10000 entries of (texture*, name*, pixel-colour) at renderSub dword 1
//   (byte 4), stride 12.  Loads via D3DXCreateTextureFromFileExW (mipmaps
//   1024 with a no-mipmap retry), stores the bottom-left pixel colour at
//   entry+12..14, returns 1 on success.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/d3d_wrapper.hpp"
#include "mikudancestudio/model_alpha_pass.hpp"
#include "mikudancestudio/path_workspace.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/text_encoding.hpp"

#include "mikudancestudio/texture_lookup.hpp"

namespace mikudancestudio {
namespace {

// minimal D3DX constants/struct for the dynamic loader (no SDK headers)
constexpr UINT kD3dxDefault = 0xFFFFFFFFu;
struct ImgInfo {                    // first fields of D3DXIMAGE_INFO
    UINT Width, Height, Depth, MipLevels;
    UINT rest[16];
};

bool WsOpenOk(const wchar_t* path) {
    int fh = -1;
    if (_wsopen_s(&fh, path, _O_BINARY, _SH_DENYNO, _S_IREAD) == 0) {
        _close(fh);
        return true;
    }
    return false;
}

}  // namespace

const wchar_t* ResolveUserFilePath(PathResolutionWorkspace& workspace,
                                   const wchar_t* path) {      // 0x4089F0
    wchar_t* out = workspace.resolvedPath;
    const wchar_t* dirBase = workspace.executableDirectory;
    wchar_t tmp[256];
    if (path[0] == L'\0') {
        out[0] = L'\0';
        return out;
    }
    const wchar_t* uf = wcsstr(path, L"UserFile");
    if (uf != nullptr) {
        swprintf_s(tmp, 0x100, L"%s%s", dirBase, uf);
        bool ok = WsOpenOk(tmp);
        if (!ok &&
            workspace.projectDirectory[0] != L'\0') {
        swprintf_s(tmp, 0x100, L"%s%s",
                       workspace.projectDirectory, uf);
            ok = WsOpenOk(tmp);
        }
        if (ok) {
            wcscpy_s(out, 0x100, tmp);
            return out;
        }
    }
    if (WsOpenOk(path)) {
        wcscpy_s(out, 0x100, path);
        return out;
    }
    const size_t len = wcslen(path);
    if (len < 3) {
        out[0] = L'\0';
        return out;
    }
    const wchar_t* ext = &path[len - 3];
    const wchar_t* scan = ext;
    const wchar_t* sub = nullptr;
    if (wcscmp(scan, L"pmd") == 0 || wcscmp(scan, L"P") == 0)
        sub = L"UserFile\\Model";
    else if (wcscmp(&path[len - 3], L"wav") == 0 ||
             wcscmp(&path[len - 3], L"WAV") == 0)
        sub = L"UserFile\\Wave";
    else if ((wcscmp(&path[len - 3], L"x") == 0 ||
              wcscmp(&path[len - 3], L"X") == 0) ||
             (wcscmp(&path[len - 3], L"vac") == 0 ||
              wcscmp(&path[len - 3], L"V") == 0))
        sub = L"UserFile\\Accessory";
    if (sub == nullptr) {
        out[0] = L'\0';
        return out;
    }
    // walk back to the last path separator (fail if none)
    while (scan > path && *scan != L'\\') --scan;
    if (*scan != L'\\') {
        out[0] = L'\0';
        return out;
    }
    swprintf_s(tmp, 0x100, L"%s%s%s", dirBase, sub, scan);
    if (WsOpenOk(tmp)) {
        wcscpy_s(out, 0x100, tmp);
        return out;
    }
    out[0] = L'\0';
    return out;
}

bool ResolveAnsiUserFile(unsigned char* sub, const char* mbName,
                         wchar_t* wideOut, rsize_t sizeWords,
                         PathResolutionWorkspace& paths) {     // 0x407BA0
    wcscpy_s(wideOut, sizeWords, L"");
    if (mbName == nullptr || mbName[0] == '\0')
        return false;
    wchar_t candidate[512] = {};
    auto resolve = [&]() {
        const wchar_t* value = ResolveUserFilePath(paths, candidate);
        if (value[0] == L'\0')
            return false;
        wcsncpy_s(wideOut, sizeWords, value, _TRUNCATE);
        return true;
    };
    (void)sub;
#if MMD_ORIGINAL_PROFILE
    std::wstring decoded;
    if (text_encoding::HasUtf8Bom(mbName) ||
        !text_encoding::Decode(mbName, 932, decoded) ||
        decoded.size() >= 256 || decoded.size() >= sizeWords) return false;
    wcscpy_s(candidate, decoded.c_str());
    return resolve();
#else
    const bool utf8 = text_encoding::HasUtf8Bom(mbName);
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        std::wstring decoded;
        if (!text_encoding::Decode(utf8 ? mbName + 3 : mbName,
                                   utf8 ? CP_UTF8 : cp, decoded))
            continue;
        if (decoded.size() >= 256 || decoded.size() >= sizeWords)
            continue;
        wcscpy_s(candidate, decoded.c_str());
        if (resolve())
            return true;
        if (utf8) break;
    }
    return false;
#endif
}

errno_t ConvertMaterialName(unsigned char* sub, const char* mbName,
                            wchar_t* wideOut, rsize_t sizeWords,
                            const wchar_t* dirW) {             // 0x407DA0
    wchar_t probe[512];
    errno_t result = wcscpy_s(wideOut, sizeWords, L"");
    if (mbName[0] == '\0')
        return result;

#if MMD_ORIGINAL_PROFILE
    std::wstring decoded;
    if (text_encoding::HasUtf8Bom(mbName) ||
        !text_encoding::Decode(mbName, 932, decoded) || decoded.size() >= sizeWords)
        return ERROR_NO_UNICODE_TRANSLATION;
    return wcscpy_s(wideOut, sizeWords, decoded.c_str());
#else

    int cw = MultiByteToWideChar(0, 0, mbName, -1, nullptr, 0);
    wchar_t* tmp = static_cast<wchar_t*>(malloc(2 * cw));
    if (MultiByteToWideChar(0, 0, mbName,
                            static_cast<int>(strlen(mbName)) + 1, tmp, cw))
        wcsncpy_s(wideOut, sizeWords, tmp, _TRUNCATE);
    free(tmp);

    // probe ACP result, then the four stored locale conversions
    for (int li = 0; li < 5; ++li) {
        swprintf_s(probe, 0x200, L"%s%s", dirW, wideOut);
        if (WsOpenOk(probe))
            return 0;
        if (li < 4)
            _mbstowcs_s_l(nullptr, wideOut, sizeWords, mbName, _TRUNCATE,
                reinterpret_cast<D3DRenderer*>(sub)->localeTable[li]);
    }
    return result;
#endif
}

int LoadTextureShared(unsigned char* sub, wchar_t* path) {     // 0x407490
    if (path[0] == L'\0')
        return 0;
    D3DRenderer* r = reinterpret_cast<D3DRenderer*>(sub);
    IDirect3DDevice9* dev = r->device;
    // Cache is the wrapper's 10000-entry resourcePool (stride 12 on x86, 24 on
    // x64; the original write base is sub+0 at 0x40750A): heapBuffer holds the name*,
    // comObject the texture*; the rgb triple is written to the tag
    // dword (originally +0xC..+0xE), which falls into the NEXT entry's first
    // dword in the flat view (never scanned). The original scan base is
    // sub+4 (0x4074AF), i.e. it reads {name, tex} pairs.
    // (Port fix: an earlier layout read the texture pointer as the name,
    // crashing the wcscmp scan on the second texture - found by desktop
    // runtime testing with Luka_Megurine.pmd, docs/ARCHITECTURE.md §8.)
    int used = 0;
    while (used < 10000) {
        wchar_t* nm = static_cast<wchar_t*>(
            r->resourcePool[used].heapBuffer);
        if (nm == nullptr)
            break;
        if (wcscmp(nm, path) == 0)
            return 1;
        ++used;
    }
    // Deliberate hardening: the original counts pool entries against
    // 10000 (0x140008C45) but never checks the counter after the loop,
    // then derives the write base from it (0x140008C55 onward, used*24) and
    // writes the name, the texture and the colour there. A full pool
    // therefore writes one slot past the array, which on x64 lands on the
    // localeTable at +240008. This port instead refuses to add the entry.
    // Defect MB-26.
    if (used >= 10000)
        return 0;
    // The RGB sample occupies the low three bytes of this entry's tag.
    // Do not retain the original x86 wrapper-relative +0xC/+0xE offsets:
    // x64 pointer widening would otherwise overwrite the next cache entry.
    unsigned char* tagBytes =
        reinterpret_cast<unsigned char*>(&r->resourcePool[used].tag);
    auto* entryTex = reinterpret_cast<IDirect3DTexture9**>(
        &r->resourcePool[used].comObject);
    auto* entryName = reinterpret_cast<wchar_t**>(
        &r->resourcePool[used].heapBuffer);
    ImgInfo info = {};
    HRESULT hr;
    auto* d3dx = &d3dx::Get();
    if (!d3dx->Load() || d3dx->fromFileExW == nullptr)
        return 0;
    hr = d3dx->fromFileExW(dev, path, kD3dxDefault, kD3dxDefault, 1, 1024,
                           D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, kD3dxDefault,
                           kD3dxDefault, 0, &info, nullptr, entryTex);
    if (info.Width == 1 && info.Height == 1 && hr == 0) {
        if (*entryTex != nullptr) {
            (*entryTex)->Release();
            *entryTex = nullptr;
        }
        hr = d3dx->fromFileExW(dev, path, kD3dxDefault, kD3dxDefault, 1, 0,
                               D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                               kD3dxDefault, kD3dxDefault, 0, &info, nullptr,
                               entryTex);
    }
    if (FAILED(hr) &&
        FAILED(d3dx->fromFileExW(dev, path, kD3dxDefault, kD3dxDefault, 1, 0,
                                 D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                 kD3dxDefault, kD3dxDefault, 0, &info,
                                 nullptr, entryTex))) {
        *entryTex = nullptr;
        return 0;
    }
    const size_t plen = wcslen(path) + 1;
    *entryName = static_cast<wchar_t*>(malloc(2 * plen));
    wcscpy_s(*entryName, plen, path);

    CacheTextureColorAndAlpha(*entryTex, tagBytes);
    InvalidateTextureLookup();
    return 1;
}

}  // namespace mikudancestudio
