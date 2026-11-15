#pragma once

// Test-only POSIX equivalents for the standalone codec's MSVC descriptor API.
// Never on the include path of the inspector itself or shipped Windows builds.
#include <cstdint>
#include <sys/stat.h>
#include <unistd.h>

using __int64 = std::int64_t;
static_assert(sizeof(off_t) >= sizeof(__int64));

inline __int64 _telli64(int fd) { return lseek(fd, 0, SEEK_CUR); }
inline __int64 _lseeki64(int fd, __int64 offset, int whence) { return lseek(fd, offset, whence); }
inline __int64 _filelengthi64(int fd) {
    struct stat info{};
    return fstat(fd, &info) == 0 ? info.st_size : -1;
}
inline int _read(int fd, void* out, unsigned int count) { return read(fd, out, count); }
inline int _write(int fd, const void* in, unsigned int count) { return write(fd, in, count); }
