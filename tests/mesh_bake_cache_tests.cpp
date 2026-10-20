// MAINTENANCE: hidden CPU/file regression for mesh_bake_cache over real Ogawa
// archives.  No window, no D3D device and no core library: the module reaches
// the scene only through MMDApp::ModelSlot / MMDApp::AccessorySlot, so this
// regression supplies the constructor it needs and fills the slots with dummy
// objects.  Every fixture archive is written with the real Alembic package and
// read back through the module.
//
// Run: mesh_bake_cache_tests.exe [artifact directory]
//   default artifact directory:
//     .local/artifacts/alembic-export/mesh-cache/fixtures
// The regression deletes and recreates that directory, so it must not be the
// directory the executable itself runs from.

#define NOMINMAX

#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_limits.hpp"

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mikumikustudio {

// The regression never creates a window, a device or a renderer, and the module
// only reads the two slot arrays.  Supplying the constructor here keeps the test
// free of the core library and of the whole GPU/UI link surface.
MMDApp::MMDApp() {}

}  // namespace mikumikustudio

namespace {

namespace Abc = Alembic::Abc;
namespace Geom = Alembic::AbcGeom;

using mikumikustudio::MMDApp;
using mikumikustudio::MMDAppState;
using mikumikustudio::deformed_mesh_capture::Mesh;
using mikumikustudio::kModelSlotCount;
namespace mdl = mikumikustudio::mdl;
using mikumikustudio::mesh_bake_cache::Clear;
using mikumikustudio::mesh_bake_cache::Empty;
using mikumikustudio::mesh_bake_cache::Forget;
using mikumikustudio::mesh_bake_cache::Has;
using mikumikustudio::mesh_bake_cache::Install;
using mikumikustudio::mesh_bake_cache::kRecordKey;
using mikumikustudio::mesh_bake_cache::kRecordKeyV3;
using mikumikustudio::mesh_bake_cache::Load;
using mikumikustudio::mesh_bake_cache::Sample;
using mikumikustudio::mesh_bake_cache::Save;
using mikumikustudio::mesh_bake_cache::BindGeneration;
using mikumikustudio::mesh_bake_cache::EntryInfo;
using mikumikustudio::mesh_bake_cache::Info;
namespace extension = mikumikustudio::pmm_extension;

void Check(bool ok, const std::string& why) {
    if (ok) return;
    std::printf("FAIL: %s\n", why.c_str());
    std::exit(1);
}

void Trace(const std::string& what) { std::printf("  %s\n", what.c_str()); }

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// A slot entry is only ever compared as an address, never dereferenced, so any
// distinct dummy address stands in for the model or accessory record.
struct FakeObject {
    alignas(std::max_align_t) char storage[64] = {};
};

void ResetSlots(MMDApp& app) {
    for (int slot = 0; slot < kModelSlotCount; ++slot) app.ModelSlot(slot) = nullptr;
    for (int slot = 0; slot < 255; ++slot) app.AccessorySlot(slot) = nullptr;
}

struct ChildSpec {
    std::string name;
    float xOffset = 0.0f;
};

// Writes a real Ogawa archive.  Each child is a triangle whose X depends on the
// sample index and on the child offset, so a wrong sample or a wrong child is
// visible in the sampled positions.  `nativePatch` adds the inclusive frame
// range a native bake carries, and `firstFrame`/`lastFrame` say which one, so a
// retime can be checked against the frame limit of the timeline.
void WriteArchive(const std::filesystem::path& path,
                  const std::vector<ChildSpec>& children, int samples, double fps,
                  double startSeconds, bool nativePatch=false, int firstFrame=30,
                  int lastFrame=32) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Check(stream.is_open(), "create fixture archive " + path.u8string());
    {
        // The archive must be destroyed (its index and trailer written) before
        // the stream is flushed and closed.
        Alembic::AbcCoreAbstract::MetaData metadata;
        metadata.set("application", "MikuMikuStudio-mesh_bake_cache-tests");
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive()(&stream, metadata));
        const auto sampling =
            archive.addTimeSampling(Abc::TimeSampling(1.0 / fps, startSeconds));
        auto root = archive.getTop();
        if(nativePatch){Abc::OInt32Property(root.getProperties(),"mdsBakeFirstFrame").set(firstFrame);Abc::OInt32Property(root.getProperties(),"mdsBakeLastFrame").set(lastFrame);}
        for (const auto& child : children) {
            Geom::OPolyMesh shape(root, child.name, sampling);
            Abc::OUInt32ArrayProperty sourceIds;
            if(nativePatch){Abc::OBoolProperty(shape.getSchema().getUserProperties(),"mdsVertexPatch").set(true);sourceIds=Abc::OUInt32ArrayProperty(shape.getSchema().getUserProperties(),"mdsSourceVertices",sampling);}
            for (int sample = 0; sample < samples; ++sample) {
                const float shift = child.xOffset + float(sample);
                std::vector<Abc::V3f> positions{{shift, 0.0f, 0.0f},
                                                {shift + 1.0f, 0.0f, 0.0f},
                                                {shift, 1.0f, 0.0f}};
                std::vector<Abc::V2f> uvs{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
                std::vector<std::int32_t> indices{0, 1, 2};
                std::vector<std::int32_t> counts{3};
                Geom::OPolyMeshSchema::Sample meshSample{
                    Abc::P3fArraySample(positions), Abc::Int32ArraySample(indices),
                    Abc::Int32ArraySample(counts)};
                meshSample.setUVs(Geom::OV2fGeomParam::Sample(
                    Abc::V2fArraySample(uvs), Geom::kVertexScope));
                shape.getSchema().set(meshSample);
                if(nativePatch){const uint32_t ids[]{0,UINT32_MAX,2};sourceIds.set(Abc::UInt32ArraySample(ids,3));}
            }
        }
    }
    stream.flush();
    stream.close();
    Check(!stream.fail(), "write fixture archive " + path.u8string());
}

// Writes one child that is an empty mesh in every frame but the last, the shape
// alembic_export::Export produces when a mesh only appears in the middle of the
// timeline: the missing frames keep their place in the sample list.
void WriteSparseArchive(const std::filesystem::path& path,
                        const std::string& childName, int samples) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Check(stream.is_open(), "create fixture archive " + path.u8string());
    {
        Alembic::AbcCoreAbstract::MetaData metadata;
        metadata.set("application", "MikuMikuStudio-mesh_bake_cache-tests");
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive()(&stream, metadata));
        const auto sampling =
            archive.addTimeSampling(Abc::TimeSampling(1.0 / 30.0, 0.0));
        auto root = archive.getTop();
        Geom::OPolyMesh shape(root, childName, sampling);
        for (int sample = 0; sample < samples; ++sample) {
            std::vector<Abc::V3f> positions;
            std::vector<Abc::V2f> uvs;
            std::vector<std::int32_t> indices;
            std::vector<std::int32_t> counts;
            if (sample + 1 == samples) {
                positions = {{7.0f, 0.0f, 0.0f},
                             {8.0f, 0.0f, 0.0f},
                             {7.0f, 1.0f, 0.0f}};
                uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
                indices = {0, 1, 2};
                counts = {3};
            }
            Geom::OPolyMeshSchema::Sample meshSample{
                Abc::P3fArraySample(positions), Abc::Int32ArraySample(indices),
                Abc::Int32ArraySample(counts)};
            meshSample.setUVs(Geom::OV2fGeomParam::Sample(Abc::V2fArraySample(uvs),
                                                          Geom::kVertexScope));
            shape.getSchema().set(meshSample);
        }
    }
    stream.flush();
    stream.close();
    Check(!stream.fail(), "write fixture archive " + path.u8string());
}

void ExpectTriangle(const Mesh& mesh, const std::string& childName, float x0,
                    const std::string& why) {    Check(mesh.name == childName, why + ": child name");
    Check(mesh.positions.size() == 3, why + ": vertex count");
    Check(mesh.uvs.size() == 3, why + ": uv count");
    Check(mesh.indices.size() == 3, why + ": index count");
    Check(std::abs(mesh.positions[0][0] - x0) < 1e-4f,
          why + ": sampled position (got " + std::to_string(mesh.positions[0][0]) +
              ", want " + std::to_string(x0) + ")");
    Check(std::abs(mesh.positions[0][1]) < 1e-6f &&
              std::abs(mesh.positions[0][2]) < 1e-6f,
          why + ": sampled position on the other axes");
    Check(mesh.uvs[1][0] == 1.0f && mesh.uvs[2][1] == 1.0f, why + ": uvs survive");
    Check(mesh.indices[0] == 0 && mesh.indices[1] == 1 && mesh.indices[2] == 2,
          why + ": topology survives");
}

std::vector<Mesh> SampleAt(const void* object, double seconds,
                           const std::string& why) {
    std::vector<Mesh> meshes;
    std::string error;
    const bool ok = Sample(object, seconds, meshes, error);
    Check(ok, why + ": sample succeeds (" + error + ")");
    return meshes;
}

void SampleFails(const void* object, double seconds, const std::string& why) {
    std::vector<Mesh> meshes;
    std::string error;
    Check(!Sample(object, seconds, meshes, error), why + ": sample fails");
    Check(!error.empty(), why + ": the failure carries a message");
    Check(meshes.empty(), why + ": a failed sample returns nothing");
}

// A retimed sample time that is meant to land on a baked frame boundary.  The
// epsilon keeps the lookup from depending on whether a division and a
// multiplication round trip lands exactly on the recorded sample time.
constexpr double kRetimeEpsilon = 1e-9;

