#include "cache_source_registry.hpp"
#include "cache_business_wire.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace mikumikustudio::cache_business {
namespace {
using wire::Reader;using wire::Writer;
struct Limit{};struct Unsupported{};
using FileId=std::pair<std::uint32_t,std::uint64_t>;
FileId File(const cache_files::Identity& i)noexcept{return {i.volume,i.fileIndex};}
template<size_t N> bool Zero(const std::array<std::uint8_t,N>& a)noexcept{return std::all_of(a.begin(),a.end(),[](auto b){return b==0;});}
bool Empty(const cache_files::Identity& i)noexcept{return !i.volume&&!i.fileIndex&&!i.fileBytes;}
bool Empty(const cache_files::RawSourceInfo& i)noexcept{return i.name.empty()&&Empty(i.identity)&&Zero(i.payloadDigest)&&!i.payloadBytes;}
bool Empty(const cache_files::RawSourcePlan& p)noexcept{return p.name.empty()&&Empty(p.createdIdentity)&&!p.sourceBytes;}
bool Name(const std::wstring& n)noexcept{
    if(n.size()<38||n.size()>53||n.compare(32,5,L".raw."))return false;
    for(size_t i=0;i<32;++i)if(!((n[i]>=L'0'&&n[i]<=L'9')||(n[i]>=L'a'&&n[i]<=L'f')))return false;
    for(size_t i=37;i<n.size();++i)if(!((n[i]>=L'0'&&n[i]<=L'9')||(n[i]>=L'a'&&n[i]<=L'z')))return false;
    return true;
}
bool Info(const cache_files::RawSourceInfo& i)noexcept{
    return Name(i.name)&&i.payloadBytes&&i.payloadBytes<=INT64_MAX&&i.identity.fileBytes==i.payloadBytes;
}
bool Plan(const cache_files::RawSourcePlan& p)noexcept{return Name(p.name)&&!p.createdIdentity.fileBytes&&p.sourceBytes&&p.sourceBytes<=INT64_MAX;}
bool Captured(SourceStage s)noexcept{return s==SourceStage::Captured||s==SourceStage::AbortCaptured;}
bool Stage(SourceStage s)noexcept{return s>=SourceStage::Reserved&&s<=SourceStage::AbortCaptured;}
std::uint64_t Add(std::uint64_t a,std::uint64_t b){if(b>UINT64_MAX-a)throw Limit{};return a+b;}
std::uint64_t SourceBytes(const SourceRecord& s){return Add(source_wire::SourceFixedBytes,Add(s.file.name.size(),std::uint64_t(s.owners.size())*8));}
std::uint64_t OperationBytes(const SourceOperation& o){return Add(source_wire::OperationFixedBytes,o.plan.name.size());}
std::uint64_t EncodedBytes(const SourceRegistry& s){
    auto bytes=Add(source_wire::RegistryHeaderBytes,std::uint64_t(s.directories.size())*source_wire::DirectoryBytes);
    for(const auto& v:s.sources)bytes=Add(bytes,SourceBytes(v));for(const auto& v:s.operations)bytes=Add(bytes,OperationBytes(v));return bytes;
}
const DirectoryBinding* Directory(const SourceRegistry& s,std::uint64_t id)noexcept{
    const auto it=std::lower_bound(s.directories.begin(),s.directories.end(),id,[](const auto& a,auto b){return a.id<b;});
    return it!=s.directories.end()&&it->id==id?&*it:nullptr;
}
std::set<FileId> FileIdentities(const SourceRegistry& s){
    std::set<FileId> files;
    for(const auto& v:s.sources)files.insert(File(v.file.identity));
    for(const auto& v:s.operations)if(v.stage!=SourceStage::Reserved)files.insert(File(v.plan.createdIdentity));
    return files;
}
void PutName(Writer& w,const std::wstring& n){for(const auto c:n)w.Put(static_cast<std::uint8_t>(c),1);}
std::wstring ReadName(Reader& r,std::uint64_t count){
    if(count>53)throw Limit{};r.Need(static_cast<size_t>(count));std::wstring n;n.resize(static_cast<size_t>(count));
    for(auto& c:n)c=static_cast<wchar_t>(r.Get(1));return n;
}
void PutIdentity(Writer& w,const cache_files::Identity& i){w.Put(i.volume,4);w.Zero(4);w.Put(i.fileIndex,8);w.Put(i.fileBytes,8);}
cache_files::Identity ReadIdentity(Reader& r){cache_files::Identity i;i.volume=static_cast<std::uint32_t>(r.Get(4));r.Zero(4);i.fileIndex=r.Get(8);i.fileBytes=r.Get(8);return i;}
void PutDirectory(Writer& w,const DirectoryBinding& d){w.Put(source_wire::DirectoryBytes,4);w.Put(static_cast<std::uint32_t>(d.area),4);w.Put(d.id,8);w.Put(d.identity.volume,4);w.Zero(4);w.Put(d.identity.fileIndex,8);}
void PutSource(Writer& w,const SourceRecord& s){
    w.Put(SourceBytes(s),4);w.Put(static_cast<std::uint32_t>(s.state),4);w.Array(s.id);w.Array(s.createdBy);w.Put(s.directoryId,8);w.Put(s.lastUse,8);
    w.Put(s.owners.size(),4);w.Put(s.file.name.size(),4);PutIdentity(w,s.file.identity);w.Put(s.file.payloadBytes,8);w.Array(s.file.payloadDigest);w.Zero(16);
    PutName(w,s.file.name);for(const auto id:s.owners)w.Put(id,8);
}
void PutOperation(Writer& w,const SourceOperation& o){
    w.Put(OperationBytes(o),4);w.Put(static_cast<std::uint32_t>(o.stage),4);w.Array(o.id);w.Array(o.sourceId);w.Put(o.directoryId,8);w.Put(o.reservedBytes,8);
    w.Put(o.plan.name.size(),4);w.Zero(4);PutIdentity(w,o.plan.createdIdentity);w.Put(o.plan.sourceBytes,8);
    PutIdentity(w,o.captured.identity);w.Put(o.captured.payloadBytes,8);w.Array(o.captured.payloadDigest);w.Zero(16);PutName(w,o.plan.name);
}
DirectoryBinding ReadDirectory(Reader& r){
    if(r.Get(4)!=source_wire::DirectoryBytes)throw wire::Invalid{};DirectoryBinding d;d.area=static_cast<SourceArea>(r.Get(4));
    if(d.area!=SourceArea::Video)throw Unsupported{};d.id=r.Get(8);d.identity.volume=static_cast<std::uint32_t>(r.Get(4));r.Zero(4);d.identity.fileIndex=r.Get(8);return d;
}
SourceRecord ReadSource(Reader& r){
    const auto begin=r.at;const auto bytes=r.Get(4);if(bytes<source_wire::SourceFixedBytes||bytes>r.size-begin)throw wire::Invalid{};
    SourceRecord s;s.state=static_cast<SourceState>(r.Get(4));if(s.state!=SourceState::Published&&s.state!=SourceState::Retired)throw Unsupported{};
    r.Array(s.id);r.Array(s.createdBy);s.directoryId=r.Get(8);s.lastUse=r.Get(8);const auto owners=r.Get(4),name=r.Get(4);
    if(owners>MaximumOwnersPerEntry||name>53)throw Limit{};
    if(bytes!=source_wire::SourceFixedBytes+name+owners*8)throw wire::Invalid{};
    s.file.identity=ReadIdentity(r);s.file.payloadBytes=r.Get(8);r.Array(s.file.payloadDigest);r.Zero(16);s.file.name=ReadName(r,name);
    s.owners.reserve(static_cast<size_t>(owners));for(size_t i=0;i<owners;++i)s.owners.push_back(r.Get(8));
    if(r.at-begin!=bytes)throw wire::Invalid{};return s;
}
SourceOperation ReadOperation(Reader& r){
    const auto begin=r.at;const auto bytes=r.Get(4);if(bytes<source_wire::OperationFixedBytes||bytes>r.size-begin)throw wire::Invalid{};
    SourceOperation o;o.stage=static_cast<SourceStage>(r.Get(4));if(!Stage(o.stage))throw Unsupported{};
    r.Array(o.id);r.Array(o.sourceId);o.directoryId=r.Get(8);o.reservedBytes=r.Get(8);const auto name=r.Get(4);r.Zero(4);
    if(name>53)throw Limit{};if(bytes!=source_wire::OperationFixedBytes+name)throw wire::Invalid{};
    o.plan.createdIdentity=ReadIdentity(r);o.plan.sourceBytes=r.Get(8);o.captured.identity=ReadIdentity(r);o.captured.payloadBytes=r.Get(8);r.Array(o.captured.payloadDigest);r.Zero(16);
    o.plan.name=ReadName(r,name);if(Captured(o.stage))o.captured.name=o.plan.name;
    if(r.at-begin!=bytes)throw wire::Invalid{};return o;
}
}

