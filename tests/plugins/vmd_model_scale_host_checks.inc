// Included after vmd_model_host_checks.inc; uses its real VMD 1/2 writer.
namespace vmd_model_scale_host_checks {
unsigned FindKey(const mdl::ModelRecord& model,unsigned bone,unsigned frame){
    unsigned at=bone;for(std::size_t visited=0;visited<mdl::BoneKeyCapacity(model.boneKeys);++visited){
        const auto& key=model.boneKeys[at];if(key.frame==frame)return at;
        if(!key.next)break;at=key.next;
    }Check(false,"real scaled model VMD key exists");return 0;
}
void AppendScale(vmd_model_host_checks::File& file,const mdl::ModelRecord& model,unsigned bone){
    bone_scale_io::VmdExtension extension;bone_scale_io::PrefixFingerprint prefix;
    prefix.Append(file.bytes.data(),file.bytes.size());extension.prefixBytes=prefix.bytes();extension.prefixCrc32=prefix.crc32();
    for(unsigned i=0;i<2;++i){bone_scale_io::VmdExtension::Entry entry;
        std::memcpy(entry.name.data(),model.boneTable[bone].name,strnlen_s(model.boneTable[bone].name,15));entry.frame=i*8;
        if(!i)entry.scale.v[0]=2.5f;entry.curve=i?bone_scale::Curve{8,31,92,120}:bone_scale::Curve{14,37,88,118};extension.entries.push_back(entry);}
    const auto tail=bone_scale_io::EncodeExtension(extension);file.Raw(tail.data(),tail.size());file.Put(std::uint64_t(tail.size()));file.Raw("MMSXVMDX",8);
    FILE* stream=nullptr;Check(!_wfopen_s(&stream,file.path.c_str(),L"wb")&&stream,"open scaled model VMD fixture");
    Check(std::fwrite(file.bytes.data(),1,file.bytes.size(),stream)==file.bytes.size(),"write real writer-produced scale/curve tail");Check(!std::fclose(stream),"close scaled VMD fixture");
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    auto* raw=app->ModelSlot(0);Check(raw&&!history.UndoCount(),"real scale VMD starts with loaded model and empty common history");auto& model=*mdl::Mdl(raw);
    const auto bone=vmd_model_host_checks::ShortBone(model);const auto morph=vmd_model_host_checks::ShortMorph(model);
    const auto frame=app->CurrentFrame();const auto camera=app->CameraMode();const auto selected=app->SelectedModelSlot();const auto last=app->LastRegisteredFrame();const auto mode=history.Mode();const auto initial=value();
    std::array<char,sizeof(model.name)> name{};std::memcpy(name.data(),model.name,sizeof(model.name));std::memset(model.name,0,sizeof(model.name));std::memcpy(model.name,"scale-vmd",9);
    for(int version:{1,2})for(auto recording:{plugins::SharedHistory::RecordingMode::ExplicitOnly,plugins::SharedHistory::RecordingMode::AllOperations}){
        Check(history.SetMode(recording)==MMS_OK,"configure real scale VMD recording mode");app->CameraMode()=0;app->SelectedModelSlot()=0;app->CurrentFrame()=853;
        const model_paste_host_checks::Snapshot before(model);const auto beforeScale=bone_scale::AuthoringState::Capture(raw);
        vmd_model_host_checks::File file(version,model,bone,morph);AppendScale(file,model,bone);
        Menu(app,L"Edit probe once");LoadVmdFile(file.path.c_str());
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==853,"real standard plus scale/curve VMD publishes one explicit common transaction");
        const auto first=FindKey(model,bone,853),second=FindKey(model,bone,861);
        bone_scale::Matrix scaled;scaled.v[0]=2.5f;
        Check(bone_scale::Key(raw,int(first))==scaled&&bone_scale::KeyCurve(raw,int(first))==bone_scale::Curve{14,37,88,118}&&
            bone_scale::Key(raw,int(second))==bone_scale::Matrix{}&&bone_scale::KeyCurve(raw,int(second))==bone_scale::Curve{8,31,92,120},"real import keeps scale and scale-to-identity interpolation curves on their individual keys");
        const model_paste_host_checks::Snapshot after(model);const auto afterScale=bone_scale::AuthoringState::Capture(raw);
        LoadVmdFile(file.path.c_str());Check(history.UndoCount()==2&&after.Matches(model)&&afterScale.Equals(bone_scale::AuthoringState::Capture(raw)),"identical real scaled VMD adds no redundant history");
        Check(DeleteFileW(file.path.c_str()),"delete real scale VMD source before undo redo");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==initial+1&&before.Matches(model)&&beforeScale.Equals(bone_scale::AuthoringState::Capture(raw)),"one real undo restores standard and all scale authoring with interleaved plugin history");
        app->CameraMode()=1;app->SelectedModelSlot()=1;app->CurrentFrame()=27;Menu(app,L"Edit probe redo");
        Check(after.Matches(model)&&afterScale.Equals(bone_scale::AuthoringState::Capture(raw))&&app->CurrentFrame()==27,"real scale VMD redo retains owned source after deletion and UI target change");
        Menu(app,L"Edit probe redo");for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"real scaled model VMD returns journal to baseline");history.Collect();
    }
    std::memcpy(model.name,name.data(),sizeof(model.name));app->CurrentFrame()=frame;app->CameraMode()=camera;app->SelectedModelSlot()=selected;app->LastRegisteredFrame()=last;
    Check(history.SetMode(mode)==MMS_OK,"restore real scale VMD recording mode");app->SceneModified()=0;
    std::puts("PASS real model VMD v1/v2 scale matrix and nonlinear/identity scale curves with deleted-source interleaved history in both modes");
}
}
