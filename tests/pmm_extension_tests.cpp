// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Regressions for the PMM v2 tail extension codec ("MMSXPMM2" trailer; legacy
// "MDSXPMM2" is read), see include/mikumikustudio/pmm_extension.hpp and
// src/io/pmm_extension.cpp.
//
// Pure CPU: no window, no GPU, no MMDApp, no other project module.  Every
// temporary file is deleted by its TempFile guard on every path.  The tests
// build the wire bytes and the CRC32 values with their own helpers, so the
// on-disk layout (little-endian fields, no struct padding) is pinned
// independently of the module under test.
#include "mikumikustudio/pmm_extension.hpp"

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pmmx = mikumikustudio::pmm_extension;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

// ---------------------------------------------------------------------------
// Independent wire-format helpers.
// ---------------------------------------------------------------------------

const char kPmmHeader[] = "Polygon Movie maker 0002";  // 24 bytes, no NUL.

// Reference CRC32 (IEEE, reflected, poly 0xEDB88320), bit by bit.
std::uint32_t RefCrc32(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
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

std::uint32_t GetU32At(const std::vector<std::uint8_t>& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 4) {
        return 0xDEADDEADu;  // out-of-range reads stay visible but never crash
    }
    return static_cast<std::uint32_t>(bytes[pos]) |
           (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
}

std::uint64_t GetU64At(const std::vector<std::uint8_t>& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 8) {
        return 0xDEADDEADDEADDEADull;
    }
    return static_cast<std::uint64_t>(GetU32At(bytes, pos)) |
           (static_cast<std::uint64_t>(GetU32At(bytes, pos + 4)) << 32);
}

std::vector<std::uint8_t> BytesOf(const char* text) {
    return std::vector<std::uint8_t>(text, text + std::strlen(text));
}

pmmx::Record MakeRecord(const std::string& key, std::vector<std::uint8_t> value) {
    pmmx::Record record;
    record.key = key;
    record.value = std::move(value);
    return record;
}

// Standard PMM prefix of `total` bytes, starting with the 24-byte
// "Polygon Movie maker 0002" header.  `total` must be at least 24.
std::vector<std::uint8_t> StandardBytes(std::size_t total) {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), kPmmHeader, kPmmHeader + 24);
    while (bytes.size() < total) {
        bytes.push_back(static_cast<std::uint8_t>(0xA0 + (bytes.size() & 0x0F)));
    }
    return bytes;
}

std::vector<std::uint8_t> EncodeStream(const std::vector<pmmx::Record>& records) {
    std::vector<std::uint8_t> stream;
    for (const pmmx::Record& record : records) {
        PutU32(stream, static_cast<std::uint32_t>(record.key.size()));
        PutU32(stream, static_cast<std::uint32_t>(record.value.size()));
        stream.insert(stream.end(), record.key.begin(), record.key.end());
        stream.insert(stream.end(), record.value.begin(), record.value.end());
    }
    return stream;
}

