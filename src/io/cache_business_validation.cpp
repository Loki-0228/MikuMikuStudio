#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "cache_business_codec.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace mikumikustudio::cache_business {
namespace {
constexpr auto Overflow=(std::numeric_limits<std::uint64_t>::max)();
std::uint64_t Add(std::uint64_t a,std::uint64_t b) noexcept {
    return b>Overflow-a?Overflow:a+b;
}
std::uint64_t Multiply(std::uint64_t a,std::uint64_t b) noexcept {
    return b&&a>Overflow/b?Overflow:a*b;
}
bool DataKind(cache_files::Kind kind) noexcept {
    return kind==cache_files::Kind::Effect||kind==cache_files::Kind::Texture;
}
bool NameValid(const std::wstring& name) noexcept {
    if(name.size()!=37||name.compare(32,5,L".blob")!=0)return false;
    for(std::size_t i=0;i<32;++i)
        if(!((name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f')))return false;
    return true;
}
char16_t UpperAscii(char16_t c) noexcept {return c>=u'a'&&c<=u'z'?char16_t(c-u'a'+u'A'):c;}
bool Word(const std::u16string& path,std::size_t start,std::size_t count,const char16_t* value) noexcept {
    std::size_t i=0;for(;value[i];++i)if(i>=count||UpperAscii(path[start+i])!=value[i])return false;
    return i==count;
}
bool Reserved(const std::u16string& path,std::size_t start,std::size_t count) noexcept {
    std::size_t base=0;while(base<count&&path[start+base]!=u'.')++base;
    // Win32 recognizes a DOS device basename even before an extension. Spaces
    // before that extension must not disguise e.g. "CON .txt".
    while(base&&path[start+base-1]==u' ')--base;
    if(Word(path,start,base,u"CON")||Word(path,start,base,u"PRN")||Word(path,start,base,u"AUX")||
       Word(path,start,base,u"NUL")||Word(path,start,base,u"CONIN$")||Word(path,start,base,u"CONOUT$"))return true;
    if(base!=4||(!Word(path,start,3,u"COM")&&!Word(path,start,3,u"LPT")))return false;
    const auto digit=path[start+3];
    return (digit>=u'1'&&digit<=u'9')||digit==u'\u00b9'||digit==u'\u00b2'||digit==u'\u00b3';
}
bool Segment(const std::u16string& path,std::size_t start,std::size_t count) noexcept {
    if(!count||path[start+count-1]==u'.'||path[start+count-1]==u' ')return false;
    for(std::size_t i=0;i<count;++i){const auto c=path[start+i];
        if(c<32||c==u'<'||c==u'>'||c==u':'||c==u'"'||c==u'/'||c==u'|'||c==u'?'||c==u'*')return false;
    }
    return !Reserved(path,start,count);
}
bool PathValid(const std::u16string& path) noexcept {
    const auto n=path.size();std::size_t start=0,segments=0;bool unc=false;
    if(n>=3&&((path[0]>=u'A'&&path[0]<=u'Z')||(path[0]>=u'a'&&path[0]<=u'z'))&&path[1]==u':'&&path[2]==u'\\')start=3;
    else if(n>=2&&path[0]==u'\\'&&path[1]==u'\\'){start=2;unc=true;}
    else return false;
    if(start==n)return !unc; // DOS volume root is lexical absolute.
    while(start<n){
        auto end=start;while(end<n&&path[end]!=u'\\')++end;
        if(!Segment(path,start,end-start))return false;
        ++segments;
        if(end==n)return !unc||segments>=2;
        if(end+1==n)return unc&&segments==2; // Optional UNC share-root separator.
        start=end+1;
    }
    return false;
}
struct InvalidComparison {};
int ComparePath(const std::u16string& a,const std::u16string& b) {
    static_assert(sizeof(wchar_t)==sizeof(char16_t),"Win32 UTF-16 code units");
    // Counts are checked against MaximumPathUnits before this API. Do not
    // use locale case conversion or resolve filesystem aliases in a codec.
    const auto result=CompareStringOrdinal(reinterpret_cast<LPCWCH>(a.data()),static_cast<int>(a.size()),
        reinterpret_cast<LPCWCH>(b.data()),static_cast<int>(b.size()),TRUE);
    if(!result)throw InvalidComparison{};
    return result;
}
Result Projects(const std::vector<Project>& values) {
    if(values.size()>MaximumProjects)return Result::Limit;
    for(std::size_t i=0;i<values.size();++i){const auto& p=values[i];
        if(p.normalizedPath.size()>MaximumPathUnits||p.displayPath.size()>MaximumPathUnits)return Result::Limit;
        if(!p.id||(i&&values[i-1].id>=p.id)||!PathValid(p.normalizedPath)||!PathValid(p.displayPath))return Result::Invalid;
        if(ComparePath(p.normalizedPath,p.displayPath)!=CSTR_EQUAL)return Result::Invalid;
    }
    std::vector<const std::u16string*> paths;paths.reserve(values.size());
    for(const auto& p:values)paths.push_back(&p.normalizedPath);
    std::sort(paths.begin(),paths.end(),[](const auto* a,const auto* b){return ComparePath(*a,*b)==CSTR_LESS_THAN;});
    for(std::size_t i=1;i<paths.size();++i)if(ComparePath(*paths[i-1],*paths[i])==CSTR_EQUAL)return Result::Invalid;
    return Result::Ok;
}
bool EntryLess(const Entry& a,const Entry& b) noexcept {
    if(a.blob.spec.kind!=b.blob.spec.kind)return std::uint32_t(a.blob.spec.kind)<std::uint32_t(b.blob.spec.kind);
    return a.blob.name<b.blob.name;
}
Result Entries(const std::vector<Entry>& values,const cache_files::Owner& owner) noexcept {
    if(values.size()>MaximumEntries)return Result::Limit;
    for(std::size_t i=0;i<values.size();++i){const auto& entry=values[i];
        if(entry.owners.size()>MaximumOwnersPerEntry||entry.blob.payloadBytes>cache_files::MaximumPayloadBytes)return Result::Limit;
        if(!detail::ValidBlob(entry.blob,owner,false)||(i&&!EntryLess(values[i-1],entry)))return Result::Invalid;
        if(entry.availability!=Availability::Valid&&entry.availability!=Availability::Invalid&&entry.availability!=Availability::Retired)return Result::Invalid;
        for(std::size_t j=0;j<entry.owners.size();++j)
            if(!entry.owners[j]||(j&&entry.owners[j-1]>=entry.owners[j]))return Result::Invalid;
    }
    return Result::Ok;
}
Result Currents(const std::vector<Current>& values) noexcept {
    if(values.size()>MaximumCurrent)return Result::Limit;
    for(std::size_t i=0;i<values.size();++i)
        if(!DataKind(values[i].key.kind)||(i&&!detail::KeyLess(values[i-1].key,values[i].key)))return Result::Invalid;
    return Result::Ok;
}
std::uint64_t SizeOf(const Project& p) noexcept {return detail::RecordBytes(p);}
std::uint64_t SizeOf(const Entry& e) noexcept {return detail::RecordBytes(e);}
std::uint64_t SizeOf(const Current&) noexcept {return 64;}
struct Pagination {
    std::size_t count=0;
    std::uint64_t bytes=ManifestHeaderBytes;
    template<class T> Result Scan(const std::vector<T>& records) noexcept {
        std::uint64_t pageBytes=0;
        for(const auto& record:records){const auto size=SizeOf(record);
            if(size>MaximumPageBytes-PageHeaderBytes)return Result::Limit;
            if(!pageBytes||size>MaximumPageBytes-pageBytes){
                if(count==MaximumPages)return Result::Limit;
                ++count;pageBytes=PageHeaderBytes;bytes=Add(bytes,PageRefBytes+PageHeaderBytes);
            }
            pageBytes+=size;bytes=Add(bytes,size);
            if(bytes>MaximumLiveBytes)return Result::Limit;
        }
        return Result::Ok;
    }
};
Result PaginatedSize(const Index& index,Pagination& result) noexcept {
    auto r=result.Scan(index.projects);if(r!=Result::Ok)return r;
    r=result.Scan(index.entries);if(r!=Result::Ok)return r;
    return result.Scan(index.current);
}
void NameFor(const GenerationId& generation,wchar_t (&name)[38]) noexcept {
    constexpr wchar_t hex[]=L"0123456789abcdef";
    for(std::size_t i=0;i<generation.size();++i){name[2*i]=hex[generation[i]>>4];name[2*i+1]=hex[generation[i]&15];}
    name[32]=L'.';name[33]=L'b';name[34]=L'l';name[35]=L'o';name[36]=L'b';name[37]=0;
}
template<class T,class Append> void MakePages(const std::vector<T>& records,PageKind kind,
    const cache_files::Owner& owner,std::vector<Page>& pages,Append append) {
    std::uint32_t ordinal=0;std::uint64_t bytes=0;
    for(const auto& record:records){const auto size=SizeOf(record);
        if(!bytes||size>MaximumPageBytes-bytes){
            Page page;page.storeOwner=owner;page.kind=kind;page.ordinal=ordinal++;
            pages.push_back(std::move(page));bytes=PageHeaderBytes;
        }
        append(pages.back(),record);bytes+=size;
    }
}
}

namespace detail {
bool ValidOwner(const cache_files::Owner& owner) noexcept {
    return std::any_of(owner.begin(),owner.end(),[](auto value){return value!=0;});
}
bool ValidBlob(const cache_files::BlobInfo& blob,const cache_files::Owner& owner,bool page) noexcept {
    if(!ValidOwner(owner)||blob.spec.owner!=owner||!NameValid(blob.name)||
       blob.payloadBytes>cache_files::MaximumPayloadBytes||blob.payloadBytes>Overflow-cache_files::HeaderBytes||
       blob.identity.fileBytes!=cache_files::HeaderBytes+blob.payloadBytes)return false;
    if(page)return blob.spec.kind==cache_files::Kind::IndexPage&&blob.spec.key==PageKey&&
        blob.payloadBytes>=PageHeaderBytes&&blob.payloadBytes<=MaximumPageBytes;
    return DataKind(blob.spec.kind);
}
std::uint64_t RecordBytes(const Project& project) noexcept {
    return Add(40,Multiply(Add(project.normalizedPath.size(),project.displayPath.size()),2));
}
std::uint64_t RecordBytes(const Entry& entry) noexcept {return Add(192,Multiply(entry.owners.size(),8));}
std::size_t Count(const Page& page) noexcept {
    switch(page.kind){case PageKind::Projects:return page.projects.size();case PageKind::Entries:return page.entries.size();case PageKind::Current:return page.current.size();}
    return 0;
}
std::uint64_t PageBytes(const Page& page) noexcept {
    std::uint64_t size=PageHeaderBytes;
    switch(page.kind){
    case PageKind::Projects:
        if(!page.entries.empty()||!page.current.empty())return Overflow;
        for(const auto& item:page.projects)size=Add(size,RecordBytes(item));return size;
    case PageKind::Entries:
        if(!page.projects.empty()||!page.current.empty())return Overflow;
        for(const auto& item:page.entries)size=Add(size,RecordBytes(item));return size;
    case PageKind::Current:
        if(!page.projects.empty()||!page.entries.empty())return Overflow;
        return Add(size,Multiply(page.current.size(),64));
    }
    return Overflow;
}
bool KeyLess(const LogicalKey& a,const LogicalKey& b) noexcept {
    if(a.kind!=b.kind)return std::uint32_t(a.kind)<std::uint32_t(b.kind);
    if(a.consumerFormat!=b.consumerFormat)return a.consumerFormat<b.consumerFormat;
    return a.key<b.key;
}
}

Result ValidatePage(const Page& page) noexcept {
    try {
        if(page.ordinal>=MaximumPages)return Result::Limit;
        if(!detail::ValidOwner(page.storeOwner))return Result::Invalid;
        Result result=Result::Invalid;
        switch(page.kind){
        case PageKind::Projects:
            if(!page.entries.empty()||!page.current.empty())return Result::Invalid;
            result=Projects(page.projects);break;
        case PageKind::Entries:
            if(!page.projects.empty()||!page.current.empty())return Result::Invalid;
            result=Entries(page.entries,page.storeOwner);break;
        case PageKind::Current:
            if(!page.projects.empty()||!page.entries.empty())return Result::Invalid;
            result=Currents(page.current);break;
        default:return Result::Invalid;
        }
        if(result!=Result::Ok)return result;
        if(!detail::Count(page))return Result::Invalid;
        return detail::PageBytes(page)>MaximumPageBytes?Result::Limit:Result::Ok;
    }catch(const std::bad_alloc&){return Result::AllocationFailure;}
    catch(...){return Result::Invalid;}
}
Result Validate(const Index& index) noexcept {
    try {
        if(index.projects.size()>MaximumProjects||index.entries.size()>MaximumEntries||index.current.size()>MaximumCurrent)return Result::Limit;
        if(!detail::ValidOwner(index.storeOwner)||!index.maintenanceEpoch||!index.nextProjectId)return Result::Invalid;
        auto result=Projects(index.projects);if(result!=Result::Ok)return result;
        if(!index.projects.empty()&&index.nextProjectId<=index.projects.back().id)return Result::Invalid;
        result=Entries(index.entries,index.storeOwner);if(result!=Result::Ok)return result;
        result=Currents(index.current);if(result!=Result::Ok)return result;
        Pagination size;result=PaginatedSize(index,size);if(result!=Result::Ok)return result;
        for(const auto& entry:index.entries)for(const auto owner:entry.owners){
            const auto project=std::lower_bound(index.projects.begin(),index.projects.end(),owner,
                [](const Project& p,ProjectId id){return p.id<id;});
            if(project==index.projects.end()||project->id!=owner)return Result::Invalid;
        }
        std::vector<bool> used(index.entries.size(),false);
        for(const auto& current:index.current){
            wchar_t name[38]{};NameFor(current.generation,name);
            const auto entry=std::lower_bound(index.entries.begin(),index.entries.end(),name,
                [&](const Entry& e,const wchar_t* expected){
                    if(e.blob.spec.kind!=current.key.kind)return std::uint32_t(e.blob.spec.kind)<std::uint32_t(current.key.kind);
                    return e.blob.name.compare(expected)<0;
                });
            if(entry==index.entries.end()||entry->blob.spec.kind!=current.key.kind||entry->blob.name.compare(name)!=0||
               entry->availability!=Availability::Valid||entry->consumerFormat!=current.key.consumerFormat||entry->blob.spec.key!=current.key.key)return Result::Invalid;
            const auto slot=static_cast<std::size_t>(entry-index.entries.begin());
            if(used[slot])return Result::Invalid;
            used[slot]=true;
        }
        return Result::Ok;
    }catch(const std::bad_alloc&){return Result::AllocationFailure;}
    catch(...){return Result::Invalid;}
}
Result Partition(const Index& index,std::vector<Page>& output) noexcept {
    const auto result=Validate(index);if(result!=Result::Ok)return result;
    try {
        Pagination size;const auto checked=PaginatedSize(index,size);if(checked!=Result::Ok)return checked;
        std::vector<Page> candidate;candidate.reserve(size.count);
        MakePages(index.projects,PageKind::Projects,index.storeOwner,candidate,[](Page& p,const Project& v){p.projects.push_back(v);});
        MakePages(index.entries,PageKind::Entries,index.storeOwner,candidate,[](Page& p,const Entry& v){p.entries.push_back(v);});
        MakePages(index.current,PageKind::Current,index.storeOwner,candidate,[](Page& p,const Current& v){p.current.push_back(v);});
        output.swap(candidate);return Result::Ok;
    }catch(const std::bad_alloc&){return Result::AllocationFailure;}
    catch(...){return Result::Invalid;}
}
}
