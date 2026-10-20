// Standalone contract tests for the owned UI property value codec.
// Independent console main; no test framework and no NDEBUG-dependent asserts.
//
// Build example:
//   cl /nologo /std:c++17 /EHsc /utf-8 /I sdk\mms\include ^
//      tests\plugins\ui_property_value_tests.cpp src\plugins\ui_property_value.cpp ^
//      /Fe:build\verification\ui-value-codec-20261020\ui_property_value_tests.exe
#include "../../src/plugins/ui_property_value.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using mikumikustudio::plugins::UiPropertyValue;

namespace {
bool g_failAllocations = false;
unsigned g_checks = 0;
unsigned g_failures = 0;

const char* ResultName(MmsResult result) {
    switch (result) {
    case MMS_OK: return "MMS_OK";
    case MMS_E_ARGUMENT: return "MMS_E_ARGUMENT";
    case MMS_E_MEMORY: return "MMS_E_MEMORY";
    default: return "other";
    }
}

void Check(bool ok, const char* expression, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
}

void CheckResult(MmsResult actual, MmsResult expected, const char* expression, const char* file, int line) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s returned %s, expected %s\n",
        file, line, expression, ResultName(actual), ResultName(expected));
}
} // namespace

// Allocation failures are how the tests distinguish "rejected before any
// allocation" (MMS_E_ARGUMENT) from "tried to allocate" (MMS_E_MEMORY).
void* operator new(std::size_t size) {
    if (g_failAllocations) throw std::bad_alloc();
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)
#define CHECK_RESULT(expression, expected) CheckResult((expression), (expected), #expression, __FILE__, __LINE__)

MmsValue EmptyValue() { return MmsValue{}; }

MmsValue BoolValue(uint64_t value) {
    MmsValue result{};
    result.type = MMS_VALUE_BOOL;
    result.value.unsignedValue = value;
    return result;
}

MmsValue IntValue(int64_t value) {
    MmsValue result{};
    result.type = MMS_VALUE_INT;
    result.value.signedValue = value;
    return result;
}

MmsValue UintValue(uint64_t value) {
    MmsValue result{};
    result.type = MMS_VALUE_UINT;
    result.value.unsignedValue = value;
    return result;
}

MmsValue NumberValue(double value) {
    MmsValue result{};
    result.type = MMS_VALUE_NUMBER;
    result.value.number = value;
    return result;
}

MmsValue VectorValue(std::initializer_list<double> values) {
    MmsValue result{};
    result.type = MMS_VALUE_VECTOR;
    result.components = static_cast<uint32_t>(values.size());
    uint32_t at = 0;
    for (double value : values) {
        if (at == 4) break;
        result.value.vector[at++] = value;
    }
    return result;
}

MmsValue TextValue(const void* data, size_t size) {
    MmsValue result{};
    result.type = MMS_VALUE_TEXT;
    result.value.text = MmsString{static_cast<const char*>(data), static_cast<uint64_t>(size)};
    return result;
}

MmsValue BytesValue(const void* data, size_t size) {
    MmsValue result{};
    result.type = MMS_VALUE_BYTES;
    result.value.bytes = MmsBytes{data, static_cast<uint64_t>(size)};
    return result;
}

MmsValue IdValue(uint64_t high, uint64_t low) {
    MmsValue result{};
    result.type = MMS_VALUE_ID;
    result.value.id = MmsId{low, high};
    return result;
}

std::string TextOf(const MmsValue& value) {
    if (value.value.text.bytes == 0) return std::string();
    return std::string(value.value.text.data, static_cast<size_t>(value.value.text.bytes));
}

std::string BytesOf(const MmsValue& value) {
    if (value.value.bytes.bytes == 0) return std::string();
    return std::string(static_cast<const char*>(value.value.bytes.data), static_cast<size_t>(value.value.bytes.bytes));
}

std::vector<std::string_view> Views(const std::vector<std::string>& fields) {
    std::vector<std::string_view> views;
    views.reserve(fields.size());
    for (const std::string& field : fields) views.emplace_back(field);
    return views;
}

MmsResult ParseFields(uint32_t type, uint32_t components, std::initializer_list<std::string_view> fields) {
    std::vector<std::string_view> views(fields.begin(), fields.end());
    UiPropertyValue output;
    return UiPropertyValue::Parse(type, components, views, output);
}

MmsResult ParseFields(uint32_t type, uint32_t components, const std::vector<std::string_view>& fields) {
    UiPropertyValue output;
    return UiPropertyValue::Parse(type, components, fields, output);
}

bool ParseInto(UiPropertyValue& output, uint32_t type, uint32_t components, std::initializer_list<std::string_view> fields) {
    std::vector<std::string_view> views(fields.begin(), fields.end());
    return UiPropertyValue::Parse(type, components, views, output) == MMS_OK;
}

bool ExpectArgument(uint32_t type, uint32_t components, std::initializer_list<std::string_view> fields) {
    return ParseFields(type, components, fields) == MMS_E_ARGUMENT;
}

