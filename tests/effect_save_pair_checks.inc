// Included by effect_save_state_tests.cpp. No on-disk files or real provider.
using mikumikustudio::effect_save::AcknowledgementCallbacks;
using mikumikustudio::effect_save::ArtifactFingerprint;
using mikumikustudio::effect_save::LiveSaveState;
using mikumikustudio::effect_save::PairSeal;
using mikumikustudio::effect_save::PairTransaction;
using mikumikustudio::effect_save::PublicationReceipt;
using mikumikustudio::effect_save::PublicationStatus;
using mikumikustudio::effect_save::SidecarFingerprint;
using mikumikustudio::effect_save::StagedProjectEvidence;

static_assert(!std::is_copy_constructible_v<PairTransaction> && !std::is_move_constructible_v<PairTransaction>);
static_assert(std::is_same_v<decltype(std::declval<PairTransaction&>().GetSeal()), const PairSeal&>);

ArtifactFingerprint Fingerprint(std::string_view bytes) {
    return {bytes.size(), FakeCapture::FakeDigest({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()})};
}

struct FakePair {
    FakeCapture provider;
    const std::string pmm = "PMM snapshot with FOV 61";
    const std::string sidecar = "plugin data for FOV 61";
    StagedProjectEvidence project;
    CaptureAttempt capture;

    explicit FakePair(SaveKind kind = SaveKind::Manual, bool withSidecar = true)
        : project{SetKind(kind), Fingerprint(pmm), {}}, capture(Freeze()) {
        if (withSidecar)
            project.sidecar = SidecarFingerprint{project.request.pmmPath + L".mms-plugins.json", Fingerprint(sidecar)};
    }

    Request SetKind(SaveKind kind) {
        provider.request.kind = kind;
        return provider.request;
    }

    CaptureAttempt Freeze() {
        CaptureAdmission admission;
        return admission.Capture(provider.request, provider.providerBytes.size(), provider.Callbacks());
    }

    // Simulate the worker publishing the immutable EMM and staged project.
    // Build the receipt from what was actually published, not the input seal.
    PublicationReceipt Publish() const {
        const auto emm = capture.snapshot->Bytes();
        return {PublicationStatus::Committed, {project.request, Fingerprint(pmm),
            Fingerprint({reinterpret_cast<const char*>(emm.data), emm.size}), project.sidecar ?
                std::optional<SidecarFingerprint>{{project.sidecar->path, Fingerprint(sidecar)}} : std::nullopt}};
    }
};

