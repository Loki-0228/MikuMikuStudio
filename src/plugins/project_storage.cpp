#include "project_storage.hpp"
#include "json.hpp"
#include "scene_digest.hpp"
#include "../io/pmm_save_versions.hpp"
#include <array>
#include <cstddef>
#include <fstream>
#include <new>
#include <optional>

namespace mikumikustudio::plugins {
namespace {
constexpr size_t kMaxSidecar = 96 * 1024 * 1024;
constexpr char hex[] = "0123456789abcdef";
constexpr char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
bool Hex(std::string_view s, size_t n) { return s.size() == n && s.find_first_not_of("0123456789abcdef") == s.npos; }
std::string HexBytes(const unsigned char* p, size_t n) { std::string s; s.reserve(n*2); for(size_t i=0;i<n;++i){s+=hex[p[i]>>4];s+=hex[p[i]&15];} return s; }
std::string Encode(const std::vector<unsigned char>& data) {
    std::string out; out.reserve((data.size()+2)/3*4);
    for (size_t i=0;i<data.size();i+=3) {
        const uint32_t v=(uint32_t(data[i])<<16)|(i+1<data.size()?uint32_t(data[i+1])<<8:0)|(i+2<data.size()?data[i+2]:0);
        out+=base64[v>>18];out+=base64[(v>>12)&63];out+=i+1<data.size()?base64[(v>>6)&63]:'=';out+=i+2<data.size()?base64[v&63]:'=';
    } return out;
}
std::vector<unsigned char> Decode(std::string_view s) {
    if(s.size()%4) throw std::invalid_argument("base64 length");
    auto digit=[](char c)->uint32_t {const auto* p=std::strchr(base64,c);if(!p||!c)throw std::invalid_argument("base64 character");return uint32_t(p-base64);};
    std::vector<unsigned char> out; out.reserve(s.size()/4*3);
    for(size_t i=0;i<s.size();i+=4){
        const auto a=digit(s[i]),b=digit(s[i+1]); const bool p=s[i+2]=='=',q=s[i+3]=='=';
        if((p&&!q)||((p||q)&&i+4!=s.size()))throw std::invalid_argument("base64 padding");
        const auto c=p?0:digit(s[i+2]),d=q?0:digit(s[i+3]);
        if((p&&(b&15))||(q&&!p&&(c&3)))throw std::invalid_argument("base64 unused bits");
        out.push_back(static_cast<unsigned char>((a<<2)|(b>>4)));
        if(!p)out.push_back(static_cast<unsigned char>((b<<4)|(c>>2)));
        if(!q)out.push_back(static_cast<unsigned char>((c<<6)|d));
    } return out;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle(){Close();}
    void Close() noexcept {
        if(value==INVALID_HANDLE_VALUE)return;
        const DWORD error=GetLastError();
        CloseHandle(value);
        value=INVALID_HANDLE_VALUE;
        SetLastError(error);
    }
};
struct FileFailure : std::runtime_error {
    DWORD error;
    FileFailure(const char* action,DWORD code):std::runtime_error(std::string(action)+" (Win32 "+std::to_string(code)+")"),error(code?code:ERROR_GEN_FAILURE){}
};
void Check(bool ok, const char* action) { if(!ok)throw FileFailure(action,GetLastError()); }
struct Lock {
    Handle file;
    explicit Lock(const fs::path& p) { file.value=CreateFileW((p.native()+L".mms-save.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);Check(file.value!=INVALID_HANDLE_VALUE,"project lock"); }
};
using StageIdentity = std::optional<BY_HANDLE_FILE_INFORMATION>;

bool SameFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh &&
        a.nFileIndexLow==b.nFileIndexLow;
}

void WriteDurable(const fs::path& p,std::string_view bytes,StageIdentity& identity) {
    Handle f;
    f.value=CreateFileW(p.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(f.value!=INVALID_HANDLE_VALUE,"create staged file");
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(f.value,&info)!=FALSE,"inspect created stage");
    identity=info;
    size_t offset=0;while(offset<bytes.size()){DWORD n=0;const DWORD count=DWORD(std::min<size_t>(bytes.size()-offset,1024*1024));Check(WriteFile(f.value,bytes.data()+offset,count,&n,nullptr)!=FALSE&&n==count,"write staged file");offset+=n;}
    Check(FlushFileBuffers(f.value)!=FALSE,"flush staged file");
}
struct CopyDestination {
    StageIdentity& identity;
    Handle file;
    DWORD error=ERROR_SUCCESS;
};

DWORD CALLBACK CaptureCopyDestination(LARGE_INTEGER,LARGE_INTEGER,LARGE_INTEGER,LARGE_INTEGER,
                                      DWORD,DWORD,HANDLE,HANDLE destination,LPVOID context) {
    auto& copy=*static_cast<CopyDestination*>(context);
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(destination,&info)) {
        copy.error=GetLastError();
        return PROGRESS_STOP;
    }
    copy.identity=info;
    HANDLE owned=INVALID_HANDLE_VALUE;
    const auto process=GetCurrentProcess();
    if(!DuplicateHandle(process,destination,process,&owned,0,FALSE,DUPLICATE_SAME_ACCESS)) {
        copy.error=GetLastError();
        return PROGRESS_STOP;
    }
    copy.file.value=owned;
    return PROGRESS_QUIET;
}

void CopyDurable(const fs::path& from,const fs::path& to,StageIdentity& identity) {
    // The destination handle exists from the first stream callback. Keep a
    // duplicate to flush the same file after CopyFileEx has applied metadata.
    CopyDestination copy{identity};
    const BOOL copied=CopyFileExW(from.c_str(),to.c_str(),CaptureCopyDestination,&copy,nullptr,COPY_FILE_FAIL_IF_EXISTS);
    if(copy.error!=ERROR_SUCCESS)throw FileFailure("inspect copied stage",copy.error);
    Check(copied!=FALSE,"copy project file");
    if(!identity || copy.file.value==INVALID_HANDLE_VALUE)
        throw FileFailure("capture copied stage identity",ERROR_INVALID_HANDLE);
    Check(FlushFileBuffers(copy.file.value)!=FALSE,"flush copied file");
}
void Move(const fs::path& from,const fs::path& to,bool replace=true){Check(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=FALSE,"publish project file");}
struct Paths {
    fs::path main,side,journal,newMain,newSide,oldMain,oldSide,tempJournal,oldEffect;
    bool compactEffect, plugins;
    Paths(const fs::path& target,const std::string& id,bool compact=false,bool withPlugins=true):main(target),side(PluginSidecar(target)),journal(target.native()+L".mms-save.json"),compactEffect(compact),plugins(withPlugins){
        if(!Hex(id,32))throw std::invalid_argument("save ID");
        auto base=target.native()+L".mms-"+Wide(id);
        newMain=base+L".new-pmm";newSide=base+L".new-json";oldMain=base+L".old-pmm";oldSide=base+L".old-json";tempJournal=base+L".journal";
        // Existing v2 journals used the longer name. New journals explicitly
        // select a same-directory name independent of the project's filename.
        oldEffect=compactEffect?target.parent_path()/(L".mms-"+Wide(id)+L".old-emm"):
            fs::path(oldMain.native()+L".emm");
    }
    fs::path OldEffect() const { return oldEffect; }
};
bool Matches(const fs::path& p,const std::string& hash){
    if(hash.empty())return false;
    bool present=false;Check(pmm_versions::Exists(p,present),"inspect transaction member");
    return present&&FileHash(p)==hash;
}

struct StageHashes {
    std::string_view newMain, newSide, oldMain, oldSide;
};

std::string HashOpenFile(HANDLE file) {
    Digest digest;
    std::array<char,65536> bytes{};
    for (;;) {
        DWORD count=0;
        Check(::ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)!=FALSE,"hash recovery stage");
        if(!count)break;
        digest.Add(bytes.data(),count);
    }
    const auto result=digest.Finish();
    return HexBytes(result.data(),result.size());
}

bool OpenTransactionMemberForDelete(Handle& file,const fs::path& path,BY_HANDLE_FILE_INFORMATION& info,
                                    DWORD extraAccess=0,const char* operation="remove recovery stage") {
    // Keep the opened object stable from verification through rename or deletion.
    file.value=CreateFileW(path.c_str(),GENERIC_READ|DELETE|extraAccess,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.value==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND)return false;
        throw FileFailure(operation,error);
    }
    Check(GetFileInformationByHandle(file.value,&info)!=FALSE,"inspect retained member");
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
        throw FileFailure("inspect retained member",ERROR_ACCESS_DENIED);
    return true;
}

void OpenVerifiedStage(Handle& file,const fs::path& path,std::string_view expected,const char* member) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!OpenTransactionMemberForDelete(file,path,info))return;
    if(expected.empty()||HashOpenFile(file.value)!=expected)
        throw std::runtime_error(std::string("recovery stage changed outside interrupted save: ")+member);
}

