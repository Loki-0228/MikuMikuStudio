// MAINTENANCE: regression-keep. Uses the production probe and cleanup functions,
// but replaces the Sink Writer factory before it can instantiate a vendor MFT.
// Run only a copied executable in a marked installation with no existing data.
namespace probe_storage {
namespace fs = std::filesystem;
const std::string fixtureBytes = "MMS owned encoder probe fixture\n";
fs::path install, probeRoot, openedPath;
unsigned factoryCalls = 0, codecReleases = 0, writerObservations = 0;
bool throwFromFactory = false, expectedHardware = false;
EncoderProbe* activeProbe = nullptr;

std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(bool(input), "open owned storage fixture");
    std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Check(!input.bad(), "read complete storage fixture");
    return result;
}
void CreateOwned(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create a new fixture without replacing another file");
    DWORD count = 0;
    const BOOL written = WriteFile(file, fixtureBytes.data(), static_cast<DWORD>(fixtureBytes.size()), &count, nullptr);
    Check(CloseHandle(file) != FALSE, "close owned fixture handle");
    Check(written && count == fixtureBytes.size(), "write complete owned fixture");
}
void RemoveOwned(const fs::path& path) {
    Check(Bytes(path) == fixtureBytes, "verify owned fixture bytes before removal");
    Check(DeleteFileW(path.c_str()) != FALSE, "remove only the exact owned fixture");
}
bool Empty(const EncoderProbe& probe) {
    return !probe.writer && !probe.target.codec && !probe.target.transform &&
        !probe.target.index && !probe.target.transformCount && !probe.target.surfaceHash &&
        probe.file.empty() && !probe.started;
}
struct Codec final : ICodecAPI {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** value) override { *value = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override {
        Check(activeProbe && activeProbe->writer && Bytes(openedPath) == fixtureBytes,
              "target codec releases before the writer and before its file is deleted");
        ++codecReleases; return 1;
    }
    HRESULT STDMETHODCALLTYPE IsSupported(const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsModifiable(const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID*, VARIANT**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID*, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetValue(const GUID*, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetValue(const GUID*, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID*, LONG_PTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAllDefaults() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetValueWithNotify(const GUID*, VARIANT*, GUID**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAllDefaultsWithNotify(GUID**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAllSettings(IStream*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAllSettings(IStream*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAllSettingsWithNotify(IStream*, GUID**, ULONG*) override { return E_NOTIMPL; }
} codec;
void ObserveWriterRelease() {
    Check(activeProbe && !activeProbe->target.codec && !activeProbe->target.transform && codecReleases == 1,
          "normal close clears the target before releasing the writer");
    Check(Bytes(openedPath) == fixtureBytes, "writer Release sees its output file still present");
    ++writerObservations;
}
HRESULT WINAPI Factory(LPCWSTR url, IMFByteStream* stream, IMFAttributes* attributes, IMFSinkWriter** writer) {
    ++factoryCalls;
    Check(url && !stream && attributes && writer, "production supplies a file URL and writer attributes");
    openedPath = fs::path(url).lexically_normal();
    Check(openedPath.is_absolute() && openedPath.parent_path() == probeRoot &&
          openedPath.filename().wstring().find(L"MMS-encoder-probe-") == 0 && openedPath.extension() == L".mp4",
          "actual Sink Writer URL is inside this copied installation data/encoder_probes");
    UINT32 hardware = 99;
    Check(SUCCEEDED(attributes->GetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, &hardware)) &&
          hardware == (expectedHardware ? TRUE : FALSE), "production preserves the requested hardware attribute");
    CreateOwned(openedPath);
    *writer = nullptr;
    if (throwFromFactory) {
        Check(activeProbe != nullptr, "fault injection has an owned probe");
        *writer = &writers[0];
        activeProbe->target.codec = &codec;
        activeProbe->target.index = 3;
        activeProbe->target.transformCount = 4;
        activeProbe->target.surfaceHash = 123;
        activeProbe->started = true;
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return E_FAIL;
}
DWORD WINAPI MissingModule(HMODULE, LPWSTR, DWORD) { SetLastError(ERROR_GEN_FAILURE); return 0; }
struct EnvironmentValue {
    const wchar_t* name;
    std::wstring previous;
    bool existed = false;
    explicit EnvironmentValue(const wchar_t* value) : name(value) {
        const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
        if (length) {
            previous.resize(length);
            Check(GetEnvironmentVariableW(name, previous.data(), length) == length - 1, "save inherited environment value");
            previous.resize(length - 1); existed = true;
        }
    }
    void Set(const fs::path& path) { Check(SetEnvironmentVariableW(name, path.c_str()) != FALSE, "set only this process environment"); }
    ~EnvironmentValue() { Check(SetEnvironmentVariableW(name, existed ? previous.c_str() : nullptr) != FALSE, "restore inherited environment value"); }
};
struct Environment {
    fs::path oldCwd = fs::current_path(), cwd, temporary;
    EnvironmentValue temp{L"TEMP"}, tmp{L"TMP"};
    explicit Environment(const fs::path& output) : cwd(output / L"foreign-cwd"), temporary(output / L"foreign-temp") {
        Check(fs::create_directory(cwd) && fs::create_directory(temporary), "create owned alternate cwd and TEMP");
        temp.Set(temporary); tmp.Set(temporary);
        Check(SetCurrentDirectoryW(cwd.c_str()) != FALSE, "change only this process current directory");
        wchar_t value[32768]{};
        const DWORD length = GetTempPathW(32768, value);
        Check(length && length < 32768 && fs::equivalent(fs::path(value), temporary), "Windows TEMP really resolves outside the copied installation");
    }
    void NoFallback() const {
        Check(fs::is_empty(cwd) && fs::is_empty(temporary), "neither cwd nor TEMP receives a fallback file or directory");
    }
    ~Environment() { Check(SetCurrentDirectoryW(oldCwd.c_str()) != FALSE, "restore inherited current directory"); }
};
void Run(const fs::path& outputArgument) {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    Check(length && length < 32768, "independently resolve actual copied test executable");
    install = fs::canonical(fs::path(executable).parent_path());
    Check(Bytes(install / L"encoder-probe-test-installation.txt") == "MMS encoder probe test installation v1\n",
          "explicit copied installation marker has exact LF bytes");
    Check(!fs::exists(install / L"data"), "copied installation starts without a data directory");
    Check(outputArgument.is_absolute(), "output argument is absolute");
    const auto output = outputArgument.lexically_normal();
    Check(!fs::exists(output) && fs::is_directory(output.parent_path()) &&
          fs::canonical(output.parent_path()) != install && output != install,
          "output is fresh and separate from the copied installation");
    Check(fs::create_directory(output), "create only the fresh explicit output directory");
    Environment environment(fs::canonical(output));
    probeRoot = install / L"data" / L"encoder_probes";
    std::printf("probe storage installation=%ls\noutput=%ls\n", install.c_str(), output.c_str());
    Check(MH_Initialize() == MH_OK, "initialize only process-local storage hooks");
    Hook(MFCreateSinkWriterFromURL, Factory);
    Hook(GetModuleFileNameW, MissingModule);
    Check(MH_EnableHook(reinterpret_cast<void*>(MFCreateSinkWriterFromURL)) == MH_OK, "intercept factory before any vendor encoder can load");

    EncoderProbe probe;
    Check(MH_EnableHook(reinterpret_cast<void*>(GetModuleFileNameW)) == MH_OK, "inject unavailable executable location");
    const bool missingResult = CreateEncoderProbe(false, 16, 16, 30.f, 1000000, probe);
    Check(MH_DisableHook(reinterpret_cast<void*>(GetModuleFileNameW)) == MH_OK, "restore executable location lookup");
    Check(!missingResult && !factoryCalls && Empty(probe) && !fs::exists(install / L"data"),
          "unknown executable directory refuses before MF and creates no data tree");
    environment.NoFallback();
    CreateOwned(install / L"data");
    Check(!CreateEncoderProbe(false, 16, 16, 30.f, 1000000, probe) && !factoryCalls && Empty(probe),
          "a file occupying data prevents probe creation before MF");
    RemoveOwned(install / L"data"); environment.NoFallback();
    Check(fs::create_directory(install / L"data"), "create the owned data directory after removing its owned blocker");
    CreateOwned(probeRoot);
    Check(!CreateEncoderProbe(false, 16, 16, 30.f, 1000000, probe) && !factoryCalls && Empty(probe),
          "a file occupying encoder_probes prevents probe creation before MF");
    RemoveOwned(probeRoot); environment.NoFallback();

    for (bool hardware : {false, true}) {
        expectedHardware = hardware;
        const auto before = factoryCalls;
        Check(!CreateEncoderProbe(hardware, 16, 16, 30.f, 1000000, probe) && factoryCalls == before + 1,
              "real probe reaches the controlled failing factory once");
        Check(Empty(probe) && !fs::exists(openedPath), "ordinary MF failure removes the actual owned probe file and clears state");
        environment.NoFallback();
    }

    // This directly exercises successful-object cleanup, not a real encoder startup.
    openedPath = probeRoot / L"manual-close.mp4";
    CreateOwned(openedPath);
    probe.file = openedPath.wstring(); probe.writer = &writers[0]; probe.target.codec = &codec;
    probe.target.index = 3; probe.target.transformCount = 4; probe.target.surfaceHash = 123; probe.started = true;
    activeProbe = &probe; codecReleases = writerObservations = 0; releases = 0;
    writers[0].hardware = false; fault = Fault::None;
    storageWriterReleaseObserver = ObserveWriterRelease;
    CloseEncoderProbe(probe);
    Check(codecReleases == 1 && writerObservations == 1 && releases == 1 && Empty(probe) && !fs::exists(openedPath),
          "normal close releases codec and writer once in order, then deletes its file and clears state");
    CloseEncoderProbe(probe);
    Check(codecReleases == 1 && writerObservations == 1 && releases == 1, "closing an empty probe is idempotent");

    // Last creation: the faulted file intentionally survives production abandonment.
    throwFromFactory = true; expectedHardware = true;
    codecReleases = writerObservations = 0; releases = 0;
    const auto before = factoryCalls;
    Check(!CreateEncoderProbeGuarded(true, 16, 16, 30.f, 1000000, probe) && factoryCalls == before + 1 &&
          probe.writer == &writers[0] && probe.target.codec == &codec && probe.started &&
          probe.target.index == 3 && probe.target.transformCount == 4 && probe.target.surfaceHash == 123 &&
          probe.file == openedPath.wstring() && Bytes(openedPath) == fixtureBytes,
          "real SEH guard returns the partially acquired owned probe without cleanup");
    AbandonEncoderProbe(probe);
    Check(!codecReleases && !writerObservations && !releases && Empty(probe) && Bytes(openedPath) == fixtureBytes,
          "abandon clears probe fields without releasing a faulted object or deleting its file");
    CloseEncoderProbe(probe);
    Check(!codecReleases && !writerObservations && !releases && Bytes(openedPath) == fixtureBytes,
          "later normal close cannot release or delete abandoned state");
    storageWriterReleaseObserver = nullptr; activeProbe = nullptr;
    RemoveOwned(openedPath); // Test-owned fake writer has no live file handle or vendor state.
    environment.NoFallback();
    Check(fs::is_empty(probeRoot), "only owned probe files were created and none remain after test-owned cleanup");
    Check(MH_DisableHook(MH_ALL_HOOKS) == MH_OK && MH_Uninitialize() == MH_OK, "remove all process-local storage hooks");
    std::printf("PASS: actual probe path, blocked roots, missing EXE, ordered close and SEH abandonment; no vendor MFT\n");
}
}
