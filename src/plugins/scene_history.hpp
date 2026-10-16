#pragma once
#include "shared_history.hpp"
#include <vector>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Captures only the incoming bindings and ordering affected by deletion. This
// is one component of object removal, not the resource/membership operation.
// It never owns model/mesh/Bullet resources. The scene owner must retain them
// and serialize preparation/publication. Registered pool growth preserves
// logical key slots; replaced pools and changed nested storage return STALE.
// A prepared candidate must be prepared again if its resolved addresses move.
std::shared_ptr<HistoryChange> CaptureModelRemovalBindings(MMDApp&, unsigned slot);
std::shared_ptr<HistoryChange> CaptureAccessoryRemovalOrder(MMDApp&, unsigned slot);
// One binding/order delta for the entire removal set. Objects in the set keep
// their own fields untouched; only surviving objects and global tracks change.
// Empty sets, duplicate slots, invalid slots and absent objects are rejected.
std::shared_ptr<HistoryChange> CaptureObjectsRemovalBindings(MMDApp&,
    const std::vector<unsigned>& modelSlots, const std::vector<unsigned>& accessorySlots);
// Internal presence preparation shares the scene service's identity registry.
std::unique_ptr<PreparedHistoryChange> PrepareObjectPresence(const void*, MmsId, bool present);
}