bool Roundtrips(const MmsValue& source) {
    UiPropertyValue first;
    if (UiPropertyValue::Copy(source, first) != MMS_OK) return false;
    std::vector<std::string> fields;
    if (first.Format(fields) != MMS_OK) return false;
    UiPropertyValue second;
    if (UiPropertyValue::Parse(source.type, source.components, Views(fields), second) != MMS_OK) return false;
    return first.Equals(second);
}

bool SameValues(const MmsValue& left, const MmsValue& right) {
    UiPropertyValue leftValue;
    UiPropertyValue rightValue;
    if (UiPropertyValue::Copy(left, leftValue) != MMS_OK) return false;
    if (UiPropertyValue::Copy(right, rightValue) != MMS_OK) return false;
    return leftValue.Equals(rightValue);
}

void CopyAndViewChecks() {
    {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(BoolValue(1), value), MMS_OK);
        const MmsValue view = value.View();
        CHECK(view.type == MMS_VALUE_BOOL);
        CHECK(view.components == 0);
        CHECK(view.value.unsignedValue == 1);
        CHECK(Roundtrips(BoolValue(1)));
        CHECK(Roundtrips(BoolValue(0)));
    }
    const int64_t integers[] = {
        int64_t(0), int64_t(-1), int64_t(42),
        std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    for (int64_t number : integers) {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(IntValue(number), value), MMS_OK);
        CHECK(value.View().value.signedValue == number);
        CHECK(Roundtrips(IntValue(number)));
    }
    const uint64_t unsigneds[] = {uint64_t(0), uint64_t(1), uint64_t(42), std::numeric_limits<uint64_t>::max()};
    for (uint64_t number : unsigneds) {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(UintValue(number), value), MMS_OK);
        CHECK(value.View().value.unsignedValue == number);
        CHECK(Roundtrips(UintValue(number)));
    }
    const double numbers[] = {
        0.0, -0.0, 0.1, -1.5, 1e300,
        std::numeric_limits<double>::min(), std::numeric_limits<double>::max()};
    for (double number : numbers) {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(NumberValue(number), value), MMS_OK);
        CHECK(value.View().value.number == number);
        CHECK(std::signbit(value.View().value.number) == std::signbit(number));
        CHECK(Roundtrips(NumberValue(number)));
    }
    const double vectorSamples[4][4] = {
        {1.0, 0.0, 0.0, 0.0},
        {1.0, -2.5, 0.0, 0.0},
        {0.0, 0.0, 0.0, 0.0},
        {1e300, -1e-300, 0.25, -0.75}};
    for (uint32_t components = 1; components <= 4; ++components) {
        MmsValue source = VectorValue({});
        source.components = components;
        for (uint32_t i = 0; i < components; ++i) source.value.vector[i] = vectorSamples[components - 1][i];
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(source, value), MMS_OK);
        const MmsValue view = value.View();
        CHECK(view.type == MMS_VALUE_VECTOR);
        CHECK(view.components == components);
        for (uint32_t i = 0; i < components; ++i) CHECK(view.value.vector[i] == source.value.vector[i]);
        CHECK(Roundtrips(source));
    }
    const char unicode[] = "\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x98\x80";
    const char* texts[] = {"", "plain", "  padded  ", unicode, "line\nbreak\ttab"};
    for (const char* text : texts) {
        const std::string expected(text);
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(expected.data(), expected.size()), value), MMS_OK);
        const MmsValue view = value.View();
        CHECK(view.type == MMS_VALUE_TEXT);
        CHECK(view.value.text.bytes == expected.size());
        CHECK(TextOf(view) == expected);
        CHECK(Roundtrips(TextValue(expected.data(), expected.size())));
    }
    const MmsId ids[] = {
        MmsId{0, 0},
        MmsId{0x8899aabbccddeeffull, 0x0011223344556677ull},
        MmsId{~uint64_t(0), ~uint64_t(0)}};
    for (const MmsId& id : ids) {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(IdValue(id.hi, id.lo), value), MMS_OK);
        CHECK(value.View().value.id.lo == id.lo);
        CHECK(value.View().value.id.hi == id.hi);
        CHECK(Roundtrips(IdValue(id.hi, id.lo)));
    }
    {
        const unsigned char sample[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(BytesValue(sample, sizeof(sample)), value), MMS_OK);
        const MmsValue view = value.View();
        CHECK(view.type == MMS_VALUE_BYTES);
        CHECK(view.value.bytes.bytes == sizeof(sample));
        CHECK(std::memcmp(view.value.bytes.data, sample, sizeof(sample)) == 0);
        CHECK(Roundtrips(BytesValue(sample, sizeof(sample))));
        UiPropertyValue empty;
        CHECK_RESULT(UiPropertyValue::Copy(BytesValue(nullptr, 0), empty), MMS_OK);
        CHECK(empty.View().value.bytes.bytes == 0);
    }
    {
        // Non-vector types keep the caller's components instead of forcing zero.
        MmsValue source = TextValue("x", 1);
        source.components = 3;
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(source, value), MMS_OK);
        CHECK(value.View().components == 3);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(Roundtrips(source));
    }
}

