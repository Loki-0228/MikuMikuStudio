// CPU image ABI checks: ownership, lifetime, quotas, allocation failure and workers.
#include "render_image_store.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <vector>

static thread_local int failAfter = -1;
static thread_local unsigned allocationFailures = 0;
void* operator new(std::size_t size) {
    if (failAfter == 0) { ++allocationFailures; throw std::bad_alloc(); }
    if (failAfter > 0) --failAfter;
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::plugins;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return false; } } while (false)
static constexpr MmsHandle kSentinel = 0x123456789abcdef0ull;
static constexpr uint64_t MiB = 1024ull * 1024;

static MmsRenderImageApiV1 Bind(RenderImageStore& store, RenderImageStore::Client& client) {
    client.owner = &store;
    return store.Api(client);
}
static MmsRenderImageDescV1 Desc(uint32_t width = 1, uint32_t height = 1, uint64_t stride = 4) {
    return {Header<MmsRenderImageDescV1>(), width, height, MMS_IMAGE_RGBA8,
        MMS_IMAGE_COLOR_SRGB, MMS_IMAGE_ALPHA_STRAIGHT, 0, stride};
}
template<class T> static T Poison() {
    T value;
    std::memset(&value, 0xa5, sizeof(value));
    return value;
}
template<class T> static bool Unchanged(const T& value) {
    const auto expected = Poison<T>();
    return std::memcmp(&value, &expected, sizeof(value)) == 0;
}

