// Exercise the production PMD save boundary without a window or D3D device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"

#if !MMD_ORIGINAL_PROFILE
#error This test exercises the mmd932 profile only.
#endif

namespace {
using namespace mikumikustudio;
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void Require(bool ok, const char* message) {
    Check(ok, message);
    if (!ok) std::exit(2);
}
Bytes Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}
void Write(const fs::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Require(stream.good(), "write owned fixture");
}
struct Files {
    fs::path directory = fs::temp_directory_path() /
        (L"MMS-original-pmd-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::path path = directory / L"model.pmd";
    Files() { Require(fs::create_directory(directory), "create owned test directory"); }
    ~Files() { fs::remove(path); fs::remove(directory); }
};
struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::PmdVertex, 2> vertices{};
    std::array<std::uint16_t, 6> indices{}; // PMD CPU table: WORD plus padding
    std::array<mdl::ModelMaterialRecord, 1> materials{};
    std::array<mdl::BoneRecord, 1> bones{};
    std::array<mdl::MorphRecord, 2> morphs{};
    std::array<mdl::PmdVertexMorphEntry, 2> morphVertices{};
    Fixture() {
        app->state = MMDAppState{};
        app->EnglishUI() = 1;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
        app->SetSelectedModelSlot(0);
        std::strcpy(model->name, "sample");
        std::wcscpy(model->path, L"previous.pmd");
        model->vertexCount = 1; model->rawVertices = vertices.data();
        model->indexCount = 3; model->indices = indices.data();
        model->materialCount = 1; model->materials = materials.data();
        materials[0].faceVertexCount = 3;
        model->boneCount = 1; model->boneTable = bones.data(); model->groupCount = 2;
        std::strcpy(bones[0].name, "bone");
        bones[0].parent = -1; bones[0].type = mdl::BoneType::RotateMove;
        vertices[0].weightPercent = 100; vertices[1].weightPercent = 100;
    }
    void Save(const fs::path& path) { SaveEnhancedModel(app.get(), path.c_str()); }
};

void ValidPmd(Files& files) {
    Fixture f;
    // Exactly 20 bytes are legal in this field; the legacy wchar_t[20]
    // concatenation could instead truncate or invoke the invalid handler.
    std::wcscpy(f.materials[0].texturePath, L"abcdefghijklmnop.png");
    f.Save(files.path);
    auto bytes = Read(files.path);
    Check(bytes.size() == 1765, "standard PMD ends after zero rigid and joint counts");
    Check(bytes.size() >= 409 && std::memcmp(bytes.data(), "Pmd", 3) == 0 &&
          std::memcmp(bytes.data() + 389, "abcdefghijklmnop.png", 20) == 0,
          "20-byte material filename is complete in the standard texture field");
    Check(std::wstring(f.model->path) == files.path.wstring(), "successful save updates model path");
    std::wcscpy(f.materials[0].texturePath, L"ああああああああああ");
    f.Save(files.path); bytes = Read(files.path);
    bool complete = bytes.size() == 1765;
    for (std::size_t i = 0; complete && i != 10; ++i)
        complete = bytes[389 + i * 2] == 0x82 && bytes[390 + i * 2] == 0xa0;
    Check(complete, "multibyte texture name fits by CP932 bytes rather than characters");
    std::wcscpy(f.materials[0].texturePath, L"tex.png");
    std::wcscpy(f.materials[0].spherePath, L"env.sph");
    f.Save(files.path); bytes = Read(files.path);
    Check(bytes.size() == 1765 && std::memcmp(bytes.data() + 389, "tex.png*env.sph", 15) == 0,
          "texture and sphere references use the original PMD separator");
    f.model->vertexCount = 2;
    f.indices = {{0, 0xbeef, 1, 0xbeef, 0, 0xbeef}};
    f.Save(files.path); bytes = Read(files.path);
    const std::array<unsigned char, 6> packedIndices{{0, 0, 1, 0, 0, 0}};
    Check(bytes.size() == 1803 && std::memcmp(bytes.data() + 367, packedIndices.data(), 6) == 0,
          "interleaved PMD CPU indices are packed without padding in the saved file");
    Check(std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
          "successful atomic save leaves no temporary files");
}

