// Included by effect_save_state_tests.cpp; all providers, EMM syntax checks,
// and digest operations below are test doubles, not production EMM evidence.
using mikumikustudio::effect_save::CaptureAdmission;
using mikumikustudio::effect_save::CaptureAttempt;
using mikumikustudio::effect_save::CaptureCallbacks;
using mikumikustudio::effect_save::CaptureSink;
using mikumikustudio::effect_save::CaptureStatus;
using mikumikustudio::effect_save::CapturedEffects;
using mikumikustudio::effect_save::EffectBytes;
using mikumikustudio::effect_save::EffectDigest;
using mikumikustudio::effect_save::FreezeResult;
using mikumikustudio::effect_save::FreezeStatus;
using mikumikustudio::effect_save::MaximumEffectBytes;

static_assert(!std::is_copy_constructible_v<CaptureSink> && !std::is_move_constructible_v<CaptureSink>);
static_assert(!std::is_copy_constructible_v<CapturedEffects> && !std::is_move_constructible_v<CapturedEffects>);
static_assert(std::is_same_v<decltype(std::declval<CaptureAttempt>().snapshot),
    std::unique_ptr<const CapturedEffects>>);
static_assert(std::is_same_v<decltype(std::declval<CapturedEffects>().Bytes().data), const std::uint8_t*>);

struct FakeCapture {
    Request request = MakeRequest();
    std::optional<CaptureIdentity> live = request.identity;
    std::string providerBytes = "[Info]\nPmd1=one.fx\n";
    unsigned observed = 0, frozen = 0, validated = 0, hashed = 0;
    bool cancelled = false;

    FreezeResult Freeze(const Request&, CaptureSink& sink) {
        ++frozen;
        if (!sink.Begin(providerBytes.size()) || !sink.Append(providerBytes.data(), 1) ||
            !sink.Append(providerBytes.data() + 1, providerBytes.size() - 1))
            return {FreezeStatus::Failed, {}};
        return {FreezeStatus::Frozen, *live};
    }

    static EffectDigest FakeDigest(EffectBytes bytes) noexcept {
        EffectDigest digest{};
        for (std::size_t i = 0; i != bytes.size; ++i) {
            auto& entry = digest[i % digest.size()];
            entry = static_cast<std::uint8_t>(entry + bytes.data[i]);
        }
        return digest;
    }

    CaptureCallbacks Callbacks() {
        return {
            [this] { ++observed; return live; },
            [this] { return cancelled; },
            [this](const Request& r, CaptureSink& sink) { return Freeze(r, sink); },
            [this](const Request&, EffectBytes bytes) {
                ++validated;
                return std::string_view(reinterpret_cast<const char*>(bytes.data), bytes.size).substr(0, 7) == "[Info]\n";
            },
            [this](EffectBytes bytes, EffectDigest& digest) { ++hashed; digest = FakeDigest(bytes); return true; },
        };
    }
};

void RunImmutableCaptureChecks(Checker& checker) {
    for (auto kind : {SaveKind::Manual, SaveKind::SaveAs, SaveKind::Incremental, SaveKind::Backup}) {
        checker.SetContext(SaveKindName(kind));
        CaptureAdmission admission;
        const std::string expected = "[Info]\nPmd1=one.fx\n";
        const auto digest = FakeCapture::FakeDigest({reinterpret_cast<const std::uint8_t*>(expected.data()), expected.size()});
        auto result = [&] {
            FakeCapture fake;
            fake.request.kind = kind;
            fake.request.pmmPath = L"D:\\projects\\scene_002.pmm";
            fake.request.emmPath = L"D:\\projects\\scene_002.emm";
            auto callbacks = fake.Callbacks();
            const auto freeze = callbacks.freeze;
            callbacks.freeze = [&](const Request& request, CaptureSink& sink) {
                CHECK(request.kind == kind && request.pmmPath == L"D:\\projects\\scene_002.pmm" &&
                    request.emmPath == L"D:\\projects\\scene_002.emm");
                const auto nested = admission.Capture(request, expected.size(), callbacks);
                CHECK(nested.status == CaptureStatus::Busy && !nested.snapshot);
                auto frozen = freeze(request, sink);
                CHECK(frozen.status == FreezeStatus::Frozen);
                // The provider's source memory and caller's Request can be
                // changed immediately, even before host validation or hashing.
                fake.providerBytes.assign(fake.providerBytes.size(), 'x');
                fake.request.pmmPath = L"modified.pmm";
                fake.request.identity.objectIds.clear();
                return frozen;
            };
            auto captured = admission.Capture(fake.request, expected.size(), callbacks);
            CHECK(fake.frozen == 1 && fake.observed == 3 && fake.validated == 1 && fake.hashed == 1);
            fake.live.reset();
            return captured;
        }(); // The provider and callback objects are gone before the worker handoff.

        CHECK(result.status == CaptureStatus::Captured);
        if (CHECK(bool(result.snapshot))) {
            const auto& captured = *result.snapshot;
            const auto bytes = captured.Bytes();
            CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data), bytes.size) == expected);
            CHECK(captured.Digest() == digest);
            CHECK(captured.GetRequest().identity == MakeIdentity());
            CHECK(captured.GetRequest().pmmPath == L"D:\\projects\\scene_002.pmm");
            CHECK(captured.GetRequest().emmPath == L"D:\\projects\\scene_002.emm");
            CompletionTracker tracker(captured.GetRequest());
            auto edited = MakeIdentity();
            AddObjectId(edited);
            CHECK(tracker.StartWrite(kRequestId));
            CHECK(tracker.FinishWrite(kRequestId, true, edited, true));
            CHECK(tracker.BeginAcknowledgement(kRequestId, edited, true));
            CHECK(tracker.FinishAcknowledgement(kRequestId, true, edited, true));
            CHECK(tracker.GetStage() == Stage::Completed && tracker.DiskCommitted());
            CHECK(!tracker.AllowsClearCapturedDirty(edited, true) && !tracker.AllowsCloseAfterSave(edited, true));
        }
    }
    checker.ClearContext();
}

