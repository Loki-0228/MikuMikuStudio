#pragma once
#include "shared_history.hpp"

namespace mikudancestudio {
class MMDApp;
namespace plugins {
// Internal host journal. Public mms.edit remains unavailable until every
// native editing command participates in this journal.
SharedHistory* NativeHistory(MMDApp*) noexcept;
// Isolated-host migration probe only. The ordinary Initialize path never
// calls this. Requires no active plugin runtime and an initialized main window.
// This exposes mms.edit and binds only the migrated native commands; a probe
// must not dispatch writers that have not joined the common journal yet.
bool InitializeHistoryTestRuntime(MMDApp*) noexcept;
// Retry a post-publication UI update on the host's normal message boundary.
// No-op without this application's initialized runtime.
void RequestBoundHistoryUiRefresh(MMDApp*) noexcept;
}
}
