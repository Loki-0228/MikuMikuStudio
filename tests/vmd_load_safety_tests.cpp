// MAINTENANCE: regression-keep; VMD parsing, import selection, and modal input isolation.
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/build_profile.hpp"
// Exercise the real LoadVmdMotion entry, key registrars, and extension loaders.
// The dialog-enter case uses an inactive private desktop for real modal dialogs;
// the other cases create no windows. No case creates a D3D device.
// vmd_load_safety_tests.exe --case overflow|short|counts|valid|dialog-enter|tails|camera|ik|twist|force-morph
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
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
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
#include "mikumikustudio/keyboard_input.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/force_paste.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <limits>

namespace mikumikustudio {
int LoadVmdMotion(MMDApp*, const char*);
void MouseInteractionBegin(MMDApp*);
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

struct TempFile {
    char path[MAX_PATH]{};
    TempFile() {
        // TEMP is not writable in every runner (a restricted token can deny
        // creation there while still allowing the test's own directory), so
        // try the session temp path first and fall back beside the binary.
        char directories[2][MAX_PATH]{};
        char module[MAX_PATH]{};
        const DWORD moduleLength = GetModuleFileNameA(nullptr, module, MAX_PATH);
        if (moduleLength != 0 && moduleLength < MAX_PATH) {
            char* slash = std::strrchr(module, '\\');
            if (slash != nullptr) *(slash + 1) = '\0';
            std::strncpy(directories[1], module, MAX_PATH - 1);
        }
        DWORD tempLength = GetTempPathA(MAX_PATH, directories[0]);
        if (tempLength == 0 || tempLength >= MAX_PATH) directories[0][0] = '\0';
        const DWORD processId = GetCurrentProcessId();
        for (const char* directory : directories) {
            if (directory[0] == '\0') continue;
            for (unsigned attempt = 0; attempt < 64; ++attempt) {
                const int written = std::snprintf(
                    path, MAX_PATH, "%svmd-test-%lu-%u-%u.tmp",
                    directory, processId, GetTickCount(), attempt);
                if (written <= 0 || written >= MAX_PATH) break;
                const int fd = _open(path, _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE);
                if (fd >= 0) {
                    _close(fd);
                    return;
                }
                if (errno != EEXIST) break;
            }
        }
        path[0] = '\0';
        Check(false, "create temporary motion");
    }
    ~TempFile() { DeleteFileA(path); }
    void Write(const Bytes& bytes) {
        int fd = -1;
        Check(_sopen_s(&fd, path, _O_BINARY | _O_WRONLY | _O_TRUNC, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0, "open temporary motion");
        Check(bytes.empty() || _write(fd, bytes.data(), static_cast<unsigned>(bytes.size())) == static_cast<int>(bytes.size()), "write temporary motion");
        _close(fd);
    }
};

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
        for (auto& ring : model->undoRings) for (auto& slot : ring.slots) { std::free(slot.bonePose); std::free(slot.auxiliaryPose); }
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

void UnicodePaths(bool camera) {
    namespace fs = std::filesystem;
    const std::wstring previousLocale = _wsetlocale(LC_ALL, nullptr);
    const auto root = fs::temp_directory_path() /
        (L"MMS-vmd-path-" + std::to_wstring(GetCurrentProcessId()));
    Check(fs::create_directory(root), "create Unicode path test root");
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
                    Text(motion.bytes, "MDSXVMDX", 8);
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

void SetSelectionTestEnter(bool held) {
    // This thread runs on an inactive desktop. Its keyboard table drives the
    // real poll without sending input or changing the user's foreground window.
    BYTE keys[256]{};
    keys[VK_RETURN] = held ? 0x80 : 0;
    Check(SetKeyboardState(keys) != FALSE, "set private-desktop keyboard state");
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
        HWND owner = CreateWindowW(windowClass.lpszClassName, L"VMD selection test",
            WS_OVERLAPPED, 0, 0, 400, 300, nullptr, nullptr, windowClass.hInstance, nullptr);
        Check(owner != nullptr, "create private VMD input owner");
        state.owner = owner;
        fixture.app->Hwnd() = owner; fixture.model->hwnd = owner;
        SetSelectionTestEnter(false);
        // Dropped VMDs enter the loader with no menu's Enter latch. An Enter
        // used to accept the mismatch must not become command 500 afterwards.
        fixture.app->state.enterKeyState = 0;
        state.started = GetTickCount64();
        const UINT_PTR timer = SetTimer(owner, 1, 10, SelectionDialogTimer);
        Check(timer != 0, "schedule bounded VMD confirmation response");
        const int result = fixture.Load(motion);
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
            // The owner lives on an inactive private desktop, which can never
            // own the foreground. Stand the owner in for the live foreground so
            // this exercises the eligible branch the real drop path uses,
            // instead of asserting that an ineligible poll does nothing.
            mikumikustudio::SetPollKeyForegroundForTests(fixture.app.get(), owner);
            MouseInteractionBegin(fixture.app.get());
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
        SetSelectionTestEnter(false);
        DestroyWindow(owner);
        fixture.app->Hwnd() = nullptr; fixture.model->hwnd = nullptr;
        selectionDialogCase = nullptr;
    }
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

void Tails() {
    Motion motion;
    for (const Bytes& tail : {Bytes{1, 2, 3}, Bytes{'M','M','S','M','C','R','V','1', 16, 0, 0, 0}, Bytes{'M','D','S','X','M','D','S','X'}}) {
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
    bytes.insert(bytes.end(), encoded.begin(), encoded.end()); Put(bytes, std::uint64_t(encoded.size())); Text(bytes, "MDSXVMDX", 8);
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
    motion.bytes.insert(motion.bytes.end(),scaleBytes.begin(),scaleBytes.end());Put(motion.bytes,std::uint64_t(scaleBytes.size()));Text(motion.bytes,"MDSXVMDX",8);
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

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
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
    const Case cases[]{{"path-motion", UnicodeMotionPaths}, {"path-camera", UnicodeCameraPaths}, {"unicode", UnicodeIdentity}, {"overflow", Overflow}, {"short", ShortReads}, {"counts", Counts}, {"valid", Valid}, {"dialog-enter", SelectionAfterModelConfirmation}, {"tails", Tails}, {"camera", Camera}, {"ik", Ik}, {"twist", FixedAxisRotations}, {"force-morph", ForceMorphPaste}};
    if (argc == 3 && std::strcmp(argv[1], "--case") == 0) {
        for (const auto& test : cases) if (std::strcmp(argv[2], test.name) == 0) { test.run(); std::printf("VMD %s: passed\n", test.name); return 0; }
        return 2;
    }
    for (const auto& test : cases) { test.run(); std::printf("VMD %s: passed\n", test.name); }
    return 0;
}
