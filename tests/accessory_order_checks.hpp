// Included inside accessory_feedback; operates on the real dialog's private
// scratch list and cancels the dialog afterwards.
struct OrderRows {
    std::vector<std::wstring> text;
    std::vector<LRESULT> data;
    std::vector<mdl::AccessoryRecord*> owners;
    std::wstring edit;
    LRESULT selected = LB_ERR;
};
OrderRows ReadOrderRows(MMDApp* app, HWND dialog) {
    OrderRows rows;
    const auto list = GetDlgItem(dialog, panel::kOrderListBox);
    const auto count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    Check(count >= 0 && count <= 255, "order fixture has a bounded real list");
    auto** order = static_cast<mdl::AccessoryRecord**>(app->AccessoryOrderArray());
    for (LRESULT i = 0; i < count; ++i) {
        const auto length = SendMessageW(list, LB_GETTEXTLEN, i, 0);
        Check(length >= 0, "read actual order row length");
        std::wstring text(std::size_t(length) + 1, L'\0');
        Check(SendMessageW(list, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text.data())) == length,
            "read actual order row text");
        text.resize(std::size_t(length)); rows.text.push_back(std::move(text));
        rows.data.push_back(SendMessageW(list, LB_GETITEMDATA, i, 0));
        if (i < 3) rows.owners.push_back(order[i]);
    }
    wchar_t edit[256]{};
    GetDlgItemTextW(dialog, panel::kAccessoryNameEdit, edit, std::size(edit));
    rows.edit = edit; rows.selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    return rows;
}
void SameOrderRows(MMDApp* app, HWND dialog, const OrderRows& expected, const char* why) {
    const auto current = ReadOrderRows(app, dialog);
    Check(current.text == expected.text && current.data == expected.data && current.owners == expected.owners &&
        current.edit == expected.edit && current.selected == expected.selected, why);
}
struct OrderFailure { UINT message; bool pending = true; unsigned calls = 0; bool differentIndex = false; };
LRESULT CALLBACK FailOrderMessage(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& failure = *reinterpret_cast<OrderFailure*>(data);
    if (failure.pending && message == failure.message) {
        failure.pending = false; ++failure.calls;
        if (failure.differentIndex) {
            const auto count = SendMessageW(window, LB_GETCOUNT, 0, 0);
            return DefSubclassProc(window, message, static_cast<WPARAM>(count), lp);
        }
        return message == LB_INSERTSTRING ? LB_ERRSPACE : LB_ERR;
    }
    return DefSubclassProc(window, message, wp, lp);
}
POINT OrderPoint(HWND list, int row, bool lower = false) {
    RECT rect{};
    Check(SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) != LB_ERR,
        "read actual drag row geometry");
    POINT point{rect.left + 5, lower ? rect.bottom - 1 : rect.top + 1};
    Check(ClientToScreen(list, &point) != FALSE, "map drag row to native screen coordinates");
    return point;
}
void ProbeOrderMoves(MMDApp* app, HWND dialog) {
    const auto list = GetDlgItem(dialog, panel::kOrderListBox);
    Check(SendMessageW(list, LB_GETCOUNT, 0, 0) == 3 && app->AccessoryOrderArray(),
        "move boundaries use three actually loaded X accessories");
    auto** order = static_cast<mdl::AccessoryRecord**>(app->AccessoryOrderArray());
    for (int i = 0; i < 3; ++i)
        Check(SendMessageW(list, LB_SETITEMDATA, i, reinterpret_cast<LPARAM>(order[i])) != LB_ERR,
            "attach distinct identity markers to actual dialog rows");
    SendMessageW(list, LB_SETCURSEL, 1, 0);
    SetDlgItemTextW(dialog, panel::kAccessoryNameEdit, L"unchanged private edit");
    const auto before = ReadOrderRows(app, dialog);
    for (const auto& indices : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{3, 0},
        std::pair{0, 3}, std::pair{4, 0}, std::pair{0, 4}, std::pair{1, 1}}) {
        Check(!MoveAccessoryOrderItem(app, dialog, indices.first, indices.second), "invalid or unchanged move is refused");
        SameOrderRows(app, dialog, before, "invalid move preserves rows, identity, scratch, selection and name edit");
    }
    Check(!MoveAccessoryOrderItem(nullptr, dialog, 0, 1) && !MoveAccessoryOrderItem(app, nullptr, 0, 1),
        "missing app or dialog is refused before accessing scratch data");
    void* saved = app->AccessoryOrderArray(); app->AccessoryOrderArray() = nullptr;
    Check(!MoveAccessoryOrderItem(app, dialog, 0, 1), "missing order scratch is refused");
    app->AccessoryOrderArray() = saved;
    SameOrderRows(app, dialog, before, "invalid owners do not change actual list state");
    Check(SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"unexpected extra row")) == 3,
        "create a real list/scratch count mismatch");
    const auto extra = ReadOrderRows(app, dialog);
    Check(!MoveAccessoryOrderItem(app, dialog, 0, 3), "mismatched list count cannot index past the scratch array");
    SameOrderRows(app, dialog, extra, "count mismatch refuses without modifying rows or scratch");
    SendMessageW(list, LB_DELETESTRING, 3, 0);
    SameOrderRows(app, dialog, before, "removing the test-only extra row restores the exact fixture");
    for (auto message : {LB_INSERTSTRING, LB_SETITEMDATA, LB_DELETESTRING}) {
        OrderFailure failure{UINT(message)};
        Check(SetWindowSubclass(list, FailOrderMessage, 0x4d4d53a1, reinterpret_cast<DWORD_PTR>(&failure)) != FALSE,
            "install one-shot list write failure");
        Check(!MoveAccessoryOrderItem(app, dialog, 2, 0) && failure.calls == 1,
            "list allocation, item-data or deletion failure rejects the move");
        RemoveWindowSubclass(list, FailOrderMessage, 0x4d4d53a1);
        SameOrderRows(app, dialog, before, "failed list writes roll back text, item-data, scratch and selection");
    }
    SetDlgItemTextW(dialog,panel::kAccessoryNameEdit,L"atomic rename");
    const auto renameBefore=ReadOrderRows(app,dialog);
    for(auto message:{LB_INSERTSTRING,LB_SETITEMDATA,LB_DELETESTRING}) {
        OrderFailure failure{UINT(message)};
        Check(SetWindowSubclass(list,FailOrderMessage,0x4d4d53a1,reinterpret_cast<DWORD_PTR>(&failure))!=FALSE,
            "install rename list write failure");
        SendMessageW(dialog,WM_COMMAND,633,0);
        RemoveWindowSubclass(list,FailOrderMessage,0x4d4d53a1);
        Check(failure.calls==1,"rename exercises the requested list failure");
        SameOrderRows(app,dialog,renameBefore,"failed rename retains row text, item identity, scratch and typed edit");
    }
    SendMessageW(dialog,WM_COMMAND,633,0);
    const auto renamed=ReadOrderRows(app,dialog);
    Check(renamed.text[1]==L"atomic rename"&&renamed.data==before.data&&renamed.owners==before.owners,
        "successful rename preserves each item's independent identity");
    // Restore the private text and edit before the remaining hit-test checks.
    SendMessageW(list,LB_DELETESTRING,1,0);
    SendMessageW(list,LB_INSERTSTRING,1,reinterpret_cast<LPARAM>(before.text[1].c_str()));
    SendMessageW(list,LB_SETITEMDATA,1,before.data[1]);SendMessageW(list,LB_SETCURSEL,before.selected,0);
    SetDlgItemTextW(dialog,panel::kAccessoryNameEdit,before.edit.c_str());
    SameOrderRows(app,dialog,before,"restore private helper fixture after rename checks");
    const auto first = OrderPoint(list, 0), afterFirst = OrderPoint(list, 0, true), last = OrderPoint(list, 2, true);
    RECT bounds{}; GetWindowRect(list, &bounds);
    const POINT outside{bounds.right + 20, bounds.top + 5};
    Check(AccessoryOrderDropIndex(list, first, false) == 0 && AccessoryOrderDropIndex(list, afterFirst, false) == 1 &&
        AccessoryOrderDropIndex(list, last, false) == 3 && AccessoryOrderDropIndex(list, outside, false) == -1 &&
        AccessoryOrderDropIndex(nullptr, first, false) == -1, "drop hit testing accepts row halves and final insertion but refuses outside or absent lists");
    const auto dragMessage = RegisterWindowMessageW(L"commctrl_DragListMsg");
    DRAGLISTINFO drag{DL_DROPPED, list, first};
    SendMessageW(dialog, dragMessage, panel::kOrderListBox, reinterpret_cast<LPARAM>(&drag));
    SameOrderRows(app, dialog, before, "drop without a preceding drag cannot reorder objects");
    drag.uNotification = DL_BEGINDRAG; drag.ptCursor = outside;
    Check(SendMessageW(dialog, dragMessage, panel::kOrderListBox, reinterpret_cast<LPARAM>(&drag)) == FALSE,
        "drag cannot start outside an actual row");
    drag.hWnd = GetDlgItem(dialog, panel::kAccessoryNameEdit); drag.ptCursor = first;
    Check(SendMessageW(dialog, dragMessage, panel::kOrderListBox, reinterpret_cast<LPARAM>(&drag)) == FALSE,
        "drag notifications from another control are ignored");
    SameOrderRows(app, dialog, before, "invalid drag starts preserve all scratch list state");
    // Duplicate captions must keep their per-row identities through both move
    // directions. This list is private, and this dialog will be cancelled.
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 3; ++i) {
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"same caption"));
        SendMessageW(list, LB_SETITEMDATA, i, reinterpret_cast<LPARAM>(order[i]));
    }
    Check(MoveAccessoryOrderItem(app, dialog, 2, 0), "move a duplicate caption upward");
    const auto moved = ReadOrderRows(app, dialog);
    Check(moved.owners == std::vector<mdl::AccessoryRecord*>{before.owners[2], before.owners[0], before.owners[1]} &&
        moved.data == std::vector<LRESULT>{before.data[2], before.data[0], before.data[1]} && moved.selected == 0 &&
        moved.edit == L"same caption", "duplicate captions retain exact scratch and item-data identity");
    Check(MoveAccessoryOrderItem(app, dialog, 0, 2), "move a duplicate caption downward after the final row");
    const auto restored = ReadOrderRows(app, dialog);
    Check(restored.owners == before.owners && restored.data == before.data && restored.selected == 2,
        "reverse duplicate-caption move restores all original identities");
    // Simulate an incompatible subclass inserting at an unexpected position.
    // Native LB_INSERTSTRING itself honors its explicit index even in LBS_SORT.
    OrderFailure misplaced{LB_INSERTSTRING}; misplaced.differentIndex = true;
    Check(SetWindowSubclass(list, FailOrderMessage, 0x4d4d53a1, reinterpret_cast<DWORD_PTR>(&misplaced)) != FALSE,
        "install a one-shot custom insertion-position mismatch");
    Check(!MoveAccessoryOrderItem(app, dialog, 2, 0) && misplaced.calls == 1,
        "an actual insertion at an unexpected index refuses the move");
    RemoveWindowSubclass(list, FailOrderMessage, 0x4d4d53a1);
    SameOrderRows(app, dialog, restored, "unexpected-position refusal removes the inserted row and preserves all identities");
    for (int slot = 0; slot < 3; ++slot)
        Check(app->AccessorySlot(slot)->order == slot, "private helper probes never publish order into live accessories");
    std::puts("PASS accessory order helper boundaries, rollback, drag hit testing and duplicate item identities");
}

