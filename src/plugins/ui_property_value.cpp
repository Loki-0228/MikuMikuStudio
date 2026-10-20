#include "ui_property_value.hpp"
#include "common.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <system_error>

namespace mikumikustudio::plugins {
namespace {

constexpr uint64_t kMaxTextBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxByteBytes = 16ull * 1024ull * 1024ull;
constexpr char kHexDigits[] = "0123456789abcdef";

// Any escaped exception is a contract violation; bad_alloc keeps its own code.
template<class F>
MmsResult Attempt(F&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        return MMS_E_MEMORY;
    } catch (...) {
        return MMS_E_ARGUMENT;
    }
}

bool IntegerSyntax(std::string_view field, bool allowMinus) noexcept {
    if (field.empty()) return false;
    size_t at = 0;
    if (field[0] == '-') {
        if (!allowMinus) return false;
        at = 1;
    }
    if (at == field.size()) return false;
    for (size_t i = at; i < field.size(); ++i)
        if (field[i] < '0' || field[i] > '9') return false;
    return true;
}

bool ParseInteger(std::string_view field, bool allowMinus, int64_t& output) noexcept {
    if (!IntegerSyntax(field, allowMinus)) return false;
    const char* begin = field.data();
    const char* end = begin + field.size();
    int64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
    output = value;
    return true;
}

bool ParseUnsignedInteger(std::string_view field, uint64_t& output) noexcept {
    if (!IntegerSyntax(field, false)) return false;
    const char* begin = field.data();
    const char* end = begin + field.size();
    uint64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
    output = value;
    return true;
}

// Strict decimal floating point: optional minus, then an integer part and/or a
// fractional part. At least one digit must be present overall, so ".5", "-.5",
// "1." and "1.e2" are valid while a bare "." is not. An optional exponent with
// digits may follow. No whitespace, no leading plus, no hex, no inf/nan.
bool FloatSyntax(std::string_view field) noexcept {
    const size_t size = field.size();
    size_t at = 0;
    if (size == 0) return false;
    if (field[at] == '-') ++at;
    size_t digits = 0;
    while (at < size && field[at] >= '0' && field[at] <= '9') { ++at; ++digits; }
    if (at < size && field[at] == '.') {
        ++at;
        while (at < size && field[at] >= '0' && field[at] <= '9') { ++at; ++digits; }
    }
    if (digits == 0) return false;
    if (at < size && (field[at] == 'e' || field[at] == 'E')) {
        ++at;
        if (at < size && (field[at] == '+' || field[at] == '-')) ++at;
        size_t exponentDigits = 0;
        while (at < size && field[at] >= '0' && field[at] <= '9') { ++at; ++exponentDigits; }
        if (exponentDigits == 0) return false;
    }
    return at == size;
}

bool ParseFloat(std::string_view field, double& output) noexcept {
    if (!FloatSyntax(field)) return false;
    const char* begin = field.data();
    const char* end = begin + field.size();
    double value = 0.0;
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value)) return false;
    if (value == 0.0) {
        // A nonzero significand that collapses to zero underflowed even when the
        // implementation reports success, so reject it explicitly.
        for (char c : field) {
            if (c == 'e' || c == 'E') break;
            if (c >= '1' && c <= '9') return false;
        }
    }
    output = value;
    return true;
}

int HexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseId(std::string_view field, MmsId& output) noexcept {
    if (field.size() != 32) return false;
    uint64_t high = 0, low = 0;
    for (size_t i = 0; i < 32; ++i) {
        const int nibble = HexNibble(field[i]);
        if (nibble < 0) return false;
        if (i < 16) high = (high << 4) | static_cast<uint64_t>(nibble);
        else low = (low << 4) | static_cast<uint64_t>(nibble);
    }
    output.hi = high;
    output.lo = low;
    return true;
}

bool ParseBytes(std::string_view field, std::vector<unsigned char>& output) {
    if ((field.size() & 1u) != 0) return false;
    const size_t count = field.size() / 2;
    if (count > kMaxByteBytes) return false;
    for (char c : field) if (HexNibble(c) < 0) return false;
    std::vector<unsigned char> bytes(count);
    for (size_t i = 0; i < count; ++i) {
        const int high = HexNibble(field[i * 2]);
        const int low = HexNibble(field[i * 2 + 1]);
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    output = std::move(bytes);
    return true;
}

std::string HexU64(uint64_t value) {
    char buffer[16];
    for (int i = 15; i >= 0; --i) {
        buffer[i] = kHexDigits[static_cast<size_t>(value & 0xfu)];
        value >>= 4;
    }
    return std::string(buffer, sizeof(buffer));
}

bool FormatFloat(double value, std::string& output) {
    char buffer[64];
    const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (formatted.ec != std::errc{} || !std::isfinite(value)) return false;
    output.assign(buffer, formatted.ptr);
    return true;
}

} // namespace

