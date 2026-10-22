// Save a real triangle through the production PMD writer in either profile.
// This fixture uses the PMD loader's CPU layout, not a D3D device or window.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <io.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#endif

namespace {
using namespace mikumikustudio;
namespace fs = std::filesystem;
int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Files {
    fs::path directory = fs::temp_directory_path() /
        (L"MMS-pmd-indices-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::path path = directory / L"triangle.pmd";
    Files() {
        if (!fs::create_directory(directory)) {
            std::fprintf(stderr, "Could not create owned fixture directory\n");
            std::exit(2);
        }
    }
    ~Files() { std::error_code ignored; fs::remove(path, ignored); fs::remove(directory, ignored); }
};
std::vector<unsigned char> Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::PmdVertex, 3> vertices{};
    // ModelLoadPMD stores WORD indices in four-byte, zero-filled CPU slots.
    std::array<std::uint16_t, 6> indices{{2, 0, 0, 0, 1, 0}};
    std::array<mdl::ModelMaterialRecord, 1> materials{};
    std::array<mdl::BoneRecord, 1> bones{};
    Fixture() {
        app->state = MMDAppState{}; app->EnglishUI() = 1;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
        app->SetSelectedModelSlot(0);
        std::strcpy(model->name, "triangle");
        model->vertexCount = 3; model->rawVertices = vertices.data();
        vertices[1].position[0] = 1; vertices[2].position[1] = 1;
        for (auto& vertex : vertices) vertex.weightPercent = 100;
        model->indexCount = 3; model->indices = indices.data();
        model->materialCount = 1; model->materials = materials.data();
        materials[0].faceVertexCount = 3;
        model->boneCount = 1; model->boneTable = bones.data();
        bones[0].parent = -1; bones[0].type = mdl::BoneType::RotateMove;
        std::strcpy(bones[0].name, "bone"); model->groupCount = 2;
    }
};

#if !MMD_ORIGINAL_PROFILE
struct AuthoredSnapshot {
    const Fixture& fixture;
    std::vector<unsigned char> state, model;
    decltype(Fixture::vertices) vertices;
    decltype(Fixture::indices) indices;
    decltype(Fixture::materials) materials;
    decltype(Fixture::bones) bones;
    std::uint64_t revision, generation;
    static std::vector<unsigned char> Bytes(const void* p, std::size_t size) {
        const auto* first = static_cast<const unsigned char*>(p);
        return {first, first + size};
    }
    explicit AuthoredSnapshot(const Fixture& f) : fixture(f),
        state(Bytes(&f.app->state, sizeof f.app->state)), model(Bytes(f.model.get(), sizeof *f.model)),
        vertices(f.vertices), indices(f.indices), materials(f.materials), bones(f.bones),
        revision(f.app->SceneRevision()), generation(f.app->SceneGeneration()) {}
    void Unchanged(const wchar_t* successfulPath = nullptr) const {
        const auto& f = fixture;
        auto expectedModel = model;
        if (successfulPath) {
            const auto offset = reinterpret_cast<const unsigned char*>(f.model->path) -
                reinterpret_cast<const unsigned char*>(f.model.get());
            std::array<wchar_t, 256> expectedPath;
            std::memcpy(expectedPath.data(), model.data() + offset, sizeof expectedPath);
            wcscpy_s(expectedPath.data(), expectedPath.size(), successfulPath);
            std::memcpy(expectedModel.data() + offset, expectedPath.data(), sizeof expectedPath);
        }
        Check(!std::memcmp(state.data(), &f.app->state, state.size()) &&
              !std::memcmp(expectedModel.data(), f.model.get(), model.size()) &&
              !std::memcmp(vertices.data(), f.vertices.data(), sizeof vertices) &&
              !std::memcmp(indices.data(), f.indices.data(), sizeof indices) &&
              !std::memcmp(materials.data(), f.materials.data(), sizeof materials) &&
              !std::memcmp(bones.data(), f.bones.data(), sizeof bones) &&
              revision == f.app->SceneRevision() && generation == f.app->SceneGeneration(),
              "full PMD export preserves captured authored bytes and revisions except its expected successful source-path update");
    }
};
#endif

// Hook only this executable's imported CRT entry points. The production PMD
// serialization and filesystem replacement still run, including real partial
// writes. No disk-full volume or permissions change is needed for the fixture.
struct CrtFault {
    enum class Kind { Write, ShortWrite, Commit, Close };
    using Write = int (__cdecl*)(int, const void*, unsigned);
    using FileOperation = int (__cdecl*)(int);
    static inline Write realWrite = nullptr;
    static inline FileOperation realClose = nullptr;
    static inline Kind kind = Kind::Write;
    static inline unsigned failAt = 1, calls = 0;
    ULONG_PTR* slot = nullptr;
    ULONG_PTR previous = 0;
    static int __cdecl WriteResult(int fd, const void* data, unsigned count) {
        if (++calls < failAt) return realWrite(fd, data, count);
        if (kind == Kind::ShortWrite && count > 1) return realWrite(fd, data, count - 1);
        errno = ENOSPC;
        return -1;
    }
    static int __cdecl CommitResult(int) { ++calls; errno = ENOSPC; return -1; }
    static int __cdecl CloseResult(int fd) {
        ++calls;
        // Close the actual descriptor even when simulating a reported close
        // failure, so this fault injector itself cannot leak a file handle.
        realClose(fd);
        errno = EIO;
        return -1;
    }
    explicit CrtFault(Kind requested, unsigned afterWrites = 1) {
        kind = requested; failAt = afterWrites; calls = 0;
        const char* symbol = requested == Kind::Commit ? "_commit" :
                             requested == Kind::Close ? "_close" : "_write";
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
             imports.VirtualAddress && entry->Name && !slot; ++entry) {
            if (!entry->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->OriginalFirstThunk);
            auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(name->Name), symbol)) continue;
                slot = reinterpret_cast<ULONG_PTR*>(&addresses->u1.Function);
                break;
            }
        }
        if (!slot) return;
        DWORD oldProtection = 0;
        if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &oldProtection)) { slot = nullptr; return; }
        previous = *slot;
        if (requested == Kind::Close) {
            realClose = reinterpret_cast<FileOperation>(previous);
            *slot = reinterpret_cast<ULONG_PTR>(&CloseResult);
        } else if (requested == Kind::Commit) {
            *slot = reinterpret_cast<ULONG_PTR>(&CommitResult);
        } else {
            realWrite = reinterpret_cast<Write>(previous);
            *slot = reinterpret_cast<ULONG_PTR>(&WriteResult);
        }
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof *slot, oldProtection, &ignored);
    }
    ~CrtFault() {
        if (!slot) return;
        DWORD oldProtection = 0, ignored = 0;
        if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &oldProtection)) {
            *slot = previous;
            VirtualProtect(slot, sizeof *slot, oldProtection, &ignored);
        }
    }
    CrtFault(const CrtFault&) = delete;
    CrtFault& operator=(const CrtFault&) = delete;
};

