#include "ui_refresh_binding.hpp"
#include <map>
#include <mutex>
#include <limits>

namespace mikumikustudio::plugins {
namespace {
constexpr uint32_t kSources = 64, kBindings = 128, kLeases = 128, kContributions = 256;
constexpr uint64_t kModelBytes = 256 * 1024, kOwnerBytes = 2 * 1024 * 1024, kGlobalBytes = 32 * 1024 * 1024;
bool SameTarget(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) noexcept {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
bool TargetValid(const MmsUiTargetRefV1& target) noexcept {
    return Valid(&target) && target.element && target.instanceGeneration && target.projectEpoch;
}
struct Accounting {
    std::atomic<uint64_t> bytes{0};
    std::atomic<unsigned> contributions{0};
};
bool Reserve(std::atomic<uint64_t>& value, uint64_t amount, uint64_t maximum) noexcept {
    auto previous = value.load();
    do { if (previous > maximum || amount > maximum - previous) return false; }
    while (!value.compare_exchange_weak(previous, previous + amount));
    return true;
}
struct Snapshot {
    std::shared_ptr<Accounting> owner, global;
    uint64_t charge = 0;
    std::vector<unsigned char> bytes;
    ~Snapshot() { if (charge) { owner->bytes.fetch_sub(charge); global->bytes.fetch_sub(charge); } }
    MmsBytes View() const noexcept { return {bytes.empty() ? nullptr : bytes.data(), bytes.size()}; }
};
MmsResult CopySnapshot(const std::shared_ptr<Accounting>& owner, const std::shared_ptr<Accounting>& global,
                       MmsBytes bytes, uint64_t maximum, std::shared_ptr<const Snapshot>& out) {
    if ((bytes.bytes && !bytes.data) || bytes.bytes > maximum) return MMS_E_ARGUMENT;
    auto next = std::make_shared<Snapshot>(); next->owner = owner; next->global = global;
    if (!Reserve(owner->bytes, bytes.bytes, kOwnerBytes)) return MMS_E_BUSY;
    if (!Reserve(global->bytes, bytes.bytes, kGlobalBytes)) { owner->bytes.fetch_sub(bytes.bytes); return MMS_E_BUSY; }
    next->charge = bytes.bytes;
    next->bytes.resize(static_cast<size_t>(bytes.bytes));
    if (bytes.bytes) std::memcpy(next->bytes.data(), bytes.data, static_cast<size_t>(bytes.bytes));
    out = std::move(next); return MMS_OK;
}
bool Equal(const std::shared_ptr<const Snapshot>& a, MmsBytes b) noexcept {
    return a && a->bytes.size() == b.bytes && (!b.bytes || !std::memcmp(a->bytes.data(), b.data, size_t(b.bytes)));
}
}

struct UiRefreshBindings::Impl {
    struct Owner {
        Impl* host;
        RefreshClient* client;
        std::shared_ptr<Accounting> accounting = std::make_shared<Accounting>();
        std::atomic<bool> retired{false};
        unsigned sources = 0, bindings = 0, leases = 0, builders = 0;
    };
    struct Source {
        MmsHandle handle = 0;
        std::shared_ptr<Owner> owner;
        std::string id;
        uint64_t epoch = 0, revision = 0, maximum = 0;
        std::shared_ptr<const Snapshot> snapshot;
    };
    struct Binding {
        MmsHandle handle = 0, source = 0;
        std::shared_ptr<Owner> owner;
        MmsUiTargetRefV1 target{};
        void* context = nullptr;
        MmsUiDataPrepareCallbackV1 prepare = nullptr;
        uint64_t maximum = 0, consumed = 0, displayRevision = 0;
        bool active = true;
        std::shared_ptr<const Snapshot> display;
    };
    struct Lease { std::shared_ptr<Owner> owner; std::shared_ptr<const Snapshot> display; };
    struct Builder {
        std::shared_ptr<Owner> owner;
        uint64_t maximum = 0;
        bool written = false;
        MmsResult failure = MMS_OK;
        std::shared_ptr<const Snapshot> candidate;
    };
    struct Flight {
        RefreshClient& client;
        explicit Flight(Owner& owner) : client(*owner.client) { client.inFlight.fetch_add(1); }
        ~Flight() { client.inFlight.fetch_sub(1); }
    };
    mutable std::mutex mutex;
    DWORD thread = GetCurrentThreadId();
    uint64_t epoch = 1, next = 1;
    Hooks hooks;
    std::shared_ptr<Accounting> global = std::make_shared<Accounting>();
    std::map<RefreshClient*, std::shared_ptr<Owner>> owners;
    // API contexts remain stable until the manager has released all plugin tables.
    std::vector<std::shared_ptr<Owner>> contexts;
    std::map<MmsHandle, Source> sources;
    std::map<MmsHandle, std::shared_ptr<Binding>> bindings;
    std::map<MmsHandle, Lease> leases;
    std::map<MmsHandle, Builder*> builders;

