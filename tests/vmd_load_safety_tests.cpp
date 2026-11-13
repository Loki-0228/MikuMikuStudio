// MAINTENANCE: regression-keep; VMD parsing, import selection, and modal input isolation.
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/animation_curve_motion.hpp"
#include "mikumikustudio/build_profile.hpp"
// Exercise the real LoadVmdMotion entry, key registrars, and extension loaders.
// The dialog-enter case uses an inactive private desktop for real modal dialogs;
// the other cases create no windows. No case creates a D3D device.
// vmd_load_safety_tests.exe --case overflow|short|counts|valid|dialog-enter|sampler-focus|tails|camera|ik|twist|force-morph|duplicate-bones|duplicate-morphs
// vmd_load_safety_tests.exe --benchmark [warmup iterations]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iterator>
#include <memory>
#include <share.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/force_paste.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <limits>

namespace mikumikustudio {
int LoadVmdMotion(MMDApp*, const char*);
void MouseInteractionBegin(MMDApp*);
void PollKeyboardStates(MMDApp*);
void PumpEnterRegisterFrame(MMDApp*, HWND, bool);
void CmdControl500(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}
namespace {
using namespace mikumikustudio;
using Bytes = std::vector<std::uint8_t>;
void Check(bool condition, const char* description) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
template<class T> void Put(Bytes& bytes, const T& value) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}
void Text(Bytes& bytes, const char* text, std::size_t length) {
    const auto start = bytes.size(); bytes.resize(start + length);
    std::memcpy(bytes.data() + start, text, (std::min)(length, std::strlen(text)));
}
void U32(Bytes& bytes, std::uint32_t value) { Put(bytes, value); }
void SetU32(Bytes& bytes, std::size_t offset, std::uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); }

struct Motion {
    Bytes bytes;
    std::vector<std::size_t> counts, ends;
    std::size_t ikEntries = 0;
    explicit Motion(int version = 2, bool camera = false, bool empty = false) {
        Text(bytes, version == 1 ? "Vocaloid Motion Data file" : "Vocaloid Motion Data 0002", 30);
        Text(bytes, camera ? "\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe" : "sample", version == 1 ? 10 : 20);
        auto count = [&]() { counts.push_back(bytes.size()); U32(bytes, empty ? 0 : 1); };
        count();
        if (!empty) {
            Text(bytes, "bone", 15); U32(bytes, 0);
            Put(bytes, 7.f); Put(bytes, 8.f); Put(bytes, 9.f);
            for (float value : {0.f, 0.f, 0.f, 1.f}) Put(bytes, value);
            bytes.resize(bytes.size() + 64);
        }
        ends.push_back(bytes.size());
        count();
        if (!empty) { Text(bytes, "face", 15); U32(bytes, 0); Put(bytes, .75f); }
        ends.push_back(bytes.size());
        count();
        if (!empty) {
            U32(bytes, 0); Put(bytes, -25.f);
            for (float value : {1.f, 2.f, 3.f, 0.f, 0.f, 0.f}) Put(bytes, value);
            bytes.resize(bytes.size() + (version == 1 ? 4 : 24), 20);
            if (version == 2) { U32(bytes, 50); bytes.push_back(0); }
        }
        ends.push_back(bytes.size());
        count();
        if (!empty) {
            U32(bytes, 0);
            for (float value : {.2f, .3f, .4f, 1.f, -1.f, 1.f}) Put(bytes, value);
        }
        ends.push_back(bytes.size());
        if (version == 2) {
            count();
            if (!empty) { U32(bytes, 0); bytes.push_back(2); Put(bytes, .04f); }
            ends.push_back(bytes.size());
        }
        count();
        if (!empty) {
            U32(bytes, 0); bytes.push_back(0);
            ikEntries = bytes.size(); U32(bytes, 1);
            Text(bytes, "bone", 20); bytes.push_back(0);
        }
        ends.push_back(bytes.size());
    }
};

// Bounded, exclusive temporary-file allocation for the VMD regression fixtures.
// The previous helper made a single GetTempFileNameA(TEMP) call with no
// fallback, so a bad TEMP environment failed every fixture. Each candidate here
// is created with CreateFileA(CREATE_NEW): an existing path is a real, captured
// collision and is never opened or truncated. Candidate one is the process temp
// directory; candidate two is the test executable's own directory, which is
// independent of the current directory and of TEMP/TMP.
enum class TempFileSecondCandidate {
    executableDirectory,
    executableFileAsDirectory,  // probe only: use the executable file as a directory
};

struct TempFile {
    char path[MAX_PATH]{};
    const char* candidate = nullptr;
    bool owned = false;
    TempFile() : TempFile(TempFileSecondCandidate::executableDirectory) {}
    explicit TempFile(TempFileSecondCandidate secondCandidate) {
        if (CreateTempCandidate()) return;
        if (CreateExecutableCandidate(secondCandidate)) return;
        Check(false, "create temporary motion: both creation candidates failed");
    }
    ~TempFile() {
        // Delete only the file this instance created; a candidate that failed
        // before a successful CREATE_NEW never owns a path.
        if (owned) DeleteFileA(path);
    }
    // Allocation and Write are deliberately separate phases: once a CREATE_NEW
    // has produced this path, a later open/write failure keeps the existing hard
    // Check and never retries the other allocation candidate.
    void Write(const Bytes& bytes) {
        int fd = -1;
        Check(_sopen_s(&fd, path, _O_BINARY | _O_WRONLY | _O_TRUNC, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0, "open temporary motion");
        Check(bytes.empty() || _write(fd, bytes.data(), static_cast<unsigned>(bytes.size())) == static_cast<int>(bytes.size()), "write temporary motion");
        _close(fd);
    }

private:
    // A locally detected length/path problem has no Win32 error to report; only
    // a real API failure carries the GetLastError value captured at the call.
    static void ReportCandidateFailure(const char* candidateName, const char* stage,
                                       const std::string& path, bool apiFailure, DWORD error) {
        const char* shownPath = path.empty() ? "<unavailable>" : path.c_str();
        if (apiFailure) {
            std::fprintf(stderr,
                         "[TempFile] candidate=%s stage=%s path=%s result=api-failure win32=%lu\n",
                         candidateName, stage, shownPath, static_cast<unsigned long>(error));
        } else {
            std::fprintf(stderr,
                         "[TempFile] candidate=%s stage=%s path=%s result=local-path-validation (no Win32 error captured)\n",
                         candidateName, stage, shownPath);
        }
    }
    // One candidate directory, named in every diagnostic. Collisions are the
    // only retryable CreateFileA result; any other failure ends the candidate
    // immediately so the next candidate can be tried.
    bool TryCandidate(const char* candidateName, const char* directory) {
        char fullPath[MAX_PATH]{};
        DWORD lastError = 0;
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            char name[64]{};
            const int nameLength = std::snprintf(name, sizeof(name), "mms-vmd-%lu-%llu-%u.tmp",
                                                 static_cast<unsigned long>(GetCurrentProcessId()),
                                                 static_cast<unsigned long long>(GetTickCount64()),
                                                 attempt);
            if (nameLength < 0 || static_cast<std::size_t>(nameLength) >= sizeof(name)) {
                ReportCandidateFailure(candidateName, "snprintf-filename",
                                       std::string(directory) + "<unique-name>", false, 0);
                return false;
            }
            const int fullLength = std::snprintf(fullPath, sizeof(fullPath), "%s%s", directory, name);
            if (fullLength < 0 || static_cast<std::size_t>(fullLength) >= sizeof(fullPath)) {
                ReportCandidateFailure(candidateName, "snprintf-complete-path",
                                       std::string(directory) + name, false, 0);
                return false;
            }
            const HANDLE handle = CreateFileA(fullPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                lastError = GetLastError();
                if (lastError == ERROR_FILE_EXISTS || lastError == ERROR_ALREADY_EXISTS) continue;
                ReportCandidateFailure(candidateName, "CreateFileA", fullPath, true, lastError);
                return false;
            }
            if (!CloseHandle(handle)) {
                const DWORD closeError = GetLastError();
                ReportCandidateFailure(candidateName, "CloseHandle", fullPath, true, closeError);
                Check(false, "close exclusively created temporary motion");
            }
            std::memcpy(path, fullPath, sizeof(fullPath));
            candidate = candidateName;
            owned = true;
            return true;
        }
        ReportCandidateFailure(candidateName, "CreateFileA-collision-limit", fullPath, true, lastError);
        return false;
    }
    bool CreateTempCandidate() {
        char directory[MAX_PATH]{};
        const DWORD length = GetTempPathA(MAX_PATH, directory);
        if (length == 0) {
            const DWORD error = GetLastError();
            ReportCandidateFailure("temp", "GetTempPathA", std::string(), true, error);
            return false;
        }
        if (length >= MAX_PATH) {
            ReportCandidateFailure("temp", "GetTempPathA-length", std::string(), false, 0);
            return false;
        }
        std::string base(directory, length);
        if (base.back() != '\\') base.push_back('\\');
        return TryCandidate("temp", base.c_str());
    }
    bool CreateExecutableCandidate(TempFileSecondCandidate secondCandidate) {
        char modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        const DWORD moduleError = GetLastError();
        if (length == 0) {
            ReportCandidateFailure("executable", "GetModuleFileNameA", std::string(), true, moduleError);
            return false;
        }
        if (length >= MAX_PATH) {
            // Bounded read of the whole array: a truncated module path must not
            // be treated as NUL-terminated. The captured Win32 value is carried
            // as-is for this API truncation diagnostic.
            ReportCandidateFailure("executable", "GetModuleFileNameA-length",
                                   std::string(modulePath, sizeof(modulePath)), true, moduleError);
            return false;
        }
        if (secondCandidate == TempFileSecondCandidate::executableFileAsDirectory) {
            // Probe-only override: the second candidate is the full executable
            // FILE path plus a separator, so CreateFileA fails for a real Win32
            // reason without touching directory contents or ACLs.
            const std::string directory = std::string(modulePath) + '\\';
            return TryCandidate("executable-file-override", directory.c_str());
        }
        char* const delimiter = std::strrchr(modulePath, '\\');
        if (delimiter == nullptr) {
            ReportCandidateFailure("executable", "GetModuleFileNameA-directory-delimiter", modulePath, false, 0);
            return false;
        }
        delimiter[1] = '\0';
        return TryCandidate("executable", modulePath);
    }
};

