// Pure DATA-store contract. No HWND, GPU, project, or plugin DLL is needed.
#include "../../src/plugins/ui_refresh_binding.cpp"
#include <mms/cpp/data_refresh.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <array>

namespace { thread_local bool failAllocation = false; thread_local int failAfter = -1; }
void* operator new(std::size_t size) {
    if (failAllocation || (failAfter >= 0 && failAfter-- == 0)) throw std::bad_alloc();
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks; if (!value) { std::fprintf(stderr, "FAIL DATA: %s\n", message); std::exit(1); }
}
MmsString TextOf(const std::string& value) { return {value.data(), value.size()}; }
std::string Bytes(MmsBytes bytes) { return {bytes.bytes ? static_cast<const char*>(bytes.data) : "", size_t(bytes.bytes)}; }
struct Fixture {
    UiRefreshBindings bindings;
    RefreshClient client{nullptr, "example.data"}, foreign{nullptr, "example.other"};
    MmsUiRefreshBindingApiV1 api{}, other{};
    MmsUiTargetRefV1 target{Header<MmsUiTargetRefV1>(), 11, 21, 1};
    bool readOnly = false, ready = true;
    unsigned prepares = 0;
    MmsHandle lastBuilder = 0;
    std::string sampled;
    std::function<MmsResult(const MmsUiDataPrepareV1&)> onPrepare;
    Fixture() {
        bindings.SetHooks({
            [&](RefreshClient& owner, const MmsUiTargetRefV1& ref) -> MmsResult {
                if (&owner != &client) return MMS_E_ARGUMENT;
                if (!SameTarget(ref, target)) return MMS_E_STALE;
                return ready ? MMS_OK : MMS_E_NOT_READY;
            },
            [&](const std::function<MmsResult()>& body) -> MmsResult {
                if (readOnly) return MMS_E_BUSY;
                readOnly = true;
                struct Clear { bool& value; ~Clear() { value = false; } } clear{readOnly};
                return Boundary(body);
            }, [&] { return readOnly; }});
        api = bindings.Api(client); other = bindings.Api(foreign);
    }
    ~Fixture() { bindings.Retire(client); bindings.Retire(foreign); }
    MmsHandle Source(std::string id = "status", uint64_t maximum = 256) {
        MmsUiDataSourceDescV1 desc{Header<MmsUiDataSourceDescV1>(), TextOf(id), maximum, 0, 0};
        MmsHandle handle = 0; Check(api.registerSource(api.context, &desc, &handle) == MMS_OK && handle, "register real source"); return handle;
    }
    MmsUiDataSourceStateV1 Publish(MmsHandle source, uint64_t previous, const std::string& value) {
        MmsUiDataSourcePublishV1 update{Header<MmsUiDataSourcePublishV1>(), source, previous, {value.data(), value.size()}, 0, 0};
        MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
        Check(api.publishSource(api.context, &update, &state) == MMS_OK, "copy immutable source bytes"); return state;
    }
    MmsHandle Bind(MmsHandle source, bool callback = true, uint64_t maximum = 256) {
        MmsUiDataBindingDescV1 desc{Header<MmsUiDataBindingDescV1>(), target, source, this,
            callback ? &Fixture::Prepare : nullptr, maximum, 0, 0};
        MmsHandle handle = 0; Check(api.bind(api.context, &desc, &handle) == MMS_OK && handle, "bind the original target and source handles"); return handle;
    }
    static MmsResult MMS_CALL Prepare(void* context, const MmsUiDataPrepareV1* input) {
        auto& f = *static_cast<Fixture*>(context); ++f.prepares;
        Check(f.readOnly && f.client.inFlight.load(), "prepare holds host read-only guard and DLL flight");
        f.lastBuilder = input->builder; f.sampled = Bytes(input->sourceBytes);
        if (f.onPrepare) return f.onPrepare(*input);
        return f.api.writePrepared(f.api.context, input->builder, input->sourceBytes);
    }
    UiRefreshBindings::Contribution Capture(MmsHandle binding, uint64_t revision) {
        UiRefreshBindings::Contribution item;
        Check(bindings.Capture(client, binding, target, revision, item) == MMS_OK && item, "capture exact accepted source revision"); return item;
    }
    MmsResult Consume(const UiRefreshBindings::Contribution& item, uint64_t& revision, bool& changed) {
        return bindings.Consume(item, [&] { return ready ? MMS_OK : MMS_E_STALE; }, revision, changed);
    }
    MmsUiDataDisplayV1 Display(MmsHandle binding) {
        MmsUiDataDisplayV1 display{}; display.h = Header<MmsUiDataDisplayV1>();
        Check(api.acquireDisplay(api.context, binding, &display) == MMS_OK, "acquire actual immutable published display bytes"); return display;
    }
    void Release(MmsUiDataDisplayV1 display) { Check(api.releaseDisplay(api.context, display.lease) == MMS_OK, "release same host display lease"); }
};

