// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Regressions for the PMM 0003 block container codec ("PMM3END" footer,
// "PMM3TOC" table), see include/mikudancestudio/pmm_container.hpp and
// docs/reference/pmm-0003-container.md.
//
// Pure CPU: no window, no GPU, no MMDApp, no other project module.  The
// tests build the wire bytes and CRCs with their own helpers so the on-disk
// layout is pinned independently of the codec under test.
#include "mikudancestudio/pmm_container.hpp"

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pmmc = mikudancestudio::pmm_container;

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
// Independent wire-format helpers: the layout is pinned here, not derived from
// the module under test.
// ---------------------------------------------------------------------------

std::uint32_t RefCrc32(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t byte : bytes) {
        crc ^= byte;
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

std::uint32_t GetU32At(const std::vector<std::uint8_t>& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 4) return 0xDEADDEADu;
    return static_cast<std::uint32_t>(bytes[pos]) |
           (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
}

std::uint64_t GetU64At(const std::vector<std::uint8_t>& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 8)
        return 0xDEADDEADDEADDEADull;
    return static_cast<std::uint64_t>(GetU32At(bytes, pos)) |
           (static_cast<std::uint64_t>(GetU32At(bytes, pos + 4)) << 32);
}

std::vector<std::uint8_t> BytesOf(const char* text) {
    return std::vector<std::uint8_t>(text, text + std::strlen(text));
}

// Standard PMM prefix: 30 bytes beginning with the 0003 magic.  The codec does
// not parse this prefix; the content only has to be at least 30 bytes.
std::vector<std::uint8_t> StandardPrefix() {
    std::vector<std::uint8_t> bytes(30, 0);
    const char magic[] = "Polygon Movie maker 0003";
    std::memcpy(bytes.data(), magic, sizeof magic - 1);
    return bytes;
}

std::vector<std::uint8_t> LegacyTrailer(std::uint64_t standardSize) {
    std::vector<std::uint8_t> bytes;
    for (char c : std::string("MDSXPMM2")) bytes.push_back(static_cast<std::uint8_t>(c));
    PutU64(bytes, standardSize);
    PutU64(bytes, 0);  // extension size
    PutU32(bytes, 0);  // extension CRC
    PutU32(bytes, 0);  // reserved
    return bytes;
}

class TempFile {
public:
    explicit TempFile(const char* tag) {
        const char* dir = std::getenv("TEMP");
        if (dir == nullptr || *dir == '\0') dir = ".";
        static int counter = 0;
        path_ = std::string(dir) + "\\pmm_container_test_" + tag + "_" +
                std::to_string(_getpid()) + "_" + std::to_string(counter++) +
                ".bin";
        fd_ = _open(path_.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_TRUNC,
                    _S_IREAD | _S_IWRITE);
    }
    ~TempFile() {
        if (fd_ >= 0) _close(fd_);
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
        AppendBytes(bytes);
    }

