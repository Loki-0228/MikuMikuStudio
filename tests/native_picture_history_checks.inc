// Real hidden production host + actual common journal. No D3D, image decode,
// publication or undo implementation is substituted by this test.
#include "../src/plugins/native_picture_history.hpp"
#include "../src/plugins/native_global_commands.hpp"
#include "mikumikustudio/picture_menu.hpp"
#include <commdlg.h>
namespace native_picture_history_checks {
using namespace mikumikustudio;
using namespace mikumikustudio::plugins;
namespace identity = background_identity;
using Asset = media::BackgroundPictureAssetPtr;
constexpr std::uint64_t largeBudget = 512ull * 1024 * 1024;

Asset Current(MMDApp* app) {
    const auto instance = identity::Instance(app);
    return identity::CurrentPictureAsset(app, instance, identity::Acquire(app, identity::Kind::Picture));
}
struct Snapshot {
    Asset asset;
    IDirect3DTexture9* texture;
    std::wstring path;
    int x, y, width, height;
    float scale;
    unsigned visible, dirty, frame;
    std::uint64_t ticket, sceneRevision, historyRevision = 0;
    size_t undo, redo;
    Snapshot(MMDApp* app, SharedHistory& history) : asset(Current(app)), texture(app->PictureBackgroundTexture()),
        path(app->PictureBackgroundPath()), x(app->PictureOffsetX()), y(app->PictureOffsetY()),
        width(app->PictureWidth()), height(app->PictureHeight()), scale(app->PictureScale()),
        visible(app->PictureBackgroundEnabled()), dirty(app->SceneModified()), frame(app->CurrentFrame()),
        ticket(identity::Acquire(app, identity::Kind::Picture)), sceneRevision(app->SceneRevision()),
        undo(history.UndoCount()), redo(history.RedoCount()) {
        Check(history.Revision(historyRevision) == MMS_OK, "read picture history snapshot revision");
    }
    bool Matches(MMDApp* app, SharedHistory& history) const {
        std::uint64_t revision = 0;
        return history.Revision(revision) == MMS_OK && revision == historyRevision && history.UndoCount() == undo &&
            history.RedoCount() == redo && asset == Current(app) && texture == app->PictureBackgroundTexture() &&
            path == app->PictureBackgroundPath() && x == app->PictureOffsetX() && y == app->PictureOffsetY() &&
            width == app->PictureWidth() && height == app->PictureHeight() && scale == app->PictureScale() &&
            visible == app->PictureBackgroundEnabled() && dirty == app->SceneModified() && frame == app->CurrentFrame() &&
            ticket == identity::Acquire(app, identity::Kind::Picture) && sceneRevision == app->SceneRevision();
    }
};
class ReadableGeometry {
    MMDApp* app_;
    IDirect3DVertexBuffer9* original_;
public:
    explicit ReadableGeometry(MMDApp* app) : app_(app), original_(app->PictureOverlayVertices()) {
        D3DVERTEXBUFFER_DESC desc{}; IDirect3DVertexBuffer9* readable = nullptr;
        Check(original_ && SUCCEEDED(original_->GetDesc(&desc)) && desc.Size == 168 &&
              SUCCEEDED(app->Renderer()->device->CreateVertexBuffer(desc.Size, desc.Usage & ~D3DUSAGE_WRITEONLY,
                  desc.FVF, desc.Pool, &readable, nullptr)) && readable, "create readable real VB for history refresh observation");
        app->PictureOverlayVertices() = readable;
    }
    ~ReadableGeometry() { if (app_->PictureOverlayVertices()) app_->PictureOverlayVertices()->Release(); app_->PictureOverlayVertices() = original_; }
};
void Presentation(MMDApp* app) {
    Check(RefreshBoundPictureUi(*app), "complete real picture history UI refresh");
    const auto menu = GetMenu(app->MainWindow());
    const auto visible = GetMenuState(menu, 233, MF_BYCOMMAND);
    const auto remove = GetMenuState(menu, picture_menu::kRemoveCommand, MF_BYCOMMAND);
    Check(visible != UINT(-1) && bool(visible & MF_CHECKED) == bool(app->PictureBackgroundEnabled()) &&
          remove != UINT(-1) && bool(remove & (MF_GRAYED | MF_DISABLED)) == !bool(Current(app)),
          "picture show and remove menu states follow actual history publication");
    if (!Current(app)) return;
    struct Vertex { float x, y, z, w; DWORD color; float u, v; };
    static_assert(sizeof(Vertex) == 28, "native overlay vertex shape");
    Check(!app->RecordingWindow(), "history picture geometry uses the ordinary hidden viewport");
    const double ratio = app->Renderer()->viewScale / 1.2000000476837158;
    const double left = app->ViewportRect().left + app->PictureOffsetX() * ratio - 1;
    const double top = app->ViewportRect().top + app->PictureOffsetY() * ratio - 1;
    const double right = left + 1 + app->PictureWidth() * app->PictureScale() * ratio;
    const double bottom = top + 1 + app->PictureHeight() * app->PictureScale() * ratio;
    const float xy[6][2] = {{float(right),float(top)}, {float(right),float(bottom)}, {float(left),float(top)},
                           {float(left),float(top)}, {float(right),float(bottom)}, {float(left),float(bottom)}};
    const float uv[6][2] = {{1,0},{1,1},{0,0},{0,0},{1,1},{0,1}};
    void* memory = nullptr; auto* buffer = app->PictureOverlayVertices();
    Check(buffer && SUCCEEDED(buffer->Lock(0, 168, &memory, D3DLOCK_READONLY)) && memory,
          "read actual history-refreshed picture vertices");
    Vertex actual[6]{}; std::memcpy(actual, memory, sizeof(actual));
    Check(SUCCEEDED(buffer->Unlock()), "release history geometry observation lock");
    for (unsigned i=0;i!=6;++i)
        Check(std::fabs(actual[i].x-xy[i][0]) < .001f && std::fabs(actual[i].y-xy[i][1]) < .001f &&
              actual[i].z == 0 && actual[i].w == 1 && actual[i].color == 0xffffffff &&
              actual[i].u == uv[i][0] && actual[i].v == uv[i][1], "all six picture vertices match current presentation after replay");
}
void Replay(MMDApp* app, SharedHistory& history, bool redo) {
    const auto frame = app->CurrentFrame();
    Check(history.History(redo) == MMS_OK && app->CurrentFrame() == frame && !history.LastPublicationRequiresPhysicsReset(),
          "actual picture undo redo preserves the playhead and does not request physics reset");
    Presentation(app);
}
void Import(MMDApp* app, const wchar_t* path) {
    bool changed = false;
    Check(ImportBoundPicture(*app,path,changed) == MMS_OK && changed && Current(app), "import actual BMP through common picture history");
    Presentation(app);
}
void Empty(MMDApp* app, SharedHistory& history, SharedHistory::RecordingMode mode) {
    Check(history.Configure(mode,128,largeBudget) == MMS_OK, "configure ample bounded picture-history preparation budget");
    bool changed = false;
    Check(ClearBoundPicture(*app,changed) == MMS_OK, "clear previous picture through real journal before isolated case");
    Check(history.Reset() == MMS_OK, "clear setup records before picture-history case"); history.Collect();
    picture_menu::EnsureItem(app);
    Check(!Current(app) && !app->PictureBackgroundTexture(), "picture-history case starts genuinely empty");
}
const wchar_t* picked = nullptr;
unsigned picks = 0;
BOOL WINAPI Pick(LPOPENFILENAMEW request) {
    ++picks;
    if (!picked) return FALSE;
    wcscpy_s(request->lpstrFile, request->nMaxFile, picked); return TRUE;
}
struct Picker {
    Picker() {
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetUiOpenFileNameW), reinterpret_cast<void*>(&Pick), nullptr) == MH_OK &&
              MH_EnableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW)) == MH_OK, "intercept only the real picture file-dialog choice");
    }
    ~Picker() { MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW)); MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW)); picked=nullptr; }
};
void Menu(MMDApp* app, unsigned command) { SendMessageW(app->MainWindow(), WM_COMMAND, command, 0); }
void UnactivatedPicture(MMDApp* app, SharedHistory& history) {
    Empty(app,history,SharedHistory::RecordingMode::AllOperations);
    identity::Invalidate(app,identity::Kind::Picture);
    const auto instance=identity::Instance(app);
    const auto dirty=app->SceneModified();
    std::uint64_t revision=0;
    Check(history.Revision(revision)==MMS_OK && !identity::CurrentPictureTicket(app,instance),
          "fresh empty picture has no activation before opening the dialog");
    const auto unchanged=[&] {
        std::uint64_t now=0;
        return !identity::CurrentPictureTicket(app,instance) && !app->PictureBackgroundTexture() &&
            !app->PictureBackgroundPath()[0] && !app->PictureBackgroundEnabled() &&
            app->SceneModified()==dirty && history.Revision(now)==MMS_OK && now==revision &&
            !history.UndoCount() && !history.RedoCount();
    };
    const auto count=picks;picked=nullptr;Menu(app,232);
    Check(picks==count+1 && unchanged(),"first empty-picture dialog cancellation does not establish an activation");
    picture_reset_checks::TempBitmap missing,broken,image;
    missing.Remove();
    { std::ofstream stream(std::filesystem::path(broken.path),std::ios::binary|std::ios::trunc);stream<<"bad image";stream.close();Check(bool(stream),"write malformed initial picture fixture"); }
    for(const auto* path:{missing.path.c_str(),broken.path.c_str()}) {
        bool changed=true;
        Check(ImportBoundPicture(*app,path,changed)!=MMS_OK && !changed && unchanged(),
              "failed first import leaves the empty slot unactivated and history untouched");
    }
    NativePicturePlan stale;
    Check(stale.Begin(*app)==MMS_OK && unchanged(),"planning an empty picture is read-only");
    identity::Invalidate(app,identity::Kind::Picture);
    Check(stale.Clear()==MMS_E_STALE && unchanged(),"empty-slot invalidation rejects an older plan even when both tickets are zero");
    Import(app,image.path.c_str());
    const auto asset=Current(app);auto* texture=app->PictureBackgroundTexture();
    const auto scene=app->SceneGeneration();
    ResetAppState(app);
    Check(app->SceneGeneration()>scene && app->PictureBackgroundTexture()==texture &&
          !identity::CurrentPictureTicket(app,instance),"real project reset retains the picture without activating its new scene slot");
    const auto resetPicks=picks;picked=nullptr;Menu(app,232);
    Check(picks==resetPicks+1 && !identity::CurrentPictureTicket(app,instance) &&
          app->PictureBackgroundTexture()==texture,"cancelled import after real reset preserves the retained unactivated picture");
    bool changed=false;
    Check(SetBoundPictureVisibility(*app,true,changed)==MMS_OK && changed && Current(app)==asset &&
          app->PictureBackgroundTexture()==texture,"retained zero-ticket picture can be shown through common history without reacquiring it first");
    Presentation(app);
}
void Chronology(MMDApp* app, SharedHistory& history, SharedHistory::RecordingMode mode) {
    Empty(app,history,mode);
    picture_reset_checks::TempBitmap a,b;
    picked=a.path.c_str(); Menu(app,232); picked=nullptr;
    const auto first=Current(app);
    Check(first && history.UndoCount()==1 && first->Path()==a.path && app->SceneModified(), "real menu232 imports one explicit picture history entry");
    Presentation(app);
    app->PictureOffsetX()=23; app->PictureOffsetY()=-11; app->PictureScale()=.75f;
    Import(app,b.path.c_str()); const auto second=Current(app);
    Check(second && second!=first && second->Id()!=first->Id() && history.UndoCount()==2,
          "consecutive picture replacement publishes a distinct retained asset and one explicit entry");
    app->PictureOffsetX()=-7; app->PictureOffsetY()=19; app->PictureScale()=.625f;
    Menu(app,picture_menu::kRemoveCommand);
    Check(!Current(app) && !app->PictureBackgroundTexture() && history.UndoCount()==3,
          "real remove-picture command clears native resources in one explicit history node");
    Presentation(app); a.Remove(); b.Remove(); app->CurrentFrame()=37;
    for(unsigned cycle=0;cycle!=3;++cycle) {
        Replay(app,history,false);
        Check(Current(app)==second && app->PictureOffsetX()==-7 && app->PictureOffsetY()==19 && app->PictureScale()==.625f,
              "undo clear restores retained second picture with its last live placement after source removal");
        picture_reset_checks::VerifyPixels(app->Renderer()->device,Current(app)->Source());
        Replay(app,history,false);
        Check(Current(app)==first && app->PictureOffsetX()==23 && app->PictureOffsetY()==-11 && app->PictureScale()==.75f,
              "undo replacement restores first asset and its unregistered placement without a stale error");
        picture_reset_checks::VerifyPixels(app->Renderer()->device,Current(app)->Source());
        Replay(app,history,false);
        Check(!Current(app) && !app->PictureBackgroundTexture() && !history.UndoCount() && history.RedoCount()==3,
              "undo first import returns canonical empty scene with all chronological redo entries");
        Replay(app,history,true); Check(Current(app)==first,"redo first import restores its retained logical identity");
        Replay(app,history,true); Check(Current(app)==second,"redo replacement restores its retained logical identity");
        Replay(app,history,true); Check(!Current(app) && history.UndoCount()==3,"redo clear completes a repeatable three-entry history cycle");
    }
    Replay(app,history,false); // Leave second picture, with clear available for redo.
    const auto undo=history.UndoCount(), redo=history.RedoCount();
    auto* texture=app->PictureBackgroundTexture();
    Menu(app,233);
    Check(Current(app)==second && app->PictureBackgroundTexture()==texture && !app->PictureBackgroundEnabled(),
          "real menu233 visibility edit reuses the actual texture and same logical asset");
    Presentation(app);
    if(mode==SharedHistory::RecordingMode::ExplicitOnly) {
        Check(history.UndoCount()==undo && history.RedoCount()==redo,"explicit-only visibility adjustment preserves an existing redo branch");
        Replay(app,history,true); Check(!Current(app),"redo clear remains usable after unrecorded visibility change");
        Replay(app,history,false); Check(Current(app)==second && !app->PictureBackgroundEnabled(),
              "undo clear remembers the hidden live presentation left by an unrecorded adjustment");
    } else {
        Check(history.UndoCount()==undo+1 && !history.RedoCount(),"all-operations visibility appends one node and replaces the redo branch");
        app->PictureOffsetX()=41; app->PictureOffsetY()=-5; app->PictureScale()=.375f;
        Replay(app,history,false);
        Check(app->PictureBackgroundEnabled() && app->PictureBackgroundTexture()==texture && app->PictureOffsetX()==41 &&
              app->PictureOffsetY()==-5 && app->PictureScale()==.375f,"visibility undo owns only enabled state and preserves subsequently adjusted placement");
        Replay(app,history,true);
        Check(!app->PictureBackgroundEnabled() && app->PictureBackgroundTexture()==texture && app->PictureOffsetX()==41,
              "visibility redo retains current geometry and the same texture without reconstructing the picture");
    }
}
void Failures(MMDApp* app, SharedHistory& history, SharedHistory::RecordingMode mode) {
    Empty(app,history,mode); picture_reset_checks::TempBitmap a,b,broken,missing;
    { std::ofstream output(std::filesystem::path(broken.path),std::ios::binary|std::ios::trunc);output<<"bad image";output.close();Check(bool(output),"write owned malformed image"); }
    missing.Remove(); Import(app,a.path.c_str()); Import(app,b.path.c_str()); Replay(app,history,false);
    for(const auto* path:{broken.path.c_str(),missing.path.c_str()}) {
        const Snapshot before(app,history);bool changed=true;
        Check(ImportBoundPicture(*app,path,changed)!=MMS_OK && !changed && before.Matches(app,history),
              "bad or missing image keeps asset texture identity history cursor and redo branch unchanged");
    }
    { const Snapshot before(app,history);const auto count=picks;picked=nullptr;Menu(app,232);
      Check(picks==count+1 && before.Matches(app,history),"real menu232 cancellation preserves picture and redo history"); }
    bool changed=true;const Snapshot same(app,history);
    Check(SetBoundPictureVisibility(*app,bool(app->PictureBackgroundEnabled()),changed)==MMS_OK && !changed && same.Matches(app,history),
          "same visibility is a true no-op including dirty flag and redo branch");
    // Reset only the obsolete redo branch so a retained first-import record fits
    // below the old-plus-candidate decode peak; Configure itself must not evict it.
    Check(history.Reset()==MMS_OK,"clear history before precise import-budget admission check");
    Check(ClearBoundPicture(*app,changed)==MMS_OK && history.Reset()==MMS_OK,"establish empty baseline for history-budget fixture");
    Import(app,a.path.c_str());
    const auto retained=history.RetainedBytes();
    Check(retained>0 && retained+4096<80ull*1024*1024 &&
          history.Configure(mode,128,retained+4096)==MMS_OK && history.UndoCount()==1,
          "retain the existing node while limiting below two full texture preparations");
    const Snapshot limited(app,history);changed=true;
    Check(ImportBoundPicture(*app,b.path.c_str(),changed)==MMS_E_MEMORY && !changed && limited.Matches(app,history),
          "insufficient preparation budget preserves existing picture and its complete history node");
    Check(history.Configure(mode,128,largeBudget)==MMS_OK,"restore preparation budget after expected admission failure");
    // Configuration may intentionally evict nodes. Take the chronology baseline
    // afterwards; a presentation-only edit must not need another GPU allocation.
    Check(history.Configure(SharedHistory::RecordingMode::ExplicitOnly,128,16ull*1024*1024)==MMS_OK,
          "lower journal budget below the currently displayed texture's conservative payload");
    const auto undo=history.UndoCount(),redo=history.RedoCount();
    const auto asset=Current(app);auto* texture=app->PictureBackgroundTexture();
    const auto x=app->PictureOffsetX(),y=app->PictureOffsetY();const auto scale=app->PictureScale();
    for(bool visible:{false,true}) {
        changed=false;
        Check(SetBoundPictureVisibility(*app,visible,changed)==MMS_OK && changed && Current(app)==asset &&
              app->PictureBackgroundTexture()==texture && app->PictureBackgroundEnabled()==unsigned(visible) &&
              app->PictureOffsetX()==x && app->PictureOffsetY()==y && app->PictureScale()==scale &&
              history.UndoCount()==undo && history.RedoCount()==redo,
              "explicit-only visibility reuses existing texture below the journal resource limit without changing chronology");
        Presentation(app);
    }
    Check(history.Configure(SharedHistory::RecordingMode::AllOperations,128,16ull*1024*1024)==MMS_OK,
          "apply the same small bound when visibility would create a retained record");
    const Snapshot retainedVisibility(app,history);changed=true;
    Check(SetBoundPictureVisibility(*app,false,changed)==MMS_E_MEMORY && !changed && retainedVisibility.Matches(app,history),
          "all-operations visibility still obeys retained-record admission without partial state changes");
    Check(history.Configure(mode,128,largeBudget)==MMS_OK,"restore mode and limits after presentation-only admission checks");
}
void GridInterleave(MMDApp* app, SharedHistory& history) {
    Empty(app,history,SharedHistory::RecordingMode::AllOperations); picture_reset_checks::TempBitmap a;
    Import(app,a.path.c_str()); const auto asset=Current(app); const auto oldGrid=app->GroundGridEnabled();
    Menu(app,215);
    Check(app->GroundGridEnabled()!=oldGrid && history.UndoCount()==2,"real grid menu follows picture import in the common chronology");
    Menu(app,picture_menu::kRemoveCommand);a.Remove();
    Check(!Current(app) && history.UndoCount()==3,"picture clear follows the unrelated native grid record");
    Replay(app,history,false); Check(Current(app)==asset,"mixed undo first restores the picture");
    Check(history.History(false)==MMS_OK && app->GroundGridEnabled()==oldGrid && Current(app)==asset,
          "mixed undo next restores grid without changing the picture asset");
    Replay(app,history,false);Check(!Current(app),"mixed undo last returns before picture import");
    Replay(app,history,true);Check(Current(app)==asset,"mixed redo restores picture before grid");
    Check(history.History(true)==MMS_OK && app->GroundGridEnabled()!=oldGrid && Current(app)==asset,"mixed redo applies the distinct native grid entry");
    Replay(app,history,true);Check(!Current(app),"mixed redo finally applies picture clear");
}
void DeferredRefresh(MMDApp* app, SharedHistory& history) {
    Empty(app,history,SharedHistory::RecordingMode::AllOperations);
    picture_reset_checks::TempBitmap image;
    struct TemporarilyMissingVertices {
        MMDApp* app;
        IDirect3DVertexBuffer9* saved;
        explicit TemporarilyMissingVertices(MMDApp* value) : app(value),saved(value->PictureOverlayVertices()) {
            Check(saved!=nullptr,"deferred-refresh fault starts with an actual overlay vertex buffer");
            app->PictureOverlayVertices()=nullptr;
        }
        void Restore() { if(saved){app->PictureOverlayVertices()=saved;saved=nullptr;} }
        ~TemporarilyMissingVertices(){Restore();}
    } missing(app);
    bool changed=false;
    Check(ImportBoundPicture(*app,image.path.c_str(),changed)==MMS_OK && changed && Current(app) &&
          app->PictureBackgroundTexture() && history.UndoCount()==1 && !history.RedoCount(),
          "real import commits successfully exactly once even when its geometry refresh cannot run");
    const auto pending=app->PendingPictureUiTicket();
    Check(pending && !TryPicBgOverlayRefresh(app) && !RefreshBoundPictureUi(*app) && app->PendingPictureUiTicket()==pending,
          "missing native vertex buffer keeps the actual failed geometry refresh pending");
    {
        identity::Mutation mutation(app,true,false,false,true);
        Check(!RefreshBoundPictureUi(*app) && app->PendingPictureUiTicket()==pending,
              "temporary picture mutation must not discard a pending refresh for its preserved ticket");
        missing.Restore();
        Check(!RefreshBoundPictureUi(*app) && app->PendingPictureUiTicket()==pending,
              "restoring vertices alone cannot refresh through an active picture mutation");
    }
    app->PictureOffsetX()=31;app->PictureOffsetY()=-17;app->PictureScale()=.875f;
    Check(RefreshBoundPictureUi(*app) && !app->PendingPictureUiTicket() && history.UndoCount()==1 && !history.RedoCount(),
          "safe retry clears the pending ticket without republishing the import or appending another node");
    Presentation(app);
    const Snapshot complete(app,history);
    Check(RefreshBoundPictureUi(*app) && complete.Matches(app,history),"completed refresh is idempotent for scene data and chronology");
}
unsigned retiredTextures=0;
bool allRetirementsLocked=true;
constexpr GUID retirementGuid={0xa284a091,0x4a15,0x4939,{0x82,0x1a,0x74,0x53,0x10,0x73,0x21,0x63}};
class RetirementMarker final : public IUnknown {
    ULONG refs_=1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,__uuidof(IUnknown)))return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining=--refs_;
        if(!remaining){++retiredTextures;allRetirementsLocked=allRetirementsLocked&&bake::UiLocked();delete this;}
        return remaining;
    }
};
void MarkRetirement(MMDApp* app) {
    Check(app->PictureBackgroundTexture() && SUCCEEDED(app->Renderer()->device->SetTexture(0,nullptr)),
          "remove prior drawing's reference before observing actual picture texture retirement");
    auto* marker=new RetirementMarker;
    const auto status=app->PictureBackgroundTexture()->SetPrivateData(retirementGuid,marker,sizeof(IUnknown*),D3DSPD_IUNKNOWN);
    marker->Release();
    Check(SUCCEEDED(status),"attach real texture lifetime observer without replacing any COM texture method");
}
void RetirementGate(MMDApp* app, SharedHistory& history) {
    Empty(app,history,SharedHistory::RecordingMode::AllOperations);
    picture_reset_checks::TempBitmap a,b;
    Import(app,a.path.c_str());MarkRetirement(app);
    const auto initial=retiredTextures;allRetirementsLocked=true;
    Check(!bake::UiLocked(),"retirement observer starts outside the publication input gate");
    Import(app,b.path.c_str());
    Check(retiredTextures==initial+1 && allRetirementsLocked && !bake::UiLocked(),
          "replacement releases its actual retired texture under the UI input gate and leaves that gate afterwards");
    MarkRetirement(app);
    bool changed=false;
    Check(ClearBoundPicture(*app,changed)==MMS_OK && changed && retiredTextures==initial+2 &&
          allRetirementsLocked && !bake::UiLocked(),
          "picture removal also releases its actual retired texture under the input gate");
    Presentation(app);
    // This observer measures destruction-time gate state only. It does not
    // post WM_CLOSE or claim to exercise a dynamically closing host window.
}
void UnboundMenuRetry(MMDApp* app, SharedHistory& history) {
    Empty(app,history,SharedHistory::RecordingMode::AllOperations);
    picture_reset_checks::TempBitmap image;Import(app,image.path.c_str());
    Shutdown();
    Check(!BoundNativePoseHistory(app),"unbound removal fixture has no common-history refresh queue");
    const auto menu=GetMenu(app->MainWindow());
    Check(menu && SetMenu(app->MainWindow(),nullptr),"temporarily detach the real menu to fail its first refresh");
    ClearLegacyPicture(app);
    Check(!app->PictureBackgroundTexture() && app->PendingPictureUiTicket(),
          "unbound removal retains failed menu refresh without restoring the removed picture");
    Check(SetMenu(app->MainWindow(),menu),"restore the actual menu before retrying at its normal opening boundary");
    SendMessageW(app->MainWindow(),WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(GetSubMenu(menu,0)),0);
    Check(!app->PendingPictureUiTicket() && !(GetMenuState(menu,233,MF_BYCOMMAND)&MF_CHECKED) &&
          (GetMenuState(menu,picture_menu::kRemoveCommand,MF_BYCOMMAND)&(MF_GRAYED|MF_DISABLED)),
          "unbound menu opening retries show/remove state and clears its pending picture refresh");
}
void Run(MMDApp* app) {
    Check(app && app->Renderer() && app->Renderer()->device && !IsWindowVisible(app->MainWindow()),
          "picture history uses the complete hidden production D3D host");
    Shutdown();Check(InitializeHistoryTestRuntime(app),"bind actual common history runtime for native picture editing");
    auto* history=NativeHistory(app);Check(history!=nullptr,"obtain the actual host journal");
    ReadableGeometry geometry(app);Picker picker;
    UnactivatedPicture(app,*history);
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Chronology(app,*history,mode);Failures(app,*history,mode);
    }
    GridInterleave(app,*history);
    DeferredRefresh(app,*history);
    RetirementGate(app,*history);
    Empty(app,*history,SharedHistory::RecordingMode::ExplicitOnly);
    UnboundMenuRetry(app,*history);
    Check(!IsWindowVisible(app->MainWindow()),"picture history checks never show the user's host window");
    std::puts("PASS native picture common history: both modes retained replay visibility failure and grid chronology");
}
}
