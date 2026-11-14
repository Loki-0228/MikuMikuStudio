// Included from storage_checks.inc in plugin_abi_tests. These exercise the
// real Windows disk adapter with a synthetic provider, not real MME.
namespace frozen_storage_checks {
namespace save = mikumikustudio::effect_save;
using mikumikustudio::pmm_paths::Effect;

static std::unique_ptr<const save::CapturedEffects> Capture(const fs::path& target,
    const std::string& bytes,save::SaveKind kind=save::SaveKind::Manual,bool wrongDigest=false) {
    save::Request request{9,{12,61,1,1,2,3,{5}},target.native(),Effect(target).native(),kind,true};
    save::CaptureCallbacks callbacks;
    callbacks.observe=[&]{return std::optional<save::CaptureIdentity>(request.identity);};
    callbacks.freeze=[&](const auto&,save::CaptureSink& sink) {
        CHECK(sink.Begin(bytes.size()) && sink.Append(bytes.data(),bytes.size()));
        return save::FreezeResult{save::FreezeStatus::Frozen,request.identity};
    };
    callbacks.validate=[&](const auto&,save::EffectBytes payload) {
        return std::string_view(reinterpret_cast<const char*>(payload.data),payload.size)==bytes;
    };
    callbacks.sha256=[&](save::EffectBytes payload,save::EffectDigest& digest) {
        digest=FrozenEffectSha256(payload);
        if (wrongDigest) digest[0]^=1;
        return true;
    };
    save::CaptureAdmission admission;
    auto attempt=admission.Capture(request,1024,callbacks);
    CHECK(attempt.status==save::CaptureStatus::Captured && attempt.snapshot);
    return std::move(attempt.snapshot);
}

static void Run(const fs::path& root) {
    const auto directory=root/L"frozen-pair-recovery";
    fs::create_directories(directory);
    const auto staged=directory/L"stage.tmp";
    const auto journal=[](const fs::path& target) {return fs::path(target.native()+L".mms-save.json");};
    const std::string oldEmm="[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = old.pmx\r\n[Effect]\r\nPmd1 = old.fx\r\n";
    const std::string newEmm="[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = captured.pmx\r\n[Effect]\r\nPmd1 = captured.fx\r\n";
    for (unsigned stop=0;stop<=9;++stop) {
        const auto target=directory/(L"cut-"+std::to_wstring(stop)+L".pmm");
        PluginProject data;
        Write(staged,"old PMM FOV 31");
        data.records["example.a"]=StoredInt(31);
        PublishProjectPair(target,staged,data,true);
        Write(Effect(target),oldEmm);
        const auto previous=VersionBytes(target);
        // A historical companion is outside schema 4 and must stay intact.
        const auto prior=fs::path(target.native()+L"1");
        Write(prior,"independent earlier PMM");
        Write(Effect(prior),"independent earlier EMM");
        Write(staged,"captured PMM FOV 61");
        auto frozen=Capture(target,newEmm);
        data.records["example.a"]=StoredInt(61);
        bool hit=false;
        CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*frozen,&data,false,false,[&](unsigned point) {
            if(point==stop) { hit=true;throw std::runtime_error("owned SAVE-02 interruption"); }
        });}));
        CHECK(hit && fs::exists(journal(target))==(stop!=0));
        if (stop<=2) CHECK(VersionBytes(target)==previous);
        // While a current-project schema-4 record is present, .pmm1 remains
        // independently openable because this schema cannot touch retention.
        RecoverProjectPair(prior);
        CHECK(ReadFile(prior)=="independent earlier PMM" && ReadFile(Effect(prior))=="independent earlier EMM");
        const auto recovered=ReadPluginProject(target);
        CHECK(!fs::exists(journal(target)));
        if (stop<=1) CHECK(VersionBytes(target)==previous);
        else CHECK(ReadFile(target)=="captured PMM FOV 61" && ReadFile(Effect(target))==newEmm &&
            SavedInt(recovered.records.at("example.a"))==61);
        const auto first=VersionBytes(target);
        RecoverProjectPair(target);
        CHECK(VersionBytes(target)==first);
    }

    const auto target=directory/L"original.pmm";
    auto frozen=Capture(target,newEmm,save::SaveKind::Incremental);
    Write(staged,"captured original profile PMM FOV 61");
    Write(Effect(target),"external companion");
    CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*frozen,nullptr,true,false);}));
    CHECK(!fs::exists(target) && ReadFile(Effect(target))=="external companion" && !fs::exists(journal(target)));
    CHECK(DeleteFileW(Effect(target).c_str())!=FALSE);
    CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*frozen,nullptr,true,false,[&](unsigned point) {
        if(point==0) Write(Effect(target),"late external companion");
    });}));
    CHECK(!fs::exists(target) && ReadFile(Effect(target))=="late external companion" && !fs::exists(journal(target)));
    CHECK(DeleteFileW(Effect(target).c_str())!=FALSE);
    const auto lock=CreateFileW((target.native()+L".mms-save.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,
        nullptr,CREATE_NEW,FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    CHECK(lock!=INVALID_HANDLE_VALUE);
    PluginProject legacyData;
    CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*frozen,nullptr,true,false);}));
    CHECK(Throws([&]{PublishProjectPair(target,staged,legacyData,true);}));
    CHECK(CloseHandle(lock)!=FALSE);
    CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*frozen,nullptr,true,true);}));
    auto foreign=Capture(directory/L"other.pmm",newEmm);
    CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*foreign,nullptr,true,false);}));
    auto corrupted=Capture(target,newEmm,save::SaveKind::Incremental,true);
    CHECK(Throws([&]{PublishFrozenProjectPair(target,staged,*corrupted,nullptr,true,false);}));
    CHECK(!fs::exists(target) && !fs::exists(Effect(target)) && !fs::exists(journal(target)));
    const auto proof=PublishFrozenProjectPair(target,staged,*frozen,nullptr,true,false);
    CHECK(proof.request.requestId==frozen->GetRequest().requestId && proof.pmmSha256==FileHash(target) &&
        proof.emmSha256==FrozenEffectSha256(frozen->Bytes()) && ReadFile(Effect(target))==newEmm &&
        !fs::exists(PluginSidecar(target)));
    std::cout<<"SAVE-02 Windows disk transaction and legacy lock coexistence PASS\n";
}
} // namespace frozen_storage_checks
