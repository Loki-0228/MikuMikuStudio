// Hidden native controls exercise the production loader completion hook. No
// model files, D3D device, application host, or message loop are required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/model_load_ui.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace mikudancestudio;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
struct Fixture {
    HWND window = CreateWindowExW(0, L"STATIC", L"MMS model load UI test", WS_OVERLAPPED,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    Fixture() {
        Check(window != nullptr, "create hidden owner");
        for (int id : {443, 434}) {
            Check(CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
                0, 0, 200, 100, window, reinterpret_cast<HMENU>(INT_PTR(id)),
                GetModuleHandleW(nullptr), nullptr) != nullptr, "create hidden combo");
        }
        model->hwnd = window;
        Seed();
    }
    ~Fixture() { DestroyWindow(window); }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    void Seed() {
        for (int id : {443, 434}) {
            const auto combo = GetDlgItem(window, id);
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"old selection"));
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
        }
        for (auto& selected : model->selectedMorphs) selected = 123;
    }
    bool Unchanged() const {
        for (int id : {443, 434}) {
            const auto combo = GetDlgItem(window, id);
            wchar_t text[64]{};
            if (SendMessageW(combo, CB_GETCOUNT, 0, 0) != 1 ||
                SendMessageW(combo, CB_GETCURSEL, 0, 0) != 0 ||
                SendMessageW(combo, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(text)) != 13 ||
                std::wcscmp(text, L"old selection") != 0) return false;
        }
        for (const auto selected : model->selectedMorphs) if (selected != 123) return false;
        return true;
    }
    bool Refreshed() const {
        if (SendDlgItemMessageW(window, 443, CB_GETCOUNT, 0, 0) != 0 ||
            SendDlgItemMessageW(window, 434, CB_GETCOUNT, 0, 0) <= 1) return false;
        for (const auto selected : model->selectedMorphs) if (selected != -1) return false;
        return true;
    }
};
}

int main() {
    Fixture a, b;
    Check(!model_load_ui::Deferred(a.window), "default is immediate refresh");
    model_load_ui::PostLoadInitOrDefer(a.Raw());
    Check(a.Refreshed(), "default loader hook preserves PostLoadInit behavior");
    a.Seed();
    {
        model_load_ui::DeferredRefresh outer(a.window);
        Check(model_load_ui::Deferred(a.window) && !model_load_ui::Deferred(b.window), "scope is owner specific");
        model_load_ui::PostLoadInitOrDefer(a.Raw());
        Check(a.Unchanged(), "deferred loader does not change old controls or model selection");
        {
            model_load_ui::DeferredRefresh inner(a.window);
            model_load_ui::PostLoadInitOrDefer(a.Raw());
            Check(a.Unchanged(), "nested owner scope defers refresh");
            {
                model_load_ui::DeferredRefresh other(b.window);
                Check(model_load_ui::Deferred(a.window) && model_load_ui::Deferred(b.window), "nested different owners both remain deferred");
                model_load_ui::PostLoadInitOrDefer(b.Raw());
                Check(b.Unchanged(), "nested owner remains untouched");
            }
            Check(!model_load_ui::Deferred(b.window), "different owner scope unwinds separately");
        }
        Check(model_load_ui::Deferred(a.window), "inner scope does not release outer scope");
        model_load_ui::PostLoadInitOrDefer(b.Raw());
        Check(b.Refreshed() && a.Unchanged(), "unrelated owner keeps default refresh behavior");
        bool wrongThreadRejected = false, isolated = false;
        std::thread worker([&] {
            isolated = !model_load_ui::Deferred(a.window);
            try { model_load_ui::DeferredRefresh invalid(a.window); }
            catch (const std::invalid_argument&) { wrongThreadRejected = true; }
        });
        worker.join();
        Check(wrongThreadRejected && isolated && model_load_ui::Deferred(a.window), "wrong thread cannot create or disturb owner scope");
        PostLoadInit(a.Raw());
        Check(a.Refreshed(), "caller can explicitly publish after successful commit inside scope");
        a.Seed();
    }
    Check(!model_load_ui::Deferred(a.window) && a.Unchanged(), "scope exit does not automatically publish failed or cancelled work");
    try {
        model_load_ui::DeferredRefresh cancelled(a.window);
        model_load_ui::PostLoadInitOrDefer(a.Raw());
        throw std::runtime_error("simulated loader failure");
    } catch (const std::runtime_error&) {}
    Check(!model_load_ui::Deferred(a.window) && a.Unchanged(), "exception unwinding restores scope without refreshing");
    bool invalidRejected = false;
    try { model_load_ui::DeferredRefresh invalid(nullptr); }
    catch (const std::invalid_argument&) { invalidRejected = true; }
    Check(invalidRejected && !model_load_ui::Deferred(a.window), "invalid owner rejected without scope state change");
    model_load_ui::PostLoadInitOrDefer(a.Raw());
    Check(a.Refreshed(), "subsequent normal load refreshes after cancellation");
    std::printf("model load UI checks=%u passed\n", checks);
    return 0;
}