BOOL MarkStageForDeletion(HANDLE file) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file,FileDispositionInfo,&disposition,sizeof(disposition));
}

void DeleteOwnedStage(HANDLE file,const BY_HANDLE_FILE_INFORMATION& info) {
    const DWORD attributes=info.dwFileAttributes;
    const bool readOnly=(attributes&FILE_ATTRIBUTE_READONLY)!=0;
    const auto setAttributes=[&](DWORD value){
        FILE_BASIC_INFO basic{};
        basic.FileAttributes=value?value:FILE_ATTRIBUTE_NORMAL;
        return SetFileInformationByHandle(file,FileBasicInfo,&basic,sizeof(basic));
    };
    if(readOnly) {
        // Clearing READONLY affects all hard links to this file.
        if(info.nNumberOfLinks!=1)throw std::runtime_error("read-only stage has other links");
        Check(setAttributes(attributes&~FILE_ATTRIBUTE_READONLY)!=FALSE,"clear recovery stage read-only");
    }
    if(MarkStageForDeletion(file)!=FALSE)return;
    const DWORD deleteError=GetLastError();
    if(readOnly&&setAttributes(attributes)==FALSE)
        throw FileFailure("restore recovery stage attributes",GetLastError());
    throw FileFailure("remove recovery stage",deleteError);
}

void DiscardUnpublishedStage(const fs::path& path,const StageIdentity& identity) noexcept {
    if(!identity)return;
    try {
        Handle file;
        BY_HANDLE_FILE_INFORMATION info{};
        if(OpenTransactionMemberForDelete(file,path,info,FILE_WRITE_ATTRIBUTES)&&SameFile(*identity,info))
            DeleteOwnedStage(file.value,info);
    } catch(...) { /* Preserve the primary error and any file of uncertain ownership. */ }
}

struct JournalRecord {
    std::string bytes;
    StageIdentity identity;
};

