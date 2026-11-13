#include "../src/plugins/native_morph_curve_write.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include <cmath>
#include <cwchar>
namespace {
struct MorphCurveWriteFixture:MorphKeyFixture {
    std::array<std::uint32_t,3> firstCursors{{7,6,2}},secondCursors{{7,6,2}};
    std::array<unsigned char,3> firstActive{{1,1,0}},secondActive{{1,1,0}};
    MorphCurveWriteFixture(){MorphKeySeed(first);MorphKeySeed(second);first.m->morphKeyCursors=firstCursors.data();second.m->morphKeyCursors=secondCursors.data();
        first.m->morphTrackActive=firstActive.data();second.m->morphTrackActive=secondActive.data();app->CurrentFrame()=7;app->LastRegisteredFrame()=9;}
    ~MorphCurveWriteFixture(){history.Reset();history.Collect();first.m->morphKeyCursors=second.m->morphKeyCursors=nullptr;first.m->morphTrackActive=second.m->morphTrackActive=nullptr;}
    std::uint64_t Digest(){std::uint64_t hash=first.Digest();const auto add=[&](const void* data,std::size_t bytes){const auto* p=static_cast<const unsigned char*>(data);while(bytes--){hash^=*p++;hash*=1099511628211ull;}};
        add(&app->state,sizeof app->state);add(firstMorphs.data(),sizeof firstMorphs);add(firstCursors.data(),sizeof firstCursors);add(firstActive.data(),sizeof firstActive);
        add(first.m->morphKeys,mdl::MorphKeyCapacity(first.m->morphKeys)*sizeof(mdl::MorphKey));return hash;}
    animation_curve::NamedTrack Input(unsigned morph=0){return MorphAuthoredTrack(firstMorphs[morph],int(morph));}
    animation_curve::NamedTrack Native(unsigned morph=0){animation_curve::NamedTrack named;std::string error;named.id=morph_curve::StableId(int(morph),firstMorphs[morph].name);
        Check(morph_curve::ReadNative(first.raw(),int(morph),named.track,error),"read native morph curve source without authored synchronization");return named;}
    animation_curve::NamedTrack Effective(unsigned morph=0){std::vector<animation_curve::Track> tracks;std::wstring error;
        Check(morph_curve::ReadManyDetached(*app,{int(morph)},tracks,error)&&tracks.size()==1,"read detached canonical morph curve");
        return {morph_curve::StableId(int(morph),firstMorphs[morph].name),std::move(tracks.front())};}
    std::shared_ptr<plugins::HistoryChange> Plan(const std::vector<animation_curve::NamedTrack>& tracks){std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanMorphCurveWrite(*app,0,tracks,change)==MMS_OK,"prepare detached morph authored replacement");return change;}
    void Commit(const std::shared_ptr<plugins::HistoryChange>& change){std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("morph curves",change,transaction,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"reserve morph authored replacement in shared history");
        Check(CommitRegistrationNoAllocation(history,std::move(transaction))==MMS_OK,"publish morph pool layers local pose and cursors without allocations");}
};
void MorphHostCheck(bool ok,const char* stage,const char* aspect){
    if(ok)return;
    const std::string message=std::string(stage)+": "+aspect;
    Check(false,message.c_str());
}
bool MorphHostSameTrack(const animation_curve::Track& left,const animation_curve::Track& right){
    if(left.discrete!=right.discrete||left.keys.size()!=right.keys.size())return false;
    for(std::size_t index=0;index<left.keys.size();++index){
        const auto& a=left.keys[index];const auto& b=right.keys[index];
        if(a.frame!=b.frame||a.value!=b.value||a.inSlope!=b.inSlope||a.outSlope!=b.outSlope||
            a.inWeight!=b.inWeight||a.outWeight!=b.outWeight||a.interpolation!=b.interpolation||a.tangent!=b.tangent)return false;
    }
    return true;
}
struct MorphHostEditorScan{HWND owner=nullptr;HWND window=nullptr;unsigned count=0;};
BOOL CALLBACK MorphHostEditorScanProc(HWND window,LPARAM context){
    auto& scan=*reinterpret_cast<MorphHostEditorScan*>(context);
    wchar_t name[128]{};
    if(!GetClassNameW(window,name,128)||std::wstring(name)!=L"MikuMikuStudioCurveEditorWindow")return TRUE;
    if(GetWindow(window,GW_OWNER)!=scan.owner)return TRUE;
    ++scan.count;if(!scan.window)scan.window=window;
    return TRUE;
}
MorphHostEditorScan MorphHostEditorWindows(HWND owner){
    MorphHostEditorScan scan;scan.owner=owner;
    EnumThreadWindows(GetCurrentThreadId(),MorphHostEditorScanProc,reinterpret_cast<LPARAM>(&scan));
    return scan;
}
struct MorphHostSnapshot{
    std::vector<mdl::MorphKey> keys;
    std::array<mdl::MorphRecord,3> morphs;
    std::array<std::uint32_t,3> cursors;
    std::array<unsigned char,3> active;
    morph_curve::AuthoredLayers layers;
    const mdl::MorphKey* pool=nullptr;
    std::size_t capacity=0;
    std::uint64_t identity=0;
    std::uint32_t maxFrame=0,searchCursor=0,lastRegistered=0;
    std::uint64_t revision=0;
    std::size_t undo=0,redo=0;
    explicit MorphHostSnapshot(MorphCurveWriteFixture& f):keys(MorphKeysCopy(f.first)),morphs(f.firstMorphs),cursors(f.firstCursors),active(f.firstActive),
        pool(f.first.m->morphKeys),capacity(mdl::MorphKeyCapacity(f.first.m->morphKeys)),identity(mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)),
        maxFrame(f.first.m->maxFrame),searchCursor(f.first.m->searchCursor),lastRegistered(f.app->LastRegisteredFrame()){
        std::string error;
        Check(morph_curve::CaptureLayers(f.first.raw(),layers,error),"capture the authored expression layers before the host regression");
        Check(f.history.Revision(revision)==MMS_OK,"read the shared expression history revision");
        undo=f.history.UndoCount();redo=f.history.RedoCount();
    }
    bool Matches(MorphCurveWriteFixture& f)const{
        return MorphKeysMatch(f.first,keys)&&f.first.m->morphKeys==pool&&mdl::MorphKeyCapacity(f.first.m->morphKeys)==capacity&&
            mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity&&
            !std::memcmp(f.firstMorphs.data(),morphs.data(),sizeof morphs)&&f.firstCursors==cursors&&f.firstActive==active&&
            f.first.m->maxFrame==maxFrame&&f.first.m->searchCursor==searchCursor&&f.app->LastRegisteredFrame()==lastRegistered&&
            morph_curve::LayersUnchanged(f.first.raw(),layers);
    }
};
HWND MorphHostOpenExpression(MorphCurveWriteFixture& f,const char* stage){
    const auto lane=curve_host::MorphLaneInitial(*f.app,0);
    MorphHostCheck(!lane.empty()&&lane==morph_curve::StableId(0,f.firstMorphs[0].name),stage,"the panel eyebrow lane names its selected morph");
    curve_host::OpenMorph(f.app.get(),0,false);
    const auto scan=MorphHostEditorWindows(f.app->MainWindow());
    MorphHostCheck(scan.count==1&&scan.window!=nullptr,stage,"exactly one real host expression curve window is created");
    MorphHostCheck(scan.window!=nullptr&&curve_editor::OwnsWindow(f.app->MainWindow(),scan.window),stage,"the hidden expression curve window belongs to the fixture owner");
    MorphHostCheck(scan.window!=nullptr&&!IsWindowVisible(scan.window),stage,"the expression curve window stays hidden");
    return scan.window;
}
void MorphHostCloseExpression(MorphCurveWriteFixture& f,HWND editor,const char* stage){
    MorphHostCheck(editor!=nullptr&&IsWindow(editor),stage,"the expression curve window is live before close");
    if(editor&&IsWindow(editor))SendMessageW(editor,WM_CLOSE,0,0);
    MorphHostCheck(editor==nullptr||!IsWindow(editor),stage,"WM_CLOSE destroys the hidden expression curve window");
    const auto scan=MorphHostEditorWindows(f.app->MainWindow());
    MorphHostCheck(scan.count==0,stage,"no expression curve window remains owned by the fixture");
}
void MorphHostViewExpression(HWND editor,const char* stage){
    MorphHostCheck(editor!=nullptr&&IsWindow(editor),stage,"the expression curve window is live for read-only viewing");
    if(editor==nullptr||!IsWindow(editor))return;
    for(double frame:{1.0,5.0,9.0,13.0,17.0,21.0,25.0,28.0}){
        std::array<double,3> values{{frame,0,0}};
        MorphHostCheck(SendMessageW(editor,WM_APP+396,0,reinterpret_cast<LPARAM>(values.data()))!=0,stage,"read-only expression sampling answers");
    }
    int compare[12]{};std::uint64_t state[5]{};
    MorphHostCheck(SendMessageW(editor,WM_APP+308,0,reinterpret_cast<LPARAM>(compare))!=0,stage,"read-only comparison state answers");
    MorphHostCheck(SendMessageW(editor,WM_APP+397,0,reinterpret_cast<LPARAM>(state))!=0,stage,"read-only history state answers");
}
void MorphHostVerifyExpression(MorphCurveWriteFixture& f,const MorphHostSnapshot& snapshot,const animation_curve::NamedTrack& authored,HWND editor,const char* stage){
    MorphHostCheck(snapshot.Matches(f),stage,"native morph key bytes, model weights and authored layers are unchanged");
    std::uint64_t revision=0;
    MorphHostCheck(f.history.Revision(revision)==MMS_OK&&revision==snapshot.revision&&f.history.UndoCount()==snapshot.undo&&f.history.RedoCount()==snapshot.redo,
        stage,"shared history revision and undo/redo counts are unchanged");
    std::vector<curve_editor::Channel> channels;std::wstring error;
    const bool read=curve_host::ReadMorphChannels(*f.app,channels,error)&&channels.size()==3&&
        channels[0].id==authored.id&&MorphHostSameTrack(channels[0].track,authored.track);
    MorphHostCheck(read,stage,"the bound expression source returns the authored Bezier interpolation slopes weights and values");
    animation_curve::Track native;std::string nativeError;
    const bool nativeRead=morph_curve::ReadNative(f.first.raw(),0,native,nativeError)&&native.keys.size()==authored.track.keys.size();
    MorphHostCheck(nativeRead,stage,"the native mirror still carries the authored key count");
    if(nativeRead){
        MorphHostCheck(native.keys.front().value==static_cast<double>(static_cast<float>(authored.track.keys.front().value))&&
            native.keys.back().value==static_cast<double>(static_cast<float>(authored.track.keys.back().value)),stage,"the native mirror keeps the clean float-rounded authored endpoints");
        MorphHostCheck(native.keys.front().interpolation==animation_curve::Interpolation::Linear,stage,"the native mirror stays the legacy linear float chain");
    }
    const double firstValue=authored.track.keys.front().value,lastValue=authored.track.keys.back().value;
    const double span=double(authored.track.keys.back().frame-authored.track.keys.front().frame);
    const double secant=(lastValue-firstValue)/span;
    for(double frame:{3.0,7.0,14.0,22.0,28.0}){
        const auto expected=animation_curve::Evaluate(authored.track,frame);
        if(read){
            const auto actual=animation_curve::Evaluate(channels[0].track,frame);
            MorphHostCheck(actual.value==expected.value&&actual.derivative==expected.derivative,stage,"the bound source samples the authored value and derivative exactly");
        }
        const double linear=firstValue+secant*(frame-double(authored.track.keys.front().frame));
        MorphHostCheck(std::fabs(expected.value-linear)>.05,stage,"the authored interior value differs from a linear interpolation");
        MorphHostCheck(std::fabs(expected.derivative-secant)>.005,stage,"the authored interior derivative differs from the linear secant");
        if(editor){
            std::array<double,3> values{{frame,0,0}};
            MorphHostCheck(SendMessageW(editor,WM_APP+396,0,reinterpret_cast<LPARAM>(values.data()))!=0,stage,"the expression editor answers its sampling message");
            MorphHostCheck(values[1]==expected.value&&values[2]==expected.derivative,stage,"the expression editor samples the independently retained authored value and derivative");
        }
    }
    if(editor){
        int compare[12]{};std::uint64_t state[5]{};
        MorphHostCheck(SendMessageW(editor,WM_APP+308,0,reinterpret_cast<LPARAM>(compare))!=0&&compare[1]==0,stage,"the expression editor starts on the lane eyebrow channel");
        MorphHostCheck(SendMessageW(editor,WM_APP+397,0,reinterpret_cast<LPARAM>(state))!=0&&!state[0]&&!state[1]&&state[3]!=0,stage,"the expression editor uses the shared journal without a local undo stack");
    }
}
void MorphCurveWriteParity(){
    for(bool multiple:{false,true})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        MorphCurveWriteFixture f;Check(f.history.SetMode(mode)==MMS_OK,"morph curve explicit commit works in each recording mode");
        f.first.m->maxFrame=f.second.m->maxFrame=7;f.first.m->morphKeys[0].allocated=f.second.m->morphKeys[0].allocated=7;
        const auto unchanged=f.Native();const auto before=MorphKeysCopy(f.first);const auto source=f.Digest();
        Check(!f.Plan({unchanged})&&f.Digest()==source,"unchanged effective morph curve is a no-op without changing marks or history");
        const auto ring=PoseRingDigest(f.first);const auto bones=RegistrationKeys(f.first);const auto other=f.firstMorphs;
        std::vector<animation_curve::NamedTrack> input={f.Input()};if(multiple)input.push_back(f.Input(1));auto change=f.Plan(input);
        Check(change&&f.Digest()==source&&change->RetainedBytes()>before.size()*sizeof(mdl::MorphKey),"morph plan accounts private native and full authored payload without publishing");
        std::string error;Check(morph_curve::Write(f.second.raw(),input,error),"legacy pure morph Write supplies exact mirror oracle");f.Commit(change);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&f.first.m->searchCursor==f.second.m->searchCursor&&f.first.m->maxFrame==f.second.m->maxFrame&&f.app->LastRegisteredFrame()==50,
            "first morph curve publication exactly matches legacy slots bytes export marks and scalar bookkeeping");
        for(unsigned morph=0;morph<3;++morph)Check(MorphSameFloat(f.firstMorphs[morph].value,morph<unsigned(input.size())?float(animation_curve::Evaluate(input[morph].track,7).value):other[morph].value),
            "first curve write samples only named morph weights and preserves unrelated temporary values");
        Check(SameRegistrationKeys(f.first,bones)&&PoseRingDigest(f.first)==ring,"morph curve write does not evaluate bone pose or write old native rings");
        std::vector<std::uint8_t> actualVmd,oracleVmd;Check(morph_curve::CaptureVmd(f.first.raw(),0,actualVmd,error)&&morph_curve::CaptureVmd(f.second.raw(),0,oracleVmd,error)&&actualVmd==oracleVmd,
            "MMSMCRV1 bytes and standard-section export selection remain unchanged");
        const auto after=MorphKeysCopy(f.first);f.app->CurrentFrame()=23;f.app->SelectedModelSlot()=1;const auto foreign=f.secondMorphs;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&!morph_curve::AnyCurve(f.first.raw())&&f.app->CurrentFrame()==23,
            "undo restores exact original morph pool and author absence at the current playhead");
        Check(!std::memcmp(f.secondMorphs.data(),foreign.data(),sizeof foreign)&&f.firstMorphs[2].value==other[2].value,
            "morph curve undo preserves a different selected model and unowned morph weights");
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&f.history.UndoCount()==1,
            "morph curve redo restores the installed mirror and exact authoring in the common journal");
    }
}
void MorphCurveWriteRawLayers(){
    MorphCurveWriteFixture f;std::string error;auto seed=f.Input();Check(morph_curve::Write(f.first.raw(),{seed},error),"seed pristine morph authoring before a native edit");
    morph_curve::AuthoredLayers raw;Check(morph_curve::CaptureLayers(f.first.raw(),raw,error),"capture full raw pristine effective mirror and observed layers");
    unsigned slot=f.first.m->morphKeys[0].next;f.first.m->morphKeys[slot].value+=.4f;
    const auto source=f.Digest();const auto before=MorphKeysCopy(f.first);auto effective=f.Effective();
    Check(morph_curve::LayersUnchanged(f.first.raw(),raw)&&f.Digest()==source,"bound detached read derives a native edit without updating live author maps");
    const auto expected=float(animation_curve::Evaluate(effective.track,19).value);effective.track.keys.back().value+=.2;
    auto change=f.Plan({effective});std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK&&f.Digest()==source&&morph_curve::LayersUnchanged(f.first.raw(),raw),"private morph candidate retains the exact raw preexisting author layer");
    candidate.reset();Check(f.Digest()==source&&morph_curve::LayersUnchanged(f.first.raw(),raw),"cancelled morph replacement releases private maps without modifying raw cached layers");
    f.Commit(change);f.app->CurrentFrame()=19;
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&morph_curve::LayersUnchanged(f.first.raw(),raw)&&MorphSameFloat(f.firstMorphs[0].value,expected),
        "morph undo publishes original raw layers while sampling a private derived target at the current time");
    Check(f.history.History(true)==MMS_OK,"morph redo remains valid after restoring stale raw mirror and observed layers");
}
void MorphCurveWriteGrowthAndSelection(){
    {
        MorphCurveWriteFixture f;auto track=f.Input();track.track.keys.clear();for(int frame=0;frame<128;++frame){animation_curve::Key key;key.frame=frame;key.value=-2+.03*frame;key.interpolation=animation_curve::Interpolation::Linear;track.track.keys.push_back(key);}
        const auto before=MorphKeysCopy(f.first);const auto identity=mdl::MorphKeyPoolIdentity(f.first.m->morphKeys);std::string error;
        Check(morph_curve::Write(f.second.raw(),{track},error),"legacy morph mirror grows an independent oracle pool");f.Commit(f.Plan({track}));const auto after=MorphKeysCopy(f.first);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&mdl::MorphKeyCapacity(f.first.m->morphKeys)==mdl::MorphKeyCapacity(f.second.m->morphKeys)&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity,
            "private morph growth matches legacy capacity and bytes while retaining stable pool identity");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity&&f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after),
            "morph growth undo and redo resolve the new allocation by stable identity");
    }
    {
        MorphCurveWriteFixture f;f.Commit(f.Plan({f.Input(1),f.Input(0)}));
        const auto set=[&](unsigned morph,unsigned frame,unsigned char value){unsigned index=morph;for(;;){auto& key=f.first.m->morphKeys[index];if(key.frame==frame)key.allocated=value;if(!key.next)break;index=key.next;}};
        const auto mark=[&](unsigned morph,unsigned frame){unsigned index=morph;for(;;){const auto& key=f.first.m->morphKeys[index];if(key.frame==frame)return unsigned(key.allocated);if(!key.next)break;index=key.next;}return 0u;};
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)f.first.m->morphKeys[i].allocated=0;
        set(0,0,9);set(1,0,13);set(0,10,91);set(1,10,73);
        Check(f.history.History(false)==MMS_OK&&mark(0,0)==9&&mark(1,0)==13&&mark(0,10)==91&&mark(1,12)==0,
            "morph undo maps root and multi-selection by morph plus frame across cross-morph slot reuse");
        set(1,12,55);Check(f.history.History(true)==MMS_OK&&mark(0,10)==91&&mark(1,10)==0&&mark(1,25)==0,
            "morph redo never moves another morph's equal-frame selection into a restored slot");
        auto track=f.Effective();track.track.keys.back().value+=.2;auto change=f.Plan({track});std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare full-byte frozen morph publication token");f.first.m->morphKeys[0].allocated^=1;
        Check(!candidate->IsCurrent(),"prepared morph publication still refuses selection changes before commit");
    }
    {
        MorphCurveWriteFixture f;f.Commit(f.Plan({f.Input(0)}));f.first.m->morphKeys[1].allocated=23;
        Check(f.history.History(false)==MMS_OK&&!f.first.m->morphKeys[0].allocated&&f.first.m->morphKeys[1].allocated==23,
            "another morph's new selection does not change this record's exact selection restoration policy");
    }
}
void MorphCurveWriteMixedHistory(){
    MorphCurveWriteFixture f;
    // Morph zero is an editable PMX morph only in mode 2; legacy PMD reserves
    // it as the base morph and deliberately rejects native registration.
    f.first.m->physicsMode=f.second.m->physicsMode=2;
    f.Commit(f.Plan({f.Input()}));const auto first=MorphKeysCopy(f.first);f.firstMorphs[0].value=.42f;
    MorphRegistrationCommit(f,MorphRegistrationPlan(f,0,18));const auto registered=MorphKeysCopy(f.first);auto input=f.Effective();input.track.keys.back().value+=.25;f.Commit(f.Plan({input}));
    f.app->CurrentFrame()=19;morph_curve::Evaluate(f.first.raw(),19);
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,registered),"authored morph curve undo restores an intervening native registration");
    f.app->CurrentFrame()=21;morph_curve::Evaluate(f.first.raw(),21);Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,first),"native registration undo derives against the restored pristine author");
    f.app->CurrentFrame()=3;morph_curve::Evaluate(f.first.raw(),3);Check(f.history.History(false)==MMS_OK&&!morph_curve::AnyCurve(f.first.raw()),"earlier morph replacement remains valid after later derived-cache reads");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,"morph curve native registration curve redo preserves common history ordering");
    // An independent morph may acquire authoring after capture. Undoing this
    // record must merge current unowned maps rather than replacing the table.
    MorphCurveWriteFixture independent;auto a=independent.Plan({independent.Input(0)});independent.Commit(a);
    auto single=independent.Input(1);single.track.keys.resize(1);
    auto b=independent.Plan({single});std::unique_ptr<plugins::PreparedHistoryChange> privateB;Check(b->Prepare(true,privateB)==MMS_OK,"prepare independent author on another morph without reusing this record's slots");privateB->Publish();
    auto other=independent.Effective(1);Check(independent.history.History(false)==MMS_OK&&morph_curve::HasCurve(independent.first.raw(),1)&&!morph_curve::HasCurve(independent.first.raw(),0),
        "undo merges only owned author maps and preserves a later unowned morph's authoring");
    Check(animation_curve::Evaluate(independent.Effective(1).track,13).value==animation_curve::Evaluate(other.track,13).value,"unowned morph's exact authored sampling survives another curve undo");
}
void MorphCurveWriteHostCloseReopen(){
    MorphCurveWriteFixture f;
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    Check(plugins::BoundNativePoseHistory(f.app.get())==&f.history,"the expression host regression binds the fixture's shared history");
    Check(f.app->MainWindow()!=nullptr&&GetDlgItem(f.app->MainWindow(),panel::kPhysicsFrameCheckbox)!=nullptr&&
        GetDlgItem(f.app->MainWindow(),panel::kUndoButton)!=nullptr&&GetDlgItem(f.app->MainWindow(),panel::kReadoutPosXEdit)!=nullptr,
        "the hidden expression host owner has the common controls its publication touches");
    f.app->SelectedModelSlot()=0;f.app->CameraMode()=0;
    f.firstMorphs[0].panel=mdl::MorphPanel::eyebrow;
    MorphHostCheck(f.app->SelectedModel()==f.first.raw(),"fixture setup","the first model is the selected model");
    for(auto& index:f.first.m->selectedMorphs)index=-1;
    f.first.m->selectedMorphs[0]=0;
    MorphHostCheck(morph_curve::PanelGroup(static_cast<std::uint8_t>(f.firstMorphs[0].panel))==0,"fixture setup","the selected lane morph belongs to the panel eyebrow group");
    MorphHostCheck(!curve_host::MorphLaneInitial(*f.app,0).empty(),"fixture setup","the panel eyebrow selector names a valid lane morph");
    const auto modelIdentity=curve_host::MorphIdentityOf(*f.app,f.first.raw());
    MorphHostCheck(curve_host::MorphIdentityCurrent(*f.app,modelIdentity),"fixture setup","the model identity and selected slot agree");
    std::vector<curve_editor::Channel> baseline;std::wstring error;
    Check(curve_host::ReadMorphChannels(*f.app,baseline,error)&&baseline.size()==3,"read the pristine expression channels before the host regression");
    Check(baseline[0].id==morph_curve::StableId(0,f.firstMorphs[0].name),"the panel eyebrow channel is the selected morph");
    animation_curve::NamedTrack authored;authored.id=baseline[0].id;
    animation_curve::Key first,last;
    first.frame=0;first.value=.1;first.inSlope=.02;first.outSlope=.12;first.inWeight=.4;first.outWeight=.2;
    last.frame=30;last.value=.9;last.inSlope=-.04;last.outSlope=-.02;last.inWeight=.7;last.outWeight=.3;
    first.interpolation=last.interpolation=animation_curve::Interpolation::Bezier;
    first.tangent=last.tangent=animation_curve::Tangent::Free;
    authored.track.keys={first,last};
    auto next=baseline;next[0].track=authored.track;
    Check(curve_host::WriteMorphChannels(*f.app,modelIdentity,next,baseline,error),"the bound production expression write installs the seeded Bezier curve");
    Check(f.history.UndoCount()==1&&!f.history.RedoCount(),"the seeded expression write records exactly one shared history node");
    Check(morph_curve::CurveCount(f.first.raw())==1&&morph_curve::HasCurve(f.first.raw(),0),"the bound write installs exactly one authored expression curve");
    MorphHostSnapshot snapshot(f);
    MorphHostCheck(snapshot.undo==1&&snapshot.redo==0,"fixture setup","the seeded expression write remains the only shared history node");
    MorphHostVerifyExpression(f,snapshot,authored,nullptr,"before initial open");
    MorphHostCheck(MorphHostEditorWindows(f.app->MainWindow()).count==0,"before initial open","no host expression curve window exists before the first OpenMorph");
    HWND editor=MorphHostOpenExpression(f,"after initial open");
    MorphHostVerifyExpression(f,snapshot,authored,editor,"after initial open");
    MorphHostViewExpression(editor,"after only viewing");
    MorphHostVerifyExpression(f,snapshot,authored,editor,"after only viewing");
    MorphHostCloseExpression(f,editor,"after initial close");
    MorphHostVerifyExpression(f,snapshot,authored,nullptr,"after initial close");
    editor=MorphHostOpenExpression(f,"after reopen");
    MorphHostVerifyExpression(f,snapshot,authored,editor,"after reopen");
    MorphHostViewExpression(editor,"after repeat viewing");
    MorphHostVerifyExpression(f,snapshot,authored,editor,"after repeat viewing");
    MorphHostCloseExpression(f,editor,"after repeat close");
    MorphHostVerifyExpression(f,snapshot,authored,nullptr,"after repeat close");
    editor=MorphHostOpenExpression(f,"after repeated reopen");
    MorphHostVerifyExpression(f,snapshot,authored,editor,"after repeated reopen");
    MorphHostCloseExpression(f,editor,"after final close");
    MorphHostVerifyExpression(f,snapshot,authored,nullptr,"after final close");
    curve_editor::CloseOwned(f.app->MainWindow());
    MorphHostCheck(MorphHostEditorWindows(f.app->MainWindow()).count==0,"fixture teardown","CloseOwned leaves no expression curve window before fixture destruction");
}
void MorphCurveWriteRefusals(){
    {
        MorphCurveWriteFixture f;const auto input=f.Input();const auto before=f.Digest();auto change=f.Plan({input});
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly,8,1)==MMS_OK,"set tiny morph curve byte budget");std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("morph curves",change,transaction)==MMS_E_MEMORY&&!transaction&&f.Digest()==before,"morph byte-budget refusal leaves native pool author pose and journal intact");
    }
    for(unsigned reason=0;reason<5;++reason){MorphCurveWriteFixture f;const auto input=f.Input();std::shared_ptr<plugins::HistoryChange> change;
        if(reason==0)f.first.m->morphKeyCursors=reinterpret_cast<std::uint32_t*>(&f.firstMorphs[1].value);
        if(reason==1)f.first.m->morphTrackActive=reinterpret_cast<unsigned char*>(f.firstCursors.data());
        if(reason==2)f.second.m->morphs=f.first.m->morphs;
        if(reason==3)f.first.m->morphs=reinterpret_cast<mdl::MorphRecord*>(f.first.m->morphKeys);
        if(reason==4)f.first.m->boneTable=reinterpret_cast<mdl::BoneRecord*>(f.first.m->morphKeys);
        const auto before=f.Digest();const auto result=plugins::PlanMorphCurveWrite(*f.app,0,{input},change);const auto after=f.Digest();
        f.first.m->morphKeyCursors=f.firstCursors.data();f.first.m->morphTrackActive=f.firstActive.data();f.first.m->morphs=f.firstMorphs.data();f.second.m->morphs=f.secondMorphs.data();f.first.m->boneTable=f.first.bones.data();
        Check(result==MMS_E_NOT_READY&&!change&&before==after,"aliased morph pool pose tables cursors and other models are rejected without partial writes");
    }
    for(unsigned reason=0;reason<4;++reason){MorphCurveWriteFixture f;const auto input=f.Input();std::shared_ptr<plugins::HistoryChange> change;
        auto& model=*(reason%2?f.second.m:f.first.m);const auto oldChains=model.ikChains;const auto oldChainCount=model.ikChainCount;
        const auto oldOrder=model.boneOrderTable;const auto oldOrderCount=model.boneOrderCount;mdl::IkChain chain{};
        if(reason<2){chain.links=reinterpret_cast<std::uint16_t*>(reason%2?static_cast<void*>(&f.firstMorphs[1].value):static_cast<void*>(&f.first.m->morphKeys[0].value));
            chain.linkCount=1;model.ikChains=&chain;model.ikChainCount=1;}
        else{model.boneOrderTable=reinterpret_cast<mdl::BoneOrderEntry*>(reason%2?static_cast<void*>(f.firstCursors.data()):static_cast<void*>(&f.first.m->morphKeys[0].value));model.boneOrderCount=1;}
        const auto before=f.Digest();const auto result=plugins::PlanMorphCurveWrite(*f.app,0,{input},change);const auto after=f.Digest();
        model.ikChains=oldChains;model.ikChainCount=oldChainCount;model.boneOrderTable=oldOrder;model.boneOrderCount=oldOrderCount;
        Check(result==MMS_E_NOT_READY&&!change&&before==after,"own and peer IK links and bone evaluation tables cannot alias the morph pool weights or cursors");
    }
    for(unsigned reason=0;reason<3;++reason){MorphCurveWriteFixture f;auto input=f.Input();
        if(reason==0)input.track.keys[0].outSlope=1.e39;
        if(reason==1){input.track.keys[0].value=-3.e38;input.track.keys[1].value=3.e38;}
        if(reason==2){input.track.keys[0].value=1.e100;}
        f.app->CurrentFrame()=0;const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanMorphCurveWrite(*f.app,0,{input},change)==MMS_E_ARGUMENT&&!change&&f.Digest()==before,
            "morph controls future native delta and float storage overflow refuse even when the current frame is finite");
    }
    {
        MorphCurveWriteFixture f;auto input=f.Input();input.track.keys[0].value=-2;input.track.keys[1].value=3;
        f.Commit(f.Plan({input}));Check(f.history.UndoCount()==1,"safe negative and above-one morph weights remain supported");
    }
}
void MorphCurveWriteLifetimeAndStale(){
    {
        MorphCurveWriteFixture f;const auto before=MorphKeysCopy(f.first);f.Commit(f.Plan({f.Input()}));auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
        const auto identity=plugins::SceneObjectIdentity(f.first.raw());std::unique_ptr<plugins::SharedHistory::NativeTransaction> removal;
        Check(f.history.PrepareNative("remove model",owner->CaptureRemoval(4096),removal)==MMS_OK&&f.history.CommitNative(std::move(removal))==MMS_OK&&!f.app->ModelSlot(0),
            "remove a retained model after its morph curve edit");
        Check(f.history.History(false)==MMS_OK&&plugins::SceneObjectIdentity(f.first.raw()).generation!=identity.generation&&
            f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before),"morph curve history follows the restored stable model ID with a new generation");
        Check(f.history.History(true)==MMS_OK,"morph curve redo resolves its restored model and stable key pool");
    }
    for(unsigned reason=0;reason<3;++reason){MorphCurveWriteFixture f;auto change=f.Plan({f.Input()});
        if(reason==0)f.first.m->morphKeys[15].reserved[1]^=1;
        if(reason==1)f.firstMorphs[0].name[0]^=1;
        if(reason==2)f.app->CurrentFrame()=8;
        const auto before=f.Digest();std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.Digest()==before,
            "first morph source freezes unused pool bytes table identity and playhead before any publication");
    }
}
void MorphCurveWriteFailures(){
    unsigned failed=0;
    for(long allocation=0;allocation<2048;++allocation){MorphCurveWriteFixture f;const auto input=f.Input();std::string error;
        auto seed=f.Input(1);Check(morph_curve::Write(f.first.raw(),{seed},error),"seed unowned author data for morph failure sweep");morph_curve::AuthoredLayers raw;
        Check(morph_curve::CaptureLayers(f.first.raw(),raw,error),"capture complete layers before allocation injection");const auto before=f.Digest();
        std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;const std::vector<animation_curve::NamedTrack> payload{input};
        failAfter=allocation;auto result=plugins::PlanMorphCurveWrite(*f.app,0,payload,change);
        if(result==MMS_OK)result=plugins::Boundary([&]{return f.history.PrepareNative("morph curves",change,transaction);});failAfter=-1;
        Check(f.Digest()==before&&morph_curve::LayersUnchanged(f.first.raw(),raw)&&f.history.UndoCount()==0,"every morph allocation failure leaves full raw layers native pool current weights and history unchanged");
        if(result==MMS_OK){Check(transaction&&failed>20,"morph failure injection reaches a fully prepared author replacement");return;}
        Check(result==MMS_E_MEMORY&&!transaction,"morph preparation reports allocation failure without a publication token");++failed;
    }Check(false,"morph curve failure sweep terminates");
}
// ---------------------------------------------------------------------------
// Real OpenMorph expression window: MMD panel mouse drag persistence.
//
// The panel preview is editor-local: MmdMouseMove only rewrites the displayed
// 0..127 handles, while the host source and the shared history stay untouched.
// A release must publish exactly one native control edit for the interval that
// was displayed. Two positive controls (an endpoint key selected on the real
// canvas, one drag per handle) come first; the follow-playhead case without any
// key selection follows them.
// ---------------------------------------------------------------------------
UINT MorphHostDpi(HWND window){
    using GetDpiForWindowFn=UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getter=reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow")));
    if(getter&&window){const UINT dpi=getter(window);if(dpi>=48)return dpi;}
    HDC screen=GetDC(nullptr);
    const int dpi=screen?GetDeviceCaps(screen,LOGPIXELSX):96;
    if(screen)ReleaseDC(nullptr,screen);
    return dpi>=48?static_cast<UINT>(dpi):96;
}
struct MorphHostMmdGeometry {int margin=0;int side=0;};
MorphHostMmdGeometry MorphHostMmdGeometryOf(HWND editor,HWND& panel,const char* stage){
    MorphHostMmdGeometry geometry;
    panel=GetDlgItem(editor,1031);
    MorphHostCheck(panel!=nullptr&&IsWindow(panel),stage,"the real MMD panel exists inside the expression window");
    if(panel==nullptr)return geometry;
    RECT client={};
    GetClientRect(panel,&client);
    // Same square MmdSquare derives: a 6 logical pixel margin and the shorter
    // client side, scaled with the editor's own resolved DPI.
    const int dpi=static_cast<int>(MorphHostDpi(editor));
    geometry.margin=MulDiv(6,dpi,96);
    geometry.side=std::min(int(client.right),int(client.bottom))-2*geometry.margin;
    MorphHostCheck(geometry.side>0,stage,"the MMD panel owns a drawable handle square");
    return geometry;
}
POINT MorphHostMmdHandlePoint(const MorphHostMmdGeometry& geometry,double u,double v){
    POINT point={};
    point.x=geometry.margin+static_cast<LONG>(std::lround(u/127.0*double(geometry.side)));
    point.y=geometry.margin+geometry.side-static_cast<LONG>(std::lround(v/127.0*double(geometry.side)));
    return point;
}
std::array<double,4> MorphHostMmdDragControls(const MorphHostMmdGeometry& geometry,int handle,POINT point,
    const std::array<double,4>& start){
    std::array<double,4> controls=start;
    const double side=double(std::max(1,geometry.side));
    const double u=std::max(0.0,std::min(127.0,(point.x-geometry.margin)*127.0/side));
    const double v=std::max(0.0,std::min(127.0,(geometry.margin+geometry.side-point.y)*127.0/side));
    controls[std::size_t(handle)*2]=std::floor(u+0.5);
    controls[std::size_t(handle)*2+1]=std::floor(v+0.5);
    return controls;
}
// Read the coordinate fields SyncMmdCoordinateFields updates during the drag.
// The WM_APP+302 hook must not be used here: ComputeMmdControls would recompute
// the interval and overwrite the in-flight preview.
std::array<double,4> MorphHostMmdFields(HWND editor,const char* stage){
    std::array<double,4> controls{};
    for(int index=0;index<4;++index){
        const HWND field=GetDlgItem(editor,1096+index);
        wchar_t text[64]{};
        GetDlgItemTextW(editor,1096+index,text,64);
        wchar_t* end=nullptr;
        const double value=std::wcstod(text,&end);
        MorphHostCheck(field!=nullptr&&end!=text&&end!=nullptr&&*end==L'\0'&&std::isfinite(value),stage,
            "the MMD coordinate fields show a finite in-flight preview");
        controls[std::size_t(index)]=value;
    }
    return controls;
}
unsigned MorphHostNativeSelectionCount(MorphCurveWriteFixture& f){
    unsigned count=0;
    const auto capacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);
    for(std::size_t index=0;index<capacity;++index)if(f.first.m->morphKeys[index].allocated)++count;
    return count;
}
void MorphHostClearNativeSelection(MorphCurveWriteFixture& f){
    const auto capacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);
    for(std::size_t index=0;index<capacity;++index)f.first.m->morphKeys[index].allocated=0;
}
// Seed the straight two-key source the panel represents as diagonal handles.
// A prior overshoot seed could not enable the panel; this one must.
animation_curve::Track MorphHostSeedMmdDragCurve(MorphCurveWriteFixture& f,const char* stage){
    MorphHostCheck(plugins::BoundNativePoseHistory(f.app.get())==&f.history,stage,
        "the expression host regression binds the fixture's shared history");
    MorphHostCheck(f.app->MainWindow()!=nullptr&&GetDlgItem(f.app->MainWindow(),panel::kPhysicsFrameCheckbox)!=nullptr&&
        GetDlgItem(f.app->MainWindow(),panel::kUndoButton)!=nullptr&&GetDlgItem(f.app->MainWindow(),panel::kReadoutPosXEdit)!=nullptr,
        stage,"the hidden expression host owner has the common controls its publication touches");
    f.app->SelectedModelSlot()=0;f.app->CameraMode()=0;
    f.firstMorphs[0].panel=mdl::MorphPanel::eyebrow;
    MorphHostCheck(f.app->SelectedModel()==f.first.raw(),stage,"the first model is the selected model");
    for(auto& index:f.first.m->selectedMorphs)index=-1;
    f.first.m->selectedMorphs[0]=0;
    MorphHostCheck(morph_curve::PanelGroup(static_cast<std::uint8_t>(f.firstMorphs[0].panel))==0,stage,
        "the selected lane morph belongs to the panel eyebrow group");
    MorphHostCheck(!curve_host::MorphLaneInitial(*f.app,0).empty(),stage,
        "the panel eyebrow selector names a valid lane morph");
    const auto modelIdentity=curve_host::MorphIdentityOf(*f.app,f.first.raw());
    MorphHostCheck(curve_host::MorphIdentityCurrent(*f.app,modelIdentity),stage,
        "the model identity and selected slot agree");
    std::vector<curve_editor::Channel> baseline;std::wstring error;
    Check(curve_host::ReadMorphChannels(*f.app,baseline,error)&&baseline.size()==3,
        "read the pristine expression channels before the panel drag regression");
    Check(baseline[0].id==morph_curve::StableId(0,f.firstMorphs[0].name),
        "the panel eyebrow channel is the selected morph");
    animation_curve::Key first,last;
    first.frame=0;first.value=.1;last.frame=30;last.value=.9;
    first.interpolation=last.interpolation=animation_curve::Interpolation::Linear;
    first.tangent=last.tangent=animation_curve::Tangent::Free;
    animation_curve::Track seeded;
    seeded.keys={first,last};
    std::string controlsError;
    Check(animation_curve::SetMmdControls(seeded,1,{20,20,107,107},controlsError),
        "the detached seed expresses the diagonal 20 20 107 107 handles");
    Check(seeded.keys.back().interpolation==animation_curve::Interpolation::Linear,
        "diagonal MMD controls keep the seed a straight Linear interval");
    auto next=baseline;next[0].track=seeded;
    Check(curve_host::WriteMorphChannels(*f.app,modelIdentity,next,baseline,error),
        "the bound production expression write installs the seeded linear curve");
    Check(f.history.UndoCount()==1&&!f.history.RedoCount(),
        "the seeded expression write records exactly one shared history node");
    std::vector<curve_editor::Channel> installed;
    Check(curve_host::ReadMorphChannels(*f.app,installed,error)&&installed.size()==3&&
        MorphHostSameTrack(installed[0].track,seeded),
        "the bound source reads the seeded linear curve back bit for bit");
    Check(installed[0].track.keys.back().interpolation==animation_curve::Interpolation::Linear,
        "the seeded interval stays Linear before the drag");
    MorphHostClearNativeSelection(f);
    MorphHostCheck(MorphHostNativeSelectionCount(f)==0,stage,
        "native morph allocated selection marks are clear before the real OpenMorph");
    return seeded;
}
animation_curve::Track MorphHostMmdExpectedTrack(const animation_curve::Track& source,
    const std::array<double,4>& controls,const char* stage){
    animation_curve::Track expected=source;
    std::string error;
    MorphHostCheck(animation_curve::SetMmdControls(expected,1,controls,error),stage,
        "the retained source track expresses the dragged MMD controls");
    MorphHostCheck(expected.keys.size()==2&&expected.keys.back().interpolation==animation_curve::Interpolation::Bezier,
        stage,"off-diagonal MMD controls turn the interval into a real Bezier");
    return expected;
}
void MorphHostVerifyMmdDragCommitted(MorphCurveWriteFixture& f,HWND editor,
    const animation_curve::Track& expected,const char* stage){
    std::vector<curve_editor::Channel> channels;std::wstring error;
    const bool read=curve_host::ReadMorphChannels(*f.app,channels,error)&&channels.size()==3;
    MorphHostCheck(read,stage,"the committed expression channels can be read back");
    if(!read)return;
    const auto& track=channels[0].track;
    MorphHostCheck(MorphHostSameTrack(track,expected),stage,
        "the committed authored track carries the intended Bezier slopes and handle weights");
    MorphHostCheck(track.keys.size()==2&&track.keys.front().frame==0&&track.keys.back().frame==30,stage,
        "the committed curve keeps the two seeded frame keys");
    MorphHostCheck(track.keys.back().interpolation==animation_curve::Interpolation::Bezier,stage,
        "the committed endpoint interval is Bezier");
    MorphHostCheck(std::fabs(track.keys.front().value-.1)<1e-12&&std::fabs(track.keys.back().value-.9)<1e-12,stage,
        "the committed curve preserves the authored endpoint values");
    for(double frame:{5.0,10.0,15.0,20.0,25.0}){
        const auto wanted=animation_curve::Evaluate(expected,frame);
        const auto actual=animation_curve::Evaluate(track,frame);
        MorphHostCheck(std::fabs(actual.value-wanted.value)<=1e-9&&std::fabs(actual.derivative-wanted.derivative)<=1e-9,stage,
            "the committed interior samples match the retained source track");
        if(editor!=nullptr){
            std::array<double,3> values{{frame,0,0}};
            MorphHostCheck(SendMessageW(editor,WM_APP+396,0,reinterpret_cast<LPARAM>(values.data()))!=0,stage,
                "the expression editor answers its sampling message");
            MorphHostCheck(std::fabs(values[1]-wanted.value)<=1e-9&&std::fabs(values[2]-wanted.derivative)<=1e-9,stage,
                "the expression editor samples the committed value and derivative");
        }
    }
    MorphHostCheck(std::fabs(animation_curve::Evaluate(track,15.0).value-.5)>0.05,stage,
        "the committed interior departs from the straight seed segment");
}
void MorphCurveWriteHostMmdPanelDrag(bool selectEndKey){
    const char* label=selectEndKey?"selected-endpoint":"no-selection-follow-playhead";
    for(int handle=0;handle<2;++handle){
        std::printf("  morph curve write: MMD panel %s drag handle %d\n",label,handle);
        std::fflush(stdout);
        MorphCurveWriteFixture f;
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        const std::string stage=std::string("MMD panel ")+label+" handle "+std::to_string(handle);
        const animation_curve::Track seeded=MorphHostSeedMmdDragCurve(f,stage.c_str());
        HWND editor=MorphHostOpenExpression(f,stage.c_str());
        MorphHostCheck(editor!=nullptr,stage.c_str(),"the real OpenMorph expression window is live");
        HWND panel=nullptr;
        const auto geometry=MorphHostMmdGeometryOf(editor,panel,stage.c_str());
        if(editor==nullptr||geometry.side<=0){curve_editor::CloseOwned(f.app->MainWindow());continue;}
        if(selectEndKey){
            const auto packed=SendMessageW(editor,WM_APP+300,1,0);
            POINT keyPoint={static_cast<LONG>(static_cast<short>(LOWORD(packed))),
                static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
            MorphHostCheck(keyPoint.x>0&&keyPoint.y>0,stage.c_str(),
                "the endpoint key lies inside the real canvas");
            const HWND canvas=GetDlgItem(editor,1020);
            MorphHostCheck(canvas!=nullptr,stage.c_str(),"the real expression canvas exists");
            if(canvas!=nullptr){
                SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(keyPoint.x,keyPoint.y));
                SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(keyPoint.x,keyPoint.y));
            }
            MorphHostCheck(f.app->CurrentFrame()==30,stage.c_str(),
                "the real canvas click selects the endpoint and seeks to frame 30");
            MorphHostCheck(MorphHostNativeSelectionCount(f)==1,stage.c_str(),
                "the positive control leaves exactly the clicked endpoint selected");
        }else{
            const HWND timeline=GetDlgItem(editor,1024);const HWND seek=GetDlgItem(editor,1025);
            MorphHostCheck(timeline!=nullptr&&seek!=nullptr,stage.c_str(),
                "the real host timeline seek controls exist");
            if(timeline!=nullptr&&seek!=nullptr){
                SetWindowTextW(timeline,L"10");
                SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1025,BN_CLICKED),reinterpret_cast<LPARAM>(seek));
            }
            MorphHostCheck(f.app->CurrentFrame()==10,stage.c_str(),
                "the real host seek control moves the app frame to frame 10");
            MorphHostCheck(MorphHostNativeSelectionCount(f)==0,stage.c_str(),
                "no canvas click leaves every native morph selection mark empty");
        }
        std::array<double,4> before{};
        const BOOL enabled=static_cast<BOOL>(SendMessageW(editor,WM_APP+302,0,reinterpret_cast<LPARAM>(before.data())));
        MorphHostCheck(enabled!=FALSE,stage.c_str(),
            "the follow-playhead MMD panel enables its handles for the active interval");
        MorphHostCheck(before==std::array<double,4>{20,20,107,107},stage.c_str(),
            "the straight seed displays the diagonal 20 20 107 107 handles");
        const POINT pressed=MorphHostMmdHandlePoint(geometry,before[std::size_t(handle)*2],
            before[std::size_t(handle)*2+1]);
        const POINT targetPoint=handle==0?MorphHostMmdHandlePoint(geometry,35,95):MorphHostMmdHandlePoint(geometry,95,35);
        const LPARAM target=MAKELPARAM(targetPoint.x,targetPoint.y);
        const auto expectedControls=MorphHostMmdDragControls(geometry,handle,targetPoint,before);
        MorphHostCheck(!(expectedControls[0]==expectedControls[1]&&expectedControls[2]==expectedControls[3]),stage.c_str(),
            "the drag target is unambiguously off the diagonal");
        MorphHostSnapshot snapshot(f);
        MorphHostCheck(snapshot.undo==1&&snapshot.redo==0,stage.c_str(),
            "the seeded expression write is the only shared history node before the drag");
        SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(pressed.x,pressed.y));
        MorphHostCheck(GetCapture()==panel,stage.c_str(),
            "the real panel press acquires the MMD capture");
        SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,target);
        MorphHostCheck(GetCapture()==panel,stage.c_str(),
            "the in-flight MMD drag keeps the panel capture");
        const auto preview=MorphHostMmdFields(editor,stage.c_str());
        MorphHostCheck(preview==expectedControls,stage.c_str(),
            "the in-flight coordinate fields show the quantized dragged handle");
        MorphHostCheck(snapshot.Matches(f),stage.c_str(),
            "native keys model weights and authored layers are untouched during the drag");
        std::uint64_t revision=0;
        MorphHostCheck(f.history.Revision(revision)==MMS_OK&&revision==snapshot.revision&&
            f.history.UndoCount()==snapshot.undo&&f.history.RedoCount()==snapshot.redo,stage.c_str(),
            "the shared history records nothing while the handle is in flight");
        std::vector<curve_editor::Channel> during;std::wstring duringError;
        MorphHostCheck(curve_host::ReadMorphChannels(*f.app,during,duringError)&&during.size()==3&&
            MorphHostSameTrack(during[0].track,seeded)&&
            during[0].track.keys.back().interpolation==animation_curve::Interpolation::Linear,stage.c_str(),
            "the bound source still returns the unchanged Linear seed during the drag");
        SendMessageW(panel,WM_LBUTTONUP,0,target);
        MorphHostCheck(GetCapture()!=panel,stage.c_str(),
            "the real panel release returns the MMD capture");
        MorphHostCheck(f.history.Revision(revision)==MMS_OK&&revision==snapshot.revision+1&&
            f.history.UndoCount()==snapshot.undo+1,stage.c_str(),
            "the finished drag publishes exactly one shared history node");
        const auto expected=MorphHostMmdExpectedTrack(seeded,preview,stage.c_str());
        MorphHostVerifyMmdDragCommitted(f,editor,expected,stage.c_str());
        MorphHostCloseExpression(f,editor,stage.c_str());
        editor=MorphHostOpenExpression(f,stage.c_str());
        MorphHostVerifyMmdDragCommitted(f,editor,expected,stage.c_str());
        MorphHostCloseExpression(f,editor,stage.c_str());
        curve_editor::CloseOwned(f.app->MainWindow());
        MorphHostCheck(MorphHostEditorWindows(f.app->MainWindow()).count==0,stage.c_str(),
            "CloseOwned leaves no expression curve window after the panel drag case");
    }
}
// ---------------------------------------------------------------------------
// Real OpenMorph numeric MMD coordinate Enter regression.
//
// The four coordinate fields edit the interval the panel displays. A is the
// selected/displayed same-interval positive control: the clicked 0..30
// endpoint owns the interval the panel shows. B has no selected key and
// follows the playhead, so the displayed 0..30 interval is the only target.
// C keeps the frame-10 key selected but moves the playhead to 20, so the
// displayed 10..30 interval is the target and the untouched 0..10 interval
// must survive. Adjacent-handle linking is switched off in all three so an
// unrelated-interval assertion is meaningful.
// ---------------------------------------------------------------------------
struct MorphHostHistoryMarks {
    std::uint64_t revision = 0;
    std::size_t undo = 0;
    std::size_t redo = 0;
};
MorphHostHistoryMarks MorphHostHistoryMarksOf(MorphCurveWriteFixture& f,const char* stage){
    MorphHostHistoryMarks marks;
    MorphHostCheck(f.history.Revision(marks.revision)==MMS_OK,stage,"read the shared expression history revision");
    marks.undo=f.history.UndoCount();
    marks.redo=f.history.RedoCount();
    return marks;
}
void MorphHostRequireOneNewHistory(MorphCurveWriteFixture& f,const MorphHostHistoryMarks& before,const char* stage){
    std::uint64_t revision=0;
    MorphHostCheck(f.history.Revision(revision)==MMS_OK&&revision==before.revision+1&&
        f.history.UndoCount()==before.undo+1&&f.history.RedoCount()==before.redo,stage,
        "the MMD coordinate Enter publishes exactly one shared history node and leaves redo untouched");
}
void MorphHostRequireHistoryUnchanged(MorphCurveWriteFixture& f,const MorphHostHistoryMarks& marks,const char* stage){
    std::uint64_t revision=0;
    MorphHostCheck(f.history.Revision(revision)==MMS_OK&&revision==marks.revision&&
        f.history.UndoCount()==marks.undo&&f.history.RedoCount()==marks.redo,stage,
        "the shared expression history is unchanged after close and reopen");
}
void MorphHostRequireAdjacentLinkingOff(MorphCurveWriteFixture& f,const char* stage){
    const HWND checkbox=GetDlgItem(f.app->MainWindow(),panel::kPhysicsFrameCheckbox);
    MorphHostCheck(checkbox!=nullptr,stage,"the hidden host owner has the adjacent-handle linking checkbox");
    if(checkbox!=nullptr)SendMessageW(checkbox,BM_SETCHECK,BST_UNCHECKED,0);
    MorphHostCheck(IsDlgButtonChecked(f.app->MainWindow(),panel::kPhysicsFrameCheckbox)==BST_UNCHECKED,stage,
        "adjacent-handle linking is unchecked so the displayed interval is the only edit");
}
// The coordinate fields are real WS_CHILD controls created directly on the
// expression editor (MakeControl creates them with the built-in EDIT class).
// The runtime class string of a built-in control is not an authored contract
// and its capitalisation is not guaranteed, so it is logged for diagnosis and
// never asserted. Identity is the exact control id, the direct editor parent,
// IsChild and WS_CHILD; the class query only feeds the diagnostics.
bool MorphHostMmdCoordinateField(HWND editor,int id,const char* stage,const char* role){
    const HWND field=editor?GetDlgItem(editor,id):nullptr;
    wchar_t className[64]{};
    const int classLength=field?GetClassNameW(field,className,64):0;
    const int actualId=field?GetDlgCtrlID(field):0;
    const HWND actualParent=field?GetParent(field):nullptr;
    const bool windowOk=field!=nullptr&&IsWindow(field)!=FALSE;
    const bool idOk=windowOk&&actualId==id;
    const bool parentOk=windowOk&&actualParent==editor;
    const bool isChild=windowOk&&IsChild(editor,field)!=FALSE;
    const bool styleChild=windowOk&&(GetWindowLongPtrW(field,GWL_STYLE)&WS_CHILD)!=0;
    const bool identity=windowOk&&idOk&&parentOk&&isChild&&styleChild;
    if(!identity){
        std::printf("  MMD coordinate %s identity: stage=%s expectedId=%d actualId=%d hwnd=%p class=%ls parent=%p expectedParent=%p isChild=%d styleChild=%d\n",
            role,stage,id,actualId,reinterpret_cast<void*>(field),
            classLength>0?className:L"(none)",reinterpret_cast<void*>(actualParent),
            reinterpret_cast<void*>(editor),isChild?1:0,styleChild?1:0);
        std::fflush(stdout);
    }
    MorphHostCheck(identity,stage,"the MMD coordinate field is a real WS_CHILD control owned directly by the expression editor");
    return identity;
}
void MorphHostTypeMmdCoordinates(HWND editor,const std::array<int,4>& values,const char* stage){
    for(int index=0;index<4;++index){
        const int id=1096+index;
        if(!MorphHostMmdCoordinateField(editor,id,stage,"coordinate field"))return;
        const HWND field=GetDlgItem(editor,id);
        const auto text=std::to_wstring(values[std::size_t(index)]);
        SetWindowTextW(field,text.c_str());
        wchar_t shown[32]{};
        GetDlgItemTextW(editor,id,shown,32);
        if(std::wstring(shown)!=text){
            std::printf("  MMD coordinate field text roundtrip: stage=%s id=%d hwnd=%p expected=%ls actual=%ls\n",
                stage,id,reinterpret_cast<void*>(field),text.c_str(),shown);
            std::fflush(stdout);
        }
        MorphHostCheck(std::wstring(shown)==text,stage,"the MMD coordinate field shows the typed integer");
    }
}
void MorphHostPressMmdCoordinateEnter(HWND editor,const char* stage){
    if(!MorphHostMmdCoordinateField(editor,1096,stage,"Enter target"))return;
    const HWND field=GetDlgItem(editor,1096);
    wchar_t className[64]{};
    GetClassNameW(field,className,64);
    std::printf("  MMD coordinate Enter child: stage=%s id=%d hwnd=%p class=%ls parent=%p editor=%p\n",
        stage,GetDlgCtrlID(field),reinterpret_cast<void*>(field),
        className[0]?className:L"(none)",reinterpret_cast<void*>(GetParent(field)),
        reinterpret_cast<void*>(editor));
    std::fflush(stdout);
    SendMessageW(field,WM_KEYDOWN,VK_RETURN,0);
}
void MorphHostRequireMmdReadout(HWND editor,const std::wstring& expected,const char* stage){
    wchar_t text[128]{};
    GetDlgItemTextW(editor,1032,text,128);
    MorphHostCheck(std::wstring(text)==expected,stage,"the MMD readout names the displayed interval");
}
std::vector<int> MorphHostMorphSelectionFrames(MorphCurveWriteFixture& f,unsigned morph=0){
    std::vector<int> frames;
    const auto capacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);
    if(morph>=capacity)return frames;
    std::size_t slot=morph;
    for(std::size_t visited=0;visited<capacity;++visited){
        if(slot>=capacity)return {};
        const auto& key=f.first.m->morphKeys[slot];
        if(key.allocated)frames.push_back(static_cast<int>(key.frame));
        if(!key.next)break;
        slot=key.next;
    }
    return frames;
}
void MorphHostRequireOtherTracks(const std::vector<curve_editor::Channel>& before,
    const std::vector<curve_editor::Channel>& after,const char* stage){
    MorphHostCheck(before.size()==after.size()&&before.size()>=3,stage,"the expression channel list is stable");
    for(std::size_t index=1;index<before.size()&&index<after.size();++index)
        MorphHostCheck(before[index].id==after[index].id&&MorphHostSameTrack(before[index].track,after[index].track),stage,
            "the other expression tracks keep their key frames values and handle geometry");
}
animation_curve::Track MorphHostMmdExpectedInterval(const animation_curve::Track& source,std::size_t endKey,
    const std::array<double,4>& controls,const char* stage){
    animation_curve::Track expected=source;
    std::string error;
    MorphHostCheck(animation_curve::SetMmdControls(expected,endKey,controls,error),stage,
        "the retained source track expresses the typed MMD controls on the displayed interval");
    MorphHostCheck(endKey>0&&endKey<expected.keys.size()&&
        expected.keys[endKey].interpolation==animation_curve::Interpolation::Bezier,stage,
        "the typed off-diagonal controls turn the displayed interval into a real Bezier");
    return expected;
}
void MorphHostVerifyMmdCoordinateTrack(MorphCurveWriteFixture& f,const animation_curve::Track& source,
    const animation_curve::Track& expected,HWND editor,const char* stage){
    std::vector<curve_editor::Channel> channels;std::wstring error;
    const bool read=curve_host::ReadMorphChannels(*f.app,channels,error)&&channels.size()==3;
    MorphHostCheck(read,stage,"the committed expression channels can be read back");
    if(!read)return;
    const auto& track=channels[0].track;
    MorphHostCheck(MorphHostSameTrack(track,expected),stage,
        "the committed authored track equals the retained source and SetMmdControls oracle");
    MorphHostCheck(track.keys.size()==source.keys.size(),stage,"the committed curve keeps the seeded key count");
    if(track.keys.size()==source.keys.size()){
        for(std::size_t index=0;index<track.keys.size();++index)
            MorphHostCheck(track.keys[index].frame==source.keys[index].frame&&
                track.keys[index].value==source.keys[index].value,stage,
                "the committed curve keeps the seeded frame and value of every endpoint");
        MorphHostCheck(track.keys[0].outWeight==source.keys[0].outWeight&&track.keys[0].outSlope==source.keys[0].outSlope&&
            track.keys[1].inWeight==source.keys[1].inWeight&&track.keys[1].inSlope==source.keys[1].inSlope,stage,
            "the first interval keeps its out and in handle geometry unchanged");
        for(double frame:{2.0,5.0,8.0}){
            const auto before=animation_curve::Evaluate(source,frame);
            const auto after=animation_curve::Evaluate(track,frame);
            MorphHostCheck(std::fabs(after.value-before.value)<=1e-12&&std::fabs(after.derivative-before.derivative)<=1e-12,stage,
                "the untouched first interval samples exactly as before the second-interval edit");
        }
    }
    for(double frame:{5.0,10.0,15.0,20.0,25.0,28.0}){
        const auto wanted=animation_curve::Evaluate(expected,frame);
        const auto actual=animation_curve::Evaluate(track,frame);
        MorphHostCheck(std::fabs(actual.value-wanted.value)<=1e-9&&std::fabs(actual.derivative-wanted.derivative)<=1e-9,stage,
            "the committed interior samples match the retained source track");
        if(editor!=nullptr){
            std::array<double,3> values{{frame,0,0}};
            MorphHostCheck(SendMessageW(editor,WM_APP+396,0,reinterpret_cast<LPARAM>(values.data()))!=0,stage,
                "the expression editor answers its sampling message");
            MorphHostCheck(std::fabs(values[1]-wanted.value)<=1e-9&&std::fabs(values[2]-wanted.derivative)<=1e-9,stage,
                "the expression editor samples the committed value and derivative");
        }
    }
}
void MorphCurveWriteHostMmdCoordinateEnter(bool selectEndKey){
    const char* label=selectEndKey?"selected-endpoint":"no-selection-follow-playhead";
    std::printf("  morph curve write: MMD coordinate Enter %s\n",label);std::fflush(stdout);
    MorphCurveWriteFixture f;
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    const std::string stage=std::string("MMD coordinate Enter ")+label;
    f.app->EnglishUI()=1;
    MorphHostRequireAdjacentLinkingOff(f,stage.c_str());
    const animation_curve::Track seeded=MorphHostSeedMmdDragCurve(f,stage.c_str());
    HWND editor=MorphHostOpenExpression(f,stage.c_str());
    MorphHostCheck(editor!=nullptr,stage.c_str(),"the real OpenMorph expression window is live");
    if(editor==nullptr){curve_editor::CloseOwned(f.app->MainWindow());return;}
    if(selectEndKey){
        const auto packed=SendMessageW(editor,WM_APP+300,1,0);
        POINT keyPoint={static_cast<LONG>(static_cast<short>(LOWORD(packed))),
            static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
        MorphHostCheck(keyPoint.x>0&&keyPoint.y>0,stage.c_str(),"the endpoint key lies inside the real canvas");
        const HWND canvas=GetDlgItem(editor,1020);
        MorphHostCheck(canvas!=nullptr,stage.c_str(),"the real expression canvas exists");
        if(canvas!=nullptr){
            SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(keyPoint.x,keyPoint.y));
            SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(keyPoint.x,keyPoint.y));
        }
        MorphHostCheck(f.app->CurrentFrame()==30,stage.c_str(),"the real canvas click selects the endpoint and seeks to frame 30");
        MorphHostCheck(MorphHostMorphSelectionFrames(f)==std::vector<int>{30},stage.c_str(),
            "the positive control leaves exactly the clicked endpoint selected");
    }else{
        const HWND timeline=GetDlgItem(editor,1024);
        const HWND seek=GetDlgItem(editor,1025);
        MorphHostCheck(timeline!=nullptr&&seek!=nullptr,stage.c_str(),"the real host timeline seek controls exist");
        if(timeline!=nullptr&&seek!=nullptr){
            SetWindowTextW(timeline,L"10");
            SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1025,BN_CLICKED),reinterpret_cast<LPARAM>(seek));
        }
        MorphHostCheck(f.app->CurrentFrame()==10,stage.c_str(),"the real host seek control moves the app frame to frame 10");
        MorphHostCheck(MorphHostNativeSelectionCount(f)==0&&MorphHostMorphSelectionFrames(f).empty(),stage.c_str(),
            "no canvas click leaves every native morph selection mark empty");
    }
    MorphHostRequireMmdReadout(editor,std::wstring(L"Range 0")+L"\u2013"+L"30",stage.c_str());
    std::array<double,4> displayed{};
    const BOOL enabled=static_cast<BOOL>(SendMessageW(editor,WM_APP+302,0,reinterpret_cast<LPARAM>(displayed.data())));
    MorphHostCheck(enabled!=FALSE,stage.c_str(),"the MMD panel enables the displayed 0..30 interval");
    MorphHostCheck(displayed==std::array<double,4>{20,20,107,107},stage.c_str(),
        "the displayed 0..30 interval keeps the seeded diagonal handles");
    std::vector<curve_editor::Channel> tracks;std::wstring trackError;
    Check(curve_host::ReadMorphChannels(*f.app,tracks,trackError)&&tracks.size()==3,
        "read the expression tracks before the coordinate Enter");
    const auto marks=MorphHostHistoryMarksOf(f,stage.c_str());
    MorphHostTypeMmdCoordinates(editor,std::array<int,4>{35,95,107,107},stage.c_str());
    MorphHostPressMmdCoordinateEnter(editor,stage.c_str());
    const auto expected=MorphHostMmdExpectedTrack(seeded,std::array<double,4>{35,95,107,107},stage.c_str());
    MorphHostRequireOneNewHistory(f,marks,stage.c_str());
    MorphHostVerifyMmdDragCommitted(f,editor,expected,stage.c_str());
    std::vector<curve_editor::Channel> after;std::wstring afterError;
    Check(curve_host::ReadMorphChannels(*f.app,after,afterError)&&after.size()==3,
        "read the expression tracks after the coordinate Enter");
    MorphHostRequireOtherTracks(tracks,after,stage.c_str());
    const auto committed=MorphHostHistoryMarksOf(f,stage.c_str());
    MorphHostCloseExpression(f,editor,stage.c_str());
    MorphHostRequireHistoryUnchanged(f,committed,stage.c_str());
    editor=MorphHostOpenExpression(f,stage.c_str());
    MorphHostVerifyMmdDragCommitted(f,editor,expected,stage.c_str());
    MorphHostRequireHistoryUnchanged(f,committed,stage.c_str());
    MorphHostCloseExpression(f,editor,stage.c_str());
    curve_editor::CloseOwned(f.app->MainWindow());
    MorphHostCheck(MorphHostEditorWindows(f.app->MainWindow()).count==0,stage.c_str(),
        "CloseOwned leaves no expression curve window after the coordinate Enter case");
}
void MorphCurveWriteHostMmdCoordinateEnterDifferentInterval(){
    std::printf("  morph curve write: MMD coordinate Enter selected-key different interval\n");std::fflush(stdout);
    MorphCurveWriteFixture f;
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    const char* stage="MMD coordinate Enter selected-key different interval";
    f.app->EnglishUI()=1;
    MorphHostRequireAdjacentLinkingOff(f,stage);
    MorphHostSeedMmdDragCurve(f,stage);
    std::vector<curve_editor::Channel> baseline;std::wstring error;
    Check(curve_host::ReadMorphChannels(*f.app,baseline,error)&&baseline.size()==3,
        "read the seeded expression channels before the three-key interval setup");
    const auto identity=curve_host::MorphIdentityOf(*f.app,f.first.raw());
    MorphHostCheck(curve_host::MorphIdentityCurrent(*f.app,identity),stage,
        "the three-key seed still targets the selected model");
    animation_curve::Key first,middle,last;
    first.frame=0;first.value=.1;middle.frame=10;middle.value=.5;last.frame=30;last.value=.9;
    first.interpolation=middle.interpolation=last.interpolation=animation_curve::Interpolation::Linear;
    first.tangent=middle.tangent=last.tangent=animation_curve::Tangent::Free;
    animation_curve::Track seeded;seeded.keys={first,middle,last};
    std::string controlsError;
    Check(animation_curve::SetMmdControls(seeded,1,{20,20,107,107},controlsError)&&
        animation_curve::SetMmdControls(seeded,2,{30,30,100,100},controlsError),
        "the detached three-key seed expresses both legal diagonal MMD intervals");
    Check(seeded.keys[1].interpolation==animation_curve::Interpolation::Linear&&
        seeded.keys[2].interpolation==animation_curve::Interpolation::Linear,
        "both diagonal MMD intervals stay straight Linear seeds");
    auto next=baseline;next[0].track=seeded;
    Check(curve_host::WriteMorphChannels(*f.app,identity,next,baseline,error),
        "the bound production expression write installs the three-key seed");
    MorphHostClearNativeSelection(f);
    MorphHostCheck(MorphHostNativeSelectionCount(f)==0,stage,
        "native morph allocated selection marks are clear before the real OpenMorph");
    std::vector<curve_editor::Channel> installed;
    Check(curve_host::ReadMorphChannels(*f.app,installed,error)&&installed.size()==3&&
        MorphHostSameTrack(installed[0].track,seeded),
        "the bound source reads the three-key seed back bit for bit");
    {
        std::array<double,4> intervalOne{},intervalTwo{};
        Check(animation_curve::GetMmdControls(installed[0].track,1,intervalOne)&&
            intervalOne==std::array<double,4>{20,20,107,107}&&
            animation_curve::GetMmdControls(installed[0].track,2,intervalTwo)&&
            intervalTwo==std::array<double,4>{30,30,100,100},
            "the two seeded intervals expose distinguishable diagonal MMD controls");
    }
    HWND editor=MorphHostOpenExpression(f,stage);
    MorphHostCheck(editor!=nullptr,stage,"the real OpenMorph expression window is live");
    if(editor==nullptr){curve_editor::CloseOwned(f.app->MainWindow());return;}
    const auto packed=SendMessageW(editor,WM_APP+300,1,0);
    POINT keyPoint={static_cast<LONG>(static_cast<short>(LOWORD(packed))),
        static_cast<LONG>(static_cast<short>(HIWORD(packed)))};
    MorphHostCheck(keyPoint.x>0&&keyPoint.y>0,stage,"the frame 10 key lies inside the real canvas");
    const HWND canvas=GetDlgItem(editor,1020);
    MorphHostCheck(canvas!=nullptr,stage,"the real expression canvas exists");
    if(canvas!=nullptr){
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(keyPoint.x,keyPoint.y));
        SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(keyPoint.x,keyPoint.y));
    }
    MorphHostCheck(f.app->CurrentFrame()==10,stage,"the real canvas click selects frame 10 and seeks there");
    MorphHostCheck(MorphHostMorphSelectionFrames(f)==std::vector<int>{10},stage,
        "exactly the clicked frame 10 key is selected");
    const HWND timeline=GetDlgItem(editor,1024);
    const HWND seek=GetDlgItem(editor,1025);
    MorphHostCheck(timeline!=nullptr&&seek!=nullptr,stage,"the real host timeline seek controls exist");
    if(timeline!=nullptr&&seek!=nullptr){
        SetWindowTextW(timeline,L"20");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1025,BN_CLICKED),reinterpret_cast<LPARAM>(seek));
    }
    MorphHostCheck(f.app->CurrentFrame()==20,stage,"the real host seek moves the playhead to frame 20 without a canvas click");
    MorphHostCheck(MorphHostMorphSelectionFrames(f)==std::vector<int>{10},stage,
        "the playhead move leaves only the frame 10 key selected");
    MorphHostRequireMmdReadout(editor,std::wstring(L"Range 10")+L"\u2013"+L"30",stage);
    std::array<double,4> displayed{};
    const BOOL enabled=static_cast<BOOL>(SendMessageW(editor,WM_APP+302,0,reinterpret_cast<LPARAM>(displayed.data())));
    MorphHostCheck(enabled!=FALSE,stage,"the MMD panel enables the displayed 10..30 interval");
    MorphHostCheck(displayed==std::array<double,4>{30,30,100,100},stage,
        "the MMD panel displays the second interval's diagonal controls");
    std::vector<curve_editor::Channel> tracks;std::wstring trackError;
    Check(curve_host::ReadMorphChannels(*f.app,tracks,trackError)&&tracks.size()==3,
        "read the expression tracks before the second-interval coordinate Enter");
    const auto marks=MorphHostHistoryMarksOf(f,stage);
    MorphHostTypeMmdCoordinates(editor,std::array<int,4>{35,95,107,107},stage);
    MorphHostPressMmdCoordinateEnter(editor,stage);
    const auto expected=MorphHostMmdExpectedInterval(seeded,2,std::array<double,4>{35,95,107,107},stage);
    MorphHostRequireOneNewHistory(f,marks,stage);
    MorphHostVerifyMmdCoordinateTrack(f,seeded,expected,editor,stage);
    std::vector<curve_editor::Channel> after;std::wstring afterError;
    Check(curve_host::ReadMorphChannels(*f.app,after,afterError)&&after.size()==3,
        "read the expression tracks after the second-interval coordinate Enter");
    MorphHostRequireOtherTracks(tracks,after,stage);
    const auto committed=MorphHostHistoryMarksOf(f,stage);
    MorphHostCloseExpression(f,editor,stage);
    MorphHostRequireHistoryUnchanged(f,committed,stage);
    editor=MorphHostOpenExpression(f,stage);
    MorphHostVerifyMmdCoordinateTrack(f,seeded,expected,editor,stage);
    MorphHostRequireHistoryUnchanged(f,committed,stage);
    MorphHostCloseExpression(f,editor,stage);
    curve_editor::CloseOwned(f.app->MainWindow());
    MorphHostCheck(MorphHostEditorWindows(f.app->MainWindow()).count==0,stage,
        "CloseOwned leaves no expression curve window after the second-interval coordinate case");
}
void NativeMorphCurveWriteChecks(){
    std::puts("  morph curve write: legacy mirror export and common history parity");MorphCurveWriteParity();
    std::puts("  morph curve write: raw layer restoration and private effective sampling");MorphCurveWriteRawLayers();
    std::puts("  morph curve write: pool growth root and multi-morph selection");MorphCurveWriteGrowthAndSelection();
    std::puts("  morph curve write: mixed native history and unowned author preservation");MorphCurveWriteMixedHistory();
    std::puts("  morph curve write: real expression host close and reopen");MorphCurveWriteHostCloseReopen();
    std::puts("  morph curve write: real OpenMorph MMD panel selected-endpoint drag");MorphCurveWriteHostMmdPanelDrag(true);
    std::puts("  morph curve write: real OpenMorph MMD panel no-selection follow-playhead drag");MorphCurveWriteHostMmdPanelDrag(false);
    std::puts("  morph curve write: budgets aliasing finite evaluation and lifetime");MorphCurveWriteRefusals();MorphCurveWriteLifetimeAndStale();
    std::puts("  morph curve write: full private preparation allocation sweep");MorphCurveWriteFailures();
    std::puts("  morph curve write: real OpenMorph MMD coordinate Enter selected-endpoint");MorphCurveWriteHostMmdCoordinateEnter(true);
    std::puts("  morph curve write: real OpenMorph MMD coordinate Enter no-selection follow-playhead");MorphCurveWriteHostMmdCoordinateEnter(false);
    std::puts("  morph curve write: real OpenMorph MMD coordinate Enter selected-key different interval");MorphCurveWriteHostMmdCoordinateEnterDifferentInterval();
}
}
