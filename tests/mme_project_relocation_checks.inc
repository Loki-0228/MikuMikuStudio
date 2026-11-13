// Included by the isolated scene lifecycle host with packaged legacy MME.
namespace mme_project_relocation_checks {
namespace fs = std::filesystem;

fs::path selected;
fs::path wantedEffect;
fs::path originalEffect;
fs::path invalidEffect;
fs::path alternateEffect;
unsigned modelPrompts = 0;
unsigned pickerCalls = 0;
unsigned compiled = 0;
unsigned originalCompiles = 0;
unsigned rejectedEffectCompiles = 0;
unsigned alternateEffectCompiles = 0;
RayEffectCreate nativeCompiler = nullptr;

INT_PTR ModelPrompt(MMDApp*, HINSTANCE, HWND, ProjectModelIssue issue, const wchar_t*) {
    Check(issue == ProjectModelIssue::MissingFile, "only the missing model recovery prompts");
    ++modelPrompts;
    return modelPrompts == 1 ? 1 : 4;
}

BOOL ModelPicker(LPOPENFILENAMEW picker) {
    ++pickerCalls;
    Check(picker && wcscpy_s(picker->lpstrFile, picker->nMaxFile, selected.c_str()) == 0,
        "return the exact selected model from the moved tree");
    return TRUE;
}

HRESULT WINAPI ObserveCompiler(IDirect3DDevice9* device, LPCWSTR filename,
        const void* macros, const void* includes, DWORD flags, void* pool, void** effect, void** errors) {
    const HRESULT hr = nativeCompiler(device, filename, macros, includes, flags, pool, effect, errors);
    if (filename && invalidEffect.native() == filename && FAILED(hr)) ++rejectedEffectCompiles;
    if (filename && SUCCEEDED(hr) && effect && *effect) {
        if (wantedEffect.native() == filename) ++compiled;
        if (originalEffect.native() == filename) ++originalCompiles;
        if (alternateEffect.native() == filename) ++alternateEffectCompiles;
    }
    return hr;
}

void Install(void* target, void* replacement, void** previous = nullptr) {
    Check(target && MH_CreateHook(target, replacement, previous) == MH_OK &&
          MH_EnableHook(target) == MH_OK, "install the exact picker or effect observer");
}

auto ReadEffectPixels(MMDApp* app, bool centerOnly = true) {
    app->CameraMode() = 1;
    app->ViewOffsetY() = 10;
    app->CameraDistance() = -45;
    app->CameraFov() = 30;
    app->CameraPerspective() = 0;
    app->GroundGridEnabled() = 0;
    app->state.messageSeen = 10;
    FrameDriver(app);
    FrameSceneOutputScope scope(app);
    Check(scope.Ready(), "establish hidden production render output");
    for (unsigned i = 0; i != 4; ++i) {
        app->state.messageSeen = 10;
        RenderFrameScene(app);
    }
    RECT center = app->ViewportRect();
    if (centerOnly) {
        InflateRect(&center, -(center.right - center.left) / 4, -(center.bottom - center.top) / 4);
    } else {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> backbuffer;
        D3DSURFACE_DESC description{};
        Check(SUCCEEDED(app->Renderer()->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
              SUCCEEDED(backbuffer->GetDesc(&description)), "read the actual backbuffer dimensions");
        const RECT available{0, 0, static_cast<LONG>(description.Width), static_cast<LONG>(description.Height)};
        Check(IntersectRect(&center, &center, &available) != FALSE, "the viewport intersects the completed backbuffer");
    }
    return ReadGizmoPixels(app->Renderer()->device, center);
}

auto CountEffectPixels(const std::vector<DWORD>& pixels, bool red) {
    return std::count_if(pixels.begin(), pixels.end(), [red](DWORD pixel) {
        const auto r = (pixel >> 16) & 0xffu, g = (pixel >> 8) & 0xffu;
        return (red ? r > 224 && g < 25 : g > 224 && r < 25) && (pixel & 0xffu) < 25;
    });
}

auto EffectPixels(MMDApp* app, bool red = false) {
    return CountEffectPixels(ReadEffectPixels(app), red);
}

// The production cache records its decisions in an in-memory diagnostic ring.
// Capture the same messages in this disposable test process.
void ObserveCacheLog(const char* format, ...) noexcept {
    const auto error = GetLastError();
    if (std::strncmp(format, "MME_EFFECT_CACHE ", 17) == 0) {
        std::va_list args;
        va_start(args, format);
        std::vprintf(format, args);
        va_end(args);
        std::putchar('\n');
    }
    SetLastError(error);
}

void RunEffectCache(MMDApp* app, const fs::path& root, std::wstring_view phase) {
    Check(plugins::LegacyMmeStatus().loaded && GetModuleHandleW(L"MMEffect.dll"),
        "load the real packaged MME provider");
    const auto directory = root / L"Projects";
    const auto model = directory / L"cache-model.pmx";
    const auto effect = directory / L"effects/cache-color.fx";
    const auto project = directory / L"cache-scene.pmm";
    Check(fs::is_regular_file(model) && fs::is_regular_file(effect), "use the owned MME cache fixture");
    if (phase == L"prepare") {
        Check(!fs::exists(project) && !fs::exists(pmm_paths::Effect(project)), "create the project once");
        LoadModelFile(app, model.c_str(), false);
        Check(app->ModelSlot(0) && SaveSceneCopy(app, project.c_str()), "save the real PMM");
        std::ofstream emm(pmm_paths::Effect(project), std::ios::binary);
        emm << "[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = " << model.string()
            << "\r\n[Effect]\r\nDefault = none\r\nPmd1 = effects\\cache-color.fx\r\n";
        emm.close();
        Check(bool(emm), "write the real MME sidecar assignment");
    } else {
        Check(phase == L"green" || phase == L"red", "read the expected fixture output colour");
        const auto mh = MH_Initialize();
        Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize cache log observer");
        auto* log = reinterpret_cast<void*>(&runtime_log::Write);
        Install(log, reinterpret_cast<void*>(&ObserveCacheLog));
        const auto start = std::chrono::steady_clock::now();
        Check(LoadSceneFile(project.c_str()) && app->ModelSlot(0) && !app->ModelSlot(1) &&
            fs::path(mdl::Mdl(app->ModelSlot(0))->path) == model, "open the same real project and model");
        const auto opened = std::chrono::steady_clock::now();
        const auto pixels = ReadEffectPixels(app);
        const auto rendered = std::chrono::steady_clock::now();
        Check(MH_DisableHook(log) == MH_OK && MH_RemoveHook(log) == MH_OK, "detach the test log observer");
        const auto green = CountEffectPixels(pixels, false);
        const auto red = CountEffectPixels(pixels, true);
        Check(phase == L"green" ? green > 100 && red == 0 : red > 100 && green == 0,
            "render the assigned effect with the current include colour");
        std::uint64_t hash = 14695981039346656037ull;
        for (auto pixel : pixels) {
            hash ^= pixel & 0xffffffu;
            hash *= 1099511628211ull;
        }
        using Milliseconds = std::chrono::duration<double, std::milli>;
        std::printf("MME_CACHE_FRAME green=%zu red=%zu pixels=%zu rgb_hash=%016llx load_ms=%.3f render_ms=%.3f\n",
            static_cast<std::size_t>(green), static_cast<std::size_t>(red), pixels.size(),
            static_cast<unsigned long long>(hash), Milliseconds(opened - start).count(),
            Milliseconds(rendered - opened).count());
    }
    std::printf("PASS real MME effect cache phase=%ls\n", phase.data());
}

unsigned effectPickers = 0, missingEffectNotices = 0, standaloneFailures = 0;
decltype(&MessageBoxA) nativeMessageBox = nullptr;
std::vector<int> standaloneAnswers;
std::size_t standalonePromptIndex = 0;
HWND lastStandalonePrompt;

LRESULT CALLBACK DriveStandaloneEffectRecovery(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        const auto dialog = reinterpret_cast<HWND>(wp);
        wchar_t label[96]{};
        GetDlgItemTextW(dialog, 1003, label, 96);
        if (std::wcscmp(label, L"Open without recovery") == 0) {
            if (dialog == lastStandalonePrompt) return 1;
            lastStandalonePrompt = dialog;
            Check(standalonePromptIndex < standaloneAnswers.size(), "only the owned missing-FX dialog may open");
            const auto list = GetDlgItem(dialog, 1002);
            wchar_t reference[256]{};
            Check(SendMessageW(list, LB_GETCOUNT, 0, 0) == 1 &&
                  SendMessageW(list, LB_GETCURSEL, 0, 0) == 0 &&
                  SendMessageW(list, LB_GETTEXTLEN, 0, 0) < 256 &&
                  SendMessageW(list, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(reference)) > 0 &&
                  originalEffect == fs::path(reference),
                  "show and preselect the exact standalone missing EMM reference");
            GetDlgItemTextW(dialog, IDCANCEL, label, 96);
            Check(std::wcscmp(label, L"Cancel opening") == 0, "offer an explicit whole-open cancel action");
            const int choice = standaloneAnswers[standalonePromptIndex++];
            Check(PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(choice, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(dialog, choice))) != FALSE, "submit the owned recovery choice");
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

BOOL WINAPI ObserveEffectPickerA(LPOPENFILENAMEA) {
    Check(false, "standalone recovery uses only the Unicode FX picker");
    return FALSE;
}

BOOL WINAPI ObserveEffectPickerW(LPOPENFILENAMEW picker) {
    Check(picker && picker->lpstrTitle && std::wcsstr(picker->lpstrTitle, L"bound.fx"),
          "use the real Unicode selection entry for the missing FX");
    ++effectPickers;
    if (effectPickers == 1) return FALSE;
    Check(effectPickers <= 3, "the existing and skipped FX paths need no extra picker");
    const auto& path = effectPickers == 2 ? invalidEffect : wantedEffect;
    Check(wcscpy_s(picker->lpstrFile, picker->nMaxFile, path.c_str()) == 0, "choose the owned fixture FX");
    return TRUE;
}

int WINAPI ObserveMissingEffectNotice(HWND owner, LPCSTR message, LPCSTR title, UINT flags) {
    if (message && title && std::strcmp(title, "MikuMikuEffect") == 0 &&
        std::strstr(message, "The following effect files do not exist:") &&
        std::strstr(message, "bound.fx")) {
        Check((flags & MB_TYPEMASK) == MB_OK, "missing effect notice only offers acknowledgement");
        ++missingEffectNotices;
        std::printf("MME missing FX notice: %s\n", message);
        return IDOK;
    }
    return nativeMessageBox(owner, message, title, flags);
}

int StandaloneCompileNotice(MMDApp*, HWND, UiMessage message, UINT flags, const wchar_t* detail) {
    Check(message == UiMessage::ProjectEffectRecoveryFailed && (flags & MB_TYPEMASK) == MB_OK &&
          detail && invalidEffect.native() == detail, "report the invalid replacement and leave the recovery dialog available");
    ++standaloneFailures;
    return IDOK;
}

void CheckMissingEffectEntry(MMDApp* app, const fs::path& project, const fs::path& healthy,
                             const fs::path& firstModel, const fs::path& secondModel, std::ptrdiff_t baselineGreen) {
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize isolated effect entry observation");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1") != FALSE, "observe actual effect compilation");
    invalidEffect = project.parent_path() / L"invalid/Effects/Light/bound.fx";
    Check(fs::create_directories(invalidEffect.parent_path()), "prepare the isolated invalid FX path");
    std::ofstream(invalidEffect, std::ios::binary) << "this is not valid HLSL";
    auto* compiler = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
    auto* ansiPicker = reinterpret_cast<void*>(&GetOpenFileNameA);
    auto* widePicker = reinterpret_cast<void*>(&GetOpenFileNameW);
    auto* notice = reinterpret_cast<void*>(&MessageBoxA);
    auto* compileNotice = reinterpret_cast<void*>(&ShowUiMessage);
    Install(compiler, reinterpret_cast<void*>(&ObserveCompiler), reinterpret_cast<void**>(&nativeCompiler));
    Install(ansiPicker, reinterpret_cast<void*>(&ObserveEffectPickerA));
    Install(widePicker, reinterpret_cast<void*>(&ObserveEffectPickerW));
    Install(notice, reinterpret_cast<void*>(&ObserveMissingEffectNotice), reinterpret_cast<void**>(&nativeMessageBox));
    Install(compileNotice, reinterpret_cast<void*>(&StandaloneCompileNotice));
    const auto dialogHook = SetWindowsHookExW(WH_CBT, DriveStandaloneEffectRecovery, nullptr, GetCurrentThreadId());
    Check(dialogHook != nullptr, "drive the production missing-effect dialog without activation");

    const auto open = [&](std::initializer_list<int> answers, bool succeeds = true) {
        app->EnglishUI() = kUiEnglish;
        standaloneAnswers = answers;
        standalonePromptIndex = 0;
        lastStandalonePrompt = nullptr;
        Check(LoadSceneFile(project.c_str()) == succeeds && standalonePromptIndex == standaloneAnswers.size(),
              "follow the expected standalone decisions in the real PMM loader");
        if (!succeeds) return std::ptrdiff_t{0};
        Check(app->ModelSlot(0) && app->ModelSlot(1) &&
              fs::path(mdl::Mdl(app->ModelSlot(0))->path) == firstModel &&
              fs::path(mdl::Mdl(app->ModelSlot(1))->path) == secondModel &&
              fs::path(app->EnvFileName()) == project && !app->SceneModified(),
              "both models stay at their original paths and open does not mark the scene dirty");
        return EffectPixels(app);
    };
    const auto greenBefore = open({});
    Check(greenBefore > baselineGreen + 100 && originalCompiles > 0 && !missingEffectNotices,
          "the available original effect compiles and draws before any recovery");
    Check(SetCurrentDirectoryW(project.parent_path().c_str()) != FALSE,
          "leave the original FX directory before moving only the effect");
    Check(fs::create_directories(wantedEffect.parent_path()), "create the relocated effect directory");
    fs::rename(originalEffect, wantedEffect);

    const auto greenSkipped = open({1003});
    Check(missingEffectNotices > 0 && effectPickers == 0 && compiled == 0 &&
          greenBefore > greenSkipped + 100, "open with both models after explicitly skipping FX recovery");
    std::puts("PASS standalone skip opens both original models without binding the missing effect");

    const auto noticesBeforeCancel = missingEffectNotices;
    open({IDCANCEL}, false);
    Check(!app->ModelSlot(0) && !app->ModelSlot(1) && !app->EnvFileName()[0] &&
          project_relocation::Ticket::Capture(*app).Infer(originalEffect.native()).status == project_relocation::Status::NoMatch,
          "cancel retires the partial project and its relocation owner");
    Check(LoadSceneFile(healthy.c_str()) && EffectPixels(app) == baselineGreen &&
          missingEffectNotices == noticesBeforeCancel && compiled == 0,
          "the next project receives no pending FX binding or missing notice from the cancelled open");
    std::puts("PASS standalone cancel clears the partial project and leaves the next scene independent");

    const auto recovered = open({IDOK, IDOK, IDOK});
    const auto mapping = project_relocation::Ticket::Capture(*app).Infer(originalEffect.native());
    Check(effectPickers == 3 && standaloneFailures == 1 && rejectedEffectCompiles == 1 && compiled >= 2 &&
          mapping.status == project_relocation::Status::Candidate && fs::path(mapping.path) == wantedEffect &&
          recovered == greenBefore && missingEffectNotices == noticesBeforeCancel,
          "cancel picker or reject broken FX then retry, compile and draw the selected FX without moving either model");
    std::puts("PASS standalone recovery retries a cancelled picker and invalid FX, then binds and draws the moved FX");

    const auto compiledBeforeReopen = compiled;
    Check(open({1003}) == greenSkipped && compiled == compiledBeforeReopen &&
          project_relocation::Ticket::Capture(*app).Infer(originalEffect.native()).status == project_relocation::Status::NoMatch,
          "reopening and choosing Skip never carries over an earlier replacement");
    Check(fs::copy_file(wantedEffect, originalEffect), "restore the original effect path");
    const auto previousNotices = missingEffectNotices, previousCompiles = originalCompiles;
    Check(open({}) == greenBefore && effectPickers == 3 && missingEffectNotices == previousNotices &&
          originalCompiles > previousCompiles, "a restored original needs no recovery and draws its original effect");
    std::puts("PASS standalone mapping is confined to the accepted open; restoring the original needs no picker");
    Check(UnhookWindowsHookEx(dialogHook) != FALSE, "remove the isolated recovery dialog hook");
    for (void* hook : {compileNotice, notice, widePicker, ansiPicker, compiler}) {
        MH_DisableHook(hook);
        MH_RemoveHook(hook);
    }
}

unsigned effectRecoveryDialogs = 0, effectReferenceDialogs = 0, effectCompileFailures = 0;
bool testEffectSuccess = false;

LRESULT CALLBACK DriveEffectRecovery(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        const auto dialog = reinterpret_cast<HWND>(wp);
        wchar_t label[96]{};
        GetDlgItemTextW(dialog, 688, label, 96);
        if (std::wcscmp(label, L"Locate an effect first") == 0) {
            const auto cancel = GetDlgItem(dialog, 690) ? 690 : 689;
            const auto button = !testEffectSuccess && effectRecoveryDialogs == 3 ? cancel : 688;
            ++effectRecoveryDialogs;
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(button, BN_CLICKED),
                         reinterpret_cast<LPARAM>(GetDlgItem(dialog, button)));
            return 1;
        }
        GetDlgItemTextW(dialog, 1001, label, 96);
        if (std::wcscmp(label, L"Select the original FX that is missing.") == 0) {
            ++effectReferenceDialogs;
            Check(SendDlgItemMessageW(dialog, 1002, LB_GETCOUNT, 0, 0) == 2,
                  "both missing EMM paths are shown in the production dialog");
            // The list is in path order. Select bound.fx, after alternate.fx.
            wchar_t reference[256]{};
            Check(SendDlgItemMessageW(dialog, 1002, LB_GETTEXTLEN, 1, 0) < 256 &&
                  SendDlgItemMessageW(dialog, 1002, LB_GETTEXT, 1, reinterpret_cast<LPARAM>(reference)) > 0 &&
                  originalEffect == fs::path(reference), "choose the exact EMM reference rather than the first list item");
            SendDlgItemMessageW(dialog, 1002, LB_SETCURSEL, 1, 0);
            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

BOOL RecoveryPicker(LPOPENFILENAMEW picker) {
    Check(picker && picker->lpstrTitle && std::wcsstr(picker->lpstrTitle, L"bound.fx") &&
          (picker->Flags & OFN_FILEMUSTEXIST), "open the FX picker for the exact original file");
    ++pickerCalls;
    if (!testEffectSuccess && pickerCalls == 1) return FALSE;
    if (!testEffectSuccess && pickerCalls == 3) {
        Check(fs::create_directories(originalEffect.parent_path()) && fs::copy_file(wantedEffect, originalEffect),
              "restore the old FX path while the chooser is open");
    }
    const auto& chosen = testEffectSuccess || pickerCalls == 3 ? wantedEffect : invalidEffect;
    Check(wcscpy_s(picker->lpstrFile, picker->nMaxFile, chosen.c_str()) == 0, "select the owned fixture effect");
    return TRUE;
}

int RecoveryCompileNotice(MMDApp*, HWND, UiMessage message, UINT flags, const wchar_t* detail) {
    Check(message == UiMessage::ProjectEffectRecoveryFailed && (flags & MB_TYPEMASK) == MB_OK &&
          detail && invalidEffect.native() == detail, "show the failed replacement FX path");
    ++effectCompileFailures;
    return IDOK;
}

void CheckEffectFirst(MMDApp* app, const fs::path& project, const fs::path& healthy, const fs::path& root,
                      const fs::path& firstModel, const fs::path& secondModel, std::ptrdiff_t baselineGreen, bool v1) {
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize effect recovery observers");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1"), "observe the true FX compiler");
    invalidEffect = root / L"invalid/Effects/Light/bound.fx";
    Check(fs::create_directories(invalidEffect.parent_path()), "prepare the isolated invalid FX path");
    std::ofstream(invalidEffect, std::ios::binary) << "this is not valid HLSL";
    auto* compiler = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
    auto* picker = reinterpret_cast<void*>(&GetUiOpenFileNameW);
    auto* notice = reinterpret_cast<void*>(&ShowUiMessage);
    Install(compiler, reinterpret_cast<void*>(&ObserveCompiler), reinterpret_cast<void**>(&nativeCompiler));
    Install(picker, reinterpret_cast<void*>(&RecoveryPicker));
    Install(notice, reinterpret_cast<void*>(&RecoveryCompileNotice));
    const auto dialogHook = SetWindowsHookExW(WH_CBT, DriveEffectRecovery, nullptr, GetCurrentThreadId());
    Check(dialogHook != nullptr, "drive production recovery dialogs without activating them");
    app->EnglishUI() = kUiEnglish;
    Check(!LoadSceneFile(project.c_str()) && effectRecoveryDialogs == 4 && effectReferenceDialogs == 3 &&
          pickerCalls == 3 && effectCompileFailures == 1 && rejectedEffectCompiles == 1 && compiled == 0 &&
          !app->ModelSlot(0) && !app->ModelSlot(1),
          "picker cancellation, a compiler failure and the reappearing original return to model recovery");
    Check(project_relocation::Ticket::Capture(*app).Infer(originalEffect.native()).status == project_relocation::Status::NoMatch,
          "cancelled and failed recovery leave no path mapping");
    Check(LoadSceneFile(healthy.c_str()) && EffectPixels(app) == baselineGreen && compiled == 0,
          "reopen the unrelated project after effect recovery was cancelled");
    std::puts("PASS effect picker cancellation and compile failure keep model recovery available");
    Check(fs::remove(originalEffect), "remove only the owned FX copy restored by the picker");
    std::puts("PASS an original FX that reappears during selection is not redirected");

