// Real hidden host and journal; calls the gesture backend without moving or
// capturing the desktop mouse. The frame/message entry is tested separately.
#include "../src/plugins/native_picture_gesture.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/native_viewport_history.hpp"
#include "mikumikustudio/background_picture_source.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <limits>

namespace native_picture_gesture_checks {
using namespace mikumikustudio;
using namespace mikumikustudio::plugins;
namespace picture = native_picture_history_checks;
namespace identity = background_identity;
using Mode = SharedHistory::RecordingMode;

struct Placement {
    std::int32_t x, y;
    float scale;
    explicit Placement(MMDApp* app) : x(app->PictureOffsetX()), y(app->PictureOffsetY()), scale(app->PictureScale()) {}
    bool Matches(MMDApp* app) const {
        return x == app->PictureOffsetX() && y == app->PictureOffsetY() &&
            std::memcmp(&scale, &app->PictureScale(), sizeof(scale)) == 0;
    }
};
std::uint64_t Revision(SharedHistory& history) {
    std::uint64_t result = 0;
    Check(history.Revision(result) == MMS_OK, "read actual picture gesture journal revision");
    return result;
}
void Start(MMDApp* app, SharedHistory& history, int operation = 1) {
    Check(!GetCapture(), "backend picture test starts without capturing the desktop mouse");
    Check(BeginPictureGesture(*app, history, app->MainWindow(), operation) == MMS_OK && PictureGestureActive(*app) &&
          PictureGestureSerial(*app) != 0, "begin real picture gesture and expose its nonzero identity");
}
void Preview(MMDApp* app, int x, int y, float scale) {
    Check(PreviewPictureGesture(*app, x, y, scale) == MMS_OK && app->PictureOffsetX() == x &&
          app->PictureOffsetY() == y && app->PictureScale() == scale,
          "real gesture backend applies the complete requested preview");
}
void End(MMDApp* app, bool expectedChange) {
    bool changed = !expectedChange;
    Check(EndPictureGesture(*app, changed) == MMS_OK && changed == expectedChange && !PictureGestureActive(*app),
          "real gesture end publishes once and releases the active transaction");
    picture::Presentation(app);
}
void ResetCase(MMDApp* app, SharedHistory& history, Mode mode) {
    CancelPictureGesture(*app);
    picture::Empty(app, history, mode);
    app->CurrentFrame() = 37;
}

void ModesAndNoop(MMDApp* app, SharedHistory& history, Mode mode) {
    ResetCase(app, history, mode);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    const auto asset = picture::Current(app);
    auto* texture = app->PictureBackgroundTexture();
    const Placement initial(app);
    // Grid is itself an Adjustment. Build its real record under AllOperations
    // first, then exercise the requested mode with that existing redo branch.
    Check(history.SetMode(Mode::AllOperations) == MMS_OK, "temporarily enable recording for the grid redo fixture");
    const auto importCount = history.UndoCount();
    const auto grid = app->GroundGridEnabled();
    picture::Menu(app, 215);
    Check(history.UndoCount() == importCount + 1 && app->GroundGridEnabled() != grid && picture::Current(app) == asset,
          "grid fixture really records one adjustment without replacing the imported picture");
    Check(history.History(false) == MMS_OK && app->GroundGridEnabled() == grid && history.RedoCount() == 1,
          "prepare a real grid redo branch before picture preview");
    Check(history.SetMode(mode) == MMS_OK && history.Mode() == mode && history.UndoCount() == importCount &&
          history.RedoCount() == 1 && picture::Current(app) == asset,
          "restore the requested recording mode without discarding the grid redo fixture");
    app->SceneModified() = 0;
    const auto undo = history.UndoCount(), redo = history.RedoCount();
    const auto revision = Revision(history);
    const auto sourceBytes = media::GlobalPictureSourceBudget().Usage().used;
    Start(app, history, 2);
    const auto serial = PictureGestureSerial(*app);
    Check(BeginPictureGesture(*app, history, app->MainWindow(), 2) != MMS_OK && PictureGestureSerial(*app) == serial,
          "a duplicate begin cannot replace the owned gesture");
    for (int i = 1; i != 5; ++i) {
        Preview(app, initial.x + i * 7, initial.y - i * 3, initial.scale);
        Check(history.UndoCount() == undo && history.RedoCount() == redo && Revision(history) == revision &&
              !app->SceneModified() && app->CurrentFrame() == 37 && picture::Current(app) == asset &&
              app->PictureBackgroundTexture() == texture && media::GlobalPictureSourceBudget().Usage().used == sourceBytes,
              "preview neither publishes history nor marks dirty nor allocates a new picture source or texture");
    }
    End(app, true);
    Check(app->SceneModified() && app->CurrentFrame() == 37 && !history.LastPublicationRequiresPhysicsReset(),
          "completed placement marks the project dirty without moving the playhead or resetting physics");
    const Placement applied(app);
    if (mode == Mode::AllOperations) {
        Check(history.UndoCount() == undo + 1 && !history.RedoCount(),
              "all-operations combines four placement previews into one node and replaces redo");
        picture::Replay(app, history, false);
        Check(initial.Matches(app) && app->PictureBackgroundTexture() == texture,
              "undo placement restores original coordinates using the existing texture");
        picture::Replay(app, history, true);
        Check(applied.Matches(app) && app->PictureBackgroundTexture() == texture,
              "redo placement restores final preview without rebuilding the texture");
    } else {
        Check(history.UndoCount() == undo && history.RedoCount() == redo,
              "explicit-only placement leaves the existing redo branch intact");
        Check(history.History(true) == MMS_OK && app->GroundGridEnabled() != grid && applied.Matches(app),
              "the unrelated real redo entry remains usable after an unrecorded placement");
    }
    // A round trip must preserve even an existing redo branch and clean flag.
    Check(history.History(false) == MMS_OK, "create redo for no-op gesture checks");
    app->SceneModified() = 0;
    const picture::Snapshot before(app, history);
    const Placement start(app);
    Start(app, history);
    Preview(app, start.x + 8, start.y - 5, start.scale * .75f);
    Preview(app, start.x, start.y, start.scale);
    End(app, false);
    Check(before.Matches(app, history), "round-trip placement keeps the entire history cursor, redo and clean state");
    Start(app, history);
    End(app, false);
    Check(before.Matches(app, history), "click without preview is a complete no-op");
}

void Cancellation(MMDApp* app, SharedHistory& history, Mode mode) {
    ResetCase(app, history, mode);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    app->SceneModified() = 0;
    const picture::Snapshot before(app, history);
    const Placement initial(app);
    Start(app, history);
    Preview(app, initial.x + 11, initial.y - 9, initial.scale * .5f);
    Check(CancelPictureGesture(*app) && !PictureGestureActive(*app) && before.Matches(app, history),
          "cancel restores owned placement and leaves history, resource identity and dirty state unchanged");
    Check(!CancelPictureGesture(*app) && before.Matches(app, history), "repeated cancellation is harmless");
    Start(app, history);
    Preview(app, initial.x + 11, initial.y - 9, initial.scale * .5f);
    app->PictureOffsetY() = initial.y + 101; // Deliberate foreign author.
    CancelPictureGesture(*app);
    Check(!PictureGestureActive(*app) && app->PictureOffsetX() == initial.x &&
          app->PictureOffsetY() == initial.y + 101 && app->PictureScale() == initial.scale &&
          history.UndoCount() == before.undo && history.RedoCount() == before.redo && Revision(history) == before.historyRevision,
          "cancellation rolls back owned x and scale while preserving a foreign y edit");
    const Placement foreign(app);
    Start(app, history);
    Preview(app, foreign.x + 1, foreign.y + 2, foreign.scale * .75f);
    app->PictureOffsetX() = foreign.x + 301;
    const auto foreignX = app->PictureOffsetX();
    Check(PreviewPictureGesture(*app, 1, 2, 1) != MMS_OK && app->PictureOffsetX() == foreignX,
          "conflicting foreign state rejects the next preview instead of overwriting it");
    CancelPictureGesture(*app);
    Check(!PictureGestureActive(*app) && app->PictureOffsetX() == foreignX &&
          app->PictureOffsetY() == foreign.y && app->PictureScale() == foreign.scale,
          "conflict cleanup preserves foreign x and restores the remaining owned fields");
    picture::Presentation(app);
}

void MetadataBudget(MMDApp* app, SharedHistory& history, Mode mode) {
    ResetCase(app, history, mode);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    const auto asset = picture::Current(app);
    auto* texture = app->PictureBackgroundTexture();
    Check(history.Reset() == MMS_OK, "discard resource-owning import record before isolating metadata cost");
    history.Collect();
    Check(history.Configure(mode, 128, 16ull * 1024 * 1024) == MMS_OK, "set small normal journal budget for resident image placement");
    const auto sourceBytes = media::GlobalPictureSourceBudget().Usage().used;
    const auto retained = history.RetainedBytes();
    const auto assetOwners = asset.use_count(), sourceOwners = asset->Source().use_count();
    const Placement initial(app);
    Start(app, history);
    Preview(app, initial.x + 3, initial.y - 4, initial.scale * .625f);
    Check(asset.use_count() == assetOwners && asset->Source().use_count() == sourceOwners,
          "active gesture owns stable metadata rather than an extra retained asset or encoded source lease");
    End(app, true);
    Check(picture::Current(app) == asset && app->PictureBackgroundTexture() == texture &&
          media::GlobalPictureSourceBudget().Usage().used == sourceBytes &&
          history.RetainedBytes() >= retained && history.RetainedBytes() - retained < 65536,
          "placement charges only bounded metadata, never a duplicate source or conservative GPU payload");
    Check(asset.use_count() == assetOwners && asset->Source().use_count() == sourceOwners,
          "completed placement record does not retain its asset or source outside the resource-owning history");
    if (mode == Mode::AllOperations) {
        Check(history.UndoCount() == 1, "metadata-only recorded gesture fits below resident texture payload");
        picture::Replay(app, history, false);
        Check(initial.Matches(app), "low-budget metadata undo remains valid");
        picture::Replay(app, history, true);
    } else Check(!history.UndoCount(), "low-budget explicit-only preview does not retain a node");
    Check(history.Configure(Mode::AllOperations, 128, 1) == MMS_OK, "use intentionally insufficient internal journal limit for admission failure");
    const picture::Snapshot denied(app, history);
    Check(BeginPictureGesture(*app, history, app->MainWindow(), 1) == MMS_E_MEMORY && !PictureGestureActive(*app) &&
          denied.Matches(app, history), "failed gesture reservation preserves live placement and journal atomically");
    Check(history.Configure(mode, 128, picture::largeBudget) == MMS_OK, "restore normal journal limit after isolated budget checks");
}

void ReplacementChronology(MMDApp* app, SharedHistory& history) {
    ResetCase(app, history, Mode::AllOperations);
    picture_reset_checks::TempBitmap a, b;
    picture::Import(app, a.path.c_str());
    const auto first = picture::Current(app);
    const Placement a0(app);
    Start(app, history); Preview(app, 23, -11, .75f); End(app, true);
    const Placement a1(app);
    picture::Import(app, b.path.c_str());
    const auto second = picture::Current(app);
    const Placement b0(app);
    Start(app, history); Preview(app, -7, 19, .625f); End(app, true);
    const Placement b1(app);
    const auto grid = app->GroundGridEnabled();
    picture::Menu(app, 215);
    bool changed = false;
    Check(ClearBoundPicture(*app, changed) == MMS_OK && changed && history.UndoCount() == 6,
          "imports, placements, grid and clear share one six-node chronology");
    a.Remove(); b.Remove();
    for (unsigned cycle = 0; cycle != 2; ++cycle) {
        picture::Replay(app, history, false);
        Check(picture::Current(app) == second && b1.Matches(app), "undo clear restores B with its final recorded placement");
        Check(history.History(false) == MMS_OK && app->GroundGridEnabled() == grid && b1.Matches(app),
              "undo interleaved grid leaves B untouched");
        picture::Replay(app, history, false);
        Check(picture::Current(app) == second && b0.Matches(app), "undo B gesture owns the restored B asset");
        picture::Replay(app, history, false);
        Check(picture::Current(app) == first && a1.Matches(app), "undo replacement restores A at its completed gesture result");
        picture_reset_checks::VerifyPixels(app->Renderer()->device, first->Source());
        picture::Replay(app, history, false);
        Check(a0.Matches(app), "older A gesture remains replayable after a replacement changes activation tickets");
        picture::Replay(app, history, false);
        Check(!picture::Current(app) && !history.UndoCount() && history.RedoCount() == 6,
              "undo entire mixed sequence reaches the true empty baseline");
        picture::Replay(app, history, true); Check(picture::Current(app) == first && a0.Matches(app), "redo imports retained A without its original file");
        picture::Replay(app, history, true); Check(a1.Matches(app), "redo applies A placement");
        picture::Replay(app, history, true); Check(picture::Current(app) == second && b0.Matches(app), "redo imports retained B without its original file");
        picture_reset_checks::VerifyPixels(app->Renderer()->device, second->Source());
        picture::Replay(app, history, true); Check(b1.Matches(app), "redo applies B placement");
        Check(history.History(true) == MMS_OK && app->GroundGridEnabled() != grid, "redo interleaved grid");
        picture::Replay(app, history, true); Check(!picture::Current(app) && history.UndoCount() == 6, "redo clear finishes the mixed chronology");
    }
}

void ExplicitDraftReplacement(MMDApp* app, SharedHistory& history) {
    ResetCase(app, history, Mode::ExplicitOnly);
    picture_reset_checks::TempBitmap a, b;
    picture::Import(app, a.path.c_str());
    const auto first = picture::Current(app);
    Start(app, history); Preview(app, 39, -21, .8125f); End(app, true);
    const Placement draft(app);
    Check(history.UndoCount() == 1, "explicit-only drag remains a draft after the explicit import");
    picture::Import(app, b.path.c_str());
    const auto second = picture::Current(app);
    Check(history.UndoCount() == 2, "replacement contributes one explicit node after the unregistered drag");
    a.Remove(); b.Remove();
    for (unsigned cycle = 0; cycle != 2; ++cycle) {
        picture::Replay(app, history, false);
        Check(picture::Current(app) == first && draft.Matches(app),
              "undo replacement remembers the actual unregistered gesture result on the asset being left");
        picture::Replay(app, history, true);
        Check(picture::Current(app) == second, "redo replacement remains valid after restoring the draft placement");
    }
}

void ReplayFieldOwnership(MMDApp* app, SharedHistory& history) {
    ResetCase(app, history, Mode::AllOperations);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    const Placement initial(app);
    Start(app, history); Preview(app, 14, -18, .6875f); End(app, true);
    const Placement final(app);
    // A foreign visibility change is deliberately not represented by a node.
    // The placement participant must own only x/y/scale on replay.
    app->PictureBackgroundEnabled() = 0;
    picture::Replay(app, history, false);
    Check(initial.Matches(app) && !app->PictureBackgroundEnabled(),
          "undo placement does not reveal a picture hidden by another author");
    picture::Replay(app, history, true);
    Check(final.Matches(app) && !app->PictureBackgroundEnabled(),
          "redo placement preserves current visibility as well");
}

void Lifecycle(MMDApp* app, SharedHistory& history) {
    ResetCase(app, history, Mode::AllOperations);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    const auto old = picture::Current(app);
    const Placement initial(app);
    Start(app, history); Preview(app, 44, -12, .5f);
    picture::Import(app, image.path.c_str());
    const auto replacement = picture::Current(app);
    Check(replacement != old && replacement->Id() != old->Id() && !PictureGestureActive(*app) && history.UndoCount() == 2,
          "real same-path reload cancels an unfinished gesture before publishing the distinct asset");
    const picture::Snapshot replaced(app, history);
    bool changed = true;
    Check(PreviewPictureGesture(*app, 90, 91, 2) != MMS_OK && EndPictureGesture(*app, changed) != MMS_OK && !changed &&
          replaced.Matches(app, history), "late old preview/end cannot edit the replacement or append history");
    picture::Replay(app, history, false);
    Check(picture::Current(app) == old && initial.Matches(app), "replacement undo restores pre-gesture placement, not cancelled scratch");
    picture_reset_checks::TempBitmap missing;
    missing.Remove();
    const picture::Snapshot beforeFailedReplace(app, history);
    Start(app, history); Preview(app, 55, 66, .5f);
    changed = true;
    Check(ImportBoundPicture(*app, missing.path.c_str(), changed) != MMS_OK && !changed &&
          !PictureGestureActive(*app) && beforeFailedReplace.Matches(app, history),
          "a failed real replacement still cancels scratch before preserving the old image and redo branch");
    Start(app, history); Preview(app, 33, 22, .75f);
    const auto generation = BoundNativePoseGeneration(app);
    { NativePoseTestBinding nested(*app, history); Check(BoundNativePoseGeneration(app) != generation, "exercise a real temporary history binding change"); }
    Check(!PictureGestureActive(*app) && initial.Matches(app), "binding replacement cancels preview and releases its transaction");
    Start(app, history); Preview(app, 12, 13, .875f);
    const auto scene = app->SceneGeneration();
    ResetAppState(app);
    Check(app->SceneGeneration() > scene && !PictureGestureActive(*app), "actual new-project reset cancels the live gesture before changing scene generation");
    const Placement reset(app);
    Check(PreviewPictureGesture(*app, 99, 98, 2) != MMS_OK && reset.Matches(app), "stale scene preview cannot modify retained reset picture placement");
    // Reset retains the asset but intentionally hides it; use the real command
    // to make the following isolated cases eligible for an interactive gesture.
    changed = false;
    Check(SetBoundPictureVisibility(*app, true, changed) == MMS_OK && app->PictureBackgroundEnabled(),
          "real visibility publication makes the reset-retained picture available again");
    picture::Presentation(app);
}

void InvalidAndUnavailable(MMDApp* app, SharedHistory& history) {
    ResetCase(app, history, Mode::AllOperations);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    const picture::Snapshot before(app, history);
    for (int operation : {0, 3, 15, 21})
        Check(BeginPictureGesture(*app, history, app->MainWindow(), operation) != MMS_OK &&
              !PictureGestureActive(*app) && before.Matches(app, history),
              "non-background operation cannot reserve a picture gesture or modify state");
    for (float scale : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        Start(app, history);
        Check(PreviewPictureGesture(*app, 101, 102, scale) != MMS_OK,
              "invalid preview scale is rejected before any placement field is stored");
        CancelPictureGesture(*app);
        Check(before.Matches(app, history), "invalid preview and cancellation preserve all source fields and chronology");
    }
    Start(app, history); Preview(app, 21, -9, .5f);
    auto* device = app->Renderer()->device;
    app->Renderer()->device = nullptr;
    const auto result = PreviewPictureGesture(*app, 999, 888, 2);
    CancelPictureGesture(*app);
    app->Renderer()->device = device;
    Check(result != MMS_OK && !PictureGestureActive(*app) && before.Matches(app, history),
          "unavailable renderer rejects preview and rolls back owned fields without using an absent device");
    picture::Presentation(app);
    // This is a readiness failure, not a claim that a real D3D device reset or
    // full resource reconstruction has been verified.
}

void DeferredRefresh(MMDApp* app, SharedHistory& history) {
    ResetCase(app, history, Mode::AllOperations);
    picture_reset_checks::TempBitmap image;
    picture::Import(app, image.path.c_str());
    const auto count = history.UndoCount();
    Start(app, history); Preview(app, 31, -17, .875f);
    auto* vertices = app->PictureOverlayVertices();
    Check(vertices != nullptr, "refresh failure fixture owns a real vertex buffer");
    app->PictureOverlayVertices() = nullptr;
    bool changed = false;
    const auto status = EndPictureGesture(*app, changed);
    const auto pending = app->PendingPictureUiTicket();
    app->PictureOverlayVertices() = vertices;
    Check(status == MMS_OK && changed && !PictureGestureActive(*app) && pending && history.UndoCount() == count + 1,
          "failed UI refresh does not turn an already committed gesture into failure or lose its pending ticket");
    {
        identity::Mutation mutation(app, true, false, false, true);
        RefreshPictureGesture(*app);
        Check(app->PendingPictureUiTicket() == pending, "gesture refresh preserves pending work through a temporary media mutation");
    }
    RefreshPictureGesture(*app);
    Check(!app->PendingPictureUiTicket() && history.UndoCount() == count + 1,
          "safe deferred refresh does not duplicate the committed placement node");
    picture::Presentation(app);
    picture::Replay(app, history, false);
}

void FrameInput(MMDApp* app, SharedHistory& history) {
    namespace input=cursor_checks;
    input::ScopedInputHooks guards;
    const auto main=app->MainWindow();
    const auto savedScale=app->Renderer()->viewScale;
    app->Renderer()->viewScale=1.2f;
    app->state.optflag[0]=1;app->CameraMode()=1;app->CameraParentModel()=-1;
    const double factor=1.2000000476837158/app->Renderer()->viewScale;
    const auto camera=[&]{return std::array<float,6>{app->CameraDistance(),app->CameraPositionX(),app->CameraPositionY(),
        app->CameraPositionZ(),app->ViewOffsetX(),app->ViewOffsetY()};};
    const auto press=[&](int operation) {
        input::capture=input::foreground=main;input::held=input::control=true;
        app->LeftMouseButtonState()=0;app->CtrlModifierState()=1;
        const auto panels=FramePanelLayout(app);
        app->MouseX()=panels.TopX(operation==1?48:18);app->MouseY()=panels.TopY(-12);
        app->PreviousMouseX()=app->MouseX();app->PreviousMouseY()=app->MouseY();
        input::pointer={app->MouseX(),app->MouseY()};ClientToScreen(main,&input::pointer);
        Check(HandleViewportToolPress(app),"actual header hit test consumes the requested Ctrl tool press");
    };
    const auto position=[&](int dx,int dy) {
        input::pointer={app->PreviousMouseX()+dx,app->PreviousMouseY()+dy};
        Check(ClientToScreen(main,&input::pointer),"convert controlled final pointer sample through the real viewport window");
    };
    const auto move=[&](int dx,int dy) {position(dx,dy);MouseInteractionBegin(app);MouseInteractionEnd(app);};
    const auto loadInputPicture=[&](const wchar_t* path) {
        picture::Import(app,path);
        // The real importer fits an 8x8 fixture to the viewport and can choose
        // a starting scale above the legacy drag clamp of 100. Final-sample
        // assertions need an unclamped baseline, not that import-fit value.
        app->PictureOffsetX()=0;app->PictureOffsetY()=0;app->PictureScale()=1;
        app->PendingPictureUiTicket()=identity::CurrentPictureTicket(app,identity::Instance(app));
        Check(app->PendingPictureUiTicket()!=0 && RefreshBoundPictureUi(*app),
              "establish an in-range input fixture and refresh its actual placement geometry");
        picture::Presentation(app);
    };
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}) for(bool windowRelease:{false,true}) for(int operation:{1,2}) {
        input::capture=nullptr;
        ResetCase(app,history,mode);picture_reset_checks::TempBitmap image;loadInputPicture(image.path.c_str());
        const Placement initial(app);const auto beforeCamera=camera();const auto count=history.UndoCount();
        press(operation);
        std::printf("DIAG picture input mode=%s release=%s op=%d initial=(%d,%d,%.9g) size=%dx%d afterpress mouse=(%d,%d) previous=(%d,%d)\n",
            mode==Mode::AllOperations?"all":"explicit",windowRelease?"window":"poll",operation,
            initial.x,initial.y,double(initial.scale),app->PictureWidth(),app->PictureHeight(),
            app->MouseX(),app->MouseY(),app->PreviousMouseX(),app->PreviousMouseY());
        Check(PictureGestureActive(*app) && OwnsCenteredViewportDrag(app),"production header starts a bound picture gesture");
        // The operation remains a picture gesture even after Ctrl is released.
        input::control=false;
        move(5,-3);
        std::printf("DIAG picture input firstmove actual=(%d,%d,%.9g) mouse=(%d,%d) previous=(%d,%d)\n",
            app->PictureOffsetX(),app->PictureOffsetY(),double(app->PictureScale()),
            app->MouseX(),app->MouseY(),app->PreviousMouseX(),app->PreviousMouseY());
        Check(history.UndoCount()==count && camera()==beforeCamera,"frame preview changes picture only and has not yet appended history");
        position(2,-1);input::held=false;
        if(windowRelease) {
            // lParam deliberately carries stale pre-warp coordinates. The real
            // bound window router must consume GetCursorPos's latest sample.
            Check(HandleBoundViewportMessage(*app,main,WM_LBUTTONUP,0,MAKELPARAM(1,1)),
                  "actual bound window router handles picture release");
        } else {MouseInteractionBegin(app);MouseInteractionEnd(app);}
        Check(!PictureGestureActive(*app) && !OwnsCenteredViewportDrag(app) && !input::capture && input::visibility==0,
              "both production release paths end the transaction and restore cursor ownership");
        std::printf("DIAG picture input released mode=%s release=%s op=%d actual=(%d,%d,%.9g) expectedPan=(%lld,%lld) expectedZoom=%.9g centerBefore=(%.9g,%.9g) centerAfter=(%.9g,%.9g)\n",
            mode==Mode::AllOperations?"all":"explicit",windowRelease?"window":"poll",operation,
            app->PictureOffsetX(),app->PictureOffsetY(),double(app->PictureScale()),
            static_cast<long long>(std::llround(initial.x+7*factor)),static_cast<long long>(std::llround(initial.y-4*factor)),
            initial.scale*std::exp(.04),initial.x+app->PictureWidth()*initial.scale*.5,
            initial.y+app->PictureHeight()*initial.scale*.5,
            app->PictureOffsetX()+app->PictureWidth()*app->PictureScale()*.5,
            app->PictureOffsetY()+app->PictureHeight()*app->PictureScale()*.5);
        if(operation==2) {
            Check(app->PictureOffsetX()==std::llround(initial.x+7*factor) &&
                  app->PictureOffsetY()==std::llround(initial.y-4*factor) && app->PictureScale()==initial.scale,
                  "pan includes the unsampled final release delta exactly once");
        } else {
            const auto expected=initial.scale*std::exp(.04);
            Check(std::fabs(app->PictureScale()-expected)<.00001 &&
                  std::abs(app->PictureOffsetX()+app->PictureWidth()*app->PictureScale()*.5-
                           (initial.x+app->PictureWidth()*initial.scale*.5))<=.51 &&
                  std::abs(app->PictureOffsetY()+app->PictureHeight()*app->PictureScale()*.5-
                           (initial.y+app->PictureHeight()*initial.scale*.5))<=.51,
                  "zoom includes the final release sample and keeps the actual picture center");
        }
        Check(history.UndoCount()==count+(mode==Mode::AllOperations?1:0) && camera()==beforeCamera,
              "real input path honors recording mode and never falls through to camera editing");
        const Placement complete(app);const auto completeCount=history.UndoCount();
        input::pointer={0,0};HandleViewportToolRelease(app);
        Check(complete.Matches(app) && history.UndoCount()==completeCount,
              "duplicate release cannot replay the final sample or append a second node");
        picture::Presentation(app);
    }
    // Simulated OS warp denial still runs production centering and delta math.
    input::capture=nullptr;ResetCase(app,history,Mode::AllOperations);
    picture_reset_checks::TempBitmap image;loadInputPicture(image.path.c_str());
    const Placement initial(app);const auto count=history.UndoCount();
    input::warpSucceeds=false;press(2);
    const POINT unwarped{app->MouseX(),app->MouseY()};
    Check(PictureGestureActive(*app) && app->PreviousMouseX()==unwarped.x && app->PreviousMouseY()==unwarped.y,
          "bound gesture preserves the sampled header position when initial warp fails");
    MouseInteractionBegin(app);MouseInteractionEnd(app);
    Check(initial.Matches(app),"failed initial warp creates no picture movement at a stationary pointer");
    move(4,-2);const Placement once(app);
    MouseInteractionBegin(app);MouseInteractionEnd(app);
    Check(once.Matches(app),"failed subsequent centering cannot repeat the same picture delta");
    input::held=false;
    Check(HandleBoundViewportMessage(*app,main,WM_LBUTTONUP,0,0) && history.UndoCount()==count+1,
          "denied warps do not prevent a real subsequent movement from committing once");
    Check(app->PictureOffsetX()==std::llround(initial.x+4*factor) && app->PictureOffsetY()==std::llround(initial.y-2*factor),
          "bound failed-warp gesture contains only the actual four-by-minus-two pointer movement");
    input::warpSucceeds=true;
    const auto beforeUnreadable=history.UndoCount();
    press(2);move(5,2);const Placement lastReadable(app);
    position(100,100);input::positionSucceeds=false;input::held=false;
    const auto released=HandleBoundViewportMessage(*app,main,WM_LBUTTONUP,0,MAKELPARAM(777,666));
    input::positionSucceeds=true;
    Check(released && !PictureGestureActive(*app) && lastReadable.Matches(app) && history.UndoCount()==beforeUnreadable+1,
          "failed final cursor query commits only the last accepted frame sample and ignores stale message coordinates");
    const auto cancellation=[&](UINT message,WPARAM wp,LPARAM lp) {
        const Placement before(app);const auto nodes=history.UndoCount();const auto revision=Revision(history);
        press(2);move(7,8);Check(!before.Matches(app),"prepare a real frame preview before cancellation");
        if(message==WM_CAPTURECHANGED)input::capture=nullptr;
        HandleBoundViewportMessage(*app,main,message,wp,lp);
        Check(!PictureGestureActive(*app) && !OwnsCenteredViewportDrag(app) && before.Matches(app) &&
              history.UndoCount()==nodes && Revision(history)==revision && !input::capture && input::visibility==0,
              "real input cancellation restores preview without creating history or retaining capture");
        input::held=false;HandleViewportToolRelease(app);
    };
    cancellation(WM_KEYDOWN,VK_ESCAPE,0);
    cancellation(WM_CANCELMODE,0,0);
    cancellation(WM_CAPTURECHANGED,0,0);
    // The visible video is intentionally a metadata-only priority fixture, not
    // an AVI codec test. Bound video drag remains unavailable in this slice.
    const auto beforeVideo=camera();const Placement pictureBeforeVideo(app);const auto nodes=history.UndoCount();
    app->AviBackgroundEnabled()=1;app->AviFrameWidth()=160;app->AviFrameHeight()=96;
    const auto aviX=app->AviOffsetX(),aviY=app->AviOffsetY();const auto aviScale=app->AviScale();
    press(2);move(9,10);input::held=false;HandleViewportToolRelease(app);
    Check(!PictureGestureActive(*app) && pictureBeforeVideo.Matches(app) && camera()==beforeVideo &&
          app->AviOffsetX()==aviX && app->AviOffsetY()==aviY && app->AviScale()==aviScale && history.UndoCount()==nodes,
          "topmost video priority never edits the underlying picture or bypasses unavailable video history");
    app->AviBackgroundEnabled()=0;app->AviFrameWidth()=app->AviFrameHeight()=0;
    input::capture=nullptr;input::held=input::control=false;app->CtrlModifierState()=0;
    app->Renderer()->viewScale=savedScale;
    app->PendingPictureUiTicket()=identity::Acquire(app,identity::Kind::Picture);
    picture::Presentation(app);
}

