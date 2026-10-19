// ===========================================================================
// processor_settings.hpp - global processor policy (host side)
//
// One menu entry governs three roles (docs/records/performance/devices.md):
//   Renderer  主渲染设备 - main D3D9 device, shadows, edges, transparency,
//             final MME output.       Options: Automatic, Adapter.
//   Vertices  顶点计算设备 - native model vertex transform back end.
//             Options: Automatic, Adapter, Cpu.
//   Mmv       MMV 处理设备 - MMV compute shaders, mesh passes, textures and
//             intermediate drawing.   Options: FollowRenderer, Automatic,
//             Adapter, Cpu.  Default: FollowRenderer.
//
// Scope: per installation directory, shared by every project in that installation
// (data/processor.ini).  PMM/VMM/VEM never store
// these choices, and opening, creating or saving a project never changes
// them.
//
// Effect rule: Requested is what the user picked; Active is what this run
// uses.  Initialize freezes Active exactly once at startup; every later
// modification only updates Requested (and the file) and takes effect on the
// next launch.  SaveRequested never activates anything.
//
// Device identity: Selection::adapterId is a DXGI LUID packed as
//   (uint64_t(uint32_t(luid.HighPart)) << 32) | uint32_t(luid.LowPart)
// - the same packing skinning::Preference uses.  It is a per-boot identity,
// not a cross-restart one.  Selection::identity carries the full hardware
// identity (D3DADAPTER_IDENTIFIER9 vendor/device/subsystem/revision), so a
// stale LUID is rebound at startup by a unique full-identity match; an
// ambiguous or missing device falls back to the automatic choice with
// ResolvedInfo::fallbackReason reported while Requested keeps the user's
// saved selection.  Vertices and explicit Mmv GPU selections resolve through
// the same re-enumeration; nothing passes a saved LUID straight through.
//
// The main D3D9 device is still created through the original Direct3DCreate9
// path so MME keeps observing its D3DX imports; only the adapter ordinal is
// chosen here.  IDirect3D9Ex is used solely as the LUID oracle, and a 9Ex
// LUID is trusted only after the two interfaces report the same full
// identity for that ordinal.
//
// InitD3D reports what it actually created through RecordDeviceResult: a REF
// (software rasterizer) success is reported as the CPU mode so a
// FollowRenderer MMV picks its software backend, and a total CreateDevice
// failure marks the renderer as failed so no preselected GPU is displayed as
// running.
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct IDirect3D9;

