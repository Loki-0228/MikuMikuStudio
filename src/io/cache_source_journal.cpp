#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "cache_source_journal.hpp"
#include "cache_business_reader.hpp"
#include "mikumikustudio/cache_catalog.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace mikumikustudio::cache_business {
namespace {
namespace files=cache_files;namespace catalog=cache_catalog;
using J=JournalResult;using C=CaptureState;
bool Same(const files::DirectoryIdentity&a,const files::DirectoryIdentity&b)noexcept{return a.volume==b.volume&&a.fileIndex==b.fileIndex;}
bool Same(const files::Identity&a,const files::Identity&b)noexcept{return a.volume==b.volume&&a.fileIndex==b.fileIndex&&a.fileBytes==b.fileBytes;}
bool Same(const files::RawSourcePlan&a,const files::RawSourcePlan&b)noexcept{return a.name==b.name&&a.sourceBytes==b.sourceBytes&&Same(a.createdIdentity,b.createdIdentity);}
bool Same(const files::RawSourceInfo&a,const files::RawSourceInfo&b)noexcept{return a.name==b.name&&a.payloadBytes==b.payloadBytes&&a.payloadDigest==b.payloadDigest&&Same(a.identity,b.identity);}
bool Same(const SourceOperation&a,const SourceOperation&b)noexcept{return a.id==b.id&&a.sourceId==b.sourceId&&a.directoryId==b.directoryId&&a.stage==b.stage&&a.reservedBytes==b.reservedBytes&&Same(a.plan,b.plan)&&Same(a.captured,b.captured);}
bool Same(const SourceRecord&a,const SourceRecord&b)noexcept{return a.id==b.id&&a.createdBy==b.createdBy&&a.directoryId==b.directoryId&&a.state==b.state&&a.lastUse==b.lastUse&&a.owners==b.owners&&Same(a.file,b.file);}
template<class T>bool Same(const std::optional<T>&a,const std::optional<T>&b)noexcept{return bool(a)==bool(b)&&(!a||Same(*a,*b));}
template<class T>bool Nonzero(const T&a)noexcept{return std::any_of(a.begin(),a.end(),[](auto c){return c!=0;});}
J Map(files::Result r)noexcept{switch(r){case files::Result::Ok:return J::Ok;case files::Result::Missing:return J::Missing;case files::Result::Busy:return J::Busy;case files::Result::Invalid:return J::Invalid;case files::Result::Conflict:return J::Conflict;case files::Result::Limit:return J::Limit;case files::Result::Cancelled:return J::Cancelled;case files::Result::CleanupRequired:return J::NeedsCleanup;case files::Result::Corrupt:return J::RecoveryRequired;default:return J::Io;}}
J Map(catalog::Result r)noexcept{switch(r){case catalog::Result::Ok:return J::Ok;case catalog::Result::Missing:return J::Missing;case catalog::Result::Invalid:return J::Invalid;case catalog::Result::Busy:return J::Busy;case catalog::Result::Conflict:return J::Conflict;case catalog::Result::Limit:return J::Limit;case catalog::Result::RecoveryRequired:return J::RecoveryRequired;case catalog::Result::Indeterminate:return J::Indeterminate;default:return J::Io;}}
J Map(Result r)noexcept{switch(r){case Result::Ok:return J::Ok;case Result::Limit:return J::Limit;case Result::AllocationFailure:return J::AllocationFailure;case Result::Unsupported:return J::RecoveryRequired;default:return J::Invalid;}}
J Map(ReadResult r)noexcept{switch(r){case ReadResult::Ok:return J::Ok;case ReadResult::Busy:return J::Busy;case ReadResult::Limit:return J::Limit;case ReadResult::AllocationFailure:return J::AllocationFailure;case ReadResult::Io:return J::Io;default:return J::RecoveryRequired;}}
template<class F>J Boundary(F&&f)noexcept{try{return f();}catch(const std::bad_alloc&){return J::AllocationFailure;}catch(...){return J::Io;}}
auto FindOp(SourceRegistry&s,OperationId id){return std::find_if(s.operations.begin(),s.operations.end(),[&](const auto&v){return v.id==id;});}
auto FindSource(SourceRegistry&s,SourceId id){return std::find_if(s.sources.begin(),s.sources.end(),[&](const auto&v){return v.id==id;});}
std::optional<SourceOperation> Op(const SourceRegistry&s,OperationId id){for(const auto&v:s.operations)if(v.id==id)return v;return {};}
std::optional<SourceRecord> Source(const SourceRegistry&s,SourceId id){for(const auto&v:s.sources)if(v.id==id)return v;return {};}
struct Attempt {
    std::optional<SourceOperation> beforeOp,afterOp;
    std::optional<SourceRecord> beforeSource,afterSource;
    C afterState=C::New;
};
struct RetirementAttempt {SourceRecord before,after;};
struct View {catalog::Snapshot snapshot;files::ReadLease manifest;PublishedIndex index;BusinessRoot root;};
}
struct SourceCaptureState {
    OperationId id{};SourceId source{};std::uint64_t declared=0;
    C state=C::New;bool external=true,unresolved=false;
    std::optional<SourceOperation> confirmed;
    std::optional<SourceRecord> published;
    std::unique_ptr<Attempt> attempt;
    files::RawSourceWrite write;files::RawSourceLease lease;files::RawSourceCleanup cleanup;
    files::RawSourcePlan plan;files::RawSourceInfo info;
    std::wstring unrecordedName;
};
struct SourceJournalState {
    files::Directory control,pages,raw;
    files::DirectoryIdentity controlId{},pagesId{},rawId{},catalogId{};
    std::unique_ptr<catalog::Store> catalog;
    files::Owner owner{};std::uint64_t epoch=0,directoryId=0;
    DWORD thread=GetCurrentThreadId();bool inCall=false;
    std::map<OperationId,std::shared_ptr<SourceCaptureState>> captures;
    std::map<SourceId,RetirementAttempt> retirements;
    std::weak_ptr<SourceJournalState> self;
    std::shared_ptr<SourceJournalState> keepAlive;
};
namespace {
using BindingKey=std::pair<std::uint32_t,std::uint64_t>;
std::mutex bindingsMutex;
std::map<BindingKey,std::weak_ptr<SourceJournalState>> bindings;
struct Call {
    SourceJournalState& s;bool entered=false;
    explicit Call(SourceJournalState&v):s(v){if(GetCurrentThreadId()==s.thread&&!s.inCall){s.inCall=true;entered=true;}}
    ~Call(){if(entered)s.inCall=false;}
    J Result()const noexcept{return GetCurrentThreadId()!=s.thread?J::Thread:entered?J::Ok:J::Busy;}
};
bool Resources(const SourceCaptureState&c)noexcept{return c.write.IsValid()||c.lease.IsValid()||c.cleanup.IsValid()||!c.unrecordedName.empty();}
void Trim(SourceJournalState&s)noexcept{if(!s.retirements.empty())return;for(const auto&v:s.captures)if(Resources(*v.second)||v.second->unresolved)return;s.keepAlive.reset();}
void EraseCapture(SourceJournalState&s,const std::shared_ptr<SourceCaptureState>&c)noexcept{
    const auto it=s.captures.find(c->id);
    if(it!=s.captures.end()&&it->second==c)s.captures.erase(it);
}
void Finish(SourceJournalState&s,const std::shared_ptr<SourceCaptureState>&c)noexcept{EraseCapture(s,c);Trim(s);}
J Lock(SourceJournalState&s,files::FileLock&lock){return Map(s.control.Lock(0,lock));}
J Load(SourceJournalState&s,View&v,bool recover=false){
    auto r=s.catalog->ReadPinned(v.snapshot,v.manifest);
    if(r==catalog::Result::RecoveryRequired&&recover){catalog::RecoveryReport report;const auto fixed=s.catalog->RecoverOwned(report);if(fixed!=catalog::Result::Ok)return Map(fixed);r=s.catalog->ReadPinned(v.snapshot,v.manifest);}
    if(r!=catalog::Result::Ok)return Map(r);
    const auto decoded=DecodeBusinessRoot(v.snapshot.Bytes().data(),v.snapshot.Bytes().size(),v.root);if(decoded!=Result::Ok)return Map(decoded);
    if(v.snapshot.StoreId()!=v.root.legacy.storeOwner)return J::RecoveryRequired;
    const auto read=LoadPublishedIndex(v.snapshot,s.pages,v.index);if(read!=ReadResult::Ok)return Map(read);
    const auto& source=v.root.sources;
    if(source.directories.size()!=1||source.directories[0].area!=SourceArea::Video||!Same(source.directories[0].identity,s.rawId))return J::Conflict;
    if(Nonzero(s.owner)&&(source.storeOwner!=s.owner||source.maintenanceEpoch!=s.epoch||source.directories[0].id!=s.directoryId))return J::Conflict;
    return J::Ok;
}
J VerifyFootprint(SourceCaptureState&c,const View&v){
    const auto operation=Op(v.root.sources,c.id);
    const auto source=Source(v.root.sources,c.source);
    if(!Same(operation,c.confirmed)||!Same(source,c.published))return J::Conflict;
    return J::Ok;
}
void ApplyAttempt(SourceCaptureState&c)noexcept{
    c.confirmed=std::move(c.attempt->afterOp);c.published=std::move(c.attempt->afterSource);
    c.state=c.attempt->afterState;c.unresolved=false;c.attempt.reset();
}
J Commit(SourceJournalState&s,SourceCaptureState&c,const View&v,BusinessRoot&next,C state){
    const auto cross=ValidateSourcesAgainstIndex(next.sources,v.index.Value());if(cross!=Result::Ok)return Map(cross);
    Bytes bytes;const auto encoded=EncodeBusinessRoot(next,bytes);if(encoded!=Result::Ok)return Map(encoded);
    auto attempt=std::make_unique<Attempt>();attempt->beforeOp=Op(v.root.sources,c.id);attempt->beforeSource=Source(v.root.sources,c.source);
    attempt->afterOp=Op(next.sources,c.id);attempt->afterSource=Source(next.sources,c.source);attempt->afterState=state;
    c.attempt=std::move(attempt);catalog::Snapshot result;
    const auto published=s.catalog->Commit(v.snapshot,bytes,result);
    if(published==catalog::Result::Ok){ApplyAttempt(c);return J::Ok;}
    if(published==catalog::Result::Indeterminate){c.unresolved=true;s.keepAlive=s.self.lock();return J::Indeterminate;}
    c.attempt.reset();return Map(published);
}
J ResolveImpl(SourceJournalState&s,SourceCaptureState&c){
    files::FileLock lock;auto result=Lock(s,lock);if(result!=J::Ok)return result;View v;result=Load(s,v,true);if(result!=J::Ok)return result;
    if(!c.unresolved)return VerifyFootprint(c,v);
    if(!c.attempt)return J::RecoveryRequired;
    const auto operation=Op(v.root.sources,c.id);
    const auto source=Source(v.root.sources,c.source);
    if(Same(operation,c.attempt->afterOp)&&Same(source,c.attempt->afterSource)){ApplyAttempt(c);return J::Ok;}
    if(Same(operation,c.attempt->beforeOp)&&Same(source,c.attempt->beforeSource)){c.unresolved=false;c.attempt.reset();return J::Conflict;}
    return J::Conflict;
}
J MutateOperation(SourceJournalState&s,SourceCaptureState&c,const View&v,const SourceOperation&op,C state){
    BusinessRoot next=v.root;auto it=FindOp(next.sources,c.id);if(it==next.sources.operations.end())return J::Conflict;*it=op;return Commit(s,c,v,next,state);
}
J ConfirmAbsent(SourceJournalState&s,const std::wstring&name){
    std::vector<std::wstring> names;const auto listed=s.raw.EnumerateAll(65536,names);if(listed!=files::Result::Ok)return Map(listed);
    for(const auto&entry:names){
        const auto comparison=CompareStringOrdinal(entry.c_str(),-1,name.c_str(),-1,TRUE);
        if(!comparison)return J::Io;
        if(comparison==CSTR_EQUAL)return J::NeedsCleanup;
    }
    return J::Ok;
}
J CheckReservedRecoveryNames(SourceJournalState&s,const SourceRegistry&registry){
    // A crash before Prepared publication can leave an unrecorded generation.
    // Names only exclude known responsibilities; this is not ownership proof,
    // permission to delete, or an accounting/physical-size scan.
    std::set<std::wstring> known;
    for(const auto&source:registry.sources)
        if(source.directoryId==s.directoryId)known.insert(source.file.name);
    for(const auto&operation:registry.operations)
        if(operation.directoryId==s.directoryId&&operation.stage!=SourceStage::Reserved)
            known.insert(operation.plan.name);
    std::vector<std::wstring> names;
    const auto listed=s.raw.EnumerateAll(65536,names);
    if(listed!=files::Result::Ok)return Map(listed);
    for(const auto&name:names)if(!known.count(name))return J::NeedsCleanup;
    return J::Ok;
}
J GetCleanup(SourceJournalState&s,SourceCaptureState&c,const SourceOperation&op){
    if(c.cleanup.IsValid()||c.write.IsValid())return J::Ok;
    c.lease={};
    const auto r=(op.stage==SourceStage::Captured||op.stage==SourceStage::AbortCaptured)?
        s.raw.RecoverRawSourceCleanup(s.rawId,op.captured,c.cleanup):s.raw.RecoverRawSourceCleanup(s.rawId,op.plan,c.cleanup);
    return Map(r);
}
J CancelImpl(const std::shared_ptr<SourceJournalState>&s,const std::shared_ptr<SourceCaptureState>&c,bool recovery){
    if(c->unresolved)return J::Indeterminate;
    if(c->state==C::Published)return J::NotReady;
    if(c->state==C::Cancelled){Finish(*s,c);return J::Ok;}
    if(c->state==C::New){c->state=C::Cancelled;Finish(*s,c);return J::Ok;}
    files::FileLock lock;auto result=Lock(*s,lock);if(result!=J::Ok)return result;View v;result=Load(*s,v);if(result!=J::Ok)return result;
    result=VerifyFootprint(*c,v);if(result!=J::Ok)return result;
    if(!c->confirmed)return J::Conflict;
    auto op=*c->confirmed;
    if(recovery&&op.stage==SourceStage::Reserved&&!Resources(*c)){
        result=CheckReservedRecoveryNames(*s,v.root.sources);
        if(result!=J::Ok)return result;
    }
    // A locally created file may not yet have a Prepared record. Preserve its
    // exact plan before either closing a handle or publishing cleanup intent.
    if(op.stage==SourceStage::Reserved&&c->write.IsValid()){
        op.plan=c->write.Plan();op.reservedBytes=op.plan.sourceBytes;op.stage=SourceStage::AbortPrepared;
        result=MutateOperation(*s,*c,v,op,C::Prepared);if(result!=J::Ok)return result;
        v=View{};result=Load(*s,v);if(result!=J::Ok)return result;
    }
    if(op.stage!=SourceStage::Reserved){
        const bool aborting=op.stage==SourceStage::AbortPrepared||op.stage==SourceStage::AbortCaptured;
        bool missing=false;
        if(recovery){result=GetCleanup(*s,*c,op);missing=result==J::Missing;if(result!=J::Ok&&!missing)return result;}
        if(!aborting){op.stage=op.stage==SourceStage::Captured?SourceStage::AbortCaptured:SourceStage::AbortPrepared;
            result=MutateOperation(*s,*c,v,op,c->state);if(result!=J::Ok)return result;
            v=View{};result=Load(*s,v);if(result!=J::Ok)return result;
        }
        if(!recovery){result=GetCleanup(*s,*c,op);missing=result==J::Missing;if(result!=J::Ok&&!missing)return result;}
        if(!missing){const auto removed=c->write.IsValid()?c->write.Abort():c->cleanup.Abort();if(removed!=files::Result::DeletePending)return Map(removed);}
        result=ConfirmAbsent(*s,op.plan.name);if(result!=J::Ok)return result;
    }else {
        // An identity-query + cleanup double failure has no trustworthy plan.
        // Only the original CREATE_NEW receipt permits deletion. Keep its name
        // for absence confirmation, never use it to reopen/delete an unknown.
        if(c->cleanup.IsValid()){
            if(c->unrecordedName.empty())c->unrecordedName=c->cleanup.Name();
            const auto removed=c->cleanup.Abort();if(removed!=files::Result::DeletePending)return Map(removed);
        }
        if(!c->unrecordedName.empty()){result=ConfirmAbsent(*s,c->unrecordedName);if(result!=J::Ok)return result;c->unrecordedName.clear();}
    }
    BusinessRoot next=v.root;const auto it=FindOp(next.sources,c->id);if(it==next.sources.operations.end())return J::Conflict;
    next.sources.operations.erase(it);result=Commit(*s,*c,v,next,C::Cancelled);
    if(result==J::Ok)Finish(*s,c);return result;
}
}

