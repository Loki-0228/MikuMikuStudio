#pragma once

namespace mikudancestudio {
class MMDApp;

// Internal status-returning bodies. The legacy void entry points remain intact.
// replaced becomes true only after the shared-history replacement boundary.
bool TryLoadSceneV1(MMDApp*, int fd, bool& replaced);
bool TryLoadSceneV2(MMDApp*, int fd, bool& replaced);
// Called after that boundary and before the first native scene release.
void AcceptProjectSceneReplacement(MMDApp*);
}
