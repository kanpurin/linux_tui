CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=
LDLIBS ?= -lncursesw

TARGET := autotest-builder
SRC := autotest_builder.c

.PHONY: all clean run regen-scripts

all: $(TARGET) regen-scripts

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

regen-scripts: $(TARGET)
	./$(TARGET) --regen-scripts

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