// --tempfile-probe: drive the same allocator the suite uses, with no fixture,
// dialog, or Unicode path work. Print the selected candidate and path, exercise
// the existing Write with a known payload, verify the readback, then verify scope
// destruction removed the owned file.
int RunTempFileProbe() {
    char created[MAX_PATH]{};
    {
        TempFile file;
        std::memcpy(created, file.path, sizeof(created));
        std::printf("tempfile-probe: candidate=%s path=%s\n", file.candidate, file.path);
        std::fflush(stdout);
        const Bytes payload{'M', 'M', 'S', '-', 'T', 'E', 'M', 'P', 'F', 'I', 'L', 'E', '\n'};
        file.Write(payload);
        std::ifstream input(file.path, std::ios::binary);
        Check(bool(input), "tempfile-probe opens the written temporary motion for readback");
        const Bytes readback((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
        Check(readback == payload, "tempfile-probe readback matches the written payload");
    }
    const DWORD attributes = GetFileAttributesA(created);
    const DWORD attributesError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    Check(attributes == INVALID_FILE_ATTRIBUTES,
          "tempfile-probe scope destruction removes the owned temporary file");
    Check(attributesError == ERROR_FILE_NOT_FOUND || attributesError == ERROR_PATH_NOT_FOUND,
          "tempfile-probe removal is confirmed by a not-found Win32 error");
    std::puts("tempfile-probe: candidate, path, write, readback, and owned cleanup passed");
    return 0;
}

// --tempfile-probe-all-fail: the same creation engine with the first candidate
// pointed by the child environment's real TEMP/TMP at a nonexistent path and the
// second candidate overridden to the executable file path plus a separator. Both
// real CreateFileA calls must fail, and the constructor then exits through Check
// with code 1. An unexpected success reports code 2 after scope destruction
// instead of reporting a pass.
int RunTempFileProbeAllFail() {
    {
        TempFile file(TempFileSecondCandidate::executableFileAsDirectory);
        std::printf("tempfile-probe-all-fail: unexpected success candidate=%s path=%s\n",
                    file.candidate, file.path);
        std::fflush(stdout);
    }
    return 2;
}

struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 3> bones{};
    std::array<mdl::IkChain, 3> ikChains{};
    std::array<mdl::MorphRecord, 2> morphs{};
    std::array<unsigned char, 3> selected{}, physics{};
    std::vector<mdl::BoneKey> boneKeys = std::vector<mdl::BoneKey>(mdl::kBoneKeyCapacity);
    std::vector<mdl::MorphKey> morphKeys = std::vector<mdl::MorphKey>(mdl::kMorphKeyCapacity);
    std::vector<mdl::DisplayKey> displayKeys = std::vector<mdl::DisplayKey>(mdl::kDisplayKeyCapacity);
    std::vector<mdl::CameraKey> cameraKeys = std::vector<mdl::CameraKey>(mdl::kTimelineKeyCapacity);
    std::vector<mdl::LightKey> lightKeys = std::vector<mdl::LightKey>(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadowKeys = std::vector<mdl::SelfShadowKey>(mdl::kTimelineKeyCapacity);
    std::unique_ptr<mdl::AccessoryRecord> accessory;
    std::vector<mdl::AccessoryKey> accessoryKeys;
    MMDApp* previous = g_Block;
    TempFile file;
    explicit Fixture(bool camera = false) {
        app->state = MMDAppState{};
        std::memset(renderer.get(), 0, sizeof(*renderer)); app->Renderer() = renderer.get();
        app->ModelSlot(0) = Raw(); app->SetSelectedModelSlot(0); app->EnglishUI() = 1;
        app->state.optflag[0] = camera ? 1 : 0;
        app->CameraKeys() = cameraKeys.data(); app->LightKeys() = lightKeys.data(); app->ShadowKeys() = shadowKeys.data();
        model->boneCount = 1; model->boneTable = bones.data(); model->boneKeys = boneKeys.data();
        model->morphCount = 2; model->morphs = morphs.data(); model->morphKeys = morphKeys.data();
        model->displayKeys = displayKeys.data(); model->boneSelection = selected.data(); model->bonePhysicsState = physics.data();
        model->loadComplete = 1;
        std::strcpy(model->name, "sample"); std::strcpy(bones[0].name, "bone"); std::strcpy(morphs[1].name, "face");
        bones[0].parent = -1; bones[0].slotIndex = -1; bones[0].rotQuat[3] = 1; bones[0].type = mdl::BoneType::RotateMove;
        boneKeys[0].rotation[3] = 1; boneKeys[0].position[0] = -3;
        boneKeys[0].allocated = boneKeys.back().allocated = 1;
        morphKeys[1].value = .125f; morphKeys[1].allocated = morphKeys.back().allocated = 1;
        displayKeys[0].visible = 1; displayKeys[0].allocated = displayKeys.back().allocated = 1;
        cameraKeys[0].distance = -10; cameraKeys[0].fov = 45; cameraKeys[0].parentModel = -1;
        lightKeys[0].color[0] = .8f; shadowKeys[0].distance = .01f;
        model->undoState[0] = 4; model->undoState[1] = 3;
        auto& slot = model->undoRings[0].slots[5]; slot.operation = 9; slot.dirty = 1;
        slot.bonePose = static_cast<mdl::BonePoseSnapshot*>(std::malloc(sizeof(mdl::BonePoseSnapshot)));
        slot.auxiliaryPose = std::malloc(192);
        Check(slot.bonePose && slot.auxiliaryPose, "allocate sentinel undo");
        std::memset(slot.bonePose, 0x31, sizeof(mdl::BonePoseSnapshot)); std::memset(slot.auxiliaryPose, 0x42, 192);
        g_Block = app.get();
    }
    void EnableIk() {
        model->boneCount = static_cast<std::uint32_t>(bones.size());
        for (std::size_t i = 1; i < bones.size(); ++i) {
            bones[i].parent = -1; bones[i].slotIndex = -1; bones[i].rotQuat[3] = 1;
            bones[i].type = mdl::BoneType::RotateMove; boneKeys[i].rotation[3] = 1;
        }
        std::strcpy(bones[1].name, "second");
        std::memcpy(bones[2].name, "abcdefghijklmnopqrst", 20);
        model->ikChainCount = static_cast<std::uint32_t>(ikChains.size()); model->ikChains = ikChains.data();
        // Deliberately differ from both bone order and serialized entry order.
        ikChains[0].boneIndex = 2; ikChains[1].boneIndex = 0; ikChains[2].boneIndex = 1;
        for (auto& chain : ikChains) { chain.targetBone = 0; chain.enabled = 1; }
        for (auto& key : displayKeys) {
            key.ikStates = static_cast<std::uint8_t*>(std::malloc(ikChains.size()));
            Check(key.ikStates != nullptr, "allocate real per-display IK flags");
            std::memset(key.ikStates, 1, ikChains.size());
        }
    }
    void EnableAccessory() {
        accessory = std::make_unique<mdl::AccessoryRecord>(); accessoryKeys.resize(mdl::kTimelineKeyCapacity);
        std::strcpy(accessory->name, "prop.x"); accessory->parentModel = -1;
        accessoryKeys[0].parentModel = -1; accessoryKeys[0].scale = 1; accessoryKeys[0].opacity = 1;
        app->AccessorySlot(3) = accessory.get(); app->AccessoryKeys(3) = accessoryKeys.data();
    }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    ~Fixture() {
        bone_scale::Forget(Raw()); morph_curve::Forget(Raw());
        for (auto& ring : model->undoRings) for (auto& slot : ring.slots) { mikumikustudio::undo_storage::Free(slot.bonePose); mikumikustudio::undo_storage::Free(slot.auxiliaryPose); }
        for (auto& key : displayKeys) { std::free(key.ikStates); std::free(key.selectorStates); }
        g_Block = previous;
    }
    int Load(const Bytes& bytes) { file.Write(bytes); return LoadVmdMotion(app.get(), file.path); }
    Bytes State() const {
        Bytes bytes;
        auto append = [&](const void* data, std::size_t size) { const auto* p = static_cast<const std::uint8_t*>(data); bytes.insert(bytes.end(), p, p + size); };
        append(model.get(), sizeof(*model)); append(&app->state, sizeof(app->state));
        append(boneKeys.data(), boneKeys.size() * sizeof(boneKeys[0]));
        append(morphKeys.data(), morphKeys.size() * sizeof(morphKeys[0]));
        append(displayKeys.data(), displayKeys.size() * sizeof(displayKeys[0]));
        append(cameraKeys.data(), cameraKeys.size() * sizeof(cameraKeys[0]));
        append(lightKeys.data(), lightKeys.size() * sizeof(lightKeys[0]));
        append(shadowKeys.data(), shadowKeys.size() * sizeof(shadowKeys[0]));
        const auto& slot = model->undoRings[0].slots[5];
        append(slot.bonePose, sizeof(mdl::BonePoseSnapshot)); append(slot.auxiliaryPose, 192);
        return bytes;
    }
    void Rejected(const Bytes& bytes, const Bytes& before) {
        Check(Load(bytes) == 0, "malformed input rejected by production entry");
        Check(State() == before, "rejected input preserves all tracks, marks, undo buffers, and app state");
    }
    void Imported(const Bytes& bytes, bool populated = true) {
        Check(Load(bytes) == static_cast<int>(model->boneCount), "model production load returns completed bone seek");
        Check(model->undoState[0] == 5 && model->undoRings[0].slots[5].operation == 2, "valid input commits undo slot");
        if (populated) {
            Check(boneKeys[0].position[0] == 7 && morphKeys[1].value == .75f, "real registrars import bone and morph values");
            Check(model->undoRings[0].slots[5].dirty == 1, "one distinct overwritten bone snapshot recorded");
        }
    }
};

void StandardModelExchange(const std::filesystem::path& input, const std::filesystem::path& output) {
    Check(input.is_absolute() && output.is_absolute() && !std::filesystem::exists(output),
          "use absolute input and a new output path");
    std::ifstream stream(input, std::ios::binary);
    Check(bool(stream), "open the standard exchange fixture");
    const Bytes motion((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    Fixture fixture;
    std::strcpy(fixture.model->name, "VMD Fixture");
    std::memcpy(fixture.bones[0].name, "\x83\x5a\x83\x93\x83\x5e\x81\x5b", 9);
    std::memcpy(fixture.morphs[1].name, "\x82\xdc\x82\xce\x82\xbd\x82\xab", 9);
    Check(fixture.Load(motion) == 1, "import the standard VMD into a matching model");
    const auto bone = fixture.boneKeys[0].next;
    const auto face = fixture.morphKeys[1].next;
    Check(bone && bone < fixture.boneKeys.size() && fixture.boneKeys[bone].frame == 18 &&
          face && face < fixture.morphKeys.size() && fixture.morphKeys[face].frame == 23,
          "the model has both nonzero frame endpoints");
    for (auto& key : fixture.boneKeys) key.allocated = 0;
    for (auto& key : fixture.morphKeys) key.allocated = 0;
    for (auto& key : fixture.displayKeys) key.allocated = 0;
    fixture.boneKeys[0].allocated = fixture.boneKeys[bone].allocated = 1;
    fixture.morphKeys[1].allocated = fixture.morphKeys[face].allocated = 1;
    SaveVmdFile(output.c_str());
    Check(std::filesystem::is_regular_file(output), "the production writer produced a standard motion file");
    std::puts("PASS standard VMD import and export on two CP932 tracks");
}

// The Unicode-path case keeps one TempFile anchor alive for the whole function
// and puts the test root next to the file that anchor actually created. The
// anchor's selected candidate is therefore the only directory source; this code
// does not make a second TEMP/CWD/executable decision. The printed lines are
// evidence of that source, not a claim that TEMP was bad or the suite passed.
// CreateDirectoryW is the ownership test: only TRUE creates this run's
// directory, while an existing path is a collision that gets a new name and is
// never reused.
void ReportUnicodePathRootFailure(const char* candidate, const std::wstring& parent,
                                  const std::wstring& root, unsigned attempt,
                                  const char* stage, bool apiFailure, DWORD error) {
    const wchar_t* shownParent = parent.empty() ? L"<unavailable>" : parent.c_str();
    const wchar_t* shownRoot = root.empty() ? L"<unavailable>" : root.c_str();
    if (apiFailure) {
        std::fprintf(stderr,
                     "[UnicodePaths] candidate=%s stage=%s attempt=%u parent=%ls root=%ls "
                     "result=api-failure win32=%lu\n",
                     candidate, stage, attempt, shownParent, shownRoot,
                     static_cast<unsigned long>(error));
    } else {
        std::fprintf(stderr,
                     "[UnicodePaths] candidate=%s stage=%s attempt=%u parent=%ls root=%ls "
                     "result=local-path-validation (no Win32 error captured)\n",
                     candidate, stage, attempt, shownParent, shownRoot);
    }
}
std::wstring CreateUnicodePathRoot(const char* candidate, const std::wstring& parent) {
    unsigned lastAttempt = 0;
    DWORD lastCollisionError = 0;
    std::wstring lastRoot;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        lastAttempt = attempt;
        wchar_t leaf[64]{};
        const int leafLength = std::swprintf(
            leaf, std::size(leaf), L"MMS-vmd-path-%lu-%llu-%u",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(GetTickCount64()), attempt);
        if (leafLength < 0 || static_cast<std::size_t>(leafLength) >= std::size(leaf)) {
            ReportUnicodePathRootFailure(candidate, parent, lastRoot, attempt,
                                         "swprintf-root-name", false, 0);
            Check(false, "compose Unicode path root name");
            return std::wstring();
        }
        std::wstring root = parent;
        if (root.empty() || (root.back() != L'\\' && root.back() != L'/')) root.push_back(L'\\');
        root.append(leaf);
        lastRoot = root;
        if (root.size() >= MAX_PATH) {
            ReportUnicodePathRootFailure(candidate, parent, root, attempt,
                                         "root-path-length", false, 0);
            Check(false, "Unicode path root fits CreateDirectoryW");
            return std::wstring();
        }
        if (CreateDirectoryW(root.c_str(), nullptr)) return root;
        const DWORD createError = GetLastError();
        if (createError == ERROR_ALREADY_EXISTS || createError == ERROR_FILE_EXISTS) {
            lastCollisionError = createError;
            continue;
        }
        ReportUnicodePathRootFailure(candidate, parent, root, attempt, "CreateDirectoryW",
                                     true, createError);
        Check(false, "create Unicode path test root");
        return std::wstring();
    }
    ReportUnicodePathRootFailure(candidate, parent, lastRoot, lastAttempt,
                                 "CreateDirectoryW-collision-limit", true, lastCollisionError);
    Check(false, "create Unicode path test root: collision limit");
    return std::wstring();
}
void UnicodePaths(bool camera) {
    namespace fs = std::filesystem;
    const std::wstring previousLocale = _wsetlocale(LC_ALL, nullptr);
    // Construct the shared allocator's anchor before any CRT locale change and
    // keep it alive until this function returns. Its destructor still removes
    // only its own file. The parent of anchor.path is the directory source for
    // the test root; failing to create a directory or fixture below is a hard
    // failure, never a reason to retry another candidate or fall back.
    TempFile anchor;
    // anchor.path was made by the ...A file APIs, so decode it with the file-API
    // code page. The CRT locale changes in the loop below and must not take part
    // in this conversion.
    const UINT fileApiCodePage = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
    wchar_t anchorPath[MAX_PATH]{};
    const int anchorPathLength =
        MultiByteToWideChar(fileApiCodePage, 0, anchor.path, -1, anchorPath, MAX_PATH);
    if (anchorPathLength == 0) {
        const DWORD conversionError = GetLastError();
        std::fprintf(stderr,
                     "[UnicodePaths] candidate=%s stage=MultiByteToWideChar anchor=%s "
                     "code-page=%u result=api-failure win32=%lu\n",
                     anchor.candidate, anchor.path, static_cast<unsigned>(fileApiCodePage),
                     static_cast<unsigned long>(conversionError));
        Check(false, "convert TempFile anchor path with the file API code page");
    } else if (anchorPathLength > MAX_PATH || anchorPath[anchorPathLength - 1] != L'\0') {
        std::fprintf(stderr,
                     "[UnicodePaths] candidate=%s stage=MultiByteToWideChar-length anchor=%s "
                     "code-page=%u result=local-path-validation (no Win32 error captured)\n",
                     anchor.candidate, anchor.path, static_cast<unsigned>(fileApiCodePage));
        Check(false, "TempFile anchor wide path fits its buffer");
    }
    const fs::path anchorWidePath(anchorPath);
    const fs::path anchorParent = anchorWidePath.parent_path();
    Check(!anchorParent.empty(), "TempFile anchor path has a parent directory");
    fs::path rootParent = anchorParent;
    if (!rootParent.is_absolute()) {
        // GetTempPathA can return a relative TEMP value; the filesystem library
        // and Win32 wide APIs keep the descendant names Unicode here.
        wchar_t absoluteParent[MAX_PATH]{};
        const DWORD absoluteLength =
            GetFullPathNameW(rootParent.c_str(), MAX_PATH, absoluteParent, nullptr);
        if (absoluteLength == 0) {
            const DWORD absoluteError = GetLastError();
            ReportUnicodePathRootFailure(anchor.candidate, rootParent.native(), std::wstring(),
                                         0, "GetFullPathNameW", true, absoluteError);
            Check(false, "resolve TempFile anchor parent to an absolute wide path");
        }
        if (absoluteLength >= MAX_PATH) {
            ReportUnicodePathRootFailure(anchor.candidate, rootParent.native(), std::wstring(),
                                         0, "GetFullPathNameW-length", false, 0);
            Check(false, "absolute TempFile anchor parent fits its buffer");
        }
        rootParent = fs::path(std::wstring(absoluteParent, absoluteLength));
    }
    const std::wstring rootWide = CreateUnicodePathRoot(anchor.candidate, rootParent.native());
    const fs::path root(rootWide);
    std::printf("[UnicodePaths] anchor-candidate=%s anchor-path=%ls anchor-parent=%ls "
                "created-root=%ls\n",
                anchor.candidate, anchorPath, rootParent.c_str(), root.c_str());
    std::fflush(stdout);
    unsigned caseNumber = 0;
    for (const wchar_t* locale : {L"japanese", L""}) {
        Check(_wsetlocale(LC_ALL, locale) != nullptr, "set production or system CRT locale");
        for (const wchar_t* folder : {L"中文目录", L"plain", L"混合_髪_무대_😀"}) {
            const auto directory = root / folder;
            Check(fs::create_directory(directory), "create motion directory");
            for (const wchar_t* name : {L"motion.vmd", L"动作与镜头_😀.vmd"}) {
                Fixture fixture(camera);
                Motion motion(2, camera);
                if (!camera) {
                    // Scale data reopens the VMD; it must receive the same
                    // Unicode path as the standard bone and morph reader.
                    bone_scale_io::PrefixFingerprint fingerprint;
                    fingerprint.Append(motion.bytes.data(), motion.bytes.size());
                    bone_scale_io::VmdExtension extension;
                    extension.prefixBytes = fingerprint.bytes();
                    extension.prefixCrc32 = fingerprint.crc32();
                    bone_scale_io::VmdExtension::Entry entry;
                    std::memcpy(entry.name.data(), "bone", 4);
                    entry.scale.v[0] = entry.scale.v[4] = entry.scale.v[8] = 2;
                    extension.entries.push_back(entry);
                    const auto tail = bone_scale_io::EncodeExtension(extension);
                    motion.bytes.insert(motion.bytes.end(), tail.begin(), tail.end());
                    Put(motion.bytes, std::uint64_t(tail.size()));
                    Text(motion.bytes, "MMSXVMDX", 8);
                }
                const auto path = directory / name;
                {
                    std::ofstream file(path, std::ios::binary);
                    file.write(reinterpret_cast<const char*>(motion.bytes.data()), motion.bytes.size());
                    file.close();
                    Check(bool(file), "write Unicode VMD fixture");
                }
                std::printf("VMD path case %u: camera=%d ACP=%u locale=%ls\n",
                            ++caseNumber, camera, GetACP(), _wsetlocale(LC_ALL, nullptr));
                std::fflush(stdout);
                // Shared production entry for the file dialog and file drop.
                LoadVmdFile(path.c_str());
                if (camera) {
                    Check(fixture.cameraKeys[0].distance == -25 && fixture.cameraKeys[0].fov == 50,
                          "Unicode path imports camera keys");
                    Check(fixture.lightKeys[0].color[0] == .2f && fixture.shadowKeys[0].mode == 2,
                          "Unicode path imports light and shadow keys");
                } else {
                    Check(fixture.boneKeys[0].position[0] == 7 && fixture.morphKeys[1].value == .75f,
                          "Unicode path imports bone and morph keys");
                    Check(bone_scale::Key(fixture.Raw(), 0).v[0] == 2,
                          "Unicode path imports the scale extension");
                }
                Check(fs::remove(path), "remove Unicode VMD fixture");
            }
            Check(fs::remove(directory), "remove motion directory");
        }
    }
    Check(fs::remove(root), "remove Unicode path test root");
    Check(_wsetlocale(LC_ALL, previousLocale.c_str()) != nullptr, "restore CRT locale");
}
void UnicodeMotionPaths() { UnicodePaths(false); }
void UnicodeCameraPaths() { UnicodePaths(true); }

void ForceMorphPaste() {
    Fixture fixture;
    auto* app = fixture.app.get();
    auto* model = fixture.model.get();
    model->physicsMode = 2;
    // Duplicate names must not redirect an explicitly selected destination.
    for (auto& morph : fixture.morphs) std::strcpy(morph.name, "face");
    fixture.morphs[0].panel = mdl::MorphPanel::eye;
    fixture.morphs[1].panel = mdl::MorphPanel::mouth;
    std::array<mdl::FrameGroup, 2> rows{};
    rows[0].targetIndex = 0; rows[1].targetIndex = 1;
    model->displayFrames = rows.data(); model->facialFrameCount = 2;
    Check(ForcePasteMorphTarget(app) == -1, "force paste requires an explicit expression selection");
    rows[0].selected = rows[1].selected = 1;
    Check(ForcePasteMorphTarget(app) == -2, "force paste rejects multiple expression destinations");
    rows[0].selected = 0;
    Check(ForcePasteMorphTarget(app) == 1, "force paste resolves the selected expression by index");

    std::array<mdl::MorphClipboardRecord, 3> clipboard{};
    for (auto& record : clipboard) std::strcpy(record.name, "face");
    clipboard[0].frame = 0; clipboard[0].value = .2f;
    clipboard[1].frame = 7; clipboard[1].value = .8f;
    clipboard[2].frame = 20; clipboard[2].value = .4f;
    const auto originalClipboard = clipboard;
    app->MorphClipboard() = clipboard.data(); app->ClipboardCounts().morphs = 3;
    app->CurrentFrame() = 30;
    Check(RegisterMorphKeyAtIndex(fixture.Raw(), 1, 37, .1f) &&
          RegisterMorphKeyAtIndex(fixture.Raw(), 1, 60, .9f), "prepare overwritten and preserved expression keys");
    Check(ForcePasteMorphKeys(app, ForcePasteMorphTarget(app)), "paste expression keys through the production operation");
    std::uint32_t index = 1, previous = 0;
    const std::array<std::uint32_t, 5> frames{0, 30, 37, 50, 60};
    const std::array<float, 5> values{.125f, .2f, .8f, .4f, .9f};
    for (unsigned i = 0; i < frames.size(); ++i) {
        const auto& key = fixture.morphKeys[index];
        Check(key.frame == frames[i] && key.value == values[i] && key.previous == previous,
              "expression paste preserves spacing, overwrites matching frames, and retains other keys");
        Check(key.allocated == (i > 0 && i < 4), "only pasted expression keys are selected");
        previous = index; index = key.next;
        Check((index == 0) == (i + 1 == frames.size()), "expression key chain has no duplicate or missing frames");
    }
    Check(fixture.morphKeys[0].next == 0 && fixture.morphKeys[0].value == 0,
          "same-name source expression is not modified");
    Check(model->maxFrame == 60 && app->state.lastRegisteredFrame == 60 && app->SceneModified(),
          "expression paste updates project extent and modified state");
    Check(std::memcmp(clipboard.data(), originalClipboard.data(), sizeof(clipboard)) == 0,
          "force paste preserves clipboard names, relative frames, and values");
    // Frame zero and a single copied value use the same operation.
    app->CurrentFrame() = 0; app->ClipboardCounts().morphs = 1;
    Check(ForcePasteMorphKeys(app, 1) && fixture.morphKeys[1].value == .2f,
          "single expression value can replace the frame-zero key");
    app->CurrentFrame() = 100;
    Check(ForcePasteMorphKeys(app, 1) && model->maxFrame == 100 && app->state.lastRegisteredFrame == 100,
          "repeated force paste extends the animation at the new current frame");
    auto reject = [&] {
        const auto before = fixture.State();
        Check(!ForcePasteMorphKeys(app, 1), "invalid or blocked expression paste is rejected");
        Check(fixture.State() == before, "rejected expression paste does not mutate keyframes or selection");
    };
    clipboard[0].value = std::numeric_limits<float>::quiet_NaN(); reject();
    clipboard[0].value = .2f; clipboard[0].frame = std::numeric_limits<std::uint32_t>::max(); reject();
    clipboard[0].frame = 0; app->ClipboardCounts().morphs = 0; reject();
    app->ClipboardCounts().morphs = 1; app->PlaybackActive() = 1; reject();
    app->PlaybackActive() = 0; app->FrameStepPlayback() = 1; reject();
    app->FrameStepPlayback() = 0; fixture.morphs[1].panel = mdl::MorphPanel::system; reject();
    app->MorphClipboard() = nullptr;
    std::puts("force_morph: selection, same-name destinations, values, timing, overwrite, repeated paste, and rejection checks passed");
}

void FixedAxisRotations() {
    Check(d3dx::Get().Load(), "load production quaternion functions");
    unsigned tested = 0, failures = 0;
    double maximumError = 0;
    constexpr double radians = 3.14159265358979323846 / 180;
    const std::array<std::array<float, 3>, 3> axes{{{1,0,0}, {-1,0,0}, {.6f,.8f,0}}};
    for (int version : {1, 2}) for (bool pmx : {false, true}) {
        for (auto type : {mdl::BoneType::FixedAxis, mdl::BoneType::UnderIk, mdl::BoneType::RotateMove}) {
            for (const auto& axis : axes) {
                Fixture fixture;
                fixture.model->physicsMode = pmx ? 2 : 1;
                fixture.model->boneCount = 2;
                fixture.bones[0].type = type;
                fixture.bones[0].flags = mdl::kBoneFlagFixedAxis;
                fixture.bones[0].tailBone = 1;
                fixture.bones[1].parent = -1;
                fixture.bones[1].slotIndex = -1;
                fixture.bones[1].rotQuat[3] = fixture.boneKeys[1].rotation[3] = 1;
                for (int c = 0; c < 3; ++c) {
                    fixture.bones[0].axis[c] = axis[c];
                    fixture.bones[0].position[c] = float(c + 1);
                    fixture.bones[1].position[c] = fixture.bones[0].position[c] + axis[c]*5;
                }
                for (float degrees : {-60.f, 60.f, -120.f, 120.f, 0.f, 180.f, -270.f, 270.f}) {
                    for (float sign : {1.f, -1.f}) {
                        Motion motion(version);
                        std::array<float, 4> expected{};
                        for (int c = 0; c < 3; ++c)
                            expected[c] = float(sign * axis[c] * std::sin(degrees*radians/2));
                        expected[3] = float(sign * std::cos(degrees*radians/2));
                        const auto quaternionOffset = motion.counts[0] + 4 + 15 + 4 + 12;
                        std::memcpy(motion.bytes.data()+quaternionOffset, expected.data(), sizeof(expected));
                        Check(fixture.Load(motion.bytes) == 2, "load fixed-axis VMD through production parser and registrar");
                        double error = 0;
                        for (int c = 0; c < 4; ++c)
                            error = (std::max)(error, std::abs(double(fixture.boneKeys[0].rotation[c])-expected[c]));
                        maximumError = (std::max)(maximumError, error);
                        if (error > 2e-6) {
                            if (failures < 6) {
                                const auto* q = fixture.boneKeys[0].rotation;
                                const double sine = q[0]*axis[0]+q[1]*axis[1]+q[2]*axis[2];
                                std::printf("twist_mismatch version=%d pmx=%u type=%u input_degrees=%.0f loaded_degrees=%.6f error=%.6f\n",
                                    version, pmx, unsigned(type), degrees, 2*std::atan2(sine,q[3])/radians, error);
                            }
                            ++failures;
                        }
                        ++tested;
                    }
                }
            }
        }
    }
    std::printf("fixed_axis_vmd cases=%u failures=%u maximum_quaternion_error=%.9g\n", tested, failures, maximumError);
    Check(!failures, "VMD import preserves signed fixed-axis rotations and equivalent quaternion signs");
}

void Overflow() {
    Fixture fixture; const auto before = fixture.State(); Motion motion;
    motion.bytes.resize(motion.ends[0]); SetU32(motion.bytes, motion.counts[0], 0x04000000u);
    fixture.Rejected(motion.bytes, before);
}
void ShortReads() {
    for (int version : {1, 2}) for (bool camera : {false, true}) {
        Fixture fixture(camera); const auto before = fixture.State(); Motion motion(version, camera);
        for (std::size_t end : {std::size_t(0), std::size_t(29), motion.counts[0] - 1})
            fixture.Rejected(Bytes(motion.bytes.begin(), motion.bytes.begin() + end), before);
        for (std::size_t i = 0; i < motion.counts.size(); ++i) {
            for (std::size_t part : {std::size_t(1), std::size_t(2), std::size_t(3)})
                fixture.Rejected(Bytes(motion.bytes.begin(), motion.bytes.begin() + motion.counts[i] + part), before);
            fixture.Rejected(Bytes(motion.bytes.begin(), motion.bytes.begin() + motion.ends[i] - 1), before);
        }
        fixture.Rejected(Bytes(motion.bytes.begin(), motion.bytes.begin() + motion.ikEntries + 3), before);
    }
}
void Counts() {
    Fixture fixture; const auto before = fixture.State(); Motion motion;
    for (auto at : motion.counts) { auto bad = motion.bytes; SetU32(bad, at, 0xffffffffu); fixture.Rejected(bad, before); }
    auto bad = motion.bytes; SetU32(bad, motion.ikEntries, 10001); fixture.Rejected(bad, before);
    bad = motion.bytes; SetU32(bad, motion.ikEntries, 0xffffffffu); fixture.Rejected(bad, before);
}
void Valid() {
    for (int version : {1, 2}) {
        Motion motion(version);
        { Fixture fixture; fixture.Imported(motion.bytes); Check(fixture.displayKeys[0].visible == 0, "nonempty skipped sections preserve IK boundary and false visibility"); }
        for (std::size_t i = 1; i + 1 < motion.ends.size(); ++i) {
            Fixture fixture; fixture.Imported(Bytes(motion.bytes.begin(), motion.bytes.begin() + motion.ends[i]));
        }
        Motion empty(version, false, true); Fixture fixture; fixture.Imported(empty.bytes, false);
    }
}

struct SelectionDialogCase {
    Fixture* fixture;
    int response;
    bool enterHeld;
    unsigned dialogs = 0, registrations = 0;
    HWND owner = nullptr;
    ULONGLONG started = 0;
    bool responseSent = false;
};
SelectionDialogCase* selectionDialogCase = nullptr;

// Clears the thread-local raw key source and this app's foreground override
// before the owner window and fixture die.  Clear() keeps that teardown check
// explicit; the destructor is the fallback and repeats it without effect.
struct SelectionInputScope {
    explicit SelectionInputScope(Fixture& fixture) : fixture(&fixture) {}
    ~SelectionInputScope() { Clear(); }
    SelectionInputScope(const SelectionInputScope&) = delete;
    SelectionInputScope& operator=(const SelectionInputScope&) = delete;
    void Clear() {
        if (fixture == nullptr) return;
        ClearPolledKeyStateForTests();
        ClearPollKeyForegroundForTests(fixture->app.get());
        fixture = nullptr;
    }
    Fixture* fixture;
};

void SetSelectionTestEnter(bool held) {
    // Deterministic raw source for the real poll: the inactive private desktop
    // cannot report OS input, so seed the thread-local test source instead of
    // sending input or changing the user's foreground window.  This is not real
    // desktop focus acceptance; the modal dialog and the registrar stay real.
    unsigned char keys[256]{};
    keys[VK_RETURN] = held ? 0x80 : 0;
    SetPolledKeyStateForTests(keys);
    Check(HasPolledKeyStateForTests(), "selection Enter source is enabled");
    const std::uint16_t expected = static_cast<std::uint16_t>(held ? 0x8000 : 0);
    Check(static_cast<std::uint16_t>(ReadPolledKeyState(VK_RETURN)) == expected &&
          static_cast<std::uint16_t>(ReadPolledAsyncKeyState(VK_RETURN)) == expected,
          "selection Enter source reports the exact polled value");
}

void CALLBACK SelectionDialogTimer(HWND, UINT, UINT_PTR, DWORD) {
    auto& state = *selectionDialogCase;
    Check(GetTickCount64() - state.started < 5000,
          "VMD confirmation appears and closes within five seconds");
    if (state.responseSent) return;
    // An inactive desktop need not activate a MessageBox. Find its initialized
    // controls from the real modal loop, not from an activation/creation hook.
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM parameter) -> BOOL {
        wchar_t type[32]{};
        GetClassNameW(window, type, 32);
        if (std::wcscmp(type, L"#32770") == 0 &&
            GetWindow(window, GW_OWNER) == selectionDialogCase->owner &&
            GetDlgItem(window, IDOK) && GetDlgItem(window, IDCANCEL)) {
            *reinterpret_cast<HWND*>(parameter) = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    ++state.dialogs;
    state.responseSent = true;
    SetSelectionTestEnter(state.enterHeld);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(state.response, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(dialog, state.response)));
}

LRESULT CALLBACK SelectionInputOwner(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_COMMAND && LOWORD(wp) == 500) {
        ++selectionDialogCase->registrations;
        CmdControl500(selectionDialogCase->fixture->app.get(), window, 500, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

DWORD WINAPI SelectionAfterModelConfirmationThread(void* parameter) {
    Check(SetThreadDesktop(static_cast<HDESK>(parameter)) != FALSE,
          "use inactive VMD confirmation desktop");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = SelectionInputOwner;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MmsVmdSelectionInputTest";
    Check(RegisterClassW(&windowClass) != 0, "register VMD input test owner");

    Bytes motion;
    Text(motion, "Vocaloid Motion Data 0002", 30);
    Text(motion, "another model", 20);
    U32(motion, 3);
    for (unsigned frame : {0u, 12u, 24u}) {
        Text(motion, "bone", 15); U32(motion, frame);
        for (float value : {7.f, 8.f, 9.f, 0.f, 0.f, 0.f, 1.f}) Put(motion, value);
        motion.resize(motion.size() + 64);
    }
    U32(motion, 3);
    for (unsigned frame : {0u, 12u, 24u}) {
        Text(motion, "face", 15); U32(motion, frame); Put(motion, .75f);
    }
    U32(motion, 0); U32(motion, 0); U32(motion, 0); // camera, light, shadow
    U32(motion, 3);
    for (unsigned frame : {0u, 12u, 24u}) {
        U32(motion, frame); motion.push_back(1); U32(motion, 0);
    }

    for (bool enterHeld : {true, false}) for (int response : {IDOK, IDCANCEL}) {
        Fixture fixture;
        SelectionDialogCase state{&fixture, response, enterHeld};
        selectionDialogCase = &state;
        SelectionInputScope inputScope(fixture);
        Check(!HasPolledKeyStateForTests(), "dialog case starts with no leaked test key source");
        HWND owner = CreateWindowW(windowClass.lpszClassName, L"VMD selection test",
            WS_OVERLAPPED, 0, 0, 400, 300, nullptr, nullptr, windowClass.hInstance, nullptr);
        Check(owner != nullptr, "create private VMD input owner");
        state.owner = owner;
        fixture.app->Hwnd() = owner; fixture.model->hwnd = owner;
        // The private desktop has no real foreground window, so after the
        // samplers gate on foreground ownership this explicit owner is the
        // only way the poll below can stay eligible. Without it the deliberate
        // Enter control would never reach the registrar.
        SetPollKeyForegroundForTests(fixture.app.get(), owner);
        // Dropped VMDs enter the loader with no menu's Enter latch. An Enter
        // used to accept the mismatch must not become command 500 afterwards.
        // Two real polls establish sustained eligibility without forcing the
        // idle cell: the first post-modal poll must not be a regain, which
        // would latch Enter at held (3) and hide a modal loop that failed to
        // consume its dismissal key.
        SetSelectionTestEnter(false);
        MouseInteractionBegin(fixture.app.get());
        MouseInteractionBegin(fixture.app.get());
        Check(PollKeyEligible(fixture.app.get()) && !PollKeyRegained(fixture.app.get()),
              "pre-modal poll is eligible and not a regain");
        Check(fixture.app->state.enterKeyState == 0, "real idle poll leaves the Enter cell clear");
        Check(state.registrations == 0, "idle precondition registers no Enter command");
        state.started = GetTickCount64();
        const UINT_PTR timer = SetTimer(owner, 1, 10, SelectionDialogTimer);
        Check(timer != 0, "schedule bounded VMD confirmation response");
        const int result = fixture.Load(motion);
        // The real modal loop must consume exactly the Enter it was dismissed
        // with: the held cases leave the polled cell at 3 and the idle cases at
        // 0, for both IDOK and IDCANCEL.
        Check(fixture.app->state.enterKeyState == (enterHeld ? 3 : 0),
              "modal dismissal consumes exactly the Enter it was closed with");
        Check(PollKeyEligible(fixture.app.get()) && !PollKeyRegained(fixture.app.get()),
              "modal polling neither loses nor regains input eligibility");
        Check(KillTimer(owner, timer) != FALSE && state.dialogs == 1,
              "one real VMD model-name confirmation was handled");
        const bool imported = response == IDOK;
        Check(result == (imported ? 1 : 0), "confirmation accepts or cancels the VMD import");

        auto checkSelection = [&] {
            if (!imported) {
                Check(fixture.model->undoState[0] == 4 && fixture.boneKeys[0].position[0] == -3 &&
                      fixture.morphKeys[1].value == .125f && fixture.boneKeys[0].allocated &&
                      fixture.morphKeys[1].allocated && fixture.displayKeys[0].allocated &&
                      fixture.boneKeys.back().allocated && fixture.morphKeys.back().allocated &&
                      fixture.displayKeys.back().allocated,
                      "cancel preserves motion, previous selections, and undo history");
                return;
            }
            auto checkTrack = [](const auto* keys, unsigned root) {
                unsigned slot = root;
                for (unsigned frame : {0u, 12u, 24u}) {
                    Check(keys[slot].frame == frame && keys[slot].allocated,
                          "every imported bone, morph, and display key remains selected");
                    if (frame == 24) Check(keys[slot].next == 0, "imported track ends at the expected key");
                    else Check(keys[slot].next != 0, "imported track retains its linked keys");
                    slot = keys[slot].next;
                }
            };
            checkTrack(fixture.model->boneKeys, 0);
            checkTrack(fixture.model->morphKeys, 1);
            checkTrack(fixture.model->displayKeys, 0);
            Check(fixture.model->undoState[0] == 5 && fixture.model->undoState[1] == 5,
                  "confirmation creates only the motion import undo entry");
            Check(!fixture.boneKeys.back().allocated && !fixture.morphKeys.back().allocated &&
                  !fixture.displayKeys.back().allocated,
                  "import selects its own keys instead of unrelated previous selections");
        };
        checkSelection();
        auto poll = [&](bool held) {
            SetSelectionTestEnter(held);
            MouseInteractionBegin(fixture.app.get());
            Check(PollKeyEligible(fixture.app.get()) && !PollKeyRegained(fixture.app.get()),
                  "selection poll keeps sustained eligibility without a regain");
            PumpEnterRegisterFrame(fixture.app.get(), owner, true);
        };
        for (int tick = 0; tick < 3; ++tick) {
            poll(enterHeld);
            Check(state.registrations == 0, "modal confirmation never leaks an Enter registration");
            checkSelection();
        }
        poll(false); poll(false);
        Check(state.registrations == 0 && fixture.app->state.enterKeyState == 0,
              "releasing the confirmation key returns the input poll to idle");
        if (imported) {
            fixture.selected[0] = 1;
            fixture.app->CurrentFrame() = 36;
            poll(true);
            Check(state.registrations == 1 && fixture.model->undoState[0] == 6,
                  "a later deliberate Enter still registers exactly one frame");
            unsigned slot = 0;
            for (int steps = 0; steps < 4 && fixture.model->boneKeys[slot].next; ++steps)
                slot = fixture.model->boneKeys[slot].next;
            Check(fixture.model->boneKeys[slot].frame == 36 && fixture.model->boneKeys[slot].allocated,
                  "deliberate Enter reaches the real registrar and selects its new key");
            poll(true);
            Check(state.registrations == 1, "holding a deliberate Enter does not repeat registration");
        }
        // Drop the raw source and the foreground override before the owner
        // window and fixture die; the guard destructor is the idempotent
        // fallback for the same cleanup.
        inputScope.Clear();
        Check(!HasPolledKeyStateForTests(), "dialog case leaves no test key source behind");
        Check(!fixture.app->PolledKeys().HasForegroundOverride(),
              "dialog case clears the foreground override before teardown");
        DestroyWindow(owner);
        fixture.app->Hwnd() = nullptr; fixture.model->hwnd = nullptr;
        selectionDialogCase = nullptr;
    }
    Check(!HasPolledKeyStateForTests(), "dialog-enter worker leaves no test key source behind");
    Check(UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance) != FALSE,
          "release private VMD owner class");
    return 0;
}

void SelectionAfterModelConfirmation() {
    wchar_t desktopName[80]{};
    swprintf_s(desktopName, L"MMS-vmd-selection-%lu", GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(desktop != nullptr, "create inactive VMD confirmation desktop");
    // Destroy any system-created IME/dialog windows with their owning thread
    // before closing the desktop; they can outlive the explicit owner window.
    const HANDLE thread = CreateThread(nullptr, 0, SelectionAfterModelConfirmationThread,
                                       desktop, 0, nullptr);
    Check(thread && WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0,
          "VMD confirmation regression finishes within thirty seconds");
    DWORD exitCode = 1;
    Check(GetExitCodeThread(thread, &exitCode) && exitCode == 0,
          "private VMD input thread succeeds");
    CloseHandle(thread);
    // The Windows input framework can retain a helper thread on this desktop
    // until process exit. In that case the process owns its remaining handle.
    Check(CloseDesktop(desktop) != FALSE || GetLastError() == ERROR_BUSY,
          "release inactive VMD confirmation desktop");
}

// ---- foreground-owner focus regression for the two production samplers ----
// Regression contract for the shared foreground-owner latch.  Both production
// samplers (MouseInteractionBegin and PollKeyboardStates) read the polled
// raw-key helpers and must advance AdvancePollKeyEligibility and gate
// keyboard-history cells the same way:
//   * the first sampler entry after an ineligible poll regains eligibility and
//     the second sampler of the same frame sees sustained eligibility;
//   * keyboard-history cells clear while ineligible, a key held across the loss
//     is latched as held (3) instead of firing a press (1), and a release
//     followed by a repress is a fresh press;
//   * mouse button cells keep their ordinary 1/3/2 edges regardless of the
//     keyboard gate and of a regain.
// The case still runs on a private inactive desktop for the inactive
// viewport/window behaviour, but it no longer injects OS input: the regression
// fixture observed SetKeyboardState report success while the immediate raw and
// table readbacks were zero (parallel-focus-test2.log), so the fixture seeds the
// thread-local test source instead (SetPolledKeyStateForTests) and reads it back
// through the same helpers production uses: ReadPolledKeyState and
// ReadPolledAsyncKeyState.  The seam replaces only the raw key source, so
// foreground eligibility and the edge cells stay whatever the samplers compute.
// The existing foreground override stands in for the foreground query an
// inactive desktop cannot answer.  No input is sent, no foreground is changed,
// and the interactive desktop is never switched.
using PollSampler = void (*)(MMDApp*);

struct SamplerPlan {
    const char* name;
    PollSampler first;
    PollSampler second;  // nullptr for a one-sampler plan
};

const SamplerPlan kSamplerPlans[] = {
    {"mouse", MouseInteractionBegin, nullptr},
    {"keyboard", PollKeyboardStates, nullptr},
    {"mouse then keyboard", MouseInteractionBegin, PollKeyboardStates},
    {"keyboard then mouse", PollKeyboardStates, MouseInteractionBegin},
};

bool HasSecondSampler(const SamplerPlan& plan) {
    return plan.second != nullptr;
}

std::size_t SamplerCalls(const SamplerPlan& plan) {
    return HasSecondSampler(plan) ? 2 : 1;
}

void FailSamplerPlan(const SamplerPlan& plan, const char* description) {
    std::fprintf(stderr, "FAIL: %s [sampler plan: %s]\n", description, plan.name);
    std::exit(1);
}

// Failure evidence for the seeded-source sampler fixture.  The intent is the
// raw key state the fixture last stored through SetPolledKeyStateForTests; the
// counters locate the sample and the phase names the check being run.  Nothing
// here changes what the fixture does or what production samples.
struct SamplerInputIntent {
    int virtualKey = -1;
    bool down = false;
};
SamplerInputIntent g_samplerIntent;
const SamplerPlan* g_samplerPlan = nullptr;
const char* g_samplerPhase = "setup";
unsigned g_samplerFrame = 0;
unsigned g_samplerCall = 0;

struct SamplerCallEvidence {
    int virtualKey = -1;
    bool requestedDown = false;
    bool sourceEnabled = false;
    std::int16_t sourceSync = 0;
    std::int16_t sourceAsync = 0;
    std::int32_t cell = 0;
    bool eligible = false;
    bool regained = false;
    void* owner = nullptr;
    void* floating = nullptr;
    bool overrideEnabled = false;
    void* overrideValue = nullptr;
    HWND foreground = nullptr;
};

struct SamplerCallResult {
    SamplerCallEvidence pre, post;
    std::int32_t oldCell = 0;
};

bool SamplerSourceDown(std::int16_t source) {
    return (source & 0x8000) != 0;
}

// SetSamplerKey stores only 0x80 or 0x00 in the sampled byte, so while the test
// source is enabled both read helpers must report exactly 0x8000 or 0x0000.
bool SamplerSourceMatches(const SamplerCallEvidence& evidence, bool down) {
    const unsigned expected = down ? 0x8000u : 0u;
    return evidence.sourceEnabled &&
           static_cast<unsigned>(static_cast<std::uint16_t>(evidence.sourceSync)) == expected &&
           static_cast<unsigned>(static_cast<std::uint16_t>(evidence.sourceAsync)) == expected;
}

SamplerCallEvidence CaptureSamplerEvidence(MMDApp* app, std::int32_t cell) {
    SamplerCallEvidence evidence;
    evidence.virtualKey = g_samplerIntent.virtualKey;
    evidence.requestedDown = g_samplerIntent.down;
    // Read the test source through the same two helpers the samplers use; the
    // cell, latches, and foreground recorded afterwards locate the sample.
    evidence.sourceEnabled = HasPolledKeyStateForTests();
    if (evidence.virtualKey >= 0) {
        evidence.sourceSync = ReadPolledKeyState(evidence.virtualKey);
        evidence.sourceAsync = ReadPolledAsyncKeyState(evidence.virtualKey);
    }
    evidence.cell = cell;
    evidence.eligible = PollKeyEligible(app);
    evidence.regained = PollKeyRegained(app);
    evidence.owner = app->state.hwnd;
    evidence.floating = app->state.floatingWindow;
    evidence.overrideEnabled = app->PolledKeys().HasForegroundOverride();
    evidence.overrideValue = app->PolledKeys().ForegroundOverride();
    evidence.foreground = GetForegroundWindow();
    return evidence;
}

SamplerCallResult InvokeSampler(MMDApp* app, PollSampler sampler, std::int32_t& cell) {
    SamplerCallResult result;
    result.oldCell = cell;
    result.pre = CaptureSamplerEvidence(app, cell);
    ++g_samplerCall;
    sampler(app);
    result.post = CaptureSamplerEvidence(app, cell);
    return result;
}

bool SamplerInputMismatch(const SamplerCallResult& result) {
    const bool expectedDown = result.pre.requestedDown;
    return !SamplerSourceMatches(result.pre, expectedDown) ||
           !SamplerSourceMatches(result.post, expectedDown);
}

void PrintSamplerCallEvidence(const SamplerPlan& plan, const char* cellName,
                              std::size_t samplerIndex, const SamplerCallResult& result,
                              int expectedCell, int expectedEligibility, int expectedRegain) {
    const auto& pre = result.pre;
    const auto& post = result.post;
    std::fprintf(stderr, "[sampler-diagnostic] plan=%s phase=%s frame=%u call=%u sampler=%zu cell=%s\n",
                 plan.name, g_samplerPhase, g_samplerFrame, g_samplerCall, samplerIndex, cellName);
    std::fprintf(stderr, "  intent: vk=%d requested_down=%d\n", pre.virtualKey, pre.requestedDown ? 1 : 0);
    // These lines describe the explicit test source, not OS key state.
    const auto printSourceState = [](const char* label, const SamplerCallEvidence& evidence) {
        std::fprintf(stderr,
                     "  %-4s: test_source_enabled=%d sync=0x%04X down=%d async=0x%04X down=%d "
                     "cell=%d eligible=%d regained=%d\n",
                     label, evidence.sourceEnabled ? 1 : 0,
                     static_cast<unsigned>(static_cast<std::uint16_t>(evidence.sourceSync)),
                     SamplerSourceDown(evidence.sourceSync) ? 1 : 0,
                     static_cast<unsigned>(static_cast<std::uint16_t>(evidence.sourceAsync)),
                     SamplerSourceDown(evidence.sourceAsync) ? 1 : 0,
                     static_cast<int>(evidence.cell), evidence.eligible ? 1 : 0,
                     evidence.regained ? 1 : 0);
    };
    printSourceState("pre", pre);
    printSourceState("post", post);
    if (expectedCell < 0)
        std::fprintf(stderr, "  cell: old=%d new=%d (settle frame has no expected cell)\n",
                     static_cast<int>(result.oldCell), static_cast<int>(post.cell));
    else
        std::fprintf(stderr, "  cell: old=%d expected=%d new=%d\n",
                     static_cast<int>(result.oldCell), expectedCell, static_cast<int>(post.cell));
    if (expectedEligibility >= 0 && expectedRegain >= 0)
        std::fprintf(stderr, "  expect: eligible=%d regained=%d\n", expectedEligibility, expectedRegain);
    // Owner/override plus the desktop's current foreground: localisation only.
    std::fprintf(stderr,
                 "  windows_pre : owner=%p floating=%p override_enabled=%d override=%p foreground=%p\n",
                 pre.owner, pre.floating, pre.overrideEnabled ? 1 : 0, pre.overrideValue,
                 static_cast<void*>(pre.foreground));
    std::fprintf(stderr,
                 "  windows_post: owner=%p floating=%p override_enabled=%d override=%p foreground=%p\n",
                 post.owner, post.floating, post.overrideEnabled ? 1 : 0, post.overrideValue,
                 static_cast<void*>(post.foreground));
}

void FailSamplerCall(const SamplerPlan& plan, const char* cellName, std::size_t samplerIndex,
                     const SamplerCallResult& result, int expectedCell,
                     int expectedEligibility, int expectedRegain, bool cellMismatch,
                     bool inputMismatch, bool eligibilityMismatch, bool regainMismatch) {
    if (cellMismatch)
        std::fprintf(stderr, "FAIL: %s=%d after sampler %zu, expected %d [sampler plan: %s]\n",
                     cellName, static_cast<int>(result.post.cell), samplerIndex,
                     expectedCell, plan.name);
    if (inputMismatch)
        std::fprintf(stderr,
                     "FAIL: injected sampler key precondition does not match the sampler test"
                     " source [sampler plan: %s]\n", plan.name);
    if (eligibilityMismatch)
        std::fprintf(stderr, "FAIL: sampler entry must advance the foreground eligibility [sampler plan: %s]\n",
                     plan.name);
    if (regainMismatch)
        std::fprintf(stderr, "FAIL: only the first sampler after a loss reports the regain edge [sampler plan: %s]\n",
                     plan.name);
    PrintSamplerCallEvidence(plan, cellName, samplerIndex, result, expectedCell,
                             expectedEligibility, expectedRegain);
    std::exit(1);
}

// One frame = every sampler of the plan in order.  expected holds one value per
// sampler call, in call order; only the first call of a frame can report the
// regain edge because that call advances the shared latch.  Each call records
// the seeded test-source key, the sampled cell, the owner/foreground identities,
// and the eligibility latch before and after; every mismatch is reported
// together.
void ExpectSamplerFrame(MMDApp* app, const SamplerPlan& plan, const char* cellName,
                        std::int32_t& cell, const std::vector<std::int32_t>& expected,
                        bool eligible, bool regained) {
    const PollSampler steps[2] = {plan.first, plan.second};
    const std::size_t calls = SamplerCalls(plan);
    if (expected.size() != calls)
        FailSamplerPlan(plan, "one expected value is required per sampler call");
    ++g_samplerFrame;
    for (std::size_t i = 0; i < calls; ++i) {
        const SamplerCallResult result = InvokeSampler(app, steps[i], cell);
        const bool cellMismatch = result.post.cell != expected[i];
        const bool inputMismatch = SamplerInputMismatch(result);
        const bool eligibilityMismatch = PollKeyEligible(app) != eligible;
        const bool regainMismatch = PollKeyRegained(app) != (i == 0 && regained);
        if (cellMismatch || inputMismatch || eligibilityMismatch || regainMismatch)
            FailSamplerCall(plan, cellName, i + 1, result, static_cast<int>(expected[i]),
                            eligible ? 1 : 0, (i == 0 && regained) ? 1 : 0,
                            cellMismatch, inputMismatch, eligibilityMismatch, regainMismatch);
    }
}

// Settle frames assert no cell value, but their seeded key still has to be the
// state the sampler reads; report the same evidence when it is not.
void RunSamplerFrame(MMDApp* app, const SamplerPlan& plan, const char* cellName,
                     std::int32_t& cell) {
    const PollSampler steps[2] = {plan.first, plan.second};
    const std::size_t calls = SamplerCalls(plan);
    ++g_samplerFrame;
    for (std::size_t i = 0; i < calls; ++i) {
        const SamplerCallResult result = InvokeSampler(app, steps[i], cell);
        if (SamplerInputMismatch(result))
            FailSamplerCall(plan, cellName, i + 1, result, -1, -1, -1,
                            false, true, false, false);
    }
}

void SetSamplerKey(int virtualKey, bool down) {
    // Seed the thread-local test source: a zeroed 256-byte array with the one
    // sampled byte set to 0x80 when down.  SetKeyboardState is gone because the
    // regression fixture observed it report success while the immediate raw and
    // table readbacks stayed zero.
    unsigned char source[256]{};
    source[virtualKey] = down ? 0x80 : 0;
    SetPolledKeyStateForTests(source);
    g_samplerIntent.virtualKey = virtualKey;
    g_samplerIntent.down = down;
    // Read the stored source back through the same helpers production uses and
    // require the exact encoding the setter wrote: bit 0x80 becomes SHORT 0x8000
    // and nothing else is set for these keys.
    const std::uint16_t expected = down ? 0x8000u : 0u;
    const bool enabled = HasPolledKeyStateForTests();
    const std::uint16_t sync = static_cast<std::uint16_t>(ReadPolledKeyState(virtualKey));
    const std::uint16_t async = static_cast<std::uint16_t>(ReadPolledAsyncKeyState(virtualKey));
    if (!enabled || sync != expected || async != expected) {
        std::fprintf(stderr,
                     "FAIL: private-desktop sampler key injection is not visible to the sampler"
                     " test source [sampler plan: %s, phase: %s]\n"
                     "  requested: vk=%d down=%d\n"
                     "  test_source: enabled=%d sync=0x%04X async=0x%04X expected=0x%04X\n",
                     g_samplerPlan ? g_samplerPlan->name : "-", g_samplerPhase,
                     virtualKey, down ? 1 : 0, enabled ? 1 : 0,
                     static_cast<unsigned>(sync), static_cast<unsigned>(async),
                     static_cast<unsigned>(expected));
        std::exit(1);
    }
}

// Observation exchanged with the short-lived source-scope thread.
struct SamplerSourceThreadObservation {
    bool startedDisabled = false;
    bool ownSourceApplied = false;
    bool clearedBeforeExit = false;
};

// Runs on a fresh thread: the test source is thread-local, so this thread must
// start disabled, may seed its own bytes, and must leave them cleared.
DWORD WINAPI SamplerSourceContractThread(void* parameter) {
    auto& observation = *static_cast<SamplerSourceThreadObservation*>(parameter);
    observation.startedDisabled = !HasPolledKeyStateForTests();
    unsigned char own[256]{};
    own[VK_F3] = 0x80;
    SetPolledKeyStateForTests(own);
    observation.ownSourceApplied =
        HasPolledKeyStateForTests() &&
        static_cast<std::uint16_t>(ReadPolledKeyState(VK_F3)) == 0x8000u &&
        static_cast<std::uint16_t>(ReadPolledAsyncKeyState(VK_F3)) == 0x8000u &&
        static_cast<std::uint16_t>(ReadPolledKeyState(VK_F1)) == 0u;
    ClearPolledKeyStateForTests();
    observation.clearedBeforeExit = !HasPolledKeyStateForTests();
    return 0;
}

// Contract checks for the thread-local test key source itself.  They run on the
// sampler-focus worker thread under the same private desktop and touch no window
// and no OS input: the seam copies caller bytes into a 256-byte array, reports
// those bytes while enabled, and forwards to the original Win32 reads while
// disabled.
void CheckPolledKeySourceContract() {
    g_samplerPhase = "source-contract";
    constexpr int kKey = VK_F1;  // not polled by either sampler
    const auto sync = [](int virtualKey) {
        return static_cast<unsigned>(static_cast<std::uint16_t>(ReadPolledKeyState(virtualKey)));
    };
    const auto async = [](int virtualKey) {
        return static_cast<unsigned>(static_cast<std::uint16_t>(ReadPolledAsyncKeyState(virtualKey)));
    };

    // The setter copies: mutating the caller's array afterwards changes nothing.
    unsigned char source[256]{};
    source[kKey] = 0x80;
    SetPolledKeyStateForTests(source);
    source[kKey] = 0x00;
    Check(sync(kKey) == 0x8000u && async(kKey) == 0x8000u,
          "the test source copies caller bytes instead of keeping their pointer");

    // A full-array replacement drops the previously down key.
    unsigned char replacement[256]{};
    replacement[kKey + 1] = 0x80;
    SetPolledKeyStateForTests(replacement);
    Check(sync(kKey) == 0u && async(kKey) == 0u,
          "a full-array replacement clears the previous down key");
    Check(sync(kKey + 1) == 0x8000u && async(kKey + 1) == 0x8000u,
          "a full-array replacement stores the new down key");

    // Raw byte to SHORT encoding: 0x80 -> 0x8000, bit 0x01 -> toggle bit.
    const struct { unsigned char byte; unsigned expected; } encodings[] = {
        {0x00, 0x0000u}, {0x01, 0x0001u}, {0x80, 0x8000u}, {0x81, 0x8001u}};
    for (const auto& encoding : encodings) {
        unsigned char bytes[256]{};
        bytes[kKey] = encoding.byte;
        SetPolledKeyStateForTests(bytes);
        char description[96]{};
        std::snprintf(description, sizeof(description),
                      "the test source encodes raw byte 0x%02X as SHORT 0x%04X",
                      static_cast<unsigned>(encoding.byte), encoding.expected);
        Check(sync(kKey) == encoding.expected && async(kKey) == encoding.expected, description);
    }
    Check(sync(-1) == 0u && async(-1) == 0u && sync(256) == 0u && async(256) == 0u,
          "the test source reports zero for indices outside 0..255");

    // Clearing disables the seam.  The default Win32 forwarding is verified
    // statically against the foundation contract, not by a second pair of OS
    // reads here: GetAsyncKeyState's low bit is consumed by the read itself, so
    // two sequential calls are not a deterministic comparison.
    ClearPolledKeyStateForTests();
    Check(!HasPolledKeyStateForTests(), "clearing the test source disables it");
    unsigned char again[256]{};
    again[kKey] = 0x80;
    SetPolledKeyStateForTests(again);
    Check(HasPolledKeyStateForTests() && sync(kKey) == 0x8000u && async(kKey) == 0x8000u,
          "the test source can be enabled again after clearing");

    // A second short-lived thread starts disabled, sets its own source without
    // changing the parent's, and clears itself before exiting.
    SamplerSourceThreadObservation observation;
    const HANDLE observer = CreateThread(nullptr, 0, SamplerSourceContractThread,
                                         &observation, 0, nullptr);
    Check(observer != nullptr, "start the test-source scope thread");
    Check(WaitForSingleObject(observer, 5000) == WAIT_OBJECT_0,
          "the test-source scope thread finishes within five seconds");
    CloseHandle(observer);
    Check(observation.startedDisabled && observation.ownSourceApplied && observation.clearedBeforeExit,
          "a second thread gets its own test source and clears it");
    Check(HasPolledKeyStateForTests() && sync(kKey) == 0x8000u && async(kKey) == 0x8000u,
          "a second thread cannot change the parent test source");
    ClearPolledKeyStateForTests();
    Check(!HasPolledKeyStateForTests(), "the source contract leaves the test source cleared");
}

std::int32_t& DeleteKeyCell(MMDApp* app) { return app->state.deleteKeyState; }
std::int32_t& EnterKeyCell(MMDApp* app) { return app->state.enterKeyState; }
std::int32_t& Numpad5KeyCell(MMDApp* app) { return app->state.numpadKeyState[5]; }

struct SamplerKeyboardCell {
    const char* name;
    int virtualKey;
    std::int32_t& (*cell)(MMDApp*);
};

const SamplerKeyboardCell kSamplerKeyboardCells[] = {
    {"Delete", VK_DELETE, DeleteKeyCell},
    {"Enter", VK_RETURN, EnterKeyCell},
    {"Numpad5", VK_NUMPAD5, Numpad5KeyCell},
};

std::int32_t& LeftMouseCell(MMDApp* app) { return app->state.leftMouseButtonState; }
std::int32_t& MiddleMouseCell(MMDApp* app) { return app->state.middleMouseButtonState; }
std::int32_t& RightMouseCell(MMDApp* app) { return app->state.rightMouseButtonState; }

struct SamplerMouseCell {
    const char* name;
    int virtualKey;
    std::int32_t& (*cell)(MMDApp*);
};

// All three buttons are polled by both samplers and must stay exempt from the
// keyboard gate.  The synthetic left button is safe on this desktop because the
// inactive viewport never becomes active: the press edge skips the viewport
// tool entry and the release edge only runs its idle reset before the poll
// returns, so no viewport tool can start.
const SamplerMouseCell kSamplerMouseCells[] = {
    {"LeftButton", VK_LBUTTON, LeftMouseCell},
    {"MiddleButton", VK_MBUTTON, MiddleMouseCell},
    {"RightButton", VK_RBUTTON, RightMouseCell},
};

void CheckSamplerEligibility(const SamplerPlan& plan, MMDApp* app,
                             HWND mainWindow, HWND floatingWindow, HWND foreignWindow) {
    g_samplerPhase = "eligibility";
    SetSamplerKey(VK_DELETE, false);
    std::int32_t& idle = DeleteKeyCell(app);
    const auto zeros = [&] { return std::vector<std::int32_t>(SamplerCalls(plan), 0); };
    if (PollKeyEligible(app) || PollKeyRegained(app))
        FailSamplerPlan(plan, "a freshly constructed app must have no polled keyboard history");
    if (app->PolledKeys().HasForegroundOverride() || app->PolledKeys().ForegroundOverride() != nullptr)
        FailSamplerPlan(plan, "a freshly constructed app must not have a foreground override");

    // Without an override the private desktop has no matching foreground, so
    // the sampler's default eligibility is off.
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), false, false);

    // First eligible entry regains; the next frame sees sustained eligibility.
    SetPollKeyForegroundForTests(app, mainWindow);
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), true, true);
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), true, false);

    // A foreign foreground is ineligible.
    SetPollKeyForegroundForTests(app, foreignWindow);
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), false, false);

    // An explicit null override stays enabled and is ineligible.
    SetPollKeyForegroundForTests(app, nullptr);
    if (!app->PolledKeys().HasForegroundOverride() || app->PolledKeys().ForegroundOverride() != nullptr)
        FailSamplerPlan(plan, "an explicit null foreground override must stay enabled");
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), false, false);

    // The floating owner is eligible and regains after the loss.
    SetPollKeyForegroundForTests(app, floatingWindow);
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), true, true);

    // The main owner regains after the loss as well.
    SetPollKeyForegroundForTests(app, foreignWindow);
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), false, false);
    SetPollKeyForegroundForTests(app, mainWindow);
    ExpectSamplerFrame(app, plan, "Delete", idle, zeros(), true, true);
}

