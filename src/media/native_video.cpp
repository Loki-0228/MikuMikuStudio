#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/native_media.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "mikumikustudio/mf_optional.hpp"
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mikumikustudio::media {
namespace {
using Microsoft::WRL::ComPtr;
struct Failure { HRESULT hr; const wchar_t* operation; };
struct Cancelled {};
void Check(HRESULT hr, const wchar_t* operation) { if (FAILED(hr)) throw Failure{hr, operation}; }
std::wstring Describe(const Failure& failure) {
    wchar_t code[32]{}; swprintf_s(code, L" (0x%08lX)", failure.hr);
    return std::wstring(failure.operation) + code;
}
struct Runtime {
    Runtime() {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"Video COM initialization");
        const HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) { CoUninitialize(); Check(hr, L"Media Foundation initialization"); }
    }
    ~Runtime() { MFShutdown(); CoUninitialize(); }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { Reset(); }
    void Reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};
// CreateProcess arguments use the Windows CRT quoting rules, never a shell.
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::wstring FindFFmpeg() {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
    const auto folder = std::filesystem::path(executable).parent_path();
    for (const auto& candidate : {folder / L"tools" / L"ffmpeg.exe", folder / L"ffmpeg.exe"})
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate.wstring();
    // Search the configured PATH explicitly; do not search a project's current directory.
    const DWORD count = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::vector<wchar_t> path(count ? count : 1);
    if (count && GetEnvironmentVariableW(L"PATH", path.data(), count) &&
        SearchPathW(path.data(), L"ffmpeg.exe", nullptr, DWORD(std::size(executable)), executable, nullptr))
        return executable;
    throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        L"Windows 无法解码该视频。10-bit H.264 等格式需要 FFmpeg；请将 ffmpeg.exe 放入程序的 tools 目录或 PATH"};
}
struct PipeDecoder {
    Handle process, output;
    ~PipeDecoder() { Close(); }
    void Close() {
        output.Reset();
        if (process.value) {
            if (WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) TerminateProcess(process.value, 0);
            WaitForSingleObject(process.value, 1000);
            process.Reset();
        }
    }
    void Start(const std::wstring& executable, const std::wstring& source,
               double seconds, UINT width, UINT height, UINT rate, UINT scale) {
        Close();
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle write, null;
        Check(CreatePipe(&output.value, &write.value, &security, 0) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), L"Video pipe");
        Check(SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), L"Video pipe ownership");
        null.Reset(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security, OPEN_EXISTING, 0, nullptr));
        Check(null.value != INVALID_HANDLE_VALUE ? S_OK : HRESULT_FROM_WIN32(GetLastError()), L"Video process streams");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        Check(InitializeProcThreadAttributeList(attributes, 1, 0, &size) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), L"Video process attributes");
        struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Cleanup() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        HANDLE inherited[]{write.value, null.value};
        Check(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                        sizeof(inherited), nullptr, nullptr) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), L"Video process handles");
        wchar_t seek[64]{}; swprintf_s(seek, L"%.9f", std::max(0.0, seconds - 0.0000001));
        const std::wstring filter = L"fps=" + std::to_wstring(rate) + L"/" + std::to_wstring(scale) +
            L",scale=" + std::to_wstring(width) + L":" + std::to_wstring(height);
        std::wstring command = Quote(executable) +
            L" -hide_banner -loglevel error -nostdin -threads 2 -filter_threads 1 -ss " + seek +
            L" -i " + Quote(source) + L" -map 0:v:0 -an -sn -dn -vf " + Quote(filter) +
            L" -pix_fmt bgra -f rawvideo pipe:1";
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = startup.StartupInfo.hStdError = null.value;
        startup.StartupInfo.hStdOutput = write.value;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &child) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), L"Start FFmpeg video decoder");
        process.Reset(child.hProcess); CloseHandle(child.hThread);
    }
    bool Read(unsigned char* data, size_t count, const std::atomic<bool>& stop) {
        size_t done = 0;
        auto progress = std::chrono::steady_clock::now();
        while (done < count) {
            if (stop.load()) throw Cancelled{};
            DWORD available = 0;
            if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) {
                if (!done && GetLastError() == ERROR_BROKEN_PIPE) return false;
                throw Failure{E_FAIL, L"Incomplete FFmpeg video frame"};
            }
            if (!available) {
                if (std::chrono::steady_clock::now() - progress > std::chrono::seconds(30))
                    throw Failure{HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"FFmpeg video decode timeout"};
                Sleep(2); continue;
            }
            DWORD read = 0;
            Check(ReadFile(output.value, data + done, DWORD(std::min<size_t>(available, count - done)),
                           &read, nullptr) && read ? S_OK : E_FAIL, L"Read FFmpeg video frame");
            done += read; progress = std::chrono::steady_clock::now();
        }
        return true;
    }
};

