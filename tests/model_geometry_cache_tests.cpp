#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "../src/model/model_geometry_cache.hpp"
#include "../src/model/pmx_vertex_prepare.hpp"
#include "mikumikustudio/sequential_file_reader.hpp"
#include "model_geometry_reference.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <io.h>
#include <share.h>

using namespace mikumikustudio;
using namespace mikumikustudio::model_geometry_cache;
namespace {
using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;
std::size_t checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL %s last_error=%lu\n", message, GetLastError());
        std::exit(1);
    }
}
template<class T> void Put(Bytes& bytes, T value) {
    const auto* data = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(T));
}
void Write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Check(bool(stream), "create owned fixture");
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.close();
    Check(bool(stream), "write owned fixture");
}
Bytes Pmx(std::uint32_t vertices, unsigned uv = 4, unsigned boneSize = 2,
          unsigned vertexSize = 2) {
    Bytes bytes{'P','M','X',' '};
    Put(bytes, 2.f);
    for (unsigned char v : std::array<unsigned char, 9>{8, 0, static_cast<unsigned char>(uv),
         static_cast<unsigned char>(vertexSize), 1, 1,
         static_cast<unsigned char>(boneSize), 1, 1})
        Put(bytes, v);
    for (int text = 0; text < 4; ++text) Put(bytes, 0u);
    Put(bytes, vertices);
    for (std::uint32_t vertex = 0; vertex < vertices; ++vertex) {
        for (unsigned value = 0; value < 8 + uv * 4; ++value)
            Put(bytes, float(value) * .25f + float(vertex % 31));
        const unsigned char type = static_cast<unsigned char>(vertex % 6);
        Put(bytes, type);
        const auto bone = [&](int slot) {
            const auto value = static_cast<int>(vertex % 193) - 96 + slot;
            if (boneSize == 1) Put(bytes, static_cast<std::int8_t>(value));
            else if (boneSize == 2) Put(bytes, static_cast<std::int16_t>(value));
            else Put(bytes, value);
        };
        bone(0);
        if (type == 1 || type == 3) {
            bone(1); Put(bytes, .25f);
            if (type == 3)
                for (int i = 0; i < 9; ++i) Put(bytes, float(i * 3 - 9));
        } else if (type == 2) {
            bone(1); bone(2); bone(3);
            for (int i = 0; i < 4; ++i) Put(bytes, .25f);
        }
        Put(bytes, .75f);
    }
    Put(bytes, vertices * 3);
    for (std::uint32_t index = 0; index < vertices * 3; ++index) {
        if (vertexSize == 1) Put(bytes, static_cast<std::uint8_t>(index % vertices));
        else if (vertexSize == 2) Put(bytes, static_cast<std::uint16_t>(index % vertices));
        else Put(bytes, index % vertices);
    }
    // Minimal empty materials/bones/morphs/display/rigid/joint sections.
    for (int section = 0; section < 6; ++section) Put(bytes, 0u);
    return bytes;
}
Bytes Pmd(std::uint32_t vertices) {
    Bytes bytes{'P','m','d'};
    Put(bytes, 1.f);
    bytes.resize(283, 0); // name[20] and comment[256]
    Put(bytes, vertices);
    for (std::uint32_t i = 0; i < vertices; ++i) {
        for (int value = 0; value < 8; ++value) Put(bytes, float(i % 31 + value));
        Put(bytes, static_cast<std::int16_t>(i % 193));
        Put(bytes, static_cast<std::int16_t>(-1));
        Put(bytes, static_cast<std::int8_t>(i % 101));
        Put(bytes, static_cast<std::uint8_t>(i % 2));
    }
    Put(bytes, vertices * 3);
    for (std::uint32_t i = 0; i < vertices * 3; ++i)
        Put(bytes, static_cast<std::uint16_t>(i % vertices));
    bytes.resize(bytes.size() + 24, 0);
    return bytes;
}
struct Loaded {
    Layout layout;
    std::vector<mdl::PmxVertex> pmx;
    std::vector<mdl::PmdVertex> pmd;
    std::vector<std::int32_t> indices;
    std::int64_t vertexStart = -1, vertexEnd = -1, indexEnd = -1;
    bool hit = false, enabled = false, complete = false;
    double milliseconds = 0;
};
bool ReadExact(fileio::SequentialFileReader& reader, void* out, unsigned count) {
    return reader.Read(out, count) == static_cast<int>(count);
}
Loaded Load(const std::filesystem::path& path, Cache& cache, bool disabled = false,
            bool fullLoadSucceeded = true, bool synthetic = false,
            bool originalReference = false) {
    Loaded result;
    const auto start = Clock::now();
    int descriptor = -1;
    if (_wsopen_s(&descriptor, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0))
        return result;
    fileio::SequentialFileReader reader(descriptor);
    char magic[3]{};
    if (!ReadExact(reader, magic, 3)) return result;
    if (!std::memcmp(magic, "PMX", 3)) {
        char space = 0;
        float version = 0;
        unsigned char header[9]{};
        if (!ReadExact(reader, &space, 1) || !ReadExact(reader, &version, 4)
            || version != 2.f || !ReadExact(reader, header, 9)
            || header[0] != 8 || header[1] != 0 || header[2] > 4)
            return result;
        result.layout = {Format::pmx, 0, header[2], header[6], header[3]};
        const auto length = _filelengthi64(descriptor);
        for (int text = 0; text < 4; ++text) {
            std::uint32_t bytes = 0;
            if (!ReadExact(reader, &bytes, 4) || (bytes & 1u)
                || reader.Tell() > length || bytes > length - reader.Tell()
                || !reader.SeekAbsolute(reader.Tell() + bytes))
                return result;
        }
    } else if (!std::memcmp(magic, "Pmd", 3)) {
        result.layout.format = Format::pmd;
        if (!reader.SeekAbsolute(283)) return result;
    } else return result;
    if (!ReadExact(reader, &result.layout.vertexCount, 4)) return result;
    result.vertexStart = reader.Tell();
    auto session = Session::Begin(descriptor, result.layout, result.vertexStart, &cache, disabled);
    result.hit = session.Hit();
    result.enabled = session.Enabled();
    const auto count = result.layout.vertexCount;
    if (count > 4 * 1024 * 1024) return result;
    if (result.layout.format == Format::pmx) {
        result.pmx.resize(count);
        if (count) std::memset(result.pmx.data(), 0xcd, count * sizeof(mdl::PmxVertex));
        if (session.Hit()) {
            if (!session.CopyPmxVertices(result.pmx.data())
                || !reader.SeekAbsolute(session.VertexEnd())) return result;
        } else {
            for (auto& vertex : result.pmx) {
                if (originalReference)
                    geometry_test_reference::OriginalVertex(reader,
                        result.layout.additionalUv, result.layout.boneIndexSize, vertex);
                else if (!pmx_prepare::ReadPackedVertex(reader, result.layout.additionalUv,
                                                        result.layout.boneIndexSize, vertex))
                    return result;
            }
        }
    } else {
        result.pmd.resize(count);
        if (session.Hit()) {
            if (!session.CopyPmdVertices(result.pmd.data())
                || !reader.SeekAbsolute(session.VertexEnd())) return result;
        } else {
            for (auto& vertex : result.pmd)
                if (!ReadExact(reader, &vertex, 38)) return result;
        }
    }
    result.vertexEnd = reader.Tell();
    std::uint32_t indexCount = 0;
    if (!ReadExact(reader, &indexCount, 4) || indexCount > 12 * 1024 * 1024)
        return result;
    result.indices.resize(indexCount);
    if (session.Hit()) {
        const bool copied = result.layout.format == Format::pmx
            ? session.CopyPmxIndices(result.indices.data(), indexCount)
            : session.CopyPmdIndices(reinterpret_cast<std::uint16_t*>(result.indices.data()),
                                     indexCount);
        if (!copied || !reader.SeekAbsolute(session.IndexEnd())) return result;
    } else {
        const unsigned indexBytes = result.layout.format == Format::pmd ? 2
            : result.layout.vertexIndexSize == 1 || result.layout.vertexIndexSize == 2
                ? result.layout.vertexIndexSize : 4;
        for (auto& index : result.indices)
            if (!ReadExact(reader, &index, indexBytes)) return result;
    }
    result.indexEnd = reader.Tell();
    if (result.layout.format == Format::pmx)
        session.CapturePmx(result.pmx.data(), result.indices.data(), indexCount,
                           result.vertexEnd, result.indexEnd);
    else
        session.CapturePmd(result.pmd.data(),
                           reinterpret_cast<const std::uint16_t*>(result.indices.data()),
                           indexCount, result.vertexEnd, result.indexEnd);
    if (synthetic) {
        unsigned char tail[24]{};
        if (!ReadExact(reader, tail, sizeof(tail))) return result;
        for (unsigned char byte : tail) if (byte) return result;
    }
    if (fullLoadSucceeded) session.Commit();
    result.complete = fullLoadSucceeded;
    result.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return result;
}
void Equal(const Loaded& a, const Loaded& b) {
    Check(a.complete && b.complete, "both geometry loads complete");
    Check(a.vertexStart == b.vertexStart && a.vertexEnd == b.vertexEnd
              && a.indexEnd == b.indexEnd, "cached parser positions match");
    Check(a.indices == b.indices, "expanded indices match");
    Check(a.pmx.size() == b.pmx.size() && a.pmd.size() == b.pmd.size(), "vertex counts match");
    if (!a.pmx.empty())
        Check(std::memcmp(a.pmx.data(), b.pmx.data(), a.pmx.size() * sizeof(a.pmx[0])) == 0,
              "PMX fields and untouched bytes match");
    if (!a.pmd.empty())
        Check(std::memcmp(a.pmd.data(), b.pmd.data(), a.pmd.size() * sizeof(a.pmd[0])) == 0,
              "PMD fields and padding match");
}
FILETIME LastWrite(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    Check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != FALSE, "file time");
    return data.ftLastWriteTime;
}
void SetWrite(const std::filesystem::path& path, FILETIME time) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "open fixture time");
    Check(SetFileTime(handle, nullptr, nullptr, &time) != FALSE, "restore old mtime");
    CloseHandle(handle);
}
void Regressions(const std::filesystem::path& root) {
    Cache cache;
    auto file = root / L"model.pmx";
    auto bytes = Pmx(300);
    Write(file, bytes);
    auto cold = Load(file, cache, false, true, true);
    Check(cold.complete && cold.enabled && !cold.hit, "cold parsed geometry stored");
    auto warm = Load(file, cache, false, true, true);
    Check(warm.hit, "warm geometry hit");
    Equal(cold, warm);
    auto disabled = Load(file, cache, true, true, true);
    Check(!disabled.enabled && !disabled.hit, "explicit cache bypass");
    Equal(warm, disabled);
    warm.pmx[0].position[0] += 456.f;
    warm.indices[0] = 123456;
    cold.pmx.clear();
    cold.indices.clear();
    auto independent = Load(file, cache, false, true, true);
    Equal(independent, disabled);
    Check(independent.pmx.data() != disabled.pmx.data()
              && independent.indices.data() != disabled.indices.data(), "independent ownership");

    const auto oldTime = LastWrite(file);
    const auto geometryOffset = static_cast<std::size_t>(disabled.vertexStart);
    float changed = 91.25f;
    std::memcpy(bytes.data() + geometryOffset, &changed, 4);
    Write(file, bytes);
    SetWrite(file, oldTime);
    auto edited = Load(file, cache, false, true, true);
    Check(edited.complete && !edited.hit && edited.pmx[0].position[0] == changed,
          "same size content change with restored mtime invalidates");
    Check(Load(file, cache, false, true, true).hit, "edited source can be cached");

    auto replacement = root / L"replacement.pmx";
    changed = -27.5f;
    std::memcpy(bytes.data() + geometryOffset, &changed, 4);
    Write(replacement, bytes);
    SetWrite(replacement, oldTime);
    Check(MoveFileExW(replacement.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE,
          "atomically replace owned fixture");
    auto replaced = Load(file, cache, false, true, true);
    Check(replaced.complete && !replaced.hit && replaced.pmx[0].position[0] == changed,
          "same path replacement invalidates identity");

    std::filesystem::create_directories(root / L"second");
    auto sameName = root / L"second" / L"model.pmx";
    Write(sameName, Pmx(300, 0, 1, 1));
    auto otherPath = Load(sameName, cache, false, true, true);
    Check(!otherPath.hit, "same basename different path misses");
    Check(Load(sameName, cache, false, true, true).hit, "independent path hits");
    Check(Load(file, cache, false, true, true).hit, "first path remains valid");

    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "fixture writer");
    auto writerBypass = Load(file, cache, false, true, true);
    Check(writerBypass.complete && !writerBypass.enabled && !writerBypass.hit,
          "existing writer bypasses cache");
    CloseHandle(writer);

    auto bad = bytes;
    bad.resize(geometryOffset + 9);
    Write(file, bad);
    SetWrite(file, oldTime);
    auto truncated = Load(file, cache, false, true, true);
    Check(!truncated.complete && !truncated.hit, "truncation never hidden by old snapshot");
    Write(file, bytes);
    Check(Load(file, cache, false, true, true).complete, "valid replacement reloads");
    auto tailBad = bytes;
    tailBad.back() = 1;
    Write(file, tailBad);
    const auto stores = cache.GetStatistics().stores;
    Check(!Load(file, cache, false, true, true).complete, "invalid later section fails");
    Check(cache.GetStatistics().stores == stores, "later load failure does not publish snapshot");
    Write(file, bytes);
    Check(!Load(file, cache, false, false, true).complete, "cancelled full load");
    Check(!Load(file, cache, false, true, true).hit, "cancelled load did not publish");
    Check(DeleteFileW(file.c_str()) != FALSE, "delete owned model fixture");
    Check(!Load(file, cache, false, true, true).complete, "deleted source cannot hit");
    Write(file, bytes);
    Check(!Load(file, cache, false, true, true).hit, "new file after deletion misses");

    auto pmdFile = root / L"model.pmd";
    Write(pmdFile, Pmd(500));
    auto pmdCold = Load(pmdFile, cache, false, true, true);
    auto pmdWarm = Load(pmdFile, cache, false, true, true);
    Check(pmdCold.complete && !pmdCold.hit && pmdWarm.hit, "PMD decoded geometry reuse");
    Equal(pmdCold, pmdWarm);
    pmdWarm.pmd[0].position[0] = 197.f;
    pmdWarm.indices[0] = 9999;
    Equal(pmdCold, Load(pmdFile, cache, false, true, true));

    for (unsigned uv = 0; uv <= 4; ++uv) {
        for (unsigned indexSize : {1u, 2u, 4u}) {
            auto variant = root / L"variant.pmx";
            Write(variant, Pmx(31, uv, indexSize, indexSize));
            auto first = Load(variant, cache, false, true, true);
            auto second = Load(variant, cache, false, true, true);
            Check(!first.hit && second.hit, "PMX layout cache isolation");
            Equal(first, second);
        }
    }
    Cache bounded(48000, 40000, 2);
    for (int index = 0; index < 6; ++index) {
        auto path = root / (L"bounded-" + std::to_wstring(index) + L".pmd");
        Write(path, Pmd(400));
        Check(Load(path, bounded, false, true, true).complete, "bounded load");
        const auto stats = bounded.GetStatistics();
        Check(stats.bytes + stats.pendingBytes <= 48000 && stats.entries <= 2,
              "resident plus pending storage bounded");
    }
    Check(bounded.GetStatistics().evictions > 0, "LRU eviction exercised");
    auto oldest = root / L"bounded-0.pmd";
    Check(!Load(oldest, bounded, false, true, true).hit, "oldest unused geometry evicted");
    Cache tiny(512, 256, 1);
    Check(!Load(pmdFile, tiny, false, true, true).enabled
              && tiny.GetStatistics().entries == 0, "oversized model bypass");
    {
        Cache pendingBudget(48000, 40000, 2);
        const auto prepare = [&](const std::filesystem::path& path) {
            int descriptor = -1;
            Check(!_wsopen_s(&descriptor, path.c_str(), _O_RDONLY | _O_BINARY,
                            _SH_DENYNO, 0), "open pending snapshot source");
            auto value = Session::Begin(descriptor, pmdCold.layout,
                                         pmdCold.vertexStart, &pendingBudget);
            value.CapturePmd(pmdCold.pmd.data(),
                reinterpret_cast<const std::uint16_t*>(pmdCold.indices.data()),
                static_cast<std::uint32_t>(pmdCold.indices.size()),
                pmdCold.vertexEnd, pmdCold.indexEnd);
            _close(descriptor);
            return value;
        };
        auto firstPath = root / L"pending-a.pmd";
        auto secondPath = root / L"pending-b.pmd";
        auto thirdPath = root / L"pending-c.pmd";
        Write(firstPath, Pmd(500)); Write(secondPath, Pmd(500)); Write(thirdPath, Pmd(500));
        auto first = prepare(firstPath);
        auto second = prepare(secondPath);
        auto third = prepare(thirdPath);
        const auto stats = pendingBudget.GetStatistics();
        Check(stats.pendingBytes > 0 && stats.bytes + stats.pendingBytes <= 48000,
              "unpublished snapshots reserve within total budget");
        first.Commit(); second.Commit(); third.Commit();
        Check(pendingBudget.GetStatistics().bytes <= 48000
                  && pendingBudget.GetStatistics().pendingBytes == 0,
              "commit releases pending accounting");
    }
    {
        Cache leasedBudget(48000, 40000, 2);
        Check(Load(pmdFile, leasedBudget, false, true, true).complete, "lease seed");
        int descriptor = -1;
        Check(!_wsopen_s(&descriptor, pmdFile.c_str(), _O_RDONLY | _O_BINARY,
                        _SH_DENYNO, 0), "open leased snapshot source");
        auto lease = Session::Begin(descriptor, pmdCold.layout,
                                    pmdCold.vertexStart, &leasedBudget);
        Check(lease.Hit(), "lease acquired");
        leasedBudget.Clear();
        Check(leasedBudget.GetStatistics().bytes > 0, "clear retains leased allocation accounting");
        auto afterClear = Load(pmdFile, leasedBudget, false, true, true);
        Check(afterClear.complete && !afterClear.hit, "clear retires leased entry for future lookups");
        Check(leasedBudget.GetStatistics().bytes + leasedBudget.GetStatistics().pendingBytes <= 48000,
              "retired snapshot still counts toward allocation bound");
        lease.FinishGeometry();
        _close(descriptor);
        leasedBudget.Clear();
        Check(leasedBudget.GetStatistics().bytes == 0, "released lease can be cleared");
    }
    {
        Cache immutableSnapshot;
        auto source = root / L"immutable.pmx";
        Write(source, Pmx(31));
        auto instance = Load(source, immutableSnapshot, true, true, true);
        const auto original = instance;
        int descriptor = -1;
        Check(!_wsopen_s(&descriptor, source.c_str(), _O_RDONLY | _O_BINARY,
                        _SH_DENYNO, 0), "open immutable snapshot source");
        auto pending = Session::Begin(descriptor, instance.layout,
                                       instance.vertexStart, &immutableSnapshot);
        pending.CapturePmx(instance.pmx.data(), instance.indices.data(),
            static_cast<std::uint32_t>(instance.indices.size()),
            instance.vertexEnd, instance.indexEnd);
        _close(descriptor);
        instance.pmx[0].position[0] = 63.f;
        instance.pmx[0].edgeScale = 1.75f;
        instance.pmx[0].bone[0] = 197;
        instance.pmx[3].sdef.r0Offset[0] = 123.f;
        instance.pmx[0].materialIndex = 999;
        instance.indices[0] = 29;
        pending.Commit();
        auto restored = Load(source, immutableSnapshot, false, true, true);
        Check(restored.hit, "captured immutable geometry published");
        Equal(original, restored);
    }
    {
        Cache changedBeforeCommit;
        int descriptor = -1;
        Check(!_wsopen_s(&descriptor, pmdFile.c_str(), _O_RDONLY | _O_BINARY,
                         _SH_DENYNO, 0), "open commit validation source");
        auto pending = Session::Begin(descriptor, pmdCold.layout,
                                       pmdCold.vertexStart, &changedBeforeCommit);
        pending.CapturePmd(pmdCold.pmd.data(),
            reinterpret_cast<const std::uint16_t*>(pmdCold.indices.data()),
            static_cast<std::uint32_t>(pmdCold.indices.size()),
            pmdCold.vertexEnd, pmdCold.indexEnd);
        _close(descriptor);
        auto different = Pmd(500);
        different[static_cast<std::size_t>(pmdCold.vertexStart)] ^= 7;
        Write(pmdFile, different);
        pending.Commit();
        Check(changedBeforeCommit.GetStatistics().entries == 0
                  && changedBeforeCommit.GetStatistics().pendingBytes == 0,
              "source changed between capture and full-load success is not published");
    }
    Check(SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", L"1") != FALSE,
          "set disable control");
    Check(!Load(pmdFile, cache, false, true, true).enabled, "environment disables cache");
    SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", nullptr);
    std::printf("PASS model geometry cache checks=%zu hits=%llu stores=%llu invalidations=%llu\n",
        checks, cache.GetStatistics().hits, cache.GetStatistics().stores,
        cache.GetStatistics().invalidations);
}
void Benchmark(const std::vector<std::filesystem::path>& paths) {
    Cache cache;
    std::vector<Loaded> reference;
    for (const auto& path : paths) {
        auto baseline = Load(path, cache, true);
        Check(baseline.complete, "real geometry baseline");
        auto original = Load(path, cache, true, true, false, true);
        Equal(original, baseline);
        std::printf("REFERENCE item=%zu original_ms=%.3f current_uncached_ms=%.3f\n",
                    reference.size(), original.milliseconds, baseline.milliseconds);
        auto cold = Load(path, cache);
        Equal(baseline, cold);
        std::printf("COLD item=%zu vertices=%u indices=%zu hit=%d ms=%.3f\n",
                     reference.size(), cold.layout.vertexCount, cold.indices.size(),
                     cold.hit, cold.milliseconds);
        reference.push_back(std::move(baseline));
    }
    unsigned round = 0;
    for (char mode : std::string("ABBAABBA")) {
        ++round;
        double total = 0;
        unsigned hits = 0;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto result = Load(paths[i], cache, mode == 'A');
            Equal(reference[i], result);
            Check(result.hit == (mode == 'B'), "benchmark cache mode matches observed hit");
            total += result.milliseconds;
            hits += result.hit;
            std::printf("MODEL round=%u mode=%c item=%zu hit=%d ms=%.3f\n",
                         round, mode, i, result.hit, result.milliseconds);
        }
        std::printf("ROUND round=%u mode=%c hits=%u total_ms=%.3f\n",
                     round, mode, hits, total);
    }
    const auto stats = cache.GetStatistics();
    std::printf("CACHE entries=%zu bytes=%zu pending=%zu hits=%llu misses=%llu stores=%llu\n",
                 stats.entries, stats.bytes, stats.pendingBytes, stats.hits,
                 stats.misses, stats.stores);
}
}
int wmain(int argc, wchar_t** argv) {
    Check(argc >= 2, "provide owned validation output root");
    const auto root = std::filesystem::absolute(argv[1]) /
        (L"cpu-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    Regressions(root);
    std::vector<std::filesystem::path> paths;
    for (int i = 2; i < argc; ++i) paths.emplace_back(argv[i]);
    if (!paths.empty()) Benchmark(paths);
}
