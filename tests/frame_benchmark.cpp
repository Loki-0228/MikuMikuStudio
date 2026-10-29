// MAINTENANCE: benchmark; purpose and cleanup conditions in tests/README.md.
//
// Multi-model frame benchmark.  One PMX/PMD model can be loaded into several
// independent model slots (--copies) with a fixed layout, or a PMM scene can
// be loaded as before.  Usage and the JSON schema live in
// docs/development/benchmarks.md.
//
// MMV stages use the host's existing CPU timers. --require-mmv/--forbid-mmv
// verify the plugin state before sampling. Skin-only/native drivers do not
// evaluate MMV, so their stage measurements are marked unavailable.
//
// Nothing here is a product path: the executable is EXCLUDE_FROM_ALL, and the
// renderer phase timers need MIKUMIKUSTUDIO_PERF; MMV host timers do not.
#include "mikumikustudio/perf_timing.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/sequential_file_reader.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/transparent_sort_cache.hpp"
#include "mikumikustudio/texture_prepare_cache.hpp"
#include "mikumikustudio/effect_file_watch.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <commctrl.h>

namespace mikumikustudio { void LoadSceneFile(); }
using namespace mikumikustudio;
#include "playback_benchmark.hpp"
unsigned loadDialogs=0;
void PrintTransparentCache() {
    const auto s=transparent_sort_cache::GetStatistics();
    std::printf("TRANSPARENT_CACHE source_hits=%llu source_misses=%llu object_hits=%llu object_misses=%llu plan_hits=%llu plan_misses=%llu gpu_hits=%llu uploads=%llu allocations=%llu source_bytes=%zu gpu_bytes=%zu\n",
        s.sourceHits,s.sourceMisses,s.objectHits,s.objectMisses,s.planHits,s.planMisses,s.gpuHits,s.uploads,s.allocations,s.sourceBytes,s.gpuBytes);
}
bool skipMissingModels=false;
unsigned skippedModels=0;
bool benchmarkRendering=false;
// True only while InitMainWindowAndD3D runs; see the hook below.
bool initializingApp=false;
// Mirrors kExitInit in the anonymous namespace further down.
constexpr int kBenchmarkExitInit = 3;
BOOL CALLBACK PrintDialogText(HWND window, LPARAM) {
    wchar_t wide[2048]{};
    char utf8[8192]{};
    GetWindowTextW(window, wide, 2048);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, nullptr, nullptr);
    if (utf8[0]) std::printf("dialog: %s\n", utf8);
    return TRUE;
}
LRESULT CALLBACK BenchmarkDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND window=reinterpret_cast<HWND>(wp);
        wchar_t cls[32]{}; GetClassNameW(window,cls,32);
        if (wcscmp(cls,L"#32770")==0) {
            ++loadDialogs;
            wchar_t title[256]{}; GetWindowTextW(window,title,256);
            // An init-phase dialog (for example "CreateDevice Failed") is a
            // failure: hide it, report it, and fail with code 3 at once, or the
            // modal box blocks until the harness timeout.
            if(initializingApp){
                ShowWindow(window,SW_HIDE);
                std::printf("%s dialog encountered\n", benchmarkRendering ? "render" : "load");
                PrintDialogText(window, 0);
                EnumChildWindows(window, PrintDialogText, 0);
                std::fflush(stdout);
                ExitProcess(kBenchmarkExitInit);
            }
            std::printf("%s dialog encountered\n", benchmarkRendering ? "render" : "load");
            PrintDialogText(window, 0);
            EnumChildWindows(window, PrintDialogText, 0);
            // Effects may fail at their first draw or device reset, after load.
            // A modal error must invalidate this run, never hang or get timed.
            if (benchmarkRendering) ExitProcess(18);
            if(g_Block)std::printf("load status: %s\n",g_Block->state.statusText);
            ShowWindow(window,SW_HIDE);
            if(skipMissingModels&&GetDlgItem(window,0x2B0)){
                ++skippedModels;PostMessageW(window,WM_COMMAND,0x2B0,0);
            }
            else if (GetDlgItem(window,IDCANCEL)||GetDlgItem(window,0x2AF)) PostMessageW(window,WM_COMMAND,IDCANCEL,0);
            else if (GetDlgItem(window,IDOK)) PostMessageW(window,WM_COMMAND,IDOK,0);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}

namespace {

// ---------------------------------------------------------------------------
// Exit codes.  The first group is the historical driver contract and must not
// move; the second group covers the multi-model options.
// ---------------------------------------------------------------------------
constexpr int kExitUsage = 2;          // unknown option / bad value
constexpr int kExitInit = 3;
constexpr int kExitMme = 4;
constexpr int kExitScene = 5;          // no model loaded, or a dialog failed
constexpr int kExitQuery = 6;
constexpr int kExitFlush = 7;
constexpr int kExitOutput = 8;
constexpr int kExitNative = 9;
constexpr int kExitNativeValidation = 10;
constexpr int kExitCopiesMissing = 11; // fewer copies loaded than requested
constexpr int kExitViewport = 12;      // the requested viewport is unreachable
constexpr int kExitJson = 13;
constexpr int kExitSurface = 14;
constexpr int kExitDigest = 15;
constexpr int kExitDigestOpen = 16;
constexpr int kExitVisibility = 17;    // a copy does not project into the view
constexpr int kExitSaveScene = 19;     // --save-scene could not write its PMM
constexpr int kExitMmvRequired = 20;   // --require-mmv but MMV is not loaded
constexpr int kExitMmvForbidden = 21;  // --forbid-mmv but MMV is loaded
constexpr int kExitMmvUpdate = 22;     // a sampled MMV update failed or was skipped
constexpr int kExitShadowModes = 56;   // frozen 1 -> 2 -> 1 comparison failed

constexpr int kMaxCopies = 8;
constexpr int kMaxSamples = 100000;
constexpr int kMinViewport = 16;
constexpr int kMaxViewport = 16384;

enum class Layout { Overlap, Spread };

struct Options {
    const wchar_t* target = nullptr;
    const wchar_t* output = nullptr;   // historical positional output path
    const wchar_t* json = nullptr;
    const wchar_t* saveScene = nullptr;
    const wchar_t* extraModel = nullptr;
    bool native = false;
    bool throughputSkipMissing = false;
    bool throughput = false;
    bool explicitThroughput = false;
    bool skinOnly = false;
    bool unbuffered = false;
    bool loadSkipMissing = false;
    bool loadOnly = false;
    int copies = 1;
    bool copiesGiven = false;
    Layout layout = Layout::Overlap;
    bool layoutGiven = false;
    int warmup = 30;
    int samples = 150;
    int playbackSeconds = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    bool viewportGiven = false;
    bool modelFile = false;
    bool cameraGiven = false;
    bool cameraPanel = false;
    bool noPreview = false;
    float camera[7]{}; // x,y,z,distance,yaw,pitch,roll; angles in degrees
    bool shadowCameraSweep = false;
    float shadowCameraSweepEnd[7]{};
    int sweepFrames = 61;
    bool sweepFramesGiven = false;
    float orbitDegrees = 0;
    bool selfShadowGiven = false;
    int selfShadowMode = 0;
    int shadowRange = -1;
    bool lightGiven = false;
    float light[3]{};
    int fineShadow = -1;
    int groundShadow = -1;
    int adaptiveShadow = -1;
    bool fixedModelShadows = true;
    int dynamicPrecision = -1;
    int sortPrecision = -1;
    int antialias = -1;
    int outlines = -1;
    int sortEnabled = -1;
    int sortCrossObjects = -1;
    bool verifyViewToggles = false;
    bool verifyShadowCycle = false;
    bool verifyShadowZoom = false;
    bool verifyShadowModes = false;
    bool requireMmv = false;   // sampling may start only with MMV loaded
    bool forbidMmv = false;    // sampling may start only without MMV loaded
    const wchar_t* processor = L"auto";
};

bool ParseLong(const wchar_t* text, long& value) {
    if (text == nullptr || *text == L'\0') return false;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (end == text || (end != nullptr && *end != L'\0')) return false;
    value = parsed;
    return true;
}

bool ParseViewport(const wchar_t* text, int& width, int& height) {
    if (text == nullptr) return false;
    const wchar_t* separator = wcschr(text, L'x');
    if (separator == nullptr) separator = wcschr(text, L'X');
    if (separator == nullptr || separator == text) return false;
    const std::wstring left(text, static_cast<std::size_t>(separator - text));
    const std::wstring right(separator + 1);
    long w = 0, h = 0;
    if (!ParseLong(left.c_str(), w) || !ParseLong(right.c_str(), h)) return false;
    if (w < kMinViewport || w > kMaxViewport || h < kMinViewport ||
        h > kMaxViewport)
        return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

// Parses everything after the target path.  The historical form
// "<target> <mode> [output]" keeps working: an unrecognized token that does
// not start with "--" is the positional output path, exactly as before.
int ParseOptions(int argc, wchar_t** argv, Options& options) {
    options.target = argv[1];
    int positional = 0;
    bool modeSeen = false;
    for (int i = 2; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        const auto value = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 >= argc) {
                std::fwprintf(stderr, L"missing value for %ls\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        const auto setMode = [&]() { modeSeen = true; };
        if (wcscmp(arg, L"--native") == 0) {
            options.native = true; setMode();
        } else if (wcscmp(arg, L"--playback-seconds") == 0) {
            long seconds = 0;
            if (!ParseLong(value(arg), seconds) || seconds < 1 || seconds > 300) return kExitUsage;
            options.playbackSeconds = static_cast<int>(seconds);
            setMode();
        } else if (wcscmp(arg, L"--throughput") == 0) {
            options.explicitThroughput = true; setMode();
        } else if (wcscmp(arg, L"--throughput-skip-missing") == 0) {
            options.throughputSkipMissing = true; setMode();
        } else if (wcscmp(arg, L"--extra-model") == 0) {
            options.extraModel = value(arg);
            if (!options.extraModel) return kExitUsage;
        } else if (wcscmp(arg, L"--processor") == 0) {
            const wchar_t* text = value(arg);
            if (!text || (wcscmp(text, L"auto") && wcscmp(text, L"cpu") && wcscmp(text, L"gpu"))) {
                std::fputs("--processor takes auto, cpu or gpu\n", stderr);
                return kExitUsage;
            }
            options.processor = text;
        } else if (wcscmp(arg, L"--skin-only") == 0) {
            options.skinOnly = true; setMode();
        } else if (wcscmp(arg, L"--load-unbuffered") == 0) {
            options.unbuffered = true; setMode();
        } else if (wcscmp(arg, L"--load-skip-missing") == 0) {
            options.loadSkipMissing = true; setMode();
        } else if (wcscmp(arg, L"--load-only") == 0) {
            options.loadOnly = true; setMode();
        } else if (wcscmp(arg, L"--copies") == 0) {
            const wchar_t* text = value(arg);
            long copies = 0;
            if (text == nullptr || !ParseLong(text, copies) || copies < 1 ||
                copies > kMaxCopies) {
                std::fputs("--copies takes an integer in 1..8\n", stderr);
                return kExitUsage;
            }
            options.copies = static_cast<int>(copies);
            options.copiesGiven = true;
        } else if (wcscmp(arg, L"--layout") == 0) {
            const wchar_t* text = value(arg);
            if (text == nullptr) return kExitUsage;
            if (wcscmp(text, L"overlap") == 0)
                options.layout = Layout::Overlap;
            else if (wcscmp(text, L"spread") == 0)
                options.layout = Layout::Spread;
            else {
                std::fputs("--layout takes overlap or spread\n", stderr);
                return kExitUsage;
            }
            options.layoutGiven = true;
        } else if (wcscmp(arg, L"--warmup") == 0) {
            const wchar_t* text = value(arg);
            long warmup = 0;
            if (text == nullptr || !ParseLong(text, warmup) || warmup < 1 ||
                warmup > kMaxSamples) {
                std::fputs("--warmup takes a positive frame count\n", stderr);
                return kExitUsage;
            }
            options.warmup = static_cast<int>(warmup);
        } else if (wcscmp(arg, L"--samples") == 0) {
            const wchar_t* text = value(arg);
            long samples = 0;
            if (text == nullptr || !ParseLong(text, samples) || samples < 1 ||
                samples > kMaxSamples) {
                std::fputs("--samples takes a positive frame count\n", stderr);
                return kExitUsage;
            }
            options.samples = static_cast<int>(samples);
        } else if (wcscmp(arg, L"--sweep-frames") == 0) {
            long frames = 0;
            if (!ParseLong(value(arg), frames) || frames < 2 || frames > 10000) {
                std::fputs("--sweep-frames takes an integer in 2..10000\n", stderr);
                return kExitUsage;
            }
            options.sweepFrames = static_cast<int>(frames);
            options.sweepFramesGiven = true;
        } else if (wcscmp(arg, L"--ground-shadow") == 0) {
            long mode = 0;
            if (!ParseLong(value(arg), mode) || mode < 0 || mode > 1) return kExitUsage;
            options.groundShadow = int(mode);
        } else if (wcscmp(arg, L"--fine-shadow") == 0) {
            const wchar_t* text = value(arg);
            long mode = 0;
            if (!text || !ParseLong(text, mode) || mode < 0 || mode > 1) return kExitUsage;
            options.fineShadow = static_cast<int>(mode);
        } else if (wcscmp(arg, L"--fixed-model-shadows") == 0) {
            long mode=0;
            if(!ParseLong(value(arg),mode) || mode<0 || mode>1)return kExitUsage;
            options.fixedModelShadows=mode!=0;
        } else if (wcscmp(arg, L"--adaptive-shadow") == 0) {
            long mode = 0;
            const wchar_t* text = value(arg);
            if (!text || !ParseLong(text, mode) || mode < 0 || mode > 1) return kExitUsage;
            options.adaptiveShadow = static_cast<int>(mode);
        } else if (wcscmp(arg, L"--dynamic-precision") == 0) {
            long position = 0;
            const wchar_t* text = value(arg);
            if (!text || !ParseLong(text, position) || position < 0 || position > 9999) return kExitUsage;
            options.dynamicPrecision = static_cast<int>(position);
        } else if (wcscmp(arg, L"--sort-precision") == 0) {
            long value = -1;
            if (++i >= argc || !ParseLong(argv[i], value) || value < 0 || value > static_cast<int>(TransparentSortPrecision::Off)) return kExitUsage;
            options.sortPrecision = int(value);
        } else if (wcscmp(arg, L"--antialias") == 0 || wcscmp(arg, L"--outlines") == 0 ||
                   wcscmp(arg, L"--sort-enabled") == 0 || wcscmp(arg, L"--sort-cross-objects") == 0) {
            long value = -1;
            if (++i >= argc || !ParseLong(argv[i], value) || value < 0 || value > 1) return kExitUsage;
            if (wcscmp(arg, L"--antialias") == 0) options.antialias = int(value);
            else if (wcscmp(arg, L"--outlines") == 0) options.outlines = int(value);
            else if(wcscmp(arg,L"--sort-cross-objects")==0)options.sortCrossObjects=int(value);
            else options.sortEnabled = int(value);
        } else if (wcscmp(arg, L"--verify-view-toggles") == 0) {
            options.verifyViewToggles = true;
        } else if (wcscmp(arg, L"--verify-shadow-cycle") == 0) {
            options.verifyShadowCycle = true;
        } else if (wcscmp(arg, L"--verify-shadow-zoom") == 0) {
            options.verifyShadowZoom = true;
        } else if (wcscmp(arg, L"--verify-shadow-modes") == 0) {
            options.verifyShadowModes = true;
        } else if (wcscmp(arg, L"--require-mmv") == 0) {
            options.requireMmv = true;
        } else if (wcscmp(arg, L"--no-preview") == 0) {
            options.noPreview = true;
        } else if (wcscmp(arg, L"--camera-panel") == 0) {
            options.cameraPanel = true;
        } else if (wcscmp(arg, L"--forbid-mmv") == 0) {
            options.forbidMmv = true;
        } else if (wcscmp(arg, L"--self-shadow") == 0) {
            const wchar_t* text = value(arg);
            long mode = 0;
            // Deliberately not setMode(): this option only overrides a state
            // field, so it combines with --camera and --throughput rather than
            // selecting a driver mode of its own.
            if (text == nullptr || !ParseLong(text, mode) || mode < 0 ||
                mode > 2) {
                std::fputs("--self-shadow takes 0, 1 or 2\n", stderr);
                return kExitUsage;
            }
            options.selfShadowMode = static_cast<int>(mode);
            options.selfShadowGiven = true;
        } else if (wcscmp(arg, L"--shadow-range") == 0) {
            const wchar_t* text = value(arg);
            long range = 0;
            if (!text || !ParseLong(text, range) || range < 0 || range > 9999) return kExitUsage;
            options.shadowRange = static_cast<int>(range);
        } else if (wcscmp(arg, L"--camera") == 0 || wcscmp(arg, L"--shadow-camera-sweep") == 0 ||
                   wcscmp(arg, L"--orbit-degrees") == 0 || wcscmp(arg, L"--light") == 0) {
            const bool camera = wcscmp(arg, L"--camera") == 0;
            const bool sweep = wcscmp(arg, L"--shadow-camera-sweep") == 0;
            const bool light = wcscmp(arg, L"--light") == 0;
            const wchar_t* cursor = value(arg);
            if (!cursor) return kExitUsage;
            for (int field = 0; field < (camera || sweep ? 7 : light ? 3 : 1); ++field) {
                wchar_t* end = nullptr;
                const float parsed = std::wcstof(cursor, &end);
                const bool last = field == (camera || sweep ? 6 : light ? 2 : 0);
                if (end == cursor || !std::isfinite(parsed) ||
                    (last ? *end != L'\0' : *end != L',')) {
                    std::fputs("invalid camera/sweep/orbit/light value\n", stderr); return kExitUsage;
                }
                if (camera) options.camera[field] = parsed;
                else if (sweep) options.shadowCameraSweepEnd[field] = parsed;
                else if (light) options.light[field] = parsed;
                else options.orbitDegrees = parsed;
                cursor = end + (last ? 0 : 1);
            }
            if (camera) options.cameraGiven = true;
            if (sweep) options.shadowCameraSweep = true;
            if (light) {
                if (options.light[0] == 0 && options.light[1] == 0 && options.light[2] == 0) return kExitUsage;
                options.lightGiven = true;
            }
        } else if (wcscmp(arg, L"--json") == 0) {
            options.json = value(arg);
            if (options.json == nullptr) return kExitUsage;
        } else if (wcscmp(arg, L"--save-scene") == 0) {
            options.saveScene = value(arg);
            if (options.saveScene == nullptr) return kExitUsage;
        } else if (wcscmp(arg, L"--viewport") == 0) {
            const wchar_t* text = value(arg);
            if (text == nullptr ||
                !ParseViewport(text, options.viewportWidth,
                               options.viewportHeight)) {
                std::fputs("--viewport takes WIDTHxHEIGHT\n", stderr);
                return kExitUsage;
            }
            options.viewportGiven = true;
        } else if (arg[0] == L'-' && arg[1] == L'-') {
            std::fwprintf(stderr, L"unknown option %ls\n", arg);
            return kExitUsage;
        } else if (positional == 0) {
            options.output = arg;
            ++positional;
        } else {
            std::fputs("too many positional arguments\n", stderr);
            return kExitUsage;
        }
    }
    if ((options.cameraGiven || options.orbitDegrees != 0) &&
        (options.copiesGiven || options.layoutGiven || options.skinOnly || options.native)) {
        std::fputs("camera/orbit cannot be combined with layouts, skin-only or native mode\n", stderr);
        return kExitUsage;
    }
    if (options.requireMmv && options.forbidMmv) {
        std::fputs("--require-mmv and --forbid-mmv are mutually exclusive\n",
                   stderr);
        return kExitUsage;
    }
    options.throughput = options.native || options.throughputSkipMissing ||
                         options.explicitThroughput;
    options.loadOnly = options.loadOnly || options.loadSkipMissing || options.unbuffered;
    if (options.sweepFramesGiven && !options.shadowCameraSweep) {
        std::fputs("--sweep-frames requires --shadow-camera-sweep\n", stderr);
        return kExitUsage;
    }
    if (options.shadowCameraSweep) {
        if (!options.throughput || !options.output || !options.viewportGiven ||
            options.native || options.skinOnly || options.loadOnly || options.playbackSeconds ||
            options.copiesGiven || options.layoutGiven || options.orbitDegrees != 0 ||
            options.verifyShadowCycle || options.verifyShadowZoom || options.verifyShadowModes ||
            options.verifyViewToggles || options.saveScene) {
            std::fputs("--shadow-camera-sweep requires DX9 --throughput, --viewport and an output path; "
                       "cannot combine with layouts, playback, orbit, other verification modes or scene saving\n", stderr);
            return kExitUsage;
        }
        options.cameraPanel = true;
    }
    if (options.verifyShadowModes) {
        if (!options.explicitThroughput || !options.output || !options.viewportGiven ||
            options.native || options.skinOnly || options.loadOnly || options.playbackSeconds ||
            options.shadowCameraSweep || options.copiesGiven || options.layoutGiven || options.orbitDegrees != 0 ||
            options.verifyShadowCycle || options.verifyShadowZoom || options.verifyViewToggles || options.saveScene) {
            std::fputs("--verify-shadow-modes requires DX9 --throughput, --viewport and an output path; "
                       "cannot combine with sweep, layouts, playback, orbit, other verification modes or scene saving\n", stderr);
            return kExitUsage;
        }
        options.cameraPanel = true;
    }
    if ((options.requireMmv || options.forbidMmv) &&
        (options.native || options.skinOnly || options.loadOnly)) {
        std::fputs("MMV load-state gates require the full DX9 rendering driver\n", stderr);
        return kExitUsage;
    }
    (void)positional;
    const wchar_t* extension = wcsrchr(options.target, L'.');
    options.modelFile =
        extension != nullptr &&
        (_wcsicmp(extension, L".pmx") == 0 || _wcsicmp(extension, L".pmd") == 0);
    if (options.copiesGiven && !options.modelFile) {
        std::fputs("--copies accepts a PMX or PMD model file, not a scene\n",
                   stderr);
        return kExitUsage;
    }
    if (options.saveScene != nullptr && (!options.modelFile || !options.throughput ||
            options.native || options.skinOnly || (!options.copiesGiven && !options.layoutGiven && !options.extraModel))) {
        std::fputs("--save-scene requires --throughput, a model file and a layout/copy count or --extra-model\n",
                   stderr);
        return kExitUsage;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// JSON output.  A small nested builder: the schema is fixed, so the writer
// only has to place keys, and every missing measurement is written as null
// instead of a fabricated zero.
// ---------------------------------------------------------------------------
std::string Utf8(const wchar_t* text) {
    if (text == nullptr) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 1) return std::string();
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size) - 1);
    return out;
}

std::string Escaped(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (raw) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += raw;
            }
        }
    }
    return out;
}

