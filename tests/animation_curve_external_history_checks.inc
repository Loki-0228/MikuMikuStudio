// Host journal stub: the shared editor must not own another copy of its history.
struct ExternalCurveJournal {
    struct Node { unsigned source;std::vector<ce::Channel> before,after; };
    HostSource sources[2];std::vector<Node> nodes;
    std::size_t cursor=0;std::uint64_t revision=1;
    unsigned applies=0,traversals=0,queries=0,motions=0;
    bool failQuery=false,failTraversal=false,failApply=false,readAfterTraversal=false,readAfterApply=false,queryAfterApply=false,hideUndo=false;
    HWND window=nullptr;
    std::function<void()> onQuery,onApply,onRead;
    ExternalCurveJournal(){
        for(unsigned index=0;index<2;++index)sources[index].data.push_back(
            MakeChannel(index?"y":"x",index?L"Y":L"X",{MakeKey(0,0),MakeKey(10,5)}));
    }
    void Push(unsigned source,const std::vector<ce::Channel>& next){
        nodes.resize(cursor);nodes.push_back({source,sources[source].data,next});
        sources[source].data=next;cursor=nodes.size();++revision;
    }
    ce::ExternalHistory Bridge(){
        ce::ExternalHistory bridge;
        bridge.query=[this](ce::HistoryState& state,std::wstring& error){
            ++queries;if(onQuery)onQuery();
            if(failQuery){error=L"query unavailable";return false;}
            state={revision,cursor!=0&&!hideUndo,cursor<nodes.size()};return true;
        };
        bridge.traverse=[this](bool redo,std::wstring& error){
            if(failTraversal){error=L"mutation gate busy";return false;}
            if(redo?cursor==nodes.size():!cursor){error=L"empty host history";return false;}
            auto& node=nodes[redo?cursor:cursor-1];sources[node.source].data=redo?node.after:node.before;
            if(redo)++cursor;else --cursor;++revision;++traversals;
            if(readAfterTraversal)for(auto& source:sources)source.failRead=true;
            return true;
        };
        return bridge;
    }
    ce::Session Session(){
        ce::Session session;session.id="external-journal";session.language=2;session.history=Bridge();
        for(unsigned index=0;index<2;++index){
            auto source=sources[index].Make(index?"second":"first",index?L"Second":L"First");
            source.externalHistory=true;
            const auto read=source.read;
            source.read=[this,read](auto& channels,auto& error){const bool result=read(channels,error);if(onRead)onRead();return result;};
            source.apply=[this,index](const auto& next,std::wstring& error){
                ++applies;
                if(GetCapture()){error=L"capture blocks publication";return false;}
                if(onApply)onApply();
                if(failApply){error=L"write refused";return false;}
                Push(index,next);if(readAfterApply)sources[index].failRead=true;if(queryAfterApply)failQuery=true;return true;
            };
            source.readMotion=[this,index](void*,std::wstring&){
                ++motions;auto next=sources[index].data;
                next[0].track.keys.push_back(MakeKey(50,8));Push(index,next);return true;
            };
            session.sources.push_back(std::move(source));
        }
        return session;
    }
    HWND Open(){window=static_cast<HWND>(ce::Open(nullptr,Session(),false));return window;}
    void Select(unsigned source){
        SendMessageW(GetDlgItem(window,kIdSourceCombo),CB_SETCURSEL,source,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdSourceCombo,CBN_SELCHANGE),0);
    }
    void Tick(){
        const auto style=GetWindowLongPtrW(window,GWL_STYLE);
        SetWindowLongPtrW(window,GWL_STYLE,style|WS_VISIBLE);
        SendMessageW(window,WM_TIMER,kPlayheadTimerId,0);
        SetWindowLongPtrW(window,GWL_STYLE,style);
    }
    void Add(int frame,double value){
        SetText(window,kIdFrameEdit,std::to_wstring(frame).c_str());SetText(window,kIdValueEdit,std::to_wstring(value).c_str());Click(window,kIdAddKey);
    }
    void ReadOnly(bool expected){Check(bool(IsWindowEnabled(GetDlgItem(window,kIdAddKey)))!=expected,"external refresh state controls authoring availability");}
    void NoLocalHistory(){
        std::uint64_t state[5]{};Check(SendMessageW(window,WM_APP+397,0,reinterpret_cast<LPARAM>(state))!=0,"read external editor history state");
        Check(state[0]==0&&state[1]==0,"external sources retain no local undo or redo snapshots");
    }
    ~ExternalCurveJournal(){ce::CloseOwned(nullptr);}
};
void ExternalHistoryAcrossSources(){
    ExternalCurveJournal journal;HWND window=journal.Open();
    Check(!IsWindowEnabled(GetDlgItem(window,kIdUndo))&&!IsWindowEnabled(GetDlgItem(window,kIdRedo)),"empty host journal disables history buttons");
    journal.Add(20,3);Check(journal.cursor==1&&KeyCount(window)==3,"first source publishes once to host history");
    journal.Select(1);journal.Add(30,7);Check(journal.cursor==2&&KeyCount(window)==3,"second source shares the same host journal");
    KeyboardModifiers keys;keys.Set(true);SendMessageW(GetDlgItem(window,kIdCanvas),WM_KEYDOWN,'Z',0);
    Check(journal.cursor==1&&journal.traversals==1&&KeyCount(window)==2&&journal.applies==2,"Ctrl+Z traverses host history without applying a local snapshot");
    SendMessageW(GetDlgItem(window,kIdFitAll),WM_KEYDOWN,'Y',0);
    Check(journal.cursor==2&&KeyCount(window)==3&&journal.applies==2,"Ctrl+Y from a child control redoes host history");keys.Set();
    journal.Select(0);Click(window,kIdUndo);
    Check(journal.cursor==1&&KeyCount(window)==3&&journal.sources[1].data[0].track.keys.size()==2,"undo from a different source still traverses the latest shared operation");
    Click(window,kIdUndo);Check(journal.cursor==0&&KeyCount(window)==2,"the next undo reaches the earlier source");
    Click(window,kIdRedo);Click(window,kIdRedo);Check(journal.cursor==2&&journal.applies==2,"redo across sources never adds replacement history nodes");
    auto native=journal.sources[0].data;native[0].track.keys.push_back(MakeKey(40,9));journal.Push(0,native);
    const auto applies=journal.applies;journal.Tick();
    Check(KeyCount(window)==4&&journal.applies==applies,"history revision polling adopts a native or plugin edit without source.apply");
    const int reads=journal.sources[0].readCalls;const auto revision=journal.revision;
    journal.hideUndo=true;journal.Tick();Check(!IsWindowEnabled(GetDlgItem(window,kIdUndo)),"limit changes disable undo even when history revision is unchanged");
    journal.hideUndo=false;journal.Tick();Check(IsWindowEnabled(GetDlgItem(window,kIdUndo))&&journal.revision==revision&&journal.sources[0].readCalls==reads,"count-only changes do not reread curve data");
    auto replacement=journal.sources[0].data;replacement[0].track.keys.push_back(MakeKey(60,11));journal.Push(0,replacement);
    bool changed=false;journal.onRead=[&]{if(!changed){changed=true;++journal.revision;}};journal.Tick();journal.ReadOnly(true);
    journal.onRead={};journal.Tick();journal.ReadOnly(false);
    Check(KeyCount(window)==5&&journal.applies==applies,"a history revision race retries the read without authoring or traversing");
    journal.NoLocalHistory();
}
void ExternalHistoryRefreshFailures(){
    ExternalCurveJournal journal;HWND window=journal.Open();journal.Add(20,3);
    journal.readAfterTraversal=true;Click(window,kIdUndo);
    Check(journal.cursor==0&&journal.traversals==1,"host undo commits once before a failed canonical read");journal.ReadOnly(true);
    Click(window,kIdUndo);Click(window,kIdRedo);Check(journal.traversals==1,"pending canonical reads never repeat the committed traversal");
    journal.readAfterTraversal=false;for(auto& source:journal.sources)source.failRead=false;journal.Tick();
    Check(journal.traversals==1&&KeyCount(window)==2&&IsWindowEnabled(GetDlgItem(window,kIdRedo)),"timer read recovery restores controls and redo without traversal");journal.ReadOnly(false);
    Click(window,kIdRedo);Check(journal.traversals==2&&journal.applies==1&&KeyCount(window)==3,"recovered redo is a separate explicit user operation");
    journal.readAfterApply=true;journal.Add(30,7);Check(journal.cursor==2&&journal.applies==2,"failed post-write refresh does not repeat source.apply");journal.ReadOnly(true);
    journal.readAfterApply=false;journal.sources[0].failRead=false;journal.Tick();journal.ReadOnly(false);Check(KeyCount(window)==4,"post-write canonical data recovers from the host");
    const auto cursor=journal.cursor;journal.failQuery=true;journal.Tick();journal.ReadOnly(true);
    Check(journal.cursor==cursor,"query failure does not erase host history");journal.failQuery=false;journal.Tick();journal.ReadOnly(false);
    journal.failTraversal=true;Click(window,kIdUndo);
    Check(journal.cursor==cursor&&journal.traversals==2&&journal.applies==2,"busy mutation gate leaves shared history and local curve data unchanged");
    journal.failTraversal=false;journal.failApply=true;journal.Add(40,8);
    Check(journal.cursor==cursor&&KeyCount(window)==4,"refused external writes cannot publish a local undo entry");
    journal.failApply=false;journal.queryAfterApply=true;journal.Add(40,8);
    Check(journal.cursor==cursor+1,"post-publication query failure keeps the committed host edit");journal.ReadOnly(true);
    journal.queryAfterApply=false;journal.failQuery=false;journal.Tick();
    Check(KeyCount(window)==5,"a failed post-publication query retries the canonical read without republishing");journal.ReadOnly(false);journal.NoLocalHistory();
}
void ExternalHistoryGesturesAndReentry(){
    ExternalCurveJournal journal;HWND window=journal.Open();
    journal.onApply=[&]{SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdUndo,BN_CLICKED),0);};
    DragKeyToFrame(window,1,20);
    Check(journal.cursor==1&&journal.applies==1&&FindKey(journal.sources[0].data,0,20)&&!GetCapture(),"one completed canvas drag releases capture then publishes one host edit");
    Check(journal.traversals==0,"write callbacks cannot reenter external undo");journal.onApply={};Click(window,kIdFitAll);
    const auto origin=AskPoint(window,kHookPointForKey,1),destination=AskPoint(window,kHookPointForFrame,30);
    HWND gesture=GetDlgItem(window,kIdCanvas);
    SendMessageW(gesture,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(origin.x,origin.y));
    SendMessageW(gesture,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(destination.x,origin.y));
    SendMessageW(gesture,WM_KEYDOWN,VK_ESCAPE,0);SendMessageW(gesture,WM_LBUTTONUP,0,MAKELPARAM(destination.x,origin.y));
    Check(journal.cursor==1&&journal.applies==1&&FindKey(journal.sources[0].data,0,20),"a cancelled canvas drag never publishes or changes host history");
    const auto point=AskPoint(window,kHookPointForFrame,5);HWND canvas=GetDlgItem(window,kIdCanvas);
    SendMessageW(canvas,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    Check(journal.cursor==2&&journal.applies==2&&FindKey(journal.sources[0].data,0,5),"shape-preserving insertion publishes one external edit");
    Click(window,kIdDeleteKey);Check(journal.cursor==3&&journal.applies==3,"Delete publishes one external edit");
    Click(window,kIdUndo);Check(journal.cursor==2,"seed a shared redo branch for a no-op");
    ClickKey(window,0);const auto applies=journal.applies;Click(window,kIdApplyKey);
    Check(journal.cursor==2&&journal.nodes.size()==3&&journal.applies==applies,"an unchanged explicit apply preserves shared redo without writing");
    Click(window,kIdReadMotion);Check(journal.cursor==3&&journal.motions==1&&FindKey(journal.sources[0].data,0,50),"motion import is one host action and re-reads canonical data");
    const auto traversals=journal.traversals;
    journal.onQuery=[&]{SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdUndo,BN_CLICKED),0);};journal.Tick();
    Check(journal.traversals==traversals,"read-only query callbacks cannot reenter history traversal");journal.onQuery={};journal.NoLocalHistory();
}
void ExternalHistoryOptIn(){
    ExternalCurveJournal journal;auto session=journal.Session();session.history.traverse={};
    journal.window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));journal.ReadOnly(true);
    journal.Add(20,3);Click(journal.window,kIdUndo);
    Check(journal.applies==0&&journal.traversals==0,"missing external callbacks fail closed instead of creating local history");
    ce::CloseOwned(nullptr);journal.window=nullptr;
    HostSource local;local.data.push_back(MakeChannel("x",L"X",{MakeKey(0,0),MakeKey(10,5)}));
    auto ordinary=MakeSession(local);ordinary.history=journal.Bridge();
    HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(ordinary),false));
    SetText(window,kIdFrameEdit,L"20");SetText(window,kIdValueEdit,L"3");Click(window,kIdAddKey);Click(window,kIdUndo);
    Check(local.applyCalls==2&&KeyCount(window)==2&&!journal.traversals,"a source without opt-in retains its existing local history");ce::CloseOwned(nullptr);
}
struct ExternalEditFocusProbe {
    HWND field;unsigned disabled=0,focusLost=0;
    static LRESULT CALLBACK Window(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
        auto& probe=*reinterpret_cast<ExternalEditFocusProbe*>(data);
        if(message==WM_ENABLE&&!wp)++probe.disabled;
        if(message==WM_KILLFOCUS)++probe.focusLost;
        return DefSubclassProc(window,message,wp,lp);
    }
    explicit ExternalEditFocusProbe(HWND value):field(value){
        Check(SetWindowSubclass(field,Window,1,reinterpret_cast<DWORD_PTR>(this))!=FALSE,"observe external value-field focus and availability");
    }
    ~ExternalEditFocusProbe(){RemoveWindowSubclass(field,Window,1);}
};
void ExternalHistoryFocus(){
    ExternalCurveJournal journal;auto session=journal.Session();
    session.sources[0].readKeySelection=[&](const std::string&){
        std::vector<int> frames;for(const auto& key:journal.sources[0].data[0].track.keys)frames.push_back(key.frame);return frames;
    };
    HWND window=journal.window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
    // Permit native focus without showing a window or reading desktop pixels,
    // as in the existing modeless tab-order regression.
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);const HWND previous=GetFocus();
    SetWindowLongPtrW(window,GWL_STYLE,style|WS_VISIBLE);
    const HWND field=GetDlgItem(window,kIdValueEdit),peer=GetDlgItem(window,kIdFitAll);
    SetFocus(field);Check(GetFocus()==field&&IsWindowEnabled(field),"external value field obtains focus before publication");
    ExternalEditFocusProbe probe(field);
    journal.Add(20,3);
    Check(journal.cursor==1&&GetFocus()==field&&probe.disabled==0&&probe.focusLost==0,
        "successful external publication never temporarily disables or defocuses its text field");
    auto next=journal.sources[0].data;next[0].track.keys.push_back(MakeKey(30,7));journal.Push(0,next);journal.Tick();
    Check(KeyCount(window)==4&&GetFocus()==field&&probe.disabled==0&&probe.focusLost==0,
        "successful external revision refresh preserves native edit focus without disable-enable cycling");
    // A failed canonical read still publishes the required read-only state.
    journal.readAfterApply=true;journal.Add(40,8);
    Check(!IsWindowEnabled(field)&&probe.disabled!=0,"failed external canonical read disables editing rather than preserving a writable stale field");
    SetFocus(peer);Check(GetFocus()==peer,"navigation remains available while external read recovery is pending");
    journal.readAfterApply=false;journal.sources[0].failRead=false;journal.Tick();
    Check(IsWindowEnabled(field)&&GetFocus()==peer&&KeyCount(window)==5,
        "canonical read recovery enables editing without stealing the user's new focus");
    SetWindowLongPtrW(window,GWL_STYLE,style);if(previous&&IsWindow(previous))SetFocus(previous);
}
void ExternalHistoryReadBoundary(){
    ExternalCurveJournal journal;HWND window=journal.Open();journal.Add(20,3);
    const auto cursor=journal.cursor;const auto applies=journal.applies;
    journal.onRead=[]{throw std::bad_alloc();};
    journal.Add(30,7);
    Check(journal.cursor==cursor&&journal.applies==applies&&journal.traversals==0,
        "a pre-write source read exception refuses the edit before host publication or history traversal");
    journal.ReadOnly(true);journal.onRead={};journal.Tick();journal.ReadOnly(false);
    Check(KeyCount(window)==3,"a failed pre-write read recovers canonical data without replaying the refused edit");
    journal.onRead=[&]{SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdUndo,BN_CLICKED),0);};
    journal.Add(30,7);journal.onRead={};
    Check(journal.cursor==cursor+1&&journal.applies==applies+1&&journal.traversals==0,
        "source read callbacks cannot recursively traverse the host journal during pre-write or canonical reads");
    journal.NoLocalHistory();
}
void ExternalHistoryChecks(){
    ExternalHistoryAcrossSources();ExternalHistoryRefreshFailures();ExternalHistoryGesturesAndReentry();ExternalHistoryOptIn();ExternalHistoryFocus();ExternalHistoryReadBoundary();
    Note("external shared history, canonical recovery and local-host compatibility");
}
