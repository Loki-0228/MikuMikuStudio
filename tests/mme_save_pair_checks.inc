// Windows real-MME SAVE-02 acceptance. Included only by the opt-in scene host.
namespace mme_save_pair_checks {
namespace fs = std::filesystem;

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(bool(file), "read an owned fixture file");
    return {std::istreambuf_iterator<char>(file), {}};
}

void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    file.close();
    Check(bool(file), "write an owned fixture file");
}

fs::path Sidecar(const fs::path& path) {
    return path.native() + pmm_paths::PluginSidecarSuffix;
}

std::vector<DWORD> Draw(MMDApp* app, RECT* area = nullptr) {
    app->CameraMode() = 1;
    app->ViewOffsetY() = 10;
    app->CameraDistance() = -45;
    app->CameraFov() = 30;
    app->CameraPerspective() = 0;
    app->GroundGridEnabled() = 0;
    app->state.messageSeen = 10;
    FrameDriver(app);
    FrameSceneOutputScope output(app);
    Check(output.Ready(), "hidden production D3D9 output is ready");
    for (int i = 0; i < 4; ++i) {
        app->state.messageSeen = 10;
        RenderFrameScene(app);
    }
    if (!area) return {};
    Microsoft::WRL::ComPtr<IDirect3DSurface9> backbuffer;
    D3DSURFACE_DESC size{};
    Check(SUCCEEDED(app->Renderer()->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
          SUCCEEDED(backbuffer->GetDesc(&size)), "read the rendered backbuffer dimensions");
    const RECT available{0, 0, static_cast<LONG>(size.Width), static_cast<LONG>(size.Height)};
    const auto viewport = app->ViewportRect();
    Check(IntersectRect(area, &viewport, &available) != FALSE, "read pixels only inside the rendered viewport");
    InflateRect(area, -(area->right - area->left) / 4, -(area->bottom - area->top) / 4);
    return ReadGizmoPixels(app->Renderer()->device, *area);
}

void Render(MMDApp* app) {
    Draw(app);
}

std::size_t RenderGreen(MMDApp* app, const fs::path& image = {}) {
    RECT area{};
    const auto pixels = Draw(app, &area);
    const auto green = std::count_if(pixels.begin(), pixels.end(), [](DWORD pixel) {
        return ((pixel >> 8) & 0xffu) > 224 && ((pixel >> 16) & 0xffu) < 25 && (pixel & 0xffu) < 25;
    });
    if (!image.empty()) {
        std::string bytes = "P6\n" + std::to_string(area.right - area.left) + " " +
            std::to_string(area.bottom - area.top) + "\n255\n";
        bytes.reserve(bytes.size() + pixels.size() * 3);
        for (auto pixel : pixels) {
            bytes.push_back(static_cast<char>((pixel >> 16) & 0xffu));
            bytes.push_back(static_cast<char>((pixel >> 8) & 0xffu));
            bytes.push_back(static_cast<char>(pixel & 0xffu));
        }
        Write(image, bytes);
    }
    return static_cast<std::size_t>(green);
}

void Camera(MMDApp* app, int fov) {
    app->CurrentFrame() = 0;
    app->CameraFov() = static_cast<float>(fov);
    RegisterCameraState(app, 0);
    app->SceneModified() = 1;
    Check(app->CameraKeys()[0].fov == fov, "register the exact test FOV at frame zero");
}

void LoadNextWithoutRender(MMDApp* app, const fs::path& path) {
    // Reject an unexpected render during the post-completion edit instead of
    // letting the test accidentally consume the legacy notification first.
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize the process-local render observer");
    auto* target = reinterpret_cast<void*>(&BeginEffectObjectRegistration);
    const auto failIfRendered = +[] { Check(false, "a new effect render entered during the post-completion edit"); };
    Check(MH_CreateHook(target, reinterpret_cast<void*>(failIfRendered), nullptr) == MH_OK &&
          MH_EnableHook(target) == MH_OK, "guard the no-render edit boundary");
    LoadModelFile(app, path.c_str(), false);
    Camera(app, 81);
    Check(MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK, "remove the render observer before drawing");
}

void MmeMenuSwitch(MMDApp* app, UINT command, bool enabled) {
    const auto read = [&] {
        const UINT state = GetMenuState(GetMenu(app->MainWindow()), command, MF_BYCOMMAND);
        Check(state != UINT(-1), "real MME installed its menu command");
        return (state & MF_CHECKED) != 0;
    };
    if (read() != enabled) SendMessageW(app->MainWindow(), WM_COMMAND, command, 0);
    Check(read() == enabled, "real MME changed its own menu state");
}

void OpenAndRender(MMDApp* app, const fs::path& project, const fs::path& model, int fov,
                   const fs::path& image = {}) {
    Check(LoadSceneFile(project.c_str()) && app->ModelSlot(0) && !app->ModelSlot(1) &&
          fs::path(mdl::Mdl(app->ModelSlot(0))->path) == model && app->CameraKeys()[0].fov == fov,
          "reopen the exact captured model count, model path and FOV via the native PMM loader");
    constexpr UINT effects = 40006;
    const auto green = RenderGreen(app, image);
    MmeMenuSwitch(app, effects, false);
    auto controlImage = image;
    if (!image.empty()) controlImage.replace_filename(image.stem().wstring() + L"-effect-off.ppm");
    const auto control = RenderGreen(app, controlImage);
    MmeMenuSwitch(app, effects, true);
    const auto restored = RenderGreen(app);
    std::printf("MME_SAVE_PAIR reopened=%ls model=%ls fov=%d green=%zu control=%zu restored=%zu\n",
                project.filename().c_str(), model.filename().c_str(), fov, green, control, restored);
    Check(green > control + 100 && restored > control + 100,
          "the assigned effect changes real D3D9 pixels and restores after the provider toggle");
}

// Trap only the backup worker's exact lock-file open in this disposable host.
// Timer capture has finished on the UI thread before the worker reaches here.
struct TimerGate {
    static TimerGate* active;
    decltype(&CreateFileW) original = nullptr;
    void* target = nullptr;
    const std::wstring lockPath;
    const DWORD uiThread = GetCurrentThreadId();
    HANDLE reached = nullptr, release = nullptr;
    static HANDLE WINAPI Create(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                DWORD disposition, DWORD attributes, HANDLE templateFile) {
        const DWORD incoming = GetLastError();
        const auto& gate = *active;
        if (GetCurrentThreadId() != gate.uiThread && name && _wcsicmp(name, gate.lockPath.c_str()) == 0 &&
            access == (GENERIC_READ | GENERIC_WRITE) && share == 0 && disposition == OPEN_ALWAYS &&
            (attributes & FILE_FLAG_DELETE_ON_CLOSE)) {
            Check(SetEvent(gate.reached) && WaitForSingleObject(gate.release, 15000) == WAIT_OBJECT_0,
                  "release the exact worker after the post-capture edit");
        }
        SetLastError(incoming);
        return gate.original(name, access, share, security, disposition, attributes, templateFile);
    }
    explicit TimerGate(const fs::path& path) : lockPath(path.native()) {
        Check(!active, "one timer barrier at a time");
        reached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(reached && release, "create disposable backup barrier events");
        const auto mh = MH_Initialize();
        Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize the process-local timer observer");
        target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
        active = this;
        Check(target && MH_CreateHook(target, reinterpret_cast<void*>(&Create),
                                      reinterpret_cast<void**>(&original)) == MH_OK &&
              MH_EnableHook(target) == MH_OK, "attach the process-local backup lock observer");
    }
    TimerGate(const TimerGate&) = delete;
    TimerGate& operator=(const TimerGate&) = delete;
    ~TimerGate() {
        Check(MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK,
              "detach the timer observer after joining the worker");
        active = nullptr;
        Check(CloseHandle(reached) && CloseHandle(release), "close the two backup barrier events");
    }
};
TimerGate* TimerGate::active = nullptr;

void Run(MMDApp* app, const fs::path& root, std::wstring_view phase, std::wstring_view mode) {
    const bool save = phase == L"save";
    const bool overlap = mode == L"overlap";
    const bool timed = overlap || mode == L"timer";
    Check((save || phase == L"reopen") && (timed || mode == L"incremental"), "use a known save-pair phase and case");
    const auto& legacy = plugins::LegacyMmeStatus();
    if (!legacy.error.empty()) std::fprintf(stderr, "legacy startup: %s\n", legacy.error.c_str());
    Check(legacy.loaded && GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll") &&
          !GetModuleHandleW(L"MikuMikuDance.exe"), "load real packaged MME through a renamed host");
    Check(!IsWindowVisible(app->MainWindow()), "the real host stays hidden");
    std::printf("MME_SAVE_PAIR profile=%s\n", MMD_ORIGINAL_PROFILE ? "mmd932" : "full");
    app->EnglishUI() = kUiEnglish;
    LocalizeUI(app);
    const auto projects = root / L"Projects";
    const auto backups = root / L"backups";
    const auto folder = timed ? backups : projects;
    const auto source = projects / L"scene.pmm";
    const auto good = folder / (timed ? L"timer_002.pmm" : L"scene_002.pmm");
    const auto target = overlap ? folder / L"timer_003.pmm" : good;
    const auto orphan = folder / (timed ? L"timer_001.emm" : L"scene_001.emm");
    const auto model = projects / L"first.pmx";
    const auto second = projects / L"second.pmx";
    const auto third = projects / L"third.pmx";
    const auto fx = projects / L"green.fx";
    const auto sourceEmm = pmm_paths::Effect(source), targetEmm = pmm_paths::Effect(target);
    const auto lockPath = backups / L".mms-autosave.lock";
    const auto recent = root / L"recents.txt";
    const auto members = [](const fs::path& path) {
        std::vector<fs::path> result{path, pmm_paths::Effect(path)};
        if (!MMD_ORIGINAL_PROFILE) result.push_back(Sidecar(path));
        return result;
    };
    const auto absent = [&](const fs::path& path) {
        const auto paths = members(path);
        return std::none_of(paths.begin(), paths.end(), [](const auto& part) { return fs::exists(part); });
    };
    const auto verifyOrphan = [&] {
        auto pmm = orphan;
        pmm.replace_extension(L".pmm");
        Check(Read(orphan) == "external orphan EMM\n" && !fs::exists(pmm) && !fs::exists(Sidecar(pmm)),
              "the unrelated EMM reserves its name and is never changed");
    };
    Check(fs::is_regular_file(model) && fs::is_regular_file(second) && fs::is_regular_file(third) &&
          fs::is_regular_file(fx) && fs::is_directory(backups), "use only the runner's disposable assets");
    if (!save) {
        verifyOrphan();
        OpenAndRender(app, source, model, 31);
        if (overlap) OpenAndRender(app, good, model, 51);
        if (overlap && absent(target)) {
            std::puts("MME_SAVE_PAIR outcome=absent");
            return;
        }
        OpenAndRender(app, target, model, 61, root / L"reopened.ppm");
        Check(fs::is_regular_file(targetEmm) && fs::exists(Sidecar(target)) == !MMD_ORIGINAL_PROFILE,
              "the reopened PMM has the expected EMM and profile-specific members");
        std::puts("MME_SAVE_PAIR outcome=pair");
        return;
    }

    Check(absent(source) && absent(good) && absent(target) && !fs::exists(orphan),
          "start with no preexisting project, pair, or orphan in the disposable case");
    auto settings = autosave::Current();
    settings.enabled = timed;
    settings.keepPrevious = false;
    settings.numberedManualVersions = false;
    settings.overwriteOldest = false;
    settings.maxBackups = 100;
    settings.intervalMinutes = 1;
    settings.directory = backups.wstring();
    settings.backupPattern = L"timer_[/N]";
    settings.incrementPattern = L"[/NAME]_[/N]";
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "configure only the owned backup settings");
    LoadModelFile(app, model.c_str(), false);
    Check(app->ModelSlot(0) && !app->ModelSlot(1), "load exactly the first fixture model");
    Render(app);
    MmeMenuSwitch(app, 40022, false);
    Camera(app, 31);
    Check(wcscpy_s(app->EnvFileName(), 256, source.c_str()) == 0 && SaveSceneFile(app),
          "write the one-model baseline with the ordinary PMM writer");
    Render(app);
    Check(!fs::exists(sourceEmm), "consume the baseline notification while EMM autosave is off");
    Write(sourceEmm, "[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = " + model.string() +
          "\r\n[Effect]\r\nDefault = none\r\nPmd1 = " + fx.string() + "\r\nPmd1.show = true\r\n");
    Write(orphan, "external orphan EMM\n");
    OpenAndRender(app, source, model, 31, root / L"baseline.ppm");
    MmeMenuSwitch(app, 40022, true);
    std::vector<std::pair<fs::path, std::string>> preserve;
    for (const auto& path : members(source)) preserve.emplace_back(path, Read(path));

    if (!timed) {
        settings.numberedManualVersions = true;
        Check(autosave::Configure(app, settings, error), "enable numbered saves through the production settings");
        Camera(app, 61);
        SendMessageW(app->MainWindow(), WM_COMMAND, 207, 0);
        Check(SceneSaveInProgress(), "the actual Save menu queues the numbered worker");
        background_save::Wait(app);
        Check(!SceneSaveInProgress() && fs::path(app->EnvFileName()) == good && !app->SceneModified(),
              "normal numbered save publishes number two and clears the unchanged scene");
        Render(app);
    } else {
        const auto generation = app->SceneGeneration();
        const bool hasRecent = fs::exists(recent);
        Check(hasRecent == !MMD_ORIGINAL_PROFILE, "the recents check observes this profile's real isolated store");
        const auto recentBytes = hasRecent ? Read(recent) : std::string{};
        const auto live = [&](int fov, bool hasSecond = false, bool hasThird = false) {
            Check(app->SceneGeneration() == generation && fs::path(app->EnvFileName()) == source &&
                  app->SceneModified() && app->CameraKeys()[0].fov == fov &&
                  app->ModelSlot(0) && fs::path(mdl::Mdl(app->ModelSlot(0))->path) == model &&
                  (app->ModelSlot(1) != nullptr) == hasSecond && (app->ModelSlot(2) != nullptr) == hasThird &&
                  (!hasSecond || fs::path(mdl::Mdl(app->ModelSlot(1))->path) == second) &&
                  (!hasThird || fs::path(mdl::Mdl(app->ModelSlot(2))->path) == third) &&
                  fs::exists(recent) == hasRecent && (!hasRecent || Read(recent) == recentBytes),
                  "timer backup preserves the current project, unsaved model/FOV edits, dirty flag and recents");
        };
        Camera(app, overlap ? 51 : 61);
        autosave::Initialize(app);
        const auto start = GetTickCount64();
        autosave::Tick(app, start);
        Check(!SceneSaveInProgress() && absent(good), "timer does not save early");
        const HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        Check(lock != INVALID_HANDLE_VALUE, "hold the fixture's real backup lock");
        const auto due = start + 60000;
        autosave::Tick(app, due);
        Check(SceneSaveInProgress(), "the due production timer queues a worker despite the held lock");
        background_save::Wait(app);
        Check(!SceneSaveInProgress() && std::wstring_view(background_save::StatusText(app)) ==
              L"Backup failed. Check the destination.", "the lock conflict reports a backup failure");
        Render(app);
        live(overlap ? 51 : 61);
        Check(absent(good) && CloseHandle(lock) && !fs::exists(lockPath),
              "locked attempt publishes nothing; release only the owned lock");
        autosave::Tick(app, due + 60000);
        Check(SceneSaveInProgress(), "the next due timer retries through production");
        background_save::Wait(app);
        Check(!SceneSaveInProgress() && std::wstring_view(background_save::StatusText(app)) == L"Backup saved",
              "the retry publishes the previous complete backup");
        Render(app);
        live(overlap ? 51 : 61);
        for (const auto& path : members(good)) {
            Check(fs::is_regular_file(path), "normal timer retry produced every expected member");
            if (overlap) preserve.emplace_back(path, Read(path));
        }

        if (overlap) {
            Camera(app, 61);
            const auto revision = app->SceneRevision();
            {
                TimerGate gate(lockPath);
                autosave::Tick(app, due + 120000);
                Check(SceneSaveInProgress() && WaitForSingleObject(gate.reached, 10000) == WAIT_OBJECT_0 &&
                      absent(target), "capture the one-model FOV-61 scene and park its real timer worker");
                LoadModelFile(app, second.c_str(), false);
                Camera(app, 71);
                Render(app); // Register the newer live object with the real provider before completion.
                Check(app->SceneRevision() != revision && SceneSaveInProgress() && absent(target),
                      "model/FOV edit has reached real MME before the captured PMM can publish");
                live(71, true);
                Check(SetEvent(gate.release), "unpark the real backup worker");
                background_save::Wait(app);
            }
            Check(!SceneSaveInProgress(), "join the worker and handle completion before the next edit");
            const std::wstring completion = background_save::StatusText(app);
            const bool published = completion == L"Backup saved";
            const bool refused = completion == L"Backup failed. Check the destination.";
            std::printf("MME_SAVE_PAIR completion=%s pmm=%d emm=%d sidecar=%d\n",
                        published ? "saved" : refused ? "refused" : "other",
                        int(fs::exists(target)), int(fs::exists(targetEmm)), int(fs::exists(Sidecar(target))));
            Check(published || refused, "the backup reports saved or a definite refusal");
            const bool emmAtCompletion = fs::exists(targetEmm);
            std::vector<std::pair<fs::path, std::string>> completedMembers;
            for (const auto& member : members(target)) {
                if (fs::is_regular_file(member)) completedMembers.emplace_back(member, Read(member));
            }
            LoadNextWithoutRender(app, third);
            Check(fs::exists(targetEmm) == emmAtCompletion,
                  "the post-completion edit occurs before the next provider render");
            live(81, true, true);
            Render(app);
            live(81, true, true);
            for (const auto& [path, bytes] : completedMembers)
                Check(Read(path) == bytes, "the later live edit/render cannot rewrite a completed member");
            if (refused) Check(absent(target), "a refused overlap publishes no PMM, EMM, or sidecar");
        }
        autosave::Shutdown();
    }
    for (const auto& [path, bytes] : preserve)
        Check(Read(path) == bytes, "the original project and prior good generation stay byte-identical");
    verifyOrphan();
    Check(!IsWindowVisible(app->MainWindow()), "the test never shows its window");
    std::puts("MME_SAVE_PAIR write-phase-complete");
}
} // namespace mme_save_pair_checks