// ---------------------------------------------------------------------------
// Module files
// ---------------------------------------------------------------------------

// Mirrors the module's private layout: one shared root that every instance of
// the application uses, and one random per-process directory under it.
std::filesystem::path SharedTempRoot() {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) base = std::filesystem::current_path(ec);
    return base / "MikuMikuStudio" / "mesh_bake_cache";
}

// The private directories of this process, found by the process id the module
// puts in front of the random suffix.
std::vector<std::filesystem::path> SessionDirectories() {
    std::vector<std::filesystem::path> found;
    const std::string prefix =
        "mesh-cache-" + std::to_string(GetCurrentProcessId()) + "-";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(SharedTempRoot(), ec)) {
        std::error_code nameError;
        if (!entry.is_directory(nameError)) continue;
        const auto name = entry.path().filename().u8string();
        if (name.rfind(prefix, 0) == 0) found.push_back(entry.path());
    }
    return found;
}

// Copies the module made, i.e. the ".abc" files inside its own directories.
std::size_t CountTempCopies() {
    std::size_t count = 0;
    for (const auto& directory : SessionDirectories()) {
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(directory, ec)) {
            std::error_code fileError;
            if (entry.is_regular_file(fileError) &&
                entry.path().extension() == ".abc")
                ++count;
        }
    }
    return count;
}

// Private directories that appeared since an earlier listing.
std::vector<std::filesystem::path> NewDirectories(
    const std::vector<std::filesystem::path>& before) {
    std::vector<std::filesystem::path> created;
    for (const auto& candidate : SessionDirectories()) {
        bool known = false;
        for (const auto& previous : before) known = known || previous == candidate;
        if (!known) created.push_back(candidate);
    }
    return created;
}

// The same count for one known directory.
std::size_t CountCopiesIn(const std::filesystem::path& directory) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory, ec)) {
        std::error_code fileError;
        if (entry.is_regular_file(fileError) && entry.path().extension() == ".abc")
            ++count;
    }
    return count;
}

std::size_t CountSidecars(const std::filesystem::path& directory) {
    std::error_code ec;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const auto name = entry.path().filename().u8string();
        const std::string suffix = mikumikustudio::mesh_bake_cache::kSidecarSuffix;
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            ++count;
    }
    return count;
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Check(stream.is_open(), "read back " + path.u8string());
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// PMM extension fixtures
// ---------------------------------------------------------------------------

// The record layout is re-implemented here on purpose: Save is checked against
// this independent reader, and the negative cases are built with the same
// writer, so a layout drift in the module cannot silently pass.  Version 2
// repeats the version 1 layout and appends the time scale of every entry as an
// IEEE 754 binary64, so the same writer covers both.
struct RawEntry {
    int kind = 0;
    int slot = 0;
    std::string path;
    double timeScale = 1;
};

constexpr std::uint32_t kMagic = 0x43534D4Du;  // 'M','M','S','C'
constexpr std::uint32_t kRecordVersionV2 = 2;

void AppendRawF64(std::vector<std::uint8_t>& value, double number) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &number, sizeof bits);
    for (int shift = 0; shift < 64; shift += 8)
        value.push_back(std::uint8_t((bits >> shift) & 0xFFu));
}

std::vector<std::uint8_t> BuildValue(const std::vector<RawEntry>& entries,
                                     std::uint32_t version = 1) {
    std::vector<std::uint8_t> value;
    const auto u32 = [&value](std::uint32_t number) {
        for (int shift = 0; shift < 32; shift += 8)
            value.push_back(std::uint8_t((number >> shift) & 0xFFu));
    };
    u32(kMagic);
    u32(version);
    u32(std::uint32_t(entries.size()));
    for (const auto& entry : entries) {
        value.push_back(std::uint8_t(entry.kind));
        value.push_back(0);
        value.push_back(0);
        value.push_back(0);
        u32(std::uint32_t(entry.slot));
        u32(std::uint32_t(entry.path.size()));
        value.insert(value.end(), entry.path.begin(), entry.path.end());
        if (version == kRecordVersionV2) AppendRawF64(value, entry.timeScale);
    }
    return value;
}

std::vector<RawEntry> ParseValue(const std::vector<std::uint8_t>& value,
                                 const std::string& why,
                                 std::uint32_t expectedVersion = 1) {
    std::size_t cursor = 0;
    const auto u32 = [&]() {
        Check(cursor + 4 <= value.size(), why + ": header fits");
        const std::uint32_t number =
            std::uint32_t(value[cursor]) | (std::uint32_t(value[cursor + 1]) << 8) |
            (std::uint32_t(value[cursor + 2]) << 16) |
            (std::uint32_t(value[cursor + 3]) << 24);
        cursor += 4;
        return number;
    };
    Check(u32() == kMagic, why + ": magic");
    Check(u32() == expectedVersion, why + ": version");
    const std::uint32_t count = u32();
    std::vector<RawEntry> entries;
    for (std::uint32_t index = 0; index < count; ++index) {
        Check(cursor + 8 <= value.size(), why + ": entry header fits");
        RawEntry entry;
        entry.kind = value[cursor];
        Check(value[cursor + 1] == 0 && value[cursor + 2] == 0 &&
                  value[cursor + 3] == 0,
              why + ": reserved bytes are zero");
        cursor += 4;
        entry.slot = int(u32());
        const std::uint32_t pathBytes = u32();
        Check(cursor + pathBytes <= value.size(), why + ": path fits");
        entry.path.assign(reinterpret_cast<const char*>(value.data() + cursor),
                          pathBytes);
        cursor += pathBytes;
        if (expectedVersion == kRecordVersionV2) {
            Check(cursor + 8 <= value.size(), why + ": time scale fits");
            std::uint64_t bits = 0;
            for (int shift = 0; shift < 64; shift += 8)
                bits |= std::uint64_t(value[cursor + std::size_t(shift / 8)]) << shift;
            std::memcpy(&entry.timeScale, &bits, sizeof entry.timeScale);
            cursor += 8;
        }
        entries.push_back(std::move(entry));
    }
    Check(cursor == value.size(), why + ": no trailing bytes");
    return entries;
}

constexpr std::size_t kStandardPrefixBytes = 32;

void AppendExtension(const std::filesystem::path& path,
                     const extension::Document& document) {
    const int fd = _wopen(path.c_str(), _O_RDWR | _O_BINARY);
    Check(fd >= 0, "open the project file for the extension");
    Check(_lseeki64(fd, 0, SEEK_END) == _filelengthi64(fd),
          "the extension starts at end of file");
    Check(extension::Append(fd, document), "append the project extension");
    _close(fd);
}

void WriteProjectFile(const std::filesystem::path& path,
                      const extension::Document& document) {
    const std::string standard = "Polygon Movie maker 0002" + std::string(8, 'x');
    Check(standard.size() == kStandardPrefixBytes, "fixture prefix size");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Check(stream.is_open(), "create the project file");
        stream.write(standard.data(), std::streamsize(standard.size()));
    }
    AppendExtension(path, document);
}

bool ReadProjectFile(const std::filesystem::path& path,
                     extension::Document& document, std::uint64_t& standardSize) {
    const int fd = _wopen(path.c_str(), _O_RDONLY | _O_BINARY);
    if (fd < 0) return false;
    const auto result = extension::Read(fd, document, standardSize);
    _close(fd);
    return result == extension::ReadResult::Loaded;
}

// ---------------------------------------------------------------------------
// Scene stand-in
// ---------------------------------------------------------------------------

const char* kModelChild = "model_0_material_0_shader_11_pass_0";
const char* kModelGroupChild = "model_0_material_1_shader_12_pass_0";
const char* kAccessoryChild = "accessory_2_material_1_shader_7_pass_0";
const char* kSecondModelChild = "model_1_material_0_shader_13_pass_0";

struct Scene {
    explicit Scene(MMDApp& app) : app(app) {}

    MMDApp& app;
    FakeObject model0;
    FakeObject model1;
    FakeObject accessory2;
    FakeObject unbound;

    void BindModel0() {
        app.ModelSlot(0) = reinterpret_cast<unsigned char*>(&model0);
    }
    void BindModel1() {
        app.ModelSlot(1) = reinterpret_cast<unsigned char*>(&model1);
    }
    void UnbindModel1() { app.ModelSlot(1) = nullptr; }
    void BindAccessory2() {
        app.AccessorySlot(2) = reinterpret_cast<mdl::AccessoryRecord*>(&accessory2);
    }
    void UnbindAccessory2() { app.AccessorySlot(2) = nullptr; }

    const void* Model0() const { return app.ModelSlot(0); }
    const void* Model1() const { return app.ModelSlot(1); }
    const void* Accessory2() const { return app.AccessorySlot(2); }
};

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

