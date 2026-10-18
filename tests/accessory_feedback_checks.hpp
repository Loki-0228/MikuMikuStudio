// MAINTENANCE: regression-keep; real accessory imports, dialog commands and PMM round trips.
namespace accessory_feedback {
enum class DialogAction { Settings, Delete, CancelDelete };
enum class MoveAction { Buttons, Drag, CancelDrag, OutsideDrag };
DialogAction action = DialogAction::Settings;
MoveAction moveAction = MoveAction::Buttons;
int split = -1, moveFrom = -1, moveTo = -1, dialogs = 0, initialSplit = -1;
bool cancelSettings = false;

LRESULT CALLBACK HideDialog(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_WINDOWPOSCHANGING) {
        auto* position = reinterpret_cast<WINDOWPOS*>(lp);
        position->flags &= ~SWP_SHOWWINDOW;
        position->flags |= SWP_NOACTIVATE;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, HideDialog, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
LRESULT CALLBACK InspectDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* created = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(created->style & WS_CHILD)) {
            created->style &= ~WS_VISIBLE;
            SetWindowSubclass(reinterpret_cast<HWND>(wp), HideDialog, 1, 0);
        }
    }
    if (code == HCBT_ACTIVATE) {
        const HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t cls[32]{};
        GetClassNameW(dialog, cls, 32);
        if (std::wcscmp(cls, L"#32770") == 0) {
            if (GetPropW(dialog, L"MMS.AccessoryFeedback.Inspected")) return 1;
            SetPropW(dialog, L"MMS.AccessoryFeedback.Inspected", reinterpret_cast<HANDLE>(1));
            ++dialogs;
            if (action == DialogAction::Settings) {
                Check(GetDlgItem(dialog, panel::kAccessoryOrderEdit) != nullptr,
                      "open the actual Accessory Edit dialog");
                initialSplit = GetDlgItemInt(dialog, panel::kAccessoryOrderEdit, nullptr, TRUE);
                if (split >= 0) SetDlgItemInt(dialog, panel::kAccessoryOrderEdit, split, FALSE);
                if (moveFrom >= 0) {
                    const HWND list=GetDlgItem(dialog,panel::kOrderListBox);
                    SendMessageW(list,LB_SETCURSEL,moveFrom,0);
                    if(moveAction==MoveAction::Buttons) {
                        for (int i = moveFrom; i != moveTo; i += moveFrom < moveTo ? 1 : -1)
                            SendMessageW(dialog, WM_COMMAND, moveFrom < moveTo ? 0x277 : 0x276, 0);
                    } else {
                        const UINT message=RegisterWindowMessageW(L"commctrl_DragListMsg");
                        const auto point=[&](int row,bool after) {
                            RECT rect{};Check(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&rect))!=LB_ERR,"get drag item rectangle");
                            POINT pt{rect.left+8,after?rect.bottom-1:rect.top+1};ClientToScreen(list,&pt);return pt;
                        };
                        DRAGLISTINFO info{DL_BEGINDRAG,list,point(moveFrom,false)};
                        Check(SendMessageW(dialog,message,panel::kOrderListBox,reinterpret_cast<LPARAM>(&info))==TRUE,"accessory list accepts native drag begin");
                        const auto count=SendMessageW(list,LB_GETCOUNT,0,0);
                        const auto insertion=moveTo>moveFrom?moveTo+1:moveTo;
                        info.ptCursor=point(insertion<count?insertion:static_cast<int>(count)-1,insertion==count);
                        if(moveAction==MoveAction::OutsideDrag) {
                            RECT rect{};GetWindowRect(list,&rect);info.ptCursor={rect.right+40,rect.top+20};
                        }
                        info.uNotification=DL_DRAGGING;
                        Check(SendMessageW(dialog,message,panel::kOrderListBox,reinterpret_cast<LPARAM>(&info))==
                              (moveAction==MoveAction::OutsideDrag?DL_STOPCURSOR:DL_MOVECURSOR),"drag feedback matches valid or outside drop target");
                        info.uNotification=moveAction==MoveAction::CancelDrag?DL_CANCELDRAG:DL_DROPPED;
                        SendMessageW(dialog,message,panel::kOrderListBox,reinterpret_cast<LPARAM>(&info));
                    }
                }
                PostMessageW(dialog, WM_COMMAND, cancelSettings ? IDCANCEL : 0x278, 0);
            } else {
                Check(GetDlgItem(dialog, IDOK) && GetDlgItem(dialog, IDCANCEL), "accessory delete confirmation");
                PostMessageW(dialog, WM_COMMAND, action == DialogAction::Delete ? IDOK : IDCANCEL, 0);
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void Settings(MMDApp* app, int value = -1, int from = -1, int to = -1, bool cancel = false,
              MoveAction method = MoveAction::Buttons) {
    action = DialogAction::Settings; split = value; moveFrom = from; moveTo = to; cancelSettings = cancel;
    moveAction=method;
    const int before = dialogs;
    CmdFileMenu(app, app->MainWindow(), 249, 0);
    Check(dialogs == before + 1, "exercise one production accessory order dialog");
}
void Choose(MMDApp* app, int slot) {
    const HWND combo = GetDlgItem(app->MainWindow(), panel::kAccessoryCombo);
    SendMessageW(combo, CB_SETCURSEL, app->AccessorySlot(slot)->order, 0);
    SendMessageW(app->MainWindow(), WM_COMMAND, MAKEWPARAM(panel::kAccessoryCombo, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(combo));
    Check(app->SelectedAccessorySlot() == slot, "select the fixture through the accessory combo");
}
void Remove(MMDApp* app, bool cancel = false) {
    action = cancel ? DialogAction::CancelDelete : DialogAction::Delete;
    const int before = dialogs;
    CmdControl450(app, app->MainWindow(), 473, 0);
    Check(dialogs == before + 1, "exercise one production delete confirmation");
}
void Selected(MMDApp* app, int slot, int order) {
    Check(app->SelectedAccessorySlot() == slot && app->AccessorySlot(slot)->order == order,
          "accessory manipulation keeps the expected object identity");
    Check(SendDlgItemMessageW(app->MainWindow(), panel::kAccessoryCombo, CB_GETCURSEL, 0, 0) == order,
          "accessory combo follows the selected object's displayed order");
    Check(app->AccessorySlot(slot)->rowSelected != 0, "timeline highlights the same accessory");
    wchar_t text[64]{};
    GetDlgItemTextW(app->MainWindow(), 478, text, 64);
    Check(std::fabs(std::wcstod(text, nullptr) - app->AccessorySlot(slot)->position[0]) < .0001,
          "accessory position controls show the selected object's values");
}
} // namespace accessory_feedback

void CheckAccessoryFeedback(MMDApp* app, const std::filesystem::path& output, const wchar_t* mode) {
    using namespace accessory_feedback;
    std::filesystem::create_directories(output);
    std::array<std::filesystem::path, 5> paths;
    for (int i = 0; i < 5; ++i) {
        paths[i] = output / (L"accessory-" + std::to_wstring(i) + L".x");
        std::ofstream(paths[i]) << "xof 0303txt 0032\nMesh tri {3;-5;5;0;,0;15;0;,5;5;0;;1;3;0,1,2;;MeshMaterialList {1;1;0;;Material {0.8;0.3;0.1;1.0;;1.0;0;0;0;;0;0;0;;}}}";
    }
    HHOOK hook = SetWindowsHookExW(WH_CBT, InspectDialog, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "install hidden accessory dialog driver");
    const auto load = [&](int fixture) {
        action = DialogAction::Settings; // A loader error is an unexpected dialog.
        LoadAccessoryFile(paths[fixture].c_str());
    };
    const auto seed = [&]() {
        ResetAppState(app);
        for (int i = 0; i < 3; ++i) {
            load(i);
            Check(app->AccessorySlot(i) && app->AccessorySlot(i)->order == i, "load three actual X accessories");
            app->AccessorySlot(i)->position[0] = 11.f * (i + 1);
            RegisterAccessoryKey(app, 0, i);
        }
    };
    const auto roundTrip = [&](const std::wstring& name) {
        const auto file = output / (name + L".pmm");
        Check(SaveSceneCopy(app, file.c_str()), "save accessory order project");
        action = DialogAction::Settings;
        ResetAppState(app);
        wcscpy_s(app->EnvFileName(), 256, file.c_str()); LoadSceneFile();
    };
    for (int language : {0, 1, 2}) {
        app->EnglishUI() = language; LocalizeUI(app);
        if (std::wcscmp(mode, L"split") == 0 || std::wcscmp(mode, L"all") == 0) {
            for (int boundary : {1, 0, 3}) {
                seed(); Settings(app, boundary);
                load(3); load(4);
                Check(app->AccessoryRenderSplitOrder() == boundary, "imports preserve the user-entered draw-order split, including zero and the old maximum");
                Settings(app);
                Check(initialSplit == boundary, "reopened Accessory Edit shows the unchanged split");
                roundTrip(L"split-" + std::to_wstring(language) + L"-" + std::to_wstring(boundary));
                Check(app->AccessoryRenderSplitOrder() == boundary, "PMM preserves the split after adding accessories");
            }
        }
        if (std::wcscmp(mode, L"selection") == 0 || std::wcscmp(mode, L"all") == 0) {
            seed(); Choose(app, 1);
            Settings(app); Selected(app, 1, 1);
            const HWND scope = GetDlgItem(app->MainWindow(), panel::kRegisterScopeCombo);
            SendMessageW(scope, CB_SETCURSEL, 6, 0); // Accessory C, independent of manipulation selection B.
            Settings(app, 1, 2, 0); Selected(app, 1, 2);
            Check(SendMessageW(scope, CB_GETCURSEL, 0, 0) == 4, "registration scope follows C after reordering");
            Settings(app, 0, 2, 0, true); Selected(app, 1, 2);
            Check(app->AccessoryRenderSplitOrder() == 1 && app->AccessorySlot(2)->order == 0,
                  "cancel preserves order, split and manipulation selection");
            for (int globalScope = 0; globalScope < 4; ++globalScope) {
                SendMessageW(scope, CB_SETCURSEL, globalScope, 0); Settings(app); Selected(app, 1, 2);
                Check(SendMessageW(scope, CB_GETCURSEL, 0, 0) == globalScope, "OK preserves each global registration scope");
            }
            roundTrip(L"selection-" + std::to_wstring(language));
            Check(app->AccessorySlot(2)->order == 0 && app->AccessorySlot(1)->order == 2,
                  "PMM retains reordered accessories");
        }
        if (std::wcscmp(mode, L"drag") == 0 || std::wcscmp(mode, L"all") == 0) {
            seed();Choose(app,1);
            const HWND scope=GetDlgItem(app->MainWindow(),panel::kRegisterScopeCombo);
            SendMessageW(scope,CB_SETCURSEL,6,0);
            Settings(app,1,2,0,false,MoveAction::Drag);Selected(app,1,2);
            Check(app->AccessorySlot(2)->order==0&&app->AccessorySlot(0)->order==1&&
                  SendMessageW(scope,CB_GETCURSEL,0,0)==4,"drag upward moves the correct object and registration scope");
            Settings(app,-1,0,2,false,MoveAction::Drag);Selected(app,1,1);
            Check(app->AccessorySlot(2)->order==2,"drag downward can place an item after the last row");
            Settings(app,-1,1,1,false,MoveAction::Drag);Selected(app,1,1);
            for(auto method:{MoveAction::CancelDrag,MoveAction::OutsideDrag}) {
                Settings(app,-1,2,0,false,method);Selected(app,1,1);
                Check(app->AccessorySlot(2)->order==2,"cancelled or outside drag preserves the list");
            }
            app->SceneModified()=0;
            Settings(app,0,2,0,true,MoveAction::Drag);Selected(app,1,1);
            Check(app->AccessorySlot(2)->order==2&&app->AccessoryRenderSplitOrder()==1&&app->SceneModified()==0,
                  "dialog cancellation discards dragged order, split and dirty state");
            // Identical captions must retain distinct accessory identities.
            ResetAppState(app);load(0);load(0);load(0);Choose(app,1);
            Settings(app,1,2,0,false,MoveAction::Drag);Selected(app,1,2);
            Check(app->AccessorySlot(2)->order==0&&app->AccessorySlot(0)->order==1,"duplicate accessory names do not confuse dragging");
            roundTrip(L"drag-"+std::to_wstring(language));
            Check(app->AccessorySlot(2)->order==0&&app->AccessorySlot(1)->order==2&&app->AccessoryRenderSplitOrder()==1,
                  "PMM retains dragged order and model draw split");
        }
        if (std::wcscmp(mode, L"delete") == 0 || std::wcscmp(mode, L"all") == 0) {
            seed(); Settings(app, 2, 0, 2); // B, C, A: last entry is slot zero, not the highest slot.
            Choose(app, 1); Remove(app, true); Selected(app, 1, 0);
            Remove(app); Selected(app, 0, 1);
            Check(!app->AccessorySlot(1) && app->AccessoryRenderSplitOrder() == 1,
                  "delete compacts list and adjusts a split before the removed entry");
            load(3);
            Check(app->AccessorySlot(1) && app->AccessorySlot(1)->order == 2 && app->AccessoryRenderSplitOrder() == 1,
                  "new import reuses a free slot without resetting the split");
            Choose(app, 1); Remove(app); Selected(app, 0, 1);
            Remove(app); Selected(app, 2, 0);
            Remove(app);
            Check(SendDlgItemMessageW(app->MainWindow(), panel::kAccessoryCombo, CB_GETCOUNT, 0, 0) == 0 &&
                  SendDlgItemMessageW(app->MainWindow(), panel::kAccessoryCombo, CB_GETCURSEL, 0, 0) == CB_ERR,
                  "deleting the final accessory leaves an empty unselected combo");
            Check(app->AccessoryRenderSplitOrder() == 0, "empty accessory list has no pre-model accessories");
            load(0); Choose(app, 0); Selected(app, 0, 0);
            Check(app->AccessoryRenderSplitOrder() == 0, "reload after deleting all preserves zero split");
        }
        std::printf("PASS accessory feedback language=%d mode=%ls\n", language, mode);
    }
    UnhookWindowsHookEx(hook);
}
