// MAINTENANCE: regression-keep; global processor policy (host module).
// Covers role constraints and the FollowRenderer default, Requested/Active
// separation with the one-time startup freeze, atomic saves that keep the old
// file and the old Requested on failure, stale-LUID rebinding by full hardware
// identity with ambiguous/missing fallback, duplicate-name safety, restart
// matching, the one-shot Vertices->skinning conversion, the MMV callback
// (follow/CPU/adapter, software-rasterizer follow) and the recorded device
// result (REF = CPU mode, total failure = nothing displayed as running).
// Hidden run: no window is shown anywhere.  Logs and fixtures land in
// .local/artifacts/global-processor/ (kept on failure).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/skinning_backend.hpp"

using namespace mikumikustudio;
namespace processor = mikumikustudio::processor;
namespace {
FILE* g_log = nullptr;
unsigned g_checks = 0;

void LogLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputc('\n', stdout);
    if (g_log != nullptr) {
        va_start(args, format);
        std::vfprintf(g_log, format, args);
        va_end(args);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}
void Check(bool ok, const char* message) {
    ++g_checks;
    if (ok) return;
    LogLine("FAIL %s", message);
    if (g_log != nullptr) { std::fclose(g_log); g_log = nullptr; }
    std::exit(1);
}

std::wstring RepoRoot() {
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring directory(module);
    for (int depth = 0; depth < 8; ++depth) {
        const auto slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        directory.resize(slash);
        const std::wstring probe = directory + L"\\CMakeLists.txt";
        if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW((directory + L"\\src").c_str()) != INVALID_FILE_ATTRIBUTES)
            return directory;
    }
    return L".";
}
void EnsureDirectory(const std::wstring& directory) {
    if (directory.empty()) return;
    if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const auto slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos) EnsureDirectory(directory.substr(0, slash));
    CreateDirectoryW(directory.c_str(), nullptr);
}
// Snapshot of the real per-user config file: this run must never create,
// modify or delete it (tests only use injected paths under .local/artifacts).
struct FileStamp {
    bool exists = false;
    unsigned long long size = 0;
    unsigned long long mtime = 0;
};
FileStamp Stamp(const std::wstring& path) {
    FileStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path.empty() &&
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        stamp.exists = true;
        stamp.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
                     data.nFileSizeLow;
        stamp.mtime =
            (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
    }
    return stamp;
}

std::wstring ReadText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const DWORD size = GetFileSize(file, nullptr);
    std::string bytes(size, '\0');
    DWORD read = 0;
    const BOOL ok = size == 0 || ReadFile(file, &bytes[0], size, &read, nullptr);
    CloseHandle(file);
    if (!ok) return {};
    // The config file is UTF-16 LE with a BOM (written by the module).
    if (read >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring text(read / 2 - 1, L'\0');
        if (!text.empty())
            std::memcpy(&text[0], bytes.data() + 2, text.size() * sizeof(wchar_t));
        return text;
    }
    const int wide = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                         static_cast<int>(read), nullptr, 0);
    std::wstring text(wide > 0 ? wide : 0, L'\0');
    if (wide > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                      static_cast<int>(read), &text[0], wide);
    return text;
}

