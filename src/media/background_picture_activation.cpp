#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/background_picture_activation.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/background_picture_policy.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "background_picture_decode.hpp"
#include "../plugins/background_identity.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <limits>
#include <new>
#include <utility>

namespace mikumikustudio::media {
namespace identity = plugins::background_identity;
namespace {
std::uint64_t Add(std::uint64_t a, std::uint64_t b) noexcept {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
// Up to 128 bits per texel, including the managed system-memory copy. Both
// decode recipes have bounded target dimensions regardless of source size.
std::uint64_t TextureBound(PictureDecodeMode mode) noexcept {
    if (mode == PictureDecodeMode::Fallback512) return 512ull * 512 * 32;
    std::uint64_t texels = 0;
    for (unsigned side = 1024; side; side >>= 1) texels += std::uint64_t(side) * side;
    return texels * 32;
}
std::uint64_t TextureBound(const BackgroundPictureAssetPtr& asset) noexcept {
    return asset ? TextureBound(asset->DecodeMode()) : 0;
}
}
struct PreparedPictureActivation::State {
    MMDApp* app = nullptr;
    identity::PicturePreparation receipt;
    std::uint64_t scene = 0, bytes = 0, resourceBytes = 0;
    D3DRenderer* renderer = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* original = nullptr; // borrowed while receipt remains valid
    IDirect3DTexture9* candidate = nullptr;
    IDirect3DTexture9* retired = nullptr;
    std::array<wchar_t, 256> path{};
    std::int32_t width = 0, height = 0, x = 0, y = 0;
    float scale = 1.f;
    std::uint8_t enabled = 0;
    BackgroundPictureAssetPtr target;
    PicturePresentation presentation;
    bool deviceOwned = false, published = false;

    PictureActivationResult Snapshot(MMDApp* owner, std::uint64_t instance) noexcept {
        app = owner;
        if (!identity::PreparePictureActivation(app, instance, receipt)) return PictureActivationResult::NotReady;
        scene = app->SceneGeneration(); renderer = app->Renderer();
        device = renderer ? renderer->device : nullptr;
        if (!device) return PictureActivationResult::NotReady;
        original = app->PictureBackgroundTexture();
        width = app->PictureWidth(); height = app->PictureHeight();
        x = app->PictureOffsetX(); y = app->PictureOffsetY();
        std::memcpy(&scale, &app->PictureScale(), sizeof(float));
        enabled = app->PictureBackgroundEnabled();
        std::memcpy(path.data(), app->PictureBackgroundPath(), sizeof path);
        if (wcsnlen_s(path.data(), path.size()) == path.size()) return PictureActivationResult::NotReady;
        const auto& before = receipt.expectedAsset;
        if (bool(before) != bool(original)) return PictureActivationResult::NotReady;
        if (before && (before->Width() != width || before->Height() != height ||
            std::wcscmp(before->Path(), path.data()))) return PictureActivationResult::NotReady;
        if (!before && (path[0] || enabled)) return PictureActivationResult::NotReady;
        return Current() ? PictureActivationResult::Ok : PictureActivationResult::Stale;
    }
    std::uint64_t BaseBytes() const noexcept {
        auto total = Add(sizeof(State) + sizeof(PreparedPictureActivation) + 8 * sizeof(void*), TextureBound(receipt.expectedAsset));
        return receipt.expectedAsset ? Add(total, receipt.expectedAsset->RetainedBytes()) : total;
    }
    PictureActivationResult ValidateTexture(PictureDecodeMode mode) const noexcept {
        if (!Current()) return PictureActivationResult::Stale;
        if (!candidate) return PictureActivationResult::DecodeFailed;
        IDirect3DDevice9* decodedDevice = nullptr;
        const HRESULT located = candidate->GetDevice(&decodedDevice);
        const bool sameDevice = SUCCEEDED(located) && decodedDevice == device;
        if (decodedDevice) decodedDevice->Release();
        if (!Current()) return PictureActivationResult::Stale;
        if (!sameDevice) return PictureActivationResult::DecodeFailed;
        D3DSURFACE_DESC description{};
        const HRESULT described = candidate->GetLevelDesc(0, &description);
        if (!Current()) return PictureActivationResult::Stale;
        const UINT levels = candidate->GetLevelCount();
        if (!Current()) return PictureActivationResult::Stale;
        const bool fallback = mode == PictureDecodeMode::Fallback512;
        const UINT side = fallback ? 512 : 1024;
        if (FAILED(described) || description.Width > side || description.Height > side ||
            !description.Width || !description.Height || description.Pool != D3DPOOL_MANAGED ||
            !levels || levels > (fallback ? 1u : 11u)) return PictureActivationResult::DecodeFailed;
        return PictureActivationResult::Ok;
    }