// Low-level record writer that lets a test lie about the sizes.
void PushRawRecord(std::vector<std::uint8_t>& stream, std::uint32_t keySize,
                   std::uint32_t valueSize, const std::vector<std::uint8_t>& key,
                   const std::vector<std::uint8_t>& value) {
    PutU32(stream, keySize);
    PutU32(stream, valueSize);
    stream.insert(stream.end(), key.begin(), key.end());
    stream.insert(stream.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> EncodeTrailer(std::uint64_t standardSize, std::uint64_t extensionSize,
                                        std::uint32_t crc, std::uint32_t reserved) {
    std::vector<std::uint8_t> trailer;
    trailer.insert(trailer.end(), "MMSXPMM2", "MMSXPMM2" + 8);
    PutU64(trailer, standardSize);
    PutU64(trailer, extensionSize);
    PutU32(trailer, crc);
    PutU32(trailer, reserved);
    return trailer;
}

// prefix + record stream + trailer; the trailer claims the given sizes.
std::vector<std::uint8_t> ComposeFile(const std::vector<std::uint8_t>& prefix,
                                      const std::vector<std::uint8_t>& stream,
                                      std::uint64_t standardSize, std::uint64_t extensionSize,
                                      std::uint32_t crc, std::uint32_t reserved) {
    std::vector<std::uint8_t> bytes = prefix;
    bytes.insert(bytes.end(), stream.begin(), stream.end());
    const std::vector<std::uint8_t> trailer =
        EncodeTrailer(standardSize, extensionSize, crc, reserved);
    bytes.insert(bytes.end(), trailer.begin(), trailer.end());
    return bytes;
}

// A complete, self-consistent file with one extension record stream.
std::vector<std::uint8_t> ValidFile(const std::vector<std::uint8_t>& prefix,
                                    const std::vector<std::uint8_t>& stream) {
    return ComposeFile(prefix, stream, prefix.size(), stream.size(), RefCrc32(stream), 0);
}

// ---------------------------------------------------------------------------
// Temp file guard: created in %TEMP%, deleted on every exit path.
// ---------------------------------------------------------------------------

class TempFile {
public:
    explicit TempFile(const char* tag) {
        const char* dir = std::getenv("TEMP");
        if (dir == nullptr || *dir == '\0') {
            dir = ".";
        }
        static int counter = 0;
        path_ = std::string(dir) + "\\pmm_extension_test_" + tag + "_" +
                std::to_string(_getpid()) + "_" + std::to_string(counter++) + ".bin";
        fd_ = _open(path_.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_TRUNC,
                    _S_IREAD | _S_IWRITE);
    }

    ~TempFile() {
        if (fd_ >= 0) {
            _close(fd_);
        }
        _unlink(path_.c_str());
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    bool ok() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    void AppendBytes(const std::vector<std::uint8_t>& bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const int wrote = _write(fd_, bytes.data() + done,
                                     static_cast<unsigned int>(bytes.size() - done));
            if (wrote <= 0) {
                Check(false, "temp file write failed");
                return;
            }
            done += static_cast<std::size_t>(wrote);
        }
    }

    void WriteWhole(const std::vector<std::uint8_t>& bytes) {
        _lseeki64(fd_, 0, SEEK_SET);
        _chsize_s(fd_, 0);
        AppendBytes(bytes);  // the position stays at end of file, as Append needs
    }

    std::vector<std::uint8_t> ReadWhole() {
        const __int64 length = _filelengthi64(fd_);
        std::vector<std::uint8_t> bytes(length > 0 ? static_cast<std::size_t>(length) : 0);
        _lseeki64(fd_, 0, SEEK_SET);
        std::size_t done = 0;
        while (done < bytes.size()) {
            const int got = _read(fd_, bytes.data() + done,
                                  static_cast<unsigned int>(bytes.size() - done));
            if (got <= 0) {
                Check(false, "temp file read failed");
                break;
            }
            done += static_cast<std::size_t>(got);
        }
        return bytes;
    }

    void PatchByte(__int64 offset, std::uint8_t value) {
        _lseeki64(fd_, offset, SEEK_SET);
        _write(fd_, &value, 1);
    }

    void TruncateTo(__int64 length) { _chsize_s(fd_, length); }
    void Seek(__int64 offset) { _lseeki64(fd_, offset, SEEK_SET); }
    __int64 Tell() const { return _telli64(fd_); }
    __int64 Length() const { return _filelengthi64(fd_); }

private:
    std::string path_;
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

// A file without the trailer reads Absent, clears the document, keeps
// standardSize and restores the file position.
void TestNoTrailerIsAbsent() {
    {
        TempFile file("absent_plain");
        Check(file.ok(), "absent_plain: temp file created");
        file.WriteWhole(StandardBytes(64));
        pmmx::Document out;
        out.records.push_back(MakeRecord("stale", BytesOf("x")));
        std::uint64_t standardSize = 0xDEADull;
        file.Seek(5);
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Absent,
              "standard bytes only read Absent");
        Check(out.records.empty(), "Absent clears the document");
        Check(standardSize == 0xDEADull, "Absent leaves standardSize unchanged");
        Check(file.Tell() == 5, "Absent restores the file position");
    }
    {
        TempFile file("absent_empty");
        Check(file.ok(), "absent_empty: temp file created");
        pmmx::Document out;
        out.records.push_back(MakeRecord("stale", BytesOf("x")));
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Absent,
              "an empty file reads Absent");
        Check(out.records.empty(), "an empty file clears the document");
    }
    {
        TempFile file("absent_short");
        Check(file.ok(), "absent_short: temp file created");
        file.WriteWhole(StandardBytes(31));  // shorter than the 32-byte trailer
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Absent,
              "a file shorter than the trailer reads Absent");
    }
    {
        // Trailer bytes present, but not at end of file: no magic at EOF.
        TempFile file("absent_off");
        Check(file.ok(), "absent_off: temp file created");
        const std::vector<std::uint8_t> stream = EncodeStream({MakeRecord("k", {1})});
        std::vector<std::uint8_t> bytes =
            ComposeFile(StandardBytes(64), stream, 64, stream.size(), RefCrc32(stream), 0);
        bytes.push_back(0x01);
        bytes.push_back(0x02);
        bytes.push_back(0x03);
        file.WriteWhole(bytes);
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Absent,
              "a trailer that is not the last 32 bytes reads Absent");
    }
}

