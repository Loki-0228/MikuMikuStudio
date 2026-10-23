// MAINTENANCE: regression-keep. CPU-only tests; no user cache or application startup.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "mikumikustudio/cache_catalog.hpp"
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
namespace catalog=mikumikustudio::cache_catalog;
using Bytes=std::vector<std::uint8_t>;
using Result=catalog::Result;
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void NewDirectory(const fs::path& p){
    Check(p.is_absolute()&&p.native().size()>3,"test directory must be absolute non-root");
    Check(CreateDirectoryW(p.c_str(),nullptr)!=FALSE,"test directory must be newly created");
}
files::Directory Open(const fs::path& p,bool create=true){
    files::Directory d;Check(files::Directory::Open(p,create,d)==files::Result::Ok,"open owned file directory");return d;
}
bool Same(const catalog::Snapshot& a,const catalog::Snapshot& b){
    return a.IsValid()==b.IsValid()&&a.Revision()==b.Revision()&&a.StoreId()==b.StoreId()&&a.Bytes()==b.Bytes();
}
Bytes Single(std::uint8_t byte){return Bytes{byte};}
catalog::Snapshot Init(const fs::path& root,const Bytes& bytes=Bytes{0,0xff,0x80,3}){
    catalog::Snapshot s;
    Check(catalog::Store(root).Initialize(bytes,s)==Result::Ok,"initialize empty catalog");
    Check(s.IsValid()&&s.Revision()==1&&s.Bytes()==bytes&&s.StoreId()!=files::Owner{},"initial snapshot identity and content");
    return s;
}
fs::path Blob(const fs::path& root,const files::BlobInfo& info){return root/L"snapshots"/info.name;}
bool DeletionAllowed(const fs::path& p){
    HANDLE h=CreateFileW(p.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE){
        const DWORD error=GetLastError();
        Check(error==ERROR_SHARING_VIOLATION||error==ERROR_ACCESS_DENIED,"denied probe is a sharing or access denial");
        return false;
    }
    CloseHandle(h);return true;
}
void CorruptPayload(const fs::path& p){
    std::fstream f(p,std::ios::in|std::ios::out|std::ios::binary);
    Check(bool(f),"open owned publication for corruption fixture");
    f.seekp(130);
    const char byte=static_cast<char>(0xee);
    f.write(&byte,1);
    Check(bool(f),"write owned corruption fixture");
}
fs::path Upper(const fs::path& p){
    auto text=p.native();
    for(auto& c:text)c=static_cast<wchar_t>(std::towupper(c));
    return fs::path(text);
}
void IdentityAndShare(const fs::path& root){
    const auto path=root/L"identity";NewDirectory(path);
    auto dir=Open(path,false),same=Open(path,false);
    files::DirectoryIdentity a{},b{},shared{};
    Check(dir.GetIdentity(a)==files::Result::Ok&&a.fileIndex!=0,"read pinned directory identity");
    Check(same.GetIdentity(b)==files::Result::Ok&&a.volume==b.volume&&a.fileIndex==b.fileIndex,
        "reopened directory keeps physical identity");
    files::Directory copy;
    Check(dir.Share(copy)==files::Result::Ok&&copy.IsValid(),"share the pinned directory");
    Check(copy.GetIdentity(shared)==files::Result::Ok&&shared.volume==a.volume&&shared.fileIndex==a.fileIndex,
        "shared directory has the same identity");
    Check(dir.Share(dir)==files::Result::Ok,"self share is valid");
    files::Directory invalid;
    const auto kept=shared;
    Check(invalid.Share(copy)==files::Result::Invalid,"invalid directory cannot share");
    Check(copy.IsValid(),"failed share preserves an existing destination");
    Check(invalid.GetIdentity(shared)==files::Result::Invalid&&shared.volume==kept.volume&&shared.fileIndex==kept.fileIndex,
        "invalid directory preserves identity output");
    const auto childPath=path/L"child";NewDirectory(childPath);
    auto child=Open(childPath,false);
    files::DirectoryIdentity c{};
    Check(child.GetIdentity(c)==files::Result::Ok&&c.volume==a.volume&&c.fileIndex!=a.fileIndex,
        "child directory identity differs from its parent index");
    const auto upper=Upper(path);
    files::Directory upperDir;
    Check(files::Directory::Open(upper,false,upperDir)==files::Result::Ok,"case-insensitive spelling opens the same directory");
    files::DirectoryIdentity u{};
    Check(upperDir.GetIdentity(u)==files::Result::Ok&&u.volume==a.volume&&u.fileIndex==a.fileIndex,
        "spelling alias resolves to the same physical directory");
    files::Directory derived;
    {
        auto parent=Open(path,false);
        Check(parent.Child(L"child",false,derived)==files::Result::Ok&&derived.IsValid(),"open child through a pinned parent");
    }
    files::DirectoryIdentity d{};
    Check(derived.GetIdentity(d)==files::Result::Ok&&d.fileIndex==c.fileIndex,"child outlives its parent guards");
}
void BoundReads(const fs::path& root){
    const auto path=root/L"bound-reads";
    const Bytes firstBytes{1,2,3};
    {
        auto dir=Open(path);
        catalog::Store bound(std::move(dir));
        catalog::Snapshot first;
        Check(bound.Initialize(firstBytes,first)==Result::Ok&&first.Revision()==1,"initialize through a fixed directory binding");
        catalog::Snapshot readBack;
        Check(bound.Read(readBack)==Result::Ok&&Same(first,readBack),"binding reads its own publication");
        catalog::Snapshot pathRead;
        Check(catalog::Store(path).Read(pathRead)==Result::Ok&&Same(first,pathRead),"path store reads the binding publication");
        catalog::Store copy=bound;
        catalog::Snapshot copied;
        Check(copy.Read(copied)==Result::Ok&&Same(first,copied),"copied store shares the binding");
        catalog::Store moved(std::move(bound));
        catalog::Snapshot movedRead;
        Check(moved.Read(movedRead)==Result::Ok&&Same(first,movedRead),"moved binding keeps working");
        catalog::Snapshot invalidOut=movedRead;
        Check(bound.Read(invalidOut)==Result::Invalid&&Same(invalidOut,movedRead),"moved-from binding never falls back to a path");
        Check(bound.Initialize(Single(9),invalidOut)==Result::Invalid&&Same(invalidOut,movedRead),
            "moved-from binding cannot initialize");
        catalog::Snapshot commitOut;
        Check(bound.Commit(movedRead,Single(10),commitOut)==Result::Invalid&&!commitOut.IsValid(),
            "moved-from binding cannot commit");
        catalog::Snapshot copyRead;
        Check(copy.Read(copyRead)==Result::Ok&&copyRead.Revision()==1,"copy survives move of the original");
        catalog::Snapshot committed;
        Check(copy.Commit(copyRead,Single(5),committed)==Result::Ok&&committed.Revision()==2,
            "copy commits after the original was moved from");
        files::Directory invalid;
        catalog::Store broken(std::move(invalid));
        catalog::Snapshot sentinel=committed,output=committed;
        Check(broken.Read(sentinel)==Result::Invalid&&Same(sentinel,committed),"invalid binding reports invalid read");
        Check(broken.Initialize(Single(7),sentinel)==Result::Invalid&&Same(sentinel,committed),
            "invalid binding cannot initialize");
        Check(broken.Commit(committed,Single(8),output)==Result::Invalid&&Same(output,committed),
            "invalid binding cannot commit");
    }
    catalog::Snapshot final;
    Check(catalog::Store(path).Read(final)==Result::Ok&&final.Revision()==2&&final.Bytes()==Single(5),
        "path store observes the commit made through a binding copy");
}
void OriginChecks(const fs::path& root){
    const auto path=root/L"origin";NewDirectory(path);
    const auto upper=Upper(path);
    files::DirectoryIdentity lowerIdentity{};
    {auto d=Open(path,false);Check(d.GetIdentity(lowerIdentity)==files::Result::Ok,"capture origin directory identity");}
    const catalog::Snapshot first=Init(path,Single(0x10));
    catalog::Store pathLower(path),pathUpper(upper);
    catalog::Snapshot upperRead;
    Check(pathUpper.Read(upperRead)==Result::Ok&&Same(first,upperRead),"case variant path store reads the same catalog");
    catalog::Snapshot sentinel=upperRead,output=upperRead;
    Check(pathUpper.Commit(first,Single(0x11),output)==Result::Conflict&&Same(output,sentinel),
        "two path spellings require identical spelling");
    Check(pathLower.Commit(upperRead,Single(0x12),output)==Result::Conflict&&Same(output,sentinel),
        "path spelling check is symmetric");
    auto lowerDir=Open(path,false),upperDir=Open(upper,false);
    files::DirectoryIdentity upperIdentity{};
    Check(upperDir.GetIdentity(upperIdentity)==files::Result::Ok&&upperIdentity.volume==lowerIdentity.volume&&
        upperIdentity.fileIndex==lowerIdentity.fileIndex,"case variant binding resolves the same physical root");
    catalog::Store boundLower(std::move(lowerDir)),boundUpper(std::move(upperDir));
    catalog::Snapshot boundRead;
    Check(boundLower.Read(boundRead)==Result::Ok&&Same(first,boundRead),"binding reads the path store publication");
    catalog::Snapshot viaBound;
    Check(boundUpper.Commit(first,Single(0x20),viaBound)==Result::Ok&&viaBound.Revision()==2,
        "binding accepts a path-origin snapshot at the same physical root");
    catalog::Snapshot pathNow;
    Check(pathLower.Read(pathNow)==Result::Ok&&pathNow.Revision()==2&&pathNow.Bytes()==Single(0x20),
        "binding commit is visible through a path store");
    catalog::Snapshot originBound;
    Check(boundLower.Read(originBound)==Result::Ok&&originBound.Revision()==2,"binding origin re-reads the current revision");
    catalog::Snapshot viaPath;
    Check(pathUpper.Commit(originBound,Single(0x21),viaPath)==Result::Ok&&viaPath.Revision()==3,
        "path store accepts a bound origin by physical identity");
    catalog::Snapshot copy=viaPath;
    Check(boundLower.Commit(copy,Single(0x22),viaPath)==Result::Ok&&viaPath.Revision()==4,
        "copied snapshot retains binding origin");
    const catalog::Snapshot foreign=Init(root/L"origin-foreign",Single(0x30));
    catalog::Snapshot foreignSentinel=foreign;
    Check(boundLower.Commit(foreign,Single(0x31),foreignSentinel)==Result::Conflict&&Same(foreignSentinel,foreign),
        "binding rejects a snapshot from another physical catalog");
    catalog::Snapshot checkpointOut=originBound;
    Check(boundLower.Checkpoint(originBound,checkpointOut)==Result::Conflict&&Same(checkpointOut,originBound),
        "stale binding origin cannot checkpoint a newer revision");
    catalog::Snapshot current,pathCurrent;
    Check(boundLower.Read(current)==Result::Ok&&pathLower.Read(pathCurrent)==Result::Ok&&Same(current,pathCurrent),
        "binding and path origins read the same current revision");
    catalog::Snapshot checkpoint;
    Check(boundLower.Checkpoint(current,checkpoint)==Result::Ok&&checkpoint.Revision()==5,
        "checkpoint through a binding accepts the bound origin");
    catalog::Snapshot afterCheckpoint;
    Check(boundLower.Read(afterCheckpoint)==Result::Ok&&Same(afterCheckpoint,checkpoint),"binding re-reads its checkpoint");
    Check(pathUpper.Checkpoint(current,checkpointOut)==Result::Conflict&&Same(checkpointOut,originBound),
        "path spelling still governs checkpoint origins");
    Check(boundLower.Checkpoint(foreign,checkpointOut)==Result::Conflict&&Same(checkpointOut,originBound),
        "checkpoint rejects a snapshot from another physical catalog");
}
void PinnedLease(const fs::path& root){
    const auto path=root/L"pinned";
    catalog::Store store(path);
    const Bytes firstBytes{0x41},secondBytes{0x42};
    const catalog::Snapshot first=Init(path,firstBytes);
    catalog::Snapshot pinnedSnapshot;
    files::ReadLease lease;
    Check(store.ReadPinned(pinnedSnapshot,lease)==Result::Ok&&Same(first,pinnedSnapshot)&&lease.IsValid(),
        "pinned read returns the unchanged snapshot and a lease");
    Check(lease.Info().spec.kind==files::Kind::CatalogSnapshot&&lease.Info().spec.owner==first.StoreId(),
        "pinned lease identifies the current snapshot envelope");
    const auto pinnedBlob=Blob(path,lease.Info());
    Check(fs::exists(pinnedBlob),"pinned snapshot file exists");
    Check(!DeletionAllowed(pinnedBlob),"pinned snapshot cannot be deleted");
    Bytes payload;
    Check(lease.ReadAll(payload)==files::Result::Ok&&payload==first.Bytes(),"pinned lease reads the verified payload");
    catalog::Store missing(root/L"pinned-missing");
    catalog::Snapshot missingOut=pinnedSnapshot;
    files::ReadLease survivor=std::move(lease);
    Check(missing.ReadPinned(missingOut,survivor)==Result::Missing&&Same(missingOut,pinnedSnapshot)&&survivor.IsValid(),
        "missing store preserves both pinned outputs");
    Check(!fs::exists(root/L"pinned-missing"),"failed pinned read never initializes the store");
    const auto emptyPath=root/L"pinned-empty";NewDirectory(emptyPath);
    catalog::Snapshot emptyOut=pinnedSnapshot;
    Check(catalog::Store(emptyPath).ReadPinned(emptyOut,survivor)==Result::Missing&&Same(emptyOut,pinnedSnapshot),
        "empty directory declines a pinned read");
    auto lockDir=Open(path,false);
    files::FileLock held;
    Check(lockDir.Lock(0,held)==files::Result::Ok,"hold the catalog lock externally");
    catalog::Snapshot busyOut=pinnedSnapshot;
    Check(store.ReadPinned(busyOut,survivor)==Result::Busy&&Same(busyOut,pinnedSnapshot)&&survivor.IsValid(),
        "busy catalog preserves both pinned outputs");
    held={};
    catalog::Snapshot live;
    Check(store.ReadPinned(live,survivor)==Result::Ok&&Same(live,pinnedSnapshot),
        "catalog lock release restores pinned reads and reuses the lease");
    {
        catalog::Store temporary(path);
        catalog::Snapshot tempSnapshot;
        files::ReadLease tempLease;
        Check(temporary.ReadPinned(tempSnapshot,tempLease)==Result::Ok&&Same(tempSnapshot,pinnedSnapshot),
            "pinned read through another store instance");
    }
    catalog::Snapshot second,third;
    Check(store.Commit(first,secondBytes,second)==Result::Ok&&second.Revision()==2,
        "commit while the previous snapshot is pinned");
    Check(store.Checkpoint(second,third)==Result::Ok&&third.Revision()==3,
        "checkpoint while the previous snapshot is pinned");
    catalog::CollectionReport report;
    Check(store.CollectRetired(report)==Result::Ok&&report.attempted==3&&report.busy==1&&report.absent==2&&
        report.failed==0&&report.pending==0,"pinned retired snapshot blocks collection while other retired metadata is removed");
    Check(fs::exists(pinnedBlob),"blocked retired snapshot remains present");
    catalog::Snapshot current;
    Check(store.ReadPinned(current,survivor)==Result::Ok&&current.Revision()==3&&Same(current,third),
        "re-pinned read replaces the old lease with the current snapshot");
    Check(!DeletionAllowed(Blob(path,survivor.Info())),"replacement lease pins the current snapshot");
    Check(store.CollectRetired(report)==Result::Ok&&report.attempted==1&&report.absent==1&&report.busy==0,
        "released retired snapshot is collected");
    Check(!fs::exists(pinnedBlob),"collected snapshot is absent from the namespace");
    const auto corruptPath=root/L"pinned-corrupt";
    Init(corruptPath,Bytes{0x51,0x52,0x53,0x54});
    catalog::Store corrupt(corruptPath);
    catalog::Snapshot good;
    files::ReadLease goodLease;
    Check(corrupt.ReadPinned(good,goodLease)==Result::Ok,"capture corrupt-store fixture snapshot");
    const auto corruptBlob=Blob(corruptPath,goodLease.Info());
    goodLease={};
    CorruptPayload(corruptBlob);
    catalog::Snapshot safe=third;
    Check(corrupt.ReadPinned(safe,survivor)==Result::RecoveryRequired&&Same(safe,third)&&survivor.IsValid(),
        "corrupt published snapshot preserves both pinned outputs");
    Bytes still;
    Check(survivor.ReadAll(still)==files::Result::Ok&&still==second.Bytes(),
        "failed pinned read leaves the previous lease state readable");
    catalog::Snapshot again;
    files::ReadLease secondLease;
    Check(store.ReadPinned(again,secondLease)==Result::Ok&&Same(again,current)&&secondLease.IsValid(),
        "two live leases can pin the same current snapshot");
    Check(!DeletionAllowed(Blob(path,secondLease.Info())),"either live lease blocks deletion");
}
void BoundPinned(const fs::path& root){
    const auto path=root/L"bound-pinned";
    auto dir=Open(path);
    catalog::Store bound(std::move(dir));
    catalog::Snapshot first;
    Check(bound.Initialize(Single(0x61),first)==Result::Ok&&first.Revision()==1,"initialize bound store for pinned read");
    catalog::Snapshot pinned;
    files::ReadLease lease;
    Check(bound.ReadPinned(pinned,lease)==Result::Ok&&lease.IsValid(),"pinned read through a fixed directory binding");
    const auto pinnedBlob=Blob(path,lease.Info());
    Check(!DeletionAllowed(pinnedBlob),"binding-pinned snapshot cannot be deleted");
    catalog::Snapshot second,third;
    Check(bound.Commit(pinned,Single(0x62),second)==Result::Ok&&second.Revision()==2,"bound commit while pinned");
    Check(bound.Checkpoint(second,third)==Result::Ok&&third.Revision()==3,"bound checkpoint while pinned");
    catalog::CollectionReport report;
    Check(bound.CollectRetired(report)==Result::Ok&&report.attempted==3&&report.busy==1&&report.absent==2,
        "binding pin blocks collection of its retired snapshot");
    lease={};
    Check(bound.CollectRetired(report)==Result::Ok&&report.attempted==1&&report.absent==1&&report.busy==0,
        "releasing the binding pin permits collection");
    catalog::Snapshot pathRead;
    Check(catalog::Store(path).Read(pathRead)==Result::Ok&&Same(pathRead,third),"path store observes the bound checkpoint");
}
void SnapshotCopies(const fs::path& root){
    const auto path=root/L"snapshot-copies";NewDirectory(path);
    const auto first=Init(path,Single(0x71));
    catalog::Snapshot copy=first;
    Check(Same(copy,first),"snapshot copy preserves published fields");
    catalog::Snapshot committed;
    Check(catalog::Store(path).Commit(copy,Single(0x72),committed)==Result::Ok&&committed.Revision()==2,
        "snapshot copy retains the private CAS origin for commit");
    catalog::Snapshot checkpointed;
    Check(catalog::Store(path).Checkpoint(committed,checkpointed)==Result::Ok&&checkpointed.Revision()==3,
        "committed output retains the private CAS origin for checkpoint");
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Check(argc==3&&std::wcscmp(argv[1],L"--store")==0,"usage: --store NEW_ABSOLUTE_DIR");
        const fs::path root(argv[2]);NewDirectory(root);
        IdentityAndShare(root);BoundReads(root);OriginChecks(root);PinnedLease(root);BoundPinned(root);SnapshotCopies(root);
        {std::ofstream result(root/L"result.txt");result<<"checks="<<checks<<"\n";}
        std::printf("PASS cache catalog pinned checks=%u evidence=%ls\n",checks,root.c_str());return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL cache catalog pinned checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL cache catalog pinned unexpected exception\n");return 2;}
}
