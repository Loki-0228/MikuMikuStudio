// Persistence for native bone scaling (bone_scale.hpp).
//
// Two independent channels, neither of which extends a legacy ABI:
//
//   PMM v2 tail extension record  "org.mikudancestudio.model.<slot>.boneScale.v1"
//   or "org.mikudancestudio.model.<slot>.boneScale.v2"
//     One record per model slot inside the existing "MMSXPMM2" extension
//     container (legacy "MDSXPMM2" is also read, pmm_extension.hpp).  The
//     standard PMM and the container layout are untouched; a record this
//     reader does not understand (an unknown version suffix) keeps its raw
//     bytes and is rewritten unchanged.
//     The two known suffixes name the two layouts below; a slot holds the one
//     layout its capture selected and never both.
//
//   VMD tail extension  (the motion file itself, no side file)
//     The standard VMD prefix stays byte-for-byte what the stock writer
//     produces; the scales ride in an extension appended after it and located
//     by a fixed footer.  The stock MMD 9.32 loader stops at the end of the
//     standard IK section and closes the file without any EOF check
//     (verified statically: VA 0x14006E786 loop, _close at 0x14006E7B3), so
//     the tail is invisible to it.  The extension records the byte count and
//     CRC32 of the standard prefix, so a motion re-saved or edited by another
//     program is detected and stale scales are ignored instead of being
//     applied to the wrong motion.
//
// Wire layouts (all integers little-endian, fields written one by one, no
// C++ struct ever dumped to disk):
//
// PMM record value, version 1
// ("org.mikudancestudio.model.<slot>.boneScale.v1"):
//   uint32 poseCount
//   uint32 keyCount
//   poseCount x { int32 bone; char name[20]; float m[9]; }        (60 bytes)
//   keyCount  x { int32 bone; char name[20]; uint32 frame; float m[9]; }
//                                                                 (64 bytes)
//   `bone` is the bone index at capture time, `name` the NUL-padded SJIS
//   bone name and `frame` the absolute model frame.  A reader must check
//   both index and name before applying ("same-slot bone identity"), so
//   nothing is keyed by key-slot number and a PMM load that reorders the
//   key pool still lands on the right records.
//
// PMM record value, version 2
// ("org.mikudancestudio.model.<slot>.boneScale.v2"):
//   the complete version 1 layout above, followed by
//   keyCount x { uint8 x1; uint8 y1; uint8 x2; uint8 y2; }        (4 bytes)
//   The curve is that key's bone_scale::Curve tuple, each component 0..127.
//   The i-th curve belongs to the i-th key record, so a curve lands on the
//   key slot that record's bone identity resolves to, exactly like the
//   matrix.  Pose records carry no curve: a curve describes the interval
//   between two scaled keys.
//
// A key record carries whatever of the two halves is not the default: a real
// scale matrix, a non-linear curve, or both.  The scale and the curve of one
// key are stored independently in the model (bone_scale::SetKey drops only the
// matrix of an identity value and leaves the curve alone), so a key whose scale
// returned to 1 still owns the curve of the interval that ends on it and must
// still be written - dropping it is what lost the end curve of a scale 2 to
// scale 1 motion.  A record that holds neither half is the default state and is
// not written at all.
//
// Version selection: the encoder writes version 2 as soon as one key record
// it contains carries a curve other than the linear one, and version 1 when
// none does.  With no curve data the output is byte-for-byte what the
// version 1 encoder produced.  The decoder reads the layout the record key
// promises - the ".v1" suffix means the version 1 layout, the ".v2" suffix the
// version 2 layout - so a version 2 payload whose curve array is cut short is a
// short version 2 record and is refused, never read as a version 1 record with
// the curves silently dropped.  A version 1 payload reads back with every curve
// linear.  A suffix this reader does not know is an unknown version: its bytes
// are preserved unchanged and are never reinterpreted as the newest known
// layout.
//
// VMD file with an extension:
//   [standard VMD bytes: header + full standard body]   prefixBytes
//   [extension payload]                                 payloadBytes
//   [footer: uint64 payloadBytes; char magic[8] = "MMSXVMDX"]     (16 bytes)
//            (legacy "MDSXVMDX" is also read)
//
//   A file is "just a standard VMD" when the last 8 bytes are not the footer
//   magic; a file with the footer must satisfy
//   prefixBytes + payloadBytes + 16 == file size, where prefixBytes comes
//   from the payload.
//
// Extension payload:
//   char     magic[8]   = "MMSXMMSX"
//   uint32   version    = 1 or 2
//   uint32   reserved   = 0
//   uint64   prefixBytes = byte count of the standard VMD prefix
//   uint32   prefixCrc32 = IEEE CRC32 of all standard VMD bytes
//   uint32   entryCount
//   entryCount x { char name[15]; uint32 frame; float m[9]; }     (55 bytes,
//                                                                  version 1)
//   entryCount x { char name[15]; uint32 frame; float m[9];
//                  uint8 x1; uint8 y1; uint8 x2; uint8 y2; }      (59 bytes,
//                                                                  version 2)
//   uint32   crc32      = IEEE CRC32 of every preceding byte of the payload
//   `name` is the exact 15-byte SJIS bone-name field written to the VMD and
//   `frame` is the VMD frame minus the min frame of the save (the same
//   minFrame SaveVmdFile subtracts).  The curve of a version 2 entry is the
//   same 0..127 tuple as in the PMM record.
//   The version sizes the entries: a version 1 payload holds 55-byte entries
//   and a version 2 payload holds 59-byte entries, and a payload that does
//   not fit its own version is refused rather than read with the wrong
//   stride.  Version 2 is written as soon as one entry carries a curve other
//   than the linear one; with no curve data the payload is byte-for-byte the
//   version 1 payload.  As in the PMM record, an entry is written when it
//   holds a real matrix or a non-linear curve, so a bone that scaled back to 1
//   still carries the curve of the interval that ends at its key.  The footer,
//   prefixBytes, prefixCrc32 and the CRC32 coverage are identical in both
//   versions.
//
// A curve is the linear curve (20/20/107/107) or a real keyframe curve.  The
// model stores a linear curve as "no entry" (bone_scale::SetKeyCurve), so a
// key without a curve and a key whose curve is explicitly linear are one and
// the same state in memory.  Both wire layouts keep that: a curve travels
// only with the key record it belongs to, and a missing curve reads back as
// bone_scale::LinearCurve().
//
// Both decoders are strict: truncated or oversized payloads, count fields
// that overflow the buffer, duplicate keys, non-finite or negative-
// determinant matrices, curve bytes outside 0..127 and unknown
// magic/version/CRC mismatches are refused
// as a whole; nothing is ever half applied.  A tail that cannot be used is
// ignored with a runtime log entry - the standard part of the motion still
// loads normally either way.  The encoders never drop a sample; an oversized
// extension fails the save visibly instead of silently losing scale.  With
// no scale to export, no tail is written at all.
//
// Everything in this header is deterministic CPU code.  The model-facing
// functions at the bottom read and write bone_scale state through the
// bone_scale.hpp API only; no legacy bone or key structure is extended.

