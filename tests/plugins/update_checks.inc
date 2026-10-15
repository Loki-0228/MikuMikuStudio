static void UpdateChecks() {
    using namespace native_ui;
    UpdateCoordinator q; int calls = 0;
    q.Request({1, 1, 1, 10, UiDirty}); q.Request({1, 1, 1, 12, LayoutDirty});
    CHECK(q.Deadline() == 10);
    auto done = [&](const UpdateRequest& r) { ++calls; return UpdateOutcome{UpdateResult::Done, r.flags}; };
    CHECK(q.Drain(9, 8, done) == 0 && calls == 0);
    CHECK(q.Drain(10, 8, [&](const UpdateRequest& r) {
        CHECK(r.flags == (UiDirty | LayoutDirty)); q.Request({1, 1, 2, 10, PresentDirty});
        CHECK(q.Drain(10, 8, done) == 0); return UpdateOutcome{UpdateResult::Done, r.flags};
    }) == 1);
    CHECK(q.Deadline() == 10); CHECK(q.Drain(10, 8, done) == 1 && calls == 1);
    q.Request({1, 1, 1, 10, RenderDirty}); CHECK(!q.Deadline());
    q.Request({1, 1, 3, 20, TargetDirty | PresentDirty});
    CHECK(q.Drain(20, 8, [](const UpdateRequest&) { return UpdateOutcome{UpdateResult::Retry, 0, 50}; }) == 1);
    CHECK(q.Deadline() == 50 && q.Drain(49, 8, done) == 0);
    q.Available(1, false); CHECK(!q.Deadline() && q.Drain(60, 8, done) == 0);
    q.Available(1, true); CHECK(q.Deadline() == 50 && q.Drain(60, 8, done) == 1);
    q.Request({1, 1, 4, 60, RenderDirty});
    CHECK(q.Drain(60, 8, [](const UpdateRequest&) { return UpdateOutcome{UpdateResult::Failed}; }) == 1);
    CHECK(q.Failed(1) && !q.Deadline()); q.Request({1, 1, 4, 70, RenderDirty}); CHECK(!q.Failed(1));
    CHECK(q.Drain(70, 8, [&](const UpdateRequest&) { q.Request({1, 2, 1, 71, PresentDirty}); return UpdateOutcome{UpdateResult::Retry, 0, 80}; }) == 1);
    CHECK(q.Drain(71, 8, [&](const UpdateRequest& r) { CHECK(r.windowGeneration == 2 && r.flags == PresentDirty); return UpdateOutcome{UpdateResult::Done, r.flags}; }) == 1);
    q.Request({2, 1, 1, 80, UiDirty}); q.Request({3, 1, 1, 80, RenderDirty});
    CHECK(q.Drain(80, 8, [&](const UpdateRequest& r) { if (r.id == 2) q.Request({3, 2, 1, 80, PresentDirty}); else CHECK(false); return UpdateOutcome{UpdateResult::Done, r.flags}; }) == 2);
    CHECK(q.Drain(80, 8, [&](const UpdateRequest& r) { CHECK(r.id == 3 && r.windowGeneration == 2); return UpdateOutcome{UpdateResult::Done, r.flags}; }) == 1);
    CHECK(!q.Deadline());
    q.Request({1, 2, 2, 90, UiDirty}); q.Request({2, 1, 2, 90, UiDirty | TargetDirty});
    CHECK(q.Drain(90, 8, [&](const UpdateRequest& r) { CHECK(r.id == 1); q.Request({2, 1, 3, 90, RenderDirty}); return UpdateOutcome{UpdateResult::Done, r.flags}; }) == 2);
    CHECK(q.Drain(90, 8, [&](const UpdateRequest& r) { CHECK(r.id == 2 && r.geometryRevision == 3 && r.flags == (UiDirty | TargetDirty | RenderDirty)); return UpdateOutcome{UpdateResult::Done, r.flags}; }) == 1);
    for (bool throwing : {false, true}) {
        q.Request({9, 1, 1, 100, UiDirty | TargetDirty});
        CHECK(q.Drain(100, 8, [&](const UpdateRequest&) -> UpdateOutcome { q.Request({9, 1, 2, 100, RenderDirty}); if (throwing) throw std::runtime_error("provider failed"); return {UpdateResult::Failed}; }) == 1);
        CHECK(q.Drain(100, 8, [&](const UpdateRequest& r) { CHECK(r.id == 9 && r.geometryRevision == 2 && r.flags == (UiDirty | TargetDirty | RenderDirty)); return UpdateOutcome{UpdateResult::Done, r.flags}; }) == 1);
        q.Forget(9);
    }
}
