#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/background_picture_source.hpp"
#include "../src/plugins/background_identity.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace allocation_probe {
thread_local bool reject = false;
thread_local unsigned attempts = 0;
}
void* operator new(std::size_t size) {
    if (allocation_probe::reject) { ++allocation_probe::attempts; throw std::bad_alloc(); }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace mikumikustudio::media;
namespace identity = mikumikustudio::plugins::background_identity;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (false)

struct File final {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(HANDLE handle) noexcept : value(handle) {}
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    File(const File&) = delete;
};

struct SourceFile final {
    std::wstring path;
    SourceFile() {
        wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, directory);
        if (!length || length >= MAX_PATH || !GetTempFileNameW(directory, L"mpr", 0, name))
            throw std::runtime_error("create isolated picture registry fixture");
        try { path = name; }
        catch (...) { DeleteFileW(name); throw; }
    }
    ~SourceFile() { DeleteFileW(path.c_str()); }
    BackgroundPictureSourcePtr Capture(unsigned char marker) {
        const unsigned char bytes[]{marker, 17, 29, 43};
        {
            File file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("open fixture writer");
            DWORD written = 0;
            if (!WriteFile(file.value, bytes, sizeof(bytes), &written, nullptr) || written != sizeof(bytes))
                throw std::runtime_error("write fixture source");
        }
        BackgroundPictureSourcePtr result;
        const auto status = CaptureBackgroundPictureSource(path.c_str(),
            BackgroundPictureSource::OverheadBytes() + sizeof(bytes), result);
        CHECK(status == PictureSourceResult::Ok && result);
        if (!result) throw std::runtime_error("capture fixture source");
        CHECK(result->Size() == sizeof(bytes) && std::memcmp(result->Data(), bytes, sizeof(bytes)) == 0);
        return result;
    }
};

// No application, D3D texture or window is constructed. The production
// registry uses this address only as an opaque owner identity; publication
// writes real local fields and intentionally uses int* as its template type.
struct Owner final {
    int appStorage = 0, firstTexture = 1, secondTexture = 2;
    int* texture = &firstTexture;
    wchar_t path[256] = L"initial";
    std::int32_t width = 10, height = 11, x = 12, y = 13;
    float scale = 1.f;
    std::uint8_t enabled = 0;
    Owner() {
        if (!identity::Register(App())) throw std::runtime_error("register fixture owner");
    }
    ~Owner() {
        identity::Forget(App());
        // Production deliberately retains closed-address tombstones. This
        // standalone test owns this address and removes only its own entries.
        auto& registry = identity::GetRegistry();
        identity::Guard guard(registry.lock);
        registry.registered.erase(App());
        registry.closing.erase(App());
    }
    const void* App() const noexcept { return &appStorage; }
    std::uint64_t Instance() const noexcept { return identity::Instance(App()); }
    std::uint64_t Ticket() const noexcept { return identity::Acquire(App(), identity::Kind::Picture); }
    BackgroundPictureSourcePtr Query(std::uint64_t instance, std::uint64_t ticket) const noexcept {
        return identity::CurrentPictureSource(App(), instance, ticket);
    }
    bool Publish(identity::Mutation& mutation, const BackgroundPictureSourcePtr& source,
        int* candidate, int*& previous) {
        BackgroundPictureAssetPtr asset;
        CHECK(CreateBackgroundPictureAsset(source, L"published", 20, 21, PictureDecodeMode::Primary1024,
            UINT64_MAX, asset) == PictureAssetResult::Ok);
        return mutation.PublishPicture(App(), texture, candidate, previous, path, L"published",
            width, 20, height, 21, x, y, scale, 4, 2.f, enabled, asset);
    }
    std::uint64_t Install(const BackgroundPictureSourcePtr& source) {
        {
            identity::Mutation mutation(App(), true, false, false, true);
            int* previous = nullptr;
            CHECK(Publish(mutation, source, &secondTexture, previous));
            CHECK(!Query(Instance(), 1));
        }
        const auto ticket = Ticket();
        CHECK(ticket != 0 && Query(Instance(), ticket) == source);
        return ticket;
    }
};

