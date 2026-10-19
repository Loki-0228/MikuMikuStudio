// MAINTENANCE: regression-keep; native bone-scaling persistence codec
// (include/mikumikustudio/bone_scale_io.hpp, src/io/bone_scale_io.cpp):
// the PMM v2 "org.mikudancestudio.model.<slot>.boneScale.v1" record payload,
// the single-file VMD tail extension and imported-key matcher.
//
// The codec sections exercise only the public codec API.  The VPD section at
// the bottom additionally drives the real SaveVpdFile/LoadVpdFile against a
// hand-built model fixture and an MMDApp shell - still no GPU device and no
// window.  Every temporary file is created under %TEMP% and deleted by
// its guard on every exit path.  The tests build wire bytes and CRC32 values
// with their own helpers, so the on-disk layout (little-endian fields, no
// struct padding) is pinned independently of the module under test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <process.h>
#include <sys/stat.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/vpd_scale_io.hpp"

namespace bs = mikumikustudio::bone_scale;
namespace bsio = mikumikustudio::bone_scale_io;
namespace mdl = mikumikustudio::mdl;
namespace vpdio = mikumikustudio::vpd_scale_io;

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

void PutName(std::vector<std::uint8_t>& out, const char* name,
             std::size_t fieldSize) {
    for (std::size_t i = 0; i < fieldSize; ++i)
        out.push_back(i < std::strlen(name)
                          ? static_cast<std::uint8_t>(name[i])
                          : 0);
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

bool SameMatrix(const bs::Matrix& a, const bs::Matrix& b) {
    return a.v == b.v;
}

std::array<char, 20> Name20(const char* name) {
    std::array<char, 20> field{};
    for (std::size_t i = 0; i < field.size() && name[i] != '\0'; ++i)
        field[i] = name[i];
    return field;
}

std::array<char, 15> Name15(const char* name) {
    std::array<char, 15> field{};
    for (std::size_t i = 0; i < field.size() && name[i] != '\0'; ++i)
        field[i] = name[i];
    return field;
}

// A PMM record value built by hand, so the decoder can be fed data the
// encoder would never produce.  `curves` are the version 2 curve array: when
// it is not empty the four bytes per key follow every key record.
std::vector<std::uint8_t> HandPmmPayload(
    std::uint32_t poseCount, std::uint32_t keyCount,
    const std::vector<std::array<float, 10>>& pose,  // bone + 9 floats
    const std::vector<std::array<float, 11>>& keys,  // bone + frame + 9 floats
    const std::vector<const char*>& poseNames,
    const std::vector<const char*>& keyNames,
    const std::vector<bs::Curve>& curves = {}) {
    std::vector<std::uint8_t> out;
    PutU32(out, poseCount);
    PutU32(out, keyCount);
    for (std::size_t i = 0; i < pose.size(); ++i) {
        PutU32(out, static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(pose[i][0])));
        PutName(out, i < poseNames.size() ? poseNames[i] : "bone", 20);
        for (int j = 0; j < 9; ++j) PutF32(out, pose[i][static_cast<std::size_t>(j) + 1]);
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        PutU32(out, static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(keys[i][0])));
        PutName(out, i < keyNames.size() ? keyNames[i] : "bone", 20);
        PutU32(out, static_cast<std::uint32_t>(keys[i][1]));
        for (int j = 0; j < 9; ++j) PutF32(out, keys[i][static_cast<std::size_t>(j) + 2]);
    }
    for (std::size_t i = 0; i < curves.size(); ++i)
        out.insert(out.end(), curves[i].begin(), curves[i].end());
    return out;
}

// A extension file built by hand (magic, version, reserved, fingerprint,
// count, entries, CRC32 of everything before the CRC).  `curves`, when not
// empty, appends the four version 2 curve bytes to every entry it covers.
std::vector<std::uint8_t> HandExtension(
    std::uint32_t version, std::uint64_t prefixBytes, std::uint32_t prefixCrc32,
    const std::vector<std::array<float, 10>>& entries,  // frame + 9 floats
    const std::vector<const char*>& names, bool goodCrc = true,
    const std::vector<bs::Curve>& curves = {}) {
    std::vector<std::uint8_t> body;
    const char magic[8] = {'M', 'D', 'S', 'X', 'M', 'D', 'S', 'X'};
    body.insert(body.end(), magic, magic + 8);
    PutU32(body, version);
    PutU32(body, 0);
    PutU64(body, prefixBytes);
    PutU32(body, prefixCrc32);
    PutU32(body, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        PutName(body, i < names.size() ? names[i] : "bone", 15);
        PutU32(body, static_cast<std::uint32_t>(entries[i][0]));
        for (int j = 0; j < 9; ++j)
            PutF32(body, entries[i][static_cast<std::size_t>(j) + 1]);
        if (i < curves.size())
            body.insert(body.end(), curves[i].begin(), curves[i].end());
    }
    PutU32(body, RefCrc32(body));
    if (!goodCrc) body[body.size() - 1] ^= 0xFFu;
    return body;
}

// ---------------------------------------------------------------------------
// Temp files under %TEMP%, deleted on every exit path.
// ---------------------------------------------------------------------------

std::wstring TempDir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) return L".";
    return std::wstring(buffer, length);
}

class TempMotion {
public:
    TempMotion() {
        wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH,directory) || !GetTempFileNameW(directory,L"bsi",0,path)) std::abort();
        path_=path;
    }
    ~TempMotion() { DeleteFileW(path_.c_str()); }
    const std::wstring& path() const { return path_; }
    int Open(int mode) const {
        int fd=-1;
        if (_wsopen_s(&fd,path_.c_str(),mode|_O_BINARY,_SH_DENYNO,_S_IREAD|_S_IWRITE)!=0) std::abort();
        return fd;
    }
    bool Write(const std::vector<std::uint8_t>& bytes) const {
        const int fd=Open(_O_WRONLY|_O_TRUNC);
        const bool okay=_write(fd,bytes.data(),static_cast<unsigned>(bytes.size()))==static_cast<int>(bytes.size());
        return _close(fd)==0 && okay;
    }
    std::vector<std::uint8_t> Read() const {
        const int fd=Open(_O_RDONLY);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(_filelengthi64(fd)));
        if(_read(fd,bytes.data(),static_cast<unsigned>(bytes.size()))!=static_cast<int>(bytes.size()))std::abort();
        _close(fd);return bytes;
    }
private:
    std::wstring path_;
};

const std::vector<std::uint8_t> kFakeVmd = [] {
    std::vector<std::uint8_t> bytes;
    const char header[30] = "Vocaloid Motion Data 0002";
    bytes.insert(bytes.end(), header, header + 30);
    for (int i = 0; i < 234; ++i)
        bytes.push_back(static_cast<std::uint8_t>(0x40 + (i & 0x3F)));
    return bytes;
}();

// ---------------------------------------------------------------------------
// PMM record codec.
// ---------------------------------------------------------------------------