void RunPairSealChecks(Checker& checker) {
    FakePair fake;
    if (!CHECK(bool(fake.capture.snapshot))) return;
    const auto reject = [&](const char* name, const auto& change) {
        checker.SetContext(name);
        auto evidence = fake.project;
        change(evidence);
        bool rejected = false;
        try { PairTransaction transaction(*fake.capture.snapshot, evidence); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    };
    reject("different request id", [](auto& e) { ++e.request.requestId; });
    reject("different PMM name", [](auto& e) { e.request.pmmPath += L".other"; });
    reject("different EMM name", [](auto& e) { e.request.emmPath += L".other"; });
    reject("different save kind", [](auto& e) { e.request.kind = SaveKind::Backup; });
    reject("effect save not requested", [](auto& e) { e.request.saveEffects = false; });
    reject("different project identity", [](auto& e) { ++e.request.identity.sceneRevision; });
    reject("different effect identity", [](auto& e) { ++e.request.identity.effectRevision; });
    reject("different object ordering", [](auto& e) { ReorderObjectIds(e.request.identity); });
    reject("empty PMM fingerprint", [](auto& e) { e.pmm.size = 0; });
    reject("empty sidecar fingerprint", [](auto& e) { e.sidecar->file.size = 0; });
    reject("unresolved sidecar", [](auto& e) { e.sidecar->path.clear(); });
    reject("sidecar with embedded NUL", [](auto& e) { e.sidecar->path.push_back(L'\0'); });
    reject("sidecar is PMM", [](auto& e) { e.sidecar->path = e.request.pmmPath; });
    reject("sidecar is EMM", [](auto& e) { e.sidecar->path = e.request.emmPath; });
    checker.ClearContext();
}

void RunPairSuccessChecks(Checker& checker) {
    for (auto kind : {SaveKind::Manual, SaveKind::SaveAs, SaveKind::Incremental, SaveKind::Backup}) {
        for (bool withSidecar : {false, true}) {
            checker.SetContext(std::string(SaveKindName(kind)) + (withSidecar ? " with sidecar" : " without sidecar"));
            FakePair fake(kind, withSidecar);
            if (!CHECK(bool(fake.capture.snapshot))) continue;
            const auto effectFingerprint = Fingerprint(fake.provider.providerBytes);
            auto live = *fake.provider.live;
            const auto frozen = live;
            PairTransaction transaction(*fake.capture.snapshot, fake.project);
            CHECK(!transaction.DiskCommitted());
            CHECK(transaction.StartWrite(kRequestId));
            BumpSceneRevision(live);
            BumpEffectRevision(live);
            AddObjectId(live);
            fake.provider.providerBytes = "[Info]\nPmd1=newer.fx\n";
            CHECK(transaction.GetSeal().emm == effectFingerprint);
            CHECK(!(transaction.GetSeal().emm == Fingerprint(fake.provider.providerBytes)));
            CHECK(transaction.GetSeal().request.identity == frozen);
            const auto published = fake.Publish();
            CHECK(transaction.GetSeal() == published.seal);
            CHECK(bool(published.seal.sidecar) == withSidecar);
            // The caller's stages/inputs may be released after the worker returns.
            fake.project.request.pmmPath.clear();
            fake.capture.snapshot.reset();
            CHECK(transaction.GetSeal() == published.seal);
            CHECK(transaction.FinishWrite(published, live, true));
            CHECK(transaction.GetStage() == Stage::AwaitingAcknowledgement && transaction.DiskCommitted());
            unsigned acknowledgements = 0;
            const AcknowledgementCallbacks callbacks{
                [&](const PairSeal& seal) {
                    ++acknowledgements;
                    CHECK(seal == published.seal);
                    return published.seal;
                },
                [&] { return LiveSaveState{live, true}; },
            };
            CHECK(transaction.Acknowledge(kRequestId, {live, true}, callbacks));
            CHECK(transaction.GetStage() == Stage::Completed && transaction.DiskCommitted());
            CHECK(transaction.AllowsApplySceneCompletion(live, true) == (kind != SaveKind::Backup));
            CHECK(!transaction.AllowsClearCapturedDirty(live, true));
            CHECK(!transaction.AllowsCloseAfterSave(live, true));
            CHECK(transaction.AllowsClearCapturedDirty(frozen, true) == (kind != SaveKind::Backup));
            CHECK(transaction.AllowsCloseAfterSave(frozen, true) == (kind != SaveKind::Backup));
            CHECK(!transaction.Acknowledge(kRequestId, {live, true}, callbacks));
            CHECK(acknowledgements == 1);
        }
    }
    checker.ClearContext();
}

void RunPairWriteReceiptChecks(Checker& checker) {
    FakePair fake;
    if (!CHECK(bool(fake.capture.snapshot))) return;
    const auto& live = *fake.provider.live;
    const auto published = fake.Publish();
    {
        PairTransaction transaction(*fake.capture.snapshot, fake.project);
        CHECK(!transaction.FinishWrite(published, live, true));
        CHECK(!transaction.Acknowledge(kRequestId, {live, true}, {}));
        CHECK(!transaction.CancelBeforeWrite(kRequestId + 1));
        CHECK(transaction.CancelBeforeWrite(kRequestId));
        CHECK(transaction.GetStage() == Stage::CancelledBeforeWrite);
        CHECK(!transaction.StartWrite(kRequestId));
        CHECK(!transaction.FinishWrite(published, live, true));
    }
    {
        PairTransaction transaction(*fake.capture.snapshot, fake.project);
        CHECK(!transaction.StartWrite(kRequestId + 1));
        CHECK(transaction.StartWrite(kRequestId));
        auto wrong = published;
        ++wrong.seal.request.requestId;
        CHECK(!transaction.FinishWrite(wrong, live, true));
        CHECK(transaction.GetStage() == Stage::Writing);
        CHECK(transaction.FinishWrite(published, live, true));
        CHECK(!transaction.FinishWrite(published, live, true));
    }
    const auto refuse = [&](const char* name, Stage outcome, const auto& change) {
        checker.SetContext(name);
        PairTransaction transaction(*fake.capture.snapshot, fake.project);
        CHECK(transaction.StartWrite(kRequestId));
        auto receipt = published;
        change(receipt);
        CHECK(transaction.FinishWrite(receipt, live, true));
        CHECK(transaction.GetStage() == outcome && !transaction.DiskCommitted());
        CHECK(!transaction.FinishWrite(published, live, true));
        CHECK(!transaction.Acknowledge(kRequestId, {live, true}, {}));
        CHECK(!transaction.AllowsApplySceneCompletion(live, true));
        CHECK(!transaction.AllowsClearCapturedDirty(live, true));
        CHECK(!transaction.AllowsCloseAfterSave(live, true));
    };
    constexpr auto uncertain = Stage::PublicationUncertain;
    refuse("old pair confirmed intact", Stage::NotCommitted, [](auto& r) { r.status = PublicationStatus::NotCommitted; });
    refuse("interrupted publication", uncertain, [](auto& r) { r.status = PublicationStatus::NeedsRecovery; });
    refuse("unrecognized status", uncertain, [](auto& r) { r.status = static_cast<PublicationStatus>(99); });
    refuse("wrong PMM target", uncertain, [](auto& r) { r.seal.request.pmmPath += L".other"; });
    refuse("wrong EMM target", uncertain, [](auto& r) { r.seal.request.emmPath += L".other"; });
    refuse("wrong captured identity", uncertain, [](auto& r) { ++r.seal.request.identity.effectRevision; });
    refuse("wrong PMM digest", uncertain, [](auto& r) { ++r.seal.pmm.sha256[0]; });
    refuse("wrong EMM digest", uncertain, [](auto& r) { ++r.seal.emm.sha256[0]; });
    refuse("truncated EMM", uncertain, [](auto& r) { --r.seal.emm.size; });
    refuse("PMM only", uncertain, [](auto& r) { r.seal.emm = {}; });
    refuse("missing required sidecar", uncertain, [](auto& r) { r.seal.sidecar.reset(); });
    refuse("wrong sidecar", uncertain, [](auto& r) { ++r.seal.sidecar->file.sha256[0]; });
    refuse("incorrect abort receipt", uncertain, [](auto& r) { r.status = PublicationStatus::NotCommitted; ++r.seal.pmm.size; });
    checker.ClearContext();
}

void RunPairAcknowledgementChecks(Checker& checker) {
    FakePair fake;
    if (!CHECK(bool(fake.capture.snapshot))) return;
    const auto& captured = *fake.provider.live;
    const auto published = fake.Publish();
    const auto drive = [&](const char* name, Stage outcome, const auto& acknowledge, bool observeFails = false) {
        checker.SetContext(name);
        PairTransaction transaction(*fake.capture.snapshot, fake.project);
        LiveSaveState live{captured, true};
        unsigned calls = 0, observations = 0;
        const AcknowledgementCallbacks callbacks{
            [&](const PairSeal& seal) -> std::optional<PairSeal> {
                ++calls;
                CHECK(seal == published.seal);
                CHECK(transaction.GetStage() == Stage::Acknowledging);
                CHECK(!transaction.Acknowledge(kRequestId, live, {}));
                CHECK(!transaction.StartWrite(kRequestId));
                return acknowledge(live.identity, live.providerAvailable);
            },
            [&] {
                ++observations;
                CHECK(transaction.GetStage() == Stage::Acknowledging);
                CHECK(!transaction.Acknowledge(kRequestId, live, {}));
                if (observeFails) throw 1;
                return live;
            },
        };
        CHECK(transaction.StartWrite(kRequestId));
        CHECK(!transaction.Acknowledge(kRequestId, live, callbacks));
        CHECK(transaction.FinishWrite(published, live.identity, live.providerAvailable));
        CHECK(!transaction.Acknowledge(kRequestId + 1, live, callbacks));
        CHECK(transaction.Acknowledge(kRequestId, live, callbacks));
        CHECK(transaction.GetStage() == outcome && transaction.DiskCommitted());
        CHECK(!transaction.AllowsClearCapturedDirty(live.identity, live.providerAvailable));
        CHECK(!transaction.AllowsCloseAfterSave(live.identity, live.providerAvailable));
        CHECK(!transaction.Acknowledge(kRequestId, live, callbacks));
        CHECK(calls == 1 && observations == 1);
    };
    constexpr auto failed = Stage::CommittedAcknowledgementFailed;
    drive("no receipt", failed, [](auto&, auto&) -> std::optional<PairSeal> { return std::nullopt; });
    drive("throwing provider", failed, [](auto&, auto&) -> std::optional<PairSeal> { throw 1; });
    drive("old request receipt", failed, [&](auto&, auto&) { auto s = published.seal; --s.request.requestId; return s; });
    drive("other effect bytes", failed, [&](auto&, auto&) { auto s = published.seal; ++s.emm.sha256[0]; return s; });
    drive("other project bytes", failed, [&](auto&, auto&) { auto s = published.seal; ++s.pmm.sha256[0]; return s; });
    drive("unavailable live observation", failed, [&](auto&, auto&) { return published.seal; }, true);
    drive("provider retired in callback", Stage::CommittedProviderRetired,
        [&](auto&, auto& available) { available = false; return published.seal; });
    drive("scene replaced in callback", Stage::CommittedStaleScene,
        [&](auto& live, auto&) { ++live.sceneGeneration; return published.seal; });
    drive("effect edited in callback", Stage::Completed,
        [&](auto& live, auto&) { ++live.effectRevision; return published.seal; });

    checker.SetContext("missing observer prevents calling provider");
    {
        PairTransaction transaction(*fake.capture.snapshot, fake.project);
        unsigned calls = 0;
        CHECK(transaction.StartWrite(kRequestId));
        CHECK(transaction.FinishWrite(published, captured, true));
        CHECK(transaction.Acknowledge(kRequestId, {captured, true}, {
            [&](const PairSeal&) { ++calls; return published.seal; }, {},
        }));
        CHECK(transaction.GetStage() == failed && transaction.DiskCommitted());
        CHECK(calls == 0);
    }
    checker.SetContext("provider unavailable before callback");
    {
        PairTransaction transaction(*fake.capture.snapshot, fake.project);
        unsigned calls = 0;
        CHECK(transaction.StartWrite(kRequestId));
        CHECK(transaction.FinishWrite(published, captured, true));
        CHECK(!transaction.Acknowledge(kRequestId, {captured, false}, {
            [&](const PairSeal&) { ++calls; return published.seal; },
            [&] { return LiveSaveState{captured, false}; },
        }));
        CHECK(transaction.GetStage() == Stage::CommittedProviderRetired && transaction.DiskCommitted());
        CHECK(calls == 0);
    }
    checker.ClearContext();
}
