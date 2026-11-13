#pragma once
#include "negative_authoring.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace mikumikustudio::negative_frames::detail {
// Internal storage row, not a public plugin ABI. Original v1 rows carry a null
// author and retain their old interpolation semantics. Extended producers must
// supply the complete immutable author; current-pose inference is not a fallback.
struct Key {
    unsigned char* model=nullptr;
    const void* identity=nullptr;
    int target=0,frame=0;
    bool morph=false,selected=false;
    std::array<double,16> values{};
    std::int64_t clipboardOffset=0;
    authoring::Ptr author;
};
inline bool SameKeyData(const Key& a,const Key& b) noexcept {
    // Selection and clipboard offsets are transient UI data.
    return a.model==b.model && a.identity==b.identity && a.target==b.target &&
        a.frame==b.frame && a.morph==b.morph && a.values==b.values && authoring::Same(a.author,b.author);
}
inline std::uint64_t AddBytes(std::uint64_t a,std::uint64_t b) noexcept {
    return b>UINT64_MAX-a?UINT64_MAX:a+b;
}
inline std::uint64_t RetainedKeyBytes(const std::vector<Key>& keys) noexcept {
    std::uint64_t bytes=keys.capacity()>UINT64_MAX/sizeof(Key)?UINT64_MAX:
        std::uint64_t(keys.capacity())*sizeof(Key);
    // Charge each reference conservatively. Sharing must not hide retained
    // payloads; globally deduplicated resource accounting is a separate layer.
    for(const auto& key:keys)bytes=AddBytes(bytes,authoring::RetainedBytes(key.author));
    return bytes;
}
}
