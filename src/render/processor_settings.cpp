// ===========================================================================
// processor_settings.cpp - global processor policy (host side)
//
// See include/mikumikustudio/processor_settings.hpp for the contract.  This
// file owns: the per-installation config file (data/processor.ini,
// atomically replaced), the Requested/Active split with a
// single startup freeze, adapter resolution by LUID then full hardware
// identity (vendor/device/subsystem/revision) then unique name, and the
// one-shot conversion of the resolved Vertices choice into
// skinning::Preference (the native vertex auto strategy keeps its own
// startup correctness/timing calibration).
//
// The legacy data\skinning.ini is deliberately neither read nor migrated: an
// old per-model skinning intent must never silently rewrite global settings.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/local_data_path.hpp"
#include <Windows.h>
#include <d3d9.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/graphics_runtime.hpp"

namespace mikumikustudio {
namespace processor {
namespace {

using Microsoft::WRL::ComPtr;

Configuration requested;   // mmv defaults to FollowRenderer
Configuration active;
bool frozen = false;
ResolvedInfo actualRenderer;
ResolvedInfo actualVertices;
ResolvedInfo actualMmv;
bool rendererResolved = false;
std::wstring configOverride;
std::vector<AdapterEntry> rendererAdapters;
std::vector<AdapterEntry> computeAdapters;
bool computeEnumerated = false;

const wchar_t* kMissingValue = L"\x01missing";

const wchar_t* ModeToken(Mode mode) {
    switch (mode) {
    case Mode::Adapter: return L"adapter";
    case Mode::Cpu: return L"cpu";
    case Mode::FollowRenderer: return L"follow";
    default: return L"auto";
    }
}
const char* ModeNameA(Mode mode) {
    switch (mode) {
    case Mode::Adapter: return "adapter";
    case Mode::Cpu: return "cpu";
    case Mode::FollowRenderer: return "follow";
    default: return "auto";
    }
}
bool ParseMode(const wchar_t* token, Mode& mode) {
    if (token == nullptr) return false;
    if (_wcsicmp(token, L"auto") == 0) { mode = Mode::Automatic; return true; }
    if (_wcsicmp(token, L"adapter") == 0) { mode = Mode::Adapter; return true; }
    if (_wcsicmp(token, L"cpu") == 0) { mode = Mode::Cpu; return true; }
    if (_wcsicmp(token, L"follow") == 0) { mode = Mode::FollowRenderer; return true; }
    return false;
}

std::uint64_t PackLuid(const LUID& luid) {
    return (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart)) << 32) |
           static_cast<std::uint32_t>(luid.LowPart);
}
std::wstring Widen(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int needed = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring wide(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], needed);
    return wide;
}
std::wstring HexId(std::uint64_t id) {
    wchar_t buffer[17]{};
    swprintf_s(buffer, L"%016llx", static_cast<unsigned long long>(id));
    return buffer;
}
std::wstring Hex32(std::uint32_t value) {
    wchar_t buffer[9]{};
    swprintf_s(buffer, L"%08x", value);
    return buffer;
}
bool ParseHexId(const wchar_t* text, std::uint64_t& id) {
    if (text == nullptr) return false;
    const std::size_t length = wcslen(text);
    if (length != 16) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const wchar_t c = text[i];
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                         (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    id = std::wcstoull(text, nullptr, 16);
    return true;
}
bool ParseHex32(const wchar_t* text, std::uint32_t& value) {
    if (text == nullptr) return false;
    const std::size_t length = wcslen(text);
    if (length != 8) return false;
    for (std::size_t i = 0; i < length; ++i) {
        const wchar_t c = text[i];
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                         (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    value = static_cast<std::uint32_t>(std::wcstoul(text, nullptr, 16));
    return true;
}
std::wstring SingleLine(std::wstring text) {
    for (wchar_t& c : text)
        if (c == L'\r' || c == L'\n') c = L' ';
    return text;
}
bool HasIdentity(const DeviceIdentity& identity) {
    return identity.vendorId != 0 || identity.deviceId != 0 ||
           identity.subSysId != 0 || identity.revision != 0;
}

const wchar_t* SectionName(Role role) {
    switch (role) {
    case Role::Vertices: return L"Vertices";
    case Role::Mmv: return L"Mmv";
    default: return L"Renderer";
    }
}
void AppendNote(std::wstring& target, const std::wstring& note) {
    if (note.empty()) return;
    if (!target.empty()) target += L"; ";
    target += note;
}

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------
std::wstring ReadField(const wchar_t* path, const wchar_t* section,
                       const wchar_t* key) {
    wchar_t buffer[512]{};
    GetPrivateProfileStringW(section, key, kMissingValue, buffer, 512, path);
    if (wcscmp(buffer, kMissingValue) == 0) return {};
    return buffer;
}

Selection DefaultSelection(Role role) {
    Selection selection;
    if (role == Role::Mmv) selection.mode = Mode::FollowRenderer;
    return selection;
}

Selection ReadSelection(const wchar_t* path, Role role, std::wstring& warning) {
    const wchar_t* section = SectionName(role);
    Selection selection = DefaultSelection(role);

    const std::wstring modeText = ReadField(path, section, L"Mode");
    Mode parsed = Mode::Automatic;
    if (!modeText.empty() && !ParseMode(modeText.c_str(), parsed)) {
        AppendNote(warning, std::wstring(section) + L": unknown mode '" +
                                modeText + L"' (kept default)");
    } else if (!modeText.empty()) {
        selection.mode = parsed;
    }

    const std::wstring idText = ReadField(path, section, L"AdapterId");
    if (!idText.empty() && !ParseHexId(idText.c_str(), selection.adapterId)) {
        AppendNote(warning, std::wstring(section) + L": invalid AdapterId '" +
                                idText + L"' (kept default)");
        selection.adapterId = 0;
    }
    selection.adapterName = ReadField(path, section, L"AdapterName");
    const std::pair<const wchar_t*, std::uint32_t*> identityFields[] = {
        {L"VendorId", &selection.identity.vendorId},
        {L"DeviceId", &selection.identity.deviceId},
        {L"SubSysId", &selection.identity.subSysId},
        {L"Revision", &selection.identity.revision},
    };
    for (const auto& field : identityFields) {
        const std::wstring text = ReadField(path, section, field.first);
        if (!text.empty() && !ParseHex32(text.c_str(), *field.second)) {
            AppendNote(warning, std::wstring(section) + L": invalid " +
                                    field.first + L" (kept 0)");
            *field.second = 0;
        }
    }

    std::wstring reason;
    if (!IsValid(role, selection, &reason)) {
        AppendNote(warning, std::wstring(section) + L": " + reason + L" (kept default)");
        return DefaultSelection(role);
    }
    return selection;
}

Configuration ReadConfiguration(const wchar_t* path, std::wstring& warning) {
    Configuration configuration;
    configuration.renderer = ReadSelection(path, Role::Renderer, warning);
    configuration.vertices = ReadSelection(path, Role::Vertices, warning);
    configuration.mmv = ReadSelection(path, Role::Mmv, warning);
    return configuration;
}

std::wstring Serialize(const Configuration& configuration) {
    std::wstring text;
    text += L"; MikuMikuStudio processor settings (this Windows user, all projects).\r\n";
    text += L"; Changes take effect on the next launch.\r\n";
    const auto emit = [&](Role role, const Selection& selection) {
        text += L"["; text += SectionName(role); text += L"]\r\n";
        text += L"Mode="; text += ModeToken(selection.mode); text += L"\r\n";
        text += L"AdapterId="; text += HexId(selection.adapterId); text += L"\r\n";
        text += L"AdapterName="; text += SingleLine(selection.adapterName); text += L"\r\n";
        text += L"VendorId="; text += Hex32(selection.identity.vendorId); text += L"\r\n";
        text += L"DeviceId="; text += Hex32(selection.identity.deviceId); text += L"\r\n";
        text += L"SubSysId="; text += Hex32(selection.identity.subSysId); text += L"\r\n";
        text += L"Revision="; text += Hex32(selection.identity.revision); text += L"\r\n";
    };
    emit(Role::Renderer, configuration.renderer);
    emit(Role::Vertices, configuration.vertices);
    emit(Role::Mmv, configuration.mmv);
    return text;
}

bool EnsureDirectory(const std::wstring& directory) {
    if (directory.empty()) return true;
    const DWORD attribute = GetFileAttributesW(directory.c_str());
    if (attribute != INVALID_FILE_ATTRIBUTES)
        return (attribute & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const auto slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0)
        if (!EnsureDirectory(directory.substr(0, slash))) return false;
    return CreateDirectoryW(directory.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// Atomic replace: the caller's old file survives any failure untouched.
bool WriteAtomic(const std::wstring& path, const std::wstring& text,
                 std::wstring& error) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !EnsureDirectory(path.substr(0, slash))) {
        error = L"cannot create the configuration directory (error " +
                std::to_wstring(GetLastError()) + L")";
        return false;
    }
    const std::wstring temporary =
        path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot write the configuration file (error " +
                std::to_wstring(GetLastError()) + L")";
        return false;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof bom, &written, nullptr) &&
              written == sizeof bom;
    if (ok && !text.empty()) {
        const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
        ok = WriteFile(file, text.c_str(), bytes, &written, nullptr) &&
             written == bytes;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok) {
        error = L"cannot write the configuration file (error " +
                std::to_wstring(GetLastError()) + L")";
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"cannot replace the configuration file (error " +
                std::to_wstring(GetLastError()) + L")";
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Adapter tables
// ---------------------------------------------------------------------------
std::unordered_map<std::uint64_t, unsigned> DxgiPerformanceRanks() {
    std::unordered_map<std::uint64_t, unsigned> ranks;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(graphics_runtime::CreateFactory1(IID_PPV_ARGS(&factory)))) return ranks;
    ComPtr<IDXGIFactory6> factory6;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) factory6.Reset();
    unsigned rank = 0;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory6
            ? factory6->EnumAdapterByGpuPreference(
                  index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                  IID_PPV_ARGS(&adapter))
            : factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ranks.emplace(PackLuid(description.AdapterLuid), rank++);
    }
    return ranks;
}

bool SameIdentifier(const D3DADAPTER_IDENTIFIER9& a,
                    const D3DADAPTER_IDENTIFIER9& b) {
    return a.VendorId == b.VendorId && a.DeviceId == b.DeviceId &&
           a.SubSysId == b.SubSysId && a.Revision == b.Revision &&
           std::memcmp(&a.DeviceIdentifier, &b.DeviceIdentifier,
                       sizeof(a.DeviceIdentifier)) == 0 &&
           std::strcmp(a.Description, b.Description) == 0;
}

// The LUID oracle is the system runtime's Direct3DCreate9Ex, resolved at run
// time from <GetSystemDirectoryW()>\d3d9.dll by absolute path.  It is
// deliberately neither a static import nor a basename load: the MME
// deployment ships a same-named d3d9.dll proxy in the application directory
// and the loader's basename short-circuit would hand that proxy's exports to
// a pathless call.  The loaded module is verified with GetModuleFileNameW +
// CompareStringOrdinal before any export is used.  The caller must release
// the created IDirect3D9Ex and then FreeLibrary the returned module; on any
// failure this returns nullptr with module == nullptr (nothing leaked) and
// the adapter LUIDs stay unknown.  (Loader probe conclusion: safety comes
// from the absolute path itself; search flags do not apply to an absolute
// path.  See .local/artifacts/global-processor/.)
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
        loaded == GetModuleHandleW(nullptr) ||
        CompareStringOrdinal(resolved, -1, expected.c_str(), -1, TRUE) !=
            CSTR_EQUAL) {
        runtime_log::Write(
            "PROCESSOR: system d3d9.dll verification failed; adapter LUIDs unknown");
        FreeLibrary(loaded);
        return nullptr;
    }
    const auto proc = reinterpret_cast<Create9ExProc>(
        GetProcAddress(loaded, "Direct3DCreate9Ex"));
    if (proc == nullptr) {
        FreeLibrary(loaded);
        return nullptr;
    }
    module = loaded;
    return proc;
}

