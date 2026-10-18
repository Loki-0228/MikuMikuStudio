#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/image_sequence_export.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/globals.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/model_layout.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/transparent_capture.hpp"
#include <cmath>
#include <algorithm>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <wrl/client.h>

namespace mikudancestudio::image_sequence {
namespace {
struct Session { Options options; std::uint64_t written = 0; };
std::unordered_map<MMDApp*, Session> sessions;
std::unordered_map<MMDApp*, bool> stills;
template<class Key>
std::uint32_t TrackEnd(const Key* keys, std::size_t capacity, std::uint32_t head = 0) {
    std::uint32_t last = 0;
    if (!keys) return last;
    for (std::size_t visited = 0; visited < capacity && head < capacity; ++visited) {
        last = std::max(last, std::min(keys[head].frame, std::uint32_t(frame_limits::maximum)));
        const auto next = keys[head].next;
        if (!next || next == head) break;
        head = next;
    }
    return last;
}
std::wstring FileError(MMDApp* app, const wchar_t* path, unsigned long code) {
    wchar_t number[32]{};
    swprintf_s(number, L" (0x%08lX)", code);
    return std::wstring(Text(app, L"无法写入图片，请检查路径、权限及可用空间：\n",
        L"Could not write image. Check the path, permissions and free space:\n",
        L"画像を書き込めません。パス、アクセス権、空き容量を確認してください：\n")) + path + number;
}
std::wstring Collision(MMDApp* app, const std::wstring& path) {
    return std::wstring(Text(app, L"已存在同名文件，请更换输出文件夹或前缀：\n",
        L"This file already exists. Choose another folder or prefix:\n",
        L"同名のファイルがあります。出力フォルダーまたは接頭辞を変更してください：\n")) + path;
}
}

int FormatFromPath(const wchar_t* path) {
    const auto extension=std::filesystem::path(path ? path : L"").extension().wstring();
    for(int i=0;i<std::size(kFormats);++i)
        if(_wcsicmp(extension.c_str(),kFormats[i].extension)==0 ||
           (i==1 && _wcsicmp(extension.c_str(),L".jpeg")==0))return i;
    return -1;
}
bool SupportsAlpha(int format) { return format==2 || format==3; }
void BeginStill(MMDApp* app,int format) { stills.insert_or_assign(app,SupportsAlpha(format)); }
void EndStill(MMDApp* app) { if(stills.erase(app))ReleaseTransparentCapture(app); }
bool Transparent(MMDApp* app) {
    const auto found=sessions.find(app);
    if(found!=sessions.end())return SupportsAlpha(found->second.options.format);
    const auto still=stills.find(app);
    return app && app->RecordingWindow() && still!=stills.end() && still->second;
}
long SaveSurface(const wchar_t* path,int format,IDirect3DSurface9* surface,bool transparent) {
    auto& api=d3dx::Get();
    if(format<0 || format>=std::size(kFormats) || !surface || !api.Load() || !api.saveSurfaceToFileW)return E_INVALIDARG;
    if(!transparent || !SupportsAlpha(format))return api.saveSurfaceToFileW(path,kFormats[format].d3dxFormat,surface,nullptr,nullptr);
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> pixels;
    D3DSURFACE_DESC desc{};
    HRESULT hr=surface->GetDesc(&desc);if(FAILED(hr))return hr;
    if(desc.Format!=D3DFMT_A8R8G8B8)return E_INVALIDARG;
    hr=surface->GetDevice(&device);if(FAILED(hr))return hr;
    hr=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&pixels,nullptr);if(FAILED(hr))return hr;
    hr=device->GetRenderTargetData(surface,pixels.Get());if(FAILED(hr))return hr;
    D3DLOCKED_RECT lock{};hr=pixels->LockRect(&lock,nullptr,0);if(FAILED(hr))return hr;
    for(UINT y=0;y<desc.Height;++y) {
        auto* row=static_cast<unsigned char*>(lock.pBits)+std::size_t(y)*lock.Pitch;
        for(UINT x=0;x<desc.Width;++x) {
            auto* p=row+x*4;const unsigned alpha=p[3];
            for(int c=0;c<3;++c)p[c]=alpha?static_cast<unsigned char>(std::min(255u,(unsigned(p[c])*255+alpha/2)/alpha)):0;
        }
    }
    pixels->UnlockRect();
    return api.saveSurfaceToFileW(path,kFormats[format].d3dxFormat,pixels.Get(),nullptr,nullptr);
}

const wchar_t* Text(MMDApp* app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    return app->EnglishUI() == 2 ? zh : app->EnglishUI() == 1 ? en : ja;
}

