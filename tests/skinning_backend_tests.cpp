// MAINTENANCE: hardware-manual. Hidden window; no desktop capture.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "../src/model/skinning_calibration_cache.hpp"
#include "mikumikustudio/skinning_menu.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <limits>
#include <vector>
using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
// Test seam, defined in src/window/skinning_menu.cpp.  skinning_menu.hpp is
// deliberately not extended for it, so the declaration lives here: the pure
// classification behind the renderer menu's "not provided by the current
// D3D9" rows.
namespace mikumikustudio {
std::vector<unsigned> RendererExtraDeviceRows(
    const std::vector<processor::AdapterEntry>& rendererAdapters,
    const std::vector<processor::AdapterEntry>& computeAdapters);
}
namespace {
void Check(bool good, const char* message) {
    if (!good) { std::printf("FAIL %s\n", message); std::exit(1); }
}
// A synthetic adapter for the classification tables.  Cards of one model share
// one full hardware identity, so same-model twins are built with equal
// vendor/device/subsystem/revision and different LUIDs.
processor::AdapterEntry Device(unsigned ordinal, std::uint64_t luid, const wchar_t* name,
                               std::uint32_t deviceId) {
    processor::AdapterEntry entry;
    entry.ordinal = ordinal;
    entry.adapterId = luid;
    entry.adapterName = name;
    entry.identity = {0x10DE, deviceId, 0x12340001, 1};
    return entry;
}
// The console cannot print every wide character in the C locale, so probe rows
// are logged with \uXXXX escapes for anything outside printable ASCII.
void PrintRow(const char* tag, std::size_t index, const wchar_t* text) {
    std::printf("%s %zu: ", tag, index);
    for (const wchar_t* p = text; *p; ++p) {
        const unsigned c = static_cast<unsigned>(*p);
        if (c >= 0x20 && c < 0x7F) std::printf("%c", static_cast<char>(c));
        else std::printf("\\u%04x", c);
    }
    std::printf("\n");
}
// Help is found through its About command (201), the convention the port
// already uses in enhance_model_io.cpp and physics_bake_dialog.cpp.  A decoy
// popup sits after Help in the test bar, so the lookup cannot pass by taking
// the last top-level menu.
HMENU HelpMenu(HMENU bar) {
    const int count=GetMenuItemCount(bar);
    for(int i=0;i<count;++i) {
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        if(!GetMenuItemInfoW(bar,UINT(i),TRUE,&item)||!item.hSubMenu) continue;
        if(GetMenuState(item.hSubMenu,201,MF_BYCOMMAND)!=UINT(-1)) return item.hSubMenu;
    }
    return nullptr;
}
// The processor entry is the Help item whose submenu carries one of the
// processor commands, so it is found by content and not by text or by a cached
// handle - a language switch cannot lose it.
int ProcessorEntryPosition(HMENU help) {
    const int count=GetMenuItemCount(help);
    for(int i=0;i<count;++i) {
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        if(!GetMenuItemInfoW(help,UINT(i),TRUE,&item)||!item.hSubMenu) continue;
        if(GetMenuState(item.hSubMenu,kSkinningAutomaticId,MF_BYCOMMAND)!=UINT(-1)||
           GetMenuState(item.hSubMenu,kRendererAutomaticId,MF_BYCOMMAND)!=UINT(-1)||
           GetMenuState(item.hSubMenu,kMmvFollowRendererId,MF_BYCOMMAND)!=UINT(-1)) return i;
    }
    return -1;
}
std::wstring AutomaticCaption(MMDApp* app, HMENU menu) {
    RefreshSkinningMenu(app);
    MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
    Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "processor submenu");
    // Position 0 is 主渲染设备 and 1 is 顶点计算设备.  The
    // legacy automatic id belongs to the vertex compute submenu.
    HMENU vertices=GetSubMenu(item.hSubMenu,1);
    Check(vertices!=nullptr, "vertex compute submenu");
    wchar_t label[512]{};
    Check(GetMenuStringW(vertices,kSkinningAutomaticId,label,512,MF_BYCOMMAND)>0, "automatic item text");
    std::wstring text=label;
    for(std::size_t i=0;i+1<text.size();++i)
        if(text[i]==L'&' && text[i+1]==L'&') text.erase(i,1);
    return text;
}
// The greyed current-state line of the vertex compute submenu.  It shows the
// real skinning status of the most recent model computation, not the pending
// choice and not a device the menu invented.
std::wstring VertexCurrentLine(MMDApp* app, HMENU menu) {
    RefreshSkinningMenu(app);
    MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
    Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "processor submenu");
    wchar_t label[512]{};
    Check(GetMenuStringW(GetSubMenu(item.hSubMenu,1),0,label,512,MF_BYPOSITION)>0, "vertex current line");
    std::wstring text=label;
    for(std::size_t i=0;i+1<text.size();++i)
        if(text[i]==L'&' && text[i+1]==L'&') text.erase(i,1);
    return text;
}
// GetMenuState returns (UINT)-1 for a command the menu does not have, and -1
// carries every flag bit - so an absent item must be turned into "no state"
// before any flag is tested.
UINT ItemState(HMENU menu, unsigned id) {
    const UINT state=GetMenuState(menu,id,MF_BYCOMMAND);
    return state==static_cast<UINT>(-1)?0u:state;
}
struct Snapshot { std::vector<unsigned char> main; std::vector<mdl::EdgeVertex> edge; };
Snapshot Read(IDirect3DVertexBuffer9* main, IDirect3DVertexBuffer9* edge, unsigned stride, unsigned count) {
    Snapshot s; s.main.resize(stride * count); s.edge.resize(count);
    void* bytes = nullptr;
    Check(SUCCEEDED(main->Lock(0, 0, &bytes, D3DLOCK_READONLY)), "lock body");
    std::memcpy(s.main.data(), bytes, s.main.size()); main->Unlock();
    Check(SUCCEEDED(edge->Lock(0, 0, &bytes, D3DLOCK_READONLY)), "lock edge");
    std::memcpy(s.edge.data(), bytes, count * sizeof(mdl::EdgeVertex)); edge->Unlock();
    return s;
}
void Same(const Snapshot& cpu, const Snapshot& gpu) {
    Check(cpu.main.size() == gpu.main.size(), "body size");
    for (std::size_t i = 0; i < cpu.main.size() / 4; ++i) {
        float a, b;
        std::memcpy(&a, cpu.main.data() + 4 * i, 4); std::memcpy(&b, gpu.main.data() + 4 * i, 4);
        Check(std::isfinite(b) && std::abs(double(a) - b) <= 2e-5 * std::max(1., std::abs(double(a))), "body position/normal/UV match");
    }
    for (std::size_t i = 0; i < cpu.edge.size(); ++i) {
        Check(cpu.edge[i].diffuse == gpu.edge[i].diffuse, "outline color matches");
        for (unsigned c = 0; c < 3; ++c)
            Check(std::isfinite(gpu.edge[i].position[c]) &&
                  std::abs(double(cpu.edge[i].position[c]) - gpu.edge[i].position[c]) <=
                      2e-5 * std::max(1., std::abs(double(cpu.edge[i].position[c]))), "outline position matches");
    }
}
}
int main(int argc, char** argv) {
    const bool menuOnly = argc > 1 && std::strcmp(argv[1], "--menu-only") == 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using skinning::FastestMeasurement;
    Check(FastestMeasurement({{0,5,true},{10,4,true},{20,1,true}})==20, "later integrated candidate can beat discrete");
    Check(FastestMeasurement({{0,1,true},{10,4,true},{20,2,true}})==0, "CPU can win automatic selection");
    Check(FastestMeasurement({{0,5,true},{10,.1,false},{20,2,true}})==20, "failed candidate cannot win");
    Check(FastestMeasurement({{0,5,true},{10,-1,true},{20,std::numeric_limits<double>::quiet_NaN(),true}})==0, "invalid timings cannot win");
    HWND window = CreateWindowW(L"STATIC", L"skinning regression", WS_OVERLAPPED, 0,0,64,64,nullptr,nullptr,nullptr,nullptr);
    Check(window != nullptr, "hidden window");
    HMENU menu = CreateMenu();
    for (unsigned i=0;i<3;++i) AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(CreatePopupMenu()), L"menu");
    // Help carries its About command (201), and a decoy popup sits after it, so
    // the processor entry can only find Help by that command and never by being
    // the last top-level menu.
    HMENU helpBar=CreatePopupMenu();
    AppendMenuW(helpBar, MF_STRING, 260, L"Japanese Mode");
    AppendMenuW(helpBar, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(helpBar, MF_STRING, 201, L"About");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpBar), L"help");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(CreatePopupMenu()), L"decoy after help");
    SetMenu(window, menu);
    auto app=std::make_unique<MMDApp>(); app->state=MMDAppState{}; app->state.hwnd=window;
    // ---- quick Help menu regression, no GPU work -------------------------
    // The processor entry lives in Help, the handles survive repeated
    // explicit refreshes and all three languages are applied. Everything
    // runs against a workspace override; the real LOCALAPPDATA is neither read
    // nor written.
    {
        std::filesystem::create_directories(L".local/artifacts/global-processor/menu-help");
        const std::wstring quickConfig=L".local/artifacts/global-processor/menu-help/processor.ini";
        processor::ResetForTesting();
        processor::SetConfigPathOverrideForTesting(quickConfig.c_str());
        for(unsigned language=0;language<3;++language) {
            app->EnglishUI()=static_cast<unsigned char>(language);
            RefreshSkinningMenu(app.get());
            HMENU help=HelpMenu(menu);
            Check(help!=nullptr, "Help is found through its About command");
            Check(GetMenuState(help,201,MF_BYCOMMAND)!=UINT(-1), "About still anchors Help");
            const int at=ProcessorEntryPosition(help);
            Check(at>=0, "the processor entry lives in Help");
            for(int i=at+1;i<GetMenuItemCount(help);++i) {
                MENUITEMINFOW probe{sizeof probe}; probe.fMask=MIIM_ID|MIIM_FTYPE;
                if(!GetMenuItemInfoW(help,UINT(i),TRUE,&probe)) continue;
                if(probe.fType&MFT_SEPARATOR) continue;
                Check(probe.wID==201, "the entry sits directly in front of About");
                break;
            }
            MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
            Check(GetMenuItemInfoW(help,UINT(at),TRUE,&item)&&item.hSubMenu, "the processor submenu exists");
            HMENU popup=item.hSubMenu;
            wchar_t label[128]{};
            GetMenuStringW(help,UINT(at),label,128,MF_BYPOSITION);
            const wchar_t* expect=language==2?L"运算器":language==1?L"Processors":L"演算器";
            Check(std::wcscmp(label,expect)==0, "the caption follows the language");
            MENUITEMINFOW stale{sizeof stale}; stale.fMask=MIIM_SUBMENU;
            Check(!GetMenuItemInfoW(GetSubMenu(menu,2),kSkinningMenuId,FALSE,&stale), "View no longer holds the entry");
            HMENU level2[2]{GetSubMenu(popup,0),GetSubMenu(popup,1)};
            Check(level2[0]&&level2[1]&&GetMenuItemCount(popup)==5,
                  "only host processor roles are shown without a loaded MMV DLL");
            for(int round=0;round<5;++round) {
                SendMessageW(window,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(popup),MAKELPARAM(0,TRUE));
                RefreshSkinningMenu(app.get());
                HMENU helpAgain=HelpMenu(menu);
                Check(ProcessorEntryPosition(helpAgain)==at, "the entry keeps its position across a refresh");
                MENUITEMINFOW again{sizeof again}; again.fMask=MIIM_SUBMENU;
                Check(GetMenuItemInfoW(helpAgain,UINT(at),TRUE,&again)&&again.hSubMenu==popup,
                      "the submenu handle survives a refresh");
                for(unsigned role=0;role<2;++role)
                    Check(GetSubMenu(again.hSubMenu,role)==level2[role], "a level-two handle survives a refresh");
            }
            Check(!(ItemState(level2[1],kSkinningAutomaticId)&MF_GRAYED), "the automatic command stays selectable");
            Check(!(ItemState(level2[1],kSkinningCpuId)&MF_GRAYED), "the CPU command stays selectable");
        }
        // The next-launch rule is unchanged: a pick writes Requested only.
        app->EnglishUI()=static_cast<unsigned char>(2);
        RefreshSkinningMenu(app.get());
        const auto quickActive=processor::Active();
        const auto quickPreference=skinning::GetPreference();
        app->SceneModified()=0;
        Check(HandleSkinningCommand(app.get(),kSkinningCpuId), "pick CPU from Help");
        Check(processor::Requested().vertices.mode==processor::Mode::Cpu, "the pick writes Requested");
        Check(processor::Active().vertices.mode==quickActive.vertices.mode, "Active is untouched");
        Check(skinning::GetPreference().mode==quickPreference.mode, "the live back end is untouched");
        Check(app->SceneModified()==0, "still no project edit");

        // ---- the renderer menu lists every supported device ----------------
        // CPU-only hidden-menu work.  The merge rules are checked against
        // synthetic D3D9+DXGI tables (pure classification), and the rendered
        // menu is checked against the machine's real cached lists.  No GPU
        // device is created for this and no performance is sampled.
        {
            // One card listed by both D3D9 and DXGI is listed once.
            std::vector<processor::AdapterEntry> d3d9{Device(0, 0xA1, L"NVIDIA GeForce RTX 4070", 1)};
            std::vector<processor::AdapterEntry> dxgi{Device(0, 0xA1, L"NVIDIA GeForce RTX 4070", 1)};
            Check(RendererExtraDeviceRows(d3d9, dxgi).empty(),
                  "a single card present in both lists is not listed twice");

            // Mixed machine: one D3D9 card plus one DXGI-only card.
            dxgi.push_back(Device(1, 0xB2, L"Intel(R) UHD Graphics", 2));
            std::vector<unsigned> rows = RendererExtraDeviceRows(d3d9, dxgi);
            Check(rows.size()==1 && rows[0]==1,
                  "mixed machine: only the DXGI-only card becomes an extra row");

            // A repeated LUID collapses into one row, and a repeated LUID the
            // D3D9 list provides is not shown at all.
            dxgi.push_back(Device(2, 0xB2, L"Intel(R) UHD Graphics", 2));
            rows = RendererExtraDeviceRows(d3d9, dxgi);
            Check(rows.size()==1 && rows[0]==1, "a repeated LUID collapses into one row");
            dxgi.push_back(Device(3, 0xA1, L"NVIDIA GeForce RTX 4070", 1));
            rows = RendererExtraDeviceRows(d3d9, dxgi);
            Check(rows.size()==1 && rows[0]==1, "a repeated provided LUID adds no row");

            // Same model, different cards: neither the identity nor the name
            // merges two physical cards.
            std::vector<processor::AdapterEntry> twinsD3d9{Device(0, 0x11, L"Intel(R) UHD Graphics", 7)};
            std::vector<processor::AdapterEntry> twinsDxgi{
                Device(0, 0x11, L"Intel(R) UHD Graphics", 7),
                Device(1, 0x12, L"Intel(R) UHD Graphics", 7)};
            rows = RendererExtraDeviceRows(twinsD3d9, twinsDxgi);
            Check(rows.size()==1 && rows[0]==1, "same model different cards are never merged");

            // An unknown LUID merges only through an identity match that is
            // unique on both sides.
            std::vector<processor::AdapterEntry> unknownLuid{Device(0, 0, L"Intel(R) UHD Graphics", 7)};
            Check(RendererExtraDeviceRows(twinsD3d9, unknownLuid).empty(),
                  "an unknown LUID merges through a unique identity match");
            std::vector<processor::AdapterEntry> sameModelTwins{
                Device(0, 0x11, L"Intel(R) UHD Graphics", 7),
                Device(1, 0x12, L"Intel(R) UHD Graphics", 7)};
            rows = RendererExtraDeviceRows(sameModelTwins, unknownLuid);
            Check(rows.size()==1 && rows[0]==0,
                  "an unknown LUID never merges into same-model twins");
            std::vector<processor::AdapterEntry> twoUnknown{
                Device(0, 0, L"Intel(R) UHD Graphics", 7),
                Device(1, 0, L"Intel(R) UHD Graphics", 7)};
            rows = RendererExtraDeviceRows(twinsD3d9, twoUnknown);
            Check(rows.size()==2,
                  "two unknown-LUID rows are never collapsed into one card");

            // The name never merges anything, and a row without LUID and
            // identity cannot be matched at all.
            std::vector<processor::AdapterEntry> namedD3d9{Device(0, 0x11, L"Same Name", 1)};
            std::vector<processor::AdapterEntry> namedDxgi{Device(0, 0x22, L"Same Name", 2)};
            rows = RendererExtraDeviceRows(namedD3d9, namedDxgi);
            Check(rows.size()==1 && rows[0]==0, "the adapter name never merges a row");
            processor::AdapterEntry blank; blank.adapterName=L"Same Name";
            std::vector<processor::AdapterEntry> blankDxgi{blank};
            rows = RendererExtraDeviceRows(namedD3d9, blankDxgi);
            Check(rows.size()==1 && rows[0]==0,
                  "a row without LUID and identity stays an extra row");
            std::printf("PASS renderer device classification: single, mixed, duplicate LUID, same model\n");
        }

        // The rendered menu: every D3D9 item keeps its command and mapping,
        // and the extra devices appear as greyed info rows marked
        // 当前 D3D9 未提供.  The D3D9 table is filled through the production
        // ResolveRenderer enumeration (adapter enumeration only).
        ComPtr<IDirect3D9> menuD3d; menuD3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        std::wstring resolveError;
        if(menuD3d)
            processor::ResolveRenderer(menuD3d.Get(), processor::Configuration{}.renderer, resolveError);
        const auto& renderers=processor::RendererAdapters();
        const auto& computes=processor::ComputeAdapters();
        const std::vector<unsigned> extras=RendererExtraDeviceRows(renderers,computes);
        const std::size_t listed=std::min<std::size_t>(renderers.size(),kRendererAdapterLimit);
        app->EnglishUI()=static_cast<unsigned char>(2);
        RefreshSkinningMenu(app.get());
        HMENU rendererMenu=nullptr;
        {
            MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
            Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu for the device sweep");
            rendererMenu=GetSubMenu(item.hSubMenu,0);
            Check(rendererMenu!=nullptr, "the main renderer submenu exists");
            Check(GetMenuItemCount(rendererMenu)==static_cast<int>(4+listed+extras.size()),
                  "the renderer submenu holds headers, choices and one row per extra device");
            wchar_t label[512]{};
            Check(GetMenuStringW(rendererMenu,0,label,512,MF_BYPOSITION)>0 &&
                  std::wcsstr(label,L"当前设备")!=nullptr, "the current device line is kept");
            Check(GetMenuStringW(rendererMenu,1,label,512,MF_BYPOSITION)>0 &&
                  std::wcsstr(label,L"下次启动")!=nullptr, "the next launch line is kept");
            MENUITEMINFOW automatic{sizeof automatic}; automatic.fMask=MIIM_ID;
            Check(GetMenuItemInfoW(rendererMenu,3,TRUE,&automatic) &&
                  automatic.wID==kRendererAutomaticId, "the automatic row keeps its command");
            Check(!(GetMenuState(rendererMenu,3,MF_BYPOSITION)&MF_GRAYED),
                  "the automatic row stays selectable");
            for(std::size_t i=0;i<listed;++i) {
                MENUITEMINFOW row{sizeof row}; row.fMask=MIIM_ID;
                Check(GetMenuItemInfoW(rendererMenu,static_cast<UINT>(4+i),TRUE,&row), "renderer device row");
                Check(row.wID==kRendererAdapterFirstId+static_cast<UINT>(i),
                      "the renderer command mapping is unchanged");
                const bool greyed=(GetMenuState(rendererMenu,static_cast<UINT>(4+i),MF_BYPOSITION)&MF_GRAYED)!=0;
                Check(greyed==(renderers[i].adapterId==0),
                      "a renderer device row is greyed only without a LUID");
            }
            for(std::size_t j=0;j<extras.size();++j) {
                const UINT position=static_cast<UINT>(4+listed+j);
                MENUITEMINFOW row{sizeof row}; row.fMask=MIIM_ID;
                Check(GetMenuItemInfoW(rendererMenu,position,TRUE,&row), "extra device row");
                Check(row.wID==0, "an extra device row carries no device command");
                Check(GetMenuState(rendererMenu,position,MF_BYPOSITION)&MF_GRAYED,
                      "an extra device row cannot be selected");
                Check(GetMenuStringW(rendererMenu,position,label,512,MF_BYPOSITION)>0, "extra device row text");
                Check(std::wcsstr(label,L"当前 D3D9 未提供")!=nullptr,
                      "an extra device row is marked 当前 D3D9 未提供");
                Check(std::wcsstr(label,L"当前设备")==nullptr && std::wcsstr(label,L"下次启动")==nullptr,
                      "an extra device row is not a status line");
            }
            // Evidence for the report: the exact rows the menu renders.
            for(std::size_t i=0;i<listed;++i) {
                wchar_t text[512]{};
                GetMenuStringW(rendererMenu,static_cast<UINT>(4+i),text,512,MF_BYPOSITION);
                PrintRow("renderer row",i,text);
            }
            for(std::size_t j=0;j<extras.size();++j) {
                wchar_t text[512]{};
                GetMenuStringW(rendererMenu,static_cast<UINT>(4+listed+j),text,512,MF_BYPOSITION);
                PrintRow("extra row",j,text);
            }
            std::printf("renderer menu probe: d3d9 rows=%zu extra rows=%zu computes=%zu\n",
                        listed,extras.size(),computes.size());
        }

        // An info row is not a command, and a command past the D3D9 list never
        // falls back onto another device.
        Check(!HandleSkinningCommand(app.get(),0), "an info row is not a command");
        {
            const auto before=processor::Requested().renderer;
            Check(HandleSkinningCommand(app.get(),kRendererAdapterFirstId+static_cast<unsigned>(listed)),
                  "a command past the renderer list is accepted without effect");
            Check(processor::Requested().renderer==before,
                  "no silent fallback onto another device");
        }
        if(listed>0 && renderers[0].adapterId!=0) {
            Check(HandleSkinningCommand(app.get(),kRendererAdapterFirstId), "renderer device pick accepted");
            Check(processor::Requested().renderer.adapterId==renderers[0].adapterId &&
                  processor::Requested().renderer.identity==renderers[0].identity,
                  "the renderer pick maps to the real device");
        }

        // The submenu handles and the extra rows stay stable across refreshes.
        for(int round=0;round<3;++round) {
            RefreshSkinningMenu(app.get());
            MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
            Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu after refresh");
            Check(GetSubMenu(item.hSubMenu,0)==rendererMenu,
                  "the renderer submenu handle survives a refresh");
            Check(GetMenuItemCount(rendererMenu)==static_cast<int>(4+listed+extras.size()),
                  "the extra rows are stable across refreshes");
        }

        // The marker text follows all three languages.
        const wchar_t* markers[3]={L"（現在の D3D9 では未提供）",
                                   L"(not provided by the current D3D9)",
                                   L"（当前 D3D9 未提供）"};
        for(unsigned language=0;language<3;++language) {
            app->EnglishUI()=static_cast<unsigned char>(language);
            RefreshSkinningMenu(app.get());
            if(extras.empty()) continue;
            wchar_t label[512]{};
            Check(GetMenuStringW(rendererMenu,static_cast<UINT>(4+listed),label,512,MF_BYPOSITION)>0,
                  "extra device row text per language");
            Check(std::wcsstr(label,markers[language])!=nullptr,
                  "the extra device row follows the language");
        }
        app->EnglishUI()=static_cast<unsigned char>(2);
        std::printf("PASS renderer menu rows: %zu D3D9 choices, %zu extra device info rows\n",
                    listed,extras.size());

        processor::ResetForTesting();
        std::printf("PASS help menu regression\n");
        if(menuOnly) {
            skinning::Shutdown();
            SetMenu(window,nullptr); DestroyWindow(window); DestroyMenu(menu);
            std::printf("PASS menu-only\n");
            return 0;
        }
    }

    const auto adapters = skinning::AvailableAdapters();
    Check(!adapters.empty(), "hardware D3D12 required for this regression");
    gpu::Skinning retry;
    Check(!retry.Initialize(false, UINT64_MAX), "missing explicit adapter fails");
    Check(retry.Initialize(false, adapters.front().id), "failed initialization can retry cleanly");
    bool integrated = false;
    for (const auto& a : adapters) {
        Check(a.id && !a.name.empty(), "adapter identity");
        Check(!integrated || a.integrated, "discrete before integrated");
        integrated |= a.integrated;
        std::printf("adapter=%ls id=%016llx integrated=%d\n", a.name.c_str(),
                    static_cast<unsigned long long>(a.id), a.integrated);
    }
    ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    Check(bool(d3d), "D3D9 runtime");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth=pp.BackBufferHeight=64; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    Check(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)), "D3D9 device");
    app->CameraDistance()=-20; app->CameraFov()=30;
    app->EnglishUI()=2;
    Check(AutomaticCaption(app.get(),menu)==L"自动（首次测速）", "the automatic choice never names a device");
    Check(VertexCurrentLine(app.get(),menu)==L"最近一次模型运算：等待模型运算", "pending automatic caption does not invent a device");
    skinning::SetPreference({});
    skinning::InitializeAutomatic(app.get(),device.Get());
    const auto startupCount=skinning::GetStatus().automaticCalibrations;
    Check(startupCount==6 && skinning::GetStartupMeasurements().size()==6,
          "startup measures three sizes for PMD and PMX before any model exists");
    skinning::InitializeAutomatic(app.get(),device.Get());
    Check(skinning::GetStatus().automaticCalibrations==startupCount,"startup calibration runs only once");
    for(const auto& profile:skinning::GetStartupMeasurements()) {
        Check(profile.report.measurements.size()==adapters.size()+1,"startup checks CPU and every GPU");
        for(const auto& measurement:profile.report.measurements)
            Check(measurement.valid,"startup geometry matches CPU on every available GPU");
    }
    auto model=std::make_unique<mdl::ModelRecord>();
    constexpr unsigned count=8;
    mdl::BoneRecord bones[4]{}; mdl::PmdVertex pmd[count]{}; mdl::PmxVertex pmx[count]{};
    mdl::ModelMaterialRecord material{};
    model->vertexCount=count; model->boneCount=4; model->materialCount=1;
    model->loadComplete=1; model->edgeScale=1; model->boneTable=bones; model->materials=&material;
    model->rawVertices=pmd; model->pmxVertices=pmx;
    float world[16]{}; world[0]=world[5]=world[10]=world[15]=1;
    for(unsigned b=0;b<4;++b) {
        std::memcpy(bones[b].matInit,world,sizeof world); const float a=b*.13f;
        bones[b].matInit[0]=std::cos(a); bones[b].matInit[1]=std::sin(a);
        bones[b].matInit[4]=-std::sin(a); bones[b].matInit[5]=std::cos(a); bones[b].matInit[12]=b*.4f;
    }
    material.edgeColor[0]=.2f; material.edgeColor[1]=.4f; material.edgeColor[2]=.6f; material.edgeColor[3]=.8f; material.edgeSize=1.3f;
    for(unsigned v=0;v<count;++v) {
        pmd[v].position[0]=pmx[v].position[0]=v*.12f; pmd[v].normal[1]=pmx[v].normal[1]=1;
        pmd[v].uv[0]=pmx[v].uv[0]=.25f; pmd[v].uv[1]=pmx[v].uv[1]=.75f;
        pmd[v].bone[0]=0; pmd[v].bone[1]=1; pmd[v].weightPercent=60; pmd[v].edgeDisabled=v&1;
        pmx[v].weightType=static_cast<mdl::PmxWeightType>(v%4); pmx[v].edgeScale=.9f; pmx[v].hasPmxEdgeData=v&1;
        for(unsigned b=0;b<4;++b) { pmx[v].bone[b]=b; pmx[v].weight[b]=v%4==2?.25f:.6f; }
        for(unsigned uv=0;uv<4;++uv) for(unsigned c=0;c<4;++c) pmx[v].additionalUvByComponent[c][uv]=(v+uv+c)*.125f;
        pmx[v].sdef.center[0]=.2f; pmx[v].sdef.r0Offset[1]=.3f; pmx[v].sdef.r1Offset[2]=.4f;
    }
    auto* bytes=reinterpret_cast<unsigned char*>(model.get()); unsigned cases=0;
    std::array<std::array<float,16>,4> rigidMatrices{};
    for(unsigned b=0;b<4;++b) std::memcpy(rigidMatrices[b].data(),bones[b].matInit,64);
    for(int uv=-1;uv<=4;++uv) {
        model->physicsMode=uv<0?0:2; model->pmxAdditionalUvCount=uv<0?0:uv;
        const unsigned stride=32+16*model->pmxAdditionalUvCount;
        ComPtr<IDirect3DVertexBuffer9> main,edge;
        Check(SUCCEEDED(device->CreateVertexBuffer(count*stride,0,0,D3DPOOL_MANAGED,&main,nullptr)), "body buffer");
        Check(SUCCEEDED(device->CreateVertexBuffer(count*sizeof(mdl::EdgeVertex),0,0,D3DPOOL_MANAGED,&edge,nullptr)), "edge buffer");
        model->vertexBuffer=main.Get(); model->vertexBuffer2=edge.Get();
        void* init=nullptr; Check(SUCCEEDED(edge->Lock(0,0,&init,0)), "init edge");
        for(unsigned v=0;v<count;++v) static_cast<mdl::EdgeVertex*>(init)[v].diffuse=0x7f123456;
        edge->Unlock();
        for(bool affine : {false,true}) for(bool opaque : {false,true}) {
            for(unsigned b=0;b<4;++b) {
                std::memcpy(bones[b].matInit,rigidMatrices[b].data(),64);
                if(affine) {
                    const float stretch[9]={1.5f+b*.1f,.2f,0,.1f,.65f+b*.05f,.15f,0,.1f,1.2f};
                    for(unsigned row=0;row<3;++row) for(unsigned col=0;col<3;++col) {
                        float sum=0;
                        for(unsigned k=0;k<3;++k) sum+=stretch[row*3+k]*rigidMatrices[b][k*4+col];
                        bones[b].matInit[row*4+col]=sum;
                    }
                }
            }
            app->state.selfShadowEnabled=opaque;
            skinning::SetPreference({skinning::Mode::Cpu}); UpdateModelVertexBuffers(app.get(),bytes,world);
            const auto cpu=Read(main.Get(),edge.Get(),stride,count);
            for(const auto& a : adapters) {
                skinning::SetPreference({skinning::Mode::Adapter,a.id,a.name}); UpdateModelVertexBuffers(app.get(),bytes,world);
                Check(skinning::GetStatus().result==skinning::Result::Gpu, "bridge actually used GPU");
                Check(skinning::GetStatus().adapterName==a.name, "requested GPU used");
                Check(VertexCurrentLine(app.get(),menu)==L"最近一次模型运算："+a.name, "automatic caption follows the active GPU");
                Same(cpu,Read(main.Get(),edge.Get(),stride,count));
                Check(mdl::ResourceAs<IDirect3DVertexBuffer9>(model->vertexBuffer)==main.Get(), "MME body identity");
                Check(mdl::ResourceAs<IDirect3DVertexBuffer9>(model->vertexBuffer2)==edge.Get(), "MME edge identity");
                ++cases;
            }
        }
        if(uv>=0) {
            const auto type=pmx[0].weightType; pmx[0].weightType=static_cast<mdl::PmxWeightType>(4);
            skinning::SetPreference({skinning::Mode::Cpu}); UpdateModelVertexBuffers(app.get(),bytes,world);
            const auto cpu=Read(main.Get(),edge.Get(),stride,count);
            skinning::SetPreference({}); UpdateModelVertexBuffers(app.get(),bytes,world);
            Check(skinning::GetStatus().result==skinning::Result::Unsupported, "QDEF CPU fallback");
            Check(VertexCurrentLine(app.get(),menu)==L"最近一次模型运算：CPU（该模型使用兼容回退）", "automatic caption follows CPU compatibility fallback");
            Same(cpu,Read(main.Get(),edge.Get(),stride,count)); pmx[0].weightType=type;
        }
        skinning::SetPreference({}); UpdateModelVertexBuffers(app.get(),bytes,world);
        const auto* report=skinning::GetAutomaticReport(bytes);
        Check(report && report->measurements.size()==adapters.size()+1, "model selects the startup CPU and GPU measurements");
        double minimum=std::numeric_limits<double>::infinity(); std::uint64_t expected=0;
        for(const auto& measurement:report->measurements) {
            std::printf("measurement id=%016llx valid=%d ms=%.6f\n",
                static_cast<unsigned long long>(measurement.adapterId),measurement.valid,measurement.milliseconds);
            if(measurement.valid && measurement.milliseconds>0 && measurement.milliseconds<minimum) {
                minimum=measurement.milliseconds; expected=measurement.adapterId;
            }
        }
        Check(report->selectedAdapterId==expected, "automatic chooses measured minimum including CPU");
        Check(skinning::GetPreference().mode==skinning::Mode::Automatic, "measured device does not replace saved auto intent");
        const auto calibrated=skinning::GetStatus().automaticCalibrations;
        const auto selected=report->selectedAdapterId;
        UpdateModelVertexBuffers(app.get(),bytes,world);
        Check(skinning::GetStatus().automaticCalibrations==calibrated, "unchanged model uses cached calibration");
        if(selected) Check(skinning::GetStatus().result==skinning::Result::Gpu, "selected GPU used");
        else Check(skinning::GetStatus().result==skinning::Result::Cpu && skinning::GetStatus().cpuFallbacks==0, "faster CPU is a selection not a failure");
        const auto automatic=Read(main.Get(),edge.Get(),stride,count);
        if(uv==4) {
            ComPtr<IDirect3DVertexBuffer9> replacement;
            Check(SUCCEEDED(device->CreateVertexBuffer(count*stride,0,0,D3DPOOL_MANAGED,&replacement,nullptr)), "replacement body buffer");
            model->vertexBuffer=replacement.Get();
            UpdateModelVertexBuffers(app.get(),bytes,world);
            Check(skinning::GetStatus().automaticCalibrations==calibrated, "replacing a model buffer reuses startup calibration");
            Same(automatic,Read(replacement.Get(),edge.Get(),stride,count));
            skinning::ForgetModel(bytes);
            Check(!skinning::GetAutomaticReport(bytes), "model disposal releases its selected profile");
            skinning::InitializeAutomatic(app.get(),device.Get());
            UpdateModelVertexBuffers(app.get(),bytes,world);
            Check(skinning::GetAutomaticReport(bytes)!=nullptr &&
                  skinning::GetStatus().automaticCalibrations==startupCount,
                  "disposing and reloading a model never repeats startup calibration");
            model->vertexBuffer=main.Get();
        }
        skinning::SetPreference({skinning::Mode::Cpu}); UpdateModelVertexBuffers(app.get(),bytes,world);
        Same(Read(main.Get(),edge.Get(),stride,count),automatic);
        Check(!skinning::GetAutomaticReport(bytes), "preference change invalidates calibration");
        skinning::SetPreference({skinning::Mode::Cpu}); UpdateModelVertexBuffers(app.get(),bytes,world);
        Check(skinning::GetStatus().result==skinning::Result::Cpu && skinning::GetStatus().gpuUpdates==0, "live CPU switch");
    }
    Check(skinning::GetStatus().automaticCalibrations==startupCount,
          "different formats, buffer replacements and preferences preserve startup measurements");
    // Recreate process-local state while retaining the disk cache. Empty
    // startup must neither benchmark nor create model/device working buffers.
    skinning::Shutdown(); skinning::SetPreference({});
    skinning::InitializeAutomatic(app.get(),device.Get());
    Check(skinning::GetStatus().automaticCalibrations==0 &&
          skinning::GetStartupMeasurements().size()==6,
          "a later launch loads six profiles without benchmarking");
    namespace cache=skinning::calibration_cache;
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr,executable,32768)!=0,"resolve executable for cache location");
    const auto expectedCache=std::filesystem::path(executable).parent_path()/L"Data"/L"processor-config-MMS-v1.bin";
    Check(cache::Path(L"MMS")==expectedCache,"cache is stored next to mmconfig.ini");
    const auto originalDirectory=std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    Check(cache::Path(L"MMS")==expectedCache,"opening a project elsewhere cannot relocate the cache");
    std::filesystem::current_path(originalDirectory);
    const auto cacheFixture=std::filesystem::path(L".local/artifacts/skinning-cache")/L"fixture.bin";
    const auto profiles=skinning::GetStartupMeasurements();
    Check(cache::Save(cacheFixture,L"test-cpu",adapters,profiles),"save complete calibration fixture");
    std::vector<skinning::StartupMeasurement> restored;
    auto rebooted=adapters;
    for(auto& adapter:rebooted)adapter.id+=1000000;
    std::reverse(rebooted.begin(),rebooted.end());
    Check(cache::Load(cacheFixture,L"test-cpu",rebooted,restored),"remap reboot LUIDs and adapter order");
    if(!restored.empty())for(const auto& m:restored.front().report.measurements) {
        bool found=m.adapterId==0;
        for(const auto& adapter:rebooted)found=found || adapter.id==m.adapterId;
        Check(found,"cached ranking only uses current adapter identifiers");
    }
    Check(!cache::Load(cacheFixture,L"changed-cpu",adapters,restored),"CPU changes invalidate calibration");
    auto changed=adapters;changed.front().hardwareKey+=L"-new-driver";
    Check(!cache::Load(cacheFixture,L"test-cpu",changed,restored),"GPU/driver changes invalidate calibration");
    {std::ofstream corrupt(cacheFixture,std::ios::binary|std::ios::trunc);corrupt<<"truncated";}
    Check(!cache::Load(cacheFixture,L"test-cpu",adapters,restored),"truncated calibration is rejected");
    Check(cache::Save(cacheFixture,L"test-cpu",adapters,profiles),"replace a damaged cache atomically");
    Check(cache::Load(cacheFixture,L"test-cpu",adapters,restored),"replacement cache remains readable");
    wchar_t temp[MAX_PATH]{},path[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH,temp)!=0 && GetTempFileNameW(temp,L"mds",0,path), "temp settings");
    Check(DeleteFileW(path), "remove empty temp file");
    for(const auto& value : std::vector<skinning::Preference>{{},{skinning::Mode::Cpu},{skinning::Mode::Adapter,adapters.front().id,adapters.front().name}}) {
        skinning::SetPreference(value); Check(skinning::SavePreference(path), "save settings");
        skinning::SetPreference({}); skinning::LoadPreference(path);
        Check(skinning::GetPreference().mode==value.mode && skinning::GetPreference().adapterId==value.adapterId &&
              skinning::GetPreference().adapterName==value.adapterName, "reload settings");
    }
    Check(WritePrivateProfileStringW(L"Skinning",L"AdapterId",L"invalid",path), "invalid settings");
    skinning::LoadPreference(path);
    Check(skinning::GetPreference().mode==skinning::Mode::Automatic, "invalid settings fall back to auto");
    Check(DeleteFileW(path), "remove test settings");
    // ---- the processor menu ---------------------------------------------
    // Every choice is written to a test override under the workspace, never to
    // the real %LOCALAPPDATA%\MikuMikuStudio\processor.ini.
    const std::filesystem::path configDir = L".local/artifacts/global-processor";
    std::filesystem::create_directories(configDir);
    const std::wstring configPath = (configDir / L"processor.ini").wstring();
    processor::ResetForTesting();
    processor::SetConfigPathOverrideForTesting(configPath.c_str());
    Check(HandleSkinningCommand(app.get(),kSkinningCpuId), "restore a known selection before the language sweep");

    app->EnglishUI()=static_cast<unsigned char>(2);
    RefreshSkinningMenu(app.get());
    {
        HMENU view=HelpMenu(menu);
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(view,kSkinningMenuId,FALSE,&item) && item.hSubMenu, "processor submenu exists");
        HMENU popup=item.hSubMenu;
        wchar_t label[256]{};
        Check(GetMenuStringW(view,kSkinningMenuId,label,256,MF_BYCOMMAND)>0, "processor caption");
        Check(std::wcscmp(label,L"运算器")==0, "Chinese processor caption");
        Check(std::wcsstr(label,L"蒙皮")==nullptr, "no skinning wording in the caption");
        Check(GetMenuItemCount(popup)>=2, "two host processor role submenus");
        const wchar_t* names[2]={L"主渲染设备",L"顶点计算设备"};
        for(unsigned role=0;role<2;++role) {
            HMENU sub=GetSubMenu(popup,role);
            Check(sub!=nullptr, "role submenu is a popup");
            Check(GetMenuStringW(popup,role,label,256,MF_BYPOSITION)>0 &&
                  std::wcscmp(label,names[role])==0, "role submenu name");
            Check(GetMenuState(sub,0,MF_BYPOSITION)&MF_GRAYED, "current device line is greyed");
            Check(GetMenuState(sub,1,MF_BYPOSITION)&MF_GRAYED, "next launch line is greyed");
            Check(GetMenuItemCount(sub)>=4, "role submenu has header and choices");
        }
        Check(GetMenuStringW(GetSubMenu(popup,0),0,label,256,MF_BYPOSITION)>0 &&
              std::wcsstr(label,L"当前设备")!=nullptr, "renderer shows the current device");
        Check(GetMenuStringW(GetSubMenu(popup,0),1,label,256,MF_BYPOSITION)>0 &&
              std::wcsstr(label,L"下次启动")!=nullptr, "renderer shows the next launch intent");
        Check(GetMenuStringW(GetSubMenu(popup,1),0,label,256,MF_BYPOSITION)>0 &&
              std::wcsstr(label,L"最近一次模型运算")!=nullptr, "vertex line is the global last computation");
        Check(GetMenuState(popup,kMmvFollowRendererId,MF_BYCOMMAND)==UINT(-1),
              "the host processor menu no longer exposes MMV processing");
        Check(!HandleSkinningCommand(app.get(),kMmvCpuId),
              "an MMV processor choice is rejected while its DLL is not loaded");
    }

    // Selecting a device only writes Requested and the file.  Active and the
    // live skinning preference stay as they were: no SetPreference, no rebuild.
    const auto activeBefore=processor::Active();
    const auto preferenceBefore=skinning::GetPreference();
    app->SceneModified()=0;
    Check(HandleSkinningCommand(app.get(),kSkinningCpuId), "vertex CPU command accepted");
    Check(processor::Requested().vertices.mode==processor::Mode::Cpu, "requested vertices is CPU");
    Check(processor::Active().vertices.mode==activeBefore.vertices.mode &&
          processor::Active().vertices.adapterId==activeBefore.vertices.adapterId,
          "active vertices is untouched");
    Check(skinning::GetPreference().mode==preferenceBefore.mode &&
          skinning::GetPreference().adapterId==preferenceBefore.adapterId,
          "live skinning preference is untouched");
    Check(app->SceneModified()==0, "choosing a processor never dirties the project");
    Check(std::filesystem::exists(configPath), "the choice reaches the per-user file");
    {
        HMENU popup=GetSubMenu(HelpMenu(menu),0);(void)popup;
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu after save");
        HMENU vertices=GetSubMenu(item.hSubMenu,1);
        Check(ItemState(vertices,kSkinningCpuId)&MF_CHECKED, "CPU radio follows Requested");
    }

    // An adapter choice carries the full hardware identity the enumeration
    // reported, and the rendered name escapes ampersands and numbers duplicates.
    const auto& computes=processor::ComputeAdapters();
    if(!computes.empty()) {
        Check(HandleSkinningCommand(app.get(),kSkinningAdapterFirstId), "vertex adapter command accepted");
        Check(processor::Requested().vertices.mode==processor::Mode::Adapter, "requested vertices is an adapter");
        Check(processor::Requested().vertices.adapterId==computes.front().adapterId, "requested adapter id");
        Check(processor::Requested().vertices.identity==computes.front().identity, "requested identity is filled");
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu after adapter pick");
        wchar_t label[256]{};
        Check(GetMenuStringW(GetSubMenu(item.hSubMenu,1),kSkinningAdapterFirstId,label,256,MF_BYCOMMAND)>0,
              "adapter item text");
        const auto& name=computes.front().adapterName;
        for(std::size_t i=0;i<name.size();++i) if(name[i]==L'&') {
            Check(std::wcsstr(label,L"&&")!=nullptr, "ampersands are escaped for the menu");
            break;
        }
    }

    // A listed device without a LUID can never be matched back at the next
    // launch, so it is shown greyed and never accepted.
    for(std::size_t i=0;i<std::min<std::size_t>(computes.size(),kSkinningAdapterLimit);++i) {
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu for LUID check");
        const UINT state=ItemState(GetSubMenu(item.hSubMenu,1),
                                      kSkinningAdapterFirstId+static_cast<UINT>(i));
        if(computes[i].adapterId==0) Check(state&MF_GRAYED, "a zero LUID entry is greyed");
        else Check(!(state&MF_GRAYED), "a real LUID entry stays selectable");
    }

    // Several cards of one model share one hardware identity - this machine has
    // four Intel entries - so the saved LUID is what tells them apart.  The
    // radio has to follow the exact card, not the first one whose identity
    // happens to be equal.
    {
        std::size_t twin=0;
        for(std::size_t i=1;i<computes.size();++i)
            if(computes[i].identity==computes.front().identity &&
               computes[i].adapterId!=computes.front().adapterId) { twin=i; break; }
        if(twin!=0) {
            Check(HandleSkinningCommand(app.get(),kSkinningAdapterFirstId+static_cast<UINT>(twin)),
                  "pick the second card of one model");
            RefreshSkinningMenu(app.get());
            MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
            Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu for twin cards");
            HMENU vertices=GetSubMenu(item.hSubMenu,1);
            const UINT expected=kSkinningAdapterFirstId+static_cast<UINT>(twin);
            Check(ItemState(vertices,expected)&MF_CHECKED, "the exact LUID keeps its radio");
            Check(!(ItemState(vertices,kSkinningAdapterFirstId)&MF_CHECKED),
                  "the first same-identity card is not checked instead");
        }
    }

    // A non-device mode is a fresh choice: the previous card's id, name and
    // identity are all dropped.
    Check(HandleSkinningCommand(app.get(),kSkinningCpuId), "vertex CPU command accepted");
    Check(processor::Requested().vertices.adapterId==0 &&
          processor::Requested().vertices.adapterName.empty() &&
          processor::Requested().vertices.identity==processor::DeviceIdentity{},
          "choosing CPU clears the previous device");
    Check(HandleSkinningCommand(app.get(),kSkinningAutomaticId), "vertex automatic command accepted");
    Check(processor::Requested().vertices.adapterId==0 &&
          processor::Requested().vertices.identity==processor::DeviceIdentity{},
          "choosing automatic clears the previous device");

    // The MMV target remains persisted independently of the host menu.
    {
        auto configuration=processor::Requested();
        configuration.mmv=processor::Selection{};
        configuration.mmv.mode=processor::Mode::Automatic;
        std::wstring error;
        Check(processor::SaveRequested(configuration,error), "save an automatic MMV target");
        Check(processor::Requested().mmv.mode==processor::Mode::Automatic,
              "an automatic MMV target remains stored for the plugin");
    }

    // A saved card the enumeration no longer offers must not fall back onto
    // Automatic or Follow: nothing is checked and the reason is shown.
    {
        auto configuration=processor::Requested();
        configuration.vertices=processor::Selection{};
        configuration.vertices.mode=processor::Mode::Adapter;
        configuration.vertices.adapterId=0x123456789ull;
        configuration.vertices.adapterName=L"no-such-card";
        configuration.vertices.identity=processor::DeviceIdentity{1,2,3,4};
        std::wstring error;
        Check(processor::SaveRequested(configuration,error), "save a card that is not present");
        RefreshSkinningMenu(app.get());
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu for missing card");
        HMENU vertices=GetSubMenu(item.hSubMenu,1);
        const auto resolved=processor::ResolveFromTable(configuration.vertices,computes);
        std::printf("missing-card probe: fellBack=%d ordinal=%u id=%016llx name=%ls adapters=%zu\n",
                    resolved.fellBack?1:0,resolved.adapterOrdinal,
                    static_cast<unsigned long long>(resolved.adapterId),
                    resolved.adapterName.c_str(),computes.size());
        for(unsigned id=kSkinningAutomaticId;id<=kSkinningAdapterFirstId+kSkinningAdapterLimit;++id)
            if(ItemState(vertices,id)&MF_CHECKED) {
                std::printf("missing-card probe: checked id=%u\n",id);
                Check(false, "nothing is checked for a missing card");
                break;
            }
        const int count=GetMenuItemCount(vertices);
        wchar_t label[256]{};
        bool explained=false;
        for(int position=0;position<count;++position)
            if(GetMenuStringW(vertices,position,label,256,MF_BYPOSITION)>0 &&
               std::wcsstr(label,L"已保存设备不可用")) { explained=true; break; }
        Check(explained, "a missing saved card is explained in the menu");
    }

    // A save that cannot be written keeps both the radio and Requested, and
    // says so in the status line instead of a message box.
    const auto requestedBefore=processor::Requested();
    const std::wstring blockedPath = (configDir / L"not-a-file").wstring(); std::filesystem::create_directories(blockedPath);
    processor::SetConfigPathOverrideForTesting(blockedPath.c_str());
    Check(HandleSkinningCommand(app.get(),kSkinningAutomaticId), "automatic command accepted");
    Check(processor::Requested().vertices.mode==requestedBefore.vertices.mode &&
          processor::Requested().vertices.adapterId==requestedBefore.vertices.adapterId,
          "a failed save keeps Requested");
    {
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(HelpMenu(menu),kSkinningMenuId,FALSE,&item), "submenu after failed save");
        HMENU popup=item.hSubMenu;
        const int count=GetMenuItemCount(popup);
        wchar_t label[512]{};
        Check(GetMenuStringW(popup,count-1,label,512,MF_BYPOSITION)>0 &&
              std::wcsstr(label,L"保存失败")!=nullptr, "the failure is reported in the menu");
    }
    processor::SetConfigPathOverrideForTesting(configPath.c_str());
    Check(HandleSkinningCommand(app.get(),kSkinningCpuId), "restore a known selection before the language sweep");

    app->EnglishUI()=static_cast<unsigned char>(2);
    for(unsigned language=0;language<3;++language) {
        app->EnglishUI()=static_cast<unsigned char>(language);
        RefreshSkinningMenu(app.get()); RefreshSkinningMenu(app.get());
        HMENU view=HelpMenu(menu);
        int entries=0;
        for(int i=0;i<GetMenuItemCount(view);++i) {
            MENUITEMINFOW probe{sizeof probe}; probe.fMask=MIIM_SUBMENU;
            if(GetMenuItemInfoW(view,UINT(i),TRUE,&probe)&&probe.hSubMenu&&
               GetMenuState(probe.hSubMenu,kSkinningAutomaticId,MF_BYCOMMAND)!=UINT(-1)) ++entries;
        }
        Check(entries==1, "no duplicated submenu after language refresh");
        Check(GetMenuState(view,201,MF_BYCOMMAND)!=UINT(-1), "About stays in Help");
        MENUITEMINFOW item{sizeof item}; item.fMask=MIIM_SUBMENU;
        Check(GetMenuItemInfoW(view,kSkinningMenuId,FALSE,&item) && item.hSubMenu, "submenu exists");
        HMENU popup=item.hSubMenu;
        HMENU vertices=GetSubMenu(popup,1);
        Check(ItemState(vertices,kSkinningCpuId)&MF_CHECKED, "CPU radio state");
        wchar_t label[128]{};
        Check(GetMenuStringW(view,kSkinningMenuId,label,128,MF_BYCOMMAND)>0, "localized caption");
        if(language==2) {
            Check(std::wcscmp(label,L"运算器")==0, "Chinese display menu label");
            Check(GetMenuStringW(popup,0,label,128,MF_BYPOSITION)>0 &&
                  std::wcscmp(label,L"主渲染设备")==0, "Chinese renderer submenu");
            Check(GetMenuStringW(popup,1,label,128,MF_BYPOSITION)>0 &&
                  std::wcscmp(label,L"顶点计算设备")==0, "Chinese vertex submenu");
            Check(GetMenuItemCount(popup)==5, "Chinese processor menu contains host roles only");
        } else if(language==1) {
            Check(std::wcscmp(label,L"Processors")==0, "English display menu label");
            Check(GetMenuStringW(popup,1,label,128,MF_BYPOSITION)>0 &&
                  std::wcscmp(label,L"Vertex compute")==0, "English vertex submenu");
        } else {
            Check(std::wcscmp(label,L"演算器")==0, "Japanese display menu label");
            Check(GetMenuStringW(popup,1,label,128,MF_BYPOSITION)>0 &&
                  std::wcscmp(label,L"頂点演算デバイス")==0, "Japanese vertex submenu");
        }
    }
    processor::ResetForTesting();
    skinning::Shutdown(); model->vertexBuffer=model->vertexBuffer2=nullptr; app->state.hwnd=nullptr;
    SetMenu(window,nullptr); DestroyWindow(window); DestroyMenu(menu);
    std::printf("PASS %u GPU/CPU cases, PMD/PMX UV strides, fallback, settings and menus\n",cases);
}
