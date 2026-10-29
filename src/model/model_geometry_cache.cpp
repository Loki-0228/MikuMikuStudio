#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "model_geometry_cache.hpp"
#include <algorithm>
#include <cstring>
#include <io.h>
#include <list>
#include <mutex>
#include <string>
#include <vector>

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
bool SameIdentity(const Identity& a, const Identity& b) {
    return a.path == b.path && a.size == b.size && a.fileIndex == b.fileIndex
        && a.volume == b.volume && a.created == b.created
        && a.written == b.written && a.changed == b.changed;
}
bool Identify(HANDLE handle, Identity& output) {
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(handle, &info)
        || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || !basic.ChangeTime.QuadPart || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0,
                                                     FILE_NAME_NORMALIZED);
    if (!required || required > 32768) return false;
    std::wstring path(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), required,
                                                   FILE_NAME_NORMALIZED);
    if (!written || written >= required) return false;
    path.resize(written);
    output.path = std::move(path);
    output.size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    output.fileIndex = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    output.volume = info.dwVolumeSerialNumber;
    output.created = basic.CreationTime.QuadPart;
    output.written = basic.LastWriteTime.QuadPart;
    output.changed = basic.ChangeTime.QuadPart;
    return true;
}
bool LockStableFile(const Identity& expected, Handle& guard) {
    // Do not wait for writers or change the original descriptor's sharing.
    // Existing writers, delete/replace races, and unsupported files bypass.
    guard.value = CreateFileW(expected.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (guard.value == INVALID_HANDLE_VALUE) return false;
    Identity current;
    if (!Identify(guard.value, current) || !SameIdentity(expected, current)) {
        guard.Reset();
        return false;
    }
    return true;
}
bool SameLayout(const Layout& a, const Layout& b) noexcept {
    return a.format == b.format && a.vertexCount == b.vertexCount
        && a.additionalUv == b.additionalUv && a.boneIndexSize == b.boneIndexSize
        && a.vertexIndexSize == b.vertexIndexSize;
}
bool Disabled() noexcept {
    wchar_t text[4]{};
    return GetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", text, 4) == 1
        && text[0] == L'1';
}
// Snapshot only parsed fields. Bone indices are already signed 32-bit,
// additional UVs are transposed, and SDEF offsets are already expanded.
// This is a compact CPU geometry representation, not original file bytes.
// Unused fields and mutable material/morph bookkeeping never enter the cache.
template<class Vertex, class Copy>
void VisitPmxFields(Vertex& vertex, std::uint8_t additionalUv, Copy copy) noexcept {
    copy(&vertex, 32);
    for (unsigned component = 0; component < 4 && additionalUv; ++component)
        copy(vertex.additionalUvByComponent[component], additionalUv * sizeof(float));
    copy(&vertex.weightType, 1);
    switch (vertex.weightType) {
    case mdl::PmxWeightType::bdef2:
        copy(vertex.bone, 8); copy(vertex.weight, 4);
        break;
    case mdl::PmxWeightType::bdef4:
        copy(vertex.bone, 16); copy(vertex.weight, 16);
        break;
    case mdl::PmxWeightType::sdef:
        copy(vertex.bone, 8); copy(vertex.weight, 4);
        copy(&vertex.sdef, sizeof(vertex.sdef));
        break;
    default:
        copy(vertex.bone, 4);
        break;
    }
    copy(&vertex.edgeScale, 4);
    copy(&vertex.hasPmxEdgeData, 1);
}
std::size_t PmxBytes(const mdl::PmxVertex* vertices, std::uint32_t count,
                     std::uint8_t additionalUv) noexcept {
    std::size_t bytes = 0;
    for (std::uint32_t i = 0; i < count; ++i)
        VisitPmxFields(vertices[i], additionalUv,
                       [&bytes](const void*, std::size_t size) { bytes += size; });
    return bytes;
}
struct Entry {
    Identity identity;
    Layout layout;
    mutable bool retired = false;  // Protected by Cache::Impl::mutex.
    std::int64_t vertexStart = 0, vertexEnd = 0, indexEnd = 0;
    std::vector<unsigned char> pmx;
    std::vector<mdl::PmdVertex> pmd;
    std::vector<std::int32_t> indices;
    std::size_t Bytes() const noexcept {
        return sizeof(Entry) + 64 + identity.path.capacity() * sizeof(wchar_t)
            + pmx.capacity()
            + pmd.capacity() * sizeof(mdl::PmdVertex)
            + indices.capacity() * sizeof(std::int32_t);
    }
};
}  // namespace

