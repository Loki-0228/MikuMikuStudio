namespace {
std::array<float, 7> NumericCameraPose(MMDApp& app) {
    return {app.CameraPositionX(), app.CameraPositionY(), app.CameraPositionZ(), app.CameraPitch(), app.CameraYaw(), app.CameraRoll(), app.CameraDistance()};
}
void CameraNumericHistoryChecks() {
    GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto before = NumericCameraPose(*f.app); const auto digest = f.Digest();
    plugins::CameraNumericEdit cancelled; Check(cancelled.Begin(*f.app, f.history) == MMS_OK, "capture cancellable camera numeric session");
    Check(f.history.Enter() == MMS_OK, "camera dialog session holds no history reservation"); f.history.Leave();
    const auto values = cancelled.Values();
    Check(values[0] == before[0] && values[6] == -before[6] && values[3] < 0 && values[4] > 0 && f.Digest() == digest,
        "camera numeric display uses original pitch and distance signs without live writes");
    Check(cancelled.Apply(values) == MMS_OK && f.Digest() == digest && !f.history.UndoCount(),
        "unchanged camera display retains exact original radians without rounding into history");
    plugins::CameraNumericEdit edit; Check(edit.Begin(*f.app, f.history) == MMS_OK, "capture camera numeric authoring");
    auto input = edit.Values(); input[0] = 12.5f; input[3] = 30; input[5] = 90; input[6] = 60;
    Check(edit.Apply(input) == MMS_OK && edit.Apply(input) == MMS_E_NOT_READY && f.history.UndoCount() == 1,
        "seven-field camera edit commits once and consumes the session");
    constexpr float radians = 3.14159265358979323846f / 180.0f;
    Check(f.app->CameraPositionX() == 12.5f && f.app->CameraPositionY() == before[1] &&
        f.app->CameraPitch() == -30 * radians && f.app->CameraYaw() == before[4] &&
        f.app->CameraRoll() == 90 * radians && f.app->CameraDistance() == -60,
        "camera numeric commit applies only changed coordinates angles and signed distance");
    const auto after = NumericCameraPose(*f.app);
    f.app->CurrentFrame() = 29; f.app->CameraPositionY() = 21; f.app->CameraParentModel() = 33;
    Check(f.history.History(false) == MMS_OK && f.app->CameraPositionX() == before[0] &&
        f.app->CameraPitch() == before[3] && f.app->CameraYaw() == before[4] && f.app->CameraDistance() == before[6] &&
        f.app->CameraPositionY() == 21 && f.app->CameraParentModel() == 33 && f.app->CurrentFrame() == 29,
        "camera numeric undo restores changed fields and retains later unrelated pose binding and playhead");
    Check(f.history.History(true) == MMS_OK && f.app->CameraPositionX() == after[0] && f.app->CameraDistance() == after[6] &&
        f.app->CameraPositionY() == 21, "camera numeric redo preserves untouched fields");
}
void CameraNumericStaleChecks() {
    for (int mutation = 0; mutation < 9; ++mutation) {
        GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        plugins::CameraNumericEdit edit; Check(edit.Begin(*f.app, f.history) == MMS_OK, "capture camera modal stale source");
        auto input = edit.Values(); input[0] = 88;
        if (mutation == 0) ++f.app->CurrentFrame();
        if (mutation == 1) f.app->CameraPositionX() += 2;
        if (mutation == 2) f.app->CameraPitch() += .25f;
        if (mutation == 3) f.app->CameraParentModel() = 3;
        if (mutation == 4) f.app->CameraFov() += 1;
        if (mutation == 5) f.app->CameraPerspective() ^= 1;
        if (mutation == 6) f.app->CameraMode() = 0;
        if (mutation == 7) Check(f.history.Reset() == MMS_OK, "reset history while camera input is open");
        if (mutation == 8) { auto* replacement = mdl::KeyPoolCreate<mdl::CameraKey>(4);
            std::memcpy(replacement, f.app->CameraKeys(), 4 * sizeof(mdl::CameraKey));
            mdl::KeyPoolDestroy(f.app->CameraKeys()); f.app->CameraKeys() = replacement; }
        const auto digest = f.Digest();
        Check(edit.Apply(input) == MMS_E_STALE && f.Digest() == digest && !f.history.UndoCount(),
            "camera numeric source change rejects stale input without live writes");
    }
    PoseNumericFixture f; auto* keys = mdl::KeyPoolCreate<mdl::CameraKey>(4); f.app->CameraKeys() = keys;
    f.app->CameraMode() = 1; f.app->CameraParentModel() = 0; f.app->CameraParentBone() = 0;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    plugins::CameraNumericEdit parent; Check(parent.Begin(*f.app, f.history) == MMS_OK, "capture camera parent object identity");
    auto input = parent.Values(); input[0] = 8;
    plugins::ForgetSceneObject(f.first.raw()); plugins::SceneObjectIdentity(f.first.raw());
    Check(parent.Apply(input) == MMS_E_STALE && !f.history.UndoCount() && !f.app->CameraPositionX(),
        "same parent slot with a new object generation cannot accept old camera numeric input");
    f.history.Reset(); f.history.Collect(); mdl::KeyPoolDestroy(f.app->CameraKeys());
}
void CameraNumericFailureChecks() {
    GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    CmdViewMenu(f.app.get(), nullptr, 302, 0); Check(f.history.History(false) == MMS_OK, "seed camera numeric redo");
    const auto digest = f.Digest(); unsigned failures = 0; bool succeeded = false;
    for (long limit = 0; limit < 128; ++limit) {
        plugins::CameraNumericEdit edit; failAfter = limit; auto result = edit.Begin(*f.app, f.history);
        if (result == MMS_OK) { auto input = edit.Values(); input[0] = 17; input[3] = 30; result = edit.Apply(input); }
        failAfter = -1;
        if (result == MMS_OK) { succeeded = true; break; }
        ++failures;
        Check(f.Digest() == digest && !f.history.UndoCount() && f.history.RedoCount() == 1,
            "camera numeric allocation failure leaves every app byte all keys and redo unchanged");
        Check(f.history.Enter() == MMS_OK, "camera numeric failure releases history reservation"); f.history.Leave();
    }
    Check(succeeded && failures >= 5 && f.history.UndoCount() == 1 && !f.history.RedoCount(),
        "camera numeric failure sweep reaches preparation then commits successfully");
    Check(f.history.History(false) == MMS_OK, "camera numeric remains undoable after failed preparations");
    plugins::CameraNumericEdit invalid; Check(invalid.Begin(*f.app, f.history) == MMS_OK, "capture invalid camera numeric input");
    auto input = invalid.Values(); input[6] = std::numeric_limits<float>::infinity(); const auto before = f.Digest();
    Check(invalid.Apply(input) == MMS_E_ARGUMENT && f.Digest() == before && f.history.RedoCount() == 1,
        "non-finite camera input rejects all seven fields and preserves redo");
}
}
void NativeCameraNumericChecks() { CameraNumericHistoryChecks(); CameraNumericStaleChecks(); CameraNumericFailureChecks(); }
