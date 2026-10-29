// Integration checks for editor feedback: use the real windows, camera and PMM paths.
void CheckEditorFeedback(MMDApp* app, const wchar_t* mode, const std::filesystem::path& output) {
    auto* raw = app->SelectedModel();
    Check(raw != nullptr, "feedback fixture has a model");
    auto& model = *mdl::Mdl(raw);
    const HWND main = app->MainWindow();
    const auto choose = [&](int selection) {
        HWND combo = GetDlgItem(main, panel::kMainComboModel);
        SendMessageW(combo, CB_SETCURSEL, selection, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(panel::kMainComboModel, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
    };
    if(std::wcscmp(mode,L"negative")==0){
        choose(0);app->SidebarWidth()=600;app->PlaybackActive()=0;
        negative_frames::RegisterGlobal(app,negative_frames::Global::Camera,-10);
        negative_frames::RegisterGlobal(app,negative_frames::Global::Light,-10);
        app->CurrentFrame()=0;app->state.timelineStartFrame=-20;PanelPaint(app);
        const auto click=[&](int x,int y){app->MouseX()=x;app->MouseY()=y;HandleLButtonDown(app);};
        click(230,167);
        Check(app->CurrentFrame()==0&&app->state.timelineStartFrame==-20&&app->SelectionBoxDragging(),"negative cell begins selection without seeking or recentering");
        HandleLButtonUp(app);SendMessageW(main,WM_COMMAND,416,0);
        Check(negative_frames::Command(app,420),"vertical selection recognizes a negative cell");
        app->CurrentFrame()=-30;negative_frames::Command(app,421);
        Check(negative_frames::Count(app)==4,"vertical selection copies camera and light negative keys together");
        app->CurrentFrame()=0;app->state.timelineStartFrame=-20;PanelPaint(app);
        click(230,152);Check(app->CurrentFrame()==-10&&app->state.timelineStartFrame==-20,"negative ruler click seeks without shifting visible range");HandleLButtonUp(app);
        app->CurrentFrame()=0;click(211,162);HandleMouseMove(237,188);HandleLButtonUp(app);
        Check(app->state.timelineStartFrame==-20,"negative marquee keeps timeline scroll fixed");
        Check(negative_frames::Command(app,420),"negative marquee selects keys");app->CurrentFrame()=-50;negative_frames::Command(app,421);
        Check(negative_frames::Count(app)==6,"drag box selects both negative global tracks");
        std::puts("PASS negative cell/ruler clicks, stable scroll, vertical selection, and real mouse marquee");return;
    }
    if (std::wcscmp(mode, L"track") == 0) {
        const int modelCombo = model.comboSelIndex;
        choose(0);
        app->CameraParentModel() = -1;
        for (int frame : {0, 30}) {
            app->CameraPositionX() = 7.f + frame;
            app->CameraPositionY() = 13.f;
            app->CameraPositionZ() = -8.f;
            app->CameraPitch() = .2f; app->CameraYaw() = .3f; app->CameraRoll() = -.1f;
            app->CameraDistance() = -45.f - frame;
            RegisterCameraState(app, frame);
        }
        app->state.followCameraEnabled = 1;
        const auto capture = [&]() {
            SetupFrameWorldTransform(app);
            std::array<D3DMATRIX, 2> matrices{};
            app->Renderer()->device->GetTransform(D3DTS_WORLD, &matrices[0]);
            app->Renderer()->device->GetTransform(D3DTS_VIEW, &matrices[1]);
            return matrices;
        };
        for (int frame : {0, 15, 30}) {
            choose(0); app->CurrentFrame() = frame; ReloadModels(app); ApplyCurrentCameraView(app);
            const auto expected = capture();
            const auto same = [&]() {
                const auto actual = capture();
                for (int m = 0; m < 2; ++m) for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
                    Check(std::fabs(actual[m].m[r][c] - expected[m].m[r][c]) < 1e-5f,
                          "Track keeps the recorded camera transform during refresh and model editing");
            };
            choose(modelCombo); same();
            for (int repeat = 0; repeat < 3; ++repeat) { PostModelReload(app); same(); }
            app->state.modelReloadPending = 1;
            app->DeltaTime() = 1.f / 30.f; app->state.messageSeen = 10;
            FrameDriver(app); same();
            choose(0); same();
        }
        std::puts("PASS Track model/camera switches, repeated refresh and rendered-frame transforms");
    } else if (std::wcscmp(mode, L"focus") == 0) {
        HWND combo = GetDlgItem(main, panel::kMainComboModel);
        for (int selection : {int(model.comboSelIndex), 0}) {
            SetFocus(combo);
            Check(GetFocus() == combo, "focus actual model selector");
            choose(selection);
            SendMessageW(main, WM_COMMAND, MAKEWPARAM(panel::kMainComboModel, CBN_CLOSEUP),
                         reinterpret_cast<LPARAM>(combo));
            Check(GetFocus() == main, "closing model selector returns focus to frame navigation");
            const int before = app->CurrentFrame();
            app->state.rightKeyState = 1; ConsumeArrowKeyNavigation(app); app->state.rightKeyState = 0;
            Check(app->CurrentFrame() == before + 1, "Right advances frame after model selection");
            app->state.leftKeyState = 1; ConsumeArrowKeyNavigation(app); app->state.leftKeyState = 0;
            Check(app->CurrentFrame() == before && SendMessageW(combo, CB_GETCURSEL, 0, 0) == selection,
                  "Left returns frame without changing the selected model or camera");
        }
        std::puts("PASS model selector focus and immediate Left/Right frame navigation");
    } else if (std::wcscmp(mode, L"groups") == 0) {
        Check(model.groupCount > 1, "fixture has multiple display groups");
        for (int pattern : {0, 1, 2}) {
            auto* current = app->SelectedModel();
            auto* groups = mdl::DisplayGroups(current);
            const int count = mdl::Mdl(current)->groupCount;
            std::vector<unsigned char> expected;
            for (int i = 0; i < count; ++i) {
                groups[i].flags = pattern == 2 ? i % 2 : pattern;
                expected.push_back(groups[i].flags);
            }
            const auto path = output / (L"groups-" + std::to_wstring(pattern) + L".pmm");
            Check(SaveSceneCopy(app, path.c_str()), "save display group expansion state");
            ResetAppState(app); wcscpy_s(app->EnvFileName(), 256, path.c_str()); LoadSceneFile();
            current = app->SelectedModel();
            Check(current && mdl::Mdl(current)->groupCount == count, "reopen same display groups");
            groups = mdl::DisplayGroups(current);
            for (int i = 0; i < count; ++i)
                Check(groups[i].flags == expected[i], "PMM preserves each collapsed or expanded group");
        }
        std::puts("PASS collapsed, expanded and mixed display groups survive PMM save/reopen");
    } else {
        choose(model.comboSelIndex);
        Check(model.boneCount > 1, "fixture has two bones for deletion isolation");
        std::fill(model.boneSelection, model.boneSelection + model.boneCount, 0);
        model.boneSelection[0] = model.boneSelection[1] = 1;
        RegisterSelectedBoneKeys(raw, 120, 0);
        const bool box = std::wcscmp(mode, L"box") == 0;
        if (box) {
            model.boneSelection[0] = 0;
            RegisterSelectedBoneKeys(raw, 121, 0);
        }
        const auto at = [&](int bone, int frame) {
            auto* keys = model.boneKeys;
            int index = bone;
            while (keys[index].frame < static_cast<unsigned>(frame) && keys[index].next) index = int(keys[index].next);
            Check(keys[index].frame == static_cast<unsigned>(frame), "find fixture key at the requested frame");
            return index;
        };
        const int old = at(0, 120), current = at(1, box ? 121 : 120);
        for (std::size_t i = 0; i < mdl::BoneKeyCapacity(model); ++i) model.boneKeys[i].allocated = 0;
        for (std::size_t i = 0; i < mdl::MorphKeyCapacity(model.morphKeys); ++i) model.morphKeys[i].allocated = 0;
        for (std::size_t i = 0; i < mdl::KeyPoolCapacity(model.displayKeys); ++i) model.displayKeys[i].allocated = 0;
        model.boneSelection[0] = 1; model.boneSelection[1] = 0; model.selectedBone = 0;
        app->CurrentFrame() = 120;
        if (box) {
            // Old key has scrolled out of view. A new unshifted marquee must clear it.
            model.boneKeys[old].allocated = 1;
            app->state.timelineStartFrame = 121;
            for (int i = 0; i < model.groupCount; ++i) mdl::DisplayGroups(raw)[i].flags = 1;
            model.boneListPos = 0;
            PostLanguageSweep(app);
            int hit = 0;
            while (hit < 40000 && app->state.rowHitBone[hit] != current) ++hit;
            Check(hit < 40000, "new key has a real painted timeline hit cell");
            const int x = 100 + 13 * (hit / 200), y = 167 + 14 * (hit % 200);
            app->SelectionBoxDragging() = 1; app->TimelineSelectionChanged() = 0;
            app->SelectionBoxAnchorX() = x - 11; app->SelectionBoxAnchorY() = y - 151;
            app->state.shiftModifierState = 0;
            HandleMouseMove(x + 5, y + 6);
            app->SelectionBoxDragging() = 0;
            Check(!model.boneKeys[old].allocated && model.boneKeys[current].allocated,
                  "new marquee replaces offscreen previous key selection");
            // Shift still adds to the selection.
            model.boneKeys[old].allocated = 1;
            app->state.shiftModifierState = 3; app->SelectionBoxDragging() = 1;
            HandleMouseMove(x + 6, y + 6);
            app->SelectionBoxDragging() = 0; app->state.shiftModifierState = 0;
            Check(model.boneKeys[old].allocated && model.boneKeys[current].allocated,
                  "Shift marquee retains the previous selection");
            std::puts("PASS replacement and additive marquee selection across offscreen keys");
            return;
        }
        model.boneKeys[current].allocated = 1;
        SetFocus(main); app->state.deleteKeyState = 1;
        PumpDeleteRebuild(app, main, true); app->state.deleteKeyState = 0;
        Check(model.boneKeys[current].frame == 0 && model.boneKeys[old].frame == 120,
              "Delete removes only explicit timeline keys, preserving previously selected bone keys");
        std::int32_t frame = 120; UndoModelEdit(raw, &frame);
        Check(model.boneKeys[current].frame == 120 && model.boneKeys[old].frame == 120,
              "undo restores only the intended deleted key");
        RedoModelEdit(raw, &frame);
        Check(model.boneKeys[current].frame == 0 && model.boneKeys[old].frame == 120,
              "redo still preserves the other bone");
        Check(model.morphCount > 0, "fixture has a morph for cross-channel deletion");
        for (std::size_t i = 0; i < mdl::BoneKeyCapacity(model); ++i) model.boneKeys[i].allocated = 0;
        model.morphKeys[0].value = .75f; model.morphKeys[0].allocated = 1;
        app->state.deleteKeyState = 1; PumpDeleteRebuild(app, main, true); app->state.deleteKeyState = 0;
        Check(model.morphKeys[0].value == 0.f && model.boneKeys[old].frame == 120,
              "an explicit morph selection never deletes the previously selected bone");
        // With no explicit timeline selection, viewport-only Delete keeps working.
        for (std::size_t i = 0; i < mdl::BoneKeyCapacity(model); ++i) model.boneKeys[i].allocated = 0;
        app->state.deleteKeyState = 1; PumpDeleteRebuild(app, main, true); app->state.deleteKeyState = 0;
        Check(model.boneKeys[old].frame == 0, "viewport-only Delete still removes selected bone's current key");
        std::puts("PASS bone/morph deletion isolation, undo/redo and viewport-only Delete");
    }
}
