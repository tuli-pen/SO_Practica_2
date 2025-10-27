CC=gcc
CFLAGS=-Wall -Wextra -O2

all: p2-searchd p2-dataProgram

p2-searchd: src/p2-searchd.c src/common.h
	$(CC) $(CFLAGS) -o p2-searchd src/p2-searchd.c -pthread

p2-dataProgram: src/p2-dataProgram.c src/common.h
	$(CC) $(CFLAGS) -o p2-dataProgram src/p2-dataProgram.c -pthread

clean:
	rm -f p2-searchd p2-dataProgram
