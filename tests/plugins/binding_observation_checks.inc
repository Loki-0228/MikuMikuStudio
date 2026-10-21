// 原 PropertyRecord、刷新队列及自有 HWND 的确定性检查。
// 在 scoped_property_checks.inc 之后包含，复用该文件的真实 fixture。
// 复用 overflow_menu_checks.inc 的 operator new 故障注入，不替换生产分配器。
// 不构造 MMDApp 场景，不开放公开 mms.edit，不使用全局输入或像素验证，
// 不声称真实场景求值或硬件验收。入口：BindingObservationChecks()。
namespace binding_observation_checks {
using Fixture = scoped_property_checks::Fixture;
using State = MmsUiPropertyRefreshScopedSubscriptionStateV1;

struct Activity {
    std::array<unsigned, 2> reads, releases, hookCalls, prepares;
    std::array<int64_t, 2> displays;
    unsigned layouts, triggerReads;
    std::vector<unsigned> trace;
    unsigned wakes;
    uint64_t registryRevision, preferencesRevision;
    explicit Activity(Fixture& f)
        : reads(f.reads), releases(f.releases), hookCalls(f.hookCalls), prepares(f.prepares),
          displays(f.displays), layouts(f.layouts), triggerReads(f.triggerReads), trace(f.trace),
          wakes(f.wakes.load()), registryRevision(f.registry.Revision()),
          preferencesRevision(f.registry.PreferencesRevision()) {}
};

void SceneUnchanged(Fixture& f) {
    CHECK(!f.edits && !f.stages && f.playhead == 31 && f.sceneRevision == 73);
}

void ActivityUnchanged(Fixture& f, const Activity& before, bool checkWake = true) {
    CHECK(f.reads == before.reads && f.releases == before.releases &&
        f.hookCalls == before.hookCalls && f.prepares == before.prepares &&
        f.displays == before.displays && f.layouts == before.layouts &&
        f.triggerReads == before.triggerReads && f.trace == before.trace);
    CHECK(f.registry.Revision() == before.registryRevision &&
        f.registry.PreferencesRevision() == before.preferencesRevision);
    if (checkWake) CHECK(f.wakes.load() == before.wakes);
    SceneUnchanged(f);
}

void ReportState(const char* phase, const char* side, const State& state, unsigned iteration = 0) {
    std::cerr << "BINDING_OBSERVATION phase=" << phase << " iteration=" << iteration << " side=" << side
        << " checks=" << checks << " subscription=" << state.subscription
        << " seq=" << state.latestSequence << '/' << state.submittedSequence << '/' << state.completedSequence
        << " work=" << state.workGeneration << '/' << state.submittedWorkGeneration << '/' << state.completedWorkGeneration
        << " dependency=" << state.submittedDependencyRevision << " wait=" << state.waitingFor
        << " result=" << state.lastResult << " pending=" << state.pending
        << " target=" << state.target.element << '/' << state.target.instanceGeneration << '/' << state.target.projectEpoch
        << " property=" << state.property.property << '/' << state.property.registrationGeneration << '/' << state.property.projectEpoch
        << " scope=" << state.scope << " kinds=" << state.kinds << " flags=" << state.flags
        << " header=" << state.h.size << '/' << state.h.major << '/' << state.h.minor
        << " reserved=" << state.reserved << '/' << state.reservedTail0 << '/' << state.reservedTail << '\n';
}

void SameState(Fixture& f, const State& before, const char* phase, unsigned iteration = 0) {
    const auto now = f.Status();
    if (std::memcmp(&now, &before, sizeof(now))) {
        ReportState(phase, "expected", before, iteration); ReportState(phase, "actual", now, iteration);
        const auto* expectedBytes = reinterpret_cast<const unsigned char*>(&before);
        const auto* actualBytes = reinterpret_cast<const unsigned char*>(&now);
        for (size_t i = 0; i < sizeof(now); ++i) if (expectedBytes[i] != actualBytes[i]) {
            std::cerr << "BINDING_OBSERVATION first_different_byte=" << i
                << " expected=" << unsigned(expectedBytes[i]) << " actual=" << unsigned(actualBytes[i]) << '\n';
            break;
        }
        MmsUiDataSourceStateV1 source{}; source.h = Header<MmsUiDataSourceStateV1>();
        const auto sourceResult = f.data.getSource(f.data.context, f.sources[1], &source);
        std::cerr << "BINDING_OBSERVATION source_result=" << sourceResult << " source=" << source.source
            << " revision=" << source.revision << " tracked_revision=" << f.sourceRevisions[1]
            << " epoch=" << source.projectEpoch << " bytes=" << source.byteCount
            << " reads=" << f.reads[0] << '/' << f.reads[1]
            << " prepares=" << f.prepares[0] << '/' << f.prepares[1]
            << " layouts=" << f.layouts << " wakes=" << f.wakes.load()
            << " queue_pending=" << f.queue.Pending()
            << " allocation_hits=" << overflow_allocation_fault::hits
            << " remaining_failures=" << overflow_allocation_fault::matchingFailures
            << " fault_bytes=" << overflow_allocation_fault::bytes << '\n';
    }
    // 包括通知序号、三种工作代次、先前提交的依赖版本及失败等待原因。
    // 普通观察可以更新内部 watch，不能将其伪装成新工作或覆盖 MEMORY。
    CHECK(!std::memcmp(&now, &before, sizeof(now)));
}

MmsUiDataSourceStateV1 Source(Fixture& f) {
    MmsUiDataSourceStateV1 out{}; out.h = Header<MmsUiDataSourceStateV1>();
    CHECK(f.data.getSource(f.data.context, f.sources[1], &out) == MMS_OK);
    CHECK(out.source == f.sources[1] && out.revision == f.sourceRevisions[1] &&
        out.byteCount == sizeof(int64_t));
    return out;
}

struct ClaimedFailure {
    State primed, bound, failed;
    Activity activity;
    ClaimedFailure(Fixture& f, State a, State b, State c)
        : primed(a), bound(b), failed(c), activity(f) {}
};

ClaimedFailure MakeClaimedBindingObservationFailure(Fixture& f) {
    CHECK(!overflow_allocation_fault::bytes && !overflow_allocation_fault::matchingFailures);
    CHECK(sizeof(UiRefreshBindings::PropertySource) == 56);
    f.StraightGraph(); f.Subscribe(); f.Complete();
    const auto primed = f.Status();
    ReportState("setup/native-inputs-completed", "checkpoint", primed);
    CHECK(!primed.pending && !primed.lastResult && !primed.waitingFor &&
        primed.latestSequence && primed.latestSequence == primed.submittedSequence &&
        primed.latestSequence == primed.completedSequence &&
        primed.workGeneration == primed.submittedWorkGeneration &&
        primed.workGeneration == primed.completedWorkGeneration);
    CHECK(f.reads[0] == 1 && f.reads[1] == 1 && !f.prepares[0] && !f.prepares[1] &&
        f.displays[0] == 7 && f.displays[1] == 9 && f.layouts == 1 && !f.triggerReads &&
        !f.queue.Pending());
    const Activity completedActivity(f);

    // 来源登记和首次发布先完成，再仅对真实 Bind 的后置观察注入故障。
    // 避免把 source/map 分配失败误认为“Bind 成功但 watch 未安装”。
    MmsUiDataSourceDescV1 source{Header<MmsUiDataSourceDescV1>(),
        S("example.scoped.property.binding-observation-memory"), 64, 0, 0};
    CHECK(f.data.registerSource(f.data.context, &source, &f.sources[1]) == MMS_OK && f.sources[1]);
    f.Publish(1, 401); const auto published = Source(f);
    CHECK(published.revision == 1 && !f.queue.Pending());
    std::cerr << "BINDING_OBSERVATION phase=setup/source-published-before-bind source=" << published.source
        << " revision=" << published.revision << " bytes=" << published.byteCount
        << " binding=" << f.bindings[1] << '\n';
    SameState(f, primed, "setup/source-published-before-bind"); ActivityUnchanged(f, completedActivity);

    MmsUiDataBindingDescV1 binding{}; binding.h = Header<MmsUiDataBindingDescV1>();
    binding.source = f.sources[1]; binding.target = f.Target(f.inputs[1]).target;
    binding.context = &f; binding.maxDisplayBytes = 64;
    binding.prepare = [](void* p, const MmsUiDataPrepareV1* input) -> MmsResult {
        auto& x = *static_cast<Fixture*>(p); ++x.prepares[1];
        CHECK(x.registry.InCallback() && x.queue.ReadOnlyCause(x.client));
        return x.data.writePrepared(x.data.context, input->builder, input->sourceBytes);
    };
    const auto bindHits = overflow_allocation_fault::hits;
    overflow_allocation_fault::bytes = 2 * sizeof(UiRefreshBindings::PropertySource);
    const auto result = f.data.bind(f.data.context, &binding, &f.bindings[1]);
    const auto bindRemainingBytes = overflow_allocation_fault::bytes;
    overflow_allocation_fault::bytes = 0;
    CHECK(result == MMS_OK && f.bindings[1] &&
        overflow_allocation_fault::hits == bindHits + 1 && !bindRemainingBytes &&
        !overflow_allocation_fault::matchingFailures);
    const auto bound = f.Status();
    ReportState("binding/post-observation-failure", "checkpoint", bound);
    std::cerr << "BINDING_OBSERVATION phase=binding/post-observation-failure bind_result=" << result
        << " hits_before=" << bindHits << " hits_after=" << overflow_allocation_fault::hits
        << " remaining_bytes=" << bindRemainingBytes << " binding=" << f.bindings[1] << '\n';
    CHECK(bound.pending && bound.latestSequence == primed.latestSequence &&
        bound.workGeneration == primed.workGeneration + 1 &&
        bound.submittedSequence == primed.submittedSequence &&
        bound.completedSequence == primed.completedSequence &&
        bound.submittedWorkGeneration == primed.submittedWorkGeneration &&
        bound.completedWorkGeneration == primed.completedWorkGeneration &&
        bound.submittedDependencyRevision == primed.submittedDependencyRevision);
    CHECK(f.queue.Bindings().HasBinding(f.client, binding.target) && f.queue.Pending() &&
        f.wakes.load() > completedActivity.wakes);
    ActivityUnchanged(f, completedActivity, false);
    const auto resourceClock = f.queue.Bindings().ResourceCapacityRevision();
    const auto displayClock = f.queue.Bindings().DisplayCapacityRevision();
    const auto wakes = f.wakes.load();

    // 三次 112 字节失败分别发生在：DATA scheduler 的 property 观察、
    // property scheduler 的观察、原 Request::CaptureProperty 的 sources.reserve。
    // 图 Capture 和成员构造不匹配 112；最后一处在安装 source observer 前。
    // 只有精确命中三次且余额归零，才认为真正尝试了原 Request。
    const auto requestHits = overflow_allocation_fault::hits;
    overflow_allocation_fault::matchingFailures = 3;
    overflow_allocation_fault::bytes = 2 * sizeof(UiRefreshBindings::PropertySource);
    f.queue.Drain();
    const auto remaining = overflow_allocation_fault::matchingFailures;
    const auto remainingBytes = overflow_allocation_fault::bytes;
    overflow_allocation_fault::bytes = 0; overflow_allocation_fault::matchingFailures = 0;
    CHECK(overflow_allocation_fault::hits == requestHits + 3 && !remaining && !remainingBytes);
    const auto failed = f.Status();
    ReportState("request/capture-memory-before-catchup", "checkpoint", failed);
    std::cerr << "BINDING_OBSERVATION phase=request/capture-memory-before-catchup hits_before=" << requestHits
        << " hits_after=" << overflow_allocation_fault::hits << " remaining_failures=" << remaining
        << " remaining_bytes=" << remainingBytes << '\n';
    CHECK(failed.pending && failed.lastResult == MMS_E_MEMORY &&
        failed.waitingFor == MMS_UI_PROPERTY_REFRESH_WAIT_DISPLAY_CAPACITY &&
        failed.latestSequence == primed.latestSequence &&
        failed.workGeneration == bound.workGeneration &&
        failed.submittedSequence == primed.submittedSequence &&
        failed.completedSequence == primed.completedSequence &&
        failed.submittedWorkGeneration == primed.submittedWorkGeneration &&
        failed.completedWorkGeneration == primed.completedWorkGeneration &&
        failed.submittedDependencyRevision == primed.submittedDependencyRevision);
    CHECK(!f.queue.Pending() && f.wakes.load() == wakes &&
        f.queue.Bindings().DisplayCapacityRevision() == displayClock &&
        f.queue.Bindings().ResourceCapacityRevision() == resourceClock &&
        Source(f).revision == published.revision);
    ActivityUnchanged(f, completedActivity, false);
    return ClaimedFailure(f, primed, bound, failed);
}

void CheckRecovery(Fixture& f, const ClaimedFailure& before, uint64_t sequence,
    uint64_t workGeneration, int64_t nativeA, int64_t explicitB) {
    f.Complete(); const auto complete = f.Status();
    CHECK(!complete.pending && !complete.waitingFor && !complete.lastResult &&
        complete.latestSequence == sequence && complete.submittedSequence == sequence &&
        complete.completedSequence == sequence && complete.workGeneration == workGeneration &&
        complete.submittedWorkGeneration == workGeneration &&
        complete.completedWorkGeneration == workGeneration &&
        complete.submittedDependencyRevision == before.primed.submittedDependencyRevision);
    CHECK(f.reads[0] == before.activity.reads[0] + 1 &&
        f.reads[1] == before.activity.reads[1] &&
        f.releases[0] == before.activity.releases[0] + 1 &&
        f.releases[1] == before.activity.releases[1] &&
        f.prepares[0] == before.activity.prepares[0] &&
        f.prepares[1] == before.activity.prepares[1] + 1 &&
        f.layouts == before.activity.layouts + 1 && !f.triggerReads);
    CHECK(f.displays[0] == nativeA && f.displays[1] == before.activity.displays[1] &&
        f.BoundDisplay(1) == explicitB && !f.queue.Pending());
    CHECK(f.trace.size() == before.activity.trace.size() + 4 &&
        f.trace[before.activity.trace.size()] == 10 &&
        f.trace[before.activity.trace.size() + 1] == 100 &&
        f.trace[before.activity.trace.size() + 2] == 101 &&
        f.trace[before.activity.trace.size() + 3] == 200);
    CHECK(f.registry.Revision() == before.activity.registryRevision &&
        f.registry.PreferencesRevision() == before.activity.preferencesRevision);
    SceneUnchanged(f);
    const Activity settled(f);
    for (unsigned n = 0; n < 6; ++n) {
        f.queue.Drain(); SameState(f, complete, "recovery/settled", n); ActivityUnchanged(f, settled); CHECK(!f.queue.Pending());
    }
}

void ClaimedBindingEventDoesNotReplayAfterRequestMemoryFailure() {
    std::cerr << "BINDING_OBSERVATION case=claimed-binding-memory-notify\n";
    Fixture f; const auto failed = MakeClaimedBindingObservationFailure(f);
    // 第一轮无故障 Drain 可以安装 B 的真实绑定/source watch，但原工作已经
    // 由 Request claim 并以 MEMORY 停泊。它不能再 Change/Repark 自启动。
    // 本段直接检查失败后的停泊，不能只检查最终消费完成。
    for (unsigned n = 0; n < 6; ++n) {
        f.queue.Drain(); SameState(f, failed.failed, "parked/ordinary-observation-after-request-memory", n);
        ActivityUnchanged(f, failed.activity);
        CHECK(!f.queue.Pending() && Source(f).revision == 1);
    }
    f.values[0] = 74; f.Notify(); const auto notified = f.Status();
    CHECK(notified.pending && notified.latestSequence == failed.failed.latestSequence + 1 &&
        notified.workGeneration == failed.failed.workGeneration + 1 &&
        notified.submittedSequence == failed.primed.submittedSequence &&
        notified.completedSequence == failed.primed.completedSequence &&
        f.queue.Pending() && f.wakes.load() > failed.activity.wakes);
    CheckRecovery(f, failed, notified.latestSequence, notified.workGeneration, 74, 401);
}

void RealSnapshotReleaseBeforeBindingObservationRecoversClaimedFailure() {
    std::cerr << "BINDING_OBSERVATION case=claimed-binding-memory-real-capacity\n";
    Fixture f; const auto failed = MakeClaimedBindingObservationFailure(f);
    // 尚未补观察就替换 source。401 没有 display/票据/lease 持有，故替换为
    // 402 会真实释放 retained snapshot，并推进容量时钟。这个原发布事件
    // 应使 DISPLAY 停泊恢复，但不产生新的 property sequence/work generation。
    // 此例不能单独证明未安装的 source watch 收到了发布；真实容量通知本身
    // 已足够合法恢复。若要隔离纯发布，必须先保留旧 snapshot 排除容量变化；
    // 现 SourcePublished 协议只要求 SOURCE/CONSUMER 恢复，不能擅自把
    // MEMORY/DISPLAY 下无容量变化的纯发布视作新的 property Notify。
    const auto displayClock = f.queue.Bindings().DisplayCapacityRevision();
    const auto resourceClock = f.queue.Bindings().ResourceCapacityRevision();
    f.values[0] = 75; f.Publish(1, 402);
    CHECK(Source(f).revision == 2 && f.queue.Bindings().DisplayCapacityRevision() > displayClock &&
        f.queue.Bindings().ResourceCapacityRevision() > resourceClock);
    const auto released = f.Status();
    CHECK(released.pending && released.latestSequence == failed.failed.latestSequence &&
        released.workGeneration == failed.failed.workGeneration &&
        released.submittedSequence == failed.primed.submittedSequence &&
        released.completedSequence == failed.primed.completedSequence &&
        released.submittedWorkGeneration == failed.primed.submittedWorkGeneration &&
        released.completedWorkGeneration == failed.primed.completedWorkGeneration &&
        f.queue.Pending() && f.wakes.load() > failed.activity.wakes);
    ActivityUnchanged(f, failed.activity, false);
    CheckRecovery(f, failed, released.latestSequence, released.workGeneration, 75, 402);
}
} // namespace binding_observation_checks

void BindingObservationChecks() {
    binding_observation_checks::ClaimedBindingEventDoesNotReplayAfterRequestMemoryFailure();
    binding_observation_checks::RealSnapshotReleaseBeforeBindingObservationRecoversClaimedFailure();
}
