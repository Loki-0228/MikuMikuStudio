// Real standard-file entry points for the mmd932 profile. No D3D device or
// visible window. PMM alone needs hidden combo controls for its native counts.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <share.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/vpd_scale_io.hpp"
#include "../src/model/vmd_read_guard.hpp"

namespace {
using namespace mikumikustudio;
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void Require(bool ok, const char* message) {
    Check(ok, message);
    if (!ok) std::exit(2);
}
struct Files {
    fs::path directory = fs::temp_directory_path() /
        (L"MMS-original-io-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::vector<fs::path> created;
    Files() { Require(fs::create_directory(directory), "create owned test directory"); }
    fs::path Path(const wchar_t* name) { auto path = directory / name; created.push_back(path); return path; }
    ~Files() { for (const auto& path : created) fs::remove(path); fs::remove(directory); }
};
Bytes Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), {});
}
void Write(const fs::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Require(output.good(), "write owned fixture");
}
int Open(const fs::path& path, int flags) {
    int fd = -1;
    Require(_wsopen_s(&fd, path.c_str(), flags | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0,
            "open owned fixture by Unicode path");
    return fd;
}
template<class T> void Put(Bytes& bytes, const T& value) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof value);
}
std::uint32_t U32(const Bytes& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) { Check(false, "field is inside saved file"); return 0; }
    std::uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
}
vmd_read::Input Standard(const fs::path& path) {
    int fd = Open(path, _O_RDONLY);
    vmd_read::Input input;
    const bool valid = input.Load(fd); _close(fd);
    Require(valid, "production VMD validator accepts saved standard body");
    return input;
}
struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 1> bones{};
    std::array<mdl::MorphRecord, 2> morphs{};
    std::array<mdl::IkChain, 1> ik{};
    std::array<unsigned char, 1> selected{{1}}, physics{};
    std::vector<mdl::BoneKey> keys = std::vector<mdl::BoneKey>(mdl::kBoneKeyCapacity);
    std::vector<mdl::MorphKey> morphKeys = std::vector<mdl::MorphKey>(mdl::kMorphKeyCapacity);
    std::vector<mdl::DisplayKey> display = std::vector<mdl::DisplayKey>(mdl::kDisplayKeyCapacity);
    std::vector<mdl::CameraKey> camera = std::vector<mdl::CameraKey>(mdl::kTimelineKeyCapacity);
    std::vector<mdl::LightKey> light = std::vector<mdl::LightKey>(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadow = std::vector<mdl::SelfShadowKey>(mdl::kTimelineKeyCapacity);
    std::vector<mdl::GravityKey> gravity = std::vector<mdl::GravityKey>(mdl::kTimelineKeyCapacity);
    std::unique_ptr<mdl::AccessoryRecord> accessory = std::make_unique<mdl::AccessoryRecord>();
    std::vector<mdl::AccessoryKey> accessoryKeys = std::vector<mdl::AccessoryKey>(mdl::kTimelineKeyCapacity);
    MMDApp* old = g_Block;
    Fixture() {
        app->state = MMDAppState{};
        std::memset(renderer.get(), 0, sizeof *renderer); app->Renderer() = renderer.get();
        app->EnglishUI() = 1; app->ModelSlot(0) = Raw(); app->SetSelectedModelSlot(0);
        app->CameraKeys() = camera.data(); app->LightKeys() = light.data();
        app->ShadowKeys() = shadow.data(); app->GravityKeys() = gravity.data();
        model->boneCount = 1; model->boneTable = bones.data(); model->boneKeys = keys.data();
        model->morphCount = 2; model->morphs = morphs.data(); model->morphKeys = morphKeys.data();
        model->displayKeys = display.data(); model->boneSelection = selected.data(); model->bonePhysicsState = physics.data();
        model->ikChainCount = 1; model->ikChains = ik.data(); model->loadComplete = 1;
        std::strcpy(model->name, "sample"); std::strcpy(bones[0].name, "bone"); std::strcpy(morphs[1].name, "face");
        bones[0].parent = -1; bones[0].slotIndex = -1; bones[0].rotQuat[3] = 1;
        bones[0].type = mdl::BoneType::RotateMove; ik[0].targetBone = 0; ik[0].enabled = 1;
        keys[0].rotation[3] = 1; keys[0].next = 1;
        keys[1].allocated = 1; keys[1].frame = 40; keys[1].rotation[3] = 1; keys[1].position[0] = 7;
        morphKeys[1].next = 2; morphKeys[2].previous = 1;
        morphKeys[2].allocated = 1; morphKeys[2].frame = 50; morphKeys[2].value = .75f;
        display[0].next = 1; display[0].visible = 1;
        display[1].allocated = 1; display[1].frame = 60; display[1].visible = 1;
        model->maxFrame = 60;
        for (auto& key : display) {
            key.ikStates = static_cast<std::uint8_t*>(std::malloc(1));
            Require(key.ikStates != nullptr, "allocate IK fixture state"); key.ikStates[0] = 1;
        }
        camera[0].parentModel = -1; camera[0].distance = -25; camera[0].fov = 45;
        std::strcpy(accessory->name, "prop.x"); accessory->parentModel = -1;
        accessoryKeys[0].parentModel = -1; accessoryKeys[0].scale = 1; accessoryKeys[0].opacity = 1;
        g_Block = app.get();
    }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    ~Fixture() {
        bone_scale::Forget(Raw()); morph_curve::Forget(Raw());
        for (auto& ring : model->undoRings) for (auto& slot : ring.slots) { std::free(slot.bonePose); std::free(slot.auxiliaryPose); }
        for (auto& key : display) { std::free(key.ikStates); std::free(key.selectorStates); }
        g_Block = old;
    }
};

