#pragma once
// C++17 ownership and read-only views over the ORIGINAL mms.ui.catalog table.
// Query and all service/release calls belong on the host UI thread. Immutable
// value()/span views may be read on workers while their lease remains alive.
// Contexts must outlive the wrappers; return every owning lease to the UI
// thread before destruction. Destructors can only attempt best-effort release.
#include <mms/cpp/ui.hpp>
#include <mms/ui_catalog.h>
#include <limits>

namespace mms::sdk {
namespace detail {
inline bool CatalogString(MmsString value) noexcept {
    if (!value.bytes) return true;
    if (!value.data || value.bytes > (std::numeric_limits<std::size_t>::max)()) return false;
    return value.bytes <= (std::numeric_limits<std::uintptr_t>::max)()
        - reinterpret_cast<std::uintptr_t>(value.data);
}
template<class T> bool CatalogSpan(MmsSpan span) noexcept {
    if (!span.count) return true;
    if (!span.data || span.stride < sizeof(T) || span.stride % alignof(T) || span.recordVersion != 1 ||
        reinterpret_cast<std::uintptr_t>(span.data) % alignof(T)) return false;
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    if (span.count - 1 > (maximum - sizeof(T)) / span.stride) return false;
    const auto bytes = static_cast<std::size_t>(span.count - 1) * span.stride + sizeof(T);
    return bytes <= (std::numeric_limits<std::uintptr_t>::max)()
        - reinterpret_cast<std::uintptr_t>(span.data);
}
template<class T> const T* CatalogAt(MmsSpan span, std::uint64_t index) noexcept {
    if (index >= span.count) return nullptr;
    return reinterpret_cast<const T*>(static_cast<const unsigned char*>(span.data)
        + static_cast<std::size_t>(index) * span.stride);
}
inline bool CatalogEntry(const MmsUiCatalogEntryV1& entry, std::uint64_t epoch) noexcept {
    return TableHeader(entry) && entry.element && entry.id.bytes && entry.ownerPlugin.bytes &&
        CatalogString(entry.id) && CatalogString(entry.ownerPlugin) && CatalogString(entry.parentId) &&
        CatalogString(entry.label) && CatalogString(entry.commandId) && CatalogString(entry.propertyId) &&
        TableHeader(entry.target) && entry.target.element == entry.element && epoch &&
        entry.target.projectEpoch == epoch && !entry.reserved &&
        bool(entry.knownFields & MMS_UI_CATALOG_PRESENTATION) == bool(entry.target.instanceGeneration);
}
inline bool CatalogSnapshotValue(const MmsUiCatalogSnapshotV1& value, std::uint32_t scopes) noexcept {
    if (!TableHeader(value) || !value.handle || !value.projectEpoch || !scopes ||
        value.requestedScopes != scopes || value.reserved ||
        !CatalogSpan<MmsUiCatalogEntryV1>(value.elements) ||
        !CatalogSpan<MmsUiCatalogRegistrationV1>(value.registrations)) return false;
    for (std::uint64_t i = 0; i < value.elements.count; ++i)
        if (!CatalogEntry(*CatalogAt<MmsUiCatalogEntryV1>(value.elements, i), value.projectEpoch)) return false;
    for (std::uint64_t i = 0; i < value.registrations.count; ++i) {
        const auto& registration = *CatalogAt<MmsUiCatalogRegistrationV1>(value.registrations, i);
        if (!TableHeader(registration) || !registration.handle || !registration.id.bytes ||
            !registration.ownerPlugin.bytes || !CatalogString(registration.id) ||
            !CatalogString(registration.ownerPlugin) || !CatalogString(registration.label)) return false;
    }
    return true;
}
inline MmsResult CatalogBadSuccess(const MmsUiCatalogApiV1& api, MmsHandle handle) noexcept {
    // A host returning both a malformed successful lease and a failed release
    // has violated the C contract. Attempt the original release once and
    // report its failure; no hidden registration or second lease store exists.
    if (handle) {
        const auto result = Call([&] { return api.releaseSnapshot(api.context, handle); });
        if (result) return result;
    }
    return MMS_E_FAILED;
}
} // namespace detail

class CatalogServices;
class CatalogSnapshot {
    MmsUiCatalogApiV1 api_{};
    MmsUiCatalogSnapshotV1 value_{};
    std::thread::id thread_{};
    friend class CatalogServices;
public:
    CatalogSnapshot() noexcept = default;
    CatalogSnapshot(const CatalogSnapshot&) = delete;
    CatalogSnapshot& operator=(const CatalogSnapshot&) = delete;
    CatalogSnapshot(CatalogSnapshot&& other) noexcept
        : api_(other.api_), value_(std::exchange(other.value_, MmsUiCatalogSnapshotV1{})), thread_(other.thread_) {
        other.api_ = {}; other.thread_ = {};
    }
    // Assignment cannot silently discard a destination whose Release failed.
    CatalogSnapshot& operator=(CatalogSnapshot&&) = delete;
    ~CatalogSnapshot() { (void)Release(); }
    explicit operator bool() const noexcept { return value_.handle != 0; }
    MmsHandle get() const noexcept { return value_.handle; }
    const MmsUiCatalogSnapshotV1& value() const noexcept { return value_; }
    const MmsSpan& Elements() const noexcept { return value_.elements; }
    const MmsSpan& Registrations() const noexcept { return value_.registrations; }
    const MmsUiCatalogEntryV1* Element(std::uint64_t index) const noexcept {
        return detail::CatalogAt<MmsUiCatalogEntryV1>(value_.elements, index);
    }
    const MmsUiCatalogRegistrationV1* Registration(std::uint64_t index) const noexcept {
        return detail::CatalogAt<MmsUiCatalogRegistrationV1>(value_.registrations, index);
    }
    // Call explicitly during stop and retain this object on ANY failure,
    // including NOT_FOUND. Empty Release is a no-op on every thread.
    MmsResult Release() noexcept {
        if (!value_.handle) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.releaseSnapshot(api_.context, value_.handle); });
        if (result) return result;
        value_ = {}; api_ = {}; thread_ = {}; return MMS_OK;
    }
};

