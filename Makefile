CC       ?= gcc
AR       ?= ar
NM       ?= nm

CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS   ?= -O2 -g
WARNINGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes
NO_ALLOC_BUILTINS = -fno-builtin-malloc -fno-builtin-free \
                    -fno-builtin-calloc -fno-builtin-realloc
LDLIBS   += -pthread

BUILD_DIR = build
LIB       = $(BUILD_DIR)/libmemctl.a
ALLOC_OBJ = $(BUILD_DIR)/allocator.o
TEST_BIN  = $(BUILD_DIR)/test_runner
EXTRA_BIN = $(BUILD_DIR)/extra_tests
BENCH_BIN = $(BUILD_DIR)/bench
BASE_BIN  = $(BUILD_DIR)/bench_baseline
GLIBC_BIN = $(BUILD_DIR)/bench_glibc

.PHONY: all test check check-symbols extra bench clean prebuilt demo

all: $(LIB) $(TEST_BIN) $(EXTRA_BIN) $(BENCH_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(ALLOC_OBJ): src/allocator.c include/libmemctl.h include/libmemctl_stats.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(NO_ALLOC_BUILTINS) -c $< -o $@

$(LIB): $(ALLOC_OBJ)
	$(AR) rcs $@ $^

$(TEST_BIN): tests/test_runner.c include/libmemctl.h $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LIB) $(LDLIBS) -o $@

$(EXTRA_BIN): tests/extra_tests.c include/libmemctl.h $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< $(LIB) $(LDLIBS) -o $@

$(BENCH_BIN): bench/bench.c include/libmemctl.h include/libmemctl_stats.h $(LIB)
	$(CC) $(CPPFLAGS) -DLIBMEMCTL_STATS $(CFLAGS) $(WARNINGS) $< $(LIB) $(LDLIBS) -o $@

$(BASE_BIN): bench/bench.c bench/baseline.c include/libmemctl.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(NO_ALLOC_BUILTINS) \
		bench/bench.c bench/baseline.c $(LDLIBS) -o $@

$(GLIBC_BIN): bench/bench.c bench/glibc_wrap.c include/libmemctl.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) \
		bench/bench.c bench/glibc_wrap.c $(LDLIBS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

extra: $(EXTRA_BIN)
	./$(EXTRA_BIN)

check-symbols: $(ALLOC_OBJ)
	@bad="$$( $(NM) -u $(ALLOC_OBJ) | sed 's/.* U //' | sed 's/@.*//' | \
		grep -E '^(malloc|free|calloc|realloc|aligned_alloc|posix_memalign|valloc|pvalloc|sbrk)$$' || true )"; \
	if [ -n "$$bad" ]; then \
		echo "Forbidden allocator symbol(s) referenced by allocator.o:"; \
		echo "$$bad"; exit 1; \
	else \
		echo "Allocator symbol check passed."; \
	fi

check: check-symbols test extra

bench: $(BENCH_BIN) $(BASE_BIN) $(GLIBC_BIN)
	./$(BENCH_BIN) > $(BUILD_DIR)/bench_libmemctl.txt
	./$(BASE_BIN) > $(BUILD_DIR)/bench_baseline.txt
	./$(GLIBC_BIN) > $(BUILD_DIR)/bench_glibc.txt
	@echo "Wrote $(BUILD_DIR)/bench_libmemctl.txt, bench_baseline.txt, bench_glibc.txt"

prebuilt: all $(BASE_BIN) $(GLIBC_BIN)
	mkdir -p prebuilt
	cp -f $(LIB) $(TEST_BIN) $(EXTRA_BIN) $(BENCH_BIN) $(BASE_BIN) $(GLIBC_BIN) prebuilt/
	cp -f $(BUILD_DIR)/bench_libmemctl.txt $(BUILD_DIR)/bench_baseline.txt $(BUILD_DIR)/bench_glibc.txt prebuilt/ 2>/dev/null || true

demo: check bench
	@echo "Demo run complete. See build/bench_*.txt"

clean:
	rm -rf $(BUILD_DIR)
