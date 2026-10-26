#include "mikumikustudio/display_motion_snapshot.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/key_pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {

// Retained budget covers the two owning objects plus the actual capacity of
// every vector. All additions and multiplications saturate against the limit.
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride,
            std::uint64_t limit) {
    if (bytes > limit || (stride != 0 && count > (limit - bytes) / stride))
        throw std::length_error("display motion snapshot budget");
    bytes += count * stride;
}

bool ValidTime(double frame) noexcept {
    return std::isfinite(frame) && frame >= 0.0 &&
           frame <= static_cast<double>(frame_limits::maximum);
}

// Native frame seek selects the last key whose frame is not after the request.
// Fractional requests therefore keep the preceding integer key's value.
bool FindKey(const std::vector<std::uint32_t>& frames, double frame,
             std::size_t& index) noexcept {
    const auto after = std::upper_bound(frames.begin(), frames.end(), frame,
        [](double time, std::uint32_t keyFrame) {
            return time < static_cast<double>(keyFrame);
        });
    if (after == frames.begin()) return false;
    index = static_cast<std::size_t>(after - frames.begin()) - 1;
    return true;
}

}  // namespace

struct DisplayMotionSnapshot::Impl {
    std::vector<std::uint32_t> frames;
    std::vector<std::uint8_t> visible;
    std::vector<std::uint8_t> ik;
    std::vector<DisplaySelectorSample> selectors;
    std::vector<std::int32_t> ikBones;
    std::vector<std::int32_t> selectorBones;
    std::uint32_t ikCount = 0;
    std::uint32_t selectorCount = 0;
    std::uint64_t bytes = 0;
};

DisplayMotionSnapshot::DisplayMotionSnapshot() = default;
DisplayMotionSnapshot::~DisplayMotionSnapshot() = default;

