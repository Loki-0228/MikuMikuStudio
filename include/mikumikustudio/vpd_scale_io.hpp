// Persistence for native bone scaling in VPD pose files (vpd_file.cpp).
//
// The standard VPD text stays byte-for-byte what the stock writer produces and
// ends at exactly the same place; the pose scales ride in a self-describing
// extension appended after it and located by a fixed footer.  The stock MMD
// 9.32 pose loader stops after the last standard pose record and closes the
// file without an EOF check (the port keeps the same read sequence in
// LoadVpdFile), so the tail is invisible to it.  A VPD without the tail is an
// ordinary pose file: every bone the pose touches falls back to the default
// identity scale exactly as before, so a load can never inherit the residual
// scale of the previous pose.
//
// File layout (all integers little-endian, fields written one by one, no C++
// struct ever dumped to disk):
//
//   [standard VPD text]                           prefixBytes
//   [extension payload]                           payloadBytes
//   [footer: uint64 payloadBytes;
//            char magic[8] = "MMSXVPDX"]          (16 bytes)
//            (legacy "MDSXVPDX" is also read)
//
//   A file is "just a standard VPD" when the last 8 bytes are not the footer
//   magic; a file with the footer must satisfy
//   prefixBytes + payloadBytes + 16 == file size, where prefixBytes comes from
//   the payload.
//
// Extension payload:
//   char     magic[8]    = "MMSXVPDS"
//   uint32   version     = 1
//   uint32   reserved    = 0
//   uint64   prefixBytes = byte count of the standard VPD text
//   uint32   prefixCrc32 = IEEE CRC32 of all standard VPD bytes
//   uint32   entryCount
//   entryCount x { int32 bone; char name[20]; float m[9]; }   (60 bytes)
//   uint32   crc32       = IEEE CRC32 of every preceding payload byte
//
//   `bone` is the model bone index at save time and `name` the NUL-padded
//   SJIS bone name (the BoneRecord::name field), the same "name next to the
//   index" identity the PMM pose records use (bone_scale_io.hpp).  `m` is the
//   bone_scale::Matrix pose scale, nine floats row-major.  A pose record
//   carries no interpolation curve: a curve describes the interval between
//   two timeline keys and a pose has no timeline, so this channel is version 1
//   only and a payload whose version this reader does not know is refused
//   whole, never reinterpreted as the newest known layout.
//
//   The version sizes the entries (60 bytes) and the payload length is exact:
//   32 + 60 * entryCount + 4.  A payload that does not fit that formula is
//   refused rather than read with the wrong stride, so the reader can never
//   step outside the buffer.
//
// Writer rules (mirroring the VMD extension encoder):
//   With no scale to export no tail is written at all, and a save without a
//   tail can never revive the scales of an older file.  An entry is written
//   for each bone the standard pose segment writes (the selected bones) whose
//   pose scale is a real, non-identity matrix; the identity pose scale is the
//   default every touched bone falls back to anyway.  A matrix that is not
//   finite, not invertible or out of the accepted bounds is not a sample and
//   is dropped, exactly like the VMD encoder drops an invalid matrix.  Two
//   entries with the same (bone, name) pair would be ambiguous: the encoder
//   keeps the first and drops the rest, and the decoder refuses a payload
//   that contains such a duplicate at all.  Two entries may share a name when
//   their bone indices differ - a model with duplicate bone names is a real
//   case and both bones keep their own scale.
//
// Reader rules, split along the line the existing extensions draw:
//   Wire-level problems refuse the whole tail and leave the standard pose
//   untouched: truncation, a payload size that breaks the length formula,
//   count overflow, a wrong payload or prefix CRC, an unknown version or
//   magic, duplicate (bone, name) entries, and any matrix that is not finite
//   with a positive determinant (NaN/Inf included).  The tail is then ignored
//   with a runtime log entry and every touched bone keeps the default
//   identity scale - nothing is ever half applied.
//   Model-mapping problems skip just that entry: a bone index outside the
//   model, a name that no longer matches the recorded index, a name that
//   matches no bone ("missing bone") and a name that matches more than one
//   bone without the recorded index disambiguating it ("duplicate names") are
//   never guessed at.  An entry may only land on a bone the standard segment
//   itself touched, so the tail can never scale a bone the pose did not
//   select and the load's undo, registration and selection scope stay exactly
//   what the standard loader set.
//
// Entry-to-bone resolution (Resolve), applied per entry in file order:
//   1. The recorded mapping still holds: 0 <= bone < bone count and the bone
//      at that index carries exactly the entry's name.  The entry lands on
//      that index - this is what disambiguates duplicate names on the model
//      it was saved from.
//   2. The mapping drifted (a different model, or bones were inserted or
//      removed): the name resolves the entry when it matches exactly one
//      bone.  No match is a missing bone and several matches are ambiguous;
//      both skip the entry.
//   3. A target bone already claimed by an earlier entry is skipped (first
//      entry wins), and a target the standard segment did not touch is
//      skipped as out of scope.
//   Every skipped entry leaves its bone on the default identity scale.
//
// Standard boundary verification: the loader knows where its own parse of the
// standard text ended - 5 lines of header plus 5 lines per pose record - and
// LoadTail checks that the tail sits exactly there.  A tail that claims a
// different boundary is ignored.  The check counts line ends, so a bone or
// model name that itself contains a byte 0x0A breaks the accounting; such a
// file keeps its pose but its tail is ignored (the stock text format cannot
// carry such a name either).
//
// Everything in this header is deterministic CPU code with its own matrix
// validation, byte readers and CRC32, so a test can pin the wire format
// without a model.  The model-facing glue stays in vpd_file.cpp and reads and
// writes bone scale through the bone_scale.hpp API only; no legacy bone or
// record structure is extended.

