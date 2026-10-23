#pragma once
#include <mikumikustudio/cache_files.hpp>

#include <cstddef>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif

// Internal CNG primitives for the cache manager. Windows 7 capable: only APIs
// available through the static bcrypt import library are used.
namespace mikumikustudio::cache_files::crypto {
namespace detail {
// NTSTATUS success convention: STATUS_SUCCESS and informational codes are >= 0.
constexpr bool NtSuccess(NTSTATUS status) noexcept { return status >= 0; }
}

class Sha256 final {
public:
    Sha256() noexcept = default;
    ~Sha256() noexcept { Reset(); }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) = delete;
    Sha256& operator=(Sha256&&) = delete;

    bool Start() noexcept {
        if (provider != nullptr || hash != nullptr) {
            return false;
        }
        finished = false;
        failed = false;
        object.clear();

        NTSTATUS status = BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!detail::NtSuccess(status)) {
            provider = nullptr;
            Reset();
            return false;
        }

        ULONG objectBytes = 0;
        ULONG digestBytes = 0;
        if (!ReadProperty(BCRYPT_OBJECT_LENGTH, objectBytes)
            || !ReadProperty(BCRYPT_HASH_LENGTH, digestBytes)) {
            Reset();
            return false;
        }
        if (digestBytes != static_cast<ULONG>(Digest{}.size())
            || objectBytes == 0
            || objectBytes > kMaximumObjectBytes) {
            Reset();
            return false;
        }

        try {
            object.assign(objectBytes, UCHAR{0});
        } catch (...) {
            Reset();
            return false;
        }

        status = BCryptCreateHash(provider, &hash, object.data(), objectBytes, nullptr, 0, 0);
        if (!detail::NtSuccess(status)) {
            hash = nullptr;
            Reset();
            return false;
        }
        return true;
    }

    bool Update(const void* bytes, std::size_t size) noexcept {
        if (hash == nullptr || failed || finished) {
            return false;
        }
        if (bytes == nullptr && size != 0) {
            return false;
        }
        if (size == 0) {
            return true;
        }

        const UCHAR* cursor = static_cast<const UCHAR*>(bytes);
        std::size_t remaining = size;
        while (remaining != 0) {
            const std::size_t piece = remaining < kChunkBytes ? remaining : kChunkBytes;
            const NTSTATUS status = BCryptHashData(hash, const_cast<PUCHAR>(cursor), static_cast<ULONG>(piece), 0);
            if (!detail::NtSuccess(status)) {
                failed = true;
                return false;
            }
            cursor += piece;
            remaining -= piece;
        }
        return true;
    }

    bool Finish(Digest& output) noexcept {
        if (hash == nullptr || failed || finished) {
            return false;
        }

        Digest local{};
        const NTSTATUS status = BCryptFinishHash(
            hash, reinterpret_cast<PUCHAR>(local.data()), static_cast<ULONG>(local.size()), 0);
        if (!detail::NtSuccess(status)) {
            failed = true;
            return false;
        }
        output = local;
        finished = true;
        return true;
    }

private:
    bool ReadProperty(LPCWSTR name, ULONG& value) noexcept {
        value = 0;
        ULONG returned = 0;
        const NTSTATUS status = BCryptGetProperty(provider, name, reinterpret_cast<PUCHAR>(&value),
            static_cast<ULONG>(sizeof(value)), &returned, 0);
        return detail::NtSuccess(status) && returned == sizeof(value);
    }

    // The hash object buffer must outlive the hash handle. Reset destroys
    // handles before releasing the buffer, and the destructor calls Reset.
    void Reset() noexcept {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
            hash = nullptr;
        }
        if (provider != nullptr) {
            BCryptCloseAlgorithmProvider(provider, 0);
            provider = nullptr;
        }
        object.clear();
        finished = false;
        failed = false;
    }

    static constexpr ULONG kMaximumObjectBytes = 1024u * 1024u;
    static constexpr std::size_t kChunkBytes = 1024u * 1024u;

    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    bool finished = false;
    bool failed = false;
};

inline bool Random(Owner& output) noexcept {
    Owner local{};
    const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(local.data()),
        static_cast<ULONG>(local.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!detail::NtSuccess(status)) {
        return false;
    }
    output = local;
    return true;
}
}
