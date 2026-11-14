#pragma once
#include "mikumikustudio/menu_command_ids.hpp"
#include <bitset>

namespace mikumikustudio::legacy_mmd_plugin {
class MenuIds {
    friend class MenuIdAllocator;
    static constexpr unsigned first = menu_command_ids::kLegacyObserverFirst;
    static constexpr unsigned count = menu_command_ids::kLegacyObserverLimit - first;
    std::bitset<count> owned_;

public:
    bool ObservesCommand(unsigned id, unsigned notification, bool hasControl, bool isMainWindow) const noexcept {
        return isMainWindow && !hasControl && notification == 0 &&
            menu_command_ids::IsLegacyObserver(id) && owned_[id - first];
    }

    template<class Remove>
    void RemoveAll(Remove remove) {
        for (unsigned i = 0; i < count; ++i)
            if (owned_[i]) remove(first + i);
        owned_.reset();
    }

    bool Any() const noexcept { return owned_.any(); }
};

// One allocator per host session. Even retired IDs and IDs already in the
// menu are skipped permanently so a queued command cannot change owners.
class MenuIdAllocator {
    unsigned next_ = menu_command_ids::kLegacyObserverFirst;

public:
    template<class IsInUse>
    int Allocate(MenuIds& owner, IsInUse isInUse) {
        while (next_ < menu_command_ids::kLegacyObserverLimit) {
            const unsigned id = next_++;
            if (isInUse(id)) continue;
            owner.owned_[id - MenuIds::first] = true;
            return static_cast<int>(id);
        }
        return -1;
    }
};
}
