CC = gcc
CFLAGS = -Wall -Wextra -g -O2
SRC = myshell.c
TARGET = myshell

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean

