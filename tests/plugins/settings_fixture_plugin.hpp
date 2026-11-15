#pragma once
#include <mms/settings.h>

// Native test harness only. Requests and their buffers live through Manager::Drain.
struct SettingsFixtureRequest {
    enum class Op { Read, Merge } op = Op::Read;
    char* buffer = nullptr;
    uint64_t capacity = 0, bytes = 0;
    MmsString expected{}, patch{};
    MmsResult result = MMS_E_NOT_READY;
    bool completed = false;
};
struct SettingsFixtureObservations {
    MmsResult query = MMS_E_NOT_READY;
    MmsResult createRead = MMS_E_NOT_READY;
    MmsResult startMerge = MMS_E_NOT_READY;
    MmsResult eventRead = MMS_E_NOT_READY;
    MmsResult closingRead = MMS_E_FAILED;
    unsigned unexpectedEvents = 0;
};
extern "C" const SettingsFixtureObservations* MMS_CALL MmsSettingsFixtureObserve();
extern "C" MmsResult MMS_CALL MmsSettingsFixtureQueue(SettingsFixtureRequest*);
extern "C" MmsResult MMS_CALL MmsSettingsFixtureDirect(SettingsFixtureRequest*);
