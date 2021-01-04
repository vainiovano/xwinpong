.POSIX:
.SUFFIXES:
SHELL	= /bin/sh
LDLIBS	= -lxcb -lxcb-keysyms -lxcb-util

all: xcb_pong
xcb_pong: main.o window.o
	$(CC) $(LDFLAGS) $(LDLIBS) -o xcb_pong main.o window.o
main.o: main.c
	$(CC) -c $(CFLAGS) main.c
window.o: window.c
	$(CC) -c $(CFLAGS) window.c

clean:
	rm -f -- xcb_pong *.o