class Json {
public:
    explicit Json(std::string& target) : out_(target) {
        out_ += '{';
        open_.push_back('{');
        first_.push_back(true);
    }
    void Object(const char* key) { Key(key); out_ += '{'; Push('{'); }
    void Array(const char* key) { Key(key); out_ += '['; Push('['); }
    void Item() { Comma(); out_ += '{'; Push('{'); }
    void End() {
        const char kind = open_.back();
        open_.pop_back();
        first_.pop_back();
        out_ += kind == '{' ? '}' : ']';
    }
    void Number(const char* key, double value, int precision = 6) {
        Key(key);
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        out_ += buffer;
    }
    void Integer(const char* key, long long value) {
        Key(key);
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%lld", value);
        out_ += buffer;
    }
    void String(const char* key, const std::string& value) {
        Key(key);
        out_ += '"';
        out_ += Escaped(value);
        out_ += '"';
    }
    void String(const char* key, const wchar_t* value) {
        if (value == nullptr) { Null(key); return; }
        String(key, Utf8(value));
    }
    void Bool(const char* key, bool value) {
        Key(key);
        out_ += value ? "true" : "false";
    }
    void Null(const char* key) { Key(key); out_ += "null"; }
    void NumberOrNull(const char* key, double value, bool present) {
        if (present) Number(key, value); else Null(key);
    }
    std::string Finish() { return out_ + "\n}\n"; }

private:
    void Push(char kind) {
        open_.push_back(kind);
        first_.push_back(true);
    }
    void Comma() {
        if (!first_.empty()) {
            if (!first_.back()) out_ += ',';
            first_.back() = false;
        }
    }
    void Key(const char* key) {
        Comma();
        out_ += '"';
        out_ += key;
        out_ += "\":";
    }
    std::string& out_;
    std::vector<char> open_;
    std::vector<bool> first_;
};

// The full renderer uses affine world-light maps. Measure both singular values
// so an anisotropic projection cannot hide excess sampling on its denser axis.
void WriteFixedModelShadows(Json& json,MMDApp* app) {
    json.Array("fixed_model_shadows");
    for(int slot=0;slot<kModelSlotCount;++slot) {
        auto* model=native_mesh::RenderModel(app->ModelSlot(slot));if(!model)continue;
        const auto info=shadow_filter::ReadModelInfo(app,model);
        json.Item();json.Integer("slot",slot);json.Bool("valid",info.valid);json.Integer("resolution",info.size);
        double scale=0;for(int row=0;row<3;++row)scale+=double(info.projection[row*4])*info.projection[row*4];
        json.Number("density",std::sqrt(scale)*info.size*.5,9);json.Number("requested_density",info.densityFloor,9);
        json.Array("projection");for(int row=0;row<4;++row){json.Item();for(int axis=0;axis<4;++axis){const char* keys[]{"x","y","z","w"};json.Number(keys[axis],info.projection[row*4+axis],9);}json.End();}json.End();
        json.End();
    }
    json.End();
}

struct ShadowDensity {
    double minimum=0,maximum=0;
    bool valid=false;
};
constexpr double kShadowDensityTolerance=1e-4;
ShadowDensity MeasureShadowDensity(const float* matrix,UINT width,UINT height) {
    if (!width || !height || matrix[3]!=0 || matrix[7]!=0 || matrix[11]!=0 ||
        !std::isfinite(matrix[15]) || matrix[15]==0) return {};
    double xx=0,yy=0,xy=0;
    for (int row=0;row<3;++row) {
        const double x=double(matrix[row*4])*width/(2*matrix[15]);
        const double y=double(matrix[row*4+1])*height/(2*matrix[15]);
        xx+=x*x;yy+=y*y;xy+=x*y;
    }
    const double disc=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy);
    ShadowDensity result{std::sqrt((std::max)(0.0,(xx+yy-disc)*.5)),
        std::sqrt((std::max)(0.0,(xx+yy+disc)*.5)),false};
    result.valid=std::isfinite(result.minimum) && std::isfinite(result.maximum) && result.minimum>0;
    return result;
}
bool ShadowDensityWithinReference(const ShadowDensity& density,double reference) {
    return density.valid && std::isfinite(reference) && reference>0 &&
        density.maximum<=reference*(1+kShadowDensityTolerance)+1e-7;
}
void WriteShadowDensity(Json& json,const ShadowDensity& density,double reference,bool available,
                         bool includeReference=true,double planned=0) {
    const bool valid=available && density.valid;
    const bool ratioValid=valid && std::isfinite(reference) && reference>0;
    if (includeReference)json.NumberOrNull("reference_density",reference,std::isfinite(reference) && reference>0);
    json.NumberOrNull("actual_density",density.minimum,valid);
    json.NumberOrNull("actual_density_max",density.maximum,valid);
    json.NumberOrNull("reference_ratio",ratioValid?density.minimum/reference:0,ratioValid);
    json.NumberOrNull("reference_ratio_max",ratioValid?density.maximum/reference:0,ratioValid);
    json.Bool("density_within_reference",available && ShadowDensityWithinReference(density,reference));
    const bool planValid=available && std::isfinite(planned) && planned>0;
    json.NumberOrNull("planned_density",planned,planValid);
    json.NumberOrNull("planned_reference_ratio",planValid && ratioValid?planned/reference:0,planValid && ratioValid);
    json.NumberOrNull("actual_planned_ratio",planValid && valid?density.minimum/planned:0,planValid && valid);
    json.Bool("density_within_plan",planValid && ShadowDensityWithinReference(density,planned));
}
bool ShadowSampleDimensions(MMDApp* app,UINT& width,UINT& height) {
    const auto sampling=shadow_filter::ReadNativeSamplingState(app);
    D3DSURFACE_DESC desc{};
    if (!sampling.texture || FAILED(sampling.texture->GetLevelDesc(0,&desc)))return false;
    width=desc.Width;height=desc.Height;
    return width && height;
}
// Reconstruct the recorded slab rather than assuming a five-unit camera box.
// The same near/far interval may require a larger sphere with an extreme lens.
bool ShadowSlabRadius(const float* worldViewProjection,const float* projection,
                      float nearDepth,float farDepth,double& radius) {
    if (!std::isfinite(nearDepth) || !std::isfinite(farDepth) || farDepth<=nearDepth)return false;
    using Matrix=d3dx::D3DXMATRIXF;
    auto& api=d3dx::Get();Matrix inverseClip{},inverseProjection{},inverseView{};
    if (!api.inverse(&inverseClip,nullptr,reinterpret_cast<const Matrix*>(worldViewProjection)) ||
        !api.inverse(&inverseProjection,nullptr,reinterpret_cast<const Matrix*>(projection)))return false;
    api.multiply(&inverseView,reinterpret_cast<const Matrix*>(projection),&inverseClip);
    const auto transform=[&](const std::array<float,3>& point,const Matrix& matrix,std::array<float,3>& result) {
        float out[4]{};api.vec3Transform(out,point.data(),&matrix);
        if (!std::isfinite(out[3]) || std::abs(out[3])<1e-12f)return false;
        for(int axis=0;axis<3;++axis) {
            result[axis]=out[axis]/out[3];
            if(!std::isfinite(result[axis]))return false;
        }
        return true;
    };
    std::array<std::array<float,3>,8> points{};double centre[3]{};
    for (int corner=0;corner<4;++corner) {
        const float x=corner&1?1.f:-1.f,y=corner&2?1.f:-1.f;
        std::array<float,3> start{},middle{};
        if(!transform({x,y,0},inverseProjection,start) || !transform({x,y,.5f},inverseProjection,middle))return false;
        const float dz=middle[2]-start[2];
        if (!(dz>0) || !std::isfinite(dz))return false;
        for (int end=0;end<2;++end) {
            std::array<float,3> point{};
            const float depth=end?farDepth:nearDepth;
            for(int axis=0;axis<3;++axis)
                point[axis]=start[axis]+(middle[axis]-start[axis])*(depth-start[2])/dz;
            auto& world=points[corner+end*4];
            if(!transform(point,inverseView,world))return false;
            for(int axis=0;axis<3;++axis)centre[axis]+=world[axis]/8.0;
        }
    }
    radius=0;
    for(const auto& point:points) {
        double squared=0;
        for(int axis=0;axis<3;++axis)squared+=(point[axis]-centre[axis])*(point[axis]-centre[axis]);
        radius=(std::max)(radius,std::sqrt(squared)/.8);
    }
    return std::isfinite(radius) && radius>0;
}

struct ShadowPlanInputs {
    std::array<float,16> lens{};
    std::array<double,3> detailDemandDepth{}; // middle, near, bridge
    std::array<double,3> detailAllocationWidth{};
    unsigned maximumSize=0,viewportWidth=0,viewportHeight=0;
    double coverageDensity=0,coverageDemandDepth=0,farBoundary=0,middleFarDepth=0,nearFarDepth=0;
    float finestDensity=0,projectedPixels=0;
    bool valid=false,deviceViewport=false,dynamic=false;
};
ShadowPlanInputs ReadShadowPlanInputs(MMDApp* app) {
    ShadowPlanInputs result;
    auto* renderer=app->Renderer();
    if(!renderer || !renderer->device)return result;
    D3DCAPS9 caps{};
    if(FAILED(renderer->device->GetDeviceCaps(&caps)) ||
       FAILED(renderer->device->GetTransform(D3DTS_PROJECTION,reinterpret_cast<D3DMATRIX*>(result.lens.data()))))return result;
    result.maximumSize=(std::min)({8192u,static_cast<unsigned>(caps.MaxTextureWidth),
        static_cast<unsigned>(caps.MaxTextureHeight),app->ShadowResolutionLimit()*2u});
    const float reference=NativeShadowSamplingDensity(app->LightDirection(),app->ShadowDistance(),
        app->SelfShadowMode(),app->ShadowResolutionLimit());
    result.finestDensity=NativeShadowSamplingDensity(app->LightDirection(),.1f,
        app->SelfShadowMode(),app->ShadowResolutionLimit());
    D3DVIEWPORT9 viewport{};
    result.deviceViewport=SUCCEEDED(renderer->device->GetViewport(&viewport)) && viewport.Width && viewport.Height;
    result.viewportWidth=result.deviceViewport?viewport.Width:app->RenderWidth();
    result.viewportHeight=result.deviceViewport?viewport.Height:app->RenderHeight();
    result.projectedPixels=(std::max)(float(result.viewportWidth)*std::abs(result.lens[0]),
        float(result.viewportHeight)*std::abs(result.lens[5]));
    result.dynamic=app->AdaptiveShadowResolution();
    const auto middle=shadow_filter::ReadDetailInfo(app,0);
    const auto nearInfo=shadow_filter::ReadDetailInfo(app,1);
    const auto bridge=shadow_filter::ReadDetailInfo(app,2);
    result.detailDemandDepth[0]=nearInfo.farDepth-(nearInfo.farDepth-nearInfo.nearDepth)*.35f;
    result.detailDemandDepth[1]=nearInfo.nearDepth;
    result.detailDemandDepth[2]=middle.farDepth;
    result.coverageDemandDepth=bridge.farDepth;
    result.farBoundary=bridge.farDepth;
    result.middleFarDepth=middle.farDepth;
    result.nearFarDepth=nearInfo.farDepth;
    result.coverageDensity=reference;
    if(result.dynamic) {
        for(const auto* info:{&nearInfo,&middle,&bridge})
            if(!std::isfinite(info->nearDepth) || !std::isfinite(info->farDepth) ||
               info->farDepth<=info->nearDepth)return result;
        // Coverage keeps the original 80-unit-radius depth budget even when
        // the dynamic bridge expands. Reconstruct its eight corner radius
        // independently; the current bridge endpoint cannot serve as oracle.
        const float budget=(std::max)(NativeShadowSamplingDensity(app->LightDirection(),.1f,1,app->ShadowResolutionLimit()),
            NativeShadowSamplingDensity(app->LightDirection(),.1f,2,app->ShadowResolutionLimit()));
        const double radius=(std::min)(5.0,(result.maximumSize>2?result.maximumSize-2:1)/(2.0*budget))*16;
        double lo=nearInfo.nearDepth,hi=lo+(std::max)(1.0,radius*4);
        for(unsigned iteration=0;iteration<28;++iteration) {
            const double mid=(lo+hi)*.5;double measured=0;
            if(!ShadowSlabRadius(reinterpret_cast<const float*>(&app->WorldViewProjection()),result.lens.data(),
                nearInfo.nearDepth,float(mid),measured))return result;
            if(measured<=radius)lo=mid;else hi=mid;
        }
        result.coverageDemandDepth=(std::max)(lo,double(nearInfo.nearDepth)+.001);
        // Full-picture coverage retains the selected native density in both
        // fixed and dynamic modes; allocation limits are checked separately.
        result.coverageDensity=reference;
        // The translated bridge retains its old nominal 120-radius light box.
        // Its new visible slab is longer and no longer sphere-fitted, so using
        // the new endpoints here would invent a larger allocation requirement.
        const double bridgeRadius=radius*1.5;
        lo=nearInfo.nearDepth;hi=lo+(std::max)(1.0,bridgeRadius*4);
        for(unsigned iteration=0;iteration<28;++iteration) {
            const double mid=(lo+hi)*.5;double measured=0;
            if(!ShadowSlabRadius(reinterpret_cast<const float*>(&app->WorldViewProjection()),result.lens.data(),
                nearInfo.nearDepth,float(mid),measured))return result;
            if(measured<=bridgeRadius)lo=mid;else hi=mid;
        }
        double measuredBridgeRadius=0;
        if(!ShadowSlabRadius(reinterpret_cast<const float*>(&app->WorldViewProjection()),result.lens.data(),
            nearInfo.nearDepth,float((std::max)(lo,double(nearInfo.nearDepth)+.001)),measuredBridgeRadius))return result;
        result.detailAllocationWidth[2]=2*measuredBridgeRadius;
    }
    result.valid=result.maximumSize>1 && std::isfinite(result.coverageDensity) && result.coverageDensity>0 &&
        std::isfinite(result.finestDensity) && result.finestDensity>0;
    return result;
}
struct CoverageFixedLimit {
    double minimum=0,maximum=0;
    bool known=false,within=false;
};
CoverageFixedLimit AssessCoverageFixedLimit(const float* matrix,const ShadowDensity& actual,
    double reference,const ShadowPlanInputs& inputs) {
    CoverageFixedLimit result;
    const auto capacity=MeasureShadowDensity(matrix,inputs.maximumSize,inputs.maximumSize);
    result.known=inputs.valid && actual.valid && capacity.valid && std::isfinite(reference) && reference>0;
    if(!result.known)return result;
    // This is the same already-fitted coverage box, including its snapping
    // margin. Do not deduct that margin a second time at maximum allocation.
    result.minimum=(std::min)(reference,capacity.minimum);
    result.maximum=(std::min)(reference,capacity.maximum);
    result.within=actual.minimum<=result.minimum*(1+kShadowDensityTolerance)+1e-7 &&
        actual.maximum<=result.maximum*(1+kShadowDensityTolerance)+1e-7;
    return result;
}
void WriteCoverageFixedLimit(Json& json,const CoverageFixedLimit& limit,const ShadowPlanInputs& inputs) {
    json.Bool("close_layer",false);
    json.String("fixed_capacity_basis","same_fitted_box_at_maximum_resolution");
    json.NumberOrNull("fixed_capacity_density",limit.minimum,limit.known);
    json.NumberOrNull("fixed_capacity_density_max",limit.maximum,limit.known);
    json.NumberOrNull("demand_depth",inputs.coverageDemandDepth,
        inputs.valid && std::isfinite(inputs.coverageDemandDepth) && inputs.coverageDemandDepth>0);
    json.NumberOrNull("far_boundary_depth",inputs.farBoundary,
        inputs.valid && std::isfinite(inputs.farBoundary) && inputs.farBoundary>0);
    json.Bool("far_density_within_fixed_capacity",limit.known && limit.within);
}
struct DetailSamplingPlan {
    double requested=0,requiredSize=0,maximumDensity=0,expectedActual=0,demandDepth=0,fixedCapacity=0,allocationWidth=0;
    unsigned expectedResolution=0;
    bool translatedBridge=false,allocationWithinBudget=false;
    bool closeLayer=false,farWithinFixed=false,fixedBlendMatches=false;
    bool known=false,hardwareSufficient=false,allocationSufficient=false,matchesCapacity=false,recordedPlanMatches=false;
};
DetailSamplingPlan AssessDetailSampling(const shadow_filter::DetailInfo& info,const ShadowDensity& actual,
    double reference,bool dynamic,const float* camera,const ShadowPlanInputs& inputs,unsigned layer) {
    DetailSamplingPlan result;
    if(layer>=inputs.detailDemandDepth.size())return result;
    result.closeLayer=dynamic || layer!=2;
    result.demandDepth=inputs.detailDemandDepth[layer];
    double radius=0;
    if(!inputs.valid)return result;
    result.translatedBridge=dynamic && layer==2;
    if(result.translatedBridge)result.allocationWidth=inputs.detailAllocationWidth[layer];
    else {
        if(!ShadowSlabRadius(camera,inputs.lens.data(),info.nearDepth,info.farDepth,radius))return result;
        result.allocationWidth=2*radius;
    }
    if(!std::isfinite(result.allocationWidth) || result.allocationWidth<=0)return result;
    result.maximumDensity=(inputs.maximumSize-1)/result.allocationWidth;
    result.requested=dynamic?ShadowLayerDensity(float(reference),inputs.finestDensity,inputs.projectedPixels,
        float(result.demandDepth),inputs.lens[15]==1.f,true,result.closeLayer,float(result.maximumDensity)):reference;
    result.recordedPlanMatches=std::isfinite(info.densityFloor) && info.densityFloor>0 &&
        std::abs(info.densityFloor-result.requested)<=result.requested*kShadowDensityTolerance+1e-7;
    result.known=info.valid && inputs.valid && info.size>1 && result.recordedPlanMatches;
    if(!result.known)return result;
    // Two allocation texels are reserved by the planner; the fitted map retains
    // one texel of snapping margin. Check the actual allocation independently.
    result.requiredSize=result.allocationWidth*result.requested+2;
    result.expectedResolution=ShadowTextureSizeForDensity(float(result.allocationWidth),float(result.requested),inputs.maximumSize);
    result.allocationWithinBudget=info.size<=result.expectedResolution;
    result.fixedCapacity=(std::min)(reference,result.maximumDensity);
    const double allocationDensity=(info.size-1)/result.allocationWidth;
    result.expectedActual=(std::min)(result.requested,allocationDensity);
    result.hardwareSufficient=result.maximumDensity>=result.requested*(1-kShadowDensityTolerance);
    result.allocationSufficient=allocationDensity>=result.requested*(1-kShadowDensityTolerance);
    result.matchesCapacity=actual.valid && ShadowDensityWithinReference(actual,result.requested) &&
        std::abs(actual.minimum-result.expectedActual)<=result.expectedActual*kShadowDensityTolerance+1e-7 &&
        std::abs(actual.maximum-result.expectedActual)<=result.expectedActual*kShadowDensityTolerance+1e-7;
    result.farWithinFixed=!result.closeLayer && ShadowDensityWithinReference(actual,result.fixedCapacity);
    const double expectedStart=layer==1?info.farDepth-(info.farDepth-info.nearDepth)*.35:
        layer==0?(dynamic?(std::max)(inputs.nearFarDepth,double(info.farDepth)/1.5):inputs.nearFarDepth):
        (dynamic?(std::max)(inputs.middleFarDepth,double(info.farDepth)/2.5):inputs.middleFarDepth);
    result.fixedBlendMatches=std::isfinite(info.blendStart) &&
        std::abs(info.blendStart-expectedStart)<=(std::max)(1.0,double(info.farDepth))*2e-6;
    return result;
}
void WriteDetailSamplingPlan(Json& json,const DetailSamplingPlan& plan) {
    json.Bool("close_layer",plan.closeLayer);
    json.String("fixed_capacity_basis",plan.translatedBridge?"unchanged_nominal_bridge_box_at_maximum_resolution":
        "same_camera_slab_at_maximum_resolution");
    json.NumberOrNull("allocation_box_width",plan.allocationWidth,plan.known);
    json.NumberOrNull("budget_resolution",plan.expectedResolution,plan.known);
    json.Bool("allocation_within_unchanged_budget",plan.known && plan.allocationWithinBudget);
    json.NumberOrNull("demand_depth",plan.demandDepth,plan.known);
    json.Bool("capacity_known",plan.known);
    json.NumberOrNull("required_resolution",plan.requiredSize,plan.known);
    json.NumberOrNull("maximum_attainable_density",plan.maximumDensity,plan.known);
    json.NumberOrNull("expected_actual_density",plan.expectedActual,plan.known);
    json.Bool("hardware_sufficient",plan.known && plan.hardwareSufficient);
    json.Bool("allocation_sufficient",plan.known && plan.allocationSufficient);
    json.Bool("capacity_limited",plan.known && !plan.allocationSufficient);
    json.Bool("recorded_plan_matches",plan.recordedPlanMatches);
    json.Bool("density_matches_capacity",plan.known && plan.matchesCapacity);
    json.NumberOrNull("fixed_capacity_density",plan.fixedCapacity,plan.known);
    json.Bool("fade_matches_geometry",plan.known && plan.fixedBlendMatches);
    if(plan.closeLayer) {
        json.Null("far_density_within_fixed_capacity");json.Null("far_fixed_blend_matches");
    } else {
        json.Bool("far_density_within_fixed_capacity",plan.known && plan.farWithinFixed);
        json.Bool("far_fixed_blend_matches",plan.known && plan.fixedBlendMatches);
    }
}