void CheckSamplerKeyboardCycle(const SamplerPlan& plan, MMDApp* app,
                               HWND mainWindow, HWND floatingWindow, HWND foreignWindow,
                               const SamplerKeyboardCell& keyboard) {
    g_samplerPhase = "keyboard-cycle";
    std::int32_t& cell = keyboard.cell(app);
    // One sampler sees the release edge once; a second sampler in the same frame
    // consumes it, so the paired value differs from the single-sampler value.
    const auto frame = [&](std::int32_t single, std::int32_t paired, bool eligible, bool regained) {
        const std::vector<std::int32_t> expected = HasSecondSampler(plan)
            ? std::vector<std::int32_t>{single, paired}
            : std::vector<std::int32_t>{single};
        ExpectSamplerFrame(app, plan, keyboard.name, cell, expected, eligible, regained);
    };

    SetSamplerKey(keyboard.virtualKey, false);
    SetPollKeyForegroundForTests(app, foreignWindow);
    frame(0, 0, false, false);       // ineligible: clears any stale edge
    SetPollKeyForegroundForTests(app, mainWindow);
    frame(0, 0, true, true);         // idle eligible baseline, first regain
    frame(0, 0, true, false);        // sustained idle never synthesizes a press
    SetSamplerKey(keyboard.virtualKey, true);
    frame(1, 3, true, false);        // press edge, then the second sampler sees held
    frame(3, 3, true, false);        // hold
    SetSamplerKey(keyboard.virtualKey, false);
    frame(2, 0, true, false);        // release edge, then idle on the second sampler
    SetSamplerKey(keyboard.virtualKey, true);
    frame(1, 3, true, false);        // immediate repress is a fresh press
    frame(3, 3, true, false);        // key held across the loss
    SetPollKeyForegroundForTests(app, foreignWindow);
    frame(0, 0, false, false);       // background loss clears even a stale down
    SetPollKeyForegroundForTests(app, floatingWindow);
    frame(3, 3, true, true);         // regain latches held, neither sampler fires a press
    SetSamplerKey(keyboard.virtualKey, false);
    frame(2, 0, true, false);        // release after regain
    SetSamplerKey(keyboard.virtualKey, true);
    frame(1, 3, true, false);        // release then repress works after regain
}