bsio::Samples SampleBlock() {
    bsio::Samples samples;
    bsio::PoseSample poseA;
    poseA.bone = 2;
    poseA.name = Name20("center");
    poseA.scale = Matrix({2, 0, 0, 0, 2, 0, 0, 0, 2});
    bsio::PoseSample poseB;
    poseB.bone = 5;
    poseB.name = Name20("left arm");
    // Coordinate-system switches can shear a bone, so all 9 floats survive.
    poseB.scale = Matrix({1, 0.25f, 0, 0, 1, 0, 0.5f, 0, 1});
    samples.pose.push_back(poseA);
    samples.pose.push_back(poseB);
    bsio::KeySample keyA;
    keyA.bone = 2;
    keyA.name = Name20("center");
    keyA.frame = 30;
    keyA.scale = Matrix({1.5f, 0, 0, 0, 1.5f, 0, 0, 0, 1.5f});
    bsio::KeySample keyB;
    keyB.bone = 7;
    keyB.name = Name20("right leg");
    keyB.frame = 120;
    keyB.scale = Matrix({1, 0, 0, 0, 3, 0, 0, 0, 1});
    samples.keys.push_back(keyA);
    samples.keys.push_back(keyB);
    return samples;
}

void PmmKeyParsing() {
    Check(bsio::PmmKey(3) == "org.mikudancestudio.model.3.boneScale.v1",
          "PMM record key follows the existing org.mikudancestudio namespace");
    Check(bsio::PmmKey(255, 2) ==
              "org.mikudancestudio.model.255.boneScale.v2" &&
              bsio::PmmKey(255, bsio::kPmmVersionV1) == bsio::PmmKey(255),
          "the record key names the layout version it holds");
    int slot = -1;
    std::string version;
    Check(bsio::ParsePmmKey("org.mikudancestudio.model.3.boneScale.v1", slot,
                            version) &&
              slot == 3 && version == "v1",
          "PMM record key parses back to slot and version");
    Check(bsio::ParsePmmKey("org.mikudancestudio.model.255.boneScale.v2", slot,
                            version) &&
              slot == 255 && version == "v2",
          "the version 2 record suffix parses back to slot and version");
    Check(bsio::ParsePmmKey("org.mikudancestudio.model.9.boneScale.v9", slot,
                            version) &&
              slot == 9 && version == "v9",
          "an unknown version suffix stays its own version and is never read as v2");
    Check(!bsio::ParsePmmKey("org.mikudancestudio.model.3.boneScale.", slot,
                             version),
          "a missing version suffix is not a bone-scale record");
    Check(!bsio::ParsePmmKey("org.mikudancestudio.model.x.boneScale.v1", slot,
                             version),
          "a non-numeric slot is not a bone-scale record");
    Check(!bsio::ParsePmmKey("org.mikudancestudio.view.transparent_sort",
                             slot, version),
          "unrelated PMM keys are left to their own reader");
    Check(!bsio::ParsePmmKey("mds.viewport.zoom", slot, version),
          "a foreign namespace key is not a bone-scale record");
}

void PmmRoundTrip() {
    const bsio::Samples source = SampleBlock();
    const std::vector<std::uint8_t> bytes = bsio::EncodePmm(source);
    Check(!bytes.empty(), "PMM payload encodes");
    Check(bytes.size() == 8 + 2 * 60 + 2 * 64,
          "PMM payload layout is 8 bytes of counts plus 60/64 byte entries");
    bsio::Samples decoded;
    Check(bsio::DecodePmm(bytes.data(), bytes.size(), bsio::kPmmVersionV1,
                          decoded) == bsio::PmmDecode::Ok,
          "PMM payload decodes");
    Check(decoded.pose.size() == 2 && decoded.keys.size() == 2,
          "PMM round trip keeps every pose and key sample");
    Check(decoded.pose[0].bone == 2 && decoded.pose[1].bone == 5,
          "PMM round trip keeps bone indices");
    Check(std::memcmp(decoded.pose[1].name.data(), "left arm", 8) == 0,
          "PMM round trip keeps the bone name for identity checks");
    Check(decoded.keys[0].frame == 30 && decoded.keys[1].frame == 120,
          "PMM round trip keeps absolute key frames");
    Check(SameMatrix(decoded.pose[1].scale, source.pose[1].scale),
          "PMM round trip keeps all 9 floats of a sheared matrix");
    Check(SameMatrix(decoded.keys[1].scale, source.keys[1].scale),
          "PMM round trip keeps key matrices exactly");

    // The encoder never writes identity scales; a decode of an empty block
    // leaves both maps empty.
    const std::vector<std::uint8_t> empty = bsio::EncodePmm({});
    Check(empty.size() == 8, "an empty capture encodes to counts only");
    bsio::Samples emptyDecoded;
    Check(bsio::DecodePmm(empty.data(), empty.size(), bsio::kPmmVersionV1,
                          emptyDecoded) == bsio::PmmDecode::Ok &&
              emptyDecoded.pose.empty() && emptyDecoded.keys.empty(),
          "an empty capture round trips to nothing");
}

// The version 1 bytes of SampleBlock(), built by the independent helper above.
std::vector<std::uint8_t> SampleBlockVersion1() {
    return HandPmmPayload(
        2, 2,
        {std::array<float, 10>{2, 2, 0, 0, 0, 2, 0, 0, 0, 2},
         std::array<float, 10>{5, 1, 0.25f, 0, 0, 1, 0, 0.5f, 0, 1}},
        {std::array<float, 11>{2, 30, 1.5f, 0, 0, 0, 1.5f, 0, 0, 0, 1.5f},
         std::array<float, 11>{7, 120, 1, 0, 0, 0, 3, 0, 0, 0, 1}},
        {"center", "left arm"}, {"center", "right leg"});
}

