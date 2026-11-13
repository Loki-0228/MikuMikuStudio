#pragma once

#include <Windows.h>
#include <cstddef>
#include "mikumikustudio/image_sequence_export.hpp"

namespace mikumikustudio {
class MMDApp;

HRESULT ChooseStillImagePath(MMDApp* app, HWND owner, wchar_t* file, std::size_t capacity,
                             std::optional<image_sequence::StillFileIdentity>& overwrite);
}
