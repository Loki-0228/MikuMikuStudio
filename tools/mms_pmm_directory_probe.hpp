#pragma once

#include "mms_format_probe.hpp"
#include "mikumikustudio/pmm_container.hpp"

#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace mikumikustudio::format_probe {

enum class DirectoryStatus { Absent, MetadataOk, Invalid, OverlappingRanges, BudgetExceeded, IoError };

struct DirectoryBlock {
    std::string key;
    std::uint32_t version, flags, encoding;
    std::uint64_t payloadSize;
};

struct DirectoryResult {
    Result header{};
    DirectoryStatus status = DirectoryStatus::Absent;
    std::size_t bytesExamined = 0;
    std::vector<DirectoryBlock> blocks{};
};

inline constexpr std::size_t kMaxDirectoryTocBytes = 256 * 1024;
inline constexpr std::uint32_t kMaxDirectoryRows = 2048;
inline constexpr std::size_t kMaxDirectoryReadBytes = 1024 * 1024;

namespace directory_detail {

// Wire offsets follow docs/reference/pmm-0003-container.md. Sizes and versions
// come from the codec; Linux check-format-inspect also tests its real writer.
inline std::uint32_t U32(const char* bytes) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes);
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) |
        (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

inline std::uint64_t U64(const char* p) {
    return std::uint64_t{U32(p)} | (std::uint64_t{U32(p + 4)} << 32);
}

// Reflected IEEE CRC32, identical to src/io/pmm_container.cpp. Retain
// the uncomplemented state between chunks; an empty sequence yields zero.
inline constexpr auto kCrc32Table = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        auto crc = i;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        table[i] = crc;
    }
    return table;
}();
inline constexpr std::uint32_t kCrc32Xor = 0xFFFFFFFFu;

inline std::uint32_t UpdateCrc32(std::uint32_t crc, const char* bytes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        crc = (crc >> 8) ^ kCrc32Table[(crc ^ static_cast<unsigned char>(bytes[i])) & 0xFFu];
    return crc;
}

struct Entry {
    std::size_t tocPosition;
    std::string_view key;
    std::uint64_t offset, end;
};

struct PayloadRange {
    std::uint64_t offset, size;
    std::uint32_t crc;
};

