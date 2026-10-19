// VPD pose-file tail extension for native bone scaling: the "MMSXVPDX" footer
// after the standard VPD text.  Wire layout and strictness rules are
// documented in include/mikudancestudio/vpd_scale_io.hpp.
//
// This module is deterministic CPU code with its own matrix validation, byte
// readers and CRC32 (the same standalone shape as pmm_extension.cpp), so a
// test can build and pin the wire format without a model and without linking
// the core.  The model-facing glue in vpd_file.cpp calls in here and writes
// bone scale only through the bone_scale.hpp API.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "mikudancestudio/vpd_scale_io.hpp"

namespace mikudancestudio {
namespace vpd_scale_io {
namespace {

// ---- CRC32 (IEEE, reflected, polynomial 0xEDB88320) ----------------------

struct Crc32Table {
    std::uint32_t value[256];
    Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            value[i] = crc;
        }
    }
};

std::uint32_t Crc32Update(std::uint32_t crc, const void* data, std::size_t size) {
    static const Crc32Table table;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table.value[(crc ^ bytes[i]) & 0xFFu];
    return crc;
}

std::uint32_t Crc32Of(const std::uint8_t* data, std::size_t size) {
    return Crc32Update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

// ---- little-endian byte writers and readers ------------------------------

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    PutU32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
    PutU32(out, static_cast<std::uint32_t>(value >> 32));
}

void PutF32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    PutU32(out, bits);
}

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool U32(std::uint32_t& out) {
        if (size - pos < 4) return false;
        out = static_cast<std::uint32_t>(data[pos]) |
              (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    }
    bool U64(std::uint64_t& out) {
        std::uint32_t low = 0, high = 0;
        if (!U32(low) || !U32(high)) return false;
        out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
        return true;
    }
    bool I32(std::int32_t& out) {
        std::uint32_t bits = 0;
        if (!U32(bits)) return false;
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    bool F32(float& out) {
        std::uint32_t bits = 0;
        if (!U32(bits)) return false;
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    bool Bytes(void* out, std::size_t count) {
        if (size - pos < count) return false;
        std::memcpy(out, data + pos, count);
        pos += count;
        return true;
    }
};

// ---- matrix validation (mirrors bone_scale::Valid semantics) -------------

double Determinant(const bone_scale::Matrix& a) {
    const auto& m = a.v;
    return double(m[0]) * (double(m[4]) * m[8] - double(m[5]) * m[7]) -
           double(m[1]) * (double(m[3]) * m[8] - double(m[5]) * m[6]) +
           double(m[2]) * (double(m[3]) * m[7] - double(m[4]) * m[6]);
}

bool InverseMatrix(const bone_scale::Matrix& a, bone_scale::Matrix& out) {
    const auto& m = a.v;
    const double d = Determinant(a);
    if (!std::isfinite(d) || std::abs(d) < 1.e-18) return false;
    const double cof[9] = {
        double(m[4]) * m[8] - double(m[5]) * m[7],
        double(m[2]) * m[7] - double(m[1]) * m[8],
        double(m[1]) * m[5] - double(m[2]) * m[4],
        double(m[5]) * m[6] - double(m[3]) * m[8],
        double(m[0]) * m[8] - double(m[2]) * m[6],
        double(m[2]) * m[3] - double(m[0]) * m[5],
        double(m[3]) * m[7] - double(m[4]) * m[6],
        double(m[1]) * m[6] - double(m[0]) * m[7],
        double(m[0]) * m[4] - double(m[1]) * m[3]};
    for (int i = 0; i < 9; ++i) out.v[i] = float(cof[i] / d);
    return true;
}

bool ValidMatrix(const bone_scale::Matrix& a) {
    for (float v : a.v)
        if (!std::isfinite(v) || std::abs(v) > 10000.f) return false;
    if (Determinant(a) <= 1.e-12) return false;
    bone_scale::Matrix inv;
    if (!InverseMatrix(a, inv)) return false;
    for (float v : inv.v)
        if (!std::isfinite(v) || std::abs(v) > 10000.f) return false;
    return true;
}

bool IdentityMatrix(const bone_scale::Matrix& a) {
    return a.v == bone_scale::Matrix{}.v;
}

// strcmp semantics inside two NUL-padded fixed fields.
bool FieldEquals(const char* a, std::size_t aSize, const char* b,
                 std::size_t bSize) {
    for (std::size_t i = 0;; ++i) {
        const char ca = i < aSize ? a[i] : '\0';
        const char cb = i < bSize ? b[i] : '\0';
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

// ---- size limits ---------------------------------------------------------

// Decode-side corruption guards.  The encoder never truncates what the model
// holds beyond these drops of non-samples (see the header), and a reader that
// would accept an untrusted file must stay inside its buffer.
constexpr std::uint32_t kMaxEntries = 1u << 20;
constexpr std::uint64_t kMaxTailBytes = 8ull * 1024ull * 1024ull;
constexpr std::size_t kEntryBytes = 60;
constexpr std::size_t kPayloadHeaderBytes = 32;
constexpr std::size_t kFooterBytes = 16;

// ---- tail identification -------------------------------------------------

// New writes use MMS; the MDS forms are read for old files.
const char kPayloadMagic[8] = {'M', 'M', 'S', 'X', 'V', 'P', 'D', 'S'};
const char kLegacyPayloadMagic[8] = {'M', 'D', 'S', 'X', 'V', 'P', 'D', 'S'};
const char kFooterMagic[8] = {'M', 'M', 'S', 'X', 'V', 'P', 'D', 'X'};
const char kLegacyFooterMagic[8] = {'M', 'D', 'S', 'X', 'V', 'P', 'D', 'X'};

// ---- shared encode -------------------------------------------------------

// Entries that survive the writer rules: a real, non-identity matrix and no
// earlier entry with the same (bone, name) identity.  Everything else is not
// a sample and is dropped.
std::vector<const Entry*> KeptEntries(const Extension& extension) {
    std::vector<const Entry*> kept;
    std::set<std::pair<std::int32_t, std::array<char, 20>>> seen;
    for (const Entry& entry : extension.entries) {
        if (!ValidMatrix(entry.scale) || IdentityMatrix(entry.scale)) continue;
        if (!seen.insert({entry.bone, entry.name}).second) continue;
        kept.push_back(&entry);
    }
    return kept;
}

std::vector<std::uint8_t> EncodePayload(const Extension& extension,
                                        std::uint32_t& keptCount) {
    const std::vector<const Entry*> kept = KeptEntries(extension);
    keptCount = static_cast<std::uint32_t>(kept.size());
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kPayloadMagic, kPayloadMagic + sizeof kPayloadMagic);
    PutU32(out, kVersion);
    PutU32(out, 0);
    PutU64(out, extension.prefixBytes);
    PutU32(out, extension.prefixCrc32);
    PutU32(out, keptCount);
    for (const Entry* entry : kept) {
        PutU32(out, static_cast<std::uint32_t>(entry->bone));
        out.insert(out.end(), entry->name.begin(), entry->name.end());
        for (float v : entry->scale.v) PutF32(out, v);
    }
    PutU32(out, Crc32Of(out.data(), out.size()));
    return out;
}

// ---- file helpers --------------------------------------------------------

// Streams the first `bytes` bytes of the open file and reports their CRC32,
// their line-end count and their last byte - one pass feeds the checksum, the
// standard-boundary check and its "the text ends on a line end" guard.
bool Fingerprint(int fd, std::uint64_t bytes, std::uint32_t& crc,
                 std::uint64_t& newlines, bool& endsWithNewline) {
    if (_lseeki64(fd, 0, SEEK_SET) != 0) return false;
    std::uint32_t state = 0xFFFFFFFFu;
    std::uint64_t lineEnds = 0;
    std::uint8_t last = 0;
    std::uint64_t remaining = bytes;
    std::vector<std::uint8_t> buffer(64u * 1024u);
    while (remaining > 0) {
        const unsigned int chunk = static_cast<unsigned int>(
            remaining > buffer.size() ? buffer.size() : remaining);
        const int got = _read(fd, buffer.data(), chunk);
        if (got != static_cast<int>(chunk)) return false;
        for (unsigned int i = 0; i < chunk; ++i)
            if (buffer[i] == '\n') ++lineEnds;
        last = buffer[chunk - 1];
        state = Crc32Update(state, buffer.data(), chunk);
        remaining -= chunk;
    }
    crc = state ^ 0xFFFFFFFFu;
    newlines = lineEnds;
    endsWithNewline = last == '\n';
    return true;
}

bool WriteAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const unsigned int chunk = static_cast<unsigned int>(
            (size - done) > 0x100000u ? 0x100000u : (size - done));
        const int wrote = _write(fd, data + done, chunk);
        if (wrote != static_cast<int>(chunk)) return false;
        done += static_cast<std::size_t>(wrote);
    }
    return true;
}

}  // namespace