SourceJournal::SourceJournal(std::shared_ptr<SourceJournalState>s):state_(std::move(s)){}
SourceJournal::~SourceJournal()=default;
SourceCapture::SourceCapture(std::shared_ptr<SourceJournalState>s,std::shared_ptr<SourceCaptureState>c):journal_(std::move(s)),capture_(std::move(c)){}
SourceCapture::~SourceCapture(){
    capture_->external=false;
    if(GetCurrentThreadId()!=journal_->thread)return;
    // Complete consumes the writer before a Captured record can be published.
    // A confirmed Published record has replaced that operation and owns the
    // durable responsibility. Its undelivered read pin is not unfinished work.
    // Keep unexpected cleanup resources conservative instead of discarding them.
    if(capture_->state==C::Published&&!capture_->unresolved&&capture_->published&&
       !capture_->confirmed&&!capture_->write.IsValid()&&!capture_->cleanup.IsValid()&&
       capture_->unrecordedName.empty()){
        capture_->lease={};Finish(*journal_,capture_);return;
    }
    if(!Resources(*capture_)&&!capture_->unresolved)Finish(*journal_,capture_);
}
C SourceCapture::State()const noexcept{return capture_->state;}
bool SourceCapture::NeedsResolution()const noexcept{return capture_->unresolved;}
const OperationId& SourceCapture::Id()const noexcept{return capture_->id;}
J SourceJournal::Open(const files::Directory&control,const files::Directory&pages,const files::Directory&raw,std::unique_ptr<SourceJournal>&out)noexcept{
    return OpenImpl(control,pages,raw,out,false);
}
J SourceJournal::OpenForRecovery(const files::Directory&control,const files::Directory&pages,const files::Directory&raw,std::unique_ptr<SourceJournal>&out)noexcept{
    return OpenImpl(control,pages,raw,out,true);
}
J SourceJournal::OpenImpl(const files::Directory&control,const files::Directory&pages,const files::Directory&raw,std::unique_ptr<SourceJournal>&out,bool recovery)noexcept{
    return Boundary([&]()->J{
        if(out)return J::Busy;auto fresh=std::make_shared<SourceJournalState>();files::Directory catalogDir;
        auto r=control.GetIdentity(fresh->controlId);if(r!=files::Result::Ok)return Map(r);
        r=pages.GetIdentity(fresh->pagesId);if(r!=files::Result::Ok)return Map(r);r=raw.GetIdentity(fresh->rawId);if(r!=files::Result::Ok)return Map(r);
        r=control.Child(L"catalog",false,catalogDir);if(r!=files::Result::Ok)return Map(r);r=catalogDir.GetIdentity(fresh->catalogId);if(r!=files::Result::Ok)return Map(r);
        const files::DirectoryIdentity ids[]{fresh->controlId,fresh->pagesId,fresh->rawId,fresh->catalogId};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)if(Same(ids[i],ids[j]))return J::Invalid;
        const BindingKey key{fresh->controlId.volume,fresh->controlId.fileIndex};std::shared_ptr<SourceJournalState>s;
        {std::lock_guard<std::mutex> guard(bindingsMutex);const auto it=bindings.find(key);if(it!=bindings.end())s=it->second.lock();}
        if(s){if(GetCurrentThreadId()!=s->thread)return J::Thread;if(!Same(s->pagesId,fresh->pagesId)||!Same(s->rawId,fresh->rawId)||!Same(s->catalogId,fresh->catalogId))return J::Conflict;}
        else {
            r=control.Share(fresh->control);if(r!=files::Result::Ok)return Map(r);r=pages.Share(fresh->pages);if(r!=files::Result::Ok)return Map(r);r=raw.Share(fresh->raw);if(r!=files::Result::Ok)return Map(r);
            fresh->catalog=std::make_unique<catalog::Store>(std::move(catalogDir));fresh->self=fresh;s=fresh;
        }
        Call call(*s);if(call.Result()!=J::Ok)return call.Result();files::FileLock lock;auto result=Lock(*s,lock);if(result!=J::Ok)return result;
        // Only a previously verified binding with retained local responsibility
        // may recover its own interrupted catalog while reopening. Fresh roots
        // must be explicitly recovered by their owner before Open can adopt them.
        View v;result=Load(*s,v,recovery||(s!=fresh&&bool(s->keepAlive)));if(result!=J::Ok)return result;
        if(!Nonzero(s->owner)){s->owner=v.root.sources.storeOwner;s->epoch=v.root.sources.maintenanceEpoch;s->directoryId=v.root.sources.directories[0].id;}
        auto candidate=std::unique_ptr<SourceJournal>(new SourceJournal(s));
        {std::lock_guard<std::mutex> guard(bindingsMutex);const auto it=bindings.find(key);if(it!=bindings.end()){auto existing=it->second.lock();if(existing&&existing!=s)return J::Busy;}
            bindings[key]=s;for(auto it=bindings.begin();it!=bindings.end();){if(it->second.expired())it=bindings.erase(it);else ++it;}}
        out.swap(candidate);return J::Ok;
    });
}
J SourceJournal::CreateCapture(OperationId operation,SourceId source,std::uint64_t bytes,std::unique_ptr<SourceCapture>&out)noexcept{
    return Boundary([&]()->J{Call call(*state_);if(call.Result()!=J::Ok)return call.Result();if(out)return J::Busy;
        if(!Nonzero(operation)||!Nonzero(source)||!bytes||bytes>INT64_MAX)return J::Invalid;
        if(state_->captures.count(operation))return J::Busy;if(state_->captures.size()>=MaximumSourceOperations)return J::Limit;
        for(const auto& v:state_->captures)if(v.second->source==source)return J::Busy;
        auto data=std::make_shared<SourceCaptureState>();data->id=operation;data->source=source;data->declared=bytes;
        auto candidate=std::unique_ptr<SourceCapture>(new SourceCapture(state_,data));state_->captures.emplace(operation,data);out.swap(candidate);return J::Ok;});
}
J SourceCapture::Reserve()noexcept{return Boundary([&]()->J{Call call(*journal_);if(call.Result()!=J::Ok)return call.Result();auto& c=*capture_;if(c.unresolved)return J::Indeterminate;if(c.state!=C::New)return J::NotReady;
    files::FileLock lock;auto result=Lock(*journal_,lock);if(result!=J::Ok)return result;View v;result=Load(*journal_,v);if(result!=J::Ok)return result;
    result=VerifyFootprint(c,v);if(result!=J::Ok)return result;
    SourceOperation op;op.id=c.id;op.sourceId=c.source;op.directoryId=journal_->directoryId;op.reservedBytes=c.declared;
    BusinessRoot next=v.root;next.sources.operations.push_back(op);std::sort(next.sources.operations.begin(),next.sources.operations.end(),[](const auto&a,const auto&b){return a.id<b.id;});
    return Commit(*journal_,c,v,next,C::Reserved);});}
