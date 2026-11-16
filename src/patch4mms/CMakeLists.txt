cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MMSPatch4MMS LANGUAGES CXX)
endif()
add_library(mms_patch4mms SHARED plugin.cpp)
target_include_directories(mms_patch4mms PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/mms/include")
target_compile_features(mms_patch4mms PRIVATE cxx_std_17)
set_target_properties(mms_patch4mms PROPERTIES OUTPUT_NAME dx12mmepatch
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/PLUGINS/dx12mmepatch")
# The host recognizes this bundled bridge through its direct marker export.
# A separate dependency also removes stale manifests when the DLL is up to date.
add_custom_target(mms_patch4mms_remove_manifest
    COMMAND "${CMAKE_COMMAND}" -E rm -f
        "${CMAKE_BINARY_DIR}/$<CONFIG>/PLUGINS/dx12mmepatch/plugin.json"
    VERBATIM)
add_dependencies(mms_patch4mms mms_patch4mms_remove_manifest)
install(TARGETS mms_patch4mms RUNTIME DESTINATION PLUGINS/dx12mmepatch)
install(CODE "file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/PLUGINS/dx12mmepatch/plugin.json\")")

set_property(TARGET mms_patch4mms APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
foreach(asset README.md)
    add_custom_command(TARGET mms_patch4mms POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${asset}" "$<TARGET_FILE_DIR:mms_patch4mms>/${asset}")
endforeach()
install(FILES README.md DESTINATION PLUGINS/dx12mmepatch)
