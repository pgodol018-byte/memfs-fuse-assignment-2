
CC=gcc
CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64 -fsanitize=thread
LDFLAGS=-lfuse -fsanitize=thread

OBJ=memfs.o node.o dir.o statistics.o locks.o

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

memfs: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o memfs

.PHONY: clean
clean:
	rm -f *.o memfs

