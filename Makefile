TARGET  ?= bin/clock
DESTDIR ?= /usr/local

SOURCES = $(patsubst %,src/%.c, \
		main \
	)

OBJS    = $(SOURCES:src/%.c=.objs/%.o)
DIRS    = $(patsubst %/,%,$(sort $(dir $(TARGET) $(OBJS))))

CFLAGS  = -std=c99 -pedantic -O3 -Wall -Wextra -Wpedantic -Wformat=2 \
			-Wno-unused-parameter -Wshadow -Wwrite-strings -Wstrict-prototypes \
			-Wold-style-definition -Wredundant-decls -Wnested-externs \
			-Wmissing-include-dirs
LDFLAGS = -static
LIBS    =

all: $(TARGET)
.PHONY: all clear install

$(TARGET): $(OBJS) | bin
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	strip $@

.objs/%.o: src/%.c | .objs
	$(CC) $(CFLAGS) -c $< -o $@

# (CC) -M -MP -MT '$(<:src/%.c=.objs/%.o) $@' $(CFLAGS) $< > $@
# .objs/main.o: src/main.h

$(DIRS):
	-mkdir -p $@

clean:
	-rm -f $(TARGET) $(OBJS)
	-rmdir -p $(DIRS)

install:
	install -o 0 -g 0 -m 0755 $(TARGET) $(DESTDIR)/bin/
