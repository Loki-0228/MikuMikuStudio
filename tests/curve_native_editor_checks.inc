// Native precision is checked by the real host tests; this fixture checks
// the shared editor's subdivision callback and authoritative bake sampling.
void NativeSubdivisionRoute() {
    HostSource host;
    host.data.push_back(MakeChannel("native",L"Native",{MakeKey(0,0),MakeKey(20,20)}));
    host.data[0].sample=[](const ce::Channel&,double f){return ac::Sample{f*f/20.0,f/10.0};};
    host.data[0].nativeControls.resize(2);
    auto session=MakeSession(host);int calls=0;bool refuse=false;
    session.sources[0].subdivide=[&](auto& channels,std::size_t c,int frame,std::size_t& index,std::wstring& error){
        ++calls;if(refuse){error=L"native subdivision refused";return false;}
        std::string detail;
        if(ac::InsertPreservingShape(channels[c].track,frame,index,detail)!=ac::InsertResult::Inserted)return false;
        channels[c].track.keys[index].value=channels[c].sample(channels[c],frame).value;return true;
    };
    auto window=static_cast<HWND>(ce::Open(nullptr,session,false));
    auto canvas=GetDlgItem(window,kIdCanvas);auto point=AskPoint(window,kHookPointForFrame,10);
    SendMessageW(canvas,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    Check(calls==1&&host.applyCalls==1&&host.data[0].track.keys.size()==3,"native double click uses source subdivision and one publication");
    Check(host.data[0].track.keys[1].frame==10&&host.data[0].track.keys[1].value==5,"native subdivision keeps source value rather than scalar proxy");
    Click(window,kIdUndo);Check(host.data[0].track.keys.size()==2,"native subdivision participates in editor undo");
    refuse=true;point=AskPoint(window,kHookPointForFrame,10);const auto applied=host.applyCalls;
    SendMessageW(canvas,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    Check(calls==2&&host.applyCalls==applied&&host.data[0].track.keys.size()==2,"refused subdivision leaves source intact");
    Click(window,kIdRedo);Check(host.data[0].track.keys.size()==3,"refused subdivision preserves redo");
    ce::CloseOwned(nullptr);
}
void NativeSampleBake() {
    HostSource host;
    host.data.push_back(MakeChannel("native",L"Native",{MakeKey(0,0),MakeKey(20,20)}));
    host.data[0].sample=[](const ce::Channel&,double f){return ac::Sample{f*f/20.0,f/10.0};};
    host.data[0].nativeControls.resize(2);
    auto window=static_cast<HWND>(ce::Open(nullptr,MakeSession(host),false));
    SetText(window,kIdBakeFirstEdit,L"0");SetText(window,kIdBakeLastEdit,L"20");SetText(window,kIdBakeStepEdit,L"5");
    Click(window,kIdBakeRun);
    Check(host.applyCalls==1&&host.data[0].track.keys.size()==5,"native baking publishes source samples once");
    for(const auto& key:host.data[0].track.keys)Check(key.value==double(key.frame)*key.frame/20,"native baking reads authoritative sampler");
    Check(!host.data[0].sample&&host.data[0].nativeControls.empty()&&host.data[0].preservedCameraIntervals.empty(),"baked scalar data releases native sampling metadata");
    Check(host.data[0].track.keys[1].outSlope==0.75,"baked slopes follow sampled values");
    Click(window,kIdUndo);Check(host.data[0].sample&&host.data[0].nativeControls.size()==2,"bake undo restores native evaluation");
    Click(window,kIdRedo);Check(!host.data[0].sample&&host.data[0].track.keys[2].value==5,"bake redo restores sampled values");
    ce::CloseOwned(nullptr);
}
