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
void Tests(){
    const files::Digest abc{0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    Check(Hash(Bytes{'a','b','c'})==abc,"independent SHA oracle matches standard abc vector");
    Check(intent::RecordBytes==752&&intent::MaximumBytes==752&&intent::Key==files::Digest{4},"literal protocol size and envelope key");
    const auto sentinel=Example(19);
    for(auto revision:{1ull,2ull,0xffffffffffffffffull}){
        const auto r=Example(revision);const auto golden=Golden(r);Bytes encoded{7,8,9};
        Check(intent::Encode(r,encoded)==Result::Ok&&encoded==golden,"encode exact independent 752-byte golden");
        auto decoded=sentinel;Check(intent::Decode(golden.data(),golden.size(),decoded)==Result::Ok&&Equal(decoded,r),"decode independent golden every field");
        auto predicted=sentinel.publication.previous;
        Check(intent::PlannedInfo(r.snapshot,predicted)==Result::Ok&&Equal(predicted,r.publication.snapshot),"snapshot prediction changes created size to completed size");
        Check(intent::PlannedInfo(r.commit,predicted)==Result::Ok&&predicted.identity.fileBytes==496&&
            predicted.identity.fileIndex==r.commit.createdIdentity.fileIndex&&predicted.payloadDigest==r.commit.payloadDigest,
            "commit prediction preserves planned digest and file identity without filesystem evidence");
    }
    const auto valid=Example(2);const auto good=Golden(valid);
    const auto reject=[&](const Bytes& bytes){auto out=sentinel;Check(intent::Decode(bytes.data(),bytes.size(),out)==Result::Invalid&&Equal(out,sentinel),"invalid wire retains complete output");};
    for(std::size_t size=0;size<752;++size){auto b=good;b.resize(size);reject(b);}
    {auto b=good;b.push_back(0);reject(b);}
    {auto out=sentinel;Check(intent::Decode(nullptr,752,out)==Result::Invalid&&Equal(out,sentinel),"null input retains output");}
    for(unsigned offset:{0u,15u,16u,17u,18u,19u,20u,21u,22u,23u,40u,41u,42u,43u,44u,45u,46u,47u}){
        auto b=good;b[offset]^=0x80;reject(b);
    }
    for(unsigned base:{416u,584u}){
        for(unsigned offset:{37u,38u,39u,60u,61u,62u,63u,140u,141u,142u,143u}){auto b=good;b[base+offset]=1;reject(b);}
        for(unsigned offset:{0u,31u,32u,36u}){auto b=good;b[base+offset]='Z';reject(b);}
        for(unsigned offset:{40u,56u,64u,96u,128u,136u,144u,152u,160u}){
            auto b=good;b[base+offset]^=1;
            // Changing just commit's fileIndex remains valid if it stays distinct.
            if(base==584&&offset==144)continue;
            reject(b);
        }
    }
    {auto b=good;std::fill_n(b.begin()+24,16,0);reject(b);}
    // The embedded record retains its own reserved-byte and semantic validation.
    for(unsigned offset:{20u,85u,92u,188u,245u,252u,348u}){auto b=good;b[48+offset]=1;reject(b);}
    const auto invalid=[&](auto change){
        auto record=valid;change(record);Bytes out{71,72,73};
        Check(intent::Encode(record,out)==Result::Invalid&&out==Bytes({71,72,73}),"invalid record retains encoded output");
    };
    invalid([](auto& r){r.operation.fill(0);});
    invalid([](auto& r){r.publication.owner[0]^=1;});
    invalid([](auto& r){r.publication.revision=0;});
    invalid([](auto& r){r.snapshot.spec.owner[0]^=1;});
    invalid([](auto& r){r.snapshot.name=L"../foreign.blob";});
    invalid([](auto& r){r.commit.name=L"0123456789ABCDEF0123456789abcdef.blob";});
    invalid([](auto& r){r.snapshot.payloadDigest[31]^=1;});
    invalid([](auto& r){r.snapshot.payloadBytes=0xffffffffffffffffull;});
    invalid([](auto& r){r.snapshot.createdIdentity.fileBytes=1;});
    invalid([](auto& r){r.commit.createdIdentity.fileBytes=1;});
    invalid([](auto& r){r.commit.createdIdentity.volume^=1;});
    invalid([](auto& r){r.commit.createdIdentity.fileIndex=r.snapshot.createdIdentity.fileIndex;});
    invalid([](auto& r){r.commit.spec.owner[0]^=1;});
    invalid([](auto& r){r.commit.spec.key[31]=1;});
    invalid([](auto& r){r.commit.spec.kind=files::Kind::CatalogCheckpoint;});
    invalid([](auto& r){r.commit.payloadBytes=367;r.commit.completedBytes=495;});
    invalid([](auto& r){r.commit.payloadDigest[0]^=1;});
    invalid([](auto& r){r.commit.completedBytes=497;});
    invalid([](auto& r){r.publication.revision=3;}); // Valid publication, stale planned commit digest.
    for(int mutation=0;mutation<6;++mutation){
        auto plan=valid.snapshot;auto out=sentinel.publication.previous;const auto before=out;
        if(mutation==0)plan.name=L"bad.blob";
        if(mutation==1)plan.spec.owner.fill(0);
        if(mutation==2)plan.spec.kind=static_cast<files::Kind>(8);
        if(mutation==3)plan.payloadBytes=files::MaximumPayloadBytes+1;
        if(mutation==4)plan.createdIdentity.fileBytes=1;
        if(mutation==5)++plan.completedBytes;
        Check(intent::PlannedInfo(plan,out)==Result::Invalid&&Equal(out,before),"invalid prediction preserves output");
    }
    // Zero-length snapshots are legal; no invented nonempty payload restriction.
    auto empty=valid;empty.snapshot.payloadBytes=0;empty.snapshot.completedBytes=128;
    empty.snapshot.payloadDigest=Hash({});empty.publication.snapshot.payloadBytes=0;
    empty.publication.snapshot.identity.fileBytes=128;empty.publication.snapshot.payloadDigest=empty.snapshot.payloadDigest;
    empty.commit.payloadDigest=Hash(Publication(empty.publication));Bytes encoded;
    Check(intent::Encode(empty,encoded)==Result::Ok&&encoded==Golden(empty),"empty snapshot remains canonical");
    std::puts("BOUNDARY pure memory codec; planned identities are not verified filesystem identities; no crash recovery authorization tested");
}
}
int main(){
    try{Tests();std::printf("PASS cache intent checks=%u\n",checks);return 0;}
    catch(const std::exception& e){std::fprintf(stderr,"FAIL cache intent checks=%u: %s\n",checks,e.what());return 1;}
}
