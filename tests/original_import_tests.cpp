// Production import boundaries, with no visible windows or D3D device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <fcntl.h>
#include <io.h>
#include "mikumikustudio/original_import.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/wave_audio_context.hpp"
#include "../src/model/vmd_read_guard.hpp"
#include "../src/io/pmm_io_common.hpp"

namespace mikumikustudio { void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t); }
namespace {
using namespace mikumikustudio;
using Bytes = std::vector<unsigned char>;
namespace fs = std::filesystem;
int checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template<class T> void Put(Bytes& bytes, const T& value) {
    const auto* first = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof value);
}
void Text(Bytes& bytes, const wchar_t* text) {
    const auto size = static_cast<unsigned>(wcslen(text) * sizeof(wchar_t));
    Put(bytes, size);
    const auto* first = reinterpret_cast<const unsigned char*>(text);
    bytes.insert(bytes.end(), first, first + size);
}
Bytes Pmx(const wchar_t* name, const wchar_t* bone, const wchar_t* texture = nullptr) {
    Bytes bytes{'P','M','X',' '}; Put(bytes, 2.f);
    for (unsigned char v : {8,0,0,1,1,1,1,1,1}) Put(bytes, v);
    Text(bytes, name); Text(bytes, L"model"); Text(bytes, L"info"); Text(bytes, L"info");
    Put(bytes, 0u); Put(bytes, 0u); // vertices, faces
    Put(bytes, texture ? 1u : 0u); if (texture) Text(bytes, texture);
    Put(bytes, 0u); Put(bytes, 1u); // materials, bones
    Text(bytes, bone); Text(bytes, L"bone");
    for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
    Put(bytes, static_cast<unsigned char>(255)); Put(bytes, 0u);
    Put(bytes, static_cast<unsigned short>(0));
    for (int i = 0; i < 3; ++i) Put(bytes, 0.f);
    for (int i = 0; i < 4; ++i) Put(bytes, 0u); // morphs, frames, bodies, joints
    return bytes;
}
struct Files {
    fs::path old = fs::current_path();
    fs::path directory = fs::temp_directory_path() / (L"MMS-import-" + std::to_wstring(GetCurrentProcessId()));
    fs::path path = directory / L"model.dat";
    Files() { Check(fs::create_directory(directory), "create owned fixture directory"); }
    ~Files() { fs::current_path(old); fs::remove(path); fs::remove(directory); }
    void Write(const Bytes& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Check(out.good(), "write fixture");
    }
};
void Load(Files& files, const Bytes& bytes, bool accepted,
          const wchar_t* expectedName = nullptr, const wchar_t* expectedBone = nullptr) {
    files.Write(bytes);
    auto model = std::make_unique<mdl::ModelRecord>();
    auto renderer = std::make_unique<D3DRenderer>();
    PathResolutionWorkspace paths{};
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    ModelInitDefaults(raw);
    const bool loaded = ModelLoadPMD(raw, nullptr, files.path.c_str(), renderer.get(), 1, 0, 1, nullptr, paths);
    if (loaded != accepted) std::fprintf(stderr, "model import mismatch: %zu bytes, expected=%d actual=%d name=%ls bone=%ls\n",
        bytes.size(), accepted, loaded, expectedName ? expectedName : L"", expectedBone ? expectedBone : L"");
    Check(loaded == accepted, "production model loader accepts/rejects at import");
    if (loaded && expectedName) Check(model->pmxTextBuffers[0] &&
        std::wcscmp(model->pmxTextBuffers[0], expectedName) == 0,
        "accepted PMX keeps the complete source model name");
    if (loaded && expectedBone) Check(model->boneCount && model->boneTable[0].jpText &&
        std::wcscmp(model->boneTable[0].jpText, expectedBone) == 0,
        "accepted PMX keeps the complete source bone name");
    ModelDispose(raw);
    if (!accepted) {
        auto app = std::make_unique<MMDApp>();
        std::memset(&app->state, 0, sizeof app->state);
        app->Renderer() = renderer.get();
        app->EnglishUI() = 1;
        app->SceneModified() = 0;
        LoadModelFile(app.get(), files.path.c_str(), false);
        Check(!app->ModelSlot(0) && !app->SceneModified(), "rejected model leaves no scene slot or dirty flag");
    }
}
void Pmd(Files& files) {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof app->state);
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    std::strcpy(model->name, "model"); std::strcpy(bone.name, "bone");
    bone.parent = -1; bone.type = mdl::BoneType::RotateMove;
    model->boneCount = 1; model->boneTable = &bone; model->groupCount = 2;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get()); app->SetSelectedModelSlot(0);
    SaveEnhancedModel(app.get(), files.path.c_str());
    std::ifstream in(files.path, std::ios::binary);
    Bytes bytes(std::istreambuf_iterator<char>(in), {}); in.close();
    Check(bytes.size() > 300 && bytes[0] == 'P', "create PMD with production writer");
    Load(files, bytes, true);
    bytes[7] = 0x82; bytes[8] = 0;
    Load(files, bytes, true);
}
void MotionNames(Files& files) {
    Bytes bytes(50, 0);
    std::memcpy(bytes.data(), "Vocaloid Motion Data 0002", 25);
    std::memcpy(bytes.data() + 30, "model", 5);
    Put(bytes, 1u); const auto bone = bytes.size(); bytes.resize(bytes.size() + 111, 0);
    std::memcpy(bytes.data() + bone, "bone", 4); Put(bytes, 0u);
    const auto check = [&](bool fit) {
        files.Write(bytes); int fd = _wopen(files.path.c_str(), _O_BINARY | _O_RDONLY);
        vmd_read::Input input; Check(fd >= 0 && input.Load(fd), "complete VMD fixture parses"); _close(fd);
        Check(input.OriginalNamesFit() == fit, "all VMD names checked before applying keys");
    };
    check(true); bytes[bone] = 0x82; bytes[bone + 1] = 0; check(false);
    bytes[bone] = 'A'; bytes[bone + 1] = 0;
    bytes[30] = 0xef; bytes[31] = 0xbb; bytes[32] = 0xbf; check(false);
}
void ModelPaths(Files& files) {
    const auto original = files.path;
    for (const std::wstring& filename : {std::wstring(L"model-\U0001f31f.pmx"), std::wstring(100, L'あ') + L".pmx"}) {
        files.path = files.directory / filename;
        Check(!original_import::Text(files.path.c_str()), "fixture exceeds original CP932 path representation");
        Load(files, Pmx(L"model", L"bone"), true, L"model", L"bone");
        fs::remove(files.path);
    }
    files.path = original;
    auto model = std::make_unique<mdl::ModelRecord>();
    auto renderer = std::make_unique<D3DRenderer>();
    PathResolutionWorkspace paths{};
    std::wstring tooLong(256, L'A');
    Check(!ModelLoadPMD(reinterpret_cast<unsigned char*>(model.get()), nullptr, tooLong.c_str(),
        renderer.get(), 1, 0, 1, nullptr, paths) && GetLastError() == ERROR_FILENAME_EXCED_RANGE,
        "path beyond the host buffer fails cleanly before CRT copying");
}
void ProjectNames(Files& files) {
    for (const Bytes& bytes : {Bytes{'o','k',0}, Bytes{0x82,0,0}}) {
        files.Write(bytes); int fd = _wopen(files.path.c_str(), _O_BINARY | _O_RDONLY);
        char value[3]{}; bool accepted = true;
        try { pmm_io::ReadText(fd, value, sizeof value, 2); }
        catch (const pmm_io::UnsupportedText&) { accepted = false; }
        _close(fd);
        Check(accepted == (!build_profile::original || bytes[0] == 'o'), "PMM reader rejects invalid names only in minimal build");
    }
}
void Editor() {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof app->state);
    auto model = std::make_unique<mdl::ModelRecord>(); model->physicsMode = 2;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get()); app->SetSelectedModelSlot(0);
    if (build_profile::original) {
        for (unsigned command : {259u, 261u, 262u, 263u}) {
            SetLastError(0); CmdViewMenu(app.get(), nullptr, static_cast<std::uint16_t>(command), 0);
            Check(GetLastError() == ERROR_NOT_SUPPORTED, "PMX editor command is rejected before opening a dialog");
            Check(app->EnhancedModelDirty() == 0 && app->state.enterKeyState == 0 &&
                app->state.frameCopyDialog == nullptr && app->state.dialogFlags[12] == 0,
                "rejected editor command leaves model and dialog state unchanged");
        }
    } else Check(original_import::CanEdit(*model), "full build does not gain minimal editor restriction");
}
void References() {
    Check(original_import::Reference(L"C:\\models\\初音ミク.pmx", nullptr, 1), "CP932 reference is allowed");
    Check(original_import::Reference(L"C:\\models\\\U0001f31f.pmx", nullptr, 1) == !build_profile::original,
        "unrepresentable reference fails only in minimal build");
    const std::wstring longPath(128, L'あ');
    Check(original_import::Reference(longPath.c_str(), nullptr, 1) == !build_profile::original,
        "reference limit counts CP932 bytes");
    if (build_profile::original) {
        auto app = std::make_unique<MMDApp>();
        std::memset(&app->state, 0, sizeof app->state);
        auto accessory = std::make_unique<mdl::AccessoryRecord>();
        Check(!LoadAccessoryObject(app.get(), accessory.get(), L"C:\\\U0001f31f.x"),
            "accessory import rejects unsupported path before using renderer");
        wcscpy_s(app->AviBackgroundPath(), 256, L"C:\\\U0001f31f.avi");
        app->AviBackgroundEnabled() = 1;
        SetLastError(0); LoadAviFile(app.get());
        Check(GetLastError() == ERROR_NO_UNICODE_TRANSLATION && !app->AviBackgroundPath()[0] &&
            !app->AviBackgroundEnabled(),
            "AVI import rejects unsupported reference before opening media");
        wcscpy_s(app->PictureBackgroundPath(), 256, L"C:\\\U0001f31f.bmp");
        app->PictureBackgroundEnabled() = 1;
        SetLastError(0); LoadBackgroundPicture(app.get());
        Check(GetLastError() == ERROR_NO_UNICODE_TRANSLATION && !app->PictureBackgroundPath()[0] &&
            !app->PictureBackgroundEnabled(),
            "background image import rejects unsupported reference before using renderer");
        WaveAudioContext audio{};
        Check(!WaveLoadFile(&audio, L"C:\\\U0001f31f.wav", app->PathWorkspace()) &&
            GetLastError() == ERROR_NO_UNICODE_TRANSLATION,
            "WAV import rejects unsupported reference before opening audio");
    }
}
void AtomicMotionImport(Files& files) {
    if (!build_profile::original) return;
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof app->state);
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{}; std::strcpy(bone.name, "bone"); bone.trans[0] = 77;
    model->boneCount = 1; model->boneTable = &bone; std::strcpy(model->name, "model");
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get()); app->SetSelectedModelSlot(0);
    app->EnglishUI() = 1;
    MMDApp* previous = g_Block; g_Block = app.get();
    const std::string pose = "Vocaloid Pose Data file\n\nmodel.osm;\n2;\n\n"
        "Bone0{bone\n  1,2,3;\n  0,0,0,1;\n}\n\n"
        "Bone1{\x82\n  4,5,6;\n  0,0,0,1;\n}\n\n";
    files.Write(Bytes(pose.begin(), pose.end()));
    SetLastError(0); LoadVpdFile(files.path.c_str());
    Check(GetLastError() == ERROR_NO_UNICODE_TRANSLATION && bone.trans[0] == 77 &&
        !model->undoDirty && !model->undoState[0], "invalid late VPD name leaves pose and undo state unchanged");
    Bytes motion(50, 0); std::memcpy(motion.data(), "Vocaloid Motion Data 0002", 25);
    std::memcpy(motion.data() + 30, "model", 5); Put(motion, 2u);
    const auto start = motion.size(); motion.resize(start + 222, 0);
    std::memcpy(motion.data() + start, "bone", 4); motion[start + 111] = 0x82; Put(motion, 0u);
    files.Write(motion);
    SetLastError(0); LoadVmdFile(files.path.c_str());
    Check(GetLastError() == ERROR_NO_UNICODE_TRANSLATION && bone.trans[0] == 77 &&
        !model->undoDirty && !model->undoState[0], "invalid late VMD name leaves keys and undo state unchanged");
    g_Block = previous;
}
}
int main() {
    Files files;
    Load(files, Pmx(L"初音ミク", L"センター"), true);
    Load(files, Pmx(L"model", L"abcdefghijklmno"), true);
    Load(files, Pmx(L"model", L"abcdefghijklmnop"), true, L"model", L"abcdefghijklmnop");
    Load(files, Pmx(L"model", L"abcdefghijklmnopqrst"), true, L"model", L"abcdefghijklmnopqrst");
    Load(files, Pmx(L"abcdefghijklmnopqrstu", L"bone"), true, L"abcdefghijklmnopqrstu", L"bone");
    Load(files, Pmx(L"model", L"あいうえおかきA"), true);
    Load(files, Pmx(L"model", L"あいうえおかきく"), true, L"model", L"あいうえおかきく");
    Load(files, Pmx(L"\U0001f31f", L"bone"), true, L"\U0001f31f", L"bone");
    Load(files, Pmx(L"model", L"\U0001f31f"), true, L"model", L"\U0001f31f");
    Load(files, Pmx(L"model", L"bone", L"\U0001f31f.png"), true);
    auto truncated = Pmx(L"model", L"bone"); truncated.resize(23); // incomplete model-name text
    Load(files, truncated, false);
    Pmd(files);
    ModelPaths(files);
    MotionNames(files);
    ProjectNames(files);
    Editor();
    References();
    AtomicMotionImport(files);
    std::printf("original_import_tests: %d checks, profile=%s\n", checks, build_profile::original ? "minimal" : "full");
}