    std::vector<std::uint8_t> ReadWhole() {
        const __int64 length = _filelengthi64(fd_);
        std::vector<std::uint8_t> bytes(
            length > 0 ? static_cast<std::size_t>(length) : 0);
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
    __int64 Length() const { return _filelengthi64(fd_); }

private:
    std::string path_;
    int fd_ = -1;
};

pmmc::Block MakeBlock(std::string key, std::vector<std::uint8_t> payload,
                      std::uint32_t version = 1, std::uint32_t flags = 0,
                      pmmc::Encoding encoding = pmmc::Encoding::Binary) {
    pmmc::Block block;
    block.key = std::move(key);
    block.version = version;
    block.flags = flags;
    block.encoding = encoding;
    block.payload = std::move(payload);
    return block;
}

bool WriteContainer(TempFile& file, const pmmc::Document& document) {
    file.WriteWhole(StandardPrefix());
    _lseeki64(file.fd(), 0, SEEK_END);
    return pmmc::Write(file.fd(), document);
}

pmmc::ReadResult ReadContainer(TempFile& file, pmmc::Document& document,
                               std::uint64_t& standardSize) {
    file.Seek(0);
    return pmmc::Read(file.fd(), document, standardSize);
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

void TestAbsentPrefixOnly() {
    TempFile file("absent");
    Check(file.ok(), "create absent fixture");
    file.WriteWhole(StandardPrefix());
    pmmc::Document document;
    std::uint64_t standardSize = 123;
    Check(ReadContainer(file, document, standardSize) == pmmc::ReadResult::Absent,
          "a bare PMM prefix has no container");
    Check(document.blocks.empty() && standardSize == 0,
          "Absent clears the outputs");
}

void TestRoundTrip() {
    TempFile file("roundtrip");
    Check(file.ok(), "create roundtrip fixture");
    pmmc::Document input;
    input.blocks.push_back(MakeBlock("org.example.binary",
                                     std::vector<std::uint8_t>{0, 1, 2, 255},
                                     3, 0x10, pmmc::Encoding::Binary));
    input.blocks.push_back(MakeBlock(
        "org.example.text",
        BytesOf("模型/路径"), 1, 0, pmmc::Encoding::Utf8));
    Check(WriteContainer(file, input), "write roundtrip container");

    pmmc::Document output;
    std::uint64_t standardSize = 0;
    Check(ReadContainer(file, output, standardSize) == pmmc::ReadResult::Loaded,
          "read roundtrip container");
    Check(standardSize == 30, "standardSize is the prefix length");
    Check(output.blocks.size() == 2, "both blocks roundtrip");
    const pmmc::Block* binary = pmmc::Find(output, "org.example.binary");
    const pmmc::Block* text = pmmc::Find(output, "org.example.text");
    Check(binary != nullptr && binary->version == 3 && binary->flags == 0x10 &&
              binary->encoding == pmmc::Encoding::Binary &&
              binary->payload == std::vector<std::uint8_t>({0, 1, 2, 255}),
          "binary block fields and payload roundtrip");
    Check(text != nullptr && text->encoding == pmmc::Encoding::Utf8 &&
              text->payload == BytesOf("模型/路径"),
          "UTF-8 block roundtrips without transcoding");
    const std::vector<std::uint8_t> bytes = file.ReadWhole();
    Check(bytes.size() > 64 &&
              std::memcmp(bytes.data() + bytes.size() - 64, "PMM3END", 7) == 0,
          "container ends with the PMM3END footer");
}

void TestUnknownBlockPreserved() {
    TempFile file("unknown");
    Check(file.ok(), "create unknown fixture");
    pmmc::Document input;
    input.blocks.push_back(MakeBlock("org.example.known",
                                     std::vector<std::uint8_t>{1, 2, 3}));
    input.blocks.push_back(MakeBlock(
        "org.example.unknown", std::vector<std::uint8_t>{9, 8, 7, 0, 255},
        7, 0x80000000u, pmmc::Encoding::Cp932));
    Check(WriteContainer(file, input), "write unknown fixture");

    pmmc::Document first;
    std::uint64_t standardSize = 0;
    Check(ReadContainer(file, first, standardSize) == pmmc::ReadResult::Loaded,
          "read unknown fixture");
    pmmc::Block* known = nullptr;
    for (pmmc::Block& block : first.blocks)
        if (block.key == "org.example.known") known = &block;
    Check(known != nullptr, "known block is present");
    if (known != nullptr) known->payload = {4, 5};

    TempFile rewritten("unknown_rewritten");
    Check(rewritten.ok(), "create rewritten fixture");
    Check(WriteContainer(rewritten, first), "rewrite with the unknown block");
    pmmc::Document second;
    Check(ReadContainer(rewritten, second, standardSize) ==
              pmmc::ReadResult::Loaded,
          "read rewritten container");
    const pmmc::Block* unknown = pmmc::Find(second, "org.example.unknown");
    Check(unknown != nullptr && unknown->version == 7 &&
              unknown->flags == 0x80000000u &&
              unknown->encoding == pmmc::Encoding::Cp932 &&
              unknown->payload ==
                  std::vector<std::uint8_t>({9, 8, 7, 0, 255}),
          "unknown block keepts its flag, encoding and payload bytes");
    const pmmc::Block* changed = pmmc::Find(second, "org.example.known");
    Check(changed != nullptr &&
              changed->payload == std::vector<std::uint8_t>({4, 5}),
          "known block receives the new payload");
}

void TestLegacyTrailerIsLegacy() {
    TempFile file("legacy");
    Check(file.ok(), "create legacy fixture");
    std::vector<std::uint8_t> bytes = StandardPrefix();
    const std::vector<std::uint8_t> trailer = LegacyTrailer(30);
    bytes.insert(bytes.end(), trailer.begin(), trailer.end());
    file.WriteWhole(bytes);
    pmmc::Document document;
    std::uint64_t standardSize = 0;
    Check(ReadContainer(file, document, standardSize) == pmmc::ReadResult::Legacy,
          "MDSXPMM2 is reported as Legacy");
    Check(document.blocks.empty() && standardSize == 30,
          "legacy standard size is returned");
}

void TestEmptyDocument() {
    TempFile file("empty");
    Check(file.ok(), "create empty fixture");
    pmmc::Document empty;
    Check(pmmc::Representable(empty), "an empty document is representable");
    Check(WriteContainer(file, empty), "write an empty container");
    pmmc::Document output;
    std::uint64_t standardSize = 0;
    Check(ReadContainer(file, output, standardSize) == pmmc::ReadResult::Loaded,
          "read an empty container");
    Check(output.blocks.empty(), "empty container has no blocks");
    Check(file.Length() == 30 + 24 + 64, "empty container is prefix + TOC + footer");
}

void TestExactLayout() {
    TempFile file("layout");
    Check(file.ok(), "create layout fixture");
    pmmc::Document input;
    const std::vector<std::uint8_t> payload{1, 2, 3};
    input.blocks.push_back(MakeBlock("k", payload, 2, 3, pmmc::Encoding::Utf8));
    Check(WriteContainer(file, input), "write layout fixture");
    const std::vector<std::uint8_t> bytes = file.ReadWhole();
    Check(bytes.size() == 30 + (32 + 1 + 3) + (24 + 40 + 1) + 64,
          "one-block container has the expected size");

    // Block header at 30, key at 62, payload at 63.
    Check(GetU32At(bytes, 30) == 1 && GetU32At(bytes, 34) == 2 &&
              GetU32At(bytes, 38) == 3 && GetU32At(bytes, 42) == 1,
          "block header key size/version/flags/encoding");
    Check(GetU64At(bytes, 46) == 3 && GetU32At(bytes, 54) == RefCrc32(payload),
          "block header payload size and CRC");
    Check(bytes[62] == 'k', "block key follows the header");
    Check(std::equal(payload.begin(), payload.end(), bytes.begin() + 63),
          "block payload follows the key");

    // TOC at 66 = 30 + 36, size 65, footer at 131.
    const std::size_t tocOffset = 66;
    Check(std::memcmp(bytes.data() + tocOffset, "PMM3TOC", 7) == 0,
          "TOC magic");
    Check(GetU32At(bytes, tocOffset + 8) == 1 &&
              GetU32At(bytes, tocOffset + 16) == 1,
          "TOC version and entry count");
    Check(GetU32At(bytes, tocOffset + 24) == 1 &&
              GetU32At(bytes, tocOffset + 28) == 2 &&
              GetU32At(bytes, tocOffset + 32) == 3 &&
              GetU32At(bytes, tocOffset + 36) == 1,
          "TOC entry copies block fields");
    Check(GetU64At(bytes, tocOffset + 40) == 30 &&
              GetU64At(bytes, tocOffset + 48) == 3,
          "TOC entry block offset and payload size");
    Check(GetU32At(bytes, tocOffset + 56) == RefCrc32(payload),
          "TOC entry payload CRC");

    const std::size_t footer = bytes.size() - 64;
    Check(std::memcmp(bytes.data() + footer, "PMM3END", 7) == 0,
          "footer magic");
    Check(GetU32At(bytes, footer + 8) == 1 &&
              GetU64At(bytes, footer + 16) == 30 &&
              GetU64At(bytes, footer + 24) == tocOffset &&
              GetU64At(bytes, footer + 32) == 65 &&
              GetU64At(bytes, footer + 40) == bytes.size(),
          "footer sizes");
    const std::vector<std::uint8_t> toc(bytes.begin() + tocOffset,
                                        bytes.begin() + footer);
    Check(GetU32At(bytes, footer + 48) == RefCrc32(toc), "footer TOC CRC");
}

void TestInvalidFooterAndToc() {
    {
        TempFile file("bad_footer_version");
        pmmc::Document input;
        input.blocks.push_back(MakeBlock("k", {1}));
        Check(WriteContainer(file, input), "write bad-footer fixture");
        file.PatchByte(file.Length() - 64 + 8, 2);
        pmmc::Document document;
        std::uint64_t standardSize = 0;
        Check(ReadContainer(file, document, standardSize) ==
                  pmmc::ReadResult::Invalid,
              "unsupported footer version is rejected");
    }
    {
        TempFile file("bad_total");
        pmmc::Document input;
        input.blocks.push_back(MakeBlock("k", {1}));
        Check(WriteContainer(file, input), "write bad-total fixture");
        file.PatchByte(file.Length() - 64 + 40, 0);
        pmmc::Document document;
        std::uint64_t standardSize = 0;
        Check(ReadContainer(file, document, standardSize) ==
                  pmmc::ReadResult::Invalid,
              "footer total size mismatch is rejected");
    }
    {
        TempFile file("bad_toc_crc");
        pmmc::Document input;
        input.blocks.push_back(MakeBlock("k", {1}));
        Check(WriteContainer(file, input), "write bad-toc-crc fixture");
        file.PatchByte(file.Length() - 64 - 65 + 24, 0xFF);
        pmmc::Document document;
        std::uint64_t standardSize = 0;
        Check(ReadContainer(file, document, standardSize) ==
                  pmmc::ReadResult::Invalid,
              "TOC CRC mismatch is rejected");
    }
    {
        TempFile file("bad_payload_crc");
        pmmc::Document input;
        input.blocks.push_back(MakeBlock("k", {1, 2, 3}));
        Check(WriteContainer(file, input), "write bad-payload-crc fixture");
        file.PatchByte(30 + 32 + 1, 0xFF);
        pmmc::Document document;
        std::uint64_t standardSize = 0;
        Check(ReadContainer(file, document, standardSize) ==
                  pmmc::ReadResult::Invalid,
              "payload CRC mismatch is rejected");
    }
}

void TestRepresentableAndKeys() {
    pmmc::Document document;
    document.blocks.push_back(MakeBlock("dup", {1}));
    document.blocks.push_back(MakeBlock("dup", {2}));
    Check(!pmmc::Representable(document), "duplicate keys are not representable");
    Check(!pmmc::Write(-1, document), "Write rejects duplicate keys before I/O");

    document.blocks.clear();
    document.blocks.push_back(MakeBlock(std::string(255, 'a'), {1}));
    Check(pmmc::Representable(document), "255-byte key is representable");
    document.blocks[0].key = std::string(256, 'a');
    Check(!pmmc::Representable(document), "256-byte key is rejected");
    document.blocks[0].key.clear();
    Check(!pmmc::Representable(document), "empty key is rejected");
    document.blocks[0].key = std::string("a\0b", 3);
    Check(!pmmc::Representable(document), "NUL in key is rejected");

    pmmc::Document doc;
    pmmc::Set(doc, MakeBlock("a", {1}));
    pmmc::Set(doc, MakeBlock("b", {2}));
    pmmc::Set(doc, MakeBlock("a", {3}));
    Check(doc.blocks.size() == 2 && pmmc::Find(doc, "a")->payload ==
                                        std::vector<std::uint8_t>({3}),
          "Set upserts in place");
    Check(pmmc::Erase(doc, "a") && pmmc::Find(doc, "a") == nullptr &&
              doc.blocks.size() == 1,
          "Erase removes a block");
    Check(!pmmc::Erase(doc, "missing"), "Erase reports a missing key");
}

}  // namespace

int main() {
    TestAbsentPrefixOnly();
    TestRoundTrip();
    TestUnknownBlockPreserved();
    TestLegacyTrailerIsLegacy();
    TestEmptyDocument();
    TestExactLayout();
    TestInvalidFooterAndToc();
    TestRepresentableAndKeys();

    if (g_failures == 0) {
        std::printf("pmm_container_tests: all %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "pmm_container_tests: %d of %d checks failed\n",
                 g_failures, g_checks);
    return 1;
}
