// Deterministic CPU canvas checks; no host API or visual inspection is needed.
#include <mms/cpp/image_canvas.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
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

using mms::sdk::CpuImageCanvas;
using mms::sdk::ImageCanvasOptions;
using mms::sdk::ImageScale;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return false; } } while (false)
template<class T> static MmsHeader Header() { return {sizeof(T), 1, 0}; }
static MmsRenderImageInfoV1 Info(uint32_t width, uint32_t height,
    uint32_t format = MMS_IMAGE_RGBA8, uint32_t alpha = MMS_IMAGE_ALPHA_OPAQUE) {
    const uint64_t stride = uint64_t(width) * 4;
    return {Header<MmsRenderImageInfoV1>(), width, height, format, MMS_IMAGE_COLOR_SRGB,
        alpha, 0, stride, stride * height, 0};
}
static MmsRenderImageMapV1 Mapping(const void* pixels, const MmsRenderImageInfoV1& info) {
    return {Header<MmsRenderImageMapV1>(), 1, pixels, info.byteCount, info.rowStride};
}
template<size_t N> static bool PixelsEqual(const CpuImageCanvas& canvas, const std::array<uint32_t, N>& expected) {
    return uint64_t(canvas.Width()) * canvas.Height() == N && canvas.Pixels() &&
        std::memcmp(canvas.Pixels(), expected.data(), N * sizeof(uint32_t)) == 0;
}

static bool BackgroundAndChannels() {
    CpuImageCanvas canvas;
    CHECK(canvas.Width() == 0 && canvas.Height() == 0);
    CHECK(canvas.Compose(nullptr, nullptr, 2, 3) == MMS_OK);
    CHECK(canvas.Width() == 2 && canvas.Height() == 3);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 6>{0xff202020, 0xff202020, 0xff202020, 0xff202020, 0xff202020, 0xff202020}));
    CHECK(canvas.Compose(nullptr, nullptr, 2, 1, {ImageScale::Contain, 0x123456}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 2>{0xff123456, 0xff123456}));
    const std::array<unsigned char, 16> rgba{255,0,0,0, 0,255,0,64, 0,0,255,128, 1,2,3,255};
    auto info = Info(2, 2); auto map = Mapping(rgba.data(), info);
    CHECK(canvas.Compose(&info, &map, 2, 2) == MMS_OK);
    const std::array<uint32_t, 4> expected{0xffff0000, 0xff00ff00, 0xff0000ff, 0xff010203};
    CHECK(PixelsEqual(canvas, expected));
    const std::array<unsigned char, 16> bgra{0,0,255,0, 0,255,0,64, 255,0,0,128, 3,2,1,255};
    info.format = MMS_IMAGE_BGRA8; map.pixels = bgra.data();
    CHECK(canvas.Compose(&info, &map, 2, 2) == MMS_OK && PixelsEqual(canvas, expected));
    return true;
}

static bool AlphaRules() {
    CpuImageCanvas canvas;
    const ImageCanvasOptions options{ImageScale::Stretch, 0x204060};
    // Expected values are fixed decimal examples, not an implementation clone.
    const std::array<unsigned char, 20> straight{200,100,50,128, 200,100,50,0,
        200,100,50,255, 255,0,127,1, 2,4,6,254};
    auto info = Info(5, 1, MMS_IMAGE_RGBA8, MMS_IMAGE_ALPHA_STRAIGHT);
    auto map = Mapping(straight.data(), info);
    CHECK(canvas.Compose(&info, &map, 5, 1, options) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 5>{0xff745249, 0xff204060, 0xffc86432, 0xff214060, 0xff020406}));
    const std::array<unsigned char, 20> premul{100,50,25,128, 0,0,0,0,
        200,100,50,255, 255,255,255,128, 1,2,3,0};
    info.alphaMode = MMS_IMAGE_ALPHA_PREMULTIPLIED; map.pixels = premul.data();
    CHECK(canvas.Compose(&info, &map, 5, 1, options) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 5>{0xff745249, 0xff204060, 0xffc86432, 0xffffffff, 0xff214263}));
    const std::array<unsigned char, 4> bgra{50,100,200,128};
    info = Info(1, 1, MMS_IMAGE_BGRA8, MMS_IMAGE_ALPHA_STRAIGHT); map = Mapping(bgra.data(), info);
    CHECK(canvas.Compose(&info, &map, 1, 1, options) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 1>{0xff745249}));
    info.alphaMode = MMS_IMAGE_ALPHA_OPAQUE;
    CHECK(canvas.Compose(&info, &map, 1, 1, options) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 1>{0xffc86432}));
    return true;
}

