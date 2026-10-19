#pragma once
#include <functional>
#include <string>
namespace mikumikustudio {
class MMDApp;
namespace scene_sampling {
// The callback renders the private evaluated scene through the live object
// identities required by MME. Motion keys and the editor's physics world are
// never changed. Times include preroll, starting at zero.
bool Visit(MMDApp& app, int firstFrame, int lastFrame, int fps,
           const std::function<bool(double seconds, bool output)>& visitor,
           std::string& error);
}
}