// Two settings plus one unknown record survive Append -> Read unchanged, the
// standard prefix is not rewritten, and the file position comes back.
void TestLegacyMarkerIsRead() {
    TempFile file("legacy_marker");
    Check(file.ok(), "create legacy marker fixture");
    const auto prefix = StandardBytes(55);
    std::vector<std::uint8_t> stream;
    const std::vector<std::uint8_t> key = BytesOf("legacy.key");
    const std::vector<std::uint8_t> value = BytesOf("legacy.value");
    PushRawRecord(stream, static_cast<std::uint32_t>(key.size()),
                  static_cast<std::uint32_t>(value.size()), key, value);
    std::vector<std::uint8_t> bytes = ValidFile(prefix, stream);
    std::memcpy(bytes.data() + bytes.size() - 32, "MDSXPMM2", 8);
    file.WriteWhole(bytes);

    pmmx::Document out;
    std::uint64_t standardSize = 0;
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
          "legacy MDSXPMM2 marker is read");
    Check(standardSize == prefix.size() && out.records.size() == 1 &&
              out.records[0].key == "legacy.key" &&
              out.records[0].value == value,
          "legacy marker records are decoded");
}

void TestRoundTripSettingsAndUnknown() {
    TempFile file("roundtrip");
    Check(file.ok(), "roundtrip: temp file created");
    const std::vector<std::uint8_t> prefix = StandardBytes(64);
    file.WriteWhole(prefix);

    pmmx::Document doc;
    doc.records.push_back(MakeRecord("mds.viewport.zoom", {0x10, 0x20}));
    doc.records.push_back(MakeRecord("mds.ui.language", BytesOf("en")));
    doc.records.push_back(MakeRecord("vendor.unknown.token", {0x00, 0xFF, 0x7F, 0x00}));
    const std::vector<std::uint8_t> stream = EncodeStream(doc.records);

    Check(pmmx::Append(file.fd(), doc), "Append writes settings and an unknown record");
    Check(file.Length() == 64 + static_cast<__int64>(stream.size()) + 32,
          "the file grows by records plus the 32-byte trailer");

    const std::vector<std::uint8_t> written = file.ReadWhole();
    Check(written.size() >= 64 &&
              std::memcmp(written.data(), prefix.data(), 64) == 0,
          "Append leaves the standard prefix untouched");

    pmmx::Document out;
    std::uint64_t standardSize = 0;
    file.Seek(13);
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
          "round trip reads Loaded");
    Check(standardSize == 64, "standardSize is the prefix size");
    const bool shape = out.records.size() == 3;
    Check(shape, "round trip returns three records");
    if (shape) {
        Check(out.records[0].key == "mds.viewport.zoom" &&
                  out.records[0].value == std::vector<std::uint8_t>{0x10, 0x20},
              "first setting survives the round trip");
        Check(out.records[1].key == "mds.ui.language" && out.records[1].value == BytesOf("en"),
              "second setting survives the round trip");
        Check(out.records[2].key == "vendor.unknown.token" &&
                  out.records[2].value == std::vector<std::uint8_t>{0x00, 0xFF, 0x7F, 0x00},
              "unknown record survives the round trip with its binary value");
    }

    const pmmx::Record* found = pmmx::Find(out, "mds.ui.language");
    Check(found != nullptr && found->value == BytesOf("en"), "Find returns the setting record");
    Check(pmmx::Find(out, "MDS.UI.LANGUAGE") == nullptr, "Find is case sensitive");
    Check(pmmx::Find(out, "mds.ui") == nullptr, "Find needs an exact key");
    Check(file.Tell() == 13, "Loaded restores the file position");
}

