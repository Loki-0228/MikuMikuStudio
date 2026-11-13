// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/background_identity.hpp"
#include "mikumikustudio/background_picture_source.hpp"
#include "mikumikustudio/background_picture_asset.hpp"
#include "mikumikustudio/background_picture_activation.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "../src/app/settings_json.hpp"
#endif
#include <vfw.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

using namespace mikumikustudio;
#if !MMD_ORIGINAL_PROFILE && defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
extern "C" d3dx::FnCreateTexInMemEx __imp_D3DXCreateTextureFromFileInMemoryEx;
#endif
namespace {
void Check(bool ok, const char* message);
#if !MMD_ORIGINAL_PROFILE
unsigned errorDialogCount = 0;
LRESULT CALLBACK CloseErrorDialog(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_ACTIVATE) {
        wchar_t name[32]{};
        HWND window = reinterpret_cast<HWND>(wparam);
        if (GetClassNameW(window, name, 32) && std::wcscmp(name, L"#32770") == 0) {
            ++errorDialogCount;
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
MMDApp* reentryApp = nullptr;
int reentryCalls = 0;
int fallbackCalls = 0;
d3dx::FnCreateTexInMemEx fallbackDecoder = nullptr;
const void* fallbackBytes = nullptr;
UINT fallbackSize = 0;
HRESULT WINAPI ForcePictureFallback(IDirect3DDevice9* device, LPCVOID bytes, UINT size, UINT requestedWidth,
    UINT requestedHeight, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    DWORD filter, DWORD mipFilter, D3DCOLOR colorKey, void* info, void* palette, IDirect3DTexture9** texture) {
    ++fallbackCalls;
    if (fallbackCalls == 1) {
        Check(requestedWidth == 1024 && requestedHeight == 1024, "first actual picture decode requests primary dimensions");
        fallbackBytes = bytes; fallbackSize = size;
        return E_FAIL;
    }
    Check(fallbackCalls == 2 && requestedWidth == 512 && requestedHeight == 512 && levels == 1 &&
          bytes == fallbackBytes && size == fallbackSize, "fallback uses the same captured source with actual retry dimensions");
    return fallbackDecoder(device, bytes, size, requestedWidth, requestedHeight, levels, usage, format, pool,
        filter, mipFilter, colorKey, info, palette, texture);
}
int budgetDecodeCalls = 0;
HRESULT WINAPI UnexpectedBudgetDecode(IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT,
    DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**) {
    ++budgetDecodeCalls;
    return E_FAIL;
}
HRESULT WINAPI ReentrantPictureFailure(IDirect3DDevice9*, LPCVOID bytes, UINT size, UINT, UINT, UINT,
    DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, void*, IDirect3DTexture9**) {
    ++reentryCalls;
    Check(bytes && size > sizeof(BITMAPFILEHEADER), "reentrant decoder receives captured image bytes");
    plugins::background_identity::Mutation nested(reentryApp, true, false);
    return E_FAIL;
}
void WriteImport(d3dx::FnCreateTexInMemEx value) {
    DWORD old = 0, ignored = 0;
    Check(VirtualProtect(&__imp_D3DXCreateTextureFromFileInMemoryEx, sizeof(value), PAGE_READWRITE, &old) != FALSE,
          "unlock picture decoder import");
    __imp_D3DXCreateTextureFromFileInMemoryEx = value;
    Check(VirtualProtect(&__imp_D3DXCreateTextureFromFileInMemoryEx, sizeof(value), old, &ignored) != FALSE,
          "restore picture decoder import protection");
}
#endif
#endif
constexpr int width = 160, height = 96;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
std::vector<unsigned char> Pixels(int frame) {
    std::vector<unsigned char> data(width * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto* pixel = &data[((height - 1 - y) * width + x) * 3];
            pixel[0] = static_cast<unsigned char>(40 + frame * 100);
            pixel[1] = static_cast<unsigned char>(32 + y * 2);
            pixel[2] = static_cast<unsigned char>(32 + x);
        }
    return data;
}
void WriteBitmap(const std::wstring& path, int frame) {
    BITMAPINFOHEADER info{};
    info.biSize = sizeof info; info.biWidth = width; info.biHeight = height;
    info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = width * height * 3;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42; header.bfOffBits = sizeof header + sizeof info;
    header.bfSize = header.bfOffBits + info.biSizeImage;
    FILE* file = nullptr;
    Check(_wfopen_s(&file, path.c_str(), L"wb") == 0, "create BMP fixture");
    const auto pixels = Pixels(frame);
    Check(std::fwrite(&header, sizeof header, 1, file) == 1 &&
          std::fwrite(&info, sizeof info, 1, file) == 1 &&
          std::fwrite(pixels.data(), pixels.size(), 1, file) == 1, "write BMP fixture");
    Check(std::fclose(file) == 0, "close complete BMP fixture");
}
struct MediaFiles {
    std::wstring bitmap, video, sourceBitmap, budgetBitmap, leaseBitmap;
    MediaFiles() {
        wchar_t directory[MAX_PATH]{}, name[100]{};
        Check(GetTempPathW(MAX_PATH, directory) > 0, "temporary media directory");
        swprintf_s(name, L"MDS_background_%lu_背景_日本語", GetCurrentProcessId());
        bitmap = std::wstring(directory) + name + L".bmp";
        video = std::wstring(directory) + name + L".avi";
        sourceBitmap = std::wstring(directory) + name + L"_source.bmp";
        budgetBitmap = std::wstring(directory) + name + L"_budget.bmp";
        leaseBitmap = std::wstring(directory) + name + L"_lease.bmp";
        BITMAPINFOHEADER info{};
        info.biSize = sizeof info; info.biWidth = width; info.biHeight = height;
        info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = width * height * 3;
        WriteBitmap(bitmap, 0);
        AVIFileInit();
        PAVIFILE avi = nullptr;
        Check(AVIFileOpenW(&avi, video.c_str(), OF_WRITE | OF_CREATE, nullptr) == 0, "create AVI fixture");
        AVISTREAMINFOW streamInfo{};
        streamInfo.fccType = streamtypeVIDEO; streamInfo.dwScale = 1; streamInfo.dwRate = 30;
        streamInfo.rcFrame = {0, 0, width, height};
        PAVISTREAM stream = nullptr;
        Check(AVIFileCreateStreamW(avi, &stream, &streamInfo) == 0, "create AVI video stream");
        Check(AVIStreamSetFormat(stream, 0, &info, sizeof info) == 0, "set AVI bitmap format");
        for (int frame = 0; frame < 2; ++frame) {
            auto pixels = Pixels(frame);
            Check(AVIStreamWrite(stream, frame, 1, pixels.data(), static_cast<LONG>(pixels.size()),
                                 AVIIF_KEYFRAME, nullptr, nullptr) == 0, "write AVI frame");
        }
        AVIStreamRelease(stream); AVIFileRelease(avi);
    }
    ~MediaFiles() {
        DeleteFileW(bitmap.c_str()); DeleteFileW(video.c_str()); DeleteFileW(sourceBitmap.c_str()); AVIFileExit();
        DeleteFileW(budgetBitmap.c_str()); DeleteFileW(leaseBitmap.c_str());
    }
};

#if !MMD_ORIGINAL_PROFILE
constexpr std::uint64_t MiB = 1024ull * 1024;
struct BudgetSettings {
    bool enabled = false;
    std::filesystem::path path;
    std::uint64_t previousLimit = 0;
    BudgetSettings(int argc, wchar_t** argv) {
        if (argc == 1) return;
        Check(argc == 3 && (!std::wcscmp(argv[1], L"--picture-source-budget") ||
              !std::wcscmp(argv[1], L"--picture-activation")), "known isolated picture test mode");
        wchar_t executable[32768]{};
        Check(GetModuleFileNameW(nullptr, executable, 32768) > 0, "resolve isolated budget test executable");
        Check(std::filesystem::path(argv[2]).is_absolute(), "isolated picture test requires an absolute EXE directory");
        const auto actual = std::filesystem::canonical(std::filesystem::path(executable).parent_path());
        const auto requested = std::filesystem::canonical(argv[2]);
        Check(std::filesystem::equivalent(actual, requested), "budget mode requires its explicitly named isolated EXE directory");
        path = system_settings::Path();
        Check(path == actual / L"data" / L"settings.json" && !std::filesystem::exists(path.parent_path()),
              "budget mode refuses an existing data directory before writing any settings");
        if (!std::wcscmp(argv[1], L"--picture-activation")) return; // Read-only settings; no data directory is created.
        previousLimit = media::GlobalPictureSourceBudget().Usage().limit;
        enabled = true;
        Check(system_settings::WriteSection("pictureBudgetSentinel", "{\"untouched\":73}") == system_settings::Result::Ok &&
              system_settings::WriteSection("resources", "{\"futureResource\":{\"keep\":91}}") == system_settings::Result::Ok,
              "seed only new isolated settings with unrelated fields and no source budget preference");
    }
    ~BudgetSettings() {
        if (!enabled) return;
        Check(system_settings::WriteSection("resources", "{\"backgroundPictureSourcesMiB\":128}") == system_settings::Result::Ok,
              "restore default preference through merging settings writer");
        std::string body;
        Check(system_settings::ReadSection("resources", body) == system_settings::Result::Ok, "read restored resource settings");
        const auto resources = settings_json::Object(body);
        Check(resources.At("backgroundPictureSourcesMiB").UInt() == 128 && resources.At("futureResource").At("keep").UInt() == 91,
              "restoring the default budget preserves unknown resource members");
        Check(system_settings::ReadSection("pictureBudgetSentinel", body) == system_settings::Result::Ok &&
              settings_json::Object(body).At("untouched").UInt() == 73, "budget updates preserve unrelated settings sections");
        media::GlobalPictureSourceBudget().Configure(previousLimit);
        Check(DeleteFileW(path.c_str()) != FALSE, "remove only this mode's newly created settings file");
        Check(DeleteFileW((path.wstring() + L".lock").c_str()) != FALSE, "remove only this mode's newly created settings lock");
        Check(RemoveDirectoryW(path.parent_path().c_str()) != FALSE, "remove empty owned data directory without recursive deletion");
        std::puts("PASS picture source budget loading and settings isolation");
    }
};
void WritePaddedBitmap(const std::wstring& path, std::uint64_t bytes) {
    WriteBitmap(path, 0);
    Check(bytes > sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + width * height * 3 && bytes <= 16 * MiB,
          "bounded padding fixture fits at most sixteen MiB");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open owned BMP for bounded trailing padding");
    LARGE_INTEGER length{}; length.QuadPart = static_cast<LONGLONG>(bytes);
    const bool ready = SetFilePointerEx(file, length, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    Check(ready, "append bounded BMP padding without allocating the candidate in the test");
}
// D3DXIMAGE_INFO layout, matching the dynamically resolved decoder signature.
struct ImageInfo {
    UINT width, height, depth, levels;
    D3DFORMAT format;
    D3DRESOURCETYPE resource;
    UINT fileFormat;
};
static_assert(sizeof(ImageInfo) == 28, "D3DX image information ABI");
void CheckSourcePixels(IDirect3DDevice9* device,
    const std::shared_ptr<const media::BackgroundPictureSource>& source, int frame) {
    Check(source && source->Data() && source->Size() > sizeof(BITMAPFILEHEADER) &&
          source->RetainedBytes() >= source->Size(),
          "retained picture source owns nonempty encoded bytes and accounts for them");
    auto& api = d3dx::Get();
    Check(api.Load() && api.fromMemEx, "resolve retained-source image decoder");
    IDirect3DTexture9* texture = nullptr;
    ImageInfo info{};
    Check(SUCCEEDED(api.fromMemEx(device, source->Data(), static_cast<UINT>(source->Size()),
        width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, 1, 1, 0,
        &info, nullptr, &texture)) && texture, "decode retained image bytes without reopening a path");
    Check(info.width == width && info.height == height, "retained BMP decoder reports original source dimensions");
    D3DSURFACE_DESC desc{};
    Check(SUCCEEDED(texture->GetLevelDesc(0, &desc)) && desc.Width == width && desc.Height == height &&
          desc.Format == D3DFMT_A8R8G8B8, "decoded source has the expected readable texture geometry");
    D3DLOCKED_RECT lock{};
    Check(SUCCEEDED(texture->LockRect(0, &lock, nullptr, D3DLOCK_READONLY)), "lock retained-source texture");
    for (int y = 0; y < height; y += 7) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
        for (int x = 0; x < width; x += 11)
            Check((row[x] & 0xFFFFFF) == DWORD(((32 + x) << 16) | ((32 + y * 2) << 8) | (40 + frame * 100)),
                  "retained source reproduces its exact original BMP pixels");
    }
    texture->UnlockRect(0); texture->Release();
}
void CheckLoaderBudget(MMDApp* app, const MediaFiles& files) {
    auto& budget = media::GlobalPictureSourceBudget();
    Check(budget.Usage().limit == 128 * MiB, "missing resource budget field selects the documented default");
    Check(system_settings::WriteSection("resources", "{\"backgroundPictureSourcesMiB\":16}") == system_settings::Result::Ok,
          "configure real loader's isolated sixteen-MiB preference");
    Check(TryReplaceBackgroundPicture(app, files.bitmap.c_str()), "load small baseline under configured source budget");
    Check(budget.Usage().limit == 16 * MiB, "real loader applies current settings to the global budget");
    const auto rejectPreserving = [&](const wchar_t* path) {
        auto* const texture = app->PictureBackgroundTexture();
        const std::wstring previousPath = app->PictureBackgroundPath();
        const auto x = app->PictureOffsetX(), y = app->PictureOffsetY();
        const auto scale = app->PictureScale();
        const auto w = app->PictureWidth(), h = app->PictureHeight();
        const auto enabled = app->PictureBackgroundEnabled();
        const auto dirty = app->SceneModified();
        const auto instance = plugins::background_identity::Instance(app);
        const auto ticket = plugins::background_identity::Acquire(app, plugins::background_identity::Kind::Picture);
        const auto source = plugins::background_identity::CurrentPictureSource(app, instance, ticket);
        const auto asset = plugins::background_identity::CurrentPictureAsset(app, instance, ticket);
        const auto usage = budget.Usage();
        Check(source && asset && asset->Id() && asset->Source() == source && usage.used >= source->RetainedBytes(),
              "published baseline asset retains the source charged to the shared budget");
        const auto assetId = asset->Id();
        errorDialogCount = 0;
        HHOOK hook = SetWindowsHookExW(WH_CBT, CloseErrorDialog, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "close only the budget test thread's expected error dialog");
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
        const auto oldImport = __imp_D3DXCreateTextureFromFileInMemoryEx;
        budgetDecodeCalls = 0; WriteImport(&UnexpectedBudgetDecode);
#endif
        const bool loaded = TryReplaceBackgroundPicture(app, path);
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
        WriteImport(oldImport); d3dx::Get().Load();
        Check(budgetDecodeCalls == 0, "budget or invalid settings rejects before either D3DX decode attempt");
#endif
        UnhookWindowsHookEx(hook);
        Check(!loaded && errorDialogCount == 1, "rejected source import reports exactly one error");
        Check(app->PictureBackgroundTexture() == texture && previousPath == app->PictureBackgroundPath() &&
              app->PictureOffsetX() == x && app->PictureOffsetY() == y && app->PictureScale() == scale &&
              app->PictureWidth() == w && app->PictureHeight() == h && app->PictureBackgroundEnabled() == enabled &&
              app->SceneModified() == dirty &&
              plugins::background_identity::CurrentPictureSource(app, instance, ticket) == source &&
              plugins::background_identity::CurrentPictureAsset(app, instance, ticket) == asset && asset->Id() == assetId &&
              plugins::background_identity::IsCurrent(app, plugins::background_identity::Kind::Picture, ticket),
              "rejected source preserves texture path dimensions placement status identity and source lease");
        const auto after = budget.Usage();
        Check(after.used == usage.used && after.limit == usage.limit,
              "rejected import leaks no charge and invalid configuration cannot alter the budget");
    };
    const auto overhead = media::BackgroundPictureSource::OverheadBytes();
    Check(overhead < MiB && budget.Usage().used > 0, "small existing source makes old-plus-candidate peak measurable");
    WritePaddedBitmap(files.budgetBitmap, 16 * MiB - overhead);
    Check(std::filesystem::file_size(files.budgetBitmap) + overhead == 16 * MiB,
          "first candidate fits the limit alone but cannot coexist with the published source");
    rejectPreserving(files.budgetBitmap.c_str());

    WritePaddedBitmap(files.leaseBitmap, 3 * MiB);
    WritePaddedBitmap(files.budgetBitmap, 14 * MiB);
    const auto beforeExtra = budget.Usage().used;
    media::BackgroundPictureSourcePtr extra;
    Check(media::CaptureBackgroundPictureSource(files.leaseBitmap.c_str(), 16 * MiB, budget, extra) == media::PictureSourceResult::Ok &&
          extra && budget.Usage().used == beforeExtra + extra->RetainedBytes(),
          "independent retained source uses the same global accounting as the loader");
    Check(beforeExtra + 14 * MiB + overhead <= 16 * MiB && budget.Usage().used + 14 * MiB + overhead > 16 * MiB,
          "concurrent lease alone changes this candidate from admissible to over-budget");
    rejectPreserving(files.budgetBitmap.c_str());
    extra.reset();
    Check(budget.Usage().used == beforeExtra, "releasing the extra lease returns its entire charge immediately");
    Check(TryReplaceBackgroundPicture(app, files.budgetBitmap.c_str()), "identical candidate succeeds after the concurrent lease is released");
    const auto ticket = plugins::background_identity::Acquire(app, plugins::background_identity::Kind::Picture);
    const auto source = plugins::background_identity::CurrentPictureSource(app, plugins::background_identity::Instance(app), ticket);
    Check(source && source->Size() == 14 * MiB && budget.Usage().used <= budget.Usage().limit,
          "accepted padded BMP remains correctly charged within the configured limit");
    CheckSourcePixels(app->Renderer()->device, source, 0);
    for (const char* invalid : {"{\"backgroundPictureSourcesMiB\":15}",
                                "{\"backgroundPictureSourcesMiB\":4097}",
                                "{\"backgroundPictureSourcesMiB\":16.5}",
                                "{\"backgroundPictureSourcesMiB\":\"16\"}"}) {
        Check(system_settings::WriteSection("resources", invalid) == system_settings::Result::Ok, "persist isolated invalid resource preference");
        rejectPreserving(files.bitmap.c_str());
    }
    Check(system_settings::WriteSection("resources", "{\"backgroundPictureSourcesMiB\":16}") == system_settings::Result::Ok,
          "restore valid resource preference before settings read-failure case");
    const auto settings = system_settings::Path();
    HANDLE locked = CreateFileW(settings.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold only isolated settings file unreadable for this import");
    rejectPreserving(files.bitmap.c_str());
    CloseHandle(locked);
    std::puts("PASS actual picture loader: shared source peaks, lease release, invalid settings and unreadable settings");
}
#endif

void CheckPixels(MMDApp* app, int frame, float left, float top, float right, float bottom) {
    auto* device = app->Renderer()->device;
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF00FF, 1, 0)), "clear render target");
    Check(SUCCEEDED(device->BeginScene()), "begin background render");
    RenderModelsFixed(app);
    Check(SUCCEEDED(device->EndScene()), "end background render");
    IDirect3DSurface9 *back = nullptr, *read = nullptr;
    Check(SUCCEEDED(device->GetRenderTarget(0, &back)), "get background render target");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(192, 128, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &read, nullptr)), "create background readback");
    Check(SUCCEEDED(device->GetRenderTargetData(back, read)), "read rendered background pixels");
    D3DLOCKED_RECT lock{};
    Check(SUCCEEDED(read->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock rendered background");
    int checked = 0;
    for (int y = static_cast<int>(top) + 5; y < bottom - 5; y += 7) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
        for (int x = static_cast<int>(left) + 5; x < right - 5; x += 7) {
            const float sourceX = (x - left) / (right - left) * width - .5f;
            const float sourceY = (y - top) / (bottom - top) * height - .5f;
            const int red = (row[x] >> 16) & 255, green = (row[x] >> 8) & 255, blue = row[x] & 255;
            if (std::abs(red - (32 + sourceX)) > 4 || std::abs(green - (32 + sourceY * 2)) > 4 ||
                std::abs(blue - (40 + frame * 100)) > 2) {
                std::fprintf(stderr, "pixel (%d,%d)=%08lx expected RGB approx %.1f,%.1f,%d\n",
                             x, y, row[x], 32 + sourceX, 32 + sourceY * 2, 40 + frame * 100);
                Check(false, "both background triangles cover the rectangle with undistorted UVs");
            }
            ++checked;
        }
    }
    const auto* outside = static_cast<const DWORD*>(lock.pBits);
    Check((outside[0] & 0xFFFFFF) == 0xFF00FF, "background placement leaves outside pixels untouched");
    read->UnlockRect(); read->Release(); back->Release();
    std::printf("PASS background frame %d: %d pixel samples\n", frame, checked);
}
#if !MMD_ORIGINAL_PROFILE
#include "background_picture_activation_checks.hpp"
#endif
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if !MMD_ORIGINAL_PROFILE
    BudgetSettings budgetSettings(argc, argv);
