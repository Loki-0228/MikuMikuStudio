#include <mms/cpp/ui.hpp>
#include <mms/cpp/image_panel.hpp>
#include <new>

using mms::sdk::Header;
using mms::sdk::Text;
struct Plugin {
    mms::sdk::ImagePanel panel;
    mms::sdk::RegistrationBundle registrations;
    MmsRenderImageApiV1 images{};
    explicit Plugin(mms::sdk::UiServices ui) : registrations(std::move(ui)) {}
};
static MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1*, void** out) {
    if (!host || !out || !host->queryService) return MMS_E_ARGUMENT;
    *out = nullptr;
    mms::sdk::UiServices ui;
    if (const auto result = mms::sdk::UiServices::Query(*host, ui)) return result;
    auto* plugin = new (std::nothrow) Plugin(std::move(ui));
    if (!plugin) return MMS_E_MEMORY;
    const auto queried = host->queryService(host->context, Text("mms.render.image"), 1, 0,
        sizeof(plugin->images), &plugin->images);
    const auto result = queried == MMS_OK ? plugin->panel.Initialize(plugin->images) : queried;
    if (result != MMS_OK) { delete plugin; return result; }
    *out = plugin; return MMS_OK;
}
static MmsResult MMS_CALL Start(void* context) {
    auto& plugin = *static_cast<Plugin*>(context);
    const auto surface = plugin.panel.Surface(Text("org.example.image-panel.surface"));
    if (const auto result = plugin.registrations.AddSurface(surface)) return result;
    MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>();
    element.id = Text("org.example.image-panel.panel");
    element.ownerPlugin = Text("org.example.image-panel");
    element.parentId = Text("mms.workspace.tabs.extensions");
    element.label = Text("CPU 图像面板");
    element.surfaceId = surface.id; element.kind = MMS_UI_PANEL;
    if (const auto result = plugin.registrations.AddElement(element)) return result;
    // Four source pixels; the same image lease can feed other panel objects.
    const unsigned char pixels[16] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 255, 255, 255, 255, 0
    };
    const MmsRenderImageDescV1 desc{Header<MmsRenderImageDescV1>(), 2, 2,
        MMS_IMAGE_RGBA8, MMS_IMAGE_COLOR_SRGB, MMS_IMAGE_ALPHA_STRAIGHT, 0, 8};
    MmsHandle image = 0;
    if (const auto result = plugin.images.createCpu(plugin.images.context, &desc, {pixels, sizeof(pixels)}, &image)) return result;
    const auto result = plugin.panel.SetImage(image);
    const auto released = plugin.images.release(plugin.images.context, image);
    return result != MMS_OK ? result : released;
}
static void MMS_CALL Event(void*, const MmsEventV1*) {}
static MmsResult MMS_CALL Stop(void* context) {
    auto& plugin = *static_cast<Plugin*>(context);
    if (const auto result = plugin.registrations.Close()) return result;
    return plugin.panel.Close();
}
static void MMS_CALL Destroy(void* context) { delete static_cast<Plugin*>(context); }
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {Header<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