// ---- payload codec -------------------------------------------------------

std::vector<std::uint8_t> Encode(const Extension& extension) {
    std::uint32_t keptCount = 0;
    return EncodePayload(extension, keptCount);
}

Decode DecodePayload(const std::uint8_t* data, std::size_t size,
                     Extension& out) {
    out = {};
    if (data == nullptr || size < kPayloadHeaderBytes + 4 ||
        size > kMaxTailBytes)
        return Decode::BadData;
    if (std::memcmp(data, kPayloadMagic, sizeof kPayloadMagic) != 0 &&
        std::memcmp(data, kLegacyPayloadMagic, sizeof kLegacyPayloadMagic) != 0)
        return Decode::BadMagic;
    Reader reader{data, size, 8};
    std::uint32_t version = 0, reserved = 0, crc = 0, count = 0;
    std::uint64_t prefixBytes = 0;
    if (!reader.U32(version) || !reader.U32(reserved) ||
        !reader.U64(prefixBytes) || !reader.U32(crc) || !reader.U32(count))
        return Decode::BadData;
    // Version 1 is the only layout this reader knows.  A future version is an
    // unknown version: refused as a whole and never read as version 1.
    if (version != kVersion) return Decode::BadVersion;
    if (reserved != 0) return Decode::BadData;
    (void)crc;
    if (count > kMaxEntries) return Decode::BadData;
    // The length formula is exact: one header, 60 bytes per entry, one CRC.
    // A payload that does not fit it is refused rather than read with a
    // guessed stride, so no read can leave the buffer.
    const std::uint64_t expected =
        kPayloadHeaderBytes + std::uint64_t(count) * kEntryBytes + 4;
    if (expected != static_cast<std::uint64_t>(size))
        return Decode::BadData;
    std::uint32_t stored = 0;
    std::memcpy(&stored, data + size - 4, 4);
    if (stored != Crc32Of(data, size - 4)) return Decode::BadCrc;
    Extension decoded;
    decoded.prefixBytes = prefixBytes;
    decoded.prefixCrc32 = crc;
    decoded.entries.reserve(count);
    std::set<std::pair<std::int32_t, std::array<char, 20>>> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry entry;
        if (!reader.I32(entry.bone) ||
            !reader.Bytes(entry.name.data(), entry.name.size()))
            return Decode::BadData;
        for (float& v : entry.scale.v)
            if (!reader.F32(v)) return Decode::BadData;
        // NaN/Inf, a non-positive determinant and out-of-bounds values are all
        // "not a matrix": the payload is refused as a whole.
        if (!ValidMatrix(entry.scale)) return Decode::BadData;
        if (!seen.insert({entry.bone, entry.name}).second)
            return Decode::BadData;
        decoded.entries.push_back(entry);
    }
    out = std::move(decoded);
    return Decode::Ok;
}

