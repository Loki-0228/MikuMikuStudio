// Uses the real recording startup, frame submission and MP4 publication.
// Encoder registration and the transform selected by the sink are observed separately.
struct RegisteredH264Hardware {
    struct Entry {
        GUID classId;
        std::wstring module;
    };
    std::vector<Entry> entries;
    UINT32 count = 0;
};

RegisteredH264Hardware EnumerateH264Hardware(bool requireNv12) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    RegisteredH264Hardware registered;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        requireNv12 ? &input : nullptr, &output, &activations, &registered.count);
    std::printf("hardware-registration nv12-filter=%d hr=0x%08lx count=%u\n",
        requireNv12, static_cast<unsigned long>(hr), registered.count);
    Check(SUCCEEDED(hr), "enumerate system H.264 hardware registrations");
    for (UINT32 index = 0; index < registered.count; ++index) {
        IMFActivate* activation = activations[index];
        GUID classId{};
        WCHAR name[256]{};
        WCHAR id[40]{};
        activation->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), nullptr);
        if (SUCCEEDED(activation->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &classId))) {
            StringFromGUID2(classId, id, ARRAYSIZE(id));
            const auto key = std::wstring(L"CLSID\\") + id + L"\\InprocServer32";
            WCHAR path[MAX_PATH]{};
            DWORD bytes = sizeof(path);
            std::wstring module;
            if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ,
                nullptr, path, &bytes) == ERROR_SUCCESS) {
                module = std::filesystem::path(path).filename().wstring();
            }
            registered.entries.push_back({classId, std::move(module)});
        }
        std::printf("hardware-entry name=%ls clsid=%ls\n", name, id);
        activation->Release();
    }
    CoTaskMemFree(activations);
    return registered;
}

std::wstring ModuleForEncoderAddress(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)) return {};
    WCHAR path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    return length && length < ARRAYSIZE(path) ? std::wstring(path, length) : std::wstring{};
}

void ObserveEncoderImplementation(const Recording& recording, const char* mode, const char* stage) {
    EncoderTarget encoder;
    Check(ResolveSinkWriterEncoder(recording.writer, recording.videoStream, encoder),
        "resolve the actual H.264 transform in the recording writer");
    mikumikustudio::media_compat::SinkWriterExtended* writer = nullptr;
    if (SUCCEEDED(recording.writer->QueryInterface(IID_PPV_ARGS(&writer)))) {
        for (DWORD index = 0; index < encoder.transformCount; ++index) {
            GUID category{};
            IMFTransform* transform = nullptr;
            Check(SUCCEEDED(writer->GetTransformForStream(recording.videoStream, index, &category, &transform)) &&
                transform, "read the active stream transform and category");
            IMFMediaType* mediaType = nullptr;
            GUID subtype{};
            if (SUCCEEDED(transform->GetOutputCurrentType(0, &mediaType))) {
                mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
                mediaType->Release();
            }
            std::printf("encoder-chain mode=%s stage=%s index=%lu selected=%d video-encoder=%d h264-output=%d\n",
                mode, stage, index, index == encoder.index,
                IsEqualGUID(category, MFT_CATEGORY_VIDEO_ENCODER), IsEqualGUID(subtype, MFVideoFormat_H264));
            transform->Release();
        }
        writer->Release();
    }
    // The Windows SDK's IMFTransformVtbl places ProcessInput and ProcessOutput
    // after IUnknown and the first 21 IMFTransform methods. Read addresses
    // only. These interfaces may be wrappers, whose module remains visible.
    constexpr unsigned processInputSlot = 24;
    constexpr unsigned processOutputSlot = 25;
    const void* const* transformTable = nullptr;
    const void* codecTable = nullptr;
    std::memcpy(&transformTable, encoder.transform, sizeof(transformTable));
    std::memcpy(&codecTable, encoder.codec, sizeof(codecTable));
    std::printf("encoder-implementation mode=%s stage=%s transform=%ls input=%ls output=%ls codec=%ls\n",
        mode, stage, ModuleForEncoderAddress(transformTable).c_str(),
        ModuleForEncoderAddress(transformTable[processInputSlot]).c_str(),
        ModuleForEncoderAddress(transformTable[processOutputSlot]).c_str(),
        ModuleForEncoderAddress(codecTable).c_str());
}

void ObserveSystemEncoder(const Recording& recording, const RegisteredH264Hardware& hardware,
                          const char* mode) {
    EncoderTarget encoder;
    const bool resolved = ResolveSinkWriterEncoder(recording.writer, recording.videoStream, encoder);
    GUID classId{};
    WCHAR name[256]{};
    WCHAR id[40]{};
    bool hasHardwareUrl = false;
    bool encoderNv12 = false;
    if (resolved) {
        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(encoder.transform->GetAttributes(&attributes))) {
            UINT32 length = 0;
            hasHardwareUrl = SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length)) && length != 0;
            attributes->GetString(MFT_FRIENDLY_NAME_Attribute, name, ARRAYSIZE(name), nullptr);
            attributes->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &classId);
            attributes->Release();
        }
        IPersist* persist = nullptr;
        if (IsEqualGUID(classId, GUID_NULL) && SUCCEEDED(encoder.transform->QueryInterface(IID_PPV_ARGS(&persist)))) {
            persist->GetClassID(&classId);
            persist->Release();
        }
        IMFMediaType* input = nullptr;
        if (SUCCEEDED(encoder.transform->GetInputCurrentType(0, &input))) {
            GUID subtype{};
            encoderNv12 = SUCCEEDED(input->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                IsEqualGUID(subtype, MFVideoFormat_NV12);
            input->Release();
        }
        StringFromGUID2(classId, id, ARRAYSIZE(id));
    }
    const bool registeredClass = std::any_of(hardware.entries.begin(), hardware.entries.end(),
        [&](const auto& entry) { return IsEqualGUID(entry.classId, classId); });
    std::printf("encoder mode=%s resolved=%d writer-nv12=%d transform-nv12=%d hardware-url=%d hardware-clsid=%d clsid=%ls name=%ls fallback-notice=%d\n",
        mode, resolved, recording.nv12Input, encoderNv12, hasHardwareUrl, registeredClass,
        id, name, g_hardwareFallbackNoticeShown);
}

