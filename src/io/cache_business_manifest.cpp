#include "cache_business_wire.hpp"
#include <set>
#include <type_traits>
#include <utility>

namespace mikumikustudio::cache_business {
namespace {
std::uint32_t KindNumber(PageKind kind) noexcept {return static_cast<std::uint32_t>(kind);}
std::uint64_t MinimumRecord(PageKind kind) noexcept {
    return kind==PageKind::Projects?40:kind==PageKind::Entries?192:64;
}
}
Result ValidateManifest(const Manifest& manifest) noexcept {
    try {
        if (!detail::ValidOwner(manifest.storeOwner) || !manifest.maintenanceEpoch || !manifest.nextProjectId)
            return Result::Invalid;
        if (manifest.projectCount>MaximumProjects || manifest.entryCount>MaximumEntries ||
            manifest.currentCount>MaximumCurrent || manifest.pages.size()>MaximumPages ||
            manifest.liveEncodedBytes>MaximumLiveBytes) return Result::Limit;
        std::uint64_t bytes=ManifestHeaderBytes+PageRefBytes*manifest.pages.size();
        std::uint64_t counts[3]{};
        std::uint32_t ordinals[3]{};
        std::uint32_t previousKind=0;
        std::set<std::wstring> names;
        for (const auto& page:manifest.pages) {
            const auto kind=KindNumber(page.kind);
            if(page.blob.payloadBytes>MaximumPageBytes)return Result::Limit;
            if (kind<1 || kind>3 || kind<previousKind || !page.recordCount ||
                page.ordinal!=ordinals[kind-1] || !detail::ValidBlob(page.blob,manifest.storeOwner,true))
                return Result::Invalid;
            if (page.ordinal>=MaximumPages || page.recordCount>(kind==1?MaximumProjects:MaximumEntries))
                return Result::Limit;
            if (PageHeaderBytes+MinimumRecord(page.kind)*page.recordCount>page.blob.payloadBytes ||
                !names.insert(page.blob.name).second) return Result::Invalid;
            previousKind=kind;++ordinals[kind-1];counts[kind-1]+=page.recordCount;
            bytes+=page.blob.payloadBytes;
        }
        if (counts[0]!=manifest.projectCount || counts[1]!=manifest.entryCount ||
            counts[2]!=manifest.currentCount || bytes!=manifest.liveEncodedBytes) return Result::Invalid;
        return bytes>MaximumLiveBytes?Result::Limit:Result::Ok;
    } catch (...) {return Result::AllocationFailure;}
}
Result EncodeManifest(const Manifest& manifest,Bytes& output) noexcept {
    const auto result=ValidateManifest(manifest);if(result!=Result::Ok)return result;
    try {
        wire::Writer writer(ManifestHeaderBytes+PageRefBytes*manifest.pages.size());
        writer.Magic(wire::ManifestMagic);writer.Put(1,4);writer.Put(ManifestHeaderBytes,4);
        writer.Array(manifest.storeOwner);writer.Put(manifest.maintenanceEpoch,8);writer.Put(manifest.nextProjectId,8);
        writer.Put(manifest.projectCount,4);writer.Put(manifest.entryCount,4);writer.Put(manifest.currentCount,4);
        writer.Put(manifest.pages.size(),4);writer.Put(manifest.liveEncodedBytes,8);writer.Zero(16);
        for(const auto& page:manifest.pages){
            writer.Put(KindNumber(page.kind),4);writer.Put(page.ordinal,4);writer.Put(page.recordCount,4);writer.Zero(4);writer.Blob(page.blob);
        }
        if(writer.at!=writer.bytes.size())return Result::Invalid;
        output.swap(writer.bytes);return Result::Ok;
    } catch(const wire::Invalid&){return Result::Invalid;} catch(...){return Result::AllocationFailure;}
}
Result DecodeManifest(const void* bytes,std::size_t size,Manifest& output) noexcept {
    if(!bytes || size<ManifestHeaderBytes)return Result::Invalid;
    if(size>ManifestHeaderBytes+MaximumPages*PageRefBytes)return Result::Limit;
    try {
        wire::Reader reader{static_cast<const std::uint8_t*>(bytes),size};reader.Magic(wire::ManifestMagic);
        if(reader.Get(4)!=1)return Result::Unsupported;
        if(reader.Get(4)!=ManifestHeaderBytes)return Result::Invalid;
        Manifest candidate;reader.Array(candidate.storeOwner);candidate.maintenanceEpoch=reader.Get(8);candidate.nextProjectId=reader.Get(8);
        candidate.projectCount=static_cast<std::uint32_t>(reader.Get(4));candidate.entryCount=static_cast<std::uint32_t>(reader.Get(4));
        candidate.currentCount=static_cast<std::uint32_t>(reader.Get(4));const auto count=reader.Get(4);
        candidate.liveEncodedBytes=reader.Get(8);reader.Zero(16);
        if(count>MaximumPages)return Result::Limit;
        if(ManifestHeaderBytes+count*PageRefBytes!=size)return Result::Invalid;
        if(candidate.projectCount>MaximumProjects || candidate.entryCount>MaximumEntries || candidate.currentCount>MaximumCurrent ||
            candidate.liveEncodedBytes>MaximumLiveBytes)return Result::Limit;
        candidate.pages.reserve(static_cast<std::size_t>(count));
        for(std::size_t i=0;i<count;++i){
            PageRef page;page.kind=static_cast<PageKind>(reader.Get(4));page.ordinal=static_cast<std::uint32_t>(reader.Get(4));
            page.recordCount=static_cast<std::uint32_t>(reader.Get(4));reader.Zero(4);page.blob=reader.Blob();candidate.pages.push_back(std::move(page));
        }
        if(reader.at!=size)return Result::Invalid;
        const auto result=ValidateManifest(candidate);if(result!=Result::Ok)return result;
        static_assert(std::is_nothrow_swappable_v<Manifest>);using std::swap;swap(candidate,output);return Result::Ok;
    } catch(const wire::Invalid&){return Result::Invalid;} catch(...){return Result::AllocationFailure;}
}
Result Assemble(const Manifest& manifest,const std::vector<Page>& pages,Index& output) noexcept {
    const auto valid=ValidateManifest(manifest);if(valid!=Result::Ok)return valid;
    if(pages.size()!=manifest.pages.size())return Result::Invalid;
    try {
        // Validate the entire local page set before allocating the combined index.
        for(std::size_t i=0;i<pages.size();++i){
            const auto& page=pages[i];const auto& ref=manifest.pages[i];
            const auto result=ValidatePage(page);if(result!=Result::Ok)return result;
            if(page.kind!=ref.kind || page.ordinal!=ref.ordinal || page.storeOwner!=manifest.storeOwner ||
                detail::Count(page)!=ref.recordCount || detail::PageBytes(page)!=ref.blob.payloadBytes)return Result::Invalid;
        }
        Index candidate;candidate.storeOwner=manifest.storeOwner;candidate.maintenanceEpoch=manifest.maintenanceEpoch;candidate.nextProjectId=manifest.nextProjectId;
        candidate.projects.reserve(manifest.projectCount);candidate.entries.reserve(manifest.entryCount);candidate.current.reserve(manifest.currentCount);
        for(const auto& page:pages){
            candidate.projects.insert(candidate.projects.end(),page.projects.begin(),page.projects.end());
            candidate.entries.insert(candidate.entries.end(),page.entries.begin(),page.entries.end());
            candidate.current.insert(candidate.current.end(),page.current.begin(),page.current.end());
        }
        const auto result=Validate(candidate);if(result!=Result::Ok)return result;
        static_assert(std::is_nothrow_swappable_v<Index>);using std::swap;swap(candidate,output);return Result::Ok;
    } catch(...){return Result::AllocationFailure;}
}
}
