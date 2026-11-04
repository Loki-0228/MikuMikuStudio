#include "material_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mme_re::mmhack {
namespace {

class ParseError final : public std::runtime_error {
public:
  explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};

class Reader {
public:
  explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}
  std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  std::size_t position() const noexcept { return offset_; }
  const std::uint8_t* read(std::uint64_t count) {
    if (count > remaining()) {
      throw ParseError("truncated model at byte " + std::to_string(offset_));
    }
    const auto* result = bytes_.data() + offset_;
    offset_ += static_cast<std::size_t>(count);
    return result;
  }
  void skip(std::uint64_t count) { static_cast<void>(read(count)); }
  std::uint8_t u8() { return *read(1); }
  std::uint16_t u16() {
    const auto* p = read(2);
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t{p[1]} << 8));
  }
  std::uint32_t u32() {
    const auto* p = read(4);
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) |
        (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
  }
  std::uint32_t count(const char* description) {
    const auto value = u32();
    if (value > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
      throw ParseError(std::string("negative ") + description + " at byte " + std::to_string(offset_ - 4));
    }
    return value;
  }
  std::int32_t index(std::uint8_t width) {
    if (width == 1) {
      const auto value = u8();
      return value < 128 ? value : static_cast<std::int32_t>(value) - 256;
    }
    if (width == 2) {
      const auto value = u16();
      return value < 32768 ? value : static_cast<std::int32_t>(value) - 65536;
    }
    const auto value = u32();
    std::int32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
  }
private:
  const std::vector<std::uint8_t>& bytes_;
  std::size_t offset_ = 0;
};

std::wstring decode(const std::uint8_t* bytes, std::size_t size, UINT codepage) {
  if (size == 0) return {};
  if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    throw ParseError("text is larger than the Windows conversion limit");
  }
  const auto* text = reinterpret_cast<const char*>(bytes);
  const DWORD flags = codepage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
  const int length = static_cast<int>(size);
  const int count = MultiByteToWideChar(codepage, flags, text, length, nullptr, 0);
  if (count == 0) throw ParseError("invalid model text encoding");
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(codepage, flags, text, length, result.data(), count) != count) {
    throw ParseError("model text conversion failed");
  }
  return result;
}

std::wstring pmx_text(Reader& reader, std::uint8_t encoding) {
  const auto length = reader.count("PMX text length");
  const auto* data = reader.read(length);
  if (encoding == 1) return decode(data, length, CP_UTF8);
  if ((length & 1U) != 0) throw ParseError("odd UTF-16LE PMX text length");
  std::wstring result(length / 2U, L'\0');
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<wchar_t>(data[i * 2] | (std::uint16_t{data[i * 2 + 1]} << 8));
  }
  return result;
}

void skip_pmx_text(Reader& reader) { reader.skip(reader.count("PMX text length")); }

void require_minimum_bytes(Reader& reader, std::uint32_t count,
    std::uint32_t minimum_record_size, const char* description) {
  if (std::uint64_t{count} * minimum_record_size > reader.remaining()) {
    throw ParseError(std::string("truncated ") + description + " table at byte " + std::to_string(reader.position()));
  }
}

std::wstring builtin_toon(std::uint8_t index) {
  if (index >= 10) return {};
  std::wstring result = L"toon00.bmp";
  const unsigned value = static_cast<unsigned>(index) + 1U;
  result[4] = static_cast<wchar_t>(L'0' + value / 10U);
  result[5] = static_cast<wchar_t>(L'0' + value % 10U);
  return result;
}