void Encoding() {
    namespace enc = original_file_encoding;
    std::string bytes;
    Check(enc::Encode(L"初音ミク", 20, bytes) && bytes == "\x8f\x89\x89\xb9\x83\x7e\x83\x4e", "standard text is explicit CP932");
    Check(!enc::Encode(L"😀", 20, bytes), "unrepresentable Unicode is rejected");
    Check(!enc::Encode(L"\u00a5", 20, bytes), "best-fit yen substitution is rejected");
    const std::wstring fits(10, L'あ');
    Check(enc::Encode(fits.c_str(), 20, bytes) && bytes.size() == 20, "complete 20-byte name needs no terminator");
    Check(!enc::Encode(fits.c_str(), 19, bytes), "field capacity is checked in encoded bytes");
    char field[20]; std::memset(field, 0x55, sizeof field);
    Check(!enc::Fixed(field, sizeof field, fits.c_str()) && field[0] == 0x55, "path reserves NUL and failure leaves destination untouched");
    Check(!enc::Name("\x82", 1, nullptr, 20, bytes), "split CP932 lead byte is refused");
    Check(!enc::Name("\xef\xbb\xbf" "abc", 6, nullptr, 20, bytes), "tagged UTF-8 is not emitted as a standard name");
}

void Vmd(Files& files) {
    Fixture f;
    const auto path = files.Path(L"動作_😀.vmd");
    SaveVmdFile(path.c_str());
    auto bytes = Read(path); auto input = Standard(path);
    Check(input.Size() == input.standardEnd && bytes.size() == 238, "model VMD ends exactly after standard IK records");
    Check(U32(bytes, 50) == 1 && U32(bytes, 69) == 0, "standard bone count and minimum frame stay native");
    Check(U32(bytes, 165) == 1 && U32(bytes, 184) == 10, "standard morph frame uses the same native minimum");
    f.keys[0].position[0] = -3;
    LoadVmdFile(path.c_str());
    Check(f.keys[0].position[0] == 7, "real wide VMD entry opens Unicode filename without ACP substitution");

    // A checksummed MMSMCRV1 envelope with an unknown payload. Construct it
    // here so the minimal provider need not expose an extension encoder.
    Bytes block{'M','M','S','M','C','R','V','1'};
    Put(block, std::uint32_t(17)); Put(block, std::uint32_t(0x8cdc1683)); block.push_back('x');
    bytes.insert(bytes.end(), block.begin(), block.end());
    bone_scale_io::PrefixFingerprint fingerprint; fingerprint.Append(bytes.data(), bytes.size());
    bone_scale_io::VmdExtension scale; scale.prefixBytes = fingerprint.bytes(); scale.prefixCrc32 = fingerprint.crc32();
    bone_scale_io::VmdExtension::Entry entry; std::memcpy(entry.name.data(), "bone", 4);
    entry.scale.v[0] = entry.scale.v[4] = entry.scale.v[8] = 3; scale.entries.push_back(entry);
    const auto payload = bone_scale_io::EncodeExtension(scale);
    bytes.insert(bytes.end(), payload.begin(), payload.end()); Put(bytes, std::uint64_t(payload.size()));
    const char magic[] = "MDSXVMDX"; bytes.insert(bytes.end(), magic, magic + 8);
    Write(path, bytes); f.keys[0].position[0] = -4; LoadVmdFile(path.c_str());
    Check(f.keys[0].position[0] == 7 && bone_scale::Identity(bone_scale::Key(f.Raw(), 0)), "full-profile tails do not affect standard pose import");
    SaveVmdFile(path.c_str()); input = Standard(path);
    Check(input.Size() == input.standardEnd, "re-saving extended VMD strips every extension");

    const Bytes sentinel{'o','l','d'}; Write(path, sentinel);
    wchar_t invalid[] = L"bone😀"; f.bones[0].jpText = invalid;
    SaveVmdFile(path.c_str()); Check(Read(path) == sentinel, "unrepresentable PMX bone name cannot overwrite an existing motion");
    wchar_t longName[] = L"1234567890123456"; f.bones[0].jpText = longName;
    SaveVmdFile(path.c_str()); Check(Read(path) == sentinel, "16-byte keyed bone name is refused before 15-byte VMD write");
    wchar_t fitName[] = L"123456789012345"; f.bones[0].jpText = fitName;
    SaveVmdFile(path.c_str()); bytes = Read(path);
    Check(bytes.size() > 69 && std::memcmp(bytes.data() + 54, "123456789012345", 15) == 0, "full 15-byte bone name writes without truncation");
    f.bones[0].jpText = nullptr;
    Write(path, sentinel); wchar_t invalidModel[] = L"sample😀"; f.model->pmxTextBuffers[0] = invalidModel;
    SaveVmdFile(path.c_str()); Check(Read(path) == sentinel, "model name is validated independently of its narrow mirror");
    f.model->pmxTextBuffers[0] = nullptr;

    SaveVmdFile(path.c_str()); bytes = Read(path);
    const auto position = f.keys[0].position[0];
    const auto undo = f.model->undoState[0];
    bytes.pop_back(); Write(path, bytes); LoadVmdFile(path.c_str());
    Check(f.keys[0].position[0] == position && f.model->undoState[0] == undo,
          "truncated standard VMD still fails before pose or undo changes");
}

