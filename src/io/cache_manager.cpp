#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/cache_storage_policy.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace mikumikustudio::cache_manager {
namespace {
namespace fs=std::filesystem;
constexpr std::uint64_t MiB=1024ull*1024;
constexpr std::uint32_t indexMagic=0x434d534d,indexVersion=1;
std::recursive_mutex stateMutex;
std::uint64_t Now() {FILETIME t{};GetSystemTimeAsFileTime(&t);return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
std::wstring Lower(std::wstring s) {
    if(s.empty())return s;
    std::wstring lower(s.size(),L'\0');
    if(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,s.data(),int(s.size()),lower.data(),int(lower.size()),nullptr,nullptr,0)>0)return lower;
    for(auto& c:s)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';return s;
}
fs::path Full(const fs::path& p) {
    if(p.empty())return {};
    std::error_code e;auto out=fs::absolute(p,e).lexically_normal();
    while(!out.empty() && out!=out.root_path() && !out.has_filename())out=out.parent_path();
    return e?fs::path{}:out;
}
std::wstring Identity(const fs::path& p) {return Lower(Full(p).wstring());}
fs::path RootPath() {
    const auto capacity=GetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",nullptr,0);
    if(capacity && capacity<=32768) {
        std::vector<wchar_t> value(capacity);
        const auto n=GetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",value.data(),capacity);
        if(n && n<capacity)return Full(fs::path(value.data()));
    }
    return Full(local_data::ExecutableDirectory()/L"Cache");
}
bool SafeParents(const fs::path& p) {
    for(auto item=p;!item.empty();) {
        const auto a=GetFileAttributesW(item.c_str());
        if(a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        const auto parent=item.parent_path();if(parent==item)break;item=parent;
    }
    return true;
}
std::wstring Relative(const fs::path& root,const fs::path& candidate) {
    const auto full=Full(candidate);
    if(full.empty() || !SafeParents(full))return {};
    const auto base=Identity(root)+L"\\",name=Identity(full);
    if(name.compare(0,base.size(),base))return {};
    const fs::path rel=name.substr(base.size());
    const auto parts=std::vector<fs::path>(rel.begin(),rel.end());
    if(parts.size()!=3 || parts[1]==L"." || parts[1]==L"..")return {};
    const bool effect=parts[0]==L"mme",texture=parts[0]==L"textures";
    if((!effect && !texture) || parts[2].extension()!=(effect?L".fxb":L".mtc"))return {};
    const auto stem=parts[2].stem().wstring();
    if(stem.size()!=64 || !std::all_of(stem.begin(),stem.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))return {};
    return rel.wstring();
}
struct FileInfo {std::uint64_t bytes=0,stamp=0;bool exists=false;};
FileInfo Info(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a) ||
       (a.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return {};
    return {(std::uint64_t(a.nFileSizeHigh)<<32)|a.nFileSizeLow,
        (std::uint64_t(a.ftLastWriteTime.dwHighDateTime)<<32)|a.ftLastWriteTime.dwLowDateTime,true};
}
bool Recognized(const fs::path& p) {
    const auto f=Info(p);if(!f.exists || f.bytes<76 || f.bytes>256*MiB)return false;
    std::ifstream stream(p,std::ios::binary);std::uint32_t header[4]{};
    if(!stream.read(reinterpret_cast<char*>(header),sizeof(header)))return false;
    const bool fx=p.extension()==L".fxb";
    return fx ? header[0]==0x43454d4d && header[1]==3 && header[2] && header[2]<=16*MiB &&
        header[3] && header[3]<=16*MiB && std::uint64_t(header[2])+header[3]+80==f.bytes :
        header[0]==0x4354504d && header[1]==2 && std::uint64_t(header[2])+76==f.bytes;
}
bool Deletable(const fs::path& p) {
    if(!SafeParents(p))return false;
    HANDLE h=CreateFileW(p.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;CloseHandle(h);return true;
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
    Handle(Handle&& b) noexcept:value(std::exchange(b.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& b) noexcept {if(this!=&b){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);value=std::exchange(b.value,INVALID_HANDLE_VALUE);}return *this;}
};
struct Entry {std::wstring relative;std::uint64_t bytes=0,lastUse=0;bool invalid=false;std::set<std::wstring> owners;};
struct ProjectRecord {std::wstring path;std::uint64_t opened=0;bool pinned=false;};
template<class T> bool Read(std::istream& in,T& value) {return bool(in.read(reinterpret_cast<char*>(&value),sizeof(value)));}
template<class T> void Write(std::ostream& out,const T& value) {out.write(reinterpret_cast<const char*>(&value),sizeof(value));}
bool ReadText(std::istream& in,std::wstring& s) {
    std::uint32_t count=0;if(!Read(in,count)||count>32768)return false;
    s.resize(count);return bool(in.read(reinterpret_cast<char*>(s.data()),count*sizeof(wchar_t))) && s.find(L'\0')==s.npos;
}
void WriteText(std::ostream& out,const std::wstring& s) {Write(out,std::uint32_t(s.size()));out.write(reinterpret_cast<const char*>(s.data()),s.size()*sizeof(wchar_t));}
struct State {
    fs::path root,index,settingsFile;
    Settings settings;
    std::map<std::wstring,Entry> entries;
    std::map<std::wstring,ProjectRecord> projects;
    std::map<std::wstring,Handle> leases;
    std::set<std::wstring> seen;
    std::wstring current;
    std::uint64_t indexStamp=0,lastFlush=0;
    bool pending=false,healthy=true,dirty=false,initialized=false;
    std::wstring error;
    explicit State(fs::path path):root(std::move(path)),index(root/L"Management"/L"index.bin"),
        settingsFile(root.parent_path()/L"data"/L"cache.ini"){}
    void LoadSettings() {
        settings.enabled=GetPrivateProfileIntW(L"cache",L"enabled",1,settingsFile.c_str())!=0;
        settings.automaticCleanup=GetPrivateProfileIntW(L"cache",L"automaticCleanup",1,settingsFile.c_str())!=0;
        const auto limit=GetPrivateProfileIntW(L"cache",L"limitMiB",4096,settingsFile.c_str());
        settings.limitBytes=std::clamp<std::uint64_t>(limit,64,102400)*MiB;
        settings.recentProjects=std::clamp(GetPrivateProfileIntW(L"cache",L"recentProjects",10,settingsFile.c_str()),1u,100u);
    }
    bool Save() {
        if(!healthy)return false;
        std::error_code e;fs::create_directories(index.parent_path(),e);
        if(e || !SafeParents(index))return false;
        const fs::path temp=index.wstring()+L".tmp-"+std::to_wstring(GetCurrentProcessId());
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);
         Write(out,indexMagic);Write(out,indexVersion);Write(out,std::uint32_t(projects.size()));
         for(const auto& p:projects){WriteText(out,p.second.path);Write(out,p.second.opened);Write(out,std::uint8_t(p.second.pinned));}
         Write(out,std::uint32_t(entries.size()));
         for(const auto& p:entries){const auto& v=p.second;WriteText(out,v.relative);Write(out,v.bytes);Write(out,v.lastUse);
             Write(out,std::uint8_t(v.invalid));Write(out,std::uint32_t(v.owners.size()));for(const auto& owner:v.owners)WriteText(out,owner);}
         out.flush();if(!out){DeleteFileW(temp.c_str());return false;}}
        if(!MoveFileExW(temp.c_str(),index.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());return false;}
        indexStamp=Info(index).stamp;lastFlush=GetTickCount64();dirty=false;return true;
    }
    void Load() {
        const auto f=Info(index);
        if(!f.exists){if(!initialized){entries.clear();projects.clear();}return;}
        if(initialized && f.stamp==indexStamp)return;
        if(!SafeParents(index)){healthy=false;error=L"index-unreadable";return;}
        std::ifstream in(index,std::ios::binary);
        std::uint32_t magic=0,version=0,count=0;
        std::map<std::wstring,Entry> loadedEntries;
        std::map<std::wstring,ProjectRecord> loadedProjects;
        bool ok=f.bytes<=64*MiB && Read(in,magic)&&magic==indexMagic&&Read(in,version)&&version==indexVersion&&Read(in,count)&&count<=10000;
        for(unsigned n=0;ok && n<count;++n){ProjectRecord p;std::uint8_t pin=0;
            ok=ReadText(in,p.path)&&Read(in,p.opened)&&Read(in,pin)&&pin<=1;p.pinned=pin!=0;
            if(ok)loadedProjects.emplace(Identity(p.path),std::move(p));}
        ok=ok&&Read(in,count)&&count<=100000;
        for(unsigned n=0;ok && n<count;++n){Entry p;std::uint8_t invalid=0;std::uint32_t owners=0;
            ok=ReadText(in,p.relative)&&Relative(root,root/p.relative)==p.relative&&Read(in,p.bytes)&&p.bytes<=256*MiB&&Read(in,p.lastUse)&&Read(in,invalid)&&invalid<=1&&Read(in,owners)&&owners<=10000;
            p.invalid=invalid!=0;
            for(unsigned j=0;ok&&j<owners;++j){std::wstring owner;ok=ReadText(in,owner);if(ok && loadedProjects.count(owner))p.owners.insert(std::move(owner));else ok=false;}
            if(ok)loadedEntries.emplace(p.relative,std::move(p));}
        ok=ok && in.peek()==EOF;
        if(!ok){healthy=false;error=L"index-unreadable";return;}
        entries=std::move(loadedEntries);projects=std::move(loadedProjects);indexStamp=f.stamp;healthy=true;error.clear();
    }
    void Discover() {
        for(const wchar_t* category:{L"MME",L"Textures"}) {
            std::error_code e;const auto base=root/category;
            if(!SafeParents(base))continue;
            fs::recursive_directory_iterator it(base,fs::directory_options::skip_permission_denied,e),end;
            unsigned visited=0;
            for(;!e&&it!=end&&++visited<200000;it.increment(e)) {
                if(it->is_directory(e)) {if(it.depth()>=1 || !SafeParents(it->path()))it.disable_recursion_pending();continue;}
                const auto rel=Relative(root,it->path());
                if(rel.empty() || entries.count(rel) || !Recognized(it->path()))continue;
                const auto f=Info(it->path());entries.emplace(rel,Entry{rel,f.bytes,f.stamp,false,{}});dirty=true;
            }
        }
    }
    void Sync() {
        LoadSettings();Load();
        if(!initialized){Reconcile();initialized=true;if(dirty&&healthy)Save();}
    }
    void Use(const fs::path& path) {
        const auto rel=Relative(root,path);const auto f=Info(path);
        if(rel.empty() || !f.exists)return;
        auto found=entries.find(rel);
        if(found==entries.end() && !Recognized(path))return;
        auto& entry=entries[rel];const bool created=entry.relative.empty();
        entry.relative=rel;entry.bytes=f.bytes;entry.lastUse=Now();entry.invalid=false;
        bool association=false;
        if(pending)seen.insert(rel);
        else if(!current.empty())association=entry.owners.insert(current).second;
        else seen.insert(rel);
        if(!leases.count(rel)) {
            HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(h!=INVALID_HANDLE_VALUE)leases.emplace(rel,Handle(h));
        }
        dirty=true;
        if(created || association || GetTickCount64()-lastFlush>=5000)Save();
    }
    std::set<std::wstring> Recent() const {
        std::vector<std::pair<std::uint64_t,std::wstring>> order;
        for(const auto& p:projects)order.push_back({p.second.opened,p.first});
        std::sort(order.rbegin(),order.rend());std::set<std::wstring> result;
        for(std::size_t i=0;i<std::min<std::size_t>(settings.recentProjects,order.size());++i)result.insert(order[i].second);
        return result;
    }
    bool Pinned(const Entry& e) const {for(const auto& owner:e.owners){const auto p=projects.find(owner);if(p!=projects.end()&&p->second.pinned)return true;}return false;}
    bool RecentEntry(const Entry& e,const std::set<std::wstring>& recent) const {for(const auto& owner:e.owners)if(recent.count(owner))return true;return false;}
    std::uint64_t Total(const std::wstring& except={}) const {std::uint64_t bytes=0;for(const auto& p:entries)if(p.first!=except)bytes+=p.second.bytes;return bytes;}
    void Reconcile() {
        Discover();
        for(auto it=entries.begin();it!=entries.end();) {
            const auto f=Info(root/it->first);
            if(!f.exists){it=entries.erase(it);dirty=true;}else{it->second.bytes=f.bytes;++it;}
        }
    }
    CleanupResult Remove(Cleanup mode,const std::wstring& project,std::uint64_t target,bool automatic=false,const std::wstring& except={}) {
        CleanupResult result;
        if(!healthy && mode!=Cleanup::All){result.error=error;return result;}
        const auto recent=Recent();std::vector<std::wstring> candidates;
        for(const auto& p:entries) {
            const auto& entry=p.second;
            if(p.first==except)continue;
            if(mode==Cleanup::Project && (!entry.owners.count(project) || entry.owners.size()!=1))continue;
            if(mode==Cleanup::Reclaimable && (Pinned(entry) || (!automatic && !entry.invalid && RecentEntry(entry,recent))))continue;
            candidates.push_back(p.first);
        }
        std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) {
            const auto& x=entries.at(a);const auto& y=entries.at(b);
            const int px=x.invalid?0:RecentEntry(x,recent)?2:1,py=y.invalid?0:RecentEntry(y,recent)?2:1;
            return px!=py?px<py:x.lastUse!=y.lastUse?x.lastUse<y.lastUse:a<b;
        });
        auto total=Total(except);
        for(const auto& name:candidates) {
            if(automatic && total<=target)break;
            const auto& entry=entries.at(name);const auto path=root/name;
            if(leases.count(name) || !Deletable(path) || !DeleteFileW(path.c_str())){++result.skipped;continue;}
            result.bytes+=entry.bytes;++result.files;total-=entry.bytes;entries.erase(name);dirty=true;
        }
        if(mode==Cleanup::All && !healthy) {healthy=true;error.clear();}
        if(dirty&&!Save())result.error=L"index-unwritable";
        return result;
    }
};
std::unique_ptr<State> state;
struct Guard {
    Handle cross;
    bool acquired=false;
    std::unique_lock<std::recursive_mutex> local;
    State* s=nullptr;
    Guard():local(stateMutex,std::defer_lock) {
        const auto root=RootPath();if(root.empty() || !SafeParents(root))return;
        std::uint64_t hash=1469598103934665603ull;for(auto c:Identity(root)){hash^=c;hash*=1099511628211ull;}
        const std::wstring name=L"Local\\MMS-cache-"+std::to_wstring(hash);
        cross=Handle(CreateMutexW(nullptr,FALSE,name.c_str()));if(!cross.value || cross.value==INVALID_HANDLE_VALUE)return;
        const DWORD wait=WaitForSingleObject(cross.value,2000);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)return;
        acquired=true;local.lock();
        if(!state || Identity(state->root)!=Identity(root))state=std::make_unique<State>(root);
        s=state.get();s->Sync();
    }
    ~Guard(){if(acquired)ReleaseMutex(cross.value);}
    explicit operator bool() const {return s!=nullptr;}
};
}
fs::path Root() noexcept {try{return RootPath();}catch(...){return {};}}
bool Managed(const fs::path& p) noexcept {try{return !Relative(RootPath(),p).empty();}catch(...){return false;}}
Settings Current() noexcept {try{State settings(RootPath());settings.LoadSettings();return settings.settings;}catch(...){}return {};}
bool Enabled() noexcept {return Current().enabled;}
bool Configure(const Settings& value,std::wstring& error) noexcept {
    try {
        if(value.limitBytes<64*MiB || value.limitBytes>102400*MiB || value.recentProjects<1 || value.recentProjects>100){error=L"invalid-settings";return false;}
        Guard g;if(!g){error=L"cache-busy";return false;}
        auto& s=*g.s;std::error_code e;fs::create_directories(s.settingsFile.parent_path(),e);
        if(e || !SafeParents(s.settingsFile)){error=L"settings-unwritable";return false;}
        const auto temp=s.settingsFile.wstring()+L".tmp-"+std::to_wstring(GetCurrentProcessId());
        const auto put=[&](const wchar_t* key,const std::wstring& text){return WritePrivateProfileStringW(L"cache",key,text.c_str(),temp.c_str())!=0;};
        bool ok=put(L"enabled",value.enabled?L"1":L"0") && put(L"automaticCleanup",value.automaticCleanup?L"1":L"0") &&
            put(L"limitMiB",std::to_wstring(value.limitBytes/MiB)) && put(L"recentProjects",std::to_wstring(value.recentProjects));
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,temp.c_str());
        ok=ok && MoveFileExW(temp.c_str(),s.settingsFile.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        if(!ok){DeleteFileW(temp.c_str());error=L"settings-unwritable";return false;}
        s.settings=value;
        if(value.enabled && value.automaticCleanup && s.Total()>value.limitBytes)
            s.Remove(Cleanup::Reclaimable,{},value.limitBytes*8/10,true);
        error.clear();return true;
    }catch(...){error=L"cache-unavailable";return false;}
}
void Touch(const fs::path& path) noexcept {try{if(cache_storage_policy::MemoryOnly() || !Managed(path))return;Guard g;if(g)g.s->Use(path);}catch(...){} }
void Invalidate(const fs::path& path) noexcept {try{if(cache_storage_policy::MemoryOnly() || !Managed(path))return;Guard g;if(g){const auto id=Relative(g.s->root,path);auto it=g.s->entries.find(id);if(it!=g.s->entries.end()){it->second.invalid=true;g.s->dirty=true;g.s->Save();}}}catch(...){} }
void BeginProject(const wchar_t* path) noexcept {try{if(cache_storage_policy::MemoryOnly())return;Guard g;if(!g)return;auto& s=*g.s;if(s.dirty)s.Save();s.leases.clear();s.current=Identity(path?path:L"");s.seen.clear();s.pending=true;}catch(...){} }
void ProjectOpened(const wchar_t* path) noexcept {
    try{if(cache_storage_policy::MemoryOnly() || !path||!*path)return;Guard g;if(!g)return;auto& s=*g.s;const auto id=Identity(path);
        auto& p=s.projects[id];p.path=Full(path).wstring();p.opened=Now();s.current=id;s.pending=false;
        for(const auto& lease:s.leases){auto it=s.entries.find(lease.first);if(it!=s.entries.end())it->second.owners.insert(id);}
        for(const auto& name:s.seen){auto it=s.entries.find(name);if(it!=s.entries.end())it->second.owners.insert(id);}
        s.seen.clear();s.dirty=true;s.Save();
    }catch(...){}
}
void EndProjectLoad() noexcept {try{if(cache_storage_policy::MemoryOnly())return;Guard g;if(g && g.s->pending){g.s->pending=false;g.s->current.clear();g.s->seen.clear();g.s->leases.clear();}}catch(...){} }
void ResetScene() noexcept {try{if(cache_storage_policy::MemoryOnly())return;Guard g;if(g && !g.s->pending){g.s->current.clear();g.s->seen.clear();g.s->leases.clear();if(g.s->dirty)g.s->Save();}}catch(...){} }
void Shutdown() noexcept {EndProjectLoad();ResetScene();}
bool Pin(const std::wstring& path,bool pin) noexcept {try{Guard g;if(!g)return false;auto it=g.s->projects.find(Identity(path));if(it==g.s->projects.end())return false;it->second.pinned=pin;g.s->dirty=true;return g.s->Save();}catch(...){return false;}}
Snapshot Inspect() noexcept {
    Snapshot out;
    try{Guard g;if(!g){out.error=L"cache-busy";return out;}auto& s=*g.s;s.Reconcile();out.settings=s.settings;out.error=s.error;
        const auto recent=s.Recent();
        std::map<std::wstring,std::size_t> projectRows;
        for(const auto& p:s.projects){projectRows[p.first]=out.projects.size();out.projects.push_back({p.second.path,p.second.opened,0,0,0,p.second.pinned,p.first==s.current});}
        for(const auto& p:s.entries){const auto& v=p.second;const bool available=!s.leases.count(p.first)&&Deletable(s.root/p.first);
            out.totalBytes+=v.bytes;if(p.first.rfind(L"mme\\",0)==0)out.effectBytes+=v.bytes;else out.textureBytes+=v.bytes;
            if(available && !s.Pinned(v) && (v.invalid || !s.RecentEntry(v,recent)))out.reclaimableBytes+=v.bytes;
            for(const auto& owner:v.owners){auto row=projectRows.find(owner);if(row==projectRows.end())continue;
                auto& project=out.projects[row->second];project.totalBytes+=v.bytes;
                if(v.owners.size()>1)project.sharedBytes+=v.bytes;else if(available)project.reclaimableBytes+=v.bytes;}
        }
        std::sort(out.projects.begin(),out.projects.end(),[](const Project&a,const Project&b){return a.lastOpened>b.lastOpened;});
        if(s.dirty&&s.healthy)s.Save();
    }catch(...){out.error=L"cache-unavailable";}
    return out;
}
CleanupResult Clean(Cleanup mode,const std::wstring& project) noexcept {
    try{Guard g;if(!g)return {0,0,0,L"cache-busy"};g.s->Reconcile();return g.s->Remove(mode,Identity(project),0);}catch(...){return {0,0,0,L"cache-unavailable"};}
}
struct WriteReservation::Impl {
    std::unique_ptr<Guard> guard;
    fs::path path;
    bool allowed=false,managed=false;
};
WriteReservation::WriteReservation(const fs::path& path,std::uint64_t bytes) noexcept {
    try{impl_=std::make_unique<Impl>();impl_->path=path;impl_->managed=Managed(path);
        if(!impl_->managed){impl_->allowed=true;return;}
        if(cache_storage_policy::MemoryOnly())return;
        impl_->guard=std::make_unique<Guard>();auto& g=*impl_->guard;if(!g)return;
        auto& s=*g.s;if(!s.settings.enabled || !s.healthy || bytes>s.settings.limitBytes)return;
        const auto rel=Relative(s.root,path);
        if(s.Total(rel)>s.settings.limitBytes-bytes)s.Reconcile();
        if(s.Total(rel)>s.settings.limitBytes-bytes && s.settings.automaticCleanup)
            s.Remove(Cleanup::Reclaimable,{},std::min(s.settings.limitBytes*8/10,s.settings.limitBytes-bytes),true,rel);
        impl_->allowed=s.Total(rel)<=s.settings.limitBytes-bytes;
    }catch(...){impl_.reset();}
}
WriteReservation::~WriteReservation()=default;
WriteReservation::WriteReservation(WriteReservation&&) noexcept=default;
WriteReservation& WriteReservation::operator=(WriteReservation&&) noexcept=default;
WriteReservation::operator bool() const noexcept {return impl_ && impl_->allowed;}
void WriteReservation::Commit() noexcept {try{if(*this && impl_->managed && impl_->guard && *impl_->guard){auto& s=*impl_->guard->s;s.Use(impl_->path);s.Save();}}catch(...){} }
}