    testEffectSuccess = true;
    Check(LoadSceneFile(project.c_str()) && effectRecoveryDialogs == 5 && effectReferenceDialogs == 4 &&
          pickerCalls == 4 && effectCompileFailures == 1 && compiled == 1 &&
          app->ModelSlot(0) && app->ModelSlot(1) &&
          fs::path(mdl::Mdl(app->ModelSlot(0))->path) == firstModel &&
          fs::path(mdl::Mdl(app->ModelSlot(1))->path) == secondModel,
          "a real FX preflight and one file choice recover both model paths during the same PMM load");
    if (v1) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto model = app->ModelSlot(slot);
            const auto keys = mdl::BoneKeys(model);
            Check(mdl::Mdl(model)->boneCount == 1 && keys && keys[0].position[0] == float(10 + slot),
                  "V1 recovery preserves the distinct serialized bone key of each migrated PMD");
        }
        std::puts("PASS V1 PMM keeps both serialized bone keys after effect-first recovery");
    }
    const auto green = EffectPixels(app);
    Check(compiled > 1 && alternateEffectCompiles > 0 && green > baselineGreen + 100,
          "MME binds and draws the recovered effect and compiles the sibling effect after loading both models");
    Check(LoadSceneFile(healthy.c_str()) && EffectPixels(app) == baselineGreen,
          "the next scene cannot reuse the effect's path mapping");
    UnhookWindowsHookEx(dialogHook);
    for (void* hook : {notice, picker, compiler}) {
        MH_DisableHook(hook);
        MH_RemoveHook(hook);
    }
    std::puts("PASS one FX selection recovers both models and binds the real MME effect");
}

