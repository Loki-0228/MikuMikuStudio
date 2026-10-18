#pragma once

#include <Windows.h>
#include <unknwn.h>
#include "mikudancestudio/dshow_recorder.hpp"

namespace mikudancestudio::recording_state {

enum class AckResult { Ready, Ended, Cancelled, TimedOut };

// The caller owns a COM reference throughout this wait. Pumping can finish or
// reset a recording, so check session/resource identities after every dispatch.
template<class Current, class Pump>
AckResult WaitForAck(IPushSource* push, unsigned char* flag, Current current,
                     Pump pump, DWORD timeoutMs = 120000) {
    const auto began = GetTickCount64();
    for (;;) {
        if (!current()) return AckResult::Cancelled;
        unsigned char acknowledged = 0;
        if (FAILED(push->GetStreamingState(&acknowledged))) return AckResult::Ended;
        if (acknowledged) {
            if (flag) *flag = acknowledged;
            return AckResult::Ready;
        }
        if (!pump() || !current()) return AckResult::Cancelled;
        if (GetTickCount64() - began >= timeoutMs) return AckResult::TimedOut;
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

} // namespace mikudancestudio::recording_state
