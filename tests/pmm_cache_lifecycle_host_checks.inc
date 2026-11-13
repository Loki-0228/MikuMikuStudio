// Included in the hidden PMM host fixture after Normal and LegacyRelocationOpen.
namespace host_cache {
namespace cache = cache_manager;
fs::path base, payload;
bool touchTail = false, insideTailTouch = false, poisonLivePath = false, denyProof = false, throwReset = false;
unsigned proofDenials = 0;
struct ResetFault {};
struct Finishes { unsigned loaded=0, rejected=0, failed=0, reset=0; };
Finishes* finishes = nullptr;
bool firstUseAfterReset = false;
DWORD leaseAtResetCompletion = ERROR_INVALID_FUNCTION;

void SelectRoot(const fs::path& parent) {
    const auto root = parent / L"Cache";
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR", root.c_str()) &&
          SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS", (parent/L"settings.json").c_str()) &&
          SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR", (root/L"MME").c_str()) &&
          SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR", (root/L"Textures").c_str()),
          "manager, settings and both disk producers use only fixture paths");
}
#if !MMD_ORIGINAL_PROFILE
decltype(&CreateFileW) realCreate = nullptr;
HANDLE WINAPI OpenFile(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templ) {
    if (insideTailTouch && denyProof && path && access == FILE_READ_ATTRIBUTES && creation == OPEN_EXISTING &&
        (flags & FILE_FLAG_OPEN_REPARSE_POINT) && !_wcsicmp(path,payload.c_str())) {
        ++proofDenials;
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return realCreate(path,access,share,security,creation,flags,templ);
}
DWORD DeleteProbe();
decltype(&cache::FinishScene) realFinish = nullptr;
bool ObserveFinish(cache::SceneToken t,cache::SceneOutcome outcome,const wchar_t* path) noexcept {
    const bool applied = realFinish(t,outcome,path);
    if (finishes) {
        if (outcome == cache::SceneOutcome::Loaded) ++(applied ? finishes->loaded : finishes->rejected);
        else if (applied && outcome == cache::SceneOutcome::Failed) ++finishes->failed;
        else if (applied && outcome == cache::SceneOutcome::ResetComplete) {
            ++finishes->reset;
            if (firstUseAfterReset) {
                leaseAtResetCompletion=DeleteProbe();
                cache::Touch(payload); // anonymous use before the enclosing loader unwinds
            }
        }
    }
    return applied;
}
decltype(&PostLanguageSweep) realSweep = nullptr;
void Sweep(MMDApp* value) {
    if (throwReset && value == app) { throwReset=false; throw ResetFault{}; }
    realSweep(value);
}
void StoreFixture() {
    payload = cache::Root()/L"Textures"/L"shared-texture"/(std::wstring(64,L'7') + L".mtc");
    const auto tmp = fs::path(payload.wstring()+L".tmp-host");
    fs::create_directories(payload.parent_path());
    std::uint32_t words[1024]{0x4354504d,2,4096-76};
    cache::WriteReservation write(payload,sizeof(words));
    Check(bool(write),"host owns a real cache publication reservation");
    const HANDLE file=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE && write.TrackTemporary(tmp),"identify host's own empty temporary");
    DWORD n=0;
    Check(WriteFile(file,words,sizeof(words),&n,nullptr) && n==sizeof(words) && CloseHandle(file) &&
        MoveFileExW(tmp.c_str(),payload.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) &&
        write.Commit(),"publish registered host texture payload");
}
DWORD DeleteProbe() {
    const HANDLE file=CreateFileW(payload.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(file);return ERROR_SUCCESS;
}
void Current(const fs::path& expected, bool used = false) {
    static unsigned inspection = 0;
    ++inspection;
    const auto view=cache::Inspect();
    if (!view.error.empty()) {
        std::fprintf(stderr, "DIAG cache inspection=%u root=%ls expected=%ls payloadRequired=%d error=%ls\n",
            inspection, cache::Root().c_str(), expected.c_str(), used, view.error.c_str());
    }
    Check(view.error.empty(),"inspect healthy isolated cache after host boundary");
    unsigned count=0;
    for (const auto& row:view.projects) if (row.current) {
        ++count;
        Check(!expected.empty() && !_wcsicmp(row.path.c_str(),expected.c_str()) &&
            (!used || row.totalBytes>=4096),"actual host target and genuine parser-tail use own current row");
    }
    Check(count==(expected.empty()?0u:1u),"exactly the expected number of current cache owners");
}
#endif
}

