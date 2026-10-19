// MAINTENANCE: regression-keep; VPD pose-file bone scale tail extension
// (include/mikudancestudio/vpd_scale_io.hpp, src/io/vpd_scale_io.cpp).
//
// What is pinned here:
//   - the wire layout of the tail (footer "MMSXVPDX", payload "MMSXVPDS",
//     with the legacy MDSXVPDX/MDSXVPDS forms still accepted)
//     little-endian fields, 32-byte payload header, 60-byte entries, trailing
//     CRC32), built byte by byte with the test's own helpers so the layout is
//     independent of the module under test;
//   - the standard VPD text stays byte-for-byte the stock text and the tail is
//     appended after the loader's end position (5 header lines plus 5 lines
//     per pose record);
//   - old format handling: a file without the footer loads as an ordinary
//     pose (no entries, every touched bone stays on the default identity
//     scale) and a save without scale never revives an older tail;
//   - corruption handling: truncation, size/count mismatch, footer and payload
//     forgeries, future versions, NaN/Inf/reflected matrices and duplicate
//     entries all refuse the tail as a whole;
//   - entry-to-bone resolution: missing bones, duplicate names, stale index
//     mappings, the out-of-scope rule and first-entry-wins.
//
// Only the pure codec is exercised: no window, no GPU, no model record.  Every
// temporary file is created under %TEMP% and deleted by its guard on every
// exit path.  Run with an optional section name (roundtrip, old-format,
// corruption, resolution) to produce that section's log alone.
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "mikudancestudio/vpd_scale_io.hpp"

namespace bs = mikudancestudio::bone_scale;
namespace vpd = mikudancestudio::vpd_scale_io;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

// ---------------------------------------------------------------------------
// Independent wire-format helpers.
// ---------------------------------------------------------------------------

// Reference CRC32 (IEEE, reflected, poly 0xEDB88320), bit by bit.
std::uint32_t RefCrc32(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

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

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(ReadU32(bytes, offset)) |
           (static_cast<std::uint64_t>(ReadU32(bytes, offset + 4)) << 32);
}

bs::Matrix Matrix(std::initializer_list<float> nine) {
    bs::Matrix result;
    int index = 0;
    for (float value : nine) {
        if (index < 9) result.v[static_cast<std::size_t>(index)] = value;
        ++index;
    }
    return result;
}

std::array<char, 20> Name20(const char* name) {
    std::array<char, 20> field{};
    for (std::size_t i = 0; i < field.size() && name[i] != '\0'; ++i)
        field[i] = name[i];
    return field;
}

vpd::Entry Entry(std::int32_t bone, const char* name, const bs::Matrix& scale) {
    vpd::Entry entry;
    entry.bone = bone;
    entry.name = Name20(name);
    entry.scale = scale;
    return entry;
}

// The 60-byte entry block exactly as the writer must lay it out:
// int32 bone, 20-byte name field, nine little-endian float32.
std::vector<std::uint8_t> RawEntry(std::int32_t bone, const char* name,
                                   const bs::Matrix& scale) {
    std::vector<std::uint8_t> out;
    PutU32(out, static_cast<std::uint32_t>(bone));
    const std::array<char, 20> field = Name20(name);
    out.insert(out.end(), field.begin(), field.end());
    for (float v : scale.v) PutF32(out, v);
    Check(out.size() == 60, "independent entry block is 60 bytes");
    return out;
}

// A hand-built payload: everything explicit, so corruption tests can forge
// fields the encoder would never write.  `declaredCount` may lie about the
// number of entry blocks that follow.
std::vector<std::uint8_t> HandPayload(
    const char* magic, std::uint32_t version, std::uint32_t reserved,
    std::uint64_t prefixBytes, std::uint32_t prefixCrc,
    std::uint32_t declaredCount,
    const std::vector<std::vector<std::uint8_t>>& entries, bool withCrc = true) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(magic[i]));
    PutU32(out, version);
    PutU32(out, reserved);
    PutU64(out, prefixBytes);
    PutU32(out, prefixCrc);
    PutU32(out, declaredCount);
    for (const std::vector<std::uint8_t>& entry : entries)
        out.insert(out.end(), entry.begin(), entry.end());
    if (withCrc) PutU32(out, RefCrc32(out));
    return out;
}

std::vector<std::uint8_t> TailFile(const std::vector<std::uint8_t>& text,
                                   const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes = text;
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    PutU64(bytes, static_cast<std::uint64_t>(payload.size()));
    const char footer[] = "MDSXVPDX";
    bytes.insert(bytes.end(), footer, footer + 8);
    return bytes;
}

