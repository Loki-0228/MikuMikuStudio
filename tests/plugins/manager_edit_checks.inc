// Include after ManifestText/Write. Uses a separately loaded C ABI DLL so
// callback classification and ownership do not depend on direct C++ fixtures.
struct ManagerEditProbe {
    HMODULE module = nullptr;
    int (*Value)() = nullptr;
    uint64_t (*Flags)() = nullptr;
    int (*Cancels)() = nullptr;
    uint64_t (*Pending)() = nullptr;
    uint64_t (*ProviderFlags)() = nullptr;
    void (*ProviderMask)(uint64_t) = nullptr;
    explicit ManagerEditProbe(const fs::path& path) {
        module = LoadLibraryW(path.c_str()); CHECK(module != nullptr);
        Value = reinterpret_cast<decltype(Value)>(GetProcAddress(module, "GetProbeValue"));
        Flags = reinterpret_cast<decltype(Flags)>(GetProcAddress(module, "GetProbeFlags"));
        Cancels = reinterpret_cast<decltype(Cancels)>(GetProcAddress(module, "GetProbeCancels"));
        Pending = reinterpret_cast<decltype(Pending)>(GetProcAddress(module, "GetProbePending"));
        ProviderFlags=reinterpret_cast<decltype(ProviderFlags)>(GetProcAddress(module,"GetProviderProbeFlags"));
        ProviderMask=reinterpret_cast<decltype(ProviderMask)>(GetProcAddress(module,"SetProviderProbeMask"));
        if (!Value || !Flags || !Cancels || !Pending || !ProviderFlags || !ProviderMask) { FreeLibrary(module); module = nullptr; CHECK(false); }
    }
    ~ManagerEditProbe() { if (module) FreeLibrary(module); }
    ManagerEditProbe(const ManagerEditProbe&) = delete;
};
struct ManagerEditHost final : EditHost {
    SharedHistory history;
    EditStore* attached = nullptr;
    bool poisoned = false, entered = false;
    ManagerEditHost(StorageStore& storage, std::function<MmsResult()> gate) : history(storage, std::move(gate)) {}
    void AttachStore(EditStore* store) override { history.AttachStore(store); attached = store; }
    void DetachStore(EditStore* store) noexcept override { history.DetachStore(store); if (attached == store) attached = nullptr; }
    MmsResult Revision(uint64_t& revision) override { return history.Revision(revision); }
    MmsResult Enter() override { const auto result = history.Enter(); if (!result) entered = true; return result; }
    void Leave() noexcept override { entered = false; history.Leave(); }
    MmsResult Reserve(std::shared_ptr<const EditRecord> record, std::unique_ptr<EditPublication>& out) override {
        return history.Reserve(std::move(record), out);
    }
    MmsResult History(bool redo) override { return history.History(redo); }
    void Poison() noexcept override { poisoned = true; history.Poison(); }
};
struct ManagerEditCase {
    fs::path root;
    std::unique_ptr<Manager> manager;
    std::unique_ptr<ManagerEditHost> host;
    std::shared_ptr<unsigned> allowed = std::make_shared<unsigned>(0);
    struct Observations { unsigned published = 0; bool clean = true, throwNext = false; };
    std::shared_ptr<Observations> observations = std::make_shared<Observations>();
    explicit ManagerEditCase(const fs::path& path, bool attach = true) : root(path) {
        manager = std::make_unique<Manager>(root / "user", Log{});
        auto* m = manager.get(); const auto count = allowed;
        host = std::make_unique<ManagerEditHost>(manager->Storage(), [m, count] {
            if (m->InCallback() && !m->CanInvokeEdit()) return MMS_E_BUSY;
            if (m->CanInvokeEdit()) ++*count;
            return MMS_OK;
        });
        auto* h = host.get(); const auto observed = observations;
        manager->SetEditPublished([m, h, observed] {
            ++observed->published;
            observed->clean = observed->clean && h->attached && !h->attached->InCallback() && !h->entered && !m->CanInvokeEdit();
            if (observed->throwNext) { observed->throwNext = false; throw std::runtime_error("publication observer"); }
        });
        if (attach) CHECK(manager->AttachEditHost(*host) == MMS_OK);
    }
    ~ManagerEditCase() {
        if (!manager) return;
        manager->Stop();
        auto result = manager->DetachEditHost();
        if (result == MMS_E_FAILED) result = manager->DetachEditHost();
        if (result) { manager.release(); host.release(); return; }
        // Storage is owned by Manager and must outlive the external journal.
        host.reset(); manager.reset();
    }
    fs::path Package(const fs::path& dll, const std::string& id) {
        const auto package = root / "PLUGINS" / Wide(id);
        const auto entry = id + ".dll";
        fs::create_directories(package); fs::copy_file(dll, package / Wide(entry));
        Write(package / "plugin.json", ManifestText(id, entry)); return package / Wide(entry);
    }
    void Load(bool fail = false) {
        manager->Load(root / "PLUGINS"); const auto status = manager->Status(); CHECK(!status.empty());
        for (const auto& item : status) {
            if (item.stage != (fail ? "failed" : "started")) std::cout << "edit probe load " << item.id << " " << item.stage << " " << item.error << "\n";
            CHECK(item.stage == (fail ? "failed" : "started"));
        }
    }
    uint64_t Revision() { uint64_t revision = 0; CHECK(host->Revision(revision) == MMS_OK); return revision; }
    MmsResult Invoke(const std::string& id, const char* action, uint64_t revision = 0, MmsHandle transaction = 0) {
        UiClient visitor{&manager->Ui(), "example.edit-observer"}; auto api = manager->Ui().Api(visitor);
        const auto command = id + "." + action; MmsNamedValue args[2]{}; uint32_t count = 0;
        if (revision) { args[count].name = S("revision"); args[count].value.type = MMS_VALUE_INT; args[count++].value.value.signedValue = int64_t(revision); }
        if (transaction) { args[count].name = S("transaction"); args[count].value.type = MMS_VALUE_INT; args[count++].value.value.signedValue = int64_t(transaction); }
        return api.invoke(api.context, Text(command), {}, {args, count, sizeof(MmsNamedValue), 1});
    }
    void Close() { CHECK(manager->Stop()); CHECK(manager->DetachEditHost() == MMS_OK); CHECK(host->attached == nullptr); }
};
static void ManagerProviderCallbackChecks(const fs::path& root,const fs::path& dll){
    const std::string id="org.example.provider-probe";
    for(bool queued:{false,true})for(uint64_t phase:{1ull,2ull,4ull,8ull,16ull}){
        ManagerEditCase f(root/((queued?"provider-task-":"provider-callback-")+std::to_string(phase)));ManagerEditProbe p(f.Package(dll,id));f.Load();
        const auto token=f.manager->Storage().RestoreToken(id),revision=f.Revision();p.ProviderMask(phase);
        const auto result=f.Invoke(id,queued?(phase==4?"queue-throw":"queue"):(phase==4?"prepare-fail":"once"),revision);
        CHECK(result==(!queued&&phase==4?MMS_E_FAILED:MMS_OK));
        if(queued){f.manager->Drain();CHECK((p.Flags()&512)==(phase==4?0:512));}
        CHECK(p.ProviderFlags()==(phase|(phase<<8))&&f.manager->Storage().RestoreToken(id)==token);
        CHECK(p.Value()==(phase==4?0:1)&&f.Revision()==revision+(phase==4?0:1)&&f.host->history.UndoCount()==(phase==4?0:1));
        CHECK(!f.host->attached->HasPending()&&f.observations->published==(phase==4?0u:1u)&&f.observations->clean);
        if(phase==4)CHECK(p.Cancels()==1);
        p.ProviderMask(0);
        if(phase!=4){CHECK(f.Invoke(id,"undo")==MMS_OK&&p.Value()==0);CHECK(f.Invoke(id,"redo")==MMS_OK&&p.Value()==1);}
        f.Close();
    }
    {
        ManagerEditCase f(root/"provider-between-phases");ManagerEditProbe p(f.Package(dll,id));f.Load();
        CHECK(f.Invoke(id,"prepared",f.Revision())==MMS_OK);const auto token=f.manager->Storage().RestoreToken(id),revision=f.Revision();
        CHECK(f.Invoke(id,"replace-provider")==MMS_OK&&f.manager->Storage().RestoreToken(id)!=token&&f.host->attached->HasPending());
        CHECK(f.Invoke(id,"commit-pending")==MMS_E_STALE&&p.Cancels()==1&&p.Value()==0&&f.Revision()==revision&&!f.host->attached->HasPending());
        CHECK(!f.host->history.UndoCount()&&!f.observations->published);f.Close();
    }
    std::cout<<"provider mutations rejected inside five edit callbacks; between-phase replacement remains checked PASS\n";
}
static void ManagerEditChecks(const fs::path& root, const fs::path& dll) {
    const std::string id = "org.example.edit-probe";
    {
        ManagerEditCase f(root / "edit-default", false); ManagerEditProbe p(f.Package(dll, id)); f.Load();
        CHECK((p.Flags() & 3) == 2); CHECK(f.Invoke(id, "once") == MMS_E_NOT_READY);
        CHECK(f.manager->AttachEditHost(*f.host) == MMS_E_CONFLICT);
        CHECK(!f.manager->CanInvokeEdit() && f.host->attached == nullptr); f.Close();
    }
    {
        ManagerEditCase f(root / "edit-commands"); ManagerEditProbe p(f.Package(dll, id));
        CHECK(f.host->attached != nullptr); CHECK(f.manager->AttachEditHost(*f.host) == MMS_E_CONFLICT);
        MmsResult attach = MMS_OK, detach = MMS_OK;
        std::thread worker([&] { attach = f.manager->AttachEditHost(*f.host); detach = f.manager->DetachEditHost(); }); worker.join();
        CHECK(attach == MMS_E_THREAD && detach == MMS_E_THREAD && f.host->attached != nullptr);
        f.Load(); CHECK((p.Flags() & (1 | 4 | 4096)) == (1 | 4 | 4096));
        CHECK(!f.manager->CanInvokeEdit() && !f.manager->InCallback());
        {
            std::unique_ptr<SharedHistory::ResetTransaction> reset;
            CHECK(f.host->history.PrepareReset(reset) == MMS_OK);
            CHECK(f.Invoke(id, "once", f.Revision()) == MMS_E_BUSY);
            CHECK(p.Pending() == 0 && !f.host->attached->HasPending() && f.observations->published == 0);
        }
        CHECK(f.Invoke(id, "once", f.Revision()) == MMS_OK); CHECK(p.Value() == 1 && f.host->history.UndoCount() == 1);
        CHECK(f.observations->published == 1 && f.observations->clean);
        CHECK(f.Invoke(id, "twice", f.Revision()) == MMS_OK); CHECK(p.Value() == 3 && f.host->history.UndoCount() == 3);
        CHECK(f.Invoke(id, "undo") == MMS_OK); CHECK(p.Value() == 2 && f.host->history.UndoCount() == 2 && f.host->history.RedoCount() == 1);
        CHECK(f.Invoke(id, "redo") == MMS_OK); CHECK(p.Value() == 3 && f.host->history.UndoCount() == 3 && f.host->history.RedoCount() == 0);
        CHECK(f.observations->published == 5 && f.observations->clean);
        CHECK(f.Invoke(id, "nested") == MMS_OK);
        constexpr uint64_t denied = 4 | 8 | 16 | 32 | 64 | 128 | 2048 | 4096 | 16384 | 32768;
        CHECK((p.Flags() & denied) == denied); CHECK(p.Value() == 3 && !f.host->attached->HasPending());
        CHECK(*f.allowed >= 11 && !f.manager->CanInvokeEdit());
        const auto revision = f.Revision(); const auto undo = f.host->history.UndoCount();
        CHECK(f.Invoke(id, "once", revision - 1) == MMS_E_STALE);
        CHECK(p.Value() == 3 && f.Revision() == revision && f.host->history.UndoCount() == undo);
        int cancels = p.Cancels(); CHECK(f.Invoke(id, "prepare-fail", revision) == MMS_E_FAILED);
        CHECK(p.Cancels() == ++cancels && !f.host->attached->HasPending() && p.Pending() == 0);
        CHECK(f.Invoke(id, "throw", revision) == MMS_E_FAILED);
        CHECK(p.Cancels() == ++cancels && !f.host->attached->HasPending() && p.Value() == 3);
        CHECK(f.Revision() == revision && f.host->history.UndoCount() == undo);
        CHECK(f.observations->published == 5);
        StoredRecords saved; CHECK(f.manager->Storage().Capture(saved) == MMS_OK && saved.size() == 1);
        CHECK(f.manager->Storage().PrepareRestore(saved) == MMS_OK);
        CHECK(f.manager->DetachEditHost() == MMS_E_BUSY && !f.manager->Stop());
        CHECK(f.Invoke(id, "once", revision) == MMS_E_BUSY && p.Value() == 3);
        f.manager->Storage().Cancel();
        f.observations->throwNext = true;
        CHECK(f.Invoke(id, "queue", f.Revision()) == MMS_OK); CHECK(p.Value() == 3); f.manager->Drain();
        CHECK(p.Value() == 4 && (p.Flags() & 512) && f.host->history.UndoCount() == undo + 1);
        CHECK(f.observations->published == 6 && f.observations->clean && !f.observations->throwNext);
        CHECK(f.Invoke(id, "queue-throw", f.Revision()) == MMS_OK); f.manager->Drain();
        CHECK(p.Value() == 4 && p.Cancels() == ++cancels && !f.host->attached->HasPending());
        CHECK(f.observations->published == 6);
        CHECK(f.Invoke(id, "queue", f.Revision()) == MMS_OK); f.Close();
        CHECK((p.Flags() & (256 | 1024 | 8192)) == (256 | 1024 | 8192)); CHECK(p.Cancels() == cancels);
    }
    for (const char* pending : {"pending", "prepared"}) {
        ManagerEditCase f(root / (std::string("edit-stop-") + pending)); ManagerEditProbe p(f.Package(dll, id)); f.Load();
        CHECK(f.Invoke(id, pending, f.Revision()) == MMS_OK && p.Pending() != 0);
        CHECK(f.observations->published == 0);
        CHECK(f.host->attached->HasPending() && f.host->history.Reset() == MMS_E_BUSY);
        std::unique_ptr<SharedHistory::ResetTransaction> reset; CHECK(f.host->history.PrepareReset(reset) == MMS_E_BUSY && !reset);
        CHECK(f.manager->Stop()); CHECK(p.Cancels() == 1 && (p.Flags() & (256 | 8192)) == (256 | 8192));
        CHECK(!f.host->attached->HasPending()); CHECK(f.manager->Storage().RestoreToken(id) == 0);
        CHECK(f.manager->DetachEditHost() == MMS_OK && !f.host->attached);
        CHECK(f.host->history.Reset() == MMS_OK);
    }
    {
        ManagerEditCase f(root / "edit-multiple-owners"); const std::string a = "org.example.edit-a", b = "org.example.edit-b";
        ManagerEditProbe pa(f.Package(dll, a)), pb(f.Package(dll, b)); f.Load();
        CHECK(f.Invoke(a, "pending", f.Revision()) == MMS_OK); const auto transaction = pa.Pending(); CHECK(transaction != 0);
        CHECK(f.Invoke(b, "join-fail", f.Revision(), transaction) == MMS_E_FAILED);
        CHECK(pa.Pending() == transaction && pa.Cancels() == 0 && pb.Cancels() == 0 && f.host->attached->HasPending());
        CHECK(f.host->history.Reset() == MMS_E_BUSY);
        CHECK(f.Invoke(a, "finish") == MMS_OK && pa.Value() == 1 && pb.Value() == 1);
        CHECK(!f.host->attached->HasPending() && f.host->history.UndoCount() == 1);
        CHECK(f.Invoke(b, "undo") == MMS_OK && pa.Value() == 0 && pb.Value() == 0);
        CHECK(f.Invoke(a, "redo") == MMS_OK && pa.Value() == 1 && pb.Value() == 1); f.Close();
    }
    {
        ManagerEditCase f(root / "edit-detach"); ManagerEditProbe p(f.Package(dll, id)); f.Load();
        CHECK(f.Invoke(id, "prepared", f.Revision()) == MMS_OK);
        CHECK(f.manager->DetachEditHost() == MMS_OK && !f.host->attached && p.Cancels() == 1);
        // The plugin intentionally retains its negotiated table. Its context
        // remains valid, but detached calls cannot dereference the old store.
        CHECK(f.Invoke(id, "once") == MMS_E_NOT_READY && p.Value() == 0);
        CHECK(f.manager->AttachEditHost(*f.host) == MMS_E_CONFLICT); f.Close();
    }
    {
        ManagerEditCase f(root / "edit-detach-failure"); ManagerEditProbe p(f.Package(dll, id)); f.Load();
        CHECK(f.Invoke(id, "cancel-throw-pending", f.Revision()) == MMS_OK);
        CHECK(f.manager->DetachEditHost() == MMS_E_FAILED && f.host->attached && !f.host->attached->HasPending());
        CHECK(f.host->poisoned && p.Cancels() == 1 && p.Pending() == 0);
        CHECK(f.Invoke(id, "once") == MMS_E_NOT_READY && p.Value() == 0);
        CHECK(f.manager->DetachEditHost() == MMS_OK && !f.host->attached && p.Cancels() == 1); f.Close();
    }
    {
        ManagerEditCase f(root / "edit-load-failure"); const std::string failedId = "org.example.failstart";
        ManagerEditProbe p(f.Package(dll, failedId)); f.Load(true);
        CHECK((p.Flags() & (1 | 4 | 256)) == (1 | 4 | 256));
        CHECK(!f.host->attached->HasPending() && f.manager->Storage().RestoreToken(failedId) == 0 && f.manager->Ui().commands.empty()); f.Close();
    }
    ManagerProviderCallbackChecks(root,dll);
    std::cout << "manager edit callback/lifetime checks PASS\n";
}