void FailedSave(Files& files, Fixture& fixture) {
    const auto previous = Read(files.path);
    std::wcscpy(fixture.model->path, L"previous.pmd");
#if !MMD_ORIGINAL_PROFILE
    const AuthoredSnapshot authored(fixture);
#endif
    const auto unchanged = [&] (const char* label) {
        Check(Read(files.path) == previous && std::wstring(fixture.model->path) == L"previous.pmd", label);
        Check(std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
              "failed save removes its partial temporary file");
#if !MMD_ORIGINAL_PROFILE
        authored.Unchanged();
#endif
    };
    for (const auto kind : {CrtFault::Kind::Write, CrtFault::Kind::ShortWrite,
                            CrtFault::Kind::Commit, CrtFault::Kind::Close}) {
        for (const unsigned failAt : {1u, 8u}) {
            if (failAt != 1 && (kind == CrtFault::Kind::Commit || kind == CrtFault::Kind::Close)) continue;
            DWORD error = ERROR_SUCCESS;
            {
                CrtFault fault(kind, failAt);
                Check(fault.slot != nullptr, "install a scoped fault on the production CRT import");
                if (!fault.slot) continue;
                SetLastError(ERROR_SUCCESS);
                SaveEnhancedModel(fixture.app.get(), files.path.c_str());
                error = GetLastError();
                Check(CrtFault::calls == failAt, "writer stops at the fault and attempts flush/close only once");
            }
            Check(error == ERROR_WRITE_FAULT, "write, flush or close failure reports an unsuccessful save");
            unchanged("first or later write, short-write, flush and close faults preserve destination and path");
        }
    }
    // Deny replacement while allowing ordinary writes. Direct truncation would
    // succeed with this sharing mode; a failed atomic publish must preserve it.
    HANDLE locked = CreateFileW(files.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock existing PMD against replacement only");
    if (locked != INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_SUCCESS);
        SaveEnhancedModel(fixture.app.get(), files.path.c_str());
        const DWORD error = GetLastError();
        CloseHandle(locked);
        Check(error == ERROR_WRITE_FAULT, "failed publication reports write failure");
        unchanged("denied replacement preserves the original PMD bytes and prior model path");
    }
    const auto missing = files.directory / L"missing" / L"new.pmd";
    SetLastError(ERROR_SUCCESS);
    SaveEnhancedModel(fixture.app.get(), missing.c_str());
    Check(GetLastError() == ERROR_WRITE_FAULT && !fs::exists(missing), "unavailable destination creates no partial PMD");
    unchanged("failure to create the temporary PMD preserves destination and prior model path");
    SaveEnhancedModel(fixture.app.get(), files.path.c_str());
    Check(Read(files.path) == previous && std::wstring(fixture.model->path) == files.path.wstring(),
          "save succeeds again with identical PMD bytes after all faults are removed");
#if !MMD_ORIGINAL_PROFILE
    authored.Unchanged(files.path.c_str());
#endif
}
#if !MMD_ORIGINAL_PROFILE
void VertexExportIsolation() {
    Files files; Fixture f;
    std::array<mdl::MorphRecord, 1> morphs{};
    std::array<mdl::PmdVertexMorphEntry, 3> entries{{
        {0, {10.f, 20.f, 30.f}}, {2, {40.f, 50.f, 60.f}}, {0, {-1.f, -2.f, -3.f}}}};
    std::strcpy(morphs[0].name, "base");
    morphs[0].vertexEntries = entries.data(); morphs[0].offsetCount = 3;
    f.model->morphs = morphs.data(); f.model->morphCount = 1;
    f.model->morph0Table = entries.data(); f.model->morph0Count = 3;
    std::wcscpy(f.model->path, L"original-source.pmd");
    f.app->CurrentFrame() = 37; f.app->state.sceneModified = 0;
    f.app->RecordedFrameCountByte1() = 7; f.app->EnhancedModelDirty() = 5;
    for (unsigned i = 0; i < f.vertices.size(); ++i) {
        f.vertices[i].position[2] = 7.f + i;
        f.vertices[i].normal[0] = .25f * (i + 1);
        f.vertices[i].uv[1] = -.5f * (i + 1);
        f.vertices[i].edgeDisabled = i ? 0 : 3;
    }
    // Reuse/Remember only compare these identities; no D3D method is called.
    // This is a CPU cache-validity test, not rendering with fake COM objects.
    f.model->loadComplete = 1;
    f.model->vertexBuffer = reinterpret_cast<void*>(std::uintptr_t(1));
    f.model->vertexBuffer2 = reinterpret_cast<void*>(std::uintptr_t(2));
    auto* raw = reinterpret_cast<unsigned char*>(f.model.get());
    struct CacheCleanup { unsigned char* model; ~CacheCleanup() { model_vertex_cache::Forget(model); } } cleanup{raw};
    model_vertex_cache::SetEnabledForTests(true);
    Check(!model_vertex_cache::Reuse(f.app.get(), raw, 1.f) &&
          !model_vertex_cache::Reuse(f.app.get(), raw, 1.f), "prime the actual CPU cache with two stable input descriptions");
    model_vertex_cache::Remember(f.app.get(), raw, 1.f);
    Check(model_vertex_cache::Reuse(f.app.get(), raw, 1.f), "CPU cache is reusable before export");
    const AuthoredSnapshot authored(f); const auto originalEntries = entries; const auto originalMorphs = morphs;
    const auto unchanged = [&] {
        authored.Unchanged(files.path.c_str());
        Check(!std::memcmp(entries.data(), originalEntries.data(), sizeof entries) &&
              !std::memcmp(morphs.data(), originalMorphs.data(), sizeof morphs),
              "full export preserves morph-0 definitions and their identities");
        const auto before = model_vertex_cache::GetStats();
        const auto hit = model_vertex_cache::Reuse(f.app.get(), raw, 1.f);
        const auto after = model_vertex_cache::GetStats();
        Check(hit && after.hits == before.hits + 1 && after.misses == before.misses && after.stores == before.stores,
              "full export does not invalidate or rebuild an already completed CPU cache");
    };
    SaveEnhancedModel(f.app.get(), files.path.c_str()); unchanged();
    const auto bytes = Read(files.path);
    constexpr std::size_t firstVertex = 283 + 4;
    const std::array<std::array<float, 3>, 3> expected{{{{-1.f,-2.f,-3.f}}, {{1.f,0.f,8.f}}, {{40.f,50.f,60.f}}}};
    bool exact = bytes.size() >= firstVertex + f.vertices.size() * 38;
    for (unsigned i = 0; exact && i < f.vertices.size(); ++i) {
        const auto* v = bytes.data() + firstVertex + i * 38;
        exact = !std::memcmp(v, expected[i].data(), 12) &&
            !std::memcmp(v + 12, f.vertices[i].normal, 12) && !std::memcmp(v + 24, f.vertices[i].uv, 8) &&
            !std::memcmp(v + 32, f.vertices[i].bone, 4) && v[36] == f.vertices[i].weightPercent &&
            v[37] == (f.vertices[i].edgeDisabled ? 1 : 0);
    }
    Check(exact, "real PMD contains last-wins morph-0 absolute positions and all original non-position vertex fields");
    SaveEnhancedModel(f.app.get(), files.path.c_str());
    Check(Read(files.path) == bytes, "repeated pure exports are byte-identical"); unchanged();
    for (const auto kind : {CrtFault::Kind::Write, CrtFault::Kind::ShortWrite, CrtFault::Kind::Commit, CrtFault::Kind::Close}) {
        DWORD error;
        { CrtFault fault(kind, kind == CrtFault::Kind::Write || kind == CrtFault::Kind::ShortWrite ? 8u : 1u);
          Check(fault.slot != nullptr, "install actual CRT fault during morph-0 PMD export");
          SetLastError(ERROR_SUCCESS); SaveEnhancedModel(f.app.get(), files.path.c_str()); error = GetLastError(); }
        Check(error == ERROR_WRITE_FAULT && Read(files.path) == bytes &&
              std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
              "failed morph-0 write/flush/close preserves the destination and removes temporary output"); unchanged();
    }
    HANDLE locked = CreateFileW(files.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "deny only replacement during pure morph-0 export");
    if (locked != INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_SUCCESS); SaveEnhancedModel(f.app.get(), files.path.c_str()); const auto error = GetLastError(); CloseHandle(locked);
        Check(error == ERROR_WRITE_FAULT && Read(files.path) == bytes, "failed morph-0 replacement leaves destination intact"); unchanged();
    }
    model_vertex_cache::Forget(raw);
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        if (invalid == 0) f.model->rawVertices = nullptr;
        if (invalid == 1) f.model->morph0Table = nullptr;
        if (invalid == 2) entries[1].vertexIndex = f.model->vertexCount;
        const AuthoredSnapshot invalidInput(f);
        SetLastError(ERROR_SUCCESS); SaveEnhancedModel(f.app.get(), files.path.c_str());
        Check(GetLastError() == ERROR_INVALID_DATA && Read(files.path) == bytes &&
              std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
              "invalid raw/morph-0 storage or index is refused before creating output");
        invalidInput.Unchanged();
        f.model->rawVertices = f.vertices.data(); f.model->morph0Table = entries.data(); entries[1] = originalEntries[1];
    }
    // Keep the original pre-pass condition, including dormant table metadata.
    // A null morph list makes the table irrelevant; a zero base count likewise
    // does not dereference a null table when the morph list exists.
    for (const bool nullList : {true, false}) {
        f.model->morphs = nullList ? nullptr : morphs.data(); f.model->morphCount = nullList ? 0 : 1;
        f.model->morph0Table = nullptr; f.model->morph0Count = nullList ? 3 : 0;
        const AuthoredSnapshot dormant(f);
        SaveEnhancedModel(f.app.get(), files.path.c_str()); const auto dormantBytes = Read(files.path); dormant.Unchanged();
        Check(dormantBytes.size() >= firstVertex + 3 * 38 &&
              !std::memcmp(dormantBytes.data() + firstVertex, f.vertices[0].position, 12) &&
              !std::memcmp(dormantBytes.data() + firstVertex + 2 * 38, f.vertices[2].position, 12),
              "ignored morph-0 table metadata leaves the original vertex positions in the PMD");
    }
}
void NameFidelity() {
    Files files; Fixture fixture; auto& model = *fixture.model;
    struct TextCleanup {
        mdl::ModelRecord& model;
        ~TextCleanup() { for (auto*& value : model.pmxTextBuffers) { owned_text::Free(value); value = nullptr; } }
    } cleanup{model};
    const std::string name(20, 'n'), english(20, 'e'), localComment(256, 'c'), englishComment(256, 'd');
    std::memcpy(model.name, name.data(), name.size()); std::memcpy(model.nameEn, english.data(), english.size());
    std::memcpy(model.comment, localComment.data(), localComment.size());
    std::memcpy(model.commentEn, englishComment.data(), englishComment.size());
    Check(extended_names::Assign(model.pmxTextBuffers[1], std::wstring(20, L'e')) &&
          extended_names::Assign(model.pmxTextBuffers[2], std::wstring(256, L'c')) &&
          extended_names::Assign(model.pmxTextBuffers[3], std::wstring(256, L'd')),
          "prepare matching full-width Unicode and raw PMD metadata");
    SaveEnhancedModel(fixture.app.get(), files.path.c_str());
    const auto original = Read(files.path);
    // Walk the standard triangle body through the English-presence byte.
    constexpr std::size_t englishOffset = 283 + 4 + 3 * 38 + 4 + 3 * 2 + 4 + 70 + 2 + 39 + 2 + 2 + 1 + 1 + 4 + 1;
    Check(original.size() == 1841 &&
          !std::memcmp(original.data() + 7, name.data(), 20) &&
          !std::memcmp(original.data() + 27, localComment.data(), 256) &&
          !std::memcmp(original.data() + englishOffset, english.data(), 20) &&
          !std::memcmp(original.data() + englishOffset + 20, englishComment.data(), 256),
          "production PMD writer preserves all twenty-byte names and 256-byte comments without a terminator");
    std::wcscpy(model.path, L"previous-names.pmd");
    for (unsigned invalid = 0; invalid != 3; ++invalid) {
        Check(extended_names::Assign(model.pmxTextBuffers[1], invalid == 0 ? std::wstring(20, L'x') : std::wstring(20, L'e')) &&
              extended_names::Assign(model.pmxTextBuffers[3], invalid == 1 ? L"outside CP932 😀" :
                  invalid == 2 ? std::wstring(257, L'd') : std::wstring(256, L'd')),
              "prepare an unexportable canonical name or comment with unchanged raw fields");
        SetLastError(ERROR_SUCCESS); SaveEnhancedModel(fixture.app.get(), files.path.c_str());
        Check(GetLastError() == ERROR_INVALID_DATA && Read(files.path) == original &&
              std::wstring(model.path) == L"previous-names.pmd" &&
              std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
              "actual PMD save refuses stale or unrepresentable metadata without modifying destination, path or leaving temporary files");
    }
}
#endif
}