UiPropertyValue::UiPropertyValue(UiPropertyValue&& other) noexcept
    : type_(other.type_), components_(other.components_), signedValue_(other.signedValue_),
      unsignedValue_(other.unsignedValue_), number_(other.number_), id_(other.id_),
      text_(std::move(other.text_)), bytes_(std::move(other.bytes_)) {
    std::copy(std::begin(other.vector_), std::end(other.vector_), std::begin(vector_));
    other.Reset();
}

UiPropertyValue& UiPropertyValue::operator=(UiPropertyValue&& other) noexcept {
    if (this != &other) {
        type_ = other.type_;
        components_ = other.components_;
        signedValue_ = other.signedValue_;
        unsignedValue_ = other.unsignedValue_;
        number_ = other.number_;
        std::copy(std::begin(other.vector_), std::end(other.vector_), std::begin(vector_));
        id_ = other.id_;
        text_ = std::move(other.text_);
        bytes_ = std::move(other.bytes_);
        other.Reset();
    }
    return *this;
}

void UiPropertyValue::Reset() noexcept {
    type_ = MMS_VALUE_EMPTY;
    components_ = 0;
    signedValue_ = 0;
    unsignedValue_ = 0;
    number_ = 0.0;
    std::fill(std::begin(vector_), std::end(vector_), 0.0);
    id_ = MmsId{0, 0};
    text_.clear();
    bytes_.clear();
}

MmsResult UiPropertyValue::Copy(const MmsValue& source, UiPropertyValue& output) noexcept {
    return Attempt([&]() -> MmsResult {
        if (source.type < MMS_VALUE_BOOL || source.type > MMS_VALUE_BYTES) return MMS_E_ARGUMENT;
        if (source.components > 4) return MMS_E_ARGUMENT;
        if (source.type == MMS_VALUE_VECTOR && source.components == 0) return MMS_E_ARGUMENT;
        UiPropertyValue candidate;
        candidate.type_ = source.type;
        candidate.components_ = source.components;
        switch (source.type) {
        case MMS_VALUE_BOOL:
            if (source.value.unsignedValue > 1) return MMS_E_ARGUMENT;
            candidate.unsignedValue_ = source.value.unsignedValue;
            break;
        case MMS_VALUE_INT:
            candidate.signedValue_ = source.value.signedValue;
            break;
        case MMS_VALUE_UINT:
            candidate.unsignedValue_ = source.value.unsignedValue;
            break;
        case MMS_VALUE_NUMBER:
            if (!std::isfinite(source.value.number)) return MMS_E_ARGUMENT;
            candidate.number_ = source.value.number;
            break;
        case MMS_VALUE_VECTOR:
            for (uint32_t i = 0; i < source.components; ++i) {
                if (!std::isfinite(source.value.vector[i])) return MMS_E_ARGUMENT;
                candidate.vector_[i] = source.value.vector[i];
            }
            break;
        case MMS_VALUE_TEXT:
            candidate.text_ = String(source.value.text);
            break;
        case MMS_VALUE_ID:
            candidate.id_ = source.value.id;
            break;
        case MMS_VALUE_BYTES: {
            const MmsBytes bytes = source.value.bytes;
            if (bytes.bytes > kMaxByteBytes || (bytes.bytes && !bytes.data)) return MMS_E_ARGUMENT;
            const size_t count = static_cast<size_t>(bytes.bytes);
            candidate.bytes_.resize(count);
            if (count) std::memcpy(candidate.bytes_.data(), bytes.data, count);
            break;
        }
        default:
            return MMS_E_ARGUMENT;
        }
        output = std::move(candidate);
        return MMS_OK;
    });
}

MmsResult UiPropertyValue::Parse(uint32_t type, uint32_t components,
        const std::vector<std::string_view>& fields, UiPropertyValue& output) noexcept {
    return Attempt([&]() -> MmsResult {
        if (type < MMS_VALUE_BOOL || type > MMS_VALUE_BYTES) return MMS_E_ARGUMENT;
        if (components > 4) return MMS_E_ARGUMENT;
        if (type == MMS_VALUE_VECTOR) {
            if (components == 0 || fields.size() != components) return MMS_E_ARGUMENT;
        } else if (fields.size() != 1) {
            return MMS_E_ARGUMENT;
        }
        UiPropertyValue candidate;
        candidate.type_ = type;
        candidate.components_ = components;
        switch (type) {
        case MMS_VALUE_BOOL:
            if (fields[0] == "0") candidate.unsignedValue_ = 0;
            else if (fields[0] == "1") candidate.unsignedValue_ = 1;
            else return MMS_E_ARGUMENT;
            break;
        case MMS_VALUE_INT: {
            int64_t value = 0;
            if (!ParseInteger(fields[0], true, value)) return MMS_E_ARGUMENT;
            candidate.signedValue_ = value;
            break;
        }
        case MMS_VALUE_UINT: {
            uint64_t value = 0;
            if (!ParseUnsignedInteger(fields[0], value)) return MMS_E_ARGUMENT;
            candidate.unsignedValue_ = value;
            break;
        }
        case MMS_VALUE_NUMBER: {
            double value = 0.0;
            if (!ParseFloat(fields[0], value)) return MMS_E_ARGUMENT;
            candidate.number_ = value;
            break;
        }
        case MMS_VALUE_VECTOR:
            for (uint32_t i = 0; i < components; ++i)
                if (!ParseFloat(fields[i], candidate.vector_[i])) return MMS_E_ARGUMENT;
            break;
        case MMS_VALUE_TEXT:
            candidate.text_ = String(MmsString{fields[0].data(), static_cast<uint64_t>(fields[0].size())});
            break;
        case MMS_VALUE_ID:
            if (!ParseId(fields[0], candidate.id_)) return MMS_E_ARGUMENT;
            break;
        case MMS_VALUE_BYTES:
            if (!ParseBytes(fields[0], candidate.bytes_)) return MMS_E_ARGUMENT;
            break;
        default:
            return MMS_E_ARGUMENT;
        }
        output = std::move(candidate);
        return MMS_OK;
    });
}