void QueryAndPublication() {
    SourceFile file;
    Owner owner, other;
    auto first = file.Capture(3);
    const auto instance = owner.Instance();
    const auto ticket = owner.Install(first);
    CHECK(owner.Query(instance, ticket) == first);
    CHECK(!identity::CurrentPictureSource(nullptr, instance, ticket));
    CHECK(!owner.Query(0, ticket));
    CHECK(!owner.Query(instance, 0));
    CHECK(!owner.Query(other.Instance(), ticket));
    CHECK(!identity::CurrentPictureSource(other.App(), instance, ticket));
    const auto videoTicket = identity::Acquire(owner.App(), identity::Kind::Video);
    CHECK(videoTicket != 0 && !owner.Query(instance, videoTicket));
    BackgroundPictureSourcePtr wrongThread = first;
    std::thread worker([&] { wrongThread = owner.Query(instance, ticket); });
    worker.join();
    CHECK(!wrongThread && owner.Query(instance, ticket) == first);

    auto replacement = file.Capture(7);
    {
        identity::Mutation mutation(owner.App(), true, false, false, true);
        CHECK(!owner.Query(instance, ticket));
        CHECK(owner.Ticket() == 0);
        int* previous = &owner.firstTexture;
        int* const originalTexture = owner.texture;
        CHECK(!owner.Publish(mutation, replacement, nullptr, previous));
        CHECK(previous == &owner.firstTexture && owner.texture == originalTexture);
        CHECK(!owner.Query(instance, ticket));
    }
    CHECK(owner.Query(instance, ticket) == first);
    {
        identity::Mutation outer(owner.App(), true, false, false, true);
        { identity::Mutation nested(owner.App(), true, false, false, true); }
        int* previous = nullptr;
        CHECK(!outer.Current() && !owner.Publish(outer, replacement, &owner.firstTexture, previous));
        CHECK(previous == nullptr && !owner.Query(instance, ticket));
    }
    CHECK(owner.Query(instance, ticket) == first);

    auto external = owner.Query(instance, ticket);
    std::weak_ptr<const BackgroundPictureSource> retired = first;
    first.reset();
    std::uint64_t nextTicket = 0;
    {
        identity::Mutation mutation(owner.App(), true, false, false, true);
        int* previous = nullptr;
        CHECK(owner.Publish(mutation, replacement, &owner.firstTexture, previous));
        CHECK(previous == &owner.secondTexture && owner.texture == &owner.firstTexture);
        CHECK(owner.width == 20 && owner.height == 21 && owner.x == 0 && owner.y == 4 && owner.scale == 2.f && owner.enabled == 1);
        CHECK(!owner.Query(instance, ticket) && owner.Ticket() == 0);
        CHECK(!owner.Publish(mutation, external, &owner.secondTexture, previous));
    }
    nextTicket = owner.Ticket();
    CHECK(nextTicket != 0 && nextTicket != ticket);
    CHECK(!owner.Query(instance, ticket) && owner.Query(instance, nextTicket) == replacement);
    CHECK(!retired.expired() && external->Size() == 4 && external->Data()[0] == 3);
    external.reset();
    CHECK(retired.expired());
}

void InvalidationAndReuse() {
    SourceFile file;
    Owner owner;
    for (unsigned action = 0; action != 4; ++action) {
        auto source = file.Capture(static_cast<unsigned char>(30 + action));
        const auto instance = owner.Instance();
        const auto ticket = owner.Install(source);
        std::weak_ptr<const BackgroundPictureSource> weak = source;
        source.reset();
        CHECK(!weak.expired());
        if (action == 0) {
            identity::Invalidate(owner.App(), identity::Kind::Video);
            CHECK(owner.Query(instance, ticket) && !weak.expired());
            identity::Invalidate(owner.App(), identity::Kind::Picture);
        } else if (action == 1) {
            identity::Forget(owner.App());
        } else if (action == 2) {
            CHECK(!identity::Register(owner.App()));
            CHECK(owner.Instance() == 0 && owner.Ticket() == 0);
            CHECK(weak.expired());
            CHECK(identity::Register(owner.App()));
            CHECK(owner.Instance() != 0 && owner.Instance() != instance);
        } else {
            {
                identity::Mutation closing(owner.App(), true, true, true);
                CHECK(!owner.Query(instance, ticket));
                CHECK(owner.Instance() == 0 && owner.Ticket() == 0);
                CHECK(!weak.expired()); // Retired source is owned by Mutation until destruction.
                CHECK(!identity::Register(owner.App()));
            }
            CHECK(weak.expired());
            CHECK(identity::Register(owner.App()));
            CHECK(owner.Instance() != 0 && owner.Instance() != instance);
        }
        CHECK(weak.expired());
        CHECK(!owner.Query(instance, ticket));
        const auto fresh = owner.Ticket();
        CHECK(fresh != 0 && fresh != ticket);
        CHECK(!owner.Query(owner.Instance(), fresh));
        CHECK(!owner.Query(instance, fresh));
    }
}

void MutationRetirement() {
    SourceFile file;
    Owner owner;
    auto source = file.Capture(61);
    const auto instance = owner.Instance();
    const auto ticket = owner.Install(source);
    std::weak_ptr<const BackgroundPictureSource> weak = source;
    source.reset();
    {
        identity::Mutation mutation(owner.App(), true, false);
        CHECK(!owner.Query(instance, ticket) && owner.Ticket() == 0);
        CHECK(!weak.expired());
        identity::Forget(owner.App());
        CHECK(!weak.expired());
    }
    CHECK(weak.expired());
    const auto fresh = owner.Ticket();
    CHECK(fresh != 0 && fresh != ticket && !owner.Query(instance, fresh));

    source = file.Capture(67);
    const auto secondTicket = owner.Install(source);
    auto external = owner.Query(instance, secondTicket);
    weak = source;
    source.reset();
    {
        identity::Mutation closing(owner.App(), true, true, true);
        CHECK(!owner.Query(instance, secondTicket));
        CHECK(!weak.expired());
    }
    CHECK(!weak.expired() && external->Data()[0] == 67);
    CHECK(identity::Register(owner.App()));
    const auto newInstance = owner.Instance(), newTicket = owner.Ticket();
    CHECK(newInstance != 0 && newInstance != instance && newTicket != 0 && newTicket != secondTicket);
    CHECK(!owner.Query(newInstance, newTicket));
    CHECK(!owner.Query(instance, secondTicket));
    CHECK(external->Size() == 4 && external->Data()[0] == 67);
    external.reset();
    CHECK(weak.expired());
}

