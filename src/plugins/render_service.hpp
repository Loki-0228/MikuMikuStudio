#pragma once
#include "common.hpp"
#include <mms/render.h>
#include <cstdint>
#include <map>
#include <vector>
#include <string>

namespace mikumikustudio::plugins {
// Host-owned render information and device resource leases. Generation is
// advanced by the integration after every successful device restoration.
class RenderService {
public:
    struct Client {
        RenderService* owner = nullptr;
        std::string id;
        MmsRenderFrameCallbackV1 frame = nullptr;
        void* frameContext = nullptr;
        MmsRenderPhaseCallbackV1 phase = nullptr;
        void* phaseContext = nullptr;
        bool retired = false;
    };
    // Convenience entry point: marks the loss, then requests an immediate drain.
    // Inside a render callback the drain is deferred: the loss stays pending
    // until FlushDeviceLost is called again outside the callback.
    void DeviceLost(uint64_t generation) noexcept;
    // Marks the device as lost without running plugin code. A nested mark from a
    // release callback is ignored because the outer drain owns the leases.
    void MarkDeviceLost(uint64_t generation) noexcept;
    // Releases every uncancelled lease when a loss is pending. Returns true when
    // nothing is pending or the drain finished; false when called from a callback
    // or from inside the drain, leaving the loss pending for a later call.
    bool FlushDeviceLost() noexcept;
    bool DeviceLossPending() const noexcept { return pending_; }
    void FramePresented(uint32_t width, uint32_t height, uint32_t format) noexcept;
    void DeviceRestored(uint64_t generation) noexcept;
    MmsRenderApiV1 Api(Client& client);
    MmsRenderApiV1_1 Api11(Client& client);
    MmsRenderApiV1_2 Api12(Client& client);
    void FrameBegin() noexcept;
    void FrameEnd() noexcept;
    // Called before the plugin is stopped; release callbacks are not invoked
    // because the plugin is already shutting down.
    void DropClient(const std::string& id) noexcept;
    uint64_t Generation() const noexcept { return generation_; }
    bool InCallback() const noexcept { return callbackDepth_ != 0; }
private:
    struct CallbackScope {
        unsigned& depth;
        explicit CallbackScope(unsigned& value) noexcept : depth(value) { ++depth; }
        ~CallbackScope() { --depth; }
    };
    struct Lease {
        std::string owner;
        std::string name;
        void (MMS_CALL *release)(void*) = nullptr;
        void* context = nullptr;
        uint64_t generation = 0;
    };
    MmsResult Query(MmsRenderInfoV1* out) const;
    MmsResult Acquire(Client& client, const MmsRenderLeaseDescV1* desc, MmsHandle* out);
    MmsResult Release(Client& client, MmsHandle lease);
    void ReleaseAll(const char* reason) noexcept;
    void Invoke(Lease& lease, const char* reason) noexcept;
    void SetFrameCallback(Client& client, MmsRenderFrameCallbackV1 callback, void* user);
    void ClearFrameCallback(Client& client);
    void SetPhaseCallback(Client& client, MmsRenderPhaseCallbackV1 callback, void* user);
    void ClearPhaseCallback(Client& client);
    void InvokePhase(uint32_t phase) noexcept;
    std::map<MmsHandle, Lease> leases_;
    std::vector<Client*> clients_;
    MmsHandle next_ = 1;
    unsigned callbackDepth_ = 0;
    // True while ReleaseAll is dispatching lease release callbacks. The drain
    // owns the lease map, so nested device events must not dispatch again.
    bool draining_ = false;
    // A loss has been marked but its lease drain has not completed yet. Restore
    // requests are rejected while this is set.
    bool pending_ = false;
    uint64_t generation_ = 1, frameIndex_ = 0;
    uint32_t lost_ = 0, width_ = 0, height_ = 0, format_ = 0;
};
}
