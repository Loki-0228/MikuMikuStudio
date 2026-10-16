# Included from the full-profile UI test block. The fixture creates only hidden
# native controls; it does not require a renderer, application host or GPU.
add_executable(scene_history_ui_tests EXCLUDE_FROM_ALL tests/scene_history_ui_tests.cpp)
target_link_libraries(scene_history_ui_tests PRIVATE mikudancestudio_core winmm dsound)
set_target_properties(scene_history_ui_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
add_test(NAME scene_history_ui_regressions COMMAND scene_history_ui_tests)
set_tests_properties(scene_history_ui_regressions PROPERTIES TIMEOUT 20)