void Camera(Files& files) {
    Fixture f; f.app->state.optflag[0] = 1;
    f.app->AccessorySlot(0) = f.accessory.get(); f.app->AccessoryKeys(0) = f.accessoryKeys.data();
    f.camera[0].next = 1; f.camera[1] = f.camera[0]; f.camera[1].next = 0;
    f.camera[1].selected = 1; f.camera[1].frame = 20;
    f.accessoryKeys[0].selected = 1; f.accessoryKeys[0].frame = 0;
    const auto path = files.Path(L"camera.vmd"); SaveVmdFile(path.c_str());
    auto bytes = Read(path); auto input = Standard(path);
    Check(input.Size() == input.standardEnd && bytes.size() == 135, "camera writer appends no accessory tail");
    Check(U32(bytes, 62) == 0, "earlier selected accessory does not shift standard camera minFrame");
    accessory_motion_io::Track track; track.name = "prop.x";
    accessory_motion_io::Key key; key.position[0] = 91; track.keys.push_back(key);
    const auto tail = accessory_motion_io::Encode({track}); bytes.insert(bytes.end(), tail.begin(), tail.end());
    Write(path, bytes); f.accessoryKeys[0].position[0] = 4;
    // Import into a fresh camera track. Reusing the frame-20 source chain
    // would exercise the unrelated legacy global-key head-insertion policy
    // instead of this standard-reader/ignored-extension boundary.
    f.camera[0].next = 0; f.camera[1] = {}; f.camera[0].distance = -3;
    LoadVmdFile(path.c_str());
    if (f.camera[0].distance != -25 || f.accessoryKeys[0].position[0] != 4)
        std::fprintf(stderr, "camera import diagnostic: head.distance=%g next=%u accessory.x=%g\n",
                     f.camera[0].distance, f.camera[0].next, f.accessoryKeys[0].position[0]);
    Check(f.camera[0].distance == -25 && f.accessoryKeys[0].position[0] == 4, "real camera load ignores valid accessory tail while loading camera");
}

