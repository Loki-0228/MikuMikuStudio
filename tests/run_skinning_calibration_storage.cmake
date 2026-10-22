# Each invocation owns a new copied installation. Keep prior evidence intact.
if(NOT IS_ABSOLUTE "${PROBE}" OR NOT EXISTS "${PROBE}" OR NOT IS_ABSOLUTE "${OUTPUT_ROOT}")
    message(FATAL_ERROR "An absolute existing probe and absolute output root are required")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(run "${OUTPUT_ROOT}/c-${suffix}")
if(EXISTS "${run}")
    message(FATAL_ERROR "Refusing to reuse a calibration test installation")
endif()
file(MAKE_DIRECTORY "${run}/installation" "${run}/cwd" "${run}/temporary" "${run}/legacy")
file(COPY_FILE "${PROBE}" "${run}/installation/calibration-probe.exe")
file(CONFIGURE OUTPUT "${run}/installation/calibration-test-installation.txt"
    CONTENT "MMS calibration test installation v1\n" NEWLINE_STYLE UNIX)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "TEMP=${run}/temporary" "TMP=${run}/temporary" "LOCALAPPDATA=${run}/legacy"
        "${run}/installation/calibration-probe.exe" --storage "${run}/fixtures"
    WORKING_DIRECTORY "${run}/cwd"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
file(WRITE "${run}/result.txt" "exit=${result}\n${output}\n${errors}")
message(STATUS "Calibration storage evidence: ${run}\n${output}")
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "Calibration storage regression failed: ${result}\n${errors}")
endif()
