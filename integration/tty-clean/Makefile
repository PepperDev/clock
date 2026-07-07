BIN := ../../obj/integration/tty/test
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra

.PHONY: run clean

$(BIN): test.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<

run: $(BIN)
	CLOCK_BIN="$(CLOCK_BIN)" ./$(BIN)

clean:
	rm -rf ../../obj/integration/tty
