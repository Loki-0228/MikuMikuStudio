#include "native_vmd_import.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_vmd_source.hpp"
#include "native_model_vmd_source.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "native_history_commands.hpp"
#include "native_morph_commands.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/model.hpp"
#include <array>
#include <algorithm>
#include <cstring>
namespace mikumikustudio::plugins {
MmsResult CheckVmdImportHistory(MMDApp* app) noexcept {
    if (!app) return MMS_E_ARGUMENT;
    return BoundNativePoseHistory(app) ? MMS_E_NOT_READY : MMS_OK;
}
bool RefuseBoundVmdImport(MMDApp*) noexcept { return false; }

namespace {
bool ImportControlsReady(MMDApp& app) noexcept {
    return !app.PlaybackActive()&&!app.FrameStepPlayback()&&!app.state.recordPlaybackStartPending&&!app.SceneMutationInProgress()&&
        !bake::UiLocked()&&!SceneSaveBlocksEdits()&&!GetCapture()&&!app.FrameRangeDialog()&&!app.state.frameCopyDialog&&
        !app.RecordingWindow()&&!app.GravitySettingDialog()&&!app.GroundShadowColorDialog()&&!app.AccessoryOrderArray()&&
        !app.state.accessoryEditDialogOpen&&!NativeModelOutlineDialogBlocks(app);
}
bool ImportReady(MMDApp& app,SharedHistory& history) noexcept {
    return ImportControlsReady(app)&&history.CheckNativeEdit()==MMS_OK&&ImportControlsReady(app);
}
// Matching depends on the complete visible accessory set. Freeze it only for
// the first publication; subsequent history uses the already resolved slots.
struct VmdAccessoryMapping {
    struct Object {unsigned slot;mdl::AccessoryRecord* pointer;MmsId id;std::uint8_t order;std::array<char,99> name{};std::size_t length=0;};
    struct Parent {unsigned slot;unsigned char* pointer;MmsId id;mdl::BoneRecord* bones;unsigned count;};
    MMDApp& app;std::array<mdl::AccessoryRecord*,255> slots{};std::vector<Object> objects;std::vector<Parent> parents;
    explicit VmdAccessoryMapping(MMDApp& a):app(a){
        for(unsigned slot=0;slot<slots.size();++slot)if((slots[slot]=a.AccessorySlot(slot))){
            auto* object=slots[slot];Object saved{slot,object,SceneObjectIdentity(object).id,object->order};
            saved.length=strnlen_s(object->name,99);std::memcpy(saved.name.data(),object->name,saved.length);objects.push_back(saved);
        }
        std::stable_sort(objects.begin(),objects.end(),[](const auto& x,const auto& y){return x.order<y.order;});
    }
    void ParentAt(int index){
        if(index<0||index>=int(kModelSlotCount))return;
        if(std::any_of(parents.begin(),parents.end(),[&](const auto& p){return p.slot==unsigned(index);}))return;
        auto* model=app.ModelSlot(unsigned(index));auto* m=model?mdl::Mdl(model):nullptr;
        parents.push_back({unsigned(index),model,model?SceneObjectIdentity(model).id:MmsId{},m?m->boneTable:nullptr,m?m->boneCount:0});
    }
    bool Current()const noexcept {
        for(unsigned i=0;i<slots.size();++i)if(app.AccessorySlot(i)!=slots[i])return false;
        for(const auto& object:objects){if(!SceneObjectPresent(object.pointer,object.id)||object.pointer->order!=object.order||
            std::memcmp(object.pointer->name,object.name.data(),object.length)||
            (object.length<99&&object.pointer->name[object.length]))return false;}
        for(const auto& parent:parents){if(app.ModelSlot(parent.slot)!=parent.pointer)return false;
            if(parent.pointer&&(!SceneObjectPresent(parent.pointer,parent.id)||mdl::Mdl(parent.pointer)->boneTable!=parent.bones||mdl::Mdl(parent.pointer)->boneCount!=parent.count))return false;}
        return true;
    }
    std::uint64_t RetainedBytes()const noexcept {return sizeof(*this)+objects.capacity()*sizeof(Object)+parents.capacity()*sizeof(Parent);}
};
struct VmdMappedChange final:HistoryChange,std::enable_shared_from_this<VmdMappedChange>{
    std::shared_ptr<HistoryChange> inner;std::shared_ptr<VmdAccessoryMapping> mapping;bool published=false;std::uint64_t generation=1;
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<VmdMappedChange> source;std::unique_ptr<PreparedHistoryChange> inner;std::uint64_t generation;bool consumed=false,first;
        explicit Candidate(std::shared_ptr<VmdMappedChange> s):source(std::move(s)),generation(source->generation),first(!source->published){}
        bool IsCurrent()const noexcept override{return !consumed&&source->generation==generation&&(!first||source->mapping->Current())&&inner&&inner->IsCurrent();}
        void Publish()noexcept override{if(!IsCurrent())std::terminate();inner->Publish();source->published=true;++source->generation;consumed=true;}
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override{
            std::uint64_t bytes;if(!inner->TargetRetainedBytes(bytes))return false;const auto own=std::uint64_t(sizeof(VmdMappedChange)+4*sizeof(void*))+source->mapping->RetainedBytes();out=own>UINT64_MAX-bytes?UINT64_MAX:own+bytes;return true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();if(generation==UINT64_MAX)return MMS_E_NOT_READY;if(!published&&(!forward||!mapping->Current()))return MMS_E_STALE;
        return Boundary([&]()->MmsResult{auto candidate=std::make_unique<Candidate>(shared_from_this());if(auto result=inner->Prepare(forward,candidate->inner))return result;
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;});
    }
    std::uint64_t RetainedBytes()const noexcept override{const auto own=std::uint64_t(sizeof(*this)+4*sizeof(void*))+mapping->RetainedBytes();const auto bytes=inner->RetainedBytes();return bytes>UINT64_MAX-own?UINT64_MAX:own+bytes;}
};
MmsResult PlanVmd(MMDApp& app,const char* ansiPath,const wchar_t* widePath,std::shared_ptr<HistoryChange>& out,std::array<bool,255>* matched){
    out.reset();if(matched)matched->fill(false);
    if(!app.CameraMode()||app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        GlobalVmdSource decoded;if(auto result=DecodeGlobalVmdSource(ansiPath,widePath,unsigned(app.CurrentFrame()),decoded))return result;
        std::shared_ptr<VmdAccessoryMapping> mapping;
        if(!decoded.accessoryTracks.empty()){
            mapping=std::make_shared<VmdAccessoryMapping>(app);std::vector<std::string> names;names.reserve(mapping->objects.size());
            for(const auto& object:mapping->objects)names.emplace_back(object.name.data(),object.length);
            const auto matches=accessory_motion_io::MatchTracks(names,decoded.accessoryTracks);unsigned ignored=0;
            for(std::size_t track=0;track<decoded.accessoryTracks.size();++track){if(matches[track]<0){++ignored;continue;}
                const auto slot=mapping->objects[std::size_t(matches[track])].slot;
                for(const auto& key:decoded.accessoryTracks[track].keys){
                    mapping->ParentAt(key.parentModel);mdl::AccessoryClipboardKey record{};record.slot=std::uint8_t(slot);record.frameOffset=key.frame;
                    record.visible=key.visible;record.shadowEnabled=key.shadowEnabled;
                    auto* parent=key.parentModel>=0&&key.parentModel<int(kModelSlotCount)?app.ModelSlot(unsigned(key.parentModel)):nullptr;
                    record.parentModel=parent?key.parentModel:-1;record.parentBone=parent?key.parentBone:0;
                    if(parent&&(!mdl::Mdl(parent)->boneTable||key.parentBone<0||unsigned(key.parentBone)>=mdl::Mdl(parent)->boneCount))return MMS_E_ARGUMENT;
                    std::memcpy(record.position,key.position,sizeof(record.position));std::memcpy(record.rotation,key.rotation,sizeof(record.rotation));record.scale=key.scale;record.opacity=key.opacity;
                    decoded.source.accessories.push_back(record);if(matched)(*matched)[slot]=true;
                }
            }
            if(ignored)runtime_log::Write("VMD accessory tracks ignored: %u unmatched names",ignored);
        }
        std::shared_ptr<HistoryChange> change;if(auto result=PlanGlobalKeyPaste(app,decoded.source,change))return result;if(!change)return MMS_OK;
        if(mapping){auto wrapper=std::make_shared<VmdMappedChange>();wrapper->inner=std::move(change);wrapper->mapping=std::move(mapping);out=std::move(wrapper);}
        else out=std::move(change);return MMS_OK;
    });
}
}
MmsResult PlanGlobalVmdImport(MMDApp& app,const char* ansiPath,const wchar_t* widePath,std::shared_ptr<HistoryChange>& out)noexcept{return PlanVmd(app,ansiPath,widePath,out,nullptr);}