#else
    Check(argc == 1, "original profile runs the unchanged overlay suite without ABI budget settings");
#endif
    MediaFiles files;
    HWND window = CreateWindowW(L"STATIC", L"Background media regression", WS_POPUP,
        0, 0, 192, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden render window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get(); app->Hwnd() = window; app->EnglishUI() = 1;
    auto& pp = renderer->presentParameters;
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 192; pp.BackBufferHeight = 128; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &renderer->device)), "create device");
    auto* device = renderer->device;
    renderer->viewScale = 1.2f;
    app->ViewportRect() = {8, 10, 168, 106};
    for (auto** buffer : {&app->PictureOverlayVertices(), &app->AviOverlayVertices()}) {
        Check(SUCCEEDED(device->CreateVertexBuffer(168, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_MANAGED, buffer, nullptr)), "create background vertices");
        void* data = nullptr; (*buffer)->Lock(0, 0, &data, 0);
        std::memset(data, 0, 168); (*buffer)->Unlock();
    }
    wcscpy_s(app->PictureBackgroundPath(), 256, files.bitmap.c_str());
    LoadBackgroundPicture(app.get());
    Check(app->PictureBackgroundEnabled() && app->PictureWidth() == width && app->PictureHeight() == height,
          "actual BMP loader reads dimensions and enables background");
    CheckPixels(app.get(), 0, 7, 9, 168, 106);
    app->PictureOffsetX() = 20; app->PictureOffsetY() = 15; app->PictureScale() = .5f;
    PicBgOverlayRefresh(app.get());
    CheckPixels(app.get(), 0, 27, 24, 108, 73);
