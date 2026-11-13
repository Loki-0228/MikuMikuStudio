#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/legacy_mme.hpp"
#include "mikumikustudio/legacy_mme_bootstrap.hpp"
#include <d3d9.h>

#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "effect_compile_cache.hpp"
#include "../io/project_relocation.hpp"
#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include <filesystem>
#include <mutex>
#include <vector>
#include <algorithm>
#include <set>
#include <limits>

namespace mikumikustudio {
namespace {
namespace fs = std::filesystem;
struct RelativeAssignment {
    fs::path legacy; // MME expands relative entries against its installation.
    fs::path local;
};
std::mutex assignmentMutex;
std::vector<RelativeAssignment> assignments;
std::set<fs::path> effectReferences;
fs::path assignmentSource;
struct ProjectRelocation {
    fs::path source;
    std::weak_ptr<const project_relocation::RootMappings> roots;
    DWORD thread = 0;
} projectRelocation;
// MMHack consumes notifications on a later frame. Never recycle a published
// alias for another project while that notification may still be queued.
// Refuse new aliases at the bound instead of retaining unbounded session data.
constexpr std::size_t kMaxProjectAliases = 4096;
std::vector<RelativeAssignment> projectAliases;
std::uint64_t projectAliasSequence = 0;

struct PreserveLastError {
    DWORD value = GetLastError();
    ~PreserveLastError() { SetLastError(value); }
};
bool AvailableAlias(const fs::path& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND;
}
enum class AliasResult { None, Mapped, Conflict };
AliasResult ProjectAlias(const wchar_t* filename, std::wstring& result) {
    if (!filename || !*filename) return AliasResult::None;
    const wchar_t* leaf = filename;
    for (const wchar_t* p = filename; *p; ++p) if (*p == L'\\' || *p == L'/') leaf = p + 1;
    if (_wcsnicmp(leaf, L"~mms-", 5) != 0) return AliasResult::None;
    const auto path = fs::absolute(filename).lexically_normal();
    std::lock_guard<std::mutex> lock(assignmentMutex);
    for (const auto& entry : projectAliases) {
        if (_wcsicmp(path.c_str(), entry.legacy.c_str()) == 0) {
            auto notification = entry.legacy;
            notification.replace_extension(L".pmm");
            // A file created under a formerly virtual name must not make a
            // delayed notification overwrite either that file or its effects.
            if (!AvailableAlias(entry.legacy) || !AvailableAlias(notification)) return AliasResult::Conflict;
            result = entry.local.wstring();
            return AliasResult::Mapped;
        }
    }
    return AliasResult::None;
}
AliasResult ProjectAliasBytes(const char* filename, std::wstring& result, UINT* codePage = nullptr) {
    if (!filename || !*filename) return AliasResult::None;
    // Published aliases have an ASCII basename. This is only a negative
    // filter; decode the full path before interpreting any path separator.
    bool candidate = false;
    for (const char* p = filename; *p; ++p)
        if (*p == '~' && _strnicmp(p, "~mms-", 5) == 0) { candidate = true; break; }
    if (!candidate) return AliasResult::None;
    {
        std::lock_guard<std::mutex> lock(assignmentMutex);
        if (projectAliases.empty()) return AliasResult::None;
    }
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        std::wstring decoded;
        if (!text_encoding::Decode(filename, cp, decoded)) continue;
        const auto found = ProjectAlias(decoded.c_str(), result);
        if (found != AliasResult::None) {
            if (codePage) *codePage = cp;
            return found;
        }
    }
    return AliasResult::None;
}

bool RegularFile(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
bool MissingFile(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Caller holds assignmentMutex.
void ClearAssignments() {
    assignments.clear();
    effectReferences.clear();
    assignmentSource.clear();
}
fs::path ExecutableDirectory() {
    return local_data::ExecutableDirectory();
}
std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    text = text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    return text;
}

bool ReadAssignmentBytes(HANDLE file, bool preservePosition, std::string& bytes,
                         DWORD& restoreFailure) {
    LARGE_INTEGER size{}, saved{}, zero{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024)
        return false;
    // Allocate before changing a handle that has not yet been returned to MME.
    std::string next(static_cast<std::size_t>(size.QuadPart), '\0');
    if (preservePosition && !SetFilePointerEx(file, zero, &saved, FILE_CURRENT)) return false;
    struct RestorePosition {
        HANDLE file;
        LARGE_INTEGER offset;
        DWORD& failure;
        bool pending;
        bool Apply() noexcept {
            if (!pending) return true;
            pending = false;
            if (SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) return true;
            failure = GetLastError();
            if (!failure) failure = ERROR_SEEK;
            return false;
        }
        ~RestorePosition() { Apply(); }
    } restore{file, saved, restoreFailure, preservePosition};
    if (preservePosition && !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    const bool complete = ReadFile(file, next.data(), static_cast<DWORD>(next.size()), &read, nullptr) &&
        read == next.size();
    if (!restore.Apply() || !complete) return false;
    bytes.swap(next);
    return true;
}

struct AssignmentTable {
    std::vector<RelativeAssignment> paths;
    std::set<fs::path> references;
    std::set<std::vector<fs::path>> referenceSpellings;
};

AssignmentTable ReadAssignmentTable(const fs::path& emm, HANDLE opened, DWORD* positionFailure,
                                    bool collectSpellings) {
    AssignmentTable table;
    std::set<std::pair<fs::path, fs::path>> seen;
    const auto executable = ExecutableDirectory();
    // A packaged legacy MME lives in this installation's cache, so its
    // assignments also expand against the module's own directory. A failed or
    // truncated module path keeps only the EXE mapping.
    fs::path moduleDirectory;
    if (plugins::LegacyMmeStatus().loaded) {
        if (const HMODULE module = LegacyMmeModule()) {
            std::vector<wchar_t> filename(32768);
            const DWORD length = GetModuleFileNameW(module, filename.data(),
                static_cast<DWORD>(filename.size()));
            if (length && length < filename.size())
                moduleDirectory = fs::path(filename.data()).parent_path();
        }
    }
    const bool owned = opened == INVALID_HANDLE_VALUE;
    HANDLE file = owned ? CreateFileW(emm.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : opened;
    if (file != INVALID_HANDLE_VALUE) {
        struct Close { HANDLE file; bool owned; ~Close() { if (owned) CloseHandle(file); } } close{file, owned};
        // Bound the compatibility parser; MME retains ownership of its format.
        std::string bytes;
        DWORD restoreFailure = 0;
        bool loaded = ReadAssignmentBytes(file, !owned, bytes,
            positionFailure ? *positionFailure : restoreFailure);
        if (!loaded && !owned) {
            // MME can open an EMM with no read right. Parse a private read of
            // the same path rather than clearing all of its relative aliases.
            const HANDLE fallback = CreateFileW(emm.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fallback != INVALID_HANDLE_VALUE) {
                Close fallbackClose{fallback, true};
                loaded = ReadAssignmentBytes(fallback, false, bytes, restoreFailure);
            }
        }
        if (loaded) {
                bool effectSection = false;
                for (std::size_t offset = 0; offset < bytes.size();) {
                    const auto end = bytes.find('\n', offset);
                    auto line = Trim(bytes.substr(offset, end == std::string::npos ? end : end - offset));
                    offset = end == std::string::npos ? bytes.size() : end + 1;
                    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
                    if (line.front() == '[') {
                        effectSection = _stricmp(line.c_str(), "[Effect]") == 0 ||
                            (_strnicmp(line.c_str(), "[Effect@", 8) == 0 && line.back() == ']');
                        continue;
                    }
                    const auto equal = line.find('=');
                    if (equal == std::string::npos) continue;
                    const auto value = Trim(line.substr(equal + 1));
                    if (value.empty() || value.find('\0') != std::string::npos) continue;
                    // Decode before looking for separators: CP932 can contain
                    // 0x5c inside a character, not just as a directory separator.
                    std::vector<fs::path> spellings;
                    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
                        std::wstring decoded;
                        if (!text_encoding::Decode(value.c_str(), cp, decoded)) continue;
                        const fs::path relative(decoded);
                        if (relative.empty() || !relative.has_extension()) continue;
                        if (effectSection && _wcsicmp(relative.extension().c_str(), L".fx") == 0 &&
                            (relative.is_absolute() || !relative.has_root_path())) {
                            const auto anchored = relative.is_absolute() ? relative : emm.parent_path() / relative;
                            const auto normalized = project_relocation::NormalizeFile(anchored.lexically_normal().native());
                            if (!normalized.empty()) {
                                table.references.emplace(normalized);
                                if (collectSpellings) spellings.emplace_back(normalized);
                            }
                        }
                        if (relative.has_root_path()) continue;
                        const auto legacy = (executable / relative).lexically_normal();
                        const auto local = (emm.parent_path() / relative).lexically_normal();
                        if (seen.emplace(legacy, local).second) table.paths.push_back({legacy, local});
                        if (!moduleDirectory.empty()) {
                            const auto moduleLegacy = (moduleDirectory / relative).lexically_normal();
                            if (seen.emplace(moduleLegacy, local).second)
                                table.paths.push_back({moduleLegacy, local});
                        }
                    }
                    if (!spellings.empty()) {
                        std::sort(spellings.begin(), spellings.end());
                        spellings.erase(std::unique(spellings.begin(), spellings.end()), spellings.end());
                        table.referenceSpellings.insert(std::move(spellings));
                    }
                }
        }
    }
    return table;
}

void ReadAssignments(const fs::path& emm, HANDLE opened = INVALID_HANDLE_VALUE,
                     DWORD* positionFailure = nullptr) {
    auto source = emm.lexically_normal();
    auto table = ReadAssignmentTable(emm, opened, positionFailure, false);
    std::lock_guard<std::mutex> lock(assignmentMutex);
    assignments.swap(table.paths); // A project without EMM must not retain old aliases.
    effectReferences.swap(table.references);
    assignmentSource.swap(source);
}

bool RelativePath(const wchar_t* filename, std::wstring& resolved) {
    if (!filename || !*filename) return false;
    auto path = fs::path(filename).lexically_normal();
    if (!path.has_root_path()) path = (ExecutableDirectory() / path).lexically_normal();
    std::lock_guard<std::mutex> lock(assignmentMutex);
    // All project-relative candidates precede MME's expanded absolute path.
    for (const auto& entry : assignments) {
        if (_wcsicmp(path.c_str(), entry.legacy.c_str()) == 0 && RegularFile(entry.local)) {
            resolved = entry.local.native();
            return true;
        }
    }
    return false;
}

// Caller holds assignmentMutex.
bool RelocationCurrent() {
    return projectRelocation.thread == GetCurrentThreadId() && assignmentSource == projectRelocation.source &&
        !projectRelocation.roots.expired() && !effectReferences.empty();
}

bool RelocatedEffectPath(const wchar_t* filename, std::wstring& resolved) {
    const auto* extension = filename ? std::wcsrchr(filename, L'.') : nullptr;
    if (!extension || _wcsicmp(extension, L".fx") != 0) return false;
    std::lock_guard<std::mutex> lock(assignmentMutex);
    // RootMappings belongs to the UI thread. The weak alias expires on reset,
    // cancellation or scene replacement, including delayed legacy callbacks.
    if (!RelocationCurrent()) return false;
    const auto roots = projectRelocation.roots.lock();
    auto path = fs::path(filename).lexically_normal();
    if (!path.has_root_path()) path = (ExecutableDirectory() / path).lexically_normal();
    if (!roots || !MissingFile(path)) return false;

    const auto candidateFor = [&](const fs::path& old) {
        const auto normalized = project_relocation::NormalizeFile(old.native());
        if (effectReferences.find(normalized) == effectReferences.end() || !MissingFile(old)) return std::wstring{};
        auto result = roots->Infer(normalized);
        if (result.status != project_relocation::Status::Candidate || !RegularFile(result.path)) return std::wstring{};
        return std::move(result.path);
    };
    auto candidate = candidateFor(path);
    for (const auto& entry : assignments) {
        if (entry.legacy != path || entry.local == path) continue;
        auto mapped = candidateFor(entry.local);
        if (mapped.empty()) continue;
        if (!candidate.empty() && candidate != mapped) return false;
        candidate = std::move(mapped);
    }
    if (candidate.empty()) return false;
    resolved = std::move(candidate);
    return true;
}

// GetFullPathNameA can only return a spelling that the active code page
// represents. A real source outside that code page is written as UTF-8 bytes
// instead, following the native byte-capacity contract: a short buffer reports
// the required size including the terminator without writing anything, and a
// successful call returns the byte length without the terminator and points
// lpFilePart at the last path component.
bool WriteUtf8FullPath(const std::wstring& source, LPSTR buffer, DWORD length,
                       LPSTR* filePart, DWORD incomingError, DWORD& written) {
    std::string encoded;
    if (!text_encoding::EncodeExact(source.c_str(), CP_UTF8, encoded)) return false;
    // A longer path cannot be described by the native DWORD contract.
    if (encoded.size() >= (std::numeric_limits<DWORD>::max)()) return false;
    const DWORD required = static_cast<DWORD>(encoded.size() + 1);
    SetLastError(incomingError);
    if (!buffer || length < required) {
        written = required;
        return true;
    }
    std::memcpy(buffer, encoded.c_str(), required);
    if (filePart) {
        std::size_t offset = 0;
        for (std::size_t index = encoded.size(); index > 0; --index) {
            const char character = encoded[index - 1];
            if (character == '\\' || character == '/') {
                offset = index;
                break;
            }
        }
        *filePart = buffer + offset;
    }
    written = static_cast<DWORD>(encoded.size());
    return true;
}

// Normalize a decoded spelling against the process's real current directory.
// Nothing here guesses the directory the EXE lives in.
bool NormalizeAgainstCwd(const std::wstring& decoded, std::wstring& normalized) {
    normalized.clear();
    if (decoded.empty()) return false;
    std::vector<wchar_t> buffer(32768);
    const DWORD length = ::GetFullPathNameW(decoded.c_str(),
        static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= static_cast<DWORD>(buffer.size())) return false;
    normalized.assign(buffer.data(), length);
    return true;
}

// Accept a spelling only when the exact bytes decode strictly and normalize to
// an existing regular file.
bool ExistingRegularPath(const char* bytes, UINT codePage, std::wstring& path) {
    std::wstring decoded;
    if (!text_encoding::Decode(bytes, codePage, decoded)) return false;
    std::wstring normalized;
    if (!NormalizeAgainstCwd(decoded, normalized) ||
        !RegularFile(fs::path(normalized))) return false;
    path = std::move(normalized);
    return true;
}

bool RelocatedEffectBytes(const char* bytes, std::wstring& result, UINT* codePage = nullptr) {
    const auto* extension = bytes ? std::strrchr(bytes, '.') : nullptr;
    if (!extension || _stricmp(extension, ".fx") != 0) return false;
    {
        std::lock_guard<std::mutex> lock(assignmentMutex);
        if (!RelocationCurrent()) return false;
    }
    std::wstring candidate;
    UINT matchedCodePage = 0;
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        std::wstring decoded, path;
        if (!text_encoding::Decode(bytes, cp, decoded) || !RelocatedEffectPath(decoded.c_str(), path)) continue;
        if (!candidate.empty() && candidate != path) return false;
        if (!matchedCodePage) matchedCodePage = cp;
        candidate = std::move(path);
    }
    if (candidate.empty()) return false;
    std::wstring native, absolute;
    if (text_encoding::Decode(bytes, GetACP(), native) && NormalizeAgainstCwd(native, absolute) &&
        !MissingFile(absolute)) return false;
    result = std::move(candidate);
    if (codePage) *codePage = matchedCodePage;
    return true;
}

// MMEffect expands relative EMM effect entries against its own module
// directory and normalizes them with GetFullPathNameA. The adapter already
// maps the exact legacy spelling to the project file for reads, but that
// mapping is transient: MME keeps the module-relative expansion, so a later
// Save As records a spelling the next load cannot resolve. Replace the
// normalized result with the mapped project file's real absolute path so MME
// holds the real source identity through load and every following save.
// A source the active code page cannot represent is persisted as UTF-8, and a
// restarted process without that assignment accepts the saved real UTF-8 file.
// Only an exact assignment mapping whose local file is a regular file, or an
// existing regular file named by a strict UTF-8 decode, may change the result.
// Unmapped names, absolute non-aliases, missing or directory targets, invalid
// parameters and allocation failures keep the native behavior, including
// buffer sizing, lpFilePart, output and LastError.
DWORD WINAPI EffectFullPathNameA(LPCSTR filename, DWORD length, LPSTR buffer,
                                 LPSTR* filePart) {
    const DWORD incomingError = GetLastError();
    try {
        // Interpret the input only for a valid size query or a usable buffer.
        if (filename && *filename && (buffer || length == 0)) {
            std::wstring source;
            bool mapped = false;
            // Decode the original bytes before normalizing: GetFullPathNameA
            // first would already have replaced bytes the active code page
            // cannot represent.
            for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
                std::wstring decoded;
                if (!text_encoding::Decode(filename, cp, decoded)) continue;
                std::wstring normalized;
                if (!NormalizeAgainstCwd(decoded, normalized)) continue;
                if (RelativePath(normalized.c_str(), source)) {
                    mapped = true;
                    break;
                }
            }
            if (!mapped) mapped = RelocatedEffectBytes(filename, source);
            bool utf8Source = false;
            if (!mapped) {
                // A restarted process has no assignment for a source that was
                // persisted as UTF-8. The native ACP meaning wins when those
                // same bytes already name an existing file, so ambiguous
                // input is never claimed.
                std::wstring acpPath;
                std::wstring utf8Path;
                if (!ExistingRegularPath(filename, GetACP(), acpPath) &&
                    ExistingRegularPath(filename, CP_UTF8, utf8Path)) {
                    source.swap(utf8Path);
                    utf8Source = true;
                }
            }
            if (mapped || utf8Source) {
                // A mapped local path was only lexically normalized. Apply the
                // same trailing dot/space and separator rules the native call
                // would, and keep the native path when that check fails.
                std::wstring normalizedSource;
                if (NormalizeAgainstCwd(source, normalizedSource)) {
                    // MME calls ANSI Win32 APIs, so a lossless ACP spelling
                    // keeps the native call; anything else is returned as
                    // UTF-8 bytes.
                    std::string encoded;
                    if (text_encoding::EncodeExact(normalizedSource.c_str(), GetACP(), encoded)) {
                        SetLastError(incomingError);
                        return ::GetFullPathNameA(encoded.c_str(), length, buffer, filePart);
                    }
                    DWORD written = 0;
                    if (WriteUtf8FullPath(normalizedSource, buffer, length, filePart,
                                          incomingError, written))
                        return written;
                }
            }
        }
    } catch (...) { /* Compatibility preparation must not escape a native API. */ }
    SetLastError(incomingError);
    return ::GetFullPathNameA(filename, length, buffer, filePart);
}

HANDLE TrackAssignmentRead(HANDLE file, bool readOnly, DWORD attributes) noexcept {
    if (file == INVALID_HANDLE_VALUE || !readOnly) return file;
    const DWORD error = GetLastError();
    DWORD positionFailure = 0;
    try {
        // Also cover EMM files opened directly through MME's own Load menu.
        // The handle names the exact file chosen by Windows, independent of ACP.
        wchar_t filename[32768]{};
        const DWORD size = GetFinalPathNameByHandleW(file, filename, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size && size < 32768) {
            std::wstring path(filename, size);
            if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) path = L"\\\\" + path.substr(8);
            else if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
            if (pmm_paths::IsEffect(path)) {
                if (attributes & FILE_FLAG_OVERLAPPED) {
                    // Never issue synchronous reads on an asynchronous handle.
                    std::lock_guard<std::mutex> lock(assignmentMutex);
                    ClearAssignments();
                } else ReadAssignments(path, file, &positionFailure);
            }
        }
    } catch (...) {
        // An unreadable new assignment must not reuse the previous project.
        std::lock_guard<std::mutex> lock(assignmentMutex);
        ClearAssignments();
    }
    if (positionFailure) {
        // This newly opened handle is still private to the wrapper. Do not
        // return it as a successful open after failing to restore its cursor.
        CloseHandle(file);
        SetLastError(positionFailure);
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(error);
    return file;
}

// MME keeps EMM paths as bytes and calls ANSI Win32 APIs. A file saved on
// Japanese Windows can therefore point at an existing, different Unicode
// spelling on Chinese Windows. Prefer the current ACP and only fall back to
// another encoding when that exact decoded file really exists.
bool ExistingPath(const char* bytes, std::wstring& path, UINT& codePage,
                  bool allowProjectAliases = true) {
    if (!bytes || !*bytes) return false;
    if (allowProjectAliases && ProjectAliasBytes(bytes, path, &codePage) == AliasResult::Mapped) return true;
    if (allowProjectAliases) for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        std::wstring decoded;
        if (text_encoding::Decode(bytes, cp, decoded) && RelativePath(decoded.c_str(), path)) {
            codePage = cp;
            return true;
        }
    }
    if (allowProjectAliases && RelocatedEffectBytes(bytes, path, &codePage)) return true;
    bool nonAscii = false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes); *p; ++p)
        nonAscii |= *p >= 0x80;
    if (!nonAscii || (!std::strchr(bytes, '\\') && !std::strchr(bytes, '/')))
        return false;
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        if (!text_encoding::Decode(bytes, cp, path)) continue;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            codePage = cp;
            return true;
        }
    }
    return false;
}

