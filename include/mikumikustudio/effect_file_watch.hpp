#pragma once
#include <cstdint>
namespace mikumikustudio::effect_file_watch {
struct Stats { std::uint64_t reads = 0, hits = 0, notifications = 0; unsigned files = 0, directories = 0; };
// Keep native MME's auto-reload toggle and reload procedure. Cache only its
// repeated timestamp query; compiler loads and manual reloads bypass the cache.
void Install();
Stats GetStats();
}
