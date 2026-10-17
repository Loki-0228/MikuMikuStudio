#include "../src/plugins/native_frame_lines.hpp"
namespace frame_line_host_checks {
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    using Mode=plugins::SharedHistory::RecordingMode;
    const auto oldMode=history.Mode();const auto frame=app->CurrentFrame();const auto cameraMode=app->CameraMode();
    Check(history.Reset()==MMS_OK,"frame line actual-host fixture starts with empty history");history.Collect();
    const auto initialPlugin=value();
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned family=0;family<2;++family) {
        Check(history.SetMode(mode)==MMS_OK,"actual frame line history mode accepted");
        app->CameraMode()=1;app->CurrentFrame()=41;
        const auto keysCamera=std::vector<mdl::CameraKey>(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        const auto keysLight=std::vector<mdl::LightKey>(app->LightKeys(),app->LightKeys()+mdl::KeyPoolCapacity(app->LightKeys()));
        // Register via the real menu route to create a key in the host's owned pool.
        SendMessageW(app->MainWindow(),WM_COMMAND,family?468:452,0);
        Check(history.UndoCount()==1,"actual registrar seeds global frame line fixture");
        const auto registeredCamera=std::vector<mdl::CameraKey>(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        const auto registeredLight=std::vector<mdl::LightKey>(app->LightKeys(),app->LightKeys()+mdl::KeyPoolCapacity(app->LightKeys()));
        Menu(app,L"Edit probe once");SendMessageW(app->MainWindow(),WM_COMMAND,family?257:255,0);
        const auto has=[&](unsigned f){if(family){for(std::size_t i=1;i<mdl::KeyPoolCapacity(app->LightKeys());++i)if(app->LightKeys()[i].frame==f)return true;}
            else for(std::size_t i=1;i<mdl::KeyPoolCapacity(app->CameraKeys());++i)if(app->CameraKeys()[i].frame==f)return true;return false;};
        Check(history.UndoCount()==3&&has(42)&&!has(41)&&value()==initialPlugin+1,"actual insert-frame-line joins native and plugin explicit history");
        SendMessageW(app->MainWindow(),WM_COMMAND,family?258:256,0);
        Check(history.UndoCount()==4&&has(41)&&!has(42),"actual delete-frame-line shifts global key back");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(std::memcmp(app->CameraKeys(),registeredCamera.data(),registeredCamera.size()*sizeof(mdl::CameraKey))==0&&
            std::memcmp(app->LightKeys(),registeredLight.data(),registeredLight.size()*sizeof(mdl::LightKey))==0,"undo both frame lines restores registered pool bytes");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(!history.UndoCount()&&value()==initialPlugin&&std::memcmp(app->CameraKeys(),keysCamera.data(),keysCamera.size()*sizeof(mdl::CameraKey))==0&&
            std::memcmp(app->LightKeys(),keysLight.data(),keysLight.size()*sizeof(mdl::LightKey))==0,"frame line and real plugin undo restore original pools and plugin state");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe redo");
        Check(history.UndoCount()==4&&has(41)&&value()==initialPlugin+1,"actual plugin redo replays both frame line transactions");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");
        Check(history.Reset()==MMS_OK,"clear completed frame line history fixture");history.Collect();
    }
    const auto selectedSlot=app->SelectedModelSlot();app->SelectedModelSlot()=0;
    auto& model=*mdl::Mdl(app->ModelSlot(0));
    Check(model.boneCount&&model.morphCount,"actual model frame line fixture has bone and morph tracks");
    const unsigned morphIndex=model.morphCount>1?1:0;
    const auto selected=model.boneSelection[0];const auto selectedMorph=model.selectedMorphs[0];
    const auto physics=model.physicsMode;const auto initialPosition=model.boneTable[0].trans[0];const auto initialDirty=model.bonePhysicsState[0];const auto initialWeight=model.morphs[morphIndex].value;
    model.boneSelection[0]=1;model.selectedMorphs[0]=int(morphIndex);if(!morphIndex)model.physicsMode=2;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned family=0;family<2;++family) {
        Check(history.SetMode(mode)==MMS_OK,"actual model frame line history mode accepted");
        app->CameraMode()=0;app->CurrentFrame()=43;
        const auto keysBone=std::vector<mdl::BoneKey>(model.boneKeys,model.boneKeys+mdl::BoneKeyCapacity(model.boneKeys));
        const auto keysMorph=std::vector<mdl::MorphKey>(model.morphKeys,model.morphKeys+mdl::MorphKeyCapacity(model.morphKeys));
        if(family)model.morphs[morphIndex].value=.456123f;else {model.boneTable[0].trans[0]=.789123f;model.bonePhysicsState[0]=1;}
        const auto authorPosition=model.boneTable[0].trans[0];const auto authorWeight=model.morphs[morphIndex].value;
        SendMessageW(app->MainWindow(),WM_COMMAND,family?525:500,0);
        Check(history.UndoCount()==1,"actual model registrar seeds frame line fixture");
        Menu(app,L"Edit probe once");SendMessageW(app->MainWindow(),WM_COMMAND,family?257:255,0);
        const auto has=[&](unsigned f){if(family){for(std::size_t i=model.morphCount;i<mdl::MorphKeyCapacity(model.morphKeys);++i)if(model.morphKeys[i].frame==f)return true;}
            else for(std::size_t i=model.boneCount;i<mdl::BoneKeyCapacity(model.boneKeys);++i)if(model.boneKeys[i].frame==f)return true;return false;};
        Check(history.UndoCount()==3&&has(44)&&!has(43),"actual model insert frame line joins plugin journal");
        SendMessageW(app->MainWindow(),WM_COMMAND,family?258:256,0);
        Check(history.UndoCount()==4&&has(43)&&!has(44),"actual model delete frame line shifts key back");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");
        Check(!history.UndoCount()&&value()==initialPlugin&&std::memcmp(model.boneKeys,keysBone.data(),keysBone.size()*sizeof(mdl::BoneKey))==0&&
            std::memcmp(model.morphKeys,keysMorph.data(),keysMorph.size()*sizeof(mdl::MorphKey))==0,"model frame line and plugin undo restore original key pools");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe redo");
        Check(history.UndoCount()==4&&has(43)&&value()==initialPlugin+1&&model.boneTable[0].trans[0]==authorPosition,
            "actual model frame line redo preserves dirty author position");
        if(family)Check(model.morphs[morphIndex].value==authorWeight,"actual morph frame line redo retains current author weight");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");
        Check(history.Reset()==MMS_OK,"clear completed model frame line fixture");history.Collect();
    }
    model.boneSelection[0]=selected;model.selectedMorphs[0]=selectedMorph;model.physicsMode=physics;
    model.boneTable[0].trans[0]=initialPosition;model.bonePhysicsState[0]=initialDirty;model.morphs[morphIndex].value=initialWeight;app->SelectedModelSlot()=selectedSlot;
    Check(history.SetMode(oldMode)==MMS_OK,"restore actual host history mode");app->CurrentFrame()=frame;app->CameraMode()=cameraMode;app->SceneModified()=0;
    std::puts("PASS actual global frame line commands, both history modes and real plugin interleave");
}
}
