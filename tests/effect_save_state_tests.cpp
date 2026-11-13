// MAINTENANCE: pure-CPU coverage for the private effect save protocol.
//
// The provider, scene and completion consumer in this file are fakes. They
// exercise only the tracker's bookkeeping contract; nothing here reads or
// writes files, the registry, a window or the GPU, nothing loads MME, and
// nothing here proves real EMM persistence.
//
// Failures are collected per scenario and printed at the end; the process
// exits non-zero when any check fails.

#include "io/effect_save_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using mikumikustudio::effect_save::CaptureIdentity;
using mikumikustudio::effect_save::CompletionTracker;
using mikumikustudio::effect_save::Request;
using mikumikustudio::effect_save::SaveKind;
using mikumikustudio::effect_save::Stage;

static_assert(!std::is_copy_constructible_v<CompletionTracker>,
    "CompletionTracker must stay non-copyable");
static_assert(!std::is_copy_assignable_v<CompletionTracker>,
    "CompletionTracker must stay non-copyable");
static_assert(!std::is_move_constructible_v<CompletionTracker>,
    "CompletionTracker must stay non-movable");
static_assert(!std::is_move_assignable_v<CompletionTracker>,
    "CompletionTracker must stay non-movable");
static_assert(std::is_same_v<decltype(std::declval<CompletionTracker&>().GetRequest()), const Request&>,
    "GetRequest must return const Request&");
static_assert(
    std::is_same_v<decltype(std::declval<const CompletionTracker&>().GetRequest()), const Request&>,
    "GetRequest must return const Request& from a const tracker");
static_assert(std::is_copy_constructible_v<Request>,
    "Request must be copyable because the constructor takes it by value");

constexpr std::uint64_t kRequestId = 5100;

struct ScenarioReport {
    std::string name;
    int checks = 0;
    std::vector<std::string> failures;
};

class Checker {
public:
    explicit Checker(ScenarioReport& report) : report_(report) {}

    bool That(bool condition, const char* description, int line) {
        ++report_.checks;
        if (condition) return true;
        std::string message = "line " + std::to_string(line) + ": " + description;
        if (!context_.empty()) message += " [" + context_ + "]";
        report_.failures.push_back(std::move(message));
        return false;
    }

    void SetContext(std::string context) { context_ = std::move(context); }
    void ClearContext() { context_.clear(); }

private:
    ScenarioReport& report_;
    std::string context_;
};

#define CHECK(condition) checker.That((condition), #condition, __LINE__)

CaptureIdentity MakeIdentity() {
    CaptureIdentity identity;
    identity.sceneGeneration = 400;
    identity.sceneRevision = 7;
    identity.historyBinding = 0;
    identity.historyRevision = 0;
    identity.providerInstance = 900;
    identity.effectRevision = 12;
    identity.objectIds = {31, 32, 33};
    return identity;
}

Request MakeRequest(SaveKind kind = SaveKind::Manual) {
    Request request;
    request.requestId = kRequestId;
    request.identity = MakeIdentity();
    request.pmmPath = L"D:\\projects\\scene.pmm";
    request.emmPath = L"D:\\projects\\scene.emm";
    request.kind = kind;
    request.saveEffects = true;
    return request;
}

const char* SaveKindName(SaveKind kind) noexcept {
    switch (kind) {
    case SaveKind::Manual:
        return "Manual";
    case SaveKind::SaveAs:
        return "SaveAs";
    case SaveKind::Incremental:
        return "Incremental";
    case SaveKind::Backup:
        return "Backup";
    }
    return "unknown";
}

template <typename RequestFactory>
void ExpectRejected(Checker& checker, RequestFactory factory, const char* description, int line) {
    bool rejected = false;
    try {
        CompletionTracker tracker(factory());
        (void)tracker;
    } catch (const std::invalid_argument&) {
        rejected = true;
    } catch (...) {
        // A different exception type is still a failed expectation.
    }
    checker.That(rejected, description, line);
}

// Fake user scene. No file or window is involved; the counters only show
// whether a completion consumer touched the project path, dirty flag or open
// state.
struct FakeProjectState {
    std::wstring currentPath = L"D:\\projects\\original.pmm";
    bool dirty = true;
    bool open = true;
    int pathApplications = 0;
    int recentApplications = 0;
    int dirtyClears = 0;
    int closes = 0;
};

// Fake completion consumer: it applies a completion effect only when
// FinishAcknowledgement returned true and the tracker is Completed, and it
// still rechecks the tracker's queries for the caller's current identity.
struct SimulatedCompletionConsumer {
    void ApplyAfterSuccessfulFinish(const CompletionTracker& tracker, bool finishReturnedTrue,
        const CaptureIdentity& current, bool providerAvailable, FakeProjectState& project) const {
        if (!finishReturnedTrue || tracker.GetStage() != Stage::Completed) return;
        if (tracker.AllowsApplySceneCompletion(current, providerAvailable)) {
            project.currentPath = tracker.GetRequest().pmmPath;
            ++project.pathApplications;
            ++project.recentApplications;
        }
        if (tracker.AllowsClearCapturedDirty(current, providerAvailable)) {
            project.dirty = false;
            ++project.dirtyClears;
        }
        if (tracker.AllowsCloseAfterSave(current, providerAvailable)) {
            project.open = false;
            ++project.closes;
        }
    }
};