J SourceCapture::Prepare(const wchar_t*path)noexcept{return Boundary([&]()->J{Call call(*journal_);if(call.Result()!=J::Ok)return call.Result();auto& c=*capture_;
    if(c.unresolved)return J::Indeterminate;if(c.state!=C::Reserved||!c.confirmed)return J::NotReady;if(Resources(c))return J::NeedsCleanup;
    files::FileLock lock;auto result=Lock(*journal_,lock);if(result!=J::Ok)return result;View v;result=Load(*journal_,v);if(result!=J::Ok)return result;result=VerifyFootprint(c,v);if(result!=J::Ok)return result;
    journal_->keepAlive=journal_;
    const auto prepared=journal_->raw.PrepareRawSource(path,c.confirmed->reservedBytes,c.write,c.cleanup);
    if(prepared!=files::Result::Ok){Trim(*journal_);return Map(prepared);}
    c.plan=c.write.Plan();auto op=*c.confirmed;op.plan=c.plan;op.reservedBytes=c.plan.sourceBytes;op.stage=SourceStage::Prepared;
    return MutateOperation(*journal_,c,v,op,C::Prepared);});}
J SourceCapture::Complete(const std::atomic_bool*cancelled)noexcept{return Boundary([&]()->J{Call call(*journal_);if(call.Result()!=J::Ok)return call.Result();auto& c=*capture_;
    if(c.unresolved)return J::Indeterminate;if(c.state!=C::Prepared||!c.confirmed||c.confirmed->stage!=SourceStage::Prepared)return J::NotReady;
    files::FileLock lock;auto result=Lock(*journal_,lock);if(result!=J::Ok)return result;View v;result=Load(*journal_,v);if(result!=J::Ok)return result;result=VerifyFootprint(c,v);if(result!=J::Ok)return result;
    if(!c.lease.IsValid()){if(!c.write.IsValid())return J::NotReady;const auto copied=c.write.Complete(c.lease,cancelled);if(copied!=files::Result::Ok)return Map(copied);}
    c.info=c.lease.Info();
    auto op=*c.confirmed;op.stage=SourceStage::Captured;op.captured=c.info;return MutateOperation(*journal_,c,v,op,C::Captured);});}
