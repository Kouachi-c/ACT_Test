CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -g
TARGET  = uart_demo
SRC     = uart_demo.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
