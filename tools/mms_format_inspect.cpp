#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "mms_format_probe.hpp"

#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace mikumikustudio::format_probe;

// Unbuffered input keeps the byte budget true at the OS boundary. No write
// access, and no attempt to read devices, directories, or POSIX pipes.
class Input {
public:
#ifdef _WIN32
    using Char = wchar_t;
    explicit Input(const Char* path)
        : handle_(CreateFileW(path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~Input() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }

    bool Size(std::uint64_t& size) const {
        LARGE_INTEGER n{};
        if (GetFileType(handle_) != FILE_TYPE_DISK ||
            !GetFileSizeEx(handle_, &n) || n.QuadPart < 0) return false;
        size = static_cast<std::uint64_t>(n.QuadPart);
        return true;
    }

    bool ReadAt(std::uint64_t offset, char* buffer, std::size_t count) const {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        DWORD actual = 0;
        const auto want = static_cast<DWORD>(count);
        return SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN) &&
            ReadFile(handle_, buffer, want, &actual, nullptr) && actual == want;
    }

private:
    HANDLE handle_;
#else
    using Char = char;
    explicit Input(const Char* path) : fd_(open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) {}
    ~Input() { if (fd_ >= 0) close(fd_); }

    bool Size(std::uint64_t& size) const {
        struct stat info{};
        if (fd_ < 0 || fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
            return false;
        size = static_cast<std::uint64_t>(info.st_size);
        return true;
    }

    bool ReadAt(std::uint64_t offset, char* buffer, std::size_t count) const {
        while (count) {
            const auto n = pread(fd_, buffer, count, static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            buffer += n;
            count -= static_cast<std::size_t>(n);
            offset += static_cast<std::uint64_t>(n);
        }
        return true;
    }

private:
    int fd_;
#endif
public:
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
};

std::string_view Name(Status value) {
    switch (value) {
    case Status::Recognized: return "recognized";
    case Status::Incomplete: return "incomplete";
    case Status::Unrecognized: return "unrecognized";
    case Status::IoError: return "io_error";
    }
    return "unknown";
}

std::string_view Name(Format value) {
    switch (value) {
    case Format::Pmm: return "pmm";
    case Format::Pmx: return "pmx";
    case Format::Vmd: return "vmd";
    case Format::Unknown: return {};
    }
    return {};
}

std::string_view Name(Gate value) {
    switch (value) {
    case Gate::Accepted: return "accepted";
    case Gate::Rejected: return "rejected";
    case Gate::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view Name(Footer value) {
    switch (value) {
    case Footer::NotChecked: return "not_checked";
    case Footer::NotDetected: return "not_detected";
    case Footer::MarkerOnly: return "marker_only";
    }
    return "not_checked";
}

void PrintJson(const Result& result) {
    // All values are fixed ASCII tokens. Do not echo paths or raw file bytes.
    const auto token = [](std::string_view value) {
        if (value.empty()) std::cout << "null";
        else std::cout << '"' << value << '"';
    };
    std::cout << "{\"schema\":1,\"status\":";
    token(Name(result.status));
    std::cout << ",\"format\":";
    token(Name(result.format));
    std::cout << ",\"version\":";
    token(result.version);
    std::cout << ",\"encoding\":";
    token(result.encoding);
    std::cout << ",\"native_header\":{\"full\":";
    token(Name(result.full));
    std::cout << ",\"original\":";
    token(Name(result.original));
    std::cout << "},\"pmm3end\":";
    token(Name(result.pmm3end));
    std::cout << ",\"bytes_examined\":" << result.bytesExamined
              << ",\"validation\":\"header_only\"}\n";
}

void PrintText(const Result& result) {
    if (result.status == Status::IoError) {
        std::cerr << "Could not inspect the input as a readable regular file.\n";
        return;
    }
    const auto format = Name(result.format);
    std::cout << "Status: " << Name(result.status) << "\nFormat: "
              << (format.empty() ? "unknown" : format);
    if (!result.version.empty()) std::cout << ' ' << result.version;
    if (!result.encoding.empty()) std::cout << " (" << result.encoding << ')';
    std::cout << "\nNative header gates: full=" << Name(result.full)
              << ", original=" << Name(result.original) << '\n';
    if (result.pmm3end == Footer::MarkerOnly)
        std::cout << "PMM3END: standalone codec marker only (not verified)\n";
    else if (result.pmm3end == Footer::NotDetected)
        std::cout << "PMM3END: marker not detected\n";
    std::cout << "Scope: header inspection only (" << result.bytesExamined
              << " bytes read); file contents not validated or imported.\n";
}

void Usage(std::ostream& out) {
    out << "Usage: mms_format_inspect [--json] [--] FILE\n"
           "       mms_format_inspect --help\n"
           "Inspect PMM, PMX, or VMD headers by content. Does not validate or import files.\n"
           "Exit: 0 recognized, 1 incomplete or unrecognized, 2 usage or I/O error.\n";
}

bool IsOption(std::basic_string_view<Input::Char> arg, std::string_view option) {
    return std::equal(arg.begin(), arg.end(), option.begin(), option.end());
}

int Run(int argc, Input::Char** argv) {
    if (argc == 2 && IsOption(argv[1], "--help")) {
        Usage(std::cout);
        return 0;
    }
    const Input::Char* path = nullptr;
    bool json = false, options = true;
    for (int i = 1; i < argc; ++i) {
        const std::basic_string_view<Input::Char> arg = argv[i];
        if (options && !path && !json && IsOption(arg, "--json")) json = true;
        else if (options && !path && IsOption(arg, "--")) options = false;
        else if (!path && !arg.empty() && (!options || arg.front() != '-')) path = argv[i];
        else {
            Usage(std::cerr);
            return 2;
        }
    }
    if (!path) {
        Usage(std::cerr);
        return 2;
    }

    const Input input(path);
    std::uint64_t size = 0;
    const auto result = input.Size(size)
        ? Inspect(size, [&](auto offset, auto* bytes, auto count) {
              return input.ReadAt(offset, bytes, count);
          })
        : Result{Status::IoError};
    if (json) PrintJson(result);
    else PrintText(result);
    if (result.status == Status::Recognized) return 0;
    return result.status == Status::IoError ? 2 : 1;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return Run(argc, argv); }
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
