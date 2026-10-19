// Unit tests for the `.x` accessory block of camera / light VMD files
// (accessory_motion_io.hpp): round trip, rejection of damaged blocks, and the
// by-name, in-order matching rule.
#include "mikumikustudio/accessory_motion_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace io = accessory_motion_io;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(7); }
}

io::Track MakeTrack(const char* name, std::uint32_t firstFrame, int keys) {
    io::Track track;
    track.name = name;
    for (int i = 0; i < keys; ++i) {
        io::Key key;
        key.frame = firstFrame + static_cast<std::uint32_t>(i) * 10;
        key.visible = static_cast<std::uint8_t>(i % 2);
        key.shadowEnabled = 1;
        key.parentModel = i == 0 ? -1 : 2;
        key.parentBone = i;
        key.position[0] = 1.5f * i; key.position[1] = -2.0f; key.position[2] = 3.25f;
        key.rotation[0] = 0.1f; key.rotation[1] = 0.2f * i; key.rotation[2] = -0.3f;
        key.scale = 1.0f + i; key.opacity = 0.5f;
        track.keys.push_back(key);
    }
    return track;
}

bool SameKey(const io::Key& a, const io::Key& b) {
    for (int i = 0; i < 3; ++i)
        if (a.position[i] != b.position[i] || a.rotation[i] != b.rotation[i])
            return false;
    return a.frame == b.frame && a.visible == b.visible &&
           a.shadowEnabled == b.shadowEnabled && a.parentModel == b.parentModel &&
           a.parentBone == b.parentBone && a.scale == b.scale &&
           a.opacity == b.opacity;
}

void RoundTrip() {
    // "stage" in Shift-JIS bytes plus two ASCII names, one repeated.
    const std::vector<io::Track> tracks = {
        MakeTrack("\x83\x58\x83\x65\x81\x5b\x83\x57.x", 0, 3),
        MakeTrack("light.x", 5, 1),
        MakeTrack("light.x", 7, 2)};
    const std::vector<std::uint8_t> block = io::Encode(tracks);
    std::vector<io::Track> decoded;
    Check(io::Decode(block.data(), block.size(), decoded), "block decodes");
    Check(decoded.size() == tracks.size(), "track count survives");
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        Check(decoded[t].name == tracks[t].name, "name bytes survive");
        Check(decoded[t].keys.size() == tracks[t].keys.size(), "key count survives");
        for (std::size_t k = 0; k < tracks[t].keys.size(); ++k)
            Check(SameKey(decoded[t].keys[k], tracks[t].keys[k]), "key fields survive");
    }
    // Trailing bytes after the block (another tool's tail) are tolerated.
    std::vector<std::uint8_t> padded = block;
    padded.insert(padded.end(), {1, 2, 3, 4});
    Check(io::Decode(padded.data(), padded.size(), decoded) && decoded.size() == 3,
          "bytes after the block are ignored");
    std::puts("PASS accessory block round trip");
}

void Rejection() {
    const std::vector<io::Track> tracks = {MakeTrack("a.x", 0, 2)};
    const std::vector<std::uint8_t> block = io::Encode(tracks);
    std::vector<io::Track> decoded;
    std::vector<std::uint8_t> bad = block;
    bad[0] = 'X';
    Check(!io::Decode(bad.data(), bad.size(), decoded) && decoded.empty(),
          "wrong magic is rejected");
    bad = block;
    bad[20] ^= 0x40;
    Check(!io::Decode(bad.data(), bad.size(), decoded), "a damaged payload fails the CRC");
    bad = block;
    bad.resize(bad.size() - 1);
    Check(!io::Decode(bad.data(), bad.size(), decoded), "a truncated block is rejected");
    bad = block;
    bad[8] = 2;
    Check(!io::Decode(bad.data(), bad.size(), decoded), "an unknown version is rejected");
    Check(!io::Decode(nullptr, 0, decoded), "no tail decodes to nothing");
    std::puts("PASS damaged blocks are rejected");
}

void Matching() {
    const std::vector<io::Track> tracks = {
        MakeTrack("A.x", 0, 1), MakeTrack("B.x", 0, 1), MakeTrack("A.x", 0, 1),
        MakeTrack("A.x", 0, 1), MakeTrack("C.x", 0, 1)};
    // Scene in panel order: A, B, A, D.  The file has three A tracks but the
    // scene only two A accessories; C has none; D gets nothing.
    const std::vector<std::string> scene = {"A.x", "B.x", "A.x", "D.x"};
    const std::vector<int> match = io::MatchTracks(scene, tracks);
    Check(match.size() == tracks.size(), "one result per track");
    Check(match[0] == 0 && match[2] == 2,
          "same-name tracks go to same-name accessories in order");
    Check(match[1] == 1, "a unique name matches its accessory");
    Check(match[3] == -1, "a surplus same-name track is dropped, nothing is added");
    Check(match[4] == -1, "a track without an accessory is dropped");

    // More accessories than tracks: the earlier ones receive the motion.
    const std::vector<std::string> many = {"A.x", "A.x", "A.x"};
    const std::vector<int> few = io::MatchTracks(many, {MakeTrack("A.x", 0, 1)});
    Check(few.size() == 1 && few[0] == 0,
          "with extra accessories the first in order receives the track");
    std::puts("PASS by-name, in-order matching");
}
}  // namespace

int main() {
    RoundTrip();
    Rejection();
    Matching();
    std::puts("PASS: accessory motion io");
    return 0;
}
