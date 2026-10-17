#include "mikudancestudio/system_settings.hpp"
#include "mikudancestudio/local_data_path.hpp"
#include "../plugins/json.hpp"
#include <atomic>

namespace mikudancestudio::system_settings {
namespace {
using plugins::Json;
constexpr DWORD MaximumBytes = 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Lock {
    Handle file;
    OVERLAPPED overlap{};
    bool held = false;
    ~Lock() { if (held) UnlockFileEx(file.value, 0, 1, 0, &overlap); }
    Result Acquire(const std::filesystem::path& path) {
        file.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? Result::Busy : Result::Io;
        }
        const auto start = GetTickCount64();
        do {
            if (LockFileEx(file.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0, 1, 0, &overlap)) { held = true; return Result::Ok; }
            const auto error = GetLastError();
            if (error != ERROR_LOCK_VIOLATION && error != ERROR_IO_PENDING) return Result::Io;
            if (GetTickCount64() - start >= 2000) return Result::Busy;
            Sleep(10);
        } while (true);
    }
};
void Quote(std::string& output, const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    output += '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { output += '\\'; output += char(c); }
        else if (c < 32) { output += "\\u00"; output += hex[c >> 4]; output += hex[c & 15]; }
        else output += char(c);
    }
    output += '"';
}
void Serialize(std::string& output, const Json& value) {
    switch (value.kind) {
    case Json::Null: output += "null"; break;
    case Json::Boolean: case Json::Number: output += value.text; break;
    case Json::StringValue: Quote(output, value.text); break;
    case Json::Array: {
        output += '['; bool first = true;
        for (const auto& item : value.array) { if (!first) output += ','; first = false; Serialize(output, item); }
        output += ']'; break;
    }
    case Json::Object: {
        output += '{'; bool first = true;
        for (const auto& item : value.object) {
            if (!first) output += ','; first = false;
            Quote(output, item.first); output += ':'; Serialize(output, item.second);
        }
        output += '}'; break;
    }
    }
}
std::string Serialize(const Json& value) { std::string output; Serialize(output, value); return output; }
bool ValidName(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-';
    });
}
void Merge(Json& destination, const Json& update) {
    for (const auto& entry : update.object) {
        auto found = destination.object.find(entry.first);
        if (found != destination.object.end() && found->second.kind == Json::Object && entry.second.kind == Json::Object)
            Merge(found->second, entry.second);
        else destination.object[entry.first] = entry.second;
    }
}
Result Load(const std::filesystem::path& path, Json& root) {
    Handle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Result::Missing : Result::Io;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) return Result::Io;
    if (size.QuadPart < 0 || size.QuadPart > MaximumBytes) return Result::Invalid;
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) return Result::Io;
    try {
        root = Json::Parse(bytes, MaximumBytes);
        if (root.kind != Json::Object || root.At("schemaVersion").kind != Json::Number ||
            root.At("schemaVersion").text != "1" || root.At("sections").kind != Json::Object) return Result::Invalid;
        for (const auto& section : root.At("sections").object)
            if (section.second.kind != Json::Object) return Result::Invalid;
    } catch (const std::invalid_argument&) { return Result::Invalid; }
    return Result::Ok;
}
Result Save(const std::filesystem::path& path, const std::string& bytes, bool exists) {
    static std::atomic<unsigned long> serial{0};
    std::filesystem::path temporary;
    Handle file;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = path;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
        file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return Result::Io;
    }
    if (file.value == INVALID_HANDLE_VALUE) return Result::Io;
    DWORD written = 0;
    const bool ready = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file.value);
    CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
    bool saved = false;
    if (ready) saved = exists ? ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != FALSE
        : MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!saved) DeleteFileW(temporary.c_str());
    return saved ? Result::Ok : Result::Io;
}
}
Result Store::ReadSection(std::string_view name, std::string& output) const noexcept {
    try {
        if (!ValidName(name) || path_.empty() || !path_.is_absolute()) return Result::Invalid;
        std::error_code error;
        if (!std::filesystem::exists(path_.parent_path(), error)) return error ? Result::Io : Result::Missing;
        Lock lock;
        const auto acquired = lock.Acquire(path_.wstring() + L".lock");
        if (acquired != Result::Ok) return acquired;
        Json root;
        const auto loaded = Load(path_, root);
        if (loaded != Result::Ok) return loaded;
        const auto& sections = root.At("sections").object;
        const auto found = sections.find(std::string(name));
        if (found == sections.end()) return Result::Missing;
        auto serialized = Serialize(found->second);
        output.swap(serialized);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Store::WriteSection(std::string_view name, std::string_view object) const noexcept {
    return Write(name, object, false, nullptr);
}
Result Store::CompareExchangeSection(std::string_view name, const std::string* expected,
    std::string_view object) const noexcept {
    return Write(name, object, true, expected);
}
Result Store::Write(std::string_view name, std::string_view object, bool compare,
    const std::string* expected) const noexcept {
    try {
        if (!ValidName(name) || path_.empty() || !path_.is_absolute()) return Result::Invalid;
        Json update;
        try { update = Json::Parse(object, MaximumBytes); }
        catch (const std::invalid_argument&) { return Result::Invalid; }
        if (update.kind != Json::Object) return Result::Invalid;
        std::string expectedCanonical;
        if (compare && expected) {
            try {
                const auto parsed = Json::Parse(*expected, MaximumBytes);
                if (parsed.kind != Json::Object) return Result::Invalid;
                expectedCanonical = Serialize(parsed);
            } catch (const std::invalid_argument&) { return Result::Invalid; }
        }
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) return Result::Io;
        Lock lock;
        const auto acquired = lock.Acquire(path_.wstring() + L".lock");
        if (acquired != Result::Ok) return acquired;
        Json root;
        const auto loaded = Load(path_, root);
        if (loaded != Result::Ok && loaded != Result::Missing) return loaded;
        std::string before;
        if (loaded == Result::Missing) root = Json::Parse("{\"schemaVersion\":1,\"sections\":{}}");
        else before = Serialize(root);
        auto& sections = root.object.at("sections").object;
        const auto current = sections.find(std::string(name));
        if (compare && (expected ? current == sections.end() || Serialize(current->second) != expectedCanonical
            : current != sections.end())) return Result::Conflict;
        auto& section = sections[std::string(name)];
        if (section.kind != Json::Object) section.kind = Json::Object;
        if (compare) section = std::move(update);
        else Merge(section, update);
        auto bytes = Serialize(root);
        if (bytes.size() > MaximumBytes) return Result::Invalid;
        // Validate combined depth/node limits as well as byte size before save.
        try { Json::Parse(bytes, MaximumBytes); }
        catch (const std::invalid_argument&) { return Result::Invalid; }
        if (bytes == before) return Result::Ok;
        bytes += '\n';
        if (bytes.size() > MaximumBytes) return Result::Invalid;
        return Save(path_, bytes, loaded == Result::Ok);
    } catch (...) { return Result::Io; }
}
std::filesystem::path Path() { return local_data::File(L"settings.json"); }
Result ReadSection(std::string_view name, std::string& output) noexcept {
    try { return Store(Path()).ReadSection(name, output); } catch (...) { return Result::Io; }
}
Result WriteSection(std::string_view name, std::string_view object) noexcept {
    try { return Store(Path()).WriteSection(name, object); } catch (...) { return Result::Io; }
}
Result CompareExchangeSection(std::string_view name, const std::string* expected,
    std::string_view object) noexcept {
    try { return Store(Path()).CompareExchangeSection(name, expected, object); }
    catch (...) { return Result::Io; }
}
}
