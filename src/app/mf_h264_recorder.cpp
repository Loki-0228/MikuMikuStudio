#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <codecapi.h>
// ICodecAPI comes from strmif.h (pulled in via dshow_recorder.hpp/dshow.h).
// icodecapi.h must NOT be included: its struct CodecAPIEventData sits outside
// the __ICodecAPI_INTERFACE_DEFINED__ guard and collides with strmif.h's copy.
#include <strmif.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "mikumikustudio/mf_optional.hpp"
#include <wmcodecdsp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <new>
#include <unordered_map>
#include <vector>

#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/mf_h264_recorder.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/wave_audio_context.hpp"

namespace mikumikustudio {
void ReleaseSinkWriterQuietly(IMFSinkWriter*& writer) noexcept;
namespace {

struct Recording {
    IMFSinkWriter* writer = nullptr;
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    bool hasAudio = false;
    HANDLE audioFile = INVALID_HANDLE_VALUE;
    std::uint32_t audioDataBytes = 0;
    std::uint32_t audioDataOffset = 0;
    std::uint32_t audioBlockAlign = 0;
    std::uint32_t audioSampleRate = 0;
    std::uint64_t audioSamplesWritten = 0;
    std::uint64_t frameIndex = 0;
    std::uint64_t frameDuration = 0;
    std::uint64_t frameBytes = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    // Set when the export runs on the hardware encoder: the Sink Writer then
    // takes NV12 and every BGRA frame is converted in WriteBuiltInH264Frame.
    bool nv12Input = false;
    std::vector<unsigned char> convertScratch;
    std::uint32_t fpsNumerator = 30;
    std::uint32_t fpsDenominator = 1;
    HWND owner = nullptr;
    bool mfStarted = false;
    unsigned char uiLanguage = kUiJapanese;

    ~Recording() {
        if (audioFile != INVALID_HANDLE_VALUE)
            CloseHandle(audioFile);
        ReleaseSinkWriterQuietly(writer);
        if (mfStarted)
            MFShutdown();
    }
};

std::mutex gRecordingsMutex;
std::unordered_map<const DShowRecorder*, std::unique_ptr<Recording>> gRecordings;

struct CodecProperty {
    const wchar_t* nameEn;
    const wchar_t* nameZh;
    const wchar_t* nameJa;
    GUID key;
};

// Standard Media Foundation ICodecAPI properties probed for the settings
// window.  ICodecAPI has no "list every property" call, so only these known
// standard CODECAPI_* keys can be discovered; a row is shown only when the
// encoder reports the key as supported (IsSupported == S_OK) AND modifiable
// (IsModifiable == S_OK).  Vendor-private settings and vendor property pages
// are NOT covered here - those stay on each DirectShow encoder's own dialog
// (ShowCodecConfigDialog) in the AVI export path.
constexpr CodecProperty kH264Properties[] = {
    {L"Rate control", L"码率控制", L"レート制御", CODECAPI_AVEncCommonRateControlMode},
    {L"Average bit rate", L"平均码率", L"平均ビットレート", CODECAPI_AVEncCommonMeanBitRate},
    {L"Maximum bit rate", L"最大码率", L"最大ビットレート", CODECAPI_AVEncCommonMaxBitRate},
    {L"Quality", L"质量", L"品質", CODECAPI_AVEncCommonQuality},
    {L"Quality vs. speed", L"质量／速度平衡", L"品質と速度のバランス", CODECAPI_AVEncCommonQualityVsSpeed},
    {L"Multipass mode", L"多遍编码模式", L"マルチパスモード", CODECAPI_AVEncCommonMultipassMode},
    {L"Low latency", L"低延迟", L"低遅延", CODECAPI_AVEncCommonLowLatency},
    {L"Real-time encoding", L"实时编码", L"リアルタイムエンコード", CODECAPI_AVEncCommonRealTime},
    {L"VBV buffer size", L"VBV 缓冲区大小", L"VBV バッファーサイズ", CODECAPI_AVEncCommonBufferSize},
    {L"Allow frame drops", L"允许丢帧", L"フレームドロップを許可", CODECAPI_AVEncCommonAllowFrameDrops},
    {L"Key-frame interval", L"关键帧间隔", L"キーフレーム間隔", CODECAPI_AVEncVideoMaxKeyframeDistance},
    {L"H.264 CABAC", L"H.264 CABAC", L"H.264 CABAC", CODECAPI_AVEncH264CABACEnable},
    {L"Maximum reference frames", L"最大参考帧数", L"最大参照フレーム数", CODECAPI_AVEncVideoMaxNumRefFrame},
    {L"Minimum quantizer", L"最小量化参数", L"最小量子化値", CODECAPI_AVEncVideoMinQP},
    {L"Maximum quantizer", L"最大量化参数", L"最大量子化値", CODECAPI_AVEncVideoMaxQP},
    {L"Encoder usage", L"编码器用途", L"エンコーダーの用途", CODECAPI_AVEncVideoUsage},
    {L"Worker threads", L"工作线程数", L"ワーカースレッド数", CODECAPI_AVEncNumWorkerThreads},
    {L"Slice control mode", L"切片控制模式", L"スライス制御モード", CODECAPI_AVEncSliceControlMode},
    {L"Slice size", L"切片大小", L"スライスサイズ", CODECAPI_AVEncSliceControlSize},
    {L"Low-latency mode", L"低延迟模式", L"低遅延モード", CODECAPI_AVLowLatencyMode},
};

const wchar_t* UiText(unsigned char language, const wchar_t* english,
                      const wchar_t* chinese, const wchar_t* japanese) {
    if (language == kUiChinese) return chinese;
    if (language == kUiEnglish) return english;
    return japanese;
}

const wchar_t* PropertyName(const GUID& key,
                            unsigned char language = kUiEnglish) {
    for (const CodecProperty& property : kH264Properties) {
        if (IsEqualGUID(property.key, key))
            return UiText(language, property.nameEn,
                          property.nameZh, property.nameJa);
    }
    return UiText(language, L"Encoder setting", L"编码器设置项", L"エンコーダー設定");
}

bool PropertySupported(ICodecAPI* codec, const GUID& key) {
    return codec != nullptr && codec->IsSupported(&key) == S_OK;
}

bool PropertyModifiable(ICodecAPI* codec, const GUID& key) {
    return codec != nullptr && codec->IsModifiable(&key) == S_OK;
}

struct CodecOverride {
    GUID key{};
    VARIANT value{};

    CodecOverride() { VariantInit(&value); }
    CodecOverride(const CodecOverride& other) : key(other.key) {
        VariantInit(&value);
        VariantCopy(&value, const_cast<VARIANT*>(&other.value));
    }
    CodecOverride& operator=(const CodecOverride& other) {
        if (this != &other) {
            VariantClear(&value);
            key = other.key;
            VariantInit(&value);
            VariantCopy(&value, const_cast<VARIANT*>(&other.value));
        }
        return *this;
    }
    CodecOverride(CodecOverride&& other) noexcept : key(other.key), value(other.value) {
        VariantInit(&other.value);
    }
    CodecOverride& operator=(CodecOverride&& other) noexcept {
        if (this != &other) {
            VariantClear(&value);
            key = other.key;
            value = other.value;
            VariantInit(&other.value);
        }
        return *this;
    }
    ~CodecOverride() { VariantClear(&value); }
};

// One saved configuration per encoder family (0 = built-in software H.264,
// 1 = "system auto" which may pick a hardware encoder).  surfaceHash is the
// fingerprint of the encoder ICodecAPI surface the settings window edited; the
// export recomputes it on the encoder the Sink Writer really uses and reports
// when the two differ, so a setting can never silently fail to take effect.
struct CodecSettingGroup {
    std::vector<CodecOverride> overrides;
    std::uint64_t surfaceHash = 0;
    bool hasSurfaceHash = false;
};

std::mutex gCodecOverridesMutex;
CodecSettingGroup gCodecGroups[2];

struct EncoderSettingRow {
    const CodecProperty* property = nullptr;
    VARIANT current{};
    std::vector<VARIANT> values;
    HWND editor = nullptr;
    bool isList = false;

