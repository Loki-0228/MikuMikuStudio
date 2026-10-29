// MAINTENANCE: regression-keep; PMM model adaptation and key selection save/reopen.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/build_profile.hpp"

namespace mikumikustudio {
void MouseInteractionBegin(MMDApp*);
void PumpEnterRegisterFrame(MMDApp*, HWND, bool);
}

using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
unsigned adaptations = 0, errors = 0;
LRESULT CALLBACK DialogHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if (!(create->lpcs->style & WS_CHILD)) {
            create->lpcs->x = -32000; create->lpcs->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) {
        HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t type[32]{}; GetClassNameW(dialog, type, 32);
        if (std::wcscmp(type, L"#32770") == 0) {
            if (GetPropW(dialog, L"AdaptationTestSeen")) return 1;
            SetPropW(dialog, L"AdaptationTestSeen", reinterpret_cast<HANDLE>(1));
            if (GetDlgItem(dialog, 687)) {
                ++adaptations;
                PostMessageW(dialog, WM_COMMAND, 687, 0);
            } else {
                ++errors;
                EnumChildWindows(dialog, [](HWND child, LPARAM)->BOOL {
                    wchar_t text[1024]{}; GetWindowTextW(child, text, 1024);
                    if (*text) std::fwprintf(stderr, L"Unexpected dialog: %ls\n", text);
                    return TRUE;
                }, 0);
                std::exit(3);
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
template<class T> void Put(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof value);
}
void Text(std::ofstream& file, const std::wstring& value) {
    Put(file, std::int32_t(value.size() * sizeof(wchar_t)));
    file.write(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
}
void WritePmx(const std::filesystem::path& path,
              const std::vector<std::wstring>& bones,
              const std::vector<std::wstring>& morphs, unsigned groups = 1) {
    std::ofstream file(path, std::ios::binary);
    file.write("PMX ", 4); Put(file, 2.0f);
    const unsigned char header[] = {0, 0, 1, 1, 1, 1, 1, 1};
    Put(file, static_cast<unsigned char>(sizeof header));
    file.write(reinterpret_cast<const char*>(header), sizeof header);
    Text(file, L"Adaptation"); Text(file, L"Adaptation"); Text(file, L""); Text(file, L"");
    for (int i = 0; i < 4; ++i) Put(file, std::int32_t(0)); // vertices, indices, textures, materials
    Put(file, std::int32_t(bones.size()));
    for (const auto& name : bones) {
        Text(file, name); Text(file, name);
        Put(file, 0.0f); Put(file, 0.0f); Put(file, 0.0f);
        Put(file, std::int8_t(-1)); Put(file, std::int32_t(0)); Put(file, std::uint16_t(0));
        Put(file, 0.0f); Put(file, 1.0f); Put(file, 0.0f);
    }
    Put(file, std::int32_t(morphs.size()));
    for (const auto& name : morphs) {
        Text(file, name); Text(file, name);
        Put(file, std::uint8_t(1)); Put(file, std::uint8_t(1)); // brow panel, vertex morph
        Put(file, std::int32_t(0));
    }
    Put(file, std::int32_t(groups));
    for (unsigned group = 0; group < groups; ++group) {
        Text(file, L"Group" + std::to_wstring(group)); Text(file, L"Group"); Put(file, std::uint8_t(0));
        Put(file, std::int32_t(bones.size()));
        for (std::size_t i = 0; i < bones.size(); ++i) {
            Put(file, std::uint8_t(0)); Put(file, std::int8_t(i));
        }
    }
    Put(file, std::int32_t(0)); Put(file, std::int32_t(0)); // rigid bodies, joints
    Check(bool(file), "write isolated PMX fixture");
}
struct ExpectedTrack { float value; unsigned char selected; };
using Values = std::map<std::string, ExpectedTrack>;
void Seed(mdl::ModelRecord& model, Values& bones, Values& morphs) {
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        const float value = 10.0f + i;
        const auto selected = static_cast<unsigned char>(i % 2);
        bones.emplace(model.boneTable[i].name, ExpectedTrack{value, selected});
        auto& head = model.boneKeys[i]; head.allocated = selected; head.position[0] = value;
        const auto first = model.boneCount + 2 * i;
        head.next = first;
        for (unsigned n = 0; n < 2; ++n) {
            auto& key = model.boneKeys[first + n]; key = head;
            key.frame = 10 * (n + 1); key.position[0] = value + n + 1;
            key.allocated = n == 0 ? !selected : selected;
            key.previous = n == 0 ? i : first;
            key.next = n == 0 ? first + 1 : 0;
        }
    }
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        const float value = 0.1f * (i + 1);
        const auto selected = static_cast<unsigned char>(i % 2);
        morphs.emplace(model.morphs[i].name, ExpectedTrack{value, selected});
        auto& head = model.morphKeys[i]; head.allocated = selected; head.value = value;
        const auto first = model.morphCount + 2 * i;
        head.next = first;
        for (unsigned n = 0; n < 2; ++n) {
            auto& key = model.morphKeys[first + n]; key = head;
            key.frame = 10 * (n + 1); key.value = value + 0.01f * (n + 1);
            key.allocated = n == 0 ? !selected : selected;
            key.previous = n == 0 ? i : first;
            key.next = n == 0 ? first + 1 : 0;
        }
    }
    // Use the registrar so each display key retains its own IK/parent payload.
    for (unsigned n = 0; n < 3; ++n)
        Check(RegisterDisplayKeyFromRecord(reinterpret_cast<unsigned char*>(&model),
                  10 * n, 1, 0, nullptr, 0, nullptr, 0), "seed real display key track");
    auto display = std::uint32_t(0);
    for (unsigned n = 0; n < 3; ++n) {
        model.displayKeys[display].allocated = n != 1;
        display = model.displayKeys[display].next;
    }
}
template<class Key, class Value> void CheckTrack(const Key* keys, std::uint32_t head,
                                               ExpectedTrack expected, float increment, Value value) {
    if (keys[head].frame != 0 || value(keys[head]) != expected.value)
        std::fprintf(stderr, "track head=%u stride=%zu frame=%u value=%g expected=%g\n",
                     head, sizeof(Key), keys[head].frame, double(value(keys[head])), double(expected.value));
    Check(keys[head].frame == 0 && value(keys[head]) == expected.value,
          "mapped frame-zero value survives");
    Check(keys[head].allocated == expected.selected, "mapped frame-zero selection survives");
    auto previous = head;
    for (unsigned n = 0; n < 2; ++n) {
        const auto next = keys[previous].next;
        Check(next != 0 && keys[next].previous == previous && keys[next].frame == 10 * (n + 1),
              "mapped sparse key links and frames survive");
        Check(value(keys[next]) == expected.value + increment * (n + 1), "mapped sparse key values survive");
        Check(keys[next].allocated == (n == 0 ? !expected.selected : expected.selected),
              "mapped selected and unselected sparse keys survive");
        previous = next;
    }
    Check(keys[previous].next == 0, "mapped track ends after both sparse keys");
}
void Verify(const mdl::ModelRecord& model, const Values& bones, const Values& morphs) {
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        auto found = bones.find(model.boneTable[i].name);
        if (found == bones.end()) {
            Check(model.boneKeys[i].next == 0 && model.boneKeys[i].position[0] == 0.0f &&
                  model.boneKeys[i].rotation[3] == 1.0f, "new bone retains default pose and empty track");
        } else CheckTrack(model.boneKeys, i, found->second, 1.0f, [](const auto& key) { return key.position[0]; });
    }
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        auto found = morphs.find(model.morphs[i].name);
        if (found == morphs.end()) {
            Check(model.morphKeys[i].next == 0 && model.morphKeys[i].value == 0.0f,
                  "new morph retains zero weight and empty track");
        } else CheckTrack(model.morphKeys, i, found->second, 0.01f, [](const auto& key) { return key.value; });
    }
    CheckTrack(model.displayKeys, 0, {1.0f, 1}, 0.0f, [](const auto& key) { return key.visible; });
}
void LoadForCase(MMDApp* app, const wchar_t* path, bool holdEnter,
                 const Values& bones, const Values& morphs) {
    if (!holdEnter) { LoadSceneFile(path); return; }
    BYTE savedKeyboard[256]{};
    Check(GetKeyboardState(savedKeyboard) != FALSE, "save thread keyboard state");
    const auto setEnter = [](bool held) {
        BYTE keyboard[256]{};
        keyboard[VK_RETURN] = held ? 0x80 : 0;
        Check(SetKeyboardState(keyboard) != FALSE, "set only the test thread Enter state");
    };
    // Recent-project and file-drop entry points can load before the frame
    // pump sees the Enter press. Keep that same press held across the load.
    app->state.enterKeyState = 0;
    setEnter(true);
    LoadSceneFile(path);
    Check(app->SelectedModel() != nullptr && app->CameraMode() == 0,
          "held Enter fixture reopens in model editing mode");
    const auto* selected = mdl::Mdl(app->SelectedModel());
    const auto undo = selected->undoState[0], redo = selected->undoState[1];
    const auto checkSelection = [&] {
        for (int slot = 0; slot < 2; ++slot) {
            Check(app->ModelSlot(slot) != nullptr, "held Enter fixture retains both models");
            Verify(*mdl::Mdl(app->ModelSlot(slot)), bones, morphs);
        }
        Check(selected->undoState[0] == undo && selected->undoState[1] == redo,
              "project-opening Enter does not create an extra undo entry");
    };
    checkSelection();
    for (unsigned tick = 0; tick < 5; ++tick) {
        setEnter(tick < 3);
        MouseInteractionBegin(app);
        PumpEnterRegisterFrame(app, app->MainWindow(), true);
        checkSelection();
    }
    Check(app->state.enterKeyState == 0, "releasing project-opening Enter returns the poll to idle");
    Check(SetKeyboardState(savedKeyboard) != FALSE, "restore thread keyboard state");
    std::puts("PASS held Enter after real PMM load preserves selections and undo");
}
void VerifyNames(const mdl::ModelRecord& model, const std::vector<std::wstring>& bones,
                 const std::vector<std::wstring>& morphs) {
    Check(model.boneCount == bones.size() && model.morphCount == morphs.size(), "edited model counts survive");
    for (std::size_t i = 0; i < bones.size(); ++i)
        Check(text_encoding::BoneName(model.boneTable[i], false) == bones[i], "edited bone names and order survive");
    for (std::size_t i = 0; i < morphs.size(); ++i)
        Check(text_encoding::MorphName(model.morphs[i], false) == morphs[i], "edited morph names and order survive");
}
std::vector<char> Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto directory = std::filesystem::absolute(argv[1]);
    Check(std::filesystem::create_directories(directory), "use a fresh fixture directory");
    const auto preference = directory / L"antialias.ini";
    SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG", preference.c_str());
    SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX", L"0");
    WritePrivateProfileStringW(L"Antialias", L"Enabled", L"0", preference.c_str());
    const auto hook = SetWindowsHookExW(WH_CBT, DialogHook, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "install adaptation dialog driver");
    auto* app = new MMDApp(); g_Block = app;
    app->state = MMDAppState{}; app->InitDefaults(); app->EnglishUI() = 1;
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production app");
    const std::vector<std::wstring> sourceBones{L"RootBone", L"Arm", L"Leg"};
    const std::vector<std::wstring> sourceMorphs{L"Face", L"Blink", L"Smile"};
    const auto pmx = directory / L"model.pmx", pmm = directory / L"source.pmm";
    WritePmx(pmx, sourceBones, sourceMorphs);
    LoadModelFile(app, pmx.c_str(), false);
    Check(app->ModelSlot(0) != nullptr, "load original PMX");
    Values bones, morphs; Seed(*mdl::Mdl(app->ModelSlot(0)), bones, morphs);
    const auto unchangedPmx = directory / L"unchanged.pmx";
    WritePmx(unchangedPmx, sourceBones, sourceMorphs);
    LoadModelFile(app, unchangedPmx.c_str(), false);
    Check(app->ModelSlot(1) != nullptr, "load unaffected second model");
    Seed(*mdl::Mdl(app->ModelSlot(1)), bones, morphs);
    Check(extended_names::Assign(mdl::Mdl(app->ModelSlot(1))->boneTable[1].enText, L"Saved English name"),
          "give unaffected model metadata an observable saved value");
    Check(SaveSceneCopy(app, pmm.c_str()), "save original PMM with animated tracks");
    const auto originalBytes = Bytes(pmm);
    std::vector<std::filesystem::path> sources{pmm};
