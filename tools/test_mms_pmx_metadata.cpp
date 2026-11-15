#include "mms_pmx_metadata_probe.hpp"

#include <cassert>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace mikumikustudio::format_probe;

struct Request {
    std::uint64_t offset;
    std::size_t count;
    bool operator==(const Request& other) const {
        return offset == other.offset && count == other.count;
    }
};
}  // namespace

int main() {
    constexpr char prefix[] =
        "PMX \x00\x00\x00\x40\x08\x01\x00\x02\x01\x01\x02\x02\x02"
        "\x04\0\0\0" "Test" "\0\0\0\0" "\0\0\0\0" "\0\0\0\0";
    constexpr auto metadataSize = sizeof(prefix) - 1;
    const std::vector<Request> expected{{0, 17}, {17, 4}, {21, 4}, {25, 4}, {29, 4}, {33, 4}};
    std::vector<Request> requests;
    std::size_t failAt = expected.size();
    const auto read = [&](std::uint64_t offset, char* bytes, std::size_t count) {
        requests.push_back({offset, count});
        assert(offset <= metadataSize && count <= metadataSize - offset);
        if (requests.size() - 1 == failAt) return false;
        std::memcpy(bytes, prefix + offset, count);
        return true;
    };

    // A huge claimed size may not expand the read range even by one byte.
    constexpr auto largeSize = (std::numeric_limits<std::uint64_t>::max)();
    auto result = InspectPmxMetadata(largeSize, read);
    assert(result.status == PmxMetadataStatus::MetadataOk);
    assert(result.bytesExamined == metadataSize && requests == expected);
    assert(result.fields[0] == U"Test" && result.fields[1].empty());

    for (failAt = 0; failAt < expected.size(); ++failAt) {
        requests.clear();
        result = InspectPmxMetadata(largeSize, read);
        assert(result.status == PmxMetadataStatus::IoError);
        assert(result.header.status == (failAt == 0 ? Status::IoError : Status::Recognized));
        assert(result.bytesExamined == expected[failAt].offset);
        assert(requests.size() == failAt + 1 &&
               std::equal(requests.begin(), requests.end(), expected.begin()));
        for (const auto& field : result.fields) assert(field.empty());
    }

    requests.clear();
    result = InspectPmxMetadata(22, read);  // Full length, but only one of four indicated bytes.
    assert(result.status == PmxMetadataStatus::Truncated && result.bytesExamined == 21);
    assert((requests == std::vector<Request>{{0, 17}, {17, 4}}));

    requests.clear();
    result = InspectPmxMetadata(largeSize, [&](auto offset, auto* bytes, auto count) {
        if (!read(offset, bytes, count)) return false;
        if (offset == 17) {
            assert(count == 4);
            const char overBudget[] = "\x01\x00\x01\x00";  // 64 KiB + 1.
            std::memcpy(bytes, overBudget, count);
        }
        return true;
    });
    assert(result.status == PmxMetadataStatus::BudgetExceeded && result.bytesExamined == 21);
    assert((requests == std::vector<Request>{{0, 17}, {17, 4}}));
}