    EncoderSettingRow() { VariantInit(&current); }
    EncoderSettingRow(const EncoderSettingRow&) = delete;
    EncoderSettingRow& operator=(const EncoderSettingRow&) = delete;
    EncoderSettingRow(EncoderSettingRow&& other) noexcept
        : property(other.property), current(other.current),
          values(std::move(other.values)), editor(other.editor), isList(other.isList) {
        VariantInit(&other.current);
        other.editor = nullptr;
    }
    EncoderSettingRow& operator=(EncoderSettingRow&& other) noexcept {
        if (this != &other) {
            VariantClear(&current);
            for (VARIANT& value : values) VariantClear(&value);
            property = other.property;
            current = other.current;
            VariantInit(&other.current);
            values = std::move(other.values);
            editor = other.editor;
            other.editor = nullptr;
            isList = other.isList;
        }
        return *this;
    }
    ~EncoderSettingRow() {
        VariantClear(&current);
        for (VARIANT& value : values) VariantClear(&value);
    }
};

struct EncoderSettingsDialog {
    HWND window = nullptr;
    HWND owner = nullptr;
    HFONT font = nullptr;
    HWND bitrate = nullptr;
    ICodecAPI* codec = nullptr;
    std::uint32_t* averageBitrate = nullptr;
    std::uint64_t surfaceHash = 0;
    bool hasSurfaceHash = false;
    int group = 0;
    int scroll = 0;
    bool done = false;
    bool accepted = false;
    unsigned char uiLanguage = kUiJapanese;
    std::vector<EncoderSettingRow> rows;
};

std::wstring VariantText(const VARIANT& value, const GUID* property = nullptr,
                         unsigned char language = kUiEnglish) {
    if (property != nullptr &&
        IsEqualGUID(*property, CODECAPI_AVEncCommonRateControlMode)) {
        switch (value.vt == VT_I4 ? value.lVal : value.ulVal) {
        case eAVEncCommonRateControlMode_CBR: return L"CBR";
        case eAVEncCommonRateControlMode_PeakConstrainedVBR:
            return UiText(language, L"Peak constrained VBR", L"峰值约束 VBR", L"ピーク制約 VBR");
        case eAVEncCommonRateControlMode_UnconstrainedVBR:
            return UiText(language, L"Unconstrained VBR", L"非约束 VBR", L"非制約 VBR");
        case eAVEncCommonRateControlMode_Quality:
            return UiText(language, L"Quality", L"质量优先", L"品質優先");
        case eAVEncCommonRateControlMode_LowDelayVBR:
            return UiText(language, L"Low-delay VBR", L"低延迟 VBR", L"低遅延 VBR");
        case eAVEncCommonRateControlMode_GlobalVBR:
            return UiText(language, L"Global VBR", L"全局 VBR", L"グローバル VBR");
        case eAVEncCommonRateControlMode_GlobalLowDelayVBR:
            return UiText(language, L"Global low-delay VBR", L"全局低延迟 VBR", L"グローバル低遅延 VBR");
        default: break;
        }
    }
    if (value.vt == VT_BOOL)
        return value.boolVal != VARIANT_FALSE
            ? UiText(language, L"On", L"开", L"オン")
            : UiText(language, L"Off", L"关", L"オフ");
    if (value.vt == VT_BSTR)
        return value.bstrVal != nullptr ? value.bstrVal : L"";
    VARIANT converted{};
    VariantInit(&converted);
    std::wstring result;
    if (SUCCEEDED(VariantChangeType(&converted,
            const_cast<VARIANT*>(&value), 0, VT_BSTR)) && converted.bstrVal != nullptr)
        result = converted.bstrVal;
    VariantClear(&converted);
    return result;
}

bool IsEditableVariant(VARTYPE type) {
    return type == VT_BOOL || type == VT_I2 || type == VT_UI2 ||
           type == VT_I4 || type == VT_UI4 || type == VT_INT ||
           type == VT_UINT || type == VT_I8 || type == VT_UI8 ||
           type == VT_R4 || type == VT_R8 || type == VT_BSTR;
}

// ---- Encoder resolution through the Sink Writer ---------------------------
//
// The Sink Writer owns the video chain (input converter -> encoder -> sink)
// and IMFSinkWriterEx::GetTransformForStream exposes its links together with
// the MFT category of each link.  The settings window and the export both
// resolve the encoder with the code below, so what the window enumerates is
// the very MFT the export feeds - not "some encoder found in the registry".

constexpr DWORD kMaxSinkWriterTransforms = 32;

struct EncoderTarget {
    IMFTransform* transform = nullptr;
    ICodecAPI* codec = nullptr;
    DWORD index = 0;
    DWORD transformCount = 0;
    std::uint64_t surfaceHash = 0;

    void Reset() {
        if (codec != nullptr) codec->Release();
        if (transform != nullptr) transform->Release();
        codec = nullptr;
        transform = nullptr;
        index = 0;
        transformCount = 0;
        surfaceHash = 0;
    }
    ~EncoderTarget() { Reset(); }
};

// FNV-1a fingerprint of the editable standard-property surface of one
// encoder: which of kH264Properties it reports supported/modifiable, plus the
// position in the Sink Writer chain.  The settings window stores it; the
// export recomputes it on the encoder it actually uses and reports mismatch.
std::uint64_t HashEncoderSurface(ICodecAPI* codec, DWORD index,
                                 DWORD transformCount) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](const void* data, std::size_t bytes) {
        const auto* bytes8 = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            hash ^= bytes8[i];
            hash *= 1099511628211ull;
        }
    };
    mix(&index, sizeof index);
    mix(&transformCount, sizeof transformCount);
    for (const CodecProperty& property : kH264Properties) {
        const unsigned char state = static_cast<unsigned char>(
            (PropertySupported(codec, property.key) ? 1 : 0) |
            (PropertyModifiable(codec, property.key) ? 2 : 0));
        mix(&state, sizeof state);
    }
    return hash;
}

// Finds the encoder MFT inside one Sink Writer stream.  Preference order:
// the link whose category is MFT_CATEGORY_VIDEO_ENCODER, else the link whose
// ICodecAPI exposes the most known standard properties (>= 1).
bool ResolveSinkWriterEncoder(IMFSinkWriter* writer, DWORD stream,
                              EncoderTarget& target) {
    target.Reset();
    if (writer == nullptr)
        return false;
    mikumikustudio::media_compat::SinkWriterExtended* extended = nullptr;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(&extended)))) {
        // Win7 exposes the stream encoder through the original Sink Writer.
        // Query the actual writer, not an unrelated encoder from the registry.
        IMFTransform* transform = nullptr;
        if (FAILED(writer->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&transform))))
            return false;
        ICodecAPI* codec = nullptr;
        if (FAILED(transform->QueryInterface(IID_PPV_ARGS(&codec)))) {
            transform->Release();
            return false;
        }
        target.transform = transform;
        target.codec = codec;
        target.transformCount = 1;
        target.surfaceHash = HashEncoderSurface(codec, 0, 1);
        return true;
    }

    struct Candidate {
        IMFTransform* transform = nullptr;
        ICodecAPI* codec = nullptr;
        DWORD index = 0;
        int score = 0;
    };
    std::vector<Candidate> candidates;
    DWORD count = 0;
    for (DWORD index = 0; index < kMaxSinkWriterTransforms; ++index) {
        GUID category = {};
        IMFTransform* transform = nullptr;
        const HRESULT hr = extended->GetTransformForStream(
            stream, index, &category, &transform);
        if (hr == MF_E_INVALIDINDEX || FAILED(hr) || transform == nullptr)
            break;
        ++count;
        ICodecAPI* codec = nullptr;
        if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec))) &&
            codec != nullptr) {
            Candidate candidate;
            candidate.transform = transform;
            candidate.codec = codec;
            candidate.index = index;
            candidate.score = IsEqualGUID(category, MFT_CATEGORY_VIDEO_ENCODER)
                                  ? 100 : 0;
            for (const CodecProperty& property : kH264Properties) {
                if (PropertySupported(codec, property.key)) {
                    candidate.score += 1;
                    if (PropertyModifiable(codec, property.key))
                        candidate.score += 1;
                }
            }
            if (candidate.score > 0) {
                candidates.push_back(candidate);
                continue;
            }
            codec->Release();
        }
        transform->Release();
    }
    extended->Release();

    const Candidate* best = nullptr;
    for (const Candidate& candidate : candidates) {
        if (best == nullptr || candidate.score > best->score)
            best = &candidate;
    }
    if (best != nullptr) {
        target.transform = best->transform;
        target.codec = best->codec;
        target.index = best->index;
        target.transformCount = count;
        target.surfaceHash = HashEncoderSurface(best->codec, best->index, count);
        for (const Candidate& candidate : candidates) {
            if (&candidate != best) {
                candidate.codec->Release();
                candidate.transform->Release();
            }
        }
    } else {
        for (const Candidate& candidate : candidates) {
            candidate.codec->Release();
            candidate.transform->Release();
        }
    }
    return best != nullptr;
}

