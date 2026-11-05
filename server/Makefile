# NexusSync server — builds on Linux, macOS, and Windows (MSYS2 UCRT64).
#
#   make            release build      -> build/nexusd
#   make debug      -O0 -g             -> build/debug/nexusd
#   make asan       ASan + UBSan       -> build/asan/nexusd   (Linux, macOS)
#   make tsan       ThreadSanitizer    -> build/tsan/  + runs the tests
#   make test       unit + integration tests (under asan where available)
#   make clean
#
# Written for GNU make 3.81 (the version macOS ships), so no newer features.

CC ?= cc
ifeq ($(CC),)
CC := cc
endif

# ---- platform detection -----------------------------------------------------

ifeq ($(OS),Windows_NT)
  NX_OS := windows
  EXE := .exe
  PLAT_SRC := src/platform/plat_win32.c
  # -static: nexusd.exe runs on Windows machines without MSYS2 installed.
  OS_LDLIBS := -static -lpthread -lws2_32 -liphlpapi -lbcrypt
  SANITIZERS_OK := 0
else
  UNAME_S := $(shell uname -s)
  EXE :=
  PLAT_SRC := src/platform/plat_posix.c
  ifeq ($(UNAME_S),Darwin)
    NX_OS := macos
    OS_LDLIBS := -lpthread
    # With only the Command Line Tools installed, a leftover newer (beta)
    # SDK can be picked by default while the linker is older, which fails
    # with "tapi error: unknown architecture". The CLT installer keeps
    # MacOSX.sdk pointing at the SDK that matches its linker, so prefer it.
    # An SDKROOT set in the environment always wins.
    ifeq ($(origin SDKROOT),undefined)
      CLT_SDK := /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
      ifeq ($(shell xcode-select -p 2>/dev/null),/Library/Developer/CommandLineTools)
        ifneq ($(wildcard $(CLT_SDK)),)
          export SDKROOT := $(CLT_SDK)
        endif
      endif
    endif
  else
    NX_OS := linux
    OS_LDLIBS := -lpthread -lm
  endif
  SANITIZERS_OK := 1
endif

# ---- flags --------------------------------------------------------------------

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wpointer-arith -Wformat=2 -Werror=vla -Werror=implicit-function-declaration
ifeq ($(WERROR),1)
  WARN += -Werror
endif

BASE_CFLAGS := -std=c11 $(WARN) -Iinclude -Isrc -Ivendor -MMD -MP
# Tests may include the test header and engine internals.
TEST_CFLAGS := -Itests

MODE ?= release
ifeq ($(MODE),release)
  MODE_CFLAGS := -O2 -g -DNDEBUG
  MODE_LDFLAGS :=
  OUT := build
else ifeq ($(MODE),debug)
  MODE_CFLAGS := -O0 -g
  MODE_LDFLAGS :=
  OUT := build/debug
else ifeq ($(MODE),asan)
  MODE_CFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
  MODE_LDFLAGS := -fsanitize=address,undefined
  OUT := build/asan
else ifeq ($(MODE),tsan)
  MODE_CFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=thread
  MODE_LDFLAGS := -fsanitize=thread
  OUT := build/tsan
else
  $(error unknown MODE=$(MODE))
endif

ALL_CFLAGS := $(BASE_CFLAGS) $(MODE_CFLAGS) $(CFLAGS)
ALL_LDFLAGS := $(MODE_LDFLAGS) $(LDFLAGS)

# ---- sources ------------------------------------------------------------------

# Every .c under src/ except main.c and the platform file for the other OS.
LIB_SRC := $(wildcard src/util/*.c src/crypto/*.c src/engine/*.c src/net/*.c src/api/*.c) \
           src/platform/plat_common.c $(PLAT_SRC) \
           $(wildcard vendor/*.c)
LIB_OBJ := $(patsubst %.c,$(OUT)/obj/%.o,$(LIB_SRC))
LIB := $(OUT)/libnexus.a

MAIN_OBJ := $(OUT)/obj/src/main.o
NEXUSD := $(OUT)/nexusd$(EXE)

UNIT_SRC := $(wildcard tests/unit/test_*.c)
INTEG_SRC := $(wildcard tests/integration/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(OUT)/tests/%$(EXE),$(UNIT_SRC) $(INTEG_SRC))

# ---- top-level targets ----------------------------------------------------------

.PHONY: all bins debug asan tsan test test-run clean print-os

all:
	@$(MAKE) --no-print-directory MODE=release bins

debug:
	@$(MAKE) --no-print-directory MODE=debug bins

ifeq ($(SANITIZERS_OK),1)
asan:
	@$(MAKE) --no-print-directory MODE=asan bins

# Tests run under ASan + UBSan by default (CLAUDE.md "Commands").
test:
	@$(MAKE) --no-print-directory MODE=asan bins test-run

tsan:
	@$(MAKE) --no-print-directory MODE=tsan bins test-run
else
asan tsan:
	@echo "Sanitizers are not available with MinGW on Windows; run them on Linux or macOS (CI does)." >&2
	@exit 1

test:
	@$(MAKE) --no-print-directory MODE=debug bins test-run
endif

bins: $(NEXUSD) $(TEST_BINS)

print-os:
	@echo $(NX_OS)

# Runs every test executable; stops at the first failing one.
# ASAN_OPTIONS: detect_leaks is unsupported on macOS arm64, so it is only
# requested on Linux.
ifeq ($(NX_OS),linux)
  SAN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
else
  SAN_ENV := UBSAN_OPTIONS=print_stacktrace=1
endif

test-run: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
	  echo "== $$t"; \
	  NX_TEST_TMP=$(OUT)/tmp $(SAN_ENV) ./$$t; \
	done; echo "All test executables passed ($(MODE), $(NX_OS))."

clean:
	rm -rf build

# ---- rules ------------------------------------------------------------------------

# Test objects first: make 3.81 uses the first pattern rule that matches.
$(OUT)/obj/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -c $< -o $@

$(OUT)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(NEXUSD): $(MAIN_OBJ) $(LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(OS_LDLIBS)

$(OUT)/tests/%$(EXE): $(OUT)/obj/tests/%.o $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(OS_LDLIBS)

-include $(shell find build -name '*.d' 2>/dev/null)

# Keep object files that make would otherwise treat as intermediate and delete.
.SECONDARY:
