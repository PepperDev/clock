.PHONY: run clean

TEST_DIRS := $(sort $(dir $(wildcard */Makefile)))

run:
	@failed=0; \
	for dir in $(TEST_DIRS); do \
		name=$$(basename $$dir); \
		echo "[INTEGRATION] $$name"; \
		$(MAKE) -C $$dir run CLOCK_BIN="$(CLOCK_BIN)" || { failed=1; echo "[FAIL] $$name"; }; \
	done; \
	if [ $$failed -ne 0 ]; then exit 1; fi
