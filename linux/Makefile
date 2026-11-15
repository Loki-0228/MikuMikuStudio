# Native Linux build of the platform-independent subset, not the Windows GUI.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

linux_dir := $(dir $(lastword $(MAKEFILE_LIST)))
source_dir := $(abspath $(linux_dir)/..)
BUILD_DIR ?= $(linux_dir)build
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
PYTHON ?= python3

ifneq ($(shell uname -s),Linux)
$(error This entry point builds the native Linux CPU targets only)
endif

curve_sources := src/animation/animation_curve.cpp src/animation/animation_curve_insert.cpp src/animation/animation_retime.cpp
curve_objects := $(addprefix $(BUILD_DIR)/obj/,$(curve_sources:.cpp=.o))
curve_library := $(BUILD_DIR)/libmms_animation_curve.a

smoke_tests := animation_curve_tests background_gesture_core_tests workspace_layout_preview_tests checked_glyph_tests \
	effect_save_state_tests offscreen_gdi_owner_tests offscreen_gdi_runtime_tests \
	offscreen_gdi_surface_tests status_extension_invalidation_tests legacy_mmd_menu_tests \
	pmd_geometry_wire_tests mesh_bake_bone_selection_tests mesh_bake_index_filter_tests \
	autosave_rotation_tests project_recovery_cleanup_tests plugins/snapshot_cpp_safety_tests \
	plugins/settings_cpp_tests
tests := $(smoke_tests) animation_retime_tests
test_bins := $(addprefix $(BUILD_DIR)/tests/,$(tests))
test_objects := $(addprefix $(BUILD_DIR)/obj/tests/,$(addsuffix .o,$(tests)))
retime_test := $(BUILD_DIR)/tests/animation_retime_tests
format_inspect := $(BUILD_DIR)/mms_format_inspect
format_inspect_obj := $(BUILD_DIR)/obj/tools/mms_format_inspect.o
format_inspect_test := $(BUILD_DIR)/tests/test_mms_pmm_directory
pmx_metadata_test := $(BUILD_DIR)/tests/test_mms_pmx_metadata
pmx_metadata_test_obj := $(BUILD_DIR)/obj/tools/test_mms_pmx_metadata.o
format_codec_obj := $(BUILD_DIR)/obj/src/io/pmm_container.o
format_test_objs := $(BUILD_DIR)/obj/tools/test_mms_pmm_directory.o $(format_codec_obj)
workspace_layout_obj := $(BUILD_DIR)/obj/src/plugins/workspace_layout_preview.o

.PHONY: all check check-retime format-inspect check-format-inspect
all: $(curve_library) $(test_bins) $(format_inspect)

format-inspect: $(format_inspect)

check-format-inspect: $(format_inspect) $(format_inspect_test) $(pmx_metadata_test)
	$(PYTHON) "$(source_dir)/tools/test_mms_format_inspect.py" "$<"
	$(PYTHON) "$(source_dir)/tools/test_mms_pmm_directory.py" "$<" "$(format_inspect_test)"
	"$(pmx_metadata_test)"
	$(PYTHON) "$(source_dir)/tools/test_mms_pmx_metadata.py" "$<"

$(format_codec_obj): private override CPPFLAGS += -D_FILE_OFFSET_BITS=64 -I"$(source_dir)/linux/test_compat"

$(format_inspect_test): $(format_test_objs)
$(pmx_metadata_test): $(pmx_metadata_test_obj)
$(format_inspect_test) $(pmx_metadata_test):
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

$(format_inspect): $(format_inspect_obj)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

check: $(addprefix $(BUILD_DIR)/tests/,$(smoke_tests))
	@set -e; for test in $^; do \
		printf '\n== %s ==\n' "$${test##*/}"; \
		"$$test"; \
	done

# Keep this existing regression runnable without declaring it a passing smoke test.
check-retime: $(retime_test)
	"$<"

$(curve_library): $(curve_objects)
	$(AR) rcs "$@" $^

$(test_bins): $(BUILD_DIR)/tests/%: $(BUILD_DIR)/obj/tests/%.o
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -pthread $^ $(LDLIBS) -o "$@"

$(BUILD_DIR)/tests/animation_curve_tests $(retime_test): $(curve_library)
$(BUILD_DIR)/tests/workspace_layout_preview_tests: $(workspace_layout_obj)

# Only this CPU fixture includes the Windows record-layout assertions.
$(BUILD_DIR)/obj/tests/mesh_bake_bone_selection_tests.o: private override CXXFLAGS += -fshort-wchar

# Mock SDK tables only: keep the real plugin ABI Windows x64.
$(BUILD_DIR)/obj/tests/plugins/snapshot_cpp_safety_tests.o \
$(BUILD_DIR)/obj/tests/plugins/settings_cpp_tests.o: private override CPPFLAGS += \
	-I"$(source_dir)/sdk/mms/include" -D_WIN32 -D_WIN64 -D__cdecl=

$(BUILD_DIR)/obj/%.o: $(source_dir)/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) -I"$(source_dir)/include" -I"$(source_dir)/src" \
		$(CXXFLAGS) -std=c++17 -pthread -MMD -MP -c "$<" -o "$@"

-include $(curve_objects:.o=.d) $(test_objects:.o=.d) $(format_inspect_obj:.o=.d) $(format_test_objs:.o=.d) $(pmx_metadata_test_obj:.o=.d) $(workspace_layout_obj:.o=.d)
