// Include after native_pose_scope_checks.inc and native_camera_multiply_checks.inc.
// This group uses a real system DirectSound device, hidden windows and an
// installation-local WAV fixture. It does not initialize a graphics device.
#include "../src/plugins/native_audio_history.hpp"
#include "mikumikustudio/wave_audio_asset.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/audio_menu.hpp"
#include <MinHook.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <chrono>
namespace mikumikustudio {void CmdOpenWave(MMDApp*);}
namespace native_audio_checks {
using namespace plugins;
namespace fs=std::filesystem;
struct Files {
    fs::path directory,a,b,invalid;
    Files(){static unsigned ordinal=0;directory=local_data::File((L"audio-history-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++ordinal)).c_str());
        fs::create_directories(directory);a=directory/L"first.wav";b=directory/L"second.wav";invalid=directory/L"missing-data.wav";
        Wave(a,8000,1,16,8000,23);Wave(b,11025,2,8,11025,61);
        std::ofstream bad(invalid,std::ios::binary);bad.write("RIFF",4);const std::uint32_t size=4;bad.write(reinterpret_cast<const char*>(&size),4);bad.write("WAVE",4);
    }
    static void Wave(const fs::path& path,unsigned rate,unsigned channels,unsigned bits,unsigned frames,unsigned seed){
        std::ofstream file(path,std::ios::binary);const auto align=channels*(bits/8),bytes=frames*align,pad=bytes&1u;
        const auto u16=[&](unsigned value){const auto n=std::uint16_t(value);file.write(reinterpret_cast<const char*>(&n),2);};
        const auto u32=[&](unsigned value){const auto n=std::uint32_t(value);file.write(reinterpret_cast<const char*>(&n),4);};
        file.write("RIFF",4);u32(bytes+36+pad);file.write("WAVEfmt ",8);u32(16);u16(1);u16(channels);u32(rate);u32(rate*align);u16(align);u16(bits);
        file.write("data",4);u32(bytes);for(unsigned index=0;index<bytes;++index){const unsigned char sample=static_cast<unsigned char>((index*37+seed)&255u);file.write(reinterpret_cast<const char*>(&sample),1);}
        if(pad)file.put(0);Check(bool(file),"write real PCM audio fixture");
    }
    ~Files(){std::error_code ignored;fs::remove_all(directory,ignored);}
};
struct Scene:PoseNumericFixture {
    WaveAudioContext audio{};HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previousBitmap=nullptr;
    Scene(){app->Audio()=&audio;audio.mainWindow=app->MainWindow();audio.englishUI=1;audio.volume=-10000;app->EnglishUI()=1;
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);const auto folder=fs::path(executable).parent_path().native();wcscpy_s(app->ExeDir(),256,folder.c_str());
        app->SidebarWidth()=300;dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=360;
        info.bmiHeader.biHeight=-64;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;void* pixels=nullptr;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);Check(dc&&bitmap,"create hidden real waveform drawing surface");previousBitmap=SelectObject(dc,bitmap);
        app->TimelineDC()=audio.timelineDC=dc;
    }
    ~Scene(){history.Reset();history.Collect();ClearWaveAudio(app.get());app->Audio()=nullptr;app->TimelineDC()=nullptr;
        SelectObject(dc,previousBitmap);DeleteObject(bitmap);DeleteDC(dc);}
    void Import(const fs::path& path){bool changed=false;Check(ImportBoundAudio(*app,path.c_str(),changed)==MMS_OK&&changed,"import real PCM through common history");}
    std::uint64_t Digest()const{
        std::uint64_t value=14695981039346656037ull;const auto add=[&](const void* raw,std::size_t bytes){auto* p=static_cast<const unsigned char*>(raw);while(bytes--){value^=*p++;value*=1099511628211ull;}};
        add(&audio,sizeof audio);add(app->WavePath(),256*sizeof(wchar_t));add(&app->WaveEnabled(),1);add(&app->DirectSoundAvailable(),1);
        if(audio.waveformColumns){add(audio.waveformMax,audio.waveformColumns);add(audio.waveformMin,audio.waveformColumns);}return value;
    }
    std::shared_ptr<HistoryChange> Plan(const fs::path& path){NativeAudioPlan plan;std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app)==MMS_OK&&plan.Read(path.c_str())==MMS_OK&&plan.Take(change)==MMS_OK&&change,"capture private audio replacement");return change;}
    void Commit(const std::shared_ptr<HistoryChange>& change){std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("audio",change,transaction,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"prepare complete audio publication");
        failAfter=0;const auto status=history.CommitNative(std::move(transaction));failAfter=-1;Check(status==MMS_OK,"audio file waveform cache and sound buffer publish without allocation");}
    void Playback(){Check(audio.streamingBuffer&&audio.fileHandle&&fs::exists(audio.path),"restored clip owns an actual playable private PCM file and streaming buffer");
        WaveSeekAndFeed(&audio,0);Check(audio.feedThread!=0,"restored clip starts the real DirectSound feed worker after original removal");
        Sleep(35);CloseDataFile(&audio);Check(!audio.feedThread&&WaveStopState(&audio)==2,"restored PCM feed stops and joins normally");}
};
void RetainedPlaybackAndChronology(){
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Files files;Scene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"select audio history recording mode");
        const auto modelDigest=f.first.Digest(),other=f.second.Digest();f.Import(files.a);auto first=wave_audio::Active(&f.audio);
        const auto firstMax=std::vector<unsigned char>(f.audio.waveformMax,f.audio.waveformMax+f.audio.waveformColumns);
        const auto firstMin=std::vector<unsigned char>(f.audio.waveformMin,f.audio.waveformMin+f.audio.waveformColumns);
        Check(first&&f.app->WaveEnabled()&&f.app->DirectSoundAvailable()&&media::AudioSource(&f.audio)==files.a.native()&&
            f.audio.timelineDC==f.app->TimelineDC()&&f.audio.format.nSamplesPerSec==8000,"first import publishes source name flags format and waveform binding");
        f.Import(files.b);auto second=wave_audio::Active(&f.audio);const auto secondPath=std::wstring(f.audio.path);
        Check(second&&second!=first&&f.history.UndoCount()==2&&!f.app->PhysicsResetPending(),"replacement adds one explicit node without resetting any model physics");
        Check(TryClearBoundAudio(f.app.get())&&!f.app->WaveEnabled()&&!f.app->WavePath()[0]&&!f.audio.streamingBuffer&&
            !f.audio.fileHandle&&!f.audio.waveformColumns&&f.history.UndoCount()==3,"real audio-clear command atomically removes clip resources and records one node");
        fs::rename(files.a,files.a.native()+L".moved");fs::rename(files.b,files.b.native()+L".moved");
        f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=37;
        Check(f.history.History(false)==MMS_OK&&wave_audio::Active(&f.audio)==second&&!std::wcscmp(f.app->WavePath(),files.b.c_str()),"undo clear restores retained replacement without its source file");f.Playback();
        Check(f.history.History(false)==MMS_OK&&wave_audio::Active(&f.audio)==first&&f.audio.waveformColumns==int(firstMax.size())&&
            !std::memcmp(f.audio.waveformMax,firstMax.data(),firstMax.size())&&!std::memcmp(f.audio.waveformMin,firstMin.data(),firstMin.size()),
            "undo replacement restores exact first PCM waveforms independently of source files");f.Playback();
        Check(f.history.History(false)==MMS_OK&&!f.app->WaveEnabled()&&!f.app->WavePath()[0]&&f.history.History(true)==MMS_OK&&wave_audio::Active(&f.audio)==first,
            "undo original import then redo recreates the first retained clip");f.Playback();
        Check(f.history.History(true)==MMS_OK&&wave_audio::Active(&f.audio)==second&&f.history.History(true)==MMS_OK&&!f.app->WaveEnabled(),"redo replacement and deletion reuse immutable recorded PCM");
        Check(f.first.Digest()==modelDigest&&f.second.Digest()==other&&f.app->CurrentFrame()==37,"audio history across selected models does not edit poses or move the playhead");
        Check(f.history.History(false)==MMS_OK,"restore current replacement for history-eviction lifetime check");
        first.reset();second.reset();Check(f.history.Reset()==MMS_OK,"drop all audio history nodes while a replacement stays active");f.history.Collect();
        Check(fs::exists(secondPath)&&!std::wcscmp(f.audio.path,secondPath.c_str()),"active audio lease survives removal of every history record");f.Playback();
    }
}
void LegacyCaptureAndWaveformParity(){
    const auto stage=[](const char* phase){std::printf("    audio legacy: %s\n",phase);std::fflush(stdout);};
    stage("fixture");
    Files files;Scene f;const auto path=files.a.native();
    stage("native loader and device");
    Check(WaveLoadFile(&f.audio,path.c_str(),f.app->PathWorkspace())&&WaveStartPlayback(&f.audio),"load a real pre-migration WAV with the existing native loader");
    wcscpy_s(f.app->WavePath(),256,path.c_str());f.app->WaveEnabled()=f.app->DirectSoundAvailable()=1;
    const auto maximum=std::vector<unsigned char>(f.audio.waveformMax,f.audio.waveformMax+f.audio.waveformColumns),minimum=std::vector<unsigned char>(f.audio.waveformMin,f.audio.waveformMin+f.audio.waveformColumns);
    LARGE_INTEGER zero{},before{},after{};Check(SetFilePointerEx(f.audio.fileHandle,zero,&before,FILE_CURRENT)!=FALSE,"capture original legacy read handle position");
    stage("decode immutable source");auto decoded=wave_audio::Asset::Decode(path.c_str(),256ull*1024*1024);
    stage("prepare device snapshot");
    {wave_audio::Prepared prepared(f.audio,decoded);Check(prepared.IsCurrent(),"independently prepare retained PCM using a legacy device");stage("publish device snapshot");prepared.Publish();stage("retire old device snapshot");}
    stage("compare native waveform");
    Check(f.audio.waveformColumns==int(maximum.size())&&!std::memcmp(f.audio.waveformMax,maximum.data(),maximum.size())&&
        !std::memcmp(f.audio.waveformMin,minimum.data(),minimum.size()),"checked PCM waveform exactly matches actual legacy float and signed-byte sampling");
    // A second legacy context exercises the independent-handle capture, before
    // the active lease shortcut can apply.
    stage("independent legacy loader");WaveAudioContext legacy{};legacy.mainWindow=f.app->MainWindow();legacy.englishUI=1;
    Check(WaveLoadFile(&legacy,path.c_str(),f.app->PathWorkspace()),"load a separate legacy source for atomic snapshot capture");
    Check(SetFilePointerEx(legacy.fileHandle,zero,&before,FILE_CURRENT)!=FALSE,"read legacy source cursor before snapshot");
    stage("capture independent legacy bytes");auto snapshot=wave_audio::Asset::Capture(legacy,path.c_str(),256ull*1024*1024);
    Check(snapshot&&SetFilePointerEx(legacy.fileHandle,zero,&after,FILE_CURRENT)&&before.QuadPart==after.QuadPart,"capturing legacy PCM never moves the live file cursor");
    stage("reset independent legacy context");WaveCtxReset(&legacy);std::free(legacy.waveformMax);std::free(legacy.waveformMin);
    stage("bound clear after legacy snapshot");
    NativePoseTestBinding binding(*f.app,f.history);Check(TryClearBoundAudio(f.app.get()),"clear a real legacy-origin clip through common history");
    stage("remove external legacy source");decoded.reset();snapshot.reset();fs::rename(files.a,files.a.native()+L".gone");
    stage("undo legacy clear");
    Check(f.history.History(false)==MMS_OK&&f.audio.waveformColumns==int(maximum.size())&&!std::memcmp(f.audio.waveformMax,maximum.data(),maximum.size()),
        "undo legacy-origin clear restores its captured samples after source removal");stage("play legacy snapshot");f.Playback();stage("legacy cleanup");
}
const wchar_t* selection=nullptr;
unsigned picks=0;
BOOL SelectAudio(LPOPENFILENAMEW picker){++picks;if(!selection)return FALSE;wcscpy_s(picker->lpstrFile,picker->nMaxFile,selection);return TRUE;}
struct PickerHook {
    PickerHook(){const auto initialized=MH_Initialize();Check(initialized==MH_OK||initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize actual audio-picker interception");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetUiOpenFileNameW),reinterpret_cast<void*>(&SelectAudio),nullptr)==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW))==MH_OK,"intercept actual production file picker only");}
    ~PickerHook(){MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));selection=nullptr;}
};
void ActualMenuAndNoop(){
    PickerHook picker;
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Files files;Scene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"set real audio menu mode");
        selection=nullptr;const auto empty=f.Digest();CmdOpenWave(f.app.get());Check(f.Digest()==empty&&!f.history.UndoCount(),"actual menu206 cancellation preserves the empty audio state");
        const auto source=files.a.native();selection=source.c_str();CmdOpenWave(f.app.get());
        Check(f.history.UndoCount()==1&&f.app->SceneModified()&&f.audio.streamingBuffer&&!wave_audio::NeedsRefresh(&f.audio),"actual menu206 creates one explicit node and refreshes the actual waveform DC");
        Check(f.history.History(false)==MMS_OK,"seed real audio redo by undoing menu import");const auto redo=f.history.RedoCount();selection=nullptr;CmdOpenWave(f.app.get());
        Check(f.history.RedoCount()==redo&&!f.history.UndoCount()&&!f.app->WaveEnabled(),"cancelled menu206 preserves the existing redo branch");
        Check(f.history.History(true)==MMS_OK,"restore clip before source-identity no-op");const auto current=f.Digest();selection=source.c_str();CmdOpenWave(f.app.get());
        Check(f.Digest()==current&&f.history.UndoCount()==1,"loading identical PCM and source metadata is a true menu no-op");
        Check(TryClearBoundAudio(f.app.get())&&f.history.History(false)==MMS_OK,"seed deletion redo");const auto redoClear=f.history.RedoCount();
        bool changed=true;Check(ImportBoundAudio(*f.app,source.c_str(),changed)==MMS_OK&&!changed&&f.history.RedoCount()==redoClear,"identity replacement preserves an unrelated existing redo entry");
    }
}
void RemoveMenuAndDrop(){
    Files files;
    for(bool bound:{false,true}){
        Scene f;std::unique_ptr<NativePoseTestBinding> binding;if(bound)binding=std::make_unique<NativePoseTestBinding>(*f.app,f.history);
        const auto menu=CreateMenu(),file=CreatePopupMenu();Check(menu&&file&&AppendMenuW(file,MF_STRING,206,L"Open audio")&&AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"File")&&
            SetMenu(f.app->MainWindow(),menu),"build an actual File popup identified by original206 command");
        audio_menu::EnsureItem(f.app.get());Check(GetMenuItemID(file,1)==audio_menu::kRemoveCommand&&
            (GetMenuState(file,audio_menu::kRemoveCommand,MF_BYCOMMAND)&MF_GRAYED),"remove-audio menu appears after206 and starts disabled");
        for(unsigned language:{0u,1u,2u}){f.app->EnglishUI()=std::uint8_t(language);audio_menu::EnsureItem(f.app.get());wchar_t text[80]{};
            Check(GetMenuStringW(file,audio_menu::kRemoveCommand,text,80,MF_BYCOMMAND)>0&&!std::wcscmp(text,language==0?L"音声を削除":language==1?L"Remove audio":L"移除音频"),"remove-audio menu follows all three native UI languages");}
        f.app->EnglishUI()=1;
        if(bound){
            const auto text=files.a.native();const auto bytes=sizeof(DROPFILES)+(text.size()+2)*sizeof(wchar_t);const auto memory=GlobalAlloc(GHND,bytes);
            Check(memory!=nullptr,"allocate actual Unicode audio drop payload");auto* payload=static_cast<DROPFILES*>(GlobalLock(memory));payload->pFiles=sizeof(DROPFILES);payload->fWide=TRUE;
            std::memcpy(reinterpret_cast<unsigned char*>(payload)+sizeof(DROPFILES),text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);HandleDropFiles(static_cast<HDROP>(memory));
            Check(f.history.UndoCount()==1&&f.app->WaveEnabled()&&!std::wcscmp(f.app->WavePath(),text.c_str()),"real file-drop handler imports one common audio node without prewriting the live source path");
        }else{
            Check(WaveLoadFile(&f.audio,files.a.c_str(),f.app->PathWorkspace())&&WaveStartPlayback(&f.audio),"prepare a real unbound legacy audio clip");
            wcscpy_s(f.app->WavePath(),256,files.a.c_str());f.app->WaveEnabled()=f.app->DirectSoundAvailable()=1;
        }
        audio_menu::EnsureItem(f.app.get());Check(!(GetMenuState(file,audio_menu::kRemoveCommand,MF_BYCOMMAND)&MF_GRAYED),"loaded audio enables the actual remove command");
        const auto nodeCount=f.history.UndoCount();CommandDispatch(nullptr,MAKEWPARAM(audio_menu::kRemoveCommand,0));
        Check(!f.app->WaveEnabled()&&!f.app->WavePath()[0]&&!f.audio.waveformMax&&!f.audio.waveformMin&&!f.audio.fileHandle&&!f.audio.streamingBuffer&&
            (GetMenuState(file,audio_menu::kRemoveCommand,MF_BYCOMMAND)&MF_GRAYED)&&f.history.UndoCount()==nodeCount+(bound?1:0),
            "actual32815 dispatch clears real resources in both bound common-history and unbound legacy modes");
        if(bound){Check(f.history.History(false)==MMS_OK&&f.app->WaveEnabled(),"remove command remains undoable through the common cursor");
            const auto active=wave_audio::Active(&f.audio);const auto count=f.history.UndoCount();const auto mute=f.app->state.automaticFrameAdvanceEnabled;
            CommandDispatch(nullptr,MAKEWPARAM(297,0));Check(wave_audio::Active(&f.audio)==active&&f.app->WaveEnabled()&&f.history.UndoCount()==count&&f.app->state.automaticFrameAdvanceEnabled!=mute,
                "original297 keeps its mute toggle semantics and does not remove or register an audio resource");}
        SetMenu(f.app->MainWindow(),nullptr);DestroyMenu(menu);
    }
}
using BufferCreate=HRESULT(WINAPI*)(IDirectSound*,LPCDSBUFFERDESC,LPDIRECTSOUNDBUFFER*,LPUNKNOWN);
HRESULT WINAPI RefuseBuffer(IDirectSound*,LPCDSBUFFERDESC,LPDIRECTSOUNDBUFFER* out,LPUNKNOWN){*out=nullptr;return DSERR_OUTOFMEMORY;}
void FailuresAndBudget(){
    Files files;Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Import(files.a);Check(f.history.Reset()==MMS_OK,"keep an active clip while clearing the failure-test journal");f.history.Collect();const auto before=f.Digest();
    {NativeAudioPlan invalid;Check(invalid.Begin(*f.app)==MMS_OK&&invalid.Read(files.invalid.c_str())==MMS_E_ARGUMENT&&f.Digest()==before,"missing-data RIFF is rejected with a bounded scan and no live teardown");}
    {auto change=f.Plan(files.b);const auto retained=change->RetainedBytes();Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,128,retained-1)==MMS_OK,"set an audio payload budget below the retained PCM size");
        std::unique_ptr<SharedHistory::NativeTransaction> token;Check(f.history.PrepareNative("budget",change,token)==MMS_E_MEMORY&&!token&&f.Digest()==before,"actual PCM/cache/waveform budget refusal preserves old playback resources");}
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore audio budget");
    bool success=false;unsigned failures=0;
    for(long limit=0;limit<1024&&!success;++limit){NativeAudioPlan plan;failAfter=limit;auto result=plan.Begin(*f.app);if(result==MMS_OK)result=plan.Read(files.b.c_str());
        std::shared_ptr<HistoryChange> change;if(result==MMS_OK)result=plan.Take(change);failAfter=-1;
        Check(f.Digest()==before,"PCM capture decode cache and private planning OOM never changes the old audio context");
        if(result==MMS_OK){Check(bool(change),"audio planning fault sweep reaches a substantive retained snapshot");success=true;}else{++failures;Check(result==MMS_E_MEMORY,"audio plan OOM reports memory failure");}}
    Check(success&&failures>5,"sweep retained audio allocations to successful completion");
    auto change=f.Plan(files.b);std::unique_ptr<SharedHistory::NativeTransaction> token;success=false;failures=0;
    for(long limit=0;limit<256&&!success;++limit){failAfter=limit;const auto result=f.history.PrepareNative("audio candidate",change,token);failAfter=-1;
        Check(f.Digest()==before,"partial candidate cleanup retains old DS file and waveform resources");
        if(result==MMS_OK)success=true;else{++failures;Check(result==MMS_E_MEMORY&&!token,"candidate OOM releases common-history reservation");}}
    Check(success&&failures>2,"sweep prepared audio resources to success");token.reset();
    const auto mh=MH_Initialize();Check(mh==MH_OK||mh==MH_ERROR_ALREADY_INITIALIZED,"initialize audio-resource failure interception");
    auto* create=(*reinterpret_cast<void***>(f.audio.directSound))[3];
    Check(MH_CreateHook(create,reinterpret_cast<void*>(&RefuseBuffer),nullptr)==MH_OK&&MH_EnableHook(create)==MH_OK,"inject real DirectSound buffer creation refusal");
    const auto refused=f.history.PrepareNative("DS failure",change,token);MH_DisableHook(create);MH_RemoveHook(create);
    Check(refused!=MMS_OK&&!token&&f.Digest()==before,"failed DirectSound preparation cannot replace the old clip or publish history");
    f.Commit(change);const auto after=f.Digest();failAfter=0;const auto undo=f.history.History(false);failAfter=-1;
    Check(undo==MMS_E_MEMORY&&f.Digest()==after&&f.history.UndoCount()==1,"undo OOM retains the complete active replacement and undo cursor");
    Check(f.history.History(false)==MMS_OK,"retry audio undo after allocation failure");const auto undone=f.Digest();failAfter=0;const auto redo=f.history.History(true);failAfter=-1;
    Check(redo==MMS_E_MEMORY&&f.Digest()==undone&&f.history.RedoCount()==1,"redo OOM preserves restored PCM and redo cursor");
    Check(f.history.History(true)==MMS_OK,"retry audio redo");
    WaveSeekAndFeed(&f.audio,0);NativeAudioPlan busy;Check(busy.Begin(*f.app)==MMS_E_BUSY,"active feed is refused without prematurely stopping the old clip");CloseDataFile(&f.audio);
    NativeAudioPlan stale;Check(stale.Begin(*f.app)==MMS_OK,"capture audio picker before binding replacement");{NativePoseTestBinding nested(*f.app,f.history);}
    Check(stale.Read(files.a.c_str())==MMS_E_STALE,"recycled common-history binding invalidates a modal audio source");
}
void PluginComposition(){
    Files files;Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Import(files.a);auto first=wave_audio::Active(&f.audio);auto change=f.Plan(files.b);
    camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.audio-history";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register actual audio/plugin history participant");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);MmsHandle transaction=0;std::uint64_t revision=0;
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("audio and plugin"),revision,&transaction)==MMS_OK,"begin real mixed audio/plugin transaction");
    NativeAudioPlan busy;Check(busy.Begin(*f.app)==MMS_E_BUSY,"plugin draft excludes a competing audio import");auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK&&
        api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&f.history.UndoCount()==2,"plugin state and retained audio publish as one common-history node");
    auto second=wave_audio::Active(&f.audio);fs::rename(files.a,files.a.native()+L".moved");fs::rename(files.b,files.b.native()+L".moved");
    Check(api.undo(api.context)==MMS_OK&&plugin.value==0&&wave_audio::Active(&f.audio)==first&&api.redo(api.context)==MMS_OK&&plugin.value==42&&wave_audio::Active(&f.audio)==second,
        "mixed plugin undo redo uses retained PCM after original files disappear");
    Check(f.history.Reset()==MMS_OK,"retire mixed audio records before unloading serializer");f.history.Collect();Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister mixed audio serializer");
}
}
void NativeAudioHistoryChecks(){
    std::puts("  audio: retained PCM playback and cross-model chronology");native_audio_checks::RetainedPlaybackAndChronology();
    std::puts("  audio: original-loader waveform parity and legacy capture");native_audio_checks::LegacyCaptureAndWaveformParity();
    std::puts("  audio: actual menu cancellation no-op and repaint");native_audio_checks::ActualMenuAndNoop();
    std::puts("  audio: actual remove32815 menu and Unicode drop");native_audio_checks::RemoveMenuAndDrop();
    std::puts("  audio: memory resource budget and stale-source refusal");native_audio_checks::FailuresAndBudget();
    std::puts("  audio: actual plugin composition");native_audio_checks::PluginComposition();
}
// Explicit fixture-dependent entry. The ordinary group above remains fully
// self-contained and must not silently skip unavailable compressed codecs.
void NativeCompressedAudioHistoryChecks(const wchar_t* fixtureDirectory){
    using namespace native_audio_checks;
    Check(fixtureDirectory&&*fixtureDirectory,"actual compressed audio fixture directory supplied");
    const auto fixtures=fs::absolute(fixtureDirectory);
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})
        for(const auto* extension:{L"mp3",L"aac",L"m4a",L"wma"}){
            Files files;Scene f;NativePoseTestBinding binding(*f.app,f.history);
            Check(f.history.SetMode(mode)==MMS_OK,"select actual compressed audio recording mode");
            const auto name=std::wstring(L"tone.")+extension;
            const auto source=files.directory/name,limited=files.directory/(std::wstring(L"limited.")+extension);
            Check(fs::copy_file(fixtures/name,source)&&fs::copy_file(fixtures/name,limited),"copy real compressed codec sources into the isolated audio fixture");
            f.Import(source);const auto retained=wave_audio::Active(&f.audio);const auto bytes=f.audio.dataSize;
            const auto maximum=std::vector<unsigned char>(f.audio.waveformMax,f.audio.waveformMax+f.audio.waveformColumns);
            const auto minimum=std::vector<unsigned char>(f.audio.waveformMin,f.audio.waveformMin+f.audio.waveformColumns);
            Check(retained&&bytes>0&&f.audio.format.wFormatTag==WAVE_FORMAT_PCM&&f.audio.format.wBitsPerSample==16&&
                !std::wcscmp(media::AudioSource(&f.audio),source.c_str())&&f.history.UndoCount()==1,
                "each actual compressed codec publishes retained PCM16 source identity and one explicit node");
            Check(TryClearBoundAudio(f.app.get())&&!f.app->WaveEnabled()&&f.history.UndoCount()==2,"clear actual compressed audio as one additional common-history node");
            fs::rename(source,source.native()+L".moved");
            Check(!fs::exists(source)&&f.history.History(false)==MMS_OK&&wave_audio::Active(&f.audio)==retained&&f.audio.dataSize==bytes&&
                f.audio.waveformColumns==int(maximum.size())&&!std::memcmp(f.audio.waveformMax,maximum.data(),maximum.size())&&
                !std::memcmp(f.audio.waveformMin,minimum.data(),minimum.size())&&!std::wcscmp(f.app->WavePath(),source.c_str()),
                "undo restores exact compressed-source PCM and waveform after its original path is removed");
            f.Playback();
            Check(f.history.History(true)==MMS_OK&&!f.app->WaveEnabled()&&f.history.History(false)==MMS_OK&&wave_audio::Active(&f.audio)==retained,
                "compressed audio redo clears and another undo recreates the same retained clip without decoding its source again");
            Check(f.history.Configure(mode,128,1024)==MMS_OK,"set a small retained audio budget while a decoded clip remains active");
            const auto before=f.Digest();bool changed=true;
            Check(ImportBoundAudio(*f.app,limited.c_str(),changed)==MMS_E_MEMORY&&!changed&&f.Digest()==before&&wave_audio::Active(&f.audio)==retained,
                "each actual compressed codec aborts at the byte cap and leaves the previous PCM device buffers waveform and source identity intact");
            std::wprintf(L"  compressed audio history: %s mode=%u\n",extension,unsigned(mode));
        }
}