class CatalogEntryLease {
    MmsUiCatalogApiV1 api_{};
    MmsUiCatalogEntryV1 value_{};
    MmsHandle handle_ = 0;
    std::thread::id thread_{};
    friend class CatalogServices;
public:
    CatalogEntryLease() noexcept = default;
    CatalogEntryLease(const CatalogEntryLease&) = delete;
    CatalogEntryLease& operator=(const CatalogEntryLease&) = delete;
    CatalogEntryLease(CatalogEntryLease&& other) noexcept
        : api_(other.api_), value_(other.value_), handle_(std::exchange(other.handle_, 0)), thread_(other.thread_) {
        other.api_ = {}; other.value_ = {}; other.thread_ = {};
    }
    CatalogEntryLease& operator=(CatalogEntryLease&&) = delete;
    ~CatalogEntryLease() { (void)Release(); }
    explicit operator bool() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    const MmsUiCatalogEntryV1& value() const noexcept { return value_; }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (thread_ != std::this_thread::get_id()) return MMS_E_THREAD;
        const auto result = detail::Call([&] { return api_.releaseSnapshot(api_.context, handle_); });
        if (result) return result;
        handle_ = 0; value_ = {}; api_ = {}; thread_ = {}; return MMS_OK;
    }
};

class CatalogServices {
    MmsUiCatalogApiV1 api_{};
    std::thread::id thread_{};
    bool ready_ = false;
public:
    // Call on the UI thread. A failed negotiation leaves out unchanged. This
    // stores the negotiated C table; it creates no registry or registrations.
    static MmsResult Query(const MmsHostV1& host, CatalogServices& out) noexcept {
        if (host.h.size < sizeof(host) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return detail::Call([&]() -> MmsResult {
            MmsUiCatalogApiV1 table{};
            const auto result = host.queryService(host.context, Text("mms.ui.catalog"), 1, 0,
                static_cast<std::uint32_t>(sizeof(table)), &table);
            if (result) return result;
            if (!detail::TableHeader(table)) return MMS_E_VERSION;
            if (!table.getCapabilities || !table.acquireSnapshot || !table.resolveElement || !table.releaseSnapshot)
                return MMS_E_UNSUPPORTED;
            CatalogServices candidate; candidate.api_ = table;
            candidate.thread_ = std::this_thread::get_id(); candidate.ready_ = true;
            out = candidate; return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    MmsResult CheckThread() const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        return thread_ == std::this_thread::get_id() ? MMS_OK : MMS_E_THREAD;
    }
    // Borrowed original table for advanced callers; the C rules still apply.
    const MmsUiCatalogApiV1& api() const noexcept { return api_; }
    MmsResult Capabilities(MmsUiCatalogCapabilitiesV1& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        MmsUiCatalogCapabilitiesV1 candidate{}; candidate.h = Header<MmsUiCatalogCapabilitiesV1>();
        const auto result = detail::Call([&] { return api_.getCapabilities(api_.context, &candidate); });
        if (result) return result;
        if (!detail::TableHeader(candidate) || candidate.reserved || !candidate.supportedScopes ||
            !candidate.maxLeasesPerPlugin || !candidate.maxLeasesGlobal || !candidate.maxSnapshotBytes ||
            !candidate.maxRetainedBytesPerPlugin || !candidate.maxRetainedBytesGlobal || !candidate.maxEntries)
            return MMS_E_FAILED;
        out = candidate; return MMS_OK;
    }
    MmsResult Acquire(std::uint32_t scopes, CatalogSnapshot& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        MmsUiCatalogSnapshotV1 candidate{}; candidate.h = Header<MmsUiCatalogSnapshotV1>();
        const auto result = detail::Call([&] { return api_.acquireSnapshot(api_.context, scopes, &candidate); });
        if (result) return result;
        if (!detail::CatalogSnapshotValue(candidate, scopes))
            return detail::CatalogBadSuccess(api_, candidate.handle);
        out.api_ = api_; out.value_ = candidate; out.thread_ = thread_; return MMS_OK;
    }
    MmsResult Resolve(const MmsUiTargetRefV1& expected, CatalogEntryLease& out) const noexcept {
        if (const auto result = CheckThread()) return result;
        if (out) return MMS_E_BUSY;
        MmsUiCatalogEntryV1 candidate{}; candidate.h = Header<MmsUiCatalogEntryV1>(); MmsHandle lease = 0;
        const auto result = detail::Call([&] { return api_.resolveElement(api_.context, &expected, &candidate, &lease); });
        if (result) return result;
        if (!lease || !detail::CatalogEntry(candidate, expected.projectEpoch) ||
            candidate.element != expected.element || candidate.target.instanceGeneration != expected.instanceGeneration)
            return detail::CatalogBadSuccess(api_, lease);
        out.api_ = api_; out.value_ = candidate; out.handle_ = lease; out.thread_ = thread_; return MMS_OK;
    }
};
} // namespace mms::sdk
