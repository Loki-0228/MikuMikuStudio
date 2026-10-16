// Include after native_global_registration_checks.inc. The byte oracle calls
// the unbound production 431/432 commands with hidden controls, without D3D.
#include "../src/plugins/native_camera_interpolation.hpp"
#include "../src/plugins/native_key_commands.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include "mikudancestudio/panel_controls.hpp"
namespace camera_interpolation_checks {
struct Fixture : GlobalRegistrationFixture {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    HWND combo = nullptr;
    Fixture() {
        app->MainWindow() = CreateWindowExW(0, L"STATIC", L"camera interpolation test", WS_OVERLAPPED,
            0, 0, 400, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(app->MainWindow() != nullptr, "camera interpolation hidden parent created");
        combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
            0, 0, 80, 120, app->MainWindow(), reinterpret_cast<HMENU>(INT_PTR(panel::kInterpCurveCombo)),
            GetModuleHandleW(nullptr), nullptr);
        Check(combo != nullptr, "camera interpolation seven-channel combo created");
        for (int channel = 0; channel < 7; ++channel)
            Check(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"channel")) == channel,
                "camera interpolation combo contains six individual channels and all");
        dc = CreateCompatibleDC(nullptr); bitmap = CreateBitmap(128, 128, 1, 32, nullptr);
        Check(dc && bitmap, "camera interpolation drawing cache created without a renderer");
        oldBitmap = SelectObject(dc, bitmap); app->CurveDC() = dc;
        app->CurrentFrame() = 17; app->LastRegisteredFrame() = 91;
        app->SceneModified() = 1; app->RecordedFrameCountByte1() = 0; app->PhysicsResetPending() = 0;
        for (unsigned slot = 0; slot < 3; ++slot) {
            auto& key = app->CameraKeys()[slot]; key.selected = slot == 0 ? 2 : slot == 2 ? 5 : 0;
            for (unsigned channel = 0; channel < 6; ++channel) {
                key.interpolation[0][channel] = std::uint8_t(3 + slot + channel);
                key.interpolation[1][channel] = std::uint8_t(30 + slot + channel);
                key.interpolation[2][channel] = std::uint8_t(89 + slot + channel);
                key.interpolation[3][channel] = std::uint8_t(115 + slot + channel);
            }
        }
        for (unsigned channel = 0; channel < 6; ++channel) {
            app->state.lightA[channel] = std::uint8_t(17 + channel);
            app->state.lightB[channel] = std::uint8_t(43 + channel);
            app->state.lightC[channel] = std::uint8_t(81 + channel);
            app->state.lightD[channel] = std::uint8_t(108 + channel);
        }
    }
    ~Fixture() {
        app->CurveDC() = nullptr;
        SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
        DestroyWindow(app->MainWindow()); app->MainWindow() = nullptr;
    }
    plugins::CameraInterpolationOptions Options(unsigned channel, bool reset = false) const {
        plugins::CameraInterpolationOptions options;
        options.channels = channel == 6 ? 63 : std::uint8_t(1u << channel);
        for (unsigned lane = 0; lane < 6; ++lane)
            options.values[lane] = reset ? std::array<std::uint8_t, 4>{20, 20, 107, 107} :
                std::array<std::uint8_t, 4>{app->state.lightA[lane], app->state.lightB[lane],
                    app->state.lightC[lane], app->state.lightD[lane]};
        return options;
    }
    void Oracle(unsigned channel, bool reset) {
        SendMessageW(combo, CB_SETCURSEL, channel, 0);
        auto* previous = g_Block; g_Block = app.get();
        CmdControl400(app.get(), app->MainWindow(), reset ? 432 : 431, 0);
        g_Block = previous;
    }
    // Only 24 camera interpolation bytes per slot are excluded. This guards
    // all selections, poses, playhead/cursors, light mirror, and other pools.
    std::uint64_t Protected() const {
        std::uint64_t value = 14695981039346656037ull;
        const auto add = [&](const void* p, std::size_t bytes) {
            auto* data = static_cast<const unsigned char*>(p);
            while (bytes--) { value ^= *data++; value *= 1099511628211ull; }
        };
        add(&app->state, sizeof(app->state)); add(&app->SceneLight(), sizeof(D3DLIGHT9));
        for (std::size_t slot = 0; slot < mdl::KeyPoolCapacity(app->CameraKeys()); ++slot) {
            auto key = app->CameraKeys()[slot]; std::memset(key.interpolation, 0, sizeof(key.interpolation)); add(&key, sizeof(key));
        }
        const auto pool = [&](const auto* keys) { add(keys, mdl::KeyPoolCapacity(keys) * sizeof(*keys)); };
        pool(app->LightKeys()); pool(app->ShadowKeys()); pool(app->GravityKeys());
        for (unsigned slot = 0; slot < 255; ++slot) pool(app->AccessoryKeys(slot));
        return value;
    }
};
std::shared_ptr<plugins::HistoryChange> Plan(Fixture& f, const plugins::CameraInterpolationOptions& options) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanCameraInterpolationEdit(*f.app, options, change) == MMS_OK,
        "camera interpolation private plan succeeds");
    return change;
}
void Commit(Fixture& f, const std::shared_ptr<plugins::HistoryChange>& change) {
    Check(bool(change), "camera interpolation plan owns an actual change");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("camera interpolation", change, transaction) == MMS_OK,
        "camera interpolation prepares one common history node");
    failAfter = 0; const auto result = f.history.CommitNative(std::move(transaction)); failAfter = -1;
    Check(result == MMS_OK, "camera interpolation commit allocates nothing");
}
std::vector<std::uint8_t> Curves(MMDApp& app, curve_host::Kind kind = curve_host::Kind::Camera) {
    std::vector<curve_editor::Channel> channels; std::wstring error;
    Check(curve_host::Read(app, kind, 0, channels, error), "camera interpolation authoring can be read");
    std::vector<animation_curve::NamedTrack> tracks;
    for (const auto& channel : channels) tracks.push_back({channel.id, channel.track});
    std::vector<std::uint8_t> encoded; std::string detail;
    Check(animation_curve::Encode(tracks, encoded, detail), "camera interpolation authoring has an exact codec snapshot");
    return encoded;
}
std::vector<std::uint8_t> SavedCurves(MMDApp& app) {
    // This is the production PMM-save capture path. Do not call Read first:
    // Capture itself must recognize a normally grown authoring pool.
    pmm_extension::Document document; curve_host::Capture(app, document);
    const auto* record = pmm_extension::Find(document, "org.mikudancestudio.animation_curves.v1");
    Check(record != nullptr, "PMM capture retains its authored-curves extension");
    std::vector<animation_curve::NamedTrack> tracks; std::string error;
    Check(animation_curve::Decode(record->value.data(), record->value.size(), tracks, error) &&
        std::any_of(tracks.begin(), tracks.end(), [](const auto& track) { return track.id.find("0/0/") == 0; }),
        "PMM capture includes real camera authoring rather than an empty extension");
    return record->value;
}
void Author(Fixture& f) {
    std::vector<curve_editor::Channel> channels; std::wstring error;
    Check(curve_host::Read(*f.app, curve_host::Kind::Camera, 0, channels, error), "read camera authoring fixture");
    channels[0].track.keys[0].outWeight = .19; channels[0].track.keys[0].outSlope = .173;
    channels[0].track.keys[0].tangent = animation_curve::Tangent::Free;
    channels[0].track.keys[1].inWeight = .31; channels[0].track.keys[1].inSlope = -.113;
    channels[0].track.keys[1].tangent = animation_curve::Tangent::Free;
    channels[2].track.keys[2].interpolation = animation_curve::Interpolation::Step;
    Check(curve_host::Write(*f.app, curve_host::Kind::Camera, 0, channels, error), "author free camera handles and a Step interval");
    Check(curve_host::Read(*f.app, curve_host::Kind::Light, 0, channels, error), "read unrelated light authoring fixture");
    channels[0].track.keys[0].outWeight = .23; channels[0].track.keys[1].inWeight = .37;
    Check(curve_host::Write(*f.app, curve_host::Kind::Light, 0, channels, error), "author an unrelated light track");
    for (std::size_t slot = 0; slot < mdl::KeyPoolCapacity(f.app->CameraKeys()); ++slot) f.app->CameraKeys()[slot].selected = 0;
    f.app->CameraKeys()[0].selected = 2; f.app->CameraKeys()[2].selected = 5;
}
void LegacyParity() {
    for (bool reset : {false, true}) for (unsigned channel = 0; channel < 7; ++channel) {
        Fixture f, oracle;
        const auto original = GlobalRegistrationKeys(f.app->CameraKeys());
        const auto protectedBefore = f.Protected(), before = f.Digest();
        const auto options = f.Options(channel, reset); auto change = Plan(f, options);
        Check(change && f.Digest() == before, "camera interpolation planning preserves the complete live state");
        oracle.Oracle(channel, reset); Commit(f, change);
        Check(GlobalRegistrationSame(f.app->CameraKeys(), GlobalRegistrationKeys(oracle.app->CameraKeys())),
            "camera interpolation native bytes match actual legacy 431/432 for all six channels and all");
        Check(f.Protected() == protectedBefore && !std::memcmp(&f.app->CameraKeys()[1], &original[1], sizeof(mdl::CameraKey)),
            "camera interpolation edits selected root and tail only and preserves every unrelated byte");
        auto beforeKeys = original, afterKeys = GlobalRegistrationKeys(f.app->CameraKeys());
        for (unsigned slot = 0; slot < 3; ++slot)
            f.app->CameraKeys()[slot].selected = beforeKeys[slot].selected = afterKeys[slot].selected = std::uint8_t(slot + 8);
        f.app->CameraPositionX() = 843.125f; f.app->CameraRotation()[1] = -.713f; f.app->CameraFov() = 81.5f;
        f.app->CameraDistance() = -811; f.app->CurrentFrame() = 28; f.app->LastRegisteredFrame() = 904;
        f.app->CameraTrackCursor() = 1; f.app->AccessoryKeys(254)[1].selected = 31;
        const auto currentProtected = f.Protected();
        Check(f.history.History(false) == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), beforeKeys) && f.Protected() == currentProtected,
            "camera interpolation undo restores only authoring and preserves later selection pose and playback head");
        Check(f.history.History(true) == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), afterKeys) && f.Protected() == currentProtected,
            "camera interpolation redo neither seeks nor changes other tracks and native metadata");
    }
}
void AuthoringAndGrowth() {
    for (unsigned channel : {0u, 2u, 3u, 6u}) {
        Fixture f, oracle; Author(f); Author(oracle);
        const auto original = GlobalRegistrationKeys(f.app->CameraKeys());
        const auto pristine = Curves(*f.app), light = Curves(*f.app, curve_host::Kind::Light);
        const auto protectedBefore = f.Protected();
        auto change = Plan(f, f.Options(channel));
        Check(Curves(*f.app) == pristine, "planning preserves free handles and Step authoring");
        oracle.Oracle(channel, false); Commit(f, change);
        const auto after = GlobalRegistrationKeys(f.app->CameraKeys()); const auto authoredAfter = Curves(*f.app);
        const auto savedAfter = SavedCurves(*f.app);
        curve_host::AuthoringTable captured; std::wstring captureError;
        Check(curve_host::CaptureAuthoring(*f.app, captured, captureError), "capture camera authoring table before growth");
        Check(authoredAfter == Curves(*oracle.app) && Curves(*f.app, curve_host::Kind::Light) == light && f.Protected() == protectedBefore,
            "target authoring matches ordinary native reconciliation and preserves unrelated authored tracks");
        const auto identity = mdl::KeyPoolIdentity(f.app->CameraKeys());
        Check(mdl::KeyPoolEnsure(f.app->CameraKeys(), 65) && mdl::KeyPoolIdentity(f.app->CameraKeys()) == identity,
            "camera interpolation pool growth preserves strong identity");
        Check(curve_host::AuthoringUnchanged(*f.app, captured), "authoring snapshot remains current across allocation-only growth");
        curve_host::AuthoringTable grownCapture; std::vector<curve_host::AuthoringEntry> grownEntries;
        Check(curve_host::CaptureAuthoring(*f.app, grownCapture, captureError) &&
            curve_host::AuthoringEntries(grownCapture, grownEntries) &&
            std::any_of(grownEntries.begin(), grownEntries.end(), [](const auto& entry) { return entry.kind == curve_host::Kind::Camera; }),
            "capture authoring table directly after growth retains the camera entry");
        Check(Curves(*f.app) == authoredAfter && SavedCurves(*f.app) == savedAfter,
            "ordinary Read immediately after growth preserves authored handles Step and later PMM capture");
        const auto nextCapacity = mdl::KeyPoolCapacity(f.app->CameraKeys()) + 1;
        Check(mdl::KeyPoolEnsure(f.app->CameraKeys(), nextCapacity) && mdl::KeyPoolIdentity(f.app->CameraKeys()) == identity,
            "grow again to exercise save capture before any ordinary authoring read");
        Check(SavedCurves(*f.app) == savedAfter && Curves(*f.app) == authoredAfter,
            "PMM capture immediately after growth preserves original camera authoring without a prior Read");
        f.app->CameraKeys()[64].selected = 19; f.app->CameraKeys()[64].reserved2[1] = 81;
        const auto tail = f.app->CameraKeys()[64]; auto* pool = f.app->CameraKeys();
        f.app->CurrentFrame() = 94; f.app->CameraPositionY() = -123.25f; const auto preserved = f.Protected();
        Check(f.history.History(false) == MMS_OK && f.app->CameraKeys() == pool && GlobalRegistrationSame(pool, original) &&
            !std::memcmp(&pool[64], &tail, sizeof(tail)) && Curves(*f.app) == pristine && f.Protected() == preserved,
            "camera interpolation undo after owned growth restores exact free handles and Step without shrinking or seeking");
        Check(f.history.History(true) == MMS_OK && GlobalRegistrationSame(pool, after) && Curves(*f.app) == authoredAfter &&
            Curves(*f.app, curve_host::Kind::Light) == light && f.Protected() == preserved,
            "camera interpolation redo survives authoring reads and growth while preserving unrelated tail slots");
    }
    {
        Fixture f; Author(f); const auto pristine = Curves(*f.app), saved = SavedCurves(*f.app);
        Check(mdl::KeyPoolEnsure(f.app->CameraKeys(), 65), "grow authored camera before capturing a new interpolation record");
        auto change = Plan(f, f.Options(0));
        Check(change && SavedCurves(*f.app) == saved && Curves(*f.app) == pristine,
            "a new plan on a just-grown pool captures the existing authoring without clearing it");
        Commit(f, change);
        Check(f.history.History(false) == MMS_OK && Curves(*f.app) == pristine && SavedCurves(*f.app) == saved,
            "new interpolation history after growth restores original free handles Step and saved authoring");
    }
    {
        Fixture f; Author(f); f.app->CameraKeys()[2].selected = 0;
        auto options = f.Options(2);
        for (unsigned point = 0; point < 4; ++point) options.values[2][point] = f.app->CameraKeys()[0].interpolation[point][2];
        const auto pristine = Curves(*f.app); const auto before = f.Digest();
        Check(!Plan(f, options) && f.Digest() == before && Curves(*f.app) == pristine,
            "equal camera interpolation bytes preserve the existing free and Step authoring layer");
    }
}
void NoopAndInvalid() {
    Fixture f; auto options = f.Options(6); Commit(f, Plan(f, options));
    Check(f.history.History(false) == MMS_OK, "camera interpolation creates redo for no-op rejection checks");
    for (std::size_t slot = 0; slot < mdl::KeyPoolCapacity(f.app->CameraKeys()); ++slot) f.app->CameraKeys()[slot].selected = 0;
    auto before = f.Digest();
    Check(!Plan(f, options) && f.Digest() == before && f.history.RedoCount() == 1,
        "zero-selection camera interpolation preserves redo and complete scene state");
    f.app->CameraKeys()[0].selected = 7; options.channels = 1;
    for (unsigned point = 0; point < 4; ++point) options.values[0][point] = f.app->CameraKeys()[0].interpolation[point][0];
    before = f.Digest();
    Check(!Plan(f, options) && f.Digest() == before && f.history.RedoCount() == 1,
        "equal camera interpolation handles are a true no-op with retained redo");
    auto zero = options; zero.channels = 0; zero.values[0][0] = 255;
    Check(!Plan(f, zero) && f.Digest() == before && f.history.RedoCount() == 1,
        "zero channel mask ignores disabled values and preserves redo");
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        auto bad = f.Options(0); if (invalid == 0) bad.channels = 64;
        else bad.values[0][invalid - 1] = 128;
        std::shared_ptr<plugins::HistoryChange> out;
        Check(plugins::PlanCameraInterpolationEdit(*f.app, bad, out) == MMS_E_ARGUMENT && !out && f.Digest() == before,
            "invalid camera interpolation mask or enabled handle rejects without writes");
    }
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        Fixture bad; auto* keys = bad.app->CameraKeys();
        if (invalid == 0) keys[1].next = 1;
        if (invalid == 1) keys[2].previous = 0;
        if (invalid == 2) keys[2].frame = keys[1].frame;
        if (invalid == 3) keys[2].next = 99;
        if (invalid == 4) keys[3].selected = 1;
        if (invalid == 5) keys[0].frame = 4;
        const auto unchanged = bad.Digest(); std::shared_ptr<plugins::HistoryChange> out;
        Check(plugins::PlanCameraInterpolationEdit(*bad.app, bad.Options(6), out) == MMS_E_ARGUMENT && !out && bad.Digest() == unchanged,
            "camera interpolation rejects malformed links frames and selected orphan slots atomically");
    }
    {
        Fixture borrowed; auto* owned = borrowed.app->CameraKeys(); std::vector<mdl::CameraKey> raw(10000);
        std::memcpy(raw.data(), owned, 4 * sizeof(*owned)); borrowed.app->CameraKeys() = raw.data();
        std::shared_ptr<plugins::HistoryChange> out;
        const auto result = plugins::PlanCameraInterpolationEdit(*borrowed.app, borrowed.Options(0), out);
        borrowed.app->CameraKeys() = owned;
        Check(result == MMS_E_NOT_READY && !out, "camera interpolation never assigns ownership to a borrowed key pool");
    }
}
void RegistrationGrowthComposition() {
    Fixture f; GlobalRegistrationFillTail(f.app->CameraKeys()); Author(f);
    const auto original = GlobalRegistrationKeys(f.app->CameraKeys());
    const auto pristine = Curves(*f.app), originalSaved = SavedCurves(*f.app);
    auto interpolation = Plan(f, f.Options(0)); Commit(f, interpolation);
    const auto afterInterpolation = GlobalRegistrationKeys(f.app->CameraKeys());
    const auto interpolationCurves = Curves(*f.app), interpolationSaved = SavedCurves(*f.app);
    const auto oldCapacity = mdl::KeyPoolCapacity(f.app->CameraKeys());
    const auto identity = mdl::KeyPoolIdentity(f.app->CameraKeys());
    auto registration = GlobalRegistrationPlan(f, 0, 80); GlobalRegistrationCommit(f, registration);
    Check(mdl::KeyPoolCapacity(f.app->CameraKeys()) > oldCapacity && mdl::KeyPoolIdentity(f.app->CameraKeys()) == identity,
        "camera registration following interpolation grows the full owned pool within its own transaction");
    const auto afterRegistration = GlobalRegistrationKeys(f.app->CameraKeys());
    const auto registrationCurves = Curves(*f.app), registrationSaved = SavedCurves(*f.app);
    Check(f.history.UndoCount() == 2 && f.history.History(false) == MMS_OK &&
        GlobalRegistrationSame(f.app->CameraKeys(), afterInterpolation) && Curves(*f.app) == interpolationCurves &&
        SavedCurves(*f.app) == interpolationSaved,
        "undo grown registration restores the preceding interpolation authoring after ordinary Read and PMM capture");
    Check(f.history.History(false) == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), original) &&
        Curves(*f.app) == pristine && SavedCurves(*f.app) == originalSaved,
        "undo preceding interpolation restores original free and Step curves without losing the grown allocation");
    Check(f.history.History(true) == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), afterInterpolation) &&
        Curves(*f.app) == interpolationCurves && SavedCurves(*f.app) == interpolationSaved,
        "redo interpolation restores its own authoring before the later registration");
    Check(f.history.History(true) == MMS_OK && GlobalRegistrationSame(f.app->CameraKeys(), afterRegistration) &&
        Curves(*f.app) == registrationCurves && SavedCurves(*f.app) == registrationSaved,
        "redo registration preserves its native keys and authored curves across the two-family history sequence");
}
void FailuresAndStale() {
    {
        Fixture f; Author(f); const auto before = f.Digest(), protectedBefore = f.Protected();
        const auto authored = Curves(*f.app); const auto options = f.Options(6);
        std::shared_ptr<plugins::HistoryChange> change; bool success = false; unsigned failures = 0;
        for (long limit = 0; limit < 2048 && !success; ++limit) {
            failAfter = limit; const auto result = plugins::PlanCameraInterpolationEdit(*f.app, options, change); failAfter = -1;
            Check(f.Digest() == before && Curves(*f.app) == authored, "camera interpolation planning OOM leaves native and free authoring untouched");
            if (result == MMS_OK) { success = bool(change); Check(success, "camera interpolation fault sweep yields a complete record"); }
            else { ++failures; Check(result == MMS_E_MEMORY && !change, "camera interpolation planning OOM leaves no partial output"); }
        }
        Check(success && failures > 0, "camera interpolation planning allocation sweep reaches success");
        plugins::SharedHistory tiny(f.storage, {}, 16, 1); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(tiny.PrepareNative("camera interpolation budget", change, transaction) == MMS_E_MEMORY && !transaction && f.Digest() == before,
            "camera interpolation retained curve payload respects the common budget");
        success = false; failures = 0;
        for (long limit = 0; limit < 2048 && !success; ++limit) {
            std::string label = "camera interpolation preparation";
            failAfter = limit; const auto result = f.history.PrepareNative(std::move(label), change, transaction); failAfter = -1;
            Check(f.Digest() == before && Curves(*f.app) == authored, "camera interpolation preparation OOM cannot publish a partial curve");
            if (result == MMS_OK) success = true;
            else { ++failures; Check(result == MMS_E_MEMORY && !transaction && !f.history.UndoCount(), "camera interpolation preparation failure preserves the journal"); }
        }
        Check(success && failures > 0, "camera interpolation candidate allocation sweep reaches success");
        failAfter = 0; transaction.reset(); failAfter = -1;
        Check(f.Digest() == before && Curves(*f.app) == authored && !f.history.UndoCount(), "canceling a prepared interpolation transaction requires no allocation");
        Commit(f, change); const auto after = f.Digest();
        failAfter = 0; const auto undo = f.history.History(false); failAfter = -1;
        Check(undo == MMS_E_MEMORY && f.Digest() == after && f.history.UndoCount() == 1, "failed camera interpolation undo leaves the live edit and cursor intact");
        Check(f.history.History(false) == MMS_OK, "camera interpolation undo retries after OOM");
        failAfter = 0; const auto redo = f.history.History(true); failAfter = -1;
        Check(redo == MMS_E_MEMORY && f.Digest() == before && f.history.RedoCount() == 1, "failed camera interpolation redo preserves the previous state and redo node");
        Check(f.history.History(true) == MMS_OK && f.Protected() == protectedBefore, "camera interpolation redo retries without seek or pose changes");
        // Publication in both directions is independently allocation-free.
        for (bool forward : {false, true}) {
            std::unique_ptr<plugins::PreparedHistoryChange> candidate;
            Check(change->Prepare(forward, candidate) == MMS_OK && candidate, "prepare camera interpolation inverse or redo privately");
            failAfter = 0; const bool current = candidate->IsCurrent(); if (current) candidate->Publish(); failAfter = -1;
            Check(current && f.Digest() == (forward ? after : before), "camera interpolation inverse and redo publish without allocation");
        }
    }
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        Fixture f; if (mutation == 5) Author(f);
        auto change = Plan(f, f.Options(6)); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("stale camera interpolation", change, transaction) == MMS_OK, "prepare stale camera interpolation fixture");
        auto* original = f.app->CameraKeys(); mdl::CameraKey* replacement = nullptr;
        if (mutation == 0) f.app->CameraKeys()[2].interpolation[0][0] ^= 1;
        if (mutation == 1) f.app->CameraKeys()[2].selected ^= 1;
        if (mutation == 2) f.app->CameraKeys()[1].eye[1] += 1;
        if (mutation == 3) {
            replacement = mdl::KeyPoolCreate<mdl::CameraKey>(4); std::memcpy(replacement, original, 4 * sizeof(*original));
            f.app->CameraKeys() = replacement;
        }
        if (mutation == 4) Author(f);
        if (mutation == 5) curve_host::Reset(f.app.get());
        const auto unchanged = f.Digest();
        failAfter = 0; const auto result = f.history.CommitNative(std::move(transaction)); failAfter = -1;
        Check(result == MMS_E_STALE && f.Digest() == unchanged && !f.history.UndoCount(), "stale interpolation keys marks identity or authored curves reject without allocation or partial writes");
        if (replacement) { f.app->CameraKeys() = original; mdl::KeyPoolDestroy(replacement); }
    }
    {
        Fixture f; Author(f); auto change = Plan(f, f.Options(6));
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("camera interpolation growth race", change, transaction) == MMS_OK,
            "prepare camera interpolation before an external pool growth");
        const auto identity = mdl::KeyPoolIdentity(f.app->CameraKeys());
        Check(mdl::KeyPoolEnsure(f.app->CameraKeys(), 65) && mdl::KeyPoolIdentity(f.app->CameraKeys()) == identity,
            "external camera growth keeps identity but changes the candidate's source allocation");
        const auto unchanged = f.Digest();
        failAfter = 0; const auto result = f.history.CommitNative(std::move(transaction)); failAfter = -1;
        Check(result == MMS_E_STALE && f.Digest() == unchanged && !f.history.UndoCount(),
            "pool growth after preparation invalidates the candidate without invalidating old committed history");
    }
    {
        Fixture f; Author(f); auto change = Plan(f, f.Options(0)); Commit(f, change);
        const auto authored = Curves(*f.app); const auto saved = SavedCurves(*f.app);
        auto* previous = f.app->CameraKeys();
        auto* replacement = mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(previous));
        std::memcpy(replacement, previous, mdl::KeyPoolCapacity(previous) * sizeof(*previous));
        Check(mdl::KeyPoolIdentity(replacement) != mdl::KeyPoolIdentity(previous),
            "same-content replacement has a distinct registered camera-pool identity");
        f.app->CameraKeys() = replacement;
        const auto before = f.Digest();
        Check(f.history.History(false) == MMS_E_STALE && f.Digest() == before && f.history.UndoCount() == 1,
            "committed interpolation cannot undo into an identical replacement pool");
        Check(Curves(*f.app) != authored && !curve_host::Has(f.app.get(), curve_host::Kind::Camera),
            "ordinary Read never transfers free or Step authoring to a replacement camera pool");
        pmm_extension::Document document; curve_host::Capture(*f.app, document);
        const auto* record = pmm_extension::Find(document, "org.mikudancestudio.animation_curves.v1");
        Check(record && record->value != saved,
            "PMM capture does not serialize the replaced camera object's old authored curves");
        f.app->CameraKeys() = previous; mdl::KeyPoolDestroy(replacement);
    }
}
void Routes() {
    for (bool reset : {false, true}) {
        Fixture f, oracle; oracle.Oracle(6, reset);
        SendMessageW(f.combo, CB_SETCURSEL, 6, 0);
        Check(!plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), reset),
            "ordinary unbound camera command retains its legacy owner");
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        auto* previous = g_Block; g_Block = f.app.get();
        CmdControl400(f.app.get(), f.app->MainWindow(), reset ? 432 : 431, 0);
        g_Block = previous;
        Check(f.history.UndoCount() == 1 && GlobalRegistrationSame(f.app->CameraKeys(), GlobalRegistrationKeys(oracle.app->CameraKeys())) &&
            f.app->SceneModified() == 1 && f.app->RecordedFrameCountByte1() == 1 && !f.app->PhysicsResetPending(),
            "actual bound camera 431/432 writes one common history node and agrees with the unbound byte oracle");
        Check(f.app->CurrentFrame() == 17 && f.app->LastRegisteredFrame() == 91 && f.app->CameraTrackCursor() == 2 &&
            f.app->CameraPositionX() == 4.125f && f.app->CameraFov() == 67.75f && f.app->CameraDistance() == -72.5f,
            "bound camera interpolation refresh preserves the unregistered camera and playback metadata");
        Check(f.history.History(false) == MMS_OK, "bound camera interpolation retains redo after undo");
        for (std::size_t slot = 0; slot < mdl::KeyPoolCapacity(f.app->CameraKeys()); ++slot) f.app->CameraKeys()[slot].selected = 0;
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        const auto unchanged = f.Digest();
        Check(plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), reset) && f.Digest() == unchanged && f.history.RedoCount() == 1,
            "bound no-selection camera interpolation consumes the command without dirtying or dropping redo");
        f.app->CameraKeys()[0].selected = 1;
        for (unsigned gate = 0; gate < 4; ++gate) {
            if (gate == 0) f.app->PlaybackActive() = 1;
            if (gate == 1) f.app->FrameStepPlayback() = 1;
            if (gate == 2) f.app->state.recordPlaybackStartPending = 1;
            if (gate == 3) Check(f.history.Enter() == MMS_OK, "hold the camera interpolation common-history gate");
            const auto blocked = f.Digest();
            Check(plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), reset) && f.Digest() == blocked && f.history.RedoCount() == 1,
                "busy playback or pending recording never falls through to legacy camera interpolation");
            if (gate == 3) f.history.Leave();
            f.app->PlaybackActive() = f.app->FrameStepPlayback() = f.app->state.recordPlaybackStartPending = 0;
        }
        SendMessageW(f.combo, CB_SETCURSEL, WPARAM(-1), 0);
        const auto invalid = f.Digest();
        Check(plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), reset) && f.Digest() == invalid && f.history.RedoCount() == 1,
            "invalid camera interpolation combo is consumed without legacy writes");
        SendMessageW(f.combo, CB_SETCURSEL, 6, 0);
        failAfter = 0; const bool handled = plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), reset); failAfter = -1;
        Check(handled && f.Digest() == invalid && f.history.RedoCount() == 1,
            "camera interpolation route consumes preparation OOM without dirtying or clearing redo");
    }
}
void PluginPending() {
    Fixture f; auto change = Plan(f, f.Options(6)); plugins::EditStore edits(f.history, f.storage);
    plugins::EditClient client{&edits, "test.camera.interpolation", false}; auto api = edits.Api(client);
    std::uint64_t revision = 0; Check(f.history.Revision(revision) == MMS_OK, "read camera interpolation plugin revision");
    MmsHandle pending = 0; const char label[] = "pending camera plugin";
    Check(api.begin(api.context, {label, sizeof(label) - 1}, revision, &pending) == MMS_OK &&
        edits.JoinNative(client, pending, change) == MMS_OK && api.prepare(api.context, pending) == MMS_OK,
        "prepare a plugin transaction owning camera interpolation");
    const auto before = f.Digest(); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("competing interpolation", change, transaction) == MMS_E_BUSY && !transaction && f.Digest() == before,
        "prepared plugin transaction blocks competing camera interpolation without falling through");
    {
        plugins::NativePoseTestBinding binding(*f.app, f.history); SendMessageW(f.combo, CB_SETCURSEL, 6, 0);
        Check(plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), false) && f.Digest() == before,
            "actual camera interpolation route consumes a plugin-pending rejection without legacy writes");
    }
    api.cancel(api.context, pending);
    Check(f.Digest() == before && !f.history.UndoCount(), "canceling pending plugin interpolation leaves every native byte unchanged");
    Commit(f, change);
}
} // namespace camera_interpolation_checks
namespace {
void NativeCameraInterpolationChecks() {
    using namespace camera_interpolation_checks;
    std::puts("RUN camera interpolation legacy six-channel parity"); LegacyParity();
    std::puts("RUN camera interpolation authored curves and owned growth"); AuthoringAndGrowth();
    std::puts("RUN camera interpolation followed by registration growth"); RegistrationGrowthComposition();
    std::puts("RUN camera interpolation no-op malformed and borrowed rejection"); NoopAndInvalid();
    std::puts("RUN camera interpolation OOM budget stale and no-allocation publication"); FailuresAndStale();
    std::puts("RUN camera interpolation actual bound command routes"); Routes();
    std::puts("RUN camera interpolation plugin transaction exclusion"); PluginPending();
}
}
