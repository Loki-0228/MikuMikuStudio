#include "native_camera_interpolation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
using Key = mdl::CameraKey;
using Controls = std::array<std::array<std::uint8_t, 4>, 6>;

void AddBytes(std::uint64_t& total, std::uint64_t count, std::uint64_t stride = 1) noexcept {
    total = stride && count > (UINT64_MAX - total) / stride ? UINT64_MAX : total + count * stride;
}

struct KeySnapshot {
    std::uint32_t slot;
    Key key;
};

// Only occupied keys belong to the track. Keep snapshots in slot order so a
// target view can resolve a link without reading mutable live key contents.
std::vector<KeySnapshot> CaptureKeys(const Key* pool, std::size_t capacity, bool requireMarkedKeys) {
    if (!pool || !capacity || capacity > mdl::KeyPoolLimit<Key>())
        throw std::invalid_argument("camera key pool bounds");
    std::vector<KeySnapshot> keys;
    std::vector<unsigned char> visited(capacity, 0);
    std::uint32_t slot = 0, previous = 0, lastFrame = 0;
    for (std::size_t hops = 0;; ++hops) {
        if (hops >= capacity || slot >= capacity || visited[slot])
            throw std::invalid_argument("camera key cycle");
        const auto& key = pool[slot];
        if (key.frame > frame_limits::maximum ||
            (!hops && (key.frame || key.previous)) ||
            (hops && (key.frame <= lastFrame || key.previous != previous)))
            throw std::invalid_argument("camera key order");
        if (!std::isfinite(key.distance)) throw std::invalid_argument("camera key distance");
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!std::isfinite(key.eye[axis]) || !std::isfinite(key.target[axis]))
                throw std::invalid_argument("camera key pose");
        for (const auto& point : key.interpolation)
            for (auto value : point)
                if (value > 127) throw std::invalid_argument("camera key controls");
        visited[slot] = 1;
        keys.push_back({slot, key});
        if (!key.next) break;
        previous = slot;
        lastFrame = key.frame;
        slot = key.next;
    }
    for (std::size_t slotIndex = 1; slotIndex < capacity; ++slotIndex)
        if (!visited[slotIndex] && (pool[slotIndex].frame ||
            (requireMarkedKeys && pool[slotIndex].selected)))
            throw std::invalid_argument("orphan or marked unused camera key");
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    return keys;
}

bool SameKey(Key a, Key b, bool selection) noexcept {
    if (!selection) a.selected = b.selected = 0;
    return std::memcmp(&a, &b, sizeof(Key)) == 0;
}

bool Matches(const Key* pool, std::size_t capacity, const std::vector<KeySnapshot>& snapshot,
             bool selection) noexcept {
    std::size_t next = 0;
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        if (next < snapshot.size() && snapshot[next].slot == slot) {
            if (!SameKey(pool[slot], snapshot[next++].key, selection)) return false;
        } else if (pool[slot].frame || (selection && pool[slot].selected)) return false;
    }
    return next == snapshot.size();
}

Controls ReadControls(const Key& key) noexcept {
    Controls result{};
    for (unsigned channel = 0; channel < result.size(); ++channel)
        for (unsigned point = 0; point < result[channel].size(); ++point)
            result[channel][point] = key.interpolation[point][channel];
    return result;
}

bool SameControls(const Key& key, const Controls& expected, std::uint8_t channels) noexcept {
    for (unsigned channel = 0; channel < expected.size(); ++channel)
        if (channels & (1u << channel))
            for (unsigned point = 0; point < expected[channel].size(); ++point)
                if (key.interpolation[point][channel] != expected[channel][point]) return false;
    return true;
}

void WriteControls(Key& key, const Controls& values, std::uint8_t channels) noexcept {
    for (unsigned channel = 0; channel < values.size(); ++channel)
        if (channels & (1u << channel))
            for (unsigned point = 0; point < values[channel].size(); ++point)
                key.interpolation[point][channel] = values[channel][point];
}

struct InterpolationChange final : HistoryChange, std::enable_shared_from_this<InterpolationChange> {
    struct Patch {
        std::uint32_t slot, frame;
        std::uint8_t channels;
        Controls before, after;
    };
    MMDApp& app;
    std::uint64_t identity;
    std::size_t capacity;
    std::uint64_t generation = 1;
    bool published = false, applied = false;
    std::vector<KeySnapshot> initialKeys;
    std::vector<Patch> patches;
    curve_host::TrackAuthoring authoring;

    explicit InterpolationChange(MMDApp& value)
        : app(value), identity(mdl::KeyPoolIdentity(value.CameraKeys())),
          capacity(mdl::KeyPoolCapacity(value.CameraKeys())), authoring(value, curve_host::Kind::Camera) {}

