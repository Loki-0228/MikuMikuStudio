// MAINTENANCE: regression-keep; purpose in tests/README.md.
// Focused regressions for the MME D3DX bytecode cache
// (src/exports/effect_compile_cache.{hpp,cpp}):
//   * key stability and invalidation by source text, included content, macros,
//     D3DXSHADER flags, source path and compiler DLL identity;
//   * disk entry validation: truncation, wrong magic/length/digest/key and
//     oversized headers are misses that never reach D3DX;
//   * bypasses and fallbacks: custom include handler, disabled cache,
//     unreadable/oversized macros, oversized preprocessing, preprocessor and
//     compiler diagnostics, compile/create failures and an unwritable cache
//     directory; none of them becomes a load failure;
//   * the hit seam: a hit never calls CompileEffect again but creates one new
//     effect per call with the same device, pool and flags.
//
// The fake D3DX layer replaces the ID3DXBuffer/ID3DXEffectCompiler COM objects,
// so the default case needs no DirectX runtime and no device.  The explicit
// "--integration <new output directory>" case loads the system (or
// MMS_EFFECT_CACHE_TEST_D3DX) D3DX9 DLL, creates a selected D3D9 device and runs
// cold/warm/modified-include/corrupt-cache/fallback against the real compiler.
// Before any cache state it also loads the same legal FX through the untouched
// original entry point. A child with a separate cwd checks D3DX32/43 include
// lookup, nested includes and content changes with preserved timestamps.
// It writes only below the supplied output directory and keeps diagnostics.
#include "effect_compile_cache.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mikumikustudio::effect_compile_cache;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

void RunTest(const char* name, void (*test)()) {
    const int failuresBefore = g_failures;
    std::printf("[ RUN  ] %s\n", name);
    std::fflush(stdout);
    test();
    const int newFailures = g_failures - failuresBefore;
    std::printf("[ %s ] %s", newFailures == 0 ? " OK " : "FAIL", name);
    if (newFailures != 0) std::printf(" (%d new failures)", newFailures);
    std::printf("\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Files and environment (each test writes only below its own temporary folder)
// ---------------------------------------------------------------------------
int g_tempSerial = 0;

std::wstring WidenTag(const char* tag) {
    std::wstring text;
    for (const char* current = tag; current && *current; ++current)
        text.push_back(
            static_cast<wchar_t>(static_cast<unsigned char>(*current)));
    return text;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(),
                             static_cast<int>(text.size()), result.data(),
                             length, nullptr, nullptr))
        return {};
    return result;
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* tag) {
        wchar_t base[32768]{};
        const DWORD length = GetTempPathW(_countof(base), base);
        const std::filesystem::path root =
            length ? std::filesystem::path(base) : std::filesystem::path(L".");
        path = root / (std::wstring(L"mms-effect-compile-cache-") +
                       std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(++g_tempSerial) + L"-" + WidenTag(tag));
        std::error_code error;
        std::filesystem::create_directories(path, error);
        Check(!error, "temporary cache directory is created");
    }
    // Every directory carries the process id, a run serial and the case name,
    // so it can never name a pre-existing directory.  It is intentionally kept
    // for diagnostics instead of being removed recursively: the test only
    // writes below this path and never cleans anything it did not create in
    // this run.  Codex points TEMP/TMP at a task-local directory.
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void PrepareCache(const std::filesystem::path& directory) {
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", nullptr);
    const std::wstring path = directory.wstring();
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", path.c_str());
    ClearMemoryCache();
}

void ResetCacheEnvironment() {
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", nullptr);
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", nullptr);
    ClearMemoryCache();
}

void TestCacheDirectoryPaths() {
    TempDir directory("directory");
    PrepareCache(directory.path);
    Check(CacheDirectory() == directory.path.wstring(), "explicit cache directory wins");
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", nullptr);
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    Check(count != 0 && count < 32768, "executable path is available");
    const auto expected = std::filesystem::path(executable).parent_path() / L"Cache" / L"MME";
    Check(std::filesystem::path(CacheDirectory()) == expected, "default cache belongs to the executable");
    const auto before = std::filesystem::current_path();
    std::filesystem::current_path(directory.path);
    const auto changed = CacheDirectory();
    std::filesystem::current_path(before);
    Check(std::filesystem::path(changed) == expected, "effect cwd changes do not redirect the cache");
    ResetCacheEnvironment();
}

std::vector<unsigned char> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

bool WriteBytes(const std::filesystem::path& path,
                const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    return WriteBytes(path,
                      std::vector<unsigned char>(text.begin(), text.end()));
}

std::vector<std::filesystem::path> CacheFiles(
    const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == L".fxb")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::uint64_t CacheByteCount(const std::filesystem::path& directory) {
    std::uint64_t total = 0;
    for (const auto& file : CacheFiles(directory)) {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(file, error);
        if (!error) total += static_cast<std::uint64_t>(size);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Fake D3DX compiler/buffer seam
// ---------------------------------------------------------------------------
int g_fakeBuffersCreated = 0;
int g_fakeBuffersReleased = 0;
int g_fakeCompilersCreated = 0;
int g_fakeCompilersReleased = 0;
int g_fakeEffectsCreated = 0;
int g_fakeEffectsReleased = 0;

struct FakeBuffer {
    void** vtable = nullptr;
    std::vector<unsigned char> bytes;
};

struct FakeCompiler {
    void** vtable = nullptr;
};

struct FakeEffect {
    void** vtable = nullptr;
    int marker = 0;
};

// Every fake object is a real COM-shaped object: its vtable slot 2 is the
// release used by the production RAII path, so objects acquired and released by
// the cache (and effects released during a failure) go through the same code.
void** BufferVtable();
void** CompilerVtable();
void** EffectVtable();

struct TestMacro {
    const char* name;
    const char* definition;
};

struct FakeState {
    // Preprocessor.  The fake emits D3DX-style #line directives for the main
    // source file, exactly like D3DXPreprocessShaderFromFileW, and can append
    // further directives to exercise the include-equivalence guard.
    int preprocessCalls = 0;
    std::filesystem::path sourcePath;
    std::string preprocessedSuffix;
    bool suppressLineDirectives = false;
    bool preprocessFail = false;
    bool preprocessDiagnostics = false;
    bool preprocessHuge = false;
    // Compiler.
    int createCompilerCalls = 0;
    bool compilerFail = false;
    bool compilerDiagnostics = false;
    bool compileFail = false;
    bool compileDiagnostics = false;
    std::vector<unsigned char> binary;
    std::string lastCompilerSource;
    DWORD lastCompilerFlags = 0;
    const void* lastCompilerMacros = nullptr;
    const void* lastCompilerInclude = nullptr;
    // Bytecode compile.
    int compileCalls = 0;
    DWORD lastCompileFlags = 0;
    // Effect creation.
    int createEffectCalls = 0;
    IDirect3DDevice9* lastDevice = nullptr;
    void* lastPool = nullptr;
    DWORD lastEffectFlags = 0;
    const void* lastEffectMacros = nullptr;
    const void* lastEffectInclude = nullptr;
    std::vector<unsigned char> lastEffectBinary;
    std::vector<FakeEffect*> effects;
    bool createEffectFail = false;
};

FakeState* g_fake = nullptr;

std::vector<unsigned char> DefaultBinary() {
    return std::vector<unsigned char>{'M', 'M', 'S', 'F', 'X', 0, 0, 1};
}

FakeBuffer* MakeBuffer(const void* data, std::size_t size) {
    ++g_fakeBuffersCreated;
    auto* buffer = new FakeBuffer();
    buffer->vtable = BufferVtable();
    buffer->bytes.assign(static_cast<const unsigned char*>(data),
                         static_cast<const unsigned char*>(data) + size);
    return buffer;
}

FakeBuffer* MakeBuffer(const char* text) {
    return MakeBuffer(text, std::strlen(text));
}

void WINAPI FakeUnused() {}

HRESULT WINAPI FakeQueryInterface(void*, const void*, void**) {
    return E_NOTIMPL;
}

ULONG WINAPI FakeAddRef(void*) { return 1; }

ULONG WINAPI FakeBufferRelease(void* object) {
    ++g_fakeBuffersReleased;
    delete static_cast<FakeBuffer*>(object);
    return 0;
}

ULONG WINAPI FakeCompilerRelease(void* object) {
    ++g_fakeCompilersReleased;
    delete static_cast<FakeCompiler*>(object);
    return 0;
}

void* WINAPI FakeBufferPointer(void* object) {
    auto* buffer = static_cast<FakeBuffer*>(object);
    return buffer && !buffer->bytes.empty() ? buffer->bytes.data() : nullptr;
}

DWORD WINAPI FakeBufferSize(void* object) {
    auto* buffer = static_cast<FakeBuffer*>(object);
    return buffer ? static_cast<DWORD>(buffer->bytes.size()) : 0;
}

ULONG WINAPI FakeReleaseDispatch(void* object) {
    void** table = *static_cast<void***>(object);
    using Function = ULONG(WINAPI*)(void*);
    return reinterpret_cast<Function>(table[2])(object);
}

void** BufferVtable() {
    static void* table[5] = {};
    static bool initialized = false;
    if (!initialized) {
        table[0] = reinterpret_cast<void*>(&FakeQueryInterface);
        table[1] = reinterpret_cast<void*>(&FakeAddRef);
        table[2] = reinterpret_cast<void*>(&FakeBufferRelease);
        table[3] = reinterpret_cast<void*>(&FakeBufferPointer);
        table[4] = reinterpret_cast<void*>(&FakeBufferSize);
        initialized = true;
    }
    return table;
}

void** CompilerVtable() {
    static void* table[60] = {};
    static bool initialized = false;
    if (!initialized) {
        for (auto& slot : table) slot = reinterpret_cast<void*>(&FakeUnused);
        table[2] = reinterpret_cast<void*>(&FakeCompilerRelease);
        initialized = true;
    }
    return table;
}

ULONG WINAPI FakeEffectRelease(void* object) {
    auto* effect = static_cast<FakeEffect*>(object);
    // A successful effect belongs to the caller, but a failure path in the
    // cache may release it before the test cleanup runs.  Removing it from the
    // live list here keeps ReleaseFakeEffects from deleting it twice.
    if (g_fake) {
        auto& effects = g_fake->effects;
        effects.erase(std::remove(effects.begin(), effects.end(), effect),
                      effects.end());
    }
    ++g_fakeEffectsReleased;
    delete effect;
    return 0;
}

void** EffectVtable() {
    static void* table[3] = {};
    static bool initialized = false;
    if (!initialized) {
        table[0] = reinterpret_cast<void*>(&FakeQueryInterface);
        table[1] = reinterpret_cast<void*>(&FakeAddRef);
        table[2] = reinterpret_cast<void*>(&FakeEffectRelease);
        initialized = true;
    }
    return table;
}

HRESULT WINAPI FakePreprocess(LPCWSTR filename, const void* macros,
                              const void* include, void** text, void** errors) {
    FakeState& state = *g_fake;
    ++state.preprocessCalls;
    (void)filename;
    (void)macros;
    (void)include;
    if (text) *text = nullptr;
    if (errors) *errors = nullptr;
    if (state.preprocessFail) {
        if (errors) *errors = MakeBuffer("preprocess failure");
        return E_FAIL;
    }
    std::string body;
    if (state.preprocessHuge) {
        body.assign(kMaxPreprocessedBytes + 1, 'x');
    } else {
        const std::vector<unsigned char> bytes = ReadWholeFile(state.sourcePath);
        body.assign(bytes.begin(), bytes.end());
        if (!state.suppressLineDirectives) {
            // Same shape as D3DXPreprocessShaderFromFileW: one #line directive
            // per expanded file, with the file name as it was passed/expanded.
            // D3DX echoes the caller's file name, which is what the production
            // include guard compares against; the body still comes from
            // state.sourcePath so a test can exercise a relative spelling.
            const std::wstring lineName =
                (filename && *filename) ? std::wstring(filename)
                                        : state.sourcePath.wstring();
            body = "#line 1 \"" + Narrow(lineName) + "\"\n" + body;
        }
        body += state.preprocessedSuffix;
    }
    if (text) *text = MakeBuffer(body.data(), body.size());
    if (state.preprocessDiagnostics && errors)
        *errors = MakeBuffer("preprocess warning");
    return S_OK;
}

HRESULT WINAPI FakeCreateCompiler(const char* source, UINT length,
                                  const void* macros, const void* include,
                                  DWORD flags, void** compiler, void** errors) {
    FakeState& state = *g_fake;
    ++state.createCompilerCalls;
    state.lastCompilerSource.assign(source ? source : "",
                                    source ? length : 0);
    state.lastCompilerFlags = flags;
    state.lastCompilerMacros = macros;
    state.lastCompilerInclude = include;
    if (compiler) *compiler = nullptr;
    if (errors) *errors = nullptr;
    if (state.compilerFail) {
        if (errors) *errors = MakeBuffer("compiler failure");
        return E_FAIL;
    }
    ++g_fakeCompilersCreated;
    auto* created = new FakeCompiler();
    created->vtable = CompilerVtable();
    if (compiler) *compiler = created;
    if (state.compilerDiagnostics && errors)
        *errors = MakeBuffer("parse warning");
    return S_OK;
}

HRESULT WINAPI FakeCompileEffect(void* compiler, DWORD flags, void** binary,
                                 void** errors) {
    FakeState& state = *g_fake;
    (void)compiler;
    ++state.compileCalls;
    state.lastCompileFlags = flags;
    if (binary) *binary = nullptr;
    if (errors) *errors = nullptr;
    const std::vector<unsigned char>& bytes =
        state.binary.empty() ? DefaultBinary() : state.binary;
    if (binary) *binary = MakeBuffer(bytes.data(), bytes.size());
    if (state.compileDiagnostics && errors)
        *errors = MakeBuffer("compile warning");
    return state.compileFail ? E_FAIL : S_OK;
}

HRESULT WINAPI FakeCreateEffect(IDirect3DDevice9* device, LPCVOID data,
                                UINT length, const void* macros,
                                const void* include, DWORD flags, void* pool,
                                void** effect, void** errors) {
    FakeState& state = *g_fake;
    ++state.createEffectCalls;
    state.lastDevice = device;
    state.lastPool = pool;
    state.lastEffectFlags = flags;
    state.lastEffectMacros = macros;
    state.lastEffectInclude = include;
    state.lastEffectBinary.assign(static_cast<const unsigned char*>(data),
                                  static_cast<const unsigned char*>(data) +
                                      length);
    if (effect) *effect = nullptr;
    if (errors) *errors = nullptr;
    if (state.createEffectFail) return E_FAIL;
    ++g_fakeEffectsCreated;
    auto* object = new FakeEffect();
    object->vtable = EffectVtable();
    object->marker = static_cast<int>(state.effects.size()) + 1;
    state.effects.push_back(object);
    if (effect) *effect = object;
    return S_OK;
}

D3DX MakeHooks(FakeState& state) {
    g_fake = &state;
    D3DX hooks;
    hooks.module = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000));
    hooks.preprocessFromFile = &FakePreprocess;
    hooks.createCompiler = &FakeCreateCompiler;
    hooks.createEffect = &FakeCreateEffect;
    hooks.compileEffect = &FakeCompileEffect;
    hooks.bufferPointer = &FakeBufferPointer;
    hooks.bufferSize = &FakeBufferSize;
    hooks.release = &FakeReleaseDispatch;
    hooks.identity.fill(0x42);
    hooks.identity[0] = 0x11;
    hooks.identityValid = true;
    return hooks;
}