processor::Selection AdapterChoice(std::uint64_t id, const wchar_t* name,
                                   processor::DeviceIdentity identity = {}) {
    processor::Selection selection;
    selection.mode = processor::Mode::Adapter;
    selection.adapterId = id;
    selection.adapterName = name;
    selection.identity = identity;
    return selection;
}
processor::AdapterEntry Entry(unsigned ordinal, std::uint64_t id,
                              const wchar_t* name,
                              processor::DeviceIdentity identity,
                              unsigned rank) {
    processor::AdapterEntry entry;
    entry.ordinal = ordinal;
    entry.adapterId = id;
    entry.adapterName = name;
    entry.identity = identity;
    entry.performanceRank = rank;
    return entry;
}
std::uint64_t PackLuid(const LUID& luid) {
    return (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart)) << 32) |
           static_cast<std::uint32_t>(luid.LowPart);
}
// The 9Ex oracle must come from the system directory by absolute path (never
// a static import, never a basename load), verified with GetModuleFileNameW +
// CompareStringOrdinal.  The resolved path is logged as loader evidence.  The
// caller releases the COM object and then FreeLibrary's the module.
using Create9ExProc = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
Create9ExProc SystemCreate9Ex(HMODULE& module) {
    module = nullptr;
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return nullptr;
    const std::wstring expected = std::wstring(directory) + L"\\d3d9.dll";
    HMODULE loaded = LoadLibraryW(expected.c_str());  // absolute path
    if (loaded == nullptr) return nullptr;
    wchar_t resolved[MAX_PATH]{};
    const DWORD got = GetModuleFileNameW(loaded, resolved, MAX_PATH);
    if (got == 0 || got >= MAX_PATH ||
        CompareStringOrdinal(resolved, -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
        LogLine("FAIL system d3d9.dll path verification (resolved=%ls)", resolved);
        FreeLibrary(loaded);
        return nullptr;
    }
    LogLine("9Ex oracle module: %ls", resolved);
    const auto proc = reinterpret_cast<Create9ExProc>(
        GetProcAddress(loaded, "Direct3DCreate9Ex"));
    if (proc == nullptr) {
        FreeLibrary(loaded);
        return nullptr;
    }
    module = loaded;
    return proc;
}

void RestartFrom(const std::wstring& path) {
    processor::ResetForTesting();
    processor::SetConfigPathOverrideForTesting(path.c_str());
}

// ---- defaults: Mmv is FollowRenderer everywhere --------------------------
void DefaultsAndConstraints() {
    const processor::Configuration defaults{};
    Check(defaults.renderer.mode == processor::Mode::Automatic &&
              defaults.vertices.mode == processor::Mode::Automatic &&
              defaults.mmv.mode == processor::Mode::FollowRenderer,
          "Configuration{} defaults Mmv to FollowRenderer");
    processor::ResetForTesting();
    Check(processor::Requested().mmv.mode == processor::Mode::FollowRenderer &&
              processor::Active().mmv.mode == processor::Mode::FollowRenderer,
          "ResetForTesting keeps the FollowRenderer default");

    std::wstring reason;
    Check(processor::IsValid(processor::Role::Renderer,
                             {processor::Mode::Automatic, 0, L"", {}}),
          "renderer accepts automatic");
    Check(processor::IsValid(processor::Role::Renderer,
                             AdapterChoice(0x10, L"GPU")),
          "renderer accepts a concrete adapter");
    Check(!processor::IsValid(processor::Role::Renderer,
                              {processor::Mode::Cpu, 0, L"", {}}, &reason) &&
              !reason.empty(),
          "renderer rejects CPU with a reason");
    Check(!processor::IsValid(processor::Role::Renderer,
                              {processor::Mode::FollowRenderer, 0, L"", {}}, &reason) &&
              !reason.empty(),
          "renderer rejects follow-renderer with a reason");
    Check(!processor::IsValid(processor::Role::Renderer,
                              AdapterChoice(0, L"GPU"), &reason) &&
              !reason.empty(),
          "adapter without a device id is rejected");
    Check(processor::IsValid(processor::Role::Vertices,
                             {processor::Mode::Cpu, 0, L"", {}}),
          "vertices accepts CPU");
    Check(!processor::IsValid(processor::Role::Vertices,
                              {processor::Mode::FollowRenderer, 0, L"", {}}, &reason) &&
              !reason.empty(),
          "vertices rejects follow-renderer");
    Check(processor::IsValid(processor::Role::Mmv,
                             {processor::Mode::FollowRenderer, 0, L"", {}}),
          "mmv accepts follow-renderer");
    Check(processor::IsValid(processor::Role::Mmv,
                             {processor::Mode::Cpu, 0, L"", {}}),
          "mmv accepts CPU");
    processor::Configuration bad;
    bad.vertices.mode = processor::Mode::FollowRenderer;
    Check(!processor::IsValid(bad, &reason) &&
              reason.find(L"Vertices") != std::wstring::npos,
          "configuration validation names the failing role");
    LogLine("PASS defaults (Mmv=FollowRenderer) and role constraints");
}

// ---- save / reload round trip and restart matching -----------------------
void RoundTripAndRestart(const std::wstring& path) {
    processor::DeviceIdentity identity{0x10DE, 0x1234, 0x10DE, 0xA1};
    processor::Configuration config;
    config.renderer = AdapterChoice(0xAAAABBBBCCCCDDDDull, L"Renderer GPU", identity);
    config.vertices.mode = processor::Mode::Cpu;
    config.mmv = AdapterChoice(0x1111222233334444ull, L"MMV GPU",
                               {0x1002, 0x5678, 0x1002, 0xB2});
    std::wstring error;
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save requested configuration");
    Check(error.empty(), "a successful save reports no error");
    // Simulated next launch: fresh state, same file.
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize reads the file");
    Check(error.empty(), "a clean file produces no warning");
    Check(processor::Active().renderer == config.renderer &&
              processor::Active().vertices == config.vertices &&
              processor::Active().mmv == config.mmv,
          "restart restores all three roles including the hardware identity");
    Check(processor::Requested().renderer == config.renderer,
          "requested equals the saved selection after restart");
    const std::wstring text = ReadText(path);
    Check(text.find(L"Mode=adapter") != std::wstring::npos &&
              text.find(L"Mode=cpu") != std::wstring::npos &&
              text.find(L"AdapterName=Renderer GPU") != std::wstring::npos &&
              text.find(L"VendorId=000010de") != std::wstring::npos &&
              text.find(L"SubSysId=000010de") != std::wstring::npos,
          "the file carries the three roles and the hardware identity in plain text");
    LogLine("PASS save/reload round trip with identity and restart matching");
}

// ---- Requested vs Active: freeze once, later saves only wait -------------
void FreezeAfterInitialize(const std::wstring& path) {
    processor::Configuration first;
    first.vertices.mode = processor::Mode::Cpu;
    processor::Configuration second;
    second.vertices = AdapterChoice(0x5555666677778888ull, L"Later GPU");
    second.renderer = AdapterChoice(0x9999AAAABBBBCCCCull, L"Later renderer");
    std::wstring error;
    RestartFrom(path);
    Check(processor::SaveRequested(first, error), "save the first configuration");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize freezes the active set");
    Check(processor::SaveRequested(second, error),
          "the UI may save a new selection without activating it");
    Check(processor::Requested().vertices == second.vertices,
          "requested shows the newly saved selection");
    Check(processor::Active().vertices == first.vertices,
          "active keeps the frozen selection (takes effect next launch)");
    Check(processor::Initialize(error), "a second initialize is harmless");
    Check(processor::Active().vertices == first.vertices,
          "active is frozen exactly once");
    LogLine("PASS requested/active separation with a one-time freeze");
}

// ---- atomic save failures keep the old file and the old Requested --------
void SaveFailuresKeepEverything(const std::wstring& directory) {
    const std::wstring path = directory + L"\\processor.ini";
    processor::Configuration first;
    first.vertices.mode = processor::Mode::Cpu;
    processor::Configuration second;
    second.vertices = AdapterChoice(0x1234ull, L"Never written");
    std::wstring error;
    RestartFrom(path);
    Check(processor::SaveRequested(first, error), "save the baseline configuration");

    // Failure 1: the target file is read-only, the replace step must fail.
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0,
          "make the config read-only");
    Check(!processor::SaveRequested(second, error) && !error.empty(),
          "saving over a read-only file fails with an error");
    Check(processor::Requested().vertices == first.vertices,
          "a failed save keeps the previous requested value");
    Check(ReadText(path).find(L"Mode=cpu") != std::wstring::npos,
          "a failed save leaves the old file contents untouched");
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    // Failure 2: a plain file blocks the configuration directory.  The module
    // state keeps the Requested value of the earlier successful save.
    const std::wstring blocker = directory + L"\\blocker";
    HANDLE file = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create the blocking file");
    CloseHandle(file);
    processor::SetConfigPathOverrideForTesting(
        (blocker + L"\\processor.ini").c_str());
    Check(!processor::SaveRequested(second, error) && !error.empty(),
          "saving into an impossible directory fails with an error");
    Check(processor::Requested().vertices == first.vertices,
          "the failed write keeps the previous requested value");
    LogLine("PASS atomic save failures keep the old file and the old requested value");
}

