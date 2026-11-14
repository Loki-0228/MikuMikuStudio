// Portable SAVE-02 policy/capture tests. See tests/README.md for the command.
#include "io/effect_pair_completion.hpp"
#include "io/effect_pair_transaction.hpp"

#include <openssl/evp.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace save = mikumikustudio::effect_save;
namespace tx = save::transaction;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::cerr << "FAIL " << what << '\n'; std::exit(1); }
}
template<class F> bool Refuses(F operation) {
    try { operation(); } catch (const std::exception&) { return true; }
    return false;
}

void Move(tx::Observation& disk, const tx::Move& move) {
    auto& active=disk.active[move.member];
    auto& staged=disk.staged[move.member];
    auto& old=disk.displaced[move.member];
    const auto rename=[](std::string& from,std::string& to) {
        Check(!from.empty() && to.empty(),"rename has verified source and never overwrites");
        from.swap(to);
    };
    switch (move.action) {
    case tx::Action::DisplacePrevious: rename(active,old);break;
    case tx::Action::InstallFresh: rename(staged,active);break;
    case tx::Action::RemoveFresh: active.clear();break;
    case tx::Action::RestorePrevious: rename(old,active);break;
    }
}
void Apply(tx::Observation& disk,const tx::Plan& plan) {
    for (const auto& move : plan.moves) Move(disk,move);
}
void VerifyComplete(const tx::Generation& generation,const tx::Observation& disk,bool forward) {
    const auto result=tx::Resolve(generation,disk,forward);
    Check(result.forward==forward && result.moves.empty(),"repeated recovery is idempotent");
    Check(disk.active==(forward?generation.fresh:generation.previous),"exact whole generation active");
}

void EveryInterruption(const tx::Generation& generation) {
    const tx::Observation initial{generation.previous,generation.fresh,{}};
    auto before=initial;
    Apply(before,tx::Resolve(generation,before,false));
    VerifyComplete(generation,before,false);
    const auto original=tx::Resolve(generation,initial,true);
    for (std::size_t point=0;point<=original.moves.size();++point) {
        auto disk=initial;
        for (std::size_t i=0;i<point;++i) Move(disk,original.moves[i]);
        const auto recovery=tx::Resolve(generation,disk,true);
        Check(recovery.forward,"durable forward recovery chooses complete fresh generation");
        Apply(disk,recovery);
        VerifyComplete(generation,disk,true);
        for (std::size_t i=0;i<tx::MemberCount;++i) {
            disk.displaced[i].clear();
            disk.staged[i].clear();
            VerifyComplete(generation,disk,true);
        }
    }
}

void FallbackAndInterference() {
    const tx::Generation generation{{"new PMM","new EMM","new JSON"},{"old PMM","old EMM","old JSON"},true};
    const tx::Observation initial{generation.previous,generation.fresh,{}};
    const auto forward=tx::Resolve(generation,initial,true);
    Check(forward.moves[0].member==tx::Effect && forward.moves.back().member==tx::Main,
        "EMM publishes before PMM becomes the discoverable entry");
    auto partial=initial;
    for (std::size_t i=0;i<4;++i) Move(partial,forward.moves[i]);
    partial.staged[tx::Main].clear(); // New EMM and JSON, but fresh PMM vanished.
    const auto rollback=tx::Resolve(generation,partial,true);
    Check(!rollback.forward && rollback.moves.size()==4,"missing fresh bytes trigger complete old recovery");
    for (std::size_t i=0;i<=rollback.moves.size();++i) {
        auto disk=partial;
        for (std::size_t j=0;j<i;++j) Move(disk,rollback.moves[j]);
        Apply(disk,tx::Resolve(generation,disk,false));
        VerifyComplete(generation,disk,false);
    }
    auto missingOld=partial;
    missingOld.displaced[tx::Effect].clear();
    Check(Refuses([&]{tx::Resolve(generation,missingOld,true);}),"no complete generation preserves split as pending");
    auto external=partial;
    external.active[tx::Main]="late external PMM";
    Check(Refuses([&]{tx::Resolve(generation,external,true);}),"external PMM conflict is not a missing member");
    external=initial;
    external.staged[tx::Effect]="corrupt staged EMM";
    Check(Refuses([&]{tx::Resolve(generation,external,true);}),"wrong stage hash fails before any public mutation");
    const tx::Generation create{generation.fresh,{},true};
    tx::Observation orphan{{},create.fresh,{}};
    orphan.active[tx::Effect]="external orphan EMM";
    Check(Refuses([&]{tx::Resolve(create,orphan,true);}),"do not rotate or remove a late orphan companion");
    orphan.active[tx::Effect]=create.fresh[tx::Effect];
    Check(Refuses([&]{tx::Resolve(create,orphan,false);}),"even byte-identical external arrivals are not ours");
    tx::Observation after{generation.fresh,{},{}};
    Check(Refuses([&]{tx::Resolve(generation,after,false);}),"durable rollback does not flip forward");
}

