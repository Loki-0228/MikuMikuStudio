// Actual Manager/DLL/NativeUi and posted refresh messages. No explicit refresh
// tickets, scene/playhead claims, or production edit-host attachment.
#include "scoped_property_probe.h"
#include "shared_history.hpp"
namespace manager_scoped_property_checks {
using native_property_input_checks::Caption;
struct Probe {
    ScopedPropertyBeginFn begin = nullptr; ScopedPropertySetFn set = nullptr; ScopedPropertyNotifyFn notify = nullptr;
    ScopedPropertyBurstFn burst = nullptr; ScopedPropertyMaskFn graph = nullptr, bind = nullptr, fail = nullptr;
    ScopedPropertySimpleFn threads = nullptr, replace = nullptr, removeSeed = nullptr, hold = nullptr, release = nullptr;
    ScopedPropertyQueryFn query = nullptr;
    explicit Probe(HMODULE module) {
        CHECK(module);
#define SCOPED_PROPERTY_EXPORT(member, name) member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name)); CHECK(member)
        SCOPED_PROPERTY_EXPORT(begin, "ScopedPropertyBegin"); SCOPED_PROPERTY_EXPORT(set, "ScopedPropertySet");
        SCOPED_PROPERTY_EXPORT(notify, "ScopedPropertyNotify"); SCOPED_PROPERTY_EXPORT(burst, "ScopedPropertyBurst");
        SCOPED_PROPERTY_EXPORT(graph, "ScopedPropertyGraph"); SCOPED_PROPERTY_EXPORT(bind, "ScopedPropertyBindInput");
        SCOPED_PROPERTY_EXPORT(fail, "ScopedPropertyFailReads"); SCOPED_PROPERTY_EXPORT(threads, "ScopedPropertyCheckThreads");
        SCOPED_PROPERTY_EXPORT(replace, "ScopedPropertyReplaceTrigger"); SCOPED_PROPERTY_EXPORT(removeSeed, "ScopedPropertyRemoveSeed");
        SCOPED_PROPERTY_EXPORT(hold, "ScopedPropertyAcquireHold"); SCOPED_PROPERTY_EXPORT(release, "ScopedPropertyReleaseHold");
        SCOPED_PROPERTY_EXPORT(query, "ScopedPropertyQuery");
#undef SCOPED_PROPERTY_EXPORT
    }
    ScopedPropertyProbeState Read() const { ScopedPropertyProbeState s{}; s.size = sizeof(s); CHECK(query(&s) == MMS_OK); return s; }
    ScopedPropertyProbeState Complete(Manager& manager, uint64_t sequence, uint64_t minimumWork = 1) const {
        const auto until = GetTickCount64() + 3000; auto value = Read();
        const auto done = [&] { const auto& s = value.subscriptionState;
            return value.query == MMS_OK && !s.pending && !s.waitingFor && s.lastResult == MMS_OK &&
                s.latestSequence == sequence && s.submittedSequence == sequence && s.completedSequence == sequence &&
                (value.scope == MMS_UI_REFRESH_SELF || (s.workGeneration >= minimumWork &&
                    s.submittedWorkGeneration == s.workGeneration && s.completedWorkGeneration == s.workGeneration)); };
        while (!done() && GetTickCount64() < until) { manager.Drain(); manager_refresh_checks::Pump(2); value = Read(); }
        if (!done()) std::cerr << "SCOPED_PROPERTY_COMPLETE scope=" << value.scope << " query=" << value.query << " wanted=" << sequence <<
            " sequence=" << value.subscriptionState.latestSequence << '/' << value.subscriptionState.submittedSequence << '/' << value.subscriptionState.completedSequence <<
            " work=" << value.subscriptionState.workGeneration << '/' << value.subscriptionState.submittedWorkGeneration << '/' << value.subscriptionState.completedWorkGeneration <<
            " pending=" << value.subscriptionState.pending << " wait=" << value.subscriptionState.waitingFor << " result=" << value.subscriptionState.lastResult << '\n';
        CHECK(done()); return value;
    }
    ScopedPropertyProbeState Waiting(Manager& manager, uint32_t wait) const {
        const auto until = GetTickCount64() + 3000; auto s = Read();
        while (s.query == MMS_OK && !(s.subscriptionState.waitingFor & wait) && GetTickCount64() < until) {
            manager.Drain(); manager_refresh_checks::Pump(2); s = Read();
        }
        CHECK(s.query == MMS_OK && s.subscriptionState.pending && (s.subscriptionState.waitingFor & wait)); return s;
    }
};
bool Same(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
bool Same(const MmsUiPropertyRefV1& a, const MmsUiPropertyRefV1& b) {
    return a.property == b.property && a.registrationGeneration == b.registrationGeneration && a.projectEpoch == b.projectEpoch;
}
unsigned Reads(const ScopedPropertyProbeState& s) { return s.reads[0] + s.reads[1] + s.reads[2]; }
HWND Field(HWND root, const wchar_t* label) {
    struct Search { const wchar_t* label; HWND found = nullptr; } search{label};
    EnumChildWindows(root, [](HWND child, LPARAM context) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(context); wchar_t type[32]{}; GetClassNameW(child, type, 32);
        if (_wcsicmp(type, L"EDIT")) return TRUE;
        const auto wrapper = GetParent(child);
        for (auto label = GetWindow(wrapper, GW_CHILD); label; label = GetWindow(label, GW_HWNDNEXT)) {
            GetClassNameW(label, type, 32);
            if (!_wcsicmp(type, L"STATIC") && Caption(label) == s.label) { CHECK(!s.found); s.found = child; }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    CHECK(search.found && IsWindowVisible(search.found)); return search.found;
}
void Negotiation(const ScopedPropertyProbeState& s) {
    CHECK(s.oldSize == 64 && !s.oldMinor && s.newSize == 88 && s.newMinor == 1 && s.shortQuery == MMS_E_ARGUMENT && s.negotiationPreserved);
    CHECK(s.oldCaps.h.size == 32 && s.capabilities.h.size == 40 && s.capabilities.supportedScopes ==
        (MMS_UI_REFRESH_SELF | MMS_UI_REFRESH_SUBTREE | MMS_UI_REFRESH_DEPENDENTS));
    CHECK(s.oldCaps.supportedKinds == s.capabilities.supportedKinds && s.oldCaps.supportedFlags == s.capabilities.supportedFlags &&
        !s.oldCaps.supportedFlags && s.editQuery == MMS_E_NOT_FOUND && !s.explicitRequests);
}
void Identity(Manager& manager, const std::string& id, const ScopedPropertyProbeState& s) {
    CHECK(s.query == MMS_OK && s.oldQuery == MMS_OK && s.subscription && s.subscriptionState.subscription == s.subscription &&
        s.oldState.subscription == s.subscription && s.oldState.latestSequence == s.subscriptionState.latestSequence &&
        s.oldState.completedSequence == s.subscriptionState.completedSequence);
    const auto trigger = s.scope == MMS_UI_REFRESH_SELF ? 1u : 0u;
    CHECK(Same(s.subscriptionState.property, s.refs[trigger]) && Same(s.oldState.property, s.refs[trigger]) &&
        Same(s.subscriptionState.target, s.scope == MMS_UI_REFRESH_SELF ? s.targetRefs[0] : s.seed) &&
        Same(s.oldState.target, s.subscriptionState.target));
    CHECK(s.subscriptionState.scope == s.scope && s.subscriptionState.kinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT));
    CHECK(manager.Ui().properties.size() == 3 && manager.Ui().elements.at(id + ".seed").handle == s.seedHandle);
    for (unsigned i = 0; i < 3; ++i) {
        const auto property = manager.Ui().properties.at(id + ".property." + std::to_string(i));
        CHECK(property->handle == s.properties[i] && property->registrationGeneration == s.refs[i].registrationGeneration && s.refs[i].projectEpoch == s.seed.projectEpoch);
        CHECK(s.targetRefs[i].element == s.targets[i] && s.targetRefs[i].instanceGeneration && s.targetRefs[i].projectEpoch == s.seed.projectEpoch);
        CHECK(s.releases[i] == s.successes[i]);
    }
    CHECK(!s.reads[0] && s.identity && s.callbacksUi && s.maxReadDepth == 1 && !s.readDepth && !s.overflow && !s.stages &&
        !manager.CanInvokeEdit() && !s.explicitRequests);
    // The independent trigger is a property only: it names no native INPUT.
    for (const auto& element : manager.Ui().elements) CHECK(String(element.second.desc.propertyId) != id + ".property.0");
    CHECK(s.source && s.bindings[2] && !s.bindings[1]);
}
void ReadOnly(const ScopedPropertyProbeState& s) {
    CHECK(s.readOnlyChecks == 1 && s.outputsPreserved && s.readNotify == MMS_E_BUSY && s.readWorkerNotify == MMS_E_BUSY &&
        s.readWorkerQuery == MMS_OK && s.readSubscribe == MMS_E_BUSY && s.readUnsubscribe == MMS_E_BUSY && s.readGraph == MMS_E_BUSY &&
        s.readWorkerPublish == MMS_OK && s.sourceValue == 777 && s.sourceRevision == 2);
}
void Phases(const ScopedPropertyProbeState& s, unsigned first, uint32_t members) {
    CHECK(s.events > first && s.events <= SCOPED_PROPERTY_TRACE); bool state = false; uint32_t stateMask = 0;
    std::array<unsigned, 3> stateCounts{};
    for (unsigned i = first; i < s.events; ++i) { const auto& e = s.trace[i];
        CHECK(e.target < 3 && (members & (1u << e.target)) && (e.phase == 1 || e.phase == 2));
        if (e.phase == 1) CHECK(!state);
        else { state = true; stateMask |= 1u << e.target; ++stateCounts[e.target];
            CHECK(e.value == s.stateValues[e.target] && e.revision == s.stateRevisions[e.target]);
            if (s.bindings[e.target]) CHECK(e.value == s.display[e.target] && e.revision == s.displayRevisions[e.target]);
            else CHECK(e.target < 2 && e.value == s.lastRead[e.target + 1]);
        }
    }
    CHECK(stateMask == members);
    for (unsigned i = 0; i < 3; ++i) CHECK(stateCounts[i] == ((members & (1u << i)) ? 1u : 0u));
}
void Stable(Manager& manager, uint64_t revision, uint64_t preferences) {
    CHECK(manager.Ui().Revision() == revision && manager.Ui().PreferencesRevision() == preferences && !manager.CanInvokeEdit());
}
void Dependents(Manager& manager, const Probe& probe, HWND a, HWND b, ScopedPropertyProbeState& s) {
    const auto revision = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
    auto readsA = s.reads[1], readsB = s.reads[2]; auto sequence = s.subscriptionState.completedSequence;
    auto events = s.events;
    CHECK(probe.set(1, 31) == MMS_OK && probe.notify(1, 1) == MMS_OK && probe.set(1, 32) == MMS_OK && probe.set(2, 42) == MMS_OK);
    auto queued = probe.Read(); CHECK(queued.workerWasWorker && queued.reads[1] == readsA && queued.reads[2] == readsB &&
        queued.subscriptionState.latestSequence == sequence + 1 && queued.subscriptionState.completedSequence == sequence && Caption(a) == L"11");
    s = probe.Complete(manager, ++sequence); CHECK(s.reads[1] == ++readsA && s.reads[2] == ++readsB && s.lastRead[1] == 32 && s.lastRead[2] == 42 &&
        Caption(a) == L"32" && Caption(b) == L"42"); Phases(s, events, 3); Stable(manager, revision, preferences);
    events = s.events;
    CHECK(probe.burst(100, 41) == MMS_OK); queued = probe.Read();
    CHECK(queued.workerWasWorker && queued.values[1] == 140 && queued.values[2] == 1140 && queued.reads[1] == readsA && queued.reads[2] == readsB &&
        queued.subscriptionState.latestSequence == sequence + 41 && queued.subscriptionState.completedSequence == sequence);
    sequence += 41; s = probe.Complete(manager, sequence); CHECK(s.reads[1] == ++readsA && s.reads[2] == ++readsB && Caption(a) == L"140" && Caption(b) == L"1140");
    Phases(s, events, 3);
    events = s.events; auto work = s.subscriptionState.workGeneration; auto dependency = s.subscriptionState.submittedDependencyRevision;
    CHECK(probe.graph(7) == MMS_OK); s = probe.Complete(manager, sequence, work + 1);
    CHECK(s.prepares[2] == 1 && s.display[2] == 777 && s.displayRevisions[2] == 2 && s.subscriptionState.submittedDependencyRevision > dependency);
    CHECK(s.reads[1] == ++readsA && s.reads[2] == ++readsB); Phases(s, events, 7);
    events = s.events; work = s.subscriptionState.workGeneration; const auto excludedState = s.states[1];
    CHECK(probe.graph(5) == MMS_OK); s = probe.Complete(manager, sequence, work + 1);
    CHECK(s.reads[1] == ++readsA && s.reads[2] == readsB && s.states[1] == excludedState); Phases(s, events, 5);
    events = s.events; work = s.subscriptionState.workGeneration;
    CHECK(probe.graph(7) == MMS_OK); s = probe.Complete(manager, sequence, work + 1);
    CHECK(s.reads[1] == ++readsA && s.reads[2] == ++readsB); Phases(s, events, 7);
    // Explicit DATA on A wins over its native property read. Other members
    // remain in the same composite ticket, rather than parking the entire scope.
    events = s.events; CHECK(probe.bind(1) == MMS_OK && probe.set(1, 600) == MMS_OK && probe.notify(1, 1) == MMS_OK);
    s = probe.Complete(manager, ++sequence); CHECK(s.reads[1] == readsA && s.reads[2] == ++readsB && s.prepares[0] == 1 &&
        s.display[0] == 777 && s.displayRevisions[0] == 2 && s.stateValues[0] == 777 && Caption(a) == L"140"); Phases(s, events, 7);
    events = s.events; work = s.subscriptionState.workGeneration;
    CHECK(probe.set(1, 601) == MMS_OK && probe.bind(0) == MMS_OK);
    s = probe.Complete(manager, sequence, work + 1); CHECK(s.reads[1] == ++readsA && s.reads[2] == ++readsB && Caption(a) == L"601"); Phases(s, events, 7);
    // A failed original read is a real consumer wait. Clearing the failure is
    // not itself a notification and cannot cause a polling retry.
    CHECK(probe.fail(4) == MMS_OK && probe.set(1, 701) == MMS_OK && probe.set(2, 702) == MMS_OK && probe.notify(1, 1) == MMS_OK);
    s = probe.Waiting(manager, MMS_UI_PROPERTY_REFRESH_WAIT_CONSUMER); ++sequence;
    CHECK(s.subscriptionState.latestSequence == sequence && s.subscriptionState.lastResult == MMS_E_FAILED && s.reads[2] == ++readsB);
    const auto failedReads = Reads(s), failedEvents = s.events;
    for (unsigned i = 0; i < 8; ++i) { manager.Drain(); manager_refresh_checks::Pump(2); }
    CHECK(Reads(probe.Read()) == failedReads && probe.Read().events == failedEvents && !manager.Ui().Refresh().Pending());
    CHECK(probe.fail(0) == MMS_OK); manager_refresh_checks::Pump(4); CHECK(Reads(probe.Read()) == failedReads);
    events = probe.Read().events; CHECK(probe.notify(1, 1) == MMS_OK); s = probe.Complete(manager, ++sequence);
    CHECK(s.reads[2] == ++readsB && s.lastRead[1] == 701 && s.lastRead[2] == 702 && Caption(a) == L"701" && Caption(b) == L"702");
    Phases(s, events, 7); Stable(manager, revision, preferences); ReadOnly(s);
}
void Draft(Manager& manager, SharedHistory& history, unsigned& rejectedEdits, const Probe& probe, HWND field, ScopedPropertyProbeState& s) {
    CHECK(!(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY)); uint64_t revision = 0; CHECK(history.Revision(revision) == MMS_OK);
    const auto uiRevision = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision(), sequence = s.subscriptionState.completedSequence;
    native_property_input_checks::Text(field, L"-"); SendMessageW(field, EM_SETSEL, 0, 1); SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto events = s.events; const auto target = s.targetRefs[0];
    CHECK(probe.burst(500, 2) == MMS_OK); s = probe.Complete(manager, sequence + 2);
    CHECK(Caption(field) == L"-" && GetFocus() == field && Same(s.targetRefs[0], target) && s.lastRead[1] == 501);
    DWORD begin = 0, end = 0; SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end)); CHECK(!begin && end == 1);
    MSG tab{}; tab.hwnd = field; tab.message = WM_KEYDOWN; tab.wParam = VK_TAB;
    CHECK(SendMessageW(field, WM_GETDLGCODE, VK_TAB, reinterpret_cast<LPARAM>(&tab)) & DLGC_WANTTAB); Phases(s, events, 3);
    SendMessageW(field, WM_IME_ENDCOMPOSITION, 0, 0); native_property_input_checks::Escape(field); CHECK(Caption(field) == L"501");
    uint64_t current = 0; CHECK(history.Revision(current) == MMS_OK && current == revision && !history.UndoCount() && !history.RedoCount() &&
        !history.RetainedBytes() && !rejectedEdits && !probe.Read().stages); Stable(manager, uiRevision, preferences);
}
// lifecycle: 0 ordinary pending stop; 1 original seed removal; 2 trigger
// PropertyRecord replacement; 3 the actual queue project epoch transition.
void Run(const fs::path& root, const fs::path& library, uint32_t scope, unsigned lifecycle = 0, bool draft = false, bool legacy = false) {
    const std::string suffix = draft ? "draft" : legacy ? "legacy-self" : lifecycle ? "retire-" + std::to_string(lifecycle) :
        scope == MMS_UI_REFRESH_SELF ? "self" : scope == MMS_UI_REFRESH_SUBTREE ? "subtree" : "dependents";
    const auto id = "org.example.scoped-property-" + suffix;
    const auto directory = root / ("scoped-property-manager-" + suffix), package = directory / "PLUGINS" / "fixture";
    fs::create_directories(package); fs::copy_file(library, package / "scoped-property-probe.dll"); Write(package / "plugin.json", ManifestText(id, "scoped-property-probe.dll"));
    std::vector<std::string> logs; const auto log = [&](const std::string& message) { logs.push_back(message); };
    const auto count = [&](const char* text) { return std::count_if(logs.begin(), logs.end(), [&](const auto& s) { return s.find(text) != std::string::npos; }); };
    Manager manager(directory / "user", log); SharedHistory history(manager.Storage()); unsigned rejectedEdits = 0;
    if (draft) {
        // Test-only bridge: a real SharedHistory revision permits an editable
        // NativePropertyInput draft, while every commit is refused. This does
        // not attach the production edit host or expose the mms.edit service.
        manager.Ui().SetPropertyInputBridges([&](UiClient&, uint64_t& revision) { return history.Revision(revision); },
            [&](UiClient&, const PropertyInputSnapshot&, const MmsValue&) { ++rejectedEdits; return MMS_E_UNSUPPORTED; });
    }
    manager.Load(package.parent_path()); const auto statuses = manager.Status();
    if (statuses.size() != 1 || statuses[0].stage != "started") { for (const auto& s : statuses) std::cerr << "SCOPED_PROPERTY_START " << s.id << ' ' << s.stage << ' ' << s.error << '\n';
        for (const auto& s : logs) std::cerr << "SCOPED_PROPERTY_LOG " << s << '\n'; }
    CHECK(statuses.size() == 1 && statuses[0].stage == "started" && count("SCOPED_PROPERTY_START") == 1);
    const auto module = GetModuleHandleW(L"scoped-property-probe.dll"); Probe probe(module); Negotiation(probe.Read());
    manager_refresh_checks::Window window; CHECK(SetWindowPos(window.value, nullptr, 0, 0, 1000, 700, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, log); native->SetPropertyArea(800, 150, true);
    native->Refresh(); CHECK(native->Layout(10, 940, 160, true) > 0);
    const auto a = Field(native->Toolbar(), L"Scoped property A"), b = Field(native->PropertyArea(), L"Scoped property B");
    CHECK(GetParent(a) != GetParent(b) && Caption(a) == L"11" && Caption(b) == L"12" && (GetWindowLongPtrW(b, GWL_STYLE) & ES_READONLY));
    if (!draft) CHECK(GetWindowLongPtrW(a, GWL_STYLE) & ES_READONLY);
    const auto registry = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
    CHECK(probe.begin(scope, 0, 0) == MMS_OK); auto s = probe.Read(); CHECK(!Reads(s) && !s.prepares[0] && !s.prepares[2] && s.subscriptionState.pending);
    auto sequence = s.subscriptionState.latestSequence; CHECK(sequence);
    if (scope == MMS_UI_REFRESH_DEPENDENTS) { s = probe.Waiting(manager, MMS_UI_PROPERTY_REFRESH_WAIT_GRAPH); CHECK(!Reads(s) && !s.events);
        const auto work = s.subscriptionState.workGeneration; CHECK(probe.graph(3) == MMS_OK); s = probe.Complete(manager, sequence, work + 1);
        CHECK(s.reads[1] == 1 && s.reads[2] == 1 && !s.prepares[2]); Phases(s, 0, 3);
    } else { s = probe.Complete(manager, sequence); CHECK(s.reads[1] == 1 && !s.reads[2] && !s.prepares[2]); Phases(s, 0, 1); }
    Identity(manager, id, s); ReadOnly(s); Stable(manager, registry, preferences);
    CHECK(probe.threads() == MMS_OK); s = probe.Read();
    CHECK(s.workerQuery == MMS_OK && s.workerCaps == MMS_E_THREAD && s.workerRef == MMS_E_THREAD && s.workerSubscribe == MMS_E_THREAD &&
        s.workerUnsubscribe == MMS_E_THREAD && s.outputsPreserved);
    if (draft) Draft(manager, history, rejectedEdits, probe, a, s);
    else if (scope == MMS_UI_REFRESH_DEPENDENTS && !lifecycle) Dependents(manager, probe, a, b, s);
    else if (!lifecycle) { const auto events = s.events, reads = s.reads[1]; sequence = s.subscriptionState.completedSequence;
        CHECK(probe.burst(80, 3) == MMS_OK); const auto queued = probe.Read(); CHECK(queued.reads[1] == reads && queued.subscriptionState.latestSequence == sequence + 3);
        s = probe.Complete(manager, sequence + 3); CHECK(s.reads[1] == reads + 1 && !s.reads[2] && Caption(a) == L"82" && Caption(b) == L"12"); Phases(s, events, 1); }
    // The input HWND and its original target identity survive ordinary refreshes.
    CHECK(Field(native->Toolbar(), L"Scoped property A") == a && Field(native->PropertyArea(), L"Scoped property B") == b);
    s = probe.Read(); const auto stoppedReads = Reads(s); CHECK(probe.hold() == MMS_OK);
    if (lifecycle) {
        if (lifecycle == 1) CHECK(probe.removeSeed() == MMS_OK);
        else if (lifecycle == 2) CHECK(probe.replace() == MMS_OK);
        else manager.Ui().Refresh().AdvanceProjectEpoch();
        s = probe.Read(); CHECK(s.query == MMS_E_STALE && s.oldQuery == MMS_E_STALE && s.queryOutputPreserved);
        // Removing an independent seed retires the subscription, not the
        // original trigger property. Replacement and epoch changes retire it.
        CHECK(probe.notify(1, 1) == (lifecycle == 1 ? MMS_OK : MMS_E_STALE));
        for (unsigned i = 0; i < 4; ++i) { manager.Drain(); manager_refresh_checks::Pump(2); }
        CHECK(Reads(probe.Read()) == stoppedReads);
    } else CHECK(probe.notify(1, 1) == MMS_OK && probe.Read().subscriptionState.pending);
    CHECK(!manager.Stop() && manager.Status()[0].stage == "stopping" && GetModuleHandleW(L"scoped-property-probe.dll") == module);
    s = probe.Read(); CHECK(s.stops == 1 && s.readsAtStop == stoppedReads && s.stopQuery == MMS_E_STALE && s.stopNotify == MMS_E_STALE &&
        s.stopUnsubscribe == MMS_E_STALE && s.stopOutputPreserved && !s.subscription && s.heldSnapshot);
    const auto readLogs = count("SCOPED_PROPERTY_READ");
    for (unsigned i = 0; i < 4; ++i) { manager.Drain(); manager_refresh_checks::Pump(2); }
    CHECK(Reads(probe.Read()) == stoppedReads && count("SCOPED_PROPERTY_READ") == readLogs && !count("SCOPED_PROPERTY_DESTROY"));
    native.reset(); CHECK(probe.release() == MMS_OK && manager.Stop() && GetModuleHandleW(L"scoped-property-probe.dll") == nullptr);
    CHECK(count("SCOPED_PROPERTY_DESTROY") == 1 && manager.Ui().properties.empty() && manager.Ui().commands.empty() && manager.Ui().elements.size() == 6 && manager.Stop());
    CHECK(!history.UndoCount() && !history.RedoCount() && !rejectedEdits);
    if (draft) manager.Ui().SetPropertyInputBridges({}, {});
}
} // namespace manager_scoped_property_checks
static void ManagerScopedPropertyChecks(const fs::path& root, const fs::path& library) {
    using manager_scoped_property_checks::Run;
    Run(root, library, MMS_UI_REFRESH_DEPENDENTS);
    Run(root, library, MMS_UI_REFRESH_SUBTREE);
    Run(root, library, MMS_UI_REFRESH_SELF);
    Run(root, library, MMS_UI_REFRESH_SELF, 0, false, true);
    Run(root, library, MMS_UI_REFRESH_DEPENDENTS, 0, true);
    for (unsigned retirement = 1; retirement <= 3; ++retirement) Run(root, library, MMS_UI_REFRESH_DEPENDENTS, retirement);
}
