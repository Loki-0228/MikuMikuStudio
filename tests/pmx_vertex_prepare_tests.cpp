#include "../src/model/pmx_vertex_parallel.hpp"
#include "mikumikustudio/sequential_file_reader.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>

using namespace mikumikustudio;
namespace {
void Check(bool success, const char* message) {
    if (!success) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}
using Bytes = std::vector<unsigned char>;
template<class T> void Put(Bytes& bytes, T value) {
    const auto* start = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), start, start + sizeof(value));
}
struct Reader {
    const Bytes& bytes;
    std::size_t position = 0;
    std::thread::id owner = std::this_thread::get_id();
    int Read(void* output, unsigned count) {
        Check(std::this_thread::get_id() == owner, "only calling thread reads source");
        const auto size = std::min<std::size_t>(count, bytes.size() - position);
        std::memcpy(output, bytes.data() + position, size);
        position += size;
        return static_cast<int>(size);
    }
};
template<class Input>
void OriginalVertex(Input& reader, unsigned additionalUv, unsigned boneIdxSize,
                    mdl::PmxVertex& vertex) {
        auto readIdx = [&reader](int size) -> std::int32_t {
            switch (size) {
            case 1: { std::int8_t v = 0; reader.Read(&v, 1); return v; }
            case 2: { std::uint16_t v = 0; reader.Read(&v, 2);
                      return static_cast<std::int16_t>(v); }
            default: { std::int32_t v = 0; reader.Read(&v, 4); return v; }
            }
        };

        {
            mdl::PmxVertex& rec = vertex;
            reader.Read(&rec, 0x20);                          // pos+normal+uv
            const int addl = additionalUv;
            // Deliberate hardening: the original takes this loop bound straight
            // from the header's additional-UV byte and only tests it for > 0
            // (verified at 0x1400AA33E, re-read on every iteration at
            // 0x1400AA3D2), never against the four UV entries of
            // additionalUvByComponent, so a count above 4 overwrites weightType
            // and the rest of the working vertex. The port stores at most four
            // UVs and consumes the surplus floats without storing them, so the
            // stream position is unchanged.
            // Defect MB-01.
            constexpr int kPmxAdditionalUvSlots =
                sizeof(rec.additionalUvByComponent)
                / sizeof(rec.additionalUvByComponent[0]);
            for (int j = 0; j < addl; ++j) {                // SOA addl UVs
                for (int c = 0; c < 4; ++c) {
                    float surplus = 0.0f;
                    float* destination = j < kPmxAdditionalUvSlots
                                             ? &rec.additionalUvByComponent[c][j]
                                             : &surplus;
                    reader.Read(destination, 4);
                }
            }
            reader.Read(&rec.weightType, 1);
            switch (rec.weightType) {
            case mdl::PmxWeightType::bdef2:
                rec.bone[0] = readIdx(boneIdxSize);
                rec.bone[1] = readIdx(boneIdxSize);
                reader.Read(rec.weight, 4);
                break;
            case mdl::PmxWeightType::bdef4:
                for (int b = 0; b < 4; ++b)
                    rec.bone[b] = readIdx(boneIdxSize);
                reader.Read(rec.weight, 0x10);
                break;
            case mdl::PmxWeightType::sdef: {
                rec.bone[0] = readIdx(boneIdxSize);
                rec.bone[1] = readIdx(boneIdxSize);
                reader.Read(rec.weight, 4);                   // weight
                reader.Read(rec.sdef.center, 0xC);            // PMX C
                reader.Read(rec.sdef.r0Offset, 0xC);          // PMX R0
                reader.Read(rec.sdef.r1Offset, 0xC);          // PMX R1
                const float w = rec.weight[0];
                float mid[3];
                for (int c = 0; c < 3; ++c)
                    mid[c] = w * rec.sdef.r0Offset[c]
                           + (1.0f - w) * rec.sdef.r1Offset[c];
                for (int c = 0; c < 3; ++c) {
                    rec.sdef.r0Offset[c] -= mid[c];
                    rec.sdef.r1Offset[c] -= mid[c];
                }
                break;
            }
            default:                                        // BDEF1 (and original fallback)
                rec.bone[0] = readIdx(boneIdxSize);
                break;
            }
            reader.Read(&rec.edgeScale, 4);
            rec.hasPmxEdgeData = 1;
        }

}
Bytes Fixture(std::size_t count, unsigned uv, unsigned indexSize) {
    Bytes bytes;
    for (std::size_t vertex = 0; vertex < count; ++vertex) {
        for (unsigned item = 0; item < 8 + 4 * uv; ++item)
            Put(bytes, static_cast<float>((vertex % 137) * 0.017 + item));
        const unsigned char type = static_cast<unsigned char>(vertex % 6);
        Put(bytes, type);
        const auto index = [&]() {
            const int value = static_cast<int>(vertex % 251) - 125;
            if (indexSize == 1) Put(bytes, static_cast<std::int8_t>(value));
            else if (indexSize == 2) Put(bytes, static_cast<std::int16_t>(value));
            else Put(bytes, value);
        };
        index();
        if (type == 1 || type == 3) {
            index();
            Put(bytes, static_cast<float>(vertex % 10) * 0.1f);
            if (type == 3)
                for (int item = 0; item < 9; ++item)
                    Put(bytes, static_cast<float>(item * 3 - 7));
        } else if (type == 2) {
            index(); index(); index();
            for (int item = 0; item < 4; ++item)
                Put(bytes, 0.25f);
        }
        Put(bytes, 0.75f);
    }
    return bytes;
}
void Equivalent(const Bytes& bytes, std::size_t count, unsigned uv,
                unsigned indexSize, bool benchmark = false) {
    std::vector<mdl::PmxVertex> expected(count), actual(count);
    std::memset(expected.data(), 0xcd, expected.size() * sizeof(expected[0]));
    Reader reference{bytes};
    const auto legacyStart = std::chrono::steady_clock::now();
    for (auto& vertex : expected)
        OriginalVertex(reference, uv, indexSize, vertex);
    const double legacyMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - legacyStart).count();
    if (benchmark) std::printf(" legacy_ms=%.3f", legacyMs);
    Check(reference.position == bytes.size(), "reference consumed vertex section");
    std::memset(actual.data(), 0xcd, actual.size() * sizeof(actual[0]));
    Reader serialInput{bytes};
    for (auto& vertex : actual)
        Check(pmx_prepare::ReadPackedVertex(
                  serialInput, static_cast<std::uint8_t>(uv),
                  static_cast<std::uint8_t>(indexSize), vertex),
              "stack record completed");
    Check(serialInput.position == reference.position, "stack stream position");
    Check(std::memcmp(actual.data(), expected.data(),
                       actual.size() * sizeof(actual[0])) == 0,
          "stack records byte-identical to original parser");
    for (unsigned workers : {0u, 1u, 2u}) {
        std::memset(actual.data(), 0xcd, actual.size() * sizeof(actual[0]));
        Reader input{bytes};
        pmx_prepare::Statistics stats;
        const auto start = std::chrono::steady_clock::now();
        const auto result = pmx_prepare::ReadVertices(
            input, count, static_cast<std::uint8_t>(uv),
            static_cast<std::uint8_t>(indexSize), actual.data(), workers, &stats);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        Check(result == pmx_prepare::Result::complete, "prepared parsing completed");
        Check(input.position == reference.position, "stream position unchanged");
        Check(std::memcmp(actual.data(), expected.data(),
                           actual.size() * sizeof(actual[0])) == 0,
              "prepared records byte-identical including untouched fields");
        Check(stats.temporaryBytes <= 4 * 1024 * 1024, "temporary bytes bounded");
        if (benchmark)
            std::printf(" workers=%u actual_workers=%zu batches=%zu ms=%.3f",
                        workers, stats.workers, stats.batches, ms);
    }
    if (benchmark) std::puts("");
}
void Regressions() {
    const unsigned originalControl=_mm_getcsr();
    for(unsigned mode:{0x0000u,0x2000u,0x4000u,0x6000u,0x8040u}) {
        _mm_setcsr((originalControl&~0xe040u)|mode);
        constexpr auto count=pmx_prepare::kMinimumParallelVertices+1;
        Equivalent(Fixture(count,4,4),count,4,4);
    }
    _mm_setcsr(originalControl);
    for (unsigned uv : {0u, 1u, 2u, 3u, 4u})
        for (unsigned indexSize : {1u, 2u, 4u, 0u, 3u}) {
            constexpr auto count=pmx_prepare::kMinimumParallelVertices+1;
            const auto bytes = Fixture(count, uv, indexSize);
            Equivalent(bytes, count, uv, indexSize);
        }
    for (std::size_t count : {0u, 1u, 8191u, 8192u, 8193u})
        Equivalent(Fixture(count, 4, 4), count, 4, 4);
    const auto valid = Fixture(7, 4, 4);
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const Bytes bytes(valid.begin(), valid.begin() + length);
        Reader reader{bytes};
        std::array<mdl::PmxVertex, 7> output{};
        Check(pmx_prepare::ReadVertices(reader, 7, 4, 4, output.data(), 2)
                  == pmx_prepare::Result::truncated, "truncated record rejected");
        Reader serial{bytes};
        std::memset(output.data(), 0xcd, sizeof(output));
        std::size_t completed = 0;
        while (completed < output.size()
               && pmx_prepare::ReadPackedVertex(serial, 4, 4, output[completed]))
            ++completed;
        Check(completed < output.size(), "stack path rejects every truncation");
        const auto* untouchedTail =
            reinterpret_cast<const unsigned char*>(&output[completed]);
        for (std::size_t byte = 0; byte < sizeof(output) - completed * sizeof(output[0]); ++byte)
            Check(untouchedTail[byte] == 0xcd, "failed stack record not committed");
    }
    Reader untouched{valid};
    std::array<mdl::PmxVertex, 7> output{};
    Check(pmx_prepare::ReadVertices(untouched, 7, 5, 4, output.data(), 2)
              == pmx_prepare::Result::unavailable && untouched.position == 0,
          "unsupported layout leaves stream untouched");
    Check(!pmx_prepare::ReadPackedVertex(untouched, 5, 4, output[0])
              && untouched.position == 0,
          "unsupported stack layout leaves stream untouched");
    struct FailingReader {
        const Bytes& bytes;
        int failOnCall;
        int calls = 0;
        std::size_t position = 0;
        int Read(void* target, unsigned count) {
            if (++calls == failOnCall) return -1;
            std::memcpy(target, bytes.data() + position, count);
            position += count;
            return static_cast<int>(count);
        }
    };
    for (int failOnCall : {1, 2}) {
        FailingReader failing{valid, failOnCall};
        mdl::PmxVertex vertex;
        std::memset(&vertex, 0xcd, sizeof(vertex));
        const auto before = vertex;
        Check(!pmx_prepare::ReadPackedVertex(failing, 4, 4, vertex)
                  && std::memcmp(&vertex, &before, sizeof(vertex)) == 0,
              "I/O failure leaves destination uncommitted");
    }
    std::atomic<bool> cancelled{true};
    Check(pmx_prepare::ReadVertices(untouched, 7, 4, 4, output.data(), 2,
                                    nullptr, &cancelled)
              == pmx_prepare::Result::cancelled && untouched.position == 0,
          "cancelled task does not consume input");
    constexpr auto largeCount=pmx_prepare::kMinimumParallelVertices+1;
    auto large=Fixture(largeCount,4,4);large.pop_back();
    Reader shortParallel{large};
    std::vector<mdl::PmxVertex> largeOutput(largeCount);
    Check(pmx_prepare::ReadVertices(shortParallel,largeCount,4,4,largeOutput.data(),2)
        == pmx_prepare::Result::truncated,"truncation joins outstanding worker batches");
    std::puts("PASS vertex expansion, mixed weights, signed indices, UV transpose, "
              "SDEF, boundaries, truncation, cancellation and bounded memory");
}
void BenchmarkFile(const wchar_t* path, std::size_t start, std::size_t end,
                   unsigned count, unsigned uv, unsigned indexSize) {
    std::vector<mdl::PmxVertex> expected(count), actual(count);
    std::memset(expected.data(), 0xcd, expected.size() * sizeof(expected[0]));
    {
        int fd = _wopen(path, _O_RDONLY | _O_BINARY);
        Check(fd >= 0 && _lseeki64(fd, start, SEEK_SET) == start, "open original stream");
        fileio::SequentialFileReader input(fd);
        for (auto& vertex : expected)
            OriginalVertex(input, uv, indexSize, vertex);
        Check(input.Tell() == end, "original stream consumed exact vertex section");
    }
    // A: legacy scalar reads; B: identical packed batches on the caller;
    // C: packed batches with up to two CPU workers. Each pair has both orders.
    for (char mode : std::string("ABBAACCABCCBADDA")) {
        std::memset(actual.data(), 0xcd, actual.size() * sizeof(actual[0]));
        int fd = _wopen(path, _O_RDONLY | _O_BINARY);
        Check(fd >= 0 && _lseeki64(fd, start, SEEK_SET) == start, "open benchmark stream");
        fileio::SequentialFileReader input(fd);
        const auto clock = std::chrono::steady_clock::now();
        if (mode == 'A' || (mode != 'D' && count < pmx_prepare::kMinimumParallelVertices)) {
            for (auto& vertex : actual)
                OriginalVertex(input, uv, indexSize, vertex);
        } else if (mode == 'D') {
            for (auto& vertex : actual)
                Check(pmx_prepare::ReadPackedVertex(input,
                    static_cast<std::uint8_t>(uv),
                    static_cast<std::uint8_t>(indexSize), vertex),
                    "stack record completed");
        } else {
            Check(pmx_prepare::ReadVertices(input, count,
                       static_cast<std::uint8_t>(uv),
                       static_cast<std::uint8_t>(indexSize), actual.data(),
                       mode == 'B' ? 0 : 2)
                      == pmx_prepare::Result::complete, "prepared stream completed");
        }
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - clock).count();
        Check(input.Tell() == end, "prepared stream consumed exact vertex section");
        Check(std::memcmp(actual.data(), expected.data(),
                           actual.size() * sizeof(actual[0])) == 0,
              "real model matches original parser byte for byte");
        std::printf(" file_mode=%c ms=%.3f\n", mode, milliseconds);
    }
    for(char mode:std::string("ABBAABBA")) {
        std::memset(actual.data(),0xcd,actual.size()*sizeof(actual[0]));
        const int fd=_wopen(path,_O_RDONLY|_O_BINARY);
        Check(fd>=0 && _lseeki64(fd,start,SEEK_SET)==start,"open production comparison stream");
        fileio::SequentialFileReader input(fd);
        const auto begin=std::chrono::steady_clock::now();
        if(mode=='B' && count>=pmx_prepare::kMinimumParallelVertices)
            Check(pmx_prepare::ReadVertices(input,count,uv,indexSize,actual.data(),2)==pmx_prepare::Result::complete,
                "production parallel comparison");
        else for(auto& vertex:actual)Check(pmx_prepare::ReadPackedVertex(input,uv,indexSize,vertex),"production serial comparison");
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        Check(input.Tell()==end && !std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(actual[0])),
            "production comparison exact original bytes");
        std::printf("PRODUCTION_ABBA mode=%c ms=%.6f\n",mode,elapsed);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 1) {
        Regressions();
        return 0;
    }
    for (int arg = 1; arg < argc; ++arg) {
        std::ifstream file(std::filesystem::path(argv[arg]), std::ios::binary);
        Bytes bytes((std::istreambuf_iterator<char>(file)), {});
        Check(bytes.size() > 17 && std::memcmp(bytes.data(), "PMX ", 4) == 0,
              "PMX input");
        Reader input{bytes, 17};
        for (int i = 0; i < 4; ++i) {
            std::uint32_t count = 0;
            Check(input.Read(&count, 4) == 4 && count <= bytes.size() - input.position,
                  "PMX text");
            input.position += count;
        }
        std::uint32_t count = 0;
        Check(input.Read(&count, 4) == 4, "PMX vertex count");
        const std::size_t begin = input.position;
        mdl::PmxVertex temporary{};
        for (std::uint32_t i = 0; i < count; ++i)
            pmx_prepare::DecodeVertex(input, bytes[10], bytes[14], temporary);
        Bytes section(bytes.begin() + begin, bytes.begin() + input.position);
        std::printf("vertices=%u bytes=%zu uv=%u index=%u\n",
                    count, section.size(), bytes[10], bytes[14]);
        Equivalent(section, count, bytes[10], bytes[14]);
        BenchmarkFile(argv[arg], begin, input.position, count, bytes[10], bytes[14]);
    }
}
