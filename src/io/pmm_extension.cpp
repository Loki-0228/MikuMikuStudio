// PMM v2 tail extension codec ("MMSXPMM2" trailer; legacy "MDSXPMM2" is read).
//
// Format, limits and API contract: include/mikudancestudio/pmm_extension.hpp.
// Serialization is field by field (explicit little-endian stores); no C++
// struct is written to or read from disk, so padding cannot leak into the
// file.  Integer size arithmetic is checked before it is used, so a hostile
// uint64 in the trailer cannot wrap a size check.

#include "mikudancestudio/pmm_extension.hpp"

#include <io.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace mikudancestudio {
namespace pmm_extension {
namespace {

const char kMagic[8] = {'M', 'M', 'S', 'X', 'P', 'M', 'M', '2'};
// Files written before the MDS -> MMS rename keep the old marker.  Read it,
// never write it.
const char kLegacyMagic[8] = {'M', 'D', 'S', 'X', 'P', 'M', 'M', '2'};

struct Crc32Table {
    std::uint32_t entry[256];
    Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            }
            entry[i] = c;
        }
    }
};

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    static const Crc32Table table;
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ table.entry[(crc ^ data[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

void PutU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void PutU64(std::uint8_t* out, std::uint64_t value) {
    PutU32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
    PutU32(out + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t GetU32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

std::uint64_t GetU64(const std::uint8_t* in) {
    return static_cast<std::uint64_t>(GetU32(in)) |
           (static_cast<std::uint64_t>(GetU32(in + 4)) << 32);
}

// Reads exactly `size` bytes; false on EOF or error.  `size` is bounded by the
// caller (kMaxExtensionBytes or kTrailerSize).
bool ReadExact(int fd, void* buffer, std::size_t size) {
    std::uint8_t* out = static_cast<std::uint8_t*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const std::size_t remaining = size - done;
        const unsigned int want =
            remaining > 0x40000000u ? 0x40000000u : static_cast<unsigned int>(remaining);
        const int got = _read(fd, out + done, want);
        if (got <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(got);
    }
    return true;
}

// Writes exactly `size` bytes; false on error or on a write that stops short.
bool WriteAll(int fd, const void* buffer, std::size_t size) {
    const std::uint8_t* in = static_cast<const std::uint8_t*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const std::size_t remaining = size - done;
        const unsigned int want =
            remaining > 0x40000000u ? 0x40000000u : static_cast<unsigned int>(remaining);
        const int wrote = _write(fd, in + done, want);
        if (wrote <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(wrote);
    }
    return true;
}

bool IsRepresentableKey(const std::string& key) {
    if (key.empty() || key.size() > kMaxKeyBytes) {
        return false;
    }
    return std::memchr(key.data(), 0, key.size()) == nullptr;
}

// Parses the record stream into `parsed`.  Fails on any malformed record:
// short header, keySize outside 1..255, key or value past the end of the
// stream, NUL in a key, duplicate key, more than kMaxRecordCount records.
bool ParseRecords(const std::vector<std::uint8_t>& bytes, Document& parsed) {
    parsed.records.clear();
    std::unordered_set<std::string> seen;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 8) {
            return false;
        }
        const std::uint32_t keySize = GetU32(bytes.data() + pos);
        const std::uint32_t valueSize = GetU32(bytes.data() + pos + 4);
        pos += 8;
        if (keySize < 1 || keySize > kMaxKeyBytes) {
            return false;
        }
        const std::uint64_t payload = static_cast<std::uint64_t>(keySize) +
                                      static_cast<std::uint64_t>(valueSize);
        if (payload > static_cast<std::uint64_t>(bytes.size() - pos)) {
            return false;
        }
        const char* keyBegin = reinterpret_cast<const char*>(bytes.data() + pos);
        if (std::memchr(keyBegin, 0, keySize) != nullptr) {
            return false;
        }
        if (parsed.records.size() >= kMaxRecordCount) {
            return false;
        }
        Record record;
        record.key.assign(keyBegin, keySize);
        if (!seen.insert(record.key).second) {
            return false;
        }
        const std::uint8_t* valueBegin = bytes.data() + pos + keySize;
        record.value.assign(valueBegin, valueBegin + valueSize);
        parsed.records.push_back(std::move(record));
        pos += static_cast<std::size_t>(payload);
    }
    return true;
}

// Validates the trailer against the file length and loads the records.  The
// caller has already matched the magic.  Overflow-safe: every addition is
// checked before it happens, and extensionSize is bounded first.
ReadResult LoadFromTrailer(int fd, const std::uint8_t* trailer, std::uint64_t fileLength,
                           Document& parsed, std::uint64_t& standardOut) {
    const std::uint64_t standard = GetU64(trailer + 8);
    const std::uint64_t extension = GetU64(trailer + 16);
    const std::uint32_t crc = GetU32(trailer + 24);
    const std::uint32_t reserved = GetU32(trailer + 28);

    if (reserved != 0) {
        return ReadResult::Invalid;
    }
    if (extension > kMaxExtensionBytes) {
        return ReadResult::Invalid;
    }
    if (standard < kMinStandardSize) {
        return ReadResult::Invalid;
    }
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total = standard;
    if (total > max - extension) {
        return ReadResult::Invalid;
    }
    total += extension;
    if (total > max - kTrailerSize) {
        return ReadResult::Invalid;
    }
    total += kTrailerSize;
    if (total != fileLength) {
        return ReadResult::Invalid;
    }

    std::vector<std::uint8_t> bytes;
    if (extension != 0) {
        // standard <= fileLength - kTrailerSize - extension, so the offset fits
        // __int64 and the buffer stays within kMaxExtensionBytes.
        bytes.resize(static_cast<std::size_t>(extension));
        if (_lseeki64(fd, static_cast<__int64>(standard), SEEK_SET) < 0) {
            return ReadResult::IoError;
        }
        if (!ReadExact(fd, bytes.data(), bytes.size())) {
            return ReadResult::IoError;
        }
    }
    if (Crc32(bytes.data(), bytes.size()) != crc) {
        return ReadResult::Invalid;
    }
    if (!ParseRecords(bytes, parsed)) {
        return ReadResult::Invalid;
    }
    standardOut = standard;
    return ReadResult::Loaded;
}

}  // namespace

ReadResult Read(int fd, Document& out, std::uint64_t& standardSize) {
    out.records.clear();

    const __int64 saved = _telli64(fd);
    if (saved < 0) {
        return ReadResult::IoError;
    }
    const __int64 length = _filelengthi64(fd);
    if (length < 0) {
        return ReadResult::IoError;
    }

    ReadResult result = ReadResult::Absent;
    Document parsed;
    std::uint64_t parsedStandard = 0;

    if (length >= static_cast<__int64>(kTrailerSize)) {
        std::uint8_t trailer[kTrailerSize];
        if (_lseeki64(fd, length - static_cast<__int64>(kTrailerSize), SEEK_SET) < 0) {
            result = ReadResult::IoError;
        } else if (!ReadExact(fd, trailer, kTrailerSize)) {
            result = ReadResult::IoError;
        } else if (std::memcmp(trailer, kMagic, sizeof(kMagic)) == 0 ||
                   std::memcmp(trailer, kLegacyMagic, sizeof(kLegacyMagic)) == 0) {
            result = LoadFromTrailer(fd, trailer, static_cast<std::uint64_t>(length), parsed,
                                     parsedStandard);
        }
    }

    if (_lseeki64(fd, saved, SEEK_SET) < 0) {
        out.records.clear();
        return ReadResult::IoError;
    }
    if (result == ReadResult::Loaded) {
        out = std::move(parsed);
        standardSize = parsedStandard;
    }
    return result;
}

bool Representable(const Document& document) {
    if (document.records.size() > kMaxRecordCount) {
        return false;
    }

    std::unordered_set<std::string> seen;
    std::uint64_t total = 0;
    for (const Record& record : document.records) {
        if (!IsRepresentableKey(record.key)) {
            return false;
        }
        if (!seen.insert(record.key).second) {
            return false;
        }
        if (record.value.size() > UINT32_MAX || static_cast<std::uint64_t>(record.value.size()) > kMaxExtensionBytes) {
            return false;
        }
        const std::uint64_t add = 8ull + static_cast<std::uint64_t>(record.key.size()) +
                                  static_cast<std::uint64_t>(record.value.size());
        if (add > kMaxExtensionBytes - total) {
            return false;
        }
        total += add;

    }
    return true;
}

bool Append(int fd, const Document& document) {
    if (!Representable(document)) return false;
    const __int64 position = _telli64(fd);
    const __int64 length = _filelengthi64(fd);
    if (position < 0 || length < 0) {
        return false;
    }
    if (position != length) {
        return false;  // must append at the end of file
    }
    if (position < static_cast<__int64>(kMinStandardSize)) {
        return false;
    }

    // Stream records without allocating a second copy of the entire document.
    static const Crc32Table table;
    std::uint32_t crc=0xffffffffu;std::uint64_t total=0;
    auto write=[&](const void* bytes,std::size_t count){
        const auto* p=static_cast<const std::uint8_t*>(bytes);
        for(std::size_t i=0;i<count;++i)crc=(crc>>8)^table.entry[(crc^p[i])&255];
        total+=count;return WriteAll(fd,bytes,count);
    };
    for(const auto& record:document.records){
        std::uint8_t header[8];PutU32(header,static_cast<std::uint32_t>(record.key.size()));PutU32(header+4,static_cast<std::uint32_t>(record.value.size()));
        if(!write(header,8)||!write(record.key.data(),record.key.size())||!write(record.value.data(),record.value.size()))return false;
    }
    std::uint8_t trailer[kTrailerSize];
    std::memcpy(trailer, kMagic, sizeof(kMagic));
    PutU64(trailer + 8, static_cast<std::uint64_t>(position));
    PutU64(trailer + 16, total);
    PutU32(trailer + 24, crc^0xffffffffu);
    PutU32(trailer + 28, 0);

    return WriteAll(fd, trailer, kTrailerSize);
}

const Record* Find(const Document& document, const std::string& key) {
    for (const Record& record : document.records) {
        if (record.key == key) {
            return &record;
        }
    }
    return nullptr;
}

void Set(Document& document, const std::string& key, std::vector<std::uint8_t> value) {
    if (!IsRepresentableKey(key)) {
        return;
    }
    for (Record& record : document.records) {
        if (record.key == key) {
            record.value = std::move(value);
            return;
        }
    }
    Record record;
    record.key = key;
    record.value = std::move(value);
    document.records.push_back(std::move(record));
}

bool Erase(Document& document, const std::string& key) {
    for (std::vector<Record>::iterator it = document.records.begin();
         it != document.records.end(); ++it) {
        if (it->key == key) {
            document.records.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace pmm_extension
}  // namespace mikudancestudio