void UnsignedBoneReferences(Files& files) {
    Fixture f;
    std::vector<mdl::BoneRecord> bones(32769);
    for (auto& bone : bones) bone.parent = -1;
    f.model->boneCount = static_cast<std::uint32_t>(bones.size());
    f.model->boneTable = bones.data();
    mdl::IkChain ik{};
    std::uint16_t link = 32768;
    ik.boneIndex = 32768; ik.targetBone = 32768; ik.linkCount = 1; ik.links = &link;
    f.model->ikChainCount = 1; f.model->ikChains = &ik;
    std::array<mdl::DisplayGroup, 3> groups{};
    f.model->groupCount = 3; f.model->groupNames = groups.data();
    mdl::FrameGroup row{}; row.targetIndex = 32768; row.groupIndex = 2;
    f.model->rigidBodyCount = 1; f.model->rbGroups = &row;
    f.Save(files.path);
    const auto bytes = Read(files.path);
    const auto word = [&](std::size_t offset, unsigned value) {
        return offset + 2 <= bytes.size() &&
            (static_cast<unsigned>(bytes[offset]) | static_cast<unsigned>(bytes[offset + 1]) << 8) == value;
    };
    const std::size_t ikCountOffset = 411 + bones.size() * 39;
    const std::size_t displayOffset = ikCountOffset + 2 + 13 + 2 + 1 + 1 + 50 + 4;
    Check(word(409, 32769) && word(ikCountOffset, 1) &&
          word(ikCountOffset + 2, 32768) && word(ikCountOffset + 4, 32768) &&
          word(ikCountOffset + 13, 32768),
          "IK driver, target and link 32768 survive unsigned WORD serialization");
    Check(word(displayOffset, 32768), "bone display reference 32768 survives unsigned WORD serialization");
    const auto saved = bytes;
    bones[0].parent = 32768;
    SetLastError(ERROR_SUCCESS); f.Save(files.path); const DWORD error = GetLastError();
    Check(error == ERROR_INVALID_DATA && Read(files.path) == saved,
          "parent index 32768 still fails its signed WORD boundary without replacing the saved model");
}

void RefusedPmd(Files& files) {
    const Bytes previous{'o','l','d',' ','P','M','D'};
    const auto refuses = [&](Fixture& f, const char* message) {
        Write(files.path, previous);
        const std::wstring oldPath = f.model->path;
        SetLastError(ERROR_SUCCESS); f.Save(files.path);
        const DWORD error = GetLastError();
        Check(error == ERROR_INVALID_DATA && Read(files.path) == previous &&
              std::wstring(f.model->path) == oldPath, message);
    };
    {
        Fixture f; wchar_t name[] = L"bone😀"; f.bones[0].jpText = name;
        refuses(f, "Unicode bone name is refused before overwriting existing PMD");
    }
    {
        Fixture f; wchar_t name[] = L"123456789012345678901"; f.model->pmxTextBuffers[0] = name;
        refuses(f, "long authoritative model name cannot silently use its short mirror");
    }
    {
        Fixture f; std::strcpy(f.bones[0].name, "\x82");
        refuses(f, "incomplete CP932 lead byte is refused");
    }
    {
        Fixture f; std::memcpy(f.bones[0].nameEn, "12345678901234567890", 20);
        refuses(f, "name that original MMD would truncate at byte 19 is refused");
    }
    {
        Fixture f; std::wcscpy(f.materials[0].texturePath, L"tex😀.png");
        refuses(f, "unrepresentable material reference is refused");
    }
    {
        Fixture f; std::wcscpy(f.materials[0].texturePath, L"abcdefghijklmnopq.png");
        refuses(f, "21-byte material filename is not truncated to 20 bytes");
    }
    {
        Fixture f; std::wcscpy(f.model->modelDirectory, L"C:\\model\\");
        std::wcscpy(f.materials[0].texturePath, L"D:\\other\\tex.png");
        refuses(f, "unrelated texture path is not stripped by directory length");
    }
    {
        Fixture f; f.model->physicsMode = 2;
        refuses(f, "PMX data is not silently exported through the PMD writer");
    }
    {
        Fixture f; f.model->boneCount = 65536;
        refuses(f, "bone count is checked before narrowing to its 16-bit PMD field");
    }
    {
        Fixture f; f.indices[0] = 1;
        refuses(f, "out-of-range mesh index is refused");
    }
    {
        Fixture f; f.bones[0].parent = 65536;
        refuses(f, "bone reference is checked before narrowing to 16 bits");
    }
    {
        Fixture f; f.materials[0].faceVertexCount = 0;
        refuses(f, "material triangle spans must cover the standard index table");
    }
    {
        Fixture f; mdl::JointRecord joint{};
        joint.rigidA = -1; joint.rigidB = -1;
        f.model->jointCount = 1; f.model->jointTable = &joint;
        refuses(f, "PMD joint cannot refer to a missing rigid body");
    }
    {
        Fixture f; f.model->vertexCount = 2; f.model->morphCount = 2;
        f.model->morphs = f.morphs.data();
        for (unsigned i = 0; i < 2; ++i) {
            f.morphs[i].offsetCount = 1; f.morphs[i].vertexEntries = &f.morphVertices[i];
            f.morphVertices[i].vertexIndex = i;
        }
        refuses(f, "morph vertex absent from base table cannot omit its index from the PMD body");
    }
    Check(std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
          "refused preflights create no temporary files");
    {
        Fixture f; Write(files.path, previous);
        HANDLE locked = CreateFileW(files.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "lock destination against replacement");
        SetLastError(ERROR_SUCCESS); f.Save(files.path); const DWORD error = GetLastError();
        CloseHandle(locked);
        Check(error == ERROR_WRITE_FAULT && Read(files.path) == previous &&
              std::wstring(f.model->path) == L"previous.pmd",
              "failed atomic replacement preserves existing PMD and model path");
        Check(std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
              "failed replacement cleans up only its temporary file");
    }
}
} // namespace

int main() {
    Files files;
    ValidPmd(files); UnsignedBoneReferences(files); RefusedPmd(files);
    std::printf("original_pmd_save_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