std::unique_ptr<DisplayMotionSnapshot> DisplayMotionSnapshot::Capture(
    const unsigned char* source, std::uint64_t limit) {
    if (!source) throw std::invalid_argument("display motion snapshot source");
    const auto& model = *mdl::Mdl(source);
    const mdl::DisplayKey* const pool = model.displayKeys;
    if (!pool) throw std::invalid_argument("display motion snapshot key pool");
    const std::size_t poolCapacity = mdl::KeyPoolCapacity(pool);
    const std::uint64_t int32Max =
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
    if (poolCapacity == 0 ||
        static_cast<std::uint64_t>(poolCapacity) > int32Max)
        throw std::invalid_argument("display motion snapshot key pool bounds");
    const std::uint32_t capacity = static_cast<std::uint32_t>(poolCapacity);
    const std::uint64_t ikCount = model.ikChainCount;
    const std::uint64_t selectorCount = model.boneOrderCount;
    const std::uint64_t boneCount = model.boneCount;
    if (ikCount > int32Max || selectorCount > int32Max ||
        boneCount > int32Max)
        throw std::invalid_argument("display motion snapshot nested counts");
    if (ikCount != 0 && !model.ikChains)
        throw std::invalid_argument("display motion snapshot IK table");
    if (selectorCount != 0 && !model.boneOrderTable)
        throw std::invalid_argument("display motion snapshot selector table");

    // Every snapshot keeps at least one key, so reject an impossible retained
    // budget before walking the mapping tables. The final check below repeats
    // this against the full reachable set and the actual capacities.
    {
        std::uint64_t required = 0;
        Charge(required, 1,
               sizeof(DisplayMotionSnapshot) + sizeof(Impl), limit);
        Charge(required, 1, sizeof(std::uint32_t), limit);
        Charge(required, 1, sizeof(std::uint8_t), limit);
        Charge(required, ikCount, sizeof(std::uint8_t), limit);
        Charge(required, ikCount, sizeof(std::int32_t), limit);
        Charge(required, selectorCount, sizeof(DisplaySelectorSample), limit);
        Charge(required, selectorCount, sizeof(std::int32_t), limit);
    }

    // The mapping tables are copied so sampling never needs the bone or pose
    // tables. Their live targets are validated here, not at sample time.
    const mdl::IkChain* const chains = model.ikChains;
    for (std::uint64_t i = 0; i < ikCount; ++i) {
        const std::int32_t bone = chains[i].boneIndex;
        if (bone < 0 || static_cast<std::uint64_t>(bone) >= boneCount)
            throw std::invalid_argument("display motion snapshot IK bone");
    }
    const auto* const order =
        static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
    for (std::uint64_t i = 0; i < selectorCount; ++i) {
        const std::int32_t bone = order[i].boneIndex;
        if (i == 0) {
            // Slot zero is the whole-model external parent.
            if (bone != -1)
                throw std::invalid_argument(
                    "display motion snapshot selector root");
        } else if (bone < 0 ||
                   static_cast<std::uint64_t>(bone) >= boneCount)
            throw std::invalid_argument("display motion snapshot selector bone");
    }

    // Only the chain reachable from slot zero is motion data. Allocation flags
    // and unused pool slots are irrelevant; unreachable garbage is ignored.
    std::uint64_t keyCount = 0;
    std::uint32_t slot = 0;
    std::uint32_t previousSlot = 0;
    std::uint32_t previousFrame = 0;
    for (;;) {
        if (slot >= capacity)
            throw std::invalid_argument("display motion snapshot chain slot");
        const mdl::DisplayKey& key = pool[slot];
        if (keyCount == 0) {
            if (key.frame != 0)
                throw std::invalid_argument(
                    "display motion snapshot chain head");
        } else if (key.previous != previousSlot ||
                   key.frame <= previousFrame) {
            throw std::invalid_argument("display motion snapshot chain order");
        }
        if (key.previous >= capacity || key.next >= capacity)
            throw std::invalid_argument("display motion snapshot chain link");
        if (key.frame > static_cast<std::uint32_t>(frame_limits::maximum))
            throw std::invalid_argument("display motion snapshot chain frame");
        if (ikCount != 0 && !mdl::IkStates(key))
            throw std::invalid_argument("display motion snapshot IK payload");
        if (selectorCount != 0 && !mdl::SelectorStates(key))
            throw std::invalid_argument(
                "display motion snapshot selector payload");
        ++keyCount;
        if (keyCount > int32Max)
            throw std::invalid_argument("display motion snapshot chain length");
        if (key.next == 0) break;
        previousSlot = slot;
        previousFrame = key.frame;
        slot = key.next;
    }

    const std::uint64_t ikBytes = keyCount * ikCount;
    const std::uint64_t selectorSamples = keyCount * selectorCount;
    const std::uint64_t base =
        sizeof(DisplayMotionSnapshot) + sizeof(Impl);
    // Refuse an impossible retained budget before any sequence is allocated.
    std::uint64_t lower = 0;
    Charge(lower, 1, base, limit);
    Charge(lower, keyCount, sizeof(std::uint32_t), limit);
    Charge(lower, keyCount, sizeof(std::uint8_t), limit);
    Charge(lower, ikBytes, sizeof(std::uint8_t), limit);
    Charge(lower, ikCount, sizeof(std::int32_t), limit);
    Charge(lower, selectorSamples, sizeof(DisplaySelectorSample), limit);
    Charge(lower, selectorCount, sizeof(std::int32_t), limit);

    auto result =
        std::unique_ptr<DisplayMotionSnapshot>(new DisplayMotionSnapshot);
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
    value.ikCount = static_cast<std::uint32_t>(ikCount);
    value.selectorCount = static_cast<std::uint32_t>(selectorCount);
    value.frames.resize(static_cast<std::size_t>(keyCount));
    value.visible.resize(static_cast<std::size_t>(keyCount));
    value.ik.resize(static_cast<std::size_t>(ikBytes));
    value.selectors.resize(static_cast<std::size_t>(selectorSamples));
    value.ikBones.resize(static_cast<std::size_t>(ikCount));
    value.selectorBones.resize(static_cast<std::size_t>(selectorCount));

    // Charge again against the capacities the allocators actually reserved.
    std::uint64_t retained = 0;
    Charge(retained, 1, base, limit);
    Charge(retained, value.frames.capacity(), sizeof(std::uint32_t), limit);
    Charge(retained, value.visible.capacity(), sizeof(std::uint8_t), limit);
    Charge(retained, value.ik.capacity(), sizeof(std::uint8_t), limit);
    Charge(retained, value.selectors.capacity(),
           sizeof(DisplaySelectorSample), limit);
    Charge(retained, value.ikBones.capacity(), sizeof(std::int32_t), limit);
    Charge(retained, value.selectorBones.capacity(), sizeof(std::int32_t),
           limit);
    value.bytes = retained;

    for (std::uint64_t i = 0; i < ikCount; ++i)
        value.ikBones[static_cast<std::size_t>(i)] = chains[i].boneIndex;
    for (std::uint64_t i = 0; i < selectorCount; ++i)
        value.selectorBones[static_cast<std::size_t>(i)] = order[i].boneIndex;

    slot = 0;
    for (std::uint64_t index = 0; index < keyCount; ++index) {
        const mdl::DisplayKey& key = pool[slot];
        value.frames[static_cast<std::size_t>(index)] = key.frame;
        value.visible[static_cast<std::size_t>(index)] = key.visible;
        if (ikCount != 0)
            std::memcpy(value.ik.data() +
                            static_cast<std::size_t>(index * ikCount),
                        mdl::IkStates(key), static_cast<std::size_t>(ikCount));
        const mdl::BoneReference* const pairs = mdl::SelectorStates(key);
        for (std::uint64_t i = 0; i < selectorCount; ++i) {
            DisplaySelectorSample& sample =
                value.selectors[static_cast<std::size_t>(
                    index * selectorCount + i)];
            sample.modelIndex = pairs[i].modelIndex;
            sample.boneIndex = pairs[i].boneIndex;
            sample.windowStart = 0;
            sample.windowEnd = 0;
        }
        if (index + 1 < keyCount) slot = key.next;
    }

    // Windows are the contiguous run of identical parent pairs: the start is
    // the run's first frame, the end is the first frame after it, or zero when
    // the run reaches the chain terminal. This matches the native integer seek
    // windows for exact, between-key and terminal requests.
    for (std::uint64_t i = 0; i < selectorCount; ++i) {
        std::uint64_t start = 0;
        while (start < keyCount) {
            const DisplaySelectorSample& first =
                value.selectors[static_cast<std::size_t>(
                    start * selectorCount + i)];
            const std::int32_t modelIndex = first.modelIndex;
            const std::int32_t boneIndex = first.boneIndex;
            std::uint64_t end = start + 1;
            while (end < keyCount) {
                const DisplaySelectorSample& next =
                    value.selectors[static_cast<std::size_t>(
                        end * selectorCount + i)];
                if (next.modelIndex != modelIndex ||
                    next.boneIndex != boneIndex)
                    break;
                ++end;
            }
            const std::uint32_t windowStart =
                value.frames[static_cast<std::size_t>(start)];
            const std::uint32_t windowEnd =
                end < keyCount
                    ? value.frames[static_cast<std::size_t>(end)]
                    : 0;
            for (std::uint64_t j = start; j < end; ++j) {
                DisplaySelectorSample& sample =
                    value.selectors[static_cast<std::size_t>(
                        j * selectorCount + i)];
                sample.windowStart = windowStart;
                sample.windowEnd = windowEnd;
            }
            start = end;
        }
    }
    return result;
}

