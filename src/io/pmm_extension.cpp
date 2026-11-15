// PMM v2 tail extension codec ("MMSXPMM2" trailer).
//
// Format, limits and API contract: include/mikumikustudio/pmm_extension.hpp.
// Serialization is field by field (explicit little-endian stores); no C++
// struct is written to or read from disk, so padding cannot leak into the
// file.  Integer size arithmetic is checked before it is used, so a hostile
// uint64 in the trailer cannot wrap a size check.

#include "mikumikustudio/pmm_extension.hpp"

#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace mikumikustudio {
namespace pmm_extension {
namespace {

const char kMagic[8] = {'M', 'M', 'S', 'X', 'P', 'M', 'M', '2'};

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

class Crc32 {
public:
    void Update(const void* data, std::size_t size) {
        static const Crc32Table table;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            crc_ = (crc_ >> 8) ^ table.entry[(crc_ ^ bytes[i]) & 0xFFu];
        }
    }

    std::uint32_t Value() const { return crc_ ^ 0xFFFFFFFFu; }

private:
    std::uint32_t crc_ = 0xFFFFFFFFu;
};

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

// Reads exactly `size` bytes; false on EOF or error.
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

// One fixed-size buffer coalesces small reads and checksums discarded bytes.
// Large pieces of a value go straight into its owning vector.
class TailReader {
public:
    TailReader(int fd, std::size_t size) : fd_(fd), remaining_(size) {}

    std::size_t Remaining() const { return remaining_; }
    std::uint32_t Checksum() const { return crc_.Value(); }

    bool Read(void* buffer, std::size_t size) {
        if (size > remaining_) {
            return false;
        }
        auto* out = static_cast<std::uint8_t*>(buffer);
        while (size != 0) {
            std::size_t count;
            if (next_ != end_) {
                count = (std::min)(size, end_ - next_);
                std::memcpy(out, bytes_ + next_, count);
                next_ += count;
            } else if (size >= sizeof(bytes_)) {
                count = sizeof(bytes_);
                if (!ReadChunk(out, count)) {
                    return false;
                }
            } else {
                next_ = 0;
                end_ = (std::min)(remaining_, sizeof(bytes_));
                if (!ReadChunk(bytes_, end_)) {
                    return false;
                }
                continue;
            }
            out += count;
            size -= count;
            remaining_ -= count;
        }
        return true;
    }

    bool Drain() {
        // Buffered bytes were already included in the CRC when read.
        remaining_ -= end_ - next_;
        next_ = end_ = 0;
        while (remaining_ != 0) {
            const std::size_t count = (std::min)(remaining_, sizeof(bytes_));
            if (!ReadChunk(bytes_, count)) {
                return false;
            }
            remaining_ -= count;
        }
        return true;
    }

private:
    bool ReadChunk(void* out, std::size_t count) {
        if (!ReadExact(fd_, out, count)) {
            return false;
        }
        crc_.Update(out, count);
        return true;
    }

    int fd_;
    std::size_t remaining_;
    std::size_t next_ = 0;
    std::size_t end_ = 0;
    Crc32 crc_;
    std::uint8_t bytes_[64 * 1024];
};

ReadResult ReadRecords(TailReader& reader, Document& parsed) {
    std::unordered_set<std::string> seen;
    while (reader.Remaining() != 0) {
        std::uint8_t header[8];
        if (reader.Remaining() < sizeof(header)) {
            return ReadResult::Invalid;
        }
        if (!reader.Read(header, sizeof(header))) {
            return ReadResult::IoError;
        }
        const std::uint32_t keySize = GetU32(header);
        const std::uint32_t valueSize = GetU32(header + 4);
        if (keySize < 1 || keySize > kMaxKeyBytes) {
            return ReadResult::Invalid;
        }
        if (keySize > reader.Remaining() ||
            valueSize > reader.Remaining() - keySize ||
            parsed.records.size() >= kMaxRecordCount) {
            return ReadResult::Invalid;
        }
        char key[kMaxKeyBytes];
        if (!reader.Read(key, keySize)) {
            return ReadResult::IoError;
        }
        if (std::memchr(key, 0, keySize) != nullptr) {
            return ReadResult::Invalid;
        }
        Record record;
        record.key.assign(key, keySize);
        if (!seen.insert(record.key).second) {
            return ReadResult::Invalid;
        }
        record.value.resize(valueSize);
        if (!reader.Read(record.value.data(), valueSize)) {
            return ReadResult::IoError;
        }
        parsed.records.push_back(std::move(record));
    }
    return ReadResult::Loaded;
}

ReadResult LoadRecords(int fd, __int64 offset, std::size_t size, std::uint32_t crc,
                       Document& parsed) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) {
        return ReadResult::IoError;
    }
    TailReader reader(fd, size);

    ReadResult result;
    try {
        result = ReadRecords(reader, parsed);
    } catch (...) {
        // Preserve I/O and checksum precedence over failures while parsing a
        // record. With a fully readable, matching tail, propagate the original
        // exception; none of the tentative records reach the caller.
        if (!reader.Drain()) {
            return ReadResult::IoError;
        }
        if (reader.Checksum() != crc) {
            return ReadResult::Invalid;
        }
        throw;
    }
    // An early framing error still has to read the full claimed stream before
    // reporting Invalid: a short or failed read takes precedence as IoError.
    if (result == ReadResult::IoError || !reader.Drain()) {
        return ReadResult::IoError;
    }
    return reader.Checksum() == crc ? result : ReadResult::Invalid;
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

    if (extension != 0) {
        // The validated offset fits __int64 and the size fits size_t.
        const ReadResult result = LoadRecords(fd, static_cast<__int64>(standard),
                                              static_cast<std::size_t>(extension), crc, parsed);
        if (result != ReadResult::Loaded) {
            return result;
        }
    } else if (crc != 0) {
        return ReadResult::Invalid;
    }
    standardOut = standard;
    return ReadResult::Loaded;
}

// A normal restore failure is reported as IoError. If loading throws, restore
// on unwind without replacing the original exception with a new error.
class FilePositionGuard {
public:
    FilePositionGuard(int fd, __int64 saved) : fd_(fd), saved_(saved) {}

    ~FilePositionGuard() {
        if (needsRestore_) {
            Restore();
        }
    }

    bool Restore() {
        needsRestore_ = false;
        return _lseeki64(fd_, saved_, SEEK_SET) >= 0;
    }

    FilePositionGuard(const FilePositionGuard&) = delete;
    FilePositionGuard& operator=(const FilePositionGuard&) = delete;

private:
    int fd_;
    __int64 saved_;
    bool needsRestore_ = true;
};

}  // namespace

ReadResult Read(int fd, Document& out, std::uint64_t& standardSize) {
    out.records.clear();

    const __int64 saved = _telli64(fd);
    if (saved < 0) {
        return ReadResult::IoError;
    }
    FilePositionGuard position(fd, saved);
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
        } else if (std::memcmp(trailer, kMagic, sizeof(kMagic)) == 0) {
            result = LoadFromTrailer(fd, trailer, static_cast<std::uint64_t>(length), parsed,
                                     parsedStandard);
        }
    }

    if (!position.Restore()) {
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
    Crc32 crc;
    std::uint64_t total=0;
    auto write=[&](const void* bytes,std::size_t count){
        crc.Update(bytes, count);
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
    PutU32(trailer + 24, crc.Value());
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
}  // namespace mikumikustudio
