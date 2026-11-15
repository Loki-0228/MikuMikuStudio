#include <mms/cpp/settings.hpp>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

using mms::sdk::SettingsServices;

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (false)

constexpr std::uint64_t Limit = MMS_SETTINGS_MAX_JSON_BYTES;
constexpr auto Huge = (std::numeric_limits<std::uint64_t>::max)();

struct Fake {
    struct Step {
        std::uint64_t capacity;
        MmsResult result;
        std::uint64_t reported = 0;
        std::string_view bytes{};
        int fault = 0;
    };
    std::optional<std::string> record{}, lastExpected{};
    std::string lastPatch;
    std::vector<Step> steps;
    std::size_t next = 0;
    unsigned queries = 0, reads = 0, merges = 0;
    bool shortHeader = false, wrongMajor = false, noRead = false, noMerge = false;
    int queryFault = 0, mergeFault = 0;
    MmsResult queryResult = MMS_OK, mergeResult = MMS_OK;
    SettingsServices* requeryInto = nullptr;
    Fake* replacement = nullptr;

    static void Fault(int code) {
        if (code == 1) throw std::bad_alloc{};
        if (code == 2) throw std::length_error("fixture");
        if (code == 3) throw std::runtime_error("fixture");
    }
    static MmsResult MMS_CALL Read(void* context, char* buffer, std::uint64_t capacity, std::uint64_t* length) {
        auto& self = *static_cast<Fake*>(context);
        ++self.reads;
        CHECK(length != nullptr && (buffer != nullptr) == (capacity != 0) && capacity <= Limit);
        if (!buffer && self.requeryInto) {
            CHECK(SettingsServices::Query(self.replacement->Host(), *self.requeryInto) == MMS_OK);
        }
        if (!self.steps.empty()) {
            CHECK(self.next < self.steps.size());
            if (self.next >= self.steps.size()) return MMS_E_FAILED;
            const auto& step = self.steps[self.next++];
            CHECK(capacity == step.capacity && step.bytes.size() <= capacity);
            *length = step.reported;
            if (!step.bytes.empty() && buffer && step.bytes.size() <= capacity) {
                // Some failures intentionally scribble into the scratch buffer.
                std::memcpy(buffer, step.bytes.data(), step.bytes.size());
            }
            Fault(step.fault);
            return step.result;
        }
        if (!self.record) return MMS_E_NOT_FOUND;
        *length = self.record->size();
        if (!buffer) return MMS_OK;
        if (capacity < *length) return MMS_E_MEMORY;
        std::memcpy(buffer, self.record->data(), self.record->size());
        return MMS_OK;
    }
    static MmsResult MMS_CALL Merge(void* context, MmsString expected, MmsString patch) {
        auto& self = *static_cast<Fake*>(context);
        ++self.merges;
        CHECK((expected.data != nullptr) == (expected.bytes != 0) && expected.bytes <= Limit);
        CHECK(patch.data && patch.bytes && patch.bytes <= Limit);
        if (expected.data) self.lastExpected = std::string(expected.data, static_cast<std::size_t>(expected.bytes));
        else self.lastExpected.reset();
        self.lastPatch.assign(patch.data, static_cast<std::size_t>(patch.bytes));
        Fault(self.mergeFault);
        if (self.mergeResult != MMS_OK) return self.mergeResult;
        // Exact-byte comparison only: this fixture checks forwarding, not JSON merge semantics.
        return self.lastExpected == self.record ? MMS_OK : MMS_E_CONFLICT;
    }
    static MmsResult MMS_CALL Query(void* context, MmsString name, std::uint32_t major, std::uint32_t minor,
                                    std::uint32_t size, void* out) {
        auto& self = *static_cast<Fake*>(context);
        ++self.queries;
        CHECK(name.data && name.bytes == 12 && std::memcmp(name.data, "mms.settings", 12) == 0);
        CHECK(major == 1 && minor == 0 && size == sizeof(MmsSettingsApiV1) && out);
        const MmsSettingsApiV1 api{
            {self.shortHeader ? 1u : static_cast<std::uint32_t>(sizeof(MmsSettingsApiV1)),
             static_cast<std::uint16_t>(self.wrongMajor ? 2 : 1), 0},
            context, self.noRead ? nullptr : Read, self.noMerge ? nullptr : Merge};
        *static_cast<MmsSettingsApiV1*>(out) = api;
        Fault(self.queryFault);
        return self.queryResult;
    }
    MmsHostV1 Host() {
        MmsHostV1 host{};
        host.h = {sizeof(host), 1, 0}; host.context = this; host.queryService = Query;
        return host;
    }
};

