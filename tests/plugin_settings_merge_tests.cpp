#include "../src/app/plugin_settings_merge.hpp"
#include "../src/app/settings_json.hpp"
#include <iostream>

namespace {
using mikumikustudio::plugin_settings_merge::Result;
using mikumikustudio::settings_json::Json;
using mikumikustudio::settings_json::Serialize;

const std::string Empty = R"({"schemaVersion":1,"sections":{}})";
const std::string Document = R"({
    "schemaVersion":1, "futureRoot":{"enabled":true},
    "sections":{
        "pluginSettings":{
            "demo.palette":{"nested":{"mode":2,"future":{"tag":"keep"}},"a":1,"array":[1,2]},
            "demo.palette.child":{"newlyAdded":[true]}, "peer":{"changedSinceRead":900}
        },
        "sceneDefaults":{"unknown":{"keep":"host"}}
    }
})";
const std::string Baseline = R"({"array":[1,2],"a":1,"nested":{"future":{"tag":"keep"},"mode":2}})";

void Check(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

std::string Plan(Result expected, std::string_view document, std::string_view id, const std::string* baseline,
    std::string_view patch) {
    const auto root = Json::Parse(document);
    const auto original = Serialize(root);
    const auto update = Json::Parse(patch);
    const auto originalUpdate = Serialize(update);
    const auto parsedBaseline = baseline ? Json::Parse(*baseline) : Json{};
    const auto originalBaseline = Serialize(parsedBaseline);
    std::string output = "untouched";
    const auto actual = mikumikustudio::plugin_settings_merge::Plan(root, id, baseline ? &parsedBaseline : nullptr, update,
        [](const Json& value) { return Serialize(value); }, output);
    Check(actual == expected, "unexpected merge result");
    Check(Serialize(root) == original && Serialize(update) == originalUpdate && Serialize(parsedBaseline) == originalBaseline,
        "input tree changed");
    Check(expected == Result::Ready ? output.back() == '\n' : output == "untouched",
        "output mutated on an unsuccessful or unchanged plan");
    return output;
}

void IsolationAndConcurrentEdits() {
    const auto result = Plan(Result::Ready, Document, "demo.palette", &Baseline,
        R"({"nested":{"mode":4,"new":"yes"},"array":[3],"a":null,"peer":{"local":true},"a.b":"literal"})");
    const auto root = Json::Parse(result);
    const auto& sections = root.At("sections");
    const auto& plugins = sections.At("pluginSettings");
    const auto& mine = plugins.At("demo.palette");
    Check(mine.At("nested").At("future").At("tag").String() == "keep", "unknown nested key lost");
    Check(mine.At("nested").At("mode").UInt() == 4 && mine.At("nested").At("new").String() == "yes", "local patch lost");
    Check(mine.At("array").Items().size() == 1 && mine.At("array").Items().front().UInt() == 3, "arrays are not atomic");
    Check(mine.At("a").kind == Json::Null && mine.At("a.b").String() == "literal", "null or dotted key misinterpreted");
    Check(mine.At("peer").At("local").text == "true" && plugins.At("peer").At("changedSinceRead").UInt() == 900,
        "peer value exposed as a path or overwritten");
    Check(plugins.At("demo.palette.child").At("newlyAdded").Items().size() == 1, "prefix-related plugin was affected");
    Check(root.At("futureRoot").At("enabled").text == "true" &&
        sections.At("sceneDefaults").At("unknown").At("keep").String() == "host", "host fields lost");

    std::string stale = R"({"array":[1,2],"a":1,"nested":{"future":{"tag":"changed"},"mode":2}})";
    Plan(Result::Conflict, Document, "demo.palette", &stale, R"({"a":8})");
    std::string alternateNumber = R"({"array":[1,2],"a":1.0,"nested":{"future":{"tag":"keep"},"mode":2}})";
    Plan(Result::Conflict, Document, "demo.palette", &alternateNumber, R"({"a":8})");
    Plan(Result::Conflict, Document, "demo.palette", nullptr, R"({"a":8})");
    Plan(Result::Conflict, Document, "demo.palette-new", &Baseline, R"({})");
    Plan(Result::Unchanged, Document, "demo.palette", &Baseline, R"({"nested":{"mode":2},"a":1})");
    Plan(Result::Unchanged, Empty, "demo.palette", nullptr, R"({})");

    const std::string emptyObject = "{}";
    const auto withEmptyEntry = R"({"schemaVersion":1,"sections":{"pluginSettings":{"empty":{}}}})";
    Plan(Result::Conflict, withEmptyEntry, "empty", nullptr, R"({"a":8})");
    Plan(Result::Ready, withEmptyEntry, "empty", &emptyObject, R"({"a":8})");
}

void InvalidAndOpaqueTypes() {
    for (const auto id : {"", "mms", "mms.builtin", "MMS", "abc/D", "abc\\D", "with space", "\xc3\xa9"})
        Plan(Result::Invalid, Document, id, nullptr, R"({"a":8})");
    Plan(Result::Invalid, Empty, std::string(129, 'a'), nullptr, R"({"a":8})");
    Plan(Result::Ready, Empty, std::string(128, 'a'), nullptr, R"({"a":8})");
    Plan(Result::Ready, Empty, "mms-other", nullptr, R"({"a":8})");
    Plan(Result::Ready, Empty, "..", nullptr, R"({"a":8})"); // one literal key, never a path
    Plan(Result::Invalid, Document, std::string("x\0y", 3), nullptr, R"({"a":8})");

    for (const auto patch : {R"({"a":99,"nested":null})", R"({"a":99,"nested":3})", R"({"a":99,"nested":[]})",
         R"({"a":99,"nested":{"future":false}})", R"({"a":99,"nested":{"future":"text"}})", R"([])", R"(1)"})
        Plan(Result::Invalid, Document, "demo.palette", &Baseline, patch);
    Plan(Result::Ready, Document, "demo.palette", &Baseline, R"({"a":{"new":true}})");
    const std::string malformedBaseline = "[]";
    Plan(Result::Invalid, Empty, "demo.palette", &malformedBaseline, R"({"a":8})");

    for (const auto doc : {R"({})", R"({"schemaVersion":2,"sections":{}})", R"({"schemaVersion":1,"sections":[]})",
         R"({"schemaVersion":1,"sections":{"pluginSettings":[]}})",
         R"({"schemaVersion":1,"sections":{"pluginSettings":{"demo.palette":null}}})"})
        Plan(Result::Corrupt, doc, "demo.palette", nullptr, R"({"a":8})");
    const auto futurePeer = R"({"schemaVersion":1,"sections":{"pluginSettings":{"unknownPeer":{"extra":null},"future":5}}})";
    const auto planned = Plan(Result::Ready, futurePeer, "demo.palette", nullptr, R"({"a":8})");
    Check(Json::Parse(planned).At("sections").At("pluginSettings").At("future").UInt() == 5, "opaque peer entry removed");
}

std::string NumberArray(std::size_t count) {
    std::string result = R"({"a":[0)";
    for (std::size_t i = 1; i < count; ++i) result += ",0";
    return result + "]}";
}

void CombinedCodecBounds() {
    const std::string id = "test";
    auto nested = [](std::size_t count) {
        return R"({"a":)" + std::string(count, '[') + "0" + std::string(count, ']') + "}";
    };
    Plan(Result::Ready, Empty, id, nullptr, nested(44));
    Plan(Result::Limit, Empty, id, nullptr, nested(45));
    Plan(Result::Ready, Empty, id, nullptr, NumberArray(32762));
    Plan(Result::Limit, Empty, id, nullptr, NumberArray(32763));

    const auto maxString = 64 * 1024 - std::string(R"({"a":""})").size();
    auto padded = [](std::size_t count) { return R"({"a":")" + std::string(count, 'x') + "\"}"; };
    const auto ownLimit = Plan(Result::Ready, Empty, id, nullptr, padded(maxString));
    const auto fullOwn = padded(maxString);
    Plan(Result::Limit, Empty, id, nullptr, padded(maxString + 1));
    Plan(Result::Limit, ownLimit, id, &fullOwn, R"({"also":1})");

    auto withPeer = [](std::size_t count) {
        return R"({"schemaVersion":1,"sections":{"pluginSettings":{"opaque":")" +
            std::string(count, 'x') + "\"}}}";
    };
    const auto small = R"({"a":1})";
    const auto registrySize = Serialize(Json::Parse(Plan(Result::Ready, withPeer(0), id, nullptr, small))
        .At("sections").At("pluginSettings")).size();
    const auto peerLimit = 512 * 1024 - registrySize;
    Plan(Result::Ready, withPeer(peerLimit), id, nullptr, small);
    Plan(Result::Limit, withPeer(peerLimit + 1), id, nullptr, small);

    auto withHost = [](std::size_t count) {
        return R"({"schemaVersion":1,"otherRootField":")" + std::string(count, 'x') + "\",\"sections\":{}}";
    };
    const auto baseBytes = Plan(Result::Ready, withHost(0), id, nullptr, small).size();
    const auto hostLimit = 1024 * 1024 - baseBytes;
    Check(Plan(Result::Ready, withHost(hostLimit), id, nullptr, small).size() == 1024 * 1024,
        "exact document limit rejected");
    Plan(Result::Limit, withHost(hostLimit + 1), id, nullptr, small);
}
} // namespace

int main() {
    try {
        IsolationAndConcurrentEdits();
        InvalidAndOpaqueTypes();
        CombinedCodecBounds();
        std::cout << "plugin settings merge policy checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
