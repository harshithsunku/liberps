# SPDX-License-Identifier: Apache-2.0
#
# liberps build.  Usage:
#   make                      build library, apps (default compiler, -O2)
#   make test                 build + run unit and scenario tests
#   make CC=clang SAN=address,undefined test
#   make check                gcc+clang x {plain, ASan+UBSan} x all tests (CI gate)
#   make cov                  gcc --coverage, run tests, per-file line coverage of the core
#   make fuzz                 clang libFuzzer targets (FUZZ_SECONDS=60 per target)
#   make install PREFIX=/usr/local
#
# Each (compiler, sanitizer, coverage) combination builds into its own directory under build/.

CC        ?= cc
SAN       ?=
COV       ?= 0
OPT       ?= -O2 -g
PREFIX    ?= /usr/local
WERROR    ?= 1

comma := ,
CCNAME    := $(notdir $(firstword $(CC)))
BUILD     ?= build/$(CCNAME)$(if $(SAN),-$(subst $(comma),-,$(SAN)))$(if $(filter 1,$(COV)),-cov)

WARN      := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes \
             -Wmissing-prototypes -Wold-style-definition -Wcast-qual -Wpointer-arith \
             -Wformat=2 -Wundef -Wvla -Wimplicit-fallthrough -Wwrite-strings
ifeq ($(WERROR),1)
WARN      += -Werror
endif

CPPFLAGS  += -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS    += -std=c11 $(OPT) $(WARN) -fPIC -fvisibility=hidden -fno-strict-aliasing
LDFLAGS   +=
LDLIBS    +=

ifneq ($(SAN),)
CFLAGS    += -fsanitize=$(SAN) -fno-omit-frame-pointer -fno-sanitize-recover=all
LDFLAGS   += -fsanitize=$(SAN)
endif
ifeq ($(COV),1)
CFLAGS    += --coverage -O0
LDFLAGS   += --coverage
endif

UNAME_S   := $(shell uname -s)