// Fake acknowledgement provider. The test invokes it exactly when
// BeginAcknowledgement returned true; it never talks to MME.
struct FakeAcknowledgementProvider {
    int invocations = 0;
    bool reentryRefused = false;

    void Invoke(CompletionTracker& tracker, const CaptureIdentity& current, bool providerAvailable) {
        ++invocations;
        reentryRefused =
            !tracker.BeginAcknowledgement(tracker.GetRequest().requestId, current, providerAvailable);
    }
};

void DriveToAwaitingAcknowledgement(Checker& checker, CompletionTracker& tracker) {
    const Request& request = tracker.GetRequest();
    CHECK(tracker.StartWrite(request.requestId));
    CHECK(tracker.FinishWrite(request.requestId, true, request.identity, true));
    CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);
}

void DriveToAcknowledging(Checker& checker, CompletionTracker& tracker) {
    const Request& request = tracker.GetRequest();
    CHECK(tracker.StartWrite(request.requestId));
    CHECK(tracker.FinishWrite(request.requestId, true, request.identity, true));
    CHECK(tracker.BeginAcknowledgement(request.requestId, request.identity, true));
    CHECK(tracker.GetStage() == Stage::Acknowledging);
}

void DriveToCompleted(Checker& checker, CompletionTracker& tracker, const CaptureIdentity& current) {
    const Request& request = tracker.GetRequest();
    CHECK(tracker.StartWrite(request.requestId));
    CHECK(tracker.FinishWrite(request.requestId, true, current, true));
    CHECK(tracker.BeginAcknowledgement(request.requestId, current, true));
    CHECK(tracker.FinishAcknowledgement(request.requestId, true, current, true));
    CHECK(tracker.GetStage() == Stage::Completed);
}

struct IdentityEdit {
    const char* description;
    void (*apply)(CaptureIdentity&);
};

void BumpSceneRevision(CaptureIdentity& identity) { identity.sceneRevision += 1; }
void BumpHistoryRevision(CaptureIdentity& identity) { identity.historyRevision += 1; }
void BumpEffectRevision(CaptureIdentity& identity) { identity.effectRevision += 1; }
void AddObjectId(CaptureIdentity& identity) { identity.objectIds.push_back(99); }
void RemoveObjectId(CaptureIdentity& identity) { identity.objectIds.pop_back(); }
void ReorderObjectIds(CaptureIdentity& identity) {
    std::swap(identity.objectIds.front(), identity.objectIds.back());
}

const IdentityEdit kIdentityEdits[] = {
    {"sceneRevision", BumpSceneRevision},
    {"historyRevision", BumpHistoryRevision},
    {"effectRevision", BumpEffectRevision},
    {"object added", AddObjectId},
    {"object removed", RemoveObjectId},
    {"object order", ReorderObjectIds},
};

void RunConstructionChecks(Checker& checker) {
    // The original configuration has no shared history binding and may capture
    // no objects; both are valid for tracking.
    {
        Request request = MakeRequest();
        request.identity.historyBinding = 0;
        request.identity.historyRevision = 0;
        request.identity.objectIds.clear();
        CompletionTracker tracker(request);
        CHECK(tracker.GetStage() == Stage::Ready);
        CHECK(!tracker.DiskCommitted());
        CHECK(tracker.GetRequest().identity == request.identity);
    }

    for (SaveKind kind : {SaveKind::Manual, SaveKind::SaveAs, SaveKind::Incremental, SaveKind::Backup}) {
        checker.SetContext(SaveKindName(kind));
        CompletionTracker tracker(MakeRequest(kind));
        CHECK(tracker.GetStage() == Stage::Ready);
        CHECK(tracker.GetRequest().kind == kind);
        CHECK(tracker.GetRequest().saveEffects);
        checker.ClearContext();
    }

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.requestId = 0;
        return request;
    }, "zero requestId must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.identity.sceneGeneration = 0;
        return request;
    }, "zero sceneGeneration must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.identity.providerInstance = 0;
        return request;
    }, "zero providerInstance must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.saveEffects = false;
        return request;
    }, "saveEffects=false must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.pmmPath.clear();
        return request;
    }, "empty pmmPath must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.emmPath.clear();
        return request;
    }, "empty emmPath must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.pmmPath.push_back(L'\0');
        return request;
    }, "embedded NUL in pmmPath must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.emmPath.push_back(L'\0');
        return request;
    }, "embedded NUL in emmPath must be rejected", __LINE__);

    ExpectRejected(checker, [] {
        Request request = MakeRequest();
        request.kind = static_cast<SaveKind>(97);
        return request;
    }, "unknown SaveKind must be rejected", __LINE__);
}

