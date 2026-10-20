// Include after native_morph_category_checks.inc and native_camera_multiply_checks.inc.
#include "../src/plugins/native_morph_tools.hpp"
#include "../src/plugins/edit_store.hpp"
#include <functional>
#include <climits>
namespace mikumikustudio { void CmdFileMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace native_morph_tools_checks {
using namespace plugins;
using Tool=NativeMorphTool;
struct Scene:MorphCurveWriteFixture {
    Scene(){app->HInstance()=GetModuleHandleW(nullptr);app->EnglishUI()=1;app->CameraMode()=0;app->state.optflag[0]=0;app->PhysicsResetPending()=0;
        app->state.blinkPhase=0;app->LastRegisteredFrame()=93;
        // This fixture edits morph zero as a mouth track. PMD reserves it as
        // the base morph; use PMX mode so actual native Seek evaluates it.
        first.m->physicsMode=second.m->physicsMode=2;
        for(auto* table:{&firstMorphs,&secondMorphs}){(*table)[0].panel=mdl::MorphPanel::mouth;(*table)[1].panel=mdl::MorphPanel::eye;(*table)[2].panel=mdl::MorphPanel::other;
            std::memcpy((*table)[1].name,"\x82\xdc\x82\xce\x82\xbd\x82\xab",9);}
    }
    std::shared_ptr<HistoryChange> Shift(int delta){NativeMorphToolsPlan plan;std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app,Tool::MouthShift)==MMS_OK&&plan.ReadShift(delta)==MMS_OK&&plan.Take(change)==MMS_OK,"capture and read private lip shift");return change;}
    std::shared_ptr<HistoryChange> Blink(int start,int end,int(*random)()=nullptr,unsigned* count=nullptr){NativeMorphToolsPlan plan;std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app,Tool::RandomBlink)==MMS_OK&&plan.ReadBlink(start,end,random)==MMS_OK&&plan.Take(change)==MMS_OK,"generate one private random blink sequence");
        if(count)*count=plan.BlinkCount();return change;}
    void CommitTool(const std::shared_ptr<HistoryChange>& change){Check(bool(change),"morph tool has an actual change");std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(history.PrepareNative("native morph tool",change,token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"prepare one explicit morph tool node");
        Check(CommitRegistrationNoAllocation(history,std::move(token))==MMS_OK,"morph tool keys layers weights and metadata publish without allocation");}
    std::vector<std::uint8_t> Authors(){morph_curve::AuthoredLayers layers;std::vector<animation_curve::NamedTrack> tracks;std::vector<std::uint8_t> bytes;std::string error;
        Check(morph_curve::CaptureLayers(first.raw(),layers,error)&&morph_curve::LayersTracks(layers,tracks,error)&&animation_curve::Encode(tracks,bytes,error),"capture detached morph tool authors");return bytes;}
    void Command(unsigned id){auto* previous=g_Block;g_Block=app.get();CmdFileMenu(app.get(),app->MainWindow(),std::uint16_t(id),0);g_Block=previous;}
};
struct Modal {
    inline static thread_local Modal* current=nullptr;
    HWND owner;HHOOK observe=nullptr,hide=nullptr;bool blink,cancel;int first,last;unsigned dialogs=0,reports=0;
    std::array<HWND,8> windows{};unsigned windowCount=0;bool timedOut=false;
    static constexpr UINT_PTR timeoutId=0x4d4f5250;
    std::function<void()> during;
    static LRESULT CALLBACK Hide(int code,WPARAM wp,LPARAM lp){if(code==HCBT_CREATEWND){auto* value=reinterpret_cast<CBT_CREATEWNDW*>(lp);value->lpcs->style&=~WS_VISIBLE;
            if(!(value->lpcs->style&WS_CHILD))value->lpcs->x=value->lpcs->y=-32000;}
        if(code==HCBT_ACTIVATE)return 1;return CallNextHookEx(nullptr,code,wp,lp);}
    static void CALLBACK Timeout(HWND dialog,UINT,UINT_PTR id,DWORD){
        if(!current||id!=timeoutId)return;bool owned=false;for(unsigned i=0;i<current->windowCount;++i)owned|=current->windows[i]==dialog;if(!owned)return;
        current->timedOut=true;KillTimer(dialog,timeoutId);char title[128]{};GetWindowTextA(dialog,title,int(sizeof title));
        std::fprintf(stderr,"FAIL morph tool modal timed out: %s hwnd=%p\n",title,static_cast<void*>(dialog));
        // This is a test-owned dialog, not a user window. A broken scripted
        // command must return a failed check rather than leave a hidden loop.
        EndDialog(dialog,IDCANCEL);
    }
    static LRESULT CALLBACK Inspect(int code,WPARAM wp,LPARAM lp){if(code>=0&&current){const auto& message=*reinterpret_cast<CWPRETSTRUCT*>(lp);
        if(message.message==WM_INITDIALOG&&GetWindow(message.hwnd,GW_OWNER)==current->owner){const auto dialog=message.hwnd;
            Check(current->windowCount<current->windows.size(),"bound the morph tool modal observer's owned dialogs");
            current->windows[current->windowCount++]=dialog;Check(SetTimer(dialog,timeoutId,5000,Timeout)!=0,"arm a test-owned modal timeout");
            if(GetDlgItem(dialog,current->blink?panel::kBlinkStartEdit:panel::kShiftFramesEdit)){
                ++current->dialogs;SetDlgItemInt(dialog,current->blink?panel::kBlinkStartEdit:panel::kShiftFramesEdit,current->first,TRUE);
                if(current->blink)SetDlgItemInt(dialog,panel::kBlinkEndEdit,current->last,TRUE);
                std::printf("    modal input: %s [%d,%d] %s\n",current->blink?"blink":"shift",current->first,current->last,current->cancel?"cancel":"OK");std::fflush(stdout);
                if(current->during)current->during();PostMessageW(dialog,WM_COMMAND,current->cancel?IDCANCEL:IDOK,0);
            }else{++current->reports;
                // The native message box may use IDCANCEL for its sole OK
                // button. Send the real button command, not an assumed IDOK.
                auto button=GetDlgItem(dialog,IDOK);if(!button)button=GetDlgItem(dialog,IDCANCEL);
                Check(button!=nullptr,"native morph report exposes a real dismissal button");
                std::printf("    modal report: button=%d\n",GetDlgCtrlID(button));std::fflush(stdout);
                PostMessageW(dialog,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(button),BN_CLICKED),reinterpret_cast<LPARAM>(button));}
        }}return CallNextHookEx(nullptr,code,wp,lp);}
    Modal(Scene& scene,bool random,int value,int end=0,bool abort=false):owner(scene.app->MainWindow()),blink(random),cancel(abort),first(value),last(end){
        Check(!current,"one morph tool modal observer");current=this;hide=SetWindowsHookExW(WH_CBT,Hide,nullptr,GetCurrentThreadId());
        observe=SetWindowsHookExW(WH_CALLWNDPROCRET,Inspect,nullptr,GetCurrentThreadId());Check(hide&&observe,"observe real morph tool dialogs without activation");}
    ~Modal(){for(unsigned i=0;i<windowCount;++i)if(IsWindow(windows[i]))KillTimer(windows[i],timeoutId);
        UnhookWindowsHookEx(observe);UnhookWindowsHookEx(hide);current=nullptr;Check(!timedOut,"real morph tool modals complete without timeout assistance");}
};
void ShiftHistory(){
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})for(int shift:{0,4,-12,-80}){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"select lip shift recording mode");
        const auto before=MorphKeysCopy(f.first),foreign=MorphKeysCopy(f.second);const auto weights=f.firstMorphs;const auto bones=f.first.bones;
        const auto boneKeys=RegistrationKeys(f.first);const auto ring=PoseRingDigest(f.first);const auto cursors=f.firstCursors;const auto active=f.firstActive;const auto digest=f.Digest();
        auto change=f.Shift(shift);Check(f.Digest()==digest,"lip shift planning does not write keys author maps pose flags or UI");
        if(!shift){Check(!change&&f.history.UndoCount()==0,"zero lip shift is a no-op");continue;}f.CommitTool(change);const auto after=MorphKeysCopy(f.first);
        Check(f.history.UndoCount()==1&&!std::memcmp(weights.data(),f.firstMorphs.data(),sizeof weights)&&f.firstCursors==cursors&&f.firstActive==active,
            "lip shift records once in both modes and preserves all unregistered morph weights and cursors");
        unsigned previous=0;for(unsigned index=f.first.m->morphKeys[0].next;index;index=f.first.m->morphKeys[index].next){
            Check(f.first.m->morphKeys[index].frame==unsigned(std::int64_t(before[index].frame)+shift)&&f.first.m->morphKeys[index].previous==previous,
                "lip shift retains numeric frame offsets and repairs the clipped head's backwards link");previous=index;}
        Check(MorphKeysMatch(f.second,foreign)&&SameRegistrationKeys(f.first,boneKeys)&&PoseRingDigest(f.first)==ring&&
            !std::memcmp(bones.data(),f.first.bones.data(),sizeof bones)&&f.first.m->maxFrame==99&&f.first.m->searchCursor==5&&f.app->LastRegisteredFrame()==93,
            "lip shift leaves another model bones old rings search cursor and native high-water markers untouched");
        f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;f.app->CurrentFrame()=37;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&MorphKeysMatch(f.second,foreign),"lip undo restores exact source while another model and playhead are selected");
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&!std::memcmp(weights.data(),f.firstMorphs.data(),sizeof weights),
            "lip redo restores the immutable shifted keys without erasing dirty morph pose");
    }
}
void NativeMenuAndBlinkOracle(){
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Scene f,oracle;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"select real morph tool menu mode");
        const auto before=MorphKeysCopy(f.first);const auto weights=f.firstMorphs;const auto boneKeys=RegistrationKeys(f.first);
        std::printf("    actual menu mode=%u bound shift\n",unsigned(mode));std::fflush(stdout);
        {Modal modal(f,false,4);f.Command(225);Check(modal.dialogs==1,"225 enters its actual frame-shift dialog");}
        std::puts("    unbound shift oracle");std::fflush(stdout);
        {auto renderer=std::make_unique<D3DRenderer>();oracle.app->Renderer()=renderer.get();Modal modal(oracle,false,4);oracle.Command(225);oracle.app->Renderer()=nullptr;}
        Check(MorphKeysMatch(f.first,MorphKeysCopy(oracle.first))&&f.history.UndoCount()==1,"real bound 225 exactly matches positive-shift legacy native bytes");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&f.history.RedoCount()==1,"real 225 participates in the common undo cursor");
        {Modal modal(f,false,100,0,true);f.Command(225);Check(modal.dialogs==1,"225 cancellation closes actual modal");}
        {Modal modal(f,false,0);f.Command(225);}
        {Modal modal(f,true,10,10);f.Command(227);Check(modal.dialogs==1&&modal.reports==1,"empty blink range reports zero without committing");}
        Check(f.history.UndoCount()==0&&f.history.RedoCount()==1&&MorphKeysMatch(f.first,before),"cancel and both no-ops preserve the existing redo branch");
        // Reset the unbound oracle after its positive shift, then compare the
        // real random registrar against an independently seeded private plan.
        std::memcpy(oracle.first.m->morphKeys,before.data(),before.size()*sizeof(mdl::MorphKey));
        oracle.app->LastRegisteredFrame()=93;oracle.app->state.blinkPhase=0;
        std::puts("    bound blink generator");std::fflush(stdout);
        std::srand(1729);{Modal modal(f,true,0,350);f.Command(227);Check(modal.dialogs==1&&modal.reports==1,"227 opens the actual range dialog and one count report");}
        std::puts("    unbound random registrar oracle");std::fflush(stdout);
        std::srand(1729);{Modal modal(oracle,true,0,350);oracle.Command(227);Check(modal.dialogs==1&&modal.reports==1,"unbound real 227 supplies a separately generated native oracle");}
        const auto after=MorphKeysCopy(f.first);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(oracle.first))&&f.first.m->maxFrame==oracle.first.m->maxFrame&&
            f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame()&&f.app->state.blinkPhase==oracle.app->state.blinkPhase&&f.history.UndoCount()==1,
            "random blink keys allocation export marks maxFrame last registration and phase exactly match the native command");
        Check(!std::memcmp(weights.data(),f.firstMorphs.data(),sizeof weights)&&SameRegistrationKeys(f.first,boneKeys)&&f.first.m->searchCursor==5,
            "bound random blink preserves dirty weights and unrelated keys instead of applying a broad seek");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&f.app->state.blinkPhase==0&&f.app->LastRegisteredFrame()==93,
            "blink undo restores source keys and owned phase/high-water metadata");
        std::srand(73);const auto next=std::rand();std::srand(73);
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&std::rand()==next,"blink redo restores the recorded sequence without consuming randomness again");
    }
}
void AuthoringAndCleanPose(){
    Scene f;NativePoseTestBinding binding(*f.app,f.history);std::string error;const auto first=f.Input(0),other=f.Input(1);
    Check(morph_curve::Write(f.first.raw(),{first,other},error),"seed independent Free author layers for lip and blink");
    morph_curve::AuthoredLayers before;Check(morph_curve::CaptureLayers(f.first.raw(),before,error),"capture raw authoring before retime");
    const auto source=f.Effective(0),unrelated=f.Effective(1);const auto sourceKeys=MorphKeysCopy(f.first);
    const auto charged=before.RetainedBytes();Check(charged>sizeof(morph_curve::AuthoredLayers),"author snapshots report retained maps controls and native baselines");
    f.firstMorphs[0].value=88.25f;f.CommitTool(f.Shift(4));const auto shifted=f.Effective(0),stillOther=f.Effective(1);
    Check(shifted.track.keys.size()==source.track.keys.size()&&shifted.track.keys.front().frame==0,"lip author retime keeps its frame-zero anchor");
    for(std::size_t key=0;key<source.track.keys.size();++key){const auto& a=source.track.keys[key];const auto& b=shifted.track.keys[key];
        Check(b.frame==(a.frame?a.frame+4:0)&&a.value==b.value&&a.inSlope==b.inSlope&&a.outSlope==b.outSlope&&a.inWeight==b.inWeight&&a.outWeight==b.outWeight,
            "lip retime preserves exact double Free handles and only moves positive-time author keys");}
    std::vector<std::uint8_t> a,b;Check(animation_curve::Encode({unrelated},a,error)&&animation_curve::Encode({stillOther},b,error)&&a==b,"lip retime leaves another morph's author track exact");
    Check(f.firstMorphs[0].value==88.25f&&f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,sourceKeys)&&morph_curve::LayersUnchanged(f.first.raw(),before),
        "lip undo restores the complete raw pristine/effective/mirror/observed state and retains dirty weight");
    std::srand(31);f.CommitTool(f.Blink(0,350));const auto blinkKeys=MorphKeysCopy(f.first);
    Check(morph_curve::LayersUnchanged(f.first.raw(),before),"blink registration preserves pristine authors and all raw baselines instead of replacing them with linear keys");
    Check(f.history.History(false)==MMS_OK&&morph_curve::LayersUnchanged(f.first.raw(),before)&&f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,blinkKeys),
        "blink undo and redo preserve exact authored layers through pool growth");
    Scene clean,cleanOracle;NativePoseTestBinding cleanBinding(*clean.app,clean.history);
    SeekModelFrame(clean.first.raw(),7,0);SeekModelFrame(cleanOracle.first.raw(),7,0);const auto oldWeight=clean.firstMorphs[0].value;
    clean.CommitTool(clean.Shift(4));
    {auto renderer=std::make_unique<D3DRenderer>();cleanOracle.app->Renderer()=renderer.get();Modal modal(cleanOracle,false,4);cleanOracle.Command(225);cleanOracle.app->Renderer()=nullptr;}
    Check(MorphSameFloat(clean.firstMorphs[0].value,cleanOracle.firstMorphs[0].value)&&clean.app->CurrentFrame()==7,"clean lip weight matches real legacy shift and native float seek at the unchanged playhead");
    Check(clean.history.History(false)==MMS_OK&&MorphSameFloat(clean.firstMorphs[0].value,oldWeight),"clean lip undo restores the original local sample");
}
void NegativeAuthorClipping(){
    for(int shift:{-12,-80}){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);std::string error;auto input=f.Input(0);
        input.track.keys[2].outSlope=1.75;input.track.keys[3].inSlope=-2.25;
        Check(morph_curve::Write(f.first.raw(),{input},error),"seed a weighted Free lip curve with an overshooting control hull");
        const auto source=f.Effective(0);auto expected=source;
        Check(animation_curve::Evaluate(source.track,35).value>1.5,"the retained Free interval actually overshoots its endpoint values");
        expected.track.keys.erase(std::remove_if(expected.track.keys.begin(),expected.track.keys.end(),[&](const auto& key){return key.frame>0&&key.frame+shift<=0;}),expected.track.keys.end());
        for(auto& key:expected.track.keys)if(key.frame)key.frame+=shift;
        morph_curve::AuthoredLayers before,after;Check(morph_curve::CaptureLayers(f.first.raw(),before,error),"capture raw layers before negative lip retime");
        const auto nativeBefore=MorphKeysCopy(f.first);f.firstMorphs[0].value=88.25f;f.CommitTool(f.Shift(shift));
        const auto actual=f.Effective(0);std::vector<std::uint8_t> expectedBytes,actualBytes;
        Check(animation_curve::Encode({expected},expectedBytes,error)&&animation_curve::Encode({actual},actualBytes,error)&&expectedBytes==actualBytes,
            "negative lip shift clips nonpositive keys and preserves every surviving Free handle and frame-zero anchor exactly");
        if(shift==-12)Check(animation_curve::Evaluate(actual.track,23).value==animation_curve::Evaluate(source.track,35).value,
            "an entirely retained overshooting Bezier interval translates without changing its control hull");
        else Check(actual.track.keys.size()==1&&actual.track.keys.front().frame==0,"a fully clipped mouth curve keeps only its own zero anchor");
        const auto nativeAfter=MorphKeysCopy(f.first);Check(morph_curve::CaptureLayers(f.first.raw(),after,error),"capture raw negative-shift target layers");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,nativeBefore)&&morph_curve::LayersUnchanged(f.first.raw(),before)&&f.firstMorphs[0].value==88.25f,
            "negative-shift undo restores exact native and all raw author layers without replacing dirty weight");
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,nativeAfter)&&morph_curve::LayersUnchanged(f.first.raw(),after),
            "negative-shift redo restores the same clipped Free handles and raw baselines");
    }
}
unsigned randomCalls=0;
int FixedRandom(){++randomCalls;return 16384;}
int ZeroRandom(){++randomCalls;return 0;}
int InvalidRandom(){++randomCalls;return 32768;}
void RefusalBudgetAndAllocation(){
    Scene f;NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Digest();
    {NativeMorphToolsPlan plan;Check(plan.Begin(*f.app,Tool::MouthShift)==MMS_OK&&plan.ReadShift(INT_MAX)==MMS_E_ARGUMENT&&f.Digest()==before,"lip overflow refuses privately with no writes");}
    {NativeMorphToolsPlan plan;Check(plan.Begin(*f.app,Tool::RandomBlink)==MMS_OK&&plan.ReadBlink(-1,20)==MMS_E_ARGUMENT&&plan.ReadBlink(0,20,InvalidRandom)==MMS_E_ARGUMENT&&f.Digest()==before,
        "invalid blink ranges and random values leave the complete scene unchanged");}
    {f.app->state.blinkPhase=1;const auto stable=f.Digest();randomCalls=0;NativeMorphToolsPlan plan;
        Check(plan.Begin(*f.app,Tool::RandomBlink)==MMS_OK&&plan.ReadBlink(0,1,ZeroRandom)==MMS_E_FAILED&&randomCalls==4097&&f.Digest()==stable,
            "bounded retry refuses a non-progressing random source instead of hanging or leaking a partial blink");f.app->state.blinkPhase=0;}
    {NativeMorphToolsPlan plan;Check(plan.Begin(*f.app,Tool::MouthShift)==MMS_OK,"capture lip modal before binding replacement");
        {NativePoseTestBinding nested(*f.app,f.history);}Check(plan.ReadShift(2)==MMS_E_STALE,"modal binding generation changes refuse before dereferencing a captured journal");}
    auto change=f.Shift(-12);const auto retained=change->RetainedBytes();
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,8,retained-1)==MMS_OK,"set insufficient native morph budget");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("budget",change,token)==MMS_E_MEMORY&&!token&&f.Digest()==before,"budget failure publishes neither shifted keys nor authoring");
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,128,256ull*1024*1024)==MMS_OK,"restore morph tool journal budget");
    bool ready=false;unsigned failures=0;
    for(long limit=0;limit<2048&&!ready;++limit){std::string label="morph tool OOM sweep";failAfter=limit;const auto status=f.history.PrepareNative(std::move(label),change,token);failAfter=-1;
        Check(f.Digest()==before&&!f.history.UndoCount(),"morph preparation allocation failures leave keys pose authoring and journal unchanged");
        if(status==MMS_OK)ready=true;else{++failures;Check(status==MMS_E_MEMORY&&!token,"failed morph candidate releases its reservation");}}
    Check(ready&&failures,"morph tool allocation sweep reaches a complete candidate");token.reset();f.CommitTool(change);const auto after=f.Digest();
    failAfter=0;const auto undo=f.history.History(false);failAfter=-1;
    Check(undo==MMS_E_MEMORY&&f.Digest()==after&&f.history.UndoCount()==1,"undo allocation failure preserves the published morph tool");
    Check(f.history.History(false)==MMS_OK,"retry morph tool undo");failAfter=0;const auto redo=f.history.History(true);failAfter=-1;
    Check(redo==MMS_E_MEMORY&&f.Digest()==before&&f.history.RedoCount()==1,"redo allocation failure preserves the source and redo branch");
    Check(f.history.History(true)==MMS_OK,"retry morph tool redo");
    Scene random;NativePoseTestBinding randomBinding(*random.app,random.history);randomCalls=0;auto blink=random.Blink(0,350,FixedRandom);const auto calls=randomCalls;
    random.CommitTool(blink);Check(random.history.History(false)==MMS_OK&&random.history.History(true)==MMS_OK&&randomCalls==calls,
        "prepare publication undo and redo never invoke the discarded random callback");
}
void PluginComposition(){
    Scene f;NativePoseTestBinding binding(*f.app,f.history);camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.morph-tools";
    f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof(serializer),&provider)==MMS_OK,"register real morph tool plugin serializer");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);auto change=f.Shift(4);const auto before=MorphKeysCopy(f.first);
    std::uint64_t revision=0;MmsHandle transaction=0;constexpr char label[]="mixed morph tool";
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,{label,sizeof(label)-1},revision,&transaction)==MMS_OK,"begin real mixed plugin/native morph transaction");
    NativeMorphToolsPlan busy;Check(busy.Begin(*f.app,Tool::RandomBlink)==MMS_E_BUSY,"plugin draft excludes morph tool modal admission");
    auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK&&
        api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&f.history.UndoCount()==1,"plugin state and lip frames publish as one common history entry");
    const auto after=MorphKeysCopy(f.first);
    Check(api.undo(api.context)==MMS_OK&&plugin.value==0&&MorphKeysMatch(f.first,before)&&api.redo(api.context)==MMS_OK&&plugin.value==42&&MorphKeysMatch(f.first,after),
        "real plugin undo and redo replay the same native morph tool node");
    Check(f.history.Reset()==MMS_OK,"clear mixed morph history before unregistering serializer");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister morph tool serializer");
}
}
void NativeMorphToolsChecks(){
    std::puts("  morph tools: shift and both history modes");native_morph_tools_checks::ShiftHistory();
    std::puts("  morph tools: actual menus and independent random registrar oracle");native_morph_tools_checks::NativeMenuAndBlinkOracle();
    std::puts("  morph tools: raw author layers and clean/dirty local pose");native_morph_tools_checks::AuthoringAndCleanPose();
    std::puts("  morph tools: negative author clipping and retained control hull");native_morph_tools_checks::NegativeAuthorClipping();
    std::puts("  morph tools: refusal budget allocation and random retry bounds");native_morph_tools_checks::RefusalBudgetAndAllocation();
    std::puts("  morph tools: real plugin composition");native_morph_tools_checks::PluginComposition();
}
