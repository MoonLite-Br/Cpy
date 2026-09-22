CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -std=gnu11
LDLIBS  += -lm -pthread
LIBS    := $(wildcard lib/*.cpy)
SRC     := $(sort $(wildcard src/*.c) src/stdlib_data.c)
BIN     := bin/cpy

all: $(BIN)

src/stdlib_data.c: $(LIBS) tools/gen_stdlib.sh
	sh tools/gen_stdlib.sh $(LIBS) > $@

$(BIN): $(SRC) src/cpy.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

debug:
	@mkdir -p bin
	$(CC) -g -O0 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -std=gnu11 -o bin/cpy-asan $(SRC) $(LDLIBS)

test: $(BIN)
	./run_tests.sh

clean:
	rm -rf bin

.PHONY: all debug test clean