// ---- resolution: identity rebinding, fallbacks, duplicate names ---------
void ResolutionTables() {
    const processor::DeviceIdentity twinIdentity{1, 2, 3, 4};
    const processor::DeviceIdentity soloIdentity{5, 6, 7, 8};
    const processor::DeviceIdentity splitIdentity{9, 10, 11, 12};
    std::vector<processor::AdapterEntry> table;
    table.push_back(Entry(0, 0x100, L"Twin", twinIdentity, 2));
    table.push_back(Entry(1, 0x200, L"Twin", twinIdentity, 0));
    table.push_back(Entry(2, 0x300, L"Solo", soloIdentity, 1));
    table.push_back(Entry(3, 0x400, L"Split A", splitIdentity, 3));
    table.push_back(Entry(4, 0x500, L"Split B", splitIdentity, 4));

    // Automatic prefers the better performance rank.
    const auto automatic = processor::ResolveFromTable(
        {processor::Mode::Automatic, 0, L"", {}}, table);
    Check(!automatic.fellBack && automatic.adapterOrdinal == 1,
          "automatic prefers the high-performance adapter");

    // Exact id wins even when the name is duplicated.
    const auto byId = processor::ResolveFromTable(
        AdapterChoice(0x100, L"Twin", twinIdentity), table);
    Check(!byId.fellBack && byId.adapterOrdinal == 0,
          "an exact device id binds to that adapter despite the duplicate name");

    // A stale LUID rebinds by a unique full hardware identity.
    const auto staleLuid = processor::ResolveFromTable(
        AdapterChoice(0xFFFF0000FFFF0000ull, L"renamed by driver", soloIdentity),
        table);
    Check(!staleLuid.fellBack && staleLuid.adapterOrdinal == 2 &&
              staleLuid.adapterId == 0x300,
          "a stale LUID rebinds by the unique hardware identity");

    // An old LUID that another card now owns must never steal the binding:
    // the saved identity wins over the saved LUID.
    const auto reusedLuid = processor::ResolveFromTable(
        AdapterChoice(0x200, L"Twin", soloIdentity), table);
    Check(!reusedLuid.fellBack && reusedLuid.adapterOrdinal == 2 &&
              reusedLuid.adapterId == 0x300,
          "an old LUID owned by a different card never steals the binding");

    // Same name, different chip: with a saved identity that no device matches,
    // neither the name nor a colliding LUID may force a bind.
    const auto sameNameOtherChip = processor::ResolveFromTable(
        AdapterChoice(0x300, L"Solo", {0x7777, 0x8888, 0x9999, 0x11}), table);
    Check(sameNameOtherChip.fellBack &&
              !sameNameOtherChip.fallbackReason.empty(),
          "a same-named card with a different chip identity is never bound");

    // Without a saved identity nothing is verifiable after a reboot (the
    // LUID may belong to another card now): report the fallback instead of
    // trusting a single-item id guess.
    const auto legacyByLuid = processor::ResolveFromTable(
        AdapterChoice(0x100, L"", {}), table);
    Check(legacyByLuid.fellBack && !legacyByLuid.fallbackReason.empty(),
          "a selection without a saved identity never binds by an id guess");

    // Same-model duplicates the identity cannot single out never bind: the
    // name is not a single-item guess, the run falls back with a reason.
    const auto narrowed = processor::ResolveFromTable(
        AdapterChoice(0xFFFFull, L"Split B", splitIdentity), table);
    Check(narrowed.fellBack && !narrowed.fallbackReason.empty(),
          "same-model duplicates never bind by a name guess");

    // Ambiguous identity without a unique match must not mis-bind.
    const auto ambiguous = processor::ResolveFromTable(
        AdapterChoice(0xFFFFull, L"Twin", twinIdentity), table);
    Check(ambiguous.fellBack && !ambiguous.fallbackReason.empty() &&
              (ambiguous.adapterOrdinal == 1),
          "an ambiguous identity falls back to the automatic pick with a reason");

    // A name alone never binds, even when unique.
    const auto byName = processor::ResolveFromTable(
        AdapterChoice(0x999, L"Solo", {}), table);
    Check(byName.fellBack && !byName.fallbackReason.empty(),
          "a name alone never binds without a saved identity");

    // Missing device keeps the reason public and runs on the fallback.
    const auto missing = processor::ResolveFromTable(
        AdapterChoice(0xDEAD, L"Unplugged GPU", {0xAAAA, 0xBBBB, 0xCCCC, 0xDD}),
        table);
    Check(missing.fellBack && !missing.fallbackReason.empty() &&
              missing.adapterOrdinal == 1,
          "a missing device falls back with a public reason");

    // CPU resolves without any device.
    const auto cpu = processor::ResolveFromTable(
        {processor::Mode::Cpu, 0, L"", {}}, table);
    Check(cpu.mode == processor::Mode::Cpu && cpu.adapterName == L"CPU" &&
              !cpu.fellBack,
          "CPU resolves to the software path");

    // An empty enumeration never claims a device.
    const auto empty = processor::ResolveFromTable(
        {processor::Mode::Automatic, 0, L"", {}}, {});
    Check(empty.fellBack && !empty.fallbackReason.empty(),
          "an empty adapter table reports a fallback");
    LogLine("PASS resolution: identity-authoritative matching and fallback reasons");
}