void FormatChecks() {
    {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(BoolValue(1), value), MMS_OK);
        std::vector<std::string> fields{"stale", "older"};
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == "1");
        CHECK_RESULT(UiPropertyValue::Copy(BoolValue(0), value), MMS_OK);
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == "0");
    }
    {
        const int64_t numbers[] = {int64_t(0), int64_t(-42), std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
        const char* expected[] = {"0", "-42", "-9223372036854775808", "9223372036854775807"};
        for (size_t i = 0; i < 4; ++i) {
            UiPropertyValue value;
            CHECK_RESULT(UiPropertyValue::Copy(IntValue(numbers[i]), value), MMS_OK);
            std::vector<std::string> fields;
            CHECK_RESULT(value.Format(fields), MMS_OK);
            CHECK(fields.size() == 1);
            CHECK(fields[0] == expected[i]);
        }
    }
    {
        const uint64_t numbers[] = {uint64_t(0), uint64_t(42), std::numeric_limits<uint64_t>::max()};
        const char* expected[] = {"0", "42", "18446744073709551615"};
        for (size_t i = 0; i < 3; ++i) {
            UiPropertyValue value;
            CHECK_RESULT(UiPropertyValue::Copy(UintValue(numbers[i]), value), MMS_OK);
            std::vector<std::string> fields;
            CHECK_RESULT(value.Format(fields), MMS_OK);
            CHECK(fields.size() == 1);
            CHECK(fields[0] == expected[i]);
        }
    }
    {
        // Floating point text must parse back to the identical double.
        const double numbers[] = {0.1, -1.5, 1e300, std::numeric_limits<double>::min(), std::numeric_limits<double>::max()};
        for (double number : numbers) {
            UiPropertyValue first;
            CHECK_RESULT(UiPropertyValue::Copy(NumberValue(number), first), MMS_OK);
            std::vector<std::string> fields;
            CHECK_RESULT(first.Format(fields), MMS_OK);
            CHECK(fields.size() == 1);
            UiPropertyValue second;
            CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_NUMBER, 0, Views(fields), second), MMS_OK);
            CHECK(first.Equals(second));
        }
    }
    {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(VectorValue({1.25, -0.5, 4.0}), value), MMS_OK);
        std::vector<std::string> fields{"a", "b", "c", "d", "e"};
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 3);
        UiPropertyValue parsed;
        CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_VECTOR, 3, Views(fields), parsed), MMS_OK);
        CHECK(parsed.Equals(value));
    }
    {
        const char unicode[] = "\xE4\xB8\xAD\xE6\x96\x87";
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(unicode, sizeof(unicode) - 1), value), MMS_OK);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == unicode);
    }
    {
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(IdValue(0x0011223344556677ull, 0x8899aabbccddeeffull), value), MMS_OK);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == "00112233445566778899aabbccddeeff");
    }
    {
        const unsigned char sample[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
        UiPropertyValue value;
        CHECK_RESULT(UiPropertyValue::Copy(BytesValue(sample, sizeof(sample)), value), MMS_OK);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == "00017f80ff");
    }
}