void RunIdentityEqualityChecks(Checker& checker) {
    const CaptureIdentity base = MakeIdentity();
    const auto expectScalarChange = [&](const char* description, const CaptureIdentity& changed) {
        checker.SetContext(description);
        CHECK(changed != base);
        CHECK(!(changed == base));
        CHECK(base != changed);
        checker.ClearContext();
    };

    CaptureIdentity same = base;
    CHECK(same == base);
    CHECK(!(same != base));

    {
        CaptureIdentity changed = base;
        changed.sceneGeneration += 1;
        expectScalarChange("sceneGeneration", changed);
    }
    {
        CaptureIdentity changed = base;
        changed.sceneRevision += 1;
        expectScalarChange("sceneRevision", changed);
    }
    {
        CaptureIdentity changed = base;
        changed.historyBinding += 1;
        expectScalarChange("historyBinding", changed);
    }
    {
        CaptureIdentity changed = base;
        changed.historyRevision += 1;
        expectScalarChange("historyRevision", changed);
    }
    {
        CaptureIdentity changed = base;
        changed.providerInstance += 1;
        expectScalarChange("providerInstance", changed);
    }
    {
        CaptureIdentity changed = base;
        changed.effectRevision += 1;
        expectScalarChange("effectRevision", changed);
    }

    {
        checker.SetContext("objectIds element");
        CaptureIdentity changed = base;
        changed.objectIds[1] = 4242;
        CHECK(changed != base);
        CHECK(!(changed == base));
        checker.ClearContext();
    }
    {
        checker.SetContext("objectIds order");
        CaptureIdentity changed = base;
        std::swap(changed.objectIds.front(), changed.objectIds.back());
        CHECK(changed != base);
        checker.ClearContext();
    }
    {
        checker.SetContext("objectIds length");
        CaptureIdentity longer = base;
        longer.objectIds.push_back(4242);
        CHECK(longer != base);
        CaptureIdentity shorter = base;
        shorter.objectIds.pop_back();
        CHECK(shorter != base);
        checker.ClearContext();
    }
    {
        checker.SetContext("empty object set");
        CaptureIdentity empty;
        CaptureIdentity alsoEmpty;
        CHECK(empty == alsoEmpty);
        CaptureIdentity notEmpty = base;
        CHECK(empty != notEmpty);
        CHECK(notEmpty != empty);
        checker.ClearContext();
    }
}

void RunRequestCopyChecks(Checker& checker) {
    Request original = MakeRequest();
    const Request expected = original;
    CompletionTracker tracker(original);

    // Mutating the caller's request after construction must not reach the
    // tracker's sealed copy.
    original.requestId = kRequestId + 1;
    original.identity.sceneGeneration += 10;
    original.identity.providerInstance += 10;
    original.identity.objectIds.push_back(999);
    original.pmmPath = L"D:\\elsewhere\\other.pmm";
    original.emmPath = L"D:\\elsewhere\\other.emm";
    original.kind = SaveKind::Backup;
    original.saveEffects = false;

    const Request& internal = tracker.GetRequest();
    CHECK(internal.requestId == expected.requestId);
    CHECK(internal.identity == expected.identity);
    CHECK(internal.pmmPath == expected.pmmPath);
    CHECK(internal.emmPath == expected.emmPath);
    CHECK(internal.kind == expected.kind);
    CHECK(internal.saveEffects == expected.saveEffects);
    CHECK(tracker.GetStage() == Stage::Ready);

    // GetRequest hands out one stable reference to the immutable copy.
    const CompletionTracker& constTracker = tracker;
    CHECK(&constTracker.GetRequest() == &internal);
}

void RunCompletionChecks(Checker& checker) {
    for (SaveKind kind : {SaveKind::Manual, SaveKind::SaveAs, SaveKind::Incremental}) {
        checker.SetContext(SaveKindName(kind));
        Request request = MakeRequest(kind);
        const CaptureIdentity captured = request.identity;
        const std::uint64_t id = request.requestId;
        CompletionTracker tracker(request);
        FakeProjectState project;
        SimulatedCompletionConsumer consumer;

        CHECK(tracker.GetStage() == Stage::Ready);
        CHECK(!tracker.DiskCommitted());
        CHECK(tracker.StartWrite(id));
        CHECK(tracker.GetStage() == Stage::Writing);
        CHECK(!tracker.DiskCommitted());
        CHECK(tracker.FinishWrite(id, true, captured, true));
        CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);
        CHECK(tracker.DiskCommitted());
        CHECK(tracker.BeginAcknowledgement(id, captured, true));
        CHECK(tracker.GetStage() == Stage::Acknowledging);
        CHECK(tracker.DiskCommitted());
        const bool finished = tracker.FinishAcknowledgement(id, true, captured, true);
        CHECK(finished);
        CHECK(tracker.GetStage() == Stage::Completed);
        CHECK(tracker.DiskCommitted());

        consumer.ApplyAfterSuccessfulFinish(tracker, finished, captured, true, project);
        CHECK(project.pathApplications == 1);
        CHECK(project.recentApplications == 1);
        CHECK(project.dirtyClears == 1);
        CHECK(project.closes == 1);
        CHECK(project.currentPath == request.pmmPath);
        CHECK(!project.dirty);
        CHECK(!project.open);

        // A duplicate receipt or a foreign requestId is refused and must not be
        // consumed a second time.
        const bool duplicate = tracker.FinishAcknowledgement(id, true, captured, true);
        const bool foreign = tracker.FinishAcknowledgement(id + 1, true, captured, true);
        CHECK(!duplicate);
        CHECK(!foreign);
        CHECK(tracker.GetStage() == Stage::Completed);
        consumer.ApplyAfterSuccessfulFinish(tracker, duplicate, captured, true, project);
        consumer.ApplyAfterSuccessfulFinish(tracker, foreign, captured, true, project);
        CHECK(project.pathApplications == 1);
        CHECK(project.recentApplications == 1);
        CHECK(project.dirtyClears == 1);
        CHECK(project.closes == 1);
        checker.ClearContext();
    }
}