void SourcesAndExactRevision() {
    Fixture f;
    MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
    Check(f.api.getCapabilities(f.api.context, &caps) == MMS_OK && caps.maxSourceBytes && caps.maxContributionsPerPlugin, "query real data quotas");
    const auto source = f.Source();
    MmsUiDataSourceDescV1 duplicate{Header<MmsUiDataSourceDescV1>(), {"status", 6}, 256, 0, 0};
    MmsHandle untouched = 999;
    Check(f.api.registerSource(f.api.context, &duplicate, &untouched) == MMS_E_CONFLICT && untouched == 999, "stable source IDs are unique per owner");
    MmsUiDataSourcePublishV1 update{Header<MmsUiDataSourcePublishV1>(), source, 0, {"first", 5}, 0, 0};
    MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>();
    MmsResult worker = MMS_E_FAILED;
    std::thread publish([&] { worker = f.api.publishSource(f.api.context, &update, &state); }); publish.join();
    Check(worker == MMS_OK && state.revision == 1 && state.projectEpoch == 1 && state.byteCount == 5, "worker publication generates a real source revision");
    Check(f.Publish(source, 1, "first").revision == 1, "identical source content does not invent another revision");
    const auto binding = f.Bind(source);
    Check(f.bindings.HasBinding(f.client, f.target) && !f.bindings.HasBinding(f.foreign, f.target), "DATA capability uses the same target identity and owner");
    auto first = f.Capture(binding, 1);
    std::string second = "second";
    auto newer = f.Publish(source, 1, second); second[0] = 'X';
    Check(newer.revision == 2, "new source content advances its revision exactly once");
    uint64_t revision = 999; bool changed = false;
    Check(f.Consume(first, revision, changed) == MMS_OK && revision == 1 && changed && f.sampled == "first", "accepted N consumes N after N+1 publication");
    auto old = f.Display(binding); Check(Bytes(old.bytes) == "first" && old.sourceRevision == 1 && old.displayRevision == 1, "published model is actual prepared data");
    UiRefreshBindings::Contribution late;
    Check(f.bindings.Capture(f.client, binding, f.target, 1, late) == MMS_E_STALE && !late, "uncaptured old revisions fail instead of reading N+1");
    auto next = f.Capture(binding, 2);
    Check(f.Consume(next, revision, changed) == MMS_OK && revision == 2 && changed && f.sampled == "second", "source storage copied caller bytes before return");
    auto current = f.Display(binding);
    Check(Bytes(current.bytes) == "second" && current.displayRevision == 2 && Bytes(old.bytes) == "first", "old paint lease remains immutable after publication");
    const auto prepares = f.prepares;
    Check(f.Consume(next, revision, changed) == MMS_OK && !changed && f.prepares == prepares, "same consumed revision does not prepare again");
    Check(f.api.writePrepared(f.api.context, f.lastBuilder, {"late", 4}) == MMS_E_STALE, "builder is invalid immediately after prepare");
    Check(f.other.releaseDisplay(f.other.context, old.lease) == MMS_E_ARGUMENT, "foreign owner cannot release display lease");
    f.Release(old); f.Release(current);
    newer = f.Publish(source, 2, "different-input");
    f.onPrepare = [&](const MmsUiDataPrepareV1& input) { return f.api.writePrepared(f.api.context, input.builder, {"second", 6}); };
    auto same = f.Capture(binding, newer.revision);
    Check(f.Consume(same, revision, changed) == MMS_OK && !changed && revision == newer.revision, "equal prepared model consumes real revision without a visual change");
    current = f.Display(binding); Check(current.displayRevision == 2 && current.sourceRevision == newer.revision, "display and source revisions have distinct meanings"); f.Release(current);
    Check(f.api.unregisterSource(f.api.context, source) == MMS_OK && !f.bindings.HasBinding(f.client, f.target), "source removal invalidates dependent bindings");
    Check(f.Consume(next, revision, changed) == MMS_E_STALE, "accepted work cannot publish after its source is unregistered");
}