void ReleaseFakeEffects(FakeState& state) {
    // Release through the COM vtable like the real caller.  FakeEffectRelease
    // removes the object from state.effects, so snapshot the live list first:
    // an effect already released on a production error path is never deleted
    // twice here.
    const std::vector<FakeEffect*> effects = state.effects;
    for (FakeEffect* effect : effects) FakeReleaseDispatch(effect);
    state.effects.clear();
}

IDirect3DDevice9* TestDevice() {
    return reinterpret_cast<IDirect3DDevice9*>(
        static_cast<std::uintptr_t>(0x1000));
}

void* TestPool() {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2000));
}

struct CreateOutcome {
    bool handled = false;
    HRESULT result = E_FAIL;
    void* effect = nullptr;
    void* errors = nullptr;
};

CreateOutcome CreateWith(const D3DX& hooks, IDirect3DDevice9* device,
                         const wchar_t* filename, const void* macros,
                         const void* include, DWORD flags, void* pool) {
    CreateOutcome outcome;
    outcome.handled = TryCreateEffectWithHooks(
        hooks, device, filename, macros, include, flags, pool, &outcome.effect,
        &outcome.errors, outcome.result);
    if (outcome.errors) {
        hooks.release(outcome.errors);
        outcome.errors = nullptr;
    }
    return outcome;
}

// ---------------------------------------------------------------------------
// Key stability and invalidation
// ---------------------------------------------------------------------------
void TestNamedCacheLayout() {
    TempDir directory("named-layout");
    const auto cache = directory.path / L"cache";
    PrepareCache(cache);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto sourceA = directory.path / L"source-a" / L"ray.fx";
    const auto sourceB = directory.path / L"source-b" / L"ray.fx";
    std::filesystem::create_directories(sourceA.parent_path());
    std::filesystem::create_directories(sourceB.parent_path());
    const std::string text = "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n";
    Check(WriteText(sourceA, text) && WriteText(sourceB, text),
          "same-named source fixtures are created");
    auto create = [&](const std::filesystem::path& source) {
        state.sourcePath = source;
        return CreateWith(hooks, TestDevice(), source.c_str(), nullptr, nullptr, 0, nullptr);
    };
    Check(create(sourceA).handled, "named effect compiles");
    const auto first = CacheFiles(cache);
    Check(first.size() == 1 && first.front().parent_path() == cache / L"ray",
          "ray.fx stores its cache inside the readable ray directory");
    Check(create(sourceB).handled && state.compileCalls == 2,
          "same effect name in another source directory has a separate key");
    const auto both = CacheFiles(cache);
    Check(both.size() == 2 && both[0].parent_path() == cache / L"ray" &&
          both[1].parent_path() == cache / L"ray" &&
          both[0].filename() != both[1].filename(),
          "same-named effects share the readable folder without overwriting variants");
    ClearMemoryCache();
    Check(create(sourceA).handled && create(sourceB).handled && state.compileCalls == 2,
          "both same-named effects hit their own disk entry");
    const auto spaced = directory.path / L"evening .fx";
    Check(WriteText(spaced, text) && create(spaced).handled,
          "an effect whose stem ends with a space loads");
    Check(std::filesystem::is_directory(cache / L"evening") && CacheFiles(cache).size() == 3,
          "a trailing space cannot create an unusable Windows directory");
    ReleaseFakeEffects(state);
}

void TestMemoryOnly() {
    TempDir directory("memory-only");
    const auto cache = directory.path / L"cache";
    PrepareCache(cache);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    const auto alternate = directory.path / L"other.fx";
    const std::string text = "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n";
    Check(WriteText(source, text) && WriteText(alternate, text), "memory-only source fixtures");
    auto create = [&](const std::filesystem::path& path) {
        state.sourcePath = path;
        return CreateWith(hooks, TestDevice(), path.c_str(), nullptr, nullptr, 0, nullptr);
    };
    Check(create(source).handled && state.compileCalls == 1 && CacheFiles(cache).size() == 1,
          "seed a persistent entry before RAM-only mode");
    ClearMemoryCache();
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", L"1");
    Check(CacheDirectory().empty(), "RAM-only mode has no disk cache root");
    const auto first = create(source);
    const auto second = create(source);
    Check(first.handled && second.handled && first.effect != second.effect && state.compileCalls == 2,
          "RAM-only ignores an existing disk entry and then reuses bytecode in memory");
    Check(create(alternate).handled && state.compileCalls == 3 && CacheFiles(cache).size() == 1,
          "RAM-only never publishes a new disk entry");
    ClearMemoryCache();
    Check(create(source).handled && state.compileCalls == 4,
          "discarding RAM requires compilation again in memory-only mode");
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", nullptr);
    ClearMemoryCache();
    Check(create(source).handled && state.compileCalls == 4 && CacheFiles(cache).size() == 1,
          "persistent entry remains usable after leaving RAM-only mode");
    ReleaseFakeEffects(state);
}