void RunBackupChecks(Checker& checker) {
    Request request = MakeRequest(SaveKind::Backup);
    const CaptureIdentity captured = request.identity;
    const std::uint64_t id = request.requestId;
    CompletionTracker tracker(request);
    FakeProjectState project;
    const std::wstring originalPath = project.currentPath;
    SimulatedCompletionConsumer consumer;

    CHECK(tracker.StartWrite(id));
    CHECK(tracker.FinishWrite(id, true, captured, true));
    CHECK(tracker.BeginAcknowledgement(id, captured, true));
    const bool finished = tracker.FinishAcknowledgement(id, true, captured, true);
    CHECK(finished);
    CHECK(tracker.GetStage() == Stage::Completed);
    CHECK(tracker.DiskCommitted());

    consumer.ApplyAfterSuccessfulFinish(tracker, finished, captured, true, project);
    CHECK(project.pathApplications == 0);
    CHECK(project.recentApplications == 0);
    CHECK(project.dirtyClears == 0);
    CHECK(project.closes == 0);
    CHECK(project.currentPath == originalPath);
    CHECK(project.dirty);
    CHECK(project.open);

    CHECK(!tracker.AllowsApplySceneCompletion(captured, true));
    CHECK(!tracker.AllowsClearCapturedDirty(captured, true));
    CHECK(!tracker.AllowsCloseAfterSave(captured, true));
}

void RunEventGuardChecks(Checker& checker) {
    Request request = MakeRequest();
    const CaptureIdentity captured = request.identity;
    const std::uint64_t id = request.requestId;
    const std::uint64_t foreign = id + 1;
    CompletionTracker tracker(request);

    // Ready: later-stage events and foreign ids must not move the state.
    CHECK(!tracker.BeginAcknowledgement(id, captured, true));
    CHECK(!tracker.FinishAcknowledgement(id, true, captured, true));
    CHECK(!tracker.FinishWrite(id, true, captured, true));
    CHECK(!tracker.StartWrite(foreign));
    CHECK(!tracker.CancelBeforeWrite(foreign));
    CHECK(tracker.GetStage() == Stage::Ready);
    CHECK(!tracker.DiskCommitted());

    // Writing: duplicate start, cancel, acknowledgement and a foreign finish
    // are refused.
    CHECK(tracker.StartWrite(id));
    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.StartWrite(foreign));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.BeginAcknowledgement(id, captured, true));
    CHECK(!tracker.FinishAcknowledgement(id, true, captured, true));
    CHECK(!tracker.FinishWrite(foreign, true, captured, true));
    CHECK(tracker.GetStage() == Stage::Writing);
    CHECK(!tracker.DiskCommitted());
    CHECK(tracker.FinishWrite(id, true, captured, true));
    CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);
    CHECK(tracker.DiskCommitted());

    // A committed stage cannot be restarted, cancelled or written again.
    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, captured, true));
    CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);
    CHECK(tracker.DiskCommitted());

    CHECK(!tracker.BeginAcknowledgement(foreign, captured, true));
    CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);
    CHECK(tracker.BeginAcknowledgement(id, captured, true));
    CHECK(tracker.GetStage() == Stage::Acknowledging);
    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, captured, true));
    CHECK(!tracker.BeginAcknowledgement(id, captured, true));
    CHECK(!tracker.FinishAcknowledgement(foreign, true, captured, true));
    CHECK(tracker.GetStage() == Stage::Acknowledging);
    CHECK(tracker.DiskCommitted());

    CHECK(tracker.FinishAcknowledgement(id, true, captured, true));
    CHECK(tracker.GetStage() == Stage::Completed);
    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, captured, true));
    CHECK(!tracker.BeginAcknowledgement(id, captured, true));
    CHECK(!tracker.FinishAcknowledgement(id, true, captured, true));
    CHECK(tracker.GetStage() == Stage::Completed);
    CHECK(tracker.DiskCommitted());

    {
        CompletionTracker uncommitted(MakeRequest());
        const Request& inner = uncommitted.GetRequest();
        CHECK(uncommitted.StartWrite(inner.requestId));
        CHECK(uncommitted.FinishWrite(inner.requestId, false, inner.identity, true));
        CHECK(uncommitted.GetStage() == Stage::NotCommitted);
        CHECK(!uncommitted.DiskCommitted());
        CHECK(!uncommitted.AllowsApplySceneCompletion(inner.identity, true));
        CHECK(!uncommitted.AllowsClearCapturedDirty(inner.identity, true));
        CHECK(!uncommitted.AllowsCloseAfterSave(inner.identity, true));
        CHECK(!uncommitted.StartWrite(inner.requestId));
        CHECK(!uncommitted.CancelBeforeWrite(inner.requestId));
        CHECK(!uncommitted.FinishWrite(inner.requestId, true, inner.identity, true));
        CHECK(!uncommitted.BeginAcknowledgement(inner.requestId, inner.identity, true));
        CHECK(!uncommitted.FinishAcknowledgement(inner.requestId, true, inner.identity, true));
        CHECK(uncommitted.GetStage() == Stage::NotCommitted);
        CHECK(!uncommitted.DiskCommitted());
    }
    {
        CompletionTracker cancelled(MakeRequest());
        const Request& inner = cancelled.GetRequest();
        CHECK(!cancelled.FinishWrite(inner.requestId, true, inner.identity, true));
        CHECK(cancelled.CancelBeforeWrite(inner.requestId));
        CHECK(cancelled.GetStage() == Stage::CancelledBeforeWrite);
        CHECK(!cancelled.DiskCommitted());
        CHECK(!cancelled.StartWrite(inner.requestId));
        CHECK(!cancelled.FinishWrite(inner.requestId, true, inner.identity, true));
        CHECK(!cancelled.CancelBeforeWrite(inner.requestId));
        CHECK(!cancelled.BeginAcknowledgement(inner.requestId, inner.identity, true));
        CHECK(!cancelled.FinishAcknowledgement(inner.requestId, true, inner.identity, true));
        CHECK(cancelled.GetStage() == Stage::CancelledBeforeWrite);
        CHECK(!cancelled.AllowsApplySceneCompletion(inner.identity, true));
        CHECK(!cancelled.AllowsClearCapturedDirty(inner.identity, true));
        CHECK(!cancelled.AllowsCloseAfterSave(inner.identity, true));
    }
}

