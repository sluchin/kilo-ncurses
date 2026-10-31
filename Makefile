CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -pedantic $(shell pkg-config --cflags-only-I ncursesw 2>/dev/null)
LDLIBS += $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)

kilo-ncurses: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: kilo-ncurses
	python3 tests/smoke.py

# make coverage: 計測用にビルドして, テストを実行し, 行と分岐のカバレッジを確かめる.
# 100% を下回ると失敗する. gcovr が必要. 結果は build/coverage/index.html.
# 保存の失敗やメモリ不足は, tests/fault.c (LD_PRELOAD) で起こす.
COVDIR = build/coverage
coverage:
	rm -rf $(COVDIR) && mkdir -p $(COVDIR)
	$(CC) -std=c99 -Wall -Wextra -pedantic -O0 --coverage $(shell pkg-config --cflags-only-I ncursesw 2>/dev/null) \
		-o $(COVDIR)/kilo-ncurses src/main.c $(LDLIBS)
	$(CC) -Wall -Wextra -shared -fPIC -o $(COVDIR)/fault.so tests/fault.c -ldl
	KILO_BIN=$(COVDIR)/kilo-ncurses KILO_FAULT_LIB=$(CURDIR)/$(COVDIR)/fault.so python3 tests/smoke.py
	gcovr -r . --object-directory $(COVDIR) --filter src/ --exclude-unreachable-branches \
		--html-details $(COVDIR)/index.html --print-summary --txt \
		--fail-under-line 100 --fail-under-branch 100

clean:
	rm -rf kilo-ncurses build

format:
	npx --yes markdownlint-cli2 --fix

format-check:
	npx --yes markdownlint-cli2

.PHONY: test coverage clean format format-check
