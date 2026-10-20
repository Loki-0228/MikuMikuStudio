#pragma once
#include <mms/plugin.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mikumikustudio::plugins {

// Owns the backing storage of one UI property MmsValue.
//
// Copy and Parse validate into an internal temporary and only replace the
// destination after the whole value is accepted, so a failed call leaves the
// destination untouched. Source and destination may alias.
//
// View() returns a borrowed MmsValue whose pointers stay valid until this
// object is assigned or destroyed. The caller does not own that storage.
class UiPropertyValue {
public:
    UiPropertyValue() noexcept = default;
    UiPropertyValue(const UiPropertyValue&) = default;
    UiPropertyValue& operator=(const UiPropertyValue&) = default;
    UiPropertyValue(UiPropertyValue&& other) noexcept;
    UiPropertyValue& operator=(UiPropertyValue&& other) noexcept;
    ~UiPropertyValue() = default;

    static MmsResult Copy(const MmsValue& source, UiPropertyValue& output) noexcept;
    static MmsResult Parse(uint32_t type, uint32_t components,
        const std::vector<std::string_view>& fields, UiPropertyValue& output) noexcept;

    MmsValue View() const noexcept;
    MmsResult Format(std::vector<std::string>& fields) const noexcept;
    bool Equals(const UiPropertyValue& other) const noexcept;

private:
    void Reset() noexcept;

    uint32_t type_ = MMS_VALUE_EMPTY;
    uint32_t components_ = 0;
    int64_t signedValue_ = 0;
    uint64_t unsignedValue_ = 0;
    double number_ = 0.0;
    double vector_[4] = {};
    MmsId id_ = {};
    std::string text_;
    std::vector<unsigned char> bytes_;
};

}