void FailuresAndReadOnly() {
    Fixture f; const auto source = f.Source(); f.Publish(source, 0, "visible"); const auto binding = f.Bind(source);
    auto initial = f.Capture(binding, 1); uint64_t revision = 0; bool changed = false;
    Check(f.Consume(initial, revision, changed) == MMS_OK, "publish baseline before failure cases");
    auto old = f.Display(binding); f.Publish(source, 1, "new"); auto candidate = f.Capture(binding, 2);
    for (int mode = 0; mode < 6; ++mode) {
        f.onPrepare = [&](const MmsUiDataPrepareV1& input) -> MmsResult {
            if (mode == 0) { f.api.writePrepared(f.api.context, input.builder, {"wrong", 5}); return MMS_E_FAILED; }
            if (mode == 1) throw std::bad_alloc();
            if (mode == 2 || mode == 5) {
                failAllocation = mode == 2; failAfter = mode == 5 ? 1 : -1;
                const auto result = f.api.writePrepared(f.api.context, input.builder, {"oom", 3});
                failAllocation = false; failAfter = -1;
                Check(result == MMS_E_MEMORY, "host candidate allocation reports memory failure before or after byte reservation"); return result;
            }
            if (mode == 3) {
                Check(f.api.writePrepared(f.api.context, input.builder, {"one", 3}) == MMS_OK, "first builder write succeeds");
                Check(f.api.writePrepared(f.api.context, input.builder, {"two", 3}) == MMS_E_CONFLICT, "second write poisons the candidate"); return MMS_OK;
            }
            return MMS_OK; // A successful callback must still supply a model.
        };
        revision = 1234; changed = true;
        const auto result = f.Consume(candidate, revision, changed);
        Check(result == (mode == 1 || mode == 2 || mode == 5 ? MMS_E_MEMORY : mode == 3 ? MMS_E_CONFLICT : MMS_E_FAILED) &&
            revision == 1234 && changed && !f.readOnly, "failed prepare leaves outputs untouched and ends read-only protection");
        auto display = f.Display(binding);
        Check(display.sourceRevision == 1 && Bytes(display.bytes) == "visible" && Bytes(old.bytes) == "visible", "failure keeps the previous retained display model"); f.Release(display);
    }
    MmsUiDataSourcePublishV1 update{Header<MmsUiDataSourcePublishV1>(), source, 2, {"later", 5}, 0, 0};
    MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>(); state.revision = 8765;
    failAllocation = true; const auto publish = f.api.publishSource(f.api.context, &update, &state); failAllocation = false;
    Check(publish == MMS_E_MEMORY && state.revision == 8765, "source OOM keeps old revision and output intact");
    failAfter = 1; const auto afterReserve = f.api.publishSource(f.api.context, &update, &state); failAfter = -1;
    Check(afterReserve == MMS_E_MEMORY && state.revision == 8765, "source payload allocation failure refunds its already reserved bytes");
    f.onPrepare = [&](const MmsUiDataPrepareV1& input) {
        Check(f.api.publishSource(f.api.context, &update, &state) == MMS_E_BUSY, "prepare cannot cause another source publication on its callback chain");
        Check(f.api.unbind(f.api.context, binding) == MMS_E_BUSY && f.api.unregisterSource(f.api.context, source) == MMS_E_BUSY, "prepare cannot mutate source or binding identity");
        MmsUiDataSourceStateV1 readable{}; readable.h = Header<MmsUiDataSourceStateV1>();
        Check(f.api.getSource(f.api.context, source, &readable) == MMS_OK && readable.revision == 2, "pure source metadata remains readable in callback");
        auto display = f.Display(binding); Check(Bytes(display.bytes) == "visible", "callback reads the old model until host swap"); f.Release(display);
        return f.api.writePrepared(f.api.context, input.builder, input.sourceBytes);
    };
    Check(f.Consume(candidate, revision, changed) == MMS_OK && revision == 2 && changed, "successful guarded prepare swaps the model");
    f.Publish(source, 2, "discard"); auto stale = f.Capture(binding, 3);
    f.onPrepare = [&](const MmsUiDataPrepareV1& input) { f.ready = false; return f.api.writePrepared(f.api.context, input.builder, input.sourceBytes); };
    revision = 5678;
    Check(f.Consume(stale, revision, changed) == MMS_E_STALE && revision == 5678, "target revalidation after prepare rejects candidate before publication");
    auto display = f.Display(binding); Check(display.sourceRevision == 2 && Bytes(display.bytes) == "new", "late target failure keeps the last valid model"); f.Release(display); f.Release(old);
}