#if !MMD_ORIGINAL_PROFILE
struct OrderGuardChange final : plugins::HistoryChange {
    int& value;
    explicit OrderGuardChange(int& target) : value(target) {}
    struct Candidate final : plugins::PreparedHistoryChange {
        int& value; int before, after;
        Candidate(int& v, int source, int target) : value(v), before(source), after(target) {}
        bool IsCurrent() const noexcept override { return value == before; }
        void Publish() noexcept override { value = after; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        out = std::make_unique<Candidate>(value, forward ? 0 : 1, forward ? 1 : 0); return MMS_OK;
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this); }
};
void CheckBoundOrderRefusal(MMDApp* app) {
    plugins::StorageStore storage; int marker = 0; plugins::SharedHistory history(storage);
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(history.PrepareNative("order guard retained redo", std::make_shared<OrderGuardChange>(marker), transaction) == MMS_OK &&
        history.CommitNative(std::move(transaction)) == MMS_OK && history.History(false) == MMS_OK && marker == 0 &&
        history.UndoCount() == 0 && history.RedoCount() == 1, "seed a real shared-history redo entry before rejected ordering");
    struct State {
        std::array<mdl::AccessoryRecord, 3> accessories;
        std::array<mdl::AccessoryRecord*, 3> owners;
        void* scratch;
        int split, frame;
        unsigned char dirty, selected;
        std::int32_t enter;
        std::array<std::int32_t, 18> flags;
    };
    const auto read = [&] {
        State state{};
        for (int i = 0; i < 3; ++i) { state.owners[i] = app->AccessorySlot(i); state.accessories[i] = *state.owners[i]; }
        state.scratch = app->AccessoryOrderArray(); state.split = app->AccessoryRenderSplitOrder();
        state.frame = app->CurrentFrame(); state.dirty = app->SceneModified(); state.selected = app->SelectedAccessorySlot();
        state.enter = app->state.enterKeyState;
        std::copy_n(app->state.dialogFlags, state.flags.size(), state.flags.begin());
        return state;
    };
    const auto oldDialogFlag = app->state.dialogFlags[8], oldEnter = app->state.enterKeyState;
    app->state.dialogFlags[8] = 0; app->state.enterKeyState = 0;
    Check(!app->AccessoryOrderArray(), "bound rejection starts without an order scratch allocation");
    const auto before = read();
    std::uint64_t revision = 0; Check(history.Revision(revision) == MMS_OK, "read rejection history revision");
    const auto same = [&] {
        const auto current = read();
        Check(current.owners == before.owners && current.scratch == before.scratch && current.split == before.split &&
            current.frame == before.frame && current.dirty == before.dirty && current.selected == before.selected &&
            current.enter == before.enter && current.flags == before.flags &&
            std::memcmp(current.accessories.data(), before.accessories.data(), sizeof current.accessories) == 0,
            "bound order rejection preserves scratch, live object bytes, flags, selection, frame and dirty state");
        std::uint64_t after = 0;
        Check(history.Revision(after) == MMS_OK && after == revision && history.UndoCount() == 0 &&
            history.RedoCount() == 1 && marker == 0, "rejected order command preserves the history revision and redo entry");
    };
    {
        plugins::NativePoseTestBinding binding(*app, history);
        const int previousDialogs = dialogs;
        action = DialogAction::Settings; split = moveFrom = moveTo = -1; cancelSettings = true;
        const auto playback = app->PlaybackActive(); app->PlaybackActive() = 1;
        SendMessageW(app->MainWindow(), WM_COMMAND, 249, 0); same();
        CommandDispatch(nullptr, 249); same();
        CmdFileMenu(app, app->MainWindow(), 249, 0); same();
        app->PlaybackActive() = playback;
        Check(dialogs == previousDialogs, "bound ordering routes create no dialog during playback");
        SendMessageW(app->MainWindow(), WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(GetMenu(app->MainWindow())), 0);
        const auto menuState = GetMenuState(GetMenu(app->MainWindow()), 249, MF_BYCOMMAND);
        Check(menuState != UINT(-1) && !(menuState & (MF_GRAYED | MF_DISABLED)),
            "opening a menu makes migrated ordering available while shared history is bound");
        // Test the second independent refresh entry without relying on menu-open.
        EnableMenuItem(GetMenu(app->MainWindow()), 249, MF_BYCOMMAND | MF_GRAYED);
        Check(plugins::RefreshBoundObjectUi(*app, history), "refresh bound accessory object controls");
        const auto refreshed = GetMenuState(GetMenu(app->MainWindow()), 249, MF_BYCOMMAND);
        Check(refreshed != UINT(-1) && !(refreshed & (MF_GRAYED | MF_DISABLED)),
            "common-history object refresh independently enables migrated ordering without a topology change");
    }
    Check(plugins::RefreshBoundObjectUi(*app, history) &&
        !(GetMenuState(GetMenu(app->MainWindow()), 249, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)),
        "unbinding shared history re-enables legacy ordering through the same refresh");
    EnableMenuItem(GetMenu(app->MainWindow()), 249, MF_BYCOMMAND | MF_GRAYED);
    SendMessageW(app->MainWindow(), WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(GetMenu(app->MainWindow())), 0);
    Check(!(GetMenuState(GetMenu(app->MainWindow()), 249, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)),
        "opening a menu after unbinding independently re-enables legacy ordering");
    std::uint64_t after = 0;
    Check(history.Revision(after) == MMS_OK && after == revision && history.RedoCount() == 1 &&
        history.History(true) == MMS_OK && marker == 1, "the unrelated redo entry remains executable after rejected order commands");
    Choose(app, 1);
    app->state.dialogFlags[8] = oldDialogFlag; app->state.enterKeyState = oldEnter;
    std::puts("PASS accessory ordering rejects playback and both bound and legacy menus remain available");
}
#endif
