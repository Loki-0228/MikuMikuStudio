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
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/sequential_file_reader.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <commctrl.h>

namespace mikumikustudio { void LoadSceneFile(); }
using namespace mikumikustudio;
#include "playback_benchmark.hpp"
unsigned loadDialogs=0;
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
    float camera[7]{}; // x,y,z,distance,yaw,pitch,roll; angles in degrees
    float orbitDegrees = 0;
    bool selfShadowGiven = false;
    int selfShadowMode = 0;
    int shadowRange = -1;
    bool lightGiven = false;
    float light[3]{};
    int fineShadow = -1;
    int adaptiveShadow = -1;
    int dynamicPrecision = -1;
    int sortPrecision = -1;
    int antialias = -1;
    int outlines = -1;
    int sortEnabled = -1;
    bool verifyViewToggles = false;
    bool verifyShadowCycle = false;
    bool verifyShadowZoom = false;
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
        } else if (wcscmp(arg, L"--fine-shadow") == 0) {
            const wchar_t* text = value(arg);
            long mode = 0;
            if (!text || !ParseLong(text, mode) || mode < 0 || mode > 1) return kExitUsage;
            options.fineShadow = static_cast<int>(mode);
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
                   wcscmp(arg, L"--sort-enabled") == 0) {
            long value = -1;
            if (++i >= argc || !ParseLong(argv[i], value) || value < 0 || value > 1) return kExitUsage;
            if (wcscmp(arg, L"--antialias") == 0) options.antialias = int(value);
            else if (wcscmp(arg, L"--outlines") == 0) options.outlines = int(value);
            else options.sortEnabled = int(value);
        } else if (wcscmp(arg, L"--verify-view-toggles") == 0) {
            options.verifyViewToggles = true;
        } else if (wcscmp(arg, L"--verify-shadow-cycle") == 0) {
            options.verifyShadowCycle = true;
        } else if (wcscmp(arg, L"--verify-shadow-zoom") == 0) {
            options.verifyShadowZoom = true;
        } else if (wcscmp(arg, L"--require-mmv") == 0) {
            options.requireMmv = true;
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
        } else if (wcscmp(arg, L"--camera") == 0 || wcscmp(arg, L"--orbit-degrees") == 0 || wcscmp(arg, L"--light") == 0) {
            const bool camera = wcscmp(arg, L"--camera") == 0;
            const bool light = wcscmp(arg, L"--light") == 0;
            const wchar_t* cursor = value(arg);
            if (!cursor) return kExitUsage;
            for (int field = 0; field < (camera ? 7 : light ? 3 : 1); ++field) {
                wchar_t* end = nullptr;
                const float parsed = std::wcstof(cursor, &end);
                const bool last = field == (camera ? 6 : light ? 2 : 0);
                if (end == cursor || !std::isfinite(parsed) ||
                    (last ? *end != L'\0' : *end != L',')) {
                    std::fputs("invalid camera/orbit/light value\n", stderr); return kExitUsage;
                }
                if (camera) options.camera[field] = parsed;
                else if (light) options.light[field] = parsed;
                else options.orbitDegrees = parsed;
                cursor = end + (last ? 0 : 1);
            }
            if (camera) options.cameraGiven = true;
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
            options.native || options.skinOnly || (!options.copiesGiven && !options.layoutGiven))) {
        std::fputs("--save-scene requires --throughput, a model file and an explicit layout/copy count\n",
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
    void Number(const char* key, double value) {
        Key(key);
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.6g", value);
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
    "screen_capture", "begin_scene", "effect_api"};
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
    const double loadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-loadStart).count();
    // Reinstall the same editor state before each frame: UI refreshes can
    // restore a keyframe's shadow mode or the selected model's bone mode.
    const auto installSelfShadow = [&] {
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
    if (options.cameraGiven) {
        app->CameraPositionX() = options.camera[0];
        app->CameraPositionY() = options.camera[1];
        app->CameraPositionZ() = options.camera[2];
        app->ViewOffsetX() = options.camera[0];
        app->ViewOffsetY() = options.camera[1];
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
        const bool sharedCoverage = app->ShadowDistance() == fixedRange &&
            std::memcmp(&fixedProjection, &app->LightViewProjection(), sizeof(fixedProjection)) == 0;
        precision(9500);
        capture(L".wide.png");
        const bool wideCoverage = std::abs(app->ShadowDistance() - .005f) < 1e-7f;
        precision(fixedPosition);
        toggle(false);
        const auto restored = capture(L".restored.png");
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
        json.Integer("shadow_width", app->Renderer()->renderTargetWidth);
        json.Bool("toggle_verification_passed", viewTogglesVerified);
        json.End();
        json.Object("self_shadow");
        json.Bool("adaptive_resolution", app->AdaptiveShadowResolution());
        json.Integer("shadow_slider", ShadowSliderPosition(app));
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
            for (const Copy& copy : copyRecords) {
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
                SaveSceneFile(app);
                FILE* saved = nullptr;
                char header[24]{};
                if (_wfopen_s(&saved, options.saveScene, L"rb") != 0 || !saved) {
                    jsonResult = kExitSaveScene;
                } else {
                    const auto bytes = std::fread(header, 1, sizeof header, saved);
                    if (bytes != sizeof header || std::memcmp(header, "Polygon Movie maker 0002", 23) != 0)
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