void CheckSamplerMouseCycle(const SamplerPlan& plan, MMDApp* app,
                            HWND mainWindow, HWND foreignWindow,
                            const SamplerMouseCell& mouse) {
    g_samplerPhase = "mouse-cycle";
    std::int32_t& cell = mouse.cell(app);
    const auto frame = [&](std::int32_t single, std::int32_t paired, bool eligible, bool regained) {
        const std::vector<std::int32_t> expected = HasSecondSampler(plan)
            ? std::vector<std::int32_t>{single, paired}
            : std::vector<std::int32_t>{single};
        ExpectSamplerFrame(app, plan, mouse.name, cell, expected, eligible, regained);
    };

    // Mouse cells are not gated, so two idle frames settle any previous edge.
    SetSamplerKey(mouse.virtualKey, false);
    SetPollKeyForegroundForTests(app, foreignWindow);
    RunSamplerFrame(app, plan, mouse.name, cell);
    RunSamplerFrame(app, plan, mouse.name, cell);
    SetPollKeyForegroundForTests(app, mainWindow);
    frame(0, 0, true, true);         // eligible idle baseline
    SetSamplerKey(mouse.virtualKey, true);
    frame(1, 3, true, false);        // eligible press
    frame(3, 3, true, false);        // hold
    SetSamplerKey(mouse.virtualKey, false);
    frame(2, 0, true, false);        // release
    frame(0, 0, true, false);        // idle after release
    SetPollKeyForegroundForTests(app, foreignWindow);
    SetSamplerKey(mouse.virtualKey, true);
    frame(1, 3, false, false);       // keyboard-ineligible frames keep the mouse edge
    frame(3, 3, false, false);       // hold
    SetSamplerKey(mouse.virtualKey, false);
    frame(2, 0, false, false);       // release
    frame(0, 0, false, false);       // idle
    SetPollKeyForegroundForTests(app, mainWindow);
    SetSamplerKey(mouse.virtualKey, true);
    frame(1, 3, true, true);         // a regain does not latch the mouse as held
    frame(3, 3, true, false);        // hold
    SetSamplerKey(mouse.virtualKey, false);
    frame(2, 0, true, false);        // release
}