struct SameNameRecovery {
    std::array<fs::path, 2> originals, replacements;
    unsigned dialogs = 0, picks = 0, failures = 0;
    bool skip = false;
    HWND lastDialog = nullptr;
};
SameNameRecovery* sameNameRecovery = nullptr;

LRESULT CALLBACK DriveSameNameRecovery(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE && sameNameRecovery) {
        auto& probe = *sameNameRecovery;
        const auto dialog = reinterpret_cast<HWND>(wp);
        wchar_t label[64]{};
        GetDlgItemTextW(dialog, 1003, label, 64);
        if (std::wcscmp(label, L"Open without recovery") == 0) {
            if (dialog == probe.lastDialog) return 1;
            probe.lastDialog = dialog;
            const unsigned index = probe.dialogs++;
            Check(index < (probe.skip ? 1u : 2u),
                  "each confirmed same-name FX is removed from the list, without another recovery failure");
            const auto list = GetDlgItem(dialog, 1002);
            const auto rows = SendMessageW(list, LB_GETCOUNT, 0, 0);
            Check(rows == 2 - index, "show each unresolved same-name reference exactly once");
            if (rows == 2)
                Check(SendMessageW(list, LB_GETCURSEL, 0, 0) == LB_ERR && !IsWindowEnabled(GetDlgItem(dialog, IDOK)),
                      "same-name recovery waits for an explicit choice");
            int selectedRow = -1;
            for (int row = 0; row < rows; ++row) {
                wchar_t path[256]{};
                const auto length = SendMessageW(list, LB_GETTEXTLEN, row, 0);
                Check(length > 0 && length < 256 &&
                      SendMessageW(list, LB_GETTEXT, row, reinterpret_cast<LPARAM>(path)) == length,
                      "read the exact path from the real list");
                const fs::path reference(path);
                Check(reference == probe.originals[index] || (index == 0 && reference == probe.originals[1]),
                      "do not conflate the two same-name references");
                if (reference == probe.originals[index]) selectedRow = row;
            }
            Check(selectedRow >= 0, "the intended same-name source is listed");
            if (!probe.skip) {
                Check(SendMessageW(list, LB_SETCURSEL, selectedRow, 0) != LB_ERR, "select the source by its full path");
                SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(1002, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
                Check(IsWindowEnabled(GetDlgItem(dialog, IDOK)), "a source choice enables replacement");
            }
            const int command = probe.skip ? 1003 : IDOK;
            Check(PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED),
                               reinterpret_cast<LPARAM>(GetDlgItem(dialog, command))) != FALSE,
                  "submit the real same-name recovery dialog command");
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

BOOL WINAPI PickSameNamedEffect(LPOPENFILENAMEW picker) {
    auto& probe = *sameNameRecovery;
    const auto index = probe.picks++;
    Check(picker && index < probe.replacements.size() && !probe.skip &&
          picker->lpstrTitle && std::wcsstr(picker->lpstrTitle, probe.originals[index].c_str()) &&
          wcscpy_s(picker->lpstrFile, picker->nMaxFile, probe.replacements[index].c_str()) == 0,
          "identify the selected original in the picker and choose the replacement with the same leaf name");
    return TRUE;
}

int RecordSameNameFailure(MMDApp*, HWND, UiMessage message, UINT, const wchar_t*) {
    Check(message == UiMessage::ProjectEffectRecoveryFailed, "only observe an FX recovery rejection");
    ++sameNameRecovery->failures;
    std::puts("SAME_NAME explicit FX replacement rejected");
    return IDOK;
}

void CheckSameNamedEffects(MMDApp* app, const fs::path& root, const fs::path& project,
                          const fs::path& healthy, const fs::path& first, const fs::path& second,
                          const fs::path& firstFx, const fs::path& secondFx) {
    SameNameRecovery probe{{firstFx, secondFx}, {
        root / L"green/Effects/Light/bound.fx", root / L"red/Effects/Shadow/bound.fx"}};
    sameNameRecovery = &probe;
    app->EnglishUI() = kUiEnglish;
    const auto open = [&] {
        probe.lastDialog = nullptr;
        Check(LoadSceneFile(project.c_str()) && app->ModelSlot(0) && app->ModelSlot(1) &&
              fs::path(mdl::Mdl(app->ModelSlot(0))->path) == first &&
              fs::path(mdl::Mdl(app->ModelSlot(1))->path) == second && !app->SceneModified(),
              "open both original models through the real PMM path");
    };
    const auto colours = [&](unsigned shown) {
        for (unsigned i = 0; i < 2; ++i) {
            auto* model = mdl::Mdl(app->ModelSlot(i));
            model->loadComplete = model->displayKeys[0].visible = i == shown;
        }
        const auto pixels = ReadEffectPixels(app, false);
        return std::pair{CountEffectPixels(pixels, false), CountEffectPixels(pixels, true)};
    };
    const auto verify = [&] {
        const auto a = colours(0), b = colours(1);
        std::printf("SAME_NAME model0 green=%zu red=%zu model1 green=%zu red=%zu\n",
            size_t(a.first), size_t(a.second), size_t(b.first), size_t(b.second));
        // The wider V1 sample also contains a few saturated overlay pixels.
        // Compare the model's dominant colour against the independent control.
        Check(a.first > 1000 && a.second < a.first / 8 && b.second > 1000 && b.first < b.second / 8,
              "each model independently draws its own same-name FX");
        return std::pair{a.first, b.second};
    };
    open();
    const auto baseline = verify();
    Check(SetCurrentDirectoryW(root.c_str()), "leave the original shader directories");
    for (unsigned i = 0; i < 2; ++i) {
        Check(fs::create_directories(probe.replacements[i].parent_path()), "prepare an independent destination root");
        fs::rename(probe.originals[i], probe.replacements[i]);
    }
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize same-name recovery observation");
    auto* picker = reinterpret_cast<void*>(&GetOpenFileNameW);
    auto* notice = reinterpret_cast<void*>(&MessageBoxA);
    auto* failure = reinterpret_cast<void*>(&ShowUiMessage);
    Install(picker, reinterpret_cast<void*>(&PickSameNamedEffect));
    Install(notice, reinterpret_cast<void*>(&ObserveMissingEffectNotice), reinterpret_cast<void**>(&nativeMessageBox));
    Install(failure, reinterpret_cast<void*>(&RecordSameNameFailure));
    const auto dialog = SetWindowsHookExW(WH_CBT, DriveSameNameRecovery, nullptr, GetCurrentThreadId());
    Check(dialog != nullptr, "exercise the actual same-name selection dialog");
    open();
    Check(probe.dialogs == 2 && probe.picks == 2 && !probe.failures && !missingEffectNotices,
          "the two explicit replacements complete without rejection");
    const auto recovered = verify();
    const auto sameCoverage = [](auto expected, auto actual) {
        return actual >= expected * 9 / 10 && actual <= expected * 11 / 10;
    };
    Check(sameCoverage(baseline.first, recovered.first) && sameCoverage(baseline.second, recovered.second),
          "both recovered colours cover the same models as the original files");
    const auto accepted = project_relocation::Ticket::Capture(*app);
    for (unsigned i = 0; i < 2; ++i) {
        const auto mapping = accepted.Infer(probe.originals[i].native());
        Check(mapping.status == project_relocation::Status::Candidate && fs::path(mapping.path) == probe.replacements[i],
              "keep the chosen destinations for both references");
    }
    Check(LoadSceneFile(healthy.c_str()) && !accepted.Current(*app) &&
          accepted.Infer(firstFx.native()).status == project_relocation::Status::NoMatch,
          "the unrelated scene retires the exact FX mappings");
    const auto clean = ReadEffectPixels(app);
    Check(CountEffectPixels(clean, false) == 0 && CountEffectPixels(clean, true) == 0,
          "the unrelated scene draws neither recovered FX");
    probe.skip = true;
    probe.dialogs = 0;
    open();
    const auto firstSkipped = colours(0), secondSkipped = colours(1);
    Check(firstSkipped.first < baseline.first / 10 && firstSkipped.second < baseline.second / 10 &&
          secondSkipped.first < baseline.first / 10 && secondSkipped.second < baseline.second / 10 &&
          probe.dialogs == 1 && probe.picks == 2 &&
          missingEffectNotices > 0, "reopen and skip without reusing either previous replacement");
    Check(UnhookWindowsHookEx(dialog) != FALSE, "remove same-name recovery dialog observation");
    for (void* hook : {picker, notice, failure}) {
        MH_DisableHook(hook);
        MH_RemoveHook(hook);
    }
    sameNameRecovery = nullptr;
    std::puts("PASS real MME recovers and draws same-name FX separately and retires both on reopen");
}

void Run(MMDApp* app, const fs::path& root, bool relative, bool missingEffectEntry, bool effectFirst, bool v1,
         bool sameName) {
    const auto projectDirectory = relative ? root / L"Projects" : root;
    const auto old = projectDirectory / L"old", moved = root / L"moved";
    const auto a = fs::path(v1 ? L"Models\\A\\first.pmd" : L"Models\\A\\first.pmx");
    const auto b = fs::path(v1 ? L"Models\\B\\second.pmd" : L"Models\\B\\second.pmx");
    const auto fx = fs::path(L"Effects\\Light\\bound.fx");
    const auto project = projectDirectory / L"moved-scene.pmm", emm = pmm_paths::Effect(project);
    selected = moved / a;
    wantedEffect = moved / fx;
    if (relative) originalEffect = old / fx;
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"), "load the real legacy MME pair");
    Check(fs::exists(project) == v1 && !fs::exists(emm) && !fs::exists(moved), "own a fresh project migration fixture");
    const auto healthy = root / L"healthy.pmm";
    const auto healthyModel = root / (v1 ? L"different-object.pmd" : L"different-object.pmx");
    fs::copy_file(old / a, healthyModel);
    LoadModelFile(app, healthyModel.c_str(), false);
    Check(app->ModelSlot(0) && SaveSceneCopy(app, healthy.c_str()) && !fs::exists(pmm_paths::Effect(healthy)),
        "save an unrelated healthy project with a distinct model path and no EMM");
    Check(LoadSceneFile(healthy.c_str()), "open the unrelated project for its own baseline");
    const auto baselineGreen = EffectPixels(app);
    const auto baselineRed = relative ? EffectPixels(app, true) : 0;
    Check(ResetAppState(app), "reset before preparing the migration project");
    if (v1) {
        Check(LoadSceneFile(project.c_str()) && app->ModelSlot(0) && app->ModelSlot(1),
              "read the V1 project while both original PMDs are available");
    } else {
        LoadModelFile(app, (old / a).c_str(), false);
        LoadModelFile(app, (old / b).c_str(), false);
        Check(app->ModelSlot(0) && app->ModelSlot(1), "load both original models into the source project");
        app->CameraMode() = 1;
        app->GroundGridEnabled() = 0;
        app->CameraDistance() = -20;
        app->ViewOffsetY() = 0;
        Check(SaveSceneCopy(app, project.c_str()), "save two old model paths through the real PMM writer");
    }
    Check(ResetAppState(app), "reset before moving the resource root");
    Check(SetCurrentDirectoryW(root.c_str()) != FALSE, "leave the old resource directory before moving it");

    const auto writeEmm = [&](const fs::path& path) {
        std::ofstream file(path, std::ios::binary);
        const auto effectReference = relative ? fs::path(L"old") / fx : old / fx;
        if (relative) {
            Check(!effectReference.has_root_path() && path.parent_path() / effectReference == old / fx &&
                  !fs::exists(root / effectReference),
                "the EMM-relative reference points at the old tree only from the EMM directory");
        }
        auto secondEffect = effectReference;
        if (sameName) secondEffect = fs::path(L"old/Effects/Shadow/bound.fx");
        if (effectFirst) {
            secondEffect.replace_filename(L"alternate.fx");
            Check(fs::copy_file(old / fx, projectDirectory / secondEffect), "prepare a second referenced FX");
        }
        file << "[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = " << (old / a).string()
             << "\r\nPmd2 = " << (old / b).string() << "\r\n[Effect]\r\nDefault = none\r\nPmd1 = "
             << effectReference.string() << "\r\nPmd2 = " << secondEffect.string() << "\r\n";
        Check(file.good(), "save the real EMM rows with the old object and effect paths");
    };
    writeEmm(emm);
    const auto read = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        Check(file.good(), "read original fixture data for the unchanged-content check");
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto pmmBytes = read(project), emmBytes = read(emm);
    if (sameName) {
        CheckSameNamedEffects(app, root, project, healthy, old / a, old / b, old / fx,
                              old / L"Effects\\Shadow\\bound.fx");
        Check(read(project) == pmmBytes && read(emm) == emmBytes, "same-name recovery preserves PMM and EMM bytes");
        return;
    }
    if (missingEffectEntry) {
        CheckMissingEffectEntry(app, project, healthy, old / a, old / b, baselineGreen);
        Check(read(project) == pmmBytes && read(emm) == emmBytes, "probing the effect never rewrites the PMM or EMM");
        return;
    }
    std::error_code moveError;
    fs::rename(old, moved, moveError);
    if (moveError) std::fprintf(stderr, "fixture move failed: %d\n", moveError.value());
    Check(!moveError, "move only the owned fixture root");
    Check(!fs::exists(old) && fs::is_regular_file(wantedEffect), "move models and sibling effect directory together");
    if (effectFirst) {
        alternateEffect = wantedEffect.parent_path() / L"alternate.fx";
        CheckEffectFirst(app, project, healthy, root, moved / a, moved / b, baselineGreen, v1);
        Check(read(project) == pmmBytes && read(emm) == emmBytes, "effect recovery leaves the PMM and EMM byte-identical");
        return;
    }

    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize private test hooks");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1") != FALSE, "observe actual software effect compilation");
    auto* prompt = reinterpret_cast<void*>(&ShowProjectModelDialog);
    auto* picker = reinterpret_cast<void*>(&GetUiOpenFileNameW);
    auto* compiler = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
    Install(prompt, reinterpret_cast<void*>(&ModelPrompt));
    Install(picker, reinterpret_cast<void*>(&ModelPicker));
    Install(compiler, reinterpret_cast<void*>(&ObserveCompiler), reinterpret_cast<void**>(&nativeCompiler));

    Check(LoadSceneFile(project.c_str()), "load the moved PMM after selecting one model");
    Check(modelPrompts == 1 && pickerCalls == 1, "one manual model choice finds the other model by hierarchy");
    Check(app->ModelSlot(0) && app->ModelSlot(1) && selected.native() == mdl::Mdl(app->ModelSlot(0))->path &&
          (moved / b).native() == mdl::Mdl(app->ModelSlot(1))->path,
        "both loaded models keep the exact relocated paths");
    std::puts("PASS one model selection relocates both native model paths");
    const auto green = EffectPixels(app);
    Check(compiled > 0, "MME compiles the relocated sibling effect through its real compiler");
    std::printf("DIAG relocated effect compiled=%u green=%zu\n", compiled, static_cast<std::size_t>(green));
    Check(green > baselineGreen + 100, "the relocated green effect is actually bound and draws model pixels");
    std::printf("PASS model selection recovers the real sibling MME effect prompts=%u compiled=%u green=%zu\n",
        modelPrompts, compiled, static_cast<std::size_t>(green));
    if (relative) {
        std::puts("PASS real MME binds the moved EMM-relative effect");
        Check(!originalCompiles && !fs::exists(originalEffect), "the initial pass had no original effect");
        Check(fs::create_directories(originalEffect.parent_path()) &&
              fs::copy_file(root / L"original-control.fx", originalEffect),
            "restore only the original relative FX while both models still need relocation");
        modelPrompts = pickerCalls = 0;
        Check(LoadSceneFile(project.c_str()) && modelPrompts == 1 && pickerCalls == 1,
            "reopen the same PMM with the original FX restored and the models still moved");
        const auto red = EffectPixels(app, true);
        std::printf("DIAG original relative effect compiled=%u red=%zu baseline=%zu\n",
            originalCompiles, static_cast<std::size_t>(red), static_cast<std::size_t>(baselineRed));
        Check(originalCompiles > 0 && red > baselineRed + 100 && EffectPixels(app) == baselineGreen,
            "the existing EMM-relative original is compiled and drawn ahead of the relocated FX");
        std::puts("PASS real MME prefers the existing EMM-relative original");
        Check(SetCurrentDirectoryW(root.c_str()) != FALSE, "leave the effect directory before cancellation");
        fs::rename(originalEffect, root / L"original-restored.fx", moveError);
        Check(!moveError, "park the owned original effect before checking cancellation");
    }

    const auto missing = moved / b;
    const auto parked = missing.parent_path() / L"parked.pmx";
    fs::rename(missing, parked);
    modelPrompts = pickerCalls = 0;
    const auto beforeCancel = compiled;
    Check(!LoadSceneFile(project.c_str()) && modelPrompts == 2 && pickerCalls == 1 &&
          !app->ModelSlot(0) && !app->ModelSlot(1),
        "cancel at the second model after accepting a new root and clear the partial scene");
    Check(project_relocation::Ticket::Capture(*app).Infer((old / fx).native()).status == project_relocation::Status::NoMatch,
        "the cancelled load leaves no project root that can redirect effects");
    fs::rename(parked, missing);
    Check(LoadSceneFile(healthy.c_str()) && app->ModelSlot(0) && !app->ModelSlot(1) &&
          healthyModel.native() == mdl::Mdl(app->ModelSlot(0))->path,
        "open a healthy project after cancelling recovery");
    const auto greenAfterCancel = EffectPixels(app);
    std::printf("DIAG after cancel compiled=%u previous=%u green=%zu healthy_baseline=%zu\n",
        compiled, beforeCancel, static_cast<std::size_t>(greenAfterCancel), static_cast<std::size_t>(baselineGreen));
    Check(greenAfterCancel == baselineGreen && compiled == beforeCancel,
        "the healthy project never compiles or draws the cancelled project's effect");
    Check(read(project) == pmmBytes && read(emm) == emmBytes,
        "the original PMM and EMM bytes are unchanged after recovery and cancellation");
    std::puts("PASS cancelled model recovery does not leak an effect into the next project");
    for (void* hook : {compiler, picker, prompt}) {
        MH_DisableHook(hook);
        MH_RemoveHook(hook);
    }
}

