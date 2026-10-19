// MAINTENANCE: regression-keep; source-level MMDModern lookup equivalents.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/draw_order.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace mikumikustudio;
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
    auto renderer = std::make_unique<D3DRenderer>();
    *renderer = {};
    wchar_t first[] = L"纹理/A.png", same[] = L"纹理/A.png", lower[] = L"纹理/a.png";
    renderer->resourcePool[0].heapBuffer = first;
    renderer->resourcePool[1].heapBuffer = same;
    renderer->resourcePool[2].heapBuffer = lower;
    InvalidateTextureLookup();
    Check(FindTextureResource(nullptr, first) == nullptr, "null renderer");
    Check(FindTextureResource(renderer.get(), nullptr) == nullptr, "null path");
    Check(FindTextureResource(renderer.get(), L"") == nullptr, "empty path");
    Check(FindTextureResource(renderer.get(), L"absent") == nullptr, "missing path");
    for (int i = 0; i < 1000; ++i)
        Check(FindTextureResource(renderer.get(), same) == &renderer->resourcePool[0], "first duplicate wins");
    Check(FindTextureResource(renderer.get(), lower) == &renderer->resourcePool[2], "case-sensitive path");
    renderer->resourcePool[0].tag = 12345;
    Check(FindTextureResource(renderer.get(), first)->tag == 12345, "reads current tag after reload");
    InvalidateTextureLookup();
    renderer->resourcePool[0].heapBuffer = lower;
    Check(FindTextureResource(renderer.get(), first) == &renderer->resourcePool[1], "replacement invalidates first match");
    InvalidateTextureLookup();
    renderer->resourcePool[1].heapBuffer = nullptr;
    Check(FindTextureResource(renderer.get(), first) == nullptr, "first null terminates used pool");
    std::deque<std::wstring> names;
    for (int i = 0; i < 10000; ++i) {
        names.push_back(L"texture/" + std::to_wstring(i));
        renderer->resourcePool[i].heapBuffer = names.back().data();
    }
    InvalidateTextureLookup();
    Check(FindTextureResource(renderer.get(), L"texture/9999") == &renderer->resourcePool[9999], "full pool final slot");
    Check(FindTextureResource(renderer.get(), L"texture/10000") == nullptr, "full pool miss");
    InvalidateTextureLookup();
    *renderer = {};
    Check(FindTextureResource(renderer.get(), L"texture/0") == nullptr, "renderer address reused after reset");

    struct Object { int order; } objects[] = {{254}, {0}, {254}, {-1}, {255}, {7}};
    std::array<Object*, 255> slots{};
    slots[0] = &objects[0]; slots[4] = &objects[1]; slots[9] = &objects[2];
    slots[10] = &objects[3]; slots[11] = &objects[4]; slots[254] = &objects[5];
    int resolves = 0;
    const auto build = [&] {
        return BuildDrawOrder<255>([&](int i) { ++resolves; return slots[i]; },
            [](Object* p) { return p->order; });
    };
    auto order = build();
    Check(resolves == 255, "one slot resolution per pass");
    Check(order[254].model == &objects[0] && order[254].slot == 0, "first duplicate order wins");
    Check(order[7].slot == 254 && order[0].slot == 4 && !order[1].model, "sparse and final slots");
    objects[0].order = 3; slots[4] = nullptr;
    order = build();
    Check(order[3].model == &objects[0] && order[254].model == &objects[2] && !order[0].model,
          "next traversal observes edits and deletion");
    std::puts("PASS texture lifetime, duplicate/case/full-pool lookup, and linear draw-order construction");
}