bool TryImportBoundVmd(MMDApp* app,const char* ansiPath,const wchar_t* widePath,int& result) noexcept {
    if(!app)return false;auto* history=BoundNativePoseHistory(app);if(!history)return false;
    result=0;
    if(!ImportReady(*app,*history))return true;
    bool committed=false,boneCurvesSkipped=false;
    const auto status=Boundary([&]()->MmsResult {
        std::shared_ptr<HistoryChange> change;std::array<bool,255> matched{};
        const bool modelMode=!app->CameraMode();
        if(modelMode){
            const auto slot=app->SelectedModelSlot();if(slot>=kModelSlotCount||!app->ModelSlot(slot))return MMS_E_NOT_READY;
            if(app->CurrentFrame()<0||app->CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            ModelVmdSource source;if(auto error=DecodeModelVmdSource(ansiPath,widePath,unsigned(app->CurrentFrame()),source))return error;
            const auto& target=*mdl::Mdl(app->ModelSlot(slot));
            const bool same=source.hasUnicodeNames?source.names.model==text_encoding::ModelName(target,false):
                source.modelName==std::string(target.name,strnlen_s(target.name,sizeof(target.name)));
            if(!same){
                auto* pointer=app->ModelSlot(slot);const auto identity=SceneObjectIdentity(pointer).id;
                const auto bindingGeneration=BoundNativePoseGeneration(app);if(bindingGeneration==UINT64_MAX)return MMS_E_NOT_READY;
                const auto frame=app->CurrentFrame();std::uint64_t revision=0;
                if(auto error=history->Revision(revision))return error;
                const auto owner=app->MainWindow();const int language=app->EnglishUI();
                const wchar_t* text=language==1?L"This motion is for a different model. Only matching names will be imported. Continue?":
                    language==2?L"此动作属于其他模型。只会导入名称匹配的轨道。是否继续？":L"別のモデル用のモーションです。同じ名前のトラックのみ読み込みます。続行しますか？";
                if(!owner||MessageBoxW(owner,text,L"MMS VMD",MB_OKCANCEL|MB_ICONWARNING)!=IDOK)return MMS_OK;
                std::uint64_t currentRevision=0;
                if(BoundNativePoseGeneration(app)!=bindingGeneration||BoundNativePoseHistory(app)!=history||
                    !ImportReady(*app,*history)||history->Revision(currentRevision)!=MMS_OK||currentRevision!=revision||
                    app->CameraMode()||app->SelectedModelSlot()!=slot||app->CurrentFrame()!=frame||app->ModelSlot(slot)!=pointer||
                    !SceneObjectPresent(pointer,identity))return MMS_E_STALE;
            }
            ModelPasteReport report;
            if(auto error=PlanModelVmdImport(*app,slot,source,change,&report))return error;
            boneCurvesSkipped=report.boneCurvesSkipped;
        }else if(auto error=PlanVmd(*app,ansiPath,widePath,change,&matched))return error;
        if(!change)return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        if(auto error=history->PrepareNative("Import VMD motion",std::move(change),token,SharedHistory::NativeIntent::ExplicitCommit))return error;
        if(auto error=history->CommitNative(std::move(token)))return error;
        committed=true;
        if(modelMode){
            // The old loader returns SeekModelFrame's last loop counter.
            // Preserve it without evaluating the just-published scene again.
            const auto& model=*mdl::Mdl(app->SelectedModel());result=int(model.morphCount);
            for(unsigned index=0;index<model.boneCount;++index){const auto type=model.boneTable[index].type;
                if(static_cast<std::int8_t>(type)<=static_cast<std::int8_t>(mdl::BoneType::Effector)||type==mdl::BoneType::FixedAxis)result=int(index)+1;}
        }
        bake::ScopedUiInputGate inputGate;
        PublishBoundPoseEdit(*app,*history,true);
        if(modelMode){
            RefreshBoundMorphReadout(*app);
            curve_host::NotifyModelRegistered(app,false);curve_host::NotifyModelRegistered(app,true);
        }
        if(!modelMode)for(auto kind:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow})curve_host::NotifyRegistered(app,kind);
        for(unsigned slot=0;slot<matched.size();++slot)if(matched[slot])curve_host::NotifyRegistered(app,curve_host::Kind::Accessory,int(slot));
        if(std::any_of(matched.begin(),matched.end(),[](bool value){return value;}))SyncAccessoryEditPanel(app);
        SelectionReeval(app);if(app->MainWindow()&&app->Renderer())PanelPaint(app);
        return MMS_OK;
    });
    if(status==MMS_OK&&boneCurvesSkipped){
        runtime_log::Write("controlled VMD: bone curve extension skipped; standard keys preserved");
        if(const auto owner=app->MainWindow()){
            const int language=app->EnglishUI();
            const wchar_t* text=language==1?L"Free curves require compatible bone tracks. Only the standard bone keys were imported.":
                language==2?L"骨骼自由曲线与目标轨道不兼容，本次只读取了标准骨骼关键帧。":
                L"自由カーブが対象のボーントラックと一致しません。標準ボーンキーのみ読み込みました。";
            MessageBoxW(owner,text,L"MMS VMD",MB_OK|MB_ICONWARNING);
        }
    }
    if(status!=MMS_OK&&committed){
        runtime_log::Write("controlled VMD import committed; UI refresh incomplete");
        if(app->MainWindow())InvalidateRect(app->MainWindow(),nullptr,FALSE);
        return true;
    }
    if(status!=MMS_OK){
        runtime_log::Write("controlled VMD import failed before publication");
        if(const auto owner=app->MainWindow()){
            const int language=app->EnglishUI();
            const wchar_t* text=status==MMS_E_NOT_READY
                ? (language==1?L"This VMD data is not supported by shared-history import yet. No scene data was changed.":language==2?
                    L"共同历史导入暂不支持此 VMD 数据。场景数据未修改。":L"この VMD データは共通履歴の読み込みに未対応です。シーンは変更されていません。")
                : status==MMS_E_NOT_FOUND
                ? (language==1?L"Cannot open VMD file. No scene data was changed.":language==2?L"无法打开 VMD 文件。场景数据未修改。":L"VMD ファイルを開けません。シーンは変更されていません。")
                : status==MMS_E_MEMORY
                ? (language==1?L"Not enough memory or history budget to import VMD. No scene data was changed.":language==2?
                    L"内存或历史预算不足，无法导入 VMD。场景数据未修改。":L"VMD の読み込みに必要なメモリまたは履歴容量が不足しています。シーンは変更されていません。")
                : (language==1?L"The VMD file or target contains invalid data. No scene data was changed.":language==2?
                    L"VMD 文件或目标数据无效。场景数据未修改。":L"VMD ファイルまたは対象データが不正です。シーンは変更されていません。");
            MessageBoxW(owner,text,L"MMS VMD",MB_OK|MB_ICONWARNING);
        }
    }
    return true;
}
}
#endif