bool Sha256(save::EffectBytes bytes,save::EffectDigest& digest) {
    unsigned len=0;
    return EVP_Digest(bytes.data,bytes.size,digest.data(),&len,EVP_sha256(),nullptr)==1 && len==digest.size();
}
save::ProviderReceipt EchoFixtureReceipt(const save::PublishedPair& pair) {
    return {pair.request.requestId,pair.request.identity,pair.request.pmmPath,pair.request.emmPath,
        pair.pmmSha256,pair.emmSha256};
}
void ReceiptAndLiveState() {
    save::Request r{93,{1,61,1,6,2,3,{31}},L"C:\\scene.pmm",L"C:\\scene.emm",save::SaveKind::Manual,true};
    auto live=save::LiveIdentity{r.identity,true};
    std::string assignment="[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = captured.pmx\r\n"
        "[Effect]\r\nPmd1 = captured.fx\r\n";
    save::CaptureCallbacks callbacks;
    callbacks.observe=[&]{return std::optional<save::CaptureIdentity>(live.identity);};
    callbacks.freeze=[&](const auto&,save::CaptureSink& sink) {
        Check(sink.Begin(assignment.size()) && sink.Append(assignment.data(),assignment.size()),"fixture freezes bytes");
        return save::FreezeResult{save::FreezeStatus::Frozen,live.identity};
    };
    callbacks.validate=[&](const auto&,save::EffectBytes bytes) {
        return std::string_view(reinterpret_cast<const char*>(bytes.data),bytes.size)==assignment;
    };
    callbacks.sha256=Sha256;
    save::CaptureAdmission admission;
    const auto captured=admission.Capture(r,1024,callbacks);
    Check(captured.status==save::CaptureStatus::Captured && captured.snapshot,"fixture captured and hashed");
    const auto& frozen=*captured.snapshot;
    const save::PublishedPair proof{r,std::string(64,'1'),frozen.Digest()};
    assignment="new model and assignment";
    live.identity.sceneRevision=71;
    live.identity.effectRevision=4;
    live.identity.objectIds.push_back(45);
    Check(std::string_view(reinterpret_cast<const char*>(frozen.Bytes().data),frozen.Bytes().size)
        .find("captured.fx")!=std::string_view::npos,"new live effect does not change immutable EMM");
    const auto observe=[&]{ return live; };
    auto receipt=EchoFixtureReceipt(proof);
    const auto acknowledge=[&](const auto&){ return std::optional<save::ProviderReceipt>(receipt); };
    for (unsigned mismatch=0;mismatch<7;++mismatch) {
        save::CompletionTracker completion(r);
        Check(completion.StartWrite(r.requestId),"enter the tracked write once");
        receipt=EchoFixtureReceipt(proof);
        switch (mismatch) {
        case 0: receipt.requestId++;break;
        case 1: receipt.identity.effectRevision++;break;
        case 2: receipt.pmmPath+=L"1";break;
        case 3: receipt.emmPath+=L"1";break;
        case 4: receipt.pmmSha256[0]='2';break;
        case 5: receipt.emmSha256[0]^=1;break;
        case 6: break;
        }
        const bool done=save::CompleteFrozenPublication(completion,frozen,proof,observe,acknowledge);
        Check(done==(mismatch==6) && completion.DiskCommitted(),"only receipt bound to exact publication succeeds");
        Check(!completion.AllowsClearCapturedDirty(live.identity,true) &&
            !completion.AllowsCloseAfterSave(live.identity,true),"a later edit always stays dirty and open");
        Check(!save::CompleteFrozenPublication(completion,frozen,proof,observe,acknowledge),"duplicate receipt is refused");
    }
    save::CompletionTracker retired(r);
    Check(retired.StartWrite(r.requestId),"begin provider-retirement fixture");
    const auto retire=[&](const auto&) {
        live.providerAvailable=false;
        return std::optional<save::ProviderReceipt>(EchoFixtureReceipt(proof));
    };
    Check(!save::CompleteFrozenPublication(retired,frozen,proof,observe,retire) &&
        retired.GetStage()==save::Stage::CommittedProviderRetired,"provider retirement during acknowledgement is fail-closed");
    live={r.identity,true};
    save::CompletionTracker exact(r);
    Check(exact.StartWrite(r.requestId) &&
        save::CompleteFrozenPublication(exact,frozen,proof,observe,acknowledge) &&
        exact.AllowsCloseAfterSave(live.identity,true),"only matching confirmed unchanged manual save can close");
}
}

int main() {
    EveryInterruption({{"new PMM","new EMM","new JSON"},{"old PMM","old EMM","old JSON"},true});
    EveryInterruption({{"new PMM","new EMM","new JSON"},{},true});
    EveryInterruption({{"new PMM","unchanged EMM",{}},{"old PMM","unchanged EMM",{}},false});
    EveryInterruption({{"new PMM","new EMM",{}},{"old PMM",{},{}},false});
    FallbackAndInterference();
    ReceiptAndLiveState();
    std::cout << "PASS SAVE-02 transaction policy and request-bound receipts: " << checks << " checks\n";
}