void ParseChecks() {
    {
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_BOOL, 0, {"1"}));
        CHECK(value.View().value.unsignedValue == 1);
        CHECK(ParseInto(value, MMS_VALUE_BOOL, 0, {"0"}));
        CHECK(value.View().value.unsignedValue == 0);
    }
    {
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_INT, 0, {"-9223372036854775808"}));
        CHECK(value.View().value.signedValue == std::numeric_limits<int64_t>::min());
        CHECK(ParseInto(value, MMS_VALUE_INT, 0, {"9223372036854775807"}));
        CHECK(value.View().value.signedValue == std::numeric_limits<int64_t>::max());
        CHECK(ParseInto(value, MMS_VALUE_INT, 0, {"-42"}));
        CHECK(value.View().value.signedValue == -42);
        CHECK(ParseInto(value, MMS_VALUE_INT, 0, {"0"}));
        CHECK(value.View().value.signedValue == 0);
    }
    {
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_UINT, 0, {"18446744073709551615"}));
        CHECK(value.View().value.unsignedValue == std::numeric_limits<uint64_t>::max());
        CHECK(ParseInto(value, MMS_VALUE_UINT, 0, {"0"}));
        CHECK(value.View().value.unsignedValue == 0);
    }
    {
        struct Number { const char* text; double value; };
        const Number numbers[] = {
            {"0", 0.0}, {"-0", -0.0}, {"1", 1.0}, {"-1.5", -1.5},
            {"1e3", 1000.0}, {"1E-3", 0.001}, {"0.1", 0.1},
            {".5", 0.5}, {"-.5", -0.5}, {"1.", 1.0}, {"-1.", -1.0},
            {"1.e2", 100.0}, {"-.5e1", -5.0}, {"1.E-2", 0.01}, {".5e-3", 0.0005},
            {"2.2250738585072014e-308", std::numeric_limits<double>::min()},
            {"1.7976931348623157e308", std::numeric_limits<double>::max()}};
        for (const Number& number : numbers) {
            UiPropertyValue value;
            CHECK(ParseInto(value, MMS_VALUE_NUMBER, 0, {number.text}));
            const MmsValue view = value.View();
            CHECK(view.value.number == number.value);
            CHECK(std::signbit(view.value.number) == std::signbit(number.value));
        }
    }
    {
        // Native numeric input may omit either side of the decimal point; those
        // forms must survive Format -> Parse exactly.
        const char* forms[] = {
            ".5", "-.5", "1.", "-1.", "1.e2", "-1.E-2", ".5e1",
            "0.", "-0.", ".125", "1.e-2", "-.25e+1"};
        for (const char* form : forms) {
            UiPropertyValue first;
            CHECK(ParseInto(first, MMS_VALUE_NUMBER, 0, {form}));
            std::vector<std::string> fields;
            CHECK_RESULT(first.Format(fields), MMS_OK);
            CHECK(fields.size() == 1);
            UiPropertyValue second;
            CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_NUMBER, 0, Views(fields), second), MMS_OK);
            CHECK(first.Equals(second));
        }
    }
    {
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_VECTOR, 2, {"1.5", "-2"}));
        const MmsValue view = value.View();
        CHECK(view.type == MMS_VALUE_VECTOR);
        CHECK(view.components == 2);
        CHECK(view.value.vector[0] == 1.5);
        CHECK(view.value.vector[1] == -2.0);
        CHECK(ParseInto(value, MMS_VALUE_VECTOR, 4, {"0", "1e-3", "-4.5", "6E2"}));
        CHECK(value.View().components == 4);
        CHECK(value.View().value.vector[3] == 600.0);
        CHECK(ParseInto(value, MMS_VALUE_VECTOR, 3, {".5", "1.", "-.25e2"}));
        const MmsValue formsView = value.View();
        CHECK(formsView.components == 3);
        CHECK(formsView.value.vector[0] == 0.5);
        CHECK(formsView.value.vector[1] == 1.0);
        CHECK(formsView.value.vector[2] == -25.0);
    }
    {
        const char unicode[] = "\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x98\x80";
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_TEXT, 0, {"  keep  "}));
        CHECK(TextOf(value.View()) == "  keep  ");
        CHECK(ParseInto(value, MMS_VALUE_TEXT, 0, {unicode}));
        CHECK(TextOf(value.View()) == unicode);
        CHECK(ParseInto(value, MMS_VALUE_TEXT, 0, {""}));
        CHECK(TextOf(value.View()).empty());
    }
    {
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_ID, 0, {"00112233445566778899AABBCCDDEEFF"}));
        const MmsValue view = value.View();
        CHECK(view.value.id.hi == 0x0011223344556677ull);
        CHECK(view.value.id.lo == 0x8899aabbccddeeffull);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == "00112233445566778899aabbccddeeff");
    }
    {
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_BYTES, 0, {"00FFab"}));
        const MmsValue view = value.View();
        CHECK(view.value.bytes.bytes == 3);
        CHECK(static_cast<const unsigned char*>(view.value.bytes.data)[0] == 0x00);
        CHECK(static_cast<const unsigned char*>(view.value.bytes.data)[1] == 0xff);
        CHECK(static_cast<const unsigned char*>(view.value.bytes.data)[2] == 0xab);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == "00ffab");
        CHECK(ParseInto(value, MMS_VALUE_BYTES, 0, {""}));
        CHECK(value.View().value.bytes.bytes == 0);
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0].empty());
    }
    {
        // Non-vector components are preserved, never silently rewritten.
        UiPropertyValue value;
        CHECK(ParseInto(value, MMS_VALUE_NUMBER, 3, {"1.25"}));
        CHECK(value.View().components == 3);
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(ParseInto(value, MMS_VALUE_TEXT, 2, {"x"}));
        CHECK(value.View().components == 2);
        CHECK(ParseInto(value, MMS_VALUE_ID, 4, {"00000000000000000000000000000000"}));
        CHECK(value.View().components == 4);
    }
}

