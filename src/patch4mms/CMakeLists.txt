cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MMSPatch4MMS LANGUAGES CXX)
endif()
add_library(mms_patch4mms SHARED plugin.cpp)
target_include_directories(mms_patch4mms PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/mms/include")
target_compile_features(mms_patch4mms PRIVATE cxx_std_17)
set_target_properties(mms_patch4mms PROPERTIES OUTPUT_NAME Patch4MMS
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/PLUGINS/patch4mms")
add_custom_command(TARGET mms_patch4mms POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "$<TARGET_FILE_DIR:mms_patch4mms>/plugin.json")
install(TARGETS mms_patch4mms RUNTIME DESTINATION PLUGINS/patch4mms)
install(FILES plugin.json DESTINATION PLUGINS/patch4mms)

set_property(TARGET mms_patch4mms APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${CMAKE_CURRENT_SOURCE_DIR}/Enable.cmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/Disable.cmd" "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
foreach(asset Enable.cmd Disable.cmd README.md)
    add_custom_command(TARGET mms_patch4mms POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${asset}" "$<TARGET_FILE_DIR:mms_patch4mms>/${asset}")
endforeach()
install(FILES Enable.cmd Disable.cmd README.md DESTINATION PLUGINS/patch4mms)
