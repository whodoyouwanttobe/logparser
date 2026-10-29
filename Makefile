# logparser — сборка
CC      ?= gcc
CLANG_FORMAT ?= clang-format-18
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
PG_INC  := $(shell pg_config --includedir 2>/dev/null)
CPPFLAGS += -I$(PG_INC) -Isrc
PG_LIB  := $(shell pg_config --libdir 2>/dev/null)
LDFLAGS += -L$(PG_LIB)
LDLIBS  += -lpq

SRC := src/parser.c src/db.c src/importer.c src/ui.c src/main.c
OBJ := $(SRC:.c=.o)

.PHONY: all test clean format lint

all: logparser

logparser: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# unit-тесты парсера: БД не нужна
tests/test_parser: tests/test_parser.c src/parser.c src/parser.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_parser.c src/parser.c

test: tests/test_parser
	./tests/test_parser

format:
	$(CLANG_FORMAT) -i src/*.c src/*.h tests/*.c

lint:
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/*.h tests/*.c

clean:
	rm -f logparser src/*.o tests/test_parser
