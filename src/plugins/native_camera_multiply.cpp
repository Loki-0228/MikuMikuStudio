#include "native_camera_multiply.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
using Key = mdl::CameraKey;
using Pose = std::array<double, 12>;
constexpr double nativePi = 3.141592025756836;
constexpr double authoredPi = 3.14159265358979323846;
struct Snapshot { std::uint32_t slot; Key key; };
struct Values {
    std::array<float, 7> scalar{};
    std::int32_t fov = 0;
};
Values ReadValues(const Key& key) noexcept {
    return {{key.eye[0], key.eye[1], key.eye[2], key.target[0], key.target[1], key.target[2], key.distance}, key.fov};
}
bool SameValues(const Values& a, const Values& b) noexcept {
    return a.fov == b.fov && !std::memcmp(a.scalar.data(), b.scalar.data(), sizeof(a.scalar));
}
void WriteValues(Key& key, const Values& value) noexcept {
    for (unsigned axis = 0; axis < 3; ++axis) { key.eye[axis] = value.scalar[axis]; key.target[axis] = value.scalar[axis + 3]; }
    key.distance = value.scalar[6]; key.fov = value.fov;
}
void Add(std::uint64_t& total, std::uint64_t count, std::uint64_t stride = 1) noexcept {
    total = stride && count > (UINT64_MAX - total) / stride ? UINT64_MAX : total + count * stride;
}
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool authorized = replay && NativeObjectPublicationAuthorized(&app);
    return (!bake::UiLocked() || authorized) && (!app.SceneMutationInProgress() || authorized) &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture();
}
Pose ReadPose(MMDApp& app) noexcept {
    Pose pose{};
    for (unsigned axis = 0; axis < 3; ++axis) { pose[axis] = app.CameraPositionAxis(axis); pose[axis + 3] = app.CameraRotation()[axis]; }
    pose[6] = app.CameraDistance(); pose[7] = app.CameraFov(); pose[8] = app.CameraPerspective();
    pose[9] = app.CameraParentModel(); pose[10] = app.CameraParentBone(); return pose;
}
bool Finite(const Pose& pose) noexcept { for (auto value : pose) if (!std::isfinite(value)) return false; return true; }
void WritePose(MMDApp& app, const Pose& pose) noexcept {
    if (app.CameraDistance() != float(pose[6]) || app.CameraFov() != float(pose[7])) app.cameraProjectionRefreshPending = true;
    for (unsigned axis = 0; axis < 3; ++axis) { app.CameraPositionAxis(axis) = float(pose[axis]); app.CameraRotation()[axis] = float(pose[axis + 3]); }
    app.CameraDistance() = float(pose[6]); app.CameraFov() = float(pose[7]);
}
std::vector<Snapshot> Capture(const Key* pool, std::size_t capacity, bool selectedSpare) {
    if (!pool || !capacity || capacity > mdl::KeyPoolLimit<Key>()) throw std::invalid_argument("camera multiply pool bounds");
    std::vector<unsigned char> visited(capacity);
    std::vector<Snapshot> result;
    unsigned slot = 0, previous = 0, frame = 0;
    for (std::size_t hops = 0;; ++hops) {
        if (hops >= capacity || slot >= capacity || visited[slot]) throw std::invalid_argument("camera multiply cyclic chain");
        const auto& key = pool[slot];
        if (key.frame > frame_limits::maximum || (!hops && (key.frame || key.previous)) ||
            (hops && (key.frame <= frame || key.previous != previous))) throw std::invalid_argument("camera multiply key order");
        if (!std::isfinite(key.distance)) throw std::invalid_argument("camera multiply nonfinite distance");
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!std::isfinite(key.eye[axis]) || !std::isfinite(key.target[axis])) throw std::invalid_argument("camera multiply nonfinite pose");
        for (const auto& point : key.interpolation) for (auto value : point)
            if (value > 127) throw std::invalid_argument("camera multiply invalid easing");
        visited[slot] = 1; result.push_back({slot, key});
        if (!key.next) break;
        previous = slot; frame = key.frame; slot = key.next;
    }
    for (std::size_t i = 1; i < capacity; ++i)
        if (!visited[i] && (pool[i].frame || (selectedSpare && pool[i].selected))) throw std::invalid_argument("camera multiply orphan or selected spare");
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; }); return result;
}
bool Matches(const Key* pool, std::size_t capacity, const std::vector<Snapshot>& snapshot, bool selection, bool selectedSpare = false) noexcept {
    std::size_t next = 0;
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        if (next < snapshot.size() && snapshot[next].slot == slot) {
            auto a = pool[slot], b = snapshot[next++].key;
            if (!selection) a.selected = b.selected = 0;
            if (std::memcmp(&a, &b, sizeof(Key))) return false;
        } else if (pool[slot].frame || (selectedSpare && pool[slot].selected)) return false;
    }
    return next == snapshot.size();
}
const Key* At(const std::vector<Snapshot>& keys, unsigned slot) noexcept {
    const auto at = std::lower_bound(keys.begin(), keys.end(), slot, [](const auto& key, unsigned value) { return key.slot < value; });
    return at != keys.end() && at->slot == slot ? &at->key : nullptr;
}
curve_host::NativeKeyView View(const std::vector<Snapshot>& keys, std::size_t capacity) noexcept {
    return {&keys, capacity, [](const void* context, std::uint32_t slot) noexcept -> const void* {
        return At(*static_cast<const std::vector<Snapshot>*>(context), slot);
    }};
}
float Ease(const Key& key, unsigned lane, float time) noexcept {
    const auto& c = key.interpolation;
    if (c[0][lane] == c[1][lane] && c[2][lane] == c[3][lane]) return time;
    const float x1 = float(c[0][lane]) / 127.f, x2 = float(c[2][lane]) / 127.f;
    float u = .5f, half = .25f;
    for (unsigned i = 0; i < 12; ++i) {
        const float v = 1 - u, v3 = v * 3;
        const float x = (v3 * v * u) * x1 + (v3 * u * u) * x2 + u * u * u;
        if (x == time) break;
        u = x >= time ? u - half : u + half; half *= .5f;
    }
    const float v = 1 - u, v3 = v * 3;
    return (v3 * v * u) * float(c[1][lane]) / 127.f + (v3 * u * u) * float(c[3][lane]) / 127.f + u * u * u;
}
Pose NativePose(const std::vector<Snapshot>& keys, unsigned frame) {
    auto* b = At(keys, 0);
    while (b->frame < frame && b->next) b = At(keys, b->next);
    const bool between = b->frame > frame;
    const auto& a = between ? *At(keys, b->previous) : *b;
    const auto first = ReadValues(a), last = ReadValues(*b);
    Pose pose{}; for (unsigned i = 0; i < 7; ++i) pose[i] = first.scalar[i]; pose[7] = float(a.fov);
    pose[8] = a.perspective; pose[9] = a.parentModel; pose[10] = a.parentBone;
    if (between && b->frame - a.frame > 1) {
        const float fraction = float(double(frame - a.frame) / double(b->frame - a.frame));
        for (unsigned i = 0; i < 8; ++i) {
            const unsigned lane = i < 3 ? i : i < 6 ? 3 : i == 6 ? 4 : 5;
            const double start = i < 7 ? first.scalar[i] : double(first.fov), end = i < 7 ? last.scalar[i] : double(last.fov);
            pose[i] = float(double(Ease(*b, lane, fraction)) * (end - start) + start);
        }
    }
    return pose;
}
Pose Sample(const curve_host::TrackAuthoring::Target& curves, const std::vector<Snapshot>& keys, unsigned frame) {
    if (!curves.Authored()) return NativePose(keys, frame);
    Pose result{};
    if (!curves.Sample(frame, result)) throw std::invalid_argument("camera multiply authored sample");
    for (unsigned i = 3; i < 6; ++i) result[i] = float(result[i] * authoredPi / 180);
    for (auto& value : result) value = double(float(value));
    return result;
}
Values Transform(const Values& before, const CameraMultiplyOptions& options) {
    auto after = before;
    for (unsigned lane = 0; lane < 7; ++lane) {
        float delta = options.offset[lane];
        if (lane >= 3 && lane < 6) delta = float((lane == 3 ? -double(delta) : double(delta)) * nativePi / 180.0);
        if (options.scale[lane] != 1.f || delta != 0.f) after.scalar[lane] = before.scalar[lane] * options.scale[lane] + delta;
        if (!std::isfinite(after.scalar[lane])) throw std::invalid_argument("camera multiply floating overflow");
    }
    if (options.scale[7] != 1.f || options.offset[7] != 0.f) {
        const double value = double(before.fov) * options.scale[7] + options.offset[7];
        // File IO and the original command carry an int32, without a 1..179
        // clamp. Keep that domain, but reject undefined floating conversions.
        const double truncated = std::trunc(value);
        if (!std::isfinite(value) || truncated < INT32_MIN || truncated > INT32_MAX) throw std::invalid_argument("camera multiply FOV overflow");
        after.fov = static_cast<std::int32_t>(value);
    }
    return after;
}
}