// Unknown keys keep their original key bytes, value bytes and order.
void TestUnknownOrderAndValuePreserved() {
    TempFile file("unknown_order");
    Check(file.ok(), "unknown_order: temp file created");
    file.WriteWhole(StandardBytes(64));

    pmmx::Document doc;
    doc.records.push_back(MakeRecord("zzz.last", {0xFF, 0x00, 0xFE}));
    doc.records.push_back(MakeRecord("aaa.first", {0x00}));
    doc.records.push_back(MakeRecord("Mm.Mixed", {0x7F, 0x80, 0x00, 0x01}));
    doc.records.push_back(MakeRecord("mds.ui.language", BytesOf("ja")));
    Check(pmmx::Append(file.fd(), doc), "Append keeps mixed-case and unknown keys");

    pmmx::Document out;
    std::uint64_t standardSize = 0;
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
          "unknown-order file reads Loaded");
    const bool shape = out.records.size() == 4;
    Check(shape, "unknown-order file returns four records");
    if (shape) {
        Check(out.records[0].key == "zzz.last" && out.records[1].key == "aaa.first" &&
                  out.records[2].key == "Mm.Mixed" && out.records[3].key == "mds.ui.language",
              "unknown keys keep their original order");
        Check(out.records[0].value == std::vector<std::uint8_t>{0xFF, 0x00, 0xFE} &&
                  out.records[1].value == std::vector<std::uint8_t>{0x00} &&
                  out.records[2].value == std::vector<std::uint8_t>{0x7F, 0x80, 0x00, 0x01} &&
                  out.records[3].value == BytesOf("ja"),
              "unknown keys keep their original value bytes");
    }
}

// The wire layout is exactly: uint32 keySize, uint32 valueSize, key, value,
// then the 32-byte trailer with little-endian fields and no padding.
void TestExactByteLayout() {
    TempFile file("layout");
    Check(file.ok(), "layout: temp file created");
    const std::vector<std::uint8_t> prefix = StandardBytes(40);
    file.WriteWhole(prefix);

    pmmx::Document doc;
    doc.records.push_back(MakeRecord("ab", {1, 2, 3}));
    Check(pmmx::Append(file.fd(), doc), "Append writes the single record");

    std::vector<std::uint8_t> expected = prefix;
    expected.push_back(0x02);  // keySize = 2, little-endian
    expected.push_back(0x00);
    expected.push_back(0x00);
    expected.push_back(0x00);
    expected.push_back(0x03);  // valueSize = 3, little-endian
    expected.push_back(0x00);
    expected.push_back(0x00);
    expected.push_back(0x00);
    expected.push_back('a');
    expected.push_back('b');
    expected.push_back(1);
    expected.push_back(2);
    expected.push_back(3);
    const std::uint32_t crc = RefCrc32(std::vector<std::uint8_t>(expected.begin() + 40,
                                                                 expected.end()));
    const std::vector<std::uint8_t> trailer = EncodeTrailer(40, 13, crc, 0);
    expected.insert(expected.end(), trailer.begin(), trailer.end());

    const std::vector<std::uint8_t> actual = file.ReadWhole();
    Check(actual.size() == expected.size(), "wire file has the exact expected size");
    Check(actual == expected, "wire bytes match the hand-built little-endian layout");
    Check(GetU64At(actual, actual.size() - 24) == 40, "trailer standardSize is little-endian");
    Check(GetU64At(actual, actual.size() - 16) == 13, "trailer extensionSize is little-endian");
    Check(GetU32At(actual, actual.size() - 8) == crc, "trailer crc32 is little-endian");
    Check(GetU32At(actual, actual.size() - 4) == 0, "trailer reserved is zero");
}

// An empty document writes an empty record stream with CRC32 0 and reads back
// as Loaded with zero records; a non-zero CRC on the empty stream is Invalid.
void TestEmptyDocument() {
    TempFile file("empty_doc");
    Check(file.ok(), "empty_doc: temp file created");
    file.WriteWhole(StandardBytes(64));

    pmmx::Document empty;
    Check(pmmx::Append(file.fd(), empty), "Append accepts an empty document");
    Check(file.Length() == 64 + 32, "an empty document writes only the trailer");

    const std::vector<std::uint8_t> written = file.ReadWhole();
    Check(GetU64At(written, 64 + 8) == 64, "empty document records standardSize = 64");
    Check(GetU64At(written, 64 + 16) == 0, "empty document records extensionSize = 0");
    Check(GetU32At(written, 64 + 24) == 0, "CRC32 of the empty record stream is 0");

    pmmx::Document out;
    std::uint64_t standardSize = 0;
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
          "empty document reads Loaded");
    Check(out.records.empty(), "empty document reads zero records");
    Check(standardSize == 64, "empty document keeps standardSize = 64");

    TempFile bad("empty_crc");
    Check(bad.ok(), "empty_crc: temp file created");
    const std::vector<std::uint8_t> stream;
    bad.WriteWhole(ComposeFile(StandardBytes(64), stream, 64, 0, 1, 0));
    out.records.push_back(MakeRecord("stale", BytesOf("x")));
    standardSize = 0;
    Check(pmmx::Read(bad.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
          "a wrong CRC on the empty record stream is Invalid");
    Check(out.records.empty(), "Invalid clears the document");
}

