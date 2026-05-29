CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11
LDFLAGS = -lncurses
TARGET = megamenu
SRC = main.c config.c ui.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) megamenu.config

clean:
	rm -f $(TARGET)
