CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -pthread -Iinclude
LDFLAGS ?= -pthread

BIN_DIR = bin
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests

SRCS = $(SRC_DIR)/config.c \
       $(SRC_DIR)/log.c \
       $(SRC_DIR)/net.c \
       $(SRC_DIR)/http.c \
       $(SRC_DIR)/queue.c \
       $(SRC_DIR)/banker.c \
       $(SRC_DIR)/naive.c \
       $(SRC_DIR)/lockorder.c \
       $(SRC_DIR)/detect.c \
       $(SRC_DIR)/rm.c \
       $(SRC_DIR)/metrics.c \
       $(SRC_DIR)/handlers.c \
       $(SRC_DIR)/worker.c

OBJS = $(SRCS:.c=.o)

all: $(BIN_DIR)/safeserve

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/safeserve: $(BIN_DIR) $(OBJS) $(SRC_DIR)/main.o
	$(CC) $(CFLAGS) $(OBJS) $(SRC_DIR)/main.o -o $@ $(LDFLAGS)

# Tests
$(BIN_DIR)/test_banker: $(BIN_DIR) $(SRC_DIR)/banker.o $(TEST_DIR)/test_banker.c
	$(CC) $(CFLAGS) $(SRC_DIR)/banker.o $(TEST_DIR)/test_banker.c -o $@ $(LDFLAGS)

$(BIN_DIR)/test_queue: $(BIN_DIR) $(SRC_DIR)/queue.o $(TEST_DIR)/test_queue.c
	$(CC) $(CFLAGS) $(SRC_DIR)/queue.o $(TEST_DIR)/test_queue.c -o $@ $(LDFLAGS)

$(BIN_DIR)/test_http: $(BIN_DIR) $(SRC_DIR)/http.o $(SRC_DIR)/log.o $(TEST_DIR)/test_http.c
	$(CC) $(CFLAGS) $(SRC_DIR)/http.o $(SRC_DIR)/log.o $(TEST_DIR)/test_http.c -o $@ $(LDFLAGS)

$(BIN_DIR)/test_rm_stress: $(BIN_DIR) $(SRC_DIR)/rm.o $(SRC_DIR)/banker.o $(SRC_DIR)/naive.o $(SRC_DIR)/lockorder.o $(SRC_DIR)/detect.o $(SRC_DIR)/metrics.o $(SRC_DIR)/log.o $(TEST_DIR)/test_rm_stress.c
	$(CC) $(CFLAGS) $(SRC_DIR)/rm.o $(SRC_DIR)/banker.o $(SRC_DIR)/naive.o $(SRC_DIR)/lockorder.o $(SRC_DIR)/detect.o $(SRC_DIR)/metrics.o $(SRC_DIR)/log.o $(TEST_DIR)/test_rm_stress.c -o $@ $(LDFLAGS)

$(BIN_DIR)/demo_runner: $(BIN_DIR) $(SRC_DIR)/rm.o $(SRC_DIR)/banker.o $(SRC_DIR)/naive.o $(SRC_DIR)/lockorder.o $(SRC_DIR)/detect.o $(SRC_DIR)/metrics.o $(SRC_DIR)/log.o tools/demo_runner.c
	$(CC) $(CFLAGS) $(SRC_DIR)/rm.o $(SRC_DIR)/banker.o $(SRC_DIR)/naive.o $(SRC_DIR)/lockorder.o $(SRC_DIR)/detect.o $(SRC_DIR)/metrics.o $(SRC_DIR)/log.o tools/demo_runner.c -o $@ $(LDFLAGS)

test: $(BIN_DIR)/test_banker $(BIN_DIR)/test_queue $(BIN_DIR)/test_http $(BIN_DIR)/test_rm_stress
	@echo "=== Running Unit & Stress Tests ==="
	./$(BIN_DIR)/test_banker
	./$(BIN_DIR)/test_queue
	./$(BIN_DIR)/test_http
	./$(BIN_DIR)/test_rm_stress
	@echo "=== All Tests Passed Successfully ==="

demo: $(BIN_DIR)/demo_runner
	./$(BIN_DIR)/demo_runner

tsan: CFLAGS += -fsanitize=thread
tsan: LDFLAGS += -fsanitize=thread
tsan: clean all test

asan: CFLAGS += -fsanitize=address
asan: LDFLAGS += -fsanitize=address
asan: clean all test

clean:
	rm -rf $(BIN_DIR) $(SRC_DIR)/*.o $(TEST_DIR)/*.o

.PHONY: all test clean tsan asan
