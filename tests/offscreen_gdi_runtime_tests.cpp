#include "mikumikustudio/offscreen_gdi_runtime.hpp"
#include <array>
#include <cstdio>

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool value) { if (!value) throw ++checks; ++checks; }
struct Fixture {
    std::uint64_t thread = 5;
    bool deleteDc = true, deleteBitmap = true;
    int pixel = 0;
    unsigned dcDeletes = 0, bitmapDeletes = 0;
    OffscreenOwnerThread Thread() noexcept {
        return {this, [](void* p) noexcept { return static_cast<Fixture*>(p)->thread; }};
    }
    OffscreenSurfaceOps Ops() noexcept {
        return {this,
            [](void*, std::uintptr_t) noexcept -> std::uintptr_t { return 10; },
            [](void* p, std::uintptr_t, int, int, void** out) noexcept -> std::uintptr_t {
                *out = &static_cast<Fixture*>(p)->pixel; return 20;
            },
            [](void*, std::uintptr_t, std::uintptr_t) noexcept { return true; },
            [](void* p, std::uintptr_t) noexcept {
                auto& f = *static_cast<Fixture*>(p); ++f.bitmapDeletes; return f.deleteBitmap;
            },
            {this,
             [](void*, std::uintptr_t, std::uintptr_t, std::uintptr_t* out) noexcept { *out = 30; return true; },
             [](void*, std::uintptr_t, std::uintptr_t) noexcept { return true; },
             [](void* p, std::uintptr_t) noexcept { auto& f = *static_cast<Fixture*>(p); ++f.dcDeletes; return f.deleteDc; },
             [](void*, std::uintptr_t) noexcept { return true; }}};
    }
};
struct ThreadFixture {
    std::uint64_t id = 77;
    bool acquire = false, alive = true;
    unsigned acquisitions = 0;
    OffscreenRuntimeThreadOps Ops() noexcept {
        return {this,
            [](void* p) noexcept { return static_cast<ThreadFixture*>(p)->id; },
            [](void* p) noexcept -> std::uintptr_t {
                auto& f = *static_cast<ThreadFixture*>(p); ++f.acquisitions; return f.acquire ? 99 : 0;
            },
            [](void* p, std::uintptr_t h) noexcept { return h == 99 && static_cast<ThreadFixture*>(p)->alive; }};
    }
};
}
int main() {
    try {
        ThreadFixture tf; OffscreenRuntimeThreadBinding binding(tf.Ops());
        OffscreenGdiRuntimeCore<1> guarded(binding.Thread());
        Check(!guarded.Start() && tf.acquisitions == 1);
        tf.acquire = true; Check(guarded.Start() && tf.acquisitions == 2);
        Fixture rejected; tf.id = 78; Check(!guarded.Reserve(rejected.Ops()).epoch);
        tf.id = 77; tf.alive = false;
        // The same numeric ID must not revive an owner after its thread exits.
        Check(!guarded.Start() && !guarded.ShutdownAtSafePoint() && tf.acquisitions == 2);
        Fixture f;
        OffscreenGdiRuntimeCore<1, 2> runtime(f.Thread());
        Check(runtime.Start());
        const auto old = runtime.Reserve(f.Ops());
        Check(old.epoch == 1 && runtime.Initialize(old, 1, 8, 8) == OffscreenSurfaceInit::Ready);
        bool callback = false;
        Check(runtime.WithDraw(old, [&](auto& session) {
            OffscreenSurfaceView view{};
            callback = session.Read(view) && view.ready && !runtime.Start() &&
                !runtime.ShutdownAtSafePoint() && !runtime.RetryAtSafePoint() && !runtime.Release(old);
            return callback;
        }) && callback);
        f.thread = 6;
        Check(!runtime.Start() && !runtime.ShutdownAtSafePoint() && !runtime.Release(old));
        f.thread = 5;
        f.deleteDc = false;
        Check(!runtime.ShutdownAtSafePoint());
        std::size_t occupied = 0, pending = 0; bool closing = false;
        Check(runtime.Counts(occupied, pending, closing) && occupied == 1 && pending == 1 && closing);
        Check(!runtime.Reserve(f.Ops()).epoch && !runtime.Start());
        Check(!runtime.RetryAtSafePoint() && f.dcDeletes == 2 && f.bitmapDeletes == 0);
        f.deleteDc = true; f.deleteBitmap = false;
        Check(!runtime.RetryAtSafePoint() && f.bitmapDeletes == 1);
        f.deleteBitmap = true;
        Check(runtime.RetryAtSafePoint() && runtime.Counts(occupied, pending, closing) && occupied == 0 && closing);
        Check(runtime.Start());
        const auto current = runtime.Reserve(f.Ops());
        Check(current.epoch == 2 && current.surface.index == old.surface.index &&
              current.surface.generation == old.surface.generation);
        Check(runtime.Initialize(old, 1, 8, 8) == OffscreenSurfaceInit::Rejected && !runtime.Release(old));
        Check(runtime.Initialize(current, 1, 8, 8) == OffscreenSurfaceInit::Ready && runtime.Release(current));
        Check(runtime.ShutdownAtSafePoint() && !runtime.Start());
        std::printf("offscreen_gdi_runtime_tests: %u checks passed\n", checks); return 0;
    } catch (unsigned n) { std::fprintf(stderr, "runtime check %u failed\n", n); return 1; }
}