MmsValue UiPropertyValue::View() const noexcept {
    MmsValue value{};
    value.type = type_;
    value.components = components_;
    switch (type_) {
    case MMS_VALUE_BOOL:
        value.value.unsignedValue = unsignedValue_;
        break;
    case MMS_VALUE_INT:
        value.value.signedValue = signedValue_;
        break;
    case MMS_VALUE_UINT:
        value.value.unsignedValue = unsignedValue_;
        break;
    case MMS_VALUE_NUMBER:
        value.value.number = number_;
        break;
    case MMS_VALUE_VECTOR:
        for (uint32_t i = 0; i < components_ && i < 4; ++i) value.value.vector[i] = vector_[i];
        break;
    case MMS_VALUE_TEXT:
        value.value.text = MmsString{text_.data(), static_cast<uint64_t>(text_.size())};
        break;
    case MMS_VALUE_ID:
        value.value.id = id_;
        break;
    case MMS_VALUE_BYTES:
        value.value.bytes = MmsBytes{bytes_.data(), static_cast<uint64_t>(bytes_.size())};
        break;
    default:
        break;
    }
    return value;
}

MmsResult UiPropertyValue::Format(std::vector<std::string>& fields) const noexcept {
    return Attempt([&]() -> MmsResult {
        std::vector<std::string> result;
        switch (type_) {
        case MMS_VALUE_BOOL:
            result.emplace_back(unsignedValue_ ? "1" : "0");
            break;
        case MMS_VALUE_INT: {
            char buffer[32];
            const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), signedValue_);
            if (formatted.ec != std::errc{}) return MMS_E_ARGUMENT;
            result.emplace_back(buffer, formatted.ptr);
            break;
        }
        case MMS_VALUE_UINT: {
            char buffer[32];
            const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), unsignedValue_);
            if (formatted.ec != std::errc{}) return MMS_E_ARGUMENT;
            result.emplace_back(buffer, formatted.ptr);
            break;
        }
        case MMS_VALUE_NUMBER: {
            std::string field;
            if (!FormatFloat(number_, field)) return MMS_E_ARGUMENT;
            result.push_back(std::move(field));
            break;
        }
        case MMS_VALUE_VECTOR:
            if (components_ == 0 || components_ > 4) return MMS_E_ARGUMENT;
            for (uint32_t i = 0; i < components_; ++i) {
                std::string field;
                if (!FormatFloat(vector_[i], field)) return MMS_E_ARGUMENT;
                result.push_back(std::move(field));
            }
            break;
        case MMS_VALUE_TEXT:
            result.push_back(text_);
            break;
        case MMS_VALUE_ID:
            result.push_back(HexU64(id_.hi) + HexU64(id_.lo));
            break;
        case MMS_VALUE_BYTES: {
            std::string field;
            field.reserve(bytes_.size() * 2);
            for (unsigned char byte : bytes_) {
                field += kHexDigits[static_cast<size_t>(byte >> 4)];
                field += kHexDigits[static_cast<size_t>(byte & 0xfu)];
            }
            result.push_back(std::move(field));
            break;
        }
        default:
            return MMS_E_ARGUMENT;
        }
        fields.swap(result);
        return MMS_OK;
    });
}

bool UiPropertyValue::Equals(const UiPropertyValue& other) const noexcept {
    if (type_ != other.type_ || components_ != other.components_) return false;
    switch (type_) {
    case MMS_VALUE_EMPTY:
        return true;
    case MMS_VALUE_BOOL:
    case MMS_VALUE_UINT:
        return unsignedValue_ == other.unsignedValue_;
    case MMS_VALUE_INT:
        return signedValue_ == other.signedValue_;
    case MMS_VALUE_NUMBER:
        return number_ == other.number_;
    case MMS_VALUE_VECTOR:
        for (uint32_t i = 0; i < components_ && i < 4; ++i)
            if (vector_[i] != other.vector_[i]) return false;
        return true;
    case MMS_VALUE_TEXT:
        return text_ == other.text_;
    case MMS_VALUE_ID:
        return id_.lo == other.id_.lo && id_.hi == other.id_.hi;
    case MMS_VALUE_BYTES:
        return bytes_ == other.bytes_;
    default:
        return false;
    }
}

} // namespace mikumikustudio::plugins