J SourceCapture::Publish(const std::vector<ProjectId>&owners,std::uint64_t lastUse,files::RawSourceLease&out)noexcept{return Boundary([&]()->J{Call call(*journal_);if(call.Result()!=J::Ok)return call.Result();auto& c=*capture_;
    if(c.unresolved)return J::Indeterminate;if(c.state!=C::Captured&&c.state!=C::Published)return J::NotReady;
    files::FileLock lock;auto result=Lock(*journal_,lock);if(result!=J::Ok)return result;View v;result=Load(*journal_,v);if(result!=J::Ok)return result;result=VerifyFootprint(c,v);if(result!=J::Ok)return result;
    if(c.state==C::Captured){if(!c.confirmed||c.confirmed->stage!=SourceStage::Captured)return J::NotReady;
        SourceRecord source;source.id=c.source;source.createdBy=c.id;source.directoryId=journal_->directoryId;source.file=c.confirmed->captured;source.owners=owners;source.lastUse=lastUse;
        BusinessRoot next=v.root;const auto it=FindOp(next.sources,c.id);if(it==next.sources.operations.end())return J::Conflict;next.sources.operations.erase(it);next.sources.sources.push_back(source);
        std::sort(next.sources.sources.begin(),next.sources.sources.end(),[](const auto&a,const auto&b){return a.id<b.id;});result=Commit(*journal_,c,v,next,C::Published);if(result!=J::Ok)return result;
    }else if(!c.published||c.published->owners!=owners||c.published->lastUse!=lastUse)return J::Conflict;
    if(!c.lease.IsValid()){const auto acquired=journal_->raw.AcquireRawSource(c.published->file,c.lease);if(acquired!=files::Result::Ok)return Map(acquired);}
    out=std::move(c.lease);Finish(*journal_,capture_);return J::Ok;});}
J SourceCapture::Cancel()noexcept{return Boundary([&]()->J{Call call(*journal_);if(call.Result()!=J::Ok)return call.Result();return CancelImpl(journal_,capture_,false);});}
J SourceCapture::Resolve()noexcept{return Boundary([&]()->J{Call call(*journal_);if(call.Result()!=J::Ok)return call.Result();const auto result=ResolveImpl(*journal_,*capture_);if(result==J::Ok&&capture_->state==C::Cancelled)Finish(*journal_,capture_);else Trim(*journal_);return result;});}
J SourceJournal::Recover(OperationId id)noexcept{return Boundary([&]()->J{Call call(*state_);if(call.Result()!=J::Ok)return call.Result();if(!Nonzero(id))return J::Invalid;
    std::shared_ptr<SourceCaptureState> c;const auto found=state_->captures.find(id);
    if(found!=state_->captures.end()){c=found->second;if(c->external)return J::Busy;if(c->unresolved){const auto r=ResolveImpl(*state_,*c);if(r!=J::Ok&&r!=J::Conflict)return r;if(c->unresolved)return J::Conflict;}}
    else {
        files::FileLock lock;auto r=Lock(*state_,lock);if(r!=J::Ok)return r;View v;r=Load(*state_,v,true);if(r!=J::Ok)return r;const auto operation=Op(v.root.sources,id);if(!operation)return J::Missing;
        c=std::make_shared<SourceCaptureState>();c->external=false;c->id=id;c->source=operation->sourceId;c->declared=operation->reservedBytes;c->confirmed=operation;
        c->state=operation->stage==SourceStage::Reserved?C::Reserved:(operation->stage==SourceStage::Captured||operation->stage==SourceStage::AbortCaptured)?C::Captured:C::Prepared;
        state_->captures.emplace(id,c);
    }
    if(c->state==C::Published){c->lease={};Finish(*state_,c);return J::NotReady;}
    state_->keepAlive=state_;const auto result=CancelImpl(state_,c,true);Trim(*state_);return result;});}
