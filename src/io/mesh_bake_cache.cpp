#include "mikumikustudio/frame_limits.hpp"
// Disk-backed cache of baked deformed meshes.  See the header for the archive
// naming contract, the PMM record layout and the file-lifetime rules.
#define NOMINMAX

#include "mikumikustudio/mesh_bake_cache.hpp"

#include "mikumikustudio/mmd_app.hpp"

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <system_error>
#include <utility>

namespace mikumikustudio {
namespace mesh_bake_cache {
namespace {

namespace Abc = Alembic::Abc;
namespace Geom = Alembic::AbcGeom;

using Mesh = deformed_mesh_capture::Mesh;

constexpr std::uint8_t kKindModel = 0;
constexpr std::uint8_t kKindAccessory = 1;
constexpr int kAccessorySlotCount = 255;

// 'M','M','S','C' as a little-endian 32-bit word.  The legacy 'M','D','S','C'
// form is still accepted when reading an old cache.
constexpr std::uint32_t kRecordMagic = 0x43534D4Du;
constexpr std::uint32_t kLegacyRecordMagic = 0x4353444Du;
constexpr std::uint32_t kRecordVersion = 1;
// The version 2 layout repeats version 1 and appends the time scale of every
// entry.  It is only written once an entry carries a scale other than 1.
constexpr std::uint32_t kRecordVersionV2 = 2;
// Version 3 = version 2 plus the cutoff layer of every entry (uint32).
constexpr std::uint32_t kRecordVersionV3 = 3;
constexpr std::size_t kMaxRecordEntries = 512;
constexpr std::size_t kMaxRelativePathBytes = 512;
constexpr std::size_t kMaxSidecarStemBytes = 64;
constexpr std::size_t kHashChunkBytes = 64 * 1024;
// The timeline works in whole frames at a fixed 30 fps rate, and MMD accepts no
// frame beyond mikumikustudio::frame_limits::maximum.  A retime may not push a native range past it.
constexpr double kBakeFrameRate = 30.0;
constexpr double kMaxTimelineFrames = double(mikumikustudio::frame_limits::maximum);

std::string Utf8(const std::filesystem::path& path) { return path.u8string(); }

std::string Quoted(const std::filesystem::path& path) {
    return "'" + Utf8(path) + "'";
}

// ---------------------------------------------------------------------------
// Archive child naming
// ---------------------------------------------------------------------------

// One object of the scene.  The slot alone identifies the object; the kind
// separates the model and accessory slot arrays.
struct ObjectKey {
    unsigned kind = 0;
    int slot = 0;
    bool operator<(const ObjectKey& other) const {
        return kind != other.kind ? kind < other.kind : slot < other.slot;
    }
    bool operator==(const ObjectKey& other) const {
        return kind == other.kind && slot == other.slot;
    }
};

bool ParseDigits(const std::string& text, std::size_t begin, std::size_t end,
                 std::uint64_t& value) {
    if (begin >= end || end > text.size()) return false;
    std::uint64_t result = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = std::uint64_t(c - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

// Accepts exactly "model_<slot>_material_<m>_shader_<h>_pass_<p>" or the
// accessory variant.  A name from a foreign archive is rejected rather than
// guessed at, so nothing is bound to the wrong object.
bool ParseMeshName(const std::string& name, ObjectKey& key) {
    static constexpr char kModelPrefix[] = "model_";
    static constexpr char kAccessoryPrefix[] = "accessory_";
    static constexpr char kMaterial[] = "_material_";
    static constexpr char kShader[] = "_shader_";
    static constexpr char kPass[] = "_pass_";

    std::size_t cursor = 0;
    if (name.compare(0, sizeof kModelPrefix - 1, kModelPrefix) == 0) {
        key.kind = kKindModel;
        cursor = sizeof kModelPrefix - 1;
    } else if (name.compare(0, sizeof kAccessoryPrefix - 1, kAccessoryPrefix) == 0) {
        key.kind = kKindAccessory;
        cursor = sizeof kAccessoryPrefix - 1;
    } else {
        return false;
    }

    const auto materialAt = name.find(kMaterial, cursor);
    if (materialAt == std::string::npos) return false;
    const auto shaderAt = name.find(kShader, materialAt + (sizeof kMaterial - 1));
    if (shaderAt == std::string::npos) return false;
    const auto passAt = name.find(kPass, shaderAt + (sizeof kShader - 1));
    if (passAt == std::string::npos) return false;

    std::uint64_t slot = 0, material = 0, shader = 0, pass = 0;
    if (!ParseDigits(name, cursor, materialAt, slot)) return false;
    if (!ParseDigits(name, materialAt + (sizeof kMaterial - 1), shaderAt, material))
        return false;
    if (!ParseDigits(name, shaderAt + (sizeof kShader - 1), passAt, shader))
        return false;
    if (!ParseDigits(name, passAt + (sizeof kPass - 1), name.size(), pass))
        return false;
    if (slot > std::uint64_t(std::numeric_limits<int>::max())) return false;
    key.slot = int(slot);
    return true;
}

const void* LiveObject(MMDApp& app, unsigned kind, int slot) {
    if (slot < 0) return nullptr;
    if (kind == kKindModel) {
        if (slot >= kModelSlotCount) return nullptr;
        return app.ModelSlot(slot);
    }
    if (kind == kKindAccessory) {
        if (slot >= kAccessorySlotCount) return nullptr;
        return app.AccessorySlot(slot);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Open archives
// ---------------------------------------------------------------------------

// One open Alembic archive plus the stream that keeps it readable.  `stream` is
// declared before `archive` so the archive is destroyed first, and the stream is
// closed only afterwards.
struct Archive {
    std::filesystem::path file;
    // Directory this process created for its own copies.  A copy is only ever
    // removed when it sits in exactly that directory.
    std::filesystem::path ownerDirectory;
    bool temporary = false;
    bool rangeLimited = false;
    double firstSeconds = 0, endSeconds = 0;
    std::shared_ptr<std::ifstream> stream;
    std::shared_ptr<Abc::IArchive> archive;

    ~Archive() {
        // Release the reader before the stream goes away, then drop this one
        // private copy.  A persistent sidecar is never removed here, and the
        // removal is never recursive.
        archive.reset();
        stream.reset();
        if (!temporary || file.empty()) return;
        if (ownerDirectory.empty() || file.parent_path() != ownerDirectory) return;
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
};

struct Child {
    std::size_t index = 0;  // index of the top-level archive child
    std::string name;
    std::vector<double> times;
};

struct ObjectEntry {
    ObjectKey key;
    std::shared_ptr<Archive> source;
    std::vector<Child> children;
    // Sampling reads the archive at seconds / timeScale, so this is the ratio
    // between the timeline the caller samples and the baked frames.  A retime
    // replaces the entry with a copy that carries the multiplied scale; the
    // archive, and therefore the geometry in it, is shared and never rewritten.
    double timeScale = 1;
    // The MMV layer whose output these meshes are (kNoCutoffLayer for a legacy
    // full-chain bake) and the host object generation they were sampled from.
    // The generation is a session value: a cache loaded from a project carries
    // 0 until the host binds it to the object it plays back on.
    std::uint32_t cutoffLayer = kNoCutoffLayer;
    std::uint64_t sourceGeneration = 0;
};

// Entries are held behind shared handles on purpose: building the next state of
// the cache then only copies map nodes and handles, never the frame-time tables
// or the open archives, and the commit can be a plain pointer swap.
using EntryHandle = std::shared_ptr<const ObjectEntry>;
using StoreMap = std::map<const void*, EntryHandle>;

struct Store {
    std::mutex mutex;
    StoreMap objects;
    // Bumped by every change of `objects`, so a retime plan can tell whether the
    // cache it was built from is still the live one.
    unsigned long long revision = 0;
    // Private directory of this process, created on first use with a random
    // name so that two running instances never share one.  Empty until then.
    std::filesystem::path sessionDirectory;
    unsigned long long tempCounter = 0;

    ~Store() {
        // Release the copies, then drop the directory only if it is empty.  The
        // shared root above it is never touched.
        objects.clear();
        if (sessionDirectory.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(sessionDirectory, ignored);
    }
};

Store& GetStore() {
    static Store store;
    return store;
}

bool OpenArchive(const std::filesystem::path& file, bool temporary,
                 const std::filesystem::path& ownerDirectory,
                 std::shared_ptr<Archive>& out, std::string& error) {
    auto archive = std::make_shared<Archive>();
    archive->file = file;
    archive->ownerDirectory = ownerDirectory;
    archive->temporary = temporary;
    archive->stream = std::make_shared<std::ifstream>(file, std::ios::binary);
    if (!archive->stream->is_open()) {
        error = "Cannot open the mesh cache archive " + Quoted(file);
        return false;
    }
    try {
        std::vector<std::istream*> streams{archive->stream.get()};
        archive->archive = std::make_shared<Abc::IArchive>(
            Alembic::AbcCoreOgawa::ReadArchive(streams), "mesh_bake_cache");
        if (!archive->archive->valid()) {
            error = "The mesh cache file is not a readable Alembic archive: " +
                    Quoted(file);
            return false;
        }
    } catch (const std::exception& e) {
        error = "Cannot read " + Quoted(file) + " as an Alembic archive: " + e.what();
        return false;
    }
    try {
        const auto properties=archive->archive->getTop().getProperties();
        if(properties.getPropertyHeader("mdsBakeFirstFrame")||properties.getPropertyHeader("mdsBakeLastFrame")){
            const auto first=Abc::IInt32Property(properties,"mdsBakeFirstFrame").getValue();
            const auto last=Abc::IInt32Property(properties,"mdsBakeLastFrame").getValue();
            if(first<0||last<first||last>frame_limits::maximum)throw std::runtime_error("Invalid native bake frame range");
            archive->rangeLimited=true;archive->firstSeconds=first/30.;archive->endSeconds=(last+1)/30.;
        }
    }catch(const std::exception& e){error=e.what();return false;}
    out = std::move(archive);
    return true;
}

// Collects every mesh of the archive that follows the naming contract, grouped
// by the object it belongs to.  Children that do not match the contract are
// reported through `ignored` and skipped.
bool EnumerateChildren(const Archive& archive,
                       std::map<ObjectKey, std::vector<Child>>& groups,
                       std::size_t& ignored, std::string& error) {
    try {
        const auto root = archive.archive->getTop();
        for (std::size_t index = 0; index < root.getNumChildren(); ++index) {
            const std::string name = root.getChildHeader(index).getName();
            ObjectKey key;
            if (!ParseMeshName(name, key)) {
                ++ignored;
                continue;
            }
            Geom::IPolyMesh mesh(root, name);
            auto& schema = mesh.getSchema();
            const auto sampleCount = schema.getNumSamples();
            if (sampleCount == 0) {
                error = "The mesh cache archive child '" + name + "' has no samples";
                return false;
            }
            Child child;
            child.index = index;
            child.name = name;
            child.times.reserve(std::size_t(sampleCount));
            const auto sampling = schema.getTimeSampling();
            for (std::size_t sample = 0; sample < sampleCount; ++sample)
                child.times.push_back(sampling->getSampleTime(sample));
            groups[key].push_back(std::move(child));
        }
    } catch (const std::exception& e) {
        error = std::string("Cannot enumerate the mesh cache archive: ") + e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

// A native range boundary on the retimed timeline.  The timeline holds whole
// 30 fps frames, so the boundary is snapped to one, half up, instead of keeping
// a fraction of a frame.  At a scale of 1 a boundary is already a whole frame and
// the half-up rounding absorbs the tiny error a division and multiplication
// round trip can leave, so the untimed behaviour is exactly the old one.
double ScaledBoundarySeconds(double seconds, double timeScale) {
    return std::floor(seconds * kBakeFrameRate * timeScale + 0.5) / kBakeFrameRate;
}

std::size_t SampleIndex(const std::vector<double>& times, double seconds) {
    if (times.empty()) return 0;
    if (!(seconds > times.front())) return 0;  // also clamps NaN
    if (seconds >= times.back()) return times.size() - 1;
    const auto above = std::upper_bound(times.begin(), times.end(), seconds);
    return std::size_t(above - times.begin()) - 1;
}

bool ReadMesh(const Archive& archive, const Child& child, std::size_t index,
              Mesh& out, std::string& error) {
    try {
        const auto root = archive.archive->getTop();
        Geom::IPolyMesh mesh(root, child.name);
        auto& schema = mesh.getSchema();
        const auto selector = Abc::ISampleSelector(Abc::index_t(index));
        const auto sample = schema.getValue(selector);

        std::size_t vertexCount = 0;
        const auto positions = sample.getPositions();
        if (positions) vertexCount = positions->size();

        out = Mesh{};
        out.name = child.name;
        const auto properties=schema.getUserProperties();
        if(properties.valid()&&properties.getPropertyHeader("mdsVertexPatch"))
            out.vertexPatch=Abc::IBoolProperty(properties,"mdsVertexPatch").getValue();
        // The source-vertex map is read whenever the archive carries it: a
        // layer bake stores it for the complete object too, because the replay
        // maps every unrecordable field through it.  The map is taken verbatim
        // - UINT32_MAX entries mean "no source row" and cover both generated
        // vertices and the unselected rows of a partial patch. A legacy
        // complete archive without the map has unknown source rows and leaves
        // the vector empty; a partial patch without the map is refused.
        if(vertexCount&&properties.valid()&&properties.getPropertyHeader("mdsSourceVertices")){
            const auto sourceIds=Abc::IUInt32ArrayProperty(properties,"mdsSourceVertices").getValue(selector);
            if(!sourceIds||sourceIds->size()!=vertexCount){error="Invalid source-vertex map in baked mesh";return false;}
            out.sourceVertices.assign(sourceIds->get(),sourceIds->get()+sourceIds->size());
        }else if(out.vertexPatch&&vertexCount){error="Invalid source-vertex map in baked mesh";return false;}
        // A frame the exporter back-filled with an empty sample carries no
        // positions; its index stream is left over from a later frame and is
        // meaningless here, so the mesh is simply empty.
        if (vertexCount == 0) return true;

        out.positions.reserve(vertexCount);
        for (std::size_t i = 0; positions && i < vertexCount; ++i) {
            const auto& position = (*positions)[i];
            out.positions.push_back({position.x, position.y, position.z});
        }

        const auto uvs =
            schema.getUVsParam().getExpandedValue(selector).getVals();
        const std::size_t uvCount = uvs ? uvs->size() : 0;
        if (uvCount != 0 && uvCount != vertexCount) {
            error = "The mesh cache archive child '" + child.name +
                    "' has a UV count that does not match its vertex count";
            return false;
        }
        out.uvs.reserve(uvCount);
        for (std::size_t i = 0; uvs && i < uvCount; ++i) {
            const auto& uv = (*uvs)[i];
            out.uvs.push_back({uv[0], uv[1]});
        }

        if(schema.getNormalsParam().valid()){
            const auto normals=schema.getNormalsParam().getExpandedValue(selector).getVals();
            if(normals&&normals->size()==vertexCount)for(size_t i=0;i<vertexCount;++i){const auto& n=(*normals)[i];out.normals.push_back({n.x,n.y,n.z});}
        }
        const auto indices = sample.getFaceIndices();
        const auto counts = sample.getFaceCounts();
        if (counts && indices) {
            std::size_t cursor = 0;
            for (std::size_t face = 0; face < counts->size(); ++face) {
                const std::int32_t corners = (*counts)[face];
                if (corners < 3 || cursor + std::size_t(corners) > indices->size()) {
                    error = "The mesh cache archive child '" + child.name +
                            "' has a malformed face list";
                    return false;
                }
                for (std::int32_t corner = 1; corner + 1 < corners; ++corner) {
                    const std::int32_t triangle[3] = {(*indices)[cursor],
                                                      (*indices)[cursor + std::size_t(corner)],
                                                      (*indices)[cursor + std::size_t(corner) + 1]};
                    for (const std::int32_t vertex : triangle) {
                        if (vertex < 0 || std::size_t(vertex) >= vertexCount) {
                            error = "The mesh cache archive child '" + child.name +
                                    "' has an out-of-range face index";
                            return false;
                        }
                        out.indices.push_back(vertex);
                    }
                }
                cursor += std::size_t(corners);
            }
            if (cursor != indices->size()) {
                error = "The mesh cache archive child '" + child.name +
                        "' has trailing face indices";
                return false;
            }
        }
    } catch (const std::exception& e) {
        error = "Cannot read '" + child.name + "' from the mesh cache: " + e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Private temporary copies
// ---------------------------------------------------------------------------

// Shared root that every instance of the application uses.  It is created when
// needed and is never removed by this module, not even recursively.
std::filesystem::path SharedTempRoot() {
    std::error_code ignored;
    auto base = std::filesystem::temp_directory_path(ignored);
    if (ignored || base.empty()) base = std::filesystem::current_path(ignored);
    return base / "MikuMikuStudio" / "mesh_bake_cache";
}

// Creates (once) the directory this process owns for its own copies: an
// unguessable name under the shared root, and the directory is only accepted
// when this call created it, so a second running instance can never end up
// sharing or deleting another instance's directory.
bool EnsureSessionDirectory(std::filesystem::path& directory, std::string& error) {
    auto& store = GetStore();
    if (!store.sessionDirectory.empty()) {
        std::error_code existsError;
        if (std::filesystem::is_directory(store.sessionDirectory, existsError)) {
            directory = store.sessionDirectory;
            return true;
        }
        store.sessionDirectory.clear();
    }

    const auto root = SharedTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        error = "Cannot create the mesh cache root directory " + Quoted(root) + ": " +
                ec.message();
        return false;
    }

    std::random_device device;
    std::mt19937_64 generator(device() ^
                              static_cast<std::uint64_t>(
                                  std::chrono::steady_clock::now()
                                      .time_since_epoch()
                                      .count()));
    const std::string prefix =
        "mesh-cache-" + std::to_string(GetCurrentProcessId()) + "-";
    for (int attempt = 0; attempt < 32; ++attempt) {
        char suffix[33]{};
        std::snprintf(suffix, sizeof suffix, "%016llx%016llx",
                      static_cast<unsigned long long>(generator()),
                      static_cast<unsigned long long>(generator()));
        const auto candidate = root / (prefix + suffix);
        ec.clear();
        // create_directory fails when the name is already taken, which is the
        // ownership test: only a directory this call created is ours.
        if (std::filesystem::create_directory(candidate, ec)) {
            store.sessionDirectory = candidate;
            directory = candidate;
            return true;
        }
        if (ec) {
            error = "Cannot create the private mesh cache directory " +
                    Quoted(candidate) + ": " + ec.message();
            return false;
        }
    }
    error = "Cannot find a free private mesh cache directory under " + Quoted(root);
    return false;
}

bool CopyToPrivateFile(const std::filesystem::path& abc,
                       std::filesystem::path& copy,
                       std::filesystem::path& ownerDirectory, std::string& error) {
    auto& store = GetStore();
    std::filesystem::path directory;
    if (!EnsureSessionDirectory(directory, error)) return false;
    ownerDirectory = directory;
    copy = directory / ("mesh-" + std::to_string(++store.tempCounter) + ".abc");
    std::error_code ec;
    std::filesystem::copy_file(abc, copy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Cannot copy " + Quoted(abc) + " into the mesh cache: " + ec.message();
        std::filesystem::remove(copy, ec);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Record codec
// ---------------------------------------------------------------------------

struct DiskEntry {
    ObjectKey key;
    std::string relativePath;
    double timeScale = 1;
    std::uint32_t cutoffLayer = kNoCutoffLayer;
};

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(std::uint8_t((value >> shift) & 0xFFu));
}

void AppendU8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

// The version 2 layout stores an IEEE 754 binary64 as eight little-endian bytes,
// so the record is independent of the byte order of the machine that wrote it.
void AppendF64(std::vector<std::uint8_t>& out, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "the record stores a time scale as an IEEE 754 binary64");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(std::uint8_t((bits >> shift) & 0xFFu));
}

bool TakeU8(const std::vector<std::uint8_t>& value, std::size_t& cursor,
            std::uint8_t& out) {
    if (cursor >= value.size()) return false;
    out = value[cursor++];
    return true;
}

bool TakeU32(const std::vector<std::uint8_t>& value, std::size_t& cursor,
             std::uint32_t& out) {
    if (cursor + 4 > value.size()) return false;
    out = std::uint32_t(value[cursor]) | (std::uint32_t(value[cursor + 1]) << 8) |
          (std::uint32_t(value[cursor + 2]) << 16) |
          (std::uint32_t(value[cursor + 3]) << 24);
    cursor += 4;
    return true;
}

bool TakeF64(const std::vector<std::uint8_t>& value, std::size_t& cursor,
             double& out) {
    if (cursor + 8 > value.size()) return false;
    std::uint64_t bits = 0;
    for (int shift = 0; shift < 64; shift += 8)
        bits |= std::uint64_t(value[cursor + std::size_t(shift / 8)]) << shift;
    cursor += 8;
    std::memcpy(&out, &bits, sizeof out);
    return true;
}

// A time scale this module may act on: finite and strictly positive, so it can
// never invert, freeze or overflow the sampling division.
bool ValidTimeScale(double timeScale) {
    return std::isfinite(timeScale) && timeScale > 0;
}

std::vector<std::uint8_t> EncodeRecord(const std::vector<DiskEntry>& entries,
                                       std::uint32_t version) {
    std::vector<std::uint8_t> value;
    value.reserve(12 + entries.size() * 40);
    AppendU32(value, kRecordMagic);
    AppendU32(value, version);
    AppendU32(value, std::uint32_t(entries.size()));
    for (const auto& entry : entries) {
        AppendU8(value, std::uint8_t(entry.key.kind));
        AppendU8(value, 0);
        AppendU8(value, 0);
        AppendU8(value, 0);
        AppendU32(value, std::uint32_t(entry.key.slot));
        AppendU32(value, std::uint32_t(entry.relativePath.size()));
        value.insert(value.end(), entry.relativePath.begin(), entry.relativePath.end());
        if (version >= kRecordVersionV2) AppendF64(value, entry.timeScale);
        if (version >= kRecordVersionV3) AppendU32(value, entry.cutoffLayer);
    }
    return value;
}

bool ValidSlot(const ObjectKey& key) {
    if (key.kind == kKindModel) return key.slot >= 0 && key.slot < kModelSlotCount;
    if (key.kind == kKindAccessory)
        return key.slot >= 0 && key.slot < kAccessorySlotCount;
    return false;
}

// Rejects anything the module would not have written: the path must stay inside
// the PMM directory, carry no NUL or control byte and name a sidecar.
bool ValidRelativePath(const std::string& text, std::string& why) {
    const std::string suffix = kSidecarSuffix;
    if (text.empty() || text.size() > kMaxRelativePathBytes) {
        why = "the sidecar path length is out of range";
        return false;
    }
    for (const char c : text) {
        if (c == '\0' || static_cast<unsigned char>(c) < 0x20) {
            why = "the sidecar path contains a control byte";
            return false;
        }
    }
    if (text.size() <= suffix.size() ||
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) != 0) {
        why = "the sidecar path does not end in " + suffix;
        return false;
    }
    const std::filesystem::path path = std::filesystem::u8path(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        why = "the sidecar path is absolute";
        return false;
    }
    for (const auto& part : path) {
        if (part == L".." || part == L"." || part.empty()) {
            why = "the sidecar path leaves the project directory";
            return false;
        }
    }
    return true;
}

// Full structural validation.  Nothing is installed until this has accepted the
// whole value, so a malformed record cannot half-apply.  `expectedVersion` is
// the version the key the value was read from promises: a record whose internal
// version disagrees with its key is rejected rather than guessed at.
bool ParseRecord(const std::vector<std::uint8_t>& value,
                 std::uint32_t expectedVersion, std::vector<DiskEntry>& entries,
                 std::string& error) {
    std::size_t cursor = 0;
    std::uint32_t magic = 0, version = 0, count = 0;
    if (!TakeU32(value, cursor, magic) ||
        (magic != kRecordMagic && magic != kLegacyRecordMagic)) {
        error = "The mesh cache record does not start with its magic number";
        return false;
    }
    if (!TakeU32(value, cursor, version) || version != expectedVersion) {
        error = "The mesh cache record has an unsupported version";
        return false;
    }
    if (!TakeU32(value, cursor, count) || count > kMaxRecordEntries) {
        error = "The mesh cache record has an invalid entry count";
        return false;
    }

    std::vector<DiskEntry> parsed;
    parsed.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint8_t kind = 0, reserved1 = 0, reserved2a = 0, reserved2b = 0;
        std::uint32_t slot = 0, pathBytes = 0;
        if (!TakeU8(value, cursor, kind) || !TakeU8(value, cursor, reserved1) ||
            !TakeU8(value, cursor, reserved2a) || !TakeU8(value, cursor, reserved2b) ||
            !TakeU32(value, cursor, slot) || !TakeU32(value, cursor, pathBytes)) {
            error = "The mesh cache record is shorter than its entry count requires";
            return false;
        }
        if (reserved1 != 0 || reserved2a != 0 || reserved2b != 0) {
            error = "The mesh cache record has a non-zero reserved byte";
            return false;
        }
        if (pathBytes == 0 || pathBytes > kMaxRelativePathBytes ||
            cursor + pathBytes > value.size()) {
            error = "The mesh cache record has an invalid sidecar path length";
            return false;
        }
        DiskEntry entry;
        entry.key.kind = kind;
        entry.key.slot = int(slot);
        entry.relativePath.assign(
            reinterpret_cast<const char*>(value.data() + cursor), pathBytes);
        cursor += pathBytes;
        // The version 2 layout appends the time scale of this entry, and the
        // version 3 layout its cutoff layer.  A record that is short here, or
        // that carries a value which could never have been written, is rejected
        // as a whole.
        if (version >= kRecordVersionV2) {
            if (!TakeF64(value, cursor, entry.timeScale)) {
                error = "The mesh cache record is shorter than its time scales require";
                return false;
            }
            if (!ValidTimeScale(entry.timeScale)) {
                error = "The mesh cache record carries an invalid time scale";
                return false;
            }
        }
        if (version >= kRecordVersionV3) {
            if (!TakeU32(value, cursor, entry.cutoffLayer)) {
                error = "The mesh cache record is shorter than its cutoff layers require";
                return false;
            }
            if (entry.cutoffLayer != kNoCutoffLayer && entry.cutoffLayer > 65535u) {
                error = "The mesh cache record carries an invalid cutoff layer";
                return false;
            }
        }
        if (!ValidSlot(entry.key)) {
            error = "The mesh cache record names an out-of-range object slot";
            return false;
        }
        if (!ValidRelativePath(entry.relativePath, error)) return false;
        for (const auto& previous : parsed) {
            if (previous.key == entry.key) {
                error = "The mesh cache record names the same object slot twice";
                return false;
            }
        }
        parsed.push_back(std::move(entry));
    }
    if (cursor != value.size()) {
        error = "The mesh cache record has trailing bytes";
        return false;
    }
    entries = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
// Persistent sidecars
// ---------------------------------------------------------------------------

// Content hash used to name a published sidecar.  Two independent 64-bit
// accumulators make an accidental clash between different bakes negligible.
bool HashFile(const std::filesystem::path& file, std::uint64_t& first,
              std::uint64_t& second, std::uint64_t& size, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open()) {
        error = "Cannot read the mesh cache archive " + Quoted(file);
        return false;
    }
    std::uint64_t fnv = 14695981039346656037ull;
    std::uint64_t mix = 0x9E3779B97F4A7C15ull;
    std::vector<char> buffer(kHashChunkBytes);
    size = 0;
    while (stream) {
        stream.read(buffer.data(), std::streamsize(buffer.size()));
        const std::streamsize got = stream.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            const auto byte = static_cast<unsigned char>(buffer[std::size_t(i)]);
            fnv ^= byte;
            fnv *= 1099511628211ull;
            mix ^= byte;
            mix += 0x165667B19E3779F9ull;
            mix = (mix << 31) | (mix >> 33);
            mix *= 0x9E3779B97F4A7C15ull;
        }
        size += std::uint64_t(got);
    }
    if (!stream.eof()) {
        error = "Cannot read the mesh cache archive " + Quoted(file);
        return false;
    }
    first = fnv;
    second = mix;
    return true;
}

std::string ShortStem(const std::string& stem) {
    if (stem.size() <= kMaxSidecarStemBytes) return stem;
    std::size_t cut = kMaxSidecarStemBytes;
    while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0u) == 0x80u) --cut;
    return stem.substr(0, cut);
}

// Copies `archive` next to the PMM under a name that is unique per archive
// content, and returns the path relative to the PMM directory.  An existing
// sidecar with that name came from the same bytes, so it is reused as is.  Old
// sidecars of earlier versions are never removed here: the caller may still
// hold a PMM that points at them.
bool PublishSidecar(const std::filesystem::path& directory,
                    const std::filesystem::path& pmmStem, const Archive& archive,
                    std::string& relative, std::string& error) {
    std::uint64_t first = 0, second = 0, size = 0;
    if (!HashFile(archive.file, first, second, size, error)) return false;
    char hash[33]{};
    std::snprintf(hash, sizeof hash, "%016llx%016llx",
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(second));
    const std::string name =
        ShortStem(Utf8(pmmStem)) + "." + hash + kSidecarSuffix;
    const auto target = directory / std::filesystem::u8path(name);

    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        const auto staging = directory / std::filesystem::u8path(
            name + ".partial-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::copy_file(archive.file, staging,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Cannot publish the mesh cache sidecar next to the project: " +
                    ec.message();
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return false;
        }
        std::filesystem::rename(staging, target, ec);
        if (ec && !std::filesystem::exists(target, ec)) {
            error = "Cannot move the mesh cache sidecar into place: " + ec.message();
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return false;
        }
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
    }

    std::uintmax_t published = 0;
    ec.clear();
    published = std::filesystem::file_size(target, ec);
    if (ec || published != size) {
        error = "The published mesh cache sidecar " + Quoted(target) +
                " does not match the cached archive";
        return false;
    }
    relative = name;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Retime
// ---------------------------------------------------------------------------

// One prepared retime.  The type is opaque outside this file: a caller only
// hands the pointer back to CommitTimeScale.
struct TimeScalePlan {
    // Cache revision the plan was built from.  A plan whose revision is no
    // longer the live one was overtaken by another change of the cache.
    unsigned long long revision = 0;
    // Set once the plan has been applied, so it cannot be applied twice.
    bool consumed = false;
    // Complete next state: the entries that were live when the plan was built,
    // each a copy that carries the multiplied time scale and shares the archive
    // of the entry it came from.  No archive is reopened and no file is copied.
    StoreMap objects;
};

namespace {

// Rejects a retimed entry the timeline cannot hold: the sampling division must
// stay exact enough to be useful, and a native range may not be pushed past the
// last frame MMD accepts.
bool ValidRetimedEntry(const ObjectEntry& entry, double timeScale,
                       std::string& why) {
    if (!ValidTimeScale(timeScale)) {
        why = "the resulting time scale is not a finite positive number";
        return false;
    }
    const auto& source = *entry.source;
    if (!source.rangeLimited) return true;
    const double first = source.firstSeconds * kBakeFrameRate * timeScale;
    const double last = source.endSeconds * kBakeFrameRate * timeScale;
    if (!std::isfinite(first) || !std::isfinite(last) || first < 0 || last < first ||
        last > kMaxTimelineFrames) {
        why = "the resulting frame range would leave the timeline";
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool Install(MMDApp& app, const std::filesystem::path& abc, std::string& error,
             const std::function<bool(std::string&)>& beforeCommit,
             std::uint32_t cutoffLayer) {
    error.clear();
    if (abc.empty()) {
        error = "The mesh cache source path is empty";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(abc, ec)) {
        error = "The mesh cache source " + Quoted(abc) + " does not exist";
        return false;
    }

    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);

    std::filesystem::path copy;
    std::filesystem::path ownerDirectory;
    if (!CopyToPrivateFile(abc, copy, ownerDirectory, error)) return false;

    std::shared_ptr<Archive> archive;
    if (!OpenArchive(copy, true, ownerDirectory, archive, error)) return false;

    std::map<ObjectKey, std::vector<Child>> groups;
    std::size_t ignored = 0;
    if (!EnumerateChildren(*archive, groups, ignored, error)) return false;
    if (groups.empty()) {
        error = "The archive " + Quoted(abc) +
                " contains no model_<slot>_material_... or accessory_<slot>_material_..."
                " mesh";
        return false;
    }

    // The archive has parsed completely; allocate the candidate entries.  The
    // private copy and these entries are released by RAII if anything below
    // fails, so the cache cannot be left half changed.
    std::map<const void*, EntryHandle> pending;
    for (auto& group : groups) {
        const void* object = LiveObject(app, group.first.kind, group.first.slot);
        if (!object) continue;
        auto entry = std::make_shared<ObjectEntry>();
        entry->key = group.first;
        entry->source = archive;
        entry->children = std::move(group.second);
        entry->cutoffLayer = cutoffLayer;
        // sourceGeneration stays 0 (unbound): each object binds its own
        // generation exactly once at its first replay through BindGeneration.
        pending[object] = std::move(entry);
    }
    if (pending.empty()) {
        error = "The archive " + Quoted(abc) +
                " names no model or accessory that is currently in the scene";
        return false;
    }

    // The complete next state: the entries in use now, with the candidates
    // substituted in.  This is the last step that can allocate or throw, and it
    // leaves the live map untouched.
    StoreMap candidate;
    try {
        candidate = store.objects;
        for (auto& item : pending) candidate[item.first] = std::move(item.second);
    } catch (const std::exception& e) {
        error = std::string("Cannot prepare the mesh cache commit: ") + e.what();
        return false;
    }

    // Last chance to veto, with the candidate fully built and the previous cache
    // still untouched.
    if (beforeCommit) {
        std::string reason;
        bool commit = false;
        try {
            commit = beforeCommit(reason);
        } catch (const std::exception& e) {
            error = std::string("The mesh cache install was rejected by the caller: ") +
                    e.what();
            return false;
        } catch (...) {
            error = "The mesh cache install was rejected by the caller";
            return false;
        }
        if (!commit) {
            error = reason.empty() ? std::string("The mesh cache install was cancelled")
                                   : std::move(reason);
            return false;
        }
    }

    // Commit: a pure pointer swap that cannot throw, and nothing after it can
    // fail the install.  The entries in use until now move into `candidate` and
    // are released when it leaves this scope; that release only deletes private
    // copies which nothing references any more.
    static_assert(
        noexcept(std::declval<StoreMap&>().swap(std::declval<StoreMap&>())),
        "the mesh cache commit must be a noexcept container swap");
    store.objects.swap(candidate);
    ++store.revision;
    return true;
}

bool Has(const void* object) {
    if (!object) return false;
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.objects.find(object);
    return found != store.objects.end() && found->second != nullptr;
}

bool Info(const void* object, EntryInfo& out) {
    out = EntryInfo{};
    if (!object) return false;
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.objects.find(object);
    if (found == store.objects.end() || !found->second) return false;
    out.cutoffLayer = found->second->cutoffLayer;
    out.sourceGeneration = found->second->sourceGeneration;
    return true;
}

bool BindGeneration(const void* object, std::uint64_t generation) {
    if (!object || generation == 0) return false;
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.objects.find(object);
    if (found == store.objects.end() || !found->second) return false;
    auto& entry = *const_cast<ObjectEntry*>(found->second.get());
    // Binding happens exactly once per entry: a cache loaded from a project
    // binds the object it first plays back on, and a later object replacement
    // makes the entry stale instead of silently rebinding every frame.
    if (entry.sourceGeneration == 0) { entry.sourceGeneration = generation; return true; }
    return entry.sourceGeneration == generation;
}

bool RangeLimited(const void* object) {
    auto& store=GetStore();std::lock_guard<std::mutex> lock(store.mutex);
    const auto found=store.objects.find(object);
    return found!=store.objects.end()&&found->second&&found->second->source->rangeLimited;
}
bool ActiveAt(const void* object,double seconds) {
    if(!std::isfinite(seconds))return false;
    auto& store=GetStore();std::lock_guard<std::mutex> lock(store.mutex);
    const auto found=store.objects.find(object);if(found==store.objects.end()||!found->second)return false;
    const auto& entry=*found->second;const auto& source=*entry.source;
    if(!source.rangeLimited)return true;
    return seconds>=ScaledBoundarySeconds(source.firstSeconds,entry.timeScale)&&
           seconds<ScaledBoundarySeconds(source.endSeconds,entry.timeScale);
}

bool Sample(const void* object, double seconds, std::vector<Mesh>& meshes,
            std::string& error) {
    meshes.clear();
    error.clear();
    if (!object) {
        error = "No object was given for the mesh cache lookup";
        return false;
    }
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    const auto found = store.objects.find(object);
    if (found == store.objects.end() || !found->second) {
        error = "This object has no baked mesh cache";
        return false;
    }
    const ObjectEntry& entry = *found->second;
    // The caller samples the retimed timeline; the archive is read at the time
    // the bake was originally recorded at.
    const double sourceSeconds = seconds / entry.timeScale;
    std::vector<Mesh> group;
    group.reserve(entry.children.size());
    for (const auto& child : entry.children) {
        Mesh mesh;
        if (!ReadMesh(*entry.source, child,
                      SampleIndex(child.times, sourceSeconds), mesh, error))
            return false;
        group.push_back(std::move(mesh));
    }
    if (group.empty()) {
        error = "The baked mesh cache entry for this object is empty";
        return false;
    }
    meshes = std::move(group);
    return true;
}

void Forget(const void* object) {
    if (!object) return;
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    if (store.objects.erase(object) != 0) ++store.revision;
}

void Clear() {
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    // Dropping the entries releases the archives, and each archive removes the
    // one file it copied into this process's own directory.
    if (!store.objects.empty()) ++store.revision;
    store.objects.clear();
    if (store.sessionDirectory.empty()) return;
    std::error_code ignored;
    // Deliberately not remove_all: this only ever deletes the private directory
    // of this process, and only while it is empty.  The shared root above it is
    // never touched, and a file this module did not create - whether it sits in
    // the shared root or was planted inside our own directory - makes the
    // removal fail harmlessly and is left exactly where it is.
    if (std::filesystem::remove(store.sessionDirectory, ignored)) {
        store.sessionDirectory.clear();
        return;
    }
    std::error_code existsError;
    if (!std::filesystem::is_directory(store.sessionDirectory, existsError))
        store.sessionDirectory.clear();
}

bool Empty() {
    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.objects.empty();
}

bool Save(MMDApp& app, const std::filesystem::path& pmm,
          pmm_extension::Document& document, std::string& error) {
    error.clear();
    if (pmm.empty()) {
        error = "The project path is required to publish the mesh cache";
        return false;
    }
    const auto directory = std::filesystem::absolute(pmm).parent_path();
    const auto stem = pmm.stem();
    if (directory.empty() || stem.empty()) {
        error = "The project path " + Quoted(pmm) +
                " has no directory or file name to publish the mesh cache next to";
        return false;
    }

    std::lock_guard<std::mutex> lock(GetStore().mutex);

    // Resolve through the live slots: an object that has been unloaded since it
    // was baked is not recorded, and the slot number always matches the scene.
    struct Live {
        ObjectKey key;
        std::shared_ptr<Archive> source;
        double timeScale = 1;
        std::uint32_t cutoffLayer = kNoCutoffLayer;
    };
    std::vector<Live> live;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        const void* object = app.ModelSlot(slot);
        if (!object) continue;
        const auto found = GetStore().objects.find(object);
        if (found == GetStore().objects.end() || !found->second) continue;
        live.push_back({found->second->key, found->second->source,
                        found->second->timeScale, found->second->cutoffLayer});
    }
    for (int slot = 0; slot < kAccessorySlotCount; ++slot) {
        const void* object = app.AccessorySlot(slot);
        if (!object) continue;
        const auto found = GetStore().objects.find(object);
        if (found == GetStore().objects.end() || !found->second) continue;
        live.push_back({found->second->key, found->second->source,
                        found->second->timeScale, found->second->cutoffLayer});
    }

    if (live.empty()) {
        // A stale record would otherwise point at sidecars of a cache the user
        // has already replaced or cleared. All known versions go, so a project
        // never keeps a record of a cache that is no longer there.
        pmm_extension::Erase(document, kRecordKey);
        pmm_extension::Erase(document, kRecordKeyV2);
        pmm_extension::Erase(document, kRecordKeyV3);
        return true;
    }

    std::map<const Archive*, std::string> published;
    std::vector<DiskEntry> entries;
    entries.reserve(live.size());
    bool retimed = false;
    bool layered = false;
    for (const auto& item : live) {
        // One object cannot legitimately occupy two slots, but never emit a
        // record that Load would reject as a duplicate slot.
        bool duplicate = false;
        for (const auto& seen : entries)
            duplicate = duplicate || seen.key == item.key;
        if (duplicate) continue;
        const Archive* source = item.source.get();
        auto found = published.find(source);
        if (found == published.end()) {
            std::string relative;
            if (!PublishSidecar(directory, stem, *source, relative, error))
                return false;
            found = published.emplace(source, std::move(relative)).first;
        }
        DiskEntry entry;
        entry.key = item.key;
        entry.relativePath = found->second;
        entry.timeScale = item.timeScale;
        entry.cutoffLayer = item.cutoffLayer;
        retimed = retimed || entry.timeScale != 1.0;
        layered = layered || entry.cutoffLayer != kNoCutoffLayer;
        entries.push_back(std::move(entry));
    }

    // The document is only touched after every sidecar is in place.  Exactly one
    // of the known versions stays behind: a layered cache needs the version that
    // carries its cutoff layers, a retimed one the version that carries its time
    // scales, and neither may leave a stale record of another version that Load
    // would prefer.
    if (layered) {
        pmm_extension::Set(document, kRecordKeyV3,
                           EncodeRecord(entries, kRecordVersionV3));
        pmm_extension::Erase(document, kRecordKey);
        pmm_extension::Erase(document, kRecordKeyV2);
    } else if (retimed) {
        pmm_extension::Set(document, kRecordKeyV2,
                           EncodeRecord(entries, kRecordVersionV2));
        pmm_extension::Erase(document, kRecordKey);
        pmm_extension::Erase(document, kRecordKeyV3);
    } else {
        pmm_extension::Set(document, kRecordKey,
                           EncodeRecord(entries, kRecordVersion));
        pmm_extension::Erase(document, kRecordKeyV2);
        pmm_extension::Erase(document, kRecordKeyV3);
    }
    return true;
}

bool Load(MMDApp& app, const std::filesystem::path& pmm,
          const pmm_extension::Document& document, std::string& error) {
    error.clear();
    // The newest record wins while several are present: a version 3 record
    // carries both the cutoff layers and the time scales, a version 2 one the
    // time scales that a version 1 record left over from an earlier save would
    // otherwise hide.
    const auto* layeredRecord = pmm_extension::Find(document, kRecordKeyV3);
    const auto* retimedRecord = pmm_extension::Find(document, kRecordKeyV2);
    const auto* legacyRecord = pmm_extension::Find(document, kRecordKey);
    if (!layeredRecord && !retimedRecord && !legacyRecord) return true;  // never baked
    if (pmm.empty()) {
        error = "The project path is required to resolve the mesh cache sidecars";
        return false;
    }
    const auto directory = std::filesystem::absolute(pmm).parent_path();
    if (directory.empty()) {
        error = "The project path " + Quoted(pmm) +
                " has no directory to resolve the mesh cache sidecars against";
        return false;
    }

    // Each key promises its own internal version, and a mismatch is a malformed
    // record rather than a reason to fall back to the other key.
    std::vector<DiskEntry> entries;
    if (layeredRecord) {
        if (!ParseRecord(layeredRecord->value, kRecordVersionV3, entries, error))
            return false;
    } else if (retimedRecord) {
        if (!ParseRecord(retimedRecord->value, kRecordVersionV2, entries, error))
            return false;
    } else if (!ParseRecord(legacyRecord->value, kRecordVersion, entries, error)) {
        return false;
    }
    if (entries.empty()) return true;

    std::lock_guard<std::mutex> lock(GetStore().mutex);

    // Open each sidecar once, then take from it only the objects this record
    // names, so a sidecar shared with an earlier bake cannot add extra objects.
    struct Sidecar {
        std::shared_ptr<Archive> archive;
        std::map<ObjectKey, std::vector<Child>> groups;
    };
    std::map<std::string, Sidecar> opened;
    std::map<const void*, EntryHandle> pending;
    for (const auto& entry : entries) {
        auto found = opened.find(entry.relativePath);
        if (found == opened.end()) {
            Sidecar fresh;
            const auto file = directory / std::filesystem::u8path(entry.relativePath);
            // A sidecar is persistent state of the project, not a private copy:
            // it has no owner directory and is never removed by this module.
            if (!OpenArchive(file, false, std::filesystem::path(), fresh.archive,
                             error))
                return false;
            std::size_t ignored = 0;
            if (!EnumerateChildren(*fresh.archive, fresh.groups, ignored, error))
                return false;
            found = opened.emplace(entry.relativePath, std::move(fresh)).first;
        }
        const auto group = found->second.groups.find(entry.key);
        if (group == found->second.groups.end()) {
            error = "The mesh cache sidecar '" + entry.relativePath +
                    "' does not contain the object slot the project records";
            return false;
        }
        const void* object = LiveObject(app, entry.key.kind, entry.key.slot);
        if (!object) continue;  // that object is not in the scene right now
        auto installed = std::make_shared<ObjectEntry>();
        installed->key = entry.key;
        installed->source = found->second.archive;
        installed->children = group->second;
        installed->timeScale = entry.timeScale;
        installed->cutoffLayer = entry.cutoffLayer;
        pending[object] = std::move(installed);
    }
    if (pending.empty()) {
        error = "The project records a mesh cache, but none of its objects is "
                "currently in the scene";
        return false;
    }

    // Commit through a fresh map and a swap, exactly like Install, so a failed
    // allocation cannot leave the project cache half applied.
    StoreMap candidate;
    try {
        candidate = GetStore().objects;
        for (auto& item : pending) candidate[item.first] = std::move(item.second);
    } catch (const std::exception& e) {
        error = std::string("Cannot prepare the mesh cache load: ") + e.what();
        return false;
    }
    GetStore().objects.swap(candidate);
    ++GetStore().revision;
    return true;
}

// ---------------------------------------------------------------------------
// Retime
// ---------------------------------------------------------------------------

bool PrepareTimeScale(double ratio, TimeScalePlanPtr& plan, std::size_t& count,
                      std::string& error) {
    error.clear();
    count = 0;
    plan.reset();
    if (!ValidTimeScale(ratio)) {
        error = "The mesh cache retime ratio must be a finite positive number";
        return false;
    }

    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);

    // The whole next state is allocated here, while the live cache is untouched,
    // so the commit itself cannot fail on allocation.  The plan object is built
    // inside the same guard: this interface reports failure through its return
    // value and `error`, so an allocation that fails must not escape as an
    // exception.
    StoreMap retimed;
    TimeScalePlanPtr fresh;
    try {
        for (const auto& item : store.objects) {
            if (!item.second) continue;
            const ObjectEntry& entry = *item.second;
            const double timeScale = entry.timeScale * ratio;
            std::string why;
            if (!ValidRetimedEntry(entry, timeScale, why)) {
                error = "The mesh cache cannot be retimed by " +
                        std::to_string(ratio) + ": " + why;
                return false;
            }
            auto copy = std::make_shared<ObjectEntry>(entry);
            copy->timeScale = timeScale;
            copy->cutoffLayer = entry.cutoffLayer;
            copy->sourceGeneration = entry.sourceGeneration;
            retimed[item.first] = std::move(copy);
        }
        fresh = std::make_shared<TimeScalePlan>();
        fresh->revision = store.revision;
        fresh->objects = std::move(retimed);
    } catch (const std::exception& e) {
        error = std::string("Cannot prepare the mesh cache retime: ") + e.what();
        return false;
    }

    count = fresh->objects.size();
    plan = std::move(fresh);
    return true;
}

bool CommitTimeScale(const TimeScalePlanPtr& plan, std::string& error,
                     const std::function<bool(std::string&)>& beforeCommit) {
    error.clear();
    if (!plan) {
        error = "The mesh cache retime plan is empty";
        return false;
    }

    auto& store = GetStore();
    std::lock_guard<std::mutex> lock(store.mutex);

    if (plan->consumed) {
        error = "The mesh cache retime plan was already applied";
        return false;
    }
    if (plan->revision != store.revision) {
        error = "The mesh cache changed after this retime was prepared";
        return false;
    }

    // The complete next state, copied from the plan before anything can still
    // fail.  Copying the map only copies handles: the entries, and the archives
    // behind them, are shared with the plan and are not opened again.
    StoreMap candidate;
    try {
        candidate = plan->objects;
    } catch (const std::exception& e) {
        error = std::string("Cannot prepare the mesh cache retime commit: ") +
                e.what();
        return false;
    }

    // Same last chance to veto as in Install.  The callback also runs for a plan
    // whose cache is empty: the caller uses this commit as the transaction that
    // wraps its own work, and that transaction has to happen even when no
    // geometry is cached at all.
    if (beforeCommit) {
        std::string reason;
        bool commit = false;
        try {
            commit = beforeCommit(reason);
        } catch (const std::exception& e) {
            error = std::string("The mesh cache retime was rejected by the caller: ") +
                    e.what();
            return false;
        } catch (...) {
            error = "The mesh cache retime was rejected by the caller";
            return false;
        }
        if (!commit) {
            error = reason.empty() ? std::string("The mesh cache retime was cancelled")
                                   : std::move(reason);
            return false;
        }
    }

    // Commit: a pure pointer swap that cannot throw.  The entries in use until
    // now move into `candidate` and are released when it leaves this scope.
    static_assert(
        noexcept(std::declval<StoreMap&>().swap(std::declval<StoreMap&>())),
        "the mesh cache retime commit must be a noexcept container swap");
    store.objects.swap(candidate);
    ++store.revision;
    plan->consumed = true;
    return true;
}

}  // namespace mesh_bake_cache
}  // namespace mikumikustudio
