#include "mikumikustudio/background_picture_asset.hpp"
#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace mikumikustudio::media {
namespace {
std::uint64_t NextIdentity() noexcept {
    static std::atomic<std::uint64_t> last{0};
    auto observed = last.load(std::memory_order_relaxed);
    while (observed != (std::numeric_limits<std::uint64_t>::max)()) {
        if (last.compare_exchange_weak(observed, observed + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) return observed + 1;
    }
    return 0;
}
}

BackgroundPictureAsset::BackgroundPictureAsset(BackgroundPictureSourcePtr source,
    const wchar_t* path, std::uint32_t length, std::int32_t width, std::int32_t height,
    PictureDecodeMode mode, std::uint64_t id) noexcept
    : source_(std::move(source)), width_(width), height_(height), mode_(mode), id_(id) {
    for (std::uint32_t i = 0; i < length; ++i) path_[i] = path[i];
}
std::uint64_t BackgroundPictureAsset::OverheadBytes() noexcept {
    return sizeof(BackgroundPictureAsset) + 4 * sizeof(void*);
}
std::uint64_t BackgroundPictureAsset::RetainedBytes() const noexcept {
    // Construction checked this sum, and both operands are immutable.
    return OverheadBytes() + source_->RetainedBytes();
}

PictureAssetResult CreateBackgroundPictureAsset(BackgroundPictureSourcePtr source,
    const wchar_t* path, std::int32_t width, std::int32_t height, PictureDecodeMode mode,
    std::uint64_t maxRetainedBytes, BackgroundPictureAssetPtr& out) noexcept {
    if (!source || !path || width <= 0 || height <= 0 ||
        (mode != PictureDecodeMode::Primary1024 && mode != PictureDecodeMode::Fallback512)) return PictureAssetResult::Invalid;
    std::uint32_t length = 0;
    while (length < 256 && path[length]) ++length;
    if (!length || length == 256) return PictureAssetResult::Invalid;
    const auto sourceBytes = source->RetainedBytes(), overhead = BackgroundPictureAsset::OverheadBytes();
    if (sourceBytes > (std::numeric_limits<std::uint64_t>::max)() - overhead ||
        maxRetainedBytes < overhead || sourceBytes > maxRetainedBytes - overhead) return PictureAssetResult::TooLarge;
    const auto id = NextIdentity();
    if (!id) return PictureAssetResult::IdentityExhausted;
    try {
        BackgroundPictureAssetPtr candidate(new BackgroundPictureAsset(std::move(source), path, length, width, height, mode, id));
        out.swap(candidate);
        return PictureAssetResult::Ok;
    } catch (const std::bad_alloc&) {
        return PictureAssetResult::NoMemory;
    } catch (...) {
        return PictureAssetResult::NoMemory;
    }
}
}
