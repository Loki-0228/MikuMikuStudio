#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/plugins/background_identity.hpp"
#include <cstdio>

using namespace mikumikustudio::plugins::background_identity;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (false)
struct ThreadCheck { void* app; std::uint64_t instance, ticket; PlacementFields fields; PlacementResult result; };
static DWORD WINAPI WrongThread(void* value) {
    auto& check = *static_cast<ThreadCheck*>(value);
    Placement observed{};
    check.result = PlacementAccess(check.app, Kind::Picture, check.instance, check.ticket, check.fields, observed);
    return 0;
}

int main() {
    int first = 0, second = 0;
    CHECK(Acquire(nullptr, Kind::Picture) == 0);
    CHECK(!Acquire(&first, Kind::Picture));
    CHECK(!Register(nullptr));
    CHECK(Register(&first));
    CHECK(Register(&second));
    const auto instance = Instance(&first);
    CHECK(instance && Instance(&second) != instance);
    auto picture = Acquire(&first, Kind::Picture);
    auto video = Acquire(&first, Kind::Video);
    auto other = Acquire(&second, Kind::Picture);
    CHECK(picture && video && other && picture != video && picture != other);
    std::int32_t x = 1, y = 2; float scale = 1.f;
    PlacementFields fields{&x, &y, &scale};
    Placement observed{}, expected{1, 2, 1.f}, next{3, 4, 2.f};
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, observed) == PlacementResult::Ok);
    CHECK(observed.x == 1 && observed.y == 2 && observed.scale == 1.f);
    ThreadCheck check{&first, instance, picture, fields, PlacementResult::Invalid};
    HANDLE thread = CreateThread(nullptr, 0, WrongThread, &check, 0, nullptr);
    CHECK(thread != nullptr);
    CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CHECK(check.result == PlacementResult::WrongThread);
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, observed, &expected, &next, 7) == PlacementResult::Ok);
    CHECK(x == 3 && y == 4 && scale == 2.f);
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, observed, &expected, &next, 7) == PlacementResult::Conflict);
    CHECK(x == 3 && y == 4 && scale == 2.f);
    Placement aliasedExpected{3, 4, 2.f};
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, aliasedExpected, &aliasedExpected, &expected, 7) == PlacementResult::Ok);
    CHECK(x == 1 && y == 2 && scale == 1.f && aliasedExpected.x == 3);
    Placement aliasedReplacement{7, 8, 3.f};
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, aliasedReplacement, &expected, &aliasedReplacement, 7) == PlacementResult::Ok);
    CHECK(x == 7 && y == 8 && scale == 3.f && aliasedReplacement.x == 1);
    Placement conflictingExpected{0, 0, 0.f};
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, conflictingExpected, &conflictingExpected, &next, 7) == PlacementResult::Conflict);
    CHECK(x == 7 && y == 8 && scale == 3.f);
    Placement allAliased{7, 8, 3.f};
    CHECK(PlacementAccess(&first, Kind::Picture, instance, picture, fields, allAliased, &allAliased, &allAliased, 7) == PlacementResult::Ok);
    CHECK(x == 7 && y == 8 && scale == 3.f);
    CHECK(PlacementAccess(&first, Kind::Picture, instance + 1, picture, fields, observed) == PlacementResult::Stale);
    {
        Mutation preparing(&first, true, false, false, true);
        CHECK(!IsCurrent(&first, Kind::Picture, picture));
        CHECK(!Acquire(&first, Kind::Picture));
        {
            Mutation nestedPreparation(&first, true, false, false, true);
        }
        CHECK(!preparing.Current());
    }
    CHECK(IsCurrent(&first, Kind::Picture, picture));
    {
        int oldTexture = 1, newTexture = 2, *texture = &oldTexture, *old = nullptr;
        wchar_t path[256] = L"old", nextPath[] = L"new";
        std::int32_t w = 10, h = 11, px = 12, py = 13; float zoom = 1;
        std::uint8_t enabled = 1;
        Mutation preparing(&first, true, false, false, true);
        CHECK(preparing.PublishPicture(&first, texture, &newTexture, old, path, nextPath,
            w, 20, h, 21, px, py, zoom, 4, 2.f, enabled));
        CHECK(!preparing.PublishPicture(&first, texture, &newTexture, old, path, nextPath,
            w, 20, h, 21, px, py, zoom, 4, 2.f, enabled));
    }
    CHECK(!IsCurrent(&first, Kind::Picture, picture));
    picture = Acquire(&first, Kind::Picture);
    CHECK(picture != 0);
    {
        int oldTexture = 1, newTexture = 2, *texture = &oldTexture, *old = nullptr;
        wchar_t path[256] = L"old", nextPath[] = L"new";
        std::int32_t w = 10, h = 11, px = 12, py = 13; float zoom = 1;
        std::uint8_t enabled = 1;
        Mutation outer(&first, true, false);
        {
            Mutation reentered(&first, true, false);
        }
        CHECK(!outer.PublishPicture(&first, texture, &newTexture, old, path, nextPath,
            w, 20, h, 21, px, py, zoom, 4, 2.f, enabled));
        CHECK(texture == &oldTexture && old == nullptr && path[0] == L'o' && px == 12 && py == 13);
    }
    {
        int oldTexture = 1, newTexture = 2, *texture = &oldTexture, *old = nullptr;
        wchar_t path[256] = L"old", nextPath[] = L"new";
        std::int32_t w = 10, h = 11, px = 12, py = 13; float zoom = 1;
        std::uint8_t enabled = 1;
        Mutation publish(&first, true, false);
        CHECK(publish.PublishPicture(&first, texture, &newTexture, old, path, nextPath,
            w, 20, h, 21, px, py, zoom, 4, 2.f, enabled));
        CHECK(texture == &newTexture && old == &oldTexture && path[0] == L'n');
        CHECK(w == 20 && h == 21 && px == 0 && py == 4 && zoom == 2.f && enabled == 1);
    }
    picture = Acquire(&first, Kind::Picture);
    CHECK(picture != 0);
    CHECK(Acquire(&first, Kind::Picture) == picture);
    Invalidate(&first, Kind::Picture);
    CHECK(!IsCurrent(&first, Kind::Picture, picture));
    CHECK(IsCurrent(&first, Kind::Video, video));
    CHECK(IsCurrent(&second, Kind::Picture, other));
    const auto replacement = Acquire(&first, Kind::Picture);
    CHECK(replacement && replacement != picture);
    Forget(&first);
    CHECK(!IsCurrent(&first, Kind::Picture, replacement));
    CHECK(!IsCurrent(&first, Kind::Video, video));
    const auto reusedAddress = Acquire(&first, Kind::Picture);
    CHECK(reusedAddress && reusedAddress != replacement);
    {
        Mutation outer(&first, true, false);
        CHECK(PlacementAccess(&first, Kind::Picture, instance, reusedAddress, fields, observed) == PlacementResult::Stale);
        CHECK(!IsCurrent(&first, Kind::Picture, reusedAddress));
        CHECK(!Acquire(&first, Kind::Picture));
        CHECK(!Acquire(&second, Kind::Picture));
        CHECK(!IsCurrent(&second, Kind::Picture, other));
        CHECK(Acquire(&first, Kind::Video));
        {
            Mutation nested(&first, true, true);
            CHECK(!Acquire(&first, Kind::Picture));
            CHECK(!Acquire(&first, Kind::Video));
        }
        CHECK(!Acquire(&first, Kind::Picture));
        CHECK(Acquire(&first, Kind::Video));
    }
    CHECK(IsCurrent(&second, Kind::Picture, other));
    CHECK(Acquire(&first, Kind::Picture) != reusedAddress);
    {
        Mutation closing(&first, true, true, true);
        CHECK(!Acquire(&first, Kind::Picture));
        CHECK(!Register(&first));
    }
    CHECK(!Acquire(&first, Kind::Picture));
    CHECK(!Acquire(&first, Kind::Video));
    CHECK(!Instance(&first));
    CHECK(Register(&first));
    CHECK(Instance(&first) && Instance(&first) != instance);
    CHECK(!IsCurrent(&first, Kind::Picture, reusedAddress));
    const auto newInstance = Acquire(&first, Kind::Picture);
    CHECK(newInstance && newInstance != reusedAddress && newInstance != replacement);
    // A host can destroy an instance without running ShutdownCleanup. A new
    // instance at the same address must never see the old instance or ticket,
    // even when the caller ignores the failed registration result.
    const auto abandonedInstance = Instance(&second);
    CHECK(IsCurrent(&second, Kind::Picture, other));
    CHECK(!Register(&second));
    CHECK(!Instance(&second));
    CHECK(!Acquire(&second, Kind::Picture));
    CHECK(!IsCurrent(&second, Kind::Picture, other));
    CHECK(Register(&second));
    CHECK(Instance(&second) && Instance(&second) != abandonedInstance);
    const auto replacementTicket = Acquire(&second, Kind::Picture);
    CHECK(replacementTicket && replacementTicket != other);
    Forget(&first);
    Forget(&second);
}
