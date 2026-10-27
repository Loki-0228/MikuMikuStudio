# Native plugin ABI. This target has no renderer, CUDA, NVAPI or DX12 dependency.
enable_language(C)
add_subdirectory(third_party/native_ui)
add_library(mms_system_settings STATIC
    src/app/system_settings.cpp
    src/plugins/json.cpp)
target_include_directories(mms_system_settings PUBLIC include sdk/mms/include)
target_compile_definitions(mms_system_settings PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mms_system_settings PUBLIC user32)
add_library(mms_cache_policy STATIC src/io/cache_policy.cpp)
target_link_libraries(mms_cache_policy PUBLIC mms_system_settings)
add_library(mms_cache_manager STATIC src/io/cache_manager.cpp)
target_include_directories(mms_cache_manager PUBLIC include)
target_compile_definitions(mms_cache_manager PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_link_libraries(mms_cache_manager PUBLIC mms_cache_policy)
add_library(mms_cache_files STATIC src/io/cache_files.cpp)
target_include_directories(mms_cache_files PUBLIC include)
target_compile_definitions(mms_cache_files PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mms_cache_files PUBLIC bcrypt)
target_link_libraries(mikumikustudio_core PUBLIC mms_cache_files)
add_library(mms_cache_catalog STATIC src/io/cache_catalog.cpp src/io/cache_catalog_codec.cpp
    src/io/cache_catalog_intent.cpp)
target_link_libraries(mms_cache_catalog PUBLIC mms_cache_files)
target_compile_definitions(mms_cache_catalog PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_link_libraries(mikumikustudio_core PUBLIC mms_cache_catalog)
add_library(mms_cache_business STATIC
    src/io/cache_business_validation.cpp
    src/io/cache_business_page.cpp
    src/io/cache_business_manifest.cpp
    src/io/cache_business_reader.cpp)
target_link_libraries(mms_cache_business PUBLIC mms_cache_files mms_cache_catalog)
target_compile_definitions(mms_cache_business PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_link_libraries(mikumikustudio_core PUBLIC mms_cache_business)
foreach(business_test cache_business_codec_tests cache_business_allocation_tests cache_business_reader_tests)
    add_executable(${business_test} EXCLUDE_FROM_ALL tests/${business_test}.cpp)
    target_link_libraries(${business_test} PRIVATE mms_cache_business)
    target_compile_definitions(${business_test} PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
    target_link_options(${business_test} PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
    set_target_properties(${business_test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endforeach()
foreach(catalog_test cache_catalog_tests cache_catalog_recovery_tests cache_catalog_allocation_tests cache_catalog_budget_tests
        cache_catalog_intent_tests cache_catalog_owned_recovery_tests
        cache_catalog_maintenance_tests cache_catalog_maintenance_recovery_tests
        cache_catalog_maintenance_allocation_tests cache_catalog_pinned_tests)
    add_executable(${catalog_test} EXCLUDE_FROM_ALL tests/${catalog_test}.cpp)
    target_link_libraries(${catalog_test} PRIVATE mms_cache_catalog)
    target_compile_definitions(${catalog_test} PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
    target_link_options(${catalog_test} PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
    set_target_properties(${catalog_test} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endforeach()
target_link_libraries(cache_catalog_tests PRIVATE mms_cache_business)
add_executable(cache_prepared_files_tests EXCLUDE_FROM_ALL tests/cache_prepared_files_tests.cpp)
target_link_libraries(cache_prepared_files_tests PRIVATE mms_cache_files)
target_compile_definitions(cache_prepared_files_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_link_options(cache_prepared_files_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(cache_prepared_files_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_executable(cache_files_tests EXCLUDE_FROM_ALL tests/cache_files_tests.cpp)
target_link_libraries(cache_files_tests PRIVATE mms_cache_files)
target_compile_definitions(cache_files_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(cache_files_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(cache_files_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_executable(cache_files_fault_tests EXCLUDE_FROM_ALL tests/cache_files_fault_tests.cpp)
target_link_libraries(cache_files_fault_tests PRIVATE mms_cache_files)
target_compile_definitions(cache_files_fault_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(cache_files_fault_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(cache_files_fault_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
target_compile_definitions(mms_cache_policy PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mikumikustudio_core PUBLIC mms_cache_manager)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
add_executable(cache_manager_tests EXCLUDE_FROM_ALL tests/cache_manager_tests.cpp)
target_link_libraries(cache_manager_tests PRIVATE mms_cache_manager)
target_compile_definitions(cache_manager_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_link_options(cache_manager_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(cache_manager_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endif()
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
add_executable(cache_manager_ui_tests EXCLUDE_FROM_ALL
    tests/cache_manager_ui_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
target_link_libraries(cache_manager_ui_tests PRIVATE mikumikustudio_core minhook::minhook comctl32 user32)
target_link_options(cache_manager_ui_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
set_target_properties(cache_manager_ui_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endif()
add_executable(cache_manager_policy_tests EXCLUDE_FROM_ALL tests/cache_manager_policy_tests.cpp)
target_link_libraries(cache_manager_policy_tests PRIVATE mms_cache_policy)
target_compile_definitions(cache_manager_policy_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(cache_manager_policy_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(cache_manager_policy_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
# The policy tests require new, explicitly owned directories. The verification
# runner supplies them instead of a reusable CTest path beside user settings.
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
    src/plugins/render_service.cpp
    src/plugins/render_image_store.cpp
    src/plugins/evaluation_store.cpp
    src/plugins/ui_catalog.cpp
    src/plugins/native_ui_catalog.cpp
    src/plugins/native_menu_snapshot.cpp
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

add_executable(ui_catalog_tests EXCLUDE_FROM_ALL tests/ui_catalog_tests.cpp)
target_link_libraries(ui_catalog_tests PRIVATE mms_plugin_runtime)
set_target_properties(ui_catalog_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME ui_catalog_contracts COMMAND ui_catalog_tests)
set_tests_properties(ui_catalog_contracts PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)

add_executable(ui_catalog_sdk_tests EXCLUDE_FROM_ALL tests/ui_catalog_sdk_tests.cpp)
target_include_directories(ui_catalog_sdk_tests PRIVATE sdk/mms/include)
target_compile_features(ui_catalog_sdk_tests PRIVATE cxx_std_17)
add_executable(ui_catalog_c_layout EXCLUDE_FROM_ALL tests/ui_catalog_c_layout.c)
target_include_directories(ui_catalog_c_layout PRIVATE sdk/mms/include)
set_target_properties(ui_catalog_c_layout PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
foreach(catalog_target ui_catalog_tests ui_catalog_sdk_tests ui_catalog_c_layout)
    target_link_options(${catalog_target} PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
    set_target_properties(${catalog_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endforeach()
add_test(NAME ui_catalog_sdk_contracts COMMAND ui_catalog_sdk_tests)
add_test(NAME ui_catalog_c_layout_contract COMMAND ui_catalog_c_layout)
set_tests_properties(ui_catalog_sdk_contracts ui_catalog_c_layout_contract PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_library(mms_plugin_ui_catalog_probe SHARED EXCLUDE_FROM_ALL
    tests/plugins/ui_catalog_probe.cpp tests/plugins/ui_catalog_probe.def)
target_include_directories(mms_plugin_ui_catalog_probe PRIVATE sdk/mms/include)
target_compile_definitions(mms_plugin_ui_catalog_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_plugin_ui_catalog_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_plugin_ui_catalog_probe PROPERTIES PREFIX "" OUTPUT_NAME "ui_catalog_probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/ui_catalog/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_source.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_model_toon.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_texture_reload.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/model_physics_resources.cpp src/plugins/native_model_physics.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_curve_write.cpp src/plugins/native_curve_commands.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_morph_curve_write.cpp src/plugins/native_bone_curve_write.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_frame_lines.cpp src/plugins/native_key_paste.cpp src/plugins/native_vmd_import.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_global_paste.cpp src/plugins/native_gravity_paste.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_vmd_source.cpp src/plugins/native_model_vmd_source.cpp src/plugins/native_viewport_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_viewport_reset_camera.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_camera_hud_history.cpp src/plugins/native_global_key_delete.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_accessory_hud_history.cpp)
    target_sources(mikumikustudio_core PRIVATE src/plugins/native_accessory_parent_history.cpp)
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

add_executable(evaluation_time_tests EXCLUDE_FROM_ALL tests/evaluation_time_tests.cpp)
target_include_directories(evaluation_time_tests PRIVATE include)
target_compile_features(evaluation_time_tests PRIVATE cxx_std_17)
target_link_options(evaluation_time_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_time_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME evaluation_time_exact COMMAND evaluation_time_tests)
set_tests_properties(evaluation_time_exact PROPERTIES TIMEOUT 60)

add_executable(plugin_abi_tests EXCLUDE_FROM_ALL tests/plugins/runtime_tests.cpp tests/plugins/abi_layout.cpp)
add_test(NAME plugin_abi_panel_state COMMAND plugin_abi_tests --panel-state-only)
set_tests_properties(plugin_abi_panel_state PROPERTIES TIMEOUT 90)
add_test(NAME plugin_abi_menu_state COMMAND plugin_abi_tests --menu-state-only)
set_tests_properties(plugin_abi_menu_state PROPERTIES TIMEOUT 90)
add_executable(business_event_tests EXCLUDE_FROM_ALL tests/plugins/business_event_tests.cpp)
add_executable(render_callback_tests EXCLUDE_FROM_ALL tests/plugins/render_callback_tests.cpp src/plugins/render_service.cpp)
target_include_directories(render_callback_tests PRIVATE src/plugins sdk/mms/include)
target_link_options(render_callback_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(render_callback_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_render_callbacks COMMAND render_callback_tests)
set_tests_properties(plugin_abi_render_callbacks PROPERTIES TIMEOUT 30)
add_executable(render_lifecycle_tests EXCLUDE_FROM_ALL tests/plugins/render_lifecycle_tests.cpp)
add_executable(render_image_tests EXCLUDE_FROM_ALL tests/plugins/render_image_tests.cpp src/plugins/render_image_store.cpp)
target_include_directories(render_image_tests PRIVATE src/plugins sdk/mms/include)
target_compile_definitions(render_image_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(render_image_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(render_image_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_render_images COMMAND render_image_tests)
add_executable(evaluation_store_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_store_tests.cpp)
target_link_libraries(evaluation_store_tests PRIVATE mms_plugin_runtime)
target_include_directories(evaluation_store_tests PRIVATE src/plugins)
target_link_options(evaluation_store_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_store_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_evaluation_store COMMAND evaluation_store_tests)
add_executable(evaluation_channels_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_channels_tests.cpp)
target_link_libraries(evaluation_channels_tests PRIVATE mms_plugin_runtime)
target_include_directories(evaluation_channels_tests PRIVATE src/plugins)
target_link_options(evaluation_channels_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_channels_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_evaluation_channels COMMAND evaluation_channels_tests)
add_executable(evaluation_display_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_display_tests.cpp)
target_link_libraries(evaluation_display_tests PRIVATE mms_plugin_runtime)
target_include_directories(evaluation_display_tests PRIVATE src/plugins)
target_link_options(evaluation_display_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_display_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_evaluation_display COMMAND evaluation_display_tests)
add_executable(evaluation_parent_state_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_parent_state_tests.cpp)
target_link_libraries(evaluation_parent_state_tests PRIVATE mms_plugin_runtime)
target_include_directories(evaluation_parent_state_tests PRIVATE src/plugins)
target_link_options(evaluation_parent_state_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_parent_state_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_evaluation_parent_state COMMAND evaluation_parent_state_tests)
add_executable(evaluation_scene_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_scene_tests.cpp)
target_link_libraries(evaluation_scene_tests PRIVATE mms_plugin_runtime)
target_include_directories(evaluation_scene_tests PRIVATE src/plugins)
target_link_options(evaluation_scene_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_scene_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_evaluation_scene COMMAND evaluation_scene_tests)
add_executable(evaluation_cpp_sdk_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_cpp_sdk_tests.cpp)
target_include_directories(evaluation_cpp_sdk_tests PRIVATE sdk/mms/include)
target_compile_features(evaluation_cpp_sdk_tests PRIVATE cxx_std_17)
target_link_options(evaluation_cpp_sdk_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_cpp_sdk_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_evaluation_cpp_sdk COMMAND evaluation_cpp_sdk_tests)
add_library(mms_evaluation_lifecycle_plugin SHARED EXCLUDE_FROM_ALL
    tests/plugins/evaluation_lifecycle_plugin.cpp tests/plugins/evaluation_lifecycle_plugin.def)
target_include_directories(mms_evaluation_lifecycle_plugin PRIVATE sdk/mms/include)
target_compile_definitions(mms_evaluation_lifecycle_plugin PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_evaluation_lifecycle_plugin PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_evaluation_lifecycle_plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_executable(evaluation_lifecycle_tests EXCLUDE_FROM_ALL tests/plugins/evaluation_lifecycle_tests.cpp)
target_link_libraries(evaluation_lifecycle_tests PRIVATE mms_plugin_runtime)
target_include_directories(evaluation_lifecycle_tests PRIVATE src/plugins)
target_link_options(evaluation_lifecycle_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(evaluation_lifecycle_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(evaluation_lifecycle_tests mms_evaluation_lifecycle_plugin)
add_test(NAME plugin_abi_evaluation_lifecycle COMMAND evaluation_lifecycle_tests
    "${CMAKE_BINARY_DIR}/tests/evaluation-lifecycle" "$<TARGET_FILE:mms_evaluation_lifecycle_plugin>")
add_library(mms_evaluation_host_probe SHARED EXCLUDE_FROM_ALL tests/plugins/evaluation_host_probe.cpp)
target_include_directories(mms_evaluation_host_probe PRIVATE sdk/mms/include)
target_compile_definitions(mms_evaluation_host_probe PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_evaluation_host_probe PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_evaluation_host_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
set_tests_properties(plugin_abi_render_images PROPERTIES TIMEOUT 120)
add_executable(image_canvas_tests EXCLUDE_FROM_ALL tests/plugins/image_canvas_tests.cpp)
target_include_directories(image_canvas_tests PRIVATE sdk/mms/include)
target_link_options(image_canvas_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(image_canvas_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_image_canvas COMMAND image_canvas_tests)
add_executable(image_panel_tests EXCLUDE_FROM_ALL tests/plugins/image_panel_tests.cpp)
target_link_libraries(image_panel_tests PRIVATE mms_plugin_runtime)
target_include_directories(image_panel_tests PRIVATE src/plugins)
target_compile_definitions(image_panel_tests PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(image_panel_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(image_panel_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME plugin_abi_image_panel COMMAND image_panel_tests)
set_tests_properties(plugin_abi_image_canvas plugin_abi_image_panel PROPERTIES TIMEOUT 90)
add_library(mms_image_panel_example MODULE EXCLUDE_FROM_ALL
    sdk/mms/examples/image_panel/plugin.cpp sdk/mms/examples/image_panel/plugin.def)
target_include_directories(mms_image_panel_example PRIVATE sdk/mms/include)
target_compile_features(mms_image_panel_example PRIVATE cxx_std_17)
target_compile_definitions(mms_image_panel_example PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_libraries(mms_image_panel_example PRIVATE user32 gdi32)
target_link_options(mms_image_panel_example PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_image_panel_example PROPERTIES OUTPUT_NAME image_panel
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/image-panel/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/plugins/image-panel/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_executable(image_panel_example_tests EXCLUDE_FROM_ALL tests/plugins/image_panel_example_tests.cpp)
target_link_libraries(image_panel_example_tests PRIVATE mms_plugin_runtime)
target_include_directories(image_panel_example_tests PRIVATE src/plugins)
target_link_options(image_panel_example_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(image_panel_example_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(image_panel_example_tests mms_image_panel_example)
add_test(NAME plugin_abi_image_panel_example COMMAND image_panel_example_tests
    "${CMAKE_BINARY_DIR}/tests/image-panel-example" "$<TARGET_FILE:mms_image_panel_example>"
    "${CMAKE_SOURCE_DIR}/sdk/mms/examples/image_panel/plugin.json")
set_tests_properties(plugin_abi_image_panel_example PROPERTIES TIMEOUT 90)
add_library(mms_image_lifecycle_plugin SHARED EXCLUDE_FROM_ALL tests/plugins/image_lifecycle_plugin.cpp tests/plugins/image_lifecycle_plugin.def)
target_include_directories(mms_image_lifecycle_plugin PRIVATE sdk/mms/include)
target_compile_definitions(mms_image_lifecycle_plugin PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
target_link_options(mms_image_lifecycle_plugin PRIVATE /SUBSYSTEM:WINDOWS,6.01 /DEBUG:NONE)
set_target_properties(mms_image_lifecycle_plugin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_executable(image_lifecycle_tests EXCLUDE_FROM_ALL tests/plugins/image_lifecycle_tests.cpp)
target_link_libraries(image_lifecycle_tests PRIVATE mms_plugin_runtime)
target_include_directories(image_lifecycle_tests PRIVATE src/plugins)
target_link_options(image_lifecycle_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(image_lifecycle_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(image_lifecycle_tests mms_image_lifecycle_plugin)
add_test(NAME plugin_abi_image_lifecycle COMMAND image_lifecycle_tests
    "${CMAKE_BINARY_DIR}/tests/image-lifecycle" "$<TARGET_FILE:mms_image_lifecycle_plugin>")
set_tests_properties(plugin_abi_image_lifecycle PROPERTIES TIMEOUT 120)
add_executable(device_event_tests EXCLUDE_FROM_ALL tests/plugins/device_event_tests.cpp)
target_link_libraries(device_event_tests PRIVATE mms_plugin_runtime)
target_include_directories(device_event_tests PRIVATE src/plugins)
target_link_options(device_event_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(device_event_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(device_event_tests mms_lifecycle_plugin)
add_test(NAME plugin_abi_device_events COMMAND device_event_tests
    "${CMAKE_BINARY_DIR}/tests/device-events" "$<TARGET_FILE:mms_lifecycle_plugin>")
set_tests_properties(plugin_abi_device_events PROPERTIES TIMEOUT 120)
target_link_libraries(render_lifecycle_tests PRIVATE mms_plugin_runtime)
target_include_directories(render_lifecycle_tests PRIVATE src/plugins)
target_link_options(render_lifecycle_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(render_lifecycle_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(render_lifecycle_tests mms_lifecycle_plugin)
add_test(NAME plugin_abi_render_lifecycle COMMAND render_lifecycle_tests
    "${CMAKE_BINARY_DIR}/tests/render-lifecycle" "$<TARGET_FILE:mms_lifecycle_plugin>")
set_tests_properties(plugin_abi_render_lifecycle PROPERTIES TIMEOUT 90)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tests/host-events/PLUGINS/test")
file(WRITE "${CMAKE_BINARY_DIR}/tests/host-events/PLUGINS/test/plugin.json"
"{\"schemaVersion\":1,\"id\":\"org.example.host-events\",\"name\":\"host events probe\",\"version\":\"1.0.0\",\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},\"entry\":\"lifecycle.dll\",\"architecture\":\"x64\",\"minWindows\":\"6.1.7601\",\"capabilities\":[\"ui.command\"],\"requiresServices\":[{\"id\":\"mms.ui\",\"major\":1,\"minMinor\":0}],\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}")
add_executable(host_event_tests EXCLUDE_FROM_ALL tests/plugins/host_event_tests.cpp
    res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
target_include_directories(host_event_tests PRIVATE include)
target_link_options(host_event_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
target_link_libraries(host_event_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
target_link_options(host_event_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(host_event_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(host_event_tests mms_lifecycle_plugin)
add_custom_command(TARGET host_event_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:mms_lifecycle_plugin>"
        "${CMAKE_BINARY_DIR}/tests/host-events/PLUGINS/test/lifecycle.dll")
add_test(NAME plugin_abi_host_events COMMAND host_event_tests)
set_tests_properties(plugin_abi_host_events PROPERTIES TIMEOUT 120
    ENVIRONMENT "MMS_PLUGIN_TEST_DIR=${CMAKE_BINARY_DIR}/tests/host-events/PLUGINS;MMS_ABI_PROBE_LOG=${CMAKE_BINARY_DIR}/tests/host-events/plugin.log")
add_executable(frame_phase_tests EXCLUDE_FROM_ALL tests/plugins/frame_phase_tests.cpp
    res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
target_include_directories(frame_phase_tests PRIVATE include)
target_link_options(frame_phase_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}"
    /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
target_link_libraries(frame_phase_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
set_target_properties(frame_phase_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(frame_phase_tests mms_lifecycle_plugin)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
    add_test(NAME plugin_abi_frame_phases COMMAND frame_phase_tests
        "${CMAKE_BINARY_DIR}/tests/frame-phases" "$<TARGET_FILE:mms_lifecycle_plugin>")
    set_tests_properties(plugin_abi_frame_phases PROPERTIES TIMEOUT 120)
endif()
target_link_libraries(business_event_tests PRIVATE mms_plugin_runtime)
add_executable(device_lease_host_tests EXCLUDE_FROM_ALL tests/plugins/device_lease_host_tests.cpp
    res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
target_include_directories(device_lease_host_tests PRIVATE include)
target_link_options(device_lease_host_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}"
    /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
target_link_libraries(device_lease_host_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
set_target_properties(device_lease_host_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(device_lease_host_tests mms_lifecycle_plugin)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
    add_test(NAME plugin_abi_device_leases COMMAND device_lease_host_tests
        "${CMAKE_BINARY_DIR}/tests/device-leases" "$<TARGET_FILE:mms_lifecycle_plugin>")
    set_tests_properties(plugin_abi_device_leases PROPERTIES TIMEOUT 120)
endif()
target_include_directories(business_event_tests PRIVATE src/plugins)
target_link_options(business_event_tests PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DEBUG:NONE)
set_target_properties(business_event_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_dependencies(business_event_tests mms_lifecycle_plugin)
add_test(NAME plugin_abi_business_events COMMAND business_event_tests
    "${CMAKE_BINARY_DIR}/tests/business-events"
    "${CMAKE_BINARY_DIR}/tests/plugins/lifecycle/$<CONFIG>/lifecycle.dll")
set_tests_properties(plugin_abi_business_events PROPERTIES TIMEOUT 90)
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