Result ValidateSources(const SourceRegistry& s)noexcept{
    try {
        if(s.maintenanceProtocol!=1&&s.maintenanceProtocol!=2)return Result::Unsupported;
        if(s.directories.size()>MaximumSourceDirectories||s.sources.size()>MaximumSources||s.operations.size()>MaximumSourceOperations)return Result::Limit;
        for(const auto& v:s.sources)if(v.owners.size()>MaximumOwnersPerEntry)return Result::Limit;
        // Bound encoded input before allocating lookup sets; oversized valid
        // containers report Limit even when subsequent allocations would fail.
        if(EncodedBytes(s)>MaximumSourceRegistryBytes)return Result::Limit;
        if(Zero(s.storeOwner)||!s.maintenanceEpoch)return Result::Invalid;
        std::set<SourceArea> areas;std::set<FileId> directoryIds;
        for(size_t i=0;i<s.directories.size();++i){const auto& d=s.directories[i];
            if(d.area!=SourceArea::Video)return Result::Unsupported;
            if(!d.id||(i&&s.directories[i-1].id>=d.id)||!areas.insert(d.area).second||!directoryIds.insert({d.identity.volume,d.identity.fileIndex}).second)return Result::Invalid;
        }
        std::set<SourceId> sourceIds;std::set<OperationId> operations;std::set<std::pair<std::uint64_t,std::wstring>> names;std::set<FileId> files;
        std::uint64_t knownBytes=0;
        for(size_t i=0;i<s.sources.size();++i){const auto& v=s.sources[i];
            if(v.state!=SourceState::Published&&v.state!=SourceState::Retired)return Result::Unsupported;
            const auto* d=Directory(s,v.directoryId);
            if(Zero(v.id)||Zero(v.createdBy)||(i&&s.sources[i-1].id>=v.id)||!d||!Info(v.file)||v.file.identity.volume!=d->identity.volume||
                !sourceIds.insert(v.id).second||!operations.insert(v.createdBy).second||!names.insert({v.directoryId,v.file.name}).second||
                directoryIds.count(File(v.file.identity))||!files.insert(File(v.file.identity)).second)return Result::Invalid;
            for(size_t n=0;n<v.owners.size();++n)if(!v.owners[n]||(n&&v.owners[n-1]>=v.owners[n]))return Result::Invalid;
            knownBytes=Add(knownBytes,v.file.payloadBytes);
        }
        for(size_t i=0;i<s.operations.size();++i){const auto& v=s.operations[i];
            if(!Stage(v.stage))return Result::Unsupported;const auto* d=Directory(s,v.directoryId);
            if(Zero(v.id)||Zero(v.sourceId)||(i&&s.operations[i-1].id>=v.id)||!d||!v.reservedBytes||v.reservedBytes>INT64_MAX||
                !sourceIds.insert(v.sourceId).second||!operations.insert(v.id).second)return Result::Invalid;
            if(v.stage==SourceStage::Reserved){if(!Empty(v.plan)||!Empty(v.captured))return Result::Invalid;}
            else {
                if(!Plan(v.plan)||v.plan.sourceBytes!=v.reservedBytes||v.plan.createdIdentity.volume!=d->identity.volume||
                    !names.insert({v.directoryId,v.plan.name}).second||directoryIds.count(File(v.plan.createdIdentity))||!files.insert(File(v.plan.createdIdentity)).second)return Result::Invalid;
                if(Captured(v.stage)){
                    if(!Info(v.captured)||v.captured.name!=v.plan.name||File(v.captured.identity)!=File(v.plan.createdIdentity)||v.captured.payloadBytes!=v.plan.sourceBytes)return Result::Invalid;
                }else if(!Empty(v.captured))return Result::Invalid;
            }
            knownBytes=Add(knownBytes,v.reservedBytes);
        }
        return Result::Ok;
    }catch(const Limit&){return Result::Limit;}catch(...){return Result::AllocationFailure;}
}
Result ValidateSourcesAgainstIndex(const SourceRegistry& s,const Index& index)noexcept{
    const auto valid=ValidateSources(s);if(valid!=Result::Ok)return valid;
    const auto old=Validate(index);if(old!=Result::Ok)return old;
    if(s.storeOwner!=index.storeOwner||s.maintenanceEpoch!=index.maintenanceEpoch)return Result::Invalid;
    try {
        for(const auto& v:s.sources)for(const auto id:v.owners){const auto it=std::lower_bound(index.projects.begin(),index.projects.end(),id,[](const Project& p,ProjectId value){return p.id<value;});
            if(it==index.projects.end()||it->id!=id)return Result::Invalid;}
        const auto files=FileIdentities(s);
        for(const auto& entry:index.entries)if(files.count(File(entry.blob.identity)))return Result::Invalid;
        return Result::Ok;
    }catch(...){return Result::AllocationFailure;}
}
Result EncodeSources(const SourceRegistry& s,Bytes& output)noexcept{
    const auto valid=ValidateSources(s);if(valid!=Result::Ok)return valid;
    try {
        const auto bytes=EncodedBytes(s);Writer w(static_cast<size_t>(bytes));w.Magic(source_wire::RegistryMagic);w.Put(s.maintenanceProtocol,4);w.Put(source_wire::RegistryHeaderBytes,4);
        w.Array(s.storeOwner);w.Put(s.maintenanceEpoch,8);w.Put(s.directories.size(),4);w.Put(s.sources.size(),4);w.Put(s.operations.size(),4);w.Zero(4);w.Put(bytes,8);w.Zero(8);
        for(const auto& d:s.directories)PutDirectory(w,d);for(const auto& v:s.sources)PutSource(w,v);for(const auto& v:s.operations)PutOperation(w,v);
        if(w.at!=w.bytes.size())return Result::Invalid;output.swap(w.bytes);return Result::Ok;
    }catch(const Limit&){return Result::Limit;}catch(const wire::Invalid&){return Result::Invalid;}catch(...){return Result::AllocationFailure;}
}
Result DecodeSources(const void* bytes,std::size_t size,SourceRegistry& output)noexcept{
    if(!bytes||size<source_wire::RegistryHeaderBytes)return Result::Invalid;if(size>MaximumSourceRegistryBytes)return Result::Limit;
    try {
        Reader r{static_cast<const std::uint8_t*>(bytes),size};r.Magic(source_wire::RegistryMagic);const auto protocol=r.Get(4);if(protocol!=1&&protocol!=2)return Result::Unsupported;
        if(r.Get(4)!=source_wire::RegistryHeaderBytes)return Result::Invalid;SourceRegistry s;s.maintenanceProtocol=static_cast<std::uint32_t>(protocol);r.Array(s.storeOwner);s.maintenanceEpoch=r.Get(8);
        const auto directories=r.Get(4),sources=r.Get(4),operations=r.Get(4);r.Zero(4);const auto total=r.Get(8);r.Zero(8);
        if(directories>MaximumSourceDirectories||sources>MaximumSources||operations>MaximumSourceOperations)return Result::Limit;
        if(total!=size||source_wire::RegistryHeaderBytes+directories*source_wire::DirectoryBytes+sources*source_wire::SourceFixedBytes+operations*source_wire::OperationFixedBytes>size)return Result::Invalid;
        s.directories.reserve(static_cast<size_t>(directories));s.sources.reserve(static_cast<size_t>(sources));s.operations.reserve(static_cast<size_t>(operations));
        for(size_t i=0;i<directories;++i)s.directories.push_back(ReadDirectory(r));for(size_t i=0;i<sources;++i)s.sources.push_back(ReadSource(r));for(size_t i=0;i<operations;++i)s.operations.push_back(ReadOperation(r));
        if(r.at!=size)return Result::Invalid;const auto valid=ValidateSources(s);if(valid!=Result::Ok)return valid;
        static_assert(std::is_nothrow_swappable_v<SourceRegistry>);using std::swap;swap(s,output);return Result::Ok;
    }catch(const Unsupported&){return Result::Unsupported;}catch(const Limit&){return Result::Limit;}catch(const wire::Invalid&){return Result::Invalid;}catch(...){return Result::AllocationFailure;}
}
Result ValidateBusinessRoot(const BusinessRoot& root)noexcept{
    const auto manifest=ValidateManifest(root.legacy);if(manifest!=Result::Ok)return manifest;
    const auto sources=ValidateSources(root.sources);if(sources!=Result::Ok)return sources;
    if(root.legacy.storeOwner!=root.sources.storeOwner||root.legacy.maintenanceEpoch!=root.sources.maintenanceEpoch)return Result::Invalid;
    try {
        const auto files=FileIdentities(root.sources);
        for(const auto& page:root.legacy.pages)if(files.count(File(page.blob.identity)))return Result::Invalid;
        return Add(source_wire::RootHeaderBytes,Add(ManifestHeaderBytes+std::uint64_t(root.legacy.pages.size())*PageRefBytes,EncodedBytes(root.sources)))>MaximumBusinessRootBytes?Result::Limit:Result::Ok;
    }
    catch(const Limit&){return Result::Limit;}catch(...){return Result::AllocationFailure;}
}
Result EncodeBusinessRoot(const BusinessRoot& root,Bytes& output)noexcept{
    const auto valid=ValidateBusinessRoot(root);if(valid!=Result::Ok)return valid;
    try {
        Bytes legacy,sources;auto result=EncodeManifest(root.legacy,legacy);if(result!=Result::Ok)return result;
        result=EncodeSources(root.sources,sources);if(result!=Result::Ok)return result;
        const auto total=source_wire::RootHeaderBytes+legacy.size()+sources.size();if(total>MaximumBusinessRootBytes)return Result::Limit;
        Writer w(total);w.Magic(source_wire::RootMagic);w.Put(source_wire::Version,4);w.Put(source_wire::RootHeaderBytes,4);w.Array(root.legacy.storeOwner);w.Put(root.legacy.maintenanceEpoch,8);
        w.Put(legacy.size(),8);w.Put(sources.size(),8);w.Put(total,8);w.Zero(8);w.Need(legacy.size());std::copy(legacy.begin(),legacy.end(),w.bytes.begin()+w.at);w.at+=legacy.size();
        w.Need(sources.size());std::copy(sources.begin(),sources.end(),w.bytes.begin()+w.at);w.at+=sources.size();if(w.at!=total)return Result::Invalid;
        output.swap(w.bytes);return Result::Ok;
    }catch(const wire::Invalid&){return Result::Invalid;}catch(...){return Result::AllocationFailure;}
}
Result DecodeBusinessRoot(const void* bytes,std::size_t size,BusinessRoot& output)noexcept{
    if(!bytes||size<16)return Result::Invalid;if(size>MaximumBusinessRootBytes)return Result::Limit;
    try {
        BusinessRoot candidate;
        if(!std::memcmp(bytes,wire::ManifestMagic,16)){
            const auto result=DecodeManifest(bytes,size,candidate.legacy);if(result!=Result::Ok)return result;
            candidate.sources.storeOwner=candidate.legacy.storeOwner;candidate.sources.maintenanceEpoch=candidate.legacy.maintenanceEpoch;
        }else {
            if(size<source_wire::RootHeaderBytes)return Result::Invalid;
            Reader r{static_cast<const std::uint8_t*>(bytes),size};r.Magic(source_wire::RootMagic);if(r.Get(4)!=source_wire::Version)return Result::Unsupported;
            if(r.Get(4)!=source_wire::RootHeaderBytes)return Result::Invalid;cache_files::Owner owner{};r.Array(owner);const auto epoch=r.Get(8);
            const auto legacy=r.Get(8),sources=r.Get(8),total=r.Get(8);r.Zero(8);
            if(legacy>ManifestHeaderBytes+MaximumPages*PageRefBytes||sources>MaximumSourceRegistryBytes)return Result::Limit;
            if(total!=size||source_wire::RootHeaderBytes+legacy+sources!=size)return Result::Invalid;
            auto result=DecodeManifest(r.data+r.at,static_cast<size_t>(legacy),candidate.legacy);if(result!=Result::Ok)return result;r.at+=static_cast<size_t>(legacy);
            result=DecodeSources(r.data+r.at,static_cast<size_t>(sources),candidate.sources);if(result!=Result::Ok)return result;
            if(owner!=candidate.legacy.storeOwner||epoch!=candidate.legacy.maintenanceEpoch)return Result::Invalid;
        }
        const auto valid=ValidateBusinessRoot(candidate);if(valid!=Result::Ok)return valid;
        static_assert(std::is_nothrow_swappable_v<BusinessRoot>);using std::swap;swap(candidate,output);return Result::Ok;
    }catch(const wire::Invalid&){return Result::Invalid;}catch(...){return Result::AllocationFailure;}
}
}