// ---- Vertices becomes skinning::Preference exactly once ------------------
void VerticesToSkinning(const std::wstring& path) {
    std::wstring error;
    processor::Configuration config;
    config.vertices.mode = processor::Mode::Cpu;
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save vertices=CPU");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize with vertices=CPU");
    Check(skinning::GetPreference().mode == skinning::Mode::Cpu,
          "vertices=CPU becomes the skinning start preference");

    // A stale/unknown adapter must NOT pass its saved LUID through: the run
    // falls back to automatic while Requested keeps the saved selection.
    config.vertices = AdapterChoice(0x1122334455667788ull, L"Gone GPU",
                                    {0x7777, 0x8888, 0x9999, 0x11});
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save vertices=a missing adapter");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize with a missing adapter");
    Check(processor::ActualVertices().fellBack &&
              !processor::ActualVertices().fallbackReason.empty(),
          "a missing vertices adapter reports the fallback reason");
    Check(skinning::GetPreference().mode == skinning::Mode::Automatic,
          "a missing vertices adapter falls back to the automatic strategy");
    Check(processor::Requested().vertices == config.vertices,
          "the fallback keeps the saved vertices selection in Requested");

    config.vertices.mode = processor::Mode::Automatic;
    config.vertices.adapterId = 0;
    config.vertices.adapterName.clear();
    config.vertices.identity = {};
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save vertices=automatic");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize with vertices=automatic");
    Check(skinning::GetPreference().mode == skinning::Mode::Automatic,
          "vertices=automatic keeps the native auto calibration strategy");
    Check(!processor::ActualVertices().fellBack,
          "automatic vertices claims no single device");
    LogLine("PASS vertices choice becomes skinning::Preference once at startup");
}

