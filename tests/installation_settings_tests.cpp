// CPU-only installation settings checks. No resource/shader compilation or GPU is required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "../src/app/settings_json.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "window_settings_locale_checks.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fstream>
namespace fs=std::filesystem;
namespace as=mikumikustudio::autosave;
using namespace mikumikustudio;
namespace {
unsigned checks=0;
fs::path root;
void Check(bool ok,const char* reason){++checks;if(!ok){std::printf("FAIL %s\n",reason);std::exit(1);}}
#include "installation_settings_checks.inc"

void WindowSettingsLocaleChecks() {
    using system_settings::Result;
    const auto path = root / L"window-locale" / L"settings.json";
    Check(!fs::exists(path), "window locale test needs a fresh fixture");
    system_settings::Store store(path);
    Check(store.WriteSection("window", R"({"future":{"keep":7}})") == Result::Ok &&
        store.WriteSection("resources", R"({"future":9})") == Result::Ok, "seed settings to preserve across window writes");
    auto app = std::make_unique<MMDApp>();
    ::new(static_cast<void*>(&app->state)) MMDAppState{};
    window_settings_regression::Run(*app, Check, [&](const std::string& snapshot) {
        std::string body;
        Check(store.WriteSection("window", snapshot) == Result::Ok &&
            store.ReadSection("window", body) == Result::Ok, "central store accepts and reads the shipped window snapshot");
        const auto saved = settings_json::Object(body);
        Check(settings_json::Float(saved, "sidebarRatio", -1.0f) == 0.8125f &&
            saved.At("future").At("keep").UInt() == 7 &&
            store.ReadSection("resources", body) == Result::Ok && settings_json::Object(body).At("future").UInt() == 9,
            "central store round-trips ratios while preserving unknown keys and sections");
    });
    std::printf("PASS window settings locale (%u checks)\n", checks);
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);Check(argc==3,"disposable root and explicit settings mode required");
    root=fs::absolute(argv[1]);wchar_t executable[32768]{};Check(GetModuleFileNameW(nullptr,executable,32768)!=0,"resolve actual test EXE");
    const std::wstring mode=argv[2];
    if(mode==L"--settings-window-locale"){WindowSettingsLocaleChecks();return 0;}
    Check(mode==L"--settings-fresh" || mode==L"--settings-default" || mode==L"--settings-custom" ||
        mode==L"--settings-check-custom" || mode==L"--settings-readonly" || mode==L"--settings-override" ||
        mode==L"--settings-migrate" || mode==L"--settings-migrate-relative","known settings probe mode");
    SettingsLocationChecks(executable,mode);return 0;
}