void QueryAndThread() {
    Fake available, other;
    SettingsServices settings;
    std::string out = "sentinel";
    CHECK(!settings.ready() && settings.Read(out) == MMS_E_NOT_READY && out == "sentinel");
    CHECK(settings.CompareAndMerge(std::nullopt, "{}") == MMS_E_NOT_READY);
    CHECK(SettingsServices::Query(MmsHostV1{}, settings) == MMS_E_VERSION && !settings.ready());
    auto host = available.Host();
    host.h.major = 2;
    CHECK(SettingsServices::Query(host, settings) == MMS_E_VERSION && available.queries == 0);
    host.h.major = 1; host.queryService = nullptr;
    CHECK(SettingsServices::Query(host, settings) == MMS_E_ARGUMENT && available.queries == 0);
    other.queryResult = MMS_E_NOT_FOUND;
    CHECK(SettingsServices::Query(other.Host(), settings) == MMS_E_NOT_FOUND && !settings.ready());
    CHECK(SettingsServices::Query(available.Host(), settings) == MMS_OK && settings.ready());
    const auto preserves = [&](MmsResult expected) {
        CHECK(SettingsServices::Query(other.Host(), settings) == expected && settings.ready() &&
              settings.api().context == &available);
    };
    preserves(MMS_E_NOT_FOUND);
    other.queryResult = MMS_OK;
    other.shortHeader = true; preserves(MMS_E_VERSION); other.shortHeader = false;
    other.wrongMajor = true; preserves(MMS_E_VERSION); other.wrongMajor = false;
    other.noRead = true; preserves(MMS_E_UNSUPPORTED); other.noRead = false;
    other.noMerge = true; preserves(MMS_E_UNSUPPORTED); other.noMerge = false;
    for (int fault : {1, 2, 3}) {
        other.queryFault = fault;
        preserves(fault == 3 ? MMS_E_FAILED : MMS_E_MEMORY);
    }
    MmsResult read = MMS_OK, merge = MMS_OK;
    std::thread worker([&] { read = settings.Read(out); merge = settings.CompareAndMerge(std::nullopt, "{}"); });
    worker.join();
    CHECK(read == MMS_E_THREAD && merge == MMS_E_THREAD && out == "sentinel");
    CHECK(available.reads == 0 && available.merges == 0);
}

void ReadCopies() {
    Fake host;
    SettingsServices settings;
    CHECK(SettingsServices::Query(host.Host(), settings) == MMS_OK);
    std::string out = "unchanged";
    CHECK(settings.Read(out) == MMS_E_NOT_FOUND && out == "unchanged" && host.reads == 1);
    host.record = "{}";
    CHECK(settings.Read(out) == MMS_OK && out == "{}" && host.reads == 3);
    host.record = u8"{\"label\":\"\u8a2d\u5b9a\"}";
    CHECK(out == "{}");
    CHECK(settings.Read(out) == MMS_OK && out == host.record && host.reads == 5);
    host.record = std::string("{\"s\":\"") + std::string(static_cast<std::size_t>(Limit) - 8, 'x') + "\"}";
    CHECK(host.record->size() == Limit && settings.Read(out) == MMS_OK && out == host.record);

    const auto script = [&](std::initializer_list<Fake::Step> steps, MmsResult expected,
                            std::string_view onSuccess = {}) {
        host.steps.assign(steps); host.next = 0;
        const std::string before = "sentinel";
        out = before;
        const auto reads = host.reads;
        CHECK(settings.Read(out) == expected);
        CHECK(host.next == steps.size() && host.reads == reads + steps.size());
        CHECK(out == (expected == MMS_OK ? onSuccess : before));
    };
    script({{0,MMS_OK,9}, {9,MMS_OK,2,"{}"}}, MMS_OK, "{}");
    script({{0,MMS_OK,2}, {2,MMS_E_MEMORY,7}, {7,MMS_OK,7,R"({"v":1})"}}, MMS_OK, R"({"v":1})");
    script({{0,MMS_OK,2}, {2,MMS_E_MEMORY,7}, {7,MMS_E_MEMORY,9}}, MMS_E_MEMORY);
    script({{0,MMS_OK,2}, {2,MMS_E_MEMORY,7}, {7,MMS_E_NOT_FOUND}}, MMS_E_NOT_FOUND);
    script({{0,MMS_E_BUSY,10}}, MMS_E_BUSY);
    script({{0,MMS_OK,2}, {2,MMS_E_NOT_FOUND}}, MMS_E_NOT_FOUND);
    script({{0,MMS_OK,2}, {2,MMS_E_FAILED,5,"!!"}}, MMS_E_FAILED);
    script({{0,MMS_OK,2}, {2,MMS_E_MEMORY,0}}, MMS_E_MEMORY);
    script({{0,MMS_OK,Huge}}, MMS_E_MEMORY);
    script({{0,MMS_OK,2}, {2,MMS_E_MEMORY,Huge}}, MMS_E_MEMORY);
    script({{0,MMS_OK,2}, {2,MMS_OK,Huge,"{}"}}, MMS_E_FAILED);
    script({{0,MMS_OK,0}}, MMS_E_FAILED);
    script({{0,MMS_OK,2}, {2,MMS_OK,1,"{"}}, MMS_E_FAILED);
    for (int fault : {1, 2, 3}) {
        const auto expected = fault == 3 ? MMS_E_FAILED : MMS_E_MEMORY;
        script({{0,MMS_OK,2,{},fault}}, expected);
        script({{0,MMS_OK,2}, {2,MMS_OK,2,"{}",fault}}, expected);
    }

    host.steps.clear(); host.record = "{}";
    Fake nextHost;
    nextHost.record = R"({"other":true})";
    host.requeryInto = &settings; host.replacement = &nextHost;
    CHECK(settings.Read(out) == MMS_OK && out == "{}" && settings.api().context == &nextHost && nextHost.reads == 0);
    CHECK(settings.Read(out) == MMS_OK && out == nextHost.record && nextHost.reads == 2);
}

