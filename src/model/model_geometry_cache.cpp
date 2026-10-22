#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include "model_geometry_cache.hpp"
#include "mikumikustudio/build_profile.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <io.h>
#include <limits>
#include <list>
#include <mutex>
#include <string>
#include <utility>

namespace mikumikustudio::model_geometry_cache {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { Reset(); }
    void Reset() noexcept {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};
struct Identity {
    std::wstring path;
    std::uint64_t size = 0, fileIndex = 0;
    DWORD volume = 0;
    LONGLONG created = 0, written = 0, changed = 0;
};
bool SameIdentity(const Identity& a, const Identity& b) noexcept {
    return a.path == b.path && a.size == b.size && a.fileIndex == b.fileIndex && a.volume == b.volume &&
        a.created == b.created && a.written == b.written && a.changed == b.changed;
}
bool Identify(HANDLE handle, Identity& output) {
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK) return false;
    BY_HANDLE_FILE_INFORMATION info{}; FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
        !basic.ChangeTime.QuadPart || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!size || size > 32768) return false;
    std::wstring path(size, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), size, FILE_NAME_NORMALIZED);
    if (!written || written >= size) return false;
    path.resize(written); output.path = std::move(path);
    output.size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    output.fileIndex = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    output.volume = info.dwVolumeSerialNumber;
    output.created = basic.CreationTime.QuadPart; output.written = basic.LastWriteTime.QuadPart;
    output.changed = basic.ChangeTime.QuadPart;
    return output.size <= std::uint64_t((std::numeric_limits<std::int64_t>::max)());
}
bool Current(HANDLE handle, const Identity& expected) {
    Identity current; return Identify(handle, current) && SameIdentity(current, expected);
}
bool LockStableFile(const Identity& expected, Handle& guard) {
    guard.value = CreateFileW(expected.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (guard.value == INVALID_HANDLE_VALUE) return false;
    if (Current(guard.value, expected)) return true;
    guard.Reset(); return false;
}
using Digest = std::array<unsigned char, 32>;
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    bool Open() noexcept {
        return BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    bool Add(unsigned char* bytes, ULONG count) noexcept { return BCryptHashData(hash, bytes, count, 0) >= 0; }
    bool Finish(Digest& result) noexcept { return BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0) >= 0; }
};
// Read through the independent guard handle, never the parser's descriptor or
// buffered cursor. On a miss the full-file sample also supplies the geometry
// digest from those same bytes. Existing mappings can bypass sharing locks:
// these samples detect observed changes, not malicious concurrent/ABA writes.
bool Fingerprint(HANDLE handle, std::uint64_t first, std::uint64_t last, Digest& result,
                 std::uint64_t sliceFirst = 0, std::uint64_t sliceLast = 0, Digest* slice = nullptr) noexcept {
    if (first > last || last > std::uint64_t((std::numeric_limits<std::int64_t>::max)()) ||
        (slice && (sliceFirst < first || sliceFirst > sliceLast || sliceLast > last))) return false;
    Hash whole, part;
    if (!whole.Open() || (slice && !part.Open())) return false;
    LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(first);
    if (!SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN)) return false;
    unsigned char buffer[64 * 1024];
    for (auto position = first; position < last;) {
        const DWORD want = static_cast<DWORD>((std::min)(std::uint64_t(sizeof(buffer)), last - position));
        DWORD got = 0;
        if (!ReadFile(handle, buffer, want, &got, nullptr) || !got || got > want || !whole.Add(buffer, got)) return false;
        if (slice) {
            const auto begin = (std::max)(position, sliceFirst), end = (std::min)(position + got, sliceLast);
            if (begin < end && !part.Add(buffer + static_cast<std::size_t>(begin - position), ULONG(end - begin))) return false;
        }
        position += got;
    }
    return whole.Finish(result) && (!slice || part.Finish(*slice));
}
bool SameLayout(const Layout& a, const Layout& b) noexcept {
    return a.format == b.format && a.vertexCount == b.vertexCount && a.additionalUv == b.additionalUv &&
        a.boneIndexSize == b.boneIndexSize && a.vertexIndexSize == b.vertexIndexSize;
}
bool Disabled() noexcept {
    wchar_t text[4]{};
    return GetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", text, 4) == 1 && text[0] == L'1';
}
bool IndexWidth(unsigned width) noexcept { return width == 1 || width == 2 || width == 4; }
template<class Vertex, class Copy>
void VisitPmxFields(Vertex& vertex, unsigned additionalUv, Copy copy) noexcept {
    copy(&vertex, 32);
    for (unsigned component = 0; component < 4 && additionalUv; ++component)
        copy(vertex.additionalUvByComponent[component], additionalUv * sizeof(float));
    copy(&vertex.weightType, 1);
    switch (vertex.weightType) {
    case mdl::PmxWeightType::bdef2: copy(vertex.bone, 8); copy(vertex.weight, 4); break;
    case mdl::PmxWeightType::bdef4: copy(vertex.bone, 16); copy(vertex.weight, 16); break;
    case mdl::PmxWeightType::sdef:
        copy(vertex.bone, 8); copy(vertex.weight, 4); copy(&vertex.sdef, sizeof(vertex.sdef)); break;
    default: copy(vertex.bone, 4); break; // The ABI parser's BDEF1/unknown/QDEF fallback.
    }
    copy(&vertex.edgeScale, 4); copy(&vertex.hasPmxEdgeData, 1);
}
bool AddSize(std::size_t& size, std::size_t addition) noexcept {
    if (addition > (std::numeric_limits<std::size_t>::max)() - size) return false;
    size += addition; return true;
}
struct Entry {
    Identity identity;
    Layout layout;
    mutable bool retired = false; // Cache mutex only; immutable payload remains leased.
    std::int64_t vertexStart = 0, vertexEnd = 0, indexEnd = 0;
    Digest geometry{};
    std::uint32_t indexCount = 0;
    std::size_t vertexBytes = 0, payloadBytes = 0;
    // One exact-size allocation avoids vector capacity exceeding a reservation.
    std::unique_ptr<unsigned char[]> payload;
    std::size_t Bytes() const noexcept {
        return sizeof(Entry) + 64 + identity.path.capacity() * sizeof(wchar_t) + payloadBytes;
    }
};
bool ValidRange(const Layout& layout, std::uint64_t fileBytes, std::int64_t first,
                std::int64_t verticesEnd, std::int64_t indicesEnd, std::uint32_t count,
                std::uint64_t vertexWireBytes) noexcept {
    const unsigned width = layout.format == Format::pmd ? 2 : layout.vertexIndexSize;
    return first >= 0 && verticesEnd >= first && indicesEnd >= verticesEnd &&
        std::uint64_t(indicesEnd) <= fileBytes && std::uint64_t(verticesEnd - first) == vertexWireBytes &&
        std::uint64_t(indicesEnd - verticesEnd) == 4 + std::uint64_t(count) * width;
}
}