void PmmCurveVersioning() {
    const bsio::Samples linear = SampleBlock();
    const std::vector<std::uint8_t> v1 = SampleBlockVersion1();
    Check(bsio::EncodePmm(linear) == v1,
          "a capture without curves keeps the byte-for-byte version 1 PMM payload");
    Check(!bsio::PmmHasCurves(linear),
          "a capture without curves does not select the version 2 record");
    bsio::Samples decoded;
    Check(bsio::DecodePmm(v1.data(), v1.size(), bsio::kPmmVersionV1, decoded) ==
              bsio::PmmDecode::Ok &&
              decoded.keys.size() == 2 &&
              bs::Linear(decoded.keys[0].curve) &&
              bs::Linear(decoded.keys[1].curve),
          "a version 1 PMM payload reads back with every curve linear");

    // One key curve selects version 2: four bytes per key record, after the
    // whole version 1 layout and in the same order.
    bsio::Samples curved = linear;
    // Both tuples must be non-linear under bone_scale::Linear, whose rule is
    // x1 == y1 && x2 == y2: {0,0,127,127} would count as linear there, exactly
    // as bone_scale stores the identity tuple.
    curved.keys[0].curve = {20, 20, 20, 107};
    curved.keys[1].curve = {20, 40, 100, 120};
    Check(bsio::PmmHasCurves(curved),
          "a non-linear key curve selects the version 2 record");
    const std::vector<std::uint8_t> v2 = bsio::EncodePmm(curved);
    Check(v2.size() == v1.size() + 2 * 4,
          "a version 2 PMM payload appends four curve bytes per key");
    Check(std::memcmp(v2.data(), v1.data(), v1.size()) == 0,
          "the version 2 PMM payload starts with the exact version 1 layout");
    bsio::Samples roundTrip;
    Check(bsio::DecodePmm(v2.data(), v2.size(), bsio::kPmmVersionV2, roundTrip) ==
              bsio::PmmDecode::Ok,
          "a version 2 PMM payload decodes");
    Check(roundTrip.keys.size() == 2 &&
              roundTrip.keys[0].bone == 2 && roundTrip.keys[0].frame == 30 &&
              roundTrip.keys[1].bone == 7 && roundTrip.keys[1].frame == 120,
          "a version 2 round trip keeps every key bone identity");
    Check(roundTrip.keys[0].curve == curved.keys[0].curve &&
              roundTrip.keys[1].curve == curved.keys[1].curve,
          "each key curve comes back on its own key record, value by value");
    Check(SameMatrix(roundTrip.keys[1].scale, curved.keys[1].scale) &&
              SameMatrix(roundTrip.pose[1].scale, curved.pose[1].scale),
          "a version 2 round trip keeps matrices and pose records");

    // Data a hand builder produced, not just what this encoder writes.
    const std::vector<std::uint8_t> handV2 = HandPmmPayload(
        0, 1, {}, {std::array<float, 11>{0, 30, 2, 0, 0, 0, 2, 0, 0, 0, 2}},
        {}, {"center"}, {{20, 40, 107, 127}});
    Check(bsio::DecodePmm(handV2.data(), handV2.size(), bsio::kPmmVersionV2,
                          decoded) == bsio::PmmDecode::Ok &&
              decoded.keys.size() == 1 &&
              decoded.keys[0].curve == bs::Curve{20, 40, 107, 127},
          "a hand-built version 2 payload decodes curve by curve");

    // The payload must fit its own layout exactly: a version 2 array cut short
    // is refused whole by the version the record key promises.
    const std::vector<std::uint8_t> shortCurves(v2.begin(), v2.end() - 2);
    Check(bsio::DecodePmm(shortCurves.data(), shortCurves.size(),
                          bsio::kPmmVersionV2, decoded) ==
              bsio::PmmDecode::BadData,
          "a version 2 payload with a short curve array is refused");
    // A curve byte outside the 0..127 control-point domain is a bad field.
    const std::vector<std::uint8_t> badCurve = HandPmmPayload(
        0, 1, {}, {std::array<float, 11>{0, 30, 2, 0, 0, 0, 2, 0, 0, 0, 2}},
        {}, {"center"}, {{20, 40, 107, 200}});
    Check(bsio::DecodePmm(badCurve.data(), badCurve.size(), bsio::kPmmVersionV2,
                          decoded) == bsio::PmmDecode::BadData,
          "a curve byte above 127 is refused");

    // A key whose scale returned to 1 still owns the curve of the interval that
    // ends on it.  The model keeps the matrix and the curve independently, so
    // the curve alone has to select version 2 and travel on its own key record:
    // dropping it here is what lost the end curve of a scale 2 -> scale 1 move.
    bsio::Samples curveOnly;
    bsio::KeySample identityKey;
    identityKey.bone = 0;
    identityKey.name = Name20("center");
    identityKey.frame = 30;
    identityKey.curve = {20, 20, 20, 107};  // scale stays identity
    curveOnly.keys.push_back(identityKey);
    Check(bsio::PmmHasCurves(curveOnly),
          "a curve on a key whose scale is identity selects version 2");
    const std::vector<std::uint8_t> curveOnlyV2 = bsio::EncodePmm(curveOnly);
    Check(curveOnlyV2.size() == 8 + 64 + 4,
          "the curve-only key is one 64-byte key record plus its four curve bytes");
    // Key record: int32 bone, name[20], uint32 frame, nine floats.  The nine
    // floats of a curve-only key are the identity the capture held.
    const auto keyMatrixIsIdentity = [&curveOnlyV2]() {
        for (int i = 0; i < 9; ++i) {
            const std::uint32_t bits = ReadU32(curveOnlyV2, 36 + std::size_t(i) * 4);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof value);
            if (value != ((i % 4 == 0) ? 1.0f : 0.0f)) return false;
        }
        return true;
    };
    Check(keyMatrixIsIdentity(),
          "the curve-only key keeps the identity matrix it was captured with");
    bsio::Samples curveOnlyDecoded;
    Check(bsio::DecodePmm(curveOnlyV2.data(), curveOnlyV2.size(),
                          bsio::kPmmVersionV2, curveOnlyDecoded) ==
              bsio::PmmDecode::Ok &&
              curveOnlyDecoded.keys.size() == 1 &&
              curveOnlyDecoded.keys[0].bone == 0 &&
              curveOnlyDecoded.keys[0].frame == 30 &&
              SameMatrix(curveOnlyDecoded.keys[0].scale, bs::Matrix{}) &&
              curveOnlyDecoded.keys[0].curve == bs::Curve{20, 20, 20, 107},
          "a curve-only key round trips with its identity matrix and its curve");

    // A key that holds neither half is the default state and stays out.
    bsio::Samples defaultOnly;
    bsio::KeySample plainKey;
    plainKey.bone = 0;
    plainKey.name = Name20("center");
    plainKey.frame = 30;
    defaultOnly.keys.push_back(plainKey);
    Check(!bsio::PmmHasCurves(defaultOnly) &&
              bsio::EncodePmm(defaultOnly).size() == 8,
          "a key with neither a matrix nor a curve stays out of the payload");
}

