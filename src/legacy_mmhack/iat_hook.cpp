#include "iat_hook.hpp"
#include <cstdint>
#include <cstring>
#include <limits>
namespace mme_re::mmhack {
namespace {
bool within(std::size_t offset, std::size_t bytes, std::size_t size) { return offset <= size && bytes <= size - offset; }
}
IatPatchResult replace_import_address(void* image, std::size_t size, FARPROC original, FARPROC replacement) {
  IatPatchResult result;
  if (!image || !original || !replacement || !within(0, sizeof(IMAGE_DOS_HEADER), size)) { result.error = ERROR_INVALID_PARAMETER; return result; }
  auto* base = static_cast<std::uint8_t*>(image);
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, base, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || !within(static_cast<std::size_t>(dos.e_lfanew), sizeof(IMAGE_NT_HEADERS64), size)) { result.error = ERROR_BAD_EXE_FORMAT; return result; }
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, base + dos.e_lfanew, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IAT) { result.error = ERROR_BAD_EXE_FORMAT; return result; }
  const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
  if (!directory.VirtualAddress || !within(directory.VirtualAddress, directory.Size, size)) { result.error = ERROR_BAD_EXE_FORMAT; return result; }
  for (std::size_t offset = 0; within(offset, sizeof(FARPROC), directory.Size); offset += sizeof(FARPROC)) {
    auto* slot = base + directory.VirtualAddress + offset;
    FARPROC value{};
    std::memcpy(&value, slot, sizeof(value));
    if (value != original) continue;
    DWORD protection{};
    if (!VirtualProtect(slot, sizeof(FARPROC), PAGE_READWRITE, &protection)) { result.error = GetLastError(); return result; }
    std::memcpy(slot, &replacement, sizeof(replacement));
    ++result.replaced;
    DWORD ignored{};
    if (!VirtualProtect(slot, sizeof(FARPROC), protection, &ignored)) { result.error = GetLastError(); return result; }
  }
  return result;
}
IatPatchResult hook_import(const char* name, FARPROC replacement, HMODULE target, HMODULE provider) {
  if (!target) target = GetModuleHandleW(nullptr);
  if (!target || !provider || !name) return {0, ERROR_INVALID_PARAMETER};
  const auto original = GetProcAddress(provider, name);
  if (!original) return {0, ERROR_PROC_NOT_FOUND};
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(target);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return {0, ERROR_BAD_EXE_FORMAT};
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(target) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return {0, ERROR_BAD_EXE_FORMAT};
  return replace_import_address(target, nt->OptionalHeader.SizeOfImage, original, replacement);
}
}