    explicit Impl(Hooks value) : hooks(std::move(value)) {}
    bool ReadOnly() const { return GetCurrentThreadId() == thread && hooks.inCallback && hooks.inCallback(); }
    MmsResult Mutation(Owner& owner, bool ui) const {
        if (ui && GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        return owner.retired || owner.client->stopping.load() ? MMS_E_NOT_READY : MMS_OK;
    }
    MmsHandle Next() { if (next == UINT64_MAX) throw std::bad_alloc(); return next++; }
    std::shared_ptr<Owner> For(RefreshClient& client) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = owners.find(&client);
        if (found != owners.end() && (!found->second->retired.load() || client.stopping.load())) return found->second;
        if (found != owners.end() && (found->second->leases || found->second->builders ||
            found->second->accounting->contributions.load())) throw std::invalid_argument("DATA owner still leased");
        auto owner = std::make_shared<Owner>(); owner->host = this; owner->client = &client;
        contexts.push_back(owner); owners[&client] = owner; return owner;
    }
    MmsResult Caps(Owner& owner, MmsUiDataCapabilitiesV1* out) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        *out = {Header<MmsUiDataCapabilitiesV1>(), kSources, kBindings, kLeases, 1,
            kModelBytes, kModelBytes, kOwnerBytes, kContributions, 0}; return MMS_OK;
    }
    MmsResult Register(Owner& owner, const MmsUiDataSourceDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || desc->flags || desc->reserved || !desc->maxBytes || desc->maxBytes > kModelBytes) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, true)) return r;
        auto id = String(desc->id); if (!ValidId(id)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        if (owner.sources >= kSources || sources.size() >= 1024) return MMS_E_BUSY;
        for (const auto& item : sources) if (item.second.owner.get() == &owner && item.second.id == id) return MMS_E_CONFLICT;
        const auto handle = Next();
        Source source; source.handle = handle; source.owner = owners.at(owner.client); source.id = std::move(id);
        source.epoch = epoch; source.maximum = desc->maxBytes;
        sources.emplace(handle, std::move(source)); ++owner.sources; *out = handle; return MMS_OK;
    }
    MmsResult SourceState(Owner& owner, MmsHandle handle, MmsUiDataSourceStateV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        auto found = sources.find(handle); if (found == sources.end()) return MMS_E_STALE;
        const auto& s = found->second;
        if (s.owner.get() != &owner) return MMS_E_ARGUMENT;
        *out = {Header<MmsUiDataSourceStateV1>(), s.handle, s.revision, s.epoch, s.snapshot ? s.snapshot->bytes.size() : 0}; return MMS_OK;
    }
    MmsResult Publish(Owner& owner, const MmsUiDataSourcePublishV1* update, MmsUiDataSourceStateV1* out) {
        if (!Valid(update) || !Valid(out) || update->flags || update->reserved || (update->bytes.bytes && !update->bytes.data)) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, false)) return r;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        auto found = sources.find(update->source); if (found == sources.end()) return MMS_E_STALE;
        auto& s = found->second; if (s.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (s.epoch != epoch) return MMS_E_STALE;
        if (update->expectedRevision != s.revision) return MMS_E_CONFLICT;
        if (update->bytes.bytes > s.maximum) return MMS_E_ARGUMENT;
        if (!Equal(s.snapshot, update->bytes)) {
            if (s.revision == UINT64_MAX) return MMS_E_BUSY;
            std::shared_ptr<const Snapshot> candidate;
            if (const auto r = CopySnapshot(owner.accounting, global, update->bytes, s.maximum, candidate)) return r;
            s.snapshot = std::move(candidate); ++s.revision;
        }
        *out = {Header<MmsUiDataSourceStateV1>(), s.handle, s.revision, s.epoch, s.snapshot->bytes.size()}; return MMS_OK;
    }
    void RemoveBinding(std::map<MmsHandle, std::shared_ptr<Binding>>::iterator found) noexcept {
        found->second->active = false; --found->second->owner->bindings; bindings.erase(found);
    }
    MmsResult Unregister(Owner& owner, MmsHandle handle) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        std::lock_guard<std::mutex> lock(mutex);
        auto found = sources.find(handle); if (found == sources.end()) return MMS_E_STALE;
        if (found->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        for (auto it = bindings.begin(); it != bindings.end();) {
            if (it->second->source == handle) { auto erase = it++; RemoveBinding(erase); } else ++it;
        }
        --owner.sources; sources.erase(found); return MMS_OK;
    }
    MmsResult Bind(Owner& owner, const MmsUiDataBindingDescV1* desc, MmsHandle* out) {
        if (!Valid(desc) || !out || !TargetValid(desc->target) || !desc->source || !desc->maxDisplayBytes ||
            desc->maxDisplayBytes > kModelBytes || desc->flags || desc->reserved) return MMS_E_ARGUMENT;
        if (const auto r = Mutation(owner, true)) return r;
        if (!hooks.validateTarget) return MMS_E_NOT_READY;
        if (const auto r = hooks.validateTarget(*owner.client, desc->target)) return r;
        auto binding = std::make_shared<Binding>();
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        if (desc->target.projectEpoch != epoch) return MMS_E_STALE;
        auto source = sources.find(desc->source); if (source == sources.end() || source->second.epoch != epoch) return MMS_E_STALE;
        if (source->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (owner.bindings >= kBindings || bindings.size() >= 2048) return MMS_E_BUSY;
        for (const auto& entry : bindings) if (SameTarget(entry.second->target, desc->target)) return MMS_E_CONFLICT;
        binding->handle = Next(); binding->source = desc->source; binding->owner = source->second.owner;
        binding->target = desc->target; binding->context = desc->context; binding->prepare = desc->prepare; binding->maximum = desc->maxDisplayBytes;
        bindings.emplace(binding->handle, binding); ++owner.bindings; *out = binding->handle; return MMS_OK;
    }
    MmsResult Unbind(Owner& owner, MmsHandle handle) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        if (ReadOnly()) return MMS_E_BUSY;
        std::lock_guard<std::mutex> lock(mutex);
        auto found = bindings.find(handle); if (found == bindings.end()) return MMS_E_STALE;
        if (found->second->owner.get() != &owner) return MMS_E_ARGUMENT;
        RemoveBinding(found); return MMS_OK;
    }
    MmsResult WritePrepared(Owner& owner, MmsHandle handle, MmsBytes bytes) {
        if (GetCurrentThreadId() != thread) return MMS_E_THREAD;
        std::lock_guard<std::mutex> lock(mutex);
        auto found = builders.find(handle); if (found == builders.end()) return MMS_E_STALE;
        auto& builder = *found->second;
        if (builder.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (owner.retired || owner.client->stopping.load()) return builder.failure = MMS_E_CANCELLED;
        if (builder.failure) return builder.failure;
        if (builder.written) return builder.failure = MMS_E_CONFLICT;
        const auto result = Boundary([&] { return CopySnapshot(owner.accounting, global, bytes, builder.maximum, builder.candidate); });
        if (result) return builder.failure = result;
        builder.written = true; return MMS_OK;
    }
    MmsResult Acquire(Owner& owner, MmsHandle handle, MmsUiDataDisplayV1* out) {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex);
        if (owner.retired || owner.client->stopping.load()) return MMS_E_NOT_READY;
        auto found = bindings.find(handle); if (found == bindings.end()) return MMS_E_STALE;
        const auto& b = *found->second; if (b.owner.get() != &owner) return MMS_E_ARGUMENT;
        if (!b.display) return MMS_E_NOT_READY;
        if (owner.leases >= kLeases || leases.size() >= 4096) return MMS_E_BUSY;
        const auto lease = Next(); leases.emplace(lease, Lease{b.owner, b.display}); ++owner.leases;
        *out = {Header<MmsUiDataDisplayV1>(), lease, b.handle, b.source, b.consumed, b.displayRevision, b.target.projectEpoch, b.display->View()}; return MMS_OK;
    }
    MmsResult Release(Owner& owner, MmsHandle handle) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = leases.find(handle); if (found == leases.end()) return MMS_E_NOT_FOUND;
        if (found->second.owner.get() != &owner) return MMS_E_ARGUMENT;
        --owner.leases; leases.erase(found); return MMS_OK;
    }
};

