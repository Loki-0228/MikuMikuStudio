#pragma once
#include "cache_business_codec.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace mikumikustudio::cache_business::wire {
struct Invalid {};
// Magic is exactly 16 ASCII bytes, without a string terminator on the wire.
inline constexpr char PageMagic[16]={'M','M','S','C','a','c','h','e','P','a','g','e','0','0','0','1'};
inline constexpr char ManifestMagic[16]={'M','M','S','C','a','c','h','e','I','n','d','e','x','0','0','1'};
struct Reader {
    const std::uint8_t* data;
    std::size_t size,at=0;
    void Need(std::size_t n) const {if(at>size || n>size-at)throw Invalid{};}
    std::uint64_t Get(unsigned n) {if(n>8)throw Invalid{};Need(n);std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(data[at++])<<(8*i);return v;}
    void Zero(std::size_t n){Need(n);while(n--)if(data[at++]!=0)throw Invalid{};}
    void Magic(const char (&magic)[16]){Need(16);if(std::memcmp(data+at,magic,16))throw Invalid{};at+=16;}
    template<std::size_t N> void Array(std::array<std::uint8_t,N>& a){Need(N);std::copy_n(data+at,N,a.begin());at+=N;}
    std::u16string String(std::size_t n){if(n>MaximumPathUnits)throw Invalid{};Need(n*2);std::u16string s;s.resize(n);for(auto& c:s)c=static_cast<char16_t>(Get(2));return s;}
    cache_files::BlobInfo Blob(){
        Need(160);cache_files::BlobInfo b;b.name.resize(37);for(auto& c:b.name)c=static_cast<wchar_t>(Get(1));Zero(3);
        b.spec.kind=static_cast<cache_files::Kind>(Get(4));Zero(4);Array(b.spec.owner);Array(b.spec.key);Array(b.payloadDigest);
        b.payloadBytes=Get(8);b.identity.volume=static_cast<std::uint32_t>(Get(4));Zero(4);b.identity.fileIndex=Get(8);b.identity.fileBytes=Get(8);return b;
    }
};
struct Writer {
    Bytes bytes;
    std::size_t at=0;
    explicit Writer(std::size_t size):bytes(size,0){}
    void Need(std::size_t n) const {if(at>bytes.size() || n>bytes.size()-at)throw Invalid{};}
    void Put(std::uint64_t v,unsigned n){if(n>8)throw Invalid{};Need(n);for(unsigned i=0;i<n;++i)bytes[at++]=static_cast<std::uint8_t>(v>>(8*i));}
    void Zero(std::size_t n){Need(n);at+=n;}
    void Magic(const char (&magic)[16]){Need(16);std::copy_n(reinterpret_cast<const std::uint8_t*>(magic),16,bytes.begin()+at);at+=16;}
    template<std::size_t N> void Array(const std::array<std::uint8_t,N>& a){Need(N);std::copy(a.begin(),a.end(),bytes.begin()+at);at+=N;}
    void String(const std::u16string& s){for(auto c:s)Put(static_cast<std::uint16_t>(c),2);}
    void Blob(const cache_files::BlobInfo& b){
        if(b.name.size()!=37)throw Invalid{};for(auto c:b.name)Put(static_cast<std::uint8_t>(c),1);Zero(3);
        Put(static_cast<std::uint32_t>(b.spec.kind),4);Zero(4);Array(b.spec.owner);Array(b.spec.key);Array(b.payloadDigest);
        Put(b.payloadBytes,8);Put(b.identity.volume,4);Zero(4);Put(b.identity.fileIndex,8);Put(b.identity.fileBytes,8);
    }
};
}
