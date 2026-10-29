// Full-window regression: PMM camera mode must retain a loaded model target,
// and rotation-only bones must accept/reset manual translation through buttons.
#include "mikumikustudio/bone_parent_coordinates.hpp"
#include "mikumikustudio/negative_frames.hpp"
namespace mikumikustudio {void StepFrame(MMDApp*,bool);void CmdControl400(MMDApp*,HWND,std::uint16_t,std::uint16_t);}
void CheckModelEditControls(mikumikustudio::MMDApp* app, const wchar_t* directory) {
    using namespace mikumikustudio;
    const auto root = std::filesystem::absolute(directory);
    std::filesystem::create_directories(root);
    const auto path = root / L"camera-mode.pmm";
    auto* combo = GetDlgItem(app->MainWindow(), panel::kMainComboModel);
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    ApplyModelComboSelection(app);
    Check(app->CameraMode() != 0, "fixture saves in camera mode");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str());
    SaveSceneFile(app);
    Check(std::filesystem::exists(path), "save real camera-mode PMM");
    app->MainModelComboSelection() = 0;
    LoadSceneFile();
    Check(app->CameraMode() != 0 && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0,
        "PMM retains camera mode on load");
    SendMessageW(GetDlgItem(app->MainWindow(), panel::kModelEditToggle), BM_CLICK, 0, 0);
    Check(app->CameraMode() == 0 && app->SelectedModel() != nullptr,
        "To model switches to loaded model without opening file picker");
    auto* model = app->SelectedModel();
    auto* record = mdl::Mdl(model);
    int bone = -1;
    for (unsigned i = 0; i < record->boneCount; ++i) {
        if (record->boneTable[i].type == mdl::BoneType::RotateMove &&
            !(record->boneTable[i].flags & mdl::kBoneFlagMovable) &&
            record->boneTable[i].parent >= 0) { bone = static_cast<int>(i); break; }
    }
    Check(bone >= 0, "loaded model has a parented rotation-only bone");
    std::memset(record->boneSelection, 0, record->boneCount);
    record->selectedBone = bone;
    record->boneSelection[bone] = 1;
    for (int axis = 0; axis < 3; ++axis) {
        HWND edit = GetDlgItem(app->MainWindow(), panel::kReadoutPosXEdit + axis);
        char input[64]{};
        std::snprintf(input, sizeof(input), "%.6f", BoneParentBindOffset(model, bone, axis) + 2.5f);
        SetWindowTextA(edit, input);
        CommitEditControlTail(app, edit);
        Check(std::abs(record->boneTable[bone].trans[axis] - 2.5f) < .00001f,
            "real numeric control moves rotation-only bone");
        SendMessageW(GetDlgItem(app->MainWindow(), 537 + axis), BM_CLICK, 0, 0);
        Check(record->boneTable[bone].trans[axis] == 0,
            "real XYZ button resets rotation-only bone");
        app->state.messageSeen = 1;
        FrameDriver(app);
        Check(record->boneTable[bone].trans[axis] == 0,
            "rendering cannot restore the previous typed displacement");
    }
    for(float& value:record->boneTable[bone].trans)value=3.f;
    record->boneTable[bone].rotQuat[0]=.5f;record->boneTable[bone].rotQuat[3]=.8660254f;
    record->bonePhysicsState[bone]=1;
    SendMessageW(GetDlgItem(app->MainWindow(),panel::kBoneResetButton),BM_CLICK,0,0);
    for(float value:record->boneTable[bone].trans)Check(value==0,"real bone reset clears every translation axis");
    Check(record->boneTable[bone].rotQuat[0]==0 && record->boneTable[bone].rotQuat[3]==1,"real bone reset restores identity rotation");
    app->CurrentFrame()=0;RefreshAfterFrameApply(app);
    std::array<float,3> zeroPosition{};
    std::copy_n(record->boneTable[bone].trans,3,zeroPosition.begin());
    const int morph=record->morphCount>1 ? 1 : -1;
    const float zeroMorph=morph>=0 ? record->morphs[morph].value : 0;
    record->boneTable[bone].trans[0]+=9;record->bonePhysicsState[bone]=1;
    if(morph>=0)record->morphs[morph].value=.73f;
    StepFrame(app,false);
    Check(app->CurrentFrame()==-1,"left at zero enters a signed negative frame");
    Check(std::abs(record->boneTable[bone].trans[0]-zeroPosition[0])<1e-5,
        "left at zero discards unregistered rotation-only bone displacement");
    if(morph>=0)Check(std::abs(record->morphs[morph].value-zeroMorph)<1e-5,"negative seek discards unregistered morph edit");
    StepFrame(app,true);
    Check(app->CurrentFrame()==0,"right from negative returns to frame zero");
    app->CurrentFrame()=-20;RefreshAfterFrameApply(app);
    record->boneTable[bone].trans[0]=zeroPosition[0]+4;
    RegisterSelectedBoneKeys(model,-20,0);
    if(morph>=0){record->morphs[morph].value=.8f;RegisterMorphKeyCurrent(model,morph,-20);}
    Check(negative_frames::Count(app)>=unsigned(morph>=0?2:1),"real register backends store negative model keys");
    const auto registeredCount=negative_frames::Count(app);
    SendMessageW(GetDlgItem(app->MainWindow(),537),BM_CLICK,0,0);
    Check(record->boneTable[bone].trans[0]==0,"XYZ reset also works on a negative frame");
    CmdControl400(app,app->MainWindow(),400,0);
    Check(std::abs(record->boneTable[bone].trans[0]-(zeroPosition[0]+4))<1e-4 && negative_frames::Count(app)==registeredCount,
        "undo after negative-frame XYZ reset restores pose without undoing registered negative keys");
    app->CurrentFrame()=-10;RefreshAfterFrameApply(app);
    Check(std::abs(record->boneTable[bone].trans[0]-(zeroPosition[0]+2))<1e-4,
        "negative bone interpolation reaches frame-zero pose without touching native key chain");
    SetDlgItemTextA(app->MainWindow(),panel::kPlayStartFrameEdit,"0");
    SetDlgItemTextA(app->MainWindow(),panel::kPlayStopFrameEdit,"-1");
    const auto savedPath=root/L"negative-model-keys.pmm";
    wcscpy_s(app->EnvFileName(),kPathFieldCapacity,savedPath.c_str());
    SaveSceneFile(app);
    Check(std::filesystem::exists(savedPath),"save negative keys through real PMM writer");
    LoadSceneFile();
    Check(app->CurrentFrame()==-10 && negative_frames::Count(app)>=unsigned(morph>=0?2:1),
        "real PMM reload restores signed cursor and negative tracks");
    model=app->ModelSlot(0);record=mdl::Mdl(model);
    SeekModelFrame(model,-20,0);
    Check(std::abs(record->boneTable[bone].trans[0]-(zeroPosition[0]+4))<1e-4,
        "reloaded negative bone key evaluates exactly");
    if(morph>=0)Check(std::abs(record->morphs[morph].value-.8f)<1e-5,"reloaded negative morph key evaluates exactly");
    char endpoint[32]{};GetDlgItemTextA(app->MainWindow(),panel::kPlayStopFrameEdit,endpoint,sizeof(endpoint));
    Check(std::string(endpoint)=="-1","signed playback endpoint survives extension roundtrip");
    SetDlgItemTextA(app->MainWindow(),panel::kRangeStartEdit,"-20");
    SetDlgItemTextA(app->MainWindow(),panel::kRangeStopEdit,"-20");
    CmdControl400(app,app->MainWindow(),415,0);
    Check(IsWindowEnabled(GetDlgItem(app->MainWindow(),420)),"negative range selection enables actual copy control");
    SendMessageW(GetDlgItem(app->MainWindow(),420),BM_CLICK,0,0);
    const auto beforePaste=negative_frames::Count(app);
    Check(IsWindowEnabled(GetDlgItem(app->MainWindow(),421)),"negative clipboard enables actual paste control");
    app->CurrentFrame()=-40;SendMessageW(GetDlgItem(app->MainWindow(),421),BM_CLICK,0,0);
    Check(negative_frames::Count(app)>beforePaste,"real range select, copy and paste retain signed keyframes");
    app->CurrentFrame()=-40;RefreshAfterFrameApply(app);
    Check(std::abs(record->boneTable[bone].trans[0]-(zeroPosition[0]+4))<1e-4,
        "pasted negative key evaluates at signed destination");
    app->CurrentFrame()=5;SendMessageW(GetDlgItem(app->MainWindow(),421),BM_CLICK,0,0);
    SeekModelFrame(model,5,0);
    Check(std::abs(record->boneTable[bone].trans[0]-(zeroPosition[0]+4))<1e-4,
        "negative clipboard pastes into native positive key pool");
    SetDlgItemTextA(app->MainWindow(),panel::kRangeStartEdit,"5");
    SetDlgItemTextA(app->MainWindow(),panel::kRangeStopEdit,"5");
    CmdControl400(app,app->MainWindow(),415,0);
    CmdControl400(app,app->MainWindow(),420,0);
    app->CurrentFrame()=-60;CmdControl400(app,app->MainWindow(),421,0);
    RefreshAfterFrameApply(app);
    Check(std::abs(record->boneTable[bone].trans[0]-(zeroPosition[0]+4))<1e-4,
        "native positive clipboard pastes into negative key store");
    app->PlaybackStartsAtCurrentFrame()=1;
    CmdControl400(app,app->MainWindow(),408,0);
    Check(app->PlaybackStartSeconds()>=0 && app->PlaybackEndSeconds()>=0,"playback from negative cursor never starts below zero");
    if(app->PlaybackActive())CmdControl400(app,app->MainWindow(),408,0);
    Check(!IsWindowVisible(app->MainWindow()), "model edit regression stays hidden");
    std::puts("PASS camera-mode PMM toggle and real rotation-only XYZ reset");
}