void VmdNameBoundaries(Files& files) {
    Fixture f;
    // No selected 15-byte bone/morph records: this case exercises the
    // separate 20-byte model header and IK fields through the real loader.
    f.keys[1].allocated = 0; f.morphKeys[2].allocated = 0;
    wchar_t complete[] = L"abcdefghijklmnopqrst";
    std::memcpy(f.model->name, "abcdefghijklmnopqrs", 20);
    f.model->pmxTextBuffers[0] = complete;
    std::memcpy(f.bones[0].name, "abcdefghijklmnopqrs", 20);
    f.bones[0].jpText = complete;
    f.display[1].ikStates[0] = 0;
    const auto path = files.Path(L"name-boundary.vmd"); SaveVmdFile(path.c_str());
    const auto bytes = Read(path);
    Check(bytes.size() >= 50 && std::memcmp(bytes.data() + 30, "abcdefghijklmnopqrst", 20) == 0,
          "VMD writes the complete 20-byte authoritative model identity");
    f.display[0].ikStates[0] = 1; const auto undo = f.model->undoState[0];
    LoadVmdFile(path.c_str());
    Check(f.model->undoState[0] != undo && f.display[0].ikStates[0] == 0,
          "VMD own model identity and 20-byte IK identity survive shorter PMX mirrors");
    f.model->pmxTextBuffers[0] = nullptr; f.bones[0].jpText = nullptr;
    f.display[0].allocated = 0; f.display[1].allocated = 1; SaveVmdFile(path.c_str());
    f.model->pmxTextBuffers[0] = complete; f.bones[0].jpText = complete;
    f.display[0].ikStates[0] = 1; LoadVmdFile(path.c_str());
    Check(f.display[0].ikStates[0] == 0, "VMD original model and IK raw aliases remain readable");
}

void Vpd(Files& files) {
    Fixture f; const auto path = files.Path(L"ポーズ_😀.vpd");
    std::memcpy(f.bones[0].name, "abcdefghijklmnopqrst", 20); f.bones[0].trans[0] = 12;
    SaveVpdFile(path.c_str()); auto bytes = Read(path);
    const std::string text(bytes.begin(), bytes.end());
    Check(text.find("Bone0{abcdefghijklmnopqrst\r\n") != std::string::npos, "VPD writes bounded full 20-byte bone names");
    Check(text.size() >= 5 && text.substr(text.size() - 5) == "}\r\n\r\n", "VPD ends at the standard text boundary");
    vpd_scale_io::Entry entry; entry.bone = 0; std::memcpy(entry.name.data(), f.bones[0].name, 20);
    entry.scale.v[0] = entry.scale.v[4] = entry.scale.v[8] = 3;
    Require(vpd_scale_io::AppendTail(path.wstring(), {entry}) != vpd_scale_io::TailSave::Failed, "construct valid VPD scale tail");
    f.bones[0].trans[0] = -1; LoadVpdFile(path.c_str());
    Check(f.bones[0].trans[0] == 12 && bone_scale::Identity(bone_scale::Pose(f.Raw(), 0)), "VPD full name imports native pose and ignores scale tail");
    SaveVpdFile(path.c_str()); Check(Read(path) == bytes, "extended VPD re-save returns identical standard bytes");
    // Reproduce PMX WideToSjis's NUL-reserving mirror rather than assigning
    // an artificial 20-byte raw field only: the authoritative name is wider.
    wchar_t asciiBoundary[] = L"abcdefghijklmnopqrst";
    f.bones[0].jpText = asciiBoundary; f.bones[0].name[19] = 0;
    f.bones[0].trans[0] = 23; SaveVpdFile(path.c_str());
    f.bones[0].trans[0] = -2; LoadVpdFile(path.c_str());
    Check(f.bones[0].trans[0] == 23, "PMX complete 20-byte ASCII identity survives its 19-byte mirror");
    wchar_t japaneseBoundary[] = L"あいうえおかきくけこ";
    std::string japaneseBytes;
    Require(original_file_encoding::Encode(japaneseBoundary, 20, japaneseBytes), "encode 20-byte Japanese boundary fixture");
    std::memset(f.bones[0].name, 0, 20); std::memcpy(f.bones[0].name, japaneseBytes.data(), 19);
    f.bones[0].jpText = japaneseBoundary; f.bones[0].trans[0] = 34;
    SaveVpdFile(path.c_str()); f.bones[0].trans[0] = -3; LoadVpdFile(path.c_str());
    Check(f.bones[0].trans[0] == 34, "PMX complete 20-byte Japanese identity survives its shorter mirror");
    f.bones[0].name[18] = 0; // A valid shorter alias in an original pose.
    f.bones[0].jpText = nullptr; f.bones[0].trans[0] = 45; SaveVpdFile(path.c_str());
    f.bones[0].jpText = japaneseBoundary; f.bones[0].trans[0] = -4; LoadVpdFile(path.c_str());
    Check(f.bones[0].trans[0] == 45, "original VPD legacy mirror remains a compatible import alias");
    const Bytes sentinel{'o','l','d'}; Write(path, sentinel);
    wchar_t invalid[] = L"名前😀"; f.bones[0].jpText = invalid;
    SaveVpdFile(path.c_str()); Check(Read(path) == sentinel, "VPD rejects unrepresentable name without partial overwrite");
    wchar_t invalidLine[] = L"bad\nname"; f.bones[0].jpText = invalidLine;
    SaveVpdFile(path.c_str()); Check(Read(path) == sentinel, "VPD refuses line-breaking names");
    f.bones[0].jpText = nullptr;
}