#if !MMD_ORIGINAL_PROFILE
    Check(plugins::background_identity::Register(app.get()), "register picture test owner");
    auto* previousPicture = app->PictureBackgroundTexture();
    Check(TryReplaceBackgroundPicture(app.get(), files.bitmap.c_str()), "replace picture with a private candidate");
    Check(app->PictureBackgroundTexture() && app->PictureBackgroundTexture() != previousPicture,
          "replacement publishes a new texture before retiring the old one");
    const auto* keptPicture = app->PictureBackgroundTexture();
    const std::wstring keptPath = app->PictureBackgroundPath();
    const auto keptX = app->PictureOffsetX(), keptY = app->PictureOffsetY();
    const float keptScale = app->PictureScale();
    const auto keptWidth=app->PictureWidth(), keptHeight=app->PictureHeight();
    const auto keptEnabled=app->PictureBackgroundEnabled();
    const std::uint8_t keptDirty=app->SceneModified();
    const auto keptTicket = plugins::background_identity::Acquire(app.get(), plugins::background_identity::Kind::Picture);
    Check(keptTicket != 0, "acquire picture identity");
    const auto keptInstance = plugins::background_identity::Instance(app.get());
    const auto keptSource = plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, keptTicket);
    const auto keptAsset = plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, keptTicket);
    Check(keptAsset && keptAsset->Id() && keptAsset->Source() == keptSource &&
          std::wstring(keptAsset->Path()) == keptPath && keptAsset->Width() == width && keptAsset->Height() == height,
          "actual replacement publishes a stable asset with resolved path original dimensions and the same retained source");
    Check(keptAsset->DecodeMode() == media::PictureDecodeMode::Primary1024,
          "ordinary actual decode records the primary restoration recipe");
    const auto keptAssetId = keptAsset->Id();
    CheckSourcePixels(device, keptSource, 0);
    const auto badPath = files.bitmap + L".bad.bmp";
    FILE* badFile = nullptr;
    Check(_wfopen_s(&badFile, badPath.c_str(), L"wb") == 0, "create invalid picture fixture");
    Check(std::fwrite("bad", 1, 3, badFile) == 3, "write invalid picture fixture");
    std::fclose(badFile);
    HHOOK errorHook = SetWindowsHookExW(WH_CBT, CloseErrorDialog, nullptr, GetCurrentThreadId());
    Check(errorHook != nullptr, "install error dialog hook");
    const bool badLoaded = TryReplaceBackgroundPicture(app.get(), badPath.c_str());
    UnhookWindowsHookEx(errorHook);
    DeleteFileW(badPath.c_str());
    Check(!badLoaded, "reject an invalid existing image");
    Check(plugins::background_identity::IsCurrent(app.get(), plugins::background_identity::Kind::Picture, keptTicket),
          "failed decode preserves picture identity");
    Check(plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, keptTicket) == keptSource,
          "failed decode preserves the exact published source lease");
    Check(plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, keptTicket) == keptAsset &&
          keptAsset->Id() == keptAssetId, "failed decode preserves the exact published asset and its stable identity");
    // The now-deleted fixture is a valid-length but unresolvable path. Count
    // the real modal dialog and close it on activation so this also catches a
    // regression where resolution fails silently before the decoder runs.
    Check(GetFileAttributesW(badPath.c_str())==INVALID_FILE_ATTRIBUTES, "missing picture fixture remains absent");
    errorDialogCount=0;
    errorHook=SetWindowsHookExW(WH_CBT,CloseErrorDialog,nullptr,GetCurrentThreadId());
    Check(errorHook!=nullptr,"install missing picture error hook");
    const bool missingLoaded=TryReplaceBackgroundPicture(app.get(),badPath.c_str());
    UnhookWindowsHookEx(errorHook);
    Check(!missingLoaded && errorDialogCount==1,"unresolvable picture reports exactly one error");
    Check(app->PictureBackgroundTexture()==keptPicture && keptPath==app->PictureBackgroundPath() &&
          app->PictureWidth()==keptWidth && app->PictureHeight()==keptHeight &&
          app->PictureOffsetX()==keptX && app->PictureOffsetY()==keptY && app->PictureScale()==keptScale &&
          app->PictureBackgroundEnabled()==keptEnabled && app->SceneModified()==keptDirty &&
          plugins::background_identity::IsCurrent(app.get(),plugins::background_identity::Kind::Picture,keptTicket),
          "unresolvable picture leaves prior content, placement, status and identity unchanged");
    Check(plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, keptTicket) == keptSource,
          "missing input preserves the exact published source lease");
    Check(plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, keptTicket) == keptAsset &&
          keptAsset->Id() == keptAssetId, "missing input preserves the exact published asset and its stable identity");
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
    reentryApp = app.get(); reentryCalls = 0;
    const auto oldImport = __imp_D3DXCreateTextureFromFileInMemoryEx;
    WriteImport(&ReentrantPictureFailure);
    const bool reenteredLoaded = TryReplaceBackgroundPicture(app.get(), files.bitmap.c_str());
    WriteImport(oldImport);
    d3dx::Get().Load();
    reentryApp = nullptr;
    Check(!reenteredLoaded && reentryCalls == 1, "reentrant decoder failure does not retry a stale candidate");
    Check(app->PictureBackgroundTexture() == keptPicture && keptPath == app->PictureBackgroundPath() &&
          app->PictureOffsetX() == keptX && app->PictureOffsetY() == keptY && app->PictureScale() == keptScale,
          "reentrant decode keeps the currently published picture");
