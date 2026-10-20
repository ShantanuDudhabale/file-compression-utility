
CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
THREAD_FLAGS = -pthread
LDLIBS = -lz

SRC = src/main.c src/compressor.c src/decompressor.c \
      src/file_utils.c src/benchmark.c

TARGET = filecompress

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -Isrc $(SRC) -o $(TARGET) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean

