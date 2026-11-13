#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct IDirect3DSurface9;
namespace mikumikustudio {
class MMDApp;
namespace image_sequence {
constexpr unsigned kMenuCommand = 32811;
struct Format { const wchar_t* name; const wchar_t* extension; int d3dxFormat; };
inline constexpr Format kFormats[] = {
    {L"BMP", L".bmp", 0}, {L"JPG", L".jpg", 1},
    {L"PNG", L".png", 3}, {L"DDS", L".dds", 4},
    {L"DIB", L".dib", 6}, {L"PFM", L".pfm", 8}, {L"HDR", L".hdr", 7},
};
struct Options {
    std::wstring directory;
    std::wstring prefix = L"frame_";
    int format = 2;
    int width = 0, height = 0;
    int start = 0, end = 0;
    float fps = 30;
    bool closeAfterExport = false;
    int playbackFps = 30;
};
int FormatFromPath(const wchar_t*);
bool SupportsAlpha(int format);
bool Transparent(MMDApp*);
struct StillFileIdentity {
    std::uint32_t volume;
    std::uint64_t file, lastWrite, size;
    bool operator==(const StillFileIdentity& other) const noexcept {
        return volume == other.volume && file == other.file && lastWrite == other.lastWrite && size == other.size;
    }
};
std::optional<StillFileIdentity> ReadStillFileIdentity(const wchar_t* path) noexcept;
void BeginStill(MMDApp*, int format, std::optional<StillFileIdentity> overwrite);
bool StillActive(MMDApp*);
void SetStillFrameReady(MMDApp*, bool ready);
bool StillFrameReady(MMDApp*);
void EndStill(MMDApp*);
// Cancel only a pending still capture, without stopping or sampling playback.
bool CancelStillCapture(MMDApp*);
// Alpha render targets contain premultiplied colour. Image files use straight alpha.
long SaveSurface(const wchar_t*, int format, IDirect3DSurface9*, bool transparent);
long SaveStillSurface(MMDApp*, const wchar_t*, IDirect3DSurface9*, bool transparent, std::wstring& retained);
const wchar_t* Text(MMDApp*, const wchar_t* zh, const wchar_t* en, const wchar_t* ja);
std::uint32_t DefaultEndFrame(MMDApp*);
// Actual scene tracks and audio, independent of the editor's playback range.
std::uint32_t SceneEndFrame(MMDApp*);
std::uint64_t FrameCount(const Options&);
std::wstring FramePath(const Options&, std::uint64_t number);
bool Validate(MMDApp*, const Options&, std::wstring& error);
// Prepare checks the complete output range for collisions before rendering.
bool Prepare(MMDApp*, const Options&, std::wstring& error);
bool Active(MMDApp*);
bool WriteFrame(MMDApp*, IDirect3DSurface9*, std::wstring& error);
bool Finish(MMDApp*, bool reachedRequestedEnd, std::wstring& error);
void Abort(MMDApp*);
void RefreshMenu(MMDApp*);
bool HandleCommand(MMDApp*, unsigned command);
} // namespace image_sequence
} // namespace mikumikustudio
