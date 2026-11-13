#pragma once
#include "background_picture_asset.hpp"
#include <cstdint>
#include <memory>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::media {
enum class PictureActivationResult { Ok, Invalid, NotReady, Stale, TooLarge, NoMemory, DecodeFailed };
struct PicturePresentation {
    std::int32_t x = 0, y = 0;
    float scale = 1.f;
    bool visible = false;
};

// Internal preparation primitive, not a history command or public plugin API.
// UI-thread only; the application must remain alive until publication. Cleanup
// never dereferences the app and releases COM references outside registry locks.
class PreparedPictureActivation final {
public:
    ~PreparedPictureActivation();
    PreparedPictureActivation(const PreparedPictureActivation&) = delete;
    PreparedPictureActivation& operator=(const PreparedPictureActivation&) = delete;
    // out must be empty. Failure preserves the native picture and out. Null
    // target means canonical empty state (0,0,1,false), ignoring presentation.
    // The bound covers retained sources, object storage and conservative managed
    // texture payloads of both current and candidate pictures, not process RSS,
    // driver overhead or decoder scratch. It is not a global resource budget.
    static PictureActivationResult Prepare(MMDApp* app, BackgroundPictureAssetPtr target,
        PicturePresentation presentation, std::uint64_t maxPreparedBytes,
        std::unique_ptr<PreparedPictureActivation>& out) noexcept;
    // Capture and decode a new file privately, retaining that first candidate
    // texture. No scene mutation, dirty flag, UI operation or error dialog.
    static PictureActivationResult PrepareImport(MMDApp* app, const wchar_t* path,
        std::uint64_t maxPreparedBytes, std::unique_ptr<PreparedPictureActivation>& out) noexcept;
    // Retain the current complete asset/texture without decoding. The two COM
    // references share one texture payload; publication still gets a new ticket.
    static PictureActivationResult PreparePresentation(MMDApp* app, PicturePresentation presentation,
        std::uint64_t maxPreparedBytes, std::unique_ptr<PreparedPictureActivation>& out) noexcept;
    const BackgroundPictureAssetPtr& TargetAsset() const noexcept;
    const PicturePresentation& TargetPresentation() const noexcept;
    bool IsCurrent() const noexcept;
    // No allocation, COM call, message, dirty flag, UI refresh or history entry.
    // Retired texture remains owned by this object until destruction; callers
    // must finish their history publication before destroying it or refreshing.
    bool Publish() noexcept;
    std::uint64_t PreparedBytes() const noexcept;
    std::uint64_t ResourceBytes() const noexcept;
private:
    struct State;
    explicit PreparedPictureActivation(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
};
}
