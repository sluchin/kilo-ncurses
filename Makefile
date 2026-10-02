CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -pedantic $(shell pkg-config --cflags-only-I ncursesw 2>/dev/null)
LDLIBS += $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)

kilo-ncurses: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: kilo-ncurses
	python3 tests/smoke.py

clean:
	rm -f kilo-ncurses

.PHONY: test clean