struct UiRefreshBindings::DataContribution {
    std::shared_ptr<Impl::Binding> binding;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<Accounting> global;
    uint64_t revision = 0;
    bool charged = false;
    ~DataContribution() {
        if (charged) { binding->owner->accounting->contributions.fetch_sub(1); global->contributions.fetch_sub(1); }
    }
};

UiRefreshBindings::UiRefreshBindings(Hooks hooks) : impl_(std::make_unique<Impl>(std::move(hooks))) {}
UiRefreshBindings::~UiRefreshBindings() = default;
void UiRefreshBindings::SetHooks(Hooks hooks) {
    if (GetCurrentThreadId() != impl_->thread) throw std::invalid_argument("DATA hooks thread");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->contexts.empty()) throw std::invalid_argument("DATA hooks already in use");
    impl_->hooks = std::move(hooks);
}
MmsUiRefreshBindingApiV1 UiRefreshBindings::Api(RefreshClient& client) {
    auto owner = impl_->For(client);
    MmsUiRefreshBindingApiV1 api{}; api.h = Header<MmsUiRefreshBindingApiV1>(); api.context = owner.get();
#define DATA_CALL(body) auto& o = *static_cast<Impl::Owner*>(p); Impl::Flight flight(o); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiDataCapabilitiesV1* out) { DATA_CALL(return o.host->Caps(o, out)); };
    api.registerSource = [](void* p, const MmsUiDataSourceDescV1* desc, MmsHandle* out) { DATA_CALL(return o.host->Register(o, desc, out)); };
    api.publishSource = [](void* p, const MmsUiDataSourcePublishV1* update, MmsUiDataSourceStateV1* out) { DATA_CALL(return o.host->Publish(o, update, out)); };
    api.getSource = [](void* p, MmsHandle h, MmsUiDataSourceStateV1* out) { DATA_CALL(return o.host->SourceState(o, h, out)); };
    api.unregisterSource = [](void* p, MmsHandle h) { DATA_CALL(return o.host->Unregister(o, h)); };
    api.bind = [](void* p, const MmsUiDataBindingDescV1* desc, MmsHandle* out) { DATA_CALL(return o.host->Bind(o, desc, out)); };
    api.unbind = [](void* p, MmsHandle h) { DATA_CALL(return o.host->Unbind(o, h)); };
    api.writePrepared = [](void* p, MmsHandle h, MmsBytes bytes) { DATA_CALL(return o.host->WritePrepared(o, h, bytes)); };
    api.acquireDisplay = [](void* p, MmsHandle h, MmsUiDataDisplayV1* out) { DATA_CALL(return o.host->Acquire(o, h, out)); };
    api.releaseDisplay = [](void* p, MmsHandle h) { DATA_CALL(return o.host->Release(o, h)); };
#undef DATA_CALL
    return api;
}
MmsResult UiRefreshBindings::Capture(RefreshClient& client, MmsHandle handle, const MmsUiTargetRefV1& target,
                                    uint64_t revision, Contribution& out) {
    if (!TargetValid(target) || !handle || !revision) return MMS_E_ARGUMENT;
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.bindings.find(handle); if (found == state.bindings.end()) return MMS_E_STALE;
    const auto& b = found->second;
    if (b->owner->client != &client) return MMS_E_ARGUMENT;
    if (b->owner->retired || client.stopping.load()) return MMS_E_NOT_READY;
    if (!SameTarget(b->target, target) || target.projectEpoch != state.epoch) return MMS_E_STALE;
    auto source = state.sources.find(b->source); if (source == state.sources.end()) return MMS_E_STALE;
    if (source->second.revision != revision) return MMS_E_STALE;
    if (!source->second.snapshot) return MMS_E_NOT_READY;
    if (b->owner->accounting->contributions.load() >= kContributions || state.global->contributions.load() >= 4096) return MMS_E_BUSY;
    auto item = std::make_shared<DataContribution>(); item->binding = b; item->snapshot = source->second.snapshot;
    item->global = state.global; item->revision = revision;
    b->owner->accounting->contributions.fetch_add(1); state.global->contributions.fetch_add(1); item->charged = true;
    out = std::move(item); return MMS_OK;
}
MmsResult UiRefreshBindings::Consume(const Contribution& contribution, const std::function<MmsResult()>& revalidate,
                                    uint64_t& consumedRevision, bool& displayChanged) {
    auto& state = *impl_;
    if (GetCurrentThreadId() != state.thread) return MMS_E_THREAD;
    if (!contribution || !revalidate) return MMS_E_ARGUMENT;
    if (!state.hooks.readOnly) return MMS_E_NOT_READY;
    auto binding = contribution->binding;
    Impl::Flight flight(*binding->owner);
    bool changed = false;
    const auto result = Boundary([&] { return state.hooks.readOnly([&]() -> MmsResult {
        if (const auto r = revalidate()) return r;
        bool already = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (binding->owner->retired || binding->owner->client->stopping.load()) return MMS_E_CANCELLED;
            if (!binding->active || binding->target.projectEpoch != state.epoch) return MMS_E_STALE;
            if (binding->consumed > contribution->revision) return MMS_E_STALE;
            already = binding->consumed == contribution->revision;
        }
        if (already) return MMS_OK;
        Impl::Builder builder; builder.owner = binding->owner; builder.maximum = binding->maximum;
        MmsHandle token = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (builder.owner->builders) return MMS_E_BUSY;
            token = state.Next(); state.builders.emplace(token, &builder); ++builder.owner->builders;
        }
        struct ClearBuilder {
            Impl& state; MmsHandle token; Impl::Builder& builder;
            ~ClearBuilder() { std::lock_guard<std::mutex> lock(state.mutex); state.builders.erase(token); --builder.owner->builders; }
        } clear{state, token, builder};
        const MmsUiDataPrepareV1 input{Header<MmsUiDataPrepareV1>(), binding->target, binding->source,
            contribution->revision, contribution->snapshot->View(), token, binding->consumed};
        MmsResult prepared = MMS_OK;
        if (binding->prepare) prepared = Boundary([&] { return binding->prepare(binding->context, &input); });
        else prepared = state.WritePrepared(*binding->owner, token, input.sourceBytes);
        if (prepared) return prepared;
        if (builder.failure) return builder.failure;
        if (!builder.written || !builder.candidate) return MMS_E_FAILED;
        if (const auto r = revalidate()) return r;
        std::lock_guard<std::mutex> lock(state.mutex);
        if (binding->owner->retired || binding->owner->client->stopping.load()) return MMS_E_CANCELLED;
        if (!binding->active || binding->target.projectEpoch != state.epoch) return MMS_E_STALE;
        if (binding->consumed > contribution->revision) return MMS_E_STALE;
        changed = !Equal(binding->display, builder.candidate->View());
        if (changed && binding->displayRevision == UINT64_MAX) return MMS_E_BUSY;
        if (changed) { binding->display.swap(builder.candidate); ++binding->displayRevision; }
        binding->consumed = contribution->revision;
        return MMS_OK;
    }); });
    if (!result) { consumedRevision = contribution->revision; displayChanged = changed; }
    return result;
}
bool UiRefreshBindings::HasBinding(const RefreshClient& client, const MmsUiTargetRefV1& target) const {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    if (target.projectEpoch != state.epoch || client.stopping.load()) return false;
    for (const auto& item : state.bindings) if (item.second->owner->client == &client && SameTarget(item.second->target, target)) return true;
    return false;
}
void UiRefreshBindings::InvalidateTarget(MmsHandle element) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    for (auto it = state.bindings.begin(); it != state.bindings.end();) {
        if (it->second->target.element == element) { auto erase = it++; state.RemoveBinding(erase); } else ++it;
    }
}
void UiRefreshBindings::AdvanceProjectEpoch(uint64_t epoch) noexcept {
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    state.epoch = epoch;
    for (auto& item : state.bindings) item.second->active = false;
    state.bindings.clear(); state.sources.clear();
    for (auto& item : state.owners) { item.second->sources = 0; item.second->bindings = 0; }
}
void UiRefreshBindings::Retire(RefreshClient& client) noexcept {
    client.stopping.store(true);
    auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.owners.find(&client); if (found == state.owners.end()) return;
    auto& owner = *found->second; owner.retired = true;
    for (auto it = state.bindings.begin(); it != state.bindings.end();) {
        if (it->second->owner.get() == &owner) { auto erase = it++; state.RemoveBinding(erase); } else ++it;
    }
    for (auto it = state.sources.begin(); it != state.sources.end();) {
        if (it->second.owner.get() == &owner) { --owner.sources; it = state.sources.erase(it); } else ++it;
    }
}
bool UiRefreshBindings::HasLeases(const RefreshClient& client) const {
    const auto& state = *impl_; std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.owners.find(const_cast<RefreshClient*>(&client));
    if (found == state.owners.end()) return false;
    const auto& o = *found->second;
    return client.inFlight.load() || o.sources || o.bindings || o.leases || o.builders || o.accounting->contributions.load();
}
}