static bool ScaleRules() {
    CpuImageCanvas canvas;
    // Top row R/G/B, bottom row cyan/magenta/yellow.
    const std::array<unsigned char, 24> pixels{255,0,0,255, 0,255,0,255, 0,0,255,255,
        0,255,255,255, 255,0,255,255, 255,255,0,255};
    auto info = Info(3, 2); auto map = Mapping(pixels.data(), info);
    constexpr uint32_t R = 0xffff0000, G = 0xff00ff00, B = 0xff0000ff;
    constexpr uint32_t C = 0xff00ffff, M = 0xffff00ff, Y = 0xffffff00, K = 0xff102030;
    CHECK(canvas.Compose(&info, &map, 5, 3, {ImageScale::Stretch, 0x102030}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 15>{R,R,G,G,B, R,R,G,G,B, C,C,M,M,Y}));
    CHECK(canvas.Compose(&info, &map, 7, 7, {ImageScale::Contain, 0x102030}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 49>{
        K,K,K,K,K,K,K,
        R,R,R,G,G,B,B,
        R,R,R,G,G,B,B,
        C,C,C,M,M,Y,Y,
        C,C,C,M,M,Y,Y,
        K,K,K,K,K,K,K,
        K,K,K,K,K,K,K}));
    CHECK(canvas.Compose(&info, &map, 2, 2, {ImageScale::Contain, 0x102030}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 4>{R,G,K,K}));
    CHECK(canvas.Compose(&info, &map, 2, 2, {ImageScale::Stretch, 0x102030}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 4>{R,G,C,M}));

    // Transposed source tests horizontal centering and odd unused width.
    info = Info(2, 3); map = Mapping(pixels.data(), info);
    CHECK(canvas.Compose(&info, &map, 7, 7, {ImageScale::Contain, 0x102030}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 49>{
        K,R,R,G,G,K,K,
        K,R,R,G,G,K,K,
        K,R,R,G,G,K,K,
        K,B,B,C,C,K,K,
        K,B,B,C,C,K,K,
        K,M,M,Y,Y,K,K,
        K,M,M,Y,Y,K,K}));
    return true;
}

static bool ExtremeAspectAndAliasing() {
    CpuImageCanvas canvas;
    std::vector<unsigned char> pixels(16384 * 4, 0);
    for (size_t i = 0; i < 16384; ++i) { pixels[i * 4] = 0x60; pixels[i * 4 + 3] = 255; }
    // Only the three expected nearest-neighbor samples carry these colors.
    pixels[0] = 0x11; pixels[5461 * 4] = 0x22; pixels[10922 * 4] = 0x33;
    auto info = Info(1, 16384); auto map = Mapping(pixels.data(), info);
    CHECK(canvas.Compose(&info, &map, 7, 3, {ImageScale::Contain, 0}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 21>{
        0xff000000,0xff000000,0xff000000,0xff110000,0xff000000,0xff000000,0xff000000,
        0xff000000,0xff000000,0xff000000,0xff220000,0xff000000,0xff000000,0xff000000,
        0xff000000,0xff000000,0xff000000,0xff330000,0xff000000,0xff000000,0xff000000}));
    info = Info(16384, 1); map = Mapping(pixels.data(), info);
    CHECK(canvas.Compose(&info, &map, 3, 7, {ImageScale::Contain, 0}) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 21>{
        0xff000000,0xff000000,0xff000000,
        0xff000000,0xff000000,0xff000000,
        0xff000000,0xff000000,0xff000000,
        0xff110000,0xff220000,0xff330000,
        0xff000000,0xff000000,0xff000000,
        0xff000000,0xff000000,0xff000000,
        0xff000000,0xff000000,0xff000000}));
    const std::array<unsigned char, 8> original{255,0,0,255, 0,255,0,255};
    info = Info(2, 1); map = Mapping(original.data(), info);
    CHECK(canvas.Compose(&info, &map, 2, 1) == MMS_OK);
    info.format = MMS_IMAGE_BGRA8; map.pixels = canvas.Pixels();
    CHECK(canvas.Compose(&info, &map, 4, 2) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 8>{0xffff0000,0xffff0000,0xff00ff00,0xff00ff00,
        0xffff0000,0xffff0000,0xff00ff00,0xff00ff00}));
    return true;
}