// A flipped record byte or a wrong trailer CRC32 is Invalid, and the file
// position is still restored.
void TestCrcErrors() {
    {
        TempFile file("crc_record");
        Check(file.ok(), "crc_record: temp file created");
        const std::vector<std::uint8_t> stream = EncodeStream({MakeRecord("crc.key", {1, 2, 3})});
        file.WriteWhole(ValidFile(StandardBytes(64), stream));
        file.PatchByte(64 + 8 + 7, 0x99);  // first value byte

        pmmx::Document out;
        out.records.push_back(MakeRecord("stale", BytesOf("x")));
        std::uint64_t standardSize = 0;
        file.Seek(7);
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "a flipped record byte is Invalid");
        Check(out.records.empty(), "Invalid clears the document instead of returning a parse");
        Check(standardSize == 0, "Invalid leaves standardSize unchanged");
        Check(file.Tell() == 7, "Invalid restores the file position");
    }
    {
        TempFile file("crc_trailer");
        Check(file.ok(), "crc_trailer: temp file created");
        const std::vector<std::uint8_t> stream = EncodeStream({MakeRecord("crc.key", {1, 2, 3})});
        file.WriteWhole(ValidFile(StandardBytes(64), stream));
        file.PatchByte(file.Length() - 8, 0x01);  // low byte of crc32

        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "a wrong trailer CRC32 is Invalid");
    }
}

// Trailer size errors: mismatched standardSize / extensionSize, standardSize
// below 30, extensionSize over 16 MiB, and the uint64 wrap-around trap.
void TestSizeErrors() {
    const std::vector<std::uint8_t> stream = EncodeStream({MakeRecord("k", {1})});

    {
        TempFile file("size_standard");
        Check(file.ok(), "size_standard: temp file created");
        file.WriteWhole(ComposeFile(StandardBytes(64), stream, 65, stream.size(),
                                    RefCrc32(stream), 0));
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "standardSize + extensionSize + 32 != file length is Invalid");
    }
    {
        TempFile file("size_extension");
        Check(file.ok(), "size_extension: temp file created");
        file.WriteWhole(ComposeFile(StandardBytes(64), stream, 64, stream.size() - 1,
                                    RefCrc32(stream), 0));
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "a short extensionSize claim is Invalid");
    }
    {
        // standardSize = 29 is below the minimum even when the length adds up.
        TempFile file("size_min");
        Check(file.ok(), "size_min: temp file created");
        file.WriteWhole(ComposeFile(StandardBytes(29), stream, 29, stream.size(),
                                    RefCrc32(stream), 0));
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "standardSize = 29 is Invalid");
    }
    {
        // standardSize = 30 is the smallest accepted standard part.
        TempFile file("size_min_ok");
        Check(file.ok(), "size_min_ok: temp file created");
        file.WriteWhole(ComposeFile(StandardBytes(30), stream, 30, stream.size(),
                                    RefCrc32(stream), 0));
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
              "standardSize = 30 is Loaded");
        Check(standardSize == 30, "standardSize = 30 is reported back");
    }
    {
        // extensionSize = 16 MiB + 1 is over the limit even with a matching
        // length and a correct CRC.
        TempFile file("size_max");
        Check(file.ok(), "size_max: temp file created");
        const std::size_t extension = 16u * 1024u * 1024u + 1u;
        const std::vector<std::uint8_t> zeros(extension, 0x00);
        std::vector<std::uint8_t> bytes = StandardBytes(30);
        bytes.insert(bytes.end(), zeros.begin(), zeros.end());
        const std::vector<std::uint8_t> trailer =
            EncodeTrailer(30, extension, RefCrc32(zeros), 0);
        bytes.insert(bytes.end(), trailer.begin(), trailer.end());
        file.WriteWhole(bytes);

        pmmx::Document out;
        out.records.push_back(MakeRecord("stale", BytesOf("x")));
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "extensionSize over 16 MiB is Invalid");
        Check(out.records.empty(), "the size-bound failure clears the document");
    }
    {
        // standardSize + extensionSize + 32 wraps to the real file length in
        // uint64 arithmetic; the wrap must be rejected before any read.
        TempFile file("size_wrap");
        Check(file.ok(), "size_wrap: temp file created");
        std::vector<std::uint8_t> bytes = StandardBytes(68);
        const std::vector<std::uint8_t> trailer =
            EncodeTrailer(0xFFFFFFFFFFFFFFE0ull, 100, 0, 0);  // (2^64-32) + 100 + 32 == 100 (mod 2^64)
        bytes.insert(bytes.end(), trailer.begin(), trailer.end());
        Check(bytes.size() == 100, "the wrap-around fixture is 100 bytes");
        file.WriteWhole(bytes);

        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "a uint64 size wrap-around is Invalid, not a false match");
    }
}

