#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "mikumikustudio/wic_mov_recorder.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "staged_output_publication.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mikumikustudio {
using Microsoft::WRL::ComPtr;
namespace {
using Bytes = std::vector<unsigned char>;
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void Need(bool ok, const char* message) { if (!ok) throw Failure(message); }
void Hr(HRESULT hr, const char* message) { Need(SUCCEEDED(hr), message); }
std::wstring Wide(const char* text) { const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0); if (count <= 1) return L"PNG/MOV export failed."; std::wstring value(static_cast<std::size_t>(count), L'\0'); MultiByteToWideChar(CP_UTF8, 0, text, -1, value.data(), count); value.resize(static_cast<std::size_t>(count - 1)); return value; }
void U16(Bytes& b, std::uint16_t n) { b.push_back(static_cast<unsigned char>(n >> 8)); b.push_back(static_cast<unsigned char>(n)); }
void U32(Bytes& b, std::uint32_t n) { U16(b, static_cast<std::uint16_t>(n >> 16)); U16(b, static_cast<std::uint16_t>(n)); }
void U64(Bytes& b, std::uint64_t n) { U32(b, static_cast<std::uint32_t>(n >> 32)); U32(b, static_cast<std::uint32_t>(n)); }
void Four(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
void Zero(Bytes& b, std::size_t n) { b.resize(b.size() + n); }
std::size_t Box(Bytes& b, const char* type) { const auto p = b.size(); U32(b, 0); Four(b, type); return p; }
void End(Bytes& b, std::size_t p) { Need(b.size() - p <= UINT32_MAX, "MOV atom too large"); const auto n = static_cast<std::uint32_t>(b.size() - p); for (int i = 0; i < 4; ++i) b[p + i] = static_cast<unsigned char>(n >> (24 - i * 8)); }
std::size_t Full(Bytes& b, const char* type, std::uint32_t flags = 0) { const auto p = Box(b, type); U32(b, flags); return p; }
void Matrix(Bytes& b) { for (std::uint32_t n : {0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u}) U32(b, n); }
struct Recording {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring destination, temporary, spare;
    std::optional<recording_output_path::FileIdentity> originalDestination;
    int width = 0, height = 0;
    std::uint32_t fpsNum = 0, fpsDen = 1;
    std::uint64_t frames = 0, position = 0;
    bool com = false, failed = false;
    std::vector<std::uint32_t> sizes;
    std::vector<std::uint64_t> offsets;
    Bytes pixels;
    ComPtr<IWICImagingFactory> factory;
    ~Recording() { factory.Reset(); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); if (!temporary.empty()) DeleteFileW(temporary.c_str()); if (com) CoUninitialize(); }
    void Write(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        while (size) { const DWORD count = static_cast<DWORD>((std::min)(size, std::size_t(16 * 1024 * 1024))); DWORD done = 0; Need(WriteFile(file, p, count, &done, nullptr) && done == count, "MOV write failed"); p += count; size -= count; position += count; }
    }
    void Write(const Bytes& bytes) { Write(bytes.data(), bytes.size()); }
    void Seek(std::uint64_t offset) { LARGE_INTEGER p{}; p.QuadPart = static_cast<LONGLONG>(offset); Need(SetFilePointerEx(file, p, nullptr, FILE_BEGIN), "MOV seek failed"); position = offset; }
    Bytes EncodePng() {
        ComPtr<IStream> stream; Hr(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create PNG stream");
        ComPtr<IWICBitmapEncoder> encoder; Hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frame; Hr(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
        Hr(frame->Initialize(nullptr), "Initialize PNG frame"); Hr(frame->SetSize(width, height), "Set PNG dimensions");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA; Hr(frame->SetPixelFormat(&format), "Set PNG pixel format");
        Need(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "WIC PNG encoder cannot retain BGRA alpha");
        Hr(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data()), "Write PNG pixels");
        Hr(frame->Commit(), "Commit PNG frame"); Hr(encoder->Commit(), "Commit PNG stream");
        STATSTG stat{}; Hr(stream->Stat(&stat, STATFLAG_NONAME), "Read PNG size"); Need(stat.cbSize.QuadPart <= UINT32_MAX, "PNG sample too large");
        Bytes output(static_cast<std::size_t>(stat.cbSize.QuadPart)); LARGE_INTEGER zero{}; Hr(stream->Seek(zero, STREAM_SEEK_SET, nullptr), "Seek PNG stream"); ULONG read = 0;
        Hr(stream->Read(output.data(), static_cast<ULONG>(output.size()), &read), "Read PNG stream"); Need(read == output.size(), "Truncated PNG sample"); return output;
    }
    void Sample(const Bytes& data) { Need(!data.empty() && data.size() <= UINT32_MAX, "Invalid PNG sample"); offsets.push_back(position); sizes.push_back(static_cast<std::uint32_t>(data.size())); Write(data); }
};

void CreateStagingFile(Recording& recording) {
    const auto directory = std::filesystem::path(recording.destination).parent_path();
    const auto room = MAX_PATH - 1 - static_cast<std::ptrdiff_t>(directory.native().size()) - 1 - 4;
    const auto digits = static_cast<std::size_t>((std::clamp)(room, std::ptrdiff_t{1}, std::ptrdiff_t{8}));
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        GUID id{};
        Hr(CoCreateGuid(&id), "Create MOV temporary name");
        wchar_t name[9]{};
        swprintf_s(name, L"%08lX", id.Data1);
        const std::wstring stem(name, digits);
        std::wstring temporary = (directory / (stem + L".tmp")).native();
        std::wstring spare;
        if (recording.originalDestination) spare = (directory / (stem + L".bak")).native();
        if (_wcsicmp(temporary.c_str(), recording.destination.c_str()) == 0 ||
            (!spare.empty() && _wcsicmp(spare.c_str(), recording.destination.c_str()) == 0)) continue;
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            recording.file = file;
            recording.temporary.swap(temporary);
            recording.spare.swap(spare);
            return;
        }
        const DWORD status = GetLastError();
        Need(status == ERROR_FILE_EXISTS || status == ERROR_ALREADY_EXISTS, "Create MOV temporary file");
    }
    throw Failure("Create MOV temporary file");
}

