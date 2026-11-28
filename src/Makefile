CC := gcc
CFLAGS := -Wall -Wextra -O2
BIN := bin
COMMON := iolib.c banner.c
HDRS := iolib.h banner.h

all: $(BIN)/workload $(BIN)/uring_cp

$(BIN):
	mkdir -p $(BIN)

$(BIN)/workload: workload.c $(COMMON) $(HDRS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ workload.c $(COMMON)

$(BIN)/uring_cp: uring_cp.c $(COMMON) $(HDRS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ uring_cp.c $(COMMON) -luring

run: $(BIN)/workload
	./$(BIN)/workload $(ARGS)

clean:
	rm -rf $(BIN)

.PHONY: all run clean