bool SameJournal(const JournalRecord& expected, const JournalRecord& observed) {
    return expected.identity && observed.identity && SameFile(*expected.identity,*observed.identity) &&
        expected.bytes==observed.bytes;
}

std::string ReadJournalBytes(HANDLE file) {
    LARGE_INTEGER size{};
    Check(GetFileSizeEx(file,&size)!=FALSE,"size recovery record");
    if(size.QuadPart<0||size.QuadPart>1024*1024)throw std::runtime_error("recovery record size");
    std::string bytes(static_cast<size_t>(size.QuadPart),'\0');
    DWORD read=0;
    Check(::ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE,"read recovery record");
    if(read!=bytes.size())throw std::runtime_error("short recovery record");
    return bytes;
}

JournalRecord ReadJournal(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(file,&info)!=FALSE,"inspect recovery record");
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
        throw FileFailure("inspect recovery record",ERROR_ACCESS_DENIED);
    return {ReadJournalBytes(file),info};
}

JournalRecord ReadJournal(const fs::path& path) {
    Handle file;
    file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    Check(file.value!=INVALID_HANDLE_VALUE,"open recovery record");
    return ReadJournal(file.value);
}

constexpr std::string_view kForwardDecision="MMS-SAVE-FORWARD\n";
constexpr std::string_view kRollbackDecision="MMS-SAVE-ROLLBACK\n";

struct ParsedJournal {
    Json json;
    bool forward=false;
    std::string_view partial;
};

ParsedJournal ParseJournal(std::string_view bytes) {
    const auto end=bytes.rfind('}');
    if(end==bytes.npos)throw std::invalid_argument("recovery record object");
    auto json=Json::Parse(bytes.substr(0,end+1));
    auto tail=bytes.substr(end+1);
    const auto start=tail.find_first_not_of(" \t\r\n");
    tail=start==tail.npos?std::string_view{}:tail.substr(start);
    if(json.At("schemaVersion").UInt()<2) {
        if(!tail.empty())throw std::invalid_argument("unexpected recovery decision");
        return {std::move(json)};
    }
    const auto initial=json.At("decision").String();
    bool forward=initial=="forward";
    if(!forward&&initial!="rollback")throw std::invalid_argument("retention decision");
    // A legacy record may start at either decision. Each transaction can
    // commit forward once and later fall back to rollback once.
    const auto consume=[&](std::string_view marker) {
        if(tail.substr(0,marker.size())!=marker)return false;
        tail.remove_prefix(marker.size());
        return true;
    };
    const auto partial=[&](std::string_view marker) {
        if(tail.size()>=marker.size()||marker.substr(0,tail.size())!=tail)
            throw std::invalid_argument("recovery decision trailer");
        return ParsedJournal{std::move(json),forward,tail};
    };
    if(!forward) {
        if(!consume(kForwardDecision))return partial(kForwardDecision);
        forward=true;
    }
    if(!consume(kRollbackDecision))return partial(kRollbackDecision);
    if(!tail.empty())throw std::invalid_argument("recovery decision trailer");
    return {std::move(json),false};
}

void RemoveJournal(const fs::path& path,const JournalRecord& expected) {
    Handle file;
    JournalRecord observed;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!OpenTransactionMemberForDelete(file,path,info,0,"complete recovery record"))
        throw FileFailure("complete recovery record",ERROR_FILE_NOT_FOUND);
    observed.identity=info;
    observed.bytes=ReadJournalBytes(file.value);
    if(!SameJournal(expected,observed))throw std::runtime_error("recovery record changed before cleanup");
    Check(MarkStageForDeletion(file.value)!=FALSE,"complete recovery record");
}

// Remove owned stages before the recovery journal. Until the journal is gone
// a stage failure leaves authentic recovery evidence for a later retry;
// deleting it first would drop that record. The continuation runs after the
// regular stages and before the journal.
template<class FinishStages>
void Cleanup(const Paths& p,const StageHashes& hashes,const JournalRecord& journal,FinishStages&& finishStages){
    // Verify all surviving regular stages before removing any of them. A
    // name that was absent at verification never gets reopened for deletion.
    std::array<Handle,4> stages;
    OpenVerifiedStage(stages[0],p.newMain,hashes.newMain,"new PMM");
    OpenVerifiedStage(stages[1],p.oldMain,hashes.oldMain,"previous PMM");
    if(p.plugins) {
        OpenVerifiedStage(stages[2],p.newSide,hashes.newSide,"new plugin sidecar");
        OpenVerifiedStage(stages[3],p.oldSide,hashes.oldSide,"previous plugin sidecar");
    }
    // The successful journal move already consumed tempJournal. A file that
    // later appears at that name was never part of this recovery.
    for(auto& stage:stages) {
        if(stage.value==INVALID_HANDLE_VALUE)continue;
        Check(MarkStageForDeletion(stage.value)!=FALSE,"remove recovery stage");
        stage.Close();
    }
    finishStages();
    RemoveJournal(p.journal,journal);
}
void Cleanup(const Paths& p,const StageHashes& hashes,const JournalRecord& journal){Cleanup(p,hashes,journal,[]{});}
void ResolvePair(const Paths& p,const std::string& nm,const std::string& ns,const std::string& om,const std::string& os,bool forward){
    if(forward){
        if(!Matches(p.main,nm))Move(p.newMain,p.main);
        if(p.plugins&&!Matches(p.side,ns))Move(p.newSide,p.side);
        Check(Matches(p.main,nm)&&(!p.plugins||Matches(p.side,ns)),"verify recovered pair");
    }else{
        if((!om.empty()&&!Matches(p.main,om)&&!Matches(p.oldMain,om))||(p.plugins&&!os.empty()&&!Matches(p.side,os)&&!Matches(p.oldSide,os)))throw std::runtime_error("neither saved pair is recoverable");
        auto restore=[](const fs::path& staged,const fs::path& dst,const std::string& hash){if(hash.empty()){if(fs::exists(dst))Check(DeleteFileW(dst.c_str())!=FALSE,"remove incomplete new file");}else if(!Matches(dst,hash))Move(staged,dst);};
        restore(p.oldMain,p.main,om);if(p.plugins)restore(p.oldSide,p.side,os);
        Check((om.empty()?!fs::exists(p.main):Matches(p.main,om))&&(!p.plugins||(os.empty()?!fs::exists(p.side):Matches(p.side,os))),"verify restored previous pair");
    }
}

