// Kept in the real-MME host so every capture uses the production scene path.
#include "antialias_temporal_checks.hpp"
#include "antialias_motion_probe.hpp"
#include "antialias_stability_checks.hpp"
#include <array>
#include <iomanip>
#include <limits>
// Paired GPU/CPU history-field captures must submit identical real inputs.
// FrameDriver advances playback and physics after the scene render, so the six
// parity captures below drive the production scene render and the real device
// Present directly; these helpers freeze and report the submitted inputs.
extern "C" float ExpGetFrameTime();
struct AntialiasCameraState {
    std::uint8_t mode=0;
    float positionX=0,positionY=0,positionZ=0,distance=0;
    float pitch=0,yaw=0,roll=0,fov=0;
    float viewOffsetX=0,viewOffsetY=0;
    std::uint8_t perspective=0;
    std::int32_t parentModel=0,parentBone=0;
    int referenceMode=0;
    std::uint8_t attachmentTransformSuppressed=0;
    D3DMATRIX attachmentBasis{};
};
AntialiasCameraState CaptureAntialiasCamera(MMDApp* app) {
    AntialiasCameraState camera;
    camera.mode=app->CameraMode();
    camera.positionX=app->CameraPositionX();camera.positionY=app->CameraPositionY();camera.positionZ=app->CameraPositionZ();
    camera.distance=app->CameraDistance();
    camera.pitch=app->CameraPitch();camera.yaw=app->CameraYaw();camera.roll=app->CameraRoll();
    camera.fov=app->CameraFov();
    camera.viewOffsetX=app->ViewOffsetX();camera.viewOffsetY=app->ViewOffsetY();
    camera.perspective=app->CameraPerspective();
    camera.parentModel=app->CameraParentModel();camera.parentBone=app->CameraParentBone();
    camera.referenceMode=static_cast<int>(app->CameraReferenceMode());
    camera.attachmentTransformSuppressed=app->CameraAttachmentTransformSuppressed();
    camera.attachmentBasis=app->CameraAttachmentBasis();
    return camera;
}
void ApplyAntialiasCamera(MMDApp* app,const AntialiasCameraState& camera) {
    app->CameraMode()=camera.mode;
    app->CameraPositionX()=camera.positionX;app->CameraPositionY()=camera.positionY;app->CameraPositionZ()=camera.positionZ;
    app->CameraDistance()=camera.distance;
    app->CameraPitch()=camera.pitch;app->CameraYaw()=camera.yaw;app->CameraRoll()=camera.roll;
    app->CameraFov()=camera.fov;
    app->ViewOffsetX()=camera.viewOffsetX;app->ViewOffsetY()=camera.viewOffsetY;
    app->CameraPerspective()=camera.perspective;
    app->CameraParentModel()=camera.parentModel;app->CameraParentBone()=camera.parentBone;
    app->CameraReferenceMode()=static_cast<CameraAttachmentReference>(camera.referenceMode);
    app->CameraAttachmentTransformSuppressed()=camera.attachmentTransformSuppressed;
    app->CameraAttachmentBasis()=camera.attachmentBasis;
}
bool SameAntialiasCamera(const AntialiasCameraState& first,const AntialiasCameraState& second) {
    return first.mode==second.mode&&
        first.positionX==second.positionX&&first.positionY==second.positionY&&first.positionZ==second.positionZ&&
        first.distance==second.distance&&first.pitch==second.pitch&&first.yaw==second.yaw&&first.roll==second.roll&&
        first.fov==second.fov&&first.viewOffsetX==second.viewOffsetX&&first.viewOffsetY==second.viewOffsetY&&
        first.perspective==second.perspective&&first.parentModel==second.parentModel&&first.parentBone==second.parentBone&&
        first.referenceMode==second.referenceMode&&first.attachmentTransformSuppressed==second.attachmentTransformSuppressed&&
        std::memcmp(&first.attachmentBasis,&second.attachmentBasis,sizeof(D3DMATRIX))==0;
}
struct AntialiasBoneSnapshot {
    bool loaded=false;
    unsigned char* model=nullptr;
    unsigned boneCount=0;
    std::vector<float> matrices;
};
std::array<AntialiasBoneSnapshot,kModelSlotCount> SaveAntialiasBoneSnapshot(MMDApp* app) {
    std::array<AntialiasBoneSnapshot,kModelSlotCount> snapshot{};
    for(int slot=0;slot<kModelSlotCount;++slot) {
        auto* model=app->ModelSlot(slot);if(!model)continue;
        const auto* record=mdl::Mdl(model);auto* bones=mdl::Bones(model);
        auto& entry=snapshot[slot];entry.loaded=true;entry.model=model;entry.boneCount=record->boneCount;
        if(!bones)continue;
        entry.matrices.resize(std::size_t(entry.boneCount)*16);
        for(unsigned bone=0;bone<entry.boneCount;++bone)
            std::memcpy(entry.matrices.data()+std::size_t(bone)*16,bones[bone].matInit,16*sizeof(float));
    }
    return snapshot;
}
void RestoreAntialiasBoneSnapshot(MMDApp* app,const std::array<AntialiasBoneSnapshot,kModelSlotCount>& snapshot) {
    for(int slot=0;slot<kModelSlotCount;++slot) {
        const auto& entry=snapshot[slot];if(!entry.loaded)continue;
        auto* model=app->ModelSlot(slot);if(model!=entry.model)continue;
        auto* bones=mdl::Bones(model);
        if(!bones||entry.matrices.size()!=std::size_t(entry.boneCount)*16)continue;
        for(unsigned bone=0;bone<entry.boneCount;++bone)
            std::memcpy(bones[bone].matInit,entry.matrices.data()+std::size_t(bone)*16,16*sizeof(float));
    }
}
std::uint64_t AntialiasBoneMatricesHash(MMDApp* app,int slot,unsigned& boneCount,bool& loaded) {
    std::uint64_t hash=14695981039346656037ull;
    boneCount=0;loaded=false;
    auto* model=app->ModelSlot(slot);if(!model)return hash;
    loaded=true;
    const auto* record=mdl::Mdl(model);boneCount=record->boneCount;
    const auto mix=[&](std::uint32_t value) { hash^=value;hash*=1099511628211ull; };
    mix(static_cast<std::uint32_t>(slot));mix(boneCount);
    auto* bones=mdl::Bones(model);if(!bones)return hash;
    for(unsigned bone=0;bone<boneCount;++bone)for(unsigned element=0;element<16;++element) {
        std::uint32_t bits=0;std::memcpy(&bits,&bones[bone].matInit[element],sizeof bits);mix(bits);
    }
    return hash;
}
struct AntialiasParityInput {
    std::int32_t frame=0;
    double playbackCursorSeconds=0;
    float effectFrameTime=0;
    AntialiasCameraState camera;
    std::array<std::uint64_t,kModelSlotCount> boneHashes{};
    std::array<unsigned,kModelSlotCount> boneCounts{};
    std::array<bool,kModelSlotCount> modelLoaded{};
};
void CaptureAntialiasParityInput(MMDApp* app,AntialiasParityInput& input) {
    input.frame=app->CurrentFrame();
    input.playbackCursorSeconds=app->PlaybackCursorSeconds();
    input.effectFrameTime=ExpGetFrameTime();
    input.camera=CaptureAntialiasCamera(app);
    for(int slot=0;slot<kModelSlotCount;++slot)
        input.boneHashes[slot]=AntialiasBoneMatricesHash(app,slot,input.boneCounts[slot],input.modelLoaded[slot]);
}
std::uint64_t AntialiasAttachmentBasisHash(const D3DMATRIX& basis) {
    std::uint64_t hash=14695981039346656037ull;
    for(unsigned row=0;row<4;++row)for(unsigned column=0;column<4;++column) {
        std::uint32_t bits=0;std::memcpy(&bits,&basis.m[row][column],sizeof bits);
        hash^=bits;hash*=1099511628211ull;
    }
    return hash;
}
void PrintAntialiasParityInput(const char* stage,const char* round,const AntialiasParityInput& input) {
    std::printf("PAIR INPUT stage=%s round=%s frame=%d cursor=%.9f effect_time=%.9f "
        "pos=(%g,%g,%g) distance=%g rotation=(%g,%g,%g) fov=%g view=(%g,%g) perspective=%u "
        "camera_mode=%u parent=%d:%d reference=%d suppress=%u attachment=%016llx",
        stage,round,input.frame,input.playbackCursorSeconds,input.effectFrameTime,
        input.camera.positionX,input.camera.positionY,input.camera.positionZ,input.camera.distance,
        input.camera.pitch,input.camera.yaw,input.camera.roll,input.camera.fov,
        input.camera.viewOffsetX,input.camera.viewOffsetY,unsigned(input.camera.perspective),
        unsigned(input.camera.mode),input.camera.parentModel,input.camera.parentBone,input.camera.referenceMode,
        unsigned(input.camera.attachmentTransformSuppressed),
        static_cast<unsigned long long>(AntialiasAttachmentBasisHash(input.camera.attachmentBasis)));
    for(int slot=0;slot<kModelSlotCount;++slot)if(input.modelLoaded[slot])
        std::printf(" bones[%d]=%u:%016llx",slot,input.boneCounts[slot],
            static_cast<unsigned long long>(input.boneHashes[slot]));
    std::putchar('\n');
}
bool SameAntialiasBones(const AntialiasParityInput& first,const AntialiasParityInput& second) {
    for(int slot=0;slot<kModelSlotCount;++slot)
        if(first.modelLoaded[slot]!=second.modelLoaded[slot]||first.boneCounts[slot]!=second.boneCounts[slot]||
            first.boneHashes[slot]!=second.boneHashes[slot])return false;
    return true;
}
using AntialiasCheckFormat=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,DWORD,D3DRESOURCETYPE,D3DFORMAT);
AntialiasCheckFormat originalAntialiasCheckFormat=nullptr;
unsigned rejectedAntialiasFloatQueries=0;
bool rejectAntialiasVertexTexture=false;
bool RequireOriginalCoverageForTest(){return false;}
HRESULT WINAPI RejectAntialiasFloatTarget(IDirect3D9* d3d,UINT adapter,D3DDEVTYPE type,D3DFORMAT adapterFormat,
    DWORD usage,D3DRESOURCETYPE resource,D3DFORMAT format) {
    if(format==D3DFMT_A32B32G32R32F &&
       (usage&(rejectAntialiasVertexTexture?D3DUSAGE_QUERY_VERTEXTEXTURE:D3DUSAGE_RENDERTARGET))) {
        ++rejectedAntialiasFloatQueries;return D3DERR_NOTAVAILABLE;
    }
    return originalAntialiasCheckFormat(d3d,adapter,type,adapterFormat,usage,resource,format);
}
void CheckAntialiasMenus(MMDApp* app) {
    Check(app->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
        "legacy outline preference uses direct softening on restart");
    const auto select=[&](antialias::Algorithm mode) {
        SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kFirstCommand+unsigned(mode),0);
        Check(app->antialiasing.algorithm==mode,"menu selects actual algorithm");
    };
    select(antialias::Algorithm::MmdOriginal);
    select(antialias::Algorithm::Fxaa);
    for(int language=0;language<3;++language) {
        app->EnglishUI()=language;RefreshMenuLanguage(app);
        SendMessageW(app->MainWindow(),WM_ENTERMENULOOP,0,0);
        HMENU bar=GetMenu(app->MainWindow()),parent=nullptr,popup=nullptr;int index=-1;
        for(int i=0;i<GetMenuItemCount(bar);++i) {
            auto child=GetSubMenu(bar,i);
            for(int j=0;j<GetMenuItemCount(child);++j) {
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_STATE;
                GetMenuItemInfoW(child,j,TRUE,&item);
                if(item.wID==277){parent=child;popup=item.hSubMenu;index=j;}
            }
        }
        Check(popup && GetMenuItemCount(popup)==5,
            "AA flyout contains only Off and algorithm choices");
        wchar_t offLabel[32]{};GetMenuStringW(popup,antialias::kOffCommand,offLabel,32,MF_BYCOMMAND);
        Check(std::wcscmp(offLabel,language==2?L"关闭":language==1?L"Off":L"オフ")==0,"Off is localized in every UI language");
        const auto commandPosition=[&](unsigned command) {
            for(int i=0;i<GetMenuItemCount(popup);++i) {
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID;
                if(GetMenuItemInfoW(popup,i,TRUE,&item) && item.wID==command)return i;
            }
            return -1;
        };
        const int smaaPosition=commandPosition(antialias::kSmaaCommand);
        auto smaaPopup=GetSubMenu(popup,smaaPosition);
        Check(smaaPopup && GetMenuItemCount(smaaPopup)==4,"four SMAA variants are available in the right-hand submenu");
        const auto checkOutlineSelection=[&]() {
            for(const auto retired:{antialias::kOutlineCommand,antialias::kOutlineFxaaCommand,antialias::kOutlineMsaaCommand})
                Check(GetMenuState(popup,retired,MF_BYCOMMAND)==UINT(-1),"independent outline commands are absent in every language");
            Check(app->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
                "outline softening remains automatic when switching or toggling AA");
        };
        const auto checkSmaaSelection=[&](antialias::Algorithm active) {
            // Selecting a variant refreshes the native submenu, so do not keep
            // the old handle between command dispatches.
            const auto child=GetSubMenu(popup,smaaPosition);
            const bool enabled=app->antialiasing.enabled;
            Check(((GetMenuState(parent,277,MF_BYCOMMAND)&MF_CHECKED)!=0)==enabled,"main item mirrors enabled state");
            Check(((GetMenuState(popup,antialias::kOffCommand,MF_BYCOMMAND)&MF_CHECKED)!=0)==!enabled,
                "Off is marked exactly when the main item is disabled");
            const bool smaaActive=enabled && active>=antialias::Algorithm::Smaa && active<antialias::Algorithm::Count;
            Check(((GetMenuState(popup,antialias::kSmaaCommand,MF_BYCOMMAND)&MF_CHECKED)!=0)==smaaActive,
                "SMAA parent is marked only when a SMAA variant is selected");
            Check(GetMenuDefaultItem(child,FALSE,0)==UINT(-1),"SMAA submenu has no default choice");
            for(const auto variant:{antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                                   antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
                Check(((GetMenuState(child,antialias::kFirstCommand+unsigned(variant),MF_BYCOMMAND)&MF_CHECKED)!=0)==(enabled && active==variant),
                    "SMAA child marks the current algorithm instead of the remembered variant");
            }
            for(const auto other:{antialias::Algorithm::MmdOriginal,antialias::Algorithm::Fxaa,antialias::Algorithm::Taa}) {
                Check(((GetMenuState(popup,antialias::kFirstCommand+unsigned(other),MF_BYCOMMAND)&MF_CHECKED)!=0)==(enabled && active==other),
                    "the algorithm submenu marks only the selected family");
            }
            checkOutlineSelection();
        };
        checkSmaaSelection(antialias::Algorithm::Fxaa);
        for(const auto retired:{antialias::kOutlineCommand,antialias::kOutlineFxaaCommand,antialias::kOutlineMsaaCommand}) {
            SendMessageW(app->MainWindow(),WM_COMMAND,retired,0);
            Check(app->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive &&
                app->antialiasing.enabled && app->antialiasing.algorithm==antialias::Algorithm::Fxaa,
                "retired outline commands cannot change activation or the automatic strategy");
        }
        Check((GetMenuState(popup,antialias::kFirstCommand+unsigned(antialias::Algorithm::Fxaa),MF_BYCOMMAND)&MF_CHECKED)!=0,
            "selected algorithm has radio check");
        SendMessageW(app->MainWindow(),WM_MENUSELECT,MAKEWPARAM(smaaPosition,MF_POPUP),reinterpret_cast<LPARAM>(popup));
        MSG smaaEnter{};smaaEnter.message=WM_KEYDOWN;smaaEnter.wParam=VK_RETURN;
        Check(antialias::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&smaaEnter))==0,
            "Enter on the SMAA label is left to native submenu navigation");
        MSG smaaPending{};
        Check(!PeekMessageW(&smaaPending,app->MainWindow(),WM_COMMAND,WM_COMMAND,PM_REMOVE),
            "opening the SMAA submenu does not post a selection command");
        Check(app->antialiasing.algorithm==antialias::Algorithm::Fxaa,
            "opening the SMAA submenu preserves the current algorithm");
        for(const auto active:{antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                               antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced,
                               antialias::Algorithm::Taa,antialias::Algorithm::MmdOriginal,antialias::Algorithm::Fxaa}) {
            select(active);checkSmaaSelection(active);
            Check(app->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
                "changing the AA algorithm retains automatic outline processing");
            CommandDispatch(nullptr,MAKEWPARAM(277,0));
            Check(!app->antialiasing.enabled && app->antialiasing.algorithm==active,"main Off remembers the selected algorithm");
            checkSmaaSelection(active);
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineFxaaCommand,0);
            Check(!app->antialiasing.enabled && app->antialiasing.algorithm==active &&
                app->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
                "a disabled outline command cannot enable AA or change its remembered strategy");
            CommandDispatch(nullptr,MAKEWPARAM(277,0));
            Check(app->antialiasing.enabled && app->antialiasing.algorithm==active,"main On restores the selected algorithm");
            checkSmaaSelection(active);
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOffCommand,0);
            Check(!app->antialiasing.enabled && app->antialiasing.algorithm==active,"submenu Off disables without forgetting the algorithm");
            checkSmaaSelection(active);
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOffCommand,0);
            Check(!app->antialiasing.enabled,"selecting Off twice remains disabled");
            select(active);
            Check(app->antialiasing.enabled,"selecting an algorithm turns AA back on");
            checkSmaaSelection(active);
        }
        for(const auto active:{antialias::Algorithm::MmdOriginal,antialias::Algorithm::Msaa2,
                               antialias::Algorithm::Msaa4,antialias::Algorithm::Msaa8}) {
            select(active);checkOutlineSelection();
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineFxaaCommand,0);
            Check(app->antialiasing.enabled && app->antialiasing.algorithm==active &&
                app->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
                "original and explicit MSAA ignore unavailable outline strategy commands");
        }
        select(antialias::Algorithm::Fxaa);
        SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineFxaaCommand,0);
        checkOutlineSelection();
        SendMessageW(app->MainWindow(),WM_MENUSELECT,MAKEWPARAM(index,MF_POPUP),reinterpret_cast<LPARAM>(parent));
        MSG enter{};enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
        Check(antialias::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&enter))==1,"Enter on parent keeps toggle action");
        MSG pending{};
        Check(PeekMessageW(&pending,app->MainWindow(),WM_COMMAND,WM_COMMAND,PM_REMOVE) && LOWORD(pending.wParam)==277,
            "split menu posts original AA toggle command");
        SendMessageW(app->MainWindow(),WM_EXITMENULOOP,0,0);
        Check(GetSubMenu(parent,index)==nullptr,"flyout detached on exit without changing original item");
        DispatchMessageW(&pending);Check(!app->antialiasing.enabled,"keyboard activates toggle");
        CommandDispatch(nullptr,MAKEWPARAM(277,0));
    }
    std::puts("PASS AA menus with automatic outlines in Japanese, English and Chinese");
}
// Default-off per-draw forensics for the paired GPU/CPU motion fields. Only an
// exact MMS_AA_DRAW_TRACE=1 registers this second native draw observer for the
// two existing single-frame parity renders. The production motion observer is
// already registered after the warm frames, so it runs first; the published
// draw counter confirms a motion write happened before the field is sampled.
// Nothing is drawn, presented or sampled beyond the four fixed pixels, and the
// source device state restored by the motion observer is only read.
struct AntialiasDrawTraceProbe final : deformed_mesh_capture::DrawObserver {
    enum class ValueStatus { Ok, None, Invalid };
    struct Pixel { unsigned x, y; };
    static constexpr unsigned kPixelCount = 4;
    static constexpr Pixel kPixels[kPixelCount] = {{804, 123}, {733, 136}, {500, 100}, {950, 170}};
    static constexpr std::size_t kMaxErrors = 8;
    struct Row {
        unsigned draw = 0;
        const char* kind = "unknown";
        int slot = -1;
        bool fallback = false;
        unsigned material = 0;
        const void* object = nullptr;
        unsigned pass = 0;
        unsigned primitive = 0;
        int base = 0;
        unsigned minVertex = 0, vertexCount = 0, firstIndex = 0, primitives = 0;
        ValueStatus vertexShader = ValueStatus::None;
        ValueStatus pixelShader = ValueStatus::None;
        ValueStatus constants = ValueStatus::None;
        std::uint64_t vertexShaderHash = 0, pixelShaderHash = 0, constantsHash = 0;
        bool stateValid = false;
        DWORD zEnable = 0, zWrite = 0, zFunc = 0, depthBias = 0, slopeBias = 0;
        const char* readback = "skipped";
        std::array<std::array<float, 4>, kPixelCount> pixels{};
        std::array<bool, kPixelCount> valid{};
        // MMS_AA_DRAW_INPUTS=1 only. These hold bytes already returned by the
        // existing GetFunction / GetVertexShaderConstantF reads for this row;
        // they stay empty in the default probe mode.
        bool captureInputs = false;
        std::vector<unsigned char> vertexShaderBytes;
        std::vector<unsigned char> vertexConstantBytes;
        unsigned vertexConstantRegisters = 0;
    };

