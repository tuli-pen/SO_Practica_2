# Makefile simple para compilar los dos programas

all: p2-search p2-dataProgram

p2-search: hash.c index2.c p2-search.c
	gcc hash.c index2.c p2-search.c -o p2-search

p2-dataProgram: p2-dataProgram.c
	gcc p2-dataProgram.c -o p2-dataProgram

clean:
	rm -f p2-search p2-dataProgram