void CopyRejectChecks() {
    UiPropertyValue output;
    CHECK_RESULT(UiPropertyValue::Copy(EmptyValue(), output), MMS_E_ARGUMENT);
    {
        MmsValue source = TextValue("x", 1);
        source.type = 9;
        CHECK_RESULT(UiPropertyValue::Copy(source, output), MMS_E_ARGUMENT);
    }
    {
        MmsValue source = TextValue("x", 1);
        source.components = 5;
        CHECK_RESULT(UiPropertyValue::Copy(source, output), MMS_E_ARGUMENT);
    }
    CHECK_RESULT(UiPropertyValue::Copy(VectorValue({}), output), MMS_E_ARGUMENT);
    {
        MmsValue source = VectorValue({1.0, 2.0});
        source.components = 5;
        CHECK_RESULT(UiPropertyValue::Copy(source, output), MMS_E_ARGUMENT);
    }
    CHECK_RESULT(UiPropertyValue::Copy(BoolValue(2), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(NumberValue(std::numeric_limits<double>::quiet_NaN()), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(NumberValue(std::numeric_limits<double>::infinity()), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(NumberValue(-std::numeric_limits<double>::infinity()), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(VectorValue({1.0, std::numeric_limits<double>::quiet_NaN()}), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(VectorValue({std::numeric_limits<double>::infinity(), 2.0}), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(TextValue(nullptr, 4), output), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Copy(BytesValue(nullptr, 4), output), MMS_E_ARGUMENT);
    {
        const char invalidUtf8[] = "\xC3\x28";
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(invalidUtf8, sizeof(invalidUtf8) - 1), output), MMS_E_ARGUMENT);
        const char withNul[] = {'a', '\0', 'b'};
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(withNul, sizeof(withNul)), output), MMS_E_ARGUMENT);
    }
    CHECK_RESULT(UiPropertyValue::Copy(TextValue(nullptr, 0), output), MMS_OK);
    CHECK(output.View().value.text.bytes == 0);
    CHECK_RESULT(UiPropertyValue::Copy(BytesValue(nullptr, 0), output), MMS_OK);
    CHECK(output.View().value.bytes.bytes == 0);
}

void ParseRejectChecks() {
    CHECK(ExpectArgument(MMS_VALUE_EMPTY, 0, {""}));
    CHECK(ExpectArgument(9, 0, {""}));
    CHECK(ExpectArgument(MMS_VALUE_TEXT, 5, {"x"}));
    CHECK(ExpectArgument(MMS_VALUE_VECTOR, 5, {"1", "2", "3", "4", "5"}));
    CHECK(ExpectArgument(MMS_VALUE_VECTOR, 0, {}));
    {
        const char* invalid[] = {"", "2", "-0", "00", "true", "01", " 1", "1 "};
        for (const char* field : invalid) CHECK(ExpectArgument(MMS_VALUE_BOOL, 0, {field}));
    }
    {
        const char* invalid[] = {
            "", "+1", " 1", "1 ", "0x10", "1.0", "1e3", "--1", "-",
            "9223372036854775808", "-9223372036854775809", "1a", "1 2"};
        for (const char* field : invalid) CHECK(ExpectArgument(MMS_VALUE_INT, 0, {field}));
    }
    {
        const char* invalid[] = {
            "", "-1", "+1", " 1", "1 ", "0x10", "1.0",
            "18446744073709551616", "1a"};
        for (const char* field : invalid) CHECK(ExpectArgument(MMS_VALUE_UINT, 0, {field}));
    }
    {
        const char* invalid[] = {
            "", " 1", "1 ", "\t1", "1\n", "+1", "+0", "+.5", "+1.",
            "1e", "1e+", "1e-", "1.2.3", "0x10", "0x1p3",
            "nan", "NaN", "inf", "-inf", "infinity", "1,5", "1f", "1_000",
            "1e-400", "1e400", "-1e400", "1e999999", "--1", "-", ".", "-.",
            "1..2", "1e1.5", ".e2", "-.e2", "..5", ".5.5",
            "1.e", "1.e+", ".5e", "1.e-400", ".5e-400", "e5", "-e5", "1.a"};
        for (const char* field : invalid) CHECK(ExpectArgument(MMS_VALUE_NUMBER, 0, {field}));
    }
    {
        CHECK(ExpectArgument(MMS_VALUE_VECTOR, 2, {"1"}));
        CHECK(ExpectArgument(MMS_VALUE_VECTOR, 2, {"1", "2", "3"}));
        CHECK(ExpectArgument(MMS_VALUE_VECTOR, 2, {"1", "nan"}));
        CHECK(ExpectArgument(MMS_VALUE_VECTOR, 2, {"1", "1e400"}));
        CHECK(ExpectArgument(MMS_VALUE_NUMBER, 0, {"1", "2"}));
        CHECK(ExpectArgument(MMS_VALUE_TEXT, 0, {}));
        CHECK(ExpectArgument(MMS_VALUE_BOOL, 0, {"1", "0"}));
    }
    {
        // TEXT must be strict UTF-8 without embedded NUL, never truncated.
        const char overlong[] = "\xC0\xAF";
        const char surrogate[] = "\xED\xA0\x80";
        const char truncated[] = "\xE2\x82";
        const char loneContinuation[] = "\x80";
        const char fiveByte[] = "\xF8\x88\x80\x80\x80";
        const char withNul[] = {'a', '\0', 'b'};
        CHECK(ExpectArgument(MMS_VALUE_TEXT, 0, {overlong}));
        CHECK(ExpectArgument(MMS_VALUE_TEXT, 0, {surrogate}));
        CHECK(ExpectArgument(MMS_VALUE_TEXT, 0, {truncated}));
        CHECK(ExpectArgument(MMS_VALUE_TEXT, 0, {loneContinuation}));
        CHECK(ExpectArgument(MMS_VALUE_TEXT, 0, {fiveByte}));
        const std::vector<std::string_view> withNulField{std::string_view(withNul, sizeof(withNul))};
        CHECK(ParseFields(MMS_VALUE_TEXT, 0, withNulField) == MMS_E_ARGUMENT);
    }
    {
        const char* invalid[] = {
            "", "00112233445566778899aabbccddeef",
            "00112233445566778899aabbccddeeff0",
            "00112233445566778899aabbccddeefg",
            "0x112233445566778899aabbccddeeff",
            "00112233 44556677 8899aabb ccddeeff",
            "00112233445566778899AABBCCDDEEFF "};
        for (const char* field : invalid) CHECK(ExpectArgument(MMS_VALUE_ID, 0, {field}));
    }
    {
        const char* invalid[] = {"0", "abc", "0x12", "00 11", "zz", "0g", "00-11"};
        for (const char* field : invalid) CHECK(ExpectArgument(MMS_VALUE_BYTES, 0, {field}));
    }
}

void EqualsChecks() {
    {
        UiPropertyValue zero;
        UiPropertyValue negativeZero;
        CHECK_RESULT(UiPropertyValue::Copy(NumberValue(0.0), zero), MMS_OK);
        CHECK_RESULT(UiPropertyValue::Copy(NumberValue(-0.0), negativeZero), MMS_OK);
        CHECK(zero.Equals(negativeZero));
        CHECK(negativeZero.Equals(zero));
    }
    CHECK(SameValues(BoolValue(1), BoolValue(1)));
    CHECK(!SameValues(BoolValue(1), BoolValue(0)));
    CHECK(SameValues(IntValue(std::numeric_limits<int64_t>::min()), IntValue(std::numeric_limits<int64_t>::min())));
    CHECK(!SameValues(IntValue(1), IntValue(2)));
    CHECK(SameValues(UintValue(std::numeric_limits<uint64_t>::max()), UintValue(std::numeric_limits<uint64_t>::max())));
    CHECK(!SameValues(UintValue(1), UintValue(2)));
    CHECK(SameValues(NumberValue(0.0), NumberValue(-0.0)));
    CHECK(!SameValues(NumberValue(1.0), NumberValue(2.0)));
    CHECK(SameValues(VectorValue({1.0, -2.0, 3.0}), VectorValue({1.0, -2.0, 3.0})));
    CHECK(!SameValues(VectorValue({1.0, -2.0, 3.0}), VectorValue({1.0, -2.0, 4.0})));
    CHECK(SameValues(TextValue("x", 1), TextValue("x", 1)));
    CHECK(!SameValues(TextValue("x", 1), TextValue("y", 1)));
    CHECK(SameValues(IdValue(0x0011223344556677ull, 0x8899aabbccddeeffull),
        IdValue(0x0011223344556677ull, 0x8899aabbccddeeffull)));
    CHECK(!SameValues(IdValue(0x0011223344556677ull, 0x8899aabbccddeeffull),
        IdValue(0x8899aabbccddeeffull, 0x0011223344556677ull)));
    CHECK(SameValues(BytesValue("ab", 2), BytesValue("ab", 2)));
    CHECK(!SameValues(BytesValue("ab", 2), BytesValue("ac", 2)));
    CHECK(!SameValues(TextValue("x", 1), IntValue(1)));
    {
        // Components participate in equality; union padding never does.
        MmsValue plain = TextValue("x", 1);
        MmsValue otherComponents = TextValue("x", 1);
        otherComponents.components = 1;
        CHECK(!SameValues(plain, otherComponents));
        MmsValue padded = BoolValue(1);
        padded.value.vector[3] = 12.5;
        MmsValue clean = BoolValue(1);
        CHECK(SameValues(padded, clean));
    }
    {
        UiPropertyValue empty;
        CHECK(empty.Equals(UiPropertyValue()));
    }
}

void OwnershipChecks() {
    const std::string longText = "0123456789abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    CHECK(longText.size() > 32);
    UiPropertyValue value;
    CHECK_RESULT(UiPropertyValue::Copy(TextValue(longText.data(), longText.size()), value), MMS_OK);
    {
        const MmsValue borrowed = value.View();
        CHECK(TextOf(borrowed) == longText);
        UiPropertyValue alias;
        CHECK_RESULT(UiPropertyValue::Copy(borrowed, alias), MMS_OK);
        CHECK(alias.Equals(value));
        // The source view points into the destination's own backing.
        CHECK_RESULT(UiPropertyValue::Copy(borrowed, value), MMS_OK);
        CHECK(value.Equals(alias));
        std::vector<std::string> fields;
        CHECK_RESULT(value.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0] == longText);
    }
    {
        UiPropertyValue copy = value;
        const MmsValue copyView = copy.View();
        value = UiPropertyValue{};
        CHECK(TextOf(copyView) == longText);
        CHECK(value.View().type == MMS_VALUE_EMPTY);
    }
    {
        UiPropertyValue source;
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(longText.data(), longText.size()), source), MMS_OK);
        UiPropertyValue moved = std::move(source);
        CHECK(TextOf(moved.View()) == longText);
        CHECK(source.View().type == MMS_VALUE_EMPTY);
        UiPropertyValue assigned;
        CHECK_RESULT(UiPropertyValue::Copy(BoolValue(0), assigned), MMS_OK);
        assigned = std::move(moved);
        CHECK(TextOf(assigned.View()) == longText);
        CHECK(moved.View().type == MMS_VALUE_EMPTY);
    }
    {
        const std::string longBytes(300, static_cast<char>(0x5a));
        UiPropertyValue bytes;
        CHECK_RESULT(UiPropertyValue::Copy(BytesValue(longBytes.data(), longBytes.size()), bytes), MMS_OK);
        UiPropertyValue copy = bytes;
        const MmsValue copyView = copy.View();
        bytes = UiPropertyValue{};
        CHECK(BytesOf(copyView) == longBytes);
    }
    {
        UiPropertyValue first;
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(longText.data(), longText.size()), first), MMS_OK);
        UiPropertyValue second;
        CHECK_RESULT(UiPropertyValue::Copy(first.View(), second), MMS_OK);
        CHECK(first.View().value.text.data != second.View().value.text.data);
        CHECK(second.Equals(first));
    }
}