J SourceJournal::Acquire(SourceId id,files::RawSourceLease&out)noexcept{return Boundary([&]()->J{Call call(*state_);if(call.Result()!=J::Ok)return call.Result();if(!Nonzero(id))return J::Invalid;
    files::FileLock lock;auto result=Lock(*state_,lock);if(result!=J::Ok)return result;View v;result=Load(*state_,v);if(result!=J::Ok)return result;
    const auto source=Source(v.root.sources,id);if(!source)return J::Missing;if(source->state!=SourceState::Published)return J::NotReady;return Map(state_->raw.AcquireRawSource(source->file,out));});}
J SourceJournal::Retire(SourceId id)noexcept{
    return Boundary([&]()->J{
        Call call(*state_);if(call.Result()!=J::Ok)return call.Result();
        if(!Nonzero(id))return J::Invalid;
        if(state_->retirements.count(id))return J::Indeterminate;
        files::FileLock lock;auto result=Lock(*state_,lock);if(result!=J::Ok)return result;
        View v;result=Load(*state_,v);if(result!=J::Ok)return result;
        BusinessRoot next=v.root;const auto source=FindSource(next.sources,id);
        if(source==next.sources.sources.end())return J::Missing;
        if(source->state==SourceState::Retired)return J::Ok;
        RetirementAttempt attempt;attempt.before=*source;
        // Explicit retirement preserves owners and only changes metadata. It
        // does not infer cleanup permission or release any caller's read pin.
        source->state=SourceState::Retired;attempt.after=*source;
        Bytes bytes;const auto encoded=EncodeBusinessRoot(next,bytes);
        if(encoded!=Result::Ok)return Map(encoded);
        // Allocate the exact before/after receipt before publication can occur.
        state_->retirements.emplace(id,std::move(attempt));
        catalog::Snapshot published;
        const auto committed=state_->catalog->Commit(v.snapshot,bytes,published);
        if(committed==catalog::Result::Indeterminate){
            state_->keepAlive=state_;return J::Indeterminate;
        }
        state_->retirements.erase(id);Trim(*state_);return Map(committed);
    });
}
J SourceJournal::ResolveRetirement(SourceId id)noexcept{
    return Boundary([&]()->J{
        Call call(*state_);if(call.Result()!=J::Ok)return call.Result();
        if(!Nonzero(id))return J::Invalid;
        const auto pending=state_->retirements.find(id);
        if(pending==state_->retirements.end())return J::NotReady;
        files::FileLock lock;auto result=Lock(*state_,lock);if(result!=J::Ok)return result;
        View v;result=Load(*state_,v,true);if(result!=J::Ok)return result;
        const auto current=Source(v.root.sources,id);
        if(current&&Same(*current,pending->second.after))result=J::Ok;
        else if(current&&Same(*current,pending->second.before))result=J::Conflict;
        else return J::Conflict; // A foreign footprint cannot discharge this receipt.
        state_->retirements.erase(pending);Trim(*state_);return result;
    });
}
}
