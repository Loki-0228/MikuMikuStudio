// Included by pmm_input_host_tests.cpp. Use the production PMM writer so
// both skip paths must consume the same model records as an ordinary open.
void V2RelocationCases(const fs::path& emptyV1, const fs::path& emptyV2,
                       const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto root = directory / (L"v2rel-" + std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(root), "own a fresh V2 relocation fixture");
    const auto old = root / L"old", good = root / L"new" / L"UserFile" / L"r", wrong = root / L"wrong";
    const auto wrongAgain = root / L"wrong-again";
    const auto projectDir = root / L"UserFile";
    const auto first = projectDir / L"first.pmm", second = projectDir / L"second.pmm";
    const auto file = [](const fs::path& base, unsigned i) {
        return base / std::wstring(1, static_cast<wchar_t>(L'A' + i)) / L"v2rel.pmd";
    };
    const auto write = [](const fs::path& path, unsigned vertices, const char* name, bool mismatch) {
        fs::create_directories(path.parent_path());
        Check(!fs::exists(path) && path.native().size() < 256, "bounded fresh V2 model path");
        auto pmd = geometry_test_reference::Pmd(vertices, true);
        // PMD header = 3-byte magic, float version and a 20-byte model name.
        std::memset(pmd.bytes.data() + 7, 0, 20);
        std::memcpy(pmd.bytes.data() + 7, name, std::strlen(name));
        if (mismatch) {
            constexpr char bone[] = "root";
            const auto begin = pmd.bytes.begin() + pmd.indexEnd;
            const auto pos = std::search(begin, pmd.bytes.end(), bone, bone + sizeof bone);
            Check(pos != pmd.bytes.end() &&
                  std::search(pos + sizeof bone, pmd.bytes.end(), bone, bone + sizeof bone) == pmd.bytes.end(),
                  "wrong candidate has one identifiable fixture bone");
            std::memcpy(&*pos, "fake", sizeof bone);
        }
        Wire wire; wire.bytes = std::move(pmd.bytes); wire.Write(path);
    };
    const char* names[] = {"rel-A", "rel-B", "rel-C", "rel-D"};
    std::vector<fs::path> originals;
    for (unsigned i = 0; i < 4; ++i) {
        originals.push_back(file(old, i));
        write(originals.back(), 3 * (i + 1), names[i], false);
    }
    const auto seed = root / L"seed.pmm";
    WriteV1ModelProject(seed, emptyV1, originals);
    const auto language = app->EnglishUI();
    app->EnglishUI() = 1;
    Normal(seed, camera, light);
    Check(fs::create_directory(projectDir), "own V2 fixture projects directory");
#if !MMD_ORIGINAL_PROFILE
    constexpr char projectMarker[] = "test.mms.v2_relocation.project_setting";
#endif
    for (const auto& path : {first, second}) {
#if !MMD_ORIGINAL_PROFILE
        pmm_extension::Set(app->PmmProjectSettings().document, projectMarker,
                           {static_cast<std::uint8_t>(path == first ? 0x41 : 0x42)});
#endif
        Check(SaveSceneCopy(app, path.c_str()), "real writer saves four source records for V2 recovery");
        const auto bytes = Bytes(path);
        Check(bytes.size() > 30 && !std::memcmp(bytes.data(), "Polygon Movie maker 0002", 24),
              "exercise actual standard V2 model records");
    }
    Normal(emptyV2, camera, light);
    for (unsigned i : {0u, 1u, 3u}) {
        const auto target = file(good, i);
        fs::create_directories(target.parent_path());
        fs::rename(originals[i], target); // only the PMDs created above
        Check(!fs::exists(originals[i]) && fs::is_regular_file(target), "fixture model actually moved");
    }
    write(file(good, 2), 21, "decoy-C", false); // C must continue to load from old.
    write(file(wrong, 0), 24, "wrong-A", true);
    write(file(wrongAgain, 0), 45, "wrong-again", true);
    for (unsigned i : {1u, 3u})
        write(file(wrongAgain, i), 3 * (i + 1), names[i], false);
    write(file(wrong, 1), 27, "wrong-B", true);
    write(file(wrong, 3), 30, "wrong-D", false); // would load if skipped B published its choice.
    const auto unavailable = root / L"unavailable";
    write(file(unavailable, 0), 36, "unavailable-A", false);
    write(file(unavailable, 1), 39, "unavailable-B", false); // never learn the unavailable candidate's root.
    write(file(unavailable, 3), 42, "unavailable-D", false);
    for (unsigned i : {0u, 1u, 3u}) write(file(projectDir / L"r", i), 33, "decoy", false);

    const auto verify = [&](unsigned slot, const fs::path& alternative = {}) {
        const auto expected = slot == 2 ? originals[slot]
            : file(alternative.empty() ? good : alternative, slot);
        auto raw = app->ModelSlot(slot);
        Check(raw && expected.native() == mdl::Mdl(raw)->path && mdl::Mdl(raw)->vertexCount == 3 * (slot + 1) &&
              !std::strcmp(mdl::Mdl(raw)->name, names[slot]) && mdl::Mdl(raw)->boneCount == 1 &&
              mdl::BoneKeys(raw) && mdl::BoneKeys(raw)[0].position[0] == float(10 + slot),
              "V2 retained exact model file, geometry, name and the record's bone key");
    };
    const auto expectedRoot = root.native() + L"\\";
    const auto originalModelDirectory = projectDir.native();
    const std::wstring savedModelDirectory = app->DirModel();
    const auto load = [&](const fs::path& path, std::vector<V2RelocationReply> replies,
                          const fs::path& accepted, bool legacy = false, const fs::path& corrupt = {}) {
        constexpr UINT rememberFolder = 0x12D;
        const auto previousCheck = CheckMenuItem(GetMenu(app->MainWindow()), rememberFolder, MF_BYCOMMAND | MF_CHECKED);
        Check(previousCheck != UINT(-1), "enable the real remember-folder option for V2 model recovery");
        CopyDirPathW(app->DirModel(), originalModelDirectory.c_str());
        const auto acceptedDirectory = accepted.empty() ? fs::path(originalModelDirectory) : accepted.parent_path();
        const auto disappearances = std::count_if(replies.begin(), replies.end(),
            [](const auto& reply) { return reply.disappearsOnOpen; });
        V2RelocationDialogs dialogs{std::move(replies)};
        if (!legacy) dialogs.projectRoot = expectedRoot.c_str();
        dialogs.originalModelDirectory = originalModelDirectory.c_str();
        dialogs.expectedPickerDirectory = originalModelDirectory.c_str();
        unsigned errors = 0;
        if (!corrupt.empty()) {
            dialogs.expectBadModel = true;
            scopedProjectOpenNotice = [&](UiMessage message, const wchar_t* detail) {
                Check(message == UiMessage::OpenModelError && detail && fs::path(detail) == corrupt &&
                      fs::is_regular_file(corrupt) && dialogs.sawBadModel && monitor && inputOpen,
                      "a present invalid replacement still reports the exact model-open error");
                ++errors;
                faultThrown = true; // Classify the real reset in the original-profile observer.
                return IDOK;
            };
        }
        v2Relocation = &dialogs;
        if (accepted.empty()) {
            const auto sourceBytes = Bytes(path);
            const auto tails = tailCalls;
            Arm(path, Fault::None, false);
            Finish(LoadSceneFile(path.c_str()), false);
            Check(tailCalls == tails && !RelocationTicket::Capture(*app).Current(*app) &&
                  Bytes(path) == sourceBytes, "an aborted reselection publishes no scene, mapping or source edit");
        } else if (legacy) {
            const auto sourceBytes = Bytes(path);
            LegacyRelocationOpen(path, false);
            Check(Bytes(path) == sourceBytes, "legacy model reselection leaves the source PMM unchanged");
        } else {
            Normal(path, camera, light);
        }
        v2Relocation = nullptr;
        scopedProjectOpenNotice = nullptr;
        Check(errors == (corrupt.empty() ? 0u : 1u), "only the present invalid replacement shows the load error");
        CheckMenuItem(GetMenu(app->MainWindow()), rememberFolder, MF_BYCOMMAND | previousCheck);
        Check(!dialogs.awaitingPicker && dialogs.next == dialogs.replies.size() &&
              dialogs.disappearingCandidate.empty() && dialogs.parkedCandidate.empty() &&
              !dialogs.retainedModel && !dialogs.retainedBoneKeys && dialogs.disappearedOnOpen == static_cast<unsigned>(disappearances),
              "consumed exactly the expected V2 decisions and real model open failures");
        Check(legacy || accepted.empty() || !std::wcscmp(app->PathWorkspace().projectDirectory, expectedRoot.c_str()),
            "the V2 model recovery leaves the actual PMM resource root intact");
        Check(!std::wcscmp(app->DirModel(), acceptedDirectory.c_str()),
            "only the accepted replacement changes the model picker folder");
#if !MMD_ORIGINAL_PROFILE
        const auto* marker = pmm_extension::Find(app->PmmProjectSettings().document, projectMarker);
        const auto expected = static_cast<std::uint8_t>(path == first ? 0x41 : 0x42);
        Check(accepted.empty() ? marker == nullptr
                              : marker && marker->value.size() == 1 && marker->value.front() == expected,
              "reselection neither loses this project's settings nor publishes settings for an aborted open");
#endif
    };
    constexpr auto missing = ProjectModelIssue::MissingFile;
    constexpr auto mismatch = ProjectModelIssue::StructureMismatch;
    const auto savedMode = pmm_io::bufferedReads;
    const auto invalidReplacement = unavailable / L"v2-invalid.pmd";
    std::ofstream(invalidReplacement, std::ios::binary) << "not a PMD";
    const auto fx = old / L"Effects" / L"row.fx", newFx = good / L"Effects" / L"row.fx";
    for (bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered;
        std::printf("STAGE V2 relocation mode=%s retry a disappeared selection\n", buffered ? "buffered" : "direct");
        load(first, {{missing, L"rel-A", 1, file(unavailable, 0), true},
                     {missing, L"rel-A", 1, file(good, 0)}}, file(good, 0));
        for (unsigned i = 0; i < 4; ++i) verify(i);
        const auto captured = RelocationTicket::Capture(*app);
        Check(captured.Infer(fx.native()).path == newFx.native(), "same-generation consumer sees real V2 model mapping");

        std::puts("STAGE V2 relocation retries after a structure replacement disappears");
        load(second, {{missing, L"rel-A", 1, file(wrong, 0)},
                      {mismatch, L"wrong-A", 3, file(wrongAgain, 0)},
                      {mismatch, L"wrong-again", 3, file(unavailable, 0), true},
                      {mismatch, L"wrong-again", 3, file(good, 0)}}, file(good, 0));
        CheckRetired(captured);
        Check(RelocationTicket::Capture(*app).Infer(fx.native()).path == newFx.native(),
              "stale write does not disturb second project's finally accepted model mapping");
        for (unsigned i = 0; i < 4; ++i) verify(i);

        std::puts("STAGE V2 relocation skip before and after names/counts");
        // Direct: skip in both dialogs. Buffered: close each corresponding
        // picker. D must ask even though wrong/D and unavailable/D are readable.
        load(first, {{missing, L"rel-A", 1, file(unavailable, 0), true},
                     {missing, L"rel-A", buffered ? 1 : 3, {}},
                     {missing, L"rel-B", 1, file(wrong, 1)},
                     {mismatch, L"wrong-B", 3, file(unavailable, 1), true, 1},
                     {mismatch, L"wrong-B", buffered ? 3 : 4, {}},
                     {missing, L"rel-D", 1, file(good, 3)}}, file(good, 3));
        Check(!app->ModelSlot(0) && !app->ModelSlot(1), "later acceptance leaves both skipped V2 slots empty");
        verify(2); verify(3);

        if (buffered) {
            std::puts("STAGE V2 structure recovery can adapt the retained model");
            load(second, {{missing, L"rel-A", 1, file(wrongAgain, 0)},
                          {mismatch, L"wrong-again", 3, file(unavailable, 0), true},
                          {mismatch, L"wrong-again", 1, {}}}, file(wrongAgain, 0));
            const auto* retained = app->ModelSlot(0);
            Check(retained && file(wrongAgain, 0).native() == mdl::Mdl(retained)->path &&
                  mdl::Mdl(retained)->vertexCount == 45 && mdl::Mdl(retained)->boneCount == 1 &&
                  !std::strcmp(mdl::Mdl(retained)->name, "wrong-again"),
                  "continue keeps the last loaded mismatched model instead of the disappeared selection");
            for (unsigned i : {1u, 2u, 3u}) verify(i, wrongAgain);
            Check(RelocationTicket::Capture(*app).Infer(fx.native()).path ==
                  (wrongAgain / L"Effects" / L"row.fx").native(),
                  "only the adapted model teaches its relocation to later records");
        }

        std::puts(buffered ? "STAGE V2 present invalid structure replacement stays an error"
                           : "STAGE V2 structure recovery can cancel after disappearance");
        load(second, {{missing, L"rel-A", 1, file(wrongAgain, 0)},
                      {mismatch, L"wrong-again", 3, file(unavailable, 0), true},
                      {mismatch, L"wrong-again", buffered ? 3 : 5, buffered ? invalidReplacement : fs::path{}}},
             {}, false, buffered ? invalidReplacement : fs::path{});
    }
    std::puts("PASS PMM V2 structure reselection retries, adapts, skips and cancels after selected-file disappearance");
    pmm_io::bufferedReads = false;
    load(first, {{missing, L"rel-A", 1, file(unavailable, 0), true},
                 {missing, L"rel-A", 1, file(wrong, 0)},
                 {mismatch, L"wrong-A", 3, file(unavailable, 0), true},
                 {mismatch, L"wrong-A", 3, file(good, 0)}}, file(good, 0), true);
    std::puts("PASS PMM V2 legacy entry recovers both initial and structure replacement disappearance");
    Check(RelocationTicket::Capture(*app).Infer(file(old, 1).native()).path == file(good, 1).native(),
          "direct V2 legacy success retains the mapping after its wrapper returns");
    pmm_io::bufferedReads = true; // Keep the existing PMX rejection case in buffered mode.
    // Selecting a real PMX with a partial zero joint count must fail before
    // the replacement reaches PMM structure reconciliation or teaches a root.
    const auto truncatedPath = wrong / L"A" / L"v2rel.pmx";
    auto truncated = geometry_test_reference::Pmx(3, 0, 1, 1, true);
    truncated.bytes.resize(truncated.bytes.size() - 2); // two of four count bytes remain
    Wire{truncated.bytes}.Write(truncatedPath);
    const auto sourceBytes = Bytes(first);
    const std::wstring pickerBefore = app->DirModel();
    const auto tails = tailCalls;
    const auto stores = model_geometry_cache::GetProcessStatistics().stores;
    unsigned errors = 0;
    V2RelocationDialogs rejected{{{missing, L"rel-A", 1, truncatedPath}}};
    scopedProjectOpenNotice = [&](UiMessage message, const wchar_t* detail) {
        Check(message == UiMessage::OpenModelError && detail && fs::path(detail) == truncatedPath &&
              monitor && inputOpen, "real selected-model error identifies the truncated PMX");
        const auto* partial = app->ModelSlot(0);
        // loadComplete is visibility and ModelInitDefaults starts it at one.
        // Animation pools and key indices appear only after the PMX tail succeeds.
        Check(partial && mdl::Mdl(partial)->boneCount == 1 && !mdl::Mdl(partial)->boneKeys &&
              !mdl::Mdl(partial)->morphKeys && !mdl::Mdl(partial)->displayKeys &&
              !mdl::Mdl(partial)->boneKeyIndices && !mdl::Mdl(partial)->morphKeyIndices &&
              !RelocationTicket::Capture(*app).Current(*app),
              "late PMX failure does not complete the candidate or publish a relocation owner");
        ++errors;
        faultThrown = true; // Classify the real abort refresh in the original-profile observer.
        return IDOK;
    };
    v2Relocation = &rejected;
    Arm(first, Fault::None, false);
    const bool loaded = LoadSceneFile(first.c_str());
    v2Relocation = nullptr;
    scopedProjectOpenNotice = nullptr;
    Finish(loaded, false);
    const auto after = model_geometry_cache::GetProcessStatistics();
    Check(errors == 1 && rejected.next == 1 && !rejected.awaitingPicker && tailCalls == tails &&
          !app->sceneRecoveryPending && app->DirModel() == pickerBefore &&
          !RelocationTicket::Capture(*app).Current(*app) && after.stores == stores && !after.pendingBytes &&
          Bytes(first) == sourceBytes && Bytes(truncatedPath) == truncated.bytes,
          "rejected selected PMX leaves no model, mapping, cache publication or source edit");
    std::puts("PASS PMM v2 selected PMX rejects partial zero joint count");
    CopyDirPathW(app->DirModel(), savedModelDirectory.c_str());
    pmm_io::bufferedReads = savedMode;
    app->EnglishUI() = language;
    Normal(emptyV2, camera, light);
}