bool MissingPath(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_INVALID_NAME || error == ERROR_NO_UNICODE_TRANSLATION;
}

bool SettingsTarget(LPCWSTR filename, std::wstring& target) {
    const auto& legacy = plugins::LegacyMmeStatus();
    if (!filename || !legacy.loaded || legacy.settingsFile.empty()) return false;
    const auto path = fs::absolute(filename).lexically_normal();
    bool matches = _wcsicmp(path.c_str(), (legacy.directory / L"MMEffect.ini").c_str()) == 0;
    // Match the exact ANSI spelling the original module-directory API supplies,
    // even on systems where its directory contains non-ACP characters.
    if (!matches) {
        std::vector<char> name(32768);
        const auto module = LegacyMmeModule();
        const DWORD n = module ? GetModuleFileNameA(module, name.data(), DWORD(name.size())) : 0;
        std::wstring decoded;
        if (n && n < name.size() && text_encoding::Decode(name.data(), GetACP(), decoded)) {
            auto expected = fs::path(decoded).parent_path() / L"MMEffect.ini";
            matches = _wcsicmp(path.c_str(), expected.lexically_normal().c_str()) == 0;
        }
    }
    if (matches) target = legacy.settingsFile.wstring();
    return matches;
}
bool SettingsTarget(LPCSTR filename, std::wstring& target) {
    if (!filename || !plugins::LegacyMmeStatus().loaded) return false;
    std::wstring decoded;
    return text_encoding::Decode(filename, GetACP(), decoded) && SettingsTarget(decoded.c_str(), target);
}
HANDLE OpenSettingsFile(const std::wstring& path, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD attributes, HANDLE templateFile) {
    // Validate the opened object before truncating an existing preference file.
    const DWORD actualCreation = creation == CREATE_ALWAYS ? OPEN_ALWAYS :
        creation == TRUNCATE_EXISTING ? OPEN_EXISTING : creation;
    HANDLE file = CreateFileW(path.c_str(), access | FILE_READ_ATTRIBUTES, share, security,
        actualCreation, attributes | FILE_FLAG_OPEN_REPARSE_POINT, templateFile);
    if (file == INVALID_HANDLE_VALUE) return file;
    const DWORD error = GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        CloseHandle(file); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    if ((creation == CREATE_ALWAYS || creation == TRUNCATE_EXISTING) && !SetEndOfFile(file)) {
        const auto failure = GetLastError(); CloseHandle(file); SetLastError(failure); return INVALID_HANDLE_VALUE;
    }
    SetLastError(error); return file;
}
std::atomic<decltype(&CreateFileW)> previousEffectFileW{&CreateFileW};
HANDLE WINAPI OpenEffectFileW(LPCWSTR filename, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD attributes, HANDLE templateFile) {
    const DWORD error = GetLastError();
    try {
        std::wstring target;
        if (SettingsTarget(filename, target)) {
            SetLastError(error); return OpenSettingsFile(target, access, share, security, creation, attributes, templateFile);
        }
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    SetLastError(error); return previousEffectFileW.load(std::memory_order_acquire)(filename, access, share, security, creation, attributes, templateFile);
}

DWORD WINAPI EffectFileAttributes(LPCSTR filename) {
    const DWORD incomingError = GetLastError();
    try {
        std::wstring preferred;
        if (SettingsTarget(filename, preferred)) {
            SetLastError(incomingError);
            return GetFileAttributesW(preferred.c_str());
        }
        const auto alias = ProjectAliasBytes(filename, preferred);
        if (alias == AliasResult::Conflict) { SetLastError(ERROR_FILE_EXISTS); return INVALID_FILE_ATTRIBUTES; }
        if (alias == AliasResult::Mapped) {
            SetLastError(incomingError);
            return GetFileAttributesW(preferred.c_str());
        }
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_FILE_ATTRIBUTES;
    }
    try {
        std::wstring preferred;
        UINT preferredCp = 0;
        if (ExistingPath(filename, preferred, preferredCp)) {
            SetLastError(incomingError);
            return GetFileAttributesW(preferred.c_str());
        }
    } catch (...) { /* Compatibility preparation must not escape a native API. */ }
    SetLastError(incomingError);
    const DWORD attributes = GetFileAttributesA(filename);
    if (attributes != INVALID_FILE_ATTRIBUTES) return attributes;
    const DWORD error = GetLastError();
    try {
        std::wstring path;
        UINT cp = 0;
        if (MissingPath(error) && ExistingPath(filename, path, cp)) {
            SetLastError(error);
            return GetFileAttributesW(path.c_str());
        }
    } catch (...) { /* Retain the original failure and its error code. */ }
    SetLastError(error);
    return attributes;
}

HANDLE WINAPI OpenEffectFile(LPCSTR filename, DWORD access, DWORD share,
                            LPSECURITY_ATTRIBUTES security, DWORD creation,
                            DWORD attributes, HANDLE templateFile) {
    const bool readOnly = creation == OPEN_EXISTING &&
        (access == GENERIC_READ || access == 0) && !(attributes & FILE_FLAG_DELETE_ON_CLOSE);
    const DWORD incomingError = GetLastError();
    try {
        std::wstring preferred;
        if (SettingsTarget(filename, preferred)) {
            SetLastError(incomingError);
            return OpenSettingsFile(preferred, access, share, security, creation, attributes, templateFile);
        }
        const auto alias = ProjectAliasBytes(filename, preferred);
        if (alias == AliasResult::Conflict) { SetLastError(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE; }
        if (alias == AliasResult::Mapped) {
            // Writes and creates redirect only through this exact registration,
            // never through filename/encoding/relative-assignment guesses.
            SetLastError(incomingError);
            return TrackAssignmentRead(CreateFileW(preferred.c_str(), access, share, security,
                creation, attributes, templateFile), readOnly, attributes);
        }
    } catch (...) {
        // Do not fall through to a physical file under a virtual spelling if
        // allocating the exact mapping failed, for either reads or writes.
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    try {
        std::wstring preferred;
        UINT preferredCp = 0;
        if (readOnly && ExistingPath(filename, preferred, preferredCp)) {
            SetLastError(incomingError);
            HANDLE file = CreateFileW(preferred.c_str(), access, share, security, creation, attributes, templateFile);
            // A selected existing project file is authoritative. A sharing or
            // access failure must not compile another same-named EXE file.
            return TrackAssignmentRead(file, readOnly, attributes);
        }
    } catch (...) { /* Fall back to the native open if preparation fails. */ }
    SetLastError(incomingError);
    HANDLE file = CreateFileA(filename, access, share, security, creation,
                              attributes, templateFile);
    if (file != INVALID_HANDLE_VALUE) return TrackAssignmentRead(file, readOnly, attributes);
    const DWORD error = GetLastError();
    // A compatibility lookup must never redirect writes or create a new file.
    try {
        std::wstring path;
        UINT cp = 0;
        if (readOnly && MissingPath(error) && ExistingPath(filename, path, cp)) {
            SetLastError(error);
            return TrackAssignmentRead(CreateFileW(path.c_str(), access, share, security, creation,
                               attributes, templateFile), readOnly, attributes);
        }
    } catch (...) { /* Retain the native open failure. */ }
    SetLastError(error);
    return file;
}

int WINAPI DecodeEffectPath(UINT codePage, DWORD flags, LPCCH bytes, int byteCount,
                           LPWSTR output, int capacity) {
    const DWORD incomingError = GetLastError();
    try {
    if ((codePage == CP_ACP || codePage == CP_THREAD_ACP || codePage == GetACP()) &&
        bytes && (byteCount == -1 || byteCount > 0) &&
        (flags == 0 || flags == MB_ERR_INVALID_CHARS)) {
        std::string bounded = byteCount == -1 ? std::string(bytes)
                                              : std::string(bytes, byteCount);
        if (!bounded.empty() && bounded.back() == '\0') bounded.pop_back();
        std::wstring path;
        UINT resolvedCodePage = 0;
        if (bounded.find('\0') == std::string::npos &&
            ExistingPath(bounded.c_str(), path, resolvedCodePage) &&
            resolvedCodePage != GetACP()) {
            // An assignment may identify the byte encoding even when the
            // EXE-relative file does not exist. Convert the original bytes,
            // never the alias: subsequent W writes/deletes keep their target.
            SetLastError(incomingError);
            return MultiByteToWideChar(resolvedCodePage, flags, bytes, byteCount, output, capacity);
        }
    }
    } catch (...) { /* Preserve native conversion on compatibility failure. */ }
    SetLastError(incomingError);
    return MultiByteToWideChar(codePage, flags, bytes, byteCount, output, capacity);
}

// MMEffect derives its writable files, including MMEffect.ini, from the
// directory that GetModuleFileNameA(nullptr) reports. A legacy package keeps
// that module inside the adapter's cache, so report MMEffect's own module while
// the package is loaded. Every other request forwards unchanged.
DWORD WINAPI EffectModuleFileNameA(HMODULE module, LPSTR filename, DWORD size) {
    if (module != nullptr || !plugins::LegacyMmeStatus().loaded)
        return ::GetModuleFileNameA(module, filename, size);
    const DWORD incomingError = GetLastError();
    const HMODULE effect = LegacyMmeModule();
    SetLastError(incomingError);
    return ::GetModuleFileNameA(effect, filename, size);
}

using CreateEffectFile = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR, const void*, const void*,
    DWORD, void*, void**, void**);
std::atomic<CreateEffectFile> originalEffectFile{nullptr};
// Cumulative per-process counters for the acceptance log. They count every
// intercepted D3DXCreateEffectFromFileW call, cached or not, so the last
// MME_EFFECT_RESULT line proves the launch total (e.g. 45/45, 90/90 after a
// reopen) even when the short per-call cache lines are overwritten in the
// bounded log ring.
std::atomic<unsigned long long> effectFileCalls{0};
std::atomic<unsigned long long> effectFileSucceeded{0};

// Read COM dispatch addresses without calling the object. A non-null device or
// pool alone does not establish that its vtable is still valid at FX creation.
bool ReadDispatchPointer(const void* address, void*& value) noexcept {
    value = nullptr;
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, &value,
        sizeof(value), &read) && read == sizeof(value);
}
void TraceEffectDispatch(const char* label, const void* object, bool device) noexcept {
    if (!object) {
        runtime_log::Write("MME_EFFECT_DISPATCH %s object=null", label);
        return;
    }
    void* table = nullptr;
    if (!ReadDispatchPointer(object, table) || !table) {
        runtime_log::Write("MME_EFFECT_DISPATCH %s object=%p vtable=unreadable", label, object);
        return;
    }
    // IDirect3DDevice9 shader creation is used by the FX compiler; the effect
    // pool exposes IUnknown. Log targets only, retaining the normal call path.
    const unsigned indices[] = {0, 1, 2, 7, 23, 86, 91, 106};
    for (unsigned i = 0; i < (device ? _countof(indices) : 3); ++i) {
        const auto slot = reinterpret_cast<ULONG_PTR>(table) + indices[i] * sizeof(void*);
        void* target = nullptr;
        const bool readable = ReadDispatchPointer(reinterpret_cast<void*>(slot), target);
        MEMORY_BASIC_INFORMATION region{};
        const bool mapped = readable && target && VirtualQuery(target, &region, sizeof(region));
        runtime_log::Write("MME_EFFECT_DISPATCH %s vtable=%p slot=%u target=%p readable=%u base=%p protection=0x%lX",
            label, table, indices[i], target, unsigned(readable), mapped ? region.AllocationBase : nullptr,
            mapped ? region.Protect : 0);
    }
}
HRESULT WINAPI TraceEffectFile(IDirect3DDevice9* device, LPCWSTR filename, const void* macros,
        const void* include, DWORD flags, void* pool, void** effect, void** errors) {
    const DWORD incomingError = GetLastError();
    const auto create = originalEffectFile.load(std::memory_order_acquire);
    std::wstring resolved;
    const wchar_t* actual = filename;
    try { if (RelativePath(filename, resolved) || RelocatedEffectPath(filename, resolved)) actual = resolved.c_str(); }
    catch (...) { /* Keep the original compiler input on lookup failure. */ }
    runtime_log::WritePath("MME_EFFECT_FILE", actual);
    runtime_log::Write("MME_EFFECT_CREATE function=%p device=%p include=%p pool=%p macros=%p flags=0x%08lX",
        create, device, include, pool, macros, flags);
    TraceEffectDispatch("device", device, true);
    TraceEffectDispatch("pool", pool, false);
    SetLastError(incomingError);
    HRESULT result = E_NOINTERFACE;
    if (create) {
        // The bytecode cache only replaces the compile/create work for the
        // default include handler.  Every bypass or failure falls through to
        // the original entry point, which keeps the HRESULT, diagnostics and
        // output ownership of the native call.
        if (!effect_compile_cache::TryCreateEffect(
                reinterpret_cast<void*>(create), device, actual, macros, include,
                flags, pool, effect, errors, result))
            result = create(device, actual, macros, include, flags, pool, effect, errors);
    }
    const auto error = GetLastError();
    const unsigned long long calls =
        effectFileCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned long long succeeded = SUCCEEDED(result)
        ? effectFileSucceeded.fetch_add(1, std::memory_order_relaxed) + 1
        : effectFileSucceeded.load(std::memory_order_relaxed);
    void* returnedEffect = nullptr;
    ReadDispatchPointer(effect, returnedEffect);
    runtime_log::Write("MME_EFFECT_RESULT hr=0x%08lX effect=%p calls=%llu succeeded=%llu",
        static_cast<unsigned long>(result), returnedEffect, calls, succeeded);
    SetLastError(error);
    return result;
}

struct ImportReplacement { const char* name; void* original; void* replacement; };

// Restrict compatibility and effect diagnostics to MMEffect's own imports.
// Other plugins and the host keep their normal ANSI API behavior.
void PatchImports(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= size ||
        imports.Size > size - imports.VirtualAddress) return;
    const ImportReplacement replacements[] = {
        {"GetFileAttributesA", reinterpret_cast<void*>(&GetFileAttributesA),
                              reinterpret_cast<void*>(&EffectFileAttributes)},
        {"CreateFileW", reinterpret_cast<void*>(&CreateFileW), reinterpret_cast<void*>(&OpenEffectFileW)},
        {"CreateFileA", reinterpret_cast<void*>(&CreateFileA),
                        reinterpret_cast<void*>(&OpenEffectFile)},
        {"MultiByteToWideChar", reinterpret_cast<void*>(&MultiByteToWideChar),
                                reinterpret_cast<void*>(&DecodeEffectPath)},
        {"GetFullPathNameA", reinterpret_cast<void*>(&GetFullPathNameA),
                             reinterpret_cast<void*>(&EffectFullPathNameA)},
        {"GetModuleFileNameA", reinterpret_cast<void*>(&GetModuleFileNameA),
                               reinterpret_cast<void*>(&EffectModuleFileNameA)},
    };
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (std::size_t i = 0; i < imports.Size / sizeof(*descriptors); ++i) {
        const auto& descriptor = descriptors[i];
        if (!descriptor.Name) break;
        if (descriptor.Name >= size ||
            strnlen_s(reinterpret_cast<const char*>(base + descriptor.Name), size - descriptor.Name) == size - descriptor.Name ||
            !descriptor.OriginalFirstThunk || descriptor.OriginalFirstThunk >= size ||
            !descriptor.FirstThunk || descriptor.FirstThunk >= size) continue;
        const char* library = reinterpret_cast<const char*>(base + descriptor.Name);
        const bool kernel = _stricmp(library, "kernel32.dll") == 0;
        const auto compiler = _strnicmp(library, "d3dx9_", 6) == 0
            ? reinterpret_cast<CreateEffectFile>(GetProcAddress(GetModuleHandleA(library), "D3DXCreateEffectFromFileW"))
            : nullptr;
        if (!kernel && !compiler) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor.OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor.FirstThunk);
        const auto nameCount = (size - descriptor.OriginalFirstThunk) / sizeof(*names);
        const auto slotCount = (size - descriptor.FirstThunk) / sizeof(*slots);
        for (std::size_t j = 0; j < nameCount && j < slotCount && names[j].u1.AddressOfData; ++j) {
            if (IMAGE_SNAP_BY_ORDINAL(names[j].u1.Ordinal)) continue;
            const auto rva = names[j].u1.AddressOfData;
            if (rva >= size || size - rva <= sizeof(WORD)) continue;
            const char* name = reinterpret_cast<const char*>(base + rva + sizeof(WORD));
            if (strnlen_s(name, size - rva - sizeof(WORD)) == size - rva - sizeof(WORD)) continue;
            if (compiler && std::strcmp(name, "D3DXCreateEffectFromFileW") == 0 &&
                slots[j].u1.Function == reinterpret_cast<ULONG_PTR>(compiler)) {
                DWORD protection = 0;
                auto* slot = &slots[j].u1.Function;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
                    originalEffectFile.store(compiler, std::memory_order_release);
                    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                        reinterpret_cast<void*>(&TraceEffectFile), reinterpret_cast<void*>(compiler));
                    DWORD ignored = 0; VirtualProtect(slot, sizeof(void*), protection, &ignored);
                }
            }
            if (!kernel) continue;
            for (const auto& entry : replacements) {
                if (std::strcmp(name, entry.name) != 0) continue;
                auto* previous = reinterpret_cast<void*>(slots[j].u1.Function);
                if (previous != entry.original) {
                    if (std::strcmp(name, "CreateFileW") != 0 || previous == entry.replacement ||
                        !plugins::LegacyMmeStatus().loaded) continue;
                    HMODULE owner = nullptr;
                    const auto systemEntry = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW"));
                    if (previous != systemEntry &&
                        (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(previous), &owner) || !owner || owner != GetModuleHandleW(L"MMHack.dll"))) continue;
                }
                DWORD protection = 0;
                auto* slot = &slots[j].u1.Function;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) continue;
                if (std::strcmp(name, "CreateFileW") == 0)
                    previousEffectFileW.store(reinterpret_cast<decltype(&CreateFileW)>(previous), std::memory_order_release);
                InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                                                  entry.replacement, previous);
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), protection, &ignored);
            }
        }
    }
}
}  // namespace

