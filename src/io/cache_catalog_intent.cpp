#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cache_catalog_intent.hpp"
#include "cache_files_crypto.hpp"
#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

namespace mikumikustudio::cache_catalog::intent {
namespace {
using cache_files::BlobInfo;
using cache_files::WritePlan;
constexpr std::array<std::uint8_t,16> Magic{
    'M','M','S','C','a','c','h','e','I','n','t','e','n','t','0','1'};
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
static_assert(std::is_nothrow_move_assignable_v<Record>);
static_assert(std::is_nothrow_move_assignable_v<BlobInfo>);
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
}
