CC := $(shell command -v musl-gcc 2>/dev/null || echo gcc)
CFLAGS := -std=c99 -pedantic -pedantic-errors -O3 -Wall -Wextra -Werror -flto -MMD -MP -Isrc -pthread
LDFLAGS := -static -flto -pthread
COV_CFLAGS := -std=c99 -pedantic -pedantic-errors -O0 -g -coverage -Wall -Wextra -Werror -MMD -MP -Isrc -pthread
COV_LDFLAGS := -static -coverage -pthread

SRC := $(shell find src -name '*.c' 2>/dev/null)
OBJ := $(patsubst src/%,obj/%,$(SRC:.c=.o))
DEP := $(OBJ:.o=.d)
BIN := bin/clock
DIRS := $(sort $(dir $(OBJ)))

COV_DIR := obj/cov
COV_OBJ := $(patsubst src/%,$(COV_DIR)/%,$(filter-out src/main.o,$(SRC:.c=.o)))
COV_DEP := $(COV_OBJ:.o=.d)
COV_DIRS := $(sort $(dir $(COV_OBJ)))

TEST_SRC := $(wildcard tests/*_test.c)
TEST_BIN := $(patsubst tests/%_test.c,obj/tests/%_test,$(TEST_SRC))

LIZARD_FLAGS ?= --warnings_only
MAX_LOC_PER_FILE ?= 300

.PHONY: all install format lint cppcheck lizard loc-check test coverage cov-list report integration clean FORCE

all: $(BIN)

$(BIN): $(OBJ)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(DIRS): ; -mkdir -p $@

obj/%.o: src/%.c | $(DIRS)
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEP)

install: $(BIN)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(BIN) $(DESTDIR)/usr/bin/clock
	strip $(DESTDIR)/usr/bin/clock
	install -d $(DESTDIR)/usr/share/man/man1
	install -m 644 man/clock.1 $(DESTDIR)/usr/share/man/man1/

format:
	@for f in $$(find src tests -name '*.c' -o -name '*.h' 2>/dev/null); do \
	  indent -linux -l120 -i2 -nut "$$f" -o "$${f}.new" 2>/dev/null; \
	  if [ "$$(md5sum < "$$f" | head -c32)" != "$$(md5sum < "$${f}.new" | head -c32)" ]; then \
	    cp "$${f}.new" "$$f"; \
	  fi; \
	  rm -f "$${f}.new" "$$f"~; \
	done; \
	true

lint: format cppcheck lizard loc-check

cppcheck:
	cppcheck --enable=all --check-level=exhaustive --inline-suppr --error-exitcode=1 --quiet -I src/ src/

lizard:
	lizard --languages=c --CCN 6 --length 40 --arguments 5 -Ttoken_count=150 -ENS $(LIZARD_FLAGS) src/

loc-check:
	@lizard --languages=c --length 40 -Ttoken_count=150 -ENS src/ 2>/dev/null | \
	awk -v max=$(MAX_LOC_PER_FILE) \
	  'BEGIN{h=0} /^=+/{h++;next} h==2 && $$1+0==$$1{for(i=2;i<=NF;i++){if($$i ~ /\.c$$/){f=$$i; break}}; if(f && $$1>max){printf "FAIL: %s has %d LOC (max %d)\n",f,$$1,max; ec=1}} \
	   END{exit ec}'

$(COV_DIRS): ; @mkdir -p $@

$(COV_DIR)/%.o: src/%.c | $(COV_DIRS)
	@$(CC) $(COV_CFLAGS) -c -o $@ $<

MOCK := tests/mock_syscall.c

FORCE:

obj/tests/: ; @mkdir -p $@

$(TEST_BIN): obj/tests/%_test: tests/%_test.c $(MOCK) $(COV_OBJ) FORCE | obj/tests/
	@$(CC) $(COV_CFLAGS) -o $@ $< $(MOCK) $(COV_OBJ) $(COV_LDFLAGS)

test: $(TEST_BIN)
	@find $(CURDIR)/$(COV_DIR) -name '*.gcda' -delete 2>/dev/null || true
	@EC=0; PASS=0; FAIL=0; TOTAL=0; \
	for t in $(TEST_BIN); do \
		TOTAL=$$((TOTAL + 1)); \
		name=$$(basename $$t); \
		if $$t; then \
			PASS=$$((PASS + 1)); \
			echo "[PASS] $$name"; \
		else \
			FAIL=$$((FAIL + 1)); \
			echo "[FAIL] $$name"; \
			EC=1; \
		fi; \
	done; \
	echo "$$PASS/$$TOTAL tests passed"; \
	if stty -a 2>/dev/null | grep -E -- '(^| )-echo($| )' >/dev/null 2>&1; then \
		echo "[FAIL] terminal echo not restored; running stty sane"; \
		stty sane 2>/dev/null || true; \
		EC=1; \
	fi; \
	exit $$EC

integration: $(BIN)
	$(MAKE) -C integration run CLOCK_BIN="$(CURDIR)/$(BIN)"

GCOV_DIR := obj/gcov

coverage:
ifneq ($(COV_SKIP_TEST),1)
coverage: test
endif
	@rm -rf $(GCOV_DIR); \
	mkdir -p $(GCOV_DIR); \
	cd $(GCOV_DIR) && \
	for f in $$(find $(CURDIR)/$(COV_DIR) -name '*.gcda' ! -name '*syscall.gcda' 2>/dev/null); do \
		base=$${f%.gcda}; \
		src=$$(echo $$base | sed 's|$(CURDIR)/$(COV_DIR)/|$(CURDIR)/src/|').c; \
		gcov -o "$$base" -s "$(CURDIR)" "$$src" 2>/dev/null | grep 'Lines executed' | tail -1; \
	done > gcov.txt; \
	data=$$(cat gcov.txt); \
	echo "$$data" | sed 's/Lines executed://; s/% of / /' | \
	awk '{t+=$$2; c+=$$2*$$1/100} END {p=c/t*100; printf "Coverage: %.2f%% (%d/%d lines)\n", p, c, t; if (p < 80.0) {printf "[FAIL] coverage below 80%%\n"; exit 1}}'

cov-list:
	@for f in $$(find $(CURDIR)/$(COV_DIR) -name '*.gcda' ! -name '*syscall.gcda' 2>/dev/null); do \
	  base=$${f%.gcda}; \
	  src=$$(echo $$base | sed 's|$(CURDIR)/$(COV_DIR)/|src/|').c; \
	  gcov -o "$$base" -s "$(CURDIR)" "$(CURDIR)/$$src" 2>/dev/null; \
	done | awk '/^File /{f=substr($$2,2,length($$2)-2)} /Lines executed:/ && f ~ /\.c$$/{split($$2,a,":"); pct=a[2]+0; t=$$4+0; e=int(pct*t/100+0.5); if(pct<80 && t>0) printf "%5.1f%% (%d/%d)  %s\n",pct,e,t,f}' | sort -t/ -k1 -n | uniq

report:
	@RC=0; \
	echo "=== cppcheck ==="; \
	out=$$(make --no-print-directory cppcheck 2>&1) || { \
		echo "$$out" | grep '^src/' | sed 's/:.*//' | sort -u; \
		echo "FAIL $$(echo "$$out" | grep -c '^src/')"; RC=1; \
	}; \
	[ -z "$$(echo "$$out" | grep '^src/')" ] && echo "PASS 0"; \
	echo "=== lizard ==="; \
	out=$$(make --no-print-directory lizard 2>&1) || { \
		echo "$$out" | awk '/warning:/{print $$2}' | sort -u; \
		echo "FAIL"; RC=1; \
	}; \
	[ -z "$$(echo "$$out" | grep 'warning:')" ] && echo "PASS"; \
	echo "=== loc-check ==="; \
	out=$$(make --no-print-directory loc-check 2>&1) || { echo "$$out"; RC=1; }; \
	[ -z "$$out" ] && echo "PASS"; \
	echo "=== test ==="; \
	out=$$(make --no-print-directory test 2>&1); rc=$$?; \
	summary=$$(echo "$$out" | grep -oE '[0-9]+/[0-9]+ tests passed'); \
	[ $$rc -eq 0 ] && echo "PASS $$summary" || { echo "FAIL $$summary"; RC=1; }; \
	echo "=== coverage ==="; \
	out=$$(make --no-print-directory coverage COV_SKIP_TEST=1 2>&1); rc=$$?; \
	echo "$$out" | grep 'Lines executed' | \
	    awk '{p=$$3; sub(/%/, "", p); if (p+0 < 80) print $$0}'; \
	last=$$(echo "$$out" | grep 'Coverage:'); \
	[ $$rc -eq 0 ] && echo "PASS $$last" || { echo "FAIL $$last"; RC=1; }; \
	exit $$RC

clean:
	rm -rf bin obj

-include $(COV_DEP)