static bool CopyAndLifetime() {
    RenderImageStore store;
    RenderImageStore::Client a, b;
    const auto api = Bind(store, a), other = Bind(store, b);
    CHECK(api.h.size == sizeof(api) && api.h.major == 1 && api.h.minor == 0);
    MmsRenderImageCapsV1 caps{};
    CHECK(api.getCapabilities(api.context, &caps) == MMS_OK);
    CHECK(caps.h.size == sizeof(caps) && caps.h.major == 1 && caps.h.minor == 0);
    CHECK(caps.flags == MMS_IMAGE_CAP_CPU_COPY && caps.reserved == 0);
    CHECK(caps.maxWidth == 16384 && caps.maxHeight == 16384);
    CHECK(caps.maxImageBytes == 64 * MiB && caps.maxClientBytes == 128 * MiB && caps.maxTotalBytes == 256 * MiB);
    CHECK(caps.maxImages == 4096 && caps.maxMappings == 1024);
    CHECK(api.getCapabilities(api.context, nullptr) == MMS_E_ARGUMENT);

    // The last row intentionally has no trailing padding; opaque/premultiplied
    // metadata must not silently transform the caller's channel bytes.
    for (uint32_t format : {MMS_IMAGE_RGBA8, MMS_IMAGE_BGRA8}) {
        for (uint32_t alpha : {MMS_IMAGE_ALPHA_OPAQUE, MMS_IMAGE_ALPHA_STRAIGHT, MMS_IMAGE_ALPHA_PREMULTIPLIED}) {
            std::array<unsigned char, 20> source{1,2,3,4,5,6,7,8,201,202,203,204,9,10,11,12,13,14,15,16};
            const std::array<unsigned char, 16> expected{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
            auto desc = Desc(2, 2, 12); desc.format = format; desc.alphaMode = alpha;
            MmsHandle image = 0, retained = 0;
            CHECK(api.createCpu(api.context, &desc, {source.data(), source.size()}, &image) == MMS_OK);
            CHECK(image != 0 && image != kSentinel);
            source.fill(0);
            MmsRenderImageInfoV1 info{};
            CHECK(api.describe(api.context, image, &info) == MMS_OK);
            CHECK(info.h.size == sizeof(info) && info.h.major == 1 && info.h.minor == 0);
            CHECK(info.width == 2 && info.height == 2 && info.format == format && info.colorSpace == MMS_IMAGE_COLOR_SRGB);
            CHECK(info.alphaMode == alpha && info.flags == 0 && info.rowStride == 8 && info.byteCount == 16 && info.deviceGeneration == 0);
            CHECK(api.retain(api.context, image, &retained) == MMS_OK && retained != image);
            MmsRenderImageMapV1 first{}, second{};
            CHECK(api.mapRead(api.context, image, &first) == MMS_OK);
            CHECK(api.mapRead(api.context, retained, &second) == MMS_OK);
            CHECK(first.h.size == sizeof(first) && first.h.major == 1 && first.h.minor == 0);
            CHECK(first.mapping != second.mapping && first.mapping != image && first.mapping != retained);
            CHECK(first.pixels == second.pixels && first.rowStride == 8 && first.byteCount == 16);
            CHECK(std::memcmp(first.pixels, expected.data(), expected.size()) == 0);
            auto foreignInfo = Poison<MmsRenderImageInfoV1>();
            auto foreignMap = Poison<MmsRenderImageMapV1>();
            MmsHandle foreignHandle = kSentinel;
            CHECK(other.describe(other.context, image, &foreignInfo) == MMS_E_NOT_FOUND && Unchanged(foreignInfo));
            CHECK(other.retain(other.context, image, &foreignHandle) == MMS_E_NOT_FOUND && foreignHandle == kSentinel);
            CHECK(other.mapRead(other.context, image, &foreignMap) == MMS_E_NOT_FOUND && Unchanged(foreignMap));
            CHECK(other.release(other.context, image) == MMS_E_NOT_FOUND);
            CHECK(other.unmap(other.context, first.mapping) == MMS_E_NOT_FOUND);
            CHECK(api.unmap(api.context, image) == MMS_E_NOT_FOUND);
            CHECK(api.release(api.context, first.mapping) == MMS_E_NOT_FOUND);
            CHECK(api.release(api.context, image) == MMS_OK);
            CHECK(api.release(api.context, image) == MMS_E_NOT_FOUND);
            CHECK(api.describe(api.context, retained, &info) == MMS_OK);
            CHECK(api.release(api.context, retained) == MMS_OK);
            CHECK(store.HasLeases(a) && !store.HasLeases(b));
            CHECK(std::memcmp(first.pixels, expected.data(), expected.size()) == 0);
            CHECK(api.unmap(api.context, first.mapping) == MMS_OK);
            CHECK(api.unmap(api.context, first.mapping) == MMS_E_NOT_FOUND);
            CHECK(store.HasLeases(a));
            CHECK(std::memcmp(second.pixels, expected.data(), expected.size()) == 0);
            CHECK(api.unmap(api.context, second.mapping) == MMS_OK && !store.HasLeases(a));
        }
    }
    return true;
}

static bool InvalidInputs() {
    RenderImageStore store; RenderImageStore::Client client;
    const auto api = Bind(store, client);
    const std::array<unsigned char, 16> source{};
    const auto valid = Desc(2, 2, 8);
    auto rejected = [&](const MmsRenderImageDescV1* desc, MmsBytes pixels, MmsResult expected) {
        MmsHandle image = kSentinel;
        return api.createCpu(api.context, desc, pixels, &image) == expected && image == kSentinel;
    };
    CHECK(rejected(nullptr, {source.data(), source.size()}, MMS_E_ARGUMENT));
    CHECK(api.createCpu(api.context, &valid, {source.data(), source.size()}, nullptr) == MMS_E_ARGUMENT);
    CHECK(rejected(&valid, {nullptr, source.size()}, MMS_E_ARGUMENT));
    CHECK(rejected(&valid, {source.data(), source.size() - 1}, MMS_E_ARGUMENT));
    CHECK(rejected(&valid, {source.data(), 0}, MMS_E_ARGUMENT));
    auto desc = valid; desc.h.size = sizeof(desc) - 1;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    desc = valid; desc.h.major = 2;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    desc = valid; desc.h.minor = 1;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    desc = valid; desc.width = 0;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    desc = valid; desc.height = 0;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    desc = valid; desc.flags = 1;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    desc = valid; desc.rowStride = 7;
    CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_ARGUMENT));
    for (auto field : {0u, 1u, 2u}) {
        desc = valid;
        if (field == 0) desc.format = 99;
        else if (field == 1) desc.colorSpace = 99;
        else desc.alphaMode = 99;
        CHECK(rejected(&desc, {source.data(), source.size()}, MMS_E_UNSUPPORTED));
    }
    desc = valid; desc.rowStride = UINT64_MAX;
    CHECK(rejected(&desc, {source.data(), UINT64_MAX}, MMS_E_ARGUMENT));
    desc = valid; desc.height = 3; desc.rowStride = UINT64_MAX / 2 + 1;
    CHECK(rejected(&desc, {source.data(), UINT64_MAX}, MMS_E_ARGUMENT));
    desc = Desc();
    CHECK(rejected(&desc, {reinterpret_cast<const void*>(UINTPTR_MAX - 2), 4}, MMS_E_ARGUMENT));
    desc = Desc(16385, 1, 16385 * 4);
    CHECK(rejected(&desc, {source.data(), UINT64_MAX}, MMS_E_MEMORY));
    desc = Desc(1, 16385, 4);
    CHECK(rejected(&desc, {source.data(), UINT64_MAX}, MMS_E_MEMORY));
    desc = Desc(8192, 2049, 8192 * 4);
    CHECK(rejected(&desc, {source.data(), UINT64_MAX}, MMS_E_MEMORY));

    MmsHandle image = 0;
    CHECK(api.createCpu(api.context, &valid, {source.data(), source.size()}, &image) == MMS_OK);
    CHECK(api.describe(api.context, image, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.retain(api.context, image, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.mapRead(api.context, image, nullptr) == MMS_E_ARGUMENT);
    auto info = Poison<MmsRenderImageInfoV1>();
    auto map = Poison<MmsRenderImageMapV1>();
    MmsHandle retained = kSentinel;
    CHECK(api.describe(api.context, 0, &info) == MMS_E_NOT_FOUND && Unchanged(info));
    CHECK(api.mapRead(api.context, kSentinel, &map) == MMS_E_NOT_FOUND && Unchanged(map));
    CHECK(api.retain(api.context, 0, &retained) == MMS_E_NOT_FOUND && retained == kSentinel);
    CHECK(api.unmap(api.context, 0) == MMS_E_NOT_FOUND);
    CHECK(api.release(api.context, 0) == MMS_E_NOT_FOUND);
    CHECK(api.release(api.context, image) == MMS_OK);
    CHECK(api.describe(api.context, image, &info) == MMS_E_NOT_FOUND && Unchanged(info));
    return true;
}

static bool GuardedPadding() {
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    struct Pages {
        unsigned char* data;
        ~Pages() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
    } allocation{static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
    CHECK(allocation.data != nullptr);
    DWORD previous = 0;
    CHECK(VirtualProtect(allocation.data + page, page, PAGE_NOACCESS, &previous));
    CHECK(VirtualProtect(allocation.data + page * 3, page, PAGE_NOACCESS, &previous));
    auto* source = allocation.data + page - 4;
    const std::array<unsigned char, 8> expected{31, 32, 33, 34, 41, 42, 43, 44};
    std::memcpy(source, expected.data(), 4);
    std::memcpy(source + page * 2, expected.data() + 4, 4);
    RenderImageStore store; RenderImageStore::Client client;
    const auto api = Bind(store, client);
    auto desc = Desc(1, 2, page * 2); MmsHandle image = 0;
    CHECK(api.createCpu(api.context, &desc, {source, page * 2 + 4}, &image) == MMS_OK);
    MmsRenderImageMapV1 map{};
    CHECK(api.mapRead(api.context, image, &map) == MMS_OK);
    CHECK(map.byteCount == 8 && std::memcmp(map.pixels, expected.data(), expected.size()) == 0);
    CHECK(api.release(api.context, image) == MMS_OK);
    CHECK(api.unmap(api.context, map.mapping) == MMS_OK);
    // A single row needs no stride-sized backing buffer, even at UINT64_MAX.
    desc = Desc(1, 1, UINT64_MAX);
    CHECK(api.createCpu(api.context, &desc, {source, 4}, &image) == MMS_OK);
    CHECK(api.release(api.context, image) == MMS_OK);
    return true;
}

static bool HandleAndMappingLimits() {
    RenderImageStore store; RenderImageStore::Client client;
    const auto api = Bind(store, client);
    const std::array<unsigned char, 4> pixels{2, 3, 4, 5}; const auto desc = Desc();
    std::array<MmsHandle, 4096> images{};
    std::array<MmsRenderImageMapV1, 1024> maps{};
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &images[0]) == MMS_OK);
    for (size_t i = 1; i < images.size(); ++i)
        CHECK(api.retain(api.context, images[0], &images[i]) == MMS_OK && images[i] != images[i - 1]);
    MmsHandle extra = kSentinel;
    CHECK(api.retain(api.context, images[0], &extra) == MMS_E_MEMORY && extra == kSentinel);
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &extra) == MMS_E_MEMORY && extra == kSentinel);
    CHECK(api.release(api.context, images.back()) == MMS_OK);
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &extra) == MMS_OK && extra != images.back());
    images.back() = extra;
    for (auto& map : maps) CHECK(api.mapRead(api.context, images[0], &map) == MMS_OK);
    auto excessMap = Poison<MmsRenderImageMapV1>();
    CHECK(api.mapRead(api.context, images[0], &excessMap) == MMS_E_MEMORY && Unchanged(excessMap));
    const auto oldMapping = maps.back().mapping;
    CHECK(api.unmap(api.context, oldMapping) == MMS_OK);
    CHECK(api.mapRead(api.context, images[0], &maps.back()) == MMS_OK && maps.back().mapping != oldMapping);
    for (auto handle : images) CHECK(api.release(api.context, handle) == MMS_OK);
    CHECK(store.HasLeases(client));
    for (const auto& map : maps) {
        CHECK(std::memcmp(map.pixels, pixels.data(), pixels.size()) == 0);
        CHECK(api.unmap(api.context, map.mapping) == MMS_OK);
    }
    CHECK(!store.HasLeases(client));
    return true;
}

