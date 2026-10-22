#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/owned_text.hpp"
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
std::string Key(int slot) {return "org.mikumikustudio.model."+std::to_string(slot)+".unicode.v1";}
std::string CommentKey(int slot) {return "org.mikumikustudio.model."+std::to_string(slot)+".comments.v1";}
bool Exact(const std::wstring& name,std::size_t bytes) {std::string encoded;return original_file_encoding::Encode(name.c_str(),bytes,encoded);}
bool Comment(const char* raw, std::size_t capacity, const wchar_t* unicode, std::wstring& value) {
    if (unicode) {
        owned_text::Allocation allocation;
        if (owned_text::Find(unicode, allocation)) {
            const auto units = allocation.bytes / sizeof(wchar_t);
            std::size_t length = 0; while (length < units && unicode[length]) ++length;
            if (length == units) return false;
            // PMX preserves the complete allocation, including embedded NULs.
            // This string record cannot represent a nonzero tail losslessly;
            // omit only this language so reload retains its source allocation.
            const auto* bytes = reinterpret_cast<const unsigned char*>(unicode);
            for (auto at = (length + 1) * sizeof(wchar_t); at < allocation.bytes; ++at)
                if (bytes[at]) return false;
            value.assign(unicode, length);
        } else value = unicode;
        return true;
    }
    const std::string bytes(raw, strnlen_s(raw, capacity));
    // The legacy PMD loader can NUL-terminate a full-width field in the middle
    // of its last CP932 character. Keep that source-language field untouched
    // on reload; do not reject the entire project or invent replacement text.
    return !text_encoding::HasUtf8Bom(bytes.c_str()) && text_encoding::Decode(bytes.c_str(), 932, value);
}
bool PmdField(const char* raw, std::size_t capacity, const wchar_t* unicode) {
    const std::string bytes(raw, strnlen_s(raw, capacity));
    if (unicode) {
        std::string encoded;
        return original_file_encoding::Encode(unicode, capacity, encoded) && encoded == bytes;
    }
    std::wstring decoded;
    return !text_encoding::HasUtf8Bom(bytes.c_str()) && text_encoding::Decode(bytes.c_str(), 932, decoded);
}
}
bool Assign(wchar_t*& destination,const std::wstring& value) noexcept {
    if(value.size()>(SIZE_MAX/sizeof(wchar_t))-1)return false;
    auto* copy=static_cast<wchar_t*>(std::malloc((value.size()+1)*sizeof(wchar_t)));
    if(!copy)return false;
    std::memcpy(copy,value.c_str(),(value.size()+1)*sizeof(wchar_t));
    try { owned_text::Remember(copy,(value.size()+1)*sizeof(wchar_t)); }
    catch(...) { std::free(copy);return false; }
    owned_text::Free(destination);destination=copy;return true;
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
bool CanSavePmdNames(const mdl::ModelRecord& m) {
    if (!PmdField(m.name, sizeof m.name, m.pmxTextBuffers[0]) ||
        !PmdField(m.nameEn, sizeof m.nameEn, m.pmxTextBuffers[1]) ||
        !PmdField(m.comment, sizeof m.comment, m.pmxTextBuffers[2]) ||
        !PmdField(m.commentEn, sizeof m.commentEn, m.pmxTextBuffers[3])) return false;
    if ((m.boneCount && !m.boneTable) || (m.morphCount && !m.morphs) ||
        (m.rigidCount && !m.rigidTable) || (m.jointCount && !m.jointTable)) return false;
    for (std::uint32_t i = 0; i < m.boneCount; ++i) {
        const auto& bone = m.boneTable[i];
        if (!PmdField(bone.name, sizeof bone.name, bone.jpText) ||
            !PmdField(bone.nameEn, sizeof bone.nameEn, bone.enText)) return false;
    }
    for (std::uint32_t i = 0; i < m.morphCount; ++i) {
        const auto& morph = m.morphs[i];
        if (!PmdField(morph.name, sizeof morph.name, morph.jpText) ||
            (i && !PmdField(morph.nameEn, sizeof morph.nameEn, morph.enText))) return false;
    }
    const auto count = mdl::DisplayGroupCount(m), first = mdl::FacialFrameCount(m) ? 3u : 2u;
    const auto* groups = static_cast<const mdl::DisplayGroup*>(m.groupNames);
    if (count > first && !groups) return false;
    for (std::uint32_t i = first; i < count; ++i) {
        text_encoding::DisplayGroupTextState state;
        if (!text_encoding::CaptureDisplayGroupText(m, groups[i], state) ||
            !PmdField(groups[i].name, sizeof groups[i].name, state.present ? state.names[0].c_str() : nullptr) ||
            !PmdField(groups[i].nameEn, sizeof groups[i].nameEn, state.present ? state.names[1].c_str() : nullptr)) return false;
    }
    for (std::uint32_t i = 0; i < m.rigidCount; ++i)
        if (!PmdField(m.rigidTable[i].name, sizeof m.rigidTable[i].name, m.rigidTable[i].jpText)) return false;
    for (std::uint32_t i = 0; i < m.jointCount; ++i)
        if (!PmdField(m.jointTable[i].name, sizeof m.jointTable[i].name, m.jointTable[i].jpText)) return false;
    return true;
}
void CapturePmm(const MMDApp& app,pmm_extension::Document& doc) {
    for(int slot=0;slot<kModelSlotCount;++slot) {
        const auto* raw=app.ModelSlot(slot);if(!raw){pmm_extension::Erase(doc,Key(slot));pmm_extension::Erase(doc,CommentKey(slot));continue;}
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
        Writer comments; comments.u32(1); std::array<std::wstring,2> text; std::uint32_t present=0;
        if(Comment(m.comment,sizeof m.comment,m.pmxTextBuffers[2],text[0]))present|=1;
        if(Comment(m.commentEn,sizeof m.commentEn,m.pmxTextBuffers[3],text[1]))present|=2;
        comments.u32(present);for(unsigned language=0;language<2;++language)if(present&(1u<<language))comments.text(text[language]);
        pmm_extension::Set(doc,CommentKey(slot),std::move(comments.data));
    }
}
void LoadPmm(MMDApp& app,const pmm_extension::Document& doc,
             const std::vector<int>& adaptedModelSlots) {
    struct Free { void operator()(wchar_t* p) const noexcept { owned_text::Free(p); } };
    struct Replacement { wchar_t** target; std::unique_ptr<wchar_t, Free> value; };
    struct Prepared {
        std::vector<Replacement> text;
        std::vector<std::pair<mdl::DisplayGroup*, std::uint8_t>> flags;
        text_encoding::PreparedDisplayGroupTextBatch groups;
        bool hasGroups = false;
        void Add(wchar_t*& target, const std::wstring& value) {
            wchar_t* raw = nullptr; if (!Assign(raw, value)) throw std::bad_alloc();
            std::unique_ptr<wchar_t, Free> next(raw);
            text.push_back({&target, std::move(next)});
        }
    };
    std::vector<Prepared> prepared;
    for(int slot=0;slot<kModelSlotCount;++slot) {
        auto* raw=app.ModelSlot(slot);if(!raw)continue;
        const auto* entry=pmm_extension::Find(doc,Key(slot));
        const auto* comments=pmm_extension::Find(doc,CommentKey(slot));
        if(!entry&&!comments)continue;
        auto& m=*mdl::Mdl(raw); Prepared next;
        const bool adapted = std::find(adaptedModelSlots.begin(), adaptedModelSlots.end(), slot) != adaptedModelSlots.end();
        if(entry && adapted)
            runtime_log::Write("PMM Unicode names ignored for adapted model slot %d", slot);
        if(entry && !adapted) {
            const auto prepareNames = [&] {
                Reader r{entry->value.data(),entry->value.size()};
                if(r.u32()==1) {
                    std::vector<std::wstring> values;values.push_back(r.text());values.push_back(r.text());
                    if(r.u32()!=m.boneCount) {
                        runtime_log::Write("PMM Unicode names ignored for changed bone table in slot %d", slot); return;
                    }
                    for(std::uint32_t i=0;i<m.boneCount;++i)for(int l=0;l<2;++l)values.push_back(r.text());
                    if(r.u32()!=m.morphCount) {
                        runtime_log::Write("PMM Unicode names ignored for changed morph table in slot %d", slot); return;
                    }
                    for(std::uint32_t i=0;i<m.morphCount;++i)for(int l=0;l<2;++l)values.push_back(r.text());
                    const auto count=r.u32(),faces=r.u32();
                    if(count!=mdl::DisplayGroupCount(m)||faces!=mdl::FacialFrameCount(m)) {
                        runtime_log::Write("PMM Unicode names ignored for changed display groups in slot %d", slot); return;
                    }
                    std::vector<std::array<std::wstring,2>> names(count);std::vector<std::uint8_t> flags(count);
                    for(std::uint32_t i=0;i<count;++i){names[i][0]=r.text();names[i][1]=r.text();const auto f=r.u32();if(f>255)throw std::runtime_error("Invalid group state");flags[i]=std::uint8_t(f);}
                    if(r.at!=r.size)throw std::runtime_error("Unexpected Unicode name bytes");
                    if((m.boneCount&&!m.boneTable)||(m.morphCount&&!m.morphs)||(count&&!m.groupNames))
                        throw std::runtime_error("Missing Unicode model tables");
                    std::size_t at=0;
                    for(int l=0;l<2;++l)next.Add(m.pmxTextBuffers[l],values[at++]);
                    for(std::uint32_t i=0;i<m.boneCount;++i){next.Add(m.boneTable[i].jpText,values[at++]);next.Add(m.boneTable[i].enText,values[at++]);}
                    for(std::uint32_t i=0;i<m.morphCount;++i){next.Add(m.morphs[i].jpText,values[at++]);next.Add(m.morphs[i].enText,values[at++]);}
                    auto* table=static_cast<mdl::DisplayGroup*>(m.groupNames);
                    std::vector<std::pair<const mdl::DisplayGroup*,text_encoding::DisplayGroupTextState>> targets;
                    targets.reserve(count); next.flags.reserve(count);
                    for(std::uint32_t i=0;i<count;++i) {
                        text_encoding::DisplayGroupTextState state;state.present=true;state.names=std::move(names[i]);
                        state.legacy={std::string(table[i].name,strnlen_s(table[i].name,sizeof table[i].name)),
                            std::string(table[i].nameEn,strnlen_s(table[i].nameEn,sizeof table[i].nameEn))};
                        targets.emplace_back(&table[i],std::move(state));next.flags.emplace_back(&table[i],flags[i]);
                    }
                    if(!targets.empty()) {
                        if(!next.groups.Prepare(m,targets))throw std::runtime_error("Invalid Unicode display group owner");
                        next.hasGroups=true;
                    }
                }
            };
            prepareNames();
        }
        if(comments) {
            Reader r{comments->value.data(),comments->value.size()};
            if(r.u32()==1) {
                const auto present=r.u32();if(present&~3u)throw std::runtime_error("Invalid Unicode comment presence");
                std::array<std::wstring,2> text;
                for(unsigned language=0;language<2;++language)if(present&(1u<<language))text[language]=r.text();
                if(r.at!=r.size)throw std::runtime_error("Unexpected Unicode comment bytes");
                for(unsigned language=0;language<2;++language)if(present&(1u<<language))next.Add(m.pmxTextBuffers[2+language],text[language]);
            }
        }
        prepared.push_back(std::move(next));
    }
    // All models, comments and sidecar nodes are ready before the first write.
    // These operations do not pump messages or call plugins; publication only
    // hands off registered malloc buffers, existing map nodes and flag bytes.
    for(const auto& slot:prepared)if(slot.hasGroups&&!slot.groups.IsCurrent())
        throw std::runtime_error("Unicode display group source changed");
    for(auto& slot:prepared) {
        for(auto& text:slot.text){auto* old=*text.target;*text.target=text.value.release();owned_text::Free(old);}
        if(slot.hasGroups)slot.groups.Publish();
        for(const auto& flag:slot.flags)flag.first->flags=flag.second;
    }
}
std::vector<std::uint8_t> EncodeMotion(const MotionNames& names,std::uint64_t prefixSize,std::uint32_t prefixCrc) {
    Writer w;w.text(names.model);w.names(names.bones);w.names(names.morphs);w.indices(names.boneOwners);w.indices(names.morphOwners);w.indices(names.ikOwners);
    Writer out;const char magic[]="MMSNAME1";out.data.insert(out.data.end(),magic,magic+8);
    out.u64(w.data.size());out.u64(prefixSize);out.u32(prefixCrc);out.u32(Crc(w.data.data(),w.data.size()));
    out.data.insert(out.data.end(),w.data.begin(),w.data.end());return out.data;
}
std::size_t DecodeMotion(const std::uint8_t* file,std::size_t size,std::size_t offset,MotionNames& out) {
    if(offset>size||size-offset<8||std::memcmp(file+offset,"MMSNAME1",8))return 0;
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
