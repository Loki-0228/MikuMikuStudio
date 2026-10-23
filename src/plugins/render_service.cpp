#include "render_service.hpp"

namespace mikumikustudio::plugins {
void RenderService::DeviceLost(uint64_t generation) noexcept {
    generation_ = generation ? generation : generation_;
    lost_ = 1;
    ReleaseAll("device lost");
}
void RenderService::FramePresented(uint32_t width, uint32_t height, uint32_t format) noexcept {
    ++frameIndex_;
    if (width) width_ = width;
    if (height) height_ = height;
    format_ = format;
}
void RenderService::DeviceRestored(uint64_t generation) noexcept {
    if (generation) generation_ = generation;
    lost_ = 0;
}
MmsRenderApiV1 RenderService::Api(Client& client) {
    client.owner = this;
    MmsRenderApiV1 api{Header<MmsRenderApiV1>(), &client};
    api.query = [](void* context, MmsRenderInfoV1* out) {
        return Boundary([&] { return static_cast<Client*>(context)->owner->Query(out); });
    };
    api.acquireLease = [](void* context, const MmsRenderLeaseDescV1* desc, MmsHandle* out) {
        return Boundary([&] { return static_cast<Client*>(context)->owner->Acquire(*static_cast<Client*>(context), desc, out); });
    };
    api.releaseLease = [](void* context, MmsHandle lease) {
        return Boundary([&] { return static_cast<Client*>(context)->owner->Release(*static_cast<Client*>(context), lease); });
    };
    return api;
}
void RenderService::DropClient(const std::string& id) noexcept {
    for (auto it = leases_.begin(); it != leases_.end();) {
        if (it->second.owner == id) it = leases_.erase(it); else ++it;
    }
}
MmsResult RenderService::Query(MmsRenderInfoV1* out) const {
    if (!out) return MMS_E_ARGUMENT;
    MmsRenderInfoV1 info{};
    info.h = Header<MmsRenderInfoV1>();
    info.deviceGeneration = generation_;
    info.frameIndex = frameIndex_;
    info.lost = lost_;
    info.width = width_;
    info.height = height_;
    info.backBufferFormat = format_;
    *out = info;
    return MMS_OK;
}
MmsResult RenderService::Acquire(Client& client, const MmsRenderLeaseDescV1* desc, MmsHandle* out) {
    if (!desc || !out || !Valid(desc) || !desc->release || (desc->flags & ~MMS_RENDER_LEASE_DEVICE) || desc->reserved) {
        return MMS_E_ARGUMENT;
    }
    if (client.id.empty() || String(desc->name).size() > 128) return MMS_E_ARGUMENT;
    Lease lease;
    lease.owner = client.id;
    lease.name = String(desc->name);
    lease.release = desc->release;
    lease.context = desc->context;
    lease.generation = generation_;
    const MmsHandle handle = next_++;
    leases_.emplace(handle, std::move(lease));
    *out = handle;
    return MMS_OK;
}
MmsResult RenderService::Release(Client& client, MmsHandle lease) {
    const auto found = leases_.find(lease);
    if (found == leases_.end() || found->second.owner != client.id) return MMS_E_NOT_FOUND;
    leases_.erase(found);
    return MMS_OK;
}
void RenderService::Invoke(Lease& lease, const char* reason) noexcept {
    if (!lease.release) return;
    (void)reason;
    try {
        Boundary([&] { lease.release(lease.context); return MMS_OK; });
    } catch (...) {
    }
}
void RenderService::ReleaseAll(const char* reason) noexcept {
    auto leases = std::move(leases_);
    leases_.clear();
    for (auto& [handle, lease] : leases) Invoke(lease, reason);
}
}
