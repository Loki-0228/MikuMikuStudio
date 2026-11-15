#pragma once
#include "manifest.hpp"

namespace mikumikustudio::plugins {
struct LegacyMmdImage {
    Manifest manifest;
    fs::path facade;
};
// Authenticate the source, then adapt only a separate local copy. The original
// filename and source package remain untouched, regardless of the host EXE name.
LegacyMmdImage PrepareLegacyMmdImage(const Manifest&, const fs::path& install);
}