static bool InvalidInputsAndAtomicity() {
    CpuImageCanvas canvas;
    CHECK(canvas.Compose(nullptr, nullptr, 2, 2, {ImageScale::Contain, 0x123456}) == MMS_OK);
    const std::array<uint32_t, 4> expected{0xff123456,0xff123456,0xff123456,0xff123456};
    const std::array<unsigned char, 8> source{1,2,3,4, 5,6,7,8};
    const auto original = Info(1, 1); const auto originalMap = Mapping(source.data(), original);
    auto reject = [&](const MmsRenderImageInfoV1* info, const MmsRenderImageMapV1* map,
        uint32_t width, uint32_t height, MmsResult result, ImageCanvasOptions options = {}) {
        return canvas.Compose(info, map, width, height, options) == result &&
            canvas.Width() == 2 && canvas.Height() == 2 && PixelsEqual(canvas, expected);
    };
    CHECK(reject(&original, nullptr, 1, 1, MMS_E_ARGUMENT));
    CHECK(reject(nullptr, &originalMap, 1, 1, MMS_E_ARGUMENT));
    auto info = original; auto map = originalMap;
    info.h.size = sizeof(info) - 1; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.h.major = 2; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.h.minor = 1; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; map.h.size = sizeof(map) - 1; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.h.major = 2; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.h.minor = 1; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.mapping = 0; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.pixels = nullptr; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.rowStride = 8; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.byteCount = 3; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; map.pixels = reinterpret_cast<const void*>(UINTPTR_MAX - 1);
    CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    map = originalMap; info.width = 0; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.height = 0; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.flags = 1; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.deviceGeneration = 9; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.rowStride = 8; map.rowStride = 8; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; map = originalMap; info.byteCount = 3; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; info.byteCount = 8; map.byteCount = 8; CHECK(reject(&info, &map, 1, 1, MMS_E_ARGUMENT));
    info = original; map = originalMap; info.format = 99; CHECK(reject(&info, &map, 1, 1, MMS_E_UNSUPPORTED));
    info = original; info.colorSpace = 99; CHECK(reject(&info, &map, 1, 1, MMS_E_UNSUPPORTED));
    info = original; info.alphaMode = 99; CHECK(reject(&info, &map, 1, 1, MMS_E_UNSUPPORTED));
    info = Info(16385, 1); map = Mapping(source.data(), info);
    CHECK(reject(&info, &map, 1, 1, MMS_E_MEMORY));
    info = Info(1, 16385); map = Mapping(source.data(), info);
    CHECK(reject(&info, &map, 1, 1, MMS_E_MEMORY));
    info = Info(8192, 2049); map = Mapping(source.data(), info);
    CHECK(reject(&info, &map, 1, 1, MMS_E_MEMORY));
    CHECK(reject(nullptr, nullptr, 1, 1, MMS_E_ARGUMENT, {static_cast<ImageScale>(99), 0}));
    CHECK(reject(nullptr, nullptr, 1, 1, MMS_E_ARGUMENT, {ImageScale::Contain, 0x80123456}));
    CHECK(reject(nullptr, nullptr, 16385, 1, MMS_E_MEMORY));
    CHECK(reject(nullptr, nullptr, 1, 16385, MMS_E_MEMORY));
    CHECK(reject(nullptr, nullptr, 8192, 2049, MMS_E_MEMORY));
    CHECK(reject(nullptr, nullptr, UINT32_MAX, UINT32_MAX, MMS_E_MEMORY));
    // Validation must not be bypassed by an empty target.
    CHECK(reject(nullptr, &originalMap, 0, 0, MMS_E_ARGUMENT));
    info = original; info.format = 99;
    CHECK(reject(&info, &originalMap, 0, 0, MMS_E_UNSUPPORTED));
    map = originalMap; map.mapping = 0;
    CHECK(reject(&original, &map, 0, 9, MMS_E_ARGUMENT));

    // A larger read span is allowed; only the described pixel is consumed.
    info = original; map = originalMap; map.byteCount = source.size();
    CHECK(canvas.Compose(&info, &map, 1, 1) == MMS_OK);
    CHECK(PixelsEqual(canvas, std::array<uint32_t, 1>{0xff010203}));
    return true;
}

