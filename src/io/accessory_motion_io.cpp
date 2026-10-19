#include "mikumikustudio/key_pool.hpp"
// MMS addition: `.x` accessory motion inside a camera / light VMD.  The block
// format and the matching rule are documented in accessory_motion_io.hpp.
#include "mikumikustudio/accessory_motion_io.hpp"

#include <algorithm>
#include <cstring>
#include <map>

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {

// accessory_paste.cpp: pastes one clipboard record at the current frame plus
// its offset into the record's own slot (useSelectedSlot == 0).  Returns 0 when
// the 10000-key table is full (the overflow box has been shown).
int PasteAccessoryKeyRecord(MMDApp* app, void* recData, int useSelectedSlot);

namespace accessory_motion_io {
namespace {

constexpr std::size_t kHeaderBytes = 8 + 4 + 4;
constexpr std::size_t kMaxNameBytes = 99;
constexpr std::uint32_t kKeyCapacity = static_cast<std::uint32_t>(mdl::KeyPoolLimit<mdl::AccessoryKey>());

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

template <typename T>
void Put(std::vector<std::uint8_t>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    template <typename T>
    bool Get(T& value) {
        if (size - at < sizeof(T)) return false;
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
};

// Loaded accessories in panel (combo) order.
std::vector<int> SceneSlots(MMDApp* app) {
    std::vector<int> slots;
    for (int slot = 0; slot < 255; ++slot)
        if (app->AccessorySlot(slot) != nullptr) slots.push_back(slot);
    std::stable_sort(slots.begin(), slots.end(), [app](int a, int b) {
        return app->AccessorySlot(a)->order < app->AccessorySlot(b)->order;
    });
    return slots;
}

std::string AccessoryName(const mdl::AccessoryRecord& record) {
    std::size_t length = 0;
    while (length < sizeof(record.name) && length < kMaxNameBytes &&
           record.name[length] != '\0')
        ++length;
    return std::string(record.name, length);
}

}  // namespace

std::vector<std::uint8_t> Encode(const std::vector<Track>& tracks) {
    std::vector<std::uint8_t> payload;
    Put(payload, static_cast<std::uint32_t>(tracks.size()));
    for (const Track& track : tracks) {
        const std::size_t nameLength = std::min(track.name.size(), kMaxNameBytes);
        payload.push_back(static_cast<std::uint8_t>(nameLength));
        payload.insert(payload.end(), track.name.begin(),
                       track.name.begin() + static_cast<std::ptrdiff_t>(nameLength));
        Put(payload, static_cast<std::uint32_t>(track.keys.size()));
        for (const Key& key : track.keys) {
            Put(payload, key.frame);
            Put(payload, key.visible);
            Put(payload, key.shadowEnabled);
            Put(payload, key.parentModel);
            Put(payload, key.parentBone);
            for (float v : key.position) Put(payload, v);
            for (float v : key.rotation) Put(payload, v);
            Put(payload, key.scale);
            Put(payload, key.opacity);
        }
    }
    std::vector<std::uint8_t> block(kMagic, kMagic + sizeof(kMagic));
    Put(block, kVersion);
    Put(block, static_cast<std::uint32_t>(payload.size()));
    block.insert(block.end(), payload.begin(), payload.end());
    Put(block, Crc32(payload.data(), payload.size()));
    return block;
}

bool Decode(const std::uint8_t* data, std::size_t size,
            std::vector<Track>& tracks) {
    tracks.clear();
    if (data == nullptr || size < kHeaderBytes + 4 ||
        std::memcmp(data, kMagic, sizeof(kMagic)) != 0)
        return false;
    std::uint32_t version = 0, payloadBytes = 0;
    std::memcpy(&version, data + 8, 4);
    std::memcpy(&payloadBytes, data + 12, 4);
    if (version != kVersion || payloadBytes > size - kHeaderBytes - 4)
        return false;
    const std::uint8_t* payload = data + kHeaderBytes;
    std::uint32_t crc = 0;
    std::memcpy(&crc, payload + payloadBytes, 4);
    if (crc != Crc32(payload, payloadBytes))
        return false;

    Reader in{payload, payloadBytes};
    std::uint32_t trackCount = 0;
    if (!in.Get(trackCount) || trackCount > 255) return false;
    std::vector<Track> parsed(trackCount);
    for (Track& track : parsed) {
        std::uint8_t nameLength = 0;
        if (!in.Get(nameLength) || nameLength > kMaxNameBytes ||
            in.size - in.at < nameLength)
            return false;
        track.name.assign(reinterpret_cast<const char*>(payload + in.at), nameLength);
        in.at += nameLength;
        std::uint32_t keyCount = 0;
        if (!in.Get(keyCount) || keyCount > kKeyCapacity ||
            (in.size - in.at) / kKeyBytes < keyCount)
            return false;
        track.keys.resize(keyCount);
        for (Key& key : track.keys) {
            bool ok = in.Get(key.frame) && in.Get(key.visible) &&
                      in.Get(key.shadowEnabled) && in.Get(key.parentModel) &&
                      in.Get(key.parentBone);
            for (float& v : key.position) ok = ok && in.Get(v);
            for (float& v : key.rotation) ok = ok && in.Get(v);
            ok = ok && in.Get(key.scale) && in.Get(key.opacity);
            if (!ok) return false;
        }
    }
    if (in.at != in.size) return false;
    tracks = std::move(parsed);
    return true;
}

std::vector<int> MatchTracks(const std::vector<std::string>& sceneNames,
                             const std::vector<Track>& tracks) {
    std::map<std::string, std::vector<int>> byName;
    for (int i = 0; i < static_cast<int>(sceneNames.size()); ++i)
        byName[sceneNames[static_cast<std::size_t>(i)]].push_back(i);
    std::map<std::string, std::size_t> used;
    std::vector<int> result;
    result.reserve(tracks.size());
    for (const Track& track : tracks) {
        const auto found = byName.find(track.name);
        std::size_t& next = used[track.name];
        if (found != byName.end() && next < found->second.size())
            result.push_back(found->second[next++]);
        else
            result.push_back(-1);
    }
    return result;
}

std::vector<Track> CaptureSelected(MMDApp* app, std::uint32_t& minFrame) {
    std::vector<Track> tracks;
    if (app == nullptr) return tracks;
    for (int slot : SceneSlots(app)) {
        const mdl::AccessoryKey* keys = app->AccessoryKeys(slot);
        const auto capacity = mdl::KeyPoolCapacity(keys);
        if (keys == nullptr) continue;
        Track track;
        track.name = AccessoryName(*app->AccessorySlot(slot));
        std::uint32_t index = 0;
        for (std::uint32_t hops = 0; hops < capacity; ++hops) {
            const mdl::AccessoryKey& source = keys[index];
            if (source.selected != 0) {
                Key key;
                key.frame = source.frame;
                key.visible = source.visible;
                key.shadowEnabled = source.shadowEnabled;
                key.parentModel = source.parentModel;
                key.parentBone = source.parentBone;
                std::memcpy(key.position, source.position, sizeof(key.position));
                std::memcpy(key.rotation, source.rotation, sizeof(key.rotation));
                key.scale = source.scale;
                key.opacity = source.opacity;
                track.keys.push_back(key);
                minFrame = std::min(minFrame, source.frame);
            }
            index = source.next;
            if (index == 0 || index >= capacity) break;
        }
        if (!track.keys.empty()) tracks.push_back(std::move(track));
    }
    return tracks;
}

bool AnySelected(MMDApp* app) {
    std::uint32_t minFrame = 0xFFFFFFFFu;
    return !CaptureSelected(app, minFrame).empty();
}

int Apply(MMDApp* app, const std::vector<Track>& tracks) {
    if (app == nullptr || tracks.empty()) return 0;
    const std::vector<int> slots = SceneSlots(app);
    std::vector<std::string> names;
    names.reserve(slots.size());
    for (int slot : slots) names.push_back(AccessoryName(*app->AccessorySlot(slot)));
    const std::vector<int> match = MatchTracks(names, tracks);

    int applied = 0;
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        if (match[t] < 0) continue;
        const int slot = slots[static_cast<std::size_t>(match[t])];
        ++applied;
        for (const Key& key : tracks[t].keys) {
            mdl::AccessoryClipboardKey record{};
            record.frameOffset = key.frame;
            record.slot = static_cast<std::uint8_t>(slot);
            record.visible = key.visible;
            record.shadowEnabled = key.shadowEnabled;
            // A parent names a model slot of the saving scene; keep it only
            // when this scene has a model there.
            const bool parentExists = key.parentModel >= 0 &&
                                      key.parentModel < kModelSlotCount &&
                                      app->ModelSlot(key.parentModel) != nullptr;
            record.parentModel = parentExists ? key.parentModel : -1;
            record.parentBone = parentExists ? key.parentBone : 0;
            std::memcpy(record.position, key.position, sizeof(record.position));
            std::memcpy(record.rotation, key.rotation, sizeof(record.rotation));
            record.scale = key.scale;
            record.opacity = key.opacity;
            if (PasteAccessoryKeyRecord(app, &record, 0) == 0) break;  // table full
        }
        ApplyAccessoryTrack(app, slot);
    }
    return applied;
}

}  // namespace accessory_motion_io
}  // namespace mikumikustudio
