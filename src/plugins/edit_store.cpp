#include "edit_store.hpp"

namespace mikudancestudio::plugins {
namespace {
struct Busy { bool& value; Busy(bool& b):value(b){value=true;} ~Busy(){value=false;} };
struct HostLock { EditHost& host; ~HostLock(){host.Leave();} };
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
    uint64_t revision=0;if(auto r=host_.Revision(revision))return r;if(revision!=expected)return MMS_E_STALE;
    auto transaction=std::make_unique<Transaction>();transaction->owner=&c;transaction->expected=expected;transaction->label=String(label);
    if(transaction->label.empty())return MMS_E_ARGUMENT;
    const auto h=next_++;transactions_.emplace(h,std::move(transaction));*out=h;return MMS_OK;
}
MmsResult EditStore::Join(EditClient& c,MmsHandle h,const MmsTransactionParticipantV1* p){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);if(found==transactions_.end())return MMS_E_STALE;
    auto& t=*found->second;if(t.prepared)return MMS_E_CONFLICT;
    if(t.participants.size()>=4096)return MMS_E_BUSY;
    if(!Valid(p)||!p->prepare||!p->commit||!p->cancel||!p->getUndoState||!p->releaseUndoState||String(p->pluginId)!=c.id)return MMS_E_ARGUMENT;
    const auto serializer=storage_.RestoreToken(c.id);if(!serializer)return MMS_E_NOT_READY;
    for(const auto& item:t.participants)if(item.id==c.id)return MMS_E_CONFLICT;
    t.participants.push_back({c.id,*p,serializer});return MMS_OK;
}
void EditStore::CancelAll(Transaction& t) noexcept {
    for(auto it=t.participants.rbegin();it!=t.participants.rend();++it){
        if(Boundary([&]{it->table.cancel(it->table.context);return MMS_OK;}))poisoned_=true;
    }
    t.participants.clear();t.publication.reset();
}
MmsResult EditStore::Prepare(EditClient& c,MmsHandle h){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);if(found==transactions_.end()||found->second->owner!=&c)return MMS_E_STALE;
    auto& t=*found->second;if(t.prepared)return MMS_E_CONFLICT;
    Busy guard(busy_);
    auto r=Boundary([&]() -> MmsResult {
        auto entered=host_.Enter();if(entered)return entered;HostLock lock{host_};
        uint64_t revision=0;if(auto result=host_.Revision(revision))return result;if(revision!=t.expected)return MMS_E_STALE;
        if(t.participants.empty())return MMS_E_ARGUMENT;
        auto record=std::make_shared<EditRecord>();record->label=t.label;
        for(const auto& p:t.participants){
            const auto serializer=storage_.RestoreToken(p.id);if(!serializer)return MMS_E_NOT_READY;
            if(serializer!=p.serializer)return MMS_E_STALE;
            auto result=Boundary([&]{return p.table.prepare(p.table.context);});if(result)return result;
        }
        // Capture only after every participant has prepared; all of them still
        // own their staged contexts until the transaction is consumed.
        for(const auto& p:t.participants){
            MmsBytes before{},after{};uint32_t schema=0;MmsHandle lease=0;
            auto result=Boundary([&]{return p.table.getUndoState(p.table.context,&before,&after,&schema,&lease);});if(result)return result;
            auto copied=Boundary([&]{record->before.emplace(p.id,CopyState(before,schema));record->after.emplace(p.id,CopyState(after,schema));return MMS_OK;});
            auto released=Boundary([&]{p.table.releaseUndoState(p.table.context,lease);return MMS_OK;});
            if(released){poisoned_=true;return released;}if(copied)return copied;
        }
        auto reserved=host_.Reserve(std::move(record),t.publication);if(reserved)return reserved;
        if(!t.publication)return MMS_E_FAILED;
        t.prepared=true;return MMS_OK;
    });
    if(r){CancelAll(t);transactions_.erase(found);}return r;
}
MmsResult EditStore::Commit(EditClient& c,MmsHandle h){
    if(auto r=Ready(c))return r;
    auto found=transactions_.find(h);if(found==transactions_.end()||found->second->owner!=&c)return MMS_E_STALE;
    if(!found->second->prepared)return MMS_E_CONFLICT;
    auto t=std::move(found->second);transactions_.erase(found);Busy guard(busy_);
    auto r=Boundary([&]() -> MmsResult {
        auto entered=host_.Enter();if(entered)return entered;HostLock lock{host_};
        uint64_t revision=0;if(auto result=host_.Revision(revision))return result;if(revision!=t->expected)return MMS_E_STALE;
        for(const auto& p:t->participants){
            const auto serializer=storage_.RestoreToken(p.id);if(!serializer)return MMS_E_NOT_READY;
            if(serializer!=p.serializer)return MMS_E_STALE;
        }
        // Commit consumes the context even for a nonconforming throwing plugin.
        // Never attempt cancel on such a consumed context. Further edits stop.
        for(auto& p:t->participants){
            const auto committed=Boundary([&]{p.table.commit(p.table.context);return MMS_OK;});
            p.table.context=nullptr;p.table.cancel=nullptr;
            if(committed){poisoned_=true;return committed;}
        }
        t->publication->Publish();t->participants.clear();return MMS_OK;
    });
    if(r){
        for(auto it=t->participants.rbegin();it!=t->participants.rend();++it)if(it->table.cancel)
            if(Boundary([&]{it->table.cancel(it->table.context);return MMS_OK;}))poisoned_=true;
    }
    return r;
}
void EditStore::Cancel(EditClient& c,MmsHandle h){
    if(GetCurrentThreadId()!=thread_||busy_)return;
    auto found=transactions_.find(h);if(found==transactions_.end()||found->second->owner!=&c)return;
    Busy guard(busy_);CancelAll(*found->second);transactions_.erase(found);
}
MmsResult EditStore::History(EditClient& c,bool redo){
    if(auto r=Ready(c))return r;
    for(auto& [h,t]:transactions_)if(t->prepared)return MMS_E_BUSY;
    Busy guard(busy_);const auto result=host_.History(redo);
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
EditStore::~EditStore(){busy_=true;for(auto& [h,t]:transactions_)CancelAll(*t);}
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