void FailureKeepsDestinationChecks() {
    const char keepText[] = "\xE4\xB8\xAD";
    UiPropertyValue output;
    CHECK_RESULT(UiPropertyValue::Copy(TextValue(keepText, sizeof(keepText) - 1), output), MMS_OK);
    const UiPropertyValue before = output;
    std::vector<std::string> beforeFields;
    CHECK_RESULT(output.Format(beforeFields), MMS_OK);

    CHECK_RESULT(UiPropertyValue::Copy(EmptyValue(), output), MMS_E_ARGUMENT);
    CHECK(output.Equals(before));
    const MmsValue badBool = BoolValue(2);
    CHECK_RESULT(UiPropertyValue::Copy(badBool, output), MMS_E_ARGUMENT);
    CHECK(output.Equals(before));
    const char invalidUtf8[] = "\xC3\x28";
    CHECK_RESULT(UiPropertyValue::Copy(TextValue(invalidUtf8, sizeof(invalidUtf8) - 1), output), MMS_E_ARGUMENT);
    CHECK(output.Equals(before));
    // Long enough to require heap storage; short strings live in SSO and
    // would never reach the failing allocation.
    const std::string longAllocText(64, 'a');
    g_failAllocations = true;
    const MmsResult memoryCopy = UiPropertyValue::Copy(TextValue(longAllocText.data(), longAllocText.size()), output);
    g_failAllocations = false;
    CHECK_RESULT(memoryCopy, MMS_E_MEMORY);
    CHECK(output.Equals(before));

    UiPropertyValue empty;
    std::vector<std::string> fields{"left", "right"};
    CHECK_RESULT(empty.Format(fields), MMS_E_ARGUMENT);
    CHECK(fields.size() == 2);
    CHECK(fields[0] == "left");
    CHECK(fields[1] == "right");
    const std::vector<std::string> savedFields = fields;
    g_failAllocations = true;
    const MmsResult memoryFormat = before.Format(fields);
    g_failAllocations = false;
    CHECK_RESULT(memoryFormat, MMS_E_MEMORY);
    CHECK(fields == savedFields);

    CHECK(ParseInto(output, MMS_VALUE_BOOL, 0, {"1"}));
    const UiPropertyValue parseBefore = output;
    const std::vector<std::string_view> invalidFields{"not-a-bool"};
    CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_BOOL, 0, invalidFields, output), MMS_E_ARGUMENT);
    CHECK(output.Equals(parseBefore));
    const std::vector<std::string_view> textFields{std::string_view(longAllocText)};
    const UiPropertyValue parseSaved = output;
    g_failAllocations = true;
    const MmsResult parseMemory = UiPropertyValue::Parse(MMS_VALUE_TEXT, 0, textFields, output);
    g_failAllocations = false;
    CHECK_RESULT(parseMemory, MMS_E_MEMORY);
    CHECK(output.Equals(parseSaved));
}