void TestKeyInvalidation() {
    TempDir directory("keys");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;

    TestMacro baseMacros[] = {{"TINT_R", "0.25"}, {nullptr, nullptr}};
    TestMacro changedMacros[] = {{"TINT_R", "0.75"}, {nullptr, nullptr}};
    TestMacro ambiguousA[] = {{"A", "BC"}, {nullptr, nullptr}};
    TestMacro ambiguousB[] = {{"AB", "C"}, {nullptr, nullptr}};
    auto* device = TestDevice();
    void* pool = TestPool();

    const auto first = CreateWith(hooks, device, source.c_str(), baseMacros,
                                  nullptr, 0x1000, pool);
    Check(first.handled && first.result == S_OK && first.effect != nullptr,
          "cold call compiles and creates an effect");
    Check(first.errors == nullptr,
          "successful call leaves the errors output null");
    Check(state.compileCalls == 1, "cold call compiles once");
    Check(state.createEffectCalls == 1, "cold call creates one effect");
    Check(state.lastDevice == device && state.lastPool == pool &&
              state.lastEffectFlags == 0x1000,
          "creation receives the caller device, pool and flags");
    Check(state.createCompilerCalls == 1 &&
              state.lastCompilerMacros == nullptr &&
              state.lastCompilerInclude == nullptr,
          "compiler receives preprocessed text without macros or include handler");
    Check(state.lastCompilerSource.find("float4 Tint") != std::string::npos,
          "compiler input is the preprocessed text");
    Check(state.lastCompilerFlags == 0x1000 &&
              state.lastCompileFlags == 0x1000,
          "the caller flags reach both the compiler and CompileEffect");

    const auto warm = CreateWith(hooks, device, source.c_str(), baseMacros,
                                 nullptr, 0x1000, pool);
    Check(warm.handled && warm.effect != nullptr &&
              warm.effect != first.effect,
          "warm call creates a distinct effect object");
    Check(state.compileCalls == 1,
          "memory hit does not call CompileEffect again");
    Check(state.createEffectCalls == 2,
          "every hit still creates its own effect");
    Check(state.lastDevice == device && state.lastPool == pool &&
              state.lastEffectFlags == 0x1000,
          "hit keeps device, pool and flags");

    ClearMemoryCache();
    const auto disk = CreateWith(hooks, device, source.c_str(), baseMacros,
                                 nullptr, 0x1000, pool);
    Check(disk.handled && disk.effect != nullptr,
          "disk hit still creates an effect");
    Check(state.compileCalls == 1,
          "disk hit does not call CompileEffect again");
    Check(CacheFiles(directory.path).size() == 1,
          "cold call stored exactly one disk entry");

    const auto changedFlags = CreateWith(hooks, device, source.c_str(),
                                         baseMacros, nullptr, 0x2000, pool);
    Check(changedFlags.handled && state.compileCalls == 2,
          "flags change invalidates the key");

    const auto changedMacro = CreateWith(hooks, device, source.c_str(),
                                         changedMacros, nullptr, 0x1000, pool);
    Check(changedMacro.handled && state.compileCalls == 3,
          "macro value change invalidates the key");

    const auto macroA = CreateWith(hooks, device, source.c_str(), ambiguousA,
                                   nullptr, 0x1000, pool);
    const auto macroB = CreateWith(hooks, device, source.c_str(), ambiguousB,
                                   nullptr, 0x1000, pool);
    Check(macroA.handled && macroB.handled && state.compileCalls == 5,
          "length-prefixed macro fields keep A=BC and AB=C distinct");

    D3DX changedIdentity = hooks;
    changedIdentity.identity[0] ^= 0xFF;
    const auto identity =
        CreateWith(changedIdentity, device, source.c_str(), baseMacros, nullptr,
                   0x1000, pool);
    Check(identity.handled && state.compileCalls == 6,
          "compiler identity change invalidates the key");

    WriteText(source, "float4 Tint = float4(0.85, 0.5, 0.75, 1.0);\n");
    const auto changedSource = CreateWith(hooks, device, source.c_str(),
                                          baseMacros, nullptr, 0x1000, pool);
    Check(changedSource.handled && state.compileCalls == 7,
          "source text change invalidates the key");

    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    const auto restored = CreateWith(hooks, device, source.c_str(), baseMacros,
                                     nullptr, 0x1000, pool);
    Check(restored.handled && state.compileCalls == 7,
          "restored inputs hit the earlier memory entry");

    const auto copied = directory.path / L"copy.fx";
    std::filesystem::copy_file(
        source, copied, std::filesystem::copy_options::overwrite_existing);
    const auto otherPath = CreateWith(hooks, device, copied.c_str(), baseMacros,
                                      nullptr, 0x1000, pool);
    Check(otherPath.handled && state.compileCalls == 8,
          "source path change invalidates the key");
    Check(otherPath.effect != restored.effect,
          "each call keeps a distinct effect object");
    Check(state.preprocessCalls == 11,
          "every query preprocesses the current source once");

    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Non-ASCII source paths
// ---------------------------------------------------------------------------
void TestNonAsciiSourcePath() {
    TempDir directory("unicode-path");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source =
        directory.path / L"日本_测试_проверка" / L"夕空_光影.fx";
    std::error_code error;
    std::filesystem::create_directories(source.parent_path(), error);
    Check(!error, "non-ASCII source directory is created");
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;
    // Resolve a real relative path so the cache verifies the same file that
    // the preprocessor consumes, including the non-ASCII directory names.
    const std::wstring relative =
        std::filesystem::relative(source, std::filesystem::current_path()).wstring();
    const auto outcome = CreateWith(hooks, TestDevice(), relative.c_str(),
                                    nullptr, nullptr, 0x60, nullptr);
    Check(outcome.handled && outcome.effect != nullptr,
          "non-ASCII relative source path is verified and cached");
    Check(state.compileCalls == 1,
          "non-ASCII relative source path reached the compiler");
    const auto files = CacheFiles(directory.path);
    Check(files.size() == 1 && files.front().parent_path().filename() == L"夕空_光影",
          "Unicode effect names are preserved in the cache folder");
    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Damaged disk entries
// ---------------------------------------------------------------------------
void TestCorruptEntries() {
    TempDir directory("corrupt");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;
    TestMacro macros[] = {{"ENABLE_FOG", "1"}, {nullptr, nullptr}};
    auto* device = TestDevice();

    const auto cold =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0, nullptr);
    Check(cold.handled && state.compileCalls == 1,
          "cold call compiles and stores");
    const auto files = CacheFiles(directory.path);
    Check(files.size() == 1, "cold call stored one entry");
    const auto entry = files.empty() ? std::filesystem::path{} : files.front();

    auto expectMissAndRecompile = [&](const char* label) {
        ClearMemoryCache();
        const int before = state.compileCalls;
        const auto outcome = CreateWith(hooks, device, source.c_str(), macros,
                                        nullptr, 0, nullptr);
        Check(outcome.handled && outcome.effect != nullptr,
              std::string(label) +
                  ": damaged entry still loads through a fresh compile");
        Check(state.compileCalls == before + 1,
              std::string(label) + ": damaged entry is treated as a miss");
    };

    {
        const auto bytes = ReadWholeFile(entry);
        Check(bytes.size() > 40, "stored entry has a full header");
        WriteBytes(entry, std::vector<unsigned char>(bytes.begin(),
                                                     bytes.begin() + 40));
        expectMissAndRecompile("truncated entry");
    }
    {
        auto bytes = ReadWholeFile(entry);
        Check(!bytes.empty(), "entry exists before payload corruption");
        if (!bytes.empty()) bytes.back() ^= 0x5A;
        WriteBytes(entry, bytes);
        expectMissAndRecompile("payload digest mismatch");
    }
    {
        auto bytes = ReadWholeFile(entry);
        if (!bytes.empty()) bytes[0] ^= 0xFF;
        WriteBytes(entry, bytes);
        expectMissAndRecompile("magic mismatch");
    }
    {
        std::vector<unsigned char> bytes(80, 0);
        bytes[0] = 'M';
        bytes[1] = 'M';
        bytes[2] = 'E';
        bytes[3] = 'C';
        bytes[4] = 1;
        bytes[8] = 0xFF;
        bytes[9] = 0xFF;
        bytes[10] = 0xFF;
        bytes[11] = 0x7F;
        bytes[12] = 1;
        WriteBytes(entry, bytes);
        expectMissAndRecompile("oversized declared lengths");
    }
    {
        const std::string garbage = "random bytes that are not a cache entry";
        WriteBytes(entry, std::vector<unsigned char>(garbage.begin(),
                                                     garbage.end()));
        expectMissAndRecompile("foreign file content");
    }

    // A valid entry for another key must be rejected by the key comparison,
    // not accepted because its own digest and payload digest are valid.
    ClearMemoryCache();
    const auto rebuilt =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0, nullptr);
    Check(rebuilt.handled, "entry is rebuilt after the damaged-file cases");
    const auto entryA = CacheFiles(directory.path).front();
    const auto copySource = directory.path / L"copy.fx";
    std::filesystem::copy_file(
        source, copySource, std::filesystem::copy_options::overwrite_existing);
    ClearMemoryCache();
    const auto copyOutcome = CreateWith(hooks, device, copySource.c_str(),
                                        macros, nullptr, 0, nullptr);
    Check(copyOutcome.handled, "second source path compiles");
    const auto filesAfter = CacheFiles(directory.path);
    Check(filesAfter.size() == 2, "two source paths stored two entries");
    std::filesystem::path entryB;
    for (const auto& candidate : filesAfter)
        if (candidate.filename() != entryA.filename()) entryB = candidate;
    Check(!entryB.empty(), "second entry found");
    if (!entryB.empty()) {
        std::filesystem::copy_file(
            entryB, entryA, std::filesystem::copy_options::overwrite_existing);
        ClearMemoryCache();
        const int before = state.compileCalls;
        const auto wrongKey = CreateWith(hooks, device, source.c_str(), macros,
                                         nullptr, 0, nullptr);
        Check(wrongKey.handled && wrongKey.effect != nullptr,
              "entry of another key still loads through a fresh compile");
        Check(state.compileCalls == before + 1,
              "entry of another key is a miss even with valid digests");
    }

    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Bypass and fallback paths
// ---------------------------------------------------------------------------
void TestFallbackAndDiagnostics() {
    TempDir directory("fallback");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;
    TestMacro macros[] = {{"ENABLE_FOG", "1"}, {nullptr, nullptr}};
    auto* device = TestDevice();

    auto expectBypassWithoutPreprocess = [&](const char* label,
                                             const void* macroList,
                                             const void* includeHandler,
                                             DWORD flags,
                                             IDirect3DDevice9* callDevice,
                                             LPCWSTR file) {
        const int preprocessBefore = state.preprocessCalls;
        const auto outcome = CreateWith(hooks, callDevice, file, macroList,
                                        includeHandler, flags, nullptr);
        Check(!outcome.handled, std::string(label) + ": call falls back");
        Check(state.preprocessCalls == preprocessBefore,
              std::string(label) + ": no preprocessing happens");
    };

    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1");
    expectBypassWithoutPreprocess("disabled cache", macros, nullptr, 0, device,
                                  source.c_str());
    Check(state.createCompilerCalls == 0 && state.compileCalls == 0 &&
              state.createEffectCalls == 0,
          "disabled cache performs no compiler work");
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", nullptr);

    expectBypassWithoutPreprocess(
        "custom include handler", macros,
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1234)), 0,
        device, source.c_str());
    expectBypassWithoutPreprocess("missing device", macros, nullptr, 0, nullptr,
                                  source.c_str());
    expectBypassWithoutPreprocess("missing filename", macros, nullptr, 0, device,
                                  nullptr);
    {
        void* effect = nullptr;
        void* errors = nullptr;
        HRESULT result = E_FAIL;
        Check(!TryCreateEffectWithHooks(hooks, device, source.c_str(), macros,
                                        nullptr, 0, nullptr, nullptr, &errors,
                                        result),
              "missing effect output falls back");
    }
    expectBypassWithoutPreprocess(
        "unreadable macros",
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(1)), nullptr,
        0, device, source.c_str());
    {
        TestMacro invalid[] = {
            {reinterpret_cast<const char*>(static_cast<std::uintptr_t>(1)), "1"},
            {nullptr, nullptr}};
        expectBypassWithoutPreprocess("unreadable macro name", invalid, nullptr,
                                      0, device, source.c_str());
    }
    {
        std::vector<TestMacro> unterminated(kMaxMacroCount + 1);
        for (auto& macro : unterminated) {
            macro.name = "A";
            macro.definition = "1";
        }
        expectBypassWithoutPreprocess("macro array without terminator",
                                      unterminated.data(), nullptr, 0, device,
                                      source.c_str());
    }
    {
        const std::string oversized(kMaxMacroDefinitionBytes + 1, 'x');
        TestMacro huge[] = {{"HUGE", oversized.c_str()}, {nullptr, nullptr}};
        expectBypassWithoutPreprocess("oversized macro definition", huge,
                                      nullptr, 0, device, source.c_str());
    }

    auto expectPreprocessFallback = [&](const char* label, bool diagnostics) {
        const int preprocessBefore = state.preprocessCalls;
        const int compileBefore = state.compileCalls;
        state.preprocessDiagnostics = diagnostics;
        const auto outcome = CreateWith(hooks, device, source.c_str(), macros,
                                        nullptr, 0, nullptr);
        Check(!outcome.handled, std::string(label) + ": call falls back");
        Check(state.preprocessCalls == preprocessBefore + 1,
              std::string(label) + ": preprocessing ran once");
        Check(state.compileCalls == compileBefore,
              std::string(label) + ": nothing was compiled");
        state.preprocessDiagnostics = false;
    };

    state.preprocessHuge = true;
    {
        const int preprocessBefore = state.preprocessCalls;
        const auto outcome = CreateWith(hooks, device, source.c_str(), macros,
                                        nullptr, 0, nullptr);
        Check(!outcome.handled, "oversized preprocessed text falls back");
        Check(state.preprocessCalls == preprocessBefore + 1,
              "oversized preprocessed text was preprocessed once");
        Check(state.compileCalls == 0,
              "oversized preprocessing never reaches CompileEffect");
    }
    state.preprocessHuge = false;

    state.preprocessFail = true;
    expectPreprocessFallback("preprocessor failure", false);
    state.preprocessFail = false;

    // Real D3DX output need not retain #line directives. Cache eligibility
    // must not depend on synthetic source-location markers.
    state.suppressLineDirectives = true;
    {
        const auto outcome = CreateWith(hooks, device, source.c_str(), macros,
                                        nullptr, 0x52, nullptr);
        Check(outcome.handled, "self-contained output without #line is cacheable");
        Check(state.compileCalls == 1, "source without #line compiles once");
    }
    state.suppressLineDirectives = false;

    expectPreprocessFallback("preprocessor diagnostics", true);

    const auto normal = CreateWith(hooks, device, source.c_str(), macros,
                                   nullptr, 0x40, nullptr);
    Check(normal.handled && normal.effect != nullptr &&
              normal.errors == nullptr,
          "healthy call compiles and creates an effect");
    Check(state.compileCalls == 2 && state.createEffectCalls == 2,
          "healthy call compiled once");

    state.compilerFail = true;
    auto outcome = CreateWith(hooks, device, source.c_str(), macros, nullptr,
                              0x41, nullptr);
    Check(!outcome.handled,
          "compiler creation failure falls back to the original entry point");
    Check(state.compileCalls == 2, "failed compiler creation never compiles");
    state.compilerFail = false;

    state.compilerDiagnostics = true;
    outcome = CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x42,
                         nullptr);
    Check(!outcome.handled, "parser diagnostics fall back");
    Check(state.compileCalls == 2, "parser diagnostics never compile");
    state.compilerDiagnostics = false;

    state.compileFail = true;
    outcome = CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x43,
                         nullptr);
    Check(!outcome.handled, "CompileEffect failure falls back");
    Check(state.compileCalls == 3, "CompileEffect failure is observable");
    state.compileFail = false;

    state.compileDiagnostics = true;
    outcome = CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x44,
                         nullptr);
    Check(!outcome.handled, "CompileEffect diagnostics fall back");
    Check(state.compileCalls == 4, "CompileEffect diagnostics are observable");
    state.compileDiagnostics = false;

    state.createEffectFail = true;
    outcome = CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x45,
                         nullptr);
    Check(!outcome.handled, "effect creation failure falls back");
    Check(state.compileCalls == 5,
          "failed effect creation reached the compiler path");
    state.createEffectFail = false;
    const auto retried = CreateWith(hooks, device, source.c_str(), macros,
                                    nullptr, 0x45, nullptr);
    Check(retried.handled, "retry after a creation failure succeeds");
    Check(state.compileCalls == 6,
          "failed creation did not store bytecode for the retry");

    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Size limits and an unwritable cache directory
