#pragma once

// CPU-only compositor for one mms.render.image 1.0 CPU image. The helper owns
// only the output pixels; it never creates, retains or maps host images and
// performs no window, GPU, scene or host-table access.
//
// Output is tightly packed 32-bit BGRA in Windows little-endian byte order
// (B, G, R, A) with alpha forced to 255. Channels are combined in sRGB byte
// space; there is no linear-light conversion, no implicit premultiplication and
// the source pixels are never modified.
//
// Compose validates the options and both inputs before allocating. A candidate
// buffer is prepared completely and swapped in only on success, so a failure
// keeps the previous pixels and dimensions. Error mapping follows the image
// service conventions: ARGUMENT for invalid options (a background above 24
// bits or an unknown ImageScale), invalid headers, pointers, dimensions, flags
// or device generation and for geometry outside the CPU image contract;
// UNSUPPORTED for pixel formats, color spaces and alpha modes outside 1.0;
// MEMORY for dimensions or byte counts beyond 16,384 pixels and 64 MiB and for
// allocation failure; FAILED for any other exception.
#include <mms/render_image.h>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace mms::sdk {
enum class ImageScale { Contain, Stretch };

namespace image_canvas_detail {
constexpr std::uint32_t kMaxDimension = 16384u;
constexpr std::uint64_t kMaxBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint32_t kBackgroundMask = 0x00FFFFFFu;
constexpr std::uint32_t kOpaqueAlpha = 0xFFu;

inline bool HeaderValid(const MmsHeader& header, std::size_t bytes) noexcept {
    return header.size >= bytes && header.major == MMS_ABI_MAJOR && header.minor == MMS_ABI_MINOR;
}

inline std::uint32_t Pack(std::uint32_t red, std::uint32_t green, std::uint32_t blue) noexcept {
    return (kOpaqueAlpha << 24) | (red << 16) | (green << 8) | blue;
}

inline std::uint32_t BlendStraight(std::uint32_t channel, std::uint32_t alpha,
                                   std::uint32_t background) noexcept {
    return (channel * alpha + background * (255u - alpha) + 127u) / 255u;
}

inline std::uint32_t BlendPremultiplied(std::uint32_t channel, std::uint32_t alpha,
                                        std::uint32_t background) noexcept {
    const std::uint32_t value = (channel * 255u + background * (255u - alpha) + 127u) / 255u;
    return value > 255u ? 255u : value;
}

inline MmsResult ValidateInfo(const MmsRenderImageInfoV1& info) noexcept {
    if (!HeaderValid(info.h, sizeof(info))) return MMS_E_ARGUMENT;
    if (info.width == 0 || info.height == 0) return MMS_E_ARGUMENT;
    if (info.width > kMaxDimension || info.height > kMaxDimension) return MMS_E_MEMORY;
    const std::uint64_t rowStride = std::uint64_t(info.width) * 4u;
    if (info.rowStride != rowStride) return MMS_E_ARGUMENT;
    const std::uint64_t byteCount = rowStride * info.height;
    if (byteCount > kMaxBytes) return MMS_E_MEMORY;
    if (info.byteCount != byteCount) return MMS_E_ARGUMENT;
    if (info.flags != 0 || info.deviceGeneration != 0) return MMS_E_ARGUMENT;
    if (info.format != MMS_IMAGE_RGBA8 && info.format != MMS_IMAGE_BGRA8) return MMS_E_UNSUPPORTED;
    if (info.colorSpace != MMS_IMAGE_COLOR_SRGB) return MMS_E_UNSUPPORTED;
    if (info.alphaMode != MMS_IMAGE_ALPHA_OPAQUE && info.alphaMode != MMS_IMAGE_ALPHA_STRAIGHT &&
        info.alphaMode != MMS_IMAGE_ALPHA_PREMULTIPLIED) return MMS_E_UNSUPPORTED;
    return MMS_OK;
}

inline MmsResult ValidateMap(const MmsRenderImageMapV1& map, const MmsRenderImageInfoV1& info) noexcept {
    if (!HeaderValid(map.h, sizeof(map))) return MMS_E_ARGUMENT;
    if (map.mapping == 0 || map.pixels == nullptr) return MMS_E_ARGUMENT;
    if (map.rowStride != info.rowStride || map.byteCount < info.byteCount) return MMS_E_ARGUMENT;
    // The caller guarantees readability. Only the bytes actually read
    // (info.byteCount, validated non-zero) must have an address that does not
    // wrap; the mapping may grant a larger span. A last byte at UINTPTR_MAX is
    // representable and accepted.
    const std::uint64_t address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(map.pixels));
    const std::uint64_t lastAddress = static_cast<std::uint64_t>(~static_cast<std::uintptr_t>(0));
    if (info.byteCount - 1u > lastAddress - address) return MMS_E_ARGUMENT;
    return MMS_OK;
}

