#pragma once

namespace mikumikustudio {
class MMDApp;
// -1: no expression row selected; -2: multiple destinations selected.
int ForcePasteMorphTarget(const MMDApp* app);
bool ForcePasteMorphKeys(MMDApp* app, int target);
void RefreshForcePasteControls(MMDApp* app);
}
