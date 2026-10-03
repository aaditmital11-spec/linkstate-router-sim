# Makefile for the link-state routing simulator.
#
# -std=c11                     ISO C11, no GNU extensions
# -Wall -Wextra                every warning we can reasonably get; the build
#                              is expected to be completely warning free
# -g -O0                       full debug info, no optimisation, so that gdb
#                              shows real variables and real line numbers
# -D_POSIX_C_SOURCE=200809L    strict C11 hides the POSIX interfaces we need
#                              (clock_gettime, poll, sigaction), so we ask for
#                              POSIX.1-2008 explicitly in one central place

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -g -O0 -D_POSIX_C_SOURCE=200809L

BIN     = router
SRCS    = src/router.c src/topology.c src/lsdb.c src/spf.c
OBJS    = $(SRCS:.c=.o)

# "all" is first, so a bare "make" builds the router binary.
.PHONY: all clean test
all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# Pattern rule: how to turn any src/foo.c into src/foo.o. The header list is a
# prerequisite so that editing a header forces the dependent objects to rebuild.
src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJS)
	rm -rf logs __pycache__ tests/__pycache__ .pytest_cache

test: all
	python3 -m pytest -v tests/