// ---------------------------------------------------------------------------
void TestLimitsAndUnwritableCache() {
    TempDir directory("limits");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;
    TestMacro macros[] = {{"ENABLE_FOG", "1"}, {nullptr, nullptr}};
    auto* device = TestDevice();

    state.binary.assign(kMaxPayloadBytes + 1, 0xAB);
    const auto oversized =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0, nullptr);
    Check(!oversized.handled,
          "bytecode above the payload limit falls back to the original call");
    Check(state.compileCalls == 1, "oversized bytecode was compiled once");
    Check(CacheFiles(directory.path).empty(),
          "oversized bytecode is not stored");
    state.binary.clear();

    const auto blocker = directory.path / L"blocker";
    WriteText(blocker, "not a directory");
    const auto unwritable = blocker / L"cache";
    const std::wstring unwritablePath = unwritable.wstring();
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", unwritablePath.c_str());
    ClearMemoryCache();
    const auto first =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0, nullptr);
    Check(first.handled && first.effect != nullptr,
          "unwritable cache directory still loads through the memory path");
    Check(state.compileCalls == 2,
          "unwritable cache directory compiles instead of failing");
    ClearMemoryCache();
    const auto second =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0, nullptr);
    Check(second.handled && second.effect != nullptr,
          "unwritable cache directory recompiles instead of failing");
    Check(state.compileCalls == 3,
          "unwritable cache directory has no disk entry to hit");
    Check(!std::filesystem::exists(unwritable),
          "no cache directory is created below a regular file");

    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Disk capacity accounting
// ---------------------------------------------------------------------------
void TestDiskBudget() {
    TempDir directory("budget");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;
    TestMacro macros[] = {{"ENABLE_FOG", "1"}, {nullptr, nullptr}};
    auto* device = TestDevice();

    // A single write larger than the whole byte budget must be skipped.  Only
    // counting the bytes that already exist would let the first large entry
    // through and make the advertised bound false.
    SetDiskLimitsForTesting(1024, 100);
    const auto overBudget =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0, nullptr);
    Check(overBudget.handled && overBudget.effect != nullptr,
          "a write over the byte budget still loads");
    Check(CacheFiles(directory.path).empty(),
          "the incoming entry size counts against the byte budget");

    // Entry-count bound: the third distinct entry is skipped, while existing
    // entries stay readable from the full cache.
    SetDiskLimitsForTesting(2, 1024 * 1024);
    const auto first =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x10, nullptr);
    const auto second =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x11, nullptr);
    const auto third =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x12, nullptr);
    Check(first.handled && second.handled && third.handled,
          "cache-full runs still load through the original path");
    Check(CacheFiles(directory.path).size() == 2,
          "the entry-count budget bounds the directory");
    const auto otherSource = directory.path / L"another-effect.fx";
    Check(WriteText(otherSource, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n"),
          "another effect fixture is written");
    state.sourcePath = otherSource;
    const auto other = CreateWith(hooks, device, otherSource.c_str(), macros,
                                  nullptr, 0x10, nullptr);
    Check(other.handled && CacheFiles(directory.path).size() == 2,
          "another effect folder shares the same global entry-count budget");
    state.sourcePath = source;
    const int compileBeforeHit = state.compileCalls;
    ClearMemoryCache();
    const auto hit =
        CreateWith(hooks, device, source.c_str(), macros, nullptr, 0x10, nullptr);
    Check(hit.handled && state.compileCalls == compileBeforeHit,
          "an existing entry is usable while the cache is full");

    // Replacing a key at the exact byte budget must be allowed: the old file is
    // replaced, not added to the total.
    const std::uint64_t totalBytes = CacheByteCount(directory.path);
    const auto target = CacheFiles(directory.path).front();
    const auto original = ReadWholeFile(target);
    Check(original.size() > 40, "budget fixture entry has a full header");
    WriteBytes(target, std::vector<unsigned char>(original.begin(),
                                                  original.begin() + 40));
    SetDiskLimitsForTesting(2, totalBytes);
    ClearMemoryCache();
    bool repaired = false;
    for (const DWORD flags : {0x10u, 0x11u}) {
        const int before = state.compileCalls;
        const auto outcome = CreateWith(hooks, device, source.c_str(), macros,
                                        nullptr, flags, nullptr);
        if (!outcome.handled) continue;
        if (state.compileCalls == before + 1) {
            repaired = true;
            break;
        }
    }
    Check(repaired, "a same-key rewrite is allowed at the exact byte budget");
    Check(CacheByteCount(directory.path) == totalBytes,
          "the rewritten entry keeps the directory inside the byte budget");

    SetDiskLimitsForTesting(0, 0);
    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Injected allocation/storage failures
// ---------------------------------------------------------------------------
void TestExceptionSafety() {
    TempDir directory("exceptions");
    PrepareCache(directory.path);
    FakeState state;
    const D3DX hooks = MakeHooks(state);
    const auto source = directory.path / L"main.fx";
    WriteText(source, "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n");
    state.sourcePath = source;
    TestMacro macros[] = {{"ENABLE_FOG", "1"}, {nullptr, nullptr}};
    auto* device = TestDevice();

    const int outstandingBuffers =
        g_fakeBuffersCreated - g_fakeBuffersReleased;
    const int outstandingCompilers =
        g_fakeCompilersCreated - g_fakeCompilersReleased;

    void* const sentinelEffect =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xABCD));
    void* const sentinelErrors =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xEF01));
    const HRESULT sentinelResult =
        static_cast<HRESULT>(static_cast<std::int32_t>(0x80001234u));

    auto expectFallbackWithoutOutputChange = [&](FaultPoint point,
                                                 const char* label,
                                                 DWORD flags) {
        void* effect = sentinelEffect;
        void* errors = sentinelErrors;
        HRESULT result = sentinelResult;
        SetFaultPointForTesting(point);
        const bool handled = TryCreateEffectWithHooks(
            hooks, device, source.c_str(), macros, nullptr, flags, nullptr,
            &effect, &errors, result);
        SetFaultPointForTesting(FaultPoint::kNone);
        Check(!handled, std::string(label) + ": injected exception falls back");
        Check(effect == sentinelEffect,
              std::string(label) + ": effect output is untouched");
        Check(errors == sentinelErrors,
              std::string(label) + ": errors output is untouched");
        Check(result == sentinelResult,
              std::string(label) + ": result output is untouched");
    };

    expectFallbackWithoutOutputChange(FaultPoint::kBeforePreprocessedCopy,
                                      "preprocessed-copy exception", 0x80);
    Check(g_fakeBuffersCreated - g_fakeBuffersReleased == outstandingBuffers,
          "preprocessed-copy exception releases the candidate buffer");
    Check(state.compileCalls == 0,
          "preprocessed-copy exception never reaches CompileEffect");

    expectFallbackWithoutOutputChange(FaultPoint::kBeforeBytecodeCopy,
                                      "bytecode-copy exception", 0x81);
    Check(g_fakeBuffersCreated - g_fakeBuffersReleased == outstandingBuffers,
          "bytecode-copy exception releases the candidate buffers");
    Check(g_fakeCompilersCreated - g_fakeCompilersReleased ==
              outstandingCompilers,
          "bytecode-copy exception releases the candidate compiler");
    Check(state.compileCalls == 1,
          "bytecode-copy exception happened after one compile");

    // A publication failure happens after the effect object exists: the load
    // must still succeed and deliver exactly that object.
    void* effect = sentinelEffect;
    void* errors = sentinelErrors;
    HRESULT result = sentinelResult;
    SetFaultPointForTesting(FaultPoint::kBeforeCachePublish);
    const bool handled = TryCreateEffectWithHooks(
        hooks, device, source.c_str(), macros, nullptr, 0x82, nullptr, &effect,
        &errors, result);
    SetFaultPointForTesting(FaultPoint::kNone);
    Check(handled, "publication exception does not fail the load");
    Check(effect != sentinelEffect && effect != nullptr,
          "the created effect is delivered despite the publication exception");
    Check(errors == nullptr,
          "successful call sets the errors output to null");
    Check(result == S_OK, "the creation HRESULT is returned");
    Check(state.compileCalls == 2, "publication exception run compiled once");
    Check(CacheFiles(directory.path).empty(),
          "publication exception stored nothing");
    Check(g_fakeBuffersCreated - g_fakeBuffersReleased == outstandingBuffers,
          "publication exception leaves no candidate COM objects behind");

    ClearMemoryCache();
    void* retryEffect = nullptr;
    void* retryErrors = nullptr;
    HRESULT retryResult = E_FAIL;
    const bool retried = TryCreateEffectWithHooks(
        hooks, device, source.c_str(), macros, nullptr, 0x82, nullptr,
        &retryEffect, &retryErrors, retryResult);
    Check(retried && retryEffect != nullptr,
          "the next call after a publication exception succeeds");
    Check(state.compileCalls == 3,
          "the failed publication had stored no memory entry");
    Check(CacheFiles(directory.path).size() == 1,
          "the retry stored the disk entry");

    void* hitEffect = nullptr;
    void* hitErrors = nullptr;
    HRESULT hitResult = E_FAIL;
    const bool hit = TryCreateEffectWithHooks(
        hooks, device, source.c_str(), macros, nullptr, 0x82, nullptr,
        &hitEffect, &hitErrors, hitResult);
    Check(hit && state.compileCalls == 3,
          "the entry stored by the retry is reused");

    ReleaseFakeEffects(state);
}