// ---- Export stream construction (shared by export and settings probe) -----

void SetVideoAttributes(IMFMediaType* type, GUID subtype,
                        std::uint32_t width, std::uint32_t height,
                        std::uint32_t fpsNumerator,
                        std::uint32_t fpsDenominator);

void FpsToRatio(float fps, std::uint32_t& numerator, std::uint32_t& denominator) {
    numerator = static_cast<std::uint32_t>(std::lround(fps * 1000.0f));
    denominator = 1000;
    const std::uint32_t divisor = std::gcd(numerator, denominator);
    if (divisor != 0) {
        numerator /= divisor;
        denominator /= divisor;
    }
}

// Packed bottom-up BGRA -> top-down NV12, BT.709 with the 16..235 / 16..240 studio range that
// H.264 normally carries.  The hardware encoder asks for NV12 (AMD answer in
// AMF issue #470: the H.264 core only runs pre-analysis on NV12), so the
// conversion happens here on the CPU instead of inside the encoder's D3D9
// colour-conversion path. The shared DirectShow readback buffer stores the
// bottom scanline first; NV12 requires top-down rows in both planes. RGB32
// keeps its existing negative stride and must not be flipped a second time.
void ConvertBgraToNv12(const unsigned char* bgra, int width, int height,
                       unsigned char* nv12) {
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    unsigned char* luma = nv12;
    unsigned char* chroma = nv12 + pixelCount;
    const auto clampTo = [](int value, int low, int high) {
        return static_cast<unsigned char>(std::clamp(value, low, high));
    };
    for (int y = 0; y < height; ++y) {
        const unsigned char* source =
            bgra + static_cast<std::size_t>(height - 1 - y) * rowBytes;
        unsigned char* destination = luma + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const int b = source[x * 4 + 0];
            const int g = source[x * 4 + 1];
            const int r = source[x * 4 + 2];
            const int full = (54 * r + 183 * g + 19 * b + 128) >> 8;
            destination[x] = clampTo(16 + ((219 * full + 128) >> 8), 16, 235);
        }
    }
    for (int y = 0; y < height; y += 2) {
        const unsigned char* row0 =
            bgra + static_cast<std::size_t>(height - 1 - y) * rowBytes;
        const unsigned char* row1 = row0 - rowBytes;
        unsigned char* destination =
            chroma + static_cast<std::size_t>(y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            int b = 0;
            int g = 0;
            int r = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const unsigned char* row = dy == 0 ? row0 : row1;
                for (int dx = 0; dx < 2; ++dx) {
                    const unsigned char* pixel = row + (x + dx) * 4;
                    b += pixel[0];
                    g += pixel[1];
                    r += pixel[2];
                }
            }
            b /= 4;
            g /= 4;
            r /= 4;
            const int full = (54 * r + 183 * g + 19 * b + 128) >> 8;
            destination[x] = clampTo(128 + (((b - full) * 121) >> 8), 16, 240);
            destination[x + 1] =
                clampTo(128 + (((r - full) * 143) >> 8), 16, 240);
        }
    }
}

// Builds the H.264 output type and the input type, then adds the video stream
// to the Sink Writer.  Shared so the settings probe configures the same chain
// the export will use: the hardware path takes NV12 (what the AMD encoder asks
// for), the software path keeps RGB32 and lets Media Foundation convert.
HRESULT AddH264VideoStream(IMFSinkWriter* writer, std::int32_t width,
                           std::int32_t height, float fps,
                           std::uint32_t averageBitrate, bool nv12Input,
                           DWORD* stream) {
    std::uint32_t fpsNumerator = 30;
    std::uint32_t fpsDenominator = 1;
    FpsToRatio(fps, fpsNumerator, fpsDenominator);
    const std::uint64_t frameBytes = std::uint64_t(width) * height * 4;
    const std::uint64_t nv12Bytes = std::uint64_t(width) * height * 3 / 2;

    IMFMediaType* output = nullptr;
    IMFMediaType* input = nullptr;
    HRESULT hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr)) {
        SetVideoAttributes(output, MFVideoFormat_H264, width, height,
                           fpsNumerator, fpsDenominator);
        output->SetUINT32(MF_MT_AVG_BITRATE,
                          std::clamp<std::uint32_t>(averageBitrate,
                              250'000, 100'000'000));
        hr = writer->AddStream(output, stream);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) {
        if (nv12Input) {
            SetVideoAttributes(input, MFVideoFormat_NV12, width, height,
                               fpsNumerator, fpsDenominator);
            input->SetUINT32(MF_MT_DEFAULT_STRIDE,
                             static_cast<UINT32>(width));
            input->SetUINT32(MF_MT_SAMPLE_SIZE,
                             static_cast<UINT32>(nv12Bytes));
            input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
            input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            input->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            input->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        } else {
            SetVideoAttributes(input, MFVideoFormat_RGB32, width, height,
                               fpsNumerator, fpsDenominator);
            input->SetUINT32(MF_MT_DEFAULT_STRIDE,
                             static_cast<UINT32>(-std::int32_t(width * 4)));
            input->SetUINT32(MF_MT_SAMPLE_SIZE,
                             static_cast<UINT32>(frameBytes));
            input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        }
        hr = writer->SetInputMediaType(*stream, input, nullptr);
    }
    if (input != nullptr) input->Release();
    if (output != nullptr) output->Release();
    return hr;
}

// ---- Settings probe -------------------------------------------------------
//
// A throw-away Sink Writer built exactly like the export one (same attributes,
// same media types) so the settings window can enumerate the encoder the
// export will really pick, including the built-in vs. system-auto difference
// (MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS).

struct EncoderProbe {
    IMFSinkWriter* writer = nullptr;
    EncoderTarget target;
    std::wstring file;
    bool started = false;
};

void CloseEncoderProbe(EncoderProbe& probe) {
    probe.target.Reset();
    if (probe.writer != nullptr) {
        probe.writer->Release();
        probe.writer = nullptr;
    }
    if (!probe.file.empty()) {
        DeleteFileW(probe.file.c_str());
        probe.file.clear();
    }
    probe.started = false;
}

// Issue #34: the AMD hardware H.264 MFT (amdh264enc64) faults inside d3d9
// while Media Foundation builds the DXVA surface chain for the Sink Writer,
// and the editor dies with an access violation.  The encoder runs in-process
// behind this app's d3d9 proxy, so its D3D9 usage is not something the export
// path can fix; what it can do is refuse to keep using hardware transforms
// after the first fault.  The flag below is session-wide, so a fault caught
// while opening the settings window also keeps the later export on the
// software encoder.
bool g_hardwareEncoderUnsafe = false;
bool g_hardwareFallbackNoticeShown = false;

// Tells the user once per session that the export fell back to software.  The
// fault itself happens inside the vendor MFT, so there is no HRESULT to show;
// this notice is the only signal the user gets that the quality/speed differs
// from the "system H.264" they picked.
void ReportHardwareFallbackOnce(void* ownerWindow, unsigned char language) {
    if (g_hardwareFallbackNoticeShown)
        return;
    g_hardwareFallbackNoticeShown = true;
    MessageBoxW(static_cast<HWND>(ownerWindow),
                UiText(language,
                       L"The system hardware encoder stopped with a driver fault "
                       L"while starting the MP4/H.264 export.\n\n"
                       L"This export continues with the software H.264 encoder. "
                       L"Pick \"System H.264 auto\" in the export dialog again if "
                       L"you want to retry the hardware encoder.",
                       L"系统硬件编码器在启动 MP4/H.264 导出时发生驱动故障。\n\n"
                       L"本次导出已改用软件 H.264 编码器继续。"
                       L"如需重试硬件编码器，请在导出对话框里重新选择"
                       L"「系统自动 H.264」。",
                       L"システムのハードウェアエンコーダーが MP4/H.264 "
                       L"書き出しの開始時にドライバー障害を起こしました。\n\n"
                       L"この書き出しはソフトウェア H.264 エンコーダーで続行します。"
                       L"ハードウェアエンコーダーを再試行するには、"
                       L"書き出しダイアログで「系统自动 H.264」を選び直してください。"),
                UiText(language, L"MP4 Export", L"MP4 导出", L"MP4 書き出し"),
                MB_OK | MB_ICONWARNING);
}

bool CreateEncoderProbe(bool system, std::int32_t width, std::int32_t height,
                        float fps, std::uint32_t averageBitrate,
                        EncoderProbe& probe);

// Drops a probe whose encoder MFT just faulted without releasing anything.
// Releasing an MFT that crashed inside its own D3D9 call can fault again, so
// the (bounded, once-per-session) leak is the safer option.
void AbandonEncoderProbe(EncoderProbe& probe) noexcept {
    probe.writer = nullptr;
    probe.target.transform = nullptr;
    probe.target.codec = nullptr;
    probe.target.index = 0;
    probe.target.transformCount = 0;
    probe.target.surfaceHash = 0;
    probe.file.clear();
    probe.started = false;
}

// SEH wrapper: no local objects with destructors here, so the __try block does
// not need C++ unwinding (the same idiom as the crash-backup handler).
bool CreateEncoderProbeGuarded(bool system, std::int32_t width,
                               std::int32_t height, float fps,
                               std::uint32_t averageBitrate,
                               EncoderProbe& probe) noexcept {
    __try {
        return CreateEncoderProbe(system, width, height, fps, averageBitrate,
                                  probe);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime_log::Write(
            "H264 encoder probe faulted code=0x%08lX hardware=%d",
            static_cast<unsigned long>(GetExceptionCode()), system ? 1 : 0);
        return false;
    }
}

bool CreateEncoderProbe(bool system, std::int32_t width, std::int32_t height,
                        float fps, std::uint32_t averageBitrate,
                        EncoderProbe& probe) {
    CloseEncoderProbe(probe);
    width &= ~1;
    height &= ~1;
    if (width < 2) width = 2;
    if (height < 2) height = 2;
    if (!std::isfinite(fps) || fps <= 0.0f) fps = 30.0f;

    wchar_t directory[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, directory) == 0)
        return false;
    wchar_t file[MAX_PATH]{};
    swprintf_s(file, L"%sMMS-encoder-probe-%lu-%llu.mp4", directory,
               GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));
    probe.file = file;

    IMFAttributes* writerAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&writerAttributes, 1);
    if (SUCCEEDED(hr))
        hr = writerAttributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, system ? TRUE : FALSE);
    if (SUCCEEDED(hr))
        hr = MFCreateSinkWriterFromURL(file, nullptr, writerAttributes,
                                       &probe.writer);
    if (writerAttributes != nullptr) writerAttributes->Release();
    if (FAILED(hr)) {
        CloseEncoderProbe(probe);
        return false;
    }

    DWORD stream = 0;
    hr = AddH264VideoStream(probe.writer, width, height, fps,
                            averageBitrate, system, &stream);
    if (SUCCEEDED(hr) &&
        !ResolveSinkWriterEncoder(probe.writer, stream, probe.target)) {
        // Some Sink Writer builds only instantiate the chain on BeginWriting.
        hr = probe.writer->BeginWriting();
        probe.started = SUCCEEDED(hr);
        if (SUCCEEDED(hr))
            ResolveSinkWriterEncoder(probe.writer, stream, probe.target);
    }
    if (probe.target.codec == nullptr) {
        CloseEncoderProbe(probe);
        return false;
    }
    return true;
}