std::atomic<unsigned long long> nextRevision{0};
struct Frame {
    std::vector<unsigned char> dib;
    LONGLONG time = 0, end = 0;
    unsigned long long revision = 0;
    bool eos = false;
    bool Contains(LONGLONG target) const { return target >= time && (target < end || eos); }
};
struct Decoder {
    Runtime runtime;
    ComPtr<IMFSourceReader> reader;
    UINT width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
    UINT rate = 30, scale = 1;
    LONG stride = 0;
    double duration = 0;
    LONGLONG origin = 0, frameDuration = 333333, pipeIndex = -1;
    bool originKnown = false;
    std::wstring path, ffmpeg;
    PipeDecoder pipe;
    std::shared_ptr<Frame> frame;
    const std::atomic<bool>& stop;

    explicit Decoder(const std::atomic<bool>& stopping) : stop(stopping) {}
    void ReadFormat() {
        ComPtr<IMFMediaType> type;
        Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type), L"RGB32 media type");
        GUID subtype{}; Check(type->GetGUID(MF_MT_SUBTYPE, &subtype), L"Video pixel format");
        if (subtype != MFVideoFormat_RGB32) throw Failure{MF_E_INVALIDMEDIATYPE, L"Video must decode to RGB32"};
        Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &sourceWidth, &sourceHeight), L"Video dimensions");
        if (!sourceWidth || !sourceHeight || sourceWidth > 16384 || sourceHeight > 16384)
            throw Failure{E_INVALIDARG, L"Invalid video dimensions"};
        UINT32 value = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &value))) stride = static_cast<LONG>(value);
        else Check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, sourceWidth, &stride), L"Video stride");
    }
    std::shared_ptr<Frame> Allocate() const {
        auto result = std::make_shared<Frame>();
        result->dib.resize(sizeof(BITMAPINFOHEADER) + size_t(width) * height * 4);
        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(result->dib.data());
        *header = {}; header->biSize = sizeof(*header); header->biWidth = width; header->biHeight = height;
        header->biPlanes = 1; header->biBitCount = 32; header->biCompression = BI_RGB;
        return result;
    }
    void Copy(IMFSample* sample, Frame& destination) {
        ComPtr<IMFMediaBuffer> buffer;
        Check(sample->ConvertToContiguousBuffer(&buffer), L"Video frame buffer");
        ComPtr<IMF2DBuffer> twoD;
        BYTE* data = nullptr; LONG pitch = stride; DWORD bytes = 0;
        const bool use2D = SUCCEEDED(buffer.As(&twoD));
        if (use2D) Check(twoD->Lock2D(&data, &pitch), L"Lock video frame");
        else Check(buffer->Lock(&data, nullptr, &bytes), L"Lock video frame");
        struct Unlock {
            IMFMediaBuffer* buffer; IMF2DBuffer* twoD;
            ~Unlock() { if (twoD) twoD->Unlock2D(); else buffer->Unlock(); }
        } unlock{buffer.Get(), use2D ? twoD.Get() : nullptr};
        if (std::abs(LONGLONG(pitch)) < LONGLONG(sourceWidth) * 4 ||
            (!use2D && bytes < std::uint64_t(std::abs(LONGLONG(pitch))) * sourceHeight))
            throw Failure{E_INVALIDARG, L"Truncated video frame or invalid stride"};
        if (!use2D && pitch < 0) data += size_t(-LONGLONG(pitch)) * (sourceHeight - 1);
        auto* pixels = destination.dib.data() + sizeof(BITMAPINFOHEADER);
        for (UINT y = 0; y < height; ++y) {
            auto* row = pixels + size_t(height - 1 - y) * width * 4;
            const auto* source = data + ptrdiff_t(std::uint64_t(y) * sourceHeight / height) * pitch;
            if (width == sourceWidth) std::memcpy(row, source, size_t(width) * 4);
            else for (UINT x = 0; x < width; ++x)
                std::memcpy(row + size_t(x) * 4, source + size_t(std::uint64_t(x) * sourceWidth / width) * 4, 4);
        }
    }
    void Open(const std::wstring& source, int maxDimension) {
        path = source;
        ComPtr<IMFAttributes> attributes;
        Check(MFCreateAttributes(&attributes, 2), L"Video reader attributes");
        attributes->SetUINT32(media_compat::advancedVideoProcessing, TRUE);
        HRESULT opened = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
        bool basicReader = false;
        if (FAILED(opened)) {
            basicReader = true;
            reader.Reset();
            attributes->DeleteItem(media_compat::advancedVideoProcessing);
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            opened = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
        }
        Check(opened, L"Open video");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), L"Select video");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE), L"Select video");
        ComPtr<IMFMediaType> native;
        Check(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native), L"Native video format");
        Check(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height), L"Native video dimensions");
        if (!width || !height || width > 16384 || height > 16384) throw Failure{E_INVALIDARG, L"Invalid video dimensions"};
        UINT numerator = 0, denominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) && numerator && denominator) {
            rate = numerator; scale = denominator;
            frameDuration = std::max<LONGLONG>(1, 10000000LL * scale / rate);
        }
        PROPVARIANT length{};
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &length)) && length.vt == VT_UI8)
            duration = length.uhVal.QuadPart / 10000000.0;
        PropVariantClear(&length);
        if (!std::isfinite(duration) || duration <= 0 || duration > 1e9) throw Failure{E_INVALIDARG, L"Video has no finite duration"};
        if (maxDimension > 0 && std::max(width, height) > UINT(maxDimension)) {
            const double ratio = double(maxDimension) / std::max(width, height);
            width = std::max(2u, UINT(width * ratio) & ~1u); height = std::max(2u, UINT(height * ratio) & ~1u);
        }
        const auto configure = [&] {
            ComPtr<IMFMediaType> type;
            Check(MFCreateMediaType(&type), L"Video media type");
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
            HRESULT format = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());
            if (FAILED(format)) {
                type->DeleteItem(MF_MT_FRAME_SIZE);
                format = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());
            }
            Check(format, L"Decode RGB32 video");
            ReadFormat(); Decode(0);
        };
        try {
            try { configure(); }
            catch (const Failure&) {
                // An older reader may accept the unknown advanced attribute
                // but fail RGB32 negotiation. Retry its basic CPU processor
                // before requiring an external codec fallback.
                if (basicReader) throw;
                reader.Reset(); frame.reset(); origin = 0; originKnown = false;
                attributes->DeleteItem(media_compat::advancedVideoProcessing);
                attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                Check(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader), L"Open basic video reader");
                Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), L"Select video");
                Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE), L"Select video");
                configure();
            }
        } catch (const Failure&) {
            // In particular Windows' H.264 MFT does not decode High 10. Keep
            // the native metadata and use a bounded, disk-free pipe fallback.
            ffmpeg = FindFFmpeg(); reader.Reset(); frame.reset(); Decode(0);
        }
    }
    std::shared_ptr<Frame> Decode(LONGLONG target) {
        if (frame && frame->Contains(target)) return frame;
        if (!ffmpeg.empty()) return DecodePipe(target);
        if (frame && (target < frame->time || target > frame->end + 10000000)) {
            PROPVARIANT position{}; position.vt = VT_I8;
            position.hVal.QuadPart = std::min(target + origin, LONGLONG(duration * 10000000) - 1);
            Check(reader->SetCurrentPosition(GUID_NULL, position), L"Seek video");
        }
        ComPtr<IMFSample> selected;
        LONGLONG time = 0, end = 0; bool eos = false;
        unsigned emptyReads = 0;
        for (;;) {
            if (stop.load()) throw Cancelled{};
            ComPtr<IMFSample> sample; DWORD flags = 0; LONGLONG timestamp = 0;
            Check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample), L"Read video frame");
            // H.264 commonly reports a new stride/aperture on its first sample.
            // Re-read and validate the RGB layout instead of rejecting the clip.
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ReadFormat();
            if (sample) {
                if (!originKnown) { origin = timestamp; originKnown = true; }
                selected = sample; time = timestamp - origin;
                LONGLONG sampleDuration = frameDuration;
                sample->GetSampleDuration(&sampleDuration);
                end = time + std::max<LONGLONG>(sampleDuration, 1);
                emptyReads = 0;
            } else if (++emptyReads > 1024) throw Failure{E_FAIL, L"Video decoder returned no frames"};
            eos = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if ((sample && end > target) || eos) break;
        }
        if (!selected) {
            if (eos && frame) {
                auto last = std::make_shared<Frame>(*frame); last->eos = true; frame = last; return frame;
            }
            throw Failure{E_FAIL, L"Video has no frames"};
        }
        auto next = Allocate(); Copy(selected.Get(), *next);
        next->time = time; next->end = end; next->eos = eos; next->revision = ++nextRevision;
        frame = next; return frame;
    }
    std::shared_ptr<Frame> DecodePipe(LONGLONG target) {
        const LONGLONG wanted = LONGLONG(std::floor(target / 10000000.0 * rate / scale + 1e-7));
        if (frame && wanted == pipeIndex) return frame;
        if (!pipe.process.value || wanted < pipeIndex || wanted > pipeIndex + 30) {
            pipe.Start(ffmpeg, path, double(wanted) * scale / rate, width, height, rate, scale);
            pipeIndex = wanted - 1;
        }
        auto next = Allocate();
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        bool readFrame = false;
        while (pipeIndex < wanted) {
            if (!pipe.Read(pixels.data(), pixels.size(), stop)) {
                if (readFrame) { next->eos = true; break; }
                if (frame) { auto last = std::make_shared<Frame>(*frame); last->eos = true; frame = last; return frame; }
                throw Failure{E_FAIL, L"FFmpeg could not decode the first video frame"};
            }
            ++pipeIndex; readFrame = true;
        }
        for (UINT y = 0; y < height; ++y)
            std::memcpy(next->dib.data() + sizeof(BITMAPINFOHEADER) + size_t(height - 1 - y) * width * 4,
                        pixels.data() + size_t(y) * width * 4, size_t(width) * 4);
        next->time = LONGLONG(double(pipeIndex) * scale / rate * 10000000);
        next->end = LONGLONG(std::ceil(double(pipeIndex + 1) * scale / rate * 10000000));
        next->revision = ++nextRevision; frame = next; return frame;
    }
};

