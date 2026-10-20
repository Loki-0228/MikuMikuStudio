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
    void (*PropertyMode)(uint32_t)=nullptr;
    uint64_t (*PropertyFlags)()=nullptr,(*PropertyCalls)()=nullptr,(*PropertyCandidates)()=nullptr,(*PropertyOther)()=nullptr;
    MmsResult (*PropertyFailure)()=nullptr;
    explicit ManagerEditProbe(const fs::path& path) {
        module = LoadLibraryW(path.c_str()); CHECK(module != nullptr);
        Value = reinterpret_cast<decltype(Value)>(GetProcAddress(module, "GetProbeValue"));
        Flags = reinterpret_cast<decltype(Flags)>(GetProcAddress(module, "GetProbeFlags"));
        Cancels = reinterpret_cast<decltype(Cancels)>(GetProcAddress(module, "GetProbeCancels"));
        Pending = reinterpret_cast<decltype(Pending)>(GetProcAddress(module, "GetProbePending"));
        ProviderFlags=reinterpret_cast<decltype(ProviderFlags)>(GetProcAddress(module,"GetProviderProbeFlags"));
        ProviderMask=reinterpret_cast<decltype(ProviderMask)>(GetProcAddress(module,"SetProviderProbeMask"));
        PropertyMode=reinterpret_cast<decltype(PropertyMode)>(GetProcAddress(module,"SetPropertyStageMode"));
        PropertyFlags=reinterpret_cast<decltype(PropertyFlags)>(GetProcAddress(module,"GetPropertyStageFlags"));
        PropertyCalls=reinterpret_cast<decltype(PropertyCalls)>(GetProcAddress(module,"GetPropertyStageCalls"));
        PropertyCandidates=reinterpret_cast<decltype(PropertyCandidates)>(GetProcAddress(module,"GetPropertyCandidateCount"));
        PropertyOther=reinterpret_cast<decltype(PropertyOther)>(GetProcAddress(module,"GetPropertyOther"));
        PropertyFailure=reinterpret_cast<decltype(PropertyFailure)>(GetProcAddress(module,"GetPropertyStageFailure"));
        if (!Value || !Flags || !Cancels || !Pending || !ProviderFlags || !ProviderMask || !PropertyMode || !PropertyFlags || !PropertyCalls || !PropertyCandidates || !PropertyOther || !PropertyFailure) { FreeLibrary(module); module = nullptr; CHECK(false); }
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
        CHECK(p.ProviderFlags()==(phase|(phase<<8)|(phase<<16))&&f.manager->Storage().RestoreToken(id)==token);
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
static void ManagerPropertyStageChecks(const fs::path& root,const fs::path& dll){
    const std::string id="org.example.property-probe";
    // Flags 0..13 prove every forbidden operation was attempted, while the
    // exact join succeeded. The upper flags prove nested read/state ran.
    constexpr uint64_t probes=16383|65536|131072;
    {
        ManagerEditCase f(root/"property-default",false);ManagerEditProbe p(f.Package(dll,id));f.Load();
        CHECK(f.Invoke(id,"property-once")==MMS_E_NOT_READY&&p.PropertyCalls()==0);
        const auto record=f.manager->Ui().properties.at(id+".editable");auto api=f.manager->Ui().Api(*record->owner);
        MmsValue value{};value.type=MMS_VALUE_INT;value.value.signedValue=7;
        CHECK(api.stageProperty(api.context,1,Text(id+".editable"),{},&value)==MMS_E_NOT_READY&&p.PropertyCalls()==0);
        f.Close();
    }
    {
        ManagerEditCase f(root/"property-coalesce");ManagerEditProbe p(f.Package(dll,id));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(id,"property-once",revision)==MMS_OK);
        CHECK(p.PropertyCalls()==2&&p.PropertyFlags()==probes&&p.PropertyCandidates()==0&&p.Value()==9);
        CHECK(f.Revision()==revision+1&&f.host->history.UndoCount()==1&&f.observations->published==1&&f.observations->clean);
        CHECK(f.Invoke(id,"undo")==MMS_OK&&p.Value()==0);CHECK(f.Invoke(id,"redo")==MMS_OK&&p.Value()==9);
        CHECK(f.Invoke(id,"once",f.Revision())==MMS_OK&&p.Value()==10);
        CHECK(f.Invoke(id,"property-once",f.Revision())==MMS_OK&&p.Value()==9&&f.host->history.UndoCount()==3);
        CHECK(f.Invoke(id,"undo")==MMS_OK&&p.Value()==10);CHECK(f.Invoke(id,"redo")==MMS_OK&&p.Value()==9);
        CHECK(p.PropertyCalls()==4&&!f.host->attached->HasPending()&&p.PropertyCandidates()==0);f.Close();
    }
    {
        ManagerEditCase f(root/"property-pending-cancel");ManagerEditProbe p(f.Package(dll,id));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(id,"property-pending",revision)==MMS_OK);const auto tx=p.Pending();CHECK(tx&&p.PropertyCandidates()==1&&p.Value()==0);
        CHECK(f.Invoke(id,"property-stage",revision,tx)==MMS_OK&&p.PropertyCalls()==2&&p.Value()==0);
        CHECK(f.Invoke(id,"prepare-pending",revision,tx)==MMS_OK&&p.PropertyCandidates()==1&&p.Value()==0);
        CHECK(!f.host->history.UndoCount()&&f.Revision()==revision&&!f.observations->published);
        CHECK(f.Invoke(id,"property-cancel",revision,tx)==MMS_OK&&p.Cancels()==1&&p.PropertyCandidates()==0&&!p.Pending());
        CHECK(!f.host->attached->HasPending()&&p.Value()==0&&f.Revision()==revision&&!f.host->history.UndoCount());
        CHECK(f.Invoke(id,"property-pending",revision)==MMS_OK&&f.Invoke(id,"finish")==MMS_OK&&p.Value()==7);
        CHECK(f.Invoke(id,"undo")==MMS_OK&&p.Value()==0);CHECK(f.Invoke(id,"redo")==MMS_OK&&p.Value()==7);f.Close();
    }
    for(uint32_t mode:{1u,2u,3u,4u}){
        ManagerEditCase f(root/("property-failed-"+std::to_string(mode)));ManagerEditProbe p(f.Package(dll,id));f.Load();
        const auto revision=f.Revision();p.PropertyMode(mode);
        CHECK(f.Invoke(id,"property-once",revision)==(mode==1||mode==3?MMS_E_FAILED:MMS_E_ARGUMENT));
        CHECK(p.PropertyCalls()==1&&p.PropertyFlags()==(mode==1||mode==4?probes&~uint64_t(8192):probes));
        CHECK(p.Cancels()==(mode==2||mode==3?1:0)&&p.PropertyCandidates()==0&&p.Value()==0);
        CHECK(!f.host->attached->HasPending()&&!f.host->history.UndoCount()&&!f.observations->published&&f.Revision()==revision);
        p.PropertyMode(0);CHECK(f.Invoke(id,"property-once",revision)==MMS_OK&&p.Value()==9&&p.PropertyCalls()==2);f.Close();
    }
    {
        ManagerEditCase f(root/"property-queue");ManagerEditProbe p(f.Package(dll,id));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(id,"property-queue",revision)==MMS_OK&&p.Value()==0&&p.PropertyCalls()==0&&!f.host->history.UndoCount());
        f.manager->Drain();CHECK(p.PropertyCalls()==2&&p.PropertyFlags()==(probes|16384)&&p.Value()==9&&p.PropertyCandidates()==0);
        CHECK(f.Revision()==revision+1&&f.host->history.UndoCount()==1&&f.observations->published==1&&f.observations->clean);
        CHECK(f.Invoke(id,"undo")==MMS_OK&&p.Value()==0);CHECK(f.Invoke(id,"redo")==MMS_OK&&p.Value()==9);f.Close();
    }
    {
        ManagerEditCase f(root/"property-exact-failure");ManagerEditProbe p(f.Package(dll,id));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(id,"property-fail-keep",revision)==MMS_OK&&p.PropertyFailure()==MMS_E_ARGUMENT);
        CHECK(p.PropertyCalls()==3&&(p.PropertyFlags()&32768)&&p.Pending()&&p.PropertyOther()&&p.Pending()!=p.PropertyOther());
        CHECK(p.Cancels()==1&&p.PropertyCandidates()==1&&p.Value()==0&&f.host->attached->HasPending());
        CHECK(f.Revision()==revision&&!f.host->history.UndoCount()&&!f.observations->published);
        CHECK(f.Invoke(id,"property-try",revision,p.PropertyOther())==MMS_OK&&p.PropertyFailure()==MMS_E_STALE);
        CHECK(p.PropertyCandidates()==1&&p.Cancels()==1&&p.PropertyCalls()==3);
        CHECK(f.Invoke(id,"finish")==MMS_OK&&p.Value()==7&&p.PropertyCandidates()==0&&f.host->history.UndoCount()==1);
        CHECK(f.Invoke(id,"undo")==MMS_OK&&p.Value()==0);CHECK(f.Invoke(id,"redo")==MMS_OK&&p.Value()==7);f.Close();
    }
    {
        ManagerEditCase f(root/"property-empty-transaction");ManagerEditProbe p(f.Package(dll,id));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(id,"property-empty",revision)==MMS_OK&&p.Pending()&&p.PropertyCandidates()==0);
        CHECK(f.Invoke(id,"property-stage",revision)==MMS_OK&&p.PropertyCandidates()==1&&p.Value()==0);
        CHECK(f.Invoke(id,"finish")==MMS_OK&&p.Value()==9&&f.host->history.UndoCount()==1);f.Close();
    }
    // Replacing the same public handle creates a new registration identity.
    for(bool prepared:{false,true}){
        ManagerEditCase f(root/(prepared?"property-replaced-commit":"property-replaced-prepare"));ManagerEditProbe p(f.Package(dll,id));f.Load();
        const auto revision=f.Revision();CHECK(f.Invoke(id,"property-pending",revision)==MMS_OK);
        if(prepared)CHECK(f.Invoke(id,"prepare-pending")==MMS_OK);
        const auto record=f.manager->Ui().properties.at(id+".editable");auto api=f.manager->Ui().Api(*record->owner);auto desc=record->desc;
        desc.info.label=S("replacement registration");CHECK(api.updateProperty(api.context,record->handle,&desc)==MMS_OK);
        const auto replacement=f.manager->Ui().properties.at(id+".editable");CHECK(replacement.get()!=record.get()&&replacement->handle==record->handle);
        CHECK(f.Invoke(id,prepared?"commit-pending":"prepare-pending")==MMS_E_STALE);
        CHECK(p.Cancels()==1&&p.PropertyCandidates()==0&&p.Value()==0&&!f.host->attached->HasPending()&&f.Revision()==revision);
        CHECK(!f.host->history.UndoCount()&&!f.observations->published);f.Close();
    }
    {
        ManagerEditCase f(root/"property-unrelated-ui");ManagerEditProbe p(f.Package(dll,id));f.Load();
        CHECK(f.Invoke(id,"property-pending",f.Revision())==MMS_OK);
        for(bool prepared:{false,true}){
            if(prepared)CHECK(f.Invoke(id,"prepare-pending")==MMS_OK);
            const auto record=f.manager->Ui().properties.at(id+".value");auto api=f.manager->Ui().Api(*record->owner);auto desc=record->desc;
            desc.info.label=prepared?S("after prepare"):S("before prepare");CHECK(api.updateProperty(api.context,record->handle,&desc)==MMS_OK);
        }
        CHECK(f.Invoke(id,"commit-pending")==MMS_OK&&p.Value()==7&&!p.Cancels()&&p.PropertyCandidates()==0&&f.host->history.UndoCount()==1);f.Close();
    }
    {
        ManagerEditCase f(root/"property-replaced-serializer");ManagerEditProbe p(f.Package(dll,id));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(id,"property-pending",revision)==MMS_OK&&f.Invoke(id,"prepare-pending")==MMS_OK);
        const auto token=f.manager->Storage().RestoreToken(id);CHECK(f.Invoke(id,"replace-provider")==MMS_OK&&f.manager->Storage().RestoreToken(id)!=token);
        CHECK(f.Invoke(id,"commit-pending")==MMS_E_STALE&&p.Cancels()==1&&p.PropertyCandidates()==0&&p.Value()==0);
        CHECK(!f.host->attached->HasPending()&&!f.host->history.UndoCount()&&f.Revision()==revision&&!f.observations->published);f.Close();
    }
    {
        ManagerEditCase f(root/"property-owner-target");const std::string a="org.example.property-a",b="org.example.property-b";
        ManagerEditProbe pa(f.Package(dll,a)),pb(f.Package(dll,b));f.Load();const auto revision=f.Revision();
        CHECK(f.Invoke(a,"property-pending",revision)==MMS_OK&&f.Invoke(b,"property-empty",revision)==MMS_OK);const auto tx=pa.Pending();
        const auto ar=f.manager->Ui().properties.at(a+".editable"),br=f.manager->Ui().properties.at(b+".editable");
        auto aa=f.manager->Ui().Api(*ar->owner),ba=f.manager->Ui().Api(*br->owner);MmsValue value{};value.type=MMS_VALUE_INT;value.value.signedValue=9;
        CHECK(ba.stageProperty(ba.context,tx,Text(a+".editable"),{},&value)==MMS_E_ARGUMENT);
        MmsObjectRef target{};target.id.lo=1;CHECK(aa.stageProperty(aa.context,tx,Text(a+".editable"),target,&value)==MMS_E_ARGUMENT);
        CHECK(aa.stageProperty(aa.context,tx,Text(a+".editable"),{},&value)==MMS_E_BUSY); // No command/task authorization.
        CHECK(pa.PropertyCalls()==1&&pa.PropertyCandidates()==1&&!pa.Cancels()&&pb.PropertyCalls()==0);
        CHECK(f.Invoke(b,"property-stage",revision,tx)==MMS_E_STALE);
        CHECK(pa.Pending()==tx&&pa.PropertyCandidates()==1&&!pa.Cancels()&&pb.PropertyCalls()==0);
        CHECK(f.Invoke(a,"finish")==MMS_OK&&pa.Value()==7&&pb.Value()==0&&f.host->history.UndoCount()==1);f.Close();
    }
    for(bool prepared:{false,true}){
        ManagerEditCase f(root/(prepared?"property-stop-prepared":"property-stop-staged"));ManagerEditProbe p(f.Package(dll,id));f.Load();
        CHECK(f.Invoke(id,"property-pending",f.Revision())==MMS_OK);if(prepared)CHECK(f.Invoke(id,"prepare-pending")==MMS_OK);
        CHECK(p.PropertyCandidates()==1&&p.Value()==0&&!f.host->history.UndoCount());f.Close();
        CHECK(p.Cancels()==1&&p.PropertyCandidates()==0&&p.PropertyCalls()==1);
    }
    {
        ManagerEditCase f(root/"property-cancel-violation");ManagerEditProbe p(f.Package(dll,id));f.Load();p.PropertyMode(5);
        CHECK(f.Invoke(id,"property-pending",f.Revision())==MMS_OK&&f.Invoke(id,"property-cancel")==MMS_OK);
        CHECK(p.Cancels()==1&&p.PropertyCandidates()==0&&p.Value()==0&&f.host->poisoned&&!f.host->attached->HasPending());
        CHECK(!f.host->history.UndoCount()&&!f.observations->published);
        CHECK(f.manager->DetachEditHost()==MMS_OK);f.Close();
    }
    std::cout<<"real DLL property stage/coalescing/authorization/identity/cancellation/history PASS\n";
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
    ManagerPropertyStageChecks(root,dll);
    std::cout << "manager edit callback/lifetime checks PASS\n";
}