void parse_pmx(Reader& reader, MaterialTable& result) {
  const auto* header = reader.read(17);
  float version;
  std::memcpy(&version, header + 4, sizeof(version));
  if (!std::isfinite(version) || version >= 3.0f) throw ParseError("unsupported PMX version");
  const auto globals = header[8];
  const auto encoding = header[9];
  const auto additional_uv = header[10];
  if (globals < 8 || encoding > 1) throw ParseError("invalid PMX header globals");
  const auto vertex_index_width = header[11];
  const auto texture_index_width = header[12];
  const auto bone_index_width = header[14];
  for (std::size_t i = 11; i != 17; ++i) {
    if (header[i] != 1 && header[i] != 2 && header[i] != 4) {
      throw ParseError("invalid PMX index width");
    }
  }
  reader.skip(static_cast<std::uint64_t>(globals) - 8U);
  for (unsigned i = 0; i != 4; ++i) skip_pmx_text(reader);

  const auto vertex_count = reader.count("PMX vertex count");
  require_minimum_bytes(reader, vertex_count, 38, "PMX vertex");
  for (std::uint32_t i = 0; i < vertex_count; ++i) {
    reader.skip((std::uint64_t{additional_uv} + 2U) * 16U);
    switch (reader.u8()) {
    case 0: reader.skip(bone_index_width); break;
    case 1: reader.skip(std::uint64_t{bone_index_width} * 2U + 4U); break;
    case 2: reader.skip(std::uint64_t{bone_index_width} * 4U + 16U); break;
    case 3: reader.skip(std::uint64_t{bone_index_width} * 2U + 40U); break;
    default: throw ParseError("unsupported PMX vertex weight type (original parser supports BDEF1/BDEF2/BDEF4/SDEF)");
    }
    reader.skip(4);
  }
  const auto surface_count = reader.count("PMX surface count");
  reader.skip(std::uint64_t{surface_count} * vertex_index_width);
  const auto texture_count = reader.count("PMX texture count");
  require_minimum_bytes(reader, texture_count, 4, "PMX texture");
  std::vector<std::wstring> textures;
  textures.reserve(texture_count);
  for (std::uint32_t i = 0; i < texture_count; ++i) textures.push_back(pmx_text(reader, encoding));
  const auto material_count = reader.count("PMX material count");
  require_minimum_bytes(reader, material_count, 85, "PMX material");
  result.materials.reserve(material_count);
  for (std::uint32_t i = 0; i < material_count; ++i) {
    MaterialNames material;
    material.name = pmx_text(reader, encoding);
    material.alternate_name = pmx_text(reader, encoding);
    reader.skip(0x41);
    reader.skip(std::uint64_t{texture_index_width} * 2U + 1U);
    if (reader.u8() == 0) {
      const auto index = reader.index(texture_index_width);
      // -1 is the PMX "no texture" sentinel. Other invalid indices are a
      // malformed reference, not permission to index outside the texture list.
      if (index < -1 || (index >= 0 && static_cast<std::uint32_t>(index) >= texture_count)) {
        throw ParseError("PMX toon texture index is outside the texture table");
      }
      if (index >= 0) material.toon_texture = textures[static_cast<std::size_t>(index)];
    } else {
      material.toon_texture = builtin_toon(reader.u8());
    }
    skip_pmx_text(reader);
    reader.skip(4);
    result.materials.push_back(std::move(material));
  }
  result.format = MaterialFileFormat::pmx;
}

std::vector<std::wstring> companion_lines(const std::vector<std::uint8_t>* bytes) {
  if (bytes == nullptr || bytes->empty()) return {};
  UINT codepage = CP_ACP;
  std::size_t offset = 0;
  if (bytes->size() >= 3 && (*bytes)[0] == 0xef && (*bytes)[1] == 0xbb && (*bytes)[2] == 0xbf) {
    codepage = CP_UTF8;
    offset = 3;
  }
  std::vector<std::wstring> lines;
  while (offset < bytes->size()) {
    const auto begin = offset;
    while (offset < bytes->size() && (*bytes)[offset] != '\n' && (*bytes)[offset] != '\r') ++offset;
    lines.push_back(decode(bytes->data() + begin, offset - begin, codepage));
    if (offset < bytes->size() && (*bytes)[offset++] == '\r' && offset < bytes->size() && (*bytes)[offset] == '\n') ++offset;
  }
  return lines;
}

void parse_pmd(Reader& reader, const std::vector<std::uint8_t>* companion, MaterialTable& result) {
  const auto* header = reader.read(0x11b);
  float version;
  std::memcpy(&version, header + 3, sizeof(version));
  if (version != 1.0f) throw ParseError("unsupported PMD version");
  reader.skip(std::uint64_t{reader.count("PMD vertex count")} * 0x26U);
  reader.skip(std::uint64_t{reader.count("PMD surface count")} * 2U);
  const auto material_count = reader.count("PMD material count");
  require_minimum_bytes(reader, material_count, 0x46, "PMD material");
  std::vector<std::uint8_t> toon_indices;
  toon_indices.reserve(material_count);
  for (std::uint32_t i = 0; i < material_count; ++i) {
    reader.skip(0x2c);
    toon_indices.push_back(reader.u8());
    reader.skip(0x19);
  }
  const auto bone_count = reader.u16();
  reader.skip(std::uint64_t{bone_count} * 0x27U);
  const auto ik_count = reader.u16();
  for (std::uint16_t i = 0; i < ik_count; ++i) {
    reader.skip(4);
    const auto links = reader.u8();
    reader.skip(std::uint64_t{links} * 2U + 6U);
  }
  const auto morph_count = reader.u16();
  for (std::uint16_t i = 0; i < morph_count; ++i) {
    reader.skip(0x14);
    const auto vertices = reader.count("PMD morph vertex count");
    reader.skip(std::uint64_t{vertices} * 0x10U + 1U);
  }
  const auto face_display_count = reader.u8();
  reader.skip(std::uint64_t{face_display_count} * 2U);
  const auto bone_display_name_count = reader.u8();
  reader.skip(std::uint64_t{bone_display_name_count} * 0x32U);
  reader.skip(std::uint64_t{reader.count("PMD bone display count")} * 3U);
  if (reader.u8() != 0) {
    reader.skip(0x114);
    reader.skip(std::uint64_t{bone_count} * 20U);
    reader.skip(morph_count == 0 ? 0U : std::uint64_t{morph_count - 1U} * 20U);
    reader.skip(std::uint64_t{bone_display_name_count} * 0x32U);
  }
  std::vector<std::wstring> toon_names;
  toon_names.reserve(10);
  for (unsigned i = 0; i != 10; ++i) {
    const auto* name = reader.read(100);
    const auto* end = std::find(name, name + 100, std::uint8_t{});
    toon_names.push_back(decode(name, static_cast<std::size_t>(end - name), CP_ACP));
  }
  const auto names = companion_lines(companion);
  result.materials.reserve(material_count);
  for (std::uint32_t i = 0; i < material_count; ++i) {
    MaterialNames material;
    if (toon_indices[i] < toon_names.size()) material.toon_texture = toon_names[toon_indices[i]];
    if (i < names.size()) material.name = material.alternate_name = names[i];
    result.materials.push_back(std::move(material));
  }
  result.format = MaterialFileFormat::pmd;
}