struct Video {
    std::mutex mutex;
    std::condition_variable ready;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::shared_ptr<Frame> latest, displayed;
    std::wstring error;
    double duration = 0;
    UINT width = 0, height = 0;
    LONGLONG requested = 0;
    unsigned long long request = 1, completed = 0;
    bool opened = false, failed = false;
    ~Video() { stop = true; ready.notify_all(); if (worker.joinable()) worker.join(); }
    void Run(std::wstring path, int maxDimension) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        try {
            Decoder decoder(stop);
            decoder.Open(path, maxDimension);
            {
                std::lock_guard<std::mutex> lock(mutex);
                width = decoder.width; height = decoder.height; duration = decoder.duration;
                latest = decoder.frame; opened = true; completed = 1;
            }
            ready.notify_all();
            for (;;) {
                LONGLONG target; unsigned long long serial;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [&] { return stop.load() || request != completed; });
                    if (stop.load()) return;
                    target = requested; serial = request;
                }
                // No lock is held during seeking, decoding, conversion or IO.
                // There is one pending target; dragging never builds a queue.
                auto next = decoder.Decode(target);
                std::shared_ptr<Frame> previous;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    previous = std::move(latest);
                    latest = std::move(next); completed = serial;
                }
                ready.notify_all();
            }
        } catch (const Cancelled&) { return; }
          catch (const Failure& failure) { std::lock_guard<std::mutex> lock(mutex); error = Describe(failure); failed = true; }
          catch (const std::exception&) { std::lock_guard<std::mutex> lock(mutex); error = L"视频解码资源不足。"; failed = true; }
        ready.notify_all();
    }
};
// The map and displayed snapshots belong to the UI thread. Only the worker's
// mailbox crosses threads; COM objects are created and destroyed on that worker.
std::unordered_map<MMDApp*, std::unique_ptr<Video>> videos;
}

