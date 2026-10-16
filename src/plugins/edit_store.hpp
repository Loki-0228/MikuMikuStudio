#pragma once
#include "storage_store.hpp"
#include <mms/edit.h>

namespace mikudancestudio::plugins {
class HistoryChange;
struct EditRecord {
    std::string label;
    StoredRecords before, after;
    std::shared_ptr<HistoryChange> native;
};
// A host reservation owns all memory needed for the history node and native
// publication. Its destructor cancels an unpublished reservation. Publish may
// only swap preallocated state and must not call plugins or pump messages.
class EditPublication {
public:
    virtual ~EditPublication() = default;
    virtual bool IsCurrent() const noexcept { return true; }
    virtual void Publish() noexcept = 0;
};
class EditHost {
public:
    virtual ~EditHost() = default;
    virtual MmsResult Revision(uint64_t&) = 0;
    virtual MmsResult Enter() = 0;
    virtual void Leave() noexcept = 0;
    virtual MmsResult Reserve(std::shared_ptr<const EditRecord>, std::unique_ptr<EditPublication>&) = 0;
    virtual MmsResult History(bool redo) = 0;
    // A participant violated a void callback contract. Freeze native history
    // too, because the scene can now contain a partly published operation.
    virtual void Poison() noexcept {}
};
class EditStore;
struct EditClient { EditStore* store=nullptr; std::string id; bool stopping=false; };
class EditStore {
    struct Participant { std::string id; MmsTransactionParticipantV1 table; MmsHandle serializer; };
    struct Transaction {
        EditClient* owner;
        uint64_t expected;
        std::string label;
        std::vector<Participant> participants;
        std::shared_ptr<HistoryChange> native;
        std::unique_ptr<EditPublication> publication;
        bool prepared=false;
    };
    DWORD thread_=GetCurrentThreadId();
    EditHost& host_;
    StorageStore& storage_;
    bool busy_=false,poisoned_=false;
    MmsHandle next_=1;
    std::map<MmsHandle,std::unique_ptr<Transaction>> transactions_;
    MmsResult Ready(EditClient&) const;
    MmsResult Begin(EditClient&,MmsString,uint64_t,MmsHandle*);
    MmsResult Join(EditClient&,MmsHandle,const MmsTransactionParticipantV1*);
    MmsResult Prepare(EditClient&,MmsHandle);
    MmsResult Commit(EditClient&,MmsHandle);
    void Cancel(EditClient&,MmsHandle);
    void CancelAll(Transaction&) noexcept;
    MmsResult History(EditClient&,bool);
public:
    EditStore(EditHost& host,StorageStore& storage):host_(host),storage_(storage){}
    ~EditStore();
    MmsEditApiV1 Api(EditClient&);
    // Internal scene/UI services attach their aggregate native candidate to
    // the same transaction; this pointer never crosses the public C ABI.
    MmsResult JoinNative(EditClient&, MmsHandle, std::shared_ptr<HistoryChange>);
    bool Retire(EditClient&);
    bool InCallback() const { return busy_; }
};
}
