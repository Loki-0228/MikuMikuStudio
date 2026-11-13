#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/background_picture_asset.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local bool allocationsArmed = false, allocationFailed = false;
thread_local std::size_t allocationIndex = 0, failAt = (std::numeric_limits<std::size_t>::max)();
}
void* operator new(std::size_t size) {
    if (allocationsArmed && allocationIndex++ == failAt) { allocationFailed = true; throw std::bad_alloc(); }
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::media;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (false)
constexpr auto unlimited = (std::numeric_limits<std::uint64_t>::max)();
constexpr std::array<unsigned char, 7> payload{{2, 3, 5, 7, 11, 13, 17}};
struct Fixture {
    wchar_t path[MAX_PATH]{};
    Fixture() {
        wchar_t temp[MAX_PATH]{};
        const auto length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH || !GetTempFileNameW(temp, L"mba", 0, path)) throw std::runtime_error("asset fixture file");
        const auto file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { DeleteFileW(path); throw std::runtime_error("asset fixture writer"); }
        DWORD written = 0;
        const bool ok = WriteFile(file, payload.data(), DWORD(payload.size()), &written, nullptr) && written == payload.size();
        CloseHandle(file);
        if (!ok) { DeleteFileW(path); throw std::runtime_error("asset fixture bytes"); }
    }
    ~Fixture() { DeleteFileW(path); }
    BackgroundPictureSourcePtr Capture(PictureSourceBudget& budget) {
        BackgroundPictureSourcePtr source;
        const auto result = CaptureBackgroundPictureSource(path, unlimited, budget, source);
        CHECK(result == PictureSourceResult::Ok && source);
        if (!source) throw std::runtime_error("asset fixture capture");
        return source;
    }
};
void Ownership() {
    Fixture fixture; PictureSourceBudget budget;
    auto source = fixture.Capture(budget);
    const auto charge = source->RetainedBytes(), total = charge + BackgroundPictureAsset::OverheadBytes();
    wchar_t path[256] = L"captured-original.bmp";
    BackgroundPictureAssetPtr first, second;
    CHECK(CreateBackgroundPictureAsset(source, path, 640, 480, PictureDecodeMode::Primary1024, total, first) == PictureAssetResult::Ok);
    CHECK(CreateBackgroundPictureAsset(source, L"fallback.bmp", 640, 480, PictureDecodeMode::Fallback512, total, second) == PictureAssetResult::Ok);
    if (!first || !second) return;
    CHECK(first->Id() && second->Id() > first->Id() && first->Source() == source && second->Source() == source);
    CHECK(first->Width() == 640 && first->Height() == 480 && first->DecodeMode() == PictureDecodeMode::Primary1024 &&
        second->DecodeMode() == PictureDecodeMode::Fallback512 && first->RetainedBytes() == total);
    path[0] = L'X';
    CHECK(std::wcscmp(first->Path(), L"captured-original.bmp") == 0);
    CHECK(DeleteFileW(fixture.path));
    CHECK(std::memcmp(first->Source()->Data(), payload.data(), payload.size()) == 0);
    CHECK(budget.Usage().used == charge);
    const auto oldId = second->Id();
    source.reset(); first.reset();
    CHECK(budget.Usage().used == charge && second->Source()->Size() == payload.size());
    auto assetCopy = second; second.reset();
    CHECK(assetCopy->Id() == oldId && budget.Usage().used == charge);
    assetCopy.reset(); CHECK(budget.Usage().used == 0);
}
void InvalidAndBudget() {
    Fixture fixture; PictureSourceBudget budget;
    const auto source = fixture.Capture(budget);
    BackgroundPictureAssetPtr output;
    CHECK(CreateBackgroundPictureAsset(source, fixture.path, 1, 1, PictureDecodeMode::Primary1024, unlimited, output) == PictureAssetResult::Ok);
    const auto sentinel = output;
    const auto bytes = source->RetainedBytes() + BackgroundPictureAsset::OverheadBytes();
    const auto reject = [&](BackgroundPictureSourcePtr input, const wchar_t* path, std::int32_t width, std::int32_t height,
        PictureDecodeMode mode, std::uint64_t limit, PictureAssetResult expected) {
        CHECK(CreateBackgroundPictureAsset(std::move(input), path, width, height, mode, limit, output) == expected && output == sentinel);
        CHECK(budget.Usage().used == source->RetainedBytes());
    };
    reject({}, L"a", 1, 1, PictureDecodeMode::Primary1024, unlimited, PictureAssetResult::Invalid);
    reject(source, nullptr, 1, 1, PictureDecodeMode::Primary1024, unlimited, PictureAssetResult::Invalid);
    reject(source, L"", 1, 1, PictureDecodeMode::Primary1024, unlimited, PictureAssetResult::Invalid);
    std::array<wchar_t, 256> unterminated{}; unterminated.fill(L'x');
    reject(source, unterminated.data(), 1, 1, PictureDecodeMode::Primary1024, unlimited, PictureAssetResult::Invalid);
    const std::array<std::array<std::int32_t, 2>, 4> badDimensions{{{{0, 1}}, {{-1, 1}}, {{1, 0}}, {{1, -1}}}};
    for (const auto dimensions : badDimensions)
        reject(source, L"a", dimensions[0], dimensions[1], PictureDecodeMode::Primary1024, unlimited, PictureAssetResult::Invalid);
    for (const auto mode : {PictureDecodeMode(0), PictureDecodeMode(3), PictureDecodeMode(UINT32_MAX)})
        reject(source, L"a", 1, 1, mode, unlimited, PictureAssetResult::Invalid);
    for (const auto limit : {std::uint64_t(0), BackgroundPictureAsset::OverheadBytes(), bytes - 1})
        reject(source, L"a", 1, 1, PictureDecodeMode::Primary1024, limit, PictureAssetResult::TooLarge);
    unterminated.back() = L'\0';
    CHECK(CreateBackgroundPictureAsset(source, unterminated.data(), INT32_MAX, INT32_MAX,
        PictureDecodeMode::Fallback512, bytes, output) == PictureAssetResult::Ok && output->Path()[255] == L'\0' && std::wcslen(output->Path()) == 255);
    CHECK(output->Id() > sentinel->Id() && output->RetainedBytes() == bytes);
    // Path may alias the old output. Its metadata must be copied before swap.
    CHECK(CreateBackgroundPictureAsset(output->Source(), output->Path(), 3, 4, PictureDecodeMode::Primary1024,
        bytes, output) == PictureAssetResult::Ok && std::wcslen(output->Path()) == 255 && output->Width() == 3);
}
void AllocationFailures() {
    Fixture fixture; PictureSourceBudget budget;
    auto source = fixture.Capture(budget);
    BackgroundPictureAssetPtr output;
    CHECK(CreateBackgroundPictureAsset(source, L"sentinel", 2, 3, PictureDecodeMode::Primary1024, unlimited, output) == PictureAssetResult::Ok);
    const auto sentinel = output;
    allocationIndex = 0; failAt = (std::numeric_limits<std::size_t>::max)(); allocationsArmed = true;
    const auto initial = CreateBackgroundPictureAsset(source, L"allocation-probe", 2, 3, PictureDecodeMode::Fallback512, unlimited, output);
    allocationsArmed = false;
    const auto count = allocationIndex;
    CHECK(initial == PictureAssetResult::Ok && count >= 2 && count < 8);
    if (!output) return;
    const auto successfulId = output->Id();
    for (std::size_t index = 0; index < count; ++index) {
        output = sentinel; const auto references = source.use_count();
        allocationIndex = 0; allocationFailed = false; failAt = index; allocationsArmed = true;
        const auto result = CreateBackgroundPictureAsset(source, L"failed", 2, 3, PictureDecodeMode::Fallback512, unlimited, output);
        allocationsArmed = false;
        CHECK(allocationFailed && result == PictureAssetResult::NoMemory && output == sentinel);
        CHECK(source.use_count() == references && budget.Usage().used == source->RetainedBytes());
    }
    CHECK(CreateBackgroundPictureAsset(source, L"recovered", 2, 3, PictureDecodeMode::Primary1024, unlimited, output) == PictureAssetResult::Ok);
    CHECK(output->Id() > successfulId + count && budget.Usage().used == source->RetainedBytes());
}
void ConcurrentIdentities() {
    Fixture fixture; PictureSourceBudget budget;
    auto source = fixture.Capture(budget);
    constexpr std::size_t workers = 8, perWorker = 64;
    std::array<std::array<BackgroundPictureAssetPtr, perWorker>, workers> assets;
    std::atomic<unsigned> failed{0}; std::vector<std::thread> threads;
    struct JoinThreads {
        std::vector<std::thread>& threads;
        ~JoinThreads() { for (auto& thread : threads) if (thread.joinable()) thread.join(); }
    } joinThreads{threads};
    for (std::size_t worker = 0; worker < workers; ++worker) threads.emplace_back([&, worker] {
        for (auto& asset : assets[worker]) if (CreateBackgroundPictureAsset(source, L"parallel", 4, 5,
            PictureDecodeMode::Primary1024, unlimited, asset) != PictureAssetResult::Ok) ++failed;
    });
    for (auto& thread : threads) thread.join();
    CHECK(failed == 0 && budget.Usage().used == source->RetainedBytes());
    std::vector<std::uint64_t> ids;
    for (auto& group : assets) for (const auto& asset : group) { CHECK(asset && asset->Source() == source); if (asset) ids.push_back(asset->Id()); }
    std::sort(ids.begin(), ids.end());
    CHECK(ids.size() == workers * perWorker && !ids.empty() && ids.front() != 0 && std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    const auto last = ids.empty() ? 0 : ids.back();
    for (auto& group : assets) for (auto& asset : group) asset.reset();
    BackgroundPictureAssetPtr next;
    CHECK(CreateBackgroundPictureAsset(source, L"after-release", 1, 1, PictureDecodeMode::Primary1024, unlimited, next) == PictureAssetResult::Ok && next->Id() > last);
    source.reset(); next.reset(); CHECK(budget.Usage().used == 0);
}
}
int main() {
    try { Ownership(); InvalidAndBudget(); AllocationFailures(); ConcurrentIdentities(); }
    catch (const std::exception& error) { allocationsArmed = false; std::fprintf(stderr, "exception: %s\n", error.what()); CHECK(false); }
    std::printf("background_picture_asset_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