// ---- stale LUIDs rebind through the real enumeration --------------------
void ComputeRebind(const std::wstring& path) {
    // Copy the entry out: ResetForTesting clears the cached table.
    const std::vector<processor::AdapterEntry> table = processor::ComputeAdapters();
    if (table.empty()) {
        LogLine("SKIP compute rebind: no compute adapter enumerated");
        return;
    }
    const processor::AdapterEntry front = table.front();
    std::size_t twins = 0;
    for (const auto& entry : table)
        if (entry.identity == front.identity) ++twins;
    if (twins != 1) {
        LogLine("SKIP compute rebind: the first adapter identity is not unique");
        return;
    }
    std::wstring error;
    // Vertices: stale LUID, different name, correct identity -> rebind.
    processor::Configuration config;
    config.vertices = AdapterChoice(0xFFFFFFFFFFFFFFFFull, L"stale name",
                                    front.identity);
    config.mmv = AdapterChoice(0xFFFFFFFFFFFFFFFEull, L"stale mmv name",
                               front.identity);
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save stale identities");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize with stale LUIDs");
    Check(skinning::GetPreference().adapterId == front.adapterId,
          "a stale vertices LUID rebinds to the current device by identity");
    Check(processor::ActualVertices().adapterId == front.adapterId &&
              !processor::ActualVertices().fellBack,
          "actual vertices reports the rebound device");
    Check(processor::ResolvedMmv().adapterId == front.adapterId,
          "a stale MMV LUID rebinds to the current device by identity");
    Check(processor::Requested().vertices.adapterId == 0xFFFFFFFFFFFFFFFFull,
          "the rebound run keeps the stale selection in Requested");
    LogLine("PASS stale LUIDs rebind through the live enumeration");
}

// ---- MMV callback modes -------------------------------------------------
void MmvCallback(const std::wstring& path) {
    std::wstring error;
    processor::Configuration config;
    config.mmv.mode = processor::Mode::Cpu;
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save mmv=CPU");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize with mmv=CPU");
    const auto cpu = processor::ResolvedMmv();
    Check(cpu.mode == processor::Mode::Cpu && cpu.adapterId == 0,
          "mmv=CPU reports the software mode without a device id");

    config.mmv = AdapterChoice(0x4242424242424242ull, L"Gone MMV GPU",
                               {0x1111, 0x2222, 0x3333, 0x44});
    RestartFrom(path);
    Check(processor::SaveRequested(config, error), "save mmv=a missing adapter");
    RestartFrom(path);
    Check(processor::Initialize(error), "initialize with a missing MMV adapter");
    Check(processor::ActualMmv().fellBack &&
              !processor::ActualMmv().fallbackReason.empty(),
          "a missing MMV adapter reports the fallback reason");
    Check(processor::Requested().mmv == config.mmv,
          "the fallback keeps the saved MMV selection in Requested");
    LogLine("PASS resolved MMV callback (CPU, missing-adapter fallback)");
}