std::uint32_t DisplayMotionSnapshot::KeyCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->frames.size()) : 0;
}

bool DisplayMotionSnapshot::KeyFrame(std::uint32_t index, std::uint32_t& out) const noexcept {
    if (!impl_ || index >= impl_->frames.size()) return false;
    out = impl_->frames[index];
    return true;
}

std::uint32_t DisplayMotionSnapshot::IkCount() const noexcept {
    return impl_ ? impl_->ikCount : 0;
}

std::uint32_t DisplayMotionSnapshot::SelectorCount() const noexcept {
    return impl_ ? impl_->selectorCount : 0;
}

std::uint64_t DisplayMotionSnapshot::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

bool DisplayMotionSnapshot::IkBone(std::uint32_t index,
                                   std::int32_t& out) const noexcept {
    if (!impl_ || index >= impl_->ikCount) return false;
    out = impl_->ikBones[index];
    return true;
}

bool DisplayMotionSnapshot::SelectorBone(std::uint32_t index,
                                         std::int32_t& out) const noexcept {
    if (!impl_ || index >= impl_->selectorCount) return false;
    out = impl_->selectorBones[index];
    return true;
}

DisplayMotionStatus DisplayMotionSnapshot::SampleVisibility(
    double frame, std::uint8_t& out) const noexcept {
    if (!impl_ || !ValidTime(frame)) return DisplayMotionStatus::InvalidTime;
    std::size_t key = 0;
    if (!FindKey(impl_->frames, frame, key))
        return DisplayMotionStatus::InvalidIndex;
    out = impl_->visible[key];
    return DisplayMotionStatus::Ok;
}

DisplayMotionStatus DisplayMotionSnapshot::SampleIk(
    int index, double frame, std::uint8_t& out) const noexcept {
    if (!impl_ || index < 0 ||
        static_cast<std::uint32_t>(index) >= impl_->ikCount)
        return DisplayMotionStatus::InvalidIndex;
    if (!ValidTime(frame)) return DisplayMotionStatus::InvalidTime;
    std::size_t key = 0;
    if (!FindKey(impl_->frames, frame, key))
        return DisplayMotionStatus::InvalidIndex;
    out = impl_->ik[key * impl_->ikCount + static_cast<std::uint32_t>(index)];
    return DisplayMotionStatus::Ok;
}

DisplayMotionStatus DisplayMotionSnapshot::SampleSelector(
    int index, double frame, DisplaySelectorSample& out) const noexcept {
    if (!impl_ || index < 0 ||
        static_cast<std::uint32_t>(index) >= impl_->selectorCount)
        return DisplayMotionStatus::InvalidIndex;
    if (!ValidTime(frame)) return DisplayMotionStatus::InvalidTime;
    std::size_t key = 0;
    if (!FindKey(impl_->frames, frame, key))
        return DisplayMotionStatus::InvalidIndex;
    out = impl_->selectors[key * impl_->selectorCount +
                            static_cast<std::uint32_t>(index)];
    return DisplayMotionStatus::Ok;
}

}  // namespace mikumikustudio
#endif
