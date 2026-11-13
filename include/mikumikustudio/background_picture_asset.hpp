#pragma once

#include "background_picture_source.hpp"
#include <cstdint>
#include <memory>

namespace mikumikustudio::media {
enum class PictureDecodeMode : std::uint32_t { Primary1024 = 1, Fallback512 = 2 };
enum class PictureAssetResult { Ok, Invalid, TooLarge, NoMemory, IdentityExhausted };

// Immutable logical asset, independent of a D3D device or activation ticket.
// The path is metadata only; creating an asset never opens it.
class BackgroundPictureAsset final {
public:
    ~BackgroundPictureAsset() = default;
    BackgroundPictureAsset(const BackgroundPictureAsset&) = delete;
    BackgroundPictureAsset& operator=(const BackgroundPictureAsset&) = delete;
    std::uint64_t Id() const noexcept { return id_; }
    const BackgroundPictureSourcePtr& Source() const noexcept { return source_; }
    const wchar_t* Path() const noexcept { return path_; }
    std::int32_t Width() const noexcept { return width_; }
    std::int32_t Height() const noexcept { return height_; }
    PictureDecodeMode DecodeMode() const noexcept { return mode_; }
    // Includes the shared source for future history accounting. Metadata is
    // not charged to PictureSourceBudget and sharing does not reserve again.
    std::uint64_t RetainedBytes() const noexcept;
    static std::uint64_t OverheadBytes() noexcept;
private:
    BackgroundPictureAsset(BackgroundPictureSourcePtr, const wchar_t*, std::uint32_t,
        std::int32_t, std::int32_t, PictureDecodeMode, std::uint64_t) noexcept;
    BackgroundPictureSourcePtr source_;
    wchar_t path_[256]{};
    std::int32_t width_, height_;
    PictureDecodeMode mode_;
    std::uint64_t id_;
    friend PictureAssetResult CreateBackgroundPictureAsset(BackgroundPictureSourcePtr,
        const wchar_t*, std::int32_t, std::int32_t, PictureDecodeMode, std::uint64_t,
        std::shared_ptr<const BackgroundPictureAsset>&) noexcept;
};
using BackgroundPictureAssetPtr = std::shared_ptr<const BackgroundPictureAsset>;

// Failure preserves out. IDs are process-unique and never reused, including
// IDs consumed by a failed allocation. No file, GPU or scene work is performed.
PictureAssetResult CreateBackgroundPictureAsset(BackgroundPictureSourcePtr source,
    const wchar_t* path, std::int32_t width, std::int32_t height, PictureDecodeMode mode,
    std::uint64_t maxRetainedBytes, BackgroundPictureAssetPtr& out) noexcept;
}