std::wstring PublicationError(const std::wstring& destination, HRESULT result,
                              const std::wstring& retained, unsigned char language) {
    std::wstring message = language == kUiChinese
        ? L"无法将 MOV 写入当前目标。请重新选择输出路径，或确认覆盖后重试。"
        : language == kUiJapanese
        ? L"指定した出力先に MOV を書き込めませんでした。出力先を選び直すか、上書きを確認して再試行してください。"
        : L"The MOV could not be written to the selected destination. Choose the path again or confirm replacement and retry.";
    message += L"\n\n";
    message += destination;
    wchar_t status[32]{};
    swprintf_s(status, L"\n\nHRESULT 0x%08lX", static_cast<unsigned long>(result));
    message += status;
    if (!retained.empty()) {
        message += language == kUiChinese ? L"\n\n先前的视频保留在：\n"
            : language == kUiJapanese ? L"\n\n以前の動画の保存先：\n"
                                     : L"\n\nThe previous video was kept at:\n";
        message += retained;
    }
    return message;
}

std::unordered_map<const DShowRecorder*, std::unique_ptr<Recording>> active;
std::uint32_t Duration(std::uint64_t frames, std::uint32_t num, std::uint32_t den) { const auto v = (frames * den * 1000 + num - 1) / num; Need(v <= UINT32_MAX, "MOV duration too long"); return static_cast<std::uint32_t>(v); }
void Track(Bytes& b, const Recording& r) {
    const auto track = Box(b, "trak"), tkhd = Full(b, "tkhd", 7); U32(b, 0); U32(b, 0); U32(b, 1); U32(b, 0); U32(b, Duration(r.frames, r.fpsNum, r.fpsDen)); Zero(b, 8); U16(b, 0); U16(b, 0); U16(b, 0); U16(b, 0); Matrix(b); U32(b, static_cast<std::uint32_t>(r.width) << 16); U32(b, static_cast<std::uint32_t>(r.height) << 16); End(b, tkhd);
    const auto mdia = Box(b, "mdia"), mdhd = Full(b, "mdhd"); U32(b, 0); U32(b, 0); U32(b, r.fpsNum); U32(b, static_cast<std::uint32_t>(r.frames * r.fpsDen)); U16(b, 0); U16(b, 0); End(b, mdhd);
    const auto hdlr = Full(b, "hdlr"); U32(b, 0); Four(b, "vide"); Zero(b, 12); const char name[] = "MMS WIC PNG"; b.insert(b.end(), name, name + sizeof(name)); End(b, hdlr);
    const auto minf = Box(b, "minf"), vmhd = Full(b, "vmhd", 1); Zero(b, 8); End(b, vmhd);
    const auto dinf = Box(b, "dinf"), dref = Full(b, "dref"); U32(b, 1); const auto url = Full(b, "url ", 1); End(b, url); End(b, dref); End(b, dinf);
    const auto stbl = Box(b, "stbl"), stsd = Full(b, "stsd"); U32(b, 1); const auto png = Box(b, "png "); Zero(b, 6); U16(b, 1); Zero(b, 16); U16(b, static_cast<std::uint16_t>(r.width)); U16(b, static_cast<std::uint16_t>(r.height)); U32(b, 72 << 16); U32(b, 72 << 16); U32(b, 0); U16(b, 1); const char compressor[] = "PNG"; b.push_back(3); b.insert(b.end(), compressor, compressor + 3); Zero(b, 28); U16(b, 24); U16(b, 0xffff); End(b, png); End(b, stsd);
    const auto stts = Full(b, "stts"); U32(b, 1); U32(b, static_cast<std::uint32_t>(r.frames)); U32(b, r.fpsDen); End(b, stts);
    const auto stsc = Full(b, "stsc"); U32(b, 1); U32(b, 1); U32(b, 1); U32(b, 1); End(b, stsc);
    const auto stsz = Full(b, "stsz"); U32(b, 0); U32(b, static_cast<std::uint32_t>(r.frames)); for (auto n : r.sizes) U32(b, n); End(b, stsz);
    const auto co64 = Full(b, "co64"); U32(b, static_cast<std::uint32_t>(r.offsets.size())); for (auto n : r.offsets) U64(b, n); End(b, co64);
    End(b, stbl); End(b, minf); End(b, mdia); End(b, track);
}
}
bool IsPngMovRecording(const DShowRecorder* rec) { return active.find(rec) != active.end(); }
void AbortPngMovRecording(DShowRecorder* rec) { active.erase(rec); }
bool StartPngMovRecording(DShowRecorder* rec, const wchar_t* path, int width, int height,
                          float fps, std::wstring& error,
                          const recording_output_path::Selection* approved, bool* destinationChanged) {
    AbortPngMovRecording(rec); error.clear();
    if (destinationChanged) *destinationChanged = false;
    try {
        Need(rec && path && *path && width >= 2 && height >= 2 && width <= 16384 && height <= 16384 && std::isfinite(fps) && fps >= .001f && fps <= 240, "Invalid PNG/MOV settings");
        recording_output_path::Selection current;
        Need(recording_output_path::ReadSelection(path, current) == ERROR_SUCCESS, "Unable to inspect MOV output path");
        if (approved && !recording_output_path::SameSelection(*approved, current)) {
            if (destinationChanged) *destinationChanged = true;
            error = L"The MOV output file changed after confirmation.";
            return false;
        }
        auto r = std::make_unique<Recording>();
        r->destination = std::move(current.path);
        r->originalDestination = current.existing;
        r->width = width; r->height = height;
        r->fpsNum = static_cast<std::uint32_t>(std::lround(fps * 1000)); r->fpsDen = 1000; const auto divisor = std::gcd(r->fpsNum, r->fpsDen); r->fpsNum /= divisor; r->fpsDen /= divisor;
        r->pixels.resize(static_cast<std::size_t>(width) * height * 4);
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); r->com = SUCCEEDED(initialized); if (initialized != RPC_E_CHANGED_MODE) Hr(initialized, "Initialize COM");
        Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&r->factory)), "Create Windows PNG encoder");
        CreateStagingFile(*r);
        Bytes header; const auto ftyp = Box(header, "ftyp"); Four(header, "qt  "); U32(header, 0); Four(header, "qt  "); End(header, ftyp); U32(header, 1); Four(header, "mdat"); U64(header, 0); r->Write(header);
        active.emplace(rec, std::move(r)); return true;
    } catch (const std::exception& e) { error = Wide(e.what()); return false; }
}
bool WritePngMovFrame(DShowRecorder* rec, const void* data, std::size_t size) {
    const auto found = active.find(rec); if (found == active.end()) return false; auto& r = *found->second;
    try {
        Need(!r.failed && data && size == r.pixels.size(), "Invalid MOV frame"); const auto* input = static_cast<const unsigned char*>(data);
        for (int y = 0; y < r.height; ++y) for (int x = 0; x < r.width; ++x) { const auto* p = input + (static_cast<std::size_t>(r.height - 1 - y) * r.width + x) * 4; auto* d = r.pixels.data() + (static_cast<std::size_t>(y) * r.width + x) * 4; const unsigned a = p[3]; for (int c = 0; c < 3; ++c) d[c] = a ? static_cast<unsigned char>((std::min)(255u, (unsigned(p[c]) * 255u + a / 2) / a)) : 0; d[3] = static_cast<unsigned char>(a); }
        r.Sample(r.EncodePng()); ++r.frames; return true;
    } catch (...) { r.failed = true; return false; }
}
bool FinishPngMovRecording(DShowRecorder* rec, std::uint64_t expected, std::wstring& error, unsigned char uiLanguage) {
    error.clear(); const auto found = active.find(rec); if (found == active.end()) return false; auto r = std::move(found->second); active.erase(found);
    try {
        Need(!r->failed, "PNG/MOV frame encoding failed"); Need(expected && r->frames == expected, "MOV export did not reach its requested frame count");
        const auto end = r->position; Bytes length; U64(length, end - 20); r->Seek(28); r->Write(length); r->Seek(end);
        Bytes movie; const auto moov = Box(movie, "moov"), mvhd = Full(movie, "mvhd"); U32(movie, 0); U32(movie, 0); U32(movie, 1000); U32(movie, Duration(r->frames, r->fpsNum, r->fpsDen)); U32(movie, 1 << 16); U16(movie, 0x100); Zero(movie, 10); Matrix(movie); Zero(movie, 24); U32(movie, 2); End(movie, mvhd); Track(movie, *r); End(movie, moov); r->Write(movie);
        Need(FlushFileBuffers(r->file), "Flush MOV failed"); Need(CloseHandle(r->file), "Close MOV failed"); r->file = INVALID_HANDLE_VALUE;
        std::wstring retained;
        const HRESULT published = staged_output::Publish(r->temporary, r->destination,
            r->originalDestination, std::move(r->spare), retained);
        if (!retained.empty()) runtime_log::WritePath("MOV previous output retained at", retained.c_str());
        if (FAILED(published)) {
            error = PublicationError(r->destination, published, retained, uiLanguage);
            return false;
        }
        r->temporary.clear();
        return true;
    } catch (const std::exception& e) { error = Wide(e.what()); return false; }
}
}
