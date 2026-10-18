void CurveTransportAndShortcuts(){
    for(unsigned language=0;language<3;++language){
        HostSource host;
        host.data.push_back(MakeChannel("x",L"X",{MakeKey(0,0),MakeKey(100,10)}));
        double frame=0;int seeks=0;bool playing=false;
        auto session=MakeSession(host,language);
        session.currentFrame=[&]{return frame;};
        session.isPlaying=[&]{return playing;};
        session.seek=[&](int next){frame=next;++seeks;playing=false;};
        HWND window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));
        HWND canvas=GetDlgItem(window,kIdCanvas);
        const auto ruler=[&](int value){auto p=AskPoint(window,kHookPointForFrame,value);p.y=12;return MAKELPARAM(p.x,p.y);};
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,ruler(20));
        Check(std::abs(frame-20)<=1&&seeks==1,"top ruler click seeks the host");
        SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,ruler(50));
        Check(std::abs(frame-50)<=1,"ruler drag seeks continuously");
        SendMessageW(canvas,WM_LBUTTONUP,0,ruler(70));
        Check(std::abs(frame-70)<=1,"seek samples the final release position");
        wchar_t text[32]{};GetDlgItemTextW(window,1024,text,32);
        Check(_wtoi(text)==int(frame),"direct seek immediately updates the editor frame input");
        Check(host.applyCalls==0,"time navigation never edits curves");
        ClickChannelOnce(window,0);
        playing=true;
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,ruler(30));
        SendMessageW(canvas,WM_LBUTTONUP,0,ruler(30));
        Check(std::abs(frame-30)<=1&&!playing,"hidden active channel and playback still allow ruler seeking");
        const auto key=[&](int control,int code,bool ctrl=false,bool shift=false,LPARAM flags=0){
            BYTE before[256]{},state[256]{};GetKeyboardState(before);
            state[VK_CONTROL]=ctrl?0x80:0;state[VK_SHIFT]=shift?0x80:0;SetKeyboardState(state);
            SendMessageW(control?GetDlgItem(window,control):window,WM_KEYDOWN,code,flags);
            SetKeyboardState(before);
        };
        key(kIdCanvas,VK_INSERT);
        Check(KeyCount(window)==3&&FindKey(host.data,0,int(frame)),"Insert registers a key at the playhead on a hidden channel");
        key(kIdCanvas,'Z',true);
        Check(KeyCount(window)==2,"Ctrl+Z undoes from the canvas");
        key(kIdSourceCombo,'Y',true);
        Check(KeyCount(window)==3,"Ctrl+Y redoes from a focused child control");
        key(0,'Z',true);key(kIdChannelList,'Z',true,true);
        Check(KeyCount(window)==3,"Ctrl+Shift+Z redoes from the channel tree");
        const int writes=host.applyCalls;
        key(kIdMmdPanel,'Z',true,false,LPARAM(1)<<30);
        Check(host.applyCalls==writes,"held shortcut does not repeat edits");
        key(kIdValueEdit,'Z',true);
        Check(host.applyCalls==writes,"editing text keeps native text undo");
        SetText(window,kIdValueEdit,L"123");
        SendMessageW(GetDlgItem(window,kIdValueEdit),EM_SETSEL,0,-1);
        key(kIdValueEdit,VK_DELETE);
        Check(KeyCount(window)==3&&GetText(window,kIdValueEdit).empty(),"Delete inside a text field only deletes text");
        key(kIdChannelList,VK_DELETE);
        Check(KeyCount(window)==2,"Delete from the tree removes the selected key");
        key(kIdMmdPanel,'Z',true);
        Check(KeyCount(window)==3,"Ctrl+Z from the MMD panel restores the deleted key");
        frame=80;key(kIdCanvas,VK_RETURN);
        Check(KeyCount(window)==4&&FindKey(host.data,0,80),"Enter registers the current frame instead of moving a previous key");
        ce::CloseOwned(nullptr);
    }
    Note("ruler scrubbing, hidden curves, immediate frame display and focused-child shortcuts in three languages");
}

void SpeedViewShortIntervals(){
    HostSource host;
    host.data.push_back(MakeChannel("x",L"X",{MakeShapeKey(0,0),MakeShapeKey(10,10),MakeShapeKey(100000,10)}));
    HWND window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    HWND view=GetDlgItem(window,kIdViewCombo);
    SendMessageW(view,CB_SETCURSEL,1,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(kIdViewCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(view));
    const double peak=ac::Evaluate(host.data[0].track,5).derivative*30;
    const auto point=AskPoint(window,kHookPointForFrame,5);RECT client{};GetClientRect(GetDlgItem(window,kIdCanvas),&client);
    Check(peak>1&&point.y>=24&&point.y<client.bottom-18,"speed fitting retains a short interval's interior peak between distant keys");
    Check(RenderInk(GetDlgItem(window,kIdCanvas))>100,"speed view paints with the fitted analytic range");
    Check(host.applyCalls==0,"speed display never writes or changes VMD curves");
    ce::CloseOwned(nullptr);
}
