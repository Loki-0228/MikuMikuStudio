# Native plugin ABI. This target has no renderer, CUDA, NVAPI or DX12 dependency.
enable_language(C)
add_subdirectory(third_party/native_ui)
add_library(mms_system_settings STATIC
    src/app/system_settings.cpp
    src/plugins/json.cpp)
target_include_directories(mms_system_settings PUBLIC include sdk/mms/include)
target_compile_definitions(mms_system_settings PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mms_system_settings PUBLIC user32)
add_executable(system_settings_tests EXCLUDE_FROM_ALL tests/system_settings_tests.cpp)
target_link_libraries(system_settings_tests PRIVATE mms_system_settings)
target_link_options(system_settings_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(system_settings_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME system_settings_store COMMAND system_settings_tests)
set_tests_properties(system_settings_store PROPERTIES TIMEOUT 60)
add_library(mms_plugin_runtime STATIC
    src/plugins/manifest.cpp
    src/plugins/loader_win32.cpp
    src/plugins/ui_registry.cpp
    src/plugins/ui_property_value.cpp
    src/plugins/ui_property_draft.cpp
    src/plugins/ui_refresh.cpp
    src/plugins/ui_refresh_dependencies.cpp
    src/plugins/ui_refresh_binding.cpp
    src/plugins/ui_property_refresh.cpp
    src/plugins/ui_preferences_file.cpp
    src/plugins/manager.cpp
    src/plugins/scene_store.cpp
    src/plugins/scene_identity.cpp
    src/plugins/storage_store.cpp
    src/plugins/edit_store.cpp
    src/plugins/shared_history.cpp
    src/plugins/project_storage.cpp
    src/plugins/native_ui.cpp src/plugins/native_property_input.cpp)
target_include_directories(mms_plugin_runtime PUBLIC sdk/mms/include include PRIVATE src/plugins)
target_link_libraries(mms_plugin_runtime PUBLIC mms_system_settings native_ui::win32 user32 gdi32 comctl32 bcrypt)
target_compile_definitions(mms_plugin_runtime PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
# Host-side static libraries inherit the host/Conan runtime. Only independent
# plugin DLLs select /MT; ownership never crosses the public C ABI.
target_link_libraries(mikumikustudio_core PUBLIC mms_plugin_runtime)
target_sources(mikumikustudio_core PRIVATE src/plugins/integration.cpp src/app/update_coordinator.cpp src/plugins/scene_host.cpp src/plugins/scene_geometry.cpp src/plugins/scene_definitions.cpp src/plugins/scene_history.cpp)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
    target_sources(mikumikustudio_core PRIVATE src/model/model_import.cpp src/model/import_texture_cache.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_object_commands.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_accessory_settings.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_order.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_outline.cpp src/plugins/native_model_outline_ui.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_outline_color.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_names.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_toon.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/model_physics_resources.cpp src/plugins/native_model_physics.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_curve_write.cpp src/plugins/native_curve_commands.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_morph_curve_write.cpp src/plugins/native_bone_curve_write.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_frame_lines.cpp src/plugins/native_key_paste.cpp src/plugins/native_vmd_import.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_global_paste.cpp src/plugins/native_gravity_paste.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_vmd_source.cpp src/plugins/native_model_vmd_source.cpp src/plugins/native_viewport_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_viewport_reset_camera.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_camera_hud_history.cpp src/plugins/native_global_key_delete.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_accessory_hud_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_hud_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_morph_reset.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_physics_toggle.cpp src/plugins/native_center_offset.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_camera_follow.cpp src/plugins/native_camera_follow_sample.cpp src/plugins/native_camera_follow_ui.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_camera_parent_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_global_registration.cpp src/plugins/native_accessory_registration.cpp src/plugins/native_global_commands.cpp src/plugins/native_camera_interpolation.cpp src/plugins/native_camera_multiply.cpp src/plugins/native_morph_category.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_multiply.cpp src/plugins/native_morph_tools.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_physics_mode.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_floor_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/media/wave_audio_asset.cpp src/plugins/native_audio_history.cpp src/window/audio_menu.cpp)
    target_sources(mikumikustudio_core PRIVATE src/physics/history_physics.cpp src/plugins/scene_object_history.cpp src/plugins/scene_resource_budget.cpp src/plugins/native_pose_scope.cpp src/plugins/native_history_commands.cpp src/plugins/native_pose_axis.cpp src/plugins/native_bone_registration.cpp src/plugins/native_key_commands.cpp src/plugins/native_bone_evaluation.cpp src/plugins/native_display_keys.cpp src/plugins/native_morph_keys.cpp src/plugins/native_morph_commands.cpp src/plugins/native_vpd_import.cpp src/plugins/native_model_keys.cpp)
endif()

if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
    add_executable(native_model_toon_loading_tests EXCLUDE_FROM_ALL
        tests/native_model_toon_loading_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(native_model_toon_loading_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(native_model_toon_loading_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}" /SUBSYSTEM:CONSOLE,6.01)
    set_target_properties(native_model_toon_loading_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endif()

add_executable(display_group_text_tests EXCLUDE_FROM_ALL tests/display_group_text_tests.cpp src/main/text_encoding.cpp)
target_include_directories(display_group_text_tests PRIVATE include)
target_compile_features(display_group_text_tests PRIVATE cxx_std_17)
target_compile_definitions(display_group_text_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(display_group_text_tests PRIVATE user32)
target_link_options(display_group_text_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(display_group_text_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME display_group_text_contract COMMAND display_group_text_tests)
set_tests_properties(display_group_text_contract PROPERTIES TIMEOUT 60)

add_executable(native_model_outline_color_legacy_tests EXCLUDE_FROM_ALL tests/native_model_outline_color_legacy_tests.cpp)
target_link_libraries(native_model_outline_color_legacy_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
target_link_options(native_model_outline_color_legacy_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(native_model_outline_color_legacy_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME native_model_outline_color_legacy COMMAND native_model_outline_color_legacy_tests)
set_tests_properties(native_model_outline_color_legacy PROPERTIES TIMEOUT 60)

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
add_test(NAME plugin_abi_panel_state COMMAND plugin_abi_tests --panel-state-only)
set_tests_properties(plugin_abi_panel_state PROPERTIES TIMEOUT 90)
add_test(NAME plugin_abi_menu_state COMMAND plugin_abi_tests --menu-state-only)
set_tests_properties(plugin_abi_menu_state PROPERTIES TIMEOUT 90)
add_executable(ui_property_value_tests EXCLUDE_FROM_ALL tests/plugins/ui_property_value_tests.cpp)
target_link_libraries(ui_property_value_tests PRIVATE mms_plugin_runtime)
target_include_directories(ui_property_value_tests PRIVATE src/plugins)
target_link_options(ui_property_value_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(ui_property_value_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME ui_property_value_contract COMMAND ui_property_value_tests)
set_tests_properties(ui_property_value_contract PROPERTIES TIMEOUT 60)
add_executable(ui_property_draft_tests EXCLUDE_FROM_ALL tests/plugins/ui_property_draft_tests.cpp)
target_link_libraries(ui_property_draft_tests PRIVATE mms_plugin_runtime)
target_include_directories(ui_property_draft_tests PRIVATE src/plugins)
target_link_options(ui_property_draft_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(ui_property_draft_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME ui_property_draft_contract COMMAND ui_property_draft_tests)
set_tests_properties(ui_property_draft_contract PROPERTIES TIMEOUT 60)
add_executable(ui_refresh_binding_tests EXCLUDE_FROM_ALL tests/plugins/ui_refresh_binding_tests.cpp)
target_include_directories(ui_refresh_binding_tests PRIVATE src/plugins sdk/mms/include)
target_compile_features(ui_refresh_binding_tests PRIVATE cxx_std_17)
target_compile_definitions(ui_refresh_binding_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(ui_refresh_binding_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(ui_refresh_binding_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME ui_refresh_binding_contract COMMAND ui_refresh_binding_tests)
set_tests_properties(ui_refresh_binding_contract PROPERTIES TIMEOUT 60)
add_executable(ui_refresh_subscription_store_tests EXCLUDE_FROM_ALL tests/plugins/ui_refresh_subscription_store_tests.cpp)
target_include_directories(ui_refresh_subscription_store_tests PRIVATE src/plugins sdk/mms/include)
target_compile_features(ui_refresh_subscription_store_tests PRIVATE cxx_std_17)
target_compile_definitions(ui_refresh_subscription_store_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(ui_refresh_subscription_store_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(ui_refresh_subscription_store_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME ui_refresh_subscription_store COMMAND ui_refresh_subscription_store_tests)
set_tests_properties(ui_refresh_subscription_store PROPERTIES TIMEOUT 60)
add_test(NAME ui_refresh_subscription_runtime COMMAND plugin_abi_tests --subscription-refresh-only "$<TARGET_FILE:mms_subscription_refresh_probe>")
set_tests_properties(ui_refresh_subscription_runtime PROPERTIES TIMEOUT 180)
add_library(mms_abi_c_check OBJECT EXCLUDE_FROM_ALL tests/plugins/abi_c.c)
target_include_directories(mms_abi_c_check PRIVATE sdk/mms/include)
set_target_properties(mms_abi_c_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(plugin_abi_tests mms_abi_c_check)
target_include_directories(plugin_abi_tests PRIVATE src/plugins)
target_link_libraries(plugin_abi_tests PRIVATE mms_plugin_runtime native_ui::update minhook::minhook)
target_compile_options(plugin_abi_tests PRIVATE /UNDEBUG /bigobj)
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
add_test(NAME ui_layout_refresh_runtime COMMAND plugin_abi_tests --layout-refresh-only "$<TARGET_FILE:mms_data_refresh_probe>")
set_tests_properties(ui_layout_refresh_runtime PROPERTIES TIMEOUT 120)
add_library(mms_data_refresh_probe MODULE EXCLUDE_FROM_ALL tests/plugins/data_refresh_plugin.cpp tests/plugins/data_refresh_plugin.def)
target_include_directories(mms_data_refresh_probe PRIVATE sdk/mms/include)
target_compile_features(mms_data_refresh_probe PRIVATE cxx_std_17)
target_compile_definitions(mms_data_refresh_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mms_data_refresh_probe PRIVATE user32 gdi32)
target_link_options(mms_data_refresh_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_data_refresh_probe PROPERTIES OUTPUT_NAME data_refresh_probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/data_refresh/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/data_refresh/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_data_refresh_probe)
add_library(mms_dependency_refresh_probe MODULE EXCLUDE_FROM_ALL
    tests/plugins/dependency_refresh_plugin.cpp tests/plugins/dependency_refresh_plugin.def)
target_include_directories(mms_dependency_refresh_probe PRIVATE sdk/mms/include)
target_compile_features(mms_dependency_refresh_probe PRIVATE cxx_std_17)
target_compile_definitions(mms_dependency_refresh_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mms_dependency_refresh_probe PRIVATE user32)
target_link_options(mms_dependency_refresh_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_dependency_refresh_probe PROPERTIES OUTPUT_NAME dependency_refresh_probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/dependency_refresh/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/dependency_refresh/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_dependency_refresh_probe)
add_library(mms_subscription_refresh_probe MODULE EXCLUDE_FROM_ALL
    tests/plugins/subscription_refresh_plugin.cpp tests/plugins/subscription_refresh_plugin.def)
target_include_directories(mms_subscription_refresh_probe PRIVATE sdk/mms/include)
target_compile_features(mms_subscription_refresh_probe PRIVATE cxx_std_17)
target_compile_definitions(mms_subscription_refresh_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_subscription_refresh_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_subscription_refresh_probe PROPERTIES OUTPUT_NAME subscription_refresh_probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/subscription_refresh/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/subscription_refresh/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_subscription_refresh_probe)
add_library(mms_property_refresh_probe MODULE EXCLUDE_FROM_ALL
    tests/plugins/property_refresh_plugin.cpp tests/plugins/property_refresh_plugin.def)
target_include_directories(mms_property_refresh_probe PRIVATE sdk/mms/include)
target_compile_features(mms_property_refresh_probe PRIVATE cxx_std_17)
target_compile_definitions(mms_property_refresh_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_property_refresh_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_property_refresh_probe PROPERTIES OUTPUT_NAME property_refresh_probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/property_refresh/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/property_refresh/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_property_refresh_probe)
add_test(NAME ui_property_refresh_runtime COMMAND plugin_abi_tests --property-refresh-only "$<TARGET_FILE:mms_property_refresh_probe>")
set_tests_properties(ui_property_refresh_runtime PROPERTIES TIMEOUT 90)
add_library(mms_scoped_subscription_probe MODULE EXCLUDE_FROM_ALL
    tests/plugins/scoped_subscription_plugin.cpp tests/plugins/scoped_subscription_plugin.def)
target_include_directories(mms_scoped_subscription_probe PRIVATE sdk/mms/include)
target_compile_features(mms_scoped_subscription_probe PRIVATE cxx_std_17)
target_compile_definitions(mms_scoped_subscription_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_scoped_subscription_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_scoped_subscription_probe PROPERTIES OUTPUT_NAME scoped_subscription_probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/scoped_subscription/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/scoped_subscription/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_scoped_subscription_probe)
add_test(NAME ui_scoped_refresh_subscription_runtime COMMAND plugin_abi_tests --scoped-subscription-only "$<TARGET_FILE:mms_scoped_subscription_probe>")
set_tests_properties(ui_scoped_refresh_subscription_runtime PROPERTIES TIMEOUT 120)
add_library(mms_scoped_property_probe MODULE EXCLUDE_FROM_ALL
    tests/plugins/scoped_property_plugin.cpp tests/plugins/scoped_property_plugin.def)
target_include_directories(mms_scoped_property_probe PRIVATE sdk/mms/include)
target_compile_features(mms_scoped_property_probe PRIVATE cxx_std_17)
target_compile_definitions(mms_scoped_property_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_scoped_property_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_scoped_property_probe PROPERTIES OUTPUT_NAME scoped_property_probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/scoped_property/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/scoped_property/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(plugin_abi_tests mms_scoped_property_probe)
add_test(NAME ui_scoped_property_refresh_runtime COMMAND plugin_abi_tests --scoped-property-only "$<TARGET_FILE:mms_scoped_property_probe>")
set_tests_properties(ui_scoped_property_refresh_runtime PROPERTIES TIMEOUT 120)
add_library(mms_auto_refresh_example MODULE EXCLUDE_FROM_ALL
    sdk/mms/examples/auto_refresh/plugin.cpp sdk/mms/examples/auto_refresh/plugin.def)
target_include_directories(mms_auto_refresh_example PRIVATE sdk/mms/include)
target_compile_features(mms_auto_refresh_example PRIVATE cxx_std_17)
target_compile_definitions(mms_auto_refresh_example PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_compile_options(mms_auto_refresh_example PRIVATE /utf-8)
target_link_options(mms_auto_refresh_example PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_auto_refresh_example PROPERTIES OUTPUT_NAME auto_refresh
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/auto_refresh/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/auto_refresh/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_custom_command(TARGET mms_auto_refresh_example POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/mms/examples/auto_refresh/plugin.json" "$<TARGET_FILE_DIR:mms_auto_refresh_example>/plugin.json")
add_dependencies(plugin_abi_tests mms_auto_refresh_example)
add_test(NAME ui_auto_refresh_sdk_example COMMAND plugin_abi_tests --auto-refresh-example-only "$<TARGET_FILE:mms_auto_refresh_example>")
set_tests_properties(ui_auto_refresh_sdk_example PROPERTIES TIMEOUT 60)
add_test(NAME ui_dependency_refresh_runtime COMMAND plugin_abi_tests --dependency-refresh-only "$<TARGET_FILE:mms_dependency_refresh_probe>")
set_tests_properties(ui_dependency_refresh_runtime PROPERTIES TIMEOUT 180)
add_test(NAME ui_data_refresh_runtime COMMAND plugin_abi_tests --data-only "$<TARGET_FILE:mms_data_refresh_probe>")
set_tests_properties(ui_data_refresh_runtime PROPERTIES TIMEOUT 120)
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
add_test(NAME ui_input_state_runtime COMMAND plugin_abi_tests --input-state-only)
set_tests_properties(ui_input_state_runtime PROPERTIES TIMEOUT 90)
add_test(NAME ui_property_mount_runtime COMMAND plugin_abi_tests --property-mount-only)
set_tests_properties(ui_property_mount_runtime PROPERTIES TIMEOUT 90)
add_test(NAME ui_side_mount_runtime COMMAND plugin_abi_tests --side-mount-only)
set_tests_properties(ui_side_mount_runtime PROPERTIES TIMEOUT 90)
add_test(NAME ui_refresh_recovery_runtime COMMAND plugin_abi_tests --recovery-only)
set_tests_properties(ui_refresh_recovery_runtime PROPERTIES TIMEOUT 90)
if(TARGET side_mount_host_tests)
    add_library(mms_side_mount_host_probe MODULE EXCLUDE_FROM_ALL
        tests/plugins/side_mount_host_plugin.cpp tests/plugins/side_mount_host_plugin.def)
    target_include_directories(mms_side_mount_host_probe PRIVATE sdk/mms/include)
    target_compile_features(mms_side_mount_host_probe PRIVATE cxx_std_17)
    target_compile_definitions(mms_side_mount_host_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
    target_link_options(mms_side_mount_host_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
    set_target_properties(mms_side_mount_host_probe PROPERTIES OUTPUT_NAME side_mount_host_probe
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/side_mounts/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/side_mounts/$<CONFIG>"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_dependencies(side_mount_host_tests mms_side_mount_host_probe)
endif()
install(FILES PLUGINS/config.json DESTINATION PLUGINS)
