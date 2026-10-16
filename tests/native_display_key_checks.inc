// Include after native_bone_registration_checks.inc. Run NativeDisplayKeyChecks().
// Uses the actual legacy DeleteMarkedModelKeys as the independent byte oracle.
#include "../src/plugins/native_display_keys.hpp"
namespace {
void DisplayTestSeed(Fixture& fixture) {
    auto& m=*fixture.m;m.ikChainCount=2;m.boneOrderCount=3;
    for(unsigned slot=0;slot<4;++slot) {
        auto& key=m.displayKeys[slot];delete[] key.ikStates;delete[] key.selectorStates;
        key={};key.ikStates=new unsigned char[2]{static_cast<unsigned char>(slot%2),static_cast<unsigned char>((slot+1)%2)};
        key.selectorStates=new mdl::BoneReference[3]{{int(slot+1),int(slot+4)},{-1,7},{3,int(slot)}};
        key.visible=0;key.reserved0[1]=static_cast<unsigned char>(slot+10);
        key.reserved1[2]=static_cast<unsigned char>(slot+20);
    }
    m.displayKeys[0].next=3;
    m.displayKeys[3].frame=10;m.displayKeys[3].next=1;
    m.displayKeys[1].frame=20;m.displayKeys[1].previous=3;m.displayKeys[1].next=2;
    m.displayKeys[2].frame=30;m.displayKeys[2].previous=1;
}
struct DisplayTestState {
    std::vector<mdl::DisplayKey> keys;
    std::vector<std::vector<unsigned char>> ik;
    std::vector<std::vector<mdl::BoneReference>> parents;
    explicit DisplayTestState(const Fixture& fixture) {
        const auto& m=*fixture.m;const auto capacity=mdl::KeyPoolCapacity(m.displayKeys);
        for(std::size_t slot=0;slot<capacity;++slot) {
            auto key=m.displayKeys[slot];key.ikStates=nullptr;key.selectorStates=nullptr;keys.push_back(key);
            ik.emplace_back();parents.emplace_back();
            if(m.ikChainCount)ik.back().assign(m.displayKeys[slot].ikStates,m.displayKeys[slot].ikStates+m.ikChainCount);
            if(m.boneOrderCount)parents.back().assign(m.displayKeys[slot].selectorStates,m.displayKeys[slot].selectorStates+m.boneOrderCount);
        }
    }
    bool Matches(const Fixture& fixture) const {
        const auto& m=*fixture.m;if(mdl::KeyPoolCapacity(m.displayKeys)<keys.size())return false;
        for(std::size_t slot=0;slot<keys.size();++slot) {
            const auto& key=m.displayKeys[slot];const auto& saved=keys[slot];
            if(key.frame!=saved.frame||key.previous!=saved.previous||key.next!=saved.next||
                key.visible!=saved.visible||key.allocated!=saved.allocated||
                std::memcmp(key.reserved0,saved.reserved0,sizeof(key.reserved0))||
                std::memcmp(key.reserved1,saved.reserved1,sizeof(key.reserved1))||
                (!ik[slot].empty()&&std::memcmp(key.ikStates,ik[slot].data(),ik[slot].size()))||
                (!parents[slot].empty()&&std::memcmp(key.selectorStates,parents[slot].data(),parents[slot].size()*sizeof(mdl::BoneReference))))return false;
        }
        return true;
    }
};
std::shared_ptr<plugins::HistoryChange> DisplayTestPlan(BoneRegistrationFixture& f,unsigned slot=0) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanDisplayKeyDeletion(*f.app,slot,change)==MMS_OK&&change,"display deletion produces an independent record");
    return change;
}
void DisplayTestCommit(BoneRegistrationFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("display key deletion",change,transaction)==MMS_OK,"display deletion journal prepares");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"display deletion commits without allocation");
}
void DisplayTestFreeGrowth(Fixture& f) {
    // Fixture's own four buffers use new[]. DisplayKeyPoolEnsure creates the
    // additional payloads with malloc/calloc; clean those before Fixture exits.
    for(std::size_t slot=4;slot<mdl::KeyPoolCapacity(f.m->displayKeys);++slot) {
        std::free(f.m->displayKeys[slot].ikStates);f.m->displayKeys[slot].ikStates=nullptr;
        std::free(f.m->displayKeys[slot].selectorStates);f.m->displayKeys[slot].selectorStates=nullptr;
    }
}
void DisplayDeletionParityChecks() {
    const std::vector<std::vector<unsigned>> cases={{0},{3},{1},{2},{1,3},{0,1,2,3}};
    for(const auto& marks:cases) {
        BoneRegistrationFixture f;DisplayTestSeed(f.first);DisplayTestSeed(f.second);
        for(auto slot:marks)f.first.m->displayKeys[slot].allocated=f.second.m->displayKeys[slot].allocated=3;
        const DisplayTestState before(f.first);const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);
        std::array<unsigned char*,4> pointers{};for(unsigned slot=0;slot<4;++slot)pointers[slot]=f.first.m->displayKeys[slot].ikStates;
        auto change=DisplayTestPlan(f);
        Check(change->RetainedBytes()>2*(2+3*sizeof(mdl::BoneReference))&&before.Matches(f.first)&&f.first.Digest()==digest,
            "display planning budgets deep before and after payloads without live writes");
        DisplayTestCommit(f,change);DeleteMarkedModelKeys(f.second.raw(),f.app->CurrentFrame());
        Check(DisplayTestState(f.second).Matches(f.first),"display root middle tail and adjacent deletion match the real legacy writer");
        for(unsigned slot=0;slot<4;++slot)Check(f.first.m->displayKeys[slot].ikStates==pointers[slot],
            "display publication preserves the native nested-buffer addresses");
        Check(PoseRingDigest(f.first)==ring&&f.first.m->maxFrame==99&&f.first.m->searchCursor==5&&
            f.history.UndoCount()==1,"display deletion leaves old ring frame maximum and allocation cursor unchanged");
        const DisplayTestState after(f.first);
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"display undo restores links IK switches external parents and marks");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"display redo restores defaults and complete neighbour state");
    }
}
void DisplayDeletionOwnedLifetimeChecks() {
    BoneRegistrationFixture f;DisplayTestSeed(f.first);DisplayTestSeed(f.second);
    f.first.m->displayKeys[1].allocated=1;f.second.m->displayKeys[2].allocated=1;
    const DisplayTestState firstBefore(f.first),secondBefore(f.second);
    const auto firstRing=PoseRingDigest(f.first),secondRing=PoseRingDigest(f.second);
    auto change=DisplayTestPlan(f);DisplayTestCommit(f,change);DisplayTestCommit(f,DisplayTestPlan(f,1));
    const DisplayTestState firstAfter(f.first),secondAfter(f.second);
    for(unsigned count=0;count<100;++count)PushBoneEditUndo(f.app.get());
    const auto overwritten=PoseRingDigest(f.first);
    Check(f.history.History(false)==MMS_OK&&secondBefore.Matches(f.second)&&firstAfter.Matches(f.first),
        "display deletion on another model is first in common undo order");
    Check(f.history.History(false)==MMS_OK&&firstBefore.Matches(f.first)&&PoseRingDigest(f.first)==overwritten&&
        PoseRingDigest(f.second)==secondRing&&firstRing!=overwritten,
        "deep display history survives one hundred legacy bone-ring snapshots");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&
        firstAfter.Matches(f.first)&&secondAfter.Matches(f.second)&&f.SelectionUnchanged(),
        "cross-model display redo restores both owners without changing selection");
    f.history.Reset();f.history.Collect();
    // The independently retained record owns both directions. Candidate also
    // keeps it alive when its caller drops the final record reference.
    std::unique_ptr<plugins::PreparedHistoryChange> inverse;
    Check(change->Prepare(false,inverse)==MMS_OK&&inverse->IsCurrent(),"display inverse candidate prepares independently of journal ownership");
    change.reset();failAfter=0;inverse->Publish();inverse.reset();failAfter=-1;
    Check(firstBefore.Matches(f.first),"orphaned prepared candidate retains its deep payload and publishes without allocation");
}
void DisplayDeletionGrowthAndDirectionChecks() {
    BoneRegistrationFixture f;DisplayTestSeed(f.first);f.first.m->displayKeys[1].allocated=1;
    const DisplayTestState before(f.first);auto change=DisplayTestPlan(f);
    const auto identity=mdl::KeyPoolIdentity(f.first.m->displayKeys);auto* old=f.first.m->displayKeys;
    Check(mdl::DisplayKeyPoolEnsure(*f.first.m,5)&&f.first.m->displayKeys!=old&&
        mdl::KeyPoolIdentity(f.first.m->displayKeys)==identity,"registered display growth retains logical pool identity");
    f.first.m->displayKeys[4].visible=7;f.first.m->displayKeys[4].ikStates[1]=9;
    for(bool forward:{true,false,true}) {
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(forward,candidate)==MMS_OK&&candidate->IsCurrent(),"display history resolves grown pool by stable identity");
        failAfter=0;candidate->Publish();candidate.reset();failAfter=-1;
        Check(f.first.m->displayKeys[4].visible==7&&f.first.m->displayKeys[4].ikStates[1]==9,
            "sparse display publication leaves subsequently added buffer contents untouched");
        if(!forward)Check(before.Matches(f.first),"display inverse publication restores original nested values after pool growth");
    }
    DisplayTestFreeGrowth(f.first);
}
void DisplayDeletionEmptyPayloadAndIdentityChecks() {
    {
        BoneRegistrationFixture f;DisplayTestSeed(f.first);
        f.first.m->ikChainCount=f.first.m->boneOrderCount=0;f.first.m->displayKeys[0].allocated=1;
        const auto ik=f.first.m->displayKeys[0].ikStates[0];
        const DisplayTestState before(f.first);auto change=DisplayTestPlan(f);DisplayTestCommit(f,change);
        Check(f.first.m->displayKeys[0].visible==1&&f.first.m->displayKeys[0].ikStates[0]==ik&&
            f.history.History(false)==MMS_OK&&before.Matches(f.first),
            "zero IK and external-parent counts need no nested copies and preserve untouched buffers");
    }
    {
        BoneRegistrationFixture f;DisplayTestSeed(f.first);f.first.m->displayKeys[1].allocated=1;
        auto change=DisplayTestPlan(f);const auto identity=mdl::KeyPoolIdentity(f.first.m->displayKeys);
        for(unsigned slot=0;slot<4;++slot){delete[] f.first.m->displayKeys[slot].ikStates;delete[] f.first.m->displayKeys[slot].selectorStates;}
        mdl::KeyPoolDestroy(f.first.m->displayKeys);f.first.m->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(4);
        DisplayTestSeed(f.first);const DisplayTestState replacement(f.first);
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(mdl::KeyPoolIdentity(f.first.m->displayKeys)!=identity&&change->Prepare(true,candidate)==MMS_E_STALE&&
            !candidate&&replacement.Matches(f.first),"new display ownership cannot revive history even when an allocator reuses an address");
    }
}
void DisplayDeletionRejectionChecks() {
    BoneRegistrationFixture f;DisplayTestSeed(f.first);std::shared_ptr<plugins::HistoryChange> change;
    const auto clean=f.first.Digest();
    Check(plugins::PlanDisplayKeyDeletion(*f.app,0,change)==MMS_OK&&!change&&f.first.Digest()==clean,
        "unmarked display deletion is a true no-op");
    f.first.m->displayKeys[1].allocated=1;
    for(int family=0;family<2;++family) {
        if(family)f.first.m->morphKeys[0].allocated=1;else f.first.m->boneKeys[0].allocated=1;
        const auto before=f.first.Digest();
        Check(plugins::PlanDisplayKeyDeletion(*f.app,0,change)==MMS_E_NOT_READY&&!change&&f.first.Digest()==before,
            "mixed display and bone or morph marks refuse the whole plan");
        f.first.m->boneKeys[0].allocated=f.first.m->morphKeys[0].allocated=0;
    }
    const auto original=f.first.m->displayKeys[1];
    for(int kind=0;kind<6;++kind) {
        auto& key=f.first.m->displayKeys[1];key=original;
        if(kind==0)key.next=999;if(kind==1)key.next=1;if(kind==2)key.frame=5;
        if(kind==3)key.ikStates=nullptr;if(kind==4)key.selectorStates=nullptr;
        if(kind==5)key.ikStates=f.first.m->displayKeys[2].ikStates;
        Check(plugins::PlanDisplayKeyDeletion(*f.app,0,change)==MMS_E_ARGUMENT&&!change,
            "malformed display chain missing nested payload and alias ownership are rejected");
        key=original;
    }
    f.first.m->displayKeys[3].next=2;f.first.m->displayKeys[2].previous=3;
    Check(plugins::PlanDisplayKeyDeletion(*f.app,0,change)==MMS_E_ARGUMENT&&!change,
        "marked display key disconnected from its root is rejected");
    f.first.m->displayKeys[3].next=1;f.first.m->displayKeys[2].previous=1;
    std::array<mdl::DisplayKey,4> borrowed{};borrowed[0].allocated=1;
    auto* pool=f.first.m->displayKeys;f.first.m->displayKeys=borrowed.data();
    Check(plugins::PlanDisplayKeyDeletion(*f.app,0,change)==MMS_E_NOT_READY&&!change,
        "unregistered display storage is rejected before assuming legacy capacity or ownership");
    f.first.m->displayKeys=pool;
    for(int kind=0;kind<4;++kind) {
        if(kind==0)f.app->PlaybackActive()=1;if(kind==1)f.app->FrameStepPlayback()=1;
        if(kind==2)f.app->state.recordPlaybackStartPending=1;
        Check(plugins::PlanDisplayKeyDeletion(*f.app,kind==3?255:0,change)==MMS_E_NOT_READY&&!change,
            "display deletion refuses invalid owner and playback or recording gates");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=0;
    }
}
void DisplayDeletionFailureAndStaleChecks() {
    BoneRegistrationFixture f;DisplayTestSeed(f.first);f.first.m->displayKeys[1].allocated=1;
    const DisplayTestState before(f.first);const auto digest=f.first.Digest();
    std::shared_ptr<plugins::HistoryChange> change;unsigned failures=0;bool planned=false;
    for(long allowance=0;allowance<256&&!planned;++allowance) {
        failAfter=allowance;const auto result=plugins::PlanDisplayKeyDeletion(*f.app,0,change);failAfter=-1;
        Check(f.first.Digest()==digest&&before.Matches(f.first),"display planning OOM preserves links and every nested byte");
        if(result==MMS_OK){Check(bool(change),"display OOM sweep returns one complete record");planned=true;}
        else{++failures;Check(result==MMS_E_MEMORY&&!change,"display planning OOM has no partial output");}
    }
    Check(planned&&failures>10,"display planning injection reaches deep payload and retained record allocations");
    bool committed=false;failures=0;
    for(long allowance=0;allowance<256&&!committed;++allowance) {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;std::string label="display deep history";
        failAfter=allowance;const auto result=f.history.PrepareNative(std::move(label),change,tx);failAfter=-1;
        Check(f.first.Digest()==digest&&before.Matches(f.first),"display candidate OOM preserves live nested pointers and data");
        if(result==MMS_OK){Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,
            "display candidate commits allocation-free after fault sweep");committed=true;}
        else{++failures;Check(result==MMS_E_MEMORY&&!tx&&f.history.UndoCount()==0,"display prepare OOM leaves journal empty");}
    }
    Check(committed&&failures>8,"display journal injection covers independent nested candidates");
    const DisplayTestState after(f.first);
    failAfter=0;const auto failedUndo=f.history.History(false);failAfter=-1;
    Check(failedUndo==MMS_E_MEMORY&&after.Matches(f.first)&&f.history.UndoCount()==1,
        "failed display inverse preparation cannot partially reset nested payloads");
    Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"display undo survives previous allocation failure");
    failAfter=0;const auto failedRedo=f.history.History(true);failAfter=-1;
    Check(failedRedo==MMS_E_MEMORY&&before.Matches(f.first)&&f.history.RedoCount()==1,
        "failed display redo preserves the redo node and all selection bytes");
    {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> cancelled;
        Check(f.history.PrepareNative("cancel display deletion",change,cancelled)==MMS_OK,
            "display cancellation prepares without consuming redo");
        failAfter=0;cancelled.reset();failAfter=-1;
        Check(before.Matches(f.first)&&f.history.UndoCount()==0&&f.history.RedoCount()==1,
            "cancelling prepared display deletion preserves selection and redo without allocating");
    }
    for(int kind=0;kind<6;++kind) {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("stale display",change,tx)==MMS_OK,"display stale candidate prepared");
        auto& key=f.first.m->displayKeys[1];auto* old=key.ikStates;
        auto* unused=f.first.m->displayKeys[0].ikStates;
        if(kind==0)key.ikStates[0]^=1;if(kind==1)++key.selectorStates[0].boneIndex;
        if(kind==2){key.ikStates=new unsigned char[2];std::memcpy(key.ikStates,old,2);}
        if(kind==3)key.allocated=7;if(kind==4)++f.first.m->ikChainCount;
        if(kind==5)f.first.m->displayKeys[0].ikStates=key.ikStates;
        Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_E_STALE&&f.history.RedoCount()==1,
            "changed IK values parents payload addresses counts and marks invalidate display candidate without clearing redo");
        if(kind==0)key.ikStates[0]^=1;if(kind==1)--key.selectorStates[0].boneIndex;
        if(kind==2){delete[] key.ikStates;key.ikStates=old;}
        if(kind==3)key.allocated=1;if(kind==4)--f.first.m->ikChainCount;
        if(kind==5)f.first.m->displayKeys[0].ikStates=unused;
        Check(before.Matches(f.first),"stale candidate cleanup did not modify any other display field");
    }
    Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"display redo remains valid after stale candidate refusals");
}
}
void NativeDisplayKeyChecks() {
    std::puts("  display keys: native parity");DisplayDeletionParityChecks();
    std::puts("  display keys: owned lifetime and common history");DisplayDeletionOwnedLifetimeChecks();
    std::puts("  display keys: growth and publication");DisplayDeletionGrowthAndDirectionChecks();
    std::puts("  display keys: empty payload and replacement identity");DisplayDeletionEmptyPayloadAndIdentityChecks();
    std::puts("  display keys: rejected inputs");DisplayDeletionRejectionChecks();
    std::puts("  display keys: failure and stale");DisplayDeletionFailureAndStaleChecks();
}