// Read the wrapper once, then the TOC and only the declared block headers and
// keys. Check every range before reading any block header so even overlapping
// metadata cannot cause a read from another declared block's payload.
template<class ReadAt>
DirectoryResult InspectPmmMetadata(std::uint64_t fileSize, ReadAt& readAt,
                                   std::vector<PayloadRange>* payloads = nullptr) {
    namespace wire = mikumikustudio::pmm_container;
    static_assert(kFooterSize == wire::kFooterSize);

    DirectoryResult out;
    std::size_t requested = 0;
    const auto read = [&](std::uint64_t offset, char* data, std::size_t count) {
        if (count > kMaxDirectoryReadBytes - requested) {
            out.status = DirectoryStatus::BudgetExceeded;
            return false;
        }
        requested += count;
        if (!readAt(offset, data, count)) {
            out.status = DirectoryStatus::IoError;
            return false;
        }
        out.bytesExamined += count;
        return true;
    };

    FooterBytes footer{};
    out.header = Inspect(fileSize, read, &footer);
    if (out.header.pmm3end != Footer::MarkerOnly) return out;

    out.status = DirectoryStatus::Invalid;
    const auto* end = footer.data();
    if (U32(end + 8) != wire::kContainerVersion || U32(end + 12) != 0 ||
        U64(end + 40) != fileSize || U32(end + 52) != 0 || U64(end + 56) != 0)
        return out;
    const auto standardSize = U64(end + 16);
    const auto tocOffset = U64(end + 24);
    const auto tocSize = U64(end + 32);
    const auto footerOffset = fileSize - kFooterSize;
    if (standardSize < wire::kMinimumStandardSize || standardSize > tocOffset ||
        tocOffset > footerOffset || tocSize != footerOffset - tocOffset ||
        tocSize < wire::kTocHeaderSize)
        return out;
    if (tocSize > kMaxDirectoryTocBytes) {
        out.status = DirectoryStatus::BudgetExceeded;
        return out;
    }

    std::array<char, wire::kTocHeaderSize> tocHeader{};
    if (!read(tocOffset, tocHeader.data(), tocHeader.size())) return out;
    const auto* head = tocHeader.data();
    if (!StartsWith({head, tocHeader.size()}, std::string_view{"PMM3TOC\0", 8}) ||
        U32(head + 8) != wire::kTocVersion || U32(head + 12) != 0 || U32(head + 20) != 0)
        return out;
    const auto count = U32(head + 16);
    const auto rowsSize = tocSize - wire::kTocHeaderSize;
    if (rowsSize < std::uint64_t{count} * (wire::kTocEntrySize + 1) ||
        rowsSize > std::uint64_t{count} * (wire::kTocEntrySize + wire::kMaxKeyBytes))
        return out;
    if (count > kMaxDirectoryRows) {
        out.status = DirectoryStatus::BudgetExceeded;
        return out;
    }

    std::vector<char> toc(static_cast<std::size_t>(tocSize));
    std::copy(tocHeader.begin(), tocHeader.end(), toc.begin());
    if (rowsSize && !read(tocOffset + tocHeader.size(), toc.data() + tocHeader.size(),
                         static_cast<std::size_t>(rowsSize)))
        return out;
    const auto tocCrc = UpdateCrc32(kCrc32Xor, toc.data(), toc.size()) ^ kCrc32Xor;
    if (tocCrc != U32(end + 48)) return out;

    std::vector<Entry> entries;
    entries.reserve(count);
    std::set<std::string_view> keys;
    std::size_t pos = wire::kTocHeaderSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (toc.size() - pos < wire::kTocEntrySize) return out;
        const auto* row = toc.data() + pos;
        const auto keySize = U32(row);
        if (!keySize || keySize > wire::kMaxKeyBytes ||
            keySize > toc.size() - pos - wire::kTocEntrySize || U32(row + 36) != 0)
            return out;
        const std::string_view key(row + wire::kTocEntrySize, keySize);
        if (key.find('\0') != std::string_view::npos || !keys.insert(key).second) return out;

        const auto offset = U64(row + 16);
        const auto headSize = wire::kBlockHeaderSize + keySize;
        const auto payloadSize = U64(row + 24);
        if (offset < standardSize || offset > tocOffset || headSize > tocOffset - offset ||
            payloadSize > tocOffset - offset - headSize)
            return out;
        entries.push_back({pos, key, offset, offset + headSize + payloadSize});
        pos += wire::kTocEntrySize + keySize;
    }
    if (pos != toc.size()) return out;

    // Native Read admits gaps and a different TOC order. Allow both, but
    // decline to inspect intersecting ranges: a supposed header could be
    // inside another block's declared payload. This is not a native verdict.
    std::vector<const Entry*> byOffset;
    byOffset.reserve(entries.size());
    for (const auto& entry : entries) byOffset.push_back(&entry);
    std::sort(byOffset.begin(), byOffset.end(),
              [](auto a, auto b) { return a->offset < b->offset; });
    auto previousEnd = standardSize;
    for (const auto* entry : byOffset) {
        if (entry->offset < previousEnd) {
            out.status = DirectoryStatus::OverlappingRanges;
            return out;
        }
        previousEnd = entry->end;
    }

    std::array<char, wire::kBlockHeaderSize> blockHead{};
    std::array<char, wire::kMaxKeyBytes> blockKey{};
    for (const auto* entry : byOffset) {
        if (!read(entry->offset, blockHead.data(), blockHead.size())) return out;
        const auto* row = toc.data() + entry->tocPosition;
        // key size, version, flags, encoding; then payload length and stored
        // CRC. The TOC alone carries the intervening block offset.
        if (std::memcmp(blockHead.data(), row, 16) != 0 ||
            std::memcmp(blockHead.data() + 16, row + 24, 12) != 0 ||
            U32(blockHead.data() + 28) != 0)
            return out;
        // Verify the header before using the key length, so a lying TOC
        // cannot extend the key read into a shorter key's payload.
        if (!read(entry->offset + blockHead.size(), blockKey.data(), entry->key.size()))
            return out;
        if (std::string_view(blockKey.data(), entry->key.size()) != entry->key) return out;
    }

    // Only expose ranges once all directory rows, block headers and keys
    // have agreed. Payload verification needs no printable block listing.
    if (payloads) {
        payloads->reserve(entries.size());
        for (const auto& entry : entries) {
            const auto* row = toc.data() + entry.tocPosition;
            payloads->push_back({entry.offset + wire::kBlockHeaderSize + entry.key.size(),
                                 U64(row + 24), U32(row + 32)});
        }
    } else {
        out.blocks.reserve(entries.size());
        for (const auto& entry : entries) {
            const auto* row = toc.data() + entry.tocPosition;
            out.blocks.push_back({std::string(entry.key), U32(row + 4), U32(row + 8),
                                  U32(row + 12), U64(row + 24)});
        }
    }
    out.status = DirectoryStatus::MetadataOk;
    return out;
}

}  // namespace directory_detail

template<class ReadAt>
DirectoryResult InspectPmmDirectory(std::uint64_t fileSize, ReadAt readAt) {
    return directory_detail::InspectPmmMetadata(fileSize, readAt);
}

}  // namespace mikumikustudio::format_probe
