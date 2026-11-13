#include "mikumikustudio/original_import.hpp"
// ===========================================================================
// VA 0x0042AE20 - CopyDirPathW         (original: sub_42AE20, 16 bytes)
// VA 0x0042AE40 - CopyPathW         (original: sub_42AE40, 23 bytes)
// VA 0x00433250 - LoadAviFile       (original: sub_433250, 0x53F bytes)
// VA 0x004337A0 - LoadBackgroundPicture (original: sub_4337A0, 0x293 bytes)
// ===========================================================================
// The background-media loaders behind menu 0xD5 ("load background AVI
// file") and menu 0xE8 (case 232, "load background picture file"), plus
// the two path-copy thunks both flows use (wcscpy_s with a fixed count:
// 0x3E8 for the UserFile dir buffers, 0x100 for the media path buffers).
//
// 0x433250/0x4337A0 are __thiscall(app); like 0x418500 the file path is
// read from app storage (app+0x9E1EC for AVI, app+0x9E448 for pictures),
// so these app-taking overloads supersede the old path-taking stub
// declarations (those twins have since been deleted from stubs.cpp).
//
// AVI uses the raw VFW API (AVIFIL32): open -> AVIFileInfoA -> pick the
// 'vids' stream with the LOWEST wPriority (initial best 0xFFFF) ->
// AVIStreamGetFrameOpen -> AVIStreamGetFrame(0) for the BITMAPINFOHEADER.
// The picture loader goes through D3DXCreateTextureFromFileExW (1024x1024,
// D3DPOOL_MANAGED; retry 512x512/1 mip level), storing the texture at
// app+0x9E42C like the original.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/native_media.hpp"
#include <algorithm>
#include <cmath>
#include <utility>
#include <Windows.h>
#if !MMD_ORIGINAL_PROFILE
#include "../plugins/background_identity.hpp"
#include "../plugins/native_picture_gesture.hpp"
#include "mikumikustudio/background_picture_source.hpp"
#include "mikumikustudio/background_picture_policy.hpp"
#include "mikumikustudio/background_picture_load.hpp"
#include "background_picture_decode.hpp"
#endif
#include <d3d9.h>
#include <vfw.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {
namespace {

// 0x52BA94: "AVIデータ読込"
static const char kMsgAviCaptionJp[] =
    "AVI\x83\x66\x81\x5B\x83\x5E\x93\xC7\x8D\x9E";
// 0x52CA3C: "AVIファイルが読み込めません"
static const char kMsgAviOpenFailJp[] =
    "AVI\x83\x74\x83\x40\x83\x43\x83\x8B\x82\xAA\x93\xC7\x82\xDD\x8D\x9E"
    "\x82\xDF\x82\xDC\x82\xB9\x82\xF1";
// 0x52BA28: "AVIファイルを読み込めません"
static const char kMsgAviOpenFail2Jp[] =
    "AVI\x83\x74\x83\x40\x83\x43\x83\x8B\x82\xF0\x93\xC7\x82\xDD\x8D\x9E"
    "\x82\xDF\x82\xDC\x82\xB9\x82\xF1";
// 0x52CA04: "AVIFILEINFOを読み込めません"
static const char kMsgAviInfoFailJp[] =
    "AVIFILEINFO\x82\xF0\x93\xC7\x82\xDD\x8D\x9E\x82\xDF\x82\xDC\x82\xB9"
    "\x82\xF1";
// 0x52C9D4: "AVIFileGetStream失敗"
static const char kMsgAviGetStreamFailJp[] =
    "AVIFileGetStream\x8E\xB8\x94\x73";
// 0x52C9A8: "AVIStreamInfo失敗"
static const char kMsgAviStreamInfoFailJp[] =
    "AVIStreamInfo\x8E\xB8\x94\x73";
// 0x52CA70: "BMPファイルを読み込めませんでした"
static const char kMsgBmpFailJp[] =
    "BMP\x83\x74\x83\x40\x83\x43\x83\x8B\x82\xF0\x93\xC7\x82\xDD\x8D\x9E"
    "\x82\xDF\x82\xDC\x82\xB9\x82\xF1\x82\xC5\x82\xB5\x82\xBD";
// 0x52CA94: "BMPファイル読込"
static const char kMsgBmpCaptionJp[] = "BMP\x83\x74\x83\x40\x83\x43\x83\x8B"
                                       "\x93\xC7\x8D\x9E";

// Complete D3DXIMAGE_INFO ABI: the loader writes all seven fields, even
// though placement only consumes width and height.
struct ImgInfo {
    UINT Width, Height, Depth, MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    UINT ImageFileFormat;
};
static_assert(sizeof(ImgInfo) == 28);

// x64 端纵横比数学用的 .rdata float 常量（SSE 单精度乘除，
// sub_7FF7CB487A20 @0x7ff7cb487ed3/0x7ff7cb487f03 引用）。
constexpr float kOverlaySpan = 1.2f;   // float @0x7FF7CB552B3C (3F 99 99 9A)
constexpr float kOverlayHalf = 0.5f;   // float @0x7FF7CB55298C (3F 00 00 00)

// 0x50747B: CRT _wsopen_s(&fh, path, _O_BINARY, _SH_DENYNO, _S_IREAD);
// used as an existence probe (returns errno, 0 = exists).
bool WsOpenOk(const wchar_t* path) {
    int fh = -1;
    if (_wsopen_s(&fh, path, _O_BINARY, _SH_DENYNO, _S_IREAD) == 0) {
        _close(fh);
        return true;
    }
    return false;
}

// 背景媒体（AVI/图片）的纵横比参数，AVI 路径与图片路径共用。
// x64 定谳（全 SSE 单精度，求值序即下式书写序）：
//   AVI   sub_7FF7CB487A20 @0x7ff7cb487ec5..0x7ff7cb487f1b
//   图片  sub_7FF7CB488970 @0x7ff7cb488bf2..0x7ff7cb488c55（同式）
//   zoom = ((hideRectW / viewScale) * 1.2f) / mediaW     -> 写 app+652036
//   posY = h2 - ((mediaH * zoom * viewScale) / 1.2f) * 0.5f  -> 写 app+652032
//   h2 = (hideBottom - hideTop) / 2（整数除法）
// 物理含义：媒体恰好铺满视口宽并垂直居中——消费端 ComputeOverlayRect
// (src/render/bg_overlay.cpp) 的 W*zoom*ratio/1.2 恰好还原出 hideRectW，
// 即本 zoom 正是该公式所需。
// 历史注记：旧版写成 scale/offsetY 双倒数+反号，根因是 x86 x87 的
// fdivrp/fsubrp 反操作数伪影误导；x64 SSE 版（divss/mulss/subss 操作
// 数顺序直观可见）证明上述才是原意。
// 另：hideLeft 实为右缘、hideRight 实为左缘（见 mmd_app.hpp 的 RECT
// 视图），故 hideLeft - hideRight 是正的视口宽；h2 同理取正半高。
void MediaAspect(MMDApp* app, std::int32_t width, std::int32_t height,
                 float& scaleOut, std::int32_t& posOut) {
    auto& s = *app;
    const std::int32_t w = s.state.hideLeft - s.state.hideRight;   // 0x4336B7
    const std::int32_t h2 =
        (s.state.hideBottom - s.state.hideTop) / 2;                // 0x4336E9
    const float ratio = s.Renderer()->viewScale;                   // 0x4336E0
    const float spanW = static_cast<float>(w) / ratio;             // 0x4336F5..
    const float zoom = (spanW * kOverlaySpan) / static_cast<float>(width);
    const float drawnH =
        (static_cast<float>(height) * zoom * ratio) / kOverlaySpan;
    scaleOut = zoom;
    posOut = static_cast<std::int32_t>(
        static_cast<float>(h2) - drawnH * kOverlayHalf);           // 0x43370D..
}

}  // namespace