void CacheBasics(const std::filesystem::path& directory, Scene& scene,
                 std::size_t tempBaseline) {
    Trace("install, sample, clamp, replace, forget, reject");
    const auto archive = directory / L"bake-a.abc";
    WriteArchive(archive, {{kModelChild, 100.0f}, {kAccessoryChild, 200.0f}}, 4,
                 30.0, 0.0);

    scene.BindModel0();
    scene.BindAccessory2();

    std::string error;
    const bool installed = Install(scene.app, archive, error);
    Check(installed, "install the first archive: " + error);
    Check(Has(scene.Model0()), "the model object is cached");
    Check(Has(scene.Accessory2()), "the accessory object is cached");
    Check(!Has(nullptr), "a null object is never cached");
    Check(!Empty(), "the cache is not empty after an install");
    Check(CountTempCopies() > tempBaseline,
          "the module keeps its own copy of the archive");

    // The input is disposable: the module holds its own copy.
    std::error_code ec;
    std::filesystem::remove(archive, ec);
    Check(!std::filesystem::exists(archive, ec), "the fixture archive is gone");

    auto model = SampleAt(scene.Model0(), 0.0, "model at 0 s");
    Check(model.size() == 1, "one mesh for one child");
    ExpectTriangle(model[0], kModelChild, 100.0f, "model at 0 s");
    ExpectTriangle(SampleAt(scene.Model0(), 1.0 / 30.0 + 1e-9, "model at 1/30 s")[0],
                   kModelChild, 101.0f, "model at 1/30 s");
    ExpectTriangle(SampleAt(scene.Model0(), 0.5 / 30.0, "model between samples")[0],
                   kModelChild, 100.0f, "the nearest sample at or before the time");
    ExpectTriangle(SampleAt(scene.Model0(), 1e6, "model past the end")[0], kModelChild,
                   103.0f, "a time past the last sample clamps to it");
    ExpectTriangle(SampleAt(scene.Model0(), -5.0, "model before the start")[0],
                   kModelChild, 100.0f, "a time before the first sample clamps to it");
    ExpectTriangle(SampleAt(scene.Accessory2(), 2.0 / 30.0, "accessory at 2/30 s")[0],
                   kAccessoryChild, 202.0f, "the accessory reads its own child");

    SampleFails(&scene.unbound, 0.0, "an object that was never cached");
    SampleFails(nullptr, 0.0, "a null object");

    // One object can own several meshes (material/shader/pass splits).
    const auto grouped = directory / L"bake-grouped.abc";
    WriteArchive(grouped, {{kModelChild, 10.0f}, {kModelGroupChild, 20.0f}}, 2, 30.0,
                 0.0);
    Check(Install(scene.app, grouped, error), "install the grouped archive: " + error);
    const auto group = SampleAt(scene.Model0(), 0.0, "grouped model");
    Check(group.size() == 2, "one object returns its whole mesh group");
    ExpectTriangle(group[0], kModelChild, 10.0f, "group member 0");
    ExpectTriangle(group[1], kModelGroupChild, 20.0f, "group member 1");
    Check(Has(scene.Accessory2()), "replacing one object keeps the other entries");
    ExpectTriangle(
        SampleAt(scene.Accessory2(), 0.0, "accessory after an unrelated install")[0],
        kAccessoryChild, 200.0f, "the untouched entry keeps its data");

    // Forgetting one object must not disturb another, or delete its archive.
    const auto copiesBefore = CountTempCopies();
    Forget(scene.Accessory2());
    Forget(scene.Accessory2());
    Forget(nullptr);
    Check(!Has(scene.Accessory2()), "forget drops exactly that object");
    Check(Has(scene.Model0()), "forget leaves other objects alone");
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after a forget")[0],
                   kModelChild, 10.0f, "the surviving object keeps its data");
    Check(CountTempCopies() == copiesBefore - 1,
          "the released archive removes only its own private copy");

    // Rejections must leave the working cache and the temporary directory alone.
    const auto before = CountTempCopies();
    const auto foreign = directory / L"foreign.abc";
    WriteArchive(foreign, {{"torus", 0.0f}}, 1, 30.0, 0.0);
    error.clear();
    Check(!Install(scene.app, foreign, error),
          "an archive without MMD mesh names is rejected");
    Check(!error.empty(), "the rejection carries a message");
    const auto absent = directory / L"absent-slot.abc";
    WriteArchive(absent, {{kSecondModelChild, 5.0f}}, 1, 30.0, 0.0);
    Check(!Install(scene.app, absent, error),
          "an archive whose object is not in the scene is rejected");
    Check(!Install(scene.app, directory / L"missing.abc", error),
          "a missing archive is rejected");
    const auto junk = directory / L"junk.abc";
    {
        std::ofstream stream(junk, std::ios::binary | std::ios::trunc);
        stream << "this is not an ogawa archive at all";
    }
    Check(!Install(scene.app, junk, error), "a corrupt archive is rejected");
    Check(!Install(scene.app, std::filesystem::path(), error),
          "an empty path is rejected");
    Check(CountTempCopies() == before,
          "a rejected install leaves no private copy behind");
    Check(Has(scene.Model0()), "a rejected install keeps the working cache");
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after rejections")[0],
                   kModelChild, 10.0f, "the cache survives every rejection");

    // An object that is in the scene but in no archive is not cached.
    scene.BindModel1();
    Check(!Has(scene.Model1()), "an object no archive names is not cached");

    // A frame where the mesh was not drawn keeps its place in the group as an
    // empty mesh, so the group length and order never change between frames.
    const auto sparse = directory / L"bake-sparse.abc";
    const std::string sparseChild = "model_1_material_0_shader_14_pass_0";
    WriteSparseArchive(sparse, sparseChild, 2);
    Check(Install(scene.app, sparse, error), "install the sparse archive: " + error);
    const auto missing = SampleAt(scene.Model1(), 0.0, "sparse model at 0 s");
    Check(missing.size() == 1, "the frame without a mesh keeps the group length");
    Check(missing[0].name == sparseChild, "the frame without a mesh keeps the name");
    Check(missing[0].positions.empty() && missing[0].uvs.empty() &&
              missing[0].indices.empty(),
          "the frame without a mesh returns empty arrays");
    ExpectTriangle(SampleAt(scene.Model1(), 1.0 / 30.0, "sparse model at 1/30 s")[0],
                   sparseChild, 7.0f, "the frame with the mesh returns it");
    Forget(scene.Model1());
    Check(!Has(scene.Model1()), "the sparse entry is gone after a forget");
    scene.UnbindModel1();
}

