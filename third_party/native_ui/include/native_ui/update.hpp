#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace native_ui {
enum Dirty : uint32_t { LayoutDirty = 1, UiDirty = 2, EvaluateDirty = 4, RenderDirty = 8, PresentDirty = 16, TargetDirty = 32 };
struct UpdateRequest {
    uint64_t id = 0, windowGeneration = 0, geometryRevision = 0, deadline = 0;
    uint32_t flags = 0;
};
enum class UpdateResult { Done, Retry, Failed };
struct UpdateOutcome {
    UpdateResult result = UpdateResult::Done;
    uint32_t consumed = 0;
    uint64_t retryDeadline = 0;
};
// One UI thread owns this coordinator. Callbacks may request more work, but
// recursive Drain never executes callbacks. Export jobs stay outside this
// latest-preview-request queue; it must not discard ordered output frames.
class UpdateCoordinator {
    struct Entry { std::optional<UpdateRequest> pending; uint64_t window = 0, geometry = 0; bool known = false, available = true, failed = false; };
    std::map<uint64_t, Entry> entries_;
    bool draining_ = false;
public:
    void Request(const UpdateRequest&);
    void Available(uint64_t id, bool value);
    void Forget(uint64_t id);
    std::optional<uint64_t> Deadline() const;
    bool Failed(uint64_t id) const;
    size_t Drain(uint64_t now, size_t budget, const std::function<UpdateOutcome(const UpdateRequest&)>&);
};
}