fs::path mixedSaveTarget;
unsigned mixedSavePicks = 0;
unsigned mixedAccessoryPrompts = 0;
bool mixedExpectV1Model = false;

INT_PTR MixedModelPrompt(MMDApp* app, HINSTANCE instance, HWND owner,
                         ProjectModelIssue issue, const wchar_t* name) {
    if (issue == ProjectModelIssue::MissingAccessory) {
        ++mixedAccessoryPrompts;
        return 4;
    }
    if (mixedExpectV1Model) {
        Check(issue == ProjectModelIssue::MissingV1Model, "the V1 source uses the V1 model recovery prompt");
        return ++modelPrompts == 1 ? 1 : 4;
    }
    return ModelPrompt(app, instance, owner, issue, name);
}

BOOL MixedSavePicker(LPOPENFILENAMEW picker) {
    ++mixedSavePicks;
    Check(picker && picker->lpstrDefExt && std::wcscmp(picker->lpstrDefExt, L"pmm") == 0 &&
          wcscpy_s(picker->lpstrFile, picker->nMaxFile, mixedSaveTarget.c_str()) == 0,
          "Save As requests a PMM and receives the isolated destination");
    return TRUE;
}

void RunMixedVac(MMDApp* app, const fs::path& root, std::wstring_view phase, bool v1) {
    const auto old = root / L"Projects" / L"old";
    const auto moved = root / L"moved";
    const auto project = old.parent_path() / L"mixed.pmm";
    const auto sourceEmm = pmm_paths::Effect(project);
    mixedSaveTarget = root / L"Saved" / L"recovered.pmm";
    const auto savedEmm = pmm_paths::Effect(mixedSaveTarget);
    const fs::path first = v1 ? L"Models\\A\\first.pmd" : L"Models\\A\\first.pmx";
    const fs::path second = v1 ? L"Models\\B\\second.pmd" : L"Models\\B\\second.pmx";
    const fs::path descriptor = L"UserFile\\Props\\placement.vac";
    const fs::path mesh = L"UserFile\\Props\\UserFile\\meshes\\shape.x";
    const fs::path texture = mesh.parent_path() / L"image.tga";
    const fs::path fx = L"Effects\\Light\\bound.fx";
    selected = moved / first;
    wantedEffect = moved / fx;
    alternateEffect = wantedEffect.parent_path() / L"accessory.fx";
    const bool recovering = phase == L"recover";
    mixedExpectV1Model = recovering && v1;
    Check(recovering || phase == L"reopen", "select the mixed resource phase");
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
        "load the real legacy MME pair for the mixed resource project");
    const auto read = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        Check(file.good(), "read an owned mixed fixture file");
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto checkResources = [&](const fs::path& tree) {
        Check(app->ModelSlot(0) && app->ModelSlot(1) && !app->ModelSlot(2) &&
              fs::path(mdl::Mdl(app->ModelSlot(0))->path) == tree / first &&
              fs::path(mdl::Mdl(app->ModelSlot(1))->path) == tree / second,
            "the mixed project retains both exact model paths");
        if (v1) {
            for (unsigned slot = 0; slot < 2; ++slot) {
                const auto model = app->ModelSlot(slot);
                const auto keys = mdl::BoneKeys(model);
                Check(mdl::Mdl(model)->boneCount == 1 && keys && keys[0].position[0] == float(10 + slot),
                      "each V1 PMD keeps its distinct serialized bone key");
            }
        }
        const auto* accessory = app->AccessorySlot(0);
        Check(accessory && !app->AccessorySlot(1) && accessory->mesh && accessory->materials &&
              accessory->materialCount == 1 && accessory->texturePaths &&
              fs::path(accessory->sourcePath) == tree / descriptor &&
              fs::equivalent(accessory->directory, (tree / mesh).parent_path()),
            "the mixed project keeps the VAC descriptor and resolves its nested X");
        const auto color = static_cast<const D3DMATERIAL9*>(accessory->materials)->Ambient.r;
        Check(std::fabs(color - .2f) < .001f, "the resolved X wins over the wrong same-name mesh");
        const auto* key = app->AccessoryKeys(0);
        Check(key && (v1 ? key[0].scale == 1.25f && key[0].position[0] == 2.5f
                         : key[0].scale == 2.5f && key[0].position[1] == 2.f),
            "the mixed project keeps the VAC placement on the PMM track");
        const auto* path = reinterpret_cast<const wchar_t*>(accessory->texturePaths[0]);
        const auto* resource = FindTextureResource(app->Renderer(), path);
        Check(fs::path(path) == tree / texture && resource && resource->comObject,
            "the accessory uses the actual texture beside its resolved X");
        auto* image = static_cast<IDirect3DTexture9*>(resource->comObject);
        D3DLOCKED_RECT pixels{};
        Check(SUCCEEDED(image->LockRect(0, &pixels, nullptr, D3DLOCK_READONLY)) && pixels.pBits,
            "read the texture actually used by the mixed project");
        const DWORD rgb = *static_cast<const DWORD*>(pixels.pBits) & 0xffffff;
        Check(SUCCEEDED(image->UnlockRect(0)) && rgb == 0x3288c5,
            "the migrated X material keeps the correct texture pixels");
    };

    if (recovering) {
        Check(fs::is_regular_file(old / fx) && fs::is_regular_file(old / descriptor) &&
              fs::exists(project) == v1 && !fs::exists(sourceEmm) && !fs::exists(moved) &&
              !fs::exists(mixedSaveTarget) && !fs::exists(savedEmm), "own a fresh mixed migration fixture");
        if (v1) {
            Check(LoadSceneFile(project.c_str()), "load the original mixed V1 file while its resources still exist");
            checkResources(old);
        } else {
            LoadModelFile(app, (old / first).c_str(), false);
            LoadModelFile(app, (old / second).c_str(), false);
            LoadAccessoryFile((old / descriptor).c_str());
            checkResources(old);
            app->CameraMode() = 1;
            Check(SaveSceneCopy(app, project.c_str()) && !fs::exists(sourceEmm),
                "save the source PMM through the real writer before seeding the EMM");
        }
        std::ofstream emm(sourceEmm, std::ios::binary);
        emm << "[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = " << (old / first).string()
            << "\r\nPmd2 = " << (old / second).string()
            << "\r\nAcs1 = " << (old / descriptor).string()
            << "\r\n[Effect]\r\nDefault = none\r\nPmd1 = old\\Effects\\Light\\bound.fx"
            << "\r\nPmd2 = old\\Effects\\Light\\bound.fx"
            << "\r\nAcs1 = old\\Effects\\Light\\accessory.fx\r\n";
        emm.close();
        Check(bool(emm), "seed a real relative assignment in the mixed project's source EMM");
        Check(ResetAppState(app) && SetCurrentDirectoryW(root.c_str()),
            "release the mixed source before moving only its fixture root");
        fs::rename(old, moved);
    }
    Check(!fs::exists(old) && fs::is_regular_file(moved / fx), "the old resource root is absent");
    const auto originalProject = read(project), originalEmm = read(sourceEmm);
    Check(originalProject.substr(20, 4) == (v1 ? "0001" : "0002"), "the source uses the requested PMM wire format");
    const auto baselineGreen = recovering ? CountEffectPixels(ReadEffectPixels(app, false), false) : 0;
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize the mixed resource observers");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1"), "observe actual compilation of the relocated FX");
    auto* prompt = reinterpret_cast<void*>(&ShowProjectModelDialog);
    auto* picker = reinterpret_cast<void*>(&GetUiOpenFileNameW);
    auto* savePicker = reinterpret_cast<void*>(&GetUiSaveFileNameW);
    auto* compiler = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
    Install(prompt, reinterpret_cast<void*>(&MixedModelPrompt));
    Install(picker, reinterpret_cast<void*>(&ModelPicker));
    Install(savePicker, reinterpret_cast<void*>(&MixedSavePicker));
    Install(compiler, reinterpret_cast<void*>(&ObserveCompiler), reinterpret_cast<void**>(&nativeCompiler));

    if (recovering) {
        const auto vac = moved / descriptor;
        const auto parked = vac.parent_path() / L"placement.parked";
        Check(!fs::exists(parked), "own the temporary missing-VAC fixture name");
        fs::rename(vac, parked);
        Check(!LoadSceneFile(project.c_str()) && modelPrompts == 1 && pickerCalls == 1 &&
              mixedAccessoryPrompts == 1 && !app->ModelSlot(0) && !app->ModelSlot(1) && !app->AccessorySlot(0),
            "a missing inferred VAC can be canceled after model relocation without leaving a partial project");
        const auto mapping = project_relocation::Ticket::Capture(*app).Infer((old / fx).native()).status;
        const auto cancelledGreen = CountEffectPixels(ReadEffectPixels(app, false), false);
        std::printf("MIXED_VAC cancel mapping=%d green=%zu baseline=%zu compiled=%u accessory_compiled=%u\n",
            static_cast<int>(mapping), static_cast<std::size_t>(cancelledGreen),
            static_cast<std::size_t>(baselineGreen), compiled, alternateEffectCompiles);
        Check(mapping == project_relocation::Status::NoMatch, "cancelling the accessory retires the directory mapping");
        Check(compiled == 0 && alternateEffectCompiles == 0, "MME never compiles either moved FX after cancelling");
        Check(cancelledGreen <= baselineGreen, "the cancelled mixed project adds no effect-coloured pixels");
        fs::rename(parked, vac);
        modelPrompts = pickerCalls = mixedAccessoryPrompts = 0;
        std::puts("PASS mixed VAC cancellation clears the partial scene and leaves no effect mapping");
    }
    const auto& opening = recovering ? project : mixedSaveTarget;
    if (!recovering) Check(read(opening).substr(20, 4) == "0002", "the saved mixed project uses the V2 format");
    Check(LoadSceneFile(opening.c_str()), "open the actual mixed PMM with its legacy MME sidecar");
    const unsigned expectedPicks = recovering ? 1 : 0;
    Check(modelPrompts == expectedPicks && pickerCalls == expectedPicks && mixedAccessoryPrompts == 0,
        "one model selection recovers the mixed project, and the saved pair needs no selection");
    checkResources(moved);
    std::printf("MIXED_VAC phase=%ls scene_loaded=1 source_version=%u\n", phase.data(), v1 ? 1u : 2u);
    const auto pixels = ReadEffectPixels(app, false);
    const auto green = CountEffectPixels(pixels, false);
    const auto magenta = std::count_if(pixels.begin(), pixels.end(), [](DWORD pixel) {
        return ((pixel >> 16) & 0xffu) > 224 && ((pixel >> 8) & 0xffu) < 25 && (pixel & 0xffu) > 224;
    });
    std::printf("MIXED_VAC phase=%ls model_prompts=%u picks=%u compiled=%u green=%zu baseline=%zu accessory_compiled=%u magenta=%zu\n",
        phase.data(), modelPrompts, pickerCalls, compiled, static_cast<std::size_t>(green),
        static_cast<std::size_t>(baselineGreen), alternateEffectCompiles, static_cast<std::size_t>(magenta));
    Check(compiled > 0 && green > baselineGreen + 100 && alternateEffectCompiles > 0 && magenta > 100,
        "both relocated relative effects compile and draw on the mixed project");
    if (recovering) {
        constexpr UINT autoSave = 40022;
        const UINT enabled = GetMenuState(GetMenu(app->MainWindow()), autoSave, MF_BYCOMMAND);
        Check(enabled != UINT(-1), "find the actual provider's save option");
        if (!(enabled & MF_CHECKED)) SendMessageW(app->MainWindow(), WM_COMMAND, autoSave, 0);
        Check(GetMenuState(GetMenu(app->MainWindow()), autoSave, MF_BYCOMMAND) & MF_CHECKED,
            "enable EMM saving before Save As");
        SendMessageW(app->MainWindow(), WM_COMMAND, 208, 0);
        Check(mixedSavePicks == 1 && SceneSaveInProgress(), "the actual Save As starts the background PMM write");
        background_save::Wait(app);
        Check(!SceneSaveInProgress() && fs::path(app->EnvFileName()) == mixedSaveTarget &&
              fs::is_regular_file(mixedSaveTarget), "the recovered scene is saved to the chosen new path");
        EffectPixels(app);
        Check(fs::is_regular_file(savedEmm), "MME writes the sidecar beside the recovered PMM");
        const auto assignment = read(savedEmm);
        Check(assignment.find("bound.fx") != std::string::npos && assignment.find("accessory.fx") != std::string::npos,
            "the recovered EMM carries the model and accessory effect assignments");
    }
    Check(read(project) == originalProject && read(sourceEmm) == originalEmm,
        "recovery, Save As and reopen preserve the original PMM and EMM");
    for (void* hook : {compiler, savePicker, picker, prompt}) {
        Check(MH_DisableHook(hook) == MH_OK && MH_RemoveHook(hook) == MH_OK,
            "remove the mixed resource observer");
    }
    std::printf("PASS real MME mixed model VAC and FX phase=%ls\n", phase.data());
}