    AntialiasDrawTraceProbe(MMDApp* app, const char* label) : app_(app), label_(label) {}
    ~AntialiasDrawTraceProbe() override { End(); }

    void Begin() noexcept {
        End();
        rows_.clear();
        errors_.clear();
        failed_ = false;
        expectedDraws_ = 0;
        // Read the inputs switch only here. Begin() is reached only through
        // AntialiasDrawTraceScope under an exact MMS_AA_DRAW_TRACE=1, so
        // MMS_AA_DRAW_INPUTS cannot enable the probe on its own.
        const char* const inputs = std::getenv("MMS_AA_DRAW_INPUTS");
        captureInputs_ = inputs != nullptr && std::strcmp(inputs, "1") == 0;
        if (!app_) { Fail("draw trace has no app"); return; }
        active_ = true;
        try {
            std::string error;
            if (deformed_mesh_capture::Observe(*app_, this, error)) registered_ = true;
            else Fail(error.c_str());
        } catch (...) { Fail("draw trace observer registration threw"); }
    }
    void End() noexcept {
        active_ = false;
        if (registered_) {
            deformed_mesh_capture::StopObserving(this);
            registered_ = false;
        }
        // Release the borrowed default-pool surface before later device resets;
        // the system surface and device reference live no longer than the frame.
        readbackSource_.Reset();
        readbackSystem_.Reset();
        readbackDevice_.Reset();
        cachedTexture_ = nullptr;
        cachedWidth_ = cachedHeight_ = 0;
    }
    // Runs after the traced RenderFrameScene returned; every failure is checked
    // here so the noexcept callback never throws or exits the process.
    void Finish() noexcept {
        End();
        bool passed = false;
        try {
            const unsigned draws = antialias::motion::GetStats(app_).draws;
            if (draws != expectedDraws_) {
                char text[192];
                std::snprintf(text, sizeof text,
                    "traced frame has %u motion draws but the observer recorded %u", draws, expectedDraws_);
                Fail(text);
            }
            if (rows_.size() != std::size_t(expectedDraws_))
                Fail("traced row count does not match observed motion draws");
            WriteTable();
            if (captureInputs_) {
                unsigned targets = 0;
                for (const auto& row : rows_) {
                    if (!row.captureInputs) continue;
                    ++targets;
                    if (row.vertexShader != ValueStatus::Ok || row.vertexShaderBytes.empty())
                        Fail("draw inputs target has no valid vertex shader bytes");
                    if (row.constants != ValueStatus::Ok || row.vertexConstantRegisters == 0 ||
                        row.vertexConstantBytes.empty())
                        Fail("draw inputs target has no valid vertex shader constants");
                }
                if (targets == 0)
                    Fail("draw inputs mode found no target slot 3/4 material 1 scene pass draws");
                WriteInputs();
            }
            for (const auto& message : errors_)
                std::fprintf(stderr, "AA DRAW TRACE ERROR: %s\n", message.c_str());
            std::printf("AA DRAW TRACE path=%s motion_draws=%u observed=%u rows=%llu errors=%llu\n",
                label_, draws, expectedDraws_,
                static_cast<unsigned long long>(rows_.size()),
                static_cast<unsigned long long>(errors_.size()));
            passed = errors_.empty() && !failed_;
        } catch (...) {
            Fail("draw trace finish threw");
        }
        Check(passed, "per-draw motion forensics capture is complete and error free");
    }

