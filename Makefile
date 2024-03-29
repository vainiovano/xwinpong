.POSIX:
.SUFFIXES:
SHELL	= /bin/sh
LDLIBS	= -lxcb -lxcb-keysyms -lxcb-util

all: xwinpong
xwinpong: main.o window.o
	$(CC) $(LDFLAGS) -o xwinpong main.o window.o $(LDLIBS)
main.o: main.c window.h
	$(CC) -c $(CFLAGS) main.c
window.o: window.c window.h
	$(CC) -c $(CFLAGS) window.c

clean:
	rm -f -- xwinpong *.o
