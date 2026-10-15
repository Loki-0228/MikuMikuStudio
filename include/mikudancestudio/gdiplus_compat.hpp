#pragma once
#include <algorithm>
// SDK 19041 GDI+ uses unqualified min/max inside namespace Gdiplus.
// Supply functions there without restoring Windows' global min/max macros.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