// Malformed record streams: duplicate key, bad keySize, NUL in key, truncated
// record header, valueSize past the end of the stream.
void TestRecordErrors() {
    struct Case {
        const char* name;
        std::vector<std::uint8_t> stream;
    };

    std::vector<Case> cases;

    {
        std::vector<std::uint8_t> s = EncodeStream({MakeRecord("dup", {1})});
        const std::vector<std::uint8_t> second = EncodeStream({MakeRecord("dup", {2})});
        s.insert(s.end(), second.begin(), second.end());
        cases.push_back({"duplicate key", s});
    }
    {
        std::vector<std::uint8_t> s;
        PushRawRecord(s, 0, 1, {}, {0x41});  // keySize = 0
        cases.push_back({"keySize 0", s});
    }
    {
        std::vector<std::uint8_t> s;
        const std::vector<std::uint8_t> key(256, 'a');
        PushRawRecord(s, 256, 0, key, {});  // keySize = 256
        cases.push_back({"keySize 256", s});
    }
    {
        std::vector<std::uint8_t> s;
        PushRawRecord(s, 3, 1, {'a', 0x00, 'b'}, {9});  // NUL inside the key
        cases.push_back({"NUL in key", s});
    }
    {
        std::vector<std::uint8_t> s;
        PutU32(s, 1);  // record header cut in half
        cases.push_back({"truncated record header", s});
    }
    {
        std::vector<std::uint8_t> s;
        PushRawRecord(s, 1, 0xFFFFFF00u, {'x'}, {});  // valueSize past the stream
        cases.push_back({"valueSize past the end", s});
    }
    {
        std::vector<std::uint8_t> s = EncodeStream({MakeRecord("ok", {1})});
        s.push_back(0x01);  // trailing partial header
        s.push_back(0x02);
        s.push_back(0x03);
        cases.push_back({"trailing partial header", s});
    }

    for (const Case& testCase : cases) {
        TempFile file("record_error");
        Check(file.ok(), std::string(testCase.name) + ": temp file created");
        file.WriteWhole(ValidFile(StandardBytes(64), testCase.stream));
        pmmx::Document out;
        out.records.push_back(MakeRecord("stale", BytesOf("x")));
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              std::string(testCase.name) + " is Invalid");
        Check(out.records.empty(), std::string(testCase.name) + " clears the document");
    }
}

// 16384 records are accepted, 16385 are rejected on both sides.
void TestRecordCountLimit() {
    {
        TempFile file("count_ok");
        Check(file.ok(), "count_ok: temp file created");
        file.WriteWhole(StandardBytes(64));
        pmmx::Document doc;
        for (std::uint32_t i = 0; i < 16384; ++i) {
            doc.records.push_back(MakeRecord("k" + std::to_string(i), {0x01}));
        }
        Check(pmmx::Append(file.fd(), doc), "Append accepts 16384 records");

        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
              "16384 records read Loaded");
        const bool shape = out.records.size() == 16384;
        Check(shape, "16384 records come back");
        if (shape) {
            Check(out.records.front().key == "k0" && out.records.back().key == "k16383",
                  "the 16384 records keep their order");
        }
    }
    {
        TempFile file("count_over");
        Check(file.ok(), "count_over: temp file created");
        pmmx::Document doc;
        for (std::uint32_t i = 0; i < 16385; ++i) {
            doc.records.push_back(MakeRecord("k" + std::to_string(i), {0x01}));
        }
        const std::vector<std::uint8_t> stream = EncodeStream(doc.records);
        file.WriteWhole(ValidFile(StandardBytes(64), stream));

        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded && out.records.size()==16385,
              "16385 records expand automatically");
    }
}