#endif
    const std::wstring tooLong(300, L'x');
    Check(!TryReplaceBackgroundPicture(app.get(), tooLong.c_str()), "reject oversized picture path");
    Check(app->PictureBackgroundTexture() == keptPicture && keptPath == app->PictureBackgroundPath() &&
          app->PictureOffsetX() == keptX && app->PictureOffsetY() == keptY && app->PictureScale() == keptScale,
          "failed replacement keeps texture, path and placement");

#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
    fallbackCalls = 0; fallbackBytes = nullptr; fallbackSize = 0;
    fallbackDecoder = __imp_D3DXCreateTextureFromFileInMemoryEx;
    WriteImport(&ForcePictureFallback);
    const bool fallbackLoaded = TryReplaceBackgroundPicture(app.get(), files.bitmap.c_str());
    WriteImport(fallbackDecoder); d3dx::Get().Load(); fallbackDecoder = nullptr;
    Check(fallbackLoaded && fallbackCalls == 2, "first decoder failure falls back exactly once to a real successful decode");
    const auto fallbackTicket = plugins::background_identity::Acquire(app.get(), plugins::background_identity::Kind::Picture);
    const auto fallbackAsset = plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, fallbackTicket);
    Check(fallbackAsset && fallbackAsset->Id() && fallbackAsset->Id() != keptAssetId &&
          fallbackAsset->DecodeMode() == media::PictureDecodeMode::Fallback512 &&
          fallbackAsset->Width() == width && fallbackAsset->Height() == height &&
          std::wstring(fallbackAsset->Path()) == app->PictureBackgroundPath() &&
          fallbackAsset->Source() == plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, fallbackTicket),
          "successful retry publishes its own asset identity with the actual fallback recipe and source metadata");
    D3DSURFACE_DESC fallbackTexture{};
    Check(SUCCEEDED(app->PictureBackgroundTexture()->GetLevelDesc(0, &fallbackTexture)) &&
          fallbackTexture.Width == 512 && fallbackTexture.Height == 512,
          "published texture really comes from the 512-pixel fallback decode");
    CheckSourcePixels(device, fallbackAsset->Source(), 0);
    CheckPixels(app.get(), 0, 7 + app->PictureOffsetX(), 9 + app->PictureOffsetY(),
                8 + app->PictureOffsetX() + width * app->PictureScale(),
                10 + app->PictureOffsetY() + height * app->PictureScale());