// ---------------------------------------------------------------------------
// Scene placement
// ---------------------------------------------------------------------------
struct Bounds {
    float min[3]{};
    float max[3]{};
    bool valid = false;
};

Bounds ModelBounds(MMDApp* app, unsigned char* model) {
    Bounds bounds;
    float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    GpuModelInput input;
    // geometry=true: the packer only fills the vertex array when it applies
    // the vertex morphs, and the bounds must come from real positions.
    if (!PrepareGpuModelInput(app, model, identity, input, true) ||
        input.vertices.empty())
        return bounds;
    for (int axis = 0; axis < 3; ++axis) {
        bounds.min[axis] = input.vertices[0].position[axis];
        bounds.max[axis] = input.vertices[0].position[axis];
    }
    for (const auto& vertex : input.vertices)
        for (int axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
    bounds.valid = true;
    return bounds;
}

struct Copy {
    int slot = -1;
    unsigned char* model = nullptr;
    float offset[3]{};
    std::vector<float> lastTranslation;        // 3 floats per bone
    std::vector<unsigned char> translationKnown;  // one flag per bone
    unsigned applications = 0;   // frames on which a new pose was translated
};

// Fixed offsets, derived only from the model's own bounding box and the
// requested copy count.  No randomness and no clock input, so two runs of the
// same build lay the scene out identically, and two builds of the same model
// differ only by the measured bounds they report.
struct Vec3 { float x = 0, y = 0, z = 0; };

std::vector<Vec3> LayoutCenters(Layout layout, int copies, const Bounds& bounds) {
    const float size[3] = {
        bounds.max[0] - bounds.min[0],
        bounds.max[1] - bounds.min[1],
        bounds.max[2] - bounds.min[2]};
    const float center[3] = {
        (bounds.max[0] + bounds.min[0]) * 0.5f,
        (bounds.max[1] + bounds.min[1]) * 0.5f,
        (bounds.max[2] + bounds.min[2]) * 0.5f};
    std::vector<Vec3> centers(static_cast<std::size_t>(copies));
    // The explicit layouts are framed by the fixed overhead camera below, so
    // the world X axis is the screen horizontal and the world Z axis is the
    // screen vertical.  Spreading along Y would collapse into the depth axis
    // of that camera and the copies would sit on top of each other on screen.
    if (layout == Layout::Overlap) {
        // Visible stacking: a small shift in X and Z keeps every copy on
        // screen, and the small height step keeps them off one common plane.
        for (int i = 0; i < copies; ++i) {
            const float t = static_cast<float>(i) -
                            static_cast<float>(copies - 1) * 0.5f;
            centers[i].x = center[0] + t * 0.18f * size[0];
            centers[i].z = center[2] + t * 0.08f * size[2];
            centers[i].y = center[1] + t * 0.05f * std::max(size[1], 1.0f);
        }
        return centers;
    }
    // Spread: a grid in the X-Z plane whose cell is 1.15x the model bounding
    // box, so the copies cannot touch even when the bounds are exact.
    const int columns = copies <= 4 ? copies : 4;
    const int rows = (copies + columns - 1) / columns;
    const float cell[2] = {size[0] * 1.15f, size[2] * 1.15f};
    for (int i = 0; i < copies; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        centers[i].x = center[0] +
                       (static_cast<float>(column) -
                        static_cast<float>(columns - 1) * 0.5f) * cell[0];
        centers[i].z = center[2] +
                       (static_cast<float>(row) -
                        static_cast<float>(rows - 1) * 0.5f) * cell[1];
        centers[i].y = center[1];
    }
    return centers;
}

// A rigid translation of every bone's world matrix, applied as a pure delta on
// top of whatever pose the engine has produced.  The engine's own values are
// never replaced by a stored rest pose: when a frame recomputes a bone matrix
// the offset is added to that new value, and when it does not the matrix is
// left exactly as it is.  A static scene therefore keeps a constant offset
// without accumulating, and animation or physics that rewrites the pose is
// translated rather than frozen.
void ApplyCopyOffsets(std::vector<Copy>& copies) {
    for (Copy& copy : copies) {
        if (copy.model == nullptr) continue;
        auto* bones = mdl::Bones(copy.model);
        const unsigned boneCount = mdl::Mdl(copy.model)->boneCount;
        // A copy without placement state was not laid out (a single copy,
        // skin-only, or a scene file): leave its bones untouched.
        if (bones == nullptr ||
            copy.lastTranslation.size() < static_cast<std::size_t>(boneCount) * 3 ||
            copy.translationKnown.size() < boneCount)
            continue;
        for (unsigned bone = 0; bone < boneCount; ++bone) {
            float* matrix = bones[bone].matInit;
            float* last = &copy.lastTranslation[bone * 3];
            if (copy.translationKnown[bone] != 0 && matrix[12] == last[0] &&
                matrix[13] == last[1] && matrix[14] == last[2])
                continue;  // the offset we applied is still in place
            matrix[12] += copy.offset[0];
            matrix[13] += copy.offset[1];
            matrix[14] += copy.offset[2];
            last[0] = matrix[12];
            last[1] = matrix[13];
            last[2] = matrix[14];
            copy.translationKnown[bone] = 1;
            ++copy.applications;
        }
    }
}

// Mean of a strided vertex sample from the skinned vertex buffer of one copy.
// This reads back the geometry the renderer actually draws, so the placement
// can be checked against the reported offsets instead of being taken on trust.
bool SampledVertexCenter(unsigned char* model, Vec3& center, unsigned& sampled) {
    auto& record = *mdl::Mdl(model);
    auto* buffer = mdl::ResourceAs<IDirect3DVertexBuffer9>(record.vertexBuffer);
    if (buffer == nullptr || record.vertexCount == 0) return false;
    const unsigned vertexCount = record.vertexCount;
    // The main buffer is 32 + 16 * additionalUvCount bytes per vertex
    // (model_skinning.cpp: stride = 32 + 16 * additionalUv); PMD uses 32.
    const unsigned stride =
        record.physicsMode == 2 ? 32u + 16u * record.pmxAdditionalUvCount : 32u;
    auto* base = reinterpret_cast<const unsigned char*>(nullptr);
    void* data = nullptr;
    if (FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) return false;
    base = static_cast<const unsigned char*>(data);
    double sum[3] = {0.0, 0.0, 0.0};
    unsigned count = 0;
    const unsigned step = vertexCount > 4096 ? vertexCount / 4096 : 1;
    for (unsigned vertex = 0; vertex < vertexCount; vertex += step) {
        const float* position =
            reinterpret_cast<const float*>(base + std::size_t(vertex) * stride);
        sum[0] += position[0];
        sum[1] += position[1];
        sum[2] += position[2];
        ++count;
    }
    buffer->Unlock();
    if (count == 0) return false;
    center.x = static_cast<float>(sum[0] / count);
    center.y = static_cast<float>(sum[1] / count);
    center.z = static_cast<float>(sum[2] / count);
    sampled = count;
    return true;
}

struct Matrix {
    float m[4][4]{};
};

Matrix ReadTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state) {
    Matrix result;
    device->GetTransform(state, reinterpret_cast<D3DMATRIX*>(&result));
    return result;
}

// D3DX matrices are row-major and D3D multiplies row vectors: p' = p * M, so
// component j of the result is the dot product of p with COLUMN j of M.
// Concatentation is therefore the ordinary product W * V * P.
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += static_cast<double>(a.m[row][k]) *
                       static_cast<double>(b.m[k][column]);
            out.m[row][column] = static_cast<float>(sum);
        }
    return out;
}

Vec3 TransformPoint(const Matrix& matrix, const Vec3& point) {
    Vec3 out;
    out.x = point.x * matrix.m[0][0] + point.y * matrix.m[1][0] +
            point.z * matrix.m[2][0] + matrix.m[3][0];
    out.y = point.x * matrix.m[0][1] + point.y * matrix.m[1][1] +
            point.z * matrix.m[2][1] + matrix.m[3][1];
    out.z = point.x * matrix.m[0][2] + point.y * matrix.m[1][2] +
            point.z * matrix.m[2][2] + matrix.m[3][2];
    return out;
}

struct Clip {
    double x = 0;
    double y = 0;
    double w = 0;
};

// Applies the projection to a point already in view space (w = 1).
Clip ProjectView(const Matrix& projection, const Vec3& view) {
    Clip out;
    out.x = static_cast<double>(view.x) * projection.m[0][0] +
            static_cast<double>(view.y) * projection.m[1][0] +
            static_cast<double>(view.z) * projection.m[2][0] + projection.m[3][0];
    out.y = static_cast<double>(view.x) * projection.m[0][1] +
            static_cast<double>(view.y) * projection.m[1][1] +
            static_cast<double>(view.z) * projection.m[2][1] + projection.m[3][1];
    out.w = static_cast<double>(view.x) * projection.m[0][3] +
            static_cast<double>(view.y) * projection.m[1][3] +
            static_cast<double>(view.z) * projection.m[2][3] + projection.m[3][3];
    return out;
}

struct Projected {
    float x = 0;      // pixels inside the viewport
    float y = 0;
    float w = 0;      // clip w
    bool inside = false;
};

Projected ProjectPoint(const Matrix& worldView, const Matrix& projection,
                       const Vec3& point, int width, int height) {
    const Clip clip = ProjectView(projection, TransformPoint(worldView, point));
    Projected out;
    out.w = static_cast<float>(clip.w);
    if (clip.w == 0.0) return out;
    const double ndcX = clip.x / clip.w;
    const double ndcY = clip.y / clip.w;
    out.x = static_cast<float>((ndcX * 0.5 + 0.5) * width);
    out.y = static_cast<float>((1.0 - (ndcY * 0.5 + 0.5)) * height);
    out.inside = out.w > 0.0f && ndcX >= -1.0 && ndcX <= 1.0 &&
                 ndcY >= -1.0 && ndcY <= 1.0;
    return out;
}

std::string FormatVec3(const Vec3& value) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "[%.4f,%.4f,%.4f]", value.x, value.y,
                  value.z);
    return buffer;
}

#ifdef MIKUMIKUSTUDIO_PERF
const char* const kPhaseNames[] = {
    "frame", "scene", "vertex_update", "morph", "skin", "transparent_sort",
    "materials", "physics", "present", "sort_lock", "sort_cpu", "pose",
    "physics_sync", "physics_readback", "physics_step", "bone_morph",
    "accessories", "edges", "shadow_map", "ground_shadow", "resource_guard",
    "clear", "composition", "overlays", "end_scene", "validation_release",
    "screen_capture", "begin_scene", "effect_api", "shadow_bounds", "shadow_bounds_lock"};
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == perf::Count,
              "phase name table must match mikumikustudio::perf::Phase");
#endif

}  // namespace

// Same driver for before/after builds.  No scene saves, message-loop FPS cap,
// heap scans. The separate MME target uses the same driver. Measures forced editor frames, not animation playback
// FPS.  See docs/development/benchmarks.md for the CLI contract and JSON schema.
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return kExitUsage;
    Options options;
    const int parse = ParseOptions(argc, argv, options);
    if (parse != 0) return parse;
    skinning::SetPreference({wcscmp(options.processor, L"cpu") == 0 ? skinning::Mode::Cpu : skinning::Mode::Automatic});
    const bool native = options.native;
    const bool throughput = options.throughput;
    const int copies = options.copies;
    const int warmup = options.warmup;
    const int samples = options.samples;
    skipMissingModels = options.loadSkipMissing || options.throughputSkipMissing;
    fileio::bufferedReads = !options.unbuffered;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto* app = new MMDApp();
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    std::puts("initialize");
    HHOOK hook=SetWindowsHookExW(WH_CBT,BenchmarkDialog,nullptr,GetCurrentThreadId());
    // Marked so the hook turns an init-phase dialog into exit code 3.
    initializingApp=true;
    const bool initOk=InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE);
    initializingApp=false;
    if (!initOk) return kExitInit;
    ShowWindow(app->MainWindow(), SW_HIDE);
#ifdef REQUIRE_MME
    if (!GetModuleHandleW(L"MMEffect.dll")) return kExitMme;
    std::puts("MME loaded");
#else
    if (GetModuleHandleW(L"MMEffect.dll")) return kExitMme;
