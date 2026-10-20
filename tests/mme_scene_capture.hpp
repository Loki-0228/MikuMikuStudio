// Fixed-pose MME comparison capture. Included by the hidden production host.
// This path does not exercise the host's temporal-AA regression suite.
#include <stdexcept>
#include <iomanip>

namespace mme_scene_capture {
void Need(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int Option(const char* name, int fallback, int minimum, int maximum) {
    const auto* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    Need(end != value && !*end && parsed >= minimum && parsed <= maximum, name);
    return int(parsed);
}
void SavePixels(MMDApp* app, const std::filesystem::path& path, unsigned width, unsigned height) {
    auto* device = app->Renderer()->device;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> back, resolved, cpu;
    Need(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)), "MME capture backbuffer");
    D3DSURFACE_DESC desc{};
    Need(SUCCEEDED(back->GetDesc(&desc)) && desc.Width == width && desc.Height == height,
        "MME capture has the requested fixed dimensions");
    Need(desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8, "MME capture BGRA format");
    auto* source = back.Get();
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        Need(SUCCEEDED(device->CreateRenderTarget(width, height, desc.Format, D3DMULTISAMPLE_NONE,
            0, FALSE, &resolved, nullptr)), "MME capture resolve target");
        Need(SUCCEEDED(device->StretchRect(source, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)),
            "MME capture resolves multisampling");
        source = resolved.Get();
    }
    Need(SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, desc.Format,
        D3DPOOL_SYSTEMMEM, &cpu, nullptr)) && SUCCEEDED(device->GetRenderTargetData(source, cpu.Get())),
        "MME capture reads the rendered GPU image");
    D3DLOCKED_RECT lock{};
    Need(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "MME capture locks pixels");
    std::vector<std::uint32_t> pixels(std::size_t(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const unsigned char*>(lock.pBits) + y * lock.Pitch);
        for (unsigned x = 0; x < width; ++x)
            pixels[std::size_t(y) * width + x] = row[x] | 0xFF000000u;
    }
    cpu->UnlockRect();
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(pixels[0])));
    file.close();
    Need(bool(file), "MME capture writes all BGRA pixels");
    std::printf("MME_CAPTURE saved=%ls size=%ux%u\n", path.c_str(), width, height);
}
void Run(MMDApp* app, const wchar_t* project, const wchar_t* output) {
    const auto input = std::filesystem::absolute(project);
    const auto directory = std::filesystem::absolute(output);
    Need(input.extension() == L".pmm" && std::filesystem::is_regular_file(input), "MME capture requires an existing PMM");
    const int width = Option("MMS_MME_CAPTURE_WIDTH", 1280, 64, 8192);
    const int height = Option("MMS_MME_CAPTURE_HEIGHT", 720, 64, 8192);
    const int warmup = Option("MMS_MME_CAPTURE_WARMUP", 32, 0, 10000);
    const int frames = Option("MMS_MME_CAPTURE_FRAMES", 4, 1, 10000);
    std::filesystem::create_directories(directory);
    Need(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"), "MME DLLs are loaded");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, input.c_str());
    LoadSceneFile();
    if (const auto* effects = _wgetenv(L"MMS_MME_CAPTURE_EMM")) NotifyEffectFileOpen(effects);
    int models = 0, accessories = 0;
    std::set<const void*> modelPointers;
    for (int slot = 0; slot < kModelSlotCount; ++slot) if (const auto* model = app->ModelSlot(slot)) {
        Need(modelPointers.insert(model).second, "MME scene model ownership is distinct"); ++models;
    }
    for (int slot = 0; slot < 255; ++slot) accessories += app->AccessorySlot(slot) != nullptr;
    Need(models || accessories, "MME capture loaded scene objects");
    app->SetSelectedModelSlot(0);
    app->PlaybackActive() = 1;
    app->PlaybackPhysicsMode() = 0;
    app->CameraMode() = 1;
    app->EditMode() = ViewportEditMode::None;
    app->FpsOverlayEnabled() = 0;
    std::wstring error;
    Need(antialias::Select(app, antialias::Algorithm::MmdOriginal, error), "MME capture selects original host AA");
    if (!app->antialiasing.enabled) Need(antialias::Toggle(app, error), "MME capture enables original host AA");
    app->RenderWidth() = width; app->RenderHeight() = height;
    app->RecordingWindow() = CreateWindowW(L"STATIC", L"Hidden MME scene capture", WS_POPUP,
        0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Need(app->RecordingWindow() != nullptr, "MME capture creates hidden output window");
    auto* renderer = app->Renderer();
    renderer->screenWidth = width; renderer->screenHeight = height;
    renderer->presentParameters.BackBufferWidth = unsigned(width);
    renderer->presentParameters.BackBufferHeight = unsigned(height);
    Need(PostDeviceReset(app), "MME capture resets the device to fixed resolution");
    Need(!IsWindowVisible(app->MainWindow()) && !IsWindowVisible(app->RecordingWindow()), "MME capture windows stay hidden");
    const auto frameIndex = app->CurrentFrame();
    const float camera[] = {app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
        app->CameraDistance(), app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraFov()};
    std::printf("MME_CAPTURE begin models=%d accessories=%d size=%dx%d frame=%d warmup=%d captures=%d\n",
        models, accessories, width, height, frameIndex, warmup, frames);
    for (int submission = 0; submission < warmup + frames; ++submission) {
        // Render the saved pose directly. FrameDriver would advance playback,
        // recording, physics and camera keys while comparing temporal effects.
        app->CurrentFrame() = frameIndex;
        app->CameraPositionX() = camera[0]; app->CameraPositionY() = camera[1]; app->CameraPositionZ() = camera[2];
        app->CameraDistance() = camera[3]; app->CameraPitch() = camera[4]; app->CameraYaw() = camera[5];
        app->CameraRoll() = camera[6]; app->CameraFov() = camera[7];
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10;
        RenderFrameScene(app);
        if (submission >= warmup) {
            wchar_t name[40]{}; swprintf_s(name, L"frame-%04d.bgra", submission - warmup);
            SavePixels(app, directory / name, unsigned(width), unsigned(height));
        }
        Need(SUCCEEDED(renderer->device->Present(nullptr, nullptr, app->RecordingWindow(), nullptr)),
            "MME capture completes the hidden frame");
    }
    std::ofstream manifest(directory / L"capture.json");
    manifest << std::setprecision(9) << "{\n  \"width\": " << width << ",\n  \"height\": " << height
        << ",\n  \"warmup_submissions\": " << warmup << ",\n  \"captured_submissions\": " << frames
        << ",\n  \"pmm_frame\": " << frameIndex << ",\n  \"fps\": 30,\n  \"models\": " << models
        << ",\n  \"accessories\": " << accessories
        << ",\n  \"pixel_format\": \"BGRA8\",\n  \"alpha_mode\": \"opaque_backbuffer\""
        << ",\n  \"host_aa\": \"MmdOriginal\",\n  \"camera\": [";
    for (unsigned i = 0; i < 8; ++i) manifest << (i ? ", " : "") << camera[i];
    manifest << "]\n}\n"; manifest.close();
    Need(bool(manifest), "MME capture writes its manifest");
}
void Release(MMDApp* app) {
    antialias::Release(app);
    antialias::motion::Release(app);
    app->PlaybackActive() = 0;
    if (app->RecordingWindow()) {
        DestroyWindow(app->RecordingWindow()); app->RecordingWindow() = nullptr;
    }
    ResetAppState(app);
}
}
