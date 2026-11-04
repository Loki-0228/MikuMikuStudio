# MMHack compatibility implementation adapted from MME-RE/src/mmhack.
add_library(mms_legacy_mmhack_core STATIC
    state.cpp host_api.cpp iat_hook.cpp callbacks.cpp scene_router.cpp
    light_matrix.cpp com_wrappers.cpp d3dx_hooks.cpp extra_exports.cpp material_parser.cpp)
target_compile_features(mms_legacy_mmhack_core PUBLIC cxx_std_17)
target_include_directories(mms_legacy_mmhack_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mms_legacy_mmhack_core SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/dxsdk-d3dx/include)
target_compile_definitions(mms_legacy_mmhack_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0601 _WIN32_WINNT=0x0601)
target_compile_options(mms_legacy_mmhack_core PRIVATE /utf-8 /W4)
target_link_libraries(mms_legacy_mmhack_core PUBLIC user32 d3d9 ${MIKUMIKUSTUDIO_D3DX_IMPLIB})
add_dependencies(mms_legacy_mmhack_core mikumikustudio_d3dx_imports)
add_library(MMSLegacyMMHack SHARED dll_entry.cpp MMHack.def)
target_link_libraries(MMSLegacyMMHack PRIVATE mms_legacy_mmhack_core)
set_target_properties(MMSLegacyMMHack PROPERTIES OUTPUT_NAME MMHack
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
target_link_options(MMSLegacyMMHack PRIVATE /SUBSYSTEM:WINDOWS,6.01)
add_dependencies(MikuMikuStudio MMSLegacyMMHack)
foreach(kind scene_router com_lifetime)
    add_executable(legacy_mmhack_${kind}_tests EXCLUDE_FROM_ALL tests/${kind}_tests.cpp)
    target_link_libraries(legacy_mmhack_${kind}_tests PRIVATE mms_legacy_mmhack_core)
    target_compile_options(legacy_mmhack_${kind}_tests PRIVATE /utf-8)
    set_target_properties(legacy_mmhack_${kind}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    add_test(NAME legacy_mmhack_${kind} COMMAND legacy_mmhack_${kind}_tests)
endforeach()

# Keep the optional legacy package independent of app-local CRT DLLs and API sets.
set_property(TARGET mms_legacy_mmhack_core MMSLegacyMMHack
    legacy_mmhack_scene_router_tests legacy_mmhack_com_lifetime_tests
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