void LayoutEncoderRows(EncoderSettingsDialog* state) {
    constexpr int rowHeight = 32;
    constexpr int visibleRows = 12;
    const int firstY = 64;
    for (std::size_t i = 0; i < state->rows.size(); ++i) {
        const int y = firstY + static_cast<int>(i) * rowHeight - state->scroll;
        const bool visible = y >= firstY && y < firstY + visibleRows * rowHeight;
        ShowWindow(GetDlgItem(state->window, 1000 + static_cast<int>(i)),
                   visible ? SW_SHOW : SW_HIDE);
        if (state->rows[i].editor != nullptr)
            ShowWindow(state->rows[i].editor, visible ? SW_SHOW : SW_HIDE);
        MoveWindow(GetDlgItem(state->window, 1000 + static_cast<int>(i)),
                   16, y, 265, 24, TRUE);
        if (state->rows[i].editor != nullptr)
            MoveWindow(state->rows[i].editor, 288, y, 282, 24, TRUE);
    }
}

bool CommitEncoderSettings(EncoderSettingsDialog* state) {
    wchar_t bitrateText[64]{};
    GetWindowTextW(state->bitrate, bitrateText, static_cast<int>(std::size(bitrateText)));
    wchar_t* end = nullptr;
    const double mbps = wcstod(bitrateText, &end);
    if (end == bitrateText || *end != L'\0' || !std::isfinite(mbps) ||
        mbps < 0.25 || mbps > 100.0) {
        MessageBoxW(state->window,
            UiText(state->uiLanguage,
                L"Bit rate must be between 0.25 and 100 Mbps.",
                L"码率必须在 0.25 到 100 Mbps 之间。",
                L"ビットレートは 0.25～100 Mbps の範囲で入力してください。"),
            UiText(state->uiLanguage, L"Encoder settings", L"编码器设置", L"エンコーダー設定"),
            MB_OK | MB_ICONWARNING);
        return false;
    }
    const auto bitrate = static_cast<std::uint32_t>(std::llround(mbps * 1'000'000.0));
    std::vector<CodecOverride> overrides;
    for (EncoderSettingRow& row : state->rows) {
        VARIANT value{};
        VariantInit(&value);
        if (row.isList) {
            const LRESULT selected = SendMessageW(row.editor, CB_GETCURSEL, 0, 0);
            if (selected < 0 || static_cast<std::size_t>(selected) >= row.values.size())
                continue;
            VariantCopy(&value, &row.values[static_cast<std::size_t>(selected)]);
        } else {
            wchar_t text[128]{};
            GetWindowTextW(row.editor, text, static_cast<int>(std::size(text)));
            VARIANT source{};
            source.vt = VT_BSTR;
            source.bstrVal = SysAllocString(text);
            const HRESULT convert = VariantChangeType(&value, &source, 0, row.current.vt);
            VariantClear(&source);
            if (FAILED(convert)) {
                VariantClear(&value);
                MessageBoxW(state->window,
                    UiText(state->uiLanguage,
                        L"An encoder setting is not a valid number.",
                        L"有一项编码器设置不是有效数值。",
                        L"エンコーダー設定に無効な数値があります。"),
                    UiText(state->uiLanguage, L"Encoder settings", L"编码器设置", L"エンコーダー設定"),
                    MB_OK | MB_ICONWARNING);
                return false;
            }
        }
        const HRESULT set = state->codec->SetValue(&row.property->key, &value);
        if (FAILED(set)) {
            VariantClear(&value);
            MessageBoxW(state->window,
                UiText(state->uiLanguage,
                    L"The encoder rejected a setting. Choose a value it supports.",
                    L"此编码器拒绝了某项设置，请选择它支持的值。",
                    L"エンコーダーが設定値を受け付けませんでした。対応する値を選択してください。"),
                UiText(state->uiLanguage, L"Encoder settings", L"编码器设置", L"エンコーダー設定"),
                MB_OK | MB_ICONWARNING);
            return false;
        }
        CodecOverride overrideValue;
        overrideValue.key = row.property->key;
        VariantCopy(&overrideValue.value, &value);
        overrides.emplace_back(std::move(overrideValue));
        VariantClear(&value);
    }
    {
        std::lock_guard<std::mutex> lock(gCodecOverridesMutex);
        gCodecGroups[state->group].overrides = std::move(overrides);
        gCodecGroups[state->group].surfaceHash = state->surfaceHash;
        gCodecGroups[state->group].hasSurfaceHash = state->hasSurfaceHash;
    }
    *state->averageBitrate = bitrate;
    return true;
}