struct Cache::Impl {
    Impl(std::size_t bytes, std::size_t entryBytes, std::size_t entries)
        : byteLimit(bytes), entryByteLimit(std::min(bytes, entryBytes)), entryLimit(entries) {}
    mutable std::mutex mutex;
    std::list<std::shared_ptr<const Entry>> entries;
    Statistics statistics;
    std::size_t byteLimit, entryByteLimit, entryLimit;
};
struct Session::State {
    Cache::Impl* cache = nullptr;
    Identity identity;
    Layout layout;
    std::int64_t vertexStart = 0;
    Handle guard;
    std::shared_ptr<const Entry> hit;
    std::shared_ptr<Entry> pending;
    std::size_t reserved = 0;
    void ReleaseReservation() noexcept {
        if (!reserved) return;
        std::lock_guard<std::mutex> lock(cache->mutex);
        cache->statistics.pendingBytes -= reserved;
        reserved = 0;
    }
    ~State() { ReleaseReservation(); }
    bool Reserve(std::size_t bytes) {
        if (bytes > cache->entryByteLimit) return false;
        std::lock_guard<std::mutex> lock(cache->mutex);
        while (cache->statistics.bytes + cache->statistics.pendingBytes
                   > cache->byteLimit - bytes) {
            auto removable = cache->entries.end();
            for (auto it = cache->entries.end(); it != cache->entries.begin();) {
                --it;
                if (it->use_count() == 1) { removable = it; break; }
            }
            if (removable == cache->entries.end()) return false;
            cache->statistics.bytes -= (*removable)->Bytes();
            cache->entries.erase(removable);
            ++cache->statistics.evictions;
        }
        cache->statistics.pendingBytes += bytes;
        reserved = bytes;
        return true;
    }
};

Cache::Cache(std::size_t bytes, std::size_t entryBytes, std::size_t entries)
    : impl_(std::make_unique<Impl>(bytes, entryBytes, entries)) {}
Cache::~Cache() = default;
Statistics Cache::GetStatistics() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = impl_->statistics;
    result.entries = impl_->entries.size();
    return result;
}
void Cache::Clear() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto pending = impl_->statistics.pendingBytes;
    impl_->statistics = {};
    impl_->statistics.pendingBytes = pending;
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        (*it)->retired = true;
        if (it->use_count() == 1) it = impl_->entries.erase(it);
        else {
            impl_->statistics.bytes += (*it)->Bytes();
            ++it;
        }
    }
}
Session::Session() noexcept = default;
Session::~Session() = default;
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

namespace {
Cache& ProcessCache() { static Cache cache; return cache; }
}
Statistics GetProcessStatistics() noexcept { return ProcessCache().GetStatistics(); }

Session Session::Begin(int descriptor, Layout layout, std::int64_t vertexStart,
                       Cache* cache, bool disabled) noexcept {
    Session result;
    try {
        if (!cache) {
            cache = &ProcessCache();
        }
        const std::size_t vertexStride =
            layout.format == Format::pmx ? 42 : sizeof(mdl::PmdVertex);
        if (disabled || Disabled() || descriptor < 0 || vertexStart < 0
            || (layout.format == Format::pmx && layout.additionalUv > 4)
            || std::uint64_t(layout.vertexCount) * vertexStride > cache->impl_->entryByteLimit
            || !cache->impl_->entryLimit) {
            std::lock_guard<std::mutex> lock(cache->impl_->mutex);
            ++cache->impl_->statistics.bypasses;
            return result;
        }
        auto state = std::make_unique<State>();
        state->cache = cache->impl_.get();
        state->layout = layout;
        state->vertexStart = vertexStart;
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
        if (!Identify(handle, state->identity)
            || static_cast<std::uint64_t>(vertexStart) > state->identity.size
            || !LockStableFile(state->identity, state->guard)) {
            std::lock_guard<std::mutex> lock(cache->impl_->mutex);
            ++cache->impl_->statistics.bypasses;
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(cache->impl_->mutex);
            auto& store = *cache->impl_;
            for (auto it = store.entries.begin(); it != store.entries.end();) {
                const auto& entry = **it;
                if (entry.identity.path != state->identity.path) { ++it; continue; }
                if (entry.retired || !SameIdentity(entry.identity, state->identity)
                    || !SameLayout(entry.layout, layout)
                    || entry.vertexStart != vertexStart) {
                    if (!entry.retired) ++store.statistics.invalidations;
                    entry.retired = true;
                    if (it->use_count() == 1) {
                        store.statistics.bytes -= entry.Bytes();
                        it = store.entries.erase(it);
                    } else ++it;
                    continue;
                }
                state->hit = *it;
                store.entries.splice(store.entries.begin(), store.entries, it);
                break;
            }
            if (state->hit) ++store.statistics.hits;
            else ++store.statistics.misses;
        }
        result.state_ = std::move(state);
    } catch (...) {
        // Cache allocation/OS failures never replace the normal loader.
    }
    return result;
}
bool Session::Enabled() const noexcept { return state_ != nullptr; }
bool Session::Hit() const noexcept { return state_ && state_->hit; }
std::int64_t Session::VertexEnd() const noexcept { return Hit() ? state_->hit->vertexEnd : -1; }
std::int64_t Session::IndexEnd() const noexcept { return Hit() ? state_->hit->indexEnd : -1; }
std::uint32_t Session::IndexCount() const noexcept {
    return Hit() ? static_cast<std::uint32_t>(state_->hit->indices.size()) : 0;
}
bool Session::CopyPmxVertices(mdl::PmxVertex* destination) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmx
        || (!destination && state_->layout.vertexCount)) return false;
    const auto* source = state_->hit->pmx.data();
    for (std::uint32_t i = 0; i < state_->layout.vertexCount; ++i)
        VisitPmxFields(destination[i], state_->layout.additionalUv,
            [&source](void* field, std::size_t size) {
                std::memcpy(field, source, size); source += size;
            });
    return true;
}
bool Session::CopyPmdVertices(mdl::PmdVertex* destination) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmd
        || (!destination && state_->layout.vertexCount)) return false;
    if (!state_->hit->pmd.empty())
        std::memcpy(destination, state_->hit->pmd.data(),
                    state_->hit->pmd.size() * sizeof(mdl::PmdVertex));
    return true;
}
bool Session::CopyPmxIndices(std::int32_t* destination, std::uint32_t count) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmx || count != IndexCount()
        || (!destination && count)) return false;
    if (count) std::memcpy(destination, state_->hit->indices.data(), count * sizeof(*destination));
    return true;
}
bool Session::CopyPmdIndices(std::uint16_t* destination, std::uint32_t count) const noexcept {
    if (!Hit() || state_->layout.format != Format::pmd || count != IndexCount()
        || (!destination && count)) return false;
    // The PMD host array has a four-byte stride: the upper uint16 stays zero.
    if (count) std::memcpy(destination, state_->hit->indices.data(), count * sizeof(std::int32_t));
    return true;
}

