#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cache_catalog_intent.hpp"
#include "cache_files_crypto.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace mikumikustudio::cache_catalog::intent {
namespace {
using cache_files::BlobInfo;
using cache_files::WritePlan;
constexpr std::array<std::uint8_t,16> Magic{
    'M','M','S','C','a','c','h','e','I','n','t','e','n','t','0','1'};
constexpr std::array<std::uint8_t,16> ApprovedMagic{
    'M','M','S','C','a','c','h','e','A','p','p','r','v','0','0','1'};
bool Zero(const std::uint8_t* p,std::size_t n) noexcept {
    return std::all_of(p,p+n,[](auto v){return v==0;});
}
bool Name(const std::wstring& name) noexcept {
    if(name.size()!=37||name.compare(32,5,L".blob")!=0)return false;
    for(std::size_t i=0;i<32;++i)
        if(!((name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f')))return false;
    return true;
}
bool PlanValid(const WritePlan& p) noexcept {
    const auto kind=static_cast<std::uint32_t>(p.spec.kind);
    return Name(p.name)&&!Zero(p.spec.owner.data(),p.spec.owner.size())&&kind>=1&&kind<=7&&
        p.payloadBytes<=cache_files::MaximumPayloadBytes&&p.createdIdentity.fileBytes==0&&
        p.completedBytes==cache_files::HeaderBytes+p.payloadBytes;
}
void Put(std::uint8_t* p,std::uint64_t value,unsigned size) noexcept {
    for(unsigned i=0;i<size;++i)p[i]=static_cast<std::uint8_t>(value>>(8*i));
}
std::uint64_t Get(const std::uint8_t* p,unsigned size) noexcept {
    std::uint64_t value=0;
    for(unsigned i=0;i<size;++i)value|=std::uint64_t(p[i])<<(8*i);
    return value;
}
void PutPlan(std::uint8_t* p,const WritePlan& plan) noexcept {
    for(unsigned i=0;i<37;++i)p[i]=static_cast<std::uint8_t>(plan.name[i]);
    std::copy(plan.spec.owner.begin(),plan.spec.owner.end(),p+40);
    Put(p+56,static_cast<std::uint32_t>(plan.spec.kind),4);
    std::copy(plan.spec.key.begin(),plan.spec.key.end(),p+64);
    std::copy(plan.payloadDigest.begin(),plan.payloadDigest.end(),p+96);
    Put(p+128,plan.payloadBytes,8);Put(p+136,plan.createdIdentity.volume,4);
    Put(p+144,plan.createdIdentity.fileIndex,8);Put(p+152,plan.createdIdentity.fileBytes,8);
    Put(p+160,plan.completedBytes,8);
}
bool GetPlan(const std::uint8_t* p,WritePlan& plan) {
    if(!Zero(p+37,3)||!Zero(p+60,4)||!Zero(p+140,4))return false;
    plan.name.assign(p,p+37);
    std::copy_n(p+40,16,plan.spec.owner.begin());
    plan.spec.kind=static_cast<cache_files::Kind>(Get(p+56,4));
    std::copy_n(p+64,32,plan.spec.key.begin());std::copy_n(p+96,32,plan.payloadDigest.begin());
    plan.payloadBytes=Get(p+128,8);plan.createdIdentity.volume=static_cast<std::uint32_t>(Get(p+136,4));
    plan.createdIdentity.fileIndex=Get(p+144,8);plan.createdIdentity.fileBytes=Get(p+152,8);
    plan.completedBytes=Get(p+160,8);return PlanValid(plan);
}
bool SnapshotMatches(const WritePlan& p,const BlobInfo& b) noexcept {
    return p.name==b.name&&p.spec.owner==b.spec.owner&&p.spec.kind==b.spec.kind&&
        p.spec.key==b.spec.key&&p.payloadDigest==b.payloadDigest&&p.payloadBytes==b.payloadBytes&&
        p.createdIdentity.volume==b.identity.volume&&p.createdIdentity.fileIndex==b.identity.fileIndex&&
        p.completedBytes==b.identity.fileBytes;
}
Result Validate(const Record& r,std::vector<std::uint8_t>& publication) {
    if(Zero(r.operation.data(),r.operation.size())||!PlanValid(r.snapshot)||!PlanValid(r.commit)||
        !SnapshotMatches(r.snapshot,r.publication.snapshot)||
        r.snapshot.createdIdentity.volume!=r.commit.createdIdentity.volume||
        r.snapshot.createdIdentity.fileIndex==r.commit.createdIdentity.fileIndex||
        r.commit.spec.kind!=cache_files::Kind::CatalogCommit||r.commit.spec.owner!=codec::NamespaceOwner||
        r.commit.spec.key!=codec::CommitKey||r.commit.payloadBytes!=codec::RecordBytes)return Result::Invalid;
    const auto result=codec::Encode(r.publication,publication);
    if(result!=Result::Ok)return result;
    cache_files::crypto::Sha256 hash;cache_files::Digest digest{};
    if(!hash.Start()||!hash.Update(publication.data(),publication.size())||!hash.Finish(digest))return Result::Io;
    return digest==r.commit.payloadDigest?Result::Ok:Result::Invalid;
}
bool Add(std::uint64_t& total,std::uint64_t amount) noexcept {
    if(amount>(std::numeric_limits<std::uint64_t>::max)()-total)return false;
    total+=amount;return true;
}
bool AddRounded(std::uint64_t& total,std::uint64_t bytes,std::uint64_t unit) noexcept {
    const auto remainder=bytes%unit;
    if(remainder&&!Add(bytes,unit-remainder))return false;
    return Add(total,bytes);
}
Result ApprovalArithmetic(std::size_t payload,const PublicationApproval& approval) noexcept {
    if(payload>MaximumSnapshotBytes||!approval.allocationUnit)return Result::Invalid;
    const auto& directories=approval.directories;
    for(std::size_t i=0;i<directories.size();++i){
        if(directories[i].volume!=directories.front().volume)return Result::Invalid;
        for(std::size_t j=0;j<i;++j)
            if(directories[i].fileIndex==directories[j].fileIndex)return Result::Invalid;
    }
    std::uint64_t baseline=approval.physicalBytes;
    if(!Add(baseline,approval.legacyReservedBytes)||!Add(baseline,approval.beforeRawResidual)||
        !Add(baseline,approval.beforeMetadata))return Result::Invalid;
    std::uint64_t peak=approval.physicalBytes;
    if(!Add(peak,approval.legacyReservedBytes)||!Add(peak,approval.afterRawResidual)||
        !Add(peak,approval.afterMetadata)||
        !AddRounded(peak,cache_files::HeaderBytes+static_cast<std::uint64_t>(payload),approval.allocationUnit)||
        !AddRounded(peak,cache_files::HeaderBytes+codec::RecordBytes,approval.allocationUnit)||
        !AddRounded(peak,cache_files::HeaderBytes+ApprovedRecordBytes,approval.allocationUnit)||
        !AddRounded(peak,cache_files::HeaderBytes+ApprovedRecordBytes,approval.allocationUnit))return Result::Invalid;
    return peak==approval.approvedPeak&&
        (peak<=baseline||(approval.policyEnabled&&peak<=approval.policyLimitBytes))?
        Result::Ok:Result::Invalid;
}
// The caller has already validated the original operation, so completed sizes
// equal the sizes used by ApprovalArithmetic. No hashing or allocations here.
Result ApprovalForValidatedRecord(const Record& record,const PublicationApproval& approval) noexcept {
    const auto result=ApprovalArithmetic(static_cast<std::size_t>(record.snapshot.payloadBytes),approval);
    if(result!=Result::Ok)return result;
    for(const auto& directory:approval.directories){
        if(directory.volume!=record.snapshot.createdIdentity.volume||
            directory.volume!=record.commit.createdIdentity.volume||
            directory.fileIndex==record.snapshot.createdIdentity.fileIndex||
            directory.fileIndex==record.commit.createdIdentity.fileIndex)return Result::Invalid;
    }
    return Result::Ok;
}
void PutApproval(std::uint8_t* bytes,const PublicationApproval& approval) noexcept {
    for(std::size_t i=0;i<approval.directories.size();++i){
        auto* p=bytes+0+i*16;
        Put(p,approval.directories[i].volume,4);
        Put(p+8,approval.directories[i].fileIndex,8);
    }
    const std::array<std::uint64_t,9> values{approval.allocationUnit,approval.physicalBytes,
        approval.legacyReservedBytes,approval.beforeRawResidual,approval.beforeMetadata,
        approval.afterRawResidual,approval.afterMetadata,approval.policyLimitBytes,approval.approvedPeak};
    for(std::size_t i=0;i<values.size();++i)Put(bytes+144+i*8,values[i],8);
    Put(bytes+216,approval.policyEnabled?1:0,4);
}
bool GetApproval(const std::uint8_t* bytes,PublicationApproval& approval) noexcept {
    for(std::size_t i=0;i<approval.directories.size();++i){
        const auto* p=bytes+0+i*16;
        if(!Zero(p+4,4))return false;
        approval.directories[i].volume=static_cast<std::uint32_t>(Get(p,4));
        approval.directories[i].fileIndex=Get(p+8,8);
    }
    approval.allocationUnit=Get(bytes+144,8);
    approval.physicalBytes=Get(bytes+152,8);
    approval.legacyReservedBytes=Get(bytes+160,8);
    approval.beforeRawResidual=Get(bytes+168,8);
    approval.beforeMetadata=Get(bytes+176,8);
    approval.afterRawResidual=Get(bytes+184,8);
    approval.afterMetadata=Get(bytes+192,8);
    approval.policyLimitBytes=Get(bytes+200,8);
    approval.approvedPeak=Get(bytes+208,8);
    const auto enabled=Get(bytes+216,4);
    if(enabled>1||!Zero(bytes+220,4))return false;
    approval.policyEnabled=enabled!=0;return true;
}
static_assert(std::is_nothrow_move_assignable_v<Record>);
static_assert(std::is_nothrow_move_assignable_v<BlobInfo>);
static_assert(std::is_nothrow_move_assignable_v<ApprovedRecord>);
static_assert(static_cast<std::size_t>(ApprovalArea::Count)==9);
static_assert(32+RecordBytes==784&&784+9*16==928&&928+9*8==1000&&1008==ApprovedRecordBytes);
}
Result PlannedInfo(const WritePlan& plan,BlobInfo& output) noexcept {
    try {
        if(!PlanValid(plan))return Result::Invalid;
        BlobInfo value;value.name=plan.name;value.spec=plan.spec;value.payloadDigest=plan.payloadDigest;
        value.payloadBytes=plan.payloadBytes;value.identity=plan.createdIdentity;
        value.identity.fileBytes=plan.completedBytes;output=std::move(value);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result Encode(const Record& record,std::vector<std::uint8_t>& output) noexcept {
    try {
        std::vector<std::uint8_t> publication;
        const auto result=Validate(record,publication);if(result!=Result::Ok)return result;
        std::vector<std::uint8_t> bytes(RecordBytes,0);
        std::copy(Magic.begin(),Magic.end(),bytes.begin());Put(bytes.data()+16,1,4);
        std::copy(record.operation.begin(),record.operation.end(),bytes.begin()+24);
        std::copy(publication.begin(),publication.end(),bytes.begin()+48);
        PutPlan(bytes.data()+416,record.snapshot);PutPlan(bytes.data()+584,record.commit);
        output.swap(bytes);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result Decode(const void* bytes,std::size_t size,Record& output) noexcept {
    try {
        if(!bytes||size!=RecordBytes)return Result::Invalid;
        const auto* p=static_cast<const std::uint8_t*>(bytes);
        if(!std::equal(Magic.begin(),Magic.end(),p)||Get(p+16,4)!=1||!Zero(p+20,4)||!Zero(p+40,8))return Result::Invalid;
        Record value;std::copy_n(p+24,16,value.operation.begin());
        const auto decoded=codec::Decode(p+48,codec::RecordBytes,value.publication);
        if(decoded!=Result::Ok)return decoded;
        if(!GetPlan(p+416,value.snapshot)||!GetPlan(p+584,value.commit))return Result::Invalid;
        std::vector<std::uint8_t> publication;
        const auto result=Validate(value,publication);if(result!=Result::Ok)return result;
        output=std::move(value);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result ValidateApproval(std::size_t snapshotPayloadBytes,const PublicationApproval& approval) noexcept {
    return ApprovalArithmetic(snapshotPayloadBytes,approval);
}
Result ValidateApproval(const Record& record,const PublicationApproval& approval) noexcept {
    try {
        std::vector<std::uint8_t> publication;
        const auto result=Validate(record,publication);
        return result==Result::Ok?ApprovalForValidatedRecord(record,approval):result;
    }catch(...){return Result::Io;}
}
Result EncodeApproved(const ApprovedRecord& record,std::vector<std::uint8_t>& output) noexcept {
    try {
        std::vector<std::uint8_t> operation;
        const auto encoded=Encode(record.operation,operation);
        if(encoded!=Result::Ok)return encoded;
        const auto valid=ApprovalForValidatedRecord(record.operation,record.approval);
        if(valid!=Result::Ok)return valid;
        std::vector<std::uint8_t> bytes(ApprovedRecordBytes,0);
        std::copy(ApprovedMagic.begin(),ApprovedMagic.end(),bytes.begin());Put(bytes.data()+16,1,4);
        std::copy(operation.begin(),operation.end(),bytes.begin()+32);
        PutApproval(bytes.data()+784,record.approval);
        output.swap(bytes);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result DecodeApproved(const void* bytes,std::size_t size,ApprovedRecord& output) noexcept {
    try {
        if(!bytes||size!=ApprovedRecordBytes)return Result::Invalid;
        const auto* p=static_cast<const std::uint8_t*>(bytes);
        if(!std::equal(ApprovedMagic.begin(),ApprovedMagic.end(),p)||Get(p+16,4)!=1||!Zero(p+20,12))
            return Result::Invalid;
        ApprovedRecord candidate;
        const auto decoded=Decode(p+32,RecordBytes,candidate.operation);
        if(decoded!=Result::Ok)return decoded;
        if(!GetApproval(p+784,candidate.approval))return Result::Invalid;
        const auto valid=ApprovalForValidatedRecord(candidate.operation,candidate.approval);
        if(valid!=Result::Ok)return valid;
        output=std::move(candidate);return Result::Ok;
    }catch(...){return Result::Io;}
}
namespace {
constexpr std::array<std::uint8_t,16> CheckpointMagic{
    'M','M','S','C','a','c','h','e','C','k','A','p','0','0','0','1'};
Result CheckpointArithmetic(std::size_t count,const PublicationApproval& a) noexcept {
    const auto encoded=ApprovedCheckpointRecordSize(count);
    if(!encoded||!a.allocationUnit)return Result::Invalid;
    for(std::size_t i=0;i<a.directories.size();++i){
        if(a.directories[i].volume!=a.directories.front().volume)return Result::Invalid;
        for(std::size_t j=0;j<i;++j)if(a.directories[i].fileIndex==a.directories[j].fileIndex)return Result::Invalid;
    }
    std::uint64_t baseline=a.physicalBytes,peak=a.physicalBytes;
    if(!Add(baseline,a.legacyReservedBytes)||!Add(baseline,a.beforeRawResidual)||!Add(baseline,a.beforeMetadata)||
       !Add(peak,a.legacyReservedBytes)||!Add(peak,a.afterRawResidual)||!Add(peak,a.afterMetadata)||
       !AddRounded(peak,cache_files::HeaderBytes+encoded-ApprovedCheckpointHeaderBytes,a.allocationUnit)||
       !AddRounded(peak,cache_files::HeaderBytes+encoded,a.allocationUnit)||
       !AddRounded(peak,cache_files::HeaderBytes+encoded,a.allocationUnit))return Result::Invalid;
    return peak==a.approvedPeak&&(peak<=baseline||(a.policyEnabled&&peak<=a.policyLimitBytes))?Result::Ok:Result::Invalid;
}
Result ValidateCheckpoint(const ApprovedCheckpointRecord& r,std::vector<std::uint8_t>& payload) {
    const auto& plan=r.checkpoint;
    if(Zero(r.operation.data(),r.operation.size())||!PlanValid(plan)||
       plan.spec.kind!=cache_files::Kind::CatalogCheckpoint||plan.spec.owner!=codec::NamespaceOwner||
       plan.spec.key!=codec::CheckpointKey)return Result::Invalid;
    const auto result=codec::EncodeCheckpoint(r.publication,payload);if(result!=Result::Ok)return result;
    if(plan.payloadBytes!=payload.size()||CheckpointArithmetic(r.publication.retired.size(),r.approval)!=Result::Ok)return Result::Invalid;
    const auto distinct=[&](const BlobInfo& ref){return ref.identity.volume==plan.createdIdentity.volume&&
        ref.identity.fileIndex!=plan.createdIdentity.fileIndex;};
    if(!distinct(r.publication.state.snapshot)||!distinct(r.publication.state.previous))return Result::Invalid;
    for(const auto& retired:r.publication.retired)if(!distinct(retired))return Result::Invalid;
    for(const auto& directory:r.approval.directories)if(directory.volume!=plan.createdIdentity.volume||
        directory.fileIndex==plan.createdIdentity.fileIndex)return Result::Invalid;
    cache_files::crypto::Sha256 hash;cache_files::Digest digest{};
    if(!hash.Start()||!hash.Update(payload.data(),payload.size())||!hash.Finish(digest))return Result::Io;
    return digest==plan.payloadDigest?Result::Ok:Result::Invalid;
}
static_assert(std::is_nothrow_move_assignable_v<ApprovedCheckpointRecord>);
static_assert(48+168==216&&216+224==ApprovedCheckpointHeaderBytes);
}
Result ValidateCheckpointApproval(std::size_t count,const PublicationApproval& approval) noexcept {
    return CheckpointArithmetic(count,approval);
}
Result ValidateCheckpointApproval(const ApprovedCheckpointRecord& record) noexcept {
    try{std::vector<std::uint8_t> payload;return ValidateCheckpoint(record,payload);}catch(...){return Result::Io;}
}
Result EncodeApprovedCheckpoint(const ApprovedCheckpointRecord& record,std::vector<std::uint8_t>& output) noexcept {
    try{
        std::vector<std::uint8_t> payload;const auto valid=ValidateCheckpoint(record,payload);if(valid!=Result::Ok)return valid;
        std::vector<std::uint8_t> bytes(ApprovedCheckpointHeaderBytes+payload.size(),0);
        std::copy(CheckpointMagic.begin(),CheckpointMagic.end(),bytes.begin());Put(bytes.data()+16,1,4);
        std::copy(record.operation.begin(),record.operation.end(),bytes.begin()+24);
        PutPlan(bytes.data()+48,record.checkpoint);PutApproval(bytes.data()+216,record.approval);
        std::copy(payload.begin(),payload.end(),bytes.begin()+ApprovedCheckpointHeaderBytes);
        output.swap(bytes);return Result::Ok;
    }catch(...){return Result::Io;}
}
Result DecodeApprovedCheckpoint(const void* data,std::size_t size,ApprovedCheckpointRecord& output) noexcept {
    try{
        if(!data||size<ApprovedCheckpointRecordSize(1)||size>MaximumApprovedCheckpointBytes)return Result::Invalid;
        const auto* p=static_cast<const std::uint8_t*>(data);
        if(!std::equal(CheckpointMagic.begin(),CheckpointMagic.end(),p)||Get(p+16,4)!=1||!Zero(p+20,4)||!Zero(p+40,8))return Result::Invalid;
        ApprovedCheckpointRecord value;std::copy_n(p+24,16,value.operation.begin());
        if(!GetPlan(p+48,value.checkpoint)||!GetApproval(p+216,value.approval))return Result::Invalid;
        const auto decoded=codec::DecodeCheckpoint(p+ApprovedCheckpointHeaderBytes,size-ApprovedCheckpointHeaderBytes,value.publication);
        if(decoded!=Result::Ok)return decoded;
        std::vector<std::uint8_t> payload;const auto valid=ValidateCheckpoint(value,payload);if(valid!=Result::Ok)return valid;
        output=std::move(value);return Result::Ok;
    }catch(...){return Result::Io;}
}

}