#endif

    // Work on a separate BMP so path deletion/rewriting cannot alter the
    // independent bitmap and AVI fixtures used by the existing render checks.
    WriteBitmap(files.sourceBitmap, 0);
    Check(TryReplaceBackgroundPicture(app.get(), files.sourceBitmap.c_str()), "publish separately owned source fixture");
    const auto firstTicket = plugins::background_identity::Acquire(app.get(), plugins::background_identity::Kind::Picture);
    auto firstSource = plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, firstTicket);
    const auto firstAsset = plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, firstTicket);
    Check(firstSource && firstSource != keptSource && firstTicket != keptTicket,
          "replacing picture publishes an independent source and media identity");
    Check(firstAsset && firstAsset != keptAsset && firstAsset->Id() && firstAsset->Id() != keptAssetId &&
          firstAsset->Source() == firstSource && std::wstring(firstAsset->Path()) == app->PictureBackgroundPath() &&
          firstAsset->Width() == width && firstAsset->Height() == height &&
          firstAsset->DecodeMode() == media::PictureDecodeMode::Primary1024,
          "new picture receives its own asset identity while preserving actual source metadata");
    const auto firstAssetId = firstAsset->Id();
    const std::wstring firstAssetPath(firstAsset->Path());
    CheckSourcePixels(device, keptSource, 0);
    CheckSourcePixels(device, firstSource, 0);
    const auto* firstData = firstSource->Data();
    const auto firstSize = firstSource->Size();
    const auto firstRetained = firstSource->RetainedBytes();
    Check(DeleteFileW(files.sourceBitmap.c_str()) != FALSE &&
          GetFileAttributesW(files.sourceBitmap.c_str()) == INVALID_FILE_ATTRIBUTES,
          "delete only the independent source fixture after successful capture");
    CheckSourcePixels(device, firstSource, 0);
    WriteBitmap(files.sourceBitmap, 1);
    CheckSourcePixels(device, firstSource, 0);
    Check(TryReplaceBackgroundPicture(app.get(), files.sourceBitmap.c_str()), "reload changed BMP at the same path");
    const auto secondTicket = plugins::background_identity::Acquire(app.get(), plugins::background_identity::Kind::Picture);
    const auto secondSource = plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, secondTicket);
    const auto secondAsset = plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, secondTicket);
    Check(secondTicket != firstTicket && secondSource && secondSource != firstSource &&
          secondSource->Data() != firstData,
          "same-path reload captures a distinct immutable source instead of aliasing old bytes");
    Check(secondAsset && secondAsset != firstAsset && secondAsset->Id() && secondAsset->Id() != firstAssetId &&
          secondAsset->Source() == secondSource && std::wstring(secondAsset->Path()) == firstAssetPath &&
          secondAsset->Width() == width && secondAsset->Height() == height &&
          secondAsset->DecodeMode() == media::PictureDecodeMode::Primary1024,
          "same-path reload publishes a distinct asset identity with the same resolved path and original dimensions");
    const auto secondAssetId = secondAsset->Id();
    Check(!plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, firstTicket),
          "old media ticket cannot query the replacement source");
    Check(!plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, firstTicket),
          "retired activation ticket cannot retrieve either the old or replacement asset");
    Check(!plugins::background_identity::CurrentPictureSource(app.get(), 0, secondTicket) &&
          !plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, 0) &&
          !plugins::background_identity::CurrentPictureSource(nullptr, keptInstance, secondTicket),
          "source queries reject missing owner identity, media identity and application");
    Check(firstSource->Data() == firstData && firstSource->Size() == firstSize &&
          firstSource->RetainedBytes() == firstRetained,
          "old source lease retains stable bytes and accounting across file changes and reload");
    CheckSourcePixels(device, firstSource, 0);
    CheckSourcePixels(device, secondSource, 1);
    {
        plugins::background_identity::Mutation pending(app.get(), true, false, false, true);
        Check(!plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, secondTicket),
              "current source query is unavailable while its picture is being mutated");
        Check(!plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, secondTicket),
              "current asset query is unavailable during picture mutation");
        CheckSourcePixels(device, secondSource, 1);
    }
    Check(plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, secondTicket) == secondSource,
          "unpublished failed candidate restores query access to its unchanged source");
    Check(plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, secondTicket) == secondAsset &&
          secondAsset->Id() == secondAssetId, "unpublished candidate preserves the same asset and identity");
    plugins::background_identity::Invalidate(app.get(), plugins::background_identity::Kind::Picture);
    Check(!plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, secondTicket),
          "invalidated identity cannot query retained picture data");
    Check(!plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, secondTicket),
          "invalidated activation cannot retrieve an old asset");
    const auto emptyTicket = plugins::background_identity::Acquire(app.get(), plugins::background_identity::Kind::Picture);
    Check(emptyTicket && emptyTicket != secondTicket &&
          !plugins::background_identity::CurrentPictureSource(app.get(), keptInstance, emptyTicket),
          "allocating a fresh identity does not resurrect an invalidated source");
    Check(!plugins::background_identity::CurrentPictureAsset(app.get(), keptInstance, emptyTicket),
          "fresh activation ticket cannot resurrect an invalidated asset");
    CheckSourcePixels(device, firstSource, 0);
    CheckSourcePixels(device, secondSource, 1);
    firstSource.reset();
    Check(firstAsset->Id() == firstAssetId && std::wstring(firstAsset->Path()) == firstAssetPath &&
          firstAsset->Source()->Data() == firstData && firstAsset->Source()->Size() == firstSize,
          "retired asset independently keeps its original bytes and metadata after the separate source lease is released");
    CheckSourcePixels(device, firstAsset->Source(), 0);
    Check(secondAsset->Id() == secondAssetId && secondAsset->Source() == secondSource,
          "external asset lease remains valid after registry invalidation");
    CheckSourcePixels(device, secondAsset->Source(), 1);
    if (budgetSettings.enabled) CheckLoaderBudget(app.get(), files);
    if (argc == 3 && !std::wcscmp(argv[1], L"--picture-activation")) {
        CheckPictureActivation(app.get(), d3d, files);
        Check(!std::filesystem::exists(system_settings::Path().parent_path()),
              "activation-only checks create no installation data or settings files");
    }