void RetainedResetAndMetadata() {
    SourceFile file; Owner owner;
    auto source = file.Capture(79);
    const auto instance = owner.Instance(), oldTicket = owner.Install(source);
    auto asset = identity::CurrentPictureAsset(owner.App(), instance, oldTicket);
    CHECK(asset && asset->Source() == source && asset->Id() != 0);
    {
        identity::Mutation reset(owner.App(), true, true, false, true,
            identity::PictureRetention::RetainForSceneReset);
        CHECK(!owner.Query(instance, oldTicket) && !owner.Ticket());
        CHECK(!identity::CurrentPictureAsset(owner.App(), instance, oldTicket));
    }
    const auto fresh = owner.Ticket();
    CHECK(fresh && fresh != oldTicket && !identity::IsCurrent(owner.App(), identity::Kind::Picture, oldTicket));
    CHECK(identity::CurrentPictureAsset(owner.App(), instance, fresh) == asset && owner.Query(instance, fresh) == source);
    {
        identity::Mutation mutation(owner.App(), true, false, false, true);
        int* previous = &owner.firstTexture;
        const auto beforeTexture = owner.texture;
        const auto beforeX = owner.x;
        CHECK(!mutation.PublishPicture(owner.App(), owner.texture, &owner.firstTexture, previous,
            owner.path, L"mismatch", owner.width, 20, owner.height, 21, owner.x, owner.y,
            owner.scale, 12, 4.f, owner.enabled, asset));
        CHECK(previous == &owner.firstTexture && owner.texture == beforeTexture && owner.x == beforeX);
        CHECK(!mutation.PublishPicture(owner.App(), owner.texture, &owner.firstTexture, previous,
            owner.path, L"published", owner.width, 22, owner.height, 21, owner.x, owner.y,
            owner.scale, 12, 4.f, owner.enabled, asset));
    }
    CHECK(identity::CurrentPictureAsset(owner.App(), instance, fresh) == asset);
    {
        identity::Mutation reset(owner.App(), true, true, false, false,
            identity::PictureRetention::RetainForSceneReset);
        identity::Invalidate(owner.App(), identity::Kind::Picture);
    }
    const auto afterNested = owner.Ticket();
    CHECK(afterNested && afterNested != fresh && !owner.Query(instance, afterNested));
    CHECK(asset->Source()->Data()[0] == 79); // outer reset must not revive it

    owner.Install(source);
    std::weak_ptr<const BackgroundPictureAsset> closed = identity::CurrentPictureAsset(owner.App(), instance, owner.Ticket());
    {
        identity::Mutation closing(owner.App(), false, false, true, false,
            identity::PictureRetention::RetainForSceneReset);
        CHECK(!owner.Instance() && !owner.Ticket() && !closed.expired());
    }
    CHECK(closed.expired());
}

