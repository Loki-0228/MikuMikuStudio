// Real PMX edits and real PMM workspace reconciliation in a hidden production
// host. Run a copied executable from its own fresh isolation directory:
//   host-run/pmm_model_adaptation_tests.exe host-run
// The real module path must place all local_data files in host-run/data.
// This is not a complete interactive or hardware compatibility acceptance test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/text_encoding.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <commctrl.h>
#include <MinHook.h>
#include <cmath>
#include <iomanip>
#include "../src/window/curve_editor_axis.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/scene_store.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#endif

#if MMD_ORIGINAL_PROFILE
int wmain() {
    std::puts("SKIP: this suite verifies full-profile MMS project extensions and identities");
    return 77;
}
#else
#include "pmm_selection_restore_checks.inc"
using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0, adaptations = 0;
MMDApp* activeApp = nullptr;
bool acceptAdaptation = false;
const wchar_t* expectedOpenError = nullptr;
unsigned expectedOpenDialogs = 0;
const char* activeCase = "initialization";

void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL [%s] %s\n", activeCase, message); std::exit(1); }
}
void PrintDialog(HWND window) {
    wchar_t title[1024]{}, type[64]{};
    GetWindowTextW(window, title, 1024); GetClassNameW(window, type, 64);
    std::fwprintf(stderr, L"Dialog HWND=%p class=%ls title=%ls\n", window, type, title);
    EnumChildWindows(window, [](HWND child, LPARAM)->BOOL {
        wchar_t text[2048]{}, type[64]{};
        GetWindowTextW(child, text, 2048); GetClassNameW(child, type, 64);
        if (*text) std::fwprintf(stderr, L"  child id=%d class=%ls text=%ls\n", GetDlgCtrlID(child), type, text);
        return TRUE;
    }, 0);
    std::fflush(stderr);
}
[[noreturn]] void UnexpectedDialog(HWND window, const char* reason) {
    std::fprintf(stderr, "FAIL [%s] unexpected dialog: %s\n", activeCase, reason);
    PrintDialog(window); std::exit(3);
}
LRESULT CALLBACK AfterDialogInit(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (message.message == WM_INITDIALOG) {
            HWND window = message.hwnd; wchar_t type[64]{};
            GetClassNameW(window, type, 64);
            if (!std::wcscmp(type, L"#32770")) {
                DWORD process = 0;
                if (GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId())
                    UnexpectedDialog(window, "the thread hook received a foreign window");
                const auto owner = GetWindow(window, GW_OWNER);
                if (!activeApp || owner != activeApp->MainWindow())
                    UnexpectedDialog(window, "the dialog is not owned by this hidden test host");
                if (expectedOpenError) {
                    wchar_t title[256]{}, buttonType[64]{};
                    GetWindowTextW(window, title, 256);
                    auto button = GetDlgItem(window, IDOK);
                    // Some message-box builds expose the sole OK-labelled
                    // button as IDCANCEL during WM_INITDIALOG completion.
                    if (!button) {
                        auto candidate = GetDlgItem(window, IDCANCEL); wchar_t caption[32]{};
                        if (candidate) GetWindowTextW(candidate, caption, 32);
                        if (!std::wcscmp(caption, L"OK")) button = candidate;
                    }
                    if (button) GetClassNameW(button, buttonType, 64);
                    struct ExpectedText { const wchar_t* needle; bool found; } text{expectedOpenError, false};
                    EnumChildWindows(window, [](HWND child, LPARAM data)->BOOL {
                        auto& expected = *reinterpret_cast<ExpectedText*>(data);
                        wchar_t value[2048]{}; GetWindowTextW(child, value, 2048);
                        if (std::wcsstr(value, expected.needle)) expected.found = true;
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&text));
                    if (std::wcscmp(title, L"Load project") || !button || !IsChild(window, button) ||
                        std::wcscmp(buttonType, L"Button") || !text.found || expectedOpenDialogs)
                        UnexpectedDialog(window, "expected exactly one owned project-open error with its real OK button");
                    ++expectedOpenDialogs;
                    // Simulate the already-pending Enter edge in the real modal
                    // dialog; the actual button's normal command closes it.
                    activeApp->state.enterKeyState = 1;
                    if (!PostMessageW(window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button)))
                        UnexpectedDialog(window, "could not queue the actual error dialog OK command");
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                const auto button = GetDlgItem(window, 687);
                wchar_t buttonType[64]{}, caption[256]{}, body[1024]{};
                if (button) { GetClassNameW(button, buttonType, 64); GetWindowTextW(button, caption, 256); }
                GetDlgItemTextW(window, 686, body, 1024);
                if (!acceptAdaptation || !button || !IsChild(window, button) ||
                    std::wcscmp(buttonType, L"Button") ||
                    std::wcscmp(caption, L"Adapt PMM model data and continue") ||
                    !std::wcsstr(body, L"Model structure differs from the PMM:"))
                    UnexpectedDialog(window, "expected only the actual structure-mismatch button 687");
                if (GetPropW(window, L"MmsAdaptationHostInitSeen"))
                    UnexpectedDialog(window, "the same compatibility window initialized twice");
                if (!SetPropW(window, L"MmsAdaptationHostInitSeen", reinterpret_cast<HANDLE>(1)))
                    UnexpectedDialog(window, "could not mark the owned compatibility window");
                ++adaptations;
                std::printf("DIAG [%s] genuine compatibility dialog=%p button=%p\n", activeCase, window, button);
                // WM_INITDIALOG has already returned from the real dialog proc.
                // Queue its actual button notification, including the real
                // button HWND. Only production ProjectModelDialogProc decides
                // the result and closes the dialog; the test never EndDialogs it.
                SetWindowPos(window, nullptr, -32000, -32000, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (!PostMessageW(window, WM_COMMAND, MAKEWPARAM(687, BN_CLICKED), reinterpret_cast<LPARAM>(button)))
                    UnexpectedDialog(window, "could not queue the actual compatibility button notification");
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
struct DialogHook {
    HHOOK value = SetWindowsHookExW(WH_CALLWNDPROCRET, AfterDialogInit, nullptr, GetCurrentThreadId());
    DialogHook() { Check(value != nullptr, "install the local after-WM_INITDIALOG observer"); }
    ~DialogHook() { if (value) UnhookWindowsHookEx(value); }
};

template<class T> void Put(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
void Text(std::ofstream& file, const std::wstring& value) {
    static_assert(sizeof(wchar_t) == 2, "the fixture encodes PMX UTF16LE on Windows");
    Put(file, std::int32_t(value.size() * sizeof(wchar_t)));
    file.write(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
}
constexpr wchar_t kDiskLocalComment[] = L"PMX source local comment";
constexpr wchar_t kDiskEnglishComment[] = L"PMX source English comment";
constexpr wchar_t kSavedLocalComment[] = L"PMM independent local comment \u4FDD\u7559\u5907\u6CE8";
constexpr wchar_t kSavedEnglishComment[] = L"PMM independent English comment \u2606";
constexpr wchar_t kSavedEnglishBone[] = L"Saved English name \u2606";

void WritePmx(const fs::path& path, const std::vector<std::wstring>& bones,
              const std::vector<std::wstring>& morphs, unsigned groups = 1) {
    Check(!bones.empty() && bones.size() < 127 && morphs.size() < 127 && groups > 0,
        "the isolated PMX fixture fits its one-byte index representation");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(bool(file), "open an isolated PMX fixture");
    file.write("PMX ", 4); Put(file, 2.0f);
    const unsigned char header[]{0, 0, 1, 1, 1, 1, 1, 1};
    Put(file, static_cast<unsigned char>(sizeof(header))); file.write(reinterpret_cast<const char*>(header), sizeof(header));
    Text(file, L"Adaptation"); Text(file, L"Adaptation-English");
    Text(file, kDiskLocalComment); Text(file, kDiskEnglishComment);
    for (unsigned i = 0; i < 4; ++i) Put(file, std::int32_t(0)); // no geometry, textures or materials
    Put(file, std::int32_t(bones.size()));
    for (const auto& name : bones) {
        Text(file, name); Text(file, name);
        Put(file, 0.f); Put(file, 0.f); Put(file, 0.f);
        Put(file, std::int8_t(-1)); Put(file, std::int32_t(0)); Put(file, std::uint16_t(0));
        Put(file, 0.f); Put(file, 1.f); Put(file, 0.f);
    }
    Put(file, std::int32_t(morphs.size()));
    for (const auto& name : morphs) {
        Text(file, name); Text(file, name); Put(file, std::uint8_t(1)); Put(file, std::uint8_t(1));
        Put(file, std::int32_t(0)); // vertex morph with no offsets
    }
    Put(file, std::int32_t(groups));
    for (unsigned group = 0; group < groups; ++group) {
        Text(file, L"Group" + std::to_wstring(group)); Text(file, L"Group-English-" + std::to_wstring(group));
        Put(file, std::uint8_t(0)); Put(file, std::int32_t(bones.size()));
        for (std::size_t i = 0; i < bones.size(); ++i) { Put(file, std::uint8_t(0)); Put(file, std::int8_t(i)); }
    }
    Put(file, std::int32_t(0)); Put(file, std::int32_t(0)); // no rigid bodies or joints
    Check(bool(file), "write a complete real PMX fixture");
}
using Values = std::map<std::string, float>;
struct Motion { Values bones, morphs; };
Motion Seed(mdl::ModelRecord& model, unsigned slot) {
    Check(mdl::BoneKeyCapacity(model) >= model.boneCount * 3 &&
        mdl::MorphKeyCapacity(model) >= model.morphCount * 3,
        "real imported key pools have room for the two extra keys per track");
    Motion out;
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        const float value = 10.f + 40.f * slot + i; out.bones.emplace(model.boneTable[i].name, value);
        auto& head = model.boneKeys[i]; head.allocated = 1; head.position[0] = value;
        const auto first = model.boneCount + 2 * i; head.next = first;
        for (unsigned n = 0; n < 2; ++n) {
            auto& key = model.boneKeys[first + n]; key = head; key.frame = 10 * (n + 1);
            key.position[0] = value + n + 1; key.previous = n == 0 ? i : first; key.next = n == 0 ? first + 1 : 0;
        }
    }
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        const float value = .1f * (i + 1) + .4f * slot; out.morphs.emplace(model.morphs[i].name, value);
        auto& head = model.morphKeys[i]; head.allocated = 1; head.value = value;
        const auto first = model.morphCount + 2 * i; head.next = first;
        for (unsigned n = 0; n < 2; ++n) {
            auto& key = model.morphKeys[first + n]; key = head; key.frame = 10 * (n + 1);
            key.value = value + .01f * (n + 1); key.previous = n == 0 ? i : first; key.next = n == 0 ? first + 1 : 0;
        }
    }
    return out;
}
template<class Key, class Value> void VerifyTrack(const Key* keys, std::size_t capacity,
    std::uint32_t head, float expected, float increment, Value value) {
    Check(head < capacity && value(keys[head]) == expected, "mapped frame-zero value survives");
    auto previous = head;
    for (unsigned n = 0; n < 2; ++n) {
        const auto next = keys[previous].next;
        Check(next != 0 && next < capacity && keys[next].previous == previous && keys[next].frame == 10 * (n + 1),
            "mapped sparse key links are in bounds with the expected predecessor and time");
        Check(value(keys[next]) == expected + increment * (n + 1), "mapped sparse key value belongs to its named source track");
        previous = next;
    }
    Check(keys[previous].next == 0, "mapped track has exactly the two seeded sparse keys");
}
void VerifyMotion(const mdl::ModelRecord& model, const Motion& expected) {
    const auto bones = mdl::BoneKeyCapacity(model), morphs = mdl::MorphKeyCapacity(model);
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        const auto found = expected.bones.find(model.boneTable[i].name);
        if (found == expected.bones.end())
            Check(model.boneKeys[i].next == 0 && model.boneKeys[i].position[0] == 0.f && model.boneKeys[i].rotation[3] == 1.f,
                "a new or renamed bone has a default pose and no old track");
        else VerifyTrack(model.boneKeys, bones, i, found->second, 1.f, [](const auto& key) { return key.position[0]; });
    }
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        const auto found = expected.morphs.find(model.morphs[i].name);
        if (found == expected.morphs.end()) Check(model.morphKeys[i].next == 0 && model.morphKeys[i].value == 0.f,
            "a new or renamed morph has zero weight and no old track");
        else VerifyTrack(model.morphKeys, morphs, i, found->second, .01f, [](const auto& key) { return key.value; });
    }
}
void VerifyNames(const mdl::ModelRecord& model, const std::vector<std::wstring>& bones,
                 const std::vector<std::wstring>& morphs) {
    Check(model.boneCount == bones.size() && model.morphCount == morphs.size(), "replacement PMX table sizes survive PMM loading");
    for (std::size_t i = 0; i < bones.size(); ++i) for (bool english : {false, true})
        Check(text_encoding::BoneName(model.boneTable[i], english) == bones[i], "replacement bone names and order survive in both languages");
    for (std::size_t i = 0; i < morphs.size(); ++i) for (bool english : {false, true})
        Check(text_encoding::MorphName(model.morphs[i], english) == morphs[i], "replacement morph names and order survive in both languages");
}
void VerifyComments(const mdl::ModelRecord& model, bool saved) {
    const auto* native = saved ? kSavedLocalComment : kDiskLocalComment;
    const auto* english = saved ? kSavedEnglishComment : kDiskEnglishComment;
    Check(model.pmxTextBuffers[2] && model.pmxTextBuffers[3] &&
        !std::wcscmp(model.pmxTextBuffers[2], native) && !std::wcscmp(model.pmxTextBuffers[3], english),
        "independent comments follow their own record rather than the skipped name dictionary");
}
void NoHistory(MMDApp& app) {
    auto* history = plugins::NativeHistory(&app);
    const auto admitted = history ? history->CheckNativeEdit() : MMS_E_NOT_READY;
    if (!history || admitted != MMS_OK || history->UndoCount() || history->RedoCount())
        std::printf("DIAG [%s] history=%p gate=%d undo=%zu redo=%zu layoutReady=%u mutation=%u visible=%u\n",
            activeCase, history, int(admitted), history ? history->UndoCount() : 0, history ? history->RedoCount() : 0,
            unsigned(app.WindowLayoutReady()), unsigned(app.SceneMutationInProgress()), unsigned(IsWindowVisible(app.MainWindow())));
    Check(history && admitted == MMS_OK && !history->UndoCount() && !history->RedoCount(),
        "the ordinary host remains ready with no new shared-history entries");
    Check(!app.SceneMutationInProgress() && !IsWindowVisible(app.MainWindow()), "real PMM loading releases its mutation scope and the main host stays hidden");
}

struct Identity { MmsObjectRef object{}; std::vector<MmsId> bones, morphs; };
Identity CaptureIdentity(MMDApp& app, unsigned slot) {
    auto store = plugins::CreateSceneStore(&app); plugins::SceneClient client; const auto api = ([&]{ MmsSceneApiV1 value{}; if (store->Api(client, value) != MMS_OK) throw std::runtime_error("scene API binding failed"); return value; }());
    const auto object = plugins::SceneObjectIdentity(app.ModelSlot(slot));
    MmsSceneQueryV1 query{}; query.h = plugins::Header<MmsSceneQueryV1>();
    query.stage = MMS_STAGE_BIND; query.flags = MMS_SCENE_CURRENT_TIME;
    query.channels = MMS_CHANNEL_BONES | MMS_CHANNEL_MORPHS; query.objects = {&object, 1, sizeof(object), 1};
    MmsSnapshotV1 snapshot{};
    const auto acquired = api.acquire(api.context, &query, &snapshot);
    if (acquired != MMS_OK) std::printf("DIAG [%s] bind snapshot slot=%u result=%d layoutReady=%u mutation=%u\n",
        activeCase, slot, int(acquired), unsigned(app.WindowLayoutReady()), unsigned(app.SceneMutationInProgress()));
    Check(acquired == MMS_OK, "acquire a real bind snapshot to observe persisted object and element identities");
    struct Release { const MmsSceneApiV1& api; MmsHandle handle; ~Release() { api.release(api.context, handle); } } release{api, snapshot.handle};
    MmsSpan objects{}; Check(api.objects(api.context, snapshot.handle, &objects) == MMS_OK &&
        objects.data && objects.count == 1 && objects.recordVersion == 1 && objects.stride >= sizeof(MmsObjectV1),
        "the identity snapshot selects the exact live model");
    MmsObjectV1 selected{}; std::memcpy(&selected, objects.data, sizeof(selected)); Identity out; out.object = selected.object;
    Check((out.object.id.lo || out.object.id.hi) && out.object.generation,
        "the actual selected object has a nonempty ID and live runtime generation");
    MmsSpan bones{}, morphs{};
    Check(api.readChannel(api.context, snapshot.handle, out.object, MMS_CHANNEL_BONES, &bones) == MMS_OK &&
        api.readChannel(api.context, snapshot.handle, out.object, MMS_CHANNEL_MORPHS, &morphs) == MMS_OK,
        "read actual bone and morph IDs from the production scene transport");
    const auto& model = *mdl::Mdl(app.ModelSlot(slot));
    Check(bones.count == model.boneCount && morphs.count == model.morphCount &&
        (!bones.count || bones.data) && (!morphs.count || morphs.data) && bones.recordVersion == 1 && morphs.recordVersion == 1 &&
        bones.stride >= sizeof(MmsBoneV1) && morphs.stride >= sizeof(MmsMorphV1), "identity spans match the actual imported model tables");
    for (std::size_t i = 0; i < bones.count; ++i) { MmsBoneV1 bone{};
        std::memcpy(&bone, static_cast<const unsigned char*>(bones.data) + i * bones.stride, sizeof(bone));
        Check(bone.id.lo || bone.id.hi, "every actual bone exposes a nonempty element ID"); out.bones.push_back(bone.id); }
    for (std::size_t i = 0; i < morphs.count; ++i) { MmsMorphV1 morph{};
        std::memcpy(&morph, static_cast<const unsigned char*>(morphs.data) + i * morphs.stride, sizeof(morph));
        Check(morph.id.lo || morph.id.hi, "every actual morph exposes a nonempty element ID"); out.morphs.push_back(morph.id); }
    return out;
}
bool Same(MmsId first, MmsId second) { return first.lo == second.lo && first.hi == second.hi; }
void SameElements(const std::vector<MmsId>& actual, const std::vector<MmsId>& expected) {
    Check(actual.size() == expected.size(), "unchanged model keeps the same number of persisted elements");
    for (std::size_t i = 0; i < actual.size(); ++i) Check(Same(actual[i], expected[i]), "unchanged model restores each element ID at its original index");
}
void NoOldElements(const Identity& actual, const Identity& old) {
    for (const auto& current : {actual.bones, actual.morphs}) for (auto id : current) {
        Check(std::none_of(old.bones.begin(), old.bones.end(), [&](MmsId prior) { return Same(id, prior); }) &&
            std::none_of(old.morphs.begin(), old.morphs.end(), [&](MmsId prior) { return Same(id, prior); }),
            "adaptation retires all old positional element IDs instead of reusing an old index");
    }
}
void RestoredIdentity(const Identity& actual, const Identity& saved, bool adapted) {
    Check(Same(actual.object.id, saved.object.id) && actual.object.generation != saved.object.generation,
        "reopening preserves the scene object ID and creates a fresh runtime generation");
    if (adapted) NoOldElements(actual, saved);
    else { SameElements(actual.bones, saved.bones); SameElements(actual.morphs, saved.morphs); }
}
std::vector<char> Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); Check(bool(file), "open exact source-file byte observation");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void WriteBytes(const fs::path& path, const std::vector<char>& bytes, std::size_t size) {
    Check(size <= bytes.size(), "write only an observed source-file prefix");
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(bytes.data(), static_cast<std::streamsize>(size));
    Check(bool(file), "write an isolated standard-prefix or rejected-marker fixture");
}
void EarlyOpenEnterChecks(MMDApp& app, const fs::path& directory, const std::array<Motion, 2>& motion) {
    const auto missing = directory / L"missing-open.pmm";
    const auto invalid = directory / L"invalid-header.pmm";
    const auto unsupported = directory / L"unsupported-version.pmm";
    Check(!fs::exists(missing), "missing project fixture does not exist");
    std::vector<char> badHeader(30, '\0');
    WriteBytes(invalid, badHeader, badHeader.size());
    std::vector<char> badVersion(30, '\0');
    std::memcpy(badVersion.data(), "Polygon Movie maker", 19);
    std::memcpy(badVersion.data() + 20, "9999", 4);
    WriteBytes(unsupported, badVersion, badVersion.size());

    const std::wstring oldFile = app.EnvFileName(), oldDirectory = app.PathWorkspace().projectDirectory;
    const unsigned char oldDirty = app.SceneModified(), oldLayout = app.WindowLayoutReady();
    const int oldFrame = app.CurrentFrame(), oldEnter = app.state.enterKeyState;
    const std::array<unsigned char*, 2> models{app.ModelSlot(0), app.ModelSlot(1)};
    const std::array<MmsObjectRef, 2> identities{plugins::SceneObjectIdentity(models[0]), plugins::SceneObjectIdentity(models[1])};
    auto* const history = plugins::NativeHistory(&app);
    Check(history != nullptr, "open error fixture has the active shared history");
    std::uint64_t revision = 0;
    Check(history->Revision(revision) == MMS_OK, "capture shared history revision before open failures");
    const auto undo = history->UndoCount(), redo = history->RedoCount();
    const auto* const camera = app.CameraKeys();
    const auto cameraCapacity = mdl::KeyPoolCapacity(camera);
    Check(camera && cameraCapacity, "open error fixture has camera keyframes");
    const std::vector<mdl::CameraKey> cameraBefore(camera, camera + cameraCapacity);
    struct ErrorCase { const char* name; fs::path path; const wchar_t* text; };
    const ErrorCase cases[]{
        {"open-missing", missing, L"Cannot open the project file."},
        {"open-invalid-header", invalid, L"This is not a valid PMM file."},
        {"open-unsupported-version", unsupported, L"This PMM file version is not supported."}
    };
    for (const auto& item : cases) {
        activeCase = item.name; expectedOpenError = item.text; expectedOpenDialogs = 0;
        app.state.enterKeyState = 0;
        Check(!LoadSceneFile(item.path.c_str()), "early project-open failure is rejected");
        expectedOpenError = nullptr;
        Check(expectedOpenDialogs == 1 && app.state.enterKeyState == 3,
            "the modal error consumes the pending Enter edge before returning");
        Check(std::wstring(app.EnvFileName()) == oldFile && std::wstring(app.PathWorkspace().projectDirectory) == oldDirectory &&
            app.SceneModified() == oldDirty && app.WindowLayoutReady() == oldLayout && app.CurrentFrame() == oldFrame,
            "early open failure preserves project identity, dirty state, layout and playhead");
        std::uint64_t after = 0;
        Check(history == plugins::NativeHistory(&app) && history->Revision(after) == MMS_OK && after == revision &&
            history->UndoCount() == undo && history->RedoCount() == redo,
            "early open failure preserves the shared history and revision");
        Check(app.CameraKeys() == camera && mdl::KeyPoolCapacity(camera) == cameraCapacity &&
            !std::memcmp(camera, cameraBefore.data(), cameraCapacity * sizeof(mdl::CameraKey)),
            "early open failure preserves all camera keyframes");
        for (unsigned slot = 0; slot < 2; ++slot) {
            Check(app.ModelSlot(slot) == models[slot], "early open failure keeps the original model object");
            const auto identity = plugins::SceneObjectIdentity(app.ModelSlot(slot));
            Check(Same(identity.id, identities[slot].id) && identity.generation == identities[slot].generation,
                "early open failure preserves model identity and generation");
            VerifyMotion(*mdl::Mdl(app.ModelSlot(slot)), motion[slot]);
        }
    }
    activeCase = "open-empty-path"; expectedOpenDialogs = 0; app.state.enterKeyState = 1;
    Check(!LoadSceneFile(L"") && expectedOpenDialogs == 0 && app.state.enterKeyState == 1,
        "an empty path returns before any modal dialog or Enter consumption");
    app.state.enterKeyState = oldEnter;
}
bool Load(MMDApp& app, const fs::path& path, bool prompt) {
    adaptations = 0; acceptAdaptation = prompt;
    const bool loaded = LoadSceneFile(path.c_str()); acceptAdaptation = false;
    Check(loaded && adaptations == (prompt ? 1u : 0u), "real PMM load succeeds with exactly the expected genuine compatibility dialog count");
    NoHistory(app); return loaded;
}
void SameDirectory(const fs::path& actual, const fs::path& expected, const char* message) {
    Check(!_wcsicmp(actual.lexically_normal().c_str(), expected.lexically_normal().c_str()), message);
}
void VerifyChangedGroups(const mdl::ModelRecord& model) {
    Check(mdl::DisplayGroupCount(model) == 3, "groups-only PMX change really alters the allocated display table");
    const auto* groups = static_cast<const mdl::DisplayGroup*>(model.groupNames);
    Check(text_encoding::DisplayGroupName(groups[2], false) == L"Group2" &&
        text_encoding::DisplayGroupName(groups[2], true) == L"Group-English-2",
        "the additional display group retains both names from the replacement PMX");
}
} // namespace

