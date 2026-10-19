// PMM 0003 block container codec.  Format reference:
// docs/reference/pmm-0003-container.md.  This module is deliberately
// standalone so the wire layout can be tested without MMDApp, a window or a
// GPU.  It only reads/writes the container after the standard PMM prefix.

#include "mikudancestudio/pmm_container.hpp"

#include <io.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace mikudancestudio::pmm_container {
namespace {

constexpr char kTocMagic[8] = {'P', 'M', 'M', '3', 'T', 'O', 'C', '\0'};
constexpr char kFooterMagic[8] = {'P', 'M', 'M', '3', 'E', 'N', 'D', '\0'};
constexpr char kLegacyMagic[8] = {'M', 'D', 'S', 'X', 'P', 'M', 'M', '2'};
constexpr char kCurrentLegacyMagic[8] = {'M', 'M', 'S', 'X', 'P', 'M', 'M', '2'};

// Footer offsets.
constexpr std::size_t kFooterVersion = 8;
constexpr std::size_t kFooterFlags = 12;
constexpr std::size_t kFooterStandardSize = 16;
constexpr std::size_t kFooterTocOffset = 24;
constexpr std::size_t kFooterTocSize = 32;
constexpr std::size_t kFooterTotalSize = 40;
constexpr std::size_t kFooterTocCrc = 48;
constexpr std::size_t kFooterReserved32 = 52;
constexpr std::size_t kFooterReserved64 = 56;

// TOC header offsets.
constexpr std::size_t kTocVersionOffset = 8;
constexpr std::size_t kTocFlagsOffset = 12;
constexpr std::size_t kTocCountOffset = 16;
constexpr std::size_t kTocReservedOffset = 20;

// Block header offsets.
constexpr std::size_t kBlockKeySize = 0;
constexpr std::size_t kBlockVersion = 4;
constexpr std::size_t kBlockFlags = 8;
constexpr std::size_t kBlockEncoding = 12;
constexpr std::size_t kBlockPayloadSize = 16;
constexpr std::size_t kBlockPayloadCrc = 24;
constexpr std::size_t kBlockReserved = 28;

// TOC entry offsets (relative to the entry).
constexpr std::size_t kEntryKeySize = 0;
constexpr std::size_t kEntryVersion = 4;
constexpr std::size_t kEntryFlags = 8;
constexpr std::size_t kEntryEncoding = 12;
constexpr std::size_t kEntryBlockOffset = 16;
constexpr std::size_t kEntryPayloadSize = 24;
constexpr std::size_t kEntryPayloadCrc = 32;
constexpr std::size_t kEntryReserved = 36;

std::uint32_t GetU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t GetU64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(GetU32(p)) |
           (static_cast<std::uint64_t>(GetU32(p + 4)) << 32);
}

void PutU32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xFFu);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void PutU64(std::uint8_t* p, std::uint64_t value) {
    PutU32(p, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
    PutU32(p + 4, static_cast<std::uint32_t>(value >> 32));
}

// IEEE CRC32, reflected, polynomial 0xEDB88320.  The table is initialized
// once by a function-local static (thread-safe in C++11 and later).
struct Crc32Table {
    std::uint32_t entry[256]{};
    Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            entry[i] = c;
        }
    }
};

