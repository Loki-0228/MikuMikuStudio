#include "mikudancestudio/accessory_motion_io.hpp"
#include "../../src/plugins/native_vmd_source.hpp"
namespace vmd_accessory_host_checks {
unsigned At(MMDApp* app,unsigned slot,unsigned frame){const auto* keys=app->AccessoryKeys(slot);unsigned at=0;while(keys[at].frame<frame&&keys[at].next)at=keys[at].next;Check(keys[at].frame==frame,"real accessory VMD key exists");return at;}
void Tail(vmd_global_host_checks::File& file,const std::vector<accessory_motion_io::Track>& tracks){const auto tail=accessory_motion_io::Encode(tracks);file.bytes.insert(file.bytes.end(),tail.begin(),tail.end());file.Write();}
accessory_motion_io::Track Track(const char* name,float x){accessory_motion_io::Track track;track.name=name;for(unsigned i=0;i<3;++i){accessory_motion_io::Key key;key.frame=i*5;key.position[0]=x+float(i);key.rotation[1]=.15f*float(i);key.scale=1.2f;key.opacity=.7f;key.parentModel=254;key.parentBone=42;track.keys.push_back(key);}return track;}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    auto* a=app->AccessorySlot(0);auto* b=app->AccessorySlot(1);Check(a&&b&&!history.UndoCount(),"real accessory import starts with two loaded accessories and empty history");
    const auto oldA=*a,oldB=*b;const auto frame=app->CurrentFrame();const auto camera=app->CameraMode();const auto last=app->LastRegisteredFrame();const auto mode=history.Mode();const auto initial=value();
    const auto counts=app->ClipboardCounts();const auto cameraClip=app->CameraClipboard();const auto accessoryClip=app->AccessoryClipboard();
    strcpy_s(a->name,"same");strcpy_s(b->name,"same");
    // Accessory order is a zero-based permutation: ValidOrder checks every
    // position [0, accessoryCount), unlike the one-based model combo order.
    a->order=1;b->order=0;
    for(auto recording:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Check(history.SetMode(recording)==MMS_OK,"set real accessory import recording mode");app->CameraMode()=1;app->CurrentFrame()=711;
        vmd_global_host_checks::File file(2);Tail(file,{Track("same",21),Track("same",41),Track("unmatched",61)});
        const global_paste_host_checks::Keys before(*app);Menu(app,L"Edit probe once");LoadVmdFile(file.path.c_str());
        Check(history.UndoCount()==2&&value()==initial+1&&app->AccessoryKeys(1)[At(app,1,711)].position[0]==21&&app->AccessoryKeys(0)[At(app,0,711)].position[0]==41,
            "real VMD matches duplicate accessory names by panel order within one explicit standard-plus-accessory transaction");
        Check(app->AccessoryKeys(1)[At(app,1,711)].parentModel==-1&&app->AccessoryKeys(1)[At(app,1,711)].parentBone==0,"real missing accessory parent safely detaches");
        Check(app->CameraClipboard()==cameraClip&&app->AccessoryClipboard()==accessoryClip&&!std::memcmp(&counts,&app->ClipboardCounts(),sizeof(counts)),"real accessory VMD never replaces clipboard metadata");
        const global_paste_host_checks::Keys after(*app);LoadVmdFile(file.path.c_str());Check(history.UndoCount()==2&&after.Equals(*app),"real identical accessory VMD preserves journal");
        Check(DeleteFileW(file.path.c_str()),"remove real accessory VMD before undo");Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(before.Equals(*app)&&value()==initial+1,"real accessory undo restores all families between plugin edits");
        Menu(app,L"Edit probe redo");Check(after.Equals(*app),"real accessory redo uses owned source without rereading VMD");Menu(app,L"Edit probe redo");
        for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");Check(value()==initial&&history.Reset()==MMS_OK,"real accessory history returns to baseline");history.Collect();
    }
    *a=oldA;*b=oldB;app->CurrentFrame()=frame;app->CameraMode()=camera;app->LastRegisteredFrame()=last;Check(history.SetMode(mode)==MMS_OK,"restore accessory import recording mode");app->SceneModified()=0;
    std::puts("PASS real accessory VMD mixed import duplicate names panel order clipboard preservation and interleaved history");
}
// Run last: this deliberately replaces the scene and invalidates history references.
void RunPersistence(MMDApp* app){
    Check(app&&app->AccessorySlot(0)&&plugins::BoundNativePoseHistory(app),"accessory persistence requires a loaded accessory and bound history");
    app->CameraMode()=1;app->CurrentFrame()=733;auto* accessory=app->AccessorySlot(0);strcpy_s(accessory->name,"vmd-persistence-target");const std::string name(accessory->name,strnlen_s(accessory->name,99));
    vmd_global_host_checks::File file(2);Tail(file,{Track(name.c_str(),73)});LoadVmdFile(file.path.c_str());
    const auto expected=app->AccessoryKeys(0)[At(app,0,733)];Check(expected.position[0]==73,"accessory motion committed before real PMM save");
    const auto project=std::filesystem::absolute(L"vmd-accessory-persistence.pmm");Check(SaveSceneCopy(app,project.c_str()),"save imported accessory motion in real PMM");
    Check(DeleteFileW(file.path.c_str()),"remove VMD before PMM reopen");ResetAppState(app);wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();
    Check(app->AccessorySlot(0)!=nullptr,"PMM reopens accessory resource");const auto& restored=app->AccessoryKeys(0)[At(app,0,733)];
    // PMM's accessory opacity is a packed percentage byte, not a float:
    // save truncates double(opacity)*100, wraps 2*(-28-percent) into u8,
    // and load reconstructs (100-(byte>>1))/100.f. In particular .7f is
    // below exact .7, so this legacy format round-trips it as .69f.
    const int opacityPercent=static_cast<int>(static_cast<double>(expected.opacity)*100.0);
    const auto packedOpacity=static_cast<unsigned char>((-28-opacityPercent)*2+(expected.visible?1:0));
    const float expectedPmmOpacity=static_cast<float>(100-(packedOpacity>>1))/100.0f;
    auto printKey=[](const char* label,const mdl::AccessoryKey& key){
        std::printf("DIAG PMM accessory %s frame=%u position=(%.9g,%.9g,%.9g) rotation=(%.9g,%.9g,%.9g) scale=%.9g opacity=%.9g parent=%d/%d visible=%u shadow=%u selected=%u links=%u/%u\n",
            label,key.frame,key.position[0],key.position[1],key.position[2],key.rotation[0],key.rotation[1],key.rotation[2],key.scale,key.opacity,key.parentModel,key.parentBone,unsigned(key.visible),unsigned(key.shadowEnabled),unsigned(key.selected),key.previous,key.next);
    };
    printKey("before save",expected);printKey("after reload",restored);
    std::printf("DIAG PMM opacity percent=%d packed=%u expectedReload=%.9g\n",opacityPercent,unsigned(packedOpacity),expectedPmmOpacity);std::fflush(stdout);
    Check(restored.frame==expected.frame&&!std::memcmp(restored.position,expected.position,sizeof(expected.position))&&
        !std::memcmp(restored.rotation,expected.rotation,sizeof(expected.rotation))&&restored.scale==expected.scale&&
        restored.parentModel==expected.parentModel&&restored.parentBone==expected.parentBone,
        "real PMM preserves imported frame full transform scale and parent exactly without original VMD");
    Check(restored.opacity==expectedPmmOpacity&&restored.visible==(packedOpacity&1)&&
        restored.shadowEnabled==(expected.shadowEnabled!=0)&&restored.selected==(expected.selected!=0),
        "real PMM opacity and flags match exact legacy packed-percentage encoding");
    std::puts("PASS real accessory VMD import PMM save and reload");
}
}
