#include "mikumikustudio/offscreen_gdi_owner.hpp"
#include <array>
#include <cstdio>
#include <type_traits>

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool value) { ++checks; if (!value) throw checks; }
using Owner = OffscreenGdiOwner<2, 2>;
struct Fixture {
    std::uint64_t thread = 11;
    bool deleteDc = true, deleteFont = true, deleteBitmap = true, selectBitmap = true, selectFont = true;
    bool reenter = false, reentryRejected = false;
    int storage = 0;
    std::array<int, 32> events{}; unsigned eventCount = 0;
    Owner* owner = nullptr;
    void Log(int x) noexcept { if (eventCount < events.size()) events[eventCount++] = x; }
    OffscreenOwnerThread Thread() noexcept {
        return {this, [](void* c) noexcept { return static_cast<Fixture*>(c)->thread; }};
    }
    OffscreenSurfaceOps Ops() noexcept {
        return {this,
            [](void* c, std::uintptr_t) noexcept -> std::uintptr_t { auto& f = *static_cast<Fixture*>(c); f.Log(1); return 10; },
            [](void* c, std::uintptr_t, int, int, void** p) noexcept -> std::uintptr_t { auto& f = *static_cast<Fixture*>(c); f.Log(2); *p = &f.storage; return 20; },
            [](void* c, std::uintptr_t, std::uintptr_t) noexcept { auto& f = *static_cast<Fixture*>(c); f.Log(3); return f.selectBitmap; },
            [](void* c, std::uintptr_t) noexcept { auto& f = *static_cast<Fixture*>(c); f.Log(7); return f.deleteBitmap; },
            {this,
                [](void* c, std::uintptr_t, std::uintptr_t, std::uintptr_t* previous) noexcept { auto& f = *static_cast<Fixture*>(c); f.Log(4); *previous = 40; return f.selectFont; },
                [](void* c, std::uintptr_t, std::uintptr_t) noexcept { auto& f = *static_cast<Fixture*>(c); f.Log(5); return true; },
                [](void* c, std::uintptr_t) noexcept { auto& f = *static_cast<Fixture*>(c); f.Log(6);
                    if (f.reenter && f.owner) f.reentryRejected = !f.owner->Retry() && !f.owner->Shutdown() && !f.owner->Reserve(f.Ops()).generation;
                    return f.deleteDc; },
                [](void* c, std::uintptr_t) noexcept { auto& f = *static_cast<Fixture*>(c); f.Log(8); return f.deleteFont; }} };
    }
};
struct Factory {
    Fixture* fixture = nullptr;
    unsigned calls = 0;
    std::uintptr_t result = 30;
    bool reentryRejected = false;
    Owner::DrawSession* nested = nullptr;
    bool nestedRejected = false;
    static std::uintptr_t Create(void* context) noexcept {
        auto& f = *static_cast<Factory*>(context); ++f.calls;
        auto& host = *f.fixture;
        f.reentryRejected = !host.owner->Retry() && !host.owner->Shutdown() &&
            !host.owner->Reserve(host.Ops()).generation;
        if (f.nested) {
            OffscreenSurfaceView view{};
            f.nestedRejected = !f.nested->Read(view);
        }
        return f.result;
    }
};
static_assert(!std::is_copy_constructible_v<Owner::DrawSession>);
static_assert(!std::is_move_constructible_v<Owner::DrawSession>);
}
int main() {
    try {
        Fixture f; Owner owner(f.Thread()); f.owner = &owner;
        auto first = owner.Reserve(f.Ops()); Check(first.generation == 1);
        Check(owner.Initialize(first, 1, 10, 10) == OffscreenSurfaceInit::Ready);
        Check(!owner.RestoreFont(first)); OffscreenSurfaceView initial{};
        Check(owner.Read(first, initial) && initial.ready && initial.font == 0);
        Check(owner.SelectOwnedFont(first, 30) == OffscreenFontSelection::Selected);
        f.thread = 12; OffscreenSurfaceView untouched{}; untouched.dc = 99;
        Check(!owner.Read(first, untouched) && untouched.dc == 99);
        Check(!owner.Release(first) && !owner.Retry() && !owner.Shutdown() && !owner.Reserve(f.Ops()).generation);
        f.thread = 11; f.deleteDc = false; f.reenter = true;
        Check(!owner.Release(first) && f.reentryRejected);
        std::size_t occupied = 0, pending = 0; bool closing = true;
        Check(owner.Counts(occupied, pending, closing) && occupied == 1 && pending == 1 && !closing);
        Check(!owner.Read(first, untouched) && untouched.dc == 99);
        Check(!owner.Retry()); f.deleteDc = true; f.deleteFont = false;
        Check(!owner.Retry() && owner.Counts(occupied, pending, closing) && pending == 1);
        f.deleteFont = true; f.deleteBitmap = false;
        Check(!owner.Retry() && owner.Counts(occupied, pending, closing) && pending == 1);
        f.deleteBitmap = true; Check(owner.Retry() && owner.Counts(occupied, pending, closing) && occupied == 0);
        auto second = owner.Reserve(f.Ops()); Check(second.index == first.index && second.generation == 2);
        Check(!owner.Release(first) && !owner.Read(first, untouched));
        f.selectBitmap = false;
        Check(owner.Initialize(second, 1, 10, 10) == OffscreenSurfaceInit::FailedOwned);
        Check(owner.Counts(occupied, pending, closing) && pending == 1 && owner.Retry());
        auto third = owner.Reserve(f.Ops()); Check(third.generation == 1 && third.index != first.index);
        Check(owner.Shutdown() && !owner.Reserve(f.Ops()).generation);
        Check(owner.Counts(occupied, pending, closing) && !occupied && closing && owner.Retry());

        Fixture factoryFixture; Owner factoryOwner(factoryFixture.Thread()); factoryFixture.owner = &factoryOwner;
        Factory factory{&factoryFixture}; auto slot = factoryOwner.Reserve(factoryFixture.Ops());
        Check(factoryOwner.CreateAndSelect(slot, Factory::Create, &factory) == OffscreenFontSelection::Rejected && factory.calls == 0);
        Check(factoryOwner.Initialize(slot, 1, 10, 10) == OffscreenSurfaceInit::Ready);
        factory.result = 0;
        Check(factoryOwner.CreateAndSelect(slot, Factory::Create, &factory) == OffscreenFontSelection::Rejected && factory.calls == 1 && factory.reentryRejected);
        OffscreenSurfaceView view{}; Check(factoryOwner.Read(slot, view) && view.ready && !view.font);
        factory.result = 30;
        Check(factoryOwner.CreateAndSelect(slot, Factory::Create, &factory) == OffscreenFontSelection::Selected && factory.calls == 2);
        Check(factoryOwner.CreateAndSelect(slot, Factory::Create, &factory) == OffscreenFontSelection::Rejected && factory.calls == 2);
        Check(factoryOwner.RestoreFont(slot));
        factoryFixture.selectFont = false; factoryFixture.deleteDc = false;
        Check(factoryOwner.CreateAndSelect(slot, Factory::Create, &factory) == OffscreenFontSelection::FailedOwned && factory.calls == 3);
        Check(factoryOwner.Counts(occupied, pending, closing) && occupied == 1 && pending == 1 && !closing);
        Check(!factoryOwner.Retry()); factoryFixture.deleteDc = true; factoryFixture.deleteFont = false;
        Check(!factoryOwner.Retry()); factoryFixture.deleteFont = true;
        Check(factoryOwner.Retry() && factoryOwner.Counts(occupied, pending, closing) && !occupied);
        Check(factoryOwner.CreateAndSelect(slot, Factory::Create, &factory) == OffscreenFontSelection::Rejected && factory.calls == 3);
        Check(factoryOwner.Shutdown());

        Fixture drawFixture; Owner drawOwner(drawFixture.Thread()); drawFixture.owner = &drawOwner;
        Factory drawFactory{&drawFixture}; auto drawSlot = drawOwner.Reserve(drawFixture.Ops());
        Check(drawOwner.Initialize(drawSlot, 1, 30, 12) == OffscreenSurfaceInit::Ready);
        int published = 7, staged = 0;
        const bool drawOk = drawOwner.WithDraw(drawSlot, [&](Owner::DrawSession& session) {
            OffscreenSurfaceView surface{};
            Check(session.Read(surface) && surface.ready && surface.width == 30 && surface.height == 12);
            const auto beforeFont = surface;
            OffscreenSurfaceView denied{}; denied.dc = 99;
            Check(!drawOwner.Read(drawSlot, denied) && denied.dc == 99);
            Check(!drawOwner.Release(drawSlot) && !drawOwner.Retry() && !drawOwner.Shutdown() && !drawOwner.Reserve(drawFixture.Ops()).generation);
            Check(!drawOwner.WithDraw(drawSlot, [](Owner::DrawSession&) { return true; }));
            Check(session.CreateAndSelect(Factory::Create, &drawFactory) == OffscreenFontSelection::Selected && drawFactory.reentryRejected);
            OffscreenSurfaceView selected{};
            Check(session.Read(selected) && selected.dc == beforeFont.dc && selected.bitmap == beforeFont.bitmap &&
                selected.pixels == beforeFont.pixels && selected.width == beforeFont.width && selected.height == beforeFont.height && selected.font == 30);
            Check(session.RestoreFont());
            OffscreenSurfaceView restored{};
            Check(session.Read(restored) && restored.dc == beforeFont.dc && restored.bitmap == beforeFont.bitmap &&
                restored.pixels == beforeFont.pixels && restored.width == beforeFont.width && restored.height == beforeFont.height && !restored.font);
            staged = 42; return true;
        });
        if (drawOk) published = staged;
        Check(drawOk && published == 42 && drawOwner.Read(drawSlot, view) && view.ready && !view.font);
        staged = 99;
        Check(!drawOwner.WithDraw(drawSlot, [&](Owner::DrawSession& session) -> bool {
            Check(session.CreateAndSelect(Factory::Create, &drawFactory) == OffscreenFontSelection::Selected);
            throw 123;
        }));
        Check(published == 42 && drawOwner.Counts(occupied, pending, closing) && occupied == 1 && pending == 1);
        drawFixture.deleteDc = false; Check(!drawOwner.Retry());
        drawFixture.deleteDc = true; Check(drawOwner.Retry() && !drawOwner.Read(drawSlot, view));
        auto stale = drawSlot; drawSlot = drawOwner.Reserve(drawFixture.Ops());
        Check(!drawOwner.WithDraw(stale, [](Owner::DrawSession&) { return true; }));
        Check(drawOwner.Initialize(drawSlot, 1, 30, 12) == OffscreenSurfaceInit::Ready);
        Check(!drawOwner.WithDraw(drawSlot, [&](Owner::DrawSession& session) {
            drawFactory.nested = &session;
            const auto selected = session.CreateAndSelect(Factory::Create, &drawFactory);
            drawFactory.nested = nullptr;
            return selected == OffscreenFontSelection::Selected;
        }) && drawFactory.nestedRejected);
        Check(drawOwner.Counts(occupied, pending, closing) && pending == 1 && drawOwner.Retry());
        drawSlot = drawOwner.Reserve(drawFixture.Ops());
        Check(drawOwner.Initialize(drawSlot, 1, 30, 12) == OffscreenSurfaceInit::Ready);
        drawFixture.thread = 12;
        Check(!drawOwner.WithDraw(drawSlot, [](Owner::DrawSession&) { return true; }));
        drawFixture.thread = 11;
        Check(!drawOwner.WithDraw(drawSlot, [](Owner::DrawSession& session) { session.Fail(); return true; }));
        Check(drawOwner.Counts(occupied, pending, closing) && pending == 1 && drawOwner.Shutdown());
        std::printf("offscreen_gdi_owner_tests: %u checks passed\n", checks); return 0;
    } catch (unsigned n) { std::fprintf(stderr, "owner check %u failed\n", n); return 1; }
}
