// MAINTENANCE: regression-keep. Pure codec checks; no disk identity authorization.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include "../src/io/cache_catalog_intent.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

#pragma comment(lib,"bcrypt.lib")
namespace {
namespace files=mikumikustudio::cache_files;
namespace codec=mikumikustudio::cache_catalog::codec;
namespace intent=mikumikustudio::cache_catalog::intent;
using Bytes=std::vector<std::uint8_t>;
using Result=intent::Result;
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void Put(Bytes& bytes,std::size_t offset,std::uint64_t value,unsigned size){
    for(unsigned i=0;i<size;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(8*i));
}
// Independent CNG invocation; never call the production SHA wrapper or wire encoder.
files::Digest Hash(const Bytes& bytes){
    struct State {
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;Bytes object;
        ~State(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    } state;
    Check(BCryptOpenAlgorithmProvider(&state.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"test SHA provider");
    ULONG size=0,got=0;
    Check(BCryptGetProperty(state.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof size,&got,0)>=0&&
        got==sizeof size&&size>0&&size<1024*1024,"test SHA object length");
    state.object.resize(size);
    Check(BCryptCreateHash(state.algorithm,&state.hash,state.object.data(),size,nullptr,0,0)>=0,"test SHA object");
    if(!bytes.empty())Check(BCryptHashData(state.hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0,"test SHA input");
    files::Digest digest{};Check(BCryptFinishHash(state.hash,digest.data(),32,0)>=0,"test SHA digest");return digest;
}
files::Owner NamespaceOwner(){
    files::Owner owner{};const char text[]="MMSCatalogHead01";std::copy_n(text,16,owner.begin());return owner;
}
bool Equal(const files::BlobInfo& a,const files::BlobInfo& b){
    return a.name==b.name&&a.spec.owner==b.spec.owner&&a.spec.kind==b.spec.kind&&a.spec.key==b.spec.key&&
        a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.identity.volume==b.identity.volume&&
        a.identity.fileIndex==b.identity.fileIndex&&a.identity.fileBytes==b.identity.fileBytes;
}
bool Equal(const files::WritePlan& a,const files::WritePlan& b){
    return a.name==b.name&&a.spec.owner==b.spec.owner&&a.spec.kind==b.spec.kind&&a.spec.key==b.spec.key&&
        a.payloadDigest==b.payloadDigest&&a.payloadBytes==b.payloadBytes&&a.createdIdentity.volume==b.createdIdentity.volume&&
        a.createdIdentity.fileIndex==b.createdIdentity.fileIndex&&a.createdIdentity.fileBytes==b.createdIdentity.fileBytes&&
        a.completedBytes==b.completedBytes;
}
bool Equal(const intent::Record& a,const intent::Record& b){
    return a.operation==b.operation&&a.publication.owner==b.publication.owner&&a.publication.revision==b.publication.revision&&
        Equal(a.publication.snapshot,b.publication.snapshot)&&Equal(a.publication.previous,b.publication.previous)&&
        Equal(a.snapshot,b.snapshot)&&Equal(a.commit,b.commit);
}
void Ref(Bytes& b,std::size_t at,const files::BlobInfo& r){
    for(unsigned i=0;i<37;++i)b.at(at+i)=static_cast<std::uint8_t>(r.name.at(i));
    Put(b,at+40,static_cast<std::uint32_t>(r.spec.kind),4);
    std::copy(r.spec.owner.begin(),r.spec.owner.end(),b.begin()+at+48);
    std::copy(r.spec.key.begin(),r.spec.key.end(),b.begin()+at+64);
    std::copy(r.payloadDigest.begin(),r.payloadDigest.end(),b.begin()+at+96);
    Put(b,at+128,r.payloadBytes,8);Put(b,at+136,r.identity.volume,4);
    Put(b,at+144,r.identity.fileIndex,8);Put(b,at+152,r.identity.fileBytes,8);
}
Bytes Publication(const codec::Record& r){
    Bytes b(368,0);const char magic[]="MMSCacheHead0001";std::copy_n(magic,16,b.begin());
    Put(b,16,1,4);Put(b,24,r.revision,8);std::copy(r.owner.begin(),r.owner.end(),b.begin()+32);
    Ref(b,48,r.snapshot);if(r.revision>1)Ref(b,208,r.previous);return b;
}
void Plan(Bytes& b,std::size_t at,const files::WritePlan& p){
    for(unsigned i=0;i<37;++i)b.at(at+i)=static_cast<std::uint8_t>(p.name.at(i));
    std::copy(p.spec.owner.begin(),p.spec.owner.end(),b.begin()+at+40);
    Put(b,at+56,static_cast<std::uint32_t>(p.spec.kind),4);
    std::copy(p.spec.key.begin(),p.spec.key.end(),b.begin()+at+64);
    std::copy(p.payloadDigest.begin(),p.payloadDigest.end(),b.begin()+at+96);
    Put(b,at+128,p.payloadBytes,8);Put(b,at+136,p.createdIdentity.volume,4);
    Put(b,at+144,p.createdIdentity.fileIndex,8);Put(b,at+152,p.createdIdentity.fileBytes,8);
    Put(b,at+160,p.completedBytes,8);
}
Bytes Golden(const intent::Record& r){
    Bytes b(752,0);const char magic[]="MMSCacheIntent01";std::copy_n(magic,16,b.begin());
    Put(b,16,1,4);std::copy(r.operation.begin(),r.operation.end(),b.begin()+24);
    const auto publication=Publication(r.publication);std::copy(publication.begin(),publication.end(),b.begin()+48);
    Plan(b,416,r.snapshot);Plan(b,584,r.commit);return b;
}
intent::Record Example(std::uint64_t revision){
    intent::Record r;for(unsigned i=0;i<16;++i){r.operation[i]=static_cast<std::uint8_t>(32+i);r.publication.owner[i]=static_cast<std::uint8_t>(i+1);}
    r.publication.revision=revision;
    auto& snapshot=r.publication.snapshot;
    snapshot.name=L"0123456789abcdef0123456789abcdef.blob";snapshot.spec.owner=r.publication.owner;
    snapshot.spec.kind=files::Kind::CatalogSnapshot;snapshot.spec.key[0]=1;
    snapshot.payloadBytes=3;snapshot.payloadDigest=Hash(Bytes{'a','b','c'});
    snapshot.identity={0x12345678,0x0102030405060708ull,131};
    if(revision>1){
        r.publication.previous=snapshot;auto& previous=r.publication.previous;
        previous.name=L"22222222222222222222222222222222.blob";
        previous.spec.owner=NamespaceOwner();previous.spec.kind=files::Kind::CatalogCommit;previous.spec.key[0]=2;
        previous.payloadBytes=368;previous.identity.fileBytes=496;previous.identity.fileIndex=51;
    }
    r.snapshot.name=snapshot.name;r.snapshot.spec=snapshot.spec;r.snapshot.payloadDigest=snapshot.payloadDigest;
    r.snapshot.payloadBytes=snapshot.payloadBytes;r.snapshot.createdIdentity=snapshot.identity;
    r.snapshot.createdIdentity.fileBytes=0;r.snapshot.completedBytes=131;
    auto& commit=r.commit;commit.name=L"fedcba9876543210fedcba9876543210.blob";
    commit.spec.owner=NamespaceOwner();commit.spec.kind=files::Kind::CatalogCommit;commit.spec.key[0]=2;
    commit.payloadBytes=368;commit.createdIdentity={0x12345678,0x0807060504030201ull,0};commit.completedBytes=496;
    commit.payloadDigest=Hash(Publication(r.publication));return r;
}

namespace catalog=mikumikustudio::cache_catalog;
std::uint64_t Rounded(std::uint64_t n,std::uint64_t unit){return (n/unit+(n%unit!=0))*unit;}
void Budget(intent::ApprovedCheckpointRecord& r){
    auto& a=r.approval;const auto n=376+r.publication.retired.size()*160;
    a.approvedPeak=a.physicalBytes+a.legacyReservedBytes+a.afterRawResidual+a.afterMetadata+
        Rounded(128+n,a.allocationUnit)+2*Rounded(128+440+n,a.allocationUnit);
}
intent::ApprovedCheckpointRecord Checkpoint(){
    const auto old=Example(2);intent::ApprovedCheckpointRecord r;r.operation=old.operation;
    r.publication.state=old.publication;r.publication.retired.push_back(old.publication.previous);
    r.checkpoint=old.commit;r.checkpoint.createdIdentity.fileIndex=900;
    r.checkpoint.spec.kind=files::Kind::CatalogCheckpoint;r.checkpoint.spec.key=files::Digest{3};
    Bytes payload;Check(codec::EncodeCheckpoint(r.publication,payload)==Result::Ok,"independent valid checkpoint payload");
    r.checkpoint.payloadBytes=payload.size();r.checkpoint.completedBytes=128+payload.size();r.checkpoint.payloadDigest=Hash(payload);
    auto& a=r.approval;for(unsigned i=0;i<9;++i)a.directories[i]={r.checkpoint.createdIdentity.volume,100u+i};
    a.allocationUnit=4096;a.physicalBytes=8192;a.afterRawResidual=1000;a.afterMetadata=4000;
    a.policyEnabled=true;a.policyLimitBytes=64*1024*1024;Budget(r);return r;
}
Bytes CheckpointGolden(const intent::ApprovedCheckpointRecord& r){
    Bytes payload;Check(codec::EncodeCheckpoint(r.publication,payload)==Result::Ok,"encode separately validated checkpoint payload");
    Bytes bytes(440+payload.size(),0);const char magic[]="MMSCacheCkAp0001";std::copy_n(magic,16,bytes.begin());
    Put(bytes,16,1,4);std::copy(r.operation.begin(),r.operation.end(),bytes.begin()+24);Plan(bytes,48,r.checkpoint);
    for(unsigned i=0;i<9;++i){Put(bytes,216+16*i,r.approval.directories[i].volume,4);Put(bytes,224+16*i,r.approval.directories[i].fileIndex,8);}
    const auto&a=r.approval;const std::array<std::uint64_t,9> values{a.allocationUnit,a.physicalBytes,a.legacyReservedBytes,a.beforeRawResidual,a.beforeMetadata,a.afterRawResidual,a.afterMetadata,a.policyLimitBytes,a.approvedPeak};
    for(unsigned i=0;i<9;++i)Put(bytes,360+8*i,values[i],8);Put(bytes,432,a.policyEnabled?1:0,4);
    std::copy(payload.begin(),payload.end(),bytes.begin()+440);return bytes;
}
void Tests(){
    Check(intent::ApprovedCheckpointRecordSize(0)==0&&intent::ApprovedCheckpointRecordSize(193)==0&&
        intent::ApprovedCheckpointRecordSize(1)==976&&intent::ApprovedCheckpointRecordSize(192)==31536&&
        intent::MaximumOperationBytes==31536,"literal bounded checkpoint format sizes");
    const auto valid=Checkpoint();const auto golden=CheckpointGolden(valid);Bytes encoded{7,8};
    Check(intent::EncodeApprovedCheckpoint(valid,encoded)==Result::Ok&&encoded==golden,"approved checkpoint matches independently assembled fixed offsets");
    intent::ApprovedCheckpointRecord decoded;
    Check(intent::DecodeApprovedCheckpoint(golden.data(),golden.size(),decoded)==Result::Ok,"decode valid checkpoint approval");
    Bytes roundtrip;Check(intent::EncodeApprovedCheckpoint(decoded,roundtrip)==Result::Ok&&roundtrip==golden,"decoded retirement facts and approval round-trip canonically");
    Check(intent::ValidateCheckpointApproval(1,valid.approval)==Result::Ok,"precreation approval needs no candidate files");
    const auto rejectBytes=[&](const Bytes& bad){auto output=valid;Bytes prior;Check(intent::EncodeApprovedCheckpoint(output,prior)==Result::Ok,"capture output sentinel");
        Check(intent::DecodeApprovedCheckpoint(bad.data(),bad.size(),output)!=Result::Ok,"reject malformed approved checkpoint bytes");
        Bytes after;Check(intent::EncodeApprovedCheckpoint(output,after)==Result::Ok&&after==prior,"failed decode preserves complete output");};
    for(std::size_t n=0;n<golden.size();++n)rejectBytes(Bytes(golden.begin(),golden.begin()+n));
    auto extra=golden;extra.push_back(0);rejectBytes(extra);
    for(auto offset:{0u,16u,20u,40u,85u,108u,188u,220u,436u}){auto bytes=golden;bytes[offset]^=1;rejectBytes(bytes);}
    auto enabled=golden;Put(enabled,432,2,4);rejectBytes(enabled);
    auto digest=golden;digest[48+96]^=1;rejectBytes(digest);
    auto retired=golden;retired.back()^=1;rejectBytes(retired);
    const auto reject=[&](const intent::ApprovedCheckpointRecord& r){Bytes out{5,6,7};Check(intent::EncodeApprovedCheckpoint(r,out)!=Result::Ok&&out==Bytes({5,6,7}),"invalid record preserves encoder output");};
    auto bad=valid;bad.operation={};reject(bad);
    bad=valid;bad.checkpoint.createdIdentity.fileBytes=1;reject(bad);
    bad=valid;bad.checkpoint.createdIdentity.fileIndex=bad.publication.state.snapshot.identity.fileIndex;reject(bad);
    bad=valid;bad.checkpoint.createdIdentity.fileIndex=bad.publication.retired[0].identity.fileIndex;reject(bad);
    bad=valid;bad.checkpoint.createdIdentity.fileIndex=bad.approval.directories[0].fileIndex;reject(bad);
    bad=valid;bad.checkpoint.createdIdentity.volume^=1;reject(bad);
    bad=valid;bad.checkpoint.spec.kind=files::Kind::CatalogCommit;reject(bad);
    bad=valid;bad.checkpoint.spec.key=files::Digest{2};reject(bad);
    bad=valid;bad.approval.directories[1]=bad.approval.directories[0];reject(bad);
    bad=valid;bad.approval.directories[0].volume^=1;reject(bad);
    bad=valid;bad.approval.allocationUnit=0;reject(bad);
    bad=valid;++bad.approval.approvedPeak;reject(bad);
    bad=valid;bad.approval.policyLimitBytes=bad.approval.approvedPeak-1;reject(bad);
    bad=valid;bad.approval.physicalBytes=UINT64_MAX;reject(bad);
    bad=valid;bad.approval.allocationUnit=UINT64_MAX;reject(bad);
    bad=valid;bad.approval.policyEnabled=false;reject(bad);
    bad=valid;bad.approval.beforeMetadata=bad.approval.approvedPeak;bad.approval.policyEnabled=false;
    Check(intent::ValidateCheckpointApproval(1,bad.approval)==Result::Ok&&intent::EncodeApprovedCheckpoint(bad,encoded)==Result::Ok,"prepaid maintenance remains legal when new allocation disabled");
    auto zero=valid;zero.checkpoint.createdIdentity.volume=0;zero.publication.state.snapshot.identity.volume=0;zero.publication.state.previous.identity.volume=0;zero.publication.retired[0].identity.volume=0;
    for(auto& dir:zero.approval.directories)dir.volume=0;Bytes payload;Check(codec::EncodeCheckpoint(zero.publication,payload)==Result::Ok,"zero volume checkpoint payload");zero.checkpoint.payloadDigest=Hash(payload);
    Check(intent::EncodeApprovedCheckpoint(zero,encoded)==Result::Ok,"zero volume serial remains valid");
    const auto old=Example(2);Check(intent::Encode(old,encoded)==Result::Ok&&encoded==Golden(old)&&encoded.size()==752,"legacy ordinary wire unchanged");
    intent::ApprovedRecord approved;approved.operation=old;approved.approval=valid.approval;
    auto&a=approved.approval;a.approvedPeak=a.physicalBytes+a.legacyReservedBytes+a.afterRawResidual+a.afterMetadata+4*4096;
    Check(intent::EncodeApproved(approved,encoded)==Result::Ok&&encoded.size()==1008,"old approved record remains exact 1008 bytes");
    intent::ApprovedRecord oldDecoded;Check(intent::DecodeApproved(encoded.data(),encoded.size(),oldDecoded)==Result::Ok&&Equal(oldDecoded.operation,old),"old approved protocol decodes after shared offset helper change");
    Bytes oldRoundtrip;Check(intent::EncodeApproved(oldDecoded,oldRoundtrip)==Result::Ok&&oldRoundtrip==encoded,"old approval fields retain exact bytes");
    rejectBytes(encoded);intent::ApprovedRecord untouched;Check(intent::DecodeApproved(golden.data(),golden.size(),untouched)!=Result::Ok,"old decoder refuses checkpoint protocol");
}
}
int main(){try{Tests();std::printf("PASS cache checkpoint intent checks=%u\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL cache checkpoint intent checks=%u: %s\n",checks,e.what());return 1;}}