void CheckFinishWriteIdentityChange(Checker& checker, const CaptureIdentity& changed,
    bool providerAvailable, Stage expectedStage) {
    Request request = MakeRequest();
    CompletionTracker tracker(request);
    const std::uint64_t id = request.requestId;

    CHECK(tracker.StartWrite(id));
    CHECK(tracker.FinishWrite(id, true, changed, providerAvailable));
    CHECK(tracker.GetStage() == expectedStage);
    CHECK(tracker.DiskCommitted());
    CHECK(!tracker.AllowsApplySceneCompletion(changed, providerAvailable));
    CHECK(!tracker.AllowsClearCapturedDirty(changed, providerAvailable));
    CHECK(!tracker.AllowsCloseAfterSave(changed, providerAvailable));

    // The committed outcome stays terminal.
    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, changed, providerAvailable));
    CHECK(!tracker.BeginAcknowledgement(id, changed, providerAvailable));
    CHECK(!tracker.FinishAcknowledgement(id, true, changed, providerAvailable));
    CHECK(tracker.GetStage() == expectedStage);
    CHECK(tracker.DiskCommitted());
}

void RunFinishWriteBoundaryChecks(Checker& checker) {
    {
        checker.SetContext("sceneGeneration changed before FinishWrite");
        CaptureIdentity changed = MakeIdentity();
        changed.sceneGeneration += 1;
        CheckFinishWriteIdentityChange(checker, changed, true, Stage::CommittedStaleScene);
        checker.ClearContext();
    }
    {
        checker.SetContext("historyBinding changed before FinishWrite");
        CaptureIdentity changed = MakeIdentity();
        changed.historyBinding += 1;
        CheckFinishWriteIdentityChange(checker, changed, true, Stage::CommittedStaleScene);
        checker.ClearContext();
    }
    {
        checker.SetContext("providerInstance changed before FinishWrite");
        CaptureIdentity changed = MakeIdentity();
        changed.providerInstance += 1;
        CheckFinishWriteIdentityChange(checker, changed, true, Stage::CommittedProviderRetired);
        checker.ClearContext();
    }
    {
        checker.SetContext("provider unavailable before FinishWrite");
        CheckFinishWriteIdentityChange(checker, MakeIdentity(), false,
            Stage::CommittedProviderRetired);
        checker.ClearContext();
    }
}

void CheckBeginIdentityChange(Checker& checker, const CaptureIdentity& changed,
    bool providerAvailable, Stage expectedStage) {
    CompletionTracker tracker(MakeRequest());
    const std::uint64_t id = tracker.GetRequest().requestId;
    DriveToAwaitingAcknowledgement(checker, tracker);

    FakeAcknowledgementProvider fakeProvider;
    const bool begun = tracker.BeginAcknowledgement(id, changed, providerAvailable);
    CHECK(!begun);
    // The fake provider is invoked only after a true BeginAcknowledgement.
    if (begun) fakeProvider.Invoke(tracker, changed, providerAvailable);
    CHECK(fakeProvider.invocations == 0);

    CHECK(tracker.GetStage() == expectedStage);
    CHECK(tracker.DiskCommitted());
    CHECK(!tracker.AllowsApplySceneCompletion(changed, providerAvailable));
    CHECK(!tracker.AllowsClearCapturedDirty(changed, providerAvailable));
    CHECK(!tracker.AllowsCloseAfterSave(changed, providerAvailable));

    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, changed, providerAvailable));
    CHECK(tracker.GetStage() == expectedStage);
    CHECK(tracker.DiskCommitted());
}

