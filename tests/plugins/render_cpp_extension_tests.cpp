#include <mms/cpp/render.hpp>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

static_assert(!std::is_move_assignable_v<mms::sdk::RenderLease>);
static_assert(!std::is_invocable_v<decltype(&mms::sdk::RenderLease::TransferFrom),
    mms::sdk::RenderLease&, mms::sdk::RenderLease&&>);

namespace {
int checks = 0;
void Check(bool value) { ++checks; if (!value) throw checks; }
struct Host {
    bool failRelease = false, old = false, incomplete = false, badHeader = false;
    unsigned acquired = 0, released = 0, frameSet = 0, frameClear = 0, phaseSet = 0, phaseClear = 0;
    MmsRenderFrameCallbackV1 frameCallback = nullptr;
    MmsRenderPhaseCallbackV1 phaseCallback = nullptr;
    void *frameContext = nullptr, *phaseContext = nullptr;
    MmsRenderApiV1_2 api{};
    MmsHostV1 host{};
    Host() {
        api.h = {sizeof(api), 1, 0}; api.context = this;
        api.query = [](void*, MmsRenderInfoV1*) -> MmsResult { return MMS_OK; };
        api.acquireLease = [](void* c, const MmsRenderLeaseDescV1*, MmsHandle* h) -> MmsResult {
            *h = ++static_cast<Host*>(c)->acquired; return MMS_OK;
        };
        api.releaseLease = [](void* c, MmsHandle) -> MmsResult {
            auto& h = *static_cast<Host*>(c); ++h.released; return h.failRelease ? MMS_E_BUSY : MMS_OK;
        };
        api.setFrameCallback = [](void* c, MmsRenderFrameCallbackV1 cb, void* user) -> MmsResult { auto& h=*static_cast<Host*>(c); ++h.frameSet; h.frameCallback=cb; h.frameContext=user; return MMS_OK; };
        api.clearFrameCallback = [](void* c) { ++static_cast<Host*>(c)->frameClear; };
        api.setPhaseCallback = [](void* c, MmsRenderPhaseCallbackV1 cb, void* user) -> MmsResult { auto& h=*static_cast<Host*>(c); ++h.phaseSet; h.phaseCallback=cb; h.phaseContext=user; return MMS_OK; };
        api.clearPhaseCallback = [](void* c) { ++static_cast<Host*>(c)->phaseClear; };
        host.h = {sizeof(host), 1, 0}; host.context = this;
        host.queryService = [](void* c, MmsString name, uint32_t major, uint32_t minor, uint32_t size, void* out) -> MmsResult {
            auto& h = *static_cast<Host*>(c);
            if (std::string_view(name.data, size_t(name.bytes)) != "mms.render" || major != 1 || minor > 2) return MMS_E_NOT_FOUND;
            if (h.old && minor != 0) return MMS_E_VERSION;
            const size_t need = minor == 2 ? sizeof(MmsRenderApiV1_2) : minor == 1 ? sizeof(MmsRenderApiV1_1) : sizeof(MmsRenderApiV1);
            if (!out || size < need) return MMS_E_ARGUMENT;
            auto api = h.api; api.h.size = static_cast<uint32_t>(need);
            if (h.badHeader && minor) api.h.size = sizeof(MmsHeader);
            if (h.incomplete) api.clearPhaseCallback = nullptr;
            std::memcpy(out, &api, need); return MMS_OK;
        };
    }
};
}
int main() {
    try {
        using namespace mms::sdk;
        Host h; RenderServices s; Check(RenderServices::Query(h.host, s) == MMS_OK);
        Check(s.SetFrameCallback(nullptr, nullptr) == MMS_E_NOT_READY);
        Check(s.RequireFrame() == MMS_OK && s.RequirePhase() == MMS_OK);
        auto frame = +[](void*, const MmsRenderInfoV1*) {};
        auto phase = +[](void*, const MmsRenderInfoV1*, uint32_t) {};
        Check(s.SetFrameCallback(frame, &h) == MMS_OK && h.frameCallback == frame && h.frameContext == &h);
        Check(s.ClearFrameCallback() == MMS_OK);
        Check(s.SetPhaseCallback(phase, &h) == MMS_OK && h.phaseCallback == phase && h.phaseContext == &h);
        Check(s.ClearPhaseCallback() == MMS_OK);
        Check(h.frameSet == 1 && h.frameClear == 1 && h.phaseSet == 1 && h.phaseClear == 1);
        RenderLease a, b;
        Check(s.Acquire("a", nullptr, nullptr, a) == MMS_OK && a.get() == 1);
        Check(s.Acquire("b", nullptr, nullptr, a) == MMS_E_BUSY && h.acquired == 1);
        Check(s.Acquire("b", nullptr, nullptr, b) == MMS_OK && b.get() == 2);
        h.failRelease = true; Check(a.Release() == MMS_E_BUSY && a.get() == 1);
        Check(a.TransferFrom(b) == MMS_E_BUSY && a.get() == 1 && b.get() == 2);
        Check(a.TransferFrom(a) == MMS_OK && a.get() == 1);
        h.failRelease = false; Check(a.TransferFrom(b) == MMS_OK && a.get() == 2 && !b.valid());
        Check(a.Release() == MMS_OK && !a.valid());
        Host old; old.old = true; RenderServices basic;
        Check(RenderServices::Query(old.host, basic) == MMS_OK && basic.RequirePhase() == MMS_E_VERSION);
        Host broken; broken.incomplete = true; RenderServices partial;
        Check(RenderServices::Query(broken.host, partial) == MMS_OK && partial.RequirePhase() == MMS_E_UNSUPPORTED);
        Host bad; bad.badHeader = true; RenderServices rejected;
        Check(RenderServices::Query(bad.host, rejected) == MMS_OK && rejected.RequireFrame() == MMS_E_VERSION);
        Check(rejected.RequirePhase() == MMS_E_VERSION);
        std::printf("render_cpp_extension_tests: %d checks passed\n", checks); return 0;
    } catch (int at) { std::fprintf(stderr, "check %d failed\n", at); return 1; }
}
