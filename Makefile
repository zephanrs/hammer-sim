MAKEFLAGS += --no-print-directory

CXX ?= c++
PYTHON ?= python3
BASE_CXXFLAGS ?= -O2 -std=gnu++20 -Wno-register -pthread
WARNINGS ?= 0
VERBOSE ?= 0

ifeq ($(WARNINGS),1)
CXXFLAGS := $(BASE_CXXFLAGS) -Wall -Wextra -pedantic
else
CXXFLAGS := $(BASE_CXXFLAGS)
endif

ifeq ($(VERBOSE),1)
Q :=
else
Q := @
endif

APP_DIR ?= ../hammerblade
APP_NAME ?= $(notdir $(abspath $(APP_DIR)))
HOST_SRC ?= $(APP_DIR)/main.cpp
KERNEL_SRC ?= $(APP_DIR)/kernel.cpp
TEST_DEFS_MK := $(APP_DIR)/test_defs.mk
TESTS_MK := $(APP_DIR)/tests.mk
TARGET ?= hammer-sim
BUILD_ROOT := build
BIN_ROOT := bin
BUILD_DIR := $(BUILD_ROOT)/$(APP_NAME)
BIN_DIR := $(BIN_ROOT)/$(APP_NAME)
APP_DIR_ABS := $(abspath $(APP_DIR))
RUNNER_MAKEFILE := $(BIN_DIR)/Makefile

TILE_GROUP_DIM_X ?= 16
TILE_GROUP_DIM_Y ?= 8

INCLUDES := -Iinclude -I$(APP_DIR) -I.
COMMON_DEFINES := -DHB_NATIVE_SIM -DTILE_GROUP_DIM_X=$(TILE_GROUP_DIM_X) -DTILE_GROUP_DIM_Y=$(TILE_GROUP_DIM_Y)

TESTS =
ifneq ($(MAKECMDGOALS),clean)
include $(TEST_DEFS_MK)
include $(TESTS_MK)
endif

NATIVE_BINS := $(addprefix $(BIN_DIR)/,$(addsuffix /$(TARGET),$(TESTS)))

.PHONY: all run run-all test clean list-bins $(addprefix run-,$(TESTS))
.SECONDARY:

all: $(NATIVE_BINS) $(RUNNER_MAKEFILE)
	@printf '%s\n' $(NATIVE_BINS)
	@printf 'runner makefile: %s\n' $(RUNNER_MAKEFILE)

$(BUILD_DIR)/%/generated_kernel.cpp: $(KERNEL_SRC) scripts/generate_kernel.py runtime.hpp
	$(Q)mkdir -p $(dir $@)
	$(Q)$(PYTHON) scripts/generate_kernel.py $(KERNEL_SRC) $@

$(BIN_DIR)/%/$(TARGET): runtime.cpp driver_main.cpp $(HOST_SRC) $(BUILD_DIR)/%/generated_kernel.cpp
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) $(COMMON_DEFINES) \
		$(call native-defines-for-test,$*) \
		$(INCLUDES) -o $@ runtime.cpp driver_main.cpp $(HOST_SRC) $(BUILD_DIR)/$*/generated_kernel.cpp

$(RUNNER_MAKEFILE): app_bin.mk $(TEST_DEFS_MK) $(TESTS_MK)
	$(Q)mkdir -p $(dir $@)
	$(Q){ \
		printf 'APP_DIR := %s\n' '$(APP_DIR_ABS)'; \
		printf 'APP_BIN_DIR := %s\n' '$(abspath $(BIN_DIR))'; \
		printf 'TARGET := %s\n' '$(TARGET)'; \
		printf 'include ../../app_bin.mk\n'; \
	} > $@

run: $(RUNNER_MAKEFILE)
	$(MAKE) -C $(BIN_DIR) run-all

run-all: run
test: run

list-bins:
	@printf '%s\n' $(NATIVE_BINS)

clean:
	$(Q)rm -rf $(BUILD_ROOT) $(BIN_ROOT) $(TARGET)
