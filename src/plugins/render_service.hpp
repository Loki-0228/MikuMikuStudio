#pragma once
#include "common.hpp"
#include <mms/render.h>
#include <cstdint>
#include <map>
#include <string>

namespace mikumikustudio::plugins {
// Host-owned render information and device resource leases. Generation is
// advanced by the integration after every successful device restoration.
class RenderService {
public:
    struct Client {
        RenderService* owner = nullptr;
        std::string id;
    };
    void DeviceLost(uint64_t generation) noexcept;
    void DeviceRestored(uint64_t generation) noexcept;
    MmsRenderApiV1 Api(Client& client);
    // Called before the plugin is stopped; release callbacks are not invoked
    // because the plugin is already shutting down.
    void DropClient(const std::string& id) noexcept;
    uint64_t Generation() const noexcept { return generation_; }
private:
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
    std::map<MmsHandle, Lease> leases_;
    MmsHandle next_ = 1;
    uint64_t generation_ = 1;
    uint32_t lost_ = 0;
};
}
