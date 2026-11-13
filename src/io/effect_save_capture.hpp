#pragma once

#include "effect_save_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

namespace mikumikustudio::effect_save {

using EffectDigest = std::array<std::uint8_t, 32>;
// Match the existing effect assignment reader's upper bound. The caller can
// reserve a smaller per-save budget before capture; this is not a global lease.
inline constexpr std::size_t MaximumEffectBytes = 16 * 1024 * 1024;

struct EffectBytes {
    const std::uint8_t* data;
    std::size_t size;
};

class CaptureAdmission;

// A synchronous provider can only copy bytes in; it cannot borrow the host
// buffer or transfer ownership of its own memory. Never retain this sink.
class CaptureSink final {
public:
    bool Begin(std::size_t size) noexcept {
        if (failed_ || bytes_ || size == 0 || size > limit_) return Fail();
        try { bytes_ = std::make_unique<std::uint8_t[]>(size); }
        catch (...) { return Fail(); }
        size_ = size;
        return true;
    }

    bool Append(const void* source, std::size_t size) noexcept {
        if (failed_ || !bytes_ || size > size_ - written_ || (size && !source)) return Fail();
        if (size) std::memcpy(bytes_.get() + written_, source, size);
        written_ += size;
        return true;
    }

private:
    friend class CaptureAdmission;
    explicit CaptureSink(std::size_t limit) noexcept : limit_(limit) {}
    ~CaptureSink() = default;
    CaptureSink(CaptureSink&&) = delete;
    CaptureSink& operator=(CaptureSink&&) = delete;
    bool Fail() noexcept { failed_ = true; return false; }
    bool Complete() const noexcept { return !failed_ && bytes_ && written_ == size_; }

    const std::size_t limit_;
    std::size_t size_ = 0;
    std::size_t written_ = 0;
    bool failed_ = false;
    std::unique_ptr<std::uint8_t[]> bytes_;
};

enum class FreezeStatus { Unsupported, Failed, Cancelled, Frozen };
struct FreezeResult {
    FreezeStatus status = FreezeStatus::Failed;
    CaptureIdentity identity;
};

struct CaptureCallbacks {
    // Host: obtain the full current identity at the safe capture boundary.
    // No identity means there is no supported provider available at this point.
    // Observation and cancellation checks must not change scene/provider state.
    std::function<std::optional<CaptureIdentity>()> observe;
    std::function<bool()> cancelled;
    // Provider: freeze the actual effect state and attest which identity was
    // serialized. An asynchronous notification cannot satisfy this contract.
    std::function<FreezeResult(const Request&, CaptureSink&)> freeze;
    // Host (never provider): validate the exact EMM variant and hash the
    // accepted bytes with SHA-256. Neither callback may retain the view.
    std::function<bool(const Request&, EffectBytes)> validate;
    std::function<bool(EffectBytes, EffectDigest&)> sha256;
};

// The only value handed to a worker. It carries no callbacks, leases into
// provider memory, or live scene pointers. The request already names the
// final PMM and companion targets, including any selected increment number.
class CapturedEffects final {
public:
    const Request& GetRequest() const noexcept { return request_; }
    EffectBytes Bytes() const noexcept { return {bytes_.get(), size_}; }
    const EffectDigest& Digest() const noexcept { return digest_; }

private:
    friend class CaptureAdmission;
    CapturedEffects(Request&& request, std::unique_ptr<std::uint8_t[]>&& bytes,
        std::size_t size, EffectDigest digest) noexcept
        : request_(std::move(request)), bytes_(std::move(bytes)), size_(size), digest_(digest) {}
    CapturedEffects(CapturedEffects&&) = delete;
    CapturedEffects& operator=(CapturedEffects&&) = delete;

    const Request request_;
    const std::unique_ptr<const std::uint8_t[]> bytes_;
    const std::size_t size_;
    const EffectDigest digest_;
};

enum class CaptureStatus {
    Captured, InvalidRequest, Unsupported, Busy, Cancelled, Stale,
    ProviderFailed, PayloadRejected, ValidationFailed, DigestFailed, HostFailed,
};

struct CaptureAttempt {
    CaptureStatus status;
    std::unique_ptr<const CapturedEffects> snapshot;
};

// Main-thread only; share one instance across a host's capture entry points.
// The caller must hold the PMM/sidecar capture boundary and supply their exact
// identity and already resolved, distinct filesystem targets. This helper
// does no I/O or alias detection and is not connected to production saving.
class CaptureAdmission final {
public:
    CaptureAdmission() = default;
    CaptureAdmission(const CaptureAdmission&) = delete;
    CaptureAdmission& operator=(const CaptureAdmission&) = delete;

    CaptureAttempt Capture(const Request& expected, std::size_t budget,
        const CaptureCallbacks& callbacks) noexcept {
        using Status = CaptureStatus;
        if (busy_) return {Status::Busy, {}};
        busy_ = true;
        struct Release { bool& busy; ~Release() { busy = false; } } release{busy_};
        try {
            if (detail::RequestError(expected) || expected.pmmPath == expected.emmPath)
                return {Status::InvalidRequest, {}};
            if (!callbacks.freeze || !callbacks.observe) return {Status::Unsupported, {}};
            if (!callbacks.validate || !callbacks.sha256) return {Status::HostFailed, {}};
            if (budget == 0) return {Status::PayloadRejected, {}};
            std::unordered_set<std::uint64_t> ids;
            for (const auto id : expected.identity.objectIds)
                if (!id || !ids.insert(id).second) return {Status::InvalidRequest, {}};
            Request request = expected;
            const auto cancelled = [&] { return callbacks.cancelled && callbacks.cancelled(); };
            if (cancelled()) return {Status::Cancelled, {}};
            const auto before = callbacks.observe();
            if (cancelled()) return {Status::Cancelled, {}};
            if (!before) return {Status::Unsupported, {}};
            if (*before != request.identity) return {Status::Stale, {}};

            CaptureSink sink(budget < MaximumEffectBytes ? budget : MaximumEffectBytes);
            FreezeResult result;
            try { result = callbacks.freeze(request, sink); }
            catch (...) { return {Status::ProviderFailed, {}}; }
            if (cancelled() || result.status == FreezeStatus::Cancelled) return {Status::Cancelled, {}};
            if (result.status == FreezeStatus::Unsupported) return {Status::Unsupported, {}};
            if (result.status != FreezeStatus::Frozen) return {Status::ProviderFailed, {}};
            if (result.identity != request.identity) return {Status::Stale, {}};
            if (!sink.Complete()) return {Status::PayloadRejected, {}};
            const auto frozen = callbacks.observe();
            if (cancelled()) return {Status::Cancelled, {}};
            if (!frozen || *frozen != request.identity) return {Status::Stale, {}};
            const EffectBytes bytes{sink.bytes_.get(), sink.size_};
            if (!callbacks.validate(request, bytes)) return {Status::ValidationFailed, {}};
            EffectDigest digest{};
            if (!callbacks.sha256(bytes, digest)) return {Status::DigestFailed, {}};

            const auto after = callbacks.observe();
            if (cancelled()) return {Status::Cancelled, {}};
            if (!after || *after != request.identity) return {Status::Stale, {}};
            return {Status::Captured, std::unique_ptr<const CapturedEffects>(
                new CapturedEffects(std::move(request), std::move(sink.bytes_), sink.size_, digest))};
        } catch (...) {
            return {Status::HostFailed, {}};
        }
    }

private:
    bool busy_ = false;
};

} // namespace mikumikustudio::effect_save