struct Cache::Impl {
    Impl(std::size_t bytes, std::size_t entryBytes, std::size_t entries)
        : byteLimit(bytes), entryByteLimit((std::min)(bytes, entryBytes)), entryLimit(entries) {}
    mutable std::mutex mutex;
    std::list<std::shared_ptr<const Entry>> entries;
    Statistics statistics;
    const std::size_t byteLimit, entryByteLimit, entryLimit;
    std::uint64_t epoch = 1;
    void SweepRetired() noexcept {
        for (auto it = entries.begin(); it != entries.end();) {
            if ((*it)->retired && it->use_count() == 1) {
                statistics.bytes -= (*it)->Bytes(); it = entries.erase(it);
            } else ++it;
        }
    }
    bool EvictOne() noexcept {
        for (auto it = entries.end(); it != entries.begin();) {
            --it;
            if (it->use_count() == 1) {
                statistics.bytes -= (*it)->Bytes(); entries.erase(it); ++statistics.evictions; return true;
            }
        }
        return false;
    }
    void RetirePath(const std::wstring& path) noexcept {
        for (const auto& entry : entries) if (entry->identity.path == path && !entry->retired) {
            entry->retired = true; ++statistics.invalidations;
        }
        SweepRetired();
    }
};
struct Session::State {
    Cache::Impl* cache = nullptr;
    Identity identity;
    Layout layout;
    std::int64_t vertexStart = 0;
    std::uint64_t epoch = 0;
    Digest before{};
    Handle guard;
    std::shared_ptr<const Entry> hit;
    std::shared_ptr<Entry> pending;
    std::size_t reserved = 0;
    bool canceled = false, finished = false;
    void ReleaseReservation() noexcept {
        if (!reserved) return;
        try {
            std::lock_guard<std::mutex> lock(cache->mutex);
            cache->statistics.pendingBytes -= reserved; reserved = 0;
        } catch (...) {} // Retain a conservative charge if locking itself failed.
    }
    void CancelPending() noexcept { pending.reset(); ReleaseReservation(); }
    void FinishGeometry() noexcept {
        finished = true;
        guard.Reset();
        try { std::lock_guard<std::mutex> lock(cache->mutex); hit.reset(); cache->SweepRetired(); }
        catch (...) { hit.reset(); }
    }
    ~State() { FinishGeometry(); CancelPending(); }
    bool Reserve(std::size_t bytes) {
        if (bytes > cache->entryByteLimit) return false;
        std::lock_guard<std::mutex> lock(cache->mutex);
        if (epoch != cache->epoch || !epoch) return false;
        cache->SweepRetired();
        if (cache->statistics.pendingBytes > cache->byteLimit - bytes) return false;
        const auto available = cache->byteLimit - bytes - cache->statistics.pendingBytes;
        while (cache->statistics.bytes > available) if (!cache->EvictOne()) return false;
        cache->statistics.pendingBytes += bytes; reserved = bytes; return true;
    }
    bool GeometryCurrent() const {
        if (!hit || guard.value == INVALID_HANDLE_VALUE || !Current(guard.value, identity)) return false;
        Digest digest;
        return Fingerprint(guard.value, std::uint64_t(hit->vertexStart), std::uint64_t(hit->indexEnd), digest) &&
            digest == hit->geometry && Current(guard.value, identity);
    }
    std::shared_ptr<Entry> Prepare(std::size_t vertexBytes, std::uint32_t count,
                                    std::int64_t vertexEnd, std::int64_t indexEnd) {
        auto entry = std::make_shared<Entry>();
        entry->identity = identity; entry->layout = layout; entry->vertexStart = vertexStart;
        entry->vertexEnd = vertexEnd; entry->indexEnd = indexEnd; entry->indexCount = count;
        entry->vertexBytes = vertexBytes; entry->payloadBytes = vertexBytes;
        if (std::uint64_t(count) * 4 > (std::numeric_limits<std::size_t>::max)() ||
            !AddSize(entry->payloadBytes, std::size_t(count) * 4) ||
            entry->payloadBytes > cache->entryByteLimit || !Reserve(entry->Bytes())) return {};
        if (entry->payloadBytes) entry->payload.reset(new unsigned char[entry->payloadBytes]);
        return entry;
    }
    bool CaptureCurrent(Entry& entry) {
        if (!Current(guard.value, identity)) return false;
        Digest after;
        return Fingerprint(guard.value, 0, identity.size, after, std::uint64_t(vertexStart),
            std::uint64_t(entry.indexEnd), &entry.geometry) && after == before && Current(guard.value, identity);
    }
};