// ---- tail files ----------------------------------------------------------

TailSave AppendTail(int fd, const Extension& extension) {
    const auto position = _telli64(fd);
    if (position < 0 ||
        static_cast<std::uint64_t>(position) != extension.prefixBytes)
        return TailSave::Failed;
    std::uint32_t keptCount = 0;
    const std::vector<std::uint8_t> payload =
        EncodePayload(extension, keptCount);
    if (keptCount == 0) {
        // Nothing to export: no tail is written at all, so a save without
        // scale can never leave a stale scale block behind.
        return TailSave::Untouched;
    }
    if (payload.size() > kMaxTailBytes) return TailSave::Failed;
    std::vector<std::uint8_t> tail;
    tail.reserve(payload.size() + kFooterBytes);
    tail.insert(tail.end(), payload.begin(), payload.end());
    PutU64(tail, static_cast<std::uint64_t>(payload.size()));
    tail.insert(tail.end(), kFooterMagic, kFooterMagic + sizeof kFooterMagic);
    if (!WriteAll(fd, tail.data(), tail.size())) return TailSave::Failed;
    return TailSave::Written;
}

TailSave AppendTail(const std::wstring& path, const std::vector<Entry>& entries) {
    if (entries.empty()) return TailSave::Untouched;
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), 0x2 | 0x8000 /*_O_RDWR|_O_BINARY*/,
                  0x40 /*_SH_DENYNO*/, 0x80 | 0x100 /*_S_IREAD|_S_IWRITE*/) != 0 ||
        fd < 0)
        return TailSave::Failed;
    const __int64 length = _filelengthi64(fd);
    Extension extension;
    extension.entries = entries;
    std::uint32_t crc = 0;
    std::uint64_t newlines = 0;
    bool endsWithNewline = false;
    TailSave result = TailSave::Failed;
    if (length >= 0 &&
        Fingerprint(fd, static_cast<std::uint64_t>(length), crc, newlines,
                    endsWithNewline)) {
        extension.prefixBytes = static_cast<std::uint64_t>(length);
        extension.prefixCrc32 = crc;
        if (_lseeki64(fd, length, SEEK_SET) >= 0)
            result = AppendTail(fd, extension);
    }
    _close(fd);
    return result;
}