void RunSavePair(MMDApp* app, const fs::path& root, const wchar_t* stage, bool timed, bool observeLiveSave) {
    const bool save = std::wcscmp(stage, L"save") == 0;
    Check(save || std::wcscmp(stage, L"reopen") == 0, "run a known EMM save pair phase");
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"), "load the real legacy MME pair");
    const auto folder = root / L"Projects";
    const auto source = folder / L"scene.pmm";
    const auto destination = timed ? root / L"backups/timer_002.pmm" : folder / L"scene_002.pmm";
    const auto sourceEmm = pmm_paths::Effect(source), destinationEmm = pmm_paths::Effect(destination);
    const auto occupied = timed ? root / L"backups/timer_001.emm" : folder / L"scene_001.emm";
    const auto occupiedProject = occupied.parent_path() / (occupied.stem().wstring() + L".pmm");
    const auto model = (folder / L"old/Models/A/first.pmx").make_preferred();
    const auto second = (folder / L"old/Models/B/second.pmx").make_preferred();
    const auto relativeEffect = fs::path(L"old/Effects/Light/bound.fx").make_preferred();
    wantedEffect = folder / relativeEffect;
    const auto read = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        Check(bool(file), "read only an owned project fixture");
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize isolated compile observation");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1"), "verify the exact live FX compilation");
    auto* compiler = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
    Install(compiler, reinterpret_cast<void*>(&ObserveCompiler), reinterpret_cast<void**>(&nativeCompiler));
    app->EnglishUI() = kUiEnglish;
    LocalizeUI(app);
    if (save) {
        Check(!fs::exists(source) && !fs::exists(sourceEmm) && !fs::exists(occupied) &&
              !fs::exists(destination) && !fs::exists(destinationEmm), "own every PMM and EMM test destination");
        LoadModelFile(app, model.c_str(), false);
        Check(app->ModelSlot(0), "load the real source PMX");
        app->CurrentFrame() = 0;
        app->CameraFov() = 31;
        RegisterCameraState(app, 0);
        Check(wcscpy_s(app->EnvFileName(), 256, source.c_str()) == 0 && SaveSceneFile(app),
            "create the baseline with ordinary saving so it starts its own naming series");
        EffectPixels(app); // Consume the baseline notification with the provider's save option off.
        Check(!fs::exists(sourceEmm), "baseline saving has not created any EMM before seeding the source assignment");
        std::ofstream(sourceEmm, std::ios::binary) << "[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = "
            << model.string() << "\r\n[Effect]\r\nDefault = none\r\nPmd1 = "
            << relativeEffect.string() << "\r\n";
        std::ofstream(occupied, std::ios::binary) << "occupied effect companion";
    }
    const auto original = read(source), originalEmm = read(sourceEmm), originalFx = read(wantedEffect);
    const auto baseModel = read(model), occupiedBytes = read(occupied);
    Check(originalEmm.find(relativeEffect.string()) != std::string::npos && occupiedBytes == "occupied effect companion",
        "the source uses a real relative FX and the first number is occupied only by its EMM");
    const auto loadAndRender = [&](const fs::path& path, float fov) {
        Check(LoadSceneFile(path.c_str()) && app->ModelSlot(0) && !app->ModelSlot(1) &&
              fs::path(mdl::Mdl(app->ModelSlot(0))->path) == model && app->CameraKeys()[0].fov == fov,
            "reopen the expected model and camera data through the production PMM entry");
        const auto green = EffectPixels(app);
        std::printf("%s path=%ls fov=%.0f compiled=%u green=%zu\n", timed ? "MME_TIMED_BACKUP" : "MME_INCREMENTAL",
            path.filename().c_str(), fov, compiled, static_cast<std::size_t>(green));
        Check(compiled > 0 && green > 100, "the expected effect compiles and renders on the reopened model");
    };
    if (save) {
        loadAndRender(source, 31);
        constexpr UINT autoSave = 40022;
        const UINT enabled = GetMenuState(GetMenu(app->MainWindow()), autoSave, MF_BYCOMMAND);
        Check(enabled != UINT(-1), "find the actual legacy MME auto-save setting");
        if (!(enabled & MF_CHECKED)) SendMessageW(app->MainWindow(), WM_COMMAND, autoSave, 0);
        Check(GetMenuState(GetMenu(app->MainWindow()), autoSave, MF_BYCOMMAND) & MF_CHECKED,
            "enable the actual provider's EMM save option");
        auto settings = autosave::Current();
        settings.enabled = timed;
        settings.numberedManualVersions = !timed && !observeLiveSave;
        settings.incrementPattern = L"[/NAME]_[/N]";
        if (timed) {
            settings.intervalMinutes = 1;
            settings.directory = destination.parent_path().wstring();
            settings.backupPattern = L"timer_[/N]";
        }
        std::wstring error;
        Check(autosave::Configure(app, settings, error), "set the chosen save policy in the isolated installation");
        app->CurrentFrame() = 0;
        app->CameraFov() = 61;
        RegisterCameraState(app, 0);
        app->SceneModified() = 1;
        if (timed) {
            const auto recent = local_data::File(L"recent-pmm.txt");
            const bool hasRecent = fs::exists(recent);
            const auto recentBytes = hasRecent ? read(recent) : std::string{};
            const auto liveUnchanged = [&] {
                return fs::path(app->EnvFileName()) == source && app->SceneModified() &&
                    app->CameraKeys()[0].fov == 61 && fs::exists(recent) == hasRecent &&
                    (!hasRecent || read(recent) == recentBytes);
            };
            autosave::Initialize(app);
            const auto start = GetTickCount64();
            autosave::Tick(app, start);
            Check(!SceneSaveInProgress() && !fs::exists(destination), "no backup starts before the configured interval");
            const auto lockPath = destination.parent_path() / L".mms-autosave.lock";
            const HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            Check(lock != INVALID_HANDLE_VALUE, "own the fixture's backup-folder lock for a real sharing conflict");
            const auto due = start + 60000;
            autosave::Tick(app, due);
            Check(SceneSaveInProgress(), "the due timer starts the production worker even with the folder lock held");
            background_save::Wait(app);
            const std::wstring failure = background_save::StatusText(app);
            EffectPixels(app);
            Check(!SceneSaveInProgress() && failure == L"Backup failed. Check the destination." && liveUnchanged() &&
                !fs::exists(destination) && !fs::exists(destinationEmm) && read(occupied) == occupiedBytes,
                "locked backup leaves no new PMM or EMM and preserves the active project");
            std::printf("MME_TIMED_BACKUP failed status=%ls active=%ls dirty=%d\n", failure.c_str(),
                app->EnvFileName(), int(app->SceneModified()));
            Check(CloseHandle(lock) && !fs::exists(lockPath), "release only the owned fixture lock");
            autosave::Tick(app, due + 60000);
            Check(SceneSaveInProgress(), "the next due timer retries through the real backup worker");
            background_save::Wait(app);
            Check(!SceneSaveInProgress() && std::wstring(background_save::StatusText(app)) == L"Backup saved" &&
                liveUnchanged(), "completed backup leaves the active path, camera edit, dirty flag and recents unchanged");
            std::printf("MME_TIMED_BACKUP saved status=%ls active=%ls dirty=%d\n", background_save::StatusText(app),
                app->EnvFileName(), int(app->SceneModified()));
            autosave::Shutdown();
        } else if (observeLiveSave) {
            // Hold the production writer at the first callback after CaptureScene.
            // The UI thread remains free to load a model and present the new scene.
            const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Check(release != nullptr, "create a gate owned by this fixture");
            bool completed = false;
            bool diskSaved = false;
            bool emmAtCompletion = false;
            background_save::Options options;
            options.beforeWrite = [release] { return WaitForSingleObject(release, 15000) == WAIT_OBJECT_0; };
            options.completed = [&](bool ok) {
                completed = true;
                diskSaved = ok;
                emmAtCompletion = fs::exists(destinationEmm);
            };
            const auto capturedGeneration = app->SceneGeneration();
            const auto capturedRevision = app->SceneRevision();
            Check(background_save::Request(app, destination.c_str(), std::move(options)) &&
                  SceneSaveInProgress() && !fs::exists(destination),
                "freeze the one-model PMM and hold its worker before publication");
            LoadModelFile(app, second.c_str(), false);
            Check(app->ModelSlot(0) && app->ModelSlot(1) && !app->ModelSlot(2) &&
                  fs::path(mdl::Mdl(app->ModelSlot(1))->path) == second &&
                  app->SceneGeneration() == capturedGeneration,
                "the live project gains a second model without replacing the scene");
            app->CameraFov() = 71;
            RegisterCameraState(app, 0);
            app->SceneModified() = 1;
            EffectPixels(app);
            Check(app->SceneRevision() != capturedRevision && app->CameraKeys()[0].fov == 71 &&
                  !completed && !fs::exists(destination) && !fs::exists(destinationEmm),
                "render the newer revision while the captured PMM is still held");
            Check(background_save::DeferClose(app), "record a close requested while the save is pending");
            Check(SetEvent(release), "let the worker publish the frozen PMM");
            background_save::Wait(app);
            Check(CloseHandle(release), "close the gate after the joined worker");
            Check(completed && diskSaved && !emmAtCompletion && !fs::exists(destinationEmm) &&
                  fs::is_regular_file(destination) && !SceneSaveInProgress() && app->SceneModified() &&
                  fs::path(app->EnvFileName()) == destination,
                "PMM completion reports saved with new edits dirty and no EMM on disk yet");
            MSG pendingClose{};
            Check(!PeekMessageW(&pendingClose, app->MainWindow(), WM_CLOSE, WM_CLOSE, PM_NOREMOVE),
                "an incomplete effect save keeps the window open");
            Check(std::wstring_view(background_save::StatusText(app)) ==
                  L"PMM saved. EMM not updated because the scene changed during save.",
                "the completion status explicitly reports the missing effect update");
            std::printf("MME_LIVE_SAVE completed=%d emm_at_completion=%d captured_revision=%llu live_revision=%llu\n",
                diskSaved, emmAtCompletion, static_cast<unsigned long long>(capturedRevision),
                static_cast<unsigned long long>(app->SceneRevision()));
        } else {
            SendMessageW(app->MainWindow(), WM_COMMAND, 207, 0);
            Check(SceneSaveInProgress(), "the actual Save menu starts the numbered background save");
            background_save::Wait(app);
            std::printf("MME_INCREMENTAL save target=%ls dirty=%d status=%ls\n", app->EnvFileName(),
                int(app->SceneModified()), background_save::StatusText(app));
            Check(!SceneSaveInProgress() && !app->SceneModified() && fs::path(app->EnvFileName()) == destination,
                "manual incremental save updates the active project to number two");
        }
        Check(fs::is_regular_file(destination) && !fs::exists(occupiedProject),
            "publish number two without creating a project beside the number-one EMM");
        EffectPixels(app); // Render frames consume the legacy provider notification.
        if (observeLiveSave) {
            Check(!fs::exists(destinationEmm), "rendering cannot publish a companion from the newer scene");
            std::puts("MME_LIVE_SAVE stale_companion_skipped=1");
        } else {
            Check(fs::is_regular_file(destinationEmm) && read(destinationEmm).find(".fx") != std::string::npos,
                "the real MME provider writes the companion for the actual numbered target");
        }
    } else {
        if (observeLiveSave) {
            Check(LoadSceneFile(destination.c_str()) && app->ModelSlot(0) && !app->ModelSlot(1) &&
                  fs::path(mdl::Mdl(app->ModelSlot(0))->path) == model && app->CameraKeys()[0].fov == 61 &&
                  !fs::exists(destinationEmm), "reopen the captured PMM without a mismatched companion");
            std::puts("MME_LIVE_SAVE reopened_pmm_models=first.pmx fov=61");
        } else {
            loadAndRender(destination, 61);
        }
        loadAndRender(source, 31);
    }
    Check(read(source) == original && read(sourceEmm) == originalEmm && read(wantedEffect) == originalFx &&
          read(occupied) == occupiedBytes && read(model) == baseModel,
        "the original pair, occupied EMM and effect assets remain byte-identical");
    MH_DisableHook(compiler);
    MH_RemoveHook(compiler);
    const char* operation = observeLiveSave ? "live save protection" : timed ? "timed backup" : "incremental";
    std::printf("PASS real MME %s pair phase=%ls\n", operation, stage);
}

