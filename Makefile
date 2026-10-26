CC = gcc
CFLAGS = -O3 -Wall -Wextra -std=c99 -Iinclude
LDFLAGS =

SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
TEST_DIR = tests
BENCH_DIR = benchmark

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

TARGET = $(BUILD_DIR)/append-only-btree-engine
TEST_TARGET = $(BUILD_DIR)/test_runner
BENCH_TARGET = $(BUILD_DIR)/bench_runner

.PHONY: all build test clean benchmark directories

all: directories build

directories:
	@mkdir -p $(BUILD_DIR)

build: directories $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: directories
	@if [ -d "$(TEST_DIR)" ] && [ "$$($(wildcard $(TEST_DIR)/*.c))" != "" ]; then \
		$(CC) $(CFLAGS) $(TEST_DIR)/*.c $(filter-out $(SRC_DIR)/main.c, $(SRCS)) -o $(TEST_TARGET) $(LDFLAGS); \
		./$(TEST_TARGET); \
	else \
		echo "No tests found in $(TEST_DIR). Skipping."; \
	fi

benchmark: directories
	@if [ -d "$(BENCH_DIR)" ] && [ "$$($(wildcard $(BENCH_DIR)/*.c))" != "" ]; then \
		$(CC) $(CFLAGS) $(BENCH_DIR)/*.c $(filter-out $(SRC_DIR)/main.c, $(SRCS)) -o $(BENCH_TARGET) $(LDFLAGS); \
		./$(BENCH_TARGET); \
	else \
		echo "No benchmarks found in $(BENCH_DIR). Skipping."; \
	fi

clean:
	@rm -rf $(BUILD_DIR)