#include "mikumikustudio/clipboard_layout.hpp"
namespace vmd_model_host_checks {
struct File {
    std::wstring path;std::vector<unsigned char> bytes;
    template<class T>void Put(const T& value){const auto* p=reinterpret_cast<const unsigned char*>(&value);bytes.insert(bytes.end(),p,p+sizeof(value));}
    void Raw(const void* data,std::size_t count){const auto* p=static_cast<const unsigned char*>(data);bytes.insert(bytes.end(),p,p+count);}
    File(int version,const mdl::ModelRecord& model,unsigned bone,int morph){
        path=std::filesystem::absolute(L"model-motion-\u6d4b\u8bd5-"+std::to_wstring(version)+L".vmd").native();
        char header[30]{};strcpy_s(header,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002");Raw(header,30);
        char name[20]{};std::memcpy(name,model.name,(std::min)(std::size_t(version==1?10:20),strnlen_s(model.name,sizeof(model.name))));Raw(name,version==1?10:20);
        Put(std::uint32_t(2));
        for(unsigned i=0;i<2;++i){char target[15]{};std::memcpy(target,model.boneTable[bone].name,strnlen_s(model.boneTable[bone].name,15));Raw(target,15);Put(std::uint32_t(i*8));
            const float position[3]={3+float(i),4,5},rotation[4]={0,.6f,0,.8f};Raw(position,sizeof(position));Raw(rotation,sizeof(rotation));
            unsigned char curve[64]{};for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)curve[16*lane+4*row]=(row<2?20:107);Raw(curve,sizeof(curve));}
        Put(std::uint32_t(morph<0?0:2));if(morph>=0)for(unsigned i=0;i<2;++i){char target[15]{};std::memcpy(target,model.morphs[morph].name,strnlen_s(model.morphs[morph].name,15));Raw(target,15);Put(std::uint32_t(i*8));Put(.23f+.11f*float(i));}
        Put(std::uint32_t(0));Put(std::uint32_t(0));if(version==2)Put(std::uint32_t(0));
        Put(std::uint32_t(2));for(unsigned i=0;i<2;++i){Put(std::uint32_t(i*8));Put(std::uint8_t(i!=0));Put(std::uint32_t(0));}
        FILE* file=nullptr;Check(!_wfopen_s(&file,path.c_str(),L"wb")&&file,"open Unicode model VMD fixture");Check(std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size(),"write standard model VMD bytes");Check(!std::fclose(file),"close model VMD fixture");
    }
    ~File(){DeleteFileW(path.c_str());}
};
unsigned ShortBone(const mdl::ModelRecord& model){
    for(unsigned i=0;i<model.boneCount;++i)if(model.boneTable[i].name[0]&&strnlen_s(model.boneTable[i].name,20)<15&&model.boneTable[i].type!=mdl::BoneType::FixedAxis&&
        !(model.boneTable[i].type==mdl::BoneType::UnderIk&&(model.boneTable[i].flags&mdl::kBoneFlagFixedAxis)))return i;
    for(unsigned i=0;i<model.boneCount;++i)if(model.boneTable[i].name[0]&&strnlen_s(model.boneTable[i].name,20)<15)return i;
    Check(false,"real model has a legacy VMD bone name");return 0;
}
int ShortMorph(const mdl::ModelRecord& model){for(unsigned i=0;i<model.morphCount;++i)if(model.morphs[i].name[0]&&strnlen_s(model.morphs[i].name,20)<15)return int(i);return -1;}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    Check(app->ModelSlot(0)&&!history.UndoCount(),"real model VMD starts with loaded PMX or PMD and empty history");auto& model=*mdl::Mdl(app->ModelSlot(0));
    const auto target=ShortBone(model);const auto morph=ShortMorph(model);const auto frame=app->CurrentFrame();const auto camera=app->CameraMode();const auto last=app->LastRegisteredFrame();const auto slot=app->SelectedModelSlot();const auto mode=history.Mode();const auto initial=value();
    std::array<char,sizeof(model.name)> originalName{};std::memcpy(originalName.data(),model.name,sizeof(model.name));
    // Both legacy header widths can represent this exact current model name.
    std::memset(model.name,0,sizeof(model.name));std::memcpy(model.name,"host-vmd",8);
    const auto counts=app->ClipboardCounts();const auto boneClip=app->BoneClipboard();const auto morphClip=app->MorphClipboard();const auto displayClip=app->DisplayClipboard();
    for(int version:{1,2})for(auto recording:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Check(history.SetMode(recording)==MMS_OK,"set model VMD explicit recording mode");app->CameraMode()=0;app->SelectedModelSlot()=0;app->CurrentFrame()=769;
        File file(version,model,target,morph);const model_paste_host_checks::Snapshot before(model);const auto max=model.maxFrame;const auto cursor=model.searchCursor;
        Menu(app,L"Edit probe prepared");LoadVmdFile(file.path.c_str());Check(before.Matches(model)&&!history.UndoCount(),"pending plugin reservation blocks model VMD without writes");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");
        const auto ansiSource=std::filesystem::absolute(L"model-vmd-return.vmd");
        Check(CopyFileW(file.path.c_str(),ansiSource.c_str(),FALSE),"create ASCII source for model VMD return compatibility");
        int legacyResult=int(model.morphCount);for(unsigned index=0;index<model.boneCount;++index){const auto type=model.boneTable[index].type;
            if(static_cast<std::int8_t>(type)<=static_cast<std::int8_t>(mdl::BoneType::Effector)||type==mdl::BoneType::FixedAxis)legacyResult=int(index)+1;}
        Check(LoadVmdMotion(app,"model-vmd-return.vmd")==legacyResult,"ANSI model VMD preserves the legacy last-evaluated loop counter without reevaluating");
        Check(DeleteFileW(ansiSource.c_str()),"remove ANSI model source immediately after import");
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==769,"real Unicode model VMD imports one explicit node between plugin edits");
        unsigned at=target;while(model.boneKeys[at].frame<769&&model.boneKeys[at].next)at=model.boneKeys[at].next;
        Check(model.boneKeys[at].frame==769&&model.boneKeys[at].interpolation[0]==20&&model.boneKeys[at].interpolation[8]==107,"real model VMD imports the named bone and interpolation at current-frame offset");
        if(model.boneTable[target].type!=mdl::BoneType::FixedAxis&&!(model.boneTable[target].type==mdl::BoneType::UnderIk&&(model.boneTable[target].flags&mdl::kBoneFlagFixedAxis)))
            Check(model.boneKeys[at].rotation[1]==.6f&&model.boneKeys[at].rotation[3]==.8f,"real model VMD preserves regular bone quaternion");
        if(morph>=0){unsigned key=unsigned(morph);while(model.morphKeys[key].frame<769&&model.morphKeys[key].next)key=model.morphKeys[key].next;
            Check(model.morphKeys[key].frame==769&&model.morphKeys[key].value==.23f,"real model VMD imports matched morph weights");}
        unsigned display=0;while(model.displayKeys[display].frame<769&&model.displayKeys[display].next)display=model.displayKeys[display].next;
        Check(model.displayKeys[display].frame==769&&!model.displayKeys[display].visible,"real model VMD imports explicit model visibility with empty IK list");
        Check(app->BoneClipboard()==boneClip&&app->MorphClipboard()==morphClip&&app->DisplayClipboard()==displayClip&&!std::memcmp(&counts,&app->ClipboardCounts(),sizeof(counts)),"model VMD preserves live clipboard storage and counts");
        const model_paste_host_checks::Snapshot after(model);LoadVmdFile(file.path.c_str());Check(history.UndoCount()==2&&after.Matches(model),"identical real model VMD keeps history unchanged");
        Check(DeleteFileW(file.path.c_str()),"remove model VMD source before history traversal");Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(before.Matches(model)&&model.maxFrame==max&&model.searchCursor==cursor&&value()==initial+1,"real model VMD undo restores keys marks metadata and interleaved plugin value");
        app->CameraMode()=1;app->SelectedModelSlot()=1;app->CurrentFrame()=31;Menu(app,L"Edit probe redo");
        Check(after.Matches(model)&&app->CurrentFrame()==31,"real model VMD redo owns source across model and playhead changes");Menu(app,L"Edit probe redo");
        for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");Check(value()==initial&&history.Reset()==MMS_OK,"model VMD common journal returns to baseline");history.Collect();
    }
    std::memcpy(model.name,originalName.data(),sizeof(model.name));app->CurrentFrame()=frame;app->CameraMode()=camera;app->SelectedModelSlot()=slot;app->LastRegisteredFrame()=last;Check(history.SetMode(mode)==MMS_OK,"restore model VMD history mode");app->SceneModified()=0;
    std::puts("PASS real Unicode model VMD v1/v2 bone morph display explicit modes clipboard preservation and interleaved deleted-source history");
}
}