std::uint32_t DefaultEndFrame(MMDApp* app) {
    BOOL valid = FALSE;
    const int stop = static_cast<int>(GetDlgItemInt(app->MainWindow(),
        panel::kPlayStopFrameEdit, &valid, TRUE));
    if (valid && stop > 0) return static_cast<std::uint32_t>(stop);
    return std::max(app->LastRegisteredFrame(), SceneEndFrame(app));
}

std::uint32_t SceneEndFrame(MMDApp* app) {
    std::uint32_t end = 0;
    const auto track = [&](const auto* keys) {
        end = std::max(end, TrackEnd(keys, mdl::KeyPoolCapacity(keys)));
    };
    track(app->CameraKeys()); track(app->LightKeys());
    track(app->ShadowKeys()); track(app->GravityKeys());
    for (int slot = 0; slot < 255; ++slot)
        if (app->AccessorySlot(slot)) track(app->AccessoryKeys(slot));
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        const auto* model = app->ModelSlot(slot);
        if (!model) continue;
        const auto& m = *mdl::Mdl(model);
        const auto boneCapacity = mdl::BoneKeyCapacity(m);
        for (std::uint32_t bone = 0; bone < std::min<std::size_t>(m.boneCount, boneCapacity); ++bone)
            end = std::max(end, TrackEnd(m.boneKeys, boneCapacity, bone));
        const auto morphCapacity = mdl::MorphKeyCapacity(m);
        for (std::uint32_t morph = 0; morph < std::min<std::size_t>(m.morphCount, morphCapacity); ++morph)
            end = std::max(end, TrackEnd(m.morphKeys, morphCapacity, morph));
        track(m.displayKeys);
    }
    if (const auto* audio = app->Audio(); audio && audio->dataSize > 0 &&
        audio->format.nAvgBytesPerSec > 0) {
        const double frames = std::ceil(static_cast<double>(audio->dataSize) *
            g_FrameScale / audio->format.nAvgBytesPerSec);
        if (std::isfinite(frames) && frames > end)
            end = static_cast<std::uint32_t>(std::min(frames, double(frame_limits::maximum)));
    }
    return end;
}

std::uint64_t FrameCount(const Options& o) {
    if (o.start < 0 || o.end < o.start || o.end == INT_MAX ||
        !std::isfinite(o.fps) || o.fps < .001f || o.fps > 240) return 0;
    const double frames = std::ceil((static_cast<double>(o.end) - o.start + 1) * o.fps / 30.0);
    return frames > 0 && frames <= INT_MAX ? static_cast<std::uint64_t>(frames) : 0;
}

std::wstring FramePath(const Options& o, std::uint64_t number) {
    if (o.format < 0 || o.format >= std::size(kFormats)) return {};
    std::uint64_t leading = static_cast<std::uint64_t>(std::max(0, o.end));
    int width = 1;
    while (leading >= 10) { leading /= 10; ++width; }
    if (leading >= 5) ++width;
    // A higher output FPS can produce more images than timeline frames.
    // Keep one stable width for the whole sequence even in that case.
    int countWidth = 1;
    for (auto count = FrameCount(o); count >= 10; count /= 10) ++countWidth;
    width = std::max(width, countWidth);
    wchar_t digits[32]{};
    swprintf_s(digits, L"%0*llu", width, static_cast<unsigned long long>(number));
    return (std::filesystem::path(o.directory) /
        (o.prefix + digits + kFormats[o.format].extension)).wstring();
}

bool Validate(MMDApp* app, const Options& o, std::wstring& error) {
    error.clear();
    if (o.width < 2 || o.height < 2 || o.width > 16384 || o.height > 16384) {
        error = Text(app, L"图片宽高必须在 2 到 16384 之间。", L"Image dimensions must be between 2 and 16384.", L"画像の幅と高さは2～16384で指定してください。");
    } else if (FrameCount(o) == 0) {
        error = Text(app, L"请填写有效的起止帧和帧率。起始帧不得小于 0，结束帧不得小于起始帧，帧率须为 0.001 到 240，图片总数不得超过 2147483647。",
            L"Enter a valid range and frame rate: start >= 0, end >= start, FPS 0.001 to 240, at most 2147483647 images.",
            L"開始フレームは0以上、終了は開始以上、FPSは0.001～240、画像数は2147483647以下で指定してください。");
    } else if (o.format < 0 || o.format >= std::size(kFormats)) {
        error = Text(app, L"请选择图片格式。", L"Select an image format.", L"画像形式を選択してください。");
    } else if (o.prefix.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
        std::any_of(o.prefix.begin(), o.prefix.end(), [](wchar_t c) { return c < 32; }) ||
        (!o.prefix.empty() && (o.prefix.back() == L'.' || o.prefix.back() == L' '))) {
        error = Text(app, L"前缀不能包含路径分隔符或以下字符：< > : \" / \\ | ? *，也不能以空格或句点结尾。",
            L"The prefix cannot contain control characters or < > : \" / \\ | ? *, or end with a space or period.",
            L"接頭辞に制御文字や < > : \" / \\ | ? * は使えません。末尾の空白やピリオドも使えません。");
    } else {
        const DWORD attributes = GetFileAttributesW(o.directory.c_str());
        if (o.directory.empty() || !std::filesystem::path(o.directory).is_absolute() ||
            attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = Text(app, L"请选择有效的输出文件夹。", L"Select an existing output folder.", L"有効な出力フォルダーを選択してください。");
        } else if (FramePath(o, FrameCount(o)).size() >= MAX_PATH || o.directory.size() > MAX_PATH - 14) {
            error = Text(app, L"输出路径过长，请缩短前缀或选择更短的文件夹路径。",
                L"The output path is too long. Shorten the prefix or choose a shorter folder path.",
                L"出力パスが長すぎます。接頭辞を短くするか、短いフォルダーパスを選択してください。");
        }
    }
    return error.empty();
}