    bool Structure() const {
        return mdl::KeyPoolIdentity(app.CameraKeys()) == identity &&
            mdl::KeyPoolCapacity(app.CameraKeys()) >= capacity;
    }

    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<InterpolationChange> source;
        bool forward, first, consumed = false;
        std::uint64_t generation;
        Key* pool;
        std::size_t capacity;
        std::vector<KeySnapshot> before, target;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves;

        Candidate(std::shared_ptr<InterpolationChange> value, bool direction)
            : source(std::move(value)), forward(direction), first(!source->published),
              generation(source->generation), pool(source->app.CameraKeys()), capacity(mdl::KeyPoolCapacity(pool)) {}

        const Key* Target(std::uint32_t slot) const noexcept {
            const auto found = std::lower_bound(target.begin(), target.end(), slot,
                [](const auto& key, std::uint32_t index) { return key.slot < index; });
            return found != target.end() && found->slot == slot ? &found->key : nullptr;
        }

        bool IsCurrent() const noexcept override {
            try {
                return !consumed && generation == source->generation && forward != source->applied &&
                    source->Structure() && source->app.CameraKeys() == pool &&
                    mdl::KeyPoolCapacity(pool) == capacity && Matches(pool, capacity, before, first) &&
                    curves && curves->IsCurrent();
            } catch (...) { return false; }
        }

        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (const auto& patch : source->patches)
                WriteControls(pool[patch.slot], forward ? patch.after : patch.before, patch.channels);
            // The full transaction was validated before the first native write.
            // Publish accepts the final pool pointer and does not resample pose.
            curves->Publish(pool);
            source->published = true;
            source->applied = forward;
            ++source->generation;
            consumed = true;
        }
    };

    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!Structure() || !authoring.IsCurrent() || forward == applied) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            auto* pool = app.CameraKeys();
            const auto currentCapacity = mdl::KeyPoolCapacity(pool);
            if (!published && !Matches(pool, currentCapacity, initialKeys, true)) return MMS_E_STALE;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            candidate->before = CaptureKeys(pool, currentCapacity, false);
            candidate->target = candidate->before;
            for (const auto& patch : patches) {
                const auto found = std::lower_bound(candidate->target.begin(), candidate->target.end(), patch.slot,
                    [](const auto& key, std::uint32_t slot) { return key.slot < slot; });
                if (found == candidate->target.end() || found->slot != patch.slot || found->key.frame != patch.frame ||
                    !SameControls(found->key, forward ? patch.before : patch.after, patch.channels)) return MMS_E_STALE;
                WriteControls(found->key, forward ? patch.after : patch.before, patch.channels);
            }
            curve_host::NativeKeyView view{candidate.get(), currentCapacity,
                [](const void* context, std::uint32_t slot) noexcept -> const void* {
                    return static_cast<const Candidate*>(context)->Target(slot);
                }};
            candidate->curves = authoring.Prepare(view);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate);
            return MMS_OK;
        });
    }

    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        AddBytes(bytes, initialKeys.capacity(), sizeof(KeySnapshot));
        AddBytes(bytes, patches.capacity(), sizeof(Patch));
        const auto authorBytes = authoring.RetainedBytes();
        // TrackAuthoring itself is already part of sizeof(*this).
        AddBytes(bytes, authorBytes == UINT64_MAX ? UINT64_MAX : authorBytes - sizeof(authoring));
        return bytes;
    }
};
}

MmsResult PlanCameraInterpolationEdit(MMDApp& app, const CameraInterpolationOptions& options,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (options.channels & ~63u) return MMS_E_ARGUMENT;
    for (unsigned channel = 0; channel < options.values.size(); ++channel)
        if (options.channels & (1u << channel))
            for (auto value : options.values[channel]) if (value > 127) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        if (app.PlaybackActive() || app.FrameStepPlayback() || app.state.recordPlaybackStartPending ||
            app.SceneMutationInProgress() || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        if (!options.channels) return MMS_OK;
        auto* pool = app.CameraKeys();
        const auto capacity = mdl::KeyPoolCapacity(pool);
        bool selected = false;
        for (std::size_t slot = 0; slot < capacity; ++slot) selected |= pool[slot].selected != 0;
        if (!selected) return MMS_OK;
        auto initial = CaptureKeys(pool, capacity, true);
        std::vector<InterpolationChange::Patch> patches;
        for (const auto& key : initial) {
            if (!key.key.selected) continue;
            const auto before = ReadControls(key.key);
            auto after = before;
            std::uint8_t changed = 0;
            for (unsigned channel = 0; channel < options.values.size(); ++channel)
                if ((options.channels & (1u << channel)) && before[channel] != options.values[channel]) {
                    after[channel] = options.values[channel];
                    changed |= std::uint8_t(1u << channel);
                }
            if (changed) patches.push_back({key.slot, key.key.frame, changed, before, after});
        }
        if (patches.empty()) return MMS_OK;
        auto change = std::make_shared<InterpolationChange>(app);
        change->initialKeys = std::move(initial);
        change->patches = std::move(patches);
        out = std::move(change);
        return MMS_OK;
    });
}
}
#endif