void ProjectScopeAuthorization() {
    Owner owner, other;
    {
        identity::Mutation project(owner.App(), true, true, false, true);
        CHECK(project.Current() && project.AllowsProjectPicture(owner.App()));
        CHECK(!project.AllowsProjectPicture(nullptr) && !project.AllowsProjectPicture(other.App()));
        CHECK(owner.Ticket() == 0); // authorization never makes public acquisition available
        bool wrongThreadAllowed = true;
        std::thread worker([&] { wrongThreadAllowed = project.AllowsProjectPicture(owner.App()); });
        worker.join(); CHECK(!wrongThreadAllowed);
        {
            identity::Mutation avi(owner.App(), false, true);
            CHECK(project.AllowsProjectPicture(owner.App()) && !project.Current());
            CHECK(!avi.AllowsProjectPicture(owner.App()));
        }
        CHECK(project.AllowsProjectPicture(owner.App()) && !project.Current());
        CHECK(owner.Ticket() == 0);
        // The project may create a new authorized scope, but cannot publish
        // using its now-stale outer serial after the AVI work.
        int* previous = &owner.secondTexture;
        CHECK(!project.ClearPicture(owner.App(), owner.texture, previous, owner.path,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == &owner.secondTexture && owner.texture == &owner.firstTexture);
        {
            identity::Mutation picture(owner.App(), true, false, false, true);
            CHECK(picture.Current() && picture.AllowsProjectPicture(owner.App()));
            CHECK(!project.AllowsProjectPicture(owner.App()));
            CHECK(picture.ClearPicture(owner.App(), owner.texture, previous, owner.path,
                owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
            CHECK(previous == &owner.firstTexture && !owner.texture);
            CHECK(!picture.AllowsProjectPicture(owner.App()));
        }
        CHECK(!project.AllowsProjectPicture(owner.App()));
    }
    {
        int unregistered = 0;
        identity::Mutation unknown(&unregistered, true, false);
        CHECK(!unknown.AllowsProjectPicture(&unregistered));
        CHECK(!identity::Register(&unregistered));
        CHECK(!unknown.AllowsProjectPicture(&unregistered));
    }
    {
        identity::Mutation project(owner.App(), true, false, false, true);
        CHECK(project.AllowsProjectPicture(owner.App()));
        {
            identity::Mutation reset(owner.App(), true, true, false, false,
                identity::PictureRetention::RetainForSceneReset);
            CHECK(!project.AllowsProjectPicture(owner.App()));
        }
        CHECK(!project.AllowsProjectPicture(owner.App()));
    }
    {
        identity::Mutation project(other.App(), true, false, false, true);
        CHECK(project.AllowsProjectPicture(other.App()));
        CHECK(!identity::Register(other.App()));
        CHECK(!project.AllowsProjectPicture(other.App()));
        CHECK(!identity::Register(other.App())); // live scope forbids address reuse
        CHECK(!project.AllowsProjectPicture(other.App()));
    }
    CHECK(identity::Register(other.App()));
    { identity::Mutation fresh(other.App(), true, false, false, true);
        CHECK(fresh.AllowsProjectPicture(other.App())); }
    {
        identity::Mutation closing(owner.App(), true, true, true);
        CHECK(!closing.AllowsProjectPicture(owner.App()));
    }
}

void ClearPublication() {
    SourceFile file; Owner owner, other;
    auto source = file.Capture(83);
    const auto instance = owner.Instance(), oldTicket = owner.Install(source);
    auto external = owner.Query(instance, oldTicket);
    std::weak_ptr<const BackgroundPictureSource> sourceWeak = source;
    std::weak_ptr<const BackgroundPictureAsset> assetWeak = identity::CurrentPictureAsset(owner.App(), instance, oldTicket);
    source.reset();
    const auto texture = owner.texture;
    const std::wstring path = owner.path;
    const auto width = owner.width, height = owner.height, x = owner.x, y = owner.y;
    const float scale = owner.scale;
    const auto enabled = owner.enabled;
    const auto unchanged = [&] {
        return owner.texture == texture && path == owner.path && owner.width == width && owner.height == height &&
            owner.x == x && owner.y == y && owner.scale == scale && owner.enabled == enabled;
    };
    {
        identity::Mutation clear(owner.App(), true, false, false, true);
        int* previous = &owner.firstTexture;
        CHECK(!clear.ClearPicture(other.App(), owner.texture, previous, owner.path,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == &owner.firstTexture && unchanged());
        CHECK(!clear.ClearPicture(owner.App(), owner.texture, previous, nullptr,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == &owner.firstTexture && unchanged());
        bool cleared = true;
        std::thread worker([&] {
            cleared = clear.ClearPicture(owner.App(), owner.texture, previous, owner.path,
                owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled);
        });
        worker.join(); CHECK(!cleared && previous == &owner.firstTexture && unchanged());
    }
    CHECK(identity::IsCurrent(owner.App(), identity::Kind::Picture, oldTicket));
    CHECK(owner.Query(instance, oldTicket) == external && !assetWeak.expired());
    {
        identity::Mutation stale(owner.App(), true, false, false, true);
        { identity::Mutation nested(owner.App(), false, true); }
        int* previous = &owner.firstTexture;
        CHECK(!stale.ClearPicture(owner.App(), owner.texture, previous, owner.path,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == &owner.firstTexture && unchanged());
    }
    CHECK(owner.Query(instance, oldTicket) == external);
    {
        identity::Mutation clear(owner.App(), true, false, false, true);
        int* previous = nullptr;
        CHECK(clear.ClearPicture(owner.App(), owner.texture, previous, owner.path,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == texture && !owner.texture && !owner.path[0] && owner.width == 0 && owner.height == 0 &&
            owner.x == 0 && owner.y == 0 && owner.scale == 1.f && owner.enabled == 0);
        CHECK(assetWeak.expired() && !sourceWeak.expired() && external->Data()[0] == 83);
        CHECK(!owner.Query(instance, oldTicket) && !owner.Ticket());
        previous = &owner.firstTexture;
        CHECK(!clear.ClearPicture(owner.App(), owner.texture, previous, owner.path,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == &owner.firstTexture && !owner.texture);
    }
    const auto ticket = owner.Ticket();
    CHECK(ticket && ticket != oldTicket && !owner.Query(instance, ticket));
    CHECK(!identity::CurrentPictureAsset(owner.App(), instance, ticket));
    CHECK(owner.firstTexture == 1 && owner.secondTexture == 2); // no texture operation was invoked
    external.reset(); CHECK(sourceWeak.expired());
    {
        identity::Mutation closing(owner.App(), true, true, true);
        int* previous = &owner.secondTexture;
        CHECK(!closing.ClearPicture(owner.App(), owner.texture, previous, owner.path,
            owner.width, owner.height, owner.x, owner.y, owner.scale, owner.enabled));
        CHECK(previous == &owner.secondTexture);
    }
}

void HiddenPicturePublication() {
    SourceFile file; Owner owner;
    auto source = file.Capture(89);
    const auto instance = owner.Instance(), ticket = owner.Install(source);
    auto asset = identity::CurrentPictureAsset(owner.App(), instance, ticket);
    {
        identity::Mutation picture(owner.App(), true, false, false, true);
        int* previous = &owner.firstTexture;
        CHECK(!picture.PublishPicture(owner.App(), owner.texture, &owner.firstTexture, previous, owner.path,
            asset->Path(), owner.width, asset->Width(), owner.height, asset->Height(), owner.x, owner.y,
            owner.scale, -17, .75f, owner.enabled, asset, -53, 2));
        CHECK(previous == &owner.firstTexture && owner.texture == &owner.secondTexture && owner.x == 0 && owner.y == 4 &&
            owner.scale == 2.f && owner.enabled == 1);
    }
    CHECK(owner.Query(instance, ticket) == source && identity::IsCurrent(owner.App(), identity::Kind::Picture, ticket));
    {
        identity::Mutation picture(owner.App(), true, false, false, true);
        int* previous = nullptr;
        CHECK(picture.PublishPicture(owner.App(), owner.texture, &owner.firstTexture, previous, owner.path,
            asset->Path(), owner.width, asset->Width(), owner.height, asset->Height(), owner.x, owner.y,
            owner.scale, -17, .75f, owner.enabled, asset, -53, 0));
        CHECK(previous == &owner.secondTexture && owner.texture == &owner.firstTexture && owner.x == -53 && owner.y == -17 &&
            owner.scale == .75f && owner.enabled == 0 && std::wcscmp(owner.path, asset->Path()) == 0);
        CHECK(!picture.AllowsProjectPicture(owner.App()));
    }
    const auto next = owner.Ticket();
    CHECK(next && next != ticket && owner.Query(instance, next) == source &&
        identity::CurrentPictureAsset(owner.App(), instance, next) == asset);
}

bool PublishPrepared(Owner& owner, identity::PicturePreparation& receipt,
    const BackgroundPictureAssetPtr& asset, int* candidate, int*& previous,
    std::int32_t x = -53, std::int32_t y = -17, float scale = .75f, std::uint8_t visible = 0) {
    return identity::PublishPreparedPicture(receipt, owner.texture, candidate, previous,
        owner.path, asset ? asset->Path() : L"", owner.width, asset ? asset->Width() : 0,
        owner.height, asset ? asset->Height() : 0, owner.x, owner.y, owner.scale,
        x, y, scale, owner.enabled, visible, asset);
}

bool SameReceipt(const identity::PicturePreparation& a, const identity::PicturePreparation& b) {
    return a.app == b.app && a.instance == b.instance && a.expectedTicket == b.expectedTicket &&
        a.reservedTicket == b.reservedTicket && a.pictureMutationSerial == b.pictureMutationSerial &&
        a.expectedAsset == b.expectedAsset;
}

void PreparedPublication() {
    SourceFile file; Owner owner;
    const auto source = file.Capture(97);
    const auto instance = owner.Instance(), ticket = owner.Install(source);
    const auto asset = identity::CurrentPictureAsset(owner.App(), instance, ticket);
    identity::PicturePreparation first, competing;
    // A failing allocator cannot prevent preparation or publication once the
    // fixed owner slot and immutable asset exist. No D3D methods are involved.
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool prepared = identity::PreparePictureActivation(owner.App(), instance, first);
    allocation_probe::reject = false;
    CHECK(prepared && allocation_probe::attempts == 0);
    CHECK(first.expectedTicket == ticket && first.expectedAsset == asset && first.reservedTicket > ticket);
    CHECK(owner.Ticket() == ticket && owner.texture == &owner.secondTexture);
    CHECK(identity::CurrentPictureAsset(owner.App(), instance, ticket) == asset);
    CHECK(identity::PreparePictureActivation(owner.App(), instance, competing));
    CHECK(competing.reservedTicket > first.reservedTicket && identity::PicturePreparationCurrent(first));
    CHECK(first.pictureMutationSerial == competing.pictureMutationSerial);
    const auto copied = first;
    const auto reserved = first.reservedTicket;
    int* previous = nullptr;
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool published = PublishPrepared(owner, first, asset, &owner.firstTexture, previous);
    allocation_probe::reject = false;
    CHECK(published && allocation_probe::attempts == 0);
    CHECK(!first.reservedTicket && owner.Ticket() == reserved && previous == &owner.secondTexture);
    CHECK(owner.texture == &owner.firstTexture && owner.x == -53 && owner.y == -17 && owner.scale == .75f && !owner.enabled);
    CHECK(!identity::IsCurrent(owner.App(), identity::Kind::Picture, ticket));
    CHECK(identity::CurrentPictureAsset(owner.App(), instance, reserved) == asset);
    CHECK(!identity::PicturePreparationCurrent(copied) && !identity::PicturePreparationCurrent(competing));
    previous = &owner.secondTexture;
    CHECK(!PublishPrepared(owner, first, asset, &owner.secondTexture, previous));
    CHECK(!PublishPrepared(owner, competing, asset, &owner.secondTexture, previous));
    CHECK(previous == &owner.secondTexture && owner.texture == &owner.firstTexture && owner.Ticket() == reserved);
    CHECK(competing.reservedTicket != 0); // failure does not consume the receipt

    identity::PicturePreparation empty;
    CHECK(identity::PreparePictureActivation(owner.App(), instance, empty));
    CHECK(empty.pictureMutationSerial == copied.pictureMutationSerial + 1);
    const auto emptyTicket = empty.reservedTicket;
    const std::uint32_t minusZeroBits = 0x80000000u;
    float minusZero; std::memcpy(&minusZero, &minusZeroBits, sizeof(minusZero));
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool cleared = PublishPrepared(owner, empty, {}, nullptr, previous, INT32_MIN, INT32_MAX, minusZero);
    allocation_probe::reject = false;
    CHECK(cleared && !allocation_probe::attempts && previous == &owner.firstTexture);
    CHECK(!owner.texture && !owner.path[0] && !owner.width && !owner.height && !owner.enabled);
    CHECK(owner.x == INT32_MIN && owner.y == INT32_MAX && std::memcmp(&owner.scale, &minusZero, sizeof(float)) == 0);
    CHECK(owner.Ticket() == emptyTicket && !identity::CurrentPictureAsset(owner.App(), instance, emptyTicket));
    identity::PicturePreparation restore;
    CHECK(identity::PreparePictureActivation(owner.App(), instance, restore));
    CHECK(!restore.expectedAsset && restore.expectedTicket == emptyTicket);
    CHECK(PublishPrepared(owner, restore, asset, &owner.secondTexture, previous, 91, -123, 3.25f, 1));
    CHECK(!previous && owner.texture == &owner.secondTexture && owner.x == 91 && owner.y == -123 &&
        owner.scale == 3.25f && owner.enabled == 1);
    CHECK(owner.Ticket() != reserved && owner.Ticket() != emptyTicket);
    CHECK(identity::CurrentPictureAsset(owner.App(), instance, owner.Ticket()) == asset);
    CHECK(owner.firstTexture == 1 && owner.secondTexture == 2);
}

void PreparedFailuresAndInvalidation() {
    SourceFile file; Owner owner;
    const auto source = file.Capture(101);
    const auto instance = owner.Instance(), ticket = owner.Install(source);
    const auto asset = identity::CurrentPictureAsset(owner.App(), instance, ticket);
    identity::PicturePreparation receipt;
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt));
    const auto saved = receipt;
    CHECK(!identity::PreparePictureActivation(nullptr, instance, receipt) && SameReceipt(receipt, saved));
    CHECK(!identity::PreparePictureActivation(owner.App(), 0, receipt) && SameReceipt(receipt, saved));
    CHECK(!identity::PreparePictureActivation(owner.App(), instance + 1, receipt) && SameReceipt(receipt, saved));
    int* previous = &owner.firstTexture;
    const auto unchanged = [&] {
        return SameReceipt(receipt, saved) && previous == &owner.firstTexture && owner.texture == &owner.secondTexture &&
            owner.width == 20 && owner.height == 21 && owner.x == 0 && owner.y == 4 && owner.scale == 2.f &&
            owner.enabled == 1 && std::wcscmp(owner.path, L"published") == 0 && owner.Ticket() == ticket;
    };
    CHECK(!PublishPrepared(owner, receipt, asset, nullptr, previous) && unchanged());
    CHECK(!PublishPrepared(owner, receipt, {}, &owner.firstTexture, previous) && unchanged());
    CHECK(!PublishPrepared(owner, receipt, {}, nullptr, previous, 0, 0, 1.f, 1) && unchanged());
    CHECK(!PublishPrepared(owner, receipt, asset, &owner.firstTexture, previous, 0, 0, 1.f, 2) && unchanged());
    wchar_t longPath[256]; for (auto& c : longPath) c = L'x';
    for (const wchar_t* invalidPath : {static_cast<const wchar_t*>(nullptr), L"mismatch", static_cast<const wchar_t*>(longPath)}) {
        CHECK(!identity::PublishPreparedPicture(receipt, owner.texture, &owner.firstTexture, previous,
            owner.path, invalidPath, owner.width, 20, owner.height, 21, owner.x, owner.y, owner.scale,
            0, 0, 1.f, owner.enabled, 1, asset) && unchanged());
    }
    CHECK(!identity::PublishPreparedPicture(receipt, owner.texture, &owner.firstTexture, previous,
        owner.path, asset->Path(), owner.width, 22, owner.height, 21, owner.x, owner.y, owner.scale,
        0, 0, 1.f, owner.enabled, 1, asset) && unchanged());
    bool wrongPrepare = true, wrongCurrent = true, wrongPublish = true;
    std::thread worker([&] {
        wrongPrepare = identity::PreparePictureActivation(owner.App(), instance, receipt);
        wrongCurrent = identity::PicturePreparationCurrent(receipt);
        wrongPublish = PublishPrepared(owner, receipt, asset, &owner.firstTexture, previous);
    });
    worker.join(); CHECK(!wrongPrepare && !wrongCurrent && !wrongPublish && unchanged());
    CHECK(identity::PicturePreparationCurrent(receipt));
    { identity::Mutation video(owner.App(), false, true); CHECK(identity::PicturePreparationCurrent(receipt)); }
    CHECK(identity::PicturePreparationCurrent(receipt));
    {
        identity::Mutation picture(owner.App(), true, false, false, true);
        CHECK(!identity::PicturePreparationCurrent(receipt));
        CHECK(!identity::PreparePictureActivation(owner.App(), instance, receipt) && SameReceipt(receipt, saved));
        CHECK(!PublishPrepared(owner, receipt, asset, &owner.firstTexture, previous));
        { identity::Mutation nested(owner.App(), true, false, false, true); }
    }
    CHECK(unchanged() && !identity::PicturePreparationCurrent(receipt));
    CHECK(!PublishPrepared(owner, receipt, asset, &owner.firstTexture, previous) && unchanged());
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt));
    identity::Invalidate(owner.App(), identity::Kind::Picture);
    CHECK(!identity::PicturePreparationCurrent(receipt));
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt) && !receipt.expectedTicket);
    CHECK(owner.Ticket() != ticket);
    CHECK(!identity::PicturePreparationCurrent(receipt));
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt));
    identity::Forget(owner.App());
    CHECK(!identity::PicturePreparationCurrent(receipt));
    CHECK(owner.Ticket() != 0 && identity::PreparePictureActivation(owner.App(), instance, receipt));
    const auto beforeClose = receipt;
    {
        identity::Mutation closing(owner.App(), true, true, true);
        CHECK(!identity::PicturePreparationCurrent(receipt));
        CHECK(!identity::PreparePictureActivation(owner.App(), instance, receipt) && SameReceipt(receipt, beforeClose));
        CHECK(!PublishPrepared(owner, receipt, asset, &owner.firstTexture, previous));
    }
    CHECK(identity::Register(owner.App()) && owner.Instance() != instance);
    CHECK(owner.Ticket() && !identity::PicturePreparationCurrent(receipt));
    CHECK(!PublishPrepared(owner, receipt, asset, &owner.firstTexture, previous));
    CHECK(owner.texture == &owner.secondTexture && previous == &owner.firstTexture);
}

