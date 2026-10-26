#pragma once
#include "common.hpp"
#include <mms/render_image.h>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace mikumikustudio::plugins {
// Host-owned immutable CPU images. Image handles and read mappings share one
// payload, so retaining or mapping never copies pixels and never charges the
// byte budget twice. Every table call, Retire and HasLeases locks the same
// mutex; no plugin code runs while the lock is held.
//
// A Client lives in caller-owned stable storage. The caller sets Client::owner
// before publishing the table to plugin code, so Api itself writes no Client
// state and may run again while worker threads read the owner. Handles are
// bound to the exact Client that created them: another Client, even one for the
// same plugin, receives MMS_E_NOT_FOUND.
class RenderImageStore {
public:
    struct Client {
        RenderImageStore* owner = nullptr;
        bool retired = false;
        // Live image handles, live mappings and charged bytes. The store mutex
        // guards retired and these counters; owner is set by the caller before
        // the table has any concurrent users.
        uint32_t images = 0;
        uint32_t mappings = 0;
        uint64_t bytes = 0;
        Client() = default;
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        Client(Client&&) = delete;
        Client& operator=(Client&&) = delete;
    };
    RenderImageStore() = default;
    ~RenderImageStore();
    RenderImageStore(const RenderImageStore&) = delete;
    RenderImageStore& operator=(const RenderImageStore&) = delete;
    RenderImageStore(RenderImageStore&&) = delete;
    RenderImageStore& operator=(RenderImageStore&&) = delete;

    // Builds the table for one client on the UI thread. The table binds context
    // to the client and stays callable from worker threads. Only table fields
    // are written here: Client::owner must already point at this store, which
    // keeps a repeated Api call race-free against background owner readers.
    MmsRenderImageApiV1 Api(Client& client);
    // Drops every image handle owned by the client. Mappings keep their pixels
    // alive so a background unmap can still use the pointer; repeatable and
    // allocation-free.
    void Retire(Client& client) noexcept;
    // True while one of the client's mappings still exists. The host uses this
    // to delay plugin unload until background unmap finishes.
    bool HasLeases(Client& client) const;

private:
    struct Payload;
    struct Entry {
        Client* owner = nullptr;
        std::shared_ptr<const Payload> payload;
    };
    MmsResult Capabilities(Client& client, MmsRenderImageCapsV1* out) const;
    MmsResult Create(Client& client, const MmsRenderImageDescV1* desc, MmsBytes pixels, MmsHandle* out);
    MmsResult Describe(Client& client, MmsHandle image, MmsRenderImageInfoV1* out) const;
    MmsResult Retain(Client& client, MmsHandle image, MmsHandle* out);
    MmsResult MapRead(Client& client, MmsHandle image, MmsRenderImageMapV1* out);
    MmsResult Unmap(Client& client, MmsHandle mapping);
    MmsResult Release(Client& client, MmsHandle image);
    // Returns the payload's byte charge when the passed reference is the last
    // one. Called with the mutex held, after the owning table entry is erased.
    void Settle(const std::shared_ptr<const Payload>& payload, Client& client) noexcept;

    mutable std::mutex mutex_;
    std::map<MmsHandle, Entry> images_;
    std::map<MmsHandle, Entry> mappings_;
    // One monotonically increasing sequence shared by images and mappings; never
    // reused and marked by the high bit so small integers from other services
    // cannot collide. Zero means the sequence is exhausted.
    uint64_t next_ = (uint64_t(1) << 63) | 1u;
    uint64_t totalBytes_ = 0;
};
}