// The extension may fill the 16 MiB limit exactly.
void TestExtensionSizeBoundary() {
    TempFile file("ext_boundary");
    Check(file.ok(), "ext_boundary: temp file created");
    file.WriteWhole(StandardBytes(64));

    pmmx::Document doc;
    const std::size_t valueSize = 16u * 1024u * 1024u - 8u - 1u;  // stream total == 16 MiB
    std::vector<std::uint8_t> value(valueSize, 0x5A);
    value.back() = 0x77;
    doc.records.push_back(MakeRecord("k", value));
    Check(pmmx::Append(file.fd(), doc), "Append accepts an extension of exactly 16 MiB");

    pmmx::Document out;
    std::uint64_t standardSize = 0;
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
          "an extension of exactly 16 MiB reads Loaded");
    const bool shape = out.records.size() == 1;
    Check(shape, "the 16 MiB record comes back");
    if (shape) {
        Check(out.records[0].value.size() == valueSize, "the 16 MiB value keeps its size");
        Check(out.records[0].value.front() == 0x5A && out.records[0].value.back() == 0x77,
              "the 16 MiB value keeps its bytes");
    }
}

// reserved must be zero; anything else is Invalid.
void TestReservedMustBeZero() {
    const std::vector<std::uint8_t> stream = EncodeStream({MakeRecord("k", {1})});
    {
        TempFile file("reserved_low");
        Check(file.ok(), "reserved_low: temp file created");
        file.WriteWhole(ValidFile(StandardBytes(64), stream));
        file.PatchByte(file.Length() - 4, 0x01);
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "reserved = 1 is Invalid");
    }
    {
        TempFile file("reserved_high");
        Check(file.ok(), "reserved_high: temp file created");
        file.WriteWhole(ValidFile(StandardBytes(64), stream));
        file.PatchByte(file.Length() - 1, 0x80);  // reserved = 0x80000000
        pmmx::Document out;
        std::uint64_t standardSize = 0;
        Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Invalid,
              "reserved = 0x80000000 is Invalid");
    }
}

// A file truncated through the trailer has no magic at EOF and reads Absent.
void TestTrailerTruncationIsAbsent() {
    TempFile file("truncated");
    Check(file.ok(), "truncated: temp file created");
    const std::vector<std::uint8_t> stream = EncodeStream({MakeRecord("k", {1})});
    const std::vector<std::uint8_t> bytes = ValidFile(StandardBytes(64), stream);
    file.WriteWhole(bytes);
    file.TruncateTo(static_cast<__int64>(bytes.size()) - 4);

    pmmx::Document out;
    out.records.push_back(MakeRecord("stale", BytesOf("x")));
    std::uint64_t standardSize = 0;
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Absent,
          "a file truncated through the trailer reads Absent");
    Check(out.records.empty(), "the truncated file clears the document");
}

// Set upserts in place, Erase removes, both are case sensitive, and keys that
// cannot be serialized are ignored.
void TestSetEraseFind() {
    pmmx::Document doc;
    Check(pmmx::Find(doc, "a") == nullptr, "Find on an empty document returns nullptr");
    Check(!pmmx::Erase(doc, "a"), "Erase on an empty document returns false");

    pmmx::Set(doc, "a", BytesOf("1"));
    Check(doc.records.size() == 1 && doc.records[0].key == "a",
          "Set appends a record for a new key");
    pmmx::Set(doc, "b", BytesOf("2"));
    Check(doc.records.size() == 2, "Set appends a second record");

    pmmx::Set(doc, "a", BytesOf("3"));
    Check(doc.records.size() == 2, "Set on an existing key adds no record");
    if (doc.records.size() == 2) {
        Check(doc.records[0].key == "a" && doc.records[0].value == BytesOf("3"),
              "Set replaces the value and keeps the record position");
        Check(doc.records[1].key == "b" && doc.records[1].value == BytesOf("2"),
              "Set leaves other records alone");
    }

    const pmmx::Record* found = pmmx::Find(doc, "a");
    Check(found != nullptr && found->value == BytesOf("3"), "Find returns the upserted record");
    Check(pmmx::Find(doc, "A") == nullptr, "Find is case sensitive");
    Check(!pmmx::Erase(doc, "B"), "Erase is case sensitive");

    Check(pmmx::Erase(doc, "b"), "Erase removes the record");
    Check(doc.records.size() == 1 && doc.records[0].key == "a", "Erase keeps the other records");
    Check(!pmmx::Erase(doc, "b"), "Erase of an absent key returns false");

    pmmx::Set(doc, std::string(), BytesOf("x"));
    pmmx::Set(doc, std::string(256, 'k'), BytesOf("x"));
    pmmx::Set(doc, std::string("a\0b", 3), BytesOf("x"));
    Check(doc.records.size() == 1, "Set ignores keys that cannot be serialized");

    // The edited document still round trips through the file format.
    TempFile file("set_erase");
    Check(file.ok(), "set_erase: temp file created");
    file.WriteWhole(StandardBytes(64));
    Check(pmmx::Append(file.fd(), doc), "the edited document appends");
    pmmx::Document out;
    std::uint64_t standardSize = 0;
    Check(pmmx::Read(file.fd(), out, standardSize) == pmmx::ReadResult::Loaded,
          "the edited document reads back Loaded");
    Check(out.records.size() == 1 && out.records[0].key == "a" &&
              out.records[0].value == BytesOf("3"),
          "the edited document round trips its final state");
}

