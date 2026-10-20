void CheckVersionEffectSave(MMDApp* app, const std::filesystem::path& project) {
    const auto effect = pmm_paths::Effect(project);
    Check(std::filesystem::is_regular_file(effect), "versioned EMM was loaded from the matching suffix");
    std::filesystem::remove(effect);
    NotifyEffectFileSave(project.c_str());
    for (int i = 0; i < 5; ++i) {
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
    }
    Check(std::filesystem::is_regular_file(effect) && std::filesystem::file_size(effect) > 0,
        "native MME writes the matching versioned EMM after PMM save notification");
    auto incorrect = project; incorrect.replace_extension(L".emm");
    Check(!std::filesystem::exists(incorrect), "versioned EMM save never overwrites the base project's effects");
}