// ---------------------------------------------------------------------------
// Real D3DX integration entries (explicit; need the local DirectX runtime)
// ---------------------------------------------------------------------------
FnCompileEffect g_realCompile = nullptr;
int g_integrationCompileCalls = 0;

HRESULT WINAPI CountingCompile(void* compiler, DWORD flags, void** effect,
                               void** errors) {
    ++g_integrationCompileCalls;
    return g_realCompile ? g_realCompile(compiler, flags, effect, errors)
                         : E_FAIL;
}

int CheckIntegration(bool condition, const char* description) {
    if (condition) {
        std::printf("[ OK ] %s\n", description);
        return 0;
    }
    std::printf("[FAIL] %s\n", description);
    return 1;
}

// Full D3DX diagnostics text of one returned buffer (no truncation, first NUL
// terminates the text).  The buffer is not released here.
std::string BufferText(const D3DX& hooks, void* buffer) {
    if (!buffer || !hooks.bufferPointer || !hooks.bufferSize) return {};
    const void* pointer = hooks.bufferPointer(buffer);
    const DWORD size = hooks.bufferSize(buffer);
    if (!pointer || !size) return {};
    std::string text(static_cast<const char*>(pointer), size);
    const std::size_t terminator = text.find('\0');
    if (terminator != std::string::npos) text.resize(terminator);
    return text;
}

bool WidenArgument(const char* argument, std::wstring& wide) {
    if (!argument) return false;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument,
                                     -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, argument, -1, nullptr, 0);
    }
    if (length <= 0) return false;
    wide.resize(static_cast<std::size_t>(length));
    if (MultiByteToWideChar(codePage, flags, argument, -1, wide.data(),
                            length) != length)
        return false;
    wide.pop_back();
    return true;
}

std::wstring FindD3dxDll() {
    wchar_t configured[32768]{};
    const DWORD configuredLength =
        GetEnvironmentVariableW(L"MMS_EFFECT_CACHE_TEST_D3DX", configured,
                                _countof(configured));
    if (configuredLength > 0 && configuredLength < _countof(configured))
        return std::wstring(configured, configuredLength);
    wchar_t systemDirectory[32768]{};
    const UINT systemLength =
        GetSystemDirectoryW(systemDirectory, _countof(systemDirectory));
    if (!systemLength || systemLength >= _countof(systemDirectory)) return {};
    for (const wchar_t* name : {L"\\d3dx9_43.dll", L"\\d3dx9_32.dll"}) {
        const std::wstring candidate =
            std::wstring(systemDirectory, systemLength) + name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

struct D3DXContext {
    HMODULE module = nullptr;
    void* createFromFile = nullptr;
    D3DX hooks{};
    std::wstring path;
};

bool LoadD3DXContext(D3DXContext& context) {
    context.path = FindD3dxDll();
    if (context.path.empty()) return false;
    context.module = LoadLibraryW(context.path.c_str());
    if (!context.module) return false;
    context.createFromFile = reinterpret_cast<void*>(
        GetProcAddress(context.module, "D3DXCreateEffectFromFileW"));
    if (!context.createFromFile ||
        !ResolveD3DX(context.createFromFile, context.hooks)) {
        FreeLibrary(context.module);
        context.module = nullptr;
        return false;
    }
    return true;
}

void UnloadD3DXContext(D3DXContext& context) {
    if (context.module) FreeLibrary(context.module);
    context.module = nullptr;
}

struct TestDeviceContext {
    IDirect3D9* d3d = nullptr;
    HWND window = nullptr;
    IDirect3DDevice9* device = nullptr;
    const wchar_t* typeName = L"NULLREF";
};

bool CreateTestDevice(TestDeviceContext& context) {
    wchar_t name[16]{};
    const DWORD nameLength = GetEnvironmentVariableW(
        L"MMS_EFFECT_CACHE_TEST_DEVICE", name, _countof(name));
    const bool hardware =
        nameLength == 3 && _wcsnicmp(name, L"hal", 3) == 0;
    context.typeName = hardware ? L"HAL" : L"NULLREF";
    context.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!context.d3d) return false;
    context.window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0,
                                     0, 64, 64, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.BackBufferWidth = 64;
    parameters.BackBufferHeight = 64;
    parameters.hDeviceWindow = context.window;
    const HRESULT result = context.d3d->CreateDevice(
        D3DADAPTER_DEFAULT,
        hardware ? D3DDEVTYPE_HAL : D3DDEVTYPE_NULLREF, context.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &context.device);
    if (FAILED(result) || !context.device) {
        context.device = nullptr;
        return false;
    }
    return true;
}

void DestroyTestDevice(TestDeviceContext& context) {
    if (context.device) {
        context.device->Release();
        context.device = nullptr;
    }
    if (context.window) {
        DestroyWindow(context.window);
        context.window = nullptr;
    }
    if (context.d3d) {
        context.d3d->Release();
        context.d3d = nullptr;
    }
}

// ID3DXEffect inherits ID3DXBaseEffect: GetParameterByName is slot 9 and
// GetValue is slot 21 (same vtable on x86/x64).
bool ReadEffectTint(void* effect, float out[4]) {
    if (!effect) return false;
    void** table = *static_cast<void***>(effect);
    using FnGetParameterByName = void* (WINAPI*)(void*, void*, const char*);
    using FnGetValue = HRESULT(WINAPI*)(void*, void*, void*, UINT);
    auto* getParameterByName =
        reinterpret_cast<FnGetParameterByName>(table[9]);
    auto* getValue = reinterpret_cast<FnGetValue>(table[21]);
    if (!getParameterByName || !getValue) return false;
    void* handle = getParameterByName(effect, nullptr, "Tint");
    if (!handle) return false;
    return SUCCEEDED(getValue(effect, handle, out, 16));
}

bool SameTint(const float left[4], const float right[4]) {
    for (int index = 0; index < 4; ++index) {
        const float difference = left[index] - right[index];
        if (difference > 1e-6f || difference < -1e-6f) return false;
    }
    return true;
}

std::string TintText(const float tint[4]) {
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g, %g)", tint[0], tint[1],
                  tint[2], tint[3]);
    return buffer;
}

void WriteTintInclude(const std::filesystem::path& path,
                      const char* tintLiteral) {
    WriteText(path, std::string("float4 Tint = float4(") + tintLiteral +
                        ", 0.5, 0.75, 1.0);\n");
}

std::string IncludeLine(const std::filesystem::path& includePath) {
    return "#include \"" + Narrow(includePath.wstring()) + "\"";
}

void WriteCacheableFx(const std::filesystem::path& path) {
    WriteText(path,
              "#ifndef TINT_R\n"
              "#define TINT_R 0.25\n"
              "#endif\n"
              "float4 Tint = float4(TINT_R, 0.5, 0.75, 1.0);\n"
              "float4x4 WorldViewProj;\n"
              "float4 MainPS(float2 uv : TEXCOORD0) : COLOR "
              "{ return Tint; }\n"
              "technique Technique0 { pass P0 { PixelShader = compile "
              "ps_2_0 MainPS(); } }\n");
}

void WriteIncludedMain(const std::filesystem::path& path,
                       const std::string& includeLine) {
    WriteText(path,
              includeLine + "\n" +
                  "float4x4 WorldViewProj;\n"
                  "float4 MainPS(float2 uv : TEXCOORD0) : COLOR "
                  "{ return Tint; }\n"
                  "technique Technique0 { pass P0 { PixelShader = compile "
                  "ps_2_0 MainPS(); } }\n");
}

// One attempt through each path.  wrapperHandled is the cache module's own
// answer; when it is false the harness invokes the original entry point exactly
// like the production wrapper does.
struct ScenarioOutcome {
    HRESULT originalResult = E_FAIL;
    bool originalCreated = false;
    float originalTint[4]{};
    bool originalHasTint = false;
    std::string originalDiagnostics;
    std::string wrapperDiagnostics;
    bool wrapperHandled = false;
    HRESULT wrapperResult = E_FAIL;
    bool wrapperCreated = false;
    float wrapperTint[4]{};
    bool wrapperHasTint = false;
};

using CreateEffectFromFileW = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR,
                                              const void*, const void*, DWORD,
                                              void*, void**, void**);

ScenarioOutcome RunScenario(const D3DX& hooks, IDirect3DDevice9* device,
                            void* createFromFile, const wchar_t* file,
                            const void* macros, DWORD flags = 0) {
    const auto original =
        reinterpret_cast<CreateEffectFromFileW>(createFromFile);
    ScenarioOutcome outcome;
    {
        void* effectObject = nullptr;
        void* errors = nullptr;
        outcome.originalResult = original(device, file, macros, nullptr, flags,
                                          nullptr, &effectObject, &errors);
        outcome.originalDiagnostics = BufferText(hooks, errors);
        if (errors) hooks.release(errors);
        if (effectObject) {
            outcome.originalCreated = true;
            outcome.originalHasTint =
                ReadEffectTint(effectObject, outcome.originalTint);
            hooks.release(effectObject);
        }
    }
    {
        void* effectObject = nullptr;
        void* errors = nullptr;
        HRESULT result = E_FAIL;
        outcome.wrapperHandled = TryCreateEffectWithHooks(
            hooks, device, file, macros, nullptr, flags, nullptr, &effectObject,
            &errors, result);
        if (!outcome.wrapperHandled) {
            if (errors) hooks.release(errors);
            effectObject = nullptr;
            errors = nullptr;
            result = original(device, file, macros, nullptr, flags, nullptr,
                              &effectObject, &errors);
        }
        outcome.wrapperResult = result;
        outcome.wrapperDiagnostics = BufferText(hooks, errors);
        if (errors) hooks.release(errors);
        if (effectObject) {
            outcome.wrapperCreated = true;
            outcome.wrapperHasTint =
                ReadEffectTint(effectObject, outcome.wrapperTint);
            hooks.release(effectObject);
        }
    }
    return outcome;
}

