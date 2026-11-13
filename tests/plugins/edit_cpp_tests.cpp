#include <mms/cpp/edit.hpp>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

using namespace mms::sdk;
namespace {
unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
struct Fake {
    MmsResult queryResult=MMS_OK, beginResult=MMS_OK, prepareResult=MMS_OK, commitResult=MMS_OK;
    MmsHandle resultHandle=41, lastHandle=0;
    bool shortHeader=false, missing=false, consumed=false, acceptCancel=true, throwUndo=false;
    unsigned queries=0, begins=0, joins=0, prepares=0, commits=0, cancels=0, undos=0, redos=0;
    std::uint64_t revision=0;
    bool labelOk=false;
    static Fake& F(void* p) { return *static_cast<Fake*>(p); }
    static MmsResult MMS_CALL Begin(void* p,MmsString label,std::uint64_t rev,MmsHandle* out) {
        auto& f=F(p); ++f.begins; f.revision=rev;
        f.labelOk=label.bytes==4 && label.data && std::memcmp(label.data,"test",4)==0;
        *out=f.resultHandle; return f.beginResult;
    }
    static MmsResult MMS_CALL Join(void* p,MmsHandle h,const MmsTransactionParticipantV1* item) {
        auto& f=F(p); ++f.joins; f.lastHandle=h; return item ? MMS_OK : MMS_E_ARGUMENT;
    }
    static MmsResult MMS_CALL Prepare(void* p,MmsHandle h) {
        auto& f=F(p); ++f.prepares; f.lastHandle=h; return f.prepareResult;
    }
    static MmsResult MMS_CALL Commit(void* p,MmsHandle h) {
        auto& f=F(p); ++f.commits; f.lastHandle=h; return f.commitResult;
    }
    static void MMS_CALL Cancel(void* p,MmsHandle h) {
        auto& f=F(p); ++f.cancels; f.lastHandle=h; if (f.acceptCancel) f.consumed=true;
    }
    static MmsResult MMS_CALL Undo(void* p) { auto& f=F(p); ++f.undos; if (f.throwUndo) throw std::bad_alloc{}; return MMS_OK; }
    static MmsResult MMS_CALL Redo(void* p) { ++F(p).redos; return MMS_OK; }
    static MmsResult MMS_CALL Query(void* p,MmsString name,uint32_t major,uint32_t minor,uint32_t bytes,void* out) {
        auto& f=F(p); ++f.queries;
        if (name.bytes!=8 || !name.data || std::memcmp(name.data,"mms.edit",8) || major!=1 || minor!=0 || bytes!=sizeof(MmsEditApiV1)) return MMS_E_ARGUMENT;
        MmsEditApiV1 api{{f.shortHeader ? 1u : uint32_t(sizeof(MmsEditApiV1)),1,0},p,Begin,Prepare,Join,Commit,Cancel,Undo,Redo};
        if (f.missing) api.cancel=nullptr;
        *static_cast<MmsEditApiV1*>(out)=api;
        return f.queryResult;
    }
    MmsHostV1 Host() { MmsHostV1 h{}; h.h={sizeof(h),1,0}; h.context=this; h.queryService=Query; return h; }
};
static_assert(!std::is_copy_constructible_v<EditTransaction>);
static_assert(!std::is_move_assignable_v<EditTransaction>);
static_assert(std::is_nothrow_move_constructible_v<EditTransaction>);
void Run() {
    Fake f; EditServices services;
    MmsHostV1 invalid{};
    CHECK(EditServices::Query(invalid,services)==MMS_E_VERSION && !services.ready());
    invalid.h={sizeof(invalid),1,0};
    CHECK(EditServices::Query(invalid,services)==MMS_E_ARGUMENT && !services.ready());
    CHECK(EditServices::Query(f.Host(),services)==MMS_OK && services.ready() && f.queries==1);
    EditServices preserved=services;
    f.queryResult=MMS_E_VERSION;
    CHECK(EditServices::Query(f.Host(),preserved)==MMS_E_VERSION && preserved.ready());
    f.queryResult=MMS_OK; f.shortHeader=true;
    CHECK(EditServices::Query(f.Host(),preserved)==MMS_E_VERSION && preserved.ready());
    f.shortHeader=false; f.missing=true;
    CHECK(EditServices::Query(f.Host(),preserved)==MMS_E_UNSUPPORTED && preserved.ready());
    f.missing=false;
    EditTransaction tx;
    f.beginResult=MMS_E_BUSY;
    CHECK(services.Begin("test",12,tx)==MMS_E_BUSY && !tx);
    f.beginResult=MMS_OK; f.resultHandle=0;
    CHECK(services.Begin("test",12,tx)==MMS_E_FAILED && !tx);
    f.resultHandle=41;
    CHECK(services.Begin("test",12,tx)==MMS_OK && tx && tx.get()==41 && f.labelOk && f.revision==12);
    CHECK(services.Begin("test",12,tx)==MMS_E_BUSY);
    MmsTransactionParticipantV1 participant{};
    CHECK(tx.Join(participant)==MMS_OK && f.joins==1 && f.lastHandle==41);
    MmsResult wrongJoin=MMS_OK, wrongPrepare=MMS_OK, wrongCommit=MMS_OK, wrongCancel=MMS_OK, wrongUndo=MMS_OK, wrongRedo=MMS_OK, wrongBegin=MMS_OK;
    std::thread worker([&] {
        EditTransaction other;
        wrongJoin=tx.Join(participant); wrongPrepare=tx.Prepare(); wrongCommit=tx.Commit();
        wrongCancel=tx.RequestCancel(); wrongUndo=services.Undo(); wrongRedo=services.Redo(); wrongBegin=services.Begin("test",12,other);
    }); worker.join();
    CHECK(wrongJoin==MMS_E_THREAD && wrongPrepare==MMS_E_THREAD && wrongCommit==MMS_E_THREAD &&
          wrongCancel==MMS_E_THREAD && wrongUndo==MMS_E_THREAD && wrongRedo==MMS_E_THREAD && wrongBegin==MMS_E_THREAD);
    CHECK(f.prepares==0 && f.commits==0 && f.cancels==0 && f.undos==0 && f.redos==0 && tx.get()==41);
    f.prepareResult=MMS_E_STALE; f.consumed=true;
    CHECK(tx.Prepare()==MMS_E_STALE && tx.get()==41);
    f.acceptCancel=false; f.consumed=false;
    CHECK(tx.RequestCancel()==MMS_OK && tx.get()==41 && f.cancels==1 && !f.consumed);
    f.acceptCancel=true;
    CHECK(tx.RequestCancel()==MMS_OK && tx.get()==41 && f.cancels==2);
    auto moved=std::move(tx);
    CHECK(!tx && moved.get()==41);
    CHECK(moved.detach()==41 && !moved);
    f.prepareResult=MMS_OK; f.commitResult=MMS_E_FAILED;
    CHECK(services.Begin("test",3,tx)==MMS_OK && tx.Prepare()==MMS_OK);
    CHECK(tx.Commit()==MMS_E_FAILED && tx.get()==41);
    CHECK(tx.RequestCancel()==MMS_OK && tx.detach()==41);
    CHECK(services.Begin("test",4,tx)==MMS_OK);
    f.commitResult=MMS_OK;
    CHECK(tx.Commit()==MMS_OK && !tx && tx.RequestCancel()==MMS_E_NOT_READY);
    CHECK(services.Undo()==MMS_OK && services.Redo()==MMS_OK && f.undos==1 && f.redos==1);
    f.throwUndo=true;
    CHECK(services.Undo()==MMS_E_MEMORY);
    const unsigned previous=f.cancels;
    { EditTransaction abandoned; CHECK(services.Begin("test",5,abandoned)==MMS_OK); }
    CHECK(f.cancels==previous); // Destructor does not pretend to cancel.
}
}
int main() { Run(); std::printf("edit_cpp_tests: %u checks, %u failures\n",checks,failures); return failures ? 1 : 0; }
