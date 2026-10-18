// Real lifecycle DLL tasks must resume without an explicit Manager::Drain()
// after a nested message pump consumes their original wake messages.
namespace task_dispatch_checks {
static void Run(const fs::path& root, const fs::path& fixture, bool readOnly, bool stopPending) {
    const auto name = readOnly ? "task-read-only-retry" : stopPending ? "task-stop-retry" : "task-storage-retry";
    const auto package = root / name / "PLUGINS" / "fixture";
    fs::create_directories(package); fs::copy_file(fixture, package / "task-lifecycle.dll");
    const auto id = std::string("org.example.gui-queue-") + name;
    Write(package / "plugin.json", ManifestText(id, "task-lifecycle.dll"));
    std::vector<std::string> logs;
    const auto count = [&](const char* marker) {
        return std::count_if(logs.begin(), logs.end(), [&](const auto& line) { return line.find(marker) != std::string::npos; });
    };
    Manager manager(root / name / "user", [&](const auto& line) { logs.push_back(line); });
    manager.Load(package.parent_path());
    CHECK(manager.Status().size() == 1 && manager.Status()[0].stage == "started");
    const auto pending = [&] { CHECK(count("TASK_RUN") == 0 && count("TASK_CANCEL") == 0); };
    pending();
    if (readOnly) {
        const auto element = manager.Ui().elements.at(id + ".button").handle;
        CHECK(manager.Ui().WithElementState(element, [&](const MmsCommandStateV1&) -> MmsResult {
            CHECK(manager.Ui().InCallback());
            manager_refresh_checks::Pump(55); pending(); return MMS_OK;
        }) == MMS_OK);
    } else {
        CHECK(manager.Storage().Prepare({}) == MMS_OK && manager.Storage().Prepared());
        manager_refresh_checks::Pump(55); pending();
        manager.Storage().Cancel();
    }
    if (stopPending) {
        CHECK(manager.Stop());
        CHECK(count("TASK_RUN") == 0 && count("TASK_CANCEL") == 2 && count("DESTROY") == 1);
    } else {
        const auto until = GetTickCount64() + 1500;
        while ((count("TASK_RUN") != 1 || count("TASK_CANCEL") != 1) && GetTickCount64() < until)
            manager_refresh_checks::Pump(5);
        CHECK(count("TASK_RUN") == 1 && count("TASK_CANCEL") == 1 && count("RUNNING_CANCEL_BUSY") == 1);
        CHECK(manager.Stop() && count("DESTROY") == 1);
    }
    CHECK(GetModuleHandleW(L"task-lifecycle.dll") == nullptr);
    manager_refresh_checks::Pump(45);
    CHECK(count("TASK_RUN") == (stopPending ? 0 : 1) && count("TASK_CANCEL") == (stopPending ? 2 : 1));
    CHECK(manager.Stop() && count("DESTROY") == 1);
    std::cout << "task dispatch " << name << " PASS\n";
}
static void EditResume(const fs::path& root, const fs::path& fixture) {
    const std::string id = "org.example.edit-probe";
    ManagerEditCase f(root / "task-edit-retry"); ManagerEditProbe p(f.Package(fixture, id)); f.Load();
    CHECK(f.Invoke(id, "once", f.Revision()) == MMS_OK && p.Value() == 1);
    CHECK(f.Invoke(id, "undo") == MMS_OK && p.Value() == 0 && f.host->history.RedoCount() == 1);
    const auto revision = f.Revision(); const auto notifications = f.observations->published;
    CHECK(f.Invoke(id, "queue", revision) == MMS_OK && p.Value() == 0);
    StoredRecords saved; CHECK(f.manager->Storage().Capture(saved) == MMS_OK);
    CHECK(f.manager->Storage().PrepareRestore(saved) == MMS_OK);
    manager_refresh_checks::Pump(55);
    CHECK(p.Value() == 0 && f.Revision() == revision && f.host->history.UndoCount() == 0 &&
        f.host->history.RedoCount() == 1 && f.observations->published == notifications);
    f.manager->Storage().Cancel();
    const auto until = GetTickCount64() + 1500;
    while (p.Value() != 1 && GetTickCount64() < until) manager_refresh_checks::Pump(5);
    CHECK(p.Value() == 1 && f.Revision() == revision + 1 && f.host->history.UndoCount() == 1 &&
        f.host->history.RedoCount() == 0 && f.observations->published == notifications + 1 && f.observations->clean);
    CHECK(f.Invoke(id, "undo") == MMS_OK && p.Value() == 0);
    CHECK(f.Invoke(id, "redo") == MMS_OK && p.Value() == 1);
    const auto beforeFailure = f.Revision(); const auto published = f.observations->published;
    const auto cancelled = p.Cancels();
    CHECK(f.Invoke(id, "queue-throw", beforeFailure) == MMS_OK);
    CHECK(f.manager->Storage().PrepareRestore(saved) == MMS_OK);
    manager_refresh_checks::Pump(55);
    CHECK(p.Cancels() == cancelled && p.Value() == 1 && f.Revision() == beforeFailure);
    f.manager->Storage().Cancel();
    const auto failureUntil = GetTickCount64() + 1500;
    while (p.Cancels() == cancelled && GetTickCount64() < failureUntil) manager_refresh_checks::Pump(5);
    CHECK(p.Cancels() == cancelled + 1 && p.Value() == 1 && p.Pending() == 0 && !f.host->attached->HasPending());
    CHECK(f.Revision() == beforeFailure && f.host->history.UndoCount() == 1 &&
        f.host->history.RedoCount() == 0 && f.observations->published == published && f.observations->clean);
    manager_refresh_checks::Pump(45);
    CHECK(p.Cancels() == cancelled + 1 && f.Revision() == beforeFailure && f.observations->published == published);
    f.Close();
    std::cout << "task dispatch shared edit, redo and throwing callback retry PASS\n";
}
} // namespace task_dispatch_checks
static void TaskDispatchChecks(const fs::path& root, const fs::path& fixture, const fs::path& editFixture) {
    task_dispatch_checks::Run(root, fixture, false, false);
    task_dispatch_checks::Run(root, fixture, true, false);
    task_dispatch_checks::Run(root, fixture, false, true);
    task_dispatch_checks::EditResume(root, editFixture);
}