int CheckScenario(int& failures, const ScenarioOutcome& outcome,
                  bool expectHandled, const char* label) {
    const int before = failures;
    auto check = [&](bool condition, const char* what) {
        if (condition) {
            std::printf("[ OK ] %s: %s\n", label, what);
            return;
        }
        std::printf("[FAIL] %s: %s\n", label, what);
        ++failures;
    };
    check(outcome.wrapperHandled == expectHandled,
          "cache path handled state follows the include-equivalence rule");
    check(SUCCEEDED(outcome.wrapperResult) == SUCCEEDED(outcome.originalResult),
          "both paths agree on success/failure");
    check(outcome.wrapperCreated == outcome.originalCreated,
          "both paths agree on effect creation");
    check(outcome.wrapperDiagnostics == outcome.originalDiagnostics,
          "both paths return identical diagnostic text");
    if (outcome.wrapperCreated && outcome.originalCreated) {
        check(outcome.wrapperHasTint && outcome.originalHasTint,
              "both effects expose the Tint parameter");
        if (outcome.wrapperHasTint && outcome.originalHasTint)
            check(SameTint(outcome.wrapperTint, outcome.originalTint),
                  "both paths produce the same Tint parameter");
    }
    std::printf(
        "  %s: cache handled=%d hr=0x%08lX tint=%s | original hr=0x%08lX "
        "tint=%s\n",
        label, outcome.wrapperHandled ? 1 : 0,
        static_cast<unsigned long>(outcome.wrapperResult),
        outcome.wrapperHasTint ? TintText(outcome.wrapperTint).c_str() : "n/a",
        static_cast<unsigned long>(outcome.originalResult),
        outcome.originalHasTint ? TintText(outcome.originalTint).c_str() : "n/a");
    if (!outcome.originalDiagnostics.empty())
        std::printf("  %s original diagnostics:\n%s\n", label,
                    outcome.originalDiagnostics.c_str());
    return failures - before;
}

