#pragma once
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/background_picture_asset.hpp"
#include "../src/plugins/background_identity.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>

// Called only by the full-profile, isolated hidden production-host fixture.
// It writes no settings: the parent harness must run from its own data root,
// because the real importer reads the production picture resource policy.
namespace picture_reset_checks {
namespace identity = mikumikustudio::plugins::background_identity;
namespace media = mikumikustudio::media;
using mikumikustudio::MMDApp;

inline void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct ClosureCheckpoint {
    const void* app = nullptr;
    std::uint64_t instance = 0, ticket = 0;
    std::weak_ptr<const media::BackgroundPictureAsset> asset;
    std::weak_ptr<const media::BackgroundPictureSource> source;
    bool ready = false;
};
inline ClosureCheckpoint closure;

struct TempBitmap final {
    std::wstring path;
    bool present = false;
    TempBitmap() {
        wchar_t temporary[MAX_PATH]{}, name[MAX_PATH]{};
        const auto length = GetTempPathW(MAX_PATH, temporary);
        Require(length && length < MAX_PATH, "picture reset: get temporary directory");
        Require(GetTempFileNameW(temporary, L"mpr", 0, name) != 0,
            "picture reset: create unique owned fixture");
        try { path = name; }
        catch (...) { DeleteFileW(name); throw; }
        present = true;
        try {
            auto bmpPath = path + L".bmp";
            // No replacement flag: an unrelated pre-existing suffix is never
            // overwritten or deleted, even in the unlikely event of a clash.
            Require(MoveFileExW(path.c_str(), bmpPath.c_str(), 0) != FALSE,
                "picture reset: give owned fixture its BMP extension");
            path.swap(bmpPath);
            struct File {
                HANDLE value = INVALID_HANDLE_VALUE;
                ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            } file;
            file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(file.value != INVALID_HANDLE_VALUE, "picture reset: open owned BMP");
            BITMAPFILEHEADER header{};
            BITMAPINFOHEADER info{};
            std::array<unsigned char, 8 * 8 * 3> pixels{};
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof(header) + sizeof(info);
            header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
            info.biSize = sizeof(info); info.biWidth = 8; info.biHeight = 8;
            info.biPlanes = 1; info.biBitCount = 24;
            info.biSizeImage = static_cast<DWORD>(pixels.size());
            for (unsigned y = 0; y != 8; ++y) for (unsigned x = 0; x != 8; ++x) {
                auto* pixel = pixels.data() + ((7 - y) * 8 + x) * 3;
                pixel[0] = 37;
                pixel[1] = static_cast<unsigned char>(50 + 3 * y);
                pixel[2] = static_cast<unsigned char>(70 + 5 * x);
            }
            const auto write = [&](const void* bytes, DWORD count) {
                DWORD written = 0;
                Require(WriteFile(file.value, bytes, count, &written, nullptr) && written == count,
                    "picture reset: write complete BMP fixture");
            };
            write(&header, sizeof(header)); write(&info, sizeof(info));
            write(pixels.data(), static_cast<DWORD>(pixels.size()));
        } catch (...) { DeleteFileW(path.c_str()); present = false; throw; }
    }
    ~TempBitmap() { if (present) DeleteFileW(path.c_str()); }
    void Remove() {
        Require(present && DeleteFileW(path.c_str()), "picture reset: delete only the owned BMP");
        present = false;
        Require(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
            "picture reset: source file remains absent");
    }
};

inline void VerifyPixels(IDirect3DDevice9* device, const media::BackgroundPictureSourcePtr& source) {
    Require(device && source && source->Data() && source->Size(), "picture reset: retained bytes available");
    auto& decoder = mikumikustudio::d3dx::Get();
    Require(decoder.Load() && decoder.fromMemEx, "picture reset: resolve actual D3DX memory decoder");
    struct ImageInfo {
        UINT width = 0, height = 0, depth = 0, levels = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        D3DRESOURCETYPE resource = D3DRTYPE_SURFACE;
        UINT fileFormat = 0;
    } info;
    static_assert(sizeof(ImageInfo) == 28, "D3DXIMAGE_INFO ABI");
    struct Texture {
        IDirect3DTexture9* value = nullptr;
        bool locked = false;
        ~Texture() {
            if (value) { if (locked) value->UnlockRect(0); value->Release(); }
        }
    } texture;
    Require(SUCCEEDED(decoder.fromMemEx(device, source->Data(), source->Size(), 8, 8, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, 1, 1, 0, &info, nullptr, &texture.value)) && texture.value,
        "picture reset: decode source after original file deletion");
    Require(info.width == 8 && info.height == 8, "picture reset: decoder preserves source dimensions");
    D3DSURFACE_DESC description{};
    Require(SUCCEEDED(texture.value->GetLevelDesc(0, &description)) && description.Width == 8 &&
        description.Height == 8 && description.Format == D3DFMT_A8R8G8B8,
        "picture reset: readable SYSTEMMEM texture geometry");
    D3DLOCKED_RECT locked{};
    Require(SUCCEEDED(texture.value->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)),
        "picture reset: lock actual decoded pixels");
    texture.locked = true;
    for (unsigned y : {0u, 3u, 7u}) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (unsigned x : {0u, 4u, 7u}) {
            const DWORD expected = ((70 + 5 * x) << 16) | ((50 + 3 * y) << 8) | 37;
            Require((row[x] & 0xffffff) == expected, "picture reset: exact retained BMP pixel sample");
        }
    }
}

