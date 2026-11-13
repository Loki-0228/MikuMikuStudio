# Native Linux build of the platform-independent subset, not the Windows GUI.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

linux_dir := $(dir $(lastword $(MAKEFILE_LIST)))
source_dir := $(abspath $(linux_dir)/..)
BUILD_DIR ?= $(linux_dir)build
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

ifneq ($(shell uname -s),Linux)
$(error This entry point builds the native Linux CPU targets only)
endif

curve_sources := src/animation/animation_curve.cpp src/animation/animation_curve_insert.cpp src/animation/animation_retime.cpp
curve_objects := $(addprefix $(BUILD_DIR)/obj/,$(curve_sources:.cpp=.o))
curve_library := $(BUILD_DIR)/libmms_animation_curve.a

smoke_tests := animation_curve_tests background_gesture_core_tests checked_glyph_tests \
	effect_save_state_tests offscreen_gdi_owner_tests offscreen_gdi_runtime_tests \
	offscreen_gdi_surface_tests status_extension_invalidation_tests
tests := $(smoke_tests) animation_retime_tests
test_bins := $(addprefix $(BUILD_DIR)/tests/,$(tests))
test_objects := $(addprefix $(BUILD_DIR)/obj/tests/,$(addsuffix .o,$(tests)))
retime_test := $(BUILD_DIR)/tests/animation_retime_tests

.PHONY: all check check-retime
all: $(curve_library) $(test_bins)

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

$(BUILD_DIR)/obj/%.o: $(source_dir)/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) -I"$(source_dir)/include" -I"$(source_dir)/src" \
		$(CXXFLAGS) -std=c++17 -pthread -MMD -MP -c "$<" -o "$@"

-include $(curve_objects:.o=.d) $(test_objects:.o=.d)
