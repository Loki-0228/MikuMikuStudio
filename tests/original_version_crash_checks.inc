// File-level original-profile publication and the loader's existing pre-read
// gate. No scene parser, renderer, plugin sidecar or replacement recovery API.
#include "../src/io/pmm_save_versions.hpp"
#include "save_journal_fixture.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

namespace original_version_crash {
namespace fs = std::filesystem;
constexpr DWORD timeout = 60000, killed = 73;
constexpr char oldestPmm[] = "oldest original PMM payload";
constexpr char oldestEmm[] = "oldest original EMM payload";
constexpr char middlePmm[] = "middle original PMM payload";
constexpr char middleEmm[] = "middle original EMM payload";
constexpr char freshPmm[] = "new interrupted original PMM payload";
using Level = std::array<std::optional<std::string>, 2>;
using Chain = std::array<Level, 4>;

void Need(bool ok, const char* action) {
    if (!ok) throw std::runtime_error(std::string(action) + " (Win32 " + std::to_string(GetLastError()) + ")");
}
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) { Need(h && h != INVALID_HANDLE_VALUE, "open fixture handle"); }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Need(bool(file), "read fixture file");
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Need(!file.bad(), "read complete fixture file");
    return bytes;
}
void Write(const fs::path& path, const std::string& bytes, bool replace = false) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           replace ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD count = 0;
    Need(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size(),
         "write fixture file");
    Need(FlushFileBuffers(file.value) != FALSE, "flush fixture file");
}
std::wstring Nonce() {
    std::array<unsigned char, 16> bytes{};
    Need(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
         "generate fixture nonce");
    std::wstring result;
    for (auto byte : bytes) { result += L"0123456789abcdef"[byte >> 4]; result += L"0123456789abcdef"[byte & 15]; }
    return result;
}
std::wstring EventName(const std::wstring& id) { return L"Local\\MMS-original-version-" + id; }
Chain Observe(fs::path target) {
    Chain result{};
    for (auto& level : result) {
        const std::array<fs::path, 2> members{target, pmm_paths::Effect(target)};
        for (size_t i = 0; i < members.size(); ++i) {
            bool present = false;
            Need(pmm_versions::Exists(members[i], present), "inspect fixture member");
            if (present) level[i] = Read(members[i]);
        }
        Need(!fs::exists(target.native() + L".mms-plugins.json"), "original chain has no plugin sidecars");
        target += L"1";
    }
    return result;
}
Chain Before() { return {Level{middlePmm, middleEmm}, Level{oldestPmm, oldestEmm}, Level{}, Level{}}; }
Chain Forward() { return {Level{freshPmm, middleEmm}, Level{middlePmm, middleEmm}, Level{oldestPmm, oldestEmm}, Level{}}; }
Chain ForwardPending() {
    Chain result=Forward();
    result[0][0]=middlePmm;
    return result;
}
fs::path ForwardStage(const fs::path& target,const std::string& record) {
    const auto j=save_journal_fixture::Parse(record);
    Need(j.At("schemaVersion").UInt()==3 && j.At("members").String()=="pmm-emm" &&
         j.At("effectStage").UInt()==1 && j.At("decision").String()=="forward" &&
         !j.object.count("newSide") && !j.object.count("oldSide"),
         "require the original publisher's explicit durable forward decision");
    const auto id=j.At("saveId").String();
    Need(id.size()==32 && id.find_first_not_of("0123456789abcdef")==id.npos,"validate forward transaction ID");
    const auto& rows=j.At("versions").Items();
    Need(rows.size()==3,"forward journal records active, old 1 and vacant 11");
    for(const auto& row:rows)Need(row.Items().size()==2,"forward journal retains exactly PMM/EMM members");
    Need(j.At("oldMain").String()==rows[0].Items()[0].String() &&
         j.At("newMain").String()!=j.At("oldMain").String(),"forward journal distinguishes new and previous PMM bytes");
    return fs::path(target.native()+L".mms-"+std::wstring(id.begin(),id.end())+L".new-pmm");
}

class Child {
    HANDLE process_;
public:
    Child(const wchar_t* mode, const fs::path& directory, const std::wstring& id) {
        wchar_t exe[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
        Need(length && length < 32768, "locate original test executable");
        // A final dot avoids a trailing backslash escaping the closing quote.
        std::wstring command = L"\"" + std::wstring(exe) + L"\" " + mode + L" \"" +
            (directory / L".").native() + L"\" " + id;
        STARTUPINFOW startup{sizeof startup};
        PROCESS_INFORMATION info{};
        Need(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, directory.c_str(), &startup, &info) != FALSE, "launch owned original child");
        process_ = info.hProcess;
        CloseHandle(info.hThread);
    }
    ~Child() {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT && TerminateProcess(process_, 74))
            WaitForSingleObject(process_, timeout);
        CloseHandle(process_);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    void Await(HANDLE ready) const {
        const HANDLE both[]{ready, process_};
        const DWORD wait = WaitForMultipleObjects(2, both, FALSE, timeout);
        if (wait == WAIT_OBJECT_0 + 1) (void)Join("child-before-barrier");
        Need(wait == WAIT_OBJECT_0, "child reached real rename barrier");
        Need(WaitForSingleObject(process_, 0) == WAIT_TIMEOUT, "child remains parked before interruption");
    }
    void Interrupt() const { Need(TerminateProcess(process_, killed) != FALSE, "terminate only owned child"); }
    DWORD Pid() const { return GetProcessId(process_); }
    DWORD Join(const char* phase) const {
        Need(WaitForSingleObject(process_, timeout) == WAIT_OBJECT_0, "join owned child");
        DWORD code = STILL_ACTIVE;
        Need(GetExitCodeProcess(process_, &code) != FALSE, "read child exit status");
        std::printf("original child phase=%s pid=%lu exit=%lu exithex=0x%08lX\n",
                    phase, GetProcessId(process_), code, code);
        return code;
    }
};