void Pmm(Files& files) {
    Fixture f; f.app->ModelSlot(0) = nullptr; f.app->state.optflag[0] = 1;
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"original-profile-io", WS_OVERLAPPED,
        0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "create hidden PMM count-control owner");
    HWND models = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 1, 1,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kMainComboModel)), GetModuleHandleW(nullptr), nullptr);
    HWND accessories = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 1, 1,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kAccessoryCombo)), GetModuleHandleW(nullptr), nullptr);
    Require(models && accessories, "create hidden native PMM count combos");
    SendMessageW(models, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"camera/light"));
    f.app->Hwnd() = window;
    Check(!IsWindowVisible(window) && GetForegroundWindow() != window, "PMM fixture remains hidden and inactive");
    const auto path = files.Path(L"空のプロジェクト_😀.pmm"); wcscpy_s(f.app->EnvFileName(), 256, path.c_str());
    pmm_extension::Set(f.app->PmmProjectSettings().document, "example.unknown", {1,2,3});
    SaveSceneFile(f.app.get()); const auto standard = Read(path);
    Check(standard.size() > 54 && standard[54] == 0, "real empty PMM header writes zero models from native combo count");
    Check(f.app->PmmProjectSettings().document.records.empty(), "PMM save does not retain opaque full-profile records");
    pmm_extension::Document extension; pmm_extension::Set(extension, pmm_settings::kSortingPrecisionKey, {3});
    pmm_extension::Set(extension, "example.unknown", {7,8,9});
    int fd = Open(path, _O_RDWR); _lseeki64(fd, 0, SEEK_END);
    Require(pmm_extension::Append(fd, extension), "construct valid PMM extension");
    _lseeki64(fd, static_cast<__int64>(standard.size()), SEEK_SET);
    f.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    pmm_settings::Load(*f.app, fd);
    Check(_telli64(fd) == static_cast<__int64>(standard.size()), "minimal PMM settings leaves standard endpoint untouched");
    Check(f.app->PmmProjectSettings().document.records.empty() &&
          f.app->TransparentMaterialSortPrecision() == TransparentSortPrecision::Off,
          "minimal PMM loader neither keeps nor applies valid extension settings");
    _close(fd);
    Check(pmm_settings::Capture(*f.app).records.empty(), "minimal PMM capture contains no application extensions");
    SaveSceneFile(f.app.get()); Check(Read(path) == standard, "real PMM re-save removes tail without changing standard bytes");
    const auto headless = f.app->Hwnd(); f.app->Hwnd() = nullptr;
    const Bytes sentinel{'o','l','d'}; Write(path, sentinel);
    wcscpy_s(f.app->WavePath(), 256, L"C:\\music😀.wav");
    SaveSceneFile(f.app.get()); Check(Read(path) == sentinel, "PMM rejects unrepresentable referenced path atomically");
    f.app->WavePath()[0] = 0;
    f.app->ModelSlot(0) = f.Raw(); wchar_t invalidName[] = L"sample😀"; f.model->pmxTextBuffers[0] = invalidName;
    SaveSceneFile(f.app.get()); Check(Read(path) == sentinel, "PMM rejects unrepresentable authoritative model name");
    f.model->pmxTextBuffers[0] = nullptr;
    wchar_t boundary[] = L"abcdefghijklmnopqrst";
    std::memcpy(f.bones[0].name, "abcdefghijklmnopqrs", 20); f.bones[0].jpText = boundary;
    SetLastError(ERROR_SUCCESS); SaveSceneFile(f.app.get());
    const auto boneError = GetLastError();
    Check(Read(path) == sentinel && boneError == ERROR_NO_UNICODE_TRANSLATION,
          "PMM refuses mismatched complete bone identity before replacing an existing file");
    f.bones[0].jpText = nullptr;
    std::memcpy(f.morphs[1].name, "abcdefghijklmnopqrs", 20); f.morphs[1].jpText = boundary;
    SetLastError(ERROR_SUCCESS); SaveSceneFile(f.app.get());
    const auto morphError = GetLastError();
    Check(Read(path) == sentinel && morphError == ERROR_NO_UNICODE_TRANSLATION,
          "PMM refuses mismatched complete morph identity before replacing an existing file");
    f.morphs[1].jpText = nullptr;
    auto* expandedBones = mdl::BoneKeyPoolCreate(mdl::kBoneKeyCapacity + 1);
    auto* expandedMorphs = mdl::MorphKeyPoolCreate(mdl::kMorphKeyCapacity + 1);
    std::memcpy(expandedBones, f.keys.data(), 2 * sizeof(mdl::BoneKey));
    std::memcpy(expandedMorphs, f.morphKeys.data(), 3 * sizeof(mdl::MorphKey));
    f.model->boneKeys = expandedBones; f.model->morphKeys = expandedMorphs;
    auto refusesHighSlot = [&](const char* reason) {
        Write(path, sentinel); SetLastError(ERROR_SUCCESS); SaveSceneFile(f.app.get());
        const auto error = GetLastError();
        Check(Read(path) == sentinel && error == ERROR_BUFFER_OVERFLOW, reason);
    };
    expandedBones[mdl::kBoneKeyCapacity].frame = 101;
    refusesHighSlot("PMM refuses even one live bone slot beyond original capacity without overwrite");
    expandedBones[mdl::kBoneKeyCapacity] = {};
    expandedBones[0].next = mdl::kBoneKeyCapacity;
    refusesHighSlot("PMM refuses an out-of-range bone link even when its target slot is empty");
    expandedBones[0].next = 1;
    expandedMorphs[mdl::kMorphKeyCapacity].frame = 102;
    refusesHighSlot("PMM refuses even one live morph slot beyond original capacity without overwrite");
    expandedMorphs[mdl::kMorphKeyCapacity] = {};
    expandedMorphs[1].previous = mdl::kMorphKeyCapacity;
    refusesHighSlot("PMM refuses an out-of-range morph link even when its target slot is empty");
    expandedMorphs[1].previous = 0;
    SendMessageW(models, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"sample"));
    f.app->Hwnd() = headless;
    SaveSceneFile(f.app.get()); auto grown = Read(path);
    Check(grown.size() > 54 && grown[54] == 1,
          "PMM accepts oversized allocated pools whose serialized slots and links all fit original limits");
    expandedBones[mdl::kBoneKeyCapacity - 1].frame = 103;
    expandedBones[mdl::kBoneKeyCapacity - 1].previous = 0;
    expandedMorphs[mdl::kMorphKeyCapacity - 1].frame = 104;
    expandedMorphs[mdl::kMorphKeyCapacity - 1].previous = 1;
    SaveSceneFile(f.app.get());
    Check(Read(path).size() > grown.size(), "PMM accepts the last in-range bone and morph slots");
    f.model->boneKeys = f.keys.data(); f.model->morphKeys = f.morphKeys.data();
    mdl::BoneKeyPoolDestroy(expandedBones); mdl::MorphKeyPoolDestroy(expandedMorphs);
    f.app->ModelSlot(0) = nullptr;
    f.app->Hwnd() = headless; DestroyWindow(window); f.app->Hwnd() = nullptr;
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!build_profile::original) { std::fprintf(stderr, "Requires MIKUMIKUSTUDIO_PROFILE=mmd932\n"); return 2; }
    std::printf("original_profile_io_tests ACP=%u original=%d\n", GetACP(), build_profile::original ? 1 : 0);
    wchar_t desktop[256]{}; DWORD needed = 0;
    if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &needed))
        std::wprintf(wcsncmp(desktop, L"MMS_REPAIR_", 11) == 0 ? L"PRIVATE_DESKTOP name=%ls\n" : L"DESKTOP name=%ls\n", desktop);
    Files files; Encoding(); Vmd(files); Camera(files); VmdNameBoundaries(files); Vpd(files); Pmm(files);
    std::printf("original_profile_io_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
