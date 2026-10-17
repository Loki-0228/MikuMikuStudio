#include "native_viewport_reset_camera.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mikudancestudio::plugins {
namespace {
constexpr double kPi = 3.14159265358979323846;
using Key = mdl::CameraKey;
struct Row { std::uint32_t slot; Key key; };
struct CameraSample {
    std::array<float, 3> position{}, rotation{};
    float distance = 0, fov = 0;
    std::uint8_t perspective = 0;
    int parentModel = -1, parentBone = 0;
};
bool Finite(const CameraSample& sample) noexcept {
    for (float value : sample.position) if (!std::isfinite(value)) return false;
    for (float value : sample.rotation) if (!std::isfinite(value)) return false;
    return std::isfinite(sample.distance) && std::isfinite(sample.fov);
}
bool Same(Key a, Key b) noexcept {
    // Timeline selection is independent of this camera sample.
    a.selected = b.selected = 0;
    return std::memcmp(&a, &b, sizeof(Key)) == 0;
}
void Validate(const Key& key) {
    for (float value : key.eye) if (!std::isfinite(value)) throw std::invalid_argument("camera position");
    for (float value : key.target) if (!std::isfinite(value)) throw std::invalid_argument("camera rotation");
    if (!std::isfinite(key.distance) || key.fov < 1 || key.fov > 179 || key.perspective > 1 ||
        key.parentModel < -1 || key.parentModel >= int(kModelSlotCount) || key.parentBone < 0)
        throw std::invalid_argument("camera payload");
    for (const auto& row : key.interpolation) for (auto value : row)
        if (value > 127) throw std::invalid_argument("camera controls");
}
// Keep the native ReloadModels evaluator's operation order, including its
// platform-specific float stores and 12-step inversion. Scalar curve channels
// pass Euler values through degrees; native tracks must avoid that round trip.
float NativeEase(const Key& key, unsigned lane, float t) noexcept {
    const auto& c = key.interpolation;
    if (c[0][lane] == c[1][lane] && c[2][lane] == c[3][lane]) return t;
#if defined(_M_IX86)
    const double x1 = static_cast<double>(static_cast<signed char>(c[0][lane])) / 127.0;
    const double x2 = static_cast<double>(static_cast<signed char>(c[2][lane])) / 127.0;
    float u = 0.5f, half = 0.25f;
    for (int i = 0; i < 12; ++i) {
        const float om = 1.0f - u;
        const double gx = om * 3.0 * u * u * x2 + om * (om * 3.0) * u * x1 + u * u * u;
        if (static_cast<float>(gx) == t) break;
        u = static_cast<float>(gx >= static_cast<double>(t) ? u - half : u + half);
        half *= 0.5f;
    }
    const float om = 1.0f - u;
    return static_cast<float>(3.0 * om * u * u * static_cast<double>(static_cast<signed char>(c[3][lane])) / 127.0 +
        om * (om * 3.0) * u * static_cast<double>(static_cast<signed char>(c[1][lane])) / 127.0 + u * u * u);
#else
    const float x1 = static_cast<float>(static_cast<signed char>(c[0][lane])) / 127.0f;
    const float x2 = static_cast<float>(static_cast<signed char>(c[2][lane])) / 127.0f;
    float u = 0.5f, half = 0.25f;
    for (int i = 0; i < 12; ++i) {
        const float v = 1.0f - u, v3 = v * 3.0f;
        const float gx = (v3 * v * u) * x1 + (v3 * u * u) * x2 + u * u * u;
        if (gx == t) break;
        u = gx >= t ? u - half : u + half;
        half *= 0.5f;
    }
    const float v = 1.0f - u, v3 = v * 3.0f;
    const float y1 = static_cast<float>(static_cast<signed char>(c[1][lane]));
    const float y2 = static_cast<float>(static_cast<signed char>(c[3][lane]));
    return (v3 * v * u) * y1 / 127.0f + (v3 * u * u) * y2 / 127.0f + u * u * u;
#endif
}
CameraSample Copy(const Key& key) noexcept {
    CameraSample result;
    std::copy(std::begin(key.eye), std::end(key.eye), result.position.begin());
    std::copy(std::begin(key.target), std::end(key.target), result.rotation.begin());
    result.distance = key.distance; result.fov = static_cast<float>(key.fov);
    result.perspective = key.perspective;
    result.parentModel = key.parentModel; result.parentBone = key.parentBone;
    return result;
}
CameraSample NativeSample(const Key& current, const Key& previous, std::uint32_t frame) noexcept {
    if (current.frame <= frame) return Copy(current);
    CameraSample result = Copy(previous);
    if (current.frame - 1 == previous.frame) return result;
    const float t = static_cast<float>(static_cast<double>(frame - previous.frame) /
        static_cast<double>(current.frame - previous.frame));
    for (unsigned channel = 0; channel < 6; ++channel) {
        const float eased = NativeEase(current, channel, t);
        const double a = channel < 3 ? previous.eye[channel] : channel == 3 ? previous.target[0] :
            channel == 4 ? previous.distance : static_cast<double>(previous.fov);
        const double b = channel < 3 ? current.eye[channel] : channel == 3 ? current.target[0] :
            channel == 4 ? current.distance : static_cast<double>(current.fov);
        const float value = static_cast<float>(eased * (b - a) + a);
        if (channel < 3) result.position[channel] = value;
        else if (channel == 3) result.rotation[0] = value;
        else if (channel == 4) result.distance = value;
        else result.fov = value;
    }
    const float rotationEase = NativeEase(current, 3, t);
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float a = previous.target[axis], b = current.target[axis];
        result.rotation[axis] = static_cast<float>(static_cast<double>(rotationEase) *
            (static_cast<double>(b) - static_cast<double>(a)) + static_cast<double>(a));
    }
    return result;
}
}
struct ViewportResetCameraSource::Impl {
    MMDApp& app;
    const std::uint64_t identity;
    const std::size_t capacity;
    const int frame;
    curve_host::TrackAuthoring authoring;
    std::vector<Row> rows;
    CameraSample sampled;
    explicit Impl(MMDApp& value) : app(value), identity(mdl::KeyPoolIdentity(value.CameraKeys())),
        capacity(mdl::KeyPoolCapacity(value.CameraKeys())), frame(value.CurrentFrame()),
        authoring(value, curve_host::Kind::Camera) {
        if (!identity || !capacity || frame < 0 || frame > frame_limits::maximum)
            throw std::invalid_argument("camera reset source");
        const auto* keys = app.CameraKeys();
        std::vector<unsigned char> visited(capacity, 0);
        std::uint32_t slot = 0, previous = 0, last = 0;
        for (std::size_t hops = 0;; ++hops) {
            if (slot >= capacity || visited[slot]) throw std::invalid_argument("camera chain cycle");
            const auto key = keys[slot];
            if (key.frame > frame_limits::maximum || (!hops && key.frame) ||
                (hops && (key.frame <= last || key.previous != previous)))
                throw std::invalid_argument("camera chain order");
            Validate(key); visited[slot] = 1; rows.push_back({slot, key});
            if (!key.next) break;
            previous = slot; last = key.frame; slot = key.next;
        }
        for (std::size_t i = 1; i < capacity; ++i)
            if (!visited[i] && keys[i].frame) throw std::invalid_argument("orphan camera key");
        // Freeze the native result before sorting the compact snapshot by slot.
        auto current = std::lower_bound(rows.begin(), rows.end(), std::uint32_t(frame),
            [](const Row& row, std::uint32_t at) { return row.key.frame < at; });
        if (current == rows.end()) current = rows.end() - 1;
        const Key& before = current->key.frame > std::uint32_t(frame) ? (current - 1)->key : current->key;
        sampled = NativeSample(current->key, before, std::uint32_t(frame));
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.slot < b.slot; });
        const curve_host::NativeKeyView view{this, capacity, [](const void* p, std::uint32_t index) noexcept -> const void* {
            return static_cast<const Impl*>(p)->At(index);
        }};
        auto target = authoring.Prepare(view);
        if (target->Authored()) {
            std::array<double, 12> values{};
            if (!target->Sample(frame, values)) throw std::invalid_argument("camera authored sample");
            for (unsigned axis = 0; axis < 3; ++axis) {
                sampled.position[axis] = static_cast<float>(values[axis]);
                sampled.rotation[axis] = static_cast<float>(values[axis + 3] * kPi / 180);
            }
            sampled.distance = static_cast<float>(values[6]); sampled.fov = static_cast<float>(values[7]);
            sampled.perspective = static_cast<std::uint8_t>(values[8]);
            sampled.parentModel = static_cast<int>(values[9]); sampled.parentBone = static_cast<int>(values[10]);
        }
        if (!Finite(sampled) || !target->IsCurrent()) throw std::invalid_argument("changed camera source");
    }
    const Key* At(std::uint32_t slot) const noexcept {
        const auto found = std::lower_bound(rows.begin(), rows.end(), slot,
            [](const Row& row, std::uint32_t index) { return row.slot < index; });
        return found != rows.end() && found->slot == slot ? &found->key : nullptr;
    }
    bool Current() const noexcept {
        const auto* keys = app.CameraKeys();
        const auto count = mdl::KeyPoolCapacity(keys);
        if (app.CurrentFrame() != frame || mdl::KeyPoolIdentity(keys) != identity || count < capacity ||
            !authoring.CapturedCurrent()) return false;
        for (const auto& row : rows) if (!Same(keys[row.slot], row.key)) return false;
        std::size_t nonzero = 0;
        for (std::size_t i = 1; i < count; ++i) if (keys[i].frame) ++nonzero;
        return nonzero + 1 == rows.size();
    }
};
ViewportResetCameraSource::ViewportResetCameraSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ViewportResetCameraSource::~ViewportResetCameraSource() = default;
bool ViewportResetCameraSource::IsCurrent() const noexcept { return impl_->Current(); }
bool ViewportResetCameraSource::Sample(ViewportPresetState& result) const noexcept {
    const auto& value = impl_->sampled;
    result.cameraPosition = value.position; result.rotation = value.rotation;
    result.distance = value.distance; result.fov = value.fov; result.perspective = value.perspective;
    result.parentModel = value.parentModel; result.parentBone = value.parentBone;
    return true;
}
std::uint64_t ViewportResetCameraSource::RetainedBytes() const noexcept {
    std::uint64_t bytes = sizeof(*this) + sizeof(Impl) + 4 * sizeof(void*);
    const auto add = [&](std::uint64_t n, std::uint64_t stride = 1) {
        bytes = stride && n > (UINT64_MAX - bytes) / stride ? UINT64_MAX : bytes + n * stride;
    };
    add(impl_->rows.capacity(), sizeof(Row)); add(impl_->authoring.RetainedBytes()); return bytes;
}
MmsResult CaptureViewportResetCamera(MMDApp& app, std::shared_ptr<ViewportResetCameraSource>& out) noexcept {
    out.reset();
    if (!mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto impl = std::make_unique<ViewportResetCameraSource::Impl>(app);
        auto source = std::shared_ptr<ViewportResetCameraSource>(new ViewportResetCameraSource(std::move(impl)));
        if (!source->IsCurrent()) return MMS_E_STALE;
        out = std::move(source); return MMS_OK;
    });
}
}
#endif
