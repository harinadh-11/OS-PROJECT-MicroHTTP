CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
TARGET = server
SOURCE = server.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

clean:
	rm -f $(TARGET)