LRESULT CALLBACK EncoderSettingsWndProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<EncoderSettingsDialog*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<EncoderSettingsDialog*>(create->lpCreateParams);
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == IDOK) {
            if (CommitEncoderSettings(state)) {
                state->accepted = true;
                state->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
    } else if (message == WM_VSCROLL) {
        const int maxScroll = std::max(0, static_cast<int>(state->rows.size()) - 12);
        switch (LOWORD(wParam)) {
        case SB_LINEUP: state->scroll = std::max(0, state->scroll - 1); break;
        case SB_LINEDOWN: state->scroll = std::min(maxScroll, state->scroll + 1); break;
        case SB_PAGEUP: state->scroll = std::max(0, state->scroll - 10); break;
        case SB_PAGEDOWN: state->scroll = std::min(maxScroll, state->scroll + 10); break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: state->scroll = static_cast<int>(HIWORD(wParam)); break;
        default: break;
        }
        LayoutEncoderRows(state);
        return 0;
    } else if (message == WM_MOUSEWHEEL) {
        const int maxScroll = std::max(0, static_cast<int>(state->rows.size()) - 12);
        state->scroll = std::clamp(state->scroll - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA,
                                   0, maxScroll);
        LayoutEncoderRows(state);
        return 0;
    } else if (message == WM_CLOSE) {
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    } else if (message == WM_DESTROY) {
        state->done = true;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ReportFailure(HWND owner, HRESULT hr) {
    wchar_t message[192]{};
    swprintf_s(message, L"Media Foundation 视频编码失败（0x%08lX）。",
                static_cast<unsigned long>(hr));
    MessageBoxW(owner, message, L"H.264 导出", MB_OK | MB_ICONERROR);
}

void SetVideoAttributes(IMFMediaType* type, GUID subtype,
                        std::uint32_t width, std::uint32_t height,
                        std::uint32_t fpsNumerator,
                        std::uint32_t fpsDenominator) {
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fpsNumerator, fpsDenominator);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
}

// Result of pushing the saved settings into the encoder the Sink Writer
// actually uses.  Anything listed in `notApplied` did not reach the encoder,
// and the export reports it instead of silently ignoring it.
struct ApplyResult {
    bool encoderFound = false;
    bool surfaceMatched = true;
    bool anySaved = false;
    std::uint32_t applied = 0;
    std::vector<const wchar_t*> notApplied;
};

// Applies the saved settings to the encoder MFT the Sink Writer really uses
// for this export (resolved through GetTransformForStream, never an
// independently created MFT), so what the settings window showed is what gets
// configured.  Each value is re-checked against the live encoder
// (IsSupported/IsModifiable must return S_OK) before SetValue.  The Mbps
// field is carried by MF_MT_AVG_BITRATE on the output type and mirrored onto
// CODECAPI_AVEncCommonMeanBitRate when the encoder takes it there.
ApplyResult ApplyConfiguredEncoderSettings(IMFSinkWriter* writer, DWORD stream,
                                           bool system,
                                           std::uint32_t averageBitrate,
                                           unsigned char uiLanguage) {
    ApplyResult result;
    EncoderTarget target;
    if (!ResolveSinkWriterEncoder(writer, stream, target))
        return result;
    result.encoderFound = true;

    std::vector<CodecOverride> saved;
    std::uint64_t savedHash = 0;
    bool haveSavedHash = false;
    {
        std::lock_guard<std::mutex> lock(gCodecOverridesMutex);
        saved = gCodecGroups[system ? 1 : 0].overrides;
        savedHash = gCodecGroups[system ? 1 : 0].surfaceHash;
        haveSavedHash = gCodecGroups[system ? 1 : 0].hasSurfaceHash;
    }
    result.anySaved = haveSavedHash || !saved.empty();
    if (haveSavedHash && savedHash != target.surfaceHash)
        result.surfaceMatched = false;

    for (const CodecOverride& value : saved) {
        if (!PropertySupported(target.codec, value.key) ||
            !PropertyModifiable(target.codec, value.key)) {
            result.notApplied.push_back(PropertyName(value.key, uiLanguage));
            continue;
        }
        if (SUCCEEDED(target.codec->SetValue(
                &value.key, const_cast<VARIANT*>(&value.value))))
            ++result.applied;
        else
            result.notApplied.push_back(PropertyName(value.key, uiLanguage));
    }

    if (PropertySupported(target.codec, CODECAPI_AVEncCommonMeanBitRate) &&
        PropertyModifiable(target.codec, CODECAPI_AVEncCommonMeanBitRate)) {
        VARIANT bitrate{};
        VariantInit(&bitrate);
        bitrate.vt = VT_UI4;
        bitrate.ulVal = std::clamp<std::uint32_t>(averageBitrate,
                                                  250'000, 100'000'000);
        target.codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &bitrate);
    }
    return result;
}

// One-shot warning so a setting that did not reach the encoder is visible
// (the export still runs).  Reports what was dropped and whether the encoder
// used for the export matches the one the settings window enumerated.
void ReportApplyResult(HWND owner, const ApplyResult& result,
                       unsigned char uiLanguage) {
    if (!result.anySaved)
        return;
    const bool droppedSome = !result.notApplied.empty();
    if (result.encoderFound && result.surfaceMatched && !droppedSome)
        return;  // every saved setting reached the configured encoder
    wchar_t names[256]{};
    std::size_t used = 0;
    for (const wchar_t* name : result.notApplied) {
        const int written = swprintf_s(names + used, std::size(names) - used,
            used == 0 ? L"%ls" : UiText(uiLanguage, L", %ls", L"、%ls", L"、%ls"), name);
        if (written <= 0)
            break;
        used += static_cast<std::size_t>(written);
    }
    wchar_t message[768]{};
    const wchar_t* droppedText = UiText(uiLanguage,
        L"Some settings were not applied to the encoder used for this export; export continues: ",
        L"部分设置未应用到本次导出使用的编码器，导出将继续：",
        L"一部の設定を今回の出力に使うエンコーダーへ適用できませんでした。出力は続行します：");
    const wchar_t* missingEncoderText = UiText(uiLanguage,
        L"The encoder in the export pipeline could not be resolved; saved settings were not applied.\n",
        L"未能识别导出管线中的编码器；已保存的设置未应用。\n",
        L"出力パイプラインのエンコーダーを特定できず、保存した設定は適用されませんでした。\n");
    const wchar_t* mismatchText = UiText(uiLanguage,
        L"The export encoder differs from the encoder shown in the settings window.\n",
        L"本次导出使用的编码器与设置窗口中的编码器不同。\n",
        L"今回の出力に使うエンコーダーが設定画面で表示したものと異なります。\n");
    swprintf_s(message, L"%ls%ls%ls%ls",
        droppedSome ? droppedText : L"",
        droppedSome ? names : L"",
        droppedSome ? L"\n" : L"",
        result.encoderFound
            ? (result.surfaceMatched ? L"" : mismatchText)
            : missingEncoderText);
    MessageBoxW(owner, message,
        UiText(uiLanguage, L"Encoder settings", L"编码器设置", L"エンコーダー設定"),
        MB_OK | MB_ICONWARNING);
}

HRESULT WriteAudioSamplesThrough(Recording& recording,
                                 std::uint64_t targetSampleCount) {
    if (!recording.hasAudio || recording.audioDataBytes == 0)
        return S_OK;
    const std::uint64_t totalSamples =
        recording.audioDataBytes / recording.audioBlockAlign;
    targetSampleCount = std::min(targetSampleCount, totalSamples);
    const std::uint64_t maxChunkBytes =
        std::uint64_t(recording.audioSampleRate) * recording.audioBlockAlign;
    const std::uint32_t chunkCapacity = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(std::max<std::uint64_t>(maxChunkBytes,
            recording.audioBlockAlign), MAXDWORD));
    std::unique_ptr<BYTE[]> bytes(new (std::nothrow) BYTE[chunkCapacity]);
    if (!bytes)
        return E_OUTOFMEMORY;

    while (recording.audioSamplesWritten < targetSampleCount) {
        const std::uint64_t samplesWanted = std::min<std::uint64_t>(
            targetSampleCount - recording.audioSamplesWritten,
            chunkCapacity / recording.audioBlockAlign);
        const DWORD wanted = static_cast<DWORD>(
            samplesWanted * recording.audioBlockAlign);
        DWORD read = 0;
        if (!ReadFile(recording.audioFile, bytes.get(), wanted, &read, nullptr) ||
            read == 0)
            return HRESULT_FROM_WIN32(GetLastError());
        read -= read % recording.audioBlockAlign;
        if (read == 0)
            return MF_E_INVALID_FILE_FORMAT;

        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(read, &buffer);
        BYTE* destination = nullptr;
        DWORD maximum = 0;
        if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, &maximum, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(destination, bytes.get(), read);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(read);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);

        const std::uint64_t sampleCount = read / recording.audioBlockAlign;
        const LONGLONG time = static_cast<LONGLONG>(
            recording.audioSamplesWritten * 10'000'000ull /
            recording.audioSampleRate);
        const LONGLONG duration = static_cast<LONGLONG>(
            sampleCount * 10'000'000ull / recording.audioSampleRate);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
        if (SUCCEEDED(hr))
            hr = recording.writer->WriteSample(recording.audioStream, sample);

        if (sample != nullptr) sample->Release();
        if (buffer != nullptr) buffer->Release();
        if (FAILED(hr)) return hr;
        recording.audioSamplesWritten += sampleCount;
    }
    return S_OK;
}

}  // namespace