void ArmRenameBarrier(const fs::path& from,const fs::path& to,HANDLE notification,HANDLE parked) {
    save_journal_fixture::ArmRenameBarrier(from,to,notification,parked,timeout);
}
void Writer(const fs::path& dir, HANDLE notification, bool afterPublish,bool beforeRecovery=false,bool forwardRecovery=false) {
    Handle parked(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto target = dir / L"active.pmm", stage = dir / L"stage.pmm";
    Need(Observe(target) == Before(), "writer starts from independently verified seed");
    Write(stage, freshPmm);
    const auto from=beforeRecovery?pmm_paths::Effect(fs::path(target.native()+L"1")):
        (afterPublish?stage:fs::path(target.native()+L"1"));
    const auto to=beforeRecovery?pmm_paths::Effect(fs::path(target.native()+L"11")):
        (afterPublish?target:fs::path(target.native()+L"11"));
    if(forwardRecovery)
        save_journal_fixture::ArmDecisionBarrier(target.native()+L".mms-save.json",notification,parked.value,timeout);
    else ArmRenameBarrier(from,to,notification,parked.value);
    const bool saved = pmm_versions::Publish(stage, target);
    throw std::runtime_error(saved ? "writer bypassed history barrier" : "original publication failed before barrier");
}
void Recovery(const fs::path& dir,HANDLE notification=nullptr,unsigned freshProcess=0) {
    const auto target = dir / L"active.pmm";
    const bool forwardRecovery=Read(dir/L"phase.txt")=="forward-recovery";
    std::unique_ptr<Handle> parked;
    if(notification) {
        parked=std::make_unique<Handle>(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(forwardRecovery) {
            const auto staged=ForwardStage(target,Read(fs::path(target.native()+L".mms-save.json")));
            Need(Observe(target)==ForwardPending() && Read(staged)==freshPmm,
                 "forward recovery starts with advanced history, old active PMM and intact new stage");
            ArmRenameBarrier(staged,target,notification,parked->value);
        } else ArmRenameBarrier(pmm_paths::Effect(fs::path(target.native()+L"11")),
                                pmm_paths::Effect(fs::path(target.native()+L"1")),notification,parked->value);
    }
    const std::wstring report=forwardRecovery?
        (notification?L"forward-recovery-interrupted.txt":L"forward-recovery-"+std::to_wstring(freshProcess)+L".txt"):
        (notification?L"recovery-interrupted.txt":L"recovery.txt");
    std::ofstream evidence(dir / report);
    Need(bool(evidence), "open recovery evidence");
    evidence << "pid=" << GetCurrentProcessId() << " fresh_process=" << freshProcess << std::endl;
    evidence << "phase=recovery-enter" << std::endl;
    // MMDApp's constructor does not initialize state. Construct its default
    // state on the heap too, avoiding a large aggregate temporary on the stack.
    auto app = std::make_unique<MMDApp>();
    {
        const auto initialState = std::make_unique<MMDAppState>();
        app->state = *initialState;
    }
    app->InitDefaults();
    evidence << "phase=app-ready" << std::endl;
    // This is the existing gate called by LoadSceneFile before opening PMM.
    // Both attempts must use this production gate, without a recovery shortcut.
    unsigned attempt = 0;
    auto recover = [&] {
        ++attempt;
        evidence << "attempt=" << attempt << " phase=prepare-begin" << std::endl;
        const bool accepted = plugins::PrepareProjectLoad(app.get(), target.c_str());
        evidence << "attempt=" << attempt << " phase=prepare-end prepare_load=" << accepted << std::endl;
        plugins::CancelProjectLoad(app.get());
        evidence << "attempt=" << attempt << " phase=cancel-end" << std::endl;
        Need(accepted, "existing original pre-read gate accepts recovery");
        const Chain observed = Observe(target);
        evidence << "attempt=" << attempt << " phase=observe-end" << std::endl;
        return observed;
    };
    const Chain first = recover();
    if(notification)throw std::runtime_error(forwardRecovery?"recovery bypassed its real PMM forward barrier":
                                                           "recovery bypassed its real EMM rollback barrier");
    const bool journalFirst = fs::exists(target.native() + L".mms-save.json");
    const Chain second = recover();
    const bool journalSecond = fs::exists(target.native() + L".mms-save.json");
    const bool rollback = first == Before(), forward = first == Forward();
    evidence << "rollback=" << rollback << " forward=" << forward << " repeated_same=" << (first == second)
             << " journal_after_first=" << journalFirst << " journal_after_second=" << journalSecond << '\n';
    for (size_t i = 0; i < first.size(); ++i)
        evidence << "level " << i << " PMM/EMM present: " << first[i][0].has_value() << ' ' << first[i][1].has_value() << '\n';
    evidence.flush(); Need(bool(evidence), "flush recovery evidence");
    Need(rollback || forward, "original pre-read recovery restores one complete PMM/EMM generation chain");
    const bool expectedForward = forwardRecovery || Read(dir / L"phase.txt") == "published";
    Need(expectedForward ? forward : rollback, "recovery follows the durable decision at the interruption point");
    Need(first == second && !journalFirst && !journalSecond, "original recovery is complete and idempotent");
}
void RunCase(const fs::path& directory, bool afterPublish,bool interruptRecovery=false) {
    Need(!interruptRecovery||!afterPublish,"recovery interruption uses the rollback fixture");
    const auto dir = fs::absolute(directory).lexically_normal();
    Need(fs::create_directory(dir), "require a fresh isolated fixture directory");
    std::fwprintf(stderr, L"SAVE-01 original fixture=%ls\n", dir.c_str());
    const auto id = Nonce();
    Write(dir / L"owner.id", std::string(id.begin(), id.end()));
    Write(dir / L"phase.txt", afterPublish ? "published" : "history");
    const auto target = dir / L"active.pmm", stage = dir / L"stage.pmm";
    Write(stage, oldestPmm);
    Need(pmm_versions::Publish(stage, target), "seed oldest via original production publisher");
    Write(pmm_paths::Effect(target), oldestEmm);
    Write(stage, middlePmm);
    Need(pmm_versions::Publish(stage, target), "seed middle via original production publisher");
    Write(pmm_paths::Effect(target), middleEmm, true);
    Need(Observe(target) == Before(), "positive control: both seed generations match exact independent bytes");
    std::puts("positive control: original PMM/EMM seed generations match");
    const HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, EventName(id).c_str());
    const DWORD eventError = GetLastError();
    Handle event(handle);
    Need(eventError != ERROR_ALREADY_EXISTS, "own fresh barrier event");
    Child writer(interruptRecovery?L"--original-version-crash-write-recovery":
                 (afterPublish ? L"--original-version-crash-write-published" : L"--original-version-crash-write"), dir, id);
    try { writer.Await(event.value); }
    catch (...) {
        if (fs::exists(dir / L"child-error.txt")) std::printf("%s", Read(dir / L"child-error.txt").c_str());
        throw;
    }
    Chain split = Before(); split[2][0] = split[1][0]; split[1][0].reset();
    if(interruptRecovery) { split[2][1]=split[1][1];split[1][1].reset(); }
    if(afterPublish)split=Forward();
    Need(Observe(target) == split, "exact independently expected chain at the real rename barrier");
    const auto journal=fs::path(target.native()+L".mms-save.json");
    Need(fs::exists(journal), "durable recovery record precedes all historical mutations");
    const auto record=Read(journal);
    Need(record.find("\"schemaVersion\":3")!=std::string::npos &&
         record.find("\"members\":\"pmm-emm\"")!=std::string::npos &&
         record.find("\"newSide\"")==std::string::npos && record.find("\"oldSide\"")==std::string::npos,
         "original transaction carries only native member semantics");
    std::printf("verified paused original writer: phase=%s journal=1\n",
                interruptRecovery?"history-pair":(afterPublish?"published":"history"));
    writer.Interrupt(); Need(writer.Join("writer-killed") == killed, "writer ended by owned hard interruption");
    Need(Observe(target) == split, "hard interruption did not run retention destructors");
    if(interruptRecovery) {
        Need(record.find("\"decision\":\"rollback\"")!=std::string::npos,"interrupted history carries a rollback decision");
        Need(ResetEvent(event.value)!=FALSE,"reset writer notification before waiting for recovery");
        Child interrupted(L"--original-version-crash-recover-interrupt",dir,id);
        try { interrupted.Await(event.value); }
        catch (...) {
            if(fs::exists(dir/L"child-error.txt"))std::printf("%s",Read(dir/L"child-error.txt").c_str());
            throw;
        }
        Chain partial=Before();partial[2][0]=partial[1][0];partial[1][0].reset();
        Need(Observe(target)==partial && partial!=split && partial!=Before(),
             "real recovery moved EMM back while historical PMM still awaits rollback");
        Need(Read(journal)==record,"partially recovered chain retains its original durable decision");
        std::puts("verified paused original recovery: EMM at 1, PMM at 11; journal=1");
        interrupted.Interrupt();
        Need(interrupted.Join("recovery-killed")==killed,"recovery ended by owned hard interruption");
        Need(Observe(target)==partial && Read(journal)==record,"second hard interruption preserves partial recovery and journal");
        std::printf("%s",Read(dir/L"recovery-interrupted.txt").c_str());
    }
    Child recovery(L"--original-version-crash-recover", dir, id);
    const DWORD code = recovery.Join("recovery");
    if (fs::exists(dir / L"recovery.txt")) std::printf("%s", Read(dir / L"recovery.txt").c_str());
    if (fs::exists(dir / L"child-error.txt")) std::printf("%s", Read(dir / L"child-error.txt").c_str());
    Need(code == 0, "fresh original process recovered a complete, idempotent two-member chain");
    std::puts(interruptRecovery?"PASS SAVE-01 original recovery hard interruption and fresh-process retry":
              "PASS SAVE-01 original retained pair hard interruption");
}
void RunForwardRecoveryCase(const fs::path& directory) {
    const auto dir=fs::absolute(directory).lexically_normal();
    Need(fs::create_directory(dir),"require a fresh isolated forward recovery fixture");
    std::fwprintf(stderr,L"SAVE-01 original forward recovery fixture=%ls\n",dir.c_str());
    const auto id=Nonce();
    Write(dir/L"owner.id",std::string(id.begin(),id.end()));
    Write(dir/L"phase.txt","forward-recovery");
    const auto target=dir/L"active.pmm",stage=dir/L"stage.pmm";
    const auto journal=fs::path(target.native()+L".mms-save.json");
    Write(stage,oldestPmm);
    Need(pmm_versions::Publish(stage,target),"seed forward fixture oldest generation");
    Write(pmm_paths::Effect(target),oldestEmm);
    Write(stage,middlePmm);
    Need(pmm_versions::Publish(stage,target),"seed forward fixture middle generation");
    Write(pmm_paths::Effect(target),middleEmm,true);
    Need(Observe(target)==Before(),"forward fixture starts with exact independent PMM/EMM seed bytes");
    const HANDLE handle=CreateEventW(nullptr,TRUE,FALSE,EventName(id).c_str());
    const DWORD eventError=GetLastError();
    Handle event(handle);
    Need(eventError!=ERROR_ALREADY_EXISTS,"own fresh forward recovery event");
    auto await=[&](const Child& child) {
        try { child.Await(event.value); }
        catch(...) {
            if(fs::exists(dir/L"child-error.txt"))std::printf("%s",Read(dir/L"child-error.txt").c_str());
            throw;
        }
    };
    Child writer(L"--original-version-crash-write-forward",dir,id);
    await(writer);
    const auto record=Read(journal);
    const auto staged=ForwardStage(target,record);
    const Chain pending=ForwardPending();
    Need(pending!=Before() && pending!=Forward() && Observe(target)==pending,
         "forward decision boundary has advanced 1/11 but still the old active PMM");
    Need(Read(stage)==freshPmm && Read(staged)==freshPmm,"both new PMM sources precede the first hard stop");
    std::puts("verified paused original writer: decision=forward active=middle 1=middle 11=oldest new-pmm=fresh");
    writer.Interrupt();
    Need(writer.Join("forward-writer-killed")==killed,"forward writer ended by owned hard interruption");
    Need(Observe(target)==pending && Read(journal)==record && Read(staged)==freshPmm,
         "first hard stop retains the mixed publication state and original forward journal");
    Need(ResetEvent(event.value)!=FALSE,"reset forward writer event before recovery");
    Child interrupted(L"--original-version-crash-recover-interrupt",dir,id);
    await(interrupted);
    // Original recovery has one remaining public rename: history was already
    // aligned before the publisher wrote forward. This barrier is AFTER that
    // real new-pmm -> active.pmm move but BEFORE verification/journal cleanup.
    // Do not manufacture a half-rotated forward archive or rewrite its journal.
    Need(Observe(target)==Forward() && Observe(target)!=pending && !fs::exists(staged),
         "real forward recovery consumed new-pmm and installed fresh active bytes");
    Need(Read(journal)==record && Read(stage)==freshPmm,
         "forward recovery has not cleaned its journal or consumed the caller's stage");
    const auto progress=Read(dir/L"forward-recovery-interrupted.txt");
    Need(progress.find("attempt=1 phase=prepare-begin")!=std::string::npos &&
         progress.find("phase=prepare-end")==std::string::npos,
         "second barrier is inside the first production pre-read recovery call");
    std::puts("verified paused original forward recovery: new-pmm -> active.pmm completed; decision=forward journal=1 prepare-end=0");
    interrupted.Interrupt();
    Need(interrupted.Join("forward-recovery-killed")==killed,"forward recovery ended by owned hard interruption");
    Need(Observe(target)==Forward() && Read(journal)==record && !fs::exists(staged),
         "second hard stop preserves completed forward action and unfinished recovery journal");
    std::printf("%s",progress.c_str());
    Chain first{};
    for(unsigned attempt=1;attempt<=2;++attempt) {
        Child recovery(attempt==1?L"--original-version-crash-recover-forward-first":
                                  L"--original-version-crash-recover-forward-second",dir,id);
        const DWORD code=recovery.Join(attempt==1?"forward-retry-first":"forward-retry-second");
        const auto report=dir/(L"forward-recovery-"+std::to_wstring(attempt)+L".txt");
        if(fs::exists(report))std::printf("%s",Read(report).c_str());
        if(fs::exists(dir/L"child-error.txt"))std::printf("%s",Read(dir/L"child-error.txt").c_str());
        Need(code==0,"fresh process completes forward recovery through the production load gate");
        const Chain observed=Observe(target);
        Need(observed==Forward() && !fs::exists(journal) && !fs::exists(staged) && Read(stage)==freshPmm,
             "fresh process preserves exact active/1/11 PMM/EMM bytes and empty 111 without another rotation");
        if(attempt==1)first=observed;
        else Need(observed==first,"second fresh process leaves all generation contents and absences unchanged");
    }
    std::puts("PASS SAVE-01 original forward recovery hard interruption and two fresh-process retries");
}
// Observe the production call itself, without supplying a result or error.
// Other moves (journal publication and rollback) must not replace this evidence.
struct RejectedMoveEvidence {
    fs::path from,to;
    unsigned calls=0;
    BOOL result=TRUE;
    DWORD error=ERROR_SUCCESS,flags=0;
};
RejectedMoveEvidence* rejectedMoveEvidence=nullptr;
decltype(&MoveFileExW) realRejectedMove=nullptr;
BOOL WINAPI ObserveRejectedMove(LPCWSTR from,LPCWSTR to,DWORD flags) {
    const BOOL result=realRejectedMove(from,to,flags);
    const DWORD error=GetLastError();
    auto* evidence=rejectedMoveEvidence;
    if(evidence&&from&&to&&!wcscmp(from,evidence->from.c_str())&&!wcscmp(to,evidence->to.c_str())) {
        ++evidence->calls;
        evidence->result=result;
        evidence->error=error;
        evidence->flags=flags;
    }
    SetLastError(error);
    return result;
}
class RejectedMoveProbe {
    bool initialized_=false,created_=false,enabled_=false;
    void* api_=reinterpret_cast<void*>(&MoveFileExW);
    void Release() noexcept {
        const DWORD error=GetLastError();
        if(enabled_)MH_DisableHook(api_);
        if(created_)MH_RemoveHook(api_);
        rejectedMoveEvidence=nullptr;
        if(initialized_)MH_Uninitialize();
        SetLastError(error);
    }
public:
    RejectedMoveEvidence evidence;
    RejectedMoveProbe(const fs::path& from,const fs::path& to) {
        evidence.from=from;evidence.to=to;
        try {
            const auto initialized=MH_Initialize();
            initialized_=initialized==MH_OK;
            Need(initialized_||initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize rejected-move observer");
            Need(MH_CreateHook(api_,reinterpret_cast<void*>(&ObserveRejectedMove),
                              reinterpret_cast<void**>(&realRejectedMove))==MH_OK,"observe actual target replacement");
            created_=true;
            rejectedMoveEvidence=&evidence;
            Need(MH_EnableHook(api_)==MH_OK,"enable rejected-move observer");
            enabled_=true;
        } catch(...) { Release();throw; }
    }
    ~RejectedMoveProbe(){Release();}
    RejectedMoveProbe(const RejectedMoveProbe&)=delete;
    RejectedMoveProbe& operator=(const RejectedMoveProbe&)=delete;
};
void FailureControls(const fs::path& dir) {
    Need(fs::create_directory(dir), "create isolated failure controls");
    const auto target=dir/L"active.pmm",stage=dir/L"stage.pmm";
    Write(stage,oldestPmm);
    Need(pmm_versions::Publish(stage,target), "seed failure control");
    Write(pmm_paths::Effect(target),oldestEmm);
    const auto before=Observe(target);
    Write(stage,freshPmm);
    {
        Handle reader(CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        RejectedMoveProbe probe(stage,target);
        SetLastError(ERROR_SUCCESS);
        const bool saved=pmm_versions::Publish(stage,target);
        const DWORD error=GetLastError();
        const auto& observed=probe.evidence;
        std::printf("original rejected replacement: calls=%u native_result=%d native_error=%lu flags=0x%08lX publish_result=%d publish_error=%lu\n",
                    observed.calls,int(observed.result),observed.error,observed.flags,int(saved),error);
        SetLastError(error);
        Need(observed.calls==1 && observed.flags==(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),
             "observe exactly the production target replacement, not an earlier save failure");
        Need(observed.result==FALSE && observed.error!=ERROR_SUCCESS,"real target replacement was rejected by Windows");
        Need(!saved && error==observed.error,"rejected replace returns false and preserves the observed Win32 error exactly");
    }
    const auto journal=fs::path(target.native()+L".mms-save.json");
    Need(Observe(target)==before && Read(stage)==freshPmm && !fs::exists(journal),
         "ordinary rejected save restores history and preserves caller stage");
    auto app=std::make_unique<MMDApp>();
    {
        const auto state=std::make_unique<MMDAppState>();
        app->state=*state;
    }
    app->InitDefaults();
    Need(plugins::PrepareProjectLoad(app.get(),target.c_str()), "rejected save does not reappear on load");
    plugins::CancelProjectLoad(app.get());
    Need(Observe(target)==before, "load cannot commit the rejected save");
    for(unsigned schema : {1u,2u,3u,99u}) {
        const auto malformed="{\"schemaVersion\":"+std::to_string(schema)+"}";
        Write(journal,malformed);
        Need(!pmm_versions::Publish(stage,target), "invalid previous transaction refuses a new save");
        Need(!plugins::PrepareProjectLoad(app.get(),target.c_str()), "invalid previous transaction refuses load");
        Need(Read(journal)==malformed && Observe(target)==before && Read(stage)==freshPmm,
             "invalid previous transaction retains its evidence and all public bytes");
        Need(DeleteFileW(journal.c_str())!=FALSE, "remove only this control's malformed journal");
    }
    const auto side=fs::path(target.native()+L".mms-plugins.json");
    const auto historicalSide=fs::path(target.native()+L"1.mms-plugins.json");
    Write(side,"opaque active bytes, deliberately not plugin JSON");
    Write(historicalSide,"opaque historical bytes, deliberately not plugin JSON");
    Need(pmm_versions::Publish(stage,target), "original save does not interpret existing plugin sidecars");
    Need(plugins::PrepareProjectLoad(app.get(),target.c_str()), "original load does not interpret plugin sidecars");
    plugins::CancelProjectLoad(app.get());
    Need(Read(side)=="opaque active bytes, deliberately not plugin JSON" &&
         Read(historicalSide)=="opaque historical bytes, deliberately not plugin JSON" &&
         Read(target)==freshPmm && !fs::exists(stage) && !fs::exists(journal),
         "two-member publication leaves opaque sidecars untouched and consumes the caller stage");
    std::puts("PASS original false-result and invalid-journal controls");
}
// Five real sharing denials of the original two-member recovery. Every fixture
// reaches its boundary through pmm_versions::Publish and an owned hard process
// stop; this test never writes or replaces a journal. One read-only handle
// without FILE_SHARE_DELETE stays open across two production recovery calls,
// then independent processes finish the same durable decision. The fifth point
// holds the transaction's rolled-back EMM stage, which the checked cleanup
// deletes after the regular stages and before the journal.
enum class OriginalDenial { ForwardPublish, ForwardJournal, RollbackEffect, RollbackJournal, RollbackEffectCleanup };

const char* DenialAscii(OriginalDenial point) {
    switch (point) {
    case OriginalDenial::ForwardPublish: return "forward-publish";
    case OriginalDenial::ForwardJournal: return "forward-journal";
    case OriginalDenial::RollbackEffect: return "rollback-effect";
    case OriginalDenial::RollbackJournal: return "rollback-journal";
    case OriginalDenial::RollbackEffectCleanup: return "rollback-effect-cleanup";
    }
    return "invalid-denial";
}
const wchar_t* DenialName(OriginalDenial point) {
    switch (point) {
    case OriginalDenial::ForwardPublish: return L"forward-publish";
    case OriginalDenial::ForwardJournal: return L"forward-journal";
    case OriginalDenial::RollbackEffect: return L"rollback-effect";
    case OriginalDenial::RollbackJournal: return L"rollback-journal";
    case OriginalDenial::RollbackEffectCleanup: return L"rollback-effect-cleanup";
    }
    return L"invalid-denial";
}
const wchar_t* DenialWriterMode(OriginalDenial point) {
    switch (point) {
    case OriginalDenial::ForwardPublish:
    case OriginalDenial::ForwardJournal: return L"--original-version-crash-write-forward";
    case OriginalDenial::RollbackEffect: return L"--original-version-crash-write-recovery";
    case OriginalDenial::RollbackJournal:
    case OriginalDenial::RollbackEffectCleanup: return L"--original-version-crash-write";
    }
    return L"";
}
bool DenialForward(OriginalDenial point) {
    return point == OriginalDenial::ForwardPublish || point == OriginalDenial::ForwardJournal;
}
// Publisher hard stops that leave the rollback record: only the oldest PMM
// reached 11, or both oldest members did. The recovery's first inverse move is
// then the PMM or the EMM, respectively.
Chain PmmCrashChain() {
    Chain chain = Before();
    chain[2][0] = chain[1][0];
    chain[1][0].reset();
    return chain;
}
Chain PmmEffectCrashChain() {
    Chain chain = PmmCrashChain();
    chain[2][1] = chain[1][1];
    chain[1][1].reset();
    return chain;
}
std::optional<std::string> OptionalText(const char* text) {
    return text ? std::optional<std::string>{std::string(text)} : std::nullopt;
}
struct OriginalExpectation {
    Chain chain;
    std::optional<std::string> newMain, oldMain, oldEffect;
};
OriginalExpectation CrashExpectation(OriginalDenial point) {
    if (DenialForward(point)) return {ForwardPending(), OptionalText(freshPmm), std::nullopt, std::nullopt};
    if (point == OriginalDenial::RollbackEffect)
        return {PmmEffectCrashChain(), OptionalText(freshPmm), OptionalText(middlePmm), OptionalText(middleEmm)};
    return {PmmCrashChain(), OptionalText(freshPmm), OptionalText(middlePmm), OptionalText(middleEmm)};
}
OriginalExpectation DeniedExpectation(OriginalDenial point) {
    // A denied forward PMM publication changes nothing. A denied forward
    // journal completion follows the real publication of the staged new PMM,
    // so only its consumed stage is gone. A denied rollback move changes
    // nothing. A denied rollback journal completion follows the complete
    // inverse chain, and Cleanup deletes the staged new PMM, the staged
    // previous PMM and the temporary journal with checked errors, then
    // discards the checked old-EMM stage, and only then deletes the journal
    // itself. The denied journal deletion therefore leaves those four stages
    // gone while the journal stays. A denial of the checked old-EMM discard
    // runs after those regular stage removals and before the journal deletion,
    // so only that EMM stage and the journal survive.
    if (point == OriginalDenial::ForwardPublish)
        return {ForwardPending(), OptionalText(freshPmm), std::nullopt, std::nullopt};
    if (point == OriginalDenial::ForwardJournal)
        return {Forward(), std::nullopt, std::nullopt, std::nullopt};
    if (point == OriginalDenial::RollbackEffect)
        return {PmmEffectCrashChain(), OptionalText(freshPmm), OptionalText(middlePmm), OptionalText(middleEmm)};
    if (point == OriginalDenial::RollbackEffectCleanup)
        return {Before(), std::nullopt, std::nullopt, OptionalText(middleEmm)};
    return {Before(), std::nullopt, std::nullopt, std::nullopt};
}
Chain FinalChain(OriginalDenial point) { return DenialForward(point) ? Forward() : Before(); }

// Production transaction paths for a schema-3 journal. All names derive from
// the requested target and the validated save ID, exactly like project_storage.
struct OriginalFiles {
    fs::path journal, newMain, oldMain, tempJournal, oldEffect;
    OriginalFiles(const fs::path& target, const std::string& id) {
        Need(id.size() == 32 && id.find_first_not_of("0123456789abcdef") == id.npos,
             "validate the original transaction ID");
        const std::wstring wide(id.begin(), id.end());
        const auto base = target.native() + L".mms-" + wide;
        journal = target.native() + L".mms-save.json";
        newMain = base + L".new-pmm";
        oldMain = base + L".old-pmm";
        tempJournal = base + L".journal";
        oldEffect = target.parent_path() / (L".mms-" + wide + L".old-emm");
    }
};
struct OriginalJournal {
    std::string record, saveId, newMain, oldMain;
    bool forward = false;
};
OriginalJournal ReadOriginalJournal(const fs::path& target) {
    OriginalJournal result;
    result.record = Read(fs::path(target.native() + L".mms-save.json"));
    const auto j = save_journal_fixture::Parse(result.record);
    Need(j.At("schemaVersion").UInt() == 3, "require the original schema-3 journal");
    Need(j.At("members").String() == "pmm-emm", "require the original two-member journal");
    Need(j.At("effectStage").UInt() == 1, "require the original compact effect stage");
    Need(!j.object.count("newSide") && !j.object.count("oldSide"), "original journal has no plugin side members");
    const auto decision = j.At("decision").String();
    Need(decision == "forward" || decision == "rollback", "original journal carries a durable decision");
    result.forward = decision == "forward";
    result.saveId = j.At("saveId").String();
    Need(result.saveId.size() == 32 && result.saveId.find_first_not_of("0123456789abcdef") == result.saveId.npos,
         "validate the original transaction ID");
    result.newMain = j.At("newMain").String();
    result.oldMain = j.At("oldMain").String();
    const auto hex64 = [](const std::string& value) {
        return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
    };
    Need(hex64(result.newMain) && hex64(result.oldMain) && result.newMain != result.oldMain,
         "original journal distinguishes new and previous PMM hashes");
    const auto& rows = j.At("versions").Items();
    Need(rows.size() == 3, "original journal records the active and two historical levels");
    for (const auto& row : rows) {
        const auto& values = row.Items();
        Need(values.size() == 2, "original journal level retains exactly PMM/EMM members");
        for (const auto& value : values) {
            const auto hash = value.String();
            Need(hash.empty() || hex64(hash), "original journal retains hexadecimal member hashes");
        }
    }
    Need(rows[0].Items()[0].String() == result.oldMain, "journal active row records the previous PMM hash");
    Need(rows.back().Items()[0].String().empty() && rows.back().Items()[1].String().empty(),
         "journal last recorded level is the vacant terminal level");
    return result;
}
std::optional<std::string> ReadOptional(const fs::path& path) {
    bool present = false;
    Need(pmm_versions::Exists(path, present), "inspect transaction stage");
    return present ? std::optional<std::string>{Read(path)} : std::nullopt;
}
std::string Sha256Hex(const std::string& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    std::array<unsigned char, 32> digest{};
    const auto close = [&] {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    try {
        Need(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
             "open fixture SHA-256 provider");
        DWORD objectBytes = 0, written = 0;
        Need(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                               static_cast<ULONG>(sizeof(objectBytes)), &written, 0) >= 0,
             "measure fixture SHA-256 state");
        object.resize(objectBytes);
        Need(BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0,
             "create fixture SHA-256 state");
        Need(BCryptHashData(hash, const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(bytes.data())),
                            ULONG(bytes.size()), 0) >= 0, "hash fixture bytes");
        Need(BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) >= 0, "finish fixture SHA-256");
    } catch (...) {
        close();
        throw;
    }
    close();
    std::string hex;
    hex.reserve(64);
    for (const auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}
std::set<std::string> ExistingMemberHashes(const fs::path& target) {
    std::set<std::string> hashes;
    fs::path level = target;
    for (int index = 0; index < 4; ++index) {
        const std::array<fs::path, 2> members{level, pmm_paths::Effect(level)};
        for (const auto& member : members) {
            bool present = false;
            Need(pmm_versions::Exists(member, present), "inspect retained member");
            if (present) hashes.insert(Sha256Hex(Read(member)));
        }
        level += L"1";
    }
    return hashes;
}
// Cross-check the journal against the bytes and transaction names the real
// publisher created. Nothing here writes or rewrites the record.
void AuthenticateOriginalJournal(const fs::path& target, const OriginalJournal& journal, const OriginalFiles& files) {
    Need(fs::exists(files.newMain), "real publisher retains its staged new PMM before recovery");
    Need(Sha256Hex(Read(files.newMain)) == journal.newMain, "journal new PMM hash binds the production stage bytes");
    Need(Sha256Hex(Read(target)) == journal.oldMain, "journal previous PMM hash binds the production active bytes");
    const auto j = save_journal_fixture::Parse(journal.record);
    const auto& rows = j.At("versions").Items();
    Need(rows[0].Items()[1].String() == Sha256Hex(Read(pmm_paths::Effect(target))),
         "journal active EMM hash binds the production active EMM");
    const auto existing = ExistingMemberHashes(target);
    for (std::size_t index = 1; index < rows.size(); ++index)
        for (const auto& value : rows[index].Items()) {
            const auto hash = value.String();
            Need(hash.empty() || existing.count(hash) != 0, "journal historical hash binds a production member");
        }
}

// Observe the production recovery's real rename or deletion API. The hook
// only records the actual API result and error; it never alters or injects a
// failure, and the original pre-read gate swallows its exception into a false
// result.
enum class ObservedFileOp { Move, Remove, Disposition, OpenForDelete };
struct ObservedFileFailure {
    ObservedFileOp kind = ObservedFileOp::Move;
    fs::path from, to;
    DWORD thread = 0;
    unsigned calls = 0;
    BOOL result = TRUE;
    DWORD error = ERROR_SUCCESS;
    DWORD flags = 0;
    DWORD attributesBefore = INVALID_FILE_ATTRIBUTES;
    BY_HANDLE_FILE_INFORMATION fileIdentity{};
};
// One pointer per thread. Other threads see their own null pointer and only
// forward the real API, so they never read or extend the parent's record.
thread_local ObservedFileFailure* observedFileFailure = nullptr;
decltype(&MoveFileExW) realObservedMove = nullptr;
decltype(&DeleteFileW) realObservedRemove = nullptr;
decltype(&SetFileInformationByHandle) realObservedDisposition = nullptr;
decltype(&CreateFileW) realObservedOpen = nullptr;
BOOL WINAPI ObserveRecoveryMove(LPCWSTR from, LPCWSTR to, DWORD flags) {
    auto* observed = observedFileFailure;
    const DWORD incoming = GetLastError();
    const bool match = observed && observed->kind == ObservedFileOp::Move && from && to &&
        !wcscmp(from, observed->from.c_str()) && !wcscmp(to, observed->to.c_str()) &&
        GetCurrentThreadId() == observed->thread;
    // Preserve the caller's last error for the one forwarded real call.
    SetLastError(incoming);
    const BOOL result = realObservedMove(from, to, flags);
    const DWORD error = GetLastError();
    if (match) {
        ++observed->calls;
        observed->result = result;
        observed->error = error;
        observed->flags = flags;
    }
    SetLastError(error);
    return result;
}
BOOL WINAPI ObserveRecoveryRemove(LPCWSTR path) {
    auto* observed = observedFileFailure;
    const DWORD incoming = GetLastError();
    const bool match = observed && observed->kind == ObservedFileOp::Remove && path &&
        !wcscmp(path, observed->from.c_str()) && GetCurrentThreadId() == observed->thread;
    if (match) {
        ++observed->calls;
        // Real attributes immediately before the one forwarded DeleteFileW.
        observed->attributesBefore = GetFileAttributesW(path);
        // GetFileAttributesW may change the last error; restore the caller's
        // value before the real API call.
        SetLastError(incoming);
    }
    const BOOL result = realObservedRemove(path);
    const DWORD error = GetLastError();
    if (match) {
        observed->result = result;
        observed->error = error;
    }
    SetLastError(error);
    return result;
}
BOOL WINAPI ObserveRecoveryDisposition(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, LPVOID data, DWORD size) {
    auto* observed = observedFileFailure;
    const DWORD incoming = GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    const bool match = observed && observed->kind == ObservedFileOp::Disposition &&
        GetCurrentThreadId() == observed->thread && type == FileDispositionInfo &&
        size == sizeof(FILE_DISPOSITION_INFO) && data && static_cast<const FILE_DISPOSITION_INFO*>(data)->DeleteFile &&
        GetFileInformationByHandle(file, &info) && info.dwVolumeSerialNumber == observed->fileIdentity.dwVolumeSerialNumber &&
        info.nFileIndexHigh == observed->fileIdentity.nFileIndexHigh && info.nFileIndexLow == observed->fileIdentity.nFileIndexLow;
    SetLastError(incoming);
    const BOOL result = realObservedDisposition(file, type, data, size);
    const DWORD error = GetLastError();
    if (match) {
        ++observed->calls;
        observed->result = result;
        observed->error = error;
        observed->attributesBefore = info.dwFileAttributes;
    }
    SetLastError(error);
    return result;
}
HANDLE WINAPI ObserveRecoveryOpenForDelete(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE templateFile) {
    auto* observed = observedFileFailure;
    const DWORD incoming = GetLastError();
    const bool match = observed && observed->kind == ObservedFileOp::OpenForDelete &&
        GetCurrentThreadId() == observed->thread && path && (access & DELETE) &&
        !wcscmp(path, observed->from.c_str());
    if (match) observed->attributesBefore = GetFileAttributesW(path);
    SetLastError(incoming);
    const HANDLE file = realObservedOpen(path, access, share, security, creation, flags, templateFile);
    const DWORD error = GetLastError();
    if (match) {
        ++observed->calls;
        observed->result = file != INVALID_HANDLE_VALUE;
        observed->error = error;
        observed->flags = flags;
    }
    SetLastError(error);
    return file;
}
class RecoveryFileProbe {
    bool initialized_ = false, created_ = false, enabled_ = false;
    void* api_ = nullptr;
    void Release() noexcept {
        const DWORD error = GetLastError();
        if (enabled_) MH_DisableHook(api_);
        if (created_) MH_RemoveHook(api_);
        observedFileFailure = nullptr;
        if (initialized_) MH_Uninitialize();
        SetLastError(error);
    }
public:
    ObservedFileFailure observed;
    RecoveryFileProbe(ObservedFileOp kind, const fs::path& from, const fs::path& to = fs::path()) {
        observed.kind = kind;
        observed.from = from;
        observed.to = to;
        observed.thread = GetCurrentThreadId();
        try {
            const MH_STATUS initialized = MH_Initialize();
            initialized_ = initialized == MH_OK;
            Need(initialized_ || initialized == MH_ERROR_ALREADY_INITIALIZED,
                 "initialize the native recovery file-operation observer");
            void* detour = nullptr;
            void** original = nullptr;
            if (kind == ObservedFileOp::Move) {
                api_ = reinterpret_cast<void*>(&MoveFileExW);
                detour = reinterpret_cast<void*>(&ObserveRecoveryMove);
                original = reinterpret_cast<void**>(&realObservedMove);
            } else if (kind == ObservedFileOp::Remove) {
                api_ = reinterpret_cast<void*>(&DeleteFileW);
                detour = reinterpret_cast<void*>(&ObserveRecoveryRemove);
                original = reinterpret_cast<void**>(&realObservedRemove);
            } else if (kind == ObservedFileOp::OpenForDelete) {
                api_ = reinterpret_cast<void*>(&CreateFileW);
                detour = reinterpret_cast<void*>(&ObserveRecoveryOpenForDelete);
                original = reinterpret_cast<void**>(&realObservedOpen);
            } else {
                Handle file(CreateFileW(from.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
                Need(GetFileInformationByHandle(file.value, &observed.fileIdentity) != FALSE,
                     "identify the exact recovery stage before observing its deletion");
                api_ = reinterpret_cast<void*>(&SetFileInformationByHandle);
                detour = reinterpret_cast<void*>(&ObserveRecoveryDisposition);
                original = reinterpret_cast<void**>(&realObservedDisposition);
            }
            Need(MH_CreateHook(api_, detour, original) == MH_OK, "observe the actual recovery file operation");
            created_ = true;
            observedFileFailure = &observed;
            Need(MH_EnableHook(api_) == MH_OK, "enable the native recovery file-operation observer");
            enabled_ = true;
        } catch (...) {
            Release();
            throw;
        }
    }
    ~RecoveryFileProbe() { Release(); }
    RecoveryFileProbe(const RecoveryFileProbe&) = delete;
    RecoveryFileProbe& operator=(const RecoveryFileProbe&) = delete;
};
// Supplementary proof that the held member also denies DELETE access. It never
// replaces the real recovery-operation evidence above.
DWORD DeleteAccessProbe(const fs::path& path) {
    const HANDLE handle = CreateFileW(path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return error;
}
void CheckOriginalTransaction(const fs::path& target, const OriginalFiles& files, const OriginalExpectation& expected,
                              const std::string& record, const fs::path& callerStage, const char* phase) {
    const std::string label = phase;
    Need(Observe(target) == expected.chain, (label + ": active/1/11/111 PMM/EMM bytes and absences are exact").c_str());
    Need(ReadOptional(files.newMain) == expected.newMain, (label + ": staged new PMM is exact").c_str());
    Need(ReadOptional(files.oldMain) == expected.oldMain, (label + ": staged previous PMM is exact").c_str());
    Need(ReadOptional(files.oldEffect) == expected.oldEffect, (label + ": staged previous EMM is exact").c_str());
    Need(!fs::exists(files.tempJournal), (label + ": no temporary journal stage").c_str());
    Need(fs::exists(files.journal) && Read(files.journal) == record,
         (label + ": journal remains the exact publisher record").c_str());
    Need(fs::exists(callerStage) && Read(callerStage) == freshPmm, (label + ": caller stage is untouched").c_str());
}
// The parked publisher still owns its active and historical
// FILE_FLAG_DELETE_ON_CLOSE locks, so this is checked only after that process
// is gone and after every recovery call has unwound.
void CheckOriginalLocksReleased(const fs::path& target, const char* phase) {
    const std::string label = phase;
    fs::path level = target;
    for (int index = 0; index < 4; ++index) {
        Need(!fs::exists(fs::path(level.native() + L".mms-save.lock")),
             (label + ": production save lock released").c_str());
        level += L"1";
    }
}
void CheckOriginalRecovered(const fs::path& dir, const fs::path& target, const OriginalFiles& files,
                            const Chain& expected, const fs::path& callerStage, const char* phase) {
    const std::string label = phase;
    Need(Observe(target) == expected, (label + ": final two-member chain matches the durable decision").c_str());
    Need(!fs::exists(files.journal) && !fs::exists(files.newMain) && !fs::exists(files.oldMain) &&
         !fs::exists(files.oldEffect) && !fs::exists(files.tempJournal),
         (label + ": journal and every transaction stage are cleaned").c_str());
    Need(fs::exists(callerStage) && Read(callerStage) == freshPmm,
         (label + ": original recovery preserves the caller stage").c_str());
    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto name = entry.path().filename().native();
        Need(name.find(L".mms-save") == std::wstring::npos && name.rfind(L".mms-", 0) != 0,
             (label + ": no transaction residue in the isolated fixture").c_str());
    }
}
// One fresh-process retry: join it, require and preserve its real recovery.txt
// report, and bind that report to the actual child PID. Recovery uses the
// fixed recovery.txt name whenever phase is published/history, so the next
// child would otherwise overwrite this evidence.
void CheckOriginalRetryEvidence(const fs::path& dir, const fs::path& target, const OriginalFiles& files,
                                const Chain& expected, const fs::path& callerStage, bool forward,
                                Child& child, unsigned retry, DWORD pid, std::ofstream& evidence) {
    const DWORD code = child.Join(retry == 1 ? "sharing-retry-first" : "sharing-retry-second");
    if (fs::exists(dir / L"child-error.txt")) std::printf("%s", Read(dir / L"child-error.txt").c_str());
    Need(code == 0, "a fresh original process completed the denied recovery");
    Need(!fs::exists(dir / L"child-error.txt"), "fresh original recovery reported no child failure");
    const auto report = dir / L"recovery.txt";
    Need(fs::exists(report), "fresh original recovery wrote its detailed recovery.txt report");
    const auto text = Read(report);
    Need(text.find("pid=" + std::to_string(pid) + " fresh_process=0") != std::string::npos,
         "recovery report is bound to the actual fresh process PID");
    for (unsigned attempt : {1u, 2u}) {
        const auto prefix = "attempt=" + std::to_string(attempt) + " ";
        Need(text.find(prefix + "phase=prepare-end prepare_load=1") != std::string::npos,
             "recovery report records a completed production pre-read gate");
        Need(text.find(prefix + "phase=cancel-end") != std::string::npos,
             "recovery report records the production gate cancellation");
    }
    Need(text.find("repeated_same=1") != std::string::npos,
         "recovery report records the idempotent second gate pass");
    Need(text.find("journal_after_first=0") != std::string::npos &&
         text.find("journal_after_second=0") != std::string::npos,
         "recovery report records journal cleanup after both gate passes");
    const auto saved = dir / (L"recovery-retry-" + std::to_wstring(retry) + L".txt");
    Write(saved, text);
    Need(Read(saved) == text, "saved retry report is the exact child report");
    std::printf("%s", text.c_str());
    CheckOriginalRecovered(dir, target, files, expected, callerStage, "fresh-retry");
    CheckOriginalLocksReleased(target, "fresh-retry");
    char line[256]{};
    std::snprintf(line, sizeof(line),
                  "retry=%u pid=%lu exit=%lu report=recovery-retry-%u.txt final=%s journal=0 stages=0 caller_stage=untouched",
                  retry, pid, code, retry, forward ? "forward" : "rollback");
    std::printf("%s\n", line);
    evidence << line << '\n';
    evidence.flush();
    Need(bool(evidence), "flush original sharing-denial evidence");
}
// Existence, bytes and full attributes of the public active/1/11/111 PMM/EMM
// generation. Captured read-only from this isolated fixture before the writer
// starts, then checked after every production recovery the test performs.
struct OriginalPublicMember {
    fs::path path;
    bool present = false;
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    std::string bytes;
};
struct OriginalPublicSnapshot {
    std::array<std::array<OriginalPublicMember, 2>, 4> levels;
    explicit OriginalPublicSnapshot(const fs::path& target) {
        fs::path level = target;
        for (auto& members : levels) {
            const std::array<fs::path, 2> paths{level, pmm_paths::Effect(level)};
            for (size_t index = 0; index < paths.size(); ++index) {
                members[index].path = paths[index];
                bool present = false;
                Need(pmm_versions::Exists(paths[index], present), "inspect public fixture member");
                if (!present) continue;
                Need(fs::is_regular_file(paths[index]), "public fixture member is a regular file");
                members[index].present = true;
                members[index].attributes = GetFileAttributesW(paths[index].c_str());
                Need(members[index].attributes != INVALID_FILE_ATTRIBUTES,
                     "read public fixture member attributes");
                members[index].bytes = Read(paths[index]);
            }
            level += L"1";
        }
    }
    void Check(const char* phase) const {
        const std::string label = phase;
        for (const auto& members : levels)
            for (const auto& member : members) {
                bool present = false;
                Need(pmm_versions::Exists(member.path, present), "inspect final public member");
                Need(present == member.present,
                     (label + ": public PMM/EMM presence matches the independent baseline").c_str());
                if (!present) continue;
                Need(fs::is_regular_file(member.path),
                     (label + ": public member remains a regular file").c_str());
                Need(GetFileAttributesW(member.path.c_str()) == member.attributes,
                     (label + ": public member keeps its full attributes").c_str());
                Need(Read(member.path) == member.bytes,
                     (label + ": public member keeps its exact bytes").c_str());
            }
    }
    void Write(std::ofstream& out, const char* phase) const {
        for (size_t level = 0; level < levels.size(); ++level)
            for (size_t index = 0; index < levels[level].size(); ++index) {
                const auto& member = levels[level][index];
                out << "phase=" << phase << " level=" << level
                    << " member=" << (index ? "emm" : "pmm") << " present=" << member.present;
                if (member.present)
                    out << " bytes=" << member.bytes.size() << " attributes=" << member.attributes;
                out << '\n';
            }
    }
};
struct OriginalDeniedOperation {
    ObservedFileOp kind = ObservedFileOp::Move;
    fs::path from, to, lockPath;
    DWORD flags = 0;
    const char* label = "";
    const char* lockLabel = "";
};
OriginalDeniedOperation DenialOperationFor(OriginalDenial point, const fs::path& target, const OriginalFiles& files) {
    OriginalDeniedOperation result;
    switch (point) {
    case OriginalDenial::ForwardPublish:
        result.kind = ObservedFileOp::Move;
        result.from = files.newMain;
        result.to = target;
        result.flags = MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING;
        result.lockPath = files.newMain;
        result.label = "publish-new-pmm";
        result.lockLabel = "new-pmm-stage";
        break;
    case OriginalDenial::ForwardJournal:
        result.kind = ObservedFileOp::OpenForDelete;
        result.from = files.journal;
        result.lockPath = files.journal;
        result.label = "complete-recovery-journal";
        result.lockLabel = "recovery-journal";
        break;
    case OriginalDenial::RollbackEffect:
        result.kind = ObservedFileOp::OpenForDelete;
        result.from = pmm_paths::Effect(fs::path(target.native() + L"11"));
        result.lockPath = result.from;
        result.label = "restore-historical-emm";
        result.lockLabel = "historical-emm-at-11";
        break;
    case OriginalDenial::RollbackJournal:
        result.kind = ObservedFileOp::OpenForDelete;
        result.from = files.journal;
        result.lockPath = files.journal;
        result.label = "complete-recovery-journal";
        result.lockLabel = "recovery-journal";
        break;
    case OriginalDenial::RollbackEffectCleanup:
        // Open the rolled-back EMM for verified deletion after the regular stages.
        result.kind = ObservedFileOp::OpenForDelete;
        result.from = files.oldEffect;
        result.lockPath = files.oldEffect;
        result.label = "remove-rolled-back-emm";
        result.lockLabel = "rolled-back-emm-stage";
        break;
    }
    return result;
}
void RunOriginalSharingDenialCase(const fs::path& directory, OriginalDenial point, bool readonly = false) {
    Need(!readonly || point == OriginalDenial::RollbackEffectCleanup,
         "the read-only variant is defined only for the rolled-back EMM cleanup denial");
    const auto dir = fs::absolute(directory).lexically_normal();
    Need(fs::create_directory(dir), "require a fresh isolated original sharing-denial fixture");
    std::fwprintf(stderr, L"SAVE-01 original sharing-denied case=%ls variant=%ls fixture=%ls\n",
                  DenialName(point), readonly ? L"readonly" : L"writable", dir.c_str());
    const auto id = Nonce();
    Write(dir / L"owner.id", std::string(id.begin(), id.end()));
    Write(dir / L"phase.txt", DenialForward(point) ? "published" : "history");
    const auto target = dir / L"active.pmm", stage = dir / L"stage.pmm";
    Write(stage, oldestPmm);
    Need(pmm_versions::Publish(stage, target), "seed the sharing-denial oldest generation");
    Write(pmm_paths::Effect(target), oldestEmm);
    Write(stage, middlePmm);
    Need(pmm_versions::Publish(stage, target), "seed the sharing-denial middle generation");
    Write(pmm_paths::Effect(target), middleEmm, true);
    Need(Observe(target) == Before(), "sharing-denial fixture starts from exact independent PMM/EMM seed bytes");
    // Only the new read-only variant inherits a real attribute from its own
    // fixture. Both seeds are complete and the real writer has not started: the
    // active EMM is the only member marked, and the public active/1/11/111
    // PMM/EMM bytes, presence and full attributes become the independent
    // baseline for every later check.
    std::optional<OriginalPublicSnapshot> publicBaseline;
    DWORD activeEffectAttributes = INVALID_FILE_ATTRIBUTES;
    if (readonly) {
        const OriginalPublicSnapshot beforeMark(target);
        const auto activeEffect = pmm_paths::Effect(target);
        const auto attributes = GetFileAttributesW(activeEffect.c_str());
        Need(attributes != INVALID_FILE_ATTRIBUTES, "read the fresh fixture active EMM attributes");
        Need((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0,
             "the fresh fixture active EMM is a regular file");
        Need((attributes & FILE_ATTRIBUTE_READONLY) == 0, "the fresh fixture active EMM starts writable");
        Need(SetFileAttributesW(activeEffect.c_str(), attributes | FILE_ATTRIBUTE_READONLY) != FALSE,
             "add READONLY to only the new fixture's active EMM before the writer starts");
        const OriginalPublicSnapshot marked(target);
        // Re-read the attribute instead of assuming how NORMAL and READONLY
        // combine; the added READONLY bit itself is the requirement.
        Need((marked.levels[0][1].attributes & FILE_ATTRIBUTE_READONLY) != 0,
             "the active EMM carries the added READONLY bit");
        Need(marked.levels[0][1].bytes == beforeMark.levels[0][1].bytes,
             "adding READONLY does not change the active EMM bytes");
        for (size_t level = 0; level < marked.levels.size(); ++level)
            for (size_t index = 0; index < marked.levels[level].size(); ++index) {
                if (level == 0 && index == 1) continue;
                const auto& before = beforeMark.levels[level][index];
                const auto& after = marked.levels[level][index];
                Need(before.present == after.present && before.attributes == after.attributes &&
                     before.bytes == after.bytes,
                     "READONLY is added only to the new fixture's active EMM");
            }
        activeEffectAttributes = marked.levels[0][1].attributes;
        publicBaseline = marked;
        std::ofstream fixtureEvidence(dir / L"recovery-readonly-fixture.txt");
        Need(bool(fixtureEvidence), "open read-only fixture baseline evidence");
        fixtureEvidence << "pid=" << GetCurrentProcessId() << " case=" << DenialAscii(point)
                        << " variant=readonly-old-effect active_emm_attributes=" << attributes
                        << "->" << activeEffectAttributes << std::endl;
        publicBaseline->Write(fixtureEvidence, "baseline");
        fixtureEvidence.flush();
        Need(bool(fixtureEvidence), "flush read-only fixture baseline evidence");
    }
    const HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, EventName(id).c_str());
    const DWORD eventError = GetLastError();
    Handle event(handle);
    Need(eventError != ERROR_ALREADY_EXISTS, "own fresh sharing-denial barrier event");
    Child writer(DenialWriterMode(point), dir, id);
    try { writer.Await(event.value); }
    catch (...) {
        if (fs::exists(dir / L"child-error.txt")) std::printf("%s", Read(dir / L"child-error.txt").c_str());
        throw;
    }
    const auto journal = fs::path(target.native() + L".mms-save.json");
    const auto record = Read(journal);
    const auto parsed = ReadOriginalJournal(target);
    const OriginalFiles files(target, parsed.saveId);
    Need(parsed.record == record, "journal bytes are the exact production record");
    Need(parsed.forward == DenialForward(point), "hard-interrupted journal carries the expected durable decision");
    AuthenticateOriginalJournal(target, parsed, files);
    CheckOriginalTransaction(target, files, CrashExpectation(point), record, stage, "crash-state");
    std::printf("verified paused original writer: case=%s decision=%s journal=1\n",
                DenialAscii(point), parsed.forward ? "forward" : "rollback");
    writer.Interrupt();
    Need(writer.Join("sharing-denial-writer-killed") == killed, "sharing-denial writer ended by owned hard interruption");
    CheckOriginalTransaction(target, files, CrashExpectation(point), record, stage, "after-writer-kill");
    CheckOriginalLocksReleased(target, "after-writer-kill");
    const auto operation = DenialOperationFor(point, target, files);
    std::fwprintf(stderr, L"SAVE-01 original sharing-denied lock=%ls from=%ls to=%ls\n",
                 operation.lockPath.c_str(), operation.from.c_str(), operation.to.c_str());
    // The checked rolled-back EMM discard runs only after the regular stages
    // are gone; bind the real attributes before the read-only handle pins it.
    DWORD heldEffectAttributes = INVALID_FILE_ATTRIBUTES;
    if (point == OriginalDenial::RollbackEffectCleanup) {
        Need(fs::exists(files.oldEffect), "the rolled-back EMM stage exists before its denial");
        heldEffectAttributes = GetFileAttributesW(files.oldEffect.c_str());
        Need(heldEffectAttributes != INVALID_FILE_ATTRIBUTES,
             "the rolled-back EMM stage reports real attributes");
        Need((heldEffectAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0,
             "the rolled-back EMM stage is a regular file");
        if (readonly) {
            // This is the production copy, not a fixture rewrite. It must
            // already carry the fixture's real READONLY and the exact middle
            // EMM bytes before any calibration or sharing hold touches it.
            // Evidence is written before the precondition assertion so a
            // mismatch is reported, never repaired by the test.
            const bool inherited = (heldEffectAttributes & FILE_ATTRIBUTE_READONLY) != 0;
            const bool middle = Read(files.oldEffect) == middleEmm;
            const std::wstring effectNameW = files.oldEffect.filename().native();
            const std::string effectName(effectNameW.begin(), effectNameW.end());
            std::ofstream fixtureEvidence(dir / L"recovery-readonly-fixture.txt", std::ios::app);
            Need(bool(fixtureEvidence), "append inherited read-only stage evidence");
            fixtureEvidence << "inherited_effect=" << effectName << " attributes=" << heldEffectAttributes
                            << " readonly=" << inherited << " middle_bytes=" << bool(middle)
                            << " bytes=" << Read(files.oldEffect).size() << std::endl;
            fixtureEvidence.flush();
            Need(bool(fixtureEvidence), "flush inherited read-only stage evidence");
            Need(inherited, "the production rolled-back EMM stage inherited the fixture's READONLY");
            Need(middle, "the production rolled-back EMM stage is the exact middle EMM copy");
        }
    }
    std::ofstream evidence(dir / L"recovery-denied.txt");
    Need(bool(evidence), "open original sharing-denial evidence");
    evidence << "case=" << DenialAscii(point) << " variant=" << (readonly ? "readonly" : "writable")
             << " pid=" << GetCurrentProcessId()
             << " decision=" << (parsed.forward ? "forward" : "rollback")
             << " lock=" << operation.lockLabel << " journal_record=\"" << record << "\"\n";
    if (readonly)
        evidence << "baseline_active_emm_attributes=" << activeEffectAttributes
                 << " baseline_public_generations=4x2 journal=1\n";
    DWORD readonlyDeleteCalibration = ERROR_SUCCESS;
    if (readonly) {
        // Calibration only: the real API, with no sharing hold, refuses the
        // still-READONLY staged copy. Nothing is removed or rewritten.
        const BOOL removed = DeleteFileW(files.oldEffect.c_str());
        readonlyDeleteCalibration = GetLastError();
        Need(removed == FALSE && readonlyDeleteCalibration == ERROR_ACCESS_DENIED,
             "a direct delete of the read-only rolled-back EMM stage reports ERROR_ACCESS_DENIED");
        Need(fs::is_regular_file(files.oldEffect) &&
             Read(files.oldEffect) == middleEmm &&
             GetFileAttributesW(files.oldEffect.c_str()) == heldEffectAttributes,
             "the direct read-only calibration leaves bytes and full attributes unchanged");
        evidence << "calibration_readonly_delete=" << readonlyDeleteCalibration
                 << " unchanged=1 attributes=" << heldEffectAttributes << '\n';
        std::ofstream fixtureEvidence(dir / L"recovery-readonly-fixture.txt", std::ios::app);
        Need(bool(fixtureEvidence), "append read-only calibration evidence");
        fixtureEvidence << "calibration_readonly_delete=" << readonlyDeleteCalibration
                        << " unchanged=1" << std::endl;
        fixtureEvidence.flush();
        Need(bool(fixtureEvidence), "flush read-only calibration evidence");
    }
    {
        // READ sharing only: recovery may read the held member, but Windows
        // denies the replacement, inverse rename or deletion through this
        // handle. Bytes, attributes and ACL stay untouched.
        Handle held(CreateFileW(operation.lockPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD sharingCalibration = ERROR_SUCCESS;
        if (readonly) {
            // Calibrate with the existing DELETE-access probe. Never assume how
            // READONLY and the sharing hold combine in a raw delete.
            sharingCalibration = DeleteAccessProbe(files.oldEffect);
            Need(sharingCalibration == ERROR_SHARING_VIOLATION,
                 "the held read-only rolled-back EMM stage denies DELETE access with ERROR_SHARING_VIOLATION");
            Need(fs::is_regular_file(files.oldEffect) &&
                 Read(files.oldEffect) == middleEmm &&
                 GetFileAttributesW(files.oldEffect.c_str()) == heldEffectAttributes,
                 "the sharing calibration keeps the rolled-back EMM stage byte- and attribute-exact");
            evidence << "calibration_sharing_delete=" << sharingCalibration << '\n';
            std::ofstream fixtureEvidence(dir / L"recovery-readonly-fixture.txt", std::ios::app);
            Need(bool(fixtureEvidence), "append sharing calibration evidence");
            fixtureEvidence << "calibration_sharing_delete=" << sharingCalibration << std::endl;
            fixtureEvidence.flush();
            Need(bool(fixtureEvidence), "flush sharing calibration evidence");
        }
        auto app = std::make_unique<MMDApp>();
        {
            const auto state = std::make_unique<MMDAppState>();
            app->state = *state;
        }
        app->InitDefaults();
        for (unsigned attempt = 1; attempt <= 2; ++attempt) {
            unsigned calls = 0;
            BOOL nativeResult = TRUE;
            DWORD nativeError = ERROR_SUCCESS, nativeFlags = 0;
            DWORD observedAttributes = INVALID_FILE_ATTRIBUTES;
            bool accepted = true;
            {
                RecoveryFileProbe probe(operation.kind, operation.from, operation.to);
                accepted = plugins::PrepareProjectLoad(app.get(), target.c_str());
                plugins::CancelProjectLoad(app.get());
                calls = probe.observed.calls;
                nativeResult = probe.observed.result;
                nativeError = probe.observed.error;
                nativeFlags = probe.observed.flags;
                observedAttributes = probe.observed.attributesBefore;
            }
            const DWORD deleteProbe = DeleteAccessProbe(operation.lockPath);
            if (point == OriginalDenial::RollbackEffectCleanup) {
                // RED evidence precedes every assertion: the observed native
                // result, the production gate's return and the surviving
                // journal/stage must be visible even if the boundary is wrong.
                const bool journalPresent = fs::exists(files.journal);
                const bool effectPresent = fs::exists(files.oldEffect);
                const auto variant = readonly ? "readonly" : "writable";
                std::printf("RED case=%s variant=%s attempt=%u pid=%lu op=%s native_calls=%u native_result=%d "
                            "native_error=%lu attributes_before=%lu prepare_load=%d journal_present=%d old_effect_present=%d\n",
                            DenialAscii(point), variant, attempt, GetProcessId(GetCurrentProcess()), operation.label,
                            calls, int(nativeResult), nativeError, unsigned long(observedAttributes),
                            int(accepted), int(journalPresent), int(effectPresent));
                std::fflush(stdout);
                evidence << "RED case=" << DenialAscii(point) << " variant=" << variant
                         << " attempt=" << attempt
                         << " pid=" << GetProcessId(GetCurrentProcess()) << " op=" << operation.label
                         << " native_calls=" << calls << " native_result=" << int(nativeResult)
                         << " native_error=" << nativeError
                         << " attributes_before=" << observedAttributes
                         << " prepare_load=" << int(accepted)
                         << " journal_present=" << journalPresent
                         << " old_effect_present=" << effectPresent << '\n';
                evidence.flush();
                Need(bool(evidence), "flush original sharing-denial evidence");
            }
            if (readonly) {
                // A sharing conflict refuses the open before attributes change.
                const DWORD restoredAttributes = GetFileAttributesW(files.oldEffect.c_str());
                char readonlyLine[768]{};
                std::snprintf(readonlyLine, sizeof(readonlyLine),
                    "variant=readonly attempt=%u pid=%lu op=%s matched_open=%u attributes_before=%lu "
                    "readonly_cleared=%d open_result=%d open_error=%lu attributes_preserved=%d "
                    "share_probe=%lu prepare_load=%d",
                    attempt, GetProcessId(GetCurrentProcess()), operation.label, calls,
                    unsigned long(observedAttributes),
                    int(observedAttributes != INVALID_FILE_ATTRIBUTES &&
                        (observedAttributes & FILE_ATTRIBUTE_READONLY) == 0),
                    int(nativeResult), nativeError,
                    int(restoredAttributes == heldEffectAttributes), deleteProbe, int(accepted));
                std::printf("%s\n", readonlyLine);
                evidence << readonlyLine << '\n';
                evidence.flush();
                Need(bool(evidence), "flush read-only denial evidence");
            }
            Need(calls == 1, "observe exactly the one production recovery file operation");
            Need(nativeResult == FALSE, "the real recovery file operation is refused by Windows sharing");
            Need(nativeError == ERROR_SHARING_VIOLATION, "the real recovery file operation reports the Win32 sharing violation");
            if (operation.kind == ObservedFileOp::Move)
                Need(nativeFlags == operation.flags, "the denied move is the expected production rename");
            Need(!accepted, "the production pre-read gate reports the denied recovery as pending");
            Need(deleteProbe == ERROR_SHARING_VIOLATION, "the held read member still denies DELETE access");
            CheckOriginalTransaction(target, files, DeniedExpectation(point), record, stage, "denied-attempt");
            CheckOriginalLocksReleased(target, "denied-attempt");
            if (point == OriginalDenial::RollbackEffectCleanup) {
                Need(fs::is_regular_file(files.oldEffect),
                     "denied cleanup leaves the rolled-back EMM stage a regular file");
                Need(Read(files.oldEffect) == middleEmm,
                     "denied cleanup leaves the rolled-back EMM stage byte-exact");
                Need(GetFileAttributesW(files.oldEffect.c_str()) == heldEffectAttributes,
                     "denied cleanup leaves the rolled-back EMM stage attributes unchanged");
                if (readonly) {
                    Need(observedAttributes == heldEffectAttributes && (observedAttributes & FILE_ATTRIBUTE_READONLY),
                         "the sharing-denied open sees the original inherited READONLY attributes");
                    Need(Read(files.journal) == record,
                         "the denied read-only production delete leaves the journal byte-identical");
                    publicBaseline->Check("denied-attempt");
                }
            }
            char line[768]{};
            std::snprintf(line, sizeof(line),
                "case=%s variant=%s attempt=%u pid=%lu op=%s native_calls=%u native_result=%d native_error=%lu flags=0x%08lX prepare_load=%d delete_probe=%lu chain=exact journal=unchanged stages=exact caller_stage=untouched",
                DenialAscii(point), readonly ? "readonly" : "writable", attempt,
                GetProcessId(GetCurrentProcess()), operation.label, calls,
                int(nativeResult), nativeError, nativeFlags, int(accepted), deleteProbe);
            std::printf("%s\n", line);
            evidence << line << '\n';
            evidence.flush();
            Need(bool(evidence), "flush original sharing-denial evidence");
        }
    }
    if (readonly) {
        // The hold is released; the test itself never clears READONLY. The
        // real transaction copy and the independent public baseline must be
        // unchanged before two truly separate processes finish the decision.
        Need(fs::is_regular_file(files.oldEffect), "the released read-only transaction stage is a regular file");
        Need(Read(files.oldEffect) == middleEmm,
             "the released read-only transaction stage keeps the exact middle EMM bytes");
        Need(Read(files.journal) == record,
             "the released read-only transaction keeps its exact durable journal");
        const DWORD retainedAttributes = GetFileAttributesW(files.oldEffect.c_str());
        Need(retainedAttributes == heldEffectAttributes &&
             (retainedAttributes & FILE_ATTRIBUTE_READONLY) != 0,
             "the test never clears READONLY: the released transaction stage keeps its full attributes");
        publicBaseline->Check("released-hold");
        evidence << "released_hold_pid=" << GetCurrentProcessId() << " attributes=" << retainedAttributes
                 << " readonly=1 bytes=" << Read(files.oldEffect).size()
                 << " journal=1 public_generations_exact=1\n";
        std::ofstream fixtureEvidence(dir / L"recovery-readonly-fixture.txt", std::ios::app);
        Need(bool(fixtureEvidence), "append released read-only stage evidence");
        fixtureEvidence << "released_hold_pid=" << GetCurrentProcessId()
                        << " attributes=" << retainedAttributes << " readonly=1 stage=1" << std::endl;
        fixtureEvidence.flush();
        Need(bool(fixtureEvidence), "flush released read-only stage evidence");
        evidence.flush();
        Need(bool(evidence), "flush released read-only hold evidence");
    }
    // The held handle is closed; only independent processes may finish now.
    // Keep the first process object (and its handle) alive until the second
    // child is created: Windows cannot recycle that PID while the process
    // object is still referenced, so the distinct-PID assertion is stable.
    std::unique_ptr<Child> firstRetry = std::make_unique<Child>(L"--original-version-crash-recover", dir, id);
    const DWORD firstRetryPid = firstRetry->Pid();
    Need(firstRetryPid != 0, "read the first fresh original recovery process ID");
    CheckOriginalRetryEvidence(dir, target, files, FinalChain(point), stage, DenialForward(point),
                               *firstRetry, 1, firstRetryPid, evidence);
    if (readonly) {
        // Each independent process completes the production pre-read gate
        // twice; afterwards the public active/1/11/111 PMM/EMM bytes and full
        // attributes must still match the fixture's own baseline.
        publicBaseline->Check("fresh-retry-first");
        evidence << "fresh_process=1 pid=" << firstRetryPid
                 << " public_generations_exact=1 journal=0 stages=0 caller_stage=untouched level11=0 level111=0\n";
        evidence.flush();
        Need(bool(evidence), "flush first fresh read-only retry evidence");
    }
    std::unique_ptr<Child> secondRetry = std::make_unique<Child>(L"--original-version-crash-recover", dir, id);
    const DWORD secondRetryPid = secondRetry->Pid();
    Need(secondRetryPid != 0, "read the second fresh original recovery process ID");
    Need(secondRetryPid != firstRetry->Pid(), "the two fresh recovery processes have distinct PIDs");
    CheckOriginalRetryEvidence(dir, target, files, FinalChain(point), stage, DenialForward(point),
                               *secondRetry, 2, secondRetryPid, evidence);
    if (readonly) {
        publicBaseline->Check("fresh-retry-second");
        Need(GetFileAttributesW(pmm_paths::Effect(target).c_str()) == activeEffectAttributes,
             "the final active EMM still carries the fixture's READONLY attribute");
        evidence << "fresh_process=2 pid=" << secondRetryPid
                 << " public_generations_exact=1 journal=0 stages=0 caller_stage=untouched level11=0 level111=0\n";
        evidence.flush();
        Need(bool(evidence), "flush second fresh read-only retry evidence");
    }
    evidence << "PASS case=" << DenialAscii(point) << " variant=" << (readonly ? "readonly" : "writable")
             << " denied_attempts=2 fresh_processes=2 recoveries_per_process=2";
    if (readonly)
        evidence << " public_generations_exact=1 readonly_retained=1 fresh_pids="
                 << firstRetryPid << ',' << secondRetryPid;
    evidence << '\n';
    evidence.close();
    Need(!evidence.fail(), "close original sharing-denial evidence");
    std::printf("%s", Read(dir / L"recovery-denied.txt").c_str());
    std::printf("PASS SAVE-01 original sharing-denied case=%s variant=%s\n",
                DenialAscii(point), readonly ? "readonly" : "writable");
}
// The schema-3 rollback record owns a staged copy of the previous EMM. Its
// cleanup deletes that copy only after the bytes still match the recorded
// generation hash. This case replaces only that owned stage with a real
// regular file of different bytes and marks it READONLY: no sharing hold and no
// injected API failure. A production pre-read gate call may legitimately
// restore the public chain and remove its regular transaction stages before the
// hash check refuses the journal completion; that false result is the expected
// external-conflict evidence, not a recovery error. The test then resolves the
// conflict itself: its replacement is renamed to a non-transaction evidence
// name and the preserved production copy is restored. That repair is test-side
// evidence handling, never production behavior.
void RunOriginalExternalEffectConflictCase(const fs::path& directory) {
    const auto dir = fs::absolute(directory).lexically_normal();
    Need(fs::create_directory(dir), "require a fresh isolated external-effect fixture");
    std::fwprintf(stderr, L"SAVE-01 original external-effect fixture=%ls\n", dir.c_str());
    const auto id = Nonce();
    Write(dir / L"owner.id", std::string(id.begin(), id.end()));
    Write(dir / L"phase.txt", "history");
    const auto target = dir / L"active.pmm", stage = dir / L"stage.pmm";
    Write(stage, oldestPmm);
    Need(pmm_versions::Publish(stage, target), "seed external-effect oldest generation");
    Write(pmm_paths::Effect(target), oldestEmm);
    Write(stage, middlePmm);
    Need(pmm_versions::Publish(stage, target), "seed external-effect middle generation");
    Write(pmm_paths::Effect(target), middleEmm, true);
    Need(Observe(target) == Before(), "external-effect fixture starts from exact independent PMM/EMM seed bytes");
    const OriginalPublicSnapshot publicBaseline(target);
    std::ofstream evidence(dir / L"external-effect-conflict.txt");
    Need(bool(evidence), "open external-effect conflict evidence");
    evidence << "pid=" << GetCurrentProcessId()
             << " fixture=external-effect-replaced-old-effect journal_written_by=production\n";
    publicBaseline.Write(evidence, "baseline");
    evidence.flush();
    Need(bool(evidence), "flush external-effect baseline evidence");
    const HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, EventName(id).c_str());
    const DWORD eventError = GetLastError();
    Handle event(handle);
    Need(eventError != ERROR_ALREADY_EXISTS, "own fresh external-effect barrier event");
    Child writer(L"--original-version-crash-write-recovery", dir, id);
    try { writer.Await(event.value); }
    catch (...) {
        if (fs::exists(dir / L"child-error.txt")) std::printf("%s", Read(dir / L"child-error.txt").c_str());
        throw;
    }
    const auto journal = fs::path(target.native() + L".mms-save.json");
    const auto record = Read(journal);
    const auto parsed = ReadOriginalJournal(target);
    const OriginalFiles files(target, parsed.saveId);
    Need(parsed.record == record, "external-effect journal bytes are the exact production record");
    Need(!parsed.forward, "hard-interrupted external-effect journal carries the rollback decision");
    AuthenticateOriginalJournal(target, parsed, files);
    const OriginalExpectation crash = {PmmEffectCrashChain(), OptionalText(freshPmm),
                                        OptionalText(middlePmm), OptionalText(middleEmm)};
    CheckOriginalTransaction(target, files, crash, record, stage, "crash-state");
    Need(fs::exists(files.oldEffect), "the rolled-back EMM stage exists before the external replacement");
    const auto journalJson = save_journal_fixture::Parse(record);
    const std::string generationHash = journalJson.At("versions").Items()[0].Items()[1].String();
    Need(generationHash == Sha256Hex(Read(files.oldEffect)),
         "the journal generation hash binds the production rolled-back EMM stage");
    std::printf("verified paused original external-effect writer: decision=rollback old_effect=1 journal=1\n");
    writer.Interrupt();
    Need(writer.Join("external-effect-writer-killed") == killed,
         "external-effect writer ended by owned hard interruption");
    CheckOriginalTransaction(target, files, crash, record, stage, "after-writer-kill");
    CheckOriginalLocksReleased(target, "after-writer-kill");
    // Preserve the production copy itself: a same-directory rename keeps both its
    // bytes and its full attributes for the later restore.
    const auto originalEvidence = dir / L"evidence-original-old-effect.emm";
    const auto replacementEvidence = dir / L"evidence-external-replacement.emm";
    const DWORD originalEffectAttributes = GetFileAttributesW(files.oldEffect.c_str());
    Need(originalEffectAttributes != INVALID_FILE_ATTRIBUTES,
         "read the production rolled-back EMM stage attributes");
    Need((originalEffectAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0,
         "the production rolled-back EMM stage is a regular file");
    const std::string originalEffect = Read(files.oldEffect);
    Need(originalEffect == middleEmm, "the production rolled-back EMM stage is the exact middle EMM copy");
    Need(MoveFileExW(files.oldEffect.c_str(), originalEvidence.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
         "preserve the production rolled-back EMM stage under a non-transaction evidence name");
    Need(!fs::exists(files.oldEffect), "the production stage path is vacant after preservation");
    Need(fs::is_regular_file(originalEvidence) && Read(originalEvidence) == originalEffect &&
         GetFileAttributesW(originalEvidence.c_str()) == originalEffectAttributes,
         "the preserved production stage keeps its exact bytes and full attributes");
    const std::string replaced = "external conflict replacement content, not the saved EMM generation";
    const std::string originalHash = Sha256Hex(originalEffect);
    const std::string replacementHash = Sha256Hex(replaced);
    Need(replaced != originalEffect && replacementHash != generationHash && replacementHash != originalHash,
         "the external replacement bytes differ from the recorded generation hash");
    Write(files.oldEffect, replaced);
    const DWORD replacementAttributes = static_cast<DWORD>(
        (originalEffectAttributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY)) | FILE_ATTRIBUTE_READONLY);
    Need(SetFileAttributesW(files.oldEffect.c_str(), replacementAttributes) != FALSE,
         "mark the external replacement READONLY");
    const DWORD observedReplacementAttributes = GetFileAttributesW(files.oldEffect.c_str());
    Need(observedReplacementAttributes == replacementAttributes,
         "the external replacement keeps its marked full attributes");
    Need((observedReplacementAttributes & FILE_ATTRIBUTE_READONLY) != 0,
         "the external replacement carries the READONLY attribute");
    Need(fs::is_regular_file(files.oldEffect) && Read(files.oldEffect) == replaced &&
         Sha256Hex(Read(files.oldEffect)) == replacementHash,
         "the external replacement is a real regular file with its exact bytes");
    Need(Read(files.journal) == record, "the external replacement leaves the production journal byte-identical");
    Need(Observe(target) == PmmEffectCrashChain(),
         "the external replacement does not change the public crash chain");
    Need(fs::exists(files.newMain) && fs::exists(files.oldMain),
         "the regular transaction stages still precede the production cleanup");
    const std::wstring effectNameW = files.oldEffect.filename().native();
    const std::string effectName(effectNameW.begin(), effectNameW.end());
    evidence << "phase=external-replacement old_effect=" << effectName
             << " generation_sha256=" << generationHash
             << " original_bytes=\"" << originalEffect << "\" original_sha256=" << originalHash
             << " original_attributes=" << originalEffectAttributes
             << " replacement_bytes=\"" << replaced << "\" replacement_sha256=" << replacementHash
             << " replacement_attributes=" << observedReplacementAttributes
             << " replacement_readonly=1 sharing_hold=0 injected_failure=0 journal=1\n";
    evidence.flush();
    Need(bool(evidence), "flush external replacement evidence");
    auto app = std::make_unique<MMDApp>();
    {
        const auto state = std::make_unique<MMDAppState>();
        app->state = *state;
    }
    app->InitDefaults();
    const auto publicMatchesBaseline = [&] {
        for (const auto& members : publicBaseline.levels)
            for (const auto& member : members) {
                bool present = false;
                if (!pmm_versions::Exists(member.path, present)) return false;
                if (present != member.present) return false;
                if (!present) continue;
                if (!fs::is_regular_file(member.path)) return false;
                if (GetFileAttributesW(member.path.c_str()) != member.attributes) return false;
                if (Read(member.path) != member.bytes) return false;
            }
        return true;
    };
    const auto locksReleased = [&] {
        fs::path level = target;
        for (int index = 0; index < 4; ++index) {
            if (fs::exists(fs::path(level.native() + L".mms-save.lock"))) return false;
            level += L"1";
        }
        return true;
    };
    const auto replacementIntact = [&] {
        if (!fs::is_regular_file(files.oldEffect)) return false;
        const DWORD attributes = GetFileAttributesW(files.oldEffect.c_str());
        return Read(files.oldEffect) == replaced && attributes == replacementAttributes &&
            (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    };
    const auto replacementEvidenceIntact = [&] {
        if (!fs::is_regular_file(replacementEvidence)) return false;
        const DWORD attributes = GetFileAttributesW(replacementEvidence.c_str());
        return Read(replacementEvidence) == replaced && attributes == replacementAttributes &&
            (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    };
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        unsigned calls = 0;
        bool accepted = true;
        {
            RecoveryFileProbe probe(ObservedFileOp::Disposition, files.oldEffect);
            accepted = plugins::PrepareProjectLoad(app.get(), target.c_str());
            plugins::CancelProjectLoad(app.get());
            calls = probe.observed.calls;
        }
        const bool replacementPresent = fs::exists(files.oldEffect);
        const bool replacementRegular = fs::is_regular_file(files.oldEffect);
        const DWORD replacementNow = GetFileAttributesW(files.oldEffect.c_str());
        const bool replacementBytes = replacementPresent && Read(files.oldEffect) == replaced;
        const bool journalPresent = fs::exists(files.journal);
        const bool journalSame = journalPresent && Read(files.journal) == record;
        const bool chainBefore = Observe(target) == Before();
        const bool baselineMatch = publicMatchesBaseline();
        const bool newMainGone = !fs::exists(files.newMain);
        const bool oldMainGone = !fs::exists(files.oldMain);
        const bool tempJournalGone = !fs::exists(files.tempJournal);
        const bool callerStageExact = fs::exists(stage) && Read(stage) == freshPmm;
        const bool released = locksReleased();
        const std::string conflictPhase = "external-conflict-attempt-" + std::to_string(attempt);
        char line[1536]{};
        std::snprintf(line, sizeof(line),
            "phase=external-conflict attempt=%u pid=%lu prepare_load=%d old_effect_delete_calls=%u "
            "replacement_present=%d replacement_regular=%d replacement_bytes_exact=%d "
            "replacement_attributes=%lu replacement_attributes_exact=%d replacement_readonly=%d "
            "journal_present=%d journal_unchanged=%d chain_before=%d baseline_match=%d "
            "new_main_gone=%d old_main_gone=%d temp_journal_gone=%d caller_stage_exact=%d locks_released=%d",
            attempt, GetProcessId(GetCurrentProcess()), int(accepted), calls,
            int(replacementPresent), int(replacementRegular), int(replacementBytes),
            unsigned long(replacementNow), int(replacementNow == replacementAttributes),
            int(replacementPresent && (replacementNow & FILE_ATTRIBUTE_READONLY) != 0),
            int(journalPresent), int(journalSame), int(chainBefore), int(baselineMatch),
            int(newMainGone), int(oldMainGone), int(tempJournalGone), int(callerStageExact), int(released));
        std::printf("%s\n", line);
        evidence << line << '\n';
        evidence.flush();
        Need(bool(evidence), "flush external-effect conflict evidence");
        // Evidence for this attempt is durable before every behavior assertion.
        Need(!accepted, "the external conflict keeps the production pre-read gate pending");
        Need(calls == 0, "the production recovery never deletes the externally replaced rolled-back EMM stage");
        Need(replacementPresent && replacementRegular && replacementBytes,
             "the externally replaced rolled-back EMM stage keeps its exact bytes");
        Need(replacementNow == replacementAttributes,
             "the externally replaced rolled-back EMM stage keeps its full attributes");
        Need((replacementNow & FILE_ATTRIBUTE_READONLY) != 0,
             "the externally replaced rolled-back EMM stage stays READONLY");
        Need(journalSame, "the external conflict leaves the production journal byte-identical");
        Need(chainBefore, "the production call legitimately restored the original PMM/EMM generation chain");
        publicBaseline.Check(conflictPhase.c_str());
        Need(newMainGone && oldMainGone && tempJournalGone,
             "cleanup removed the regular transaction stages before the external conflict");
        Need(callerStageExact, "the production pre-read gate keeps the caller stage untouched");
        Need(released, "the production pre-read gate releases every save lock");
        CheckOriginalLocksReleased(target, conflictPhase.c_str());
        std::printf("attempt=%u external conflict: public chain restored=%d journal kept=%d "
                    "old-effect delete calls=%u replacement unchanged=%d\n",
                    attempt, int(chainBefore), int(journalSame), calls, int(replacementIntact()));
    }
    // The first gate call legitimately restored the public chain; its false
    // result comes only from the external old-EMM conflict. The second call is
    // idempotent and keeps the same journal and replacement evidence.
    // Test-side resolution of that conflict follows. Production does not repair
    // a replaced staged copy by itself.
    Need(MoveFileExW(files.oldEffect.c_str(), replacementEvidence.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
         "rename only the test-owned external replacement to its evidence name");
    Need(!fs::exists(files.oldEffect), "the production stage path is vacant before the preserved copy returns");
    Need(fs::is_regular_file(replacementEvidence) && Read(replacementEvidence) == replaced &&
         GetFileAttributesW(replacementEvidence.c_str()) == replacementAttributes &&
         (GetFileAttributesW(replacementEvidence.c_str()) & FILE_ATTRIBUTE_READONLY) != 0,
         "the preserved external replacement keeps its exact READONLY bytes and attributes");
    Need(MoveFileExW(originalEvidence.c_str(), files.oldEffect.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
         "restore the preserved production rolled-back EMM stage copy");
    Need(!fs::exists(originalEvidence), "the preserved original stage copy is consumed by the restore");
    Need(fs::is_regular_file(files.oldEffect) && Read(files.oldEffect) == originalEffect &&
         GetFileAttributesW(files.oldEffect.c_str()) == originalEffectAttributes,
         "the restored production stage keeps the original bytes and full attributes");
    Need(Sha256Hex(Read(files.oldEffect)) == generationHash,
         "the restored production stage matches the recorded generation hash again");
    Need(Read(files.journal) == record, "the test-side conflict resolution does not modify the journal");
    const std::wstring replacementNameW = replacementEvidence.filename().native();
    const std::string replacementName(replacementNameW.begin(), replacementNameW.end());
    evidence << "phase=test-side-conflict-resolution replacement_evidence=" << replacementName
             << " replacement_sha256=" << replacementHash
             << " replacement_attributes=" << GetFileAttributesW(replacementEvidence.c_str())
             << " original_restored_sha256=" << generationHash
             << " original_restored_attributes=" << originalEffectAttributes
             << " journal=1 test_side_only=1 not_production_behavior=1\n";
    evidence.flush();
    Need(bool(evidence), "flush test-side conflict resolution evidence");
    std::printf("test-side conflict resolution: replacement preserved as %s; "
                "preserved production stage restored with journal untouched\n", replacementName.c_str());
    std::unique_ptr<Child> firstRetry = std::make_unique<Child>(L"--original-version-crash-recover", dir, id);
    const DWORD firstRetryPid = firstRetry->Pid();
    Need(firstRetryPid != 0, "read the first fresh external-effect recovery process ID");
    CheckOriginalRetryEvidence(dir, target, files, Before(), stage, false, *firstRetry, 1, firstRetryPid, evidence);
    const bool firstBaseline = publicMatchesBaseline();
    const bool firstReplacementIntact = replacementEvidenceIntact();
    evidence << "phase=fresh-retry retry=1 pid=" << firstRetryPid
             << " public_baseline_exact=" << int(firstBaseline)
             << " replacement_evidence=" << replacementName
             << " replacement_bytes_exact=" << int(firstReplacementIntact)
             << " replacement_attributes=" << GetFileAttributesW(replacementEvidence.c_str())
             << " journal=0 stages=0 caller_stage=untouched no_duplicate_rotation=" << int(Observe(target) == Before())
             << '\n';
    evidence.flush();
    Need(bool(evidence), "flush first fresh external-effect retry evidence");
    Need(firstBaseline, "the first fresh process restores the exact original public baseline");
    Need(firstReplacementIntact, "the first fresh process leaves the preserved external replacement untouched");
    publicBaseline.Check("external-effect-fresh-first");
    std::unique_ptr<Child> secondRetry = std::make_unique<Child>(L"--original-version-crash-recover", dir, id);
    const DWORD secondRetryPid = secondRetry->Pid();
    Need(secondRetryPid != 0, "read the second fresh external-effect recovery process ID");
    Need(secondRetryPid != firstRetry->Pid(), "the two fresh external-effect recovery processes have distinct PIDs");
    CheckOriginalRetryEvidence(dir, target, files, Before(), stage, false, *secondRetry, 2, secondRetryPid, evidence);
    const bool secondBaseline = publicMatchesBaseline();
    const bool secondReplacementIntact = replacementEvidenceIntact();
    const DWORD finalReplacementAttributes = GetFileAttributesW(replacementEvidence.c_str());
    const bool noFurtherRotation = Observe(target) == Before() && secondBaseline;
    evidence << "phase=fresh-retry retry=2 pid=" << secondRetryPid
             << " public_baseline_exact=" << int(secondBaseline)
             << " replacement_evidence=" << replacementName
             << " replacement_bytes_exact=" << int(secondReplacementIntact)
             << " replacement_attributes=" << finalReplacementAttributes
             << " journal=0 stages=0 caller_stage=untouched no_duplicate_rotation=" << int(noFurtherRotation)
             << '\n';
    evidence.flush();
    Need(bool(evidence), "flush second fresh external-effect retry evidence");
    Need(secondBaseline, "the second fresh process keeps the exact original public baseline");
    Need(secondReplacementIntact, "the second fresh process leaves the preserved external replacement untouched");
    Need(finalReplacementAttributes == replacementAttributes &&
         (finalReplacementAttributes & FILE_ATTRIBUTE_READONLY) != 0,
         "the preserved external replacement keeps its READONLY attribute through both fresh processes");
    Need(noFurtherRotation, "a second fresh process performs no further generation rotation");
    Need(!fs::exists(files.journal) && !fs::exists(files.oldEffect) && !fs::exists(files.newMain) &&
         !fs::exists(files.oldMain) && !fs::exists(files.tempJournal),
         "both fresh processes leave the journal and every transaction stage removed");
    publicBaseline.Check("external-effect-fresh-second");
    evidence << "PASS case=external-effect-replaced old_effect_delete_calls=0 prepare_load_failures=2 "
                "test_side_conflict_resolution=1 fresh_processes=2 recoveries_per_process=2 "
                "replacement_readonly=1 journal=0 stages=0 public_generations_exact=1 fresh_pids="
             << firstRetryPid << ',' << secondRetryPid << '\n';
    evidence.close();
    Need(!evidence.fail(), "close external-effect conflict evidence");
    std::printf("%s", Read(dir / L"external-effect-conflict.txt").c_str());
    std::puts("PASS SAVE-01 original external rolled-back EMM conflict and two fresh-process retries");
}
enum class OriginalNewMainFault { ReadOnly, Replacement };

// Both faults start at the same real schema-3 rollback point. The observer
// forwards the disposition call on the exact new-PMM file handle.
void RunOriginalNewMainCleanupCase(const fs::path& directory, OriginalNewMainFault fault) {
    const bool replaced = fault == OriginalNewMainFault::Replacement;
    const auto dir = fs::absolute(directory).lexically_normal();
    Need(fs::create_directory(dir), "require a fresh original new-PMM cleanup fixture");
    const auto id = Nonce();
    Write(dir / L"owner.id", std::string(id.begin(), id.end()));
    Write(dir / L"phase.txt", "history");
    const auto target = dir / L"active.pmm", stage = dir / L"stage.pmm";
    Write(stage, oldestPmm);
    Need(pmm_versions::Publish(stage, target), "seed new-PMM cleanup oldest generation");
    Write(pmm_paths::Effect(target), oldestEmm);
    Write(stage, middlePmm);
    Need(pmm_versions::Publish(stage, target), "seed new-PMM cleanup middle generation");
    Write(pmm_paths::Effect(target), middleEmm, true);
    Need(Observe(target) == Before(), "new-PMM cleanup independent seed bytes");
    const OriginalPublicSnapshot baseline(target);
    const HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, EventName(id).c_str());
    const DWORD eventError = GetLastError();
    Handle event(handle);
    Need(eventError != ERROR_ALREADY_EXISTS, "own fresh new-PMM cleanup barrier");
    Child writer(L"--original-version-crash-write", dir, id);
    writer.Await(event.value);
    const auto parsed = ReadOriginalJournal(target);
    const OriginalFiles files(target, parsed.saveId);
    Need(!parsed.forward, "new-PMM cleanup fixture has a durable rollback decision");
    AuthenticateOriginalJournal(target, parsed, files);
    const auto crashed = CrashExpectation(OriginalDenial::RollbackJournal);
    CheckOriginalTransaction(target, files, crashed, parsed.record, stage, "paused-writer");
    writer.Interrupt();
    Need(writer.Join(replaced ? "newmain-replacement-writer-killed" : "newmain-readonly-writer-killed") == killed,
         "owned writer was hard-interrupted");
    CheckOriginalTransaction(target, files, crashed, parsed.record, stage, "interrupted-writer");
    CheckOriginalLocksReleased(target, "interrupted-writer");
    const DWORD savedAttributes = GetFileAttributesW(files.newMain.c_str());
    Need(savedAttributes != INVALID_FILE_ATTRIBUTES &&
         !(savedAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
         "new-PMM stage starts as a writable regular file");
    struct RestoreOwnedAttributes {
        fs::path path;
        DWORD saved;
        bool pending = false;
        ~RestoreOwnedAttributes() {
            if (pending && !SetFileAttributesW(path.c_str(), saved))
                std::fprintf(stderr, "FIXTURE new-PMM attribute restore failed (Win32 %lu)\n", GetLastError());
        }
        void Restore() {
            Need(SetFileAttributesW(path.c_str(), saved) != FALSE, "clear only the fixture's read-only fault");
            pending = false;
        }
    } restore{files.newMain, savedAttributes};
    const auto report = dir / (replaced ? L"newmain-replacement.txt" : L"newmain-readonly.txt");
    std::ofstream evidence(report);
    Need(bool(evidence), "open new-PMM cleanup recovery evidence");
    evidence << "pid=" << GetCurrentProcessId() << " journal=" << parsed.record << std::endl;
    baseline.Write(evidence, "baseline");
    const auto originalEvidence = dir / L"evidence-original-newmain.pmm";
    const auto replacementEvidence = dir / L"evidence-replacement-newmain.pmm";
    const std::string replacement = "external writable replacement at the new PMM stage";
    auto faulted = crashed;
    DWORD faultAttributes = INVALID_FILE_ATTRIBUTES;
    if (replaced) {
        const auto replacementHash = Sha256Hex(replacement);
        Need(replacementHash != parsed.newMain, "replacement differs from the journal's new PMM hash");
        Need(MoveFileExW(files.newMain.c_str(), originalEvidence.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
             "preserve the production new-PMM copy inside the isolated fixture");
        Need(Read(originalEvidence) == freshPmm && GetFileAttributesW(originalEvidence.c_str()) == savedAttributes,
             "preserved new-PMM copy keeps its original bytes and attributes");
        Write(files.newMain, replacement);
        faultAttributes = GetFileAttributesW(files.newMain.c_str());
        Need(faultAttributes != INVALID_FILE_ATTRIBUTES &&
             !(faultAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
             "replacement is a writable regular file");
        Need(DeleteAccessProbe(files.newMain) == ERROR_SUCCESS, "replacement permits DELETE access");
        faulted.newMain = replacement;
        evidence << "replacement_sha256=" << replacementHash << " attributes=" << faultAttributes
                 << " delete_access=1" << std::endl;
    } else {
        Need(SetFileAttributesW(files.newMain.c_str(), savedAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
             "make only the owned new-PMM stage read-only");
        restore.pending = true;
        faultAttributes = GetFileAttributesW(files.newMain.c_str());
        Need(faultAttributes != INVALID_FILE_ATTRIBUTES && (faultAttributes & FILE_ATTRIBUTE_READONLY),
             "new-PMM stage has a real read-only attribute");
        SetLastError(ERROR_SUCCESS);
        const BOOL calibration = DeleteFileW(files.newMain.c_str());
        const DWORD calibrationError = GetLastError();
        evidence << "calibration_delete=" << int(calibration) << " error=" << calibrationError
                 << " attributes=" << faultAttributes << std::endl;
        Need(!calibration && calibrationError == ERROR_ACCESS_DENIED,
             "real read-only new-PMM deletion is denied by Windows");
    }
    CheckOriginalTransaction(target, files, faulted, parsed.record, stage, "calibration");
    auto app = std::make_unique<MMDApp>();
    {
        const auto state = std::make_unique<MMDAppState>();
        app->state = *state;
    }
    app->InitDefaults();
    auto denied = faulted;
    denied.chain = Before();
    for (unsigned attempt : {1u, 2u}) {
        ObservedFileFailure observed;
        bool accepted = true;
        {
            RecoveryFileProbe probe(ObservedFileOp::Disposition, files.newMain);
            accepted = plugins::PrepareProjectLoad(app.get(), target.c_str());
            plugins::CancelProjectLoad(app.get());
            observed = probe.observed;
        }
        const DWORD retainedAttributes = GetFileAttributesW(files.newMain.c_str());
        // Record actual results before asserting the recovery boundary.
        evidence << "attempt=" << attempt << " native_calls=" << observed.calls
                 << " native_result=" << int(observed.result) << " native_error=" << observed.error
                 << " attributes_before=" << observed.attributesBefore << " attributes_after=" << retainedAttributes
                 << " prepare_load=" << accepted << " journal_present=" << fs::exists(files.journal)
                 << " stage_present=" << fs::exists(files.newMain) << std::endl;
        Need(bool(evidence), "flush denied new-PMM recovery evidence");
        if (replaced) {
            Need(observed.calls == 0, "production cleanup never marks the replacement new PMM for deletion");
        } else {
            Need(observed.calls == 1 && !observed.result && observed.error == ERROR_ACCESS_DENIED,
                 "production cleanup really attempted and failed the new-PMM deletion");
            Need(observed.attributesBefore == faultAttributes, "native deletion saw the read-only new-PMM attributes");
        }
        Need(retainedAttributes == faultAttributes, "failed new-PMM cleanup preserves its complete attributes");
        Need(!accepted, "original pre-read gate refuses incomplete cleanup");
        CheckOriginalTransaction(target, files, denied, parsed.record, stage, "denied-newmain-cleanup");
        baseline.Check("denied-newmain-cleanup");
        CheckOriginalLocksReleased(target, "denied-newmain-cleanup");
        evidence << "attempt=" << attempt << " public_generations_exact=1 stages_exact=1 journal_unchanged=1 locks=0" << std::endl;
    }
    const auto checkReplacementEvidence = [&] {
        Need(Read(replacementEvidence) == replacement &&
             GetFileAttributesW(replacementEvidence.c_str()) == faultAttributes,
             "preserved replacement keeps its exact bytes and writable attributes");
    };
    if (replaced) {
        Need(MoveFileExW(files.newMain.c_str(), replacementEvidence.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
             "move the writable replacement to its fixture evidence path");
        checkReplacementEvidence();
        Need(MoveFileExW(originalEvidence.c_str(), files.newMain.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
             "restore the preserved production new-PMM copy to resolve the conflict");
        denied.newMain = crashed.newMain;
    } else {
        restore.Restore();
    }
    Need(GetFileAttributesW(files.newMain.c_str()) == savedAttributes, "restore the exact original new-PMM attributes");
    CheckOriginalTransaction(target, files, denied, parsed.record, stage, "fault-cleared");
    baseline.Check("fault-cleared");
    evidence << "fault_cleared=1 attributes=" << savedAttributes << std::endl;
    // Keep both handles alive so the OS cannot recycle the first child's PID.
    Child first(L"--original-version-crash-recover", dir, id);
    const DWORD firstPid = first.Pid();
    Need(firstPid != 0 && firstPid != GetCurrentProcessId(), "first retry is a separate process");
    CheckOriginalRetryEvidence(dir, target, files, Before(), stage, false, first, 1, firstPid, evidence);
    baseline.Check("first-fresh-process");
    if (replaced) checkReplacementEvidence();
    Child second(L"--original-version-crash-recover", dir, id);
    const DWORD secondPid = second.Pid();
    Need(secondPid != 0 && secondPid != firstPid && secondPid != GetCurrentProcessId(), "second retry is another fresh process");
    CheckOriginalRetryEvidence(dir, target, files, Before(), stage, false, second, 2, secondPid, evidence);
    baseline.Check("second-fresh-process");
    if (replaced) checkReplacementEvidence();
    evidence << (replaced ? "PASS replaced_newmain delete_calls=0" : "PASS readonly_newmain")
             << " denied_attempts=2 fresh_processes=2 recoveries_per_process=2"
             << " public_generations_exact=1 fresh_pids=" << firstPid << ',' << secondPid << std::endl;
    evidence.close();
    Need(!evidence.fail(), "close new-PMM cleanup recovery evidence");
    std::printf("%s", Read(report).c_str());
    std::puts(replaced ? "PASS SAVE-01 original writable new-PMM replacement and two fresh-process retries" :
                         "PASS SAVE-01 original read-only new-PMM cleanup and two fresh-process retries");
}
void RunOriginalReadOnlyNewMainCase(const fs::path& directory) {
    RunOriginalNewMainCleanupCase(directory, OriginalNewMainFault::ReadOnly);
}
void RunOriginalReplacedNewMainCase(const fs::path& directory) {
    RunOriginalNewMainCleanupCase(directory, OriginalNewMainFault::Replacement);
}
int Run(const fs::path& directory) {
    const auto root=fs::absolute(directory).lexically_normal();
    Need(fs::create_directory(root), "require a fresh isolated run directory");
    RunCase(root/L"history",false);
    RunCase(root/L"published",true);
    RunCase(root/L"recovery-interrupted",false,true);
    RunForwardRecoveryCase(root/L"forward-recovery-interrupted");
    FailureControls(root/L"failure-controls");
    RunOriginalSharingDenialCase(root/L"sharing-denied-forward-publish",OriginalDenial::ForwardPublish);
    RunOriginalSharingDenialCase(root/L"sharing-denied-forward-journal",OriginalDenial::ForwardJournal);
    RunOriginalSharingDenialCase(root/L"sharing-denied-rollback-effect",OriginalDenial::RollbackEffect);
    RunOriginalSharingDenialCase(root/L"sharing-denied-rollback-journal",OriginalDenial::RollbackJournal);
    RunOriginalSharingDenialCase(root/L"sharing-denied-rollback-effect-cleanup",OriginalDenial::RollbackEffectCleanup);
    RunOriginalSharingDenialCase(root/L"sharing-denied-rollback-effect-cleanup-readonly",
                                 OriginalDenial::RollbackEffectCleanup,true);
    RunOriginalExternalEffectConflictCase(root/L"external-effect-replaced");
    RunOriginalReadOnlyNewMainCase(root/L"readonly-newmain-cleanup");
    RunOriginalReplacedNewMainCase(root/L"external-newmain-replaced");
    return 0;
}
int Dispatch(int argc, wchar_t** argv) {
    fs::path childDir;
    try {
        const std::wstring mode = argv[1];
        if (argc == 3 && mode == L"--original-version-crash-only") return Run(argv[2]);
        if (argc == 3 && mode == L"--original-version-forward-crash-only") { RunForwardRecoveryCase(argv[2]);return 0; }
        if (argc == 3 && mode == L"--original-version-readonly-newmain-only") { RunOriginalReadOnlyNewMainCase(argv[2]);return 0; }
        if (argc == 3 && mode == L"--original-version-replaced-newmain-only") { RunOriginalReplacedNewMainCase(argv[2]);return 0; }
        Need(argc == 4 && (mode == L"--original-version-crash-write" || mode == L"--original-version-crash-write-published" ||
                          mode == L"--original-version-crash-write-recovery" || mode == L"--original-version-crash-recover" ||
                          mode == L"--original-version-crash-write-forward" ||
                          mode == L"--original-version-crash-recover-forward-first" || mode == L"--original-version-crash-recover-forward-second" ||
                          mode == L"--original-version-crash-recover-interrupt"),
             "usage: --original-version-crash-only, --original-version-forward-crash-only, "
             "--original-version-readonly-newmain-only or --original-version-replaced-newmain-only <new-directory>");
        const std::wstring id = argv[3];
        Need(id.size() == 32 && id.find_first_not_of(L"0123456789abcdef") == id.npos, "validate child nonce");
        const auto dir = fs::absolute(argv[2]).lexically_normal();
        Handle event(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EventName(id).c_str()));
        Need(Read(dir / L"owner.id") == std::string(id.begin(), id.end()), "bind child to live parent's isolated fixture");
        childDir = dir;
        if(mode==L"--original-version-crash-recover-interrupt")Recovery(dir,event.value);
        else if(mode==L"--original-version-crash-recover-forward-first")Recovery(dir,nullptr,1);
        else if(mode==L"--original-version-crash-recover-forward-second")Recovery(dir,nullptr,2);
        else if(mode==L"--original-version-crash-recover")Recovery(dir);
        else Writer(dir,event.value,mode==L"--original-version-crash-write-published",mode==L"--original-version-crash-write-recovery",
                    mode==L"--original-version-crash-write-forward");
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL SAVE-01 original: %s\n", error.what());
        if (!childDir.empty()) {
            std::ofstream report(childDir / L"child-error.txt");
            report << "FAIL child: " << error.what() << std::endl;
        }
        return 1;
    }
}
} // namespace original_version_crash
