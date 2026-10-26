#include "manager.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace mikumikustudio::plugins;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while(false)
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    try {
        const auto root = fs::absolute(argv[1]) / ("run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        CHECK(!fs::exists(root));
        const auto package = root / "PLUGINS" / "image-panel";
        CHECK(fs::create_directories(package));
        CHECK(fs::copy_file(argv[2], package / "image_panel.dll"));
        CHECK(fs::copy_file(argv[3], package / "plugin.json"));
        Manager manager(root / "data", [](const std::string& line) { std::cout << line << '\n'; });
        manager.Load(package.parent_path());
        const auto statuses = manager.Status();
        CHECK(statuses.size() == 1 && statuses.front().stage == "started");
        HWND parent = CreateWindowExW(0, L"STATIC", L"image example test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(parent != nullptr);
        CHECK(manager.Ui().ShowSurface("org.example.image-panel.panel", parent, {0, 0, 64, 64}, 168, true) == MMS_OK);
        UiRegistry::SurfacePresentation shown{};
        CHECK(manager.Ui().ExistingSurface("org.example.image-panel.panel", shown));
        CHECK(shown.window != nullptr);
        HDC dc = CreateCompatibleDC(nullptr); CHECK(dc != nullptr);
        BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = 64; bitmap.bmiHeader.biHeight = -64;
        bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
        CHECK(dib != nullptr && bits != nullptr);
        const auto old = SelectObject(dc, dib);
        SendMessageW(shown.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        GdiFlush();
        const auto* pixels = static_cast<const uint32_t*>(bits);
        CHECK((pixels[10 * 64 + 10] & 0xffffff) == 0xff0000);
        CHECK((pixels[10 * 64 + 40] & 0xffffff) == 0x109010);
        CHECK((pixels[40 * 64 + 10] & 0xffffff) == 0x0000ff);
        CHECK((pixels[40 * 64 + 40] & 0xffffff) == 0x202020);
        SelectObject(dc, old); DeleteObject(dib); DeleteDC(dc);
        CHECK(manager.Stop());
        CHECK(!IsWindow(shown.window));
        CHECK(GetModuleHandleW((package / "image_panel.dll").c_str()) == nullptr);
        CHECK(DestroyWindow(parent));
        CHECK(manager.Stop());
        std::cout << "PASS " << checks << " image panel example checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; return 1;
    }
}
