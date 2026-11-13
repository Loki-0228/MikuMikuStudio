#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "mikumikustudio/background_picture_policy.hpp"
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace media = mikumikustudio::media;
namespace settings = mikumikustudio::system_settings;
using settings::Result;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (false)
struct Fixture {
    fs::path root, file;
    Fixture() {
        wchar_t temp[MAX_PATH]{}, unique[MAX_PATH]{};
        const auto count = GetTempPathW(MAX_PATH, temp);
        if (!count || count >= MAX_PATH || !GetTempFileNameW(temp, L"mbp", 0, unique)) throw std::runtime_error("fixture path");
        DeleteFileW(unique);
        root = unique;
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw std::runtime_error("fixture directory");
        file = root / L"settings.json";
    }
    ~Fixture() {
        DeleteFileW(file.c_str());
        DeleteFileW((file.native() + L".lock").c_str());
        RemoveDirectoryW(root.c_str());
    }
};
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Policy() {
    Fixture f; settings::Store store(f.file);
    std::uint64_t value = 91;
    CHECK(media::ReadBackgroundPictureSourceLimit(store, value) == Result::Ok && value == media::kDefaultPictureSourceLimit);
    CHECK(!fs::exists(f.file));
    CHECK(store.WriteSection("unrelated", R"({"keep":"exact"})") == Result::Ok);
    CHECK(store.WriteSection("resources", R"({"future":{"keep":7}})") == Result::Ok);
    CHECK(media::ReadBackgroundPictureSourceLimit(store, value) == Result::Ok && value == media::kDefaultPictureSourceLimit);
    for (unsigned limit : {16u, 128u, 4096u}) {
        CHECK(store.WriteSection("resources", "{\"backgroundPictureSourcesMiB\":" + std::to_string(limit) + "}") == Result::Ok);
        const auto before = Bytes(f.file);
        CHECK(media::ReadBackgroundPictureSourceLimit(store, value) == Result::Ok && value == std::uint64_t(limit) * 1024 * 1024);
        CHECK(Bytes(f.file) == before);
    }
    for (const auto* invalid : {"0", "15", "4097", "4294967296", "-1", "16.5", "1e2", "\"128\"", "true", "null", "[]", "{}"}) {
        CHECK(store.WriteSection("resources", std::string("{\"backgroundPictureSourcesMiB\":") + invalid + "}") == Result::Ok);
        value = 93;
        const auto before = Bytes(f.file);
        CHECK(media::ReadBackgroundPictureSourceLimit(store, value) == Result::Invalid && value == 93);
        CHECK(Bytes(f.file) == before);
    }
    CHECK(store.WriteSection("resources", R"({"backgroundPictureSourcesMiB":32})") == Result::Ok);
    std::string resources, unrelated;
    CHECK(store.ReadSection("resources", resources) == Result::Ok && resources.find("future") != std::string::npos);
    CHECK(store.ReadSection("unrelated", unrelated) == Result::Ok && unrelated.find("exact") != std::string::npos);
    {
        std::ofstream broken(f.file, std::ios::binary | std::ios::trunc);
        broken << "{invalid";
    }
    value = 97;
    CHECK(media::ReadBackgroundPictureSourceLimit(store, value) == Result::Invalid && value == 97);
    CHECK(Bytes(f.file) == "{invalid");
    CHECK(DeleteFileW(f.file.c_str()));
    CHECK(CreateDirectoryW(f.file.c_str(), nullptr));
    value = 99;
    CHECK(media::ReadBackgroundPictureSourceLimit(store, value) != Result::Ok && value == 99);
    CHECK(RemoveDirectoryW(f.file.c_str()));
}
}
int main() {
    try { Policy(); }
    catch (const std::exception& error) { std::fprintf(stderr, "exception: %s\n", error.what()); CHECK(false); }
    std::printf("background_picture_policy_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