static bool EmptyAndLimits() {
    CpuImageCanvas canvas;
    const std::array<unsigned char, 4> source{5, 6, 7, 255};
    auto info = Info(1, 1); auto map = Mapping(source.data(), info);
    CHECK(canvas.Compose(nullptr, nullptr, 2, 2) == MMS_OK);
    CHECK(canvas.Compose(&info, &map, 0, 7) == MMS_OK);
    CHECK(canvas.Width() == 0 && canvas.Height() == 7 && canvas.Pixels() == nullptr);
    CHECK(canvas.Compose(&info, &map, 9, 0) == MMS_OK);
    CHECK(canvas.Width() == 9 && canvas.Height() == 0 && canvas.Pixels() == nullptr);
    CHECK(canvas.Compose(nullptr, nullptr, 0, 0) == MMS_OK);
    CHECK(canvas.Width() == 0 && canvas.Height() == 0 && canvas.Pixels() == nullptr);
    CHECK(canvas.Compose(nullptr, nullptr, 4096, 4096, {ImageScale::Contain, 0x334455}) == MMS_OK);
    CHECK(canvas.Width() == 4096 && canvas.Height() == 4096 && canvas.Pixels());
    CHECK(canvas.Pixels()[0] == 0xff334455 && canvas.Pixels()[4096 * 4096 - 1] == 0xff334455);
    CHECK(canvas.Compose(&info, &map, 16384, 1) == MMS_OK);
    CHECK(canvas.Width() == 16384 && canvas.Height() == 1);
    return true;
}

static bool AllocationFailure() {
    CpuImageCanvas canvas;
    CHECK(canvas.Compose(nullptr, nullptr, 2, 2, {ImageScale::Contain, 0x576879}) == MMS_OK);
    const std::array<uint32_t, 4> expected{0xff576879,0xff576879,0xff576879,0xff576879};
    bool succeeded = false; unsigned failures = 0;
    for (int allocation = 0; allocation != 16; ++allocation) {
        failAfter = allocation; allocationFailures = 0;
        const auto result = canvas.Compose(nullptr, nullptr, 64, 64, {ImageScale::Contain, 0xabcdef});
        failAfter = -1;
        if (result == MMS_OK) { succeeded = true; break; }
        CHECK(result == MMS_E_MEMORY && allocationFailures == 1 && PixelsEqual(canvas, expected));
        CHECK(canvas.Width() == 2 && canvas.Height() == 2); ++failures;
    }
    CHECK(succeeded && failures != 0);
    CHECK(canvas.Width() == 64 && canvas.Height() == 64 && canvas.Pixels()[0] == 0xffabcdef);
    CHECK(canvas.Pixels()[64 * 64 - 1] == 0xffabcdef);
    return true;
}

int main() {
    if (!BackgroundAndChannels() || !AlphaRules() || !ScaleRules() || !ExtremeAspectAndAliasing() ||
        !InvalidInputsAndAtomicity() || !EmptyAndLimits() || !AllocationFailure()) return 1;
    std::printf("PASS %u CPU image canvas checks\n", checks);
    return 0;
}