// D3D9 ordinals with their 9Ex LUIDs.  IDirect3D9Ex is used only as the LUID
// oracle; a LUID is trusted only after both interfaces report the same full
// identity for that ordinal (no same-ordinal assumption).  The main device
// stays on the Direct3DCreate9 path.
std::vector<AdapterEntry> EnumerateD3d9(IDirect3D9* d3d) {
    std::vector<AdapterEntry> table;
    if (d3d == nullptr) return table;
    ComPtr<IDirect3D9Ex> ex;
    HMODULE oracleModule = nullptr;
    const Create9ExProc create9ex = SystemCreate9Ex(oracleModule);
    if (create9ex == nullptr ||
        FAILED(create9ex(D3D_SDK_VERSION, ex.GetAddressOf()))) {
        runtime_log::Write("PROCESSOR: Direct3DCreate9Ex unavailable; adapter LUIDs unknown");
        ex.Reset();
    }
    const auto ranks = DxgiPerformanceRanks();
    const UINT count = d3d->GetAdapterCount();
    for (UINT ordinal = 0; ordinal < count; ++ordinal) {
        D3DADAPTER_IDENTIFIER9 identifier{};
        if (FAILED(d3d->GetAdapterIdentifier(ordinal, 0, &identifier))) continue;
        AdapterEntry entry;
        entry.ordinal = ordinal;
        entry.adapterName = Widen(identifier.Description);
        entry.identity = {identifier.VendorId, identifier.DeviceId,
                          identifier.SubSysId, identifier.Revision};
        if (ex) {
            D3DADAPTER_IDENTIFIER9 twin{};
            if (SUCCEEDED(ex->GetAdapterIdentifier(ordinal, 0, &twin)) &&
                SameIdentifier(identifier, twin)) {
                LUID luid{};
                if (SUCCEEDED(ex->GetAdapterLUID(ordinal, &luid)))
                    entry.adapterId = PackLuid(luid);
            } else {
                runtime_log::Write(
                    "PROCESSOR: adapter %u identity mismatch between D3D9 and 9Ex; LUID not trusted",
                    ordinal);
            }
        }
        const auto found = ranks.find(entry.adapterId);
        if (entry.adapterId != 0 && found != ranks.end())
            entry.performanceRank = found->second;
        table.push_back(std::move(entry));
    }
    // Release the COM oracle first, then free its module; the adapter LUIDs
    // stay unknown when either step never happened.
    ex.Reset();
    if (oracleModule != nullptr) FreeLibrary(oracleModule);
    return table;
}