void IsolateHostCache() {
    using namespace host_cache;
    base=directory/(L"cache-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(base),"create fresh private cache fixture before host startup");
    SelectRoot(base);
}
void CacheHostParserTail() {
    if (host_cache::touchTail) {
        host_cache::insideTailTouch = true;
        cache_manager::Touch(host_cache::payload);
        host_cache::insideTailTouch = false;
    }
}
void CacheHostFinalLocalize(MMDApp* value) {
    if (host_cache::poisonLivePath) wcscpy_s(value->EnvFileName(),256,L"late-localize-decoy.pmm");
}
void CacheHostCases(const fs::path& v1,const fs::path& v2,const mdl::CameraKey& camera,const mdl::LightKey& light) {
    using namespace host_cache;
    std::puts("STAGE real host cache Load/Reset boundaries");
    // Preserve the parent runner's policy for all pre-existing host cases.
    const wchar_t* modeName=L"MMS_CACHE_MEMORY_ONLY";
    SetLastError(ERROR_SUCCESS);
    const DWORD capacity=GetEnvironmentVariableW(modeName,nullptr,0);
    const bool present=capacity || GetLastError()==ERROR_SUCCESS;
    std::wstring previous(capacity,L'\0');
    if (capacity) {
        const DWORD n=GetEnvironmentVariableW(modeName,previous.data(),capacity);
        Check(n+1==capacity,"capture inherited cache mode");
        previous.resize(n);
    }
    const auto noDisk=base/L"no-disk";
    SelectRoot(noDisk);
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1"),"enable memory-only PMM load");
    Normal(v2,camera,light);
    Check(!fs::exists(noDisk),"memory-only load creates neither cache root nor settings");
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"0"),"resume isolated disk admission");
#if MMD_ORIGINAL_PROFILE
    Check(cache::Root().empty() && !fs::exists(noDisk),"original-profile host keeps disk cache disabled");
    SelectRoot(base);