bool HasVideo(MMDApp* app) { return videos.find(app) != videos.end(); }
void ReleaseVideo(MMDApp* app) { videos.erase(app); }
unsigned long long VideoFrameRevision(MMDApp* app) {
    const auto found = videos.find(app);
    return found != videos.end() && found->second->displayed ? found->second->displayed->revision : 0;
}
bool OpenVideo(MMDApp* app, const wchar_t* path, int& width, int& height, double& duration, std::wstring& error, int maxDimension) {
    error.clear();
    try {
        auto video = std::make_unique<Video>();
        video->worker = std::thread(&Video::Run, video.get(), std::wstring(path), maxDimension);
        {
            std::unique_lock<std::mutex> lock(video->mutex);
            video->ready.wait(lock, [&] { return video->opened || video->failed; });
            if (video->failed) { error = video->error; return false; }
            width = video->width; height = video->height; duration = video->duration;
            video->displayed = video->latest;
        }
        videos[app] = std::move(video); return true;
    } catch (const std::exception&) { error = L"无法启动视频解码线程。"; return false; }
}
BITMAPINFOHEADER* VideoFrame(MMDApp* app, double seconds, std::wstring& error, bool waitForFrame) {
    error.clear();
    const auto found = videos.find(app);
    if (found == videos.end()) return nullptr;
    auto& video = *found->second;
    if (!std::isfinite(seconds)) seconds = 0;
    const auto target = LONGLONG(std::clamp(seconds, 0.0, std::max(0.0, video.duration - .0000001)) * 10000000.0);
    std::unique_lock<std::mutex> lock(video.mutex);
    if (target != video.requested) {
        video.requested = target; ++video.request; video.ready.notify_all();
    }
    if (waitForFrame) video.ready.wait(lock, [&] { return video.failed || video.completed == video.request; });
    if (video.failed) { error = video.error; return nullptr; }
    auto next = video.latest;
    lock.unlock();
    // Destroy old bitmaps outside the mailbox lock; the renderer's pointer stays
    // valid until the next UI read, regardless of worker progress.
    video.displayed = std::move(next);
    return reinterpret_cast<BITMAPINFOHEADER*>(video.displayed->dib.data());
}
}