static bool ByteBudgets() {
    RenderImageStore store; RenderImageStore::Client a, b, c;
    const auto aa = Bind(store, a), bb = Bind(store, b), cc = Bind(store, c);
    // 32 MiB input plus 256 MiB hosted payloads is the largest live allocation.
    std::vector<unsigned char> input(static_cast<size_t>(32 * MiB), 0x6d);
    const auto desc = Desc(4096, 2048, 4096 * 4);
    std::array<MmsHandle, 4> first{}, second{};
    for (auto& image : first) CHECK(aa.createCpu(aa.context, &desc, {input.data(), input.size()}, &image) == MMS_OK);
    MmsHandle extra = kSentinel; const auto small = Desc();
    CHECK(aa.createCpu(aa.context, &small, {input.data(), 4}, &extra) == MMS_E_MEMORY && extra == kSentinel);
    MmsHandle alias = 0;
    CHECK(aa.retain(aa.context, first[0], &alias) == MMS_OK);
    MmsRenderImageMapV1 pinned{};
    CHECK(aa.mapRead(aa.context, first[0], &pinned) == MMS_OK);
    for (auto& image : second) CHECK(bb.createCpu(bb.context, &desc, {input.data(), input.size()}, &image) == MMS_OK);
    CHECK(cc.createCpu(cc.context, &small, {input.data(), 4}, &extra) == MMS_E_MEMORY && extra == kSentinel);
    CHECK(aa.release(aa.context, first[0]) == MMS_OK);
    CHECK(cc.createCpu(cc.context, &small, {input.data(), 4}, &extra) == MMS_E_MEMORY && extra == kSentinel);
    CHECK(aa.release(aa.context, alias) == MMS_OK);
    CHECK(cc.createCpu(cc.context, &small, {input.data(), 4}, &extra) == MMS_E_MEMORY && extra == kSentinel);
    CHECK(static_cast<const unsigned char*>(pinned.pixels)[pinned.byteCount - 1] == 0x6d);
    CHECK(aa.unmap(aa.context, pinned.mapping) == MMS_OK);
    CHECK(cc.createCpu(cc.context, &desc, {input.data(), input.size()}, &extra) == MMS_OK);
    CHECK(cc.release(cc.context, extra) == MMS_OK);
    CHECK(aa.createCpu(aa.context, &desc, {input.data(), input.size()}, &first[0]) == MMS_OK);
    for (auto image : first) CHECK(aa.release(aa.context, image) == MMS_OK);
    for (auto image : second) CHECK(bb.release(bb.context, image) == MMS_OK);

    // The exact single-image maximum succeeds and all storage budget is reusable.
    input.resize(static_cast<size_t>(64 * MiB), 0x6d);
    const auto maximum = Desc(4096, 4096, 4096 * 4);
    CHECK(aa.createCpu(aa.context, &maximum, {input.data(), input.size()}, &extra) == MMS_OK);
    CHECK(aa.release(aa.context, extra) == MMS_OK);
    return true;
}