#endif
    std::printf("scene_plugin MMV4MMS_loaded=%u\n", GetModuleHandleW(L"MMV4MMS.dll") ? 1u : 0u);

    constexpr UINT sortingMenuId = 303;
    HWND quality = GetDlgItem(app->MainWindow(), panel::kSelfShadowCheckbox);
    const bool initialQuality = SendMessageW(quality, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const unsigned initialShadowWidth = app->Renderer()->renderTargetWidth;
    const UINT initialSortState = GetMenuState(GetMenu(app->MainWindow()), sortingMenuId, MF_BYCOMMAND);
    const auto setQuality = [&](bool enabled) {
        SendMessageW(quality, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        CommandDispatch(quality, MAKEWPARAM(panel::kSelfShadowCheckbox, BN_CLICKED));
    };
    if (options.verifyViewToggles) {
        if (!initialQuality || initialShadowWidth != 4096 || app->AutoTransparentMaterialSortingEnabled()) return 40;
        const auto oldLanguage = app->state.englishUI;
        for (int language : {2, 1, 0, 2}) {
            app->state.englishUI = language;
            RefreshMenuLanguage(app);
            HMENU view = GetSubMenu(GetMenu(app->MainWindow()), 2);
            unsigned count = 0;
            for (int i = 0; i < GetMenuItemCount(view); ++i)
                if (GetMenuItemID(view, i) == sortingMenuId) ++count;
            wchar_t caption[128]{};
            GetMenuStringW(view, sortingMenuId, caption, 128, MF_BYCOMMAND);
            const wchar_t* expected = language == 2 ? L"开启自动透明材质排序" : language == 1 ? L"enable automatic transparent material sorting" : L"透明材質の自動ソートを有効化";
            if (initialSortState != UINT(-1)) {
                if (count != 1 || wcscmp(caption, expected) != 0 || (GetMenuState(view, sortingMenuId, MF_BYCOMMAND) & MF_CHECKED)) return 41;
            } else {
                GetMenuStringW(view, 32004, caption, 128, MF_BYCOMMAND);
                const wchar_t* limitTitle = language == 2 ? L"透明排序上限" : language == 1 ? L"Transparent sort limit" : L"透明ソート上限";
                if (count != 0 || wcscmp(caption, limitTitle) != 0 ||
                    !(GetMenuState(view, 308, MF_BYCOMMAND) & MF_CHECKED)) return 41;
            }
        }
        app->state.englishUI = oldLanguage;
        RefreshMenuLanguage(app);
        // Sorting correctness needs an explicit enabled tier now that startup is off.
        CommandDispatch(nullptr, MAKEWPARAM(305, 0));
        setQuality(false);
        if (app->Renderer()->renderTargetWidth != 2048) return 42;
        setQuality(true);
        if (app->Renderer()->renderTargetWidth != 4096) return 43;
    }
    if (options.fineShadow >= 0) setQuality(options.fineShadow != 0);
    shadow_filter::SetModelShadowsEnabledForTests(options.fixedModelShadows);
    if (options.adaptiveShadow >= 0) {
        HWND adaptive = GetDlgItem(app->MainWindow(), panel::kAdaptiveShadowCheckbox);
        SendMessageW(adaptive, BM_SETCHECK, options.adaptiveShadow ? BST_CHECKED : BST_UNCHECKED, 0);
        CommandDispatch(adaptive, MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED));
        if (app->AdaptiveShadowResolution() != (options.adaptiveShadow != 0)) return 44;
    }
    if (options.dynamicPrecision >= 0) {
        if (!app->AdaptiveShadowResolution()) return 45;
        const auto light = app->SceneLight();
        HWND slider = GetDlgItem(app->MainWindow(), panel::kSelfShadowRangeSlider);
        SendMessageW(slider, TBM_SETPOS, TRUE, options.dynamicPrecision);
        SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(slider));
        SyncSelfShadowControls(app);
        if (ShadowSliderPosition(app) != options.dynamicPrecision ||
            std::abs(app->ShadowDistance() - (10000.f-options.dynamicPrecision)/100000.f) > 1e-7f ||
            std::memcmp(&light, &app->SceneLight(), sizeof(light)) != 0 ||
            SendMessageW(slider, TBM_GETPOS, 0, 0) != options.dynamicPrecision) return 46;
    }
    std::printf("view_startup quality=%u shadow_width=%u sort_state=%u\n", initialQuality, initialShadowWidth, initialSortState);

    // ---- explicit D3D viewport ------------------------------------------
    // The back buffer grows to the requested size through the same reset the
    // "render to picture file" command uses (command 276), then the render
    // size, the projection aspect and the viewport are installed explicitly
    // and read back.  RefreshMainWindowViewport derives both the viewport and
    // wrapper->aspectRatio from the window client rect, so the aspect has to
    // be rewritten here or the model pass would keep the window's aspect.
    int requestedWidth = 0;
    int requestedHeight = 0;
    if (options.viewportGiven) {
        requestedWidth = options.viewportWidth;
        requestedHeight = options.viewportHeight;
        D3DRenderer* renderer = app->Renderer();
        if (renderer != nullptr && renderer->device != nullptr) {
            app->RenderWidth() = requestedWidth;
            app->RenderHeight() = requestedHeight;
            if (renderer->screenWidth < requestedWidth ||
                renderer->screenHeight < requestedHeight) {
                renderer->presentParameters.BackBufferWidth = requestedWidth;
                renderer->presentParameters.BackBufferHeight = requestedHeight;
                renderer->screenWidth = requestedWidth;
                renderer->screenHeight = requestedHeight;
                if (!PostDeviceReset(app)) return kExitViewport;
            }
            renderer->aspectRatio =
                static_cast<float>(requestedWidth) /
                static_cast<float>(requestedHeight);
            D3DVIEWPORT9 requested{};
            requested.X = 0;
            requested.Y = 0;
            requested.Width = static_cast<DWORD>(requestedWidth);
            requested.Height = static_cast<DWORD>(requestedHeight);
            requested.MinZ = 0.0f;
            requested.MaxZ = 1.0f;
            renderer->device->SetViewport(&requested);
        }
    }
    wcscpy_s(app->EnvFileName(), 256, options.target);
    std::puts("load scene");
    const auto loadStart=std::chrono::steady_clock::now();
    fileio::readStatistics={};
    if (options.modelFile)
        for (int copy = 0; copy < copies; ++copy)
            LoadModelFile(app, options.target, false);
    else
        LoadSceneFile();
    if (options.extraModel) LoadModelFile(app, options.extraModel, false);
    const auto pmmReport=save_compatibility::InspectPmm(*app);
    std::printf("pmm_compatibility new_header=%d names_extension=%d legacy_copy=%d invalid=%d\n",
        pmmReport.needsNewPmm,pmmReport.namesNeedExtension,pmmReport.canCopyLegacy,pmmReport.invalidData);
    const double loadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-loadStart).count();
    // Reinstall the same editor state before each frame: UI refreshes can
    // restore a keyframe's shadow mode or the selected model's bone mode.
    const auto installSelfShadow = [&] {
        if (options.groundShadow >= 0) app->GroundShadowEnabled() = options.groundShadow;
        if (options.shadowRange >= 0 && options.dynamicPrecision < 0)
            app->ShadowDistance() = (10000.f - options.shadowRange) / 100000.f;
        if (options.lightGiven) {
            std::copy_n(options.light, 3, app->LightDirection());
            app->SceneLight().Direction = {options.light[0], options.light[1], options.light[2]};
        }
        if (!options.selfShadowGiven) return;
        app->EditMode() = ViewportEditMode::Camera;
        app->state.selfShadowEnabled = options.selfShadowMode != 0;
        app->SelfShadowMode() = options.selfShadowMode;
    };
    installSelfShadow();
    if (options.sortPrecision >= 0) {
        CommandDispatch(nullptr, MAKEWPARAM(304 + options.sortPrecision, 0));
        std::printf("requested_sort_precision=%d\n", options.sortPrecision);
    }
    if(options.sortCrossObjects>=0 && app->TransparentSortCrossObjects()!=(options.sortCrossObjects!=0))
        CommandDispatch(nullptr,MAKEWPARAM(kTransparentSortCrossObjectsMenuId,0));
    if (options.sortEnabled >= 0 && app->AutoTransparentMaterialSortingEnabled() != (options.sortEnabled != 0))
        CommandDispatch(nullptr, MAKEWPARAM(303, 0));
    if (options.antialias >= 0) {
        // Exercise the same menu action as the editor, including its check mark.
        const bool checked = (GetMenuState(GetMenu(app->MainWindow()), 277, MF_BYCOMMAND) & MF_CHECKED) != 0;
        if (checked != (options.antialias != 0)) CommandDispatch(nullptr, MAKEWPARAM(277, 0));
    }
    if (options.outlines == 0)
        for (int slot = 0; slot < kModelSlotCount; ++slot)
            if (auto* model = app->ModelSlot(slot)) mdl::Mdl(model)->edgeScale = 0;
    benchmarkRendering = true;
    unsigned models=0, vertices=0, accessories=0, rigidBodies=0, joints=0;
    std::vector<Copy> copyRecords;
    for (int i=0; i<kModelSlotCount; ++i) if (auto* m=app->ModelSlot(i)) {
        ++models; vertices += mdl::Mdl(m)->vertexCount;
        // rigidCount (model_layout.hpp:12752) is the physics rigid-body count
        // read by pmx_load.cpp:1523; the similarly named rigidBodyCount holds
        // the display-bone frame entry total (pmx_load.cpp:1477 and the bone
        // frame records below it), so it must not be reported as physics.
        rigidBodies += mdl::Mdl(m)->rigidCount;
        joints += mdl::Mdl(m)->jointCount;
        Copy record;
        record.slot = i;
        record.model = m;
        copyRecords.push_back(record);
        if(options.loadOnly)std::printf("model_slot=%d vertices=%u bones=%u morphs=%d complete=%u\n",i,mdl::Mdl(m)->vertexCount,mdl::Mdl(m)->boneCount,mdl::Mdl(m)->morphCount,mdl::Mdl(m)->loadComplete);
    }
    for (int i=0; i<255; ++i) if (app->AccessorySlot(i)) ++accessories;
    std::printf("load_ms=%.3f dialogs=%u models=%u vertices=%u accessories=%u\n",loadMs,loadDialogs,models,vertices,accessories);
    const auto preparation=texture_prepare_cache::GetParallelStatistics();
    std::printf("TEXTURE_PARALLEL staged=%llu decoded=%llu used=%llu disk_ready=%llu failures=%llu\n",
        preparation.staged,preparation.decoded,preparation.used,preparation.diskReady,preparation.failures);
    // Recorded so a run can state whether the scene has any physics at all:
    // the sample model used for the layout checks has none.
    std::printf("physics=rigid_bodies=%u joints=%u\n",rigidBodies,joints);
    // A requested copy that never reached a slot is a hard failure: a short
    // scene would silently report the cost of fewer models.
    if (options.copiesGiven && static_cast<int>(models) < copies) {
        std::printf("copies_loaded=%u copies_requested=%d\n", models, copies);
        ShutdownCleanup(app); delete app; g_Block=nullptr;
        return kExitCopiesMissing;
    }
    if(options.loadOnly){
        FILE* dump=nullptr;if(options.output&&(_wfopen_s(&dump,options.output,L"wb")||!dump))return kExitDigestOpen;
        std::uint64_t hash=14695981039346656037ull;
        const auto bytes=[&](const void* data,size_t count){if(dump&&count)fwrite(data,1,count,dump);auto* p=static_cast<const unsigned char*>(data);for(size_t i=0;i<count;++i){hash^=p[i];hash*=1099511628211ull;}};
        float world[16]{};world[0]=world[5]=world[10]=world[15]=1;
        for(int i=0;i<kModelSlotCount;++i)if(auto* m=app->ModelSlot(i)){
            GpuModelInput input;
            if(!PrepareGpuModelInput(app,m,world,input)){std::puts("model digest unsupported");return kExitDigest;}
            bytes(input.vertices.data(),input.vertices.size()*sizeof(gpu::Vertex));
            bytes(input.bones.data(),input.bones.size()*sizeof(gpu::Bone));
            bytes(input.materials.data(),input.materials.size()*sizeof(gpu::Material));
            const auto& record=*mdl::Mdl(m);
            for(unsigned j=0;j<record.indexCount;++j){unsigned index=record.physicsMode==2?mdl::PmxIndices(m)[j]:mdl::Indices(m)[2*j];bytes(&index,sizeof index);}
        }
        const auto& stats=fileio::readStatistics;
        std::printf("buffered=%u read_requests=%llu system_reads=%llu bytes=%llu model_digest=%016llx\n",!options.unbuffered,stats.requests,stats.systemReads,stats.bytes,hash);
        if(dump)fclose(dump);
    }
    if(options.loadOnly||loadDialogs!=skippedModels||!models){ShutdownCleanup(app);delete app;g_Block=nullptr;return loadDialogs!=skippedModels||!models?kExitScene:0;}
    auto* device=app->Renderer()->device;
    D3DDEVICE_CREATION_PARAMETERS creation{};
    device->GetCreationParameters(&creation);
    IDirect3D9* d3d=nullptr; device->GetDirect3D(&d3d);
    D3DADAPTER_IDENTIFIER9 adapter{};
    d3d->GetAdapterIdentifier(creation.AdapterOrdinal,0,&adapter); d3d->Release();
    std::printf("gpu=%s adapter=%u type=%u flags=%lu models=%u vertices=%u accessories=%u\n",
        adapter.Description,creation.AdapterOrdinal,unsigned(creation.DeviceType),
        creation.BehaviorFlags,models,vertices,accessories);
    IDirect3DQuery9* query=nullptr;
    if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT,&query))) return kExitQuery;
    const auto flush=[&] {
        HRESULT issued = query->Issue(D3DISSUE_END);
        if (FAILED(issued)) {
            std::printf("query Issue failed: 0x%08lX\n", static_cast<unsigned long>(issued));
            return false;
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        HRESULT result;
        while ((result=query->GetData(nullptr,0,D3DGETDATA_FLUSH))==S_FALSE) {
            if (std::chrono::steady_clock::now()>=deadline) {
                std::puts("query GetData timed out");
                return false;
            }
            Sleep(0);
        }
        if (result != S_OK) std::printf("query GetData failed: 0x%08lX\n", static_cast<unsigned long>(result));
        return result==S_OK;
    };
    float skinWorld[16]{};
    skinWorld[0]=skinWorld[5]=skinWorld[10]=skinWorld[15]=1.f;
    if (options.skinOnly) {
        // Deterministic nonidentity transforms exercise both weighted bones.
        for (int slot=0; slot<kModelSlotCount; ++slot) if (auto* m=app->ModelSlot(slot))
            for (unsigned b=0; b<mdl::Mdl(m)->boneCount; ++b) {
                auto* matrix=mdl::Bones(m)[b].matInit;
                std::memcpy(matrix,skinWorld,sizeof skinWorld);
                matrix[0]=.8f; matrix[1]=.6f; matrix[4]=-.6f; matrix[5]=.8f;
                matrix[12]=float(b)*.125f; matrix[13]=float(b)*-.25f;
            }
    }
    EnableD3D12Native(native, native && options.output != nullptr);

    // ---- scene layout ----------------------------------------------------
    // An explicit --copies or --layout turns on the fixed benchmark scene:
    // one bounds measurement, fixed offsets that are re-applied to every
    // frame the driver renders, and one fixed overhead viewpoint.  A single
    // copy uses the same path so its framing is the same as the multi-copy
    // runs.  Without either option the historical driver is untouched.
    Bounds bounds;
    std::vector<Vec3> centers;
    std::vector<Vec3> placements(copyRecords.size());
    const bool layoutRequested =
        options.modelFile && (options.copiesGiven || options.layoutGiven) &&
        !options.skinOnly;
    if (layoutRequested) {
        bounds = ModelBounds(app, copyRecords[0].model);
        if (!bounds.valid) {
            std::puts("model bounds unsupported");
            ShutdownCleanup(app); delete app; g_Block=nullptr;
            return kExitVisibility;
        }
        centers = LayoutCenters(options.layout, static_cast<int>(copyRecords.size()), bounds);
        for (std::size_t index = 0; index < copyRecords.size(); ++index) {
            Copy& copy = copyRecords[index];
            const unsigned boneCount = mdl::Mdl(copy.model)->boneCount;
            copy.lastTranslation.assign(static_cast<std::size_t>(boneCount) * 3,
                                        0.0f);
            copy.translationKnown.assign(boneCount, 0);
            // The bounding-box centre of the copy lands on the layout point,
            // so the framing maths and the reported offsets agree.
            const float center[3] = {
                (bounds.max[0] + bounds.min[0]) * 0.5f,
                (bounds.max[1] + bounds.min[1]) * 0.5f,
                (bounds.max[2] + bounds.min[2]) * 0.5f};
            copy.offset[0] = centers[index].x - center[0];
            copy.offset[1] = centers[index].y - center[1];
            copy.offset[2] = centers[index].z - center[2];
            placements[index] = centers[index];
        }
        // A flat model rendered from the default eye-level camera is a line.
        // Pitch -90 degrees maps world +Y onto the view -Z axis, so the
        // camera looks down from above; -75 keeps a little perspective and
        // makes the world X axis the screen horizontal.
        app->CameraYaw() = 0.0f;
        app->CameraPitch() = -75.0f * 3.14159265358979323846f / 180.0f;
        app->CameraRoll() = 0.0f;
    }
    if(options.noPreview) {app->DepthTextureCompositionEnabled()=0;app->SelfShadowCompositionEnabled()=0;}
    if(options.shadowCameraSweep) {
        app->EditMode()=ViewportEditMode::Camera;
        app->ViewOffsetX()=app->ViewOffsetY()=0;
    }
    if (options.cameraGiven) {
        if(options.cameraPanel)app->EditMode()=ViewportEditMode::Camera;
        app->CameraPositionX() = options.camera[0];
        app->CameraPositionY() = options.camera[1];
        app->CameraPositionZ() = options.camera[2];
        app->ViewOffsetX() = options.camera[0];
        app->ViewOffsetY() = options.camera[1];
        if(options.cameraPanel)app->ViewOffsetX()=app->ViewOffsetY()=0;
        app->CameraDistance() = options.camera[3];
        constexpr float radians = 3.14159265358979323846f / 180.0f;
        app->CameraYaw() = options.camera[4] * radians;
        app->CameraPitch() = options.camera[5] * radians;
        app->CameraRoll() = options.camera[6] * radians;
    }
    const float halfExtent[3] = {
        (bounds.max[0] - bounds.min[0]) * 0.5f,
        (bounds.max[1] - bounds.min[1]) * 0.5f,
        (bounds.max[2] - bounds.min[2]) * 0.5f};

    std::vector<double> times;
    std::chrono::steady_clock::time_point batchStart;
    int actualWidth = 0, actualHeight = 0;
    unsigned viewportChecks = 0;
    unsigned viewportMismatches = 0;
    // Reinstalled before every render: a device reset inside the frame would
    // otherwise put the window-derived viewport and aspect back.
    const auto installViewport = [&] {
        if (requestedWidth == 0) return;
        // Presentation margins use the host rectangle, not the D3D viewport.
        // Keep both in sync so the hidden window's sidebar margins cannot
        // paint a black rectangle over an explicit benchmark viewport.
        app->ViewportRect() = {0, 0, requestedWidth, requestedHeight};
        D3DRenderer* renderer = app->Renderer();
        if (renderer != nullptr)
            renderer->aspectRatio = static_cast<float>(requestedWidth) /
                                    static_cast<float>(requestedHeight);
        D3DVIEWPORT9 requested{};
        requested.X = 0;
        requested.Y = 0;
        requested.Width = static_cast<DWORD>(requestedWidth);
        requested.Height = static_cast<DWORD>(requestedHeight);
        requested.MinZ = 0.0f;
        requested.MaxZ = 1.0f;
        device->SetViewport(&requested);
    };
    const auto runFrame = [&]() -> double {
        // Service the hidden window as the editor's main loop does. A paint
        // handler calls FrameDriver itself, so validate paints without dispatch
        // to keep every rendered frame explicitly counted by this driver.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return -1.0;
            if (message.message == WM_PAINT) ValidateRect(message.hwnd, nullptr);
            else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        const HRESULT cooperative = device->TestCooperativeLevel();
        if (cooperative != D3D_OK) {
            std::printf("device unavailable before frame: 0x%08lX\n", static_cast<unsigned long>(cooperative));
            return -1.0;
        }
        ApplyCopyOffsets(copyRecords);
        app->CameraYaw() += options.orbitDegrees * 3.14159265358979323846f / 180.0f;
        installViewport();
        installSelfShadow();
        app->DeltaTime()=1.f/30.f;
        app->state.messageSeen=10;
        auto start=std::chrono::steady_clock::now();
        if (options.skinOnly) {
            for (int slot=0; slot<kModelSlotCount; ++slot)
                if (auto* m=app->ModelSlot(slot)) UpdateModelVertexBuffers(app,m,skinWorld);
        } else FrameDriver(app);
        // Read the viewport back after the model pass ran: anything the frame
        // changes without restoring would show up here.
        if (requestedWidth != 0) {
            D3DVIEWPORT9 after{};
            if (device->GetViewport(&after) == D3D_OK) {
                ++viewportChecks;
                if (static_cast<int>(after.Width) != requestedWidth ||
                    static_cast<int>(after.Height) != requestedHeight)
                    ++viewportMismatches;
            }
        }
        if (!throughput && !options.skinOnly && !flush()) return -1.0;
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };

    // ---- framing ---------------------------------------------------------
    // The layout must be entirely visible.  The eye distance is solved from
    // the projection the frame installs and then refined against the real
    // world/view/projection matrices, so a changed aspect or FOV still frames
    // the whole scene instead of trusting a hardcoded distance.
    bool framed = true;
    Vec3 layoutCenter;
    bool haveFrameMatrices = false;
    Matrix worldMatrix, viewMatrix, projectionMatrix;
    const auto readMatrices = [&] {
        worldMatrix = ReadTransform(device, D3DTS_WORLD);
        viewMatrix = ReadTransform(device, D3DTS_VIEW);
        projectionMatrix = ReadTransform(device, D3DTS_PROJECTION);
        haveFrameMatrices = true;
    };
    const auto maxNdcOverflow = [&]() -> double {
        if (!haveFrameMatrices || placements.empty()) return 0.0;
        const Matrix worldView = Multiply(worldMatrix, viewMatrix);
        double worst = 0.0;
        for (const Vec3& point : placements)
            for (int corner = 0; corner < 8; ++corner) {
                Vec3 sample;
                sample.x = point.x + ((corner & 1) ? halfExtent[0] : -halfExtent[0]);
                sample.y = point.y + ((corner & 2) ? halfExtent[1] : -halfExtent[1]);
                sample.z = point.z + ((corner & 4) ? halfExtent[2] : -halfExtent[2]);
                const Clip clip = ProjectView(projectionMatrix,
                                              TransformPoint(worldView, sample));
                if (clip.w <= 1e-6) { worst = 1e9; continue; }
                worst = std::max(worst, std::max(std::fabs(clip.x / clip.w),
                                                 std::fabs(clip.y / clip.w)));
            }
        return worst;
    };
    if (layoutRequested) {
        layoutCenter = {0.0f, 0.0f, 0.0f};
        for (const Vec3& point : placements) {
            layoutCenter.x += point.x / static_cast<float>(placements.size());
            layoutCenter.y += point.y / static_cast<float>(placements.size());
            layoutCenter.z += point.z / static_cast<float>(placements.size());
        }
        // Anchor the camera on the layout centre; the free model view and the
        // camera panel both read these fields.
        app->CameraPositionX() = layoutCenter.x;
        app->CameraPositionY() = layoutCenter.y;
        app->CameraPositionZ() = layoutCenter.z;
        app->ViewOffsetX() = 0.0f;
        app->ViewOffsetY() = 0.0f;
        if (runFrame() < 0.0) { query->Release(); return kExitFlush; }
        readMatrices();
        // NDC extent is close to inversely proportional to the eye distance,
        // so one ratio step lands near the target; the loop only matters when
        // the projection or the aspect makes the first guess wrong.
        const double target = 0.85;  // fraction of the NDC cube the layout fills
        const double layoutSpan = std::max(
            {static_cast<double>(halfExtent[0]), static_cast<double>(halfExtent[1]),
             static_cast<double>(halfExtent[2]), 1.0});
        const double minDistance = layoutSpan * 0.75;
        const double maxDistance = layoutSpan * 1000.0;
        for (int iteration = 0; iteration < 12; ++iteration) {
            const double overflow = maxNdcOverflow();
            if (overflow >= 1e8) {
                // The eye sits behind or inside the layout: back off first.
                const double current = std::max(
                    static_cast<double>(layoutCenter.z - app->CameraDistance()),
                    minDistance);
                app->CameraDistance() = static_cast<float>(
                    layoutCenter.z - std::min(current * 2.0, maxDistance));
                if (runFrame() < 0.0) { query->Release(); return kExitFlush; }
                readMatrices();
                continue;
            }
            if (overflow <= 0.92 && overflow >= 0.70) break;
            const double current = std::max(
                static_cast<double>(layoutCenter.z - app->CameraDistance()),
                1e-3);
            double wanted = current * overflow / target;
            if (wanted < minDistance) wanted = minDistance;
            if (wanted > maxDistance) wanted = maxDistance;
            if (fabs(wanted - current) < current * 0.01) break;
            app->CameraDistance() = static_cast<float>(layoutCenter.z - wanted);
            if (runFrame() < 0.0) { query->Release(); return kExitFlush; }
            readMatrices();
        }
        if (maxNdcOverflow() > 1.0) framed = false;
        std::printf("layout=%s copies=%zu camera_pos=(%.3f,%.3f,%.3f) camera_distance=%.3f layout_ndc=%.3f framed=%u\n",
                    options.layout == Layout::Spread ? "spread" : "overlap",
                    copyRecords.size(), app->CameraPositionX(),
                    app->CameraPositionY(), app->CameraPositionZ(),
                    app->CameraDistance(), maxNdcOverflow(), framed ? 1u : 0u);
    }

    bool viewTogglesVerified = false;
    if (options.verifyViewToggles) {
        if (runFrame() < 0.0 || !flush()) return kExitFlush;
        const auto indexDifferences = [&]() -> long long {
            long long different = 0;
            for (const auto& copy : copyRecords) {
                auto* m = copy.model;
                auto& state = *mdl::Mdl(m);
                auto* buffer = mdl::ResourceAs<IDirect3DIndexBuffer9>(state.indexBuffer);
                D3DINDEXBUFFER_DESC desc{};
                void* data = nullptr;
                if (!buffer || FAILED(buffer->GetDesc(&desc)) || FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) return -1;
                for (unsigned j = 0; j < state.indexCount; ++j) {
                    const unsigned expected = state.physicsMode == 2 ? mdl::PmxIndices(m)[j] : mdl::Indices(m)[2*j];
                    const unsigned actual = desc.Format == D3DFMT_INDEX32 ? static_cast<unsigned*>(data)[j] : static_cast<unsigned short*>(data)[j];
                    different += actual != expected;
                }
                buffer->Unlock();
            }
            return different;
        };
        const auto savedSortPrecision = app->TransparentMaterialSortPrecision();
        const long long sortedDifferences = indexDifferences();
        CommandDispatch(nullptr, MAKEWPARAM(sortingMenuId, 0));
        RefreshMenuLanguage(app);
        if (app->AutoTransparentMaterialSortingEnabled()) return 44;
        if (runFrame() < 0.0 || !flush()) return kExitFlush;
        const long long restoredDifferences = indexDifferences();
        if (restoredDifferences != 0) return 45;
        CommandDispatch(nullptr, MAKEWPARAM(sortingMenuId, 0));
        CommandDispatch(nullptr, MAKEWPARAM(304 + static_cast<int>(savedSortPrecision), 0));
        if (!app->AutoTransparentMaterialSortingEnabled() || app->TransparentMaterialSortPrecision() != savedSortPrecision) return 46;
        if (runFrame() < 0.0 || !flush()) return kExitFlush;
        const long long resortedDifferences = indexDifferences();
        // Global sorting uses private geometry, so the original index buffer
        // can remain unchanged while sorting is enabled. Pixel tests verify order.
        if (sortedDifferences < 0 || resortedDifferences < 0) return 47;
        std::printf("view_toggle_verified sorted=%lld restored=%lld resorted=%lld\n", sortedDifferences, restoredDifferences, resortedDifferences);
        viewTogglesVerified = true;
    }

    // Preserve the historical JSON fields for comparison tools. This build
    // has no MMV runtime or MMV timing samples.
    constexpr bool mmvStagesAvailable=false;
    constexpr bool mmvLoaded=false;
    constexpr double mmvSnapshotSumMs=0,mmvInputDemandSumMs=0,mmvEvaluationSumMs=0,mmvUploadSumMs=0;
    constexpr long long mmvSnapshotSamples=0,mmvEvaluationSamples=0,mmvUploadSamples=0;
    constexpr long long mmvAcceptedFrames=0,mmvFailedFrames=0;

    if (options.playbackSeconds) {
        if (!options.json || options.native || options.skinOnly || options.loadOnly ||
            options.copiesGiven || options.layoutGiven || options.output || options.orbitDegrees != 0)
            return kExitUsage;
        const int result = playback_benchmark::Run(app, options.playbackSeconds, options.json,
            [&] { installViewport(); installSelfShadow(); });
        const auto cacheStats = model_vertex_cache::GetStats();
        PrintTransparentCache();
        std::printf("MODEL_VERTEX_CACHE hits=%llu misses=%llu stores=%llu outline_hits=%llu bytes=%zu cpu_bytes=%zu\n",
            cacheStats.hits, cacheStats.misses, cacheStats.stores, cacheStats.outlineHits, cacheStats.bytes, cacheStats.cpuBytes);
        std::printf("SHADOW_BOUNDS_CACHE hits=%llu misses=%llu stores=%llu\n",
            cacheStats.shadowBoundsHits, cacheStats.shadowBoundsMisses, cacheStats.shadowBoundsStores);
        query->Release();
        ResetAppState(app); ShutdownCleanup(app); delete app; g_Block = nullptr;
        return result;
    }

    // ---- measurement loop ------------------------------------------------
    for (int i=0; i<warmup+samples; ++i) {
        if (i==warmup) {
            if (!flush() || (native && !WaitD3D12Native())) return kExitFlush;
            if (options.requireMmv && !mmvLoaded) {
                std::fputs("--require-mmv: MMV is not loaded at sampling start\n",
                           stderr);
                return kExitMmvRequired;
            }
            if (options.forbidMmv && mmvLoaded) {
                std::fputs("--forbid-mmv: MMV is loaded at sampling start\n",
                           stderr);
                return kExitMmvForbidden;
            }
#ifdef MIKUMIKUSTUDIO_PERF
            perf::Reset();
#endif
            std::printf("warmup_complete frames=%d; starting measured frames\n", warmup);
            batchStart=std::chrono::steady_clock::now();
        }
        const double ms = runFrame();
        if (ms < 0.0) return kExitFlush;
        if (native && strcmp(D3D12NativeStatus(), "D3D12 native active") != 0) {
            std::printf("native failed: %s\n", D3D12NativeStatus());
            ShutdownCleanup(app);
            return kExitNative;
        }
        if (i>=warmup) {
            times.push_back(ms);
        }
    }
    if (!flush() || (native && !WaitD3D12Native())) return kExitFlush;
    const double batchMs=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-batchStart).count();
    std::printf("mode=%s drained_batch_ms_per_frame=%.4f\n",
        native ? "native" : options.skinOnly ? "skin-only" : (throughput ? "throughput" : "serial"), batchMs/times.size());
    // MMV stage means are summed stage milliseconds divided by the number of
    // counted frames: CPU stage costs per frame, not FPS and not GPU time.
    std::printf("mmv loaded=%u sample_count=%zu input_demand_mean_ms=%.4f snapshot_mean_ms=%.4f evaluation_mean_ms=%.4f upload_mean_ms=%.4f accepted_frames=%lld failed_frames=%lld\n",
        mmvLoaded ? 1u : 0u, mmvStagesAvailable ? times.size() : 0, mmvInputDemandSumMs / times.size(),
        mmvSnapshotSumMs / times.size(), mmvEvaluationSumMs / times.size(), mmvUploadSumMs / times.size(),
        mmvAcceptedFrames, mmvFailedFrames);
