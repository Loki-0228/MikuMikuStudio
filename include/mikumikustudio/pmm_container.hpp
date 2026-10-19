// PMM 0003 block container codec ("PMM3END" footer + "PMM3TOC" table).
//
// Format reference: docs/reference/pmm-0003-container.md.
//
// Standalone module: no MMDApp, no Win32 UI, no GPU, and no dependency on
// pmm_extension.  The standard PMM prefix is written by the caller; this
// codec only reads or writes the container that follows it.
//
// Wire layout:
//   [standard PMM prefix, caller-written]  (standardSize)
//   [blocks: header + key + payload] ...
//   [TOC: "PMM3TOC" + entries]
//   [footer: "PMM3END" + sizes + CRCs]     (last 64 bytes)
//
// A container is absent when the last 64 bytes are not "PMM3END"; a trailing
// "MMSXPMM2" (or legacy "MDSXPMM2") extension is reported as
// ReadResult::Legacy so the caller can use the existing extension codec.
// Unknown blocks are caller-owned: this codec preserves their key, version,
// flags, encoding and payload bytes exactly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mikumikustudio::pmm_container {

// Text convention for a block payload.  New writers use Binary or Utf8.
enum class Encoding : std::uint32_t {
    Binary = 0,
    Utf8 = 1,
    Utf16Le = 2,
    Cp932 = 3,  // legacy import only; new writers do not emit it
};

struct Block {
    std::string key;                       // 1..255 UTF-8 bytes, no NUL
    std::uint32_t version = 1;
    std::uint32_t flags = 0;
    Encoding encoding = Encoding::Binary;
    std::vector<std::uint8_t> payload;
};

struct Document {
    std::vector<Block> blocks;
};

enum class ReadResult {
    Absent,   // no "PMM3END" and no trailing extension marker
    Legacy,   // "MMSXPMM2" or legacy "MDSXPMM2"; use pmm_extension
    Loaded,   // valid "PMM3END" container
    Invalid,  // "PMM3END" matched but the container is malformed
};

inline constexpr std::uint64_t kFooterSize = 64;
inline constexpr std::uint64_t kMinimumStandardSize = 30;
inline constexpr std::uint32_t kContainerVersion = 1;
inline constexpr std::uint32_t kTocVersion = 1;
inline constexpr std::uint32_t kBlockHeaderSize = 32;
inline constexpr std::uint32_t kTocHeaderSize = 24;
inline constexpr std::uint32_t kTocEntrySize = 40;
inline constexpr std::uint32_t kMaxKeyBytes = 255;

// Reads the container from fd.  On Loaded, `document` receives every block in
// TOC order and `standardSize` receives the standard-prefix byte count.  On
// Legacy, `standardSize` receives the trailing extension's standard size.  On
// Absent the outputs are cleared.  On Invalid the container was recognized but
// rejected; `document` is cleared and `standardSize` is left unchanged.  The
// file position is restored on every result.
ReadResult Read(int fd, Document& document, std::uint64_t& standardSize);

// Appends the container to fd.  The descriptor must be positioned at end of
// file; the standard prefix ends at that position.  Blocks are sorted by key
// for deterministic output.  Returns false without completing the write when
// the document is not representable or a write fails.  The caller owns the
// temporary-file/rename policy.
bool Write(int fd, const Document& document);

// Checks keys, duplicates, counts and size arithmetic without opening a file.
bool Representable(const Document& document);

// Case-sensitive exact lookup; nullptr when absent.  The pointer stays valid
// until the document is modified.
const Block* Find(const Document& document, std::string_view key);

// Upsert: replaces a block with the same key in place, otherwise appends.
// Invalid keys are ignored so that a Document stays representable.
void Set(Document& document, Block block);

// Removes the block with `key`; true when one was removed.
bool Erase(Document& document, std::string_view key);

}  // namespace mikumikustudio::pmm_container