#pragma once

#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/pmm_extension.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mikudancestudio {
class MMDApp;
namespace bone_scale_io {

// ---- shared sample types -------------------------------------------------

// One scaled bone of the current pose.  The name is stored next to the index
// so a load can refuse a record whose model no longer matches the bone.
struct PoseSample {
    std::int32_t bone = -1;
    std::array<char, 20> name{};  // SJIS bone name, NUL padded
    bone_scale::Matrix scale;
};

// One scaled timeline key of a bone track.  The curve belongs to this key and
// describes the interval that ends at it; the default is the linear curve,
// which is the same state as "this key has no curve".
struct KeySample {
    std::int32_t bone = -1;
    std::array<char, 20> name{};
    std::uint32_t frame = 0;  // absolute model frame
    bone_scale::Matrix scale;
    bone_scale::Curve curve = bone_scale::LinearCurve();
};

struct Samples {
    std::vector<PoseSample> pose;
    std::vector<KeySample> keys;
};

// ---- PMM v2 record codec -------------------------------------------------

// Layout versions of the per-slot record.  The suffix in the record key and
// the layout of the record value are two views of the same version.
inline constexpr std::uint32_t kPmmVersionV1 = 1;  // no curve array
inline constexpr std::uint32_t kPmmVersionV2 = 2;  // curve array after v1

// "org.mikudancestudio.model.<slot>.boneScale.v1" (the default version) or
// "...boneScale.v<version>" for an explicit version.
std::string PmmKey(int slot);
std::string PmmKey(int slot, std::uint32_t version);
// True when `key` is any version of this slot's bone-scale record.  `slot`
// receives the model slot and `version` the suffix after ".boneScale."
// (for example "v1" or "v2"); a suffix other than a version this reader
// knows is an unknown version and must be preserved verbatim, never
// reinterpreted as the newest known version.
bool ParsePmmKey(const std::string& key, int& slot, std::string& version);

enum class PmmDecode {
    Ok,
    BadData,  // truncation, count overflow, duplicates, a payload that fits
              // neither layout, a bad bone index, a curve byte outside 0..127
              // or a matrix that is not finite with positive determinant
};
// The encoders never drop a sample: what the model holds is written in full
// (a key that holds only the default state aside), and an oversized record is
// left oversized so a container that cannot hold it fails the save visibly
// instead of silently losing scale.  The decoders keep hard count caps as a
// corruption guard.
std::vector<std::uint8_t> EncodePmm(const Samples& samples);
// Decodes the layout the record key promises: `version` is kPmmVersionV1 or
// kPmmVersionV2, taken from the suffix of the key the payload was read from.
// The expected curve array is then exact, so a version 2 payload whose curve
// bytes are missing is refused as a short version 2 record instead of being
// read as a version 1 record with the curves dropped.  A version 1 payload
// decodes with every key curve linear, which is the state a key without a
// curve has in memory.
PmmDecode DecodePmm(const std::uint8_t* data, std::size_t size,
                    std::uint32_t version, Samples& out);
// Layout-lenient overload for a caller that has no record key: the exact size
// picks the layout.  It cannot tell a version 2 payload with every curve byte
// missing from a version 1 payload, so every module path takes the version
// overload above instead.
PmmDecode DecodePmm(const std::uint8_t* data, std::size_t size, Samples& out);

// True when `samples` would be encoded with a curve other than the linear
// one, that is, when EncodePmm writes the version 2 layout.  Only keys the
// encoder actually writes count: an invalid matrix is dropped, and a key is
// dropped only when its matrix is the identity *and* its curve is linear, so a
// curve on a key that scaled back to 1 still selects version 2.  Callers that
// name the record (CapturePmm) ask this so the key suffix and the payload
// layout always agree.
bool PmmHasCurves(const Samples& samples);

// ---- VMD tail extension codec -------------------------------------------

// Version written when no entry carries a curve.
inline constexpr std::uint32_t kExtensionVersion = 1;
// Version written when at least one entry carries a curve; its entries are
// four bytes longer.
inline constexpr std::uint32_t kExtensionVersionV2 = 2;

struct VmdExtension {
    struct Entry {
        std::array<char, 15> name{};  // the VMD 15-byte SJIS bone-name field
        std::uint32_t frame = 0;      // VMD frame minus the save's minFrame
        bone_scale::Matrix scale;
        bone_scale::Curve curve = bone_scale::LinearCurve();
    };
    std::uint64_t prefixBytes = 0;  // byte count of the standard VMD prefix
    std::uint32_t prefixCrc32 = 0;  // IEEE CRC32 of all standard VMD bytes
    std::vector<Entry> entries;
};

// Streaming fingerprint of the standard VMD prefix, fed by the writer.
class PrefixFingerprint {
public:
    void Append(const void* data, std::size_t size);
    std::uint64_t bytes() const { return bytes_; }
    std::uint32_t crc32() const;

private:
    std::uint64_t bytes_ = 0;
    std::uint32_t state_ = 0xFFFFFFFFu;
};

enum class ExtensionDecode {
    Ok,
    BadMagic,
    BadVersion,
    BadCrc,
    BadData,
};
// Writes version 2 as soon as one entry that the encoder keeps carries a
// curve other than the linear one, and version 1 otherwise.  With no curve
// data the bytes are exactly what the version 1 encoder produced.  Entries
// with an invalid matrix, and duplicate (name, frame) pairs, are dropped
// together with their curves; an entry whose matrix is the identity is kept
// when its curve is not linear, because that curve still shapes the interval
// ending at the key.
std::vector<std::uint8_t> EncodeExtension(const VmdExtension& extension);
// Accepts version 1 and version 2 equally; the version decides the entry
// stride (55 or 59 bytes).  An entry of a version 1 payload reads back with
// the linear curve.
ExtensionDecode DecodeExtension(const std::uint8_t* data, std::size_t size,
                                VmdExtension& out);

enum class ExtensionSave {
    Written,    // payload + footer appended after the standard prefix
    Untouched,  // no scale exported: no tail is written at all
    Failed,     // write failed; the caller must report it, nothing is silent
};
// Appends the extension to the motion file behind `fd`, which must be
// positioned at the end of the standard prefix.  `extension.prefixBytes` and
// `extension.prefixCrc32` describe that prefix and must be filled by the
// caller (PrefixFingerprint supplies both).  The payload is written whole or
// not at all: an extension the payload size limit cannot hold fails instead
// of being truncated.
ExtensionSave SaveVmdExtension(int fd, const VmdExtension& extension);

enum class ExtensionLoad {
    Loaded,   // footer found, payload and standard prefix both verified
    Absent,   // no footer magic: an ordinary motion, identity scale
    Ignored,  // footer present but unusable (bad version/CRC/structure, a
              // payload that does not align with the file, or a standard
              // prefix that no longer matches its recorded CRC)
};
// `expectedPrefixBytes` is the end offset of the standard part as the caller
// read it (the loader's _telli64 right after the last standard section); when
// it is >= 0 the extension's own prefix claim is checked against it, so the
// standard boundary is verified independently of the tail.  Pass -1 to rely
// on the tail alone.
ExtensionLoad LoadVmdExtension(const std::wstring& vmdPath, VmdExtension& out,
                               std::int64_t expectedPrefixBytes = -1);

// ---- imported-key matching (pure) ---------------------------------------

struct ImportedKey {
    std::int32_t bone = -1;   // owner bone index of the key
    std::uint32_t frame = 0;  // model frame (import offset already applied)
    int slot = -1;            // model key slot that should receive the scale
};

struct Assignment {
    int slot = -1;
    bone_scale::Matrix scale;
    bone_scale::Curve curve = bone_scale::LinearCurve();
};

// Resolves extension entries against the keys an import just created.  Entry
// frames are rebased VMD frames, so a record matches at frameOffset +
// entry.frame (frameOffset is the current frame the motion was imported at,
// as a 64-bit sum so a large offset can never wrap).  A negative frameOffset
// is refused with no assignments.  An entry whose bone name matches no bone,
// or more than one bone, is skipped; so is an entry without exactly one
// imported key at that bone and frame.  Existing scales outside the import
// are never touched.  One index pass serves every entry, so the cost stays
// linear in bones, imported keys and entries.  Each assignment carries the
// entry's curve so the caller can restore matrix and curve together; an
// entry of a version 1 payload carries the linear curve.
std::vector<Assignment> MatchImported(
    const VmdExtension& extension, const std::vector<ImportedKey>& imported,
    const std::vector<std::array<char, 20>>& boneNames,
    std::int64_t frameOffset);

// ---- model-facing glue ---------------------------------------------------

// Writes or replaces the per-slot records in `document` at the end of a PMM
// capture.  The known record is rebuilt from the current pose and keys on
// every capture, under the version suffix that matches its payload: ".v2"
// when a key carries a curve and ".v1" otherwise, with the other known
// version of the slot removed.  A record this reader cannot understand (an
// unknown version, or an undecodable payload) keeps its raw bytes until
// bone_scale::Revision or the frames of the scaled keys move past the values
// taken at the last load: only a real edit - never frame seeking - may
// replace it.  An empty slot or a different model in the slot drops the
// slot's old records so a new model can never inherit them.
void CapturePmm(const MMDApp& app, pmm_extension::Document& document);
// Applies the per-slot records after the standard PMM data has been loaded.
// A record is validated as a whole (bone identity, indices, duplicates,
// limits) before anything is applied; a record that cannot be applied is
// ignored and keeps its raw bytes in the document.  Both layouts are read,
// and a version 1 record leaves every curve linear.
void LoadPmm(const MMDApp& app, const pmm_extension::Document& document);
// Drops the per-slot baselines (new project / full reload).
void ResetPmm();

// Collects the scales and curves of the exported (allocated) bone keys
// exactly as SaveVmdFile exports them: same bone-name resolution, same
// rebased frames.  A key whose scale has returned to 1 is still collected when
// it carries a non-linear curve, so the interval that ends on it keeps its
// easing across a save.
void CaptureExtension(const unsigned char* model, std::uint32_t minFrame,
                      VmdExtension& out);
// Applies extension entries right after a VMD import, restricted to the key
// slots the import just marked allocated.  Matrix and curve are written
// together, so a version 1 entry resets the slot to the linear curve and an
// entry whose scale returned to 1 still installs its curve.
// Returns the number of applied scales.
int ApplyImportedExtension(unsigned char* model, const VmdExtension& extension,
                           int frameOffset);

}  // namespace bone_scale_io
}  // namespace mikudancestudio
