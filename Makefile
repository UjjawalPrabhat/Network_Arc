CC      ?= cc
CFLAGS  ?= -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -O2

all: bserve bcurl

bserve: src/bserve.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/proto.c

bcurl: src/bcurl.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/proto.c

test: all
	./tests/run.sh

clean:
	rm -f bserve bcurl

.PHONY: all test clean