void RunBeginAcknowledgementChecks(Checker& checker) {
    {
        checker.SetContext("sceneGeneration changed before BeginAcknowledgement");
        CaptureIdentity changed = MakeIdentity();
        changed.sceneGeneration += 1;
        CheckBeginIdentityChange(checker, changed, true, Stage::CommittedStaleScene);
        checker.ClearContext();
    }
    {
        checker.SetContext("historyBinding changed before BeginAcknowledgement");
        CaptureIdentity changed = MakeIdentity();
        changed.historyBinding += 1;
        CheckBeginIdentityChange(checker, changed, true, Stage::CommittedStaleScene);
        checker.ClearContext();
    }
    {
        checker.SetContext("providerInstance changed before BeginAcknowledgement");
        CaptureIdentity changed = MakeIdentity();
        changed.providerInstance += 1;
        CheckBeginIdentityChange(checker, changed, true, Stage::CommittedProviderRetired);
        checker.ClearContext();
    }
    {
        checker.SetContext("provider unavailable before BeginAcknowledgement");
        CheckBeginIdentityChange(checker, MakeIdentity(), false,
            Stage::CommittedProviderRetired);
        checker.ClearContext();
    }

    {
        checker.SetContext("reentrant callback");
        Request request = MakeRequest();
        const CaptureIdentity captured = request.identity;
        const std::uint64_t id = request.requestId;
        CompletionTracker tracker(request);
        DriveToAwaitingAcknowledgement(checker, tracker);

        FakeAcknowledgementProvider fakeProvider;
        CHECK(!tracker.BeginAcknowledgement(id + 1, captured, true));
        CHECK(fakeProvider.invocations == 0);
        CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);

        const bool begun = tracker.BeginAcknowledgement(id, captured, true);
        CHECK(begun);
        if (begun) fakeProvider.Invoke(tracker, captured, true);
        CHECK(fakeProvider.invocations == 1);
        CHECK(fakeProvider.reentryRefused);
        CHECK(tracker.GetStage() == Stage::Acknowledging);
        CHECK(tracker.FinishAcknowledgement(id, true, captured, true));
        CHECK(tracker.GetStage() == Stage::Completed);
        checker.ClearContext();
    }
}

void CheckPostCallbackIdentityChange(Checker& checker, const CaptureIdentity& afterCallback,
    bool providerAvailable, Stage expectedStage) {
    CompletionTracker tracker(MakeRequest());
    const std::uint64_t id = tracker.GetRequest().requestId;
    DriveToAcknowledging(checker, tracker);

    FakeProjectState project;
    SimulatedCompletionConsumer consumer;
    const std::wstring originalPath = project.currentPath;

    // The callback returned, but the scene or provider no longer matches.
    const bool accepted = tracker.FinishAcknowledgement(id, true, afterCallback, providerAvailable);
    CHECK(accepted);
    CHECK(tracker.GetStage() == expectedStage);
    CHECK(tracker.DiskCommitted());

    // A true receipt grants effects only for Stage::Completed.
    consumer.ApplyAfterSuccessfulFinish(tracker, accepted, afterCallback, providerAvailable, project);
    CHECK(project.pathApplications == 0);
    CHECK(project.recentApplications == 0);
    CHECK(project.dirtyClears == 0);
    CHECK(project.closes == 0);
    CHECK(project.currentPath == originalPath);
    CHECK(project.dirty);
    CHECK(project.open);

    CHECK(!tracker.AllowsApplySceneCompletion(afterCallback, providerAvailable));
    CHECK(!tracker.AllowsClearCapturedDirty(afterCallback, providerAvailable));
    CHECK(!tracker.AllowsCloseAfterSave(afterCallback, providerAvailable));

    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, afterCallback, providerAvailable));
    CHECK(tracker.GetStage() == expectedStage);
    CHECK(tracker.DiskCommitted());
}

void RunPostCallbackChecks(Checker& checker) {
    {
        checker.SetContext("sceneGeneration replaced during callback");
        CaptureIdentity changed = MakeIdentity();
        changed.sceneGeneration += 1;
        CheckPostCallbackIdentityChange(checker, changed, true, Stage::CommittedStaleScene);
        checker.ClearContext();
    }
    {
        checker.SetContext("historyBinding replaced during callback");
        CaptureIdentity changed = MakeIdentity();
        changed.historyBinding += 1;
        CheckPostCallbackIdentityChange(checker, changed, true, Stage::CommittedStaleScene);
        checker.ClearContext();
    }
    {
        checker.SetContext("providerInstance replaced during callback");
        CaptureIdentity changed = MakeIdentity();
        changed.providerInstance += 1;
        CheckPostCallbackIdentityChange(checker, changed, true, Stage::CommittedProviderRetired);
        checker.ClearContext();
    }
    {
        checker.SetContext("provider unavailable after callback");
        CheckPostCallbackIdentityChange(checker, MakeIdentity(), false,
            Stage::CommittedProviderRetired);
        checker.ClearContext();
    }
}

