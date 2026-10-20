// Decode files produced by both public export entries, with real geometry.
std::filesystem::path visibilityImage;
unsigned invisibleImages = 0;
struct ImageVisibilityOptions {
    int selfShadow = -1;
    float targetY = 12.f;
    bool hasTargetY = false;
};
const ImageVisibilityOptions& VisibilityOptions() {
    static const auto options = [] {
        ImageVisibilityOptions value;
        if (const auto* text = std::getenv("MMS_IMAGE_SELF_SHADOW")) {
            char* end = nullptr;
            const long mode = std::strtol(text, &end, 10);
            Check(end != text && !*end && mode >= 0 && mode <= 2, "MMS_IMAGE_SELF_SHADOW is 0, 1 or 2");
            value.selfShadow = static_cast<int>(mode);
        }
        if (const auto* text = std::getenv("MMS_IMAGE_TARGET_Y")) {
            char* end = nullptr;
            const float target = std::strtof(text, &end);
            Check(end != text && !*end && std::isfinite(target), "MMS_IMAGE_TARGET_Y is a finite float");
            value.targetY = target;
            value.hasTargetY = true;
        }
        return value;
    }();
    return options;
}
void ApplyImageVisibilitySelfShadow(MMDApp* app) {
    if (VisibilityOptions().selfShadow >= 0) app->SelfShadowMode() = VisibilityOptions().selfShadow;
}
void CheckImageVisibilitySelfShadow(MMDApp* app) {
    if (VisibilityOptions().selfShadow >= 0)
        Check(app->SelfShadowMode() == VisibilityOptions().selfShadow, "image export retains requested self-shadow mode");
}
void PrepareImageVisibilityScene(MMDApp* app) {
    ApplyImageVisibilitySelfShadow(app);
    if (VisibilityOptions().selfShadow >= 0) RegisterSelfShadowState(app, 0);
    if (VisibilityOptions().hasTargetY) app->CameraPositionY() = VisibilityOptions().targetY;
}
BOOL WINAPI SelectVisibilityImage(LPOPENFILENAMEW dialog) {
    wcscpy_s(dialog->lpstrFile, dialog->nMaxFile, visibilityImage.c_str());
    return TRUE;
}
void CheckVisibleImage(const std::filesystem::path& path, bool alpha) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))), "create independent visibility decoder");
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)), "open the exported image");
    Check(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "decode exported RGBA pixels");
    UINT width = 0, height = 0; converter->GetSize(&width, &height);
    std::vector<unsigned char> pixels(std::size_t(width) * height * 4);
    Check(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data())), "read exported RGBA pixels");
    unsigned visible = 0, clear = 0, colour = 0, geometry = 0, magenta = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        visible += pixels[i+3] != 0;
        clear += pixels[i+3] == 0;
        colour += pixels[i+3] && (pixels[i] != pixels[i+1] || pixels[i+1] != pixels[i+2]);
        geometry += pixels[i+3] && (pixels[i] != pixels[0] || pixels[i+1] != pixels[1] || pixels[i+2] != pixels[2]);
        magenta += pixels[i+3] && pixels[i] > 180 && pixels[i+1] < 80 && pixels[i+2] > 180;
    }
    std::printf("IMAGE visible=%u clear=%u colour=%u geometry=%u size=%ux%u file=%ls\n",
        visible, clear, colour, geometry, width, height, path.filename().c_str());
    if (visible <= 8 || (!alpha && geometry <= 8)) ++invisibleImages;
    if (alpha) Check(clear > 8, "alpha export also retains an empty background");
    else Check(clear == 0, "opaque format does not acquire transparent pixels");
    if (std::getenv("MMS_IMAGE_EXPECT_MAGENTA")) Check(magenta > 8, "export contains the actual assigned MME effect");
}
void CheckImageVisibility(MMDApp* app, const std::filesystem::path& directory) {
    namespace sequence = mikumikustudio::image_sequence;
    Check(app->SelectedModel() != nullptr, "image visibility test uses an actual model");
    app->GroundGridEnabled() = app->GroundShadowEnabled() = false;
    app->CameraPositionX() = 0; app->CameraPositionY() = VisibilityOptions().targetY; app->CameraPositionZ() = 0;
    app->CameraDistance() = -45; app->CameraYaw() = app->CameraPitch() = app->CameraRoll() = 0;
    app->CameraFov() = 30; app->CameraPerspective() = 0;
    PrepareImageVisibilityScene(app);
    RegisterCameraState(app, 0);
    std::printf("IMAGE options self_shadow=%d target_y=%g explicit_shadow=%d\n",
        app->SelfShadowMode(), app->CameraPositionY(), VisibilityOptions().selfShadow >= 0);
    app->RenderWidth() = 320; app->RenderHeight() = 240;
    if (const auto* aa = _wgetenv(L"MMS_IMAGE_AA")) {
        std::wstring error;
        Check(antialias::Select(app, static_cast<antialias::Algorithm>(_wtoi(aa)), error), "select image-export AA mode");
    }
    ApplyImageVisibilitySelfShadow(app);
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetSaveFileNameW), &SelectVisibilityImage, nullptr) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&GetSaveFileNameW)) == MH_OK, "select disposable still-image outputs");
    for (int warm : {0, 1}) {
        if (warm) for (int i = 0; i < 3; ++i) {
            ApplyImageVisibilitySelfShadow(app);
            app->state.messageSeen = 1; app->DeltaTime() = 1.f / 30; FrameDriver(app);
            CheckImageVisibilitySelfShadow(app);
        }
        for (int format : {2, 0, 1}) {
            const auto name = std::to_wstring(warm) + L"-" + std::to_wstring(format);
            visibilityImage = directory / (L"still-" + name + sequence::kFormats[format].extension);
            ApplyImageVisibilitySelfShadow(app);
            SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
            ApplyImageVisibilitySelfShadow(app);
            for (int i = 0; app->RecordingWindow() && i < 300; ++i) {
                ApplyImageVisibilitySelfShadow(app);
                app->state.messageSeen = 1; FrameDriver(app);
                CheckImageVisibilitySelfShadow(app);
                Sleep(1);
            }
            Check(!app->RecordingWindow(), "still capture finishes");
            CheckVisibleImage(visibilityImage, sequence::SupportsAlpha(format));
            sequence::Options options; options.directory = directory.wstring(); options.prefix = L"sequence-" + name + L"-";
            options.width = 320; options.height = 240; options.start = 0; options.end = 1; options.format = format;
            std::wstring error;
            Check(sequence::Prepare(app, options, error), "prepare visible sequence");
            app->AviOutputWidth() = options.width; app->AviOutputHeight() = options.height;
            app->AviRecordStartFrame() = options.start; app->AviRecordEndFrame() = options.end;
            app->AviRecordFps() = 30; app->AviIncludeWave() = app->AviStereoOutput() = 0;
            app->AviCloseAfterExport() = false;
            ApplyImageVisibilitySelfShadow(app);
            StartAviRecordWindow(app);
            ApplyImageVisibilitySelfShadow(app);
            for (int i = 0; sequence::Active(app) && i < 30; ++i) {
                ApplyImageVisibilitySelfShadow(app);
                app->state.messageSeen = 1; FrameDriver(app);
                CheckImageVisibilitySelfShadow(app);
            }
            Check(!sequence::Active(app) && !app->RecordingWindow(), "visible sequence finishes");
            std::printf("IMAGE camera=%g,%g,%g distance=%g mode=%d model_display=%d\n",
                app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(), app->CameraDistance(),
                app->CameraMode(), mdl::Mdl(app->ModelSlot(0))->loadComplete);
            CheckVisibleImage(sequence::FramePath(options, 1), sequence::SupportsAlpha(format));
            CheckVisibleImage(sequence::FramePath(options, 2), sequence::SupportsAlpha(format));
        }
    }
    MH_DisableHook(reinterpret_cast<void*>(&GetSaveFileNameW));
    MH_RemoveHook(reinterpret_cast<void*>(&GetSaveFileNameW));
    Check(invisibleImages == 0, "all exports retain visible model geometry");
}