bool ShowH264EncoderSettingsDialog(void* ownerWindow,
                                  bool enableSystemEncoders,
                                  std::uint32_t& averageBitrate,
                                  int exportWidth, int exportHeight,
                                  float exportFps,
                                  unsigned char uiLanguage, void* uiFont) {
    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(startup))
        return false;

    // Probe a throw-away Sink Writer built exactly like the export one and
    // enumerate the encoder it selects, so the rows below are the options of
    // the encoder the export will actually use.
    EncoderProbe probe;
    bool probed = false;
    if (enableSystemEncoders && !g_hardwareEncoderUnsafe) {
        probed = CreateEncoderProbeGuarded(true, exportWidth, exportHeight,
                                           exportFps, averageBitrate, probe);
        if (!probed) {
            // Either the hardware chain faulted inside the vendor MFT or it
            // exposed no configurable encoder.  Drop whatever it left behind
            // and enumerate the software MFT instead; the session stays on
            // software so the export cannot hit the same fault.
            g_hardwareEncoderUnsafe = true;
            AbandonEncoderProbe(probe);
        }
    }
    if (!probed) {
        probed = CreateEncoderProbeGuarded(false, exportWidth, exportHeight,
                                           exportFps, averageBitrate, probe);
    }

    EncoderSettingsDialog state;
    state.owner = static_cast<HWND>(ownerWindow);
    state.font = static_cast<HFONT>(uiFont);
    state.uiLanguage = uiLanguage;
    state.codec = probe.target.codec;
    state.averageBitrate = &averageBitrate;
    state.group = enableSystemEncoders && !g_hardwareEncoderUnsafe ? 1 : 0;
    state.surfaceHash = probe.target.surfaceHash;
    state.hasSurfaceHash = probed;

    if (state.codec != nullptr) {
        std::vector<CodecOverride> saved;
        {
            std::lock_guard<std::mutex> lock(gCodecOverridesMutex);
            saved = gCodecGroups[state.group].overrides;
        }
        for (const CodecOverride& value : saved) {
            if (PropertySupported(state.codec, value.key) &&
                PropertyModifiable(state.codec, value.key))
                state.codec->SetValue(&value.key,
                                      const_cast<VARIANT*>(&value.value));
        }

        for (const CodecProperty& property : kH264Properties) {
            if (IsEqualGUID(property.key, CODECAPI_AVEncCommonMeanBitRate))
                continue; // edited in the prominent Mbps field below
            // Only options this encoder reports supported AND modifiable.
            if (!PropertySupported(state.codec, property.key) ||
                !PropertyModifiable(state.codec, property.key))
                continue;
            EncoderSettingRow row;
            row.property = &property;
            if (FAILED(state.codec->GetValue(&property.key, &row.current)) ||
                !IsEditableVariant(row.current.vt))
                continue;

            // GetParameterValues returns exactly the values the encoder
            // accepts for this key (e.g. CBR / VBR / Quality for rate
            // control), so the list never offers unsupported choices.
            VARIANT* values = nullptr;
            ULONG valueCount = 0;
            if (SUCCEEDED(state.codec->GetParameterValues(&property.key, &values,
                                                    &valueCount)) &&
                values != nullptr && valueCount > 0 && valueCount <= 64) {
                row.isList = true;
                for (ULONG i = 0; i < valueCount; ++i) {
                    VARIANT value{};
                    VariantInit(&value);
                    if (SUCCEEDED(VariantCopy(&value, &values[i])))
                        row.values.push_back(value);
                }
            } else if (row.current.vt == VT_BOOL) {
                row.isList = true;
                VARIANT off{};
                off.vt = VT_BOOL;
                off.boolVal = VARIANT_FALSE;
                row.values.push_back(off);
                VARIANT on{};
                on.vt = VT_BOOL;
                on.boolVal = VARIANT_TRUE;
                row.values.push_back(on);
            }
            if (values != nullptr) {
                for (ULONG i = 0; i < valueCount; ++i)
                    VariantClear(&values[i]);
                CoTaskMemFree(values);
            }
            if (row.isList && row.values.empty())
                row.isList = false;
            state.rows.emplace_back(std::move(row));
        }
    }

    static constexpr wchar_t kClassName[] = L"MMS.H264EncoderSettings";
    static std::once_flag classOnce;
    static bool classRegistered = false;
    std::call_once(classOnce, [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EncoderSettingsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClassName;
        classRegistered = RegisterClassW(&wc) != 0 ||
                          GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });
    if (!classRegistered) {
        CloseEncoderProbe(probe);
        MFShutdown();
        return false;
    }

    const int shownRows = std::min<int>(12, static_cast<int>(state.rows.size()));
    const int clientHeight = 64 + shownRows * 32 + 64;
    RECT windowRect{0, 0, 600, clientHeight};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&windowRect, style, FALSE, WS_EX_DLGMODALFRAME);
    HWND owner = state.owner;
    RECT ownerRect{};
    if (owner != nullptr && IsWindow(owner)) GetWindowRect(owner, &ownerRect);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        UiText(state.uiLanguage, L"H.264 Encoder Settings",
               L"H.264 编码器设置", L"H.264 エンコーダー設定"), style,
        x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (window == nullptr) {
        CloseEncoderProbe(probe);
        MFShutdown();
        return false;
    }

    CreateWindowExW(0, L"STATIC",
        UiText(state.uiLanguage, L"Average bit rate (Mbps)",
               L"平均码率 (Mbps)", L"平均ビットレート (Mbps)"),
        WS_CHILD | WS_VISIBLE, 16, 18, 240, 24, window, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    state.bitrate = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        258, 16, 130, 25, window, reinterpret_cast<HMENU>(3000),
        GetModuleHandleW(nullptr), nullptr);
    wchar_t bitrateText[48]{};
    swprintf_s(bitrateText, L"%.3f", averageBitrate / 1'000'000.0);
    SetWindowTextW(state.bitrate, bitrateText);
    const bool hardwareBlocked = enableSystemEncoders && g_hardwareEncoderUnsafe;
    CreateWindowExW(0, L"STATIC",
        hardwareBlocked
            ? UiText(state.uiLanguage,
                L"Hardware encoder disabled after a driver fault; using the software H.264 encoder:",
                L"硬件编码器在驱动故障后已停用，改用软件 H.264 编码器：",
                L"ハードウェアエンコーダーはドライバー障害のため無効です。ソフトウェア H.264 エンコーダーを使用します：")
        : probed
            ? UiText(state.uiLanguage,
                L"Editable standard options supported by the selected encoder:",
                L"所选编码器支持且可修改的标准属性：",
                L"選択中のエンコーダーが対応する編集可能な標準設定：")
            : UiText(state.uiLanguage,
                L"No configurable Media Foundation encoder was found; only bit rate is available.",
                L"未找到可配置的 Media Foundation 编码器；当前仅能设置码率。",
                L"設定可能な Media Foundation エンコーダーが見つかりません。ビットレートのみ設定できます。"),
        WS_CHILD | WS_VISIBLE, 16, 44, 560, 18, window, nullptr,
        GetModuleHandleW(nullptr), nullptr);

    for (std::size_t i = 0; i < state.rows.size(); ++i) {
        EncoderSettingRow& row = state.rows[i];
        const int yRow = 64 + static_cast<int>(i) * 32;
        HWND label = CreateWindowExW(0, L"STATIC",
            PropertyName(row.property->key, state.uiLanguage),
            WS_CHILD | WS_VISIBLE, 16, yRow, 265, 24, window,
            reinterpret_cast<HMENU>(1000 + i), GetModuleHandleW(nullptr), nullptr);
        (void)label;
        if (row.isList) {
            row.editor = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                288, yRow, 282, 180, window, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            for (std::size_t j = 0; j < row.values.size(); ++j) {
                const std::wstring item = VariantText(row.values[j], &row.property->key,
                                                      state.uiLanguage);
                const LRESULT index = SendMessageW(row.editor, CB_ADDSTRING, 0,
                                                    reinterpret_cast<LPARAM>(item.c_str()));
                SendMessageW(row.editor, CB_SETITEMDATA, index,
                             static_cast<LPARAM>(j));
                if (VariantText(row.values[j], &row.property->key, state.uiLanguage) ==
                    VariantText(row.current, &row.property->key, state.uiLanguage))
                    SendMessageW(row.editor, CB_SETCURSEL, index, 0);
            }
        } else {
            const std::wstring current = VariantText(row.current, &row.property->key,
                                                     state.uiLanguage);
            row.editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", current.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                288, yRow, 282, 24, window, nullptr,
                GetModuleHandleW(nullptr), nullptr);
        }
    }
    const int buttonY = clientHeight - 44;
    CreateWindowExW(0, L"BUTTON",
        UiText(state.uiLanguage, L"OK", L"确定", L"確定"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        372, buttonY, 88, 28, window, reinterpret_cast<HMENU>(IDOK),
        GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON",
        UiText(state.uiLanguage, L"Cancel", L"取消", L"キャンセル"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        472, buttonY, 88, 28, window, reinterpret_cast<HMENU>(IDCANCEL),
        GetModuleHandleW(nullptr), nullptr);
    if (state.rows.size() > 12) {
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_RANGE | SIF_PAGE;
        scroll.nMin = 0;
        scroll.nMax = static_cast<int>(state.rows.size()) - 1;
        scroll.nPage = 12;
        SetScrollInfo(window, SB_VERT, &scroll, TRUE);
        LayoutEncoderRows(&state);
    }

    if (state.font == nullptr && owner != nullptr && IsWindow(owner))
        state.font = reinterpret_cast<HFONT>(SendMessageW(owner, WM_GETFONT, 0, 0));
    if (state.font == nullptr)
        state.font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), FALSE);
    EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(state.font));

    if (owner != nullptr && IsWindowEnabled(owner)) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner != nullptr && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    CloseEncoderProbe(probe);
    MFShutdown();
    return state.accepted;
}

