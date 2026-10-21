#include <mms/cpp/refresh_dependency.hpp>
#include <type_traits>

namespace cpp_refresh_dependency_checks {
namespace sdk = mms::sdk;
struct Host {
    ui_refresh_dependency_checks::Fixture fixture;
    MmsUiRefreshDependencyApiV1 original = fixture.dependency, table = original;
    MmsHostV1 host{};
    MmsResult negotiation = MMS_OK, releaseResult = MMS_OK;
    unsigned connects = 0, releases = 0;
    bool arguments = true, throwQuery = false, throwRelease = false;
    Host() {
        table.context = this;
        table.getCapabilities = [](void* p, MmsUiRefreshDependencyCapabilitiesV1* out) {
            auto& x = *static_cast<Host*>(p); return x.original.getCapabilities(x.original.context, out);
        };
        table.registerDependency = [](void* p, const MmsUiRefreshDependencyDescV1* desc, MmsHandle* out) {
            auto& x = *static_cast<Host*>(p); ++x.connects;
            return x.original.registerDependency(x.original.context, desc, out);
        };
        table.unregisterDependency = [](void* p, MmsHandle handle) -> MmsResult {
            auto& x = *static_cast<Host*>(p); ++x.releases;
            if (x.throwRelease) throw std::bad_alloc();
            if (x.releaseResult) return x.releaseResult;
            return x.original.unregisterDependency(x.original.context, handle);
        };
        host.h = Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
            auto& x = *static_cast<Host*>(p);
            const auto id = String(name);
            if (id == "mms.ui.refresh") {
                x.arguments &= major == 1 && !minor && size == sizeof(x.fixture.api);
                std::memcpy(out, &x.fixture.api, sizeof(x.fixture.api)); return MMS_OK;
            }
            x.arguments &= id == "mms.ui.refresh.dependencies" && major == 1 && !minor && size == sizeof(x.table);
            if (x.throwQuery) throw std::bad_alloc();
            if (x.negotiation) return x.negotiation;
            std::memcpy(out, &x.table, sizeof(x.table)); return MMS_OK;
        };
    }
    sdk::RefreshDependencyServices Services() {
        sdk::RefreshDependencyServices out; CHECK(sdk::RefreshDependencyServices::Query(host, out) == MMS_OK); return out;
    }
    sdk::RefreshServices Refresh() {
        sdk::RefreshServices out; CHECK(sdk::RefreshServices::Query(host, out) == MMS_OK); return out;
    }
};
static void Negotiation() {
    static_assert(!std::is_copy_constructible_v<sdk::RefreshDependency>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::RefreshDependency>);
    static_assert(!std::is_move_assignable_v<sdk::RefreshDependency>);
    Host x; sdk::RefreshDependencyServices empty; sdk::RefreshDependency edge;
    MmsUiRefreshDependencyCapabilitiesV1 caps{}; caps.maxDependenciesPerPlugin = 777;
    CHECK(!empty.ready() && empty.CheckThread() == MMS_E_NOT_READY);
    CHECK(empty.Capabilities(caps) == MMS_E_NOT_READY && caps.maxDependenciesPerPlugin == 777);
    CHECK(empty.Connect({}, {}, edge) == MMS_E_NOT_READY && !edge);
    CHECK(edge.Release() == MMS_OK && !edge.detach());
    auto service = x.Services(); const auto context = service.api().context;
    CHECK(service.ready() && context == &x && x.arguments);
    for (auto error : {MMS_E_NOT_FOUND, MMS_E_VERSION, MMS_E_NOT_READY}) {
        x.negotiation = error;
        CHECK(sdk::RefreshDependencyServices::Query(x.host, service) == error && service.api().context == context);
    }
    x.negotiation = MMS_OK; x.throwQuery = true;
    CHECK(sdk::RefreshDependencyServices::Query(x.host, service) == MMS_E_MEMORY && service.api().context == context);
    x.throwQuery = false;
    auto host = x.host; host.h.size = 1;
    CHECK(sdk::RefreshDependencyServices::Query(host, empty) == MMS_E_VERSION && !empty.ready());
    host = x.host; host.h.major = 2; CHECK(sdk::RefreshDependencyServices::Query(host, empty) == MMS_E_VERSION);
    host = x.host; host.queryService = nullptr; CHECK(sdk::RefreshDependencyServices::Query(host, empty) == MMS_E_ARGUMENT);
    const auto table = x.table;
    x.table.h.size -= 1; CHECK(sdk::RefreshDependencyServices::Query(x.host, empty) == MMS_E_VERSION);
    x.table = table; x.table.h.major = 2; CHECK(sdk::RefreshDependencyServices::Query(x.host, empty) == MMS_E_VERSION);
    x.table = table; x.table.h.size += 16; x.table.h.minor = 4;
    CHECK(sdk::RefreshDependencyServices::Query(x.host, empty) == MMS_OK && empty.ready());
    for (unsigned missing = 0; missing < 3; ++missing) {
        x.table = table;
        if (!missing) x.table.getCapabilities = nullptr;
        if (missing == 1) x.table.registerDependency = nullptr;
        if (missing == 2) x.table.unregisterDependency = nullptr;
        CHECK(sdk::RefreshDependencyServices::Query(x.host, service) == MMS_E_UNSUPPORTED && service.api().context == context);
    }
    x.table = table;
    CHECK(service.Capabilities(caps) == MMS_OK && caps.maxDependenciesPerPlugin == 512 && caps.maxDependentsPerRequest == 256);
    const auto from = x.fixture.Target(x.fixture.element).target;
    const auto to = x.fixture.Target(x.fixture.second).target;
    MmsResult threadCheck = MMS_OK, threadCaps = MMS_OK, threadConnect = MMS_OK, threadElements = MMS_OK;
    auto refresh = x.Refresh();
    std::thread worker([&] {
        threadCheck = service.CheckThread(); threadCaps = service.Capabilities(caps);
        threadConnect = service.Connect(from, to, edge);
        threadElements = service.ConnectElements(refresh, x.fixture.element, x.fixture.second, edge);
    }); worker.join();
    CHECK(threadCheck == MMS_E_THREAD && threadCaps == MMS_E_THREAD && threadConnect == MMS_E_THREAD && threadElements == MMS_E_THREAD && !edge && !x.connects);
}
static void SameObjectsAndCleanup() {
    Host x; auto refresh = x.Refresh(); auto service = x.Services(); sdk::RefreshDependency edge;
    const auto from = x.fixture.Target(x.fixture.element).target;
    const auto to = x.fixture.Target(x.fixture.second).target;
    const auto registry = x.fixture.registry.Revision(), preferences = x.fixture.registry.PreferencesRevision();
    CHECK(service.ConnectElements(refresh, x.fixture.element, x.fixture.second, edge) == MMS_OK && edge && x.connects == 1);
    const auto handle = edge.get();
    CHECK(service.Connect(from, to, edge) == MMS_E_BUSY && edge.get() == handle && x.connects == 1);
    auto duplicate = ui_refresh_dependency_checks::Desc(from, to); MmsHandle unchanged = 919;
    CHECK(x.original.registerDependency(x.original.context, &duplicate, &unchanged) == MMS_E_CONFLICT && unchanged == 919);
    sdk::RefreshDependency moved(std::move(edge)); CHECK(!edge && moved.get() == handle && edge.Release() == MMS_OK);
    sdk::RefreshTicket ticket; MmsResult accepted = MMS_E_FAILED;
    std::thread worker([&] { accepted = refresh.RefreshDependents(from, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT, ticket, 93); }); worker.join();
    CHECK(accepted == MMS_OK && ticket);
    MmsUiRefreshStateV1 low{}; low.h = Header<MmsUiRefreshStateV1>();
    CHECK(x.fixture.api.query(x.fixture.api.context, ticket.get(), &low) == MMS_OK && low.status == MMS_UI_REFRESH_PENDING);
    x.fixture.queue.Drain();
    MmsUiRefreshStateV1 state{}; CHECK(ticket.Query(state) == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == 5);
    CHECK(x.fixture.stateReads == 1 && refresh_checks::Empty(x.fixture.button) && !refresh_checks::Empty(x.fixture.sibling));
    CHECK(ticket.Release() == MMS_OK && x.fixture.registry.Revision() == registry && x.fixture.registry.PreferencesRevision() == preferences);
    MmsResult cleanup = MMS_OK;
    std::thread cleanupWorker([&] { cleanup = moved.Release(); }); cleanupWorker.join();
    CHECK(cleanup == MMS_E_THREAD && moved.get() == handle);
    x.releaseResult = MMS_E_BUSY; CHECK(moved.Release() == MMS_E_BUSY && moved.get() == handle);
    x.releaseResult = MMS_OK; x.throwRelease = true;
    CHECK(moved.Release() == MMS_E_MEMORY && moved.get() == handle); x.throwRelease = false;
    CHECK(moved.Release() == MMS_OK && !moved);
    CHECK(x.original.unregisterDependency(x.original.context, handle) == MMS_E_NOT_FOUND);
    CHECK(service.Connect(from, to, edge) == MMS_OK); const auto detached = edge.detach(); CHECK(detached && !edge);
    CHECK(x.original.unregisterDependency(x.original.context, detached) == MMS_OK);
    MmsHandle automatic = 0;
    { sdk::RefreshDependency scoped; CHECK(service.Connect(from, to, scoped) == MMS_OK); automatic = scoped.get(); }
    CHECK(x.original.unregisterDependency(x.original.context, automatic) == MMS_E_NOT_FOUND);
    CHECK(service.Connect(from, to, edge) == MMS_OK);
    CHECK(refresh.RefreshDependentElements(x.fixture.element, MMS_UI_REFRESH_REPAINT, ticket) == MMS_OK);
    x.fixture.queue.Retire(x.fixture.client);
    CHECK(edge.Release() == MMS_OK && !edge); // Host lifecycle already removed the edge.
    CHECK(ticket.Release() == MMS_OK && !ticket);
}
} // namespace cpp_refresh_dependency_checks
static void CppRefreshDependencyChecks() {
    cpp_refresh_dependency_checks::Negotiation();
    cpp_refresh_dependency_checks::SameObjectsAndCleanup();
}
