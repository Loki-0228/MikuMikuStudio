#include "../src/plugins/native_key_commands.hpp"
namespace mikumikustudio { void CmdFileMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace {
namespace unused_key_checks {
using namespace plugins;
void Constant(ModelKeyFixture& f) {
    for (auto* value : {&f.first, &f.second}) {
        for (unsigned slot : {0u, 2u, 3u}) {
            auto& key = value->m->boneKeys[slot];
            std::fill(std::begin(key.position), std::end(key.position), 0.f);
            std::fill(std::begin(key.rotation), std::end(key.rotation), 0.f); key.rotation[3] = 1;
            key.physicsDisabled = 0;
        }
        for (unsigned slot : {0u, 1u, 2u}) {
            value->m->morphKeys[slot].value = .25f;
            value->m->displayKeys[slot].visible = 1;
            value->m->displayKeys[slot].ikStates[0] = 1;
        }
        value->m->boneKeys[1].allocated = 3;
        value->m->morphKeys[0].allocated = 3;
        value->m->displayKeys[2].allocated = 3;
        value->dirty[0] = 1; value->bones[0].trans[0] = 91.f;
    }
}
std::shared_ptr<HistoryChange> Plan(ModelKeyFixture& f, std::uint32_t expected = 3) {
    std::shared_ptr<HistoryChange> change; std::uint32_t count = 99;
    Check(PlanUnusedModelKeyDeletion(*f.app, 0, false, change, count) == MMS_OK && count == expected,
        "unused plan identifies exact three-family interior selection");
    Check(bool(change) == (expected != 0), "unused empty selection is a semantic no-op"); return change;
}
struct Report {
    static inline thread_local Report* active = nullptr;
    HHOOK hook = nullptr; unsigned seen = 0;
    static LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && active && reinterpret_cast<const CWPRETSTRUCT*>(lp)->message == WM_INITDIALOG) {
            HWND window = reinterpret_cast<const CWPRETSTRUCT*>(lp)->hwnd;
            wchar_t name[32]{}; GetClassNameW(window, name, 32);
            if (!std::wcscmp(name, L"#32770")) {
                ++active->seen; ShowWindow(window, SW_HIDE);
                const auto button = GetDlgItem(window, IDOK) ? GetDlgItem(window, IDOK) : GetDlgItem(window, IDCANCEL);
                Check(button != nullptr, "unused report exposes an acknowledge button");
                PostMessageW(window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    Report() { Check(!active, "unused report hook has one owner"); active = this;
        hook = SetWindowsHookExW(WH_CALLWNDPROCRET, Inspect, nullptr, GetCurrentThreadId()); Check(hook != nullptr, "unused report hook installed"); }
    ~Report() { UnhookWindowsHookEx(hook); active = nullptr; }
};
void RoundTrip() {
    for (auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly}) {
        ModelKeyFixture f; Constant(f); NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "unused command recording mode configured");
        const ModelKeyState before(f.first), peer(f.second); const auto ring = PoseRingDigest(f.first);
        auto change = Plan(f);
        Check(before.Matches(f.first) && f.first.morphs[0].value == .91f && f.first.bones[0].trans[0] == 91.f,
            "finding unused keys never writes live marks, keys or dirty pose");
        { Report report; CmdFileMenu(f.app.get(), f.app->MainWindow(), 222, 0);
          Check(report.seen == 1, "actual menu 222 reports one completed operation"); }
        Check(f.history.UndoCount() == 1 && f.first.m->boneKeys[2].frame == 0 && f.first.m->morphKeys[1].frame == 0 &&
            f.first.m->displayKeys[1].frame == 0 && f.first.m->boneKeys[3].frame == 30 &&
            f.first.m->morphKeys[2].frame == 30 && f.first.m->displayKeys[2].frame == 30,
            "actual unused menu commits three interior deletions as one explicit node");
        Check(f.first.m->boneKeys[1].allocated == 0 && f.first.m->morphKeys[0].allocated == 0 && f.first.m->displayKeys[2].allocated == 0,
            "unused commit clears old marks together with its private selection");
        Check(f.first.morphs[0].value == .91f && f.first.bones[0].trans[0] == 91.f && f.app->CurrentFrame() == 10 &&
            peer.Matches(f.second) && PoseRingDigest(f.first) == ring && f.SelectionUnchanged(),
            "unused command preserves drafts, peer model, playhead, selection and old rings");
        const ModelKeyState after(f.first);
        Check(f.history.History(false) == MMS_OK && before.Matches(f.first), "unused undo restores keys and original marks");
        Check(f.history.History(true) == MMS_OK && after.Matches(f.first), "unused redo replays the captured result");
    }
    ModelKeyFixture f; f.Marks(); ModelKeyCommit(f, ModelKeyPlan(f));
    Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "unused no-op fixture has redo");
    const auto before = f.first.Digest(); Check(!Plan(f, 0) && f.history.RedoCount() == 1 && f.first.Digest() == before,
        "nonredundant keys preserve redo and marks");
}
void Boundaries() {
    { ModelKeyFixture f; Constant(f); f.first.m->boneKeys[2].physicsDisabled = 1;
      auto change = Plan(f, 2); ModelKeyCommit(f, change);
      Check(f.first.m->boneKeys[2].frame == 10, "equal transforms cannot delete a physics enable transition"); }
    { ModelKeyFixture f; Constant(f); const auto before = f.first.Digest();
      ModelKeySelection selected; selected.bones = {2, 2}; std::shared_ptr<HistoryChange> change;
      Check(PlanModelKeyDeletion(*f.app, 0, false, change, false, &selected) == MMS_E_ARGUMENT && !change && f.first.Digest() == before,
          "duplicate private slots reject before changing marks");
      selected.bones = {UINT32_MAX};
      Check(PlanModelKeyDeletion(*f.app, 0, false, change, false, &selected) == MMS_E_ARGUMENT && !change && f.first.Digest() == before,
          "out-of-range private slots reject before reading the pool");
      selected.bones.clear();
      Check(PlanModelKeyDeletion(*f.app, 0, false, change, true, &selected) == MMS_OK && !change,
          "empty private selection never falls back to live marks or selected bone"); }
    { ModelKeyFixture f; Constant(f); f.first.m->morphKeys[2].next = 1;
      const auto before = f.first.Digest(); std::uint32_t count = 99; std::shared_ptr<HistoryChange> change;
      Check(PlanUnusedModelKeyDeletion(*f.app, 0, false, change, count) == MMS_E_ARGUMENT && !count && !change && f.first.Digest() == before,
          "cyclic unused track rejects without partial marking or looping"); }
    { ModelKeyFixture f; Constant(f); NativePoseTestBinding binding(*f.app, f.history);
      Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly, 128, 1) == MMS_OK, "unused history budget set below candidate");
      const auto before = f.first.Digest(); std::uint32_t count = 99; MmsResult result;
      Check(TryDeleteUnusedBoundKeys(f.app.get(), count, result) && result == MMS_E_MEMORY && !count && !f.history.UndoCount() && f.first.Digest() == before,
          "unused budget rejection is handled and cannot fall back to legacy deletion"); }
}
void Failures() {
    ModelKeyFixture f; Constant(f); const auto before = f.first.Digest(); const ModelKeyState state(f.first);
    std::shared_ptr<HistoryChange> change; bool planned = false;
    for (long limit = 0; limit < 2048; ++limit) {
        std::uint32_t count = 99; failAfter = limit;
        const auto result = PlanUnusedModelKeyDeletion(*f.app, 0, false, change, count); failAfter = -1;
        Check(f.first.Digest() == before && state.Matches(f.first) && !f.history.UndoCount(), "unused planning failure preserves all live bytes");
        if (result == MMS_OK) { Check(count == 3 && change, "unused plan sweep reaches complete result"); planned = true; break; }
        Check(result == MMS_E_MEMORY && !count && !change, "unused planning allocation failure has no partial result");
    }
    Check(planned, "unused planning fault sweep terminates");
    std::unique_ptr<SharedHistory::NativeTransaction> transaction; bool prepared = false;
    for (long limit = 0; limit < 2048; ++limit) {
        failAfter = limit; const auto result = f.history.PrepareNative("unused", change, transaction); failAfter = -1;
        Check(f.first.Digest() == before && state.Matches(f.first), "unused prepare failure preserves keys and pose");
        if (result == MMS_OK) { prepared = true; break; }
        Check(result == MMS_E_MEMORY && !transaction, "unused prepare exposes no partial transaction");
    }
    Check(prepared && CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
        "unused three-family publication allocates nothing");
    Check(f.history.History(false) == MMS_OK && state.Matches(f.first), "unused failure sweep still has exact undo");
}
}
void NativeUnusedKeyChecks() {
    unused_key_checks::RoundTrip(); unused_key_checks::Boundaries(); unused_key_checks::Failures();
}
}