Cache::Cache(std::size_t bytes, std::size_t entryBytes, std::size_t entries)
    : impl_(std::make_unique<Impl>(bytes, entryBytes, entries)) {}
Cache::~Cache() = default;
Statistics Cache::GetStatistics() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->SweepRetired(); auto result = impl_->statistics; result.entries = impl_->entries.size(); return result;
    } catch (...) { return {}; }
}
void Cache::Clear() noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto& cache = *impl_;
        if (cache.epoch) ++cache.epoch; // Wrapped zero disables future admissions.
        const auto pending = cache.statistics.pendingBytes, bytes = cache.statistics.bytes;
        cache.statistics = {}; cache.statistics.pendingBytes = pending; cache.statistics.bytes = bytes;
        for (const auto& entry : cache.entries) entry->retired = true;
        cache.SweepRetired();
    } catch (...) {}
}
Session::Session() noexcept = default;
Session::~Session() = default;
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
namespace { Cache& ProcessCache() { static Cache cache; return cache; } }
Statistics GetProcessStatistics() noexcept {
    if (build_profile::original) return {};
    try { return ProcessCache().GetStatistics(); } catch (...) { return {}; }
}
Session Session::Begin(int descriptor, Layout layout, std::int64_t vertexStart, Cache* cache, bool disabled) noexcept {
    Session result;
    try {
        if (!cache) { if (build_profile::original) return result; cache = &ProcessCache(); }
        const bool pmx = layout.format == Format::pmx;
        const std::uint64_t minimum = pmx ? 42u + layout.additionalUv * 16u : sizeof(mdl::PmdVertex);
        if (disabled || Disabled() || descriptor < 0 || vertexStart < 0 ||
            (!pmx && layout.format != Format::pmd) ||
            (pmx && (layout.additionalUv > 4 || !IndexWidth(layout.boneIndexSize) || !IndexWidth(layout.vertexIndexSize))) ||
            std::uint64_t(layout.vertexCount) * minimum > cache->impl_->entryByteLimit || !cache->impl_->entryLimit) {
            std::lock_guard<std::mutex> lock(cache->impl_->mutex); ++cache->impl_->statistics.bypasses; return result;
        }
        auto state = std::make_unique<State>(); state->cache = cache->impl_.get();
        state->layout = layout; state->vertexStart = vertexStart;
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
        if (!Identify(handle, state->identity) || std::uint64_t(vertexStart) > state->identity.size ||
            // The original parser may ignore a large unknown tail. Do not
            // turn a small optional geometry cache into an unbounded scan.
            state->identity.size > (std::max)(std::uint64_t(cache->impl_->entryByteLimit),
                std::uint64_t(128) * 1024 * 1024) ||
            !LockStableFile(state->identity, state->guard)) {
            std::lock_guard<std::mutex> lock(state->cache->mutex); ++state->cache->statistics.bypasses; return result;
        }
        {
            std::lock_guard<std::mutex> lock(state->cache->mutex);
            auto& store = *state->cache; state->epoch = store.epoch;
            if (!state->epoch) { ++store.statistics.bypasses; return result; }
            for (const auto& entry : store.entries) {
                if (entry->identity.path != state->identity.path || entry->retired) continue;
                if (!SameIdentity(entry->identity, state->identity) || !SameLayout(entry->layout, layout) ||
                    entry->vertexStart != vertexStart) {
                    entry->retired = true; ++store.statistics.invalidations;
                } else state->hit = entry;
            }
            store.SweepRetired();
        }
        if (state->hit && !state->GeometryCurrent()) {
            std::lock_guard<std::mutex> lock(state->cache->mutex);
            state->hit->retired = true; ++state->cache->statistics.invalidations;
            state->hit.reset(); state->cache->SweepRetired();
        }
        if (!state->hit && (!Fingerprint(state->guard.value, 0, state->identity.size, state->before) ||
            !Current(state->guard.value, state->identity))) return result;
        {
            std::lock_guard<std::mutex> lock(state->cache->mutex);
            auto& store = *state->cache;
            if (state->epoch != store.epoch) return result;
            if (state->hit) {
                for (auto it = store.entries.begin(); it != store.entries.end(); ++it)
                    if (*it == state->hit) { store.entries.splice(store.entries.begin(), store.entries, it); break; }
                ++store.statistics.hits;
            } else ++store.statistics.misses;
        }
        result.state_ = std::move(state);
    } catch (...) {}
    return result;
}
bool Session::Enabled() const noexcept { return state_ != nullptr; }
bool Session::Hit() const noexcept { return state_ && state_->hit; }
std::int64_t Session::VertexEnd() const noexcept { return Hit() ? state_->hit->vertexEnd : -1; }
std::int64_t Session::IndexEnd() const noexcept { return Hit() ? state_->hit->indexEnd : -1; }
std::uint32_t Session::IndexCount() const noexcept { return Hit() ? state_->hit->indexCount : 0; }
bool Session::CopyPmxVertices(mdl::PmxVertex* destination) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmx || (!destination && state_->layout.vertexCount)) return false;
    try {
        if (!state_->GeometryCurrent()) return false;
        const auto* source = state_->hit->payload.get();
        for (std::uint32_t i = 0; i < state_->layout.vertexCount; ++i)
            VisitPmxFields(destination[i], state_->layout.additionalUv,
                [&source](void* field, std::size_t size) { std::memcpy(field, source, size); source += size; });
        return state_->GeometryCurrent();
    } catch (...) { return false; }
}
bool Session::CopyPmdVertices(mdl::PmdVertex* destination) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmd || (!destination && state_->layout.vertexCount)) return false;
    try {
        if (!state_->GeometryCurrent()) return false;
        if (state_->hit->vertexBytes) std::memcpy(destination, state_->hit->payload.get(), state_->hit->vertexBytes);
        return state_->GeometryCurrent();
    } catch (...) { return false; }
}
bool Session::CopyPmxIndices(std::int32_t* destination, std::uint32_t count) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmx || count != IndexCount() || (!destination && count)) return false;
    try {
        if (!state_->GeometryCurrent()) return false;
        if (count) std::memcpy(destination, state_->hit->payload.get() + state_->hit->vertexBytes, std::size_t(count) * 4);
        return state_->GeometryCurrent();
    } catch (...) { return false; }
}
bool Session::CopyPmdIndices(std::uint16_t* destination, std::uint32_t count) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmd || count != IndexCount() || (!destination && count)) return false;
    try {
        if (!state_->GeometryCurrent()) return false;
        if (count) std::memcpy(destination, state_->hit->payload.get() + state_->hit->vertexBytes, std::size_t(count) * 4);
        return state_->GeometryCurrent();
    } catch (...) { return false; }
}
void Session::CapturePmx(const mdl::PmxVertex* vertices, const std::int32_t* indices, std::uint32_t count,
                         std::int64_t vertexEnd, std::int64_t indexEnd) noexcept {
    if (!state_ || state_->pending || state_->reserved || state_->canceled || state_->finished) return;
    try {
        if (!Hit() && state_->layout.format == Format::pmx && (vertices || !state_->layout.vertexCount) && (indices || !count)) {
            std::size_t vertexBytes = 0; std::uint64_t wireBytes = 0; bool fits = true;
            for (std::uint32_t i = 0; i < state_->layout.vertexCount; ++i) {
                VisitPmxFields(vertices[i], state_->layout.additionalUv,
                    [&](const void*, std::size_t size) { fits = AddSize(vertexBytes, size) && fits; });
                const auto type = vertices[i].weightType;
                const unsigned bones = type == mdl::PmxWeightType::bdef4 ? 4 :
                    type == mdl::PmxWeightType::bdef2 || type == mdl::PmxWeightType::sdef ? 2 : 1;
                wireBytes += 37u + state_->layout.additionalUv * 16u + bones * state_->layout.boneIndexSize +
                    (bones == 4 ? 16u : bones == 2 ? 4u : 0u) + (type == mdl::PmxWeightType::sdef ? 36u : 0u);
            }
            if (fits && ValidRange(state_->layout, state_->identity.size, state_->vertexStart, vertexEnd, indexEnd, count, wireBytes)) {
                auto entry = state_->Prepare(vertexBytes, count, vertexEnd, indexEnd);
                if (entry) {
                    auto* output = entry->payload.get();
                    for (std::uint32_t i = 0; i < state_->layout.vertexCount; ++i)
                        VisitPmxFields(vertices[i], state_->layout.additionalUv,
                            [&output](const void* field, std::size_t size) { std::memcpy(output, field, size); output += size; });
                    if (count) std::memcpy(output, indices, std::size_t(count) * 4);
                    if (state_->CaptureCurrent(*entry)) state_->pending = std::move(entry);
                }
            }
        }
    } catch (...) {}
    if (!state_->pending) state_->ReleaseReservation();
    FinishGeometry();
}
void Session::CapturePmd(const mdl::PmdVertex* vertices, const std::uint16_t* indices, std::uint32_t count,
                         std::int64_t vertexEnd, std::int64_t indexEnd) noexcept {
    if (!state_ || state_->pending || state_->reserved || state_->canceled || state_->finished) return;
    try {
        if (!Hit() && state_->layout.format == Format::pmd && (vertices || !state_->layout.vertexCount) && (indices || !count) &&
            ValidRange(state_->layout, state_->identity.size, state_->vertexStart, vertexEnd, indexEnd, count,
                std::uint64_t(state_->layout.vertexCount) * 38)) {
            auto entry = state_->Prepare(std::size_t(state_->layout.vertexCount) * sizeof(mdl::PmdVertex), count, vertexEnd, indexEnd);
            if (entry) {
                if (entry->vertexBytes) std::memcpy(entry->payload.get(), vertices, entry->vertexBytes);
                if (count) {
                    auto* output = entry->payload.get() + entry->vertexBytes;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const std::uint32_t value = indices[std::size_t(i) * 2];
                        std::memcpy(output + std::size_t(i) * 4, &value, 4);
                    }
                }
                if (state_->CaptureCurrent(*entry)) state_->pending = std::move(entry);
            }
        }
    } catch (...) {}
    if (!state_->pending) state_->ReleaseReservation();
    FinishGeometry();
}
void Session::FinishGeometry() noexcept { if (state_) state_->FinishGeometry(); }
void Session::Commit() noexcept {
    if (!state_ || !state_->pending) return;
    try {
        Handle guard; Digest digest;
        if (Disabled() || state_->canceled || !LockStableFile(state_->identity, guard) ||
            !Fingerprint(guard.value, std::uint64_t(state_->vertexStart), std::uint64_t(state_->pending->indexEnd), digest) ||
            digest != state_->pending->geometry || !Current(guard.value, state_->identity)) {
            state_->CancelPending(); return;
        }
        // Allocate the admission node before touching the live LRU/accounting.
        std::list<std::shared_ptr<const Entry>> admission; admission.push_front(state_->pending);
        {
            auto& cache = *state_->cache; std::lock_guard<std::mutex> lock(cache.mutex);
            if (state_->epoch == cache.epoch && state_->epoch) {
                cache.RetirePath(state_->identity.path);
                while (cache.entries.size() >= cache.entryLimit && cache.EvictOne()) {}
                if (cache.entries.size() < cache.entryLimit) {
                    cache.entries.splice(cache.entries.begin(), admission, admission.begin());
                    cache.statistics.bytes += state_->pending->Bytes();
                    cache.statistics.pendingBytes -= state_->reserved; state_->reserved = 0;
                    ++cache.statistics.stores;
                }
            }
        }
    } catch (...) {}
    state_->CancelPending();
}
void Session::Invalidate() noexcept {
    if (!state_) return;
    state_->canceled = true;
    try { std::lock_guard<std::mutex> lock(state_->cache->mutex); state_->hit.reset(); state_->cache->RetirePath(state_->identity.path); }
    catch (...) { state_->hit.reset(); }
    state_->guard.Reset(); state_->CancelPending();
}
}