// The test key source is thread-local.  The sampler worker must disable it on
// every normal exit; a failed check exits the process instead of unwinding.
struct SamplerTestSourceScope {
    SamplerTestSourceScope() = default;
    ~SamplerTestSourceScope() { ClearPolledKeyStateForTests(); }
    SamplerTestSourceScope(const SamplerTestSourceScope&) = delete;
    SamplerTestSourceScope& operator=(const SamplerTestSourceScope&) = delete;
};

DWORD WINAPI SamplerFocusRegressionThread(void* parameter) {
    Check(SetThreadDesktop(static_cast<HDESK>(parameter)) != FALSE,
          "use inactive sampler-focus desktop");
    // No inherited enabled state: a fresh worker thread starts with the seam
    // disabled, and the contract checks leave it disabled for the fixture.
    Check(!HasPolledKeyStateForTests(), "sampler-focus worker starts without a test key source");
    CheckPolledKeySourceContract();
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MmsVmdSamplerFocusTest";
    Check(RegisterClassW(&windowClass) != 0, "register sampler-focus owner windows");
    auto create = [&](const wchar_t* title) {
        return CreateWindowW(windowClass.lpszClassName, title, WS_OVERLAPPED,
                             0, 0, 320, 240, nullptr, nullptr, windowClass.hInstance, nullptr);
    };
    const HWND mainWindow = create(L"sampler main");
    const HWND floatingWindow = create(L"sampler floating");
    const HWND foreignWindow = create(L"sampler foreign");
    Check(mainWindow && floatingWindow && foreignWindow, "create sampler-focus owner windows");

    {
        SamplerTestSourceScope sourceScope;
        for (const SamplerPlan& plan : kSamplerPlans) {
            g_samplerPlan = &plan;
            g_samplerFrame = 0;
            g_samplerCall = 0;
            Fixture fixture;
            MMDApp* app = fixture.app.get();
            app->Hwnd() = mainWindow;
            app->FloatingWindow() = floatingWindow;
            fixture.model->hwnd = mainWindow;
            CheckSamplerEligibility(plan, app, mainWindow, floatingWindow, foreignWindow);
            for (const SamplerKeyboardCell& keyboard : kSamplerKeyboardCells)
                CheckSamplerKeyboardCycle(plan, app, mainWindow, floatingWindow, foreignWindow, keyboard);
            for (const SamplerMouseCell& mouse : kSamplerMouseCells)
                CheckSamplerMouseCycle(plan, app, mainWindow, foreignWindow, mouse);
            g_samplerPhase = "cleanup";
            SetSamplerKey(VK_DELETE, false);
            ClearPollKeyForegroundForTests(app);
            if (app->PolledKeys().HasForegroundOverride())
                FailSamplerPlan(plan, "clearing the test override must disable it");
            ClearPolledKeyStateForTests();
            if (HasPolledKeyStateForTests())
                FailSamplerPlan(plan, "the sampler test key source must be cleared after a plan");
            app->Hwnd() = nullptr;
            app->FloatingWindow() = nullptr;
            fixture.model->hwnd = nullptr;
        }
    }
    Check(!HasPolledKeyStateForTests(), "sampler-focus worker leaves no test key source behind");

    Check(DestroyWindow(foreignWindow) != FALSE, "destroy sampler-focus foreign window");
    Check(DestroyWindow(floatingWindow) != FALSE, "destroy sampler-focus floating window");
    Check(DestroyWindow(mainWindow) != FALSE, "destroy sampler-focus main window");
    Check(UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance) != FALSE,
          "release sampler-focus owner class");
    return 0;
}