void PreparedLifetimeAndFixedSlot() {
    SourceFile file; Owner owner;
    auto source = file.Capture(103);
    auto instance = owner.Instance();
    BackgroundPictureAssetPtr asset;
    CHECK(CreateBackgroundPictureAsset(source, L"lease", 9, 7, PictureDecodeMode::Fallback512,
        UINT64_MAX, asset) == PictureAssetResult::Ok);
    bool released = false, unlocked = false;
    // The alias retains the real immutable asset. Its test-only deleter probes
    // the lock without blocking, so an under-lock final release fails cleanly.
    BackgroundPictureAssetPtr tracked(asset.get(), [keep = asset, &released, &unlocked](const BackgroundPictureAsset*) {
        released = true;
        auto& r = identity::GetRegistry();
        unlocked = TryAcquireSRWLockExclusive(&r.lock) != FALSE;
        if (unlocked) ReleaseSRWLockExclusive(&r.lock);
    });
    {
        identity::Mutation replace(owner.App(), true, false, false, true);
        int* previous = nullptr;
        CHECK(replace.PublishPicture(owner.App(), owner.texture, &owner.secondTexture, previous,
            owner.path, tracked->Path(), owner.width, 9, owner.height, 7, owner.x, owner.y,
            owner.scale, 0, 1.f, owner.enabled, tracked));
    }
    tracked.reset(); asset.reset();
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const auto ticket = owner.Ticket();
    allocation_probe::reject = false;
    CHECK(ticket && !allocation_probe::attempts);
    {
        auto& r = identity::GetRegistry(); identity::Guard guard(r.lock);
        CHECK(r.entries.find(identity::Key{owner.App(), identity::Kind::Picture}) == r.entries.end());
        CHECK(r.registered.find(owner.App())->second.pictureTicket == ticket);
    }
    identity::PicturePreparation receipt;
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt));
    int* previous = nullptr;
    CHECK(PublishPrepared(owner, receipt, {}, nullptr, previous));
    CHECK(!released && receipt.expectedAsset); // receipt retains the old asset
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt));
    CHECK(released && unlocked && !receipt.expectedAsset);

    int unknownOwner = 0;
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool registered = identity::Register(&unknownOwner);
    allocation_probe::reject = false;
    CHECK(!registered && allocation_probe::attempts != 0 && !identity::Instance(&unknownOwner));
    CHECK(identity::PicturePreparationCurrent(receipt));
}

