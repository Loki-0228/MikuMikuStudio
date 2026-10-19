# Preserve existing build directories and old command-line options across the
# project rename. An explicitly supplied new name always takes precedence.
foreach(_mms_suffix IN ITEMS PROFILE ENABLE_DX12 DEBUG_SYMBOLS WIN7_BASELINE
        PERF DIAG BUILD_INPUT_TESTS BUILD_EFFECT_TESTS BUILD_ENCODING_TESTS
        BUILD_UI_TESTS PRECOMPILE_EFFECTS)
    set(_mms_old "MIKUDANCESTUDIO_${_mms_suffix}")
    set(_mms_new "MIKUMIKUSTUDIO_${_mms_suffix}")
    if(DEFINED ${_mms_old} AND NOT DEFINED ${_mms_new})
        if(_mms_suffix STREQUAL "PROFILE")
            set(_mms_type STRING)
        else()
            set(_mms_type BOOL)
        endif()
        set(${_mms_new} "${${_mms_old}}" CACHE ${_mms_type}
            "Migrated from ${_mms_old}")
        message(STATUS "Migrated ${_mms_old} to ${_mms_new}=${${_mms_new}}")
    endif()
endforeach()
unset(_mms_suffix)
unset(_mms_old)
unset(_mms_new)
unset(_mms_type)
