// Include after native_morph_curve_write_checks.inc. These checks drive the
// real CmdFileMenu 226/228/231 routes and their actual confirmation dialogs.
#include "../src/plugins/native_morph_category.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include <functional>
#include <cwchar>
namespace {
namespace native_morph_category_checks {
using namespace plugins;
unsigned CommandId(unsigned panel) { return panel == 3 ? 226 : panel == 2 ? 228 : 231; }
struct Confirmation {
    inline static thread_local Confirmation* current = nullptr;
    HHOOK hook = nullptr;
    HWND owner;
    unsigned panel, seen = 0;
    int response;
    bool reversible;
    std::function<void()> during;
    static LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && current) {
            const auto& message = *reinterpret_cast<CWPRETSTRUCT*>(lp);
            if (message.message == WM_INITDIALOG && GetWindow(message.hwnd, GW_OWNER) == current->owner) {
                wchar_t type[32]{}, title[128]{}; GetClassNameW(message.hwnd, type, 32);
                if (!std::wcscmp(type, L"#32770")) {
                    GetWindowTextW(message.hwnd, title, 128);
                    const wchar_t* expected = current->panel == 3 ? L"Delete lip frames" :
                        current->panel == 2 ? L"Delete eye frames" : L"Delete eyebrow frames";
                    Check(!std::wcscmp(title, expected), "real category confirmation matches the requested MorphPanel");
                    wchar_t body[512]{}; GetDlgItemTextW(message.hwnd, 0xffff, body, 512);
                    Check(current->reversible ? (std::wcsstr(body, L"You can undo this operation.") && std::wcsstr(body, L"weights to 0")) :
                        std::wcsstr(body, L"This cannot be undone.") != nullptr,
                        "bound category confirmation describes clearing current weights and undo while legacy keeps its original warning");
                    ++current->seen; ShowWindow(message.hwnd, SW_HIDE);
                    if (current->during) current->during();
                    const auto button = GetDlgItem(message.hwnd, current->response);
                    Check(button != nullptr, "actual category confirmation has the requested OK or Cancel button");
                    PostMessageW(message.hwnd, WM_COMMAND, MAKEWPARAM(current->response, BN_CLICKED), reinterpret_cast<LPARAM>(button));
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    Confirmation(MMDApp& app, unsigned p, int answer = IDOK, bool canUndo = true)
        : owner(app.MainWindow()), panel(p), response(answer), reversible(canUndo) {
        Check(!current, "one category confirmation observer"); current = this;
        hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Inspect, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "observe production category confirmation");
    }
    ~Confirmation() { UnhookWindowsHookEx(hook); current = nullptr; }
};
struct Scene : MorphCurveWriteFixture {
    Scene() {
        app->EnglishUI() = 1; app->PhysicsResetPending() = 0;
        for (auto* table : {&firstMorphs, &secondMorphs}) for (unsigned morph = 0; morph < 3; ++morph) {
            (*table)[morph].panel = static_cast<mdl::MorphPanel>(morph + 1);
            // A category is unrelated to its PMX deformation type.
            (*table)[morph].type = static_cast<std::uint8_t>(8 - morph * 3);
        }
        for (auto* f : {&first, &second}) {
            f->m->morphKeys[0].previous = 11;
            f->m->morphKeys[2].reserved[2] = 81;
            f->m->morphKeys[7].allocated = 13;
            f->m->morphKeys[1].allocated = 9;
            f->m->morphKeys[15].reserved[0] = 73;
        }
        app->LastRegisteredFrame() = 93;
    }
    std::array<std::uint32_t, 3> Weights(unsigned slot = 0) const {
        std::array<std::uint32_t, 3> out{};
        const auto& table = slot ? secondMorphs : firstMorphs;
        for (unsigned i = 0; i < out.size(); ++i) std::memcpy(&out[i], &table[i].value, sizeof out[i]);
        return out;
    }
    void Command(unsigned panel) { CmdFileMenu(app.get(), app->MainWindow(), static_cast<std::uint16_t>(CommandId(panel)), 0); }
    std::shared_ptr<HistoryChange> PlanCategory(unsigned panel) {
        std::shared_ptr<HistoryChange> out;
        Check(PlanMorphCategoryPurge(*app, app->SelectedModelSlot(), panel, out) == MMS_OK, "prepare private category purge");
        return out;
    }
    void CommitCategory(const std::shared_ptr<HistoryChange>& change) {
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("category purge", change, transaction, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK,
            "prepare a complete explicit category transaction");
        Check(CommitRegistrationNoAllocation(history, std::move(transaction)) == MMS_OK,
            "category keys author layers weights and cursors publish without allocation");
    }
    std::vector<std::uint8_t> Layers(unsigned slot = 0) {
        morph_curve::AuthoredLayers layers; std::vector<animation_curve::NamedTrack> tracks;
        std::vector<std::uint8_t> bytes; std::string error;
        Check(morph_curve::CaptureLayers(slot ? second.raw() : first.raw(), layers, error) &&
            morph_curve::LayersTracks(layers, tracks, error) && animation_curve::Encode(tracks, bytes, error),
            "capture exact pristine morph tracks without synchronizing caches");
        return bytes;
    }
    void Authors() {
        std::vector<animation_curve::NamedTrack> tracks;
        for (unsigned morph = 0; morph < 3; ++morph) tracks.push_back(Input(morph));
        std::string error; Check(morph_curve::Write(first.raw(), tracks, error), "seed Free author tracks for every category");
        for (unsigned morph = 0; morph < 3; ++morph) firstMorphs[morph].value = .31415927f + float(morph);
    }
};
void ActualMenusAndLegacyParity() {
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly})
        for (unsigned panel : {1u, 2u, 3u}) {
            std::printf("  category parity mode=%u panel=%u legacy\n", unsigned(mode), panel);
            std::vector<mdl::MorphKey> oracle;
            {
                Scene native; const auto previous = g_Block;
                // The original menu tail always paints its timeline using
                // Renderer::screenWidth/height, even without a D3D device.
                // Supply that real layout object for this independent oracle.
                auto renderer = std::make_unique<D3DRenderer>(); native.app->Renderer() = renderer.get();
                Confirmation prompt(*native.app, panel, IDOK, false); native.Command(panel);
                Check(prompt.seen == 1, "unbound original category menu confirms once");
                oracle = MorphKeysCopy(native.first); native.app->Renderer() = nullptr; g_Block = previous;
            }
            std::printf("  category parity mode=%u panel=%u bound\n", unsigned(mode), panel);
            Scene f; NativePoseTestBinding binding(*f.app, f.history);
            Check(f.history.SetMode(mode) == MMS_OK, "choose category recording mode");
            const auto keys = MorphKeysCopy(f.first);
            const auto boneKeys = RegistrationKeys(f.first);
            const auto weights = f.Weights(), other = f.Weights(1);
            const auto bones = f.first.bones;
            const auto flags = f.first.dirty, selection = f.first.selected;
            const auto cursors = f.firstCursors; const auto active = f.firstActive;
            const auto ring = PoseRingDigest(f.first);
            f.Type(544, "unregistered bone draft");
            { Confirmation prompt(*f.app, panel); f.Command(panel); Check(prompt.seen == 1, "bound actual category menu confirms once"); }
            Check(f.history.UndoCount() == 1 && f.history.RedoCount() == 0 && MorphKeysMatch(f.first, oracle),
                "real 226/228/231 menu publishes exact legacy key bytes once in both history modes");
            for (unsigned morph = 0; morph < 3; ++morph)
                Check(f.Weights()[morph] == (morph + 1 == panel ? 0u : weights[morph]),
                    "only the requested MorphPanel clears current weights including zero-frame-only morphs");
            char draft[64]{}; GetWindowTextA(f.Edit(544), draft, sizeof draft);
            Check(!std::strcmp(draft, "unregistered bone draft") && SendMessageA(f.Edit(544), EM_GETMODIFY, 0, 0) &&
                f.Weights(1) == other && !std::memcmp(bones.data(), f.first.bones.data(), sizeof bones) &&
                flags == f.first.dirty && selection == f.first.selected && SameRegistrationKeys(f.first, boneKeys) &&
                PoseRingDigest(f.first) == ring && f.SelectionUnchanged() && f.app->CurrentFrame() == 7 &&
                f.first.m->maxFrame == 99 && f.first.m->searchCursor == 5 && f.app->LastRegisteredFrame() == 93 &&
                f.app->SceneModified() && !f.app->PhysicsResetPending(),
                "category publication preserves other model bones drafts key pools physics flags selection timeline and high-water metadata");
            for (unsigned morph = 0; morph < 3; ++morph) if (morph + 1 != panel)
                Check(f.firstCursors[morph] == cursors[morph] && f.firstActive[morph] == active[morph],
                    "unaffected category cursors are not rewritten by broad native seek");
            f.app->SelectedModelSlot() = 1; f.app->CameraMode() = 1; f.app->CurrentFrame() = 37;
            Check(f.history.History(false) == MMS_OK && MorphKeysMatch(f.first, keys) && f.Weights() == weights &&
                f.Weights(1) == other && f.app->CurrentFrame() == 37,
                "category undo restores original zero-frame payloads reserved bytes marks and exact discarded draft weights across models and playheads");
            Check(f.history.History(true) == MMS_OK && MorphKeysMatch(f.first, oracle), "category redo restores exact cleared native keys without source files");
        }
}
void AuthoringAndSparsePristine() {
    for (unsigned panel : {1u, 2u, 3u}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Authors();
        // Preserve a pristine key that the native mirror never contained. A
        // category purge must not let such a key continue playing afterwards.
        morph_curve::AuthoredLayers captured, sparse; std::vector<animation_curve::NamedTrack> tracks; std::string error;
        Check(morph_curve::CaptureLayers(f.first.raw(), captured, error) && morph_curve::LayersTracks(captured, tracks, error),
            "capture full raw author data before sparse-author counterexample");
        auto extra = tracks[panel - 1].track.keys[1]; extra.frame = 7; extra.value = 1.7;
        tracks[panel - 1].track.keys.insert(tracks[panel - 1].track.keys.begin() + 1, extra);
        Check(morph_curve::RebuildLayers(f.first.raw(), tracks, sparse, error), "prepare sparse pristine author over unchanged native chain");
        morph_curve::SwapLayers(f.first.raw(), sparse);
        const auto before = MorphKeysCopy(f.first);
        const auto weights = f.Weights(); const auto authors = f.Layers();
        auto change = f.PlanCategory(panel); Check(bool(change), "authored category clearing is a real change");
        std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(true, candidate) == MMS_OK && candidate->IsCurrent() && MorphKeysMatch(f.first, before) && f.Layers() == authors,
            "full authored category candidate leaves pristine effective observed and mirrored layers untouched during preparation");
        candidate.reset(); Check(f.Layers() == authors && MorphKeysMatch(f.first, before), "cancelling private category candidate leaves all author layers intact");
        f.CommitCategory(change); const auto cleared = MorphKeysCopy(f.first);
        for (int frame : {0, 7, 18, 51, 120}) {
            animation_curve::Track track; Check(morph_curve::Read(f.first.raw(), int(panel - 1), track, error), "read cleared category effective track");
            Check(animation_curve::Evaluate(track, frame).value == 0, "cleared category remains zero at every sampled time including unmatched pristine author keys");
        }
        for (unsigned morph = 0; morph < 3; ++morph) if (morph + 1 != panel) {
            const auto actual = f.Effective(morph);
            Check(animation_curve::Evaluate(actual.track, 13).value == animation_curve::Evaluate(tracks[morph].track, 13).value,
                "clearing one category preserves another category's exact Free curve");
        }
        Check(f.history.History(false) == MMS_OK && MorphKeysMatch(f.first, before) && f.Weights() == weights && f.Layers() == authors,
            "undo restores every exact sparse Free key slope tangent handle and current draft weight");
        Check(f.history.History(true) == MMS_OK && MorphKeysMatch(f.first, cleared), "redo remains valid after effective-cache reads");
    }
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); auto track = f.Input(0);
        track.track.keys[1].outSlope = track.track.keys[2].inSlope = 1.8e38;
        std::string error; Check(morph_curve::Write(f.first.raw(), {track}, error), "old VMD-compatible author accepts finite double controls beyond float range");
        const auto authors = f.Layers(); const auto before = MorphKeysCopy(f.first); auto change = f.PlanCategory(1);
        Check(bool(change), "deleting author data does not require converting its old large double control hull to float");
        f.CommitCategory(change); Check(f.history.History(false) == MMS_OK && f.Layers() == authors && MorphKeysMatch(f.first, before),
            "large original Free control values are restored exactly without narrowing");
    }
}
void ModalCancellationAndFreshness() {
    for (unsigned reason = 0; reason < 6; ++reason) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        const auto keys = MorphKeysCopy(f.first); const auto weights = f.Weights();
        std::unique_ptr<NativePoseTestBinding> rebound;
        { Confirmation prompt(*f.app, 1, reason == 0 ? IDCANCEL : IDOK);
            prompt.during = [&] {
                if (reason == 1) f.first.m->morphKeys[0].value += .2f;
                if (reason == 2) f.app->CurrentFrame() = 19;
                if (reason == 3) rebound = std::make_unique<NativePoseTestBinding>(*f.app, f.history);
                if (reason == 4) { bool changed = false; Check(ApplyBoundMorphValue(*f.app, 1, 0, .75f, changed) == MMS_OK && changed,
                    "modal loop can perform a distinct real history edit"); }
                if (reason == 5) f.firstMorphs[0].name[0] ^= 1;
            };
            f.Command(1); Check(prompt.seen == 1, "one real confirmation for cancelled or stale category command"); }
        Check(f.history.UndoCount() == (reason == 4 ? 1u : 0u) && f.Weights() == weights &&
            (reason == 1 || MorphKeysMatch(f.first, keys)),
            "cancelled or stale modal category command never partially clears native keys current weights or an intervening history entry");
    }
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        auto change = f.PlanCategory(1); std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("stale category", change, transaction) == MMS_OK, "freeze actual category publication candidate");
        f.first.m->morphKeys[15].reserved[1] ^= 1; const auto keys = MorphKeysCopy(f.first); const auto weights = f.Weights();
        Check(f.history.CommitNative(std::move(transaction)) == MMS_E_STALE && MorphKeysMatch(f.first, keys) && f.Weights() == weights && !f.history.UndoCount(),
            "post-prepare pool mutation rejects entire category publication without restoring over the external write");
    }
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Authors();
        auto change = f.PlanCategory(1); std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(true, candidate) == MMS_OK && candidate->IsCurrent(), "freeze category author erasure candidate");
        auto edited = f.Input(0); edited.track.keys[1].inSlope += .125;
        morph_curve::AuthoredLayers replacement; std::string error;
        Check(morph_curve::RebuildLayers(f.first.raw(), {edited, f.Input(1), f.Input(2)}, replacement, error),
            "prepare tangent-only author conflict without changing native pool");
        morph_curve::SwapLayers(f.first.raw(), replacement); const auto before = MorphKeysCopy(f.first);
        const auto weights = f.Weights();
        Check(!candidate->IsCurrent() && MorphKeysMatch(f.first, before) && f.Weights() == weights,
            "a post-prepare Free tangent conflict invalidates the whole category publication before native writes");
        morph_curve::SwapLayers(f.first.raw(), replacement);
        Check(candidate->IsCurrent(), "restoring the exact original author state restores candidate freshness");
        failAfter = 0; candidate->Publish(); candidate.reset(); failAfter = -1;
        Check(f.Weights()[0] == 0 && f.Weights()[1] == weights[1] && f.Weights()[2] == weights[2],
            "validated category author erasure completes all writes with allocation disabled");
    }
    {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        f.CommitCategory(f.PlanCategory(1));
        auto foreignTrack = f.Input(0); morph_curve::AuthoredLayers foreign; std::string error;
        Check(morph_curve::RebuildLayers(f.first.raw(), {foreignTrack}, foreign, error),
            "prepare a foreign Free author on a previously unauthored cleared category");
        morph_curve::SwapLayers(f.first.raw(), foreign); const auto keys = MorphKeysCopy(f.first);
        Check(f.history.History(false) == MMS_E_STALE && MorphKeysMatch(f.first, keys) && f.history.UndoCount() == 1,
            "category ownership includes author absence and refuses a foreign same-channel author without partial native undo");
        morph_curve::SwapLayers(f.first.raw(), foreign);
        Check(f.history.History(false) == MMS_OK, "restoring cleared author absence makes the retained category undo available again");
    }
}
void MultipleMorphsAndExactWeightBits() {
    Scene f; NativePoseTestBinding binding(*f.app, f.history);
    f.firstMorphs[1].panel = mdl::MorphPanel::eyebrow; f.Authors();
    const std::uint32_t nan = 0x7fc12345u, negativeZero = 0x80000000u;
    std::memcpy(&f.firstMorphs[0].value, &nan, sizeof nan);
    std::memcpy(&f.firstMorphs[1].value, &negativeZero, sizeof negativeZero);
    const auto keys = MorphKeysCopy(f.first); const auto weights = f.Weights(); const auto authors = f.Layers();
    auto change = f.PlanCategory(1); Check(bool(change), "one category plan covers several morphs with different PMX types");
    f.CommitCategory(change);
    Check(f.Weights()[0] == 0 && f.Weights()[1] == 0 && f.Weights()[2] == weights[2] &&
        f.first.m->morphKeys[0].next == 0 && f.first.m->morphKeys[1].next == 0,
        "one category commit clears every matching visible or hidden morph and both complete native chains");
    Check(f.history.UndoCount() == 1 && f.history.History(false) == MMS_OK && MorphKeysMatch(f.first, keys) &&
        f.Weights() == weights && f.Layers() == authors,
        "undo restores exact NaN payload signed-zero draft bits and both Free author layers without numeric conversion");
    const auto identity = mdl::MorphKeyPoolIdentity(f.first.m->morphKeys);
    const auto tail = mdl::MorphKeyCapacity(f.first.m->morphKeys);
    Check(mdl::MorphKeyPoolEnsure(*f.first.m, tail + 1) && mdl::MorphKeyPoolIdentity(f.first.m->morphKeys) == identity,
        "grow the original category key pool without replacing stable identity");
    f.first.m->morphKeys[tail].reserved[2] = 99;
    Check(f.history.History(true) == MMS_OK && f.Weights()[0] == 0 && f.Weights()[1] == 0 &&
        f.first.m->morphKeys[tail].reserved[2] == 99,
        "category redo follows relocated pool identity and preserves unowned newly allocated tail bytes");
}
void NoopBudgetsAndAllocationFailures() {
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "configure category no-op mode"); f.CommitCategory(f.PlanCategory(1));
        bool changed = false; Check(ApplyBoundMorphValue(*f.app, 0, 1, .75f, changed) == MMS_OK && changed,
            "seed unrelated scalar change after category purge");
        // Explicit-only scalar adjustments are not recorded; explicitly join
        // a fresh real category record instead to establish redo in either mode.
        if (mode == SharedHistory::RecordingMode::ExplicitOnly) f.CommitCategory(f.PlanCategory(2));
        Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "seed real redo before no-op category command");
        std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        { Confirmation prompt(*f.app, 1); f.Command(1); Check(prompt.seen == 1, "no-op preserves original single confirmation"); }
        f.history.Revision(after);
        Check(revision == after && f.history.RedoCount() == 1 && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(),
            "unchanged category preserves redo revision and dirty hints in both modes");
        Scene tiny; NativePoseTestBinding inner(*tiny.app, tiny.history); tiny.Authors(); tiny.app->state.enterKeyState = 1;
        const auto before = tiny.Digest(); const auto authors = tiny.Layers();
        Check(tiny.history.Configure(mode, 128, 1) == MMS_OK, "configure one-byte category history budget");
        { Confirmation prompt(*tiny.app, 1); tiny.Command(1); }
        Check(tiny.Digest() == before && tiny.Layers() == authors && !tiny.history.UndoCount() && !tiny.history.RetainedBytes(),
            "explicit category commit refuses insufficient budget in both modes without partial native or author writes");
    }
    Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Authors();
    bool scalarChanged = false;
    Check(ApplyBoundMorphValue(*f.app, 1, 0, .765f, scalarChanged) == MMS_OK && scalarChanged &&
        f.history.History(false) == MMS_OK && f.history.RedoCount() == 1,
        "seed a real other-model redo before category allocation injection");
    const auto before = f.Digest(); const auto authors = f.Layers(); unsigned failures = 0; bool success = false;
    for (long allocation = 0; allocation < 2048; ++allocation) {
        std::shared_ptr<HistoryChange> change; std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        failAfter = allocation;
        auto result = PlanMorphCategoryPurge(*f.app, 0, 1, change);
        if (result == MMS_OK) result = Boundary([&] { return f.history.PrepareNative("category allocation", change, transaction); });
        failAfter = -1;
        Check(f.Digest() == before && f.Layers() == authors && !f.history.UndoCount() && f.history.RedoCount() == 1,
            "every actual category plan and reservation allocation failure preserves complete source buffers author state and journal");
        if (result == MMS_OK) {
            Check(transaction && failures > 20, "category allocation sweep reaches all private author and journal preparation stages");
            Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK, "fully prepared category candidate commits with allocations disabled");
            success = true; break;
        }
        Check(result == MMS_E_MEMORY && !transaction, "category allocation refusal reports memory and releases journal reservation");
        Check(f.history.CheckNativeEdit() == MMS_OK, "failed category preparation releases the history gate"); ++failures;
    }
    Check(success && f.history.UndoCount() == 1 && f.history.RedoCount() == 0 && f.history.History(false) == MMS_OK && f.Layers() == authors,
        "successful category publication remains exactly undoable after allocation failures");
}
void ObjectAndPluginChronology() {
    Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Authors();
    const auto before = MorphKeysCopy(f.first); const auto authors = f.Layers(); const auto weights = f.Weights();
    auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 0); const auto identity = SceneObjectIdentity(f.first.raw());
    f.CommitCategory(f.PlanCategory(1));
    f.app->SelectedModelSlot() = 1; f.CommitCategory(f.PlanCategory(2));
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("remove category model", owner->CaptureRemoval(4096), transaction) == MMS_OK &&
        f.history.CommitNative(std::move(transaction)) == MMS_OK && !f.app->ModelSlot(0),
        "object removal follows category edits in the same journal");
    Check(f.history.History(false) == MMS_OK && SceneObjectIdentity(f.first.raw()).generation != identity.generation &&
        f.history.History(false) == MMS_OK && f.history.History(false) == MMS_OK &&
        MorphKeysMatch(f.first, before) && f.Layers() == authors && f.Weights() == weights,
        "undo object deletion and cross-model category edits restores stable identities exact author data and discarded draft weights");
    Check(f.history.History(true) == MMS_OK && f.history.History(true) == MMS_OK && f.history.History(true) == MMS_OK,
        "redo categories then deletion does not reopen any original model file");
    Check(f.history.History(false) == MMS_OK, "restore model before releasing caller-owned fixture resources");
    Check(f.history.Reset() == MMS_OK, "clear object chronology before mixed plugin test"); f.history.Collect();
    f.app->SelectedModelSlot() = 0;
    // Reset the categories first, then join the same real private native plan
    // to a public plugin transaction. No alternate history mechanism is used.
    auto native = f.PlanCategory(3); Check(bool(native), "mouth category remains available for mixed plugin transaction");
    native_model_order_checks::PluginState plugin; StorageClient storageClient; storageClient.id = "test.morph-category";
    f.storage.Api(storageClient); auto serializer = plugin.Serializer(); MmsHandle provider = 0;
    Check(f.storage.Register(storageClient, Text(storageClient.id), &serializer, sizeof serializer, &provider) == MMS_OK,
        "register actual serializer for category plugin chronology");
    EditStore edits(f.history, f.storage); EditClient client; client.id = storageClient.id; auto api = edits.Api(client);
    std::uint64_t revision = 0; MmsHandle edit = 0; constexpr char label[] = "plugin category purge";
    auto participant = plugin.Participant(Text(client.id));
    Check(f.history.Revision(revision) == MMS_OK && api.begin(api.context, {label, sizeof label - 1}, revision, &edit) == MMS_OK &&
        api.join(api.context, edit, &participant) == MMS_OK && edits.JoinNative(client, edit, native) == MMS_OK &&
        api.prepare(api.context, edit) == MMS_OK && api.commit(api.context, edit) == MMS_OK && plugin.value == 42 && f.history.UndoCount() == 1,
        "plugin private state and category purge atomically commit as one real public EditStore transaction");
    Check(api.undo(api.context) == MMS_OK && plugin.value == 0 && api.redo(api.context) == MMS_OK && plugin.value == 42,
        "plugin undo and redo restore the category participant through the shared journal");
    Check(f.history.Reset() == MMS_OK, "retire mixed category nodes before unloading test serializer"); f.history.Collect();
    Check(f.storage.Unregister(storageClient, provider) == MMS_OK, "unregister category serializer after retained history cleanup");
}
void OpenEditorRefresh() {
    for (bool failedRead : {false, true}) {
        Scene f; NativePoseTestBinding binding(*f.app, f.history); f.Authors();
        unsigned reads = 0; bool refuse = false;
        curve_editor::Source source; source.id = "category-test"; source.label = L"Morph category";
        source.read = [&](std::vector<curve_editor::Channel>& out, std::wstring& error) {
            ++reads;
            if (refuse) { error = L"test read failure"; return false; }
            animation_curve::Track track; std::string detail;
            if (!morph_curve::Read(f.first.raw(), 0, track, detail)) { error = L"source unavailable"; return false; }
            curve_editor::Channel channel; channel.id = "weight"; channel.label = L"Weight"; channel.track = std::move(track);
            out = {std::move(channel)}; return true;
        };
        curve_editor::Session session; session.id = "category-notification"; session.sources.push_back(std::move(source));
        const auto editor = static_cast<HWND>(curve_editor::Open(f.app->MainWindow(), std::move(session), false));
        struct CloseEditor { HWND owner; ~CloseEditor() { curve_editor::CloseOwned(owner); } } close{f.app->MainWindow()};
        Check(editor != nullptr && reads != 0, "open a real hidden curve editor on the category source");
        const auto previousReads = reads; refuse = failedRead;
        { Confirmation prompt(*f.app, 1); f.Command(1); Check(prompt.seen == 1, "category command confirms while editor is open"); }
        SendMessageW(editor, WM_APP + 401, 0, 0);
        Check(reads > previousReads && f.history.UndoCount() == 1 && f.Weights()[0] == 0,
            "actual category menu notifies an already open editor and read failure never repeats or rolls back the committed operation");
        refuse = false;
        if (failedRead) { curve_editor::NotifyOwnedChanged(f.app->MainWindow()); SendMessageW(editor, WM_APP + 401, 0, 0); }
        double sample[3] = {13, -1, -1};
        Check(SendMessageW(editor, WM_APP + 396, 0, reinterpret_cast<LPARAM>(sample)) && sample[1] == 0 &&
            f.history.UndoCount() == 1 && f.history.RedoCount() == 0,
            "open curve editor adopts the cleared channel through read-only notification without a second history entry");
    }
}
}
void NativeMorphCategoryChecks() {
    std::puts("  category: actual menus and independent legacy parity");
    native_morph_category_checks::ActualMenusAndLegacyParity();
    std::puts("  category: sparse pristine authors and finite double controls");
    native_morph_category_checks::AuthoringAndSparsePristine();
    std::puts("  category: modal cancellation and candidate freshness");
    native_morph_category_checks::ModalCancellationAndFreshness();
    std::puts("  category: multiple morphs and exact weight bits");
    native_morph_category_checks::MultipleMorphsAndExactWeightBits();
    std::puts("  category: no-op budget and allocation failures");
    native_morph_category_checks::NoopBudgetsAndAllocationFailures();
    std::puts("  category: object and real plugin chronology");
    native_morph_category_checks::ObjectAndPluginChronology();
    std::puts("  category: open editor notification and read failure");
    native_morph_category_checks::OpenEditorRefresh();
}
}
