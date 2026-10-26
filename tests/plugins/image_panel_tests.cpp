// Hidden-window integration tests. WM_PRINTCLIENT paints into a private DIB;
// no desktop input, screenshot or visual inspection is used.
#include "render_image_store.hpp"
#include "ui_registry.hpp"
#include <mms/cpp/image_panel.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mikumikustudio::plugins;
using mms::sdk::ImagePanel;
using mms::sdk::ImageCanvasOptions;
using mms::sdk::ImageScale;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
static MmsString S(const char* value) { return {value, std::strlen(value)}; }

struct HiddenParent {
    HWND window = CreateWindowExW(0, L"STATIC", L"CPU image test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~HiddenParent() { if (window && IsWindow(window)) DestroyWindow(window); }
    void Destroy() { CHECK(DestroyWindow(window)); window = nullptr; }
};

// GDI's BI_RGB destination does not promise an alpha byte. Verify every RGB
// pixel, including letterboxing, against independently specified colors.
static std::vector<uint32_t> ReadPixels(HWND window, uint32_t width, uint32_t height) {
    RECT rect{};
    CHECK(IsWindow(window) && GetClientRect(window, &rect));
    CHECK(rect.right == static_cast<LONG>(width) && rect.bottom == static_cast<LONG>(height));
    const HDC dc = CreateCompatibleDC(nullptr);
    CHECK(dc != nullptr);
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(dc); CHECK(false); }
    const auto previous = SelectObject(dc, dib);
    if (!previous || previous == HGDI_ERROR) { DeleteObject(dib); DeleteDC(dc); CHECK(false); }
    std::memset(bits, 0xa5, size_t(width) * height * 4);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    std::vector<uint32_t> result(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + size_t(width) * height);
    SelectObject(dc, previous);
    DeleteObject(dib);
    DeleteDC(dc);
    for (auto& pixel : result) pixel &= 0x00ffffffu;
    return result;
}
static void Pixels(ImagePanel& panel, uint32_t width, uint32_t height, const std::vector<uint32_t>& expected) {
    CHECK(expected.size() == size_t(width) * height);
    CHECK(ReadPixels(panel.Window(), width, height) == expected);
    CHECK(panel.LastResult() == MMS_OK);
}
static MmsHandle CreateImage(const MmsRenderImageApiV1& api, uint32_t width, uint32_t height,
                             uint32_t format, uint32_t alpha, const std::vector<unsigned char>& pixels) {
    MmsRenderImageDescV1 desc{Header<MmsRenderImageDescV1>(), width, height, format,
        MMS_IMAGE_COLOR_SRGB, alpha, 0, uint64_t(width) * 4};
    MmsHandle handle = 0;
    CHECK(api.createCpu(api.context, &desc, {pixels.data(), pixels.size()}, &handle) == MMS_OK);
    CHECK(handle != 0);
    return handle;
}
struct QueryBridge {
    MmsRenderImageApiV1 api{};
    unsigned calls = 0;
    MmsHostV1 Host() {
        MmsHostV1 host{};
        host.h = Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* context, MmsString name, uint32_t major, uint32_t minor, uint32_t bytes, void* out) -> MmsResult {
            auto& self = *static_cast<QueryBridge*>(context);
            ++self.calls;
            if (name.bytes != 16 || !name.data || std::memcmp(name.data, "mms.render.image", 16)) return MMS_E_NOT_FOUND;
            return Table(major, minor, bytes, out, self.api);
        };
        return host;
    }
};
static void RegisterPanel(UiRegistry& registry, UiClient& owner, ImagePanel& panel,
                          const char* surfaceId, const char* elementId, MmsHandle& factory, MmsHandle& element) {
    const auto surfaces = registry.SurfaceApi(owner);
    const auto api = registry.Api(owner);
    const auto surface = panel.Surface(S(surfaceId));
    CHECK(surface.flags == 0);
    CHECK(surfaces.registerSurface(surfaces.context, &surface, &factory) == MMS_OK);
    MmsUiElementV1 desc{};
    desc.h = Header<MmsUiElementV1>(); desc.id = S(elementId); desc.ownerPlugin = Text(owner.id);
    desc.parentId = S("mms.workspace.tabs.extensions"); desc.surfaceId = surface.id;
    desc.label = S("Image"); desc.kind = MMS_UI_PANEL;
    desc.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE;
    CHECK(api.registerElement(api.context, &desc, &element) == MMS_OK);
}
static void UnregisterPanel(UiRegistry& registry, UiClient& owner, MmsHandle factory, MmsHandle element) {
    const auto api = registry.Api(owner);
    const auto surfaces = registry.SurfaceApi(owner);
    CHECK(api.unregisterItem(api.context, element) == MMS_OK);
    CHECK(surfaces.unregisterSurface(surfaces.context, factory) == MMS_OK);
}
static void PanelChecks() {
    RenderImageStore store;
    RenderImageStore::Client images;
    images.owner = &store;
    const auto imageApi = store.Api(images);
    ImagePanel first, second;
    // Registry dies before the panel objects even if an assertion throws.
    UiClient owner{nullptr, "example.image-panel"};
    UiRegistry registry;
    owner.registry = &registry;
    HiddenParent parent;
    CHECK(parent.window != nullptr && !IsWindowVisible(parent.window));

    CHECK(first.SetImage(0) == MMS_E_NOT_READY);
    CHECK(first.Close() == MMS_OK);
    MmsRenderImageApiV1 invalid{};
    CHECK(first.Initialize(invalid) == MMS_E_VERSION);
    invalid = imageApi; invalid.mapRead = nullptr;
    CHECK(first.Initialize(invalid) == MMS_E_ARGUMENT);
    QueryBridge query{imageApi};
    const auto host = query.Host();
    CHECK(first.Initialize(host) == MMS_OK);
    CHECK(query.calls == 1);
    CHECK(first.Initialize(host) == MMS_E_BUSY);
    CHECK(query.calls == 1);
    CHECK(second.Initialize(imageApi) == MMS_OK);
    CHECK(first.SetOptions({ImageScale::Contain, 0}) == MMS_OK);

    const auto redGreen = CreateImage(imageApi, 2, 1, MMS_IMAGE_RGBA8, MMS_IMAGE_ALPHA_OPAQUE,
        {255, 0, 0, 0, 0, 255, 0, 1});
    CHECK(first.SetImage(redGreen) == MMS_OK);
    CHECK(store.HasLeases(images));
    MmsHandle firstFactory = 0, firstElement = 0, secondFactory = 0, secondElement = 0;
    RegisterPanel(registry, owner, first, "example.image-panel.first-factory", "example.image-panel.first", firstFactory, firstElement);
    CHECK(registry.ShowSurface("example.image-panel.first", parent.window, {1, 2, 4, 4}, 96, true) == MMS_OK);
    CHECK(first.Window() != nullptr && GetParent(first.Window()) == parent.window);
    CHECK(!IsWindowVisible(first.Window())); // Parent remains hidden throughout.
    const std::vector<uint32_t> contain{
        0, 0, 0, 0,
        0xff0000, 0xff0000, 0x00ff00, 0x00ff00,
        0xff0000, 0xff0000, 0x00ff00, 0x00ff00,
        0, 0, 0, 0};
    Pixels(first, 4, 4, contain);
    CHECK(first.Close() == MMS_E_BUSY);
    CHECK(first.SetImage(7) == MMS_E_NOT_FOUND);
    Pixels(first, 4, 4, contain);
    MmsResult workerResult = MMS_OK;
    std::thread worker([&] { workerResult = first.SetImage(0); });
    worker.join();
    CHECK(workerResult == MMS_E_THREAD);
    Pixels(first, 4, 4, contain);

    CHECK(first.SetOptions({ImageScale::Stretch, 0}) == MMS_OK);
    const std::vector<uint32_t> stretch{
        0xff0000, 0xff0000, 0x00ff00, 0x00ff00,
        0xff0000, 0xff0000, 0x00ff00, 0x00ff00,
        0xff0000, 0xff0000, 0x00ff00, 0x00ff00,
        0xff0000, 0xff0000, 0x00ff00, 0x00ff00};
    Pixels(first, 4, 4, stretch);
    CHECK(first.SetOptions({static_cast<ImageScale>(17), 0}) == MMS_E_ARGUMENT);
    Pixels(first, 4, 4, stretch);
    CHECK(first.SetOptions({ImageScale::Contain, 0}) == MMS_OK);
    CHECK(registry.ShowSurface("example.image-panel.first", parent.window, {3, 4, 2, 4}, 192, true) == MMS_OK);
    Pixels(first, 2, 4, {0, 0, 0xff0000, 0x00ff00, 0, 0, 0, 0});
    CHECK(first.SetImage(0) == MMS_OK);
    CHECK(!store.HasLeases(images));
    Pixels(first, 2, 4, std::vector<uint32_t>(8, 0));
    CHECK(first.SetImage(redGreen) == MMS_OK);
    CHECK(imageApi.release(imageApi.context, redGreen) == MMS_OK);
    Pixels(first, 2, 4, {0, 0, 0xff0000, 0x00ff00, 0, 0, 0, 0});

    // Destroying a parent sends WM_NCDESTROY independently of the registry.
    // Its later destroy callback must be harmless, and a new view uses the
    // same retained image with the new parent's geometry.
    parent.Destroy();
    CHECK(first.Window() == nullptr);
    registry.DestroyViews();
    HiddenParent replacement;
    CHECK(replacement.window != nullptr && !IsWindowVisible(replacement.window));
    CHECK(registry.ShowSurface("example.image-panel.first", replacement.window, {0, 0, 4, 4}, 168, true) == MMS_OK);
    Pixels(first, 4, 4, contain);

    CHECK(second.SetOptions({ImageScale::Stretch, 0x204060}) == MMS_OK);
    const auto straight = CreateImage(imageApi, 1, 1, MMS_IMAGE_BGRA8, MMS_IMAGE_ALPHA_STRAIGHT, {100, 50, 200, 128});
    CHECK(second.SetImage(straight) == MMS_OK);
    CHECK(imageApi.release(imageApi.context, straight) == MMS_OK);
    RegisterPanel(registry, owner, second, "example.image-panel.second-factory", "example.image-panel.second", secondFactory, secondElement);
    CHECK(registry.ShowSurface("example.image-panel.second", replacement.window, {10, 0, 2, 2}, 96, true) == MMS_OK);
    CHECK(first.Window() != second.Window());
    Pixels(second, 2, 2, std::vector<uint32_t>(4, 0x743962));
    Pixels(first, 4, 4, contain);
    const auto premultiplied = CreateImage(imageApi, 1, 1, MMS_IMAGE_BGRA8, MMS_IMAGE_ALPHA_PREMULTIPLIED, {50, 25, 100, 128});
    CHECK(second.SetImage(premultiplied) == MMS_OK);
    CHECK(imageApi.release(imageApi.context, premultiplied) == MMS_OK);
    Pixels(second, 2, 2, std::vector<uint32_t>(4, 0x743962));

    // Retirement removes public image handles, but held mappings must still
    // support redraw and recomposition after resize until explicit Close.
    store.Retire(images);
    CHECK(store.HasLeases(images));
    CHECK(first.SetImage(redGreen) == MMS_E_NOT_READY);
    Pixels(first, 4, 4, contain);
    CHECK(registry.ShowSurface("example.image-panel.second", replacement.window, {10, 0, 3, 2}, 144, true) == MMS_OK);
    Pixels(second, 3, 2, std::vector<uint32_t>(6, 0x743962));
    CHECK(second.Close() == MMS_E_BUSY);
    UnregisterPanel(registry, owner, firstFactory, firstElement);
    CHECK(first.Window() == nullptr);
    CHECK(first.Close() == MMS_OK);
    CHECK(store.HasLeases(images));
    UnregisterPanel(registry, owner, secondFactory, secondElement);
    CHECK(second.Close() == MMS_OK);
    CHECK(!store.HasLeases(images));
    CHECK(!owner.unsafeToUnload);
    CHECK(first.Close() == MMS_OK && second.Close() == MMS_OK);
    CHECK(first.SetImage(0) == MMS_E_NOT_READY);
}
int main() {
    try {
        PanelChecks();
        std::cout << "CPU image panel PASS: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
