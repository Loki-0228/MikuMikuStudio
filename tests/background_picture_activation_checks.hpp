// Included only by the real hidden D3D overlay test, inside its test namespace.
// These checks exercise the production activation object, not a replacement
// renderer, file reader, registry, or publication implementation.
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
namespace activation_checks {
namespace identity = plugins::background_identity;
using Activation = media::PreparedPictureActivation;
using Result = media::PictureActivationResult;
using Asset = media::BackgroundPictureAssetPtr;

struct Snapshot {
    IDirect3DTexture9* texture;
    std::wstring path;
    int width, height, x, y;
    float scale;
    unsigned enabled, dirty, frame;
    std::uint64_t instance, ticket, generation, revision, charged;
    Asset asset;
    explicit Snapshot(MMDApp* app)
        : texture(app->PictureBackgroundTexture()), path(app->PictureBackgroundPath()),
          width(app->PictureWidth()), height(app->PictureHeight()), x(app->PictureOffsetX()),
          y(app->PictureOffsetY()), scale(app->PictureScale()), enabled(app->PictureBackgroundEnabled()),
          dirty(app->SceneModified()), frame(app->CurrentFrame()), instance(identity::Instance(app)),
          ticket(identity::Acquire(app, identity::Kind::Picture)), generation(app->SceneGeneration()),
          revision(app->SceneRevision()), charged(media::GlobalPictureSourceBudget().Usage().used),
          asset(identity::CurrentPictureAsset(app, instance, ticket)) {}
    bool Matches(MMDApp* app) const {
        return texture == app->PictureBackgroundTexture() && path == app->PictureBackgroundPath() &&
            width == app->PictureWidth() && height == app->PictureHeight() && x == app->PictureOffsetX() &&
            y == app->PictureOffsetY() && !std::memcmp(&scale, &app->PictureScale(), sizeof(scale)) &&
            enabled == app->PictureBackgroundEnabled() && dirty == app->SceneModified() &&
            frame == app->CurrentFrame() && generation == app->SceneGeneration() && revision == app->SceneRevision() &&
            instance == identity::Instance(app) && ticket == identity::Acquire(app, identity::Kind::Picture) &&
            asset == identity::CurrentPictureAsset(app, instance, ticket) &&
            charged == media::GlobalPictureSourceBudget().Usage().used;
    }
};

// A real texture owns this marker through D3D's private-data IUnknown support.
// Destruction therefore observes actual candidate cleanup without retaining an
// extra texture reference or replacing any COM texture method.
unsigned destroyedMarkers = 0;
constexpr GUID markerGuid = {0x83b50921, 0xa315, 0x4b80, {0x98,0x32,0x48,0x32,0x23,0x07,0x54,0x91}};
class TextureMarker final : public IUnknown {
    ULONG refs_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, __uuidof(IUnknown))) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) { ++destroyedMarkers; delete this; }
        return remaining;
    }
};
enum class Injection { None, Failure, Scene, Placement };
d3dx::FnCreateTexInMemEx decoder = nullptr;
MMDApp* decodingApp = nullptr;
Asset decodingAsset;
Injection injection = Injection::None;
unsigned decodeCalls = 0;
HRESULT WINAPI Decode(IDirect3DDevice9* device, LPCVOID bytes, UINT size, UINT requestedWidth,
    UINT requestedHeight, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    DWORD filter, DWORD mipFilter, D3DCOLOR colorKey, void* info, void* palette, IDirect3DTexture9** texture) {
    ++decodeCalls;
    Check(decodingAsset && bytes == decodingAsset->Source()->Data() && size == decodingAsset->Source()->Size(),
          "activation decodes the retained asset bytes directly");
    const bool fallback = decodingAsset->DecodeMode() == media::PictureDecodeMode::Fallback512;
    Check(requestedWidth == (fallback ? 512u : 1024u) && requestedHeight == requestedWidth &&
          levels == (fallback ? 1u : 0u) && pool == D3DPOOL_MANAGED,
          "activation uses only the original asset's recorded decode recipe");
    if (injection == Injection::Failure) return E_FAIL;
    const HRESULT result = decoder(device, bytes, size, requestedWidth, requestedHeight, levels, usage,
        format, pool, filter, mipFilter, colorKey, info, palette, texture);
    if (SUCCEEDED(result) && texture && *texture) {
        auto* marker = new TextureMarker;
        const HRESULT attached = (*texture)->SetPrivateData(markerGuid, marker, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
        marker->Release();
        Check(SUCCEEDED(attached), "attach cleanup observer to real prepared texture");
    }
    if (injection == Injection::Scene) decodingApp->AdvanceSceneGeneration();
    if (injection == Injection::Placement) decodingApp->PictureOffsetX() += 9;
    return result;
}
class DecoderScope {
    bool active_ = true;
public:
    DecoderScope(MMDApp* app, Asset asset, Injection fault = Injection::None) {
        decoder = __imp_D3DXCreateTextureFromFileInMemoryEx;
        decodingApp = app; decodingAsset = std::move(asset); injection = fault; decodeCalls = 0;
        WriteImport(&Decode);
    }
    void Stop() {
        if (!active_) return;
        WriteImport(decoder); d3dx::Get().Load();
        decoder = nullptr; decodingApp = nullptr; decodingAsset.reset(); injection = Injection::None;
        active_ = false;
    }
    ~DecoderScope() { Stop(); }
    DecoderScope(const DecoderScope&) = delete;
    DecoderScope& operator=(const DecoderScope&) = delete;
};

std::array<unsigned char, 168> Vertices(MMDApp* app) {
    std::array<unsigned char, 168> result{}; void* data = nullptr;
    auto* buffer = app->PictureOverlayVertices();
    Check(buffer && SUCCEEDED(buffer->Lock(0, 168, &data, D3DLOCK_READONLY)) && data,
          "inspect real overlay geometry without updating it");
    std::memcpy(result.data(), data, result.size());
    Check(SUCCEEDED(buffer->Unlock()), "unlock observed overlay geometry");
    return result;
}

void Run(MMDApp* app, IDirect3D9* d3d, const MediaFiles& files) {
    const auto sourcePath = files.sourceBitmap + L"_activation.bmp";
    WriteBitmap(sourcePath, 0);
    Check(TryReplaceBackgroundPicture(app, sourcePath.c_str()), "import actual primary activation asset");
    const auto instance = identity::Instance(app);
    const auto primary = identity::CurrentPictureAsset(app, instance, identity::Acquire(app, identity::Kind::Picture));
    Check(primary && primary->DecodeMode() == media::PictureDecodeMode::Primary1024, "capture primary recipe from actual importer");
    fallbackCalls = 0; fallbackBytes = nullptr; fallbackSize = 0;
    fallbackDecoder = __imp_D3DXCreateTextureFromFileInMemoryEx;
    WriteImport(&ForcePictureFallback);
    const bool loaded = TryReplaceBackgroundPicture(app, sourcePath.c_str());
    WriteImport(fallbackDecoder); d3dx::Get().Load(); fallbackDecoder = nullptr;
    const auto fallback = identity::CurrentPictureAsset(app, instance, identity::Acquire(app, identity::Kind::Picture));
    Check(loaded && fallbackCalls == 2 && fallback && fallback->DecodeMode() == media::PictureDecodeMode::Fallback512,
          "capture fallback recipe from real second-attempt decoding");
    Check(DeleteFileW(sourcePath.c_str()) != FALSE && GetFileAttributesW(sourcePath.c_str()) == INVALID_FILE_ATTRIBUTES,
          "remove activation fixture before every retained-asset restoration");

    const media::PicturePresentation presentation{20, 15, .5f, true};
    bool previousTextureMarked = false;
    for (const auto& asset : {primary, fallback}) {
        for (bool visible : {true, false}) {
            Check(SUCCEEDED(app->Renderer()->device->SetTexture(0, nullptr)),
                  "remove prior rendering's texture binding before observing retired ownership");
            DecoderScope scope(app, asset);
            const Snapshot before(app); const auto vertices = Vertices(app);
            std::unique_ptr<Activation> prepared;
            auto desired = presentation; desired.visible = visible;
            Check(Activation::Prepare(app, asset, desired, UINT64_MAX, prepared) == Result::Ok && prepared &&
                  prepared->IsCurrent() && before.Matches(app) && Vertices(app) == vertices && decodeCalls == 1,
                  "successful private preparation preserves every current native field and activation");
            const std::uint64_t resourceBound = asset == fallback ? 512ull * 512 * 32 : 44739232ull;
            Check(prepared->ResourceBytes() == resourceBound && prepared->PreparedBytes() > resourceBound &&
                  prepared->PreparedBytes() >= asset->RetainedBytes(), "prepared accounting includes conservative texture and retained-source payloads");
            auto* occupied = prepared.get();
            Check(Activation::Prepare(app, asset, desired, UINT64_MAX, prepared) == Result::Invalid &&
                  prepared.get() == occupied && decodeCalls == 1 && before.Matches(app),
                  "occupied output refuses preparation without losing the existing candidate or decoding twice");
            const auto destroyedBeforePublish = destroyedMarkers;
            Check(prepared->Publish() && !prepared->IsCurrent() && !prepared->Publish() && decodeCalls == 1 &&
                  destroyedMarkers == destroyedBeforePublish,
                  "prepared publication is single-use and performs no extra image decoding");
            const auto ticket = identity::Acquire(app, identity::Kind::Picture);
            Check(ticket && ticket != before.ticket && !identity::IsCurrent(app, identity::Kind::Picture, before.ticket) &&
                  identity::CurrentPictureAsset(app, instance, ticket) == asset && app->PictureBackgroundTexture() &&
                  app->PictureBackgroundTexture() != before.texture && app->PictureWidth() == width && app->PictureHeight() == height &&
                  std::wstring(app->PictureBackgroundPath()) == sourcePath && app->PictureOffsetX() == 20 && app->PictureOffsetY() == 15 &&
                  app->PictureScale() == .5f && app->PictureBackgroundEnabled() == unsigned(visible),
                  "publication activates the same retained logical asset with the requested presentation and a fresh ticket");
            Check(app->SceneModified() == before.dirty && app->SceneRevision() == before.revision && app->CurrentFrame() == before.frame &&
                  app->SceneGeneration() == before.generation && Vertices(app) == vertices,
                  "publication does not dirty the project move time or refresh overlay geometry");
            D3DSURFACE_DESC desc{};
            Check(SUCCEEDED(app->PictureBackgroundTexture()->GetLevelDesc(0, &desc)) &&
                  desc.Width == (asset == fallback ? 512u : 1024u), "published texture matches the retained recipe");
            prepared.reset();
            if (previousTextureMarked)
                Check(destroyedMarkers == destroyedBeforePublish + 1,
                      "retired texture stays alive through publication and is released only when the prepared owner is destroyed");
            previousTextureMarked = true;
            scope.Stop();
            CheckSourcePixels(app->Renderer()->device, asset->Source(), 0);
            // Refresh is explicitly outside Publish. Rendering remains real.
            PicBgOverlayRefresh(app);
            if (visible) CheckPixels(app, 0, 27, 24, 108, 73);
        }
    }
    {
        // The current asset is fallback. A second logical asset intentionally
        // shares its immutable source; only metadata should be charged twice.
        Asset alias;
        Check(media::CreateBackgroundPictureAsset(fallback->Source(), fallback->Path(), width, height,
              fallback->DecodeMode(), UINT64_MAX, alias) == media::PictureAssetResult::Ok && alias && alias != fallback &&
              alias->Id() != fallback->Id() && alias->Source() == fallback->Source(),
              "create independent logical asset over the same retained source for accounting boundaries");
        std::uint64_t baselineBytes = 0;
        {
            DecoderScope scope(app, fallback); std::unique_ptr<Activation> same;
            Check(Activation::Prepare(app, fallback, presentation, UINT64_MAX, same) == Result::Ok,
                  "measure preparation with the same current logical asset");
            baselineBytes = same->PreparedBytes();
        }
        const auto expectedBytes = baselineBytes + media::BackgroundPictureAsset::OverheadBytes();
        DecoderScope scope(app, alias); const Snapshot before(app); std::unique_ptr<Activation> prepared;
        Check(Activation::Prepare(app, alias, presentation, expectedBytes - 1, prepared) == Result::TooLarge && !prepared &&
              before.Matches(app) && decodeCalls == 0, "shared-source preparation fails one byte below its exact retained accounting boundary");
        Check(Activation::Prepare(app, alias, presentation, expectedBytes, prepared) == Result::Ok && prepared &&
              prepared->PreparedBytes() == expectedBytes && before.Matches(app) && decodeCalls == 1,
              "shared source is charged once while distinct asset metadata is counted at the exact accepted boundary");
    }
    {
        DecoderScope scope(app, primary);
        const Snapshot before(app); std::unique_ptr<Activation> prepared;
        Check(Activation::Prepare(app, primary, presentation, 0, prepared) == Result::TooLarge && !prepared &&
              before.Matches(app) && decodeCalls == 0, "zero retained-resource bound rejects before decoding without state changes");
        const auto destroyed = destroyedMarkers;
        Check(Activation::Prepare(app, primary, presentation, UINT64_MAX, prepared) == Result::Ok && prepared,
              "prepare a real candidate solely to discard it");
        prepared.reset();
        Check(destroyedMarkers == destroyed + 1 && before.Matches(app),
              "discarding a prepared candidate releases its actual private texture without changing the host");
    }
    {
        DecoderScope scope(app, primary, Injection::Failure);
        const Snapshot before(app); std::unique_ptr<Activation> prepared;
        Check(Activation::Prepare(app, primary, presentation, UINT64_MAX, prepared) == Result::DecodeFailed && !prepared &&
              before.Matches(app) && decodeCalls == 1, "decode failure preserves current picture and does not retry a different recipe or disk path");
    }
    for (const auto fault : {Injection::Scene, Injection::Placement}) {
        DecoderScope scope(app, primary, fault);
        const Snapshot before(app); const auto destroyed = destroyedMarkers;
        std::unique_ptr<Activation> prepared;
        Check(Activation::Prepare(app, primary, presentation, UINT64_MAX, prepared) == Result::Stale && !prepared &&
              decodeCalls == 1 && destroyedMarkers == destroyed + 1,
              "decoder reentry retires the privately decoded stale texture before returning");
        Check(app->PictureBackgroundTexture() == before.texture &&
              identity::CurrentPictureAsset(app, instance, before.ticket) == before.asset &&
              app->SceneModified() == before.dirty && app->SceneRevision() == before.revision &&
              app->PictureOffsetX() == before.x + (fault == Injection::Placement ? 9 : 0) &&
              app->SceneGeneration() == before.generation + (fault == Injection::Scene ? 1 : 0),
              "failed reentrant preparation preserves the newer scene or placement rather than restoring its snapshot");
    }
    for (unsigned change = 0; change != 5; ++change) {
        DecoderScope scope(app, primary);
        std::unique_ptr<Activation> prepared;
        Check(Activation::Prepare(app, primary, presentation, UINT64_MAX, prepared) == Result::Ok,
              "prepare a candidate before a real host-state change");
        IDirect3DDevice9* replacement = nullptr;
        auto* originalDevice = app->Renderer()->device;
        if (change == 0) app->AdvanceSceneGeneration();
        if (change == 1) app->PictureOffsetX() += 3;
        if (change == 2) app->PictureBackgroundEnabled() ^= 1;
        if (change == 3) {
            auto parameters = app->Renderer()->presentParameters;
            Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, static_cast<HWND>(app->Hwnd()), D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                  &parameters, &replacement)) && replacement, "create a separate real device for stale-device validation");
            app->Renderer()->device = replacement;
        }
        if (change == 4) { identity::Mutation mutation(app, true, false, false, true); }
        const Snapshot current(app); const auto destroyed = destroyedMarkers;
        Check(!prepared->IsCurrent() && !prepared->Publish() && current.Matches(app),
              "scene placement visibility device or media mutation rejects a stale publication and preserves the new state");
        prepared.reset();
        Check(destroyedMarkers == destroyed + 1 && current.Matches(app), "stale candidate destruction only releases its private resources");
        if (replacement) { app->Renderer()->device = originalDevice; replacement->Release(); }
    }
    {
        DecoderScope scope(app, {});
        const Snapshot before(app); std::unique_ptr<Activation> empty;
        Check(Activation::Prepare(app, {}, {73, -41, 9.f, true}, UINT64_MAX, empty) == Result::Ok &&
              empty && empty->ResourceBytes() == 0 && before.Matches(app) && decodeCalls == 0,
              "empty-picture preparation ignores presentation and performs no decode or host mutation");
        Check(empty->Publish() && !empty->Publish() && !app->PictureBackgroundTexture() &&
              !app->PictureBackgroundPath()[0] && !app->PictureWidth() && !app->PictureHeight() &&
              !app->PictureOffsetX() && !app->PictureOffsetY() && app->PictureScale() == 1.f && !app->PictureBackgroundEnabled() &&
              !identity::CurrentPictureAsset(app, instance, identity::Acquire(app, identity::Kind::Picture)) &&
              app->SceneModified() == before.dirty && app->SceneRevision() == before.revision && app->CurrentFrame() == before.frame,
              "empty publication installs canonical empty fields once without altering editing state");
    }
    {
        DecoderScope scope(app, primary);
        std::unique_ptr<Activation> prepared;
        Check(Activation::Prepare(app, primary, presentation, UINT64_MAX, prepared) == Result::Ok,
              "prepare from the canonical empty picture before replacing its registered owner");
        // Forget retires media but deliberately keeps a live owner registered.
        // Address reuse is permitted only after the actual closing gate ends.
        { identity::Mutation closing(app, true, true, true); }
        Check(identity::Register(app) && identity::Instance(app) != instance,
              "register a fresh owner identity at the same real application address");
        const Snapshot replacementOwner(app); const auto destroyed = destroyedMarkers;
        Check(!prepared->IsCurrent() && !prepared->Publish() && replacementOwner.Matches(app),
              "an old candidate cannot publish into a replacement owner at the same address");
        prepared.reset();
        Check(destroyedMarkers == destroyed + 1 && replacementOwner.Matches(app),
              "owner replacement leaves stale private-texture cleanup independent of the current owner");
    }
    // Keep the existing suite's unconditional cleanup and subsequent AVI test
    // unchanged. This is a separate fixture, not the deleted restoration path.
    Check(TryReplaceBackgroundPicture(app, files.bitmap.c_str()), "restore the ordinary overlay fixture after activation-specific checks");
    Check(GetFileAttributesW(sourcePath.c_str()) == INVALID_FILE_ATTRIBUTES,
          "no activation operation recreated or depended on the deleted source file");
    std::puts("PASS prepared picture activation: retained bytes recipes atomic failure stale candidates and cleanup");
}
} // namespace activation_checks
#endif

void CheckPictureActivation(MMDApp* app, IDirect3D9* d3d, const MediaFiles& files) {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS)
    activation_checks::Run(app, d3d, files);
#else
    (void)app; (void)d3d; (void)files;
    Check(false, "activation-specific decoder fault checks require the static-import verification target");
#endif
}
