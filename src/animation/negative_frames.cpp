#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/frame_scope.hpp"
#include "mikumikustudio/clipboard_layout.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <set>
#include <tuple>
#include <type_traits>
#include <limits>
#include <unordered_map>

namespace mikumikustudio {
void RefreshAfterFrameApply(MMDApp*);
void RegisterCameraState(MMDApp*,int);void RegisterLightState(MMDApp*,int);
void RegisterSelfShadowState(MMDApp*,int);void RegisterGravityKeyCurrent(MMDApp*,std::int32_t);
void RegisterAccessoryKey(MMDApp*,int,int);
}

namespace mikumikustudio::negative_frames {
namespace {
using Values=std::array<double,16>;
struct Key {
    unsigned char* model=nullptr;
    const void* identity=nullptr;
    int target=0,frame=0;
    bool morph=false,selected=false;
    Values values{};
    std::int64_t clipboardOffset=0;
};
struct State {
    std::vector<Key> keys,clipboard;
    struct Snapshot {std::vector<Key> keys;const unsigned char* model=nullptr;int nativeCursor=0;bool drag=false;};
    std::vector<Snapshot> undo,redo;
    std::vector<std::pair<std::size_t,int>> dragging;
    std::int64_t dragMin=0,dragMax=0;
    int dragOffset=0;
    bool dragHistory=false;
};
std::unordered_map<const MMDApp*,State> states;
bool Current(const MMDApp& app,const Key& key) {
    if(!key.model) {
        if(key.target>=1000 && key.target<1255)return app.AccessorySlot(key.target-1000)==key.identity && key.identity;
        return key.identity==&app && key.target>=2 && key.target<=5;
    }
    for(int slot=0;slot<255;++slot)if(app.ModelSlot(slot)==key.model) {
        const auto* record=mdl::Mdl(key.model);
        if(record && key.identity==record->boneTable && key.target<0) {
            if(key.target==-1)return true;
            if(key.target<=-1000000)return std::int64_t(-1000000)-key.target<record->boneOrderCount;
            return std::int64_t(-2)-key.target<record->ikChainCount;
        }
        return record && key.identity==(key.morph ? static_cast<const void*>(record->morphs) : record->boneTable) &&
            key.target>=0 && key.target<int(key.morph ? record->morphCount : record->boneCount);
    }
    return false;
}
MMDApp* Owner(unsigned char* model) {
    if(!g_Block || !model)return nullptr;
    for(int slot=0;slot<255;++slot)if(g_Block->ModelSlot(slot)==model)return g_Block;
    return nullptr;
}
State::Snapshot Snapshot(const State& state) {
    const auto* model=g_Block && !g_Block->CameraMode() ? g_Block->SelectedModel() : nullptr;
    return {state.keys,model,model ? int(mdl::Mdl(model)->undoState[0]) : 0};
}
void History(State& state) {
    state.undo.push_back(Snapshot(state));
    if(state.undo.size()>30)state.undo.erase(state.undo.begin());
    state.redo.clear();
}
Values Pose(unsigned char* model,int target,bool morph) {
    Values values{};auto& record=*mdl::Mdl(model);
    if(target<0) {
        if(target==-1)values[0]=record.loadComplete;
        else if(target<=-1000000) {
            const auto& reference=mdl::BoneOrder(model)[-1000000-target];
            values[0]=double(reference.linkedModel);values[1]=double(reference.linkedBone);
        } else values[0]=mdl::IkChains(model)[-2-target].enabled;
        return values;
    }
    if(morph)values[0]=record.morphs[target].value;
    else {
        const auto& bone=record.boneTable[target];
        std::copy_n(bone.trans,3,values.begin());
        std::copy_n(bone.rotQuat,4,values.begin()+3);
        const auto scale=bone_scale::Pose(model,target);
        std::copy(scale.v.begin(),scale.v.end(),values.begin()+7);
    }
    return values;
}
void Apply(unsigned char* model,int target,bool morph,const Values& values) {
    auto& record=*mdl::Mdl(model);
    if(target<0) {
        if(target==-1)record.loadComplete=values[0]!=0;
        else if(target<=-1000000) {
            auto& reference=mdl::BoneOrder(model)[-1000000-target];
            reference.linkedModel=int(values[0]);reference.linkedBone=int(values[1]);
        } else mdl::IkChains(model)[-2-target].enabled=values[0]!=0;
        return;
    }
    if(morph)record.morphs[target].value=values[0];
    else {
        auto& bone=record.boneTable[target];
        std::copy_n(values.begin(),3,bone.trans);
        std::copy_n(values.begin()+3,4,bone.rotQuat);
        bone_scale::Matrix scale;std::copy_n(values.begin()+7,9,scale.v.begin());
        bone_scale::SetPose(model,target,scale);
    }
}
bool Valid(const Values& values,bool morph) {
    for(double value:values)if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return false;
    if(morph)return true;
    double norm=0;for(int i=3;i<7;++i)norm+=double(values[i])*values[i];
    bone_scale::Matrix scale;std::copy_n(values.begin()+7,9,scale.v.begin());
    return norm>1e-20 && bone_scale::Valid(scale);
}
bool Integer(double value,double first,double last) {
    return std::isfinite(value) && value>=first && value<=last && std::trunc(value)==value;
}
bool GlobalValid(const Values& values,int kind) {
    for(double value:values)if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return false;
    if(kind==2)return Integer(values[8],0,1) && Integer(values[9],-1,254) && Integer(values[10],-1,frame_limits::maximum) && values[7]>=0 && values[7]<=180;
    if(kind==3)return true;
    if(kind==4)return Integer(values[0],0,2);
    if(kind==5)return Integer(values[4],std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()) && Integer(values[5],0,1);
    return kind>=1000 && kind<1255 && Integer(values[8],0,1) && Integer(values[9],0,1) &&
        Integer(values[10],-1,254) && Integer(values[11],-1,frame_limits::maximum);
}
void Upsert(State& state,const Key& key) {
    auto found=std::find_if(state.keys.begin(),state.keys.end(),[&](const Key& existing) {
        return existing.model==key.model && existing.identity==key.identity && existing.morph==key.morph &&
            existing.target==key.target && existing.frame==key.frame;
    });
    if(found==state.keys.end())state.keys.push_back(key);else *found=key;
}
int Row(const MMDApp& app,const Key& key) {
    if(!key.model) {
        if(key.target>=1000) {
            for(int band=0;band<200 && app.state.jointLineMap[band]>=0;++band)
                if(app.state.jointLineMap[band]==key.target-1000)return 4+band;
            return -1;
        }
        return key.target-2;
    }
    if(key.target<0)return 0;
    const auto& record=*mdl::Mdl(key.model);
    const auto* rows=key.morph ? record.morphKeyIndices : record.boneKeyIndices;
    if(!rows)return 0;
    return std::abs(reinterpret_cast<const std::int32_t*>(rows)[key.target]);
}
Values GlobalPose(MMDApp* app,int kind) {
    Values values{};
    if(kind>=1000) {
        const auto* object=app->AccessorySlot(kind-1000);if(!object)return values;
        for(int i=0;i<3;++i){values[i]=object->position[i];values[3+i]=object->rotation[i];}
        values[6]=object->scale;values[7]=object->opacity;values[8]=object->visible;values[9]=object->shadowEnabled;
        values[10]=double(object->parentModel);values[11]=double(object->parentBone);
    } else if(kind==2) {
        for(int i=0;i<3;++i){values[i]=app->CameraPositionAxis(i);values[3+i]=app->CameraRotation()[i];}
        values[6]=app->CameraDistance();values[7]=app->CameraFov();values[8]=app->CameraPerspective();
        values[9]=app->CameraParentModel();values[10]=app->CameraParentBone();
    } else if(kind==3)for(int i=0;i<3;++i){values[i]=app->LightDirection()[i];values[3+i]=app->LightColor()[i];}
    else if(kind==4){values[0]=float(app->SelfShadowMode());values[1]=app->ShadowDistance();}
    else {values[0]=app->GravityMagnitude();for(int i=0;i<3;++i)values[1+i]=app->GravityDirection()[i];
        values[4]=app->GravityNoise();values[5]=float(app->GravityNoiseEnabled());}
    return values;
}
void ApplyGlobal(MMDApp* app,int kind,const Values& values) {
    if(kind>=1000) {
        auto* object=app->AccessorySlot(kind-1000);if(!object)return;
        for(int i=0;i<3;++i){object->position[i]=values[i];object->rotation[i]=values[3+i];}
        object->scale=values[6];object->opacity=values[7];object->visible=values[8]!=0;object->shadowEnabled=values[9]!=0;
        object->parentModel=int(values[10]);object->parentBone=int(values[11]);
    } else if(kind==2) {
        for(int i=0;i<3;++i){app->CameraPositionAxis(i)=values[i];app->CameraRotation()[i]=values[3+i];}
        app->CameraDistance()=values[6];app->CameraFov()=float(values[7]);app->CameraPerspective()=values[8]!=0;
        app->CameraParentModel()=int(values[9]);app->CameraParentBone()=int(values[10]);
    } else if(kind==3)for(int i=0;i<3;++i){app->LightDirection()[i]=values[i];app->LightColor()[i]=values[3+i];}
    else if(kind==4){app->SelfShadowMode()=int(values[0]);app->ShadowDistance()=values[1];}
    else {app->GravityMagnitude()=values[0];for(int i=0;i<3;++i)app->GravityDirection()[i]=values[1+i];
        app->GravityNoise()=int(values[4]);app->GravityNoiseEnabled()=values[5]!=0;}
}
Values GlobalZero(MMDApp* app,int kind) {
    auto values=GlobalPose(app,kind);
    if(kind>=1000 && app->AccessoryKeys(kind-1000)) {
        const auto& key=app->AccessoryKeys(kind-1000)[0];
        for(int i=0;i<3;++i){values[i]=key.position[i];values[3+i]=key.rotation[i];}
        values[6]=key.scale;values[7]=key.opacity;values[8]=key.visible;values[9]=key.shadowEnabled;
        values[10]=double(key.parentModel);values[11]=double(key.parentBone);
    } else if(kind==2 && app->CameraKeys()) {
        const auto& key=app->CameraKeys()[0];
        for(int i=0;i<3;++i){values[i]=key.eye[i];values[3+i]=key.target[i];}
        values[6]=key.distance;values[7]=float(key.fov);values[8]=key.perspective;
        values[9]=double(key.parentModel);values[10]=double(key.parentBone);
    } else if(kind==3 && app->LightKeys()) {
        const auto& key=app->LightKeys()[0];for(int i=0;i<3;++i){values[i]=key.direction[i];values[3+i]=key.color[i];}
    } else if(kind==4 && app->ShadowKeys()){values[0]=app->ShadowKeys()[0].mode;values[1]=app->ShadowKeys()[0].distance;}
    else if(kind==5 && app->GravityKeys()) {
        const auto& key=app->GravityKeys()[0];values[0]=key.acceleration;
        for(int i=0;i<3;++i)values[1+i]=key.direction[i];values[4]=double(key.noise);values[5]=key.noiseEnabled;
    }
    return values;
}
void Changed(MMDApp* app) {app->SceneModified()=1;if(app->MainWindow()){PanelPaint(app);PostViewRefresh(app);RefreshControls(app);}}
void U32(std::vector<std::uint8_t>& bytes,std::uint32_t value) {
    for(int i=0;i<4;++i)bytes.push_back(std::uint8_t(value>>(8*i)));
}
bool RegisterGlobalImpl(MMDApp* app,Global kind,int frame) {
    if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto values=GlobalPose(app,int(kind));if(!GlobalValid(values,int(kind)))return false;
    auto& state=states[app];History(state);
    Upsert(state,{nullptr,app,int(kind),frame,false,true,values});app->SceneModified()=1;return true;
}
void EvaluateGlobalsImpl(MMDApp* app,int frame) {
    auto found=states.find(app);if(found==states.end() || frame>=0)return;
    std::vector<int> kinds;
    for(const auto& key:found->second.keys)if(!key.model && Current(*app,key) && std::find(kinds.begin(),kinds.end(),key.target)==kinds.end())kinds.push_back(key.target);
    for(int kind:kinds) {
        std::vector<const Key*> keys;
        for(const auto& key:found->second.keys)if(!key.model && key.target==kind)keys.push_back(&key);
        if(keys.empty())continue;
        std::sort(keys.begin(),keys.end(),[](const Key* a,const Key* b){return a->frame<b->frame;});
        auto upper=std::upper_bound(keys.begin(),keys.end(),frame,[](int f,const Key* key){return f<key->frame;});
        const Key* previous=upper==keys.begin()?keys.front():*(upper-1);auto result=previous->values;
        if(frame>previous->frame) {
            const auto end=upper==keys.end()?GlobalZero(app,kind):(*upper)->values;
            const int endFrame=upper==keys.end()?0:(*upper)->frame;
            const double t=double(std::int64_t(frame)-previous->frame)/(std::int64_t(endFrame)-previous->frame);
            const int continuous=kind>=1000?8:kind==2?8:kind==3?6:kind==4?0:4;
            for(int i=0;i<continuous;++i)result[i]=float(result[i]+t*(double(end[i])-result[i]));
            if(kind==4)result[1]=float(result[1]+t*(double(end[1])-result[1]));
        }
        ApplyGlobal(app,kind,result);
    }
}
std::uint32_t Read32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8)|(std::uint32_t(bytes[2])<<16)|(std::uint32_t(bytes[3])<<24);
}
std::vector<Key> NativeClipboard(MMDApp* app) {
    std::vector<Key> out;const auto& counts=app->ClipboardCounts();auto* model=app->SelectedModel();
    const auto global=[&](int kind,unsigned frame,const Values& values) {
        if(frame>unsigned(frame_limits::maximum))return;
        const void* identity=kind>=1000 ? static_cast<const void*>(app->AccessorySlot(kind-1000)) : app;
        if(identity)out.push_back({nullptr,identity,kind,int(frame),false,true,values});
    };
    if(model && !app->CameraMode()) {
        const auto& record=*mdl::Mdl(model);
        for(unsigned i=0;app->BoneClipboard() && i<counts.bones;++i) {
            const auto& source=app->BoneClipboard()[i];if(source.frame>unsigned(frame_limits::maximum))continue;
            int target=frame_clipboard::BoneTarget(app,model,i);
            if(target<0)for(unsigned candidate=0;candidate<record.boneCount;++candidate)
                if(!std::strncmp(record.boneTable[candidate].name,source.name,20)){target=int(candidate);break;}
            if(target>=0) {
                Values values{};std::copy_n(source.position,3,values.begin());std::copy_n(source.rotation,4,values.begin()+3);
                const auto scale=bone_scale::Clipboard(app,int(i),false);std::copy(scale.v.begin(),scale.v.end(),values.begin()+7);
                if(Valid(values,false))out.push_back({model,record.boneTable,target,int(source.frame),false,true,values});
            }
        }
        for(unsigned i=0;app->MorphClipboard() && i<counts.morphs;++i) {
            const auto& source=app->MorphClipboard()[i];if(source.frame>unsigned(frame_limits::maximum))continue;
            int target=frame_clipboard::MorphTarget(app,model,i);
            if(target<0)for(unsigned candidate=0;candidate<record.morphCount;++candidate)
                if(!std::strncmp(record.morphs[candidate].name,source.name,20)){target=int(candidate);break;}
            if(target>=0) {
                Values values{};values[0]=source.value;
                if(Valid(values,true))out.push_back({model,record.morphs,target,int(source.frame),true,true,values});
            }
        }
        for(unsigned i=0;app->DisplayClipboard() && i<counts.displays;++i) {
            const auto& source=app->DisplayClipboard()[i];if(source.frame>unsigned(frame_limits::maximum))continue;
            const auto add=[&](int target,Values values){out.push_back({model,record.boneTable,target,int(source.frame),false,true,values});};
            Values values{};values[0]=source.visible;add(-1,values);
            for(int j=0;source.ikStates && j<source.ikCount;++j)for(unsigned k=0;k<record.ikChainCount;++k) {
                const int bone=mdl::IkChains(model)[k].boneIndex;
                if(bone>=0 && bone<int(record.boneCount) && !std::strncmp(record.boneTable[bone].name,source.ikStates[j].boneName,20)) {
                    values={};values[0]=source.ikStates[j].enabled;add(-2-int(k),values);
                }
            }
        }
    }
    if(app->CameraMode()) {
        for(unsigned i=0;app->CameraClipboard() && i<counts.cameras;++i) {
            const auto& source=app->CameraClipboard()[i];Values values{};
            for(int j=0;j<3;++j){values[j]=source.eye[j];values[3+j]=source.target[j];}
            values[6]=source.distance;values[7]=float(source.fov);values[8]=source.perspective;
            values[9]=double(source.parentModel);values[10]=double(source.parentBone);global(2,source.frame,values);
        }
        for(unsigned i=0;app->LightClipboard() && i<counts.lights;++i) {
            const auto& source=app->LightClipboard()[i];Values values{};
            for(int j=0;j<3;++j){values[j]=source.direction[j];values[3+j]=source.color[j];}global(3,source.frame,values);
        }
        for(unsigned i=0;app->ShadowClipboard() && i<counts.shadows;++i) {
            const auto& source=app->ShadowClipboard()[i];Values values{};values[0]=source.mode;values[1]=source.distance;global(4,source.frame,values);
        }
        for(unsigned i=0;app->GravityClipboard() && i<counts.gravity;++i) {
            const auto& source=app->GravityClipboard()[i];Values values{};values[0]=source.acceleration;
            for(int j=0;j<3;++j)values[1+j]=source.direction[j];values[4]=double(source.noise);values[5]=source.noiseEnabled;global(5,source.frame,values);
        }
        for(unsigned i=0;app->AccessoryClipboard() && i<counts.accessories;++i) {
            const auto& source=app->AccessoryClipboard()[i];if(source.slot>=255)continue;Values values{};
            for(int j=0;j<3;++j){values[j]=source.position[j];values[3+j]=source.rotation[j];}
            values[6]=source.scale;values[7]=source.opacity;values[8]=source.visible;values[9]=source.shadowEnabled;
            values[10]=double(source.parentModel);values[11]=double(source.parentBone);global(1000+source.slot,source.frameOffset,values);
        }
    }
    return out;
}
bool PasteTarget(MMDApp* app,Key& key) {
    if(!Current(*app,key))return false;
    if(!key.model)return app->CameraMode()!=0;
    auto* destination=app->CameraMode() ? nullptr : app->SelectedModel();
    if(!destination)return false;
    if(destination==key.model)return true;
    const auto& source=*mdl::Mdl(key.model);
    const auto& target=*mdl::Mdl(destination);
    int index=-1;
    if(key.target>=0) {
        const char* name=key.morph ? source.morphs[key.target].name : source.boneTable[key.target].name;
        const unsigned count=key.morph ? target.morphCount : target.boneCount;
        for(unsigned i=0;i<count;++i)if(!std::strncmp(name,key.morph ? target.morphs[i].name : target.boneTable[i].name,20)) {
            index=int(i);break;
        }
        if(index<0)return false;
    } else if(key.target==-1)index=-1;
    else if(key.target>-1000000) {
        const int bone=mdl::IkChains(key.model)[-2-key.target].boneIndex;
        if(bone<0 || bone>=int(source.boneCount))return false;
        for(unsigned i=0;i<target.ikChainCount;++i) {
            const int targetBone=mdl::IkChains(destination)[i].boneIndex;
            if(targetBone>=0 && targetBone<int(target.boneCount) &&
               !std::strncmp(source.boneTable[bone].name,target.boneTable[targetBone].name,20)) {
                index=-2-int(i);break;
            }
        }
        if(index==-1)return false;
    } else return false;
    key.model=destination;key.target=index;
    key.identity=key.morph ? static_cast<const void*>(target.morphs) : target.boneTable;
    return true;
}
int NativeSelectionFirst(MMDApp* app) {
    int first=frame_limits::maximum;
    const auto scan=[&](const auto* keys,std::size_t capacity,bool model) {
        for(std::size_t i=0;keys && i<capacity;++i) {
            bool marked;
            if constexpr (std::is_same_v<std::decay_t<decltype(keys[i])>,mdl::BoneKey> ||
                          std::is_same_v<std::decay_t<decltype(keys[i])>,mdl::MorphKey> ||
                          std::is_same_v<std::decay_t<decltype(keys[i])>,mdl::DisplayKey>)marked=keys[i].allocated!=0;
            else marked=keys[i].selected!=0;
            (void)model;
            if(marked)first=std::min(first,int(keys[i].frame));
        }
    };
    if(!app->CameraMode() && app->SelectedModel()) {
        const auto& record=*mdl::Mdl(app->SelectedModel());
        scan(record.boneKeys,mdl::BoneKeyCapacity(record),true);
        scan(record.morphKeys,mdl::MorphKeyCapacity(record.morphKeys),true);
        scan(record.displayKeys,mdl::KeyPoolCapacity(record.displayKeys),true);
    } else {
        scan(app->CameraKeys(),mdl::KeyPoolCapacity(app->CameraKeys()),false);
        scan(app->LightKeys(),mdl::KeyPoolCapacity(app->LightKeys()),false);
        scan(app->ShadowKeys(),mdl::KeyPoolCapacity(app->ShadowKeys()),false);
        scan(app->GravityKeys(),mdl::KeyPoolCapacity(app->GravityKeys()),false);
        for(int i=0;i<255;++i)scan(app->AccessoryKeys(i),mdl::KeyPoolCapacity(app->AccessoryKeys(i)),false);
    }
    return first;
}
}
bool RegisterGlobal(MMDApp* app,Global kind,int frame) {return RegisterGlobalImpl(app,kind,frame);}
void EvaluateGlobals(MMDApp* app,int frame) {EvaluateGlobalsImpl(app,frame);}
bool RegisterAccessory(MMDApp* app,int slot,int frame) {
    if(!app || slot<0 || slot>=255 || !app->AccessorySlot(slot) || frame>=0 || frame<frame_limits::minimum)return false;
    auto values=GlobalPose(app,1000+slot);if(!GlobalValid(values,1000+slot))return false;
    auto& state=states[app];History(state);
    Upsert(state,{nullptr,app->AccessorySlot(slot),1000+slot,frame,false,true,values});
    app->SceneModified()=1;return true;
}
bool RegisterDisplay(unsigned char* model,int frame) {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& state=states[app];History(state);auto& record=*mdl::Mdl(model);
    const auto add=[&](int target){Upsert(state,{model,record.boneTable,target,frame,false,true,Pose(model,target,false)});};
    add(-1);for(unsigned i=0;i<record.ikChainCount;++i)add(-2-int(i));
    for(unsigned i=0;i<record.boneOrderCount;++i)add(-1000000-int(i));
    app->SceneModified()=1;return true;
}
bool RegisterBone(unsigned char* model,int bone,int frame) {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& record=*mdl::Mdl(model);
    if(!record.boneTable || bone<0 || bone>=int(record.boneCount))return false;
    auto values=Pose(model,bone,false);if(!Valid(values,false))return false;
    auto& state=states[app];History(state);
    Upsert(state,{model,record.boneTable,bone,frame,false,true,values});app->SceneModified()=1;return true;
}
bool RegisterSelectedBones(unsigned char* model,int frame) {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& record=*mdl::Mdl(model);if(!record.boneTable || !record.boneSelection)return false;
    std::vector<Key> pending;
    for(int bone=0;bone<int(record.boneCount);++bone)if(record.boneSelection[bone]) {
        auto values=Pose(model,bone,false);if(!Valid(values,false))return false;
        pending.push_back({model,record.boneTable,bone,frame,false,true,values});
    }
    if(pending.empty())return true;
    auto& state=states[app];History(state);
    for(const auto& key:pending){Upsert(state,key);if(record.bonePhysicsState)record.bonePhysicsState[key.target]=0;}
    app->SceneModified()=1;return true;
}
bool RegisterMorph(unsigned char* model,int morph,int frame,float value) {
    auto* app=Owner(model);if(!app || frame>=0 || frame<frame_limits::minimum)return false;
    auto& record=*mdl::Mdl(model);
    if(!record.morphs || morph<0 || morph>=int(record.morphCount) || !std::isfinite(value))return false;
    Values values{};values[0]=value;
    auto& state=states[app];History(state);
    Upsert(state,{model,record.morphs,morph,frame,true,true,values});app->SceneModified()=1;return true;
}
void EvaluateModel(unsigned char* model,int frame) {
    auto* app=Owner(model);if(!app || frame>=0)return;
    auto found=states.find(app);if(found==states.end())return;
    std::map<std::pair<bool,int>,std::vector<const Key*>> tracks;
    for(const auto& key:found->second.keys)if(key.model==model && Current(*app,key))tracks[{key.morph,key.target}].push_back(&key);
    for(auto& pair:tracks) {
        auto& keys=pair.second;
        std::sort(keys.begin(),keys.end(),[](const Key* a,const Key* b){return a->frame<b->frame;});
        auto upper=std::upper_bound(keys.begin(),keys.end(),frame,[](int f,const Key* key){return f<key->frame;});
        const Key* previous=upper==keys.begin() ? keys.front() : *(upper-1);
        Values result=previous->values;
        if(frame>previous->frame && previous->target>=0) {
            const auto end=upper==keys.end() ? Pose(model,previous->target,previous->morph) : (*upper)->values;
            const int endFrame=upper==keys.end() ? 0 : (*upper)->frame;
            const double t=double(std::int64_t(frame)-previous->frame)/(std::int64_t(endFrame)-previous->frame);
            for(std::size_t i=0;i<result.size();++i)result[i]=float(result[i]+t*(double(end[i])-result[i]));
            if(!previous->morph) {
                double dot=0;for(int i=3;i<7;++i)dot+=double(previous->values[i])*end[i];
                double norm=0;
                for(int i=3;i<7;++i) {result[i]=float((1-t)*previous->values[i]+t*(dot<0 ? -end[i] : end[i]));norm+=double(result[i])*result[i];}
                if(norm>1e-20)for(int i=3;i<7;++i)result[i]=float(result[i]/std::sqrt(norm));
            }
        }
        Apply(model,previous->target,previous->morph,result);
    }
}
void Reset(MMDApp* app) {states.erase(app);}
void ForgetModel(unsigned char* model) {
    for(auto& pair:states) {
        const auto erase=[&](std::vector<Key>& keys){keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const Key& key){return key.model==model;}),keys.end());};
        erase(pair.second.keys);erase(pair.second.clipboard);
        for(auto& entry:pair.second.undo)erase(entry.keys);
        for(auto& entry:pair.second.redo)erase(entry.keys);
    }
}
void RefreshControls(MMDApp* app) {
    if(!app || !app->MainWindow())return;auto found=states.find(app);if(found==states.end())return;
    const auto& state=found->second;
    const bool selected=std::any_of(state.keys.begin(),state.keys.end(),[&](const Key& key){return key.selected && Current(*app,key);});
    if(selected){EnableWindow(GetDlgItem(app->MainWindow(),420),TRUE);EnableWindow(GetDlgItem(app->MainWindow(),423),TRUE);}
    if(!state.clipboard.empty())EnableWindow(GetDlgItem(app->MainWindow(),421),TRUE);
    if(app->CurrentFrame()<0 || (!state.undo.empty() && state.undo.back().drag) || (!state.redo.empty() && state.redo.back().drag)) {
        if(!state.undo.empty())EnableWindow(GetDlgItem(app->MainWindow(),400),TRUE);
        if(!state.redo.empty())EnableWindow(GetDlgItem(app->MainWindow(),401),TRUE);
    }
}
std::size_t Count(const MMDApp* app) {auto found=states.find(app);return found==states.end()?0:found->second.keys.size();}
int Earliest(const MMDApp* app) {
    auto found=states.find(app);int first=0;
    if(found!=states.end())for(const auto& key:found->second.keys)if(Current(*app,key))first=std::min(first,key.frame);
    return first;
}
void Capture(const MMDApp& app,pmm_extension::Document& document) {
    auto& mutableApp=const_cast<MMDApp&>(app);
    std::vector<std::uint8_t> cursor;
    U32(cursor,std::uint32_t(mutableApp.CurrentFrame()));U32(cursor,std::uint32_t(app.state.timelineStartFrame));
    pmm_extension::Set(document,"org.mikudancestudio.timeline.signed_cursor.v1",std::move(cursor));
    if(app.state.hwnd) {
        std::vector<std::uint8_t> bounds;
        for(int id:{panel::kPlayStartFrameEdit,panel::kPlayStopFrameEdit}) {
            char text[64]{};GetDlgItemTextA(app.state.hwnd,id,text,sizeof(text));int value=0;
            if(!frame_limits::ParsePlaybackBound(text,value))throw std::runtime_error("Invalid signed playback range");
            U32(bounds,std::uint32_t(value));
        }
        pmm_extension::Set(document,"org.mikudancestudio.timeline.signed_play_range.v1",std::move(bounds));
    }
    const auto found=states.find(&app);std::vector<std::uint8_t> bytes;U32(bytes,1);U32(bytes,0);unsigned count=0;
    if(found!=states.end())for(const auto& key:found->second.keys)if(Current(app,key)) {
        int slot=255;if(key.model){slot=0;while(slot<255 && app.ModelSlot(slot)!=key.model)++slot;}
        U32(bytes,unsigned(slot));U32(bytes,!key.model?2:key.target<0?3:key.morph?1:0);U32(bytes,unsigned(key.target));U32(bytes,std::uint32_t(key.frame));
        if(key.model && key.target>=0){const auto& record=*mdl::Mdl(key.model);
            const char* name=key.morph ? record.morphs[key.target].name : record.boneTable[key.target].name;
            bytes.insert(bytes.end(),name,name+20);}
        else bytes.insert(bytes.end(),20,0);
        for(double value:key.values){std::uint64_t bits;std::memcpy(&bits,&value,8);for(int j=0;j<8;++j)bytes.push_back(std::uint8_t(bits>>(8*j)));}++count;
    }
    if(!count){pmm_extension::Erase(document,kPmmKey);return;}
    for(int i=0;i<4;++i)bytes[4+i]=std::uint8_t(count>>(8*i));
    pmm_extension::Set(document,kPmmKey,std::move(bytes));
}
static void RestoreMetadata(MMDApp& app,const pmm_extension::Document& document) {
    if(const auto* cursor=pmm_extension::Find(document,"org.mikudancestudio.timeline.signed_cursor.v1");cursor && cursor->value.size()==8) {
        const auto frame=static_cast<std::int32_t>(Read32(cursor->value.data()));
        const auto scroll=static_cast<std::int32_t>(Read32(cursor->value.data()+4));
        if(frame>=frame_limits::minimum && frame<=frame_limits::maximum && scroll>=frame_limits::minimum && scroll<=frame_limits::maximum) {
            app.CurrentFrame()=frame;app.state.timelineStartFrame=scroll;
        }
    }
    if(const auto* bounds=pmm_extension::Find(document,"org.mikudancestudio.timeline.signed_play_range.v1");bounds && bounds->value.size()==8 && app.MainWindow()) {
        for(int i=0;i<2;++i) {
            const auto value=static_cast<std::int32_t>(Read32(bounds->value.data()+i*4));
            if(value<frame_limits::minimum || value>frame_limits::maximum)continue;
            char text[64]{};sprintf_s(text,"%d",value);
            SetDlgItemTextA(app.MainWindow(),i?panel::kPlayStopFrameEdit:panel::kPlayStartFrameEdit,text);
        }
    }
}
bool Load(MMDApp& app,const pmm_extension::Document& document) {
    const auto* payload=pmm_extension::Find(document,kPmmKey);
    if(!payload){Reset(&app);RestoreMetadata(app,document);return true;}
    const auto& bytes=payload->value;constexpr std::size_t stride=164;
    if(bytes.size()<8 || Read32(bytes.data())!=1)return false;
    const auto count=Read32(bytes.data()+4);
    if(std::uint64_t(count)*stride!=bytes.size()-8)return false;
    State pending;
    std::set<std::tuple<unsigned,unsigned,unsigned,int>> unique;
    for(unsigned i=0;i<count;++i) {
        const auto* row=bytes.data()+8+std::size_t(i)*stride;
        const auto slot=Read32(row),kind=Read32(row+4),target=Read32(row+8);
        const auto frame=static_cast<std::int32_t>(Read32(row+12));
        if(!unique.emplace(slot,kind,target,frame).second)return false;
        if(kind==2) {
            if(slot!=255 || !((target>=2 && target<=5)||(target>=1000 && target<1255)) || frame>=0 || frame<frame_limits::minimum)return false;
            Values values{};for(unsigned j=0;j<16;++j){std::uint64_t bits=0;for(int k=0;k<8;++k)bits|=std::uint64_t(row[36+j*8+k])<<(8*k);std::memcpy(&values[j],&bits,8);if(!std::isfinite(values[j]))return false;}
            if(!GlobalValid(values,int(target)))return false;
            const void* identity=target>=1000 ? static_cast<const void*>(app.AccessorySlot(target-1000)) : &app;
            if(identity)Upsert(pending,{nullptr,identity,int(target),frame,false,false,values});continue;
        }
        if(kind==3) {
            if(slot>=255 || frame>=0 || frame<frame_limits::minimum)return false;
            if(static_cast<std::int32_t>(target)>=0)return false;
            auto* model=app.ModelSlot(slot);if(!model)continue;
            Values values{};for(unsigned j=0;j<16;++j){std::uint64_t bits=0;for(int k=0;k<8;++k)bits|=std::uint64_t(row[36+j*8+k])<<(8*k);std::memcpy(&values[j],&bits,8);if(!std::isfinite(values[j]))return false;}
            Key key{model,mdl::Mdl(model)->boneTable,static_cast<std::int32_t>(target),frame,false,false,values};
            if(key.target<=-1000000) {
                if(!Integer(values[0],-1,254) || !Integer(values[1],-1,frame_limits::maximum))return false;
            } else if(!Integer(values[0],0,1))return false;
            if(Current(app,key))Upsert(pending,key);continue;
        }
        if(slot>=255 || kind>1 || frame>=0 || frame<frame_limits::minimum)return false;
        auto* model=app.ModelSlot(slot);if(!model)continue;auto& record=*mdl::Mdl(model);
        if(target>=(kind ? record.morphCount : record.boneCount))continue;
        const char* name=kind ? record.morphs[target].name : record.boneTable[target].name;
        if(std::memcmp(name,row+16,20))continue;
        Values values{};for(unsigned j=0;j<16;++j){std::uint64_t bits=0;for(int k=0;k<8;++k)bits|=std::uint64_t(row[36+j*8+k])<<(8*k);std::memcpy(&values[j],&bits,8);}
        if(!Valid(values,kind!=0))return false;
        Key key{model,kind ? static_cast<const void*>(record.morphs) : record.boneTable,int(target),frame,kind!=0,false,values};
        const auto before=pending.keys.size();Upsert(pending,key);if(pending.keys.size()==before)return false;
    }
    states[&app]=std::move(pending);RestoreMetadata(app,document);return true;
}
void SelectRange(MMDApp* app,int first,int last) {
    auto found=states.find(app);if(found==states.end())return;
    const HWND combo=GetDlgItem(app->MainWindow(),panel::kRegisterScopeCombo);
    const auto row=combo ? SendMessageW(combo,CB_GETCURSEL,0,0) : CB_ERR;
    const auto scope=row==CB_ERR ? FrameScope::AllFrames : static_cast<FrameScope>(SendMessageW(combo,CB_GETITEMDATA,row,0));
    for(auto& key:found->second.keys) {
        key.selected=false;
        if(!Current(*app,key) || key.frame<first || key.frame>last)continue;
        if(app->CameraMode()) {key.selected=!key.model && (row==CB_ERR || Row(*app,key)==row);continue;}
        if(key.model!=app->SelectedModel())continue;
        const auto& record=*mdl::Mdl(key.model);
        if(scope==FrameScope::AllFrames)key.selected=true;
        else if(scope==FrameScope::AllBones)key.selected=!key.morph && key.target>=0;
        else if(scope==FrameScope::AllMorphs)key.selected=key.morph;
        else if(scope==FrameScope::Display)key.selected=key.target<0;
        else if(scope==FrameScope::SelectedBones)key.selected=!key.morph && key.target>=0 && record.boneSelection[key.target];
        else if(scope==FrameScope::SelectedMorphs && key.morph) {
            for(unsigned i=0;i<mdl::FacialFrameCount(record);++i)if(record.displayFrames[i].selected && mdl::FrameTargetIndex(record.displayFrames[i])==key.target)key.selected=true;
        } else if(scope==FrameScope::NamedTrack && key.target>=0) {
            char name[256]{};GetWindowTextA(combo,name,sizeof(name));
            key.selected=std::strcmp(name,key.morph?record.morphs[key.target].name:record.boneTable[key.target].name)==0;
        }
    }
}
bool Command(MMDApp* app,unsigned id) {
    if(id==421 && app->CurrentFrame()<0) {
        auto& state=states[app];if(state.clipboard.empty()) {
            state.clipboard=NativeClipboard(app);
            for(auto& key:state.clipboard)key.clipboardOffset=key.frame;
        }
    }
    auto found=states.find(app);if(found==states.end())return false;auto& state=found->second;
    const bool selected=std::any_of(state.keys.begin(),state.keys.end(),[&](const Key& k){return k.selected && Current(*app,k);});
    if(id==420 && !selected){state.clipboard.clear();return false;}
    if(id==420 && selected) {
        state.clipboard=NativeClipboard(app);
        if(!state.clipboard.empty()) {
            const int first=NativeSelectionFirst(app);
            for(auto& key:state.clipboard)key.frame+=first;
        }
        for(auto key:state.keys)if(key.selected && Current(*app,key))state.clipboard.push_back(key);
        int first=0;for(const auto& key:state.clipboard)first=std::min(first,key.frame);
        for(auto& key:state.clipboard)key.clipboardOffset=std::int64_t(key.frame)-first;
        RefreshControls(app);return true;
    }
    if(id==421 && !state.clipboard.empty()) {
        auto pasted=state.clipboard;
        pasted.erase(std::remove_if(pasted.begin(),pasted.end(),[&](Key& key){return !PasteTarget(app,key);}),pasted.end());
        if(pasted.empty())return true;
        for(const auto& key:pasted)if(std::int64_t(app->CurrentFrame())+key.clipboardOffset>frame_limits::maximum ||
            std::int64_t(app->CurrentFrame())+key.clipboardOffset<frame_limits::minimum)return true;
        History(state);for(auto key:pasted){key.frame=int(std::int64_t(app->CurrentFrame())+key.clipboardOffset);
            if(key.frame<0)Upsert(state,key);
            else if(key.model) {
                Apply(key.model,key.target,key.morph,key.values);
                if(key.target<0)RegisterDisplayKeyCurrent(key.model,key.frame);
                else if(key.morph)RegisterMorphKeyCurrent(key.model,key.target,key.frame);
                else {
                    auto& record=*mdl::Mdl(key.model);
                    std::vector<unsigned char> selection(record.boneSelection,record.boneSelection+record.boneCount);
                    std::fill_n(record.boneSelection,record.boneCount,0);record.boneSelection[key.target]=1;
                    RegisterSelectedBoneKeys(key.model,key.frame,0);
                    std::copy(selection.begin(),selection.end(),record.boneSelection);
                }
            } else {
                ApplyGlobal(app,key.target,key.values);
                if(key.target==2)RegisterCameraState(app,key.frame);
                else if(key.target==3)RegisterLightState(app,key.frame);
                else if(key.target==4)RegisterSelfShadowState(app,key.frame);
                else if(key.target==5)RegisterGravityKeyCurrent(app,key.frame);
                else if(key.target>=1000)RegisterAccessoryKey(app,key.frame,key.target-1000);
            }
        }Changed(app);return true;
    }
    if(id==423 && selected) {
        History(state);state.keys.erase(std::remove_if(state.keys.begin(),state.keys.end(),[](const Key& k){return k.selected;}),state.keys.end());
        RefreshAfterFrameApply(app);Changed(app);return true;
    }
    if(id==400 || id==401) {
        auto& from=id==400 ? state.undo : state.redo;auto& to=id==400 ? state.redo : state.undo;
        if(from.empty())return false;
        if(app->CurrentFrame()>=0 && !from.back().drag)return false;
        auto now=Snapshot(state);now.drag=from.back().drag;
        if(now.model!=from.back().model || (now.model && now.nativeCursor!=from.back().nativeCursor)) {
            if(id==401)state.redo.clear();
            return false;
        }
        to.push_back(now);state.keys=std::move(from.back().keys);from.pop_back();
        RefreshAfterFrameApply(app);Changed(app);return true;
    }
    return false;
}
void Draw(MMDApp* app,void* dc,void* icons,int columns,int height) {
    auto found=states.find(app);if(found==states.end())return;
    for(const auto& key:found->second.keys)if(Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel())) {
        const auto column=std::int64_t(key.frame)-app->state.timelineStartFrame;const int row=Row(*app,key),y=14*row+17;
        if(column<0 || column>=columns || row<0 || (key.model && key.target>=0 && row<=0) || y>=height)continue;
        const int x=95+13*int(column);
        BitBlt(HDC(dc),x,y,11,11,HDC(icons),44,0,SRCAND);
        BitBlt(HDC(dc),x,y,11,11,HDC(icons),key.selected?22:0,0,SRCPAINT);
    }
}
bool SelectCell(MMDApp* app,int frame,int row,bool additive) {
    auto found=states.find(app);if(found==states.end())return false;bool hit=false;
    for(auto& key:found->second.keys)if(Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel())) {
        if(key.frame==frame && Row(*app,key)==row){key.selected=additive ? !key.selected : true;hit=true;}
        else if(!additive)key.selected=false;
    }
    return hit;
}
bool CellSelected(MMDApp* app,int frame,int row) {
    auto found=states.find(app);if(found==states.end())return false;
    return std::any_of(found->second.keys.begin(),found->second.keys.end(),[&](const Key& key){
        return Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel()) &&
            key.frame==frame && Row(*app,key)==row && key.selected;
    });
}
void BeginDrag(MMDApp* app) {
    auto found=states.find(app);if(found==states.end())return;
    auto& state=found->second;state.dragging.clear();state.dragOffset=0;state.dragHistory=false;
    for(std::size_t i=0;i<state.keys.size();++i) {
        const auto& key=state.keys[i];
        if(key.selected && Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel()))
            state.dragging.emplace_back(i,key.frame);
    }
    std::vector<bool> moving(state.keys.size(),false);
    for(const auto& item:state.dragging)moving[item.first]=true;
    // Compute collision limits once at press time, not on every mouse move.
    std::int64_t low=frame_limits::minimum,high=frame_limits::maximum;
    for(const auto& [index,frame]:state.dragging) {
        if(index>=state.keys.size())return;
        low=std::max(low,std::int64_t(frame_limits::minimum)-frame);
        high=std::min(high,std::int64_t(-1)-frame);
        const auto& key=state.keys[index];
        for(std::size_t i=0;i<state.keys.size();++i) {
            const auto& other=state.keys[i];
            if(other.model!=key.model || other.identity!=key.identity || other.target!=key.target || other.morph!=key.morph)continue;
            if(moving[i])continue;
            if(other.frame<frame)low=std::max(low,std::int64_t(other.frame)+1-frame);
            else if(other.frame>frame)high=std::min(high,std::int64_t(other.frame)-1-frame);
        }
    }
    state.dragMin=low;state.dragMax=high;
}
void Drag(MMDApp* app,int offset) {
    auto found=states.find(app);if(found==states.end())return;
    auto& state=found->second;if(state.dragging.empty())return;
    // Preserve track order, spacing and frame-zero boundary. Always use the
    // press snapshot so reversing direction restores the original frames.
    const int effective=int(std::clamp(std::int64_t(offset),state.dragMin,state.dragMax));
    if(effective==state.dragOffset)return;
    if(!state.dragHistory){History(state);state.undo.back().drag=true;state.dragHistory=true;}
    for(const auto& [index,frame]:state.dragging)state.keys[index].frame=frame+effective;
    state.dragOffset=effective;app->SceneModified()=1;RefreshControls(app);
}
void EndDrag(MMDApp* app) {
    auto found=states.find(app);if(found==states.end())return;
    auto& state=found->second;
    if(state.dragHistory && state.dragOffset==0 && !state.undo.empty())state.undo.pop_back();
    state.dragging.clear();state.dragHistory=false;state.dragOffset=0;
}
void SelectBox(MMDApp* app,std::int64_t first,std::int64_t last,int top,int bottom,bool selected,bool additive) {
    auto found=states.find(app);if(found==states.end())return;
    for(auto& key:found->second.keys)if(Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel())) {
        const int row=Row(*app,key);
        const bool visible=row>=0 && (!key.model || key.target<0 || row>0);
        if(visible && key.frame>=first && key.frame<last && row>=top && row<bottom)key.selected=selected;
        else if(!additive)key.selected=false;
    }
}
void SelectColumns(MMDApp* app) {
    auto found=states.find(app);if(found==states.end())return;
    const auto active=[&](const Key& key){return Current(*app,key) && (app->CameraMode()?!key.model:key.model==app->SelectedModel());};
    std::vector<int> frames;
    for(const auto& key:found->second.keys)if(active(key) && key.selected)frames.push_back(key.frame);
    for(auto& key:found->second.keys)if(active(key) && std::find(frames.begin(),frames.end(),key.frame)!=frames.end())key.selected=true;
}
}
