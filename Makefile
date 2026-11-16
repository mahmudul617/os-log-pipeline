CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

BIN     = bin
SRC     = src

all: $(BIN)/log_pipeline $(BIN)/log_single

$(BIN):
	mkdir -p $(BIN)

$(BIN)/log_pipeline: $(SRC)/pipeline.c $(SRC)/common.c $(SRC)/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/pipeline.c $(SRC)/common.c $(LDFLAGS)

$(BIN)/log_single: $(SRC)/single.c $(SRC)/common.c $(SRC)/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/single.c $(SRC)/common.c $(LDFLAGS)

# Build with AddressSanitizer + UndefinedBehaviorSanitizer for debugging
debug: CFLAGS = -g -O0 -Wall -Wextra -std=c11 -fsanitize=address,undefined
debug: clean all

data:
	python3 scripts/gen_logs.py 1000 data/small.log --seed 1
	python3 scripts/gen_logs.py 10000 data/medium.log --seed 2

test: all
	bash tests/run_tests.sh

bench: all
	python3 scripts/bench.py

clean:
	rm -rf $(BIN)

.PHONY: all debug data test bench clean