// DXGI adapters for the compute roles (Vertices, Mmv): LUID identity, full
// hardware identity, high-performance order.  Software/remote adapters are
// excluded like the skinning backend does.
std::vector<AdapterEntry> EnumerateDxgi() {
    std::vector<AdapterEntry> table;
#if defined(MMD_ENABLE_DX12) && !MMD_ENABLE_DX12
    return table;
#else
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(graphics_runtime::CreateFactory1(IID_PPV_ARGS(&factory)))) return table;
    ComPtr<IDXGIFactory6> factory6;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) factory6.Reset();
    unsigned rank = 0;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory6
            ? factory6->EnumAdapterByGpuPreference(
                  index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                  IID_PPV_ARGS(&adapter))
            : factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if (description.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE))
            continue;
        AdapterEntry entry;
        entry.ordinal = rank;
        entry.adapterId = PackLuid(description.AdapterLuid);
        entry.adapterName = description.Description;
        entry.identity = {description.VendorId, description.DeviceId,
                          description.SubSysId, description.Revision};
        entry.performanceRank = rank++;
        table.push_back(std::move(entry));
    }
    return table;
#endif
}

const AdapterEntry* AutomaticPick(const std::vector<AdapterEntry>& adapters) {
    const AdapterEntry* best = nullptr;
    for (const auto& entry : adapters) {
        if (best == nullptr ||
            entry.performanceRank < best->performanceRank ||
            (entry.performanceRank == best->performanceRank &&
             entry.ordinal < best->ordinal))
            best = &entry;
    }
    return best;
}