BOOL PickVersionedProject(LPOPENFILENAMEW picker) {
    ++pickerCalls;
    Check(picker && picker->lpstrFilter && picker->lpstrFile &&
          (picker->Flags & OFN_FILEMUSTEXIST), "the actual Open command asks for an existing project");
    const auto* pattern = picker->lpstrFilter + std::wcslen(picker->lpstrFilter) + 1;
    Check(std::wcscmp(pattern, L"*.pmm*") == 0 &&
          wcscpy_s(picker->lpstrFile, picker->nMaxFile, selected.c_str()) == 0,
          "the project picker includes backup extensions and returns the exact fixture path");
    return TRUE;
}

void DropVersionedProject(HWND main, const fs::path& path) {
    const auto size = sizeof(DROPFILES) + (path.native().size() + 2) * sizeof(wchar_t);
    const HGLOBAL drop = GlobalAlloc(GHND, size);
    Check(drop != nullptr, "allocate an owned Unicode file drop");
    auto* payload = static_cast<DROPFILES*>(GlobalLock(drop));
    Check(payload != nullptr, "lock the file-drop payload");
    payload->pFiles = sizeof(DROPFILES);
    payload->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(payload) + sizeof(DROPFILES), path.c_str(),
                (path.native().size() + 1) * sizeof(wchar_t));
    GlobalUnlock(drop);
    SendMessageW(main, WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0);
}