__declspec(noinline) int RunExistingPmmCases(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0); SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto run = fs::absolute(argv[1]).lexically_normal();
    Check(fs::is_directory(run) && fs::equivalent(local_data::ExecutableDirectory(), run),
        "run the actual copied executable from the supplied dedicated host-run directory");
    Check(run.native().size() + 100 < 256, "every actual PMX/PMM fixture path fits the existing 256-wchar path ABI");
    const auto marker = run / L"model-adaptation-test.started";
    const HANDLE first = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(first != INVALID_HANDLE_VALUE, "use a fresh run directory, never silently reuse another host run"); CloseHandle(first);
    const auto data = run / L"data", directory = data / L"model-adaptation-fixtures";
    fs::create_directories(data); Check(fs::create_directory(directory), "create a fresh isolated model fixture directory");
    SameDirectory(local_data::File(L"plugins"), data / L"plugins", "plugin data follows the real isolated executable root");
    SameDirectory(system_settings::Path(), data / L"settings.json", "system settings follow the real isolated executable root");
    const auto antialias = directory / L"antialias.ini", recent = directory / L"recent.txt";
    Check(SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG", antialias.c_str()) &&
        SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", recent.c_str()), "isolate explicit antialias and recent-project test settings");
    Check(WritePrivateProfileStringW(L"Antialias", L"Enabled", L"0", antialias.c_str()) &&
        WritePrivateProfileStringW(L"Antialias", L"Algorithm", L"0", antialias.c_str()), "disable antialias only in the isolated preference file");
    auto app = std::make_unique<MMDApp>(); activeApp = g_Block = app.get();
    app->state = MMDAppState{}; app->InitDefaults(); app->EnglishUI() = 1;
    DialogHook dialogs;
    Check(InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE), "initialize the actual hidden production host and D3D9 resources");
    app->EnglishUI() = 1;
    Check(!IsWindowVisible(app->MainWindow()), "real initialization honors the hidden-host test precondition");
    const std::vector<std::wstring> originalBones{L"RootBone", L"Arm", L"Leg"}, originalMorphs{L"Face", L"Blink", L"Smile"};
    const auto pmx = directory / L"model.pmx", unchanged = directory / L"unchanged.pmx", pmm = directory / L"source.pmm";
    WritePmx(pmx, originalBones, originalMorphs); WritePmx(unchanged, originalBones, originalMorphs);
    LoadModelFile(app.get(), pmx.c_str(), false); LoadModelFile(app.get(), unchanged.c_str(), false);
    Check(app->ModelSlot(0) && app->ModelSlot(1), "load two real PMX sources into the ordinary production host");
    const std::array<Motion, 2> motion{Seed(*mdl::Mdl(app->ModelSlot(0)), 0), Seed(*mdl::Mdl(app->ModelSlot(1)), 1)};
    Check(extended_names::Assign(mdl::Mdl(app->ModelSlot(1))->boneTable[1].enText, kSavedEnglishBone),
        "give only the unchanged model a saved Unicode dictionary sentinel");
    for (unsigned slot = 0; slot < 2; ++slot) {
        auto& model = *mdl::Mdl(app->ModelSlot(slot));
        Check(extended_names::Assign(model.pmxTextBuffers[2], kSavedLocalComment) &&
            extended_names::Assign(model.pmxTextBuffers[3], kSavedEnglishComment), "seed the independent comments extension on both real models");
    }
    const std::array<Identity, 2> originalIdentity{CaptureIdentity(*app, 0), CaptureIdentity(*app, 1)};
    skinning::SetPreference({skinning::Mode::Cpu}); pmm_settings::ProcessorChanged(*app);
    Check(SaveSceneCopy(app.get(), pmm.c_str()), "write the real original project, including populated identity records"); NoHistory(*app);
    const auto originalBytes = Bytes(pmm); std::uint64_t boundary = 0; pmm_extension::Document document;
    const int descriptor = _wopen(pmm.c_str(), _O_RDONLY | _O_BINARY); Check(descriptor >= 0, "open the actual saved source PMM");
    Check(pmm_extension::Read(descriptor, document, boundary) == pmm_extension::ReadResult::Loaded,
        "locate a genuine MMS extension behind the complete standard project body"); _close(descriptor);
    Check(boundary <= originalBytes.size() && originalBytes.size() >= pmm_extension::kTrailerSize &&
        !std::memcmp(originalBytes.data() + originalBytes.size() - pmm_extension::kTrailerSize, "MMSXPMM2", 8),
        "the real source writer emits only the MMS trailer marker");
    const auto legacy = directory / L"standard-only.pmm", rejected = directory / L"rejected-mds-marker.pmm";
    WriteBytes(legacy, originalBytes, static_cast<std::size_t>(boundary));
    const auto extended = directory / L"source-0003.pmm", extendedLegacy = directory / L"standard-0003.pmm";
    app->CurrentFrame() = frame_limits::legacyMaximum + 1;
    const auto needsExtended = save_compatibility::InspectPmm(*app);
    Check(needsExtended.needsNewPmm && !needsExtended.invalidData,
        "a supported extended current frame selects the real 0003 writer");
    Check(SaveSceneCopy(app.get(), extended.c_str()), "write the same animated models through the actual 0003 project writer");
    app->CurrentFrame() = 0;
    const auto extendedBytes = Bytes(extended); std::uint64_t extendedBoundary = 0;
    pmm_extension::Document extendedDocument;
    const int extendedDescriptor = _wopen(extended.c_str(), _O_RDONLY | _O_BINARY);
    Check(extendedDescriptor >= 0 &&
        pmm_extension::Read(extendedDescriptor, extendedDocument, extendedBoundary) == pmm_extension::ReadResult::Loaded,
        "read the genuine 0003 standard-body boundary and MMS trailer"); _close(extendedDescriptor);
    Check(extendedBytes.size() >= 30 && !std::memcmp(extendedBytes.data(), "Polygon Movie maker 0003", 24) &&
        extendedBoundary <= extendedBytes.size(), "the extended fixture really uses the 0003 standard body");
    WriteBytes(extendedLegacy, extendedBytes, static_cast<std::size_t>(extendedBoundary));
    auto rejectedBytes = originalBytes;
    std::memcpy(rejectedBytes.data() + rejectedBytes.size() - pmm_extension::kTrailerSize, "MDSXPMM2", 8);
    WriteBytes(rejected, rejectedBytes, rejectedBytes.size());
    EarlyOpenEnterChecks(*app, directory, motion);
    struct Case { const char* name; std::vector<std::wstring> bones, morphs; unsigned groups = 1; bool prompt = true, adapted = true; };
    const Case cases[]{
        {"unchanged", originalBones, originalMorphs, 1, false, false},
        {"reorder-bones", {L"Leg", L"RootBone", L"Arm"}, originalMorphs},
        {"add-bone", {L"New", L"RootBone", L"Arm", L"Leg"}, originalMorphs},
        {"remove-bone", {L"RootBone", L"Leg"}, originalMorphs},
        {"rename-bone", {L"RootBone", L"Renamed", L"Leg"}, originalMorphs},
        {"reorder-morphs", originalBones, {L"Smile", L"Face", L"Blink"}},
        {"add-morph", originalBones, {L"New", L"Face", L"Blink", L"Smile"}},
        {"remove-morph", originalBones, {L"Face", L"Smile"}},
        {"rename-morph", originalBones, {L"Face", L"Renamed", L"Smile"}},
        {"remove-all-morphs", originalBones, {}},
        // The loader reserves at least two group rows. 1->3 PMX frames
        // changes actual capacity (2->3); 1->2 would not test that mismatch.
        {"change-display-groups", originalBones, originalMorphs, 3, false, true},
    };
    struct Source { fs::path path; bool extension; const char* label; };
    const Source sources[]{{pmm,true,"MMS-0002"},{legacy,false,"standard-0002"},
        {extended,true,"MMS-0003"},{extendedLegacy,false,"standard-0003"}};
    for (const auto& variant : sources) for (const auto& test : cases) {
        const auto& source = variant.path;
        activeCase = test.name; std::printf("CASE %ls / %s\n", source.filename().c_str(), test.name);
        ResetAppState(app.get()); app->EnglishUI() = 1; WritePmx(pmx, test.bones, test.morphs, test.groups);
        Load(*app, source, test.prompt);
        Check(app->ModelSlot(0) && app->ModelSlot(1), "adaptation retains both requested model slots");
        VerifyNames(*mdl::Mdl(app->ModelSlot(0)), test.bones, test.morphs);
        VerifyMotion(*mdl::Mdl(app->ModelSlot(0)), motion[0]); VerifyMotion(*mdl::Mdl(app->ModelSlot(1)), motion[1]);
        const bool extension = variant.extension;
        Check(text_encoding::BoneName(mdl::Mdl(app->ModelSlot(1))->boneTable[1], true) ==
            (extension ? kSavedEnglishBone : originalBones[1]), "the unrelated model restores its own Unicode dictionary only from a supported MMS record");
        VerifyComments(*mdl::Mdl(app->ModelSlot(0)), extension); VerifyComments(*mdl::Mdl(app->ModelSlot(1)), extension);
        if (!std::strcmp(test.name, "change-display-groups")) {
            VerifyChangedGroups(*mdl::Mdl(app->ModelSlot(0)));
            Check(adaptations == 0, "groups-only changes require no bone/morph mismatch dialog");
        }
        const std::array<Identity, 2> current{CaptureIdentity(*app, 0), CaptureIdentity(*app, 1)};
        if (extension) {
            RestoredIdentity(current[0], originalIdentity[0], test.adapted);
            RestoredIdentity(current[1], originalIdentity[1], false);
        } else for (unsigned slot = 0; slot < 2; ++slot) {
            Check(!Same(current[slot].object.id, originalIdentity[slot].object.id), "a standard-only project never imports a stripped object identity");
            NoOldElements(current[slot], originalIdentity[slot]);
        }
        const auto roundtrip = directory / (source.stem().wstring() + L"-" + std::wstring(test.name, test.name + std::strlen(test.name)) + L".pmm");
        Check(SaveSceneCopy(app.get(), roundtrip.c_str()), "save the actual adapted workspace as a new MMS project"); NoHistory(*app);
        Load(*app, roundtrip, false);
        VerifyNames(*mdl::Mdl(app->ModelSlot(0)), test.bones, test.morphs);
        VerifyMotion(*mdl::Mdl(app->ModelSlot(0)), motion[0]); VerifyMotion(*mdl::Mdl(app->ModelSlot(1)), motion[1]);
        Check(text_encoding::BoneName(mdl::Mdl(app->ModelSlot(1))->boneTable[1], true) ==
            (extension ? kSavedEnglishBone : originalBones[1]), "the unrelated model's dictionary sentinel survives save and reopen");
        if (!std::strcmp(test.name, "change-display-groups")) VerifyChangedGroups(*mdl::Mdl(app->ModelSlot(0)));
        VerifyComments(*mdl::Mdl(app->ModelSlot(0)), extension); VerifyComments(*mdl::Mdl(app->ModelSlot(1)), extension);
        RestoredIdentity(CaptureIdentity(*app, 0), current[0], false);
        RestoredIdentity(CaptureIdentity(*app, 1), current[1], false);
        Check(Bytes(pmm) == originalBytes && Bytes(legacy) == std::vector<char>(originalBytes.begin(), originalBytes.begin() + static_cast<std::size_t>(boundary)),
            "loading and saving elsewhere leave both source PMM variants byte-exact");
        Check(Bytes(extended) == extendedBytes && Bytes(extendedLegacy) ==
            std::vector<char>(extendedBytes.begin(), extendedBytes.begin() + static_cast<std::size_t>(extendedBoundary)),
            "0003 loading and saving elsewhere leave the extended source PMM variants byte-exact");
        std::printf("PASS real workspace %s / %s and save/reopen\n", variant.label, test.name);
    }
    activeCase = "rejected-MDS-marker";
    ResetAppState(app.get()); app->EnglishUI() = 1; WritePmx(pmx, originalBones, originalMorphs);
    pmm_extension::Document ignored; std::uint64_t unchangedBoundary = 0x12345678;
    const int old = _wopen(rejected.c_str(), _O_RDONLY | _O_BINARY); Check(old >= 0, "open the explicitly rejected old-marker fixture");
    Check(pmm_extension::Read(old, ignored, unchangedBoundary) == pmm_extension::ReadResult::Absent && ignored.records.empty() &&
        unchangedBoundary == 0x12345678, "the old MDS marker has no supported extension semantics"); _close(old);
    Load(*app, rejected, false);
    VerifyNames(*mdl::Mdl(app->ModelSlot(0)), originalBones, originalMorphs);
    Check(text_encoding::BoneName(mdl::Mdl(app->ModelSlot(1))->boneTable[1], true) == originalBones[1],
        "the old-marker dictionary never restores its saved English-name sentinel");
    for (unsigned slot = 0; slot < 2; ++slot) {
        VerifyMotion(*mdl::Mdl(app->ModelSlot(slot)), motion[slot]); VerifyComments(*mdl::Mdl(app->ModelSlot(slot)), false);
        const auto current = CaptureIdentity(*app, slot);
        Check(!Same(current.object.id, originalIdentity[slot].object.id), "the old marker never restores persisted object IDs");
        NoOldElements(current, originalIdentity[slot]);
    }
    Check(Bytes(pmm) == originalBytes && Bytes(rejected) == rejectedBytes, "the accepted and rejected-marker inputs are never rewritten");
    activeCase = "shutdown"; NoHistory(*app); ShutdownCleanup(app.get()); activeApp = g_Block = nullptr; app.reset();
    std::printf("PASS real PMM model adaptation host: %u checks; 44 positive cases, one old-marker negative case\n", checks);
    return 0;
}
#include "pmm_expression_roundtrip_checks.inc"
int wmain(int argc,wchar_t** argv) {
    if(argc==3 && std::wstring(argv[1])==L"--selection")return pmm_selection_checks::Run(std::filesystem::absolute(argv[2]));
    if (argc == 3 && std::wcscmp(argv[1], L"--expression-pmm") == 0)
        return pmm_expression_checks::Run(argv[2]);
    return RunExistingPmmCases(argc,argv);
}
#endif