std::vector<std::wstring> EnumerateH264Encoders() {
    std::vector<std::wstring> names;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
        return names;
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_LOCALMFT |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr,
                            &output, &activations, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* friendlyName = nullptr;
            UINT32 length = 0;
            if (SUCCEEDED(activations[i]->GetAllocatedString(
                    MFT_FRIENDLY_NAME_Attribute, &friendlyName, &length)) &&
                friendlyName != nullptr) {
                names.emplace_back(friendlyName, length);
                CoTaskMemFree(friendlyName);
            }
            activations[i]->Release();
        }
        CoTaskMemFree(activations);
    }
    MFShutdown();
    return names;
}

// Creates the Sink Writer and adds the H.264 video stream under an SEH guard.
// Only scalars live in this frame, so the __try block does not need C++
// unwinding.  See g_hardwareEncoderUnsafe above for why the hardware chain is
// isolated instead of trusted.
HRESULT OpenH264SinkWriterGuarded(IMFSinkWriter** writer, const wchar_t* path,
                                  bool hardware, std::int32_t width,
                                  std::int32_t height, float fps,
                                  std::uint32_t averageBitrate,
                                  bool nv12Input, DWORD* stream) noexcept {
    __try {
        IMFAttributes* attributes = nullptr;
        HRESULT hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr))
            hr = attributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                hardware ? TRUE : FALSE);
        if (SUCCEEDED(hr))
            hr = MFCreateSinkWriterFromURL(path, nullptr, attributes, writer);
        if (attributes != nullptr) attributes->Release();
        if (SUCCEEDED(hr))
            hr = AddH264VideoStream(*writer, width, height, fps,
                                    averageBitrate, nv12Input, stream);
        return hr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime_log::Write(
            "H264 export writer faulted code=0x%08lX hardware=%d",
            static_cast<unsigned long>(GetExceptionCode()), hardware ? 1 : 0);
        return E_FAIL;
    }
}

// Releases a Sink Writer whose encoder MFT faulted, swallowing a second fault
// so the editor survives.  Reaching the writer's Release is worth attempting
// because it closes the output file the hardware attempt created; if the
// teardown faults too, the objects are leaked on purpose.
void ReleaseSinkWriterQuietly(IMFSinkWriter*& writer) noexcept {
    IMFSinkWriter* local = writer;
    writer = nullptr;
    if (local == nullptr)
        return;
    __try {
        local->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime_log::Write(
            "H264 writer release faulted code=0x%08lX",
            static_cast<unsigned long>(GetExceptionCode()));
    }
}

// Initialize the entire encoder chain before accepting an export attempt.
// BeginWriting and ICodecAPI can instantiate vendor code just like AddStream.
HRESULT StartH264SinkWriter(Recording* recording, const wchar_t* path,
                           std::int32_t width, std::int32_t height, float fps,
                           const WaveAudioContext* audio, bool hardwareActive,
                           std::uint32_t averageBitrate, ApplyResult& settings) {
    HRESULT hr = OpenH264SinkWriterGuarded(&recording->writer, path, hardwareActive,
        width, height, fps, averageBitrate, hardwareActive, &recording->videoStream);
    if (FAILED(hr)) return hr;
    settings = ApplyConfiguredEncoderSettings(recording->writer,
        recording->videoStream, hardwareActive, averageBitrate, recording->uiLanguage);
    if (recording->hasAudio) {
        IMFMediaType* audioOutput = nullptr;
        IMFMediaType* audioInput = nullptr;
        hr = MFCreateMediaType(&audioOutput);
        if (SUCCEEDED(hr)) {
            audioOutput->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            audioOutput->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            audioOutput->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                   audio->format.nChannels);
            audioOutput->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                   audio->format.nSamplesPerSec);
            audioOutput->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            audioOutput->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
            audioOutput->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            hr = recording->writer->AddStream(audioOutput,
                                               &recording->audioStream);
        }
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&audioInput);
        if (SUCCEEDED(hr)) {
            audioInput->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            audioInput->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            audioInput->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                  audio->format.nChannels);
            audioInput->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                  audio->format.nSamplesPerSec);
            audioInput->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
                                  audio->format.wBitsPerSample);
            audioInput->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                  audio->format.nBlockAlign);
            audioInput->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                  audio->format.nAvgBytesPerSec);
            audioInput->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            hr = recording->writer->SetInputMediaType(recording->audioStream,
                                                       audioInput, nullptr);
        }
        if (audioInput != nullptr) audioInput->Release();
        if (audioOutput != nullptr) audioOutput->Release();
        if (FAILED(hr)) return hr;

    }
    hr = recording->writer->BeginWriting();
    if (FAILED(hr)) return hr;
    if (!settings.encoderFound) {
        // Some Sink Writer builds instantiate the encoder chain only at
        // BeginWriting; configure the encoder then, before the first frame.
        settings = ApplyConfiguredEncoderSettings(recording->writer,
                                                  recording->videoStream,
                                                  hardwareActive,
                                                  averageBitrate,
                                                  recording->uiLanguage);
    }
    return S_OK;
}