inline void Run(MMDApp* app) {
    using namespace mikumikustudio;
    Require(app && g_Block == app && app->MainWindow() && app->Renderer() && app->Renderer()->device,
        "picture reset: initialized real application required");
    Require(!closure.ready, "picture reset: no previous unfinished closure checkpoint");
    TempBitmap fixture;
    Require(TryReplaceBackgroundPicture(app, fixture.path.c_str()), "picture reset: import real owned BMP");
    const auto instance = identity::Instance(app);
    const auto ticket = identity::Acquire(app, identity::Kind::Picture);
    auto asset = identity::CurrentPictureAsset(app, instance, ticket);
    auto source = identity::CurrentPictureSource(app, instance, ticket);
    Require(instance && ticket && asset && source && asset->Source() == source && asset->Id(),
        "picture reset: imported asset and source share actual registry ownership");
    const auto assetId = asset->Id(), scene = app->SceneGeneration();
    auto* const texture = app->PictureBackgroundTexture();
    const std::wstring path = app->PictureBackgroundPath();
    Require(texture && app->PictureBackgroundEnabled() && path == asset->Path() &&
        asset->Width() == 8 && asset->Height() == 8, "picture reset: complete published picture baseline");
    // A non-default placement distinguishes retention from silently recentering.
    app->PictureOffsetX() = 23; app->PictureOffsetY() = -17; app->PictureScale() = .75f;
    PicBgOverlayRefresh(app);
    const auto x = app->PictureOffsetX(), y = app->PictureOffsetY();
    const float scale = app->PictureScale();
    const auto width = app->PictureWidth(), height = app->PictureHeight();
    fixture.Remove();

    ResetAppState(app);
    Require(app->SceneGeneration() > scene && identity::Instance(app) == instance,
        "picture reset: actual scene reset advances scene but not application identity");
    Require(!identity::IsCurrent(app, identity::Kind::Picture, ticket) &&
        !identity::CurrentPictureAsset(app, instance, ticket) && !identity::CurrentPictureSource(app, instance, ticket),
        "picture reset: previous activation is permanently stale");
    const auto nextTicket = identity::Acquire(app, identity::Kind::Picture);
    Require(nextTicket && nextTicket != ticket && identity::CurrentPictureAsset(app, instance, nextTicket) == asset &&
        identity::CurrentPictureSource(app, instance, nextTicket) == source && asset->Id() == assetId,
        "picture reset: fresh activation discovers the same stable asset and source");
    Require(app->PictureBackgroundTexture() == texture && path == app->PictureBackgroundPath() &&
        app->PictureWidth() == width && app->PictureHeight() == height && app->PictureOffsetX() == x &&
        app->PictureOffsetY() == y && std::memcmp(&app->PictureScale(), &scale, sizeof(scale)) == 0 &&
        !app->PictureBackgroundEnabled(), "picture reset: hides but preserves texture, path, dimensions and placement");
    CommandDispatch(nullptr, MAKEWPARAM(233, 0));
    Require(app->PictureBackgroundEnabled() && app->PictureBackgroundTexture() == texture &&
        identity::CurrentPictureAsset(app, instance, nextTicket) == asset && asset->Id() == assetId &&
        identity::CurrentPictureSource(app, instance, nextTicket) == source,
        "picture reset: actual picture menu toggles retained image on without creating another asset");
    Require(GetFileAttributesW(fixture.path.c_str()) == INVALID_FILE_ATTRIBUTES,
        "picture reset: menu toggle did not require or recreate the deleted source");
    VerifyPixels(app->Renderer()->device, source);
    closure.app = app; closure.instance = instance; closure.ticket = nextTicket;
    closure.asset = asset; closure.source = source; closure.ready = true;
    std::puts("PASS real picture scene reset, new activation, menu redisplay and nine retained-source pixels");
    // No strong asset/source reference survives Run; ShutdownCleanup must
    // release the registry's final ownership without test-side Forget calls.
}

inline void VerifyClosed() {
    Require(closure.ready && closure.app, "picture reset: closure checkpoint was recorded");
    Require(identity::Instance(closure.app) == 0 &&
        !identity::IsCurrent(closure.app, identity::Kind::Picture, closure.ticket) &&
        !identity::CurrentPictureAsset(closure.app, closure.instance, closure.ticket) &&
        !identity::CurrentPictureSource(closure.app, closure.instance, closure.ticket) &&
        identity::Acquire(closure.app, identity::Kind::Picture) == 0,
        "picture reset: actual shutdown closes registry access without reviving activation");
    Require(closure.asset.expired() && closure.source.expired(),
        "picture reset: actual shutdown releases the final retained asset and source");
    closure = {};
    std::puts("PASS picture asset and source released by real ShutdownCleanup");
}
}