// ---------------------------------------------------------------------------
// Include-resolution comparison child process
// ---------------------------------------------------------------------------
// D3DX32 uses cwd; D3DX43 tries the main source directory before cwd.
// The parent therefore starts this mode as a child process with its working
// directory set to the fixture cwd folder (CreateProcessW lpCurrentDirectory),
// so this process's own working directory is never changed while two paths are
// compared.  Every fixture and cache entry stays below the given root.
int RunIncludeProbe(const std::filesystem::path& root) {
    int failures = 0;
    std::error_code error;
    const std::filesystem::path absoluteRoot =
        std::filesystem::absolute(root, error);
    const std::filesystem::path rootPath = error ? root : absoluteRoot;
    const std::filesystem::path sourceDirectory = rootPath / L"src";
    wchar_t workingDirectory[32768]{};
    GetCurrentDirectoryW(_countof(workingDirectory), workingDirectory);
    std::printf("include-probe cwd: %ls\n", workingDirectory);
    D3DXContext d3dx;
    if (!LoadD3DXContext(d3dx)) {
        std::printf("SKIP include-probe: no usable D3DX9 runtime DLL (set "
                    "MMS_EFFECT_CACHE_TEST_D3DX)\n");
        return 2;
    }
    TestDeviceContext deviceContext;
    if (!CreateTestDevice(deviceContext)) {
        std::printf("SKIP include-probe: %ls device is unavailable\n",
                    deviceContext.typeName);
        UnloadD3DXContext(d3dx);
        return 2;
    }
    std::printf("include-probe compiler: %ls\n", d3dx.path.c_str());
    std::printf("include-probe device: %ls\n", deviceContext.typeName);
    const D3DX hooks = d3dx.hooks;
    const std::filesystem::path cacheDirectory = rootPath / L"cache";
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR",
                            cacheDirectory.wstring().c_str());
    ClearMemoryCache();
    TestMacro macros[] = {{"TINT_R", "0.25"}, {nullptr, nullptr}};
    auto scenario = [&](const char* fileName) {
        return RunScenario(hooks, deviceContext.device, d3dx.createFromFile,
                           (sourceDirectory / WidenTag(fileName)).c_str(),
                           macros);
    };

    CheckScenario(failures, scenario("cacheable.fx"), true, "no include");
    CheckScenario(failures, scenario("absolute.fx"), true, "absolute include");
    const std::size_t entriesAfterCacheable =
        CacheFiles(cacheDirectory).size();
    CheckScenario(failures, scenario("relative_only_src.fx"), hooks.includesRelativeToSource,
                  "relative include only in source dir");
    CheckScenario(failures, scenario("relative_both.fx"), true,
                  "relative include present in cwd and source");
    CheckScenario(failures, scenario("subdir.fx"), true,
                  "subdirectory include");
    CheckScenario(failures, scenario("nested.fx"), true, "nested include");
    const std::size_t entriesAfterRelative = CacheFiles(cacheDirectory).size();
    const auto expectedRelative = hooks.includesRelativeToSource ? 4u : 3u;
    if (entriesAfterRelative != entriesAfterCacheable + expectedRelative) {
        std::printf("[FAIL] successful relative include cases did not write the expected cache entries\n");
        ++failures;
    } else {
        std::printf("[ OK ] successful relative include cases wrote the expected entries\n");
    }

    // The shader-file preprocessor's default handler uses the source directory
    // in both DLLs. D3DX32's effect entry instead uses cwd, so the cache uses
    // its recording handler to match the effect entry's version-specific rule.
    {
        const std::wstring file =
            (sourceDirectory / L"relative_both.fx").wstring();
        void* text = nullptr;
        void* errors = nullptr;
        const HRESULT result = hooks.preprocessFromFile(
            file.c_str(), macros, nullptr, &text, &errors);
        std::string body =
            (SUCCEEDED(result) && text) ? BufferText(hooks, text) : std::string{};
        if (errors) hooks.release(errors);
        if (text) hooks.release(text);
        const bool sourceValue = body.find("0.11") != std::string::npos;
        const bool cwdValue = body.find("0.91") != std::string::npos;
        // Line-directive output is diagnostic evidence, not a cache invariant:
        // D3DX32 need not emit it, whereas D3DX43 does.
        const bool asWrittenName =
            body.find("both.fxh") != std::string::npos;
        std::printf("  preprocessed relative_both hr=0x%08lX source-dir value=%d "
                    "cwd value=%d as-written-name=%d\n",
                    static_cast<unsigned long>(result), sourceValue ? 1 : 0,
                    cwdValue ? 1 : 0, asWrittenName ? 1 : 0);
        if (!SUCCEEDED(result) || !sourceValue || cwdValue) {
            std::printf("[FAIL] preprocessor did not show the expected "
                        "source-directory include resolution\n");
            ++failures;
        }
    }

    const auto selectedInclude = (hooks.includesRelativeToSource
        ? sourceDirectory : rootPath / L"cwd") / L"both.fxh";
    const auto includeStamp = std::filesystem::last_write_time(selectedInclude);
    WriteTintInclude(selectedInclude, "0.42");
    std::filesystem::last_write_time(selectedInclude, includeStamp);
    const auto changed = scenario("relative_both.fx");
    CheckScenario(failures, changed, true, "include content change with preserved timestamp");
    if (!changed.wrapperHasTint || std::abs(changed.wrapperTint[0] - 0.42f) > 0.0001f)
        ++failures;
    ClearMemoryCache();
    CheckScenario(failures, scenario("relative_both.fx"), true, "changed include disk hit");
    CheckScenario(failures, scenario("cwd_only.fx"), true, "include fallback to cwd");
    CheckScenario(failures, scenario("nested_subdir.fx"), true,
                  "nested include uses main source directory rather than parent include directory");

    ResetCacheEnvironment();
    DestroyTestDevice(deviceContext);
    UnloadD3DXContext(d3dx);
    if (failures != 0) {
        std::printf("FAILED %d include-resolution checks\n", failures);
        return 1;
    }
    std::printf("PASS include-resolution comparison\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Parent integration: cache behaviour with a self-contained FX
// ---------------------------------------------------------------------------
int RunIntegration(const std::filesystem::path& outputRoot) {
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    if (error) {
        std::fprintf(stderr, "FAIL: cannot create output directory\n");
        return 1;
    }
    std::filesystem::path runDirectory;
    for (int attempt = 0; attempt < 100; ++attempt) {
        runDirectory =
            outputRoot /
            (std::wstring(L"effect-compile-cache-integration-") +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(attempt));
        if (!std::filesystem::exists(runDirectory)) break;
    }
    std::filesystem::create_directories(runDirectory, error);
    if (error) {
        std::fprintf(stderr, "FAIL: cannot create run directory\n");
        return 1;
    }
    std::error_code absoluteError;
    const std::filesystem::path absoluteRunDirectory =
        std::filesystem::absolute(runDirectory, absoluteError);
    if (!absoluteError) runDirectory = absoluteRunDirectory;
    int failures = 0;
    auto finish = [&]() {
        ResetCacheEnvironment();
        std::printf("integration artifacts kept in %ls\n",
                    runDirectory.c_str());
    };

    // Self-contained FX: no #include, so the original entry point and the
    // preprocessor must agree regardless of the process working directory.  The
    // macro still exercises preprocessing; Tint makes the effect content
    // observable.
    const auto effect = runDirectory / L"probe.fx";
    WriteCacheableFx(effect);
    TestMacro macros[] = {{"TINT_R", "0.25"}, {nullptr, nullptr}};
    TestMacro macros075[] = {{"TINT_R", "0.75"}, {nullptr, nullptr}};

    D3DXContext d3dx;
    if (!LoadD3DXContext(d3dx)) {
        std::printf("SKIP: no usable D3DX9 runtime DLL found (set "
                    "MMS_EFFECT_CACHE_TEST_D3DX to override)\n");
        finish();
        return 2;
    }
    std::printf("integration compiler: %ls\n", d3dx.path.c_str());
    std::printf("identity: %02x%02x%02x%02x...\n", d3dx.hooks.identity[0],
                d3dx.hooks.identity[1], d3dx.hooks.identity[2],
                d3dx.hooks.identity[3]);
    D3DX hooks = d3dx.hooks;
    g_realCompile = hooks.compileEffect;
    hooks.compileEffect = &CountingCompile;
    g_integrationCompileCalls = 0;

    TestDeviceContext deviceContext;
    if (!CreateTestDevice(deviceContext)) {
        std::printf("SKIP: %ls D3D9 device is unavailable; the cache "
                    "integration needs a device for D3DXCreateEffect (set "
                    "MMS_EFFECT_CACHE_TEST_DEVICE=hal to try a hardware "
                    "device)\n",
                    deviceContext.typeName);
        UnloadD3DXContext(d3dx);
        finish();
        return 2;
    }
    std::printf("integration device: %ls\n", deviceContext.typeName);
    auto shutdown = [&]() {
        DestroyTestDevice(deviceContext);
        UnloadD3DXContext(d3dx);
        finish();
    };

    const auto original =
        reinterpret_cast<CreateEffectFromFileW>(d3dx.createFromFile);
    auto loadOriginal = [&](const wchar_t* file, const void* macroList,
                            std::string& diagnostics, bool* created,
                            float tint[4]) {
        void* effectObject = nullptr;
        void* errors = nullptr;
        const HRESULT result = original(deviceContext.device, file, macroList,
                                        nullptr, 0, nullptr, &effectObject,
                                        &errors);
        diagnostics = BufferText(hooks, errors);
        if (created) *created = effectObject != nullptr;
        if (tint) {
            if (!effectObject || !ReadEffectTint(effectObject, tint)) {
                tint[0] = tint[1] = tint[2] = tint[3] = 0.0f;
            }
        }
        if (errors) hooks.release(errors);
        if (effectObject) hooks.release(effectObject);
        return result;
    };

    // Baseline before any cache state exists: the untouched original entry
    // point must load the same legal FX.  A failure is reported with the full
    // HRESULT and D3DX diagnostics and stays a hard failure - never a skip.
    std::string baselineDiagnostics;
    bool baselineCreated = false;
    float baselineTint[4]{};
    const HRESULT baselineResult =
        loadOriginal(effect.c_str(), macros, baselineDiagnostics,
                     &baselineCreated, baselineTint);
    if (FAILED(baselineResult) || !baselineCreated) {
        wchar_t currentDirectory[32768]{};
        GetCurrentDirectoryW(_countof(currentDirectory), currentDirectory);
        std::printf(
            "FAIL: original D3DXCreateEffectFromFileW could not load the legal "
            "FX before any cache creation\n"
            "  file: %ls\n"
            "  cwd: %ls\n"
            "  hr=0x%08lX effect=%s\n"
            "  diagnostics:\n%s\n",
            effect.c_str(), currentDirectory,
            static_cast<unsigned long>(baselineResult),
            baselineCreated ? "created" : "null",
            baselineDiagnostics.empty() ? "(no error text)\n"
                                        : baselineDiagnostics.c_str());
        const auto minimal = runDirectory / L"minimal.fx";
        WriteText(minimal,
                  "float4x4 WorldViewProj;\n"
                  "float4 MainPS(float2 uv : TEXCOORD0) : COLOR "
                  "{ return float4(1.0, 1.0, 1.0, 1.0); }\n"
                  "technique Technique0 { pass P0 { PixelShader = compile "
                  "ps_2_0 MainPS(); } }\n");
        std::string minimalDiagnostics;
        bool minimalCreated = false;
        float minimalTint[4]{};
        const HRESULT minimalResult =
            loadOriginal(minimal.c_str(), nullptr, minimalDiagnostics,
                         &minimalCreated, minimalTint);
        std::printf(
            "diagnostic: minimal no-include FX through the same original entry "
            "point hr=0x%08lX effect=%s\n%s\n",
            static_cast<unsigned long>(minimalResult),
            minimalCreated ? "created" : "null",
            minimalDiagnostics.empty() ? "(no error text)\n"
                                       : minimalDiagnostics.c_str());
        failures += CheckIntegration(
            false,
            "original entry point loads the legal FX before cache creation");
        shutdown();
        return 1;
    }
    std::printf("baseline original FX load hr=0x%08lX tint=%s\n",
                static_cast<unsigned long>(baselineResult),
                TintText(baselineTint).c_str());
    failures += CheckIntegration(
        true, "original entry point loads the legal FX before cache creation");

    auto cacheDirectory = runDirectory / L"cache";
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR",
                            cacheDirectory.wstring().c_str());
    ClearMemoryCache();

    auto create = [&](const wchar_t* file, const void* macroList, DWORD flags,
                      void** created, float tint[4]) {
        void* errors = nullptr;
        HRESULT result = E_FAIL;
        const bool handled = TryCreateEffectWithHooks(
            hooks, deviceContext.device, file, macroList, nullptr, flags,
            nullptr, created, &errors, result);
        if (errors) hooks.release(errors);
        if (handled && tint) {
            if (!*created || !ReadEffectTint(*created, tint)) {
                tint[0] = tint[1] = tint[2] = tint[3] = 0.0f;
            }
        }
        return handled && SUCCEEDED(result) && *created != nullptr;
    };

    void* firstEffect = nullptr;
    float firstTint[4]{};
    if (!create(effect.c_str(), macros, 0, &firstEffect, firstTint)) {
        std::printf("FAIL: cached path declined or failed with a working %ls device; "
                    "original source creation already passed (check cache diagnostics; "
                    "MMS_EFFECT_CACHE_TEST_DEVICE=hal)\n",
                    deviceContext.typeName);
        shutdown();
        return 1;
    }
    failures += CheckIntegration(true,
                                 "cold real-D3DX compile and effect creation");
    failures += CheckIntegration(g_integrationCompileCalls == 1,
                                 "cold run called CompileEffect once");
    failures += CheckIntegration(SameTint(firstTint, baselineTint),
                                 "cached effect matches the original effect "
                                 "parameters");

    void* secondEffect = nullptr;
    failures += CheckIntegration(
        create(effect.c_str(), macros, 0, &secondEffect, nullptr),
        "warm real-D3DX run reuses bytecode and creates an effect");
    failures += CheckIntegration(g_integrationCompileCalls == 1,
                                 "warm run did not call CompileEffect again");
    failures += CheckIntegration(firstEffect != secondEffect,
                                 "warm run created an independent effect");

    ClearMemoryCache();
    void* diskEffect = nullptr;
    float diskTint[4]{};
    failures += CheckIntegration(
        create(effect.c_str(), macros, 0, &diskEffect, diskTint),
        "disk hit across a cleared memory cache creates an effect");
    failures += CheckIntegration(g_integrationCompileCalls == 1,
                                 "disk hit did not call CompileEffect again");
    failures += CheckIntegration(SameTint(diskTint, baselineTint),
                                 "disk-hit effect matches the original effect "
                                 "parameters");

    void* macroEffect = nullptr;
    float macroTint[4]{};
    failures += CheckIntegration(
        create(effect.c_str(), macros075, 0, &macroEffect, macroTint),
        "macro change recompiles and creates an effect");
    failures += CheckIntegration(g_integrationCompileCalls == 2,
                                 "macro change was recompiled");
    std::string macroOriginalDiagnostics;
    bool macroOriginalCreated = false;
    float macroOriginalTint[4]{};
    const HRESULT macroOriginalResult = loadOriginal(
        effect.c_str(), macros075, macroOriginalDiagnostics,
        &macroOriginalCreated, macroOriginalTint);
    failures += CheckIntegration(
        SUCCEEDED(macroOriginalResult) && macroOriginalCreated &&
            SameTint(macroTint, macroOriginalTint),
        "macro-change effect matches the original entry point");

    const auto entries = CacheFiles(cacheDirectory);
    failures += CheckIntegration(!entries.empty(),
                                 "integration runs wrote a disk entry");
    for (const auto& entry : entries) {
        const auto bytes = ReadWholeFile(entry);
        if (bytes.size() > 40)
            WriteBytes(entry, std::vector<unsigned char>(bytes.begin(),
                                                         bytes.begin() + 40));
    }
    ClearMemoryCache();
    void* corruptedEffect = nullptr;
    failures += CheckIntegration(
        create(effect.c_str(), macros075, 0, &corruptedEffect, nullptr),
        "truncated integration entry recompiles and creates an effect");
    failures += CheckIntegration(g_integrationCompileCalls == 3,
                                 "truncated entry was treated as a miss");

    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1");
    {
        void* effectObject = nullptr;
        void* errors = nullptr;
        HRESULT result = E_FAIL;
        const bool handled = TryCreateEffectWithHooks(
            hooks, deviceContext.device, effect.c_str(), macros075, nullptr, 0,
            nullptr, &effectObject, &errors, result);
        failures += CheckIntegration(
            !handled,
            "MMS_EFFECT_CACHE_DISABLE bypasses the cached path");
        std::string diagnostics;
        bool created = false;
        float originalTint[4]{};
        const HRESULT originalResult = loadOriginal(
            effect.c_str(), macros075, diagnostics, &created, originalTint);
        if (FAILED(originalResult) || !created) {
            std::printf(
                "cache-disabled original call hr=0x%08lX effect=%s\n"
                "  file: %ls\n"
                "  diagnostics:\n%s\n",
                static_cast<unsigned long>(originalResult),
                created ? "created" : "null", effect.c_str(),
                diagnostics.empty() ? "(no error text)\n"
                                    : diagnostics.c_str());
        }
        failures += CheckIntegration(
            SUCCEEDED(originalResult) && created &&
                SameTint(originalTint, macroTint),
            "cache-disabled call loads through the original D3DX entry point "
            "with the same parameters");
    }
    SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", nullptr);

    const auto broken = runDirectory / L"broken.fx";
    WriteText(broken, "#include \"missing-include.fxh\"\nfloat4 MainPS() : "
                      "COLOR { return 1; }\n");
    {
        void* effectObject = nullptr;
        void* errors = nullptr;
        HRESULT result = E_FAIL;
        const bool handled = TryCreateEffectWithHooks(
            hooks, deviceContext.device, broken.c_str(), nullptr, nullptr, 0,
            nullptr, &effectObject, &errors, result);
        failures += CheckIntegration(
            !handled, "preprocessor failure falls back to the original call");
        if (effectObject) hooks.release(effectObject);
        if (errors) hooks.release(errors);
        std::string diagnostics;
        bool created = false;
        const HRESULT originalResult = loadOriginal(
            broken.c_str(), nullptr, diagnostics, &created, nullptr);
        failures += CheckIntegration(FAILED(originalResult),
                                     "original call reports the same failure");
        failures += CheckIntegration(!diagnostics.empty(),
                                     "original error text stays observable");
        std::printf(
            "MME_EFFECT_CACHE integration fault hr=0x%08lX effect=%s\n"
            "  diagnostics:\n%s\n",
            static_cast<unsigned long>(originalResult),
            created ? "created" : "null",
            diagnostics.empty() ? "(no error text)\n"
                                : diagnostics.c_str());
    }

    {
        // The production entry point resolves the same DLL identity itself.
        ClearMemoryCache();
        void* effectObject = nullptr;
        void* errors = nullptr;
        HRESULT result = E_FAIL;
        const bool handled = TryCreateEffect(
            d3dx.createFromFile, deviceContext.device, effect.c_str(), macros,
            nullptr, 0, nullptr, &effectObject, &errors, result);
        float tint[4]{};
        const bool hasTint =
            effectObject != nullptr && ReadEffectTint(effectObject, tint);
        failures += CheckIntegration(
            handled && SUCCEEDED(result) && effectObject != nullptr &&
                hasTint && SameTint(tint, baselineTint),
            "production TryCreateEffect resolves the real D3DX DLL and loads "
            "with the original parameters");
        if (errors) hooks.release(errors);
        if (effectObject) hooks.release(effectObject);
    }

    // Preserve the exact original warnings on cold, memory and disk hits.
    // A source-only line change must invalidate the diagnostic text even when
    // the preprocessed shader tokens are identical.
    {
        const auto warningFile = runDirectory / L"warning.fx";
        const std::string warningSource = hooks.includesRelativeToSource ?
            "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n"
            "float4 MainPS(float2 uv:TEXCOORD0):COLOR {\n"
            "  float scalar = Tint;\n"
            "  return scalar;\n"
            "}\n"
            "technique Technique0 { pass P0 { PixelShader = compile ps_2_0 MainPS(); } }\n" :
            "float4 Tint = float4(0.25, 0.5, 0.75, 1.0);\n"
            "float4 MainPS(float2 uv:TEXCOORD0):COLOR {\n"
            "  float scalar = dot(Tint, Tint);\n"
            "  return scalar / 0.0;\n"
            "}\n"
            "technique Technique0 { pass P0 { PixelShader = compile ps_2_0 MainPS(); } }\n";
        WriteText(warningFile, warningSource);
        const auto stamp = std::filesystem::last_write_time(warningFile);
        {
            void* compiler = nullptr;
            void* parseErrors = nullptr;
            void* binary = nullptr;
            void* compileErrors = nullptr;
            const HRESULT parseResult = hooks.createCompilerFromFile(
                warningFile.c_str(), nullptr, nullptr, 0, &compiler, &parseErrors);
            const HRESULT compileResult = compiler
                ? hooks.compileEffect(compiler, 0, &binary, &compileErrors) : E_FAIL;
            failures += CheckIntegration(SUCCEEDED(parseResult) && SUCCEEDED(compileResult) &&
                (!BufferText(hooks, parseErrors).empty() ||
                 !BufferText(hooks, compileErrors).empty()),
                "warning fixture compiles successfully and produces compiler diagnostics");
            if (parseErrors) hooks.release(parseErrors);
            if (compileErrors) hooks.release(compileErrors);
            if (binary) hooks.release(binary);
            if (compiler) hooks.release(compiler);
        }
        const int before = g_integrationCompileCalls;
        auto checkWarning = [&](const char* label) {
            const auto outcome = RunScenario(hooks, deviceContext.device,
                d3dx.createFromFile, warningFile.c_str(), nullptr);
            CheckScenario(failures, outcome, true, label);
            // D3DX43's effect entry discards this parse warning, whereas 32
            // returns its compile warning. Match the original in either case.
            failures += CheckIntegration(hooks.includesRelativeToSource
                    ? outcome.originalDiagnostics.empty()
                    : !outcome.originalDiagnostics.empty(),
                "caller-visible diagnostics follow this D3DX version");
        };
        checkWarning("warning cold");
        failures += CheckIntegration(g_integrationCompileCalls == before + 1,
                                     "warning bytecode compiled once");
        checkWarning("warning memory hit");
        ClearMemoryCache();
        checkWarning("warning disk hit");
        failures += CheckIntegration(g_integrationCompileCalls == before + 1,
                                     "warning memory/disk hits do not recompile");
        {
            void* first = nullptr;
            void* second = nullptr;
            void* firstErrors = nullptr;
            void* secondErrors = nullptr;
            HRESULT firstResult = E_FAIL;
            HRESULT secondResult = E_FAIL;
            const bool firstHandled = TryCreateEffectWithHooks(hooks, deviceContext.device,
                warningFile.c_str(), nullptr, nullptr, 0, nullptr,
                &first, &firstErrors, firstResult);
            const bool secondHandled = TryCreateEffectWithHooks(hooks, deviceContext.device,
                warningFile.c_str(), nullptr, nullptr, 0, nullptr,
                &second, &secondErrors, secondResult);
            failures += CheckIntegration(firstHandled && secondHandled && first && second &&
                first != second && SUCCEEDED(firstResult) && SUCCEEDED(secondResult),
                "warning cache hits own independent effect objects");
            if (!hooks.includesRelativeToSource) {
                failures += CheckIntegration(firstErrors && secondErrors &&
                    firstErrors != secondErrors &&
                    BufferText(hooks, firstErrors) == BufferText(hooks, secondErrors),
                    "warning cache hits own independent diagnostic buffers");
                if (firstErrors && secondErrors) {
                    const auto secondText = BufferText(hooks, secondErrors);
                    auto* firstBytes = static_cast<unsigned char*>(hooks.bufferPointer(firstErrors));
                    if (firstBytes) firstBytes[0] ^= 1;
                    failures += CheckIntegration(BufferText(hooks, secondErrors) == secondText,
                        "mutating one returned diagnostic does not change another");
                }
            }
            if (firstErrors) hooks.release(firstErrors);
            if (secondErrors) hooks.release(secondErrors);
            if (first) hooks.release(first);
            if (second) hooks.release(second);
        }
        WriteText(warningFile, "\n\n" + warningSource);
        std::filesystem::last_write_time(warningFile, stamp);
        checkWarning("warning line-number change");
        failures += CheckIntegration(g_integrationCompileCalls == before + 2,
            "source bytes invalidate diagnostics even with unchanged timestamp");
    }

    // MME 0.37 uses this shader flag in the real project. Check the exact
    // native flag path in both DLL versions, then check its cached result.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto outcome = RunScenario(hooks, deviceContext.device,
            d3dx.createFromFile, effect.c_str(), macros, 0x1000);
        CheckScenario(failures, outcome, true,
            attempt ? "MME flags cached" : "MME flags cold");
    }

    // ---- include-resolution comparison in a child process ----
    {
        const std::filesystem::path includeRoot =
            runDirectory / L"include-probe";
        const std::filesystem::path sourceDir = includeRoot / L"src";
        const std::filesystem::path cwdDir = includeRoot / L"cwd";
        std::error_code includeError;
        std::filesystem::create_directories(sourceDir, includeError);
        std::filesystem::create_directories(cwdDir, includeError);
        if (includeError) {
            failures += CheckIntegration(
                false, "include-resolution fixture directories are created");
        } else {
            WriteCacheableFx(sourceDir / L"cacheable.fx");
            WriteTintInclude(sourceDir / L"abs_common.fxh", "0.35");
            WriteIncludedMain(sourceDir / L"absolute.fx",
                              IncludeLine(sourceDir / L"abs_common.fxh"));
            WriteTintInclude(sourceDir / L"only_src.fxh", "0.12");
            WriteIncludedMain(sourceDir / L"relative_only_src.fx",
                              "#include \"only_src.fxh\"");
            WriteTintInclude(sourceDir / L"both.fxh", "0.11");
            WriteTintInclude(cwdDir / L"both.fxh", "0.91");
            WriteIncludedMain(sourceDir / L"relative_both.fx",
                              "#include \"both.fxh\"");
            std::filesystem::create_directories(sourceDir / L"sub",
                                                includeError);
            std::filesystem::create_directories(cwdDir / L"sub", includeError);
            WriteTintInclude(sourceDir / L"sub" / L"inner.fxh", "0.22");
            WriteTintInclude(cwdDir / L"sub" / L"inner.fxh", "0.92");
            WriteIncludedMain(sourceDir / L"subdir.fx",
                              "#include \"sub/inner.fxh\"");
            WriteText(sourceDir / L"outer.fxh",
                      "#include \"inner.fxh\"\n");
            WriteTintInclude(sourceDir / L"inner.fxh", "0.33");
            WriteText(cwdDir / L"outer.fxh", "#include \"inner.fxh\"\n");
            WriteTintInclude(cwdDir / L"inner.fxh", "0.93");
            WriteIncludedMain(sourceDir / L"nested.fx",
                              "#include \"outer.fxh\"");
            WriteTintInclude(cwdDir / L"only_cwd.fxh", "0.72");
            WriteIncludedMain(sourceDir / L"cwd_only.fx",
                              "#include \"only_cwd.fxh\"");
            WriteText(sourceDir / L"sub" / L"outer.fxh", "#include \"inner.fxh\"\n");
            WriteText(cwdDir / L"sub" / L"outer.fxh", "#include \"inner.fxh\"\n");
            WriteIncludedMain(sourceDir / L"nested_subdir.fx",
                              "#include \"sub/outer.fxh\"");
            wchar_t executable[32768]{};
            const DWORD executableLength =
                GetModuleFileNameW(nullptr, executable, _countof(executable));
            std::wstring commandLine;
            if (executableLength && executableLength < _countof(executable)) {
                commandLine = L"\"" + std::wstring(executable) +
                              L"\" --include-probe \"" +
                              includeRoot.wstring() + L"\"";
            }
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            const BOOL started =
                !commandLine.empty() &&
                CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
                               TRUE, 0, nullptr, cwdDir.c_str(), &startup,
                               &process);
            if (!started) {
                std::printf(
                    "[FAIL] include-resolution comparison child starts "
                    "(GetLastError=%lu)\n",
                    static_cast<unsigned long>(GetLastError()));
                ++failures;
            } else {
                WaitForSingleObject(process.hProcess, 300000);
                DWORD exitCode = 1;
                GetExitCodeProcess(process.hProcess, &exitCode);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                std::printf("include-resolution comparison child exit=%lu\n",
                            static_cast<unsigned long>(exitCode));
                failures += CheckIntegration(
                    exitCode == 0,
                    "include-resolution comparison child passed");
                if (exitCode == 2) {
                    std::printf(
                        "note: child skipped because the local runtime or "
                        "device is unavailable\n");
                }
            }
        }
    }

    if (firstEffect) hooks.release(firstEffect);
    if (secondEffect) hooks.release(secondEffect);
    if (diskEffect) hooks.release(diskEffect);
    if (macroEffect) hooks.release(macroEffect);
    if (corruptedEffect) hooks.release(corruptedEffect);
    shutdown();
    if (failures != 0) {
        std::printf("FAILED %d integration checks\n", failures);
        return 1;
    }
    std::printf("PASS real-D3DX cache integration\n");
    return 0;
}

}  // namespace