ResolvedInfo FromEntry(const AdapterEntry& entry) {
    ResolvedInfo info;
    info.mode = Mode::Adapter;
    info.adapterOrdinal = entry.ordinal;
    info.adapterId = entry.adapterId;
    info.adapterName = entry.adapterName;
    info.identity = entry.identity;
    return info;
}

ResolvedInfo CpuInfo() {
    ResolvedInfo info;
    info.mode = Mode::Cpu;
    info.adapterName = L"CPU";
    return info;
}

// The Vertices/Mmv strategy keeps the requested mode semantics: Automatic
// never claims a single device (the startup vertex calibration and the MMV side
// decide), Adapter resolves to a concrete current device.  A missing or
// ambiguous Adapter request runs the role's own automatic behaviour instead
// of pinning a substitute device: the reason stays public and Requested keeps
// the saved selection.
ResolvedInfo ResolveComputeRole(const Selection& wanted,
                                const std::vector<AdapterEntry>& table) {
    if (wanted.mode == Mode::Cpu) return CpuInfo();
    if (wanted.mode == Mode::Automatic) {
        ResolvedInfo info;
        info.mode = Mode::Automatic;
        return info;
    }
    ResolvedInfo resolved = ResolveFromTable(wanted, table);
    if (resolved.fellBack) {
        ResolvedInfo info;
        info.mode = Mode::Automatic;
        info.fellBack = true;
        info.fallbackReason = resolved.fallbackReason;
        return info;
    }
    return resolved;
}