static bool RetirementAndWorker() {
    RenderImageStore store; RenderImageStore::Client client, other;
    const auto api = Bind(store, client), second = Bind(store, other);
    const std::array<unsigned char, 4> pixels{17, 18, 19, 20}; const auto desc = Desc();
    MmsHandle image = 0, alias = 0, unrelated = 0;
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &image) == MMS_OK);
    CHECK(api.retain(api.context, image, &alias) == MMS_OK);
    CHECK(second.createCpu(second.context, &desc, {pixels.data(), pixels.size()}, &unrelated) == MMS_OK);
    MmsRenderImageMapV1 map{};
    CHECK(api.mapRead(api.context, image, &map) == MMS_OK);
    allocationFailures = 0; failAfter = 0;
    store.Retire(client); store.Retire(client);
    failAfter = -1;
    CHECK(allocationFailures == 0 && store.HasLeases(client));
    auto caps = Poison<MmsRenderImageCapsV1>(); auto info = Poison<MmsRenderImageInfoV1>();
    auto rejectedMap = Poison<MmsRenderImageMapV1>(); MmsHandle out = kSentinel;
    CHECK(api.getCapabilities(api.context, &caps) == MMS_E_NOT_READY && Unchanged(caps));
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &out) == MMS_E_NOT_READY && out == kSentinel);
    CHECK(api.describe(api.context, image, &info) == MMS_E_NOT_READY && Unchanged(info));
    CHECK(api.retain(api.context, image, &out) == MMS_E_NOT_READY && out == kSentinel);
    CHECK(api.mapRead(api.context, image, &rejectedMap) == MMS_E_NOT_READY && Unchanged(rejectedMap));
    CHECK(api.release(api.context, image) == MMS_E_NOT_FOUND && api.release(api.context, alias) == MMS_E_NOT_FOUND);
    std::atomic<bool> worked{false};
    std::thread worker([&] {
        const bool intact = std::memcmp(map.pixels, pixels.data(), pixels.size()) == 0;
        worked = intact && api.unmap(api.context, map.mapping) == MMS_OK;
    });
    worker.join();
    CHECK(worked && !store.HasLeases(client));
    CHECK(second.describe(second.context, unrelated, &info) == MMS_OK);
    CHECK(second.release(second.context, unrelated) == MMS_OK);
    store.Retire(other);
    CHECK(!store.HasLeases(other));
    return true;
}

