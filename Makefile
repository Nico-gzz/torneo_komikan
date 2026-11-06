CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -pthread
TARGET = torneo

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