// ---- a damaged file warns and falls back per role -----------------------
void ParseWarnings(const std::wstring& path) {
    std::wstring error;
    RestartFrom(path);
    processor::Configuration config;
    config.renderer = AdapterChoice(0x7777ull, L"Kept GPU");
    config.vertices.mode = processor::Mode::Cpu;
    Check(processor::SaveRequested(config, error), "save before corrupting");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "rewrite the config by hand");
    const wchar_t* broken =
        L"\xFEFF[Renderer]\r\nMode=banana\r\nAdapterId=zzzz\r\nVendorId=xx\r\n"
        L"[Vertices]\r\nMode=tachyon\r\n[Mmv]\r\nMode=banana\r\n";
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(wcslen(broken) * sizeof(wchar_t));
    Check(WriteFile(file, broken, bytes, &written, nullptr) && written == bytes,
          "write the damaged content");
    CloseHandle(file);
    RestartFrom(path);
    Check(processor::Initialize(error), "a damaged file still initializes");
    Check(!error.empty(), "a damaged file reports parse warnings");
    Check(processor::Active().renderer.mode == processor::Mode::Automatic &&
              processor::Active().vertices.mode == processor::Mode::Automatic &&
              processor::Active().mmv.mode == processor::Mode::FollowRenderer,
          "damaged roles fall back to their documented defaults");
    LogLine("PASS damaged configuration warns and falls back per role");
}

// ---- real D3D9 enumeration, LUID cross-check, device result -------------
void RealDeviceResolution(const std::wstring& path) {
    RestartFrom(path);
    std::wstring error;
    Check(processor::Initialize(error), "initialize before device resolution");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "open the Direct3D9 enumeration");
    const unsigned first = processor::ResolveRenderer(
        d3d, processor::Active().renderer, error);
    Check(first < d3d->GetAdapterCount(), "resolved ordinal is a real adapter");
    Check(!processor::ActualRenderer().fellBack || !error.empty(),
          "a fallback always carries a reason");
    const unsigned again = processor::ResolveRenderer(
        d3d, AdapterChoice(0xDEADBEEFull, L"not present"), error);
    Check(again == first,
          "a device rebuild keeps this run's adapter (resolution is cached)");

    // The UI adapter list carries the full hardware identity.
    const auto& renderers = processor::RendererAdapters();
    Check(!renderers.empty(), "renderer adapters are published for the menu");
    bool identityPopulated = false;
    for (const auto& entry : renderers)
        if (entry.identity.vendorId != 0 || entry.identity.deviceId != 0)
            identityPopulated = true;
    Check(identityPopulated, "renderer adapters carry the hardware identity");
    const auto& compute = processor::ComputeAdapters();
    Check(!compute.empty(), "compute adapters are published for the menu");

    if (processor::ActualRenderer().adapterId != 0) {
        HMODULE oracleModule = nullptr;
        const Create9ExProc create9ex = SystemCreate9Ex(oracleModule);
        Check(create9ex != nullptr,
              "the system Direct3DCreate9Ex resolves from the system directory");
        IDirect3D9Ex* ex = nullptr;
        if (create9ex != nullptr &&
            SUCCEEDED(create9ex(D3D_SDK_VERSION, &ex)) && ex != nullptr) {
            LUID luid{};
            Check(SUCCEEDED(ex->GetAdapterLUID(first, &luid)),
                  "read the adapter LUID from the 9Ex oracle");
            Check(PackLuid(luid) == processor::ActualRenderer().adapterId,
                  "the resolved ordinal matches the verified LUID");
        }
        if (ex != nullptr) ex->Release();
        if (oracleModule != nullptr) FreeLibrary(oracleModule);
    }

    // REF success: the run is on a software rasterizer, FollowRenderer must
    // resolve MMV to the CPU mode.
    processor::RecordDeviceResult(true, true, std::wstring());
    Check(processor::ActualRenderer().softwareDevice,
          "a REF success is recorded as the software device");
    Check(processor::ResolvedMmv().mode == processor::Mode::Cpu,
          "follow-renderer MMV takes the CPU mode on a software rasterizer");
    // The software state must not keep claiming the original GPU, and a
    // pending UI save must not rewrite what this run resolved.
    processor::Configuration pending;
    pending.mmv = AdapterChoice(0x9999ull, L"pending only");
    std::wstring pendingError;
    Check(processor::SaveRequested(pending, pendingError),
          "a pending UI save succeeds without activating");
    Check(processor::ResolvedMmv().mode == processor::Mode::Cpu &&
              processor::ResolvedMmv().adapterId == 0,
          "a pending UI save never changes the resolved MMV follow");
    Check(processor::ActualRenderer().softwareDevice,
          "the software state survives a pending UI save");

    // Total failure: nothing is displayed as running.
    processor::RecordDeviceResult(false, false,
                                  L"CreateDevice failed on the resolved adapter");
    Check(processor::ActualRenderer().failed &&
              processor::ActualRenderer().error ==
                  L"CreateDevice failed on the resolved adapter",
          "a failed device creation is recorded with its error");
    Check(processor::ResolvedMmv().mode == processor::Mode::FollowRenderer,
          "follow-renderer stays unresolved when no renderer runs");

    d3d->Release();
    LogLine("PASS real D3D9 resolution: ordinal %u id=%016llx, REF/failure recording",
            first,
            static_cast<unsigned long long>(
                processor::ActualRenderer().adapterId));
}