void OptimisticMerge() {
    Fake host;
    SettingsServices settings;
    CHECK(SettingsServices::Query(host.Host(), settings) == MMS_OK);
    constexpr std::string_view update = R"({"preview":{"enabled":true}})";
    const std::string oversized(static_cast<std::size_t>(Limit) + 1, 'x');
    CHECK(settings.CompareAndMerge(std::nullopt, "") == MMS_E_ARGUMENT);
    CHECK(settings.CompareAndMerge(std::nullopt, oversized) == MMS_E_ARGUMENT);
    CHECK(settings.CompareAndMerge(std::string_view{}, update) == MMS_E_ARGUMENT);
    CHECK(settings.CompareAndMerge(std::string_view(oversized), update) == MMS_E_ARGUMENT);
    CHECK(host.merges == 0);
    CHECK(settings.CompareAndMerge(std::nullopt, update) == MMS_OK && !host.lastExpected && host.lastPatch == update);
    host.record = "{}";
    CHECK(settings.CompareAndMerge(std::nullopt, update) == MMS_E_CONFLICT);
    CHECK(settings.CompareAndMerge("{}", update) == MMS_OK && host.lastExpected == "{}" && host.lastPatch == update);
    host.record = u8"{\"\u672a\u77e5\" : null}";
    std::string baseline;
    CHECK(settings.Read(baseline) == MMS_OK && baseline == host.record);
    const auto reads = host.reads;
    CHECK(settings.CompareAndMerge(baseline, update) == MMS_OK && host.lastExpected == baseline);
    host.record = R"({"newer":true})";
    auto merges = host.merges + 1;
    CHECK(settings.CompareAndMerge(baseline, update) == MMS_E_CONFLICT && host.merges == merges && host.reads == reads);
    for (auto result : {MMS_E_BUSY, MMS_E_NOT_READY, MMS_E_FAILED, MMS_E_MEMORY, MMS_E_ARGUMENT}) {
        host.mergeResult = result;
        ++merges;
        CHECK(settings.CompareAndMerge(baseline, update) == result && host.merges == merges && host.reads == reads);
    }
    host.mergeResult = MMS_OK;
    for (int fault : {1, 2, 3}) {
        host.mergeFault = fault;
        CHECK(settings.CompareAndMerge(baseline, update) == (fault == 3 ? MMS_E_FAILED : MMS_E_MEMORY));
    }
    host.mergeFault = 0;
    host.record = std::string("{\"s\":\"") + std::string(static_cast<std::size_t>(Limit) - 8, 'x') + "\"}";
    CHECK(host.record->size() == Limit && settings.CompareAndMerge(*host.record, *host.record) == MMS_OK);
}
} // namespace

int main() {
    QueryAndThread(); ReadCopies(); OptimisticMerge();
    std::printf("settings_cpp_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