DWORD WINAPI SmallStackCacheWorker(void*) {
    try {
        TestNamedCacheLayout();
        return g_failures?1:0;
    } catch (...) { return 2; }
}
int RunSmallStackCache() {
    // MME can enter the host with much of the UI stack already occupied.
    // These are the complete cold/warm cache paths, not a path-helper mock.
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    DWORD threadId=0;
    HANDLE worker=CreateThread(nullptr,128*1024,SmallStackCacheWorker,nullptr,
        STACK_SIZE_PARAM_IS_A_RESERVATION,&threadId);
    if(!worker){std::puts("FAIL small-stack worker creation");return 1;}
    const auto wait=WaitForSingleObject(worker,15000);
    if(wait!=WAIT_OBJECT_0){std::puts("FAIL small-stack worker timeout");ExitProcess(2);}
    DWORD result=1;GetExitCodeThread(worker,&result);CloseHandle(worker);
    std::printf("small-stack cache: reserve=128 KiB checks=%d failures=%d exit=%lu\n",g_checks,g_failures,result);
    return result==0 && !g_failures?0:1;
}
int main(int argc, char** argv) {
    if(argc==2 && std::strcmp(argv[1],"--small-stack")==0)return RunSmallStackCache();
    if (argc >= 3 && std::strcmp(argv[1], "--include-probe") == 0) {
        std::wstring root;
        if (!WidenArgument(argv[2], root)) {
            std::fprintf(stderr, "invalid --include-probe root path\n");
            return 2;
        }
        return RunIncludeProbe(std::filesystem::path(root));
    }
    if (argc >= 3 && std::strcmp(argv[1], "--integration") == 0) {
        std::wstring output;
        if (!WidenArgument(argv[2], output)) {
            std::fprintf(stderr, "invalid --integration output path\n");
            return 1;
        }
        return RunIntegration(std::filesystem::path(output));
    }
    if (argc >= 2) {
        std::fprintf(stderr,
                     "usage: effect_compile_cache_tests "
                     "[--integration <new output directory> | "
                     "--include-probe <root>]\n");
        return 1;
    }
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", nullptr);
    RunTest("cache-directory", &TestCacheDirectoryPaths);
    RunTest("named-cache-layout", &TestNamedCacheLayout);
    RunTest("memory-only", &TestMemoryOnly);
    RunTest("key-invalidation", &TestKeyInvalidation);
    RunTest("non-ascii-source-path", &TestNonAsciiSourcePath);
    RunTest("corrupt-entries", &TestCorruptEntries);
    RunTest("fallback-and-diagnostics", &TestFallbackAndDiagnostics);
    RunTest("limits-and-unwritable-cache", &TestLimitsAndUnwritableCache);
    RunTest("disk-budget", &TestDiskBudget);
    RunTest("exception-safety", &TestExceptionSafety);
    SetDiskLimitsForTesting(0, 0);
    ResetCacheEnvironment();
    Check(g_fakeBuffersCreated == g_fakeBuffersReleased,
          "fake COM buffers balanced");
    Check(g_fakeCompilersCreated == g_fakeCompilersReleased,
          "fake compilers balanced");
    Check(g_fakeEffectsCreated == g_fakeEffectsReleased,
          "fake effects balanced");
    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS %d effect compile-cache checks\n", g_checks);
    return 0;
}