std::uint32_t Crc32(const void* data, std::size_t size) {
    static const Crc32Table table;
    std::uint32_t crc = 0xFFFFFFFFu;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table.entry[(crc ^ bytes[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

bool ReadExact(int fd, void* buffer, std::size_t size) {
    auto* out = static_cast<std::uint8_t*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const std::size_t remaining = size - done;
        const unsigned int want = remaining > 0x40000000u
                                      ? 0x40000000u
                                      : static_cast<unsigned int>(remaining);
        const int got = _read(fd, out + done, want);
        if (got <= 0) return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}

bool WriteAll(int fd, const void* buffer, std::size_t size) {
    const auto* in = static_cast<const std::uint8_t*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const std::size_t remaining = size - done;
        const unsigned int want = remaining > 0x40000000u
                                      ? 0x40000000u
                                      : static_cast<unsigned int>(remaining);
        const int wrote = _write(fd, in + done, want);
        if (wrote <= 0) return false;
        done += static_cast<std::size_t>(wrote);
    }
    return true;
}

bool ValidKey(std::string_view key) {
    return !key.empty() && key.size() <= kMaxKeyBytes &&
           std::memchr(key.data(), 0, key.size()) == nullptr;
}

bool RepresentableImpl(const Document& document) {
    if (document.blocks.size() >
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))
        return false;
    std::set<std::string> keys;
    std::uint64_t total = kTocHeaderSize + kFooterSize +
                          kMinimumStandardSize;
    for (const Block& block : document.blocks) {
        if (!ValidKey(block.key)) return false;
        if (!keys.insert(block.key).second) return false;
        if (block.payload.size() >
            static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()))
            return false;
        const std::uint64_t blockBytes =
            static_cast<std::uint64_t>(kBlockHeaderSize + block.key.size()) +
            static_cast<std::uint64_t>(block.payload.size());
        const std::uint64_t entryBytes =
            static_cast<std::uint64_t>(kTocEntrySize + block.key.size());
        if (total >
            (std::numeric_limits<std::uint64_t>::max)() - blockBytes)
            return false;
        total += blockBytes;
        if (total > (std::numeric_limits<std::uint64_t>::max)() - entryBytes)
            return false;
        total += entryBytes;
    }
    return total <=
           static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
}

struct TocEntry {
    const Block* block = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t payloadSize = 0;
    std::uint32_t payloadCrc = 0;
};

bool ReadContainer(int fd, __int64 length, Document& document,
                   std::uint64_t& standardSize) {
    std::uint8_t footer[kFooterSize]{};
    if (_lseeki64(fd, length - static_cast<__int64>(kFooterSize), SEEK_SET) < 0)
        return false;
    if (!ReadExact(fd, footer, sizeof footer)) return false;
    if (std::memcmp(footer, kFooterMagic, sizeof kFooterMagic) != 0)
        return false;
    if (GetU32(footer + kFooterVersion) != kContainerVersion) return false;
    if (GetU32(footer + kFooterFlags) != 0) return false;
    if (GetU32(footer + kFooterReserved32) != 0) return false;
    if (GetU64(footer + kFooterReserved64) != 0) return false;
    const std::uint64_t standard = GetU64(footer + kFooterStandardSize);
    const std::uint64_t tocOffset = GetU64(footer + kFooterTocOffset);
    const std::uint64_t tocSize = GetU64(footer + kFooterTocSize);
    const std::uint64_t totalSize = GetU64(footer + kFooterTotalSize);
    const std::uint32_t tocCrc = GetU32(footer + kFooterTocCrc);
    if (totalSize != static_cast<std::uint64_t>(length)) return false;
    if (standard < kMinimumStandardSize || standard > tocOffset) return false;
    if (tocSize < kTocHeaderSize) return false;
    if (tocOffset > (std::numeric_limits<std::uint64_t>::max)() - tocSize)
        return false;
    if (tocOffset + tocSize !=
        static_cast<std::uint64_t>(length) - kFooterSize)
        return false;
    if (tocSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        return false;

    std::vector<std::uint8_t> toc(static_cast<std::size_t>(tocSize));
    if (_lseeki64(fd, static_cast<__int64>(tocOffset), SEEK_SET) < 0)
        return false;
    if (!ReadExact(fd, toc.data(), toc.size())) return false;
    if (Crc32(toc.data(), toc.size()) != tocCrc) return false;
    if (std::memcmp(toc.data(), kTocMagic, sizeof kTocMagic) != 0) return false;
    if (GetU32(toc.data() + kTocVersionOffset) != kTocVersion) return false;
    if (GetU32(toc.data() + kTocFlagsOffset) != 0) return false;
    if (GetU32(toc.data() + kTocReservedOffset) != 0) return false;
    const std::uint32_t count = GetU32(toc.data() + kTocCountOffset);
    if (count > (toc.size() - kTocHeaderSize) / kTocEntrySize) return false;

    std::vector<Block> blocks;
    blocks.reserve(count);
    std::set<std::string> keys;
    std::size_t at = kTocHeaderSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (toc.size() - at < kTocEntrySize) return false;
        const std::uint8_t* entry = toc.data() + at;
        const std::uint32_t keySize = GetU32(entry + kEntryKeySize);
        if (keySize == 0 || keySize > kMaxKeyBytes) return false;
        if (toc.size() - at - kTocEntrySize < keySize) return false;
        const std::string key(reinterpret_cast<const char*>(entry + kTocEntrySize),
                              keySize);
        if (!ValidKey(key)) return false;
        if (!keys.insert(key).second) return false;
        const std::uint32_t version = GetU32(entry + kEntryVersion);
        const std::uint32_t flags = GetU32(entry + kEntryFlags);
        const std::uint32_t encoding = GetU32(entry + kEntryEncoding);
        const std::uint64_t blockOffset = GetU64(entry + kEntryBlockOffset);
        const std::uint64_t payloadSize = GetU64(entry + kEntryPayloadSize);
        const std::uint32_t payloadCrc = GetU32(entry + kEntryPayloadCrc);
        if (GetU32(entry + kEntryReserved) != 0) return false;
        if (blockOffset < standard) return false;
        const std::uint64_t blockHead =
            static_cast<std::uint64_t>(kBlockHeaderSize) + keySize;
        if (blockOffset > tocOffset) return false;
        if (blockHead > tocOffset - blockOffset) return false;
        if (payloadSize > tocOffset - blockOffset - blockHead) return false;
        if (payloadSize >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
            return false;

        std::uint8_t header[kBlockHeaderSize]{};
        if (_lseeki64(fd, static_cast<__int64>(blockOffset), SEEK_SET) < 0)
            return false;
        if (!ReadExact(fd, header, sizeof header)) return false;
        if (GetU32(header + kBlockKeySize) != keySize) return false;
        if (GetU32(header + kBlockVersion) != version) return false;
        if (GetU32(header + kBlockFlags) != flags) return false;
        if (GetU32(header + kBlockEncoding) != encoding) return false;
        if (GetU64(header + kBlockPayloadSize) != payloadSize) return false;
        if (GetU32(header + kBlockPayloadCrc) != payloadCrc) return false;
        if (GetU32(header + kBlockReserved) != 0) return false;
        std::string storedKey(keySize, '\0');
        if (!ReadExact(fd, storedKey.data(), storedKey.size())) return false;
        if (storedKey != key) return false;

        Block block;
        block.key = key;
        block.version = version;
        block.flags = flags;
        block.encoding = static_cast<Encoding>(encoding);
        block.payload.resize(static_cast<std::size_t>(payloadSize));
        if (!block.payload.empty() &&
            !ReadExact(fd, block.payload.data(), block.payload.size()))
            return false;
        if (Crc32(block.payload.data(), block.payload.size()) != payloadCrc)
            return false;
        blocks.push_back(std::move(block));
        at += kTocEntrySize + keySize;
    }
    if (at != toc.size()) return false;
    document.blocks = std::move(blocks);
    standardSize = standard;
    return true;
}

}  // namespace

ReadResult Read(int fd, Document& document, std::uint64_t& standardSize) {
    document.blocks.clear();
    const __int64 saved = _telli64(fd);
    if (saved < 0) return ReadResult::Invalid;
    const __int64 length = _filelengthi64(fd);
    if (length < 0) return ReadResult::Invalid;

    auto restore = [&] { _lseeki64(fd, saved, SEEK_SET); };

    // A PMM3END footer takes precedence; otherwise a trailing extension
    // marker (MMSXPMM2, or legacy MDSXPMM2) is reported so the caller can use
    // the existing extension codec.
    if (length >= static_cast<__int64>(kFooterSize)) {
        std::uint8_t footer[kFooterSize]{};
        if (_lseeki64(fd, length - static_cast<__int64>(kFooterSize), SEEK_SET) < 0) {
            restore();
            return ReadResult::Invalid;
        }
        if (!ReadExact(fd, footer, sizeof footer)) {
            restore();
            return ReadResult::Invalid;
        }
        if (std::memcmp(footer, kFooterMagic, sizeof kFooterMagic) == 0) {
            try {
                if (!ReadContainer(fd, length, document, standardSize)) {
                    document.blocks.clear();
                    restore();
                    return ReadResult::Invalid;
                }
            } catch (const std::bad_alloc&) {
                document.blocks.clear();
                restore();
                return ReadResult::Invalid;
            }
            restore();
            return ReadResult::Loaded;
        }
    }

    if (length >= 32) {
        std::uint8_t trailer[32]{};
        if (_lseeki64(fd, length - 32, SEEK_SET) < 0) {
            restore();
            return ReadResult::Invalid;
        }
        if (!ReadExact(fd, trailer, sizeof trailer)) {
            restore();
            return ReadResult::Invalid;
        }
        if (std::memcmp(trailer, kLegacyMagic, sizeof kLegacyMagic) == 0 ||
            std::memcmp(trailer, kCurrentLegacyMagic,
                        sizeof kCurrentLegacyMagic) == 0) {
            const std::uint64_t standard = GetU64(trailer + 8);
            if (standard < kMinimumStandardSize ||
                standard > static_cast<std::uint64_t>(length) - 32) {
                restore();
                return ReadResult::Invalid;
            }
            standardSize = standard;
            restore();
            return ReadResult::Legacy;
        }
    }

    restore();
    standardSize = 0;
    return ReadResult::Absent;
}

bool Representable(const Document& document) {
    try {
        return RepresentableImpl(document);
    } catch (...) {
        return false;
    }
}

bool Write(int fd, const Document& document) {
    if (!Representable(document)) return false;
    const __int64 start = _telli64(fd);
    if (start < static_cast<__int64>(kMinimumStandardSize)) return false;
    if (_filelengthi64(fd) != start) return false;

    std::vector<Block> ordered = document.blocks;
    std::sort(ordered.begin(), ordered.end(),
              [](const Block& a, const Block& b) { return a.key < b.key; });

    std::vector<TocEntry> entries;
    entries.reserve(ordered.size());
    for (const Block& block : ordered) {
        TocEntry entry;
        entry.block = &block;
        entry.offset = static_cast<std::uint64_t>(_telli64(fd));
        entry.payloadSize = static_cast<std::uint64_t>(block.payload.size());
        entry.payloadCrc = Crc32(block.payload.data(), block.payload.size());
        std::uint8_t header[kBlockHeaderSize]{};
        PutU32(header + kBlockKeySize,
               static_cast<std::uint32_t>(block.key.size()));
        PutU32(header + kBlockVersion, block.version);
        PutU32(header + kBlockFlags, block.flags);
        PutU32(header + kBlockEncoding,
               static_cast<std::uint32_t>(block.encoding));
        PutU64(header + kBlockPayloadSize, entry.payloadSize);
        PutU32(header + kBlockPayloadCrc, entry.payloadCrc);
        PutU32(header + kBlockReserved, 0);
        if (!WriteAll(fd, header, sizeof header) ||
            !WriteAll(fd, block.key.data(), block.key.size()) ||
            !WriteAll(fd, block.payload.data(), block.payload.size()))
            return false;
        entries.push_back(entry);
    }

    const std::uint64_t tocOffset = static_cast<std::uint64_t>(_telli64(fd));
    std::uint64_t tocSize = kTocHeaderSize;
    for (const TocEntry& entry : entries)
        tocSize += kTocEntrySize + entry.block->key.size();
    std::vector<std::uint8_t> toc(static_cast<std::size_t>(tocSize), 0);
    std::memcpy(toc.data(), kTocMagic, sizeof kTocMagic);
    PutU32(toc.data() + kTocVersionOffset, kTocVersion);
    PutU32(toc.data() + kTocFlagsOffset, 0);
    PutU32(toc.data() + kTocCountOffset,
           static_cast<std::uint32_t>(entries.size()));
    PutU32(toc.data() + kTocReservedOffset, 0);
    std::size_t at = kTocHeaderSize;
    for (const TocEntry& entry : entries) {
        std::uint8_t* out = toc.data() + at;
        const auto keySize = static_cast<std::uint32_t>(entry.block->key.size());
        PutU32(out + kEntryKeySize, keySize);
        PutU32(out + kEntryVersion, entry.block->version);
        PutU32(out + kEntryFlags, entry.block->flags);
        PutU32(out + kEntryEncoding,
               static_cast<std::uint32_t>(entry.block->encoding));
        PutU64(out + kEntryBlockOffset, entry.offset);
        PutU64(out + kEntryPayloadSize, entry.payloadSize);
        PutU32(out + kEntryPayloadCrc, entry.payloadCrc);
        PutU32(out + kEntryReserved, 0);
        std::memcpy(out + kTocEntrySize, entry.block->key.data(),
                    entry.block->key.size());
        at += kTocEntrySize + keySize;
    }
    const std::uint32_t tocCrc = Crc32(toc.data(), toc.size());
    if (!WriteAll(fd, toc.data(), toc.size())) return false;

    std::uint8_t footer[kFooterSize]{};
    std::memcpy(footer, kFooterMagic, sizeof kFooterMagic);
    PutU32(footer + kFooterVersion, kContainerVersion);
    PutU32(footer + kFooterFlags, 0);
    PutU64(footer + kFooterStandardSize, static_cast<std::uint64_t>(start));
    PutU64(footer + kFooterTocOffset, tocOffset);
    PutU64(footer + kFooterTocSize, tocSize);
    PutU64(footer + kFooterTotalSize, tocOffset + tocSize + kFooterSize);
    PutU32(footer + kFooterTocCrc, tocCrc);
    PutU32(footer + kFooterReserved32, 0);
    PutU64(footer + kFooterReserved64, 0);
    return WriteAll(fd, footer, sizeof footer);
}

const Block* Find(const Document& document, std::string_view key) {
    for (const Block& block : document.blocks)
        if (block.key == key) return &block;
    return nullptr;
}

void Set(Document& document, Block block) {
    if (!ValidKey(block.key)) return;
    for (Block& existing : document.blocks) {
        if (existing.key == block.key) {
            existing = std::move(block);
            return;
        }
    }
    document.blocks.push_back(std::move(block));
}

bool Erase(Document& document, std::string_view key) {
    for (auto it = document.blocks.begin(); it != document.blocks.end(); ++it) {
        if (it->key == key) {
            document.blocks.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace mikudancestudio::pmm_container