void SamplerFocusRegression() {
    wchar_t desktopName[80]{};
    swprintf_s(desktopName, L"MMS-vmd-sampler-focus-%lu", GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(desktop != nullptr, "create inactive sampler-focus desktop");
    const HANDLE thread = CreateThread(nullptr, 0, SamplerFocusRegressionThread, desktop, 0, nullptr);
    Check(thread && WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0,
          "sampler-focus regression finishes within thirty seconds");
    DWORD exitCode = 1;
    Check(GetExitCodeThread(thread, &exitCode) && exitCode == 0,
          "private sampler-focus thread succeeds");
    CloseHandle(thread);
    Check(CloseDesktop(desktop) != FALSE || GetLastError() == ERROR_BUSY,
          "release inactive sampler-focus desktop");
}

void Tails() {
    Motion motion;
    for (const Bytes& tail : {Bytes{1, 2, 3}, Bytes{'M','M','S','M','C','R','V','1', 16, 0, 0, 0}, Bytes{'M','M','S','X','M','M','S','X'}}) {
        auto bytes = motion.bytes; bytes.insert(bytes.end(), tail.begin(), tail.end());
        Fixture fixture; fixture.Imported(bytes);
    }
#if !MMD_ORIGINAL_PROFILE
    // A checksummed morph block followed by a valid scale extension proves
    // that the validated standard boundary reaches both existing codecs.
    auto bytes = motion.bytes;
    Bytes payload{'x'}, block; std::string error;
    Check(morph_curve::EncodeVmdBlock(payload, block, error), "encode bounded optional morph block");
    bytes.insert(bytes.end(), block.begin(), block.end());
    bone_scale_io::PrefixFingerprint fingerprint; fingerprint.Append(bytes.data(), bytes.size());
    bone_scale_io::VmdExtension extension; extension.prefixBytes = fingerprint.bytes(); extension.prefixCrc32 = fingerprint.crc32();
    bone_scale_io::VmdExtension::Entry entry; std::memcpy(entry.name.data(), "bone", 4);
    entry.scale.v[0] = entry.scale.v[4] = entry.scale.v[8] = 2;
    extension.entries.push_back(entry);
    const auto encoded = bone_scale_io::EncodeExtension(extension);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end()); Put(bytes, std::uint64_t(encoded.size())); Text(bytes, "MMSXVMDX", 8);
    Fixture fixture; fixture.Imported(bytes);
    Check(bone_scale::Key(fixture.Raw(), 0).v[0] == 2, "scale extension follows exact morph-block boundary");
#endif
}
void Camera() {
    for (int version : {1, 2}) {
        Fixture fixture(true); Motion motion(version, true);
        fixture.EnableAccessory();
        // Real camera exports have a zero display/IK count before the codec
        // tail. The loader must consume that end code to find MMSXACC1.
        motion.bytes.resize(motion.counts.back()); U32(motion.bytes, 0);
        accessory_motion_io::Track track; track.name = "prop.x";
        accessory_motion_io::Key first; first.frame = 0; first.position[0] = 2.5f; first.position[1] = -3;
        first.rotation[2] = .25f; first.scale = 2; first.opacity = .375f; first.shadowEnabled = 0;
        auto second = first; second.frame = 10; second.position[0] = 6; second.visible = 0; second.shadowEnabled = 1;
        track.keys = {first, second};
        const auto tail = accessory_motion_io::Encode({track});
        motion.bytes.insert(motion.bytes.end(), tail.begin(), tail.end());
        Check(fixture.Load(motion.bytes) == 0, "camera path retains its historical return value");
        Check(fixture.cameraKeys[0].distance == -25 && fixture.lightKeys[0].color[0] == .2f, "camera and light production registrars import records");
        Check(fixture.cameraKeys[0].fov == (version == 1 ? 45 : 50), "old camera shared curve layout and new camera fov remain distinct");
        if (version == 2) Check(fixture.shadowKeys[0].mode == 2, "new camera path imports self shadow");
        Check(fixture.model->undoState[0] == 4, "camera load does not modify model undo");
        const auto& head = fixture.accessoryKeys[0];
        Check(head.selected && head.position[0] == first.position[0] && head.scale == first.scale && head.opacity == first.opacity,
              "camera production entry imports accessory head after the standard end code");
        Check(head.next > 0 && head.next < fixture.accessoryKeys.size(), "accessory tail creates a second linked key");
        const auto& next = fixture.accessoryKeys[head.next];
        Check(next.frame == 10 && next.visible == 0 && next.shadowEnabled == 1 && next.position[0] == second.position[0],
              "accessory second key preserves frame and payload through the real registrar");
        Check(fixture.accessory->position[0] == first.position[0] && fixture.accessory->opacity == first.opacity,
              "accessory imported track updates the current scene pose");
        std::uint32_t minimum = 0xffffffffu;
        const auto captured = accessory_motion_io::CaptureSelected(fixture.app.get(), minimum);
        Check(minimum == 0 && captured.size() == 1 && accessory_motion_io::Encode(captured) == tail,
              "accessory tail roundtrips from codec through production load and native key capture");
    }
}
void Ik() {
    for (bool longEnabled : {false, true}) {
        Fixture fixture; fixture.EnableIk(); Motion motion;
        motion.bytes.resize(motion.counts.back()); U32(motion.bytes, 1);
        U32(motion.bytes, 0); motion.bytes.push_back(1); U32(motion.bytes, 4);
        // The unknown full-width entry is followed immediately by a valid
        // short name. Searches must advance by 21 bytes without mixing names.
        Text(motion.bytes, "01234567890123456789", 20); motion.bytes.push_back(1);
        Text(motion.bytes, "second", 20); motion.bytes.push_back(0);
        Text(motion.bytes, "bone", 20); motion.bytes.push_back(1);
        Text(motion.bytes, "abcdefghijklmnopqrst", 20); motion.bytes.push_back(longEnabled ? 1 : 0);
        // Start with the opposite flag so a missed name cannot pass by chance.
        fixture.displayKeys[0].ikStates[0] = longEnabled ? 0 : 1;
        fixture.Imported(motion.bytes);
        const auto* flags = fixture.displayKeys[0].ikStates;
        Check(flags[1] == 1 && flags[2] == 0, "consecutive IK entries map by name to reordered chains");
        Check(flags[0] == (longEnabled ? 1 : 0), "a full 20-byte IK name matches for both enable and disable");
        Check(fixture.ikChains[0].enabled == flags[0] && fixture.ikChains[1].enabled == flags[1] && fixture.ikChains[2].enabled == flags[2],
              "SeekModelFrame applies imported display flags to real IK chains");
    }
}

