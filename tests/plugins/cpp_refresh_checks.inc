#include <mms/cpp/refresh.hpp>
#include <type_traits>

namespace cpp_refresh_checks {
namespace sdk = mms::sdk;
struct Host {
    refresh_checks::Fixture fixture;
    MmsUiRefreshApiV1 table = fixture.api;
    MmsHostV1 host{};
    MmsResult negotiation = MMS_OK, releaseResult = MMS_OK, cancelResult = MMS_OK;
    unsigned requests = 0, releases = 0;
    bool arguments = true, throwQuery = false;
    Host() {
        table.context = this;
        table.getCapabilities = [](void* p, MmsUiRefreshCapabilitiesV1* out) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.getCapabilities(x.fixture.api.context, out);
        };
        table.getTarget = [](void* p, MmsHandle h, MmsUiRefreshTargetV1* out) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.getTarget(x.fixture.api.context, h, out);
        };
        table.request = [](void* p, const MmsUiRefreshRequestV1* r, MmsHandle* out) {
            auto& x = *static_cast<Host*>(p); ++x.requests; return x.fixture.api.request(x.fixture.api.context, r, out);
        };
        table.query = [](void* p, MmsHandle h, MmsUiRefreshStateV1* out) {
            auto& x = *static_cast<Host*>(p); return x.fixture.api.query(x.fixture.api.context, h, out);
        };
        table.cancel = [](void* p, MmsHandle h) {
            auto& x = *static_cast<Host*>(p); if (x.cancelResult) return x.cancelResult;
            return x.fixture.api.cancel(x.fixture.api.context, h);
        };
        table.release = [](void* p, MmsHandle h) {
            auto& x = *static_cast<Host*>(p); ++x.releases; if (x.releaseResult) return x.releaseResult;
            return x.fixture.api.release(x.fixture.api.context, h);
        };
        host.h = Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p);
            if (x.throwQuery) throw std::bad_alloc();
            x.arguments &= String(name) == "mms.ui.refresh" && major == 1 && minor == 0 && size == sizeof(x.table);
            if (x.negotiation) return x.negotiation;
            std::memcpy(out, &x.table, sizeof(x.table)); return MMS_OK;
        };
    }
    sdk::RefreshServices Services() {
        sdk::RefreshServices result; CHECK(sdk::RefreshServices::Query(host, result) == MMS_OK); return result;
    }
};
static void Negotiation() {
    static_assert(!std::is_copy_constructible_v<sdk::RefreshTicket>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::RefreshTicket>);
    static_assert(!std::is_move_assignable_v<sdk::RefreshTicket>);
    Host x; sdk::RefreshServices empty; sdk::RefreshTicket ticket;
    MmsUiRefreshTargetV1 target{}; target.geometryRevision = 777;
    MmsUiRefreshCapabilitiesV1 caps{}; caps.maxTicketsPerPlugin = 777;
    MmsUiRefreshStateV1 state{}; state.status = 777;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY);
    CHECK(empty.Target(x.fixture.element, target) == MMS_E_NOT_READY && target.geometryRevision == 777);
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && caps.maxTicketsPerPlugin == 777);
    CHECK(empty.Repaint(target, ticket) == MMS_E_NOT_READY && !ticket);
    CHECK(ticket.Query(state) == MMS_E_NOT_READY && state.status == 777);
    CHECK(ticket.Cancel() == MMS_E_NOT_READY && ticket.Release() == MMS_OK && ticket.CancelAndRelease() == MMS_OK && !ticket.detach());
    auto service = x.Services(); CHECK(service.ready() && x.arguments && service.api().context == &x);
    const auto original = service.api().context;
    for (auto error : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY}) {
        x.negotiation = error; CHECK(sdk::RefreshServices::Query(x.host, service) == error && service.api().context == original);
    }
    x.negotiation = MMS_OK;
    x.throwQuery = true;
    CHECK(sdk::RefreshServices::Query(x.host, service) == MMS_E_MEMORY && service.api().context == original);
    x.throwQuery = false;
    auto host = x.host; host.h.size = 1; CHECK(sdk::RefreshServices::Query(host, empty) == MMS_E_VERSION && !empty.ready());
    host = x.host; host.h.major = 2; CHECK(sdk::RefreshServices::Query(host, empty) == MMS_E_VERSION);
    host = x.host; host.queryService = nullptr; CHECK(sdk::RefreshServices::Query(host, empty) == MMS_E_ARGUMENT);
    auto table = x.table;
    x.table.h.size = sizeof(table) - 1; CHECK(sdk::RefreshServices::Query(x.host, empty) == MMS_E_VERSION);
    x.table = table; x.table.h.major = 2; CHECK(sdk::RefreshServices::Query(x.host, empty) == MMS_E_VERSION);
    x.table = table; x.table.h.size += 16; x.table.h.minor = 3;
    CHECK(sdk::RefreshServices::Query(x.host, empty) == MMS_OK && empty.ready());
    for (int field = 0; field < 6; ++field) {
        x.table = table;
        switch (field) {
        case 0: x.table.getCapabilities = nullptr; break; case 1: x.table.getTarget = nullptr; break;
        case 2: x.table.request = nullptr; break; case 3: x.table.query = nullptr; break;
        case 4: x.table.cancel = nullptr; break; case 5: x.table.release = nullptr; break;
        }
        CHECK(sdk::RefreshServices::Query(x.host, service) == MMS_E_UNSUPPORTED && service.api().context == original);
    }
    CHECK(service.Target(x.fixture.element, target) == MMS_OK && target.target.element == x.fixture.element);
    CHECK(service.Capabilities(caps) == MMS_OK && caps.supportedKinds == (MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_DATA));
    MmsResult threadCheck = MMS_OK, threadTarget = MMS_OK, threadCaps = MMS_OK, threadSimple = MMS_OK;
    std::thread worker([&] {
        threadCheck = service.CheckThread(); threadTarget = service.Target(x.fixture.element, target);
        threadCaps = service.Capabilities(caps);
        threadSimple = service.RepaintElement(x.fixture.element, ticket);
    }); worker.join();
    CHECK(threadCheck == MMS_E_THREAD && threadTarget == MMS_E_THREAD && threadCaps == MMS_E_THREAD && threadSimple == MMS_E_THREAD && !ticket);
}
static void OwnershipAndWorkers() {
    Host x; auto service = x.Services(); MmsUiRefreshTargetV1 target{};
    CHECK(service.Target(x.fixture.element, target) == MMS_OK);
    MmsUiDirtyRectV1 rect{2, 3, 7, 5}; sdk::RefreshTicket ticket;
    CHECK(service.RepaintRect(target, rect, ticket, 71) == MMS_OK && ticket);
    auto raw = ticket.get(); CHECK(raw && x.requests == 1);
    CHECK(service.Repaint(target, ticket) == MMS_E_BUSY && ticket.get() == raw && x.requests == 1);
    MmsUiRefreshStateV1 state{}; CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_PENDING);
    sdk::RefreshTicket moved(std::move(ticket)); CHECK(!ticket && moved.get() == raw && ticket.Release() == MMS_OK);
    // Same handle, same state, and the actual host queue consumes the request.
    x.fixture.queue.Drain(); CHECK(moved.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED);
    refresh_checks::Region(x.fixture.button, {{2,3,9,8}}); x.fixture.Clean();
    MmsUiRefreshStateV1 low{}; low.h = Header<MmsUiRefreshStateV1>();
    CHECK(service.api().query(service.api().context, raw, &low) == MMS_OK && low.status == state.status);
    CHECK(moved.Release() == MMS_OK && !moved && !x.fixture.queue.HasLeases(x.fixture.client));
    CHECK(service.Repaint(target, ticket) == MMS_OK); raw = ticket.detach(); CHECK(raw && !ticket);
    CHECK(service.api().cancel(service.api().context, raw) == MMS_OK);
    CHECK(service.api().query(service.api().context, raw, &low) == MMS_OK && low.status == MMS_UI_REFRESH_CANCELLED);
    CHECK(service.api().release(service.api().context, raw) == MMS_OK && !x.fixture.queue.HasLeases(x.fixture.client));
    // Ticket tables are values: the service wrapper can disappear or be
    // queried for another provider without changing an existing ticket.
    { auto temporary = x.Services(); CHECK(temporary.RepaintElement(x.fixture.element, ticket) == MMS_OK); }
    const auto beforeBusy = x.requests;
    CHECK(service.RepaintElement(x.fixture.element, ticket) == MMS_E_BUSY && x.requests == beforeBusy);
    CHECK(ticket.CancelAndRelease() == MMS_OK && !ticket);
    CHECK(service.Repaint(target, ticket) == MMS_OK); raw = ticket.get();
    MmsResult requestResult = MMS_E_FAILED, queryResult = MMS_E_FAILED, cancelResult = MMS_E_FAILED, releaseResult = MMS_E_FAILED;
    std::thread worker([&, services = service, owned = std::move(ticket)]() mutable {
        MmsUiRefreshStateV1 snapshot{}; queryResult = owned.Query(snapshot);
        cancelResult = owned.Cancel(); releaseResult = owned.Release();
        sdk::RefreshTicket other; requestResult = services.RepaintRects(target, &rect, 1, other, 72);
        if (!requestResult) requestResult = other.Release(); // Keep the pending contribution alive.
    }); worker.join();
    CHECK(!ticket && requestResult == MMS_OK && queryResult == MMS_OK && cancelResult == MMS_OK && releaseResult == MMS_OK);
    CHECK(x.fixture.queue.HasLeases(x.fixture.client)); x.fixture.queue.Drain();
    CHECK(!x.fixture.queue.HasLeases(x.fixture.client)); refresh_checks::Region(x.fixture.button, {{2,3,9,8}}); x.fixture.Clean();
    // Scope exit releases only the handle; it cannot silently cancel work.
    { sdk::RefreshTicket scoped; CHECK(service.Repaint(target, scoped) == MMS_OK); }
    CHECK(x.fixture.queue.HasLeases(x.fixture.client)); x.fixture.queue.Drain();
    CHECK(!x.fixture.queue.HasLeases(x.fixture.client)); refresh_checks::Region(x.fixture.button, {{0,0,300,120}});
}
static void FailureAndStopping() {
    Host x; auto service = x.Services(); auto target = x.fixture.Target(); sdk::RefreshTicket ticket;
    MmsUiDirtyRectV1 rect{2,3,7,5}; CHECK(service.RepaintRects(target, nullptr, 1, ticket) == MMS_E_ARGUMENT && !ticket);
    CHECK(service.RepaintElement(0, ticket) == MMS_E_ARGUMENT && !ticket);
    CHECK(service.RepaintRects(target, &rect, 33, ticket) == MMS_E_BUSY && !ticket);
    auto old = target; ++x.fixture.presentations[0].x; x.fixture.queue.SyncTargets(x.fixture.presentations);
    CHECK(service.RepaintRect(old, rect, ticket) == MMS_E_STALE && !ticket);
    CHECK(service.Repaint(old, ticket) == MMS_OK); // Whole-target helper accepts current geometry.
    auto raw = ticket.get(); x.cancelResult = MMS_E_BUSY;
    CHECK(ticket.CancelAndRelease() == MMS_E_BUSY && ticket.get() == raw && x.releases == 0);
    x.cancelResult = MMS_OK; x.releaseResult = MMS_E_FAILED;
    CHECK(ticket.CancelAndRelease() == MMS_E_FAILED && ticket.get() == raw);
    MmsUiRefreshStateV1 state{}; CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_CANCELLED);
    x.releaseResult = MMS_OK; CHECK(ticket.Release() == MMS_OK && !ticket);
    CHECK(service.Repaint(old, ticket) == MMS_OK); raw = ticket.get();
    CHECK(service.api().release(service.api().context, raw) == MMS_OK);
    state.status = 777; CHECK(ticket.Query(state) == MMS_E_NOT_FOUND && state.status == 777);
    CHECK(ticket.Release() == MMS_OK && !ticket); x.fixture.queue.Drain();
    CHECK(service.Repaint(old, ticket) == MMS_OK); raw = ticket.get();
    CHECK(service.api().cancel(service.api().context, raw) == MMS_OK);
    CHECK(service.api().release(service.api().context, raw) == MMS_OK);
    CHECK(ticket.CancelAndRelease() == MMS_OK && !ticket);
    CHECK(service.Repaint(old, ticket) == MMS_OK);
    x.fixture.queue.AdvanceProjectEpoch();
    CHECK(ticket.Query(state) == MMS_OK && state.result == MMS_E_STALE);
    CHECK(ticket.Release() == MMS_OK);
    CHECK(service.Repaint(old, ticket) == MMS_E_STALE && !ticket);
    target = x.fixture.Target(); CHECK(service.Repaint(target, ticket) == MMS_OK);
    x.fixture.queue.Retire(x.fixture.client);
    CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_CANCELLED);
    sdk::RefreshTicket empty; CHECK(service.Repaint(target, empty) == MMS_E_NOT_READY && !empty);
    CHECK(ticket.CancelAndRelease() == MMS_OK && !x.fixture.queue.HasLeases(x.fixture.client));
}
} // namespace cpp_refresh_checks

static void CppRefreshChecks() {
    cpp_refresh_checks::Negotiation();
    cpp_refresh_checks::OwnershipAndWorkers();
    cpp_refresh_checks::FailureAndStopping();
}