using Level = std::array<std::string,3>;
using Chain = std::vector<Level>;

std::string MemberHash(const fs::path& path) {
    bool present = false;
    Check(pmm_versions::Exists(path,present),"inspect retained member");
    return present ? FileHash(path) : std::string{};
}

void MoveRetainedMember(const fs::path& from, const fs::path& to, std::string_view expected) {
    Handle file;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!OpenTransactionMemberForDelete(file, from, info, 0, "open retained member for rename") ||
        HashOpenFile(file.value) != expected)
        throw std::runtime_error("retention source changed before rename");

    const auto name = fs::absolute(to).native();
    const auto nameBytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* rename = ::new (buffer.data()) FILE_RENAME_INFO{};
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(buffer.data() + offsetof(FILE_RENAME_INFO, FileName), name.c_str(), nameBytes + sizeof(wchar_t));
    // A late arrival at the destination makes the rename fail in the filesystem.
    Check(SetFileInformationByHandle(file.value, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())) != FALSE,
          "rename retained member");
}
// A v2 journal contains only hashes and level count; every filename is
// derived from the requested target and the validated transaction ID.
class Archive {
    pmm_versions::Retention locks_;
    std::vector<std::vector<fs::path>> names_;
    Chain before_;
    struct Rename { size_t from, member; };
    std::vector<Rename> moves_;
    size_t members_ = 3;

    Level Observe(size_t level) const {
        Level result{};
        for(size_t i=0;i<members_;++i)result[i]=MemberHash(names_[level][i]);
        return result;
    }
    void SetUp(const Paths& p,size_t count) {
        if(!pmm_paths::IsProject(p.main))throw std::invalid_argument("retention requires a PMM target");
        members_=p.plugins?3:2;
        Check(locks_.Plan(p.main,p.plugins,names_,count),"lock retained version chain");
    }
    void FinishPlan(const Paths& p) {
        if(before_.size()<2||before_.front()[0].empty()||before_.back()!=Level{})
            throw std::invalid_argument("invalid retention endpoints");
        for(size_t i=before_.size()-1;i>0;--i) {
            if(before_[i-1]==Level{})throw std::invalid_argument("empty interior retention level");
            for(size_t member=0;member<members_;++member)
                if(!before_[i-1][member].empty())moves_.push_back({i-1,member});
        }
        // Install verified, flushed copies of the active generation by
        // rename. A failed copy can never leave a partial public .pmm1/.emm1.
        names_[0] = {p.oldMain,p.OldEffect(),p.oldSide};
        names_[0].resize(members_);
    }
public:
    void Prepare(const Paths& p,const std::string& om,const std::string& os,StageIdentity& createdEffect) {
        SetUp(p,0);
        for(size_t i=0;i<names_.size();++i)before_.push_back(Observe(i));
        if(before_.front()[0]!=om||before_.front()[2]!=os)
            throw std::runtime_error("active project changed while preparing retention");
        const auto& emm=before_.front()[1];
        if(!emm.empty())CopyDurable(names_[0][1],p.OldEffect(),createdEffect);
        if(MemberHash(p.OldEffect())!=emm)throw std::runtime_error("verify staged historical EMM");
        FinishPlan(p);
    }
    void Load(const Paths& p,const Json& j,const std::string& om,const std::string& os) {
        members_=p.plugins?3:2;
        const auto& rows=j.At("versions").Items();
        if(rows.size()<2||rows.size()>250)throw std::invalid_argument("retention depth");
        for(const auto& row:rows) {
            const auto& values=row.Items();
            if(values.size()!=members_)throw std::invalid_argument("retention member count");
            Level hashes{};
            for(size_t i=0;i<members_;++i) {
                hashes[i]=values[i].String();
                if(!hashes[i].empty()&&!Hex(hashes[i],64))throw std::invalid_argument("retention hash");
            }
            before_.push_back(std::move(hashes));
        }
        if(before_.front()[0]!=om||before_.front()[2]!=os)throw std::invalid_argument("retention active hashes");
        SetUp(p,rows.size());
        FinishPlan(p);
    }
    std::string Fields() const {
        std::string fields=",\"versions\":[";
        for(size_t i=0;i<before_.size();++i) {
            if(i)fields+=',';
            const auto& h=before_[i];
            fields+="[\""+h[0]+"\",\""+h[1]+"\"";
            if(members_==3)fields+=",\""+h[2]+"\"";
            fields+="]";
        }
        return fields+"]";
    }
    void Align(const Paths& p,bool forward,bool validateOnly=false) const {
        if(MemberHash(pmm_paths::Effect(p.main))!=before_.front()[1])
            throw std::runtime_error("active EMM changed outside interrupted save");
        Chain actual(names_.size()), projected=before_;
        for(size_t i=1;i<names_.size();++i)actual[i]=Observe(i);
        // The sum of occupied public level numbers increases on every rename,
        // even when two generations have identical hashes. Match a complete
        // reachable prefix before changing anything; holes are significant.
        size_t done=0;
        while(!std::equal(actual.begin()+1,actual.end(),projected.begin()+1)) {
            if(done==moves_.size())throw std::runtime_error("retained chain changed outside interrupted save");
            const auto op=moves_[done++];
            projected[op.from+1][op.member].swap(projected[op.from][op.member]);
        }
        const size_t goal=forward?moves_.size():0;
        // Validate the entire proposed suffix before the first mutation. In
        // particular, do not partially undo history into an occupied stage.
        actual[0]=Observe(0);
        for(size_t cursor=done;cursor!=goal;) {
            const auto op=moves_[forward?cursor:cursor-1];
            auto& from=actual[op.from+(forward?0:1)][op.member];
            auto& to=actual[op.from+(forward?1:0)][op.member];
            if(from!=before_[op.from][op.member]||!to.empty())
                throw std::runtime_error("retention move conflicts with existing file");
            from.swap(to);
            if(forward)++cursor;else --cursor;
        }
        if(!forward) {
            for(size_t member : {size_t{0},size_t{2}}) {
                if(member>=members_)continue;
                const auto& expected=before_.front()[member];
                const auto& active=member==0?p.main:p.side;
                if(!expected.empty()&&MemberHash(active)!=expected&&actual[0][member]!=expected)
                    throw std::runtime_error("previous active member is not recoverable");
            }
        }
        if(validateOnly)return;
        while(done!=goal) {
            const auto op=moves_[forward?done:done-1];
            const auto& from=names_[op.from+(forward?0:1)][op.member];
            const auto& to=names_[op.from+(forward?1:0)][op.member];
            MoveRetainedMember(from,to,before_[op.from][op.member]);
            if(forward)++done;else --done;
        }
    }
    void Verify(bool forward) const {
        for(size_t i=1;i<names_.size();++i)
            if(Observe(i)!=before_[forward?i-1:i])throw std::runtime_error("verify retained version chain");
    }
    // Called between the regular stage removals and the journal deletion.
    // Only the transaction's own rolled-back EMM copy is removed, and only
    // after its recorded generation hash and regular-file form are verified.
    void DiscardRolledBackEffect(const Paths& p) const {
        const auto& expected=before_.front()[1];
        Handle file;
        BY_HANDLE_FILE_INFORMATION info{};
        if(!OpenTransactionMemberForDelete(file,p.OldEffect(),info,FILE_WRITE_ATTRIBUTES))return;
        if(expected.empty()||HashOpenFile(file.value)!=expected)
            throw std::runtime_error("rolled-back EMM stage does not match its saved generation");
        DeleteOwnedStage(file.value,info);
    }
};

