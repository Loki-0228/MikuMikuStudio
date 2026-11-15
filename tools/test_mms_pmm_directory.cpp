// Linux check-format-inspect generates its fixture with the shipping codec
// and also observes every read request made by the CLI's exact scanner.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "mms_pmm_payload_probe.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace wire = mikumikustudio::pmm_container;
namespace probe = mikumikustudio::format_probe;

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #expr); std::exit(1); } \
} while (false)

using Window = std::pair<std::uint64_t, std::size_t>;

void CheckPayloadBudget(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    const auto size = lseek(fd, 0, SEEK_END);
    CHECK(size >= 0);
    std::vector<Window> metadataWindows;
    const auto directory = probe::InspectPmmDirectory(size, [&](auto offset, auto* dest, auto count) {
        metadataWindows.emplace_back(offset, count);
        return pread(fd, dest, count, offset) == static_cast<ssize_t>(count);
    });
    CHECK(directory.status == probe::DirectoryStatus::MetadataOk);
    std::size_t calls = 0;
    const auto payloads = probe::VerifyPmmPayloads(size, [&](auto offset, auto* dest, auto count) {
        CHECK(calls < metadataWindows.size());
        CHECK((metadataWindows[calls++] == Window{offset, count}));
        return pread(fd, dest, count, offset) == static_cast<ssize_t>(count);
    });
    CHECK(payloads.status == probe::PayloadStatus::BudgetExceeded);
    CHECK(calls == metadataWindows.size() && payloads.payloadBytesExamined == 0);
    CHECK(payloads.metadataBytesExamined == directory.bytesExamined);
    CHECK(close(fd) == 0);
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
            CHECK(pread(fd, dest, count, offset) == static_cast<ssize_t>(count));
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

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--check-payload-budget") {
        CheckPayloadBudget(argv[2]);
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--check-codec") {
        const int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        wire::Document loaded;
        std::uint64_t standard = 0;
        const auto result = wire::Read(fd, loaded, standard);
        CHECK(close(fd) == 0);
        return result == wire::ReadResult::Loaded ? 0 : 1;
    }
    CHECK(argc == 2);

    const int fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    constexpr char prefix[wire::kMinimumStandardSize] = "Polygon Movie maker 0003";
    CHECK(write(fd, prefix, sizeof prefix) == sizeof prefix);

    const wire::Block opaque{"opaque.\x85\x1b\"\\\n", 123, 8, static_cast<wire::Encoding>(777),
                             {0, 0xff, 0x01}};
    const wire::Block large{"extension.geometry", 7, 4, wire::Encoding::Binary,
                            std::vector<std::uint8_t>(2 * 1024 * 1024, '1')};
    CHECK(wire::Write(fd, wire::Document{{opaque, large}}));
    const auto size = lseek(fd, 0, SEEK_CUR);
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
            CHECK(pread(fd, dest, count, offset) == static_cast<ssize_t>(count));
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
    CHECK(pwrite(fd, &corrupt, 1, firstPayload) == 1);
    CHECK(wire::Read(fd, loaded, standard) == wire::ReadResult::Invalid);
    CHECK(pwrite(fd, &restored, 1, firstPayload) == 1);
    CHECK(close(fd) == 0);
}