namespace mikumikustudio {
namespace processor {

enum class Role : std::uint8_t { Renderer = 0, Vertices = 1, Mmv = 2 };

enum class Mode : std::uint8_t {
    Automatic = 0,
    Adapter = 1,
    Cpu = 2,
    FollowRenderer = 3,
};

// The D3DADAPTER_IDENTIFIER9 hardware identity.  Cross-restart matching uses
// these fields when the saved LUID no longer exists.
struct DeviceIdentity {
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t subSysId = 0;
    std::uint32_t revision = 0;
    bool operator==(const DeviceIdentity& other) const {
        return vendorId == other.vendorId && deviceId == other.deviceId &&
               subSysId == other.subSysId && revision == other.revision;
    }
    bool operator!=(const DeviceIdentity& other) const {
        return !(*this == other);
    }
};

struct Selection {
    Mode mode = Mode::Automatic;
    std::uint64_t adapterId = 0;   // packed LUID; 0 with Adapter is invalid
    std::wstring adapterName;
    DeviceIdentity identity;
    bool operator==(const Selection& other) const {
        return mode == other.mode && adapterId == other.adapterId &&
               adapterName == other.adapterName && identity == other.identity;
    }
    bool operator!=(const Selection& other) const { return !(*this == other); }
};

struct Configuration {
    Selection renderer;
    Selection vertices;
    // MMV defaults to FollowRenderer (contract default, also in ResetForTesting).
    Selection mmv{Mode::FollowRenderer, 0, std::wstring(), DeviceIdentity{}};
};

// What a role actually runs this run: the device identity plus, when the
// saved request could not be honoured, the reason the run fell back.
struct ResolvedInfo {
    Mode mode = Mode::Automatic;
    unsigned adapterOrdinal = 0;
    std::uint64_t adapterId = 0;
    std::wstring adapterName;
    DeviceIdentity identity;
    bool fellBack = false;
    std::wstring fallbackReason;   // empty when the request was honoured
    // Post-creation facts recorded by InitD3D (renderer only):
    bool failed = false;           // no device could be created at all
    std::wstring error;            // creation failure detail
    bool softwareDevice = false;   // REF / software rasterizer actually runs
};

// One enumerated adapter.  performanceRank: 0 = best;
// kUnknownPerformanceRank when no ranking source (DXGI high-performance
// order) knows this adapter.
struct AdapterEntry {
    static constexpr unsigned kUnknownPerformanceRank = 0xFFFFFFFFu;
    unsigned ordinal = 0;
    std::uint64_t adapterId = 0;
    std::wstring adapterName;
    DeviceIdentity identity;
    unsigned performanceRank = kUnknownPerformanceRank;
};

// Role constraints: Renderer forbids Cpu and FollowRenderer; Vertices forbids
// FollowRenderer; Mmv accepts all four modes.  Adapter requires a non-zero
// adapterId.  `reason` receives a human-readable explanation on rejection.
bool IsValid(Role role, const Selection& selection, std::wstring* reason = nullptr);
bool IsValid(const Configuration& configuration, std::wstring* reason = nullptr);

// Current requested (user-chosen, saved) and active (frozen for this run)
// configurations.  Before Initialize both are the defaults: Renderer and
// Vertices Automatic, Mmv FollowRenderer.
const Configuration& Requested();
const Configuration& Active();

// Atomically persists `configuration` and updates Requested.  On any failure
// (validation, directory, write, replace) the old file stays untouched,
// Requested keeps its previous value, the call returns false and `error`
// explains it.  Never touches Active; the change starts on the next launch.
bool SaveRequested(const Configuration& configuration, std::wstring& error);

// Startup: read the config file, freeze Active exactly once, re-enumerate the
// adapters once and resolve the Vertices and explicit Mmv selections against
// that enumeration (stale LUIDs rebind by a unique full-identity match; a
// missing or ambiguous device runs the role's own automatic behaviour with
// fellBack and the reason reported while Requested keeps the saved intent).
// Active.vertices is converted into skinning::Preference exactly once here
// (the native vertex auto strategy keeps its own correctness/timing
// calibration and an Automatic Vertices choice stays Automatic).  Later calls
// keep the frozen Active.  Returns false only when no configuration path is
// resolvable (defaults are still frozen); `error` also receives parse
// warnings for the menu to show.
bool Initialize(std::wstring& error);

// The config file in use: data/processor.ini, or a
// test override.  Empty when the path cannot be resolved.
std::wstring ConfigPath();

// Resolves `choice` to a D3D9 adapter ordinal for the main device.  The first
// call of a run decides and is cached; later calls (device rebuild) return
// the same ordinal so this run's choice survives.  Ordinals are verified
// against IDirect3D9Ex::GetAdapterLUID only after both interfaces report the
// same full identity for that ordinal.  `error` receives the fallback reason
// when the saved request could not be honoured.
unsigned ResolveRenderer(IDirect3D9* d3d, const Selection& choice,
                         std::wstring& error);

// Actual identity and error for the menu: what each role really runs on and
// why it is not what was requested.  ActualRenderer().failed means no device
// could be created - the menu must show the error, never a preselected GPU as
// running.  Empty entries until the corresponding resolution ran.
const ResolvedInfo& ActualRenderer();
const ResolvedInfo& ActualVertices();
const ResolvedInfo& ActualMmv();

// MMV device callback: pure mode + LUID (plus the name as a hint), resolved at
// startup like the renderer.  A FollowRenderer request expands to the real
// renderer device of this run; when the renderer runs on a REF software
// rasterizer the follow result is the CPU mode so MMV uses its software
// backend, and when the renderer failed the request stays unresolved.  The
// MMV side owns its own capability reporting; this function never claims a
// device that was not requested.
Selection ResolvedMmv();

// Adapter lists for the UI menu and for building a Selection (the UI
// constructs Selections from AdapterEntry, identity fields included).
// RendererAdapters() is the complete D3D9 adapter list the main device can
// actually use - initialized and cached by ResolveRenderer from the real
// D3D9 enumeration, NOT the FL11-only compute list; ComputeAdapters() covers
// the Vertices and Mmv roles (DXGI).  Both carry the full DeviceIdentity.
// Empty before their enumeration runs.
const std::vector<AdapterEntry>& RendererAdapters();
const std::vector<AdapterEntry>& ComputeAdapters();

// What InitD3D actually created.  success=false marks the renderer failed
// (nothing is displayed as running); softwareRasterizer=true (REF/D3DDEVTYPE_SW)
// makes a FollowRenderer MMV resolve to the CPU mode.  `error` carries the
// creation failure detail.
void RecordDeviceResult(bool success, bool softwareRasterizer,
                        const std::wstring& error);

// Pure resolution used by ResolveRenderer / Initialize and by tests: match
// `choice` against `adapters`.  Binding a concrete adapter REQUIRES the saved
// full hardware identity: a LUID hit is verified against that identity (an
// old LUID another card now owns never binds), and after a LUID change only a
// unique full-identity match binds.  Same-model duplicates the identity
// cannot single out fall back, and a selection without a saved identity falls
// back too - the name and the id are never used as single-item guesses.  The
// fallback runs the automatic pick (best performanceRank, then lowest
// ordinal) with fellBack set and a reason; the saved selection itself is
// never rewritten here.
ResolvedInfo ResolveFromTable(const Selection& choice,
                              const std::vector<AdapterEntry>& adapters);

// Test seams.  Production code must not call these.
void SetConfigPathOverrideForTesting(const wchar_t* path);
void ResetForTesting();

}  // namespace processor
}  // namespace mikumikustudio
