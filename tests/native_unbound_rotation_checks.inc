// MAINTENANCE: regression-keep. Ordinary enhanced rotation reset, old native
// ring, actual hidden EDIT focus, no common-history binding or GPU device.
namespace {
void UnboundRotationInstallation() {
    wchar_t module[32768]{}; const auto n = GetModuleFileNameW(nullptr, module, DWORD(std::size(module)));
    Check(n && n < std::size(module), "locate unbound rotation test installation");
    const auto root = std::filesystem::path(module).parent_path();
    std::ifstream marker(root / L"unbound-rotation-test-installation.txt", std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
    Check(content == "MMS unbound rotation test installation v1\n", "unbound rotation requires its exact independent-installation marker");
    const auto plugins = root / L"PLUGINS";
    Check(!std::filesystem::exists(plugins) || (std::filesystem::is_directory(plugins) && std::filesystem::is_empty(plugins)),
        "unbound rotation runtime cannot load unrelated plugins from this installation");
}
void UnboundRotationSeed(PoseCommandFixture& f, bool scale = true) {
    f.app->CurrentFrame() = 37; f.first.m->selectedBone = 1;
    f.first.selected[0] = 0; f.first.selected[1] = 1; PoseAxisDegrees(*f.app);
    f.first.bones[1].rotQuat[2] = .6f; f.first.bones[1].rotQuat[3] = .8f;
    f.first.bones[1].trans[0] = 7.125f; f.first.bones[0].trans[1] = -9.5f;
    for (int i = 0; i < 7; ++i) f.first.bones[1].ikBackup[i] = float(21 + i);
    if (scale) Check(bone_scale::SetPose(f.first.raw(), 1, Scale(2)), "guard unbound rotation against full readout refresh");
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
}
void UnboundRotationDrafts(RotationDraftPanel& panel, int axis) {
    panel.Draft(axis, "88.75");
    for (int i = 0; i < 3; ++i) if (i != axis) RotationDraftPanel::Pending(panel.Field(i), "other rotation");
    panel.Unrelated();
}
bool UnboundRotationDraftsIntact(RotationDraftPanel& panel, int axis) {
    for (int i = 0; i < 3; ++i)
        if (!RotationDraftPanel::Text(panel.Field(i), i == axis ? "88.75" : "other rotation") ||
            !RotationDraftPanel::Modified(panel.Field(i))) return false;
    return panel.UnrelatedPreserved();
}
void UnboundRotationLoseFocus(RotationDraftPanel& panel, int axis) {
    Check(GetFocus() == panel.Field(axis), "unbound rotation target retains real focus before losing it");
    panel.LoseFocus(axis);
    Check(GetFocus() == panel.base.owner && GetFocus() != panel.Field(axis), "unbound rotation focus actually reaches its owned panel");
}
std::array<float, 4> UnboundRotationExpected(PoseCommandFixture& f, bool menu, int axis, bool floating) {
    return menu ? std::array<float, 4>{0, 0, 0, 1} : FrozenLegacyRotationTarget(PoseAxisReadDegrees(*f.app), axis, floating);
}
void UnboundRotationCaptions(PoseCommandFixture& f, RotationDraftPanel& panel, bool menu, int axis,
                             const std::array<float, 4>& expected) {
    const auto degrees = RotationDraftQuaternionDegrees(expected.data());
    Check(PoseAxisReadDegrees(*f.app) == degrees, "unbound rotation publishes degree caches from the independent old readout oracle");
    for (int i = 0; i < 3; ++i) {
        if (menu || i == axis) RotationDraftCaption(panel, i, degrees[i]);
        else Check(RotationDraftPanel::Text(panel.Field(i), "other rotation") && RotationDraftPanel::Modified(panel.Field(i)),
            "single-axis unbound rotation preserves both other pending rotation fields");
    }
    Check(panel.UnrelatedPreserved(), "unbound rotation leaves position and scale drafts untouched");
}
void UnboundRotationSuccessChecks() {
    for (bool floating : {false, true}) for (bool menu : {false, true})
        for (int axis = 0; axis < (menu ? 1 : 3); ++axis) for (int selection = 0; selection < 3; ++selection) {
            PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f); LegacyPoseResetCurve(f);
            if (selection == 1) f.first.selected[0] = f.first.selected[1] = 0;
            if (selection == 2) { f.first.selected[0] = 1; f.first.selected[1] = 0; }
            if (selection == 2) f.first.m->undoState[0] = f.first.m->undoState[1] = 29;
            UnboundRotationDrafts(panel, axis);
            const auto expected = UnboundRotationExpected(f, menu, axis, floating);
            const auto before = f.first.bones; const auto selected = f.first.selected;
            const auto other = f.second.Digest(), otherRing = PoseRingDigest(f.second);
            const auto scales = bone_scale::PoseValues(f.first.raw()); const auto curves = bone_curve::AuthoringState::Capture(f.first.raw());
            const unsigned next = (f.first.m->undoState[0] + 1) % 30;
            panel.Command(menu, axis);
            const auto& record = f.first.m->undoRings[0].slots[next];
            auto expectedBones = before; std::memcpy(expectedBones[1].rotQuat, expected.data(), sizeof expected);
            Check(f.first.m->undoState[0] == next && f.first.m->undoState[1] == next && f.first.m->undoDirty && !f.first.m->redoDirty &&
                record.operation == 1 && record.dirty == 1 && record.bonePose && record.bonePose[0].boneIndex == 1 &&
                !record.auxiliaryPose && f.first.dirty[1] == 1 && !f.first.dirty[0],
                "unbound reset captures its actual displayed bone once even with empty or different selected set and ring wrap");
            Check(!std::memcmp(f.first.bones.data(), expectedBones.data(), sizeof expectedBones) && f.first.selected == selected &&
                f.second.Digest() == other && PoseRingDigest(f.second) == otherRing && f.SelectionUnchanged() &&
                f.app->CurrentFrame() == 37 && !f.app->PhysicsResetPending() && !bone_scale::TransformsApplied(f.first.raw()) &&
                scales == bone_scale::PoseValues(f.first.raw()) && curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())),
                "ordinary rotation preserves exact unrelated bone backup translation scales curves selection and playback state");
            Check(f.app->SceneModified() && f.app->RecordedFrameCountByte1() && IsWindowEnabled(f.Edit(panel::kUndoButton)) &&
                !IsWindowEnabled(f.Edit(panel::kRedoButton)), "unbound rotation publishes project flags and native history buttons");
            UnboundRotationCaptions(f, panel, menu, axis, expected);
            const auto after = f.first.Digest(), ring = PoseRingDigest(f.first); UnboundRotationLoseFocus(panel, axis);
            Check(after == f.first.Digest() && ring == PoseRingDigest(f.first), "canceled unbound rotation draft cannot be re-committed on actual focus loss");
            Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && !f.first.dirty[1] &&
                !std::memcmp(f.first.bones.data(), before.data(), sizeof before) && scales == bone_scale::PoseValues(f.first.raw()) &&
                curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())), "native undo restores exact unbound rotation dirty and complete side state");
            Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && f.first.dirty[1] == 1 &&
                !std::memcmp(f.first.bones.data(), expectedBones.data(), sizeof expectedBones), "native redo restores exact unbound rotation");
            LegacyPoseResetUnbound(f);
        }
}
void UnboundRotationNoopAndTakeoverChecks() {
    for (bool floating : {false, true}) for (bool menu : {false, true})
        for (int axis = 0; axis < (menu ? 1 : 3); ++axis) for (int predicate = 0; predicate < 4; ++predicate) {
            PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f);
            panel.Command(menu, axis); Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()), "create genuine native redo before zero-axis reset");
            f.app->PlaybackPhysicsMode() = predicate == 1 ? 2 : 3;
            f.first.bones[1].hasRigidBody = predicate != 0;
            f.first.dirty[1] = predicate == 2 ? 2 : 0;
            TakeoverDegrees(*f.app, axis);
            if (menu) { std::fill(std::begin(f.first.bones[1].rotQuat), std::end(f.first.bones[1].rotQuat), 0.f); f.first.bones[1].rotQuat[3] = 1; }
            const bool takeover = predicate == 3;
            const auto expected = UnboundRotationExpected(f, menu, axis, floating);
            if (takeover && !menu) std::memcpy(f.first.bones[1].rotQuat, expected.data(), sizeof expected);
            UnboundRotationDrafts(panel, axis);
            const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto bones = f.first.bones;
            const auto cached = PoseAxisReadDegrees(*f.app); const auto cursor = f.first.m->undoState[0];
            // TryUndoModelEdit publishes native pose/redo payloads and cursor,
            // while command_control_400 owns the UI undoDirty/redoDirty flags.
            // Preserve the real byte here, rather than assuming this lower
            // level undo call has already published UI availability.
            const auto redoDirtyBefore = f.first.m->redoDirty;
            Check(cursor != f.first.m->undoState[1], "native undo created a redo range independent of its UI dirty byte");
            f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
            if (!takeover) failAfter = 0;
            panel.Command(menu, axis); failAfter = -1;
            if (!takeover) {
                Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && f.first.m->redoDirty == redoDirtyBefore &&
                    !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(), "excluded physics zero reset is allocation-free and retains exact data redo and project flags");
                const auto degrees = PoseAxisReadDegrees(*f.app);
                for (int i = 0; i < 3; ++i) if (menu || i == axis) { Check(degrees[i] == 0, "no-op resets only requested derived angles"); RotationDraftCaption(panel, i, 0); }
                    else Check(degrees[i] == cached[i] && RotationDraftPanel::Modified(panel.Field(i)), "zero axis does not roundtrip other Euler caches or drafts");
            } else {
                Check(f.first.m->undoState[0] == (cursor + 1) % 30 && f.first.dirty[1] == 1 &&
                    !std::memcmp(f.first.bones.data(), bones.data(), sizeof bones), "author-equal physical rotation still records dirty-only takeover without changing backup");
                UnboundRotationCaptions(f, panel, menu, axis, expected);
            }
            const auto after = f.first.Digest(), afterRing = PoseRingDigest(f.first); UnboundRotationLoseFocus(panel, axis);
            Check(after == f.first.Digest() && afterRing == PoseRingDigest(f.first), "zero reset focus loss cannot recreate a native step");
            if (takeover) {
                Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && !f.first.dirty[1], "native takeover undo restores dirty zero");
                Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && f.first.dirty[1] == 1, "native takeover redo restores dirty one");
            } else Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()), "zero reset retained usable native redo");
        }
}
void UnboundRotationAllocationChecks() {
    for (bool floating : {false, true}) for (bool menu : {false, true}) for (bool cold : {false, true}) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f, !cold);
        if (cold) bone_scale::Forget(f.first.raw()); else LegacyPoseResetCurve(f);
        UnboundRotationDrafts(panel, 0);
        const unsigned next = (f.first.m->undoState[0] + 1) % 30; auto& reused = f.first.m->undoRings[0].slots[next];
        reused.operation = 2; reused.dirty = 1; reused.frame = 7;
        reused.bonePose = static_cast<mdl::BonePoseSnapshot*>(undo_storage::Allocate(sizeof(mdl::BonePoseSnapshot)));
        reused.auxiliaryPose = undo_storage::Allocate(sizeof(mdl::BoneKeySnapshot));
        *reused.bonePose = {}; reused.bonePose->boneIndex = 1; reused.bonePose->rotation[3] = 1;
        std::memset(reused.auxiliaryPose, 0, sizeof(mdl::BoneKeySnapshot));
        const auto* oldPose = reused.bonePose; const auto* oldAux = reused.auxiliaryPose;
        const auto poseBytes = *oldPose; const auto keyBytes = *static_cast<const mdl::BoneKeySnapshot*>(oldAux);
        f.first.m->undoState[1] = next; f.first.m->redoDirty = 1;
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto cached = PoseAxisReadDegrees(*f.app);
        const auto scales = bone_scale::PoseValues(f.first.raw()); const auto curves = bone_curve::AuthoringState::Capture(f.first.raw());
        unsigned failures = 0; bool success = false;
        for (long limit = 0; limit < 512; ++limit) {
            failAfter = limit; panel.Command(menu); failAfter = -1;
            if (f.first.m->undoState[0] == next) { success = true; break; }
            ++failures; undo_storage::Allocation allocation{};
            Check(reused.bonePose == oldPose && reused.auxiliaryPose == oldAux && undo_storage::Find(oldPose, allocation) && undo_storage::Find(oldAux, allocation),
                "failed rotation retains both old native slot buffer owners before accessing payloads");
            Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && cached == PoseAxisReadDegrees(*f.app) &&
                !std::memcmp(oldPose, &poseBytes, sizeof poseBytes) && !std::memcmp(oldAux, &keyBytes, sizeof keyBytes) &&
                scales == bone_scale::PoseValues(f.first.raw()) && curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())) &&
                UnboundRotationDraftsIntact(panel, 0) && (!cold || !bone_scale::HasPoseStorage(f.first.raw())),
                "rotation preparation failure preserves complete ring payloads author pose side state caches and drafts including absent storage");
        }
        undo_storage::Allocation allocation{};
        Check(success && failures > 3 && reused.operation == 1 && reused.dirty == 1 && !reused.auxiliaryPose &&
            !undo_storage::Find(oldPose, allocation) && !undo_storage::Find(oldAux, allocation),
            "rotation allocation sweep publishes exactly one replacement and releases both obsolete buffers");
        Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()),
            "first successful rotation after cold or warm allocation failures remains undoable and redoable");
    }
}
void UnboundRotationGuardChecks() {
    for (bool floating : {false, true}) for (bool menu : {false, true}) for (bool noop : {false, true}) for (int gate = 0; gate < 7; ++gate) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f);
        if (noop) { f.app->BoneRotationEditDegreesX() = 0; f.first.bones[1].rotQuat[2] = 0; f.first.bones[1].rotQuat[3] = 1; }
        UnboundRotationDrafts(panel, 0);
        std::unique_ptr<ScopedSceneMutation> mutation; std::unique_ptr<bake::ScopedUiInputGate> input;
        if (gate == 0) f.app->PlaybackActive() = 1;
        if (gate == 1) f.app->FrameStepPlayback() = 1;
        if (gate == 2) f.app->state.recordPlaybackStartPending = 1;
        if (gate == 3) mutation = std::make_unique<ScopedSceneMutation>(*f.app);
        if (gate == 4) input = std::make_unique<bake::ScopedUiInputGate>();
        if (gate == 5) { Check(GetCapture() == nullptr, "capture only the test rotation panel"); SetCapture(panel.base.owner); Check(GetCapture() == panel.base.owner, "test-owned rotation capture is active"); }
        if (gate == 6) f.app->FrameRangeDialog() = panel.base.owner;
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto cached = PoseAxisReadDegrees(*f.app);
        panel.Command(menu);
        Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && cached == PoseAxisReadDegrees(*f.app) && UnboundRotationDraftsIntact(panel, 0),
            "unbound changed and no-op rotation honors playback mutation capture and modal guards without legacy fallback");
        panel.Clear(); f.app->PlaybackActive() = f.app->FrameStepPlayback() = f.app->state.recordPlaybackStartPending = 0;
        if (gate == 5) ReleaseCapture(); if (gate == 6) f.app->FrameRangeDialog() = nullptr;
    }
}
// Null one named import in this test executable only, then restore the exact
// pointer and page protection. Static D3DX Load re-reads its IAT, so merely
// setting math.rotX=nullptr would not exercise a missing-function failure.
struct UnboundRotationMathFault {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
    void** slot = nullptr; void* previous = nullptr;
    static void Assign(void** address, void* value) {
        DWORD protection = 0, discarded = 0;
        Check(VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &protection) != FALSE, "make only owned D3DX import slot writable");
        *address = value;
        Check(VirtualProtect(address, sizeof(*address), protection, &discarded) != FALSE, "restore D3DX import page protection");
    }
    UnboundRotationMathFault() {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        Check(rva != 0, "test executable exposes named imports for missing-math check");
        for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva); descriptor->Name && !slot; ++descriptor) {
            if (!descriptor->OriginalFirstThunk) continue;
            auto* name = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
            auto* target = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; name->u1.AddressOfData; ++name, ++target) {
                if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;
                const auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData);
                if (!std::strcmp(reinterpret_cast<const char*>(imported->Name), "D3DXMatrixRotationX")) {
                    slot = reinterpret_cast<void**>(&target->u1.Function); break;
                }
            }
        }
        Check(slot && *slot, "locate real named D3DXMatrixRotationX import"); previous = *slot; Assign(slot, nullptr);
    }
    ~UnboundRotationMathFault() { Assign(slot, previous); Check(d3dx::Get().Load(), "restore actual D3DX imports after missing-math test"); }