constexpr std::uint32_t kBenchmarkKeyCount = 1000;
std::array<float, 3> BenchmarkPosition(std::uint32_t frame) {
    return {{1.f + static_cast<float>(frame) * .125f,
             -static_cast<float>(frame % 17) * .25f,
             static_cast<float>(frame % 13) * .5f}};
}
Bytes BenchmarkMotion() {
    Bytes bytes;
    bytes.reserve(74 + 111 * kBenchmarkKeyCount);
    Text(bytes, "Vocaloid Motion Data 0002", 30); Text(bytes, "sample", 20);
    U32(bytes, kBenchmarkKeyCount);
    for (std::uint32_t frame = 0; frame < kBenchmarkKeyCount; ++frame) {
        Text(bytes, "bone", 15); U32(bytes, frame);
        for (float value : BenchmarkPosition(frame)) Put(bytes, value);
        for (float value : {0.f, 0.f, 0.f, 1.f}) Put(bytes, value);
        bytes.resize(bytes.size() + 64, 20);
    }
    // Keep this ordinary VMD valid in both production revisions. Empty morph,
    // camera, light, shadow, and display sections avoid the repaired boundaries.
    for (int section = 0; section < 5; ++section) U32(bytes, 0);
    return bytes;
}
void ParallelMotion() {
    constexpr std::uint32_t keyCount = 16384;
    Fixture fixture;
    Bytes bytes;
    Text(bytes, "Vocaloid Motion Data 0002", 30);
    Text(bytes, "sample", 20);
    U32(bytes, keyCount + 1);
    const auto firstBonePosition = bytes.size() + 15 + 4;
    auto bone = [&](std::uint32_t frame, float x) {
        Text(bytes, "bone", 15);
        U32(bytes, frame);
        for (float value : {x, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f}) Put(bytes, value);
        bytes.resize(bytes.size() + 64, 20);
    };
    for (auto frame = keyCount; frame > 0; --frame) bone(frame - 1, float(frame - 1));
    bone(keyCount / 2, -7.f); // A duplicate in the later range takes precedence.
    U32(bytes, keyCount + 1);
    auto morph = [&](std::uint32_t frame, float value) {
        Text(bytes, "face", 15);
        U32(bytes, frame);
        Put(bytes, value);
    };
    for (std::uint32_t frame = 0; frame < keyCount; ++frame) morph(frame, .25f);
    morph(keyCount / 2, .75f);
    const auto endOfMorphSection = bytes.size();
    for (int section = 0; section < 4; ++section) U32(bytes, 0);

    Check(fixture.Load(bytes) == 1, "large mixed-order motion imports");
    std::uint32_t boneSlot = 0, morphSlot = 1, previousBone = 0, previousMorph = 0;
    for (std::uint32_t frame = 0; frame < keyCount; ++frame) {
        Check(boneSlot < fixture.boneKeys.size() && morphSlot < fixture.morphKeys.size(),
              "parallel import leaves key indices in range");
        const auto& boneKey = fixture.boneKeys[boneSlot];
        const auto& morphKey = fixture.morphKeys[morphSlot];
        Check(boneKey.frame == frame && boneKey.position[0] == (frame == keyCount / 2 ? -7.f : float(frame)) &&
                  boneKey.previous == previousBone && boneKey.allocated,
              "large descending bone import preserves links and the last duplicate");
        Check(morphKey.frame == frame && morphKey.value == (frame == keyCount / 2 ? .75f : .25f) &&
                  morphKey.previous == previousMorph && morphKey.allocated,
              "large ascending morph import preserves links and the last duplicate");
        previousBone = boneSlot;
        previousMorph = morphSlot;
        boneSlot = boneKey.next;
        morphSlot = morphKey.next;
    }
    Check(!boneSlot && !morphSlot, "large motion inserts each distinct frame once");

    const auto imported = fixture.State();
    const float replacement = -99.f;
    std::memcpy(bytes.data() + firstBonePosition, &replacement, sizeof(replacement));
    bytes.resize(endOfMorphSection - 1);
    fixture.Rejected(bytes, imported);
}
std::uint64_t BenchmarkChecksum(const Fixture& fixture) {
    // FNV-1a over imported values, with fixed little-endian scalar encoding.
    // Do not hash pointers, pool indices, padding, or undo ring positions.
    std::uint64_t checksum = 14695981039346656037ull;
    const auto byte = [&](std::uint8_t value) { checksum = (checksum ^ value) * 1099511628211ull; };
    const auto word = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) byte(static_cast<std::uint8_t>(value >> shift));
    };
    const auto scalar = [&](float value) {
        std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); word(bits);
    };
    std::uint32_t index = 0, previous = 0;
    for (std::uint32_t frame = 0; frame < kBenchmarkKeyCount; ++frame) {
        Check(index < fixture.boneKeys.size(), "benchmark key chain stays within the fixture pool");
        const auto& key = fixture.boneKeys[index];
        Check(key.frame == frame && key.previous == previous && key.allocated == 1,
              "benchmark imports every ordered bone key with intact links");
        word(key.frame);
        const auto expected = BenchmarkPosition(frame);
        for (std::size_t component = 0; component < expected.size(); ++component) {
            Check(key.position[component] == expected[component], "benchmark imports each position component");
            scalar(key.position[component]);
        }
        for (std::size_t component = 0; component < 4; ++component) {
            Check(key.rotation[component] == (component == 3 ? 1.f : 0.f), "benchmark imports identity rotations");
            scalar(key.rotation[component]);
        }
        for (auto value : key.interpolation) {
            Check(value == 20, "benchmark imports all interpolation controls"); byte(value);
        }
        Check(key.physicsDisabled == 0, "benchmark preserves the ordinary physics flag"); byte(key.physicsDisabled);
        if (frame + 1 == kBenchmarkKeyCount)
            Check(key.next == 0, "benchmark imports exactly the requested key count");
        else
            Check(key.next != 0, "benchmark key chain includes the next frame");
        previous = index; index = key.next;
    }
    Check(fixture.model->maxFrame == kBenchmarkKeyCount - 1, "benchmark updates the model timeline extent");
    return checksum;
}
unsigned BenchmarkCount(const char* text) {
    unsigned count = 0;
    if (*text == 0) return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || count > 10000) return 0;
        count = count * 10 + static_cast<unsigned>(*text - '0');
    }
    return count <= 100000 ? count : 0;
}
void Benchmark(unsigned warmup, unsigned iterations) {
    // Construct the fixture and write the file once, before any measured load.
    // Warmup creates the chain, so every timed load overwrites the same keys.
    Fixture fixture;
    const auto motion = BenchmarkMotion(); fixture.file.Write(motion);
    std::vector<double> samples(iterations);
    std::uint64_t checksum = 0;
    for (unsigned i = 0; i < warmup; ++i) {
        Check(LoadVmdMotion(fixture.app.get(), fixture.file.path) == 1, "benchmark warmup completes the real model load");
        const auto actual = BenchmarkChecksum(fixture);
        if (i != 0) Check(actual == checksum, "benchmark warmup preserves imported values");
        checksum = actual;
    }
    double elapsed = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const int result = LoadVmdMotion(fixture.app.get(), fixture.file.path);
        const auto end = std::chrono::steady_clock::now();
        samples[i] = std::chrono::duration<double, std::milli>(end - start).count();
        elapsed += samples[i];
        Check(result == 1, "benchmark measured load completes through the production entry");
        Check(BenchmarkChecksum(fixture) == checksum, "benchmark measured load preserves every imported value");
    }
    std::printf("{\"benchmark\":\"vmd_repeat_import\",\"warmup\":%u,\"iterations\":%u,"
                "\"file_bytes\":%zu,\"key_count\":%u,\"elapsed_ms\":%.6f,\"mean_ms\":%.6f,"
                "\"checksum\":\"%016llx\",\"samples_ms\":[",
                warmup, iterations, motion.size(), static_cast<unsigned>(kBenchmarkKeyCount),
                elapsed, elapsed / iterations, static_cast<unsigned long long>(checksum));
    for (unsigned i = 0; i < iterations; ++i) std::printf("%s%.6f", i ? "," : "", samples[i]);
    std::printf("]}\n");
}
} // namespace

void UnicodeIdentity() {
    using namespace mikumikustudio;
    Fixture fixture;fixture.EnableIk();
    const std::wstring first=L"相同的长名称前缀_左侧_\U0001f31f",second=L"相同的长名称前缀_右侧_\U0001f31f";
    Check(extended_names::Assign(fixture.bones[0].jpText,first)&&extended_names::Assign(fixture.bones[1].jpText,second),"allocate complete names");
    std::strcpy(fixture.bones[0].name,"same-prefix");std::strcpy(fixture.bones[1].name,"same-prefix");
    Motion motion;
    extended_names::MotionNames names;names.model=L"sample";names.bones={first,second};names.morphs={L"face"};names.boneOwners={1};names.morphOwners={0};names.ikOwners={1};
    bone_scale_io::PrefixFingerprint fingerprint;fingerprint.Append(motion.bytes.data(),motion.bytes.size());
    auto block=extended_names::EncodeMotion(names,fingerprint.bytes(),fingerprint.crc32());
    motion.bytes.insert(motion.bytes.end(),block.begin(),block.end());
    const auto nameBlockEnd=motion.bytes.size();
    bone_scale_io::PrefixFingerprint scaleFingerprint;scaleFingerprint.Append(motion.bytes.data(),motion.bytes.size());
    bone_scale_io::VmdExtension scales;scales.prefixBytes=scaleFingerprint.bytes();scales.prefixCrc32=scaleFingerprint.crc32();
    bone_scale_io::VmdExtension::Entry scale;scale.name=extended_names::MotionIndexName(1);scale.frame=0;scale.scale.v[0]=2.f;scales.entries.push_back(scale);
    const auto scaleBytes=bone_scale_io::EncodeExtension(scales);
    motion.bytes.insert(motion.bytes.end(),scaleBytes.begin(),scaleBytes.end());Put(motion.bytes,std::uint64_t(scaleBytes.size()));Text(motion.bytes,"MMSXVMDX",8);
    Check(fixture.Load(motion.bytes)==int(fixture.model->boneCount),"load Unicode motion names");
    Check(fixture.boneKeys[1].position[0]==7.f&&fixture.boneKeys[0].position[0]==-3.f,"long names with the same legacy prefix address distinct bones");
    Check(fixture.displayKeys[0].ikStates[1]==1&&fixture.displayKeys[0].ikStates[2]==0,"Unicode IK names address the correct chain");
    Check(bone_scale::Key(fixture.Raw(),1).v[0]==2.f&&bone_scale::Key(fixture.Raw(),0).v[0]==1.f,"Unicode scale extension targets the same complete bone name");
    wchar_t savedPath[MAX_PATH]{};MultiByteToWideChar(CP_ACP,0,fixture.file.path,-1,savedPath,MAX_PATH);
    SaveVmdFile(savedPath);
    fixture.boneKeys[1].position[0]=-9.f;
    bone_scale::SetKey(fixture.Raw(),1,bone_scale::Matrix{});
    Check(LoadVmdMotion(fixture.app.get(),fixture.file.path)==int(fixture.model->boneCount),"real Unicode VMD writer roundtrip");
    Check(fixture.boneKeys[1].position[0]==7.f && fixture.boneKeys[0].position[0]==-3.f,
          "saved Unicode VMD retains distinct long-name owners");
    Check(bone_scale::Key(fixture.Raw(),1).v[0]==2.f,"real Unicode VMD writer preserves scale identity");
    const auto before=fixture.State();motion.bytes[nameBlockEnd-1]^=1;
    Check(fixture.Load(motion.bytes)==0&&fixture.State()==before,"damaged Unicode metadata refuses import before editing keys");
    std::free(fixture.bones[0].jpText);fixture.bones[0].jpText=nullptr;
    std::free(fixture.bones[1].jpText);fixture.bones[1].jpText=nullptr;
}

#if !MMD_ORIGINAL_PROFILE
#include "vmd_curve_compat_checks.inc"

