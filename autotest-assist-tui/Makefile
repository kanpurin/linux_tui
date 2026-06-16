CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=
LDLIBS ?= -lncursesw

TARGET := autotest-builder
SRC := autotest_builder.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