UINT VersionedRecentCommand(HWND main, const fs::path& path) {
    SendMessageW(main, WM_ENTERMENULOOP, FALSE, 0);
    const auto file = GetSubMenu(GetMenu(main), 0);
    HMENU recent = nullptr;
    for (int i = 0; i < GetMenuItemCount(file); ++i) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_SUBMENU;
        if (GetMenuItemInfoW(file, i, TRUE, &item) && item.wID == 205) {
            recent = item.hSubMenu;
            break;
        }
    }
    Check(recent != nullptr, "the full host has the actual Open submenu for recent projects");
    SendMessageW(main, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(recent), 0);
    UINT command = 0;
    for (int i = 0; i < GetMenuItemCount(recent); ++i) {
        wchar_t caption[512]{};
        GetMenuStringW(recent, i, caption, std::size(caption), MF_BYPOSITION);
        const std::wstring_view label = caption;
        const std::wstring_view target = path.native();
        if (label.size() > target.size() && label.substr(label.size() - target.size()) == target) {
            command = GetMenuItemID(recent, i);
            const auto state = GetMenuState(recent, i, MF_BYPOSITION);
            Check(command && command != UINT(-1) && !(state & (MF_DISABLED | MF_GRAYED)),
                  "the versioned recent entry is actionable");
            break;
        }
    }
    SendMessageW(main, WM_EXITMENULOOP, FALSE, 0);
    Check(command != 0, "the recent menu keeps the full path and backup extension");
    return command;
}