#endif
    app->PictureBackgroundEnabled() = 0;
    wcscpy_s(app->AviBackgroundPath(), 256, files.video.c_str());
    LoadAviFile(app.get());
    Check(app->AviBackgroundEnabled() && app->AviFrameWidth() == width && app->AviFrameHeight() == height,
          "actual AVI loader reads dimensions and enables background");
    CheckPixels(app.get(), 0, 7, 9, 168, 106);
    app->CurrentFrame() = 1; AviBgOverlayRefresh(app.get());
    CheckPixels(app.get(), 1, 7, 9, 168, 106);
    app->AviOffsetX() = 20; app->AviOffsetY() = 15; app->AviScale() = .5f;
    // Placement must reuse the displayed frame, even if the timeline changed.
    app->CurrentFrame() = 0;
    AviBgPlacementRefresh(app.get());
    CheckPixels(app.get(), 1, 27, 24, 108, 73);
    AVIStreamGetFrameClose(static_cast<PGETFRAME>(app->AviFrameReader()));
    AVIStreamRelease(static_cast<PAVISTREAM>(app->AviStream()));
    AVIFileRelease(static_cast<PAVIFILE>(app->AviFile()));
    if (app->AviDrawDib()) DrawDibClose(static_cast<HDRAWDIB>(app->AviDrawDib()));
    app->PictureBackgroundTexture()->Release(); app->AviBackgroundTexture()->Release();
    app->AviBackgroundSurface()->Release();
    app->PictureOverlayVertices()->Release(); app->AviOverlayVertices()->Release();
#if !MMD_ORIGINAL_PROFILE
    { plugins::background_identity::Mutation closing(app.get(), true, true, true); }
#endif
    device->Release(); renderer->device = nullptr; d3d->Release(); DestroyWindow(window);
    std::puts("Background BMP/AVI loading, frame advance, placement and complete-quad pixel regressions passed.");
}