struct NativeCameraMultiplyPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    struct Patch { unsigned slot, frame; Values before, after; };
    MMDApp& app;
    SharedHistory& history;
    const std::uint64_t binding, poolIdentity;
    const std::size_t capacity;
    const int initialFrame;
    const Pose initialPose;
    const std::uint32_t initialCursor;
    const unsigned char initialActive;
    std::uint64_t serial = 1;
    bool read = false, published = false, applied = false;
    std::vector<Snapshot> original;
    std::vector<Patch> patches;
    curve_host::TrackAuthoring authoring, initialAuthoring;
    std::vector<std::pair<int, int>> frameMap;
    State(MMDApp& a, SharedHistory& h) : app(a), history(h), binding(BoundNativePoseGeneration(&a)),
        poolIdentity(mdl::KeyPoolIdentity(a.CameraKeys())), capacity(mdl::KeyPoolCapacity(a.CameraKeys())),
        initialFrame(a.CurrentFrame()), initialPose(ReadPose(a)), initialCursor(a.CameraTrackCursor()), initialActive(a.CameraTrackActive()),
        authoring(a, curve_host::Kind::Camera), initialAuthoring(a, curve_host::Kind::Camera) {}
    bool Structure(bool initial) const noexcept {
        return BoundNativePoseHistory(&app) == &history && (!initial || BoundNativePoseGeneration(&app) == binding) &&
            mdl::KeyPoolIdentity(app.CameraKeys()) == poolIdentity && mdl::KeyPoolCapacity(app.CameraKeys()) >= capacity;
    }
    bool InitialSource() const noexcept {
        return Structure(true) && app.CurrentFrame() == initialFrame && ReadPose(app) == initialPose &&
            app.CameraTrackCursor() == initialCursor && app.CameraTrackActive() == initialActive &&
            initialAuthoring.CapturedCurrent() && Matches(app.CameraKeys(), mdl::KeyPoolCapacity(app.CameraKeys()), original, true, true);
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> source;
        const bool forward, first;
        const std::uint64_t serial, binding;
        const unsigned frame;
        Key* const pool;
        const std::size_t capacity;
        const Pose beforePose;
        Pose afterPose;
        const std::uint32_t cursor;
        const unsigned char active;
        const bool projection;
        bool consumed = false;
        std::vector<Snapshot> current, target;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves, sourceCurves;
        Candidate(std::shared_ptr<State> value, bool direction) : source(std::move(value)), forward(direction), first(!source->published),
            serial(source->serial), binding(BoundNativePoseGeneration(&source->app)), frame(unsigned(source->app.CurrentFrame())),
            pool(source->app.CameraKeys()), capacity(mdl::KeyPoolCapacity(pool)), beforePose(ReadPose(source->app)), afterPose(beforePose),
            cursor(source->app.CameraTrackCursor()), active(source->app.CameraTrackActive()), projection(source->app.cameraProjectionRefreshPending) {}
        bool IsCurrent() const noexcept override {
            return !consumed && source->serial == serial && forward != source->applied && source->Structure(first) &&
                BoundNativePoseGeneration(&source->app) == binding && Ready(source->app, true) &&
                source->app.CameraKeys() == pool && mdl::KeyPoolCapacity(pool) == capacity &&
                source->app.CurrentFrame() == int(frame) && ReadPose(source->app) == beforePose &&
                source->app.CameraTrackCursor() == cursor && source->app.CameraTrackActive() == active &&
                source->app.cameraProjectionRefreshPending == projection && Matches(pool, capacity, current, true, first) &&
                curves && curves->IsCurrent() && sourceCurves && sourceCurves->IsCurrent();
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (const auto& patch : source->patches) WriteValues(pool[patch.slot], forward ? patch.after : patch.before);
            curves->Publish(pool); WritePose(source->app, afterPose);
            source->published = true; source->applied = forward; ++source->serial; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!read || forward == applied || !Structure(!published) || !authoring.IsCurrent() || (!published && !InitialSource())) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (app.CurrentFrame() < 0 || unsigned(app.CurrentFrame()) > frame_limits::maximum) return MMS_E_ARGUMENT;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            if (!Finite(next->beforePose)) return MMS_E_ARGUMENT;
            next->current = Capture(next->pool, next->capacity, !published); next->target = next->current;
            for (const auto& patch : patches) {
                auto* key = const_cast<Key*>(At(next->target, patch.slot));
                if (!key || key->frame != patch.frame || !SameValues(ReadValues(*key), forward ? patch.before : patch.after)) return MMS_E_STALE;
                WriteValues(*key, forward ? patch.after : patch.before);
            }
            // Identity retiming preserves independent authored lanes and Free
            // handles. Intentional value changes release only stale exact
            // camera slices; inverse preparation restores the frozen seed.
            next->curves = authoring.PrepareRetimed(View(next->target, next->capacity), frameMap, forward, 0, true);
            next->sourceCurves = authoring.PrepareRetimed(View(next->current, next->capacity), frameMap, !forward, 0, true);
            if (next->beforePose == Sample(*next->sourceCurves, next->current, next->frame)) {
                const auto sample = Sample(*next->curves, next->target, next->frame);
                for (unsigned lane = 0; lane < 8; ++lane) next->afterPose[lane] = sample[lane];
            }
            if (!Finite(next->afterPose)) return MMS_E_ARGUMENT;
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(State) + 4 * sizeof(void*);
        Add(bytes, original.capacity(), sizeof(Snapshot)); Add(bytes, patches.capacity(), sizeof(Patch));
        Add(bytes, frameMap.capacity(), sizeof(frameMap[0]));
        for (const auto* layer : {&authoring, &initialAuthoring}) {
            const auto charge = layer->RetainedBytes(); Add(bytes, charge == UINT64_MAX ? charge : charge - sizeof(*layer));
        }
        return bytes;
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};

