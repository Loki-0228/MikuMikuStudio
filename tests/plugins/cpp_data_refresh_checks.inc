#include "ui_refresh_binding.hpp"
#include <mms/cpp/data_refresh.hpp>
#include <type_traits>

namespace cpp_data_refresh_checks {
namespace sdk = mms::sdk;
static MmsBytes Bytes(const char* value) { return {value, std::strlen(value)}; }
static bool Equal(MmsBytes bytes, const char* value) {
    return bytes.bytes == std::strlen(value) && (!bytes.bytes || std::memcmp(bytes.data, value, bytes.bytes) == 0);
}
static bool SameTarget(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
// Only negotiation and explicit failure injection are wrapped. Every successful
// operation reaches the fixture's real registry, bindings and refresh queue.
struct Host {
    refresh_checks::Fixture fixture;
    MmsUiRefreshBindingApiV1 rawData = fixture.queue.Bindings().Api(fixture.client);
    MmsUiRefreshBindingApiV1 foreignData = fixture.queue.Bindings().Api(fixture.other);
    MmsUiRefreshBindingApiV1 data = rawData;
    MmsUiRefreshApiV1 refresh = fixture.api;
    MmsHostV1 host{}, foreignHost{};
    MmsResult negotiation = MMS_OK, sourceRelease = MMS_OK, bindingRelease = MMS_OK, displayRelease = MMS_OK;
    bool arguments = true, throwQuery = false;
    unsigned bindCalls = 0, requestCalls = 0, displayReleases = 0;
    MmsUiDataBindingDescV1 lastBinding{};
    MmsUiRefreshRequestV1 lastRequest{};
    Host() {
        data.context = this;
        data.getCapabilities = [](void* p, MmsUiDataCapabilitiesV1* out) {
            auto& x = *static_cast<Host*>(p); return x.rawData.getCapabilities(x.rawData.context, out);
        };
        data.registerSource = [](void* p, const MmsUiDataSourceDescV1* desc, MmsHandle* out) {
            auto& x = *static_cast<Host*>(p); return x.rawData.registerSource(x.rawData.context, desc, out);
        };
        data.publishSource = [](void* p, const MmsUiDataSourcePublishV1* update, MmsUiDataSourceStateV1* out) {
            auto& x = *static_cast<Host*>(p); return x.rawData.publishSource(x.rawData.context, update, out);
        };
        data.getSource = [](void* p, MmsHandle source, MmsUiDataSourceStateV1* out) {
            auto& x = *static_cast<Host*>(p); return x.rawData.getSource(x.rawData.context, source, out);
        };
        data.unregisterSource = [](void* p, MmsHandle source) {
            auto& x = *static_cast<Host*>(p); if (x.sourceRelease) return x.sourceRelease;
            return x.rawData.unregisterSource(x.rawData.context, source);
        };
        data.bind = [](void* p, const MmsUiDataBindingDescV1* desc, MmsHandle* out) {
            auto& x = *static_cast<Host*>(p); ++x.bindCalls; if (desc) x.lastBinding = *desc;
            return x.rawData.bind(x.rawData.context, desc, out);
        };
        data.unbind = [](void* p, MmsHandle binding) {
            auto& x = *static_cast<Host*>(p); if (x.bindingRelease) return x.bindingRelease;
            return x.rawData.unbind(x.rawData.context, binding);
        };
        data.writePrepared = [](void* p, MmsHandle builder, MmsBytes bytes) {
            auto& x = *static_cast<Host*>(p); return x.rawData.writePrepared(x.rawData.context, builder, bytes);
        };
        data.acquireDisplay = [](void* p, MmsHandle binding, MmsUiDataDisplayV1* out) {
            auto& x = *static_cast<Host*>(p); return x.rawData.acquireDisplay(x.rawData.context, binding, out);
        };
        data.releaseDisplay = [](void* p, MmsHandle lease) {
            auto& x = *static_cast<Host*>(p); ++x.displayReleases; if (x.displayRelease) return x.displayRelease;
            return x.rawData.releaseDisplay(x.rawData.context, lease);
        };
        refresh.context = this;
        refresh.getCapabilities = [](void* p, MmsUiRefreshCapabilitiesV1* out) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.getCapabilities(x.fixture.api.context, out);
        };
        refresh.getTarget = [](void* p, MmsHandle target, MmsUiRefreshTargetV1* out) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.getTarget(x.fixture.api.context, target, out);
        };
        refresh.request = [](void* p, const MmsUiRefreshRequestV1* request, MmsHandle* out) {
            auto& x = *static_cast<Host*>(p); ++x.requestCalls; if (request) x.lastRequest = *request;
            return x.fixture.api.request(x.fixture.api.context, request, out);
        };
        refresh.query = [](void* p, MmsHandle ticket, MmsUiRefreshStateV1* out) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.query(x.fixture.api.context, ticket, out);
        };
        refresh.cancel = [](void* p, MmsHandle ticket) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.cancel(x.fixture.api.context, ticket);
        };
        refresh.release = [](void* p, MmsHandle ticket) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.release(x.fixture.api.context, ticket);
        };
        host.h = Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p);
            if (x.throwQuery) throw std::bad_alloc();
            const auto service = String(name);
            if (service == "mms.ui.refresh.bind") {
                x.arguments &= major == 1 && minor == 0 && size == sizeof(x.data);
                if (x.negotiation) return x.negotiation;
                std::memcpy(out, &x.data, sizeof(x.data)); return MMS_OK;
            }
            if (service == "mms.ui.refresh") {
                x.arguments &= major == 1 && minor == 0 && size == sizeof(x.refresh);
                std::memcpy(out, &x.refresh, sizeof(x.refresh)); return MMS_OK;
            }
            return MMS_E_NOT_FOUND;
        };
        foreignHost.h = Header<MmsHostV1>(); foreignHost.context = this;
        foreignHost.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p);
            if (String(name) != "mms.ui.refresh.bind") return MMS_E_NOT_FOUND;
            if (major != 1 || minor || size != sizeof(x.foreignData)) return MMS_E_VERSION;
            std::memcpy(out, &x.foreignData, sizeof(x.foreignData)); return MMS_OK;
        };
    }
    sdk::DataServices Data() {
        sdk::DataServices result; CHECK(sdk::DataServices::Query(host, result) == MMS_OK); return result;
    }
    sdk::RefreshServices Refresh() {
        sdk::RefreshServices result; CHECK(sdk::RefreshServices::Query(host, result) == MMS_OK); return result;
    }
};
static void Negotiation() {
    static_assert(!std::is_copy_constructible_v<sdk::DataSource> && !std::is_copy_constructible_v<sdk::DataBinding> && !std::is_copy_constructible_v<sdk::DataDisplay>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::DataSource> && std::is_nothrow_move_constructible_v<sdk::DataBinding> && std::is_nothrow_move_constructible_v<sdk::DataDisplay>);
    static_assert(!std::is_move_assignable_v<sdk::DataSource> && !std::is_move_assignable_v<sdk::DataBinding> && !std::is_move_assignable_v<sdk::DataDisplay>);
    Host x; sdk::DataServices empty; sdk::DataSource source; sdk::DataBinding binding; sdk::DataDisplay display;
    MmsUiDataCapabilitiesV1 caps{}; caps.maxSourcesPerPlugin = 777;
    MmsUiDataSourceStateV1 state{}; state.revision = 777;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY);
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && caps.maxSourcesPerPlugin == 777);
    CHECK(empty.RegisterSource(S("example.refresh.empty"), 64, source) == MMS_E_NOT_READY && !source);
    CHECK(source.State(state) == MMS_E_NOT_READY && state.revision == 777);
    CHECK(source.Publish(Bytes("none"), 0, state) == MMS_E_NOT_READY && state.revision == 777);
    CHECK(binding.Acquire(display) == MMS_E_NOT_READY && !display);
    CHECK(source.Release() == MMS_OK && binding.Release() == MMS_OK && display.Release() == MMS_OK);
    CHECK(!source.detach() && !binding.detach() && !display.detach().lease);
    auto service = x.Data(); CHECK(service.ready() && x.arguments && service.api().context == &x);
    const auto context = service.api().context;
    for (auto error : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY}) {
        x.negotiation = error; CHECK(sdk::DataServices::Query(x.host, service) == error && service.api().context == context);
    }
    x.negotiation = MMS_OK; x.throwQuery = true;
    CHECK(sdk::DataServices::Query(x.host, service) == MMS_E_MEMORY && service.api().context == context);
    x.throwQuery = false;
    auto host = x.host; host.h.size = 1; CHECK(sdk::DataServices::Query(host, empty) == MMS_E_VERSION && !empty.ready());
    host = x.host; host.h.major = 2; CHECK(sdk::DataServices::Query(host, empty) == MMS_E_VERSION && !empty.ready());
    host = x.host; host.queryService = nullptr; CHECK(sdk::DataServices::Query(host, empty) == MMS_E_ARGUMENT && !empty.ready());
    const auto table = x.data;
    x.data.h.size = sizeof(table) - 1; CHECK(sdk::DataServices::Query(x.host, service) == MMS_E_VERSION && service.api().context == context);
    x.data = table; x.data.h.major = 2; CHECK(sdk::DataServices::Query(x.host, service) == MMS_E_VERSION && service.api().context == context);
    x.data = table; x.data.h.size += 16; x.data.h.minor = 3;
    CHECK(sdk::DataServices::Query(x.host, empty) == MMS_OK && empty.ready());
    for (unsigned field = 0; field < 10; ++field) {
        x.data = table;
        switch (field) {
        case 0: x.data.getCapabilities = nullptr; break; case 1: x.data.registerSource = nullptr; break;
        case 2: x.data.publishSource = nullptr; break; case 3: x.data.getSource = nullptr; break;
        case 4: x.data.unregisterSource = nullptr; break; case 5: x.data.bind = nullptr; break;
        case 6: x.data.unbind = nullptr; break; case 7: x.data.writePrepared = nullptr; break;
        case 8: x.data.acquireDisplay = nullptr; break; case 9: x.data.releaseDisplay = nullptr; break;
        }
        CHECK(sdk::DataServices::Query(x.host, service) == MMS_E_UNSUPPORTED && service.api().context == context);
    }
    x.data = table;
    CHECK(service.Capabilities(caps) == MMS_OK && caps.maxSourcesPerPlugin && caps.maxSourceBytes);
    MmsResult threadCheck = MMS_OK, threadCaps = MMS_OK, threadRegister = MMS_OK, threadPrepare = MMS_OK;
    std::thread worker([&] {
        threadCheck = service.CheckThread(); threadCaps = service.Capabilities(caps);
        threadRegister = service.RegisterSource(S("example.refresh.worker"), 64, source);
        threadPrepare = service.WritePrepared(1, Bytes("value"));
    }); worker.join();
    CHECK(threadCheck == MMS_E_THREAD && threadCaps == MMS_E_THREAD && threadRegister == MMS_E_THREAD && threadPrepare == MMS_E_THREAD && !source);
}
static void PipelineAndImmutableLeases() {
    Host x; auto data = x.Data(); auto refresh = x.Refresh(); const auto target = x.fixture.Target();
    sdk::DataSource source; CHECK(data.RegisterSource(S("example.refresh.data"), 64, source) == MMS_OK && source);
    MmsUiDataSourceStateV1 initial{}; CHECK(source.State(initial) == MMS_OK && initial.source == source.get() && initial.revision == 0 && !initial.byteCount);
    MmsUiDataSourceStateV1 first{}, same{}, conflict{}; conflict.revision = 777;
    MmsResult publish = MMS_E_FAILED, sameResult = MMS_E_FAILED, conflictResult = MMS_E_FAILED;
    char input[] = "first";
    std::thread worker([&] {
        publish = source.Publish({input, 5}, 0, first);
        sameResult = source.Publish({input, 5}, first.revision, same);
        conflictResult = source.Publish(Bytes("stale"), 0, conflict);
    }); worker.join();
    CHECK(publish == MMS_OK && first.revision == 1 && first.source == source.get() && first.byteCount == 5);
    CHECK(sameResult == MMS_OK && same.revision == first.revision && same.projectEpoch == target.target.projectEpoch);
    CHECK(conflictResult == MMS_E_CONFLICT && conflict.revision == 777);
    input[0] = 'X';
    sdk::DataBinding binding; CHECK(data.Bind(target.target, source, 64, binding) == MMS_OK && binding);
    CHECK(x.bindCalls == 1 && SameTarget(x.lastBinding.target, target.target) && x.lastBinding.source == source.get());
    CHECK(data.Bind(target.target, source, 64, binding) == MMS_E_BUSY && x.bindCalls == 1);
    sdk::DataDisplay early; CHECK(binding.Acquire(early) == MMS_E_NOT_READY && !early);
    sdk::RefreshTicket ticket;
    CHECK(binding.Refresh(refresh, target.target, first.revision, ticket) == MMS_OK && ticket);
    CHECK(x.requestCalls == 1 && x.lastRequest.binding == binding.get() && SameTarget(x.lastRequest.target, target.target));
    CHECK(x.lastRequest.sourceRevision == first.revision && x.lastRequest.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT));
    MmsUiRefreshStateV1 pending{}; CHECK(ticket.Query(pending) == MMS_OK && pending.status == MMS_UI_REFRESH_PENDING);
    // An accepted ticket keeps exactly revision 1 even when a worker publishes 2.
    MmsUiDataSourceStateV1 second{};
    std::thread newer([&] { publish = source.Publish(Bytes("second"), first.revision, second); }); newer.join();
    CHECK(publish == MMS_OK && second.revision == first.revision + 1);
    x.fixture.queue.Drain(); MmsUiRefreshStateV1 done{};
    CHECK(ticket.Query(done) == MMS_OK && done.status == MMS_UI_REFRESH_COMPLETED && done.result == MMS_OK && done.sourceRevision == first.revision);
    CHECK(done.completedKinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT));
    const auto low = x.fixture.State(ticket.get()); CHECK(low.sourceRevision == done.sourceRevision && low.status == done.status);
    CHECK(ticket.Release() == MMS_OK);
    sdk::DataDisplay old; CHECK(binding.Acquire(old) == MMS_OK && old);
    CHECK(old.value().binding == binding.get() && old.value().source == source.get() && old.value().sourceRevision == first.revision);
    CHECK(Equal(old.bytes(), "first") && old.bytes().data != input);
    const auto oldPointer = old.bytes().data;
    const auto oldLease = old.value().lease;
    CHECK(binding.Acquire(old) == MMS_E_BUSY && old.value().lease == oldLease && old.bytes().data == oldPointer);
    CHECK(binding.Refresh(refresh, target.target, second.revision, ticket, MMS_UI_REFRESH_DATA, 91) == MMS_OK);
    x.fixture.queue.Drain(); CHECK(ticket.Query(done) == MMS_OK && done.completedKinds == MMS_UI_REFRESH_DATA && done.sourceRevision == second.revision);
    sdk::DataDisplay current; CHECK(binding.Acquire(current) == MMS_OK && Equal(current.bytes(), "second"));
    CHECK(current.value().sourceRevision == second.revision && current.value().displayRevision > old.value().displayRevision);
    CHECK(Equal(old.bytes(), "first") && old.bytes().data == oldPointer);
    CHECK(ticket.Release() == MMS_OK && current.Release() == MMS_OK && old.Release() == MMS_OK);
    CHECK(binding.Release() == MMS_OK && source.Release() == MMS_OK && !x.fixture.queue.HasLeases(x.fixture.client));
}
static void PrepareAndOwnership() {
    Host x; auto data = x.Data(); auto refresh = x.Refresh(); const auto target = x.fixture.Target(x.fixture.panelElement);
    sdk::DataSource source; CHECK(data.RegisterSource(S("example.refresh.prepared"), 64, source) == MMS_OK);
    MmsUiDataSourceStateV1 state{}; CHECK(source.Publish(Bytes("source"), 0, state) == MMS_OK);
    struct Prepare {
        sdk::DataServices* services; MmsUiTargetRefV1 target; MmsHandle source; unsigned calls = 0;
        bool arguments = true; MmsResult nestedPublish = MMS_OK; sdk::DataSource* owned;
    } prepare{&data, target.target, source.get(), 0, true, MMS_OK, &source};
    sdk::DataBinding binding;
    CHECK(data.Bind(target.target, source, 64, binding, [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
        auto& x = *static_cast<Prepare*>(p); ++x.calls;
        x.arguments &= input && SameTarget(input->target, x.target) && input->source == x.source &&
            input->sourceRevision == 1 && Equal(input->sourceBytes, "source") && input->builder != 0 && input->previousRevision == 0;
        MmsUiDataSourceStateV1 unused{}; x.nestedPublish = x.owned->Publish(Bytes("recursive"), 1, unused);
        char prepared[] = "display";
        const auto result = x.services->WritePrepared(input->builder, {prepared, 7});
        prepared[0] = 'X'; return result;
    }, &prepare) == MMS_OK);
    const auto sourceHandle = source.get(), bindingHandle = binding.get();
    sdk::RefreshTicket ticket; CHECK(binding.Refresh(refresh, target.target, state.revision, ticket) == MMS_OK);
    x.fixture.queue.Drain(); MmsUiRefreshStateV1 done{};
    CHECK(ticket.Query(done) == MMS_OK && done.status == MMS_UI_REFRESH_COMPLETED && done.result == MMS_OK);
    CHECK(prepare.calls == 1 && prepare.arguments && prepare.nestedPublish == MMS_E_BUSY);
    sdk::DataDisplay display; CHECK(binding.Acquire(display) == MMS_OK && Equal(display.bytes(), "display"));
    const auto lease = display.value().lease;
    sdk::DataDisplay movedDisplay(std::move(display)); CHECK(!display && movedDisplay.value().lease == lease && Equal(movedDisplay.bytes(), "display"));
    x.displayRelease = MMS_E_FAILED;
    CHECK(movedDisplay.Release() == MMS_E_FAILED && movedDisplay.value().lease == lease && Equal(movedDisplay.bytes(), "display"));
    x.displayRelease = MMS_OK;
    auto rawDisplay = movedDisplay.detach(); CHECK(!movedDisplay && rawDisplay.lease == lease && Equal(rawDisplay.bytes, "display"));
    CHECK(x.rawData.releaseDisplay(x.rawData.context, rawDisplay.lease) == MMS_OK);
    sdk::DataBinding movedBinding(std::move(binding)); CHECK(!binding && movedBinding.get() == bindingHandle);
    sdk::DataSource movedSource(std::move(source)); CHECK(!source && movedSource.get() == sourceHandle);
    x.bindingRelease = MMS_E_BUSY; CHECK(movedBinding.Release() == MMS_E_BUSY && movedBinding.get() == bindingHandle);
    x.bindingRelease = MMS_OK;
    const auto rawBinding = movedBinding.detach(); CHECK(!movedBinding && rawBinding == bindingHandle);
    CHECK(x.rawData.unbind(x.rawData.context, rawBinding) == MMS_OK);
    x.sourceRelease = MMS_E_FAILED; CHECK(movedSource.Release() == MMS_E_FAILED && movedSource.get() == sourceHandle);
    x.sourceRelease = MMS_OK;
    const auto rawSource = movedSource.detach(); CHECK(!movedSource && rawSource == sourceHandle);
    CHECK(x.rawData.unregisterSource(x.rawData.context, rawSource) == MMS_OK);
    CHECK(ticket.Release() == MMS_OK && !x.fixture.queue.HasLeases(x.fixture.client));
}
static void OwnersEpochAndStop() {
    Host x; auto data = x.Data(); auto refresh = x.Refresh(); const auto target = x.fixture.Target();
    sdk::DataServices foreign; CHECK(sdk::DataServices::Query(x.foreignHost, foreign) == MMS_OK);
    sdk::DataSource source, foreignSource;
    CHECK(data.RegisterSource(S("example.refresh.owner"), 64, source) == MMS_OK);
    CHECK(foreign.RegisterSource(S("example.foreign.owner"), 64, foreignSource) == MMS_OK);
    sdk::DataBinding binding;
    CHECK(data.Bind(target.target, foreignSource, 64, binding) == MMS_E_ARGUMENT && !binding && !x.bindCalls);
    CHECK(foreign.Bind(target.target, source, 64, binding) == MMS_E_ARGUMENT && !binding);
    // Bypassing the SDK cannot use another owner's source either.
    MmsUiDataBindingDescV1 desc{}; desc.h = Header<MmsUiDataBindingDescV1>(); desc.target = target.target;
    desc.source = foreignSource.get(); desc.maxDisplayBytes = 64; MmsHandle rawBinding = 777;
    CHECK(x.rawData.bind(x.rawData.context, &desc, &rawBinding) == MMS_E_ARGUMENT && rawBinding == 777);
    MmsUiDataSourceStateV1 state{}; state.h = Header<MmsUiDataSourceStateV1>(); state.revision = 777;
    CHECK(x.foreignData.getSource(x.foreignData.context, source.get(), &state) == MMS_E_ARGUMENT && state.revision == 777);
    // A distinct provider is rejected before either real queue sees a binding.
    {
        Host separate; auto otherService = separate.Data(); sdk::DataBinding wrong;
        CHECK(otherService.Bind(target.target, source, 64, wrong) == MMS_E_ARGUMENT && !wrong && !separate.bindCalls);
    }
    CHECK(foreignSource.Release() == MMS_OK);
    CHECK(source.Publish(Bytes("before epoch"), 0, state) == MMS_OK);
    CHECK(data.Bind(target.target, source, 64, binding) == MMS_OK);
    sdk::RefreshTicket ticket; CHECK(binding.Refresh(refresh, target.target, state.revision, ticket) == MMS_OK);
    x.fixture.queue.Drain(); sdk::DataDisplay lease; CHECK(binding.Acquire(lease) == MMS_OK);
    const auto pointer = lease.bytes().data; CHECK(ticket.Release() == MMS_OK);
    x.fixture.queue.AdvanceProjectEpoch();
    sdk::DataDisplay unavailable; CHECK(binding.Acquire(unavailable) == MMS_E_STALE && !unavailable);
    state.revision = 777; CHECK(source.State(state) == MMS_E_STALE && state.revision == 777);
    CHECK(source.Publish(Bytes("stale"), 1, state) == MMS_E_STALE && state.revision == 777);
    CHECK(binding.Refresh(refresh, target.target, 1, ticket) == MMS_E_STALE && !ticket);
    CHECK(Equal(lease.bytes(), "before epoch") && lease.bytes().data == pointer && x.fixture.queue.HasLeases(x.fixture.client));
    CHECK(binding.Release() == MMS_OK && !binding && source.Release() == MMS_OK && !source);
    CHECK(lease.Release() == MMS_OK && !x.fixture.queue.HasLeases(x.fixture.client));
    const auto current = x.fixture.Target();
    CHECK(data.RegisterSource(S("example.refresh.stop"), 64, source) == MMS_OK);
    CHECK(source.Publish(Bytes("survives stop"), 0, state) == MMS_OK);
    CHECK(data.Bind(current.target, source, 64, binding) == MMS_OK);
    CHECK(binding.Refresh(refresh, current.target, state.revision, ticket) == MMS_OK);
    x.fixture.queue.Drain(); CHECK(binding.Acquire(lease) == MMS_OK && ticket.Release() == MMS_OK);
    // Pending tickets stop; an already acquired immutable display pins the DLL.
    CHECK(binding.Refresh(refresh, current.target, state.revision, ticket) == MMS_OK);
    x.fixture.queue.Retire(x.fixture.client);
    MmsUiRefreshStateV1 cancelled{}; CHECK(ticket.Query(cancelled) == MMS_OK && cancelled.status == MMS_UI_REFRESH_CANCELLED);
    CHECK(binding.Acquire(unavailable) == MMS_E_NOT_READY && !unavailable);
    CHECK(source.State(state) == MMS_E_NOT_READY && source.Publish(Bytes("late"), 1, state) == MMS_E_NOT_READY);
    CHECK(Equal(lease.bytes(), "survives stop") && x.fixture.queue.HasLeases(x.fixture.client));
    CHECK(ticket.Release() == MMS_OK && binding.Release() == MMS_OK && source.Release() == MMS_OK);
    CHECK(x.fixture.queue.HasLeases(x.fixture.client));
    MmsResult released = MMS_E_FAILED;
    std::thread worker([&] { released = lease.Release(); }); worker.join();
    CHECK(released == MMS_OK && !lease && !x.fixture.queue.HasLeases(x.fixture.client));
}
} // namespace cpp_data_refresh_checks

static void CppDataRefreshChecks() {
    cpp_data_refresh_checks::Negotiation();
    cpp_data_refresh_checks::PipelineAndImmutableLeases();
    cpp_data_refresh_checks::PrepareAndOwnership();
    cpp_data_refresh_checks::OwnersEpochAndStop();
}
