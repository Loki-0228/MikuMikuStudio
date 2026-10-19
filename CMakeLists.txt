cmake_minimum_required(VERSION 3.21)

project(MikuMikuStudio LANGUAGES CXX)

# ---------------------------------------------------------------------------
# MikuMikuStudio: behavioural restoration of MikuMikuDance v932.
# MikuMikuDanceE_v932x64 is the x64 reference.  The x86 binary remains a
# source-recovery aid only.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/LegacyOptions.cmake)

set(MIKUMIKUSTUDIO_PROFILE "full" CACHE STRING "Build profile: full or mmd932")
set_property(CACHE MIKUMIKUSTUDIO_PROFILE PROPERTY STRINGS full mmd932)
if(NOT MIKUMIKUSTUDIO_PROFILE STREQUAL "full" AND NOT MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
    message(FATAL_ERROR "MIKUMIKUSTUDIO_PROFILE must be full or mmd932")
endif()
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
    add_compile_definitions(MMD_ORIGINAL_PROFILE=1)
    set(MIKUMIKUSTUDIO_ENABLE_DX12 OFF CACHE BOOL "Disabled in the original-compatible profile" FORCE)
else()
    add_compile_definitions(MMD_ORIGINAL_PROFILE=0)
endif()

option(MIKUMIKUSTUDIO_ENABLE_DX12
    "Build optional DX12 skinning/native preview; OFF keeps the DX9/MME renderer with CPU skinning"
    ON)

option(MIKUMIKUSTUDIO_DEBUG_SYMBOLS "Generate separate Release PDB files" ON)
option(MIKUMIKUSTUDIO_WIN7_BASELINE "Require the pinned Windows 7 x64 CPU baseline" OFF)
if(MIKUMIKUSTUDIO_WIN7_BASELINE)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
       NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "19.44.35229.0")
        message(FATAL_ERROR "Win7 baseline requires x64 MSVC 19.44.35229 (VS 2022 14.44.35207 toolset directory)")
    endif()
    if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION STREQUAL "10.0.19041.0")
        message(FATAL_ERROR "Win7 baseline requires Windows SDK 10.0.19041.0")
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "/arch:(AVX|AVX2|AVX512)" OR CMAKE_CXX_FLAGS_RELEASE MATCHES "/arch:(AVX|AVX2|AVX512)")
        message(FATAL_ERROR "Win7 CPU baseline cannot require AVX instructions")
    endif()
    set(MIKUMIKUSTUDIO_ENABLE_DX12 OFF CACHE BOOL "Win7 baseline uses DX9 and CPU skinning" FORCE)
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
endif()

if (MSVC)
    # Sources contain UTF-8 comments and literals; do not use the host code page.
    add_compile_options(/W3 /permissive- /utf-8)
    # Keep symbols for crash dumps without disabling Release optimization.
    # Embed compiler symbols in objects to avoid shared-PDB C1902 failures.
    add_compile_options($<$<CONFIG:Release>:/Z7>)
    if(MIKUMIKUSTUDIO_DEBUG_SYMBOLS)
        add_link_options($<$<CONFIG:Release>:/DEBUG:FULL>)
    else()
        add_link_options($<$<CONFIG:Release>:/DEBUG:NONE>)
    endif()
    add_link_options($<$<CONFIG:Release>:/OPT:REF>
                     $<$<CONFIG:Release>:/OPT:ICF>)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # The original binary tolerates implicit narrowing everywhere; keep the
    # port close to VC9 behaviour rather than modern strictness.
    add_compile_options(/wd4267 /wd4244 /wd4245)
    # VC9 had no C++17 aligned operator new/delete (P0035).  Without this
    # switch our TUs allocate over-aligned Bullet objects (e.g. the 1456-byte
    # btGeneric6DofSpringConstraint, align 16) through _aligned_malloc while
    # the bullet275 static library's deleting destructors free them with the
    # plain operator delete - free() on an _aligned_malloc block, which
    # corrupts the heap and kills the process at teardown (0xC0000374,
    # see ShutdownCleanup -> DisposePhysicsWorld).  The original shipped
    # exactly the plain-new pairing, so this is also the faithful setting.
    add_compile_options(/Zc:alignedNew-)
    add_compile_options(/Zc:offsetof-)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()


if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS
        "Configuring the x64 build (reference: MikuMikuDanceE_v932x64, "
        "VC9 SSE2, d3dx9_43).")
endif()

# ---------------------------------------------------------------------------
# Output layout.  The per-configuration runtime directory is the shipped
# directory and must contain the program only, so build intermediates are
# redirected out of it:
#   <build>/Release            MikuMikuDance.exe (+ Data\ runtime assets)
#   <build>/lib/<config>       static libraries and import libraries
#   <build>/symbols/<config>   every PDB, for crash triage / debugging
# ---------------------------------------------------------------------------
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/symbols)
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/symbols)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/Release" CACHE PATH
        "MMS release package directory" FORCE)
endif()

if (NOT WIN32)
    message(FATAL_ERROR "MikuMikuStudio targets Windows only (Win32/DirectX9).")
endif()

