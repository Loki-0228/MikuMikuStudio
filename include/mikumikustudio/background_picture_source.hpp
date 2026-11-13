#pragma once

#include <cstdint>
#include <memory>

namespace mikumikustudio::media {
enum class PictureSourceResult { Ok, Invalid, OpenFailed, TooLarge, ReadFailed, NoMemory };
namespace detail { struct PictureSourceBudgetState; }
class BackgroundPictureSource;

struct PictureSourceBudgetUsage { std::uint64_t limit, used; };
class PictureSourceBudget final {
public:
    // Construction may throw on allocation failure; never falls back to an
    // unbounded budget. Copies share one accounting state and configuration.
    explicit PictureSourceBudget(std::uint64_t limit = 128ull * 1024 * 1024);
    PictureSourceBudget(const PictureSourceBudget&) noexcept = default;
    PictureSourceBudget& operator=(const PictureSourceBudget&) = delete;
    PictureSourceBudget(PictureSourceBudget&&) = delete;
    PictureSourceBudget& operator=(PictureSourceBudget&&) = delete;
    void Configure(std::uint64_t limit) noexcept;
    PictureSourceBudgetUsage Usage() const noexcept;
private:
    std::shared_ptr<detail::PictureSourceBudgetState> state_;
    friend PictureSourceResult CaptureBackgroundPictureSource(const wchar_t*, std::uint64_t,
        PictureSourceBudget&, std::shared_ptr<const BackgroundPictureSource>&) noexcept;
};
// Initial construction may throw. The noexcept capture wrapper translates an
// allocation failure into NoMemory and preserves the previously published out.
PictureSourceBudget& GlobalPictureSourceBudget();

// Immutable encoded source bytes, independent of the source file and renderer.
// This is not a decoded-image budget or an undo implementation.
class BackgroundPictureSource final {
public:
    ~BackgroundPictureSource() noexcept;
    BackgroundPictureSource(const BackgroundPictureSource&) = delete;
    BackgroundPictureSource& operator=(const BackgroundPictureSource&) = delete;
    const unsigned char* Data() const noexcept { return bytes_.get(); }
    std::uint32_t Size() const noexcept { return size_; }
    std::uint64_t RetainedBytes() const noexcept { return OverheadBytes() + size_; }
    static std::uint64_t OverheadBytes() noexcept;
private:
    BackgroundPictureSource(std::unique_ptr<unsigned char[]> bytes, std::uint32_t size,
        std::shared_ptr<detail::PictureSourceBudgetState> budget) noexcept;
    std::unique_ptr<unsigned char[]> bytes_;
    std::uint32_t size_;
    std::shared_ptr<detail::PictureSourceBudgetState> budget_;
    friend PictureSourceResult CaptureBackgroundPictureSource(const wchar_t*, std::uint64_t,
        PictureSourceBudget&, std::shared_ptr<const BackgroundPictureSource>&) noexcept;
};
using BackgroundPictureSourcePtr = std::shared_ptr<const BackgroundPictureSource>;

// Denies concurrent file writers/deletion while reading. Failure preserves out.
// maximumRetainedBytes includes payload and OverheadBytes(), before allocation.
PictureSourceResult CaptureBackgroundPictureSource(const wchar_t* path,
    std::uint64_t maximumRetainedBytes, BackgroundPictureSourcePtr& out) noexcept;
PictureSourceResult CaptureBackgroundPictureSource(const wchar_t* path,
    std::uint64_t maximumRetainedBytes, PictureSourceBudget& budget,
    BackgroundPictureSourcePtr& out) noexcept;
}