    ~State() {
        // D3D release hooks may dispatch window input or closing. The prepared
        // owner never touches the app here; defer such messages while its
        // private/retired resources are being destroyed, including failures.
        bake::ScopedUiInputGate inputGate;
        // These calls may reenter. All publication is already complete, and
        // this destructor does not access the app, renderer or native fields.
        if (candidate) candidate->Release();
        if (retired) retired->Release();
        if (deviceOwned) device->Release();
    }
    bool Current() const noexcept {
        if (published || !identity::PicturePreparationCurrent(receipt)) return false;
        // Registry validation comes first: wrong-thread/retired-owner calls
        // must not dereference a stale application pointer.
        return app->SceneGeneration() == scene && app->Renderer() == renderer && renderer &&
            renderer->device == device && app->PictureBackgroundTexture() == original &&
            app->PictureWidth() == width && app->PictureHeight() == height &&
            app->PictureOffsetX() == x && app->PictureOffsetY() == y &&
            !std::memcmp(&app->PictureScale(), &scale, sizeof scale) &&
            app->PictureBackgroundEnabled() == enabled &&
            !std::memcmp(app->PictureBackgroundPath(), path.data(), sizeof path);
    }
};

PreparedPictureActivation::PreparedPictureActivation(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
PreparedPictureActivation::~PreparedPictureActivation() = default;
bool PreparedPictureActivation::IsCurrent() const noexcept { return state_ && state_->Current(); }
std::uint64_t PreparedPictureActivation::PreparedBytes() const noexcept { return state_->bytes; }
std::uint64_t PreparedPictureActivation::ResourceBytes() const noexcept { return state_->resourceBytes; }
const BackgroundPictureAssetPtr& PreparedPictureActivation::TargetAsset() const noexcept { return state_->target; }
const PicturePresentation& PreparedPictureActivation::TargetPresentation() const noexcept { return state_->presentation; }

PictureActivationResult PreparedPictureActivation::Prepare(MMDApp* app, BackgroundPictureAssetPtr target,
    PicturePresentation presentation, std::uint64_t maxPreparedBytes,
    std::unique_ptr<PreparedPictureActivation>& out) noexcept {
    if (!app || out) return PictureActivationResult::Invalid;
    const auto instance = identity::Instance(app);
    if (!identity::IsOwner(app, instance)) return PictureActivationResult::NotReady;
    try {
        auto state = std::make_unique<State>();
        // Reserving a number must not establish or change the active ticket,
        // including a picture that has not yet been queried through Acquire.
        if (const auto result = state->Snapshot(app, instance); result != PictureActivationResult::Ok) return result;
        const auto& before = state->receipt.expectedAsset;
        state->target = std::move(target);
        state->presentation = state->target ? presentation : PicturePresentation{};
        state->resourceBytes = TextureBound(state->target);
        auto bytes = Add(state->BaseBytes(), state->resourceBytes);
        if (state->target && state->target != before) {
            bytes = Add(bytes, BackgroundPictureAsset::OverheadBytes());
            if (!before || state->target->Source() != before->Source())
                bytes = Add(bytes, state->target->Source()->RetainedBytes());
        }
        if (bytes == UINT64_MAX || bytes > maxPreparedBytes) return PictureActivationResult::TooLarge;
        state->bytes = bytes;
        if (!state->Current()) return PictureActivationResult::Stale;
        state->device->AddRef(); state->deviceOwned = true;
        if (!state->Current()) return PictureActivationResult::Stale;
        if (state->target) {
            const auto& source = state->target->Source();
            picture_detail::ImageInfo info{};
            HRESULT result = E_FAIL;
            if (!picture_detail::DecodeAttempt(state->device, source, state->target->DecodeMode(),
                info, state->candidate, result, [&]() noexcept { return state->Current(); }))
                return PictureActivationResult::Stale;
            if (FAILED(result) || !state->candidate || info.Width != UINT(state->target->Width()) ||
                info.Height != UINT(state->target->Height())) return PictureActivationResult::DecodeFailed;
            // Reject a decoder result outside the recipe we budgeted. No
            // alternate recipe or disk fallback is allowed during restoration.
            if (const auto checked = state->ValidateTexture(state->target->DecodeMode());
                checked != PictureActivationResult::Ok) return checked;
        }
        auto prepared = std::unique_ptr<PreparedPictureActivation>(new PreparedPictureActivation(std::move(state)));
        if (!prepared->IsCurrent()) return PictureActivationResult::Stale;
        out = std::move(prepared);
        return PictureActivationResult::Ok;
    } catch (const std::bad_alloc&) { return PictureActivationResult::NoMemory; }
    catch (...) { return PictureActivationResult::DecodeFailed; }
}

PictureActivationResult PreparedPictureActivation::PreparePresentation(MMDApp* app, PicturePresentation presentation,
    std::uint64_t maxPreparedBytes, std::unique_ptr<PreparedPictureActivation>& out) noexcept {
    if (!app || out) return PictureActivationResult::Invalid;
    const auto instance = identity::Instance(app);
    if (!identity::IsOwner(app, instance)) return PictureActivationResult::NotReady;
    try {
        auto state = std::make_unique<State>();
        if (const auto result = state->Snapshot(app, instance); result != PictureActivationResult::Ok) return result;
        if (!state->receipt.expectedAsset || !state->original) return PictureActivationResult::NotReady;
        state->target = state->receipt.expectedAsset;
        state->presentation = presentation;
        state->bytes = state->BaseBytes(); // one resident payload, two references
        state->resourceBytes = TextureBound(state->target);
        if (state->bytes == UINT64_MAX || state->bytes > maxPreparedBytes) return PictureActivationResult::TooLarge;
        state->device->AddRef(); state->deviceOwned = true;
        if (!state->Current()) return PictureActivationResult::Stale;
        state->original->AddRef(); state->candidate = state->original;
        if (!state->Current()) return PictureActivationResult::Stale;
        auto prepared = std::unique_ptr<PreparedPictureActivation>(new PreparedPictureActivation(std::move(state)));
        if (!prepared->IsCurrent()) return PictureActivationResult::Stale;
        out = std::move(prepared);
        return PictureActivationResult::Ok;
    } catch (const std::bad_alloc&) { return PictureActivationResult::NoMemory; }
    catch (...) { return PictureActivationResult::DecodeFailed; }
}

PictureActivationResult PreparedPictureActivation::PrepareImport(MMDApp* app, const wchar_t* input,
    std::uint64_t maxPreparedBytes, std::unique_ptr<PreparedPictureActivation>& out) noexcept {
    if (!app || out || !input || !input[0] || wcsnlen_s(input, 256) >= 256) return PictureActivationResult::Invalid;
    const auto instance = identity::Instance(app);
    if (!identity::IsOwner(app, instance)) return PictureActivationResult::NotReady;
    try {
        auto state = std::make_unique<State>();
        if (const auto result = state->Snapshot(app, instance); result != PictureActivationResult::Ok) return result;
        std::array<wchar_t, 256> request{};
        wcscpy_s(request.data(), request.size(), input);
        // Use a private path workspace: resolving an import must not change
        // the application's path scratch buffers before journal publication.
        PathResolutionWorkspace paths = app->PathWorkspace();
        ResolveUserFilePath(paths, request.data());
        if (!state->Current()) return PictureActivationResult::Stale;
        if (!paths.resolvedPath[0] || wcsnlen_s(paths.resolvedPath, 256) >= 256)
            return PictureActivationResult::Invalid;
        std::array<wchar_t, 256> resolved{};
        wcscpy_s(resolved.data(), resolved.size(), paths.resolvedPath);
        // Admit the larger primary recipe before reading/decoding; falling
        // back must never release the old asset to manufacture spare budget.
        const auto base = Add(Add(state->BaseBytes(), TextureBound(PictureDecodeMode::Primary1024)),
            BackgroundPictureAsset::OverheadBytes());
        if (base == UINT64_MAX || base >= maxPreparedBytes) return PictureActivationResult::TooLarge;
        std::uint64_t sourceLimit = 0;
        const auto policy = ReadBackgroundPictureSourceLimit(sourceLimit);
        if (!state->Current()) return PictureActivationResult::Stale;
        if (policy != system_settings::Result::Ok) return PictureActivationResult::NotReady;
        auto& budget = GlobalPictureSourceBudget();
        budget.Configure(sourceLimit);
        if (!state->Current()) return PictureActivationResult::Stale;
        BackgroundPictureSourcePtr source;
        const auto available = (std::min)(sourceLimit, maxPreparedBytes - base);
        const auto captured = CaptureBackgroundPictureSource(resolved.data(), available, budget, source);
        if (!state->Current()) return PictureActivationResult::Stale;
        if (captured != PictureSourceResult::Ok) {
            if (captured == PictureSourceResult::TooLarge) return PictureActivationResult::TooLarge;
            if (captured == PictureSourceResult::NoMemory) return PictureActivationResult::NoMemory;
            return PictureActivationResult::DecodeFailed;
        }
        state->bytes = Add(base, source->RetainedBytes());
        if (state->bytes == UINT64_MAX || state->bytes > maxPreparedBytes) return PictureActivationResult::TooLarge;
        state->device->AddRef(); state->deviceOwned = true;
        if (!state->Current()) return PictureActivationResult::Stale;
        auto mode = PictureDecodeMode::Primary1024;
        picture_detail::ImageInfo info{};
        HRESULT decoded = E_FAIL;
        const auto current = [&]() noexcept { return state->Current(); };
        if (!picture_detail::DecodeAttempt(state->device, source, mode, info, state->candidate, decoded, current))
            return PictureActivationResult::Stale;
        if (FAILED(decoded)) {
            // Clear our pointer before Release: a reentrant failure must not
            // expose or accidentally release the abandoned candidate twice.
            auto* abandoned = state->candidate; state->candidate = nullptr;
            if (abandoned) abandoned->Release();
            if (!current()) return PictureActivationResult::Stale;
            mode = PictureDecodeMode::Fallback512; info = {};
            if (!picture_detail::DecodeAttempt(state->device, source, mode, info, state->candidate, decoded, current))
                return PictureActivationResult::Stale;
        }
        if (FAILED(decoded) || !state->candidate || !info.Width || !info.Height ||
            info.Width > INT32_MAX || info.Height > INT32_MAX) return PictureActivationResult::DecodeFailed;
        if (const auto checked = state->ValidateTexture(mode); checked != PictureActivationResult::Ok) return checked;
        if (!picture_detail::ImportPlacement(*app, *state->renderer, static_cast<std::int32_t>(info.Width),
            static_cast<std::int32_t>(info.Height), state->presentation.scale, state->presentation.y))
            return PictureActivationResult::NotReady;
        state->presentation.x = 0; state->presentation.visible = true;
        const auto created = CreateBackgroundPictureAsset(source, resolved.data(), static_cast<std::int32_t>(info.Width),
            static_cast<std::int32_t>(info.Height), mode, maxPreparedBytes, state->target);
        if (!current()) return PictureActivationResult::Stale;
        if (created != PictureAssetResult::Ok) {
            if (created == PictureAssetResult::TooLarge) return PictureActivationResult::TooLarge;
            if (created == PictureAssetResult::NoMemory) return PictureActivationResult::NoMemory;
            return PictureActivationResult::NotReady;
        }
        state->resourceBytes = TextureBound(mode);
        // PreparedBytes retains the primary peak even after a smaller fallback;
        // ResourceBytes is the actual target recipe charged after publication.
        auto prepared = std::unique_ptr<PreparedPictureActivation>(new PreparedPictureActivation(std::move(state)));
        if (!prepared->IsCurrent()) return PictureActivationResult::Stale;
        out = std::move(prepared);
        return PictureActivationResult::Ok;
    } catch (const std::bad_alloc&) { return PictureActivationResult::NoMemory; }
    catch (...) { return PictureActivationResult::DecodeFailed; }
}

bool PreparedPictureActivation::Publish() noexcept {
    if (!IsCurrent()) return false;
    auto& s = *state_;
    auto& app = *s.app;
    if (!identity::PublishPreparedPicture(s.receipt, app.PictureBackgroundTexture(), s.candidate, s.retired,
        app.PictureBackgroundPath(), s.target ? s.target->Path() : L"",
        app.PictureWidth(), s.target ? s.target->Width() : 0,
        app.PictureHeight(), s.target ? s.target->Height() : 0,
        app.PictureOffsetX(), app.PictureOffsetY(), app.PictureScale(),
        s.presentation.x, s.presentation.y, s.presentation.scale,
        app.PictureBackgroundEnabled(), std::uint8_t(s.presentation.visible), s.target)) return false;
    s.candidate = nullptr;
    s.published = true;
    return true;
}
}
#endif
