#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/cache_files.hpp"
#include "cache_source_budget.hpp"
#include "cache_source_admission.hpp"
#include "cache_catalog_intent.hpp"
#include "cache_files_crypto.hpp"
#include "mikumikustudio/cache_policy.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/cache_storage_policy.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <map>
#include <limits>
#include <list>
#include <iterator>
#include <string_view>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace mikumikustudio::cache_manager {
namespace {
namespace fs=std::filesystem;
constexpr std::uint64_t MiB=1024ull*1024;
constexpr std::uint32_t indexMagic=0x434d534d,indexVersion=1;
std::recursive_mutex stateMutex;
// Process-local scene identity, protected by stateMutex even when disk admission fails.
enum class ScenePhase { Legacy, Loading, Resetting, Active, Isolated };
struct OwnerProject {
    std::wstring path, id;
    std::uint64_t opened;
    std::size_t textBytes() const noexcept { return (path.capacity() + id.capacity() + 2) * sizeof(wchar_t); }
};
struct SceneContext {
    std::uint64_t serial = 1;
    ScenePhase phase = ScenePhase::Legacy;
    bool incompleteUse = false;
    std::shared_ptr<const OwnerProject> project;
    fs::path bindingRoot;

    bool acceptsUse() const noexcept {
        return phase == ScenePhase::Legacy || phase == ScenePhase::Loading || phase == ScenePhase::Active;
    }
} scene;
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
    if constexpr (build_profile::original) return {};
    const auto capacity=GetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",nullptr,0);
    if(capacity && capacity<=32768) {
        std::vector<wchar_t> value(capacity);
        const auto n=GetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",value.data(),capacity);
        if(n && n<capacity)return Full(fs::path(value.data()));
    }
    return Full(cache_policy::Root());
}
fs::path SettingsPath() {
    const auto capacity=GetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",nullptr,0);
    if(capacity && capacity<=32768) {
        std::vector<wchar_t> value(capacity);
        const auto n=GetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",value.data(),capacity);
        if(n && n<capacity)return Full(fs::path(value.data()));
    }
    return system_settings::Path();
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
struct FileInfo {std::uint64_t bytes=0,stamp=0;bool exists=false;DWORD error=ERROR_SUCCESS;};
FileInfo Info(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a))return {0,0,false,GetLastError()};
    if(a.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))return {0,0,false,ERROR_INVALID_DATA};
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
using PhysicalId=std::pair<std::uint32_t,std::uint64_t>;
PhysicalId Physical(const cache_files::FileObservation& value) noexcept {return {value.identity.volume,value.identity.fileIndex};}
std::uint64_t Charge(const cache_files::FileObservation& value) noexcept {
    return (std::max)(value.streamLogicalBytes,value.streamAllocationBytes);
}
bool SameObservation(const cache_files::FileObservation&a,const cache_files::FileObservation&b) noexcept {
    return a.identity.fileBytes==b.identity.fileBytes&&a.allocationBytes==b.allocationBytes&&
        a.streamLogicalBytes==b.streamLogicalBytes&&a.streamAllocationBytes==b.streamAllocationBytes&&
        a.attributes==b.attributes&&a.links==b.links&&a.streamCount==b.streamCount&&a.kind==b.kind&&
        a.deletePending==b.deletePending&&a.namedStreamsSupported==b.namedStreamsSupported;
}
struct SourceScope;
struct InstanceStamp {
    PhysicalId id;
    std::uint64_t created = 0;
    USN usn = 0;
    bool operator==(const InstanceStamp& other) const noexcept {
        return id == other.id && created == other.created && usn == other.usn;
    }
};
struct OwnerProof {
    std::wstring relative;
    InstanceStamp root, file;
    DWORDLONG journal = 0;
    // Borrow only a genuine live scene lease / source exclusion. Retirement
    // must close the last payload handle even while index settlement is owed.
    std::weak_ptr<Handle> lease;
    std::weak_ptr<SourceScope> scope;
    // Coalesce indistinguishable samples for the SAME owner only; this
    // comparison by itself is never authorization to publish an owner.
    bool sameSample(const OwnerProof& other) const noexcept {
        return relative == other.relative && root == other.root && file == other.file && journal == other.journal;
    }
};
constexpr std::size_t ownerLimit = 4096, ownerMemoryLimit = 64 * MiB;
std::size_t ownerCount = 0, ownerBytes = 0; // stateMutex; includes tentative uses on all roots
// One proof can be both a live scene use and the evidence for its first
// unsettled owner. Reserve space for both list links in the base charge.
// Additional owners of that proof each consume another slot and node charge.
struct RetainedProof final : OwnerProof {
    const std::size_t charge;
    mutable std::size_t debts = 0;
    explicit RetainedProof(OwnerProof sample)
        : OwnerProof(std::move(sample)), charge(512 + (relative.capacity()+1)*sizeof(wchar_t)) {
        if (ownerCount >= ownerLimit || charge > ownerMemoryLimit - ownerBytes) throw std::bad_alloc();
        ++ownerCount; ownerBytes += charge;
    }
    ~RetainedProof() { --ownerCount; ownerBytes -= charge; }
    RetainedProof(const RetainedProof&) = delete;
    RetainedProof& operator=(const RetainedProof&) = delete;
    void Attach() const {
        if (debts) {
            if (ownerCount >= ownerLimit || 512 > ownerMemoryLimit - ownerBytes) throw std::bad_alloc();
            ++ownerCount; ownerBytes += 512;
        }
        ++debts;
    }
    void Detach() const noexcept {
        if (--debts) { --ownerCount; ownerBytes -= 512; }
    }
};
using SceneUse = std::shared_ptr<const RetainedProof>;
struct OwnerDelta {
    SceneUse proof;
    std::shared_ptr<const OwnerProject> project;
    bool inserted = false, publishedUnverified = false, uncertain = false;
    OwnerDelta(SceneUse use, std::shared_ptr<const OwnerProject> owner)
        : proof(std::move(use)), project(std::move(owner)) {
        const auto bytes = project->textBytes();
        if (bytes > ownerMemoryLimit - ownerBytes) throw std::bad_alloc();
        ownerBytes += bytes;
        try { proof->Attach(); } catch (...) { ownerBytes -= bytes; throw; }
    }
    ~OwnerDelta() { proof->Detach(); ownerBytes -= project->textBytes(); }
    OwnerDelta(const OwnerDelta&) = delete;
    OwnerDelta& operator=(const OwnerDelta&) = delete;
};
bool ReadStamp(HANDLE handle, const PhysicalId& id, bool directory, InstanceStamp& stamp) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
        (!directory && info.nNumberOfLinks != 1) || info.dwVolumeSerialNumber != id.first ||
        ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != id.second) return false;
    stamp.id = id;
    stamp.created = (std::uint64_t(info.ftCreationTime.dwHighDateTime) << 32) | info.ftCreationTime.dwLowDateTime;
    alignas(USN_RECORD_V2) unsigned char buffer[1024]{};
    DWORD bytes = 0;
    USN_RECORD_V2 rec{};
    const auto prefix = offsetof(USN_RECORD_V2, FileName);
    if (DeviceIoControl(handle, FSCTL_READ_FILE_USN_DATA, nullptr, 0, buffer, sizeof(buffer), &bytes, nullptr) &&
        bytes >= prefix) {
        std::memcpy(&rec, buffer, prefix);
        if (rec.MajorVersion == 2 && rec.RecordLength >= prefix && rec.RecordLength <= bytes &&
            rec.FileReferenceNumber == id.second && rec.Usn > 0) stamp.usn = rec.Usn;
    }
    return true;
}
bool SampleInstances(const fs::path& root, const PhysicalId& directory, HANDLE payload,
                     const PhysicalId& file, OwnerProof& result) {
    Handle dir(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (dir.value == INVALID_HANDLE_VALUE) return false;
    const auto journal = [&] {
        USN_JOURNAL_DATA_V0 data{};
        DWORD bytes = 0;
        if (!DeviceIoControl(dir.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr) ||
            bytes < sizeof(data)) return USN_JOURNAL_DATA_V0{};
        return data;
    };
    const auto before = journal();
    if (!ReadStamp(dir.value, directory, true, result.root) || !ReadStamp(payload, file, false, result.file)) return false;
    if (before.UsnJournalID && result.root.usn > 0 && result.file.usn > 0 &&
        result.root.usn >= before.LowestValidUsn && result.root.usn < before.NextUsn &&
        result.file.usn >= before.LowestValidUsn && result.file.usn < before.NextUsn &&
        journal().UsnJournalID == before.UsnJournalID) result.journal = before.UsnJournalID;
    return true;
}
bool SameInstance(const OwnerProof& old, const OwnerProof& now) noexcept {
    if (old.root.id != now.root.id || old.file.id != now.file.id ||
        !old.root.created || old.root.created != now.root.created ||
        !old.file.created || old.file.created != now.file.created) return false;
    const bool journal = old.journal && old.journal == now.journal;
    const auto scope = old.scope.lock();
    const bool sameRoot = (scope && scope == now.scope.lock()) || (journal && old.root.usn == now.root.usn);
    // A lease holds this exact file ID live. Checking the freshly opened name
    // against that ID above is sufficient even if the journal is unavailable.
    return sameRoot && (!old.lease.expired() || (journal && old.file.usn == now.file.usn));
}
struct ReservationRecord {
    std::uint64_t bytes=0,budget=0,useSerial=0;
    bool publicationPending=false,orphaned=false;
    std::optional<PhysicalId> temporary;
};
std::uint64_t AddBytes(std::uint64_t a,std::uint64_t b) noexcept {
    const auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    return b>maximum-a?maximum:a+b;
}
Settings UnavailableSettings() noexcept {
    Settings value;value.enabled=false;value.automaticCleanup=false;value.limitBytes=0;value.recentProjects=0;return value;
}
template<class T> bool Read(std::istream& in,T& value) {return bool(in.read(reinterpret_cast<char*>(&value),sizeof(value)));}
template<class T> void Write(std::ostream& out,const T& value) {out.write(reinterpret_cast<const char*>(&value),sizeof(value));}
bool ReadText(std::istream& in,std::wstring& s) {
    std::uint32_t count=0;if(!Read(in,count)||count>32768)return false;
    s.resize(count);return bool(in.read(reinterpret_cast<char*>(s.data()),count*sizeof(wchar_t))) && s.find(L'\0')==s.npos;
}
void WriteText(std::ostream& out,const std::wstring& s) {Write(out,std::uint32_t(s.size()));out.write(reinterpret_cast<const char*>(s.data()),s.size()*sizeof(wchar_t));}
// Holds existing Video control exclusion before the first inventory and until
// the last nested manager operation/writer leaves. This does not grant quota to
// independent SourceJournal writers or initialize a missing Video area.
struct SourceScope {
    cache_files::Directory root;
    cache_business::SourceBudgetSnapshot source;
    cache_files::DirectoryIdentity identity{};
    std::uint64_t allocationUnit=0;
    bool transitionActive=false;
    // Only explicit first publication or a verified first-publication rollback
    // can have a Video namespace without an existing business responsibility.
    bool zeroSourceObligations=false;
    cache_business::SourceBudgetResult status=cache_business::SourceBudgetResult::Io;
    bool Valid() const noexcept {return zeroSourceObligations||status==cache_business::SourceBudgetResult::Ok||status==cache_business::SourceBudgetResult::Absent;}
    // createRoot=false never creates the manager root; the ordinary budget
    // validation still runs and reads the Video children through Child(false).
    // Shutdown uses this mode for its optional final flush.
    explicit SourceScope(const fs::path& path,bool recovery=false,bool createRoot=true) {
        if(cache_files::Directory::Open(path,createRoot&&!recovery,root)!=cache_files::Result::Ok||
           root.GetIdentity(identity)!=cache_files::Result::Ok)return;
        std::vector<wchar_t> mount(32768);DWORD serial=0,flags=0,component=0,sectors=0,sectorBytes=0,freeClusters=0,clusters=0;
        if(!GetVolumePathNameW(path.c_str(),mount.data(),static_cast<DWORD>(mount.size()))||
           !GetVolumeInformationW(mount.data(),nullptr,0,&serial,&component,&flags,nullptr,0)||serial!=identity.volume||
           !GetDiskFreeSpaceW(mount.data(),&sectors,&sectorBytes,&freeClusters,&clusters)||!sectors||!sectorBytes)return;
        allocationUnit=std::uint64_t(sectors)*sectorBytes;
        if(!recovery)status=cache_business::ReadSourceBudgetLocked(root,allocationUnit,source);
    }
};
struct State {
    struct SceneLease {
        std::shared_ptr<Handle> handle;
        std::uint64_t useSerial;
    };
    fs::path root,index;
    Settings settings;
    std::map<std::wstring,Entry> entries;
    std::map<std::wstring,ProjectRecord> projects;
    std::map<std::wstring,SceneLease> leases;
    std::map<std::wstring,ReservationRecord> reservations;
    std::map<PhysicalId,cache_files::FileObservation> physical;
    std::map<std::wstring,PhysicalId> observedNames;
    std::vector<std::wstring> observedPaths; // exact namespace spelling, including foreign names
    std::optional<cache_files::DirectoryIdentity> rootIdentity;
    std::optional<PhysicalId> metadataTemporary;
    std::uint64_t physicalBytes=0,allocationUnit=0,metadataBudget=0;
    std::uint64_t sourceRawResidual=0,sourceMetadataBudget=0,sourceMetadataCredit=0;
    std::set<PhysicalId> sourceDeductions;
    std::weak_ptr<SourceScope> sourceScope;
    bool inventoryReady=false,saving=false;
    bool lastSavePublished=false,forceIndexReload=false;
    std::optional<PhysicalId> expectedIndex;
    FILETIME expectedIndexCreation{};
    std::uint64_t expectedIndexBytes=0;
    // Evidence for explicit repair only; a failed read must not replace it.
    std::optional<PhysicalId> lastVerifiedIndex;
    FILETIME lastVerifiedIndexCreation{};
    // An unresolved published file must not lose its conservative charge when
    // callers switch roots and release their last ordinary guard. Cleared after
    // successful persistence; liveStates keeps a weak lookup for retry.
    std::shared_ptr<State> pendingPersistenceOwner;
    std::list<SceneUse> sceneUses;
    std::list<OwnerDelta> owed;
    bool projectingOwners = false, unconfirmedLegacyOwners = false;
    std::set<std::wstring> seen;
    std::wstring current;
    std::uint64_t indexStamp=0,indexBytes=0,lastFlush=0;
    bool pending=false,healthy=true,dirty=false,initialized=false,settingsReadable=false;
    std::wstring error;
    explicit State(fs::path path):root(std::move(path)),index(root/L"Management"/L"index.bin"){}
    bool RefreshInventory(cache_files::InventorySnapshot* observedOutput=nullptr) {
        inventoryReady=false;
        const auto scope=sourceScope.lock();
        if(!scope||!scope->Valid())return false;
        cache_files::Directory directory;
        if(scope->root.Share(directory)!=cache_files::Result::Ok)return false;
        cache_files::InventoryLimits limits;
        limits.maximumEntries=65536;limits.maximumDirectoryGuards=4096;
        limits.maximumNameCodeUnits=16u*1024*1024;
        cache_files::InventorySnapshot snapshot;
        if(directory.Inventory(limits,snapshot)!=cache_files::Result::Ok)return false;
        const auto identity=snapshot.RootIdentity();
        if(rootIdentity&&(rootIdentity->volume!=identity.volume||rootIdentity->fileIndex!=identity.fileIndex))return false;
        std::map<PhysicalId,cache_files::FileObservation> files;
        std::map<std::wstring,PhysicalId> names;
        std::vector<std::wstring> paths;paths.reserve(snapshot.Entries().size());
        std::uint64_t total=0;
        for(const auto&entry:snapshot.Entries()) {
            const auto id=Physical(entry.observed);
            const auto inserted=files.emplace(id,entry.observed);
            if(!inserted.second&&!SameObservation(inserted.first->second,entry.observed))return false;
            if(inserted.second){const auto bytes=Charge(entry.observed);if(bytes>UINT64_MAX-total)return false;total+=bytes;}
            if(!names.emplace(Lower(entry.relativePath),id).second)return false;
            paths.push_back(entry.relativePath);
        }
        // Previously charged foreign/temporary names carry the same absence
        // obligation. Preserve their exact spelling and use the extended local
        // namespace so trailing dots/spaces/device-like names are not rewritten
        // by DOS path parsing. This grants no ownership or deletion permission.
        const fs::path extendedRoot(L"\\\\?\\"+root.wstring());
        for(const auto&previous:observedPaths)if(!names.count(Lower(previous))){
            const auto status=Info(extendedRoot/previous);
            if(status.exists||(status.error!=ERROR_FILE_NOT_FOUND&&status.error!=ERROR_PATH_NOT_FOUND))return false;
        }
        // Enumeration may omit a delete-pending file while its namespace still
        // returns ACCESS_DENIED. An owned entry is discharged only by explicit
        // FILE/PATH_NOT_FOUND, never by omission from the new inventory alone.
        // Validate before swapping any ledger so all callers (including Save
        // and TrackTemporary) retain the previous observed charge on failure.
        for(const auto&entry:entries)if(!names.count(entry.first)){
            const auto status=Info(root/entry.first);
            if(status.exists||(status.error!=ERROR_FILE_NOT_FOUND&&status.error!=ERROR_PATH_NOT_FOUND))return false;
        }
        std::uint64_t rawResidual=0,sourceMetadata=0;
        std::set<PhysicalId> deductions;
        if(!scope->zeroSourceObligations&&scope->status==cache_business::SourceBudgetResult::Ok){
            cache_business::RawSourceBudgetProjection projection;
            if(cache_business::ProjectRawSourceBudget(scope->source,snapshot,projection)!=cache_business::SourceBudgetResult::Ok)return false;
            rawResidual=projection.residualBytes;sourceMetadata=scope->source.Credit().metadataBytes;
            for(const auto&deduction:projection.deductions){
                const PhysicalId id{deduction.identity.volume,deduction.identity.fileIndex};
                if(!deductions.insert(id).second)return false;
                for(const auto&reservation:reservations)if(reservation.second.temporary&&*reservation.second.temporary==id)return false;
                if(metadataTemporary&&*metadataTemporary==id)return false;
            }
        }else if(!scope->zeroSourceObligations&&names.count(L"video"))return false; // absent area appeared outside the cooperative lock
        if(rawResidual>UINT64_MAX-sourceMetadata||total>UINT64_MAX-rawResidual-sourceMetadata)return false;
        allocationUnit=scope->allocationUnit;
        sourceRawResidual=rawResidual;sourceMetadataBudget=sourceMetadata;sourceDeductions.swap(deductions);
        sourceMetadataCredit=scope->status==cache_business::SourceBudgetResult::Ok?scope->source.Credit().fundedMetadataBytes:0;
        physical.swap(files);observedNames.swap(names);observedPaths.swap(paths);physicalBytes=total;rootIdentity=identity;inventoryReady=true;
        if(observedOutput)*observedOutput=std::move(snapshot);
        return true;
    }
    std::uint64_t Round(std::uint64_t bytes) const noexcept {
        if(!allocationUnit||bytes>UINT64_MAX-(allocationUnit-1))return UINT64_MAX;
        return ((bytes+allocationUnit-1)/allocationUnit)*allocationUnit;
    }
    const cache_files::FileObservation* Observed(const std::wstring& relative) const {
        const auto name=observedNames.find(Lower(relative));if(name==observedNames.end())return nullptr;
        const auto file=physical.find(name->second);return file==physical.end()?nullptr:&file->second;
    }
    static std::uint64_t TextBytes(const std::wstring& value) noexcept {
        return value.size()>(UINT64_MAX-4)/2?UINT64_MAX:4+2*std::uint64_t(value.size());
    }
    bool RecordOwnerUse(const fs::path& path, const std::wstring& rel, bool needsOwner, SceneUse* recorded = nullptr) {
        const auto observed = observedNames.find(rel);
        if (!rootIdentity || observed == observedNames.end()) return false;
        const auto file = physical.find(observed->second);
        if (file == physical.end() || file->second.kind != cache_files::ObservationKind::File ||
            file->second.deletePending || file->second.links != 1) return false;
        Handle pin(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        OwnerProof sample;
        if (!SampleInstances(root, {rootIdentity->volume,rootIdentity->fileIndex}, pin.value, observed->second, sample)) return false;
        sample.scope = sourceScope;
        const auto lease = leases.find(rel);
        if (lease != leases.end()) {
            InstanceStamp held;
            if (ReadStamp(lease->second.handle->value, observed->second, false, held) && held.created == sample.file.created)
                sample.lease = lease->second.handle;
        }
        sample.relative = rel;
        const auto reusable = [&](const OwnerProof& old) { return old.relative == rel && SameInstance(old, sample); };
        auto use = std::find_if(sceneUses.begin(), sceneUses.end(), [&](const auto& old) {
            return reusable(*old) || old->sameSample(sample);
        });
        SceneUse proof;
        if (use != sceneUses.end() && reusable(**use)) proof = *use;
        if (!proof) for (const auto& delta : owed)
            if (reusable(*delta.proof)) { proof = delta.proof; break; }
        if (!proof) proof = std::make_shared<RetainedProof>(std::move(sample));
        // A fresh use can refresh its own witness, including a previously
        // owned entry. It never refreshes the evidence for another project.
        const bool added = use == sceneUses.end();
        if (added) use = sceneUses.insert(use, proof);
        try {
            if (needsOwner) {
                auto old = std::find_if(owed.begin(), owed.end(), [&](const auto& delta) {
                    return delta.project->id == scene.project->id && delta.proof->sameSample(*proof);
                });
                if (old == owed.end()) owed.emplace_back(proof, scene.project);
                else if (old->proof != proof) {
                    proof->Attach();
                    old->proof->Detach();
                    old->proof = proof;
                }
            }
        } catch (...) {
            if (added) sceneUses.erase(use);
            throw;
        }
        *use = std::move(proof);
        if (recorded) *recorded = *use;
        return true;
    }
    std::uint64_t FutureIndexBytes() const noexcept {
        if(projects.size()>10000||entries.size()>100000)return UINT64_MAX;
        std::uint64_t size=16;
        for(const auto&p:projects){if(p.second.path.size()>32768)return UINT64_MAX;size=AddBytes(size,AddBytes(TextBytes(p.second.path),9));}
        for(const auto&p:entries){size=AddBytes(size,AddBytes(TextBytes(p.second.relative),21));
            if(p.second.relative.size()>32768||p.second.owners.size()>10000)return UINT64_MAX;
            for(const auto&owner:p.second.owners)size=AddBytes(size,TextBytes(owner));}
        // Bound the future serialized view, including work waiting on a different scene.
        using ExtraOwners = std::map<std::wstring_view,std::set<std::wstring_view>>;
        std::optional<ExtraOwners> extraOwners;
        if (!projectingOwners && !owed.empty()) try {
            extraOwners.emplace();
            std::set<std::wstring_view> extraProjects;
            for (const auto& delta : owed) {
                const auto& owner = *delta.project;
                const auto& name = delta.proof->relative;
                if (!projects.count(owner.id) && extraProjects.insert(owner.id).second)
                    size = AddBytes(size, AddBytes(TextBytes(owner.path), 9));
                const auto entry = entries.find(name);
                if (entry == entries.end() || !entry->second.owners.count(owner.id)) {
                    auto& additions = (*extraOwners)[name];
                    if (additions.insert(owner.id).second) size = AddBytes(size, TextBytes(owner.id));
                    const auto existing = entry == entries.end() ? 0 : entry->second.owners.size();
                    if (existing + additions.size() > 10000) return UINT64_MAX;
                }
            }
            if (projects.size() + extraProjects.size() > 10000) return UINT64_MAX;
        } catch (...) { return UINT64_MAX; }
        auto count=entries.size();
        for(const auto&p:reservations){const auto existing=entries.find(p.first);
            if(p.first.size()>32768)return UINT64_MAX;
            if(existing==entries.end()){if(++count>100000)return UINT64_MAX;size=AddBytes(size,AddBytes(TextBytes(p.first),21));}
            if (p.second.useSerial != scene.serial || !scene.acceptsUse() || pending || current.empty() ||
                (existing != entries.end() && existing->second.owners.count(current))) continue;
            auto owners = existing == entries.end() ? std::size_t(0) : existing->second.owners.size();
            if (extraOwners) {
                const auto additions = extraOwners->find(p.first);
                if (additions != extraOwners->end()) {
                    if (additions->second.count(current)) continue;
                    owners += additions->second.size();
                }
            }
            if (current.size() > 32768 || owners >= 10000) return UINT64_MAX;
            size = AddBytes(size, TextBytes(current));
        }
        return size>64*MiB?UINT64_MAX:size;
    }
    std::uint64_t Residual(std::uint64_t budget,const std::optional<PhysicalId>& id) const noexcept {
        if(id){const auto found=physical.find(*id);if(found!=physical.end()){const auto actual=Charge(found->second);return budget>actual?budget-actual:0;}}
        return budget;
    }
    void LoadSettings() {
        cache_policy::Snapshot snapshot;
        settingsReadable=cache_policy::Store(root,SettingsPath()).Read(snapshot)==cache_policy::Result::Ok;
        if(!settingsReadable)return;
        const auto& value=snapshot.Value();
        settings.enabled=value.enabled;settings.automaticCleanup=value.automaticCleanup;
        settings.limitBytes=std::uint64_t(std::clamp(value.limitMiB,64u,102400u))*MiB;
        settings.recentProjects=std::clamp(value.recentProjects,1u,100u);
    }
    bool Save(const SourceScope* observedPublication = nullptr) {
        lastSavePublished = false;
        if (owed.empty()) return SaveIndex(observedPublication);
        if (saving || projectingOwners || !inventoryReady || !healthy || !settingsReadable ||
            forceIndexReload || cache_storage_policy::MemoryOnly()) return false;
        const auto scope = sourceScope.lock();
        if (!scope || scope->transitionActive || !RefreshInventory() || !rootIdentity) return false;
        const PhysicalId directory{rootIdentity->volume, rootIdentity->fileIndex};
        // Hold each surviving name against replacement through the atomic index save.
        std::map<const OwnerProof*, Handle> pins;
        struct RowUndo {
            std::map<std::wstring,ProjectRecord>::iterator row;
            std::uint64_t previous;
            bool created;
        };
        std::vector<RowUndo> rows;
        rows.reserve(owed.size());
        struct Projection {
            State& s;
            std::vector<RowUndo>& rows;
            ~Projection() {
                for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                    if (it->created) s.projects.erase(it->row);
                    else it->row->second.opened = it->previous;
                }
                for (auto& delta : s.owed) if (delta.inserted) {
                    auto entry = s.entries.find(delta.proof->relative);
                    if (entry != s.entries.end()) entry->second.owners.erase(delta.project->id);
                    delta.publishedUnverified = s.lastSavePublished;
                    delta.inserted = false;
                }
                s.projectingOwners = false;
            }
        } projection{*this, rows};
        projectingOwners = true;
        const auto last = std::prev(owed.end());
        for (auto it = owed.begin(); it != std::next(last); ++it) {
            auto& delta = *it;
            const auto& proof = *delta.proof;
            delta.uncertain = false;
            if (directory != proof.root.id) return false;
            const auto observed = observedNames.find(proof.relative);
            if (observed == observedNames.end()) {
                const auto f = Info(root / proof.relative);
                if (f.exists || (f.error != ERROR_FILE_NOT_FOUND && f.error != ERROR_PATH_NOT_FOUND)) delta.uncertain = true;
                continue;
            }
            if (observed->second != proof.file.id) continue; // Verified replacement, no transfer of ownership.
            const auto entry = entries.find(proof.relative);
            if (entry == entries.end()) { delta.uncertain = true; continue; }
            if (!pins.count(&proof)) {
                const auto path = root / proof.relative;
                if (!SafeParents(path)) { delta.uncertain = true; continue; }
                Handle pin(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                OwnerProof sample;
                sample.scope = sourceScope;
                if (!SampleInstances(root, directory, pin.value, observed->second, sample) || !SameInstance(proof, sample)) {
                    delta.uncertain = true;
                    continue;
                }
                pins.emplace(&proof, std::move(pin));
            }
            const auto& project = *delta.project;
            if (projects.size() >= 10000 && !projects.count(project.id)) return false;
            auto [row, created] = projects.try_emplace(project.id, ProjectRecord{project.path, project.opened, false});
            if (created || project.opened > row->second.opened) {
                rows.push_back({row, row->second.opened, created});
                row->second.opened = project.opened;
            }
            auto& owners = entry->second.owners;
            if (owners.size() >= 10000 && !owners.count(project.id)) return false;
            delta.inserted = owners.insert(project.id).second;
        }
        if (!dirty && std::all_of(owed.begin(), std::next(last), [](const auto& d) { return d.uncertain; })) return true;
        if (!SaveIndex()) return false;
        rows.clear();
        // A reentrant lifecycle notification may have appended newer work.
        const auto end = std::next(last);
        for (auto it = owed.begin(); it != end;) {
            const auto entry = entries.find(it->proof->relative);
            if (!it->uncertain || (entry != entries.end() && entry->second.owners.count(it->project->id))) it = owed.erase(it);
            else ++it;
        }
        return true;
    }
    bool SaveIndex(const SourceScope* observedPublication = nullptr) {
        lastSavePublished=false;
        if(!healthy||!settingsReadable||!inventoryReady||saving||forceIndexReload)return false;
        if(FutureIndexBytes()>64*MiB)return false;
        if(projects.size()>10000||entries.size()>100000)return false;
        const auto validText=[](const std::wstring& value){return value.size()<=32768&&value.find(L'\0')==std::wstring::npos;};
        for(const auto&p:projects)if(!validText(p.second.path)||Identity(p.second.path)!=p.first)return false;
        for(const auto&p:entries){if(!validText(p.second.relative)||p.first!=p.second.relative||
            Relative(root,root/p.first)!=p.first||p.second.bytes>256*MiB||p.second.owners.size()>10000)return false;
            for(const auto&owner:p.second.owners)if(!validText(owner)||!projects.count(owner))return false;}
        struct Scope {State& state;bool diskChanged=false;
            ~Scope(){state.saving=false;state.metadataBudget=0;state.metadataTemporary.reset();if(diskChanged)state.inventoryReady=false;}} scope{*this};
        saving=true;
        std::ostringstream stream(std::ios::binary|std::ios::out);
        Write(stream,indexMagic);Write(stream,indexVersion);Write(stream,std::uint32_t(projects.size()));
        for(const auto&p:projects){WriteText(stream,p.second.path);Write(stream,p.second.opened);Write(stream,std::uint8_t(p.second.pinned));}
        Write(stream,std::uint32_t(entries.size()));
        for(const auto&p:entries){const auto&v=p.second;WriteText(stream,v.relative);Write(stream,v.bytes);Write(stream,v.lastUse);
            Write(stream,std::uint8_t(v.invalid));Write(stream,std::uint32_t(v.owners.size()));for(const auto&owner:v.owners)WriteText(stream,owner);}
        if(!stream)return false;
        const auto bytes=stream.str();if(bytes.size()>64*MiB)return false;
        std::error_code e;
        const bool createdParent = fs::create_directories(index.parent_path(), e);
        if(e||!SafeParents(index))return false;
        // Commit just observed the published payload while holding this writer
        // scope. Registration only updates memory. A newly created directory
        // needs a fresh inventory, and the staged and final checks still scan.
        const auto currentScope = observedPublication ? sourceScope.lock() : nullptr;
        const bool reuseInventory = !createdParent && currentScope &&
            currentScope.get() == observedPublication && !currentScope->transitionActive && currentScope->Valid();
        if(!reuseInventory&&!RefreshInventory())return false;
        metadataBudget=Round(bytes.size());
        const auto reserved=Reserved();
        if(reserved>settings.limitBytes||Total()>settings.limitBytes-reserved)return false;
        static volatile LONG serial=0;
        fs::path temp;Handle file;
        for(unsigned attempt=0;attempt<8;++attempt){
            temp=index.wstring()+L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(InterlockedIncrement(&serial));
            file=Handle(CreateFileW(temp.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(file.value!=INVALID_HANDLE_VALUE)break;
            const auto error=GetLastError();if(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS)return false;
        }
        if(file.value==INVALID_HANDLE_VALUE)return false;
        scope.diskChanged=true;
        const auto dispose=[](HANDLE handle){FILE_DISPOSITION_INFO info{};info.DeleteFile=TRUE;return SetFileInformationByHandle(handle,FileDispositionInfo,&info,sizeof(info))!=FALSE;};
        BY_HANDLE_FILE_INFORMATION created{};
        if(!GetFileInformationByHandle(file.value,&created)||created.nNumberOfLinks!=1){dispose(file.value);return false;}
        metadataTemporary=PhysicalId{created.dwVolumeSerialNumber,(std::uint64_t(created.nFileIndexHigh)<<32)|created.nFileIndexLow};
        DWORD written=0;
        if(!WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.value)){
            dispose(file.value);return false;
        }
        // Measurement includes allocation and every ADS before publication.
        if(!RefreshInventory()){dispose(file.value);return false;}
        const auto measured=physical.find(*metadataTemporary);
        if(measured==physical.end()||measured->second.links!=1||measured->second.deletePending||measured->second.identity.fileBytes!=bytes.size()||
           Charge(measured->second)>metadataBudget||Reserved()>settings.limitBytes||Total()>settings.limitBytes-Reserved()){
            dispose(file.value);return false;
        }
        file=Handle{};
        if(!MoveFileExW(temp.c_str(),index.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
            Handle cleanup(CreateFileW(temp.c_str(),DELETE|FILE_READ_ATTRIBUTES,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            BY_HANDLE_FILE_INFORMATION current{};
            if(cleanup.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(cleanup.value,&current)&&
               current.dwVolumeSerialNumber==created.dwVolumeSerialNumber&&current.nFileIndexHigh==created.nFileIndexHigh&&current.nFileIndexLow==created.nFileIndexLow&&
               current.ftCreationTime.dwHighDateTime==created.ftCreationTime.dwHighDateTime&&current.ftCreationTime.dwLowDateTime==created.ftCreationTime.dwLowDateTime&&
               current.nNumberOfLinks==1&&!(current.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))dispose(cleanup.value);
            return false;
        }
        lastSavePublished=true;forceIndexReload=true;healthy=false;
        expectedIndex=metadataTemporary;expectedIndexCreation=created.ftCreationTime;expectedIndexBytes=bytes.size();error=L"index-unresolved";
        if(!RefreshInventory())return false;
        const auto final=Observed(L"Management\\index.bin");
        if(!final||Physical(*final)!=*metadataTemporary||final->links!=1||final->deletePending||
           final->identity.fileBytes!=bytes.size()||Charge(*final)>metadataBudget)return false;
        const auto saved=Info(index);if(!saved.exists||saved.bytes!=bytes.size())return false;
        indexStamp=saved.stamp;indexBytes=saved.bytes;lastFlush=GetTickCount64();dirty=false;
        lastVerifiedIndex=metadataTemporary;lastVerifiedIndexCreation=created.ftCreationTime;
        unconfirmedLegacyOwners = false;
        healthy=true;forceIndexReload=false;expectedIndex.reset();error.clear();scope.diskChanged=false;return true;
    }
    void Load() {
        auto f=Info(index);
        if(!f.exists){if(forceIndexReload||(f.error!=ERROR_FILE_NOT_FOUND&&f.error!=ERROR_PATH_NOT_FOUND)){healthy=false;error=forceIndexReload?L"index-unresolved":L"index-unreadable";return;}
            if(!initialized){entries.clear();projects.clear();}return;}
        if(!SafeParents(index)){healthy=false;error=L"index-unreadable";return;}
        Handle committedPin(CreateFileW(index.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION actual{};
        if(committedPin.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(committedPin.value,&actual)||
           actual.nNumberOfLinks!=1||(actual.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){healthy=false;error=forceIndexReload?L"index-unresolved":L"index-unreadable";return;}
        const PhysicalId actualIdentity{actual.dwVolumeSerialNumber,(std::uint64_t(actual.nFileIndexHigh)<<32)|actual.nFileIndexLow};
        f.bytes=(std::uint64_t(actual.nFileSizeHigh)<<32)|actual.nFileSizeLow;
        f.stamp=(std::uint64_t(actual.ftLastWriteTime.dwHighDateTime)<<32)|actual.ftLastWriteTime.dwLowDateTime;
        if(forceIndexReload){
            if(!expectedIndex||actualIdentity!=*expectedIndex||
               actual.ftCreationTime.dwHighDateTime!=expectedIndexCreation.dwHighDateTime||actual.ftCreationTime.dwLowDateTime!=expectedIndexCreation.dwLowDateTime||
               f.bytes!=expectedIndexBytes){healthy=false;error=L"index-unresolved";return;}
        }
        if(!forceIndexReload&&initialized&&healthy&&lastVerifiedIndex&&actualIdentity==*lastVerifiedIndex&&
           actual.ftCreationTime.dwHighDateTime==lastVerifiedIndexCreation.dwHighDateTime&&actual.ftCreationTime.dwLowDateTime==lastVerifiedIndexCreation.dwLowDateTime&&
           f.stamp==indexStamp&&f.bytes==indexBytes)return;
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
        entries=std::move(loadedEntries);projects=std::move(loadedProjects);indexStamp=f.stamp;indexBytes=f.bytes;healthy=true;error.clear();
        unconfirmedLegacyOwners = false;
        if (forceIndexReload) {
            // The exact candidate index was verified above. Remove just our own
            // unacknowledged insertions before rechecking the payload identities.
            for (auto& delta : owed) if (delta.publishedUnverified) {
                auto it = entries.find(delta.proof->relative);
                if (it != entries.end()) it->second.owners.erase(delta.project->id);
                delta.publishedUnverified = false;
            }
        }
        if (scene.project) current.clear(); // Reapply the active binding to the verified index.
        lastVerifiedIndex=actualIdentity;lastVerifiedIndexCreation=actual.ftCreationTime;
        if(forceIndexReload){dirty=false;forceIndexReload=false;expectedIndex.reset();}
    }
    void ApplySceneBinding(bool accessingPayload = false) {
        if (!scene.project || current == scene.project->id || cache_storage_policy::MemoryOnly() ||
            !healthy || !settingsReadable || !inventoryReady || forceIndexReload) return;
        if (!accessingPayload && seen.empty() && _wcsicmp(root.c_str(), scene.bindingRoot.c_str()) != 0) return;
        const auto& project = *scene.project;
        if (projects.size() >= 10000 && !projects.count(project.id)) return;
        auto [row, inserted] = projects.try_emplace(project.id, ProjectRecord{project.path, project.opened, false});
        if (inserted || row->second.opened < project.opened) {
            row->second.opened = project.opened;
            dirty = true;
        }
        current = project.id;
    }
    void Sync() {
        LoadSettings();Load();
        Reconcile();
        ApplySceneBinding();
        if(!initialized){initialized=true;if(dirty&&healthy)Save();}
        const bool hasOrphans = std::any_of(reservations.begin(), reservations.end(),
            [](const auto& item) { return item.second.orphaned; });
        if (hasOrphans) SettleOrphans();
        else if (!owed.empty() && !cache_storage_policy::MemoryOnly()) Save();
    }
    bool Use(const fs::path& path,bool registerPublication=false,bool persist=true,bool trackUse=true,
             SceneUse* recorded = nullptr) {
        const auto rel=Relative(root,path);const auto f=Info(path);
        if(rel.empty() || !f.exists)return false;
        auto found=entries.find(rel);
        // Reading a recognizable unknown cache is not authority to reclaim it.
        // Only an existing index entry or this producer's publication registers.
        if(found==entries.end() && (!registerPublication||!Recognized(path)))return false;
        const bool needsProof = scene.phase == ScenePhase::Loading || scene.phase == ScenePhase::Active;
        const bool needsOwner = scene.project &&
            (unconfirmedLegacyOwners || found == entries.end() || !found->second.owners.count(scene.project->id));
        if (trackUse) {
            try {
                auto lease = leases.find(rel);
                if (lease == leases.end()) {
                    Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                    if (h.value != INVALID_HANDLE_VALUE)
                        lease = leases.emplace(rel, SceneLease{std::make_shared<Handle>(std::move(h)), scene.serial}).first;
                }
                if (lease != leases.end()) lease->second.useSerial = scene.serial;
                if (needsProof && !RecordOwnerUse(path, rel, needsOwner, recorded)) {
                    if (!registerPublication) scene.incompleteUse = true;
                    return false;
                }
            } catch (...) {
                if (needsProof && !registerPublication) scene.incompleteUse = true;
                throw;
            }
        }
        if (trackUse) ApplySceneBinding(true);
        auto& entry=entries[rel];const bool created=entry.relative.empty();
        entry.relative=rel;entry.bytes=f.bytes;entry.invalid=false;
        // Registration-only settlement records a publication; it must not claim current use.
        if(trackUse||created)entry.lastUse=Now();
        bool association=false;
        if(trackUse) {
            if (scene.acceptsUse()) {
                seen.insert(rel);
                if (!pending && !current.empty()) {
                    association = scene.project ? !entry.owners.count(current) : entry.owners.insert(current).second;
                    if (association && !scene.project) unconfirmedLegacyOwners = true;
                }
            }
        }
        dirty=true;
        if(persist&&(created || association || GetTickCount64()-lastFlush>=5000))Save();
        return true;
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
    std::uint64_t Total() const noexcept {return inventoryReady?physicalBytes:UINT64_MAX;}
    std::uint64_t LegacyReserved() const noexcept {
        if(!inventoryReady)return UINT64_MAX;
        std::uint64_t bytes=0;
        for(const auto&p:reservations)bytes=AddBytes(bytes,Residual(p.second.budget,p.second.temporary));
        // One shared future index publication, not one full index per writer.
        const auto metadata=saving?Residual(metadataBudget,metadataTemporary):(reservations.empty() && owed.empty())?0:Round(FutureIndexBytes());
        return AddBytes(bytes,metadata);
    }
    std::uint64_t Reserved() const noexcept {return AddBytes(AddBytes(sourceRawResidual,sourceMetadataBudget),LegacyReserved());}
    bool UnregisteredPublication() const {for(const auto& p:reservations)if(p.second.publicationPending)return true;return false;}
    void SettleOrphans() {
        bool any=false;for(const auto& p:reservations)any=any||p.second.orphaned;
        if(!any || !healthy)return;
        Reconcile();
        if(!inventoryReady)return;
        // An inaccessible or malformed final file is not proof of deletion.
        // Keep its charge, and refuse further publication until resolved.
        for(auto it=reservations.begin();it!=reservations.end();) {
            if(!it->second.orphaned){++it;continue;}
            const auto path=root/it->first;const auto f=Info(path);
            if(f.exists) {
                const auto observed=Observed(it->first);
                if(!observed||!it->second.temporary||Physical(*observed)!=*it->second.temporary||
                   observed->links!=1||observed->deletePending||f.bytes>it->second.bytes||Charge(*observed)>it->second.budget||!Recognized(path))return;
                // Verified old publication, not current scene use: register only.
                Use(path,true,false,false);
                auto entry=entries.find(it->first);
                if(entry==entries.end() || entry->second.bytes!=f.bytes)return;
                ++it;
            } else {
                if(f.error!=ERROR_FILE_NOT_FOUND && f.error!=ERROR_PATH_NOT_FOUND)return;
                if(it->second.temporary&&physical.count(*it->second.temporary))return;
                it=reservations.erase(it);
            }
        }
        if(!UnregisteredPublication())pendingPersistenceOwner.reset();
        if(!Save())return;
        for(auto it=reservations.begin();it!=reservations.end();) {
            if(it->second.orphaned)it=reservations.erase(it);else ++it;
        }
        if(!UnregisteredPublication())pendingPersistenceOwner.reset();
    }
    void Reconcile() {
        if(!RefreshInventory()){error=L"inventory-unreadable";return;}
        if(error==L"inventory-unreadable")error.clear();
        for(auto it=entries.begin();it!=entries.end();) {
            const auto f=Info(root/it->first);
            if(!f.exists && (f.error==ERROR_FILE_NOT_FOUND || f.error==ERROR_PATH_NOT_FOUND)){it=entries.erase(it);dirty=true;}
            else {
                if(!f.exists){inventoryReady=false;error=L"inventory-unreadable";return;}
                it->second.bytes=f.bytes;++it;
            }
        }
    }
    bool RepairIndexProof(Handle& pin) {
        if(forceIndexReload||!lastVerifiedIndex||!inventoryReady||!settingsReadable)return false;
        const auto observed=Observed(L"Management\\index.bin");
        if(!observed||Physical(*observed)!=*lastVerifiedIndex||observed->kind!=cache_files::ObservationKind::File||
           observed->links!=1||observed->deletePending)return false;
        Handle candidate(CreateFileW(index.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION actual{};
        if(candidate.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(candidate.value,&actual)||
           actual.nNumberOfLinks!=1||(actual.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
           actual.dwVolumeSerialNumber!=lastVerifiedIndex->first||((std::uint64_t(actual.nFileIndexHigh)<<32)|actual.nFileIndexLow)!=lastVerifiedIndex->second||
           actual.ftCreationTime.dwHighDateTime!=lastVerifiedIndexCreation.dwHighDateTime||actual.ftCreationTime.dwLowDateTime!=lastVerifiedIndexCreation.dwLowDateTime)return false;
        pin=std::move(candidate);return true;
    }
    CleanupResult Remove(Cleanup mode,const std::wstring& project,std::uint64_t target,bool automatic=false,const std::wstring& except={}) {
        CleanupResult result;
        if(!inventoryReady){result.error=L"inventory-unreadable";return result;}
        const bool repair=!healthy;
        Handle repairPin;
        if(repair&&(mode!=Cleanup::All||automatic||!RepairIndexProof(repairPin))){result.error=error;return result;}
        std::wstring repairError=error;
        struct Candidate {
            std::wstring name;
            std::optional<cache_files::Identity> identity;
        };
        const auto recent=Recent();std::vector<Candidate> candidates;
        for(const auto& p:entries) {
            const auto& entry=p.second;
            if(p.first==except || reservations.count(p.first))continue;
            if(mode==Cleanup::Project && (!entry.owners.count(project) || entry.owners.size()!=1))continue;
            if(mode==Cleanup::Reclaimable && (Pinned(entry) || (!automatic && !entry.invalid && RecentEntry(entry,recent))))continue;
            const auto observed = Observed(p.first);
            candidates.push_back({p.first, observed ? std::optional{observed->identity} : std::nullopt});
        }
        std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) {
            const auto& x=entries.at(a.name);const auto& y=entries.at(b.name);
            const int px=x.invalid?0:RecentEntry(x,recent)?2:1,py=y.invalid?0:RecentEntry(y,recent)?2:1;
            return px!=py?px<py:x.lastUse!=y.lastUse?x.lastUse<y.lastUse:a.name<b.name;
        });
        auto total=Total();
        for(const auto& candidate:candidates) {
            if(automatic && total<=target)break;
            const auto& name = candidate.name;
            const auto path = root/name;
            // A refresh after deleting another entry must not authorize a new
            // identity at a name selected by the original cleanup pass.
            if(leases.count(name) || !candidate.identity || !SafeParents(path) ||
               cache_files::DeleteObservedFile(path,*candidate.identity)!=cache_files::Result::Ok) {
                ++result.skipped;continue;
            }
            const auto after=Info(path);
            if(after.exists || (after.error!=ERROR_FILE_NOT_FOUND && after.error!=ERROR_PATH_NOT_FOUND)) {
                ++result.skipped;continue;
            }
            const auto before=Total();
            ++result.files;entries.erase(name);dirty=true;
            if(!RefreshInventory()){result.error=L"inventory-unreadable";break;}
            total=Total();if(before>total)result.bytes=AddBytes(result.bytes,before-total);
        }
        // Only an explicit clear-all with this exact previously verified index
        // may rebuild corrupt bytes. No read failure or replacement is adoption.
        if(repair){repairPin=Handle{};healthy=true;dirty=true;}
        try {
            if(dirty&&!Save()){
                if(repair&&!lastSavePublished){healthy=false;error.swap(repairError);}
                result.error=L"index-unwritable";
            }
        }catch(...){if(repair&&!lastSavePublished){healthy=false;error.swap(repairError);}throw;}
        return result;
    }
};
std::shared_ptr<State> state;
// Reentrant test/configuration root changes must share the reservations of a
// still-live old root. Weak entries do not retain every historical cache root.
struct LiveRoot {
    std::weak_ptr<State> lookup;
    std::shared_ptr<State> sceneHold;
};
std::map<std::wstring,LiveRoot> liveStates;

void ClearSceneUse(bool releaseLeases) {
    const auto clear = [releaseLeases](State& value) {
        value.current.clear();
        value.seen.clear();
        value.sceneUses.clear();
        value.pending = false;
        if (releaseLeases) value.leases.clear();
    };
    if (state) clear(*state);
    for (auto& item : liveStates) {
        if (auto value = item.second.lookup.lock()) {
            if (value != state) clear(*value);
            // An accepted boundary keeps even switched-away roots alive until
            // their handles can retire. Updating an existing slot cannot allocate.
            item.second.sceneHold = value->leases.empty() && value->owed.empty() ? nullptr : value;
        }
    }
    scene.project.reset();
    scene.incompleteUse = false;
    scene.bindingRoot.clear();
}

void AdvanceSceneSerial() noexcept {
    // Saturate instead of allowing a stale ticket to become current again.
    if (scene.serial < UINT64_MAX) ++scene.serial;
}
// Shutdown's optional flush must not wait on another root's cross mutex while
// this thread still holds the recursive local lock and a source exclusion. A
// live guard or reservation owns both its SourceScope and that local lock, so
// the scan under the local lock detects a same-thread live scope; the same lock
// keeps the state/liveStates walk stable.
bool ActiveSourceScopeLocked() {
    const auto active=[](const State& value){return bool(value.sourceScope.lock());};
    if(state&&active(*state))return true;
    for(const auto& item:liveStates)if(auto value=item.second.lookup.lock())if(active(*value))return true;
    return false;
}
bool ActiveSourceScope() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    return ActiveSourceScopeLocked();
}
struct MutexOwnership {
    Handle handle;
    bool acquired=false;
    ~MutexOwnership(){if(acquired)ReleaseMutex(handle.value);}
};
struct Guard {
    MutexOwnership cross;
    std::unique_lock<std::recursive_mutex> local;
    std::shared_ptr<State> owned;
    std::shared_ptr<SourceScope> source;
    State* s=nullptr;
    bool admitted=false;
    // shutdownOnly is private: it may only add an already-known state's optional
    // index flush. It never creates a state/root, switches the current root,
    // prunes liveStates or runs Sync.
    explicit Guard(bool recovery=false,bool shutdownOnly=false):local(stateMutex,std::defer_lock) {
        const auto root=RootPath();if(root.empty() || !SafeParents(root))return;
        std::uint64_t hash=1469598103934665603ull;for(auto c:Identity(root)){hash^=c;hash*=1099511628211ull;}
        const std::wstring name=L"Local\\MMS-cache-"+std::to_wstring(hash);
        cross.handle=Handle(CreateMutexW(nullptr,FALSE,name.c_str()));if(!cross.handle.value || cross.handle.value==INVALID_HANDLE_VALUE)return;
        const DWORD wait=WaitForSingleObject(cross.handle.value,2000);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)return;
        cross.acquired=true;local.lock();
        const auto identity=Identity(root);
        if(shutdownOnly) {
            // Resolve only state that this process already owns. Unknown or
            // expired roots get no directory, index or Video area created.
            std::shared_ptr<State> known;
            if(state&&Identity(state->root)==identity)known=state;
            else{const auto found=liveStates.find(identity);if(found!=liveStates.end())known=found->second.lookup.lock();}
            if(!known||known->saving||known->projectingOwners||ActiveSourceScopeLocked())return;
            if(!known->initialized||!known->healthy||known->forceIndexReload||!known->dirty)return;
            if(!known->reservations.empty()||known->pendingPersistenceOwner)return;
            known->LoadSettings();if(!known->settingsReadable)return;
            owned=std::move(known);
            // Ordinary budget validation on the already-existing root; no
            // recovery shortcut and no root/Video creation.
            source=std::make_shared<SourceScope>(root,false,false);
            if(source->transitionActive||!source->Valid())return;
            // The state's weak reference points at this temporary read scope
            // and expires when the guard drops the last reference.
            owned->sourceScope=source;
            // RefreshInventory may validate physical quota, and RepairIndexProof
            // proves the last verified index still identifies the only file.
            Handle proofPin;
            if(!owned->RefreshInventory()||!owned->RepairIndexProof(proofPin))return;
            const auto disk=Info(owned->index);
            proofPin=Handle{}; // own atomic replace must not be blocked by this pin
            if(!disk.exists||disk.stamp!=owned->indexStamp||disk.bytes!=owned->indexBytes)return;
            s=owned.get();admitted=true;return;
        }
        if(!state || Identity(state->root)!=identity) {
            const auto found=liveStates.find(identity);
            auto candidate=found==liveStates.end()?std::shared_ptr<State>{}:found->second.lookup.lock();
            if(!candidate)candidate=std::make_shared<State>(root);
            if (state) {
                const bool sceneUse = scene.phase != ScenePhase::Legacy &&
                    (!state->leases.empty() || !state->seen.empty() || !state->sceneUses.empty() || !state->reservations.empty());
                liveStates.at(Identity(state->root)).sceneHold = sceneUse || !state->owed.empty() ? state : nullptr;
            }
            liveStates[identity].lookup=candidate;state=std::move(candidate);
        }
        for(auto it=liveStates.begin();it!=liveStates.end();) {
            if(it->second.lookup.expired())it=liveStates.erase(it);else ++it;
        }
        owned=state;if(owned->saving || owned->projectingOwners)return;
        source=owned->sourceScope.lock();
        if(recovery&&source)return; // never borrow a normal writer's observation
        if(!source){source=std::make_shared<SourceScope>(root,recovery);owned->sourceScope=source;}
        if(source->transitionActive)return;
        s=owned.get();
        if(recovery){
            source->transitionActive=true;
            s->inventoryReady=false;
            // Recovery supplies a verified endpoint before inventory. Sync
            // would reconcile/save/delete before the pending intent is settled.
            s->LoadSettings();s->Load();return;
        }
        if(!source->Valid()){s->inventoryReady=false;s->error=L"inventory-unreadable";return;}
        s->Sync();
        // Initial verification gates public mutations. Keep this admission
        // stable for an existing token so a later I/O failure can still settle
        // its reservation or explicitly retry Commit under the same held lock.
        admitted=s->inventoryReady;
    }
    explicit operator bool() const {return s&&admitted;}
};
}
fs::path Root() noexcept {try{return RootPath();}catch(...){return {};}}
bool Managed(const fs::path& p) noexcept {try{return !Relative(RootPath(),p).empty();}catch(...){return false;}}
Settings Current() noexcept {
    try {
        cache_policy::Snapshot snapshot;
        if(cache_policy::Store(RootPath(),SettingsPath()).Read(snapshot)!=cache_policy::Result::Ok)return UnavailableSettings();
        const auto& value=snapshot.Value();
        Settings out;
        out.enabled=value.enabled;out.automaticCleanup=value.automaticCleanup;
        out.limitBytes=std::uint64_t(std::clamp(value.limitMiB,64u,102400u))*MiB;
        out.recentProjects=std::clamp(value.recentProjects,1u,100u);
        return out;
    } catch(...) {}
    return UnavailableSettings();
}
bool Enabled() noexcept {return Current().enabled;}
bool Configure(const Settings& value,std::wstring& error) noexcept {
    try {
        if(value.limitBytes<64*MiB || value.limitBytes>102400*MiB || value.recentProjects<1 || value.recentProjects>100){error=L"invalid-settings";return false;}
        const auto root=RootPath();if(root.empty()){error=L"cache-disabled";return false;}
        // Establish disk admission before persisting any settings. Retain it
        // through the settings commit and its optional cleanup.
        Guard g;if(!g){error=L"cache-busy";return false;}
        auto& s=*g.s;
        cache_policy::Store settings(s.root,SettingsPath());
        cache_policy::Snapshot expected;
        const auto read=settings.Read(expected);
        if(read==cache_policy::Result::Disabled){error=L"cache-disabled";return false;}
        if(read!=cache_policy::Result::Ok){error=L"cache-busy";return false;}
        cache_policy::Configuration desired;
        desired.enabled=value.enabled;desired.automaticCleanup=value.automaticCleanup;
        desired.limitMiB=static_cast<std::uint32_t>(value.limitBytes/MiB);
        desired.recentProjects=value.recentProjects;
        cache_policy::Snapshot updated;
        const auto written=settings.Commit(expected,desired,updated);
        if(written==cache_policy::Result::Conflict){error=L"cache-conflict";return false;}
        if(written!=cache_policy::Result::Ok){error=written==cache_policy::Result::Busy?L"cache-busy":L"settings-unwritable";return false;}
        s.settings=value;
        s.settings.limitBytes=std::uint64_t(updated.Value().limitMiB)*MiB;
        s.settingsReadable=true;
        // Settings are committed. Automatic eviction is opportunistic and
        // can resume during the next admission if this pass cannot finish.
        try {
            const auto limit=s.settings.limitBytes, reserved=s.Reserved();
            if(value.enabled && value.automaticCleanup && (reserved>limit || s.Total()>limit-reserved))
                s.Remove(Cleanup::Reclaimable,{},reserved>limit?0:std::min(limit*8/10,limit-reserved),true);
        }catch(...){}
        error.clear();return true;
    }catch(...){error=L"cache-unavailable";return false;}
}
void Touch(const fs::path& path) noexcept {
    try {
        if (cache_storage_policy::MemoryOnly() || !Managed(path)) return;
        Guard g;
        if (!g) return;
        g.s->Use(path);
    } catch (...) {}
}

namespace {
std::uint64_t FileTimeValue(FILETIME time) noexcept {
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
bool SameFile(const BY_HANDLE_FILE_INFORMATION& info, const InstanceStamp& expected) noexcept {
    return info.dwVolumeSerialNumber == expected.id.first &&
        ((std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) == expected.id.second &&
        expected.created && FileTimeValue(info.ftCreationTime) == expected.created;
}
struct TextureVersion {
    LONGLONG changed = 0;
    std::uint64_t written = 0, size = 0;
    bool operator==(const TextureVersion& rhs) const noexcept {
        return changed == rhs.changed && written == rhs.written && size == rhs.size;
    }
};
bool ReadTextureVersion(HANDLE handle, const InstanceStamp& expected, TextureVersion& version) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (handle == INVALID_HANDLE_VALUE || !handle || !GetFileInformationByHandle(handle, &info) ||
        !SameFile(info, expected) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof basic) ||
        !basic.ChangeTime.QuadPart) return false;
    version = {basic.ChangeTime.QuadPart, FileTimeValue(info.ftLastWriteTime),
        (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
    return true;
}
bool SameVerifiedIndex(const State& cache) {
    if (!cache.lastVerifiedIndex || !SafeParents(cache.index)) return false;
    Handle index(CreateFileW(cache.index.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    return index.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(index.value, &info) &&
        info.nNumberOfLinks == 1 && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        SameFile(info, {*cache.lastVerifiedIndex, FileTimeValue(cache.lastVerifiedIndexCreation)}) &&
        FileTimeValue(info.ftLastWriteTime) == cache.indexStamp &&
        ((std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) == cache.indexBytes;
}
}

struct TextureUseReceipt::Impl {
    std::weak_ptr<State> cache;
    std::weak_ptr<const RetainedProof> use;
    fs::path settingsPath;
    Settings settings;
    std::optional<PhysicalId> indexIdentity;
    std::uint64_t indexStamp;
    TextureVersion version;
    std::uint64_t serial;

    bool StillCurrent(const fs::path& path) const;
};

bool TextureUseReceipt::Impl::StillCurrent(const fs::path& path) const {
    if (cache_storage_policy::MemoryOnly()) return false;
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    const auto owner = cache.lock();
    const auto proof = use.lock();
    if (!owner || state != owner || !proof ||
        scene.phase != ScenePhase::Loading || scene.serial != serial ||
        scene.incompleteUse || !owner->healthy || !owner->settingsReadable || !owner->inventoryReady ||
        owner->saving || owner->projectingOwners || owner->forceIndexReload ||
        !owner->owed.empty() || !owner->reservations.empty() || owner->pendingPersistenceOwner ||
        GetTickCount64() - owner->lastFlush >= 5000 || owner->lastVerifiedIndex != indexIdentity ||
        owner->indexStamp != indexStamp || RootPath() != owner->root || SettingsPath() != settingsPath ||
        Relative(owner->root, path) != proof->relative) return false;

    const auto lease = owner->leases.find(proof->relative);
    const auto entry = owner->entries.find(proof->relative);
    if (lease == owner->leases.end() || lease->second.useSerial != serial ||
        lease->second.handle != proof->lease.lock() || entry == owner->entries.end() ||
        entry->second.invalid || !owner->seen.count(proof->relative) ||
        std::find(owner->sceneUses.begin(), owner->sceneUses.end(), proof) == owner->sceneUses.end()) return false;

    Handle root(CreateFileW(owner->root.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Handle payload(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    InstanceStamp rootStamp;
    TextureVersion current;
    cache_policy::Snapshot policy;
    if (!ReadStamp(root.value, proof->root.id, true, rootStamp) || rootStamp.created != proof->root.created ||
        !ReadTextureVersion(payload.value, proof->file, current) || !(current == version) ||
        !SameVerifiedIndex(*owner) ||
        cache_policy::Store(owner->root, settingsPath).Read(policy) != cache_policy::Result::Ok) return false;
    const auto& config = policy.Value();
    return config.enabled == settings.enabled && config.automaticCleanup == settings.automaticCleanup &&
        config.LimitBytes() == settings.limitBytes && config.recentProjects == settings.recentProjects;
}

TextureUseReceipt TouchTextureRead(const fs::path& path, std::intptr_t readHandle) noexcept {
    TextureUseReceipt receipt;
    try {
        if (cache_storage_policy::MemoryOnly() || !Managed(path)) return receipt;
        Guard g;
        SceneUse use;
        if (!g || !g.s->Use(path, false, true, true, &use) ||
            scene.phase != ScenePhase::Loading || !use || use->lease.expired()) return receipt;
        TextureVersion version;
        if (!ReadTextureVersion(reinterpret_cast<HANDLE>(readHandle), use->file, version)) return receipt;
        receipt.proof_ = std::make_shared<const TextureUseReceipt::Impl>(
            TextureUseReceipt::Impl{g.owned, use, SettingsPath(), g.s->settings,
                g.s->lastVerifiedIndex, g.s->indexStamp, version, scene.serial});
    } catch (...) {}
    return receipt;
}

void FinishTextureUse(const fs::path& path, TextureUseReceipt receipt) noexcept {
    try {
        if (receipt.proof_ && receipt.proof_->StillCurrent(path)) return;
    } catch (...) {}
    Touch(path);
}

void Invalidate(const fs::path& path) noexcept {try{if(cache_storage_policy::MemoryOnly() || !Managed(path))return;Guard g;if(g){const auto id=Relative(g.s->root,path);auto it=g.s->entries.find(id);if(it!=g.s->entries.end()){it->second.invalid=true;g.s->dirty=true;g.s->Save();}}}catch(...){} }
SceneToken AcceptScene(SceneOperation operation) noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        if (operation != SceneOperation::Load && operation != SceneOperation::Reset) return {};
        ClearSceneUse(false);
        AdvanceSceneSerial();
        if (scene.serial == UINT64_MAX) { scene.phase = ScenePhase::Isolated; return {}; }
        scene.phase = operation == SceneOperation::Load ? ScenePhase::Loading : ScenePhase::Resetting;
        return SceneToken(scene.serial);
    } catch (...) { return {}; }
}

namespace {
// The native host has finished replacing the old scene. Keep leases observed
// by the new generation, including shared payloads whose handles were reused.
// Failed replacements keep all leases until a real reset or successful load.
void RetirePreviousSceneLeases() noexcept {
    for (auto& item : liveStates) {
        if (auto value = item.second.lookup.lock()) {
            for (auto it = value->leases.begin(); it != value->leases.end();) {
                if (it->second.useSerial != scene.serial) it = value->leases.erase(it);
                else ++it;
            }
            if (value->leases.empty() && value->owed.empty()) item.second.sceneHold.reset();
        }
    }
}

// The caller holds stateMutex. No Guard here: disk admission and publication
// may be deferred, even across another scene or cache-root change.
bool BindSceneUses(const wchar_t* documentPath) {
    if (scene.incompleteUse || !documentPath || !*documentPath) return false;
    auto path = Full(documentPath).wstring();
    if (path.empty() || path.size() > 32768) return false;
    auto id = Lower(path);
    auto root = RootPath();
    auto project = std::make_shared<const OwnerProject>(OwnerProject{std::move(path), std::move(id), Now()});
    // Stage every new obligation before making any of them visible. Failure
    // drops just these temporary links; scene uses and previous debts survive.
    std::map<std::shared_ptr<State>, std::list<OwnerDelta>> staged;
    for (const auto& item : liveStates) if (auto value = item.second.lookup.lock()) {
        for (const auto& use : value->sceneUses) {
            const bool queued = std::any_of(value->owed.begin(), value->owed.end(), [&](const auto& delta) {
                return delta.project->id == project->id && delta.proof == use;
            });
            if (!queued) staged[value].emplace_back(use, project);
        }
    }
    for (auto& [value, additions] : staged) {
        value->owed.splice(value->owed.end(), additions);
        value->dirty = true;
    }
    scene.bindingRoot.swap(root);
    scene.project = std::move(project);
    return true;
}
}

bool FinishScene(SceneToken token, SceneOutcome outcome, const wchar_t* loadedPath) noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        if (!token || token.serial_ != scene.serial ||
            (scene.phase != ScenePhase::Loading && scene.phase != ScenePhase::Resetting)) return false;
        if (outcome == SceneOutcome::Loaded && scene.phase == ScenePhase::Loading && loadedPath && *loadedPath) {
            if (!BindSceneUses(loadedPath)) return false;
            RetirePreviousSceneLeases();
            scene.phase = ScenePhase::Active;
            return true;
        }
        if (loadedPath) return false;
        if (outcome == SceneOutcome::ResetComplete && scene.phase == ScenePhase::Resetting) {
            ClearSceneUse(true);
            scene.phase = ScenePhase::Active;
            return true;
        }
        if (outcome == SceneOutcome::Failed) {
            ClearSceneUse(false);
            scene.phase = ScenePhase::Isolated;
            return true;
        }
        return false;
    } catch (...) { return false; }
}

SceneToken CaptureActiveScene() noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        if (scene.phase == ScenePhase::Active) return SceneToken(scene.serial);
    } catch (...) {}
    return {};
}

bool BindSavedScene(SceneToken token, const wchar_t* savedPath) noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        if (!token || token.serial_ != scene.serial || scene.phase != ScenePhase::Active) return false;
        // The host has already accepted a successful publication. Stop all
        // roots charging the old target before even allocating the new path.
        scene.project.reset();
        scene.bindingRoot.clear();
        for (auto& item : liveStates)
            if (auto value = item.second.lookup.lock()) value->current.clear();
        return BindSceneUses(savedPath);
    } catch (...) { return false; }
}

void BeginProject(const wchar_t* path) noexcept {
    try {
        if (cache_storage_policy::MemoryOnly()) return;
        Guard g;
        if (!g) return;
        auto& s = *g.s;
        auto id = Identity(path ? path : L"");
        if (s.dirty) s.Save();
        if (scene.phase != ScenePhase::Legacy) {
            ClearSceneUse(true);
            AdvanceSceneSerial();
            scene.phase = ScenePhase::Legacy;
        }
        s.leases.clear();
        s.current.swap(id);
        s.seen.clear();
        s.pending = true;
    } catch (...) {}
}
void ProjectOpened(const wchar_t* path) noexcept {
    try{if(cache_storage_policy::MemoryOnly() || !path||!*path)return;Guard g;if(!g || scene.phase != ScenePhase::Legacy)return;auto& s=*g.s;const auto id=Identity(path);
        auto& p=s.projects[id];p.path=Full(path).wstring();p.opened=Now();s.current=id;s.pending=false;
        for(const auto& name:s.seen){auto it=s.entries.find(name);if(it!=s.entries.end() && it->second.owners.insert(id).second)s.unconfirmedLegacyOwners=true;}
        s.dirty=true;s.Save();
    }catch(...){}
}
void EndProjectLoad() noexcept {
    try {
        if (cache_storage_policy::MemoryOnly()) return;
        Guard g;
        if (g && scene.phase == ScenePhase::Legacy && g.s->pending) {
            g.s->pending = false;
            g.s->current.clear();
            g.s->seen.clear();
            g.s->leases.clear();
        }
    } catch (...) {}
}
void ResetScene() noexcept {
    try {
        if (cache_storage_policy::MemoryOnly()) return;
        Guard g;
        if (g && scene.phase == ScenePhase::Legacy && !g.s->pending) {
            g.s->current.clear();
            g.s->seen.clear();
            g.s->leases.clear();
            if (g.s->dirty) g.s->Save();
        }
    } catch (...) {}
}
void Shutdown() noexcept {
    // The optional flush is the only step that may touch the disk. Skip it in
    // memory-only mode and whenever any live source exclusion exists, so no
    // cross-root mutex is attempted while a recursive local lock is held.
    // Any failure here cannot suppress the independent local cleanup below.
    try {
        if(!cache_storage_policy::MemoryOnly() && !ActiveSourceScope()) {
            Guard guard(false,true);
            if(guard && guard.s->dirty)guard.s->Save();
        }
    }catch(...){}
    // Release this process's local usage leases unconditionally. current, seen,
    // pending and leases are local scene state; entries, owners, reservations,
    // pendingPersistenceOwner and physical accounting are preserved.
    try {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        ClearSceneUse(true);
        AdvanceSceneSerial();
        scene.phase = ScenePhase::Legacy;
    }catch(...){}
}
bool Pin(const std::wstring& path,bool pin) noexcept {
    try {
        Guard g;if(!g||!g.s->healthy||g.s->forceIndexReload)return false;
        const auto key=Identity(path);auto it=g.s->projects.find(key);if(it==g.s->projects.end())return false;
        const bool previous=it->second.pinned,dirty=g.s->dirty;it->second.pinned=pin;g.s->dirty=true;
        const auto failed=[&](){
            if(g.s->lastSavePublished){
                // Rename succeeded; restoring the old in-memory pin would hide
                // a committed operation. Resolve the exact candidate index.
                try{g.s->Load();}catch(...){g.s->healthy=false;return false;}
                if(!g.s->healthy||g.s->forceIndexReload)return false;
                const auto actual=g.s->projects.find(key);
                return actual!=g.s->projects.end()&&actual->second.pinned==pin;
            }
            it->second.pinned=previous;g.s->dirty=dirty;return false;
        };
        try{if(g.s->Save())return true;}catch(...){return failed();}
        return failed();
    }catch(...){return false;}
}
Snapshot Inspect() noexcept {
    Snapshot out;
    try{Guard g;if(!g){
            if(g.s){out.totalBytes=g.s->physicalBytes;out.settings=g.s->settingsReadable?g.s->settings:UnavailableSettings();out.error=L"inventory-unreadable";}
            else out.error=L"cache-busy";
            return out;
        }auto& s=*g.s;s.Reconcile();out.settings=s.settingsReadable?s.settings:UnavailableSettings();out.error=s.error;
        if(!s.settingsReadable && out.error.empty())out.error=L"settings-unreadable";
        if(!s.inventoryReady){out.totalBytes=s.physicalBytes;out.error=L"inventory-unreadable";return out;}
        out.totalBytes=s.Total();
        const auto recent=s.Recent();
        std::map<std::wstring,std::size_t> projectRows;
        std::set<PhysicalId> categorized;
        std::map<PhysicalId,std::set<std::wstring>> physicalOwners;
        std::map<std::wstring,std::set<PhysicalId>> projectFiles;
        for(const auto&p:s.entries)if(const auto observed=s.Observed(p.first))
            physicalOwners[Physical(*observed)].insert(p.second.owners.begin(),p.second.owners.end());
        for(const auto& p:s.projects){projectRows[p.first]=out.projects.size();out.projects.push_back({p.second.path,p.second.opened,0,0,0,p.second.pinned,p.first==s.current});}
        for(const auto& p:s.entries){const auto& v=p.second;const auto observed=s.Observed(p.first);
            if(!observed)continue;
            const auto bytes=Charge(*observed);
            const bool first=categorized.insert(Physical(*observed)).second;
            const bool available=observed->links==1&&observed->streamCount<=1&&!observed->deletePending&&
                !s.leases.count(p.first)&&Deletable(s.root/p.first);
            if(first){if(p.first.rfind(L"mme\\",0)==0)out.effectBytes=AddBytes(out.effectBytes,bytes);else out.textureBytes=AddBytes(out.textureBytes,bytes);}
            if(first&&available && !s.Pinned(v) && (v.invalid || !s.RecentEntry(v,recent)))out.reclaimableBytes=AddBytes(out.reclaimableBytes,bytes);
            for(const auto& owner:physicalOwners[Physical(*observed)]){auto row=projectRows.find(owner);if(row==projectRows.end())continue;
                if(!projectFiles[owner].insert(Physical(*observed)).second)continue;
                auto& project=out.projects[row->second];project.totalBytes=AddBytes(project.totalBytes,bytes);
                if(physicalOwners[Physical(*observed)].size()>1)project.sharedBytes=AddBytes(project.sharedBytes,bytes);else if(available)project.reclaimableBytes=AddBytes(project.reclaimableBytes,bytes);}
        }
        std::sort(out.projects.begin(),out.projects.end(),[](const Project&a,const Project&b){return a.lastOpened>b.lastOpened;});
        if(s.dirty&&s.healthy)s.Save();
        if(s.inventoryReady)out.totalBytes=s.Total();else out.error=L"inventory-unreadable";
        if (out.error.empty() && std::any_of(s.owed.begin(), s.owed.end(), [](const auto& d){ return d.uncertain; }))
            out.error = L"owner-identity-unconfirmed";
    }catch(...){out.error=L"cache-unavailable";}
    return out;
}
CleanupResult Clean(Cleanup mode,const std::wstring& project) noexcept {
    try{Guard g;if(!g)return {0,0,0,L"cache-busy"};g.s->Reconcile();return g.s->Remove(mode,Identity(project),0);}catch(...){return {0,0,0,L"cache-unavailable"};}
}
struct WriteReservation::Impl {
    std::unique_ptr<Guard> guard;
    fs::path path;
    std::wstring relative;
    std::uint64_t useSerial = 0;
    bool allowed=false,managed=false,owns=false,committed=false;
    ~Impl() {
        if(!owns || !guard || !*guard)return;
        auto& s=*guard->s;const auto found=s.reservations.find(relative);
        if(found==s.reservations.end())return;
        if(found->second.publicationPending)found->second.orphaned=true;
        else s.reservations.erase(found);
    }
};
WriteReservation::WriteReservation(const fs::path& path,std::uint64_t bytes) noexcept {
    try{impl_=std::make_unique<Impl>();impl_->path=path;impl_->managed=Managed(path);
        if(!impl_->managed){impl_->allowed=true;return;}
        if(cache_storage_policy::MemoryOnly())return;
        impl_->guard=std::make_unique<Guard>();auto& g=*impl_->guard;if(!g)return;
        auto& s=*g.s;if(!s.settingsReadable || !s.settings.enabled || !s.healthy || !s.inventoryReady || !bytes||bytes>256*MiB || bytes>s.settings.limitBytes || s.UnregisteredPublication())return;
        if (scene.acceptsUse() && scene.serial != UINT64_MAX) impl_->useSerial = scene.serial;
        impl_->relative=Relative(s.root,path);
        if(impl_->relative.empty() || s.reservations.count(impl_->relative))return;
        // Include the project's row and prospective owner in the existing index budget.
        s.ApplySceneBinding(true);
        ReservationRecord record;record.bytes=bytes;record.budget=s.Round(bytes);record.useSerial=impl_->useSerial;
        s.reservations.emplace(impl_->relative,std::move(record));impl_->owns=true;
        auto reserved=s.Reserved();
        if(reserved<=s.settings.limitBytes && s.Total()>s.settings.limitBytes-reserved && s.settings.automaticCleanup)
            s.Remove(Cleanup::Reclaimable,{},std::min(s.settings.limitBytes*8/10,s.settings.limitBytes-reserved),true,impl_->relative);
        reserved=s.Reserved();
        impl_->allowed=s.inventoryReady&&reserved<=s.settings.limitBytes && s.Total()<=s.settings.limitBytes-reserved;
        if(!impl_->allowed){s.reservations.erase(impl_->relative);impl_->owns=false;}
    }catch(...){impl_.reset();}
}
WriteReservation::~WriteReservation()=default;
WriteReservation::WriteReservation(WriteReservation&&) noexcept=default;
WriteReservation& WriteReservation::operator=(WriteReservation&&) noexcept=default;
WriteReservation::operator bool() const noexcept {return impl_ && impl_->allowed;}
bool WriteReservation::TrackTemporary(const fs::path& path) noexcept {
    try {
        if(!*this)return false;
        if(!impl_->managed)return true;
        if(!impl_->owns||impl_->committed||!impl_->guard||!*impl_->guard)return false;
        auto&s=*impl_->guard->s;const auto record=s.reservations.find(impl_->relative);
        if(s.saving||s.forceIndexReload||record==s.reservations.end()||record->second.publicationPending)return false;
        const auto full=Full(path),target=Full(impl_->path);
        const auto prefix=Lower(target.filename().wstring()+L".tmp-");
        const auto filename=Lower(full.filename().wstring());
        if(Identity(full.parent_path())!=Identity(target.parent_path())||filename.size()<=prefix.size()||filename.compare(0,prefix.size(),prefix)||!SafeParents(full))return false;
        if(!s.RefreshInventory())return false;
        const auto observed=s.Observed(full.lexically_relative(s.root).wstring());
        if(!observed||observed->kind!=cache_files::ObservationKind::File||observed->links!=1||observed->deletePending||
           observed->identity.fileBytes||observed->allocationBytes||observed->streamLogicalBytes||observed->streamAllocationBytes)return false;
        const auto id=Physical(*observed);
        if(s.sourceDeductions.count(id))return false;
        if(record->second.temporary)return *record->second.temporary==id;
        for(const auto&other:s.reservations)if(other.first!=impl_->relative&&other.second.temporary&&*other.second.temporary==id)return false;
        record->second.temporary=id;return true;
    }catch(...){return false;}
}
bool WriteReservation::Commit() noexcept {
    try {
        if(!*this)return false;
        if(!impl_->managed || impl_->committed)return true;
        if(!impl_->owns || !impl_->guard || !*impl_->guard)return false;
        auto& s=*impl_->guard->s;
        if(s.saving || s.projectingOwners)return false;
        if(s.forceIndexReload)s.Load();
        auto record=s.reservations.find(impl_->relative);
        if(!s.healthy||s.forceIndexReload||record==s.reservations.end()||!record->second.temporary)return false;
        s.pendingPersistenceOwner=impl_->guard->owned;
        record->second.publicationPending=true;
        if(!s.RefreshInventory())return false;
        const auto observed=s.Observed(impl_->relative);
        if(!observed||observed->kind!=cache_files::ObservationKind::File||observed->links!=1||observed->deletePending||
           observed->identity.fileBytes>record->second.bytes||Charge(*observed)>record->second.budget||
           Physical(*observed)!=*record->second.temporary)return false;
        const auto f=Info(impl_->path);
        if(!f.exists || f.bytes>record->second.bytes || !Recognized(impl_->path))return false;
        const bool trackUse = impl_->useSerial == scene.serial && scene.acceptsUse();
        if (!s.Use(impl_->path,true,false,trackUse)) return false;
        const auto entry=s.entries.find(impl_->relative);
        if(entry==s.entries.end() || entry->second.bytes!=f.bytes || !s.Save(impl_->guard->source.get()))return false;
        s.reservations.erase(record);impl_->owns=false;impl_->committed=true;
        if(!s.UnregisteredPublication())s.pendingPersistenceOwner.reset();
        return true;
    } catch(...) {return false;}
}

namespace {
using AR=SourceAdmissionResult;
AR AdmissionMap(cache_catalog::Result result) noexcept {
    switch(result){
    case cache_catalog::Result::Ok:return AR::Ok;
    case cache_catalog::Result::Missing:return AR::Missing;
    case cache_catalog::Result::Invalid:return AR::Invalid;
    case cache_catalog::Result::Busy:return AR::Busy;
    case cache_catalog::Result::Limit:return AR::Limit;
    case cache_catalog::Result::Conflict:return AR::Conflict;
    case cache_catalog::Result::RecoveryRequired:return AR::RecoveryRequired;
    case cache_catalog::Result::Indeterminate:return AR::Indeterminate;
    default:return AR::Io;
    }
}
AR AdmissionMap(cache_business::SourceBudgetResult result) noexcept {
    switch(result){
    case cache_business::SourceBudgetResult::Ok:return AR::Ok;
    case cache_business::SourceBudgetResult::Absent:return AR::Missing;
    case cache_business::SourceBudgetResult::Invalid:return AR::Invalid;
    case cache_business::SourceBudgetResult::Busy:return AR::Busy;
    case cache_business::SourceBudgetResult::Limit:return AR::Limit;
    case cache_business::SourceBudgetResult::RecoveryRequired:return AR::RecoveryRequired;
    case cache_business::SourceBudgetResult::AllocationFailure:return AR::AllocationFailure;
    default:return AR::Io;
    }
}
AR ReadApprovalBindings(const cache_files::Directory& root,cache_catalog::PublicationApproval& approval) {
        using Area=cache_catalog::ApprovalArea;
        std::array<cache_files::Directory,static_cast<std::size_t>(Area::Count)> directories;
        const auto index=[](Area value){return static_cast<std::size_t>(value);};
        if(root.Share(directories[index(Area::Manager)])!=cache_files::Result::Ok)return AR::RecoveryRequired;
        const auto child=[&](Area parent,const wchar_t* name,Area target){
            return directories[index(parent)].Child(name,false,directories[index(target)])==cache_files::Result::Ok;
        };
        if(!child(Area::Manager,L"Video",Area::Video)||!child(Area::Video,L"control",Area::Control)||
           !child(Area::Control,L"catalog",Area::Catalog)||!child(Area::Video,L"pages",Area::Pages)||
           !child(Area::Video,L"raw",Area::Raw)||!child(Area::Catalog,L"snapshots",Area::Snapshots)||
           !child(Area::Catalog,L"commits",Area::Commits)||!child(Area::Catalog,L"operations",Area::Operations))return AR::RecoveryRequired;
        for(std::size_t i=0;i<directories.size();++i)
            if(directories[i].GetIdentity(approval.directories[i])!=cache_files::Result::Ok)return AR::RecoveryRequired;
        return AR::Ok;
    }
template<class F> AR AdmissionBoundary(F&& action) noexcept {
    try{return action();}catch(const std::bad_alloc&){return AR::AllocationFailure;}catch(...){return AR::Io;}
}
}
struct SourceAdmission::Impl {
    DWORD thread=GetCurrentThreadId();
    std::unique_ptr<Guard> guard;
    bool active=false,consumed=false;
    ~Impl(){if(active&&guard&&guard->source){
        if(consumed&&guard->s)guard->s->inventoryReady=false;
        guard->source->transitionActive=false;
    }}
    AR Ready() const noexcept {
        if(GetCurrentThreadId()!=thread)return AR::Thread;
        if(!active||!guard||!*guard)return AR::Invalid;
        return consumed?AR::Conflict:AR::Ok;
    }
    AR Store(std::unique_ptr<cache_catalog::Store>& output) {
        cache_files::Directory video,control,catalog;
        if(guard->source->root.Child(L"Video",false,video)!=cache_files::Result::Ok||
           video.Child(L"control",false,control)!=cache_files::Result::Ok||
           control.Child(L"catalog",false,catalog)!=cache_files::Result::Ok)return AR::RecoveryRequired;
        output=std::make_unique<cache_catalog::Store>(std::move(catalog));return AR::Ok;
    }
    AR ApprovalBindings(cache_catalog::PublicationApproval& approval) {
        return ReadApprovalBindings(guard->source->root,approval);
    }
    AR Approve(const cache_catalog::WritePlan& plan,std::uint64_t rawResidual,std::uint64_t futureMetadata,
               cache_catalog::PublicationApproval* receipt=nullptr,bool requireCurrentPolicy=false,
               std::uint64_t requiredMetadata=0) {
        auto& state=*guard->s;
        // A session can outlive a settings update. Read the durable policy at
        // approval, not just when opening the session. A later limit change
        // cannot revoke an already-approved responsibility, but an unreadable
        // current policy must not grant new work using stale settings.
        state.LoadSettings();if(!state.settingsReadable)return AR::Io;
        std::uint64_t peak=state.Total();
        const auto add=[&](std::uint64_t amount){if(amount==UINT64_MAX||amount>UINT64_MAX-peak)return false;peak+=amount;return true;};
        if(peak==UINT64_MAX||!add(state.LegacyReserved())||!add(rawResidual)||!add(futureMetadata))return AR::Limit;
        const auto& footprint=plan.Footprint();if(!plan.IsValid()||footprint.count>footprint.files.size())return AR::Invalid;
        for(std::uint32_t i=0;i<footprint.count;++i)if(!add(state.Round(footprint.files[i].maximumFileBytes)))return AR::Limit;
        if(requireCurrentPolicy){
            const auto requiredPeak=AddBytes(peak-futureMetadata,requiredMetadata);
            if(requiredPeak==UINT64_MAX||!state.settings.enabled||requiredPeak>state.settings.limitBytes)return AR::Limit;
        }
        const auto baseline=AddBytes(AddBytes(state.Total(),state.LegacyReserved()),
            AddBytes(state.sourceRawResidual,state.sourceMetadataCredit));
        if(baseline==UINT64_MAX)return AR::Limit;
        // Prepaid responsibility may be consumed without increasing its peak
        // even after the user lowers/disables new allocation. No new debt is
        // admitted merely because the old ledger already exceeds the limit.
        if(peak>baseline&&(!state.settings.enabled||peak>state.settings.limitBytes))return AR::Limit;
        if(receipt){
            cache_catalog::PublicationApproval candidate;
            const auto bound=ApprovalBindings(candidate);if(bound!=AR::Ok)return bound;
            candidate.allocationUnit=state.allocationUnit;candidate.physicalBytes=state.Total();
            candidate.legacyReservedBytes=state.LegacyReserved();
            candidate.beforeRawResidual=state.sourceRawResidual;candidate.beforeMetadata=state.sourceMetadataCredit;
            candidate.afterRawResidual=rawResidual;candidate.afterMetadata=futureMetadata;
            candidate.policyLimitBytes=state.settings.limitBytes;candidate.policyEnabled=state.settings.enabled;
            candidate.approvedPeak=peak;*receipt=candidate;
        }
        return AR::Ok;
    }
    AR Execute(cache_catalog::Store& store,cache_catalog::WritePlan& plan,cache_catalog::Snapshot& output,
               const cache_catalog::PublicationApproval* approval=nullptr) {
        const auto result=approval?store.ExecuteApproved(plan,*approval,output):store.Execute(plan,output);
        if(result==cache_catalog::Result::Ok||plan.IsConsumed()||plan.MayHaveWritten()){
            consumed=true;guard->s->inventoryReady=false;
        }
        return AdmissionMap(result);
    }
};
SourceAdmission::SourceAdmission(std::unique_ptr<Impl> value):impl_(std::move(value)){}
SourceAdmission::~SourceAdmission()=default;
SourceAdmissionResult SourceAdmission::Initialize(cache_catalog::Snapshot& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        Impl session;session.guard=std::make_unique<Guard>(true);auto& guard=*session.guard;
        if(!guard.s)return AR::Busy;
        if(!guard.source->root.IsValid()||!guard.source->allocationUnit)return AR::Missing;
        auto& state=*guard.s;
        struct Invalidate {State& state;~Invalidate(){state.inventoryReady=false;}} invalidate{state};
        if(!state.healthy||state.forceIndexReload||state.UnregisteredPublication())return AR::RecoveryRequired;
        // There is no readable source head yet. This private scope may inventory
        // the namespace, but cannot be shared with an ordinary writer.
        guard.source->zeroSourceObligations=true;
        cache_files::InventorySnapshot inventory;
        if(!state.RefreshInventory(&inventory))return AR::RecoveryRequired;
        const auto emptyLayout=[](const cache_files::InventorySnapshot& observed){
            const std::set<std::wstring> directories{L"video",L"video\\control",L"video\\pages",L"video\\raw",
                L"video\\control\\catalog",L"video\\control\\catalog\\snapshots",L"video\\control\\catalog\\commits",L"video\\control\\catalog\\operations"};
            for(const auto& item:observed.Entries()){
                const auto name=Lower(item.relativePath);
                if(name!=L"video"&&name.compare(0,6,L"video\\"))continue;
                const auto& actual=item.observed;
                if(actual.deletePending||(actual.attributes&FILE_ATTRIBUTE_REPARSE_POINT)||
                    actual.streamLogicalBytes||actual.streamAllocationBytes)return false;
                if(directories.count(name)){if(actual.kind!=cache_files::ObservationKind::Directory||actual.streamCount)return false;}
                else if(name==L"video\\control\\.lock"||name==L"video\\control\\catalog\\.lock"){
                    if(actual.kind!=cache_files::ObservationKind::File||actual.links!=1||actual.identity.fileBytes||actual.streamCount>1)return false;
                }else return false;
            }
            return true;
        };
        // Detect an existing authoritative head under its existing exclusion.
        // Missing locks never get repaired merely to inspect a populated store.
        cache_files::Directory video,control,catalog,pages,raw,snapshots,commits,operations;
        cache_files::FileLock controlLock;
        if(guard.source->root.Child(L"Video",false,video)==cache_files::Result::Ok&&
           video.Child(L"control",false,control)==cache_files::Result::Ok){
            const auto locked=control.LockExisting(0,controlLock);
            if(locked==cache_files::Result::Busy)return AR::Busy;
            if(locked==cache_files::Result::Ok&&control.Child(L"catalog",false,catalog)==cache_files::Result::Ok){
                cache_files::Directory bound;if(catalog.Share(bound)!=cache_files::Result::Ok)return AR::RecoveryRequired;
                cache_catalog::Store existing(std::move(bound));cache_catalog::Snapshot head;cache_files::ReadLease pin;
                const auto read=existing.ReadPinnedExistingLocks(head,pin);
                if(read==cache_catalog::Result::Ok)return AR::Conflict;
                if(read!=cache_catalog::Result::Missing)return AdmissionMap(read);
            }
        }
        if(!emptyLayout(inventory))return AR::RecoveryRequired;
        cache_catalog::WriteFootprint standby,initial;
        auto result=AdmissionMap(cache_catalog::MaximumApprovedCheckpointFootprint(standby));if(result!=AR::Ok)return result;
        const auto bytes=cache_business::source_wire::RootHeaderBytes+cache_business::ManifestHeaderBytes+
            cache_business::source_wire::RegistryHeaderBytes+cache_business::source_wire::DirectoryBytes;
        result=AdmissionMap(cache_catalog::ApprovedPublicationFootprint(bytes,initial));if(result!=AR::Ok)return result;
        std::uint64_t future=0,cost=0;
        for(std::uint32_t i=0;i<standby.count;++i)future=AddBytes(future,state.Round(standby.files[i].maximumFileBytes));
        for(std::uint32_t i=0;i<initial.count;++i)cost=AddBytes(cost,state.Round(initial.files[i].maximumFileBytes));
        state.LoadSettings();if(!state.settingsReadable)return AR::Io;
        const auto peak=AddBytes(AddBytes(AddBytes(state.Total(),state.LegacyReserved()),future),cost);
        if(peak==UINT64_MAX||!state.settings.enabled||peak>state.settings.limitBytes)return AR::Limit;
        // Release the inventory guards before creating descendants. Every Child
        // operation returns its own pinned actual directory; none is removed.
        inventory={};
        const auto child=[](const cache_files::Directory& parent,const wchar_t* name,cache_files::Directory& out){
            return out.IsValid()||parent.Child(name,true,out)==cache_files::Result::Ok;
        };
        if(!child(guard.source->root,L"Video",video)||!child(video,L"control",control))return AR::Io;
        if(!controlLock.IsValid()){
            const auto locked=control.Lock(0,controlLock);
            if(locked!=cache_files::Result::Ok)return locked==cache_files::Result::Busy?AR::Busy:AR::RecoveryRequired;
        }
        if(!child(video,L"pages",pages)||!child(video,L"raw",raw)||!child(control,L"catalog",catalog)||
           !child(catalog,L"snapshots",snapshots)||!child(catalog,L"commits",commits)||!child(catalog,L"operations",operations))return AR::Io;
        {cache_files::FileLock lock;const auto locked=catalog.Lock(0,lock);
            if(locked!=cache_files::Result::Ok)return locked==cache_files::Result::Busy?AR::Busy:AR::RecoveryRequired;}
        if(!state.RefreshInventory(&inventory)||!emptyLayout(inventory))return AR::RecoveryRequired;
        cache_files::DirectoryIdentity rawIdentity;
        if(raw.GetIdentity(rawIdentity)!=cache_files::Result::Ok)return AR::RecoveryRequired;
        cache_business::BusinessRoot root;
        if(!cache_files::crypto::Random(root.legacy.storeOwner)||
           std::all_of(root.legacy.storeOwner.begin(),root.legacy.storeOwner.end(),[](auto value){return value==0;}))return AR::Io;
        root.legacy.maintenanceEpoch=1;root.legacy.nextProjectId=1;root.legacy.liveEncodedBytes=cache_business::ManifestHeaderBytes;
        root.sources.storeOwner=root.legacy.storeOwner;root.sources.maintenanceEpoch=1;root.sources.maintenanceProtocol=2;
        root.sources.directories.push_back({1,cache_business::SourceArea::Video,rawIdentity});
        cache_business::Bytes encoded;
        if(cache_business::EncodeBusinessRoot(root,encoded)!=cache_business::Result::Ok||encoded.size()!=bytes)return AR::Invalid;
        cache_catalog::Store store(std::move(catalog));cache_catalog::WritePlan plan;
        result=AdmissionMap(store.PlanApprovedInitializeOwned(root.legacy.storeOwner,encoded,plan));if(result!=AR::Ok)return result;
        cache_catalog::PublicationApproval approval;
        result=session.Approve(plan,0,future,&approval,true,future);if(result!=AR::Ok)return result;
        // No stale inventory holds filesystem observation handles through body
        // publication; the fixed directory bindings and exclusion remain held.
        inventory={};
        return session.Execute(store,plan,output,&approval);
    });
}
SourceAdmissionResult SourceAdmission::Open(std::unique_ptr<SourceAdmission>& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        if(output)return AR::Busy;
        auto candidate=std::make_unique<Impl>();candidate->guard=std::make_unique<Guard>();auto& guard=*candidate->guard;
        if(!guard){
            if(guard.source&&guard.source->status!=cache_business::SourceBudgetResult::Ok&&guard.source->status!=cache_business::SourceBudgetResult::Absent)return AdmissionMap(guard.source->status);
            return guard.s?AR::RecoveryRequired:AR::Busy;
        }
        if(guard.source->status==cache_business::SourceBudgetResult::Absent)return AR::Missing;
        if(!guard.s->settingsReadable)return AR::Io;
        if(!guard.s->healthy||guard.s->forceIndexReload||guard.s->UnregisteredPublication())return AR::RecoveryRequired;
        // A parent legacy writer would retain stale source facts after a source
        // publication. Reject nesting instead of releasing/relocking its scope.
        if(guard.source.use_count()!=1)return AR::Busy;
        auto owner=std::unique_ptr<SourceAdmission>(new SourceAdmission(std::move(candidate)));
        owner->impl_->active=true;guard.source->transitionActive=true;output.swap(owner);return AR::Ok;
    });
}
const cache_business::SourceBudgetSnapshot& SourceAdmission::Current() const noexcept {
    static const cache_business::SourceBudgetSnapshot empty;
    return impl_&&impl_->Ready()==AR::Ok?impl_->guard->source->source:empty;
}
bool SourceAdmission::IsConsumed() const noexcept {return impl_&&impl_->consumed;}
SourceAdmissionResult SourceAdmission::Commit(const cache_catalog::Snapshot& expected,
    const cache_business::SourceRegistry& proposed,cache_catalog::Snapshot& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        if(!impl_)return AR::Invalid;auto result=impl_->Ready();if(result!=AR::Ok)return result;
        auto& guard=*impl_->guard;const auto& facts=guard.source->source;
        if(!expected.SameVersion(facts.Business().Source()))return AR::Conflict;
        cache_business::SourceCreditRequirement future;result=AdmissionMap(facts.Project(proposed,future));if(result!=AR::Ok)return result;
        cache_business::BusinessRoot next{facts.Business().Description(),proposed};
        // Upgrading credit is part of this same approved, recoverable commit,
        // not an implicit reinterpretation of an old on-disk registry.
        next.sources.maintenanceProtocol=2;cache_business::Bytes bytes;
        const auto encoded=cache_business::EncodeBusinessRoot(next,bytes);
        if(encoded!=cache_business::Result::Ok)return encoded==cache_business::Result::AllocationFailure?AR::AllocationFailure:encoded==cache_business::Result::Limit?AR::Limit:AR::Invalid;
        std::unique_ptr<cache_catalog::Store> store;result=impl_->Store(store);if(result!=AR::Ok)return result;
        cache_catalog::WritePlan plan;result=AdmissionMap(store->PlanApprovedCommit(facts.Business().Source(),bytes,plan));if(result!=AR::Ok)return result;
        cache_files::InventorySnapshot observed;if(!guard.s->RefreshInventory(&observed))return AR::RecoveryRequired;
        cache_business::RawSourceBudgetProjection projection;result=AdmissionMap(cache_business::ProjectRawSourceBudget(facts,proposed,observed,projection));if(result!=AR::Ok)return result;
        for(const auto& deduction:projection.deductions){const PhysicalId id{deduction.identity.volume,deduction.identity.fileIndex};
            for(const auto& reservation:guard.s->reservations)if(reservation.second.temporary&&*reservation.second.temporary==id)return AR::Conflict;
            if(guard.s->metadataTemporary&&*guard.s->metadataTemporary==id)return AR::Conflict;
        }
        cache_catalog::PublicationApproval approval;
        result=impl_->Approve(plan,projection.residualBytes,future.metadataBytes,&approval);if(result!=AR::Ok)return result;
        return impl_->Execute(*store,plan,output,&approval);
    });
}
SourceAdmissionResult SourceAdmission::Checkpoint(const cache_catalog::Snapshot& expected,cache_catalog::Snapshot& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        if(!impl_)return AR::Invalid;auto result=impl_->Ready();if(result!=AR::Ok)return result;
        auto& guard=*impl_->guard;const auto& facts=guard.source->source;
        if(!expected.SameVersion(facts.Business().Source()))return AR::Conflict;
        if(facts.Business().Source().IsCheckpoint())return AR::Conflict;
        // A full legacy chain cannot Commit an upgrade before maintenance.
        // Permit one approved checkpoint under CURRENT policy, preserving v1
        // payload and its funded credit. It cannot inherit the v2 paid allowance.
        const bool legacy=facts.Business().Sources().maintenanceProtocol==1;
        std::unique_ptr<cache_catalog::Store> store;result=impl_->Store(store);if(result!=AR::Ok)return result;
        cache_catalog::WritePlan plan;result=AdmissionMap(store->PlanApprovedCheckpoint(facts.Business().Source(),plan));if(result!=AR::Ok)return result;
        if(!guard.s->RefreshInventory())return AR::RecoveryRequired;
        const auto& credit=facts.Credit();if(credit.metadataBytes<credit.checkpointBytesPerStep||
            credit.fundedMetadataBytes<credit.fundedCheckpointBytesPerStep)return AR::RecoveryRequired;
        cache_catalog::PublicationApproval approval;
        result=impl_->Approve(plan,guard.s->sourceRawResidual,credit.fundedMetadataBytes-credit.fundedCheckpointBytesPerStep,
            &approval,legacy,credit.metadataBytes-credit.checkpointBytesPerStep);if(result!=AR::Ok)return result;
        return impl_->Execute(*store,plan,output,&approval);
    });
}
SourceAdmissionResult SourceAdmission::Collect(cache_catalog::CollectionReport& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        if(!impl_)return AR::Invalid;auto result=impl_->Ready();if(result!=AR::Ok)return result;
        if(!impl_->guard->source->source.Business().Source().IsCheckpoint())return AR::Conflict;
        std::unique_ptr<cache_catalog::Store> store;result=impl_->Store(store);if(result!=AR::Ok)return result;
        // Collection cannot create payload/metadata, and remains useful above a
        // lowered quota. Its report is not a refund; the next session inventories
        // actual surviving files before any new approval.
        impl_->consumed=true;impl_->guard->s->inventoryReady=false;
        return AdmissionMap(store->CollectRetired(impl_->guard->source->source.Business().Source(),output));
    });
}

