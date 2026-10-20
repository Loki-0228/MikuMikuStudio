#include "project_storage.hpp"
#include "json.hpp"
#include "scene_digest.hpp"
#include "../io/pmm_save_versions.hpp"
#include <array>
#include <fstream>

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
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
void Check(bool ok, const char* action) { if(!ok)throw std::runtime_error(std::string(action)+" (Win32 "+std::to_string(GetLastError())+")"); }
struct Lock {
    Handle file;
    explicit Lock(const fs::path& p) { file.value=CreateFileW((p.native()+L".mms-save.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,nullptr);Check(file.value!=INVALID_HANDLE_VALUE,"project lock"); }
};
void WriteDurable(const fs::path& p, std::string_view bytes) {
    Handle f; f.value=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Check(f.value!=INVALID_HANDLE_VALUE,"create staged file");
    size_t offset=0;while(offset<bytes.size()){DWORD n=0;const DWORD count=DWORD(std::min<size_t>(bytes.size()-offset,1024*1024));Check(WriteFile(f.value,bytes.data()+offset,count,&n,nullptr)!=FALSE&&n==count,"write staged file");offset+=n;}
    Check(FlushFileBuffers(f.value)!=FALSE,"flush staged file");
}
void CopyDurable(const fs::path& from,const fs::path& to){
    Check(CopyFileW(from.c_str(),to.c_str(),TRUE)!=FALSE,"copy project file");Handle f;
    f.value=CreateFileW(to.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);Check(f.value!=INVALID_HANDLE_VALUE,"open copied file");Check(FlushFileBuffers(f.value)!=FALSE,"flush copied file");
}
void Move(const fs::path& from,const fs::path& to,bool replace=true){Check(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=FALSE,"publish project file");}
struct Paths {
    fs::path main,side,journal,newMain,newSide,oldMain,oldSide,tempJournal;
    Paths(const fs::path& target,const std::string& id):main(target),side(PluginSidecar(target)),journal(target.native()+L".mms-save.json"){
        if(!Hex(id,32))throw std::invalid_argument("save ID");
        auto base=target.native()+L".mms-"+Wide(id);
        newMain=base+L".new-pmm";newSide=base+L".new-json";oldMain=base+L".old-pmm";oldSide=base+L".old-json";tempJournal=base+L".journal";
    }
};
bool Matches(const fs::path& p,const std::string& hash){return !hash.empty()&&fs::exists(p)&&FileHash(p)==hash;}
void Cleanup(const Paths& p){
    // A remaining journal is harmless and recoverable. Remove it first only
    // once both destination files are verified; stale backups never drive load.
    Check(DeleteFileW(p.journal.c_str())!=FALSE,"complete recovery record");
    for(const auto* path:{&p.newMain,&p.newSide,&p.oldMain,&p.oldSide,&p.tempJournal})DeleteFileW(path->c_str());
}
void Recover(const fs::path& target){
    const auto journal=fs::path(target.native()+L".mms-save.json");if(!fs::exists(journal))return;
    const auto j=Json::Parse(ReadFile(journal));if(j.At("schemaVersion").UInt()!=1)throw std::invalid_argument("recovery schema");
    const Paths p(target,j.At("saveId").String());
    const auto nm=j.At("newMain").String(),ns=j.At("newSide").String(),om=j.At("oldMain").String(),os=j.At("oldSide").String();
    if(!Hex(nm,64)||!Hex(ns,64)||(!om.empty()&&!Hex(om,64))||(!os.empty()&&!Hex(os,64)))throw std::invalid_argument("recovery hash");
    // Do not overwrite an unrelated file someone replaced after interruption.
    const auto known=[&](const fs::path& f,const std::string& old,const std::string& fresh){return !fs::exists(f)||Matches(f,old)||Matches(f,fresh);};
    if(!known(p.main,om,nm)||!known(p.side,os,ns))throw std::runtime_error("project changed outside interrupted save");
    const bool forward=(Matches(p.main,nm)||Matches(p.newMain,nm))&&(Matches(p.side,ns)||Matches(p.newSide,ns));
    if(forward){
        if(!Matches(p.main,nm))Move(p.newMain,p.main);
        if(!Matches(p.side,ns))Move(p.newSide,p.side);
        Check(Matches(p.main,nm)&&Matches(p.side,ns),"verify recovered pair");
    }else{
        if((!om.empty()&&!Matches(p.main,om)&&!Matches(p.oldMain,om))||(!os.empty()&&!Matches(p.side,os)&&!Matches(p.oldSide,os)))throw std::runtime_error("neither saved pair is recoverable");
        auto restore=[](const fs::path& staged,const fs::path& dst,const std::string& hash){if(hash.empty()){if(fs::exists(dst))Check(DeleteFileW(dst.c_str())!=FALSE,"remove incomplete new file");}else if(!Matches(dst,hash))Move(staged,dst);};
        restore(p.oldMain,p.main,om);restore(p.oldSide,p.side,os);
        Check((om.empty()?!fs::exists(p.main):Matches(p.main,om))&&(os.empty()?!fs::exists(p.side):Matches(p.side,os)),"verify restored previous pair");
    }
    Cleanup(p);
}
}
std::string StorageId(){std::array<unsigned char,16> bytes{};if(BCryptGenRandom(nullptr,bytes.data(),ULONG(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("storage ID");return HexBytes(bytes.data(),bytes.size());}
std::string FileHash(const fs::path& path){
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("hash file open");Digest digest;std::array<char,65536> bytes{};
    while(f){f.read(bytes.data(),bytes.size());digest.Add(bytes.data(),size_t(f.gcount()));}if(!f.eof())throw std::runtime_error("hash file read");auto result=digest.Finish();return HexBytes(result.data(),result.size());
}
fs::path PluginSidecar(const fs::path& target){return target.native()+L".mms-plugins.json";}
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
void RecoverProjectPair(const fs::path& target){if(!fs::exists(target.native()+L".mms-save.json"))return;Lock lock(target);Recover(target);}
PluginProject ReadPluginProject(const fs::path& target){
    // Opening an intact project must also work from a read-only directory.
    // Hash validation rejects a pair observed during a concurrent replacement.
    RecoverProjectPair(target);if(!fs::exists(PluginSidecar(target))){PluginProject p;p.projectId=StorageId();return p;}
    auto p=DecodeProject(ReadFile(PluginSidecar(target),kMaxSidecar));if(FileHash(target)!=p.mainHash)throw std::runtime_error("PMM and plugin sidecar hashes differ; explicit association required");return p;
}
void PublishProjectPair(const fs::path& target,const fs::path& stagedMain,PluginProject& data,bool createOnly,const std::function<void(unsigned)>& step,bool keepPrevious){
    Lock lock(target);Recover(target);
    if(!SamePath(fs::absolute(target).parent_path(),fs::absolute(stagedMain).parent_path()))throw std::invalid_argument("staged PMM must share directory");
    if(createOnly&&(fs::exists(target)||fs::exists(PluginSidecar(target))))throw std::runtime_error("project already exists");
    // Reject mismatched existing sidecars even when overwriting a different project.
    if(fs::exists(PluginSidecar(target))){auto previous=DecodeProject(ReadFile(PluginSidecar(target),kMaxSidecar));if(!Matches(target,previous.mainHash))throw std::runtime_error("existing plugin sidecar does not match PMM");}
    if(data.projectId.empty())data.projectId=StorageId();data.saveId=StorageId();data.mainHash=FileHash(stagedMain);
    const Paths p(target,data.saveId);bool journal=false,firstMoveAttempted=false,destinationChanged=false;
    pmm_versions::Retention retention;
    std::string oldMain,oldSide;
    auto checkpoint=[&](unsigned n){if(step)step(n);};
    try{
        CopyDurable(stagedMain,p.newMain);WriteDurable(p.newSide,EncodeProject(data));
        oldMain=fs::exists(target)?FileHash(target):std::string{};oldSide=fs::exists(p.side)?FileHash(p.side):std::string{};
        if(!oldMain.empty())CopyDurable(target,p.oldMain);if(!oldSide.empty())CopyDurable(p.side,p.oldSide);
        Check((oldMain.empty()||Matches(p.oldMain,oldMain))&&(oldSide.empty()||Matches(p.oldSide,oldSide)),"verify previous pair backup");
        checkpoint(0);
        const auto newSideHash=FileHash(p.newSide);
        Check(Matches(p.newMain,data.mainHash),"verify staged PMM");
        const auto record="{\"schemaVersion\":1,\"saveId\":\""+data.saveId+"\",\"newMain\":\""+data.mainHash+"\",\"newSide\":\""+newSideHash+"\",\"oldMain\":\""+oldMain+"\",\"oldSide\":\""+oldSide+"\"}";
        if(keepPrevious&&!createOnly)Check(retention.Prepare(target,true),"retain previous project version");
        WriteDurable(p.tempJournal,record);Move(p.tempJournal,p.journal,false);journal=true;checkpoint(1);
        firstMoveAttempted=true;
        Move(p.newMain,p.main,!createOnly);destinationChanged=true;checkpoint(2);Move(p.newSide,p.side,!createOnly);checkpoint(3);
        Check(Matches(p.main,data.mainHash)&&Matches(p.side,newSideHash),"verify published pair");
        retention.Commit();
        Cleanup(p);checkpoint(4);
    }catch(...){
        // A failed first Move can leave the complete old pair untouched. Abort
        // that live attempt rather than silently publishing its rejected new
        // pair on a later read. Never infer this from a checkpoint interruption
        // or after any successful destination move: those retain crash recovery.
        if(journal&&firstMoveAttempted&&!destinationChanged){
            try{
                const auto intact=[](const fs::path& f,const std::string& hash){return hash.empty()?!fs::exists(f):Matches(f,hash);};
                if(intact(p.main,oldMain)&&intact(p.side,oldSide)){Cleanup(p);journal=false;}
            }catch(...){/* Keep recovery evidence when verification/cleanup is uncertain. */}
        }
        // Before journal publication no destination changed. Otherwise retain
        // recovery artifacts unless the verified, untouched attempt was aborted.
        if(!journal)for(const auto* f:{&p.newMain,&p.newSide,&p.oldMain,&p.oldSide,&p.tempJournal})DeleteFileW(f->c_str());
        else retention.Commit(); // The recovery journal may still publish the new pair.
        throw;
    }
}
}
