#pragma once
#include <mms/mme_on12.h>
#include <Windows.h>
#include <string>
struct IDirect3DDevice9;
namespace mikumikustudio::plugins {
void SelectMmeRenderBackend(bool preferDx12) noexcept;
void ConfigureMmeRenderStartup(bool requestEnabled, bool mmeLoaded, HMODULE bridge) noexcept;
bool MmeOn12Attempting() noexcept;
bool FallBackMmeOn12(uint32_t reason) noexcept;
bool ConfirmMmeOn12Device(IDirect3DDevice9*) noexcept;
void RecordMmeDx9Device() noexcept;
MmsMmeOn12StatusV1 MmeRenderStatus() noexcept;
MmsMmeOn12ApiV1 MmeOn12Api() noexcept;
const char* MmeRenderReason() noexcept;
void RecordMmeStartupError(const std::string& id, const std::string& message);
std::string MmeStartupErrors();
}
