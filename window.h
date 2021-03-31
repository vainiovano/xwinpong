#ifndef XCB_PONG_WINDOW_H_
#define XCB_PONG_WINDOW_H_

#include <stdbool.h>
#include <stdint.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

/* One of the windows has override-redirect set and the other doesn't. window is
 * the mapped window and other_window is the unmapped one. */
struct moving_window {
  xcb_window_t window;
  xcb_window_t other_window;
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  int16_t xspeed;
  int16_t yspeed;
};

enum atom_type {
  PROTOCOL_ATOM,
  DELETE_WINDOW_ATOM,
  WINDOW_TYPE_ATOM,
  DIALOG_ATOM
};

void collide(int16_t *speed, int16_t *pos, int16_t min_pos, int16_t max_pos);

struct moving_window moving_window_create(xcb_connection_t *connection,
                                          const xcb_screen_t *screen,
                                          uint32_t color, bool borders,
                                          int16_t x, int16_t y);

/* Sets some ICCCM and EWMH atoms for window managers */
void moving_window_setup(const struct moving_window *window,
                         xcb_connection_t *connection, xcb_atom_t atoms[],
                         const char *window_name);

/* Moves the window and calculates collisions with the top and bottom edges of
 * the screen. Doesn't send X11 requests. */
void moving_window_move(struct moving_window *window,
                        const xcb_screen_t *screen, double delta);

void moving_window_send_position(const struct moving_window *window,
                                 xcb_connection_t *connection);

/* Toggles the window's decorations by unmapping the current window and mapping
 * the other window. The new mapped window is moved and resized to the correct
 * position and dimensions before mapping. */
void moving_window_swap(struct moving_window *window,
                        xcb_connection_t *connection);
#endif
