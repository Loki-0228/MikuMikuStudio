// MAINTENANCE: regression-keep; source-level MMDModern lookup equivalents.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/draw_order.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstdint>

using namespace mikumikustudio;
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void FullModelDrawOrder() {
    constexpr std::size_t capacity = 255;
    struct Object { std::uint8_t order; };
    std::array<Object, capacity> objects{};
    std::array<Object*, capacity> slots{};
    std::array<int, capacity> positions{};
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        const auto identity = (slot * 73 + 19) % capacity;
        objects[identity].order = static_cast<std::uint8_t>(identity + 1);
        slots[slot] = &objects[identity];
        positions[identity] = static_cast<int>(slot);
    }
    unsigned resolves = 0;
    const auto build = [&] {
        return BuildDrawOrder<capacity, capacity + 1>(
            [&](int slot) { ++resolves; return slots[slot]; },
            [](Object* object) { return object->order; });
    };
    auto entries = build();
    Check(resolves == capacity && entries.size() == capacity + 1,
          "255 model slots have 256 order entries including legacy zero");
    unsigned visited = 0;
    for (const auto& entry : entries) if (entry.model) ++visited;
    Check(visited == capacity && !entries[0].model, "all 255 independently occupied model slots reach the draw traversal");
    for (std::size_t order = 1; order <= capacity; ++order)
        Check(entries[order].model == &objects[order - 1] && entries[order].slot == positions[order - 1],
              "each one-based model order resolves its original identity and sparse-independent slot");
    Check(entries[255].model == &objects[254], "the 255th model participates at full capacity");

    objects[0].order = 0;
    entries = build();
    Check(entries[0].model == &objects[0] && !entries[1].model && entries[255].model == &objects[254],
          "legacy zero order remains available beside the full one-based endpoint");
    objects[1].order = 255;
    entries = build();
    const auto first = positions[1] < positions[254] ? 1 : 254;
    Check(entries[255].model == &objects[first] && entries[255].slot == positions[first],
          "duplicate order 255 keeps the first occupied physical slot");
    slots[positions[first]] = nullptr;
    entries = build();
    const auto remaining = first == 1 ? 254 : 1;
    Check(entries[255].model == &objects[remaining] && entries[255].slot == positions[remaining],
          "deleting a sparse slot exposes the remaining order-255 model on the next traversal");

    struct InvalidOrder { int order; } invalid[] = {{-1}, {256}, {255}};
    std::array<InvalidOrder*, capacity> invalidSlots{};
    invalidSlots[0] = &invalid[0]; invalidSlots[1] = &invalid[1]; invalidSlots[254] = &invalid[2];
    const auto bounded = BuildDrawOrder<capacity, capacity + 1>(
        [&](int slot) { return invalidSlots[slot]; }, [](InvalidOrder* object) { return object->order; });
    Check(bounded[255].model == &invalid[2] && !bounded[0].model,
          "negative and 256 model orders are rejected without hiding legal order 255");

    for (std::size_t slot = 0; slot < capacity; ++slot) {
        objects[slot].order = static_cast<std::uint8_t>(slot);
        slots[slot] = &objects[slot];
    }
    const auto accessories = BuildDrawOrder<capacity>(
        [&](int slot) { return slots[slot]; }, [](Object* object) { return object->order; });
    Check(accessories.size() == capacity && accessories[254].model == &objects[254],
          "all 255 accessory slots retain the zero-based default order range");
    const auto accessoryInvalid = BuildDrawOrder<capacity>(
        [&](int slot) { return invalidSlots[slot]; }, [](InvalidOrder* object) { return object->order; });
    for (const auto& entry : accessoryInvalid)
        Check(!entry.model, "the accessory default still rejects orders -1, 255 and 256");
    std::puts("PASS all 255 model identities and order 255, legacy zero, duplicate/sparse traversal and accessory bounds");
}

int main() {
    FullModelDrawOrder();
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