#endif
};
void UnboundRotationMathChecks() {
    for (bool floating : {false, true}) for (bool missing : {false, true}) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f); UnboundRotationDrafts(panel, 0);
        if (!missing) f.app->BoneRotationEditDegreesY() = std::numeric_limits<float>::infinity();
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto cached = PoseAxisReadDegrees(*f.app);
        if (missing) {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
            UnboundRotationMathFault fault;
            Check(!d3dx::Get().Load() && !d3dx::Get().rotX, "missing-math fixture actually makes production D3DX loading fail");
            panel.Command(false);
#else
            auto& math = d3dx::Get(); const auto original = math.rotX; math.rotX = nullptr;
            Check(math.Load() && !math.rotX, "dynamic D3DX retains its missing function under cached module load");
            panel.Command(false); math.rotX = original;
#endif
        } else panel.Command(false);
        Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && cached == PoseAxisReadDegrees(*f.app) &&
            UnboundRotationDraftsIntact(panel, 0) && !f.app->SceneModified(),
            "unbound rotation rejects unavailable math or nonfinite target before old ring authoring angle caches or drafts change");
    }
}
void UnboundRotationIdentityChecks() {
    for (bool floating : {false, true}) for (bool menu : {false, true}) for (bool noop : {false, true}) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f);
        if (noop) { f.app->BoneRotationEditDegreesX() = 0; f.first.bones[1].rotQuat[2] = 0; f.first.bones[1].rotQuat[3] = 1; }
        UnboundRotationDrafts(panel, 0); MmsObjectRef old{}, current{};
        Check(plugins::FindSceneObjectIdentity(f.first.raw(), old), "rotation identity fixture begins registered");
        plugins::ForgetSceneObject(f.first.raw());
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto cached = PoseAxisReadDegrees(*f.app);
        failAfter = 0; panel.Command(menu); failAfter = -1;
        Check(!plugins::FindSceneObjectIdentity(f.first.raw(), current) && before == f.first.Digest() && ring == PoseRingDigest(f.first) &&
            cached == PoseAxisReadDegrees(*f.app) && UnboundRotationDraftsIntact(panel, 0),
            "failed first rotation identity registration leaves native authoring history and drafts untouched");
        panel.Command(menu);
        Check(plugins::FindSceneObjectIdentity(f.first.raw(), current) && current.generation != old.generation &&
            plugins::SceneObjectPresent(f.first.raw(), current.id) && !RotationDraftPanel::Modified(panel.Field(0)),
            "ordinary rotation establishes a stable identity before clearing a successful or no-op draft");
        const auto after = f.first.Digest(), afterRing = PoseRingDigest(f.first); UnboundRotationLoseFocus(panel, 0);
        Check(after == f.first.Digest() && afterRing == PoseRingDigest(f.first), "new-identity rotation draft stays canceled on focus loss");
    }
}
void UnboundRotationTextFailureChecks() {
    struct Fault {
        bool reject = true; unsigned writes = 0;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Fault*>(data);
            if (self.reject && message == WM_SETTEXT) { ++self.writes; return FALSE; }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (bool menu : {false, true}) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f); UnboundRotationDrafts(panel, 0); Fault fault;
        Check(SetWindowSubclass(panel.Field(0), Fault::Proc, 2391, reinterpret_cast<DWORD_PTR>(&fault)) != FALSE, "reject only test-owned unbound rotation caption writes");
        panel.Command(menu); const auto after = f.first.Digest(), ring = PoseRingDigest(f.first);
        Check(fault.writes && f.first.dirty[1] == 1 && RotationDraftPanel::Text(panel.Field(0), "88.75") && !RotationDraftPanel::Modified(panel.Field(0)),
            "successful rotation cancels draft authorization despite failed optional caption write");
        UnboundRotationLoseFocus(panel, 0); Check(after == f.first.Digest() && ring == PoseRingDigest(f.first), "failed rotation caption cannot reintroduce old text through focus loss");
        fault.reject = false; f.app->BoneRotationEditDegreesX() = 0; panel.Command(menu);
        RotationDraftCaption(panel, 0, 0);
        Check(ring == PoseRingDigest(f.first) && panel.UnrelatedPreserved(), "no-op caption repair keeps native history and unrelated drafts");
        RemoveWindowSubclass(panel.Field(0), Fault::Proc, 2391);
    }
}
void UnboundRotationReentryChecks() {
    struct Reentry {
        PoseCommandFixture& f; HWND replacement; int action; bool fired = false;
        std::unique_ptr<plugins::NativePoseTestBinding> binding;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Reentry*>(data);
            if (!self.fired && message == EM_SETMODIFY && !wp) {
                self.fired = true; const auto result = DefSubclassProc(window, message, wp, lp);
                if (self.action == 0) self.f.app->SelectedModelSlot() = 1;
                if (self.action == 1) self.f.app->FloatingWindow() = self.replacement;
                if (self.action == 2) ++self.f.app->CurrentFrame();
                if (self.action == 3) self.binding = std::make_unique<plugins::NativePoseTestBinding>(*self.f.app, self.f.history);
                if (self.action == 4) { plugins::ForgetSceneObject(self.f.first.raw()); plugins::SceneObjectIdentity(self.f.first.raw()); }
                if (self.action == 5) self.f.first.m->selectedBone = 0;
                // Each context transition must stand on its own. Changing
                // angles in every case would mask a missing identity check.
                if (self.action == 6) {
                    self.f.app->BoneRotationEditDegreesX() = 71; self.f.app->BoneRotationEditDegreesY() = 72; self.f.app->BoneRotationEditDegreesZ() = 73;
                }
                RotationDraftPanel::Pending(window, "new context draft"); return result;
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (bool menu : {false, true}) for (int action = 0; action < 7; ++action) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f);
        f.app->BoneRotationEditDegreesX() = 0; f.first.bones[1].rotQuat[2] = 0; f.first.bones[1].rotQuat[3] = 1;
        UnboundRotationDrafts(panel, 0);
        HWND replacement = CreateWindowExA(0, "STATIC", "owned unbound rotation replacement", WS_OVERLAPPED, 0, 0, 300, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(replacement != nullptr, "create only an owned replacement rotation panel");
        for (int axis = 0; axis < 3; ++axis) {
            HWND edit = CreateWindowExA(0, "EDIT", "replacement pending", WS_CHILD, 0, 0, 80, 20, replacement,
                reinterpret_cast<HMENU>(INT_PTR(panel::kReadoutRotXEdit + axis)), GetModuleHandleW(nullptr), nullptr);
            Check(edit != nullptr, "replacement has actual distinct rotation HWNDs"); SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
        }
        Reentry reentry{f, replacement, action};
        Check(SetWindowSubclass(panel.Field(0), Reentry::Proc, 2392, reinterpret_cast<DWORD_PTR>(&reentry)) != FALSE, "observe real synchronous unbound rotation publication");
        const auto bones = f.first.bones; const auto ring = PoseRingDigest(f.first);
        const auto expectedAngles = action == 6 ? std::array<float, 3>{71, 72, 73} :
            menu ? std::array<float, 3>{0, 0, 0} : PoseAxisReadDegrees(*f.app);
        panel.Command(menu);
        Check(reentry.fired && RotationDraftPanel::Text(panel.Field(0), "new context draft") && RotationDraftPanel::Modified(panel.Field(0)) &&
            PoseAxisReadDegrees(*f.app) == expectedAngles && !std::memcmp(f.first.bones.data(), bones.data(), sizeof bones) &&
            PoseRingDigest(f.first) == ring, "unbound rotation independently detects model owner frame binding identity current-bone or angle transition");
        for (int axis = 1; axis < 3; ++axis) Check(RotationDraftPanel::Text(panel.Field(axis), "other rotation") && RotationDraftPanel::Modified(panel.Field(axis)),
            "stale rotation publication leaves remaining old target drafts intact");
        for (int axis = 0; axis < 3; ++axis) {
            HWND edit = GetDlgItem(replacement, panel::kReadoutRotXEdit + axis);
            Check(RotationDraftPanel::Text(edit, "replacement pending") && RotationDraftPanel::Modified(edit), "stale rotation publication never touches replacement owner drafts");
            SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
        }
        RemoveWindowSubclass(panel.Field(0), Reentry::Proc, 2392); reentry.binding.reset(); panel.Clear();
        f.app->SelectedModelSlot() = 0; f.app->FloatingWindow() = floating ? panel.base.owner : nullptr; DestroyWindow(replacement);
    }
}
void UnboundRotationButtonReentryChecks() {
    struct Reentry {
        PoseCommandFixture& f; bool angle; bool fired = false, sawCommitted = false;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Reentry*>(data);
            if (!self.fired && message == WM_ENABLE && wp) {
                self.fired = true;
                self.sawCommitted = self.f.first.dirty[1] == 1 && self.f.app->SceneModified() && self.f.app->RecordedFrameCountByte1();
                const auto result = DefSubclassProc(window, message, wp, lp);
                if (self.angle) {
                    self.f.app->BoneRotationEditDegreesX() = 71; self.f.app->BoneRotationEditDegreesY() = 72; self.f.app->BoneRotationEditDegreesZ() = 73;
                } else self.f.first.bones[1].trans[0] += 5;
                return result;
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (bool menu : {false, true}) for (bool angle : {false, true}) {
        PoseCommandFixture f; RotationDraftPanel panel(f, floating); UnboundRotationSeed(f); UnboundRotationDrafts(panel, 0);
        const auto expected = UnboundRotationExpected(f, menu, 0, floating);
        const auto cached = PoseAxisReadDegrees(*f.app); const auto originalX = f.first.bones[1].trans[0];
        const auto next = (f.first.m->undoState[0] + 1) % 30;
        const HWND undo = f.Edit(panel::kUndoButton), redo = f.Edit(panel::kRedoButton);
        EnableWindow(undo, FALSE); EnableWindow(redo, TRUE);
        Reentry reentry{f, angle};
        Check(SetWindowSubclass(undo, Reentry::Proc, 2393, reinterpret_cast<DWORD_PTR>(&reentry)) != FALSE,
            "observe only the owned undo button's actual enable callback");
        panel.Command(menu);
        Check(reentry.fired && reentry.sawCommitted && f.first.m->undoState[0] == next &&
            !std::memcmp(f.first.bones[1].rotQuat, expected.data(), sizeof expected),
            "button callback runs only after native authoring history and dirty flags are committed");
        Check(f.first.bones[1].trans[0] == originalX + (angle ? 0 : 5) &&
            PoseAxisReadDegrees(*f.app) == (angle ? std::array<float, 3>{71, 72, 73} : cached) &&
            IsWindowEnabled(redo) && UnboundRotationDraftsIntact(panel, 0),
            "independent translation or angle change stops remaining button and draft publication without reverting the callback");
        RemoveWindowSubclass(undo, Reentry::Proc, 2393); panel.Clear();
    }
}
void UnboundRotationRuntimeChecks() {
    for (bool floating : {false, true}) for (bool menu : {false, true}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); RotationDraftPanel panel(f, floating); UnboundRotationSeed(f);
        struct Runtime { ~Runtime() { plugins::Shutdown(); } } runtime;
        plugins::Initialize(f.app.get()); auto* history = plugins::NativeHistory(f.app.get());
        Check(history && !plugins::BoundNativePoseHistory(f.app.get()), "actual ordinary Initialize(false) leaves rotation on native history");
        UnboundRotationDrafts(panel, 0); const auto cursor = f.first.m->undoState[0]; panel.Command(menu);
        Check(f.first.m->undoState[0] == (cursor + 1) % 30 && !history->UndoCount() && !history->RedoCount(),
            "ordinary initialized rotation creates one native step without common migration records");
        const auto after = f.first.Digest(), ring = PoseRingDigest(f.first); UnboundRotationLoseFocus(panel, 0);
        Check(after == f.first.Digest() && ring == PoseRingDigest(f.first), "ordinary runtime canceled rotation draft cannot commit twice");
        panel.Clear();
        for (int repeat = 0; repeat < 2; ++repeat) {
            SetFocus(panel.Field(0)); Check(GetFocus() == panel.Field(0), "clean ordinary rotation field takes native focus"); panel.LoseFocus(0);
        }
        Check(after == f.first.Digest() && ring == PoseRingDigest(f.first), "clean focus changes keep native ring and exact rotation unchanged");
        const auto previous = f.first.bones[1]; const auto beforeTyped = f.first.m->undoState[0];
        panel.Draft(0, "12.5"); UnboundRotationLoseFocus(panel, 0);
        Check(f.first.m->undoState[0] == (beforeTyped + 1) % 30 &&
            std::memcmp(f.first.bones[1].rotQuat, previous.rotQuat, sizeof previous.rotQuat) != 0 && !history->UndoCount(),
            "real modified rotation still commits once on focus loss using the CPU-only native panel fixture");
        plugins::Shutdown(); LegacyPoseResetUnbound(f);
    }
}
void NativeUnboundRotationChecks() {
    UnboundRotationInstallation(); Check(d3dx::Get().Load(), "unbound rotation uses the real D3DX math library");
    const auto run = [](const char* name, void (*test)()) { std::printf("RUN unbound rotation %s\n", name); std::fflush(stdout); test(); };
    run("Success", UnboundRotationSuccessChecks); run("NoopAndTakeover", UnboundRotationNoopAndTakeoverChecks);
    run("Allocation", UnboundRotationAllocationChecks); run("Guards", UnboundRotationGuardChecks);
    run("Math", UnboundRotationMathChecks); run("Identity", UnboundRotationIdentityChecks);
    run("TextFailure", UnboundRotationTextFailureChecks); run("Reentry", UnboundRotationReentryChecks);
    run("ButtonReentry", UnboundRotationButtonReentryChecks);
    run("OrdinaryRuntime", UnboundRotationRuntimeChecks);
}
}
