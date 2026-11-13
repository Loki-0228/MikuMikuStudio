// Included by pmm_input_host_tests.cpp. Use the production PMM writer so
// both skip paths must consume the same model records as an ordinary open.
void V2RelocationCases(const fs::path& emptyV1, const fs::path& emptyV2,
                       const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto root = directory / (L"v2rel-" + std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(root), "own a fresh V2 relocation fixture");
    const auto old = root / L"old", good = root / L"new" / L"UserFile" / L"r", wrong = root / L"wrong";
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
    for (const auto& path : {first, second}) {
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
    write(file(wrong, 1), 27, "wrong-B", true);
    write(file(wrong, 3), 30, "wrong-D", false); // would load if skipped B published its choice.
    for (unsigned i : {0u, 1u, 3u}) write(file(projectDir / L"r", i), 33, "decoy", false);

    const auto verify = [&](unsigned slot) {
        const auto expected = slot == 2 ? originals[slot] : file(good, slot);
        auto raw = app->ModelSlot(slot);
        Check(raw && expected.native() == mdl::Mdl(raw)->path && mdl::Mdl(raw)->vertexCount == 3 * (slot + 1) &&
              !std::strcmp(mdl::Mdl(raw)->name, names[slot]) && mdl::Mdl(raw)->boneCount == 1 &&
              mdl::BoneKeys(raw) && mdl::BoneKeys(raw)[0].position[0] == float(10 + slot),
              "V2 retained exact model file, geometry, name and the record's bone key");
    };
    const auto expectedRoot = root.native() + L"\\";
    const auto originalModelDirectory = projectDir.native();
    const std::wstring savedModelDirectory = app->DirModel();
    const auto load = [&](const fs::path& path, std::vector<V2RelocationReply> replies, bool legacy = false) {
        constexpr UINT rememberFolder = 0x12D;
        const auto previousCheck = CheckMenuItem(GetMenu(app->MainWindow()), rememberFolder, MF_BYCOMMAND | MF_CHECKED);
        Check(previousCheck != UINT(-1), "enable the real remember-folder option for V2 model recovery");
        CopyDirPathW(app->DirModel(), originalModelDirectory.c_str());
        const auto acceptedDirectory = replies.back().selection.parent_path();
        V2RelocationDialogs dialogs{std::move(replies)};
        if (!legacy) dialogs.projectRoot = expectedRoot.c_str();
        dialogs.originalModelDirectory = originalModelDirectory.c_str();
        v2Relocation = &dialogs;
        if (legacy) LegacyRelocationOpen(path, false); else Normal(path, camera, light);
        v2Relocation = nullptr;
        CheckMenuItem(GetMenu(app->MainWindow()), rememberFolder, MF_BYCOMMAND | previousCheck);
        Check(!dialogs.awaitingPicker && dialogs.next == dialogs.replies.size(), "consumed exactly the expected V2 decisions");
        Check(legacy || !std::wcscmp(app->PathWorkspace().projectDirectory, expectedRoot.c_str()),
            "the V2 model recovery leaves the actual PMM resource root intact");
        Check(!std::wcscmp(app->DirModel(), acceptedDirectory.c_str()),
            "only the accepted replacement changes the model picker folder");
    };
    constexpr auto missing = ProjectModelIssue::MissingFile;
    constexpr auto mismatch = ProjectModelIssue::StructureMismatch;
    const auto savedMode = pmm_io::bufferedReads;
    for (bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered;
        std::printf("STAGE V2 relocation mode=%s one selection\n", buffered ? "buffered" : "direct");
        load(first, {{missing, L"rel-A", 1, file(good, 0)}});
        for (unsigned i = 0; i < 4; ++i) verify(i);
        const auto fx = old / L"Effects" / L"row.fx", newFx = good / L"Effects" / L"row.fx";
        const auto captured = RelocationTicket::Capture(*app);
        Check(captured.Infer(fx.native()).path == newFx.native(), "same-generation consumer sees real V2 model mapping");

        std::puts("STAGE V2 relocation second project and structure reselection");
        load(second, {{missing, L"rel-A", 1, file(wrong, 0)}, {mismatch, L"wrong-A", 3, file(good, 0)}});
        CheckRetired(captured);
        Check(RelocationTicket::Capture(*app).Infer(fx.native()).path == newFx.native(),
              "stale write does not disturb second project's finally accepted model mapping");
        for (unsigned i = 0; i < 4; ++i) verify(i);

        std::puts("STAGE V2 relocation skip before and after names/counts");
        // Direct: skip in both dialogs. Buffered: close each corresponding
        // picker. D must ask even though wrong/D is a readable matching PMD.
        load(first, {{missing, L"rel-A", buffered ? 1 : 3, {}},
                     {missing, L"rel-B", 1, file(wrong, 1)},
                     {mismatch, L"wrong-B", buffered ? 3 : 4, {}},
                     {missing, L"rel-D", 1, file(good, 3)}});
        Check(!app->ModelSlot(0) && !app->ModelSlot(1), "later acceptance leaves both skipped V2 slots empty");
        verify(2); verify(3);
    }
    load(first, {{missing, L"rel-A", 1, file(good, 0)}}, true);
    Check(RelocationTicket::Capture(*app).Infer(file(old, 1).native()).path == file(good, 1).native(),
          "direct V2 legacy success retains the mapping after its wrapper returns");
    CopyDirPathW(app->DirModel(), savedModelDirectory.c_str());
    pmm_io::bufferedReads = savedMode;
    app->EnglishUI() = language;
    Normal(emptyV2, camera, light);
}
