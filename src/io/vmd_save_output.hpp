#pragma once

#include "mikumikustudio/recording_output_path.hpp"

namespace mikumikustudio::vmd_save {
// Capture the actual selected file (or its absence) before asking whether to replace it.
bool ConfirmDestination(HWND owner, const wchar_t* path, unsigned char language,
                        recording_output_path::Selection& destination);
bool Save(const recording_output_path::Selection& approved);
}
