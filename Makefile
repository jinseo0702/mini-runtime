.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -O0 -g
LDFLAGS ?=
LDLIBS ?=
MODE ?= normal
TEST_TIMEOUT ?= 90s

ifeq ($(filter $(MODE),normal asan tsan),)
$(error MODE must be normal, asan, or tsan)
endif

BUILD_DIR := .build/$(MODE)
DEMO := $(BUILD_DIR)/noop
TEST_SOURCES := $(sort $(wildcard tests/unit/*_test.cpp))
TEST_BINARIES := $(patsubst tests/unit/%.cpp,$(BUILD_DIR)/tests/%,$(TEST_SOURCES))
CONFIG := $(BUILD_DIR)/.compile-config

ifeq ($(strip $(TEST_SOURCES)),)
$(error No unit test sources found in tests/unit)
endif

# Keep C++20, threads, warnings, and assertions enabled with custom CXXFLAGS.
REQUIRED_CPPFLAGS := -Iinclude
REQUIRED_CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -pedantic -pthread -UNDEBUG
REQUIRED_LDFLAGS := -pthread
RUN_ENV := env

ifeq ($(MODE),asan)
REQUIRED_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
REQUIRED_LDFLAGS += -fsanitize=address,undefined
RUN_ENV += ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 LSAN_OPTIONS=exitcode=23
endif
ifeq ($(MODE),tsan)
REQUIRED_CXXFLAGS += -fsanitize=thread -fno-omit-frame-pointer -fPIE
REQUIRED_LDFLAGS += -fsanitize=thread -pie
RUN_ENV += TSAN_OPTIONS=halt_on_error=1:exitcode=66
endif

# A changed compiler or flag set rebuilds binaries without requiring make clean.
define BUILD_CONFIGURATION
CXX=$(CXX)
CPPFLAGS=$(CPPFLAGS) $(REQUIRED_CPPFLAGS)
CXXFLAGS=$(CXXFLAGS) $(REQUIRED_CXXFLAGS)
LDFLAGS=$(LDFLAGS) $(REQUIRED_LDFLAGS)
LDLIBS=$(LDLIBS)
endef

.PHONY: all demo tests run test test-asan test-tsan clean help FORCE

all: demo tests
demo: $(DEMO)
tests: $(TEST_BINARIES)

run: $(DEMO)
	$(RUN_ENV) timeout "$(TEST_TIMEOUT)" "$(DEMO)"

test: tests
	@set -eu; \
	for binary in $(TEST_BINARIES); do \
		printf 'RUN  %s\n' "$$binary"; \
		$(RUN_ENV) timeout "$(TEST_TIMEOUT)" "$$binary"; \
		printf 'PASS %s\n' "$$binary"; \
	done; \
	printf 'All %s test programs passed (%s).\n' "$(words $(TEST_BINARIES))" "$(MODE)"

test-asan:
	$(MAKE) MODE=asan test

test-tsan:
	$(MAKE) MODE=tsan test

$(BUILD_DIR) $(BUILD_DIR)/tests:
	mkdir -p "$@"

$(CONFIG): export MINI_RUNTIME_COMPILE_CONFIG = $(BUILD_CONFIGURATION)
$(CONFIG): FORCE | $(BUILD_DIR)
	@set -eu; \
	temporary="$$(mktemp "$@.XXXXXX")"; \
	trap 'rm -f "$$temporary"' 0; \
	printf '%s\n' "$$MINI_RUNTIME_COMPILE_CONFIG" > "$$temporary"; \
	if ! cmp -s "$$temporary" "$@"; then mv "$$temporary" "$@"; fi

$(DEMO): examples/noop.cpp $(CONFIG) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(REQUIRED_CPPFLAGS) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) -MMD -MP -MF "$@.d" "$<" $(LDFLAGS) $(REQUIRED_LDFLAGS) $(LDLIBS) -o "$@"

$(BUILD_DIR)/tests/%: tests/unit/%.cpp $(CONFIG) | $(BUILD_DIR)/tests
	$(CXX) $(CPPFLAGS) $(REQUIRED_CPPFLAGS) $(CXXFLAGS) $(REQUIRED_CXXFLAGS) -MMD -MP -MF "$@.d" "$<" $(LDFLAGS) $(REQUIRED_LDFLAGS) $(LDLIBS) -o "$@"

-include $(DEMO).d $(addsuffix .d,$(TEST_BINARIES))

clean:
	rm -rf -- .build

help:
	@printf '%s\n' \
		'make                  Build the noop demo and all unit test programs' \
		'make run              Run the asynchronous MockBackend demo' \
		'make test             Run all unit test programs; stop on any failure' \
		'make test-asan        Run tests with ASan, UBSan, and leak detection' \
		'make test-tsan        Run tests with ThreadSanitizer' \
		'make MODE=asan demo   Build only the sanitized demo' \
		'make CXX=clang++      Select a different C++20 compiler' \
		'make clean            Remove generated files under .build/' \
		'TEST_TIMEOUT=90s      Maximum time for each run or test program'