MmsResult NativeCameraMultiplyPlan::Begin(MMDApp& app) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        if (const auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app, false) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_BUSY;
        if (app.CurrentFrame() < 0 || unsigned(app.CurrentFrame()) > frame_limits::maximum || !Finite(ReadPose(app))) return MMS_E_ARGUMENT;
        auto state = std::make_shared<State>(app, *history);
        state->original = Capture(app.CameraKeys(), state->capacity, true);
        std::vector<int> frames; state->authoring.RetimingFrames(frames);
        for (const auto& key : state->original) frames.push_back(int(key.key.frame));
        std::sort(frames.begin(), frames.end()); frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        for (int frame : frames) state->frameMap.emplace_back(frame, frame);
        if (!state->InitialSource()) return MMS_E_STALE;
        state_ = std::move(state); return MMS_OK;
    });
}
MmsResult NativeCameraMultiplyPlan::Read(const CameraMultiplyOptions& options) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    state_->read = false;
    return Boundary([&]() -> MmsResult {
        // Validate the binding before touching a journal captured across modal
        // dispatch; teardown may have destroyed the old journal already.
        if (!state_->InitialSource()) return MMS_E_STALE;
        if (const auto result = state_->history.CheckNativeEdit()) return result;
        if (!Ready(state_->app, false)) return MMS_E_BUSY;
        for (unsigned lane = 0; lane < 8; ++lane)
            if (!std::isfinite(options.scale[lane]) || !std::isfinite(options.offset[lane])) return MMS_E_ARGUMENT;
        std::vector<State::Patch> patches;
        for (const auto& snapshot : state_->original) if (snapshot.key.selected) {
            const auto before = ReadValues(snapshot.key), after = Transform(before, options);
            if (!SameValues(before, after)) patches.push_back({snapshot.slot, snapshot.key.frame, before, after});
        }
        state_->patches.swap(patches); state_->read = true; return MMS_OK;
    });
}
MmsResult NativeCameraMultiplyPlan::ReadDialog(HWND dialog) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    state_->read = false;
    CameraMultiplyOptions options;
    for (unsigned lane = 0; lane < 8; ++lane) for (unsigned pair = 0; pair < 2; ++pair) {
        auto control = GetDlgItem(dialog, 686 + int(2 * lane + pair));
        if (!control) return MMS_E_ARGUMENT;
        // Same bounded text and atof conversion as the ordinary native dialog.
        char text[256]{}; GetWindowTextA(control, text, 20);
        (pair ? options.offset[lane] : options.scale[lane]) = float(std::atof(text));
    }
    return Read(options);
}
MmsResult NativeCameraMultiplyPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->read) return MMS_E_NOT_READY;
    if (!state_->InitialSource()) return MMS_E_STALE;
    if (!state_->patches.empty()) out = std::move(state_); else state_.reset();
    return MMS_OK;
}
}
#endif
