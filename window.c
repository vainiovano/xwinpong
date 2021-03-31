#include "window.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

void collide(int16_t *speed, int16_t *pos, int16_t min_pos, int16_t max_pos) {
  if (*pos > max_pos) {
    *pos = 2 * max_pos - *pos;
    *speed *= -1;
  } else if (*pos < min_pos) {
    *pos = 2 * min_pos - *pos;
    *speed *= -1;
  }
}

static xcb_window_t window_create(xcb_connection_t *connection,
                                  const xcb_screen_t *screen, uint32_t color,
                                  bool override_redirect, int16_t x, int16_t y,
                                  uint16_t width, uint16_t height) {
  const xcb_window_t window = xcb_generate_id(connection);
  const uint32_t mask =
      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  const uint32_t values[] = {color, override_redirect,
                             XCB_EVENT_MASK_KEY_PRESS |
                                 XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_create_window(connection,                    /* connection */
                    XCB_COPY_FROM_PARENT,          /* depth */
                    window,                        /* window id */
                    screen->root,                  /* parent window */
                    x, y,                          /* x, y */
                    width, height,                 /* width, height */
                    0,                             /* border width */
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, /* class */
                    screen->root_visual,           /* visual */
                    mask, values                   /* masks */
  );
  return window;
}

static void window_setup(xcb_connection_t *connection, xcb_window_t window,
                         xcb_atom_t atoms[], const char *window_name) {
  if (atoms[PROTOCOL_ATOM] != XCB_ATOM_NONE &&
      atoms[DELETE_WINDOW_ATOM] != XCB_ATOM_NONE) {
    xcb_change_property(connection, XCB_PROP_MODE_APPEND, window,
                        atoms[PROTOCOL_ATOM], XCB_ATOM_ATOM, 32, 1,
                        &atoms[DELETE_WINDOW_ATOM]);
  }
  if (atoms[WINDOW_TYPE_ATOM] != XCB_ATOM_NONE &&
      atoms[DIALOG_ATOM] != XCB_ATOM_NONE) {
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                        atoms[WINDOW_TYPE_ATOM], XCB_ATOM_ATOM, 32, 1,
                        &atoms[DIALOG_ATOM]);
  }
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(window_name),
                      window_name);
  /* TODO: set the instance name in a better way */
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 18,
                      "xwinpong\0Xwinpong");
}

struct moving_window moving_window_create(xcb_connection_t *connection,
                                          const xcb_screen_t *screen,
                                          uint32_t color, bool borders,
                                          int16_t x, int16_t y) {
  const xcb_window_t window =
      window_create(connection, screen, color, false, x, y, 150, 150);
  const xcb_window_t other_window =
      window_create(connection, screen, color, true, x, y, 150, 150);
  return borders ? (struct moving_window){window, other_window, x, y,
                                          150,    150,          0, 0}
                 : (struct moving_window){other_window, window, x, y,
                                          150,          150,    0, 0};
}

/* Both windows get the atoms set */
void moving_window_setup(const struct moving_window *window,
                         xcb_connection_t *connection, xcb_atom_t atoms[],
                         const char *window_name) {
  window_setup(connection, window->window, atoms, window_name);
  window_setup(connection, window->other_window, atoms, window_name);
}

void moving_window_move(struct moving_window *window,
                        const xcb_screen_t *screen, double delta) {
  const double screen_resolution_multiplier =
      (double)screen->width_in_pixels / 1000.;
  window->x += window->xspeed * screen_resolution_multiplier * delta;
  window->y += window->yspeed * screen_resolution_multiplier * delta;
  collide(&window->yspeed, &window->y, 0,
          screen->height_in_pixels - window->height);
}

void moving_window_send_position(const struct moving_window *window,
                                 xcb_connection_t *connection) {
  const uint32_t coords[] = {window->x, window->y};
  xcb_configure_window(connection, window->window,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
}

static void moving_window_send_size(const struct moving_window *window,
                                    xcb_connection_t *connection) {
  const uint32_t size[] = {window->width, window->height};
  xcb_configure_window(connection, window->window,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                       size);
}

void moving_window_swap(struct moving_window *window,
                        xcb_connection_t *connection) {
  xcb_unmap_window(connection, window->window);

  xcb_window_t temp = window->window;
  window->window = window->other_window;
  window->other_window = temp;

  moving_window_send_position(window, connection);
  moving_window_send_size(window, connection);
  xcb_map_window(connection, window->window);
}
