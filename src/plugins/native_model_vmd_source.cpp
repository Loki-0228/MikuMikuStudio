#include "native_model_vmd_source.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../model/vmd_read_guard.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include <cerrno>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <map>
#include <share.h>
#include <sys/stat.h>
namespace mikudancestudio::plugins {
namespace {
struct ModelVmdDescriptor {int value=-1;~ModelVmdDescriptor(){if(value>=0)_close(value);}};
template<class T>T ModelVmdValue(const std::uint8_t* bytes){T value;std::memcpy(&value,bytes,sizeof(value));return value;}
void ModelVmdName(char* destination,std::size_t capacity,const std::uint8_t* bytes,std::size_t length){
    std::size_t count=0;while(count<length&&bytes[count]&&bytes[count]!='\n')++count;
    std::memcpy(destination,bytes,(std::min)(capacity-1,count));
}
bool ModelVmdFinite(const std::uint8_t* bytes,unsigned count){
    for(unsigned i=0;i<count;++i)if(!std::isfinite(ModelVmdValue<float>(bytes+4*i)))return false;return true;
}
MmsResult ResolveModelVmdScales(const mdl::ModelRecord& model,ModelVmdSource& source,
                              const std::vector<int>* unicodeTargets){
    if(source.scales.entries.empty())return MMS_OK;
    for(auto& record:source.source.bones){record.scale={};record.scaleCurve=bone_scale::LinearCurve();}
    // MatchImported uses destination names, with the exact index placeholders
    // the legacy Unicode import installed. Several dictionary owners of one
    // destination retain the legacy last-owner rule.
    std::vector<std::array<char,20>> names(model.boneCount);
    if(unicodeTargets){
        for(std::size_t i=0;i<unicodeTargets->size();++i){const auto bone=(*unicodeTargets)[i];
            if(bone<0||unsigned(bone)>=model.boneCount)continue;
            const auto placeholder=extended_names::MotionIndexName(static_cast<std::uint32_t>(i));
            std::memcpy(names[bone].data(),placeholder.data(),placeholder.size());}
    }else for(unsigned i=0;i<model.boneCount;++i)std::memcpy(names[i].data(),model.boneTable[i].name,names[i].size());
    std::map<std::string,int> nativeNames;
    if(!unicodeTargets)for(unsigned i=0;i<model.boneCount;++i)
        nativeNames.emplace(std::string(model.boneTable[i].name,strnlen_s(model.boneTable[i].name,sizeof(model.boneTable[i].name))),int(i));
    // A standard motion may repeat one bone/frame. The registrar overwrites
    // the same key, so only the final source record represents that imported
    // slot. Preserve MatchImported's refusal of genuinely ambiguous names.
    std::map<std::pair<int,unsigned>,int> finalRecords;
    for(std::size_t i=0;i<source.source.bones.size();++i){const auto& record=source.source.bones[i];int bone=record.targetIndex;
        if(!record.targetResolved){const auto found=nativeNames.find(std::string(record.key.name,strnlen_s(record.key.name,sizeof(record.key.name))));
            bone=found==nativeNames.end()?-1:found->second;}
        if(bone<0)continue;if(unsigned(bone)>=model.boneCount)return MMS_E_ARGUMENT;
        unsigned frame;if(!frame_limits::Add(int(source.source.frameOffset),record.key.frame,frame))return MMS_E_ARGUMENT;
        finalRecords[{bone,frame}]=int(i);
    }
    std::vector<bone_scale_io::ImportedKey> imported;imported.reserve(finalRecords.size());
    for(const auto& item:finalRecords)imported.push_back({item.first.first,item.first.second,item.second});
    const auto assignments=bone_scale_io::MatchImported(source.scales,imported,names,source.source.frameOffset);
    for(const auto& assignment:assignments){auto& record=source.source.bones[assignment.slot];record.scale=assignment.scale;record.scaleCurve=assignment.curve;}
    return MMS_OK;
}
template<class Record>struct VmdTargetName {
    std::array<char,sizeof(Record::name)> name{},english{};
    std::wstring japaneseText,englishText;
    explicit VmdTargetName(const Record& record):japaneseText(record.jpText?record.jpText:L""),englishText(record.enText?record.enText:L""){
        std::memcpy(name.data(),record.name,name.size());std::memcpy(english.data(),record.nameEn,english.size());
    }
    bool Current(const Record& record)const noexcept{
        return !std::memcmp(name.data(),record.name,name.size())&&!std::memcmp(english.data(),record.nameEn,english.size())&&
            (record.jpText?japaneseText==record.jpText:japaneseText.empty())&&(record.enText?englishText==record.enText:englishText.empty());
    }
    std::uint64_t Bytes()const noexcept{return (japaneseText.capacity()+englishText.capacity()+2)*sizeof(wchar_t);}
};
struct ModelVmdMappedChange final:HistoryChange,std::enable_shared_from_this<ModelVmdMappedChange>{
    MMDApp& app;unsigned slot;unsigned char* pointer;mdl::BoneRecord* bones;mdl::MorphRecord* morphs;
    std::vector<VmdTargetName<mdl::BoneRecord>> boneNames;
    std::vector<VmdTargetName<mdl::MorphRecord>> morphNames;
    std::shared_ptr<HistoryChange> inner;bool published=false;std::uint64_t generation=1;
    ModelVmdMappedChange(MMDApp& a,unsigned s):app(a),slot(s),pointer(a.ModelSlot(s)){
        const auto& model=*mdl::Mdl(pointer);bones=model.boneTable;morphs=model.morphs;
        if((model.boneCount&&!bones)||(model.morphCount&&!morphs))throw std::invalid_argument("missing VMD target names");
        boneNames.reserve(model.boneCount);morphNames.reserve(model.morphCount);
        for(unsigned i=0;i<model.boneCount;++i)boneNames.emplace_back(bones[i]);
        for(unsigned i=0;i<model.morphCount;++i)morphNames.emplace_back(morphs[i]);
    }
    bool CurrentNames()const noexcept{
        if(app.ModelSlot(slot)!=pointer)return false;const auto& model=*mdl::Mdl(pointer);
        if(model.boneTable!=bones||model.morphs!=morphs||model.boneCount!=boneNames.size()||model.morphCount!=morphNames.size())return false;
        for(std::size_t i=0;i<boneNames.size();++i)if(!boneNames[i].Current(bones[i]))return false;
        for(std::size_t i=0;i<morphNames.size();++i)if(!morphNames[i].Current(morphs[i]))return false;return true;
    }
    std::uint64_t OwnBytes()const noexcept{
        auto bytes=std::uint64_t(sizeof(*this)+4*sizeof(void*)+boneNames.capacity()*sizeof(boneNames[0])+morphNames.capacity()*sizeof(morphNames[0]));
        for(const auto& name:boneNames)bytes+=name.Bytes();for(const auto& name:morphNames)bytes+=name.Bytes();return bytes;
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<ModelVmdMappedChange> source;std::unique_ptr<PreparedHistoryChange> inner;std::uint64_t generation;bool first,consumed=false;
        explicit Candidate(std::shared_ptr<ModelVmdMappedChange> s):source(std::move(s)),generation(source->generation),first(!source->published){}
        bool IsCurrent()const noexcept override{return !consumed&&generation==source->generation&&inner&&inner->IsCurrent()&&(!first||source->CurrentNames());}
        void Publish()noexcept override{if(!IsCurrent())std::terminate();inner->Publish();source->published=true;++source->generation;consumed=true;}
        bool TargetRetainedBytes(std::uint64_t& bytes)const noexcept override{if(!inner->TargetRetainedBytes(bytes))return false;const auto own=source->OwnBytes();bytes=bytes>UINT64_MAX-own?UINT64_MAX:bytes+own;return true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();if(generation==UINT64_MAX)return MMS_E_NOT_READY;if(!published&&(!forward||!CurrentNames()))return MMS_E_STALE;
        return Boundary([&]()->MmsResult{auto candidate=std::make_unique<Candidate>(shared_from_this());if(auto result=inner->Prepare(forward,candidate->inner))return result;
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;});
    }
    std::uint64_t RetainedBytes()const noexcept override{const auto bytes=inner->RetainedBytes(),own=OwnBytes();return bytes>UINT64_MAX-own?UINT64_MAX:bytes+own;}
};
}
MmsResult DecodeModelVmdSource(const char* ansi,const wchar_t* wide,std::uint32_t offset,ModelVmdSource& out)noexcept{
    if((!ansi&&!wide)||offset>unsigned(frame_limits::maximum))return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        ModelVmdDescriptor file;const auto error=wide?_wsopen_s(&file.value,wide,_O_BINARY|_O_RDONLY,_SH_DENYWR,_S_IREAD):
            _sopen_s(&file.value,ansi,_O_BINARY|_O_RDONLY,_SH_DENYWR,_S_IREAD);
        if(error)return error==ENOENT||error==ENOTDIR?MMS_E_NOT_FOUND:MMS_E_FAILED;
        vmd_read::Input input;if(!input.Load(file.value))return MMS_E_ARGUMENT;
        unsigned maximum;if(!frame_limits::Add(int(offset),input.maximumFrame,maximum))return MMS_E_ARGUMENT;
        if(input.boneCount>mdl::KeyPoolLimit<mdl::BoneKey>()||input.morphCount>mdl::KeyPoolLimit<mdl::MorphKey>()||
            input.displayCount>mdl::KeyPoolLimit<mdl::DisplayKey>())return MMS_E_ARGUMENT;
        ModelVmdSource decoded;decoded.version=input.version;decoded.source.frameOffset=offset;decoded.source.advanceLastRegisteredFrame=true;
        decoded.source.resetDisplayCursor=input.displayOffset!=vmd_read::Input::absent;
        const auto nameSize=input.version==1?10u:20u;const auto* name=reinterpret_cast<const char*>(input.Data(30));
        decoded.modelName.assign(name,strnlen_s(name,nameSize));
        constexpr char cameraName[]="\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
        if(decoded.modelName==cameraName)return MMS_E_ARGUMENT;
        auto at=std::size_t(30+nameSize+4);decoded.source.bones.reserve(input.boneCount);
        constexpr unsigned curveOffsets[]={0,16,32,48,4,20,36,52,8,24,40,56,12,28,44,60};
        for(unsigned i=0;i<input.boneCount;++i,at+=111){
            const auto* bytes=input.Data(at);if(!ModelVmdFinite(bytes+19,7))return MMS_E_ARGUMENT;
            ModelBonePasteRecord record;auto& key=record.key;ModelVmdName(key.name,sizeof(key.name),bytes,15);
            key.frame=ModelVmdValue<std::uint32_t>(bytes+15);std::memcpy(key.position,bytes+19,sizeof(key.position));
            std::memcpy(key.rotation,bytes+31,sizeof(key.rotation));
            key.physicsDisabled=ModelVmdValue<std::uint16_t>(bytes+49)==3939;
            for(unsigned channel=0;channel<16;++channel){key.interpolation[channel]=bytes[47+curveOffsets[channel]];if(key.interpolation[channel]>127)return MMS_E_ARGUMENT;}
            decoded.source.bones.push_back(record);
        }
        at+=4;decoded.source.morphs.reserve(input.morphCount);
        for(unsigned i=0;i<input.morphCount;++i,at+=23){const auto* bytes=input.Data(at);ModelMorphPasteRecord record;
            ModelVmdName(record.key.name,sizeof(record.key.name),bytes,15);record.key.frame=ModelVmdValue<std::uint32_t>(bytes+15);
            record.key.value=ModelVmdValue<float>(bytes+19);if(!std::isfinite(record.key.value))record.key.value=0;
            decoded.source.morphs.push_back(record);
        }
        decoded.source.displays.reserve(input.displayCount);
        if(input.displayOffset!=vmd_read::Input::absent){at=input.displayOffset+4;
            for(unsigned i=0;i<input.displayCount;++i){ModelDisplayPasteRecord record;const auto* bytes=input.Data(at);
                record.frame=ModelVmdValue<std::uint32_t>(bytes);record.visible=bytes[4]!=0;const auto count=ModelVmdValue<std::uint32_t>(bytes+5);at+=9;
                record.ik.reserve(count);for(unsigned j=0;j<count;++j,at+=21){mdl::IkClipboardState state{};const auto* entry=input.Data(at);
                    // The legacy IK name field is 20 bytes and may be full.
                    std::memcpy(state.boneName,entry,20);state.enabled=entry[20]!=0;record.ik.push_back(state);}
                decoded.source.displays.push_back(std::move(record));
            }
        }
        // Dictionary checksums bind identities to these same immutable bytes.
        auto tail=input.standardEnd;const auto namesBytes=extended_names::DecodeMotion(input.Data(0),input.Size(),tail,decoded.names);
        decoded.hasUnicodeNames=namesBytes!=0;tail+=namesBytes;
        if(decoded.hasUnicodeNames&&(decoded.names.boneOwners.size()!=input.boneCount||decoded.names.morphOwners.size()!=input.morphCount||decoded.names.ikOwners.size()!=input.totalIkEntries))return MMS_E_ARGUMENT;
        // Match the legacy consumption order. A bad morph CRC does not
        // advance the cursor, so subsequent bone/scale blocks stay hidden.
        const auto morphBytes=morph_curve::VmdBlockSize(input.Data(tail),input.Size()-tail);
        if(morphBytes&&morphBytes<=input.Size()-tail){
            std::vector<std::uint8_t> payload;
            if(morph_curve::DecodeVmdBlock(input.Data(tail),morphBytes,payload)==morphBytes){
                decoded.source.morphCurveBlock.assign(input.Data(tail),input.Data(tail)+morphBytes);tail+=morphBytes;
            }
        }
        const auto boneBytes=bone_curve::VmdBlockSize(input.Data(tail),input.Size()-tail);
        if(boneBytes){decoded.source.boneCurveBlock.assign(input.Data(tail),input.Data(tail)+boneBytes);tail+=boneBytes;}
        // The tail is checked against the same bytes used above, including a
        // Unicode dictionary in the prefix. A damaged/stale scale extension
        // follows the established loader policy: ignore it as a whole while
        // retaining the standard motion. Allocation failure still aborts all.
        bone_scale_io::LoadVmdExtensionBytes(input.Data(0),input.Size(),decoded.scales,std::int64_t(tail));
        if(auto status=ValidateModelPasteSource(decoded.source))return status;
        out=std::move(decoded);return MMS_OK;
    });
}
MmsResult ResolveModelVmdTargets(MMDApp& app,unsigned slot,ModelVmdSource& source)noexcept{
    if(slot>=kModelSlotCount||!app.ModelSlot(slot))return MMS_E_ARGUMENT;
    if(!source.hasUnicodeNames&&source.scales.entries.empty())return MMS_OK;
    return Boundary([&]()->MmsResult{
        auto resolved=source;const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if((model.boneCount&&!model.boneTable)||(model.morphCount&&!model.morphs))return MMS_E_ARGUMENT;
        if(!source.hasUnicodeNames){if(auto result=ResolveModelVmdScales(model,resolved,nullptr))return result;source=std::move(resolved);return MMS_OK;}
        std::vector<std::wstring> names;names.reserve(model.boneCount);
        for(unsigned i=0;i<model.boneCount;++i)names.push_back(text_encoding::BoneName(model.boneTable[i],false));
        const auto bones=extended_names::MatchNames(resolved.names.bones,names);names.clear();names.reserve(model.morphCount);
        for(unsigned i=0;i<model.morphCount;++i)names.push_back(text_encoding::MorphName(model.morphs[i],false));
        const auto morphs=extended_names::MatchNames(resolved.names.morphs,names);
        resolved.source.curveTargetsResolved=true;
        resolved.source.boneCurveTargets=bones;resolved.source.morphCurveTargets=morphs;
        if(resolved.names.boneOwners.size()!=resolved.source.bones.size()||resolved.names.morphOwners.size()!=resolved.source.morphs.size())return MMS_E_ARGUMENT;
        for(std::size_t i=0;i<resolved.source.bones.size();++i){const auto owner=resolved.names.boneOwners[i];if(owner>=bones.size())return MMS_E_ARGUMENT;
            auto& key=resolved.source.bones[i];key.targetResolved=true;key.targetIndex=bones[owner];}
        for(std::size_t i=0;i<resolved.source.morphs.size();++i){const auto owner=resolved.names.morphOwners[i];if(owner>=morphs.size())return MMS_E_ARGUMENT;
            auto& key=resolved.source.morphs[i];key.targetResolved=true;key.targetIndex=morphs[owner];}
        std::size_t cursor=0;for(auto& key:resolved.source.displays){key.ikTargets.clear();key.ikTargets.reserve(key.ik.size());
            for(std::size_t j=0;j<key.ik.size();++j){if(cursor>=resolved.names.ikOwners.size())return MMS_E_ARGUMENT;
                const auto owner=resolved.names.ikOwners[cursor++];if(owner!=UINT32_MAX&&owner>=bones.size())return MMS_E_ARGUMENT;
                key.ikTargets.push_back(owner==UINT32_MAX?-1:bones[owner]);}key.ikTargetsResolved=true;}
        if(cursor!=resolved.names.ikOwners.size())return MMS_E_ARGUMENT;
        if(auto result=ResolveModelVmdScales(model,resolved,&bones))return result;
        source=std::move(resolved);return MMS_OK;
    });
}
MmsResult PlanModelVmdImport(MMDApp& app,unsigned slot,const ModelVmdSource& source,std::shared_ptr<HistoryChange>& out,ModelPasteReport* report)noexcept{
    out.reset();if(report)*report={};if(slot>=kModelSlotCount||!app.ModelSlot(slot))return MMS_E_ARGUMENT;
    if(app.CameraMode()||app.PlaybackActive()||app.FrameStepPlayback()||app.SceneMutationInProgress()||app.state.recordPlaybackStartPending)return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{
        auto wrapper=std::make_shared<ModelVmdMappedChange>(app,slot);auto resolved=source;
        if(auto result=ResolveModelVmdTargets(app,slot,resolved))return result;
        if(auto result=PlanModelKeyPaste(app,slot,resolved.source,wrapper->inner,report))return result;
        if(wrapper->inner)out=std::move(wrapper);return MMS_OK;
    });
}
}
#endif