namespace {
bool CheckDuplicateBoneVariant(const char* variant, const char* legacyName, wchar_t* completeName) {
    constexpr std::uint32_t frame = 12;
    constexpr std::array<float, 3> translations{4.f, -7.f, 11.f};
    const bool pmx = completeName != nullptr;
    std::array<mdl::PmdVertex, 2> pmdVertices{};
    pmdVertices[1].bone[0] = 1;
    pmdVertices[1].weightPercent = pmdVertices[0].weightPercent = 100;
    std::array<mdl::PmxVertex, 2> pmxVertices{};
    pmxVertices[1].bone[0] = 1;
    bool passed = true;
    const auto expect = [&](bool condition, const char* description) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: duplicate-bones (%s): %s\n", variant, description);
            passed = false;
        }
    };
    const auto prepare = [&](Fixture& fixture) {
        fixture.model->boneCount = static_cast<std::uint32_t>(fixture.bones.size());
        fixture.model->vertexCount = 2;
        fixture.model->physicsMode = pmx ? 2 : 0;
        fixture.model->pmxVertices = pmx ? pmxVertices.data() : nullptr;
        fixture.model->rawVertices = pmx ? nullptr : pmdVertices.data();
        for (std::size_t i = 0; i < fixture.bones.size(); ++i) {
            auto& bone = fixture.bones[i];
            bone.parent = -1;
            bone.slotIndex = -1;
            bone.type = mdl::BoneType::RotateMove;
            bone.rotQuat[3] = 1.f;
            bone.position[0] = static_cast<float>(i);
            fixture.boneKeys[i] = {};
            fixture.boneKeys[i].rotation[3] = 1.f;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            fixture.bones[i].jpText = completeName;
            std::strcpy(fixture.bones[i].name, legacyName);
        }
        std::strcpy(fixture.bones[2].name, "control");
        fixture.boneKeys.back().allocated = 0;
        fixture.morphKeys.back().allocated = 0;
        fixture.displayKeys.back().allocated = 0;
    };
    const auto seedTrack = [&](Fixture& fixture, std::uint32_t bone, float translation) {
        auto& head = fixture.boneKeys[bone];
        head.allocated = 1;
        head.next = fixture.model->boneCount + bone;
        auto& key = fixture.boneKeys[head.next];
        key = head;
        key.next = 0;
        key.previous = bone;
        key.frame = frame;
        key.position[0] = translation;
        fixture.model->maxFrame = frame;
    };
    const auto matchesTrack = [&](const Fixture& target, std::size_t bone, float translation, bool imported) {
        const auto& head = target.boneKeys[bone];
        const auto index = head.next;
        if ((head.allocated != 0) != imported || head.frame != 0 || head.position[0] != 0.f ||
            index < target.model->boneCount || index >= target.boneKeys.size()) return false;
        const auto& key = target.boneKeys[index];
        return key.frame == frame && key.previous == bone && key.next == 0 &&
               (key.allocated != 0) == imported && key.position[0] == translation;
    };
    const auto emptyTrack = [](const Fixture& target, std::size_t bone) {
        const auto& head = target.boneKeys[bone];
        return !head.allocated && !head.next && head.position[0] == 0.f;
    };

    Fixture source;
    prepare(source);
    // Each track has a frame-0 head and a frame-12 key. The shared frame
    // makes a collision between the two duplicate tracks observable.
    for (std::uint32_t i = 0; i < source.model->boneCount; ++i) {
        seedTrack(source, i, translations[i]);
    }
    bone_scale::Matrix scale0, scale1;
    scale0.v[0] = 1.75f;
    scale1.v[0] = 2.25f;
    bone_scale::SetKey(source.Raw(), source.boneKeys[0].next, scale0);
    bone_scale::SetKey(source.Raw(), source.boneKeys[1].next, scale1);
    wchar_t savedPath[MAX_PATH]{};
    Check(MultiByteToWideChar(CP_ACP, 0, source.file.path, -1, savedPath, MAX_PATH) != 0,
          "convert duplicate-bone VMD fixture path");
    SaveVmdFile(savedPath);

    {
        std::ifstream input(source.file.path, std::ios::binary);
        Bytes bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        constexpr char marker[] = "MMSNAME2";
        const auto block = std::search(bytes.begin(), bytes.end(), marker, marker + 8);
        Check(block != bytes.end(), "same-name VMD publishes the checksummed rig extension");
        Check(std::memcmp(bytes.data() + 54, legacyName, std::strlen(legacyName)) == 0,
              "same-name VMD keeps the standard bone name bytes");
        const auto offset = static_cast<std::size_t>(block - bytes.begin());
        extended_names::MotionNames names;
        const auto size = extended_names::DecodeMotion(bytes.data(), bytes.size(), offset, names);
        Check(size && names.boneRig, "read the source rig fingerprint from the written VMD");
        bytes[offset + size - 1] ^= 1;
        Fixture target;
        prepare(target);
        const auto before = target.State();
        expect(target.Load(bytes) == 0 && target.State() == before,
               "a damaged rig fingerprint refuses the entire import before editing keys");
    }

    const auto load = [&](Fixture& target, const char* description) {
        expect(LoadVmdMotion(target.app.get(), source.file.path) == int(target.model->boneCount), description);
    };
    {
        Fixture target;
        prepare(target);
        load(target, "load on a fresh copy of the source rig");
        expect(matchesTrack(target, 0, translations[0], true), "same rig restores duplicate track 0");
        expect(matchesTrack(target, 1, translations[1], true), "same rig restores duplicate track 1");
        expect(matchesTrack(target, 2, translations[2], true), "same rig restores the unique control track");
        expect(bone_scale::Key(target.Raw(), target.boneKeys[0].next) == scale0 &&
               bone_scale::Key(target.Raw(), target.boneKeys[1].next) == scale1,
               "scale extension restores distinct values on both duplicate tracks");
    }
    {
        Fixture target;
        prepare(target);
        // Keep the model name and ordered bone names identical while changing
        // both duplicate bones' rest positions.
        target.bones[0].position[0] = 20.f;
        target.bones[1].position[0] = 21.f;
        load(target, "load on a rig with the same names and different rest positions");
        expect(emptyTrack(target, 0) && emptyTrack(target, 1), "different rig skips both duplicate tracks");
        expect(matchesTrack(target, 2, translations[2], true), "different rig restores the unique control track");
    }
    {
        auto otherPmdVertices = pmdVertices;
        auto otherPmxVertices = pmxVertices;
        otherPmdVertices[0].bone[0] = 1;
        otherPmxVertices[0].bone[0] = 1;
        Fixture target;
        prepare(target);
        target.model->rawVertices = pmx ? nullptr : otherPmdVertices.data();
        target.model->pmxVertices = pmx ? otherPmxVertices.data() : nullptr;
        load(target, "load on a rig with only a changed vertex binding");
        expect(emptyTrack(target, 0) && emptyTrack(target, 1), "changed bindings skip both duplicate tracks");
        expect(matchesTrack(target, 2, translations[2], true), "changed bindings still allow the unique track");
    }
    {
        Fixture target;
        prepare(target);
        target.bones[1].jpText = nullptr;
        std::strcpy(target.bones[1].name, "other");
        constexpr float existingTranslation = 42.f;
        seedTrack(target, 0, existingTranslation);
        load(target, "load on a rig with just one destination matching the duplicate name");
        // Import clears the selection marks even on keys it leaves intact.
        expect(matchesTrack(target, 0, existingTranslation, false), "ambiguous source tracks leave the unique destination intact");
        expect(emptyTrack(target, 1), "renamed bone receives no keys");
        expect(matchesTrack(target, 2, translations[2], true), "unique-destination rig restores the unique control track");
    }
    return passed;
}

void DuplicateBoneIdentity() {
    wchar_t unicodeName[] = L"same complete bone name \U0001f31f";
    const bool unicode = CheckDuplicateBoneVariant("Unicode", "same-bone", unicodeName);
    const bool cp932 = CheckDuplicateBoneVariant("CP932", "\x93\xaf\x96\xbc", nullptr); // 同名
    if (!unicode || !cp932) std::exit(1);
}

void DuplicateMorphIdentity() {
    struct Model {
        Fixture fixture;
        std::array<mdl::MorphRecord, 3> morphs{};
        std::array<mdl::PmdVertexMorphEntry, 3> offsets{};

        explicit Model(bool pmx, wchar_t* completeName) {
            fixture.model->physicsMode = pmx ? 2 : 0;
            fixture.model->morphCount = static_cast<std::uint32_t>(morphs.size());
            fixture.model->morphs = morphs.data();
            fixture.morphKeys[1] = fixture.morphKeys.back() = {};
            std::strcpy(morphs[0].name, "control");
            for (unsigned i = 1; i < morphs.size(); ++i) {
                auto& morph = morphs[i];
                std::strcpy(morph.name, "\x93\xaf\x96\xbc"); // CP932 same-name token
                morph.jpText = completeName;
                morph.panel = mdl::MorphPanel::eye;
                morph.type = pmx ? 1 : 0;
                morph.offsetCount = 1;
                morph.vertexEntries = &offsets[i];
                offsets[i].vertexIndex = i - 1;
                offsets[i].offset[0] = static_cast<float>(i);
            }
        }

        void Seed(unsigned owner, float value) {
            auto& root = fixture.morphKeys[owner];
            root.next = fixture.model->morphCount + owner;
            auto& key = fixture.morphKeys[root.next];
            key.previous = owner;
            key.frame = 12;
            key.value = value;
            key.allocated = 1;
            fixture.model->maxFrame = key.frame;
        }

        bool Has(unsigned owner, float value) const {
            const auto next = fixture.morphKeys[owner].next;
            if (next < fixture.model->morphCount || next >= fixture.morphKeys.size()) return false;
            const auto& key = fixture.morphKeys[next];
            return key.previous == owner && key.frame == 12 && key.value == value;
        }
    };

    wchar_t unicode[] = L"same complete morph name \U0001f31f";
    for (bool pmx : {false, true}) {
        namespace curve = animation_curve;
        auto* completeName = pmx ? unicode : nullptr;
        Model source(pmx, completeName);
        source.Seed(0, .5f);
        source.Seed(1, .25f);
        source.Seed(2, .75f);
        std::vector<curve::NamedTrack> authored;
        for (unsigned owner : {1u, 2u}) {
            auto& track = authored.emplace_back();
            track.id = morph_curve::StableId(owner, source.morphs[owner].name);
            track.track.keys = {{0, 0}, {12, owner == 1 ? .25 : .75}};
            std::string error;
            const std::array<double, 4> controls = owner == 1 ? std::array<double, 4>{20, 80, 30, 100} :
                                                               std::array<double, 4>{18, 5, 110, 30};
            Check(curve::SetMmdControls(track.track, 1, controls, error), "give each duplicate its own Bezier ease");
        }
        std::string error;
        Check(morph_curve::Write(source.fixture.Raw(), authored, error), "attach authored curves to both duplicate tracks");
        Bytes curveBlock;
        const bool captured = morph_curve::CaptureVmd(source.fixture.Raw(), 0, curveBlock, error);
        if (!captured) std::fprintf(stderr, "duplicate-morphs: capture failed: %s\n", error.c_str());
        Check(captured && !curveBlock.empty(), "capture both authored curves for VMD");
        wchar_t path[MAX_PATH]{};
        Check(MultiByteToWideChar(CP_ACP, 0, source.fixture.file.path, -1, path, MAX_PATH) != 0,
              "convert duplicate-morph VMD fixture path");
        SaveVmdFile(path);
        Bytes bytes;
        {
            std::ifstream file(source.fixture.file.path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(file), {});
        }
        constexpr char marker[] = "MMSNAME3";
        const auto magic = std::search(bytes.begin(), bytes.end(), marker, marker + 8);
        Check(magic != bytes.end(), "the VMD writes a checksummed morph table fingerprint");
        const auto offset = static_cast<std::size_t>(magic - bytes.begin());
        extended_names::MotionNames names;
        const auto length = extended_names::DecodeMotion(bytes.data(), bytes.size(), offset, names);
        Check(length && names.morphTable && !names.boneRig, "decode the morph-only fingerprint variant");
        if (!pmx) {
            std::uint32_t bones = 0, morphs = 0;
            std::memcpy(&bones, bytes.data() + 50, 4);
            const auto morphCountOffset = 54 + std::size_t(bones) * 111;
            Check(morphCountOffset + 4 <= offset, "locate the standard morph section");
            std::memcpy(&morphs, bytes.data() + morphCountOffset, 4);
            Check(morphs == 5 && morphCountOffset + 4 + morphs * 23 <= offset, "keep all five standard morph keys");
            unsigned duplicateKeys = 0;
            for (unsigned i = 0; i < morphs; ++i) {
                const auto* name = bytes.data() + morphCountOffset + 4 + i * 23;
                duplicateKeys += std::memcmp(name, "\x93\xaf\x96\xbc", 4) == 0;
            }
            Check(duplicateKeys == 4, "same-name VMD keeps the standard CP932 morph name bytes");
        }
        {
            Model target(pmx, completeName);
            Check(LoadVmdMotion(target.fixture.app.get(), source.fixture.file.path) == 1,
                  "load the saved VMD into a fresh matching model");
            Check(target.Has(0, .5f) && target.Has(1, .25f) && target.Has(2, .75f),
                  pmx ? "Unicode duplicate morphs roundtrip on separate tracks" :
                        "CP932 duplicate morphs roundtrip on separate tracks");
            for (unsigned owner : {1u, 2u}) {
                curve::Track track;
                Check(morph_curve::Read(target.fixture.Raw(), owner, track, error) && track.keys.size() == 2 &&
                      track.keys[1].interpolation == curve::Interpolation::Bezier &&
                      std::abs(curve::Evaluate(track, 6).value - curve::Evaluate(authored[owner - 1].track, 6).value) < 1e-7,
                      "the saved Bezier ease is restored on the intended duplicate track");
            }
        }
        {
            Model target(pmx, completeName);
            std::swap(target.offsets[1], target.offsets[2]);
            target.Seed(1, .125f);
            Check(LoadVmdMotion(target.fixture.app.get(), source.fixture.file.path) == 1,
                  "load the saved VMD into a different morph table with the same ordered names");
            Check(target.Has(0, .5f) && target.Has(1, .125f) && target.fixture.morphKeys[2].next == 0,
                  "reordered same-name definitions preserve the target and still import the unique control");
        }
        {
            Model target(pmx, completeName);
            bytes[offset + length - 1] ^= 1;
            const auto before = target.fixture.State();
            Check(target.fixture.Load(bytes) == 0 && target.fixture.State() == before,
                  "a damaged fingerprint rejects the entire import without editing keys");
        }
    }
}
} // namespace
#endif

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 4 && std::strcmp(argv[1], "--standard-model-exchange") == 0) {
        StandardModelExchange(argv[2], argv[3]);
        return 0;
    }
    // TempFile verification switches run before the benchmark/case handling so
    // the original ordering below is untouched.
    if (argc >= 2 && std::strcmp(argv[1], "--tempfile-probe") == 0) return RunTempFileProbe();
    if (argc >= 2 && std::strcmp(argv[1], "--tempfile-probe-all-fail") == 0) return RunTempFileProbeAllFail();
    if (argc >= 2 && std::strcmp(argv[1], "--benchmark") == 0) {
        const unsigned warmup = argc == 2 ? 3 : argc == 4 ? BenchmarkCount(argv[2]) : 0;
        const unsigned iterations = argc == 2 ? 20 : argc == 4 ? BenchmarkCount(argv[3]) : 0;
        if (!warmup || !iterations) {
            std::fprintf(stderr, "Usage: --benchmark [warmup iterations], each count in 1..100000\n");
            return 2;
        }
        Benchmark(warmup, iterations); return 0;
    }
    struct Case { const char* name; void (*run)(); };
    const Case cases[]{
#if !MMD_ORIGINAL_PROFILE
        {"duplicate-bones", DuplicateBoneIdentity},
        {"duplicate-morphs", DuplicateMorphIdentity},
#endif
        {"parallel", ParallelMotion},
#if !MMD_ORIGINAL_PROFILE
        {"path-motion", UnicodeMotionPaths}, {"path-camera", UnicodeCameraPaths}, {"unicode", UnicodeIdentity}, {"overflow", Overflow}, {"short", ShortReads}, {"counts", Counts}, {"valid", Valid}, {"dialog-enter", SelectionAfterModelConfirmation}, {"sampler-focus", SamplerFocusRegression}, {"tails", Tails}, {"camera", Camera}, {"curve-compat", NativeCameraCurveCompatibility}, {"ik", Ik}, {"twist", FixedAxisRotations}, {"force-morph", ForceMorphPaste},
#endif
    };
    if (argc == 3 && std::strcmp(argv[1], "--case") == 0) {
        for (const auto& test : cases) if (std::strcmp(argv[2], test.name) == 0) { test.run(); std::printf("VMD %s: passed\n", test.name); return 0; }
        return 2;
    }
    for (const auto& test : cases) { test.run(); std::printf("VMD %s: passed\n", test.name); }
    return 0;
}
