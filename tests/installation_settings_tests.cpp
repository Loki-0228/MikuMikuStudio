// CPU-only installation settings checks. No resource/shader compilation or GPU is required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
namespace fs=std::filesystem;
namespace as=mikumikustudio::autosave;
using namespace mikumikustudio;
namespace {
unsigned checks=0;
fs::path root;
void Check(bool ok,const char* reason){++checks;if(!ok){std::printf("FAIL %s\n",reason);std::exit(1);}}
#include "installation_settings_checks.inc"
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);Check(argc==3,"disposable root and explicit settings mode required");
    root=fs::absolute(argv[1]);wchar_t executable[32768]{};Check(GetModuleFileNameW(nullptr,executable,32768)!=0,"resolve actual test EXE");
    const std::wstring mode=argv[2];
    Check(mode==L"--settings-fresh" || mode==L"--settings-default" || mode==L"--settings-custom" ||
        mode==L"--settings-check-custom" || mode==L"--settings-readonly" || mode==L"--settings-override","known settings probe mode");
    SettingsLocationChecks(executable,mode);return 0;
}