static bool AllocationAtomicity() {
    RenderImageStore store; RenderImageStore::Client client;
    const auto api = Bind(store, client);
    const std::array<unsigned char, 4> pixels{3, 4, 5, 6}; const auto desc = Desc();
    bool completed = false; unsigned failed = 0;
    for (int allocation = 0; allocation != 16; ++allocation) {
        MmsHandle image = kSentinel;
        failAfter = allocation; allocationFailures = 0;
        const auto result = api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &image);
        failAfter = -1;
        if (result == MMS_OK) {
            CHECK(api.release(api.context, image) == MMS_OK); completed = true; break;
        }
        CHECK(result == MMS_E_MEMORY && allocationFailures == 1 && image == kSentinel);
        CHECK(!store.HasLeases(client)); ++failed;
    }
    CHECK(completed && failed >= 2);
    MmsHandle image = 0;
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &image) == MMS_OK);
    MmsHandle alias = kSentinel; auto map = Poison<MmsRenderImageMapV1>();
    failAfter = 0; allocationFailures = 0;
    const auto retainResult = api.retain(api.context, image, &alias);
    failAfter = -1;
    CHECK(retainResult == MMS_E_MEMORY && allocationFailures == 1 && alias == kSentinel);
    failAfter = 0; allocationFailures = 0;
    const auto mapResult = api.mapRead(api.context, image, &map);
    failAfter = -1;
    CHECK(mapResult == MMS_E_MEMORY && allocationFailures == 1 && Unchanged(map) && !store.HasLeases(client));
    CHECK(api.mapRead(api.context, image, &map) == MMS_OK);
    CHECK(std::memcmp(map.pixels, pixels.data(), pixels.size()) == 0);
    // Failed image/map publication must not consume handle-count quotas.
    std::array<MmsHandle, 4095> aliases{};
    for (auto& handle : aliases) CHECK(api.retain(api.context, image, &handle) == MMS_OK);
    std::array<MmsRenderImageMapV1, 1023> mappings{};
    for (auto& mapping : mappings) CHECK(api.mapRead(api.context, image, &mapping) == MMS_OK);
    for (auto handle : aliases) CHECK(api.release(api.context, handle) == MMS_OK);
    for (const auto& mapping : mappings) CHECK(api.unmap(api.context, mapping.mapping) == MMS_OK);
    failAfter = 0; allocationFailures = 0;
    const auto releaseResult = api.release(api.context, image);
    const auto unmapResult = api.unmap(api.context, map.mapping);
    failAfter = -1;
    CHECK(releaseResult == MMS_OK && unmapResult == MMS_OK && allocationFailures == 0 && !store.HasLeases(client));
    return true;
}

