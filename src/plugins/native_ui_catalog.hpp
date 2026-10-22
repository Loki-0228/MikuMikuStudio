#pragma once
#include "ui_catalog.hpp"
#include <Windows.h>

namespace mikumikustudio::plugins {
class UiRegistry;
struct NativeCatalogMenuAlias {
    std::uintptr_t menu = 0, subMenu = 0;
    std::uint32_t commandId = 0; // UINT32_MAX permits an adapter-owned submenu root.
    MmsHandle element = 0;
    std::string id;
};
// Adapter-side capture only. Queries through mms.ui.catalog never call here.
// Owns lifetime observers, never the application's windows or menus.
class NativeUiCatalog {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeUiCatalog(UiRegistry&, HWND main);
    ~NativeUiCatalog();
    MmsResult Observe(const std::vector<NativeCatalogMenuAlias>&) noexcept;
};
}
