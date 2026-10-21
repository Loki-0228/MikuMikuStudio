// Exercise the shipped SDK example through actual command and host message
// dispatch. No probe export or direct call to its internal callbacks is used.
static void ManagerAutoRefreshExampleChecks(const fs::path& root, const fs::path& library) {
	const auto directory = root / "auto-refresh-sdk-example";
	const auto package = directory / "PLUGINS" / "example";
	fs::create_directories(package);
	fs::copy_file(library, package / "auto_refresh.dll");
	fs::copy_file(library.parent_path() / "plugin.json", package / "plugin.json");
	std::vector<std::string> logs;
	Manager manager(directory / "user", [&](const auto& value) { logs.push_back(value); });
	manager.Load(package.parent_path());
	CHECK(manager.Status().size() == 1 && manager.Status()[0].stage == "started");
	UiClient caller{&manager.Ui(), "mms"}; auto ui = manager.Ui().Api(caller);
	const auto invoke = [&] { return ui.invoke(ui.context, S("org.example.auto-refresh.publish"), {}, {}); };
	// start registered controls only. Before native publication, the command
	// must fail without attempting to bind an unavailable target.
	CHECK(invoke() == MMS_E_NOT_READY);
	manager_refresh_checks::Window window;
	auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, [&](const auto& value) { logs.push_back(value); });
	native->Refresh(); CHECK(native->Layout(10, 500, 140, true) > 0);
	HWND button = nullptr; unsigned buttons = 0;
	for (HWND child = GetWindow(native->Toolbar(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		wchar_t name[32]{}; GetClassNameW(child, name, 32);
		if (std::wcscmp(name, L"Button") == 0 && IsWindowVisible(child)) { button = child; ++buttons; }
	}
	CHECK(buttons == 1 && button);
	CHECK(SendMessageW(button, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
	const auto registry = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
	for (unsigned value = 1; value <= 3; ++value) {
		CHECK(invoke() == MMS_OK);
		const auto expected = value & 1 ? BST_CHECKED : BST_UNCHECKED;
		const auto until = GetTickCount64() + 2000;
		do { manager.Drain(); manager_refresh_checks::Pump(2); }
		while (SendMessageW(button, BM_GETCHECK, 0, 0) != expected && GetTickCount64() < until);
		CHECK(SendMessageW(button, BM_GETCHECK, 0, 0) == expected);
		CHECK(manager.Ui().Revision() == registry && manager.Ui().PreferencesRevision() == preferences);
	}
	for (const auto& line : logs) {
		CHECK(line.find("初始化未完成") == std::string::npos && line.find("发布失败") == std::string::npos &&
			line.find("重新绑定失败") == std::string::npos);
	}
	// Stop with a real queued command task; the host cancels it before destroy.
	CHECK(invoke() == MMS_OK && manager.Stop());
	manager_refresh_checks::Pump(5);
	CHECK(manager.Status()[0].stage == "stopped" && manager.Ui().commands.empty());
	native.reset(); CHECK(manager.Stop());
}