#ifdef MIKUMIKUSTUDIO_PERF
    perf::Report(static_cast<unsigned>(times.size()));
#endif
    std::printf("automatic_calibrations=%llu cpu_updates=%llu\n",
        static_cast<unsigned long long>(skinning::GetStatus().automaticCalibrations),
        static_cast<unsigned long long>(skinning::GetStatus().cpuUpdates));
    const auto cacheStats = model_vertex_cache::GetStats();
    PrintTransparentCache();
    std::printf("MODEL_VERTEX_CACHE hits=%llu misses=%llu stores=%llu outline_hits=%llu bytes=%zu cpu_bytes=%zu\n",
        cacheStats.hits, cacheStats.misses, cacheStats.stores, cacheStats.outlineHits, cacheStats.bytes, cacheStats.cpuBytes);
    std::printf("SHADOW_BOUNDS_CACHE hits=%llu misses=%llu stores=%llu\n",
        cacheStats.shadowBoundsHits, cacheStats.shadowBoundsMisses, cacheStats.shadowBoundsStores);
    std::printf("skinning=%ls adapter=%ls gpu_updates=%llu cpu_fallbacks=%llu\n",
        options.processor, skinning::GetStatus().adapterName.c_str(),
        static_cast<unsigned long long>(skinning::GetStatus().gpuUpdates),
        static_cast<unsigned long long>(skinning::GetStatus().cpuFallbacks));
    if (wcscmp(options.processor, L"gpu") == 0 &&
        (skinning::GetStatus().gpuUpdates == 0 || skinning::GetStatus().cpuFallbacks != 0)) {
        std::puts("GPU skinning was required but unavailable or fell back to CPU");
        return kExitNative;
    }
    D3DVIEWPORT9 vp{}; device->GetViewport(&vp);
    actualWidth = static_cast<int>(vp.Width);
    actualHeight = static_cast<int>(vp.Height);
    double sum=0; for(double t:times) sum+=t;
    std::sort(times.begin(),times.end());
    std::printf("viewport=%ux%u samples=%zu mean_ms=%.4f median_ms=%.4f p95_ms=%.4f equivalent_fps=%.2f\n",
        vp.Width,vp.Height,times.size(),sum/times.size(),times[times.size()/2],
        times[times.size()*95/100],1000*times.size()/sum);
    if (options.viewportGiven)
        std::printf("viewport_requested=%dx%d viewport_actual=%ux%u\n",
                    requestedWidth, requestedHeight, vp.Width, vp.Height);

    // ---- model-pass viewport evidence ------------------------------------
    // The final viewport alone is only the UI state.  Three more facts are
    // recorded for the pass that drew the models: the device viewport read
    // back immediately after every frame, the render target the models were
    // drawn into, and the projection aspect the frame installed (it comes
    // from wrapper->aspectRatio, not from the render size).
    int renderTargetWidth = 0, renderTargetHeight = 0;
    double projectionAspect = 0.0;
    bool viewportEvidenceOk = true;
    if (requestedWidth != 0) {
        IDirect3DSurface9* surface = nullptr;
        if (SUCCEEDED(device->GetRenderTarget(0, &surface)) && surface != nullptr) {
            D3DSURFACE_DESC surfaceDesc{};
            if (SUCCEEDED(surface->GetDesc(&surfaceDesc))) {
                renderTargetWidth = static_cast<int>(surfaceDesc.Width);
                renderTargetHeight = static_cast<int>(surfaceDesc.Height);
            }
            surface->Release();
        }
        readMatrices();
        if (projectionMatrix.m[0][0] != 0.0f)
            projectionAspect = static_cast<double>(projectionMatrix.m[1][1]) /
                               static_cast<double>(projectionMatrix.m[0][0]);
        std::printf("viewport_model_pass requested=%dx%d render_target=%dx%d projection_aspect=%.5f frames_checked=%u mismatches=%u\n",
                    requestedWidth, requestedHeight, renderTargetWidth,
                    renderTargetHeight, projectionAspect, viewportChecks,
                    viewportMismatches);
        const double wantedAspect = static_cast<double>(requestedWidth) /
                                    static_cast<double>(requestedHeight);
        viewportEvidenceOk =
            viewportChecks > 0 && viewportMismatches == 0 &&
            renderTargetWidth >= requestedWidth &&
            renderTargetHeight >= requestedHeight &&
            std::fabs(projectionAspect - wantedAspect) <= wantedAspect * 0.01;
    }

    // ---- visibility and placement of every copy --------------------------
    std::vector<Projected> projected(copyRecords.size());
    std::vector<Vec3> vertexCenters(copyRecords.size());
    std::vector<unsigned> sampledCounts(copyRecords.size(), 0);
    std::vector<float> placementResidual(copyRecords.size() * 3, 0.0f);
    bool allCentersVisible = true;
    bool samplesComplete = true;
    bool placementVerified = true;
    float placementTolerance = 0.0f;
    float maxPlacementResidual = 0.0f;
    if (layoutRequested) {
        readMatrices();
        const Matrix worldView = Multiply(worldMatrix, viewMatrix);
        for (std::size_t index = 0; index < copyRecords.size(); ++index) {
            projected[index] = ProjectPoint(worldView, projectionMatrix,
                                            placements[index], actualWidth,
                                            actualHeight);
            if (!projected[index].inside) allCentersVisible = false;
            // Read the drawn geometry back: the sampled mean position of each
            // copy has to move with the reported offset, which proves the
            // copies are separate models instead of one model drawn twice.
            if (!SampledVertexCenter(copyRecords[index].model,
                                     vertexCenters[index],
                                     sampledCounts[index]) ||
                sampledCounts[index] == 0)
                samplesComplete = false;
            std::printf("copy=%zu slot=%d offset=%s placed_center=%s vertex_center=%s sampled=%u applications=%u screen=(%.1f,%.1f) visible=%u\n",
                        index, copyRecords[index].slot,
                        FormatVec3({copyRecords[index].offset[0],
                                    copyRecords[index].offset[1],
                                    copyRecords[index].offset[2]}).c_str(),
                        FormatVec3(placements[index]).c_str(),
                        FormatVec3(vertexCenters[index]).c_str(),
                        sampledCounts[index], copyRecords[index].applications,
                        projected[index].x,
                        projected[index].y, projected[index].inside ? 1u : 0u);
        }
        // Each copy is checked against the first copy per axis: the sampled
        // centroid difference has to equal the requested offset difference.
        // A mere "the copies are far enough apart" test would accept a copy
        // sitting at the wrong place, so the residual is compared with a
        // tolerance derived from float noise, not from the layout distance.
        const float modelSpan = std::max(
            {halfExtent[0] * 2.0f, halfExtent[1] * 2.0f, halfExtent[2] * 2.0f,
             1.0f});
        placementTolerance = std::max(1e-3f, modelSpan * 1e-4f);
        if (samplesComplete)
            for (std::size_t index = 1; index < copyRecords.size(); ++index) {
                const float expected[3] = {
                    copyRecords[index].offset[0] - copyRecords[0].offset[0],
                    copyRecords[index].offset[1] - copyRecords[0].offset[1],
                    copyRecords[index].offset[2] - copyRecords[0].offset[2]};
                const float actual[3] = {
                    vertexCenters[index].x - vertexCenters[0].x,
                    vertexCenters[index].y - vertexCenters[0].y,
                    vertexCenters[index].z - vertexCenters[0].z};
                for (int axis = 0; axis < 3; ++axis) {
                    const float residual = actual[axis] - expected[axis];
                    placementResidual[index * 3 + axis] = residual;
                    maxPlacementResidual =
                        std::max(maxPlacementResidual, fabsf(residual));
                    if (fabsf(residual) > placementTolerance)
                        placementVerified = false;
                }
            }
        std::printf("placement_check baseline_copy=0 copies=%zu tolerance=%.6f max_residual=%.6f verified=%u samples_ok=%u framed=%u\n",
                    copyRecords.size(), placementTolerance,
                    maxPlacementResidual, placementVerified ? 1u : 0u,
                    samplesComplete ? 1u : 0u, framed ? 1u : 0u);
    }
    if (options.viewportGiven && (static_cast<int>(vp.Width) != requestedWidth ||
                                  static_cast<int>(vp.Height) != requestedHeight)) {
        std::printf("viewport request not honoured\n");
        query->Release();
        ShutdownCleanup(app); delete app; g_Block=nullptr;
        return kExitViewport;
    }
    if (options.viewportGiven && !viewportEvidenceOk) {
        std::printf("model-pass viewport evidence failed\n");
        query->Release();
        ShutdownCleanup(app); delete app; g_Block=nullptr;
        return kExitViewport;
    }

    const auto renderFrozenPose = [&](int shadowMode = -1) {
        installViewport();installSelfShadow();
        if (shadowMode >= 0) {
            app->SelfShadowMode() = shadowMode;
            app->state.selfShadowEnabled = shadowMode != 0;
        }
        app->state.messageSeen=10;
        // FrameDriver would advance animation/physics and confound the probe.
        RenderFrameScene(app);
    };
    const auto saveFrozenImage = [&](const std::wstring& imagePath) {
        IDirect3DSurface9* image=nullptr;
        if(FAILED(device->GetRenderTarget(0,&image)) || !image)return false;
        D3DSURFACE_DESC imageDesc{};
        if(FAILED(image->GetDesc(&imageDesc))){image->Release();return false;}
        if(imageDesc.MultiSampleType!=D3DMULTISAMPLE_NONE) {
            IDirect3DSurface9* resolved=nullptr;
            if(FAILED(device->CreateRenderTarget(imageDesc.Width,imageDesc.Height,imageDesc.Format,
                    D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)) ||
               FAILED(device->StretchRect(image,nullptr,resolved,nullptr,D3DTEXF_NONE))) {
                if(resolved)resolved->Release();
                image->Release();return false;
            }
            image->Release();image=resolved;
        }
        RECT area{0,0,requestedWidth,requestedHeight};
        const HRESULT saved=d3dx::Get().saveSurfaceToFileW(imagePath.c_str(),3,image,nullptr,&area);
        image->Release();
        return SUCCEEDED(saved);
    };

    // A single loaded, frozen scene exposes temporal layer transitions. The
    // ordinary benchmark still reports its starting pose and measured frames;
    // this opt-in sequence writes separate evidence beside the requested image.
    if (options.shadowCameraSweep) {
        // Unshadowed sweeps provide the matched lighting reference used to
        // separate shadow changes from wall textures and camera magnification.
        if (app->SelfShadowMode()>0 && !UseEffectModelRenderer(app)) {
            std::fputs("shadow camera sweep requires the effect model renderer\n", stderr);
            return kExitUsage;
        }
        std::error_code pathError;
        const auto output = std::filesystem::absolute(std::filesystem::path(options.output), pathError);
        if (pathError) return kExitSurface;
        std::filesystem::create_directories(output.parent_path(), pathError);
        if (pathError) return kExitSurface;
        const auto stem = output.parent_path() / output.stem();
        constexpr float degrees = 180.f / 3.14159265358979323846f;
        constexpr float radians = 3.14159265358979323846f / 180.f;
        const float start[7]{app->CameraPositionX(),app->CameraPositionY(),app->CameraPositionZ(),
            app->CameraDistance(),app->CameraYaw()*degrees,app->CameraPitch()*degrees,app->CameraRoll()*degrees};
        const auto setCamera = [&](const float camera[7]) {
            app->CameraPositionX()=camera[0];app->CameraPositionY()=camera[1];app->CameraPositionZ()=camera[2];
            app->CameraDistance()=camera[3];app->CameraYaw()=camera[4]*radians;
            app->CameraPitch()=camera[5]*radians;app->CameraRoll()=camera[6]*radians;
            app->EditMode()=ViewportEditMode::Camera;
            app->ViewOffsetX()=app->ViewOffsetY()=0;
        };
        for(int settle=0;settle<3;++settle)renderFrozenPose();
        for(int index=0;index<options.sweepFrames;++index) {
            const double fraction=double(index)/double(options.sweepFrames-1);
            float camera[7]{};
            for(int field=0;field<7;++field)
                camera[field]=float((1.0-fraction)*start[field]+fraction*options.shadowCameraSweepEnd[field]);
            setCamera(camera);
            renderFrozenPose();
            if(!flush())return kExitFlush;
            wchar_t suffix[48]{};
            swprintf_s(suffix,L".sweep-%04d",index);
            const std::wstring frameStem=stem.wstring()+suffix;
            const std::wstring imagePath=frameStem+L".png",jsonPath=frameStem+L".json";
            if (!saveFrozenImage(imagePath)) return kExitSurface;
            std::string report;
            Json json(report);
            json.String("tool","frame_benchmark");
            json.String("schema","frame_benchmark/shadow-camera-sweep/1");
            json.String("scene_path",options.target);
            json.String("image",imagePath.c_str());
            json.Integer("index",index);json.Integer("frames",options.sweepFrames);
            json.Number("fraction",fraction);
            json.Bool("physics_advanced",false);
            const auto writeMatrix = [&](const char* name,const float* matrix) {
                json.Array(name);
                for(int row=0;row<4;++row) {
                    json.Item();
                    for(int column=0;column<4;++column) {
                        const char* keys[]{"x","y","z","w"};
                        json.NumberOrNull(keys[column],matrix[row*4+column],std::isfinite(matrix[row*4+column]));
                    }
                    json.End();
                }
                json.End();
            };
            json.Object("camera");
            const char* cameraKeys[]{"pos_x","pos_y","pos_z","distance","yaw_degrees","pitch_degrees","roll_degrees"};
            for(int field=0;field<7;++field)json.Number(cameraKeys[field],camera[field]);
            json.Number("fov_degrees",app->CameraFov());
            writeMatrix("world_view_projection",reinterpret_cast<const float*>(&app->WorldViewProjection()));
            json.End();
            json.Object("self_shadow");
            json.Integer("mode",app->SelfShadowMode());
            json.Integer("slider",ShadowSliderPosition(app));
            json.Bool("adaptive_resolution",app->AdaptiveShadowResolution());
            json.Bool("filter_enhanced",app->ShadowFilterEnhanced());
            const double referenceDensity=NativeShadowSamplingDensity(app->LightDirection(),app->ShadowDistance(),
                app->SelfShadowMode(),app->ShadowResolutionLimit());
            const auto planInputs=ReadShadowPlanInputs(app);
            json.Number("reference_density",referenceDensity,9);
            json.Integer("reference_resolution",app->ShadowResolutionLimit());
            WriteFixedModelShadows(json,app);
            json.Object("coverage");
            D3DSURFACE_DESC coverageDesc{};
            const bool coverageValid=app->Renderer()->shadowSurface &&
                SUCCEEDED(app->Renderer()->shadowSurface->GetDesc(&coverageDesc));
            const auto* coverageMatrix=reinterpret_cast<const float*>(&app->LightViewProjection());
            const double coverageScale=std::sqrt(double(coverageMatrix[0])*coverageMatrix[0]+
                double(coverageMatrix[4])*coverageMatrix[4]+double(coverageMatrix[8])*coverageMatrix[8]);
            json.Bool("valid",coverageValid);json.Integer("resolution",coverageDesc.Width);
            json.Integer("height",coverageDesc.Height);
            json.NumberOrNull("width",2/coverageScale,coverageScale>0 && std::isfinite(coverageScale));
            json.NumberOrNull("texels_per_world_unit",coverageDesc.Width*coverageScale*.5,
                coverageValid && std::isfinite(coverageScale));
            UINT sampleWidth=0,sampleHeight=0;
            const bool sampleValid=ShadowSampleDimensions(app,sampleWidth,sampleHeight);
            json.Integer("sample_width",sampleWidth);json.Integer("sample_height",sampleHeight);
            WriteShadowDensity(json,MeasureShadowDensity(coverageMatrix,sampleWidth,sampleHeight),referenceDensity,sampleValid,
                true,planInputs.valid?planInputs.coverageDensity:0);
            WriteCoverageFixedLimit(json,AssessCoverageFixedLimit(coverageMatrix,
                MeasureShadowDensity(coverageMatrix,sampleWidth,sampleHeight),referenceDensity,planInputs),planInputs);
            json.Null("near_depth");json.Null("far_depth");json.Null("blend_start");
            writeMatrix("projection",coverageMatrix);
            json.End();
            const char* layerNames[]{"middle","near","bridge"};
            for(unsigned layer=0;layer<3;++layer) {
                const auto info=shadow_filter::ReadDetailInfo(app,layer);
                const auto& matrix=info.projection;
                const double scale=std::sqrt(double(matrix[0])*matrix[0]+double(matrix[4])*matrix[4]+double(matrix[8])*matrix[8]);
                json.Object(layerNames[layer]);
                json.Bool("valid",info.valid);json.Integer("resolution",info.size);
                json.NumberOrNull("width",2/scale,scale>0 && std::isfinite(scale));
                json.NumberOrNull("texels_per_world_unit",info.size*scale*.5,info.valid && std::isfinite(scale));
                const auto actual=MeasureShadowDensity(matrix.data(),info.size,info.size);
                const auto plan=AssessDetailSampling(info,actual,referenceDensity,app->AdaptiveShadowResolution(),
                    reinterpret_cast<const float*>(&app->WorldViewProjection()),planInputs,layer);
                WriteShadowDensity(json,actual,referenceDensity,info.valid,true,plan.requested);
                WriteDetailSamplingPlan(json,plan);
                json.NumberOrNull("density_floor",info.densityFloor,info.valid && std::isfinite(info.densityFloor));
                json.NumberOrNull("near_depth",info.nearDepth,info.valid && std::isfinite(info.nearDepth));
                json.NumberOrNull("far_depth",info.farDepth,info.valid && std::isfinite(info.farDepth));
                json.NumberOrNull("blend_start",info.blendStart,info.valid && std::isfinite(info.blendStart));
                writeMatrix("projection",matrix.data());
                json.End();
            }
            json.End();
            FILE* file=nullptr;
            if(_wfopen_s(&file,jsonPath.c_str(),L"wb") || !file)return kExitJson;
            const std::string text=json.Finish();
            const std::size_t written=std::fwrite(text.data(),1,text.size(),file);
            if(std::fclose(file)!=0 || written!=text.size())return kExitJson;
            if(index%10==0 || index+1==options.sweepFrames)
                std::printf("shadow_camera_sweep frame=%d/%d distance=%.6g image=%ls\n",
                    index+1,options.sweepFrames,camera[3],imagePath.c_str());
        }
        setCamera(start);
        renderFrozenPose();
        if(!flush())return kExitFlush;
    }

    if (options.verifyShadowModes) {
        auto* renderer = app->Renderer();
        if (!UseEffectModelRenderer(app) || !renderer || !renderer->effect) {
            std::fputs("shadow mode verification requires the DX9 effect model renderer\n", stderr);
            return kExitUsage;
        }
        std::error_code pathError;
        const auto output = std::filesystem::absolute(std::filesystem::path(options.output), pathError);
        if (pathError) return kExitSurface;
        std::filesystem::create_directories(output.parent_path(), pathError);
        if (pathError) return kExitSurface;
        const auto stem = (output.parent_path() / output.stem()).wstring();
        const std::wstring reportPath = stem + L".shadow-modes.json";
        struct ModeSnapshot {
            int mode = 0;
            bool comparisonRead = false, coverageValid = false, expectedMatches = false;
            UINT coverageWidth = 0, coverageHeight = 0, sampleWidth = 0, sampleHeight = 0;
            unsigned referenceSize = 0, maximumSize = 0;
            double referenceDensity=0;
            bool densityWithinPlan=false,sharedDepth=false;
            ShadowPlanInputs planInputs{};
            std::array<DetailSamplingPlan,3> plans{};
            ShadowDensity coverageDensity{};
            CoverageFixedLimit coverageFixedLimit{};
            std::array<ShadowDensity,3> detailDensity{};
            float light[3]{};
            std::array<float,16> coverage{}, toNative{}, expected{}, camera{}, lens{};
            std::array<double,16> composed{};
            std::array<shadow_filter::DetailInfo,3> details{};
            double maxError = 0;
            std::wstring image;
        };
        std::array<ModeSnapshot,3> snapshots{};
        // ID3DXEffect::GetMatrix is slot 39, immediately after SetMatrix (38).
        // Read the live effect parameter rather than recomputing the CPU plan.
        const auto FxGetMatrix = [](void* effect, const char* name, d3dx::D3DXMATRIXF* matrix) {
            using Fn = HRESULT(WINAPI*)(void*,const char*,d3dx::D3DXMATRIXF*);
            return d3dx::EffectMethod<Fn,39>(effect)(effect,name,matrix);
        };
        const int savedMode = app->SelfShadowMode();
        const auto savedEnabled = app->state.selfShadowEnabled;
        // Same-mode control: compare this frame with the formal mode-1 frame
        // before attributing small raster/cache differences to switching modes.
        const std::wstring controlImage=stem+L".mode-1-control-start.png";
        for (int settle=0;settle<3;++settle)renderFrozenPose(1);
        if (!flush())return kExitFlush;
        if (!saveFrozenImage(controlImage))return kExitSurface;
        const int modes[]{1,2,1};
        const wchar_t* suffixes[]{L".mode-1.png",L".mode-2.png",L".mode-1-repeat.png"};
        for (unsigned index=0;index<snapshots.size();++index) {
            // The last timed FrameDriver may leave newer bone matrices than
            // vertex buffers. Settle that same pose without advancing it.
            for (int settle=0;settle<3;++settle)renderFrozenPose(modes[index]);
            if (!flush()) return kExitFlush;
            auto& snapshot=snapshots[index];
            snapshot.mode=app->SelfShadowMode();
            snapshot.image=stem+suffixes[index];
            if (!saveFrozenImage(snapshot.image)) return kExitSurface;
            std::copy_n(app->LightDirection(),3,snapshot.light);
            std::copy_n(reinterpret_cast<const float*>(&app->WorldViewProjection()),16,snapshot.camera.begin());
            std::copy_n(reinterpret_cast<const float*>(&app->LightViewProjection()),16,snapshot.coverage.begin());
            D3DSURFACE_DESC desc{};
            snapshot.coverageValid=renderer->shadowSurface && SUCCEEDED(renderer->shadowSurface->GetDesc(&desc));
            snapshot.coverageWidth=desc.Width;snapshot.coverageHeight=desc.Height;
            snapshot.coverageValid &= ShadowSampleDimensions(app,snapshot.sampleWidth,snapshot.sampleHeight);
            snapshot.coverageDensity=MeasureShadowDensity(snapshot.coverage.data(),snapshot.sampleWidth,snapshot.sampleHeight);
            snapshot.referenceSize=app->ShadowResolutionLimit();
            snapshot.referenceDensity=NativeShadowSamplingDensity(snapshot.light,app->ShadowDistance(),
                snapshot.mode,snapshot.referenceSize);
            D3DCAPS9 shadowCaps{};
            if (SUCCEEDED(device->GetDeviceCaps(&shadowCaps)))
                snapshot.maximumSize=(std::min)({8192u,static_cast<unsigned>(shadowCaps.MaxTextureWidth),
                    static_cast<unsigned>(shadowCaps.MaxTextureHeight),snapshot.referenceSize*2u});
            const bool lensRead=SUCCEEDED(device->GetTransform(D3DTS_PROJECTION,
                reinterpret_cast<D3DMATRIX*>(snapshot.lens.data())));
            snapshot.planInputs=ReadShadowPlanInputs(app);
            snapshot.comparisonRead=SUCCEEDED(FxGetMatrix(renderer->effect,"MdsShadowToNative",
                reinterpret_cast<d3dx::D3DXMATRIXF*>(snapshot.toNative.data())));
            snapshot.expected=NativeShadowComparisonProjection(snapshot.light,snapshot.mode);
            snapshot.expectedMatches=snapshot.comparisonRead;
            for (int row=0;row<4;++row)for (int column=0;column<4;++column) {
                double value=0;
                for (int axis=0;axis<4;++axis)
                    value+=double(snapshot.coverage[row*4+axis])*snapshot.toNative[axis*4+column];
                const int cell=row*4+column;
                snapshot.composed[cell]=value;
                const double error=std::abs(value-snapshot.expected[cell]);
                snapshot.maxError=(std::max)(snapshot.maxError,error);
                snapshot.expectedMatches &= std::isfinite(value) &&
                    error<=2e-5*(std::max)(1.0,std::abs(double(snapshot.expected[cell])));
            }
            snapshot.densityWithinPlan=snapshot.coverageValid && snapshot.planInputs.valid &&
                ShadowDensityWithinReference(snapshot.coverageDensity,snapshot.planInputs.coverageDensity);
            snapshot.coverageFixedLimit=AssessCoverageFixedLimit(snapshot.coverage.data(),snapshot.coverageDensity,
                snapshot.referenceDensity,snapshot.planInputs);
            snapshot.sharedDepth=true;
            for (unsigned layer=0;layer<snapshot.details.size();++layer) {
                const auto info=shadow_filter::ReadDetailInfo(app,layer);
                snapshot.details[layer]=info;
                const auto density=MeasureShadowDensity(info.projection.data(),info.size,info.size);
                snapshot.detailDensity[layer]=density;
                snapshot.plans[layer]=AssessDetailSampling(info,density,snapshot.referenceDensity,
                    app->AdaptiveShadowResolution(),snapshot.camera.data(),snapshot.planInputs,layer);
                if (info.valid) {
                    snapshot.densityWithinPlan &= snapshot.plans[layer].known && snapshot.plans[layer].matchesCapacity &&
                        snapshot.plans[layer].allocationWithinBudget;
                    for(int row=0;row<4;++row)for(int column=2;column<4;++column)
                        snapshot.sharedDepth &= info.projection[row*4+column]==snapshot.coverage[row*4+column];
                }
            }
            snapshot.densityWithinPlan &= lensRead;
        }
        const auto sameGeometry = [](const ModeSnapshot& a,const ModeSnapshot& b) {
            if (a.coverageValid!=b.coverageValid || a.coverageWidth!=b.coverageWidth ||
                a.coverageHeight!=b.coverageHeight ||
                std::memcmp(a.coverage.data(),b.coverage.data(),sizeof(a.coverage))!=0) return false;
            for (unsigned layer=0;layer<a.details.size();++layer) {
                const auto& x=a.details[layer];const auto& y=b.details[layer];
                if (x.valid!=y.valid || x.size!=y.size ||
                    std::memcmp(x.projection.data(),y.projection.data(),sizeof(x.projection))!=0 ||
                    x.nearDepth!=y.nearDepth || x.farDepth!=y.farDepth ||
                    x.blendStart!=y.blendStart || x.densityFloor!=y.densityFloor) return false;
            }
            return true;
        };
        const auto sameDepth = [](const ModeSnapshot& a,const ModeSnapshot& b) {
            for(int row=0;row<4;++row)for(int column=2;column<4;++column)
                if(a.coverage[row*4+column]!=b.coverage[row*4+column])return false;
            return a.sharedDepth && b.sharedDepth;
        };
        const auto sameSlabs = [&](const ModeSnapshot& a,const ModeSnapshot& b) {
            for(unsigned layer=0;layer<a.details.size();++layer) {
                const auto& x=a.details[layer];const auto& y=b.details[layer];
                if(x.nearDepth!=y.nearDepth || x.farDepth!=y.farDepth ||
                   ((!app->AdaptiveShadowResolution() || layer==2) && x.blendStart!=y.blendStart))return false;
            }
            return true;
        };
        const bool depthUnchanged=sameDepth(snapshots[0],snapshots[1]) && sameDepth(snapshots[0],snapshots[2]);
        const bool slabsUnchanged=sameSlabs(snapshots[0],snapshots[1]) && sameSlabs(snapshots[0],snapshots[2]);
        const bool samplingWithinPlan=std::all_of(snapshots.begin(),snapshots.end(),[](const ModeSnapshot& value) {
            return value.densityWithinPlan;
        });
        const bool nearestPlanMet=std::all_of(snapshots.begin(),snapshots.end(),[](const ModeSnapshot& value) {
            return value.plans[1].known && value.plans[1].matchesCapacity;
        });
        const bool farSamplingWithinFixed=std::all_of(snapshots.begin(),snapshots.end(),[](const ModeSnapshot& value) {
            return value.coverageFixedLimit.known && value.coverageFixedLimit.within &&
                (!value.details[2].valid || (value.plans[2].known &&
                    (value.plans[2].closeLayer || value.plans[2].farWithinFixed)));
        });
        const bool farFixedFade=std::all_of(snapshots.begin(),snapshots.end(),[](const ModeSnapshot& value) {
            for(unsigned layer=0;layer<3;++layer)
                if(value.details[layer].valid && (!value.plans[layer].known || !value.plans[layer].fixedBlendMatches))return false;
            return true;
        });
        const bool expectedChange=snapshots[0].expected!=snapshots[1].expected;
        const bool comparisonChanged=snapshots[0].toNative!=snapshots[1].toNative;
        const bool comparisonRestored=std::memcmp(snapshots[0].toNative.data(),snapshots[2].toNative.data(),
            sizeof(snapshots[0].toNative))==0;
        const bool geometryUnchanged=sameGeometry(snapshots[0],snapshots[1]);
        const bool geometryRestored=sameGeometry(snapshots[0],snapshots[2]);
        const bool lightUnchanged=std::memcmp(snapshots[0].light,snapshots[1].light,sizeof(snapshots[0].light))==0 &&
            std::memcmp(snapshots[0].light,snapshots[2].light,sizeof(snapshots[0].light))==0;
        const bool cameraUnchanged=std::memcmp(snapshots[0].camera.data(),snapshots[1].camera.data(),sizeof(snapshots[0].camera))==0 &&
            std::memcmp(snapshots[0].camera.data(),snapshots[2].camera.data(),sizeof(snapshots[0].camera))==0;
        const bool expectedMatches=std::all_of(snapshots.begin(),snapshots.end(),[](const ModeSnapshot& value) {
            return value.comparisonRead && value.coverageValid && value.expectedMatches;
        });
        // At |light.z|>0.9 the original modes can use the same comparison
        // matrix; their shader coefficients still differ. Do not false-fail it.
        const bool ok=expectedMatches && expectedChange==comparisonChanged && comparisonRestored &&
            geometryRestored && depthUnchanged && slabsUnchanged && samplingWithinPlan &&
            nearestPlanMet && farSamplingWithinFixed && farFixedFade && lightUnchanged && cameraUnchanged &&
            snapshots[0].mode==1 && snapshots[1].mode==2 && snapshots[2].mode==1;
        std::string report;
        Json json(report);
        json.String("tool","frame_benchmark");
        json.String("schema","frame_benchmark/shadow-modes/6");
        json.String("scene_path",options.target);
        json.String("mode_1_control_start_image",controlImage.c_str());
        json.String("capture_sequence","1-control,1,2,1-repeat");
        json.Integer("frozen_renders_per_capture",3);
        json.Bool("filter_enhanced",app->ShadowFilterEnhanced());
        json.Number("density_relative_tolerance",kShadowDensityTolerance);
        json.Number("density_absolute_tolerance",1e-7);
        json.Bool("physics_advanced",false);
        json.Integer("slider",ShadowSliderPosition(app));
        json.Bool("adaptive_resolution",app->AdaptiveShadowResolution());
        json.Integer("viewport_width",requestedWidth);json.Integer("viewport_height",requestedHeight);
        json.Number("matrix_scaled_tolerance",2e-5);
        json.String("matrix_tolerance_rule","abs(actual-expected) <= 2e-5 * max(1, abs(expected))");
        const auto writeMatrix = [&](const char* name,const auto& matrix) {
            json.Array(name);
            for (int row=0;row<4;++row) {
                json.Item();
                const char* keys[]{"x","y","z","w"};
                for (int column=0;column<4;++column) {
                    const auto value=matrix[row*4+column];
                    if (std::isfinite(value)) json.Number(keys[column],value,9);
                    else json.Null(keys[column]);
                }
                json.End();
            }
            json.End();
        };
        json.Array("frames");
        for (const auto& snapshot:snapshots) {
            json.Item();
            json.Integer("mode",snapshot.mode);json.String("image",snapshot.image.c_str());
            json.Number("reference_density",snapshot.referenceDensity,9);
            json.Integer("reference_resolution",snapshot.referenceSize);
            json.Integer("maximum_resolution",snapshot.maximumSize);
            json.Object("light_direction");
            json.Number("x",snapshot.light[0],9);json.Number("y",snapshot.light[1],9);json.Number("z",snapshot.light[2],9);
            json.End();
            writeMatrix("world_view_projection",snapshot.camera);
            writeMatrix("camera_projection",snapshot.lens);
            json.Bool("comparison_read",snapshot.comparisonRead);
            writeMatrix("MdsShadowToNative",snapshot.toNative);
            writeMatrix("coverage_times_to_native",snapshot.composed);
            writeMatrix("expected_comparison_projection",snapshot.expected);
            json.Number("comparison_max_error",snapshot.maxError,9);
            json.Bool("expected_projection_matches",snapshot.expectedMatches);
            json.Object("coverage");
            json.Bool("valid",snapshot.coverageValid);
            json.Integer("width",snapshot.coverageWidth);json.Integer("height",snapshot.coverageHeight);
            json.Integer("sample_width",snapshot.sampleWidth);json.Integer("sample_height",snapshot.sampleHeight);
            WriteShadowDensity(json,snapshot.coverageDensity,snapshot.referenceDensity,snapshot.coverageValid,
                true,snapshot.planInputs.valid?snapshot.planInputs.coverageDensity:0);
            WriteCoverageFixedLimit(json,snapshot.coverageFixedLimit,snapshot.planInputs);
            writeMatrix("LightViewProjection",snapshot.coverage);
            json.End();
            const char* layerNames[]{"middle","near","bridge"};
            for (unsigned layer=0;layer<snapshot.details.size();++layer) {
                const auto& info=snapshot.details[layer];
                json.Object(layerNames[layer]);
                json.Bool("valid",info.valid);json.Integer("resolution",info.size);
                WriteShadowDensity(json,snapshot.detailDensity[layer],snapshot.referenceDensity,info.valid,
                    true,snapshot.plans[layer].requested);
                WriteDetailSamplingPlan(json,snapshot.plans[layer]);
                json.Number("near_depth",info.nearDepth,9);json.Number("far_depth",info.farDepth,9);
                json.Number("blend_start",info.blendStart,9);json.Number("density_floor",info.densityFloor,9);
                writeMatrix("projection",info.projection);
                json.End();
            }
            json.End();
        }
        json.End();
        json.Object("checks");
        json.Bool("comparison_change_expected",expectedChange);
        json.Bool("comparison_changed",comparisonChanged);
        json.Bool("comparison_restored_exact",comparisonRestored);
        json.Bool("geometry_unchanged_exact",geometryUnchanged);json.Bool("geometry_restored_exact",geometryRestored);
        json.Bool("camera_unchanged_exact",cameraUnchanged);
        json.Bool("shared_z_unchanged",depthUnchanged);json.Bool("depth_slabs_unchanged",slabsUnchanged);
        json.Bool("sampling_matches_plan_and_capacity",samplingWithinPlan);
        json.Bool("nearest_matches_plan_and_capacity",nearestPlanMet);
        json.Bool("far_layers_within_fixed_capacity",farSamplingWithinFixed);
        json.Bool("depth_fades_match_geometry",farFixedFade);
        json.Bool("light_unchanged",lightUnchanged);json.Bool("expected_projection_matches",expectedMatches);
        json.Bool("ok",ok);json.End();
        FILE* file=nullptr;
        if (_wfopen_s(&file,reportPath.c_str(),L"wb") || !file)return kExitJson;
        const std::string text=json.Finish();
        const auto written=std::fwrite(text.data(),1,text.size(),file);
        if (std::fclose(file)!=0 || written!=text.size())return kExitJson;
        std::printf("shadow_modes comparison_changed=%u change_expected=%u geometry_unchanged=%u "
                    "comparison_restored_exact=%u geometry_restored=%u expected_matches=%u z_unchanged=%u "
                    "slabs_unchanged=%u sampling_matches_plan=%u near_plan_met=%u far_within_fixed=%u "
                    "far_fixed_fade=%u ok=%u report=%ls\n",
            comparisonChanged,expectedChange,geometryUnchanged,comparisonRestored,geometryRestored,expectedMatches,
            depthUnchanged,slabsUnchanged,samplingWithinPlan,nearestPlanMet,farSamplingWithinFixed,farFixedFade,ok,reportPath.c_str());
        renderFrozenPose(savedMode);
        app->state.selfShadowEnabled=savedEnabled;
        if (!flush())return kExitFlush;
        if (!ok)return kExitShadowModes;
    }

    if (options.verifyShadowZoom) {
        if (!throughput || native || !options.output || !UseEffectModelRenderer(app)) return 50;
        const float savedDistance = app->CameraDistance(), savedRange = app->ShadowDistance();
        const bool savedAdaptive = app->AdaptiveShadowResolution();
        // The measured frame driver may finish with a physics pose newer than
        // the last rendered vertex buffer. Render that pose before taking the
        // coverage reference; the zoom loop below does not advance physics.
        for (int frame = 0; frame < 3; ++frame) {
            installViewport(); app->state.messageSeen = 10; RenderFrameScene(app);
        }
        const auto projection = app->LightViewProjection();
        std::vector<unsigned> adaptiveWidths, fixedWidths;
        for (bool adaptive : {true, false}) {
            HWND checkbox = GetDlgItem(app->MainWindow(), panel::kAdaptiveShadowCheckbox);
            SendMessageW(checkbox, BM_SETCHECK, adaptive ? BST_CHECKED : BST_UNCHECKED, 0);
            CommandDispatch(checkbox, MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED));
            for (float distance : {-720.f, -180.f, -45.f, -12.f, -180.f, -720.f}) {
                app->CameraDistance() = distance;
                for (int frame = 0; frame < 3; ++frame) {
                    installViewport(); app->state.messageSeen = 10; RenderFrameScene(app);
                }
                D3DSURFACE_DESC actual{}, texture{};
                auto* renderer = app->Renderer();
                if (!renderer->shadowSurface || !renderer->hdrTexture ||
                    FAILED(renderer->shadowSurface->GetDesc(&actual)) ||
                    FAILED(renderer->hdrTexture->GetLevelDesc(0, &texture)) ||
                    actual.Width != texture.Width || actual.Height != texture.Height ||
                    actual.Width != unsigned(renderer->renderTargetWidth)) return 51;
                const bool unchanged = app->ShadowDistance() == savedRange &&
                    std::memcmp(&projection, &app->LightViewProjection(), sizeof(projection)) == 0;
                std::printf("shadow_zoom adaptive=%u distance=%.0f surface=%u texture=%u coverage_preserved=%u\n",
                    adaptive, -distance, actual.Width, texture.Width, unchanged);
                if (!unchanged) return 52;
                (adaptive ? adaptiveWidths : fixedWidths).push_back(actual.Width);
                IDirect3DSurface9* target = nullptr;
                if (FAILED(device->GetRenderTarget(0, &target))) return 53;
                wchar_t suffix[80];
                swprintf_s(suffix, L".zoom-%u-%u-%u.png", adaptive, unsigned(-distance),
                    unsigned(adaptive ? adaptiveWidths.size() : fixedWidths.size()));
                const std::wstring path = std::wstring(options.output) + suffix;
                RECT area{0, 0, requestedWidth, requestedHeight};
                const HRESULT saved = d3dx::Get().saveSurfaceToFileW(path.c_str(), 3, target, nullptr, &area);
                target->Release();
                if (FAILED(saved)) return 54;
            }
        }
        const bool changed = adaptiveWidths[0] < adaptiveWidths[3] &&
            adaptiveWidths[0] == adaptiveWidths[5];
        const bool fixed = std::all_of(fixedWidths.begin(), fixedWidths.end(),
            [&](unsigned width) { return width == fixedWidths.front(); });
        std::printf("shadow_zoom_check dynamic_changed=%u far_restored=%u fixed_constant=%u\n",
            changed, adaptiveWidths[0] == adaptiveWidths[5], fixed);
        if (!changed || !fixed) return 55;
        app->CameraDistance() = savedDistance;
        app->AdaptiveShadowResolution() = savedAdaptive;
    }

    // Keep the measured pose fixed while testing the complete producer/receiver
    // path. Comparing with casters disabled distinguishes occlusion from toon
    // shading. No model data is saved, and the window remains hidden.
    if (options.verifyShadowCycle) {
        if (!throughput || native || options.skinOnly || !options.output ||
            !options.viewportGiven || !UseEffectModelRenderer(app)) return 47;
        std::vector<std::vector<DWORD>> shadowMaps;
        std::vector<unsigned> shadowWidths;
        const auto capture = [&](const wchar_t* suffix) {
            installViewport();
            app->state.messageSeen = 10;
            RenderFrameScene(app);
            IDirect3DSurface9* source = nullptr;
            IDirect3DSurface9* readback = nullptr;
            std::vector<DWORD> pixels;
            if (FAILED(device->GetRenderTarget(0, &source))) return pixels;
            D3DSURFACE_DESC desc{}; source->GetDesc(&desc);
            const std::wstring path = std::wstring(options.output) + suffix;
            RECT area{0, 0, requestedWidth, requestedHeight};
            d3dx::Get().saveSurfaceToFileW(path.c_str(), 3, source, nullptr, &area);
            if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
                IDirect3DSurface9* resolved = nullptr;
                if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                        D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) ||
                    FAILED(device->StretchRect(source, nullptr, resolved, nullptr, D3DTEXF_NONE))) {
                    if (resolved) resolved->Release();
                    source->Release();
                    return pixels;
                }
                source->Release(); source = resolved;
            }
            if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height,
                    desc.Format, D3DPOOL_SYSTEMMEM, &readback, nullptr)) &&
                SUCCEEDED(device->GetRenderTargetData(source, readback))) {
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(readback->LockRect(&locked, &area, D3DLOCK_READONLY))) {
                    for (int y = 0; y < requestedHeight; ++y) {
                        const auto* row = reinterpret_cast<const DWORD*>(
                            static_cast<const char*>(locked.pBits) + y * locked.Pitch);
                        pixels.insert(pixels.end(), row, row + requestedWidth);
                    }
                    readback->UnlockRect();
                }
            }
            if (readback) readback->Release();
            source->Release();
            if (auto* shadow = app->Renderer()->shadowSurface) {
                const std::wstring map = path + L".shadow.dds";
                d3dx::Get().saveSurfaceToFileW(map.c_str(), 4, shadow, nullptr, nullptr);
                shadow->GetDesc(&desc);
                readback = nullptr;
                std::vector<DWORD> depths;
                if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height,
                        desc.Format, D3DPOOL_SYSTEMMEM, &readback, nullptr)) &&
                    SUCCEEDED(device->GetRenderTargetData(shadow, readback))) {
                    D3DLOCKED_RECT locked{};
                    if (SUCCEEDED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                        for (unsigned y = 0; y < desc.Height; ++y) {
                            const auto* row = reinterpret_cast<const DWORD*>(
                                static_cast<const char*>(locked.pBits) + y * locked.Pitch);
                            depths.insert(depths.end(), row, row + desc.Width);
                        }
                        readback->UnlockRect();
                    }
                }
                if (readback) readback->Release();
                shadowMaps.push_back(std::move(depths));
                shadowWidths.push_back(desc.Width);
            }
            return pixels;
        };
        const auto toggle = [&](bool enabled) {
            HWND checkbox = GetDlgItem(app->MainWindow(), panel::kAdaptiveShadowCheckbox);
            SendMessageW(checkbox, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            CommandDispatch(checkbox, MAKEWPARAM(panel::kAdaptiveShadowCheckbox, BN_CLICKED));
        };
        toggle(false);
        const float fixedRange = app->ShadowDistance();
        const int fixedPosition = ShadowSliderPosition(app);
        const auto fixed = capture(L".fixed.png");
        const auto hasDetail = [&]() {
            for(unsigned layer=0;layer<3;++layer)
                if(shadow_filter::ReadDetailInfo(app,layer).valid)return true;
            return false;
        };
        const bool fixedSingleGrid=!hasDetail();
        const auto fixedProjection = app->LightViewProjection();
        struct Caster { mdl::ModelMaterialRecord* material; unsigned flags; };
        std::vector<Caster> casters;
        for (int slot = 0; slot < kModelSlotCount; ++slot) {
            auto* model = app->ModelSlot(slot);
            if (!model || mdl::Mdl(model)->physicsMode != 2) continue;
            auto* materials = mdl::Materials(model);
            for (unsigned i = 0; i < mdl::Mdl(model)->materialCount; ++i) {
                casters.push_back({materials + i, materials[i].flags});
                materials[i].flags &= ~4u;
            }
        }
        const auto unoccluded = capture(L".unoccluded.png");
        for (auto caster : casters) caster.material->flags = caster.flags;
        toggle(true);
        HWND slider = GetDlgItem(app->MainWindow(), panel::kSelfShadowRangeSlider);
        const auto precision = [&](int value) {
            SendMessageW(slider, TBM_SETPOS, TRUE, value);
            SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(slider));
        };
        capture(L".dynamic.png");
        const bool dynamicHasDetail=hasDetail();
        const bool sharedCoverage = app->ShadowDistance() == fixedRange &&
            std::memcmp(&fixedProjection, &app->LightViewProjection(), sizeof(fixedProjection)) == 0;
        precision(9500);
        capture(L".wide.png");
        const bool wideCoverage = std::abs(app->ShadowDistance() - .005f) < 1e-7f;
        precision(fixedPosition);
        toggle(false);
        const auto restored = capture(L".restored.png");
        const bool restoredSingleGrid=!hasDetail();
        std::printf("shadow_cycle fixed_single_grid=%u dynamic_detail=%u restored_single_grid=%u\n",
            fixedSingleGrid,dynamicHasDetail,restoredSingleGrid);
        if(!fixedSingleGrid || !dynamicHasDetail || !restoredSingleGrid)return 49;
        std::size_t occludedPixels = 0, changedPixels = 0;
        if (fixed.empty() || fixed.size() != unoccluded.size() || fixed.size() != restored.size()) return 48;
        for (std::size_t i = 0; i < fixed.size(); ++i) {
            int occlusion = 0, change = 0;
            for (int shift : {0, 8, 16}) {
                occlusion = std::max(occlusion, std::abs(int((fixed[i] >> shift) & 255) - int((unoccluded[i] >> shift) & 255)));
                change = std::max(change, std::abs(int((fixed[i] >> shift) & 255) - int((restored[i] >> shift) & 255)));
            }
            occludedPixels += occlusion > 8;
            changedPixels += change > 2;
        }
        const bool sameMap = shadowMaps.size() == 5 && !shadowMaps[0].empty() && shadowMaps[0] == shadowMaps[4];
        const bool resolutionOrder = shadowWidths.size() == 5 && shadowWidths[3] >= shadowWidths[2];
        std::printf("shadow_cycle occluded_pixels=%zu restored_changed_pixels=%zu range_preserved=%u map_identical=%u shared_coverage=%u wide_coverage=%u resolution_order=%u dynamic_width=%u wide_width=%u\n",
            occludedPixels, changedPixels, fixedRange == app->ShadowDistance(), sameMap, sharedCoverage, wideCoverage, resolutionOrder,
            shadowWidths.size() == 5 ? shadowWidths[2] : 0, shadowWidths.size() == 5 ? shadowWidths[3] : 0);
        // MSAA/transparent surface ties can vary at a few triangle-edge pixels.
        // Require the restored shadow depths to be bit-exact, and reject any
        // visible region changing after toggling (more than 0.1% of the frame).
        if (occludedPixels < 32 || changedPixels > fixed.size() / 1000 || !sameMap ||
            !sharedCoverage || !wideCoverage || !resolutionOrder || fixedRange != app->ShadowDistance()) return 49;
    }

    // ---- optional outputs (historical contract) --------------------------
    if (native && options.output) {
        if (!ValidateD3D12Native(options.output)) {
            std::printf("native validation failed: %s\n", D3D12NativeStatus());
            ShutdownCleanup(app);return kExitNativeValidation;
        }
        std::puts("PASS native GPU stable sort and D3D12 debug validation");
    }
    if (throughput && !native && options.output) {
        // The screenshot has to show the measured pose.  The frame tail
        // (playback catch-up, physics) rewrites the bone matrices after the
        // render, so the placement must be written back before this extra
        // render or the copies collapse onto each other again.
        ApplyCopyOffsets(copyRecords);
        installViewport();
        installSelfShadow();
        app->state.messageSeen=10;
        RenderFrameScene(app);
        IDirect3DSurface9* surface=nullptr;
        if(FAILED(device->GetRenderTarget(0,&surface)))return kExitSurface;
        D3DSURFACE_DESC outlineTarget{};
        DWORD outlineAA = 0;
        surface->GetDesc(&outlineTarget);
        device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &outlineAA);
        std::printf("outline_capture target_msaa=%u quality=%u antialias=%u outlines_requested=%d sorting=%u\n",
            unsigned(outlineTarget.MultiSampleType), outlineTarget.MultiSampleQuality, outlineAA,
            options.outlines, app->AutoTransparentMaterialSortingEnabled());
        auto& api=d3dx::Get();api.Load();
        // Format from the extension, as the screenshot command does; an
        // unknown extension keeps the historical D3DXIFF_BMP.
        int format = 0;
        if (wcsstr(options.output, L".jpg") != nullptr) format = 1;
        else if (wcsstr(options.output, L".png") != nullptr) format = 3;
        else if (wcsstr(options.output, L".dds") != nullptr) format = 4;
        else if (wcsstr(options.output, L".dib") != nullptr) format = 6;
        // With an explicit viewport, capture exactly that rectangle: the
        // reported screen coordinates are viewport relative, so the image has
        // to be the same area or the two cannot be compared.
        RECT capture{};
        const RECT* captureRect = nullptr;
        if (requestedWidth != 0 && requestedHeight != 0) {
            capture.left = 0;
            capture.top = 0;
            capture.right = requestedWidth;
            capture.bottom = requestedHeight;
            captureRect = &capture;
        }
        HRESULT hr=api.saveSurfaceToFileW(options.output,format,surface,nullptr,captureRect);
        surface->Release();if(FAILED(hr))return kExitSurface;
    }
    if (options.skinOnly && options.output) {
        FILE* output=nullptr;
        if (_wfopen_s(&output,options.output,L"wb") || !output) return kExitOutput;
        for (int slot=0; slot<kModelSlotCount; ++slot) if (auto* m=app->ModelSlot(slot)) {
            auto& record=*mdl::Mdl(m);
            for (auto resource : {record.vertexBuffer,record.vertexBuffer2}) {
                auto* vb=reinterpret_cast<IDirect3DVertexBuffer9*>(resource);
                D3DVERTEXBUFFER_DESC desc{}; void* data=nullptr;
                if (FAILED(vb->GetDesc(&desc)) || FAILED(vb->Lock(0,0,&data,0))) {
                    std::fclose(output); return kExitNative;
                }
                const auto written=std::fwrite(data,1,desc.Size,output);
                vb->Unlock();
                if (written!=desc.Size) { std::fclose(output); return kExitNativeValidation; }
            }
        }
        if (std::fclose(output)!=0) return kExitNativeValidation;
    }

    // ---- JSON report -----------------------------------------------------
    int jsonResult = 0;
    std::string report;
    if (options.json != nullptr) {
        Json json(report);
        json.String("tool", L"frame_benchmark");
        json.String("schema", L"frame_benchmark/1");
        json.String("mode", native ? L"native"
                                   : options.skinOnly ? L"skin-only"
                                   : options.loadOnly ? L"load-only"
                                   : throughput ? L"throughput" : L"serial");
        json.String("scene_path", options.target);
        json.String("scene_kind", options.modelFile ? L"model" : L"scene");
        json.Object("scene");
        json.Integer("models", models);
        json.Integer("vertices", vertices);
        json.Integer("accessories", accessories);
        // Rigid-body and joint totals explain whether the scene has physics at
        // all: the layout checks in this file use a model with none.
        json.Integer("rigid_body_count", rigidBodies);
        json.Integer("joint_count", joints);
        json.End();
        json.Object("skinning");
        json.String("requested", options.processor);
        json.Integer("gpu_updates", skinning::GetStatus().gpuUpdates);
        json.Integer("cpu_fallbacks", skinning::GetStatus().cpuFallbacks);
        json.Integer("cpu_updates", skinning::GetStatus().cpuUpdates);
        json.Integer("automatic_calibrations", skinning::GetStatus().automaticCalibrations);
        json.String("compute_adapter", skinning::GetStatus().adapterName.c_str());
        json.End();
        json.Object("gpu");
        json.String("description", adapter.Description);
        json.Integer("adapter_ordinal", creation.AdapterOrdinal);
        json.Integer("device_type", static_cast<long long>(creation.DeviceType));
        json.Integer("behavior_flags", static_cast<long long>(creation.BehaviorFlags));
        json.End();
        json.Object("viewport");
        json.Integer("width", actualWidth);
        json.Integer("height", actualHeight);
        if (options.viewportGiven) {
            json.Integer("requested_width", requestedWidth);
            json.Integer("requested_height", requestedHeight);
        } else {
            json.Null("requested_width");
            json.Null("requested_height");
        }
        json.End();
        if (requestedWidth != 0) {
            json.Object("model_pass_viewport");
            json.Integer("render_target_width", renderTargetWidth);
            json.Integer("render_target_height", renderTargetHeight);
            json.Number("projection_aspect", projectionAspect);
            json.Integer("frames_checked", static_cast<long long>(viewportChecks));
            json.Integer("frames_mismatched",
                         static_cast<long long>(viewportMismatches));
            json.Bool("ok", viewportEvidenceOk);
            json.End();
        } else {
            json.Null("model_pass_viewport");
        }
        json.Integer("copies", static_cast<long long>(copyRecords.size()));
        json.Integer("copies_requested", copies);
        json.String("layout", options.layout == Layout::Spread ? L"spread" : L"overlap");
        json.Number("orbit_degrees_per_frame", options.orbitDegrees);
        json.Integer("warmup", warmup);
        json.Integer("samples", samples);
        json.Number("load_ms", loadMs);
        json.Object("timing");
        json.Number("drained_batch_ms_per_frame", batchMs / times.size());
        json.Number("drained_frames_per_second", 1000.0 * times.size() / batchMs);
        json.Number("mean_ms", sum / times.size());
        json.Number("median_ms", times[times.size() / 2]);
        json.Number("p95_ms", times[times.size() * 95 / 100]);
        json.Number("equivalent_fps", 1000.0 * times.size() / sum);
        json.Integer("sample_count", static_cast<long long>(times.size()));
        json.End();
        // Counts with nonzero durations are observations, not invocation counts.
        json.Object("mmv");
        json.Bool("loaded", mmvLoaded);
        json.Bool("timings_available", mmvStagesAvailable);
        json.Bool("updates_valid", mmvFailedFrames == 0);
        json.Integer("accepted_frames", mmvAcceptedFrames);
        json.Integer("failed_frames", mmvFailedFrames);
        json.Integer("sample_count", mmvStagesAvailable ? static_cast<long long>(times.size()) : 0);
        json.Number("snapshot_mean_ms", mmvSnapshotSumMs / times.size());
        json.Integer("snapshot_samples", mmvSnapshotSamples);
        json.Number("input_demand_mean_ms", mmvInputDemandSumMs / times.size());
        json.Number("evaluation_mean_ms", mmvEvaluationSumMs / times.size());
        json.Integer("evaluation_samples", mmvEvaluationSamples);
        json.Number("upload_mean_ms", mmvUploadSumMs / times.size());
        json.Integer("upload_samples", mmvUploadSamples);
        json.End();
        PROCESS_MEMORY_COUNTERS memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory)) {
            json.Object("memory");
            json.Integer("peak_working_set_bytes",
                         static_cast<long long>(memory.PeakWorkingSetSize));
            json.Integer("working_set_bytes",
                         static_cast<long long>(memory.WorkingSetSize));
            json.Integer("peak_pagefile_bytes",
                         static_cast<long long>(memory.PeakPagefileUsage));
            json.End();
        } else {
            json.Null("memory");
        }
        json.Object("camera");
        json.Number("pos_x", app->CameraPositionX());
        json.Number("pos_y", app->CameraPositionY());
        json.Number("pos_z", app->CameraPositionZ());
        json.Number("view_offset_x", app->ViewOffsetX());
        json.Number("view_offset_y", app->ViewOffsetY());
        json.Number("distance", app->CameraDistance());
        json.Number("yaw", app->CameraYaw());
        json.Number("pitch", app->CameraPitch());
        json.Number("roll", app->CameraRoll());
        json.Number("fov_degrees", app->CameraFov());
        json.Bool("framed_by_layout", framed);
        json.End();
        json.Object("view_options");
        json.Bool("initial_fine_shadow", initialQuality);
        json.Integer("initial_shadow_width", initialShadowWidth);
        if (initialSortState == UINT(-1)) json.Null("initial_sort_menu_state");
        else json.Integer("initial_sort_menu_state", initialSortState);
        json.Integer("requested_sort_precision", options.sortPrecision);
        json.Integer("effective_sort_precision", static_cast<int>(app->TransparentMaterialSortPrecision()));
        json.Bool("sorting_enabled", app->AutoTransparentMaterialSortingEnabled());
        json.Bool("sorting_cross_objects", app->TransparentSortCrossObjects());
        json.Integer("shadow_width", app->Renderer()->renderTargetWidth);
        json.Bool("toggle_verification_passed", viewTogglesVerified);
        json.End();
        json.Object("self_shadow");
        json.Bool("ground_shadow_enabled", app->GroundShadowEnabled() != 0);
        json.Bool("mme_loaded", GetModuleHandleW(L"MMEffect.dll") != nullptr);
        json.Bool("adaptive_resolution", app->AdaptiveShadowResolution());
        json.Integer("shadow_slider", ShadowSliderPosition(app));
        json.Bool("filter_enhanced",app->ShadowFilterEnhanced());
        const double referenceDensity=NativeShadowSamplingDensity(app->LightDirection(),app->ShadowDistance(),
            app->SelfShadowMode(),app->ShadowResolutionLimit());
        const auto planInputs=ReadShadowPlanInputs(app);
        json.Number("reference_density",referenceDensity,9);
        json.Integer("reference_resolution",app->ShadowResolutionLimit());
            WriteFixedModelShadows(json,app);
        json.Array("light_projection");
        const auto* shadowMatrix = reinterpret_cast<const float*>(&app->LightViewProjection());
        for (int row = 0; row < 4; ++row) {
            json.Item();
            for (int column = 0; column < 4; ++column) {
                const char* keys[] = {"x", "y", "z", "w"};
                json.Number(keys[column], shadowMatrix[row * 4 + column]);
            }
            json.End();
        }
        json.End();
        json.Integer("resolution", app->Renderer()->renderTargetWidth);
        UINT sampleWidth=0,sampleHeight=0;
        const bool sampleValid=ShadowSampleDimensions(app,sampleWidth,sampleHeight);
        json.Integer("sample_width",sampleWidth);json.Integer("sample_height",sampleHeight);
        WriteShadowDensity(json,MeasureShadowDensity(shadowMatrix,sampleWidth,sampleHeight),referenceDensity,sampleValid,
            false,planInputs.valid?planInputs.coverageDensity:0);
        WriteCoverageFixedLimit(json,AssessCoverageFixedLimit(shadowMatrix,
            MeasureShadowDensity(shadowMatrix,sampleWidth,sampleHeight),referenceDensity,planInputs),planInputs);
        const auto detailShadow = shadow_filter::ReadDetailInfo(app);
        const auto nearShadow=shadow_filter::ReadDetailInfo(app,1);
        const auto& nm=nearShadow.projection;
        const double nearScale=std::sqrt(double(nm[0])*nm[0]+double(nm[4])*nm[4]+double(nm[8])*nm[8]);
        const double nearDensity=nearShadow.size*nearScale*.5;
        json.Bool("near_valid",nearShadow.valid);
        json.Integer("near_resolution",nearShadow.size);
        json.Number("near_coverage_width",nearScale>0?2/nearScale:0);
        json.Number("near_density_floor",nearShadow.densityFloor);
        json.Number("near_texels_per_world_unit",nearDensity);
        json.NumberOrNull("near_reference_ratio",referenceDensity>0?nearDensity/referenceDensity:0,nearShadow.valid && referenceDensity>0);
        json.Bool("near_density_within_reference",nearShadow.valid &&
            ShadowDensityWithinReference(MeasureShadowDensity(nm.data(),nearShadow.size,nearShadow.size),referenceDensity));
        json.Bool("near_density_floor_met",nearShadow.valid && nearDensity+1e-3>=nearShadow.densityFloor);
        const auto* cameraMatrix=reinterpret_cast<const float*>(&app->WorldViewProjection());
        json.Object("near_sampling_plan");
        const auto nearActual=MeasureShadowDensity(nm.data(),nearShadow.size,nearShadow.size);
        const auto nearPlan=AssessDetailSampling(nearShadow,nearActual,referenceDensity,
            app->AdaptiveShadowResolution(),cameraMatrix,planInputs,1);
        WriteShadowDensity(json,nearActual,referenceDensity,nearShadow.valid,true,nearPlan.requested);
        WriteDetailSamplingPlan(json,nearPlan);json.End();
        json.Bool("detail_valid", detailShadow.valid);
        json.Integer("detail_resolution", detailShadow.size);
        if (detailShadow.valid) {
            const auto& m=detailShadow.projection;
            const float scale=std::sqrt(m[0]*m[0]+m[4]*m[4]+m[8]*m[8]);
            json.Number("detail_coverage_width",2.f/scale);
            json.Number("detail_texels_per_world_unit",detailShadow.size*scale*.5f);
            json.NumberOrNull("detail_reference_ratio",referenceDensity>0?detailShadow.size*scale*.5/referenceDensity:0,referenceDensity>0);
        }
        json.Object("detail_sampling_plan");
        const auto detailActual=MeasureShadowDensity(detailShadow.projection.data(),detailShadow.size,detailShadow.size);
        const auto detailPlan=AssessDetailSampling(detailShadow,detailActual,referenceDensity,
            app->AdaptiveShadowResolution(),cameraMatrix,planInputs,0);
        WriteShadowDensity(json,detailActual,referenceDensity,detailShadow.valid,true,detailPlan.requested);
        WriteDetailSamplingPlan(json,detailPlan);json.End();
        const auto bridgeShadow=shadow_filter::ReadDetailInfo(app,2);
        json.Object("bridge");json.Bool("valid",bridgeShadow.valid);json.Integer("resolution",bridgeShadow.size);
        const auto bridgeActual=MeasureShadowDensity(bridgeShadow.projection.data(),bridgeShadow.size,bridgeShadow.size);
        const auto bridgePlan=AssessDetailSampling(bridgeShadow,bridgeActual,referenceDensity,
            app->AdaptiveShadowResolution(),cameraMatrix,planInputs,2);
        WriteShadowDensity(json,bridgeActual,referenceDensity,bridgeShadow.valid,true,bridgePlan.requested);
        WriteDetailSamplingPlan(json,bridgePlan);
        json.End();
        D3DSURFACE_DESC actualShadow{};
        if (app->Renderer()->shadowSurface && SUCCEEDED(app->Renderer()->shadowSurface->GetDesc(&actualShadow))) {
            json.Integer("surface_width", actualShadow.Width);
            json.Integer("surface_height", actualShadow.Height);
            const float scale = std::sqrt(shadowMatrix[0]*shadowMatrix[0] +
                shadowMatrix[4]*shadowMatrix[4] + shadowMatrix[8]*shadowMatrix[8]);
            json.Number("coverage_width", 2.f / scale);
            json.Number("texels_per_world_unit", actualShadow.Width * scale / 2.f);
        }
        json.Bool("requested", options.selfShadowGiven);
        json.Integer("requested_mode",
                     options.selfShadowGiven ? options.selfShadowMode : -1);
        json.Integer("mode", app->SelfShadowMode());
        json.Bool("effect_model_renderer", UseEffectModelRenderer(app));
        json.Number("distance", app->ShadowDistance());
        json.Integer("requested_range", options.shadowRange);
        json.Number("light_x", app->LightDirection()[0]);
        json.Number("light_y", app->LightDirection()[1]);
        json.Number("light_z", app->LightDirection()[2]);
        json.Bool("shadow_map_preview", app->SelfShadowCompositionEnabled() != 0);
        json.Bool("depth_preview", app->DepthTextureCompositionEnabled() != 0);
        json.End();
        if (layoutRequested) {
            json.Object("model_bounds");
            json.Number("size_x", bounds.max[0] - bounds.min[0]);
            json.Number("size_y", bounds.max[1] - bounds.min[1]);
            json.Number("size_z", bounds.max[2] - bounds.min[2]);
            json.End();
            json.Array("placements");
            for (std::size_t index = 0; index < copyRecords.size(); ++index) {
                json.Item();
                json.Integer("slot", copyRecords[index].slot);
                json.Array("offset");
                json.Item(); json.Number("x", copyRecords[index].offset[0]); json.End();
                json.Item(); json.Number("y", copyRecords[index].offset[1]); json.End();
                json.Item(); json.Number("z", copyRecords[index].offset[2]); json.End();
                json.End();
                json.Array("center");
                json.Item(); json.Number("x", placements[index].x); json.End();
                json.Item(); json.Number("y", placements[index].y); json.End();
                json.Item(); json.Number("z", placements[index].z); json.End();
                json.End();
                json.Array("vertex_center");
                json.Item(); json.Number("x", vertexCenters[index].x); json.End();
                json.Item(); json.Number("y", vertexCenters[index].y); json.End();
                json.Item(); json.Number("z", vertexCenters[index].z); json.End();
                json.End();
                json.Integer("sampled_vertices", sampledCounts[index]);
                json.Integer("pose_applications", copyRecords[index].applications);
                json.Number("screen_x", projected[index].x);
                json.Number("screen_y", projected[index].y);
                json.Bool("center_in_view", projected[index].inside);
                json.Array("offset_residual");
                json.Item(); json.Number("x", placementResidual[index * 3 + 0]); json.End();
                json.Item(); json.Number("y", placementResidual[index * 3 + 1]); json.End();
                json.Item(); json.Number("z", placementResidual[index * 3 + 2]); json.End();
                json.End();
                json.End();
            }
            json.End();
            json.Bool("all_centers_in_view", allCentersVisible);
            json.Bool("samples_complete", samplesComplete);
            json.Bool("framed", framed);
            json.Bool("placement_verified", placementVerified);
            json.Number("placement_tolerance", placementTolerance);
            json.Number("max_placement_residual", maxPlacementResidual);
        } else {
            json.Null("placements");
        }
        json.Object("notes");
        json.String("throughput_rate",
                    L"drained_frames_per_second is the reciprocal of a GPU-drained batch, not a window FPS");
        json.String("phase_timing",
                    L"phase scopes are nested; a parent includes its children and the values must not be summed");
        json.String("draw_counts",
                    L"no draw-call counter is reported; the driver does not invent one");
        json.String("production_timing",
                    L"phase timers exist only in builds configured with MIKUMIKUSTUDIO_PERF=ON");
        json.String("mmv_timing",
                    L"mmv stage values are host-side CPU durations of the latest MMV update, read once per counted frame; neither FPS nor GPU time. *_mean_ms divides the summed stage duration by sample_count, *_samples counts the counted frames where the stage reported a nonzero duration");
        json.String("mmv_gpu_timing",
                    L"MMV runtime support has been removed; no MMV GPU timing is collected");
        json.End();
