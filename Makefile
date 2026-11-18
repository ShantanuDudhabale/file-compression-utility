CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
LIBS = -lz -pthread

TARGET = pcompress

all: $(TARGET)

$(TARGET): parallel_compress.c
	$(CC) $(CFLAGS) parallel_compress.c -o $(TARGET) $(LIBS)

static: parallel_compress.c
	$(CC) $(CFLAGS) -static parallel_compress.c -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET) $(TARGET).exe *.o *.pcmp

.PHONY: all static clean
