#include "render_service.hpp"

namespace mikumikustudio::plugins {
void RenderService::DeviceLost(uint64_t generation) noexcept {
    generation_ = generation ? generation : generation_;
    lost_ = 1;
    ReleaseAll("device lost");
}
void RenderService::FramePresented(uint32_t width, uint32_t height, uint32_t format) noexcept {
    if (InCallback()) return;
    ++frameIndex_;
    if (width) width_ = width;
    if (height) height_ = height;
    format_ = format;
    if (clients_.empty()) return;
    MmsRenderInfoV1 info{};
    info.h = Header<MmsRenderInfoV1>();
    info.deviceGeneration = generation_;
    info.frameIndex = frameIndex_;
    info.lost = lost_;
    info.width = width_;
    info.height = height_;
    info.backBufferFormat = format_;
    // Copy the callback list: a callback may clear itself or register another
    // client, and the render path must not iterate a mutated container. If the
    // copy cannot be prepared, skip this notification entirely instead of
    // dispatching a partial list.
    std::vector<std::pair<MmsRenderFrameCallbackV1, void*>> callbacks;
    try {
        callbacks.reserve(clients_.size());
        for (Client* client : clients_) if (client->frame) callbacks.emplace_back(client->frame, client->frameContext);
    } catch (...) {
        return;
    }
    for (const auto& [callback, context] : callbacks) {
        CallbackScope scope(callbackDepth_);
        try { Boundary([&] { callback(context, &info); return MMS_OK; }); } catch (...) {}
    }
}
void RenderService::InvokePhase(uint32_t phase) noexcept {
    if (InCallback() || clients_.empty()) return;
    MmsRenderInfoV1 info{};
    info.h = Header<MmsRenderInfoV1>();
    info.deviceGeneration = generation_;
    info.frameIndex = frameIndex_;
    info.lost = lost_;
    info.width = width_;
    info.height = height_;
    info.backBufferFormat = format_;
    std::vector<std::pair<MmsRenderPhaseCallbackV1, void*>> callbacks;
    try {
        callbacks.reserve(clients_.size());
        for (Client* client : clients_) if (client->phase) callbacks.emplace_back(client->phase, client->phaseContext);
    } catch (...) {
        return;
    }
    for (const auto& [callback, context] : callbacks) {
        CallbackScope scope(callbackDepth_);
        try { Boundary([&] { callback(context, &info, phase); return MMS_OK; }); } catch (...) {}
    }
}
void RenderService::FrameBegin() noexcept { InvokePhase(MMS_RENDER_PHASE_BEGIN); }
void RenderService::FrameEnd() noexcept { InvokePhase(MMS_RENDER_PHASE_END); }
void RenderService::SetPhaseCallback(Client& client, MmsRenderPhaseCallbackV1 callback, void* user) {
    // The insertion can throw; do it before touching the callback fields so a
    // failure reported as MMS_E_MEMORY leaves the previous registration intact.
    if (callback && std::find(clients_.begin(), clients_.end(), &client) == clients_.end()) clients_.push_back(&client);
    client.phase = callback;
    client.phaseContext = user;
}
void RenderService::ClearPhaseCallback(Client& client) {
    client.phase = nullptr;
    client.phaseContext = nullptr;
    if (!client.frame) clients_.erase(std::remove(clients_.begin(), clients_.end(), &client), clients_.end());
}
void RenderService::SetFrameCallback(Client& client, MmsRenderFrameCallbackV1 callback, void* user) {
    if (callback && std::find(clients_.begin(), clients_.end(), &client) == clients_.end()) clients_.push_back(&client);
    client.frame = callback;
    client.frameContext = user;
}
void RenderService::ClearFrameCallback(Client& client) {
    client.frame = nullptr;
    client.frameContext = nullptr;
    if (!client.phase) clients_.erase(std::remove(clients_.begin(), clients_.end(), &client), clients_.end());
}
void RenderService::DeviceRestored(uint64_t generation) noexcept {
    if (generation) generation_ = generation;
    lost_ = 0;
}
MmsRenderApiV1 RenderService::Api(Client& client) {
    client.owner = this;
    if (std::find(clients_.begin(), clients_.end(), &client) == clients_.end()) clients_.push_back(&client);
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
MmsRenderApiV1_2 RenderService::Api12(Client& client) {
    const auto base = Api11(client);
    MmsRenderApiV1_2 api{};
    api.h = Header<MmsRenderApiV1_2>();
    api.context = &client;
    api.query = base.query;
    api.acquireLease = base.acquireLease;
    api.releaseLease = base.releaseLease;
    api.setFrameCallback = base.setFrameCallback;
    api.clearFrameCallback = base.clearFrameCallback;
    api.setPhaseCallback = [](void* context, MmsRenderPhaseCallbackV1 callback, void* user) -> MmsResult {
        return Boundary([&] { auto* client = static_cast<Client*>(context); if (client->retired) return MMS_E_NOT_READY; client->owner->SetPhaseCallback(*client, callback, user); return MMS_OK; });
    };
    api.clearPhaseCallback = [](void* context) {
        auto* client = static_cast<Client*>(context); client->owner->ClearPhaseCallback(*client);
    };
    return api;
}
MmsRenderApiV1_1 RenderService::Api11(Client& client) {
    const auto base = Api(client);
    MmsRenderApiV1_1 api{};
    api.h = Header<MmsRenderApiV1_1>();
    api.context = &client;
    api.query = base.query;
    api.acquireLease = base.acquireLease;
    api.releaseLease = base.releaseLease;
    api.setFrameCallback = [](void* context, MmsRenderFrameCallbackV1 callback, void* user) -> MmsResult {
        return Boundary([&] { auto* client = static_cast<Client*>(context); if (client->retired) return MMS_E_NOT_READY; client->owner->SetFrameCallback(*client, callback, user); return MMS_OK; });
    };
    api.clearFrameCallback = [](void* context) {
        static_cast<Client*>(context)->owner->ClearFrameCallback(*static_cast<Client*>(context));
    };
    return api;
}
void RenderService::DropClient(const std::string& id) noexcept {
    for (auto it = leases_.begin(); it != leases_.end();) {
        if (it->second.owner == id) it = leases_.erase(it); else ++it;
    }
    for (auto it = clients_.begin(); it != clients_.end();) {
        if ((*it)->id == id) {
            (*it)->retired = true;
            (*it)->frame = nullptr; (*it)->frameContext = nullptr;
            (*it)->phase = nullptr; (*it)->phaseContext = nullptr;
            it = clients_.erase(it);
        } else ++it;
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
    if (client.retired) return MMS_E_NOT_READY;
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
    CallbackScope scope(callbackDepth_);
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