// Intercept only the external refresh/input boundaries. The real frame math,
// WndProc, cancellation, history and D3D refresh implementations still run.
struct SynchronousHooks {
    inline static SynchronousHooks* current=nullptr;
    using Request=void(*)(MMDApp*,bool);
    Request original=nullptr;
    MMDApp* app;SharedHistory& history;
    UINT message=0;bool refreshObserved=false,releaseArmed=false,releaseObserved=false;
    MmsResult nestedBegin=MMS_OK;
    int afterWarps=0;std::array<int,4> afterMouse{};
    SynchronousHooks(MMDApp* a,SharedHistory& h):app(a),history(h){
        Check(!current,"one synchronous picture input observer");current=this;
        Check(MH_CreateHook(reinterpret_cast<void*>(&updates::RequestViewport),reinterpret_cast<void*>(&Refresh),
            reinterpret_cast<void**>(&original))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&updates::RequestViewport))==MH_OK,
            "observe real viewport refresh while forwarding production work");
        Check(MH_DisableHook(reinterpret_cast<void*>(&ReleaseCapture))==MH_OK&&
            MH_RemoveHook(reinterpret_cast<void*>(&ReleaseCapture))==MH_OK&&
            MH_CreateHook(reinterpret_cast<void*>(&ReleaseCapture),reinterpret_cast<void*>(&Release),nullptr)==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&ReleaseCapture))==MH_OK,
            "extend isolated capture boundary with synchronous loss and reentry");
    }
    ~SynchronousHooks(){
        MH_DisableHook(reinterpret_cast<void*>(&updates::RequestViewport));MH_RemoveHook(reinterpret_cast<void*>(&updates::RequestViewport));
        MH_DisableHook(reinterpret_cast<void*>(&ReleaseCapture));MH_RemoveHook(reinterpret_cast<void*>(&ReleaseCapture));
        Check(MH_CreateHook(reinterpret_cast<void*>(&ReleaseCapture),reinterpret_cast<void*>(&cursor_checks::Release),nullptr)==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&ReleaseCapture))==MH_OK,"restore enclosing isolated input hook");
        current=nullptr;
    }
    static void Refresh(MMDApp* app,bool changed){
        auto& self=*current;self.original(app,changed);
        if(app!=self.app||!self.message)return;
        const auto message=self.message;self.message=0;self.refreshObserved=true;
        if(message==WM_CLOSE)Check(SceneCloseBlocked(*app),"picture refresh holds the real scene-close barrier before callback");
        SendMessageW(app->MainWindow(),message,0,0);
        Check(IsWindow(app->MainWindow())&&app->Renderer()&&app->PictureBackgroundTexture(),
            "synchronous production callback leaves the live host and picture resources valid");
        if(message==WM_CLOSE)Check(app->deferredSceneMainClose==app->MainWindow()&&!app->sceneMainCloseProcessing,
            "real WM_CLOSE is deferred rather than entering destruction inside refresh");
        self.afterWarps=cursor_checks::warps;
        self.afterMouse={app->MouseX(),app->MouseY(),app->PreviousMouseX(),app->PreviousMouseY()};
    }
    static BOOL WINAPI Release(){
        auto& self=*current;const auto owner=cursor_checks::capture;cursor_checks::capture=nullptr;
        if(owner)SendMessageW(owner,WM_CAPTURECHANGED,0,0);
        if(self.releaseArmed){self.releaseArmed=false;self.releaseObserved=true;
            self.nestedBegin=BeginPictureGesture(*self.app,self.history,self.app->MainWindow(),2);}
        return TRUE;
    }
};
void SynchronousReentry(MMDApp* app,SharedHistory& history){
    namespace input=cursor_checks;
    input::ScopedInputHooks inputGuards;
    SynchronousHooks hooks(app,history);
    const auto main=app->MainWindow();
    const auto oldScale=app->Renderer()->viewScale;app->Renderer()->viewScale=1.2f;
    app->state.optflag[0]=1;app->CameraMode()=1;app->CameraParentModel()=-1;
    const auto press=[&]{
        input::capture=input::foreground=main;input::held=input::control=true;
        app->LeftMouseButtonState()=0;app->CtrlModifierState()=1;
        const auto panels=FramePanelLayout(app);app->MouseX()=panels.TopX(18);app->MouseY()=panels.TopY(-12);
        app->PreviousMouseX()=app->MouseX();app->PreviousMouseY()=app->MouseY();
        input::pointer={app->MouseX(),app->MouseY()};Check(ClientToScreen(main,&input::pointer)!=FALSE,"map synchronous header press");
        Check(HandleViewportToolPress(app)&&PictureGestureActive(*app),"start actual header gesture for synchronous callback");
    };
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        input::capture=nullptr;ResetCase(app,history,mode);picture_reset_checks::TempBitmap image;picture::Import(app,image.path.c_str());
        app->PictureOffsetX()=0;app->PictureOffsetY()=0;app->PictureScale()=1;app->SceneModified()=0;
        const picture::Snapshot baseline(app,history);
        press();hooks.message=WM_CANCELMODE;hooks.refreshObserved=false;
        // Force a location beyond the viewport so a stale outer continuation
        // would attempt to recenter after the callback cancelled the session.
        input::pointer={app->ViewportRect().right+17,app->ViewportRect().bottom+19};
        Check(ClientToScreen(main,&input::pointer)!=FALSE,"map out-of-bounds synchronous preview sample");
        MouseInteractionBegin(app);
        Check(hooks.refreshObserved&&!PictureGestureActive(*app)&&!OwnsCenteredViewportDrag(app)&&
            baseline.Matches(app,history)&&!input::capture&&input::visibility==0&&input::warps==hooks.afterWarps&&
            hooks.afterMouse==std::array<int,4>{app->MouseX(),app->MouseY(),app->PreviousMouseX(),app->PreviousMouseY()},
            "refresh cancellation restores preview and old frame continuation neither writes mouse state nor recenters");
        MouseInteractionEnd(app);
        press();Preview(app,7,-9,.75f);hooks.releaseArmed=true;hooks.releaseObserved=false;input::held=false;
        bool changed=false;Check(EndPictureGesture(*app,changed)==MMS_OK&&changed,"commit picture before capture-release callback");
        Check(hooks.releaseObserved&&hooks.nestedBegin==MMS_E_BUSY&&!PictureGestureActive(*app)&&!input::capture&&input::visibility==0,
            "synchronous capture-loss callback cannot begin through detached-session cleanup barrier");
        Check(BeginPictureGesture(*app,history,main,2)==MMS_OK&&PictureGestureActive(*app),
            "same begin succeeds after outer cleanup releases its barrier");
        Check(CancelPictureGesture(*app),"dispose successful post-cleanup admission");
        picture::Presentation(app);
    }
    input::capture=nullptr;ResetCase(app,history,Mode::AllOperations);picture_reset_checks::TempBitmap image;picture::Import(app,image.path.c_str());
    Check(!app->deferredSceneMainClose&&!app->sceneMainClosePosted,"close reentry fixture has no preexisting deferred request");
    app->PictureScale()=1;press();hooks.message=WM_CLOSE;hooks.refreshObserved=false;
    input::pointer={app->PreviousMouseX()+2,app->PreviousMouseY()+3};
    Check(ClientToScreen(main,&input::pointer)!=FALSE,"map close-reentry preview sample");
    MouseInteractionBegin(app);MouseInteractionEnd(app);
    Check(hooks.refreshObserved&&IsWindow(main)&&app->deferredSceneMainClose==main&&app->sceneMainClosePosted,
        "outer frame returns alive and posts deferred close only after its guard exits");
    // The fixture verifies deferral, not host shutdown. Remove only its own
    // deferred replay message without dispatching a save dialog or destruction.
    MSG queued{};Check(PeekMessageW(&queued,main,kSceneCloseReplayMessage,kSceneCloseReplayMessage,PM_REMOVE)!=FALSE,
        "real deferred close replay is present in the host queue");
    app->deferredSceneMainClose=nullptr;app->sceneMainClosePosted=false;
    Check(CancelPictureGesture(*app),"cancel preview after isolated close-deferral observation");
    input::held=input::control=false;app->CtrlModifierState()=0;app->Renderer()->viewScale=oldScale;
    app->PendingPictureUiTicket()=identity::Acquire(app,identity::Kind::Picture);
    picture::Presentation(app);
}

