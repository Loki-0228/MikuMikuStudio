#include "native_vpd_import.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/vpd_scale_io.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <commdlg.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <share.h>
#include <string>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
bool Ready(MMDApp& app) noexcept {
    return !app.CameraMode() && !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.SceneMutationInProgress() && !bake::UiLocked() && !SceneSaveInProgress() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() && !app.GravitySettingDialog() &&
        !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
struct BoneSource {
    std::array<char,20> name{};
    mdl::BoneType type{};std::uint16_t flags=0;int tail=0;
    std::array<float,3> position{},axis{},translation{};std::array<float,4> rotation{};
    std::uint8_t dirty=0;bone_scale::Matrix scale;
};
BoneSource Read(unsigned char* model,unsigned i) {
    const auto& m=*mdl::Mdl(model);const auto& b=m.boneTable[i];BoneSource out;
    std::memcpy(out.name.data(),b.name,out.name.size());out.type=b.type;out.flags=b.flags;out.tail=b.tailBone;
    std::memcpy(out.position.data(),b.position,12);std::memcpy(out.axis.data(),b.axis,12);std::memcpy(out.translation.data(),b.trans,12);std::memcpy(out.rotation.data(),b.rotQuat,16);
    out.dirty=m.bonePhysicsState[i];out.scale=bone_scale::Pose(model,int(i));return out;
}
bool Same(const BoneSource& a,const BoneSource& b) {
    return a.name==b.name && a.type==b.type && a.flags==b.flags && a.tail==b.tail && a.position==b.position && a.axis==b.axis &&
        a.translation==b.translation && a.rotation==b.rotation && a.dirty==b.dirty && a.scale==b.scale;
}
bool Finite(const BoneSource& b) {
    for(const auto* values:{&b.position,&b.axis,&b.translation})for(float v:*values)if(!std::isfinite(v))return false;
    for(float v:b.rotation)if(!std::isfinite(v))return false;return bone_scale::Valid(b.scale);
}
struct Target {std::array<float,3> translation{};std::array<float,4> rotation{};bone_scale::Matrix scale;bool touched=false;};
bool Line(FILE* file,char (&line)[256]) {
    return std::fgets(line,sizeof(line),file) && (std::strchr(line,'\n') || std::feof(file));
}
void Correct(const std::vector<BoneSource>& bones,unsigned index,bool pmx,float (&quat)[4]) {
    const auto& b=bones[index];
    if(b.type!=mdl::BoneType::FixedAxis && !(b.type==mdl::BoneType::UnderIk && (b.flags&mdl::kBoneFlagFixedAxis)==mdl::kBoneFlagFixedAxis))return;
    float axis[3]{};const bool tail=b.tail>=0 && unsigned(b.tail)<bones.size();auto& d=d3dx::Get();
    if(pmx)std::memcpy(axis,b.axis.data(),12);
    else if(tail){for(int c=0;c<3;++c)axis[c]=bones[unsigned(b.tail)].position[c]-b.position[c];if(d.module)d.vec3Normalize(axis,axis);}
    if((pmx||tail) && d.quatToAxisAngle){
        float out[3],angle;d.quatToAxisAngle(quat,out,&angle);if(!std::isfinite(quat[3]))quat[3]=1.f;
        float half;const float ww=quat[3]*quat[3];if(ww<=1.f)half=sqrtf(1.f-ww);else{quat[3]=1.f;half=0.f;}
        const float dot=axis[0]*out[0]+axis[1]*out[1]+axis[2]*out[2];
        const float a=sqrtf(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
        const float c=sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
        if(dot/(a*c)<0.f)half=-half;quat[0]=half*out[0];quat[1]=half*out[1];quat[2]=half*out[2];if(d.quatNormalize)d.quatNormalize(quat,quat);
    }
}
MmsResult Parse(const wchar_t* path,const std::vector<BoneSource>& bones,bool pmx,std::size_t maximum,std::vector<Target>& targets) {
    if(!path||!*path)return MMS_E_ARGUMENT;
    std::unique_ptr<FILE,decltype(&std::fclose)> file(_wfsopen(path,L"rt",_SH_DENYWR),&std::fclose);
    if(!file)return MMS_E_NOT_FOUND;
    char line[256]{};
    for(int i=0;i<3;++i)if(!Line(file.get(),line))return MMS_E_ARGUMENT;
    if(!Line(file.get(),line))return MMS_E_ARGUMENT;
    errno=0;char* end=nullptr;const long count=std::strtol(line,&end,10);
    if(end==line||errno==ERANGE||*end!=';'||count<0||std::uint64_t(count)>maximum||!Line(file.get(),line))return MMS_E_ARGUMENT;
    targets.resize(bones.size());for(unsigned i=0;i<bones.size();++i){targets[i].translation=bones[i].translation;targets[i].rotation=bones[i].rotation;targets[i].scale=bones[i].scale;}
    for(long n=0;n<count;++n){
        if(!Line(file.get(),line))return MMS_E_ARGUMENT;const auto* brace=std::strchr(line,'{');if(!brace)return MMS_E_ARGUMENT;
        std::string name(brace+1);const auto eol=name.find_first_of("\r\n");if(eol!=std::string::npos)name.resize(eol);
        float trans[3]{},quat[4]{};int used=0;
        if(!Line(file.get(),line)||std::sscanf(line," %f , %f , %f ;%n",&trans[0],&trans[1],&trans[2],&used)!=3||!used)return MMS_E_ARGUMENT;
        used=0;if(!Line(file.get(),line)||std::sscanf(line," %f , %f , %f , %f ;%n",&quat[0],&quat[1],&quat[2],&quat[3],&used)!=4||!used)return MMS_E_ARGUMENT;
        for(float v:trans)if(!std::isfinite(v))return MMS_E_ARGUMENT;for(float v:quat)if(!std::isfinite(v))return MMS_E_ARGUMENT;
        if(!Line(file.get(),line)||!std::strchr(line,'}')||!Line(file.get(),line))return MMS_E_ARGUMENT;
        for(unsigned i=0;i<bones.size();++i){
            const auto length=strnlen_s(bones[i].name.data(),bones[i].name.size());
            if(name.size()!=length||std::memcmp(name.data(),bones[i].name.data(),length))continue;
            // The legacy loop keeps the corrected quaternion for later duplicate names.
            Correct(bones,i,pmx,quat);for(float v:quat)if(!std::isfinite(v))return MMS_E_ARGUMENT;
            auto& target=targets[i];std::memcpy(target.translation.data(),trans,12);std::memcpy(target.rotation.data(),quat,16);target.scale={};target.touched=true;
        }
    }
    // Keep the text handle open (deny writes) while the existing codec reads the tail.
    vpd_scale_io::Extension extension;const auto status=vpd_scale_io::LoadTail(path,count,extension,true);
    if(status==vpd_scale_io::TailLoad::ReadError)return MMS_E_FAILED;
    if(status==vpd_scale_io::TailLoad::Loaded){
        std::vector<std::array<char,20>> names;std::vector<std::uint8_t> touched;
        for(unsigned i=0;i<bones.size();++i){names.push_back(bones[i].name);touched.push_back(targets[i].touched?1:0);}
        const auto mapping=vpd_scale_io::Resolve(extension.entries,names,touched);
        for(unsigned i=0;i<mapping.size();++i)if(mapping[i]>=0)targets[unsigned(mapping[i])].scale=extension.entries[i].scale;
    }else if(status!=vpd_scale_io::TailLoad::Absent)runtime_log::Write("controlled VPD scale: %s",vpd_scale_io::Describe(status));
    return MMS_OK;
}
bool Remember(MMDApp& app) {const auto state=GetMenuState(GetMenu(app.MainWindow()),0x12D,MF_BYCOMMAND);return state!=UINT(-1)&&(state&MF_CHECKED);}
std::wstring Parent(const wchar_t* path) {return std::filesystem::absolute(path).parent_path().lexically_normal().native();}
thread_local bool pickerActive=false;
}
struct VpdImportSession::Impl {
    MMDApp& app;SharedHistory& history;unsigned slot;unsigned char* model;MmsObjectRef identity{};
    mdl::BoneRecord* table;std::uint8_t* dirty;std::uint8_t* selection;unsigned count;unsigned physics;std::uint64_t pool,revision=0;int frame;
    std::vector<BoneSource> bones;
    Impl(MMDApp& a,SharedHistory& h) : app(a),history(h),slot(a.SelectedModelSlot()),model(a.ModelSlot(slot)),identity(SceneObjectIdentity(model)),
        table(mdl::Mdl(model)->boneTable),dirty(mdl::Mdl(model)->bonePhysicsState),selection(mdl::Mdl(model)->boneSelection),count(mdl::Mdl(model)->boneCount),
        physics(mdl::Mdl(model)->physicsMode),pool(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),frame(a.CurrentFrame()) {}
    bool Current() const {
        if(BoundNativePoseHistory(&app)!=&history||!Ready(app)||app.SelectedModelSlot()!=slot||app.ModelSlot(slot)!=model||app.CurrentFrame()!=frame||
            !SceneObjectPresent(model,identity.id)||SceneObjectIdentity(model).generation!=identity.generation)return false;
        const auto& m=*mdl::Mdl(model);if(m.boneTable!=table||m.boneCount!=count||m.bonePhysicsState!=dirty||m.boneSelection!=selection||m.physicsMode!=physics||mdl::BoneKeyPoolIdentity(m.boneKeys)!=pool)return false;
        std::uint64_t live=0;if(history.Revision(live)!=MMS_OK||live!=revision)return false;
        for(unsigned i=0;i<count;++i)if(!Same(bones[i],Read(model,i)))return false;return true;
    }
};
VpdImportSession::VpdImportSession()=default;VpdImportSession::~VpdImportSession()=default;
MmsResult VpdImportSession::Begin(MMDApp& app,SharedHistory& history) noexcept {
    if(impl_)return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if(const auto result=history.CheckNativeEdit())return result;
        if(BoundNativePoseHistory(&app)!=&history||!Ready(app)||app.SelectedModelSlot()>=kModelSlotCount||!app.SelectedModel())return MMS_E_NOT_READY;
        const auto& m=*mdl::Mdl(app.SelectedModel());
        if(!m.boneTable||!m.boneCount||!m.bonePhysicsState||!m.boneSelection||!mdl::BoneKeyPoolIdentity(m.boneKeys))return MMS_E_NOT_READY;
        auto next=std::make_unique<Impl>(app,history);if(const auto result=history.Revision(next->revision))return result;
        next->bones.reserve(next->count);for(unsigned i=0;i<next->count;++i){auto bone=Read(next->model,i);if(!Finite(bone))return MMS_E_ARGUMENT;next->bones.push_back(std::move(bone));}
        impl_=std::move(next);return MMS_OK;
    });
}
MmsResult VpdImportSession::Apply(const wchar_t* path,bool& changed,bool resetPhysics) noexcept {
    changed=false;if(!impl_)return MMS_E_NOT_READY;auto source=std::move(impl_);
    return Boundary([&]() -> MmsResult {
        if(const auto result=source->history.CheckNativeEdit())return result;if(!source->Current())return MMS_E_STALE;
        std::vector<Target> targets;if(const auto result=Parse(path,source->bones,source->physics==2,mdl::BoneKeyCapacity(*mdl::Mdl(source->model)),targets))return result;
        if(!source->Current())return MMS_E_STALE;bool different=false;
        for(unsigned i=0;i<targets.size();++i)if(targets[i].touched)different|=targets[i].translation!=source->bones[i].translation||targets[i].rotation!=source->bones[i].rotation||targets[i].scale!=source->bones[i].scale;
        if(!different)return MMS_OK;
        NativePoseScope scope;if(const auto result=scope.Begin(source->app,source->history,source->slot,"Import VPD pose",SharedHistory::NativeIntent::ExplicitCommit))return result;
        for(unsigned i=0;i<targets.size();++i)if(targets[i].touched){
            const auto& t=targets[i];std::memcpy(source->table[i].trans,t.translation.data(),12);std::memcpy(source->table[i].rotQuat,t.rotation.data(),16);
            if(!bone_scale::SetPose(source->model,int(i),t.scale)){if(!scope.Cancel())return MMS_E_FAILED;return MMS_E_MEMORY;}source->dirty[i]=1;
        }
        if(const auto result=scope.Commit(changed))return result;
        if(changed){for(unsigned i=0;i<targets.size();++i)if(targets[i].touched)source->selection[i]=1;
            Boundary([&]() -> MmsResult {PublishBoundPoseEdit(source->app,source->history,resetPhysics);return MMS_OK;});}
        return MMS_OK;
    });
}
MmsResult ImportBoundVpd(MMDApp& app,const wchar_t* path,bool& changed,bool resetPhysics) noexcept {
    changed=false;auto* history=BoundNativePoseHistory(&app);if(!history)return MMS_E_NOT_READY;
    VpdImportSession session;if(const auto result=session.Begin(app,*history))return result;return session.Apply(path,changed,resetPhysics);
}
bool TryLoadBoundVpd(MMDApp* app,const wchar_t* path,bool rememberDirectory) noexcept {
    if(!app||!BoundNativePoseHistory(app))return false;
    Boundary([&]() -> MmsResult {if(!path||!*path)return MMS_E_ARGUMENT;const auto directory=rememberDirectory?Parent(path):std::wstring{};if(directory.size()>=kPathFieldCapacity)return MMS_E_ARGUMENT;
        bool changed=false;const auto result=ImportBoundVpd(*app,path,changed);if(result==MMS_OK&&changed&&rememberDirectory)wcscpy_s(app->DirPose(),kPathFieldCapacity,directory.c_str());return result;});
    return true;
}
bool TryPickBoundVpd(MMDApp* app) noexcept {
    auto* history=app?BoundNativePoseHistory(app):nullptr;if(!history)return false;if(pickerActive)return true;
    struct Active {Active(){pickerActive=true;}~Active(){pickerActive=false;}} active;
    Boundary([&]() -> MmsResult {
        VpdImportSession session;if(const auto result=session.Begin(*app,*history))return result;
        const auto base=std::filesystem::path(app->ExeDir());const bool remember=Remember(*app);
        auto initial=remember?std::filesystem::path(app->DirPose()):std::filesystem::path(L"UserFile/Pose");if(initial.is_relative())initial=base/initial;const auto folder=std::filesystem::absolute(initial).lexically_normal().native();
        wchar_t original[32768]{};if(!GetCurrentDirectoryW(32768,original))return MMS_E_FAILED;
        struct Directory {const wchar_t* path;bool restored=false;~Directory(){if(!restored)SetCurrentDirectoryW(path);}bool Restore(){restored=SetCurrentDirectoryW(path)!=FALSE;return restored;}} directory{original};
        wchar_t file[kPathFieldCapacity]{};OPENFILENAMEW picker{};picker.lStructSize=sizeof(picker);picker.hwndOwner=app->FloatingWindow()?app->FloatingWindow():app->MainWindow();
        picker.lpstrFilter=L"Vocaloid Pose Data files(*.vpd)\0*.vpd\0";picker.lpstrFile=file;picker.nMaxFile=kPathFieldCapacity;picker.lpstrInitialDir=folder.c_str();picker.lpstrDefExt=L"vpd";
        picker.lpstrTitle=app->EnglishUI()?L"load pose data":L"\u30dd\u30fc\u30ba\u30c7\u30fc\u30bf\u8aad\u8fbc";picker.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
        const bool selected=GetUiOpenFileNameW(&picker)!=FALSE;if(!directory.Restore())return MMS_E_FAILED;if(!selected)return MMS_OK;
        const auto parent=remember?Parent(file):std::wstring{};if(parent.size()>=kPathFieldCapacity)return MMS_E_ARGUMENT;
        bool changed=false;const auto result=session.Apply(file,changed,true);if(result==MMS_OK&&changed&&remember)wcscpy_s(app->DirPose(),kPathFieldCapacity,parent.c_str());return result;
    });return true;
}
}
#endif
