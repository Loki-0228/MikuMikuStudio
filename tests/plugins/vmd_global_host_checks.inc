#include "mikumikustudio/clipboard_layout.hpp"
namespace vmd_global_host_checks {
struct File {
    std::wstring path;std::vector<unsigned char> bytes;
    template<class T> void Put(const T& value){const auto* p=reinterpret_cast<const unsigned char*>(&value);bytes.insert(bytes.end(),p,p+sizeof(value));}
    void Raw(const void* p,std::size_t count){const auto* first=static_cast<const unsigned char*>(p);bytes.insert(bytes.end(),first,first+count);}
    explicit File(int version) {
        path=std::filesystem::absolute(L"global-motion-\u6d4b\u8bd5-"+std::to_wstring(version)+L".vmd").native();
        char header[30]{};strcpy_s(header,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002");Raw(header,sizeof(header));
        char name[20]{};if(version==1)std::memcpy(name,"camera",6);else std::memcpy(name,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",13);Raw(name,version==1?10:20);
        Put(std::uint32_t(0));Put(std::uint32_t(0));Put(std::uint32_t(1));Put(std::uint32_t(0));Put(-44.f);
        const float eye[3]={1,12,3},angle[3]={.1f,.2f,.3f};Raw(eye,sizeof(eye));Raw(angle,sizeof(angle));
        if(version==1){const unsigned char curve[4]={20,107,20,107};Raw(curve,sizeof(curve));}
        else{for(unsigned lane=0;lane<6;++lane){Put(std::uint8_t(20));Put(std::uint8_t(107));Put(std::uint8_t(20));Put(std::uint8_t(107));}Put(std::int32_t(63));Put(std::uint8_t(1));}
        Put(std::uint32_t(1));Put(std::uint32_t(0));const float color[3]={.21f,.32f,.43f},direction[3]={0,-1,0};Raw(color,sizeof(color));Raw(direction,sizeof(direction));
        if(version==2){Put(std::uint32_t(1));Put(std::uint32_t(0));Put(std::uint8_t(2));Put(.053f);}Put(std::uint32_t(0));Write();
    }
    void Write(){FILE* file=nullptr;Check(!_wfopen_s(&file,path.c_str(),L"wb")&&file,"create real host Unicode VMD fixture");Check(std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size(),"write real host VMD fixture");Check(!std::fclose(file),"close real host VMD fixture");}
    ~File(){DeleteFileW(path.c_str());}
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    const auto frame=app->CurrentFrame();const auto camera=app->CameraMode();const auto slot=app->SelectedModelSlot();const auto mode=history.Mode();
    const auto last=app->LastRegisteredFrame();const auto initial=value();
    Check(app->ModelSlot(0)&&!history.UndoCount(),"actual VMD import begins in PMX or PMD fixture with empty history");
    for(int version:{1,2})for(auto recording:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        File file(version);Check(history.SetMode(recording)==MMS_OK,"configure actual VMD import recording mode");
        app->CameraMode()=1;app->SelectedModelSlot()=0;app->CurrentFrame()=643;
        const global_paste_host_checks::Keys before(*app);const auto beforeLast=app->LastRegisteredFrame();
        Menu(app,L"Edit probe once");LoadVmdFile(file.path.c_str());
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==643,"real Unicode VMD loader creates one explicit node between plugin edits");
        unsigned at=0;while(app->CameraKeys()[at].frame<643&&app->CameraKeys()[at].next)at=app->CameraKeys()[at].next;
        const auto& key=app->CameraKeys()[at];
        Check(key.frame==643&&key.fov==(version==1?45:63)&&key.parentModel==-1&&key.interpolation[2][5]==107&&key.eye[1]==12,
            "real VMD version sets expected field of view parent defaults and complete interpolation");
        const global_paste_host_checks::Keys after(*app);LoadVmdFile(file.path.c_str());
        Check(history.UndoCount()==2&&after.Equals(*app),"identical actual VMD import creates no duplicate history");
        Check(DeleteFileW(file.path.c_str()),"delete real VMD source before undo and redo");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(before.Equals(*app)&&app->LastRegisteredFrame()==beforeLast&&value()==initial+1,"real VMD undo restores all keys selection and last-frame metadata");
        app->CameraMode()=0;app->SelectedModelSlot()=1;app->CurrentFrame()=37;Menu(app,L"Edit probe redo");
        Check(after.Equals(*app)&&app->CurrentFrame()==37,"real VMD redo never rereads removed source after model and playhead changes");
        Menu(app,L"Edit probe redo");for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"real VMD interleaved journal returns to baseline");history.Collect();
    }
    app->CurrentFrame()=frame;app->CameraMode()=camera;app->SelectedModelSlot()=slot;app->LastRegisteredFrame()=last;
    Check(history.SetMode(mode)==MMS_OK,"restore actual VMD history mode");app->SceneModified()=0;
    std::puts("PASS actual Unicode VMD v1/v2 global import, explicit modes, deleted-source history and interleaved plugin edits");
}
}
