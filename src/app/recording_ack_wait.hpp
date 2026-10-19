#pragma once

#include <Windows.h>
#include <unknwn.h>
#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/recording_state.hpp"

namespace mikumikustudio::recording_state {

enum class AckResult { Ready, Ended, Cancelled, TimedOut };

// Close commands observed inside a stop's message drain are replayed once the
// drain returns. The guard is declared before local COM owners, so their final
// releases cannot observe an unguarded recorder during cleanup.
class WindowStopGuard {
    Guard guard_;
public:
    WindowStopGuard(const void* identity, Operation operation) noexcept : guard_(identity, operation) {}
    ~WindowStopGuard() {
        if (const auto window = guard_.DeferredClose())
            PostMessageW(reinterpret_cast<HWND>(window), WM_CLOSE, 0, 0);
    }
    explicit operator bool() const noexcept { return static_cast<bool>(guard_); }
};

// The caller owns a COM reference throughout this wait. Pumping can finish or
// reset a recording, so check session/resource identities after every dispatch.
template<class Current, class Pump>
AckResult WaitForAck(IPushSource* push, unsigned char* flag, Current current,
                     Pump pump, DWORD timeoutMs = 120000) {
    const auto began = GetTickCount64();
    for (;;) {
        if (!current()) return AckResult::Cancelled;
        if (!push) return AckResult::Ended;
        unsigned char acknowledged = 0;
        const auto result = push->GetStreamingState(&acknowledged);
        if (!current()) return AckResult::Cancelled;
        if (FAILED(result)) return AckResult::Ended;
        if (acknowledged) {
            if (flag) *flag = acknowledged;
            return AckResult::Ready;
        }
        if (!pump() || !current()) return AckResult::Cancelled;
        if (GetTickCount64() - began >= timeoutMs) return AckResult::TimedOut;
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

template<class Current>
bool PumpRecordingMessages(Current current) {
    MSG message{};
    for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (!current()) return false;
    }
    return current();
}

} // namespace mikumikustudio::recording_state