void LifetimeAndQuotas() {
    Fixture f; const auto source = f.Source(); f.Publish(source, 0, "retained"); const auto binding = f.Bind(source, false);
    auto work = f.Capture(binding, 1); uint64_t revision = 0; bool changed = false;
    Check(f.Consume(work, revision, changed) == MMS_OK && f.prepares == 0, "simple passthrough uses the same host pipeline without plugin callbacks");
    auto display = f.Display(binding);
    f.bindings.AdvanceProjectEpoch(2); f.target.projectEpoch = 2;
    Check(f.Consume(work, revision, changed) == MMS_E_STALE && Bytes(display.bytes) == "retained", "project replacement invalidates work while old paint lease stays readable");
    f.bindings.Retire(f.client);
    Check(f.bindings.HasLeases(f.client), "retired owner remains pinned by accepted contribution and external display lease");
    auto again = f.bindings.Api(f.client);
    Check(again.context == f.api.context, "querying a retired service cannot hide its outstanding owner leases");
    MmsUiDataDisplayV1 denied{}; denied.h = Header<MmsUiDataDisplayV1>();
    Check(f.api.acquireDisplay(f.api.context, binding, &denied) == MMS_E_NOT_READY, "stopping prevents new display leases");
    work.reset(); Check(f.bindings.HasLeases(f.client), "external lease alone prevents unload");
    f.Release(display); Check(!f.bindings.HasLeases(f.client), "last release permits unload");
    Fixture q;
    MmsUiDataCapabilitiesV1 caps{}; caps.h = Header<MmsUiDataCapabilitiesV1>();
    Check(q.api.getCapabilities(q.api.context, &caps) == MMS_OK, "read quota values rather than assume test limits");
    const auto s = q.Source(); q.Publish(s, 0, "q"); const auto b = q.Bind(s);
    std::vector<UiRefreshBindings::Contribution> held;
    for (uint32_t i = 0; i < caps.maxContributionsPerPlugin; ++i) held.push_back(q.Capture(b, 1));
    UiRefreshBindings::Contribution refused;
    Check(q.bindings.Capture(q.client, b, q.target, 1, refused) == MMS_E_BUSY && !refused, "accepted contributions have a real per-owner cap");
    held.pop_back(); Check(q.bindings.Capture(q.client, b, q.target, 1, refused) == MMS_OK, "releasing contribution returns its quota"); held.clear(); refused.reset();
    auto ready = q.Capture(b, 1); Check(q.Consume(ready, revision, changed) == MMS_OK, "publish before display lease quota");
    std::vector<MmsUiDataDisplayV1> displays;
    for (uint32_t i = 0; i < caps.maxDisplayLeasesPerPlugin; ++i) displays.push_back(q.Display(b));
    denied.h = Header<MmsUiDataDisplayV1>();
    Check(q.api.acquireDisplay(q.api.context, b, &denied) == MMS_E_BUSY, "unreleased paint leases remain charged");
    for (auto lease : displays) q.Release(lease);
    Fixture memory;
    std::string large(size_t(caps.maxSourceBytes), 'x');
    std::vector<MmsHandle> sources;
    const auto count = caps.maxRetainedBytesPerPlugin / caps.maxSourceBytes;
    for (uint64_t i = 0; i < count; ++i) { auto item = memory.Source("large." + std::to_string(i), caps.maxSourceBytes); memory.Publish(item, 0, large); sources.push_back(item); }
    auto extra = memory.Source("large.extra", caps.maxSourceBytes);
    MmsUiDataSourcePublishV1 update{Header<MmsUiDataSourcePublishV1>(), extra, 0, {large.data(), large.size()}, 0, 0};
    MmsUiDataSourceStateV1 output{}; output.h = Header<MmsUiDataSourceStateV1>(); output.revision = 77;
    Check(memory.api.publishSource(memory.api.context, &update, &output) == MMS_E_BUSY && output.revision == 77, "retained immutable source bytes enforce the advertised memory limit");
    Check(memory.api.unregisterSource(memory.api.context, sources.front()) == MMS_OK, "remove source to return retained byte charge");
    Check(memory.api.publishSource(memory.api.context, &update, &output) == MMS_OK && output.revision == 1, "quota refusal does not consume revision or leave charged memory");
}
void ValidationAndRunningStop() {
    Fixture f; const auto source = f.Source(); f.Publish(source, 0, "one"); const auto binding = f.Bind(source);
    MmsUiDataSourcePublishV1 update{Header<MmsUiDataSourcePublishV1>(), source, 1, {"two", 3}, 0, 0};
    MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>(); state.revision = 7654;
    Check(f.other.publishSource(f.other.context, &update, &state) == MMS_E_ARGUMENT && state.revision == 7654, "another plugin cannot publish source data");
    UiRefreshBindings::Contribution invalid;
    Check(f.bindings.Capture(f.foreign, binding, f.target, 1, invalid) == MMS_E_ARGUMENT && !invalid, "another plugin cannot consume a binding");
    auto stale = f.target; ++stale.instanceGeneration;
    Check(f.bindings.Capture(f.client, binding, stale, 1, invalid) == MMS_E_STALE && !invalid, "target instance generation is part of binding identity");
    ++update.expectedRevision;
    Check(f.api.publishSource(f.api.context, &update, &state) == MMS_E_CONFLICT && state.revision == 7654, "source publication uses exact compare revision");
    MmsUiDataBindingDescV1 desc{Header<MmsUiDataBindingDescV1>(), f.target, source, nullptr, nullptr, 256, 0, 0};
    MmsHandle output = 999;
    Check(f.api.bind(f.api.context, &desc, &output) == MMS_E_CONFLICT && output == 999, "only one live binding can own a target display model");
    MmsResult threadResult = MMS_OK;
    std::thread worker([&] { threadResult = f.api.unbind(f.api.context, binding); }); worker.join();
    Check(threadResult == MMS_E_THREAD, "worker cannot mutate binding identity");
    auto pending = f.Capture(binding, 1); uint64_t revision = 888; bool changed = false;
    f.onPrepare = [&](const MmsUiDataPrepareV1& input) {
        f.bindings.Retire(f.client);
        Check(f.bindings.HasLeases(f.client), "running callback and builder preserve DLL after stop");
        Check(f.api.writePrepared(f.api.context, input.builder, input.sourceBytes) == MMS_E_CANCELLED,
            "stopping rejects further writes to an in-flight builder");
        return MMS_OK;
    };
    Check(f.Consume(pending, revision, changed) == MMS_E_CANCELLED && revision == 888 && !f.readOnly,
        "running stop cancels the candidate and leaves output untouched");
    pending.reset(); Check(!f.bindings.HasLeases(f.client), "running stop releases builder and contribution without hidden leases");
}
// Only the refresh queue boundary is modeled here. Its request captures a real
// contribution from this endpoint's DATA store, with the original C handles.
struct SdkRefreshEndpoint {
    Fixture& fixture;
    UiRefreshBindings::Contribution pending;
    unsigned requests = 0;
    MmsHandle ticket;
    SdkRefreshEndpoint* serviceContext = this;
    MmsUiRefreshApiV1 api{};
    SdkRefreshEndpoint(Fixture& data, MmsHandle id) : fixture(data), ticket(id) {
        api.h = Header<MmsUiRefreshApiV1>(); api.context = &serviceContext;
        api.getCapabilities = [](void*, MmsUiRefreshCapabilitiesV1*) { return MMS_E_UNSUPPORTED; };
        api.getTarget = [](void*, MmsHandle, MmsUiRefreshTargetV1*) { return MMS_E_UNSUPPORTED; };
        api.request = [](void* context, const MmsUiRefreshRequestV1* request, MmsHandle* out) -> MmsResult {
            auto& endpoint = **static_cast<SdkRefreshEndpoint**>(context);
            ++endpoint.requests;
            auto& f = endpoint.fixture;
            const auto result = f.bindings.Capture(f.client, request->binding, request->target,
                request->sourceRevision, endpoint.pending);
            if (!result) *out = endpoint.ticket;
            return result;
        };
        api.query = [](void*, MmsHandle, MmsUiRefreshStateV1*) { return MMS_E_UNSUPPORTED; };
        api.cancel = [](void*, MmsHandle) { return MMS_E_UNSUPPORTED; };
        api.release = [](void* context, MmsHandle id) -> MmsResult {
            auto& endpoint = **static_cast<SdkRefreshEndpoint**>(context);
            if (id != endpoint.ticket || !endpoint.pending) return MMS_E_NOT_FOUND;
            endpoint.pending.reset(); return MMS_OK;
        };
    }
    static MmsResult MMS_CALL Query(void* context, MmsString name, uint32_t major, uint32_t minor,
        uint32_t size, void* out) {
        auto& endpoint = *static_cast<SdkRefreshEndpoint*>(context);
        if (std::string_view(name.data, name.bytes) == "mms.ui.refresh.bind")
            return Table(major, minor, size, out, endpoint.fixture.api);
        return QueryRefresh(context, name, major, minor, size, out);
    }
    static MmsResult MMS_CALL QueryRefresh(void* context, MmsString name, uint32_t major, uint32_t minor,
        uint32_t size, void* out) {
        auto& endpoint = *static_cast<SdkRefreshEndpoint*>(context);
        if (std::string_view(name.data, name.bytes) != "mms.ui.refresh") return MMS_E_NOT_FOUND;
        return Table(major, minor, size, out, endpoint.api);
    }
    MmsHostV1 Host() {
        MmsHostV1 host{}; host.h = Header<MmsHostV1>(); host.context = this; host.queryService = &Query;
        return host;
    }
};