void PreparationFromUnactivatedSlot() {
    Owner owner; const auto instance = owner.Instance();
    const auto slot = [&] {
        auto& r = identity::GetRegistry(); identity::Guard guard(r.lock);
        return r.registered.find(owner.App())->second.pictureTicket;
    };
    CHECK(!slot());
    identity::PicturePreparation first;
    CHECK(identity::PreparePictureActivation(owner.App(), instance, first));
    CHECK(!first.expectedTicket && first.reservedTicket && !slot());
    const auto saved = first;
    CHECK(!identity::PreparePictureActivation(owner.App(), instance + 1, first));
    CHECK(SameReceipt(first, saved) && !slot());
    first = {}; // abandoning a preparation cannot activate the empty slot
    CHECK(!slot() && identity::PicturePreparationCurrent(saved));
    identity::Invalidate(owner.App(), identity::Kind::Picture);
    CHECK(!slot() && !identity::PicturePreparationCurrent(saved));
    CHECK(identity::PreparePictureActivation(owner.App(), instance, first));
    identity::Forget(owner.App());
    CHECK(!slot() && !identity::PicturePreparationCurrent(first));
    CHECK(identity::PreparePictureActivation(owner.App(), instance, first));
    const auto reserved = first.reservedTicket;
    int* previous = nullptr;
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool published = PublishPrepared(owner, first, {}, nullptr, previous, 37, -61, 2.f);
    allocation_probe::reject = false;
    CHECK(published && !allocation_probe::attempts && slot() == reserved && reserved != 0);
    CHECK(previous == &owner.firstTexture && !owner.texture && owner.x == 37 && owner.y == -61 && owner.scale == 2.f);
}