static bool ParallelCalls() {
    RenderImageStore store; RenderImageStore::Client client;
    const auto api = Bind(store, client);
    const std::array<unsigned char, 4> pixels{22, 23, 24, 25}; const auto desc = Desc();
    MmsHandle image = 0;
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &image) == MMS_OK);
    std::atomic<unsigned> errors{0};
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i != 200; ++i) {
            MmsHandle alias = 0; MmsRenderImageMapV1 map{}; MmsRenderImageInfoV1 info{};
            if (api.retain(api.context, image, &alias) != MMS_OK) { ++errors; continue; }
            if (api.describe(api.context, alias, &info) != MMS_OK || info.byteCount != pixels.size()) ++errors;
            if (api.mapRead(api.context, alias, &map) != MMS_OK) ++errors;
            if (api.release(api.context, alias) != MMS_OK) ++errors;
            if (map.mapping) {
                if (std::memcmp(map.pixels, pixels.data(), pixels.size()) != 0) ++errors;
                if (api.unmap(api.context, map.mapping) != MMS_OK) ++errors;
            }
        }
    });
    for (auto& worker : workers) worker.join();
    CHECK(errors == 0 && !store.HasLeases(client));
    CHECK(api.release(api.context, image) == MMS_OK);
    return true;
}

int main() {
    if (!CopyAndLifetime() || !InvalidInputs() || !GuardedPadding() || !HandleAndMappingLimits() ||
        !ByteBudgets() || !RetirementAndWorker() || !AllocationAtomicity() || !ParallelCalls()) return 1;
    std::printf("PASS %u render image checks\n", checks);
    return 0;
}
