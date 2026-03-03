MAKEFLAGS += --no-print-directory

TARGET ?= hammer-sim
VERBOSE ?= 0
APP_NAME ?= $(notdir $(APP_DIR))
DEADLOCK_EXIT_CODE ?= 2

ifeq ($(VERBOSE),1)
Q :=
else
Q := @
endif

include $(APP_DIR)/test_defs.mk

TESTS =
include $(APP_DIR)/tests.mk

MAX_TEST_NAME_LEN := $(shell printf '%s\n' $(TESTS) | awk 'length > max { max = length } END { print max + 0 }')
SUMMARY_WIDTH := $(shell width=$(MAX_TEST_NAME_LEN); if [ $$width -lt 20 ]; then echo 20; else echo $$width; fi)

.PHONY: run run-all test list-bins clean $(addprefix run-,$(TESTS))

run: run-all
run-all:
	@set -eu; \
	green='\033[0;32m'; \
	red='\033[0;31m'; \
	reset='\033[0m'; \
	width='$(SUMMARY_WIDTH)'; \
	rc=0; \
	printf '%s:\n' '$(APP_NAME)'; \
	printf '%*s\n' 70 '' | tr ' ' '-'; \
	$(foreach test,$(TESTS), \
		log=$$(mktemp); \
		if ./$(test)/$(TARGET) $(call native-program-args-for-test,$(test)) >"$$log" 2>&1; then \
			status="$${green}PASSED$${reset}"; \
		else \
			code=$$?; \
			if [ $$code -eq $(DEADLOCK_EXIT_CODE) ]; then \
				status="$${red}DEADLOCK$${reset}"; \
				rc=$(DEADLOCK_EXIT_CODE); \
			else \
				status="$${red}FAILED$${reset}"; \
				if [ $$rc -eq 0 ]; then rc=1; fi; \
			fi; \
		fi; \
		printf '%-*s  %b\n' "$$width" '$(test)' "$$status"; \
		rm -f "$$log"; \
	) \
	printf '\n'; \
	exit $$rc
test: run-all

list-bins:
	@printf '%s\n' $(addprefix $(CURDIR)/,$(addsuffix /$(TARGET),$(TESTS)))

clean:
	$(Q)rm -rf $(APP_BIN_DIR)

define RUN_TEST_template
run-$(1): $(1)/$(TARGET)
	$(Q)./$(1)/$(TARGET) $(call native-program-args-for-test,$(1))
endef

$(foreach test,$(TESTS),$(eval $(call RUN_TEST_template,$(test))))
