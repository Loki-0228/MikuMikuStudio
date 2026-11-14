// MAINTENANCE: CPU regression for legacy MMDPlugin menu ownership. No Win32 hook or DLL.
#include "plugins/legacy_mmd_menu.hpp"
#include <array>
#include <cstdio>
#include <set>

using namespace mikumikustudio;
using namespace mikumikustudio::legacy_mmd_plugin;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (false)

int main() {
    // Native AA, the NVIDIA helper, MME, the native ABI, and the older menu ABI.
    constexpr std::array<unsigned, 10> otherIds = {
        34900, 34920, 34921, 34932, 34936, 40022,
        menu_command_ids::kNativeAbiFirst, menu_command_ids::kNativeAbiLimit - 1,
        menu_command_ids::kPluginMenuFirst, menu_command_ids::kPluginMenuLimit - 1,
    };
    constexpr unsigned first = menu_command_ids::kLegacyObserverFirst;
    constexpr unsigned limit = menu_command_ids::kLegacyObserverLimit;
    CHECK(limit - first == 0x800);

    std::set<unsigned> menu(otherIds.begin(), otherIds.end());
    menu.insert(first); // A menu item already present at allocation must not become observer-owned.
    MenuIdAllocator allocator;
    MenuIds observer, sibling, replacement;
    const auto inUse = [&](unsigned id) { return menu.count(id) != 0; };

    CHECK(allocator.Allocate(observer, inUse) == static_cast<int>(first + 1));
    menu.insert(first + 1);
    CHECK(allocator.Allocate(sibling, inUse) == static_cast<int>(first + 2));
    menu.insert(first + 2);
    CHECK(observer.ObservesCommand(first + 1, 0, false, true));
    CHECK(!observer.ObservesCommand(first + 1, 1, false, true)); // Accelerator
    CHECK(!observer.ObservesCommand(first + 1, 0, true, true)); // Child control
    CHECK(!observer.ObservesCommand(first + 1, 0, false, false)); // Other window
    CHECK(!observer.ObservesCommand(first, 0, false, true));
    CHECK(!observer.ObservesCommand(first + 2, 0, false, true));
    CHECK(!observer.ObservesCommand(limit - 1, 0, false, true)); // Not allocated yet
    CHECK(!sibling.ObservesCommand(first + 1, 0, false, true));
    CHECK(sibling.ObservesCommand(first + 2, 0, false, true));

    // Exhaust the old collision offsets and reach the upper end of the reserved block.
    for (unsigned id = first + 3; id < limit - 1; ++id) {
        CHECK(allocator.Allocate(observer, inUse) == static_cast<int>(id));
        CHECK(observer.ObservesCommand(id, 0, false, true));
        menu.insert(id);
    }
    for (const unsigned id : otherIds) {
        CHECK(!menu_command_ids::IsLegacyObserver(id));
        CHECK(!observer.ObservesCommand(id, 0, false, true));
    }
    CHECK(!observer.ObservesCommand(limit, 0, false, true));

    unsigned removed = 0;
    const auto remove = [&](unsigned id) { removed += static_cast<unsigned>(menu.erase(id)); };
    CHECK(observer.Any());
    observer.RemoveAll(remove);
    CHECK(removed == limit - first - 3);
    CHECK(!observer.Any());
    CHECK(!observer.ObservesCommand(first + 1, 0, false, true));
    CHECK(!inUse(first + 1));
    CHECK(inUse(first));
    CHECK(inUse(first + 2));
    CHECK(sibling.ObservesCommand(first + 2, 0, false, true));
    for (const unsigned id : otherIds) CHECK(inUse(id));

    CHECK(allocator.Allocate(replacement, inUse) == static_cast<int>(limit - 1));
    CHECK(replacement.ObservesCommand(limit - 1, 0, false, true));
    CHECK(!replacement.ObservesCommand(first + 1, 0, false, true)); // Stale queued command
    CHECK(allocator.Allocate(replacement, inUse) == -1);
    CHECK(allocator.Allocate(replacement, inUse) == -1);
    std::puts("PASS legacy MMDPlugin menu command allocation, dispatch and teardown");
    return 0;
}