void RunFailedAcknowledgementChecks(Checker& checker) {
    Request request = MakeRequest();
    const CaptureIdentity captured = request.identity;
    const std::uint64_t id = request.requestId;
    CompletionTracker tracker(request);
    DriveToAcknowledging(checker, tracker);

    // The fake acknowledgement callback throws instead of returning.
    const auto fakeThrowingCallback = []() {
        throw std::runtime_error("simulated provider callback failure");
    };
    bool callbackThrew = false;
    try {
        fakeThrowingCallback();
    } catch (const std::runtime_error&) {
        callbackThrew = true;
        CHECK(tracker.FinishAcknowledgement(id, false, captured, true));
    }
    CHECK(callbackThrew);
    CHECK(tracker.GetStage() == Stage::CommittedAcknowledgementFailed);
    CHECK(tracker.DiskCommitted());

    FakeProjectState project;
    SimulatedCompletionConsumer consumer;
    const std::wstring originalPath = project.currentPath;
    consumer.ApplyAfterSuccessfulFinish(tracker, true, captured, true, project);
    CHECK(project.pathApplications == 0);
    CHECK(project.recentApplications == 0);
    CHECK(project.dirtyClears == 0);
    CHECK(project.closes == 0);
    CHECK(project.currentPath == originalPath);
    CHECK(project.dirty);
    CHECK(project.open);

    CHECK(!tracker.FinishAcknowledgement(id, false, captured, true));
    CHECK(!tracker.FinishAcknowledgement(id, true, captured, true));
    CHECK(tracker.GetStage() == Stage::CommittedAcknowledgementFailed);
    CHECK(tracker.DiskCommitted());
    CHECK(!tracker.StartWrite(id));
    CHECK(!tracker.CancelBeforeWrite(id));
    CHECK(!tracker.FinishWrite(id, false, captured, true));
    CHECK(!tracker.BeginAcknowledgement(id, captured, true));
    CHECK(tracker.GetStage() == Stage::CommittedAcknowledgementFailed);
}

void RunEditsDuringSaveChecks(Checker& checker) {
    for (const IdentityEdit& edit : kIdentityEdits) {
        {
            checker.SetContext(std::string("before FinishWrite: ") + edit.description);
            Request request = MakeRequest();
            const CaptureIdentity captured = request.identity;
            const std::uint64_t id = request.requestId;
            CompletionTracker tracker(request);
            CHECK(tracker.StartWrite(id));
            CaptureIdentity edited = captured;
            edit.apply(edited);
            CHECK(edited != captured);
            CHECK(tracker.FinishWrite(id, true, edited, true));
            CHECK(tracker.GetStage() == Stage::AwaitingAcknowledgement);
            CHECK(tracker.BeginAcknowledgement(id, edited, true));
            CHECK(tracker.FinishAcknowledgement(id, true, edited, true));
            CHECK(tracker.GetStage() == Stage::Completed);
            CHECK(tracker.DiskCommitted());
            CHECK(tracker.AllowsApplySceneCompletion(edited, true));
            CHECK(!tracker.AllowsClearCapturedDirty(edited, true));
            CHECK(!tracker.AllowsCloseAfterSave(edited, true));
            CHECK(tracker.AllowsClearCapturedDirty(captured, true));
            checker.ClearContext();
        }
        {
            checker.SetContext(std::string("inside acknowledgement: ") + edit.description);
            CompletionTracker tracker(MakeRequest());
            const Request& request = tracker.GetRequest();
            const CaptureIdentity captured = request.identity;
            const std::uint64_t id = request.requestId;
            DriveToAcknowledging(checker, tracker);
            CaptureIdentity edited = captured;
            edit.apply(edited);
            CHECK(edited != captured);
            CHECK(tracker.FinishAcknowledgement(id, true, edited, true));
            CHECK(tracker.GetStage() == Stage::Completed);
            CHECK(tracker.DiskCommitted());
            CHECK(tracker.AllowsApplySceneCompletion(edited, true));
            CHECK(!tracker.AllowsClearCapturedDirty(edited, true));
            CHECK(!tracker.AllowsCloseAfterSave(edited, true));
            checker.ClearContext();
        }
    }
}