void LimitChecks() {
    const size_t maxTextBytes = 1024 * 1024;
    const std::string maxText(maxTextBytes, 'x');
    UiPropertyValue destination;
    CHECK_RESULT(UiPropertyValue::Copy(TextValue(maxText.data(), maxText.size()), destination), MMS_OK);
    {
        const MmsValue view = destination.View();
        CHECK(view.value.text.bytes == maxText.size());
        CHECK(TextOf(view) == maxText);
        std::vector<std::string> fields;
        CHECK_RESULT(destination.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0].size() == maxTextBytes);
        CHECK(fields[0] == maxText);
        UiPropertyValue reparsed;
        CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_TEXT, 0, Views(fields), reparsed), MMS_OK);
        CHECK(reparsed.Equals(destination));
    }
    const std::string oversizeText(maxTextBytes + 1, 'x');
    const std::vector<std::string_view> oversizeTextView{std::string_view(oversizeText)};
    const UiPropertyValue limitSaved = destination;
    g_failAllocations = true;
    const MmsResult oversizeTextCopy = UiPropertyValue::Copy(TextValue(oversizeText.data(), oversizeText.size()), destination);
    const MmsResult oversizeTextParse = UiPropertyValue::Parse(MMS_VALUE_TEXT, 0, oversizeTextView, destination);
    const MmsResult nullTextCopy = UiPropertyValue::Copy(TextValue(nullptr, 4), destination);
    const MmsResult nullBytesCopy = UiPropertyValue::Copy(BytesValue(nullptr, 4), destination);
    g_failAllocations = false;
    CHECK_RESULT(oversizeTextCopy, MMS_E_ARGUMENT);
    CHECK_RESULT(oversizeTextParse, MMS_E_ARGUMENT);
    CHECK_RESULT(nullTextCopy, MMS_E_ARGUMENT);
    CHECK_RESULT(nullBytesCopy, MMS_E_ARGUMENT);
    CHECK(destination.Equals(limitSaved));

    {
        UiPropertyValue zeroLength;
        CHECK_RESULT(UiPropertyValue::Copy(TextValue(nullptr, 0), zeroLength), MMS_OK);
        CHECK(zeroLength.View().value.text.bytes == 0);
        CHECK_RESULT(UiPropertyValue::Copy(BytesValue(nullptr, 0), zeroLength), MMS_OK);
        CHECK(zeroLength.View().value.bytes.bytes == 0);
    }

    const size_t maxBytesBytes = 16 * 1024 * 1024;
    const std::string bytesData(1024 * 1024, static_cast<char>(0x5a));
    UiPropertyValue bytesDestination;
    CHECK_RESULT(UiPropertyValue::Copy(BytesValue(bytesData.data(), bytesData.size()), bytesDestination), MMS_OK);
    {
        std::vector<std::string> fields;
        CHECK_RESULT(bytesDestination.Format(fields), MMS_OK);
        CHECK(fields.size() == 1);
        CHECK(fields[0].size() == bytesData.size() * 2);
        UiPropertyValue reparsed;
        CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_BYTES, 0, Views(fields), reparsed), MMS_OK);
        CHECK(reparsed.Equals(bytesDestination));
    }
    const std::string oversizeHex(maxBytesBytes * 2 + 2, '0');
    const std::vector<std::string_view> oversizeByteView{std::string_view(oversizeHex)};
    const UiPropertyValue bytesSaved = bytesDestination;
    g_failAllocations = true;
    const MmsResult oversizeBytesCopy = UiPropertyValue::Copy(
        BytesValue(oversizeHex.data(), maxBytesBytes + 1), bytesDestination);
    const MmsResult oversizeBytesParse = UiPropertyValue::Parse(MMS_VALUE_BYTES, 0, oversizeByteView, bytesDestination);
    g_failAllocations = false;
    CHECK_RESULT(oversizeBytesCopy, MMS_E_ARGUMENT);
    CHECK_RESULT(oversizeBytesParse, MMS_E_ARGUMENT);
    CHECK(bytesDestination.Equals(bytesSaved));
}

