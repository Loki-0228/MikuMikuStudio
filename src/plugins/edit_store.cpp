#include "edit_store.hpp"
#include <type_traits>

namespace mikumikustudio::plugins {
class PropertyStageToken {
    friend class EditStore;
    EditStore* store_;
    EditClient* owner_;
    void* transaction_;
    MmsHandle handle_;
    PropertyStageToken(EditStore& store, EditClient& owner, void* transaction, MmsHandle handle):
        store_(&store), owner_(&owner), transaction_(transaction), handle_(handle) {}
public:
    PropertyStageToken(const PropertyStageToken&) = delete;
    PropertyStageToken& operator=(const PropertyStageToken&) = delete;
};
namespace {
struct Busy { bool& value; Busy(bool& b):value(b){value=true;} ~Busy(){value=false;} };
struct HostLock { EditHost& host; ~HostLock(){host.Leave();} };
struct OptionalHostLock {
    EditHost& host; bool entered=false;
    ~OptionalHostLock(){if(entered)host.Leave();}
};
template<class F> MmsResult ParticipantCall(EditHost& host,F&& callback) noexcept {
    using Callback=std::remove_reference_t<F>;
    return Boundary([&]{return host.WithParticipantCallback([](void* context)->MmsResult {
        return (*static_cast<Callback*>(context))();
    },&callback);});
}
StoredData CopyState(MmsBytes bytes,uint32_t schema){
    if(!schema || bytes.bytes>64ull*1024*1024 || (bytes.bytes&&!bytes.data))throw std::invalid_argument("undo bytes/schema");
    StoredData data;data.schema=schema;
    if(bytes.bytes){auto* p=static_cast<const unsigned char*>(bytes.data);data.bytes.assign(p,p+size_t(bytes.bytes));}return data;
}
}
MmsResult EditStore::Ready(EditClient& c) const {
    if(GetCurrentThreadId()!=thread_)return MMS_E_THREAD;
    if(c.stopping||poisoned_)return MMS_E_NOT_READY;
    return busy_?MMS_E_BUSY:MMS_OK;
}
MmsResult EditStore::Begin(EditClient& c,MmsString label,uint64_t expected,MmsHandle* out){
    if(auto r=Ready(c))return r;
    if(!out||!expected||!ValidId(c.id))return MMS_E_ARGUMENT;
    if(transactions_.size()>=1024)return MMS_E_BUSY;
    Busy guard(busy_);
    if(auto r=host_.Enter())return r;
    HostLock lock{host_};
    uint64_t revision=0;if(auto r=host_.Revision(revision))return r;if(revision!=expected)return MMS_E_STALE;
    auto transaction=std::make_unique<Transaction>();transaction->owner=&c;transaction->expected=expected;transaction->label=String(label);
    if(transaction->label.empty())return MMS_E_ARGUMENT;
    const auto h=next_++;transactions_.emplace(h,std::move(transaction));*out=h;return MMS_OK;
}
MmsResult EditStore::Join(EditClient& c,MmsHandle h,const MmsTransactionParticipantV1* p){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);if(found==transactions_.end())return MMS_E_STALE;
    return JoinInto(*found->second,c,p);
}
MmsResult EditStore::JoinInto(Transaction& t,EditClient& c,const MmsTransactionParticipantV1* p){
    if(t.prepared)return MMS_E_CONFLICT;
    if(t.participants.size()>=4096)return MMS_E_BUSY;
    if(!Valid(p)||!p->prepare||!p->commit||!p->cancel||!p->getUndoState||!p->releaseUndoState||String(p->pluginId)!=c.id)return MMS_E_ARGUMENT;
    const auto serializer=storage_.RestoreToken(c.id);if(!serializer)return MMS_E_NOT_READY;
    for(const auto& item:t.participants)if(item.id==c.id)return MMS_E_CONFLICT;
    t.participants.push_back({c.id,*p,serializer});return MMS_OK;
}
MmsResult EditStore::Current(const Transaction& t) noexcept {
    return Boundary([&]() -> MmsResult {
    uint64_t revision=0;if(auto r=host_.Revision(revision))return r;
    if(revision!=t.expected)return MMS_E_STALE;
    if(!t.stageGuards.empty()){
        if(t.owner->stopping)return MMS_E_NOT_READY;
        const auto serializer=storage_.RestoreToken(t.owner->id);
        if(!serializer)return MMS_E_NOT_READY;
        if(serializer!=t.stageSerializer)return MMS_E_STALE;
        for(const auto& guard:t.stageGuards)if(!guard->IsCurrent())return MMS_E_STALE;
    }
    for(const auto& p:t.participants){
        const auto serializer=storage_.RestoreToken(p.id);if(!serializer)return MMS_E_NOT_READY;
        if(serializer!=p.serializer)return MMS_E_STALE;
    }
    return MMS_OK;
    });
}
MmsResult EditStore::StageProperty(EditClient& c,MmsHandle h,
    std::shared_ptr<const EditStageGuard> registration,std::function<MmsResult(PropertyStageToken&)> callback){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);
    if(found==transactions_.end()||found->second->owner!=&c)return MMS_E_STALE;
    if(found->second->prepared)return MMS_E_CONFLICT;
    if(!registration||!callback)return MMS_E_ARGUMENT;
    Busy busy(busy_);
    // These owners must die before the host lock and Busy, including the last
    // shared registration and arbitrary std::function capture destructors.
    OptionalHostLock lock{host_};
    auto retained=std::move(registration);
    auto staged=std::move(callback);
    callback=nullptr;
    auto& t=*found->second;
    const auto result=Boundary([&]() -> MmsResult {
        if(auto r=host_.Enter())return r;lock.entered=true;
        if(t.stageGuards.size()>=4096)return MMS_E_BUSY;
        const auto serializer=storage_.RestoreToken(c.id);
        if(!serializer)return MMS_E_NOT_READY;
        if(t.stageSerializer&&t.stageSerializer!=serializer)return MMS_E_STALE;
        t.stageSerializer=serializer;t.stageGuards.push_back(retained);
        if(auto r=Current(t))return r;
        PropertyStageToken token(*this,c,&t,h);
        activeStage_=&token;
        struct Revoke {PropertyStageToken*& active;~Revoke(){active=nullptr;}} revoke{activeStage_};
        const auto called=Boundary([&]{return staged(token);});
        activeStage_=nullptr;
        if(called)return called;
        if(auto r=Current(t))return r;
        for(const auto& p:t.participants)if(p.id==c.id)return MMS_OK;
        return MMS_E_ARGUMENT;
    });
    if(result){
        // Never leave the capability active while cancellation calls plugins.
        activeStage_=nullptr;
        const auto cancelled=CancelAll(t);transactions_.erase(found);
        return cancelled?MMS_E_FAILED:result;
    }
    return MMS_OK;
}
MmsResult EditStore::JoinPropertyStage(PropertyStageToken& token,EditClient& c,const MmsTransactionParticipantV1* p){
    if(GetCurrentThreadId()!=thread_)return MMS_E_THREAD;
    if(!busy_||activeStage_!=&token)return MMS_E_BUSY;
    if(c.stopping||poisoned_)return MMS_E_NOT_READY;
    if(token.store_!=this||token.owner_!=&c)return MMS_E_STALE;
    const auto found=transactions_.find(token.handle_);
    if(found==transactions_.end()||found->second.get()!=token.transaction_||found->second->owner!=&c)return MMS_E_STALE;
    return Boundary([&]{return JoinInto(*found->second,c,p);});
}
MmsResult EditStore::CancelAll(Transaction& t) noexcept {
    MmsResult result=MMS_OK;
    for(auto it=t.participants.rbegin();it!=t.participants.rend();++it){
        if(it->table.cancel && ParticipantCall(host_,[&]{it->table.cancel(it->table.context);return MMS_OK;})){
            result=MMS_E_FAILED;poisoned_=true;host_.Poison();
        }
    }
    t.participants.clear();t.publication.reset();t.native.reset();t.stageGuards.clear();
    return result;
}
MmsResult EditStore::JoinNative(EditClient& c, MmsHandle h, std::shared_ptr<HistoryChange> change) {
    if (auto r=Ready(c)) return r;
    auto found=transactions_.find(h);
    if (found==transactions_.end() || found->second->owner!=&c) return MMS_E_STALE;
    auto& t=*found->second;
    if (!change) return MMS_E_ARGUMENT;
    if (t.prepared || t.native) return MMS_E_CONFLICT;
    t.native=std::move(change); return MMS_OK;
}
MmsResult EditStore::Prepare(EditClient& c,MmsHandle h){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);if(found==transactions_.end()||found->second->owner!=&c)return MMS_E_STALE;
    auto& t=*found->second;if(t.prepared)return MMS_E_CONFLICT;
    Busy guard(busy_);
    OptionalHostLock lock{host_};
    auto r=Boundary([&]() -> MmsResult {
        auto entered=host_.Enter();if(entered)return entered;lock.entered=true;
        if(auto result=Current(t))return result;
        if(t.participants.empty()&&!t.native)return MMS_E_ARGUMENT;
        auto record=std::make_shared<EditRecord>();record->label=t.label;record->native=t.native;
        for(const auto& p:t.participants){
            if(auto result=Current(t))return result;
            auto result=ParticipantCall(host_,[&]{return p.table.prepare(p.table.context);});if(result)return result;
            if(auto current=Current(t))return current;
        }
        // Capture only after every participant has prepared; all of them still
        // own their staged contexts until the transaction is consumed.
        for(const auto& p:t.participants){
            MmsBytes before{},after{};uint32_t schema=0;MmsHandle lease=0;
            if(auto result=Current(t))return result;
            auto result=ParticipantCall(host_,[&]{return p.table.getUndoState(p.table.context,&before,&after,&schema,&lease);});
            // A lease written before an error/exception is still released.
            // Otherwise a failed read owns nothing and has no release call.
            if(result&&!lease)return result;
            auto copied=result?result:Current(t);
            if(!copied)copied=Boundary([&]{record->before.emplace(p.id,CopyState(before,schema));record->after.emplace(p.id,CopyState(after,schema));return MMS_OK;});
            auto released=ParticipantCall(host_,[&]{p.table.releaseUndoState(p.table.context,lease);return MMS_OK;});
            if(released){poisoned_=true;host_.Poison();return MMS_E_FAILED;}
            if(copied)return copied;
            if(auto current=Current(t))return current;
        }
        if(auto result=Current(t))return result;
        auto reserved=host_.Reserve(std::move(record),t.publication);if(reserved)return reserved;
        if(!t.publication)return MMS_E_FAILED;
        if(auto result=Current(t))return result;
        t.prepared=true;return MMS_OK;
    });
    if(r){const auto cancelled=CancelAll(t);transactions_.erase(found);if(cancelled)return MMS_E_FAILED;}return r;
}
MmsResult EditStore::Commit(EditClient& c,MmsHandle h){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);if(found==transactions_.end()||found->second->owner!=&c)return MMS_E_STALE;
    if(!found->second->prepared)return MMS_E_CONFLICT;
    // The guard must outlive the consumed transaction: candidate and native
    // owner destruction is part of cleanup, not a new edit-safe boundary.
    Busy guard(busy_);OptionalHostLock lock{host_};auto t=std::move(found->second);transactions_.erase(found);
    auto r=Boundary([&]() -> MmsResult {
        auto entered=host_.Enter();if(entered)return entered;lock.entered=true;
        if(auto result=Current(*t))return result;
        if(!t->publication->IsCurrent())return MMS_E_STALE;
        if(auto result=Current(*t))return result;
        // Commit consumes the context even for a nonconforming throwing plugin.
        // Never attempt cancel on such a consumed context. Further edits stop.
        for(auto& p:t->participants){
            const auto committed=ParticipantCall(host_,[&]{p.table.commit(p.table.context);return MMS_OK;});
            p.table.context=nullptr;p.table.cancel=nullptr;
            if(committed){poisoned_=true;host_.Poison();return MMS_E_FAILED;}
        }
        t->publication->Publish();t->publication.reset();t->participants.clear();t->native.reset();t->stageGuards.clear();return MMS_OK;
    });
    if(r&&CancelAll(*t))return MMS_E_FAILED;
    return r;
}
void EditStore::Cancel(EditClient& c,MmsHandle h){
    if(GetCurrentThreadId()!=thread_||busy_)return;
    auto found=transactions_.find(h);if(found==transactions_.end()||found->second->owner!=&c)return;
    Busy guard(busy_);CancelAll(*found->second);transactions_.erase(found);
}
MmsResult EditStore::History(EditClient& c,bool redo){
    if(auto r=Ready(c))return r;
    if(HasPending())return MMS_E_BUSY;
    Busy guard(busy_);Busy request(historyRequest_);const auto result=host_.History(redo);
    return result==MMS_E_VERSION?MMS_E_NOT_READY:result;
}
bool EditStore::Retire(EditClient& c){
    if(GetCurrentThreadId()!=thread_||busy_)return false;
    Busy guard(busy_);
    for(auto it=transactions_.begin();it!=transactions_.end();){
        bool owns=it->second->owner==&c;
        for(const auto& p:it->second->participants)owns=owns||p.id==c.id;
        if(owns){CancelAll(*it->second);it=transactions_.erase(it);}else ++it;
    }
    return !poisoned_;
}
MmsResult EditStore::CancelPending(){
    if(GetCurrentThreadId()!=thread_)return MMS_E_THREAD;
    if(busy_)return MMS_E_BUSY;
    Busy guard(busy_);
    MmsResult result=MMS_OK;
    for(auto& [h,t]:transactions_)if(CancelAll(*t))result=MMS_E_FAILED;
    transactions_.clear();
    return result;
}
MmsResult EditStore::CancelOwned(EditClient& c){
    if(GetCurrentThreadId()!=thread_)return MMS_E_THREAD;
    if(busy_)return MMS_E_BUSY;
    Busy guard(busy_);MmsResult result=MMS_OK;
    for(auto it=transactions_.begin();it!=transactions_.end();){
        if(it->second->owner==&c){if(CancelAll(*it->second))result=MMS_E_FAILED;it=transactions_.erase(it);}
        else ++it;
    }
    return result;
}
EditStore::~EditStore(){
    busy_=true;
    for(auto& [h,t]:transactions_)CancelAll(*t);
    transactions_.clear();
    host_.DetachStore(this);
}
MmsEditApiV1 EditStore::Api(EditClient& c){
    c.store=this;MmsEditApiV1 api{Header<MmsEditApiV1>(),&c};
    api.begin=[](void* p,MmsString l,uint64_t r,MmsHandle* h){return Boundary([&]{auto& c=*static_cast<EditClient*>(p);return c.store->Begin(c,l,r,h);});};
    api.join=[](void* p,MmsHandle h,const MmsTransactionParticipantV1* v){return Boundary([&]{auto& c=*static_cast<EditClient*>(p);return c.store->Join(c,h,v);});};
    api.prepare=[](void* p,MmsHandle h){return Boundary([&]{auto& c=*static_cast<EditClient*>(p);return c.store->Prepare(c,h);});};
    api.commit=[](void* p,MmsHandle h){return Boundary([&]{auto& c=*static_cast<EditClient*>(p);return c.store->Commit(c,h);});};
    api.cancel=[](void* p,MmsHandle h){auto& c=*static_cast<EditClient*>(p);c.store->Cancel(c,h);};
    api.undo=[](void* p){return Boundary([&]{auto& c=*static_cast<EditClient*>(p);return c.store->History(c,false);});};
    api.redo=[](void* p){return Boundary([&]{auto& c=*static_cast<EditClient*>(p);return c.store->History(c,true);});};return api;
}
}