void ReadOnlyPictureSnapshots() {
    Owner owner; const auto instance = owner.Instance();
    const auto last = [] {
        auto& r = identity::GetRegistry(); identity::Guard guard(r.lock);
        return r.last;
    };
    const auto unchangedSnapshot = [](const identity::PictureSnapshot& a, const identity::PictureSnapshot& b) {
        return a.ticket == b.ticket && a.pictureMutationSerial == b.pictureMutationSerial && a.asset == b.asset;
    };
    const auto firstLast = last();
    identity::PictureSnapshot snapshot;
    bool mutating = true;
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool queried = identity::QueryPictureSnapshot(owner.App(), instance, snapshot, &mutating);
    allocation_probe::reject = false;
    CHECK(queried && !allocation_probe::attempts && !mutating);
    CHECK(!snapshot.ticket && !snapshot.asset && !identity::CurrentPictureTicket(owner.App(), instance));
    CHECK(last() == firstLast);
    const auto firstEpoch = snapshot.pictureMutationSerial;
    identity::Invalidate(owner.App(), identity::Kind::Picture);
    CHECK(identity::QueryPictureSnapshot(owner.App(), instance, snapshot));
    CHECK(!snapshot.ticket && !snapshot.asset && snapshot.pictureMutationSerial > firstEpoch && last() == firstLast);
    const auto secondEpoch = snapshot.pictureMutationSerial;
    identity::Forget(owner.App());
    CHECK(identity::QueryPictureSnapshot(owner.App(), instance, snapshot));
    CHECK(!snapshot.ticket && !snapshot.asset && snapshot.pictureMutationSerial > secondEpoch && last() == firstLast);

    SourceFile file; const auto source = file.Capture(107);
    const auto active = owner.Install(source);
    CHECK(identity::QueryPictureSnapshot(owner.App(), instance, snapshot) && snapshot.ticket == active && snapshot.asset);
    const auto saved = snapshot;
    const auto asset = snapshot.asset;
    const auto publishedLast = last();
    for (const auto wrongInstance : {std::uint64_t(0), instance + 1}) {
        mutating = true;
        CHECK(!identity::QueryPictureSnapshot(owner.App(), wrongInstance, snapshot, &mutating));
        CHECK(!mutating && unchangedSnapshot(snapshot, saved) && last() == publishedLast);
    }
    mutating = true;
    CHECK(!identity::QueryPictureSnapshot(nullptr, instance, snapshot, &mutating));
    CHECK(!mutating && unchangedSnapshot(snapshot, saved));
    bool wrongThreadSucceeded = true, wrongThreadMutating = true;
    std::thread worker([&] {
        wrongThreadSucceeded = identity::QueryPictureSnapshot(owner.App(), instance, snapshot, &wrongThreadMutating);
    });
    worker.join();
    CHECK(!wrongThreadSucceeded && !wrongThreadMutating && unchangedSnapshot(snapshot, saved) && last() == publishedLast);
    {
        identity::Mutation reset(owner.App(), true, true, false, false,
            identity::PictureRetention::RetainForSceneReset);
        mutating = false;
        CHECK(!identity::QueryPictureSnapshot(owner.App(), instance, snapshot, &mutating));
        CHECK(mutating && unchangedSnapshot(snapshot, saved) && last() == publishedLast);
        mutating = true;
        CHECK(!identity::QueryPictureSnapshot(owner.App(), instance + 1, snapshot, &mutating));
        CHECK(!mutating && unchangedSnapshot(snapshot, saved));
    }
    mutating = true;
    allocation_probe::attempts = 0; allocation_probe::reject = true;
    const bool retained = identity::QueryPictureSnapshot(owner.App(), instance, snapshot, &mutating);
    allocation_probe::reject = false;
    CHECK(retained && !allocation_probe::attempts && !mutating);
    CHECK(!snapshot.ticket && snapshot.asset == asset && snapshot.asset->Source() == source &&
        snapshot.pictureMutationSerial > saved.pictureMutationSerial && last() == publishedLast);
    CHECK(!identity::CurrentPictureTicket(owner.App(), instance));
    const auto beforeClose = snapshot;
    {
        identity::Mutation closing(owner.App(), true, true, true);
        mutating = true;
        CHECK(!identity::QueryPictureSnapshot(owner.App(), instance, snapshot, &mutating));
        CHECK(!mutating && unchangedSnapshot(snapshot, beforeClose));
    }
    mutating = true;
    CHECK(!identity::QueryPictureSnapshot(owner.App(), instance, snapshot, &mutating));
    CHECK(!mutating && unchangedSnapshot(snapshot, beforeClose) && last() == publishedLast);
}