TailLoad LoadTail(const std::wstring& path, std::int64_t expectedRecords,
                  Extension& out) {
    out = {};
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), 0x8000 /*_O_BINARY*/,
                  0x40 /*_O_RDONLY*/, 0x80 /*_S_IREAD*/) != 0 || fd < 0)
        return TailLoad::Absent;
    const __int64 length = _filelengthi64(fd);
    if (length < static_cast<__int64>(kFooterBytes)) {
        _close(fd);
        return TailLoad::Absent;
    }
    if (_lseeki64(fd, length - static_cast<__int64>(kFooterBytes),
                  SEEK_SET) < 0) {
        _close(fd);
        return TailLoad::IgnoredBadFooter;
    }
    std::uint8_t footer[kFooterBytes] = {};
    if (_read(fd, footer, sizeof footer) != static_cast<int>(sizeof footer)) {
        _close(fd);
        return TailLoad::IgnoredBadFooter;
    }
    std::uint64_t payloadBytes = 0;
    std::memcpy(&payloadBytes, footer, sizeof payloadBytes);
    if (std::memcmp(footer + 8, kFooterMagic, sizeof kFooterMagic) != 0 &&
        std::memcmp(footer + 8, kLegacyFooterMagic,
                    sizeof kLegacyFooterMagic) != 0) {
        _close(fd);
        return TailLoad::Absent;  // an ordinary pose file: no tail at all
    }
    // The footer is ours from here on: every failure refuses the tail as a
    // whole and the standard pose keeps its default identity scales.
    if (payloadBytes > kMaxTailBytes ||
        payloadBytes + kFooterBytes > static_cast<std::uint64_t>(length)) {
        _close(fd);
        return TailLoad::IgnoredBadFooter;
    }
    const std::uint64_t prefixBytes =
        static_cast<std::uint64_t>(length) - payloadBytes - kFooterBytes;
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payloadBytes));
    if (_lseeki64(fd, static_cast<__int64>(prefixBytes), SEEK_SET) < 0 ||
        (!payload.empty() &&
         _read(fd, payload.data(),
               static_cast<unsigned int>(payload.size())) !=
             static_cast<int>(payload.size()))) {
        _close(fd);
        return TailLoad::IgnoredBadPayload;
    }
    Extension decoded;
    const Decode decode = DecodePayload(payload.data(), payload.size(), decoded);
    if (decode == Decode::BadVersion) {
        _close(fd);
        return TailLoad::IgnoredBadVersion;
    }
    if (decode == Decode::BadCrc) {
        _close(fd);
        return TailLoad::IgnoredBadCrc;
    }
    if (decode != Decode::Ok) {
        _close(fd);
        return TailLoad::IgnoredBadPayload;
    }
    // The payload must sit exactly between the standard text and the footer.
    if (decoded.prefixBytes != prefixBytes) {
        _close(fd);
        return TailLoad::IgnoredBadPayload;
    }
    // The standard text must still be the bytes it was written with, or the
    // tail is stale (a re-saved or edited pose) and its scales are wrong.
    std::uint32_t prefixCrc = 0;
    std::uint64_t newlines = 0;
    bool endsWithNewline = false;
    const bool fingerprinted =
        Fingerprint(fd, prefixBytes, prefixCrc, newlines, endsWithNewline);
    _close(fd);
    if (!fingerprinted) return TailLoad::IgnoredBadPayload;
    if (prefixCrc != decoded.prefixCrc32) return TailLoad::IgnoredBadCrc;
    // The tail must sit exactly where the loader's own read of the standard
    // segment ended: 5 header lines plus 5 lines per pose record, the last
    // one ending on a line end.
    if (expectedRecords >= 0) {
        const std::uint64_t expectedLines =
            5ull + 5ull * static_cast<std::uint64_t>(expectedRecords);
        if (newlines != expectedLines || prefixBytes == 0 || !endsWithNewline)
            return TailLoad::IgnoredBoundary;
    }
    out = std::move(decoded);
    return TailLoad::Loaded;
}

