#include "../src/plugins/native_vmd_source.hpp"
#include "mikudancestudio/accessory_motion_io.hpp"
#include <filesystem>
namespace {
struct VmdGlobalFile {
    std::wstring path;std::vector<unsigned char> bytes;
    template<class T> void Put(const T& value){const auto* p=reinterpret_cast<const unsigned char*>(&value);bytes.insert(bytes.end(),p,p+sizeof(value));}
    void Raw(const void* p,std::size_t count){const auto* data=static_cast<const unsigned char*>(p);bytes.insert(bytes.end(),data,data+count);}
    VmdGlobalFile(GlobalPasteFixture& f,int version,bool unicode=false) {
        static unsigned serial=0;path=(std::filesystem::temp_directory_path()/(L"mms-vmd-global-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial)+(unicode?L"-\u6d4b\u8bd5":L"")+L".vmd")).native();
        char header[30]{};strcpy_s(header,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002");Raw(header,sizeof(header));
        char name[20]{};if(version==1)std::memcpy(name,"camera",6);else std::memcpy(name,"\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe",13);Raw(name,version==1?10:20);
        Put(std::uint32_t(0));Put(std::uint32_t(0));Put(std::uint32_t(f.cameras.size()));
        for(auto& source:f.cameras) {
            Put(source.frame);Put(source.distance);Raw(source.eye,sizeof(source.eye));Raw(source.target,sizeof(source.target));
            if(version==1) {
                const unsigned char shared[4]={17,97,31,111};Raw(shared,sizeof(shared));
                for(unsigned lane=0;lane<6;++lane){source.interpolation[lane]=shared[0];source.interpolation[lane+6]=shared[2];source.interpolation[lane+12]=shared[1];source.interpolation[lane+18]=shared[3];}
                source.fov=45;source.perspective=0;
            } else {
                for(unsigned lane=0;lane<6;++lane){Put(source.interpolation[lane]);Put(source.interpolation[lane+12]);Put(source.interpolation[lane+6]);Put(source.interpolation[lane+18]);}
                Put(source.fov);Put(source.perspective);
            }
            source.parentModel=-1;source.parentBone=0;
        }
        Put(std::uint32_t(f.lights.size()));for(const auto& source:f.lights){Put(source.frame);Raw(source.color,sizeof(source.color));Raw(source.direction,sizeof(source.direction));}
        if(version==2){Put(std::uint32_t(f.shadows.size()));for(const auto& source:f.shadows){Put(source.frame);Put(source.mode);Put(source.distance);}}
        else f.shadows.clear();
        Put(std::uint32_t(0));Write();
    }
    ~VmdGlobalFile(){DeleteFileW(path.c_str());}
    void Write(){FILE* file=nullptr;Check(!_wfopen_s(&file,path.c_str(),L"wb")&&file,"open private VMD fixture");Check(std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size(),"write complete VMD fixture");Check(!std::fclose(file),"close VMD fixture");}
    void AccessoryTail(){accessory_motion_io::Track track;track.name="test";track.keys.emplace_back();const auto tail=accessory_motion_io::Encode({track});bytes.insert(bytes.end(),tail.begin(),tail.end());Write();}
    std::string Ansi()const{return std::filesystem::path(path).string();}
};
void VmdGlobalOracle(GlobalPasteFixture& f) {
    ClearGlobalFrameSelection(f.app.get());
    for(const auto& record:f.cameras)Check(RegisterCameraKey(f.app.get(),&record),"legacy camera registrar accepts independently encoded VMD value");
    for(const auto& record:f.lights)Check(RegisterLightKey(f.app.get(),&record),"legacy light registrar accepts independently encoded VMD value");
    for(const auto& record:f.shadows)Check(RegisterSelfShadowKey(f.app.get(),&record),"legacy shadow registrar accepts independently encoded VMD value");
}
void VmdGlobalRoundTripChecks() {
    for(int version:{1,2})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly})for(bool wide:{false,true}) {
        std::printf("  VMD global version=%d mode=%u wide=%u\n",version,unsigned(mode),unsigned(wide));std::fflush(stdout);
        GlobalPasteFixture f,oracle;VmdGlobalFile input(f,version,wide);oracle.cameras=f.cameras;oracle.lights=f.lights;oracle.shadows=f.shadows;
        f.ClearClipboard();Check(f.history.SetMode(mode)==MMS_OK,"configure VMD explicit import mode");
        const GlobalPasteKeys before(*f.app);const auto last=f.app->LastRegisteredFrame();const auto digest=f.Digest();
        std::shared_ptr<plugins::HistoryChange> change;const auto ansi=wide?std::string{}:input.Ansi();
        Check(plugins::PlanGlobalVmdImport(*f.app,wide?nullptr:ansi.c_str(),wide?input.path.c_str():nullptr,change)==MMS_OK&&change&&f.Digest()==digest,"VMD bytes decode and plan without modifying live state");
        change.reset();
        const auto previous=g_Block;
        {plugins::NativePoseTestBinding binding(*f.app,f.history);g_Block=f.app.get();
            if(wide)LoadVmdFile(input.path.c_str());else Check(LoadVmdMotion(f.app.get(),ansi.c_str())==0,"actual ANSI VMD loader keeps legacy return convention");}
        g_Block=previous;
        VmdGlobalOracle(oracle);
        Check(f.history.UndoCount()==1&&GlobalRegistrationSame(f.app->CameraKeys(),GlobalRegistrationKeys(oracle.app->CameraKeys()))&&
            GlobalRegistrationSame(f.app->LightKeys(),GlobalRegistrationKeys(oracle.app->LightKeys()))&&GlobalRegistrationSame(f.app->ShadowKeys(),GlobalRegistrationKeys(oracle.app->ShadowKeys()))&&
            f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame(),"actual VMD loader imports camera curves parent defaults lights shadows and metadata as one explicit node");
        for(unsigned slot=0;slot<255;++slot)for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.app->AccessoryKeys(slot));++i)Check(!f.app->AccessoryKeys(slot)[i].selected,"VMD import clears all accessory key marks");
        const GlobalPasteKeys after(*f.app);Check(DeleteFileW(input.path.c_str()),"remove VMD source before history traversal");
        f.app->CurrentFrame()=23;f.app->CameraMode()=0;f.app->SelectedModelSlot()=99;
        Check(f.history.History(false)==MMS_OK&&before.Same(*f.app)&&f.app->LastRegisteredFrame()==last,"VMD undo restores every pre-import key and selection without a source file");
        Check(f.history.History(true)==MMS_OK&&after.Same(*f.app)&&f.app->CurrentFrame()==23,"VMD redo owns source data across current frame and editing-mode changes");
    }
}
void VmdGlobalFailureChecks() {
    std::puts("  VMD global unsupported tail malformed source and allocation failures");std::fflush(stdout);
    GlobalPasteFixture f;VmdGlobalFile input(f,2,true);f.ClearClipboard();const auto before=f.Digest();const GlobalPasteKeys keys(*f.app);
    std::shared_ptr<plugins::HistoryChange> change;input.AccessoryTail();
    Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,input.path.c_str(),change)!=MMS_OK&&!change&&f.Digest()==before&&keys.Same(*f.app),"valid nonempty accessory extension refuses whole VMD import without standard partial changes");
    GlobalPasteFixture source;VmdGlobalFile motion(source,2,true);
    const auto full=motion.bytes;motion.bytes.pop_back();motion.Write();
    Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,motion.path.c_str(),change)!=MMS_OK&&!change&&f.Digest()==before,"truncated optional section refuses global VMD atomically");
    motion.bytes=full;motion.Write();
    bool success=false;unsigned failures=0;
    for(long allowance=0;allowance<4096;++allowance){failAfter=allowance;const auto result=plugins::PlanGlobalVmdImport(*f.app,nullptr,motion.path.c_str(),change);failAfter=-1;
        if(result==MMS_OK){success=bool(change);break;}++failures;Check(!change&&f.Digest()==before&&keys.Same(*f.app),"VMD decode and multi-track planning allocation failure preserves all live state");}
    Check(success&&failures>10,"VMD allocation sweep reaches complete private plan");
    plugins::SharedHistory budgetHistory(f.storage,{},32,1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(budgetHistory.PrepareNative("VMD budget",change,token)==MMS_E_MEMORY&&!token&&f.Digest()==before,"VMD retained data must fit history budget before publishing tracks");
    Check(f.history.PrepareNative("VMD stale",change,token)==MMS_OK,"prepare VMD candidate before source-state change");++f.app->LastRegisteredFrame();const auto changed=f.Digest();
    Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&f.Digest()==changed&&!f.history.UndoCount(),"VMD candidate refuses changed metadata without modifying other tracks");
    f.app->CameraMode()=1;const auto stable=f.Digest();
    Check(plugins::PlanGlobalVmdImport(*f.app,nullptr,L"?:\\mms-missing-vmd-source.vmd",change)!=MMS_OK&&!change&&f.Digest()==stable,"camera VMD route reports missing-file failure without legacy fallback");
}
void NativeVmdGlobalChecks(){VmdGlobalRoundTripChecks();VmdGlobalFailureChecks();}
}