inline void RenderImage(std::uint32_t* target, std::uint32_t width, std::uint32_t height,
                        const MmsRenderImageInfoV1& info, const MmsRenderImageMapV1& map,
                        ImageScale scale, std::uint32_t background) noexcept {
    const std::uint32_t backgroundRed = (background >> 16) & 0xFFu;
    const std::uint32_t backgroundGreen = (background >> 8) & 0xFFu;
    const std::uint32_t backgroundBlue = background & 0xFFu;
    const bool rgba = info.format == MMS_IMAGE_RGBA8;

    std::uint64_t fitWidth = width;
    std::uint64_t fitHeight = height;
    std::uint64_t offsetX = 0;
    std::uint64_t offsetY = 0;
    if (scale == ImageScale::Contain) {
        if (std::uint64_t(info.height) * width <= std::uint64_t(height) * info.width) {
            fitWidth = width;
            fitHeight = std::uint64_t(info.height) * width / info.width;
            if (fitHeight == 0) fitHeight = 1;
        } else {
            fitHeight = height;
            fitWidth = std::uint64_t(info.width) * height / info.height;
            if (fitWidth == 0) fitWidth = 1;
        }
        offsetX = (std::uint64_t(width) - fitWidth) / 2;
        offsetY = (std::uint64_t(height) - fitHeight) / 2;
    }

    const unsigned char* source = static_cast<const unsigned char*>(map.pixels);
    for (std::uint64_t y = 0; y != fitHeight; ++y) {
        const std::uint64_t sourceY = y * info.height / fitHeight;
        const unsigned char* row = source + sourceY * map.rowStride;
        std::uint32_t* destinationRow =
            target + static_cast<std::size_t>((offsetY + y) * width + offsetX);
        for (std::uint64_t x = 0; x != fitWidth; ++x) {
            const std::uint64_t sourceX = x * info.width / fitWidth;
            const unsigned char* pixel = row + sourceX * 4u;
            std::uint32_t red, green, blue, alpha;
            if (rgba) {
                red = pixel[0];
                green = pixel[1];
                blue = pixel[2];
                alpha = pixel[3];
            } else {
                blue = pixel[0];
                green = pixel[1];
                red = pixel[2];
                alpha = pixel[3];
            }
            switch (info.alphaMode) {
            case MMS_IMAGE_ALPHA_OPAQUE:
                break;
            case MMS_IMAGE_ALPHA_STRAIGHT:
                red = BlendStraight(red, alpha, backgroundRed);
                green = BlendStraight(green, alpha, backgroundGreen);
                blue = BlendStraight(blue, alpha, backgroundBlue);
                break;
            default: // MMS_IMAGE_ALPHA_PREMULTIPLIED, validated by ValidateInfo.
                red = BlendPremultiplied(red, alpha, backgroundRed);
                green = BlendPremultiplied(green, alpha, backgroundGreen);
                blue = BlendPremultiplied(blue, alpha, backgroundBlue);
                break;
            }
            destinationRow[x] = Pack(red, green, blue);
        }
    }
}
} // namespace image_canvas_detail

struct ImageCanvasOptions {
    ImageScale scale = ImageScale::Contain;
    std::uint32_t background = 0x202020u; // 0x00RRGGBB
};

class CpuImageCanvas {
public:
    // Composes the optional image into an opaque background canvas. A zero
    // width or height produces an empty canvas without allocating pixels, for
    // example Compose(..., 0, 7) succeeds with Width() == 0, Height() == 7 and
    // Pixels() == nullptr; requested dimensions are never collapsed to zero.
    // An unknown ImageScale value returns ARGUMENT. The inputs are only read
    // for the duration of this call; no host handle, mapping or lease is stored.
    MmsResult Compose(const MmsRenderImageInfoV1* info, const MmsRenderImageMapV1* map,
                      std::uint32_t width, std::uint32_t height,
                      ImageCanvasOptions options = {}) noexcept {
        try {
            return ComposeChecked(info, map, width, height, options);
        } catch (const std::bad_alloc&) {
            return MMS_E_MEMORY;
        } catch (...) {
            return MMS_E_FAILED;
        }
    }

    const std::uint32_t* Pixels() const noexcept {
        return pixels_.empty() ? nullptr : pixels_.data();
    }
    std::uint32_t Width() const noexcept { return width_; }
    std::uint32_t Height() const noexcept { return height_; }

private:
    MmsResult ComposeChecked(const MmsRenderImageInfoV1* info, const MmsRenderImageMapV1* map,
                             std::uint32_t width, std::uint32_t height,
                             const ImageCanvasOptions& options) {
        using namespace image_canvas_detail;
        if (options.background > kBackgroundMask) return MMS_E_ARGUMENT;
        if (options.scale != ImageScale::Contain && options.scale != ImageScale::Stretch)
            return MMS_E_ARGUMENT;
        if ((info == nullptr) != (map == nullptr)) return MMS_E_ARGUMENT;
        if (width > kMaxDimension || height > kMaxDimension) return MMS_E_MEMORY;
        if (std::uint64_t(width) * height * 4u > kMaxBytes) return MMS_E_MEMORY;
        if (info != nullptr) {
            if (const MmsResult result = ValidateInfo(*info)) return result;
            if (const MmsResult result = ValidateMap(*map, *info)) return result;
        }

        const std::uint32_t backgroundGreen = (options.background >> 8) & 0xFFu;
        const std::uint32_t backgroundRed = (options.background >> 16) & 0xFFu;
        const std::uint32_t backgroundBlue = options.background & 0xFFu;
        if (width == 0 || height == 0) {
            std::vector<std::uint32_t> empty;
            pixels_.swap(empty);
            width_ = width;
            height_ = height;
            return MMS_OK;
        }

        std::vector<std::uint32_t> candidate(
            static_cast<std::size_t>(width) * height, Pack(backgroundRed, backgroundGreen, backgroundBlue));
        if (info != nullptr) RenderImage(candidate.data(), width, height, *info, *map, options.scale, options.background);
        pixels_.swap(candidate);
        width_ = width;
        height_ = height;
        return MMS_OK;
    }

    std::vector<std::uint32_t> pixels_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};
} // namespace mms::sdk
