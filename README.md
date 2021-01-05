# Xwinpong
Xwinpong is a Pong-style game for X11, where the ball and paddles are windows
that bounce around your screen.

## Features
- window resizing (pausing makes it easier)
- toggle window borders while running (disabling window borders can reduce CPU
  usage, but grabs the keyboard and prevents resizing windows)
- window colors
- sliding paddles
- play against yourself, or even someone else!

## Building
### Dependencies
- libxcb
- libxcb-keysyms
- libxcb-util

#### Debian
Assuming that you already have `make` and a C compiler installed
```
$ sudo apt install libxcb1-dev libxcb-keysyms1-dev libxcb-util0-dev
```

### Compiling
```
$ make
```

See [Makefile](./Makefile) for build configuration variables

### Running
```
$ ./xwinpong
```

Set the `DISPLAY` environment variable if you want to connect to another X11
server (see `man 7 X`)

## Controls
key | meaning
--- | --------
w | left paddle up
s | left paddle down
up arrow | right paddle up
down arrow | right paddle down
p | toggle pause
b | toggle window borders

## Options
option | meaning | default
------ | ------- | -------
**-lc** *color* | left paddle color | black
**-bc** *color* | ball color | white
**-rc** *color* | right paddle color | black
**-borders** | start with window borders enabled | borders enabled
**+borders** | start with window borders disabled | borders enabled

### Colors
Window colors can be X11 color names or hexadecimal RGB codes.

Example:
```
$ ./xwinpong -lc "ghost white" -bc "#123456" -rc "chartreuse"
```
