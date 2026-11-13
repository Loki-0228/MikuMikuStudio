#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "cache_source_budget.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string_view>
#include <utility>

namespace mikumikustudio::cache_business {
struct SourceBudgetState {
    cache_files::Directory root, video, control, pages, raw;
    cache_files::DirectoryIdentity rootIdentity{},rawIdentity{};
    cache_files::FileLock controlLock;
    cache_files::ReadLease manifestLease;
    PublishedIndex business;
    std::uint64_t allocationUnit=0;
    SourceCreditRequirement credit;
};

namespace {
using B=SourceBudgetResult;
bool Same(const cache_files::DirectoryIdentity& a,
    const cache_files::DirectoryIdentity& b) noexcept {
    return a.volume==b.volume && a.fileIndex==b.fileIndex;
}
B Map(cache_files::Result value) noexcept {
    switch(value) {
    case cache_files::Result::Ok: return B::Ok;
    case cache_files::Result::Busy: return B::Busy;
    case cache_files::Result::Io: return B::Io;
    case cache_files::Result::Limit: return B::Limit;
    default: return B::RecoveryRequired;
    }
}
B Map(cache_catalog::Result value) noexcept {
    switch(value) {
    case cache_catalog::Result::Ok: return B::Ok;
    case cache_catalog::Result::Busy: return B::Busy;
    case cache_catalog::Result::Io: return B::Io;
    case cache_catalog::Result::Limit: return B::Limit;
    default: return B::RecoveryRequired;
    }
}
B Map(ReadResult value) noexcept {
    switch(value) {
    case ReadResult::Ok: return B::Ok;
    case ReadResult::Busy: return B::Busy;
    case ReadResult::Io: return B::Io;
    case ReadResult::Limit: return B::Limit;
    case ReadResult::AllocationFailure: return B::AllocationFailure;
    default: return B::RecoveryRequired;
    }
}
B Map(Result value) noexcept {
    switch(value) {
    case Result::Ok: return B::Ok;
    case Result::Limit: return B::Limit;
    case Result::AllocationFailure: return B::AllocationFailure;
    default: return B::Invalid;
    }
}
bool Add(std::uint64_t a,std::uint64_t b,std::uint64_t& out) noexcept {
    if(b>std::numeric_limits<std::uint64_t>::max()-a) return false;
    out=a+b;
    return true;
}
bool Multiply(std::uint64_t a,std::uint64_t b,std::uint64_t& out) noexcept {
    if(a && b>std::numeric_limits<std::uint64_t>::max()/a) return false;
    out=a*b;
    return true;
}
bool Round(std::uint64_t value,std::uint64_t unit,std::uint64_t& out) noexcept {
    if(!unit) return false;
    const auto remainder=value%unit;
    return Add(value,remainder ? unit-remainder : 0,out);
}
bool FootprintBytes(const cache_catalog::WriteFootprint& footprint,
    std::uint64_t unit,std::uint64_t& out) noexcept {
    if(!footprint.count || footprint.count>footprint.files.size()) return false;
    std::uint64_t total=0;
    for(std::uint32_t i=0;i<footprint.count;++i) {
        std::uint64_t rounded=0;
        if(!Round(footprint.files[i].maximumFileBytes,unit,rounded) ||
            !Add(total,rounded,total)) return false;
    }
    out=total;
    return true;
}
bool Binding(const SourceRegistry& source,
    const cache_files::DirectoryIdentity& raw) noexcept {
    return source.directories.size()==1 &&
        source.directories.front().area==SourceArea::Video &&
        Same(source.directories.front().identity,raw);
}

B Derive(const PublishedIndex& business,const SourceRegistry& sources,
    std::uint64_t unit,bool checkpointHead,SourceCreditRequirement& output) {
    auto checked=ValidateSourcesAgainstIndex(sources,business.Value());
    if(checked!=Result::Ok) return Map(checked);
    BusinessRoot root{business.Description(),sources};
    Bytes encoded;
    checked=EncodeBusinessRoot(root,encoded);
    if(checked!=Result::Ok) return Map(checked);

    SourceCreditRequirement next;
    std::uint64_t reservedCount=0,publishableCount=0;
    for(const auto& operation:sources.operations) {
        std::uint64_t remaining=0;
        switch(operation.stage) {
        case SourceStage::Reserved: remaining=4; ++reservedCount; ++publishableCount; break;
        case SourceStage::Prepared: remaining=3; ++publishableCount; break;
        case SourceStage::Captured: remaining=2; ++publishableCount; break;
        case SourceStage::AbortPrepared:
        case SourceStage::AbortCaptured: remaining=1; break;
        default: return B::Invalid;
        }
        std::uint64_t allocation=0;
        if(!Round(operation.reservedBytes,unit,allocation) ||
            !Add(next.rawReservedBytes,operation.reservedBytes,next.rawReservedBytes) ||
            !Add(next.rawAllocationReservedBytes,allocation,next.rawAllocationReservedBytes) ||
            !Add(next.remainingPublications,remaining,next.remainingPublications)) return B::Limit;
    }

    // Encoding a Prepared/Captured operation keeps the same fixed 176-byte
    // record and name. Publishing replaces it with a 144-byte source plus its
    // owners. All possible project IDs come from the actual verified pages.
    // Starting with current bytes and allowing each independent positive growth
    // bounds every interleaving, including partially progressed cancellation.
    std::uint64_t ownerBytes=0,nameGrowth=0,ownerGrowth=0,growth=0;
    if(!Multiply(business.Value().projects.size(),sizeof(std::uint64_t),ownerBytes) ||
        !Multiply(reservedCount,53,nameGrowth)) return B::Limit;
    constexpr auto replacementSaving=source_wire::OperationFixedBytes-source_wire::SourceFixedBytes;
    const auto perSourceGrowth=ownerBytes>replacementSaving ? ownerBytes-replacementSaving : 0;
    if(!Multiply(publishableCount,perSourceGrowth,ownerGrowth) ||
        !Add(nameGrowth,ownerGrowth,growth) ||
        !Add(encoded.size(),growth,next.maximumRootBytes)) return B::Limit;

    // Hypothetical all-owner publication may hit a format cap sooner. No legal
    // future publication can exceed that cap; do not allocate a huge artificial
    // root merely to compute this maximum. Existing pages are unchanged here.
    const std::uint64_t legacyBytes=ManifestHeaderBytes+
        std::uint64_t(root.legacy.pages.size())*PageRefBytes;
    const auto formatBound=(std::min)(std::uint64_t(MaximumBusinessRootBytes),
        source_wire::RootHeaderBytes+legacyBytes+MaximumSourceRegistryBytes);
    next.maximumRootBytes=(std::min)(next.maximumRootBytes,formatBound);
    cache_catalog::WriteFootprint footprint;
    auto result=cache_catalog::ApprovedPublicationFootprint(
        static_cast<std::size_t>(next.maximumRootBytes),footprint);
    if(result!=cache_catalog::Result::Ok) return Map(result);
    if(!FootprintBytes(footprint,unit,next.publicationBytesPerStep) ||
        !Multiply(next.remainingPublications,next.publicationBytesPerStep,
            next.pendingMetadataBytes)) return B::Limit;
    result=cache_catalog::MaximumApprovedCheckpointFootprint(footprint);
    if(result!=cache_catalog::Result::Ok) return Map(result);
    if(!FootprintBytes(footprint,unit,next.checkpointBytesPerStep)) return B::Limit;
    // Large roots can hit the metadata-byte ceiling long before MaximumCommits.
    // Allow maintenance before every future publication, plus terminal standby.
    // One checkpoint followed by successful collection compacts the live chain;
    // failed/busy collection cannot authorize another publication or retry loop.
    // This chain bound is not a consumable per-step budget: the coordinator must
    // account for actual capacity and already-spent maintenance credit.
    next.checkpointPublications=next.remainingPublications+(checkpointHead ? 0 : 1);
    if(!Multiply(next.checkpointPublications,next.checkpointBytesPerStep,
        next.maintenanceMetadataBytes) ||
        !Add(next.pendingMetadataBytes,next.maintenanceMetadataBytes,next.metadataBytes)) return B::Limit;
    next.fundedMetadataBytes=next.metadataBytes;
    next.fundedCheckpointBytesPerStep=next.checkpointBytesPerStep;
    if(sources.maintenanceProtocol==1){
        std::uint64_t legacyStep=0,legacyMaintenance=0;
        result=cache_catalog::MaximumCheckpointFootprint(footprint);
        if(result!=cache_catalog::Result::Ok)return Map(result);
        if(!FootprintBytes(footprint,unit,legacyStep)||
           !Multiply(next.checkpointPublications,legacyStep,legacyMaintenance)||
           !Add(next.pendingMetadataBytes,legacyMaintenance,next.fundedMetadataBytes))return B::Limit;
        next.fundedCheckpointBytesPerStep=legacyStep;
    }
    output=next;
    return B::Ok;
}
}

SourceBudgetSnapshot::SourceBudgetSnapshot() noexcept=default;
SourceBudgetSnapshot::~SourceBudgetSnapshot()=default;
SourceBudgetSnapshot::SourceBudgetSnapshot(SourceBudgetSnapshot&&) noexcept=default;
SourceBudgetSnapshot& SourceBudgetSnapshot::operator=(SourceBudgetSnapshot&&) noexcept=default;
bool SourceBudgetSnapshot::IsValid() const noexcept { return bool(state_); }
const PublishedIndex& SourceBudgetSnapshot::Business() const noexcept {
    static const PublishedIndex empty;
    return state_ ? state_->business : empty;
}
const cache_files::DirectoryIdentity& SourceBudgetSnapshot::RootIdentity() const noexcept {
    static const cache_files::DirectoryIdentity empty;
    return state_ ? state_->rootIdentity : empty;
}
const cache_files::DirectoryIdentity& SourceBudgetSnapshot::RawIdentity() const noexcept {
    static const cache_files::DirectoryIdentity empty;
    return state_ ? state_->rawIdentity : empty;
}
const SourceCreditRequirement& SourceBudgetSnapshot::Credit() const noexcept {
    static const SourceCreditRequirement empty;
    return state_ ? state_->credit : empty;
}
SourceBudgetResult SourceBudgetSnapshot::Project(const SourceRegistry& sources,
    SourceCreditRequirement& output) const noexcept {
    if(!state_) return B::Invalid;
    if(sources.maintenanceProtocol!=1&&sources.maintenanceProtocol!=2)return B::Invalid;
    if(!Binding(sources,state_->rawIdentity) ||
        sources.directories.front().id!=state_->business.Sources().directories.front().id)
        return B::Invalid;
    try {
        auto proposed=sources;
        proposed.maintenanceProtocol=2;
        return Derive(state_->business,proposed,state_->allocationUnit,false,output);
    } catch(const std::bad_alloc&) { return B::AllocationFailure; }
    catch(...) { return B::Io; }
}

SourceBudgetResult ReadSourceBudget(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,SourceBudgetSnapshot& output) noexcept {
    return SourceBudgetSnapshot::ReadImpl(managerRoot,allocationUnit,output,false);
}
SourceBudgetResult ReadSourceBudgetLocked(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,SourceBudgetSnapshot& output) noexcept {
    return SourceBudgetSnapshot::ReadImpl(managerRoot,allocationUnit,output,true);
}
SourceBudgetResult SourceBudgetSnapshot::ReadImpl(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,SourceBudgetSnapshot& output,bool retainControlLock,
    const cache_catalog::WritePlan* recovery) noexcept {
    if(!managerRoot.IsValid() || !allocationUnit) return B::Invalid;
    if(recovery&&(!recovery->IsValid()||recovery->Kind()!=cache_catalog::WriteKind::Recovery||
        recovery->IsConsumed()||recovery->Recovery()==cache_catalog::RecoveryAction::None))return B::Invalid;
    if(recovery&&!recovery->RecoveryTarget().IsValid())return B::RecoveryRequired;
    try {
        auto candidate=std::make_unique<SourceBudgetState>();
        auto result=managerRoot.Share(candidate->root);
        if(result!=cache_files::Result::Ok) return Map(result);
        result=managerRoot.Child(L"Video",false,candidate->video);
        if(result==cache_files::Result::Missing) return recovery ? B::RecoveryRequired : B::Absent;
        if(result!=cache_files::Result::Ok) return Map(result);
        result=candidate->video.Child(L"control",false,candidate->control);
        if(result!=cache_files::Result::Ok) return Map(result);
        result=candidate->video.Child(L"pages",false,candidate->pages);
        if(result!=cache_files::Result::Ok) return Map(result);
        result=candidate->video.Child(L"raw",false,candidate->raw);
        if(result!=cache_files::Result::Ok) return Map(result);
        cache_files::Directory catalogDirectory;
        result=candidate->control.Child(L"catalog",false,catalogDirectory);
        if(result!=cache_files::Result::Ok) return Map(result);

        std::array<cache_files::DirectoryIdentity,6> identities{};
        const cache_files::Directory* directories[]={&candidate->root,&candidate->video,
            &candidate->control,&candidate->pages,&candidate->raw,&catalogDirectory};
        for(std::size_t i=0;i<identities.size();++i) {
            result=directories[i]->GetIdentity(identities[i]);
            if(result!=cache_files::Result::Ok) return Map(result);
            for(std::size_t j=0;j<i;++j)
                if(Same(identities[i],identities[j])) return B::RecoveryRequired;
        }
        candidate->rootIdentity=identities[0];candidate->rawIdentity=identities[4];
        cache_files::FileLock controlLock;
        cache_catalog::Snapshot snapshot;
        if(recovery){
            if(!Same(identities[5],recovery->RootIdentity()))return B::RecoveryRequired;
            snapshot=recovery->RecoveryTarget();
        }else{
            result=candidate->control.LockExisting(0,controlLock);
            if(result!=cache_files::Result::Ok) return Map(result);
            cache_catalog::Store catalog(std::move(catalogDirectory));
            const auto read=catalog.ReadPinnedExistingLocks(snapshot,candidate->manifestLease);
            if(read!=cache_catalog::Result::Ok) return Map(read);
        }
        const auto loaded=LoadPublishedIndex(snapshot,candidate->pages,candidate->business);
        if(loaded!=ReadResult::Ok) return Map(loaded);
        if(!Binding(candidate->business.Sources(),candidate->rawIdentity))
            return B::RecoveryRequired;
        candidate->allocationUnit=allocationUnit;
        const auto derived=Derive(candidate->business,candidate->business.Sources(),
            allocationUnit,snapshot.IsCheckpoint(),candidate->credit);
        if(derived!=B::Ok) return derived==B::Invalid ? B::RecoveryRequired : derived;
        if(retainControlLock)candidate->controlLock=std::move(controlLock);
        output.state_.swap(candidate);
        return B::Ok;
    } catch(const std::bad_alloc&) { return B::AllocationFailure; }
    catch(...) { return B::Io; }
}

SourceBudgetResult ReadRecoverySourceBudget(const cache_files::Directory& managerRoot,
    std::uint64_t allocationUnit,const cache_catalog::WritePlan& recovery,
    SourceBudgetSnapshot& output) noexcept {
    return SourceBudgetSnapshot::ReadImpl(managerRoot,allocationUnit,output,false,&recovery);
}

SourceBudgetResult ProjectRawSourceBudget(const SourceBudgetSnapshot& facts,
    const cache_files::InventorySnapshot& inventory,RawSourceBudgetProjection& output) noexcept {
    if(!facts.state_)return B::Invalid;
    return ProjectRawSourceBudget(facts,facts.state_->business.Sources(),inventory,output);
}

SourceBudgetResult ProjectRawSourceBudget(const SourceBudgetSnapshot& facts,
    const SourceRegistry& proposed,const cache_files::InventorySnapshot& inventory,
    RawSourceBudgetProjection& output) noexcept {
    if(!facts.state_||!inventory.IsValid())return B::Invalid;
    const auto& state=*facts.state_;
    if(!Same(state.rootIdentity,inventory.RootIdentity()))return B::RecoveryRequired;
    SourceCreditRequirement proposedCredit;
    const auto validated=facts.Project(proposed,proposedCredit);
    if(validated!=B::Ok)return validated;
    try {
        // Compare the real namespace spelling without filesystem/path parsing.
        // Inventory owns every view until this pure function returns.
        struct Less {
            bool operator()(std::wstring_view a,std::wstring_view b) const {
                if(a.size()>INT_MAX||b.size()>INT_MAX)throw B::Limit;
                const auto order=CompareStringOrdinal(a.data(),static_cast<int>(a.size()),
                    b.data(),static_cast<int>(b.size()),TRUE);
                if(!order)throw B::Io;
                return order==CSTR_LESS_THAN;
            }
        };
        std::map<std::wstring_view,const cache_files::InventoryEntry*,Less> names;
        for(const auto& entry:inventory.Entries())
            if(!names.emplace(std::wstring_view(entry.relativePath),&entry).second)return B::RecoveryRequired;
        const auto raw=names.find(L"Video\\raw");
        if(raw==names.end())return B::RecoveryRequired;
        const auto& directory=raw->second->observed;
        if(directory.kind!=cache_files::ObservationKind::Directory||directory.deletePending||
           directory.identity.volume!=state.rawIdentity.volume||
           directory.identity.fileIndex!=state.rawIdentity.fileIndex)return B::RecoveryRequired;

        RawSourceBudgetProjection candidate;
        const auto& operations=proposed.operations;
        candidate.deductions.reserve(operations.size());
        using Physical=std::pair<std::uint32_t,std::uint64_t>;
        std::set<Physical> claimed;
        for(const auto& operation:operations) {
            std::uint64_t promised=0;
            if(!Round(operation.reservedBytes,state.allocationUnit,promised)||
               !Add(candidate.promisedBytes,promised,candidate.promisedBytes))return B::Limit;
            std::uint64_t deducted=0;
            if(operation.stage!=SourceStage::Reserved) {
                const auto& expected=operation.plan.createdIdentity;
                if(!claimed.emplace(expected.volume,expected.fileIndex).second)return B::RecoveryRequired;
                const std::wstring path=L"Video\\raw\\"+operation.plan.name;
                const auto found=names.find(path);
                if(found!=names.end()) {
                    const auto& observed=found->second->observed;
                    if(observed.kind!=cache_files::ObservationKind::File||observed.links!=1||observed.deletePending||
                       observed.identity.volume!=expected.volume||observed.identity.fileIndex!=expected.fileIndex||
                       observed.identity.fileBytes>operation.reservedBytes||observed.allocationBytes>promised)
                        return B::RecoveryRequired;
                    if((operation.stage==SourceStage::Captured||operation.stage==SourceStage::AbortCaptured)&&
                       observed.identity.fileBytes!=operation.reservedBytes)return B::RecoveryRequired;
                    deducted=(std::max)(observed.identity.fileBytes,observed.allocationBytes);
                    // Bounds above ensure subtraction cannot underflow. ADS is
                    // intentionally absent from this default-stream deduction.
                    if(deducted>promised)return B::RecoveryRequired;
                    candidate.deductions.push_back({operation.id,observed.identity,deducted});
                }
            }
            if(!Add(candidate.deductedBytes,deducted,candidate.deductedBytes)||
               !Add(candidate.residualBytes,promised-deducted,candidate.residualBytes))return B::Limit;
        }
        if(candidate.promisedBytes!=proposedCredit.rawAllocationReservedBytes)return B::RecoveryRequired;
        using std::swap;
        swap(output.promisedBytes,candidate.promisedBytes);
        swap(output.deductedBytes,candidate.deductedBytes);
        swap(output.residualBytes,candidate.residualBytes);
        output.deductions.swap(candidate.deductions);
        return B::Ok;
    }catch(const B failure){return failure;}
    catch(const std::bad_alloc&){return B::AllocationFailure;}
    catch(...){return B::Io;}
}
}