void ObserveEncoderModules(const RegisteredH264Hardware& hardware, const char* mode, const char* stage) {
    for (const auto& entry : hardware.entries) {
        if (!entry.module.empty()) {
            std::printf("vendor-module mode=%s stage=%s name=%ls loaded=%d\n",
                mode, stage, entry.module.c_str(), GetModuleHandleW(entry.module.c_str()) != nullptr);
        }
    }
}

unsigned encoderNotices = 0;
int WINAPI ObserveEncoderNotice(HWND, LPCWSTR, LPCWSTR, UINT) {
    ++encoderNotices;
    return IDOK;
}

decltype(&OpenH264SinkWriterGuarded) realEncoderOpen = nullptr;
unsigned refusedHardwareStarts = 0;
HRESULT OpenWithControlledHardwareFailure(IMFSinkWriter** writer, const wchar_t* path, bool hardware,
        std::int32_t width, std::int32_t height, float fps, std::uint32_t bitrate, bool nv12, DWORD* stream) noexcept {
    if (hardware) {
        ++refusedHardwareStarts;
        return MF_E_INVALIDMEDIATYPE;
    }
    return realEncoderOpen(writer, path, hardware, width, height, fps, bitrate, nv12, stream);
}

void RunSystemEncoderCheck(const std::filesystem::path& root) {
    Check(std::filesystem::create_directory(root), "use a fresh owned output directory");
    const auto hardware = EnumerateH264Hardware(false);
    EnumerateH264Hardware(true);
    Check(MH_Initialize() == MH_OK, "initialize the unattended notice hook");
    Hook(MessageBoxW, ObserveEncoderNotice);
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "intercept modal notices in the test process");

    constexpr int width = 640;
    constexpr int height = 360;
    constexpr int frameCount = 5;
    for (const auto& [mode, filename, system] : {
        std::tuple{"software", L"software.mp4", false},
        std::tuple{"system", L"system.mp4", true},
        std::tuple{"fallback", L"fallback.mp4", true}}) {
        const auto path = root / filename;
        const bool controlledFailure = std::string_view(mode) == "fallback";
        if (controlledFailure) {
            Check(MH_CreateHook(reinterpret_cast<LPVOID>(OpenH264SinkWriterGuarded),
                reinterpret_cast<LPVOID>(OpenWithControlledHardwareFailure),
                reinterpret_cast<LPVOID*>(&realEncoderOpen)) == MH_OK, "hook only the controlled startup failure");
            Check(MH_EnableHook(reinterpret_cast<LPVOID>(OpenH264SinkWriterGuarded)) == MH_OK,
                "enable the startup failure after the unmodified system and software runs");
        }
        DShowRecorder recorder{};
        ResetH264HardwareEncoderState();
        encoderNotices = 0;
        ObserveEncoderModules(hardware, mode, "before-start");
        Check(StartBuiltInH264Recording(&recorder, path.c_str(), width, height, 30.f,
            nullptr, system, 4'000'000, nullptr, 1, nullptr, nullptr), "start real H.264 export");
        const auto& recording = *gRecordings.at(&recorder);
        if (controlledFailure) {
            Check(refusedHardwareStarts == 1 && g_hardwareFallbackNoticeShown && encoderNotices == 1 &&
                !recording.nv12Input, "one failed hardware start reports and continues with real RGB32 software");
        }
        ObserveSystemEncoder(recording, hardware, mode);
        ObserveEncoderImplementation(recording, mode, "after-start");
        ObserveEncoderModules(hardware, mode, "after-start");
        for (int frame = 0; frame < frameCount; ++frame) {
            auto pixels = BottomUpQuadrants(width, height);
            for (int y = 32; y < 96; ++y) {
                for (int x = 32 + frame * 48; x < 64 + frame * 48; ++x) {
                    const auto offset = (std::size_t(height - 1 - y) * width + x) * 4;
                    std::fill_n(pixels.data() + offset, 3, 255);
                }
            }
            Check(WriteBuiltInH264Frame(&recorder, pixels.data(), pixels.size()), "submit real H.264 frame");
        }
        ObserveEncoderImplementation(recording, mode, "after-frames");
        Check(FinishBuiltInH264Recording(&recorder, frameCount), "publish complete real H.264 output");
        std::printf("published mode=%s frames=%d\n", mode, frameCount);
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