const char* Describe(TailLoad status) {
    switch (status) {
        case TailLoad::Loaded:
            return "loaded";
        case TailLoad::Absent:
            return "absent (an ordinary pose file)";
        case TailLoad::IgnoredBoundary:
            return "ignored (the tail does not sit at the end of the standard "
                   "pose)";
        case TailLoad::IgnoredBadFooter:
            return "ignored (the footer size claim does not fit the file)";
        case TailLoad::IgnoredBadPayload:
            return "ignored (the payload is truncated, mis-sized, or holds "
                   "duplicate entries or an invalid matrix)";
        case TailLoad::IgnoredBadVersion:
            return "ignored (the payload version is not understood)";
        case TailLoad::IgnoredBadCrc:
            return "ignored (the payload or the standard pose fails its "
                   "checksum; the tail is stale or corrupt)";
    }
    return "ignored";
}

// ---- entry-to-bone resolution -------------------------------------------

std::vector<std::int32_t> Resolve(const std::vector<Entry>& entries,
                                  const std::vector<std::array<char, 20>>& boneNames,
                                  const std::vector<std::uint8_t>& touched) {
    std::vector<std::int32_t> result(entries.size(), -1);
    std::vector<std::uint8_t> claimed(boneNames.size(), 0);
    for (std::size_t e = 0; e < entries.size(); ++e) {
        const Entry& entry = entries[e];
        std::int32_t target = -1;
        if (entry.bone >= 0 &&
            static_cast<std::size_t>(entry.bone) < boneNames.size() &&
            FieldEquals(boneNames[static_cast<std::size_t>(entry.bone)].data(),
                        boneNames[static_cast<std::size_t>(entry.bone)].size(),
                        entry.name.data(), entry.name.size())) {
            // 1. The recorded mapping still holds: the index names the entry.
            //    This is what keeps duplicate bone names apart on the model
            //    the pose was saved from.
            target = entry.bone;
        } else {
            // 2. The mapping drifted: the name resolves the entry only when
            //    it matches exactly one bone.  No match is a missing bone,
            //    several matches are ambiguous - both are skipped, never
            //    guessed.
            std::int32_t match = -1;
            int matches = 0;
            for (std::size_t i = 0; i < boneNames.size(); ++i) {
                if (!FieldEquals(boneNames[i].data(), boneNames[i].size(),
                                 entry.name.data(), entry.name.size()))
                    continue;
                ++matches;
                match = static_cast<std::int32_t>(i);
            }
            target = matches == 1 ? match : -1;
        }
        if (target < 0) continue;
        const std::size_t index = static_cast<std::size_t>(target);
        // 3. First entry wins on a claimed bone, and an entry may only land
        //    on a bone the standard segment itself touched.
        if (claimed[index] != 0) continue;
        if (index >= touched.size() || touched[index] == 0) continue;
        claimed[index] = 1;
        result[e] = target;
    }
    return result;
}

}  // namespace vpd_scale_io
}  // namespace mikudancestudio