void CachePersistence(const std::filesystem::path& directory, Scene& scene) {
    Trace("save, project round trip, load, strict validation");
    scene.BindModel0();
    scene.BindAccessory2();

    // model 0 is still cached from the previous stage; cache the accessory from
    // a second archive so Save has to publish two sidecars.
    const auto accessoryArchive = directory / L"bake-accessory.abc";
    WriteArchive(accessoryArchive, {{kAccessoryChild, 200.0f}}, 4, 30.0, 0.0);
    std::string error;
    Check(Install(scene.app, accessoryArchive, error),
          "install the accessory archive: " + error);

    const auto pmmPath = directory / L"\u9879\u76ee.pmm";
    extension::Document project;
    extension::Set(project, "mds.unrelated", {1, 2, 3});
    WriteProjectFile(pmmPath, project);
    const auto projectBytes = ReadFileBytes(pmmPath);

    Check(Save(scene.app, pmmPath, project, error), "save publishes the cache: " + error);
    Check(ReadFileBytes(pmmPath) == projectBytes,
          "save never opens or rewrites the project file");

    const auto* record = extension::Find(project, kRecordKey);
    Check(record != nullptr, "the document carries the cache record");
    const auto entries = ParseValue(record->value, "the saved record");
    Check(entries.size() == 2, "one record entry per cached object");
    Check(entries[0].kind == 0 && entries[0].slot == 0, "the model slot is recorded");
    Check(entries[1].kind == 1 && entries[1].slot == 2,
          "the accessory slot is recorded");
    const std::string suffix = mikumikustudio::mesh_bake_cache::kSidecarSuffix;
    for (const auto& entry : entries) {
        Check(entry.path.size() > suffix.size() &&
                  entry.path.compare(entry.path.size() - suffix.size(),
                                     suffix.size(), suffix) == 0,
              "the sidecar path carries the module suffix");
        Check(std::filesystem::exists(directory / std::filesystem::u8path(entry.path)),
              "the sidecar is published next to the project");
    }
    Check(entries[0].path != entries[1].path, "two archives publish two sidecars");
    Check(CountSidecars(directory) == 2, "the directory holds exactly two sidecars");
    Check(extension::Find(project, "mds.unrelated") != nullptr,
          "save keeps records it does not own");

    // Saving the same cache again reuses the same version-unique sidecars.
    Check(Save(scene.app, pmmPath, project, error), "save again");
    Check(CountSidecars(directory) == 2, "re-saving does not duplicate sidecars");

    // An object that has left the scene is no longer recorded.
    scene.UnbindAccessory2();
    Check(Save(scene.app, pmmPath, project, error), "save without the accessory");
    const auto trimmed = ParseValue(extension::Find(project, kRecordKey)->value,
                                    "the trimmed record");
    Check(trimmed.size() == 1 && trimmed[0].kind == 0 && trimmed[0].slot == 0,
          "an unloaded object is not recorded");
    scene.BindAccessory2();
    Check(Save(scene.app, pmmPath, project, error), "save with both objects again");

    // The application flow: the module edits the document, the caller appends it.
    WriteProjectFile(pmmPath, project);
    extension::Document reloaded;
    std::uint64_t standardSize = 0;
    Check(ReadProjectFile(pmmPath, reloaded, standardSize),
          "the project extension reads back");
    Check(standardSize == kStandardPrefixBytes,
          "the extension leaves the standard PMM prefix untouched");
    Check(extension::Find(reloaded, kRecordKey) != nullptr,
          "the module record survives the project round trip");

    Clear();
    Check(Empty(), "clear empties the cache");
    Check(CountSidecars(directory) == 2, "clear never deletes a published sidecar");
    Check(Load(scene.app, pmmPath, reloaded, error), "load the project cache: " + error);
    Check(Has(scene.Model0()) && Has(scene.Accessory2()),
          "load restores every referenced object");
    const auto model = SampleAt(scene.Model0(), 0.0, "model after a load");
    Check(model.size() == 2, "the loaded group keeps its size");
    ExpectTriangle(model[0], kModelChild, 10.0f, "the loaded group keeps its data");
    ExpectTriangle(SampleAt(scene.Accessory2(), 1.0 / 30.0, "accessory after a load")[0],
                   kAccessoryChild, 201.0f, "the loaded accessory keeps its data");

    // Every malformed record must be rejected without touching the cache.
    const auto expectRejected = [&](const std::vector<std::uint8_t>& value,
                                    const std::string& why) {
        auto broken = reloaded;
        extension::Set(broken, kRecordKey, value);
        std::string failure;
        Check(!Load(scene.app, pmmPath, broken, failure), why + ": rejected");
        Check(!failure.empty(), why + ": the rejection carries a message");
        Check(Has(scene.Model0()) && Has(scene.Accessory2()),
              why + ": the working cache survives");
        ExpectTriangle(SampleAt(scene.Model0(), 0.0, why + ": cache intact")[0],
                       kModelChild, 10.0f, why + ": cache intact");
    };

    const std::string sidecar = entries[0].path;
    {
        auto truncated = record->value;
        truncated.pop_back();
        expectRejected(truncated, "a truncated value");
    }
    {
        auto badMagic = record->value;
        badMagic[0] = 'X';
        expectRejected(badMagic, "a wrong magic number");
    }
    {
        auto badVersion = record->value;
        badVersion[4] = 9;
        expectRejected(badVersion, "an unsupported version");
    }
    {
        auto extraEntry = record->value;
        extraEntry[8] = std::uint8_t(entries.size() + 1);
        expectRejected(extraEntry, "a count larger than the value");
    }
    {
        auto trailing = record->value;
        trailing.push_back(0);
        expectRejected(trailing, "a trailing byte");
    }
    expectRejected(BuildValue({{0, 0, sidecar}, {0, 0, sidecar}}),
                   "a duplicated slot");
    expectRejected(BuildValue({{0, 9999, sidecar}}), "an out-of-range model slot");
    expectRejected(BuildValue({{1, 900, sidecar}}), "an out-of-range accessory slot");
    expectRejected(BuildValue({{2, 0, sidecar}}), "an unknown object kind");
    expectRejected(BuildValue({{0, 0, "sub/" + sidecar}}),
                   "a sidecar in a subdirectory that does not exist");
    expectRejected(BuildValue({{0, 0, "../" + sidecar}}),
                   "a sidecar path that leaves the project directory");
    expectRejected(BuildValue({{0, 0, sidecar + ".bak"}}),
                   "a path without the module suffix");
    expectRejected(BuildValue({{0, 0, ""}}), "an empty path");
    expectRejected(BuildValue({{0, 0, std::string("a\0b.mdsmesh.abc", 15)}}),
                   "a path with an embedded NUL");
    expectRejected(BuildValue({{0, 0, "absent.mdsmesh.abc"}}),
                   "a sidecar that does not exist");

    // A sidecar that does not hold the recorded object must not bind anything
    // else in its place.
    auto mismatched = reloaded;
    extension::Set(mismatched, kRecordKey, BuildValue({{0, 1, sidecar}}));
    scene.BindModel1();
    Clear();
    Check(!Load(scene.app, pmmPath, mismatched, error),
          "a sidecar that does not hold the recorded object is rejected");
    Check(Empty(), "a rejected load installs nothing");

    // Only the objects the record names are taken from a sidecar, even when the
    // sidecar holds more of them.
    const auto both = directory / L"bake-both.abc";
    WriteArchive(both, {{kModelChild, 40.0f}, {kSecondModelChild, 50.0f}}, 1, 30.0,
                 0.0);
    extension::Document twoObjects;
    WriteProjectFile(pmmPath, twoObjects);
    scene.UnbindModel1();
    Check(Install(scene.app, both, error), "install an archive with two objects");
    Check(Save(scene.app, pmmPath, twoObjects, error), "save with one live object");
    const auto single = ParseValue(extension::Find(twoObjects, kRecordKey)->value,
                                   "the single-object record");
    Check(single.size() == 1 && single[0].kind == 0 && single[0].slot == 0,
          "only the live object of the archive is recorded");
    scene.BindModel1();
    Clear();
    Check(Load(scene.app, pmmPath, twoObjects, error),
          "load the single-object record");
    Check(Has(scene.Model0()), "the recorded object is loaded");
    Check(!Has(scene.Model1()),
          "an object the record does not name is not installed from that sidecar");

    // A record with no entries is valid and installs nothing.
    extension::Document emptyRecord;
    WriteProjectFile(pmmPath, emptyRecord);
    Clear();
    Check(Load(scene.app, pmmPath, emptyRecord, error),
          "an empty record loads as a no-op");
    Check(Empty(), "an empty record installs nothing");

    // No record at all is also a no-op, and an empty cache erases a stale one.
    extension::Document stale;
    extension::Set(stale, kRecordKey, BuildValue({{0, 0, sidecar}}));
    extension::Set(stale, "mds.unrelated", {4, 5, 6});
    Clear();
    Check(Save(scene.app, pmmPath, stale, error), "save with nothing to record");
    Check(extension::Find(stale, kRecordKey) == nullptr,
          "an empty cache erases the stale record");
    Check(extension::Find(stale, "mds.unrelated") != nullptr,
          "erasing the cache keeps records it does not own");
    Check(Load(scene.app, pmmPath, stale, error), "the erased project loads as a no-op");
    Check(Empty(), "nothing was installed from the erased project");
    Check(Load(scene.app, pmmPath, extension::Document{}, error),
          "a project without a record loads as a no-op");
}

// Clear must only ever remove what this process created, and must never touch
// the shared root: another running instance keeps its own directory and files
// there, and the user may keep anything else in it.  Each check here targets
// one file, so a leftover directory from an earlier run cannot mask a mistake.
void ClearOwnership(const std::filesystem::path& directory, Scene& scene) {
    Trace("clear removes only this process's own files");
    Clear();
    Check(Empty(), "the cache starts this stage empty");

    const auto root = SharedTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);

    // Two foreign entries directly in the shared root: a file, and a directory
    // that holds a file of its own.
    const auto foreignFile = root / L"foreign-instance.tmp";
    const auto foreignDirectory = root / L"foreign-instance-dir";
    const auto foreignNested = foreignDirectory / L"other-cache.bin";
    {
        std::ofstream stream(foreignFile, std::ios::binary | std::ios::trunc);
        stream << "belongs to another instance";
    }
    std::filesystem::create_directories(foreignDirectory, ec);
    {
        std::ofstream stream(foreignNested, std::ios::binary | std::ios::trunc);
        stream << "another instance cache";
    }

    // A complete foreign private directory, as another running instance leaves.
    const auto foreignSession =
        root / L"mesh-cache-1000-0123456789abcdef0123456789abcdef";
    std::filesystem::create_directories(foreignSession, ec);
    const auto foreignSessionCopy = foreignSession / L"mesh-1.abc";
    {
        std::ofstream stream(foreignSessionCopy, std::ios::binary | std::ios::trunc);
        stream << "foreign session copy";
    }

    // The private directory this install creates is the one that was not there
    // before it.
    const auto before = SessionDirectories();
    scene.BindModel0();
    const auto archive = directory / L"bake-ownership.abc";
    WriteArchive(archive, {{kModelChild, 60.0f}}, 1, 30.0, 0.0);
    std::string error;
    Check(Install(scene.app, archive, error),
          "install into a private directory: " + error);

    std::vector<std::filesystem::path> created = NewDirectories(before);
    Check(created.size() == 1,
          "the module created exactly one private directory for this process");
    const auto session = created[0];
    Check(session != root, "the private directory is not the shared root");
    Check(CountCopiesIn(session) == 1, "the private copy sits in the private directory");

    // A file this module did not create, planted inside its own directory.
    const auto planted = session / L"planted-by-another-instance.txt";
    {
        std::ofstream stream(planted, std::ios::binary | std::ios::trunc);
        stream << "not ours";
    }

    Clear();
    Check(Empty(), "clear empties the cache");
    Check(CountCopiesIn(session) == 0,
          "clear removed the copy this module recorded");
    Check(std::filesystem::exists(planted, ec),
          "a foreign file inside the private directory survives");
    Check(std::filesystem::is_directory(session, ec),
          "the private directory survives while it holds a foreign file");
    Check(std::filesystem::exists(foreignFile, ec) &&
              ReadFileBytes(foreignFile) == "belongs to another instance",
          "a foreign file in the shared root survives");
    Check(std::filesystem::exists(foreignNested, ec),
          "a foreign directory and its file in the shared root survive");
    Check(std::filesystem::exists(foreignSessionCopy, ec),
          "another instance's private directory survives");
    Check(std::filesystem::is_directory(root, ec),
          "the shared root is never removed");

    // With the foreign file gone the empty private directory goes too, and the
    // shared root is still there.
    std::filesystem::remove(planted, ec);
    Clear();
    Check(!std::filesystem::exists(session, ec),
          "the empty private directory is removed once nothing foreign is in it");
    Check(std::filesystem::is_directory(root, ec), "the shared root still exists");

    // Take the simulated foreign entries back out; they belong to this test.
    std::filesystem::remove(foreignFile, ec);
    std::filesystem::remove_all(foreignDirectory, ec);
    std::filesystem::remove_all(foreignSession, ec);
}

