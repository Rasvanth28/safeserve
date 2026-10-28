# SafeServe build
#
#   make              build bin/safeserve and bin/loadgen
#   make test         unit, property and concurrency tests
#   make e2e          start the server and test it over HTTP
#   make demo         in-process deadlock demo: same workload under all four modes
#   make demo-live    the same demo against the real HTTP server
#   make live         live demo at http://localhost:8080/: all four strategies, controlled from the dashboard
#   make bench        benchmark sweep -> results/bench.csv (see tools/bench.sh)
#   make plot         charts from results/bench.csv
#   make asan / tsan  tests under AddressSanitizer / ThreadSanitizer
#
# Sanitizer builds go to their own directory (build-address/, build-thread/)
# so they never mix objects with the normal build.

CC      ?= cc
SAN     ?=
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -g
CFLAGS  += -pthread -Iinclude -MMD -MP
LDFLAGS += -pthread

ifeq ($(SAN),)
BUILD := build
BIN   := bin
else
BUILD := build-$(SAN)
BIN   := $(BUILD)/bin
CFLAGS  += -fsanitize=$(SAN) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SAN)
endif

CORE := config log net http queue banker naive lockorder detect rm metrics handlers worker
CORE_OBJS := $(CORE:%=$(BUILD)/src/%.o)

TESTS := test_banker test_detect test_queue test_http test_modes test_rm_stress
TEST_BINS := $(TESTS:%=$(BIN)/%)

all: $(BIN)/safeserve $(BIN)/loadgen

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN):
	mkdir -p $@

$(BIN)/safeserve: $(CORE_OBJS) $(BUILD)/src/main.o | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/loadgen: $(BUILD)/tools/loadgen.o | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

$(BIN)/demo_runner: $(CORE_OBJS) $(BUILD)/tools/demo_runner.o | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/test_%: $(CORE_OBJS) $(BUILD)/tests/test_%.o | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	@echo "=== Running unit, property and concurrency tests ==="
	@set -e; for t in $(TEST_BINS); do echo "--- $$t"; ./$$t; done
	@echo "=== All tests passed ==="

e2e: $(BIN)/safeserve
	SAFESERVE=$(BIN)/safeserve ./tests/e2e.sh

demo: $(BIN)/demo_runner
	./$(BIN)/demo_runner

demo-live: $(BIN)/safeserve
	SAFESERVE=$(BIN)/safeserve ./tools/deadlock_demo.sh

live: $(BIN)/safeserve
	SAFESERVE=$(BIN)/safeserve python3 tools/live_demo.py --open

bench: $(BIN)/safeserve $(BIN)/loadgen
	./tools/bench.sh

plot:
	python3 tools/plot.py

asan:
	$(MAKE) SAN=address test e2e

tsan:
	$(MAKE) SAN=thread test

clean:
	rm -rf build build-address build-thread build-undefined bin

.SECONDARY:
.PHONY: all test e2e demo demo-live live bench plot asan tsan clean

-include $(shell find build build-* -name '*.d' 2>/dev/null)
