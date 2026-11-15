// Generate fixtures with the shipping codec and observe every read request
// made by the CLI's exact scanner.
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "mms_pmm_payload_probe.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace wire = mikumikustudio::pmm_container;
namespace probe = mikumikustudio::format_probe;

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #expr); std::exit(1); } \
} while (false)

#ifdef _WIN32
using PathChar = wchar_t;
constexpr auto kCheckCodec = L"--check-codec";
constexpr auto kCheckPayloadBudget = L"--check-payload-budget";
constexpr auto Close = &_close;
constexpr auto Seek = &_lseeki64;
constexpr auto Write = &_write;
int Open(const PathChar* path, bool create) {
    const auto mode = create ? _O_CREAT | _O_TRUNC | _O_RDWR : _O_RDONLY;
    return _wopen(path, mode | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
}
int ReadAt(int fd, void* dest, std::size_t count, std::uint64_t offset) {
    CHECK(count <= (std::numeric_limits<unsigned>::max)());
    CHECK(offset <= std::uint64_t{(std::numeric_limits<std::int64_t>::max)()});
    if (Seek(fd, static_cast<std::int64_t>(offset), SEEK_SET) < 0) return -1;
    return _read(fd, dest, static_cast<unsigned>(count));
}
int WriteAt(int fd, const void* source, std::size_t count, std::uint64_t offset) {
    CHECK(count <= (std::numeric_limits<unsigned>::max)());
    CHECK(offset <= std::uint64_t{(std::numeric_limits<std::int64_t>::max)()});
    if (Seek(fd, static_cast<std::int64_t>(offset), SEEK_SET) < 0) return -1;
    return _write(fd, source, static_cast<unsigned>(count));
}
#else
using PathChar = char;
constexpr auto kCheckCodec = "--check-codec";
constexpr auto kCheckPayloadBudget = "--check-payload-budget";
constexpr auto Close = &close;
constexpr auto Seek = &lseek;
constexpr auto Write = &write;
constexpr auto ReadAt = &pread;
constexpr auto WriteAt = &pwrite;
int Open(const PathChar* path, bool create) {
    return open(path, (create ? O_CREAT | O_TRUNC | O_RDWR : O_RDONLY) | O_CLOEXEC, 0600);
}
#endif

using Window = std::pair<std::uint64_t, std::size_t>;

void CheckPayloadBudget(const PathChar* path) {
    const int fd = Open(path, false);
    CHECK(fd >= 0);
    const auto size = Seek(fd, 0, SEEK_END);
    CHECK(size >= 0);
    std::vector<Window> metadataWindows;
    const auto directory = probe::InspectPmmDirectory(size, [&](auto offset, auto* dest, auto count) {
        metadataWindows.emplace_back(offset, count);
        return ReadAt(fd, dest, count, offset) == static_cast<std::int64_t>(count);
    });
    CHECK(directory.status == probe::DirectoryStatus::MetadataOk);
    std::size_t calls = 0;
    const auto payloads = probe::VerifyPmmPayloads(size, [&](auto offset, auto* dest, auto count) {
        CHECK(calls < metadataWindows.size());
        CHECK((metadataWindows[calls++] == Window{offset, count}));
        return ReadAt(fd, dest, count, offset) == static_cast<std::int64_t>(count);
    });
    CHECK(payloads.status == probe::PayloadStatus::BudgetExceeded);
    CHECK(calls == metadataWindows.size() && payloads.payloadBytesExamined == 0);
    CHECK(payloads.metadataBytesExamined == directory.bytesExamined);
    CHECK(Close(fd) == 0);
}

void CheckPayloadReads(int fd, std::uint64_t size, const std::array<Window, 8>& metadataWindows,
                       std::uint64_t firstPayload, std::size_t firstSize,
                       std::uint64_t secondPayload, std::size_t secondSize) {
    std::vector<Window> windows(metadataWindows.begin(), metadataWindows.end());
    // Assert the scanner reads metadata before bounded, nonoverlapping
    // chunks of raw data, with a separate CRC for each block.
    constexpr std::size_t chunk = 64 * 1024;
    CHECK(firstSize % chunk == 0 && secondSize < chunk);
    for (std::size_t pos = 0; pos < firstSize; pos += chunk)
        windows.emplace_back(firstPayload + pos, chunk);
    windows.emplace_back(secondPayload, secondSize);

    for (std::size_t fail = 0; fail <= windows.size(); ++fail) {
        std::size_t calls = 0, metadataRead = 0, payloadRead = 0;
        const auto out = probe::VerifyPmmPayloads(size, [&](auto offset, auto* dest, auto count) {
            CHECK(calls < windows.size());
            CHECK((windows[calls] == Window{offset, count}));
            const bool metadata = calls < metadataWindows.size();
            if (calls++ == fail) return false;
            CHECK(ReadAt(fd, dest, count, offset) == static_cast<std::int64_t>(count));
            (metadata ? metadataRead : payloadRead) += count;
            return true;
        });
        CHECK(out.metadataBytesExamined == metadataRead && out.payloadBytesExamined == payloadRead);
        if (fail < windows.size()) {
            CHECK(calls == fail + 1 && out.status == probe::PayloadStatus::IoError);
        } else {
            CHECK(calls == windows.size() && out.status == probe::PayloadStatus::ChecksumsOk);
            CHECK(payloadRead == firstSize + secondSize);
        }
    }
}

#ifdef _WIN32
int wmain(int argc, PathChar** argv) {
#else
int main(int argc, PathChar** argv) {
#endif
    if (argc == 3 && std::basic_string_view<PathChar>(argv[1]) == kCheckPayloadBudget) {
        CheckPayloadBudget(argv[2]);
        return 0;
    }
    if (argc == 3 && std::basic_string_view<PathChar>(argv[1]) == kCheckCodec) {
        const int fd = Open(argv[2], false);
        CHECK(fd >= 0);
        wire::Document loaded;
        std::uint64_t standard = 0;
        const auto result = wire::Read(fd, loaded, standard);
        CHECK(Close(fd) == 0);
        return result == wire::ReadResult::Loaded ? 0 : 1;
    }
    CHECK(argc == 2);

    const int fd = Open(argv[1], true);
    CHECK(fd >= 0);
    constexpr char prefix[wire::kMinimumStandardSize] = "Polygon Movie maker 0003";
    CHECK(Write(fd, prefix, sizeof prefix) == sizeof prefix);

    const wire::Block opaque{"opaque.\x85\x1b\"\\\n", 123, 8, static_cast<wire::Encoding>(777),
                             {0, 0xff, 0x01}};
    const wire::Block large{"extension.geometry", 7, 4, wire::Encoding::Binary,
                            std::vector<std::uint8_t>(2 * 1024 * 1024, '1')};
    CHECK(wire::Write(fd, wire::Document{{opaque, large}}));
    const auto size = Seek(fd, 0, SEEK_CUR);
    CHECK(size > 0);

    const std::uint64_t first = sizeof prefix;
    const auto second = first + wire::kBlockHeaderSize + large.key.size() + large.payload.size();
    const auto toc = second + wire::kBlockHeaderSize + opaque.key.size() + opaque.payload.size();
    const auto tocRows = 2 * wire::kTocEntrySize + large.key.size() + opaque.key.size();
    const auto footer = toc + wire::kTocHeaderSize + tocRows;
    CHECK(footer + wire::kFooterSize == static_cast<std::uint64_t>(size));
    const std::array<Window, 8> windows{{
        {0, sizeof prefix}, {footer, wire::kFooterSize}, {toc, wire::kTocHeaderSize},
        {toc + wire::kTocHeaderSize, tocRows}, {first, wire::kBlockHeaderSize},
        {first + wire::kBlockHeaderSize, large.key.size()}, {second, wire::kBlockHeaderSize},
        {second + wire::kBlockHeaderSize, opaque.key.size()},
    }};
    // Fail each individual window in turn, including after one block was
    // accepted, then verify the successful case with all eight windows.
    for (std::size_t fail = 0; fail <= windows.size(); ++fail) {
        std::size_t calls = 0, read = 0;
        const auto out = probe::InspectPmmDirectory(size, [&](auto offset, auto* dest, auto count) {
            CHECK(calls < windows.size());
            CHECK((windows[calls] == std::make_pair(offset, count)));
            if (calls++ == fail) return false;
            CHECK(ReadAt(fd, dest, count, offset) == static_cast<std::int64_t>(count));
            read += count;
            return true;
        });
        CHECK(out.bytesExamined == read && read < probe::kMaxDirectoryReadBytes);
        if (fail < windows.size()) {
            CHECK(calls == fail + 1 && out.status == probe::DirectoryStatus::IoError);
            CHECK(out.blocks.empty());
        } else {
            CHECK(calls == windows.size() && out.status == probe::DirectoryStatus::MetadataOk);
            CHECK(out.blocks.size() == 2 && out.blocks[0].key == large.key &&
                  out.blocks[1].key == opaque.key);
        }
    }

    wire::Document loaded;
    std::uint64_t standard = 0;
    CHECK(wire::Read(fd, loaded, standard) == wire::ReadResult::Loaded);
    CHECK(standard == sizeof prefix && loaded.blocks.size() == 2);
    const char corrupt = '2', restored = '1';
    const auto firstPayload = first + wire::kBlockHeaderSize + large.key.size();
    const auto secondPayload = second + wire::kBlockHeaderSize + opaque.key.size();
    CheckPayloadReads(fd, size, windows, firstPayload, large.payload.size(),
                      secondPayload, opaque.payload.size());
    CHECK(WriteAt(fd, &corrupt, 1, firstPayload) == 1);
    CHECK(wire::Read(fd, loaded, standard) == wire::ReadResult::Invalid);
    CHECK(WriteAt(fd, &restored, 1, firstPayload) == 1);
    CHECK(Close(fd) == 0);
}