void RunVersionEntries(MMDApp* app, const fs::path& root) {
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
          "versioned project entry test uses the real legacy MME pair");
    const auto folder = root / L"Projects";
    const auto recent = root / L"test-recents.txt";
    const auto model = (folder / L"old/Models/A/first.pmx").make_preferred();
    const auto greenEffect = (folder / L"old/Effects/Light/bound.fx").make_preferred();
    const auto redEffect = root / L"original-control.fx";
    Check(SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", recent.c_str()) && !fs::exists(recent),
          "record recents only inside this isolated installation");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1"), "use real effect compilation");
    app->EnglishUI() = kUiEnglish;
    LocalizeUI(app);
    LoadModelFile(app, model.c_str(), false);
    Check(app->ModelSlot(0) && fs::is_regular_file(greenEffect) && fs::is_regular_file(redEffect),
          "start from the actual fixture model and two different effects");

    struct Version {
        fs::path project;
        fs::path effect;
        float fov;
        bool red;
    };
    const std::array<Version, 3> versions{{
        {folder / L"scene.pmm", {}, 31.f, false},
        {folder / L"scene.pmm1", redEffect, 51.f, true},
        {folder / L"scene.pmm11", greenEffect, 61.f, false},
    }};
    const auto read = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        Check(bool(file), "read an owned versioned project, companion or asset");
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    std::vector<std::pair<fs::path, std::string>> inputs;
    for (const auto& v : versions) {
        const auto emm = pmm_paths::Effect(v.project);
        Check(!fs::exists(v.project) && !fs::exists(emm), "only seed new PMM and EMM fixture files");
        app->CurrentFrame() = 0;
        app->CameraFov() = v.fov;
        RegisterCameraState(app, 0);
        Check(SaveSceneCopy(app, v.project.c_str()), "write each generation through the real PMM writer");
        std::ofstream output(emm, std::ios::binary);
        output << "[Info]\r\nVersion = 3\r\n[Object]\r\nPmd1 = " << model.string()
               << "\r\n[Effect]\r\nDefault = none\r\nPmd1 = "
               << (v.effect.empty() ? "none" : v.effect.string()) << "\r\n";
        output.close();
        Check(bool(output), "write a distinct companion, including the unnumbered decoy");
        inputs.emplace_back(v.project, read(v.project));
        inputs.emplace_back(emm, read(emm));
    }
    for (const auto& path : {model, redEffect, greenEffect}) inputs.emplace_back(path, read(path));
    app->SceneModified() = 0;
    const auto main = app->MainWindow();
    const auto mh = MH_Initialize();
    Check(mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED, "initialize the test-only Open picker hook");
    auto* picker = reinterpret_cast<void*>(&GetUiOpenFileNameW);
    Install(picker, reinterpret_cast<void*>(&PickVersionedProject));
    const auto verify = [&](const Version& v, const char* entry) {
        Check(fs::path(app->EnvFileName()) == v.project && app->ModelSlot(0) && !app->ModelSlot(1) &&
              fs::path(mdl::Mdl(app->ModelSlot(0))->path) == model && app->CameraKeys()[0].fov == v.fov,
              "the entry opens the correct PMM generation and camera data");
        const auto pixels = ReadEffectPixels(app);
        const auto red = CountEffectPixels(pixels, true);
        const auto green = CountEffectPixels(pixels, false);
        std::printf("VERSION_ENTRY entry=%s pmm=%ls emm=%ls fov=%.0f red=%zu green=%zu\n", entry,
            v.project.filename().c_str(), pmm_paths::Effect(v.project).filename().c_str(), v.fov,
            static_cast<std::size_t>(red), static_cast<std::size_t>(green));
        Check((v.red ? red : green) > 100 && (v.red ? green : red) == 0,
              "real MME renders this generation's effect and no other generation's effect");
    };
    for (std::size_t i = 1; i < versions.size(); ++i) {
        selected = versions[i].project;
        SendMessageW(main, WM_COMMAND, 205, 0);
        verify(versions[i], "open");
    }
    for (std::size_t i = 1; i < versions.size(); ++i) {
        DropVersionedProject(main, versions[i].project);
        verify(versions[i], "drop");
    }
#if !MMD_ORIGINAL_PROFILE
    for (std::size_t i = 1; i < versions.size(); ++i) {
        SendMessageW(main, WM_COMMAND, VersionedRecentCommand(main, versions[i].project), 0);
        verify(versions[i], "recent");
    }
#endif
    Check(pickerCalls == 2, "only the two File Open actions invoke the picker");
    for (const auto& [path, bytes] : inputs)
        Check(read(path) == bytes, "opening backup generations leaves all source PMMs, EMMs and assets unchanged");

    const auto& edited = versions[1];
    const auto editedEmm = pmm_paths::Effect(edited.project);
    const auto oldEmm = read(editedEmm);
    const auto oldEmmTime = fs::last_write_time(editedEmm);
    DropVersionedProject(main, edited.project);
    Check(fs::path(app->EnvFileName()) == edited.project && EffectPixels(app, true) > 100,
          "reopen the generation to be edited with its red effect active");
    constexpr UINT autoSave = 40022;
    const auto saveState = GetMenuState(GetMenu(main), autoSave, MF_BYCOMMAND);
    Check(saveState != UINT(-1), "find the real MME auto-save option");
    if (!(saveState & MF_CHECKED)) SendMessageW(main, WM_COMMAND, autoSave, 0);
    Check(GetMenuState(GetMenu(main), autoSave, MF_BYCOMMAND) & MF_CHECKED,
          "enable real MME companion saving for this fixture");
    auto settings = autosave::Current();
    settings.enabled = false;
    settings.keepPrevious = false;
    settings.numberedManualVersions = false;
    std::wstring error;
    Check(autosave::Configure(app, settings, error), "save this generation in place without rotating the fixture");
    app->CurrentFrame() = 0;
    app->CameraFov() = 52.f;
    RegisterCameraState(app, 0);
    app->SceneModified() = 1;
    SendMessageW(main, WM_COMMAND, 207, 0);
    background_save::Wait(app);
    Check(!app->SceneModified() && fs::path(app->EnvFileName()) == edited.project,
          "the actual Save command writes the open PMM1 in place");
    EffectPixels(app, true); // Consume the legacy save notification on real frames.
    const auto currentEmm = read(editedEmm);
    Check((currentEmm != oldEmm || fs::last_write_time(editedEmm) != oldEmmTime) &&
          currentEmm.find(edited.effect.filename().string()) != std::string::npos,
          "real MME writes the red mapping to the matching EMM1");
    for (const auto& [path, bytes] : inputs) {
        if (path != edited.project && path != editedEmm)
            Check(read(path) == bytes, "saving PMM1 does not change other generations or assets");
    }
    DropVersionedProject(main, edited.project);
    const Version updated{edited.project, edited.effect, 52.f, true};
    verify(updated, "save-reopen");
    DropVersionedProject(main, versions[2].project);
    verify(versions[2], "adjacent-after-save");
    MH_DisableHook(picker);
    MH_RemoveHook(picker);
    std::puts("PASS versioned PMM/EMM through production project entries");
}
} // namespace mme_project_relocation_checks