// Append refuses documents it cannot serialize and refuses descriptors that are
// not at end of file; every rejection writes nothing.
void TestAppendRejects() {
    TempFile file("append_rejects");
    Check(file.ok(), "append_rejects: temp file created");
    file.WriteWhole(StandardBytes(64));
    const __int64 before = file.Length();

    pmmx::Document dup;
    dup.records.push_back(MakeRecord("k", {1}));
    dup.records.push_back(MakeRecord("k", {2}));
    Check(!pmmx::Append(file.fd(), dup), "Append rejects duplicate keys");

    pmmx::Document emptyKey;
    emptyKey.records.push_back(MakeRecord(std::string(), {1}));
    Check(!pmmx::Append(file.fd(), emptyKey), "Append rejects an empty key");

    pmmx::Document nulKey;
    nulKey.records.push_back(MakeRecord(std::string("a\0b", 3), {1}));
    Check(!pmmx::Append(file.fd(), nulKey), "Append rejects a key containing NUL");

    pmmx::Document longKey;
    longKey.records.push_back(MakeRecord(std::string(256, 'k'), {1}));
    Check(!pmmx::Append(file.fd(), longKey), "Append rejects a key longer than 255 bytes");

    pmmx::Document many;
    for (std::uint32_t i = 0; i < 16385; ++i) {
        many.records.push_back(MakeRecord("k" + std::to_string(i), {1}));
    }
    TempFile manyFile("expanded_count"); manyFile.WriteWhole(StandardBytes(64));
    Check(pmmx::Append(manyFile.fd(), many), "Append expands beyond 16384 records");
    pmmx::Document manyRead; std::uint64_t manyBoundary=0;
    Check(pmmx::Read(manyFile.fd(),manyRead,manyBoundary)==pmmx::ReadResult::Loaded && manyRead.records.size()==many.records.size(), "expanded record count roundtrip");

    pmmx::Document huge;
    huge.records.push_back(
        MakeRecord("k", std::vector<std::uint8_t>(16u * 1024u * 1024u + 1u, 0x00)));
    TempFile hugeFile("expanded_size"); hugeFile.WriteWhole(StandardBytes(64));
    Check(pmmx::Append(hugeFile.fd(), huge), "Append expands beyond 16 MiB");
    Check(pmmx::Representable(huge), "save preflight accepts representable expanded data");
    pmmx::Document hugeRead; std::uint64_t hugeBoundary=0;
    Check(pmmx::Read(hugeFile.fd(),hugeRead,hugeBoundary)==pmmx::ReadResult::Loaded && hugeRead.records.size()==1 && hugeRead.records[0].value==huge.records[0].value, "expanded payload roundtrip");

    Check(file.Length() == before, "every rejected Append writes nothing");

    pmmx::Document good;
    good.records.push_back(MakeRecord("k", {1}));
    Check(pmmx::Representable(good), "save preflight accepts representable extension");
    file.Seek(10);
    Check(!pmmx::Append(file.fd(), good), "Append rejects a descriptor not at end of file");
    Check(file.Length() == before, "the not-at-end rejection writes nothing");
    file.Seek(before);

    TempFile short_file("append_short");
    Check(short_file.ok(), "append_short: temp file created");
    short_file.WriteWhole(StandardBytes(24));  // only the PMM header
    Check(!pmmx::Append(short_file.fd(), good),
          "Append rejects a standard part shorter than 30 bytes");
}

}  // namespace

int main() {
    TestNoTrailerIsAbsent();
    TestLegacyMarkerIsRead();
    TestRoundTripSettingsAndUnknown();
    TestUnknownOrderAndValuePreserved();
    TestExactByteLayout();
    TestEmptyDocument();
    TestCrcErrors();
    TestSizeErrors();
    TestRecordErrors();
    TestRecordCountLimit();
    TestExtensionSizeBoundary();
    TestReservedMustBeZero();
    TestTrailerTruncationIsAbsent();
    TestSetEraseFind();
    TestAppendRejects();

    if (g_failures == 0) {
        std::printf("pmm_extension_tests: all %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "pmm_extension_tests: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