void PmmRejects() {
    const bsio::Samples source = SampleBlock();
    const std::vector<std::uint8_t> good = bsio::EncodePmm(source);
    bsio::Samples decoded;

    std::vector<std::uint8_t> truncated(good.begin(), good.end() - 1);
    Check(bsio::DecodePmm(truncated.data(), truncated.size(), decoded) ==
              bsio::PmmDecode::BadData,
          "a truncated PMM payload is refused");
    std::vector<std::uint8_t> extra = good;
    extra.push_back(0);
    Check(bsio::DecodePmm(extra.data(), extra.size(), decoded) ==
              bsio::PmmDecode::BadData,
          "trailing bytes in a PMM payload are refused");

    std::vector<std::uint8_t> huge;
    PutU32(huge, 0xFFFFFFFFu);
    PutU32(huge, 0xFFFFFFFFu);
    Check(bsio::DecodePmm(huge.data(), huge.size(), decoded) ==
              bsio::PmmDecode::BadData,
          "count fields that overflow the buffer are refused");

    const std::array<float, 10> scale = {2, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 10> second = scale;
    second[0] = 2;
    const std::vector<std::uint8_t> dupPose = HandPmmPayload(
        2, 0, {scale, second}, {}, {"center", "center"}, {});
    Check(bsio::DecodePmm(dupPose.data(), dupPose.size(), decoded) ==
              bsio::PmmDecode::BadData,
          "duplicate pose bones are refused");

    // bone 0, frame 30, identity matrix
    const std::array<float, 11> key = {0, 30, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<std::uint8_t> dupKey = HandPmmPayload(
        0, 2, {}, {key, key}, {}, {"center", "center"});
    Check(bsio::DecodePmm(dupKey.data(), dupKey.size(), decoded) ==
              bsio::PmmDecode::BadData,
          "duplicate key bones and frames are refused");

    std::array<float, 10> nanScale = scale;
    nanScale[3] = std::numeric_limits<float>::quiet_NaN();
    const std::vector<std::uint8_t> nanPayload = HandPmmPayload(
        1, 0, {nanScale}, {}, {"center"}, {});
    Check(bsio::DecodePmm(nanPayload.data(), nanPayload.size(), decoded) ==
              bsio::PmmDecode::BadData,
          "a NaN matrix is refused");

    std::array<float, 10> reflected = {0, -1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<std::uint8_t> reflectedPayload = HandPmmPayload(
        1, 0, {reflected}, {}, {"center"}, {});
    Check(bsio::DecodePmm(reflectedPayload.data(), reflectedPayload.size(),
                          decoded) == bsio::PmmDecode::BadData,
          "a negative-determinant matrix is refused");

    std::array<float, 10> badBone = scale;
    badBone[0] = -1;
    const std::vector<std::uint8_t> badBonePayload = HandPmmPayload(
        1, 0, {badBone}, {}, {"center"}, {});
    Check(bsio::DecodePmm(badBonePayload.data(), badBonePayload.size(),
                          decoded) == bsio::PmmDecode::BadData,
          "a negative bone index is refused");
}

// ---------------------------------------------------------------------------
// VMD extension codec and imported-key matching.
// ---------------------------------------------------------------------------

bsio::VmdExtension ExtensionBlock() {
    bsio::VmdExtension extension;
    extension.prefixBytes = 264;
    extension.prefixCrc32 = 0x12345678u;
    bsio::VmdExtension::Entry a;
    a.name = Name15("center");
    a.frame = 100;
    a.scale = Matrix({1.5f, 0, 0, 0, 1.5f, 0, 0, 0, 1.5f});
    bsio::VmdExtension::Entry b;
    b.name = Name15("left arm");
    b.frame = 200;
    b.scale = Matrix({1, 0.25f, 0, 0, 1, 0, 0.5f, 0, 1});
    extension.entries.push_back(a);
    extension.entries.push_back(b);
    return extension;
}

void ExtensionCodecRoundTrip() {
    const bsio::VmdExtension source = ExtensionBlock();
    const std::vector<std::uint8_t> bytes = bsio::EncodeExtension(source);
    Check(bytes.size() == 32 + 2 * 55 + 4,
          "extension layout is a 32-byte header, 55-byte entries and a CRC32");
    bsio::VmdExtension decoded;
    Check(bsio::DecodeExtension(bytes.data(), bytes.size(), decoded) ==
              bsio::ExtensionDecode::Ok,
          "extension decodes");
    Check(decoded.prefixBytes == source.prefixBytes &&
              decoded.prefixCrc32 == source.prefixCrc32,
          "extension keeps the recorded VMD fingerprint");
    Check(decoded.entries.size() == 2 &&
              decoded.entries[0].frame == 100 &&
              decoded.entries[1].frame == 200,
          "extension keeps rebased frames");
    Check(std::memcmp(decoded.entries[1].name.data(), "left arm", 8) == 0,
          "extension keeps the 15-byte VMD bone-name field");
    Check(SameMatrix(decoded.entries[1].scale, source.entries[1].scale),
          "extension keeps all 9 matrix floats");
}

void ExtensionCodecRejects() {
    const bsio::VmdExtension source = ExtensionBlock();
    const std::vector<std::uint8_t> good = bsio::EncodeExtension(source);
    bsio::VmdExtension decoded;

    std::vector<std::uint8_t> truncated(good.begin(), good.end() - 10);
    Check(bsio::DecodeExtension(truncated.data(), truncated.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "a truncated extension is refused");
    std::vector<std::uint8_t> extra = good;
    extra.push_back(0);
    Check(bsio::DecodeExtension(extra.data(), extra.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "trailing bytes in a extension are refused");

    std::vector<std::uint8_t> badCrc = good;
    badCrc[badCrc.size() - 1] ^= 0xFFu;
    Check(bsio::DecodeExtension(badCrc.data(), badCrc.size(), decoded) ==
              bsio::ExtensionDecode::BadCrc,
          "a extension with a broken CRC32 is refused");

    std::vector<std::uint8_t> badMagic = good;
    badMagic[0] = 'X';
    Check(bsio::DecodeExtension(badMagic.data(), badMagic.size(), decoded) ==
              bsio::ExtensionDecode::BadMagic,
          "a file without the extension magic is refused");

    const std::vector<std::uint8_t> badVersion =
        HandExtension(3, 264, 0x12345678u, {}, {});
    Check(bsio::DecodeExtension(badVersion.data(), badVersion.size(), decoded) ==
              bsio::ExtensionDecode::BadVersion,
          "an unknown extension version is refused");

    std::array<float, 10> nanEntry = {100, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    nanEntry[4] = std::numeric_limits<float>::quiet_NaN();
    const std::vector<std::uint8_t> nanExtension =
        HandExtension(1, 264, 0x12345678u, {nanEntry}, {"center"});
    Check(bsio::DecodeExtension(nanExtension.data(), nanExtension.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "a extension entry with a NaN matrix is refused");

    std::array<float, 10> reflected = {100, -1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<std::uint8_t> reflectedExtension =
        HandExtension(1, 264, 0x12345678u, {reflected}, {"center"});
    Check(bsio::DecodeExtension(reflectedExtension.data(), reflectedExtension.size(),
                              decoded) == bsio::ExtensionDecode::BadData,
          "a extension entry with a negative determinant is refused");

    std::array<float, 10> entry = {100, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<std::uint8_t> duplicate = HandExtension(
        1, 264, 0x12345678u, {entry, entry}, {"center", "center"});
    Check(bsio::DecodeExtension(duplicate.data(), duplicate.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "duplicate extension entries are refused");
}

void ExtensionCurveVersioning() {
    // No curve anywhere: version 1 and the unchanged 55-byte entries.
    const bsio::VmdExtension plain = ExtensionBlock();
    const std::vector<std::uint8_t> v1 = bsio::EncodeExtension(plain);
    Check(ReadU32(v1, 8) == bsio::kExtensionVersion,
          "an extension without curves is written as version 1");
    Check(v1.size() == 32 + 2 * 55 + 4,
          "a version 1 extension keeps the 55-byte entry layout");
    bsio::VmdExtension decoded;
    Check(bsio::DecodeExtension(v1.data(), v1.size(), decoded) ==
              bsio::ExtensionDecode::Ok &&
              bs::Linear(decoded.entries[0].curve) &&
              bs::Linear(decoded.entries[1].curve),
          "a version 1 extension reads back with linear curves");

    // One non-linear entry curve selects version 2, and every entry grows by
    // the four curve bytes so the stride stays uniform.
    bsio::VmdExtension curved = ExtensionBlock();
    curved.entries[1].curve = {20, 20, 20, 107};
    const std::vector<std::uint8_t> v2 = bsio::EncodeExtension(curved);
    Check(ReadU32(v2, 8) == bsio::kExtensionVersionV2,
          "a non-linear entry curve selects version 2");
    // 32 header + 55 (entry 1) + 59 (entry 2) + 4 CRC = 150 without curves;
    // every entry carries the four curve bytes, so both entries grow by four.
    Check(v2.size() == 32 + 55 + 59 + 4 + 4,
          "a version 2 extension appends four curve bytes to every entry");
    Check(bsio::DecodeExtension(v2.data(), v2.size(), decoded) ==
              bsio::ExtensionDecode::Ok,
          "a version 2 extension decodes");
    Check(decoded.prefixBytes == curved.prefixBytes &&
              decoded.prefixCrc32 == curved.prefixCrc32,
          "a version 2 extension keeps the recorded VMD fingerprint");
    Check(decoded.entries.size() == 2 &&
              decoded.entries[1].frame == 200 &&
              decoded.entries[1].curve == curved.entries[1].curve &&
              bs::Linear(decoded.entries[0].curve),
          "a version 2 round trip keeps each curve on its own entry");

    // A bone that scaled back to 1 still carries the curve of the interval that
    // ends at its key, so the entry is written with its identity matrix and the
    // curve selects version 2.
    bsio::VmdExtension curveOnly;
    bsio::VmdExtension::Entry identity;
    identity.name = Name15("center");
    identity.frame = 10;
    identity.curve = {20, 20, 20, 107};
    curveOnly.entries.push_back(identity);
    const std::vector<std::uint8_t> curveOnlyBytes = bsio::EncodeExtension(curveOnly);
    Check(ReadU32(curveOnlyBytes, 8) == bsio::kExtensionVersionV2 &&
              curveOnlyBytes.size() == 32 + 59 + 4,
          "a curve on an entry whose scale is identity selects version 2");
    bsio::VmdExtension decodedCurveOnly;
    Check(bsio::DecodeExtension(curveOnlyBytes.data(), curveOnlyBytes.size(),
                                decodedCurveOnly) == bsio::ExtensionDecode::Ok &&
              decodedCurveOnly.entries.size() == 1 &&
              SameMatrix(decodedCurveOnly.entries[0].scale, bs::Matrix{}) &&
              decodedCurveOnly.entries[0].curve == bs::Curve{20, 20, 20, 107},
          "a curve-only entry round trips with its identity matrix and its curve");

    // An entry that holds neither half is the default state and stays out.
    bsio::VmdExtension defaultOnly;
    bsio::VmdExtension::Entry plainEntry;
    plainEntry.name = Name15("center");
    plainEntry.frame = 10;
    defaultOnly.entries.push_back(plainEntry);
    const std::vector<std::uint8_t> none = bsio::EncodeExtension(defaultOnly);
    Check(ReadU32(none, 8) == bsio::kExtensionVersion && none.size() == 32 + 4,
          "an entry with neither a matrix nor a curve stays out of the payload");

    // The version decides the stride: a version 2 header over 55-byte entries
    // is refused instead of being read with the wrong entry size.
    const std::array<float, 10> entry = {100, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<std::uint8_t> mismatch = HandExtension(
        bsio::kExtensionVersionV2, 264, 0x12345678u, {entry}, {"center"});
    Check(bsio::DecodeExtension(mismatch.data(), mismatch.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "a version 2 payload with version 1 entry sizes is refused");
    // A version 2 payload with the curve bytes cut off is refused too.
    const std::vector<std::uint8_t> shortEntry(v2.begin(), v2.end() - 6);
    Check(bsio::DecodeExtension(shortEntry.data(), shortEntry.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "a version 2 payload with a short entry is refused");
    // A curve byte outside 0..127 is refused like any other bad field.
    const std::vector<std::uint8_t> badCurve = HandExtension(
        bsio::kExtensionVersionV2, 264, 0x12345678u, {entry}, {"center"}, true,
        {{20, 40, 107, 200}});
    Check(bsio::DecodeExtension(badCurve.data(), badCurve.size(), decoded) ==
              bsio::ExtensionDecode::BadData,
          "an extension curve byte above 127 is refused");

    // And the whole thing works through the real file path.
    TempMotion file;
    Check(file.Write(kFakeVmd), "write plain motion prefix");
    bsio::PrefixFingerprint fingerprint;
    fingerprint.Append(kFakeVmd.data(), kFakeVmd.size());
    curved.prefixBytes = fingerprint.bytes();
    curved.prefixCrc32 = fingerprint.crc32();
    int fd = file.Open(_O_WRONLY);
    _lseeki64(fd, 0, SEEK_END);
    Check(bsio::SaveVmdExtension(fd, curved) == bsio::ExtensionSave::Written,
          "a version 2 tail is appended behind the standard prefix");
    Check(_close(fd) == 0, "close the saved version 2 tail");
    bsio::VmdExtension loaded;
    Check(bsio::LoadVmdExtension(file.path(), loaded,
                                 static_cast<std::int64_t>(kFakeVmd.size())) ==
              bsio::ExtensionLoad::Loaded &&
              loaded.entries.size() == 2 &&
              loaded.entries[1].curve == curved.entries[1].curve,
          "a version 2 tail loads back through the file path");
}

void ExtensionMatchingAndOffset() {
    const bsio::VmdExtension extension = ExtensionBlock();
    std::vector<std::array<char, 20>> bones;
    bones.push_back(Name20("center"));
    bones.push_back(Name20("left arm"));
    bones.push_back(Name20("head"));

    std::vector<bsio::ImportedKey> imported;
    bsio::ImportedKey at600;
    at600.bone = 0;
    at600.frame = 600;
    at600.slot = 42;
    bsio::ImportedKey at700;
    at700.bone = 1;
    at700.frame = 700;
    at700.slot = 43;
    imported.push_back(at600);
    imported.push_back(at700);

    // Entries carry rebased frames; the import offset is the current frame.
    const std::vector<bsio::Assignment> shifted =
        bsio::MatchImported(extension, imported, bones, 500);
    Check(shifted.size() == 2, "a matching extension resolves every entry");
    Check(shifted.size() == 2 && shifted[0].slot == 42 && shifted[1].slot == 43,
          "rebased frames resolve against frameOffset + frame");
    Check(shifted.size() == 2 &&
              SameMatrix(shifted[1].scale, extension.entries[1].scale),
          "the matched slot receives the recorded matrix");
    // The entry curve travels with the matrix to the same resolved slot.
    bsio::VmdExtension curved = extension;
    curved.entries[1].curve = {20, 20, 20, 107};
    const std::vector<bsio::Assignment> carried =
        bsio::MatchImported(curved, imported, bones, 500);
    Check(carried.size() == 2 &&
              carried[1].slot == 43 &&
              carried[1].curve == curved.entries[1].curve &&
              bs::Linear(carried[0].curve),
          "the matched slot receives the entry curve next to the matrix");

    const std::vector<bsio::Assignment> unshifted =
        bsio::MatchImported(extension, imported, bones, 0);
    Check(unshifted.empty(),
          "without the import offset nothing lands on the wrong frame");

    // Cross-model matching by name is fine; ambiguous names are skipped.
    std::vector<std::array<char, 20>> twins;
    twins.push_back(Name20("center"));
    twins.push_back(Name20("center"));
    twins.push_back(Name20("left arm"));
    std::vector<bsio::ImportedKey> twinKeys = imported;
    bsio::ImportedKey at700Twins;
    at700Twins.bone = 2;
    at700Twins.frame = 700;
    at700Twins.slot = 43;
    twinKeys.push_back(at700Twins);
    const std::vector<bsio::Assignment> ambiguous =
        bsio::MatchImported(extension, twinKeys, twins, 500);
    Check(ambiguous.size() == 1 && ambiguous[0].slot == 43,
          "an ambiguous bone name is skipped, a unique one still lands");

    std::vector<std::array<char, 20>> renamed;
    renamed.push_back(Name20("other"));
    renamed.push_back(Name20("left arm"));
    const std::vector<bsio::Assignment> renamedMatch =
        bsio::MatchImported(extension, imported, renamed, 500);
    Check(renamedMatch.size() == 1 && renamedMatch[0].slot == 43,
          "a missing bone name is skipped");

    // Two imported keys at one bone and frame: never guess.
    std::vector<bsio::ImportedKey> doubled = imported;
    bsio::ImportedKey duplicate = at600;
    duplicate.slot = 99;
    doubled.push_back(duplicate);
    const std::vector<bsio::Assignment> ambiguousKey =
        bsio::MatchImported(extension, doubled, bones, 500);
    Check(ambiguousKey.size() == 1 && ambiguousKey[0].slot == 43,
          "an ambiguous imported key is skipped");

    // A motion with no extension keeps identity scale everywhere.
    const bsio::VmdExtension none;
    const std::vector<bsio::Assignment> identity =
        bsio::MatchImported(none, imported, bones, 500);
    Check(identity.empty(), "a plain motion resolves to identity scale");

    // A negative import offset is refused outright.
    Check(bsio::MatchImported(extension, imported, bones, -1).empty() &&
              bsio::MatchImported(extension, imported, bones, -500).empty(),
          "a negative frameOffset is refused");

    // frameOffset + entry.frame must not wrap at 32 bits: 0x20 + 0xFFFFFFF0
    // is 0x100000010, never the 0x10 a wrapping sum would hit.
    bsio::VmdExtension wrapped;
    bsio::VmdExtension::Entry nearWrap;
    nearWrap.name = Name15("center");
    nearWrap.frame = 0xFFFFFFF0u;
    nearWrap.scale = Matrix({2, 0, 0, 0, 2, 0, 0, 0, 2});
    wrapped.entries.push_back(nearWrap);
    std::vector<bsio::ImportedKey> wrappedKeys;
    bsio::ImportedKey wrapTarget;
    wrapTarget.bone = 0;
    wrapTarget.frame = 0x10;
    wrapTarget.slot = 77;
    wrappedKeys.push_back(wrapTarget);
    Check(bsio::MatchImported(wrapped, wrappedKeys, bones, 0x20).empty(),
          "frameOffset + frame never wraps into a real key frame");
}

// ---------------------------------------------------------------------------
// The curve-loss boundary: a bone scaled 2x at one key and back to 1 at the
// next keeps the easing of the way back on the second key.  That key's matrix
// is the identity, so the curve is the only thing describing the interval, and
// both channels have to write it, read it back onto the same key slot and
// reproduce the same eased sample.  The version the record key names must also
// decide the expected curve array, or a version 2 record with its curves cut
// off would be read as a version 1 record with them silently dropped.
// ---------------------------------------------------------------------------

void CurveLossBoundary() {
    // ---- PMM: key 0 scales 2x, key 30 returns to 1 with a real curve -------
    bsio::Samples motion;
    bsio::KeySample start;
    start.bone = 3;
    start.name = Name20("center");
    start.frame = 0;
    start.scale = Matrix({2, 0, 0, 0, 2, 0, 0, 0, 2});
    bsio::KeySample back;
    back.bone = 3;
    back.name = Name20("center");
    back.frame = 30;                 // the scale has returned to 1
    back.curve = {20, 20, 20, 107};  // ease in, not linear under bone_scale::Linear
    motion.keys.push_back(start);
    motion.keys.push_back(back);
    Check(bsio::PmmHasCurves(motion),
          "a scale 2 -> scale 1 motion needs the version 2 record");
    const std::vector<std::uint8_t> record = bsio::EncodePmm(motion);
    Check(record.size() == 8 + 2 * 64 + 2 * 4,
          "both keys travel, each with its own four curve bytes");
    bsio::Samples reopened;
    Check(bsio::DecodePmm(record.data(), record.size(), bsio::kPmmVersionV2,
                          reopened) == bsio::PmmDecode::Ok &&
              reopened.keys.size() == 2,
          "the scale 2 -> scale 1 record reopens with both keys");
    Check(SameMatrix(reopened.keys[0].scale, motion.keys[0].scale),
          "the scale 2 key keeps its matrix across the round trip");
    Check(SameMatrix(reopened.keys[1].scale, bs::Matrix{}) &&
              reopened.keys[1].frame == 30 && reopened.keys[1].bone == 3,
          "the return key reopens as the identity matrix on its own frame");
    Check(reopened.keys[1].curve == motion.keys[1].curve &&
              !bs::Linear(reopened.keys[1].curve),
          "the return key keeps its non-linear curve value by value");

    // What the model does with that key slot: the interval that ends at frame
    // 30 is shaped by that key's curve, so an identical curve has to give an
    // identical sample at every point of the interval.
    int sampledSame = 0;
    for (int step = 0; step <= 10; ++step) {
        const float raw = static_cast<float>(step) / 10.0f;
        const float captured = bs::CurveTime(motion.keys[1].curve, raw);
        const float restored = bs::CurveTime(reopened.keys[1].curve, raw);
        if (captured == restored &&
            SameMatrix(bs::Interpolate(motion.keys[0].scale, bs::Matrix{}, captured),
                       bs::Interpolate(reopened.keys[0].scale, bs::Matrix{},
                                       restored)))
            ++sampledSame;
    }
    Check(sampledSame == 11,
          "every sampled point of the interval is identical after the round trip");

    // ---- VMD tail: the same motion, one entry per key ----------------------
    bsio::VmdExtension tail;
    tail.prefixBytes = 264;
    tail.prefixCrc32 = 0x12345678u;
    bsio::VmdExtension::Entry entryStart;
    entryStart.name = Name15("center");
    entryStart.frame = 0;
    entryStart.scale = Matrix({2, 0, 0, 0, 2, 0, 0, 0, 2});
    bsio::VmdExtension::Entry entryBack;
    entryBack.name = Name15("center");
    entryBack.frame = 30;
    entryBack.curve = {20, 20, 20, 107};
    tail.entries.push_back(entryStart);
    tail.entries.push_back(entryBack);
    const std::vector<std::uint8_t> payload = bsio::EncodeExtension(tail);
    Check(ReadU32(payload, 8) == bsio::kExtensionVersionV2,
          "the VMD tail of a scale 2 -> scale 1 motion is version 2");
    bsio::VmdExtension loaded;
    Check(bsio::DecodeExtension(payload.data(), payload.size(), loaded) ==
              bsio::ExtensionDecode::Ok &&
              loaded.entries.size() == 2,
          "the VMD tail reopens with both entries");
    Check(SameMatrix(loaded.entries[0].scale, entryStart.scale) &&
              SameMatrix(loaded.entries[1].scale, bs::Matrix{}) &&
              loaded.entries[1].curve == entryBack.curve,
          "the tail keeps the 2x entry and the curve-only return entry");
    // The imported-key matcher is what the apply step writes from, so the
    // curve-only entry must still resolve to its key slot with its curve.
    std::vector<std::array<char, 20>> bones;
    bones.push_back(Name20("center"));
    std::vector<bsio::ImportedKey> imported;
    bsio::ImportedKey at0;
    at0.bone = 0;
    at0.frame = 0;
    at0.slot = 11;
    bsio::ImportedKey at30;
    at30.bone = 0;
    at30.frame = 30;
    at30.slot = 12;
    imported.push_back(at0);
    imported.push_back(at30);
    const std::vector<bsio::Assignment> assignments =
        bsio::MatchImported(loaded, imported, bones, 0);
    Check(assignments.size() == 2 && assignments[1].slot == 12 &&
              SameMatrix(assignments[1].scale, bs::Matrix{}) &&
              assignments[1].curve == entryBack.curve,
          "the curve-only entry resolves to its key slot with its curve");

    // ---- The suffix, not the size, decides the curve array -----------------
    // Dropping the whole curve array leaves a payload of exactly the version 1
    // size.  Read as version 2 it is a short record; read without the suffix it
    // looks like a version 1 record whose curves are gone.  Every module path
    // passes the version its record key promises, so the short record is
    // refused instead of silently losing the curve.
    const std::vector<std::uint8_t> cut(record.begin(), record.end() - 2 * 4);
    Check(cut.size() == 8 + 2 * 64,
          "the truncated version 2 record has exactly the version 1 size");
    bsio::Samples shortRead;
    Check(bsio::DecodePmm(cut.data(), cut.size(), bsio::kPmmVersionV2,
                          shortRead) == bsio::PmmDecode::BadData,
          "a version 2 record with its last curve cut off is refused");
    Check(shortRead.keys.empty() && shortRead.pose.empty(),
          "a refused record decodes nothing at all");
    Check(bsio::DecodePmm(record.data(), record.size(), bsio::kPmmVersionV1,
                          shortRead) == bsio::PmmDecode::BadData,
          "a version 2 payload under a version 1 key is refused, not trimmed");
    const std::vector<std::uint8_t> oneShort(record.begin(), record.end() - 1);
    Check(bsio::DecodePmm(oneShort.data(), oneShort.size(), bsio::kPmmVersionV2,
                          shortRead) == bsio::PmmDecode::BadData,
          "a version 2 record one byte short is refused");
    // The layout-lenient call has no suffix to trust, and reads the cut payload
    // as the version 1 record its size matches.  That is exactly why no module
    // path uses it: CapturePmm and LoadPmm pass the version the key names.
    Check(bsio::DecodePmm(cut.data(), cut.size(), shortRead) ==
              bsio::PmmDecode::Ok &&
              shortRead.keys.size() == 2 && bs::Linear(shortRead.keys[1].curve),
          "without a suffix the size alone cannot tell a cut version 2 record");
}

// ---------------------------------------------------------------------------
// Extension file lifecycle against real files.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> TailFile(const std::vector<std::uint8_t>& payload) {
    auto bytes=kFakeVmd;
    bytes.insert(bytes.end(),payload.begin(),payload.end());
    PutU64(bytes,payload.size());
    const char footer[]="MMSXVMDX";
    bytes.insert(bytes.end(),footer,footer+8);return bytes;
}
void ExtensionFileLifecycle() {
    TempMotion file;
    Check(file.Write(kFakeVmd),"write plain motion prefix");
    bsio::VmdExtension loaded;
    Check(bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Absent,"plain motion has identity scale");
    auto extension=ExtensionBlock();
    bsio::PrefixFingerprint fingerprint;
    fingerprint.Append(kFakeVmd.data(),17);
    fingerprint.Append(kFakeVmd.data()+17,kFakeVmd.size()-17);
    extension.prefixBytes=fingerprint.bytes();extension.prefixCrc32=fingerprint.crc32();
    Check(extension.prefixBytes==kFakeVmd.size()&&extension.prefixCrc32==RefCrc32(kFakeVmd),"streaming prefix checksum agrees with independent CRC");
    int fd=file.Open(_O_WRONLY);
    Check(bsio::SaveVmdExtension(fd,extension)==bsio::ExtensionSave::Failed,"writer rejects wrong prefix boundary");
    _lseeki64(fd,0,SEEK_END);
    Check(bsio::SaveVmdExtension(fd,extension)==bsio::ExtensionSave::Written,"tail appended to standard prefix");
    Check(_close(fd)==0,"close saved extension");
    const auto good=file.Read();
    const auto payload=bsio::EncodeExtension(extension);
    Check(good==TailFile(payload),"complete file matches independent footer layout");
    Check(bsio::LoadVmdExtension(file.path(),loaded,kFakeVmd.size())==bsio::ExtensionLoad::Loaded&&loaded.entries.size()==2,"single-file tail roundtrip");
    Check(bsio::LoadVmdExtension(file.path(),loaded,kFakeVmd.size()+1)==bsio::ExtensionLoad::Ignored&&loaded.entries.empty(),"independently consumed standard boundary must match");
    auto corrupt=good;corrupt[kFakeVmd.size()+40]^=1;
    Check(file.Write(corrupt)&&bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Ignored&&loaded.entries.empty(),"bad payload checksum refuses all scales");
    corrupt=good;corrupt[40]^=1;
    Check(file.Write(corrupt)&&bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Ignored,"changed standard bytes invalidate tail");
    corrupt=good;corrupt[corrupt.size()-16]=255;
    Check(file.Write(corrupt)&&bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Ignored,"footer size must fit");
    const auto future=HandExtension(3,kFakeVmd.size(),RefCrc32(kFakeVmd),{},{});
    Check(file.Write(TailFile(future))&&bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Ignored,"unknown extension version ignored");
    auto misplaced=extension;misplaced.prefixBytes++;
    Check(file.Write(TailFile(bsio::EncodeExtension(misplaced)))&&bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Ignored,"payload and footer boundaries agree");
    auto truncated=good;truncated.resize(truncated.size()-9);
    Check(file.Write(truncated)&&bsio::LoadVmdExtension(file.path(),loaded)!=bsio::ExtensionLoad::Loaded,"truncated footer never applies scales");
    Check(file.Write(kFakeVmd),"rewrite motion without old tail");
    fd=file.Open(_O_WRONLY);_lseeki64(fd,0,SEEK_END);
    Check(bsio::SaveVmdExtension(fd,{})==bsio::ExtensionSave::Untouched,"ordinary export has no scale tail");_close(fd);
    Check(file.Read()==kFakeVmd&&bsio::LoadVmdExtension(file.path(),loaded)==bsio::ExtensionLoad::Absent,"ordinary save cannot revive old scales");
}

// ---------------------------------------------------------------------------
// VPD pose file round trip through the real SaveVpdFile / LoadVpdFile on a
// hand-built model fixture: no GPU device, no window, no renderer.  What is
// pinned here is the load semantics the codec sections cannot see: a non-unit
// nine-component scale restores exactly, a pose bone without an entry falls
// back to the default identity scale (no residue from the previous pose), a
// bone the pose does not touch keeps its scale and its selection state, one
// undo restores the pre-load scale state, and the selection/registration
// bytes the load writes stay exactly as the stock loader sets them.
// ---------------------------------------------------------------------------

void IdentityTransform(float* m) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

bs::Matrix Diagonal(float x, float y, float z) {
    return {{x, 0, 0, 0, y, 0, 0, 0, z}};
}

// The model fixture bone_scale_tests wires up: a hand-built ModelRecord whose
// bone table and key pool the io code reads without any renderer state.
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    std::vector<mdl::BoneKey> keys = std::vector<mdl::BoneKey>(mdl::kBoneKeyCapacity);
    std::vector<mdl::MorphKey> morphs = std::vector<mdl::MorphKey>(mdl::kMorphKeyCapacity);
    std::vector<mdl::DisplayKey> displays = std::vector<mdl::DisplayKey>(mdl::kDisplayKeyCapacity);
    std::vector<unsigned char> selected, dirty;
    explicit Fixture(int count = 6) : bones(count), selected(count), dirty(count) {
        record->boneCount = count;
        record->boneTable = bones.data();
        record->boneKeys = keys.data();
        record->morphKeys = morphs.data();
        record->displayKeys = displays.data();
        record->boneSelection = selected.data();
        record->bonePhysicsState = dirty.data();
        record->selectedBone = 0;
        record->loadComplete = 1;
        static const char* const kNames[6] = {"hip", "spine", "armL", "armR",
                                              "legL", "legR"};
        for (int i = 0; i < count; ++i) {
            bones[i].parent = -1;
            bones[i].slotIndex = -1;
            // 0..5 never reaches the twist-correction branch of the loader
            // (FixedAxis / UnderIk+flag), so the pose path stays plain.
            bones[i].type = static_cast<mdl::BoneType>(i % 10);
            bones[i].rotQuat[3] = 1;
            keys[i].rotation[3] = 1;
            IdentityTransform(bones[i].matInit);
            IdentityTransform(bones[i].matWorld);
            std::snprintf(bones[i].name, sizeof bones[i].name, "%s",
                          i < 6 ? kNames[i] : "bone");
        }
        std::snprintf(record->name, sizeof record->name, "sample");
    }
    unsigned char* data() { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Fixture() {
        bs::Forget(data());
        for (auto& ring : record->undoRings)
            for (auto& slot : ring.slots) {
                std::free(slot.bonePose);
                std::free(slot.auxiliaryPose);
            }
    }
};

void VpdPoseFileRoundTrip() {
    const bs::Matrix shear =
        Matrix({1.2f, 0.1f, 0.0f, 0.2f, 0.9f, 0.0f, 0.0f, 0.0f, 1.1f});
    const bs::Matrix wide = Diagonal(2.0f, 1.0f, 0.5f);
    const bs::Matrix residueFive = Diagonal(5, 5, 5);
    const bs::Matrix residueFour = Diagonal(4, 4, 4);
    const bs::Matrix outside = Diagonal(3, 3, 3);
    const bs::Matrix identity;

    Fixture f(6);
    mikumikustudio::MMDApp app;
    app.ClearModelSlots();
    app.SetSelectedModelSlot(0);
    app.SelectedModel() = f.data();
    app.PlaybackPhysicsMode() = 0;
    app.Hwnd() = nullptr;
    mikumikustudio::MMDApp* previousBlock = mikumikustudio::g_Block;
    mikumikustudio::g_Block = &app;
    TempMotion file;
    std::int32_t frame = 0;

    // ---- the round trip: a non-unit nine-component scale survives --------
    for (int i = 0; i < 4; ++i) f.selected[i] = 1;  // pose covers hip..armR
    Check(bs::SetPose(f.data(), 1, shear), "install the nine-component scale");
    Check(bs::SetPose(f.data(), 2, wide), "install a second non-unit scale");
    Check(bs::SetPose(f.data(), 4, outside),
          "install a scale on a bone outside the pose");
    mikumikustudio::SaveVpdFile(file.path().c_str());
    vpdio::Extension tail;
    Check(vpdio::LoadTail(file.path(), 4, tail) == vpdio::TailLoad::Loaded &&
              tail.entries.size() == 2,
          "the saved pose carries the scale tail with one entry per scaled "
          "bone");

    // Residue a previous pose left behind on the bones the file is about to
    // load, plus distinctive selection state on the untouched bones.
    Check(bs::SetPose(f.data(), 0, residueFour),
          "leave residue on a pose bone without an entry");
    Check(bs::SetPose(f.data(), 1, residueFive),
          "overwrite the saved scale with residue");
    Check(bs::SetPose(f.data(), 2, identity),
          "clear the saved scale before the load");
    f.selected[4] = 1;
    f.dirty[4] = 0;
    f.selected[5] = 0;
    f.dirty[5] = 1;

    mikumikustudio::LoadVpdFile(file.path().c_str());

    Check(SameMatrix(bs::Pose(f.data(), 1), shear),
          "the nine-component scale restores exactly");
    Check(SameMatrix(bs::Pose(f.data(), 2), wide),
          "the second scale restores exactly");
    Check(SameMatrix(bs::Pose(f.data(), 0), identity),
          "a pose bone without an entry falls back to identity, residue "
          "dropped");
    Check(SameMatrix(bs::Pose(f.data(), 3), identity),
          "a pose bone saved at identity stays at identity");
    Check(SameMatrix(bs::Pose(f.data(), 4), outside),
          "a bone the pose does not touch keeps its scale");
    Check(SameMatrix(bs::Pose(f.data(), 5), identity),
          "an untouched bone without scale stays untouched");
    for (int i = 0; i < 4; ++i)
        Check(f.selected[i] == 1 && f.dirty[i] == 1,
              "the load selects and registers every pose bone");
    Check(f.selected[4] == 1 && f.dirty[4] == 0,
          "an untouched bone keeps its selection state");
    Check(f.selected[5] == 0 && f.dirty[5] == 1,
          "an untouched bone keeps its registration state");
    Check(f.record->undoDirty == 1 && f.record->redoDirty == 0,
          "the load leaves exactly one undo entry");
    Check(f.record->undoRings[0].slots[f.record->undoState[0]].operation == 1,
          "the undo ring holds the pose entry the load registered");

    // ---- one undo restores the pre-load scale state ----------------------
    mikumikustudio::UndoModelEdit(f.data(), &frame);
    Check(SameMatrix(bs::Pose(f.data(), 0), residueFour),
          "one undo restores the pre-load scale of a pose bone");
    Check(SameMatrix(bs::Pose(f.data(), 1), residueFive),
          "one undo restores the residue the load replaced");
    Check(SameMatrix(bs::Pose(f.data(), 2), identity),
          "one undo restores the scale the load cleared");
    Check(SameMatrix(bs::Pose(f.data(), 4), outside),
          "one undo leaves the untouched bone alone");

    // ---- old format: a pose without the tail restores identity -----------
    Fixture plain(6);
    app.SelectedModel() = plain.data();
    for (int i = 0; i < 4; ++i) plain.selected[i] = 1;
    mikumikustudio::SaveVpdFile(file.path().c_str());
    Check(vpdio::LoadTail(file.path(), 4, tail) == vpdio::TailLoad::Absent,
          "a save without scale writes an ordinary VPD with no tail");
    Check(bs::SetPose(plain.data(), 0, residueFour),
          "leave residue on a pose bone of the old-format pose");
    Check(bs::SetPose(plain.data(), 1, shear),
          "leave a scaled pose bone behind");
    Check(bs::SetPose(plain.data(), 4, outside),
          "leave a scale outside the old-format pose");
    mikumikustudio::LoadVpdFile(file.path().c_str());
    Check(SameMatrix(bs::Pose(plain.data(), 0), identity),
          "an old VPD restores identity on a pose bone");
    Check(SameMatrix(bs::Pose(plain.data(), 1), identity),
          "an old VPD drops the previous pose's scale from every pose bone");
    Check(SameMatrix(bs::Pose(plain.data(), 4), outside),
          "an old VPD leaves a bone outside the pose alone");
    for (int i = 0; i < 4; ++i)
        Check(plain.selected[i] == 1 && plain.dirty[i] == 1,
              "an old VPD keeps the selection and registration semantics");

    mikumikustudio::g_Block = previousBlock;
    std::printf("vpd-roundtrip: SaveVpdFile/LoadVpdFile scale semantics pinned\n");
}

}  // namespace

int main() {
    PmmKeyParsing();
    PmmRoundTrip();
    PmmCurveVersioning();
    PmmRejects();
    ExtensionCodecRoundTrip();
    ExtensionCodecRejects();
    ExtensionCurveVersioning();
    ExtensionMatchingAndOffset();
    CurveLossBoundary();
    ExtensionFileLifecycle();
    VpdPoseFileRoundTrip();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf(
        "PASS: %d checks (bone scale PMM record, VMD extension codec and VPD "
        "pose round trip)\n",
        g_checks);
    return 0;
}
