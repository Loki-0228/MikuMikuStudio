#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/wave_audio_context.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mikumikustudio::media {
using Microsoft::WRL::ComPtr;
namespace {
struct Failure { HRESULT hr; const wchar_t* operation; };
void Check(HRESULT hr, const wchar_t* operation) { if (FAILED(hr)) throw Failure{hr, operation}; }
std::wstring Describe(const Failure& failure) {
    wchar_t code[32]{}; swprintf_s(code, L" (0x%08lX)", failure.hr);
    return std::wstring(failure.operation) + code + L"\nWindows 无法解码该文件。请检查文件及系统媒体组件。";
}
struct Runtime {
    bool com = false, mf = false;
    Runtime() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        com = SUCCEEDED(hr);
        if (hr != RPC_E_CHANGED_MODE) Check(hr, L"COM initialization");
        const HRESULT startup = MFStartup(MF_VERSION);
        if (FAILED(startup)) { if (com) CoUninitialize(); com = false; Check(startup, L"Media Foundation initialization"); }
        mf = true;
    }
    ~Runtime() { if (mf) MFShutdown(); if (com) CoUninitialize(); }
};
bool Extension(const wchar_t* path, const wchar_t* extension) {
    if (!path) return false;
    const wchar_t* dot = std::wcsrchr(path, L'.');
    return dot && _wcsicmp(dot, extension) == 0;
}
struct DecodedAudio {
    std::wstring source, temporary;
    ~DecodedAudio() { if (!temporary.empty()) DeleteFileW(temporary.c_str()); }
};
std::unordered_map<WaveAudioContext*, std::unique_ptr<DecodedAudio>> audioFiles;

}

bool IsAudioPath(const wchar_t* path) {
    for (const auto* ext : {L".wav", L".mp3", L".aac", L".adts", L".m4a", L".wma"})
        if (Extension(path, ext)) return true;
    return false;
}
bool IsModernVideoPath(const wchar_t* path) { return Extension(path, L".mp4") || Extension(path, L".mov"); }
bool IsVideoPath(const wchar_t* path) { return Extension(path, L".avi") || IsModernVideoPath(path); }
void ReleaseAudio(WaveAudioContext* audio) { audioFiles.erase(audio); }
const wchar_t* AudioSource(WaveAudioContext* audio) {
    const auto it = audioFiles.find(audio);
    return it == audioFiles.end() ? audio->path : it->second->source.c_str();
}
bool PrepareAudio(WaveAudioContext* audio, const wchar_t* source, std::wstring& playable, std::wstring& error) {
    playable = source; error.clear();
    if (Extension(source, L".wav")) return true;
    try {
        Runtime runtime;
        ComPtr<IMFSourceReader> reader;
        Check(MFCreateSourceReaderFromURL(source, nullptr, &reader), L"Open audio");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), L"Select audio");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE), L"Select audio");
        ComPtr<IMFMediaType> type;
        Check(MFCreateMediaType(&type), L"Audio media type");
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get()), L"Decode PCM audio");
        type.Reset();
        Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type), L"PCM media type");
        UINT32 rate = 0, channels = 0, bits = 0;
        Check(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate), L"Audio sample rate");
        Check(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels), L"Audio channels");
        Check(type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits), L"Audio bit depth");
        if (!rate || channels < 1 || channels > 2 || bits != 16)
            throw Failure{MF_E_INVALIDMEDIATYPE, L"Audio must decode to mono or stereo PCM16"};
        wchar_t folder[MAX_PATH]{}, temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"MMS", 0, temporary))
            throw Failure{HRESULT_FROM_WIN32(GetLastError()), L"Create decoded audio cache"};
        auto decoded = std::make_unique<DecodedAudio>();
        decoded->temporary = temporary; decoded->source = source;
        FILE* raw = nullptr;
        if (_wfopen_s(&raw, temporary, L"w+b") || !raw) throw Failure{E_ACCESSDENIED, L"Open audio cache"};
        std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
        unsigned char header[44]{};
        std::memcpy(header, "RIFF", 4); std::memcpy(header + 8, "WAVEfmt ", 8); std::memcpy(header + 36, "data", 4);
        const auto put16 = [&](unsigned offset, std::uint16_t value) { std::memcpy(header + offset, &value, 2); };
        const auto put32 = [&](unsigned offset, std::uint32_t value) { std::memcpy(header + offset, &value, 4); };
        put32(16, 16); put16(20, 1); put16(22, static_cast<std::uint16_t>(channels));
        put32(24, rate); put32(28, rate * channels * 2); put16(32, static_cast<std::uint16_t>(channels * 2)); put16(34, 16);
        if (std::fwrite(header, 1, 44, raw) != 44) throw Failure{E_FAIL, L"Write WAV header"};
        std::uint64_t total = 0;
        for (;;) {
            DWORD flags = 0; ComPtr<IMFSample> sample;
            Check(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample), L"Read audio");
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) throw Failure{MF_E_INVALIDMEDIATYPE, L"Audio format changed"};
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer;
                Check(sample->ConvertToContiguousBuffer(&buffer), L"PCM buffer");
                BYTE* data = nullptr; DWORD count = 0;
                Check(buffer->Lock(&data, nullptr, &count), L"Lock PCM buffer");
                const bool fits = total + count <= INT32_MAX - 44;
                const bool written = fits && std::fwrite(data, 1, count, raw) == count;
                buffer->Unlock();
                if (!written) throw Failure{E_FAIL, fits ? L"Write audio cache" : L"Decoded audio exceeds 2 GiB"};
                total += count;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (!total || total % (channels * 2)) throw Failure{E_FAIL, L"Empty or incomplete audio"};
        put32(4, static_cast<std::uint32_t>(36 + total)); put32(40, static_cast<std::uint32_t>(total));
        if (std::fseek(raw, 0, SEEK_SET) || std::fwrite(header, 1, 44, raw) != 44 || std::fflush(raw))
            throw Failure{E_FAIL, L"Finalize audio cache"};
        file.reset();
        playable = decoded->temporary;
        audioFiles[audio] = std::move(decoded);
        return true;
    } catch (const Failure& failure) { error = Describe(failure); }
      catch (const std::exception&) { error = L"音频解码内存不足。"; }
    return false;
}

}
