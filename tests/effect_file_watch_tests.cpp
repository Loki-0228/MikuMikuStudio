#include "../src/exports/effect_file_watch_cache.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
using namespace mikumikustudio::effect_file_watch;
std::uint64_t __fastcall Read(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    FILETIME time{}; const BOOL ok = GetFileTime(file, nullptr, nullptr, &time); CloseHandle(file);
    return ok ? (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime : 0;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(std::string(argv[1]) + "-" + std::to_string(GetCurrentProcessId()));
    assert(std::filesystem::create_directory(directory));
    const auto path = (directory / "effect.fx").string();
    const auto other = (directory / "unrelated.fx").string();
    std::ofstream(path) << "first";
    {
        Cache cache;
        auto stamp = cache.Get(path.c_str(), Read);
        assert(stamp && stamp == Read(path.c_str()));
        for (int i = 0; i < 1000; ++i) assert(cache.Get(path.c_str(), Read) == stamp);
        assert(cache.GetStats().reads == 1 && cache.GetStats().hits == 1000);
        std::ofstream(other) << "unrelated";
        Sleep(30); assert(cache.Get(path.c_str(), Read) == stamp);
        assert(cache.GetStats().reads == 1);
        const auto awaitChange = [&](std::uint64_t previous, bool missing) {
            const auto end = GetTickCount64() + 2000;
            std::uint64_t actual = previous;
            do { Sleep(5); actual = cache.Get(path.c_str(), Read); }
            while (actual == previous && GetTickCount64() < end);
            assert(actual != previous && (actual == 0) == missing && actual == Read(path.c_str()));
            return actual;
        };
        std::ofstream(path) << "saved longer";
        stamp = awaitChange(stamp, false);
        const auto temporary = directory / "editor.tmp";
        std::ofstream(temporary) << "atomic replacement";
        assert(MoveFileExA(temporary.string().c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING));
        stamp = awaitChange(stamp, false);
        assert(DeleteFileA(path.c_str()));
        stamp = awaitChange(stamp, true);
        std::ofstream(path) << "recreated";
        stamp = awaitChange(stamp, false);
        std::cout << "PASS: unchanged cache, unrelated write, save, atomic rename, deletion, recreation; reads="
            << cache.GetStats().reads << " hits=" << cache.GetStats().hits << '\n';
    }
    // Only this executable's two temporary fixture files are removed.
    assert(DeleteFileA(path.c_str())); assert(DeleteFileA(other.c_str()));
    assert(RemoveDirectoryW(directory.c_str()));
}
