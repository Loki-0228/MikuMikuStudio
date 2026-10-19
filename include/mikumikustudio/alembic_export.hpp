#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace mikumikustudio {
class MMDApp;
namespace alembic_export {
struct Selection {
    bool accessory = false;
    int slot = 0;
    // Empty lists select the complete object. A nonempty list restricts output.
    std::vector<std::uint32_t> materials, vertices;
    bool selectedBoneVertices = false;
};
struct Options {
    int firstFrame = 0;
    int lastFrame = 0;
    int framesPerSecond = 30;
    bool selectedOnly = false;
    bool writeAbc = true;
    bool bakeIntoProject = false;
    bool explicitSelection = false;
    std::vector<Selection> selection;
    // Bake to MMV layer x: the meshes recorded are the OUTPUT of layer x (the
    // chain [maximum..x] composed once by the runtime), and after a successful
    // bake the selected objects' MMV FX assignments on layers [x..maximum] are
    // permanently removed - never MME (-1), never the lower layers.  -1 keeps
    // the existing full-chain bake with its in-range enable keys.  A layer bake
    // refuses every inexpressible combination before committing anything.
    int mmvLayerCutoff = -1;
    bool preview=false;
};
struct Result {
    bool success = false;
    bool cancelled = false;
    bool abcWritten = false;
    bool baked = false;
    unsigned samples = 0;
    unsigned meshes = 0;
    std::string error;
};
using Progress = std::function<bool(int frame, int lastFrame)>;
bool CanExport(const MMDApp* app);
bool IsExporting();
void ShowDialog(MMDApp* app);
void ShowBakeDialog(MMDApp* app);
Result Export(MMDApp& app, const std::filesystem::path& path,
              const Options& options, const Progress& progress = {});
}
}
