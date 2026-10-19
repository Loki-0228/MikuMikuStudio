#pragma once
#include "ui_refresh.hpp"
#include <mms/refresh_binding.h>

namespace mikumikustudio::plugins {
class UiRefreshBindings {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    struct Hooks {
        std::function<MmsResult(RefreshClient&, const MmsUiTargetRefV1&)> validateTarget;
        std::function<MmsResult(const std::function<MmsResult()>&)> readOnly;
        std::function<bool()> inCallback;
    };
    struct DataContribution;
    using Contribution = std::shared_ptr<DataContribution>;
    explicit UiRefreshBindings(Hooks = {});
    ~UiRefreshBindings();
    UiRefreshBindings(const UiRefreshBindings&) = delete;
    UiRefreshBindings& operator=(const UiRefreshBindings&) = delete;
    void SetHooks(Hooks);
    MmsUiRefreshBindingApiV1 Api(RefreshClient&);
    // queue -> binding is the only nested lock order. Capture/HasBinding do
    // not call hooks or touch HWND/registry, and are safe under the queue lock.
    MmsResult Capture(RefreshClient&, MmsHandle binding, const MmsUiTargetRefV1&,
        uint64_t requestedRevision, Contribution&);
    MmsResult Consume(const Contribution&, const std::function<MmsResult()>& revalidate,
        uint64_t& consumedRevision, bool& displayChanged);
    bool HasBinding(const RefreshClient&, const MmsUiTargetRefV1&) const;
    void InvalidateTarget(MmsHandle) noexcept;
    void AdvanceProjectEpoch(uint64_t) noexcept;
    void Retire(RefreshClient&) noexcept;
    bool HasLeases(const RefreshClient&) const;
};
}
