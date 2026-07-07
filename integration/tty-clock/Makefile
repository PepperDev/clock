.PHONY: run clean

BIN := ../../bin/clock

run:
	CLOCK_BIN="$(or $(CLOCK_BIN),../../bin/clock)" python3 run.py

clean:
	rm -rf ../../obj/integration/tty-clock