#pragma once

#include "mikumikustudio/bone_scale.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio {
namespace vpd_scale_io {

// One scaled bone of the pose.  The name is stored next to the index so a
// load can refuse a record whose model no longer matches the bone.
struct Entry {
    std::int32_t bone = -1;   // model bone index at save time
    std::array<char, 20> name{};  // SJIS bone name, NUL padded (BoneRecord::name)
    bone_scale::Matrix scale;
};

struct Extension {
    std::uint64_t prefixBytes = 0;  // byte count of the standard VPD text
    std::uint32_t prefixCrc32 = 0;  // IEEE CRC32 of all standard VPD bytes
    std::vector<Entry> entries;
};

// The only version this reader understands.  A payload that declares anything
// else is an unknown version: refused as a whole and never reinterpreted.
inline constexpr std::uint32_t kVersion = 1;

// ---- payload codec --------------------------------------------------------

enum class Decode {
    Ok,
    BadMagic,
    BadVersion,
    BadCrc,
    BadData,  // truncation, count overflow, a size that breaks the length
              // formula, duplicate (bone, name) entries, or a matrix that is
              // not finite with a positive determinant
};
// Encodes what the writer rules above describe: entries with an invalid or
// identity matrix, and duplicate (bone, name) pairs after the first, are not
// samples and are dropped.  The trailing CRC32 covers every earlier payload
// byte.
std::vector<std::uint8_t> Encode(const Extension& extension);
// Strict, all-or-nothing decode of one payload buffer.  On any refusal `out`
// is left empty.  Bounds are checked before every read, so a crafted count
// field can never make the reader step past `size`.
Decode DecodePayload(const std::uint8_t* data, std::size_t size,
                     Extension& out);

// ---- tail files -----------------------------------------------------------

enum class TailSave {
    Written,    // payload + footer appended after the standard text
    Untouched,  // no scale to export: no tail is written at all
    Failed,     // write failed; the caller must report it, nothing is silent
};
// Appends the tail behind `fd`, which must be positioned at the end of the
// standard text.  `extension.prefixBytes` must equal the current offset and
// `extension.prefixCrc32` must describe the bytes before it; the call refuses
// to write when they disagree, so a tail can never be stapled to the wrong
// boundary.  The payload is written whole or not at all.
TailSave AppendTail(int fd, const Extension& extension);
// File-level convenience used by SaveVpdFile: fingerprints the file as it
// stands (prefixBytes = its length, prefixCrc32 over those bytes), then
// appends the tail for `entries`.  The entry filtering of Encode applies, and
// when nothing survives it the file is left untouched.
TailSave AppendTail(const std::wstring& path, const std::vector<Entry>& entries);

enum class TailLoad {
    Loaded,           // footer found, payload and standard text both verified
    Absent,           // no footer magic: an ordinary pose file, identity scale
    IgnoredBoundary,  // the tail does not sit where the loader's parse ended
    IgnoredBadFooter,  // footer magic present but the size claim does not fit
    IgnoredBadPayload,  // truncation, size/count mismatch, duplicates, matrix
    IgnoredBadVersion,  // the payload declares a version this reader lacks
    IgnoredBadCrc,      // payload or standard-text CRC mismatch (stale tail)
};
// Reads and verifies the tail of `path`.  `expectedRecords` is the pose-record
// count as the loader parsed it; when it is >= 0 the tail must sit exactly at
// the end of 5 + 5 * expectedRecords lines of standard text, which is where
// the loader's own read of the standard segment ended.  Pass -1 to rely on
// the tail alone (tests and tools without a parse).  Everything Ignored* is
// refused as a whole: `out` is left empty and the caller keeps the standard
// pose with default identity scales.
TailLoad LoadTail(const std::wstring& path, std::int64_t expectedRecords,
                  Extension& out);

// One sentence per outcome, for the caller's runtime log.
const char* Describe(TailLoad status);

// ---- entry-to-bone resolution (pure) -------------------------------------

// Resolves every entry to a target bone index, or -1 when the entry must be
// skipped, under the resolution rules at the top of this header.  `boneNames`
// is the model's bone table in index order (NUL-padded 20-byte SJIS fields)
// and `touched` marks the bones the standard segment wrote (its size may be
// shorter than the bone table; a bone outside it counts as untouched).  The
// result has one slot per entry, in file order, and no entry ever lands on a
// bone another entry already claimed.
std::vector<std::int32_t> Resolve(const std::vector<Entry>& entries,
                                  const std::vector<std::array<char, 20>>& boneNames,
                                  const std::vector<std::uint8_t>& touched);

}  // namespace vpd_scale_io
}  // namespace mikumikustudio
