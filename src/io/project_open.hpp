#pragma once

namespace mikumikustudio {
class MMDApp;
namespace pmm_io { class Input; }

// Internal status-returning bodies. The legacy void entry points remain intact.
// replaced becomes true only after the shared-history replacement boundary.
bool TryLoadSceneV1(MMDApp*, pmm_io::Input&, bool& replaced);
bool TryLoadSceneV2(MMDApp*, pmm_io::Input&, bool& replaced);
// Called after that boundary and before the first native scene release.
void AcceptProjectSceneReplacement(MMDApp*);
}
