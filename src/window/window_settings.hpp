#pragma once
#include "settings_json.hpp"
#include "mikumikustudio/mmd_app.hpp"

namespace mikumikustudio::window_settings {
inline std::string Snapshot(MMDApp& app, int x, int y, int width, int height,
    int maximized, int floating, int physics, int check119, int check12d) {
    std::string out = "{";
    auto number = [&](const char* name, auto value) {
        if (out.size() > 1) out += ',';
        out += settings_json::Quote(name) + ':' + std::to_string(value);
    };
    number("x",x); number("y",y); number("width",width); number("height",height);
    number("maximized",maximized); number("renderWidth",app.RenderWidth()); number("renderHeight",app.RenderHeight());
    number("sidebarWidth",app.FloatingWindow() ? app.SeparateWindowSidebarWidth() : app.SidebarWidth());
    number("uiLanguage",app.EnglishUI()); number("floating",floating);
    number("floatingMaximized",app.SeparateWindowMaximized());
    number("floatingX",app.SeparateWindowX()); number("floatingY",app.SeparateWindowY());
    number("floatingWidth",app.SeparateWindowWidth()); number("floatingHeight",app.SeparateWindowHeight());
    number("physicsFrame",physics); number("frameVolume",int(app.FrameVolumeControlEnabled()));
    number("frameNormalization",app.FrameNormalization()); number("sidebarRatio",std::isfinite(app.SidebarRatio()) ? app.SidebarRatio() : -1.0f);
    number("menu119",check119); number("menu12d",check12d);
    auto path = [&](const char* name, const wchar_t* value) { out += ',' + settings_json::Quote(name) + ':' + settings_json::Quote(std::wstring_view(value)); };
    path("modelDirectory",app.DirModel()); path("accessoryDirectory",app.DirAccs()); path("waveDirectory",app.DirWave());
    path("poseDirectory",app.DirPose()); path("motionDirectory",app.DirMotion()); path("userDirectory",app.DirUser()); path("backgroundDirectory",app.DirBg());
    return out + '}';
}
inline void ReadPath(const settings_json::Json& json, const char* name, wchar_t* target) {
    auto found = json.object.find(name);
    if (found == json.object.end() || found->second.kind != settings_json::Json::StringValue) return;
    try {
        const auto value = plugins::Wide(found->second.text);
        if (value.size() < 1000 && value.find(L'\0') == std::wstring::npos) wcscpy_s(target,1000,value.c_str());
    } catch (...) { /* Keep the valid default for malformed path text. */ }
}
}
