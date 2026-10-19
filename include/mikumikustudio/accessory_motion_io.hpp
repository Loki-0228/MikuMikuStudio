#pragma once
// MMS addition: `.x` accessory motion inside a camera / light VMD.
//
// A camera-mode motion save writes the standard VMD sections unchanged and then
// appends one block with the selected keyframes of every `.x` accessory that
// has any.  Readers that stop at the standard end code (the original MMD and
// other tools) ignore the block.  The block is self-delimiting and checked:
//
//   magic    "MMSXACC1"                                8 bytes
//   version  u32 = 1
//   size     u32  payload byte count
//   payload  u32 trackCount, then per track:
//              u8 nameLength, name bytes (the accessory name as MMD stores it,
//              Shift-JIS, without the terminator), u32 keyCount, then keyCount
//              records of 46 bytes:
//                u32 frame (rebased by the motion's minimum frame)
//                u8 visible, u8 shadow, i32 parentModel, i32 parentBone,
//                f32 position[3], f32 rotation[3], f32 scale, f32 opacity
//   crc      u32  CRC-32 (IEEE) of the payload
//
// Loading matches tracks to the scene's accessories by name, in order: the
// k-th track named N goes to the k-th accessory named N in panel order.  A
// track without a counterpart is dropped; accessories left over receive no
// motion.  Keys are pasted at the current frame plus their stored offset,
// like the camera and light keys of the same file.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio {
class MMDApp;

namespace accessory_motion_io {

inline constexpr char kMagic[8] = {'M', 'M', 'S', 'X', 'A', 'C', 'C', '1'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kKeyBytes = 46;

struct Key {
    std::uint32_t frame = 0;
    std::uint8_t visible = 1;
    std::uint8_t shadowEnabled = 0;
    std::int32_t parentModel = -1;
    std::int32_t parentBone = 0;
    float position[3] = {};
    float rotation[3] = {};
    float scale = 1.0f;
    float opacity = 1.0f;
};

struct Track {
    std::string name;  // Shift-JIS bytes, at most 99
    std::vector<Key> keys;
};

// Serialises `tracks` into one block; frames are written as stored.
std::vector<std::uint8_t> Encode(const std::vector<Track>& tracks);

// Parses a block at the start of `data`.  Returns false (leaving `tracks`
// empty) when the bytes are not a complete, intact block of this version.
bool Decode(const std::uint8_t* data, std::size_t size,
            std::vector<Track>& tracks);

// For every track, the index into `sceneNames` (scene accessories in panel
// order) that receives it, or -1.  See the header comment for the rule.
std::vector<int> MatchTracks(const std::vector<std::string>& sceneNames,
                             const std::vector<Track>& tracks);

// Selected keys of every loaded accessory, in panel order.  Lowers
// `minFrame` to the smallest selected frame; frames stay absolute.
std::vector<Track> CaptureSelected(MMDApp* app, std::uint32_t& minFrame);

// True when any loaded accessory has a selected key.
bool AnySelected(MMDApp* app);

// Pastes the decoded tracks into the matching accessories at the current
// frame.  Returns the number of tracks that found an accessory.
int Apply(MMDApp* app, const std::vector<Track>& tracks);

}  // namespace accessory_motion_io
}  // namespace mikumikustudio