#if !MMD_ORIGINAL_PROFILE
    const auto legacy = directory / L"legacy.pmm", oldTrailer = directory / L"old-trailer.pmm";
    pmm_extension::Document document; std::uint64_t standardSize = 0;
    int fd = _wopen(pmm.c_str(), _O_RDONLY | _O_BINARY);
    Check(fd >= 0 && pmm_extension::Read(fd, document, standardSize) == pmm_extension::ReadResult::Loaded,
          "read fixture extension boundary");
    _close(fd);
    { std::ofstream file(legacy, std::ios::binary); file.write(originalBytes.data(), standardSize); }
    { auto bytes = originalBytes; std::memcpy(bytes.data() + bytes.size() - 32, "MDSXPMM2", 8);
      std::ofstream file(oldTrailer, std::ios::binary); file.write(bytes.data(), bytes.size()); }
    sources.insert(sources.end(), {legacy, oldTrailer});
#endif
    struct Case { const char* name; std::vector<std::wstring> bones, morphs; unsigned groups = 1; bool prompt = true; };
    const Case cases[] = {
        {"unchanged", sourceBones, sourceMorphs, 1, false},
        {"reorder-bones", {L"Leg", L"RootBone", L"Arm"}, sourceMorphs},
        {"add-bone", {L"New", L"RootBone", L"Arm", L"Leg"}, sourceMorphs},
        {"remove-bone", {L"RootBone", L"Leg"}, sourceMorphs},
        {"rename-bone", {L"RootBone", L"Renamed", L"Leg"}, sourceMorphs},
        {"reorder-morphs", sourceBones, {L"Smile", L"Face", L"Blink"}},
        {"add-morph", sourceBones, {L"New", L"Face", L"Blink", L"Smile"}},
        {"remove-morph", sourceBones, {L"Face", L"Smile"}},
        {"rename-morph", sourceBones, {L"Face", L"Renamed", L"Smile"}},
        {"remove-all-morphs", sourceBones, {}},
        {"change-display-groups", sourceBones, sourceMorphs, 2, false},
    };
    for (const auto& source : sources) {
    for (const auto& test : cases) {
        std::printf("CASE %ls / %s\n", source.filename().c_str(), test.name);
        ResetAppState(app); WritePmx(pmx, test.bones, test.morphs, test.groups);
        adaptations = errors = 0;
        const bool holdEnter = std::strcmp(test.name, "unchanged") == 0;
        LoadForCase(app, source.c_str(), holdEnter, bones, morphs);
        std::printf("dialogs: adaptations=%u errors=%u\n", adaptations, errors);
        Check(errors == 0 && app->ModelSlot(0) != nullptr, "PMM adaptation completes without errors");
        Check(adaptations == (test.prompt ? 1u : 0u),
              "expected structure mismatch dialog is accepted once");
        VerifyNames(*mdl::Mdl(app->ModelSlot(0)), test.bones, test.morphs);
        Verify(*mdl::Mdl(app->ModelSlot(0)), bones, morphs);
        Check(app->ModelSlot(1) != nullptr, "unaffected second model is loaded");
        Verify(*mdl::Mdl(app->ModelSlot(1)), bones, morphs);
#if !MMD_ORIGINAL_PROFILE
        if (source != legacy)
            Check(text_encoding::BoneName(mdl::Mdl(app->ModelSlot(1))->boneTable[1], true) == L"Saved English name",
                  "unaffected model still restores its own Unicode dictionary");
#endif
        const auto roundtrip = directory / (source.stem().wstring() + L"-" +
            std::wstring(test.name, test.name + std::strlen(test.name)) + L".pmm");
        Check(SaveSceneCopy(app, roundtrip.c_str()), "save adapted PMM");
        adaptations = errors = 0;
        LoadForCase(app, roundtrip.c_str(), holdEnter, bones, morphs);
        Check(errors == 0 && adaptations == 0 && app->ModelSlot(0) != nullptr,
              "adapted PMM reopens without another mismatch");
        Verify(*mdl::Mdl(app->ModelSlot(0)), bones, morphs);
        VerifyNames(*mdl::Mdl(app->ModelSlot(0)), test.bones, test.morphs);
        Check(app->ModelSlot(1) != nullptr, "unaffected model survives second save/reopen");
        Verify(*mdl::Mdl(app->ModelSlot(1)), bones, morphs);
        std::printf("PASS %s and save/reopen with key selections\n", test.name);
    }
    }
    Check(Bytes(pmm) == originalBytes, "loading and adaptation leave the source PMM unchanged");
    ShutdownCleanup(app); delete app; g_Block = nullptr;
    UnhookWindowsHookEx(hook);
    return 0;
}
