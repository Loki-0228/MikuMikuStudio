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
    const auto unchanged = [&] (const char* label) {
        Check(Read(files.path) == previous && std::wstring(fixture.model->path) == L"previous.pmd", label);
        Check(std::distance(fs::directory_iterator(files.directory), fs::directory_iterator{}) == 1,
              "failed save removes its partial temporary file");
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
}
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
    Check(std::wstring(fixture.model->path) == files.path.wstring(), "successful save records the output path");
    FailedSave(files, fixture);
    std::printf("pmd_index_save_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
