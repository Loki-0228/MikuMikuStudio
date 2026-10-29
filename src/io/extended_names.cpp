#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace mikumikustudio::extended_names {
namespace {
struct Writer {
    std::vector<std::uint8_t> data;
    void u32(std::uint32_t v) { for(int i=0;i<4;++i)data.push_back(std::uint8_t(v>>(i*8))); }
    void u64(std::uint64_t v) { u32(std::uint32_t(v));u32(std::uint32_t(v>>32)); }
    void text(const std::wstring& value) {
        std::string bytes;
        if (!text_encoding::EncodeExact(value.c_str(), CP_UTF8, bytes) || bytes.size() > UINT32_MAX)
            throw std::runtime_error("Name cannot be encoded as UTF-8");
        u32(static_cast<std::uint32_t>(bytes.size())); data.insert(data.end(),bytes.begin(),bytes.end());
    }
    void names(const std::vector<std::wstring>& values) {
        if(values.size()>UINT32_MAX)throw std::length_error("Too many names");
        u32(static_cast<std::uint32_t>(values.size()));for(const auto&v:values)text(v);
    }
    void indices(const std::vector<std::uint32_t>& values) {
        if(values.size()>UINT32_MAX)throw std::length_error("Too many keys");
        u32(static_cast<std::uint32_t>(values.size()));for(auto v:values)u32(v);
    }
};
struct Reader {
    const std::uint8_t* data; std::size_t size,at=0;
    void need(std::size_t n) { if(n>size-at)throw std::runtime_error("Incomplete Unicode names"); }
    std::uint32_t u32() {need(4);std::uint32_t v=0;for(int i=0;i<4;++i)v|=std::uint32_t(data[at++])<<(i*8);return v;}
    std::uint64_t u64() {const auto lo=u32();return lo|(std::uint64_t(u32())<<32);}
    std::wstring text() {
        const auto n=u32();need(n);std::string bytes(reinterpret_cast<const char*>(data+at),n);at+=n;
        std::wstring value;
        if(bytes.find('\0')!=std::string::npos || !text_encoding::Decode(bytes.c_str(),CP_UTF8,value))
            throw std::runtime_error("Invalid UTF-8 name");
        return value;
    }
    std::vector<std::wstring> names() {
        const auto n=u32();if(n>(size-at)/4)throw std::runtime_error("Invalid name count");
        std::vector<std::wstring> out;out.reserve(n);for(std::uint32_t i=0;i<n;++i)out.push_back(text());return out;
    }
    std::vector<std::uint32_t> indices() {
        const auto n=u32();if(n>(size-at)/4)throw std::runtime_error("Invalid key count");
        std::vector<std::uint32_t> out;out.reserve(n);for(std::uint32_t i=0;i<n;++i)out.push_back(u32());return out;
    }
};
std::uint32_t Crc(const std::uint8_t* p,std::size_t n) {
    std::uint32_t c=~0u;for(std::size_t i=0;i<n;++i){c^=p[i];for(int j=0;j<8;++j)c=(c>>1)^(0xedb88320u&(0u-(c&1u)));}return ~c;
}
std::string Key(int slot) {return "org.mikudancestudio.model."+std::to_string(slot)+".unicode.v1";}
bool Exact(const std::wstring& name,std::size_t bytes) {std::string encoded;return original_file_encoding::Encode(name.c_str(),bytes,encoded);}
}
bool Assign(wchar_t*& destination,const std::wstring& value) noexcept {
    if(value.size()>(SIZE_MAX/sizeof(wchar_t))-1)return false;
    auto* copy=static_cast<wchar_t*>(std::malloc((value.size()+1)*sizeof(wchar_t)));
    if(!copy)return false;
    std::memcpy(copy,value.c_str(),(value.size()+1)*sizeof(wchar_t));
    std::free(destination);destination=copy;return true;
}
bool LegacyNamesFit(const mdl::ModelRecord& m,bool motion) {
    if(!Exact(text_encoding::ModelName(m,false),20)||!Exact(text_encoding::ModelName(m,true),20))return false;
    for(std::uint32_t i=0;i<m.boneCount;++i)
        if(!Exact(text_encoding::BoneName(m.boneTable[i],false),motion?15:19)||
           (!motion&&!Exact(text_encoding::BoneName(m.boneTable[i],true),19)))return false;
    for(std::uint32_t i=0;i<m.morphCount;++i)
        if(!Exact(text_encoding::MorphName(m.morphs[i],false),motion?15:19)||
           (!motion&&!Exact(text_encoding::MorphName(m.morphs[i],true),19)))return false;
    const auto count=mdl::DisplayGroupCount(m);
    const auto* groups=static_cast<const mdl::DisplayGroup*>(m.groupNames);
    for(std::uint32_t i=0;!motion&&groups&&i<count;++i)
        if(!Exact(text_encoding::DisplayGroupName(groups[i],false),49)||!Exact(text_encoding::DisplayGroupName(groups[i],true),49))return false;
    return true;
}
void CapturePmm(const MMDApp& app,pmm_extension::Document& doc) {
    for(int slot=0;slot<kModelSlotCount;++slot) {
        const auto* raw=app.ModelSlot(slot);if(!raw){pmm_extension::Erase(doc,Key(slot));continue;}
        const auto& m=*mdl::Mdl(raw);Writer w;w.u32(1);
        for(int language=0;language<2;++language)w.text(text_encoding::ModelName(m,language!=0));
        w.u32(m.boneCount);for(std::uint32_t i=0;i<m.boneCount;++i)for(int l=0;l<2;++l)w.text(text_encoding::BoneName(m.boneTable[i],l!=0));
        w.u32(m.morphCount);for(std::uint32_t i=0;i<m.morphCount;++i)for(int l=0;l<2;++l)w.text(text_encoding::MorphName(m.morphs[i],l!=0));
        const auto count=mdl::DisplayGroupCount(m);w.u32(count);w.u32(mdl::FacialFrameCount(m));
        const auto* groups=static_cast<const mdl::DisplayGroup*>(m.groupNames);
        for(std::uint32_t i=0;i<count;++i){
            for(int l=0;l<2;++l)w.text(text_encoding::DisplayGroupName(groups[i],l!=0));
            w.u32(groups[i].flags);
        }
        pmm_extension::Set(doc,Key(slot),std::move(w.data));
    }
}
void LoadPmm(MMDApp& app,const pmm_extension::Document& doc,
             const std::vector<int>& adaptedModelSlots) {
    for(int slot=0;slot<kModelSlotCount;++slot) {
        const auto* entry=pmm_extension::Find(doc,Key(slot));auto* raw=app.ModelSlot(slot);if(!entry||!raw)continue;
        // Standard PMM reconciliation already moved the animation to the
        // edited model. This positional name dictionary still describes the
        // old structure, including when only the order or names changed.
        if(std::find(adaptedModelSlots.begin(),adaptedModelSlots.end(),slot)!=adaptedModelSlots.end()) {
            runtime_log::Write("PMM Unicode names ignored for adapted model slot %d",slot);
            continue;
        }
        auto& m=*mdl::Mdl(raw);Reader r{entry->value.data(),entry->value.size()};
        if(r.u32()!=1)continue;
        std::vector<std::wstring> values;values.push_back(r.text());values.push_back(r.text());
        if(r.u32()!=m.boneCount)continue;
        for(std::uint32_t i=0;i<m.boneCount;++i)for(int l=0;l<2;++l)values.push_back(r.text());
        if(r.u32()!=m.morphCount)continue;
        for(std::uint32_t i=0;i<m.morphCount;++i)for(int l=0;l<2;++l)values.push_back(r.text());
        const auto groups=r.u32(),faces=r.u32();
        // Display groups can change without a bone/morph mismatch prompt.
        // A dictionary for another structure is optional metadata, not proof
        // that the standard PMM records are invalid.
        if(groups!=mdl::DisplayGroupCount(m)||faces!=mdl::FacialFrameCount(m))continue;
        std::vector<std::array<std::wstring,2>> names(groups);std::vector<std::uint8_t> flags(groups);
        for(std::uint32_t i=0;i<groups;++i){names[i][0]=r.text();names[i][1]=r.text();const auto f=r.u32();if(f>255)throw std::runtime_error("Invalid group state");flags[i]=std::uint8_t(f);}
        if(r.at!=r.size)throw std::runtime_error("Unexpected Unicode name bytes");
        // Allocate all owned strings before publishing any replacement.
        std::vector<wchar_t*> allocated(values.size(),nullptr);
        try {for(std::size_t i=0;i<values.size();++i)if(!Assign(allocated[i],values[i]))throw std::bad_alloc();}
        catch(...){for(auto*p:allocated)std::free(p);throw;}
        std::size_t at=0;
        auto publish=[&](wchar_t*& p){std::free(p);p=allocated[at++];};
        for(int l=0;l<2;++l)publish(m.pmxTextBuffers[l]);
        for(std::uint32_t i=0;i<m.boneCount;++i){publish(m.boneTable[i].jpText);publish(m.boneTable[i].enText);}
        for(std::uint32_t i=0;i<m.morphCount;++i){publish(m.morphs[i].jpText);publish(m.morphs[i].enText);}
        auto* table=static_cast<mdl::DisplayGroup*>(m.groupNames);
        for(std::uint32_t i=0;i<groups;++i){text_encoding::SetDisplayGroupName(m,table[i],names[i][0].c_str(),names[i][1].c_str());table[i].flags=flags[i];}
    }
}
std::vector<std::uint8_t> EncodeMotion(const MotionNames& names,std::uint64_t prefixSize,std::uint32_t prefixCrc) {
    Writer w;w.text(names.model);w.names(names.bones);w.names(names.morphs);w.indices(names.boneOwners);w.indices(names.morphOwners);w.indices(names.ikOwners);
    Writer out;const char magic[]="MMSNAME1";out.data.insert(out.data.end(),magic,magic+8);
    out.u64(w.data.size());out.u64(prefixSize);out.u32(prefixCrc);out.u32(Crc(w.data.data(),w.data.size()));
    out.data.insert(out.data.end(),w.data.begin(),w.data.end());return out.data;
}
std::size_t DecodeMotion(const std::uint8_t* file,std::size_t size,std::size_t offset,MotionNames& out) {
    if(offset>size||size-offset<8||(std::memcmp(file+offset,"MMSNAME1",8)&&std::memcmp(file+offset,"MDSNAME1",8)))return 0;
    Reader h{file+offset,size-offset};h.at=8;const auto n=h.u64(),prefix=h.u64();const auto crc=h.u32(),payload=h.u32();
    if(prefix!=offset||n>h.size-h.at||Crc(file,offset)!=crc||Crc(h.data+h.at,static_cast<std::size_t>(n))!=payload)throw std::runtime_error("Unicode motion checksum mismatch");
    Reader r{h.data+h.at,static_cast<std::size_t>(n)};MotionNames result;
    result.model=r.text();result.bones=r.names();result.morphs=r.names();result.boneOwners=r.indices();result.morphOwners=r.indices();result.ikOwners=r.indices();
    for(auto i:result.boneOwners)if(i>=result.bones.size())throw std::runtime_error("Invalid Unicode bone identity");
    for(auto i:result.morphOwners)if(i>=result.morphs.size())throw std::runtime_error("Invalid Unicode morph identity");
    for(auto i:result.ikOwners)if(i!=UINT32_MAX&&i>=result.bones.size())throw std::runtime_error("Invalid Unicode IK identity");
    if(r.at!=r.size)throw std::runtime_error("Unexpected Unicode motion bytes");out=std::move(result);return h.at+static_cast<std::size_t>(n);
}
std::vector<int> MatchNames(const std::vector<std::wstring>& source,const std::vector<std::wstring>& target) {
    std::map<std::wstring,int> indices;for(std::size_t i=0;i<target.size();++i){auto [it,inserted]=indices.emplace(target[i],static_cast<int>(i));if(!inserted)it->second=-1;}
    std::vector<int> out;out.reserve(source.size());for(const auto& name:source){const auto it=indices.find(name);out.push_back(it==indices.end()?-1:it->second);}return out;
}
}
