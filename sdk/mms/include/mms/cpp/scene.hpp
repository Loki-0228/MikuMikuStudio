#pragma once

// C++17 convenience wrapper for mms.scene 1.0: snapshot acquisition, object
// iteration and channel reads. The wrapper keeps the original host handle and
// returns borrowed spans; the snapshot lease must outlive every span read from
// it and must be released explicitly (no host call from a destructor).
#include <mms/scene.h>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace mms::sdk {
namespace scene_detail {
template<class F> MmsResult Call(F&& call) noexcept {
    try { return call(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
} // namespace scene_detail

class SceneSnapshot {
    MmsSceneApiV1 api_{};
    MmsHandle handle_ = 0;
public:
    SceneSnapshot() = default;
    SceneSnapshot(MmsSceneApiV1 api, MmsHandle handle) noexcept : api_(api), handle_(handle) {}
    SceneSnapshot(const SceneSnapshot&) = delete;
    SceneSnapshot& operator=(const SceneSnapshot&) = delete;
    SceneSnapshot(SceneSnapshot&& other) noexcept : api_(other.api_), handle_(other.handle_) { other.handle_ = 0; }
    SceneSnapshot& operator=(SceneSnapshot&&) = delete;
    MmsResult TransferFrom(SceneSnapshot& other) noexcept {
        if (this == &other) return MMS_OK;
        const auto result = Release();
        if (result != MMS_OK) return result;
        api_ = other.api_; handle_ = other.handle_; other.handle_ = 0;
        return MMS_OK;
    }
    ~SceneSnapshot() = default;
    bool valid() const noexcept { return handle_ != 0; }
    MmsHandle get() const noexcept { return handle_; }
    MmsResult Release() noexcept {
        if (!handle_) return MMS_OK;
        if (!api_.release) return MMS_E_UNSUPPORTED;
        const MmsHandle handle = handle_;
        handle_ = 0;
        api_.release(api_.context, handle);
        return MMS_OK;
    }
    MmsHandle Detach() noexcept { const MmsHandle handle = handle_; handle_ = 0; return handle; }
};

class SceneServices {
    MmsHostV1 host_{};
    MmsSceneApiV1 api_{};
    bool ready_ = false;
public:
    static MmsResult Query(const MmsHostV1& host, SceneServices& out) noexcept {
        if (host.h.size < sizeof(MmsHostV1) || host.h.major != 1) return MMS_E_VERSION;
        if (!host.queryService) return MMS_E_ARGUMENT;
        return scene_detail::Call([&]() -> MmsResult {
            MmsSceneApiV1 candidate{};
            const auto result = host.queryService(host.context, MmsString{"mms.scene", 9},
                1, 0, static_cast<std::uint32_t>(sizeof(candidate)), &candidate);
            if (result != MMS_OK) return result;
            if (candidate.h.size < sizeof(candidate) || candidate.h.major != 1) return MMS_E_VERSION;
            if (!candidate.acquire || !candidate.release || !candidate.objects || !candidate.readChannel)
                return MMS_E_UNSUPPORTED;
            SceneServices services;
            services.host_ = host;
            services.api_ = candidate;
            services.ready_ = true;
            out = services;
            return MMS_OK;
        });
    }
    bool ready() const noexcept { return ready_; }
    const MmsSceneApiV1& api() const noexcept { return api_; }
    MmsResult Acquire(std::uint32_t stage, std::uint32_t flags, std::uint64_t channels,
                      MmsTime time, MmsId layer, MmsSpan objectFilter, SceneSnapshot& out) const noexcept {
        if (!ready_) return MMS_E_NOT_READY;
        if (out.valid()) return MMS_E_BUSY;
        return scene_detail::Call([&]() -> MmsResult {
            MmsSceneQueryV1 query{};
            query.h = {sizeof(query), MMS_ABI_MAJOR, MMS_ABI_MINOR};
            query.stage = stage; query.flags = flags; query.channels = channels;
            query.time = time; query.layerId = layer; query.objects = objectFilter;
            MmsSnapshotV1 snapshot{};
            snapshot.h = {sizeof(snapshot), MMS_ABI_MAJOR, MMS_ABI_MINOR};
            const auto result = api_.acquire(api_.context, &query, &snapshot);
            if (result != MMS_OK) return result;
            SceneSnapshot acquired(api_, snapshot.handle);
            return out.TransferFrom(acquired);
        });
    }
    MmsResult Objects(const SceneSnapshot& snapshot, MmsSpan& out) const noexcept {
        if (!ready_ || !snapshot.valid()) return MMS_E_NOT_READY;
        return scene_detail::Call([&] { return api_.objects(api_.context, snapshot.get(), &out); });
    }
    MmsResult ReadChannel(const SceneSnapshot& snapshot, MmsObjectRef object, std::uint32_t channel,
                          MmsSpan& out) const noexcept {
        if (!ready_ || !snapshot.valid()) return MMS_E_NOT_READY;
        return scene_detail::Call([&] { return api_.readChannel(api_.context, snapshot.get(), object, channel, &out); });
    }
};
} // namespace mms::sdk
