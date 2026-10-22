// MAINTENANCE: regression-keep; private menu observation must remain read-only.
#include "../src/plugins/native_menu_snapshot.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace menu = mikumikustudio::plugins::native_menu_snapshot;
namespace {
thread_local long failAfter = -1;
unsigned checks = 0, failures = 0, callbacks = 0;
void Check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s error=%lu\n", name, GetLastError()); }
}
void Require(bool value, const char* name) { Check(value, name); if (!value) std::exit(2); }
LRESULT CALLBACK Window(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_INITMENU || message == WM_INITMENUPOPUP || message == WM_COMMAND)
        ++callbacks;
    return DefWindowProcW(window, message, w, l);
}
struct Owner {
    HWND window = nullptr;
    HMENU root = nullptr;
    Owner() {
        WNDCLASSW wc{}; wc.lpfnWndProc = Window; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MmsPrivateMenuObservationTest";
        if (!RegisterClassW(&wc)) Require(GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register owned window class");
        window = CreateWindowExW(0, wc.lpszClassName, L"private test", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        Require(window != nullptr, "create hidden test window without showing or activating it");
    }
    void Attach(HMENU value) { root = value; Require(SetMenu(window, root) != FALSE, "attach only an owned test menu"); }
    ~Owner() { if (window) { SetMenu(window, nullptr); DestroyWindow(window); } if (root) DestroyMenu(root); }
};
void Add(HMENU parent, UINT id, const wchar_t* text, HMENU sub = nullptr, UINT type = MFT_STRING, UINT state = 0) {
    MENUITEMINFOW item{}; item.cbSize = sizeof(item);
    item.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
    item.wID = id; item.fType = type; item.fState = state; item.hSubMenu = sub;
    if (!(type & (MFT_OWNERDRAW | MFT_SEPARATOR | MFT_BITMAP))) {
        item.fMask |= MIIM_STRING; item.dwTypeData = const_cast<wchar_t*>(text);
    } else { item.fMask |= MIIM_DATA; item.dwItemData = 1; }
    Require(InsertMenuItemW(parent, static_cast<UINT>(GetMenuItemCount(parent)), TRUE, &item) != FALSE, "append owned fixture row");
}
menu::Snapshot Sentinel() {
    menu::Snapshot s; s.rootMenu = 123; s.items.emplace_back(); s.items[0].text = L"sentinel";
    s.items[0].commandId = 456; return s;
}
bool Unchanged(const menu::Snapshot& s) {
    return s.rootMenu == 123 && s.items.size() == 1 && s.items[0].text == L"sentinel" && s.items[0].commandId == 456;
}
// Patch only this test executable's import slot, forwarding every ordinary API
// call. This exercises the separately compiled production module with real
// Win32 menus and deterministic changes between its length and text queries.
struct QueryProbe {
    using Fn = BOOL(WINAPI*)(HMENU,UINT,BOOL,LPMENUITEMINFOW);
    static inline Fn real = nullptr;
    static inline int action = 0;
    static inline unsigned mutations = 0;
    ULONG_PTR* slot = nullptr;
    ULONG_PTR previous = 0;
    static BOOL WINAPI Query(HMENU m, UINT row, BOOL byPosition, LPMENUITEMINFOW info) {
        if (action == 3) { action = 0; SetLastError(ERROR_INVALID_MENU_HANDLE); return FALSE; }
        if ((action == 1 || action == 2) && (info->fMask & MIIM_STRING) && info->dwTypeData) {
            const int chosen = action; action = 0; ++mutations;
            MENUITEMINFOW replacement{}; replacement.cbSize = sizeof(replacement); replacement.fMask = MIIM_STRING;
            replacement.dwTypeData = const_cast<wchar_t*>(chosen == 1 ? L"abc" : L"abcdefgh");
            Require(SetMenuItemInfoW(m, row, byPosition, &replacement) != FALSE, "mutate only owned label between observed queries");
        }
        return real(m,row,byPosition,info);
    }
    QueryProbe() {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
            directory.VirtualAddress && d->Name && !slot; ++d) {
            if (!d->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
            auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names,++addresses) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (!std::strcmp(reinterpret_cast<const char*>(name->Name), "GetMenuItemInfoW")) {
                    slot = reinterpret_cast<ULONG_PTR*>(&addresses->u1.Function); break;
                }
            }
        }
        Require(slot != nullptr, "locate actual production menu query import");
        DWORD protection = 0;
        Require(VirtualProtect(slot,sizeof *slot,PAGE_READWRITE,&protection) != FALSE,"protect owned query slot");
        previous = *slot; real = reinterpret_cast<Fn>(previous); *slot = reinterpret_cast<ULONG_PTR>(&Query);
        DWORD ignored = 0; Require(VirtualProtect(slot,sizeof *slot,protection,&ignored) != FALSE,"restore slot protection");
    }
    ~QueryProbe() {
        action = 0; DWORD protection = 0;
        const bool writable=VirtualProtect(slot,sizeof *slot,PAGE_READWRITE,&protection)!=FALSE;
        Check(writable,"reopen only the owned import slot for restoration");
        if (writable) {
            *slot = previous;Check(*slot==previous,"restore actual import address");
            DWORD ignored = 0;Check(VirtualProtect(slot,sizeof *slot,protection,&ignored)!=FALSE,"restore original import protection");
        }
    }
};
void BasicAndBudgets() {
    Owner owner; menu::Snapshot output = Sentinel();
    Check(menu::Capture(owner.window, {}, output) == menu::Status::ok && output.rootMenu == 0 && output.items.empty(), "no menu publishes an empty snapshot");
    auto root = CreateMenu(), sub = CreatePopupMenu(); Require(root && sub,"allocate owned menu tree");
    Add(root,7,L"\x4e2d\x6587\xd83d\xde3a",sub);
    Add(sub,7,L"same ID",nullptr,MFT_STRING,MFS_CHECKED|MFS_DISABLED);
    Add(sub,0,L"",nullptr,MFT_SEPARATOR); Add(sub,9,L"",nullptr,MFT_OWNERDRAW);
    Add(root,7,L""); owner.Attach(root);
    const auto before = callbacks;
    Check(menu::Capture(owner.window,{},output) == menu::Status::ok,"capture real Unicode hierarchy");
    Check(callbacks == before,"observation never initializes menus or invokes commands");
    Require(output.items.size() == 5,"all placement rows preserved");
    Check(output.items[0].text == L"\x4e2d\x6587\xd83d\xde3a" && output.items[0].textKnown,"UTF-16 surrogate label intact");
    Check(output.items[1].parent == 0 && output.items[1].position == 0 && output.items[4].parent == UINT32_MAX,"depth-first parent and physical positions");
    Check(output.items[0].commandId == 7 && output.items[1].commandId == 7 && output.items[4].commandId == 7,"duplicate IDs do not collapse placements");
    Check((output.items[1].state & (MFS_CHECKED|MFS_DISABLED)) == (MFS_CHECKED|MFS_DISABLED),"actual checked and disabled states");
    Check(!output.items[2].textKnown && !output.items[3].textKnown && output.items[2].text.empty() && output.items[3].text.empty(),"separator and owner-draw data never dereferenced");
    Check(output.items[4].textKnown && output.items[4].text.empty(),"known empty text differs from unknown label");
    menu::Limits limits; limits.maxItems=5; limits.maxTextUnits=7; limits.maxTotalTextUnits=11; limits.maxDepth=1;
    Check(menu::Capture(owner.window,limits,output)==menu::Status::ok,"exact inclusive item, UTF-16, total and row-depth budgets");
    for(unsigned kind=0;kind<3;++kind) {
        auto bounded=limits; if(kind==0)bounded.maxItems=4;else if(kind==1)bounded.maxTextUnits=6;else bounded.maxTotalTextUnits=10;
        output=Sentinel();Check(menu::Capture(owner.window,bounded,output)==menu::Status::limit&&Unchanged(output),"overflow leaves complete previous output unchanged");
    }
    output=Sentinel();limits.maxDepth=0;
    Check(menu::Capture(owner.window,limits,output)==menu::Status::invalid_argument&&Unchanged(output),"zero budget refused atomically");
    Check(menu::Capture(nullptr,{},output)==menu::Status::invalid_argument&&Unchanged(output),"null owner refused atomically");
    Check(menu::Capture(reinterpret_cast<HWND>(static_cast<UINT_PTR>(1)),{},output)==menu::Status::invalid_window&&Unchanged(output),"invalid window refused atomically");
    menu::Status threaded=menu::Status::ok;
    std::thread thread([&]{threaded=menu::Capture(owner.window,{},output);});thread.join();
    Check(threaded==menu::Status::wrong_thread&&Unchanged(output),"worker cannot query another thread's menu");
}
void MutationAndFailure() {
    Owner owner; auto root=CreateMenu();Require(root!=nullptr,"create mutation fixture");Add(root,1,L"abcd");owner.Attach(root);
    QueryProbe probe;
    for(int action : {1,2,3}) {
        MENUITEMINFOW initial{};initial.cbSize=sizeof(initial);initial.fMask=MIIM_STRING;initial.dwTypeData=const_cast<wchar_t*>(L"abcd");
        Require(SetMenuItemInfoW(root,0,TRUE,&initial)!=FALSE,"reset only owned mutation fixture");
        auto output=Sentinel();QueryProbe::action=action;const auto status=menu::Capture(owner.window,{},output);
        Check(status==(action==3?menu::Status::failed:menu::Status::changed), action==1?"one-unit shrink is an observed change":action==2?"growth beyond the old buffer is never silently truncated":"query failure is reported");
        Check(Unchanged(output),"failed or changed observations never publish partial output");
    }
    Check(QueryProbe::mutations==2,"both mutation probes executed against production queries");
}
void DepthAndAllocation() {
    Owner owner;auto root=CreateMenu(),sub=CreatePopupMenu(),empty=CreatePopupMenu();Require(root&&sub&&empty,"create owned depth fixture");
    Add(root,1,L"root",sub);Add(sub,2,L"child",empty);owner.Attach(root);
    menu::Limits limits;limits.maxDepth=1;menu::Snapshot output;
    Check(menu::Capture(owner.window,limits,output)==menu::Status::ok&&output.items.size()==2,"empty submenu adds no rows beyond maximum depth");
    Add(empty,3,L"deep");output=Sentinel();
    Check(menu::Capture(owner.window,limits,output)==menu::Status::limit&&Unchanged(output),"actual row beyond depth budget refused atomically");
    // Force both the temporary text buffer and published text beyond wstring's
    // small-string storage, so failures cover text allocations as well as rows.
    const std::wstring longText(160,L'\x4e2d');Add(empty,4,longText.c_str());
    bool reachedSuccess=false;unsigned rejected=0;
    for(long allocation=0;allocation<64;++allocation) {
        output=Sentinel();failAfter=allocation;const auto status=menu::Capture(owner.window,{},output);failAfter=-1;
        if(status==menu::Status::ok){reachedSuccess=true;break;}
        ++rejected;Check(status==menu::Status::no_memory&&Unchanged(output),"allocation failure leaves output unchanged");
    }
    Check(rejected>0&&reachedSuccess,"allocation sweep covers failures and a complete capture");
}
}
void* operator new(std::size_t bytes) {
    if(failAfter==0)throw std::bad_alloc();if(failAfter>0)--failAfter;
    if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main() {
    BasicAndBudgets();MutationAndFailure();DepthAndAllocation();
    std::printf("%s native menu snapshot: %u checks, %u failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
