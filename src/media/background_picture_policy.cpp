#include "mikumikustudio/background_picture_policy.hpp"
#include "../plugins/json.hpp"

namespace mikumikustudio::media {
system_settings::Result ReadBackgroundPictureSourceLimit(
    const system_settings::Store& store, std::uint64_t& output) noexcept {
    using system_settings::Result;
    try {
        std::string bytes;
        const auto read = store.ReadSection("resources", bytes);
        if (read == Result::Missing) { output = kDefaultPictureSourceLimit; return Result::Ok; }
        if (read != Result::Ok) return read;
        const auto section = plugins::Json::Parse(bytes);
        if (section.kind != plugins::Json::Object) return Result::Invalid;
        const auto value = section.object.find("backgroundPictureSourcesMiB");
        if (value == section.object.end()) { output = kDefaultPictureSourceLimit; return Result::Ok; }
        const auto mib = value->second.UInt();
        if (mib < 16 || mib > 4096) return Result::Invalid;
        output = std::uint64_t(mib) * 1024 * 1024;
        return Result::Ok;
    } catch (const std::bad_alloc&) { return Result::Io; }
    catch (...) { return Result::Invalid; }
}
system_settings::Result ReadBackgroundPictureSourceLimit(std::uint64_t& output) noexcept {
    try { return ReadBackgroundPictureSourceLimit(system_settings::Store(system_settings::Path()), output); }
    catch (...) { return system_settings::Result::Io; }
}
}