#ifdef MIKUMIKUSTUDIO_PERF
        json.Object("phases_ms_per_frame");
        for (int phase = 0; phase < perf::Count; ++phase) {
            char key[64];
            std::snprintf(key, sizeof key, "%s_inclusive_ms", kPhaseNames[phase]);
            json.Number(key, perf::milliseconds[phase] / static_cast<double>(times.size()));
            std::snprintf(key, sizeof key, "%s_calls", kPhaseNames[phase]);
            json.Integer(key, perf::calls[phase]);
        }
        json.End();
        json.Bool("perf_timing_compiled", true);
#else
        json.Bool("perf_timing_compiled", false);
#endif
        FILE* file = nullptr;
        if (_wfopen_s(&file, options.json, L"wb") || file == nullptr) {
            std::fputs("cannot open the JSON output file\n", stderr);
            jsonResult = kExitJson;
        } else {
            const std::string text = json.Finish();
            const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
            if (std::fclose(file) != 0 || written != text.size()) jsonResult = kExitJson;
            else std::printf("json=%ls\n", options.json);
        }
    }

    query->Release();
    if (mmvFailedFrames != 0) {
        std::fputs("MMV rejected or skipped sampled updates; performance result is invalid\n", stderr);
        if (jsonResult == 0) jsonResult = kExitMmvUpdate;
    }
    // Every part of the layout contract is a hard gate: a failed vertex
    // read-back, a layout that does not fit the view, or a copy whose sampled
    // centroid does not move by exactly the requested offset.
    if (layoutRequested && !(allCentersVisible && samplesComplete &&
                             placementVerified && framed)) {
        std::printf("layout verification failed: visible=%u samples=%u placement=%u framed=%u\n",
                    allCentersVisible ? 1u : 0u, samplesComplete ? 1u : 0u,
                    placementVerified ? 1u : 0u, framed ? 1u : 0u);
        ShutdownCleanup(app); delete app; g_Block=nullptr;
        return kExitVisibility;
    }
    // Export only after timing and layout checks. The process exits below;
    // no later sample can observe these changes to the pose and frame-0 keys.
    if (options.saveScene && jsonResult == 0) {
        if (wcslen(options.saveScene) >= 256 ||
            GetFileAttributesW(options.saveScene) != INVALID_FILE_ATTRIBUTES) {
            std::fputs("--save-scene needs a new path shorter than 256 characters\n", stderr);
            jsonResult = kExitSaveScene;
        } else {
            if (layoutRequested) for (const Copy& copy : copyRecords) {
                auto& model = *mdl::Mdl(copy.model);
                if (!model.boneTable || !model.boneKeys || !model.boneCount ||
                    model.boneCount > mdl::kBoneKeyCapacity || model.rigidCount != 0) {
                    std::fputs("scene export supports static models with a valid root/key table\n", stderr);
                    jsonResult = kExitSaveScene;
                    break;
                }
                unsigned roots = 0;
                for (unsigned bone = 0; bone < model.boneCount; ++bone) {
                    if (model.boneTable[bone].parent >= 0) continue;
                    ++roots;
                    for (int axis = 0; axis < 3; ++axis) {
                        model.boneTable[bone].trans[axis] += copy.offset[axis];
                        model.boneKeys[bone].position[axis] = model.boneTable[bone].trans[axis];
                    }
                }
                if (!roots) { jsonResult = kExitSaveScene; break; }
            }
            if (jsonResult == 0) {
                app->CurrentFrame() = 0;
                auto& camera = app->CameraKeys()[0];
                camera.distance = app->CameraDistance();
                for (int axis = 0; axis < 3; ++axis) {
                    camera.eye[axis] = app->CameraPositionAxis(axis);
                    camera.target[axis] = app->CameraRotation()[axis];
                }
                camera.fov = static_cast<int>(app->state.cameraFov);
                camera.perspective = app->CameraPerspective();
                camera.parentModel = app->CameraParentModel();
                camera.parentBone = app->CameraParentBone();
                app->FpsLimit() = 1000.f;
                SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStartFrameEdit), L"0");
                SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStopFrameEdit), L"300000");
                wcscpy_s(app->EnvFileName(), 256, options.saveScene);
                const bool savedOk = layoutRequested ? SaveSceneFile(app)
                    : SaveSceneCopy(app, options.saveScene);
                FILE* saved = nullptr;
                char header[24]{};
                if (!savedOk || _wfopen_s(&saved, options.saveScene, L"rb") != 0 || !saved) {
                    jsonResult = kExitSaveScene;
                } else {
                    const auto bytes = std::fread(header, 1, sizeof header, saved);
                    if (bytes != sizeof header ||
                        (std::memcmp(header, "Polygon Movie maker 0002", 23) != 0 &&
                         std::memcmp(header, "Polygon Movie maker 0003", 23) != 0))
                        jsonResult = kExitSaveScene;
                    std::fclose(saved);
                }
                std::printf("save_scene=%ls status=%d\n", options.saveScene, jsonResult);
            }
        }
    }
    ResetAppState(app); ShutdownCleanup(app); delete app; g_Block=nullptr;
    return jsonResult;
}