void RunCaptureRejections(Checker& checker) {
    using Status = CaptureStatus;
    CaptureAdmission admission;
    const auto expect = [&](const char* name, Status status, const auto& configure,
                            unsigned freezes, unsigned validations, unsigned hashes) {
        checker.SetContext(name);
        FakeCapture fake;
        auto callbacks = fake.Callbacks();
        std::size_t budget = 256;
        configure(fake, callbacks, budget);
        const auto result = admission.Capture(fake.request, budget, callbacks);
        CHECK(result.status == status);
        CHECK(!result.snapshot);
        CHECK(fake.frozen == freezes && fake.validated == validations && fake.hashed == hashes);
    };
    expect("effect saving disabled", Status::InvalidRequest,
        [](auto& f, auto&, auto&) { f.request.saveEffects = false; }, 0, 0, 0);
    expect("unknown save kind", Status::InvalidRequest,
        [](auto& f, auto&, auto&) { f.request.kind = static_cast<SaveKind>(99); }, 0, 0, 0);
    expect("unresolved companion", Status::InvalidRequest,
        [](auto& f, auto&, auto&) { f.request.emmPath.clear(); }, 0, 0, 0);
    expect("identical destinations", Status::InvalidRequest,
        [](auto& f, auto&, auto&) { f.request.emmPath = f.request.pmmPath; }, 0, 0, 0);
    expect("invalid stable object identity", Status::InvalidRequest,
        [](auto& f, auto&, auto&) { f.request.identity.objectIds = {31, 0}; }, 0, 0, 0);
    expect("duplicate stable object identity", Status::InvalidRequest,
        [](auto& f, auto&, auto&) { f.request.identity.objectIds = {31, 32, 31}; }, 0, 0, 0);
    expect("no freeze contract", Status::Unsupported,
        [](auto&, auto& cb, auto&) { cb.freeze = {}; }, 0, 0, 0);
    expect("provider unavailable", Status::Unsupported,
        [](auto& f, auto&, auto&) { f.live.reset(); }, 0, 0, 0);
    expect("cancelled before freeze", Status::Cancelled,
        [](auto& f, auto&, auto&) { f.cancelled = true; }, 0, 0, 0);
    expect("edit between PMM and effect capture", Status::Stale,
        [](auto& f, auto&, auto&) { BumpEffectRevision(*f.live); }, 0, 0, 0);
    expect("no available budget", Status::PayloadRejected,
        [](auto&, auto&, auto& budget) { budget = 0; }, 0, 0, 0);
    expect("no host validator", Status::HostFailed,
        [](auto&, auto& cb, auto&) { cb.validate = {}; }, 0, 0, 0);
    expect("no host digest", Status::HostFailed,
        [](auto&, auto& cb, auto&) { cb.sha256 = {}; }, 0, 0, 0);

    expect("cancelled while freezing", Status::Cancelled,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto& r, auto& s) { auto x = f.Freeze(r, s); f.cancelled = true; return x; }; }, 1, 0, 0);
    expect("freeze declined by provider", Status::Unsupported,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto&, auto&) { ++f.frozen; return FreezeResult{FreezeStatus::Unsupported, {}}; }; }, 1, 0, 0);
    expect("freeze cancelled by provider", Status::Cancelled,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto&, auto&) { ++f.frozen; return FreezeResult{FreezeStatus::Cancelled, {}}; }; }, 1, 0, 0);
    expect("provider exception", Status::ProviderFailed,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto& r, auto& s) -> FreezeResult { f.Freeze(r, s); throw 1; }; }, 1, 0, 0);
    expect("provider froze a different object order", Status::Stale,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto& r, auto& s) { auto x = f.Freeze(r, s); ReorderObjectIds(x.identity); return x; }; }, 1, 0, 0);
    expect("effect-only edit during freeze despite matching receipt", Status::Stale,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto& r, auto& s) { auto x = f.Freeze(r, s); BumpEffectRevision(*f.live); return x; }; }, 1, 0, 0);
    expect("provider retired during freeze", Status::Stale,
        [](auto& f, auto& cb, auto&) { cb.freeze = [&f](const auto& r, auto& s) { auto x = f.Freeze(r, s); f.live.reset(); return x; }; }, 1, 0, 0);
    expect("scene changed during host verification", Status::Stale,
        [](auto& f, auto& cb, auto&) { cb.sha256 = [&f](auto, auto&) { ++f.hashed; ++f.live->sceneGeneration; return true; }; }, 1, 1, 1);
    expect("cancelled during host verification", Status::Cancelled,
        [](auto& f, auto& cb, auto&) { cb.sha256 = [&f](auto, auto&) { ++f.hashed; f.cancelled = true; return true; }; }, 1, 1, 1);
    expect("host rejects format", Status::ValidationFailed,
        [](auto& f, auto&, auto&) { f.providerBytes = "invalid format"; }, 1, 1, 0);
    expect("host digest failure", Status::DigestFailed,
        [](auto& f, auto& cb, auto&) { cb.sha256 = [&f](auto, auto&) { ++f.hashed; return false; }; }, 1, 1, 1);
    expect("host validation exception", Status::HostFailed,
        [](auto& f, auto& cb, auto&) { cb.validate = [&f](const auto&, auto) -> bool { ++f.validated; throw 1; }; }, 1, 1, 0);

    // A failed sink operation remains fatal even when the provider says it
    // succeeded, and the provider cannot recover it by supplying valid bytes.
    const auto expectSinkRejection = [&](const char* name, std::size_t budget, const auto& send) {
        expect(name, Status::PayloadRejected, [&](auto& f, auto& cb, auto& limit) {
            limit = budget;
            cb.freeze = [&](const auto& r, auto& s) {
                ++f.frozen;
                send(s);
                return FreezeResult{FreezeStatus::Frozen, r.identity};
            };
        }, 1, 0, 0);
    };
    expectSinkRejection("empty payload", 256, [&](auto& sink) { CHECK(!sink.Begin(0)); });
    expectSinkRejection("missing declaration", 256, [&](auto& sink) { CHECK(!sink.Append("a", 1)); });
    expectSinkRejection("truncated payload", 256, [&](auto& sink) { CHECK(sink.Begin(2) && sink.Append("a", 1)); });
    expectSinkRejection("declared size exceeds per-save budget", 1, [&](auto& sink) { CHECK(!sink.Begin(2)); });
    expectSinkRejection("hard upper limit", MaximumEffectBytes + 1, [&](auto& sink) { CHECK(!sink.Begin(MaximumEffectBytes + 1)); });
    expectSinkRejection("oversized append and sticky failure", 256, [&](auto& sink) { CHECK(sink.Begin(1) && !sink.Append("ab", 2) && !sink.Append("a", 1)); });
    expectSinkRejection("non-empty null append", 256, [&](auto& sink) { CHECK(sink.Begin(1) && !sink.Append(nullptr, 1)); });
    expectSinkRejection("duplicate declaration", 256, [&](auto& sink) { CHECK(sink.Begin(1) && !sink.Begin(1)); });

    checker.ClearContext();
    FakeCapture retry;
    const auto result = admission.Capture(retry.request, retry.providerBytes.size(), retry.Callbacks());
    CHECK(result.status == Status::Captured && result.snapshot && retry.frozen == 1);
}

void RunCaptureBudgetBoundary(Checker& checker) {
    FakeCapture fake;
    // A valid exact-limit capture is accepted; no dependence on a terminator.
    fake.providerBytes.resize(MaximumEffectBytes, 'x');
    fake.providerBytes.back() = 'z';
    CaptureAdmission admission;
    const auto result = admission.Capture(fake.request, MaximumEffectBytes + 1, fake.Callbacks());
    CHECK(result.status == CaptureStatus::Captured);
    if (CHECK(bool(result.snapshot))) {
        const auto bytes = result.snapshot->Bytes();
        CHECK(bytes.size == MaximumEffectBytes);
        CHECK(bytes.data[MaximumEffectBytes - 1] == 'z');
    }
}
