#pragma once
#include "common.hpp"
#include <array>
#include <bcrypt.h>
namespace mikumikustudio::plugins {
class Digest {
    struct State {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        ~State() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } state;
public:
    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;
    Digest() {
        if (BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("scene digest provider");
        DWORD size = 0, written = 0;
        if (BCryptGetProperty(state.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0) < 0) throw std::runtime_error("scene digest size");
        state.object.resize(size);
        if (BCryptCreateHash(state.algorithm, &state.hash, state.object.data(), size, nullptr, 0, 0) < 0) throw std::runtime_error("scene digest create");
    }
    void Add(const void* value, size_t bytes) {
        const auto* data = static_cast<const unsigned char*>(value);
        while (bytes) {
            const auto count = ULONG(std::min(bytes, size_t(ULONG_MAX)));
            if (BCryptHashData(state.hash, const_cast<PUCHAR>(data), count, 0) < 0) throw std::runtime_error("scene digest update");
            data += count; bytes -= count;
        }
    }
    template<class T> void Add(const std::vector<T>& values) { const uint64_t count = values.size(); Add(&count, sizeof(count)); Add(values.data(), values.size() * sizeof(T)); }
    std::array<unsigned char,32> Finish() {
        std::array<unsigned char,32> result{};
        if (BCryptFinishHash(state.hash, result.data(), ULONG(result.size()), 0) < 0) throw std::runtime_error("scene digest finish");
        return result;
    }
};
}