HRESULT StartH264SinkWriterGuarded(Recording* recording, const wchar_t* path,
                                  std::int32_t width, std::int32_t height, float fps,
                                  const WaveAudioContext* audio, bool hardwareActive,
                                  std::uint32_t averageBitrate, ApplyResult& settings) noexcept {
    __try {
        return StartH264SinkWriter(recording, path, width, height, fps, audio,
                                  hardwareActive, averageBitrate, settings);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime_log::Write("H264 export initialization faulted code=0x%08lX hardware=%d",
            static_cast<unsigned long>(GetExceptionCode()), hardwareActive ? 1 : 0);
        return E_FAIL;
    }
}

bool StartBuiltInH264Recording(DShowRecorder* recorder, const wchar_t* path,
                               std::int32_t width, std::int32_t height,
                               float fps, const WaveAudioContext* audio,
                               bool enableSystemEncoders,
                               std::uint32_t averageBitrate,
                               void* ownerWindow,
                               unsigned char uiLanguage) {
    if (recorder == nullptr || path == nullptr || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 || (width & 1) != 0 ||
        (height & 1) != 0 || !std::isfinite(fps) || fps <= 0.0f)
        return false;

    auto recording = std::make_unique<Recording>();
    recording->owner = static_cast<HWND>(ownerWindow);
    recording->uiLanguage = uiLanguage;
    recording->frameBytes = std::uint64_t(width) * height * 4;
    if (recording->frameBytes > MAXDWORD)
        return false;
    recording->fpsNumerator = static_cast<std::uint32_t>(std::lround(fps * 1000.0));
    recording->fpsDenominator = 1000;
    const std::uint32_t divisor = std::gcd(recording->fpsNumerator,
                                           recording->fpsDenominator);
    recording->fpsNumerator /= divisor;
    recording->fpsDenominator /= divisor;
    recording->frameDuration = 10'000'000ull * recording->fpsDenominator /
                               recording->fpsNumerator;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) return false;
    recording->mfStarted = true;
    // Validate/open the source audio once. A bad input file must not disable
    // hardware encoding, and a retry must not leak or advance its file handle.
    if (audio != nullptr && audio->path[0] != L'\0' && audio->dataSize > 0) {
        if (audio->format.wFormatTag != WAVE_FORMAT_PCM ||
            audio->format.nChannels == 0 || audio->format.nSamplesPerSec == 0 ||
            audio->format.nBlockAlign == 0 || audio->format.wBitsPerSample == 0)
            return false;
        recording->audioFile = CreateFileW(audio->path, GENERIC_READ,
                                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (recording->audioFile == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER offset{};
        offset.QuadPart = audio->dataOffset;
        if (!SetFilePointerEx(recording->audioFile, offset, nullptr, FILE_BEGIN))
            return false;
        recording->audioDataBytes = static_cast<std::uint32_t>(audio->dataSize);
        recording->audioDataOffset = static_cast<std::uint32_t>(audio->dataOffset);
        recording->audioBlockAlign = audio->format.nBlockAlign;
        recording->audioSampleRate = audio->format.nSamplesPerSec;
        recording->hasAudio = true;
    }

    recording->width = width;
    recording->height = height;
    bool hardwareActive = enableSystemEncoders && !g_hardwareEncoderUnsafe;
    ApplyResult settings;
    hr = StartH264SinkWriterGuarded(recording.get(), path, width, height, fps,
                                  audio, hardwareActive, averageBitrate, settings);
    if (FAILED(hr) && hardwareActive) {
        g_hardwareEncoderUnsafe = true;
        hardwareActive = false;
        ReleaseSinkWriterQuietly(recording->writer);
        recording->videoStream = 0;
        recording->audioStream = 0;
        settings = ApplyResult{};
        hr = StartH264SinkWriterGuarded(recording.get(), path, width, height, fps,
                                      audio, false, averageBitrate, settings);
        if (SUCCEEDED(hr))
            ReportHardwareFallbackOnce(recording->owner, recording->uiLanguage);
    }
    if (FAILED(hr)) return false;
    recording->nv12Input = hardwareActive;
    ReportApplyResult(recording->owner, settings, recording->uiLanguage);
    std::lock_guard<std::mutex> lock(gRecordingsMutex);
    gRecordings[recorder] = std::move(recording);
    return true;
}

bool IsBuiltInH264Recording(const DShowRecorder* recorder) {
    std::lock_guard<std::mutex> lock(gRecordingsMutex);
    return recorder != nullptr && gRecordings.find(recorder) != gRecordings.end();
}

bool WriteBuiltInH264Frame(DShowRecorder* recorder, const void* bgra,
                           std::size_t bytes) {
    if (bgra == nullptr) return false;
    std::lock_guard<std::mutex> lock(gRecordingsMutex);
    const auto found = gRecordings.find(recorder);
    if (found == gRecordings.end()) return false;
    Recording& recording = *found->second;
    if (bytes != recording.frameBytes) return false;

    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    const std::uint64_t sampleBytes = recording.nv12Input
        ? std::uint64_t(recording.width) * recording.height * 3 / 2
        : recording.frameBytes;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(sampleBytes), &buffer);
    BYTE* destination = nullptr;
    DWORD maximum = 0;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, &maximum, nullptr);
    if (SUCCEEDED(hr)) {
        if (recording.nv12Input) {
            recording.convertScratch.resize(
                static_cast<std::size_t>(sampleBytes));
            ConvertBgraToNv12(static_cast<const unsigned char*>(bgra),
                              recording.width, recording.height,
                              recording.convertScratch.data());
            memcpy(destination, recording.convertScratch.data(),
                   static_cast<std::size_t>(sampleBytes));
        } else {
            memcpy(destination, bgra, bytes);
        }
        buffer->Unlock();
        hr = buffer->SetCurrentLength(static_cast<DWORD>(sampleBytes));
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    const LONGLONG time = static_cast<LONGLONG>(
        recording.frameIndex * recording.frameDuration);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(
        static_cast<LONGLONG>(recording.frameDuration));
    if (SUCCEEDED(hr))
        hr = recording.writer->WriteSample(recording.videoStream, sample);
    if (sample != nullptr) sample->Release();
    if (buffer != nullptr) buffer->Release();
    if (SUCCEEDED(hr)) {
        ++recording.frameIndex;
        const std::uint64_t audioSamplesToWrite =
            recording.frameIndex * recording.frameDuration *
            recording.audioSampleRate / 10'000'000ull;
        hr = WriteAudioSamplesThrough(recording, audioSamplesToWrite);
    }
    if (FAILED(hr))
        ReportFailure(recording.owner, hr);
    return SUCCEEDED(hr);
}

bool FinishBuiltInH264Recording(DShowRecorder* recorder,
                                std::uint64_t expectedFrames) {
    std::unique_ptr<Recording> recording;
    {
        std::lock_guard<std::mutex> lock(gRecordingsMutex);
        const auto found = gRecordings.find(recorder);
        if (found == gRecordings.end()) return false;
        recording = std::move(found->second);
        gRecordings.erase(found);
    }
    const std::uint64_t audioSamplesAtVideoEnd =
        recording->frameIndex * recording->frameDuration *
        recording->audioSampleRate / 10'000'000ull;
    const std::uint64_t totalAudioSamples = recording->audioBlockAlign == 0
        ? 0 : std::min<std::uint64_t>(
              recording->audioDataBytes / recording->audioBlockAlign,
              audioSamplesAtVideoEnd);
    HRESULT hr = WriteAudioSamplesThrough(*recording, totalAudioSamples);
    const bool completeFrameCount = expectedFrames != 0 &&
        recording->frameIndex == expectedFrames;
    if (SUCCEEDED(hr)) hr = recording->writer->Finalize();
    if (FAILED(hr)) ReportFailure(recording->owner, hr);
    return SUCCEEDED(hr) && completeFrameCount;
}

void ResetH264HardwareEncoderState() {
    if (!g_hardwareEncoderUnsafe && !g_hardwareFallbackNoticeShown)
        return;
    g_hardwareEncoderUnsafe = false;
    g_hardwareFallbackNoticeShown = false;
    runtime_log::Write("H264 hardware encoder re-enabled by the user");
}

}  // namespace mikumikustudio