// The commit callback is the caller's last chance to veto an install, and it
// exists so that work which must happen exactly when the cache changes - the
// application disables MME/MMV effects there - only runs on a valid candidate.
// It may therefore only be called after the private copy and every candidate
// entry exist, it must be able to refuse without disturbing the cache, and
// everything that can still fail must be behind it.
void InstallCallback(const std::filesystem::path& directory, Scene& scene) {
    Trace("install callback: veto, exception, ordering, commit");
    Clear();
    Check(Empty(), "the cache starts this stage empty");
    scene.BindModel0();
    scene.UnbindModel1();  // model 1 is not part of this stage

    const auto before = SessionDirectories();
    const auto first = directory / L"bake-callback-first.abc";
    WriteArchive(first, {{kModelChild, 70.0f}}, 1, 30.0, 0.0);
    std::string error;
    // The three-argument form stays source compatible: this call has no callback.
    Check(Install(scene.app, first, error), "install without a callback: " + error);
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after the first install")[0],
                   kModelChild, 70.0f, "the first archive is live");

    const auto created = NewDirectories(before);
    Check(created.size() == 1, "the first install created the private directory");
    const auto session = created[0];
    Check(CountCopiesIn(session) == 1, "the first install keeps one private copy");

    const auto second = directory / L"bake-callback-second.abc";
    WriteArchive(second, {{kModelChild, 80.0f}}, 1, 30.0, 0.0);

    // A veto: the callback runs once, sees the candidate copy already on disk,
    // refuses the install, and the previous cache must be exactly as it was.
    int calls = 0;
    bool candidatePresent = false;
    error.clear();
    const bool vetoed = Install(scene.app, second, error, [&](std::string& reason) {
        ++calls;
        // Nothing of this module is called here: the callback runs while the
        // module holds its lock.
        candidatePresent = CountCopiesIn(session) == 2;
        reason = "the caller vetoed this install";
        return false;
    });
    Check(!vetoed, "a vetoing callback refuses the install");
    Check(calls == 1, "the vetoing callback is called exactly once");
    Check(candidatePresent,
          "the private copy exists before the callback runs (old copy plus candidate)");
    Check(error == "the caller vetoed this install",
          "the callback reason reaches the caller");
    Check(CountCopiesIn(session) == 1,
          "the vetoed candidate copy is removed by the existing cleanup");
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after a veto")[0], kModelChild,
                   70.0f, "a veto leaves the previous archive live");

    // A callback that throws is a veto too, and its text reaches the caller.
    calls = 0;
    error.clear();
    const bool thrown = Install(scene.app, second, error,
                                [&](std::string&) -> bool {
                                    ++calls;
                                    throw std::runtime_error("effect teardown failed");
                                });
    Check(!thrown, "a throwing callback refuses the install");
    Check(calls == 1, "the throwing callback is called exactly once");
    Check(error.find("effect teardown failed") != std::string::npos,
          "the exception text reaches the caller: " + error);
    Check(CountCopiesIn(session) == 1, "the throwing candidate copy is removed too");
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after a throwing veto")[0],
                   kModelChild, 70.0f, "a throwing callback leaves the cache live");

    // A silent veto still reports something to the caller.
    error.clear();
    const bool silent =
        Install(scene.app, second, error, [](std::string&) { return false; });
    Check(!silent, "a silent veto refuses the install");
    Check(!error.empty(), "a silent veto still reports a reason: " + error);
    Check(CountCopiesIn(session) == 1, "a silent veto removes its candidate copy");

    // Without a valid candidate the callback must not run at all.
    const auto foreign = directory / L"bake-callback-foreign.abc";
    WriteArchive(foreign, {{"torus", 0.0f}}, 1, 30.0, 0.0);
    const auto absent = directory / L"bake-callback-absent.abc";
    WriteArchive(absent, {{kSecondModelChild, 5.0f}}, 1, 30.0, 0.0);
    const auto countCalls = [&calls](std::string&) {
        ++calls;
        return true;
    };
    calls = 0;
    Check(!Install(scene.app, foreign, error, countCalls),
          "an archive without MMD mesh names is still rejected");
    Check(!Install(scene.app, directory / L"bake-callback-missing.abc", error,
                   countCalls),
          "a missing archive is still rejected");
    Check(!Install(scene.app, absent, error, countCalls),
          "an archive whose object is not in the scene is still rejected");
    Check(calls == 0, "the callback is not called without a valid candidate");
    Check(CountCopiesIn(session) == 1,
          "rejected candidates leave the private directory as it was");
    ExpectTriangle(
        SampleAt(scene.Model0(), 0.0, "model after the rejected candidates")[0],
        kModelChild, 70.0f, "rejected candidates change nothing");

    // An accepting callback lets the candidate become the live cache, and the
    // archive it replaces releases its private copy.
    calls = 0;
    error.clear();
    const bool committed = Install(scene.app, second, error, [&](std::string&) {
        ++calls;
        return true;
    });
    Check(committed, "an accepting callback lets the install commit: " + error);
    Check(calls == 1, "the accepting callback is called exactly once");
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after the commit")[0],
                   kModelChild, 80.0f, "the candidate replaced the previous archive");
    Check(CountCopiesIn(session) == 1,
          "the replaced archive released its private copy");

    // A plain install still works after all of the above.
    const auto third = directory / L"bake-callback-third.abc";
    WriteArchive(third, {{kModelChild, 90.0f}}, 1, 30.0, 0.0);
    Check(Install(scene.app, third, error), "install without a callback still works");
    ExpectTriangle(SampleAt(scene.Model0(), 0.0, "model after a plain install")[0],
                   kModelChild, 90.0f, "the plain install is live");
    Check(CountCopiesIn(session) == 1, "the plain install released the replaced copy");

    Clear();
    Forget(scene.Model0());
}

void NativeRange(const std::filesystem::path& directory,Scene& scene){
    Clear();const auto archive=directory/L"native-range.abc";std::string error;
    WriteArchive(archive,{{kModelChild,100.f}},3,30.,1.,true);
    Check(Install(scene.app,archive,error),"install bounded source-vertex patch: "+error);
    namespace cache=mikumikustudio::mesh_bake_cache;
    Check(cache::RangeLimited(scene.Model0()),"range policy is read from native archive");
    Check(!cache::ActiveAt(scene.Model0(),29./30.)&&cache::ActiveAt(scene.Model0(),1.)&&cache::ActiveAt(scene.Model0(),32.9/30.)&&!cache::ActiveAt(scene.Model0(),33./30.),"cache and discrete FX switches share frame interval");
    const auto meshes=SampleAt(scene.Model0(),31./30.,"source-vertex patch");
    Check(meshes[0].vertexPatch&&meshes[0].sourceVertices==std::vector<uint32_t>({0,UINT32_MAX,2}),"unselected vertex sentinel and selected source indices survive ABC");
    extension::Document document;const auto project=directory/L"native-range.pmm";
    Check(Save(scene.app,project,document,error),"save range-limited cache: "+error);Clear();
    Check(Load(scene.app,project,document,error),"reopen range-limited cache: "+error);
    Check(cache::RangeLimited(scene.Model0())&&!cache::ActiveAt(scene.Model0(),0)&&cache::ActiveAt(scene.Model0(),1.),"PMM sidecar restores range metadata");
    Check(SampleAt(scene.Model0(),1.,"reopened patch")[0].sourceVertices[1]==UINT32_MAX,"PMM sidecar restores selection");
    Clear();Trace("native frame range and sparse source vertices round-trip");
}