void Run(MMDApp* app) {
    Check(app && app->Renderer() && app->Renderer()->device && !IsWindowVisible(app->MainWindow()),
          "picture gesture checks use the complete hidden production D3D host");
    Shutdown();
    Check(InitializeHistoryTestRuntime(app), "bind real shared history for picture gesture tests");
    auto* history = NativeHistory(app);
    Check(history != nullptr, "obtain the real common journal for picture gestures");
    picture::ReadableGeometry geometry(app);
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        std::printf("DIAG picture gesture mode=%s modes/noop\n", mode == Mode::AllOperations ? "all" : "explicit");
        ModesAndNoop(app, *history, mode);
        std::puts("DIAG picture gesture cancellation");
        Cancellation(app, *history, mode);
        std::puts("DIAG picture gesture metadata budget");
        MetadataBudget(app, *history, mode);
    }
    std::puts("DIAG picture gesture replacement chronology");
    ReplacementChronology(app, *history);
    std::puts("DIAG picture gesture explicit draft replacement");
    ExplicitDraftReplacement(app, *history);
    std::puts("DIAG picture gesture replay field ownership");
    ReplayFieldOwnership(app, *history);
    std::puts("DIAG picture gesture lifecycle");
    Lifecycle(app, *history);
    std::puts("DIAG picture gesture invalid/unavailable");
    InvalidAndUnavailable(app, *history);
    std::puts("DIAG picture gesture deferred refresh");
    DeferredRefresh(app, *history);
    std::puts("DIAG picture gesture actual frame/window input");
    FrameInput(app, *history);
    std::puts("DIAG picture gesture synchronous cancel capture cleanup and close deferral");
    SynchronousReentry(app,*history);
    ResetCase(app, *history, Mode::ExplicitOnly);
    Check(!GetCapture() && !IsWindowVisible(app->MainWindow()), "backend test never captures the desktop mouse or shows its window");
    std::puts("PASS native picture gesture common history: modes metadata budget cancellation retained chronology and deferred refresh");
}
}