struct SourceRecoveryAdmission::Impl {
    DWORD thread=GetCurrentThreadId();
    std::unique_ptr<Guard> guard;
    cache_files::Directory video,control;
    cache_files::FileLock controlLock;
    std::unique_ptr<cache_catalog::Store> store;
    cache_catalog::WritePlan plan;
    bool active=false,consumed=false;
    ~Impl(){
        // The shared State temporarily described a proposed endpoint. Even a
        // read-only session must not leave it available to ordinary writers.
        if(guard&&guard->s)guard->s->inventoryReady=false;
    }
    AR Ready() const noexcept {
        if(GetCurrentThreadId()!=thread)return AR::Thread;
        if(!active||!guard||!guard->s)return AR::Invalid;
        return consumed?AR::Conflict:AR::Ok;
    }
    AR VerifyApproval() const {
        const auto* approval=plan.RecoveryApproval();if(!approval)return AR::Ok;
        cache_catalog::PublicationApproval actual;
        auto result=ReadApprovalBindings(guard->source->root,actual);if(result!=AR::Ok)return result;
        for(std::size_t i=0;i<actual.directories.size();++i)
            if(actual.directories[i].volume!=approval->directories[i].volume||
               actual.directories[i].fileIndex!=approval->directories[i].fileIndex)return AR::RecoveryRequired;
        if(guard->source->allocationUnit!=approval->allocationUnit)return AR::RecoveryRequired;
        const bool rollback=plan.Recovery()==cache_catalog::RecoveryAction::Rollback;
        if(!rollback&&plan.Recovery()!=cache_catalog::RecoveryAction::ConfirmPublication)return AR::RecoveryRequired;
        if(plan.RecoveryIsInitialization()&&(approval->beforeRawResidual||approval->beforeMetadata))return AR::RecoveryRequired;
        if(guard->source->zeroSourceObligations&&(!plan.RecoveryIsInitialization()||!rollback||plan.RecoveryTarget().IsValid()))return AR::RecoveryRequired;
        // A matching raw file may now contain more of its already reserved
        // bytes. Inventory charges them physically; only that file's verified
        // default stream reduces the remaining promise. Never increase credit.
        const auto raw=rollback?approval->beforeRawResidual:approval->afterRawResidual;
        const auto metadata=rollback?approval->beforeMetadata:approval->afterMetadata;
        if(guard->s->sourceRawResidual>raw)return AR::RecoveryRequired;
        if(guard->s->sourceMetadataCredit!=metadata)return AR::RecoveryRequired;
        const auto& cost=plan.Footprint();
        if(cost.count>1||(!rollback&&cost.count))return AR::RecoveryRequired;
        if(cost.count){
            const auto bytes=cost.files[0].maximumFileBytes;
            if(cost.files[0].role!=cache_catalog::WriteRole::RollbackDecision)return AR::RecoveryRequired;
            if(plan.RecoveryIsCheckpoint()){
                // The private plan derived this size from the strictly decoded
                // exact checkpoint operation. Validate its bounded wire shape.
                const auto minimum=cache_files::HeaderBytes+cache_catalog::intent::ApprovedCheckpointRecordSize(1);
                const auto maximum=cache_files::HeaderBytes+cache_catalog::intent::MaximumApprovedCheckpointBytes;
                if(bytes<minimum||bytes>maximum||(bytes-minimum)%160)return AR::RecoveryRequired;
            }else if(bytes!=cache_files::HeaderBytes+cache_catalog::intent::ApprovedRecordBytes)return AR::RecoveryRequired;
        }
        return AR::Ok;
    }
};
SourceRecoveryAdmission::SourceRecoveryAdmission(std::unique_ptr<Impl> value):impl_(std::move(value)){}
SourceRecoveryAdmission::~SourceRecoveryAdmission()=default;
SourceAdmissionResult SourceRecoveryAdmission::Open(std::unique_ptr<SourceRecoveryAdmission>& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        if(output)return AR::Busy;
        auto candidate=std::make_unique<Impl>();candidate->guard=std::make_unique<Guard>(true);
        auto& guard=*candidate->guard;
        if(!guard.s)return AR::Busy;
        if(!guard.source->root.IsValid()||!guard.source->allocationUnit)return AR::Missing;
        if(!guard.s->healthy||guard.s->forceIndexReload||guard.s->UnregisteredPublication())return AR::RecoveryRequired;
        const auto child=guard.source->root.Child(L"Video",false,candidate->video);
        if(child==cache_files::Result::Missing)return AR::Missing;
        if(child!=cache_files::Result::Ok||candidate->video.Child(L"control",false,candidate->control)!=cache_files::Result::Ok)return AR::RecoveryRequired;
        const auto locked=candidate->control.LockExisting(0,candidate->controlLock);
        if(locked!=cache_files::Result::Ok)return locked==cache_files::Result::Busy?AR::Busy:AR::RecoveryRequired;
        cache_files::Directory catalog;
        if(candidate->control.Child(L"catalog",false,catalog)!=cache_files::Result::Ok)return AR::RecoveryRequired;
        candidate->store=std::make_unique<cache_catalog::Store>(std::move(catalog));
        auto result=AdmissionMap(candidate->store->PlanRecoveryExistingLocks(candidate->plan));if(result!=AR::Ok)return result;
        if(!candidate->plan.RecoveryTarget().IsValid()){
            const auto* approval=candidate->plan.RecoveryApproval();
            if(!candidate->plan.RecoveryIsInitialization()||candidate->plan.Recovery()!=cache_catalog::RecoveryAction::Rollback||
                !approval||approval->beforeRawResidual||approval->beforeMetadata)return AR::RecoveryRequired;
            // The catalog proved this is the first publication with no prior
            // root. Keep Target invalid, and charge all remaining actual files.
            guard.source->zeroSourceObligations=true;
        }else{
            guard.source->status=cache_business::ReadRecoverySourceBudget(guard.source->root,guard.source->allocationUnit,candidate->plan,guard.source->source);
            if(guard.source->status!=cache_business::SourceBudgetResult::Ok)return AdmissionMap(guard.source->status);
        }
        if(!guard.s->RefreshInventory())return AR::RecoveryRequired;
        result=candidate->VerifyApproval();if(result!=AR::Ok)return result;
        candidate->active=true;
        auto owner=std::unique_ptr<SourceRecoveryAdmission>(new SourceRecoveryAdmission(std::move(candidate)));
        output.swap(owner);return AR::Ok;
    });
}
const cache_business::SourceBudgetSnapshot& SourceRecoveryAdmission::Target() const noexcept {
    static const cache_business::SourceBudgetSnapshot empty;
    return impl_&&impl_->Ready()==AR::Ok?impl_->guard->source->source:empty;
}
cache_catalog::RecoveryAction SourceRecoveryAdmission::Action() const noexcept {
    return impl_&&impl_->Ready()==AR::Ok?impl_->plan.Recovery():cache_catalog::RecoveryAction::None;
}
bool SourceRecoveryAdmission::IsConsumed() const noexcept {return impl_&&impl_->consumed;}
SourceAdmissionResult SourceRecoveryAdmission::Execute(cache_catalog::RecoveryReport& output) noexcept {
    return AdmissionBoundary([&]()->AR{
        if(!impl_)return AR::Invalid;auto result=impl_->Ready();if(result!=AR::Ok)return result;
        auto& state=*impl_->guard->s;
        if(!state.RefreshInventory())return AR::RecoveryRequired;
        result=impl_->VerifyApproval();if(result!=AR::Ok)return result;
        std::uint64_t cost=0;const auto& footprint=impl_->plan.Footprint();
        if(footprint.count>footprint.files.size())return AR::Invalid;
        for(std::uint32_t i=0;i<footprint.count;++i)cost=AddBytes(cost,state.Round(footprint.files[i].maximumFileBytes));
        const auto peak=AddBytes(AddBytes(state.Total(),state.Reserved()),cost);
        if(peak==UINT64_MAX)return AR::Limit;
        if(!impl_->plan.RecoveryApproval()&&cost){
            // Old ordinary intents do not acquire a fabricated past approval.
            state.LoadSettings();if(!state.settingsReadable)return AR::Io;
            if(!state.settings.enabled||peak>state.settings.limitBytes)return AR::Limit;
        }
        // Approved intent already reserved this exact decision before candidate
        // writes. Restored pre-commit future credit and surviving candidates
        // coexist in the conservative ledger until rollback finishes; charging
        // the decision as a new ordinary publication would double-count it.
        // All unknown files remain charged, and no new work can nest here.
        result=AdmissionMap(impl_->store->ExecuteRecovery(impl_->plan,output));
        if(result==AR::Ok||impl_->plan.IsConsumed()||impl_->plan.MayHaveWritten()){
            impl_->consumed=true;state.inventoryReady=false;
        }
        return result;
    });
}
}
