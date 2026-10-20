#pragma once
#include "storage_store.hpp"
#include <mms/edit.h>

namespace mikumikustudio::plugins {
class HistoryChange;
class EditStore;
class SharedHistory;
class PropertyStageToken;
// Internal registration identity retained until a staged transaction is
// consumed. IsCurrent is a host-only, allocation-free query, never a plugin
// callback and never a global UI revision comparison.
struct EditStageGuard {
    virtual ~EditStageGuard() = default;
    virtual bool IsCurrent() const noexcept = 0;
};
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
    virtual void AttachStore(EditStore*) {}
    virtual void DetachStore(EditStore*) noexcept {}
    virtual MmsResult Revision(uint64_t&) = 0;
    virtual MmsResult Enter() = 0;
    virtual void Leave() noexcept = 0;
    virtual MmsResult Reserve(std::shared_ptr<const EditRecord>, std::unique_ptr<EditPublication>&) = 0;
    virtual MmsResult History(bool redo) = 0;
    // The manager supplies the same read-only UI boundary for every phase.
    // A pointer/context runner keeps commit and cancellation allocation-free.
    virtual MmsResult WithParticipantCallback(MmsResult (*call)(void*), void* context) {
        return call ? Boundary([&] { return call(context); }) : MMS_E_ARGUMENT;
    }
    // A participant violated a void callback contract. Freeze native history
    // too, because the scene can now contain a partly published operation.
    virtual void Poison() noexcept {}
};
struct EditClient { EditStore* store=nullptr; std::string id; bool stopping=false; };
class EditStore {
    friend class SharedHistory;
    struct Participant { std::string id; MmsTransactionParticipantV1 table; MmsHandle serializer; };
    struct Transaction {
        EditClient* owner;
        uint64_t expected;
        std::string label;
        std::vector<Participant> participants;
        std::shared_ptr<HistoryChange> native;
        std::unique_ptr<EditPublication> publication;
        std::vector<std::shared_ptr<const EditStageGuard>> stageGuards;
        MmsHandle stageSerializer=0;
        bool prepared=false;
    };
    DWORD thread_=GetCurrentThreadId();
    EditHost& host_;
    StorageStore& storage_;
    bool busy_=false,poisoned_=false,historyRequest_=false;
    MmsHandle next_=1;
    std::map<MmsHandle,std::unique_ptr<Transaction>> transactions_;
    PropertyStageToken* activeStage_=nullptr;
    MmsResult Ready(EditClient&) const;
    MmsResult Begin(EditClient&,MmsString,uint64_t,MmsHandle*);
    MmsResult Join(EditClient&,MmsHandle,const MmsTransactionParticipantV1*);
    MmsResult JoinInto(Transaction&,EditClient&,const MmsTransactionParticipantV1*);
    MmsResult Current(const Transaction&) noexcept;
    MmsResult Prepare(EditClient&,MmsHandle);
    MmsResult Commit(EditClient&,MmsHandle);
    void Cancel(EditClient&,MmsHandle);
    MmsResult CancelAll(Transaction&) noexcept;
    MmsResult History(EditClient&,bool);
    // Only the immediate EditStore::History forwarding may enter a journal
    // while busy_. Consume once so cleanup and nested host callbacks cannot
    // inherit permission after the journal's traversal has returned.
    bool ConsumeHistoryRequest() noexcept {
        const bool requested=historyRequest_;historyRequest_=false;return requested;
    }
public:
    EditStore(EditHost& host,StorageStore& storage):host_(host),storage_(storage){host_.AttachStore(this);}
    ~EditStore();
    MmsEditApiV1 Api(EditClient&);
    // Internal scene/UI services attach their aggregate native candidate to
    // the same transaction; this pointer never crosses the public C ABI.
    MmsResult JoinNative(EditClient&, MmsHandle, std::shared_ptr<HistoryChange>);
    MmsResult StageProperty(EditClient&, MmsHandle,
        std::shared_ptr<const EditStageGuard>, std::function<MmsResult(PropertyStageToken&)>);
    MmsResult JoinPropertyStage(PropertyStageToken&, EditClient&,
        const MmsTransactionParticipantV1*);
    bool Retire(EditClient&);
    // Failed initiating callbacks cancel their own transactions only. They
    // cannot cancel another owner's transaction merely by joining it.
    MmsResult CancelOwned(EditClient&);
    bool InCallback() const { return busy_; }
    bool HasPending() const noexcept { return !transactions_.empty(); }
    // UI-thread lifecycle boundary only. Does not acquire the host gate: the
    // caller may already own it. Cancel before replacing the scene or world.
    MmsResult CancelPending();
};
}
