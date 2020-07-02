.POSIX:
.SUFFIXES:
SHELL	= /bin/sh
CC	= gcc
CFLAGS	= -Wall -Wextra -std=c99 -pedantic -g
LDLIBS	= -lxcb

all: xcb_pong
xcb_pong: main.o
	$(CC) $(LDFLAGS) $(LDLIBS) -o xcb_pong main.o
main.o: main.c
	$(CC) -c $(CFLAGS) main.c