// Keep the canonical journal present and pin the exact object from identity
// verification through the durable write. An interrupted append leaves the
// prior complete decision readable with, at most, a prefix of the next one.
void ChangeDecision(const Paths& p,JournalRecord& expected,bool forward) {
    Handle file;
    file.value=CreateFileW(p.journal.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_WRITE_THROUGH,nullptr);
    Check(file.value!=INVALID_HANDLE_VALUE,"open recovery decision");
    auto observed=ReadJournal(file.value);
    if(!SameJournal(expected,observed))throw std::runtime_error("retention journal was replaced");
    const auto decision=ParseJournal(observed.bytes);
    if(decision.forward==forward)throw std::logic_error("retention decision already recorded");
    const auto marker=forward?kForwardDecision:kRollbackDecision;
    const auto remaining=marker.substr(decision.partial.size());
    observed.bytes.append(remaining);
    DWORD written=0;
    Check(WriteFile(file.value,remaining.data(),static_cast<DWORD>(remaining.size()),&written,nullptr)!=FALSE,
        "write recovery decision");
    if(written!=remaining.size())throw FileFailure("short recovery decision write",ERROR_WRITE_FAULT);
    Check(FlushFileBuffers(file.value)!=FALSE,"flush recovery decision");
    expected=std::move(observed);
}
std::string ArchiveRecord(const Paths& p,const std::string& base,const Archive& archive) {
    return base+archive.Fields()+(p.compactEffect?",\"effectStage\":1":"")+
        ",\"decision\":\"rollback\"}";
}
std::string RecordBase(unsigned schema,const std::string& id,const std::string& nm,const std::string& ns,
                       const std::string& om,const std::string& os) {
    if(schema==3)return "{\"schemaVersion\":3,\"members\":\"pmm-emm\",\"saveId\":\""+id+
        "\",\"newMain\":\""+nm+"\",\"oldMain\":\""+om+"\"";
    return "{\"schemaVersion\":"+std::to_string(schema)+",\"saveId\":\""+id+"\",\"newMain\":\""+nm+
        "\",\"newSide\":\""+ns+"\",\"oldMain\":\""+om+"\",\"oldSide\":\""+os+"\"";
}
void Recover(const fs::path& target){
    const auto journal=fs::path(target.native()+L".mms-save.json");if(!fs::exists(journal))return;
    auto record=ReadJournal(journal);
    const auto parsed=ParseJournal(record.bytes);
    const auto& j=parsed.json;
    const unsigned schema=j.At("schemaVersion").UInt();
    if(schema!=1&&schema!=2&&schema!=3)throw std::invalid_argument("recovery schema");
    const bool plugins=schema!=3;
    if(!plugins&&(j.At("members").String()!="pmm-emm"||j.object.count("newSide")||j.object.count("oldSide")))
        throw std::invalid_argument("recovery members");
    const auto id=j.At("saveId").String();
    const auto effectStage=j.object.find("effectStage");
    const bool compactEffect=effectStage!=j.object.end();
    if((compactEffect&&(schema==1||effectStage->second.UInt()!=1))||(!plugins&&!compactEffect))throw std::invalid_argument("recovery effect stage");
    const Paths p(target,id,compactEffect,plugins);
    const auto nm=j.At("newMain").String(),ns=plugins?j.At("newSide").String():std::string{},
        om=j.At("oldMain").String(),os=plugins?j.At("oldSide").String():std::string{};
    if(!Hex(nm,64)||(plugins&&!Hex(ns,64))||(!om.empty()&&!Hex(om,64))||(!os.empty()&&!Hex(os,64)))throw std::invalid_argument("recovery hash");
    const auto known=[&](const fs::path& f,const std::string& old,const std::string& fresh){
        const auto hash=MemberHash(f);return hash.empty()||hash==old||hash==fresh;
    };
    const auto verifyActive=[&]{
        if(!known(p.main,om,nm)||(plugins&&!known(p.side,os,ns)))throw std::runtime_error("project changed outside interrupted save");
    };
    verifyActive();
    const auto canForward=[&]{return (Matches(p.main,nm)||Matches(p.newMain,nm))&&(!plugins||(Matches(p.side,ns)||Matches(p.newSide,ns)));};
    Archive archive;
    bool forward=false;
    if(schema>=2) {
        archive.Load(p,j,om,os);
        if(parsed.forward) {
            forward=canForward();
            if(!forward) {
                archive.Align(p,false,true);
                ChangeDecision(p,record,false);
            }
        }
        archive.Align(p,forward);
        verifyActive();
    } else forward=canForward();
    ResolvePair(p,nm,ns,om,os,forward);
    if(schema>=2)archive.Verify(forward);
    const StageHashes hashes{nm,ns,om,os};
    if(schema>=2&&!forward)Cleanup(p,hashes,record,[&]{archive.DiscardRolledBackEffect(p);});
    else Cleanup(p,hashes,record);
}

