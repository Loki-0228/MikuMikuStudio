// Real editor command regressions for the all-code review findings.
namespace mikudancestudio {
void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void DeleteMarkedKeyframes(MMDApp*);
}
namespace {
unsigned reviewUnexpectedDialogs = 0;
LRESULT CALLBACK ReviewDismissDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        const HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t type[32]{}; GetClassNameW(dialog, type, 32);
        if (std::wcscmp(type, L"#32770") == 0) {
            ++reviewUnexpectedDialogs;
            wchar_t caption[128]{}; GetWindowTextW(dialog, caption, 128);
            std::printf("UNEXPECTED DIALOG: %ls\n", caption);
            ShowWindow(dialog, SW_HIDE);
            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void ReviewGlobalRootDeletion() {
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>(); std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get(); app->state.optflag[0] = 1;
    std::vector<mdl::CameraKey> camera(mdl::kTimelineKeyCapacity);
    std::vector<mdl::LightKey> light(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadow(mdl::kTimelineKeyCapacity);
    std::vector<mdl::GravityKey> gravity(mdl::kTimelineKeyCapacity);
    app->CameraKeys()=camera.data(); app->LightKeys()=light.data();
    app->ShadowKeys()=shadow.data(); app->GravityKeys()=gravity.data();
    auto prepare=[](auto& keys) {
        keys[0].selected=1; keys[0].next=1;
        keys[1].frame=10; keys[1].next=2;
        keys[2].frame=20; keys[2].previous=1;
    };
    prepare(camera); prepare(light); prepare(shadow); prepare(gravity);
    auto* previous=g_Block; g_Block=app.get(); DeleteMarkedKeyframes(app.get()); g_Block=previous;
    auto verify=[](const auto& keys) {
        Check(keys[0].next==1 && keys[1].next==2 && keys[2].frame==20,
              "deleting a global frame-zero key preserves unselected later keys");
    };
    verify(camera);verify(light);verify(shadow);verify(gravity);
}
void ReviewFramePasteUndo() {
    for (int kind=0;kind<3;++kind) {
        std::printf("review frame paste kind=%d setup\n",kind);
        Fixture f; std::array<mdl::MorphRecord,1> morphs{};
        std::strcpy(morphs[0].name,"face"); f.model->morphs=morphs.data();
        f.model->physicsMode=2; f.model->facialFrameCount=1;
        std::strcpy(f.bones[2].name,"bone");
        auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
        auto renderer=std::make_unique<D3DRenderer>();std::memset(renderer.get(),0,sizeof(*renderer));
        app->Renderer()=renderer.get();app->ModelSlot(0)=f.Raw();app->CurrentFrame()=10;
        mdl::MorphClipboardRecord morph{}; std::strcpy(morph.name,"face");morph.value=.75f;
        mdl::DisplayClipboardRecord display{};display.visible=0;
        mdl::BoneClipboardRecord bone{};std::strcpy(bone.name,"bone");bone.position[0]=4;bone.rotation[3]=1;
        app->ClipboardCounts().morphs=kind!=1;app->MorphClipboard()=&morph;
        app->ClipboardCounts().displays=kind!=0;app->DisplayClipboard()=&display;
        app->ClipboardCounts().bones=kind==2;app->BoneClipboard()=&bone;
        auto* previous=g_Block;g_Block=app.get();
        std::puts("review frame paste: paste");CmdControl400(app.get(),nullptr,421,0);
        Check((kind==1 || f.morphKeys[0].next!=0) && (kind==0 || f.displayKeys[0].next!=0),
              "real paste inserts every selected channel");
        std::puts("review frame paste: undo");CmdControl400(app.get(),nullptr,400,0);
        Check(f.morphKeys[0].next==0 && f.displayKeys[0].next==0 && f.keys[2].next==0,
              "one undo restores morph-only, display-only and mixed paste");
        std::puts("review frame paste: redo");CmdControl400(app.get(),nullptr,401,0);
        Check((kind==1 || f.morphKeys[0].next!=0) && (kind==0 || f.displayKeys[0].next!=0) &&
              (kind!=2 || f.keys[2].next!=0),"redo restores every pasted channel");
        g_Block=previous;
    }
}
void ReviewPosePasteUndo() {
    struct PoseCopy {char name[20];float position[3];float rotation[4];};
    static_assert(sizeof(PoseCopy)==48);
    for(int command:{497,498}) for(bool matched:{false,true}) {
        std::printf("review pose paste command=%d matched=%d\n",command,int(matched));
        Fixture f; f.model->morphCount=0;
        // A matching paste refreshes the real model panel. Loaded models own
        // these lookup tables even when the test has no visible controls.
        std::array<std::uint32_t,Fixture::kBones> boneRows{};
        std::array<std::uint32_t,1> morphRows{};
        f.model->boneKeyIndices=boneRows.data();f.model->morphKeyIndices=morphRows.data();
        std::strcpy(f.bones[1].name,"match"); f.bones[0].trans[0]=12.5f; f.bones[1].trans[0]=3;
        auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
        auto renderer=std::make_unique<D3DRenderer>();std::memset(renderer.get(),0,sizeof(*renderer));
        app->Renderer()=renderer.get();app->ModelSlot(0)=f.Raw();
        std::array<PoseCopy,2> copied{}; std::strcpy(copied[0].name,"missing");
        std::strcpy(copied[1].name,matched?"match":"absent");
        for(auto& entry:copied){entry.position[0]=7;entry.rotation[3]=1;}
        app->BoneCopyRecords()=copied.data(); app->state.copiedBoneCount=2;
        auto* previous=g_Block;g_Block=app.get();std::puts("review pose: paste call");CmdControl450(app.get(),nullptr,command,0);std::puts("review pose: paste returned");
        Check(f.model->undoRings[0].slots[f.model->undoState[0]].dirty==(matched?1:0),
              "pose paste undo contains only actual matching bones");
        std::int32_t frame=0;std::puts("review pose: undo call");UndoModelEdit(f.Raw(),&frame);std::puts("review pose: undo returned");
        Check(f.bones[0].trans[0]==12.5f && f.bones[0].rotQuat[3]==1 && f.bones[1].trans[0]==3,
              "unmatched ordinary and mirror pose records cannot reset bone zero");
        g_Block=previous;
    }
}
void ReviewVpdUndo() {
    for(int scenario=0;scenario<3;++scenario) {
        Fixture f; f.model->morphCount=0;
        std::strcpy(f.bones[0].name,"root");std::strcpy(f.bones[1].name,"root");
        f.bones[0].trans[0]=12.5f;f.bones[1].trans[0]=3;
        auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
        app->ModelSlot(0)=f.Raw();
        wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{};
        GetTempPathW(MAX_PATH,folder);GetTempFileNameW(folder,L"mvu",0,path);
        FILE* file=nullptr;_wfopen_s(&file,path,L"wb");Check(file!=nullptr,"create VPD undo fixture");
        std::fprintf(file,"Vocaloid Pose Data file\n\nother.osm;\n%d;\n\n",scenario==2?2:1);
        for(int i=0;i<(scenario==2?2:1);++i)
            std::fprintf(file,"Bone%d{%s\n  %d,2,3;\n  0,0,0,1;\n}\n\n",i,scenario?"root":"missing",i+1);
        std::fclose(file);
        auto* previous=g_Block;g_Block=app.get();LoadVpdFile(path);
        auto& entry=f.model->undoRings[0].slots[f.model->undoState[0]];
        Check(entry.dirty==(scenario?2:0),"VPD undo counts matched targets once, including duplicate names");
        std::int32_t frame=0;UndoModelEdit(f.Raw(),&frame);
        Check(f.bones[0].trans[0]==12.5f && f.bones[1].trans[0]==3 && f.bones[0].rotQuat[3]==1,
              "VPD undo restores original poses for unmatched and repeated records");
        g_Block=previous;DeleteFileW(path);
    }
}
void ReviewEditingChecks(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    const HHOOK hook=SetWindowsHookExW(WH_CBT,ReviewDismissDialog,nullptr,GetCurrentThreadId());
    std::puts("review global deletion: begin");ReviewGlobalRootDeletion();std::puts("review global deletion: passed");
    std::puts("review frame paste: begin");ReviewFramePasteUndo();std::puts("review frame paste: passed");
    std::puts("review pose paste: begin");ReviewPosePasteUndo();std::puts("review pose paste: passed");
    std::puts("review VPD undo: begin");ReviewVpdUndo();std::puts("review VPD undo: passed");
    if(hook)UnhookWindowsHookEx(hook);
    Check(!reviewUnexpectedDialogs,"editor review needs no unexpected dialogs");
}
}
