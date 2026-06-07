CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=
LDLIBS ?= -lncurses

TARGET := testforge
SRC := testforge.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
