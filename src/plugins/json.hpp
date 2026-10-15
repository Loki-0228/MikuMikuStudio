#pragma once
#include "common.hpp"
#include <map>

namespace mikudancestudio::plugins {
// Small, bounded JSON reader for manifests. Numbers retain their spelling so
// schema integers never round or silently accept fractional/exponent values.
struct Json {
    enum Kind { Null, Boolean, Number, StringValue, Array, Object } kind = Null;
    std::string text;
    std::vector<Json> array;
    std::map<std::string, Json> object;
    const Json& At(const char* key) const;
    std::string String() const;
    uint32_t UInt() const;
    const std::vector<Json>& Items() const;
    static Json Parse(std::string_view text);
};
std::string ReadFile(const fs::path&, size_t maxBytes = 1024 * 1024);
}
