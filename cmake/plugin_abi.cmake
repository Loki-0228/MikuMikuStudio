# Native plugin ABI. This target has no renderer, CUDA, NVAPI or DX12 dependency.
enable_language(C)
add_subdirectory(third_party/native_ui)
add_library(mms_plugin_runtime STATIC
    src/plugins/json.cpp
    src/plugins/manifest.cpp
    src/plugins/loader_win32.cpp
    src/plugins/ui_registry.cpp
    src/plugins/ui_refresh.cpp
    src/plugins/ui_preferences_file.cpp
    src/plugins/manager.cpp
    src/plugins/scene_store.cpp
    src/plugins/scene_identity.cpp
    src/plugins/storage_store.cpp
    src/plugins/edit_store.cpp
    src/plugins/shared_history.cpp
    src/plugins/project_storage.cpp
    src/plugins/native_ui.cpp)
target_include_directories(mms_plugin_runtime PUBLIC sdk/mms/include PRIVATE src/plugins)
target_link_libraries(mms_plugin_runtime PUBLIC native_ui::win32 user32 gdi32 comctl32 bcrypt)
target_compile_definitions(mms_plugin_runtime PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
# Host-side static libraries inherit the host/Conan runtime. Only independent
# plugin DLLs select /MT; ownership never crosses the public C ABI.
target_link_libraries(mikudancestudio_core PUBLIC mms_plugin_runtime)
target_sources(mikudancestudio_core PRIVATE src/plugins/integration.cpp src/app/update_coordinator.cpp src/plugins/scene_host.cpp src/plugins/scene_geometry.cpp src/plugins/scene_definitions.cpp src/plugins/scene_history.cpp)
if(MIKUDANCESTUDIO_PROFILE STREQUAL "full")
    target_sources(mikudancestudio_core PRIVATE src/model/model_import.cpp src/model/import_texture_cache.cpp)
    target_sources(mikudancestudio_core PRIVATE src/plugins/native_object_commands.cpp)
    target_sources(mikudancestudio_core PRIVATE src/plugins/native_global_registration.cpp src/plugins/native_accessory_registration.cpp src/plugins/native_global_commands.cpp src/plugins/native_camera_interpolation.cpp)
    target_sources(mikudancestudio_core PRIVATE src/physics/history_physics.cpp src/plugins/scene_object_history.cpp src/plugins/scene_resource_budget.cpp src/plugins/native_pose_scope.cpp src/plugins/native_history_commands.cpp src/plugins/native_pose_axis.cpp src/plugins/native_bone_registration.cpp src/plugins/native_key_commands.cpp src/plugins/native_bone_evaluation.cpp src/plugins/native_display_keys.cpp src/plugins/native_morph_keys.cpp src/plugins/native_morph_commands.cpp src/plugins/native_model_keys.cpp)
endif()

add_library(mms_hello_plugin SHARED EXCLUDE_FROM_ALL
    sdk/mms/examples/hello_menu/plugin.cpp sdk/mms/examples/hello_menu/plugin.def)
target_include_directories(mms_hello_plugin PRIVATE sdk/mms/include)
target_compile_definitions(mms_hello_plugin PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_hello_plugin PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_hello_plugin PROPERTIES OUTPUT_NAME hello_menu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/hello/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_custom_command(TARGET mms_hello_plugin POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/mms/examples/hello_menu/plugin.json" "$<TARGET_FILE_DIR:mms_hello_plugin>/plugin.json")

add_executable(plugin_abi_tests EXCLUDE_FROM_ALL tests/plugins/runtime_tests.cpp tests/plugins/abi_layout.cpp)
add_library(mms_abi_c_check OBJECT EXCLUDE_FROM_ALL tests/plugins/abi_c.c)
target_include_directories(mms_abi_c_check PRIVATE sdk/mms/include)
set_target_properties(mms_abi_c_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(plugin_abi_tests mms_abi_c_check)
target_include_directories(plugin_abi_tests PRIVATE src/plugins)
target_link_libraries(plugin_abi_tests PRIVATE mms_plugin_runtime native_ui::update)
target_compile_options(plugin_abi_tests PRIVATE /UNDEBUG)
target_link_options(plugin_abi_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(plugin_abi_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(plugin_abi_tests mms_hello_plugin)
add_library(mms_lifecycle_plugin SHARED EXCLUDE_FROM_ALL tests/plugins/lifecycle_plugin.cpp tests/plugins/lifecycle_plugin.def)
target_include_directories(mms_lifecycle_plugin PRIVATE sdk/mms/include)
target_compile_definitions(mms_lifecycle_plugin PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_lifecycle_plugin PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_lifecycle_plugin PROPERTIES OUTPUT_NAME lifecycle
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/lifecycle/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_lifecycle_plugin)
add_library(mms_plugin_edit_probe SHARED EXCLUDE_FROM_ALL tests/plugins/manager_edit_plugin.cpp tests/plugins/manager_edit_plugin.def)
target_include_directories(mms_plugin_edit_probe PRIVATE sdk/mms/include)
target_compile_definitions(mms_plugin_edit_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_plugin_edit_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_plugin_edit_probe PROPERTIES OUTPUT_NAME manager_edit_probe
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/edit/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_plugin_edit_probe)
add_test(NAME plugin_abi_contract COMMAND plugin_abi_tests "$<TARGET_FILE_DIR:mms_hello_plugin>" "$<TARGET_FILE:mms_lifecycle_plugin>" "$<TARGET_FILE:mms_plugin_edit_probe>")
set_tests_properties(plugin_abi_contract PROPERTIES TIMEOUT 90)
install(FILES PLUGINS/config.json DESTINATION PLUGINS)