// ---------------------------------------------------------------------------
// VA 0x0042AE20 - CopyDirPathW(dest, src): wcscpy_s(dest, 0x3E8, src) thunk
// for the wchar_t[1000] UserFile directory buffers.
// ---------------------------------------------------------------------------
void CopyDirPathW(wchar_t* dest, const wchar_t* src) {  // was Sub42AE20
    wcscpy_s(dest, 0x3E8, src);                                     // 0x506292
}

// ---------------------------------------------------------------------------
// VA 0x0042AE40 - CopyPathW(dest, src): wcscpy_s(dest, 0x100, src) thunk
// for the wchar_t[256] media path buffers.  (Supersedes the was-Sub42AE40
// stub, since deleted from stubs.cpp.)
// ---------------------------------------------------------------------------
void CopyPathW(wchar_t* dest, const wchar_t* src) {
    wcscpy_s(dest, 0x100, src);                                     // 0x506292
}

namespace {

struct AviInput {
    PAVIFILE file = nullptr;
    PAVISTREAM stream = nullptr;
    PGETFRAME reader = nullptr;

    ~AviInput() {
        if (reader) AVIStreamGetFrameClose(reader);
        if (stream) AVIStreamRelease(stream);
        if (file) AVIFileRelease(file);
    }
};

void SwapAviInput(MMDApp& app, AviInput& input) {
    auto* file = static_cast<PAVIFILE>(app.AviFile());
    auto* stream = static_cast<PAVISTREAM>(app.AviStream());
    auto* reader = static_cast<PGETFRAME>(app.AviFrameReader());
    app.AviFile() = input.file;
    app.AviStream() = input.stream;
    app.AviFrameReader() = input.reader;
    input.file = file;
    input.stream = stream;
    input.reader = reader;
}

void ClearVideoSurface(MMDApp& app) {
    if (auto* surface = app.AviBackgroundSurface()) {
        app.AviBackgroundSurface() = nullptr;
        surface->Release();
    }
}

void ClearProjectVideo(MMDApp& app) {
    media::ReleaseVideo(&app);
    AviInput retired;
    SwapAviInput(app, retired);
    ClearVideoSurface(app);
    app.AviBackgroundPath()[0] = L'\0';
    app.AviBackgroundEnabled() = 0;
    CheckMenuItem(GetMenu(app.MainWindow()), 0xD8, MF_UNCHECKED);
}

bool PrepareAvi(MMDApp& app, const wchar_t* path, AviInput& input, int& width, int& height) {
    const bool english = app.EnglishUI() != 0;
    const auto fail = [&](const char* en, const char* jp) {
        MessageBoxA(app.MainWindow(), english ? en : jp, english ? "open AVI file" : kMsgAviCaptionJp, 0);
        return false;
    };
    if (AVIFileOpenW(&input.file, path, OF_READ | OF_SHARE_DENY_NONE, nullptr) != 0)
        return fail("Cannot open AVI file!!", kMsgAviOpenFailJp);
    AVIFILEINFOA info{};
    if (AVIFileInfoA(input.file, &info, sizeof(info)) != 0)
        return fail("Cannot read AVIFILEINFO!!", kMsgAviInfoFailJp);

    // Keep the video stream with the lowest priority value.
    unsigned short priority = 0xFFFF;
    for (DWORD i = 0; i < info.dwStreams; ++i) {
        AviInput candidate;
        if (AVIFileGetStream(input.file, &candidate.stream, 0, i) != 0)
            return fail("Failed AVIFileGetStream", kMsgAviGetStreamFailJp);
        AVISTREAMINFOA streamInfo{};
        if (AVIStreamInfoA(candidate.stream, &streamInfo, sizeof(streamInfo)) != 0)
            return fail("Failed AVIStreamInfo", kMsgAviStreamInfoFailJp);
        if (streamInfo.fccType == streamtypeVIDEO && (!input.stream || streamInfo.wPriority < priority)) {
            std::swap(input.stream, candidate.stream);
            priority = streamInfo.wPriority;
        }
    }
    if (input.stream)
        input.reader = AVIStreamGetFrameOpen(input.stream, nullptr);
    if (!input.reader)
        return fail("Cannot open AVI file", kMsgAviOpenFail2Jp);
    const auto* frame = static_cast<const BITMAPINFOHEADER*>(AVIStreamGetFrame(input.reader, 0));
    if (!frame || frame->biWidth <= 0 || frame->biHeight == 0)
        return fail("Cannot open AVI file", kMsgAviOpenFail2Jp);
    width = frame->biWidth;
    height = frame->biHeight;
    return true;
}

bool ReplaceVideoCandidate(MMDApp& app, const wchar_t* input) {
#if MMD_ORIGINAL_PROFILE
    if (!original_import::Reference(input, app.Hwnd(), app.EnglishUI())) return false;
#endif
    auto paths = app.PathWorkspace();
    const wchar_t* path = ResolveUserFilePath(paths, input);
    // Keep the requested extension when the resolver finds no file, so the
    // corresponding importer can report the failure.
    if (!*path) path = input;
#if MMD_ORIGINAL_PROFILE
    if (!original_import::Reference(path, app.Hwnd(), app.EnglishUI())) return false;
    if (!media::IsVideoPath(path)) {
        MessageBoxW(app.MainWindow(), L"This MMD 9.32 profile supports AVI backgrounds only.",
                    L"AVI import", MB_OK | MB_ICONERROR);
        return false;
    }
#endif

    int width = 0, height = 0;
    LONG start = 0, end = 0;
    bool thirtyFps = true;
    AviInput avi;
#if !MMD_ORIGINAL_PROFILE
    const bool modern = media::IsModernVideoPath(path);
    if (modern) {
        double duration = 0;
        std::wstring error;
        if (!media::OpenVideo(&app, path, width, height, duration, error, 1024)) {
            MessageBoxW(app.MainWindow(), error.c_str(), L"Video import", MB_OK | MB_ICONERROR);
            return false;
        }
        end = static_cast<LONG>(std::min(2147483646.0, std::ceil(duration * 30.0)));
    } else
#endif
    {
        if (!PrepareAvi(app, path, avi, width, height)) return false;
        start = AVIStreamStart(avi.stream);
        end = start + AVIStreamLength(avi.stream);
        thirtyFps = AVIStreamSampleToTime(avi.stream, 1) == 33;
    }

#if !MMD_ORIGINAL_PROFILE
    plugins::background_identity::Mutation mutation(&app, false, true);
    plugins::CancelPictureGesture(app);
    if (!modern) media::ReleaseVideo(&app);
#endif
    SwapAviInput(app, avi);
    ClearVideoSurface(app);
    CopyPathW(app.AviBackgroundPath(), path);
    app.AviFrameWidth() = width;
    app.AviFrameHeight() = height;
    app.AviStreamStartFrame() = start;
    app.AviStreamEndFrame() = end;
    app.AviUsesThirtyFpsTiming() = thirtyFps;
    app.AviOffsetX() = 0;
    MediaAspect(&app, width, height, app.AviScale(), app.AviOffsetY());
    app.AviBackgroundEnabled() = 1;
    const auto menu = GetMenu(app.MainWindow());
    CheckMenuItem(menu, 0xD8, MF_CHECKED);
    AviBgOverlayRefresh(&app);
    if (!app.AviBackgroundEnabled()) CheckMenuItem(menu, 0xD8, MF_UNCHECKED);
    return app.AviBackgroundEnabled() != 0;
}

}  // namespace

