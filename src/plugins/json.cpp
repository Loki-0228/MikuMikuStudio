#include "json.hpp"
#include <fstream>
#include <limits>

namespace mikudancestudio::plugins {
namespace {
struct Reader {
    std::string_view source;
    size_t pos = 0, nodes = 0;
    [[noreturn]] void Fail() const { throw std::invalid_argument("invalid JSON at byte " + std::to_string(pos)); }
    void Space() { while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\n' || source[pos] == '\r' || source[pos] == '\t')) ++pos; }
    bool Take(char c) { Space(); if (pos < source.size() && source[pos] == c) { ++pos; return true; } return false; }
    unsigned Hex() {
        unsigned n = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos == source.size()) Fail(); char c = source[pos++];
            unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d == 16) Fail(); n = n * 16 + d;
        }
        return n;
    }
    std::string Str() {
        if (!Take('"')) Fail(); std::string out;
        while (pos < source.size()) {
            unsigned char c = source[pos++];
            if (c == '"') { plugins::String(Text(out)); return out; }
            if (c < 32) Fail();
            if (c != '\\') { out += char(c); continue; }
            if (pos == source.size()) Fail();
            switch (source[pos++]) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                unsigned a = Hex(); std::wstring w;
                if (a >= 0xd800 && a <= 0xdbff) {
                    if (pos + 2 > source.size() || source[pos++] != '\\' || source[pos++] != 'u') Fail();
                    unsigned b = Hex(); if (b < 0xdc00 || b > 0xdfff) Fail(); w = {wchar_t(a), wchar_t(b)};
                } else { if (a >= 0xdc00 && a <= 0xdfff) Fail(); w = {wchar_t(a)}; }
                out += Utf8(w); break;
            }
            default: Fail();
            }
        }
        Fail();
    }
    Json Value(unsigned depth = 0) {
        if (depth > 48 || ++nodes > 32768) Fail(); Space(); if (pos == source.size()) Fail(); Json j;
        if (source[pos] == '"') { j.kind = Json::StringValue; j.text = Str(); return j; }
        if (Take('{')) {
            j.kind = Json::Object; if (Take('}')) return j;
            do { auto key = Str(); if (!Take(':')) Fail(); auto value = Value(depth + 1);
                if (!j.object.emplace(std::move(key), std::move(value)).second) Fail();
            } while (Take(','));
            if (!Take('}')) Fail(); return j;
        }
        if (Take('[')) {
            j.kind = Json::Array; if (Take(']')) return j;
            do { j.array.push_back(Value(depth + 1)); } while (Take(','));
            if (!Take(']')) Fail(); return j;
        }
        for (const char* literal : {"null", "true", "false"}) {
            const size_t n = std::strlen(literal);
            if (source.substr(pos, n) == literal) { pos += n; j.kind = *literal == 'n' ? Json::Null : Json::Boolean; j.text = literal; return j; }
        }
        size_t start = pos; if (source[pos] == '-') ++pos;
        auto digit = [&] { return pos < source.size() && source[pos] >= '0' && source[pos] <= '9'; };
        if (!digit()) Fail();
        if (source[pos] == '0') ++pos; else while (digit()) ++pos;
        if (pos < source.size() && source[pos] == '.') { ++pos; if (!digit()) Fail(); while (digit()) ++pos; }
        if (pos < source.size() && (source[pos] == 'e' || source[pos] == 'E')) {
            ++pos; if (pos < source.size() && (source[pos] == '+' || source[pos] == '-')) ++pos;
            if (!digit()) Fail(); while (digit()) ++pos;
        }
        j.kind = Json::Number; j.text = source.substr(start, pos - start); return j;
    }
};
}
Json Json::Parse(std::string_view s) {
    if (s.size() > 1024 * 1024) throw std::invalid_argument("JSON exceeds 1 MiB");
    if (s.substr(0, 3) == "\xef\xbb\xbf") s.remove_prefix(3);
    Reader r{s}; auto out = r.Value(); r.Space(); if (r.pos != s.size()) r.Fail(); return out;
}
const Json& Json::At(const char* key) const {
    if (kind != Object) throw std::invalid_argument("expected JSON object");
    auto it = object.find(key); if (it == object.end()) throw std::invalid_argument(std::string("missing field: ") + key); return it->second;
}
std::string Json::String() const { if (kind != StringValue) throw std::invalid_argument("expected string"); return text; }
uint32_t Json::UInt() const {
    if (kind != Number || text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("expected unsigned integer");
    uint64_t n = 0; for (char c : text) { n = n * 10 + c - '0'; if (n > UINT32_MAX) throw std::invalid_argument("integer overflow"); } return uint32_t(n);
}
const std::vector<Json>& Json::Items() const { if (kind != Array) throw std::invalid_argument("expected array"); return array; }
std::string ReadFile(const fs::path& path, size_t maxBytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate); if (!f) throw std::runtime_error("cannot open " + Utf8(path.native()));
    auto size = f.tellg(); if (size < 0 || uint64_t(size) > maxBytes) throw std::invalid_argument("file size limit");
    std::string bytes(size_t(size), '\0'); f.seekg(0); if (!bytes.empty() && !f.read(bytes.data(), bytes.size())) throw std::runtime_error("short read"); return bytes;
}
}
