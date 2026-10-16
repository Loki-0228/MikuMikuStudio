#pragma once
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Captures only the incoming bindings and ordering affected by deletion. This
// is one component of object removal, not the resource/membership operation.
// It never owns model/mesh/Bullet resources. The scene owner must retain them
// and serialize preparation/publication. Replaced pools return STALE.
std::shared_ptr<HistoryChange> CaptureModelRemovalBindings(MMDApp&, unsigned slot);
std::shared_ptr<HistoryChange> CaptureAccessoryRemovalOrder(MMDApp&, unsigned slot);
// Internal presence preparation shares the scene service's identity registry.
std::unique_ptr<PreparedHistoryChange> PrepareObjectPresence(const void*, MmsId, bool present);
}