void EmptyChecks() {
    UiPropertyValue empty;
    const MmsValue view = empty.View();
    CHECK(view.type == MMS_VALUE_EMPTY);
    CHECK(view.components == 0);
    CHECK(empty.Equals(UiPropertyValue()));
    std::vector<std::string> fields{"keep"};
    CHECK_RESULT(empty.Format(fields), MMS_E_ARGUMENT);
    CHECK(fields.size() == 1);
    CHECK(fields[0] == "keep");
    CHECK_RESULT(UiPropertyValue::Copy(EmptyValue(), empty), MMS_E_ARGUMENT);
    CHECK_RESULT(UiPropertyValue::Parse(MMS_VALUE_EMPTY, 0, std::vector<std::string_view>{}, empty), MMS_E_ARGUMENT);
}

void Run() {
    CopyAndViewChecks();
    FormatChecks();
    ParseChecks();
    CopyRejectChecks();
    ParseRejectChecks();
    EqualsChecks();
    OwnershipChecks();
    FailureKeepsDestinationChecks();
    LimitChecks();
    EmptyChecks();
}
} // namespace

int main() {
    try {
        Run();
    } catch (const std::exception& error) {
        ++g_failures;
        std::fprintf(stderr, "FAIL unhandled exception: %s\n", error.what());
    } catch (...) {
        ++g_failures;
        std::fprintf(stderr, "FAIL unhandled non-standard exception\n");
    }
    std::printf("ui_property_value_tests: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
