// Included inside the scene lifecycle test namespace.
#if !MMD_ORIGINAL_PROFILE
std::filesystem::path scatterOutput;
int scatterUiAction = 0, scatterUiSeen = 0;
void CaptureScatterDialog(HWND window) {
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right-rect.left, height = rect.bottom-rect.top;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr); auto old = SelectObject(dc, bitmap);
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    GdiFlush(); BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader); header.bfSize = header.bfOffBits + width*height*4;
    std::ofstream output(scatterOutput / (L"scatter-" + std::to_wstring(g_Block->EnglishUI()) + L".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels), width*height*4);
    SelectObject(dc, old); DeleteDC(dc); DeleteObject(bitmap);
}
LRESULT CALLBACK ScatterHook(int code, WPARAM w, LPARAM l) {
    HWND window = reinterpret_cast<HWND>(w);
    if (code == HCBT_ACTIVATE && GetDlgItem(window, 5106) && !GetPropW(window, L"ScatterChecked")) {
        SetPropW(window, L"ScatterChecked", reinterpret_cast<HANDLE>(1)); ++scatterUiSeen;
        Check(IsWindowEnabled(GetDlgItem(window, IDOK)), "initial preview enables scatter");
        if (!scatterUiAction) CaptureScatterDialog(window);
        SetDlgItemTextW(window, 5103, L"nan");
        Check(!IsWindowEnabled(GetDlgItem(window, IDOK)), "changing inputs invalidates old preview");
        SendMessageW(window, WM_COMMAND, 5201, 0);
        Check(!IsWindowEnabled(GetDlgItem(window, IDOK)), "invalid numeric input cannot scatter");
        SetDlgItemTextW(window, 5103, L"30"); SendMessageW(window, WM_COMMAND, 5201, 0);
        Check(IsWindowEnabled(GetDlgItem(window, IDOK)), "corrected input can be previewed again");
        PostMessageW(window, WM_COMMAND, scatterUiAction ? IDOK : IDCANCEL, 0);
        return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}
void CheckSequenceScatter(MMDApp* app, const std::filesystem::path& output) {
    namespace ss = sequence_scatter; namespace ac = animation_curve;
    scatterOutput = output; std::filesystem::create_directories(output);
    auto* raw = app->SelectedModel(); Check(raw != nullptr, "scatter fixture model");
    auto& m = *mdl::Mdl(raw); app->CameraMode() = 0;
    std::vector<int> morphs;
    for (unsigned i = 0; i < m.morphCount && morphs.size() < 3; ++i) if (static_cast<unsigned>(m.morphs[i].panel)) morphs.push_back(i);
    Check(morphs.size() == 3, "fixture has three visible expression tracks");
    std::string error;
    std::vector<ac::NamedTrack> fixture;
    for (int morph : morphs) {
        ac::NamedTrack track; track.id = morph_curve::StableId(morph, m.morphs[morph].name);
        for (int f : {0, 10, 15, 20}) {
            ac::Key k; k.frame = f; k.value = f == 15 ? .8 : 0;
            k.tangent = ac::Tangent::Free; k.interpolation = ac::Interpolation::Bezier;
            k.inSlope = f == 15 ? .025 : -.01; k.outSlope = f == 10 ? .04 : -.02;
            track.track.keys.push_back(k);
        }
        fixture.push_back(track);
    }
    Check(morph_curve::Write(raw, fixture, error), "install source curves");
    for (std::size_t i=0;i<mdl::BoneKeyCapacity(m);++i)m.boneKeys[i].allocated=0;
    for (std::size_t i = 0; i < mdl::MorphKeyCapacity(m.morphKeys); ++i) m.morphKeys[i].allocated = 0;
    for (int c = 0; c < 2; ++c) { unsigned index = morphs[c]; do {
        m.morphKeys[index].allocated = m.morphKeys[index].frame >= 10;
        index = m.morphKeys[index].next;
    } while (index); }
    ss::Source source; Check(ss::Capture(raw, source, error), "capture selected expression keys");
    Check(source.keyCount == 6 && source.channels.size() == 2 && source.first == 10 && source.last == 20,
          "only selected channels and keys enter source");
    ss::Settings settings; settings.first = 40; settings.last = 120; settings.gapMean = 20;
    settings.gapVariance = 0; settings.speedMean = 2; settings.speedVariance = 0;
    ss::Plan plan; Check(ss::Prepare(source, settings, plan, error), "prepare fixed scatter");
    Check(plan.copies == 4 && plan.keys == 24, "fixed gap and speed produce four complete copies");
    Check(std::memcmp(source.pool.data(), m.morphKeys, source.pool.size()*sizeof(mdl::MorphKey)) == 0,
          "preview does not mutate native keys");
    auto encoded = [&](const auto& tracks) { std::vector<std::uint8_t> bytes; Check(ac::Encode(tracks, bytes, error), "encode result"); return bytes; };
    settings.gapVariance = 49; settings.speedVariance = .04;
    ss::Plan random1, random2; Check(ss::Prepare(source, settings, random1, error) && ss::Prepare(source, settings, random2, error), "random plans");
    Check(encoded(random1.tracks) == encoded(random2.tracks), "same seed preserves exact preview");
    ++settings.seed; Check(ss::Prepare(source, settings, random2, error), "reroll plan");
    Check(encoded(random1.tracks) != encoded(random2.tracks), "reroll changes placements");
    settings.first = 10; settings.last = 20; settings.gapVariance = 0; settings.speedMean = 1; settings.speedVariance = 0;
    ss::Plan blocked; Check(ss::Prepare(source, settings, blocked, error) && blocked.copies == 0 && blocked.conflicts > 0, "source never overwritten");
    settings.first = 40; settings.last = 41; Check(ss::Prepare(source, settings, blocked, error) && blocked.copies == 0, "no truncated copy");
    auto occupied = source;
    ac::Key conflict; conflict.frame = 45; conflict.interpolation = ac::Interpolation::Linear;
    occupied.channels[0].original.track.keys.push_back(conflict);
    settings.last = 100;
    Check(ss::Prepare(occupied, settings, blocked, error) && blocked.conflicts == 1 && blocked.copies == 1,
          "a collision on one participating expression skips the entire multi-expression copy");
    settings.last = 100; settings.speedMean = 20;
    Check(ss::Prepare(source, settings, blocked, error) && blocked.copies == 0 && blocked.compressed > 0, "collapsed retimed frames are skipped");
    settings.speedMean = std::numeric_limits<double>::quiet_NaN();
    Check(!ss::Prepare(source, settings, blocked, error), "NaN rejected");
    app->CurrentFrame() = 43;
    const auto beforeRing = m.undoState[0];
    Check(ss::Apply(*app, source, plan, error), "commit scatter");
    Check(std::abs(m.morphs[morphs[0]].value-.8f)<1e-6f,"scattered expression is displayed at the current frame");
    Check(m.undoState[0] == (beforeRing+1)%30, "scatter consumes exactly one undo slot");
    ac::Track actual; Check(morph_curve::Read(raw, morphs[0], actual, error), "read scattered curve");
    Check(actual.keys.size() == fixture[0].track.keys.size()+12, "all copied keys present");
    // Piecewise quantization: 10,15,20 maps to 40,43,45 at speed 2.
    for (int i = 0; i <= 20; ++i) {
        const double t = i/20.;
        Check(std::abs(ac::Evaluate(actual, 40+3*t).value - ac::Evaluate(fixture[0].track, 10+5*t).value) < 1e-7,
              "retimed Bezier shape retained within a quantized interval");
    }
    ac::Track untouched; Check(morph_curve::Read(raw, morphs[2], untouched, error), "read unselected track");
    Check(encoded(std::vector<ac::NamedTrack>{{fixture[2].id, untouched}}) == encoded(std::vector<ac::NamedTrack>{fixture[2]}), "unselected authored curve unchanged");
    const auto afterPool = std::vector<mdl::MorphKey>(m.morphKeys, m.morphKeys+mdl::MorphKeyCapacity(m.morphKeys));
    const auto afterCurve = encoded(std::vector<ac::NamedTrack>{{fixture[0].id, actual}});
    std::int32_t frame = app->CurrentFrame(); UndoModelEdit(raw, &frame);
    Check(std::abs(m.morphs[morphs[0]].value)<1e-6f,"undo immediately refreshes the displayed expression");
    Check(std::memcmp(source.pool.data(), m.morphKeys, source.pool.size()*sizeof(mdl::MorphKey)) == 0, "one undo restores all original keys and marks");
    Check(morph_curve::Read(raw, morphs[0], actual, error), "read undo curve");
    Check(encoded(std::vector<ac::NamedTrack>{{fixture[0].id, actual}}) == encoded(std::vector<ac::NamedTrack>{fixture[0]}), "undo restores exact authored curve");
    RedoModelEdit(raw, &frame);
    Check(std::abs(m.morphs[morphs[0]].value-.8f)<1e-6f,"redo immediately refreshes the displayed expression");
    Check(std::memcmp(afterPool.data(), m.morphKeys, afterPool.size()*sizeof(mdl::MorphKey)) == 0, "redo restores exact random result and marks");
    UndoModelEdit(raw, &frame);
    for (int language : {0,1,2}) {
        app->EnglishUI() = language; LocalizeUI(app);
        HMENU menu = GetSubMenu(GetMenu(app->MainWindow()), 1);
        Check(GetMenuState(menu, ss::kMenuCommand, MF_BYCOMMAND) != UINT(-1), "scatter is in Edit menu");
        int count = 0; for (int i = 0; i < GetMenuItemCount(menu); ++i) if (GetMenuItemID(menu,i) == ss::kMenuCommand) ++count;
        Check(count == 1, "language changes do not duplicate menu entry");
        HHOOK hook = SetWindowsHookExW(WH_CBT, ScatterHook, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install scatter dialog probe");
        Check(ss::HandleCommand(app, ss::kMenuCommand), "open native scatter dialog");
        UnhookWindowsHookEx(hook);
        Check(std::memcmp(source.pool.data(), m.morphKeys, source.pool.size()*sizeof(mdl::MorphKey)) == 0, "cancel leaves all keys unchanged");
    }
    Check(scatterUiSeen == 3, "capture all three dialog languages");
    scatterUiAction = 1;
    const auto uiRing = m.undoState[0];
    HHOOK applyHook = SetWindowsHookExW(WH_CBT, ScatterHook, nullptr, GetCurrentThreadId());
    Check(applyHook != nullptr, "install native apply probe");
    ss::HandleCommand(app, ss::kMenuCommand); UnhookWindowsHookEx(applyHook);
    Check(m.undoState[0] == (uiRing+1)%30, "native Scatter button commits exactly one operation");
    UndoModelEdit(raw, &frame);
    Check(std::memcmp(source.pool.data(), m.morphKeys, source.pool.size()*sizeof(mdl::MorphKey)) == 0,
          "native button result can be undone completely");
    scatterUiAction = 0;
    Check(ss::Apply(*app, source, plan, error), "commit after cancelled dialogs");
    const auto path = output / (L"scatter-roundtrip-" + std::to_wstring(GetTickCount64()) + L".pmm");
    Check(SaveSceneCopy(app, path.c_str()), "save scatter PMM");
    ResetAppState(app); wcscpy_s(app->EnvFileName(), 256, path.c_str()); LoadSceneFile();
    Check(morph_curve::Read(app->SelectedModel(), morphs[0], actual, error), "read reopened scatter");
    Check(encoded(std::vector<ac::NamedTrack>{{fixture[0].id, actual}}) == afterCurve, "PMM roundtrip retains retimed authored curves");
    raw=app->SelectedModel();auto& bonesModel=*mdl::Mdl(raw);
    std::fill(bonesModel.boneSelection,bonesModel.boneSelection+bonesModel.boneCount,0);
    bonesModel.boneSelection[0]=bonesModel.boneSelection[1]=1;
    for(int f:{10,20}){bonesModel.boneTable[0].trans[0]=float(f);RegisterSelectedBoneKeys(raw,f,0);}
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(bonesModel.morphKeys);++i)bonesModel.morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(bonesModel);++i)bonesModel.boneKeys[i].allocated=0;
    const auto keyAt=[&](int bone,int f){unsigned i=bone;while(bonesModel.boneKeys[i].frame!=unsigned(f)&&bonesModel.boneKeys[i].next)i=bonesModel.boneKeys[i].next;Check(bonesModel.boneKeys[i].frame==unsigned(f),"find scatter bone key");return i;};
    for(int b:{0,1})for(int f:{10,20})bonesModel.boneKeys[keyAt(b,f)].allocated=1;
    const auto original=keyAt(0,20);bonesModel.boneKeys[original].physicsDisabled=1;
    bonesModel.boneKeys[original].interpolation[0]=37;
    bone_scale::Matrix scale;scale.v[0]=1.5f;bone_scale::SetKey(raw,original,scale);
    bone_scale::Curve scaleCurve{30,40,90,100};bone_scale::SetKeyCurve(raw,original,scaleCurve);
    ss::Source boneSource;Check(ss::Capture(raw,boneSource,error),"capture bone-only sequence");
    Check(boneSource.keyCount==4&&boneSource.bones.size()==2&&boneSource.channels.empty(),"bone-only selection is supported");
    settings={};settings.first=200;settings.last=235;settings.gapMean=20;settings.gapVariance=0;settings.speedMean=2;settings.speedVariance=0;
    ss::Plan bonePlan;Check(ss::Prepare(boneSource,settings,bonePlan,error)&&bonePlan.copies==2&&bonePlan.keys==8,"prepare bone-only retimed copies");
    const bool boneApplied=ss::Apply(*app,boneSource,bonePlan,error);if(!boneApplied)std::fprintf(stderr,"scatter error: %s\n",error.c_str());Check(boneApplied,"apply bone-only scatter");
    const auto copied=keyAt(0,205);
    Check(bonesModel.boneKeys[copied].position[0]==20&&bonesModel.boneKeys[copied].physicsDisabled==1&&bonesModel.boneKeys[copied].interpolation[0]==37,"bone scatter retains position physics and Bezier handles");
    Check(bone_scale::Key(raw,copied)==scale&&bone_scale::KeyCurve(raw,copied)==scaleCurve,"bone scatter retains scale and its interpolation");
    UndoModelEdit(raw,&frame);
    Check(std::memcmp(boneSource.bonePool.data(),bonesModel.boneKeys,boneSource.bonePool.size()*sizeof(mdl::BoneKey))==0,"bone scatter undo restores exact keys and marks");
    RedoModelEdit(raw,&frame);Check(keyAt(0,205)==copied&&bone_scale::Key(raw,copied)==scale,"bone scatter redo restores keys and scale");
    UndoModelEdit(raw,&frame);
    for(int f:{10,20}){unsigned i=morphs[0];while(bonesModel.morphKeys[i].frame!=unsigned(f)&&bonesModel.morphKeys[i].next)i=bonesModel.morphKeys[i].next;bonesModel.morphKeys[i].allocated=1;}
    ss::Source mixed;Check(ss::Capture(raw,mixed,error)&&mixed.bones.size()==2&&mixed.channels.size()==1,"capture combined bone and expression clip");
    ss::Plan mixedPlan;Check(ss::Prepare(mixed,settings,mixedPlan,error)&&mixedPlan.keys==12,"combined clip shares the same timing");
    Check(ss::Apply(*app,mixed,mixedPlan,error),"combined scatter commits atomically");
    UndoModelEdit(raw,&frame);
    Check(std::memcmp(mixed.pool.data(),bonesModel.morphKeys,mixed.pool.size()*sizeof(mdl::MorphKey))==0&&std::memcmp(mixed.bonePool.data(),bonesModel.boneKeys,mixed.bonePool.size()*sizeof(mdl::BoneKey))==0,"one undo restores both bone and expression tracks");
    std::puts("PASS scatter: selection, deterministic randomness, gaps, speed, collisions, Bezier, one undo/redo, three-language dialog, cancellation, PMM roundtrip");
}

#endif

void CheckRepeatedDelete(MMDApp* app) {
    auto* raw = app->SelectedModel(); Check(raw != nullptr, "delete fixture model");
    auto& m = *mdl::Mdl(raw); app->CameraMode() = 0;
    std::fill(m.boneSelection, m.boneSelection+m.boneCount, 0); m.boneSelection[0] = m.boneSelection[1] = 1;
    RegisterSelectedBoneKeys(raw, 140, 0);
    const auto find = [&](int bone) { unsigned i = bone; while (m.boneKeys[i].frame != 140 && m.boneKeys[i].next) i=m.boneKeys[i].next; Check(m.boneKeys[i].frame==140,"find deletion key"); return i; };
    const unsigned a=find(0), b=find(1);
    const auto clear = [&]() {
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(m);++i)m.boneKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(m.morphKeys);++i)m.morphKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(m.displayKeys);++i)m.displayKeys[i].allocated=0;
    };
    clear(); m.boneKeys[a].allocated=1; DeleteMarkedModelKeys(raw,140);
    clear(); m.boneKeys[b].allocated=1; DeleteMarkedModelKeys(raw,140);
    std::int32_t frame=140; UndoModelEdit(raw,&frame);
    Check(m.boneKeys[a].frame==0 && m.boneKeys[b].frame==140,"one undo after two bone deletions restores only the second batch");
    Check(!m.boneKeys[a].allocated && m.boneKeys[b].allocated,"one undo selects only the restored second batch");
    std::puts("PASS two consecutive bone deletions: one undo restores/selects only the second batch");
    RedoModelEdit(raw,&frame);
    int morph=1; Check(m.morphCount>1,"delete fixture expression");
    m.morphs[morph].value=.8f; RegisterMorphKeyCurrent(raw,morph,150);
    unsigned key=morph; while(m.morphKeys[key].frame!=150 && m.morphKeys[key].next)key=m.morphKeys[key].next;
    Check(m.morphKeys[key].frame==150,"find expression deletion key");
    clear(); m.morphKeys[key].allocated=1; DeleteMarkedModelKeys(raw,150);
    SeekModelFrame(raw,150,app->PlaybackPhysicsMode());
    UndoModelEdit(raw,&frame);
    Check(m.boneKeys[a].frame==0 && m.boneKeys[b].frame==0 && m.morphKeys[key].frame==150,
          "undo expression deletion must not undo the previous bone deletion");
    Check(m.morphKeys[key].allocated && !m.boneKeys[a].allocated && !m.boneKeys[b].allocated,
          "expression undo selects only the expression, not earlier deleted bones");
    Check(std::abs(m.morphs[morph].value-.8f)<1e-6f,"deletion undo immediately restores the displayed expression");
    RedoModelEdit(raw,&frame);
    Check(m.morphKeys[key].frame==0 && m.boneKeys[b].frame==0, "redo removes only the expression again");
    UndoModelEdit(raw,&frame);
    std::puts("PASS cross-channel deletion: expression undo does not restore previous bone deletion");
    RegisterDisplayKeyCurrent(raw,160);
    unsigned display=0; while(m.displayKeys[display].frame!=160 && m.displayKeys[display].next)display=m.displayKeys[display].next;
    Check(m.displayKeys[display].frame==160,"find display deletion key");
    m.displayKeys[display].visible=0;
    if(m.ikChainCount)mdl::IkStates(m.displayKeys[display])[0]=0;
    if(m.boneOrderCount)mdl::SelectorStates(m.displayKeys[display])[0]={0,1};
    clear(); m.displayKeys[display].allocated=1; DeleteMarkedModelKeys(raw,160);
    UndoModelEdit(raw,&frame);
    Check(m.displayKeys[display].frame==160 && m.displayKeys[display].visible==0, "display deletion undo restores visibility");
    Check(!m.ikChainCount || mdl::IkStates(m.displayKeys[display])[0]==0,"display undo restores IK state");
    Check(!m.boneOrderCount || mdl::SelectorStates(m.displayKeys[display])[0].boneIndex==1,"display undo restores external parent");
    RedoModelEdit(raw,&frame); Check(m.displayKeys[display].frame==0,"display redo works");
    UndoModelEdit(raw,&frame);
    RegisterSelectedBoneKeys(raw,170,0);
    unsigned bone=0; while(m.boneKeys[bone].frame!=170 && m.boneKeys[bone].next)bone=m.boneKeys[bone].next;
    Check(m.boneKeys[bone].frame==170,"find mixed deletion bone");
    clear(); m.boneKeys[bone].allocated=m.morphKeys[key].allocated=m.displayKeys[display].allocated=1;
    const auto ring=m.undoState[0]; DeleteMarkedModelKeys(raw,170);
    Check(m.undoState[0]==(ring+1)%30,"mixed deletion consumes one undo slot");
    UndoModelEdit(raw,&frame);
    Check(m.boneKeys[bone].frame==170 && m.morphKeys[key].frame==150 && m.displayKeys[display].frame==160,
          "one undo restores mixed bone, expression and display batch");
    RedoModelEdit(raw,&frame);
    Check(m.boneKeys[bone].frame==0 && m.morphKeys[key].frame==0 && m.displayKeys[display].frame==0,
          "one redo removes exactly the mixed batch");
    UndoModelEdit(raw,&frame);
    for(int i=0;i<35;++i) {
        clear();m.morphKeys[key].allocated=1;DeleteMarkedModelKeys(raw,150);
        UndoModelEdit(raw,&frame);Check(m.morphKeys[key].frame==150,"reused undo slots restore expression");
        RedoModelEdit(raw,&frame);Check(m.morphKeys[key].frame==0,"reused redo slots delete expression");
        m.morphs[morph].value=.8f;RegisterMorphKeyCurrent(raw,morph,150);
        key=morph;while(m.morphKeys[key].frame!=150 && m.morphKeys[key].next)key=m.morphKeys[key].next;
    }
    std::puts("PASS display/IK/parent and mixed deletion undo/redo, including ring wrap");
}
