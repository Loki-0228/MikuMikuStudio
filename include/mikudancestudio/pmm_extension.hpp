// PMM v2 tail extension codec ("MMSXPMM2" trailer; legacy "MDSXPMM2" is read).
//
// Standalone module: no MMDApp, no Win32 UI, no GPU, no other project headers.
// The PMM header stays "Polygon Movie maker 0002"; the extension is appended
// after the complete standard PMM data and never rewrites it.
//
// Wire layout of a file with an extension (all integers little-endian):
//
//   [standard PMM bytes: "Polygon Movie maker 0002" header + full body]
//   [extension record stream]
//   [trailer: exactly 32 bytes, always the last 32 bytes of the file]
//
// Record stream: repeated records of
//   uint32 keySize; uint32 valueSize; key[keySize]; value[valueSize];
// key is UTF-8, value is opaque.  A key is 1..255 bytes, contains no NUL byte
// and is case-sensitive; keys are unique within one document and duplicate
// keys are rejected.  The record stream grows to the representable address/file range; each
// individual value uses a 32-bit length.
//
// Trailer (32 bytes):
//   char     magic[8]       = "MMSXPMM2"
//   uint64   standardSize   = byte count of the standard PMM prefix
//   uint64   extensionSize  = byte count of the record stream
//   uint32   crc32          = IEEE CRC32 (reflected, poly 0xEDB88320) of the
//                            record stream bytes
//   uint32   reserved       = 0
//
// New writes use "MMSXPMM2".  Files written before the MDS -> MMS rename carry
// "MDSXPMM2" and are read with the same layout; Read accepts both markers.
// Append always writes the current "MMSXPMM2" marker.
//
// There is no global or per-block version number.  Extensions are namespaced
// keys (for example "mds.viewport.zoom"); records whose keys a reader does not
// know keep their original key bytes, value bytes and order.
//
// Every struct here is serialized field by field; no C++ struct is written to
// disk, so padding cannot leak into the file.

#pragma once

#include <cstdint>
#include <limits>
#include <cstddef>
#include <string>
#include <vector>

namespace mikudancestudio {
namespace pmm_extension {

struct Record {
    std::string key;
    std::vector<std::uint8_t> value;
};

struct Document {
    std::vector<Record> records;
};

enum class ReadResult {
    Absent,   // no trailer magic at end of file: the file has no extension
    Loaded,   // trailer found, validated and parsed
    Invalid,  // magic matched but reserved, sizes, CRC or records are wrong
    IoError,  // seek or read failed
};

inline constexpr std::uint32_t kTrailerSize = 32;
inline constexpr std::uint32_t kMaxKeyBytes = 255;
inline constexpr std::size_t kMaxRecordCount = (std::numeric_limits<std::size_t>::max)() / sizeof(Record);
inline constexpr std::uint64_t kMaxExtensionBytes = static_cast<std::uint64_t>((std::numeric_limits<std::ptrdiff_t>::max)());
inline constexpr std::uint64_t kMinStandardSize = 30;

// Reads the extension trailer from the end of the file behind `fd`, which must
// be an open CRT descriptor in binary mode.  On Loaded, `out` receives the
// parsed records in file order and `standardSize` receives the byte count of
// the standard PMM prefix.  On Absent, Invalid and IoError `out` is cleared,
// `standardSize` is left unchanged and no partial parse reaches the caller.
// The file position of `fd` is restored to its entry value on every result.
ReadResult Read(int fd, Document& out, std::uint64_t& standardSize);

// Appends the record stream of `document` and the 32-byte trailer at the end of
// the file behind `fd`.  The descriptor must be positioned at end of file
// (`_telli64(fd)` equal to `_filelengthi64(fd)`); that position becomes
// standardSize and the standard prefix is never rewritten.  Returns false
// without writing when the position is not end of file, the standard part is
// shorter than kMinStandardSize, or the document is not representable (bad or
// duplicate key, more than kMaxRecordCount records, more than
// kMaxExtensionBytes).  Returns false after a failed or short write.  An empty
// document writes an empty record stream with CRC32 0.
bool Append(int fd, const Document& document);

// Checks keys, duplicates, record count and total size without opening a file.
// Save callers use this before truncating the existing standard PMM prefix.
bool Representable(const Document& document);

// Case-sensitive exact lookup; returns nullptr when the key is absent.  The
// pointer refers into `document` and stays valid until it is modified.
const Record* Find(const Document& document, const std::string& key);

// Upsert: replaces the value of the record with `key` in place (its position in
// the document is kept) or appends a new record at the end.  Keys that could
// never be serialized (empty, longer than kMaxKeyBytes, containing a NUL byte)
// are ignored so that a Document stays representable for Append.
void Set(Document& document, const std::string& key, std::vector<std::uint8_t> value);

// Removes the record with `key`; true when one was removed, false when absent.
bool Erase(Document& document, const std::string& key);

}  // namespace pmm_extension
}  // namespace mikudancestudio
