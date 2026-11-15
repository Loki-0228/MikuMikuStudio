#pragma once

// C++17 convenience wrapper for the optional mms.settings 1.0 service.
// Only the original service table is retained. Use it on the host UI thread
// within the stages allowed by the C API, while its plugin context is alive.
#include <mms/settings.h>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace mms::sdk {
namespace settings_detail {
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
} // namespace settings_detail

class SettingsServices {
    MmsSettingsApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;

    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
public:
    // Negotiates the optional service; every failure leaves out unchanged.
    static MmsResult Query(const MmsHostV1& host, SettingsServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return settings_detail::Call([&]() -> MmsResult {
            MmsSettingsApiV1 api{};
            const auto result = host.queryService(host.context, {"mms.settings", 12}, 1, 0,
                static_cast<std::uint32_t>(sizeof(api)), &api);
            if (result != MMS_OK) return result;
            if (api.h.size < sizeof(api) || api.h.major != 1) return MMS_E_VERSION;
            if (!api.read || !api.compareAndMerge) return MMS_E_UNSUPPORTED;
            SettingsServices candidate;
            candidate.api_ = api;
            candidate.thread_ = std::this_thread::get_id();
            candidate.ready_ = true;
            out = candidate;
            return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsSettingsApiV1& api() const noexcept { return api_; }

    // Returns an owned byte-for-byte snapshot, or MMS_E_NOT_FOUND for an
    // absent object. No failure changes out; "{}" is a successful value.
    MmsResult Read(std::string& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        const auto api = api_; // Keep the same context for measurement and copy.
        return settings_detail::Call([&]() -> MmsResult {
            std::uint64_t required = 0;
            const auto measured = api.read(api.context, nullptr, 0, &required);
            if (measured != MMS_OK) return measured;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                if (required > MMS_SETTINGS_MAX_JSON_BYTES) return MMS_E_MEMORY;
                if (required < 2) return MMS_E_FAILED; // At least "{}".
                std::string candidate(static_cast<std::size_t>(required), '\0');
                std::uint64_t copied = 0;
                const auto result = api.read(api.context, candidate.data(), required, &copied);
                if (result == MMS_E_MEMORY && copied > required) {
                    required = copied;
                    continue; // Only one additional copy, even if it grows again.
                }
                if (result != MMS_OK) return result;
                if (copied < 2 || copied > required) return MMS_E_FAILED;
                candidate.resize(static_cast<std::size_t>(copied));
                out.swap(candidate);
                return MMS_OK;
            }
            return MMS_E_MEMORY;
        });
    }

    // nullopt means require absence; an engaged baseline is sent verbatim.
    // The caller owns both inputs during the call. The host validates JSON
    // and callback phase; conflicts are returned without read/merge retries.
    MmsResult CompareAndMerge(std::optional<std::string_view> expected, std::string_view patch) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (patch.empty() || patch.size() > MMS_SETTINGS_MAX_JSON_BYTES ||
            (expected && (expected->empty() || expected->size() > MMS_SETTINGS_MAX_JSON_BYTES))) {
            return MMS_E_ARGUMENT;
        }
        const MmsString baseline = expected
            ? MmsString{expected->data(), static_cast<std::uint64_t>(expected->size())} : MmsString{};
        const auto api = api_;
        return settings_detail::Call([&] {
            return api.compareAndMerge(api.context, baseline, {patch.data(), static_cast<std::uint64_t>(patch.size())});
        });
    }
};
} // namespace mms::sdk
