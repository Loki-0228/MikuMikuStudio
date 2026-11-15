// Linux check-format-inspect generates its fixture with the shipping codec
// and also observes every read request made by the CLI's exact scanner.
#include "mms_pmm_directory_probe.hpp"

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

int main(int argc, char** argv) {
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
    const std::array<std::pair<std::uint64_t, std::size_t>, 8> windows{{
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
    CHECK(pwrite(fd, &corrupt, 1, firstPayload) == 1);
    CHECK(wire::Read(fd, loaded, standard) == wire::ReadResult::Invalid);
    CHECK(pwrite(fd, &restored, 1, firstPayload) == 1);
    CHECK(close(fd) == 0);
}
