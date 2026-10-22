# A fresh copied installation keeps repeated CTest runs and concurrent builds
# from sharing application data. Evidence is retained; nothing is overwritten.
if(NOT IS_ABSOLUTE "${PROBE}" OR NOT EXISTS "${PROBE}" OR NOT IS_ABSOLUTE "${OUTPUT_ROOT}")
    message(FATAL_ERROR "An absolute existing probe and absolute output root are required")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(run "${OUTPUT_ROOT}/m-${suffix}")
if(EXISTS "${run}")
    message(FATAL_ERROR "Refusing to reuse a previous mesh cache test installation")
endif()
file(MAKE_DIRECTORY "${run}/installation" "${run}/cwd" "${run}/temporary")
file(COPY_FILE "${PROBE}" "${run}/installation/mesh-cache-probe.exe")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "TEMP=${run}/temporary" "TMP=${run}/temporary"
        "${run}/installation/mesh-cache-probe.exe" "${run}/fixtures"
    WORKING_DIRECTORY "${run}/cwd"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 90)
file(WRITE "${run}/result.txt" "exit=${result}\n${output}\n${errors}")
message(STATUS "Mesh cache evidence: ${run}\n${output}")
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "Mesh cache regression failed: ${result}\n${errors}")
endif()