bool Prepare(MMDApp* app, const Options& o, std::wstring& error) {
    if (!Validate(app, o, error)) return false;
    auto& api = d3dx::Get();
    if (!api.Load() || !api.saveSurfaceToFileW) {
        error = Text(app, L"图片编码器不可用，无法开始输出。", L"The image encoder is unavailable.", L"画像エンコーダーを利用できません。");
        return false;
    }
    const auto pattern = (std::filesystem::path(o.directory) /
        (o.prefix + L"*" + kFormats[o.format].extension)).wstring();
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name.size() <= o.prefix.size()) continue;
            const wchar_t* digits = name.c_str() + o.prefix.size();
            if (!iswdigit(*digits)) continue;
            wchar_t* tail = nullptr;
            const auto number = _wcstoui64(digits, &tail, 10);
            if (number == 0 || number > FrameCount(o) ||
                _wcsicmp(tail, kFormats[o.format].extension) != 0) continue;
            const auto path = FramePath(o, number);
            if (_wcsicmp(std::filesystem::path(path).filename().c_str(), name.c_str()) == 0) {
                error = Collision(app, path);
                FindClose(search);
                return false;
            }
        } while (FindNextFileW(search, &found));
        const DWORD code = GetLastError();
        FindClose(search);
        if (code != ERROR_NO_MORE_FILES) {
            error = FileError(app, o.directory.c_str(), code);
            return false;
        }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = FileError(app, o.directory.c_str(), GetLastError());
        return false;
    }
    sessions.insert_or_assign(app, Session{o});
    return true;
}

bool Active(MMDApp* app) { return sessions.find(app) != sessions.end(); }
void Abort(MMDApp* app) { sessions.erase(app); EndStill(app); }

bool WriteFrame(MMDApp* app, IDirect3DSurface9* surface, std::wstring& error) {
    error.clear();
    const auto it = sessions.find(app);
    if (it == sessions.end()) return false;
    auto& s = it->second;
    const auto path = FramePath(s.options, s.written + 1);
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(s.options.directory.c_str(), L"MMS", 0, temporary)) {
        error = FileError(app, path.c_str(), GetLastError());
        return false;
    }
    // Use the same surface encoder as single-image output. Publish only a
    // complete frame, with no replacement even if a collision appears mid-export.
    const HRESULT hr = SaveSurface(temporary,s.options.format,surface,
        SupportsAlpha(s.options.format));
    if (FAILED(hr)) {
        DeleteFileW(temporary);
        error = FileError(app, path.c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    if (!MoveFileExW(temporary, path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary);
        error = code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS
            ? Collision(app, path) : FileError(app, path.c_str(), code);
        return false;
    }
    ++s.written;
    return true;
}

bool Finish(MMDApp* app, bool reachedRequestedEnd, std::wstring& error) {
    error.clear();
    const auto it = sessions.find(app);
    if (it == sessions.end()) return false;
    const bool complete = reachedRequestedEnd && it->second.written == FrameCount(it->second.options);
    if (reachedRequestedEnd && !complete)
        error = Text(app, L"图片序列未完成：实际输出数量与指定范围不一致。已输出的图片予以保留。",
            L"The image sequence is incomplete. The image count differs from the requested range. Saved images were kept.",
            L"連番画像は未完了です。画像数が指定範囲と一致しません。保存済みの画像は保持されます。");
    sessions.erase(it);
    return complete;
}
} // namespace mikudancestudio::image_sequence
