#include "sidecar_copy.hpp"
#include "json.hpp"
#include "project_storage.hpp"
#include "scene_digest.hpp"

namespace repair {
namespace {
namespace host = mikudancestudio::plugins;
constexpr std::size_t kSidecarLimit = 96 * 1024 * 1024;

std::string Hash(const Bytes& bytes) {
    host::Digest digest;
    digest.Add(bytes.data(), bytes.size());
    const auto value = digest.Finish();
    constexpr char digits[] = "0123456789abcdef";
    std::string hash;
    hash.reserve(value.size() * 2);
    for (auto byte : value) {
        hash += digits[byte >> 4];
        hash += digits[byte & 15];
    }
    return hash;
}

void String(std::string& out, const std::string& text) {
    constexpr char digits[] = "0123456789abcdef";
    out += '"';
    for (unsigned char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 32) {
                out += "\\u00";
                out += digits[ch >> 4];
                out += digits[ch & 15];
            } else out += static_cast<char>(ch);
        }
    }
    out += '"';
}

void Encode(std::string& out, const host::Json& value) {
    switch (value.kind) {
    case host::Json::Null: out += "null"; break;
    case host::Json::Boolean:
    case host::Json::Number:
        // Unknown numbers retain their validated spelling, without rounding.
        out += value.text;
        break;
    case host::Json::StringValue: String(out, value.text); break;
    case host::Json::Array: {
        out += '[';
        bool first = true;
        for (const auto& item : value.array) {
            if (!first) out += ',';
            first = false;
            Encode(out, item);
        }
        out += ']';
        break;
    }
    case host::Json::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.object) {
            if (!first) out += ',';
            first = false;
            String(out, key);
            out += ':';
            Encode(out, item);
        }
        out += '}';
        break;
    }
    }
    if (out.size() > kSidecarLimit) throw std::invalid_argument("plugin sidecar size");
}

bool SameJson(const host::Json& first, const host::Json& second) {
    if (first.kind != second.kind || first.text != second.text ||
        first.array.size() != second.array.size() || first.object.size() != second.object.size()) return false;
    for (std::size_t i = 0; i < first.array.size(); ++i)
        if (!SameJson(first.array[i], second.array[i])) return false;
    for (const auto& [key, value] : first.object) {
        const auto found = second.object.find(key);
        if (found == second.object.end() || !SameJson(value, found->second)) return false;
    }
    return true;
}
}

std::optional<Bytes> PrepareSidecarCopy(const std::filesystem::path& source,
                                      const Bytes& original, const Bytes& repaired) {
    const auto journal = std::filesystem::path(source.native() + L".mms-save.json");
    if (std::filesystem::exists(journal))
        throw std::runtime_error("The source has an unfinished MMS save. Open it in MMS to recover it before repairing.");
    const auto sidecar = host::PluginSidecar(source);
    if (!std::filesystem::exists(sidecar)) return std::nullopt;
    const auto bytes = host::ReadFile(sidecar, kSidecarLimit);
    // Reuse the host's schema, metadata and canonical-base64 validation.
    // ReadPluginProject would perform recovery writes on the source.
    const auto project = host::DecodeProject(bytes);
    if (project.mainHash != Hash(original))
        throw std::runtime_error("The source PMM and plugin sidecar do not match.");
    auto json = host::Json::Parse(bytes, kSidecarLimit);
    std::string saveId;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        saveId = host::StorageId();
        if (saveId != project.saveId) break;
    }
    if (saveId == project.saveId) throw std::runtime_error("Cannot create a new repair save ID.");
    const auto repairedHash = Hash(repaired);
    json.object.at("saveId").text = saveId;
    json.object.at("mainSha256").text = repairedHash;
    std::string encoded;
    encoded.reserve(bytes.size());
    Encode(encoded, json);
    const auto verified = host::DecodeProject(encoded);
    if (verified.projectId != project.projectId || verified.saveId != saveId ||
        verified.mainHash != repairedHash || verified.records.size() != project.records.size())
        throw std::runtime_error("Repaired plugin metadata verification failed.");
    for (const auto& [id, record] : project.records) {
        const auto found = verified.records.find(id);
        if (found == verified.records.end() || found->second.schema != record.schema ||
            found->second.bytes != record.bytes)
            throw std::runtime_error("Repaired plugin payload verification failed.");
    }
    if (!SameJson(json, host::Json::Parse(encoded, kSidecarLimit)))
        throw std::runtime_error("Repaired plugin extension fields verification failed.");
    return Bytes(encoded.begin(), encoded.end());
}
}