int main() {
    Files files; Fixture fixture;
    SaveEnhancedModel(fixture.app.get(), files.path.c_str());
    const auto bytes = Read(files.path);
    Check(bytes.size() == 1841, "triangle PMD has its complete standard body");
    Check(bytes.size() >= 7 && std::memcmp(bytes.data(), "Pmd", 3) == 0,
          "production save emits the PMD header");
    // Header 283, vertex count 4, three vertices of 38 bytes, index count 4.
    constexpr std::size_t indexCountOffset = 283 + 4 + 3 * 38;
    constexpr std::size_t indexOffset = indexCountOffset + 4;
    const std::array<unsigned char, 10> expected{{3, 0, 0, 0, 2, 0, 0, 0, 1, 0}};
    Check(bytes.size() >= indexOffset + 6 &&
          std::memcmp(bytes.data() + indexCountOffset, expected.data(), expected.size()) == 0,
          "four-byte CPU slots serialize as packed triangle indices 2, 0, 1");
    if (bytes.size() >= indexOffset + 6) {
        std::array<std::uint16_t, 3> triangle{};
        std::memcpy(triangle.data(), bytes.data() + indexOffset, 6);
        bool valid = true;
        for (const auto index : triangle) valid = valid && index < fixture.vertices.size();
        float twiceArea = 0;
        if (valid) {
            const auto& a = fixture.vertices[triangle[0]];
            const auto& b = fixture.vertices[triangle[1]];
            const auto& c = fixture.vertices[triangle[2]];
            twiceArea = (b.position[0] - a.position[0]) * (c.position[1] - a.position[1]) -
                        (b.position[1] - a.position[1]) * (c.position[0] - a.position[0]);
        }
        Check(valid && twiceArea == 1, "re-read triangle keeps its winding and nonzero area");
    }
    Check(std::wstring(fixture.model->path) == files.path.wstring(), "successful save records the output path for subsequent PMM references");
    FailedSave(files, fixture);
#if !MMD_ORIGINAL_PROFILE
    VertexExportIsolation();
    NameFidelity();
#endif
    std::printf("pmd_index_save_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