bool TryReplaceBackgroundVideo(MMDApp* app, const wchar_t* path) {
    if (!path || !*path || wcsnlen_s(path, kPathFieldCapacity) == kPathFieldCapacity) return false;
    return ReplaceVideoCandidate(*app, path);
}

// PMM supplies its own path. A failed load must leave this project without a
// background video even when the preceding project had one.
void LoadAviFile(MMDApp* app) {
    wchar_t path[kPathFieldCapacity];
    CopyPathW(path, app->AviBackgroundPath());
#if !MMD_ORIGINAL_PROFILE
    plugins::background_identity::Mutation mutation(app, false, true);
    plugins::CancelPictureGesture(*app);
#endif
    ClearProjectVideo(*app);
    ReplaceVideoCandidate(*app, path);
}

// ---------------------------------------------------------------------------
// VA 0x004337A0 - LoadBackgroundPicture(this=app).  Reads the path from
// app+0x9E448 after UserFile resolution; D3DX texture at app+0x9E42C.
// (Supersedes the was-Sub4337A0 stub, since deleted from stubs.cpp.)
// ---------------------------------------------------------------------------
#if !MMD_ORIGINAL_PROFILE
namespace {
struct ProjectPicturePlacement { std::int32_t x, y; float scale; bool visible, resolutionFailed; };
bool ReplacePictureCandidate(MMDApp* app, const wchar_t* input, const ProjectPicturePlacement* project) {
    // Cancellation precedes the mutation gate: inside it the old picture is
    // intentionally unqueryable, so its still-owned preview cannot be restored.
    if (app) plugins::CancelPictureGesture(*app);
    if (!app || !input || (!input[0] && !project)) return false;
    const auto instance = plugins::background_identity::Instance(app);
    if (!plugins::background_identity::IsOwner(app, instance)) return false;
    auto& s = *app;
    if (!project && !plugins::background_identity::Acquire(app, plugins::background_identity::Kind::Picture)) return false;
    wchar_t request[0x100]{};
    if (wcsncpy_s(request, input, _TRUNCATE) != 0) return false;
    const auto scene = s.SceneGeneration();
    plugins::background_identity::Mutation mutation(app, true, false, false, project == nullptr);
    const auto current = [&]() noexcept {
        return mutation.Current() && plugins::background_identity::IsOwner(app, instance) && s.SceneGeneration() == scene;
    };
    // Project replacement must not expose a previous project's texture when
    // its new picture cannot be prepared. Ordinary imports retain the old one.
    const auto clearProject = [&]() noexcept {
        if (!current()) return false;
        IDirect3DTexture9* previous = nullptr;
        if (!mutation.ClearPicture(app, s.PictureBackgroundTexture(), previous, s.PictureBackgroundPath(),
                s.PictureWidth(), s.PictureHeight(), s.PictureOffsetX(), s.PictureOffsetY(),
                s.PictureScale(), s.PictureBackgroundEnabled())) return false;
        if (previous) previous->Release();
        if (current()) CheckMenuItem(GetMenu(static_cast<HWND>(s.Hwnd())), 0xE9, MF_UNCHECKED);
        return true;
    };
    struct ProjectFailure {
        const decltype(clearProject)& clear;
        bool active;
        ~ProjectFailure() { if (active) clear(); }
    } failure{clearProject, project != nullptr};
    if (!current()) return false;
    if (project && project->resolutionFailed) {
        const bool cleared = clearProject();
        failure.active = false;
        // Releasing the previous texture may reenter. Never report an old
        // project's missing file after a newer scene has taken ownership.
        if (cleared && current()) {
            MessageBoxA(static_cast<HWND>(s.Hwnd()),
                s.EnglishUI() ? "Cannot open BMP file" : kMsgBmpFailJp,
                s.EnglishUI() ? "open BMP file" : kMsgBmpCaptionJp, 0);
        }
        return false;
    }
    if (!request[0]) {
        const bool cleared = clearProject(); failure.active = false; return cleared;
    }
    if (!s.Renderer() || !s.Renderer()->device) return false;
    PathResolutionWorkspace paths = app->PathWorkspace();
    ResolveUserFilePath(paths, request);
    if (!current()) return false;
    wchar_t path[0x100]{};
    HWND hwnd = static_cast<HWND>(s.Hwnd());
    if (wcscpy_s(path, paths.resolvedPath) != 0 || !path[0]) {
        MessageBoxA(hwnd, s.EnglishUI() ? "Cannot open BMP file" : kMsgBmpFailJp,
                    s.EnglishUI() ? "open BMP file" : kMsgBmpCaptionJp, 0);
        return false;
    }
    if (!original_import::Reference(path, hwnd, s.EnglishUI())) return false;
    if (!current()) return false;
    const bool open = WsOpenOk(path);
    if (!current()) return false;
    if (!open) {
        MessageBoxA(hwnd, s.EnglishUI() ? "Cannot open BMP file" : kMsgBmpFailJp,
                    s.EnglishUI() ? "open BMP file" : kMsgBmpCaptionJp, 0);
        return false;
    }
    std::uint64_t sourceLimit = 0;
    if (media::ReadBackgroundPictureSourceLimit(sourceLimit) != system_settings::Result::Ok) {
        if (!current()) return false;
        MessageBoxW(hwnd, project ? (s.EnglishUI() == 2 ? L"无法读取背景图片资源设置，工程图片未载入。" :
            s.EnglishUI() ? L"Cannot read picture resource settings. The project picture was not loaded." :
            L"背景画像のリソース設定を読み込めないため、プロジェクト画像を読み込めませんでした。") :
            (s.EnglishUI() == 2 ? L"无法读取背景图片资源设置，原图片已保留。" :
            s.EnglishUI() ? L"Cannot read picture resource settings. The current picture was kept." :
            L"背景画像のリソース設定を読み込めないため、元の画像を保持しました。"), L"MMS", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!current()) return false;
    // Freeze the encoded source once. Retry and future resource restoration
    // must not decode a different file behind the same path. This owns only
    // encoded bytes; journal and decoded GPU resource budgeting are separate.
    std::shared_ptr<const media::BackgroundPictureSource> source;
    auto capture = media::PictureSourceResult::NoMemory;
    try {
        auto& budget = media::GlobalPictureSourceBudget();
        budget.Configure(sourceLimit);
        capture = media::CaptureBackgroundPictureSource(path, sourceLimit, budget, source);
    } catch (...) { /* A first-use budget allocation cannot cross the legacy UI entry. */ }
    if (!current()) return false;
    if (capture != media::PictureSourceResult::Ok) {
        if (capture == media::PictureSourceResult::TooLarge) {
            MessageBoxW(hwnd, project ? (s.EnglishUI() == 2 ? L"背景图片数据超过允许的内存预算，工程图片未载入。" :
                s.EnglishUI() ? L"Picture source data exceeds the memory budget. The project picture was not loaded." :
                L"背景画像データがメモリ上限を超えたため、プロジェクト画像を読み込めませんでした。") :
                (s.EnglishUI() == 2 ? L"背景图片数据超过允许的内存预算，原图片已保留。" :
                s.EnglishUI() ? L"Picture source data exceeds the memory budget. The current picture was kept." :
                L"背景画像データがメモリ上限を超えたため、元の画像を保持しました。"), L"MMS", MB_OK | MB_ICONWARNING);
            return false;
        }
        MessageBoxA(hwnd, s.EnglishUI() ? "Cannot retain background picture data" : kMsgBmpFailJp,
                    s.EnglishUI() ? "open BMP file" : kMsgBmpCaptionJp, 0);
        return false;
    }
    auto* const renderer = s.Renderer();
    if (!renderer || !renderer->device) return false;
    auto* const device = renderer->device;
    // The app may drop its device or the decoder may pump messages. Hold our
    // own COM reference until all candidate cleanup has completed.
    device->AddRef();
    struct DeviceLease { IDirect3DDevice9* value; ~DeviceLease() { value->Release(); } } lease{device};
    if (!current()) return false;
    IDirect3DTexture9* candidate = nullptr;
    struct CandidateLease { IDirect3DTexture9*& value; ~CandidateLease() { if (value) value->Release(); } } candidateLease{candidate};
    media::picture_detail::ImageInfo info{};
    HRESULT result = E_FAIL;
    auto decodeMode = media::PictureDecodeMode::Primary1024;
    const auto decodeCurrent = [&]() noexcept { return current() && s.Renderer() == renderer && renderer->device == device; };
    bool attempted = false;
    if (!media::picture_detail::DecodeAttempt(device, source, decodeMode, info, candidate, result, decodeCurrent, &attempted))
        return false;
    if (attempted) {
        if (FAILED(result)) {
            auto* abandoned = candidate; candidate = nullptr;
            if (abandoned) abandoned->Release();
            if (!decodeCurrent()) return false;
            info = {};
            decodeMode = media::PictureDecodeMode::Fallback512;
            if (!media::picture_detail::DecodeAttempt(device, source, decodeMode, info, candidate, result, decodeCurrent, &attempted) ||
                !attempted) return false;
        }
    }
    if (FAILED(result) || !candidate || !info.Width || !info.Height ||
        info.Width > INT32_MAX || info.Height > INT32_MAX) {
        MessageBoxA(hwnd, s.EnglishUI() ? "Cannot open BMP file" : kMsgBmpFailJp,
                    s.EnglishUI() ? "open BMP file" : kMsgBmpCaptionJp, 0);
        return false;
    }
    if (!current() ||
        s.Renderer() != renderer || renderer->device != device) {
        return false;
    }
    float scale = 0; std::int32_t y = 0;
    if (project) { scale = project->scale; y = project->y; }
    else {
        if (!media::picture_detail::ImportPlacement(s, *renderer, static_cast<std::int32_t>(info.Width),
            static_cast<std::int32_t>(info.Height), scale, y)) return false;
    }
    media::BackgroundPictureAssetPtr asset;
    if (media::CreateBackgroundPictureAsset(source, path, static_cast<std::int32_t>(info.Width),
            static_cast<std::int32_t>(info.Height), decodeMode, UINT64_MAX, asset) != media::PictureAssetResult::Ok)
        return false;
    if (!current() || s.Renderer() != renderer || renderer->device != device) return false;
    IDirect3DTexture9* previous = nullptr;
    if (!mutation.PublishPicture(app, s.PictureBackgroundTexture(), candidate, previous,
            s.PictureBackgroundPath(), path, s.PictureWidth(), static_cast<std::int32_t>(info.Width),
            s.PictureHeight(), static_cast<std::int32_t>(info.Height), s.PictureOffsetX(), s.PictureOffsetY(),
            s.PictureScale(), y, scale, s.PictureBackgroundEnabled(), asset,
            project ? project->x : 0, project ? static_cast<std::uint8_t>(project->visible) : 1)) {
        return false;
    }
    failure.active = false;
    // Set the project revision before releasing COM resources or dispatching
    // any window messages, which can reenter and replace the current scene.
    if (!project) s.SceneModified() = 1;
    candidate = nullptr; // the app now owns the candidate reference
    if (previous) previous->Release();
    if (current()) {
        const auto menu = GetMenu(hwnd);
        if (current()) CheckMenuItem(menu, 0xE9, !project || project->visible ? MF_CHECKED : MF_UNCHECKED);
        // A hidden project image can be shown without another layout pass.
        // Prepare its new placement now instead of keeping the old quad.
        if (current()) PicBgOverlayRefresh(app);
    }
    return true;
}
}
bool TryReplaceBackgroundPicture(MMDApp* app, const wchar_t* input) {
    return ReplacePictureCandidate(app, input, nullptr);
}
bool LoadProjectBackgroundPicture(MMDApp* app, const wchar_t* path,
    std::int32_t x, std::int32_t y, float scale, bool visible,
    const plugins::background_identity::Mutation& projectScope, std::uint64_t expectedScene,
    bool resolutionFailed) {
    if (!projectScope.AllowsProjectPicture(app) || app->SceneGeneration() != expectedScene) return false;
    const ProjectPicturePlacement placement{x, y, scale, visible, resolutionFailed};
    return ReplacePictureCandidate(app, path, &placement);
}
#endif
void LoadBackgroundPicture(MMDApp* app) {
#if !MMD_ORIGINAL_PROFILE
    plugins::CancelPictureGesture(*app);
    plugins::background_identity::Mutation mediaMutation(app, true, false);
#endif
    auto& s = *app;
    HWND hwnd = static_cast<HWND>(s.Hwnd());
    wchar_t* stored = s.PictureBackgroundPath();
    if (!original_import::Reference(stored, hwnd, s.EnglishUI())) {
        stored[0] = L'\0';
        s.PictureBackgroundEnabled() = 0;
        return;
    }

    PathResolutionWorkspace& paths = app->PathWorkspace();
    ResolveUserFilePath(paths, stored);                             // 0x4089F0
    wchar_t path[0x100];
    wcscpy_s(path, 0x100,
             paths.resolvedPath);                                  // 0x506292
    if (!original_import::Reference(path, hwnd, s.EnglishUI())) {
        stored[0] = L'\0';
        s.PictureBackgroundEnabled() = 0;
        return;
    }
    if (!WsOpenOk(path)) {                                          // 0x50747B
        MessageBoxA(hwnd,
                    s.EnglishUI() != 0 ? "Cannot open BMP file"
                                       : kMsgBmpFailJp,
                    s.EnglishUI() != 0 ? "open BMP file" : kMsgBmpCaptionJp,
                    0);                                             // 0x4337FE
        swprintf_s(stored, 0x100, L"");                             // 0x433842
        return;
    }
    wcscpy_s(stored, 0x100, path);                                  // 0x43386D

    if (s.PictureBackgroundTexture() != nullptr) {                   // 0x433872
        s.PictureBackgroundTexture()->Release();
        s.PictureBackgroundTexture() = nullptr;
    }

    IDirect3DDevice9* device = s.Renderer()->device;                // 0x43389C
    auto& api = d3dx::Get();
    IDirect3DTexture9** slot = &s.PictureBackgroundTexture();
    ImgInfo info = {};
    constexpr UINT kD3dxDefault = 0xFFFFFFFFu;
    HRESULT hr = E_FAIL;
    if (api.Load() && api.fromFileExW != nullptr) {
        hr = api.fromFileExW(device, stored, 0x400, 0x400, 0, 0,
                             D3DFMT_UNKNOWN, D3DPOOL_MANAGED, kD3dxDefault,
                             kD3dxDefault, 0, &info, nullptr, slot);  // 0x4C68AC
        if (hr != 0) {
            hr = api.fromFileExW(device, stored, 0x200, 0x200, 1, 0,
                                 D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
                                 kD3dxDefault, kD3dxDefault, 0, &info,
                                 nullptr, slot);                    // 0x4338FC
        }
    }
    if (hr != 0) {                                                  // 0x433903
        MessageBoxA(hwnd,
                    s.EnglishUI() != 0 ? "Cannot open BMP file"
                                       : kMsgBmpFailJp,
                    s.EnglishUI() != 0 ? "open BMP file" : kMsgBmpCaptionJp,
                    0);                                             // 0x43393A
        swprintf_s(stored, 0x100, L"");                             // 0x43394A
        return;
    }

    // success 0x433965..0x433A22
    s.PictureWidth() = static_cast<std::int32_t>(info.Width);
    s.PictureHeight() = static_cast<std::int32_t>(info.Height);
    s.PictureOffsetX() = 0;                                         // 0x4339AE
    float scale = 0.0f;
    std::int32_t pos = 0;
    MediaAspect(app, static_cast<std::int32_t>(info.Width),
                static_cast<std::int32_t>(info.Height), scale, pos);
    s.PictureScale() = scale;
    s.PictureOffsetY() = pos;
    s.PictureBackgroundEnabled() = 1;                               // 0x433A07
    CheckMenuItem(GetMenu(hwnd), 0xE9, MF_CHECKED);                 // 0x4339F3..
    PicBgOverlayRefresh(app);                                                 // 0x433A22
}

}  // namespace mikumikustudio
