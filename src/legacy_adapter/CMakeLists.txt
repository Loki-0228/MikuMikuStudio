cmake_minimum_required(VERSION 3.21)

# Standalone project for MMSAdapter.dll only. The host build adds this directory
# or builds it separately; the DLL is a self-contained loader-time bridge that
# takes no link dependency on the host executable, an import library, or any
# graphics/API library.
project(MMSAdapter LANGUAGES CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MMSAdapter requires an x64 build.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "MMSAdapter requires MSVC: its 37 export ordinals are pinned by MMSAdapter.def.")
endif()

# Static CRT (/MT) needs the CMP0091 runtime-library selection.
cmake_policy(SET CMP0091 NEW)

add_library(MMSAdapter SHARED
    adapter.cpp
    # Module definition file: the linker exports these 37 names with the host's
    # original ordinals. CMake passes a .def source to the MSVC linker as-is.
    MMSAdapter.def
)

target_compile_features(MMSAdapter PRIVATE cxx_std_17)

target_compile_definitions(MMSAdapter PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0601
    _WIN32_WINNT=0x0601
)

# Same MSVC language settings as the host project: UTF-8 sources, strict
# conformance, the repository warning level.
target_compile_options(MMSAdapter PRIVATE
    /utf-8
    /W3
    /permissive-
)

set_target_properties(MMSAdapter PROPERTIES
    OUTPUT_NAME MMSAdapter
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