// Last test: deliberately exhaust this standalone process's counter without
// rolling it backwards or reissuing any ticket. No production test hook exists.
void PreparationExhaustion() {
    Owner owner; const auto instance = owner.Instance(), ticket = owner.Ticket();
    identity::PicturePreparation receipt;
    CHECK(identity::PreparePictureActivation(owner.App(), instance, receipt));
    const auto saved = receipt;
    { auto& r = identity::GetRegistry(); identity::Guard guard(r.lock); r.last = UINT64_MAX; }
    CHECK(!identity::PreparePictureActivation(owner.App(), instance, receipt) && SameReceipt(receipt, saved));
    // An already reserved ticket is still usable after the allocator exhausts.
    int* previous = nullptr;
    CHECK(PublishPrepared(owner, receipt, {}, nullptr, previous));
    CHECK(previous == &owner.firstTexture && owner.Ticket() == saved.reservedTicket && owner.Ticket() != ticket);
    const auto consumed = receipt;
    CHECK(!identity::PreparePictureActivation(owner.App(), instance, receipt) && SameReceipt(receipt, consumed));
    // Epoch exhaustion also fails closed and does not consume an output.
    { auto& r = identity::GetRegistry(); identity::Guard guard(r.lock); r.pictureMutationSerial = UINT64_MAX; }
    CHECK(!identity::PicturePreparationCurrent(saved));
    CHECK(!identity::PreparePictureActivation(owner.App(), instance, receipt) && SameReceipt(receipt, consumed));
}
}

int main() {
    try { QueryAndPublication(); InvalidationAndReuse(); MutationRetirement(); RetainedResetAndMetadata();
        ProjectScopeAuthorization(); ClearPublication(); HiddenPicturePublication(); PreparedPublication();
        PreparedFailuresAndInvalidation(); PreparedLifetimeAndFixedSlot(); PreparationFromUnactivatedSlot();
        ReadOnlyPictureSnapshots(); PreparationExhaustion(); }
    catch (const std::exception& exception) { std::fprintf(stderr, "exception: %s\n", exception.what()); CHECK(false); }
    catch (...) { CHECK(false); }
    std::printf("background_picture_registry_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