skinning::Preference PreferenceFrom(const Selection& resolved) {
    skinning::Preference preference;
    switch (resolved.mode) {
    case Mode::Cpu:
        preference.mode = skinning::Mode::Cpu;
        break;
    case Mode::Adapter:
        preference.mode = skinning::Mode::Adapter;
        preference.adapterId = resolved.adapterId;
        preference.adapterName = resolved.adapterName;
        break;
    default:
        // Automatic selects a startup measurement by model format and size.
        // Project loading never performs another correctness/timing run.
        break;
    }
    return preference;
}

Selection SelectionFrom(const ResolvedInfo& info) {
    Selection selection;
    selection.mode = info.mode;
    selection.adapterId = info.adapterId;
    selection.adapterName = info.adapterName;
    selection.identity = info.identity;
    return selection;
}

}  // namespace

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
bool IsValid(Role role, const Selection& selection, std::wstring* reason) {
    const auto reject = [&](const wchar_t* text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (reason != nullptr) reason->clear();
    switch (selection.mode) {
    case Mode::Automatic:
        return true;
    case Mode::Adapter:
        if (selection.adapterId == 0)
            return reject(L"Adapter selection requires a non-zero device id");
        return true;
    case Mode::Cpu:
        if (role == Role::Renderer)
            return reject(L"the renderer role has no CPU (software) option");
        return true;
    case Mode::FollowRenderer:
        if (role != Role::Mmv)
            return reject(L"only the MMV role may follow the renderer");
        return true;
    default:
        return reject(L"unknown mode value");
    }
}

bool IsValid(const Configuration& configuration, std::wstring* reason) {
    std::wstring local;
    if (!IsValid(Role::Renderer, configuration.renderer, &local)) {
        if (reason != nullptr) *reason = L"Renderer: " + local;
        return false;
    }
    if (!IsValid(Role::Vertices, configuration.vertices, &local)) {
        if (reason != nullptr) *reason = L"Vertices: " + local;
        return false;
    }
    if (!IsValid(Role::Mmv, configuration.mmv, &local)) {
        if (reason != nullptr) *reason = L"Mmv: " + local;
        return false;
    }
    if (reason != nullptr) reason->clear();
    return true;
}

// ---------------------------------------------------------------------------
// Requested / Active / persistence
// ---------------------------------------------------------------------------
const Configuration& Requested() { return requested; }
const Configuration& Active() { return active; }

std::wstring ConfigPath() {
    if (!configOverride.empty()) return configOverride;
    return local_data::File(L"processor.ini").wstring();
}

bool SaveRequested(const Configuration& configuration, std::wstring& error) {
    error.clear();
    std::wstring reason;
    if (!IsValid(configuration, &reason)) {
        error = L"invalid configuration: " + reason;
        return false;
    }
    const std::wstring path = ConfigPath();
    if (path.empty()) {
        error = L"processor.ini path unavailable (executable directory unavailable)";
        return false;
    }
    if (!WriteAtomic(path, Serialize(configuration), error)) {
        // The old file and the previous Requested value are both kept.
        runtime_log::Write("PROCESSOR settings save failed (kept previous file)");
        return false;
    }
    requested = configuration;
    runtime_log::Write("PROCESSOR settings saved (next launch): renderer=%s vertices=%s mmv=%s",
                       ModeNameA(configuration.renderer.mode),
                       ModeNameA(configuration.vertices.mode),
                       ModeNameA(configuration.mmv.mode));
    return true;
}

bool Initialize(std::wstring& error) {
    error.clear();
    if (frozen) return true;  // later modifications only wait for next launch
    Configuration loaded;
    const std::wstring path = ConfigPath();
    if (path.empty()) {
        error = L"processor.ini path unavailable (executable directory unavailable)";
        requested = loaded;
        active = loaded;
        frozen = true;
        return false;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        loaded = ReadConfiguration(path.c_str(), error);
    requested = loaded;
    active = loaded;
    frozen = true;

    // One re-enumeration for the compute roles: a stale saved LUID rebinds by
    // a unique full-identity match, an ambiguous or missing device falls back
    // with a reason while Requested keeps the saved intent.
    computeAdapters = EnumerateDxgi();
    computeEnumerated = true;
    actualVertices = ResolveComputeRole(active.vertices, computeAdapters);
    skinning::SetPreference(PreferenceFrom(SelectionFrom(actualVertices)));
    if (active.mmv.mode == Mode::FollowRenderer) {
        // Expanded later against the real renderer device (ResolvedMmv).
        actualMmv = ResolvedInfo{};
        actualMmv.mode = Mode::FollowRenderer;
    } else {
        actualMmv = ResolveComputeRole(active.mmv, computeAdapters);
    }
    runtime_log::Write("PROCESSOR settings active: renderer=%s vertices=%s mmv=%s",
                       ModeNameA(active.renderer.mode),
                       ModeNameA(active.vertices.mode),
                       ModeNameA(active.mmv.mode));
    if (actualVertices.fellBack)
        runtime_log::Write("PROCESSOR vertices fallback: %ls",
                           actualVertices.fallbackReason.c_str());
    if (actualMmv.fellBack)
        runtime_log::Write("PROCESSOR mmv fallback: %ls",
                           actualMmv.fallbackReason.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------
ResolvedInfo ResolveFromTable(const Selection& choice,
                              const std::vector<AdapterEntry>& adapters) {
    if (choice.mode == Mode::Cpu) return CpuInfo();
    const AdapterEntry* automatic = AutomaticPick(adapters);
    const auto fallback = [&](const std::wstring& reason) {
        ResolvedInfo info;
        info.fellBack = true;
        if (automatic != nullptr) {
            info = FromEntry(*automatic);
            info.fellBack = true;
            info.fallbackReason = reason + L"; ran on " + info.adapterName;
        } else {
            info.fallbackReason = reason + L"; no adapter is available";
        }
        return info;
    };
    if (choice.mode == Mode::Automatic) {
        if (automatic != nullptr) return FromEntry(*automatic);
        ResolvedInfo info;
        info.fellBack = true;
        info.fallbackReason = L"no adapter could be enumerated";
        return info;
    }
    // Mode::Adapter.  Binding a concrete adapter REQUIRES the saved full
    // hardware identity and that identity is authoritative: a LUID hit is
    // verified inside the identity set (an old LUID reused by a different
    // card never steals the binding), and after a LUID change only a unique
    // full-identity match binds.  Same-model duplicates the identity cannot
    // single out fall back, and a selection without a saved identity falls
    // back too - the name and the id are never used as single-item guesses.
    // The saved selection is never rewritten; a miss is reported with its
    // reason.
    if (!HasIdentity(choice.identity))
        return fallback(L"the saved device carries no hardware identity");
    std::vector<const AdapterEntry*> candidates;
    for (const auto& entry : adapters)
        if (HasIdentity(entry.identity) && entry.identity == choice.identity)
            candidates.push_back(&entry);
    if (candidates.empty())
        return fallback(L"the saved device identity is not present on this machine");
    if (choice.adapterId != 0) {
        for (const auto* entry : candidates)
            if (entry->adapterId == choice.adapterId) return FromEntry(*entry);
    }
    if (candidates.size() == 1) return FromEntry(*candidates.front());
    return fallback(L"the saved device identity matches " +
                    std::to_wstring(candidates.size()) + L" adapters");
}

unsigned ResolveRenderer(IDirect3D9* d3d, const Selection& choice,
                         std::wstring& error) {
    // One decision per run: a device rebuild keeps this run's adapter.
    if (rendererResolved) {
        error = actualRenderer.fallbackReason;
        return actualRenderer.adapterOrdinal;
    }
    error.clear();
    if (d3d == nullptr) {
        actualRenderer = ResolvedInfo{};
        actualRenderer.fellBack = true;
        actualRenderer.fallbackReason = L"no Direct3D9 interface; using adapter 0";
        rendererResolved = true;
        error = actualRenderer.fallbackReason;
        return 0;
    }
    rendererAdapters = EnumerateD3d9(d3d);
    actualRenderer = ResolveFromTable(choice, rendererAdapters);
    rendererResolved = true;
    error = actualRenderer.fallbackReason;
    runtime_log::Write(
        "PROCESSOR renderer device: ordinal=%u id=%016llx fellBack=%d",
        actualRenderer.adapterOrdinal,
        static_cast<unsigned long long>(actualRenderer.adapterId),
        actualRenderer.fellBack ? 1 : 0);
    return actualRenderer.adapterOrdinal;
}

const ResolvedInfo& ActualRenderer() { return actualRenderer; }
const ResolvedInfo& ActualVertices() { return actualVertices; }
const ResolvedInfo& ActualMmv() { return actualMmv; }

Selection ResolvedMmv() {
    const Selection& wanted = active.mmv;
    if (wanted.mode != Mode::FollowRenderer) return SelectionFrom(actualMmv);
    if (!rendererResolved) {
        // The real renderer device does not exist yet; the host asks again
        // after the main device is created.
        Selection pending;
        pending.mode = Mode::FollowRenderer;
        return pending;
    }
    if (actualRenderer.failed) {
        // No renderer runs; nothing to follow and nothing to display as
        // running.
        Selection pending;
        pending.mode = Mode::FollowRenderer;
        return pending;
    }
    if (actualRenderer.softwareDevice) {
        // The renderer runs on a REF software rasterizer: FollowRenderer must
        // take the software MMV backend.
        Selection cpu;
        cpu.mode = Mode::Cpu;
        return cpu;
    }
    return SelectionFrom(actualRenderer);
}

const std::vector<AdapterEntry>& RendererAdapters() { return rendererAdapters; }
const std::vector<AdapterEntry>& ComputeAdapters() {
    if (!computeEnumerated) {
        computeAdapters = EnumerateDxgi();
        computeEnumerated = true;
    }
    return computeAdapters;
}

void RecordDeviceResult(bool success, bool softwareRasterizer,
                        const std::wstring& error) {
    actualRenderer.failed = !success;
    actualRenderer.error = error;
    actualRenderer.softwareDevice = success && softwareRasterizer;
    runtime_log::Write("PROCESSOR renderer device result: success=%d software=%d",
                       success ? 1 : 0, actualRenderer.softwareDevice ? 1 : 0);
    if (!success && !error.empty())
        runtime_log::Write("PROCESSOR renderer device error: %ls", error.c_str());
}

// ---------------------------------------------------------------------------
// Test seams
// ---------------------------------------------------------------------------
void SetConfigPathOverrideForTesting(const wchar_t* path) {
    configOverride = path != nullptr ? path : L"";
}
void ResetForTesting() {
    requested = Configuration{};
    active = Configuration{};
    frozen = false;
    actualRenderer = ResolvedInfo{};
    actualVertices = ResolvedInfo{};
    actualMmv = ResolvedInfo{};
    rendererResolved = false;
    configOverride.clear();
    rendererAdapters.clear();
    computeAdapters.clear();
    computeEnumerated = false;
}

}  // namespace processor
}  // namespace mikumikustudio