    void Indexed(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minVertex, UINT vertexCount,
                 UINT firstIndex, UINT primitives, const void* object, unsigned material) noexcept override {
        try {
            if (!active_ || !app_) return;
            const unsigned draws = antialias::motion::GetStats(app_).draws;
            // Only a callback that follows an actual motion write samples the
            // field; shadow maps, UI passes and skipped scene draws are ignored.
            if (draws == expectedDraws_) return;
            if (draws != expectedDraws_ + 1) {
                char text[192];
                std::snprintf(text, sizeof text,
                    "motion draw counter jumped from %u to %u between callbacks", expectedDraws_, draws);
                expectedDraws_ = draws;
                Fail(text);
                return;
            }
            expectedDraws_ = draws;
            Row row;
            row.draw = draws;
            row.material = material;
            row.object = object;
            row.pass = static_cast<unsigned>(app_->ActiveRenderPass());
            row.primitive = static_cast<unsigned>(type);
            row.base = base;
            row.minVertex = minVertex;
            row.vertexCount = vertexCount;
            row.firstIndex = firstIndex;
            row.primitives = primitives;
            Classify(device, type, object, row);
            if (captureInputs_ && IsInputTarget(row)) row.captureInputs = true;
            ReadState(device, row);
            ReadShaders(device, row);
            ReadPixels(row);
            rows_.push_back(std::move(row));
        } catch (const std::exception& e) {
            Fail(e.what());
        } catch (...) {
            Fail("draw trace callback threw");
        }
    }
    void Unsupported(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT) noexcept override {}

private:
    void Fail(const char* message) noexcept {
        failed_ = true;
        try { if (errors_.size() < kMaxErrors) errors_.push_back(message); } catch (...) {}
    }
    void FailHr(const char* operation, HRESULT result) noexcept {
        char text[192];
        const int written = std::snprintf(text, sizeof text, "%s failed hr=0x%08lx",
            operation, static_cast<unsigned long>(result));
        Fail(written > 0 ? text : operation);
    }
    static std::uint64_t HashBytes(const void* data, std::size_t size) {
        std::uint64_t hash = 14695981039346656037ull;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
        return hash;
    }
    template <class Shader>
    ValueStatus ReadShaderCode(Shader* shader, std::uint64_t& hash, const char* name,
                               std::vector<unsigned char>* retained = nullptr) noexcept {
        try {
            UINT bytes = 0;
            HRESULT result = shader->GetFunction(nullptr, &bytes);
            if (FAILED(result) || bytes == 0) {
                char text[160];
                std::snprintf(text, sizeof text, "%s shader length failed hr=0x%08lx bytes=%u",
                    name, static_cast<unsigned long>(result), unsigned(bytes));
                Fail(text);
                return ValueStatus::Invalid;
            }
            const UINT requested = bytes;
            std::vector<DWORD> code((std::size_t(bytes) + 3) / 4);
            result = shader->GetFunction(code.data(), &bytes);
            if (FAILED(result)) {
                char text[160];
                std::snprintf(text, sizeof text, "%s shader bytes failed hr=0x%08lx",
                    name, static_cast<unsigned long>(result));
                Fail(text);
                return ValueStatus::Invalid;
            }
            if (retained != nullptr) {
                // The export path must see the exact byte count of the second
                // GetFunction call, never the DWORD-aligned vector padding.
                if (bytes == 0 || bytes != requested) {
                    char text[160];
                    std::snprintf(text, sizeof text, "%s shader length changed from %u to %u",
                        name, unsigned(requested), unsigned(bytes));
                    Fail(text);
                    return ValueStatus::Invalid;
                }
                if (std::size_t(bytes) > code.size() * sizeof(DWORD)) {
                    char text[160];
                    std::snprintf(text, sizeof text, "%s shader bytes exceed the allocated buffer", name);
                    Fail(text);
                    return ValueStatus::Invalid;
                }
                const auto* first = reinterpret_cast<const unsigned char*>(code.data());
                retained->assign(first, first + bytes);
            }
            // The original hash and the optional retained bytes both come from
            // the same code.data() and the same final bytes count.
            hash = HashBytes(code.data(), bytes);
            return ValueStatus::Ok;
        } catch (...) {
            Fail("shader byte read threw");
            return ValueStatus::Invalid;
        }
    }
    static bool IsInputTarget(const Row& row) noexcept {
        // Identity comes from Classify for this same callback; no object
        // address is compared across processes and Classify is unchanged. The
        // target is the current scene pass (AccessoryRenderPass::Effect, pass 2
        // in the recorded evidence), model slot 3/4, material 1.
        return row.kind != nullptr && std::strcmp(row.kind, "model") == 0 &&
            (row.slot == 3 || row.slot == 4) && row.material == 1 &&
            row.pass == static_cast<unsigned>(AccessoryRenderPass::Effect);
    }
    bool CaptureConstants(Row& row) noexcept {
        try {
            static_assert(sizeof(float) == 4, "draw inputs write raw float32 constants");
            constexpr std::size_t kFloatsPerRegister = 4;
            const std::size_t registers = constantCapacity_;
            if (registers == 0) {
                Fail("draw inputs vertex constant register count is zero");
                return false;
            }
            if (registers > (std::numeric_limits<std::size_t>::max)() / (kFloatsPerRegister * sizeof(float))) {
                Fail("draw inputs vertex constant byte count overflows");
                return false;
            }
            const std::size_t floats = registers * kFloatsPerRegister;
            const std::size_t bytes = floats * sizeof(float);
            if (constantScratch_.size() != floats) {
                Fail("draw inputs vertex constant buffer length does not match the register count");
                return false;
            }
            // GetVertexShaderConstantF(0, ...) returned c0 onward in register
            // order. Copy the raw memory so no value is re-serialized.
            const auto* raw = reinterpret_cast<const unsigned char*>(constantScratch_.data());
            row.vertexConstantBytes.assign(raw, raw + bytes);
            row.vertexConstantRegisters = static_cast<unsigned>(registers);
            return true;
        } catch (...) {
            Fail("draw inputs vertex constant copy threw");
            return false;
        }
    }
    static const char* StatusName(ValueStatus status) noexcept {
        if (status == ValueStatus::Ok) return "ok";
        if (status == ValueStatus::None) return "none";
        return "invalid";
    }
    bool WriteExactFile(const std::filesystem::path& file, const std::vector<unsigned char>& data,
                        const char* what) noexcept {
        char text[200];
        const auto limit = static_cast<unsigned long long>((std::numeric_limits<std::streamsize>::max)());
        try {
            if (data.empty()) {
                std::snprintf(text, sizeof text, "draw inputs %s file has no data", what);
                Fail(text);
                return false;
            }
            if (static_cast<unsigned long long>(data.size()) > limit) {
                std::snprintf(text, sizeof text, "draw inputs %s file exceeds the streamsize limit", what);
                Fail(text);
                return false;
            }
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                std::snprintf(text, sizeof text, "draw inputs %s file open failed", what);
                Fail(text);
                return false;
            }
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out.good()) {
                std::snprintf(text, sizeof text, "draw inputs %s file write failed", what);
                Fail(text);
                return false;
            }
            out.flush();
            if (!out.good()) {
                std::snprintf(text, sizeof text, "draw inputs %s file flush failed", what);
                Fail(text);
                return false;
            }
            out.close();
            if (out.fail()) {
                std::snprintf(text, sizeof text, "draw inputs %s file close failed", what);
                Fail(text);
                return false;
            }
            return true;
        } catch (...) {
            Fail("draw inputs file write threw");
            return false;
        }
    }
    void WriteInputs() noexcept {
        try {
            const std::filesystem::path tableFile =
                captureDirectory / (std::string("motion-") + label_ + "-draw-inputs.tsv");
            std::ofstream table(tableFile, std::ios::trunc);
            if (!table.is_open()) { Fail("draw inputs table open failed"); return; }
            table << "# MMS_AA_DRAW_INPUTS=1 raw inputs retained by the existing MMS_AA_DRAW_TRACE=1 probe\n"
                     "# vs.bin is exactly the byte count returned by GetFunction, without DWORD padding\n"
                     "# vs-constants.f32 is c0 onward in register order, each register xyzw raw little-endian float32\n"
                     "draw\tkind\tslot\tmaterial\tpass\tvs_bytes\tvs_constants_registers\tvs_hash"
                     "\tvs_constants_hash\tvs_file\tvs_constants_file\tvs_status\tvs_constants_status\n";
            for (const auto& row : rows_) {
                if (!row.captureInputs) continue;
                const std::string stem =
                    std::string("motion-") + label_ + "-draw-" + std::to_string(row.draw);
                const std::string vsFile = stem + ".vs.bin";
                const std::string constantsFile = stem + ".vs-constants.f32";
                const std::string vsWhat = "vertex shader bytes " + vsFile;
                const std::string constantsWhat = "vertex constants " + constantsFile;
                WriteExactFile(captureDirectory / vsFile, row.vertexShaderBytes, vsWhat.c_str());
                WriteExactFile(captureDirectory / constantsFile, row.vertexConstantBytes, constantsWhat.c_str());
                table << row.draw << '\t' << row.kind << '\t' << row.slot << '\t' << row.material << '\t'
                      << row.pass << '\t' << row.vertexShaderBytes.size() << '\t'
                      << row.vertexConstantRegisters << '\t';
                WriteHash(table, row.vertexShader, row.vertexShaderHash);
                table << '\t';
                WriteHash(table, row.constants, row.constantsHash);
                table << '\t' << vsFile << '\t' << constantsFile << '\t'
                      << StatusName(row.vertexShader) << '\t' << StatusName(row.constants) << '\n';
                if (!table.good()) { Fail("draw inputs table write failed"); return; }
            }
            table.flush();
            if (!table.good()) { Fail("draw inputs table flush failed"); return; }
            table.close();
            if (table.fail()) Fail("draw inputs table close failed");
        } catch (...) {
            Fail("draw inputs table threw");
        }
    }
    void EnsureConstantCapacity(IDirect3DDevice9* device) noexcept {
        if (constantCapacity_ != 0 || constantCapacityFailed_) return;
        D3DCAPS9 caps{};
        const HRESULT result = device->GetDeviceCaps(&caps);
        if (FAILED(result)) { constantCapacityFailed_ = true; FailHr("GetDeviceCaps", result); return; }
        if (caps.MaxVertexShaderConst == 0) {
            constantCapacityFailed_ = true;
            Fail("device reports zero vertex shader constants");
            return;
        }
        const unsigned count = std::min<unsigned>(static_cast<unsigned>(caps.MaxVertexShaderConst), 4096u);
        try {
            constantScratch_.assign(std::size_t(count) * 4, 0.0f);
            constantCapacity_ = count;
        } catch (...) {
            constantCapacityFailed_ = true;
            Fail("vertex constant buffer allocation failed");
        }
    }
    void Classify(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, const void* object, Row& row) noexcept {
        try {
            if (type < D3DPT_TRIANGLELIST) {
                Microsoft::WRL::ComPtr<IDirect3DIndexBuffer9> indices;
                if (SUCCEEDED(device->GetIndices(&indices)) && indices.Get() != nullptr &&
                    indices.Get() == app_->GroundGridIndices()) {
                    row.kind = "grid";
                    row.slot = -1;
                    row.fallback = false;
                    row.material = UINT(-1);
                    return;
                }
            }
            for (int slot = 0; slot < kModelSlotCount; ++slot)
                if (object != nullptr && static_cast<const void*>(app_->ModelSlot(slot)) == object) {
                    row.kind = "model";
                    row.slot = slot;
                    return;
                }
            for (int slot = 0; slot < 255; ++slot)
                if (object != nullptr && static_cast<const void*>(app_->AccessorySlot(slot)) == object) {
                    row.kind = "accessory";
                    row.slot = slot;
                    return;
                }
            // Inverted-hull outlines have no MaterialScope object. The motion
            // observer attributes them through ActiveRenderObject, so mirror
            // that fallback and mark the row instead of inventing a slot.
            const void* active = app_->ActiveRenderObject();
            for (int slot = 0; slot < kModelSlotCount; ++slot)
                if (active != nullptr && static_cast<const void*>(app_->ModelSlot(slot)) == active) {
                    row.kind = "model";
                    row.slot = slot;
                    row.fallback = true;
                    if (app_->ActiveRenderPass() == AccessoryRenderPass::ModelOutline) row.material = UINT(-1);
                    return;
                }
            for (int slot = 0; slot < 255; ++slot)
                if (active != nullptr && static_cast<const void*>(app_->AccessorySlot(slot)) == active) {
                    row.kind = "accessory";
                    row.slot = slot;
                    row.fallback = true;
                    if (app_->ActiveRenderPass() == AccessoryRenderPass::ModelOutline) row.material = UINT(-1);
                    return;
                }
            row.kind = "unknown";
            row.slot = -1;
            row.fallback = true;
        } catch (...) {
            Fail("draw object classification threw");
        }
    }
    void ReadState(IDirect3DDevice9* device, Row& row) noexcept {
        DWORD zEnable = 0, zWrite = 0, zFunc = 0, depthBias = 0, slopeBias = 0;
        const HRESULT a = device->GetRenderState(D3DRS_ZENABLE, &zEnable);
        const HRESULT b = device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
        const HRESULT c = device->GetRenderState(D3DRS_ZFUNC, &zFunc);
        const HRESULT d = device->GetRenderState(D3DRS_DEPTHBIAS, &depthBias);
        const HRESULT e = device->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &slopeBias);
        row.zEnable = zEnable;
        row.zWrite = zWrite;
        row.zFunc = zFunc;
        row.depthBias = depthBias;
        row.slopeBias = slopeBias;
        row.stateValid = SUCCEEDED(a) && SUCCEEDED(b) && SUCCEEDED(c) && SUCCEEDED(d) && SUCCEEDED(e);
        if (FAILED(a)) FailHr("GetRenderState(ZENABLE)", a);
        if (FAILED(b)) FailHr("GetRenderState(ZWRITEENABLE)", b);
        if (FAILED(c)) FailHr("GetRenderState(ZFUNC)", c);
        if (FAILED(d)) FailHr("GetRenderState(DEPTHBIAS)", d);
        if (FAILED(e)) FailHr("GetRenderState(SLOPESCALEDEPTHBIAS)", e);
    }
    void ReadShaders(IDirect3DDevice9* device, Row& row) noexcept {
        try {
            Microsoft::WRL::ComPtr<IDirect3DVertexShader9> vertex;
            Microsoft::WRL::ComPtr<IDirect3DPixelShader9> pixel;
            const HRESULT vertexResult = device->GetVertexShader(&vertex);
            const HRESULT pixelResult = device->GetPixelShader(&pixel);
            if (FAILED(vertexResult)) { row.vertexShader = ValueStatus::Invalid; FailHr("GetVertexShader", vertexResult); }
            else if (!vertex) row.vertexShader = ValueStatus::None;
            else row.vertexShader = ReadShaderCode(vertex.Get(), row.vertexShaderHash, "vertex",
                row.captureInputs ? &row.vertexShaderBytes : nullptr);
            if (FAILED(pixelResult)) { row.pixelShader = ValueStatus::Invalid; FailHr("GetPixelShader", pixelResult); }
            else if (!pixel) row.pixelShader = ValueStatus::None;
            else row.pixelShader = ReadShaderCode(pixel.Get(), row.pixelShaderHash, "pixel");
            if (row.vertexShader == ValueStatus::Ok) {
                EnsureConstantCapacity(device);
                if (constantCapacity_ == 0) row.constants = ValueStatus::Invalid;
                else {
                    const HRESULT result = device->GetVertexShaderConstantF(0, constantScratch_.data(),
                        static_cast<UINT>(constantCapacity_));
                    if (FAILED(result)) {
                        row.constants = ValueStatus::Invalid;
                        FailHr("GetVertexShaderConstantF", result);
                    } else {
                        // The original hash and the optional export both use the
                        // raw buffer returned by this GetVertexShaderConstantF.
                        row.constantsHash = HashBytes(constantScratch_.data(), constantScratch_.size() * sizeof(float));
                        row.constants = ValueStatus::Ok;
                        if (row.captureInputs && !CaptureConstants(row)) row.constants = ValueStatus::Invalid;
                    }
                }
            } else row.constants = ValueStatus::None;
        } catch (...) {
            Fail("draw shader capture threw");
        }
    }
    void RebuildReadback(IDirect3DTexture9* texture, const D3DSURFACE_DESC& desc) noexcept {
        cachedTexture_ = texture;
        cachedWidth_ = desc.Width;
        cachedHeight_ = desc.Height;
        readbackSource_.Reset();
        readbackSystem_.Reset();
        readbackDevice_.Reset();
        try {
            Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
            HRESULT result = texture->GetDevice(&device);
            if (FAILED(result) || !device) { FailHr("motion texture GetDevice", result); return; }
            result = texture->GetSurfaceLevel(0, &readbackSource_);
            if (FAILED(result) || !readbackSource_) { FailHr("motion texture GetSurfaceLevel", result); return; }
            result = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                D3DPOOL_SYSTEMMEM, &readbackSystem_, nullptr);
            if (FAILED(result) || !readbackSystem_) { FailHr("motion readback CreateOffscreenPlainSurface", result); return; }
            readbackDevice_ = device;
        } catch (...) {
            Fail("motion readback surface setup threw");
        }
    }
    void ReadPixels(Row& row) noexcept {
        try {
            IDirect3DTexture9* texture = antialias::motion::GetTexture(app_);
            if (!texture) {
                row.readback = "no_field";
                Fail("motion field texture is unavailable during a traced draw");
                return;
            }
            D3DSURFACE_DESC desc{};
            HRESULT result = texture->GetLevelDesc(0, &desc);
            if (FAILED(result)) { row.readback = "desc_failed"; FailHr("motion field GetLevelDesc", result); return; }
            if (desc.Format != D3DFMT_A32B32G32R32F) {
                row.readback = "format";
                char text[160];
                std::snprintf(text, sizeof text, "motion field format=%d is not RGBA32F", int(desc.Format));
                Fail(text);
                return;
            }
            if (cachedTexture_ != texture || cachedWidth_ != desc.Width || cachedHeight_ != desc.Height)
                RebuildReadback(texture, desc);
            if (!readbackDevice_ || !readbackSource_ || !readbackSystem_) { row.readback = "surface_failed"; return; }
            result = readbackDevice_->GetRenderTargetData(readbackSource_.Get(), readbackSystem_.Get());
            if (FAILED(result)) { row.readback = "copy_failed"; FailHr("GetRenderTargetData", result); return; }
            D3DLOCKED_RECT locked{};
            result = readbackSystem_->LockRect(&locked, nullptr, D3DLOCK_READONLY);
            if (FAILED(result)) { row.readback = "lock_failed"; FailHr("LockRect", result); return; }
            if (!locked.pBits) { row.readback = "lock_failed"; Fail("LockRect returned no pixel pointer"); return; }
            struct UnlockRect {
                IDirect3DSurface9* surface;
                ~UnlockRect() { surface->UnlockRect(); }
            } unlock{readbackSystem_.Get()};
            bool allValid = true;
            for (unsigned i = 0; i < kPixelCount; ++i) {
                const Pixel pixel = kPixels[i];
                if (pixel.x >= desc.Width || pixel.y >= desc.Height || locked.Pitch <= 0 ||
                    static_cast<unsigned>(locked.Pitch) < (pixel.x + 1) * 16u) {
                    row.valid[i] = false;
                    allValid = false;
                    continue;
                }
                const auto* line = static_cast<const unsigned char*>(locked.pBits) +
                    std::size_t(pixel.y) * std::size_t(locked.Pitch);
                std::memcpy(row.pixels[i].data(), line + std::size_t(pixel.x) * 16, 16);
                row.valid[i] = true;
            }
            row.readback = allValid ? "ok" : "partial";
        } catch (...) {
            Fail("motion field readback threw");
        }
    }
    static void WriteHash(std::ostream& out, ValueStatus status, std::uint64_t value) {
        if (status == ValueStatus::Ok)
            out << std::hex << std::setw(16) << std::setfill('0') << value << std::dec << std::setfill(' ');
        else out << (status == ValueStatus::None ? "none" : "invalid");
    }
    static void WriteDword(std::ostream& out, DWORD value) {
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(value)
            << std::dec << std::setfill(' ');
    }
    void WriteTable() noexcept {
        try {
            const std::filesystem::path file = captureDirectory / (std::string("motion-") + label_ + "-draws.tsv");
            std::ofstream table(file, std::ios::trunc);
            if (!table.is_open()) { Fail("motion draw trace table open failed"); return; }
            table << "# MMS_AA_DRAW_TRACE=1 forensic capture from the existing single-frame paired renders\n"
                     "# draw is motion.stats.draws after the production observer wrote this draw\n"
                     "# probes: p0=(804,123) p1=(733,136) p2=(500,100) p3=(950,170)\n"
                     "# rgba32f components 0=velocityX 1=velocityY 2=currentDepth 3=previousDepth; invalid=outside field or readback failure\n"
                     "# fallback=1 means the slot came from ActiveRenderObject instead of the MaterialScope object\n"
                     "draw\tkind\tslot\tfallback\tmaterial\tobject\tpass\tprimitive\tbase\tmin_vertex\tvertex_count"
                     "\tfirst_index\tprimitives\tvs_hash\tps_hash\tvs_constants_hash\tzenable\tzwrite\tzfunc"
                     "\tdepth_bias\tslope_bias\tstate_valid\treadback";
            for (unsigned i = 0; i < kPixelCount; ++i)
                table << "\tp" << i << "_valid\tp" << i << "_0\tp" << i << "_1\tp" << i << "_2\tp" << i << "_3";
            table << '\n';
            table << std::setprecision(9);
            for (const auto& row : rows_) {
                table << row.draw << '\t' << row.kind << '\t' << row.slot << '\t' << int(row.fallback) << '\t'
                      << row.material << '\t' << row.object << '\t' << row.pass << '\t' << row.primitive << '\t'
                      << row.base << '\t' << row.minVertex << '\t' << row.vertexCount << '\t' << row.firstIndex
                      << '\t' << row.primitives << '\t';
                WriteHash(table, row.vertexShader, row.vertexShaderHash);
                table << '\t';
                WriteHash(table, row.pixelShader, row.pixelShaderHash);
                table << '\t';
                WriteHash(table, row.constants, row.constantsHash);
                table << '\t';
                table << static_cast<unsigned long>(row.zEnable) << '\t' << static_cast<unsigned long>(row.zWrite)
                      << '\t' << static_cast<unsigned long>(row.zFunc) << '\t';
                WriteDword(table, row.depthBias);
                table << '\t';
                WriteDword(table, row.slopeBias);
                table << '\t' << (row.stateValid ? 1 : 0) << '\t' << row.readback;
                for (unsigned i = 0; i < kPixelCount; ++i) {
                    table << '\t' << (row.valid[i] ? 1 : 0);
                    if (!row.valid[i]) { table << "\tinvalid\tinvalid\tinvalid\tinvalid"; continue; }
                    for (unsigned component = 0; component < 4; ++component) table << '\t' << row.pixels[i][component];
                }
                table << '\n';
                if (!table.good()) { Fail("motion draw trace table write failed"); return; }
            }
            table.flush();
            if (!table.good()) { Fail("motion draw trace table flush failed"); return; }
            table.close();
            if (table.fail()) Fail("motion draw trace table close failed");
        } catch (...) {
            Fail("motion draw trace table threw");
        }
    }

    MMDApp* app_ = nullptr;
    const char* label_ = "unknown";
    bool registered_ = false, active_ = false, failed_ = false;
    bool captureInputs_ = false;
    unsigned expectedDraws_ = 0;
    std::size_t constantCapacity_ = 0;
    bool constantCapacityFailed_ = false;
    std::vector<float> constantScratch_;
    IDirect3DTexture9* cachedTexture_ = nullptr;
    unsigned cachedWidth_ = 0, cachedHeight_ = 0;
    Microsoft::WRL::ComPtr<IDirect3DDevice9> readbackDevice_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> readbackSource_, readbackSystem_;
    std::vector<Row> rows_;
    std::vector<std::string> errors_;
};
struct AntialiasDrawTraceScope {
    AntialiasDrawTraceProbe& probe;
    explicit AntialiasDrawTraceScope(AntialiasDrawTraceProbe& value) : probe(value) { probe.Begin(); }
    ~AntialiasDrawTraceScope() { probe.End(); }
    AntialiasDrawTraceScope(const AntialiasDrawTraceScope&) = delete;
    AntialiasDrawTraceScope& operator=(const AntialiasDrawTraceScope&) = delete;
};
void CheckAntialiasScene(MMDApp* app, const wchar_t* directory) {
    if(std::getenv("MMS_AA_STABILITY")) { CheckAntialiasStability(app,directory);return; }
    Check(antialias::Settings{}.outlineMode==antialias::OutlineMode::FxaaAdaptive,
        "new AA settings default to FXAA outline softening without additional MSAA");
    CheckTemporalResolveShader();
    captureDirectory = directory;
    std::filesystem::create_directories(captureDirectory);
    // TEMPORARY: native outlines only draw in the camera/playback path.
    app->PlaybackActive()=1;
    for(int warm=0;warm<3;++warm) { app->DeltaTime()=1.f/30; app->state.messageSeen=10; FrameDriver(app); }

    auto* r = app->Renderer();
    // Freeze this saved pose so algorithm comparisons do not include hair
    // physics evolving while the test traverses the menu.
    app->PlaybackPhysicsMode()=0;
    std::printf("PMM camera=%g,%g,%g distance=%g rotation=%g,%g,%g render=%dx%d viewport=%ld,%ld,%ld,%ld\n",
        app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(), app->CameraDistance(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->RenderWidth(), app->RenderHeight(),
        app->ViewportRect().left, app->ViewportRect().top, app->ViewportRect().right, app->ViewportRect().bottom);
    auto** vtable = *reinterpret_cast<void***>(r->device);
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
        MH_EnableHook(vtable[17]) == MH_OK, "capture saved PMM view");
    const auto capture=[&](const char* name,int frames=4) {
        const auto until=GetTickCount64()+90000;
        const bool wasEnabled=app->antialiasing.enabled;
        while(antialias::Initializing(app) && GetTickCount64()<until) {
            app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);Sleep(1);
        }
        Check(!antialias::Initializing(app) && app->antialiasing.enabled==wasEnabled,
            "requested AA is ready before comparison capture");
        const auto frameStart=GetTickCount64();
        for (int frame=0;frame<frames;++frame) {
            captureRequested=frame==frames-1;
            app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);
        }
        const auto frameMs=GetTickCount64()-frameStart;
        Check(!captureRequested && !capturedPixels.empty(), "saved PMM pixels captured");
        std::ofstream file(captureDirectory/(std::string(name)+".bgra"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(capturedPixels.data()),capturedPixels.size()*4);
        std::ofstream size(captureDirectory/(std::string(name)+".txt"));size<<captureWidth<<" "<<captureHeight;
        std::printf("AA CAPTURE %s mode=%u enabled=%u samples=%u frames=%d ms=%llu\n",name,unsigned(app->antialiasing.algorithm),
            app->antialiasing.enabled,unsigned(r->presentParameters.MultiSampleType),frames,frameMs);
    };
    const auto select=[&](antialias::Algorithm mode) {
        SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kFirstCommand+unsigned(mode),0);
        Check(app->antialiasing.algorithm==mode,"menu selects actual algorithm");
    };
    select(antialias::Algorithm::MmdOriginal);
    if(!app->antialiasing.enabled)CommandDispatch(nullptr,MAKEWPARAM(277,0));
    capture("original-aa");
    CommandDispatch(nullptr,MAKEWPARAM(277,0));capture("disabled");
    const auto disabled=capturedPixels;
    select(antialias::Algorithm::Fxaa);
    Check(app->antialiasing.enabled,"algorithm selection enables the main AA item");
    capture("fxaa");
    Check(disabled!=capturedPixels,"FXAA changes actual rendered edge pixels");
    auto* formatQuery=(*reinterpret_cast<void***>(r->d3d9))[10];
    Check(MH_CreateHook(formatQuery,&RejectAntialiasFloatTarget,reinterpret_cast<void**>(&originalAntialiasCheckFormat))==MH_OK &&
        MH_EnableHook(formatQuery)==MH_OK,"simulate unavailable standard D3D9 floating-point target");
    std::wstring capabilityError;
    Check(!antialias::Supported(app,antialias::Algorithm::Taa) &&
        !antialias::Select(app,antialias::Algorithm::Taa,capabilityError) && !capabilityError.empty(),
        "TAA rejects a device without floating-point render targets");
    Check(!antialias::Supported(app,antialias::Algorithm::SmaaT2xStandard) &&
        !antialias::Supported(app,antialias::Algorithm::SmaaT2xEnhanced) &&
        antialias::Supported(app,antialias::Algorithm::SmaaStandard) && antialias::Supported(app,antialias::Algorithm::Smaa),
        "SMAA T2x requires standard motion targets while both 1x presets remain available");
    Check(rejectedAntialiasFloatQueries>0 && app->antialiasing.algorithm==antialias::Algorithm::Fxaa && app->antialiasing.enabled,
        "unsupported temporal hardware preserves the working spatial AA mode");
    MH_DisableHook(formatQuery);MH_RemoveHook(formatQuery);
    Check(PostDeviceReset(app),"reset FXAA device");capture("fxaa-reset");
    select(antialias::Algorithm::Taa);capture("taa",18);
    std::printf("TAA samples=%u\n",antialias::AccumulatedSamples(app));
    Check(antialias::AccumulatedSamples(app)>=8,"TAA accumulates real jittered static frames");
    app->CameraYaw()+=.02f;capture("taa-camera-moved",1);
    Check(antialias::AccumulatedSamples(app)>=9,"camera motion reprojects temporal history instead of disabling TAA");
    const auto movingStart=antialias::AccumulatedSamples(app);
    for(int frame=0;frame<12;++frame) {
        app->CameraYaw()+=.004f;capture("taa-camera-playing",1);
    }
    Check(antialias::AccumulatedSamples(app)>=movingStart+12,"continuous camera motion retains temporal samples");
    const auto cameraMotion=antialias::motion::GetStats(app);
    const auto cameraPixels=ReadAntialiasMotionField(app).MaximumPixels();
    std::printf("TAA camera draws=%u matched=%u motion_px=%g captures=%u cache_hits=%u\n",cameraMotion.draws,cameraMotion.matchedDraws,
        cameraPixels,cameraMotion.vertexCaptures,cameraMotion.vertexCacheHits);
    Check(cameraMotion.matchedDraws>0 && cameraPixels>.01f,
        "camera motion produces real geometry velocity for history reprojection");
    app->CameraYaw()-=.068f;
    const auto* record=mdl::Mdl(app->SelectedModel());
    auto* bones=mdl::Bones(app->SelectedModel());
    Check(bones && record->boneCount,"animated TAA fixture has a skinned bone");
    std::vector<float> savedBones(std::size_t(record->boneCount)*16);
    for(unsigned bone=0;bone<record->boneCount;++bone)
        std::memcpy(savedBones.data()+std::size_t(bone)*16,bones[bone].matInit,16*sizeof(float));
    const auto savedFrame=app->CurrentFrame();const auto savedPlayback=app->PlaybackActive();
    const auto playbackStart=antialias::AccumulatedSamples(app);
    std::printf("TAA playback begin frame=%d samples=%u required=%u outline_mode=%u previous_playback=%d viewport_tool=%u\n",
        savedFrame,playbackStart,antialias::RequiredSamples(app),unsigned(app->antialiasing.outlineMode),int(savedPlayback),app->UsesViewportTool()?1u:0u);
    const auto beforePlayback=capturedPixels;
    app->PlaybackActive()=1;
    for(int frame=0;frame<12;++frame) {
        // Exercise the production playback rendering branch with deterministic
        // bone deformation; wall-clock animation updates cannot hide a reset.
        for(unsigned bone=0;bone<record->boneCount;++bone) {
            std::memcpy(bones[bone].matInit,savedBones.data()+std::size_t(bone)*16,16*sizeof(float));
            bones[bone].matInit[12]+=(frame+1)*.02f*(1+bone%3);
        }
        app->CurrentFrame()=savedFrame+frame+1;
        app->state.messageSeen=10;RenderFrameScene(app);
        captureRequested=true;r->device->Present(nullptr,nullptr,nullptr,nullptr);
        Check(!captureRequested,"animated production frame reaches presentation");
        const auto motion=antialias::motion::GetStats(app);
        const auto outline=antialias::outline_multisample::GetStats(app);
        std::printf("TAA playback step=%d frame=%d samples=%u required=%u motion=%u/%u captures=%u hits=%u gpu=%u readbacks=%u coverage=%u opaque_skips=%u outline_samples=%u outlines=%u composites=%u failed=%u fallback=%s\n",
            frame,app->CurrentFrame(),antialias::AccumulatedSamples(app),antialias::RequiredSamples(app),motion.matchedDraws,motion.draws,
            motion.vertexCaptures,motion.vertexCacheHits,motion.gpuVertexCaptures,motion.cpuVertexReadbacks,motion.coverageReplays,motion.opaqueCoverageSkips,
            outline.sampleCount,outline.outlineDraws,outline.composites,outline.frameFailed?1u:0u,outline.fallbackReason.c_str());
    }
    Check(antialias::AccumulatedSamples(app)>=playbackStart+12,
        "playback with changing skinned positions keeps temporal accumulation active");
    const auto boneMotion=antialias::motion::GetStats(app);
    const auto bonePixels=ReadAntialiasMotionField(app).MaximumPixels();
    std::printf("TAA deformed draws=%u matched=%u motion_px=%g captures=%u cache_hits=%u\n",boneMotion.draws,boneMotion.matchedDraws,
        bonePixels,boneMotion.vertexCaptures,boneMotion.vertexCacheHits);
    Check(boneMotion.matchedDraws>0 && bonePixels>.01f,
        "deformed skinned vertices produce real motion vectors during playback");
    Check(capturedPixels!=beforePlayback,"animated TAA test changes actual rendered pixels");
    for(unsigned bone=0;bone<record->boneCount;++bone)
        std::memcpy(bones[bone].matInit,savedBones.data()+std::size_t(bone)*16,16*sizeof(float));
    app->CurrentFrame()=savedFrame;app->PlaybackActive()=savedPlayback;
    // Compare both implementations on the same geometry and camera delta.
    // The capability hook also exercises old AMD/Intel devices without VTF.
    // FrameDriver advances playback and physics after the scene render, so these
    // parity captures drive the production scene render and the real device
    // Present directly with frozen, identical inputs for both rounds.
    const auto parityFrame=app->CurrentFrame();
    const auto parityCursor=app->PlaybackCursorSeconds();
    const auto parityPlayback=app->PlaybackActive();
    const auto parityDelta=app->DeltaTime();
    const auto parityMessageSeen=app->state.messageSeen;
    const auto parityCamera=CaptureAntialiasCamera(app);
    const auto parityBones=SaveAntialiasBoneSnapshot(app);
    // MMHack detects playback through the current-frame EDIT, not the host's
    // PlaybackActive byte. The direct scene path bypasses UpdateBoneFrames,
    // which normally disables this control when playback begins. Preserve that
    // observable playback input as well as the host clock for both rounds.
    const HWND parityFrameEdit=GetDlgItem(app->MainWindow(),panel::kCurrentFrameEdit);
    wchar_t parityFrameClass[32]{};
    Check(IsWindow(parityFrameEdit)&&GetClassNameW(parityFrameEdit,parityFrameClass,32)&&
        _wcsicmp(parityFrameClass,L"EDIT")==0,"paired comparison has the real current-frame edit");
    struct PlaybackEditState {
        HWND window;
        BOOL enabled;
        bool restored=false;
        void Restore() { if(!restored) { EnableWindow(window,enabled);restored=true; } }
        ~PlaybackEditState() { Restore(); }
    } parityEditState{parityFrameEdit,IsWindowEnabled(parityFrameEdit)};
    const auto setPairInputs=[&](bool moving) {
        RestoreAntialiasBoneSnapshot(app,parityBones);
        ApplyAntialiasCamera(app,parityCamera);
        if(moving)app->CameraYaw()=parityCamera.yaw+.02f;
        app->CurrentFrame()=parityFrame;
        app->PlaybackCursorSeconds()=parityCursor;
        app->PlaybackActive()=1;
        EnableWindow(parityFrameEdit,FALSE);
        Check(!IsWindowEnabled(parityFrameEdit),"paired comparison disables the playback frame edit");
        app->DeltaTime()=1.f/30;
        app->state.messageSeen=10;
    };
    const auto captureParity=[&](const char* name,int frames,bool moving,AntialiasParityInput* recorded,
                                 AntialiasDrawTraceProbe* drawTrace=nullptr) {
        const auto until=GetTickCount64()+90000;
        const bool wasEnabled=app->antialiasing.enabled;
        captureRequested=false;
        while(antialias::Initializing(app)&&GetTickCount64()<until) {
            setPairInputs(moving);RenderFrameScene(app);
            r->device->Present(nullptr,nullptr,nullptr,nullptr);Sleep(1);
        }
        Check(!antialias::Initializing(app)&&app->antialiasing.enabled==wasEnabled,
            "requested AA is ready before paired comparison capture");
        const auto frameStart=GetTickCount64();
        for(int frame=0;frame<frames;++frame) {
            setPairInputs(moving);
            const bool last=frame==frames-1;
            if(last&&recorded)CaptureAntialiasParityInput(app,*recorded);
            captureRequested=last;
            if(last&&drawTrace) {
                // The motion observer is already registered from the warm
                // frames, so it observes and writes before this second observer.
                AntialiasDrawTraceScope scope(*drawTrace);
                RenderFrameScene(app);
            } else RenderFrameScene(app);
            if(last&&drawTrace)drawTrace->Finish();
            if(const HMODULE hack=GetModuleHandleW(L"MMHack.dll")) {
                using EditMode=BOOL(WINAPI*)();
                using FrameTime=float(WINAPI*)();
                const auto editMode=reinterpret_cast<EditMode>(GetProcAddress(hack,"IsEditMode"));
                const auto frameTime=reinterpret_cast<FrameTime>(GetProcAddress(hack,"GetCurrentFrameTime"));
                Check(editMode&&frameTime,"paired MME comparison resolves actual bridge clock exports");
                const BOOL editing=editMode();
                const float time=frameTime();
                std::printf("PAIR MME name=%s frame=%d edit=%d time=%.9f host_time=%.9f frame_edit_enabled=%d\n",
                    name,frame,int(editing),time,ExpGetFrameTime(),
                    int(IsWindowEnabled(GetDlgItem(app->MainWindow(),panel::kCurrentFrameEdit))));
                Check(!editing&&time==ExpGetFrameTime(),
                    "paired MME comparison uses playback mode and the submitted host time");
            }
            r->device->Present(nullptr,nullptr,nullptr,nullptr);
            Check(!captureRequested,"paired comparison frame reaches real presentation");
        }
        const auto frameMs=GetTickCount64()-frameStart;
        Check(!captureRequested&&!capturedPixels.empty(),"saved PMM pixels captured from paired comparison present");
        std::ofstream file(captureDirectory/(std::string(name)+".bgra"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(capturedPixels.data()),capturedPixels.size()*4);
        std::ofstream size(captureDirectory/(std::string(name)+".txt"));size<<captureWidth<<" "<<captureHeight;
        std::printf("AA CAPTURE %s mode=%u enabled=%u samples=%u frames=%d ms=%llu\n",name,unsigned(app->antialiasing.algorithm),
            app->antialiasing.enabled,unsigned(r->presentParameters.MultiSampleType),frames,frameMs);
    };
    // Default-off per-draw forensics. Other values must not register a second
    // observer, read the GPU motion field or write a draw table.
    const char* const drawTraceSetting=std::getenv("MMS_AA_DRAW_TRACE");
    const bool drawTraceEnabled=drawTraceSetting&&std::strcmp(drawTraceSetting,"1")==0;
    AntialiasDrawTraceProbe gpuDrawTrace(app,"gpu"),cpuDrawTrace(app,"cpu");
    AntialiasParityInput gpuWarm,gpuMotion;
    select(antialias::Algorithm::Fxaa);captureParity("gpu-parity-reset",1,false,nullptr);
    select(antialias::Algorithm::Taa);captureParity("gpu-parity-warm",3,false,&gpuWarm);
    PrintAntialiasParityInput("warm","gpu",gpuWarm);
    captureParity("gpu-parity-motion",1,true,&gpuMotion,drawTraceEnabled?&gpuDrawTrace:nullptr);
    PrintAntialiasParityInput("motion","gpu",gpuMotion);
    const auto gpuField=ReadAntialiasMotionField(app);
    const auto gpuStats=antialias::motion::GetStats(app);
    std::printf("GPU MOTION captures=%u cache_hits=%u cpu_readbacks=%u coverage=%u skipped=%u\n",
        gpuStats.gpuVertexCaptures,gpuStats.vertexCacheHits,gpuStats.cpuVertexReadbacks,gpuStats.coverageReplays,gpuStats.opaqueCoverageSkips);
    Check(gpuStats.gpuVertexCaptures==0||gpuStats.cpuVertexReadbacks==0,
        "GPU temporal path never calls CPU vertex readback");
    app->CameraYaw()=parityCamera.yaw;
    select(antialias::Algorithm::Fxaa);captureParity("cpu-parity-reset",1,false,nullptr);
    rejectAntialiasVertexTexture=true;
    Check(MH_CreateHook(formatQuery,&RejectAntialiasFloatTarget,reinterpret_cast<void**>(&originalAntialiasCheckFormat))==MH_OK&&
        MH_EnableHook(formatQuery)==MH_OK,"simulate unavailable vertex texture sampling without rejecting temporal targets");
    const auto coverageQuery=reinterpret_cast<void*>(&deformed_mesh_capture::OpaqueCoverage);
    Check(MH_CreateHook(coverageQuery,&RequireOriginalCoverageForTest,nullptr)==MH_OK&&MH_EnableHook(coverageQuery)==MH_OK,
        "reference path replays coverage for every native material");
    AntialiasParityInput cpuWarm,cpuMotion;
    select(antialias::Algorithm::Taa);captureParity("cpu-parity-warm",3,false,&cpuWarm);
    PrintAntialiasParityInput("warm","cpu",cpuWarm);
    Check(gpuWarm.frame==cpuWarm.frame&&gpuWarm.playbackCursorSeconds==cpuWarm.playbackCursorSeconds&&
        gpuWarm.effectFrameTime==cpuWarm.effectFrameTime,
        "GPU and CPU warm submissions share the same frame, playback cursor and effect time");
    Check(SameAntialiasCamera(gpuWarm.camera,cpuWarm.camera),
        "GPU and CPU warm submissions share every camera input");
    Check(SameAntialiasBones(gpuWarm,cpuWarm),
        "GPU and CPU warm submissions share every loaded model bone matrix");
    captureParity("cpu-parity-motion",1,true,&cpuMotion,drawTraceEnabled?&cpuDrawTrace:nullptr);
    PrintAntialiasParityInput("motion","cpu",cpuMotion);
    const auto cpuField=ReadAntialiasMotionField(app);
    const auto cpuStats=antialias::motion::GetStats(app);
    Check(cpuStats.gpuVertexCaptures==0&&cpuStats.matchedDraws>0&&antialias::AccumulatedSamples(app)>=4,
        "no-VTF adapter preserves correct temporal accumulation through compatible capture");
    Check(cpuStats.opaqueCoverageSkips==0&&cpuStats.coverageReplays==cpuStats.draws,
        "CPU reference keeps all original coverage draws");
    Check(gpuMotion.frame==cpuMotion.frame&&gpuMotion.playbackCursorSeconds==cpuMotion.playbackCursorSeconds&&
        gpuMotion.effectFrameTime==cpuMotion.effectFrameTime,
        "GPU and CPU motion submissions share the same frame, playback cursor and effect time");
    Check(SameAntialiasCamera(gpuMotion.camera,cpuMotion.camera),
        "GPU and CPU motion submissions share every camera input");
    Check(SameAntialiasBones(gpuMotion,cpuMotion),
        "GPU and CPU motion submissions share every loaded model bone matrix");
    Check(gpuField.MaximumPixels()>.01f&&cpuField.MaximumPixels()>.01f,
        "paired motion frames contain real nonzero camera-induced geometry motion");
    unsigned compared=0,different=0,coverageUnion=0,coverageDifference=0;float largest=0;
    Check(cpuField.width==gpuField.width&&cpuField.height==gpuField.height,"GPU and CPU motion fields have matching dimensions");
    if(cpuField.pixels.size()==gpuField.pixels.size())for(std::size_t i=0;i<cpuField.pixels.size();++i) {
        const auto& a=gpuField.pixels[i];const auto& b=cpuField.pixels[i];
        const bool validA=a[2]>0&&a[3]>0,validB=b[2]>0&&b[3]>0;
        coverageUnion+=validA||validB;coverageDifference+=validA!=validB;
        if(validA&&validB) {
            ++compared;float difference=0;
            for(unsigned k=0;k<4;++k)difference=std::max(difference,std::abs(a[k]-b[k])/std::max(1.f,std::abs(b[k])));
            largest=std::max(largest,difference);different+=difference>1e-4f;
        }
    }
    std::printf("GPU PARITY compared=%u different=%u max_relative_error=%g\n",compared,different,largest);
    std::printf("GPU COVERAGE union=%u mismatches=%u\n",coverageUnion,coverageDifference);
    // Default-off forensics for the four-component comparison: only an exact
    // MMS_AA_PARITY_TRACE=1 serializes the two fields already read above. No
    // GPU readback, render or device change happens here, and the original
    // coverage/parity assertions below still run unchanged.
    const char* const parityTrace=std::getenv("MMS_AA_PARITY_TRACE");
    if(parityTrace&&std::strcmp(parityTrace,"1")==0) {
        static_assert(sizeof(std::array<float,4>)==4*sizeof(float),
            "forensic motion capture writes four tightly packed single-precision components");
        const auto writeField=[&](const char* name,const AntialiasMotionField& field) {
            const std::uint64_t declared=std::uint64_t(field.width)*std::uint64_t(field.height);
            Check(field.width>0&&field.height>0&&declared==field.pixels.size(),
                (std::string("forensic ")+name+" has consistent width, height and pixel count").c_str());
            // Bound the byte count before multiplying or converting to streamsize.
            const std::uint64_t byteLimit=std::min(
                std::uint64_t(std::numeric_limits<std::streamsize>::max()),
                std::uint64_t(std::numeric_limits<std::size_t>::max()));
            Check(declared<=byteLimit/std::uint64_t(4*sizeof(float)),
                (std::string("forensic ")+name+" fits size_t and streamsize byte counts").c_str());
            const std::size_t bytes=static_cast<std::size_t>(declared)*sizeof(float)*4;
            std::ofstream file(captureDirectory/(std::string(name)+".rgba32f"),std::ios::binary|std::ios::trunc);
            Check(file.is_open(),(std::string("forensic ")+name+" opens for write").c_str());
            file.write(reinterpret_cast<const char*>(field.pixels.data()),static_cast<std::streamsize>(bytes));
            file.flush();
            Check(file.good(),(std::string("forensic ")+name+" writes and flushes every pixel").c_str());
            file.close();
            Check(!file.fail(),(std::string("forensic ")+name+" closes cleanly").c_str());
            std::printf("GPU FIELD %s width=%u height=%u pixels=%llu bytes=%llu\n",name,field.width,field.height,
                static_cast<unsigned long long>(field.pixels.size()),static_cast<unsigned long long>(bytes));
        };
        writeField("motion-gpu",gpuField);
        writeField("motion-cpu",cpuField);
        struct ComponentForensics {
            const char* name=nullptr;
            std::uint64_t exceeded=0,nonfiniteGpu=0,nonfiniteCpu=0;
            float maxAbsolute=0,maxRelative=0;
            bool hasAbsolute=false,hasRelative=false;
            unsigned absoluteX=0,absoluteY=0,relativeX=0,relativeY=0;
            std::array<float,4> gpuAtAbsolute{},cpuAtAbsolute{},gpuAtRelative{},cpuAtRelative{};
        };
        const std::array<const char*,4> componentNames{"velocityX","velocityY","currentDepth","previousDepth"};
        std::array<ComponentForensics,4> components{};
        for(unsigned k=0;k<4;++k)components[k].name=componentNames[k];
        for(const auto& pixel:gpuField.pixels)for(unsigned k=0;k<4;++k)
            if(!std::isfinite(pixel[k]))++components[k].nonfiniteGpu;
        for(const auto& pixel:cpuField.pixels)for(unsigned k=0;k<4;++k)
            if(!std::isfinite(pixel[k]))++components[k].nonfiniteCpu;
        const bool pairable=gpuField.width==cpuField.width&&gpuField.height==cpuField.height&&
            gpuField.pixels.size()==cpuField.pixels.size()&&gpuField.width>0&&gpuField.height>0;
        std::uint64_t validGpu=0,validCpu=0,paired=0;
        if(pairable)for(std::size_t i=0;i<gpuField.pixels.size();++i) {
            const auto& a=gpuField.pixels[i];const auto& b=cpuField.pixels[i];
            const bool validA=a[2]>0&&a[3]>0,validB=b[2]>0&&b[3]>0;
            validGpu+=validA;validCpu+=validB;
            if(!(validA&&validB))continue;
            ++paired;
            const unsigned x=unsigned(i%gpuField.width),y=unsigned(i/gpuField.width);
            for(unsigned k=0;k<4;++k) {
                auto& component=components[k];
                // Mirror the original float expression and threshold exactly.
                const float absoluteError=std::abs(a[k]-b[k]);
                const float relativeError=absoluteError/std::max(1.f,std::abs(b[k]));
                if(relativeError>1e-4f)++component.exceeded;
                if(!std::isnan(absoluteError)&&(!component.hasAbsolute||absoluteError>component.maxAbsolute)) {
                    component.hasAbsolute=true;component.maxAbsolute=absoluteError;
                    component.absoluteX=x;component.absoluteY=y;
                    component.gpuAtAbsolute=a;component.cpuAtAbsolute=b;
                }
                if(!std::isnan(relativeError)&&(!component.hasRelative||relativeError>component.maxRelative)) {
                    component.hasRelative=true;component.maxRelative=relativeError;
                    component.relativeX=x;component.relativeY=y;
                    component.gpuAtRelative=a;component.cpuAtRelative=b;
                }
            }
        }
        if(!pairable)std::fprintf(stderr,
            "WARN forensic motion fields cannot be paired: gpu=%ux%u/%llu cpu=%ux%u/%llu\n",
            gpuField.width,gpuField.height,static_cast<unsigned long long>(gpuField.pixels.size()),
            cpuField.width,cpuField.height,static_cast<unsigned long long>(cpuField.pixels.size()));
        const auto valueOrNan=[](bool present,float value) { return present?double(value):std::nan(""); };
        const auto formatFloat=[](float value) {
            char text[32];std::snprintf(text,sizeof text,"%.9g",value);return std::string(text);
        };
        for(const auto& component:components)
            std::printf("GPU COMPONENT name=%s compared=%llu exceeded=%llu nonfinite_gpu=%llu nonfinite_cpu=%llu "
                "max_abs=%.9g at=(%d,%d) max_rel=%.9g at=(%d,%d) "
                "gpu_at_max_abs=%.9g,%.9g,%.9g,%.9g cpu_at_max_abs=%.9g,%.9g,%.9g,%.9g "
                "gpu_at_max_rel=%.9g,%.9g,%.9g,%.9g cpu_at_max_rel=%.9g,%.9g,%.9g,%.9g\n",
                component.name,static_cast<unsigned long long>(paired),
                static_cast<unsigned long long>(component.exceeded),
                static_cast<unsigned long long>(component.nonfiniteGpu),
                static_cast<unsigned long long>(component.nonfiniteCpu),
                valueOrNan(component.hasAbsolute,component.maxAbsolute),
                component.hasAbsolute?int(component.absoluteX):-1,
                component.hasAbsolute?int(component.absoluteY):-1,
                valueOrNan(component.hasRelative,component.maxRelative),
                component.hasRelative?int(component.relativeX):-1,
                component.hasRelative?int(component.relativeY):-1,
                valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[0]),valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[1]),
                valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[2]),valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[3]),
                valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[0]),valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[1]),
                valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[2]),valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[3]),
                valueOrNan(component.hasRelative,component.gpuAtRelative[0]),valueOrNan(component.hasRelative,component.gpuAtRelative[1]),
                valueOrNan(component.hasRelative,component.gpuAtRelative[2]),valueOrNan(component.hasRelative,component.gpuAtRelative[3]),
                valueOrNan(component.hasRelative,component.cpuAtRelative[0]),valueOrNan(component.hasRelative,component.cpuAtRelative[1]),
                valueOrNan(component.hasRelative,component.cpuAtRelative[2]),valueOrNan(component.hasRelative,component.cpuAtRelative[3]));
        std::ofstream table(captureDirectory/"motion-parity.tsv",std::ios::trunc);
        Check(table.is_open(),"forensic motion parity table opens");
        table<<"# MMS_AA_PARITY_TRACE=1 forensic capture of the paired GPU/CPU motion fields\n"
            "# rgba32f order: 0=velocityX 1=velocityY 2=currentDepth 3=previousDepth\n"
            "# valid pixel: currentDepth>0 and previousDepth>0 (original check definition)\n"
            "# relative error: |gpu-cpu|/max(1,|cpu|), original parity threshold 1e-4\n"
            "# positions are the pixel of the largest non-NaN absolute/relative error; nan=missing\n";
        table<<"original_threshold\t1e-4\n"
            <<"width\t"<<gpuField.width<<"\nheight\t"<<gpuField.height<<"\n"
            <<"pixels_gpu\t"<<gpuField.pixels.size()<<"\npixels_cpu\t"<<cpuField.pixels.size()<<"\n"
            <<"coverage_valid_gpu\t"<<validGpu<<"\ncoverage_valid_cpu\t"<<validCpu<<"\n"
            <<"coverage_union\t"<<coverageUnion<<"\ncoverage_mismatch\t"<<coverageDifference<<"\n"
            <<"compared\t"<<compared<<"\nmerged_different\t"<<different<<"\n"
            <<"merged_max_relative\t"<<formatFloat(largest)<<"\npairable\t"<<(pairable?1:0)<<"\n";
        table<<"component\texceeded_1e-4\tnonfinite_gpu\tnonfinite_cpu\tmax_abs\tmax_abs_x\tmax_abs_y\tmax_rel\tmax_rel_x\tmax_rel_y"
            "\tgpu_abs_0\tgpu_abs_1\tgpu_abs_2\tgpu_abs_3\tcpu_abs_0\tcpu_abs_1\tcpu_abs_2\tcpu_abs_3"
            "\tgpu_rel_0\tgpu_rel_1\tgpu_rel_2\tgpu_rel_3\tcpu_rel_0\tcpu_rel_1\tcpu_rel_2\tcpu_rel_3\n";
        char line[2048];
        for(const auto& component:components) {
            const int written=std::snprintf(line,sizeof line,
                "%s\t%llu\t%llu\t%llu\t%.9g\t%d\t%d\t%.9g\t%d\t%d"
                "\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g"
                "\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\n",
                component.name,
                static_cast<unsigned long long>(component.exceeded),
                static_cast<unsigned long long>(component.nonfiniteGpu),
                static_cast<unsigned long long>(component.nonfiniteCpu),
                valueOrNan(component.hasAbsolute,component.maxAbsolute),
                component.hasAbsolute?int(component.absoluteX):-1,
                component.hasAbsolute?int(component.absoluteY):-1,
                valueOrNan(component.hasRelative,component.maxRelative),
                component.hasRelative?int(component.relativeX):-1,
                component.hasRelative?int(component.relativeY):-1,
                valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[0]),valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[1]),
                valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[2]),valueOrNan(component.hasAbsolute,component.gpuAtAbsolute[3]),
                valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[0]),valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[1]),
                valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[2]),valueOrNan(component.hasAbsolute,component.cpuAtAbsolute[3]),
                valueOrNan(component.hasRelative,component.gpuAtRelative[0]),valueOrNan(component.hasRelative,component.gpuAtRelative[1]),
                valueOrNan(component.hasRelative,component.gpuAtRelative[2]),valueOrNan(component.hasRelative,component.gpuAtRelative[3]),
                valueOrNan(component.hasRelative,component.cpuAtRelative[0]),valueOrNan(component.hasRelative,component.cpuAtRelative[1]),
                valueOrNan(component.hasRelative,component.cpuAtRelative[2]),valueOrNan(component.hasRelative,component.cpuAtRelative[3]));
            Check(written>0&&static_cast<std::size_t>(written)<sizeof line,
                "forensic motion parity row fits its line buffer");
            table<<line;
        }
        table.flush();
        Check(table.good(),"forensic motion parity table writes and flushes");
        table.close();
        Check(!table.fail(),"forensic motion parity table closes cleanly");
    }
    Check(coverageUnion>100&&coverageDifference*1000u<=coverageUnion,
        "optimized motion coverage matches complete reference coverage at 99.9 percent of valid pixels");
    Check(compared>100&&different*1000u<=compared,"GPU history velocities and depths agree with CPU reference at 99.9 percent of valid pixels");
    app->CameraYaw()=parityCamera.yaw;MH_DisableHook(formatQuery);MH_RemoveHook(formatQuery);rejectAntialiasVertexTexture=false;
    MH_DisableHook(coverageQuery);MH_RemoveHook(coverageQuery);
    // Restore the state saved on entry to the paired comparison block.
    app->CurrentFrame()=parityFrame;
    app->PlaybackCursorSeconds()=parityCursor;
    app->PlaybackActive()=parityPlayback;
    app->DeltaTime()=parityDelta;
    app->state.messageSeen=parityMessageSeen;
    parityEditState.Restore();
    Check(IsWindowEnabled(parityFrameEdit)==parityEditState.enabled,
        "paired comparison restores the prior frame-edit enabled state");
    ApplyAntialiasCamera(app,parityCamera);
    RestoreAntialiasBoneSnapshot(app,parityBones);
    select(antialias::Algorithm::Msaa2);capture("msaa2");
    Check(r->presentParameters.MultiSampleType==D3DMULTISAMPLE_2_SAMPLES,"MSAA 2 uses 2 samples");
    select(antialias::Algorithm::Msaa4);capture("msaa4");
    Check(r->presentParameters.MultiSampleType==D3DMULTISAMPLE_4_SAMPLES,"MSAA 4 uses 4 samples");
    select(antialias::Algorithm::Msaa8);capture("msaa8");
    select(antialias::Algorithm::Smaa);capture("smaa");
    Check(disabled!=capturedPixels,"SMAA changes actual rendered edge pixels");
    Check(PostDeviceReset(app),"reset SMAA device");capture("smaa-reset");
    std::wstring smaaError;
    Check(viewport_render_scale::Configure(app,false,150,smaaError),"combine SMAA with viewport scaling");capture("smaa-scale150");
    Check(viewport_render_scale::Configure(app,true,100,smaaError),"restore viewport scale after SMAA");
    select(antialias::Algorithm::SmaaStandard);capture("smaa-standard");
    for(const auto mode:{antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
        select(mode);
        const auto before=app->CurrentFrame();app->CurrentFrame()=0;
        capture(mode==antialias::Algorithm::SmaaT2xStandard?"smaa-t2x-standard":"smaa-t2x-enhanced",2);
        Check(app->CurrentFrame()==0 && antialias::RequiredSamples(app)==2 && antialias::AccumulatedSamples(app)>=2,
            "SMAA T2x accumulates both actual submissions in a static frame-zero scene");
        app->CameraYaw()+=.004f;capture("smaa-t2x-camera",1);
        const auto moving=antialias::motion::GetStats(app);
        const auto movingPixels=ReadAntialiasMotionField(app).MaximumPixels();
        std::printf("SMAA T2x mode=%u draws=%u matched=%u motion_px=%g captures=%u cache_hits=%u\n",
            unsigned(mode),moving.draws,moving.matchedDraws,movingPixels,moving.vertexCaptures,moving.vertexCacheHits);
        Check(moving.matchedDraws>0 && movingPixels>.01f && antialias::AccumulatedSamples(app)>=3,
            "SMAA T2x retains both phases while reprojecting real camera motion");
        app->CameraYaw()-=.004f;app->CurrentFrame()=before;
    }
    select(antialias::Algorithm::Fxaa);
    Check(app->antialiasing.lastSmaaAlgorithm==antialias::Algorithm::SmaaT2xEnhanced,
        "switching to FXAA preserves the last selected SMAA variant");
    SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kSmaaCommand,0);
    Check(app->antialiasing.algorithm==antialias::Algorithm::Fxaa,
        "former SMAA parent command cannot apply a remembered variant");
    CommandDispatch(nullptr,MAKEWPARAM(277,0));capture("fxaa-disabled");
    Check(app->antialiasing.algorithm==antialias::Algorithm::Fxaa && !app->antialiasing.enabled,
        "main item toggles without forgetting algorithm");
    CommandDispatch(nullptr,MAKEWPARAM(277,0));capture("fxaa-reenabled");
    std::wstring error;
    Check(viewport_render_scale::Configure(app,false,150,error),"combine AA with viewport scaling");capture("fxaa-scale150");
    Check(viewport_render_scale::Configure(app,true,100,error),"restore viewport scale");
    // Exercise the production recording branch without starting a codec or
    // touching a user output file. It ends before editor overlays are drawn.
    app->RecordingWindow()=CreateWindowW(L"STATIC",L"Hidden AA export",WS_POPUP,0,0,640,480,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(app->RecordingWindow()!=nullptr,"hidden recording surface created");
    Check(EnsureCaptureRenderTarget(app),"initialize the production recording target before AA begins");
    Microsoft::WRL::ComPtr<IDirect3DSurface9> recordingTarget;
    r->device->GetRenderTarget(0,&recordingTarget);
    app->state.messageSeen=10;RenderFrameScene(app);
    captureRequested=true;r->device->Present(nullptr,nullptr,nullptr,nullptr);
    Check(!captureRequested,"FXAA completes recording frame before presentation");
    Microsoft::WRL::ComPtr<IDirect3DSurface9> restored;
    r->device->GetRenderTarget(0,&restored);
    Check(restored.Get()==recordingTarget.Get(),"recording restores production target ownership");
    restored.Reset();recordingTarget.Reset();
    select(antialias::Algorithm::Smaa);
    Check(EnsureCaptureRenderTarget(app),"restore capture target after the SMAA device reset");
    r->device->GetRenderTarget(0,&recordingTarget);
    app->state.messageSeen=10;RenderFrameScene(app);
    captureRequested=true;r->device->Present(nullptr,nullptr,nullptr,nullptr);
    Check(!captureRequested,"SMAA completes recording frame before presentation");
    restored.Reset();r->device->GetRenderTarget(0,&restored);
    Check(restored.Get()==recordingTarget.Get(),"SMAA recording restores production target ownership");
    restored.Reset();recordingTarget.Reset();
    select(antialias::Algorithm::Taa);
    const auto recordingFrame=app->CurrentFrame();
    for(int frame=0;frame<10;++frame) {
        app->CurrentFrame()=recordingFrame+frame;
        app->state.messageSeen=10;RenderFrameScene(app);
    }
    Check(antialias::AccumulatedSamples(app)>=8,"recording continues temporal accumulation across changing frames");
    app->CurrentFrame()=recordingFrame;
    DestroyWindow(app->RecordingWindow());app->RecordingWindow()=nullptr;
    // Exercise both shared outline strategies with each post-process family.
    // Require real native geometry rather than treating an empty/fallback
    // outline layer as successful MME compatibility.
    unsigned eligibleOutlineModels=0,eligibleOutlineMaterials=0;
    for(int slot=0;slot<kModelSlotCount;++slot)if(auto* model=app->ModelSlot(slot)) {
        const auto& state=*mdl::Mdl(model);
        if(state.loadComplete==0 || state.displayState!=0 || state.edgeScale<=0 || state.postLoadFlag2!=0)continue;
        auto* materials=mdl::Materials(model);if(!materials)continue;
        ++eligibleOutlineModels;
        for(unsigned material=0;material<state.materialCount;++material)
            eligibleOutlineMaterials+=materials[material].faceVertexCount>0 && materials[material].doubleSided!=0;
    }
    std::printf("OUTLINE fixture eligible_models=%u eligible_materials=%u playback=%d viewport_tool=%u effect_suppressed=%d\n",
        eligibleOutlineModels,eligibleOutlineMaterials,int(app->PlaybackActive()),app->UsesViewportTool()?1u:0u,int(app->ModelNonDisplayMode()));
    Check(eligibleOutlineModels>0 && eligibleOutlineMaterials>0,
        "shared-outline fixture contains visible loaded native models with enabled outline material draws");
    Check(app->PlaybackActive()!=0 || app->UsesViewportTool(),
        "shared-outline fixture opens the renderer's camera/playback outline gate");
    Check(r->postProcessEnabled==0 || app->ModelNonDisplayMode()==0,
        "shared-outline fixture does not suppress native outlines in the effect-rendering path");
    std::ofstream strategyTable(captureDirectory/L"outline-strategies.tsv");
    Check(strategyTable.good(),"create shared-outline execution evidence");
    strategyTable<<"outline_mode\talgorithm\twarmup_frames\taccumulated_samples\trequired_samples\toutline_samples\tdepth_replays\toutline_draws\tcomposites\tobserved_native\tframe_failed\tfallback\tmotion_draws\tmotion_matched\tmotion_pixels\n";
    for(const auto outline:{antialias::OutlineMode::FxaaAdaptive,antialias::OutlineMode::Multisample}) {
        std::wstring outlineError;
        const auto beforeAlgorithm=app->antialiasing.algorithm;
        const bool beforeEnabled=app->antialiasing.enabled;
        Check(antialias::SelectOutlineMode(app,outline,outlineError),"select shared outline strategy through the production API");
        Check(app->antialiasing.outlineMode==outline && app->antialiasing.algorithm==beforeAlgorithm &&
            app->antialiasing.enabled==beforeEnabled,"outline strategy does not replace the selected scene AA mode");
        for(const auto algorithm:{antialias::Algorithm::Fxaa,antialias::Algorithm::Taa,
                                 antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                                 antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
            select(algorithm);
            const bool temporal=algorithm==antialias::Algorithm::Taa || algorithm==antialias::Algorithm::SmaaT2xStandard ||
                algorithm==antialias::Algorithm::SmaaT2xEnhanced;
            const unsigned expectedSamples=algorithm==antialias::Algorithm::Taa?8:temporal?2:1;
            const int warmupFrames=algorithm==antialias::Algorithm::Taa?10:temporal?4:3;
            const auto name=std::string(outline==antialias::OutlineMode::FxaaAdaptive?"outline-fxaa-adaptive-aa":"outline-msaa-coverage-aa")+
                std::to_string(unsigned(algorithm));
            capture(name.c_str(),warmupFrames);
            const auto warmedSamples=antialias::AccumulatedSamples(app);
            const auto savedYaw=app->CameraYaw();
            if(temporal) {
                // Both observers must keep working when geometry/camera history
                // is used. A static image alone would miss silent motion fallback.
                app->CameraYaw()=savedYaw+.004f;
                capture(name.c_str(),1);
            }
            const auto native=antialias::outline_multisample::GetStats(app);
            const auto motion=antialias::motion::GetStats(app);
            const auto accumulated=antialias::AccumulatedSamples(app),required=antialias::RequiredSamples(app);
            float motionPixels=0;
            if(temporal)motionPixels=ReadAntialiasMotionField(app).MaximumPixels();
            strategyTable<<unsigned(outline)<<'\t'<<unsigned(algorithm)<<'\t'<<warmupFrames<<'\t'<<accumulated<<'\t'<<required<<'\t'
                <<native.sampleCount<<'\t'<<native.depthReplays<<'\t'<<native.outlineDraws<<'\t'<<native.composites<<'\t'
                <<native.observedNativeDevice<<'\t'<<native.frameFailed<<'\t'<<native.fallbackReason<<'\t'
                <<motion.draws<<'\t'<<motion.matchedDraws<<'\t'<<motionPixels<<'\n';
            strategyTable.flush();
            std::printf("OUTLINE mode=%u algorithm=%u accumulated=%u required=%u samples=%u depth=%u draws=%u composites=%u native=%u failed=%u fallback=%s motion=%u/%u motion_px=%g\n",
                unsigned(outline),unsigned(algorithm),accumulated,required,native.sampleCount,native.depthReplays,native.outlineDraws,
                native.composites,native.observedNativeDevice?1u:0u,native.frameFailed?1u:0u,native.fallbackReason.c_str(),
                motion.matchedDraws,motion.draws,motionPixels);
            Check(app->antialiasing.enabled && app->antialiasing.algorithm==algorithm && app->antialiasing.outlineMode==outline,
                "actual shared-outline frame preserves both requested selections and AA activation");
            Check(required==expectedSamples,"shared-outline rendering does not silently replace the requested temporal algorithm with spatial fallback");
            if(temporal) {
                Check(warmedSamples>=unsigned(warmupFrames) && accumulated>=warmedSamples+1,
                    "TAA and SMAA T2x retain actual history across a camera-moving frame with either outline strategy");
                Check(motion.draws>0 && motion.matchedDraws>0 && motionPixels>.01f,
                    "shared outline and temporal observers both preserve matched geometry and real camera velocity");
            }
            if(outline==antialias::OutlineMode::Multisample) {
                Check(native.sampleCount>=4 && native.depthReplays>0 && native.outlineDraws>0 && native.composites==1,
                    "MSAA outline strategy really replays body depth, draws multisampled hulls and composites exactly once");
                Check(native.observedNativeDevice && !native.observing && !native.frameFailed && native.fallbackReason.empty() &&
                    !antialias::outline_multisample::Active(app) && !antialias::outline_multisample::Failed(app) &&
                    antialias::outline_multisample::GetTexture(app)!=nullptr,
                    "MSAA outline frame completes on the native device with a published layer and no fallback");
            } else {
                Check(native.sampleCount==0 && native.depthReplays==0 && native.outlineDraws==0 && native.composites==0 &&
                    !native.observedNativeDevice && !native.observing && !native.frameFailed && native.fallbackReason.empty() &&
                    !antialias::outline_multisample::Active(app) && antialias::outline_multisample::GetTexture(app)==nullptr,
                    "pure FXAA outline strategy never allocates or executes an independent MSAA outline pass");
            }
            app->CameraYaw()=savedYaw;
        }
    }
    {
        std::wstring outlineError;
        Check(antialias::SelectOutlineMode(app,antialias::OutlineMode::FxaaAdaptive,outlineError),
            "restore pure FXAA outline preference before menu and restart checks");
    }
    CheckAntialiasMenus(app);
    if(std::getenv("MMS_SURFACE_MISSING_DX12")) {
        const auto status=skinning::GetStatus();
        Check(blockedSurfaceDx12>0 && status.gpuUpdates==0 && status.cpuUpdates>0,
            "all AA algorithms work after actual missing-DX12 fallback");
    }
    MH_DisableHook(vtable[17]);
}