// ---- default config without Initialize (production test drivers) --------
// Drivers may call InitD3D/ResolveRenderer directly without WinMain: with
// the default configuration and no Initialize the resolver must still work
// and must not read or write any config file.
void DefaultConfigSafety(const FileStamp& realBefore, const std::wstring& realConfig) {
    processor::ResetForTesting();
    std::wstring error;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "open the Direct3D9 enumeration (default config)");
    const unsigned ordinal = processor::ResolveRenderer(
        d3d, processor::Active().renderer, error);
    Check(ordinal < d3d->GetAdapterCount(),
          "ResolveRenderer works without Initialize under the default config");
    d3d->Release();
    const FileStamp realAfter = Stamp(realConfig);
    Check(realAfter.exists == realBefore.exists &&
              realAfter.size == realBefore.size &&
              realAfter.mtime == realBefore.mtime,
          "default-config resolution never touches the real LOCALAPPDATA settings");
    LogLine("PASS default-config ResolveRenderer without Initialize (ordinal %u)",
            ordinal);
}

}  // namespace

int main() {
    const std::wstring root = RepoRoot();
    const std::wstring artifacts = root + L"\\.local\\artifacts\\global-processor";
    EnsureDirectory(artifacts);
    const std::wstring logPath = artifacts + L"\\processor_settings_tests.log";
    g_log = _wfopen(logPath.c_str(), L"w");
    LogLine("processor_settings_tests start (root=%ls)", root.c_str());

    // The real per-user config must stay untouched for the whole run; every
    // config operation below uses an injected path under .local/artifacts.
    const std::wstring realConfig = processor::ConfigPath();
    const FileStamp realBefore = Stamp(realConfig);

    const std::wstring tmp = artifacts + L"\\tests-tmp";
    EnsureDirectory(tmp);
    const std::wstring configPath = tmp + L"\\processor.ini";
    DeleteFileW(configPath.c_str());

    DefaultConfigSafety(realBefore, realConfig);
    DefaultsAndConstraints();
    RoundTripAndRestart(configPath);
    FreezeAfterInitialize(configPath);
    SaveFailuresKeepEverything(tmp);
    ResolutionTables();
    VerticesToSkinning(configPath);
    ComputeRebind(configPath);
    MmvCallback(configPath);
    ParseWarnings(configPath);
    RealDeviceResolution(configPath);

    processor::ResetForTesting();
    DeleteFileW(configPath.c_str());
    const FileStamp realAfter = Stamp(realConfig);
    Check(realAfter.exists == realBefore.exists &&
              realAfter.size == realBefore.size &&
              realAfter.mtime == realBefore.mtime,
          "the whole run never touches the real LOCALAPPDATA processor settings");
    LogLine("PASS %u checks: global processor policy module", g_checks);
    if (g_log != nullptr) { std::fclose(g_log); g_log = nullptr; }
    return 0;
}