// The standard VPD text the stock writer produces: 5 header lines plus 5
// lines per pose record - exactly where the loader's read stops.
std::vector<std::uint8_t> StandardText(int records, bool crlf = false) {
    const char* eol = crlf ? "\r\n" : "\n";
    std::string text = std::string("Vocaloid Pose Data file") + eol + eol;
    text += std::string("sample.osm;\t\t// parent file") + eol;
    text += std::to_string(records) +
            std::string(";\t\t\t\t// total pose bones") + eol + eol;
    for (int i = 0; i < records; ++i) {
        text += "Bone" + std::to_string(i) + "{bone" + std::to_string(i) + eol;
        text += "  0.000000,0.000000,0.000000;\t\t\t\t// trans x,y,z" +
                std::string(eol);
        text += "  0.000000,0.000000,0.000000,1.000000;\t\t// Quaternion "
                "x,y,z,w" + std::string(eol);
        text += std::string("}") + eol + eol;
    }
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// ---------------------------------------------------------------------------
// Temp file under %TEMP%, deleted by its guard on every exit path.
// ---------------------------------------------------------------------------

struct TempFile {
    std::wstring path;
    TempFile() {
        wchar_t directory[MAX_PATH] = {};
        wchar_t name[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, directory);
        GetTempFileNameW(directory, L"vpd", 0, name);
        path = name;
    }
    ~TempFile() { DeleteFileW(path.c_str()); }
    bool Write(const std::vector<std::uint8_t>& bytes) {
        int fd = -1;
        if (_wsopen_s(&fd, path.c_str(),
                      0x2 | 0x8000 | 0x200 | 0x1000 /*_O_RDWR|_O_BINARY|_O_TRUNC|_O_CREAT*/,
                      0x40 /*_SH_DENYNO*/, 0x80 | 0x100) != 0 || fd < 0)
            return false;
        const int wrote = bytes.empty()
                              ? 0
                              : _write(fd, bytes.data(),
                                       static_cast<unsigned int>(bytes.size()));
        _close(fd);
        return wrote == static_cast<int>(bytes.size());
    }
    std::vector<std::uint8_t> Read() {
        std::vector<std::uint8_t> bytes;
        int fd = -1;
        if (_wsopen_s(&fd, path.c_str(), 0x8000, 0x40, 0x80) != 0 || fd < 0)
            return bytes;
        const __int64 length = _filelengthi64(fd);
        bytes.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
        if (!bytes.empty() &&
            _read(fd, bytes.data(), static_cast<unsigned int>(bytes.size())) !=
                static_cast<int>(bytes.size()))
            bytes.clear();
        _close(fd);
        return bytes;
    }
};

// The reference payload of the two-entry extension used across sections.
const bs::Matrix kShear = Matrix({1.2f, 0.1f, 0.0f, 0.2f, 0.9f, 0.0f, 0.0f, 0.0f, 1.1f});
const bs::Matrix kWide = Matrix({2.0f, 0, 0, 0, 1.0f, 0, 0, 0, 0.5f});

std::vector<vpd::Entry> TwoEntries() {
    return {Entry(3, "bone3", kShear), Entry(7, "\x93\x8b\x91\x52", kWide)};
}

// ---------------------------------------------------------------------------
// Round trip: wire layout pinning and encode/decode symmetry.
// ---------------------------------------------------------------------------

void WireLayoutAndRoundTrip() {
    const std::vector<std::uint8_t> text = StandardText(2);
    vpd::Extension extension;
    extension.prefixBytes = text.size();
    extension.prefixCrc32 = RefCrc32(text);
    extension.entries = TwoEntries();

    const std::vector<std::uint8_t> payload = vpd::Encode(extension);
    Check(payload.size() == 32 + 60 * 2 + 4,
          "payload is 32 header + 2 * 60 entries + 4 CRC bytes");
    Check(std::memcmp(payload.data(), "MMSXVPDS", 8) == 0,
          "payload magic is MMSXVPDS");
    Check(ReadU32(payload, 8) == 1, "payload version is 1");
    Check(ReadU32(payload, 12) == 0, "payload reserved field is zero");
    Check(ReadU64(payload, 16) == extension.prefixBytes,
          "payload records the standard text byte count");
    Check(ReadU32(payload, 24) == extension.prefixCrc32,
          "payload records the standard text CRC32");
    Check(ReadU32(payload, 28) == 2, "payload records its entry count");
    const std::vector<std::uint8_t> raw0 =
        RawEntry(3, "bone3", kShear);
    const std::vector<std::uint8_t> raw1 =
        RawEntry(7, "\x93\x8b\x91\x52", kWide);
    Check(std::memcmp(payload.data() + 32, raw0.data(), 60) == 0,
          "first entry block matches the independent 60-byte layout");
    Check(std::memcmp(payload.data() + 92, raw1.data(), 60) == 0,
          "second entry block matches the independent 60-byte layout");
    std::vector<std::uint8_t> body(payload.begin(), payload.end() - 4);
    Check(ReadU32(payload, payload.size() - 4) == RefCrc32(body),
          "trailing CRC32 covers every earlier payload byte");

    vpd::Extension decoded;
    Check(vpd::DecodePayload(payload.data(), payload.size(), decoded) ==
              vpd::Decode::Ok,
          "payload decodes");
    Check(decoded.prefixBytes == extension.prefixBytes &&
              decoded.prefixCrc32 == extension.prefixCrc32,
          "prefix fingerprint round-trips");
    Check(decoded.entries.size() == 2, "both entries round-trip");
    if (decoded.entries.size() == 2) {
        Check(decoded.entries[0].bone == 3 &&
                  decoded.entries[0].name == Name20("bone3") &&
                  decoded.entries[0].scale.v == kShear.v,
              "entry 0 round-trips bone, name and all nine floats");
        Check(decoded.entries[1].bone == 7 &&
                  decoded.entries[1].name == Name20("\x93\x8b\x91\x52") &&
                  decoded.entries[1].scale.v == kWide.v,
              "entry 1 round-trips bone, name and all nine floats");
    }
    // A matrix with shear travels as nine floats: no reduction, no struct
    // dump, and the re-encoded bytes are identical.
    Check(vpd::Encode(decoded) == payload, "encode(decode(payload)) is identical");

    // Writer rules: identity and invalid matrices are not samples, and a
    // duplicate (bone, name) pair keeps its first entry only.
    vpd::Extension dropped;
    dropped.prefixBytes = 17;
    dropped.prefixCrc32 = 23;
    dropped.entries.push_back(Entry(1, "bone1", bs::Matrix{}));  // identity
    dropped.entries.push_back(
        Entry(1, "bone1", Matrix({std::numeric_limits<float>::quiet_NaN(), 1, 0,
                                  0, 1, 0, 0, 0, 1})));           // invalid
    dropped.entries.push_back(Entry(2, "bone2", kShear));         // kept
    dropped.entries.push_back(Entry(2, "bone2", kWide));          // duplicate
    dropped.entries.push_back(Entry(2, "bone3", kWide));          // kept
    const std::vector<std::uint8_t> small = vpd::Encode(dropped);
    Check(ReadU32(small, 28) == 2,
          "encoder drops identity, invalid and duplicate entries");
    vpd::Extension smallDecoded;
    Check(vpd::DecodePayload(small.data(), small.size(), smallDecoded) ==
              vpd::Decode::Ok &&
              smallDecoded.entries.size() == 2,
          "the surviving two entries decode");
    if (smallDecoded.entries.size() == 2) {
        Check(smallDecoded.entries[0].scale.v == kShear.v &&
                  smallDecoded.entries[1].name == Name20("bone3"),
              "the first of a duplicate pair survives with its own matrix");
    }
    std::printf("roundtrip: wire layout and codec symmetry pinned\n");
}

// ---------------------------------------------------------------------------
// Old format: a VPD without the tail, and the file lifecycle around it.
// ---------------------------------------------------------------------------

void OldFormatAndFileLifecycle() {
    const std::vector<std::uint8_t> text = StandardText(2);
    TempFile file;
    Check(file.Write(text), "write plain standard VPD");

    vpd::Extension loaded;
    Check(vpd::LoadTail(file.path, 2, loaded) == vpd::TailLoad::Absent &&
              loaded.entries.empty(),
          "a VPD without the tail is an ordinary pose (identity scale, no "
          "entries to apply)");
    Check(vpd::AppendTail(file.path, {}) == vpd::TailSave::Untouched &&
              file.Read() == text,
          "a save without scale writes no tail and leaves the file untouched");

    Check(vpd::AppendTail(file.path, TwoEntries()) == vpd::TailSave::Written,
          "tail appended after the standard text");
    const std::vector<std::uint8_t> full = file.Read();
    Check(full.size() == text.size() + 32 + 60 * 2 + 4 + 16,
          "file is standard text + payload + 16-byte footer");
    Check(std::memcmp(full.data(), text.data(), text.size()) == 0,
          "the standard VPD text is byte-for-byte unchanged and keeps its end "
          "position");
    const std::size_t footerAt = full.size() - 16;
    Check(ReadU64(full, footerAt) == full.size() - text.size() - 16,
          "footer records the payload byte count");
    Check(std::memcmp(full.data() + footerAt + 8, "MMSXVPDX", 8) == 0,
          "footer magic is MMSXVPDX and sits at the very end");

    Check(vpd::LoadTail(file.path, 2, loaded) == vpd::TailLoad::Loaded &&
              loaded.entries.size() == 2,
          "tail round-trips through the file");
    Check(loaded.prefixBytes == text.size() &&
              loaded.prefixCrc32 == RefCrc32(text),
          "loaded prefix fingerprint matches the standard text");
    Check(vpd::LoadTail(file.path, 3, loaded) == vpd::TailLoad::IgnoredBoundary &&
              loaded.entries.empty(),
          "a tail that does not sit at the loader's end position is refused");
    Check(vpd::LoadTail(file.path, -1, loaded) == vpd::TailLoad::Loaded,
          "passing -1 relies on the tail alone");

    // CRLF text: the line accounting must survive the carriage returns.
    const std::vector<std::uint8_t> crlf = StandardText(3, true);
    Check(file.Write(crlf) &&
              vpd::AppendTail(file.path, TwoEntries()) == vpd::TailSave::Written,
          "tail appended to a CRLF standard text");
    Check(vpd::LoadTail(file.path, 3, loaded) == vpd::TailLoad::Loaded,
          "CRLF standard text still lands the tail at the loader's end "
          "position");

    // A save without scale must not revive the scales of an older file.
    Check(file.Write(text), "rewrite the file as a plain pose");
    Check(vpd::LoadTail(file.path, 2, loaded) == vpd::TailLoad::Absent &&
              loaded.entries.empty(),
          "an ordinary save cannot revive old scales");

    const std::wstring missing = file.path + L".missing";
    Check(vpd::LoadTail(missing, 2, loaded) == vpd::TailLoad::Absent,
          "an unreadable path behaves like a pose without a tail");
    std::printf("old-format: no-tail files stay stock, tail files keep the "
                "stock text untouched\n");
}

// ---------------------------------------------------------------------------
// Corruption: every failure refuses the tail as a whole.
// ---------------------------------------------------------------------------

void CorruptionRejects() {
    const std::vector<std::uint8_t> text = StandardText(2);
    vpd::Extension extension;
    extension.prefixBytes = text.size();
    extension.prefixCrc32 = RefCrc32(text);
    extension.entries = TwoEntries();
    const std::vector<std::uint8_t> payload = vpd::Encode(extension);
    const std::vector<std::uint8_t> good = TailFile(text, payload);
    TempFile file;
    vpd::Extension loaded;

    std::vector<std::uint8_t> broken = good;
    broken[text.size() + 30] ^= 0x40;  // the entry-count field
    Check(file.Write(broken) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadPayload,
          "a forged entry count is refused by the length formula before any "
          "entry is read");
    broken = good;
    broken[text.size() + 40] ^= 0x40;  // inside the first entry block
    Check(file.Write(broken) &&
              vpd::LoadTail(file.path, 2, loaded) == vpd::TailLoad::IgnoredBadCrc,
          "a forged payload byte fails the payload checksum");
    broken = good;
    broken[10] ^= 0x20;  // inside the standard text
    Check(file.Write(broken) &&
              vpd::LoadTail(file.path, 2, loaded) == vpd::TailLoad::IgnoredBadCrc,
          "changed standard bytes make the tail stale and it is refused");

    broken = TailFile(text, payload);
    broken[broken.size() - 16] = 0xFF;
    broken[broken.size() - 15] = 0xFF;  // footer size claim over the file
    Check(file.Write(broken) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadFooter,
          "a footer size that does not fit the file is refused");

    broken = TailFile(text, payload);
    broken[broken.size() - 1] ^= 0x01;  // footer magic
    Check(file.Write(broken) &&
              vpd::LoadTail(file.path, 2, loaded) == vpd::TailLoad::Absent,
          "a file whose last bytes are not the footer magic is an ordinary "
          "pose");

    broken = good;
    broken.resize(broken.size() - 4);  // cut into the footer
    Check(file.Write(broken) && vpd::LoadTail(file.path, 2, loaded) !=
                                    vpd::TailLoad::Loaded,
          "a truncated tail never applies scales");
    broken = good;
    broken.erase(broken.begin() + static_cast<std::ptrdiff_t>(text.size()) + 5,
                 broken.begin() + static_cast<std::ptrdiff_t>(text.size()) + 15);
    Check(file.Write(broken) &&
              vpd::LoadTail(file.path, 2, loaded) != vpd::TailLoad::Loaded,
          "a payload cut short under an intact footer is refused");

    const std::vector<std::uint8_t> raw =
        RawEntry(3, "bone3", kShear);
    std::vector<std::vector<std::uint8_t>> one{raw};

    Check(file.Write(TailFile(text, HandPayload("MDSXVPDS", 2, 0, text.size(),
                                                RefCrc32(text), 1, one))) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadVersion,
          "a future payload version is refused as a whole, never read as "
          "version 1");
    Check(file.Write(TailFile(text, HandPayload("MDSXVPDS", 0, 0, text.size(),
                                                RefCrc32(text), 1, one))) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadVersion,
          "version 0 is unknown too");
    Check(file.Write(TailFile(text, HandPayload("MDSXVPDS", 1, 1, text.size(),
                                                RefCrc32(text), 1, one))) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadPayload,
          "a non-zero reserved field is refused");
    Check(file.Write(TailFile(text, HandPayload("XXXXXXXX", 1, 0, text.size(),
                                                RefCrc32(text), 1, one))) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadPayload,
          "a foreign payload magic is refused");

    const bs::Matrix nan = Matrix({std::numeric_limits<float>::quiet_NaN(), 0, 0,
                                   0, 1, 0, 0, 0, 1});
    const bs::Matrix infinite = Matrix({1, 0, 0, 0,
                                        std::numeric_limits<float>::infinity(), 0,
                                        0, 0, 1});
    const bs::Matrix reflected = Matrix({-1, 0, 0, 0, 1, 0, 0, 0, 1});
    const bs::Matrix collapsed = Matrix({0, 0, 0, 0, 1, 0, 0, 0, 1});
    const bs::Matrix* bad[4] = {&nan, &infinite, &reflected, &collapsed};
    const char* badName[4] = {"NaN", "Inf", "negative determinant",
                              "collapsed determinant"};
    for (int i = 0; i < 4; ++i) {
        std::vector<std::vector<std::uint8_t>> entries{
            RawEntry(3, "bone3", *bad[i])};
        std::vector<std::uint8_t> forged =
            HandPayload("MDSXVPDS", 1, 0, text.size(), RefCrc32(text), 1, entries);
        vpd::Extension direct;
        Check(vpd::DecodePayload(forged.data(), forged.size(), direct) ==
                  vpd::Decode::BadData,
              badName[i]);
        std::string description =
            std::string("a ") + badName[i] +
            " matrix refuses the whole tail, nothing is half applied";
        Check(file.Write(TailFile(text, forged)) &&
                  vpd::LoadTail(file.path, 2, loaded) ==
                      vpd::TailLoad::IgnoredBadPayload,
              description.c_str());
    }

    std::vector<std::vector<std::uint8_t>> duplicates{
        RawEntry(3, "bone3", kShear), RawEntry(3, "bone3", kWide)};
    Check(file.Write(TailFile(text, HandPayload("MDSXVPDS", 1, 0, text.size(),
                                                RefCrc32(text), 2, duplicates))) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadPayload,
          "duplicate (bone, name) entries refuse the whole tail");

    // Count and length forgeries must fail before any entry is read.
    Check(vpd::DecodePayload(payload.data(), payload.size() - 5, loaded) ==
              vpd::Decode::BadData,
          "a payload cut below its own length formula is refused");
    std::vector<std::uint8_t> lying = HandPayload("MDSXVPDS", 1, 0, text.size(),
                                                  RefCrc32(text), 3, one);
    Check(vpd::DecodePayload(lying.data(), lying.size(), loaded) ==
              vpd::Decode::BadData,
          "an entry count that breaks the length formula is refused");
    std::vector<std::uint8_t> huge = HandPayload("MDSXVPDS", 1, 0, text.size(),
                                                 RefCrc32(text), 0xFFFFFFFFu,
                                                 one);
    Check(vpd::DecodePayload(huge.data(), huge.size(), loaded) ==
              vpd::Decode::BadData,
          "a count of 0xFFFFFFFF cannot make the reader leave its buffer");
    Check(vpd::DecodePayload(nullptr, 0, loaded) == vpd::Decode::BadData,
          "an empty buffer is refused");

    // A payload that lies about where the standard text ends is refused even
    // though its own bytes are consistent.
    std::vector<std::vector<std::uint8_t>> oneEntry{raw};
    Check(file.Write(TailFile(
              text, HandPayload("MDSXVPDS", 1, 0, text.size() + 1,
                                RefCrc32(text), 1, oneEntry))) &&
              vpd::LoadTail(file.path, 2, loaded) ==
                  vpd::TailLoad::IgnoredBadPayload,
          "payload and footer boundaries must agree");
    std::printf("corruption: every forged tail is refused as a whole\n");
}

// ---------------------------------------------------------------------------
// Resolution: missing bones, duplicate names, stale mappings, scope.
// ---------------------------------------------------------------------------

void ResolutionRules() {
    // Five bones; index 1 and 3 share the name "腕".
    std::vector<std::array<char, 20>> names{
        Name20("bone0"), Name20("\x93\x8b"), Name20("bone2"),
        Name20("\x93\x8b"), Name20("bone4")};
    std::vector<std::uint8_t> touched(5, 1);

    std::vector<vpd::Entry> entries{
        Entry(2, "bone2", kShear),
        Entry(3, "\x93\x8b", kWide),
        Entry(1, "\x93\x8b", kShear),
    };
    std::vector<std::int32_t> targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 3 && targets[0] == 2 && targets[1] == 3 &&
              targets[2] == 1,
          "an intact index+name mapping lands each entry on its own bone, "
          "duplicate names included");

    entries = {Entry(5, "\x93\x8b", kShear)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 1 && targets[0] == -1,
          "duplicate names without the recorded index are ambiguous and "
          "skipped, never guessed");

    entries = {Entry(9, "nosuch", kShear)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 1 && targets[0] == -1,
          "an entry whose bone is missing from the model is skipped");

    entries = {Entry(5, "bone2", kShear)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 1 && targets[0] == 2,
          "a drifted mapping still transfers by the stable name when the name "
          "resolves to exactly one bone");

    entries = {Entry(-1, "bone4", kWide)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 1 && targets[0] == 4,
          "the name is the stable key even without a recorded index");

    std::vector<std::uint8_t> nothing(5, 0);
    entries = {Entry(2, "bone2", kShear)};
    targets = vpd::Resolve(entries, names, nothing);
    Check(targets.size() == 1 && targets[0] == -1,
          "an entry outside the standard segment's scope is skipped");

    entries = {Entry(5, "bone2", kShear), Entry(2, "bone2", kWide)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 2 && targets[0] == 2 && targets[1] == -1,
          "the first entry wins when two entries claim one bone");

    std::vector<std::uint8_t> shortTouched(3, 1);
    entries = {Entry(4, "bone4", kShear)};
    targets = vpd::Resolve(entries, names, shortTouched);
    Check(targets.size() == 1 && targets[0] == -1,
          "a bone outside the touched table counts as out of scope");

    entries = {Entry(1, "\x93\x8b", kShear), Entry(3, "\x93\x8b", kWide)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 2 && targets[0] == 1 && targets[1] == 3,
          "two same-named bones keep their own scales through their indices");

    // No resolvable entry: the caller applies nothing and every touched bone
    // stays on the default identity scale - the no-residue rule.
    entries = {Entry(9, "nosuch", kShear), Entry(5, "\x93\x8b", kWide)};
    targets = vpd::Resolve(entries, names, touched);
    Check(targets.size() == 2 && targets[0] == -1 && targets[1] == -1,
          "an all-skipped tail leaves every touched bone on identity scale");
    std::printf("resolution: missing, ambiguous, stale and out-of-scope "
                "entries are explicit\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* section = argc > 1 ? argv[1] : "";
    const bool all = argc <= 1;
    if (all || std::strcmp(section, "roundtrip") == 0)
        WireLayoutAndRoundTrip();
    if (all || std::strcmp(section, "old-format") == 0)
        OldFormatAndFileLifecycle();
    if (all || std::strcmp(section, "corruption") == 0)
        CorruptionRejects();
    if (all || std::strcmp(section, "resolution") == 0)
        ResolutionRules();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %d checks (VPD bone scale tail extension)\n", g_checks);
    return 0;
}