namespace {
bool ValidRange(const Layout& layout, std::uint64_t fileBytes, std::int64_t start,
                std::int64_t verticesEnd, std::int64_t indicesEnd,
                std::uint32_t count) noexcept {
    if (start < 0 || verticesEnd < start || indicesEnd < verticesEnd
        || static_cast<std::uint64_t>(indicesEnd) > fileBytes)
        return false;
    const std::uint64_t indexWidth = layout.format == Format::pmd ? 2
        : (layout.vertexIndexSize == 1 || layout.vertexIndexSize == 2
               ? layout.vertexIndexSize : 4);
    if (static_cast<std::uint64_t>(indicesEnd - verticesEnd) != 4 + count * indexWidth)
        return false;
    if (layout.format == Format::pmd)
        return static_cast<std::uint64_t>(verticesEnd - start)
            == std::uint64_t(layout.vertexCount) * 38;
    return static_cast<std::uint64_t>(verticesEnd - start)
        >= std::uint64_t(layout.vertexCount) * (32 + layout.additionalUv * 16 + 1 + 1 + 4);
}
}  // namespace

void Session::CapturePmx(const mdl::PmxVertex* vertices, const std::int32_t* indices,
                         std::uint32_t count, std::int64_t verticesEnd,
                         std::int64_t indicesEnd) noexcept {
    if (!state_ || state_->reserved || state_->pending) return;
    try {
        if (Hit() || state_->layout.format != Format::pmx
            || (!vertices && state_->layout.vertexCount) || (!indices && count)
            || !ValidRange(state_->layout, state_->identity.size, state_->vertexStart,
                           verticesEnd, indicesEnd, count)) {
            FinishGeometry();
            return;
        }
        const auto vertexBytes = PmxBytes(vertices, state_->layout.vertexCount,
                                           state_->layout.additionalUv);
        const std::size_t reserve = sizeof(Entry) + 64
            + state_->identity.path.capacity() * sizeof(wchar_t)
            + vertexBytes + std::size_t(count) * sizeof(std::int32_t);
        if (!state_->Reserve(reserve)) { FinishGeometry(); return; }
        auto entry = std::make_shared<Entry>();
        entry->identity = state_->identity;
        entry->layout = state_->layout;
        entry->vertexStart = state_->vertexStart;
        entry->vertexEnd = verticesEnd;
        entry->indexEnd = indicesEnd;
        entry->pmx.resize(vertexBytes);
        auto* output = entry->pmx.data();
        for (std::uint32_t i = 0; i < state_->layout.vertexCount; ++i)
            VisitPmxFields(vertices[i], state_->layout.additionalUv,
                [&output](const void* field, std::size_t size) {
                    std::memcpy(output, field, size); output += size;
                });
        if (count) entry->indices.assign(indices, indices + count);
        if (entry->Bytes() <= state_->reserved)
            state_->pending = std::move(entry);
    } catch (...) {}
    if (!state_->pending) state_->ReleaseReservation();
    FinishGeometry();
}
void Session::CapturePmd(const mdl::PmdVertex* vertices, const std::uint16_t* indices,
                         std::uint32_t count, std::int64_t verticesEnd,
                         std::int64_t indicesEnd) noexcept {
    if (!state_ || state_->reserved || state_->pending) return;
    try {
        if (Hit() || state_->layout.format != Format::pmd
            || (!vertices && state_->layout.vertexCount) || (!indices && count)
            || !ValidRange(state_->layout, state_->identity.size, state_->vertexStart,
                           verticesEnd, indicesEnd, count)
            || std::uint64_t(state_->layout.vertexCount) * sizeof(mdl::PmdVertex)
                   + std::uint64_t(count) * 4 + sizeof(Entry)
                       + state_->identity.path.size() * 2 > state_->cache->entryByteLimit) {
            FinishGeometry();
            return;
        }
        const std::size_t reserve = sizeof(Entry) + 64
            + state_->identity.path.capacity() * sizeof(wchar_t)
            + std::size_t(state_->layout.vertexCount) * sizeof(mdl::PmdVertex)
            + std::size_t(count) * sizeof(std::int32_t);
        if (!state_->Reserve(reserve)) { FinishGeometry(); return; }
        auto entry = std::make_shared<Entry>();
        entry->identity = state_->identity;
        entry->layout = state_->layout;
        entry->vertexStart = state_->vertexStart;
        entry->vertexEnd = verticesEnd;
        entry->indexEnd = indicesEnd;
        if (state_->layout.vertexCount)
            entry->pmd.assign(vertices, vertices + state_->layout.vertexCount);
        entry->indices.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) entry->indices[i] = indices[2 * i];
        if (entry->Bytes() <= state_->reserved)
            state_->pending = std::move(entry);
    } catch (...) {}
    if (!state_->pending) state_->ReleaseReservation();
    FinishGeometry();
}
void Session::FinishGeometry() noexcept {
    if (state_) {
        state_->guard.Reset();
        state_->hit.reset();
    }
}
void Session::Commit() noexcept {
    if (!state_ || !state_->pending || Disabled()) return;
    try {
        Handle guard;
        if (!LockStableFile(state_->identity, guard)) {
            state_->pending.reset();
            state_->ReleaseReservation();
            return;
        }
        auto& cache = *state_->cache;
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (auto it = cache.entries.begin(); it != cache.entries.end();) {
            if ((*it)->identity.path == state_->identity.path) {
                (*it)->retired = true;
                if (it->use_count() == 1) {
                    cache.statistics.bytes -= (*it)->Bytes();
                    it = cache.entries.erase(it);
                } else ++it;
            } else ++it;
        }
        const std::size_t bytes = state_->pending->Bytes();
        while (cache.entries.size() >= cache.entryLimit) {
            auto removable = cache.entries.end();
            for (auto it = cache.entries.end(); it != cache.entries.begin();) {
                --it;
                if (it->use_count() == 1) { removable = it; break; }
            }
            if (removable == cache.entries.end()) return;
            cache.statistics.bytes -= (*removable)->Bytes();
            cache.entries.erase(removable);
            ++cache.statistics.evictions;
        }
        // Allocate the list node before publishing accounting.
        cache.entries.push_front(state_->pending);
        cache.statistics.bytes += bytes;
        cache.statistics.pendingBytes -= state_->reserved;
        state_->reserved = 0;
        ++cache.statistics.stores;
        state_->pending.reset();
    } catch (...) {}
}
void Session::Invalidate() noexcept {
    if (!state_) return;
    auto& cache = *state_->cache;
    std::lock_guard<std::mutex> lock(cache.mutex);
    state_->hit.reset();
    for (auto it = cache.entries.begin(); it != cache.entries.end();) {
        if ((*it)->identity.path == state_->identity.path) {
            if (!(*it)->retired) ++cache.statistics.invalidations;
            (*it)->retired = true;
            if (it->use_count() == 1) {
                cache.statistics.bytes -= (*it)->Bytes();
                it = cache.entries.erase(it);
            } else ++it;
        } else ++it;
    }
    state_->pending.reset();
    if (state_->reserved) {
        cache.statistics.pendingBytes -= state_->reserved;
        state_->reserved = 0;
    }
}

}  // namespace mikumikustudio::model_geometry_cache