void SdkBindingRefreshOwnership() {
    namespace sdk = mms::sdk;
    Fixture a, b;
    SdkRefreshEndpoint endpointA(a, 101), endpointB(b, 202);
    auto hostA = endpointA.Host(), copiedHostA = hostA, hostB = endpointB.Host();
    sdk::DataServices dataA, dataB;
    sdk::RefreshServices refreshA, refreshB, unready, anotherQuery;
    Check(sdk::DataServices::Query(hostA, dataA) == MMS_OK &&
        sdk::RefreshServices::Query(copiedHostA, refreshA) == MMS_OK &&
        dataA.api().context != refreshA.api().context && hostA.context != refreshA.api().context,
        "host negotiation and both service contexts can use distinct pointers");
    Check(sdk::DataServices::Query(hostB, dataB) == MMS_OK && sdk::RefreshServices::Query(hostB, refreshB) == MMS_OK,
        "negotiate an independent client's services through the same dispatcher");
    copiedHostA.queryService = &SdkRefreshEndpoint::QueryRefresh;
    Check(sdk::RefreshServices::Query(copiedHostA, anotherQuery) == MMS_OK &&
        anotherQuery.api().context == refreshA.api().context, "different host query callbacks can expose the same service context");
    sdk::DataSource sourceA, sourceB;
    sdk::DataBinding bindingA, bindingB;
    MmsUiDataSourceStateV1 revisionA{}, revisionB{};
    Check(dataA.RegisterSource(sdk::Text("status"), 64, sourceA) == MMS_OK &&
        dataB.RegisterSource(sdk::Text("status"), 64, sourceB) == MMS_OK &&
        sourceA.Publish({"A", 1}, 0, revisionA) == MMS_OK &&
        sourceB.Publish({"B", 1}, 0, revisionB) == MMS_OK &&
        dataA.Bind(a.target, sourceA, 64, bindingA) == MMS_OK &&
        dataB.Bind(b.target, sourceB, 64, bindingB) == MMS_OK, "establish real DATA bindings on two independent hosts");
    Check(bindingA.get() == bindingB.get() && revisionA.revision == revisionB.revision && SameTarget(a.target, b.target),
        "binding, source revision and target values collide across independent hosts");
    sdk::RefreshTicket ticket;
    Check(bindingA.Refresh(refreshB, a.target, revisionA.revision, ticket) == MMS_E_ARGUMENT && !ticket &&
        !endpointB.requests && !endpointB.pending, "foreign host context cannot submit the colliding binding");
    Check(bindingA.Refresh(anotherQuery, a.target, revisionA.revision, ticket) == MMS_E_ARGUMENT && !ticket &&
        !endpointA.requests && !endpointA.pending, "foreign query callback is rejected even with matching contexts");
    Check(bindingA.Refresh(unready, a.target, revisionA.revision, ticket) == MMS_E_NOT_READY && !ticket,
        "unnegotiated refresh service stays not ready");
    sdk::DataBinding moved(std::move(bindingA));
    Check(bindingA.Refresh(refreshB, a.target, revisionA.revision, ticket) == MMS_E_NOT_READY && !ticket,
        "moved-from binding stays not ready before host checks");
    auto refreshCopy = refreshA;
    Check(sdk::RefreshServices::Query(hostA, refreshCopy) == MMS_OK, "same client can negotiate again");
    MmsResult workerResult = MMS_E_FAILED;
    MmsResult workerRelease = MMS_OK;
    std::thread worker([&] {
        workerRelease = moved.Release();
        workerResult = moved.Refresh(refreshCopy, a.target, revisionA.revision, ticket);
    });
    worker.join();
    Check(workerRelease == MMS_E_THREAD && workerResult == MMS_OK && ticket.get() == endpointA.ticket && endpointA.requests == 1 && endpointA.pending,
        "failed release retains the moved binding and its re-queried service still works on a worker");
    Check(bindingB.Refresh(refreshA, b.target, revisionB.revision, ticket) == MMS_E_ARGUMENT &&
        ticket.get() == endpointA.ticket && endpointA.requests == 1 && endpointA.pending,
        "rejected foreign refresh leaves an occupied ticket with its original owner");
    Check(moved.Refresh(refreshA, a.target, revisionA.revision, ticket) == MMS_E_BUSY &&
        ticket.get() == endpointA.ticket && endpointA.requests == 1, "same-host output busy still bypasses the request");
    Check(ticket.Release() == MMS_OK && !endpointA.pending, "original service still releases the ticket");
    const auto priorRevision = revisionA.revision;
    Check(sourceA.Publish({"new", 3}, priorRevision, revisionA) == MMS_OK &&
        moved.Refresh(refreshA, a.target, priorRevision, ticket) == MMS_E_STALE && !ticket && endpointA.requests == 2,
        "same-host stale source revision still comes from the host");
    const auto detached = moved.detach();
    Check(detached && a.api.unbind(a.api.context, detached) == MMS_OK &&
        moved.Refresh(refreshA, a.target, revisionA.revision, ticket) == MMS_E_NOT_READY && !ticket,
        "detach leaves an empty wrapper and transfers the original handle");
    Check(bindingB.Release() == MMS_OK && dataB.Bind(b.target, sourceB, 64, moved) == MMS_OK &&
        moved.Refresh(refreshB, b.target, revisionB.revision, ticket) == MMS_OK && ticket.get() == endpointB.ticket,
        "rebinding an empty wrapper replaces the previous owner");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SourcesAndExactRevision(); FailuresAndReadOnly(); LifetimeAndQuotas(); ValidationAndRunningStop();
    SdkBindingRefreshOwnership();
    std::printf("PASS UI DATA binding: %u checks\n", checks);
}