# ---------------------------------------------------------------------------
# Dependencies (provisioned by Conan; see conanfile.py / recipes/)
# ---------------------------------------------------------------------------
find_package(BULLET275 REQUIRED)
find_package(Alembic CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa/SMAA.hlsl" MMS_SMAA_REFERENCE)
string(ASCII 183 MMS_SMAA_LATIN1_DOT)
string(REPLACE "${MMS_SMAA_LATIN1_DOT}" "·" MMS_SMAA_REFERENCE "${MMS_SMAA_REFERENCE}")
string(REPLACE "\n" "\n)smaa\"\nR\"smaa(" MMS_SMAA_REFERENCE "${MMS_SMAA_REFERENCE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS third_party/smaa/SMAA.hlsl)
configure_file(src/render/smaa_reference.in.hpp generated/smaa_reference.hpp @ONLY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fxaa/FXAA.hlsl" MMS_FXAA_REFERENCE)
string(REPLACE "\n" "\n)fxaa\"\nR\"fxaa(" MMS_FXAA_REFERENCE "${MMS_FXAA_REFERENCE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS third_party/fxaa/FXAA.hlsl)
configure_file(src/render/fxaa_reference.in.hpp generated/fxaa_reference.hpp @ONLY)

if (MIKUMIKUSTUDIO_ENABLE_DX12)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/render/d3d12_skinning.hlsl" MMD_D3D12_SKINNING_HLSL)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/render/d3d12_skinning.hlsl)
    configure_file(src/render/d3d12_skinning_shader.hpp.in generated/d3d12_skinning_shader.hpp @ONLY)
endif()
add_library(mikumikustudio_graphics_runtime STATIC src/render/graphics_runtime.cpp)
target_include_directories(mikumikustudio_graphics_runtime PUBLIC include)

add_library(mikumikustudio_d3d12 STATIC src/render/d3d12_skinning.cpp)
target_include_directories(mikumikustudio_d3d12 PUBLIC include)
target_link_libraries(mikumikustudio_d3d12 PUBLIC mikumikustudio_graphics_runtime)
if (MIKUMIKUSTUDIO_ENABLE_DX12)
    target_include_directories(mikumikustudio_d3d12 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    # Optional runtime APIs must never become startup imports in the main EXE.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/render/d3d12_native.hlsl" MMD_D3D12_NATIVE_HLSL)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/render/d3d12_sort.hlsl" MMD_D3D12_SORT_HLSL)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/render/d3d12_native.hlsl src/render/d3d12_sort.hlsl)
    configure_file(src/render/d3d12_native_shaders.hpp.in generated/d3d12_native_shaders.hpp @ONLY)
    target_compile_definitions(mikumikustudio_d3d12 PRIVATE MMD_ENABLE_DX12=1)
else()
    target_compile_definitions(mikumikustudio_d3d12 PRIVATE MMD_ENABLE_DX12=0)
endif()

# In-memory runtime diagnostics: no log file, no dump - a fatal fault renders
# the recent message ring into a modal report (see src/main/runtime_log.cpp).
# user32 supplies the MessageBoxW that keeps the process alive until the user
# acknowledges the report.
add_library(mikumikustudio_runtime_log STATIC src/main/runtime_log.cpp)
target_include_directories(mikumikustudio_runtime_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mikumikustudio_runtime_log PUBLIC user32)

# ---------------------------------------------------------------------------
# Core static library: application state, math, startup chain, effect API.
# ---------------------------------------------------------------------------
add_library(mikumikustudio_animation_curve STATIC
    src/animation/animation_curve.cpp src/animation/animation_curve_insert.cpp src/animation/animation_retime.cpp)
target_include_directories(mikumikustudio_animation_curve PUBLIC include)
add_library(mikumikustudio_curve_motion STATIC src/animation/animation_curve_motion.cpp)
target_include_directories(mikumikustudio_curve_motion PUBLIC include)
target_link_libraries(mikumikustudio_curve_motion PUBLIC mikumikustudio_animation_curve)
add_library(mikumikustudio_curve_editor STATIC src/window/animation_curve_editor.cpp)
target_include_directories(mikumikustudio_curve_editor PUBLIC include)
target_link_libraries(mikumikustudio_curve_editor PUBLIC mikumikustudio_animation_curve user32 gdi32 comctl32 gdiplus)
add_executable(animation_curve_tests EXCLUDE_FROM_ALL tests/animation_curve_tests.cpp)
target_link_libraries(animation_curve_tests PRIVATE mikumikustudio_animation_curve)
set_target_properties(animation_curve_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
enable_testing()
foreach(_case IN ITEMS legacy override fresh)
    set(_legacy_options)
    if(NOT _case STREQUAL "fresh")
        list(APPEND _legacy_options -DMIKUDANCESTUDIO_PROFILE=mmd932
            -DMIKUDANCESTUDIO_ENABLE_DX12=OFF -DMIKUDANCESTUDIO_WIN7_BASELINE=ON)
    endif()
    if(_case STREQUAL "override")
        list(APPEND _legacy_options -DMIKUMIKUSTUDIO_PROFILE=full
            -DMIKUMIKUSTUDIO_ENABLE_DX12=ON -DMIKUMIKUSTUDIO_WIN7_BASELINE=OFF)
    endif()
    add_test(NAME legacy_build_options_${_case} COMMAND ${CMAKE_COMMAND}
        -DCASE=${_case} ${_legacy_options} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_cmake_options_tests.cmake)
endforeach()

add_executable(native_shadow_tests EXCLUDE_FROM_ALL tests/native_shadow_tests.cpp)
target_include_directories(native_shadow_tests PRIVATE include)
target_link_libraries(native_shadow_tests PRIVATE d3d9 user32)
set_target_properties(native_shadow_tests PROPERTIES
    CXX_STANDARD 17 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")

add_test(NAME native_shadow_tests COMMAND native_shadow_tests "${CMAKE_CURRENT_SOURCE_DIR}/res/assets/skin_effect_sm3.fx")
add_test(NAME native_shadow_sm2_coverage COMMAND native_shadow_tests "${CMAKE_CURRENT_SOURCE_DIR}/res/assets/skin_effect_sm2.fx" --coverage-only)
add_test(NAME animation_curve_tests COMMAND animation_curve_tests)
add_executable(animation_retime_tests EXCLUDE_FROM_ALL tests/animation_retime_tests.cpp)
target_link_libraries(animation_retime_tests PRIVATE mikumikustudio_animation_curve)
set_target_properties(animation_retime_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME animation_retime_tests COMMAND animation_retime_tests)
add_executable(frame_rate_dialog_tests EXCLUDE_FROM_ALL
    tests/frame_rate_dialog_tests.cpp src/window/frame_rate_dialog.cpp src/app/app_ctor.cpp)
target_include_directories(frame_rate_dialog_tests PRIVATE include)
target_link_libraries(frame_rate_dialog_tests PRIVATE user32 gdi32 comctl32)
set_target_properties(frame_rate_dialog_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME frame_rate_dialog_tests COMMAND frame_rate_dialog_tests)
add_executable(frame_rate_correction_tests EXCLUDE_FROM_ALL
    tests/frame_rate_correction_tests.cpp src/animation/frame_rate_correction.cpp src/app/app_ctor.cpp)
target_include_directories(frame_rate_correction_tests PRIVATE include)
target_link_libraries(frame_rate_correction_tests PRIVATE mikumikustudio_animation_curve user32 gdi32 comctl32)
set_target_properties(frame_rate_correction_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME frame_rate_correction_tests COMMAND frame_rate_correction_tests)
add_executable(animation_curve_host_tests EXCLUDE_FROM_ALL tests/animation_curve_host_tests.cpp)
add_executable(bone_curve_tests EXCLUDE_FROM_ALL tests/bone_curve_tests.cpp)
target_link_libraries(bone_curve_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(bone_curve_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME bone_curve_tests COMMAND bone_curve_tests)
target_link_libraries(animation_curve_host_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(animation_curve_host_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME animation_curve_host_tests COMMAND animation_curve_host_tests)
add_executable(animation_curve_editor_tests EXCLUDE_FROM_ALL tests/animation_curve_editor_tests.cpp)
target_link_libraries(animation_curve_editor_tests PRIVATE mikumikustudio_curve_editor)
set_target_properties(animation_curve_editor_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME animation_curve_editor_tests COMMAND animation_curve_editor_tests)
add_executable(animation_curve_sidebar_tests EXCLUDE_FROM_ALL tests/animation_curve_sidebar_tests.cpp)
target_link_libraries(animation_curve_sidebar_tests PRIVATE mikumikustudio_curve_editor)
set_target_properties(animation_curve_sidebar_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME animation_curve_sidebar_tests COMMAND animation_curve_sidebar_tests)
add_executable(animation_curve_motion_tests EXCLUDE_FROM_ALL tests/animation_curve_motion_tests.cpp)
target_link_libraries(animation_curve_motion_tests PRIVATE mikumikustudio_curve_motion)
set_target_properties(animation_curve_motion_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME animation_curve_motion_tests COMMAND animation_curve_motion_tests)
add_executable(curve_comparison_tests EXCLUDE_FROM_ALL tests/curve_comparison_tests.cpp)
target_link_libraries(curve_comparison_tests PRIVATE mikumikustudio_animation_curve)
set_target_properties(curve_comparison_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME curve_comparison_tests COMMAND curve_comparison_tests)
add_executable(morph_curve_tests EXCLUDE_FROM_ALL tests/morph_curve_tests.cpp)
target_link_libraries(morph_curve_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(morph_curve_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME morph_curve_tests COMMAND morph_curve_tests)
add_executable(native_frame_retime_tests EXCLUDE_FROM_ALL tests/native_frame_retime_tests.cpp)
target_link_libraries(native_frame_retime_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(native_frame_retime_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME native_frame_retime_tests COMMAND native_frame_retime_tests)
add_executable(morph_curve_host_ui_tests EXCLUDE_FROM_ALL tests/morph_curve_host_ui_tests.cpp)
target_link_libraries(morph_curve_host_ui_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(morph_curve_host_ui_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME morph_curve_host_ui_tests COMMAND morph_curve_host_ui_tests)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
    add_executable(curve_sync_tests EXCLUDE_FROM_ALL tests/curve_sync_tests.cpp)
    target_link_libraries(curve_sync_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(curve_sync_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME curve_sync_tests COMMAND curve_sync_tests)
endif()
add_executable(bone_scale_curve_tests EXCLUDE_FROM_ALL tests/bone_scale_curve_tests.cpp)
target_link_libraries(bone_scale_curve_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(bone_scale_curve_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME bone_scale_curve_tests COMMAND bone_scale_curve_tests)

add_library(mikumikustudio_geometry_workers STATIC src/model/geometry_workers.cpp)
target_include_directories(mikumikustudio_geometry_workers PUBLIC include)
if (MSVC)
    target_compile_options(mikumikustudio_geometry_workers PRIVATE /openmp)
    # VCOMP reads OMP_WAIT_POLICY during DLL load, before WinMain for a normal
    # import. Delay loading lets the host configure its default first. Publish
    # this to every core consumer, including the independent frame benchmark.
    # Consumers that do not pull an OpenMP symbol have no vcomp import; keep
    # the optional delay-load for the ones that do and silence LNK4199 there.
    target_link_options(mikumikustudio_geometry_workers INTERFACE /DELAYLOAD:vcomp140.dll /IGNORE:4199)
    target_link_libraries(mikumikustudio_geometry_workers PUBLIC delayimp)
endif()

add_library(mikumikustudio_core STATIC
    src/animation/animation_curve_host.cpp
    src/animation/native_frame_retime.cpp
    src/animation/frame_rate_correction.cpp
    src/animation/morph_curve.cpp
    src/animation/sequence_scatter.cpp
    src/window/sequence_scatter_dialog.cpp
    src/animation/bone_curve.cpp
    src/window/frame_rate_dialog.cpp
    src/window/animation_curve_host_ui.cpp
    src/render/native_mesh.cpp
    src/mmv4mms/host.cpp
    src/window/plugin_menu.cpp
    src/app/mmd_app.cpp
    src/app/globals.cpp
    src/app/app_ctor.cpp
    src/app/app_defaults.cpp
    src/main/charset_conv.cpp
    src/main/text_encoding.cpp
    src/main/model_name_translation.cpp
    src/window/unicode_window.cpp
    src/math/quat_to_mat3x4.cpp
    src/math/color_lerp.cpp
    src/math/kernel_gaps.cpp
    src/window/path_utils.cpp
    src/window/window_init.cpp
    src/window/ui_init.cpp
    src/window/ui_font.cpp
    src/window/wndproc.cpp
    src/window/localize_ui.cpp
    src/window/ui_translation.cpp
    src/window/ui_message.cpp
    src/window/wndproc_stubs.cpp
    src/window/command_dispatch.cpp
    src/window/frame_line_edit.cpp
    src/window/accessory_paste.cpp
    src/media/wave_audio.cpp
    src/media/native_media.cpp
    src/media/native_video.cpp
    src/media/wic_mov_recorder.cpp
    src/render/transparent_capture.cpp
    src/media/media_load.cpp
    src/window/command_impl.cpp
    src/app/frame_driver.cpp
    src/app/playback_catchup.cpp
    src/app/playback_state.cpp
    src/app/timeline_advance.cpp
    src/app/frame_modes.cpp
    src/app/frame_modes_bone.cpp
    src/app/key_ladder.cpp
    src/app/keyboard_input.cpp
    src/app/pump_edit_keys.cpp
    src/app/pump_navigation.cpp
    src/physics/physics_create.cpp
    src/physics/physics_scale.cpp
    src/physics/physics_frame.cpp
    src/animation/channel_bake.cpp
    src/model/bone_key_pool.cpp
    src/physics/physics_bake.cpp
    src/physics/physics_bake_settings.cpp
    src/io/alembic_export.cpp
    src/io/mesh_bake_cache.cpp
    src/render/deformed_mesh_capture.cpp
    src/render/vertex_capture.cpp
    src/window/alembic_export_dialog.cpp
    src/physics/scene_create.cpp
    src/render/d3d_init.cpp
    src/render/default_effect.cpp
    src/render/processor_settings.cpp
    src/render/render_states.cpp
    src/render/stereo_nvapi.cpp
    src/render/frame_scene.cpp
    src/render/frame_presentation.cpp
    src/render/frame_rate.cpp
    src/render/sprite_overlay.cpp
    src/render/viewport_cursor_style.cpp
    src/render/overlay_producers.cpp
    src/render/capture_downsample.cpp
    src/render/debug_geometry.cpp
    src/render/accessory.cpp
    src/render/model_renderers.cpp
    src/render/adaptive_shadow.cpp
    src/render/shadow_filter.cpp
    src/exports/effect_display.cpp
    src/render/transparent_geometry.cpp
    src/render/transparent_peeling.cpp
    src/render/bg_overlay.cpp
    src/render/device_reset.cpp
    src/app/avi_record_start.cpp
    src/app/record_readback.cpp
    src/media/image_sequence_export.cpp
    src/window/image_sequence_dialog.cpp
    src/app/dshow_record_graph.cpp
    src/app/mf_h264_recorder.cpp
    src/model/model_init.cpp
    src/model/path_resolve.cpp
    src/model/pmd_load.cpp
    src/model/bone_scale.cpp
    src/model/bone_transform.cpp
    src/model/model_keyframe_advance.cpp
    src/model/model_keyframe_edit.cpp
    src/model/bone_edit_undo.cpp
    src/model/model_frame_seek.cpp
    src/model/vmd_load.cpp
    src/model/track_apply.cpp
    src/model/key_registrars.cpp
    src/model/morph_apply.cpp
    src/model/model_skinning.cpp
    src/model/model_vertex_cache.cpp
    src/model/skinning_backend.cpp
    src/window/skinning_menu.cpp
    src/window/recent_projects.cpp
    src/app/autosave.cpp
    src/window/autosave_dialog.cpp
    src/window/viewport_render_scale_dialog.cpp
    src/render/viewport_render_scale.cpp
    src/render/antialias.cpp
    src/render/outline_multisample.cpp
    src/render/frame_sampling.cpp
    src/render/antialias_motion.cpp
    src/render/nvidia_antialias_fix.cpp
    src/window/nvidia_antialias_menu.cpp
    src/window/antialias_menu.cpp
    src/app/viewport_corner_settings.cpp
    src/window/viewport_corner_menu.cpp
    src/window/physics_bake_dialog.cpp
    src/model/pmx_load.cpp
    src/model/add_model.cpp
    src/model/model_query_gaps.cpp
    src/model/model_dispose.cpp
    src/model/bone_sort.cpp
    src/model/post_load_init.cpp
    src/render/fill_panel.cpp
    src/window/wm_paint.cpp
    src/window/ui_timeline_gfx.cpp
    src/window/ui_control_text.cpp
    src/window/ui_ctlcolor.cpp
    src/window/ui_edit_commit.cpp
    src/window/dialog_procs.cpp
    src/window/frame_range_apply.cpp
    src/io/pmm_save.cpp
    src/io/pmm_extension.cpp
    src/io/bone_scale_io.cpp
    src/io/pmm_project_settings.cpp
    src/io/extended_names.cpp
    src/io/save_compatibility.cpp
    src/io/io_gaps.cpp
    src/io/vpd_file.cpp
    src/io/vpd_scale_io.cpp
    src/io/accessory_motion_io.cpp
    src/io/vmd_save.cpp
    src/io/pmm_load_shell.cpp
    src/io/pmm_load_v2.cpp
    src/io/pmm_load_v1.cpp
    src/window/ui_scroll_mouse.cpp
    src/window/ui_panel_paint.cpp
    src/window/ui_refresh.cpp
    src/window/ui_notify.cpp
    src/window/ui_hscroll.cpp
    src/window/ui_dropfiles.cpp
    src/window/ui_panel_sweep.cpp
      src/window/ui_view_refresh.cpp
      src/window/ui_model_reload.cpp
      src/window/ui_selection_reeval.cpp
    src/window/ui_editor_click.cpp
    src/window/ui_frame_step.cpp
    src/window/ui_frame_refresh.cpp
    src/window/ui_mousemove.cpp
    src/window/ui_windowsize.cpp
    src/window/mic_window.cpp
    src/app/subsystem_init.cpp
    src/app/reset_app_state.cpp
    src/app/scene_ownership.cpp
    src/app/app_gap_bodies.cpp
    src/app/shutdown_cleanup.cpp
    src/app/oni_kinect.cpp
    src/app/oni_skeleton_pump.cpp
    src/io/vsq_load.cpp
    src/window/dialog_select_ops.cpp
    src/window/ui_viewport_refresh.cpp
    src/window/ui_viewport_layout.cpp
    src/window/ui_mouse_misc.cpp
    src/window/ui_palette.cpp
    src/window/command_file_menu.cpp
    src/window/command_view_menu.cpp
    src/window/physics_model_dialog.cpp
    src/window/model_edge_dialog.cpp
    src/window/enhance_model_io.cpp
    src/window/misc_dialogs.cpp
    src/window/command_control_400.cpp
    src/window/command_control_450.cpp
    src/window/command_control_500.cpp
    src/features/dialog_gaps.cpp
    src/features/query_audio_gaps.cpp
    src/features/subsystem_gaps.cpp
    src/render/draw_glyph.cpp
    src/render/toon_textures.cpp
    src/render/scene_font.cpp
    src/exports/effect_api.cpp
    src/exports/effect_names.cpp
    src/exports/effect_paths.cpp
    src/exports/effect_apng.cpp
    src/exports/effect_file_watch.cpp
    src/unported/stubs.cpp
)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
    get_target_property(MMD_ORIGINAL_CORE_SOURCES mikumikustudio_core SOURCES)
    list(REMOVE_ITEM MMD_ORIGINAL_CORE_SOURCES
        src/animation/sequence_scatter.cpp src/window/sequence_scatter_dialog.cpp
        src/model/bone_scale.cpp src/physics/physics_scale.cpp
        src/animation/animation_curve_host.cpp src/animation/morph_curve.cpp src/animation/bone_curve.cpp
        src/window/animation_curve_host_ui.cpp src/window/frame_rate_dialog.cpp
        src/animation/frame_rate_correction.cpp src/animation/native_frame_retime.cpp
        src/window/physics_bake_dialog.cpp src/physics/physics_bake.cpp
        src/physics/physics_bake_settings.cpp src/animation/channel_bake.cpp
        src/mmv4mms/host.cpp src/render/native_mesh.cpp
        src/render/deformed_mesh_capture.cpp src/render/vertex_capture.cpp
        src/io/alembic_export.cpp src/io/mesh_bake_cache.cpp
        src/window/alembic_export_dialog.cpp
        src/render/transparent_geometry.cpp src/render/transparent_peeling.cpp
        src/media/native_media.cpp src/media/native_video.cpp src/media/wic_mov_recorder.cpp src/app/mf_h264_recorder.cpp)
    set_property(TARGET mikumikustudio_core PROPERTY SOURCES "${MMD_ORIGINAL_CORE_SOURCES}")
    target_sources(mikumikustudio_core PRIVATE
        src/model/bone_scale_original.cpp src/physics/physics_scale_original.cpp
        src/animation/animation_extensions_original.cpp src/window/animation_ui_original.cpp
        src/mmv4mms/host_original.cpp src/render/mesh_extensions_original.cpp
        src/media/native_media_original.cpp)
    add_executable(original_profile_io_tests EXCLUDE_FROM_ALL tests/original_profile_io_tests.cpp)
    target_link_libraries(original_profile_io_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(original_profile_io_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME original_profile_io COMMAND original_profile_io_tests)
    add_executable(original_profile_state_tests EXCLUDE_FROM_ALL tests/original_profile_state_tests.cpp)
    target_link_libraries(original_profile_state_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(original_profile_state_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME original_profile_state COMMAND original_profile_state_tests)
    add_executable(original_pmd_save_tests EXCLUDE_FROM_ALL tests/original_pmd_save_tests.cpp)
    target_link_libraries(original_pmd_save_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(original_pmd_save_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME original_pmd_save COMMAND original_pmd_save_tests)
endif()

add_executable(original_import_tests EXCLUDE_FROM_ALL tests/original_import_tests.cpp)
target_link_libraries(original_import_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(original_import_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME original_import COMMAND original_import_tests)

add_executable(pmd_index_save_tests EXCLUDE_FROM_ALL tests/pmd_index_save_tests.cpp)
target_link_libraries(pmd_index_save_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(pmd_index_save_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME pmd_index_save COMMAND pmd_index_save_tests)

option(MIKUMIKUSTUDIO_PERF "Enable render-thread phase timing for benchmarks" OFF)
if (MIKUMIKUSTUDIO_PERF)
    target_compile_definitions(mikumikustudio_core PUBLIC MIKUMIKUSTUDIO_PERF=1)
endif()

target_include_directories(mikumikustudio_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(mikumikustudio_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Pinned optional plugin ABI; the host does not build or link MMV4MMS.
target_include_directories(mikumikustudio_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/mmv4mms/include")
# Bullet 2.75 selects its Windows SIMD alignment with WIN32, not _WIN32.
# Keep consumers ABI-compatible even when custom CMAKE_CXX_FLAGS replace
# CMake's default /DWIN32 (for example, AddressSanitizer builds).
target_compile_definitions(mikumikustudio_core PUBLIC WIN32)
target_sources(mikumikustudio_core PRIVATE src/render/d3d12_native.cpp)
if (MIKUMIKUSTUDIO_ENABLE_DX12)
    target_include_directories(mikumikustudio_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(mikumikustudio_core PRIVATE MMD_ENABLE_DX12=1)
else()
    target_compile_definitions(mikumikustudio_core PRIVATE MMD_ENABLE_DX12=0)
endif()
target_link_libraries(mikumikustudio_core PUBLIC mikumikustudio_d3d12)
if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
    target_link_libraries(mikumikustudio_core PUBLIC mikumikustudio_curve_editor mikumikustudio_curve_motion Alembic::Alembic
        mfplat mfreadwrite mfuuid windowscodecs)
else()
    set_target_properties(mikumikustudio_animation_curve mikumikustudio_curve_editor mikumikustudio_curve_motion
        PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
target_link_libraries(mikumikustudio_core PUBLIC
    mikumikustudio_geometry_workers
    mikumikustudio_runtime_log
    BULLET275::BULLET275
    minhook::minhook
    bcrypt      # SHA256 allowlist for native MME APNG reset compatibility
    user32      # RegisterClass / CreateWindow / MessageBox
    gdi32       # CreateSolidBrush / GetStockObject
    msimg32     # GradientFill (0x40DF10 panel fill)
    comctl32    # InitCommonControls
    shell32     # DragAcceptFiles
    d3d9        # IDirect3DDevice9 (device slot at sub1d574+120032)
    vfw32       # DrawDibDraw (0x4168D0) + AVIFile*/AVIStream* (0x433250 background AVI)
    avifil32    # AVIFile/AVIStream background playback (0x4168D0)
    ole32       # CoInitialize/CoUninitialize (0x467313 / 0x4096C0)
    oleaut32    # VariantCopy/VariantClear/SysAllocString (encoder settings window)
    strmiids    # DirectShow IIDs/CLSIDs (recording graph 0x408F20..0x409A80)
    uuid        # IID_IPropertyBag / IID_IAMVfwCompressDialogs etc.
)
if (MSVC)
    # Align the PMX skinning stride workers with the original's VCOMP90
    # parallel regions (x64 _vcomp_fork, default static schedule).
    target_compile_options(mikumikustudio_core PRIVATE /openmp)
    target_compile_options(mikumikustudio_core PRIVATE /Zc:offsetof-)
endif()

add_executable(geometry_workers_tests EXCLUDE_FROM_ALL
    tests/geometry_workers_tests.cpp)
target_link_libraries(geometry_workers_tests PRIVATE mikumikustudio_geometry_workers)
set_target_properties(geometry_workers_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
if (MSVC)
    target_compile_options(geometry_workers_tests PRIVATE /openmp)
endif()
add_test(NAME geometry_workers_default COMMAND geometry_workers_tests default 12)
add_test(NAME geometry_workers_thread_limit COMMAND geometry_workers_tests default 3)
add_test(NAME geometry_workers_active_override COMMAND geometry_workers_tests ACTIVE 12)
add_test(NAME geometry_workers_passive_override COMMAND geometry_workers_tests PASSIVE 12)

add_executable(bone_rotation_drag_tests EXCLUDE_FROM_ALL tests/bone_rotation_drag_tests.cpp)
target_link_libraries(bone_rotation_drag_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(bone_rotation_drag_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME bone_rotation_drag COMMAND bone_rotation_drag_tests)

# ---------------------------------------------------------------------------
# d3dx9_43 import stubs - MikuMikuEffect (MME) compatibility
# ---------------------------------------------------------------------------
# MME's MMHack.dll hooks the host by rewriting the HOST's import address
# table: it resolves each D3DX9 entry point with GetProcAddress, walks the
# host module's IMAGE_DIRECTORY_ENTRY_IAT and VirtualProtects every slot bound
# to that address.  The port resolves D3DX at runtime (include/.../d3dx_dyn.hpp),
# so its IAT held no such slot, MMHack's patch loop found nothing and MME
# aborted with "Initialize error" - the original MikuMikuDance links
# d3dx9_43.lib from the DirectX SDK and therefore exposes them.
#
# The import library is generated from res/imports/d3dx9_43.def (lib.exe), so
# the build still needs no DirectX SDK; d3dx_dyn.hpp then takes its pointers
# from those slots, which also lets MME's hooks intercept the port's D3DX
# calls.  x64 only: the x86 DLL exports stdcall-decorated names and the x86
# build keeps the runtime-resolution path.
# ---------------------------------------------------------------------------
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MIKUMIKUSTUDIO_D3DX_DEF ${CMAKE_CURRENT_SOURCE_DIR}/res/imports/d3dx9_43.def)
    set(MIKUMIKUSTUDIO_D3DX_IMPLIB ${CMAKE_BINARY_DIR}/d3dx9_43.lib)
    add_custom_command(
        OUTPUT ${MIKUMIKUSTUDIO_D3DX_IMPLIB}
        COMMAND ${CMAKE_AR} /nologo /machine:x64
                "/def:${MIKUMIKUSTUDIO_D3DX_DEF}"
                "/out:${MIKUMIKUSTUDIO_D3DX_IMPLIB}"
        DEPENDS ${MIKUMIKUSTUDIO_D3DX_DEF}
        COMMENT "Generating the d3dx9_43 import library (MME compatibility)"
        VERBATIM)
    add_custom_target(mikumikustudio_d3dx_imports
                      DEPENDS ${MIKUMIKUSTUDIO_D3DX_IMPLIB})
    target_compile_definitions(mikumikustudio_core
                               PUBLIC MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS=1)
    target_link_libraries(mikumikustudio_core PUBLIC ${MIKUMIKUSTUDIO_D3DX_IMPLIB})
    add_dependencies(mikumikustudio_core mikumikustudio_d3dx_imports)
endif()

# ---------------------------------------------------------------------------
# Porting-era diagnostics.  The MIKUMIKUSTUDIO_* environment probes
# (state dumps, A/B traces, canaries) are comparison tooling against the
# original binaries, not product features.  Default OFF: the shipped build
# contains none of that code.  Turn ON only while running A/B sessions:
#   cmake -B build-x64/diag -DMIKUMIKUSTUDIO_DIAG=ON ..
# ---------------------------------------------------------------------------
option(MIKUMIKUSTUDIO_DIAG
       "Compile the porting-era diagnostics probes (MIKUMIKUSTUDIO_* env dumps/traces)" OFF)
if (MIKUMIKUSTUDIO_DIAG)
    target_compile_definitions(mikumikustudio_core PUBLIC MIKUMIKUSTUDIO_DIAG=1)
endif()

# ---------------------------------------------------------------------------
# Tests.  Never part of a release artifact: each test executable is
# EXCLUDE_FROM_ALL, so neither the default build nor a release staging
# directory ever receives a test binary.
# ---------------------------------------------------------------------------
option(MIKUMIKUSTUDIO_BUILD_INPUT_TESTS "Build keyboard and empty-model regression tests" OFF)
if (MIKUMIKUSTUDIO_BUILD_INPUT_TESTS)
    enable_testing()
    # Build explicitly when needed:
    #   cmake --build build --config Release --target keyboard_input_tests
    add_executable(keyboard_input_tests EXCLUDE_FROM_ALL tests/keyboard_input_tests.cpp)
    target_link_libraries(keyboard_input_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(keyboard_input_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME keyboard_input_regressions COMMAND keyboard_input_tests)
endif()

# Production-entry regression tests for the first repair batch.
add_executable(vmd_load_safety_tests EXCLUDE_FROM_ALL tests/vmd_load_safety_tests.cpp)
target_link_libraries(vmd_load_safety_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(vmd_load_safety_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME vmd_load_safety COMMAND vmd_load_safety_tests)
add_executable(mmv4mms_physics_input_tests EXCLUDE_FROM_ALL
    tests/mmv4mms_physics_input_tests.cpp src/app/app_ctor.cpp)
target_include_directories(mmv4mms_physics_input_tests PRIVATE include sdk/mmv4mms/include)
target_link_libraries(mmv4mms_physics_input_tests PRIVATE BULLET275::BULLET275)
set_target_properties(mmv4mms_physics_input_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME mmv4mms_physics_input COMMAND mmv4mms_physics_input_tests)

enable_testing()
add_executable(frame_rate_tests EXCLUDE_FROM_ALL tests/frame_rate_tests.cpp)
target_include_directories(frame_rate_tests PRIVATE include)
target_compile_features(frame_rate_tests PRIVATE cxx_std_17)
set_target_properties(frame_rate_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME frame_rate_counts COMMAND frame_rate_tests)
add_library(nvidia_antialias_plugin_stub SHARED EXCLUDE_FROM_ALL tests/nvidia_antialias_plugin_stub.cpp)
set_target_properties(nvidia_antialias_plugin_stub PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_options(nvidia_antialias_plugin_stub PRIVATE "/EXPORT:MMSNvidiaAAFixQueryInstalled=_MMSNvidiaAAFixQueryInstalled@4")
endif()
add_executable(nvidia_antialias_fix_tests EXCLUDE_FROM_ALL tests/nvidia_antialias_fix_tests.cpp)
add_dependencies(nvidia_antialias_fix_tests nvidia_antialias_plugin_stub)
target_include_directories(nvidia_antialias_fix_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nvidia_antialias_fix_tests PRIVATE cxx_std_17)
target_link_libraries(nvidia_antialias_fix_tests PRIVATE mikumikustudio_runtime_log user32 gdi32 shell32 advapi32)
set_target_properties(nvidia_antialias_fix_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME nvidia_antialias_fix_tests COMMAND nvidia_antialias_fix_tests "$<TARGET_FILE:nvidia_antialias_plugin_stub>")
option(MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS "Build effect API regression tests" OFF)
if (MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS)
    enable_testing()
    foreach(effect_check effect_file_watch)
        add_executable(${effect_check}_tests EXCLUDE_FROM_ALL tests/${effect_check}_tests.cpp)
        target_include_directories(${effect_check}_tests PRIVATE include)
        target_compile_features(${effect_check}_tests PRIVATE cxx_std_17)
        target_compile_options(${effect_check}_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
        set_target_properties(${effect_check}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    endforeach()
    target_link_libraries(effect_file_watch_tests PRIVATE user32)
    add_test(NAME effect_file_watch_changes COMMAND effect_file_watch_tests "${CMAKE_BINARY_DIR}/file-watch-test-fixture")
    add_executable(smaa_shader_tests EXCLUDE_FROM_ALL tests/smaa_shader_tests.cpp)
    target_include_directories(smaa_shader_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(smaa_shader_tests PRIVATE cxx_std_17)
    target_link_libraries(smaa_shader_tests PRIVATE mikumikustudio_graphics_runtime d3d9 user32)
    set_target_properties(smaa_shader_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME smaa_shader_checks COMMAND smaa_shader_tests)
    set_tests_properties(smaa_shader_checks PROPERTIES LABELS "hardware-manual" TIMEOUT 60)
    add_executable(fxaa_shader_tests EXCLUDE_FROM_ALL tests/fxaa_shader_tests.cpp)
    target_compile_features(fxaa_shader_tests PRIVATE cxx_std_17)
    target_link_libraries(fxaa_shader_tests PRIVATE ole32 windowscodecs user32)
    set_target_properties(fxaa_shader_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME fxaa_shader_checks COMMAND fxaa_shader_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fxaa/FXAA.hlsl"
        "${CMAKE_BINARY_DIR}/tests/fxaa-captures" --strict-quality)
    set_tests_properties(fxaa_shader_checks PROPERTIES LABELS "hardware-manual" TIMEOUT 60)
    add_executable(d3d12_interop_tests EXCLUDE_FROM_ALL tests/d3d12_interop_tests.cpp)
    add_executable(sequential_file_reader_tests EXCLUDE_FROM_ALL tests/sequential_file_reader_tests.cpp)
    target_include_directories(sequential_file_reader_tests PRIVATE include)
    set_target_properties(sequential_file_reader_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME sequential_file_reader_regressions COMMAND sequential_file_reader_tests)
    target_link_libraries(d3d12_interop_tests PRIVATE d3d9 d3d12 dxgi user32)
    set_target_properties(d3d12_interop_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(d3dx_import_tests EXCLUDE_FROM_ALL tests/d3dx_import_tests.cpp)
    target_link_libraries(d3dx_import_tests PRIVATE mikumikustudio_core)
    set_target_properties(d3dx_import_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME d3dx_import_regressions COMMAND d3dx_import_tests)
    add_executable(effect_api_tests EXCLUDE_FROM_ALL tests/effect_api_tests.cpp)
    target_link_libraries(effect_api_tests PRIVATE mikumikustudio_core winmm dsound)
    # Keep the test away from any user-installed d3d9/MME proxy in Release.
    set_target_properties(effect_api_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME effect_api_regressions COMMAND effect_api_tests)
    add_executable(bone_projection_tests EXCLUDE_FROM_ALL tests/bone_projection_tests.cpp)
    target_link_libraries(bone_projection_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(bone_projection_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_projection_regressions COMMAND bone_projection_tests)
    add_executable(bone_overlay_tests EXCLUDE_FROM_ALL tests/bone_overlay_tests.cpp)
    target_link_libraries(bone_overlay_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(bone_overlay_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_overlay_regressions COMMAND bone_overlay_tests)
    add_executable(model_edge_tests EXCLUDE_FROM_ALL tests/model_edge_tests.cpp)
    target_link_libraries(model_edge_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(model_edge_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME model_edge_regressions COMMAND model_edge_tests)
    add_executable(outline_coverage_tests EXCLUDE_FROM_ALL tests/outline_coverage_tests.cpp)
    target_link_libraries(outline_coverage_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(outline_coverage_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME outline_coverage_regressions COMMAND outline_coverage_tests)
    add_executable(outline_multisample_tests EXCLUDE_FROM_ALL tests/outline_multisample_tests.cpp)
    target_link_libraries(outline_multisample_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(outline_multisample_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME outline_multisample_regressions COMMAND outline_multisample_tests)
    set_tests_properties(outline_multisample_regressions PROPERTIES LABELS "hardware-manual" TIMEOUT 90)
    add_executable(model_vertex_cache_tests EXCLUDE_FROM_ALL tests/model_vertex_cache_tests.cpp)
    target_link_libraries(model_vertex_cache_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(model_vertex_cache_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(model_alpha_tests EXCLUDE_FROM_ALL tests/model_alpha_tests.cpp)
    target_include_directories(model_alpha_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(model_alpha_tests PRIVATE d3d9 user32)
    set_target_properties(model_alpha_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME model_alpha_regressions COMMAND model_alpha_tests)
    add_executable(model_alpha_occlusion_tests EXCLUDE_FROM_ALL tests/model_alpha_occlusion_tests.cpp)
    target_link_libraries(model_alpha_occlusion_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(model_alpha_occlusion_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_library(mme_presence_marker SHARED EXCLUDE_FROM_ALL tests/mme_presence_marker.cpp)
    set_target_properties(mme_presence_marker PROPERTIES OUTPUT_NAME MMEffect
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-marker/$<CONFIG>")
    add_dependencies(model_alpha_occlusion_tests mme_presence_marker)
    add_test(NAME model_alpha_sort_off COMMAND model_alpha_occlusion_tests --sort-off)
    add_test(NAME model_alpha_sort_off_mme_presence COMMAND model_alpha_occlusion_tests
        --sort-off --mme-marker $<TARGET_FILE:mme_presence_marker>)
    if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
        add_test(NAME model_alpha_occlusion_regressions COMMAND model_alpha_occlusion_tests --precision standard)
        add_test(NAME model_alpha_occlusion_low_regressions COMMAND model_alpha_occlusion_tests --precision low)
        add_test(NAME model_alpha_occlusion_high_regressions COMMAND model_alpha_occlusion_tests --precision high)
        add_test(NAME model_alpha_occlusion_maximum_regressions COMMAND model_alpha_occlusion_tests --precision maximum)
        add_test(NAME model_alpha_intersection_regressions COMMAND model_alpha_occlusion_tests --intersections)
        add_test(NAME model_alpha_low_alpha_regressions COMMAND model_alpha_occlusion_tests --low-alpha)
        add_test(NAME model_alpha_peel_budget_regressions COMMAND model_alpha_occlusion_tests --peel-budget)
        add_test(NAME model_alpha_low_alpha_msaa_regressions COMMAND model_alpha_occlusion_tests --low-alpha --msaa)
        add_test(NAME model_alpha_intersection_msaa_regressions COMMAND model_alpha_occlusion_tests --intersections --msaa)
        add_test(NAME model_alpha_peel_budget_msaa_regressions COMMAND model_alpha_occlusion_tests --peel-budget --msaa)
        add_test(NAME model_alpha_maximum_limit_regressions COMMAND model_alpha_occlusion_tests --maximum-limit)
        add_test(NAME model_alpha_maximum_limit_msaa_regressions COMMAND model_alpha_occlusion_tests --maximum-limit --msaa)
    endif()
    add_executable(capture_downsample_tests EXCLUDE_FROM_ALL tests/capture_downsample_tests.cpp)
    target_link_libraries(capture_downsample_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(capture_downsample_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME capture_downsample_regressions COMMAND capture_downsample_tests)
    set_tests_properties(capture_downsample_regressions PROPERTIES TIMEOUT 20)

    foreach (physics_test physics_pose_tests pmx_joint_tests physics_bake_settings_tests physics_scale_tests affine_collision_shape_tests)
        add_executable(${physics_test} EXCLUDE_FROM_ALL tests/${physics_test}.cpp)
        target_link_libraries(${physics_test} PRIVATE mikumikustudio_core winmm dsound)
        set_target_properties(${physics_test} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
        add_test(NAME ${physics_test} COMMAND ${physics_test})
        set_tests_properties(${physics_test} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(pmx_text_tests EXCLUDE_FROM_ALL tests/pmx_text_tests.cpp)
    target_link_libraries(pmx_text_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(pmx_text_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME pmx_text_regressions COMMAND pmx_text_tests)
    set_tests_properties(pmx_text_regressions PROPERTIES TIMEOUT 20)
    add_executable(background_overlay_tests EXCLUDE_FROM_ALL tests/background_overlay_tests.cpp)
    target_link_libraries(background_overlay_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(background_overlay_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME background_overlay_regressions COMMAND background_overlay_tests)
    add_executable(device_reset_tests EXCLUDE_FROM_ALL tests/device_reset_tests.cpp)
    target_link_libraries(device_reset_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(device_reset_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME device_reset_regressions COMMAND device_reset_tests)
endif()

if (MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bake_attributes_tests.cpp")
    add_executable(native_bake_attributes_tests EXCLUDE_FROM_ALL tests/native_bake_attributes_tests.cpp)
    target_link_libraries(native_bake_attributes_tests PRIVATE mikumikustudio_core Alembic::Alembic winmm dsound)
    set_target_properties(native_bake_attributes_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME native_bake_attributes COMMAND native_bake_attributes_tests "${CMAKE_BINARY_DIR}/native-bake-attributes-artifacts")
endif()

option(MIKUMIKUSTUDIO_BUILD_ENCODING_TESTS "Build filename encoding regression tests" OFF)
if (MIKUMIKUSTUDIO_BUILD_ENCODING_TESTS)
    enable_testing()
    add_executable(filename_encoding_tests EXCLUDE_FROM_ALL tests/filename_encoding_tests.cpp)
    target_link_libraries(filename_encoding_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(filename_encoding_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME filename_encoding_regressions COMMAND filename_encoding_tests)
    add_executable(morph_import_tests EXCLUDE_FROM_ALL tests/morph_import_tests.cpp)
    target_link_libraries(morph_import_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(morph_import_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME morph_import_regressions COMMAND morph_import_tests)
endif()

# ---------------------------------------------------------------------------
# User-acceptance regression tests: the defects reported by the field test
# round (UI language consistency, panel text, camera FOV, bone editing
# shortcuts, list selection, clipboard dialogs, PMM reload).
# ---------------------------------------------------------------------------
option(MIKUMIKUSTUDIO_BUILD_UI_TESTS "Build user-acceptance regression tests" OFF)
if (MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(mf_h264_startup_tests EXCLUDE_FROM_ALL tests/mf_h264_startup_tests.cpp)
    target_include_directories(mf_h264_startup_tests PRIVATE include)
    target_compile_options(mf_h264_startup_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Ob0>)
    target_link_libraries(mf_h264_startup_tests PRIVATE minhook::minhook mikumikustudio_runtime_log
        mfplat mfreadwrite mfuuid ole32 oleaut32 strmiids user32 gdi32)
    add_executable(model_display_order_tests EXCLUDE_FROM_ALL tests/model_display_order_tests.cpp)
    target_link_libraries(model_display_order_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    foreach(_test IN ITEMS mf_h264_startup_tests model_display_order_tests)
        set_target_properties(${_test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
        add_test(NAME ${_test} COMMAND ${_test})
        set_tests_properties(${_test} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(native_media_tests EXCLUDE_FROM_ALL tests/native_media_tests.cpp
        src/media/native_media.cpp src/media/native_video.cpp src/media/wic_mov_recorder.cpp)
    target_include_directories(native_media_tests PRIVATE include)
    target_link_libraries(native_media_tests PRIVATE mfplat mfreadwrite mfuuid ole32 oleaut32 windowscodecs)
    set_target_properties(native_media_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    enable_testing()
    foreach (ui_test ui_language_tests ui_message_tests panel_key_tests bone_registration_tests physics_bake_tests channel_bake_tests window_interaction_tests pmm_reload_tests morph_runtime_tests runtime_layout_tests)
        add_executable(${ui_test} EXCLUDE_FROM_ALL tests/${ui_test}.cpp)
        target_link_libraries(${ui_test} PRIVATE mikumikustudio_core winmm dsound)
        set_target_properties(${ui_test} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
        add_test(NAME ${ui_test} COMMAND ${ui_test})
    endforeach()
    add_test(NAME bone_translation_reset COMMAND panel_key_tests --bone-translation-reset)
    set_tests_properties(bone_translation_reset PROPERTIES TIMEOUT 20 LABELS "ui")
    add_executable(bone_key_pool_tests EXCLUDE_FROM_ALL tests/bone_key_pool_tests.cpp src/model/bone_key_pool.cpp)
    target_include_directories(bone_key_pool_tests PRIVATE include)
    set_target_properties(bone_key_pool_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_key_pool COMMAND bone_key_pool_tests)
    set_tests_properties(bone_registration_tests PROPERTIES TIMEOUT 20)
    add_test(NAME review_editing_regressions COMMAND bone_registration_tests --review-editing)
    set_tests_properties(review_editing_regressions PROPERTIES TIMEOUT 20)
    if(NOT MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
        add_test(NAME review_curve_editing_regressions COMMAND bone_registration_tests --review-curves)
        set_tests_properties(review_curve_editing_regressions PROPERTIES TIMEOUT 20)
    endif()
endif()

# ---------------------------------------------------------------------------
# Main executable.  WIN32 subsystem matches the original PE header
# (Subsystem 2, GUI).  Exports (Exp*) come from exports/MikuMikuDance.def
# exactly like the original export table at RVA 0x00141328.
# ---------------------------------------------------------------------------
# Keep compiled dialog resources in sync with their editable templates.
option(MIKUMIKUSTUDIO_PRECOMPILE_EFFECTS
    "Precompile full-profile default effects using the installed D3DX9 runtime" ON)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(MIKUMIKUSTUDIO_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/resources")
    set(MIKUMIKUSTUDIO_EFFECT_RESOURCE_FLAGS)
    if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full" AND MIKUMIKUSTUDIO_PRECOMPILE_EFFECTS)
        # Keep the MME resource entry point, but remove HLSL compilation from
        # application startup. The original-compatible profile stays verbatim.
        list(APPEND MIKUMIKUSTUDIO_EFFECT_RESOURCE_FLAGS --compile-effects)
    endif()
    file(GLOB_RECURSE MIKUMIKUSTUDIO_RESOURCE_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/res/templates/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/res/assets/*")
    add_custom_command(
        OUTPUT "${MIKUMIKUSTUDIO_RESOURCE_DIR}/gen_resources.res"
               "${MIKUMIKUSTUDIO_RESOURCE_DIR}/gen_resources_x64.res"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_resources.py" gen
                --output-dir "${MIKUMIKUSTUDIO_RESOURCE_DIR}" --profile "${MIKUMIKUSTUDIO_PROFILE}"
                ${MIKUMIKUSTUDIO_EFFECT_RESOURCE_FLAGS}
        COMMAND "${CMAKE_COMMAND}" -E touch
                "${MIKUMIKUSTUDIO_RESOURCE_DIR}/gen_resources.res"
                "${MIKUMIKUSTUDIO_RESOURCE_DIR}/gen_resources_x64.res"
        DEPENDS ${MIKUMIKUSTUDIO_RESOURCE_INPUTS}
                scripts/gen_resources.py scripts/dialog_rc.py scripts/compile_effect.py
                src/render/transparent_peeling_shader.inc
                res/mmd_manifest.xml res/mmd_manifest_x64.xml
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Regenerate embedded dialog and menu resources"
        VERBATIM)
elseif(MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
    message(FATAL_ERROR "The mmd932 profile requires Python to generate its isolated resources")
else()
    set(MIKUMIKUSTUDIO_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res")
endif()
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    # x64 build embeds the original x64 manifest (amd64 dependencies) -
    # the x86 manifest's x86 assemblies
    # make the 64-bit loader fail with STATUS_INVALID_IMAGE_FORMAT.
    set(MIKUMIKUSTUDIO_GEN_RES "${MIKUMIKUSTUDIO_RESOURCE_DIR}/gen_resources_x64.res")
else()
    set(MIKUMIKUSTUDIO_GEN_RES "${MIKUMIKUSTUDIO_RESOURCE_DIR}/gen_resources.res")
endif()
if (MIKUMIKUSTUDIO_BUILD_UI_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/device_lost_wait_tests.cpp")
    add_executable(device_lost_wait_tests EXCLUDE_FROM_ALL tests/device_lost_wait_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(device_lost_wait_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(device_lost_wait_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(device_lost_wait_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/device-lost/$<CONFIG>")
    function(add_device_lost_case entry scenario mode)
        set(test_name "device_lost_${entry}_${scenario}_${mode}")
        set(test_directory "${CMAKE_BINARY_DIR}/device-lost-artifacts/${test_name}")
        file(MAKE_DIRECTORY "${test_directory}")
        add_test(NAME "${test_name}" COMMAND device_lost_wait_tests
            --entry "${entry}" --case "${scenario}" --mode "${mode}")
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE
            WORKING_DIRECTORY "${test_directory}")
    endfunction()
    foreach(mode windowed fullscreen)
        foreach(entry frame readback record-frame)
            foreach(scenario ok ready lost-ready lost-reset message-ready quit
                reset-ok reset-fail message-reset-fail direct-reset-ok direct-reset-fail
                replace-device replace-slot reenter fatal)
                add_device_lost_case("${entry}" "${scenario}" "${mode}")
            endforeach()
            if(NOT entry STREQUAL "frame")
                add_device_lost_case("${entry}" cancel "${mode}")
                add_device_lost_case("${entry}" invalidate-surfaces "${mode}")
            endif()
        endforeach()
        add_device_lost_case(frame paused-background-seek "${mode}")
        add_device_lost_case(frame in-frame-timeline-reset-fail "${mode}")
        add_device_lost_case(record-frame in-frame-record-esc-reset-fail "${mode}")
        add_device_lost_case(record-frame in-frame-record-end-reset-fail "${mode}")
    endforeach()
    add_device_lost_case(frame in-frame-esc-reset-fail fullscreen)
endif()

if(TARGET physics_bake_tests)
    target_sources(physics_bake_tests PRIVATE res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_options(physics_bake_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    if(MIKUMIKUSTUDIO_PROFILE STREQUAL "full")
        add_test(NAME dense_keyframe_playback COMMAND physics_bake_tests --dense-playback)
    endif()
endif()
if (MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(viewport_render_scale_tests EXCLUDE_FROM_ALL
        tests/viewport_render_scale_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(viewport_render_scale_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(viewport_render_scale_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(viewport_render_scale_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME viewport_render_scale_tests COMMAND viewport_render_scale_tests)
    set_tests_properties(viewport_render_scale_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(video_export_range_tests EXCLUDE_FROM_ALL
        tests/video_export_range_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(video_export_range_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    target_link_options(video_export_range_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(video_export_range_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(installation_settings_tests EXCLUDE_FROM_ALL tests/installation_settings_tests.cpp)
    target_link_libraries(installation_settings_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(installation_settings_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(autosave_tests EXCLUDE_FROM_ALL
        tests/autosave_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(autosave_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    target_link_options(autosave_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(autosave_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(runtime_log_tests EXCLUDE_FROM_ALL
        tests/runtime_log_tests.cpp src/main/runtime_log.cpp)
    target_compile_definitions(runtime_log_tests PRIVATE MMS_RUNTIME_LOG_TESTS=1)
    target_include_directories(runtime_log_tests PRIVATE include)
    target_link_libraries(runtime_log_tests PRIVATE minhook::minhook user32)
    set_target_properties(runtime_log_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(image_sequence_tests EXCLUDE_FROM_ALL
        tests/image_sequence_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(image_sequence_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    target_link_options(image_sequence_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(image_sequence_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    foreach(review_test kinect_pose_math_tests path_resolve_tests)
        add_executable(${review_test} EXCLUDE_FROM_ALL tests/${review_test}.cpp)
        target_link_libraries(${review_test} PRIVATE mikumikustudio_core winmm dsound)
        set_target_properties(${review_test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
        add_test(NAME ${review_test} COMMAND ${review_test})
    endforeach()
    # Run explicitly on a private desktop; no device or video output is needed.
    add_executable(physics_editor_encoding_tests EXCLUDE_FROM_ALL
        tests/physics_editor_encoding_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(physics_editor_encoding_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    target_link_options(physics_editor_encoding_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(physics_editor_encoding_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    # Explicit private-desktop runner; a normal ctest process cannot meet the
    # desktop guard, and ACP variants require separate executable manifests.
    add_executable(menu_language_tests EXCLUDE_FROM_ALL
        tests/menu_language_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(menu_language_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(menu_language_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(menu_language_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME menu_language_regressions COMMAND menu_language_tests)
    add_executable(hidden_morph_tests EXCLUDE_FROM_ALL tests/hidden_morph_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(hidden_morph_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(hidden_morph_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(hidden_morph_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    # Requires the separately supplied Rou PMX sample; run explicitly with --model.
endif()
# Exercise the actual embedded model effects in the occlusion regression.
if(TARGET model_alpha_occlusion_tests)
    target_sources(model_alpha_occlusion_tests PRIVATE "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_options(model_alpha_occlusion_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
endif()
if (MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS)
    if (MIKUMIKUSTUDIO_ENABLE_DX12)
        add_executable(graphics_runtime_tests EXCLUDE_FROM_ALL tests/graphics_runtime_tests.cpp)
        target_link_libraries(graphics_runtime_tests PRIVATE mikumikustudio_core winmm dsound)
        set_target_properties(graphics_runtime_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
        foreach(mode system missing-d3d12 missing-dxgi missing-compiler missing-device-export missing-factory2-export)
            add_test(NAME graphics_runtime_${mode} COMMAND graphics_runtime_tests ${mode})
            set_tests_properties(graphics_runtime_${mode} PROPERTIES LABELS "hardware-manual" TIMEOUT 60)
        endforeach()
    endif()
    add_executable(skinning_backend_tests EXCLUDE_FROM_ALL tests/skinning_backend_tests.cpp)
    target_link_libraries(skinning_backend_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(skinning_backend_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(processor_settings_tests EXCLUDE_FROM_ALL tests/processor_settings_tests.cpp)
    target_link_libraries(processor_settings_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(processor_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(d3d12_skinning_tests EXCLUDE_FROM_ALL tests/d3d12_skinning_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(d3d12_skinning_tests PRIVATE mikumikustudio_core mikumikustudio_d3d12 winmm dsound)
    target_link_options(d3d12_skinning_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(d3d12_skinning_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
endif()

add_executable(MikuMikuStudio WIN32
    src/main/winmain.cpp
    res/MMD.rc
    ${MIKUMIKUSTUDIO_GEN_RES}
    ${CMAKE_CURRENT_SOURCE_DIR}/exports/MikuMikuDance.def
)
add_custom_command(TARGET MikuMikuStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/licenses" "$<TARGET_FILE_DIR:MikuMikuStudio>/licenses")

if (MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(affine_skinning_tests EXCLUDE_FROM_ALL tests/affine_skinning_tests.cpp)
    target_include_directories(affine_skinning_tests PRIVATE include)
    target_compile_features(affine_skinning_tests PRIVATE cxx_std_17)
    set_target_properties(affine_skinning_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME affine_skinning_tests COMMAND affine_skinning_tests)

    add_executable(bone_scale_io_tests EXCLUDE_FROM_ALL tests/bone_scale_io_tests.cpp)
    target_link_libraries(bone_scale_io_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(bone_scale_io_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_scale_io_tests COMMAND bone_scale_io_tests)

    add_executable(accessory_motion_io_tests EXCLUDE_FROM_ALL tests/accessory_motion_io_tests.cpp)
    target_link_libraries(accessory_motion_io_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(accessory_motion_io_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME accessory_motion_io_tests COMMAND accessory_motion_io_tests)

    add_executable(vpd_scale_io_tests EXCLUDE_FROM_ALL tests/vpd_scale_io_tests.cpp src/io/vpd_scale_io.cpp)
    target_include_directories(vpd_scale_io_tests PRIVATE include)
    target_compile_features(vpd_scale_io_tests PRIVATE cxx_std_17)
    set_target_properties(vpd_scale_io_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME vpd_scale_io_tests COMMAND vpd_scale_io_tests)

    add_executable(viewport_corner_layout_tests EXCLUDE_FROM_ALL tests/viewport_corner_layout_tests.cpp)
    target_link_libraries(viewport_corner_layout_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(viewport_corner_layout_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME viewport_corner_layout_tests COMMAND viewport_corner_layout_tests)

    add_executable(menu_draw_state_tests EXCLUDE_FROM_ALL tests/menu_draw_state_tests.cpp)
    target_link_libraries(menu_draw_state_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(menu_draw_state_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME menu_draw_state_tests COMMAND menu_draw_state_tests)
    add_executable(recent_projects_menu_tests EXCLUDE_FROM_ALL tests/recent_projects_menu_tests.cpp)
    target_link_libraries(recent_projects_menu_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(recent_projects_menu_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME recent_projects_menu_regressions COMMAND recent_projects_menu_tests)

    add_executable(bone_scale_tests EXCLUDE_FROM_ALL tests/bone_scale_tests.cpp)
    target_link_libraries(bone_scale_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(bone_scale_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_scale_tests COMMAND bone_scale_tests)

    # Viewport scale handles: hit testing, the merged mode button's cycle and
    # three-language captions, the panel grid and the handle drag paths.  CPU
    # only - no device, no window.
    add_executable(bone_scale_gizmo_tests EXCLUDE_FROM_ALL tests/bone_scale_gizmo_tests.cpp)
    target_link_libraries(bone_scale_gizmo_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(bone_scale_gizmo_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_scale_gizmo_tests COMMAND bone_scale_gizmo_tests)
    add_executable(native_editors_integration EXCLUDE_FROM_ALL tests/native_editors_integration.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(native_editors_integration PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(native_editors_integration PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(native_editors_integration PROPERTIES OUTPUT_NAME MikuMikuDance ENABLE_EXPORTS ON RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/native-editors/$<CONFIG>")
    # Exercise the same PMM model-dialog resources that the application embeds.
    target_sources(ui_message_tests PRIVATE res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    add_test(NAME accessory_message_regressions COMMAND ui_message_tests accessory-load)
    set_tests_properties(accessory_message_regressions PROPERTIES TIMEOUT 10)
    target_link_options(ui_message_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    # The viewport operation axis and the coordinate-system caption both read
    # embedded resources (XFILE 115 axis.x, the HUD font atlas).
    add_executable(bone_scale_hud_tests EXCLUDE_FROM_ALL
        tests/bone_scale_hud_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(bone_scale_hud_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(bone_scale_hud_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(bone_scale_hud_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME bone_scale_hud_tests COMMAND bone_scale_hud_tests "${CMAKE_BINARY_DIR}/tests/bone-scale-hud")

    add_executable(coordinate_axis_tests EXCLUDE_FROM_ALL
        tests/coordinate_axis_tests.cpp res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(coordinate_axis_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(coordinate_axis_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(coordinate_axis_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME coordinate_axis_regressions COMMAND coordinate_axis_tests)
    # Optional real-scene stress runner. No user assets are distributed with it.
    add_executable(physics_scene_tests EXCLUDE_FROM_ALL tests/physics_scene_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(physics_scene_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(physics_scene_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(physics_scene_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(vertex_capture_tests EXCLUDE_FROM_ALL tests/vertex_capture_tests.cpp src/render/vertex_capture.cpp)
    target_include_directories(vertex_capture_tests PRIVATE include)
    target_compile_features(vertex_capture_tests PRIVATE cxx_std_17)
    target_link_libraries(vertex_capture_tests PRIVATE d3d9 d3dcompiler)
    set_target_properties(vertex_capture_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME vertex_capture COMMAND vertex_capture_tests)
    add_executable(mesh_bake_cache_tests EXCLUDE_FROM_ALL tests/mesh_bake_cache_tests.cpp
        src/io/mesh_bake_cache.cpp src/io/pmm_extension.cpp)
    target_include_directories(mesh_bake_cache_tests PRIVATE include)
    target_link_libraries(mesh_bake_cache_tests PRIVATE Alembic::Alembic)
    set_target_properties(mesh_bake_cache_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME mesh_bake_cache COMMAND mesh_bake_cache_tests "${CMAKE_BINARY_DIR}/mesh-cache-test-artifacts")
    add_executable(alembic_export_tests EXCLUDE_FROM_ALL tests/alembic_export_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(alembic_export_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(alembic_export_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(alembic_export_tests PROPERTIES OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/alembic/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/alembic/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/alembic/$<CONFIG>")
    add_executable(media_render_tests EXCLUDE_FROM_ALL tests/media_render_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(media_render_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(media_render_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(media_render_tests PROPERTIES OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/media/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/media/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/media/$<CONFIG>")
    add_executable(bake_playback_tests EXCLUDE_FROM_ALL tests/bake_playback_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(bake_playback_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(bake_playback_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(bake_playback_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/bake-playback/$<CONFIG>")
    add_executable(lookup_cache_tests EXCLUDE_FROM_ALL tests/lookup_cache_tests.cpp)
    target_include_directories(lookup_cache_tests PRIVATE include)
    target_compile_features(lookup_cache_tests PRIVATE cxx_std_17)
    set_target_properties(lookup_cache_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME lookup_cache_regressions COMMAND lookup_cache_tests)
    add_executable(frame_benchmark EXCLUDE_FROM_ALL tests/frame_benchmark.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(frame_benchmark PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(frame_benchmark PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(frame_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    if(Python3_Interpreter_FOUND)
        add_test(NAME native_shadow_cycle
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_shadow_cycle.py"
                "$<TARGET_FILE:frame_benchmark>" "${CMAKE_BINARY_DIR}/tests/shadow-cycle-results")
        set_tests_properties(native_shadow_cycle PROPERTIES TIMEOUT 240)
    endif()
    add_executable(frame_benchmark_mme EXCLUDE_FROM_ALL tests/frame_benchmark.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(frame_benchmark_mme PRIVATE mikumikustudio_core winmm dsound)
    target_compile_definitions(frame_benchmark_mme PRIVATE REQUIRE_MME=1)
    target_link_options(frame_benchmark_mme PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(frame_benchmark_mme PROPERTIES
        OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-benchmark/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-benchmark/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-benchmark/$<CONFIG>")
    add_executable(pmm_extension_tests EXCLUDE_FROM_ALL tests/pmm_extension_tests.cpp src/io/pmm_extension.cpp)
    target_include_directories(pmm_extension_tests PRIVATE include)
    target_compile_features(pmm_extension_tests PRIVATE cxx_std_17)
    set_target_properties(pmm_extension_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME pmm_extension_tests COMMAND pmm_extension_tests)
    add_executable(pmm_container_tests EXCLUDE_FROM_ALL tests/pmm_container_tests.cpp src/io/pmm_container.cpp)
    target_include_directories(pmm_container_tests PRIVATE include)
    target_compile_features(pmm_container_tests PRIVATE cxx_std_17)
    set_target_properties(pmm_container_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME pmm_container_tests COMMAND pmm_container_tests)
    add_executable(pmm_project_settings_tests EXCLUDE_FROM_ALL tests/pmm_project_settings_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(pmm_project_settings_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(pmm_project_settings_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(pmm_project_settings_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(camera_follow_tests EXCLUDE_FROM_ALL tests/camera_follow_tests.cpp)
    target_link_libraries(camera_follow_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(camera_follow_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME camera_follow_regressions COMMAND camera_follow_tests)
    add_test(NAME camera_export_modes COMMAND camera_follow_tests --export-only)
    add_executable(deferred_effect_tests EXCLUDE_FROM_ALL tests/deferred_effect_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(deferred_effect_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(deferred_effect_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(deferred_effect_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(deferred_effect_mme_tests EXCLUDE_FROM_ALL tests/deferred_effect_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(deferred_effect_mme_tests PRIVATE mikumikustudio_core winmm dsound)
    target_compile_definitions(deferred_effect_mme_tests PRIVATE REQUIRE_MME=1)
    target_link_options(deferred_effect_mme_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(deferred_effect_mme_tests PROPERTIES
        OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme/lib/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme/$<CONFIG>")
    add_executable(wave_audio_tests EXCLUDE_FROM_ALL tests/wave_audio_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(wave_audio_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    target_link_options(wave_audio_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(wave_audio_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(scene_lifecycle_tests EXCLUDE_FROM_ALL tests/scene_lifecycle_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES})
    target_link_libraries(scene_lifecycle_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(scene_lifecycle_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(scene_lifecycle_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_executable(pmd_selection_render_tests EXCLUDE_FROM_ALL tests/pmd_selection_render_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(pmd_selection_render_tests PRIVATE mikumikustudio_core winmm dsound)
    target_link_options(pmd_selection_render_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(pmd_selection_render_tests PROPERTIES
        OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/pmd-selection/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/pmd-selection/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/pmd-selection/$<CONFIG>")
    # Explicit opt-in host for locally installed MME. MMHack imports the host
    # by filename and ordinal, so a normal test executable cannot exercise it.
    add_executable(scene_lifecycle_mme_tests EXCLUDE_FROM_ALL tests/scene_lifecycle_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_libraries(scene_lifecycle_mme_tests PRIVATE mikumikustudio_core winmm dsound)
    target_compile_definitions(scene_lifecycle_mme_tests PRIVATE REQUIRE_MME=1)
    target_link_options(scene_lifecycle_mme_tests PRIVATE "/MANIFEST:NO"
        "${MIKUMIKUSTUDIO_GEN_RES}")
    set_target_properties(scene_lifecycle_mme_tests PROPERTIES
        OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme/$<CONFIG>")
endif()

target_link_libraries(MikuMikuStudio PRIVATE
    mikumikustudio_core
    winmm      # timeGetTime - main loop timing
    dsound     # original timeline/audio subsystem initialization (0x4C2470)
    user32
)

# The elevated installer has no application/plugin dependencies, and only loads
# NVAPI from System32. A static CRT avoids loading app-local runtime DLLs.
add_library(MMSNvidiaAAFix SHARED src/tools/nvidia_antialias_helper.cpp)
target_include_directories(MMSNvidiaAAFix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(MMSNvidiaAAFix PRIVATE cxx_std_17)
target_link_libraries(MMSNvidiaAAFix PRIVATE shell32 advapi32)
set_target_properties(MMSNvidiaAAFix PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_link_options(MMSNvidiaAAFix PRIVATE
        "/EXPORT:DllRegisterServer=_DllRegisterServer@0,PRIVATE"
        "/EXPORT:DllUnregisterServer=_DllUnregisterServer@0,PRIVATE"
        "/EXPORT:MMSNvidiaAAFixQueryInstalled=_MMSNvidiaAAFixQueryInstalled@4,PRIVATE")
elseif(MSVC)
    # The exports are PRIVATE so the helper stays a loader/GetProcAddress
    # target; PRIVATE also suppresses LNK4104 without an extra import library.
    target_link_options(MMSNvidiaAAFix PRIVATE
        "/EXPORT:DllRegisterServer,PRIVATE"
        "/EXPORT:DllUnregisterServer,PRIVATE"
        "/EXPORT:MMSNvidiaAAFixQueryInstalled,PRIVATE")
endif()
add_dependencies(MikuMikuStudio MMSNvidiaAAFix)

# The core's PUBLIC d3dx9_43 import dependency lands in each consuming
# executable's IAT, including the host table patched by MME.

# gen_resources.res re-embeds the original .rsrc byte-exact; link.exe accepts
# .res directly as an input file (CMake itself would classify it as <None>).
# /MANIFEST:NO keeps the linker from adding a default manifest on top of the
# original one embedded through the .res.
target_link_options(MikuMikuStudio PRIVATE
    "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MikuMikuDance.map"
    "/MANIFEST:NO"
    "${MIKUMIKUSTUDIO_GEN_RES}"
)
set_target_properties(MikuMikuStudio PROPERTIES
    # The original x64 binary is MikuMikuDance.exe, and MikuMikuEffect needs
    # that exact module name: its MMHack.dll statically imports the 15 Exp*
    # entry points from "MikuMikuDance.exe", so a differently named host makes
    # the loader fail to bind them (MME then either reports "Initialize error"
    # or dereferences the unbound slots).  Porting-era comments that mention
    # MikuMikuDanceE_v932x64 refer to the reference *distribution*.
    OUTPUT_NAME "MikuMikuDance"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    # Export the 37 Exp* symbols from the .def file (ordinals 1..37).
    ENABLE_EXPORTS ON
)

# ---------------------------------------------------------------------------
# MMDxShow - the companion DirectShow push-source filter.  Its source is
# statically linked into the host so AVI codecs remain available without a
# separately-installed DLL.  The optional standalone reference DLL is still
# available for interface tests, but is no longer installed with the app.
# Built for both
# architectures: the x86 build pins the VC8 original's layout
# (../MikuMikuDanceE_v932/Data/MMDxShow.dll), the x64 build pins the VC10
# rebuild (../MikuMikuDanceE_v932x64/Data/MMDxShow.dll) - the per-flavour
# static_asserts live in src/mmdxshow/mmdxshow.hpp.  Both originals export
# the same ordinals 1..5 via the .def (no per-arch def needed).
# ---------------------------------------------------------------------------
add_library(MMDxShow SHARED EXCLUDE_FROM_ALL
    src/mmdxshow/dll_main.cpp
    src/mmdxshow/enumerators.cpp
    src/mmdxshow/source_base.cpp
    src/mmdxshow/push_source.cpp
    src/mmdxshow/push_pin.cpp
    src/mmdxshow/guids.cpp
    src/mmdxshow/crt_ledger.cpp
)

add_library(mmdxshow_embedded STATIC
    src/mmdxshow/dll_main.cpp
    src/mmdxshow/enumerators.cpp
    src/mmdxshow/source_base.cpp
    src/mmdxshow/push_source.cpp
    src/mmdxshow/push_pin.cpp
    src/mmdxshow/crt_ledger.cpp
)
target_include_directories(mmdxshow_embedded PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/mmdxshow)
target_link_libraries(mmdxshow_embedded PUBLIC ole32 strmiids uuid)
target_link_libraries(mikumikustudio_core PUBLIC mmdxshow_embedded)
target_include_directories(MMDxShow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/mmdxshow)
target_link_libraries(MMDxShow PRIVATE
    kernel32 advapi32 gdi32 user32 ole32 uuid
)
target_link_options(MMDxShow PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/mmdxshow/MMDxShow.def")
# The standalone reference filter is built only for interface tests; the host
# embeds the push-source implementation in mmdxshow_embedded.  Do not install
# the standalone DLL into the user-facing build.
set_target_properties(MMDxShow PROPERTIES
    OUTPUT_NAME "MMDxShow"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reference/$<CONFIG>"
)

if (MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(mmdxshow_interface_tests EXCLUDE_FROM_ALL tests/mmdxshow_interface_tests.cpp)
    add_executable(recording_state_tests EXCLUDE_FROM_ALL tests/recording_state_tests.cpp)
    target_include_directories(recording_state_tests PRIVATE include)
    target_link_libraries(recording_state_tests PRIVATE user32)
    add_test(NAME recording_state_tests COMMAND recording_state_tests)
    target_link_libraries(mmdxshow_interface_tests PRIVATE ole32 strmiids)
    add_dependencies(mmdxshow_interface_tests MMDxShow)
    set_target_properties(mmdxshow_interface_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME mmdxshow_interface_tests
        COMMAND mmdxshow_interface_tests "$<TARGET_FILE:MMDxShow>")
endif()

# ---------------------------------------------------------------------------
# Release layout. The final runtime package lives in build/Release.
# `cmake --install build --config Release --prefix build/Release` installs
# to that same directory:
#
#   build\Release\MikuMikuDance.exe <- DirectShow push source is embedded
#
# The Windows WIC PNG encoder creates transparent MOV files without a separate codec.
# The regression executables are EXCLUDE_FROM_ALL
# and carry no install rule, static/import libraries and PDBs live under
# build\lib and build\symbols. Fatal error logs go to backups beside the EXE.
# The runtime assets (Data\toon*.bmp, mmconfig.ini) come from the reference
# MMD installation and are not part of this source tree.
# ---------------------------------------------------------------------------
install(TARGETS MikuMikuStudio RUNTIME DESTINATION .)
# The NVIDIA driver-setting helper is an explicit, separate download/install.
install(TARGETS MMSNvidiaAAFix RUNTIME DESTINATION . COMPONENT NvidiaAAFix EXCLUDE_FROM_ALL)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/licenses/" DESTINATION licenses)

if(Python3_Interpreter_FOUND)
    add_test(NAME optional_graphics_imports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/optional_graphics_imports.py"
                "$<TARGET_FILE:MikuMikuStudio>")
endif()

if(MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS)
    add_executable(transparent_material_order_tests EXCLUDE_FROM_ALL
        tests/transparent_material_order_tests.cpp)
    target_include_directories(transparent_material_order_tests PRIVATE include)
    target_compile_features(transparent_material_order_tests PRIVATE cxx_std_17)
    set_target_properties(transparent_material_order_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME transparent_material_order_regressions COMMAND transparent_material_order_tests)
endif()

if (MIKUMIKUSTUDIO_BUILD_EFFECT_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/transparent_geometry_tests.cpp")
    add_executable(transparent_geometry_tests EXCLUDE_FROM_ALL
        tests/transparent_geometry_tests.cpp src/render/transparent_geometry.cpp)
    target_include_directories(transparent_geometry_tests PRIVATE include)
    set_target_properties(transparent_geometry_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME transparent_geometry_regressions COMMAND transparent_geometry_tests)
endif()

add_executable(mmv4mms_menu_tests EXCLUDE_FROM_ALL tests/mmv4mms_menu_tests.cpp)
target_link_libraries(mmv4mms_menu_tests PRIVATE mikumikustudio_core winmm dsound)
set_target_properties(mmv4mms_menu_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME mmv4mms_menu COMMAND mmv4mms_menu_tests)
if(NOT MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
    add_executable(mmv_material_pass_tests EXCLUDE_FROM_ALL tests/mmv_material_pass_tests.cpp)
    target_include_directories(mmv_material_pass_tests PRIVATE sdk/mmv4mms/include)
    target_link_libraries(mmv_material_pass_tests PRIVATE mikumikustudio_core winmm dsound)
    set_target_properties(mmv_material_pass_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME mmv_material_pass COMMAND mmv_material_pass_tests)
endif()

if(MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(sidebar_resize_tests EXCLUDE_FROM_ALL tests/sidebar_resize_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(sidebar_resize_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(sidebar_resize_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(sidebar_resize_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/sidebar/$<CONFIG>")
    add_executable(display_compatibility_tests EXCLUDE_FROM_ALL tests/display_compatibility_tests.cpp)
    if(NOT MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932")
        add_executable(mmv_native_shadow_bridge_tests EXCLUDE_FROM_ALL tests/mmv_native_shadow_bridge_tests.cpp
            res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
        target_include_directories(mmv_native_shadow_bridge_tests PRIVATE sdk/mmv4mms/include)
        target_link_options(mmv_native_shadow_bridge_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
        target_link_libraries(mmv_native_shadow_bridge_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
        set_target_properties(mmv_native_shadow_bridge_tests PROPERTIES ENABLE_EXPORTS ON
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mmv-native-shadow/lib/$<CONFIG>"
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mmv-native-shadow/symbols/$<CONFIG>"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mmv-native-shadow/$<CONFIG>")
    endif()
    add_executable(mme_display_tests EXCLUDE_FROM_ALL tests/mme_display_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(mme_display_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(mme_display_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    target_include_directories(mme_display_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set_target_properties(mme_display_tests PROPERTIES OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-display/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-display/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-display/$<CONFIG>")
    add_executable(mme_shadow_filter_tests EXCLUDE_FROM_ALL tests/mme_shadow_filter_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(mme_shadow_filter_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(mme_shadow_filter_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(mme_shadow_filter_tests PROPERTIES OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-shadow-filter/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-shadow-filter/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-shadow-filter/$<CONFIG>")
    add_executable(shadow_filter_tests EXCLUDE_FROM_ALL tests/shadow_filter_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(shadow_filter_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(shadow_filter_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(shadow_filter_tests PROPERTIES OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/shadow-filter/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/shadow-filter/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/shadow-filter/$<CONFIG>")
    add_executable(mme_reassign_tests EXCLUDE_FROM_ALL tests/mme_reassign_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(mme_reassign_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(mme_reassign_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(mme_reassign_tests PROPERTIES OUTPUT_NAME "MikuMikuDance" ENABLE_EXPORTS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-reassign/lib/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-reassign/symbols/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/mme-reassign/$<CONFIG>")
    target_sources(display_compatibility_tests PRIVATE res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(display_compatibility_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(display_compatibility_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(display_compatibility_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/display/$<CONFIG>")
    if(Python3_Interpreter_FOUND)
        add_test(NAME display_compatibility_regressions
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/display_compatibility_runner.py"
                "$<TARGET_FILE:display_compatibility_tests>" "${CMAKE_BINARY_DIR}/tests/display-results")
        set_tests_properties(display_compatibility_regressions PROPERTIES TIMEOUT 90)
    endif()
    add_executable(fullscreen_window_tests EXCLUDE_FROM_ALL tests/fullscreen_window_tests.cpp)
    target_sources(fullscreen_window_tests PRIVATE res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(fullscreen_window_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(fullscreen_window_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(fullscreen_window_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/fullscreen/$<CONFIG>")
endif()

if(MIKUMIKUSTUDIO_PROFILE STREQUAL "mmd932" AND MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(original_profile_ui_tests EXCLUDE_FROM_ALL tests/original_profile_ui_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(original_profile_ui_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(original_profile_ui_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(original_profile_ui_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/original-ui/$<CONFIG>")
    set(MMD_ORIGINAL_UI_RUNTIME "${CMAKE_BINARY_DIR}/tests/original-ui/runtime")
    file(MAKE_DIRECTORY "${MMD_ORIGINAL_UI_RUNTIME}/Data"
        "${MMD_ORIGINAL_UI_RUNTIME}/Local" "${MMD_ORIGINAL_UI_RUNTIME}/Roaming")
    add_test(NAME original_profile_ui COMMAND original_profile_ui_tests)
    set_tests_properties(original_profile_ui PROPERTIES TIMEOUT 45
        WORKING_DIRECTORY "${MMD_ORIGINAL_UI_RUNTIME}"
        ENVIRONMENT "LOCALAPPDATA=${MMD_ORIGINAL_UI_RUNTIME}/Local;APPDATA=${MMD_ORIGINAL_UI_RUNTIME}/Roaming")
endif()

if(MIKUMIKUSTUDIO_BUILD_UI_TESTS)
    add_executable(export_viewport_tests EXCLUDE_FROM_ALL tests/export_viewport_tests.cpp
        res/MMD.rc ${MIKUMIKUSTUDIO_GEN_RES} exports/MikuMikuDance.def)
    target_link_options(export_viewport_tests PRIVATE "/MANIFEST:NO" "${MIKUMIKUSTUDIO_GEN_RES}")
    target_link_libraries(export_viewport_tests PRIVATE mikumikustudio_core minhook::minhook winmm dsound)
    set_target_properties(export_viewport_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/export-viewport/$<CONFIG>")
    if(Python3_Interpreter_FOUND)
        add_test(NAME export_viewport_regressions
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/export_viewport_runner.py"
                "$<TARGET_FILE:export_viewport_tests>" "${CMAKE_BINARY_DIR}/tests/export-viewport-results")
        set_tests_properties(export_viewport_regressions PROPERTIES TIMEOUT 330 RUN_SERIAL TRUE)
    endif()
endif()