// The retime maps the timeline the caller samples onto the bake: Sample reads an
// archive at seconds / timeScale, and ActiveAt maps the inclusive frame range of
// a native archive through the same factor, rounded half up to a whole 30 fps
// frame.  A plan is built while the cache is untouched, is refused once the cache
// changed under it, and the version 2 record is what carries the scale across a
// project save.
void TimeScale(const std::filesystem::path& directory, Scene& scene) {
    namespace cache = mikumikustudio::mesh_bake_cache;
    Clear();
    Check(Empty(), "the retime stage starts with an empty cache");
    scene.BindModel0();
    scene.UnbindModel1();
    scene.UnbindAccessory2();

    std::error_code ec;
    const auto stage = directory / L"retime";
    std::filesystem::create_directories(stage, ec);
    Check(!ec, "create the retime stage directory");

    // Frames 30..32 at 30 fps: an inclusive range that starts at 1 s and ends
    // before 1.1 s.
    const auto archive = stage / L"retime-native.abc";
    std::string error;
    WriteArchive(archive, {{kModelChild, 100.0f}}, 3, 30.0, 1.0, true);
    Check(Install(scene.app, archive, error), "install the retimed fixture: " + error);

    ExpectTriangle(
        SampleAt(scene.Model0(), 1.0, "untimed first frame")[0], kModelChild, 100.0f,
        "the untimed cache reads frame 30 at 1 s");
    ExpectTriangle(SampleAt(scene.Model0(), 1.0 + 1.0 / 30.0 + kRetimeEpsilon,
                            "untimed second frame")[0],
                   kModelChild, 101.0f, "the untimed cache reads frame 31 next");
    Check(cache::ActiveAt(scene.Model0(), 1.0) &&
              cache::ActiveAt(scene.Model0(), 32.9 / 30.0) &&
              !cache::ActiveAt(scene.Model0(), 29.0 / 30.0) &&
              !cache::ActiveAt(scene.Model0(), 33.0 / 30.0),
          "the untimed interval is the recorded frame range");

    // A ratio the module cannot act on is refused before anything is built.
    cache::TimeScalePlanPtr plan;
    std::size_t count = 0;
    for (const double bad : {0.0, -0.0, -1.0,
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        plan.reset();
        count = 12345;
        error.clear();
        Check(!cache::PrepareTimeScale(bad, plan, count, error),
              "a ratio that is not finite and positive is refused");
        Check(!error.empty(), "the refused ratio carries a message");
        Check(plan == nullptr && count == 0,
              "the refused ratio leaves no plan behind");
    }

    // Prepare is read-only: the cache answers exactly as before, and a save of
    // the untouched cache still writes the version 1 layout byte for byte.
    error.clear();
    Check(cache::PrepareTimeScale(2.0, plan, count, error),
          "prepare a 2x retime: " + error);
    Check(plan != nullptr, "prepare hands the plan back");
    Check(count == 1, "the plan covers every cached entry");
    ExpectTriangle(SampleAt(scene.Model0(), 1.0, "sample after prepare")[0],
                   kModelChild, 100.0f, "prepare does not change the sampling");
    Check(cache::ActiveAt(scene.Model0(), 1.0) && !cache::ActiveAt(scene.Model0(), 2.0),
          "prepare does not change the interval");
    {
        extension::Document document;
        const auto pmmPath = stage / L"prepared.pmm";
        WriteProjectFile(pmmPath, document);
        Check(Save(scene.app, pmmPath, document, error),
              "save the prepared cache: " + error);
        Check(extension::Find(document, cache::kRecordKeyV2) == nullptr,
              "an untimed cache writes no version 2 record");
        const auto* record = extension::Find(document, kRecordKey);
        Check(record != nullptr, "an untimed cache still writes the version 1 record");
        const auto entries = ParseValue(record->value, "the untimed record");
        Check(entries.size() == 1, "the untimed record names the cached object");
        Check(record->value == BuildValue({{0, 0, entries[0].path}}),
              "the untimed record is byte for byte the version 1 layout");
    }

    // 2x: every baked frame sits at twice the time and the interval doubles.
    error.clear();
    Check(cache::CommitTimeScale(plan, error), "commit the 2x retime: " + error);
    Check(cache::ActiveAt(scene.Model0(), 2.0) && cache::ActiveAt(scene.Model0(), 2.19) &&
              !cache::ActiveAt(scene.Model0(), 1.99) && !cache::ActiveAt(scene.Model0(), 2.2),
          "the retimed interval is twice as long");
    ExpectTriangle(SampleAt(scene.Model0(), 2.0, "2x first frame")[0], kModelChild,
                   100.0f, "2x reads frame 30 at 2 s");
    ExpectTriangle(SampleAt(scene.Model0(), 2.0 + 2.0 / 30.0 + kRetimeEpsilon,
                            "2x second frame")[0],
                   kModelChild, 101.0f, "2x reads frame 31 two frames later");

    // A plan is consumed by a successful commit.
    error.clear();
    Check(!cache::CommitTimeScale(plan, error), "an applied plan is refused");
    Check(!error.empty(), "the refusal of an applied plan carries a message");
    Check(cache::ActiveAt(scene.Model0(), 2.0) && !cache::ActiveAt(scene.Model0(), 4.0),
          "the refused replay changes nothing");

    // A retimed cache is published as the version 2 record, and the version 1
    // record of the same project is gone.
    const auto pmmPath = stage / L"retimed.pmm";
    extension::Document document;
    extension::Set(document, "mds.unrelated", {1, 2, 3});
    WriteProjectFile(pmmPath, document);
    const auto projectBytes = ReadFileBytes(pmmPath);
    Check(Save(scene.app, pmmPath, document, error), "save the retimed cache: " + error);
    Check(ReadFileBytes(pmmPath) == projectBytes,
          "a retimed save never rewrites the project file");
    Check(extension::Find(document, kRecordKey) == nullptr,
          "a retimed save drops the version 1 record");
    Check(extension::Find(document, "mds.unrelated") != nullptr,
          "a retimed save keeps projects records it does not own");
    const auto* recordV2 = extension::Find(document, cache::kRecordKeyV2);
    Check(recordV2 != nullptr, "a retimed save writes the version 2 record");
    const auto retimed =
        ParseValue(recordV2->value, "the version 2 record", kRecordVersionV2);
    Check(retimed.size() == 1, "the version 2 record names the cached object");
    Check(retimed[0].kind == 0 && retimed[0].slot == 0,
          "the version 2 record keeps the object slot");
    Check(std::abs(retimed[0].timeScale - 2.0) < 1e-12,
          "the version 2 record carries the time scale (got " +
              std::to_string(retimed[0].timeScale) + ")");
    Check(std::filesystem::exists(stage / std::filesystem::u8path(retimed[0].path)),
          "the retimed sidecar is published next to the project");

    // The application flow: write the augmented project, read it back, load it.
    WriteProjectFile(pmmPath, document);
    extension::Document reloaded;
    std::uint64_t standardSize = 0;
    Check(ReadProjectFile(pmmPath, reloaded, standardSize),
          "the retimed project extension reads back");
    Check(standardSize == kStandardPrefixBytes,
          "the retimed extension leaves the standard PMM prefix untouched");
    Clear();
    Check(Empty(), "the cache is empty before the retimed load");
    Check(Load(scene.app, pmmPath, reloaded, error),
          "load the retimed project: " + error);
    Check(cache::ActiveAt(scene.Model0(), 2.0) && cache::ActiveAt(scene.Model0(), 2.19) &&
              !cache::ActiveAt(scene.Model0(), 1.99) && !cache::ActiveAt(scene.Model0(), 2.2),
          "the loaded project keeps the retimed interval");
    ExpectTriangle(SampleAt(scene.Model0(), 2.0 + 2.0 / 30.0 + kRetimeEpsilon,
                            "retimed load, second frame")[0],
                   kModelChild, 101.0f,
                   "the loaded project keeps reading the bake at the retimed time");

    // Every malformed or mistyped record is rejected as a whole, and the cache
    // that is already loaded stays exactly as it was.
    const auto expectRejected = [&](const std::string& key,
                                    const std::vector<std::uint8_t>& value,
                                    const std::string& why) {
        auto broken = reloaded;
        extension::Set(broken, key, value);
        std::string failure;
        Check(!Load(scene.app, pmmPath, broken, failure), why + ": rejected");
        Check(!failure.empty(), why + ": the rejection carries a message");
        Check(Has(scene.Model0()), why + ": the loaded cache survives");
        Check(cache::ActiveAt(scene.Model0(), 2.0) &&
                  !cache::ActiveAt(scene.Model0(), 1.99),
              why + ": the surviving cache keeps its time scale");
    };

    const std::string sidecar = retimed[0].path;
    {
        auto truncated = recordV2->value;
        truncated.pop_back();
        expectRejected(cache::kRecordKeyV2, truncated,
                       "a version 2 value missing its time scale");
    }
    {
        auto trailing = recordV2->value;
        trailing.push_back(0);
        expectRejected(cache::kRecordKeyV2, trailing,
                       "a version 2 value with a trailing byte");
    }
    {
        auto badCount = recordV2->value;
        badCount[8] = 2;
        expectRejected(cache::kRecordKeyV2, badCount,
                       "a version 2 count larger than the value");
    }
    expectRejected(cache::kRecordKeyV2, BuildValue({{0, 0, sidecar, 2.0}}),
                   "a version 2 key carrying the version 1 layout");
    expectRejected(cache::kRecordKeyV2,
                   BuildValue({{0, 0, sidecar, 0.0}}, kRecordVersionV2),
                   "a version 2 record with a zero time scale");
    expectRejected(cache::kRecordKeyV2,
                   BuildValue({{0, 0, sidecar, -2.0}}, kRecordVersionV2),
                   "a version 2 record with a negative time scale");
    expectRejected(
        cache::kRecordKeyV2,
        BuildValue({{0, 0, sidecar, std::numeric_limits<double>::quiet_NaN()}},
                   kRecordVersionV2),
        "a version 2 record with a NaN time scale");
    expectRejected(
        cache::kRecordKeyV2,
        BuildValue({{0, 0, sidecar, std::numeric_limits<double>::infinity()}},
                   kRecordVersionV2),
        "a version 2 record with an infinite time scale");
    {
        auto broken = reloaded;
        extension::Erase(broken, cache::kRecordKeyV2);
        extension::Set(broken, kRecordKey,
                       BuildValue({{0, 0, sidecar, 2.0}}, kRecordVersionV2));
        std::string failure;
        Check(!Load(scene.app, pmmPath, broken, failure),
              "a version 1 key carrying the version 2 layout is rejected");
        Check(!failure.empty(), "the rejection carries a message");
        Check(Has(scene.Model0()), "the loaded cache survives the mistyped key");
    }

    // When both known records are present the retimed one wins.
    {
        auto both = reloaded;
        extension::Set(both, kRecordKey, BuildValue({{0, 0, sidecar}}));
        Clear();
        Check(Load(scene.app, pmmPath, both, error),
              "load with both known records present: " + error);
        Check(Has(scene.Model0()) && cache::ActiveAt(scene.Model0(), 2.0) &&
                  !cache::ActiveAt(scene.Model0(), 1.99),
              "the version 2 record wins over the version 1 record");
    }
    // A malformed version 2 record is not silently replaced by a valid one.
    {
        auto mixed = reloaded;
        extension::Set(mixed, kRecordKey, BuildValue({{0, 0, sidecar}}));
        extension::Set(mixed, cache::kRecordKeyV2, BuildValue({{0, 0, sidecar, 2.0}}));
        Clear();
        std::string failure;
        Check(!Load(scene.app, pmmPath, mixed, failure),
              "a malformed version 2 record is not replaced by the version 1 record");
        Check(Empty(), "the invalid retimed project installs nothing");
    }

    // Undo the factor: 0.5x brings the interval and the sampling back to exactly
    // the untimed values.
    Clear();
    Check(Load(scene.app, pmmPath, reloaded, error),
          "reload the retimed project: " + error);
    cache::TimeScalePlanPtr undo;
    std::size_t undoCount = 0;
    error.clear();
    Check(cache::PrepareTimeScale(0.5, undo, undoCount, error),
          "prepare the half-speed undo: " + error);
    Check(undoCount == 1, "the undo plan covers the entry");
    Check(cache::CommitTimeScale(undo, error), "commit the half-speed undo: " + error);
    ExpectTriangle(SampleAt(scene.Model0(), 1.0, "first frame after the undo")[0],
                   kModelChild, 100.0f, "the undo restores the untimed sampling");
    Check(cache::ActiveAt(scene.Model0(), 1.0) &&
              cache::ActiveAt(scene.Model0(), 32.9 / 30.0) &&
              !cache::ActiveAt(scene.Model0(), 29.0 / 30.0) &&
              !cache::ActiveAt(scene.Model0(), 33.0 / 30.0),
          "the undo restores the untimed interval");

    // An untimed cache writes the version 1 record again and drops the stale
    // version 2 record, so a project never keeps both.
    extension::Document afterUndo = reloaded;
    Check(extension::Find(afterUndo, cache::kRecordKeyV2) != nullptr,
          "the project still carries the version 2 record before the save");
    Check(Save(scene.app, pmmPath, afterUndo, error), "save after the undo: " + error);
    Check(extension::Find(afterUndo, cache::kRecordKeyV2) == nullptr,
          "an untimed save drops the stale version 2 record");
    Check(extension::Find(afterUndo, kRecordKey) != nullptr,
          "an untimed save writes the version 1 record again");

    // Repeated factors multiply: 2x twice is 4x, and 0.25x cancels it exactly.
    const auto retime = [&](double ratio) {
        cache::TimeScalePlanPtr step;
        std::size_t stepCount = 0;
        std::string failure;
        Check(cache::PrepareTimeScale(ratio, step, stepCount, failure),
              "prepare a retime step: " + failure);
        Check(stepCount == 1, "a retime step covers the entry");
        Check(cache::CommitTimeScale(step, failure),
              "commit a retime step: " + failure);
    };
    retime(2.0);
    retime(2.0);
    ExpectTriangle(SampleAt(scene.Model0(), 4.0 + 4.0 / 30.0 + kRetimeEpsilon,
                            "4x second frame")[0],
                   kModelChild, 101.0f, "the two factors multiplied to 4x");
    Check(cache::ActiveAt(scene.Model0(), 4.0) && !cache::ActiveAt(scene.Model0(), 3.99) &&
              cache::ActiveAt(scene.Model0(), 4.39) && !cache::ActiveAt(scene.Model0(), 4.4),
          "the 4x interval is four times as long");
    retime(0.25);
    ExpectTriangle(SampleAt(scene.Model0(), 1.0, "first frame after the 4x undo")[0],
                   kModelChild, 100.0f,
                   "the quarter-speed factor cancels the two doublings");
    Check(cache::ActiveAt(scene.Model0(), 1.0) && !cache::ActiveAt(scene.Model0(), 2.0),
          "the interval is back to the untimed range");

    // The commit callback is the caller's last chance to veto, and a veto leaves
    // both the cache and the plan as they were.
    cache::TimeScalePlanPtr vetoed;
    std::size_t vetoedCount = 0;
    error.clear();
    Check(cache::PrepareTimeScale(2.0, vetoed, vetoedCount, error),
          "prepare the vetoed retime: " + error);
    int calls = 0;
    error.clear();
    const bool refused = cache::CommitTimeScale(vetoed, error, [&](std::string& reason) {
        ++calls;
        reason = "the caller vetoed this retime";
        return false;
    });
    Check(!refused, "a vetoing callback refuses the retime");
    Check(calls == 1, "the vetoing callback is called exactly once");
    Check(error == "the caller vetoed this retime",
          "the callback reason reaches the caller");
    Check(cache::ActiveAt(scene.Model0(), 1.0) && !cache::ActiveAt(scene.Model0(), 2.0),
          "a veto leaves the interval as it was");
    ExpectTriangle(SampleAt(scene.Model0(), 1.0, "sample after the veto")[0],
                   kModelChild, 100.0f, "a veto leaves the sampling as it was");

    // A callback that throws is a veto too, and the plan is still not consumed.
    calls = 0;
    error.clear();
    Check(!cache::CommitTimeScale(vetoed, error, [&](std::string&) -> bool {
              ++calls;
              throw std::runtime_error("effect teardown failed");
          }),
          "a throwing callback refuses the retime");
    Check(calls == 1, "the throwing callback is called exactly once");
    Check(error.find("effect teardown failed") != std::string::npos,
          "the exception text reaches the caller: " + error);
    Check(cache::ActiveAt(scene.Model0(), 1.0),
          "a throwing callback leaves the interval as it was");

    // The kept plan still applies, and this time the cache changes.
    error.clear();
    Check(cache::CommitTimeScale(vetoed, error),
          "the kept plan commits after the vetoes: " + error);
    Check(cache::ActiveAt(scene.Model0(), 2.0) && !cache::ActiveAt(scene.Model0(), 1.99),
          "the kept plan finally retimed the cache");

    // A plan built before the cache changed is refused, and refusing it does not
    // bring back the entry that is gone.
    cache::TimeScalePlanPtr stale;
    std::size_t staleCount = 0;
    error.clear();
    Check(cache::PrepareTimeScale(2.0, stale, staleCount, error),
          "prepare the stale plan: " + error);
    Forget(scene.Model0());
    Check(!Has(scene.Model0()), "the entry is gone before the stale commit");
    error.clear();
    Check(!cache::CommitTimeScale(stale, error),
          "a plan built before the cache changed is refused");
    Check(!error.empty(), "the stale refusal carries a message");
    Check(!Has(scene.Model0()), "the refused stale plan does not bring the entry back");

    // An empty cache still runs the callback exactly once.  The caller uses this
    // commit as the transaction that wraps its own work, so a project that has
    // no baked geometry at all must still be able to run - and to refuse - that
    // transaction.
    Clear();
    Check(Empty(), "the cache is empty before the no-cache retime");
    cache::TimeScalePlanPtr none;
    std::size_t noneCount = 12345;
    error.clear();
    Check(cache::PrepareTimeScale(2.0, none, noneCount, error),
          "prepare a retime of an empty cache: " + error);
    Check(none != nullptr && noneCount == 0,
          "an empty cache produces a plan of no entries");
    calls = 0;
    error.clear();
    Check(cache::CommitTimeScale(none, error,
                                 [&calls](std::string&) {
                                     ++calls;
                                     return true;
                                 }),
          "the empty plan applies as a no-op: " + error);
    Check(calls == 1, "the empty plan runs its callback exactly once");
    Check(Empty(), "the empty plan leaves the cache empty");
    error.clear();
    Check(!cache::CommitTimeScale(none, error), "the consumed empty plan is refused too");
    Check(!error.empty(), "the refusal of the consumed empty plan carries a message");

    // A callback that refuses an empty plan fails the commit and leaves the plan
    // unconsumed, so the very same plan can still be committed afterwards.
    cache::TimeScalePlanPtr emptyVetoed;
    std::size_t emptyVetoedCount = 12345;
    error.clear();
    Check(cache::PrepareTimeScale(2.0, emptyVetoed, emptyVetoedCount, error),
          "prepare the empty plan that will be vetoed: " + error);
    Check(emptyVetoed != nullptr && emptyVetoedCount == 0,
          "the vetoed empty plan covers no entry");
    calls = 0;
    error.clear();
    Check(!cache::CommitTimeScale(emptyVetoed, error, [&](std::string& reason) {
              ++calls;
              reason = "the caller vetoed the empty retime";
              return false;
          }),
          "a vetoing callback refuses the empty plan");
    Check(calls == 1, "the vetoing callback is called exactly once for an empty cache");
    Check(error == "the caller vetoed the empty retime",
          "the empty veto reason reaches the caller");
    Check(Empty(), "the refused empty plan leaves the cache empty");
    calls = 0;
    error.clear();
    Check(cache::CommitTimeScale(emptyVetoed, error,
                                 [&calls](std::string&) {
                                     ++calls;
                                     return true;
                                 }),
          "the empty plan is not consumed by the refusal: " + error);
    Check(calls == 1, "the kept empty plan runs its callback once");
    Check(Empty(), "the kept empty plan still changes nothing");

    // A retime that would push a native range past the last frame of the
    // timeline is refused as a whole, while one that stays inside it is accepted.
    scene.BindModel0();
    const auto wide = stage / L"retime-wide.abc";
    WriteArchive(wide, {{kModelChild, 100.0f}}, 1, 30.0, 1.0, true, 100, 100000);
    Check(Install(scene.app, wide, error), "install the long-range fixture: " + error);
    Check(cache::ActiveAt(scene.Model0(), 3.4) && !cache::ActiveAt(scene.Model0(), 3.3),
          "the long-range fixture starts at its own frame");
    cache::TimeScalePlanPtr huge;
    std::size_t hugeCount = 12345;
    error.clear();
    const double outsideTimeline = double(mikumikustudio::frame_limits::maximum) / 99999.0 + 1.0;
    Check(!cache::PrepareTimeScale(outsideTimeline, huge, hugeCount, error),
          "a retime past the last timeline frame is refused");
    Check(!error.empty() && huge == nullptr && hugeCount == 0,
          "the refused retime leaves no plan behind");
    Check(cache::ActiveAt(scene.Model0(), 3.4) && !cache::ActiveAt(scene.Model0(), 3.3) &&
              cache::ActiveAt(scene.Model0(), 3333.3) &&
              !cache::ActiveAt(scene.Model0(), 3333.4),
          "the refused retime leaves the interval as it was");
    cache::TimeScalePlanPtr inside;
    std::size_t insideCount = 0;
    error.clear();
    Check(cache::PrepareTimeScale(2.0, inside, insideCount, error),
          "prepare a retime inside the frame limit: " + error);
    Check(cache::CommitTimeScale(inside, error),
          "commit the inside-limit retime: " + error);
    Check(cache::ActiveAt(scene.Model0(), 6.7) && !cache::ActiveAt(scene.Model0(), 6.6) &&
              !cache::ActiveAt(scene.Model0(), 3.4) &&
              cache::ActiveAt(scene.Model0(), 6666.7) &&
              !cache::ActiveAt(scene.Model0(), 6666.8),
          "the inside-limit retime doubled the long-range interval");

    Clear();
    Trace("retime plan, version 2 record, stale plan and veto");
}

// Per-object cutoff layers and per-object generation bindings: one bake can
// cover several objects whose generations differ, each entry binds its own
// generation exactly once, and a stale cache is hard-rejected instead of being
// silently dropped or rebound to another object every frame.
void CutoffGenerations(const std::filesystem::path& directory, Scene& scene) {
    Clear();
    scene.BindModel1();
    const auto archive = directory / L"cutoff-two.abc";
    std::string error;
    WriteArchive(archive, {{kModelChild, 40.0f}, {kSecondModelChild, 50.0f}}, 1, 30.0, 0.0);
    Check(Install(scene.app, archive, error, {}, 2),
          "install the two-object cutoff archive: " + error);
    EntryInfo first, second;
    Check(Info(scene.Model0(), first) && Info(scene.Model1(), second),
          "both objects report cache info");
    Check(first.cutoffLayer == 2 && second.cutoffLayer == 2,
          "both entries carry the cutoff layer of their bake");
    Check(first.sourceGeneration == 0 && second.sourceGeneration == 0,
          "a fresh entry starts unbound");
    Check(BindGeneration(scene.Model0(), 111), "the first object binds its own generation");
    Check(BindGeneration(scene.Model1(), 222), "the second object binds its own generation");
    Check(Info(scene.Model0(), first) && first.sourceGeneration == 111,
          "the first entry keeps its own generation");
    Check(Info(scene.Model1(), second) && second.sourceGeneration == 222,
          "the second entry keeps its own generation");
    Check(BindGeneration(scene.Model0(), 111), "rebinding the same generation succeeds");
    Check(!BindGeneration(scene.Model0(), 999),
          "another generation is refused: this session's cache is stale");
    Check(Info(scene.Model0(), first) && first.sourceGeneration == 111,
          "a refused rebind never overwrites the binding");
    Check(!BindGeneration(scene.Model1(), 111),
          "one object's generation never binds another object");

    // Save and Load keep the cutoff layers, but the generation is a session
    // value: a reloaded entry starts unbound, binds its new object exactly
    // once, and is hard-refused from then on when that object changes.
    const auto project = directory / L"cutoff-project" / L"scene.pmm";
    std::filesystem::create_directories(project.parent_path());
    extension::Document document;
    WriteProjectFile(project, document);
    Check(Save(scene.app, project, document, error), "save the cutoff cache: " + error);
    Check(extension::Find(document, kRecordKeyV3) != nullptr,
          "a layered cache saves under the version 3 record");
    Clear();
    Check(Load(scene.app, project, document, error), "load the cutoff cache: " + error);
    Check(Info(scene.Model0(), first) && first.cutoffLayer == 2 && first.sourceGeneration == 0,
          "a loaded entry keeps its cutoff layer and starts unbound");
    Check(Info(scene.Model1(), second) && second.cutoffLayer == 2,
          "the second loaded entry keeps its own cutoff layer");
    Check(BindGeneration(scene.Model0(), 111), "the loaded entry binds its object exactly once");
    Check(!BindGeneration(scene.Model0(), 999),
          "the loaded entry is hard-refused once its object changed");

    // The loaded project document still contains v3. Clearing the live cache
    // must remove that old record at Save, otherwise Load resurrects it.
    const auto originalBytes=ReadFileBytes(archive);
    std::vector<std::pair<std::filesystem::path,std::string>> sidecars;
    for(const auto& file:std::filesystem::directory_iterator(project.parent_path())){
        if(file.is_regular_file()&&file.path().extension()==".abc")
            sidecars.emplace_back(file.path(),ReadFileBytes(file.path()));
    }
    Check(!sidecars.empty(),"v3 project published an archive to preserve");
    extension::Set(document,"test.keep-after-cache-clear",{1,2,3});
    Clear();
    Check(Save(scene.app,project,document,error),"save a cleared v3 document: "+error);
    WriteProjectFile(project,document);
    extension::Document reloaded;std::uint64_t standardSize=0;
    Check(ReadProjectFile(project,reloaded,standardSize),"read cleared v3 project from disk");
    Check(standardSize==kStandardPrefixBytes,"clearing preserves the standard PMM prefix");
    Check(extension::Find(reloaded,"test.keep-after-cache-clear")!=nullptr,
          "clearing preserves unrelated extension records");
    Check(ReadFileBytes(archive)==originalBytes,"clear-save preserves the user's original ABC bytes");
    for(const auto& sidecar:sidecars)
        Check(ReadFileBytes(sidecar.first)==sidecar.second,"clear-save preserves published sidecar bytes");
    Check(Load(scene.app,project,reloaded,error),"load the cleared v3 project: "+error);
    Check(Empty()&&!Has(scene.Model0())&&!Has(scene.Model1()),"cleared v3 caches do not resurrect after save and reload");
    Check(extension::Find(reloaded,kRecordKey)==nullptr&&
          extension::Find(reloaded,mikumikustudio::mesh_bake_cache::kRecordKeyV2)==nullptr&&
          extension::Find(reloaded,kRecordKeyV3)==nullptr,"clear removes all known cache record versions");
    Clear();
    Trace("per-object cutoff layers and generation bindings");
}

void SnapshotLifetime(const std::filesystem::path& directory, Scene& scene) {
    Clear(); scene.BindModel0();
    const auto archive = directory / L"snapshot-source.abc";
    WriteArchive(archive, {{kModelChild, 42.0f}}, 4, 30.0, 0.0);
    std::string error;
    Check(Install(scene.app, archive, error), "install snapshot source: " + error);
    auto snapshot = mikumikustudio::mesh_bake_cache::CaptureSave(scene.app);
    Clear();
    Check(Empty(), "live cache can be cleared while snapshot owns its files");
    extension::Document document;
    const auto path = directory / L"snapshot-project.pmm";
    Check(snapshot(path, document, error), "snapshot publishes after live cache was cleared: " + error);
    Check(extension::Find(document, kRecordKey) != nullptr, "snapshot retains the original cache mapping");
    Check(Load(scene.app, path, document, error), "reload snapshot cache: " + error);
    Check(Has(scene.Model0()), "saved snapshot restores its original model cache");
    snapshot = {}; Clear();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto out = std::filesystem::absolute(
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path(
                       L".local/artifacts/alembic-export/mesh-cache/fixtures"));
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
    Check(!ec, "clear the previous artifact directory " + out.u8string());
    std::filesystem::create_directories(out, ec);
    Check(!ec, "create the artifact directory " + out.u8string());
    // A non-ASCII directory and file name exercises the wide-path handling.
    const auto unicode = out / L"\u4e2d\u6587\u76ee\u5f55";
    const auto project = unicode / L"project";
    std::filesystem::create_directories(project, ec);
    Check(!ec, "create the fixture directories");

    const auto tempBaseline = CountTempCopies();
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    ResetSlots(*app);
    Scene scene(*app);

    std::printf("mesh_bake_cache regression, artifacts in %s\n",
                out.u8string().c_str());
    CacheBasics(unicode, scene, tempBaseline);
    CachePersistence(project, scene);
    ClearOwnership(unicode, scene);
    InstallCallback(unicode, scene);
    NativeRange(project, scene);
    TimeScale(project, scene);
    CutoffGenerations(unicode, scene);
    SnapshotLifetime(unicode, scene);

    Clear();
    Check(Empty(), "the cache is empty at the end");
    Check(CountTempCopies() <= tempBaseline,
          "the module leaves no private copy behind");
    std::puts("PASS: mesh_bake_cache sample, replace, forget, save, load, clear, retime");
    return 0;
}
