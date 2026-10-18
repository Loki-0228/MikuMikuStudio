#include "ui_refresh_binding.hpp"

namespace data_refresh_checks {
struct Fixture : state_refresh_checks::Fixture {
    MmsUiRefreshBindingApiV1 data = queue.Bindings().Api(client);
    MmsUiDataSourceStateV1 sourceState{};
    MmsHandle source = 0, binding = 0, lastBuilder = 0;
    unsigned prepares = 0;
    int sampled = 0;
    std::vector<unsigned> stages;
    std::function<MmsResult()> onPrepare;
    Fixture() {
        MmsUiDataSourceDescV1 desc{}; desc.h = Header<MmsUiDataSourceDescV1>(); desc.id = S("display"); desc.maxBytes = 4096;
        CHECK(data.registerSource(data.context, &desc, &source) == MMS_OK);
        MmsUiDataBindingDescV1 b{}; b.h = Header<MmsUiDataBindingDescV1>(); b.target = Target().target;
        b.source = source; b.context = this; b.maxDisplayBytes = 4096;
        b.prepare = [](void* context, const MmsUiDataPrepareV1* input) -> MmsResult {
            auto& x = *static_cast<Fixture*>(context); ++x.prepares; x.stages.push_back(1);
            CHECK(x.registry.InCallback() && x.queue.ReadOnlyCause());
            CHECK(input->source == x.source && input->sourceBytes.bytes == sizeof(int));
            std::memcpy(&x.sampled, input->sourceBytes.data, sizeof(int));
            x.lastBuilder = input->builder;
            if (x.onPrepare) if (const auto result = x.onPrepare()) return result;
            const int display = x.sampled * 2;
            return x.data.writePrepared(x.data.context, input->builder, {&display, sizeof(display)});
        };
        CHECK(data.bind(data.context, &b, &binding) == MMS_OK);
        CHECK(Target().supportedKinds == (MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_DATA));
        Publish(12);
    }
    void Publish(int value) {
        MmsUiDataSourcePublishV1 p{}; p.h = Header<MmsUiDataSourcePublishV1>(); p.source = source;
        p.expectedRevision = sourceState.revision; p.bytes = {&value, sizeof(value)};
        MmsUiDataSourceStateV1 next{}; next.h = Header<MmsUiDataSourceStateV1>();
        CHECK(data.publishSource(data.context, &p, &next) == MMS_OK); sourceState = next;
    }
    MmsUiRefreshRequestV1 DataRequest(uint32_t kinds = MMS_UI_REFRESH_DATA) {
        auto r = Request(Target()); r.kinds = kinds; r.binding = binding; r.sourceRevision = sourceState.revision; return r;
    }
    MmsHandle Submit(const MmsUiRefreshRequestV1& request) {
        MmsHandle ticket = 0; CHECK(api.request(api.context, &request, &ticket) == MMS_OK && ticket);
        tickets.push_back(ticket); return ticket;
    }
    int Display() {
        MmsUiDataDisplayV1 view{}; view.h = Header<MmsUiDataDisplayV1>();
        CHECK(data.acquireDisplay(data.context, binding, &view) == MMS_OK && view.bytes.bytes == sizeof(int));
        int value = 0; std::memcpy(&value, view.bytes.data, sizeof(value));
        CHECK(data.releaseDisplay(data.context, view.lease) == MMS_OK); return value;
    }
};
static void Pipeline() {
    Fixture x; const auto revision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    x.onState = [&] { x.stages.push_back(2); CHECK(x.Display() == 24); };
    auto ticket = x.Submit(x.DataRequest(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT));
    x.queue.Drain(); auto state = x.State(ticket);
    CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == 7 && state.sourceRevision == 1);
    CHECK(x.stages == std::vector<unsigned>({1, 2}) && x.Display() == 24 && x.prepares == 1);
    CHECK(x.registry.Revision() == revision && x.registry.PreferencesRevision() == preferences);
    CHECK(x.data.writePrepared(x.data.context, x.lastBuilder, {}) == MMS_E_STALE);
    x.Clean(); x.stages.clear(); const auto count = x.stateReads;
    const auto cached = x.Submit(x.DataRequest()); x.queue.Drain();
    CHECK(x.State(cached).completedKinds == MMS_UI_REFRESH_DATA && x.State(cached).sourceRevision == 1);
    CHECK(x.prepares == 1 && x.stages.empty() && x.stateReads == count && refresh_checks::Empty(x.button));
    x.Publish(12); CHECK(x.sourceState.revision == 1);
    const auto noChange = x.Submit(x.DataRequest()); x.queue.Drain(); CHECK(x.State(noChange).result == MMS_OK && x.prepares == 1);
}
static void ExactRevisionsAndCancellation() {
    Fixture x; auto r = x.DataRequest(); const auto old = x.Submit(r), merged = x.Submit(r);
    x.Publish(19); const auto next = x.Submit(x.DataRequest());
    CHECK(x.api.cancel(x.api.context, merged) == MMS_OK);
    x.queue.Drain();
    CHECK(x.State(old).sourceRevision == 1 && x.State(next).sourceRevision == 2 && x.State(merged).status == MMS_UI_REFRESH_CANCELLED);
    CHECK(x.prepares == 2 && x.Display() == 38);
    MmsHandle unchanged = 919; CHECK(x.api.request(x.api.context, &r, &unchanged) == MMS_E_STALE && unchanged == 919);
    x.Publish(23); const auto cancelled = x.Submit(x.DataRequest());
    CHECK(x.api.cancel(x.api.context, cancelled) == MMS_OK); x.queue.Drain();
    CHECK(x.prepares == 2 && x.Display() == 38 && x.State(cancelled).sourceRevision == 0);
}
static void FailureAndReadOnly() {
    Fixture x; auto initial = x.Submit(x.DataRequest()); x.queue.Drain(); CHECK(x.State(initial).result == MMS_OK);
    x.Publish(20); x.onPrepare = [] { return MMS_E_FAILED; };
    auto failed = x.Submit(x.DataRequest(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT));
    const auto states = x.stateReads; x.Clean(); x.queue.Drain();
    CHECK(x.State(failed).status == MMS_UI_REFRESH_FAILED && x.State(failed).completedKinds == 0 && !x.State(failed).sourceRevision);
    CHECK(x.stateReads == states && x.Display() == 24 && refresh_checks::Empty(x.button));
    x.onPrepare = [&] {
        CHECK(x.ui.unregisterItem(x.ui.context, x.second) == MMS_E_BUSY);
        auto nested = x.DataRequest(); MmsHandle out = 777;
        CHECK(x.api.request(x.api.context, &nested, &out) == MMS_E_BUSY && out == 777);
        nested = x.Request(x.Target()); nested.kinds = MMS_UI_REFRESH_STATE;
        MmsResult worker = MMS_OK;
        std::thread background([&] { worker = x.api.request(x.api.context, &nested, &out); }); background.join();
        CHECK(worker == MMS_E_BUSY && out == 777);
        return MMS_OK;
    };
    const auto success = x.Submit(x.DataRequest()); x.queue.Drain();
    CHECK(x.State(success).result == MMS_OK && x.Display() == 40 && !x.registry.InCallback() && !x.queue.ReadOnlyCause());
    CHECK(x.registry.elements.count("example.refresh.sibling") == 1);
}
static void TargetAndEpoch() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture x; const auto first = x.Submit(x.DataRequest()); x.queue.Drain(); CHECK(x.State(first).result == MMS_OK);
        x.Publish(30); const auto pending = x.Submit(x.DataRequest()); const auto reads = x.prepares;
        if (mode == 0) { ++x.presentations[0].instance; x.queue.SyncTargets(x.presentations); }
        else if (mode == 1) x.queue.AdvanceProjectEpoch();
        else if (mode == 2) CHECK(x.data.unregisterSource(x.data.context, x.source) == MMS_OK);
        else { DestroyWindow(x.button); x.button = nullptr; }
        x.queue.Drain();
        CHECK(x.State(pending).status == MMS_UI_REFRESH_FAILED && x.State(pending).result == MMS_E_STALE && x.prepares == reads);
    }
}
static void StopAndLease() {
    Fixture x; const auto completed = x.Submit(x.DataRequest()); x.queue.Drain(); CHECK(x.State(completed).result == MMS_OK);
    MmsUiDataDisplayV1 hold{}; hold.h = Header<MmsUiDataDisplayV1>();
    CHECK(x.data.acquireDisplay(x.data.context, x.binding, &hold) == MMS_OK);
    x.Publish(30); const auto pending = x.Submit(x.DataRequest()); x.queue.Retire(x.client);
    CHECK(x.State(pending).status == MMS_UI_REFRESH_CANCELLED && x.queue.HasLeases(x.client));
    int value = 0; std::memcpy(&value, hold.bytes.data, sizeof(value)); CHECK(value == 24);
    for (auto ticket : x.tickets) CHECK(x.api.release(x.api.context, ticket) == MMS_OK); x.tickets.clear();
    CHECK(x.queue.HasLeases(x.client));
    MmsResult result = MMS_E_FAILED; std::thread worker([&] { result = x.data.releaseDisplay(x.data.context, hold.lease); }); worker.join();
    CHECK(result == MMS_OK && !x.queue.HasLeases(x.client));
}
} // namespace data_refresh_checks
static void DataRefreshChecks() {
    data_refresh_checks::Pipeline(); data_refresh_checks::ExactRevisionsAndCancellation();
    data_refresh_checks::FailureAndReadOnly(); data_refresh_checks::TargetAndEpoch(); data_refresh_checks::StopAndLease();
}