#else
    Current(v2); // First disk access after the successful in-memory transition.
    SelectRoot(base);
    Hook(&CreateFileW,&OpenFile,reinterpret_cast<void**>(&realCreate));
    Hook(&cache::FinishScene,&ObserveFinish,reinterpret_cast<void**>(&realFinish));
    Hook(&PostLanguageSweep,&Sweep,reinterpret_cast<void**>(&realSweep));
    StoreFixture();
    touchTail=true;
    Normal(v1,camera,light); Current(v1,true);
    Check(DeleteProbe()==ERROR_SHARING_VIOLATION,"V1 holds its parser-tail cache lease before replacement");
    touchTail=false;
    Normal(v2,camera,light); Current(v2);
    Check(DeleteProbe()==ERROR_SUCCESS,"successful V2 open releases the V1-only cache lease");
    touchTail=true;

    Normal(v1,camera,light); Current(v1,true);
    LegacyRelocationOpen(v2,false); Current(v2,true);
    Check(DeleteProbe()==ERROR_SHARING_VIOLATION,"V2 retains the cache lease shared with V1");
    LegacyRelocationOpen(v1,true); Current(v1,true);
    Arm(v2,Fault::None,false);
    poisonLivePath=true;
    const bool loaded=LoadSceneFile(v2.c_str());
    poisonLivePath=false;
    Check(loaded && !std::wcscmp(app->EnvFileName(),L"late-localize-decoy.pmm"),"final localization changed the live path after parse");
    Finish(loaded,true);
    Current(v2,true);
    wcscpy_s(app->EnvFileName(),256,v2.c_str());
    Check(DeleteProbe()==ERROR_SHARING_VIOLATION,"accepted scene holds its real cache lease");

    Finishes events;
    finishes=&events;
    Check(plugins::PrepareProjectLoad(app,v1.c_str()),"hold a real load transaction before a second shell open");
    Check(!LoadSceneFile(v1.c_str()),"busy plugin preparation rejects shell before cache acceptance");
    plugins::CancelProjectLoad(app);
    Current(v2,true);
    Check(DeleteProbe()==ERROR_SHARING_VIOLATION && !events.loaded && !events.failed && !events.reset,
        "pre-accept refusal and plugin cancellation preserve the current cache scene");
    {
        auto* history=plugins::NativeHistory(app);
        std::unique_ptr<plugins::SharedHistory::ResetTransaction> held;
        Check(history && history->PrepareReset(held)==MMS_OK && held && !ResetAppState(app),"genuine history refusal prevents reset");
        Current(v2,true);
        Check(DeleteProbe()==ERROR_SHARING_VIOLATION && !events.failed && !events.reset,"refusal preserves owner and lease");
    }
    // Reject the real proof open inside a parser-tail Touch; the native parser
    // and the backend's Loaded/Failed transitions both run to their real result.
    denyProof=true;
    Normal(v1,camera,light);
    denyProof=false;
    Check(proofDenials && events.rejected==1 && events.failed==1 && !app->sceneRecoveryPending &&
        WriteThrough(RelocationTicket::Capture(*app)),"optional cache rejection isolates attribution while PMM and relocation succeed");
    Current({});
    Normal(v2,camera,light); Current(v2,true);

    events={};failFinalLocalize=true;
    Arm(v1,Fault::None,false);
    bool localizeThrew=false;
    try { (void)LoadSceneFile(v1.c_str()); } catch (const LocalizeFault&) { localizeThrew=true; }
    monitor=false;
    Check(localizeThrew && tailObserved && !inputOpen && events.failed==1 && !events.loaded,
        "parser success followed by shell localization failure cannot publish a cache owner");
    Current({});
    Normal(v2,camera,light); Current(v2,true);
    events={};throwReset=true;
    bool caught=false;
    try { (void)ResetAppState(app); } catch (const ResetFault&) { caught=true; }
    Check(caught && !throwReset && events.failed==1 && !events.reset,"exception after real reset acceptance isolates its token");
    Current({});
    Check(DeleteProbe()==ERROR_SHARING_VIOLATION,"partial reset retains manager protection");
    Check(ResetAppState(app) && events.reset==1 && DeleteProbe()==ERROR_SUCCESS,"full reset releases protection in the live process");

    Normal(v2,camera,light); Current(v2,true);
    const auto missing=directory/L"cache-abort-v2.pmm";
    WriteMissingV2(missing);
    V2RelocationDialogs abort{{{ProjectModelIssue::MissingFile,L"missing",2,{}}}};
    v2Relocation=&abort;
    events={};firstUseAfterReset=true;
    Arm(missing,Fault::None,false);
    const bool aborted=LoadSceneFile(missing.c_str());
    monitor=false;v2Relocation=nullptr;firstUseAfterReset=false;
    Check(!aborted && abort.next==1 && inputCloses==1 && !inputOpen && !app->sceneRecoveryPending &&
        events.failed==1 && events.reset==1 && !events.loaded && !events.rejected,
        "V2 model Abort takes a nested real reset, then outer load unwinds exactly once");
    Current({});
    Check(leaseAtResetCompletion==ERROR_SUCCESS && DeleteProbe()==ERROR_SHARING_VIOLATION,
        "nested reset retires the old lease and outer failure preserves the new anonymous use");
    finishes=nullptr;
    Normal(v1,camera,light); Current(v1,true);
    touchTail=false;
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1") && ResetAppState(app) &&
        DeleteProbe()==ERROR_SUCCESS,"real reset releases existing lease even in memory-only mode");
    Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"0"),"restore isolated disk mode");
    Current({});
    MH_DisableHook(reinterpret_cast<void*>(&PostLanguageSweep));
    MH_DisableHook(reinterpret_cast<void*>(&cache::FinishScene));
    MH_DisableHook(reinterpret_cast<void*>(&CreateFileW));
#endif
    Check(SetEnvironmentVariableW(modeName,present ? previous.c_str() : nullptr),"restore inherited cache policy");
}
