.POSIX:
.SUFFIXES:
SHELL	= /bin/sh
LDLIBS	= -lxcb -lxcb-keysyms -lxcb-util

all: xcb_pong
xcb_pong: main.o
	$(CC) $(LDFLAGS) $(LDLIBS) -o xcb_pong main.o
main.o: main.c
	$(CC) -c $(CFLAGS) main.c

clean:
	rm -f xcb_pong main.o
