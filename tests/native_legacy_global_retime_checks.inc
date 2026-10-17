// Include after native_global_sparse_registration_checks.inc.
namespace {
void LegacyGlobalRetimeSeed(GlobalRegistrationFixture& f, unsigned family) {
    GSparseAuthor(f, family);
    f.app->CurrentFrame() = 12;
    if (family) f.app->LightDirection()[0] -= .25f;
    else f.app->CameraPositionX() += 17.25f;
}
void LegacyGlobalRetimeChecks() {
    for (unsigned family = 0; family < 2; ++family) for (bool insert : {true, false}) {
        GlobalRegistrationFixture f;
        f.app->CurrentFrame() = 12;
        failAfter = 0;
        const auto affected = ShiftGlobalKeysForFrameLine(f.app.get(), family == 0, insert);
        failAfter = -1;
        Check(affected > 0, "unauthored legacy frame lines retain their allocation-free native path");
    }
    for (unsigned family = 0; family < 2; ++family) for (bool insert : {true, false}) {
        GlobalRegistrationFixture f, oracle;
        LegacyGlobalRetimeSeed(f, family);
        // An unauthored copy is an independent native-byte oracle. Its native
        // interpolation may approximate the source's higher-precision layer.
        const auto camera = GlobalRegistrationKeys(f.app->CameraKeys());
        const auto light = GlobalRegistrationKeys(f.app->LightKeys());
        std::memcpy(oracle.app->CameraKeys(), camera.data(), camera.size() * sizeof(mdl::CameraKey));
        std::memcpy(oracle.app->LightKeys(), light.data(), light.size() * sizeof(mdl::LightKey));
        oracle.app->CurrentFrame() = 12;
        oracle.app->LastRegisteredFrame() = f.app->LastRegisteredFrame();
        const auto before = GSparseRead(f, family);
        const auto other = GSparseSnapshot(f, 1 - family);
        const auto cameraPose = f.app->CameraPositionX(), lightPose = f.app->LightDirection()[0];
        const auto moved = ShiftGlobalKeysForFrameLine(f.app.get(), family == 0, insert);
        const auto expected = ShiftGlobalKeysForFrameLine(oracle.app.get(), family == 0, insert);
        Check(moved == expected && moved > 0, "legacy authored frame line reports native affected count");
        Check(GlobalRegistrationSame(f.app->CameraKeys(), GlobalRegistrationKeys(oracle.app->CameraKeys())) &&
              GlobalRegistrationSame(f.app->LightKeys(), GlobalRegistrationKeys(oracle.app->LightKeys())) &&
              f.app->LastRegisteredFrame() == oracle.app->LastRegisteredFrame(),
              "legacy authored frame line preserves original native bytes and last-frame policy");
        const auto after = GSparseRead(f, family);
        const auto destination = insert ? 32 : 30;
        Check(after[0].track.keys.size() == before[0].track.keys.size() &&
              after[0].track.keys.front().frame == 0 && after[0].track.keys.back().frame == destination &&
              !GSparseHas(after[0], insert ? 13 : 12),
              "legacy frame line moves sparse keys without filling the missing union row");
        const auto& oldFirst = before[0].track.keys.front();
        const auto& oldLast = before[0].track.keys.back();
        const auto& newFirst = after[0].track.keys.front();
        const auto& newLast = after[0].track.keys.back();
        const double ratio = 31. / destination;
        Check(newFirst.value == oldFirst.value && newLast.value == oldLast.value &&
              std::abs(newFirst.outSlope - oldFirst.outSlope * ratio) < 1e-12 &&
              std::abs(newLast.inSlope - oldLast.inSlope * ratio) < 1e-12 &&
              newFirst.outWeight == oldFirst.outWeight && newLast.inWeight == oldLast.inWeight,
              "legacy frame line preserves precise values and normalized free handles across the changed span");
        Check(GSparseSnapshot(f, 1 - family) == other && f.history.UndoCount() == 0 && f.history.RedoCount() == 0 &&
              f.app->CurrentFrame() == 12 && f.app->CameraPositionX() == cameraPose && f.app->LightDirection()[0] == lightPose,
              "unbound legacy frame line neither creates a common journal nor alters another track or dirty pose");
    }
    for (unsigned family = 0; family < 2; ++family) for (bool insert : {true, false}) {
        bool completed = false;
        for (long allocation = 0; allocation < 2048 && !completed; ++allocation) {
            GlobalRegistrationFixture f;
            LegacyGlobalRetimeSeed(f, family);
            f.app->state.optflag[0] = 1;
            const auto before = f.Digest();
            const auto authored = GSparseSnapshot(f, family);
            failAfter = allocation;
            const auto affected = ShiftGlobalKeysForFrameLine(f.app.get(), family == 0, insert);
            failAfter = -1;
            if (affected) completed = true;
            else {
                Check(f.Digest() == before && GSparseSnapshot(f, family) == authored && f.history.UndoCount() == 0,
                      "legacy retime allocation failure leaves native keys, free authoring and history untouched");
                if (allocation < 4) {
                    failAfter = allocation;
                    const auto result = family == 0
                        ? (insert ? InsertBoneCameraFrameLine(f.app.get()) : DeleteBoneCameraFrameLine(f.app.get()))
                        : (insert ? InsertFacialLightFrameLine(f.app.get()) : DeleteFacialLightFrameLine(f.app.get()));
                    failAfter = -1;
                    Check(result == 0 && f.Digest() == before && GSparseSnapshot(f, family) == authored,
                          "legacy outer frame command skips evaluation, dirty and UI tails after preparation failure");
                }
            }
        }
        Check(completed, "legacy retime allocation sweep reaches a fully prepared publication");
    }
}
}
