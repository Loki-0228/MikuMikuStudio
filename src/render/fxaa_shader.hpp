#pragma once
namespace mikudancestudio::antialias {
// One spatial resolve for native, material and MME edges. Coverage alpha stays
// independent of RGB luminance, including transparent image/video exports.
inline constexpr char kFxaaShader[] =
#include "fxaa_reference.hpp"
;
}