void InstallEffectPathCompatibility() {
    const HMODULE effect = mikumikustudio::LegacyMmeModule();
    if (effect) PatchImports(effect);
}

void PrepareEffectProjectPaths(const wchar_t* project) {
    const PreserveLastError error;
    try {
        if (project && *project) {
            // Model-load notifications share the legacy hook with project opens.
            // They must not replace the current project's relative assignments.
            if (!pmm_paths::IsProject(project) && !pmm_paths::IsEffect(project)) return;
            ReadAssignments(pmm_paths::Effect(fs::absolute(project)));
        } else {
            std::lock_guard<std::mutex> lock(assignmentMutex);
            ClearAssignments();
            projectRelocation = {};
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(assignmentMutex);
        ClearAssignments();
    }
}

std::vector<std::wstring> MissingProjectEffects(const wchar_t* project) {
    const PreserveLastError error;
    if (!project || !*project || !LegacyMmeLoaded()) return {};
    const auto source = pmm_paths::Effect(fs::absolute(project)).lexically_normal();
    // Recovery reads its own sidecar. MMEffect can read another EMM while a
    // project is still loading, so its live alias table may be unrelated.
    const auto table = ReadAssignmentTable(source, INVALID_HANDLE_VALUE, nullptr, true);
    std::set<fs::path> availableAliases;
    for (const auto& entry : table.paths)
        if (table.references.count(entry.local) && !MissingFile(entry.legacy))
            availableAliases.insert(entry.local);
    std::set<std::wstring> missing;
    for (const auto& spellings : table.referenceSpellings) {
        if (std::all_of(spellings.begin(), spellings.end(), [&](const auto& path) {
                return !availableAliases.count(path) && MissingFile(path);
            }))
            for (const auto& path : spellings) missing.insert(path.native());
    }
    return {missing.begin(), missing.end()};
}

bool CompileLegacyMmeRecoveryEffect(IDirect3DDevice9* device, const wchar_t* path) {
    const auto compile = originalEffectFile.load(std::memory_order_acquire);
    if (!device || !compile || !LegacyMmeLoaded() || !RegularFile(path)) return false;
    DWORD mipFilter = 0;
    device->GetSamplerState(1, D3DSAMP_MIPFILTER, &mipFilter);
    struct Macro { const char* name; const char* value; };
    const Macro macros[] = {{"_INDEX", "PSIZE15"},
        {mipFilter ? "MME_MIPMAP" : nullptr, mipFilter ? "" : nullptr}, {nullptr, nullptr}};
    // Use an independent effect here; the plugin binds its own effect after
    // the PMM's model slots are complete.
    constexpr DWORD backwardsCompatibility = 0x1000;
    void* effect = nullptr;
    const HRESULT result = compile(device, path, macros, nullptr, backwardsCompatibility, nullptr, &effect, nullptr);
    if (effect) static_cast<IUnknown*>(effect)->Release();
    runtime_log::Write("MME recovery FX compile hr=0x%08lX", static_cast<unsigned long>(result));
    return SUCCEEDED(result) && effect;
}

void PublishEffectProjectRelocation(const wchar_t* project,
        std::weak_ptr<const project_relocation::RootMappings> roots) noexcept {
    const PreserveLastError error;
    try {
        if (!project || !*project || roots.expired()) return;
        auto source = pmm_paths::Effect(fs::absolute(project)).lexically_normal();
        std::lock_guard<std::mutex> lock(assignmentMutex);
        projectRelocation = {std::move(source), std::move(roots), GetCurrentThreadId()};
    } catch (...) { /* Optional path recovery cannot fail an accepted PMM. */ }
}

std::wstring EffectProjectNotificationPath(const wchar_t* project) noexcept {
    const PreserveLastError error;
    try {
        if (!project || !*project) return {};
        const auto path = fs::absolute(project).lexically_normal();
        if (!pmm_paths::IsProject(path) || path.extension().wstring().size() == 4) return path.wstring();
        const auto local = pmm_paths::Effect(path);
        std::lock_guard<std::mutex> lock(assignmentMutex);
        for (const auto& entry : projectAliases) {
            if (_wcsicmp(entry.local.c_str(), local.c_str()) == 0) {
                auto notification = entry.legacy;
                notification.replace_extension(L".pmm");
                if (AvailableAlias(entry.legacy) && AvailableAlias(notification)) return notification.wstring();
            }
        }
        if (projectAliases.size() >= kMaxProjectAliases) {
            runtime_log::Write("MME project notification alias capacity reached");
            return {};
        }
        // Both virtual spellings must be absent. Bound collision retries and
        // the old MME filename capacity; never reuse a sequence after wrap.
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            if (projectAliasSequence == std::numeric_limits<std::uint64_t>::max()) break;
            auto notification = path.parent_path() / (L"~mms-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(++projectAliasSequence) + L".pmm");
            auto legacy = notification;
            legacy.replace_extension(L".emm");
            if (notification.native().size() >= MAX_PATH) break;
            if (!AvailableAlias(notification) || !AvailableAlias(legacy)) continue;
            // Construct the return value before publishing the registration.
            auto result = notification.wstring();
            projectAliases.push_back({legacy, local});
            return result;
        }
        runtime_log::Write("MME project notification alias unavailable");
    } catch (...) {
        runtime_log::Write("MME project notification alias preparation failed");
    }
    return {};
}
}  // namespace mikumikustudio