void RunCompletedRevalidationChecks(Checker& checker) {
    Request request = MakeRequest();
    const CaptureIdentity captured = request.identity;
    CompletionTracker tracker(request);
    DriveToCompleted(checker, tracker, captured);
    CHECK(tracker.DiskCommitted());
    CHECK(tracker.AllowsApplySceneCompletion(captured, true));
    CHECK(tracker.AllowsClearCapturedDirty(captured, true));
    CHECK(tracker.AllowsCloseAfterSave(captured, true));

    CaptureIdentity changed = captured;
    changed.sceneGeneration += 1;
    checker.SetContext("sceneGeneration after Completed");
    CHECK(!tracker.AllowsApplySceneCompletion(changed, true));
    CHECK(!tracker.AllowsClearCapturedDirty(changed, true));
    CHECK(!tracker.AllowsCloseAfterSave(changed, true));
    checker.ClearContext();

    changed = captured;
    changed.historyBinding += 1;
    checker.SetContext("historyBinding after Completed");
    CHECK(!tracker.AllowsApplySceneCompletion(changed, true));
    CHECK(!tracker.AllowsClearCapturedDirty(changed, true));
    CHECK(!tracker.AllowsCloseAfterSave(changed, true));
    checker.ClearContext();

    changed = captured;
    changed.providerInstance += 1;
    checker.SetContext("providerInstance after Completed");
    CHECK(!tracker.AllowsApplySceneCompletion(changed, true));
    CHECK(!tracker.AllowsClearCapturedDirty(changed, true));
    CHECK(!tracker.AllowsCloseAfterSave(changed, true));
    checker.ClearContext();

    checker.SetContext("provider unavailable after Completed");
    CHECK(!tracker.AllowsApplySceneCompletion(captured, false));
    CHECK(!tracker.AllowsClearCapturedDirty(captured, false));
    CHECK(!tracker.AllowsCloseAfterSave(captured, false));
    checker.ClearContext();

    for (const IdentityEdit& edit : kIdentityEdits) {
        checker.SetContext(std::string("after Completed: ") + edit.description);
        CaptureIdentity edited = captured;
        edit.apply(edited);
        CHECK(tracker.AllowsApplySceneCompletion(edited, true));
        CHECK(!tracker.AllowsClearCapturedDirty(edited, true));
        CHECK(!tracker.AllowsCloseAfterSave(edited, true));
        checker.ClearContext();
    }

    // Restoring the captured identity re-authorizes immediately; no stale
    // denial or old authorization is cached.
    CHECK(tracker.AllowsApplySceneCompletion(captured, true));
    CHECK(tracker.AllowsClearCapturedDirty(captured, true));
    CHECK(tracker.AllowsCloseAfterSave(captured, true));
    CHECK(!tracker.AllowsClearCapturedDirty(changed, true));
    CHECK(tracker.AllowsClearCapturedDirty(captured, true));
}

#include "effect_save_capture_checks.inc"

struct ScenarioCase {
    const char* name;
    void (*run)(Checker&);
};

}  // namespace

int main() {
    const ScenarioCase scenarios[] = {
        {"construction rejects untrackable requests", RunConstructionChecks},
        {"capture identity equality is strict", RunIdentityEqualityChecks},
        {"sealed request is copied and exposed as an immutable snapshot", RunRequestCopyChecks},
        {"manual, save-as and incremental completions apply once", RunCompletionChecks},
        {"backup completion keeps the current project path and dirty state", RunBackupChecks},
        {"duplicate, foreign and out-of-order events change nothing", RunEventGuardChecks},
        {"finish write reports stale scene and retired provider", RunFinishWriteBoundaryChecks},
        {"begin acknowledgement refuses stale scene and retired provider",
            RunBeginAcknowledgementChecks},
        {"post-callback identity changes end stale or retired", RunPostCallbackChecks},
        {"throwing acknowledgement ends committed but failed", RunFailedAcknowledgementChecks},
        {"edits during saving keep the snapshot result only", RunEditsDuringSaveChecks},
        {"completed result revalidates every query", RunCompletedRevalidationChecks},
        {"capture seals host-owned effect bytes, identity and digest", RunImmutableCaptureChecks},
        {"capture rejects stale, malformed, unsupported and cancelled attempts", RunCaptureRejections},
        {"capture accepts an exact-size payload at the hard budget", RunCaptureBudgetBoundary},
    };
    const int scenarioCount = static_cast<int>(sizeof(scenarios) / sizeof(scenarios[0]));

    std::vector<ScenarioReport> reports;
    reports.reserve(static_cast<std::size_t>(scenarioCount));
    int totalChecks = 0;
    int failedChecks = 0;
    int failedScenarios = 0;
    for (const ScenarioCase& scenario : scenarios) {
        ScenarioReport report;
        report.name = scenario.name;
        Checker checker(report);
        try {
            scenario.run(checker);
        } catch (const std::exception& error) {
            checker.That(false, ("unexpected exception: " + std::string(error.what())).c_str(),
                __LINE__);
        } catch (...) {
            checker.That(false, "unexpected non-standard exception", __LINE__);
        }
        totalChecks += report.checks;
        failedChecks += static_cast<int>(report.failures.size());
        if (!report.failures.empty()) ++failedScenarios;
        reports.push_back(std::move(report));
    }

    std::printf(
        "effect_save_state protocol scenarios (fake provider/scene/consumer; no MME or EMM access)\n");
    for (const ScenarioReport& report : reports) {
        std::printf("[%s] %s (%d checks)\n", report.failures.empty() ? "PASS" : "FAIL",
            report.name.c_str(), report.checks);
        for (const std::string& failure : report.failures) {
            std::printf("    %s\n", failure.c_str());
        }
    }
    std::printf("\n%d of %d scenarios passed; %d of %d checks failed\n",
        scenarioCount - failedScenarios, scenarioCount, failedChecks, totalChecks);
    if (failedScenarios == 0) {
        std::printf("PASS effect_save_state protocol scenarios\n");
        return 0;
    }
    std::printf("FAIL effect_save_state protocol scenarios\n");
    return 1;
}