// After a crash the OS has released all version locks. An independent open
// or save of a .pmm1 member must not adopt a half-shifted generation before
// its owning active project's journal is reconciled.
void CheckArchiveOwner(const fs::path& target) {
    fs::path owner=target;
    for(size_t depth=1;pmm_paths::IsProject(owner)&&owner.native().back()==L'1';++depth) {
        owner=owner.native().substr(0,owner.native().size()-1);
        const fs::path journal=owner.native()+L".mms-save.json";
        if(!fs::exists(journal))continue;
        const auto bytes=ReadFile(journal);
        const auto parsed=ParseJournal(bytes);
        const auto& j=parsed.json;
        const auto schema=j.At("schemaVersion").UInt();
        if(schema!=1&&schema!=2&&schema!=3)throw std::invalid_argument("retention owner schema");
        if(schema>=2) {
            if(schema==3&&j.At("members").String()!="pmm-emm")throw std::invalid_argument("retention owner members");
            const auto& rows=j.At("versions").Items();
            if(rows.size()<2||rows.size()>250)throw std::invalid_argument("retention owner depth");
            if(depth<rows.size())throw std::runtime_error("recover retained version owner first: "+Utf8(owner.native()));
        }
    }
}

void PublishOriginal(const fs::path& temporary,const fs::path& target) {
    Lock lock(target);
    CheckArchiveOwner(target);
    // A failure in a previous transaction is never treated as a failed new
    // save: leave its evidence untouched and let the caller report failure.
    Recover(target);
    if(!SamePath(target.parent_path(),temporary.parent_path())||SamePath(target,temporary))
        throw std::invalid_argument("staged PMM must be a distinct sibling");
    const auto fresh=MemberHash(temporary),old=MemberHash(target);
    if(fresh.empty())throw std::invalid_argument("missing staged PMM");
    if(old.empty()) { Move(temporary,target,false); return; }

    const auto id=StorageId();
    const Paths p(target,id,true,false);
    Archive archive;
    bool journalPublished=false,rollback=true,published=false,changingDecision=false;
    StageIdentity createdNew,createdOld,createdEffect;
    JournalRecord journal;
    try {
        CopyDurable(temporary,p.newMain,createdNew);
        CopyDurable(target,p.oldMain,createdOld);
        if(MemberHash(p.newMain)!=fresh||MemberHash(p.oldMain)!=old)
            throw std::runtime_error("verify original PMM stages");
        archive.Prepare(p,old,{},createdEffect);
        const auto base=RecordBase(3,id,fresh,{},old,{});
        journal.bytes=ArchiveRecord(p,base,archive);
        WriteDurable(p.tempJournal,journal.bytes,journal.identity);
        Move(p.tempJournal,p.journal,false);journalPublished=true;
        archive.Align(p,true);
        archive.Verify(true);
        changingDecision=true;
        ChangeDecision(p,journal,true);
        changingDecision=false;
        rollback=false;
        if(MemberHash(target)!=old||MemberHash(temporary)!=fresh)
            throw std::runtime_error("original PMM changed before publication");
        Move(temporary,target);published=true;
        if(MemberHash(target)!=fresh)throw std::runtime_error("verify published original PMM");
        archive.Align(p,true);
        archive.Verify(true);
        Cleanup(p,{fresh,{},old,{}},journal);
    } catch(...) {
        // Preserve the primary exception (including its Win32 code) even if
        // rollback cannot finish. A future load then owns the remaining work.
        // If the decision write failed, let a later recovery read the last
        // complete decision before moving or discarding any more members.
        if(journalPublished&&!published&&!changingDecision) {
            try {
                if(MemberHash(target)==old) {
                    archive.Align(p,false,true);
                    if(!rollback)ChangeDecision(p,journal,false);
                    archive.Align(p,false);
                    archive.Verify(false);
                    Cleanup(p,{fresh,{},old,{}},journal,[&]{archive.DiscardRolledBackEffect(p);});
                }
            }catch(...) {}
        }
        if(!journalPublished) {
            DiscardUnpublishedStage(p.newMain,createdNew);
            DiscardUnpublishedStage(p.oldMain,createdOld);
            DiscardUnpublishedStage(p.tempJournal,journal.identity);
            DiscardUnpublishedStage(p.oldEffect,createdEffect);
        }
        throw;
    }
}
}
std::string StorageId(){std::array<unsigned char,16> bytes{};if(BCryptGenRandom(nullptr,bytes.data(),ULONG(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("storage ID");return HexBytes(bytes.data(),bytes.size());}
std::string FileHash(const fs::path& path){
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("hash file open");Digest digest;std::array<char,65536> bytes{};
    while(f){f.read(bytes.data(),bytes.size());digest.Add(bytes.data(),size_t(f.gcount()));}if(!f.eof())throw std::runtime_error("hash file read");auto result=digest.Finish();return HexBytes(result.data(),result.size());
}
fs::path PluginSidecar(const fs::path& target){return target.native()+pmm_paths::PluginSidecarSuffix;}
std::string EncodeProject(const PluginProject& p){
    if(!Hex(p.projectId,32)||!Hex(p.saveId,32)||!Hex(p.mainHash,64)||p.records.size()>4096)throw std::invalid_argument("project metadata");
    std::string out="{\"schemaVersion\":1,\"projectId\":\""+p.projectId+"\",\"saveId\":\""+p.saveId+"\",\"mainSha256\":\""+p.mainHash+"\",\"plugins\":{";
    bool first=true;for(const auto& [id,v]:p.records){if(!ValidId(id)||!v.schema||v.bytes.size()>64ull*1024*1024)throw std::invalid_argument("plugin record");if(!first)out+=',';first=false;out+='"'+id+"\":{\"schemaVersion\":"+std::to_string(v.schema)+",\"data\":\""+Encode(v.bytes)+"\"}";if(out.size()>kMaxSidecar-2)throw std::invalid_argument("plugin sidecar size");}return out+"}}";
}
PluginProject DecodeProject(std::string_view bytes){
    auto j=Json::Parse(bytes,kMaxSidecar);if(j.At("schemaVersion").UInt()!=1)throw std::invalid_argument("plugin sidecar schema");
    PluginProject p;p.projectId=j.At("projectId").String();p.saveId=j.At("saveId").String();p.mainHash=j.At("mainSha256").String();
    if(!Hex(p.projectId,32)||!Hex(p.saveId,32)||!Hex(p.mainHash,64))throw std::invalid_argument("plugin sidecar metadata");
    const auto& entries=j.At("plugins");if(entries.kind!=Json::Object||entries.object.size()>4096)throw std::invalid_argument("plugin records");
    for(const auto& [id,v]:entries.object){if(!ValidId(id))throw std::invalid_argument("plugin ID");StoredData data;data.schema=v.At("schemaVersion").UInt();if(!data.schema)throw std::invalid_argument("plugin schema");data.bytes=Decode(v.At("data").String());if(data.bytes.size()>64ull*1024*1024)throw std::invalid_argument("plugin data size");p.records.emplace(id,std::move(data));}return p;
}
void RecoverProjectPair(const fs::path& target){CheckArchiveOwner(target);if(!fs::exists(target.native()+L".mms-save.json"))return;Lock lock(target);Recover(target);}
PluginProject ReadPluginProject(const fs::path& target){
    // Opening an intact project must also work from a read-only directory.
    // Hash validation rejects a pair observed during a concurrent replacement.
    try {
        RecoverProjectPair(target);
    } catch (const std::exception& error) {
        throw ProjectSaveRecoveryError(error.what());
    }
    if(!fs::exists(PluginSidecar(target))){PluginProject p;p.projectId=StorageId();return p;}
    auto p=DecodeProject(ReadFile(PluginSidecar(target),kMaxSidecar));if(FileHash(target)!=p.mainHash)throw std::runtime_error("PMM and plugin sidecar hashes differ; explicit association required");return p;
}
void PublishProjectPair(const fs::path& target,const fs::path& stagedMain,PluginProject& data,bool createOnly,const std::function<void(unsigned)>& step,bool keepPrevious){
    Lock lock(target);CheckArchiveOwner(target);Recover(target);
    if(!SamePath(fs::absolute(target).parent_path(),fs::absolute(stagedMain).parent_path()))throw std::invalid_argument("staged PMM must share directory");
    if(createOnly&&(fs::exists(target)||fs::exists(PluginSidecar(target))))throw std::runtime_error("project already exists");
    // Reject mismatched existing sidecars even when overwriting a different project.
    if(fs::exists(PluginSidecar(target))){auto previous=DecodeProject(ReadFile(PluginSidecar(target),kMaxSidecar));if(!Matches(target,previous.mainHash))throw std::runtime_error("existing plugin sidecar does not match PMM");}
    if(data.projectId.empty())data.projectId=StorageId();data.saveId=StorageId();data.mainHash=FileHash(stagedMain);
    const Paths p(target,data.saveId,true);
    bool journalPublished=false,firstMoveAttempted=false,destinationChanged=false;
    Archive archive;
    bool retained=false,archiveRollback=false,changingDecision=false;
    StageIdentity createdNewMain,createdNewSide,createdOldMain,createdOldSide,createdEffect;
    std::string newSideHash,oldMain,oldSide;
    JournalRecord journal;
    auto checkpoint=[&](unsigned n){if(step)step(n);};
    try{
        CopyDurable(stagedMain,p.newMain,createdNewMain);
        WriteDurable(p.newSide,EncodeProject(data),createdNewSide);
        oldMain=fs::exists(target)?FileHash(target):std::string{};oldSide=fs::exists(p.side)?FileHash(p.side):std::string{};
        if(!oldMain.empty())CopyDurable(target,p.oldMain,createdOldMain);
        if(!oldSide.empty())CopyDurable(p.side,p.oldSide,createdOldSide);
        Check((oldMain.empty()||Matches(p.oldMain,oldMain))&&(oldSide.empty()||Matches(p.oldSide,oldSide)),"verify previous pair backup");
        checkpoint(0);
        newSideHash=FileHash(p.newSide);
        Check(Matches(p.newMain,data.mainHash),"verify staged PMM");
        retained=keepPrevious&&!createOnly&&!oldMain.empty();
        const auto base=RecordBase(retained?2:1,data.saveId,data.mainHash,newSideHash,oldMain,oldSide);
        if(retained)archive.Prepare(p,oldMain,oldSide,createdEffect);
        journal.bytes=retained?ArchiveRecord(p,base,archive):base+"}";
        WriteDurable(p.tempJournal,journal.bytes,journal.identity);
        Move(p.tempJournal,p.journal,false);journalPublished=true;
        if(retained) {
            archiveRollback=true;
            archive.Align(p,true);
            archive.Verify(true);
            changingDecision=true;
            ChangeDecision(p,journal,true);
            changingDecision=false;
            archiveRollback=false;
        }
        checkpoint(1);
        if(retained&&(MemberHash(target)!=oldMain||MemberHash(p.side)!=oldSide))
            throw std::runtime_error("active project changed before retained publication");
        firstMoveAttempted=true;
        Move(p.newMain,p.main,!createOnly);destinationChanged=true;checkpoint(2);Move(p.newSide,p.side,!createOnly);checkpoint(3);
        Check(Matches(p.main,data.mainHash)&&Matches(p.side,newSideHash),"verify published pair");
        if(retained)archive.Verify(true);
        Cleanup(p,{data.mainHash,newSideHash,oldMain,oldSide},journal);checkpoint(4);
    }catch(...){
        // A failed first Move can leave the complete old pair untouched. Abort
        // that live attempt rather than silently publishing its rejected new
        // pair on a later read. Never infer this from a checkpoint interruption
        // or after any successful destination move: those retain crash recovery.
        if(journalPublished&&!changingDecision&&(archiveRollback||(firstMoveAttempted&&!destinationChanged))){
            try{
                const auto intact=[](const fs::path& f,const std::string& hash){return hash.empty()?!fs::exists(f):Matches(f,hash);};
                if(intact(p.main,oldMain)&&intact(p.side,oldSide)){
                    if(retained) {
                        if(!archiveRollback)ChangeDecision(p,journal,false);
                        archive.Align(p,false);
                        archive.Verify(false);
                        Cleanup(p,{data.mainHash,newSideHash,oldMain,oldSide},journal,[&]{archive.DiscardRolledBackEffect(p);});
                    } else Cleanup(p,{data.mainHash,newSideHash,oldMain,oldSide},journal);
                }
            }catch(...){/* Keep recovery evidence when verification/cleanup is uncertain. */}
        }
        // Before journal publication, only successful creations belong to this
        // attempt. After publication the recovery path owns cleanup, even if
        // it just completed a rollback above.
        if(!journalPublished) {
            DiscardUnpublishedStage(p.newMain,createdNewMain);
            DiscardUnpublishedStage(p.newSide,createdNewSide);
            DiscardUnpublishedStage(p.oldMain,createdOldMain);
            DiscardUnpublishedStage(p.oldSide,createdOldSide);
            DiscardUnpublishedStage(p.tempJournal,journal.identity);
            DiscardUnpublishedStage(p.oldEffect,createdEffect);
        }
        throw;
    }
}
}

namespace mikumikustudio::pmm_versions {
bool Publish(const fs::path& temporary,const fs::path& target) noexcept {
    try {
        plugins::PublishOriginal(fs::absolute(temporary),fs::absolute(target));
        return true;
    } catch(const plugins::FileFailure& failure) {
        SetLastError(failure.error);
    } catch(const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    } catch(...) {
        SetLastError(ERROR_INVALID_DATA);
    }
    return false;
}
}
