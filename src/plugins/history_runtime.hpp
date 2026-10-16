#pragma once
#include "shared_history.hpp"

namespace mikudancestudio {
class MMDApp;
namespace plugins {
// Internal host journal. Public mms.edit remains unavailable until every
// native editing command participates in this journal.
SharedHistory* NativeHistory(MMDApp*) noexcept;
}
}
