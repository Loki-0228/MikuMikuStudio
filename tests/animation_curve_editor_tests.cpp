// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Background regression for the shared modeless curve editor
// (src/window/animation_curve_editor.cpp).  Everything runs without a message
// loop: most fixtures create hidden windows; visible timer and focus cases
// control visibility explicitly. No desktop pixel is read, no GPU device is created and no message box is
// expected.  Real callbacks are verified through the real WM_COMMAND path.
//
// The control ids and the WM_APP test hooks are mirrored from the comment block
// at the top of src/window/animation_curve_editor.cpp; keep the two lists in
// step when either changes.  The hooks are read-only geometry queries:
//   kHookPointForKey   WM_APP+300  wParam = key index -> MAKELPARAM(client x, y)
//   kHookPointForFrame WM_APP+301  wParam = frame     -> MAKELPARAM(client x, y)
//   kHookMmdControls   WM_APP+302  lParam = double[4] -> TRUE when the MMD
//                                  panel can show the selected interval
//   kHookKeyCount      WM_APP+303  wParam = channel index or -1 -> key count
//
// Verification: hidden create/destroy, add/delete/undo/redo through the real
// apply callback, duplicate-frame rejection, two independent sources, a failing
// apply that must roll back, a source without callbacks that must stay disabled,
// MMD diagonal handles for a Linear interval, disabled MMD panel for an
// unrepresentable interval, the launcher icon geometry for the 22 and 18 pixel
// buttons, and the three-language layout (no overlap, >= 4 logical pixel label
// gaps, >= 4 logical pixel edit margins). Close/reopen paint and handle-drag
// damage are measured separately on a generic value curve: the canonical
// non-linear native-control geometry is rendered before any edit through both
// WM_PRINTCLIENT and the buffered WM_PAINT, and its painted height at interior
// frames is compared with the canonical Bezier so a straightened chord cannot
// pass as a curve. The fixture does not model morph/expression persistence;
// that channel identity path is host-side. Each real handle drag may damage
// only the surface it owns.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <TlHelp32.h>
#include <Shellapi.h>

#include "mikumikustudio/animation_curve_editor.hpp"
#include "../src/window/curve_editor_axis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Armed only after the host has accepted a dedicated write-lifetime fixture.
// Fail exactly one C++ allocation on this thread, then restore the allocator
// before the editor reports the failure or the fixture inspects its state.
thread_local bool g_curveFailNextAllocation=false;
thread_local unsigned g_curveAllocationFailures=0;
thread_local bool g_curveMeasureAllocations=false;
thread_local std::size_t g_curveAllocationBytes=0,g_curveLargestAllocation=0;
void* operator new(std::size_t size){
    if(g_curveFailNextAllocation){g_curveFailNextAllocation=false;++g_curveAllocationFailures;throw std::bad_alloc();}
    if(g_curveMeasureAllocations){g_curveAllocationBytes+=size;
        g_curveLargestAllocation=std::max(g_curveLargestAllocation,size);}
    if(void* value=std::malloc(size?size:1))return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
void operator delete[](void* value,std::size_t)noexcept{std::free(value);}

namespace ce = mikumikustudio::curve_editor;
namespace ac = mikumikustudio::animation_curve;

namespace {

// Control ids, mirrored from src/window/animation_curve_editor.cpp.
enum : int {
    kIdSourceLabel = 1001, kIdSourceCombo = 1002,
    kIdReadMotion = 1003, kIdWriteMotion = 1004,
    kIdViewLabel = 1006, kIdViewCombo = 1007, kIdToolLabel = 1008, kIdToolCombo = 1009,
    kIdFitAll = 1010, kIdFitRange = 1011, kIdShortcutToggle = 1012, kIdShortcutText = 1013,
    kIdChannelLabel = 1014, kIdChannelList = 1015, kIdChannelAll = 1016, kIdChannelInvert = 1017,
    kIdCanvas = 1020, kIdPlay = 1021, kIdPreviousFrame = 1022, kIdNextFrame = 1023,
    kIdTimelineFrame = 1024, kIdSeekFrame = 1025, kIdPreviousKey = 1026, kIdNextKey = 1027,
    kIdMmdLabel = 1030, kIdMmdPanel = 1031, kIdMmdReadout = 1032,
    kIdFrameLabel = 1040, kIdFrameEdit = 1041,
    kIdValueLabel = 1042, kIdValueEdit = 1043,
    kIdInterpolationLabel = 1044, kIdInterpolationCombo = 1045,
    kIdTangentLabel = 1046, kIdTangentCombo = 1047,
    kIdInSlopeLabel = 1048, kIdInSlopeEdit = 1049,
    kIdOutSlopeLabel = 1050, kIdOutSlopeEdit = 1051,
    kIdInWeightLabel = 1052, kIdInWeightEdit = 1053,
    kIdOutWeightLabel = 1054, kIdOutWeightEdit = 1055,
    kIdAddKey = 1056, kIdDeleteKey = 1057, kIdApplyKey = 1058,
    kIdUndo = 1059, kIdRedo = 1060,
    kIdBakeFirstLabel = 1070, kIdBakeFirstEdit = 1071,
    kIdBakeLastLabel = 1072, kIdBakeLastEdit = 1073,
    kIdBakeStepLabel = 1074, kIdBakeStepEdit = 1075,
    kIdBakeRun = 1076,
    kIdStatus = 1080,
    kIdCompareLabel = 1081, kIdCompareSameGroup = 1082,
    kIdCompareFilter = 1083,
    kIdHoverHint = 1085, kIdCompareAllGroups = 1086, kIdCompareFree = 1087,
};

constexpr UINT kHookPointForKey = WM_APP + 300;
constexpr UINT kHookPointForFrame = WM_APP + 301;
constexpr UINT kHookMmdControls = WM_APP + 302;
constexpr UINT kHookKeyCount = WM_APP + 303;
constexpr UINT kHookView = WM_APP + 304;
constexpr UINT kHookBarPoint = WM_APP + 305;
constexpr UINT kHookFocusId = WM_APP + 306;
constexpr UINT kHookPlayheadState = WM_APP + 307;
constexpr UINT kHookCompare = WM_APP + 308;
constexpr UINT kHookCompareRange = WM_APP + 309;
constexpr UINT kHookChannelVisible = WM_APP + 310;
constexpr UINT kHookHoverHint = WM_APP + 311;
constexpr UINT kHookNativeConstruction = WM_APP + 398;
constexpr UINT kPlayheadTimerId = 1;
constexpr double kMinimumTimeSpan = 1.0;
constexpr double kMinimumValueSpan = 1.0e-5;

int g_checks = 0;

void Check(bool condition, const char *description) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::fflush(stderr);
        std::exit(1);
    }
}

bool g_validateHeap=false;
void Note(const char *description) {
    if(g_validateHeap&&!HeapValidate(GetProcessHeap(),0,nullptr)){
        std::fprintf(stderr,"FAIL process heap after %s\n",description);std::fflush(stderr);std::exit(1);
    }
    std::printf("PASS %s\n", description);std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Optional first-chance 0xC0000005 diagnostics.
//
// Inert unless MMS_CURVE_CRASH_DIAGNOSTICS is exactly "1": no module snapshot,
// no handler registration, no ring writes and no output.  The vector handler is
// observe-only; it always returns EXCEPTION_CONTINUE_SEARCH, never edits
// CONTEXT and never resumes, swallows, exits or translates the exception, so
// the normal SEH/debugger chain and termination stay untouched.  A record
// labelled FIRST_CHANCE_AV is a first-chance observation, not a proven fatal
// or unhandled exit.
//
// Modules come from bounded Toolhelp snapshots published as immutable caches.
// The startup snapshot is published before the tests; ComparisonCallbackLifetime
// builds the other slot once and publishes it with one interlocked store, so the
// handler reads a stable snapshot without loader calls, locks or allocation.
// Later refresh requests are ignored on purpose: only the startup and the one
// comparison snapshot can ever be visible, so a published slot is never reused.
// Modules loaded after that snapshot, and names or paths whose UTF-8 form does
// not fit the bounded field, are reported as unknown or degraded.
//
// The comparison ring is a fixed POD array per thread written with scalar
// stores only.  Each slot is invalidated, filled and committed before it is
// counted, so a fault during a wrapped overwrite cannot expose a partial record.
// The handler dumps the bounded recent window oldest-to-newest; it names
// completed stages, not the exact faulting instruction, so a fault inside a
// stage is attributed to the preceding boundary.  A thread with no published
// slot reports no comparison history rather than borrowing another thread's.
// ---------------------------------------------------------------------------
enum : unsigned {
    kCurveDiagModuleCapacity = 96,
    kCurveDiagModuleNameChars = 64,
    kCurveDiagModulePathChars = 260,
    kCurveDiagModuleCacheCount = 2,
    kCurveDiagStageCapacity = 32,
    kCurveDiagReportCapacity = 2048,
    kCurveDiagStageLineCapacity = 320,
};
enum : unsigned {
    kCurveDiagModuleGenerationStartup = 1,
    kCurveDiagModuleGenerationComparison = 2,
};
struct CurveDiagModuleEntry {
    std::uintptr_t base;
    std::uintptr_t end;
    unsigned nameLength;
    unsigned pathLength;
    char name[kCurveDiagModuleNameChars];
    char path[kCurveDiagModulePathChars];
};
// ready==0 while a slot is being rebuilt; the published index is flipped only
// after ready==1, so the handler never reads a partially filled entry array.
struct CurveDiagModuleCache {
    volatile LONG ready;
    unsigned count;
    unsigned truncated;
    unsigned partial;
    unsigned unavailable;
    unsigned encodingDegraded;
    unsigned generation;
    CurveDiagModuleEntry entries[kCurveDiagModuleCapacity];
};
CurveDiagModuleCache g_curveDiagModuleCaches[kCurveDiagModuleCacheCount];
volatile LONG g_curveDiagPublishedModuleCache = -1;
volatile LONG g_curveDiagModuleRefreshDone = 0;
bool g_curveDiagEnabled = false;
HANDLE g_curveDiagStderr = INVALID_HANDLE_VALUE;
PVOID g_curveDiagHandlerHandle = nullptr;
volatile LONG g_curveDiagHandlerBusy = 0;
char g_curveDiagReportBuffer[kCurveDiagReportCapacity];
char g_curveDiagStageLineBuffer[kCurveDiagStageLineCapacity];

// Cached once at iteration start; provider/copy/retire stages read this instead
// of touching an object that the fixture may already have retired.
struct CurveDiagCase {
    int route, kind, mouse, phase, action, response;
};
thread_local CurveDiagCase g_curveDiagCase;

enum CurveDiagStageId : int {
    kCurveDiagStageIterationBegin = 1,
    kCurveDiagStageInitialOpenBegin,
    kCurveDiagStageInitialOpenEnd,
    kCurveDiagStageTriggerBegin,
    kCurveDiagStageTriggerEnd,
    kCurveDiagStageProviderCopyEntry,
    kCurveDiagStageProviderCopyAfterRetire,
    kCurveDiagStageRetireBegin,
    kCurveDiagStageRetireEnd,
    kCurveDiagStageRetireCloseOwnedBegin,
    kCurveDiagStageRetireCloseOwnedEnd,
    kCurveDiagStageRetireReplacementOpenBegin,
    kCurveDiagStageRetireReplacementOpenEnd,
    kCurveDiagStageReplacementKeyClickBegin,
    kCurveDiagStageReplacementKeyClickEnd,
    kCurveDiagStageReplacementDeleteBegin,
    kCurveDiagStageReplacementDeleteEnd,
    kCurveDiagStageFinalCloseOwnedBegin,
    kCurveDiagStageFinalCloseOwnedEnd,
    kCurveDiagStageIterationEnd,
};
struct CurveDiagStageEntry {
    unsigned sequence;
    int stage;
    int route, kind, mouse, phase, action, response;
    std::uintptr_t pointerA;
    std::uintptr_t pointerB;
    unsigned reserved;
    // Commit marker: -1 while a wrapped slot is being overwritten, then the
    // slot sequence.  The handler emits a slot only when this equals the
    // sequence it expects, so a partial overwrite is never reported.
    volatile LONG committed;
};
struct CurveDiagStageRing {
    volatile LONG published;
    CurveDiagStageEntry entries[kCurveDiagStageCapacity];
};
thread_local CurveDiagStageRing g_curveDiagStageRing;

// Small bounded append helpers; the handler must not use printf/snprintf or
// touch the heap, so it formats into this fixed buffer only.
struct CurveDiagText {
    char *data;
    unsigned capacity;
    unsigned length;
    bool truncated;
};
void CurveDiagTextInit(CurveDiagText &text, char *data, unsigned capacity) {
    text.data = data;
    text.capacity = capacity;
    text.length = 0;
    text.truncated = false;
    if (data != nullptr && capacity != 0) data[0] = '\0';
}
void CurveDiagTextChar(CurveDiagText &text, char value) {
    if (text.data == nullptr || text.length + 1 >= text.capacity) {
        text.truncated = true;
        return;
    }
    text.data[text.length++] = value;
    text.data[text.length] = '\0';
}
void CurveDiagTextLiteral(CurveDiagText &text, const char *value) {
    if (value == nullptr) return;
    for (unsigned index = 0; value[index] != '\0'; ++index) CurveDiagTextChar(text, value[index]);
}
void CurveDiagTextLength(CurveDiagText &text, const char *value, unsigned length) {
    if (value == nullptr) return;
    for (unsigned index = 0; index < length; ++index) CurveDiagTextChar(text, value[index]);
}
void CurveDiagTextUnsigned(CurveDiagText &text, unsigned long long value) {
    char digits[24];
    unsigned count = 0;
    if (value == 0) digits[count++] = '0';
    while (value != 0) {
        digits[count++] = static_cast<char>('0' + static_cast<unsigned>(value % 10));
        value /= 10;
    }
    while (count != 0) CurveDiagTextChar(text, digits[--count]);
}
void CurveDiagTextSigned(CurveDiagText &text, long long value) {
    if (value < 0) {
        CurveDiagTextChar(text, '-');
        CurveDiagTextUnsigned(text, static_cast<unsigned long long>(-(value + 1)) + 1ULL);
        return;
    }
    CurveDiagTextUnsigned(text, static_cast<unsigned long long>(value));
}
void CurveDiagTextHex(CurveDiagText &text, unsigned long long value) {
    CurveDiagTextLiteral(text, "0x");
    char digits[16];
    unsigned count = 0;
    if (value == 0) digits[count++] = '0';
    while (value != 0) {
        const unsigned nibble = static_cast<unsigned>(value & 0xFULL);
        digits[count++] = static_cast<char>(nibble < 10U ? ('0' + static_cast<int>(nibble))
                                                        : ('A' + static_cast<int>(nibble - 10U)));
        value >>= 4;
    }
    while (count != 0) CurveDiagTextChar(text, digits[--count]);
}

// The handler writes only complete bounded buffers; nothing here allocates or
// formats through printf.
void CurveDiagWriteText(CurveDiagText &text) {
    if (text.data == nullptr || text.length == 0) return;
    if (g_curveDiagStderr == nullptr || g_curveDiagStderr == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_curveDiagStderr, text.data, text.length, &written, nullptr);
}

const char *CurveDiagStageName(int stage) {
    switch (stage) {
    case kCurveDiagStageIterationBegin: return "IterationBegin";
    case kCurveDiagStageInitialOpenBegin: return "InitialOpenBegin";
    case kCurveDiagStageInitialOpenEnd: return "InitialOpenEnd";
    case kCurveDiagStageTriggerBegin: return "TriggerBegin";
    case kCurveDiagStageTriggerEnd: return "TriggerEnd";
    case kCurveDiagStageProviderCopyEntry: return "ProviderCopyEntry";
    case kCurveDiagStageProviderCopyAfterRetire: return "ProviderCopyAfterRetire";
    case kCurveDiagStageRetireBegin: return "RetireBegin";
    case kCurveDiagStageRetireEnd: return "RetireEnd";
    case kCurveDiagStageRetireCloseOwnedBegin: return "RetireCloseOwnedBegin";
    case kCurveDiagStageRetireCloseOwnedEnd: return "RetireCloseOwnedEnd";
    case kCurveDiagStageRetireReplacementOpenBegin: return "RetireReplacementOpenBegin";
    case kCurveDiagStageRetireReplacementOpenEnd: return "RetireReplacementOpenEnd";
    case kCurveDiagStageReplacementKeyClickBegin: return "ReplacementKeyClickBegin";
    case kCurveDiagStageReplacementKeyClickEnd: return "ReplacementKeyClickEnd";
    case kCurveDiagStageReplacementDeleteBegin: return "ReplacementDeleteBegin";
    case kCurveDiagStageReplacementDeleteEnd: return "ReplacementDeleteEnd";
    case kCurveDiagStageFinalCloseOwnedBegin: return "FinalCloseOwnedBegin";
    case kCurveDiagStageFinalCloseOwnedEnd: return "FinalCloseOwnedEnd";
    case kCurveDiagStageIterationEnd: return "IterationEnd";
    default: return "Unknown";
    }
}

void CurveDiagSetCase(int route, int kind, int mouse, int phase, int action, int response) {
    if (!g_curveDiagEnabled) return;
    g_curveDiagCase.route = route;
    g_curveDiagCase.kind = kind;
    g_curveDiagCase.mouse = mouse;
    g_curveDiagCase.phase = phase;
    g_curveDiagCase.action = action;
    g_curveDiagCase.response = response;
}
// Fill the next slot with scalars, commit it, then publish the total.  The
// wrapped slot is invalidated before it is overwritten, so an AV in the middle
// of a record leaves at most the previously committed slots visible.
void CurveDiagStage(CurveDiagStageId stage, std::uintptr_t pointerA = 0, std::uintptr_t pointerB = 0) {
    if (!g_curveDiagEnabled) return;
    const LONG published = g_curveDiagStageRing.published;
    CurveDiagStageEntry &entry =
        g_curveDiagStageRing.entries[static_cast<unsigned>(published) % kCurveDiagStageCapacity];
    InterlockedExchange(&entry.committed, -1);
    entry.sequence = static_cast<unsigned>(published);
    entry.stage = stage;
    entry.route = g_curveDiagCase.route;
    entry.kind = g_curveDiagCase.kind;
    entry.mouse = g_curveDiagCase.mouse;
    entry.phase = g_curveDiagCase.phase;
    entry.action = g_curveDiagCase.action;
    entry.response = g_curveDiagCase.response;
    entry.pointerA = pointerA;
    entry.pointerB = pointerB;
    entry.reserved = 0;
    InterlockedExchange(&entry.committed, published);
    InterlockedIncrement(&g_curveDiagStageRing.published);
}

// Returns the UTF-8 length, or 0 with *degraded set when the conversion fails
// or the bounded field is too small.  The cache records that degradation instead
// of silently presenting an empty name/path as a complete conversion.
unsigned CurveDiagUtf8FromWide(char *destination, unsigned capacity, const wchar_t *source, unsigned *degraded) {
    if (destination == nullptr || capacity == 0) {
        if (degraded != nullptr) *degraded = 1;
        return 0;
    }
    destination[0] = '\0';
    if (source == nullptr) return 0;
    const int converted =
        WideCharToMultiByte(CP_UTF8, 0, source, -1, destination, static_cast<int>(capacity), nullptr, nullptr);
    if (converted <= 0) {
        destination[0] = '\0';
        if (degraded != nullptr) *degraded = 1;
        return 0;
    }
    const unsigned length = static_cast<unsigned>(converted - 1);
    if (length >= capacity) {
        destination[capacity - 1] = '\0';
        if (degraded != nullptr) *degraded = 1;
        return capacity - 1;
    }
    return length;
}

void CurveDiagBuildModuleCache(CurveDiagModuleCache &cache, unsigned generation, DWORD *failure) {
    if (failure != nullptr) *failure = ERROR_SUCCESS;
    cache.ready = 0;
    cache.count = 0;
    cache.truncated = 0;
    cache.partial = 0;
    cache.unavailable = 0;
    cache.encodingDegraded = 0;
    cache.generation = generation;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (failure != nullptr) *failure = GetLastError();
        cache.unavailable = 1;
        InterlockedExchange(&cache.ready, 1);
        return;
    }
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    BOOL more = Module32FirstW(snapshot, &module);
    if (!more) {
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
            cache.partial = 1;
            if (failure != nullptr) *failure = error;
        }
    }
    while (more) {
        if (cache.count >= kCurveDiagModuleCapacity) {
            cache.truncated = 1;
            break;
        }
        CurveDiagModuleEntry &entry = cache.entries[cache.count];
        entry.base = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
        entry.end = entry.base + static_cast<std::uintptr_t>(module.modBaseSize);
        unsigned encodingDegraded = 0;
        entry.nameLength = CurveDiagUtf8FromWide(entry.name, kCurveDiagModuleNameChars, module.szModule,
                                                &encodingDegraded);
        entry.pathLength = CurveDiagUtf8FromWide(entry.path, kCurveDiagModulePathChars, module.szExePath,
                                                 &encodingDegraded);
        if (encodingDegraded != 0) cache.encodingDegraded = 1;
        ++cache.count;
        more = Module32NextW(snapshot, &module);
        if (!more) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                cache.partial = 1;
                if (failure != nullptr) *failure = error;
            }
        }
    }
    CloseHandle(snapshot);
    if (cache.count == 0 && cache.partial == 0) cache.unavailable = 1;
    // Full barrier: entries are complete before ready flips and before the
    // published index can point at this slot.
    InterlockedExchange(&cache.ready, 1);
}

const CurveDiagModuleEntry *CurveDiagFindModule(const CurveDiagModuleCache &cache, std::uintptr_t address) {
    if (cache.ready == 0) return nullptr;
    const unsigned count = cache.count < kCurveDiagModuleCapacity ? cache.count : kCurveDiagModuleCapacity;
    for (unsigned index = 0; index < count; ++index) {
        const CurveDiagModuleEntry &entry = cache.entries[index];
        if (address >= entry.base && address < entry.end) return &entry;
    }
    return nullptr;
}

const char *CurveDiagModuleState(const CurveDiagModuleCache &cache) {
    if (cache.ready == 0 || cache.unavailable != 0) return "unavailable";
    if (cache.partial != 0 || cache.encodingDegraded != 0) return "incomplete";
    if (cache.truncated != 0) return "truncated";
    return "complete";
}

// Safe-path warning only (never called from the handler): a degraded cache is
// reported explicitly instead of pretending module attribution works.
void CurveDiagWarnModuleCache(const char *when, const CurveDiagModuleCache &cache, DWORD failure) {
    const bool degraded = failure != ERROR_SUCCESS || cache.unavailable != 0 || cache.partial != 0 ||
                          cache.truncated != 0 || cache.encodingDegraded != 0;
    if (!degraded) return;
    std::fprintf(stderr, "CURVE_CRASH_DIAG warning: %s module cache %s", when,
                 CurveDiagModuleState(cache));
    if (failure != ERROR_SUCCESS)
        std::fprintf(stderr, " (GetLastError=%lu)", static_cast<unsigned long>(failure));
    std::fprintf(stderr, "; raw addresses without a cached module stay labelled unknown\n");
    std::fflush(stderr);
}

void CurveDiagAppendModule(CurveDiagText &text, std::uintptr_t address, const CurveDiagModuleCache *cache,
                           const CurveDiagModuleEntry **matchOut) {
    const CurveDiagModuleEntry *match = cache != nullptr ? CurveDiagFindModule(*cache, address) : nullptr;
    if (matchOut != nullptr) *matchOut = match;
    if (match == nullptr) {
        CurveDiagTextLiteral(text, "name=unknown base=unknown end=unknown rva=unknown");
        return;
    }
    CurveDiagTextLiteral(text, "name=");
    if (match->nameLength != 0)
        CurveDiagTextLength(text, match->name, match->nameLength);
    else
        CurveDiagTextLiteral(text, "unknown");
    CurveDiagTextLiteral(text, " base=");
    CurveDiagTextHex(text, match->base);
    CurveDiagTextLiteral(text, " end=");
    CurveDiagTextHex(text, match->end);
    CurveDiagTextLiteral(text, " rva=");
    CurveDiagTextHex(text, address - match->base);
}

void CurveDiagReportAccessViolation(PEXCEPTION_POINTERS exceptionInfo) {
    if (exceptionInfo == nullptr || exceptionInfo->ExceptionRecord == nullptr) return;
    const EXCEPTION_RECORD &record = *exceptionInfo->ExceptionRecord;
    const std::uintptr_t exceptionAddress = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    std::uintptr_t instruction = 0;
    std::uintptr_t stackPointer = 0;
    bool haveContext = false;
    const CONTEXT *context = exceptionInfo->ContextRecord;
    if (context != nullptr) {
#if defined(_M_X64)
        instruction = static_cast<std::uintptr_t>(context->Rip);
        stackPointer = static_cast<std::uintptr_t>(context->Rsp);
        haveContext = true;
#elif defined(_M_IX86)
        instruction = static_cast<std::uintptr_t>(context->Eip);
        stackPointer = static_cast<std::uintptr_t>(context->Esp);
        haveContext = true;
#endif
    }

    CurveDiagText text;
    CurveDiagTextInit(text, g_curveDiagReportBuffer, kCurveDiagReportCapacity);
    // Raw header first, so a bounded/truncated tail still names the record.
    CurveDiagTextLiteral(text, "CURVE_CRASH_DIAG FIRST_CHANCE_AV pid=");
    CurveDiagTextUnsigned(text, GetCurrentProcessId());
    CurveDiagTextLiteral(text, " tid=");
    CurveDiagTextUnsigned(text, GetCurrentThreadId());
    CurveDiagTextLiteral(text, " code=");
    CurveDiagTextHex(text, record.ExceptionCode);
    CurveDiagTextLiteral(text, " address=");
    CurveDiagTextHex(text, exceptionAddress);
    CurveDiagTextLiteral(text, "\ncontext ip=");
    if (haveContext)
        CurveDiagTextHex(text, instruction);
    else
        CurveDiagTextLiteral(text, "unknown");
    CurveDiagTextLiteral(text, " sp=");
    if (haveContext)
        CurveDiagTextHex(text, stackPointer);
    else
        CurveDiagTextLiteral(text, "unknown");
    CurveDiagTextLiteral(text, "\naccess=");
    if (record.NumberParameters >= 1) {
        const ULONG_PTR operation = record.ExceptionInformation[0];
        if (operation == 0)
            CurveDiagTextLiteral(text, "read");
        else if (operation == 1)
            CurveDiagTextLiteral(text, "write");
        else if (operation == 8)
            CurveDiagTextLiteral(text, "execute");
        else {
            CurveDiagTextLiteral(text, "unknown(");
            CurveDiagTextUnsigned(text, static_cast<unsigned long long>(operation));
            CurveDiagTextLiteral(text, ")");
        }
    } else {
        CurveDiagTextLiteral(text, "unknown");
    }
    CurveDiagTextLiteral(text, " fault=");
    if (record.NumberParameters >= 2)
        CurveDiagTextHex(text, static_cast<unsigned long long>(record.ExceptionInformation[1]));
    else
        CurveDiagTextLiteral(text, "unknown");

    const LONG published = InterlockedCompareExchange(&g_curveDiagPublishedModuleCache, -1, -1);
    const CurveDiagModuleCache *cache = nullptr;
    if (published >= 0 && published < static_cast<LONG>(kCurveDiagModuleCacheCount))
        cache = &g_curveDiagModuleCaches[published];
    const CurveDiagModuleEntry *addressModule = nullptr;
    CurveDiagTextLiteral(text, "\nmodule address: ");
    CurveDiagAppendModule(text, exceptionAddress, cache, &addressModule);
    if (addressModule != nullptr && addressModule->pathLength != 0) {
        CurveDiagTextLiteral(text, " path=");
        CurveDiagTextLength(text, addressModule->path, addressModule->pathLength);
    }
    CurveDiagTextLiteral(text, "\nmodule ip: ");
    if (haveContext)
        CurveDiagAppendModule(text, instruction, cache, nullptr);
    else
        CurveDiagTextLiteral(text, "name=unknown base=unknown end=unknown rva=unknown");
    CurveDiagTextLiteral(text, "\nmodule_cache=");
    if (cache == nullptr) {
        CurveDiagTextLiteral(text, "unknown");
    } else {
        CurveDiagTextLiteral(text, cache->generation == kCurveDiagModuleGenerationComparison ? "comparison"
                                                                                           : "startup");
        if (cache->unavailable != 0) CurveDiagTextLiteral(text, " unavailable");
        if (cache->partial != 0) CurveDiagTextLiteral(text, " incomplete");
        if (cache->truncated != 0) CurveDiagTextLiteral(text, " truncated");
        if (cache->encodingDegraded != 0) CurveDiagTextLiteral(text, " encoding-degraded");
        CurveDiagTextLiteral(text, " count=");
        CurveDiagTextUnsigned(text, cache->count);
    }

    CurveDiagTextLiteral(text, "\n");
    // Raw header first; the bounded recent ring follows as one line per slot.
    CurveDiagWriteText(text);

    const LONG total = InterlockedCompareExchange(&g_curveDiagStageRing.published, 0, 0);
    if (total <= 0) {
        CurveDiagText line;
        CurveDiagTextInit(line, g_curveDiagStageLineBuffer, kCurveDiagStageLineCapacity);
        CurveDiagTextLiteral(line, "COMPARISON_RING none (no stage published on this thread)\n");
        CurveDiagWriteText(line);
        return;
    }
    const LONG first = total > static_cast<LONG>(kCurveDiagStageCapacity)
                           ? total - static_cast<LONG>(kCurveDiagStageCapacity)
                           : 0;
    for (LONG sequence = first; sequence < total; ++sequence) {
        CurveDiagStageEntry &entry =
            g_curveDiagStageRing.entries[static_cast<unsigned>(sequence) % kCurveDiagStageCapacity];
        // Skip a wrapped slot that is being overwritten and is not committed yet;
        // the expected sequence can only match the last fully published record.
        const LONG committed = InterlockedCompareExchange(&entry.committed, sequence, sequence);
        if (committed != sequence) continue;
        CurveDiagText line;
        CurveDiagTextInit(line, g_curveDiagStageLineBuffer, kCurveDiagStageLineCapacity);
        CurveDiagTextLiteral(line, "COMPARISON_RING seq=");
        CurveDiagTextUnsigned(line, static_cast<unsigned long long>(sequence));
        CurveDiagTextLiteral(line, " stage=");
        CurveDiagTextLiteral(line, CurveDiagStageName(entry.stage));
        CurveDiagTextLiteral(line, " route=");
        CurveDiagTextSigned(line, entry.route);
        CurveDiagTextLiteral(line, " kind=");
        CurveDiagTextSigned(line, entry.kind);
        CurveDiagTextLiteral(line, " mouse=");
        CurveDiagTextSigned(line, entry.mouse);
        CurveDiagTextLiteral(line, " phase=");
        CurveDiagTextSigned(line, entry.phase);
        CurveDiagTextLiteral(line, " action=");
        CurveDiagTextSigned(line, entry.action);
        CurveDiagTextLiteral(line, " response=");
        CurveDiagTextSigned(line, entry.response);
        CurveDiagTextLiteral(line, " ptrA=");
        CurveDiagTextHex(line, entry.pointerA);
        CurveDiagTextLiteral(line, " ptrB=");
        CurveDiagTextHex(line, entry.pointerB);
        CurveDiagTextChar(line, '\n');
        CurveDiagWriteText(line);
    }
}

// Observe-only handler: no context edits, no resume/translation, no locks, no
// loader calls, no allocation, no output calls other than WriteFile on the
// stderr handle cached during safe setup.  Every path continues the search.
LONG NTAPI CurveDiagVectoredHandler(PEXCEPTION_POINTERS exceptionInfo) {
    if (InterlockedCompareExchange(&g_curveDiagHandlerBusy, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD savedError = GetLastError();
    if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr &&
        exceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        CurveDiagReportAccessViolation(exceptionInfo);
    }
    SetLastError(savedError);
    InterlockedExchange(&g_curveDiagHandlerBusy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

void CurveDiagStartup() {
    const DWORD savedError = GetLastError();
    g_curveDiagEnabled = false;
    char switchValue[8] = {};
    const DWORD switchLength =
        GetEnvironmentVariableA("MMS_CURVE_CRASH_DIAGNOSTICS", switchValue, static_cast<DWORD>(sizeof(switchValue)));
    const bool requested = switchLength == 1 && switchValue[0] == '1' && switchValue[1] == '\0';
    if (requested) {
        g_curveDiagStderr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD enumerationError = ERROR_SUCCESS;
        CurveDiagBuildModuleCache(g_curveDiagModuleCaches[0], kCurveDiagModuleGenerationStartup, &enumerationError);
        InterlockedExchange(&g_curveDiagPublishedModuleCache, 0);
        CurveDiagWarnModuleCache("startup", g_curveDiagModuleCaches[0], enumerationError);
        if (g_curveDiagStderr == nullptr || g_curveDiagStderr == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "CURVE_CRASH_DIAG warning: STD_ERROR_HANDLE is unavailable; diagnostics stay off\n");
            std::fflush(stderr);
        } else {
            g_curveDiagHandlerHandle = AddVectoredExceptionHandler(1, CurveDiagVectoredHandler);
            if (g_curveDiagHandlerHandle == nullptr) {
                const DWORD registrationError = GetLastError();
                std::fprintf(stderr,
                             "CURVE_CRASH_DIAG warning: AddVectoredExceptionHandler failed with GetLastError=%lu; "
                             "diagnostics stay off and no first-chance output will be produced\n",
                             static_cast<unsigned long>(registrationError));
                std::fflush(stderr);
            } else {
                g_curveDiagEnabled = true;
                std::fprintf(stderr, "CURVE_CRASH_DIAG armed pid=%lu tid=%lu modules=%u cache=%s\n",
                             static_cast<unsigned long>(GetCurrentProcessId()),
                             static_cast<unsigned long>(GetCurrentThreadId()),
                             g_curveDiagModuleCaches[0].count, CurveDiagModuleState(g_curveDiagModuleCaches[0]));
                std::fflush(stderr);
            }
        }
    }
    SetLastError(savedError);
}

void CurveDiagShutdown() {
    const DWORD savedError = GetLastError();
    if (g_curveDiagHandlerHandle != nullptr) {
        RemoveVectoredExceptionHandler(g_curveDiagHandlerHandle);
        g_curveDiagHandlerHandle = nullptr;
    }
    g_curveDiagEnabled = false;
    SetLastError(savedError);
}

// Called once at ComparisonCallbackLifetime entry, before any iteration or
// callback can fault.  Exactly one refresh is allowed: the startup snapshot is
// already published and this call builds the other, still invisible slot, then
// flips the index.  Any later request is a no-op, so a slot the handler may have
// observed is never rebuilt in place.  A failed refresh keeps the startup
// snapshot and says so.
void CurveDiagRefreshModules() {
    if (!g_curveDiagEnabled) return;
    const DWORD savedError = GetLastError();
    const LONG published = InterlockedCompareExchange(&g_curveDiagPublishedModuleCache, -1, -1);
    if (published < 0 || published >= static_cast<LONG>(kCurveDiagModuleCacheCount)) {
        SetLastError(savedError);
        return;
    }
    if (InterlockedCompareExchange(&g_curveDiagModuleRefreshDone, 1, 0) != 0) {
        SetLastError(savedError);
        return;
    }
    const LONG target = (published + 1) % static_cast<LONG>(kCurveDiagModuleCacheCount);
    DWORD enumerationError = ERROR_SUCCESS;
    CurveDiagBuildModuleCache(g_curveDiagModuleCaches[target], kCurveDiagModuleGenerationComparison,
                              &enumerationError);
    if (g_curveDiagModuleCaches[target].unavailable != 0 && enumerationError != ERROR_SUCCESS) {
        std::fprintf(stderr,
                     "CURVE_CRASH_DIAG warning: comparison module snapshot failed (GetLastError=%lu); keeping the "
                     "startup module cache (stale)\n",
                     static_cast<unsigned long>(enumerationError));
        std::fflush(stderr);
    } else {
        InterlockedExchange(&g_curveDiagPublishedModuleCache, target);
        CurveDiagWarnModuleCache("comparison", g_curveDiagModuleCaches[target], enumerationError);
    }
    SetLastError(savedError);
}

struct CurveDiagLifetime {
    CurveDiagLifetime() { CurveDiagStartup(); }
    ~CurveDiagLifetime() { CurveDiagShutdown(); }
    CurveDiagLifetime(const CurveDiagLifetime &) = delete;
    CurveDiagLifetime &operator=(const CurveDiagLifetime &) = delete;
};

// ---------------------------------------------------------------------------
// Host-side stub source.
// ---------------------------------------------------------------------------
struct HostSource {
    std::vector<ce::Channel> data;
    int readCalls = 0;
    int applyCalls = 0;
    bool failApply = false;
    bool failRead = false;
    int readMotionCalls = 0;
    int writeMotionCalls = 0;
    bool hasReadMotion = false;
    bool hasWriteMotion = false;
    std::vector<std::vector<ce::Channel>> applied;
    // Comparison display metadata.  storedMode is what the host remembers and
    // hands back on every open, modeWrites counts the accepted changes.
    bool compareEnabled = false;
    bool hasModeCallbacks = false;
    std::string initialMain;
    int storedMode = ce::kCompareSameGroup;
    int modeWrites = 0;
    int modeReads = 0;
    bool failModeWrite = false;

    ce::Source Make(const std::string &id, const std::wstring &label) {
        ce::Source source;
        source.id = id;
        source.label = label;
        HostSource *self = this;
        source.read = [self](std::vector<ce::Channel> &out, std::wstring &error) {
            ++self->readCalls;
            if (self->failRead) {
                error = L"the host refused to read";
                return false;
            }
            out = self->data;
            return true;
        };
        source.apply = [self](const std::vector<ce::Channel> &channels, std::wstring &error) {
            ++self->applyCalls;
            if (self->failApply) {
                error = L"the host refused this write";
                return false;
            }
            self->applied.push_back(channels);
            self->data = channels;
            return true;
        };
        if (hasReadMotion) {
            source.readMotion = [self](void *, std::wstring &error) -> bool {
                ++self->readMotionCalls;
                if (self->failRead) {
                    error = L"motion read failed";
                    return false;
                }
                return true;
            };
        }
        if (hasWriteMotion) {
            source.writeMotion = [self](void *, const std::vector<ce::Channel> &, std::wstring &error) -> bool {
                ++self->writeMotionCalls;
                if (self->failApply) {
                    error = L"motion write failed";
                    return false;
                }
                return true;
            };
        }
        source.comparisonEnabled = compareEnabled;
        source.initialMainChannel = initialMain;
        if (hasModeCallbacks) {
            source.readComparisonMode = [self]() -> int {
                ++self->modeReads;
                return self->storedMode;
            };
            source.writeComparisonMode = [self](int mode, std::wstring &error) -> bool {
                if (self->failModeWrite) {
                    error = L"the settings store refused the mode";
                    return false;
                }
                ++self->modeWrites;
                self->storedMode = mode;
                return true;
            };
        }
        return source;
    }
};

ac::Key MakeKey(std::int32_t frame, double value, ac::Interpolation interpolation = ac::Interpolation::Bezier,
                ac::Tangent tangent = ac::Tangent::Automatic) {
    ac::Key key;
    key.frame = frame;
    key.value = value;
    key.interpolation = interpolation;
    key.tangent = tangent;
    return key;
}

ce::Channel MakeChannel(const std::string &id, const std::wstring &label, std::vector<ac::Key> keys) {
    ce::Channel channel;
    channel.id = id;
    channel.label = label;
    channel.track.keys = std::move(keys);
    return channel;
}

// ---------------------------------------------------------------------------
// Window helpers.
// ---------------------------------------------------------------------------
void Click(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the command button exists");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(id), BN_CLICKED), reinterpret_cast<LPARAM>(control));
}

void SetText(HWND window, int id, const wchar_t *text) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the edit control exists");
    SetWindowTextW(control, text);
}

std::wstring GetText(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    if (!control) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &text[0], length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

int KeyCount(HWND window, int channel = -1) {
    return static_cast<int>(SendMessageW(window, kHookKeyCount, static_cast<WPARAM>(channel), 0));
}

// Reads the shared time range and the value range of the current view.
void ReadView(HWND window, double view[4]) {
    view[0] = view[1] = view[2] = view[3] = 0;
    SendMessageW(window, kHookView, 0, reinterpret_cast<LPARAM>(view));
}

LRESULT FocusId(HWND window) { return SendMessageW(window, kHookFocusId, 0, 0); }

POINT AskBarPoint(HWND window, int bar, int segment) {
    const LPARAM packed = static_cast<LPARAM>(SendMessageW(window, kHookBarPoint, static_cast<WPARAM>(bar),
                                                           static_cast<LPARAM>(segment)));
    POINT point = {static_cast<LONG>(static_cast<short>(LOWORD(packed))),
                   static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
    return point;
}

void ClickBar(HWND window, int bar, int segment) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    const POINT point = AskBarPoint(window, bar, segment);
    Check(point.x > 0 && point.y > 0, "the queried range-bar segment lies inside the canvas");
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
}

void PressKey(HWND window, int key) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    SendMessageW(canvas, WM_KEYDOWN, static_cast<WPARAM>(key), 0);
    SendMessageW(canvas, WM_KEYUP, static_cast<WPARAM>(key), 0);
}

POINT AskPoint(HWND window, UINT hook, WPARAM value) {
    const LPARAM packed = static_cast<LPARAM>(SendMessageW(window, hook, value, 0));
    POINT point = {static_cast<LONG>(static_cast<short>(LOWORD(packed))),
                   static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
    return point;
}

void ClickKey(HWND window, int keyIndex) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(canvas != nullptr, "the canvas exists");
    const POINT point = AskPoint(window, kHookPointForKey, static_cast<WPARAM>(keyIndex));
    Check(point.x > 0 && point.y > 0, "the queried key lies inside the canvas");
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(canvas, WM_LBUTTONUP, 0, position);
}

// Drags the key at `keyIndex` to `targetFrame`, keeping the value.
void DragKeyToFrame(HWND window, int keyIndex, int targetFrame) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(canvas != nullptr, "the canvas exists");
    const POINT from = AskPoint(window, kHookPointForKey, static_cast<WPARAM>(keyIndex));
    const POINT framePoint = AskPoint(window, kHookPointForFrame, static_cast<WPARAM>(targetFrame));
    Check(from.x > 0 && from.y > 0 && framePoint.x > 0, "the drag endpoints lie inside the canvas");
    POINT to = {framePoint.x, from.y};
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
}

const ac::Key *FindKey(const std::vector<ce::Channel> &channels, std::size_t channel, std::int32_t frame) {
    if (channel >= channels.size()) return nullptr;
    for (const ac::Key &key : channels[channel].track.keys) {
        if (key.frame == frame) return &key;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Channel tree and hint helpers.  The tree is driven through real Win32 mouse
// and key messages; the state comes back through the WM_APP hooks.
// ---------------------------------------------------------------------------
int ChannelItemIndex(HWND tree, HTREEITEM item) {
    TVITEMW value = {};
    value.mask = TVIF_PARAM;
    value.hItem = item;
    return item && SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&value)) && value.lParam > 0
               ? static_cast<int>(value.lParam - 1) : -1;
}

HTREEITEM FindChannelItem(HWND tree, HTREEITEM item, int index) {
    for (; item; item = TreeView_GetNextSibling(tree, item)) {
        if (ChannelItemIndex(tree, item) == index) return item;
        if (HTREEITEM child = FindChannelItem(tree, TreeView_GetChild(tree, item), index)) return child;
    }
    return nullptr;
}

HTREEITEM ChannelItem(HWND window, int index) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    Check(tree != nullptr, "the channel tree exists");
    HTREEITEM item = FindChannelItem(tree, TreeView_GetRoot(tree), index);
    Check(item != nullptr, "the channel has a tree leaf");
    return item;
}

int ChannelCaretIndex(HWND window) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    return ChannelItemIndex(tree, TreeView_GetSelection(tree));
}

int RenderInk(HWND child);

POINT ChannelRowPoint(HWND window, int index, bool checkbox) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    HTREEITEM item = ChannelItem(window, index);
    TreeView_EnsureVisible(tree, item);
    if (checkbox && !TreeView_GetImageList(tree, TVSIL_STATE)) {
        // Native checkbox images are created on the first paint. Hidden test
        // windows have not had one yet; initialize them into a memory DC.
        RenderInk(tree);
        Check(TreeView_GetImageList(tree, TVSIL_STATE) != nullptr,
              "a memory paint initializes the hidden tree's checkbox images");
    }
    RECT row = {};
    Check(TreeView_GetItemRect(tree, item, &row, TRUE) != FALSE, "the channel leaf has a text rectangle");
    const int y = (row.top + row.bottom) / 2;
    // A tree's checkbox and label have separate actions. Verify the hit zone
    // instead of guessing an indent or sending the former list-box messages.
    const UINT wanted = checkbox ? TVHT_ONITEMSTATEICON : TVHT_ONITEMLABEL;
    for (int x = 0; x < row.right; ++x) {
        TVHITTESTINFO hit = {};
        hit.pt = POINT{x, y};
        if (TreeView_HitTest(tree, &hit) == item && (hit.flags & wanted)) return hit.pt;
    }
    RECT client = {}; GetClientRect(tree, &client);
    std::fprintf(stderr, "CHANNEL miss index=%d checkbox=%d row=%ld,%ld,%ld,%ld client=%ld,%ld image=%p state=%u\n",
                 index, int(checkbox), row.left, row.top, row.right, row.bottom, client.right, client.bottom,
                 TreeView_GetImageList(tree, TVSIL_STATE), TreeView_GetItemState(tree, item, TVIS_STATEIMAGEMASK));
    Check(false, "the channel exposes the requested checkbox or label hit zone");
    return POINT{};
}

void ClickChannelOnce(HWND window, int index) {
    HWND list = GetDlgItem(window, kIdChannelList);
    const POINT point = ChannelRowPoint(window, index, true);
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(list, WM_LBUTTONUP, 0, position);
}

void ClickChannelLabel(HWND window, int index) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    const POINT point = ChannelRowPoint(window, index, false);
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(tree, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(tree, WM_LBUTTONUP, 0, position);
}

// The exact message sequence Windows delivers for a double click on the label:
// down, up, double-click, up. The checkbox is a separate visibility target.
void DoubleClickChannel(HWND window, int index) {
    HWND list = GetDlgItem(window, kIdChannelList);
    const POINT point = ChannelRowPoint(window, index, false);
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(list, WM_LBUTTONUP, 0, position);
    SendMessageW(list, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
    SendMessageW(list, WM_LBUTTONUP, 0, position);
}

int ChannelVisible(HWND window, int index) {
    return static_cast<int>(SendMessageW(window, kHookChannelVisible, static_cast<WPARAM>(index), 0));
}

int ActiveChannelIndex(HWND window) {
    int info[12] = {};
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(info));
    return info[1];
}

int HoverHint(HWND window) { return static_cast<int>(SendMessageW(window, kHookHoverHint, 0, 0)); }

void HoverControl(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the hovered control exists");
    SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
}

void LeaveControl(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the left control exists");
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
}

std::wstring ChannelRowText(HWND window, int index) {
    HWND tree = GetDlgItem(window, kIdChannelList);
    wchar_t text[256] = {};
    TVITEMW value = {};
    value.mask = TVIF_TEXT;
    value.hItem = ChannelItem(window, index);
    value.pszText = text;
    value.cchTextMax = static_cast<int>(sizeof(text) / sizeof(text[0]));
    Check(SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&value)) != FALSE,
          "the channel leaf text is readable");
    return text;
}
ce::Session MakeSession(HostSource &host, unsigned language = 2) {
    ce::Session session;
    session.id = "test";
    session.sources.push_back(host.Make("source", L"Test source"));
    session.language = language;
    session.currentFrame = [] { return 30.0; };
    session.seek = [](int) {};
    return session;
}

// ---------------------------------------------------------------------------
// 1. Hidden lifecycle.
// ---------------------------------------------------------------------------
void HiddenLifecycle() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    void *window = ce::Open(nullptr, session, false);
    Check(window != nullptr, "Open returns a window for a valid session");
    HWND handle = static_cast<HWND>(window);
    Check(IsWindow(handle) != FALSE, "the editor window exists");
    Check(IsWindowVisible(handle) == FALSE, "a hidden Open shows nothing");
    Check(IsWindowVisible(GetDlgItem(handle, kIdCanvas)) == FALSE, "the canvas of a hidden editor is invisible");
    Check(host.readCalls == 1, "Open reads the source once");
    Check(KeyCount(handle) == 2, "the read data reaches the editor");

    void *again = ce::Open(nullptr, session, false);
    Check(again == window, "one editor window is reused per owner");
    ce::CloseOwned(nullptr);
    Check(IsWindow(handle) == FALSE, "CloseOwned destroys the editor window");
    Note("hidden create/destroy with a single editor per owner");
}

// ---------------------------------------------------------------------------
// 2. Add, delete, undo and redo through the real callbacks.
// ---------------------------------------------------------------------------
void AddDeleteUndoRedo() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the editor opens hidden");
    Check(IsWindowEnabled(GetDlgItem(window, kIdAddKey)) != FALSE, "add stays enabled for a writable source");

    SetText(window, kIdFrameEdit, L"20");
    SetText(window, kIdValueEdit, L"3.5");
    const int before = host.applyCalls;
    Click(window, kIdAddKey);
    Check(host.applyCalls == before + 1, "Add reaches the apply callback");
    Check(KeyCount(window) == 3, "the track holds the new key");
    const ac::Key *added = FindKey(host.data, 0, 20);
    Check(added != nullptr, "the callback received a key at frame 20");
    Check(added && std::fabs(added->value - 3.5) < 1e-9, "the callback received the edited value");

    // A second key on the same frame is refused without touching the source.
    const int afterAdd = host.applyCalls;
    SetText(window, kIdValueEdit, L"9");
    Click(window, kIdAddKey);
    Check(host.applyCalls == afterAdd, "a duplicate frame is refused before apply");
    Check(KeyCount(window) == 3, "a refused duplicate leaves the track unchanged");
    Check(GetText(window, kIdStatus).find(L"20") != std::wstring::npos, "the status names the duplicate frame");

    ClickKey(window, 2);
    Check(GetText(window, kIdFrameEdit) == L"20", "selecting a key loads its frame into the editor");
    Click(window, kIdDeleteKey);
    Check(host.applyCalls == afterAdd + 1, "Delete reaches the apply callback");
    Check(KeyCount(window) == 2, "Delete removes the selected key");
    Check(FindKey(host.data, 0, 20) == nullptr, "the callback received the deletion");

    Click(window, kIdUndo);
    Check(KeyCount(window) == 3, "Undo restores the key");
    Check(FindKey(host.data, 0, 20) != nullptr, "Undo commits the restored track through apply");
    Click(window, kIdRedo);
    Check(KeyCount(window) == 2, "Redo removes it again");
    Check(FindKey(host.data, 0, 20) == nullptr, "Redo commits through apply as well");

    ce::CloseOwned(nullptr);
    Note("add, delete, undo and redo through the real callbacks");
}

// ---------------------------------------------------------------------------
// 3. Dragging a key moves its frame; a collision is refused.
// ---------------------------------------------------------------------------
void DragKey() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    DragKeyToFrame(window, 1, 25);
    Check(FindKey(host.data, 0, 25) != nullptr, "the dragged key lands on the snapped frame");
    Check(FindKey(host.data, 0, 10) == nullptr, "the original frame is gone");
    Check(KeyCount(window) == 3, "dragging keeps the key count");

    // Dragging onto an occupied frame must be refused outright.
    const int applyCalls = host.applyCalls;
    DragKeyToFrame(window, 1, 20);
    Check(host.applyCalls == applyCalls, "a colliding drag never reaches apply");
    Check(FindKey(host.data, 0, 25) != nullptr, "a refused drag leaves the key where it was");
    Check(FindKey(host.data, 0, 20) != nullptr, "the unselected key stays untouched");

    ce::CloseOwned(nullptr);
    Note("whole-frame key dragging with conflict rejection");
}

// ---------------------------------------------------------------------------
// 4. Two sources stay independent.
// ---------------------------------------------------------------------------
void TwoSources() {
    HostSource accessory;
    accessory.data.push_back(MakeChannel("acc.x", L"Accessory X", {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    HostSource camera;
    camera.data.push_back(MakeChannel("cam.x", L"Camera X",
                                      {MakeKey(0, 0.0), MakeKey(5, 1.0), MakeKey(10, 2.0), MakeKey(15, 3.0)}));
    camera.data.push_back(MakeChannel("cam.y", L"Camera Y", {MakeKey(0, 4.0)}));

    ce::Session session;
    session.id = "two";
    session.language = 1;
    session.sources.push_back(accessory.Make("accessory", L"Accessory"));
    session.sources.push_back(camera.Make("camera", L"Camera"));
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the two-source editor opens");
    Check(KeyCount(window) == 2, "the first source is loaded first");
    Check(camera.readCalls == 0, "the second source is not read before it is selected");

    HWND combo = GetDlgItem(window, kIdSourceCombo);
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(kIdSourceCombo, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    Check(camera.readCalls == 1, "switching the combo reads the new source");
    Check(KeyCount(window) == 4, "the second source brings its own channels");

    SetText(window, kIdFrameEdit, L"7");
    SetText(window, kIdValueEdit, L"1.25");
    const int accessoryApplies = accessory.applyCalls;
    Click(window, kIdAddKey);
    Check(camera.applyCalls == 1, "the edit is written to the selected source");
    Check(accessory.applyCalls == accessoryApplies, "the other source is not touched");
    Check(FindKey(camera.data, 0, 7) != nullptr, "the camera source received the key");

    const std::wstring title = GetText(window, 0);
    (void)title;
    wchar_t caption[256] = {};
    GetWindowTextW(window, caption, 256);
    Check(std::wstring(caption).find(L"Camera") != std::wstring::npos, "the window title names the current source");

    ce::CloseOwned(nullptr);
    Note("two sources with independent reads, writes and titles");
}

// ---------------------------------------------------------------------------
// 5. A refused apply rolls back and an invalid source stays disabled.
// ---------------------------------------------------------------------------
void RefusedApply() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    host.failApply = true;
    SetText(window, kIdFrameEdit, L"40");
    SetText(window, kIdValueEdit, L"7");
    Click(window, kIdAddKey);
    Check(host.applyCalls == 1, "the refused write still called apply");
    Check(KeyCount(window) == 2, "a refused write leaves the editing state unchanged");
    Check(host.data.size() == 1 && host.data[0].track.keys.size() == 2, "the host data is unchanged too");
    Check(GetText(window, kIdStatus).find(L"refused") != std::wstring::npos, "the status shows the callback error");

    // The rolled back state is still the original track: undo has nothing to
    // remove and the next accepted write starts from the original keys.
    Click(window, kIdUndo);
    Check(KeyCount(window) == 2, "nothing was pushed onto the undo stack by the refused write");

    host.failApply = false;
    ClickKey(window, 1);
    Click(window, kIdDeleteKey);
    Check(host.applied.empty() == false, "an accepted write follows");
    Check(host.applied.back()[0].track.keys.size() == 1, "the accepted write starts from the unmodified track");
    Check(host.applied.back()[0].track.keys.front().frame == 0, "the surviving key is the original first key");

    // A source without callbacks must not be editable at all.
    HostSource mute;
    mute.data.push_back(MakeChannel("mute", L"Muted", {MakeKey(0, 0.0)}));
    ce::Channel channel = mute.data.front();
    (void)channel;
    ce::Session muteSession;
    muteSession.id = "mute";
    muteSession.sources.push_back(ce::Source());
    muteSession.sources[0].id = "mute";
    muteSession.sources[0].label = L"Muted";
    HWND muteWindow = static_cast<HWND>(ce::Open(nullptr, muteSession, false));
    Check(muteWindow != nullptr, "a source without callbacks still opens");
    Check(IsWindowEnabled(GetDlgItem(muteWindow, kIdAddKey)) == FALSE, "editing is disabled for a source without callbacks");
    SetText(muteWindow, kIdFrameEdit, L"5");
    Click(muteWindow, kIdAddKey);
    Check(GetText(muteWindow, kIdStatus).empty() == false, "the disabled source reports a reason");
    ce::CloseOwned(nullptr);
    Note("refused apply rolls back, invalid source stays disabled");
}

// ---------------------------------------------------------------------------
// 6. MMD handle panel: diagonal for Linear, disabled with a reason otherwise.
// ---------------------------------------------------------------------------
void MmdPanel() {
    HostSource host;
    ce::Channel linear = MakeChannel(
        "pos.x", L"Position X",
        {MakeKey(0, 0.0, ac::Interpolation::Bezier, ac::Tangent::Free),
         MakeKey(10, 5.0, ac::Interpolation::Linear, ac::Tangent::Linear)});
    // The core stores a straight interval as Linear while the slopes may still
    // be zero, so the panel has to derive the diagonal handles from the
    // weights instead of drawing an S curve.
    linear.track.keys[0].outSlope = 0.0;
    linear.track.keys[1].inSlope = 0.0;
    host.data.push_back(linear);
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    ClickKey(window, 1);
    double controls[4] = {-1, -1, -1, -1};
    const BOOL enabled = static_cast<BOOL>(SendMessageW(window, kHookMmdControls, 0, reinterpret_cast<LPARAM>(controls)));
    Check(enabled != FALSE, "a Linear interval is representable");
    Check(controls[0] == controls[1] && controls[2] == controls[3],
          "a Linear interval shows the diagonal handles, not the stale slopes");
    Check(controls[0] < controls[2], "the two handles stay separated in time");
    Check(controls[0] > 0 && controls[2] < 127, "the diagonal handles stay inside 0..127");
    Check(IsWindowEnabled(GetDlgItem(window, kIdMmdPanel)) != FALSE, "the MMD panel is enabled");

    // Equal-valued endpoints cannot be normalised by the MMD handle grid, so
    // the panel must disable itself and say why instead of clamping the curve.
    HostSource flat;
    flat.data.push_back(MakeChannel("pos.y", L"Position Y", {MakeKey(0, 2.0), MakeKey(10, 2.0)}));
    ce::Session flatSession = MakeSession(flat);
    ce::CloseOwned(nullptr);
    HWND flatWindow = static_cast<HWND>(ce::Open(nullptr, flatSession, false));
    ClickKey(flatWindow, 1);
    double flatControls[4] = {};
    const BOOL flatEnabled =
        static_cast<BOOL>(SendMessageW(flatWindow, kHookMmdControls, 0, reinterpret_cast<LPARAM>(flatControls)));
    Check(flatEnabled == FALSE, "an equal-valued interval is not representable");
    Check(IsWindowEnabled(GetDlgItem(flatWindow, kIdMmdPanel)) == FALSE, "the MMD panel is disabled instead of clamping");
    Check(GetText(flatWindow, kIdMmdReadout).empty() == false, "the disabled panel explains the reason");
    ce::CloseOwned(nullptr);
    Note("MMD panel: Linear diagonal handles, disabled with a reason when unrepresentable");
}

// ---------------------------------------------------------------------------
// 7. Launcher icon geometry for the 22 and 18 pixel buttons.
// ---------------------------------------------------------------------------
int CountInk(const unsigned *pixels, int width, int left, int top, int right, int bottom, int channel) {
    int count = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const unsigned value = pixels[static_cast<std::size_t>(y) * width + x];
            const int red = static_cast<int>(value & 0xFF);
            const int green = static_cast<int>((value >> 8) & 0xFF);
            const int blue = static_cast<int>((value >> 16) & 0xFF);
            if (red < channel && green < channel && blue < channel) ++count;
        }
    }
    return count;
}

bool AnyDark(const unsigned *pixels, int width, int x, int y, int channel) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const unsigned value = pixels[static_cast<std::size_t>(y + dy) * width + (x + dx)];
            const int red = static_cast<int>(value & 0xFF);
            const int green = static_cast<int>((value >> 8) & 0xFF);
            const int blue = static_cast<int>((value >> 16) & 0xFF);
            if (red < channel && green < channel && blue < channel) return true;
        }
    }
    return false;
}

// Mirrors the documented mapping of DrawLauncher: the button rectangle is
// physical, the icon box is the button side minus two logical pixels per side,
// and the regular asset (viewport 20) is used from a 22 pixel button upwards.
struct IconGeometry {
    int left = 0;
    int top = 0;
    int side = 0;
    double scale = 1.0;
    bool regular = true;
};

IconGeometry LauncherIconRect(HDC dc, int x, int y, int width, int height, bool pressed) {
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    if (dpi < 48) dpi = 96;
    const int buttonSide = (width < height) ? width : height;
    int logical = static_cast<int>(std::lround(buttonSide * 96.0 / dpi));
    if (logical < 1) logical = 1;
    const bool regular = logical >= 21;
    int inset = 0;
    if (logical == 18 || logical == 20 || logical == 22 || logical == 24)
        inset = (std::max)(1, MulDiv(2, dpi, 96));
    else
        inset = (std::max)(1, (buttonSide + 5) / 11);
    const int offset = pressed ? (std::max)(1, MulDiv(1, dpi, 96)) : 0;
    IconGeometry geometry;
    geometry.side = buttonSide - 2 * inset;
    geometry.left = x + (width - geometry.side) / 2 + offset;
    geometry.top = y + (height - geometry.side) / 2 + offset;
    geometry.regular = regular;
    geometry.scale = geometry.side / (regular ? 20.0 : 14.0);
    return geometry;
}

int MapX(const IconGeometry &geometry, double u) {
    return geometry.left + static_cast<int>(std::lround(u * geometry.scale));
}

int MapY(const IconGeometry &geometry, double v) {
    return geometry.top + static_cast<int>(std::lround(v * geometry.scale));
}

void LauncherIcon() {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    const int width = 160;
    const int height = 96;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Check(bitmap != nullptr && bits != nullptr, "a 32-bit memory bitmap is available");
    HGDIOBJ previous = SelectObject(dc, bitmap);
    const auto *pixels = static_cast<const unsigned *>(bits);
    RECT all = {0, 0, width, height};

    // 22 pixel button: regular asset, viewport 20, icon box 18.
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 4, 4, 22, 22, false, false, false);
    IconGeometry regular = LauncherIconRect(dc, 4, 4, 22, 22, false);
    Check(regular.regular, "a 22 pixel button uses the regular asset");
    const int regularArea = regular.side * regular.side;
    const int regularInk =
        CountInk(pixels, width, regular.left, regular.top, regular.left + regular.side, regular.top + regular.side, 200);
    Check(regularInk > regularArea / 20, "the 22 pixel launcher draws its box and curve");
    Check(regularInk < regularArea / 2, "the 22 pixel launcher leaves the icon box hollow");
    // The SVG box sits at 1.5 and 18.5 of a 20 unit viewport.
    Check(AnyDark(pixels, width, MapX(regular, 10.0), MapY(regular, 1.5), 200), "the regular asset draws its top edge");
    Check(AnyDark(pixels, width, MapX(regular, 1.5), MapY(regular, 10.0), 200), "the regular asset draws its left edge");
    // The cubic passes exactly through (10, 10) of the viewport.
    Check(AnyDark(pixels, width, MapX(regular, 10.0), MapY(regular, 10.0), 200),
          "the regular asset curve passes through the icon centre");

    // 18 pixel button: compact asset, viewport 14, icon box 14.
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 4, 40, 18, 18, false, false, false);
    IconGeometry compact = LauncherIconRect(dc, 4, 40, 18, 18, false);
    Check(!compact.regular, "an 18 pixel button uses the compact asset");
    const int compactArea = compact.side * compact.side;
    const int compactInk =
        CountInk(pixels, width, compact.left, compact.top, compact.left + compact.side, compact.top + compact.side, 200);
    Check(compactInk > compactArea / 20, "the 18 pixel launcher draws its box and curve");
    Check(compactInk < compactArea / 2, "the 18 pixel launcher leaves the icon box hollow");
    // The compact cubic passes through (7, 7) of its 14 unit viewport.
    Check(AnyDark(pixels, width, MapX(compact, 7.0), MapY(compact, 7.0), 200),
          "the compact asset curve passes through its centre");
    Check(AnyDark(pixels, width, MapX(compact, 7.0), MapY(compact, 1.5), 200), "the compact asset draws its top edge");

    // Pressed, focused and disabled states must draw without changing geometry.
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 4, 4, 24, 24, true, false, true);
    IconGeometry pressed = LauncherIconRect(dc, 4, 4, 24, 24, true);
    Check(CountInk(pixels, width, pressed.left, pressed.top, pressed.left + pressed.side, pressed.top + pressed.side, 200) >
              pressed.side * pressed.side / 20,
          "the 24 pixel pressed and focused launcher draws");
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    ce::DrawLauncher(dc, 60, 4, 22, 22, false, true, false);
    IconGeometry disabled = LauncherIconRect(dc, 60, 4, 22, 22, false);
    Check(CountInk(pixels, width, disabled.left, disabled.top, disabled.left + disabled.side,
                   disabled.top + disabled.side, 220) > disabled.side * disabled.side / 20,
          "the disabled launcher still draws its icon");

    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    Note("launcher icon geometry for the 22 and 18 pixel buttons");
}

// ---------------------------------------------------------------------------
// 8. Layout in the three languages.
// ---------------------------------------------------------------------------
struct ChildList {
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectChild(HWND child, LPARAM param) {
    reinterpret_cast<ChildList *>(param)->windows.push_back(child);
    return TRUE;
}

struct Rect {
    RECT rect = {};
    std::wstring className;
    bool visible = false;
};

void CheckLayout(HWND window, const wchar_t *language) {
    RECT client = {};
    GetClientRect(window, &client);
    ChildList children;
    EnumChildWindows(window, CollectChild, reinterpret_cast<LPARAM>(&children));
    Check(children.windows.size() > 20, "the editor builds its full control set");

    std::vector<Rect> rects;
    for (HWND child : children.windows) {
        Rect entry;
        // The style bit, not IsWindowVisible: the editor of a background test
        // is hidden, and IsWindowVisible would then report every child as
        // invisible and silently skip the whole geometry check.
        entry.visible = (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0;
        if (!entry.visible) continue;
        GetWindowRect(child, &entry.rect);
        POINT corners[2] = {{entry.rect.left, entry.rect.top}, {entry.rect.right, entry.rect.bottom}};
        MapWindowPoints(nullptr, window, corners, 2);
        entry.rect.left = corners[0].x;
        entry.rect.top = corners[0].y;
        entry.rect.right = corners[1].x;
        entry.rect.bottom = corners[1].y;
        wchar_t name[64] = {};
        GetClassNameW(child, name, 64);
        entry.className = name;
        rects.push_back(entry);
        Check(entry.rect.left >= 0 && entry.rect.top >= 0 && entry.rect.right <= client.right &&
                  entry.rect.bottom <= client.bottom,
              "every visible control stays inside the client area");
    }

    for (std::size_t first = 0; first < rects.size(); ++first) {
        for (std::size_t second = first + 1; second < rects.size(); ++second) {
            const RECT &a = rects[first].rect;
            const RECT &b = rects[second].rect;
            const bool overlap = a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
            if (overlap) {
                std::fprintf(stderr, "FAIL: overlapping controls in language %ls: %ls and %ls\n", language,
                             rects[first].className.c_str(), rects[second].className.c_str());
                std::exit(1);
            }
        }
    }

    const struct {
        int label;
        int control;
        bool vertical;
    } pairs[] = {
        {kIdSourceLabel, kIdSourceCombo, false},   {kIdViewLabel, kIdViewCombo, false},
        {kIdToolLabel, kIdToolCombo, false},       {kIdChannelLabel, kIdChannelList, true},

        {kIdFrameLabel, kIdFrameEdit, false},      {kIdValueLabel, kIdValueEdit, false},
        {kIdInterpolationLabel, kIdInterpolationCombo, false},
        {kIdTangentLabel, kIdTangentCombo, false}, {kIdInSlopeLabel, kIdInSlopeEdit, false},
        {kIdOutSlopeLabel, kIdOutSlopeEdit, false}, {kIdInWeightLabel, kIdInWeightEdit, false},
        {kIdOutWeightLabel, kIdOutWeightEdit, false},
        {kIdBakeFirstLabel, kIdBakeFirstEdit, false},
        {kIdBakeLastLabel, kIdBakeLastEdit, false}, {kIdBakeStepLabel, kIdBakeStepEdit, false},
    };
    for (const auto &pair : pairs) {
        HWND label = GetDlgItem(window, pair.label);
        HWND control = GetDlgItem(window, pair.control);
        Check(label != nullptr && control != nullptr, "both members of a label pair exist");
        if(!(GetWindowLongPtrW(label,GWL_STYLE)&WS_VISIBLE)||!(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE))continue;
        RECT labelRect = {};
        RECT controlRect = {};
        GetWindowRect(label, &labelRect);
        GetWindowRect(control, &controlRect);
        POINT labelCorners[2] = {{labelRect.left, labelRect.top}, {labelRect.right, labelRect.bottom}};
        POINT controlCorners[2] = {{controlRect.left, controlRect.top}, {controlRect.right, controlRect.bottom}};
        MapWindowPoints(nullptr, window, labelCorners, 2);
        MapWindowPoints(nullptr, window, controlCorners, 2);
        const int gap = pair.vertical ? controlCorners[0].y - labelCorners[1].y
                                      : controlCorners[0].x - labelCorners[1].x;
        if (gap < 4) {
            std::fprintf(stderr, "FAIL: label gap %d in language %ls (ids %d/%d)\n", gap, language, pair.label,
                         pair.control);
            std::exit(1);
        }
        ++g_checks;
    }

    for (HWND child : children.windows) {
        wchar_t name[64] = {};
        GetClassNameW(child, name, 64);
        if (std::wstring(name) == L"Edit") {
            const DWORD margins = static_cast<DWORD>(SendMessageW(child, EM_GETMARGINS, 0, 0));
            Check(LOWORD(margins) >= 4 && HIWORD(margins) >= 4, "an edit box keeps at least 4 logical pixels of padding");
            RECT rect = {};
            GetClientRect(child, &rect);
            Check(rect.bottom - rect.top >= 16, "an edit box is tall enough for its font");
        }
    }
    (void)language;
}

void LayoutThreeLanguages() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host, 0);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the editor opens for the layout check");

    const wchar_t *names[3] = {L"ja", L"en", L"zh"};
    const wchar_t *addKeyText[3] = {L"\u30AD\u30FC\u8FFD\u52A0", L"Add key", L"\u6DFB\u52A0\u5173\u952E\u5E27"};
    for (unsigned language = 0; language < 3; ++language) {
        ce::SetLanguage(nullptr, language);
        Check(GetText(window, kIdAddKey) == addKeyText[language], "SetLanguage retranslates the controls");
        Check(GetText(window, kIdFrameEdit).empty() == false, "SetLanguage preserves the editing state");
        Check(KeyCount(window) == 2, "SetLanguage preserves the loaded channels");
        Check(GetText(window, kIdStatus).empty() == false, "a status line is always present");
        Check(IsWindowEnabled(GetDlgItem(window, kIdAddKey)) != FALSE, "language switching keeps editing enabled");
        wchar_t caption[256] = {};
        GetWindowTextW(window, caption, 256);
        Check(std::wstring(caption).find(L"Test source") != std::wstring::npos, "the title keeps naming the source");
        CheckLayout(window, names[language]);
    }

    // The layout must survive the smallest window the editor allows.  The
    // minimum comes from the editor itself, so this check scales with the DPI.
    MINMAXINFO limits = {};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    Check(limits.ptMinTrackSize.x > 0 && limits.ptMinTrackSize.y > 0, "the editor publishes a minimum window size");
    SetWindowPos(window, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(limits.ptMinTrackSize.x, limits.ptMinTrackSize.y));
    CheckLayout(window, L"minimum");

    ce::CloseOwned(nullptr);
    Note("three-language layout with minimum size, gaps and padding");
}

void LiveInterfaceScale() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0), MakeKey(10, 5)}));
    const auto session = MakeSession(host, 1);
    const auto rowHeight = [](HWND window) {
        return TreeView_GetItemHeight(GetDlgItem(window, kIdChannelList));
    };
    ce::RefreshInterfaceScale(100);
    const HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "open a hidden native curve editor at 100 percent");
    const int baselineHeight = rowHeight(window);
    const auto frame = GetText(window, kIdFrameEdit);
    const HWND edit = GetDlgItem(window, kIdFrameEdit);
    const auto font = SendMessageW(edit, WM_GETFONT, 0, 0);
    RECT initial{};
    GetWindowRect(window, &initial);
    for (int percent : {125, 200, 100}) {
        ce::RefreshInterfaceScale(percent);
        RECT resized{};
        GetWindowRect(window, &resized);
        Check(EqualRect(&initial, &resized) && KeyCount(window) == 2 && GetText(window, kIdFrameEdit) == frame,
              "live scaling retains the native editor window, key data and frame text");
        const int height = rowHeight(window);
        Check(percent == 100 ? height == baselineHeight : height > baselineHeight,
              "the native tree row height updates immediately and restores at 100 percent");
        if (percent > 100)
            Check(SendMessageW(edit, WM_GETFONT, 0, 0) != font,
                  "the existing numeric edit receives the scaled font");
    }
    ce::CloseOwned(nullptr);
    ce::RefreshInterfaceScale(200);
    const HWND reopened = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(reopened && rowHeight(reopened) > baselineHeight && KeyCount(reopened) == 2,
          "a newly opened editor adopts the active application scale");
    ce::CloseOwned(nullptr);
    ce::RefreshInterfaceScale(100);
    Note("live interface scale and newly opened curve editor");
}

// ---------------------------------------------------------------------------
// 9. Motion callbacks gate their buttons.
// ---------------------------------------------------------------------------
void MotionCallbackLifetime() {
    Check(ce::CaptureWindowSession(nullptr).expired(), "unknown windows do not acquire an editor session");
    for(bool keepToken:{false,true})for(bool external:{false,true})for(bool writing:{false,true})for(int action=0;action<4;++action){
        std::printf("motion lifetime external=%d write=%d action=%d\n",int(external),int(writing),action);
        HostSource host,replacement;
        host.data.push_back(MakeChannel("pos.x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        replacement.data.push_back(MakeChannel("pos.x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        auto session=MakeSession(host);
        session.sources[0].externalHistory=external;
        if(external){
            session.history.query=[](ce::HistoryState& state,std::wstring&){state={1,true,true};return true;};
            session.history.traverse=[](bool,std::wstring&){return false;};
        }
        HWND replacementWindow=nullptr;
        std::wstring replacementStatus;
        int calls=0,readsAfterCallback=-1;
        auto replace=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action==1||action==2){
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(replacementWindow),"a nested callback creates or replaces the editor session");
                replacementStatus=GetText(replacementWindow,kIdStatus);
            }
            readsAfterCallback=host.readCalls;
            if(action==3)throw std::runtime_error("callback retired its window");
        };
        session.sources[0].readMotion=[&](void*,std::wstring&){++calls;replace();return true;};
        session.sources[0].writeMotion=[&](void*,const std::vector<ce::Channel>& channels,std::wstring&){
            ++calls;replace();
            Check(channels.size()==1&&channels[0].track.keys[0].value==1&&channels[0].track.keys[1].value==5,
                "motion export keeps its channel snapshot after its editor session retires");
            return true;
        };
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        const auto lifetime=ce::CaptureWindowSession(window);
        const auto retained=keepToken?lifetime.lock():std::shared_ptr<void>{};
        Check(!lifetime.expired(),"registered editor exposes its current weak session");
        Click(window,writing?kIdWriteMotion:kIdReadMotion);
        Check(calls==1&&!ce::IsWindowSessionCurrent(window,lifetime),"close or replacement retires exactly the original motion session");
        Check(keepToken?!lifetime.expired():lifetime.expired(),"holding a token never extends editor session validity");
        Check(host.readCalls==readsAfterCallback&&host.applyCalls==0,"retired motion continuation does not read or apply the original source");
        if(replacementWindow){
            Check(GetText(replacementWindow,kIdStatus)==replacementStatus&&replacement.applyCalls==0,
                "old callback cannot alter replacement status or data");
            // The old operation's guard must not disable the newly opened editor.
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1,"replacement session remains editable after the retired operation returns");
        }
        ce::CloseOwned(nullptr);
    }
    Note("motion callback destruction, replacement and exceptions preserve session lifetime");
}

void SourceCallbackLifetime() {
    // kind: local canonical read, shared-history canonical read, history query.
    // phase: initial post-native setup, posted source refresh, parent command, canvas shortcut,
    // a subclassed button shortcut, or the numeric MMD child editor. All
    // retire the real registered window.
    for(int kind=0;kind!=3;++kind)for(int phase=0;phase!=6;++phase)
    for(int action=0;action!=3;++action)for(int response=0;response!=3;++response){
        std::printf("source lifetime kind=%d phase=%d action=%d response=%d\n",kind,phase,action,response);
        HostSource host,replacement;
        host.data.push_back(MakeChannel("pos.x",L"Original",{
            MakeKey(0,1,ac::Interpolation::Linear,ac::Tangent::Linear),
            MakeKey(10,5,ac::Interpolation::Linear,ac::Tangent::Linear)}));
        replacement.data.push_back(MakeChannel("pos.x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        auto session=MakeSession(host);
        session.sources[0].externalHistory=kind!=0;
        bool armed=phase==0;
        int retiredCalls=0;
        HWND replacementWindow=nullptr;
        std::wstring replacementStatus;
        auto payload=std::make_shared<int>(17);
        const std::weak_ptr<int> weakPayload=payload;
        auto retire=[&]{
            armed=false;++retiredCalls;
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(replacementWindow),"a source callback installs a live replacement session");
                replacementStatus=GetText(replacementWindow,kIdStatus);
            }
        };
        if(kind!=2){
            session.sources[0].read=[&,payload](std::vector<ce::Channel>& out,std::wstring&) {
                ++host.readCalls;
                if(armed){
                    retire();
                    Check(!weakPayload.expired()&&*payload==17,"read callable captures survive replacement while executing");
                    if(response==2)throw std::runtime_error("retired canonical read");
                    if(response==1)return false;
                }
                out=host.data;return true;
            };
        }
        if(kind!=0){
            if(kind==2){
                session.history.query=[&,payload](ce::HistoryState& out,std::wstring&) {
                    if(armed){
                        retire();
                        Check(!weakPayload.expired()&&*payload==17,"query callable captures survive replacement while executing");
                        if(response==2)throw std::runtime_error("retired history query");
                        if(response==1)return false;
                    }
                    out={1,true,true};return true;
                };
            }else session.history.query=[](ce::HistoryState& out,std::wstring&){out={1,true,true};return true;};
            session.history.traverse=[](bool,std::wstring&){return false;};
        }
        payload.reset();
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        std::weak_ptr<void> oldSession;
        std::shared_ptr<void> retainedToken;
        if(phase==0)Check(window==nullptr,"an initial Open never reports its retired session as successful");
        else{
            Check(IsWindow(window),"source lifetime fixture opens before its callback is armed");
            oldSession=ce::CaptureWindowSession(window);retainedToken=oldSession.lock();
            Check(bool(retainedToken),"the old session token can be held independently of its editor");
            if(phase>=2)ClickKey(window,1);
            if(phase==5){
                Check(IsWindowEnabled(GetDlgItem(window,1096)),"numeric MMD source lifetime fixture is editable");
                SetText(window,1096,L"21");SetText(window,1097,L"42");
                SetText(window,1098,L"86");SetText(window,1099,L"101");
            }
            armed=true;
            if(phase==1){ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);}
            if(phase==2)Click(window,kIdDeleteKey);
            if(phase==3)SendMessageW(GetDlgItem(window,kIdCanvas),WM_KEYDOWN,VK_DELETE,0);
            if(phase==4)SendMessageW(GetDlgItem(window,kIdDeleteKey),WM_KEYDOWN,VK_DELETE,0);
            if(phase==5)SendMessageW(GetDlgItem(window,1096),WM_KEYDOWN,VK_RETURN,0);
            Check(!ce::IsWindowSessionCurrent(window,oldSession)&&!oldSession.expired(),
                "registry identity retires the old continuation even with a retained strong token");
        }
        Check(retiredCalls==1&&host.applyCalls==0,"retired read or query prevents the old edit exactly once");
        Check(weakPayload.expired(),"retired callable captures are released after the native/public call unwinds");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&GetText(replacementWindow,kIdStatus)==replacementStatus&&
                KeyCount(replacementWindow)==2&&replacement.applyCalls==0,
                "old read/query and guards cannot overwrite replacement controls or channels");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(replacementWindow)==1,
                "replacement remains editable after old source and operation guards unwind");
        }else Check(!ce::OwnsWindow(nullptr,window),"closed source sessions leave no registered editor");
        ce::CloseOwned(nullptr);
    }
    Note("source reads and history queries retire safely through create, refresh and native child entries");
}

// An invoked std::function copy can release a capture which retires the editor.
// Its destructor must run before InvokeProvider's final session check.
struct ProviderCopyFinalizer {
    HostSource* host;
    bool* armed;
    std::function<void()>* retire;
    mutable bool invoked=false;
    ProviderCopyFinalizer(HostSource& h,bool& a,std::function<void()>& r):host(&h),armed(&a),retire(&r){}
    ProviderCopyFinalizer(const ProviderCopyFinalizer&)=default;
    ~ProviderCopyFinalizer(){if(invoked&&*armed){*armed=false;(*retire)();}}
    bool operator()(std::vector<ce::Channel>& out,std::wstring&)const{
        invoked=true;++host->readCalls;out=host->data;return true;
    }
    bool operator()(ce::HistoryState& out,std::wstring&)const{
        invoked=true;out={1,true,true};return true;
    }
};

void SourceCallbackFinalRelease() {
    for(bool query:{false,true})for(int action=0;action!=3;++action){
        HostSource host,replacement;
        host.data.push_back(MakeChannel("pos.x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        replacement.data.push_back(MakeChannel("pos.x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        bool armed=false;
        int calls=0;
        HWND replacementWindow=nullptr;
        std::wstring replacementStatus;
        std::function<void()> retire=[&]{
            ++calls;
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                replacementStatus=GetText(replacementWindow,kIdStatus);
            }
        };
        auto session=MakeSession(host);
        if(query){
            session.sources[0].externalHistory=true;
            session.history.query=ProviderCopyFinalizer(host,armed,retire);
            session.history.traverse=[](bool,std::wstring&){return false;};
        }else session.sources[0].read=ProviderCopyFinalizer(host,armed,retire);
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"a final-release source opens before being armed");
        ClickKey(window,1);armed=true;Click(window,kIdDeleteKey);
        Check(calls==1&&host.applyCalls==0,"callable copy destruction retires the edit before any old apply");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&GetText(replacementWindow,kIdStatus)==replacementStatus,
                "final release cannot resume into replacement status");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1,"replacement survives a source copy's final release");
        }
        ce::CloseOwned(nullptr);
    }
    Note("provider copy final release runs before the continuation validity check");
}

HWND OwnedCurveWindow() {
    HWND found=nullptr;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM data)->BOOL{
        if(!ce::OwnsWindow(nullptr,window))return TRUE;
        *reinterpret_cast<HWND*>(data)=window;return FALSE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}

void OriginalSessionFinalRelease() {
    // This releases the original Session's last capture without invoking or
    // copying that provider. It is distinct from ProviderCopyFinalizer above.
    for(int kind=0;kind!=3;++kind)for(int action=0;action!=3;++action){
        HostSource original,intermediate,replacement;
        original.data.push_back(MakeChannel("pos.x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        intermediate.data.push_back(MakeChannel("pos.x",L"Intermediate",{MakeKey(0,31),MakeKey(10,35)}));
        replacement.data.push_back(MakeChannel("pos.x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        bool armed=false;
        int releases=0;
        HWND replacementWindow=nullptr;
        std::wstring replacementStatus;
        auto payload=std::shared_ptr<int>(new int(17),[&](int* value){
            delete value;
            if(!armed)return;
            armed=false;++releases;
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(replacementWindow),"original Session release can install a nested replacement");
                replacementStatus=GetText(replacementWindow,kIdStatus);
            }
        });
        auto session=MakeSession(original);
        if(kind==0)session.sources[0].read=[&,payload](std::vector<ce::Channel>& out,std::wstring&){
            ++original.readCalls;out=original.data;return true;
        };
        if(kind==1){
            session.sources[0].externalHistory=true;
            session.history.query=[payload](ce::HistoryState& out,std::wstring&){out={1,true,true};return true;};
            session.history.traverse=[](bool,std::wstring&){return false;};
        }
        if(kind==2)session.currentFrame=[payload]{return 30.0;};
        payload.reset();
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"original Session final-release fixture opens unarmed");
        auto token=ce::CaptureWindowSession(window);auto retainedToken=token.lock();
        armed=true;
        Check(ce::Open(nullptr,MakeSession(intermediate),false)==nullptr&&releases==1,
            "original Session destruction retires exactly the intermediate Open attempt");
        Check(!ce::IsWindowSessionCurrent(window,token)&&bool(retainedToken)&&!token.expired(),
            "token retention does not keep original Session provider storage alive");
        Check(intermediate.readCalls==0&&intermediate.applyCalls==0&&original.applyCalls==0,
            "original Session releases before the retired intermediate session reads or edits");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&GetText(replacementWindow,kIdStatus)==replacementStatus&&
                KeyCount(replacementWindow)==2,"outer Open cleanup preserves the exact nested session");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1,"nested replacement remains editable after original Session destruction");
        }else Check(OwnedCurveWindow()==nullptr,"original Session final release leaves no ghost window");
        ce::CloseOwned(nullptr);
    }
    Note("original Session final capture release never reenters live Session assignment");
}

struct ProviderCopyState {
    bool armed=false,copying=false,throwCopy=false;
    const void* original=nullptr;
    int retireCalls=0,destroyedDuringCopy=0,bodyCalls=0;
    std::function<void()> retire;
};
struct CopyRetirementProvider {
    ProviderCopyState* state;
    HostSource* host;
    int marker=23;
    CopyRetirementProvider(ProviderCopyState& s,HostSource& h):state(&s),host(&h){}
    CopyRetirementProvider(const CopyRetirementProvider& other):state(other.state),host(other.host),marker(other.marker){
        if(!state->armed)return;
        state->armed=false;state->copying=true;state->original=&other;
        ++state->retireCalls;state->retire();
        Check(state->destroyedDuringCopy==0&&other.marker==23,
            "provider's original function remains alive throughout a reentrant copy constructor");
        state->copying=false;
        if(state->throwCopy)throw std::runtime_error("retired provider copy");
    }
    ~CopyRetirementProvider(){if(state->copying&&state->original==this)++state->destroyedDuringCopy;}
    bool operator()(std::vector<ce::Channel>& out,std::wstring&)const{
        ++state->bodyCalls;++host->readCalls;out=host->data;return true;
    }
    bool operator()(ce::HistoryState& out,std::wstring&)const{
        ++state->bodyCalls;out={1,true,true};return true;
    }
    std::wstring operator()(unsigned)const{++state->bodyCalls;return L"Copy source";}
};

void ProviderCopyRetirement() {
    for(int kind=0;kind!=3;++kind)for(int action=0;action!=3;++action)
    for(bool throwing:{false,true})for(bool initial:{false,true}){
        HostSource host,replacement;
        host.data.push_back(MakeChannel("pos.x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        replacement.data.push_back(MakeChannel("pos.x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        ProviderCopyState state;
        state.throwCopy=throwing;
        HWND replacementWindow=nullptr;
        std::wstring replacementStatus;
        state.retire=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(replacementWindow),"provider copy constructor installs a live nested replacement");
                replacementStatus=GetText(replacementWindow,kIdStatus);
            }
        };
        auto session=MakeSession(host);
        if(kind==0)session.sources[0].read=CopyRetirementProvider(state,host);
        if(kind==1){
            session.sources[0].externalHistory=true;
            session.history.query=CopyRetirementProvider(state,host);
            session.history.traverse=[](bool,std::wstring&){return false;};
        }
        if(kind==2)session.sources[0].localizedLabel=CopyRetirementProvider(state,host);
        state.armed=initial;
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        int calls=0;
        std::weak_ptr<void> token;
        std::shared_ptr<void> retainedToken;
        if(initial)Check(window==nullptr,"copy-time retirement prevents initial Open from reporting success");
        else{
            Check(IsWindow(window),"provider-copy retirement fixture opens unarmed");
            if(kind!=2)ClickKey(window,1);
            calls=state.bodyCalls;
            token=ce::CaptureWindowSession(window);retainedToken=token.lock();
            state.armed=true;
            if(kind==2)ce::SetLanguage(nullptr,1);else Click(window,kIdDeleteKey);
        }
        if(!(state.retireCalls==1&&state.destroyedDuringCopy==0&&state.bodyCalls==calls&&host.applyCalls==0))
            std::fprintf(stderr,"provider copy context: kind=%d action=%d throwing=%d initial=%d retire=%d destroyed=%d body=%d priorBody=%d apply=%d\n",
                kind,action,int(throwing),int(initial),state.retireCalls,state.destroyedDuringCopy,state.bodyCalls,calls,host.applyCalls);
        Check(state.retireCalls==1&&state.destroyedDuringCopy==0&&state.bodyCalls==calls&&host.applyCalls==0,
            "retirement while copying prevents invocation of the retired provider body");
        if(!initial)Check(!ce::IsWindowSessionCurrent(window,token)&&bool(retainedToken),
            "provider copying retires registry identity independently of token ownership");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&GetText(replacementWindow,kIdStatus)==replacementStatus,
                "copy failure cannot write replacement status or let outer cleanup destroy it");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1,"replacement remains editable after provider-copy retirement");
        }else Check(OwnedCurveWindow()==nullptr,"provider-copy closure leaves no ghost editor");
        ce::CloseOwned(nullptr);
    }
    Note("read/query/label copy constructors retain original storage and abort retired invocation");
}

void SourceLanguageAndOpenFailures() {
    for(int action=0;action!=3;++action){
        HostSource host,replacement;
        host.data.push_back(MakeChannel("pos.x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        replacement.data.push_back(MakeChannel("pos.x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        bool armed=false;
        HWND replacementWindow=nullptr;
        auto session=MakeSession(host);
        session.sources[0].localizedLabel=[&](unsigned){
            if(armed){
                armed=false;
                if(action!=1)ce::CloseOwned(nullptr);
                if(action!=0)replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                throw std::runtime_error("retired language callback");
            }
            return std::wstring(L"Original source");
        };
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"language lifetime fixture opens unarmed");
        armed=true;ce::SetLanguage(nullptr,1);
        Check(host.applyCalls==0,"language callback retirement never edits source data");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&GetText(replacementWindow,kIdSourceLabel)==L"源",
                "old SetLanguage cannot localize the new session after retirement");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1,"new session remains editable after public SetLanguage unwinds");
        }else Check(!IsWindow(window),"language callback can close its editor safely");
        ce::CloseOwned(nullptr);
    }
    // EnterSource runs after CreateWindowEx returns. An ordinary callback
    // exception must fail Open and close only the window owned by that attempt.
    for(bool replacing:{false,true}){
        HostSource host;
        host.compareEnabled=true;
        host.data.push_back(MakeChannel("pos.x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        if(replacing)Check(IsWindow(static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false))),
            "existing Open failure fixture creates its prior session");
        auto session=MakeSession(host);
        HWND created=nullptr;
        session.sources[0].readComparisonMode=[&]()->int{
            created=OwnedCurveWindow();throw std::runtime_error("post-create setup failure");
        };
        Check(ce::Open(nullptr,std::move(session),false)==nullptr&&created&&!IsWindow(created),
            "ordinary post-create setup failure closes its exact Open attempt");
        Check(OwnedCurveWindow()==nullptr,"failed setup leaves no registered hidden editor");
        ce::CloseOwned(nullptr);
    }
    Note("language retirement and post-create setup exceptions keep public entry ownership exact");
}

void MotionButtons() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    host.hasReadMotion = true;
    host.hasWriteMotion = true;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(IsWindowEnabled(GetDlgItem(window, kIdReadMotion)) != FALSE, "the read motion button is enabled with its callback");
    Check(IsWindowEnabled(GetDlgItem(window, kIdWriteMotion)) != FALSE, "the write motion button is enabled with its callback");
    Click(window, kIdReadMotion);
    Check(host.readMotionCalls == 1, "the read motion callback runs");
    Check(host.readCalls >= 2, "a successful motion read re-reads the source");
    Click(window, kIdWriteMotion);
    Check(host.writeMotionCalls == 1, "the write motion callback runs");
    ce::CloseOwned(nullptr);

    HostSource plain;
    plain.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0)}));
    ce::Session plainSession = MakeSession(plain);
    HWND plainWindow = static_cast<HWND>(ce::Open(nullptr, plainSession, false));
    Check(IsWindowEnabled(GetDlgItem(plainWindow, kIdReadMotion)) == FALSE,
          "the read motion button is disabled without its callback");
    Check(IsWindowEnabled(GetDlgItem(plainWindow, kIdWriteMotion)) == FALSE,
          "the write motion button is disabled without its callback");
    ce::CloseOwned(nullptr);
    Note("motion buttons follow the presence of their callbacks");
}

// ---------------------------------------------------------------------------
// 7. Rubber-band selection moves every selected key together.
// ---------------------------------------------------------------------------
void RubberBandMultiMove() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);

    const POINT first = AskPoint(window, kHookPointForKey, 1);
    const POINT last = AskPoint(window, kHookPointForKey, 2);
    Check(first.x > 0 && last.x > 0, "both keys are visible before the rubber band");
    // Starting on empty canvas clears the old selection and drags a box around
    // both keys; the corners come from the two key centres plus a margin.
    const int left = (first.x < last.x ? first.x : last.x) - 20;
    const int right = (first.x > last.x ? first.x : last.x) + 20;
    const int top = (first.y < last.y ? first.y : last.y) - 20;
    const int bottom = (first.y > last.y ? first.y : last.y) + 20;
    const POINT from = {left, top};
    const POINT to = {right, bottom};
    // Starting on empty canvas clears the old selection and drags a box.
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    // The release can be the first delivered position outside the original box.
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
    const std::wstring rubberStatus = GetText(window, kIdStatus);
    if (rubberStatus.find(L"2") == std::wstring::npos) {
        // The text is localised, so print the code units instead of %ls.
        std::fprintf(stderr, "FAIL: rubber band status length %d:", static_cast<int>(rubberStatus.size()));
        for (const wchar_t unit : rubberStatus) std::fprintf(stderr, " %04X", static_cast<unsigned>(unit));
        std::fprintf(stderr, " from %d,%d to %d,%d; keys %d,%d and %d,%d\n", static_cast<int>(from.x),
                     static_cast<int>(from.y), static_cast<int>(to.x), static_cast<int>(to.y),
                     static_cast<int>(first.x), static_cast<int>(first.y), static_cast<int>(last.x),
                     static_cast<int>(last.y));
        std::exit(1);
    }
    ++g_checks;

    const int applies = host.applyCalls;
    DragKeyToFrame(window, 1, 15);
    Check(host.applyCalls == applies + 1, "a multi-key drag commits once");
    Check(FindKey(host.data, 0, 15) != nullptr, "the dragged key moved to the target frame");
    Check(FindKey(host.data, 0, 25) != nullptr, "the other selected key moved by the same delta");
    Check(FindKey(host.data, 0, 10) == nullptr && FindKey(host.data, 0, 20) == nullptr,
          "no key was left behind at the old frames");

    ce::CloseOwned(nullptr);
    Note("rubber-band selection and multi-key dragging");
}

// ---------------------------------------------------------------------------
// 8. Channel selection and whole-range baking.
// ---------------------------------------------------------------------------
void BakeSelectedChannels() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    host.data.push_back(MakeChannel("pos.z", L"Position Z",
                                    {MakeKey(0, 1.0), MakeKey(10, 2.0), MakeKey(20, 3.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    Click(window, kIdChannelAll);
    SetText(window, kIdBakeFirstEdit, L"0");
    SetText(window, kIdBakeLastEdit, L"10");
    SetText(window, kIdBakeStepEdit, L"5");
    const int applies = host.applyCalls;
    Click(window, kIdBakeRun);
    Check(host.applyCalls == applies, "a range that does not cover every selected track is refused");
    Check(GetText(window, kIdStatus).find(L"partial-range") != std::wstring::npos,
          "the core error for a partial range reaches the status line");

    SetText(window, kIdBakeLastEdit, L"20");
    SetText(window, kIdBakeStepEdit, L"10");
    Click(window, kIdBakeRun);
    Check(host.applyCalls == applies + 1, "the whole range is baked through one apply");
    Check(host.data[0].track.keys.size() == 3, "the short channel is resampled over the whole range");
    Check(host.data[1].track.keys.size() == 3, "the long channel is resampled too");
    Check(host.data[0].track.keys[1].frame == 10 && host.data[0].track.keys[2].frame == 20,
          "the baked samples land on the requested step");
    Check(host.data[0].track.keys[0].interpolation == ac::Interpolation::Linear,
          "a baked sample interval becomes straight");

    Click(window, kIdUndo);
    Check(host.data[0].track.keys.size() == 2, "Undo restores the pre-bake track through apply");
    ce::CloseOwned(nullptr);
    Note("channel selection and whole-range baking with the core range check");
}

// ---------------------------------------------------------------------------
// 9. The painting paths render into a memory DC (no desktop pixel is read).
// ---------------------------------------------------------------------------
int RenderInk(HWND child) {
    RECT client = {};
    GetClientRect(child, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return -1;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return -1;
    }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all = {0, 0, width, height};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    const auto *pixels = static_cast<const unsigned *>(bits);
    const unsigned background = pixels[0];
    int ink = 0;
    const int total = width * height;
    for (int index = 0; index < total; ++index) {
        if (pixels[index] != background) ++ink;
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ink;
}

void CanvasPainting() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    host.data.push_back(MakeChannel("flag", L"Visible", {MakeKey(0, 1.0), MakeKey(10, 0.0)}));
    host.data[1].track.discrete = true;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    HWND mmd = GetDlgItem(window, kIdMmdPanel);
    Check(canvas != nullptr && mmd != nullptr, "the painting surfaces exist");

    Check(RenderInk(canvas) > 100, "the value view paints the curve, keys and range bars");
    ClickKey(window, 1);
    Check(RenderInk(canvas) > 100, "the selected key paints its tangent handles");
    Check(RenderInk(mmd) > 100, "the MMD panel paints the square and its two handles");

    HWND view = GetDlgItem(window, kIdViewCombo);
    SendMessageW(view, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(kIdViewCombo, CBN_SELCHANGE), reinterpret_cast<LPARAM>(view));
    Check(RenderInk(canvas) > 100, "the speed view paints the analytic derivative");

    HWND list = GetDlgItem(window, kIdChannelList);
    DoubleClickChannel(window, 1);
    Check(ChannelCaretIndex(window) == 1,
          "a list double click moves the caret to that row");
    Check(KeyCount(window) == 2, "the double click made the second channel the active one");
    Check(RenderInk(canvas) > 20, "a discrete channel in the speed view paints its notice instead of a curve");
    Check(RenderInk(mmd) > 20, "an interval with no handles still paints the disabled square");

    // A channel without keys must fall back to the empty notice.
    HostSource empty;
    empty.data.push_back(MakeChannel("empty", L"Empty", {}));
    ce::Session emptySession = MakeSession(empty);
    ce::CloseOwned(nullptr);
    HWND emptyWindow = static_cast<HWND>(ce::Open(nullptr, emptySession, false));
    HWND emptyCanvas = GetDlgItem(emptyWindow, kIdCanvas);
    Check(RenderInk(emptyCanvas) > 20, "an empty channel paints its notice");
    ce::CloseOwned(nullptr);
    Note("canvas and MMD panel painting into a memory DC");
}

// ---------------------------------------------------------------------------
// 11. Adding over an existing frame selects that key instead of writing.
// ---------------------------------------------------------------------------
void DuplicateFrameSelectsExisting() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    SetText(window, kIdFrameEdit, L"10");
    SetText(window, kIdValueEdit, L"99");
    const int applies = host.applyCalls;
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "adding over an existing frame writes nothing");
    Check(KeyCount(window) == 2, "the duplicate frame does not add a second key");
    Check(GetText(window, kIdValueEdit) == L"5", "the existing key is selected and its value is loaded");
    Check(GetText(window, kIdStatus).find(L"10") != std::wstring::npos,
          "the status says which frame is now selected");

    // The selection really is the existing key, so Delete removes that one.
    Click(window, kIdDeleteKey);
    Check(host.applyCalls == applies + 1, "the newly selected key can be deleted");
    Check(KeyCount(window) == 1, "deleting removed the key at frame 10");
    Check(FindKey(host.data, 0, 10) == nullptr, "the key at frame 10 is gone");
    ce::CloseOwned(nullptr);
    Note("an add over an existing frame selects it instead of writing");
}

// ---------------------------------------------------------------------------
// 12. Deleting re-runs the automatic tangents and frames stay inside int32.
// ---------------------------------------------------------------------------
void DeleteRecomputesAutomatic() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    ClickKey(window, 1);
    Click(window, kIdDeleteKey);
    Check(KeyCount(window) == 2, "the middle key is gone");
    const ac::Key *first = FindKey(host.data, 0, 0);
    const ac::Key *last = FindKey(host.data, 0, 20);
    Check(first != nullptr && last != nullptr, "the outer keys survive");
    const double secant = (9.0 - 0.0) / 20.0;
    Check(first && std::fabs(first->outSlope - secant) < 1e-12,
          "the surviving first key gets the automatic slope of its new interval");
    Check(last && std::fabs(last->inSlope - secant) < 1e-12,
          "the surviving last key gets the automatic slope of its new interval");
    ce::CloseOwned(nullptr);
    Note("deleting a key recomputes the automatic tangents of the remaining track");
}

void FrameRangeRejected() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    SetText(window, kIdValueEdit, L"1");
    const int applies = host.applyCalls;
    SetText(window, kIdFrameEdit, L"2147483648");  // one past std::int32_t
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "a frame above the int32 range is refused");
    Check(KeyCount(window) == 1, "the refused frame leaves the track alone");
    SetText(window, kIdFrameEdit, L"3000000000");
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "a far larger frame is refused as well");
    SetText(window, kIdFrameEdit, L"4294967296");
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies, "a frame beyond 32 bits is refused as well");

    SetText(window, kIdFrameEdit, L"2147483647");  // exactly std::int32_t
    Click(window, kIdAddKey);
    Check(host.applyCalls == applies + 1, "the largest int32 frame is accepted");
    Check(FindKey(host.data, 0, 2147483647) != nullptr, "the boundary frame is stored");
    ce::CloseOwned(nullptr);
    Note("frame numbers are range checked before the int32 conversion");
}

// ---------------------------------------------------------------------------
// 13. Localized source names and a read that only renamed channels.
// ---------------------------------------------------------------------------
void LocalizedSourceLabel() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0)}));
    ce::Session session = MakeSession(host, 1);
    session.sources[0].label = L"Plain source";
    session.sources[0].localizedLabel = [](unsigned language) -> std::wstring {
        switch (language) {
        case 0: return L"\u30AB\u30E1\u30E9";
        case 1: return L"Camera";
        default: return L"\u76F8\u673A";
        }
    };
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND combo = GetDlgItem(window, kIdSourceCombo);
    wchar_t item[128] = {};
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(item));
    Check(std::wstring(item) == L"Camera", "the combo uses the localized source name");

    ce::SetLanguage(nullptr, 2);
    item[0] = L'\0';
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(item));
    Check(std::wstring(item) == L"\u76F8\u673A", "SetLanguage re-localizes the source name");
    wchar_t caption[256] = {};
    GetWindowTextW(window, caption, 256);
    Check(std::wstring(caption).find(L"\u76F8\u673A") != std::wstring::npos,
          "the window title follows the localized name");
    ce::SetLanguage(nullptr, 0);
    item[0] = L'\0';
    SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(item));
    Check(std::wstring(item) == L"\u30AB\u30E1\u30E9", "language 0 uses the Japanese name");
    ce::CloseOwned(nullptr);
    Note("Source::localizedLabel drives the combo and the title in every language");
}

void ReadKeepsSelectionAndHistory() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    SetText(window, kIdFrameEdit, L"5");
    SetText(window, kIdValueEdit, L"2.5");
    Click(window, kIdAddKey);
    Check(KeyCount(window) == 3, "the edit produced an undo entry and a selection");
    Check(GetText(window, kIdFrameEdit) == L"5", "the new key is selected");

    // The host only renames the channel and gives it a unit.
    host.data[0].label = L"Position X renamed";
    host.data[0].unit = L"m";
    SendMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    // Native synchronization is posted and coalesced, like the real UI loop.
    MSG change{};
    while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Check(KeyCount(window) == 3, "a rename-only re-read keeps every key");
    HWND list = GetDlgItem(window, kIdChannelList);
    wchar_t item[256] = {};
    HTREEITEM first=TreeView_GetRoot(list);
    while(first && TreeView_GetChild(list,first))first=TreeView_GetChild(list,first);
    TVITEMW treeItem{};treeItem.mask=TVIF_TEXT;treeItem.hItem=first;treeItem.pszText=item;treeItem.cchTextMax=256;
    SendMessageW(list,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&treeItem));
    Check(std::wstring(item).find(L"renamed") != std::wstring::npos, "the new channel label is adopted");
    Check(std::wstring(item).find(L"m") != std::wstring::npos, "the new channel unit is adopted");
    Check(GetText(window, kIdFrameEdit) == L"5", "the local selection survives the rename");
    const int applies = host.applyCalls;
    Click(window, kIdUndo);
    Check(host.applyCalls == applies + 1, "the local undo stack survives the rename");
    Check(KeyCount(window) == 2, "undo still removes the local edit");

    // A real value change on activation is still treated as an external edit.
    host.data[0].track.keys[0].value = 42.0;
    SendMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Check(GetText(window, kIdFrameEdit) == L"30", "a real external edit clears the local selection");
    const int appliesAfter = host.applyCalls;
    Click(window, kIdUndo);
    Check(host.applyCalls == appliesAfter, "a real external edit also clears the undo stack");
    ce::CloseOwned(nullptr);
    Note("a rename-only re-read keeps the selection and history, a value change does not");
}

// ---------------------------------------------------------------------------
// 14. The playhead timer does nothing while the window is hidden.
// ---------------------------------------------------------------------------
void PendingNativeChangesBeforeWrite(bool notify = true) {
    HostSource host;
    host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(30,1)}));
    HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    SetText(window,kIdFrameEdit,L"15");SetText(window,kIdValueEdit,L"0.5");Click(window,kIdAddKey);
    host.data[0].track.keys.push_back(MakeKey(60,2));
    if(notify)ce::NotifySourceChanged(nullptr,"source");
    const int applies=host.applyCalls;
    SetText(window,kIdFrameEdit,L"45");SetText(window,kIdValueEdit,L"1.5");Click(window,kIdAddKey);
    Check(FindKey(host.data,0,60)!=nullptr,"a queued native registration survives a curve-editor write");
    Check(host.applyCalls==applies,"a stale curve draft is refreshed before it can overwrite native keys");
    Click(window,kIdUndo);
    Check(host.applyCalls==applies,"stale undo cannot remove a newly registered native key");
    SetText(window,kIdFrameEdit,L"45");SetText(window,kIdValueEdit,L"1.5");Click(window,kIdAddKey);
    Check(FindKey(host.data,0,45)&&FindKey(host.data,0,60),"retrying after refresh keeps both native and editor registrations");
    ce::CloseOwned(nullptr);
    Note("pending native changes cannot be overwritten by an editor draft or undo");
}

void NativeSelectionBeforeDelete(bool activate = true) {
    HostSource host;
    host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2),MakeKey(30,3)}));
    auto session=MakeSession(host);
    std::vector<int> selected;
    session.sources[0].readKeySelection=[&](const auto&){return selected;};
    session.sources[0].writeKeySelection=[&](const auto&,const auto& frames){selected=frames;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    ClickKey(window,1);
    Check(selected==std::vector<int>({10}),"curve selection reaches the native timeline");
    selected={20};
    if(activate)SendMessageW(window,WM_ACTIVATE,WA_ACTIVE,0);
    MSG change{};
    while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Click(window,kIdDeleteKey);
    Check(FindKey(host.data,0,10)&&!FindKey(host.data,0,20),"delete immediately after native selection targets the native selection, without waiting for a timer");
    ce::CloseOwned(nullptr);
}

void NativeEditsBeforeHistory() {
    for(int change=0;change<4;++change){
        HostSource host;
        host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2)}));
        HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
        SetText(window,kIdFrameEdit,L"15");SetText(window,kIdValueEdit,L"1.5");Click(window,kIdAddKey);
        if(change==3)Click(window,kIdUndo);
        if(change==0)host.data[0].track.keys.back().value=7;
        if(change==1)host.data[0].track.keys.erase(host.data[0].track.keys.begin()+1);
        if(change==2)host.data[0].track.keys.back().frame=21;
        if(change==3)host.data[0].track.keys.push_back(MakeKey(40,4));
        const int applies=host.applyCalls;
        Click(window,change==3?kIdRedo:kIdUndo);
        Check(host.applyCalls==applies,"native value/delete/move/registration changes invalidate stale editor history");
        Check(KeyCount(window)==int(host.data[0].track.keys.size()),"history rejection refreshes the actual native keys");
        SetText(window,kIdFrameEdit,L"45");SetText(window,kIdValueEdit,L"4.5");Click(window,kIdAddKey);
        Check(host.applyCalls==applies+1&&FindKey(host.data,0,45),"editing resumes after adopting a native change");
        if(change==0)Check(FindKey(host.data,0,20)->value==7,"retry keeps the native value change");
        if(change==1)Check(!FindKey(host.data,0,10),"retry never resurrects a natively deleted key");
        if(change==2)Check(FindKey(host.data,0,21)&&!FindKey(host.data,0,20),"retry keeps the native key move");
        if(change==3)Check(FindKey(host.data,0,40)&&!FindKey(host.data,0,15),"redo never erases a newly registered native key");
        ce::CloseOwned(nullptr);
    }
}

void CanonicalWriteAndUndo() {
    HostSource host;host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(10,1)}));
    auto session=MakeSession(host);auto apply=session.sources[0].apply;
    session.sources[0].apply=[apply](const auto& channels,auto& error){
        auto quantized=channels;
        for(auto& channel:quantized)for(auto& key:channel.track.keys)key.value=std::round(key.value*100)/100;
        return apply(quantized,error);
    };
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    ClickKey(window,1);SetText(window,kIdValueEdit,L"0.333333");Click(window,kIdApplyKey);
    Check(GetText(window,kIdValueEdit)==L"0.33","fields display the value actually accepted by the native source");
    Click(window,kIdUndo);Check(FindKey(host.data,0,10)->value==1,"native rounding does not invalidate our own undo");
    Click(window,kIdRedo);Check(FindKey(host.data,0,10)->value==0.33,"redo restores the accepted native value");
    ce::CloseOwned(nullptr);
}

void NativeChangesDuringMmdDrag() {
    HostSource host;host.data.push_back(MakeChannel("x",L"Camera X",{MakeKey(0,0),MakeKey(30,1)}));
    std::string error;Check(ce::SetChannelControls(host.data[0],1,{20,20,107,107},error),"initialize native drag interval");
    HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));ClickKey(window,1);
    HWND panel=GetDlgItem(window,kIdMmdPanel);double controls[4]{};
    Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls))!=0,"native handles are editable");
    const auto getDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    UINT dpi=getDpi?getDpi(panel):96;RECT client{};GetClientRect(panel,&client);
    const int margin=MulDiv(6,dpi?dpi:96,96),side=std::min(int(client.right),int(client.bottom))-2*margin;
    const LPARAM point=MAKELPARAM(margin+int(std::lround(controls[0]*side/127)),margin+side-int(std::lround(controls[1]*side/127)));
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point);
    const int reads=host.readCalls,applies=host.applyCalls;
    host.data[0].track.keys.push_back(MakeKey(60,2));ce::NotifySourceChanged(nullptr,"source");
    MSG change{};while(PeekMessageW(&change,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&change);
    Check(host.readCalls==reads,"native changes wait while an MMD handle drag owns the interval");
    SendMessageW(panel,WM_LBUTTONUP,0,point);
    Check(host.applyCalls==applies&&FindKey(host.data,0,60),"releasing a stale native handle cannot overwrite a registered key");
    ce::CloseOwned(nullptr);
}

// Tests the actual displayed ruler, rather than an obsolete invisible hit
// strip at the foot of the graph. Host values stand in for the shared timeline.
void TimelineRulerNavigation() {
    HostSource host;
    host.data.push_back(MakeChannel("x",L"X",{MakeKey(0,0),MakeKey(100,10)}));
    auto session=MakeSession(host);int frame=0,seeks=0;bool playing=false;
    session.currentFrame=[&]{return double(frame);};
    session.isPlaying=[&]{return playing;};
    session.seek=[&](int next){frame=next;playing=false;++seeks;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    HWND canvas=GetDlgItem(window,kIdCanvas);
    CurveEditorAxisState axis{};
    Check(SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis))!=0,"read ruler geometry");
    const auto position=[&](int f){
        auto point=AskPoint(window,kHookPointForFrame,f);
        return MAKELPARAM(point.x,(axis.plot.top-1)/2);
    };
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,position(30));
    Check(frame==30&&seeks==1,"clicking the displayed upper frame ruler seeks immediately");
    SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,position(70));
    Check(frame==70&&seeks==2,"dragging the displayed ruler seeks continuously");
    SendMessageW(canvas,WM_LBUTTONUP,0,position(80));
    Check(frame==80,"the final ruler release sample is applied");
    const int before=seeks;
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,position(80));
    SendMessageW(canvas,WM_LBUTTONUP,0,position(80));
    Check(seeks==before,"the same frame does not evaluate repeatedly");
    playing=true;
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,position(80));
    SendMessageW(canvas,WM_LBUTTONUP,0,position(80));
    Check(!playing&&seeks==before+1,"seeking the playback frame still stops transport");
    Check(host.applyCalls==0&&KeyCount(window)==2,"ruler navigation never authors keys or history");
    ce::CloseOwned(nullptr);
}

struct KeyboardModifiers {
    BYTE original[256]{};
    KeyboardModifiers(){GetKeyboardState(original);}
    ~KeyboardModifiers(){SetKeyboardState(original);}
    void Set(bool control=false,bool shift=false,bool alt=false){
        BYTE state[256]{};state[VK_CONTROL]=control?0x80:0;
        state[VK_SHIFT]=shift?0x80:0;state[VK_MENU]=alt?0x80:0;SetKeyboardState(state);
    }
};
void AuthoringKeyboardShortcuts() {
    KeyboardModifiers modifiers;
    HostSource host;host.data.push_back(MakeChannel("x",L"X",{MakeKey(0,0),MakeKey(10,5)}));
    HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    const HWND canvas=GetDlgItem(window,kIdCanvas),button=GetDlgItem(window,kIdFitAll);
    modifiers.Set();SetText(window,kIdFrameEdit,L"20");SetText(window,kIdValueEdit,L"3.5");
    SendMessageW(canvas,WM_KEYDOWN,VK_INSERT,0);
    Check(KeyCount(window)==3&&host.applyCalls==1,"Insert adds a key through the existing command");
    modifiers.Set(true);SendMessageW(window,WM_KEYDOWN,'Z',0);
    Check(KeyCount(window)==2&&host.applyCalls==2,"Ctrl+Z on the editor undoes once");
    SendMessageW(button,WM_KEYDOWN,'Y',0);
    Check(KeyCount(window)==3&&host.applyCalls==3,"Ctrl+Y on a child button redoes once");
    SendMessageW(GetDlgItem(window,kIdChannelList),WM_KEYDOWN,'Z',0);
    Check(KeyCount(window)==2,"Ctrl+Z works from the channel tree");
    modifiers.Set(true,true);SendMessageW(GetDlgItem(window,kIdMmdPanel),WM_KEYDOWN,'Z',0);
    Check(KeyCount(window)==3,"Ctrl+Shift+Z works from the native curve panel");
    const int before=host.applyCalls;
    SendMessageW(canvas,WM_KEYDOWN,'Z',LPARAM(1)<<30);
    Check(host.applyCalls==before,"held-key repeats do not add history operations");
    modifiers.Set(true,false,true);SendMessageW(canvas,WM_KEYDOWN,'Z',0);
    Check(host.applyCalls==before,"Alt combinations do not invoke authoring history");
    modifiers.Set();
    const HWND edit=GetDlgItem(window,kIdValueEdit);
    SetWindowTextW(edit,L"1234");SendMessageW(edit,EM_SETSEL,1,2);
    SendMessageW(edit,WM_KEYDOWN,VK_DELETE,0);
    Check(GetText(window,kIdValueEdit)==L"134"&&host.applyCalls==before,"Delete edits numeric text without deleting a curve key");
    modifiers.Set(true);SendMessageW(edit,WM_KEYDOWN,'Z',0);
    Check(host.applyCalls==before,"text undo never reaches curve history");
    modifiers.Set();SendMessageW(edit,WM_KEYDOWN,VK_INSERT,0);
    SendMessageW(edit,WM_KEYDOWN,VK_PROCESSKEY,0);
    Check(host.applyCalls==before,"input-box insert and IME processing never author keys");
    modifiers.Set();ClickKey(window,2);SendMessageW(button,WM_KEYDOWN,VK_DELETE,0);
    Check(KeyCount(window)==2,"Delete from a child button removes the curve selection");
    EnableWindow(window,FALSE);modifiers.Set(true);SendMessageW(canvas,WM_KEYDOWN,'Z',0);
    Check(KeyCount(window)==2,"disabled editors refuse authoring shortcuts");EnableWindow(window,TRUE);
    ce::CloseOwned(nullptr);
    auto readonly=MakeSession(host);readonly.sources[0].apply={};
    window=static_cast<HWND>(ce::Open(nullptr,std::move(readonly),false));
    const int readsBefore=host.applyCalls;
    modifiers.Set();SendMessageW(window,WM_KEYDOWN,VK_INSERT,0);
    SendMessageW(GetDlgItem(window,kIdCanvas),WM_KEYDOWN,VK_DELETE,0);
    modifiers.Set(true);SendMessageW(window,WM_KEYDOWN,'Z',0);SendMessageW(window,WM_KEYDOWN,'Y',0);
    Check(host.applyCalls==readsBefore,"read-only sessions refuse every authoring shortcut");
    ce::CloseOwned(nullptr);
    Note("ruler navigation and local authoring shortcuts");
}

void PlayheadTimer() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    double frame = 4.0;
    session.currentFrame = [&frame] { return frame; };
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    long long state[3] = {};

    const auto readState = [&] { SendMessageW(window, kHookPlayheadState, 0, reinterpret_cast<LPARAM>(state)); };
    readState();
    Check(state[0] == 0, "a hidden editor runs no playhead timer");
    Check(IsWindowVisible(window) == FALSE, "the playhead test keeps the editor hidden");

    const int reads = host.readCalls;
    const int applies = host.applyCalls;
    frame = 5.0;
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[1] == 0 && state[2] == 0, "ticks of a hidden editor neither count nor repaint");
    Check(host.readCalls == reads && host.applyCalls == applies, "a tick never reads or writes the source");

    // The visible branch is exercised without ever showing the window: setting
    // the WS_VISIBLE style bit makes IsWindowVisible true without displaying
    // anything on the desktop, and the style is restored right afterwards.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_VISIBLE);
    Check(IsWindowVisible(window) != FALSE, "the style bit makes the window report itself visible");
    frame = 6.0;
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[1] == 1, "a visible tick is processed");
    Check(state[2] == 1, "a changed frame repaints the playhead");
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[2] == 1, "an unchanged frame does not repaint again");
    frame = 7.0;
    SendMessageW(window, WM_TIMER, kPlayheadTimerId, 0);
    readState();
    Check(state[2] == 2, "the next frame change repaints again");
    Check(host.applyCalls == applies, "no tick ever wrote channel data");
    SetWindowLongPtrW(window, GWL_STYLE, style);
    Check(IsWindowVisible(window) == FALSE, "the style bit is restored");
    ce::CloseOwned(nullptr);
    Note("the playhead timer refreshes only a visible, changed frame");
}

// ---------------------------------------------------------------------------
// 15. Range-bar segments take the arrow keys.
// ---------------------------------------------------------------------------
void RangeBarKeyboard() {
    HostSource host;
    // Leave room on both sides for directional assertions. Zero is tested
    // separately below; the editor intentionally never pans to negative time.
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(20, 0.0), MakeKey(40, 9.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    double view[4] = {};

    // Without a bar click the arrows must not move the view.
    ReadView(window, view);
    const double startBefore = view[0];
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(view[0] == startBefore, "arrow keys do nothing before a range bar is clicked");

    // Time bar centre pans, one frame per press.
    ClickBar(window, 1, 0);
    ReadView(window, view);
    const double panStart = view[0], panEnd = view[1];
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs(view[0] - (panStart + 1.0)) < 1e-9 && std::fabs(view[1] - (panEnd + 1.0)) < 1e-9,
          "the time bar centre pans by one frame");
    PressKey(window, VK_LEFT);
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(std::fabs(view[0] - (panStart - 1.0)) < 1e-9, "the left arrow pans the other way");

    // Time bar low end drags the left edge.
    ClickBar(window, 1, 1);
    ReadView(window, view);
    const double fixedEnd = view[1];
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs(view[0] - (panStart)) < 1e-9 && std::fabs(view[1] - fixedEnd) < 1e-9,
          "the low grip moves only the left edge");
    ClickBar(window, 1, 2);
    PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs(view[0] - panStart) < 1e-9 && std::fabs(view[1] - (fixedEnd + 1.0)) < 1e-9,
          "the high grip moves only the right edge outward");
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(std::fabs(view[0] - panStart) < 1e-9 && std::fabs(view[1] - fixedEnd) < 1e-9,
          "the high grip moves only the right edge inward");
    ClickBar(window, 1, 1);
    for (int step = 0; step < 200; ++step) PressKey(window, VK_RIGHT);
    ReadView(window, view);
    Check(std::fabs((view[1] - view[0]) - kMinimumTimeSpan) < 1e-9,
          "the time span stops shrinking at the one-frame minimum");

    // Value bar high end (the top edge) drags the upper value and clamps.
    ClickBar(window, 2, 2);
    ReadView(window, view);
    const double upperLowBefore = view[2];
    const double upperHighBefore = view[3];
    PressKey(window, VK_DOWN);
    ReadView(window, view);
    if (!(view[2] == upperLowBefore && view[3] < upperHighBefore)) {
        const POINT probe = AskBarPoint(window, 2, 2);
        std::fprintf(stderr, "FAIL: value bar high grip %.6f,%.6f -> %.6f,%.6f (probe %d,%d)\n", upperLowBefore,
                     upperHighBefore, view[2], view[3], static_cast<int>(probe.x), static_cast<int>(probe.y));
        std::exit(1);
    }
    ++g_checks;
    const double highAfter = view[3];
    PressKey(window, VK_UP);
    ReadView(window, view);
    Check(view[2] == upperLowBefore && view[3] > highAfter, "the up arrow raises the upper edge again");

    // The low end (the bottom edge) drags the lower value only.  This runs
    // before the span reaches its minimum, where the clamp would freeze it.
    ClickBar(window, 2, 1);
    ReadView(window, view);
    const double keptHigh = view[3];
    const double keptLow = view[2];
    PressKey(window, VK_UP);
    ReadView(window, view);
    Check(view[3] == keptHigh && view[2] > keptLow, "the value bar low grip moves only the lower edge");

    // Test the centre while it is distinct from the endpoint hit zones.
    // At the minimum span the overview thumb can be less than one pixel tall,
    // so its geometric midpoint belongs to an endpoint, not a pan segment.
    ClickBar(window, 2, 0);
    ReadView(window, view);
    const double lowBefore = view[2], highBefore = view[3];
    const double valueStep = (highBefore - lowBefore) * 0.02;
    PressKey(window, VK_DOWN);
    ReadView(window, view);
    Check(std::fabs(view[2] - (lowBefore - valueStep)) < 1e-9 &&
          std::fabs(view[3] - (highBefore - valueStep)) < 1e-9,
          "the value bar centre pans both edges down by the same step");
    PressKey(window, VK_UP);
    ReadView(window, view);
    Check(std::fabs(view[2] - lowBefore) < 1e-9 && std::fabs(view[3] - highBefore) < 1e-9,
          "the value bar centre pans both edges back without changing the span");

    // Each press removes 2% of the current span; the span stops at the view
    // minimum, one thousandth of the data scale (never below 0.001 units).
    ClickBar(window, 2, 2);
    for (int step = 0; step < 1500; ++step) PressKey(window, VK_DOWN);
    ReadView(window, view);
    const double stoppedSpan = view[3] - view[2];
    PressKey(window, VK_DOWN);
    ReadView(window, view);
    Check(stoppedSpan >= 1.0e-3 - 1e-12 && std::fabs((view[3] - view[2]) - stoppedSpan) < 1e-12,
          "the value span stops shrinking at the minimum");
    Check(host.applyCalls == 0, "range-bar keys never write channel data");
    Check(KeyCount(window) == 2, "range-bar keys leave the keyframes alone");

    Click(window, kIdFitAll);
    ClickBar(window, 1, 0);
    ReadView(window, view);
    const double boundarySpan = view[1] - view[0];
    for (int step = 0; step < 60; ++step) PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] == 0.0 && std::fabs(view[1] - boundarySpan) < 1e-9,
          "time panning clamps at zero while preserving the view span");
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] == 0.0 && std::fabs(view[1] - boundarySpan) < 1e-9,
          "another left pan at zero leaves both time edges unchanged");
    ClickBar(window, 1, 1);
    PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] == 0.0 && std::fabs(view[1] - boundarySpan) < 1e-9,
          "the low time grip also clamps at zero without moving the high edge");
    Check(host.applyCalls == 0 && KeyCount(window) == 2,
          "range boundaries leave the authored keys unchanged");
    ce::CloseOwned(nullptr);
    Note("range-bar segments pan and zoom with the arrow keys");
}

// ---------------------------------------------------------------------------
// 16. Tab navigation between the native controls.
// ---------------------------------------------------------------------------
void TabNavigation() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    host.hasReadMotion = true;
    host.hasWriteMotion = true;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));

    // SetFocus refuses a window whose top level is hidden, so the style bit is
    // set for the duration of the test; nothing is displayed on the desktop
    // and the style is restored before the window is closed.
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style | WS_VISIBLE);
    SetFocus(window);

    HWND source = GetDlgItem(window, kIdSourceCombo);
    SendMessageW(source, WM_KEYDOWN, VK_TAB, 0);
    if (FocusId(window) != kIdReadMotion) {
        std::fprintf(stderr, "FAIL: Tab from the source combo focused id %ld (getfocus=%p, combo=%p)\n",
                     static_cast<long>(FocusId(window)), static_cast<void *>(GetFocus()),
                     static_cast<void *>(source));
        std::exit(1);
    }
    ++g_checks;
    HWND read = GetDlgItem(window, kIdReadMotion);
    SendMessageW(read, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdWriteMotion, "Tab keeps walking the reading order");

    Check(IsWindowEnabled(GetDlgItem(window, kIdDeleteKey)) == FALSE, "delete is disabled without a selection");
    Check(IsWindowEnabled(GetDlgItem(window, kIdApplyKey)) == FALSE, "apply is disabled without a selection");
    HWND add = GetDlgItem(window, kIdAddKey);
    SetFocus(add);
    SendMessageW(add, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdUndo, "Tab skips the disabled delete and apply buttons");

    HWND canvas = GetDlgItem(window, kIdCanvas);
    SetFocus(canvas);
    SendMessageW(canvas, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdFrameEdit, "Tab leaves the canvas towards the key fields");

    HWND run = GetDlgItem(window, kIdBakeRun);
    SetFocus(run);
    SendMessageW(run, WM_KEYDOWN, VK_TAB, 0);
    Check(FocusId(window) == kIdSourceCombo, "Tab wraps around to the first control");
    SetWindowLongPtrW(window, GWL_STYLE, style);
    Check(IsWindowVisible(window) == FALSE, "the style bit is restored");
    ce::CloseOwned(nullptr);
    Note("Tab navigation walks the native controls in reading order");
}

// ---------------------------------------------------------------------------
// 19..23. Comparison display.
// ---------------------------------------------------------------------------
ce::Channel MakeGroupChannel(const std::string &id, const std::wstring &label, int group, std::vector<ac::Key> keys,
                             bool selected = true) {
    ce::Channel channel = MakeChannel(id, label, std::move(keys));
    channel.group = group;
    channel.selected = selected;
    return channel;
}

// A key with explicit slopes and weights, which is what the comparison
// boundary cases need: equal endpoints and a Bezier bump between them.
ac::Key MakeShapeKey(std::int32_t frame, double value, double inSlope = 0.0, double outSlope = 0.0,
                     double inWeight = 1.0 / 3.0, double outWeight = 1.0 / 3.0,
                     ac::Interpolation interpolation = ac::Interpolation::Bezier) {
    ac::Key key = MakeKey(frame, value, interpolation);
    key.inSlope = inSlope;
    key.outSlope = outSlope;
    key.inWeight = inWeight;
    key.outWeight = outWeight;
    return key;
}

// IsWindowVisible also follows the parent chain, so a child of the hidden test
// window always reports false.  The comparison row is checked by its own style
// bit instead; CheckLayout below turns the parent style bit on for the duration
// of its run so its pairwise overlap check is not vacuous either.
bool StyleVisible(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    if (!control) return false;
    return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
}

// A real button click: BM_CLICK toggles an auto check box and notifies the
// parent with BN_CLICKED, so the test drives the same path the user does.
void ClickButton(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "the button exists");
    SendMessageW(control, BM_CLICK, 0, 0);
}

// The comparison modes are three direct buttons that act as a radio group, so
// picking a mode is one click and never a drop-down.
void ClickCompareMode(HWND window, int mode) {
    int id = kIdCompareSameGroup;
    if (mode == ce::kCompareAllGroups) id = kIdCompareAllGroups;
    else if (mode == ce::kCompareFree) id = kIdCompareFree;
    ClickButton(window, id);
}

int CheckedCompareMode(HWND window) {
    if (SendMessageW(GetDlgItem(window, kIdCompareSameGroup), BM_GETCHECK, 0, 0) == BST_CHECKED)
        return ce::kCompareSameGroup;
    if (SendMessageW(GetDlgItem(window, kIdCompareAllGroups), BM_GETCHECK, 0, 0) == BST_CHECKED)
        return ce::kCompareAllGroups;
    if (SendMessageW(GetDlgItem(window, kIdCompareFree), BM_GETCHECK, 0, 0) == BST_CHECKED)
        return ce::kCompareFree;
    return ce::kCompareOff;
}

// The invariant the comparison display promises: a row is ticked exactly when
// that curve is on the canvas.  The main curve draws when its own row is ticked;
// every other curve draws when it is in the compared set the hook reports.  Only
// comparison sources are held to this - for a plain source the tick is the bake
// scope and no comparison curve exists.
void CheckTicksMatchCanvas(HWND window, int channelCount, const char *what) {
    int info[12] = {};
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(info));
    for (int index = 0; index < channelCount; ++index) {
        const bool visible = ChannelVisible(window, index) == 1;
        bool drawn = false;
        if (index == info[1]) {
            drawn = visible;
        } else {
            for (int position = 0; position < info[2] && position < 8; ++position) {
                if (info[3 + position] == index) drawn = true;
            }
        }
        if (visible != drawn) {
            std::fprintf(stderr, "FAIL: %s: channel %d is ticked=%d but drawn=%d\n", what, index, visible ? 1 : 0,
                         drawn ? 1 : 0);
            std::exit(1);
        }
        ++g_checks;
    }
}

// Adds a key to the current selection instead of replacing it, the way a
// Ctrl+click does.
void ClickKeyAdditive(HWND window, int keyIndex) {
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(canvas != nullptr, "the canvas exists for an additive key click");
    const POINT point = AskPoint(window, kHookPointForKey, static_cast<WPARAM>(keyIndex));
    Check(point.x >= 0, "the key is on screen");
    const LPARAM position = MAKELPARAM(point.x, point.y);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON | MK_CONTROL, position);
    SendMessageW(canvas, WM_LBUTTONUP, 0, position);
}

void ReadCompare(HWND window, int info[12]) {
    for (int index = 0; index < 12; ++index) info[index] = 0;
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(info));
}

// lParam = double[3] {first, last, source}: the span the filter would use now.
bool ReadCompareSpan(HWND window, double span[3]) {
    span[0] = span[1] = span[2] = 0;
    return SendMessageW(window, kHookCompareRange, 0, reinterpret_cast<LPARAM>(span)) != 0;
}

// Reads a hidden WM_PRINTCLIENT render into memory, without desktop pixels.
// Comparison strokes are translucent and antialiased, so their source colour
// is not an exact output pixel value.
std::vector<unsigned> RenderPixels(HWND child) {
    RECT client = {};
    GetClientRect(child, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return {};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return {};
    }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all = {0, 0, width, height};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    const auto *pixels = static_cast<const unsigned *>(bits);
    // A BI_RGB 32 bit DIB keeps its bytes as B, G, R, unused.
    std::vector<unsigned> image;
    const int total = width * height;
    image.reserve(total);
    for (int index = 0; index < total; ++index) {
        image.push_back(pixels[index] & 0x00FFFFFFu);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return image;
}

int CountRedCurvePixels(const std::vector<unsigned> &image) {
    int count = 0;
    for (const unsigned pixel : image) {
        const int red = (pixel >> 16) & 255;
        const int green = (pixel >> 8) & 255;
        const int blue = pixel & 255;
        if (red > green + 20 && red > blue + 20) ++count;
    }
    return count;
}

// The mode set, the stability of the main curve and the host memory.
void ComparisonModes() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareSameGroup;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 0, {MakeKey(0, 0.0), MakeKey(10, 0.5)}));
    host.data.push_back(MakeGroupChannel("m_c", L"C", 1, {MakeKey(0, 0.0), MakeKey(10, 0.25)}));
    // Constant inside its own span, so it must never join an automatic set.
    host.data.push_back(MakeGroupChannel("m_d", L"D", 0, {MakeKey(0, 0.5), MakeKey(10, 0.5)}));
    // No group at all: only the free mode may show it.
    host.data.push_back(MakeGroupChannel("m_e", L"E", -1, {MakeKey(0, 0.0), MakeKey(10, 0.9)}));
    // Registered in a group but empty: never a comparison curve.
    host.data.push_back(MakeGroupChannel("m_f", L"F", 0, {}));

    const int appliesBeforeOpen = host.applyCalls;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(window != nullptr, "the comparison source opens hidden");
    Check(host.modeReads >= 1, "the editor asks the host for the remembered mode");
    Check(host.applyCalls == appliesBeforeOpen, "opening a comparison source writes no curve data");

    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareSameGroup, "the remembered mode is inherited on open");
    Check(info[1] == 0, "the initial main channel id selects the main curve");
    Check(info[2] == 1 && info[3] == 1, "the same-group set holds exactly the varying curve of that group");
    Check(IsWindowEnabled(GetDlgItem(window, kIdAddKey)) != FALSE, "the main curve stays editable");
    Check(GetText(window, kIdValueEdit).empty() == false, "the key fields still show the main curve");
    // An automatic mode owns the ticks: only the curves it draws may be ticked,
    // even though the host handed every channel over as selected.
    Check(ChannelVisible(window, 0) == 1, "the main curve is ticked in an automatic mode");
    Check(ChannelVisible(window, 1) == 1, "the curve the mode draws is ticked");
    Check(ChannelVisible(window, 2) == 0 && ChannelVisible(window, 3) == 0 && ChannelVisible(window, 4) == 0 &&
              ChannelVisible(window, 5) == 0,
          "nothing outside the automatic set stays ticked");
    CheckTicksMatchCanvas(window, 6, "same-group mode");
    Check(host.data[3].selected, "the host data itself is not rewritten by the mask");
    // A re-read hands the host flags back; the automatic mode restores its mask
    // instead of listing curves it does not draw.
    SendMessageW(window, WM_ACTIVATE, WA_ACTIVE, 0);
    Check(ChannelVisible(window, 3) == 0 && ChannelVisible(window, 1) == 1,
          "a re-read does not bring foreign ticks back into an automatic mode");
    CheckTicksMatchCanvas(window, 6, "same-group mode after a re-read");

    // Four groups: every registered and varying curve of groups 0..3.
    const int appliesBeforeSwitch = host.applyCalls;
    Check(IsWindowEnabled(GetDlgItem(window, kIdCompareAllGroups)) != FALSE,
          "the four-group button is enabled for a comparison source");
    ClickCompareMode(window, ce::kCompareAllGroups);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups, "one click on the four-group button picks that mode");
    Check(CheckedCompareMode(window) == ce::kCompareAllGroups, "the pressed button follows the mode");
    Check(info[2] == 2 && info[3] == 1 && info[4] == 2, "the four-group set holds both varying group curves");
    Check(info[1] == 0, "switching the mode keeps the main curve");
    Check(host.modeWrites == 1 && host.storedMode == ce::kCompareAllGroups,
          "the mode change reaches the host write callback");
    Check(host.applyCalls == appliesBeforeSwitch, "switching the mode writes no curve data");
    Check(host.data[1].track.keys.size() == 2 && host.data[1].track.keys[1].value == 0.5 &&
              host.data[3].track.keys.size() == 2 && host.data[3].track.keys[1].value == 0.5 &&
              host.data[5].track.keys.empty(),
          "the comparison curves keep their data through a mode switch");
    Check(ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 1, "the four-group mask ticks both curves");
    Check(ChannelVisible(window, 3) == 0 && ChannelVisible(window, 4) == 0 && ChannelVisible(window, 5) == 0,
          "the constant, ungrouped and empty curves stay unticked");
    CheckTicksMatchCanvas(window, 6, "four-group mode");

    // A manual show/hide leaves the automatic mode: the free display carries the
    // set that is on screen, and the click changes only the row it names.
    ClickChannelOnce(window, 3);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareFree, "a manual show/hide moves the display to the free segment");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "the free segment shows as selected");
    Check(ChannelVisible(window, 3) == 1, "one click shows a curve the automatic mode had hidden");
    Check(info[2] == 3 && info[3] == 1 && info[4] == 2 && info[5] == 3,
          "the drawn set gained exactly the clicked curve");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 1,
          "the curves that were visible stay visible");
    Check(ChannelVisible(window, 4) == 0 && ChannelVisible(window, 5) == 0, "the other hidden curves stay hidden");
    Check(ActiveChannelIndex(window) == 0, "a manual show/hide keeps the active curve");
    CheckTicksMatchCanvas(window, 6, "free mode after one click");
    ClickChannelOnce(window, 3);
    Check(ChannelVisible(window, 3) == 0, "a second click hides it again");
    CheckTicksMatchCanvas(window, 6, "free mode after the second click");

    // Pressing an automatic segment again regenerates its set.
    ClickCompareMode(window, ce::kCompareAllGroups);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups && info[2] == 2 && info[3] == 1 && info[4] == 2,
          "pressing the automatic segment regenerates its set");
    Check(ChannelVisible(window, 3) == 0, "the mask drops what the user had shown by hand");
    Check(ActiveChannelIndex(window) == 0, "the automatic segment keeps the main expression");
    CheckTicksMatchCanvas(window, 6, "regenerated four-group mode");

    // A host that refuses to store the mode keeps mode, flags and active curve,
    // so a manual change never half-applies.
    host.failModeWrite = true;
    const int appliesBeforeRefusal = host.applyCalls;
    ClickChannelOnce(window, 3);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups, "a refused mode write keeps the automatic mode");
    Check(CheckedCompareMode(window) == ce::kCompareAllGroups, "the segments stay on the automatic mode");
    Check(ChannelVisible(window, 3) == 0, "a refused mode write keeps the display flags");
    Check(ActiveChannelIndex(window) == 0, "a refused mode write keeps the active curve");
    Check(GetText(window, kIdStatus).empty() == false, "a refused mode write reports a status line");
    Check(host.applyCalls == appliesBeforeRefusal, "a refused mode write touches no curve data");
    CheckTicksMatchCanvas(window, 6, "refused manual change");
    host.failModeWrite = false;

    // The free mode shows exactly what the channel list has ticked.
    ce::CloseOwned(nullptr);
    host.data[1].selected = true;
    host.data[2].selected = false;
    host.data[3].selected = false;
    host.data[4].selected = false;
    host.data[5].selected = false;
    host.storedMode = ce::kCompareFree;
    ce::Session freeSession = MakeSession(host);
    HWND freeWindow = static_cast<HWND>(ce::Open(nullptr, freeSession, false));
    Check(freeWindow != nullptr, "the editor reopens for the free mode");
    ReadCompare(freeWindow, info);
    Check(info[0] == ce::kCompareFree, "the free mode is inherited from the host");
    Check(CheckedCompareMode(freeWindow) == ce::kCompareFree, "the free button shows the inherited mode");
    Check(info[2] == 1 && info[3] == 1, "the free mode shows exactly the ticked curve");
    Check(info[1] == 0, "the free mode keeps the main curve");
    Check(ChannelVisible(freeWindow, 2) == 0 && ChannelVisible(freeWindow, 3) == 0,
          "the free mode leaves the unticked rows unticked");
    CheckTicksMatchCanvas(freeWindow, 6, "inherited free mode");

    // A curve without keys has nothing to draw: ticking it would promise a
    // picture that never appears, so it stays unticked and says why.
    ClickChannelOnce(freeWindow, 5);
    Check(ChannelVisible(freeWindow, 5) == 0, "a curve without keys cannot be ticked into the display");
    Check(GetText(freeWindow, kIdStatus).empty() == false, "the refused tick reports a status line");
    CheckTicksMatchCanvas(freeWindow, 6, "refused tick of a curve without keys");

    // There is no off button: a comparison source always shows one of the three
    // modes, and pressing the pressed button changes nothing.
    const int writesBefore = host.modeWrites;
    ClickCompareMode(freeWindow, ce::kCompareFree);
    ReadCompare(freeWindow, info);
    Check(host.modeWrites == writesBefore, "pressing the current mode writes nothing");
    Check(info[0] == ce::kCompareFree && info[2] == 1, "the set is unchanged by pressing the current mode");
    Check(CheckedCompareMode(freeWindow) == ce::kCompareFree, "exactly one mode button stays pressed");
    Check(info[1] == 0, "the main curve is still active");
    // Free is the segment manual work enters, so picking it never restores the
    // main expression: the curve the user activated stays active.
    DoubleClickChannel(freeWindow, 2);
    Check(ActiveChannelIndex(freeWindow) == 2, "a double click activates the row it names");
    Check(CheckedCompareMode(freeWindow) == ce::kCompareFree, "the double click stays on the free segment");
    ClickCompareMode(freeWindow, ce::kCompareFree);
    Check(ActiveChannelIndex(freeWindow) == 2, "picking the free segment keeps the curve the user activated");
    Check(ChannelVisible(freeWindow, 1) == 1 && ChannelVisible(freeWindow, 2) == 1,
          "picking the free segment changes no display flag");
    CheckTicksMatchCanvas(freeWindow, 6, "free segment after a double click");
    ce::CloseOwned(nullptr);
    Note("comparison modes, main-curve stability and host memory");
}

// The range filter: the span, its source, the ticked curves and the readout.
void ComparisonRangeFilter() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareAllGroups;
    host.initialMain = "main";
    host.data.push_back(MakeGroupChannel("main", L"Main", 0,
                                         {MakeKey(0, 0.0), MakeKey(10, 0.0), MakeKey(20, 0.0)}));
    // Equal endpoints with a Bezier bump in between: flat at every integer
    // frame only for the first ten frames, so the span decides.
    host.data.push_back(MakeGroupChannel("bump", L"Bump", 0,
                                         {MakeShapeKey(0, 0.0, 0.0, 2.0), MakeShapeKey(20, 0.0, -2.0, 0.0)}));
    // One value everywhere.
    host.data.push_back(MakeGroupChannel("flat", L"Flat", 1,
                                         {MakeShapeKey(0, 0.5, 0.0, 0.0), MakeShapeKey(20, 0.5, 0.0, 0.0)}));
    // Varies at the front, constant afterwards.
    host.data.push_back(MakeGroupChannel("early", L"Early", 1,
                                         {MakeShapeKey(0, 0.0, 0.0, 0.0), MakeShapeKey(5, 1.0, 0.0, 0.0)}));
    // Registered in a group but without keys: nothing to draw, so the filter
    // must never tick it.
    host.data.push_back(MakeGroupChannel("ghost", L"Ghost", 1, {}));

    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(StyleVisible(window, kIdCompareFilter), "the filter button is shown");
    Check(CheckedCompareMode(window) == ce::kCompareAllGroups, "the filter starts from the stored mode");

    // Two selected keys of the main curve: frames 10 and 20.
    ClickKey(window, 1);
    ClickKeyAdditive(window, 2);
    double span[3] = {};
    Check(ReadCompareSpan(window, span), "a span is available once two keys are selected");
    Check(span[2] == 1, "the span comes from the selected keys first");
    Check(span[0] == 10.0 && span[1] == 20.0, "the span is the smallest and largest selected frame");

    // One click runs the filter once: it ticks the curves that really change in
    // the span and hands the result to the free display.
    const int writesBefore = host.modeWrites;
    ClickButton(window, kIdCompareFilter);
    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareFree, "the filter hands its result to the free display");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "the free button is pressed after a filter run");
    Check(host.modeWrites == writesBefore + 1, "the mode the filter picks reaches the host once");
    Check(host.applyCalls == 0, "a filter run writes no curve data");
    Check(info[1] == 0, "a filter run keeps the active curve");
    Check(info[2] == 1 && info[3] == 1, "only the curve that varies inside the span is shown");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1,
          "the main curve and the varying curve keep their tick");
    Check(ChannelVisible(window, 2) == 0 && ChannelVisible(window, 3) == 0,
          "the flat curve and the curve that is flat inside [10,20] are hidden");
    Check(ChannelVisible(window, 4) == 0, "a curve without keys is never ticked by the filter");
    CheckTicksMatchCanvas(window, 5, "after a filter run");
    const std::wstring filtered = GetText(window, kIdStatus);
    Check(filtered.find(L"10") != std::wstring::npos && filtered.find(L"20") != std::wstring::npos,
          "the status line names the span that was used");
    Check(filtered.find(L"\u6765\u81EA\u9009\u4E2D\u5173\u952E\u5E27") != std::wstring::npos,
          "the status line names the selected keys as the source of the span");

    // A second run with the range fields as the source: the early curve varies
    // inside [0,5] and the flat one still does not.
    ClickKey(window, 0);
    SetText(window, kIdBakeFirstEdit, L"0");
    SetText(window, kIdBakeLastEdit, L"5");
    Check(ReadCompareSpan(window, span), "the range fields provide a span without a key selection");
    Check(span[2] == 2, "the span falls back to the range fields");
    Check(span[0] == 0.0 && span[1] == 5.0, "the range fields are used as written");
    ClickButton(window, kIdCompareFilter);
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareFree && info[2] == 2 && info[3] == 1 && info[4] == 3,
          "both curves that vary inside the new span are shown");
    Check(ChannelVisible(window, 1) == 1 && ChannelVisible(window, 3) == 1, "the two varying curves are ticked");
    Check(ChannelVisible(window, 2) == 0, "the flat curve stays hidden");
    const std::wstring second = GetText(window, kIdStatus);
    Check(second.find(L"5") != std::wstring::npos, "the status line names the second span too");
    Check(ActiveChannelIndex(window) == 0, "filter runs never move the active curve");

    // A run without a usable span reports the problem and changes nothing.
    SetText(window, kIdBakeFirstEdit, L"");
    SetText(window, kIdBakeLastEdit, L"");
    ClickButton(window, kIdCompareFilter);
    const std::wstring refused = GetText(window, kIdStatus);
    Check(refused.empty() == false, "a run without a span reports a status line");
    Check(ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 0,
          "a refused run keeps the ticks it found");
    ce::CloseOwned(nullptr);
    Note("one-shot range filter: span, source, ticked curves and preserved activity");
}

// The background curves reach the canvas and leave again.
void ComparisonPainting() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareFree;
    // Fixed axis colours distinguish the green main curve from the translucent
    // red comparison curve without depending on hashed channel IDs or the old
    // opaque-grey rendering. The surrounding grid is neutral or blue.
    host.initialMain = "y";
    host.data.push_back(MakeGroupChannel("y", L"Y", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("x", L"X", 0, {MakeKey(0, 0.0), MakeKey(10, 0.2)}));
    host.data[1].selected = false;
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    const auto mainOnly = RenderPixels(canvas);
    Check(!mainOnly.empty() && ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 0,
          "the comparison paint fixture starts with only the green main curve");
    ClickCompareMode(window, ce::kCompareSameGroup);
    const auto visible = RenderPixels(canvas);
    Check(visible.size() == mainOnly.size() && visible != mainOnly &&
              CountRedCurvePixels(visible) > CountRedCurvePixels(mainOnly),
          "the background comparison curve is painted in its own colour");

    // A mode button repaints without losing the background curve.
    ClickCompareMode(window, ce::kCompareFree);
    Check(RenderPixels(canvas) == visible, "the free mode still paints the same main and ticked comparison curves");
    ClickCompareMode(window, ce::kCompareSameGroup);
    Check(RenderPixels(canvas) == visible, "the same-group mode paints the same curves again");

    // Hiding it in the list takes it off the canvas; the main curve stays.
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 0 && ActiveChannelIndex(window) == 0,
          "hiding the comparison curve preserves the main curve's active channel");
    const auto hidden = RenderPixels(canvas);
    Check(hidden == mainOnly && hidden != visible,
          "hiding the curve restores exactly the main-only canvas");
    Check(RenderInk(canvas) > 100, "the main curve is still painted");
    ClickChannelOnce(window, 1);
    Check(RenderPixels(canvas) == visible, "showing it again restores the complete original curve rendering");
    ce::CloseOwned(nullptr);
    Note("comparison curves paint behind the main curve through WM_PRINTCLIENT");
}

// A source without comparison metadata keeps the old control set and the old
// behaviour, in all three languages.
void ComparisonNoRegression() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {MakeKey(0, 0.0), MakeKey(10, 5.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    for (int id : {kIdCompareLabel, kIdCompareSameGroup, kIdCompareAllGroups, kIdCompareFree, kIdCompareFilter})
        Check(StyleVisible(window, id) == false,
              "a source without comparison metadata keeps the comparison row hidden");
    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareOff && info[2] == 0 && info[1] == 0,
          "a source without comparison metadata stays in the plain single-curve view");
    Check(host.modeWrites == 0 && host.modeReads == 0, "no comparison callback is used for a plain source");

    const int appliesBefore = host.applyCalls;
    SetText(window, kIdFrameEdit, L"20");
    SetText(window, kIdValueEdit, L"1.5");
    Click(window, kIdAddKey);
    Check(host.applyCalls == appliesBefore + 1, "editing still reaches a source without comparison metadata");
    Check(KeyCount(window) == 3, "the added key arrived");
    Click(window, kIdUndo);
    Check(KeyCount(window) == 2, "undo still works without comparison");

    for (unsigned language = 0; language < 3; ++language) {
        ce::SetLanguage(nullptr, language);
        Check(StyleVisible(window, kIdCompareSameGroup) == false,
              "the comparison row stays hidden in every language");
    }
    Check(RenderInk(GetDlgItem(window, kIdCanvas)) > 100, "the canvas paints the plain single curve");
    ce::CloseOwned(nullptr);
    Note("a source without comparison metadata is unchanged");
}

// The comparison row in the three languages and at the minimum window size.
void ComparisonLayout() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareAllGroups;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 1, {MakeKey(0, 0.0), MakeKey(10, 0.5)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(StyleVisible(window, kIdCompareSameGroup), "the comparison row appears for a comparison source");
    const wchar_t *names[3] = {L"ja", L"en", L"zh"};
    for (unsigned language = 0; language < 3; ++language) {
        ce::SetLanguage(nullptr, language);
        Check(GetText(window, kIdCompareFilter).empty() == false, "the filter button is translated");
        Check(GetText(window, kIdCompareSameGroup).empty() == false, "the same-group button is translated");
        Check(GetText(window, kIdCompareAllGroups).empty() == false, "the four-group button is translated");
        Check(GetText(window, kIdCompareFree).empty() == false, "the free button is translated");
        Check(CheckedCompareMode(window) == ce::kCompareAllGroups,
              "a language switch keeps the pressed comparison mode");
        CheckLayout(window, names[language]);
    }

    // The three segments are one control: equal width, touching, in the visual
    // order same group - free - four groups, while the stored values stay
    // 1 same group, 2 four groups and 3 free.
    RECT left = {}, middle = {}, right = {};
    Check(GetWindowRect(GetDlgItem(window, kIdCompareSameGroup), &left) != FALSE, "the left segment exists");
    Check(GetWindowRect(GetDlgItem(window, kIdCompareFree), &middle) != FALSE, "the middle segment exists");
    Check(GetWindowRect(GetDlgItem(window, kIdCompareAllGroups), &right) != FALSE, "the right segment exists");
    Check(left.left < middle.left && middle.left < right.left,
          "the visual order is same group, free, four groups");
    Check(left.right == middle.left && middle.right == right.left,
          "the three segments touch, so they read as one segmented control");
    Check((left.right - left.left) == (middle.right - middle.left) &&
              (middle.right - middle.left) == (right.right - right.left),
          "the three segments share one width");
    Check(left.top == middle.top && middle.top == right.top && left.bottom == middle.bottom &&
              middle.bottom == right.bottom,
          "the three segments share one row and one height");
    Check(static_cast<int>(SendMessageW(GetDlgItem(window, kIdCompareSameGroup), BM_GETCHECK, 0, 0)) ==
                  BST_UNCHECKED &&
              static_cast<int>(SendMessageW(GetDlgItem(window, kIdCompareFree), BM_GETCHECK, 0, 0)) ==
                  BST_UNCHECKED &&
              static_cast<int>(SendMessageW(GetDlgItem(window, kIdCompareAllGroups), BM_GETCHECK, 0, 0)) ==
                  BST_CHECKED,
          "only the current segment shows the selected state");
    ClickCompareMode(window, ce::kCompareFree);
    Check(CheckedCompareMode(window) == ce::kCompareFree, "one click selects the middle segment");
    Check(ActiveChannelIndex(window) == 0, "a segment click keeps the main curve");
    ClickCompareMode(window, ce::kCompareSameGroup);
    Check(CheckedCompareMode(window) == ce::kCompareSameGroup, "one click selects the left segment");
    MINMAXINFO limits = {};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    SetWindowPos(window, nullptr, 0, 0, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(limits.ptMinTrackSize.x, limits.ptMinTrackSize.y));
    CheckLayout(window, L"minimum comparison");
    ce::CloseOwned(nullptr);
    Note("comparison row in three languages and at the minimum window size");
}

// ---------------------------------------------------------------------------
// 24. The channel tree separates visibility checkboxes from activation labels;
// keyboard navigation and activation must follow the actual clicked leaf.
// ---------------------------------------------------------------------------
void ChannelListInteraction() {
    HostSource host;
    host.data.push_back(MakeChannel("a", L"A", {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeChannel("b", L"B", {MakeKey(0, 0.0), MakeKey(10, 2.0), MakeKey(20, 3.0)}));
    host.data.push_back(MakeChannel("c", L"C", {MakeKey(0, 0.0), MakeKey(10, 4.0), MakeKey(20, 5.0),
                                                MakeKey(30, 6.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND list = GetDlgItem(window, kIdChannelList);

    Check(ActiveChannelIndex(window) == 0, "the first channel starts active");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1 && ChannelVisible(window, 2) == 1,
          "every curve starts shown");
    Check(ChannelRowText(window, 0).find(L"\u25CF") == 0, "the active row carries the active marker");
    Check(ChannelRowText(window, 1).find(L"\u25CF") == std::wstring::npos,
          "an inactive row carries no active marker");

    ClickChannelLabel(window, 1);
    Check(ChannelCaretIndex(window) == 1 && ActiveChannelIndex(window) == 1 && KeyCount(window) == 3,
          "a label click activates the leaf and moves the keyboard caret to it");
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    Check(ChannelVisible(window, 1) == 0 && ChannelVisible(window, 0) == 1 && ChannelVisible(window, 2) == 1 &&
              ActiveChannelIndex(window) == 1,
          "Space after a label click toggles only that leaf without changing the active channel");
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    Check(ChannelCaretIndex(window) == 1 && ActiveChannelIndex(window) == 1 && ChannelVisible(window, 1) == 1,
          "Enter after a label click reactivates and shows the same leaf");
    ClickChannelLabel(window, 0);
    Check(ChannelCaretIndex(window) == 0 && ActiveChannelIndex(window) == 0 && KeyCount(window) == 2 &&
              host.applyCalls == 0,
          "clicking another label moves both caret and active channel without writing authored keys");

    // One checkbox click: show or hide only, never a change of the active curve.
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 0, "a single click hides the clicked curve");
    Check(ActiveChannelIndex(window) == 0, "a single click never changes the active curve");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 2) == 1,
          "a single click leaves the other curves alone");
    Check(KeyCount(window) == 2, "the key fields still describe the first curve");
    Check(ChannelCaretIndex(window) == 1,
          "the caret follows the clicked row");
    // Plain sources still overlay their other visible tracks, but never gain
    // the expression comparison modes or invoke their persistence callbacks.
    int plainInfo[12] = {};
    SendMessageW(window, kHookCompare, 0, reinterpret_cast<LPARAM>(plainInfo));
    Check(plainInfo[0] == ce::kCompareOff && host.modeReads == 0 && host.modeWrites == 0 &&
              !StyleVisible(window, kIdCompareSameGroup) && !StyleVisible(window, kIdCompareAllGroups) &&
              !StyleVisible(window, kIdCompareFree),
          "a plain source never gains a comparison mode from the list");
    Check(plainInfo[2] == 1 && plainInfo[3] == 2,
          "a plain source continues to draw exactly the untouched visible background channel");
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 1, "a second single click shows the curve again");
    Check(ActiveChannelIndex(window) == 0, "the active curve is still the first one");

    // A label double click includes a single click first; both activate the
    // same leaf without toggling its visibility checkbox.
    DoubleClickChannel(window, 2);
    Check(ActiveChannelIndex(window) == 2, "a double click makes that curve the active one");
    Check(ChannelVisible(window, 2) == 1, "a double click leaves the curve shown after its first click");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1,
          "a double click leaves the other curves alone");
    Check(KeyCount(window) == 4, "the key fields follow the newly active curve");
    Check(ChannelRowText(window, 2).find(L"\u25CF") == 0, "the active marker moved to the new row");
    Check(ChannelRowText(window, 0).find(L"\u25CF") == std::wstring::npos, "the old row lost the marker");

    // Activating a previously hidden leaf makes it visible again.
    ClickChannelOnce(window, 1);
    Check(ChannelVisible(window, 1) == 0, "the row is hidden before the double click");
    DoubleClickChannel(window, 1);
    Check(ChannelVisible(window, 1) == 1, "a double click on a hidden row never leaves it hidden");
    Check(ActiveChannelIndex(window) == 1, "the double click activated the hidden row");
    Check(ChannelVisible(window, 2) == 1, "the previously active curve is still shown");

    // Keyboard parity: arrows move the caret only, Space toggles, Enter
    // activates.
    TreeView_SelectItem(list, ChannelItem(window, 0));
    SendMessageW(list, WM_KEYDOWN, VK_DOWN, 0);
    Check(ChannelCaretIndex(window) == 1, "the down arrow moves the caret");
    Check(ActiveChannelIndex(window) == 1, "the down arrow does not change the active curve");
    const int beforeSpace = ActiveChannelIndex(window);
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    Check(ChannelVisible(window, 1) == 0, "Space hides or shows the caret row");
    Check(ActiveChannelIndex(window) == beforeSpace, "Space never changes the active curve");
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    Check(ChannelVisible(window, 1) == 1, "a second Space shows it again");
    SendMessageW(list, WM_KEYDOWN, VK_UP, 0);
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    Check(ActiveChannelIndex(window) == 0, "Enter activates the caret row");
    Check(KeyCount(window) == 2, "the key fields follow the keyboard activation");
    SendMessageW(list, WM_KEYDOWN, VK_END, 0);
    Check(ChannelCaretIndex(window) == 2, "End reaches the last row");
    SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    Check(ActiveChannelIndex(window) == 2 && ChannelVisible(window, 2) == 1,
          "Enter on a hidden row activates and shows it");
    Check(KeyCount(window) == 4, "the active curve really changed by keyboard");
    ClickChannelOnce(window, 0);
    Check(ChannelVisible(window, 0) == 0 && ActiveChannelIndex(window) == 2,
          "a click on another row only hides that row");
    Check(RenderInk(GetDlgItem(window, kIdCanvas)) > 20, "a hidden active curve still paints its notice");
    ce::CloseOwned(nullptr);
    Note("channel list single click, double click and keyboard reachability");
}

// ---------------------------------------------------------------------------
// 25. Hover help: one line per control, cleared on leave, never over the
// persistent status or error text.
// ---------------------------------------------------------------------------
void HoverHints() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 1, {MakeKey(0, 0.0), MakeKey(10, 0.5)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    HWND canvas = GetDlgItem(window, kIdCanvas);
    HWND mmd = GetDlgItem(window, kIdMmdPanel);
    Check(HoverHint(window) == 0 && GetText(window, kIdHoverHint).empty(), "the hint line starts empty");

    // The three canvas regions explain themselves separately.
    const POINT plot = AskPoint(window, kHookPointForFrame, 5);
    SendMessageW(canvas, WM_MOUSEMOVE, 0, MAKELPARAM(plot.x, plot.y));
    Check(HoverHint(window) == 0, "the plot has no redundant gesture paragraph");
    Check(GetText(window, kIdHoverHint).empty(), "the plot leaves the hint line empty");
    const POINT timeBar = AskBarPoint(window, 1, 0);
    SendMessageW(canvas, WM_MOUSEMOVE, 0, MAKELPARAM(timeBar.x, timeBar.y));
    Check(HoverHint(window) == 9, "the horizontal bar explains panning and zooming in time");
    const POINT valueBar = AskBarPoint(window, 2, 0);
    SendMessageW(canvas, WM_MOUSEMOVE, 0, MAKELPARAM(valueBar.x, valueBar.y));
    Check(HoverHint(window) == 10, "the vertical bar explains panning and zooming in value");
    SendMessageW(canvas, WM_MOUSELEAVE, 0, 0);
    Check(HoverHint(window) == 0, "leaving the canvas clears the hint");
    Check(GetText(window, kIdHoverHint).empty(), "the cleared hint line is empty");

    SendMessageW(mmd, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    Check(HoverHint(window) == 8, "the MMD panel explains its handles");
    SendMessageW(mmd, WM_MOUSELEAVE, 0, 0);
    Check(HoverHint(window) == 0, "leaving the MMD panel clears the hint");

    // Child controls answer through their subclass.
    const struct {
        int id;
        int hint;
        const char *text;
    } targets[] = {
        {kIdChannelList, 1, "the channel list explains click, double click and the keys"},
        {kIdCompareSameGroup, 2, "the same-group button explains the modes"},
        {kIdCompareAllGroups, 2, "the four-group button explains the modes"},
        {kIdCompareFree, 2, "the free button explains the modes"},
        {kIdCompareFilter, 3, "the filter button explains the span and the ticks"},
        {kIdAddKey, 4, "the add button explains itself"},
        {kIdDeleteKey, 5, "the delete button explains itself"},
        {kIdApplyKey, 6, "the apply button explains itself"},
    };
    for (const auto &target : targets) {
        HoverControl(window, target.id);
        Check(HoverHint(window) == target.hint, target.text);
        Check(GetText(window, kIdHoverHint).empty() == false, "a hovered control always shows a sentence");
        LeaveControl(window, target.id);
        Check(HoverHint(window) == 0, "leaving the control clears the hint");
    }

    // The hint is retranslated while it is on screen.
    HoverControl(window, kIdChannelList);
    const std::wstring chinese = GetText(window, kIdHoverHint);
    ce::SetLanguage(nullptr, 1);
    const std::wstring english = GetText(window, kIdHoverHint);
    Check(english.empty() == false && english != chinese, "the hint follows a language change");
    ce::SetLanguage(nullptr, 0);
    Check(GetText(window, kIdHoverHint) != english, "the hint follows the second language change too");
    ce::SetLanguage(nullptr, 2);
    Check(GetText(window, kIdHoverHint) == chinese, "the Chinese hint comes back unchanged");
    LeaveControl(window, kIdChannelList);

    // A hover never touches the persistent status line, so an error stays
    // readable; leaving restores the hint line only.
    const LRESULT focusBefore = FocusId(window);
    SetText(window, kIdFrameEdit, L"not a frame");
    Click(window, kIdAddKey);
    const std::wstring error = GetText(window, kIdStatus);
    Check(error.empty() == false, "the failed action left a status line");
    HoverControl(window, kIdChannelList);
    Check(HoverHint(window) == 1, "the hint is shown while the error line is on screen");
    Check(GetText(window, kIdStatus) == error, "a hover never overwrites the persistent status line");
    Check(FocusId(window) == focusBefore, "a hover never moves the keyboard focus");
    LeaveControl(window, kIdChannelList);
    Check(HoverHint(window) == 0, "leaving clears the hint again");
    Check(GetText(window, kIdStatus) == error, "the error line survives the leave as well");
    ce::CloseOwned(nullptr);
    Note("hover hints per control, cleared on leave and never over the status line");
}

// The automatic segments are the documented way back to the main expression:
// same group and four groups restore the curve Source::initialMainChannel names,
// show it and write their set into the list.  The free segment is the one manual
// work enters, so picking it leaves the active curve and the display flags alone.
void ComparisonModeRestoresMainCurve() {
    HostSource host;
    host.compareEnabled = true;
    host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareSameGroup;
    host.initialMain = "m_a";
    host.data.push_back(MakeGroupChannel("m_a", L"A", 0, {MakeKey(0, 0.0), MakeKey(10, 1.0)}));
    host.data.push_back(MakeGroupChannel("m_b", L"B", 0, {MakeKey(0, 0.0), MakeKey(10, 2.0), MakeKey(20, 3.0)}));
    host.data.push_back(MakeGroupChannel("m_c", L"C", 1, {MakeKey(0, 0.0), MakeKey(10, 4.0), MakeKey(20, 5.0),
                                                          MakeKey(30, 6.0)}));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(ActiveChannelIndex(window) == 0, "the window opens on the main curve");
    CheckTicksMatchCanvas(window, 3, "same-group entry");

    DoubleClickChannel(window, 2);
    Check(ActiveChannelIndex(window) == 2, "a double click moves the active curve away from the main one");
    Check(KeyCount(window) == 4, "the key fields follow the curve the user picked");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "the double click stays on the free segment");
    Check(ChannelVisible(window, 2) == 1, "the double click shows the curve it activated");
    Check(ChannelVisible(window, 0) == 1 && ChannelVisible(window, 1) == 1,
          "the double click leaves the other visible curves alone");
    CheckTicksMatchCanvas(window, 3, "after a double click");

    ClickCompareMode(window, ce::kCompareAllGroups);
    Check(ActiveChannelIndex(window) == 0, "pressing an automatic segment restores the main curve");
    Check(KeyCount(window) == 2, "the key fields follow the restored main curve");
    int info[12] = {};
    ReadCompare(window, info);
    Check(info[0] == ce::kCompareAllGroups && info[2] == 2 && info[3] == 1 && info[4] == 2,
          "the four-group set is computed around the restored main curve");
    CheckTicksMatchCanvas(window, 3, "restored four-group mode");

    ClickCompareMode(window, ce::kCompareSameGroup);
    ReadCompare(window, info);
    Check(info[2] == 1 && info[3] == 1, "the same-group set follows the restored main curve's group");
    CheckTicksMatchCanvas(window, 3, "restored same-group mode");

    // The main curve can be hidden by hand; an automatic segment shows it again.
    ClickChannelOnce(window, 0);
    Check(ChannelVisible(window, 0) == 0, "the main curve can be hidden from the list");
    Check(CheckedCompareMode(window) == ce::kCompareFree, "hiding by hand moves the display to free");
    ClickCompareMode(window, ce::kCompareFree);
    Check(ChannelVisible(window, 0) == 0, "picking the free segment does not force the main curve back");
    ClickCompareMode(window, ce::kCompareSameGroup);
    Check(ChannelVisible(window, 0) == 1, "an automatic segment shows the main curve again");
    Check(ActiveChannelIndex(window) == 0, "the automatic segment keeps the main curve active");
    CheckTicksMatchCanvas(window, 3, "after the automatic segment restored the main curve");
    Check(HoverHint(window) == 0, "no hint is left over from the interactions");
    ce::CloseOwned(nullptr);
    Note("automatic segments restore and show the main curve, free leaves the user's choice");
}

void AdjacentHandleLinking() {
    HostSource host;
    for(const char* id : {"rotation.x","rotation.y","rotation.z"}) {
        auto channel=MakeChannel(id,L"Rotation",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2),MakeKey(30,3)});
        channel.interpolationGroup="rotation";
        std::string error;
        for(std::size_t end=1;end<4;++end)Check(ce::SetChannelControls(channel,end,{20,20,107,107},error),"initialize linked intervals");
        host.data.push_back(channel);
    }
    bool automatic=true;
    auto session=MakeSession(host);session.sources[0].linkAdjacentControls=[&]{return automatic;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,session,false));ClickKey(window,2);
    HWND panel=GetDlgItem(window,kIdMmdPanel);
    const auto read=[&](int end){std::array<double,4> controls{};Check(ce::ReadChannelControls(host.data[0],end,controls),"read linked controls");return controls;};
    const auto drag=[&](int handle,int x,int y){
        double current[4]{};Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(current))!=0,"linked panel is editable");
        const auto getDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
        UINT dpi=getDpi?getDpi(panel):0;
        if(!dpi){HDC dc=GetDC(panel);dpi=dc?UINT(GetDeviceCaps(dc,LOGPIXELSX)):96;if(dc)ReleaseDC(panel,dc);}
        RECT client{};GetClientRect(panel,&client);const int margin=MulDiv(6,dpi?dpi:96,96);
        const int side=std::min(int(client.right),int(client.bottom))-2*margin;
        const auto point=[&](double a,double b){return MAKELPARAM(margin+int(std::lround(a*side/127.0)),margin+side-int(std::lround(b*side/127.0)));};
        SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point(current[handle*2],current[handle*2+1]));
        SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,point(x,y));SendMessageW(panel,WM_LBUTTONUP,0,point(x,y));
    };
    drag(1,80,110);auto edited=read(2),neighbor=read(3);
    Check(std::fabs(neighbor[0]-(127-edited[2]))<0.001&&std::fabs(neighbor[1]-(127-edited[3]))<0.001,"end handle links the next interval start");
    Check(read(1)==std::array<double,4>{20,20,107,107},"linking does not cascade to the previous interval");
    for(const auto& channel:host.data){std::array<double,4> controls{};Check(ce::ReadChannelControls(channel,3,controls)&&controls==neighbor,"shared rotation axes link together");}
    Click(window,kIdUndo);Check(std::fabs(read(3)[0]-20)<0.001,"one undo restores the linked neighbor");
    Click(window,kIdRedo);Check(std::fabs(read(3)[0]-neighbor[0])<0.001,"redo restores the linked edit");
    automatic=false;const auto previous=read(1);drag(0,40,60);Check(read(1)==previous,"unchecked auto leaves the previous interval intact");
    automatic=true;drag(0,45,70);edited=read(2);neighbor=read(1);
    Check(std::fabs(neighbor[2]-(127-edited[0]))<0.001&&std::fabs(neighbor[3]-(127-edited[1]))<0.001,"start handle links the previous interval end");
    host.failApply=true;const auto before=host.data;drag(1,70,105);Check(host.data[0].track.keys[2].inSlope==before[0].track.keys[2].inSlope,"host refusal rolls back all linked changes");
    ce::CloseOwned(nullptr);Note("automatic adjacent handles and atomic undo/redo");
}


// A full value curve owns its tangents even when the legacy MMD panel links
// adjacent interpolation handles. Exercise actual drags, history and fields.
void IndependentValueHandles() {
    for (bool outSide : {true, false}) {
        HostSource host;
        auto curve = MakeChannel("pos.x", L"Position X", {
            MakeKey(0, 0, ac::Interpolation::Bezier, ac::Tangent::Free),
            MakeKey(20, 5, ac::Interpolation::Bezier, ac::Tangent::Free),
            MakeKey(40, 10, ac::Interpolation::Bezier, ac::Tangent::Free)});
        curve.track.keys[1].inWeight = curve.track.keys[1].outWeight = 0.3;
        curve.track.keys[1].inSlope = curve.track.keys[1].outSlope = 0;
        host.data.push_back(curve);
        auto session = MakeSession(host);
        session.sources[0].linkAdjacentControls = [] { return true; };
        HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
        ClickKey(window, 1);
        HWND canvas = GetDlgItem(window, kIdCanvas);
        const POINT key = AskPoint(window, kHookPointForKey, 1);
        const POINT handle = AskPoint(window, kHookPointForFrame, outSide ? 26 : 14);
        const POINT target = AskPoint(window, kHookPointForFrame, outSide ? 30 : 10);
        const LPARAM origin = MAKELPARAM(handle.x, key.y);
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, origin);
        SendMessageW(canvas, WM_LBUTTONUP, 0, origin);
        Check(host.applyCalls == 0, "a value handle click does not create history");
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, origin);
        SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target.x, key.y + 20));
        SendMessageW(canvas, WM_LBUTTONUP, 0, origin);
        Check(host.applyCalls == 0, "returning the value handle to the press point does not publish a change");
        Click(window, kIdUndo);
        Check(host.applyCalls == 0, "returning the value handle to the press point leaves no undo action");
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, origin);
        Check(GetCapture() == canvas, "the value handle owns capture before the unsampled release");
        SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(target.x, key.y + (outSide ? -20 : 20)));
        const auto changed = host.data[0].track.keys[1];
        Check(host.applyCalls == 1, "value handle drag commits once");
        Check(std::fabs(outSide ? changed.outSlope : changed.inSlope) > 0.01,
              "the dragged value handle changes slope");
        Check(std::fabs((outSide ? changed.outWeight : changed.inWeight) - 0.5) < 0.015,
              "the value handle uses the actual release coordinate without a move message");
        Check((outSide ? changed.inSlope : changed.outSlope) == 0 &&
              (outSide ? changed.inWeight : changed.outWeight) == 0.3,
              "the opposite value handle remains independent with automatic linking on");
        Click(window, kIdUndo);
        Check(host.data[0].track.keys[1].inSlope == 0 && host.data[0].track.keys[1].outSlope == 0,
              "undo restores both handles");
        Click(window, kIdRedo);
        Check(host.data[0].track.keys[1].inSlope == changed.inSlope &&
              host.data[0].track.keys[1].outSlope == changed.outSlope,
              "redo preserves the independent handle edit");
        ClickKey(window, 1);
        SetText(window, outSide ? kIdOutSlopeEdit : kIdInSlopeEdit, L"0.2");
        Click(window, kIdApplyKey);
        Check((outSide ? host.data[0].track.keys[1].inSlope : host.data[0].track.keys[1].outSlope) == 0,
              "numeric free-handle editing also leaves the opposite handle alone");
        ce::CloseOwned(nullptr);
    }
    Note("independent value handles with legacy auto-link enabled");
}


void AlignedValueHandles() {
    for (bool outSide : {true, false}) {
        HostSource host;
        auto curve = MakeChannel("pos.x", L"Position X", {
            MakeKey(0, 0, ac::Interpolation::Bezier, ac::Tangent::Free),
            MakeKey(20, 5, ac::Interpolation::Bezier, ac::Tangent::Aligned),
            MakeKey(40, 10, ac::Interpolation::Bezier, ac::Tangent::Free)});
        curve.track.keys[1].inWeight = curve.track.keys[1].outWeight = 0.3;
        curve.track.keys[1].inSlope = curve.track.keys[1].outSlope = 0;
        host.data.push_back(curve);
        auto session = MakeSession(host);
        session.sources[0].linkAdjacentControls = [] { return true; };
        HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
        ClickKey(window, 1);
        HWND canvas = GetDlgItem(window, kIdCanvas);
        const auto key = AskPoint(window, kHookPointForKey, 1);
        const auto handle = AskPoint(window, kHookPointForFrame, outSide ? 26 : 14);
        const auto target = AskPoint(window, kHookPointForFrame, outSide ? 30 : 10);
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(handle.x, key.y));
        const LPARAM destination=MAKELPARAM(target.x,key.y+(outSide?-20:20));
        SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, destination);
        SendMessageW(canvas, WM_LBUTTONUP, 0, destination);
        const auto& changed = host.data[0].track.keys[1];
        Check(host.applyCalls == 1 && std::fabs(changed.outSlope) > 0.01,
              "an aligned value handle drag commits");
        Check(changed.tangent == ac::Tangent::Aligned && changed.inSlope == changed.outSlope,
              "explicit aligned mode still couples slopes");
        Check((outSide ? changed.inWeight : changed.outWeight) == 0.3,
              "aligned mode preserves the opposite handle length");
        ce::CloseOwned(nullptr);
    }
    Note("explicit aligned value handles");
}

void CurveModeButtons() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X", {
        MakeKey(0, 0, ac::Interpolation::Linear, ac::Tangent::Linear),
        MakeKey(20, 5, ac::Interpolation::Linear, ac::Tangent::Linear),
        MakeKey(40, 10, ac::Interpolation::Linear, ac::Tangent::Linear)}));
    HWND window = static_cast<HWND>(ce::Open(nullptr, MakeSession(host), false));
    ClickKey(window, 1);
    const auto choose = [&](int id, int index) {
        HWND control = GetDlgItem(window, id);
        RECT rect{}; GetClientRect(control, &rect);
        const int count = int(SendMessageW(control, CB_GETCOUNT, 0, 0));
        const int gap = id == kIdTangentCombo ? 6 : 0;
        const int x = (rect.right-gap)*(2*index+1)/(2*count) +
                      (id == kIdTangentCombo && index >= 3 ? gap : 0);
        SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, rect.bottom/2));
        SendMessageW(control, WM_LBUTTONUP, 0, MAKELPARAM(x, rect.bottom/2));
    };
    choose(kIdTangentCombo, 3); // Flat preset, after Free / Aligned / Automatic.
    Check(host.applyCalls == 1, "clicking a tangent preset commits immediately without Apply");
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Bezier &&
          host.data[0].track.keys[2].interpolation == ac::Interpolation::Bezier,
          "flat handles activate Bezier on both former linear intervals");
    Check(std::fabs(ac::Evaluate(host.data[0].track, 10).value - 2.5) > 0.1,
          "the tangent preset changes the evaluated animation, not only the button highlight");
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Free,
          "a one-shot flat preset leaves editable free handles");
    const auto flat = host.data[0].track;
    Click(window, kIdUndo);
    Check(ac::Evaluate(host.data[0].track, 10).value == 2.5, "undo restores the original linear interval");
    int unchangedApplies = host.applyCalls;
    choose(kIdInterpolationCombo, 1);
    Check(host.applyCalls == unchangedApplies,
          "reselecting Linear writes nothing");
    Click(window, kIdUndo);
    Check(host.applyCalls == unchangedApplies,
          "an unchanged interpolation choice does not append an undo entry");
    Click(window, kIdRedo);
    Check(host.applyCalls == unchangedApplies + 1 &&
          ac::Evaluate(host.data[0].track, 10).value == ac::Evaluate(flat, 10).value,
          "redo restores the evaluated preset after an unchanged interpolation choice");
    unchangedApplies = host.applyCalls;
    choose(kIdTangentCombo, 3);
    Check(host.applyCalls == unchangedApplies,
          "reapplying the same flat preset by mouse does not write unchanged curves");
    SetText(window, kIdValueEdit, L"999");
    SendMessageW(GetDlgItem(window, kIdTangentCombo), WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(GetDlgItem(window, kIdTangentCombo), WM_CHAR, L'\r', 0);
    Check(host.applyCalls == unchangedApplies && host.data[0].track.keys[1].value == 5,
          "Enter on the unchanged preset neither writes curves nor applies unrelated numeric text");

    choose(kIdInterpolationCombo, 0);
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Step &&
          ac::Evaluate(host.data[0].track, 10).value == 0,
          "step interpolation applies immediately to the interval ending at the selected key");
    Check(host.data[0].track.keys[2].interpolation == ac::Interpolation::Bezier,
          "interpolation changes only the incoming interval");
    choose(kIdInterpolationCombo, 1);
    Check(ac::Evaluate(host.data[0].track, 10).value == 2.5,
          "linear interpolation uses the chord even with flat tangent slopes");
    choose(kIdInterpolationCombo, 2);
    Check(ac::Evaluate(host.data[0].track, 10).value == ac::Evaluate(flat, 10).value,
          "Bezier interpolation restores the stored curved handles");

    choose(kIdTangentCombo, 1);
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Aligned, "aligned is a persistent handle mode");
    SetText(window, kIdOutSlopeEdit, L"0.2"); Click(window, kIdApplyKey);
    Check(host.data[0].track.keys[1].inSlope == 0.2 && host.data[0].track.keys[1].outSlope == 0.2,
          "numeric editing preserves the chosen aligned slope instead of averaging with the old field");
    choose(kIdTangentCombo, 2);
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Automatic,
          "automatic mode is applied immediately");
    SetText(window, kIdOutSlopeEdit, L"0.1"); Click(window, kIdApplyKey);
    Check(host.data[0].track.keys[1].tangent == ac::Tangent::Free &&
          host.data[0].track.keys[1].outSlope == 0.1,
          "manual slope input leaves automatic mode and keeps the typed slope");
    choose(kIdTangentCombo, 4);
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Linear &&
          host.data[0].track.keys[2].interpolation == ac::Interpolation::Linear,
          "straighten preset makes both adjacent intervals linear");
    choose(kIdTangentCombo, 5);
    Check(host.data[0].track.keys[1].interpolation == ac::Interpolation::Bezier &&
          host.data[0].track.keys[1].inSlope == 0 && ac::SideIsStraight(host.data[0].track,1,true),
          "ease in bends only the incoming side");
    choose(kIdTangentCombo, 6);
    Check(host.data[0].track.keys[1].outSlope == 0 && ac::SideIsStraight(host.data[0].track,1,false),
          "ease out bends only the outgoing side");
    std::vector<std::uint8_t> bytes; std::vector<ac::NamedTrack> decoded; std::string error;
    Check(ac::Encode({{"pos.x",host.data[0].track}}, bytes, error) &&
          ac::Decode(bytes.data(),bytes.size(),decoded,error), "edited modes survive the existing curve codec");
    for (int frame=0;frame<=40;++frame)
        Check(ac::Evaluate(decoded[0].track,frame).value == ac::Evaluate(host.data[0].track,frame).value,
              "curve serialization preserves the evaluated animation");
    host.failApply = true;
    const auto before = host.data[0].track;
    choose(kIdInterpolationCombo, 0);
    Check(host.data[0].track.keys[1].interpolation == before.keys[1].interpolation &&
          SendMessageW(GetDlgItem(window,kIdInterpolationCombo),CB_GETCURSEL,0,0) ==
              static_cast<int>(before.keys[1].interpolation),
          "a rejected mode write restores both data and button state");
    host.failApply = false;
    ce::CloseOwned(nullptr);
    window = static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    ClickKey(window,1);
    Check(SendMessageW(GetDlgItem(window,kIdTangentCombo),CB_GETCURSEL,0,0)==0,
          "reopening shows free mode, not a sticky one-shot preset");
    ClickKey(window,0);
    Check(!IsWindowEnabled(GetDlgItem(window,kIdInterpolationCombo)),
          "the first key has no incoming interval to edit");
    ce::CloseOwned(nullptr);
    host.data[0].track.discrete=true;
    window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false)); ClickKey(window,1);
    Check(!IsWindowEnabled(GetDlgItem(window,kIdTangentCombo)) &&
          !IsWindowEnabled(GetDlgItem(window,kIdInterpolationCombo)),
          "discrete channels disable unsupported interpolation and tangent operations");
    ce::CloseOwned(nullptr);
    Note("immediate curve modes and one-shot tangent presets, evaluated samples, history and serialization");

    // Endpoint keys have only one handle; multi-selection is one transaction.
    for (unsigned language=0; language<3; ++language) {
        host = HostSource{};
        host.data.push_back(MakeChannel("pos.x", L"Position X", {
            MakeKey(0,0,ac::Interpolation::Linear,ac::Tangent::Linear),
            MakeKey(20,5,ac::Interpolation::Linear,ac::Tangent::Linear),
            MakeKey(40,20,ac::Interpolation::Linear,ac::Tangent::Linear)}));
        window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host,language),false));
        Check(!IsWindowEnabled(GetDlgItem(window,kIdTangentCombo)), "handle controls require a selected key");
        ClickKey(window,2); choose(kIdTangentCombo,3);
        Check(host.data[0].track.keys[2].inSlope==0 &&
              std::fabs(ac::Evaluate(host.data[0].track,30).value-12.5)>0.1,
              "flat preset works on the final key's incoming interval");
        ClickKey(window,0); choose(kIdTangentCombo,3);
        Check(host.data[0].track.keys[0].outSlope==0 &&
              std::fabs(ac::Evaluate(host.data[0].track,10).value-2.5)>0.1,
              "flat preset works on the first key's outgoing interval");
        // A valid single-key selection now fits that key only; the initial
        // all-track Open fit already keeps every key on screen for the box.
        const POINT a=AskPoint(window,kHookPointForKey,1), b=AskPoint(window,kHookPointForKey,2);
        HWND canvas=GetDlgItem(window,kIdCanvas);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x-20,std::min(a.y,b.y)-20));
        SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x+20,std::max(a.y,b.y)+20));
        SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(b.x+20,std::max(a.y,b.y)+20));
        int applies=host.applyCalls;
        choose(kIdInterpolationCombo,0);
        Check(host.applyCalls==applies+1 && host.data[0].track.keys[1].interpolation==ac::Interpolation::Step &&
              host.data[0].track.keys[2].interpolation==ac::Interpolation::Step,
              "multi-selection changes both incoming intervals in one transaction");
        Check(ac::Evaluate(host.data[0].track,10).value==0 && ac::Evaluate(host.data[0].track,30).value==5,
              "both selected step intervals evaluate correctly");
        Click(window,kIdUndo);
        Check(host.data[0].track.keys[1].interpolation==ac::Interpolation::Bezier &&
              host.data[0].track.keys[2].interpolation==ac::Interpolation::Bezier,
              "one undo restores the complete multi-selection operation");
        applies=host.applyCalls;
        choose(kIdInterpolationCombo,2);
        Check(host.applyCalls==applies,
              "unchanged multi-selection interpolation makes no host write");
        Click(window,kIdRedo);
        Check(host.applyCalls==applies+1 && host.data[0].track.keys[1].interpolation==ac::Interpolation::Step &&
              host.data[0].track.keys[2].interpolation==ac::Interpolation::Step,
              "unchanged multi-selection interpolation preserves the complete redo operation");
        Click(window,kIdUndo);
        choose(kIdTangentCombo,2);
        Check(host.data[0].track.keys[1].tangent==ac::Tangent::Automatic &&
              host.data[0].track.keys[2].tangent==ac::Tangent::Automatic,
              "automatic mode applies to every selected key");
        ClickKey(window,0); ClickKey(window,2);
        const double oldSlope=host.data[0].track.keys[1].outSlope;
        SetText(window,kIdValueEdit,L"30"); Click(window,kIdApplyKey);
        Check(host.data[0].track.keys[1].outSlope!=oldSlope,
              "automatic slopes follow changes to a neighbouring key");
        HWND tangent=GetDlgItem(window,kIdTangentCombo);
        SendMessageW(tangent,WM_KEYDOWN,VK_HOME,0); // Free mode.
        Check(host.data[0].track.keys[2].tangent==ac::Tangent::Free,
              "keyboard selection applies free mode");
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Aligned.
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Automatic.
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Flat preset; highlight returns to Free.
        SendMessageW(tangent,WM_KEYDOWN,VK_RIGHT,0); // Straighten, not Aligned.
        Check(host.data[0].track.keys[2].interpolation==ac::Interpolation::Linear,
              "keyboard focus continues across presets after their mode highlight resets");
        ClickKey(window,1); HoverControl(window,kIdTangentCombo);
        Check(HoverHint(window)==12, "the tangent strip explains mode and preset semantics");
        HoverControl(window,kIdInterpolationCombo);
        Check(HoverHint(window)==11, "the interpolation strip explains incoming interval semantics");
        ce::CloseOwned(nullptr);
    }
    Note("endpoint keys, multi-selection, neighbour updates and keyboard presets in all three languages");

    host=HostSource{};
    host.data.push_back(MakeChannel("pos.x",L"Position X",{
        MakeKey(0,0,ac::Interpolation::Linear,ac::Tangent::Free),
        MakeKey(20,5,ac::Interpolation::Linear,ac::Tangent::Free),
        MakeKey(40,10,ac::Interpolation::Step,ac::Tangent::Free)}));
    window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    ClickKey(window,1);ClickKeyAdditive(window,2);
    Check(SendMessageW(GetDlgItem(window,kIdInterpolationCombo),CB_GETCURSEL,0,0)==-1,
          "mixed interpolation selection has no misleading active button");
    choose(kIdInterpolationCombo,1);
    Check(host.applyCalls==1 && host.data[0].track.keys[1].interpolation==ac::Interpolation::Linear &&
          host.data[0].track.keys[2].interpolation==ac::Interpolation::Linear,
          "choosing a mode already used by one selected key still updates the other key once");
    Click(window,kIdUndo);
    Check(host.data[0].track.keys[1].interpolation==ac::Interpolation::Linear &&
          host.data[0].track.keys[2].interpolation==ac::Interpolation::Step,
          "one undo restores a mixed interpolation selection");
    ce::CloseOwned(nullptr);
}

}  // namespace

#include "curve_native_editor_checks.inc"
#include "animation_curve_external_history_checks.inc"

namespace {
struct WriteLifetimeFixture {
    // Commit, local undo, local redo, external commit, external undo/redo.
    int route;
    HostSource local,replacement;
    ExternalCurveJournal journal;
    explicit WriteLifetimeFixture(int value):route(value){
        local.data.push_back(MakeChannel("x",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        replacement.data.push_back(MakeChannel("x",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
    }
    ce::Session Session(){return route<3 ? MakeSession(local) : journal.Session();}
    void Seed(HWND window){
        ClickKey(window,1);
        if(route==1||route==2||route==4||route==5)Click(window,kIdDeleteKey);
        if(route==2||route==5)Click(window,kIdUndo);
    }
    int Command()const{return route==0||route==3 ? kIdDeleteKey : route==1||route==4 ? kIdUndo : kIdRedo;}
    const std::vector<ce::Channel>& Data()const{return route<3 ? local.data : journal.sources[0].data;}
    unsigned Applies()const{return route<3 ? unsigned(local.applyCalls) : journal.applies;}
    unsigned Traversals()const{return journal.traversals;}
    int Reads()const{return route<3 ? local.readCalls : journal.sources[0].readCalls;}
    unsigned Queries()const{return journal.queries;}
};

bool SameWritePayload(const std::vector<ce::Channel>& first,const std::vector<ce::Channel>& second){
    if(first.size()!=second.size())return false;
    for(std::size_t c=0;c<first.size();++c){
        const auto& a=first[c];const auto& b=second[c];
        if(a.id!=b.id||a.label!=b.label||a.group!=b.group||a.selected!=b.selected||
            a.track.discrete!=b.track.discrete||a.track.keys.size()!=b.track.keys.size()||
            a.nativeControls!=b.nativeControls||a.controlGeometry!=b.controlGeometry)return false;
        for(std::size_t k=0;k<a.track.keys.size();++k){
            const auto& x=a.track.keys[k];const auto& y=b.track.keys[k];
            if(x.frame!=y.frame||x.value!=y.value||x.inSlope!=y.inSlope||x.outSlope!=y.outSlope||
                x.inWeight!=y.inWeight||x.outWeight!=y.outWeight||
                x.interpolation!=y.interpolation||x.tangent!=y.tangent)return false;
        }
    }
    return true;
}

struct WriteRetirementState {
    int phase=0,response=0; // Copy, invocation, callable-copy destruction.
    bool armed=false,copying=false;
    const void* copyingOriginal=nullptr;
    int calls=0,retireCalls=0,destroyedDuringCopy=0;
    const std::vector<ce::Channel>* input=nullptr;
    const ce::Channel* channelStorage=nullptr;
    const ac::Key* keyStorage=nullptr;
    std::vector<ce::Channel> inputSnapshot;
    std::wstring* errorOutput=nullptr;
    decltype(ce::Source::apply) apply;
    decltype(ce::ExternalHistory::traverse) traverse;
    std::function<void()> retire;
    void CheckArguments(){
        if(input)Check(input->data()==channelStorage&&!input->empty()&&
            (*input)[0].track.keys.data()==keyStorage&&SameWritePayload(*input,inputSnapshot),
            "apply keeps detached channels, names, key storage and values through provider retirement");
        if(errorOutput){*errorOutput=L"retired write callback";
            Check(*errorOutput==L"retired write callback","write error output remains valid through retirement");}
    }
    void Retire(){armed=false;++retireCalls;retire();CheckArguments();}
    bool Result(){if(response==2)throw std::runtime_error("retired write callback");return response==0;}
};

struct WriteRetirementProvider {
    WriteRetirementState* state;
    int marker=37;
    mutable bool invoked=false;
    explicit WriteRetirementProvider(WriteRetirementState& value):state(&value){}
    WriteRetirementProvider(const WriteRetirementProvider& other):state(other.state),marker(other.marker){
        if(!state->armed||state->phase!=0)return;
        state->copying=true;state->copyingOriginal=&other;state->Retire();
        Check(state->destroyedDuringCopy==0&&other.marker==37,
            "write provider's original callable stays alive throughout a reentrant copy");
        state->copying=false;
        if(state->response==2)throw std::runtime_error("retired write provider copy");
    }
    ~WriteRetirementProvider(){
        if(state->copying&&state->copyingOriginal==this)++state->destroyedDuringCopy;
        if(invoked&&state->armed&&state->phase==2)state->Retire();
    }
    bool operator()(const std::vector<ce::Channel>& next,std::wstring& error)const{
        invoked=true;++state->calls;
        if(!state->armed)return state->apply(next,error);
        state->input=&next;state->channelStorage=next.data();
        state->keyStorage=next.empty()?nullptr:next[0].track.keys.data();
        state->inputSnapshot=next;state->errorOutput=&error;
        if(state->response==0)Check(state->apply(next,error),"write fixture publishes its accepted edit");
        if(state->phase==1)state->Retire();
        return state->Result();
    }
    bool operator()(bool redo,std::wstring& error)const{
        invoked=true;++state->calls;
        if(!state->armed)return state->traverse(redo,error);
        state->errorOutput=&error;
        if(state->response==0)Check(state->traverse(redo,error),"write fixture commits its accepted traversal");
        if(state->phase==1)state->Retire();
        return state->Result();
    }
};

void WriteCallbackLifetime(){
    for(int route=0;route!=6;++route)for(int phase=0;phase!=3;++phase)
    for(int action=0;action!=3;++action)for(int response=0;response!=3;++response){
        std::printf("write lifetime route=%d phase=%d action=%d response=%d\n",route,phase,action,response);
        WriteLifetimeFixture fixture(route);
        WriteRetirementState state;state.phase=phase;state.response=response;
        HWND replacementWindow=nullptr;
        std::wstring replacementStatus;
        int readsAfterRetirement=-1;unsigned queriesAfterRetirement=0;
        state.retire=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(fixture.replacement),false));
                Check(IsWindow(replacementWindow),"write retirement creates a live replacement session");
                replacementStatus=GetText(replacementWindow,kIdStatus);
            }
            readsAfterRetirement=fixture.Reads();queriesAfterRetirement=fixture.Queries();
        };
        auto session=fixture.Session();
        if(route<4){state.apply=session.sources[0].apply;session.sources[0].apply=WriteRetirementProvider(state);}
        else{state.traverse=session.history.traverse;session.history.traverse=WriteRetirementProvider(state);}
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"write lifetime fixture opens unarmed");
        fixture.Seed(window);
        const auto applies=fixture.Applies(),traversals=fixture.Traversals();
        const auto keys=fixture.Data()[0].track.keys.size(),cursor=fixture.journal.cursor;
        const auto calls=state.calls;
        auto token=ce::CaptureWindowSession(window);auto retained=token.lock();
        state.armed=true;Click(window,fixture.Command());
        const bool committed=phase!=0&&response==0;
        Check(state.retireCalls==1&&state.destroyedDuringCopy==0&&
            state.calls==calls+(phase!=0),"write retirement stops the exact old invocation without another callback");
        Check(fixture.Applies()==applies+unsigned(committed&&route<4)&&
            fixture.Traversals()==traversals+unsigned(committed&&route>=4),
            "retired write preserves one completed publication and never repeats or reverses it");
        Check(fixture.Data()[0].track.keys.size()==(committed ? std::size_t(route==1||route==4?2:1) : keys),
            "host data keeps the accepted write or the unchanged rejected baseline");
        const auto expectedCursor=committed&&route>=3 ? (route==4?cursor-1:cursor+1) : cursor;
        Check(fixture.journal.cursor==expectedCursor&&fixture.Reads()==readsAfterRetirement&&
            fixture.Queries()==queriesAfterRetirement,"old write performs no canonical read or history query after retirement");
        Check(bool(retained)&&!ce::IsWindowSessionCurrent(window,token),
            "write retirement invalidates registration while the old token is retained");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&GetText(replacementWindow,kIdStatus)==replacementStatus&&
                KeyCount(replacementWindow)==2&&fixture.replacement.applyCalls==0,
                "old commit, snapshot and traversal continuations cannot overwrite replacement state");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(fixture.replacement.applyCalls==1&&KeyCount(replacementWindow)==1,
                "replacement stays editable after the retired write guards finish");
        }else Check(OwnedCurveWindow()==nullptr,"closed write session leaves no registered editor");
        ce::CloseOwned(nullptr);
    }
    Note("apply and traversal copy, invocation and final release preserve session and publication lifetime");
}

void WritePostCommitReadRetirement(){
    for(int route=0;route!=6;++route)for(int action=0;action!=3;++action){
        WriteLifetimeFixture fixture(route);
        bool armed=false,readPending=false;
        int retired=0;
        HWND replacementWindow=nullptr;
        auto session=fixture.Session();
        const auto read=session.sources[0].read;
        session.sources[0].read=[&,read](auto& next,auto& error){
            const bool accepted=read(next,error);
            if(readPending){
                readPending=false;++retired;
                if(action!=1)ce::CloseOwned(nullptr);
                if(action!=0)replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(fixture.replacement),false));
            }
            return accepted;
        };
        if(route<4){
            const auto apply=session.sources[0].apply;
            session.sources[0].apply=[&,apply](const auto& next,auto& error){
                const bool accepted=apply(next,error);if(armed&&accepted)readPending=true;return accepted;
            };
        }else{
            const auto traverse=session.history.traverse;
            session.history.traverse=[&,traverse](bool redo,auto& error){
                const bool accepted=traverse(redo,error);if(armed&&accepted)readPending=true;return accepted;
            };
        }
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"post-write canonical read fixture opens");fixture.Seed(window);
        const auto applies=fixture.Applies(),traversals=fixture.Traversals();
        armed=true;Click(window,fixture.Command());
        Check(retired==1&&fixture.Applies()==applies+unsigned(route<4)&&
            fixture.Traversals()==traversals+unsigned(route>=4),
            "retirement in the canonical read never reverses or repeats the completed write");
        Check(fixture.Data()[0].track.keys.size()==std::size_t(route==1||route==4?2:1),
            "accepted host data survives canonical-read retirement during commit, undo and redo");
        if(replacementWindow){
            Check(IsWindow(replacementWindow)&&KeyCount(replacementWindow)==2,
                "post-write retirement leaves replacement channels intact");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(fixture.replacement.applyCalls==1,"replacement remains editable after post-write canonical retirement");
        }else Check(OwnedCurveWindow()==nullptr,"post-write canonical retirement removes its closed editor");
        ce::CloseOwned(nullptr);
    }
    Note("post-publication canonical retirement does not retry apply or traverse");
}

std::array<std::uint64_t,2> LocalWriteHistory(HWND window){
    std::uint64_t state[5]{};
    Check(SendMessageW(window,WM_APP+397,0,reinterpret_cast<LPARAM>(state))!=0,
        "local write exposes its retained history counts");
    return {state[0],state[1]};
}

struct WriteFailureState {
    decltype(ce::Source::apply) apply;
    int kind=0,failures=0;
    bool armed=false;
};
struct WriteFailureProvider {
    WriteFailureState* state;
    explicit WriteFailureProvider(WriteFailureState& value):state(&value){}
    WriteFailureProvider(const WriteFailureProvider& other):state(other.state){
        if(state->armed&&state->kind==0){state->armed=false;++state->failures;throw std::bad_alloc();}
    }
    bool operator()(const std::vector<ce::Channel>& channels,std::wstring& error)const{
        if(!state->armed)return state->apply(channels,error);
        state->armed=false;++state->failures;
        if(state->kind==1){error=L"write rejected";return false;}
        if(state->kind==2)throw std::runtime_error("write failed");
        throw std::bad_alloc();
    }
};

void WriteFailureKeepsLocalHistory(){
    for(int route=0;route!=3;++route)for(int kind=0;kind!=4;++kind){
        WriteLifetimeFixture fixture(route);WriteFailureState state;state.kind=kind;
        auto session=fixture.Session();state.apply=session.sources[0].apply;
        session.sources[0].apply=WriteFailureProvider(state);
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"failed local write fixture opens");fixture.Seed(window);
        // New authoring failure must preserve an already available redo too.
        if(route==0){Click(window,kIdDeleteKey);Click(window,kIdUndo);}
        const auto before=fixture.local.data;const auto history=LocalWriteHistory(window);
        const int applies=fixture.local.applyCalls;state.armed=true;
        Click(window,fixture.Command());
        Check(state.failures==1&&fixture.local.applyCalls==applies&&
            SameWritePayload(fixture.local.data,before)&&LocalWriteHistory(window)==history,
            "copy allocation failure, rejection and exceptions keep both local history stacks and host data");
        Check(KeyCount(window)==int(before[0].track.keys.size())&&
            IsWindowEnabled(GetDlgItem(window,fixture.Command())),
            "failed local replay restores visible channels and keeps the original operation retryable");
        Click(window,fixture.Command());
        Check(fixture.local.applyCalls==applies+1&&KeyCount(window)==(route==1?2:1),
            "retry publishes the retained local commit, undo or redo exactly once");
        const auto after=LocalWriteHistory(window);
        Check(after[0]==(route==1?history[0]-1:history[0]+1)&&
            after[1]==(route==0?0:route==1?history[1]+1:history[1]-1),
            "a successful retry transfers only its own retained node");
        ce::CloseOwned(nullptr);
    }
    Note("local history survives provider-copy allocation failure, rejection and callback exceptions");
}

void WriteCanonicalFailureKeepsPublication(){
    for(int route=0;route!=3;++route){
        WriteLifetimeFixture fixture(route);bool armed=false;
        auto session=fixture.Session();const auto apply=session.sources[0].apply;
        session.sources[0].apply=[&,apply](const auto& next,auto& error){
            const bool accepted=apply(next,error);
            if(armed&&accepted)fixture.local.failRead=true;
            return accepted;
        };
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"canonical failure fixture opens");fixture.Seed(window);
        const auto history=LocalWriteHistory(window);const int applies=fixture.local.applyCalls;
        armed=true;Click(window,fixture.Command());
        const auto accepted=LocalWriteHistory(window);
        Check(fixture.local.applyCalls==applies+1&&KeyCount(window)==(route==1?2:1)&&
            !IsWindowEnabled(GetDlgItem(window,kIdAddKey)),
            "post-commit read failure preserves the accepted fallback and suspends further authoring");
        Check(accepted[0]==(route==1?history[0]-1:history[0]+1)&&
            accepted[1]==(route==0?0:route==1?history[1]+1:history[1]-1),
            "accepted local history advances once before canonical read failure");
        armed=false;fixture.local.failRead=false;
        ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(fixture.local.applyCalls==applies+1&&LocalWriteHistory(window)==accepted&&
            IsWindowEnabled(GetDlgItem(window,kIdAddKey)),
            "canonical retry recovers the accepted state without applying or transferring history again");
        Click(window,route==1?kIdRedo:kIdUndo);
        Check(fixture.local.applyCalls==applies+2,
            "the opposite local traversal remains usable after canonical recovery");
        ce::CloseOwned(nullptr);
    }
    Note("accepted local publications retain their fallback and history through canonical read failure");
}

void WriteAcceptedAllocationFailure(){
    for(int route=0;route!=3;++route){
        WriteLifetimeFixture fixture(route);bool armed=false;
        auto session=fixture.Session();const auto apply=session.sources[0].apply;
        session.sources[0].apply=[&,apply](const auto& next,auto& error){
            const bool accepted=apply(next,error);
            if(armed&&accepted)g_curveFailNextAllocation=true;
            return accepted;
        };
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"post-acceptance allocator fixture opens");fixture.Seed(window);
        const auto history=LocalWriteHistory(window);const int applies=fixture.local.applyCalls;
        const unsigned failures=g_curveAllocationFailures;armed=true;Click(window,fixture.Command());
        Check(!g_curveFailNextAllocation&&g_curveAllocationFailures==failures+1,
            "the first actual C++ allocation after host acceptance is injected once");
        const auto accepted=LocalWriteHistory(window);
        Check(fixture.local.applyCalls==applies+1&&KeyCount(window)==(route==1?2:1)&&
            accepted[0]==(route==1?history[0]-1:history[0]+1)&&
            accepted[1]==(route==0?0:route==1?history[1]+1:history[1]-1),
            "allocation failure after acceptance cannot lose the completed edit or its transferred history node");
        armed=false;ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(fixture.local.applyCalls==applies+1&&LocalWriteHistory(window)==accepted,
            "recovering from post-acceptance allocation failure never republishes or duplicates history");
        Click(window,route==1?kIdRedo:kIdUndo);
        Check(fixture.local.applyCalls==applies+2,
            "post-allocation-failure history still supports its opposite traversal");
        ce::CloseOwned(nullptr);
    }
    Note("real allocation failure after an accepted write cannot drop retained local history");
}

struct NestedWriteState {
    decltype(ce::Source::apply) apply;
    std::function<void()> pump;
    int phase=0,pumps=0;
    bool armed=false,written=false;
    void Pump(){armed=false;++pumps;pump();}
};
struct NestedWriteProvider {
    NestedWriteState* state;
    mutable bool invoked=false;
    explicit NestedWriteProvider(NestedWriteState& value):state(&value){}
    NestedWriteProvider(const NestedWriteProvider& other):state(other.state){
        if(state->armed&&state->phase==1)state->Pump();
    }
    ~NestedWriteProvider(){if(invoked&&state->armed&&state->phase==3)state->Pump();}
    bool operator()(const std::vector<ce::Channel>& next,std::wstring& error)const{
        invoked=true;
        if(state->armed&&state->phase==2)state->Pump();
        const bool result=state->apply(next,error);if(result)state->written=true;return result;
    }
};

void WriteRejectsNestedAuthoring(){
    for(int route=0;route!=3;++route)for(int phase=0;phase!=5;++phase){
        WriteLifetimeFixture fixture(route);NestedWriteState state;state.phase=phase;
        HWND window=nullptr;bool pumping=false;int nestedSelectionWrites=0;
        auto session=fixture.Session();state.apply=session.sources[0].apply;
        session.sources[0].apply=NestedWriteProvider(state);
        const auto read=session.sources[0].read;
        session.sources[0].read=[&,read](auto& next,auto& error){
            const bool result=read(next,error);
            if(state.armed&&(phase==0||(phase==4&&state.written)))state.Pump();
            return result;
        };
        session.sources[0].writeKeySelection=[&](const auto&,const auto&){if(pumping)++nestedSelectionWrites;};
        state.pump=[&]{
            pumping=true;const int applies=fixture.local.applyCalls,reads=fixture.local.readCalls;
            for(int id:{kIdDeleteKey,kIdAddKey,kIdUndo,kIdRedo,kIdApplyKey})Click(window,id);
            HWND canvas=GetDlgItem(window,kIdCanvas),panel=GetDlgItem(window,kIdMmdPanel);
            for(UINT message:{WM_LBUTTONDOWN,WM_MOUSEMOVE,WM_LBUTTONUP,WM_LBUTTONDBLCLK})
                SendMessageW(canvas,message,MK_LBUTTON,MAKELPARAM(80,80));
            for(UINT message:{WM_LBUTTONDOWN,WM_MOUSEMOVE,WM_LBUTTONUP})
                SendMessageW(panel,message,MK_LBUTTON,MAKELPARAM(40,40));
            SendMessageW(GetDlgItem(window,kIdFrameEdit),WM_KEYDOWN,VK_RETURN,0);
            SendMessageW(GetDlgItem(window,kIdChannelList),WM_KEYDOWN,VK_SPACE,0);
            ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
            Check(fixture.local.applyCalls==applies&&fixture.local.readCalls==reads&&nestedSelectionWrites==0,
                "busy copy, write, release and canonical read reject nested commands, drags and selection publication");
            pumping=false;
        };
        window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"same-session nested authoring fixture opens");fixture.Seed(window);
        const int applies=fixture.local.applyCalls;state.written=false;state.armed=true;
        Click(window,fixture.Command());
        Check(state.pumps==1&&fixture.local.applyCalls==applies+1&&KeyCount(window)==(route==1?2:1),
            "one local commit or traversal survives nested UI input with exactly one publication");
        SendMessageW(window,WM_APP+401,0,0);
        Check(fixture.local.applyCalls==applies+1&&IsWindowEnabled(GetDlgItem(window,kIdAddKey)),
            "deferred source notification reads the completed result without another write");
        ce::CloseOwned(nullptr);
    }
    Note("local preflight, provider copy, write, final release and readback reject recursive authoring");
}

struct SelectionLifetimeFixture {
    HostSource local,replacement;
    ExternalCurveJournal journal;
    bool external;
    explicit SelectionLifetimeFixture(bool value):external(value){
        local.data.push_back(MakeChannel("selection.original.channel.with.owned.name",L"Original",{MakeKey(0,1),MakeKey(10,5)}));
        journal.sources[0].data=local.data;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
    }
    ce::Session Session(){return external?journal.Session():MakeSession(local);}
    int Applies()const{return external?int(journal.applies):local.applyCalls;}
    int Reads()const{return external?journal.sources[0].readCalls:local.readCalls;}
};

struct SelectionRetirementState {
    int phase=0,response=0,calls=0,writes=0,retireCalls=0,destroyedDuringCopy=0;
    bool armed=false,copying=false;
    const void* copyingOriginal=nullptr;
    const std::string* inputId=nullptr;
    const char* inputIdStorage=nullptr;
    const std::vector<int>* inputFrames=nullptr;
    const int* inputFrameStorage=nullptr;
    std::string expectedId;
    std::vector<int> expectedFrames,hostFrames{10};
    std::function<void()> retire;
    void CheckArguments(){
        if(inputId)Check(*inputId==expectedId&&inputId->data()==inputIdStorage,
            "selection provider keeps its privately owned channel name through window retirement");
        if(inputFrames)Check(*inputFrames==expectedFrames&&inputFrames->data()==inputFrameStorage,
            "selection writer keeps its privately owned frame addresses through window retirement");
    }
    void Trace(const char* stage)const{
        std::fprintf(stderr,"SELECTION_STAGE %s phase=%d response=%d calls=%d writes=%d retires=%d armed=%d\n",
            stage,phase,response,calls,writes,retireCalls,int(armed));
        std::fflush(stderr);
    }
    void Retire(){armed=false;++retireCalls;Trace("retire begin");retire();Trace("retire returned");CheckArguments();}
};
struct SelectionRetirementProvider {
    SelectionRetirementState* state;
    int marker=73;
    mutable bool invoked=false;
    explicit SelectionRetirementProvider(SelectionRetirementState& value):state(&value){}
    SelectionRetirementProvider(const SelectionRetirementProvider& other):state(other.state),marker(other.marker){
        if(!state->armed||state->phase!=0)return;
        state->copying=true;state->copyingOriginal=&other;state->Retire();
        Check(state->destroyedDuringCopy==0&&other.marker==73,
            "selection provider storage remains alive throughout a reentrant callable copy");
        state->copying=false;if(state->response==2)throw std::bad_alloc();
    }
    ~SelectionRetirementProvider(){
        if(state->copying&&state->copyingOriginal==this)++state->destroyedDuringCopy;
        if(invoked&&state->armed&&state->phase==2)state->Retire();
    }
    void Arguments(const std::string& id)const{
        state->inputId=&id;state->inputIdStorage=id.data();state->expectedId=id;
    }
    std::vector<int> operator()(const std::string& id)const{
        invoked=true;++state->calls;
        if(!state->armed)return state->hostFrames;
        Arguments(id);
        if(state->phase==1)state->Retire();
        if(state->response==2)throw std::runtime_error("selection read retired");
        return state->response==1?std::vector<int>{}:state->hostFrames;
    }
    void operator()(const std::string& id,const std::vector<int>& frames)const{
        invoked=true;++state->calls;
        if(!state->armed){state->hostFrames=frames;++state->writes;return;}
        Arguments(id);state->inputFrames=&frames;state->inputFrameStorage=frames.data();state->expectedFrames=frames;
        if(state->response==0){state->hostFrames=frames;++state->writes;}
        if(state->phase==1)state->Retire();
        if(state->response==1)throw std::runtime_error("selection write retired");
        if(state->response==2)throw std::bad_alloc();
    }
};

void TriggerSelectionRead(HWND window,int entry){
    if(entry==1){ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);}
    if(entry==2){SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);}
    if(entry==3)Click(window,kIdDeleteKey);
    if(entry==4)SendMessageW(GetDlgItem(window,kIdCanvas),WM_KEYDOWN,VK_DELETE,0);
    if(entry==5)SendMessageW(GetDlgItem(window,kIdDeleteKey),WM_KEYDOWN,VK_DELETE,0);
    if(entry==6)SendMessageW(GetDlgItem(window,kIdValueEdit),WM_KEYDOWN,VK_RETURN,0);
    if(entry==7){HWND tree=GetDlgItem(window,kIdChannelList);TreeView_SelectItem(tree,ChannelItem(window,0));
        SendMessageW(tree,WM_KEYDOWN,VK_RETURN,0);}
}
void TriggerSelectionWrite(HWND window,int entry){
    if(entry==0)ClickKey(window,1);
    if(entry==1){const auto point=AskPoint(window,kHookPointForKey,1);
        SendMessageW(GetDlgItem(window,kIdCanvas),WM_LBUTTONDOWN,MK_LBUTTON|MK_CONTROL,MAKELPARAM(point.x,point.y));}
    if(entry==2)Click(window,kIdUndo);
    if(entry==3||entry==6)Click(window,kIdApplyKey);
    if(entry==4){KeyboardModifiers keys;keys.Set(true);
        SendMessageW(GetDlgItem(window,kIdFitAll),WM_KEYDOWN,'Y',0);}
    if(entry==5){const auto point=AskPoint(window,kHookPointForKey,1);CurveEditorAxisState axis{};
        Check(SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis))!=0&&PtInRect(&axis.plot,point),
            "selection doubleclick targets an existing interior key, inside the curve hit-test rectangle");
        SendMessageW(GetDlgItem(window,kIdCanvas),WM_LBUTTONDBLCLK,0,MAKELPARAM(point.x,point.y));}
}

void SelectionCallbackLifetime(){
    // Read entries: initial external/local creation, local posted refresh,
    // visible timer, parent/button/canvas authoring, value EDIT and tree activation.
    // Write entries: canvas click/deselect, undo/redo, existing-key doubleclick,
    // and owned local/external publication.
    for(bool writing:{false,true})for(int entry=0;entry!=(writing?7:9);++entry)
    for(int phase=0;phase!=3;++phase)for(int action=0;action!=3;++action)for(int response=0;response!=3;++response){
        std::printf("selection lifetime write=%d entry=%d phase=%d action=%d response=%d\n",writing,entry,phase,action,response);
        std::fflush(stdout);
        const bool initial=!writing&&(entry==0||entry==8);
        SelectionLifetimeFixture fixture((!writing&&entry==0)||(writing&&entry==6));SelectionRetirementState state;
        // FitAll places the last key on plot.right, excluded by PtInRect in
        // the doubleclick route. Keep the target key inside the real plot.
        if(writing&&entry==5)fixture.local.data[0].track.keys.push_back(MakeKey(20,9));
        state.phase=phase;state.response=response;
        HWND replacementWindow=nullptr,retiredWindow=nullptr;
        std::weak_ptr<void> token;std::shared_ptr<void> retained;
        std::wstring replacementStatus;int readsAfter=-1;
        state.retire=[&]{
            state.Trace("find retired window");
            retiredWindow=OwnedCurveWindow();
            if(retiredWindow){token=ce::CaptureWindowSession(retiredWindow);retained=token.lock();}
            if(action!=1){state.Trace("close retired window");ce::CloseOwned(nullptr);state.Trace("close returned");}
            if(action!=0){state.Trace("open replacement");replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(fixture.replacement),false));
                state.Trace("replacement open returned");
                Check(IsWindow(replacementWindow),"selection retirement creates a live replacement");
                replacementStatus=GetText(replacementWindow,kIdStatus);}
            readsAfter=fixture.Reads();
        };
        auto session=fixture.Session();
        if(writing)session.sources[0].writeKeySelection=SelectionRetirementProvider(state);
        else session.sources[0].readKeySelection=SelectionRetirementProvider(state);
        state.armed=initial;
        state.Trace("fixture open");
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        state.Trace("fixture open returned");
        int applies=0,calls=0,writes=0;
        if(initial)Check(window==nullptr,"initial creation does not report a retired selection session as successful");
        else{
            Check(IsWindow(window),"selection callback fixture opens before arming");
            state.Trace("prepare trigger");
            if(!writing||entry!=0)ClickKey(window,1);
            if(writing&&entry==5){const int priorCalls=state.calls,priorWrites=state.writes,priorApplies=fixture.Applies();
                TriggerSelectionWrite(window,entry);
                Check(state.calls==priorCalls+1&&state.writes==priorWrites+1&&fixture.Applies()==priorApplies&&KeyCount(window)==3,
                    "unarmed real existing-key doubleclick publishes selection once without editing the curve");}
            if(writing&&(entry==2||entry==4)){Click(window,kIdDeleteKey);if(entry==4)Click(window,kIdUndo);}
            if(writing&&(entry==3||entry==6))SetText(window,kIdValueEdit,L"7");
            applies=fixture.Applies();calls=state.calls;writes=state.writes;state.armed=true;
            state.Trace("trigger");
            if(writing)TriggerSelectionWrite(window,entry);else TriggerSelectionRead(window,entry);
            state.Trace("trigger returned");
        }
        const bool hostEdit=writing&&(entry==2||entry==3||entry==4||entry==6);
        if(state.retireCalls!=1||state.destroyedDuringCopy!=0||state.calls!=calls+int(phase!=0))
            std::fprintf(stderr,"SELECTION_LIFETIME write=%d entry=%d phase=%d action=%d response=%d calls=%d before=%d retires=%d destroyedDuringCopy=%d writes=%d beforeWrites=%d applies=%d beforeApplies=%d armed=%d\n",
                writing,entry,phase,action,response,state.calls,calls,state.retireCalls,state.destroyedDuringCopy,
                state.writes,writes,fixture.Applies(),applies,state.armed);
        Check(state.retireCalls==1&&state.destroyedDuringCopy==0&&
            state.calls==calls+int(phase!=0),
            "selection copy, invocation and final release retire exactly one original callback");
        Check(state.writes==writes+int(writing&&phase!=0&&response==0)&&
            fixture.Applies()==applies+int(hostEdit)&&fixture.Reads()==readsAfter,
            "selection retirement preserves completed edits without repeated selection writes, old reads or later apply");
        if(!initial)Check(bool(retained)&&!ce::IsWindowSessionCurrent(retiredWindow,token),
            "retaining the old token cannot revive a selection continuation");
        if(replacementWindow){
            state.Trace("check replacement");
            Check(IsWindow(replacementWindow)&&KeyCount(replacementWindow)==2&&
                GetText(replacementWindow,kIdStatus)==replacementStatus&&fixture.replacement.applyCalls==0,
                "retired selection results cannot change replacement selection, status or channels");
            const auto recoveryCanvas=GetDlgItem(replacementWindow,kIdCanvas);
            const auto recoveryPoint=AskPoint(replacementWindow,kHookPointForKey,1);
            const auto captureBefore=GetCapture();
            const std::array<SHORT,4> modifiersBefore{GetKeyState(VK_SPACE),GetKeyState(VK_MENU),GetKeyState(VK_CONTROL),GetKeyState(VK_SHIFT)};
            ClickKey(replacementWindow,1);
            const auto selectedFrame=GetText(replacementWindow,kIdFrameEdit);
            const bool deleteEnabled=IsWindowEnabled(GetDlgItem(replacementWindow,kIdDeleteKey))!=FALSE;
            Click(replacementWindow,kIdDeleteKey);
            const int recoveryKeys=KeyCount(replacementWindow);
            if(fixture.replacement.applyCalls!=1||recoveryKeys!=1){
                CurveEditorAxisState geometry{};SendMessageW(replacementWindow,WM_APP+394,0,reinterpret_cast<LPARAM>(&geometry));
                std::fprintf(stderr,"SELECTION_RECOVERY write=%d entry=%d phase=%d action=%d response=%d applies=%d keys=%d frame=%ls deleteEnabled=%d hwnd=%p canvas=%p point=%ld,%ld plot=%ld,%ld,%ld,%ld focus=%p captureBefore=%p captureAfter=%p modifiers=%04x,%04x,%04x,%04x status=%ls\n",
                    writing,entry,phase,action,response,fixture.replacement.applyCalls,recoveryKeys,selectedFrame.c_str(),deleteEnabled,
                    replacementWindow,recoveryCanvas,recoveryPoint.x,recoveryPoint.y,geometry.plot.left,geometry.plot.top,geometry.plot.right,geometry.plot.bottom,
                    GetFocus(),captureBefore,GetCapture(),unsigned(static_cast<unsigned short>(modifiersBefore[0])),unsigned(static_cast<unsigned short>(modifiersBefore[1])),
                    unsigned(static_cast<unsigned short>(modifiersBefore[2])),unsigned(static_cast<unsigned short>(modifiersBefore[3])),GetText(replacementWindow,kIdStatus).c_str());
                std::fflush(stderr);
            }
            Check(fixture.replacement.applyCalls==1&&recoveryKeys==1,
                "replacement remains editable after selection callbacks and guards unwind");
        }else Check(OwnedCurveWindow()==nullptr,"closed selection callback leaves no registered window");
        state.Trace("final close");ce::CloseOwned(nullptr);state.Trace("case complete");
    }
    Note("selection read/write entries protect private arguments and session lifetime through copying, invocation and release");
}

void SelectionFailuresAndEcho(){
    for(bool writing:{false,true})for(int failure=0;failure!=3;++failure){
        SelectionLifetimeFixture fixture(false);bool armed=false;int failures=0,reads=0,writes=0;
        std::vector<int> nativeFrames{10};
        auto session=fixture.Session();
        struct Failure {
            bool* armed;int* failures;int kind;
            Failure(bool& a,int& f,int k):armed(&a),failures(&f),kind(k){}
            Failure(const Failure& other):armed(other.armed),failures(other.failures),kind(other.kind){
                if(*armed&&kind==0){*armed=false;++*failures;throw std::bad_alloc();}
            }
            void Run()const{if(!*armed)return;*armed=false;++*failures;
                if(kind==1)throw std::runtime_error("selection provider unavailable");throw std::bad_alloc();}
        };
        const Failure fault(armed,failures,failure);
        session.sources[0].readKeySelection=[&,fault](const std::string&){
            if(!writing)fault.Run();++reads;return nativeFrames;
        };
        session.sources[0].writeKeySelection=[&,fault](const std::string&,const std::vector<int>& frames){
            if(writing)fault.Run();++writes;nativeFrames=frames;
        };
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"selection failure fixture opens");ClickKey(window,1);
        const auto history=LocalWriteHistory(window);const int beforeWrites=writes,beforeApplies=fixture.Applies();
        armed=true;
        if(writing)ClickKey(window,0);else{SetText(window,kIdValueEdit,L"7");Click(window,kIdApplyKey);}
        Check(failures==1&&fixture.Applies()==beforeApplies&&LocalWriteHistory(window)==history&&writes==beforeWrites,
            "selection callable copy failure and exceptions do not publish data, history or another selection write");
        if(!writing)Check(GetText(window,kIdFrameEdit)==L"10", "failed selection read preserves the existing selected key");
        // Native state can change independently. A normal explicit read adopts
        // it once, without synthesizing a selection write or an apply.
        nativeFrames={0};const int beforeReads=reads;
        ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(reads==beforeReads+1&&writes==beforeWrites&&fixture.Applies()==beforeApplies&&
            GetText(window,kIdFrameEdit)==L"0", "selection reading recovers independently without write-to-read recursion");
        nativeFrames.clear();ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(!IsWindowEnabled(GetDlgItem(window,kIdDeleteKey))&&writes==beforeWrites,
            "an empty native selection is a valid clear rather than a failed read");
        ClickKey(window,1);Check(writes==beforeWrites+1&&nativeFrames==std::vector<int>{10},
            "selection writing recovers with one explicit user action");
        const int acceptedWrites=writes;ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(writes==acceptedWrites&&GetText(window,kIdFrameEdit)==L"10",
            "later readback confirms an accepted write without another write");
        ce::CloseOwned(nullptr);
    }
    Note("selection exceptions preserve the current path, empty reads clear selection and explicit readback stays read-only");
}

void SelectionNestedAndHistory(){
    for(bool writing:{false,true}){
        SelectionLifetimeFixture fixture(false);bool armed=false,pumping=false;int callbacks=0,nested=0,writes=0;
        std::vector<int> nativeFrames{10};HWND window=nullptr;
        auto pump=[&]{
            armed=false;pumping=true;const int before=fixture.Applies();
            for(int id:{kIdDeleteKey,kIdAddKey,kIdUndo,kIdRedo,kIdApplyKey})Click(window,id);
            ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
            SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
            SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
            HWND canvas=GetDlgItem(window,kIdCanvas);
            for(UINT message:{WM_MBUTTONDOWN,WM_MOUSEMOVE,WM_MBUTTONUP,WM_LBUTTONDOWN,WM_LBUTTONUP,WM_LBUTTONDBLCLK})
                SendMessageW(canvas,message,MK_LBUTTON,MAKELPARAM(90,90));
            SendMessageW(GetDlgItem(window,kIdChannelList),WM_KEYDOWN,VK_RETURN,0);
            Check(nested==0&&fixture.Applies()==before,
                "selection callbacks cannot recursively author, read, write or switch their active channel");
            pumping=false;
        };
        auto session=fixture.Session();
        session.sources[0].readKeySelection=[&](const std::string&){
            ++callbacks;if(pumping)++nested;if(armed&&!writing)pump();return nativeFrames;
        };
        session.sources[0].writeKeySelection=[&](const std::string&,const std::vector<int>& frames){
            ++callbacks;++writes;if(pumping)++nested;if(armed&&writing)pump();nativeFrames=frames;
        };
        window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"selection recursion fixture opens");ClickKey(window,1);armed=true;
        if(writing)ClickKey(window,0);else{ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);}
        Check(!armed&&nested==0&&fixture.Applies()==0,"one selection request completes after blocked nested UI input");
        SendMessageW(window,WM_APP+401,0,0);
        ClickKey(window,1);Click(window,kIdDeleteKey);
        int previousWrites=writes;Click(window,kIdUndo);
        Check(writes==previousWrites+1&&KeyCount(window)==2,
            "one local Undo publishes its restored selection exactly once");
        previousWrites=writes;Click(window,kIdRedo);
        Check(writes==previousWrites+1&&KeyCount(window)==1,
            "one local Redo publishes its restored selection exactly once");
        Check(nested==0&&callbacks>0,"deferred normal reads remain usable after selection callback protection");
        ce::CloseOwned(nullptr);
    }
    Note("selection callback recursion is gated and local history publishes restored selection once");
}

void SelectionReadSemantics(){
    SelectionLifetimeFixture fixture(false);std::vector<int> nativeFrames{10};int reads=0,writes=0;
    auto session=fixture.Session();
    session.sources[0].readKeySelection=[&](const std::string&){++reads;return nativeFrames;};
    session.sources[0].writeKeySelection=[&](const std::string&,const std::vector<int>& frames){++writes;nativeFrames=frames;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    Check(IsWindow(window),"selection result semantics fixture opens");ClickKey(window,1);
    SetText(window,kIdValueEdit,L"7");Click(window,kIdApplyKey);
    Check(fixture.Applies()==1&&FindKey(fixture.local.data,0,10)->value==7,
        "an unchanged selection returns the ordinary no-change result and permits its intended edit");
    nativeFrames={0};SetText(window,kIdValueEdit,L"9");const int beforeWrites=writes;
    Click(window,kIdApplyKey);
    Check(fixture.Applies()==1&&GetText(window,kIdFrameEdit)==L"0"&&
        FindKey(fixture.local.data,0,0)->value==1,
        "a changed selection adopts the native key and rejects the stale numeric edit");
    // The command tail may confirm the new selection. It must not submit data.
    Check(writes==beforeWrites+1,"a rejected stale edit confirms the adopted selection exactly once without recursive publication");
    SetText(window,kIdValueEdit,L"9");Click(window,kIdApplyKey);
    Check(fixture.Applies()==2&&FindKey(fixture.local.data,0,0)->value==9,
        "retrying against the adopted selection edits the intended native key once");
    nativeFrames.clear();Click(window,kIdDeleteKey);
    Check(fixture.Applies()==2&&!IsWindowEnabled(GetDlgItem(window,kIdDeleteKey))&&reads>0,
        "a valid empty selection cannot fall through into deleting the previously selected key");
    ce::CloseOwned(nullptr);
    Note("unchanged, changed and empty selection reads preserve numeric-edit refusal semantics");

    // A visible canvas click seeks before mouse-up publishes its selection.
    // The seek's synchronous timer must not read back the previous host set.
    for(bool initiallySelected:{false,true}){
        std::printf("selection visible seek initiallySelected=%d\n",int(initiallySelected));std::fflush(stdout);
        SelectionLifetimeFixture visible(false);
        std::vector<int> frames=initiallySelected?std::vector<int>{10}:std::vector<int>{};
        int frame=3,selectionWrites=0,selectionReads=0;
        auto selectedSession=visible.Session();
        selectedSession.currentFrame=[&]{return double(frame);};
        selectedSession.seek=[&](int value){frame=value;};
        selectedSession.sources[0].readKeySelection=[&](const std::string&){++selectionReads;return frames;};
        selectedSession.sources[0].writeKeySelection=[&](const std::string&,const std::vector<int>& values){++selectionWrites;frames=values;};
        HWND selectedWindow=static_cast<HWND>(ce::Open(nullptr,std::move(selectedSession),false));
        Check(IsWindow(selectedWindow),"visible seek selection fixture opens");
        SetWindowLongPtrW(selectedWindow,GWL_STYLE,GetWindowLongPtrW(selectedWindow,GWL_STYLE)|WS_VISIBLE);
        ClickKey(selectedWindow,0);
        Check(frame==0&&frames==std::vector<int>{0}&&selectionWrites==1&&GetText(selectedWindow,kIdFrameEdit)==L"0"&&visible.Applies()==0,
            "visible key click seeks and publishes the new selection once instead of restoring the old host selection");
        frames={10};const int beforeRead=selectionReads;
        SendMessageW(selectedWindow,WM_TIMER,kPlayheadTimerId,0);
        Check(selectionReads==beforeRead+1&&selectionWrites==1&&GetText(selectedWindow,kIdFrameEdit)==L"10",
            "the next ordinary timer still adopts an independent host selection without writing it back");
        ce::CloseOwned(nullptr);
    }
    Note("visible key selection survives synchronous seek and ordinary timer synchronization remains enabled");
}

void SelectionDenseStableTick(){
    std::array<std::size_t,2> bytes{},largest{};
    for(int dense=0;dense!=2;++dense){
        HostSource host;std::vector<ac::Key> keys;
        const int count=dense?100000:2;keys.reserve(count);
        for(int index=0;index<count;++index)keys.push_back(MakeKey(index,double(index%11)));
        host.data.push_back(MakeChannel("dense",L"Dense",std::move(keys)));
        int reads=0,writes=0;auto session=MakeSession(host);session.currentFrame=[](){return 0.0;};
        session.sources[0].readKeySelection=[&](const std::string&){++reads;return std::vector<int>{};};
        session.sources[0].writeKeySelection=[&](const std::string&,const std::vector<int>&){++writes;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"dense stable-selection timer fixture opens");
        SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
        const int oldReads=reads,oldHostReads=host.readCalls;
        g_curveAllocationBytes=g_curveLargestAllocation=0;g_curveMeasureAllocations=true;
        for(int tick=0;tick!=8;++tick)SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
        g_curveMeasureAllocations=false;bytes[dense]=g_curveAllocationBytes;largest[dense]=g_curveLargestAllocation;
        bool unchanged=true;
        for(int index=0;index<count;++index){const auto& key=host.data[0].track.keys[index];
            if(key.frame!=index||key.value!=double(index%11)){unchanged=false;break;}}
        Check(reads==oldReads+8&&host.readCalls==oldHostReads&&writes==0&&host.applyCalls==0&&
            KeyCount(window)==count&&unchanged,
            "stable selection ticks read only selection and preserve dense source contents");
        std::printf("DENSE_SELECTION keys=%d ticks=8 bytes=%zu largest=%zu\n",count,bytes[dense],largest[dense]);
        ce::CloseOwned(nullptr);
    }
    Check(largest[1]<100000*sizeof(int)&&bytes[1]<=bytes[0]+4096,
        "unchanged dense VMD selection ticks do not allocate a full key-address array or per-key containers");
    Note("stable selection ticks preserve the common path without dense-track-sized allocation");
}

HostSource ComparisonLifetimeHost(bool link=false){
    HostSource host;
    if(link){
        for(const char* id:{"rotation.x","rotation.y","rotation.z"}){
            auto channel=MakeChannel(id,L"Rotation",{MakeKey(0,0),MakeKey(10,1),MakeKey(20,2),MakeKey(30,3)});
            channel.interpolationGroup="rotation";std::string error;
            for(std::size_t end=1;end!=4;++end)Check(ce::SetChannelControls(channel,end,{20,20,107,107},error),
                "prepare real adjacent-control intervals");
            host.data.push_back(std::move(channel));
        }
    }else{
        host.compareEnabled=true;host.initialMain="main";
        host.data.push_back(MakeGroupChannel("main",L"Main",0,{MakeKey(0,0),MakeKey(10,0),MakeKey(20,0)}));
        host.data.push_back(MakeGroupChannel("local",L"Local",0,{MakeKey(0,0),MakeKey(10,1),MakeKey(20,0)}));
        host.data.push_back(MakeGroupChannel("outside",L"Outside",-1,{MakeKey(0,0),MakeKey(10,2),MakeKey(20,0)}));
        host.data.push_back(MakeGroupChannel("flat",L"Flat",1,{MakeKey(0,1),MakeKey(20,1)}));
        for(auto& channel:host.data)channel.collections.push_back({"collection",L"Collection"});
    }
    return host;
}

void SetComparisonLinkFields(HWND window){
    Check(IsWindowEnabled(GetDlgItem(window,1096)),"real MMD coordinate fields are editable");
    for(int index=0;index!=4;++index)SetText(window,1096+index,index<2?L"20":index==2?L"90":L"100");
}
void TriggerComparisonLink(HWND window,bool mouse){
    if(!mouse){SendMessageW(GetDlgItem(window,1098),WM_KEYDOWN,VK_RETURN,0);return;}
    HWND panel=GetDlgItem(window,kIdMmdPanel);double controls[4]{};
    Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls))!=0,
        "adjacent-control lifetime fixture has a real editable MMD handle");
    const auto getDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    UINT dpi=getDpi?getDpi(panel):0;
    if(!dpi){HDC dc=GetDC(panel);dpi=dc?UINT(GetDeviceCaps(dc,LOGPIXELSX)):96;if(dc)ReleaseDC(panel,dc);}
    RECT client{};GetClientRect(panel,&client);const int margin=MulDiv(6,dpi?dpi:96,96);
    const int side=std::min(int(client.right),int(client.bottom))-2*margin;
    const auto point=[&](double x,double y){return MAKELPARAM(margin+int(std::lround(x*side/127)),
        margin+side-int(std::lround(y*side/127)));};
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,point(controls[2],controls[3]));
    SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,point(90,100));
    SendMessageW(panel,WM_LBUTTONUP,0,point(90,100));
}

struct ComparisonRetirementState {
    int kind=0,phase=0,response=0,calls=0,writes=0,retireCalls=0,destroyedDuringCopy=0;
    int hostMode=ce::kCompareSameGroup;
    bool armed=false,copying=false;
    const void* copyingOriginal=nullptr;
    std::wstring* errorOutput=nullptr;
    std::function<void()> retire;
    void Retire(){armed=false;++retireCalls;CurveDiagStage(kCurveDiagStageRetireBegin);retire();CurveDiagStage(kCurveDiagStageRetireEnd);
        if(errorOutput){*errorOutput=L"comparison callback retired";
            Check(*errorOutput==L"comparison callback retired","comparison writer error output survives retirement");}}
};
struct ComparisonProviderBase {
    ComparisonRetirementState* state;int marker=67;mutable bool invoked=false;
    explicit ComparisonProviderBase(ComparisonRetirementState& value):state(&value){}
    ComparisonProviderBase(const ComparisonProviderBase& other):state(other.state),marker(other.marker){
        if(!state->armed||state->phase!=0)return;
        // Addresses only; the stage ring never dereferences the possibly retired original.
        CurveDiagStage(kCurveDiagStageProviderCopyEntry,reinterpret_cast<std::uintptr_t>(this),reinterpret_cast<std::uintptr_t>(&other));
        state->copying=true;state->copyingOriginal=&other;state->Retire();
        CurveDiagStage(kCurveDiagStageProviderCopyAfterRetire,reinterpret_cast<std::uintptr_t>(this),reinterpret_cast<std::uintptr_t>(&other));
        Check(state->destroyedDuringCopy==0&&other.marker==67,
            "comparison and link provider originals stay alive through reentrant callable copying");
        state->copying=false;if(state->response==2)throw std::bad_alloc();
    }
    ~ComparisonProviderBase(){
        if(state->copying&&state->copyingOriginal==this)++state->destroyedDuringCopy;
        if(invoked&&state->armed&&state->phase==2)state->Retire();
    }
    void Called()const{invoked=true;++state->calls;}
};
template<int Kind>struct ComparisonRetirementProvider:ComparisonProviderBase {
    explicit ComparisonRetirementProvider(ComparisonRetirementState& value):ComparisonProviderBase(value){}
    auto operator()()const{
        Called();
        if(state->armed&&state->phase==1)state->Retire();
        if(state->response==2)throw std::runtime_error("comparison or link callback failed");
        if constexpr(Kind==0)return state->response==1?-7:ce::kCompareAllGroups;
        else return state->response==0;
    }
    bool operator()(int mode,std::wstring& error)const{
        Called();state->errorOutput=&error;
        if(state->response==0){state->hostMode=mode;++state->writes;}
        if(state->armed&&state->phase==1)state->Retire();
        if(state->response==2)throw std::runtime_error("comparison writer failed");
        if(state->response==1)error=L"mode write refused";
        return state->response==0;
    }
};

void ComparisonCallbackLifetime(){
    // Refresh the unpublished module slot once, before any iteration or
    // callback can fault; the handler keeps reading the published snapshot.
    CurveDiagRefreshModules();
    for(int route=0;route!=4;++route)for(int phase=0;phase!=3;++phase)
    for(int action=0;action!=3;++action)for(int response=0;response!=3;++response){
        const int kind=std::min(route,2);
        std::printf("comparison lifetime kind=%d mouse=%d phase=%d action=%d response=%d\n",kind,route==3,phase,action,response);
        std::fflush(stdout);
        CurveDiagSetCase(route,kind,route==3?1:0,phase,action,response);
        CurveDiagStage(kCurveDiagStageIterationBegin);
        auto host=ComparisonLifetimeHost(kind==2);HostSource replacement;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        ComparisonRetirementState state;state.kind=kind;state.phase=phase;state.response=response;
        HWND retiredWindow=nullptr,replacementWindow=nullptr;std::wstring replacementStatus;
        std::weak_ptr<void> token;std::shared_ptr<void> retained;int readsAfter=-1;
        state.retire=[&]{retiredWindow=OwnedCurveWindow();token=ce::CaptureWindowSession(retiredWindow);retained=token.lock();
            if(action!=1){CurveDiagStage(kCurveDiagStageRetireCloseOwnedBegin,reinterpret_cast<std::uintptr_t>(retiredWindow));
                ce::CloseOwned(nullptr);
                CurveDiagStage(kCurveDiagStageRetireCloseOwnedEnd,reinterpret_cast<std::uintptr_t>(retiredWindow));}
            if(action!=0){CurveDiagStage(kCurveDiagStageRetireReplacementOpenBegin);
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                CurveDiagStage(kCurveDiagStageRetireReplacementOpenEnd,reinterpret_cast<std::uintptr_t>(replacementWindow));
                Check(IsWindow(replacementWindow),"comparison retirement installs a real replacement session");
                replacementStatus=GetText(replacementWindow,kIdStatus);}
            readsAfter=host.readCalls;
        };
        auto session=MakeSession(host);
        if(kind==0)session.sources[0].readComparisonMode=ComparisonRetirementProvider<0>(state);
        if(kind==1)session.sources[0].writeComparisonMode=ComparisonRetirementProvider<1>(state);
        if(kind==2)session.sources[0].linkAdjacentControls=ComparisonRetirementProvider<2>(state);
        state.armed=kind==0;
        CurveDiagStage(kCurveDiagStageInitialOpenBegin);
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        CurveDiagStage(kCurveDiagStageInitialOpenEnd,reinterpret_cast<std::uintptr_t>(window));
        if(kind==0)Check(window==nullptr,"initial comparison read cannot return a retired Open as successful");
        else{Check(IsWindow(window),"comparison lifetime fixture opens before arming");
            if(kind==2){ClickKey(window,2);SetComparisonLinkFields(window);}
            state.armed=true;
            CurveDiagStage(kCurveDiagStageTriggerBegin,reinterpret_cast<std::uintptr_t>(window));
            if(kind==1)ClickCompareMode(window,ce::kCompareAllGroups);else TriggerComparisonLink(window,route==3);
            CurveDiagStage(kCurveDiagStageTriggerEnd,reinterpret_cast<std::uintptr_t>(window));
        }
        Check(state.retireCalls==1&&state.calls==int(phase!=0)&&state.destroyedDuringCopy==0,
            "comparison read, write and link callbacks retire exactly once through copying, invocation and release");
        Check(state.writes==int(kind==1&&phase!=0&&response==0)&&host.applyCalls==0&&host.readCalls==readsAfter,
            "comparison retirement neither repeats an accepted mode write nor continues into old source reads or apply");
        Check(bool(retained)&&!ce::IsWindowSessionCurrent(retiredWindow,token),
            "strong old tokens cannot make comparison continuations current");
        if(replacementWindow){Check(KeyCount(replacementWindow)==2&&GetText(replacementWindow,kIdStatus)==replacementStatus&&replacement.applyCalls==0,
                "comparison results and operation guards leave replacement state untouched");
            CurveDiagStage(kCurveDiagStageReplacementKeyClickBegin,reinterpret_cast<std::uintptr_t>(replacementWindow));
            ClickKey(replacementWindow,1);
            CurveDiagStage(kCurveDiagStageReplacementKeyClickEnd,reinterpret_cast<std::uintptr_t>(replacementWindow));
            CurveDiagStage(kCurveDiagStageReplacementDeleteBegin,reinterpret_cast<std::uintptr_t>(replacementWindow));
            Click(replacementWindow,kIdDeleteKey);
            CurveDiagStage(kCurveDiagStageReplacementDeleteEnd,reinterpret_cast<std::uintptr_t>(replacementWindow));
            Check(replacement.applyCalls==1,"replacement stays editable after comparison and link retirement");}
        else Check(OwnedCurveWindow()==nullptr,"closed comparison session is no longer registered");
        CurveDiagStage(kCurveDiagStageFinalCloseOwnedBegin);
        ce::CloseOwned(nullptr);
        CurveDiagStage(kCurveDiagStageFinalCloseOwnedEnd);
        CurveDiagStage(kCurveDiagStageIterationEnd);
    }
    Note("comparison read/write and link callbacks use the shared copy, invocation and release lifetime boundary");
}

void ComparisonWriteEntry(HWND window,int entry){
    if(entry==0)ClickCompareMode(window,ce::kCompareAllGroups);
    if(entry==1)ClickChannelOnce(window,2);
    if(entry==2){
        // Isolate the real doubleclick dispatch. A full DOWN/UP/DBLCLK/UP
        // gesture activates twice, so a one-shot failure on DOWN would be
        // followed by a legitimate successful retry on DBLCLK.
        HWND tree=GetDlgItem(window,kIdChannelList);const POINT point=ChannelRowPoint(window,2,false);
        SendMessageW(tree,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        SendMessageW(tree,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
    }
    if(entry==3){HWND tree=GetDlgItem(window,kIdChannelList);HTREEITEM collection=TreeView_GetRoot(tree);
        Check(collection&&ChannelItemIndex(tree,collection)<0,"comparison fixture has a real collection row");
        TreeView_SelectItem(tree,collection);SendMessageW(tree,WM_KEYDOWN,VK_SPACE,0);}
    if(entry==4)Click(window,kIdChannelAll);
    if(entry==5)Click(window,kIdChannelInvert);
    if(entry==6)ClickButton(window,kIdCompareFilter);
}

struct ComparisonFailureState {bool armed=false;int failure=0,failures=0;HWND* observedWindow=nullptr;};
struct ComparisonFailure {
    ComparisonFailureState* state;
    explicit ComparisonFailure(ComparisonFailureState& value):state(&value){}
    ComparisonFailure(const ComparisonFailure& other):state(other.state){
        if(state->armed&&state->failure==0){if(state->observedWindow)*state->observedWindow=OwnedCurveWindow();
            state->armed=false;++state->failures;throw std::bad_alloc();}}
    bool Fail()const{if(!state->armed)return false;state->armed=false;++state->failures;
        if(state->observedWindow)*state->observedWindow=OwnedCurveWindow();
        if(state->failure==1)return true;
        if(state->failure==2)throw std::runtime_error("comparison provider unavailable");
        throw std::bad_alloc();}
};

void ComparisonWriterAtomicity(){
    for(int entry=0;entry!=7;++entry)for(int failure=0;failure!=4;++failure){
        std::printf("comparison atomicity entry=%d failure=%d\n",entry,failure);std::fflush(stdout);
        auto host=ComparisonLifetimeHost();ComparisonFailureState state;state.failure=failure;
        const ComparisonFailure fault(state);int writes=0;auto session=MakeSession(host);
        session.sources[0].writeComparisonMode=[&,fault](int mode,std::wstring& error){
            if(fault.Fail()){error=L"mode refused";return false;}host.storedMode=mode;++writes;return true;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"comparison refusal fixture opens");
        SetText(window,kIdBakeFirstEdit,L"0");SetText(window,kIdBakeLastEdit,L"20");
        int before[12]{};ReadCompare(window,before);const auto history=LocalWriteHistory(window);const auto data=host.data;
        std::array<int,4> flags{};for(int index=0;index!=4;++index)flags[index]=ChannelVisible(window,index);
        state.armed=true;ComparisonWriteEntry(window,entry);
        int after[12]{};ReadCompare(window,after);bool sameFlags=true;
        for(int index=0;index!=4;++index)sameFlags&=flags[index]==ChannelVisible(window,index);
        const auto actualHistory=LocalWriteHistory(window);const bool sameData=SameWritePayload(host.data,data);
        const int checked=CheckedCompareMode(window);
        const bool unchanged=state.failures==1&&writes==0&&before[0]==after[0]&&before[1]==after[1]&&sameFlags&&
            checked==before[0]&&host.storedMode==ce::kCompareSameGroup&&host.applyCalls==0&&sameData&&actualHistory==history;
        if(!unchanged)std::fprintf(stderr,"COMPARISON_ATOMICITY entry=%d failure=%d failures=%d writes=%d mode=%d/%d active=%d/%d checked=%d stored=%d flagsSame=%d dataSame=%d applies=%d history=%llu,%llu/%llu,%llu armed=%d\n",
            entry,failure,state.failures,writes,before[0],after[0],before[1],after[1],checked,host.storedMode,sameFlags,sameData,
            host.applyCalls,static_cast<unsigned long long>(history[0]),static_cast<unsigned long long>(history[1]),
            static_cast<unsigned long long>(actualHistory[0]),static_cast<unsigned long long>(actualHistory[1]),state.armed);
        Check(unchanged,
            "mode refusal, exception and copy failure keep mode, active curve, display mask, data and history unchanged");
        CheckTicksMatchCanvas(window,4,"comparison writer refusal preserves native list ticks");
        ComparisonWriteEntry(window,entry);
        Check(writes==1&&CheckedCompareMode(window)==(entry==0?ce::kCompareAllGroups:ce::kCompareFree)&&host.applyCalls==0,
            "each real comparison entry recovers with exactly one accepted mode write and no curve apply");
        ce::CloseOwned(nullptr);
    }
    Note("all comparison writer entries preserve atomic display state through rejection and exceptions");
}

void ComparisonFilterAcceptedAllocation(){
    auto host=ComparisonLifetimeHost();bool armed=false;int writes=0;auto session=MakeSession(host);
    session.sources[0].writeComparisonMode=[&](int mode,std::wstring&){host.storedMode=mode;++writes;
        if(armed)g_curveFailNextAllocation=true;return true;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    Check(IsWindow(window),"accepted filter allocation fixture opens");
    SetText(window,kIdBakeFirstEdit,L"0");SetText(window,kIdBakeLastEdit,L"20");
    Check(ChannelVisible(window,2)==0,"automatic comparison initially hides the ungrouped varying curve");
    const auto history=LocalWriteHistory(window);const auto failures=g_curveAllocationFailures;
    armed=true;ClickButton(window,kIdCompareFilter);
    int accepted[12]{};ReadCompare(window,accepted);
    Check(!g_curveFailNextAllocation&&g_curveAllocationFailures==failures+1&&writes==1&&
        host.storedMode==ce::kCompareFree&&accepted[0]==ce::kCompareFree&&
        ChannelVisible(window,0)==1&&ChannelVisible(window,1)==1&&ChannelVisible(window,2)==1&&ChannelVisible(window,3)==0&&
        host.applyCalls==0&&LocalWriteHistory(window)==history,
        "allocation failure after writer acceptance preserves the complete accepted mode and private filter mask");
    armed=false;ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
    Check(writes==1&&ChannelVisible(window,2)==1&&CheckedCompareMode(window)==ce::kCompareFree,
        "filter UI recovery does not repeat the accepted mode write");
    CheckTicksMatchCanvas(window,4,"accepted filter mask survives subsequent native UI refresh");
    ce::CloseOwned(nullptr);
    Note("accepted filter mode and mask publish together before allocating UI work");
}

void ComparisonReadFailuresAndEntries(){
    for(bool replacing:{false,true})for(int failure:{0,2,3}){
        auto host=ComparisonLifetimeHost();
        if(replacing)Check(IsWindow(static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false))),
            "comparison read failure starts from a real existing session");
        ComparisonFailureState state;state.failure=failure;HWND attemptedWindow=nullptr;state.observedWindow=&attemptedWindow;
        const ComparisonFailure fault(state);auto session=MakeSession(host);
        session.sources[0].readComparisonMode=[fault]{fault.Fail();return ce::kCompareAllGroups;};
        state.armed=true;
        Check(ce::Open(nullptr,std::move(session),false)==nullptr&&state.failures==1&&attemptedWindow&&
            !IsWindow(attemptedWindow)&&OwnedCurveWindow()==nullptr&&host.applyCalls==0,
            "comparison read copy failure and exceptions close the exact failed Open rather than fabricating a fallback mode");
        ce::CloseOwned(nullptr);
    }
    for(bool external:{false,true}){
        auto host=ComparisonLifetimeHost();ExternalCurveJournal journal;int reads=0;
        auto session=external?journal.Session():MakeSession(host);
        if(!external)session.sources.push_back(host.Make("second",L"Second"));
        session.sources[1].comparisonEnabled=true;
        session.sources[1].readComparisonMode=[&]{++reads;return ce::kCompareAllGroups;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"comparison source-switch fixture opens");
        const auto select=[&](int index){SendMessageW(GetDlgItem(window,kIdSourceCombo),CB_SETCURSEL,index,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdSourceCombo,CBN_SELCHANGE),0);};
        select(1);
        Check(reads==1&&CheckedCompareMode(window)==ce::kCompareAllGroups,
            "first entry into a local or shared-history source reads its real comparison mode");
        select(0);select(1);
        Check(reads==(external?2:1)&&host.applyCalls==0&&journal.applies==0,
            "cached local and shared-history source revisits retain the original mode-read frequency");
        ce::CloseOwned(nullptr);
    }
    // Manual callers retain Channel/state references before EnterFree. A real
    // same-window replacement must unwind each caller before it uses them.
    for(int entry=1;entry!=7;++entry){
        auto host=ComparisonLifetimeHost();HostSource replacement;
        replacement.data.push_back(MakeChannel("new",L"New",{MakeKey(0,91),MakeKey(20,99)}));
        bool armed=false;int writes=0;HWND replacementWindow=nullptr;std::wstring status;
        auto session=MakeSession(host);
        session.sources[0].writeComparisonMode=[&](int mode,std::wstring&){host.storedMode=mode;++writes;
            if(armed){armed=false;replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                status=GetText(replacementWindow,kIdStatus);}return true;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"manual comparison replacement fixture opens");
        SetText(window,kIdBakeFirstEdit,L"0");SetText(window,kIdBakeLastEdit,L"20");
        auto token=ce::CaptureWindowSession(window);auto retained=token.lock();armed=true;
        ComparisonWriteEntry(window,entry);
        Check(writes==1&&host.applyCalls==0&&bool(retained)&&!ce::IsWindowSessionCurrent(window,token)&&
            IsWindow(replacementWindow)&&KeyCount(replacementWindow)==2&&GetText(replacementWindow,kIdStatus)==status,
            "manual comparison and filter callers never publish old state into a same-window replacement");
        ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
        Check(replacement.applyCalls==1,"manual comparison replacement remains editable after old caller references unwind");
        ce::CloseOwned(nullptr);
    }
    Note("comparison read errors preserve exact Open cleanup and all manual writer callers unwind after replacement");
}

void ComparisonReadAndLinkSemantics(){
    for(int raw:{-1,0,ce::kCompareSameGroup,ce::kCompareAllGroups,ce::kCompareFree,99}){
        auto host=ComparisonLifetimeHost();int reads=0;auto session=MakeSession(host);
        session.sources[0].readComparisonMode=[&]{++reads;return raw;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&reads==1&&CheckedCompareMode(window)==
            (raw>=ce::kCompareSameGroup&&raw<=ce::kCompareFree?raw:ce::kCompareSameGroup),
            "normal comparison integers retain the original valid-mode and fallback rules");
        ce::CloseOwned(nullptr);
    }
    for(bool mouse:{false,true})for(int result=0;result!=5;++result){
        std::printf("comparison link branch mouse=%d result=%d\n",mouse?1:0,result);std::fflush(stdout);
        auto host=ComparisonLifetimeHost(true);ComparisonFailureState state;state.failure=result==2?0:result==3?2:3;
        const ComparisonFailure fault(state);int calls=0;auto session=MakeSession(host);
        // The playback frame agrees with the selected interval end (20), so the
        // panel targets interval 10-20 (end 2) instead of 20-30.
        session.currentFrame=[] { return 20.0; };
        session.sources[0].linkAdjacentControls=[&,fault]{++calls;if(result>=2)fault.Fail();return result==0;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"link result fixture opens");ClickKey(window,2);
        // Keep a redo branch so a failed flag read cannot silently clear it.
        SetText(window,kIdValueEdit,L"2.5");Click(window,kIdApplyKey);Click(window,kIdUndo);
        Check(calls==0,"ordinary value fields and local undo never query the legacy adjacent-control flag");
        Check(GetText(window,kIdFrameEdit)==L"20","the link fixture keeps the 20 frame interval end selected");
        Check(GetText(window,kIdMmdReadout)==L"区间 10\u201320 帧",
            "the follow-playhead panel names the 10-20 frame target interval");
        double coords[4]{};
        Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(coords))!=0,
            "the displayed 10-20 interval keeps editable MMD controls; identical interval controls cannot identify the target");
        const auto history=LocalWriteHistory(window);const auto before=host.data;const int applies=host.applyCalls;
        std::array<double,4> neighbor{};Check(ce::ReadChannelControls(host.data[0],3,neighbor),"read original adjacent interval");
        if(!mouse)SetComparisonLinkFields(window);state.armed=result>=2;
        TriggerComparisonLink(window,mouse);
        std::array<double,4> after{};Check(ce::ReadChannelControls(host.data[0],3,after),"read resulting adjacent interval");
        if(result<2)Check(calls==1&&host.applyCalls==applies+1&&(result==0?after!=neighbor:after==neighbor),
            "link true mirrors the adjacent interval while false still accepts one unlinked edit");
        else Check(state.failures==1&&host.applyCalls==applies&&SameWritePayload(host.data,before)&&
            LocalWriteHistory(window)==history,
            "link exceptions and copy failures reject the complete edit without changing data or the redo branch");
        ce::CloseOwned(nullptr);
    }
    Note("comparison integer fallback and link false retain their original non-error meaning");
}

void ComparisonValueHandlesDoNotQueryLink(){
    HostSource host;int links=0;
    auto channel=MakeChannel("value",L"Value",{MakeKey(0,0,ac::Interpolation::Bezier,ac::Tangent::Free),
        MakeKey(20,5,ac::Interpolation::Bezier,ac::Tangent::Free),MakeKey(40,10,ac::Interpolation::Bezier,ac::Tangent::Free)});
    channel.track.keys[1].inWeight=channel.track.keys[1].outWeight=.3;host.data.push_back(std::move(channel));
    auto session=MakeSession(host);session.sources[0].linkAdjacentControls=[&]()->bool{++links;throw std::runtime_error("not an MMD-panel edit");};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    Check(IsWindow(window),"ordinary value handle fixture opens");ClickKey(window,1);
    const auto key=AskPoint(window,kHookPointForKey,1),handle=AskPoint(window,kHookPointForFrame,26),target=AskPoint(window,kHookPointForFrame,30);
    HWND canvas=GetDlgItem(window,kIdCanvas);
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(handle.x,key.y));
    SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x,key.y-20));
    SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(target.x,key.y-20));
    Check(links==0&&host.applyCalls==1&&std::abs(host.data[0].track.keys[1].outSlope)>.01,
        "value handle authoring remains independent and never invokes the legacy MMD link provider");
    ce::CloseOwned(nullptr);
}

void ComparisonNestedAndGeometryCopy(){
    for(int kind=0;kind!=3;++kind){
        auto host=ComparisonLifetimeHost(kind==2);HWND window=nullptr;bool armed=false,pumping=false;
        int calls=0,nested=0,writes=0;
        auto pump=[&]{armed=false;pumping=true;const int before=host.applyCalls;
            std::array<int,12> beforeCompare{},afterCompare{};ReadCompare(window,beforeCompare.data());
            std::vector<int> beforeFlags;for(std::size_t index=0;index<host.data.size();++index)
                beforeFlags.push_back(ChannelVisible(window,int(index)));
            const auto beforeHistory=LocalWriteHistory(window);
            const auto beforeFrame=GetText(window,kIdFrameEdit),beforeValue=GetText(window,kIdValueEdit);
            for(int id:{kIdCompareAllGroups,kIdCompareFree,kIdChannelAll,kIdCompareFilter,kIdDeleteKey})Click(window,id);
            SendMessageW(GetDlgItem(window,kIdChannelList),WM_KEYDOWN,VK_RETURN,0);
            ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
            Check(nested==0&&host.applyCalls==before,"comparison and link callback busy state rejects nested mode, selection and curve edits");
            ReadCompare(window,afterCompare.data());bool sameFlags=true;
            for(std::size_t index=0;index<beforeFlags.size();++index)
                sameFlags&=ChannelVisible(window,int(index))==beforeFlags[index];
            Check(beforeCompare==afterCompare&&sameFlags&&LocalWriteHistory(window)==beforeHistory&&
                GetText(window,kIdFrameEdit)==beforeFrame&&GetText(window,kIdValueEdit)==beforeValue,
                "nested UI messages preserve mode, active curve, display flags, selected-key fields and local history before the outer comparison callback resumes");
            pumping=false;};
        auto session=MakeSession(host);
        if(kind==0)session.sources[0].readComparisonMode=[&]{++calls;if(pumping)++nested;if(armed)pump();return ce::kCompareSameGroup;};
        if(kind==1)session.sources[0].writeComparisonMode=[&](int mode,std::wstring&){++calls;if(pumping)++nested;if(armed)pump();
            host.storedMode=mode;++writes;return true;};
        if(kind==2)session.sources[0].linkAdjacentControls=[&]{++calls;if(pumping)++nested;if(armed)pump();return true;};
        window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"comparison recursion fixture opens");
        if(kind==0){armed=true;auto next=MakeSession(host);next.sources[0].readComparisonMode=[&]{++calls;if(pumping)++nested;if(armed)pump();return ce::kCompareSameGroup;};
            Check(ce::Open(nullptr,std::move(next),false)==window,"read comparison recursion is exercised through real existing Open");}
        else if(kind==1){armed=true;ClickCompareMode(window,ce::kCompareAllGroups);}
        else{ClickKey(window,2);SetComparisonLinkFields(window);armed=true;TriggerComparisonLink(window,false);}
        Check(!armed&&nested==0&&host.applyCalls==(kind==2?1:0)&&writes==(kind==1?1:0)&&calls>0,
            "one owning comparison or link operation completes after nested UI messages are rejected");
        ce::CloseOwned(nullptr);
    }
    auto host=ComparisonLifetimeHost(true);bool armed=false;int sampleCopies=0,linkCalls=0;
    struct Sample {
        bool* armed;int* copies;
        Sample(bool& a,int& c):armed(&a),copies(&c){}
        Sample(const Sample& other):armed(other.armed),copies(other.copies){if(*armed){++*copies;throw std::bad_alloc();}}
        ac::Sample operator()(const ce::Channel& channel,double frame)const{return ac::Evaluate(channel.track,frame);}
    };
    for(auto& channel:host.data)channel.sample=Sample(armed,sampleCopies);
    auto session=MakeSession(host);const auto apply=session.sources[0].apply;
    session.sources[0].apply=[&,apply](const auto& next,auto& error){armed=false;return apply(next,error);};
    session.sources[0].linkAdjacentControls=[&]{++linkCalls;armed=true;return true;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    Check(IsWindow(window),"pure geometry comparison snapshot fixture opens");ClickKey(window,2);SetComparisonLinkFields(window);
    TriggerComparisonLink(window,false);
    Check(linkCalls==1&&sampleCopies==0&&host.applyCalls==1&&!armed,
        "adjacent-link geometry preparation never copies an unrelated Channel.sample closure");
    ce::CloseOwned(nullptr);
    Note("comparison recursion is gated and adjacent geometry preparation copies only required plain data");
}
// Phase A never closes or replaces a window inside this provider. The old
// direct call can therefore expose its missing copy boundary without invoking
// the lifetime-changing matrix before production protection is installed.
struct SubdivisionCopyState {
    bool armed=false,failCopy=false;
    unsigned copies=0,bodies=0,liveBodies=0;
    std::size_t channel=static_cast<std::size_t>(-1);
    int frame=-1;
};
struct SubdivisionCopyProvider {
    SubdivisionCopyState* state;
    bool invocationCopy=false;
    explicit SubdivisionCopyProvider(SubdivisionCopyState& value):state(&value){}
    SubdivisionCopyProvider(const SubdivisionCopyProvider& other):state(other.state){
        invocationCopy=state->armed;
        if(invocationCopy){++state->copies;if(state->failCopy)throw std::bad_alloc();}
    }
    bool operator()(std::vector<ce::Channel>& channels,std::size_t channel,int frame,
        std::size_t& index,std::wstring& error)const{
        ++state->bodies;if(!invocationCopy)++state->liveBodies;
        state->channel=channel;state->frame=frame;
        if(channel>=channels.size()){error=L"invalid test subdivision channel";return false;}
        std::string detail;
        if(ac::InsertPreservingShape(channels[channel].track,frame,index,detail)!=ac::InsertResult::Inserted){
            error=L"test subdivision could not prepare a key";return false;
        }
        channels[channel].track.keys[index].value=double(frame)*frame/20.0;return true;
    }
};
void NativeSubdivisionCopyBoundary(){
    bool failedCopyBlocked=true,successfulCopyOwned=true;
    for(bool failCopy:{true,false}){
        HostSource host;host.data.push_back(MakeChannel("native",L"Native",{MakeKey(0,0),MakeKey(20,20)}));
        host.data[0].sample=[](const ce::Channel&,double frame){return ac::Sample{frame*frame/20.0,frame/10.0};};
        host.data[0].nativeControls.resize(2);
        SubdivisionCopyState state;state.failCopy=failCopy;
        auto session=MakeSession(host);session.sources[0].subdivide=SubdivisionCopyProvider(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"safe subdivision copy probe opens the native two-key source");
        const HWND canvas=GetDlgItem(window,kIdCanvas);const POINT point=AskPoint(window,kHookPointForFrame,10);
        Check(canvas&&point.x>0&&point.y>0,"safe subdivision copy probe uses the real visible curve point");
        const auto token=ce::CaptureWindowSession(window);const auto history=LocalWriteHistory(window);
        Check(state.copies==0&&state.bodies==0,"preparing and opening the safe fixture does not call its subdivision provider");
        state.armed=true;SendMessageW(canvas,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        const auto after=LocalWriteHistory(window);const int keys=KeyCount(window);
        std::printf("subdivision copy fail_copy=%u copies=%u bodies=%u live_bodies=%u channel=%zu frame=%d applies=%d keys=%d history=%llu,%llu\n",
            failCopy?1u:0u,state.copies,state.bodies,state.liveBodies,state.channel,state.frame,host.applyCalls,keys,
            static_cast<unsigned long long>(after[0]),static_cast<unsigned long long>(after[1]));std::fflush(stdout);
        Check(IsWindow(window)&&ce::IsWindowSessionCurrent(window,token),
            "ordinary subdivision copy observations never close or replace the real editor");
        if(failCopy)failedCopyBlocked&=state.copies==1&&state.bodies==0&&state.liveBodies==0&&
            host.applyCalls==0&&keys==2&&host.data[0].track.keys.size()==2&&after==history;
        else successfulCopyOwned&=state.copies==1&&state.bodies==1&&state.liveBodies==0&&state.channel==0&&state.frame==10&&
            host.applyCalls==1&&keys==3&&host.data[0].track.keys.size()==3&&host.data[0].track.keys[1].frame==10&&
            host.data[0].track.keys[1].value==5&&after[0]==history[0]+1&&after[1]==0;
        state.armed=false;ce::CloseOwned(nullptr);
        Check(!IsWindow(window),"close each safe subdivision fixture before evaluating the aggregate red contract");
    }
    Check(failedCopyBlocked,"subdivision callable copy failure must stop before its body, publication and history mutation");
    Check(successfulCopyOwned,"subdivision must run one owned callable with exact channel/frame and commit its prepared native value once");
    Note("native subdivision honors its callable copy boundary before lifetime-changing tests are enabled");
}
HostSource SubdivisionHost(){
    HostSource host;host.data.push_back(MakeChannel("native",L"Native",{MakeKey(0,0),MakeKey(20,20)}));
    host.data[0].sample=[](const ce::Channel&,double frame){return ac::Sample{frame*frame/20.0,frame/10.0};};
    host.data[0].nativeControls.resize(2);return host;
}
bool PrepareNativeSubdivision(std::vector<ce::Channel>& channels,std::size_t channel,int frame,
    std::size_t& index,std::wstring& error){
    if(channel>=channels.size()){error=L"missing native subdivision channel";return false;}
    std::string detail;
    if(ac::InsertPreservingShape(channels[channel].track,frame,index,detail)!=ac::InsertResult::Inserted){
        error=L"native subdivision fixture could not insert its key";return false;}
    channels[channel].track.keys[index].value=double(frame)*frame/20.0;return true;
}
void TriggerSubdivision(HWND window,POINT point){
    SendMessageW(GetDlgItem(window,kIdCanvas),WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
}
void SeedSubdivisionRedo(HWND window){
    ClickKey(window,1);Click(window,kIdDeleteKey);Click(window,kIdUndo);
    Check(KeyCount(window)==2&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{0,1},
        "subdivision rejection fixture starts with a real retained redo node");
}
// Safe geometry Phase A changes only the width of its own canvas. It never
// closes/replaces a Session or writes host data from a provider callback.
struct SubdivisionGeometryState {
    int route=-1;bool armed=false,observing=false,parameters=true,resized=false;
    HWND canvas=nullptr;HostSource* host=nullptr;
    unsigned actions=0,sampleCopies=0,sampleBodies=0,liveSamples=0;
    unsigned subdivisionCopies=0,subdivisionBodies=0,liveSubdivisions=0,subdivisionReleases=0;
    unsigned sampleCopiesAtResize=0,sampleBodiesAtResize=0,subdivisionCopiesAtResize=0,subdivisionBodiesAtResize=0;
    unsigned paints=0,paintsAtResize=0;int readsAtResize=-1,lastFrame=-1;
    std::size_t lastChannel=static_cast<std::size_t>(-1);RECT before{},after{};
    void Resize()noexcept{
        armed=false;++actions;sampleCopiesAtResize=sampleCopies;sampleBodiesAtResize=sampleBodies;
        subdivisionCopiesAtResize=subdivisionCopies;subdivisionBodiesAtResize=subdivisionBodies;
        paintsAtResize=paints;readsAtResize=host->readCalls;
        const BOOL queried=GetClientRect(canvas,&before);
        const BOOL moved=SetWindowPos(canvas,nullptr,0,0,before.right+71,before.bottom,
            SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        const BOOL measured=GetClientRect(canvas,&after);
        resized=queried&&moved&&measured&&after.right==before.right+71&&after.bottom==before.bottom;
    }
};
struct SubdivisionGeometrySample {
    SubdivisionGeometryState* state;bool owned=false;
    explicit SubdivisionGeometrySample(SubdivisionGeometryState& value):state(&value){}
    SubdivisionGeometrySample(const SubdivisionGeometrySample& other):state(other.state),owned(state->observing){
        if(state->observing)++state->sampleCopies;
        if(state->armed&&state->route==0)state->Resize();
    }
    ac::Sample operator()(const ce::Channel& channel,double frame)const{
        if(state->observing){++state->sampleBodies;if(!owned)++state->liveSamples;
            state->parameters&=channel.id=="native"&&channel.track.keys.size()>=2&&
                channel.track.keys.front().frame==0&&channel.track.keys.back().frame==20;
            for(const auto& key:channel.track.keys)state->parameters&=key.value==7;}
        if(state->armed&&state->route==1)state->Resize();
        return ac::Evaluate(channel.track,frame);
    }
};
struct SubdivisionGeometryProvider {
    SubdivisionGeometryState* state;bool owned=false;mutable bool invoked=false;
    explicit SubdivisionGeometryProvider(SubdivisionGeometryState& value):state(&value){}
    SubdivisionGeometryProvider(const SubdivisionGeometryProvider& other):state(other.state),owned(state->observing){
        if(state->observing)++state->subdivisionCopies;
        if(state->armed&&state->route==2)state->Resize();
    }
    ~SubdivisionGeometryProvider(){
        if(state->observing&&owned&&invoked)++state->subdivisionReleases;
        if(state->armed&&state->route==4&&owned&&invoked)state->Resize();
    }
    bool operator()(std::vector<ce::Channel>& channels,std::size_t channel,int frame,
        std::size_t& index,std::wstring& error)const{
        invoked=true;if(state->observing){++state->subdivisionBodies;if(!owned)++state->liveSubdivisions;
            state->lastChannel=channel;state->lastFrame=frame;}
        const bool valid=channel<channels.size()&&channel==0&&channels[channel].id=="native"&&frame>0&&frame<20;
        state->parameters&=valid;if(!valid){error=L"flat native geometry fixture received a different target";return false;}
        const auto nativeValue=ac::Evaluate(channels[channel].track,frame);std::string detail;
        if(ac::InsertPreservingShape(channels[channel].track,frame,index,detail)!=ac::InsertResult::Inserted){
            error=L"flat native geometry fixture could not prepare its key";return false;}
        channels[channel].track.keys[index].value=nativeValue.value;
        state->parameters&=index<channels[channel].track.keys.size()&&channels[channel].track.keys[index].frame==frame&&
            channels[channel].track.keys[index].value==7&&nativeValue.derivative==0;
        if(state->armed&&state->route==3)state->Resize();return true;
    }
};
struct SubdivisionGeometryPaintCounter {
    SubdivisionGeometryState& state;
    static LRESULT CALLBACK Observe(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
        if(m==WM_PAINT)++reinterpret_cast<SubdivisionGeometryState*>(data)->paints;return DefSubclassProc(h,m,w,l);
    }
    explicit SubdivisionGeometryPaintCounter(SubdivisionGeometryState& value):state(value){
        Check(SetWindowSubclass(state.canvas,Observe,0x73c9,reinterpret_cast<DWORD_PTR>(&state))!=FALSE,
            "observe paint messages of only the safe geometry fixture's own canvas");
    }
    ~SubdivisionGeometryPaintCounter(){if(IsWindow(state.canvas))RemoveWindowSubclass(state.canvas,Observe,0x73c9);}
};
HostSource SubdivisionGeometryHost(SubdivisionGeometryState& state){
    auto host=SubdivisionHost();for(auto& key:host.data[0].track.keys){key.value=7;key.interpolation=ac::Interpolation::Linear;key.tangent=ac::Tangent::Free;key.inSlope=key.outSlope=0;}
    host.data[0].sample=SubdivisionGeometrySample(state);return host;
}
bool SubdivisionGeometryWriterBoundary(){
    bool all=true;
    // Once apply has been entered, a view-only change must not undo, retry or
    // hide an accepted data write. Post-accept canonical reads share that rule.
    for(int phase=0;phase<3;++phase){
        std::printf("subdivision writer geometry phase=%d\n",phase);std::fflush(stdout);
        SubdivisionGeometryState state;state.route=6;auto host=SubdivisionGeometryHost(state);state.host=&host;
        unsigned writerCalls=0,acceptedWrites=0,postWriteReads=0;bool writerAccepted=false,inputValid=true;
        auto session=MakeSession(host);const auto apply=session.sources[0].apply;const auto read=session.sources[0].read;
        session.sources[0].subdivide=SubdivisionGeometryProvider(state);
        session.sources[0].apply=[&,apply](const auto& channels,auto& error){
            const bool probe=state.observing;
            if(probe){++writerCalls;inputValid&=channels.size()==1&&channels[0].id=="native"&&channels[0].track.keys.size()==3;
                if(inputValid){const auto& keys=channels[0].track.keys;inputValid&=keys[0].frame==0&&keys[1].frame==10&&keys[2].frame==20&&
                    keys[0].value==7&&keys[1].value==7&&keys[2].value==7;}
                if(state.armed&&phase==0)state.Resize();}
            const bool accepted=apply(channels,error);
            if(probe&&accepted){++acceptedWrites;writerAccepted=true;if(state.armed&&phase==1)state.Resize();}
            return accepted;
        };
        session.sources[0].read=[&,read](auto& channels,auto& error){
            const bool accepted=read(channels,error);
            if(state.observing&&writerAccepted){++postWriteReads;if(accepted&&state.armed&&phase==2)state.Resize();}
            return accepted;
        };
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"writer geometry boundary opens its original native two-key source");
        state.canvas=GetDlgItem(window,kIdCanvas);Check(IsWindow(state.canvas),"writer geometry boundary owns its canvas");
        SubdivisionGeometryPaintCounter paints(state);ShowWindow(window,SW_SHOWNOACTIVATE);
        Check(IsWindowVisible(window)&&IsWindowVisible(state.canvas),"writer boundary paints only its own visible windows");
        UpdateWindow(window);UpdateWindow(state.canvas);ClickKey(window,0);Click(window,kIdDeleteKey);Click(window,kIdUndo);
        Check(KeyCount(window)==2&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{0,1},
            "writer boundary starts with the original native data and one real redo");
        const auto data=host.data;const int applies=host.applyCalls;const auto token=ce::CaptureWindowSession(window);
        const POINT point=AskPoint(window,kHookPointForFrame,10);UpdateWindow(state.canvas);ValidateRect(state.canvas,nullptr);
        state.armed=state.observing=true;TriggerSubdivision(window,point);state.armed=state.observing=false;
        const auto history=LocalWriteHistory(window);
        const bool exact=host.data.size()==1&&host.data[0].id=="native"&&host.data[0].track.keys.size()==3&&
            host.data[0].track.keys[0].frame==0&&host.data[0].track.keys[1].frame==10&&host.data[0].track.keys[2].frame==20&&
            host.data[0].track.keys[0].value==7&&host.data[0].track.keys[1].value==7&&host.data[0].track.keys[2].value==7;
        const auto acceptedData=host.data;
        const bool committed=ce::IsWindowSessionCurrent(window,token)&&state.actions==1&&state.resized&&state.parameters&&inputValid&&
            writerCalls==1&&acceptedWrites==1&&postWriteReads==1&&host.applyCalls==applies+1&&KeyCount(window)==3&&exact&&
            history==std::array<std::uint64_t,2>{1,0};
        ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);UpdateWindow(state.canvas);
        const bool noRetry=host.applyCalls==applies+1&&LocalWriteHistory(window)==history&&SameWritePayload(host.data,acceptedData);
        const int beforeUndo=host.applyCalls;Click(window,kIdUndo);
        const bool undone=host.applyCalls==beforeUndo+1&&KeyCount(window)==2&&SameWritePayload(host.data,data)&&
            LocalWriteHistory(window)==std::array<std::uint64_t,2>{0,1};
        Click(window,kIdRedo);
        const bool redone=host.applyCalls==beforeUndo+2&&KeyCount(window)==3&&SameWritePayload(host.data,acceptedData)&&
            LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0};
        std::printf("subdivision writer observation phase=%d actions=%u resized=%u writers=%u accepted=%u canonical_reads=%u width=%ld,%ld height=%ld,%ld history=%llu,%llu committed=%u no_retry=%u undo=%u redo=%u parameters=%u\n",
            phase,state.actions,unsigned(state.resized),writerCalls,acceptedWrites,postWriteReads,state.before.right,state.after.right,state.before.bottom,state.after.bottom,
            static_cast<unsigned long long>(history[0]),static_cast<unsigned long long>(history[1]),unsigned(committed),unsigned(noRetry),unsigned(undone),unsigned(redone),unsigned(inputValid&&state.parameters));
        std::fflush(stdout);all&=committed&&noRetry&&undone&&redone;
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    return all;
}
struct SubdivisionWriterCopyFixture {
    bool external;
    SubdivisionGeometryState geometry;
    HostSource local;
    ExternalCurveJournal journal;
    unsigned copies=0,bodies=0,liveBodies=0;bool inputValid=true;
    explicit SubdivisionWriterCopyFixture(bool value):external(value),local(SubdivisionGeometryHost(geometry)){
        geometry.route=6;
        if(external)journal.sources[0].data=std::move(local.data);
        geometry.host=external?&journal.sources[0]:&local;
    }
    const std::vector<ce::Channel>& Data()const{return external?journal.sources[0].data:local.data;}
    unsigned Applies()const{return external?journal.applies:static_cast<unsigned>(local.applyCalls);}
    std::array<std::uint64_t,2> History(HWND window)const{
        if(!external)return LocalWriteHistory(window);
        return {static_cast<std::uint64_t>(journal.cursor),static_cast<std::uint64_t>(journal.nodes.size()-journal.cursor)};
    }
    bool ExactInsertion()const{
        const auto& data=Data();
        return data.size()==1&&data[0].id=="native"&&data[0].track.keys.size()==3&&
            data[0].track.keys[0].frame==0&&data[0].track.keys[1].frame==10&&data[0].track.keys[2].frame==20&&
            data[0].track.keys[0].value==7&&data[0].track.keys[1].value==7&&data[0].track.keys[2].value==7;
    }
};
struct SubdivisionWriterCopyProvider {
    SubdivisionWriterCopyFixture* fixture;
    decltype(ce::Source::apply) apply;
    bool owned=false;
    SubdivisionWriterCopyProvider(SubdivisionWriterCopyFixture& value,decltype(ce::Source::apply) callback)
        :fixture(&value),apply(std::move(callback)){}
    SubdivisionWriterCopyProvider(const SubdivisionWriterCopyProvider& other)
        :fixture(other.fixture),apply(other.apply),owned(fixture->geometry.observing){
        if(fixture->geometry.observing)++fixture->copies;
        // The writer body has not been dispatched. Only copying its private
        // std::function target changes the existing canvas; the Session lives.
        if(fixture->geometry.armed)fixture->geometry.Resize();
    }
    bool operator()(const std::vector<ce::Channel>& channels,std::wstring& error)const{
        if(fixture->geometry.observing){
            ++fixture->bodies;if(!owned)++fixture->liveBodies;
            bool exact=channels.size()==1&&channels[0].id=="native"&&channels[0].track.keys.size()==3;
            if(exact){const auto& keys=channels[0].track.keys;exact=keys[0].frame==0&&keys[1].frame==10&&keys[2].frame==20&&
                keys[0].value==7&&keys[1].value==7&&keys[2].value==7;}
            fixture->inputValid&=exact;
        }
        return apply(channels,error);
    }
};
void SubdivisionWriterCopySafe(){
    struct Observation {
        bool external=false,armed=false,current=false,resized=false,unchanged=false,journalUnchanged=false;
        bool parameters=false,owned=false,redo=false,recovery=false,normal=false,pending=false,noPaint=false,repaint=false;
        unsigned copies=0,bodies=0,applies=0,actions=0;int keys=0,frame=-1;
        std::array<std::uint64_t,2> history{};
    };
    std::vector<Observation> observations;
    for(bool external:{false,true})for(bool armed:{false,true}){
        std::printf("subdivision writer copy safe external=%u armed=%u\n",unsigned(external),unsigned(armed));std::fflush(stdout);
        SubdivisionWriterCopyFixture fixture(external);auto& state=fixture.geometry;
        auto session=external?fixture.journal.Session():MakeSession(fixture.local);
        const std::string sourceId=session.sources[0].id;
        session.sources[0].subdivide=SubdivisionGeometryProvider(state);
        session.sources[0].apply=SubdivisionWriterCopyProvider(fixture,std::move(session.sources[0].apply));
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"writer-copy geometry fixture opens its original native source unarmed");
        if(external)fixture.journal.window=window;
        state.canvas=GetDlgItem(window,kIdCanvas);Check(IsWindow(state.canvas),"writer-copy geometry fixture owns its canvas");
        SubdivisionGeometryPaintCounter paints(state);ShowWindow(window,SW_SHOWNOACTIVATE);
        Check(IsWindowVisible(window)&&IsWindowVisible(state.canvas),"writer-copy geometry fixture paints only its own visible windows");
        UpdateWindow(window);UpdateWindow(state.canvas);
        ClickKey(window,0);Click(window,kIdDeleteKey);Click(window,kIdUndo);
        const auto history=fixture.History(window);
        Check(KeyCount(window)==2&&history==std::array<std::uint64_t,2>{0,1}&&IsWindowEnabled(GetDlgItem(window,kIdRedo)),
            "writer-copy geometry fixture retains a real local or shared redo node before arming");
        if(external)fixture.journal.NoLocalHistory();
        const auto data=fixture.Data();const auto otherSource=fixture.journal.sources[1].data;
        const auto journalNodes=fixture.journal.nodes;
        const auto cursor=fixture.journal.cursor;const auto revision=fixture.journal.revision;
        const unsigned traversals=fixture.journal.traversals,applies=fixture.Applies();
        const auto token=ce::CaptureWindowSession(window);const POINT point=AskPoint(window,kHookPointForFrame,10);
        double sampled[3]{10,-1234,-5678};
        Check(point.x>0&&point.y>0&&SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(sampled))&&sampled[1]==7&&sampled[2]==0,
            "writer-copy probe targets the actual flat continuous native curve through the read-only geometry hook");
        UpdateWindow(state.canvas);ValidateRect(state.canvas,nullptr);
        state.observing=true;state.armed=armed;TriggerSubdivision(window,point);state.armed=state.observing=false;
        Observation entry;entry.external=external;entry.armed=armed;entry.current=ce::IsWindowSessionCurrent(window,token);
        entry.actions=state.actions;entry.resized=state.resized;entry.copies=fixture.copies;entry.bodies=fixture.bodies;
        entry.applies=fixture.Applies()-applies;entry.keys=KeyCount(window);entry.history=fixture.History(window);entry.frame=state.lastFrame;
        entry.parameters=state.parameters&&fixture.inputValid;entry.owned=!fixture.liveBodies&&!state.liveSamples&&!state.liveSubdivisions;
        entry.journalUnchanged=fixture.journal.cursor==cursor&&fixture.journal.revision==revision&&fixture.journal.traversals==traversals&&
            fixture.journal.nodes.size()==journalNodes.size()&&SameWritePayload(fixture.journal.sources[1].data,otherSource);
        if(entry.journalUnchanged)for(std::size_t n=0;n<journalNodes.size();++n){const auto& before=journalNodes[n];const auto& after=fixture.journal.nodes[n];
            entry.journalUnchanged&=before.source==after.source&&SameWritePayload(before.before,after.before)&&SameWritePayload(before.after,after.after);}
        entry.unchanged=entry.applies==0&&entry.bodies==0&&entry.keys==2&&entry.history==history&&SameWritePayload(fixture.Data(),data)&&
            IsWindowEnabled(GetDlgItem(window,kIdRedo))&&entry.journalUnchanged;
        entry.normal=!armed&&!state.actions&&entry.copies==1&&entry.bodies==1&&entry.applies==1&&entry.keys==3&&
            entry.history==std::array<std::uint64_t,2>{1,0}&&fixture.ExactInsertion()&&fixture.journal.traversals==traversals&&
            (!external||(fixture.journal.cursor==1&&fixture.journal.nodes.size()==1&&fixture.journal.revision==revision+1));
        entry.pending=GetUpdateRect(state.canvas,nullptr,FALSE)!=FALSE;entry.noPaint=state.paints==state.paintsAtResize;
        std::printf("subdivision writer copy observation external=%u armed=%u current=%u copies=%u bodies=%u live_bodies=%u actions=%u resized=%u width=%ld,%ld height=%ld,%ld applies=%u keys=%d frame=%d history=%llu,%llu cursor=%zu revision=%llu nodes=%zu traversals=%u unchanged=%u journal_unchanged=%u normal=%u pending=%u no_paint=%u parameters=%u owned=%u\n",
            unsigned(external),unsigned(armed),unsigned(entry.current),entry.copies,entry.bodies,fixture.liveBodies,entry.actions,unsigned(entry.resized),
            state.before.right,state.after.right,state.before.bottom,state.after.bottom,entry.applies,entry.keys,entry.frame,
            static_cast<unsigned long long>(entry.history[0]),static_cast<unsigned long long>(entry.history[1]),fixture.journal.cursor,
            static_cast<unsigned long long>(fixture.journal.revision),fixture.journal.nodes.size(),fixture.journal.traversals,
            unsigned(entry.unchanged),unsigned(entry.journalUnchanged),unsigned(entry.normal),unsigned(entry.pending),unsigned(entry.noPaint),unsigned(entry.parameters),unsigned(entry.owned));
        std::fflush(stdout);
        const auto beforePaint=state.paints,beforeBodies=state.sampleBodies,beforePaintApplies=fixture.Applies();
        const auto beforePaintHistory=fixture.History(window);const auto beforePaintData=fixture.Data();
        state.observing=true;UpdateWindow(state.canvas);state.observing=false;
        entry.repaint=state.paints>beforePaint&&state.sampleBodies>beforeBodies&&fixture.Applies()==beforePaintApplies&&
            fixture.History(window)==beforePaintHistory&&SameWritePayload(fixture.Data(),beforePaintData);
        if(armed&&entry.unchanged){
            const auto beforeRedo=fixture.Applies(),beforeTraversal=fixture.journal.traversals;
            Click(window,kIdRedo);
            entry.redo=KeyCount(window)==1&&fixture.History(window)==std::array<std::uint64_t,2>{1,0}&&
                (external?fixture.Applies()==beforeRedo&&fixture.journal.traversals==beforeTraversal+1:fixture.Applies()==beforeRedo+1);
            Click(window,kIdUndo);
            entry.redo&=KeyCount(window)==2&&fixture.History(window)==history&&SameWritePayload(fixture.Data(),data)&&
                (external?fixture.Applies()==beforeRedo&&fixture.journal.traversals==beforeTraversal+2:fixture.Applies()==beforeRedo+2);
        }else if(entry.applies==1){
            // Retain every old-code observation, then use genuine undo so one
            // failing route cannot prevent collecting the next safe fixture.
            Click(window,kIdUndo);
        }
        const auto beforeRecovery=fixture.Applies();const auto beforeRecoveryTraversal=fixture.journal.traversals;
        const POINT fresh=AskPoint(window,kHookPointForFrame,10);TriggerSubdivision(window,fresh);
        entry.recovery=ce::IsWindowSessionCurrent(window,token)&&fixture.Applies()==beforeRecovery+1&&fixture.ExactInsertion()&&KeyCount(window)==3&&
            fixture.History(window)==std::array<std::uint64_t,2>{1,0}&&fixture.journal.traversals==beforeRecoveryTraversal;
        ce::NotifySourceChanged(nullptr,sourceId);SendMessageW(window,WM_APP+401,0,0);UpdateWindow(state.canvas);
        entry.recovery&=fixture.Applies()==beforeRecovery+1&&fixture.ExactInsertion()&&fixture.History(window)==std::array<std::uint64_t,2>{1,0};
        if(external)fixture.journal.NoLocalHistory();
        std::printf("subdivision writer copy recovery external=%u armed=%u redo=%u repaint=%u recovery=%u\n",
            unsigned(external),unsigned(armed),unsigned(entry.redo),unsigned(entry.repaint),unsigned(entry.recovery));std::fflush(stdout);
        observations.push_back(entry);ce::CloseOwned(nullptr);
    }
    const bool writerBoundary=SubdivisionGeometryWriterBoundary();
    bool controls=observations.size()==4,changed=true,rejected=true,recovered=true;
    for(const auto& entry:observations){
        controls&=entry.current&&entry.parameters&&entry.owned&&entry.frame==10;
        if(!entry.armed)controls&=entry.normal;
        else{changed&=entry.actions==1&&entry.resized&&entry.copies==1;
            rejected&=entry.unchanged&&entry.redo;}
        recovered&=entry.recovery;
    }
    Check(controls,"independent local and real external-journal controls each dispatch one owned writer for the exact native frame/value");
    Check(changed,"late writer-copy probes change only their original canvas width while retaining the same Session");
    Check(rejected,"writer callable copy geometry changes stop before its body and preserve canonical data, complete history and usable local/shared redo");
    Check(recovered,"disarmed writer-copy fixtures insert once with fresh geometry and deferred refresh never retries their publication");
    Check(writerBoundary,"the three existing writer-body and accepted-read geometry changes still commit once with complete undo and redo");
    Note("safe subdivision writer-copy geometry preserves local and shared history before writer dispatch");
}
void SubdivisionGeometrySafe(){
    struct Observation {
        int route=-1,applies=0,keys=0,frame=-1;std::array<std::uint64_t,2> history{};
        bool current=false,resized=false,unchanged=false,stopped=false,pending=false,noPaint=false;
        bool parameters=false,owned=false,redo=false,recovery=false,repaint=false;
        unsigned actions=0,sampleCopies=0,sampleBodies=0,subdivisionCopies=0,subdivisionBodies=0;
    };
    std::vector<Observation> observations;
    // The first independent, unarmed fixture proves the physical flat-curve
    // point reaches the real subdivision/apply/history path before any red case.
    for(int route=-1;route<6;++route){
        std::printf("subdivision geometry safe route=%d\n",route);std::fflush(stdout);
        SubdivisionGeometryState state;state.route=route;auto host=SubdivisionGeometryHost(state);state.host=&host;
        auto session=MakeSession(host);session.sources[0].subdivide=SubdivisionGeometryProvider(state);const auto read=session.sources[0].read;
        session.sources[0].read=[&,read](auto& channels,auto& error){
            const bool accepted=read(channels,error);
            if(accepted&&state.armed&&state.route==5)state.Resize();return accepted;
        };
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"safe geometry probe opens its real two-key native source");
        state.canvas=GetDlgItem(window,kIdCanvas);Check(IsWindow(state.canvas),"safe geometry probe owns its native canvas");
        SubdivisionGeometryPaintCounter paints(state);ShowWindow(window,SW_SHOWNOACTIVATE);
        Check(IsWindowVisible(window)&&IsWindowVisible(state.canvas),"safe geometry damage evidence uses visible owned windows");
        UpdateWindow(window);UpdateWindow(state.canvas);
        // The right endpoint lies on the value-bar boundary in this flat
        // fitted view. Select the left key through the real canvas instead.
        ClickKey(window,0);Click(window,kIdDeleteKey);Click(window,kIdUndo);
        const auto seeded=LocalWriteHistory(window);
        Check(KeyCount(window)==2&&seeded==std::array<std::uint64_t,2>{0,1},"safe geometry fixture starts with a real retained redo node");
        const POINT point=AskPoint(window,kHookPointForFrame,10);double sampled[3]{10,-1234,-5678};
        Check(point.x>0&&point.y>0&&SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(sampled))&&sampled[1]==7&&sampled[2]==0,
            "safe geometry probe targets the actual horizontal continuous native curve");
        UpdateWindow(state.canvas);ValidateRect(state.canvas,nullptr);
        const auto token=ce::CaptureWindowSession(window);const auto history=LocalWriteHistory(window);const auto data=host.data;
        const int applies=host.applyCalls;state.observing=true;state.armed=route>=0;TriggerSubdivision(window,point);state.armed=state.observing=false;
        Observation entry;entry.route=route;entry.applies=host.applyCalls-applies;entry.keys=KeyCount(window);
        entry.history=LocalWriteHistory(window);entry.frame=state.lastFrame;entry.actions=state.actions;
        entry.sampleCopies=state.sampleCopies;entry.sampleBodies=state.sampleBodies;
        entry.subdivisionCopies=state.subdivisionCopies;entry.subdivisionBodies=state.subdivisionBodies;
        entry.current=ce::IsWindowSessionCurrent(window,token);entry.resized=state.resized;
        entry.unchanged=SameWritePayload(host.data,data)&&entry.history==history&&entry.keys==2&&entry.applies==0;
        entry.stopped=state.sampleCopies==state.sampleCopiesAtResize&&state.sampleBodies==state.sampleBodiesAtResize&&
            state.subdivisionCopies==state.subdivisionCopiesAtResize&&state.subdivisionBodies==state.subdivisionBodiesAtResize&&host.readCalls==state.readsAtResize;
        entry.pending=GetUpdateRect(state.canvas,nullptr,FALSE)!=FALSE;entry.noPaint=state.paints==state.paintsAtResize;
        entry.parameters=state.parameters;entry.owned=!state.liveSamples&&!state.liveSubdivisions;
        std::printf("subdivision geometry observation route=%d actions=%u resized=%u width=%ld,%ld height=%ld,%ld sample_copies=%u sample_bodies=%u subdivide_copies=%u subdivide_bodies=%u subdivide_releases=%u frame=%d applies=%d keys=%d history=%llu,%llu unchanged=%u stopped=%u pending=%u no_paint=%u parameters=%u owned=%u\n",
            route,state.actions,unsigned(entry.resized),state.before.right,state.after.right,state.before.bottom,state.after.bottom,
            state.sampleCopies,state.sampleBodies,state.subdivisionCopies,state.subdivisionBodies,state.subdivisionReleases,entry.frame,entry.applies,entry.keys,
            static_cast<unsigned long long>(entry.history[0]),static_cast<unsigned long long>(entry.history[1]),unsigned(entry.unchanged),unsigned(entry.stopped),
            unsigned(entry.pending),unsigned(entry.noPaint),unsigned(entry.parameters),unsigned(entry.owned));std::fflush(stdout);
        const auto beforePaint=state.paints,beforeBodies=state.sampleBodies;const int beforePaintApplies=host.applyCalls;
        const auto beforePaintHistory=LocalWriteHistory(window);state.observing=true;UpdateWindow(state.canvas);state.observing=false;
        entry.repaint=state.paints>beforePaint&&state.sampleBodies>beforeBodies&&host.applyCalls==beforePaintApplies&&LocalWriteHistory(window)==beforePaintHistory;
        if(route>=0&&entry.unchanged){
            const int beforeRedo=host.applyCalls;Click(window,kIdRedo);
            entry.redo=host.applyCalls==beforeRedo+1&&KeyCount(window)==1&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0};
            Click(window,kIdUndo);entry.redo&=host.applyCalls==beforeRedo+2&&KeyCount(window)==2&&
                LocalWriteHistory(window)==history&&SameWritePayload(host.data,data);
        }else if(entry.applies>0){
            // Restore through the real editor history only, so an old-code red
            // observation does not prevent collecting the later recovery case.
            Click(window,kIdUndo);
        }
        const int beforeRecovery=host.applyCalls;const POINT fresh=AskPoint(window,kHookPointForFrame,10);TriggerSubdivision(window,fresh);
        entry.recovery=ce::IsWindowSessionCurrent(window,token)&&host.applyCalls==beforeRecovery+1&&KeyCount(window)==3&&
            host.data.size()==1&&host.data[0].id=="native"&&host.data[0].track.keys.size()==3&&
            host.data[0].track.keys[0].frame==0&&host.data[0].track.keys[1].frame==10&&host.data[0].track.keys[2].frame==20&&
            host.data[0].track.keys[0].value==7&&host.data[0].track.keys[1].value==7&&host.data[0].track.keys[2].value==7&&
            LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0};
        std::printf("subdivision geometry recovery route=%d redo=%u repaint=%u recovery=%u\n",route,unsigned(entry.redo),unsigned(entry.repaint),unsigned(entry.recovery));std::fflush(stdout);
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();observations.push_back(entry);
    }
    const bool writerBoundary=SubdivisionGeometryWriterBoundary();const auto& baseline=observations.front();
    Check(observations.size()==7&&baseline.current&&baseline.parameters&&baseline.owned&&!baseline.actions&&
        baseline.applies==1&&baseline.keys==3&&baseline.frame==10&&baseline.history==std::array<std::uint64_t,2>{1,0}&&baseline.recovery,
        "an independent unarmed flat native fixture really inserts frame 10 and commits one complete history step");
    bool changed=true,unchanged=true,stopped=true,redraw=true,recovered=true;
    for(std::size_t i=1;i<observations.size();++i){const auto& entry=observations[i];
        changed&=entry.current&&entry.actions==1&&entry.resized&&entry.parameters&&entry.owned;
        unchanged&=entry.unchanged&&entry.redo;stopped&=entry.stopped;
        redraw&=entry.pending&&entry.noPaint&&entry.repaint;recovered&=entry.recovery;
    }
    Check(changed,"every safe geometry route resizes only its original canvas width once and retains exact native parameters");
    Check(unchanged,"same-session geometry changes stop native insertion before apply and retain the original canonical data and usable redo");
    Check(stopped,"geometry retirement prevents every later sampler, subdivision callback and source preflight from continuing");
    Check(redraw,"geometry retirement queues a normal later paint without recursively painting inside the provider callback");
    Check(recovered,"disarmed insertion rereads the fresh geometry and commits its exact native target and value once");
    Check(writerBoundary,"a view change after writer dispatch retains exactly one real data/history commit and one complete undo/redo without retrying apply");
    Note("safe native insertion geometry covers sampler copy/body, subdivision copy/body/final release, preflight read and the accepted writer boundary");
}
struct SubdivisionRetirementState {
    int phase=0,response=0;bool armed=false,copying=false;
    unsigned copies=0,bodies=0,actions=0,destroyedDuringCopy=0;
    const void* original=nullptr;std::function<void()> action;
    void Run(){armed=false;++actions;action();}
    void ThrowResponse()const{
        if(response==2)throw std::runtime_error("native subdivision unavailable");
        if(response==3)throw std::bad_alloc();
    }
};
struct SubdivisionRetirementProvider {
    SubdivisionRetirementState* state;int marker=137;
    bool invocationCopy=false;mutable bool invoked=false;
    explicit SubdivisionRetirementProvider(SubdivisionRetirementState& value):state(&value){}
    SubdivisionRetirementProvider(const SubdivisionRetirementProvider& other):state(other.state),marker(other.marker){
        invocationCopy=state->armed;if(!invocationCopy)return;++state->copies;
        if(state->phase!=0)return;
        state->copying=true;state->original=&other;state->Run();
        Check(state->destroyedDuringCopy==0&&other.marker==137,
            "subdivision original callable remains alive throughout a retiring copy constructor");
        state->copying=false;state->ThrowResponse();
    }
    ~SubdivisionRetirementProvider(){
        if(state->copying&&state->original==this)++state->destroyedDuringCopy;
        if(invocationCopy&&invoked&&state->armed&&state->phase==2)state->Run();
    }
    bool operator()(std::vector<ce::Channel>& channels,std::size_t channel,int frame,
        std::size_t& index,std::wstring& error)const{
        invoked=true;++state->bodies;
        Check(invocationCopy&&channel==0&&frame==10&&channels.size()==1&&channels[0].id=="native",
            "subdivision receives one owned callable, private channels and exact channel/frame values");
        Check(PrepareNativeSubdivision(channels,channel,frame,index,error),
            "retiring subdivision prepares its result only in the detached input");
        if(state->armed&&state->phase==1)state->Run();
        Check(marker==137&&channel==0&&frame==10&&index==1&&channels[0].track.keys.size()==3&&
            channels[0].track.keys[1].frame==10&&channels[0].track.keys[1].value==5&&
            channels[0].sample(channels[0],10).value==5,
            "retirement cannot invalidate subdivision parameters or their retained original Channel sampler");
        state->ThrowResponse();if(state->response==1)error=L"native subdivision refused";
        return state->response!=1;
    }
};
void SubdivisionCallbackLifetime(){
    for(int phase=0;phase!=3;++phase)for(int action=0;action!=3;++action)for(int response=0;response!=4;++response){
        std::printf("subdivision lifetime phase=%d action=%d response=%d\n",phase,action,response);std::fflush(stdout);
        auto host=SubdivisionHost();HostSource replacement;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        SubdivisionRetirementState state;state.phase=phase;state.response=response;
        auto session=MakeSession(host);session.sources[0].subdivide=SubdivisionRetirementProvider(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"subdivision callback lifetime opens before its fault is armed");
        const POINT point=AskPoint(window,kHookPointForFrame,10);
        const auto token=ce::CaptureWindowSession(window);const auto retained=token.lock();
        HWND next=nullptr;std::wstring status,field;int readsAfter=-1;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"subdivision retirement installs a real replacement Session");
                if(action==1)Check(next==window,"subdivision same-owner Open keeps its native HWND");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
            readsAfter=host.readCalls;
        };
        state.armed=true;TriggerSubdivision(window,point);
        Check(state.actions==1&&state.copies==1&&state.bodies==unsigned(phase!=0)&&state.destroyedDuringCopy==0,
            "subdivision copy, body or final release retires once without retrying the provider");
        Check(retained&&!ce::IsWindowSessionCurrent(window,token)&&host.readCalls==readsAfter&&host.applyCalls==0&&
            host.data.size()==1&&host.data[0].track.keys.size()==2,
            "a retired subdivision never publishes its detached result or continues reading the old source");
        if(next){
            Check(KeyCount(next)==2&&replacement.applyCalls==0&&GetText(next,kIdStatus)==status&&
                GetText(next,kIdFrameEdit)==field&&LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},
                "retired subdivision cannot change replacement fields, status, selection history or curve data");
            ClickKey(next,1);Click(next,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(next)==1,
                "replacement Session remains editable after every old subdivision guard unwinds");
        }else Check(OwnedCurveWindow()==nullptr,"closed subdivision Session leaves no registered editor");
        state.armed=false;ce::CloseOwned(nullptr);
    }
    Note("native subdivision retains private parameters across callable copy, body and final-release retirement");
}

struct SubdivisionFailureState {bool armed=false;int failure=0,copies=0,bodies=0;};
struct SubdivisionFailureProvider {
    SubdivisionFailureState* state;
    explicit SubdivisionFailureProvider(SubdivisionFailureState& value):state(&value){}
    SubdivisionFailureProvider(const SubdivisionFailureProvider& other):state(other.state){
        if(!state->armed)return;++state->copies;
        if(state->failure==0){state->armed=false;throw std::bad_alloc();}
        if(state->failure==1){state->armed=false;throw std::runtime_error("subdivision copy failed");}
    }
    bool operator()(std::vector<ce::Channel>& channels,std::size_t channel,int frame,
        std::size_t& index,std::wstring& error)const{
        ++state->bodies;const int failure=state->armed?state->failure:-1;state->armed=false;
        if(!PrepareNativeSubdivision(channels,channel,frame,index,error))return false;
        if(failure==2){error=L"subdivision prepared but declined";return false;}
        if(failure==3)throw std::runtime_error("subdivision body failed");
        if(failure==4)throw std::bad_alloc();
        if(failure==5)index=channels[channel].track.keys.size();
        if(failure==6)channels[channel].id="wrong target";
        if(failure==7)channels[channel].track.keys[index].frame=11;
        if(failure==8)g_curveFailNextAllocation=true;
        return true;
    }
};
void SubdivisionFailuresKeepRedo(){
    for(int failure=0;failure!=9;++failure){
        auto host=SubdivisionHost();SubdivisionFailureState state;state.failure=failure;
        auto session=MakeSession(host);session.sources[0].subdivide=SubdivisionFailureProvider(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));SeedSubdivisionRedo(window);
        const auto history=LocalWriteHistory(window);const auto data=host.data;const int applies=host.applyCalls;
        const auto token=ce::CaptureWindowSession(window);const auto failures=g_curveAllocationFailures;
        const POINT point=AskPoint(window,kHookPointForFrame,10);state.armed=true;TriggerSubdivision(window,point);
        Check(!g_curveFailNextAllocation&&state.copies==1&&state.bodies==int(failure>=2)&&
            ce::IsWindowSessionCurrent(window,token)&&host.applyCalls==applies&&KeyCount(window)==2&&
            SameWritePayload(host.data,data)&&LocalWriteHistory(window)==history,
            "failed or invalid subdivision keeps the original data and retained redo without invoking apply");
        if(failure==8)Check(g_curveAllocationFailures==failures+1,
            "the real pre-publication allocation failure is consumed while preparing insertion selection");
        Click(window,kIdRedo);
        Check(host.applyCalls==applies+1&&KeyCount(window)==1&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},
            "a rejected subdivision leaves its original redo node usable exactly once");
        ce::CloseOwned(nullptr);
    }
    Note("native subdivision copy, refusal, exception, OOM and invalid-result failures preserve redo and data");
}

void SubdivisionAcceptedRecovery(){
    for(int failure=0;failure!=3;++failure){
        auto host=SubdivisionHost();bool armed=false;int calls=0;
        auto session=MakeSession(host);const auto apply=session.sources[0].apply;
        session.sources[0].subdivide=[&](auto& channels,std::size_t channel,int frame,std::size_t& index,auto& error){
            ++calls;return PrepareNativeSubdivision(channels,channel,frame,index,error);};
        session.sources[0].apply=[&,apply](const auto& channels,auto& error){
            const bool accepted=apply(channels,error);
            if(armed&&accepted){
                if(failure==0)host.failRead=true;
                else if(failure==1)g_curveFailNextAllocation=true;
            }
            return accepted;
        };
        bool failUi=false;session.sources[0].writeKeySelection=[&](const auto&,const auto&){
            if(failUi){failUi=false;throw std::runtime_error("accepted subdivision selection display failed");}};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        const POINT point=AskPoint(window,kHookPointForFrame,10);const auto failures=g_curveAllocationFailures;
        armed=true;failUi=failure==2;TriggerSubdivision(window,point);armed=false;
        Check(!g_curveFailNextAllocation&&calls==1&&host.applyCalls==1&&KeyCount(window)==3&&
            host.data[0].track.keys.size()==3&&host.data[0].track.keys[1].value==5&&
            LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},
            "accepted subdivision retains its prepared key and one history node through canonical or UI failure");
        if(failure==1)Check(g_curveAllocationFailures==failures+1,
            "accepted subdivision consumes one real post-publication allocation failure");
        host.failRead=false;ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(calls==1&&host.applyCalls==1&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},
            "read-only recovery never retries native subdivision or duplicates its accepted apply");
        Click(window,kIdUndo);Check(host.applyCalls==2&&KeyCount(window)==2,
            "accepted subdivision remains one undo step after display recovery");
        ce::CloseOwned(nullptr);
    }
    Note("accepted native subdivision survives canonical read, allocation and selection display failure without retry");
}
struct SubdivisionSampleState {
    int phase=0,response=0;bool armed=false,copying=false;
    unsigned copies=0,bodies=0,actions=0,destroyedDuringCopy=0;
    const void* original=nullptr;std::function<void()> action;
    void Run(){armed=false;++actions;action();}
    void ThrowResponse()const{
        if(response==1)throw std::runtime_error("insertion sampler failed");
        if(response==2)throw std::bad_alloc();
    }
};
struct SubdivisionSample {
    SubdivisionSampleState* state;int marker=173;bool privateCopy=false;
    explicit SubdivisionSample(SubdivisionSampleState& value):state(&value){}
    SubdivisionSample(const SubdivisionSample& other):state(other.state),marker(other.marker){
        privateCopy=state->armed;if(!privateCopy)return;++state->copies;
        if(state->phase!=0)return;
        state->copying=true;state->original=&other;state->Run();
        Check(state->destroyedDuringCopy==0&&other.marker==173,
            "copying insertion channels pins the original Channel sampler through synchronous retirement");
        state->copying=false;state->ThrowResponse();
    }
    ~SubdivisionSample(){
        if(state->copying&&state->original==this)++state->destroyedDuringCopy;
        if(privateCopy&&state->armed&&state->phase==2)state->Run();
    }
    ac::Sample operator()(const ce::Channel& channel,double frame)const{
        if(state->armed&&state->phase==1){++state->bodies;
            Check(privateCopy&&channel.id=="native"&&channel.track.keys.size()==2,
                "insertion hit testing invokes its already-private Channel sampler without another callable copy");
            state->Run();
            Check(marker==173&&channel.id=="native"&&channel.track.keys.size()==2&&std::isfinite(frame),
                "private sampled Channel and frame remain valid after its original Session retires");
            state->ThrowResponse();
        }
        return {frame*frame/20.0,frame/10.0};
    }
};
void SubdivisionSamplerLifetime(){
    for(int phase=0;phase!=3;++phase)for(int action=0;action!=3;++action)for(int response=0;response!=3;++response){
        // A noexcept sample destructor only retires; throwing is exercised by
        // copy and body, not by terminating the process in a user destructor.
        if(phase==2&&response!=0)continue;
        std::printf("insertion sample lifetime phase=%d action=%d response=%d\n",phase,action,response);std::fflush(stdout);
        auto host=SubdivisionHost();HostSource replacement;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        SubdivisionSampleState state;state.phase=phase;state.response=response;
        host.data[0].sample=SubdivisionSample(state);int subdivisions=0;
        auto session=MakeSession(host);session.sources[0].subdivide=[&](auto&,auto,auto,auto&,auto& error){
            ++subdivisions;error=L"release the private rejected insertion payload";return false;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"insertion sample lifetime opens before arming its capture");
        const POINT point=AskPoint(window,kHookPointForFrame,10);const auto token=ce::CaptureWindowSession(window);
        const auto retained=token.lock();HWND next=nullptr;std::wstring status,field;int readsAfter=-1;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"insertion sample retirement creates a real replacement Session");
                if(action==1)Check(next==window,"insertion sample same-owner replacement keeps the HWND");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
            readsAfter=host.readCalls;
        };
        state.armed=true;TriggerSubdivision(window,point);
        Check(state.actions==1&&state.copies==1&&state.bodies==unsigned(phase==1)&&
            state.destroyedDuringCopy==0&&subdivisions==int(phase==2)&&host.applyCalls==0&&host.readCalls==readsAfter&&
            retained&&!ce::IsWindowSessionCurrent(window,token)&&host.data[0].track.keys.size()==2,
            "sample copy, hit-test body and rejected private release cannot publish or resume retired insertion");
        if(next){
            Check(KeyCount(next)==2&&replacement.applyCalls==0&&GetText(next,kIdStatus)==status&&
                GetText(next,kIdFrameEdit)==field&&LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},
                "retired insertion sampler cannot update replacement controls, history or data");
            ClickKey(next,1);Click(next,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(next)==1,
                "replacement remains editable after insertion sampler capture release");
        }else Check(OwnedCurveWindow()==nullptr,"closed insertion sample leaves no registered editor");
        state.armed=false;ce::CloseOwned(nullptr);host.data[0].sample={};
    }
    Note("insertion samples retain original channel storage through channel-copy, private hit testing and final release");
}

void SubdivisionRejectsNestedAuthoring(){
    for(bool sampling:{false,true})for(int phase=0;phase!=3;++phase){
        auto host=SubdivisionHost();SubdivisionRetirementState provider;SubdivisionSampleState sample;
        provider.phase=phase;sample.phase=phase;auto session=MakeSession(host);
        if(sampling){host.data[0].sample=SubdivisionSample(sample);session=MakeSession(host);
            session.sources[0].subdivide=[](auto&,auto,auto,auto&,auto& error){error=L"private release";return false;};}
        else session.sources[0].subdivide=SubdivisionRetirementProvider(provider);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));SeedSubdivisionRedo(window);
        const POINT point=AskPoint(window,kHookPointForFrame,10);const auto history=LocalWriteHistory(window);
        const int applies=host.applyCalls,reads=host.readCalls;const auto data=host.data;
        int pumps=0;
        auto pump=[&]{
            ++pumps;const auto before=LocalWriteHistory(window);const int beforeApplies=host.applyCalls,beforeReads=host.readCalls;
            for(int command:{kIdAddKey,kIdDeleteKey,kIdApplyKey,kIdUndo,kIdRedo,kIdBakeRun})Click(window,command);
            const HWND canvas=GetDlgItem(window,kIdCanvas);
            SendMessageW(canvas,WM_KEYDOWN,VK_DELETE,0);SendMessageW(canvas,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
            ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
            Check(host.applyCalls==beforeApplies&&host.readCalls==beforeReads&&LocalWriteHistory(window)==before&&
                SameWritePayload(host.data,data),
                "nested commands, mouse insertion and source notifications cannot author during insertion callbacks");
        };
        if(sampling){sample.action=pump;sample.armed=true;}else{provider.action=pump;provider.armed=true;}
        TriggerSubdivision(window,point);
        Check(pumps==1&&host.applyCalls==applies+int(!sampling)&&host.readCalls>=reads,
            "nested insertion callbacks run once and allow only the original accepted apply");
        if(sampling)Check(LocalWriteHistory(window)==history&&SameWritePayload(host.data,data),
            "rejected insertion sample preserves both retained history stacks after nested messages");
        else Check(KeyCount(window)==3&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},
            "successful insertion commits one prepared result after nested messages are rejected");
        provider.armed=sample.armed=false;ce::CloseOwned(nullptr);host.data[0].sample={};
    }
    Note("subdivision and insertion sampler copy, body and release reject nested authoring through the same guards");
}
struct SubdivisionReleaseState {
    bool armed=false;unsigned releases=0;std::function<void()> action;
    void Run(){armed=false;++releases;action();}
};
struct SubdivisionReleaseSample {
    SubdivisionReleaseState* state;bool storedCopy=false;
    explicit SubdivisionReleaseSample(SubdivisionReleaseState& value):state(&value){}
    SubdivisionReleaseSample(const SubdivisionReleaseSample& other):state(other.state),storedCopy(true){}
    ~SubdivisionReleaseSample(){if(storedCopy&&state->armed)state->Run();}
    ac::Sample operator()(const ce::Channel&,double frame)const{return {frame*frame/20.0,frame/10.0};}
};
void SubdivisionReadPayloadRelease(){
    // True/false/changed precheck latest payload, then the old adopted Channel
    // released by an ordinary source refresh. All retire from real sample
    // destruction, never by calling an editor-only hook from the fixture.
    for(int route=0;route!=4;++route)for(int action=0;action!=3;++action){
        std::printf("insertion read payload release route=%d action=%d\n",route,action);std::fflush(stdout);
        auto host=SubdivisionHost();HostSource replacement;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        SubdivisionReleaseState state;bool armRead=false;int subdivisions=0;
        if(route==3){const SubdivisionReleaseSample sample(state);host.data[0].sample=sample;}
        auto session=MakeSession(host);const auto read=session.sources[0].read;
        session.sources[0].read=[&,read](auto& out,auto& error){
            if(!read(out,error))return false;
            if(armRead){armRead=false;const SubdivisionReleaseSample sample(state);out[0].sample=sample;
                if(route==1){error=L"read returned private data but refused";return false;}
                if(route==2)out[0].track.keys[1].value=21;
            }
            return true;
        };
        session.sources[0].subdivide=[&](auto& channels,std::size_t channel,int frame,std::size_t& index,auto& error){
            ++subdivisions;return PrepareNativeSubdivision(channels,channel,frame,index,error);};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"sample-release read fixture opens its native source");
        const POINT point=AskPoint(window,kHookPointForFrame,10);const auto token=ce::CaptureWindowSession(window);
        const auto retained=token.lock();HWND next=nullptr;std::wstring status,field;int readsAfter=-1;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"sample-release read installs a real replacement editor");
                if(action==1)Check(next==window,"sample-release read can replace only the Session on the same HWND");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
            readsAfter=host.readCalls;
        };
        if(route==3){host.data[0].sample=[](const ce::Channel&,double frame){return ac::Sample{frame*frame/20.0,frame/10.0};};
            state.armed=true;ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        }else{armRead=state.armed=true;TriggerSubdivision(window,point);}
        Check(state.releases==1&&host.applyCalls==0&&host.readCalls==readsAfter&&retained&&
            !ce::IsWindowSessionCurrent(window,token)&&subdivisions==int(route!=3)&&host.data[0].track.keys.size()==2,
            "precheck or adopted old-sample release stops before prepared apply and old source display continuations");
        if(next){Check(replacement.applyCalls==0&&KeyCount(next)==2&&GetText(next,kIdStatus)==status&&
                GetText(next,kIdFrameEdit)==field&&LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},
                "released read payload cannot update replacement status, fields, channels or history");
            ClickKey(next,1);Click(next,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(next)==1,
                "replacement remains editable after old channel sample release and source-read guards unwind");
        }else Check(OwnedCurveWindow()==nullptr,"retired read payload leaves no native or registered ghost");
        armRead=state.armed=false;ce::CloseOwned(nullptr);
    }
    Note("precheck latest and adopted old Channel sample releases cannot submit old prepared data or UI into new sources");
}
struct InsertionCaptureRelease;
struct InsertionCaptureReleaseState {
    bool armed=false,releaseReady=false,partialCopy=false;
    int throwKind=0,copies=0,laterThrows=0,created=0,live=0,releases=0,earlyReleases=0;
    std::weak_ptr<InsertionCaptureRelease> capture;
    std::function<void()> action;
};
struct InsertionCaptureRelease {
    InsertionCaptureReleaseState* state;
    explicit InsertionCaptureRelease(InsertionCaptureReleaseState& value):state(&value){++state->created;++state->live;}
    ~InsertionCaptureRelease(){
        Check(state->live==1&&state->capture.expired(),
            "the capture callback runs from its real final shared owner, with no local fixture owner retained");
        --state->live;++state->releases;
        if(!state->releaseReady){++state->earlyReleases;return;}
        state->armed=false;state->releaseReady=false;state->action();
    }
};
// The first armed copy owns one fresh capture. Further copies represent the
// host's independent canonical sampler and intentionally acquire no ownership
// of that private capture. The fixture retains only a weak observation.
struct InsertionOwnedCaptureSampler {
    InsertionCaptureReleaseState* state;int lane=0;
    std::shared_ptr<InsertionCaptureRelease> capture;
    explicit InsertionOwnedCaptureSampler(InsertionCaptureReleaseState& value,int channel=0):state(&value),lane(channel){}
    InsertionOwnedCaptureSampler(const InsertionOwnedCaptureSampler& other):state(other.state),lane(other.lane){
        if(!state->armed)return;
        if(state->partialCopy&&lane==1){
            ++state->laterThrows;state->releaseReady=true;
            Check(state->created==1&&state->live==1&&state->capture.use_count()==1,
                "later-channel copy failure sees the preceding copied Channel as the capture's sole owner");
            if(state->throwKind==0)throw std::bad_alloc();
            throw std::runtime_error("later native Channel sample copy failed");
        }
        ++state->copies;
        if(state->copies==1){
            capture=std::make_shared<InsertionCaptureRelease>(*state);state->capture=capture;
            Check(state->capture.use_count()==1,
                "a private sampler owns its capture after construction without an extra fixture strong reference");
        }
    }
    ac::Sample operator()(const ce::Channel&,double frame)const{return {frame*frame/20.0,frame/10.0};}
};

void SubdivisionCommitPayloadRelease(){
    // Rejected/throwing apply destroys CommitLocalChannels' still-owned next
    // parameter. Acceptance moves it into state, then canonical adoption retires
    // that exact private sampler. Neither is the earlier subdivide-false path.
    for(int route=0;route!=3;++route)for(int action=0;action!=3;++action){
        std::printf("subdivision commit payload release route=%d action=%d\n",route,action);std::fflush(stdout);
        InsertionCaptureReleaseState state;auto host=SubdivisionHost();HostSource replacement;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        const InsertionOwnedCaptureSampler sample(state);host.data[0].sample=sample;
        auto session=MakeSession(host);const auto apply=session.sources[0].apply;int subdivisions=0;
        session.sources[0].subdivide=[&](auto& channels,std::size_t channel,int frame,std::size_t& index,auto& error){
            ++subdivisions;return PrepareNativeSubdivision(channels,channel,frame,index,error);};
        session.sources[0].apply=[&,apply](const auto& channels,auto& error){
            Check(state.created==1&&state.live==1&&state.capture.use_count()==1&&state.releases==0&&
                channels.size()==1&&channels[0].track.keys.size()==3&&channels[0].track.keys[1].frame==10,
                "apply receives the intact detached insertion while its private sampler capture is still exclusively owned");
            state.releaseReady=true;
            if(route==0){++host.applyCalls;error=L"apply rejected the prepared native key";return false;}
            if(route==1){++host.applyCalls;throw std::runtime_error("apply failed before accepting the prepared key");}
            return apply(channels,error);
        };
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"commit-tail sample release fixture opens before arming");
        const POINT point=AskPoint(window,kHookPointForFrame,10);const auto token=ce::CaptureWindowSession(window);
        const auto retained=token.lock();HWND next=nullptr;std::wstring status,field;int readsAfter=-1;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"commit-tail capture release installs a real replacement editor");
                if(action==1)Check(next==window,"commit-tail capture release replaces its Session on the same HWND");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
            readsAfter=host.readCalls;
        };
        state.armed=true;TriggerSubdivision(window,point);
        Check(state.created==1&&state.releases==1&&state.live==0&&state.earlyReleases==0&&state.capture.expired()&&
            subdivisions==1&&host.applyCalls==1&&host.readCalls==readsAfter&&retained&&!ce::IsWindowSessionCurrent(window,token)&&
            host.data[0].track.keys.size()==std::size_t(route==2?3:2),
            "real apply-parameter or accepted canonical release retires once, preserving exactly the host's accepted outcome");
        if(next){Check(replacement.applyCalls==0&&KeyCount(next)==2&&GetText(next,kIdStatus)==status&&
                GetText(next,kIdFrameEdit)==field&&LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},
                "commit-tail release cannot transfer old result, status, selection or history into a replacement");
            ClickKey(next,1);Click(next,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(next)==1,
                "replacement remains editable after apply parameter and canonical capture releases");
        }else Check(OwnedCurveWindow()==nullptr,"commit-tail capture release leaves no closed editor registered");
        state.armed=state.releaseReady=false;ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    Note("rejected, throwing and accepted canonical apply tails protect the final private sampler capture owner");
}

void SubdivisionPartialChannelCopyRelease(){
    for(int failure=0;failure!=2;++failure)for(int action=0;action!=3;++action){
        std::printf("subdivision partial channel copy failure=%d action=%d\n",failure,action);std::fflush(stdout);
        InsertionCaptureReleaseState state;state.partialCopy=true;state.throwKind=failure;
        auto host=SubdivisionHost();host.data.push_back(MakeChannel("second",L"Second",{MakeKey(0,0),MakeKey(20,20)}));
        const InsertionOwnedCaptureSampler first(state,0),second(state,1);
        host.data[0].sample=first;host.data[1].sample=second;host.data[1].selected=false;
        HostSource replacement;replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        int subdivisions=0;auto session=MakeSession(host);
        session.sources[0].subdivide=[&](auto&,auto,auto,auto&,auto&){++subdivisions;return false;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"partial-channel copy fixture opens before either copy is armed");
        const POINT point=AskPoint(window,kHookPointForFrame,10);const int reads=host.readCalls;
        const auto token=ce::CaptureWindowSession(window);const auto retained=token.lock();
        HWND next=nullptr;std::wstring status,field;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"partial vector cleanup installs a real replacement Session");
                if(action==1)Check(next==window,"partial vector cleanup can replace only the native window's Session");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
        };
        state.armed=true;TriggerSubdivision(window,point);
        Check(state.copies==1&&state.laterThrows==1&&state.created==1&&state.releases==1&&state.live==0&&
            state.earlyReleases==0&&state.capture.expired()&&subdivisions==0&&host.applyCalls==0&&host.readCalls==reads&&
            retained&&!ce::IsWindowSessionCurrent(window,token)&&host.data.size()==2&&host.data[0].track.keys.size()==2,
            "a later Channel copy exception safely retires through the preceding private Channel's final capture release");
        if(next){Check(KeyCount(next)==2&&replacement.applyCalls==0&&GetText(next,kIdStatus)==status&&
                GetText(next,kIdFrameEdit)==field&&LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},
                "partial vector cleanup cannot continue sampling, subdividing or updating replacement controls");
            ClickKey(next,1);Click(next,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(next)==1,"replacement remains editable after partial vector cleanup");
        }else Check(OwnedCurveWindow()==nullptr,"partial vector cleanup removes its closed original editor");
        state.armed=state.releaseReady=false;ce::CloseOwned(nullptr);
        for(auto& channel:host.data)channel.sample={};host.applied.clear();
    }
    Note("partial multi-Channel copy failure protects final sampler ownership and rejects old continuation");
}

void SubdivisionCanonicalIdentitySelection(){
    for(int mode=1;mode!=4;++mode){
        auto host=SubdivisionHost();host.data.push_back(MakeChannel("other",L"Other",{MakeKey(0,91),MakeKey(20,99)}));
        host.data[1].selected=false;bool normalize=false;int subdivisions=0,seeks=0,lastFrame=-1;
        std::string selectedId;std::vector<int> selectedFrames;
        auto session=MakeSession(host);const auto apply=session.sources[0].apply;
        session.sources[0].subdivide=[&](auto& channels,std::size_t channel,int frame,std::size_t& index,auto& error){
            ++subdivisions;return PrepareNativeSubdivision(channels,channel,frame,index,error);};
        session.sources[0].apply=[&,apply](const auto& channels,auto& error){
            if(!apply(channels,error))return false;
            if(normalize){normalize=false;
                if(mode&2){auto& native=host.data[0];native.track.keys.insert(native.track.keys.begin()+1,MakeKey(5,1.25));
                    native.nativeControls.resize(native.track.keys.size());}
                if(mode&1)std::swap(host.data[0],host.data[1]);
            }
            return true;
        };
        session.sources[0].writeKeySelection=[&](const auto& id,const auto& frames){selectedId=id;selectedFrames=frames;};
        session.seek=[&](int frame){++seeks;lastFrame=frame;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"canonical identity fixture opens the intended native channel");
        const POINT point=AskPoint(window,kHookPointForFrame,10);normalize=true;TriggerSubdivision(window,point);
        Check(subdivisions==1&&host.applyCalls==1&&ActiveChannelIndex(window)==((mode&1)?1:0)&&
            KeyCount(window)==((mode&2)?4:3)&&GetText(window,kIdFrameEdit)==L"10"&&GetText(window,kIdValueEdit)==L"5"&&
            selectedId=="native"&&selectedFrames==std::vector<int>{10}&&seeks==1&&lastFrame==10&&
            LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},
            "canonical channel reordering and earlier keys restore the intended selection by stable channel id and frame");
        Click(window,kIdDeleteKey);
        const auto native=std::find_if(host.data.begin(),host.data.end(),[](const auto& channel){return channel.id=="native";});
        Check(host.applyCalls==2&&native!=host.data.end()&&std::none_of(native->track.keys.begin(),native->track.keys.end(),
            [](const auto& key){return key.frame==10;}),
            "the next edit targets the canonical frame-ten key, not its obsolete numeric channel or key index");
        ce::CloseOwned(nullptr);
    }
    Note("canonical channel and key reorderings preserve insertion selection identity and subsequent editing");
}

void SubdivisionThrowingReadRelease(){
    for(int failure=0;failure!=2;++failure)for(int action=0;action!=3;++action){
        std::printf("subdivision throwing read payload failure=%d action=%d\n",failure,action);std::fflush(stdout);
        InsertionCaptureReleaseState state;auto host=SubdivisionHost();HostSource replacement;
        replacement.data.push_back(MakeChannel("replacement",L"Replacement",{MakeKey(0,91),MakeKey(20,99)}));
        bool fillThenThrow=false;int throws=0,subdivisions=0;
        auto session=MakeSession(host);const auto read=session.sources[0].read;
        session.sources[0].read=[&,read](auto& out,auto& error){
            if(!read(out,error))return false;
            if(fillThenThrow){fillThenThrow=false;const InsertionOwnedCaptureSampler sample(state);out[0].sample=sample;
                Check(state.created==1&&state.live==1&&state.releases==0&&state.capture.use_count()==1,
                    "throwing read leaves its filled out Channel as the capture's only actual owner");
                state.releaseReady=true;++throws;
                if(failure==0)throw std::runtime_error("read filled private channels before failing");
                throw std::bad_alloc();
            }
            return true;
        };
        session.sources[0].subdivide=[&](auto& channels,std::size_t channel,int frame,std::size_t& index,auto& error){
            ++subdivisions;return PrepareNativeSubdivision(channels,channel,frame,index,error);};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==2,"throwing read cleanup opens before arming its private payload");
        const POINT point=AskPoint(window,kHookPointForFrame,10);const auto token=ce::CaptureWindowSession(window);
        const auto retained=token.lock();HWND next=nullptr;std::wstring status,field;int readsAfter=-1;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"throwing read payload release installs a real replacement editor");
                if(action==1)Check(next==window,"throwing read payload release preserves the original HWND when reopening");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
            readsAfter=host.readCalls;
        };
        fillThenThrow=state.armed=true;TriggerSubdivision(window,point);
        Check(throws==1&&state.created==1&&state.releases==1&&state.live==0&&state.earlyReleases==0&&state.capture.expired()&&
            subdivisions==1&&host.applyCalls==0&&host.readCalls==readsAfter&&retained&&!ce::IsWindowSessionCurrent(window,token)&&
            host.data[0].track.keys.size()==2,
            "filled-out read exception retires only on final latest-payload release without applying prepared data");
        if(next){Check(replacement.applyCalls==0&&KeyCount(next)==2&&GetText(next,kIdStatus)==status&&
                GetText(next,kIdFrameEdit)==field&&LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},
                "throwing latest cleanup never resumes old display or history work on the replacement");
            ClickKey(next,1);Click(next,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(next)==1,"replacement remains editable after throwing read payload cleanup");
        }else Check(OwnedCurveWindow()==nullptr,"throwing read cleanup leaves no original editor registered");
        fillThenThrow=state.armed=state.releaseReady=false;ce::CloseOwned(nullptr);
    }
    Note("throwing and OOM reads release filled latest Channel captures before any retired commit continuation");
}
void SubdivisionLifetime(){
    NativeSubdivisionCopyBoundary();SubdivisionCallbackLifetime();SubdivisionSamplerLifetime();
    SubdivisionFailuresKeepRedo();SubdivisionAcceptedRecovery();SubdivisionRejectsNestedAuthoring();SubdivisionReadPayloadRelease();
    SubdivisionCommitPayloadRelease();SubdivisionPartialChannelCopyRelease();
    SubdivisionCanonicalIdentitySelection();SubdivisionThrowingReadRelease();
}

HostSource TransportHost(){
    HostSource host;host.data.push_back(MakeChannel("transport",L"Transport",
        {MakeKey(0,1),MakeKey(10,3),MakeKey(20,2)}));return host;
}
// Phase-A ordinary sampler probes never retire a Session or publish data.
struct SamplerCopyBoundaryState {
    bool armed=false,failCopy=false;
    std::array<unsigned,2> copies{},bodies{},liveBodies{};
};
struct SamplerCopyBoundaryProvider {
    SamplerCopyBoundaryState* state;unsigned lane;bool owned=false;
    SamplerCopyBoundaryProvider(SamplerCopyBoundaryState& value,unsigned index):state(&value),lane(index){}
    SamplerCopyBoundaryProvider(const SamplerCopyBoundaryProvider& other):state(other.state),lane(other.lane){
        owned=state->armed;
        if(owned){++state->copies[lane];if(state->failCopy)throw std::bad_alloc();}
    }
    ac::Sample operator()(const ce::Channel&,double frame)const{
        if(state->armed){++state->bodies[lane];if(!owned)++state->liveBodies[lane];}
        return {frame*frame/20.0+lane,frame/10.0+lane};
    }
};
void SamplerCopyBoundary(){
    struct Observation {
        unsigned route=0;bool failCopy=false,unchanged=false,output=false,cleanup=false;
        std::array<unsigned,2> copies{},bodies{},liveBodies{};
    };
    std::vector<Observation> observed;
    // Ordinary scalars only: no callback closes/replaces the editor or writes
    // host data. Collect every raw-call observation before the first red Check.
    for(unsigned route=0;route<5;++route)for(bool failCopy:{true,false}){
        SamplerCopyBoundaryState state;state.failCopy=failCopy;HostSource host;
        for(unsigned lane=0;lane<2;++lane){
            host.data.push_back(MakeChannel("sample."+std::to_string(lane),L"Sampler",
                {MakeKey(0,0),MakeKey(10,5),MakeKey(20,9)}));
            host.data.back().sample=SamplerCopyBoundaryProvider(state,lane);
        }
        auto session=MakeSession(host);const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==3,"safe sampler copy probe opens real channels before arming");
        const HWND canvas=GetDlgItem(window,kIdCanvas),view=GetDlgItem(window,kIdViewCombo);
        Check(IsWindow(canvas)&&IsWindow(view),"safe sampler probe owns its canvas and view control");
        if(route!=0){SendMessageW(view,CB_SETCURSEL,1,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdViewCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(view));}
        const int width=route==4?613:379,height=247;
        HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;void* bits=nullptr;
        if(route>=3){
            Check(SetWindowPos(canvas,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,
                "safe sampler print probe prepares its own child width before arming");
            dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;
            info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            Check(dc&&bitmap&&bits,"safe sampler print probe owns a memory DC and DIB without reading the desktop");
            previous=SelectObject(dc,bitmap);Check(previous&&previous!=HGDI_ERROR,"select the sampler probe's owned bitmap");
            GdiFlush();
            std::fill_n(static_cast<unsigned*>(bits),std::size_t(width)*height,0x005a13c7u);
        }
        const auto token=ce::CaptureWindowSession(window);
        const auto history=LocalWriteHistory(window);const auto payload=host.data;
        double sample[3]{5.0,-1234.0,-5678.0};CurveEditorAxisState axis{};
        // Set each named field to a non-result sentinel without relying on
        // unspecified structure padding when checking an aborted output.
        axis.plot={-71,-72,-73,-74};axis.timeBar=axis.valueBar=axis.plot;
        axis.timeFirst=axis.timeLast=axis.valueFirst=axis.valueLast=-75;
        axis.timeLow=axis.timeHigh=axis.valueLow=axis.valueHigh=-76;
        axis.timeStart=axis.timeEnd=axis.viewLow=axis.viewHigh=-77;
        axis.mouseFrame=axis.mouseValue=-78;axis.mouseValid=true;axis.viewMode=-79;
        const auto originalAxis=axis;LRESULT result=0;state.armed=true;
        if(route==0)result=SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(sample));
        else if(route==1)result=SendMessageW(window,kHookPointForKey,1,0);
        else if(route==2)result=SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis));
        else result=SendMessageW(canvas,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
        state.armed=false;Observation entry;entry.route=route;entry.failCopy=failCopy;
        entry.copies=state.copies;entry.bodies=state.bodies;entry.liveBodies=state.liveBodies;
        entry.unchanged=IsWindow(window)&&ce::IsWindowSessionCurrent(window,token)&&host.applyCalls==0&&
            SameWritePayload(host.data,payload)&&LocalWriteHistory(window)==history&&KeyCount(window)==3;
        if(route==0)entry.output=failCopy?(sample[0]==5&&sample[1]==-1234&&sample[2]==-5678):
            (result!=0&&sample[0]==5&&sample[1]==1.25&&sample[2]==0.5);
        else if(route==1)entry.output=failCopy?(result==0||result==MAKELPARAM(-1,-1)):
            (static_cast<short>(LOWORD(result))>0&&static_cast<short>(HIWORD(result))>0);
        else if(route==2)entry.output=failCopy?
            (EqualRect(&axis.plot,&originalAxis.plot)&&EqualRect(&axis.timeBar,&originalAxis.timeBar)&&
             EqualRect(&axis.valueBar,&originalAxis.valueBar)&&axis.timeFirst==-75&&axis.timeLast==-75&&
             axis.valueFirst==-75&&axis.valueLast==-75&&axis.timeLow==-76&&axis.timeHigh==-76&&
             axis.valueLow==-76&&axis.valueHigh==-76&&axis.timeStart==-77&&axis.timeEnd==-77&&
             axis.viewLow==-77&&axis.viewHigh==-77&&axis.mouseFrame==-78&&axis.mouseValue==-78&&axis.mouseValid&&axis.viewMode==-79):
            (result!=0&&axis.viewMode==1&&axis.plot.right>axis.plot.left&&axis.viewHigh>axis.viewLow);
        else {GdiFlush();bool untouched=true;const auto* pixels=static_cast<const unsigned*>(bits);
            for(std::size_t i=0;i<std::size_t(width)*height&&untouched;++i)untouched=pixels[i]==0x005a13c7u;
            entry.output=failCopy?untouched:!untouched;}
        entry.cleanup=true;
        if(dc){entry.cleanup=SelectObject(dc,previous)!=HGDI_ERROR;entry.cleanup&=DeleteObject(bitmap)!=FALSE;entry.cleanup&=DeleteDC(dc)!=FALSE;}
        ce::CloseOwned(nullptr);for(auto& channel:host.data)channel.sample={};host.applied.clear();
        entry.unchanged&=!IsWindow(window);observed.push_back(entry);
        std::printf("sampler copy route=%u fail_copy=%u copies=%u,%u bodies=%u,%u live=%u,%u output=%u unchanged=%u cleanup=%u\n",
            route,unsigned(failCopy),entry.copies[0],entry.copies[1],entry.bodies[0],entry.bodies[1],
            entry.liveBodies[0],entry.liveBodies[1],unsigned(entry.output),unsigned(entry.unchanged),unsigned(entry.cleanup));std::fflush(stdout);
    }
    bool failuresOwned=true,successOwned=true,outputs=true,unchanged=true;
    for(const auto& entry:observed){
        const unsigned copies=entry.copies[0]+entry.copies[1],bodies=entry.bodies[0]+entry.bodies[1];
        if(entry.failCopy)failuresOwned&=copies==1&&bodies==0&&!entry.liveBodies[0]&&!entry.liveBodies[1];
        else {successOwned&=bodies>0&&!entry.liveBodies[0]&&!entry.liveBodies[1];
            for(unsigned lane=0;lane<2;++lane)successOwned&=entry.copies[lane]<=1&&(!entry.bodies[lane]||entry.copies[lane]==1);
            if(entry.route>=2)for(unsigned lane=0;lane<2;++lane)successOwned&=entry.bodies[lane]>0&&entry.copies[lane]==1;}
        outputs&=entry.output;unchanged&=entry.unchanged&&entry.cleanup;
    }
    Check(observed.size()==10&&unchanged,"all safe sampler operations retain source, payload, history and owned GDI cleanup");
    Check(failuresOwned,"a failed owned Channel.sample copy aborts every read-only operation before any body");
    Check(successOwned,"each read-only operation copies each participating Channel.sample once and never invokes the live closure");
    Check(outputs,"sampling and axis failures leave caller outputs untouched while successful reads and prints produce results");
    Note("safe sampler copy observations precede all lifetime-changing sampler cases");
}
struct SamplerLifetimeState;
struct SamplerOwnedRelease {
    SamplerLifetimeState* state;
    explicit SamplerOwnedRelease(SamplerLifetimeState& value):state(&value){}
    ~SamplerOwnedRelease();
};
struct SamplerLifetimeState {
    int phase=0,response=0;bool armed=false,observing=false,copying=false,bake=false,previewAware=false;
    unsigned copies=0,bodies=0,liveBodies=0,actions=0,releases=0,created=0,destroyedOriginal=0;
    const void* original=nullptr;const void* firstOwned=nullptr;const void* secondOwned=nullptr;
    unsigned cacheFromPrivate=0,firstDestroyed=0,secondDestroyed=0;
    const ce::Channel* bodyChannel=nullptr;std::weak_ptr<SamplerOwnedRelease> capture;
    std::function<void()> action;
    void Fire(){armed=false;++actions;action();}
    void Throw()const{if(response==1)throw std::runtime_error("native sampler unavailable");if(response==2)throw std::bad_alloc();}
};
SamplerOwnedRelease::~SamplerOwnedRelease(){
    Check(state->capture.expired(),"sampler final-release observer holds only a weak capture");
    ++state->releases;if(state->armed&&state->phase==2){
        if(state->bake)Check(state->copies==2&&state->cacheFromPrivate==1&&state->firstDestroyed==1&&state->secondDestroyed==1,
            "Bake releases its SamplingBatch cache first and the last shared capture only when the private channel.sample is cleared");
        state->Fire();}
}
bool SamplerTailIntact(const ce::Channel& channel){
    if(channel.collections.size()!=2||channel.preservedCameraIntervals.size()!=1)return false;
    const auto& root=channel.collections[0];const auto& leaf=channel.collections[1];
    const auto& interval=channel.preservedCameraIntervals[0];
    return root.id=="sample.collection"&&root.label==L"Sampler collection"&&leaf.id=="sample.child"&&leaf.label==L"Sampler child"&&
        interval.first==0&&interval.last==10&&interval.sourceFirst==0&&interval.sourceLast==10&&interval.sampleFirst==0&&interval.sampleLast==10&&
        interval.sourceValueFirst==0&&interval.sourceValueLast==5&&interval.controls==std::array<double,4>{127.0/3,0,254.0/3,127.0/3}&&
        !interval.oneFrameHold&&interval.transportTiming&&interval.geometry==std::array<double,6>{0,1.0/3,0,5,1.0/3,0};
}
struct SamplerLifetimeProvider {
    SamplerLifetimeState* state;bool owned=false;int marker=191;
    std::shared_ptr<SamplerOwnedRelease> capture;
    explicit SamplerLifetimeProvider(SamplerLifetimeState& value):state(&value){}
    SamplerLifetimeProvider(const SamplerLifetimeProvider& other):state(other.state),marker(other.marker){
        if(!state->observing)return;owned=true;++state->copies;
        if(state->copies==1)state->firstOwned=this;
        else if(state->copies==2){state->secondOwned=this;if(other.owned)++state->cacheFromPrivate;}
        if(state->phase==2){
            capture=other.capture?other.capture:std::make_shared<SamplerOwnedRelease>(*state);
            if(!other.capture)++state->created;state->capture=capture;
        }
        if(state->armed&&state->phase==0){state->copying=true;state->original=&other;state->Fire();
            Check(!state->destroyedOriginal&&other.marker==191,"sampling copy retains its original callable through synchronous retirement");
            state->copying=false;state->Throw();}
    }
    ~SamplerLifetimeProvider(){
        if(state->copying&&state->original==this)++state->destroyedOriginal;
        if(state->observing&&owned){if(state->firstOwned==this)++state->firstDestroyed;if(state->secondOwned==this)++state->secondDestroyed;}
    }
    ac::Sample operator()(const ce::Channel& channel,double frame)const{
        state->bodyChannel=&channel;
        if(state->observing){++state->bodies;if(!owned)++state->liveBodies;}
        if(state->armed){
            Check(owned&&channel.id=="sample.native"&&channel.track.keys.size()==3&&SamplerTailIntact(channel),
                "sampling body receives the original Channel including metadata stored after sample");
            if(state->phase==1){
                state->bodyChannel=&channel;const auto* collections=channel.collections.data();
                const auto* intervals=channel.preservedCameraIntervals.data();state->Fire();
                Check(marker==191 && &channel==state->bodyChannel&&channel.id=="sample.native"&&channel.track.keys.size()==3&&
                    channel.track.keys[1].frame==10&&channel.collections.data()==collections&&channel.preservedCameraIntervals.data()==intervals&&SamplerTailIntact(channel),
                    "retired sampling keeps the exact Channel address and its complete trailing collection and camera-interval storage readable");state->Throw();}
            // Destructors never throw. These failures unwind the real final
            // owned capture, whose noexcept destructor then retires the source.
            if(state->phase>=2)state->Throw();
        }
        if(state->previewAware)return ac::Evaluate(channel.track,frame);
        return {frame*frame/20.0,frame/10.0};
    }
};
struct SamplerMemoryPrint {
    HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
    void* bits=nullptr;int width=0,height=0;bool cleaned=false;
    SamplerMemoryPrint()=default;
    SamplerMemoryPrint(const SamplerMemoryPrint&)=delete;
    void Prepare(HWND canvas,int w=379,int h=247){
        width=w;height=h;
        Check(SetWindowPos(canvas,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"prepare an owned sampler print canvas before arming");
        dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        Check(dc&&bitmap&&bits,"allocate the sampler matrix's own memory DC and bitmap");previous=SelectObject(dc,bitmap);
        Check(previous&&previous!=HGDI_ERROR,"select the matrix's owned print bitmap");Clear();
    }
    void Clear(){if(bits){GdiFlush();std::fill_n(static_cast<unsigned*>(bits),std::size_t(width)*height,0x005a13c7u);}}
    bool Untouched()const{GdiFlush();const auto* pixels=static_cast<const unsigned*>(bits);
        for(std::size_t i=0;i<std::size_t(width)*height;++i)if(pixels[i]!=0x005a13c7u)return false;return true;}
    void Close(){if(cleaned||!dc)return;cleaned=true;
        Check(SelectObject(dc,previous)!=HGDI_ERROR&&DeleteObject(bitmap)&&DeleteDC(dc),"restore and delete only sampler-owned GDI objects");dc=nullptr;bitmap=nullptr;}
    ~SamplerMemoryPrint(){Close();}
};
HostSource SamplerLifetimeHost(SamplerLifetimeState& state){
    HostSource host;host.data.push_back(MakeChannel("sample.native",L"Native sampler",
        {MakeKey(0,0),MakeKey(10,5),MakeKey(20,20)}));
    auto& channel=host.data[0];channel.collections={{"sample.collection",L"Sampler collection"},{"sample.child",L"Sampler child"}};
    ce::NativeCameraInterval interval;interval.first=interval.sourceFirst=0;interval.last=interval.sourceLast=10;
    interval.sampleFirst=0;interval.sampleLast=10;interval.sourceValueFirst=0;interval.sourceValueLast=5;
    interval.controls={127.0/3,0,254.0/3,127.0/3};interval.oneFrameHold=false;interval.transportTiming=true;
    const auto& first=channel.track.keys[0];const auto& last=channel.track.keys[1];
    interval.geometry={first.value,first.outWeight,first.outSlope,last.value,last.inWeight,last.inSlope};
    channel.preservedCameraIntervals.push_back(interval);channel.sample=SamplerLifetimeProvider(state);return host;
}
void PrepareSamplerEntry(HWND window,int entry,SamplerMemoryPrint& print){
    if(entry>=1&&entry<=3){const HWND view=GetDlgItem(window,kIdViewCombo);SendMessageW(view,CB_SETCURSEL,1,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdViewCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(view));}
    if(entry==3)print.Prepare(GetDlgItem(window,kIdCanvas));
    if(entry==4){SetText(window,kIdBakeFirstEdit,L"0");SetText(window,kIdBakeLastEdit,L"20");SetText(window,kIdBakeStepEdit,L"5");}
}
struct SamplerOutput {
    double values[3]{5,-1234,-5678};CurveEditorAxisState axis{};
    std::array<unsigned char,sizeof(CurveEditorAxisState)> original{};LRESULT result=0;
    SamplerOutput(){std::memset(&axis,0xa5,sizeof axis);std::memcpy(original.data(),&axis,sizeof axis);}
    bool Untouched(int entry)const{
        if(entry==0)return values[0]==5&&values[1]==-1234&&values[2]==-5678;
        if(entry==1)return result==0||result==MAKELPARAM(-1,-1);
        if(entry==2)return std::memcmp(original.data(),&axis,sizeof axis)==0;
        return true;
    }
};
void InvokeSamplerEntry(HWND window,int entry,SamplerMemoryPrint& print,SamplerOutput& out){
    if(entry==0)out.result=SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(out.values));
    else if(entry==1)out.result=SendMessageW(window,kHookPointForKey,1,0);
    else if(entry==2)out.result=SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&out.axis));
    else if(entry==3)out.result=SendMessageW(GetDlgItem(window,kIdCanvas),WM_PRINTCLIENT,reinterpret_cast<WPARAM>(print.dc),PRF_CLIENT);
    else Click(window,kIdBakeRun);
}
void SamplerReadAndBakeLifetime(){
    for(int entry=0;entry<5;++entry)for(int phase=0;phase<3;++phase)
    for(int action=0;action<3;++action)for(int response=0;response<3;++response){
        std::printf("sampler lifetime entry=%d phase=%d action=%d response=%d\n",entry,phase,action,response);std::fflush(stdout);
        SamplerLifetimeState state;state.phase=phase;state.response=response;state.bake=entry==4;auto host=SamplerLifetimeHost(state),replacement=TransportHost();
        auto session=MakeSession(host);const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==3,"sampler matrix opens before arming any lifetime action");
        SamplerMemoryPrint print;PrepareSamplerEntry(window,entry,print);const auto token=ce::CaptureWindowSession(window);
        const auto payload=host.data;HWND next=nullptr;
        std::wstring status,field;int readsAfter=-1;bool releaseBeforeDrawing=true;
        state.action=[&]{
            if(entry==3)releaseBeforeDrawing&=print.Untouched()&&GetCurrentObject(print.dc,OBJ_BITMAP)==print.bitmap;
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"sampler retirement creates a real replacement editor");
                if(action==1)Check(next==window,"sampler same-owner replacement preserves its native HWND");
                status=GetText(next,kIdStatus);field=GetText(next,kIdFrameEdit);}
            readsAfter=host.readCalls;
        };
        SamplerOutput output;state.armed=state.observing=true;InvokeSamplerEntry(window,entry,print,output);state.observing=false;
        const unsigned expectedCopies=entry==4&&phase!=0?2u:1u;
        Check(state.actions==1&&state.copies==expectedCopies&&!state.liveBodies&&!state.destroyedOriginal&&
            !ce::IsWindowSessionCurrent(window,token)&&host.applyCalls==0&&host.readCalls==readsAfter&&SameWritePayload(host.data,payload),
            "copy, body and final owned release stop retired sampling before authoring or old-source continuation");
        Check(output.Untouched(entry),"retired sampler cannot partially publish hook or axis output");
        if(phase==0)Check(!state.bodies,"copy retirement never invokes the sampled body");
        else Check(state.bodies>0,"body and final-release retirement execute the real private sampler");
        if(phase==2)Check(state.created==1&&state.releases==1&&state.capture.expired(),entry==4?
            "the last CopyChannels private bake-payload capture retires before commit":
            "the actual last read-only sampler cache capture retires before output publication");
        if(entry==3)Check(releaseBeforeDrawing&&print.Untouched(),"retired sampling and final release finish before any actual print drawing");
        if(next){Check(KeyCount(next)==3&&replacement.applyCalls==0&&GetText(next,kIdStatus)==status&&GetText(next,kIdFrameEdit)==field&&
                LocalWriteHistory(next)==std::array<std::uint64_t,2>{0,0},"old sampling cannot alter replacement controls, data or history");
            ClickKey(next,1);Click(next,kIdDeleteKey);Check(replacement.applyCalls==1&&KeyCount(next)==2,"replacement remains editable after sampler guards and releases unwind");}
        else Check(OwnedCurveWindow()==nullptr,"closed sampling leaves no registered ghost window");
        state.armed=false;ce::CloseOwned(nullptr);for(auto& channel:host.data)channel.sample={};host.applied.clear();
    }
    Note("108 read-only sampler cache cases and 27 CopyChannels private bake-payload cases retain parameters and stop retired continuations");
}
void SamplerNestedOperations(){
    for(int entry:{0,3,4})for(int phase=0;phase<3;++phase){
        std::printf("sampler nested entry=%d phase=%d\n",entry,phase);std::fflush(stdout);
        SamplerLifetimeState state;state.phase=phase;state.bake=entry==4;auto host=SamplerLifetimeHost(state);
        unsigned preflightReads=0,preflightCopies=0;auto session=MakeSession(host);const auto read=session.sources[0].read;
        session.sources[0].read=[&,read](auto& out,auto& error){
            const bool preflight=state.observing&&entry==4&&host.applyCalls==0;
            const auto copies=state.copies;const bool accepted=read(out,error);
            if(preflight){++preflightReads;preflightCopies+=state.copies-copies;}return accepted;
        };
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"nested sampler fixture opens before guards are tested");
        ClickKey(window,1);SamplerMemoryPrint print,nestedPrint;PrepareSamplerEntry(window,entry,print);nestedPrint.Prepare(GetDlgItem(window,kIdCanvas),613);
        const auto history=LocalWriteHistory(window);const auto payload=host.data;unsigned pumps=0;
        state.action=[&]{
            ++pumps;const auto copies=state.copies,bodies=state.bodies;const int reads=host.readCalls,applies=host.applyCalls;
            SamplerOutput nested;SamplerMemoryPrint unused;InvokeSamplerEntry(window,0,unused,nested);
            Check(nested.Untouched(0),"nested sample hooks cannot publish a value through an active read-only operation");
            CurveEditorAxisState axis{};SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis));
            const HWND canvas=GetDlgItem(window,kIdCanvas);InvalidateRect(canvas,nullptr,FALSE);SendMessageW(canvas,WM_PAINT,0,0);
            SendMessageW(canvas,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(nestedPrint.dc),PRF_CLIENT);
            Check(nestedPrint.Untouched(),"nested print cannot draw scalar fallback, a white background or any pixel into its caller's DIB");
            for(int id:{kIdAddKey,kIdDeleteKey,kIdApplyKey,kIdUndo,kIdRedo,kIdBakeRun})Click(window,id);
            SendMessageW(canvas,WM_KEYDOWN,VK_DELETE,0);ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
            Check(state.copies==copies&&state.bodies==bodies&&host.readCalls==reads&&host.applyCalls==applies&&
                LocalWriteHistory(window)==history&&SameWritePayload(host.data,payload),
                "nested paint, print, hooks, edit commands and refresh cannot recurse or author through a sampler");
        };
        const int readsBefore=host.readCalls;SamplerOutput out;state.armed=state.observing=true;InvokeSamplerEntry(window,entry,print,out);state.observing=false;
        const unsigned expectedPreflight=entry==4?1u:0u;
        const bool isolated=pumps==1&&state.actions==1&&state.copies==unsigned(entry==4?3:1)&&!state.liveBodies&&
            state.cacheFromPrivate==unsigned(entry==4)&&preflightReads==expectedPreflight&&preflightCopies==expectedPreflight;
        if(!isolated){
            std::printf("sampler nested failure entry=%d phase=%d pumps=%u actions=%u copies=%u bodies=%u live=%u host_reads=%d read_delta=%d expected_preflight=%u preflight_reads=%u preflight_copies=%u cache_from_private=%u\n",
                entry,phase,pumps,state.actions,state.copies,state.bodies,state.liveBodies,host.readCalls,host.readCalls-readsBefore,
                expectedPreflight,preflightReads,preflightCopies,state.cacheFromPrivate);std::fflush(stdout);
        }
        Check(isolated,"each sampler stage rejects recursion; accepted Bake owns one private payload, one sampling cache and one host preflight copy");
        if(entry==4)Check(host.applyCalls==1&&host.data[0].track.keys.size()==5&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},
            "nested authoring refusal still permits the one original prepared bake commit");
        else Check(host.applyCalls==0&&LocalWriteHistory(window)==history&&SameWritePayload(host.data,payload),"nested read-only sampling keeps both history stacks and canonical data");
        Check(!g_curveFailNextAllocation,"nested sampler cases leave no armed allocation fault");
        state.armed=false;ce::CloseOwned(nullptr);for(auto& channel:host.data)channel.sample={};host.applied.clear();
    }
    Note("sampler copy, body and final-release guards reject recursive readonly work and nested edits");
}
void SamplerFailuresFallbackAndRedo(){
    for(int entry:{0,3})for(int response:{1,2}){
        SamplerLifetimeState state;state.phase=3;state.response=response;auto host=SamplerLifetimeHost(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));Check(IsWindow(window),"native failure fixture opens with its real sampler");
        SamplerMemoryPrint print;PrepareSamplerEntry(window,entry,print);const auto history=LocalWriteHistory(window);
        SamplerOutput out;state.armed=state.observing=true;InvokeSamplerEntry(window,entry,print,out);state.armed=state.observing=false;
        Check(state.copies==1&&state.bodies==1&&!state.liveBodies&&out.Untouched(entry)&&host.applyCalls==0&&LocalWriteHistory(window)==history,
            "native runtime or OOM sampling failure cannot silently fall back to scalar Evaluate");
        if(entry==3)Check(print.Untouched(),"native body runtime or OOM without retirement leaves every caller print pixel untouched");
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    {
        auto host=TransportHost();const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
        const auto expected=ac::Evaluate(host.data[0].track,5);double sample[3]{5,-1234,-5678};
        Check(SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(sample))!=0&&sample[1]==expected.value&&sample[2]==expected.derivative,
            "an absent native sampler retains the ordinary scalar Evaluate fallback");ce::CloseOwned(nullptr);
    }
    for(int response:{0,1,2}){
        SamplerLifetimeState state;state.phase=3;state.response=response;auto host=SamplerLifetimeHost(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
        ClickKey(window,1);Click(window,kIdDeleteKey);Click(window,kIdUndo);
        Check(KeyCount(window)==3&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{0,1},"failed sampler bake starts with its original three keys and one real redo");
        SamplerMemoryPrint print;PrepareSamplerEntry(window,4,print);const auto history=LocalWriteHistory(window);const auto data=host.data;const int applies=host.applyCalls;
        host.failApply=response==0;SamplerOutput out;state.armed=state.observing=true;InvokeSamplerEntry(window,4,print,out);state.armed=state.observing=false;
        Check((response==0?state.bodies>=5:state.bodies==1)&&!state.liveBodies&&host.applyCalls==applies+int(response==0)&&
            SameWritePayload(host.data,data)&&LocalWriteHistory(window)==history,
            "a refused apply or failed private bake sampler keeps canonical data and the retained redo node");
        const int refusedApplies=host.applyCalls;host.failApply=false;
        Click(window,kIdRedo);Check(host.applyCalls==refusedApplies+1&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},"rejected bake sampling leaves its original redo usable once");
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    Note("native failure is distinct from scalar fallback and rejected bake sampling preserves redo");
}
void SamplerAcceptedBakeRecovery(){
    for(int failure=0;failure<3;++failure){
        SamplerLifetimeState state;state.phase=3;auto host=SamplerLifetimeHost(state);bool armed=false,failSelection=false;unsigned selectionFailures=0;
        auto session=MakeSession(host);const auto apply=session.sources[0].apply;
        session.sources[0].readKeySelection=[](const std::string&){return std::vector<int>{10};};
        session.sources[0].writeKeySelection=[&](const auto&,const auto&){if(failSelection){failSelection=false;++selectionFailures;throw std::runtime_error("accepted bake selection UI failed");}};
        session.sources[0].apply=[&,apply](const auto& data,auto& error){const bool accepted=apply(data,error);
            if(armed&&accepted){if(failure==0)host.failRead=true;else if(failure==1)g_curveFailNextAllocation=true;else failSelection=true;}return accepted;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));SamplerMemoryPrint print;PrepareSamplerEntry(window,4,print);
        const auto failures=g_curveAllocationFailures;SamplerOutput out;armed=state.observing=true;InvokeSamplerEntry(window,4,print,out);armed=state.observing=false;
        Check(host.applyCalls==1&&host.data[0].track.keys.size()==5&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0}&&!g_curveFailNextAllocation,
            "accepted bake retains its five prepared keys and one history node through canonical read or post-apply OOM failure");
        if(failure==1)Check(g_curveAllocationFailures==failures+1,"accepted bake consumes the one real post-publication allocation failure");
        if(failure==2)Check(selectionFailures==1&&!failSelection,"accepted bake actually reaches and contains its native selection-display callback failure");
        host.failRead=false;ce::NotifySourceChanged(nullptr,"source");SendMessageW(window,WM_APP+401,0,0);
        Check(host.applyCalls==1&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{1,0},"read-only bake recovery never retries the accepted apply");
        Click(window,kIdUndo);Check(host.applyCalls==2&&host.data[0].track.keys.size()==3,"accepted bake remains one complete undo step");
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    Note("accepted native bake sampling does not retry apply after display recovery failures");
}
bool SamplerKeysEqual(const std::vector<ac::Key>& a,const std::vector<ac::Key>& b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(a[i].frame!=b[i].frame||a[i].value!=b[i].value||a[i].inSlope!=b[i].inSlope||a[i].outSlope!=b[i].outSlope||
        a[i].inWeight!=b[i].inWeight||a[i].outWeight!=b[i].outWeight||a[i].tangent!=b[i].tangent||a[i].interpolation!=b[i].interpolation)return false;
    return true;
}
void SamplerHandleCancellation(){
    for(int entry:{0,2})for(int phase=0;phase<3;++phase)for(int action=0;action<4;++action){
        std::printf("sampler preview entry=%d phase=%d action=%d\n",entry,phase,action);std::fflush(stdout);
        SamplerLifetimeState state;state.phase=phase;state.previewAware=true;auto host=SamplerLifetimeHost(state),replacement=TransportHost();
        for(auto& key:host.data[0].track.keys)key.tangent=ac::Tangent::Free;
        host.data[0].track.keys[1].outWeight=0.3;const auto canonical=host.data[0].track.keys;
        const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));ClickKey(window,1);
        const HWND canvas=GetDlgItem(window,kIdCanvas);const POINT key=AskPoint(window,kHookPointForKey,1);
        const POINT handle=AskPoint(window,kHookPointForFrame,13),target=AskPoint(window,kHookPointForFrame,16);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(handle.x,key.y));
        SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x,key.y-20));
        double previewValue[3]{15,0,0};Check(SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(previewValue))!=0,
            "read the actual native sampler's in-place Handle preview before arming");
        Check(GetCapture()==canvas&&state.bodyChannel&&state.bodyChannel->id=="sample.native"&&
            !SamplerKeysEqual(state.bodyChannel->track.keys,canonical),"real handle mouse messages prepare an unpublished in-place preview");
        const auto* borrowed=state.bodyChannel;const auto* storage=borrowed->track.keys.data();const auto preview=borrowed->track.keys;
        ac::Track baseTrack;baseTrack.keys=canonical;const auto base=ac::Evaluate(baseTrack,15),actual=ac::Evaluate(borrowed->track,15);
        Check(previewValue[1]==actual.value&&previewValue[2]==actual.derivative&&(actual.value!=base.value||actual.derivative!=base.derivative)&&host.applyCalls==0,
            "the native sampler reads the real preview geometry rather than a synthetic value or the unchanged host base");
        const auto token=ce::CaptureWindowSession(window);const auto history=LocalWriteHistory(window);HWND next=nullptr;
        state.action=[&]{
            Check(borrowed->track.keys.data()==storage&&SamplerKeysEqual(borrowed->track.keys,preview),"sampler callback enters with the borrowed preview key storage");
            if(action==0)SendMessageW(canvas,WM_KILLFOCUS,0,0);
            else if(action==1){SetCapture(GetDlgItem(window,kIdFrameEdit));Check(GetCapture()!=canvas,"a real owned capture transfer cancels the original preview");}
            else {if(action==3)ce::CloseOwned(nullptr);next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));Check(IsWindow(next),"preview retirement installs its replacement source");}
            Check(borrowed->track.keys.data()==storage&&SamplerKeysEqual(borrowed->track.keys,preview),
                "focus, capture cancellation or Open cannot swap keys while a sampler borrows the preview");
        };
        SamplerMemoryPrint print;SamplerOutput output;state.armed=state.observing=true;InvokeSamplerEntry(window,entry,print,output);state.observing=false;
        Check(state.actions==1&&state.copies==1&&!state.liveBodies&&output.Untouched(entry)&&host.applyCalls==0,
            "cancelled direct or nested-extent handle sampling does not publish its old hook result or prepared edit");
        if(action<2){
            Check(ce::IsWindowSessionCurrent(window,token)&&LocalWriteHistory(window)==history&&SamplerKeysEqual(host.data[0].track.keys,canonical),
                "same-session cancellation keeps canonical data and both history stacks");
            double sample[3]{5,0,0};SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(sample));
            Check(state.bodyChannel&&SamplerKeysEqual(state.bodyChannel->track.keys,canonical),"deferred preview restore occurs only after the sampling borrow ends");
            SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x+7,key.y-33));SendMessageW(canvas,WM_LBUTTONUP,0,0);
            Check(host.applyCalls==0&&LocalWriteHistory(window)==history,"old mouse continuation cannot commit a cancelled preview");
        }else{
            Check(!ce::IsWindowSessionCurrent(window,token)&&next&&KeyCount(next)==3&&replacement.applyCalls==0&&
                replacement.data[0].track.keys[0].value==1&&replacement.data[0].track.keys[1].value==3,
                "old dragBase is never restored into a same-HWND or recreated replacement source");
            ClickKey(next,1);Click(next,kIdDeleteKey);Check(replacement.applyCalls==1&&KeyCount(next)==2,"replacement remains editable after preview borrow retirement");
        }
        state.armed=false;ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    Note("sampler borrows defer same-session preview restoration and never restore old dragBase into a replacement");
}
struct SamplerPaintCounter {
    HWND canvas=nullptr;unsigned paints=0;
    static LRESULT CALLBACK Observe(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
        if(m==WM_PAINT)++reinterpret_cast<SamplerPaintCounter*>(data)->paints;return DefSubclassProc(h,m,w,l);
    }
    explicit SamplerPaintCounter(HWND h):canvas(h){Check(SetWindowSubclass(h,Observe,0x73b8,reinterpret_cast<DWORD_PTR>(this))!=FALSE,"observe only the owned sampler canvas paint messages");}
    ~SamplerPaintCounter(){if(IsWindow(canvas))RemoveWindowSubclass(canvas,Observe,0x73b8);}
};
void SamplerDeferredRedrawAndGeometry(){
    for(int phase=0;phase<3;++phase){
        std::printf("sampler deferred entry=0 phase=%d\n",phase);std::fflush(stdout);
        SamplerLifetimeState state;state.phase=phase;auto host=SamplerLifetimeHost(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));const HWND canvas=GetDlgItem(window,kIdCanvas);
        SamplerPaintCounter paints(canvas);SamplerMemoryPrint baselinePrint;baselinePrint.Prepare(canvas);SamplerOutput baseline;
        ShowWindow(window,SW_SHOWNOACTIVATE);
        Check(IsWindowVisible(window)&&IsWindowVisible(canvas),"native deferred paint exercises a visible owned editor and canvas");
        UpdateWindow(window);UpdateWindow(canvas);
        InvokeSamplerEntry(window,0,baselinePrint,baseline);Check(baseline.result!=0,"establish the unarmed sampler/redraw baseline");
        SendMessageW(canvas,WM_PAINT,0,0);ValidateRect(canvas,nullptr);const auto history=LocalWriteHistory(window);
        RECT probeDamage{4,4,29,31},observedDamage{};
        const BOOL probeInvalidated=InvalidateRect(canvas,&probeDamage,FALSE);
        const BOOL probeUpdated=GetUpdateRect(canvas,&observedDamage,FALSE);
        const BOOL probeValidated=ValidateRect(canvas,nullptr);
        const BOOL probeAfter=GetUpdateRect(canvas,nullptr,FALSE);
        std::printf("sampler native damage phase=%d invalidate=%u update=%u rect=%ld,%ld,%ld,%ld validate=%u after=%u window_visible=%u canvas_visible=%u paints=%u\n",
            phase,unsigned(probeInvalidated!=FALSE),unsigned(probeUpdated!=FALSE),observedDamage.left,observedDamage.top,observedDamage.right,observedDamage.bottom,
            unsigned(probeValidated!=FALSE),unsigned(probeAfter!=FALSE),unsigned(IsWindowVisible(window)!=FALSE),unsigned(IsWindowVisible(canvas)!=FALSE),paints.paints);
        std::fflush(stdout);
        state.action=[&]{
            const auto bodies=state.bodies;const auto before=paints.paints;RECT damage{4,4,29,31};
            Check(InvalidateRect(canvas,&damage,FALSE)!=FALSE,"callback explicitly requests owned canvas damage");UpdateWindow(canvas);
            if(paints.paints==before)SendMessageW(canvas,WM_PAINT,0,0);
            Check(paints.paints>before&&state.bodies==bodies,"nested native paint validates damage without recursively sampling");
        };
        SamplerMemoryPrint print;SamplerOutput output;state.armed=state.observing=true;InvokeSamplerEntry(window,0,print,output);state.armed=state.observing=false;
        const BOOL deferredUpdate=GetUpdateRect(canvas,nullptr,FALSE);
        const bool deferredComplete=state.actions==1&&state.copies==1&&!state.liveBodies&&output.result!=0&&deferredUpdate;
        if(!deferredComplete){
            std::printf("sampler deferred failure entry=0 phase=%d actions=%u copies=%u bodies=%u live=%u output=%lld update=%u paints=%u window_visible=%u canvas_visible=%u\n",
                phase,state.actions,state.copies,state.bodies,state.liveBodies,static_cast<long long>(output.result),unsigned(deferredUpdate!=FALSE),
                paints.paints,unsigned(IsWindowVisible(window)!=FALSE),unsigned(IsWindowVisible(canvas)!=FALSE));std::fflush(stdout);
        }
        Check(deferredComplete,
            "the outer sampler preserves a deferred redraw requested and pumped by its callback");
        const auto before=paints.paints,copies=state.copies,bodies=state.bodies,live=state.liveBodies;
        state.observing=true;UpdateWindow(canvas);if(paints.paints==before)SendMessageW(canvas,WM_PAINT,0,0);state.observing=false;
        Check(paints.paints>before&&!GetUpdateRect(canvas,nullptr,FALSE)&&state.copies==copies+1&&state.bodies>bodies&&state.liveBodies==live&&
            host.applyCalls==0&&LocalWriteHistory(window)==history,
            "the deferred normal paint actually resamples its owned curve after guards unwind rather than merely validating damage");
        baselinePrint.Clear();SamplerOutput printed;const auto printBodies=state.bodies;state.observing=true;
        InvokeSamplerEntry(window,3,baselinePrint,printed);state.observing=false;
        Check(state.bodies>printBodies&&!baselinePrint.Untouched(),"the resumed native print path samples and renders into its owned DIB");
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    for(int entry:{1,2,3})for(int phase=0;phase<3;++phase){
        std::printf("sampler geometry entry=%d phase=%d\n",entry,phase);std::fflush(stdout);
        SamplerLifetimeState state;state.phase=phase;auto host=SamplerLifetimeHost(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));SamplerMemoryPrint print;PrepareSamplerEntry(window,entry,print);
        const HWND canvas=GetDlgItem(window,kIdCanvas);const auto token=ce::CaptureWindowSession(window);const auto history=LocalWriteHistory(window);
        RECT baselineClient{};Check(GetClientRect(canvas,&baselineClient)!=FALSE,"capture the actual canvas client before arming resize");
        CurveEditorAxisState baselineAxis{};Check(SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&baselineAxis))!=0,"capture baseline axes before arming resize");
        SamplerOutput baseline;InvokeSamplerEntry(window,entry,print,baseline);
        Check(entry==3?!print.Untouched():baseline.result!=0,"establish unarmed key/axis/print geometry before resizing callbacks");
        std::vector<unsigned> baselinePixels;if(entry==3){GdiFlush();const auto* pixels=static_cast<const unsigned*>(print.bits);baselinePixels.assign(pixels,pixels+std::size_t(print.width)*print.height);}print.Clear();
        state.action=[&]{RECT client{};GetClientRect(canvas,&client);
            Check(SetWindowPos(canvas,nullptr,0,0,client.right+71,client.bottom+17,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,
                "sampler callback resizes the same native canvas without replacing its Session");};
        SamplerOutput output;state.armed=state.observing=true;InvokeSamplerEntry(window,entry,print,output);state.armed=state.observing=false;
        Check(state.actions==1&&state.copies==1&&ce::IsWindowSessionCurrent(window,token)&&output.Untouched(entry)&&
            host.applyCalls==0&&LocalWriteHistory(window)==history,"geometry changes abort old key/axis coordinates without a mixed-revision result");
        if(entry==3)Check(print.Untouched(),"same-HWND resize during copy, body or final release publishes no mixed-geometry print pixels");
        RECT freshClient{};Check(GetClientRect(canvas,&freshClient)!=FALSE&&freshClient.right==baselineClient.right+71&&freshClient.bottom==baselineClient.bottom+17,
            "the same native canvas retains the callback's exact larger client dimensions");
        CurveEditorAxisState freshAxis{};Check(SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&freshAxis))!=0,"read the current post-resize coordinate system");
        Check(freshAxis.plot.left==baselineAxis.plot.left&&freshAxis.plot.top==baselineAxis.plot.top&&
            freshAxis.plot.right-baselineAxis.plot.right==freshClient.right-baselineClient.right&&
            freshAxis.plot.bottom-baselineAxis.plot.bottom==freshClient.bottom-baselineClient.bottom,
            "fresh axis right and bottom follow the measured client changes exactly");
        SamplerOutput fresh;InvokeSamplerEntry(window,entry,print,fresh);
        if(entry==1){const int oldX=static_cast<short>(LOWORD(baseline.result)),newX=static_cast<short>(LOWORD(fresh.result));
            const double ratio=double(oldX-baselineAxis.plot.left)/(baselineAxis.plot.right-baselineAxis.plot.left);
            const double expectedX=freshAxis.plot.left+ratio*(freshAxis.plot.right-freshAxis.plot.left);
            Check(fresh.result!=0&&newX>oldX&&std::fabs(newX-expectedX)<=2,
                "fresh KeyPoint moves with the larger plot rather than returning its cached baseline coordinate");}
        else if(entry==2)Check(fresh.result!=0&&fresh.axis.plot.right==freshAxis.plot.right&&fresh.axis.plot.bottom==freshAxis.plot.bottom,
            "the fresh public axis hook publishes the new measured plot bounds");
        else {GdiFlush();const auto* pixels=static_cast<const unsigned*>(print.bits);
            Check(!print.Untouched()&&!std::equal(baselinePixels.begin(),baselinePixels.end(),pixels),
                "fresh native print renders the changed geometry rather than replaying the original DIB");}
        ce::CloseOwned(nullptr);host.data[0].sample={};host.applied.clear();
    }
    Note("nested redraw is deferred and same-HWND geometry changes invalidate prepared sampling output");
}
void SamplerLifetime(){
    SamplerCopyBoundary();SamplerReadAndBakeLifetime();SamplerNestedOperations();
    SamplerFailuresFallbackAndRedo();SamplerAcceptedBakeRecovery();SamplerHandleCancellation();SamplerDeferredRedrawAndGeometry();
}
// Phase-A transport writer probe: deliberately no lifetime-changing callback.
// The old direct std::function call is safe here and must fail the contract
// because it skips this copy constructor, including its prearranged failure.
struct TransportWriteCopyState {
    bool armed=false,failCopy=false,playing=false;
    double frame=5.0;
    unsigned copies=0,bodies=0,liveBodies=0;
    int argument=-1;
};
template<bool Playing>struct TransportWriteCopyProvider {
    TransportWriteCopyState* state;
    bool invocationCopy=false;
    explicit TransportWriteCopyProvider(TransportWriteCopyState& value):state(&value){}
    TransportWriteCopyProvider(const TransportWriteCopyProvider& other):state(other.state){
        invocationCopy=state->armed;
        if(invocationCopy){++state->copies;if(state->failCopy)throw std::bad_alloc();}
    }
    void operator()(std::conditional_t<Playing,bool,int> argument)const{
        ++state->bodies;if(!invocationCopy)++state->liveBodies;
        state->argument=int(argument);
        if constexpr(Playing)state->playing=argument;
        else state->frame=argument;
    }
};
void TransportWriteCopyBoundary(){
    bool allFailedCopiesBlocked=true,allSuccessfulCopiesOwned=true;
    for(bool playing:{false,true})for(bool failCopy:{true,false}){
        auto host=TransportHost();TransportWriteCopyState state;state.failCopy=failCopy;
        auto session=MakeSession(host);
        session.currentFrame=[&]{return state.frame;};session.isPlaying=[&]{return state.playing;};
        if(playing)session.setPlaying=TransportWriteCopyProvider<true>(state);
        else session.seek=TransportWriteCopyProvider<false>(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==3,"safe transport writer copy probe opens a real three-key editor");
        const auto token=ce::CaptureWindowSession(window);
        const auto history=LocalWriteHistory(window);
        Check(state.copies==0&&state.bodies==0,"opening the fixture never invokes or arms its transport writer");
        state.armed=true;
        if(playing)Click(window,kIdPlay);
        else{SetText(window,kIdTimelineFrame,L"8");Click(window,kIdSeekFrame);}
        const bool expectedValue=playing?state.playing:(state.frame==8.0);
        const bool unchanged=state.frame==5.0&&!state.playing;
        std::printf("transport writer copy playing=%u fail_copy=%u copies=%u bodies=%u live_bodies=%u argument=%d frame=%.17g playing_value=%u\n",
            playing?1u:0u,failCopy?1u:0u,state.copies,state.bodies,state.liveBodies,state.argument,state.frame,state.playing?1u:0u);
        std::fflush(stdout);
        Check(IsWindow(window)&&ce::IsWindowSessionCurrent(window,token)&&host.applyCalls==0&&
            KeyCount(window)==3&&LocalWriteHistory(window)==history,
            "safe copy-boundary observations preserve the same native editor, curve data and history");
        if(failCopy)allFailedCopiesBlocked&=state.copies==1&&state.bodies==0&&state.liveBodies==0&&unchanged;
        else allSuccessfulCopiesOwned&=state.copies==1&&state.bodies==1&&state.liveBodies==0&&
            state.argument==(playing?1:8)&&expectedValue;
        state.armed=false;ce::CloseOwned(nullptr);
        Check(!IsWindow(window),"close the safe writer probe before checking the aggregate red contract");
    }
    // Collect both writers before failing: the original implementation skips
    // both copies, executes both forbidden bodies and logs that exact result.
    Check(allFailedCopiesBlocked,"seek and setPlaying must copy the provider and reject a failed copy before invoking its body");
    Check(allSuccessfulCopiesOwned,"successful seek and setPlaying invoke one owned copy with the exact plain-value argument");
    Note("transport void writers honor the callable copy boundary before any lifetime-changing matrix runs");
}
int TransportControlCount(HWND window){
    int count=0;EnumChildWindows(window,[](HWND,LPARAM value)->BOOL{++*reinterpret_cast<int*>(value);return TRUE;},
        reinterpret_cast<LPARAM>(&count));return count;
}
void TransportControlsReady(HWND window,int count){
    Check(SendMessageW(window,kHookNativeConstruction,0,0)==3,"transport providers run only after native CreateWindowEx returns");
    Check(TransportControlCount(window)==count,"a getter sees the complete control tree after native creation returns");
    for(int id:{kIdCanvas,kIdMmdPanel,kIdChannelList,kIdFrameEdit,kIdValueEdit,kIdStatus,
        kIdPlay,kIdTimelineFrame,kIdSeekFrame,kIdPreviousFrame,kIdNextFrame,kIdPreviousKey,kIdNextKey})
        Check(IsWindow(GetDlgItem(window,id)),"all transport and editor controls exist before the first transport query");
}
void TransportRefreshSource(HWND window,const std::string& sourceId="source"){
    Check(IsWindow(window),"transport source notification targets a real editor");
    const auto session=ce::CaptureWindowSession(window);
    ce::NotifySourceChanged(nullptr,sourceId);
    MSG message{};bool dispatched=false;
    // Deliver the real notification posted by the public entry. Stop at the
    // exact Session boundary, so a nested replacement never receives an old
    // continuation's queued source work.
    while(ce::IsWindowSessionCurrent(window,session)&&
        PeekMessageW(&message,window,WM_APP+401,WM_APP+401,PM_REMOVE)){
        dispatched=true;DispatchMessageW(&message);
    }
    Check(dispatched,"transport refresh consumes an actual posted source-change message");
}

POINT TransportWriteRulerPoint(HWND window,int frame){
    CurveEditorAxisState axis{};
    Check(SendMessageW(window,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis))!=0,
        "transport writer uses the actual displayed ruler geometry");
    POINT point=AskPoint(window,kHookPointForFrame,frame);
    point.y=(axis.plot.top-1)/2;return point;
}
struct TransportWriteGesture {
    int entry=0,argument=8;
    HWND window=nullptr,canvas=nullptr;
    POINT point{};
    TransportWriteGesture(HWND value,int route):entry(route),window(value),canvas(GetDlgItem(value,kIdCanvas)){
        if(entry==0)SetText(window,kIdTimelineFrame,L"8");
        else if(entry==1)argument=1;
        else if(entry==2){point=AskPoint(window,kHookPointForKey,1);argument=10;}
        else{
            point=TransportWriteRulerPoint(window,8);
            if(entry>=4){
                const POINT before=TransportWriteRulerPoint(window,6);
                SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(before.x,before.y));
                Check(GetCapture()==canvas,"move and release probes start with a real captured ruler gesture");
            }
        }
    }
    void Run()const{
        if(entry==0)Click(window,kIdSeekFrame);
        else if(entry==1)Click(window,kIdPlay);
        else SendMessageW(canvas,entry==4?WM_MOUSEMOVE:entry==5?WM_LBUTTONUP:WM_LBUTTONDOWN,
            entry==5?0:MK_LBUTTON,MAKELPARAM(point.x,point.y));
    }
};

struct TransportWriteRetirementState {
    int phase=0,response=0,argument=-1,expectedArgument=0;
    bool armed=false,copying=false,playing=false;
    double frame=5;
    const void* original=nullptr;
    unsigned copies=0,bodies=0,publications=0,actions=0,destroyedDuringCopy=0;
    std::function<void()> action;
    void Run(){armed=false;++actions;action();}
    void ThrowResponse()const{
        if(response==1)throw std::runtime_error("transport writer failed");
        if(response==2)throw std::bad_alloc();
    }
};
template<bool Playing>struct TransportWriteRetirementProvider {
    TransportWriteRetirementState* state;
    int marker=109;
    bool invocationCopy=false;
    mutable bool invoked=false;
    explicit TransportWriteRetirementProvider(TransportWriteRetirementState& value):state(&value){}
    TransportWriteRetirementProvider(const TransportWriteRetirementProvider& other):state(other.state),marker(other.marker){
        invocationCopy=state->armed;if(!invocationCopy)return;++state->copies;
        if(state->phase!=0)return;
        state->copying=true;state->original=&other;state->Run();
        Check(state->destroyedDuringCopy==0&&other.marker==109,
            "transport writer keeps its original callable alive throughout a reentrant copy");
        state->copying=false;state->ThrowResponse();
    }
    ~TransportWriteRetirementProvider(){
        if(state->copying&&state->original==this)++state->destroyedDuringCopy;
        if(invocationCopy&&invoked&&state->armed&&state->phase==2)state->Run();
    }
    void operator()(std::conditional_t<Playing,bool,int> value)const{
        invoked=true;const bool active=state->armed;++state->bodies;++state->publications;
        state->argument=int(value);
        if constexpr(Playing)state->playing=value;
        else{state->frame=value;state->playing=false;}
        if(!active)return;
        Check(invocationCopy&&int(value)==state->expectedArgument,
            "transport writer receives one owned callable and the exact plain-value argument");
        if(state->phase==1)state->Run();
        Check(int(value)==state->expectedArgument&&marker==109,
            "transport argument and active callable remain valid after synchronous retirement");
        state->ThrowResponse();
    }
};
void TransportWriteCallbackLifetime(){
    // Explicit seek, play, key-click, ruler press, captured ruler move, release.
    for(int entry=0;entry!=6;++entry)for(int phase=0;phase!=3;++phase)
    for(int action=0;action!=3;++action)for(int response=0;response!=3;++response){
        std::printf("transport writer lifetime entry=%d phase=%d action=%d response=%d\n",entry,phase,action,response);
        std::fflush(stdout);
        auto host=TransportHost(),replacement=TransportHost();replacement.data[0].id="replacement";
        TransportWriteRetirementState state;state.phase=phase;state.response=response;
        HWND replacementWindow=nullptr;std::wstring replacementStatus,replacementField;
        int readsAfter=-1,replacementSeeks=0;
        auto session=MakeSession(host);
        session.currentFrame=[&]{return state.frame;};session.isPlaying=[&]{return state.playing;};
        if(entry==1)session.setPlaying=TransportWriteRetirementProvider<true>(state);
        else session.seek=TransportWriteRetirementProvider<false>(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window)&&KeyCount(window)==3,"transport writer lifetime fixture opens before arming");
        const TransportWriteGesture gesture(window,entry);state.expectedArgument=gesture.argument;
        const auto token=ce::CaptureWindowSession(window);const auto retained=token.lock();
        const auto data=host.data;const unsigned bodies=state.bodies,publications=state.publications;
        state.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){
                auto next=MakeSession(replacement);next.seek=[&](int){++replacementSeeks;};
                replacementWindow=static_cast<HWND>(ce::Open(nullptr,std::move(next),false));
                Check(IsWindow(replacementWindow),"writer retirement creates an actual replacement editor");
                if(action==1)Check(replacementWindow==window,"same-window writer retirement preserves its real HWND");
                replacementStatus=GetText(replacementWindow,kIdStatus);replacementField=GetText(replacementWindow,kIdFrameEdit);
            }
            readsAfter=host.readCalls;
        };
        state.armed=true;gesture.Run();
        if(state.actions!=1||state.bodies!=bodies+unsigned(phase!=0))
            std::fprintf(stderr,"TRANSPORT_WRITE entry=%d phase=%d action=%d response=%d actions=%u copies=%u bodies=%u before=%u\n",
                entry,phase,action,response,state.actions,state.copies,state.bodies,bodies);
        Check(state.actions==1&&state.copies==1&&state.bodies==bodies+unsigned(phase!=0)&&
            state.publications==publications+unsigned(phase!=0)&&state.destroyedDuringCopy==0,
            "writer copy, body and final release retire once without retrying or undoing an effective void call");
        Check(retained&&!ce::IsWindowSessionCurrent(window,token)&&host.readCalls==readsAfter&&
            host.applyCalls==0&&SameWritePayload(host.data,data),
            "retired transport never reads old channels, authors keys or accepts a retained old Session token");
        if(replacementWindow){
            Check(KeyCount(replacementWindow)==3&&replacement.applyCalls==0&&replacementSeeks==0&&
                LocalWriteHistory(replacementWindow)==std::array<std::uint64_t,2>{0,0}&&
                GetText(replacementWindow,kIdStatus)==replacementStatus&&GetText(replacementWindow,kIdFrameEdit)==replacementField,
                "old transport continuation cannot change replacement status, fields, channels or history");
            const HWND canvas=GetDlgItem(replacementWindow,kIdCanvas);
            Check(GetCapture()!=canvas,"same-window replacement does not inherit the retired ruler capture");
            const POINT point=TransportWriteRulerPoint(replacementWindow,8);
            SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x,point.y));
            SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
            Check(replacementSeeks==0&&replacement.applyCalls==0,
                "old ruler move and release cannot install a drag into the replacement Session");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(replacementWindow)==2&&replacementSeeks==1,
                "replacement remains editable after all old writer guards and message tails unwind");
        }else Check(OwnedCurveWindow()==nullptr,"closed writer Session leaves no native or registered ghost editor");
        ce::CloseOwned(nullptr);
    }
    Note("transport void writers preserve copy, invocation, final-release and mouse continuation lifetime");
}

struct TransportWriteFailureState {
    bool armed=false,playing=false;
    int failure=0,argument=-1,copies=0,bodies=0,effects=0;
    double frame=5;
};
template<bool Playing>struct TransportWriteFailureProvider {
    TransportWriteFailureState* state;
    explicit TransportWriteFailureProvider(TransportWriteFailureState& value):state(&value){}
    TransportWriteFailureProvider(const TransportWriteFailureProvider& other):state(other.state){
        if(!state->armed)return;++state->copies;
        if(state->failure<2){state->armed=false;
            if(state->failure==0)throw std::bad_alloc();
            throw std::runtime_error("writer copy failed");}
    }
    void operator()(std::conditional_t<Playing,bool,int> value)const{
        ++state->bodies;const bool fail=state->armed;state->armed=false;
        if(fail&&state->failure==2)throw std::runtime_error("writer before effect");
        if(fail&&state->failure==3)throw std::bad_alloc();
        ++state->effects;state->argument=int(value);
        if constexpr(Playing)state->playing=value;
        else state->frame=value;
        if(fail&&state->failure==4)throw std::runtime_error("writer after effect");
        if(fail&&state->failure==5)throw std::bad_alloc();
    }
};
void TransportWriteFailuresAndSync(){
    for(bool playing:{false,true})for(int failure=0;failure!=6;++failure){
        auto host=TransportHost();TransportWriteFailureState state;state.failure=failure;
        auto session=MakeSession(host);session.currentFrame=[&]{return state.frame;};session.isPlaying=[&]{return state.playing;};
        if(playing)session.setPlaying=TransportWriteFailureProvider<true>(state);
        else session.seek=TransportWriteFailureProvider<false>(state);
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"ordinary writer failure fixture opens");
        const auto token=ce::CaptureWindowSession(window);const auto history=LocalWriteHistory(window);
        state.armed=true;SetText(window,kIdTimelineFrame,L"8");Click(window,playing?kIdPlay:kIdSeekFrame);
        Check(state.copies==1&&state.bodies==int(failure>=2)&&state.effects==int(failure>=4)&&
            host.applyCalls==0&&ce::IsWindowSessionCurrent(window,token)&&LocalWriteHistory(window)==history,
            "copy/runtime/allocation failures preserve the Session and never retry a possibly effective void writer");
        Check(playing?(state.playing==(failure>=4)):(state.frame==(failure>=4?8:5)),
            "writer failure keeps the host's actual pre-effect or post-effect transport state");
        SetText(window,kIdTimelineFrame,L"9");Click(window,playing?kIdPlay:kIdSeekFrame);
        Check(state.bodies==int(failure>=2)+1&&state.effects==int(failure>=4)+1&&
            (playing?state.playing==(failure<4):state.frame==9),
            "a later explicit gesture is allowed after every writer failure guard unwinds");
        ce::CloseOwned(nullptr);
    }
    for(bool playing:{false,true})for(bool allocation:{false,true}){
        auto host=TransportHost();bool failUi=false,playingValue=false;double frame=5;
        int writes=0,failures=0;auto session=MakeSession(host);
        const auto fail=[&]{++failures;if(allocation)throw std::bad_alloc();throw std::runtime_error("post-writer UI query");};
        session.currentFrame=[&]()->double{if(failUi&&!playing)fail();return frame;};
        session.isPlaying=[&]()->bool{if(failUi&&playing)fail();return playingValue;};
        session.seek=[&](int value){++writes;frame=value;failUi=true;};
        session.setPlaying=[&](bool value){++writes;playingValue=value;failUi=true;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"post-writer UI failure fixture opens");
        const auto history=LocalWriteHistory(window);SetText(window,kIdTimelineFrame,L"8");
        // Visibility enables the ordinary playhead UI path without activating
        // or sending keyboard/mouse input outside this fixture.
        SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        Click(window,playing?kIdPlay:kIdSeekFrame);
        Check(writes==1&&failures>0&&(playing?playingValue:frame==8)&&host.applyCalls==0&&LocalWriteHistory(window)==history,
            "UI query exceptions after an effective writer do not retry it or create curve history");
        failUi=false;ce::SetLanguage(nullptr,2);RenderInk(GetDlgItem(window,kIdCanvas));
        Check(writes==1,"recovering UI display never retries an earlier successful transport write");
        ce::CloseOwned(nullptr);
    }
    Note("transport writer failures and post-effect UI failures never trigger implicit retry");
}

void TransportWriteNestedAndSemantics(){
    for(bool playing:{false,true})for(int phase=0;phase!=3;++phase){
        auto host=TransportHost();TransportWriteRetirementState state;state.phase=phase;
        HWND window=nullptr;int frameReads=0,playingReads=0;
        auto session=MakeSession(host);
        session.currentFrame=[&]{++frameReads;return state.frame;};session.isPlaying=[&]{++playingReads;return state.playing;};
        if(playing)session.setPlaying=TransportWriteRetirementProvider<true>(state);
        else session.seek=TransportWriteRetirementProvider<false>(state);
        window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"nested writer fixture opens");
        SetText(window,kIdTimelineFrame,L"8");state.expectedArgument=playing?1:8;
        state.action=[&]{
            const int frames=frameReads,states=playingReads,reads=host.readCalls;
            const auto bodies=state.bodies;const auto history=LocalWriteHistory(window);
            const auto fields=std::make_pair(GetText(window,kIdFrameEdit),GetText(window,kIdValueEdit));
            std::array<int,12> selection{},afterSelection{};ReadCompare(window,selection.data());
            const HWND canvas=GetDlgItem(window,kIdCanvas);
            const POINT point=AskPoint(window,kHookPointForKey,1);
            for(int id:std::array<int,8>{kIdPlay,kIdSeekFrame,kIdNextFrame,kIdNextKey,kIdDeleteKey,kIdAddKey,kIdUndo,kIdRedo})Click(window,id);
            SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
            SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(point.x+4,point.y));
            SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(point.x+4,point.y));
            RenderInk(canvas);SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);ce::SetLanguage(nullptr,2);
            TransportRefreshSource(window);
            ReadCompare(window,afterSelection.data());
            Check(frameReads==frames&&playingReads==states&&state.bodies==bodies&&host.readCalls==reads&&host.applyCalls==0&&
                LocalWriteHistory(window)==history&&afterSelection==selection&&
                GetText(window,kIdFrameEdit)==fields.first&&GetText(window,kIdValueEdit)==fields.second,
                "copy/body/final-release message pumps cannot recursively query, seek, play, select or author during a transport write");
        };
        const auto history=LocalWriteHistory(window);state.armed=true;Click(window,playing?kIdPlay:kIdSeekFrame);
        Check(state.actions==1&&state.copies==1&&state.bodies==1&&state.publications==1&&
            state.argument==state.expectedArgument&&host.applyCalls==0&&LocalWriteHistory(window)==history,
            "the outer writer completes once after nested transport, authoring and refresh requests are rejected or delayed");
        // Pending source notifications can be consumed after the callback gate.
        TransportRefreshSource(window);ClickKey(window,1);Click(window,kIdDeleteKey);
        Check(host.applyCalls==1&&KeyCount(window)==2,"normal editing and pending source work resume after the writer boundary");
        ce::CloseOwned(nullptr);
    }
    auto host=TransportHost();double frame=5;bool playing=false;int seeks=0,toggles=0,hostStops=0;
    auto session=MakeSession(host);session.currentFrame=[&]{return frame;};session.isPlaying=[&]{return playing;};
    session.seek=[&](int next){++seeks;++hostStops;playing=false;frame=next;};
    session.setPlaying=[&](bool next){++toggles;playing=next;};
    const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"transport compatibility fixture opens");
    const auto history=LocalWriteHistory(window);SetText(window,kIdTimelineFrame,L"5");Click(window,kIdSeekFrame);
    Check(seeks==0&&toggles==0,"a stopped same-frame seek remains a no-op");
    playing=true;Click(window,kIdSeekFrame);
    Check(seeks==1&&hostStops==1&&!playing&&toggles==0,
        "a playing same-frame seek delegates its single stop to the host without adding an editor setPlaying(false)");
    Click(window,kIdPlay);Click(window,kIdNextFrame);Click(window,kIdNextKey);
    Check(toggles==1&&seeks==3&&hostStops==3&&frame==10&&!playing&&host.applyCalls==0&&LocalWriteHistory(window)==history,
        "play, adjacent-frame and adjacent-key commands retain their existing host transport semantics");
    DragKeyToFrame(window,1,14);
    Check(host.applyCalls==1&&FindKey(host.data,0,14)&&KeyCount(window)==3,
        "a normal key press can still install and commit its drag after a same-frame seek");
    ce::CloseOwned(nullptr);
    for(bool noSeek:{false,true}){
        auto optional=TransportHost();auto absent=MakeSession(optional);int calls=0;
        absent.currentFrame=[](){return 5.0;};absent.isPlaying=[](){return false;};
        if(noSeek){absent.seek={};absent.setPlaying=[&](bool){++calls;};}
        else{absent.setPlaying={};absent.seek=[&](int){++calls;};}
        const HWND target=static_cast<HWND>(ce::Open(nullptr,std::move(absent),false));Check(IsWindow(target),"missing writer fixture opens");
        const auto before=LocalWriteHistory(target);SetText(target,kIdTimelineFrame,L"8");Click(target,noSeek?kIdSeekFrame:kIdPlay);
        Check(calls==0&&optional.applyCalls==0&&KeyCount(target)==3&&LocalWriteHistory(target)==before,
            "an absent optional writer remains a safe no-op with no fallback transport or history publication");
        ce::CloseOwned(nullptr);
    }
    Note("transport writers reject nested edits while preserving optional, same-frame, host-stop and key-drag behavior");
}
struct TransportCaptureReplacement {
    bool armed=false;
    unsigned calls=0;
    std::function<void()> action;
    static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
        auto& self=*reinterpret_cast<TransportCaptureReplacement*>(data);
        if(message==WM_NCDESTROY)RemoveWindowSubclass(window,Proc,id);
        if(message==WM_CAPTURECHANGED&&self.armed){
            self.armed=false;++self.calls;
            Check(GetCapture()!=window,"Open's capture release produces the actual synchronous native capture-change notification");
            self.action();return 0;
        }
        return DefSubclassProc(window,message,wp,lp);
    }
};
void TransportOpenGestureRetirement(){
    for(int action=0;action!=3;++action){
        auto host=TransportHost(),attempted=TransportHost(),replacement=TransportHost();
        double frame=5;int seeks=0;auto session=MakeSession(host);
        session.currentFrame=[&]{return frame;};session.isPlaying=[](){return false;};
        session.seek=[&](int next){++seeks;frame=next;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"capture-release replacement fixture opens");
        const HWND canvas=GetDlgItem(window,kIdCanvas);const POINT point=TransportWriteRulerPoint(window,6);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        Check(GetCapture()==canvas&&seeks==1,"old Session owns its real ruler capture before same-window Open");
        const auto token=ce::CaptureWindowSession(window);TransportCaptureReplacement probe;
        HWND replacementWindow=nullptr;std::wstring status;
        probe.action=[&]{
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(replacementWindow),"capture-change callback installs a live replacement Session");
                status=GetText(replacementWindow,kIdStatus);}
        };
        Check(SetWindowSubclass(canvas,TransportCaptureReplacement::Proc,0x7a61,reinterpret_cast<DWORD_PTR>(&probe))!=FALSE,
            "watch actual native capture release on the fixture's own canvas");
        probe.armed=true;
        Check(ce::Open(nullptr,MakeSession(attempted),false)==nullptr,
            "an outer Open cannot publish after its capture-release callback replaces or closes the original Session");
        Check(probe.calls==1&&!ce::IsWindowSessionCurrent(window,token)&&attempted.readCalls==0&&attempted.applyCalls==0&&host.applyCalls==0,
            "capture-release retirement aborts before the attempted Session's providers or history become live");
        if(replacementWindow){
            Check(GetText(replacementWindow,kIdStatus)==status&&KeyCount(replacementWindow)==3&&GetCapture()!=GetDlgItem(replacementWindow,kIdCanvas),
                "aborted outer Open leaves the nested replacement's controls and capture untouched");
            ClickKey(replacementWindow,1);Click(replacementWindow,kIdDeleteKey);
            Check(replacement.applyCalls==1&&KeyCount(replacementWindow)==2,
                "a replacement created by capture-change notification remains editable");
        }else Check(OwnedCurveWindow()==nullptr,"capture-release close leaves no attempted editor registered");
        if(IsWindow(canvas))RemoveWindowSubclass(canvas,TransportCaptureReplacement::Proc,0x7a61);
        ce::CloseOwned(nullptr);
    }
    {
        auto host=TransportHost(),attempted=TransportHost();double frame=5;int seeks=0;
        auto session=MakeSession(host);session.currentFrame=[&]{return frame;};session.isPlaying=[](){return false;};
        session.seek=[&](int next){++seeks;frame=next;};
        const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(IsWindow(window),"Open preparation allocation-failure fixture opens");
        const HWND canvas=GetDlgItem(window,kIdCanvas);const POINT point=TransportWriteRulerPoint(window,6);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        auto candidate=MakeSession(attempted);const auto token=ce::CaptureWindowSession(window);
        const unsigned failures=g_curveAllocationFailures;g_curveFailNextAllocation=true;
        Check(ce::Open(nullptr,std::move(candidate),false)==nullptr,"private Open preparation reports its real allocation failure");
        Check(!g_curveFailNextAllocation&&g_curveAllocationFailures==failures+1&&
            ce::IsWindowSessionCurrent(window,token)&&GetCapture()==canvas&&attempted.readCalls==0&&host.applyCalls==0,
            "failed private preparation leaves the original Session and its active gesture intact");
        const POINT end=TransportWriteRulerPoint(window,8);
        SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(end.x,end.y));
        Check(seeks==2&&frame==8&&GetCapture()!=canvas,"the original ruler can finish after an unrelated Open preparation failure");
        ce::CloseOwned(nullptr);
    }
    {
        HostSource host;auto channel=MakeChannel("handle",L"Handle",{
            MakeKey(0,0,ac::Interpolation::Bezier,ac::Tangent::Free),
            MakeKey(20,5,ac::Interpolation::Bezier,ac::Tangent::Free),
            MakeKey(40,10,ac::Interpolation::Bezier,ac::Tangent::Free)});
        channel.track.keys[1].inWeight=channel.track.keys[1].outWeight=.3;
        channel.track.keys[1].inSlope=channel.track.keys[1].outSlope=0;host.data.push_back(std::move(channel));
        const HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
        Check(IsWindow(window),"allocation-free private handle cancellation fixture opens");ClickKey(window,1);
        const HWND canvas=GetDlgItem(window,kIdCanvas);const auto original=RenderPixels(canvas);
        const POINT key=AskPoint(window,kHookPointForKey,1),handle=AskPoint(window,kHookPointForFrame,26),target=AskPoint(window,kHookPointForFrame,30);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(handle.x,key.y));
        SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x,key.y-20));
        Check(GetCapture()==canvas&&RenderPixels(canvas)!=original&&host.applyCalls==0,
            "a real captured handle changes only its private preview before cancellation");
        const auto history=LocalWriteHistory(window);const auto token=ce::CaptureWindowSession(window);
        const unsigned failures=g_curveAllocationFailures;g_curveFailNextAllocation=true;
        SendMessageW(canvas,WM_KEYDOWN,VK_ESCAPE,0);
        const bool allocationPending=g_curveFailNextAllocation;g_curveFailNextAllocation=false;
        Check(allocationPending&&g_curveAllocationFailures==failures&&ce::IsWindowSessionCurrent(window,token)&&
            GetCapture()!=canvas&&host.applyCalls==0&&LocalWriteHistory(window)==history&&RenderPixels(canvas)==original,
            "CancelDrag restores the saved complete keys by swap without allocating, publishing or losing its original Session");
        ce::CloseOwned(nullptr);
    }
    Note("same-window Open retires old gestures before publication and survives native capture-change replacement");
}
void TransportWriteLifetime(){
    TransportWriteCopyBoundary();TransportWriteCallbackLifetime();TransportWriteFailuresAndSync();TransportWriteNestedAndSemantics();TransportOpenGestureRetirement();
}
struct TransportRetirementState {
    int phase=0,response=0,calls=0,retireCalls=0,destroyedDuringCopy=0,skipBodies=0;
    bool armed=false,copying=false;const void* original=nullptr;
    std::function<void()> retire;
    void Retire(){armed=false;++retireCalls;retire();}
};
template<bool Playing>struct TransportRetirementProvider {
    TransportRetirementState* state;int marker=83;mutable bool invoked=false;
    explicit TransportRetirementProvider(TransportRetirementState& value):state(&value){}
    TransportRetirementProvider(const TransportRetirementProvider& other):state(other.state),marker(other.marker){
        if(!state->armed||state->skipBodies||state->phase!=0)return;
        state->copying=true;state->original=&other;state->Retire();
        Check(state->destroyedDuringCopy==0&&other.marker==83,
            "transport callable storage stays alive during a copy constructor that replaces its Session");
        state->copying=false;if(state->response)throw std::bad_alloc();
    }
    ~TransportRetirementProvider(){
        if(state->copying&&state->original==this)++state->destroyedDuringCopy;
        if(invoked&&state->armed&&state->phase==2)state->Retire();
    }
    auto operator()()const{
        ++state->calls;
        if(state->armed&&state->skipBodies){--state->skipBodies;if constexpr(Playing)return false;else return 5.0;}
        invoked=state->armed;const bool fail=state->armed&&state->response;
        if(state->armed&&state->phase==1)state->Retire();
        if(fail)throw std::runtime_error("transport query failed");
        if constexpr(Playing)return false;else return 5.0;
    }
};
void TransportEntry(HWND window,int entry){
    if(entry==2)RenderInk(GetDlgItem(window,kIdCanvas));
    if(entry==3){HWND canvas=GetDlgItem(window,kIdCanvas);InvalidateRect(canvas,nullptr,FALSE);SendMessageW(canvas,WM_PAINT,0,0);}
    if(entry==4)TransportRefreshSource(window);
    if(entry==5||entry==9||entry==13){SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);}
    if(entry==6)Click(window,kIdNextFrame);
    if(entry==7)Click(window,kIdNextKey);
    if(entry==8)ce::SetLanguage(nullptr,1);
    if(entry==10)Click(window,kIdPlay);
    if(entry==11||entry==12){SetText(window,kIdTimelineFrame,L"8");Click(window,kIdSeekFrame);}
}
void TransportCallbackLifetime(){
    auto baseline=TransportHost();HWND probe=static_cast<HWND>(ce::Open(nullptr,MakeSession(baseline),false));
    Check(IsWindow(probe),"transport control-count baseline opens");const int count=TransportControlCount(probe);ce::CloseOwned(nullptr);
    for(int entry=0;entry!=14;++entry)for(int phase=0;phase!=3;++phase)
    for(int action=0;action!=3;++action)for(int response=0;response!=2;++response){
        std::printf("transport query lifetime entry=%d phase=%d action=%d response=%d\n",entry,phase,action,response);
        std::fflush(stdout);
        auto host=TransportHost(),replacement=TransportHost();TransportRetirementState state;
        state.phase=phase;state.response=response;int seeks=0,toggles=0;const int skipped=entry==13?1:0;
        HWND retiredWindow=nullptr,replacementWindow=nullptr;std::wstring replacementStatus;
        std::weak_ptr<void> token;std::shared_ptr<void> retained;int readsAfter=-1;
        state.retire=[&]{retiredWindow=OwnedCurveWindow();TransportControlsReady(retiredWindow,count);
            token=ce::CaptureWindowSession(retiredWindow);retained=token.lock();
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){replacementWindow=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(replacementWindow),"transport callback installs a real replacement editor");
                replacementStatus=GetText(replacementWindow,kIdStatus);}
            readsAfter=host.readCalls;
        };
        auto session=MakeSession(host);session.currentFrame=[](){return 5.0;};session.isPlaying=[](){return false;};
        session.seek=[&](int){++seeks;};session.setPlaying=[&](bool){++toggles;};
        const bool playing=entry==1||(entry>=8&&entry<=11);
        if(playing)session.isPlaying=TransportRetirementProvider<true>(state);
        else session.currentFrame=TransportRetirementProvider<false>(state);
        const bool initial=entry<2;state.armed=initial;
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));int beforeCalls=0;
        if(initial)Check(window==nullptr,"retired startup query cannot return an old Open as successful");
        else{Check(IsWindow(window),"transport lifetime fixture opens before arming");beforeCalls=state.calls;
            state.skipBodies=skipped;state.armed=true;TransportEntry(window,entry);}
        if(state.retireCalls!=1||state.calls!=beforeCalls+skipped+int(phase!=0))
            std::fprintf(stderr,"TRANSPORT_RETIRE entry=%d phase=%d action=%d response=%d calls=%d before=%d retires=%d\n",
                entry,phase,action,response,state.calls,beforeCalls,state.retireCalls);
        Check(state.retireCalls==1&&state.calls==beforeCalls+skipped+int(phase!=0)&&state.destroyedDuringCopy==0,
            "transport query copying, invocation and final release retire precisely one original callback");
        Check(seeks==0&&toggles==0&&host.applyCalls==0&&host.readCalls==readsAfter,
            "retired read-only transport queries never continue into seeking, playback, source reads or apply");
        Check(retained&&!ce::IsWindowSessionCurrent(retiredWindow,token),"a strong old token cannot validate a retired transport query");
        if(replacementWindow){TransportControlsReady(replacementWindow,count);
            Check(GetText(replacementWindow,kIdStatus)==replacementStatus&&KeyCount(replacementWindow)==3,
                "retired query continuation leaves replacement controls, data and status unchanged");
            // Keep the input sequence free of diagnostic heap allocations and
            // extra editor messages. Only fixed-capacity scalar observations
            // run between the original DOWN/UP/Delete actions; collect richer
            // evidence only after the unchanged assertion has failed.
            const HWND recoveryCanvas=GetDlgItem(replacementWindow,kIdCanvas);
            Check(recoveryCanvas!=nullptr,"the canvas exists");
            const POINT recoveryPoint=AskPoint(replacementWindow,kHookPointForKey,1);
            Check(recoveryPoint.x>0&&recoveryPoint.y>0,"the queried key lies inside the canvas");
            const auto recoveryToken=ce::CaptureWindowSession(replacementWindow);
            struct RecoveryObservation {
                HWND focus=nullptr,capture=nullptr;
                std::array<SHORT,6> keys{};
                bool current=false,oldCurrent=false;
                bool windowEnabled=false,canvasEnabled=false,deleteEnabled=false,applyEnabled=false;
                int applies=0,reads=0;
            };
            const auto observe=[&]{
                RecoveryObservation observation;
                observation.focus=GetFocus();observation.capture=GetCapture();
                observation.keys={GetKeyState(VK_SPACE),GetKeyState(VK_MENU),GetKeyState(VK_CONTROL),GetKeyState(VK_SHIFT),
                    GetKeyState(VK_LBUTTON),GetKeyState(VK_RBUTTON)};
                observation.current=ce::IsWindowSessionCurrent(replacementWindow,recoveryToken);
                observation.oldCurrent=ce::IsWindowSessionCurrent(retiredWindow,token);
                observation.windowEnabled=IsWindowEnabled(replacementWindow)!=FALSE;
                observation.canvasEnabled=IsWindowEnabled(recoveryCanvas)!=FALSE;
                observation.deleteEnabled=IsWindowEnabled(GetDlgItem(replacementWindow,kIdDeleteKey))!=FALSE;
                observation.applyEnabled=IsWindowEnabled(GetDlgItem(replacementWindow,kIdApplyKey))!=FALSE;
                observation.applies=replacement.applyCalls;observation.reads=replacement.readCalls;return observation;
            };
            const auto beforeClick=observe();
            const LPARAM recoveryPosition=MAKELPARAM(recoveryPoint.x,recoveryPoint.y);
            SendMessageW(recoveryCanvas,WM_LBUTTONDOWN,MK_LBUTTON,recoveryPosition);
            const auto afterDown=observe();
            SendMessageW(recoveryCanvas,WM_LBUTTONUP,0,recoveryPosition);
            const auto afterUp=observe();
            Click(replacementWindow,kIdDeleteKey);
            const auto afterDelete=observe();
            const int recoveryKeys=KeyCount(replacementWindow);
            if(replacement.applyCalls!=1||recoveryKeys!=2){
                const auto printText=[](const char* label,const std::wstring& text){
                    std::fprintf(stderr," %s=\"",label);
                    for(wchar_t unit:text){
                        if(unit>=32&&unit<127&&unit!=L'\\'&&unit!=L'\"')std::fputc(int(unit),stderr);
                        else std::fprintf(stderr,"\\u%04x",unsigned(static_cast<unsigned short>(unit)));
                    }
                    std::fputc('"',stderr);
                };
                const auto print=[&](const char* stage,const RecoveryObservation& observation){
                    std::fprintf(stderr,"TRANSPORT_RECOVERY entry=%d phase=%d action=%d response=%d stage=%s hwnd=%p canvas=%p point=%ld,%ld focus=%p capture=%p keys_space_alt_ctrl_shift_left_right=%04x,%04x,%04x,%04x,%04x,%04x current=%d oldCurrent=%d windowEnabled=%d canvasEnabled=%d deleteEnabled=%d applyEnabled=%d applies=%d reads=%d\n",
                        entry,phase,action,response,stage,replacementWindow,recoveryCanvas,recoveryPoint.x,recoveryPoint.y,
                        observation.focus,observation.capture,
                        unsigned(static_cast<unsigned short>(observation.keys[0])),unsigned(static_cast<unsigned short>(observation.keys[1])),
                        unsigned(static_cast<unsigned short>(observation.keys[2])),unsigned(static_cast<unsigned short>(observation.keys[3])),
                        unsigned(static_cast<unsigned short>(observation.keys[4])),unsigned(static_cast<unsigned short>(observation.keys[5])),
                        observation.current,observation.oldCurrent,observation.windowEnabled,observation.canvasEnabled,
                        observation.deleteEnabled,observation.applyEnabled,observation.applies,observation.reads);
                };
                print("before",beforeClick);print("down",afterDown);print("up",afterUp);print("delete",afterDelete);
                // These messages may measure/sample or allocate. They are
                // deliberately absent from every successful input sequence.
                CurveEditorAxisState geometry{};
                std::array<std::uint64_t,5> history{};std::array<int,12> comparison{};
                const bool geometryReady=SendMessageW(replacementWindow,WM_APP+394,0,reinterpret_cast<LPARAM>(&geometry))!=0;
                const bool historyReady=SendMessageW(replacementWindow,WM_APP+397,0,reinterpret_cast<LPARAM>(history.data()))!=0;
                ReadCompare(replacementWindow,comparison.data());
                std::fprintf(stderr,"TRANSPORT_RECOVERY_FAILURE_ONLY keyCount=%d geometryReady=%d plot=%ld,%ld,%ld,%ld time=%.17g,%.17g value=%.17g,%.17g viewMode=%d historyReady=%d history=%llu,%llu,%llu,%llu,%llu comparison=",
                    recoveryKeys,geometryReady,geometry.plot.left,geometry.plot.top,geometry.plot.right,geometry.plot.bottom,
                    geometry.timeStart,geometry.timeEnd,geometry.viewLow,geometry.viewHigh,geometry.viewMode,historyReady,
                    static_cast<unsigned long long>(history[0]),static_cast<unsigned long long>(history[1]),
                    static_cast<unsigned long long>(history[2]),static_cast<unsigned long long>(history[3]),static_cast<unsigned long long>(history[4]));
                for(std::size_t index=0;index<comparison.size();++index)std::fprintf(stderr,index?",%d":"%d",comparison[index]);
                printText("frame",GetText(replacementWindow,kIdFrameEdit));printText("value",GetText(replacementWindow,kIdValueEdit));
                printText("status",GetText(replacementWindow,kIdStatus));std::fputc('\n',stderr);std::fflush(stderr);
            }
            Check(replacement.applyCalls==1&&recoveryKeys==2,
                "replacement Session remains editable after startup, paint, timer and navigation retirement");}
        else Check(OwnedCurveWindow()==nullptr,"transport retirement leaves no registered ghost editor");
        ce::CloseOwned(nullptr);
    }
    Note("frame and playing getters share the Session copy, invocation and final-release lifetime boundary");
}

struct TransportFailureState{bool armed=false;int failure=0,copies=0,calls=0;};
template<bool Playing>struct TransportFailureProvider{
    TransportFailureState* state;
    explicit TransportFailureProvider(TransportFailureState& value):state(&value){}
    TransportFailureProvider(const TransportFailureProvider& other):state(other.state){
        if(state->armed&&state->failure==0){++state->copies;throw std::bad_alloc();}}
    auto operator()()const{
        ++state->calls;if(state->armed&&state->failure==1)throw std::runtime_error("transport unavailable");
        if constexpr(Playing)return false;
        else{if(state->armed&&state->failure==2)return std::numeric_limits<double>::quiet_NaN();
            if(state->armed&&state->failure==3)return std::numeric_limits<double>::infinity();
            if(state->armed&&state->failure==4)return (std::numeric_limits<double>::max)();return 5.0;}
    }
};
void TransportFailuresAndMissing(){
    for(bool playing:{false,true})for(int failure=0;failure!=(playing?2:5);++failure){
        std::printf("transport query failure playing=%d failure=%d\n",playing,failure);std::fflush(stdout);
        auto host=TransportHost();TransportFailureState state;state.failure=failure;int seeks=0,toggles=0;
        auto session=MakeSession(host);session.currentFrame=[](){return 5.0;};session.isPlaying=[](){return false;};
        session.seek=[&](int){++seeks;};session.setPlaying=[&](bool){++toggles;};
        if(playing)session.isPlaying=TransportFailureProvider<true>(state);
        else session.currentFrame=TransportFailureProvider<false>(state);
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"query failure fixture opens with valid initial cache");
        const auto history=LocalWriteHistory(window);const auto frame=GetText(window,kIdFrameEdit),value=GetText(window,kIdValueEdit);
        const auto caption=GetText(window,kIdPlay);const int applies=host.applyCalls;
        state.armed=true;SetText(window,kIdTimelineFrame,L"8");
        for(int id:{kIdPreviousFrame,kIdNextFrame,kIdPreviousKey,kIdNextKey,kIdSeekFrame})Click(window,id);
        if(playing)Click(window,kIdPlay);
        else{TransportRefreshSource(window);RenderInk(GetDlgItem(window,kIdCanvas));}
        ce::SetLanguage(nullptr,2);
        Check(seeks==0&&toggles==0&&host.applyCalls==applies&&LocalWriteHistory(window)==history,
            "failed or invalid fresh queries cannot authorize navigation using an older cached value");
        Check(GetText(window,kIdFrameEdit)==frame&&GetText(window,kIdValueEdit)==value&&GetText(window,kIdPlay)==caption,
            "query failure preserves the last valid fields and caption without inventing a new stopped state");
        if(failure==0)Check(state.copies>0,"ordinary transport callable copy allocation failures reach the guarded failure path");
        state.armed=false;Click(window,kIdNextFrame);Check(seeks==1,"navigation resumes after the provider becomes available");
        ce::CloseOwned(nullptr);
    }
    auto host=TransportHost();int seeks=0,toggles=0;bool lastPlaying=false;auto session=MakeSession(host);
    session.currentFrame={};session.isPlaying={};session.seek=[&](int frame){++seeks;Check(frame==8,"missing query explicit seek keeps its entered frame");};
    session.setPlaying=[&](bool playing){++toggles;lastPlaying=playing;};
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"optional transport getter fixture opens");
    Click(window,kIdNextFrame);Click(window,kIdNextKey);Check(seeks==0,"missing frame getter disables frame-relative navigation");
    SetText(window,kIdTimelineFrame,L"8");Click(window,kIdSeekFrame);Click(window,kIdPlay);
    Check(seeks==1&&toggles==1&&lastPlaying&&host.applyCalls==0,
        "missing optional getters retain explicit seek and default-stopped play behavior");
    ce::CloseOwned(nullptr);
    Note("missing, busy, failed and invalid queries retain distinct navigation semantics");
}

void TransportNestedAndSessionCache(){
    for(bool playing:{false,true}){
        std::printf("transport query nested playing=%d\n",playing);std::fflush(stdout);
        auto host=TransportHost(),other=TransportHost();other.data[0].id="other";HWND window=nullptr;
        bool armed=false,pumping=false;int frames=0,states=0,nested=0,seeks=0,toggles=0;
        const auto pump=[&]{armed=false;pumping=true;const int oldFrames=frames,oldStates=states;
            const auto history=LocalWriteHistory(window);const auto frame=GetText(window,kIdFrameEdit),value=GetText(window,kIdValueEdit);
            const int active=ActiveChannelIndex(window);const int reads=host.readCalls;
            RenderInk(GetDlgItem(window,kIdCanvas));InvalidateRect(GetDlgItem(window,kIdCanvas),nullptr,FALSE);
            SendMessageW(GetDlgItem(window,kIdCanvas),WM_PAINT,0,0);
            SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);ce::SetLanguage(nullptr,1);
            for(int id:{kIdNextFrame,kIdPlay,kIdDeleteKey})Click(window,id);
            HWND combo=GetDlgItem(window,kIdSourceCombo);SendMessageW(combo,CB_SETCURSEL,1,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdSourceCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
            TransportRefreshSource(window);
            Check(frames==oldFrames&&states==oldStates&&nested==0,"nested paint, fields, language and timer messages never recursively query transport");
            Check(seeks==0&&toggles==0&&host.applyCalls==0&&host.readCalls==reads&&LocalWriteHistory(window)==history&&
                other.readCalls==0&&GetText(window,kIdFrameEdit)==frame&&GetText(window,kIdValueEdit)==value&&ActiveChannelIndex(window)==active&&KeyCount(window)==3,
                "busy transport queries preserve selection, source, fields, history and data before their outer call returns");
            pumping=false;};
        auto session=MakeSession(host);session.sources.push_back(other.Make("other",L"Other source"));
        session.currentFrame=[&]{++frames;if(pumping)++nested;if(armed&&!playing)pump();return 5.0;};
        session.isPlaying=[&]{++states;if(pumping)++nested;if(armed&&playing)pump();return false;};
        session.seek=[&](int){++seeks;};session.setPlaying=[&](bool){++toggles;};
        window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"transport recursion fixture opens");
        SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);armed=true;
        if(playing)ce::SetLanguage(nullptr,2);else RenderInk(GetDlgItem(window,kIdCanvas));
        Check(!armed&&nested==0&&seeks==0&&toggles==0,"the owning transport query completes once after nested authoring is rejected");
        // A Session-wide getter can be used normally by either source after the callback has returned.
        HWND combo=GetDlgItem(window,kIdSourceCombo);SendMessageW(combo,CB_SETCURSEL,1,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdSourceCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
        Check(other.readCalls>0&&host.applyCalls==0,"source switching resumes after the transport callback guard releases");
        auto replacement=MakeSession(other);int failed=0;bool sawOldField=false;
        replacement.currentFrame=[&]()->double{++failed;
            sawOldField|=GetText(window,kIdFrameEdit)==L"5";
            ce::SetLanguage(nullptr,2);RenderInk(GetDlgItem(window,kIdCanvas));
            throw std::runtime_error("new Session has no current frame");};
        replacement.isPlaying=[]()->bool{throw std::runtime_error("new Session has no playing state");};
        Check(ce::Open(nullptr,std::move(replacement),false)==window,"cache reset is exercised through same-window Open");
        Check(failed>0&&!sawOldField&&GetText(window,kIdFrameEdit).empty()&&GetText(window,kIdValueEdit).empty()&&GetText(window,kIdPlay)==L"播放",
            "display caches and fields are not inherited by a replacement Session whose first query fails or reenters");
        ce::CloseOwned(nullptr);
    }
    Note("transport callback busy state uses only current-Session display caches and gates recursive editing");
}

void TransportInvalidFramesAndMmdSource(){
    const double edge=double((std::numeric_limits<long long>::min)());
    const std::array<double,7> values{{std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),-edge,
        std::nextafter(edge,-std::numeric_limits<double>::infinity()),edge,std::nextafter(-edge,0.0)}};
    for(std::size_t test=0;test<values.size();++test){
        std::printf("transport query invalid-frame case=%zu\n",test);std::fflush(stdout);
        auto host=TransportHost();double frame=5.0;int seeks=0;auto session=MakeSession(host);
        session.currentFrame=[&]{return frame;};session.seek=[&](int){++seeks;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"invalid-frame selected-key fixture opens");
        const auto oldFrame=GetText(window,kIdFrameEdit),oldValue=GetText(window,kIdValueEdit);frame=values[test];TransportRefreshSource(window);
        const bool valid=test>=5;
        if(valid)Check(GetText(window,kIdFrameEdit)==std::to_wstring(static_cast<long long>(std::llround(frame))),
            "representable signed 64-bit frame boundaries round without Win32 long overflow");
        else Check(GetText(window,kIdFrameEdit)==oldFrame&&GetText(window,kIdValueEdit)==oldValue,
            "a nonfinite or out-of-range clock does not overwrite fields when no key is selected");
        ClickKey(window,1);
        Check(GetText(window,kIdFrameEdit)==L"10"&&GetText(window,kIdValueEdit)==L"3"&&host.applyCalls==0,
            "a selected key displays its own frame and value independently of an invalid or extreme clock");
        if(!valid){Click(window,kIdNextFrame);SetText(window,kIdTimelineFrame,L"8");Click(window,kIdSeekFrame);
            Check(seeks==0,"invalid clock navigation does not use the selected key or cached frame as authorization");}
        SetText(window,kIdValueEdit,L"9");Click(window,kIdApplyKey);
        Check(host.applyCalls==1&&FindKey(host.data,0,10)->value==9,
            "an explicit edit of the selected key remains available when the unrelated clock is invalid");
        ce::CloseOwned(nullptr);
    }
    for(int failure=0;failure!=2;++failure){
        std::printf("transport query cross-source MMD failure=%d\n",failure);std::fflush(stdout);
        auto first=TransportHost(),second=TransportHost();second.data[0].id="second";
        std::string error;const std::array<double,4> a{{20,30,90,110}},b{{40,10,100,80}};
        Check(ce::SetChannelControls(first.data[0],1,a,error)&&ce::SetChannelControls(second.data[0],1,b,error),
            "two source channels provide distinct valid native MMD controls");
        TransportFailureState state;state.failure=failure;auto session=MakeSession(first);
        session.sources.push_back(second.Make("second",L"Second"));session.currentFrame=TransportFailureProvider<false>(state);
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"cross-source MMD authorization fixture opens");
        SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);double controls[4]{};
        Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls))&&
            std::equal(a.begin(),a.end(),controls),"source A establishes an editable follow-playhead interval");
        state.armed=true;HWND combo=GetDlgItem(window,kIdSourceCombo);SendMessageW(combo,CB_SETCURSEL,1,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdSourceCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
        Check(second.readCalls>0&&!SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls))&&
            !IsWindowEnabled(GetDlgItem(window,kIdMmdPanel))&&!IsWindowEnabled(GetDlgItem(window,1096)),
            "failed frame query after a source switch disables the old source's MMD coordinates and interval");
        for(int index=0;index!=4;++index)SetText(window,1096+index,L"70");
        SendMessageW(GetDlgItem(window,1098),WM_KEYDOWN,VK_RETURN,0);
        HWND panel=GetDlgItem(window,kIdMmdPanel);
        SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
        SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(30,30));SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(30,30));
        Check(first.applyCalls==0&&second.applyCalls==0&&LocalWriteHistory(window)==std::array<std::uint64_t,2>{},
            "disabled stale MMD fields and handle messages cannot author the newly selected source");
        state.armed=false;TransportRefreshSource(window,"second");
        Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls))&&
            std::equal(b.begin(),b.end(),controls),"a recovered frame query exposes only source B's own native controls");
        ClickKey(window,1);
        SetComparisonLinkFields(window);TriggerComparisonLink(window,false);
        Check(first.applyCalls==0&&second.applyCalls==1,"recovered source B controls remain editable without touching source A");
        ce::CloseOwned(nullptr);
    }
    Note("invalid clocks preserve selected-key editing and failed source changes cannot reuse old MMD authorization");
}

void TransportPaintStateAndDenseTicks(){
    auto host=TransportHost();TransportFailureState state;auto session=MakeSession(host);
    session.currentFrame=TransportFailureProvider<false>(state);
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(IsWindow(window),"transport GDI fixture opens");
    HWND canvas=GetDlgItem(window,kIdCanvas);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,2048,2048);ReleaseDC(nullptr,screen);
    Check(dc&&bitmap,"transport paint owns a real memory DC and bitmap");
    HGDIOBJ oldBitmap=SelectObject(dc,bitmap);HFONT font=CreateFontW(11,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Arial");
    HGDIOBJ oldFont=SelectObject(dc,font);HRGN clip=CreateRectRgn(7,9,213,175);SelectClipRgn(dc,clip);
    HRGN actual=CreateRectRgn(0,0,0,0);const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    Check(font&&clip&&actual&&before>0,"transport GDI resource accounting and caller font/regions are available");
    for(int failure=0;failure!=3;++failure){state.failure=failure;state.armed=failure!=2;
        for(int pass=0;pass!=8;++pass){SendMessageW(canvas,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
            Check(GetCurrentObject(dc,OBJ_FONT)==font&&GetCurrentObject(dc,OBJ_BITMAP)==bitmap&&
                GetClipRgn(dc,actual)==1&&EqualRgn(clip,actual),
                "normal and failed transport paint restores the caller's selected font, bitmap and clipping region");
            InvalidateRect(canvas,nullptr,FALSE);SendMessageW(canvas,WM_PAINT,0,0);
            Check(!GetUpdateRect(canvas,nullptr,FALSE),"transport query failure still closes the real BeginPaint/EndPaint update region");}}
    // Buffered surfaces are lazily allocated by the first real WM_PAINT; compare only a warmed stable interval.
    const DWORD warmed=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int pass=0;pass!=16;++pass){InvalidateRect(canvas,nullptr,FALSE);SendMessageW(canvas,WM_PAINT,0,0);}
    Check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==warmed&&warmed>=before,
        "repeated guarded paints do not retain additional GDI objects after the buffered surface warms");
    bool closeOnQuery=false;auto closing=MakeSession(host);
    closing.currentFrame=[&]{if(closeOnQuery){closeOnQuery=false;ce::CloseOwned(nullptr);}return 5.0;};
    Check(ce::Open(nullptr,std::move(closing),false)==window,"paint retirement reuses the real editor window");
    closeOnQuery=true;SendMessageW(canvas,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
    Check(!closeOnQuery&&!IsWindow(window)&&GetCurrentObject(dc,OBJ_FONT)==font&&GetCurrentObject(dc,OBJ_BITMAP)==bitmap&&
        GetClipRgn(dc,actual)==1&&EqualRgn(clip,actual),
        "a transport query that destroys its editor never borrows or alters the caller's drawing resources");
    SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(actual);DeleteObject(clip);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);
    ce::CloseOwned(nullptr);
    std::array<std::size_t,2> bytes{},largest{};
    for(int dense=0;dense!=2;++dense){auto ticks=TransportHost();ticks.data[0].track.keys.clear();const int count=dense?100000:2;
        ticks.data[0].track.keys.reserve(count);for(int index=0;index<count;++index)ticks.data[0].track.keys.push_back(MakeKey(index,double(index%3)));
        int frames=0,playing=0,seeks=0,toggles=0;auto tickSession=MakeSession(ticks);
        tickSession.currentFrame=[&]{++frames;return 0.0;};tickSession.isPlaying=[&]{++playing;return false;};
        tickSession.seek=[&](int){++seeks;};tickSession.setPlaying=[&](bool){++toggles;};
        window=static_cast<HWND>(ce::Open(nullptr,std::move(tickSession),false));Check(IsWindow(window),"dense transport timer fixture opens");
        const int hiddenFrames=frames,hiddenPlaying=playing;SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
        Check(frames==hiddenFrames&&playing==hiddenPlaying,"hidden transport timers perform zero frame and playing queries");
        SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
        long long prior[3]{},after[3]{};SendMessageW(window,kHookPlayheadState,0,reinterpret_cast<LPARAM>(prior));
        const int oldFrames=frames,oldPlaying=playing,oldReads=ticks.readCalls;const auto history=LocalWriteHistory(window);
        g_curveAllocationBytes=g_curveLargestAllocation=0;g_curveMeasureAllocations=true;
        for(int tick=0;tick!=8;++tick)SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
        g_curveMeasureAllocations=false;bytes[dense]=g_curveAllocationBytes;largest[dense]=g_curveLargestAllocation;
        SendMessageW(window,kHookPlayheadState,0,reinterpret_cast<LPARAM>(after));
        Check(frames==oldFrames+8&&playing==oldPlaying+8&&after[1]==prior[1]+8&&after[2]==prior[2]&&
            seeks==0&&toggles==0&&ticks.applyCalls==0&&ticks.readCalls==oldReads&&LocalWriteHistory(window)==history&&KeyCount(window)==count,
            "stable visible ticks query readonly transport once without scene reads, seeks, history or duplicate repaint");
        std::printf("DENSE_TRANSPORT keys=%d ticks=8 bytes=%zu largest=%zu\n",count,bytes[dense],largest[dense]);ce::CloseOwned(nullptr);
    }
    Check(bytes[1]<=bytes[0]+4096&&largest[1]<100000*sizeof(int),"transport query protection adds no dense-key-sized allocation per unchanged tick");
    Note("transport drawing preserves GDI state and stable ticks avoid key-count-dependent allocation");
}

struct NativeConstructionProbe {
    inline static NativeConstructionProbe* active=nullptr;
    NativeConstructionProbe* previous=active;
    HHOOK before=nullptr,after=nullptr,cbt=nullptr;
    HWND window=nullptr;
    bool pumping=false,injected=false;
    unsigned entries=0,returns=0;
    int vetoControl=-1;unsigned vetoes=0;
    std::function<void(HWND)> onReturn,onChildCreated;
    static bool EditorWindow(HWND window){wchar_t name[96]{};
        return GetClassNameW(window,name,96)&&wcscmp(name,L"MikuMikuStudioCurveEditorWindow")==0;}
    static LRESULT CALLBACK Before(int code,WPARAM w,LPARAM l){
        if(code>=0&&active){const auto& message=*reinterpret_cast<const CWPSTRUCT*>(l);
            if(message.message==WM_CREATE&&EditorWindow(message.hwnd)&&!active->pumping&&!active->injected){
                active->window=message.hwnd;++active->entries;
                Check(SendMessageW(message.hwnd,kHookNativeConstruction,0,0)==0,
                    "native WM_CREATE entry has not returned or completed the control tree");
            }}
        return CallNextHookEx(nullptr,code,w,l);
    }
    static LRESULT CALLBACK After(int code,WPARAM w,LPARAM l){
        if(code>=0&&active){const auto& message=*reinterpret_cast<const CWPRETSTRUCT*>(l);
            if(message.message==WM_CREATE&&GetParent(message.hwnd)==active->window&&active->onChildCreated)
                active->onChildCreated(message.hwnd);
            if(message.message==WM_CREATE&&message.hwnd==active->window&&!active->pumping&&!active->injected){
                ++active->returns;active->injected=true;active->pumping=true;
                if(active->vetoControl>=0)Check(message.lResult==-1&&SendMessageW(message.hwnd,kHookNativeConstruction,0,0)==0,
                    "a required native child creation failure aborts WM_CREATE without completing provider setup");
                else Check(SendMessageW(message.hwnd,kHookNativeConstruction,0,0)==2,
                    "WM_CREATE return hook still runs inside CreateWindowEx even with every control present");
                if(active->onReturn)active->onReturn(message.hwnd);
                active->pumping=false;
            }}
        return CallNextHookEx(nullptr,code,w,l);
    }
    static LRESULT CALLBACK Cbt(int code,WPARAM w,LPARAM l){
        if(code==HCBT_CREATEWND&&active&&active->vetoControl>=0&&!active->vetoes){
            const auto* create=reinterpret_cast<const CBT_CREATEWNDW*>(l);
            if(create->lpcs->hwndParent==active->window&&reinterpret_cast<INT_PTR>(create->lpcs->hMenu)==active->vetoControl){
                ++active->vetoes;return 1;
            }
        }
        return CallNextHookEx(nullptr,code,w,l);
    }
    explicit NativeConstructionProbe(int veto=-1):vetoControl(veto){active=this;
        before=SetWindowsHookExW(WH_CALLWNDPROC,Before,nullptr,GetCurrentThreadId());
        after=SetWindowsHookExW(WH_CALLWNDPROCRET,After,nullptr,GetCurrentThreadId());
        if(veto>=0)cbt=SetWindowsHookExW(WH_CBT,Cbt,nullptr,GetCurrentThreadId());
        Check(before&&after&&(veto<0||cbt),"install thread-local native creation entry and return observers");}
    ~NativeConstructionProbe(){if(cbt)UnhookWindowsHookEx(cbt);UnhookWindowsHookEx(after);UnhookWindowsHookEx(before);active=previous;}
};
void NativeProviderReady(){
    const auto window=OwnedCurveWindow();
    Check(window&&SendMessageW(window,kHookNativeConstruction,0,0)==3,
        "provider copying, execution and final release occur only after native CreateWindowEx returns");
}
void NativeConstructionMessages(){
    auto host=TransportHost(),replacement=TransportHost();host.compareEnabled=true;
    std::array<unsigned,7> calls{};unsigned writes=0,seeks=0,toggles=0;
    const auto total=[&]{unsigned value=0;for(auto count:calls)value+=count;return value;};
    auto session=MakeSession(host);session.sources[0].externalHistory=true;
    session.currentFrame=[&]{NativeProviderReady();++calls[0];return 5.0;};
    session.isPlaying=[&]{NativeProviderReady();++calls[1];return false;};
    session.seek=[&](int){++seeks;};session.setPlaying=[&](bool){++toggles;};
    session.sources[0].localizedLabel=[&](unsigned){NativeProviderReady();++calls[2];return std::wstring(L"Created source");};
    session.sources[0].read=[&](auto& out,auto&){NativeProviderReady();++calls[3];++host.readCalls;out=host.data;return true;};
    session.history.query=[&](ce::HistoryState& out,std::wstring&){NativeProviderReady();++calls[4];out={3,false,false};return true;};
    session.history.traverse=[&](bool,std::wstring&){++writes;return false;};
    session.sources[0].readKeySelection=[&](const std::string&){NativeProviderReady();++calls[5];return std::vector<int>{10};};
    session.sources[0].writeKeySelection=[&](const std::string&,const std::vector<int>&){++writes;};
    session.sources[0].readComparisonMode=[&]{NativeProviderReady();++calls[6];return ce::kCompareSameGroup;};
    NativeConstructionProbe probe;
    probe.onReturn=[&](HWND window){
        Check(total()==0&&host.readCalls==0&&host.applyCalls==0,"native construction executes zero provider callbacks");
        const auto token=ce::CaptureWindowSession(window);auto held=token.lock();
        Check(held&&ce::IsWindowSessionCurrent(window,token),"the native construction fixture owns a registered source");
        for(HWND target:{window,GetDlgItem(window,kIdCanvas),GetDlgItem(window,kIdMmdPanel)}){
            Check(IsWindow(target),"return-hook construction probes real complete native controls");
            SendMessageW(target,WM_SIZE,0,MAKELPARAM(320,240));
            InvalidateRect(target,nullptr,FALSE);SendMessageW(target,WM_PAINT,0,0);
            Check(!GetUpdateRect(target,nullptr,FALSE),"constructing paint validates its real update region without a provider");
            RenderInk(target);
            SendMessageW(target,WM_KEYDOWN,VK_DELETE,0);
            SendMessageW(target,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(3,3));
            SendMessageW(target,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(5,5));
            SendMessageW(target,WM_LBUTTONUP,0,MAKELPARAM(5,5));
            SendMessageW(target,WM_KILLFOCUS,0,0);SendMessageW(target,WM_CAPTURECHANGED,0,0);
        }
        double controls[4]{};Check(!SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(controls)),
            "constructing callback-capable geometry hooks do not compute MMD controls");
        SendMessageW(window,kHookPointForFrame,10,0);double sample[3]{5,0,0};
        SendMessageW(window,WM_APP+396,0,reinterpret_cast<LPARAM>(sample));
        SendMessageW(window,WM_SHOWWINDOW,TRUE,0);SendMessageW(window,WM_ACTIVATE,WA_ACTIVE,0);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);SendMessageW(window,WM_TIMER,2,0);
        SetText(window,kIdTimelineFrame,L"8");Click(window,kIdSeekFrame);Click(window,kIdPlay);Click(window,kIdDeleteKey);
        for(int id:std::array<int,5>{kIdTimelineFrame,kIdFrameEdit,1096,kIdChannelList,1111}){
            HWND child=GetDlgItem(window,id);Check(IsWindow(child),"construction input targets an existing native child");
            SendMessageW(child,WM_KEYDOWN,VK_RETURN,0);SendMessageW(child,WM_KEYDOWN,VK_DELETE,0);
            SendMessageW(child,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));SendMessageW(child,WM_LBUTTONUP,0,MAKELPARAM(2,2));
        }
        ce::SetLanguage(nullptr,1);
        Check(ce::Open(nullptr,MakeSession(replacement),false)==nullptr&&ce::IsWindowSessionCurrent(window,token),
            "reentrant Open refuses a partially constructed HWND without replacing its source");
        ce::NotifySourceChanged(nullptr,"source");ce::NotifySourceChanged(nullptr,"source");
        SendMessageW(window,WM_APP+401,0,0);SendMessageW(window,WM_TIMER,2,0);
        MSG pending{};Check(!PeekMessageW(&pending,window,WM_APP+401,WM_APP+401,PM_NOREMOVE),
            "constructing source notifications stay pending without a posted refresh loop");
        Check(total()==0&&!writes&&!seeks&&!toggles&&host.readCalls==0&&host.applyCalls==0&&
            replacement.readCalls==0&&ce::IsWindowSessionCurrent(window,token),
            "construction paint, commands, transport, public language, hooks and child input never invoke providers");
        Check(SendMessageW(window,kHookNativeConstruction,0,0)==2,"pumping native messages cannot mark CreateWindowEx as returned");
    };
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    Check(IsWindow(window)&&probe.entries==1&&probe.returns==1,"native creation completes once before provider initialization");
    for(auto count:calls)Check(count>0,"frame, playing, label, source, history, selection and comparison providers initialize after native return");
    Check(GetText(window,kIdSourceLabel)==L"源"&&!writes&&!seeks&&!toggles&&host.applyCalls==0,
        "construction-time requests cannot change the source language or author data");
    Check(GetText(window,kIdBakeStepEdit)==L"1"&&KeyCount(window)==3,
        "post-native initialization preserves canonical data and the original baking defaults");
    const int reads=host.readCalls;MSG message{};unsigned wakes=0;
    while(PeekMessageW(&message,window,WM_APP+401,WM_APP+401,PM_REMOVE)){++wakes;DispatchMessageW(&message);}
    Check(wakes==1&&host.readCalls>reads,"deferred construction notifications merge into one real refresh after initialization");
    SendMessageW(window,WM_TIMER,2,0);Check(!writes&&!seeks&&!toggles,"an idle source timer cannot turn a deferred refresh into authoring");
    ce::CloseOwned(nullptr);Check(!IsWindow(window),"post-native initialized controls retain normal destruction cleanup");
    Note("native return hooks prove construction is provider-free, paint is validated and deferred refresh wakes once");
}
struct NativeConstructionEditRecorder {
    inline static constexpr wchar_t property[]=L"MMS.Tests.Curve.NativeConstruction.EditRecorder";
    HWND window=nullptr;WNDPROC previous=nullptr;
    unsigned focus=0,blur=0,capture=0,cancel=0,timer=0,show=0,dialogCode=0,keys=0,chars=0,destroyed=0;
    NativeConstructionEditRecorder()=default;
    NativeConstructionEditRecorder(const NativeConstructionEditRecorder&)=delete;
    NativeConstructionEditRecorder& operator=(const NativeConstructionEditRecorder&)=delete;
    ~NativeConstructionEditRecorder(){
        // Keep the recorder alive through a fixture's early cleanup too. The
        // product must first unwind its saved previous-procedure chain.
        if(window&&GetPropW(window,property)==this)DestroyWindow(window);
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM w,LPARAM l){
        auto* self=static_cast<NativeConstructionEditRecorder*>(GetPropW(window,property));
        if(!self)return DefWindowProcW(window,message,w,l);
        const WNDPROC native=self->previous;
        switch(message){
        case WM_SETFOCUS:++self->focus;break;
        case WM_KILLFOCUS:++self->blur;break;
        case WM_CAPTURECHANGED:++self->capture;break;
        case WM_CANCELMODE:++self->cancel;break;
        case WM_TIMER:++self->timer;break;
        case WM_SHOWWINDOW:++self->show;break;
        case WM_GETDLGCODE:++self->dialogCode;break;
        case WM_KEYDOWN:++self->keys;break;
        case WM_CHAR:++self->chars;break;
        case WM_NCDESTROY:++self->destroyed;RemovePropW(window,property);self->window=nullptr;break;
        }
        return CallWindowProcW(native,window,message,w,l);
    }
    void Install(HWND target){
        Check(!window&&target,"install exactly one native EDIT recorder before the product subclass");
        Check(SetPropW(target,property,this)!=FALSE,"store the exact native EDIT recorder owner");
        previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(target,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Procedure)));
        Check(previous!=nullptr,"capture the original EDIT procedure at its own WM_CREATE return");window=target;
    }
};
void NativeConstructionChildMaintenance(){
    auto host=TransportHost();unsigned callbacks=0,seeks=0,toggles=0;
    auto session=MakeSession(host);
    session.currentFrame=[&]{NativeProviderReady();++callbacks;return 5.0;};
    session.isPlaying=[&]{NativeProviderReady();++callbacks;return false;};
    session.seek=[&](int){++seeks;};session.setPlaying=[&](bool){++toggles;};
    session.sources[0].localizedLabel=[&](unsigned){NativeProviderReady();++callbacks;return std::wstring(L"Native edit");};
    session.sources[0].read=[&](auto& out,auto&){NativeProviderReady();++callbacks;++host.readCalls;out=host.data;return true;};
    NativeConstructionEditRecorder recorder;NativeConstructionProbe probe;HWND edit=nullptr;
    probe.onChildCreated=[&](HWND child){
        if(GetDlgCtrlID(child)==kIdTimelineFrame){
            Check(SendMessageW(GetParent(child),kHookNativeConstruction,0,0)==0,
                "install the recorder while the native EDIT exists but the parent is still constructing");
            recorder.Install(child);
        }
    };
    probe.onReturn=[&](HWND window){
        edit=GetDlgItem(window,kIdTimelineFrame);
        Check(edit==recorder.window&&IsWindow(edit)&&
            reinterpret_cast<WNDPROC>(GetWindowLongPtrW(edit,GWLP_WNDPROC))!=NativeConstructionEditRecorder::Procedure,
            "the completed product subclass retains the preinstalled recorder as its previous procedure");
        const auto noProviders=[&]{Check(callbacks==0&&host.readCalls==0&&host.applyCalls==0&&!seeks&&!toggles&&
            SendMessageW(window,kHookNativeConstruction,0,0)==2,
            "native child maintenance cannot initialize providers before CreateWindowEx returns");};
        noProviders();
        const HWND other=GetDlgItem(window,kIdPlay);SetFocus(other);
        Check(GetFocus()==other,"focus probe starts on a different real native child");
        const auto priorFocus=recorder.focus;SetFocus(edit);
        Check(GetFocus()==edit&&recorder.focus>priorFocus,
            "actual SetFocus reaches the original native EDIT procedure during parent construction");
        const auto priorBlur=recorder.blur;SetFocus(other);
        Check(GetFocus()==other&&recorder.blur>priorBlur,
            "actual focus transfer reaches the previous EDIT kill-focus procedure");
        SetFocus(edit);Check(GetFocus()==edit,"construction restores real EDIT focus before initialization");
        const auto priorCapture=recorder.capture;SetCapture(edit);
        Check(GetCapture()==edit,"construction capture probe owns the real EDIT window");ReleaseCapture();
        Check(GetCapture()!=edit&&recorder.capture>priorCapture,
            "actual capture release reaches the previous EDIT capture-change procedure");
        const auto priorCancel=recorder.cancel;SendMessageW(edit,WM_CANCELMODE,0,0);
        Check(recorder.cancel>priorCancel,"construction cancellation reaches the previous native EDIT procedure");
        const auto priorTimer=recorder.timer;SendMessageW(edit,WM_TIMER,0x4da1,0);
        Check(recorder.timer>priorTimer,"construction maintenance timer reaches the previous native EDIT procedure");
        const auto priorShow=recorder.show;ShowWindow(edit,SW_HIDE);ShowWindow(edit,SW_SHOW);
        Check(recorder.show>=priorShow+2,"real child visibility transitions reach the previous native EDIT procedure");
        MSG tab{};tab.hwnd=edit;tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;
        const auto priorDialog=recorder.dialogCode;SendMessageW(edit,WM_GETDLGCODE,VK_TAB,reinterpret_cast<LPARAM>(&tab));
        Check(recorder.dialogCode>priorDialog,"construction dialog-code queries retain the native EDIT response path");
        SetFocus(edit);const auto priorKeys=recorder.keys,priorChars=recorder.chars;
        const auto text=GetText(window,kIdTimelineFrame);
        SendMessageW(edit,WM_KEYDOWN,VK_TAB,0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
        SendMessageW(edit,WM_KEYDOWN,VK_DELETE,0);SendMessageW(edit,WM_CHAR,L'9',0);
        SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));SendMessageW(edit,WM_LBUTTONUP,0,MAKELPARAM(2,2));
        Check(recorder.keys==priorKeys&&recorder.chars==priorChars&&GetText(window,kIdTimelineFrame)==text&&GetFocus()==edit,
            "construction consumes business input without native text changes, navigation or authoring");
        noProviders();
    };
    const HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    Check(IsWindow(window)&&probe.entries==1&&probe.returns==1&&GetDlgItem(window,kIdTimelineFrame)==edit&&GetFocus()==edit,
        "the same constructed EDIT retains native focus after the successful initial Open");
    Check(callbacks>0&&host.readCalls>0&&!seeks&&!toggles&&host.applyCalls==0&&
        SendMessageW(window,kHookNativeConstruction,0,0)==3,
        "provider initialization follows native return without replaying maintenance as authoring");
    ce::CloseOwned(nullptr);Check(!IsWindow(window)&&recorder.window==nullptr&&recorder.destroyed==1,
        "native EDIT destruction traverses the previous procedure once and removes its exact recorder");
    Note("construction preserves native EDIT focus, capture, cancellation, timers, visibility and cleanup chains");
}
struct NativeInitRetirementState {
    int phase=0,response=0,calls=0,retires=0,destroyedDuringCopy=0;
    bool armed=false,copying=false;const void* original=nullptr;
    std::function<void()> retire;
    void Retire(){NativeProviderReady();armed=false;++retires;retire();}
};
template<int Kind>struct NativeInitRetirementProvider {
    NativeInitRetirementState* state;HostSource* host;int marker=73;mutable bool invoked=false;
    NativeInitRetirementProvider(NativeInitRetirementState& s,HostSource& h):state(&s),host(&h){}
    NativeInitRetirementProvider(const NativeInitRetirementProvider& other):state(other.state),host(other.host),marker(other.marker){
        if(!state->armed||state->phase!=0)return;
        NativeProviderReady();state->copying=true;state->original=&other;state->Retire();
        Check(state->destroyedDuringCopy==0&&other.marker==73,"initial provider storage survives a replacing copy constructor");
        state->copying=false;if(state->response)throw std::bad_alloc();
    }
    ~NativeInitRetirementProvider(){
        if(state->copying&&state->original==this)++state->destroyedDuringCopy;
        if(invoked&&state->armed&&state->phase==2)state->Retire();
    }
    void Body()const{
        NativeProviderReady();++state->calls;invoked=state->armed;
        const bool fail=state->armed&&state->response;
        if(state->armed&&state->phase==1)state->Retire();
        if(fail)throw std::runtime_error("initial provider failed after native construction");
    }
    auto operator()()const{Body();if constexpr(Kind==0)return 5.0;else if constexpr(Kind==1)return false;else return ce::kCompareSameGroup;}
    std::wstring operator()(unsigned)const{Body();return L"Initial source";}
    bool operator()(std::vector<ce::Channel>& out,std::wstring&)const{++host->readCalls;Body();out=host->data;return true;}
    bool operator()(ce::HistoryState& out,std::wstring&)const{Body();out={1,false,false};return true;}
    std::vector<int> operator()(const std::string&)const{Body();return {10};}
};
template<int Kind>void NativeInitialProviderRetirement(){
    for(int phase=0;phase!=3;++phase)for(int action=0;action!=3;++action)for(int response=0;response!=2;++response){
        std::printf("native initialization kind=%d phase=%d action=%d response=%d\n",Kind,phase,action,response);std::fflush(stdout);
        auto host=TransportHost(),replacement=TransportHost();host.compareEnabled=true;
        NativeInitRetirementState state;state.phase=phase;state.response=response;
        HWND retired=nullptr,next=nullptr;std::wstring status;int readsAfter=-1;
        std::weak_ptr<void> token;std::shared_ptr<void> held;
        state.retire=[&]{retired=OwnedCurveWindow();token=ce::CaptureWindowSession(retired);held=token.lock();
            if(action!=1)ce::CloseOwned(nullptr);
            if(action!=0){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));
                Check(IsWindow(next),"initial provider retirement installs a live replacement");
                if(action==1)Check(next==retired,"post-native initial Open replacement reuses the fully built HWND");
                status=GetText(next,kIdStatus);}
            readsAfter=host.readCalls;
        };
        auto session=MakeSession(host);
        if constexpr(Kind==0)session.currentFrame=NativeInitRetirementProvider<Kind>(state,host);
        if constexpr(Kind==1)session.isPlaying=NativeInitRetirementProvider<Kind>(state,host);
        if constexpr(Kind==2)session.sources[0].localizedLabel=NativeInitRetirementProvider<Kind>(state,host);
        if constexpr(Kind==3)session.sources[0].read=NativeInitRetirementProvider<Kind>(state,host);
        if constexpr(Kind==4){session.sources[0].externalHistory=true;
            session.history.query=NativeInitRetirementProvider<Kind>(state,host);session.history.traverse=[](bool,std::wstring&){return false;};}
        if constexpr(Kind==5){session.sources[0].externalHistory=true;
            session.history.query=[](ce::HistoryState& out,std::wstring&){out={1,false,false};return true;};
            session.history.traverse=[](bool,std::wstring&){return false;};
            session.sources[0].readKeySelection=NativeInitRetirementProvider<Kind>(state,host);}
        if constexpr(Kind==6)session.sources[0].readComparisonMode=NativeInitRetirementProvider<Kind>(state,host);
        state.armed=true;NativeConstructionProbe probe;
        const auto window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        Check(window==nullptr&&probe.returns==1&&state.retires==1&&state.destroyedDuringCopy==0&&state.calls==int(phase!=0),
            "initial copy, body and final release retire exactly once after the native creation call returns");
        Check(held&&!ce::IsWindowSessionCurrent(retired,token)&&host.readCalls==readsAfter&&host.applyCalls==0,
            "retired initialization never continues reading or writing the old source, even with a retained token");
        if(next){Check(IsWindow(next)&&GetText(next,kIdStatus)==status&&KeyCount(next)==3&&replacement.applyCalls==0,
                "outer initialization cleanup never changes or destroys a replacement Session");
            // Keep the original input sequence free of diagnostic heap
            // allocations and extra editor messages. Only fixed-capacity scalar
            // observations run between the original DOWN/UP/Delete actions;
            // richer evidence is collected only after the unchanged check has
            // already failed.
            const HWND recoveryCanvas=GetDlgItem(next,kIdCanvas);
            Check(recoveryCanvas!=nullptr,"the canvas exists");
            const POINT recoveryPoint=AskPoint(next,kHookPointForKey,1);
            Check(recoveryPoint.x>0&&recoveryPoint.y>0,"the queried key lies inside the canvas");
            const auto recoveryToken=ce::CaptureWindowSession(next);
            struct RecoveryObservation {
                HWND focus=nullptr,capture=nullptr;
                std::array<SHORT,6> keys{};
                bool current=false;
                bool windowEnabled=false,canvasEnabled=false,deleteEnabled=false,applyEnabled=false;
                int applies=0,reads=0;
            };
            const auto observe=[&]{
                RecoveryObservation observation;
                observation.focus=GetFocus();observation.capture=GetCapture();
                observation.keys={GetKeyState(VK_SPACE),GetKeyState(VK_MENU),GetKeyState(VK_CONTROL),GetKeyState(VK_SHIFT),
                    GetKeyState(VK_LBUTTON),GetKeyState(VK_RBUTTON)};
                observation.current=ce::IsWindowSessionCurrent(next,recoveryToken);
                observation.windowEnabled=IsWindowEnabled(next)!=FALSE;
                observation.canvasEnabled=IsWindowEnabled(recoveryCanvas)!=FALSE;
                observation.deleteEnabled=IsWindowEnabled(GetDlgItem(next,kIdDeleteKey))!=FALSE;
                observation.applyEnabled=IsWindowEnabled(GetDlgItem(next,kIdApplyKey))!=FALSE;
                observation.applies=replacement.applyCalls;observation.reads=replacement.readCalls;return observation;
            };
            const auto beforeClick=observe();
            const LPARAM recoveryPosition=MAKELPARAM(recoveryPoint.x,recoveryPoint.y);
            SendMessageW(recoveryCanvas,WM_LBUTTONDOWN,MK_LBUTTON,recoveryPosition);
            const auto afterDown=observe();
            SendMessageW(recoveryCanvas,WM_LBUTTONUP,0,recoveryPosition);
            const auto afterUp=observe();
            Click(next,kIdDeleteKey);
            const auto afterDelete=observe();
            const int recoveryKeys=KeyCount(next);
            const bool recoveryAccepted=replacement.applyCalls==1&&recoveryKeys==2;
            if(!recoveryAccepted){
                const auto printText=[](const char* label,const std::wstring& text){
                    std::fprintf(stderr," %s=\"",label);
                    for(wchar_t unit:text){
                        if(unit>=32&&unit<127&&unit!=L'\\'&&unit!=L'\"')std::fputc(int(unit),stderr);
                        else std::fprintf(stderr,"\\u%04x",unsigned(static_cast<unsigned short>(unit)));
                    }
                    std::fputc('"',stderr);
                };
                const auto print=[&](const char* stage,const RecoveryObservation& observation){
                    std::fprintf(stderr,"NATIVE_INIT_RECOVERY kind=%d phase=%d action=%d response=%d stage=%s hwnd=%p canvas=%p point=%ld,%ld focus=%p capture=%p keys_space_alt_ctrl_shift_left_right=%04x,%04x,%04x,%04x,%04x,%04x current=%d windowEnabled=%d canvasEnabled=%d deleteEnabled=%d applyEnabled=%d applies=%d reads=%d\n",
                        Kind,phase,action,response,stage,next,recoveryCanvas,recoveryPoint.x,recoveryPoint.y,
                        observation.focus,observation.capture,
                        unsigned(static_cast<unsigned short>(observation.keys[0])),unsigned(static_cast<unsigned short>(observation.keys[1])),
                        unsigned(static_cast<unsigned short>(observation.keys[2])),unsigned(static_cast<unsigned short>(observation.keys[3])),
                        unsigned(static_cast<unsigned short>(observation.keys[4])),unsigned(static_cast<unsigned short>(observation.keys[5])),
                        observation.current,observation.windowEnabled,observation.canvasEnabled,
                        observation.deleteEnabled,observation.applyEnabled,observation.applies,observation.reads);
                };
                print("before",beforeClick);print("down",afterDown);print("up",afterUp);print("delete",afterDelete);
                // These messages may measure/sample or allocate. They are
                // deliberately absent from every successful input sequence.
                CurveEditorAxisState geometry{};
                std::array<std::uint64_t,5> history{};std::array<int,12> comparison{};
                const bool geometryReady=SendMessageW(next,WM_APP+394,0,reinterpret_cast<LPARAM>(&geometry))!=0;
                const bool historyReady=SendMessageW(next,WM_APP+397,0,reinterpret_cast<LPARAM>(history.data()))!=0;
                ReadCompare(next,comparison.data());
                std::fprintf(stderr,"NATIVE_INIT_RECOVERY_FAILURE_ONLY kind=%d phase=%d action=%d response=%d keyCount=%d geometryReady=%d plot=%ld,%ld,%ld,%ld time=%.17g,%.17g value=%.17g,%.17g viewMode=%d historyReady=%d history=%llu,%llu,%llu,%llu,%llu comparison=",
                    Kind,phase,action,response,recoveryKeys,geometryReady,geometry.plot.left,geometry.plot.top,geometry.plot.right,geometry.plot.bottom,
                    geometry.timeStart,geometry.timeEnd,geometry.viewLow,geometry.viewHigh,geometry.viewMode,historyReady,
                    static_cast<unsigned long long>(history[0]),static_cast<unsigned long long>(history[1]),
                    static_cast<unsigned long long>(history[2]),static_cast<unsigned long long>(history[3]),
                    static_cast<unsigned long long>(history[4]));
                for(std::size_t index=0;index<comparison.size();++index)std::fprintf(stderr,index?",%d":"%d",comparison[index]);
                printText("frame",GetText(next,kIdFrameEdit));printText("value",GetText(next,kIdValueEdit));
                printText("status",GetText(next,kIdStatus));std::fputc('\n',stderr);std::fflush(stderr);
            }
            Check(recoveryAccepted,"replacement remains editable after initial callback retirement unwinds");}
        else Check(OwnedCurveWindow()==nullptr,"closed initialization leaves no registered ghost editor");
        ce::CloseOwned(nullptr);
    }
}
void NativeConstructionAborted(){
    for(int id:{kIdFrameEdit,kIdCanvas,kIdMmdPanel}){
        auto host=TransportHost();unsigned callbacks=0;
        {NativeConstructionProbe probe(id);auto session=MakeSession(host);
            session.currentFrame=[&]{++callbacks;return 5.0;};session.isPlaying=[&]{++callbacks;return false;};
            session.sources[0].read=[&](auto& out,auto&){++callbacks;out=host.data;return true;};
            Check(ce::Open(nullptr,std::move(session),false)==nullptr&&probe.vetoes==1&&probe.returns==1&&!callbacks&&
                host.applyCalls==0&&OwnedCurveWindow()==nullptr,
                "actual native EDIT, canvas or MMD creation refusal leaves no provider call or partial registered window");
        }
        const auto next=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
        Check(IsWindow(next)&&KeyCount(next)==3,"native child failure cleanup permits a complete subsequent Open");ce::CloseOwned(nullptr);
    }
    for(bool replace:{false,true}){
        auto host=TransportHost(),replacement=TransportHost();NativeConstructionProbe probe;HWND next=nullptr;
        unsigned callbacks=0;auto session=MakeSession(host);
        session.currentFrame=[&]{++callbacks;return 5.0;};session.isPlaying=[&]{++callbacks;return false;};
        session.sources[0].read=[&](auto& out,auto&){++callbacks;out=host.data;return true;};
        probe.onReturn=[&](HWND window){
            Check(SendMessageW(window,kHookNativeConstruction,0,0)==2&&!callbacks,"native close occurs before any provider is allowed");
            SendMessageW(window,WM_CLOSE,0,0);Check(!IsWindow(window),"construction-time WM_CLOSE retains real destruction processing");
            if(replace){next=static_cast<HWND>(ce::Open(nullptr,MakeSession(replacement),false));Check(IsWindow(next),"native close can create a separate healthy editor before outer creation returns");}
        };
        Check(ce::Open(nullptr,std::move(session),false)==nullptr&&!callbacks&&host.applyCalls==0,
            "destroying the native window aborts its exact initial Open without invoking providers");
        if(next){Check(IsWindow(next)&&KeyCount(next)==3,"failed outer native creation cannot delete a nested replacement");
            ClickKey(next,1);Click(next,kIdDeleteKey);Check(replacement.applyCalls==1,"nested replacement survives native-create abort cleanup");}
        else Check(OwnedCurveWindow()==nullptr,"aborted native creation leaves no registry owner");
        ce::CloseOwned(nullptr);
    }
    Note("native creation abort preserves destruction chains and exact replacement ownership");
}
void NativeConstructionLifetime(){
    NativeConstructionMessages();
    NativeConstructionChildMaintenance();
    NativeInitialProviderRetirement<0>();NativeInitialProviderRetirement<1>();NativeInitialProviderRetirement<2>();
    NativeInitialProviderRetirement<3>();NativeInitialProviderRetirement<4>();NativeInitialProviderRetirement<5>();NativeInitialProviderRetirement<6>();
    NativeConstructionAborted();
}

// ---------------------------------------------------------------------------
// 26. Rendering after close/reopen and the damage a real handle drag creates.
//
// The modeless editor is closed and reopened from the same canonical data three
// times.  The fixture is a value curve whose native MMD controls are genuinely
// non-linear and whose interval end key is selected, which is the state a real
// handle edit leaves behind.  The reported failure keeps the keys and handles
// but straightens the Bezier into its chord, so pixel presence alone is not
// enough: the painted row is measured at interior frames and compared with the
// canonical cubic evaluation, and must differ from the straight key-to-key
// chord.  The first render after every Open runs before any curve data is
// written, and both the memory print and the buffered window paint must agree
// across cycles.
//
// Scope note: this is a generic value-curve rendering regression.  The native
// controls below are canonical non-linear control geometry, not an emulation of
// the morph/expression host channel: morph persistence and identity live in the
// host adapter and are deliberately not exercised or judged here.
//
// The damage observer clears every update region first, then sends real mouse
// messages.  A drag may damage only the surface that owns the dragged handle;
// the editor shell and the unrelated children must stay clean.  The test never
// invalidates the editor shell itself, so a clean observation cannot be
// produced by test-side damage.
// The regression windows are really shown on a private, inactive Win32
// desktop.  That makes InvalidateRect produce genuine update regions and
// UpdateWindow deliver a real WM_PAINT with a non-empty dirty rectangle,
// without showing anything on the user's desktop or changing activation.
//
// Both fixtures run in a child process started on that desktop.  A worker
// thread cannot guarantee that helper windows are gone before the desktop is
// closed: Windows refuses SetThreadDesktop once the thread owns a window and
// the reverse switch can fail because of residual IME or helper windows.  The
// parent keeps its own borrowed desktop handle, creates the private desktop
// with default (non-inheritable) security, and starts this same executable
// with STARTUPINFO.lpDesktop set to the private name.  Only duplicated
// standard handles are inheritable and PROC_THREAD_ATTRIBUTE_HANDLE_LIST
// restricts inheritance to them.  The child runs the two original fixtures in
// order on its startup desktop and exits when they finish, so every helper
// thread it created is gone before the parent closes the desktop.  Neither
// process switches desktop and nothing uses SwitchDesktop, SendInput,
// SetForegroundWindow or a window-station change, so the user's foreground
// desktop is untouched.
void ReopenPaintRegression();
void HandleDragInvalidation();

[[noreturn]] void FailIsolatedPaint(const char *operation, DWORD error) {
    std::fprintf(stderr, "FAIL: %s failed with GetLastError=%lu in the isolated paint fixtures\n",
                 operation, static_cast<unsigned long>(error));
    std::fflush(stderr);
    std::exit(1);
}

// The evidence stream is narrow (std::printf), so convert each UTF-16 desktop name
// into a bounded stack UTF-8 buffer.  A failed name query is reported explicitly
// and is never mislabelled as a CreateDesktopW or SetThreadDesktop failure.
bool TryDesktopName(HDESK desktop, const char *operation, char (&out)[512]) {
    wchar_t wide[128] = {};
    if (GetUserObjectInformationW(desktop, UOI_NAME, wide, static_cast<DWORD>(sizeof(wide)), nullptr) == FALSE) {
        const DWORD error = GetLastError();
        std::snprintf(out, sizeof(out), "<name-query-failed>");
        std::fprintf(stderr, "FAIL: GetUserObjectInformationW(UOI_NAME) for %s failed with GetLastError=%lu\n",
                     operation, static_cast<unsigned long>(error));
        std::fflush(stderr);
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(sizeof(out)), nullptr, nullptr) == 0) {
        const DWORD error = GetLastError();
        std::snprintf(out, sizeof(out), "<name-query-failed>");
        std::fprintf(stderr, "FAIL: WideCharToMultiByte(UTF-8) for %s failed with GetLastError=%lu\n",
                     operation, static_cast<unsigned long>(error));
        std::fflush(stderr);
        return false;
    }
    return true;
}

// Fails the setup phase before any child exists.  Every handle this function was
// given is owned by it, so each handle is closed and every close failure is
// reported in addition to the original failure.
[[noreturn]] void AbortIsolatedPaintSetup(HDESK desktop, HANDLE (&inherited)[3], const char *failure,
                                          DWORD error, bool haveError) {
    if (haveError)
        std::fprintf(stderr, "FAIL: %s failed with GetLastError=%lu before the isolated paint child started\n",
                     failure, static_cast<unsigned long>(error));
    else
        std::fprintf(stderr, "FAIL: %s before the isolated paint child started\n", failure);
    for (HANDLE &handle : inherited) {
        if (handle == nullptr) continue;
        if (CloseHandle(handle) == FALSE) {
            const DWORD closeError = GetLastError();
            std::fprintf(stderr,
                         "FAIL: CloseHandle(inherited standard handle) failed with GetLastError=%lu after the isolated paint child setup failure\n",
                         static_cast<unsigned long>(closeError));
        }
        handle = nullptr;
    }
    if (CloseDesktop(desktop) == FALSE) {
        const DWORD closeError = GetLastError();
        std::fprintf(stderr,
                     "FAIL: CloseDesktop(private desktop) failed with GetLastError=%lu after the isolated paint child setup failure\n",
                     static_cast<unsigned long>(closeError));
    }
    std::fflush(stderr);
    std::exit(1);
}

void RunIsolatedPaintFixtures() {
    // Only the parent runs these checks; the two paint fixtures themselves run
    // in the child process and report their own actual check count there.  The
    // allocator fault and measurement flags are thread-local, so a fresh child
    // would start clean and hide any flag the parent left armed; assert the
    // parent state instead of resetting it.  A parent-owned editor would likewise
    // be a leak the full suite left behind: the child has a separate address
    // space, so this preflight detects the parent's own stale registry entry
    // rather than sharing it with the child.
    Check(!g_curveFailNextAllocation, "no curve allocation fault is armed before the isolated paint child");
    Check(!g_curveMeasureAllocations, "no curve allocation measurement is active before the isolated paint child");
    Check(OwnedCurveWindow() == nullptr, "no curve editor is registered before the isolated paint child starts");
    // Retain the borrowed original parent desktop handle and never close it.
    const HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (originalDesktop == nullptr) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "FAIL: GetThreadDesktop(original parent desktop) failed with GetLastError=%lu\n",
                     static_cast<unsigned long>(error));
        std::fflush(stderr);
        std::exit(1);
    }
    char originalName[512] = {};
    if (!TryDesktopName(originalDesktop, "original parent desktop", originalName)) std::exit(1);
    std::printf("DESKTOP_PARENT handle=%p name=%s\n", reinterpret_cast<void *>(originalDesktop), originalName);
    std::fflush(stdout);
    wchar_t name[96] = {};
    swprintf_s(name, L"MMS-curve-paint-isolated-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    // Private, inactive desktop.  It is created with default security, so the
    // handle itself is not inheritable and the child receives only the desktop
    // name through STARTUPINFO.lpDesktop.
    const HDESK desktop = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (desktop == nullptr) FailIsolatedPaint("CreateDesktopW", GetLastError());
    HANDLE inherited[3] = {nullptr, nullptr, nullptr};
    char createdName[512] = {};
    if (!TryDesktopName(desktop, "created private desktop", createdName))
        AbortIsolatedPaintSetup(desktop, inherited, "created private desktop name verification", 0, false);
    std::printf("DESKTOP_CREATED handle=%p name=%s\n", reinterpret_cast<void *>(desktop), createdName);
    std::fflush(stdout);
    // The parent thread must stay on its original desktop; a parent thread on
    // the private desktop would keep it in use and make the final close fail.
    HDESK parentDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (parentDesktop == nullptr)
        AbortIsolatedPaintSetup(desktop, inherited, "GetThreadDesktop(parent desktop after CreateDesktopW)",
                                GetLastError(), true);
    if (parentDesktop != originalDesktop) {
        if (SetThreadDesktop(originalDesktop) == FALSE)
            AbortIsolatedPaintSetup(desktop, inherited, "SetThreadDesktop(original parent desktop)", GetLastError(),
                                    true);
        parentDesktop = GetThreadDesktop(GetCurrentThreadId());
        if (parentDesktop == nullptr)
            AbortIsolatedPaintSetup(desktop, inherited, "GetThreadDesktop(parent desktop after restore)",
                                    GetLastError(), true);
        if (parentDesktop != originalDesktop)
            AbortIsolatedPaintSetup(desktop, inherited, "parent desktop restore verification", 0, false);
    }
    // Launch this same executable with an explicit application name and a
    // writable, quoted command line.  A truncated module path or a malformed
    // command line would start the wrong program, so both are hard failures.
    wchar_t executable[32768] = {};
    const DWORD executableCapacity = static_cast<DWORD>(sizeof(executable) / sizeof(executable[0]));
    const DWORD executableLength = GetModuleFileNameW(nullptr, executable, executableCapacity);
    if (executableLength == 0)
        AbortIsolatedPaintSetup(desktop, inherited, "GetModuleFileNameW", GetLastError(), true);
    if (executableLength >= executableCapacity)
        AbortIsolatedPaintSetup(desktop, inherited, "GetModuleFileNameW truncated the executable path", 0, false);
    std::wstring commandLine = L"\"" + std::wstring(executable) + L"\" --paint-isolated-child " + name;
    if (g_validateHeap) commandLine += L" --paint-isolated-heap-validate";
    // CreateProcessW rejects a command line longer than 32767 characters,
    // including the terminator; the module path check above covers only the
    // path, so the complete appended line is bounded here.
    if (commandLine.size() + 1 > 32767U)
        AbortIsolatedPaintSetup(desktop, inherited, "CreateProcessW command line exceeds 32767 characters", 0, false);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    // Owned inheritable duplicates of the three standard handles.  The parent's
    // originals are never closed or made inheritable, and the child's stdout and
    // stderr must reach this process's log.
    const DWORD standardHandles[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int index = 0; index < 3; ++index) {
        const HANDLE source = GetStdHandle(standardHandles[index]);
        if (source != nullptr && source != INVALID_HANDLE_VALUE) {
            if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &inherited[index], 0, TRUE,
                                DUPLICATE_SAME_ACCESS) == FALSE)
                AbortIsolatedPaintSetup(desktop, inherited, "DuplicateHandle(inherited standard handle)",
                                        GetLastError(), true);
            continue;
        }
        if (index != 0)
            AbortIsolatedPaintSetup(desktop, inherited,
                                    index == 1 ? "GetStdHandle(STD_OUTPUT_HANDLE) is unavailable"
                                               : "GetStdHandle(STD_ERROR_HANDLE) is unavailable",
                                    0, false);
        // Only an unavailable stdin may fall back to the NUL device; a missing
        // stdout or stderr is an explicit failure, never a silent discard.
        SECURITY_ATTRIBUTES inheritable = {};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;
        inherited[index] = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                       OPEN_EXISTING, 0, nullptr);
        if (inherited[index] == INVALID_HANDLE_VALUE) {
            inherited[index] = nullptr;
            AbortIsolatedPaintSetup(desktop, inherited, "CreateFileW(NUL input)", GetLastError(), true);
        }
    }
    // Explicit inheritance whitelist: bInheritHandles is TRUE only together with
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so no other parent handle leaks into the
    // child.
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0)
        AbortIsolatedPaintSetup(desktop, inherited, "InitializeProcThreadAttributeList(size query)",
                                GetLastError(), true);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) == FALSE)
        AbortIsolatedPaintSetup(desktop, inherited, "InitializeProcThreadAttributeList", GetLastError(), true);
    if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                  nullptr, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        AbortIsolatedPaintSetup(desktop, inherited, "UpdateProcThreadAttribute(handle list)", error, true);
    }
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited[0];
    startup.StartupInfo.hStdOutput = inherited[1];
    startup.StartupInfo.hStdError = inherited[2];
    startup.StartupInfo.lpDesktop = name;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child = {};
    std::fflush(stdout);
    std::fflush(stderr);
    if (CreateProcessW(executable, mutableCommandLine.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                       nullptr, nullptr, &startup.StartupInfo, &child) == FALSE) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        AbortIsolatedPaintSetup(desktop, inherited, "CreateProcessW", error, true);
    }
    DeleteProcThreadAttributeList(attributes);
    std::printf("CHILD_STARTED pid=%lu desktop=%s\n", static_cast<unsigned long>(child.dwProcessId), createdName);
    std::fflush(stdout);
    // Nothing in the child needs these duplicates once it has started.
    bool failed = false;
    for (HANDLE &handle : inherited) {
        if (handle == nullptr) continue;
        if (CloseHandle(handle) == FALSE) {
            const DWORD error = GetLastError();
            std::fprintf(stderr,
                         "FAIL: CloseHandle(inherited standard handle) failed with GetLastError=%lu after the isolated paint child started\n",
                         static_cast<unsigned long>(error));
            failed = true;
        }
        handle = nullptr;
    }
    const DWORD wait = WaitForSingleObject(child.hProcess, INFINITE);
    if (wait != WAIT_OBJECT_0) {
        // The child may still be running on the desktop, so neither the child
        // handles nor the desktop may be closed here.  Only WAIT_FAILED has a
        // meaningful last-error; any other status is its own diagnosis.
        if (wait == WAIT_FAILED) {
            const DWORD error = GetLastError();
            std::fprintf(stderr,
                         "FAIL: WaitForSingleObject(isolated paint child) failed with GetLastError=%lu; the child may still be running, so its handles and the private desktop are left open\n",
                         static_cast<unsigned long>(error));
        } else {
            std::fprintf(stderr,
                         "FAIL: WaitForSingleObject(isolated paint child) returned unexpected status %lu; the child may still be running, so its handles and the private desktop are left open\n",
                         static_cast<unsigned long>(wait));
        }
        std::fflush(stderr);
        std::exit(1);
    }
    DWORD childExit = 0;
    if (GetExitCodeProcess(child.hProcess, &childExit) == FALSE) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "FAIL: GetExitCodeProcess(isolated paint child) failed with GetLastError=%lu\n",
                     static_cast<unsigned long>(error));
        failed = true;
    }
    if (CloseHandle(child.hThread) == FALSE) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "FAIL: CloseHandle(isolated paint child thread) failed with GetLastError=%lu\n",
                     static_cast<unsigned long>(error));
        failed = true;
    }
    if (CloseHandle(child.hProcess) == FALSE) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "FAIL: CloseHandle(isolated paint child process) failed with GetLastError=%lu\n",
                     static_cast<unsigned long>(error));
        failed = true;
    }
    // The child has terminated, so the parent thread must still be on the
    // original desktop before the private desktop is closed exactly once.
    const HDESK afterChild = GetThreadDesktop(GetCurrentThreadId());
    if (afterChild == nullptr) {
        const DWORD error = GetLastError();
        std::fprintf(stderr,
                     "FAIL: GetThreadDesktop(parent desktop after the isolated paint child) failed with GetLastError=%lu\n",
                     static_cast<unsigned long>(error));
        failed = true;
    } else if (afterChild != originalDesktop) {
        std::fprintf(stderr,
                     "FAIL: parent desktop after the isolated paint child differs from the original desktop (expected %p, observed %p)\n",
                     reinterpret_cast<void *>(originalDesktop), reinterpret_cast<void *>(afterChild));
        failed = true;
    }
    if (CloseDesktop(desktop) == FALSE) {
        const DWORD error = GetLastError();
        std::fprintf(stderr,
                     "FAIL: CloseDesktop(private desktop) failed with GetLastError=%lu after the isolated paint child\n",
                     static_cast<unsigned long>(error));
        failed = true;
    }
    if (childExit != 0) {
        std::fprintf(stderr, "FAIL: the isolated paint child exited with code %lu\n",
                     static_cast<unsigned long>(childExit));
        failed = true;
    }
    if (failed) {
        std::fflush(stderr);
        std::exit(1);
    }
}

int RunIsolatedPaintChild(const char *expectedDesktopName, bool validateHeap) {
    g_validateHeap = validateHeap;
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    if (desktop == nullptr) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "FAIL: GetThreadDesktop in the isolated paint child failed with GetLastError=%lu\n",
                     static_cast<unsigned long>(error));
        std::fflush(stderr);
        return 1;
    }
    Check(desktop != nullptr, "the curve paint regression owns a private Win32 desktop");
    char actualName[512] = {};
    if (!TryDesktopName(desktop, "isolated paint child startup desktop", actualName)) return 1;
    // The parent names the desktop with an ASCII string, so the narrow argument
    // and the UOI_NAME UTF-8 conversion compare byte for byte.
    if (expectedDesktopName == nullptr || std::strcmp(actualName, expectedDesktopName) != 0) {
        std::fprintf(stderr,
                     "FAIL: the isolated paint child runs on desktop '%s' but the parent created '%s'\n",
                     actualName, expectedDesktopName == nullptr ? "<missing>" : expectedDesktopName);
        std::fflush(stderr);
        return 1;
    }
    std::printf("CHILD_DESKTOP handle=%p name=%s\n", reinterpret_cast<void *>(desktop), actualName);
    std::fflush(stdout);
    bool failure = false;
    try {
        ReopenPaintRegression();
        HandleDragInvalidation();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: the isolated paint fixtures threw: %s\n", error.what());
        failure = true;
    } catch (...) {
        std::fprintf(stderr, "FAIL: the isolated paint fixtures threw a non-standard exception\n");
        failure = true;
    }
    // A Check failure inside the fixtures exits immediately, exactly as before.
    // On the exception path the fixtures' stack-local hosts are already gone, so
    // this only releases the window and session and cannot re-enter callbacks.
    bool editorLeft = false;
    try {
        if (OwnedCurveWindow() != nullptr) ce::CloseOwned(nullptr);
        editorLeft = OwnedCurveWindow() != nullptr;
    } catch (const std::exception &error) {
        editorLeft = true;
        if (!failure) {
            std::fprintf(stderr, "FAIL: the isolated paint child editor cleanup threw: %s\n", error.what());
            failure = true;
        }
    } catch (...) {
        editorLeft = true;
        if (!failure) {
            std::fprintf(stderr, "FAIL: the isolated paint child editor cleanup threw a non-standard exception\n");
            failure = true;
        }
    }
    if (failure) {
        if (editorLeft)
            std::fprintf(stderr, "FAIL: the isolated paint child also left an editor registered to its thread\n");
        std::fflush(stderr);
        return 1;
    }
    Check(!editorLeft, "the isolated paint child leaves no editor registered to its thread");
    std::printf("curve isolated paint child passed: %d checks\n", g_checks);
    std::fflush(stdout);
    return 0;
}

void ShowOwnedEditor(HWND window) {
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Check(IsWindowVisible(window) != FALSE, "the regression editor is really shown on the private desktop");
}

// ---------------------------------------------------------------------------
struct CurveEditorSurfaces {
    HWND window = nullptr;
    HWND canvas = nullptr;
    HWND mmd = nullptr;
    HWND tree = nullptr;
    HWND view = nullptr;
    HWND status = nullptr;
    HWND add = nullptr;
    HWND frame = nullptr;
};

CurveEditorSurfaces CurveEditorSurfacesOf(HWND window) {
    CurveEditorSurfaces surfaces;
    surfaces.window = window;
    surfaces.canvas = GetDlgItem(window, kIdCanvas);
    surfaces.mmd = GetDlgItem(window, kIdMmdPanel);
    surfaces.tree = GetDlgItem(window, kIdChannelList);
    surfaces.view = GetDlgItem(window, kIdViewCombo);
    surfaces.status = GetDlgItem(window, kIdStatus);
    surfaces.add = GetDlgItem(window, kIdAddKey);
    surfaces.frame = GetDlgItem(window, kIdFrameEdit);
    return surfaces;
}

bool AllCurveEditorSurfacesLive(const CurveEditorSurfaces &surfaces) {
    return IsWindow(surfaces.window) && IsWindow(surfaces.canvas) && IsWindow(surfaces.mmd) &&
           IsWindow(surfaces.tree) && IsWindow(surfaces.view) && IsWindow(surfaces.status) &&
           IsWindow(surfaces.add) && IsWindow(surfaces.frame);
}

struct PaintMessageObserver {
    struct Entry {
        HWND window = nullptr;
        unsigned paints = 0;
    };
    std::array<Entry, 8> entries{};
    std::size_t count = 0;
    static LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                    DWORD_PTR data) {
        if (message == WM_PAINT) ++reinterpret_cast<Entry *>(data)->paints;
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
    void Add(HWND window) {
        Check(window != nullptr && IsWindow(window), "the paint observer binds a live editor surface");
        Check(count < entries.size(), "the paint observer covers the editor's complete surface set");
        entries[count].window = window;
        Check(SetWindowSubclass(window, &PaintMessageObserver::Observe, 0x73d1,
                                 reinterpret_cast<DWORD_PTR>(&entries[count])) != FALSE,
              "observe only the editor's own hidden windows");
        ++count;
    }
    explicit PaintMessageObserver(const CurveEditorSurfaces &surfaces) {
        Add(surfaces.window);
        Add(surfaces.canvas);
        Add(surfaces.mmd);
        Add(surfaces.tree);
        Add(surfaces.view);
        Add(surfaces.status);
        Add(surfaces.add);
        Add(surfaces.frame);
    }
    ~PaintMessageObserver() {
        for (std::size_t index = 0; index < count; ++index)
            if (IsWindow(entries[index].window))
                RemoveWindowSubclass(entries[index].window, &PaintMessageObserver::Observe, 0x73d1);
    }
    unsigned Paints(HWND window) const {
        for (std::size_t index = 0; index < count; ++index)
            if (entries[index].window == window) return entries[index].paints;
        return 0;
    }
    void ValidateAll() const {
        for (std::size_t index = 0; index < count; ++index)
            if (IsWindow(entries[index].window)) ValidateRect(entries[index].window, nullptr);
    }
    bool AnyPending() const {
        for (std::size_t index = 0; index < count; ++index)
            if (IsWindow(entries[index].window) &&
                GetUpdateRect(entries[index].window, nullptr, FALSE) != FALSE) return true;
        return false;
    }
    // Paints exactly the windows a gesture damaged through the real
    // BeginPaint/EndPaint path, and returns how many of them painted.
    unsigned PaintPending() const {
        unsigned painted = 0;
        for (std::size_t index = 0; index < count; ++index) {
            HWND window = entries[index].window;
            if (!IsWindow(window) || GetUpdateRect(window, nullptr, FALSE) == FALSE) continue;
            // The window is shown on the private desktop, so this delivers a
            // real WM_PAINT with the actual dirty rectangle.
            UpdateWindow(window);
            ++painted;
        }
        return painted;
    }
};

// The "x" channel curve is RGB(222, 76, 104).  The interior of its stroke stays red
// dominant; the neutral grid and paper, the gold selected key, the grey handles
// and the blue playhead stay outside this band, so a hit inside the clean strip
// can only be the curve itself.
bool IsCurveColourPixel(unsigned pixel) {
    const int red = static_cast<int>((pixel >> 16) & 255);
    const int green = static_cast<int>((pixel >> 8) & 255);
    const int blue = static_cast<int>(pixel & 255);
    return red >= 150 && red - green >= 60 && red - blue >= 50;
}

int CurveColourPixelsInRect(const std::vector<unsigned> &image, int width, int height, const RECT &area) {
    if (width <= 0 || height <= 0) return 0;
    const int left = std::max(0, std::min(width, static_cast<int>(area.left)));
    const int right = std::max(0, std::min(width, static_cast<int>(area.right)));
    const int top = std::max(0, std::min(height, static_cast<int>(area.top)));
    const int bottom = std::max(0, std::min(height, static_cast<int>(area.bottom)));
    int count = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const unsigned pixel = image[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                        static_cast<std::size_t>(x)];
            if (IsCurveColourPixel(pixel)) ++count;
        }
    }
    return count;
}

// The canvas transforms read back through the axis hook, so the printed curve
// can be compared with the canonical track instead of trusting red pixels.
CurveEditorAxisState ReadEditorAxis(HWND window) {
    CurveEditorAxisState axis = {};
    Check(SendMessageW(window, WM_APP + 394, 0, reinterpret_cast<LPARAM>(&axis)) != 0,
          "the editor publishes its canvas geometry");
    return axis;
}

double AxisValueToY(const CurveEditorAxisState &axis, double value) {
    const double span = axis.viewHigh - axis.viewLow;
    if (!(span > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(axis.plot.bottom) -
           (value - axis.viewLow) / span * static_cast<double>(axis.plot.bottom - axis.plot.top);
}

double AxisFrameToX(const CurveEditorAxisState &axis, double frame) {
    const double span = axis.timeEnd - axis.timeStart;
    const double width = static_cast<double>(axis.plot.right - axis.plot.left);
    if (!(span > 0.0) || !(width > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(axis.plot.left) + (frame - axis.timeStart) / span * width;
}

double AxisXToFrame(const CurveEditorAxisState &axis, double x) {
    const double span = axis.timeEnd - axis.timeStart;
    const double width = static_cast<double>(axis.plot.right - axis.plot.left);
    if (!(span > 0.0) || !(width > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return axis.timeStart + (x - static_cast<double>(axis.plot.left)) / width * span;
}

// Mean painted row of the single red stroke in the column nearest the
// requested frame. The drawn polyline joins integer columns, so the column
// midpoint frame is the sample the stroke mean row corresponds to.
bool MeasurePaintedCurveY(const std::vector<unsigned> &image, int width, int height,
                          const CurveEditorAxisState &axis, double frame, double &measuredY,
                          double &sampleFrame) {
    const double x = AxisFrameToX(axis, frame);
    if (!std::isfinite(x) || width <= 0 || height <= 0) return false;
    const int column = static_cast<int>(std::lround(x));
    if (column < axis.plot.left || column >= axis.plot.right || column < 0 || column >= width) return false;
    const int top = std::max<int>(axis.plot.top, 0);
    const int bottom = std::min<int>(axis.plot.bottom, height);
    int found = 0;
    double sum = 0;
    for (int row = top; row < bottom; ++row) {
        const unsigned pixel = image[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(column)];
        if (!IsCurveColourPixel(pixel)) continue;
        sum += static_cast<double>(row);
        ++found;
    }
    if (found < 1) return false;
    measuredY = sum / static_cast<double>(found);
    sampleFrame = AxisXToFrame(axis, static_cast<double>(column) + 0.5);
    return std::isfinite(sampleFrame);
}

// A straightened Bezier still paints a red line, so pixel presence alone is
// not evidence. The painted height must match the canonical cubic at interior
// frames and must differ from the straight key-to-key chord.
void CheckReopenedCurveGeometry(const std::vector<unsigned> &image, int width, int height,
                                const CurveEditorAxisState &axis, const ac::Track &canonical,
                                int cycle, const char *state) {
    Check(canonical.keys.size() >= 2, "the canonical reopen fixture holds two keys");
    const ac::Key &first = canonical.keys.front();
    const ac::Key &last = canonical.keys.back();
    const double frames[3] = {5.0, 10.0, 15.0};
    bool offChordAtMid = false;
    for (int index = 0; index < 3; ++index) {
        const double frame = frames[index];
        double measuredY = 0, sampleFrame = 0;
        const bool found = MeasurePaintedCurveY(image, width, height, axis, frame, measuredY, sampleFrame);
        const double expectedValue = found ? ac::Evaluate(canonical, sampleFrame).value : 0.0;
        const double expectedY = AxisValueToY(axis, expectedValue);
        const double chordValue = first.value + (sampleFrame - first.frame) /
            static_cast<double>(last.frame - first.frame) * (last.value - first.value);
        const double chordY = AxisValueToY(axis, chordValue);
        std::printf("REOPEN_GEOMETRY cycle=%d state=%s frame=%.0f sample_frame=%.3f measured=%.2f "
                    "expected=%.2f chord=%.2f expected_off_chord=%.2f found=%d\n",
                    cycle, state, frame, sampleFrame, measuredY, expectedY, chordY,
                    std::fabs(expectedY - chordY), found ? 1 : 0);
        std::fflush(stdout);
        Check(found, "the reopened print contains the coloured curve at an interior sample");
        Check(std::fabs(measuredY - expectedY) <= 3.0,
              "the reopened curve follows the canonical Bezier height at an interior sample");
        Check(std::fabs(measuredY - chordY) > 4.0,
              "the reopened curve is not the straight chord between its keys");
        if (frame == 10.0 && std::fabs(expectedY - chordY) >= 8.0) offChordAtMid = true;
    }
    Check(offChordAtMid, "the canonical fixture is genuinely curved at the mid interior sample");
}

// Generic value-curve fixture for the reopen paint oracle.  The non-linear
// native controls only supply canonical curved geometry; this is NOT an actual
// morph/expression channel and says nothing about morph host persistence.
HostSource ReopenPaintHost() {
    HostSource host;
    // ChannelColour matches the last slash-separated axis, so a bare "x" id is
    // the documented red axis colour the pixel matcher expects.  A dotted id
    // such as "pos.x" hashes to an unrelated hue.
    auto channel = MakeChannel("x", L"Position X",
        {MakeKey(0, 0.0, ac::Interpolation::Bezier, ac::Tangent::Free),
         MakeKey(20, 5.0, ac::Interpolation::Bezier, ac::Tangent::Free)});
    channel.nativeControls.resize(2);
    channel.controlGeometry.resize(2);
    std::string error;
    Check(ce::SetChannelControls(channel, 1, {20, 80, 107, 20}, error),
          "the reopen fixture keeps its canonical non-linear native controls");
    host.data.push_back(std::move(channel));
    return host;
}

void ReopenPaintRegression() {
    std::vector<unsigned> baselineOpened, baseline;
    int baselineOpenedCurve = -1, baselineCurvePixels = -1;
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::printf("REOPEN_PAINT cycle=%d\n", cycle);
        std::fflush(stdout);
        HostSource host = ReopenPaintHost();
        ce::Session session = MakeSession(host);
        HWND window = static_cast<HWND>(ce::Open(nullptr, std::move(session), false));
        Check(IsWindow(window), "a closed editor reopens from the canonical non-linear native controls");
        const CurveEditorSurfaces surfaces = CurveEditorSurfacesOf(window);
        Check(AllCurveEditorSurfacesLive(surfaces), "the reopened editor owns every native surface");
        ShowOwnedEditor(window);
        Check(host.readCalls == 1 && host.applyCalls == 0, "the fresh reopen reads once and writes nothing");
        Check(KeyCount(window) == 2, "the reopen adopts the canonical two-key track");
        RECT client = {};
        GetClientRect(surfaces.canvas, &client);
        const int width = static_cast<int>(client.right - client.left);
        const int height = static_cast<int>(client.bottom - client.top);
        const CurveEditorAxisState axis = ReadEditorAxis(window);
        const POINT middle = AskPoint(window, kHookPointForFrame, 10);
        Check(middle.x > 0 && middle.y > 0 && PtInRect(&axis.plot, middle),
              "the canonical curve crosses the clean middle strip");
        // Between the keys and away from every handle and key marker: the only
        // coloured pixels in this rectangle belong to the curve stroke.
        const RECT strip = {middle.x - 6, axis.plot.top, middle.x + 7, axis.plot.bottom};
        // The very first print after Open, before any interaction at all, must
        // already show the curved geometry; a straightened chord is still red,
        // so the oracle compares painted height with the canonical Bezier.
        const auto opened = RenderPixels(surfaces.canvas);
        Check(width > 0 && height > 0 &&
                  opened.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
              "the reopened canvas prints a complete client-sized image");
        const int openedCurve = CurveColourPixelsInRect(opened, width, height, strip);
        Check(openedCurve >= 5, "the first print after Open paints the curve before any interaction");
        CheckReopenedCurveGeometry(opened, width, height, axis, host.data[0].track, cycle, "opened");
        // Selecting the canonical interval end key is a UI action, not a curve
        // edit; it reveals the handles the report mentions without writing data.
        ClickKey(window, 1);
        Check(host.applyCalls == 0, "selecting the canonical end key is not a curve edit");
        Check(GetText(window, kIdFrameEdit) == L"20", "the selected key is the interval end key");
        double controls[4] = {};
        Check(SendMessageW(window, kHookMmdControls, 0, reinterpret_cast<LPARAM>(controls)) != FALSE &&
                  controls[0] == 20.0 && controls[1] == 80.0 && controls[2] == 107.0 && controls[3] == 20.0,
              "the reopened editor still exposes the canonical non-linear controls");
        const CurveEditorAxisState selectedAxis = ReadEditorAxis(window);
        const POINT selectedMiddle = AskPoint(window, kHookPointForFrame, 10);
        Check(selectedMiddle.x > 0 && PtInRect(&selectedAxis.plot, selectedMiddle),
              "the selected-key print still crosses the clean middle strip");
        const RECT selectedStrip = {selectedMiddle.x - 6, selectedAxis.plot.top, selectedMiddle.x + 7,
                                    selectedAxis.plot.bottom};
        const auto pixels = RenderPixels(surfaces.canvas);
        Check(pixels.size() == opened.size(), "the selected end key does not change the canvas size");
        const int curvePixels = CurveColourPixelsInRect(pixels, width, height, selectedStrip);
        Check(curvePixels >= 5, "the reopened editor paints its coloured curve with the end key selected");
        CheckReopenedCurveGeometry(pixels, width, height, selectedAxis, host.data[0].track, cycle, "selected");
        PaintMessageObserver paints(surfaces);
        paints.ValidateAll();
        Check(!paints.AnyPending(), "the shown reopen window starts from clean native update regions");
        InvalidateRect(surfaces.canvas, nullptr, FALSE);
        RECT damage = {};
        Check(GetUpdateRect(surfaces.canvas, &damage, FALSE) != FALSE &&
                  damage.right > damage.left && damage.bottom > damage.top,
              "a real visible-window repaint request creates non-empty canvas damage");
        const unsigned canvasPaints = paints.Paints(surfaces.canvas);
        UpdateWindow(surfaces.canvas);
        Check(paints.Paints(surfaces.canvas) > canvasPaints && !paints.AnyPending(),
              "the real WM_PAINT runs for the visible window and validates its dirty region");
        Check(paints.Paints(surfaces.window) == 0 && paints.Paints(surfaces.mmd) == 0 &&
                  paints.Paints(surfaces.tree) == 0 && paints.Paints(surfaces.view) == 0 &&
                  paints.Paints(surfaces.status) == 0 && paints.Paints(surfaces.add) == 0 &&
                  paints.Paints(surfaces.frame) == 0,
              "the real canvas repaint paints only the canvas");
        Check(RenderPixels(surfaces.canvas) == pixels,
              "the real window paint leaves the printed curve image unchanged");
        std::printf("REOPEN_PAINT_RESULT cycle=%d opened_pixels=%d selected_pixels=%d client=%dx%d\n", cycle,
                    openedCurve, curvePixels, width, height);
        std::fflush(stdout);
        if (cycle == 0) {
            baselineOpened = opened;
            baselineOpenedCurve = openedCurve;
            baseline = pixels;
            baselineCurvePixels = curvePixels;
        } else {
            Check(opened == baselineOpened, "every close/reopen cycle prints the same first-paint image");
            Check(openedCurve == baselineOpenedCurve,
                  "every close/reopen cycle paints the same number of first-paint curve pixels");
            Check(pixels == baseline, "every close/reopen cycle prints the same selected-key curve image");
            Check(curvePixels == baselineCurvePixels,
                  "every close/reopen cycle paints the same number of selected-key curve pixels");
        }
        Check(host.applyCalls == 0, "the reopen paint path never edits curve data");
        ce::CloseOwned(nullptr);
    }
    Note("close/reopen paints the generic canonical non-linear value curve before any edit, deterministically");
}

// Shared oracle for both real handle drags: exactly one surface may be
// damaged, the shell and every unrelated child stay clean, and only that
// surface receives the real BeginPaint/EndPaint message.
void CheckHandleDamage(const PaintMessageObserver &paints, const CurveEditorSurfaces &surfaces, bool valueHandle) {
    HWND relevant = valueHandle ? surfaces.canvas : surfaces.mmd;
    HWND other = valueHandle ? surfaces.mmd : surfaces.canvas;
    RECT damage = {};
    const BOOL damaged = GetUpdateRect(relevant, &damage, FALSE);
    RECT client = {};
    GetClientRect(relevant, &client);
    const BOOL shellDamage = GetUpdateRect(surfaces.window, nullptr, FALSE);
    const BOOL otherDamage = GetUpdateRect(other, nullptr, FALSE);
    const BOOL unrelatedDamage =
        GetUpdateRect(surfaces.tree, nullptr, FALSE) || GetUpdateRect(surfaces.view, nullptr, FALSE) ||
        GetUpdateRect(surfaces.status, nullptr, FALSE) || GetUpdateRect(surfaces.add, nullptr, FALSE) ||
        GetUpdateRect(surfaces.frame, nullptr, FALSE);
    const unsigned before = paints.Paints(relevant);
    const unsigned painted = paints.PaintPending();
    const unsigned after = paints.Paints(relevant);
    std::printf("HANDLE_DAMAGE surface=%s damaged=%ld,%ld,%ld,%ld client=%ld,%ld shell=%u other=%u "
                "unrelated=%u before=%u painted=%u after=%u\n",
                valueHandle ? "canvas" : "mmd", damage.left, damage.top, damage.right, damage.bottom,
                client.right, client.bottom, unsigned(shellDamage != FALSE), unsigned(otherDamage != FALSE),
                unsigned(unrelatedDamage != FALSE), before, painted, after);
    std::fflush(stdout);
    Check(damaged != FALSE && damage.right > damage.left && damage.bottom > damage.top &&
              damage.left >= client.left && damage.top >= client.top && damage.right <= client.right &&
              damage.bottom <= client.bottom,
          valueHandle ? "the dragged value handle damages only its own canvas client"
                      : "the dragged miniature handle damages only its own panel client");
    Check(shellDamage == FALSE, "a handle drag never damages the editor shell");
    Check(otherDamage == FALSE, valueHandle ? "a value handle drag never damages the MMD panel"
                                            : "a miniature handle drag never damages the curve canvas");
    Check(unrelatedDamage == FALSE, "a handle drag leaves every unrelated child control clean");
    Check(before == 0 && painted == 1 && after == 1 && !paints.AnyPending() &&
              paints.Paints(surfaces.window) == 0 && paints.Paints(other) == 0 &&
              paints.Paints(surfaces.tree) == 0 && paints.Paints(surfaces.view) == 0 &&
              paints.Paints(surfaces.status) == 0 && paints.Paints(surfaces.add) == 0 &&
              paints.Paints(surfaces.frame) == 0,
          "only the surface that owns the dragged handle receives the real WM_PAINT");
}

// Drags one real canvas value handle (each curved side) with the real mouse
// messages the control receives.  CanvasMouseMove is the code under measure:
// it invalidates via InvalidateCanvas, so this oracle shows exactly which
// native windows that reaches.
enum class DragInterruption { None, Resize, Cancel };

void InterruptCurveDrag(HWND window, HWND surface, DragInterruption interruption) {
    if (interruption == DragInterruption::Resize) {
        RECT bounds{};
        Check(GetWindowRect(window, &bounds) && SetWindowPos(window, nullptr, 0, 0,
            bounds.right - bounds.left + 123, bounds.bottom - bounds.top + 77,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "resize the real curve editor during a drag");
    } else {
        SendMessageW(surface, WM_CANCELMODE, 0, 0);
    }
    Check(GetCapture() != surface, "interruption releases curve drag capture");
}

void KeyDragInterruption() {
    HostSource host;
    host.data.push_back(MakeChannel("pos.x", L"Position X",
        {MakeKey(0, 0.0), MakeKey(10, 5.0), MakeKey(20, 9.0)}));
    const auto window = static_cast<HWND>(ce::Open(nullptr, MakeSession(host), false));
    Check(IsWindow(window), "open the editor for interrupted key movement");
    ShowOwnedEditor(window);
    const HWND canvas = GetDlgItem(window, kIdCanvas);
    const auto from = AskPoint(window, kHookPointForKey, 1);
    const auto to = AskPoint(window, kHookPointForFrame, 15);
    Check(from.x > 0 && from.y > 0 && to.x > 0, "key movement endpoints are visible");
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    Check(GetCapture() == canvas, "the key movement owns canvas capture");
    SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, from.y));
    InterruptCurveDrag(window, canvas, DragInterruption::Resize);
    SendMessageW(canvas, WM_LBUTTONUP, 0, MAKELPARAM(to.x, from.y));
    Click(window, kIdUndo);
    Check(host.applyCalls == 0 && FindKey(host.data, 0, 10), "interrupted movement leaves the key at frame 10");
    DragKeyToFrame(window, 1, 15);
    Check(host.applyCalls == 1 && FindKey(host.data, 0, 15), "new movement lands at frame 15");
    Click(window, kIdUndo);
    Check(host.applyCalls == 2 && FindKey(host.data, 0, 10), "one undo restores the original frame");
    ce::CloseOwned(nullptr);
}

void CanvasDragButtonOwnership() {
    HostSource host;
    auto channel = MakeChannel("pos.x", L"Position X", {
        MakeKey(0, 0.0, ac::Interpolation::Bezier, ac::Tangent::Free),
        MakeKey(10, 5.0, ac::Interpolation::Bezier, ac::Tangent::Free),
        MakeKey(20, 9.0, ac::Interpolation::Bezier, ac::Tangent::Free)});
    channel.track.keys[1].outWeight = 0.3;
    channel.track.keys[1].outSlope = 0.0;
    host.data.push_back(std::move(channel));
    const auto window = static_cast<HWND>(ce::Open(nullptr, MakeSession(host), false));
    Check(IsWindow(window), "open the editor for overlapping mouse buttons");
    ShowOwnedEditor(window);
    const HWND canvas = GetDlgItem(window, kIdCanvas);
    const auto key = AskPoint(window, kHookPointForKey, 1);
    const auto frame = AskPoint(window, kHookPointForFrame, 15);
    const auto handle = AskPoint(window, kHookPointForFrame, 13);
    Check(key.x > 0 && key.y > 0 && frame.x > 0 && handle.x > 0,
          "the key, handle and drag target are visible");
    const LPARAM keyPoint = MAKELPARAM(key.x, key.y);
    const LPARAM movedKey = MAKELPARAM(frame.x, key.y);
    const LPARAM handlePoint = MAKELPARAM(handle.x, key.y);
    const LPARAM movedHandle = MAKELPARAM(frame.x, key.y - 16);

    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, keyPoint);
    Check(GetCapture() == canvas, "the left button owns the key drag");
    SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, movedKey);
    SendMessageW(canvas, WM_RBUTTONDOWN, MK_LBUTTON | MK_RBUTTON, movedKey);
    SendMessageW(canvas, WM_RBUTTONUP, MK_LBUTTON, movedKey);
    Check(GetCapture() == canvas && host.applyCalls == 0 && FindKey(host.data, 0, 10),
          "releasing the right button cannot commit a left-button key drag");
    SendMessageW(canvas, WM_LBUTTONUP, 0, movedKey);
    Check(GetCapture() != canvas && host.applyCalls == 1 && FindKey(host.data, 0, 15),
          "the left release commits the key drag once");
    Click(window, kIdUndo);
    Check(host.applyCalls == 2 && FindKey(host.data, 0, 10), "undo restores the original key frame");

    ClickKey(window, 1);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, handlePoint);
    SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, movedHandle);
    SendMessageW(canvas, WM_MBUTTONDOWN, MK_LBUTTON | MK_MBUTTON, movedHandle);
    SendMessageW(canvas, WM_MBUTTONUP, MK_LBUTTON, movedHandle);
    Check(GetCapture() == canvas && host.applyCalls == 2,
          "middle-button input does not replace or finish a left-button handle drag");
    SetCapture(window);
    Check(GetCapture() == window, "the canvas loses capture to another window");
    ReleaseCapture();
    SendMessageW(canvas, WM_LBUTTONUP, 0, movedHandle);
    Check(host.applyCalls == 2 && host.data[0].track.keys[1].outSlope == 0.0,
          "capture loss cancels the preview and a stale left release writes nothing");
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, handlePoint);
    SendMessageW(canvas, WM_LBUTTONUP, 0, movedHandle);
    Check(host.applyCalls == 3 && std::abs(host.data[0].track.keys[1].outSlope) > 0.01,
          "a fresh handle drag can commit after cancellation");
    Click(window, kIdUndo);
    Check(host.applyCalls == 4 && host.data[0].track.keys[1].outSlope == 0.0,
          "one undo restores the pre-drag handle");

    SendMessageW(canvas, WM_MBUTTONDOWN, MK_MBUTTON, keyPoint);
    Check(GetCapture() == canvas, "the middle button owns the pan");
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON | MK_MBUTTON, keyPoint);
    SendMessageW(canvas, WM_LBUTTONUP, MK_MBUTTON, movedKey);
    Check(GetCapture() == canvas && host.applyCalls == 4,
          "a left click during middle-button pan does not start editing");
    SendMessageW(canvas, WM_MBUTTONUP, 0, movedKey);
    Check(GetCapture() != canvas && host.applyCalls == 4, "the middle release finishes the pan without editing");
    ce::CloseOwned(nullptr);
}

void ValueHandleDragInvalidation(bool outSide, DragInterruption interruption = DragInterruption::None) {
    HostSource host;
    auto channel = MakeChannel("pos.x", L"Position X",
        {MakeKey(0, 0.0, ac::Interpolation::Bezier, ac::Tangent::Free),
         MakeKey(10, 5.0, ac::Interpolation::Bezier, ac::Tangent::Free),
         MakeKey(20, 9.0, ac::Interpolation::Bezier, ac::Tangent::Free)});
    channel.track.keys[1].inWeight = channel.track.keys[1].outWeight = 0.3;
    channel.track.keys[1].inSlope = channel.track.keys[1].outSlope = 0.0;
    host.data.push_back(std::move(channel));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, std::move(session), false));
    Check(IsWindow(window), "the value handle drag fixture opens for the private desktop");
    const CurveEditorSurfaces surfaces = CurveEditorSurfacesOf(window);
    Check(AllCurveEditorSurfacesLive(surfaces), "the value handle drag fixture owns every native surface");
    ShowOwnedEditor(window);
    ClickKey(window, 1);
    Check(GetText(window, kIdFrameEdit) == L"10", "the value handle fixture selects the middle key");
    const POINT key = AskPoint(window, kHookPointForKey, 1);
    const POINT handle = AskPoint(window, kHookPointForFrame, outSide ? 13 : 7);
    const POINT target1 = AskPoint(window, kHookPointForFrame, outSide ? 15 : 5);
    const POINT target2 = AskPoint(window, kHookPointForFrame, outSide ? 16 : 4);
    Check(key.x > 0 && key.y > 0 && handle.x > 0 && target1.x > 0 && target2.x > 0,
          "the queried value handle and drag targets lie inside the canvas");
    SetFocus(surfaces.canvas);
    PaintMessageObserver paints(surfaces);
    paints.ValidateAll();
    Check(!paints.AnyPending(), "the value handle drag starts from clean native update regions");
    SendMessageW(surfaces.canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(handle.x, key.y));
    Check(GetCapture() == surfaces.canvas, "the real value handle press owns the canvas capture");
    Check(!GetUpdateRect(surfaces.canvas, nullptr, FALSE),
          "pressing a value handle without moving paints nothing");
    SendMessageW(surfaces.canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target1.x, key.y - 12));
    SendMessageW(surfaces.canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target2.x, key.y - 16));
    Check(host.applyCalls == 0, "an in-flight value handle drag has not published curve data");
    if (interruption != DragInterruption::None) {
        InterruptCurveDrag(window, surfaces.canvas, interruption);
        SendMessageW(surfaces.canvas, WM_LBUTTONUP, 0, MAKELPARAM(target2.x, key.y - 16));
        Click(window, kIdUndo);
        Check(host.applyCalls == 0 && host.data.front().track.keys[1].outSlope == 0.0 &&
            host.data.front().track.keys[1].inSlope == 0.0, "stale value-handle release does not write or create history");
        DragKeyToFrame(window, 1, 12);
        Check(host.applyCalls == 1 && FindKey(host.data, 0, 12), "fresh drag uses the current canvas geometry");
        ce::CloseOwned(nullptr);
        return;
    }
    CheckHandleDamage(paints, surfaces, true);
    SendMessageW(surfaces.canvas, WM_LBUTTONUP, 0, MAKELPARAM(target2.x, key.y - 16));
    Check(host.applyCalls == 1, "the value handle drag commits exactly one curve write");
    ce::CloseOwned(nullptr);
    Note(outSide ? "the outgoing value handle damages only its own curve canvas"
                 : "the incoming value handle damages only its own curve canvas");
}

// Drags one real MMD panel control handle.  MmdMouseMove owns the separate
// miniature redraw path; it must damage its own panel only.
void MmdHandleDragInvalidation(int handle, DragInterruption interruption = DragInterruption::None) {
    HostSource host;
    auto channel = MakeChannel("pos.x", L"Position X",
        {MakeKey(0, 0.0, ac::Interpolation::Bezier, ac::Tangent::Free),
         MakeKey(20, 5.0, ac::Interpolation::Bezier, ac::Tangent::Free)});
    channel.nativeControls.resize(2);
    channel.controlGeometry.resize(2);
    std::string error;
    Check(ce::SetChannelControls(channel, 1, {20, 80, 107, 20}, error),
          "the miniature handle fixture keeps canonical non-linear controls");
    host.data.push_back(std::move(channel));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, std::move(session), false));
    Check(IsWindow(window), "the miniature handle drag fixture opens for the private desktop");
    const CurveEditorSurfaces surfaces = CurveEditorSurfacesOf(window);
    Check(AllCurveEditorSurfacesLive(surfaces), "the miniature handle drag fixture owns every native surface");
    ShowOwnedEditor(window);
    ClickKey(window, 1);
    Check(GetText(window, kIdFrameEdit) == L"20", "the miniature handle fixture selects the interval end key");
    double controls[4] = {};
    Check(SendMessageW(window, kHookMmdControls, 0, reinterpret_cast<LPARAM>(controls)) != FALSE &&
              controls[0] == 20.0 && controls[1] == 80.0 && controls[2] == 107.0 && controls[3] == 20.0,
          "the miniature panel exposes the canonical non-linear native controls");
    // The panel maps the 0..127 controls onto its square.  Reproduce the same
    // client point the editor itself would draw for the dragged control.
    const auto getDpi = reinterpret_cast<UINT(WINAPI *)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    UINT dpi = getDpi ? getDpi(surfaces.mmd) : 0;
    if (!dpi) {
        HDC dc = GetDC(surfaces.mmd);
        dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
        if (dc) ReleaseDC(surfaces.mmd, dc);
    }
    RECT panel = {};
    GetClientRect(surfaces.mmd, &panel);
    const int margin = MulDiv(6, dpi ? static_cast<int>(dpi) : 96, 96);
    const int side = std::min(static_cast<int>(panel.right), static_cast<int>(panel.bottom)) - 2 * margin;
    Check(side > 0, "the miniature panel owns a drawable control square");
    const auto panelPoint = [&](double u, double v) {
        return MAKELPARAM(margin + static_cast<int>(std::lround(u * side / 127.0)),
                          margin + side - static_cast<int>(std::lround(v * side / 127.0)));
    };
    const LPARAM pressed = panelPoint(controls[handle * 2], controls[handle * 2 + 1]);
    const LPARAM target1 = handle == 0 ? panelPoint(40.0, 100.0) : panelPoint(100.0, 40.0);
    const LPARAM target2 = handle == 0 ? panelPoint(45.0, 105.0) : panelPoint(95.0, 35.0);
    const LPARAM released = handle == 0 ? panelPoint(50.0, 112.0) : panelPoint(88.0, 29.0);
    SetFocus(surfaces.mmd);
    PaintMessageObserver paints(surfaces);
    paints.ValidateAll();
    Check(!paints.AnyPending(), "the miniature handle drag starts from clean native update regions");
    SendMessageW(surfaces.mmd, WM_LBUTTONDOWN, MK_LBUTTON, pressed);
    SendMessageW(surfaces.mmd, WM_LBUTTONUP, 0, pressed);
    Check(host.applyCalls == 0, "a miniature handle click does not create history");
    SendMessageW(surfaces.mmd, WM_LBUTTONDOWN, MK_LBUTTON, pressed);
    SendMessageW(surfaces.mmd, WM_MOUSEMOVE, MK_LBUTTON, target1);
    SendMessageW(surfaces.mmd, WM_LBUTTONUP, 0, pressed);
    Check(host.applyCalls == 0, "returning the miniature handle to the press point does not publish a change");
    Click(window, kIdUndo);
    Check(host.applyCalls == 0, "returning the miniature handle to the press point leaves no undo action");
    paints.ValidateAll();
    SendMessageW(surfaces.mmd, WM_LBUTTONDOWN, MK_LBUTTON, pressed);
    Check(GetCapture() == surfaces.mmd, "the real miniature handle press owns the panel capture");
    Check(!GetUpdateRect(surfaces.mmd, nullptr, FALSE),
          "pressing a miniature handle without moving paints nothing");
    SendMessageW(surfaces.mmd, WM_MOUSEMOVE, MK_LBUTTON, target1);
    SendMessageW(surfaces.mmd, WM_MOUSEMOVE, MK_LBUTTON, target2);
    Check(host.applyCalls == 0, "an in-flight miniature drag has not published native controls");
    if (interruption != DragInterruption::None) {
        InterruptCurveDrag(window, surfaces.mmd, interruption);
        SendMessageW(surfaces.mmd, WM_LBUTTONUP, 0, released);
        Click(window, kIdUndo);
        double restored[4]{};
        Check(host.applyCalls == 0 &&
            SendMessageW(window, kHookMmdControls, 0, reinterpret_cast<LPARAM>(restored)) != FALSE &&
            std::equal(std::begin(controls), std::end(controls), std::begin(restored)),
            "interrupted miniature drag restores native controls and leaves no undo action");
        ce::CloseOwned(nullptr);
        return;
    }
    CheckHandleDamage(paints, surfaces, false);
    SendMessageW(surfaces.mmd, WM_LBUTTONUP, 0, released);
    Check(host.applyCalls == 1, "the miniature handle drag commits exactly one native control write");
    double after[4]{};
    Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(after)) != FALSE &&
          after[handle*2] == std::lround((double(LOWORD(released))-margin)*127.0/side) &&
          after[handle*2+1] == std::lround((margin+side-double(HIWORD(released)))*127.0/side) &&
          after[(1-handle)*2] == controls[(1-handle)*2] &&
          after[(1-handle)*2+1] == controls[(1-handle)*2+1],
          "the miniature uses the release coordinate and preserves the opposite handle");
    Click(window,kIdUndo);
    double restored[4]{};
    Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(restored)) != FALSE &&
          std::equal(std::begin(restored),std::end(restored),std::begin(controls)),
          "one undo restores the previous miniature control bytes");
    Click(window,kIdRedo);
    Check(SendMessageW(window,kHookMmdControls,0,reinterpret_cast<LPARAM>(restored)) != FALSE &&
          std::equal(std::begin(restored),std::end(restored),std::begin(after)),
          "redo restores the miniature control at the release point");
    ce::CloseOwned(nullptr);
    Note(handle == 0 ? "the first miniature control damages only the MMD panel"
                     : "the second miniature control damages only the MMD panel");
}

void HandleDragInvalidation() {
    ValueHandleDragInvalidation(true);
    ValueHandleDragInvalidation(false);
    MmdHandleDragInvalidation(0);
    MmdHandleDragInvalidation(1);
    Note("value and miniature handle drags damage only the surface that owns the handle");
}

// Selected-key fitting and the morph 0..1 display default.
// The status line is the only exact count of the key selection ("... N"); a
// fit never writes one, so it still describes the selection afterwards.
int SelectionCount(HWND window) {
    const std::wstring text = GetText(window, kIdStatus);
    std::size_t begin = text.size();
    while (begin > 0 && text[begin - 1] >= L'0' && text[begin - 1] <= L'9') --begin;
    if (begin == text.size()) return -1;
    return static_cast<int>(std::wcstol(text.c_str() + begin, nullptr, 10));
}

void SelectionFitRegressions() {
    HostSource host;
    host.compareEnabled = true; host.hasModeCallbacks = true;
    host.storedMode = ce::kCompareFree; host.initialMain = "main";
    // Only frames 20 and 40 get selected; the free slopes overshoot above both
    // endpoint values, so the fitted Y needs the original sampler on the span.
    host.data.push_back(MakeGroupChannel("main", L"Main", 0,
        {MakeKey(0, 0.0), MakeShapeKey(20, 4.0, 0.0, 0.35),
         MakeShapeKey(40, 8.0, -0.35, 0.0), MakeKey(100, 12.0)}));
    host.data.push_back(MakeGroupChannel("compare", L"Compare", 0,
        {MakeKey(0, -500.0), MakeKey(100, 500.0)}, false));
    ce::Session session = MakeSession(host);
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(IsWindow(window) && KeyCount(window) == 4, "the selected-fit fixture opens four keys");
    const auto setSpeedView = [&](bool speed) {
        HWND viewCombo = GetDlgItem(window, kIdViewCombo);
        SendMessageW(viewCombo, CB_SETCURSEL, speed ? 1 : 0, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(kIdViewCombo, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(viewCombo));
    };
    const auto speedBounds = [&](double bounds[2]) {
        setSpeedView(true);
        const CurveEditorAxisState axis = ReadEditorAxis(window);
        bounds[0] = axis.viewLow; bounds[1] = axis.viewHigh;
        setSpeedView(false);
    };
    ClickKey(window, 1);
    Check(GetText(window, kIdFrameEdit) == L"20" && SelectionCount(window) == 1,
          "a real click selects frame 20 as the only key");
    ClickKeyAdditive(window, 2);
    double span[3] = {}, view[4] = {};
    Check(SelectionCount(window) == 2 && ReadCompareSpan(window, span) && span[2] == 1 &&
              span[0] == 20.0 && span[1] == 40.0,
          "the additive click leaves exactly the frames 20 and 40 selected");
    ClickChannelOnce(window, 1);
    const int applies = host.applyCalls; const auto history = LocalWriteHistory(window);
    const std::wstring selectionStatus = GetText(window, kIdStatus);
    Click(window, kIdFitAll); ReadView(window, view);
    Check(view[0] == 20.0 && view[1] == 40.0,
          "Fit all takes the horizontal span from the selected key frames");
    Check(view[2] > 3.0 && view[2] < 4.0 && view[3] > 8.05 && view[3] < 11.0,
          "Fit all keeps the sampler overshoot inside the selection and excludes outside keys and the comparison lane");
    double speedFit[2] = {}; speedBounds(speedFit);
    Check(speedFit[0] < 0.0 && speedFit[1] > 0.0, "the selection fit covers the derivative range too");
    // Move the time view away so Fit time has to restore the selected span.
    ClickBar(window, 1, 0); PressKey(window, VK_LEFT);
    ReadView(window, view);
    Check(view[0] < 20.0, "the time bar moved the view before Fit time");
    double before[4] = {}, fitted[4] = {};
    ReadView(window, before);
    Click(window, kIdFitRange); ReadView(window, fitted);
    Check(fitted[0] == 20.0 && fitted[1] == 40.0, "Fit time restores the selected span");
    Check(fitted[2] == before[2] && fitted[3] == before[3],
          "Fit time leaves both value bounds unchanged");
    double speedAfter[2] = {}; speedBounds(speedAfter);
    Check(speedAfter[0] == speedFit[0] && speedAfter[1] == speedFit[1],
          "Fit time leaves both speed bounds unchanged");
    Check(host.applyCalls == applies && LocalWriteHistory(window) == history && KeyCount(window) == 4 &&
              GetText(window, kIdStatus) == selectionStatus,
          "neither fit writes data, history, keys or the selection status");
    Check(ReadCompareSpan(window, span) && span[2] == 1 && span[0] == 20.0 && span[1] == 40.0,
          "the key selection survives both fits unchanged");
    // A real trivial press on empty canvas clears the whole selection.
    const CurveEditorAxisState clearedAxis = ReadEditorAxis(window);
    HWND clearCanvas = GetDlgItem(window, kIdCanvas);
    const LPARAM empty = MAKELPARAM((clearedAxis.plot.left + clearedAxis.plot.right) / 2,
                                    clearedAxis.plot.top + 6);
    SendMessageW(clearCanvas, WM_LBUTTONDOWN, MK_LBUTTON, empty);
    SendMessageW(clearCanvas, WM_LBUTTONUP, 0, empty);
    Check(SelectionCount(window) == -1 && GetText(window, kIdStatus).find(L"\u6e05\u9664") != std::wstring::npos,
          "an empty canvas click clears the key selection");
    Click(window, kIdFitAll); ReadView(window, view);
    Check(view[0] == 0.0 && view[1] == 100.0 && view[2] < -100.0 && view[3] > 100.0,
          "without a valid selection Fit all keeps the visible all-track fallback");
    ClickKey(window, 0);
    Check(SelectionCount(window) == 1 && GetText(window, kIdFrameEdit) == L"0",
          "the frame 0 key is selected alone");
    ReadView(window, before);
    Click(window, kIdFitRange); ReadView(window, fitted);
    Check(std::isfinite(fitted[0]) && std::isfinite(fitted[1]) && fitted[0] == 0.0 && fitted[1] == 2.0 &&
              fitted[2] == before[2] && fitted[3] == before[3],
          "one selected frame at zero fits a finite non-zero [0,2] window and keeps Y");
    ce::CloseOwned(nullptr);
    // A single selected key at the maximum frame slides the +-1 window back.
    HostSource edge;
    edge.data.push_back(MakeChannel("edge", L"Edge", {MakeKey(0, 0.0), MakeKey(2147483647, 0.0)}));
    ce::Session edgeSession = MakeSession(edge);
    window = static_cast<HWND>(ce::Open(nullptr, edgeSession, false));
    Check(IsWindow(window) && KeyCount(window) == 2, "the maximum-frame fixture opens");
    const POINT edgePoint = AskPoint(window, kHookPointForKey, 1);
    HWND canvas = GetDlgItem(window, kIdCanvas);
    Check(edgePoint.x > 3 && edgePoint.y > 0, "the maximum-frame key lies inside the canvas");
    const LPARAM edgeHit = MAKELPARAM(edgePoint.x - 3, edgePoint.y);
    SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, edgeHit);
    SendMessageW(canvas, WM_LBUTTONUP, 0, edgeHit);
    Check(SelectionCount(window) == 1, "the maximum-frame key is selected alone");
    Click(window, kIdFitRange); ReadView(window, fitted);
    Check(fitted[0] == 2147483645.0 && fitted[1] == 2147483647.0,
          "one selected frame at the maximum frame fits the clamped [max-2,max] window");
    ce::CloseOwned(nullptr);
}

void MorphDefaultRange() {
    HostSource morph, plain;
    morph.data.push_back(MakeChannel("morph.weight", L"Weight", {MakeKey(0, -3.0), MakeKey(40, 7.0)}));
    plain.data.push_back(MakeChannel("plain", L"Plain", {MakeKey(0, 0.0), MakeKey(40, 1.0)}));
    ce::Session session;
    session.id = "morph-defaults"; session.language = 2;
    session.sources.push_back(morph.Make("morph", L"Morph"));
    session.sources.push_back(plain.Make("plain", L"Plain"));
    HWND window = static_cast<HWND>(ce::Open(nullptr, session, false));
    Check(IsWindow(window) && KeyCount(window) == 2, "the morph source opens on its two keys");
    double view[4] = {};
    ReadView(window, view);
    Check(view[2] == 0.0 && view[3] == 1.0, "a fresh morph source opens on exactly the 0..1 weight window");
    Check(ce::Open(nullptr, session, false) == window, "the morph editor is reused");
    ReadView(window, view);
    Check(view[2] == 0.0 && view[3] == 1.0, "a reused Open keeps the morph 0..1 window");
    HWND combo = GetDlgItem(window, kIdSourceCombo);
    const auto switchSource = [&](int index) {
        SendMessageW(combo, CB_SETCURSEL, index, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(kIdSourceCombo, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
    };
    switchSource(1); ReadView(window, view);
    Check(view[2] < -0.01 && view[3] > 1.01,
          "a non-morph source bounded to 0..1 is fitted from its data, not mistaken for morph");
    switchSource(0); ReadView(window, view);
    Check(view[2] == 0.0 && view[3] == 1.0,
          "switching back to the loaded morph source restores the 0..1 window");
    ce::CloseOwned(nullptr);
    window = static_cast<HWND>(ce::Open(nullptr, session, false));
    ReadView(window, view);
    Check(IsWindow(window) && view[2] == 0.0 && view[3] == 1.0,
          "a close/reopen keeps the morph 0..1 window");
    const int applies = morph.applyCalls;
    Click(window, kIdFitAll); ReadView(window, view);
    Check(view[2] < -1.0 && view[3] > 1.0,
          "an explicit Fit all after the morph entry exposes the real values below 0 and above 1");
    const double fitY0 = view[2], fitY1 = view[3];
    Click(window, kIdFitRange); ReadView(window, view);
    Check(view[2] == fitY0 && view[3] == fitY1, "Fit time keeps the explicit morph Y window");
    Check(morph.applyCalls == applies && KeyCount(window) == 2,
          "the morph display default and both fits never edit or write keys");
    ce::CloseOwned(nullptr);
}

class PrivateTestDesktop {
    // GetThreadDesktop returns a borrowed handle; only CreateDesktopW's handle
    // belongs to this scope. Never close a thread's borrowed desktop handle.
    HDESK original_ = nullptr;
    HDESK owned_ = nullptr;
    void Require(bool ok, const char* operation) const {
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok) {
            std::fprintf(stderr, "PRIVATE_DESKTOP failure=%s error=%lu pid=%lu tid=%lu original=%p owned=%p current=%p\n",
                         operation, error, GetCurrentProcessId(), GetCurrentThreadId(),
                         static_cast<void*>(original_), static_cast<void*>(owned_),
                         static_cast<void*>(GetThreadDesktop(GetCurrentThreadId())));
            // A snapshot is evidence at failure time, not proof that no thread
            // could have exited or changed desktops during enumeration.
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{sizeof(entry)};
                if (Thread32First(snapshot, &entry)) do {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    SetLastError(ERROR_SUCCESS);
                    HDESK desktop = GetThreadDesktop(entry.th32ThreadID);
                    const DWORD lookupError = GetLastError();
                    wchar_t name[256]{};
                    if (desktop) GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), nullptr);
                    std::fprintf(stderr, "PRIVATE_DESKTOP thread=%lu desktop=%p lookupError=%lu name=%ls\n",
                                 entry.th32ThreadID, static_cast<void*>(desktop), lookupError, name);
                    // Diagnostic only: dynamically query the Win32 start address
                    // and resolve its containing module without suspending threads.
                    using QueryThread = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
                    auto query = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
                    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                    void* start = nullptr;
                    const LONG status = thread && query ? query(thread, 9, &start, sizeof(start), nullptr) : -1;
                    HMODULE module = nullptr;
                    wchar_t path[MAX_PATH]{};
                    if (status >= 0 && start && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(start), &module))
                        GetModuleFileNameW(module, path, MAX_PATH);
                    std::fprintf(stderr, "PRIVATE_DESKTOP thread=%lu start=%p queryStatus=%ld module=%ls offset=%llu\n",
                                 entry.th32ThreadID, start, status, path,
                                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(start) - reinterpret_cast<std::uintptr_t>(module)));
                    if (thread) CloseHandle(thread);
                } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
            } else {
                std::fprintf(stderr, "PRIVATE_DESKTOP snapshotError=%lu\n", GetLastError());
            }
        }
        Check(ok, operation);
    }
public:
    static bool Enabled() {
        wchar_t enabled[2]{};
        return GetEnvironmentVariableW(L"MMS_CURVE_PRIVATE_DESKTOP", enabled, 2) == 1 && enabled[0] == L'1';
    }
    explicit PrivateTestDesktop(const char* childDesktop = nullptr) {
        if (!childDesktop) return;
        original_ = GetThreadDesktop(GetCurrentThreadId());
        Require(original_ != nullptr, "get borrowed regression desktop");
        char name[256]{};
        Require(GetUserObjectInformationA(original_, UOI_NAME, name, sizeof(name), nullptr) != FALSE,
                "query inherited regression desktop name");
        Check(std::strcmp(name, childDesktop) == 0, "regression child uses the requested desktop");
        BOOL input = TRUE;
        DWORD needed = 0;
        Require(GetUserObjectInformationW(original_, UOI_IO, &input, sizeof(input), &needed) != FALSE,
                "query inherited regression desktop input state");
        Check(!input, "curve regression uses a non-input desktop");
    }
    static int RunChild() {
        // Focus/edit operations can start process-lifetime MSCTF workers bound
        // to this desktop. Window teardown cannot retire those OS workers.
        // Keep the owning handle in a supervisor that never joins the desktop;
        // wait for normal process exit before asserting CloseDesktop succeeds.
        PrivateTestDesktop desktop;
        desktop.original_ = GetThreadDesktop(GetCurrentThreadId());
        desktop.Require(desktop.original_ != nullptr, "get supervisor desktop");
        wchar_t name[96]{};
        swprintf_s(name, L"MMS-CurveRegression-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
        desktop.owned_ = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        desktop.Require(desktop.owned_ != nullptr, "create owned regression desktop");
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        Check(length > 0 && length < 32768, "get regression child executable path");
        auto quote = [](const wchar_t* value) {
            std::wstring result = L"\"";
            unsigned slashes = 0;
            for (; *value; ++value) {
                if (*value == L'\\') { ++slashes; continue; }
                result.append(*value == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                result += *value;
                slashes = 0;
            }
            result.append(slashes * 2, L'\\');
            return result + L'"';
        };
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        desktop.Require(arguments != nullptr, "parse regression child arguments");
        std::wstring command = quote(executable) + L" --private-desktop-child " + quote(name);
        for (int index = 1; index < count; ++index) command += L" " + quote(arguments[index]);
        LocalFree(arguments);
        HANDLE inherited[3]{};
        const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
        for (int index = 0; index < 3; ++index) {
            HANDLE source = GetStdHandle(ids[index]);
            if (source && source != INVALID_HANDLE_VALUE) {
                desktop.Require(DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &inherited[index],
                                0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE, "duplicate regression child standard handle");
            } else {
                Check(index == 0, "regression child requires stdout and stderr");
                SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
                inherited[index] = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               &security, OPEN_EXISTING, 0, nullptr);
                desktop.Require(inherited[index] != INVALID_HANDLE_VALUE, "open regression child null input");
            }
        }
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        Check(bytes != 0, "size regression child handle whitelist");
        std::vector<unsigned char> storage(bytes);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        desktop.Require(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE,
                        "initialize regression child handle whitelist");
        desktop.Require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                        inherited, sizeof(inherited), nullptr, nullptr) != FALSE, "set regression child handle whitelist");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.lpDesktop = name;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inherited[0];
        startup.StartupInfo.hStdOutput = inherited[1];
        startup.StartupInfo.hStdError = inherited[2];
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        std::fflush(stdout); std::fflush(stderr);
        const BOOL started = CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE,
                                           EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
        const DWORD startError = started ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(attributes);
        for (HANDLE handle : inherited)
            desktop.Require(CloseHandle(handle) != FALSE, "close supervisor standard handle duplicate");
        SetLastError(startError);
        desktop.Require(started != FALSE, "start private desktop regression child");
        std::printf("PRIVATE_DESKTOP child=%lu desktop=%ls\n", child.dwProcessId, name);
        std::fflush(stdout);
        Check(WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0,
              "wait for regression child and all its threads to exit");
        DWORD exitCode = 1;
        desktop.Require(GetExitCodeProcess(child.hProcess, &exitCode) != FALSE, "get regression child exit code");
        desktop.Require(CloseHandle(child.hThread) != FALSE, "close regression child thread handle");
        desktop.Require(CloseHandle(child.hProcess) != FALSE, "close regression child process handle");
        Check(GetThreadDesktop(GetCurrentThreadId()) == desktop.original_, "supervisor stays on original desktop");
        desktop.Require(CloseDesktop(desktop.owned_) != FALSE, "release private regression desktop");
        desktop.owned_ = nullptr;
        std::printf("PRIVATE_DESKTOP released after child exit=%lu\n", exitCode);
        std::fflush(stdout);
        return static_cast<int>(exitCode);
    }
    ~PrivateTestDesktop() {
        if (!owned_) return;
        Check(GetThreadDesktop(GetCurrentThreadId()) == original_, "supervisor stays on original desktop during cleanup");
        Require(CloseDesktop(owned_) != FALSE, "release private regression desktop");
    }
    PrivateTestDesktop(const PrivateTestDesktop&) = delete;
    PrivateTestDesktop& operator=(const PrivateTestDesktop&) = delete;
};

} // namespace

int main(int argc,char** argv) {
    const char* childDesktop = nullptr;
    if (argc > 1 && std::strcmp(argv[1], "--private-desktop-child") == 0) {
        Check(argc >= 3 && PrivateTestDesktop::Enabled(), "validate private desktop child arguments");
        childDesktop = argv[2];
        argc -= 2; argv += 2;
    } else if (PrivateTestDesktop::Enabled() &&
               !(argc > 1 && std::strcmp(argv[1], "--paint-isolated-child") == 0)) {
        return PrivateTestDesktop::RunChild();
    }
    if (argc == 2 && std::strcmp(argv[1], "--drag-button-ownership") == 0) {
        PrivateTestDesktop desktop(childDesktop);
        CanvasDragButtonOwnership();
        std::puts("PASS curve drag button ownership and capture loss");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--drag-layout-interruption") == 0) {
        PrivateTestDesktop desktop(childDesktop);
        for (auto interruption : {DragInterruption::Resize, DragInterruption::Cancel}) {
            ValueHandleDragInvalidation(true, interruption);
            MmdHandleDragInvalidation(0, interruption);
        }
        KeyDragInterruption();
        std::puts("PASS curve drag layout interruption and retry");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--interface-scale") == 0) {
        LiveInterfaceScale();
        std::puts("PASS curve editor interface scale");
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "--private-desktop-only") == 0 ||
                      std::strcmp(argv[1], "--private-desktop-window") == 0 ||
                      std::strcmp(argv[1], "--private-desktop-selection-read") == 0 ||
                      std::strcmp(argv[1], "--private-desktop-failure-probe") == 0 ||
                      std::strcmp(argv[1], "--private-desktop-editor") == 0)) {
        wchar_t enabled[2]{};
        Check(GetEnvironmentVariableW(L"MMS_CURVE_PRIVATE_DESKTOP", enabled, 2) == 1 && enabled[0] == L'1',
              "desktop micro probe requires MMS_CURVE_PRIVATE_DESKTOP=1");
        {
            PrivateTestDesktop desktop(childDesktop);
            Check(std::strcmp(argv[1], "--private-desktop-failure-probe") != 0,
                  "intentional child failure verifies supervisor exit propagation");
            if (std::strcmp(argv[1], "--private-desktop-selection-read") == 0) {
                KeyboardModifiers keyboard; keyboard.Set();
                SelectionReadSemantics();
            }
            if (std::strcmp(argv[1], "--private-desktop-window") == 0) {
                // Hidden stock control only: no editor, message loop or input.
                HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
                                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(window != nullptr, "create hidden desktop micro probe window");
                Check(DestroyWindow(window) != FALSE, "destroy hidden desktop micro probe window");
            }
            if (std::strcmp(argv[1], "--private-desktop-editor") == 0) {
                HostSource host;
                host.data.push_back(MakeChannel("x", L"X", {MakeKey(0, 0), MakeKey(10, 5)}));
                Check(ce::Open(nullptr, MakeSession(host), false) != nullptr,
                      "open one hidden editor for desktop micro probe");
                ce::CloseOwned(nullptr);
            }
        }
        std::printf("PASS private desktop micro probe body (supervisor release pending): %d checks\n", g_checks);
        return 0;
    }
    PrivateTestDesktop privateDesktop(childDesktop);
    // Synthetic clicks use this thread's GetKeyState. Start from an explicit
    // baseline and restore it on exit; never inject or alter global input.
    KeyboardModifiers keyboardBaseline;keyboardBaseline.Set();
    // Optional observe-only first-chance diagnostics; inert unless the exact
    // MMS_CURVE_CRASH_DIAGNOSTICS=1 switch is present.  The scope removes the
    // handler on every normal return, including the internal paint child.
    CurveDiagLifetime curveDiagLifetime;(void)curveDiagLifetime;
    if(argc>1&&std::string(argv[1])=="--paint-isolated-child"){
        // Internal child launch from RunIsolatedPaintFixtures.  It runs only the
        // two original paint fixtures and never reaches the ordinary dispatch
        // below, so it cannot recurse into RunIsolatedPaintFixtures.  Any
        // malformed internal invocation is rejected here as well.
        const bool validArguments=argc==3||(argc==4&&std::string(argv[3])=="--paint-isolated-heap-validate");
        if(!validArguments){
            std::fprintf(stderr,"FAIL: --paint-isolated-child requires exactly one desktop name and optionally --paint-isolated-heap-validate\n");
            std::fflush(stderr);
            return 1;
        }
        return RunIsolatedPaintChild(argv[2],argc==4);
    }
    // Optional same-order diagnostic for a prior 0xC0000374 full-suite exit.
    // It retains every assertion and validates the process heap at group ends.
    g_validateHeap=argc>1&&std::string(argv[1])=="--heap-validate-all";
    if(argc>1&&std::string(argv[1])=="--sampler-handle-only"){
        SamplerHandleCancellation();std::printf("curve sampler handle: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--sampler-rendering-only"){
        SamplerDeferredRedrawAndGeometry();std::printf("curve sampler rendering: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--sampler-lifetime"){
        SamplerLifetime();std::printf("curve sampler lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--sampler-copy-boundary"){
        SamplerCopyBoundary();std::printf("curve sampler copy boundary: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--subdivision-geometry-safe"){
        SubdivisionGeometrySafe();std::printf("curve subdivision geometry safe: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--subdivision-writer-copy-safe"){
        SubdivisionWriterCopySafe();std::printf("curve subdivision writer copy safe: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--subdivision-lifetime"){
        SubdivisionLifetime();
        std::printf("curve subdivision lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--transport-write-lifetime"){
        TransportWriteLifetime();
        std::printf("curve transport write lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--native-construction-lifetime"){
        NativeConstructionLifetime();
        std::printf("curve native construction lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--native-initialization-reentry"){
        NativeInitialProviderRetirement<0>();NativeInitialProviderRetirement<1>();NativeInitialProviderRetirement<2>();
        NativeInitialProviderRetirement<3>();NativeInitialProviderRetirement<4>();NativeInitialProviderRetirement<5>();NativeInitialProviderRetirement<6>();
        std::printf("curve native initialization reentry: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--transport-query-lifetime"){
        TransportCallbackLifetime();TransportFailuresAndMissing();TransportNestedAndSessionCache();TransportInvalidFramesAndMmdSource();TransportPaintStateAndDenseTicks();
        std::printf("curve transport query lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--comparison-lifetime"){
        ComparisonCallbackLifetime();ComparisonWriterAtomicity();ComparisonFilterAcceptedAllocation();
        ComparisonReadFailuresAndEntries();ComparisonReadAndLinkSemantics();ComparisonValueHandlesDoNotQueryLink();ComparisonNestedAndGeometryCopy();
        std::printf("curve comparison lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--selection-lifetime"){
        SelectionCallbackLifetime();SelectionFailuresAndEcho();SelectionNestedAndHistory();
        SelectionReadSemantics();SelectionDenseStableTick();
        std::printf("curve selection lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--write-lifetime"){
        WriteCallbackLifetime();WritePostCommitReadRetirement();WriteFailureKeepsLocalHistory();
        WriteCanonicalFailureKeepsPublication();WriteAcceptedAllocationFailure();
        WriteRejectsNestedAuthoring();ExternalHistoryRefreshFailures();
        std::printf("curve write lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--source-lifetime"){
        SourceCallbackLifetime();SourceCallbackFinalRelease();OriginalSessionFinalRelease();
        ProviderCopyRetirement();SourceLanguageAndOpenFailures();
        std::printf("curve source lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--motion-lifetime"){
        MotionButtons();MotionCallbackLifetime();
        std::printf("curve motion lifetime: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--external-history"){
        ExternalHistoryChecks();std::printf("external curve history: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--reopen-paint-only"){
        RunIsolatedPaintFixtures();
        std::printf("curve editor reopen paint and handle drag: %d parent-only checks (isolated paint child checks reported separately)\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--selection-fit-only"){
        SelectionFitRegressions();MorphDefaultRange();
        std::printf("curve selection fit and morph default: %d checks\n",g_checks);return 0;
    }

    if(argc>1&&std::string(argv[1])=="--native-subdivision"){
        NativeSubdivisionRoute();NativeSampleBake();
        std::printf("native editor subdivision/bake: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--timeline-input"){
        TimelineRulerNavigation();AuthoringKeyboardShortcuts();
        std::printf("curve timeline/input regressions: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--key-operations"){
        HiddenLifecycle();AddDeleteUndoRedo();DragKey();RubberBandMultiMove();RefusedApply();
        IndependentValueHandles();AlignedValueHandles();AdjacentHandleLinking();
        TimelineRulerNavigation();AuthoringKeyboardShortcuts();RangeBarKeyboard();
        CurveModeButtons();TabNavigation();BakeSelectedChannels();
        std::printf("curve editing gestures: %d checks\n",g_checks);return 0;
    }
    if(argc>1&&std::string(argv[1])=="--native-sync"){
        PendingNativeChangesBeforeWrite(false);NativeSelectionBeforeDelete();NativeSelectionBeforeDelete(false);
        NativeEditsBeforeHistory();CanonicalWriteAndUndo();NativeChangesDuringMmdDrag();
        std::printf("native synchronization regressions: %d checks\n",g_checks);return 0;
    }
    if(argc>1 && std::string(argv[1])=="--curve-modes") {
        CurveModeButtons();
        std::printf("curve mode regressions: %d checks\n",g_checks); return 0;
    }
    if(argc>1 && std::string(argv[1])=="--curve-ui-compat") {
        IndependentValueHandles(); AlignedValueHandles(); AdjacentHandleLinking();
        LayoutThreeLanguages(); ComparisonLayout(); HoverHints(); TabNavigation();
        std::printf("curve UI compatibility regressions: %d checks\n",g_checks); return 0;
    }
    ExternalHistoryChecks();
    CurveModeButtons();
    if(argc>1 && std::string(argv[1])=="--editor-input") {
        IndependentValueHandles(); AlignedValueHandles(); AdjacentHandleLinking(); BakeSelectedChannels();
        std::printf("editor input regressions: %d checks\n",g_checks);
        return 0;
    }
    if(argc>1&&std::string(argv[1])=="auto-link"){AdjacentHandleLinking();return 0;}
    IndependentValueHandles();
    AlignedValueHandles();
    HiddenLifecycle();
    AddDeleteUndoRedo();
    DragKey();
    RubberBandMultiMove();
    TwoSources();
    RefusedApply();
    MmdPanel();
    MotionButtons();
    MotionCallbackLifetime();
    SourceCallbackLifetime();SourceCallbackFinalRelease();OriginalSessionFinalRelease();
    ProviderCopyRetirement();SourceLanguageAndOpenFailures();
    NativeConstructionLifetime();
    WriteCallbackLifetime();WritePostCommitReadRetirement();WriteFailureKeepsLocalHistory();
    WriteCanonicalFailureKeepsPublication();WriteAcceptedAllocationFailure();WriteRejectsNestedAuthoring();
    SelectionCallbackLifetime();SelectionFailuresAndEcho();SelectionNestedAndHistory();
    SelectionReadSemantics();SelectionDenseStableTick();
    ComparisonCallbackLifetime();ComparisonWriterAtomicity();ComparisonFilterAcceptedAllocation();
    ComparisonReadFailuresAndEntries();ComparisonReadAndLinkSemantics();ComparisonValueHandlesDoNotQueryLink();ComparisonNestedAndGeometryCopy();
    TransportCallbackLifetime();TransportFailuresAndMissing();TransportNestedAndSessionCache();TransportInvalidFramesAndMmdSource();TransportPaintStateAndDenseTicks();
    TransportWriteLifetime();
    SubdivisionLifetime();
    SubdivisionGeometrySafe();SubdivisionWriterCopySafe();
    SamplerLifetime();
    BakeSelectedChannels();
    NativeSubdivisionRoute();NativeSampleBake();
    DuplicateFrameSelectsExisting();
    DeleteRecomputesAutomatic();
    FrameRangeRejected();
    LocalizedSourceLabel();
    ReadKeepsSelectionAndHistory();
    PendingNativeChangesBeforeWrite();
    PendingNativeChangesBeforeWrite(false);
    NativeSelectionBeforeDelete();
    NativeSelectionBeforeDelete(false);
    NativeEditsBeforeHistory();
    CanonicalWriteAndUndo();
    NativeChangesDuringMmdDrag();
    TimelineRulerNavigation();AuthoringKeyboardShortcuts();
    PlayheadTimer();
    RangeBarKeyboard();
    TabNavigation();
    LauncherIcon();
    CanvasPainting();
    LayoutThreeLanguages();
    ComparisonModes();
    ComparisonRangeFilter();
    ComparisonPainting();
    ComparisonNoRegression();
    ComparisonLayout();
    ChannelListInteraction();
    HoverHints();
    ComparisonModeRestoresMainCurve();
    AdjacentHandleLinking();
    SelectionFitRegressions();
    MorphDefaultRange();
    RunIsolatedPaintFixtures();
    std::printf("Animation curve editor regressions passed (%d parent-only checks; isolated paint child checks reported separately)\n", g_checks);
    return 0;
}