bool is_builtin_toon(const std::wstring& name) {
  if (name.size() != 10 || name.compare(0, 4, L"toon") != 0 || name.compare(6, 4, L".bmp") != 0) return false;
  return (name[4] == L'0' && name[5] >= L'1' && name[5] <= L'9') || (name[4] == L'1' && name[5] == L'0');
}

void resolve_toon_paths(const std::filesystem::path& model_path, MaterialTable& table) {
  const auto parent = model_path.parent_path().native();
  for (auto& material : table.materials) {
    auto& texture = material.toon_texture;
    if (texture.empty() || is_builtin_toon(texture)) continue;
    std::wstring resolved = parent;
    const bool starts_with_separator = texture.front() == L'/' || texture.front() == L'\\';
    if (!starts_with_separator && !resolved.empty()) {
      const auto last = resolved.back();
      if (last != L'/' && last != L'\\' && last != L':') resolved += L'\\';
    }
    // This concatenation preserves the observed Boost path-append behavior;
    // std::filesystem operator/ would discard the parent for rooted addends.
    resolved += texture;
    std::replace(resolved.begin(), resolved.end(), L'\\', L'/');
    texture = std::move(resolved);
  }
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::string& error) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) { error = "could not open model or companion file"; return false; }
  const auto end = stream.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
    error = "model or companion file size is not representable";
    return false;
  }
  bytes.resize(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    error = "could not read complete model or companion file";
    return false;
  }
  return true;
}

} // namespace

bool parse_material_table_bytes(const std::vector<std::uint8_t>& model_bytes,
    const std::filesystem::path& model_path, const std::vector<std::uint8_t>* companion_text,
    MaterialTable& destination, std::string& error) {
  error.clear();
  try {
    if (model_bytes.size() < 3) throw ParseError("model signature is truncated");
    MaterialTable parsed;
    Reader reader(model_bytes);
    auto upper = [](std::uint8_t c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; };
    if (upper(model_bytes[0]) == 'P' && upper(model_bytes[1]) == 'M' && upper(model_bytes[2]) == 'D') {
      parse_pmd(reader, companion_text, parsed);
    } else if (upper(model_bytes[0]) == 'P' && upper(model_bytes[1]) == 'M' && upper(model_bytes[2]) == 'X') {
      parse_pmx(reader, parsed);
    } else {
      throw ParseError("unrecognized model signature");
    }
    resolve_toon_paths(model_path, parsed);
    destination = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool parse_material_table(const std::filesystem::path& model_path, MaterialTable& destination, std::string& error) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(model_path, bytes, error)) return false;
  std::vector<std::uint8_t> companion;
  const std::vector<std::uint8_t>* companion_pointer = nullptr;
  auto companion_path = model_path;
  companion_path.replace_extension(L".txt");
  std::error_code filesystem_error;
  const bool is_pmd = bytes.size() >= 3 && (bytes[0] == 'P' || bytes[0] == 'p') &&
      (bytes[1] == 'M' || bytes[1] == 'm') && (bytes[2] == 'D' || bytes[2] == 'd');
  if (is_pmd && std::filesystem::exists(companion_path, filesystem_error)) {
    if (!read_file(companion_path, companion, error)) return false;
    companion_pointer = &companion;
  }
  return parse_material_table_bytes(bytes, model_path, companion_pointer, destination, error);
}

} // namespace mme_re::mmhack