# ---- sources --------------------------------------------------------------------------------
CORE_SRC  := $(wildcard src/core/*.c)
CLI_SRC   := $(wildcard src/cli/*.c)
SIM_SRC   := $(wildcard src/platform/sim/*.c)
LINUX_SRC := $(wildcard src/platform/linux/*.c)

LIB_SRC   := $(CORE_SRC) $(CLI_SRC)
LIB_OBJ   := $(LIB_SRC:%.c=$(BUILD)/obj/%.o)
SIM_OBJ   := $(SIM_SRC:%.c=$(BUILD)/obj/%.o)
LINUX_OBJ := $(LINUX_SRC:%.c=$(BUILD)/obj/%.o)

UNIT_SRC  := $(wildcard tests/unit/*.c)
SCEN_SRC  := $(wildcard tests/scenario/*.c)
CHAOS_SRC := $(wildcard tests/chaos/*.c)
HARN_SRC  := tests/harness.c

STATIC    := $(BUILD)/liberps.a
SHARED    := $(BUILD)/liberps.so
SIMLIB    := $(BUILD)/liberps-sim.a

APPS      := $(if $(wildcard apps/erps-sim.c),$(BUILD)/erps-sim)
ifeq ($(UNAME_S),Linux)
APPS      += $(if $(wildcard apps/erpsd.c),$(BUILD)/erpsd) $(if $(wildcard apps/erpsctl.c),$(BUILD)/erpsctl)
endif

TESTS     := $(if $(UNIT_SRC),$(BUILD)/test-unit) $(if $(SCEN_SRC),$(BUILD)/test-scenario) \
             $(if $(CHAOS_SRC),$(BUILD)/test-chaos)

.PHONY: all lib apps tests test check cov fuzz install clean distclean help examples
EXAMPLES  := $(patsubst examples/%.c,$(BUILD)/%,$(wildcard examples/*.c))

all: lib apps examples
examples: $(EXAMPLES)

$(BUILD)/%: $(BUILD)/obj/examples/%.o $(STATIC)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

lib: $(STATIC) $(SHARED)
apps: $(APPS)
tests: $(TESTS)

# ---- compile ----------------------------------------------------------------------------------
$(BUILD)/obj/src/%.o: CPPFLAGS += -DERPS_BUILDING
$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

$(STATIC): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SHARED): $(LIB_OBJ)
	$(CC) -shared $(LDFLAGS) -Wl,-soname,liberps.so.0 -o $@ $^

$(SIMLIB): $(SIM_OBJ)
	$(AR) rcs $@ $^

# ---- apps -------------------------------------------------------------------------------------
$(BUILD)/erps-sim: $(BUILD)/obj/apps/erps-sim.o $(SIMLIB) $(STATIC)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/erpsd: $(BUILD)/obj/apps/erpsd.o $(LINUX_OBJ) $(STATIC)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/erpsctl: $(BUILD)/obj/apps/erpsctl.o $(STATIC)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ---- tests ------------------------------------------------------------------------------------
# Tests link the objects directly (not the .so) so internal symbols are reachable.
TEST_CPPFLAGS := -Itests -DERPS_TESTING

$(BUILD)/obj/tests/%.o: CPPFLAGS += $(TEST_CPPFLAGS)

$(BUILD)/test-unit: $(UNIT_SRC:%.c=$(BUILD)/obj/%.o) $(HARN_SRC:%.c=$(BUILD)/obj/%.o) $(LIB_OBJ) $(SIM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test-scenario: $(SCEN_SRC:%.c=$(BUILD)/obj/%.o) $(HARN_SRC:%.c=$(BUILD)/obj/%.o) $(LIB_OBJ) $(SIM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test-chaos: $(CHAOS_SRC:%.c=$(BUILD)/obj/%.o) $(HARN_SRC:%.c=$(BUILD)/obj/%.o) $(LIB_OBJ) $(SIM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: all tests
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t $(TEST_ARGS); done

# ---- CI gate ----------------------------------------------------------------------------------
CHECK_CCS  ?= gcc clang
check:
	@set -e; for cc in $(CHECK_CCS); do \
	  for san in "" "address,undefined"; do \
	    echo "######## CC=$$cc SAN=$$san"; \
	    $(MAKE) --no-print-directory CC=$$cc SAN=$$san test; \
	  done; done
	@echo "######## check: ALL GREEN"

# ---- coverage ---------------------------------------------------------------------------------
cov:
	@rm -rf build/gcc-cov
	$(MAKE) --no-print-directory CC=gcc COV=1 test
	@python3 -I tools/covsum.py build/gcc-cov src/core

# ---- fuzzing ----------------------------------------------------------------------------------
FUZZ_SRC     := $(wildcard tests/fuzz/fuzz_*.c)
FUZZ_BUILD   := build/fuzz
FUZZ_SECONDS ?= 60
FUZZ_FLAGS   := -std=c11 -O1 -g -Iinclude -Isrc -Itests -DERPS_TESTING -D_DEFAULT_SOURCE \
                -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
fuzz:
	@mkdir -p $(FUZZ_BUILD)
	@set -e; for f in $(FUZZ_SRC); do \
	  n=$$(basename $$f .c); \
	  clang $(FUZZ_FLAGS) -o $(FUZZ_BUILD)/$$n $$f $(LIB_SRC) $(SIM_SRC); \
	  mkdir -p fuzz-corpus/$$n; \
	  echo "== $$n ($(FUZZ_SECONDS)s)"; \
	  $(FUZZ_BUILD)/$$n -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 fuzz-corpus/$$n; \
	done

# ---- install ----------------------------------------------------------------------------------
install: lib
	install -d $(DESTDIR)$(PREFIX)/include/erps $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 include/erps/*.h $(DESTDIR)$(PREFIX)/include/erps/
	install -m 644 $(STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(SHARED) $(DESTDIR)$(PREFIX)/lib/liberps.so.0
	ln -sf liberps.so.0 $(DESTDIR)$(PREFIX)/lib/liberps.so
	sed -e 's|@PREFIX@|$(PREFIX)|g' erps.pc.in > $(DESTDIR)$(PREFIX)/lib/pkgconfig/erps.pc

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build fuzz-corpus

help:
	@sed -n '3,12p' Makefile
