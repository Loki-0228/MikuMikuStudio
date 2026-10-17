#include "mikudancestudio/clipboard_layout.hpp"
#include "mikudancestudio/bone_scale.hpp"
namespace model_paste_host_checks {
struct Snapshot {
    std::vector<mdl::BoneKey> bones;
    std::vector<mdl::MorphKey> morphs;
    display_paste_host_checks::Snapshot display;
    explicit Snapshot(const mdl::ModelRecord& model):bones(model.boneKeys,model.boneKeys+mdl::BoneKeyCapacity(model.boneKeys)),
        morphs(model.morphKeys,model.morphKeys+mdl::MorphKeyCapacity(model.morphKeys)),display(model){}
    bool Matches(const mdl::ModelRecord& model)const {
        return mdl::BoneKeyCapacity(model.boneKeys)>=bones.size()&&mdl::MorphKeyCapacity(model.morphKeys)>=morphs.size()&&
            !std::memcmp(model.boneKeys,bones.data(),bones.size()*sizeof(mdl::BoneKey))&&
            !std::memcmp(model.morphKeys,morphs.data(),morphs.size()*sizeof(mdl::MorphKey))&&display.Matches(model);
    }
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    auto& model=*mdl::Mdl(app->ModelSlot(0));Check(model.boneCount&&model.morphCount&&!history.UndoCount(),"real mixed model paste fixture has bone morph and display targets");
    const auto frame=app->CurrentFrame();const auto slot=app->SelectedModelSlot();const auto camera=app->CameraMode();const auto mode=history.Mode();
    const auto counts=app->ClipboardCounts();auto* oldBone=app->BoneClipboard();auto* oldMorph=app->MorphClipboard();auto* oldDisplay=app->DisplayClipboard();
    const auto initial=value();const auto last=app->LastRegisteredFrame();
    std::array<mdl::BoneClipboardRecord,3> bones{};std::array<mdl::MorphClipboardRecord,3> morphs{};std::array<mdl::DisplayClipboardRecord,3> displays{};
    std::array<std::vector<mdl::IkClipboardState>,3> ik;std::array<std::vector<mdl::SelectorClipboardState>,3> parents;
    const auto targetMorph=model.morphCount>1?1u:0u;
    for(unsigned i=0;i<3;++i) {
        std::memcpy(bones[i].name,model.boneTable[0].name,20);bones[i].frame=i==1?9:0;bones[i].rotation[3]=1;bones[i].position[1]=13+float(i);
        for(unsigned lane=0;lane<4;++lane){bones[i].interpolation[lane]=bones[i].interpolation[lane+4]=20;bones[i].interpolation[lane+8]=bones[i].interpolation[lane+12]=107;}
        bone_scale::CopyClipboard(app,int(i),bone_scale::Matrix{},false,bone_scale::LinearCurve());
        std::memcpy(morphs[i].name,model.morphs[targetMorph].name,20);morphs[i].frame=bones[i].frame;morphs[i].value=.17f+.1f*float(i);
        displays[i].frame=bones[i].frame;displays[i].visible=static_cast<unsigned char>(i%2);
        ik[i].resize(model.ikChainCount);
        for(unsigned k=0;k<model.ikChainCount;++k){const auto bone=model.ikChains[k].boneIndex;Check(bone>=0&&unsigned(bone)<model.boneCount,"actual IK source bone valid");
            std::memcpy(ik[i][k].boneName,model.boneTable[bone].name,20);ik[i][k].enabled=static_cast<unsigned char>((i+k)%2);}
        parents[i].resize(model.boneOrderCount);const auto* order=static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
        for(unsigned k=0;k<model.boneOrderCount;++k){parents[i][k].modelIndex=-1;if(k){Check(order[k].boneIndex>=0&&unsigned(order[k].boneIndex)<model.boneCount,"actual parent source bone valid");
            std::memcpy(parents[i][k].boneName,model.boneTable[order[k].boneIndex].name,20);}}
        displays[i].ikCount=int(ik[i].size());displays[i].ikStates=ik[i].data();displays[i].selectorCount=int(parents[i].size());displays[i].selectorStates=parents[i].data();
    }
    const auto bind=[&](unsigned mask){app->BoneClipboard()=(mask&1)?bones.data():nullptr;app->MorphClipboard()=(mask&2)?morphs.data():nullptr;app->DisplayClipboard()=(mask&4)?displays.data():nullptr;
        app->ClipboardCounts()={};if(mask&1)app->ClipboardCounts().bones=3;if(mask&2)app->ClipboardCounts().morphs=3;if(mask&4)app->ClipboardCounts().displays=3;};
    const auto command=[&]{SendMessageW(app->MainWindow(),WM_COMMAND,421,0);};
    for(unsigned mask:{3u,5u,6u,7u})for(auto recording:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        Check(history.SetMode(recording)==MMS_OK,"configure actual mixed model recording mode");bind(mask);app->CurrentFrame()=431;app->SelectedModelSlot()=0;app->CameraMode()=0;
        const Snapshot before(model);const auto oldMax=model.maxFrame;const auto cursor=model.searchCursor;
        std::array<unsigned char,sizeof(model.undoRings)> rings{};std::memcpy(rings.data(),model.undoRings,rings.size());const auto ring=model.undoState[0],redo=model.undoState[1];
        Menu(app,L"Edit probe prepared");command();Check(before.Matches(model)&&!history.UndoCount(),"pending plugin rejects actual mixed model paste before any family writes");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");command();Check(history.UndoCount()==2&&value()==initial+1,"actual mixed model paste creates one explicit history node between plugin edits");
        const Snapshot after(model);command();Check(history.UndoCount()==2&&after.Matches(model),"actual identical mixed model paste adds no history");
        app->BoneClipboard()=nullptr;app->MorphClipboard()=nullptr;app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(before.Matches(model)&&model.maxFrame==oldMax&&model.searchCursor==cursor&&value()==initial+1,"actual mixed undo restores deep data all marks and metadata");
        app->SelectedModelSlot()=1;app->CameraMode()=1;app->CurrentFrame()=27;Menu(app,L"Edit probe redo");
        Check(after.Matches(model)&&app->CurrentFrame()==27&&app->LastRegisteredFrame()==last,"actual mixed redo owns every clipboard family across model and frame changes");
        Check(model.undoState[0]==ring&&model.undoState[1]==redo&&!std::memcmp(model.undoRings,rings.data(),rings.size()),"actual mixed paste leaves legacy rings unchanged");
        Menu(app,L"Edit probe redo");for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"actual mixed paste returns plugin journal to baseline");history.Collect();
    }
    app->BoneClipboard()=oldBone;app->MorphClipboard()=oldMorph;app->DisplayClipboard()=oldDisplay;app->ClipboardCounts()=counts;
    app->CurrentFrame()=frame;app->SelectedModelSlot()=slot;app->CameraMode()=camera;app->LastRegisteredFrame()=last;
    Check(history.SetMode(mode)==MMS_OK,"restore actual mixed model history mode");app->SceneModified()=0;
    std::puts("PASS actual command 421 every two-family and three-family model paste with deep payloads and plugin history in both modes");
}
}
