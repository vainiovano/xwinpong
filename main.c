#define _POSIX_C_SOURCE 200809L

#include <X11/keysym.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>

#define ARR_LEN(arr) (sizeof arr / sizeof arr[0])

static inline int16_t clamp(int16_t val, int16_t min, int16_t max) {
  if (val < min)
    return min;
  if (val > max)
    return max;
  return val;
}

static void collide(int16_t *speed, int16_t *pos, int16_t min_pos,
                    int16_t max_pos) {
  if (*pos > max_pos) {
    *pos = 2 * max_pos - *pos;
    *speed *= -1;
  } else if (*pos < min_pos) {
    *pos = 2 * min_pos - *pos;
    *speed *= -1;
  }
}

enum atom_type {
  PROTOCOL_ATOM,
  DELETE_WINDOW_ATOM,
  WINDOW_TYPE_ATOM,
  DIALOG_ATOM
};

static const char *const atom_names[] = {
    [PROTOCOL_ATOM] = "WM_PROTOCOLS",
    [DELETE_WINDOW_ATOM] = "WM_DELETE_WINDOW",
    [WINDOW_TYPE_ATOM] = "_NET_WM_WINDOW_TYPE",
    [DIALOG_ATOM] = "_NET_WM_WINDOW_TYPE_DIALOG"};

static xcb_window_t window_create(xcb_connection_t *connection,
                                  const xcb_screen_t *screen, uint32_t color,
                                  int16_t x, int16_t y, uint16_t width,
                                  uint16_t height) {
  const xcb_window_t window = xcb_generate_id(connection);
  const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  const uint32_t values[] = {color, XCB_EVENT_MASK_KEY_PRESS |
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

/* Sets some ICCC and EWMH atoms for window managers */
static void window_setup(xcb_connection_t *connection, xcb_window_t window,
                         xcb_intern_atom_reply_t *atom_replies[],
                         const char *window_name) {
  if (atom_replies[PROTOCOL_ATOM] != NULL &&
      atom_replies[DELETE_WINDOW_ATOM] != NULL) {
    xcb_change_property(connection, XCB_PROP_MODE_APPEND, window,
                        atom_replies[PROTOCOL_ATOM]->atom, XCB_ATOM_ATOM, 32, 1,
                        &atom_replies[DELETE_WINDOW_ATOM]->atom);
  }
  if (atom_replies[WINDOW_TYPE_ATOM] != NULL &&
      atom_replies[DIALOG_ATOM] != NULL) {
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                        atom_replies[WINDOW_TYPE_ATOM]->atom, XCB_ATOM_ATOM, 32,
                        1, &atom_replies[DIALOG_ATOM]->atom);
  }
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(window_name),
                      window_name);
}

struct moving_window {
  xcb_window_t window;
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  int16_t xspeed;
  int16_t yspeed;
};

static struct moving_window moving_window_create(xcb_connection_t *connection,
                                                 const xcb_screen_t *screen,
                                                 int16_t x, int16_t y) {
  const xcb_window_t window =
      window_create(connection, screen, screen->black_pixel, x, y, 150, 150);
  return (struct moving_window){window, x, y, 150, 150, 0, 0};
}

static void moving_window_move(struct moving_window *window,
                               xcb_connection_t *connection,
                               const xcb_screen_t *screen) {
  window->x += window->xspeed;
  window->y += window->yspeed;
  collide(&window->yspeed, &window->y, 0,
          screen->height_in_pixels - window->height);
  const uint32_t coords[] = {window->x, window->y};
  xcb_configure_window(connection, window->window,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
}

static int check_connection_error(xcb_connection_t *connection) {
  int error = xcb_connection_has_error(connection);
  if (error) {
    xcb_disconnect(connection);
    fputs("X11 connection has been invalidated: ", stderr);
    switch (error) {
    case XCB_CONN_ERROR:
      fputs("connection error", stderr);
      break;
    case XCB_CONN_CLOSED_EXT_NOTSUPPORTED:
      /* Shouldn't happen */
      fputs("extension not supported", stderr);
      break;
    case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
      fputs("insufficient memory", stderr);
      break;
    case XCB_CONN_CLOSED_REQ_LEN_EXCEED:
      fputs("too long request", stderr);
      break;
    case XCB_CONN_CLOSED_PARSE_ERR:
      fputs("bad display string", stderr);
      break;
    case XCB_CONN_CLOSED_INVALID_SCREEN:
      fputs("invalid screen", stderr);
      break;
    default:
      fputs("unknown error", stderr);
    }
    fputc('\n', stderr);
  }
  return error;
}

int main(void) {
  int screen_num;
  xcb_connection_t *const connection = xcb_connect(NULL, &screen_num);
  if (check_connection_error(connection)) {
    return EXIT_FAILURE;
  }

  xcb_screen_iterator_t iter =
      xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int i = 0; i < screen_num; ++i) {
    xcb_screen_next(&iter);
  }

  const xcb_screen_t *const screen = iter.data;
  /* TODO: can this happen? */
  if (screen == NULL) {
    fputs("Can't get the current screen\n", stderr);
    return EXIT_FAILURE;
  }

  xcb_key_symbols_t *const key_syms = xcb_key_symbols_alloc(connection);
  if (key_syms == NULL) {
    xcb_disconnect(connection);
    fputs("Can't allocate space for keyboard map information\n", stderr);
    return EXIT_FAILURE;
  }

  /*const xcb_gcontext_t foreground = xcb_generate_id(connection);
  uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
  uint32_t values[2] = {screen->black_pixel, 0};
  xcb_create_gc(connection, foreground, screen->root, mask, values);*/

  xcb_intern_atom_cookie_t atom_requests[ARR_LEN(atom_names)];
  for (size_t i = 0; i < ARR_LEN(atom_names); ++i) {
    atom_requests[i] = xcb_intern_atom_unchecked(
        connection, 1, strlen(atom_names[i]), atom_names[i]);
  }

  xcb_intern_atom_reply_t *atom_replies[ARR_LEN(atom_names)];
  for (size_t i = 0; i < ARR_LEN(atom_names); ++i) {
    atom_replies[i] = xcb_intern_atom_reply(connection, atom_requests[i], NULL);
    /* TODO: can xcb_intern_atom_reply return NULL? */
    if (atom_replies[i] != NULL && atom_replies[i]->atom == XCB_ATOM_NONE) {
      atom_replies[i] = NULL;
    }
  }

  /* Create the ball */
  int16_t ball_xspeed = 10;
  int16_t ball_yspeed = 10;
  int16_t ball_x = screen->width_in_pixels / 2 - 150 / 2;
  int16_t ball_y = screen->height_in_pixels / 2 - 150 / 2;
  xcb_window_t ball_window = window_create(
      connection, screen, screen->white_pixel, ball_x, ball_y, 150, 150);

  /* Create the paddles */
  struct moving_window left_paddle =
      moving_window_create(connection, screen, 0, 0);
  struct moving_window right_paddle = moving_window_create(
      connection, screen, screen->width_in_pixels - 150, 0);

  window_setup(connection, ball_window, atom_replies, "XCB pong");
  window_setup(connection, left_paddle.window, atom_replies, "Left paddle");
  window_setup(connection, right_paddle.window, atom_replies, "Right paddle");

  /* atom_replies[DELETE_WINDOW_ATOM] is used for checking the client messages
   */
  free(atom_replies[PROTOCOL_ATOM]);
  free(atom_replies[WINDOW_TYPE_ATOM]);
  free(atom_replies[DIALOG_ATOM]);

  xcb_map_window(connection, ball_window);
  xcb_map_window(connection, left_paddle.window);
  xcb_map_window(connection, right_paddle.window);
  xcb_flush(connection);

  int lost = 0;
  int paused = 0;
  int exit_code = EXIT_SUCCESS;

  for (;;) {
    xcb_generic_event_t *event;
    while ((event = paused ? xcb_wait_for_event(connection)
                           : xcb_poll_for_event(connection)) != NULL) {
      if (event->response_type == 0) {
        xcb_generic_error_t *const err = (xcb_generic_error_t *)event;
        fprintf(stderr,
                "Received X11 error %" PRIu8 " (%s); request major code %" PRIu8
                ", minor code %" PRIu16 "\n",
                err->error_code, xcb_event_get_error_label(err->error_code),
                err->major_code, err->minor_code);
        free(event);
        continue;
      }

      switch (XCB_EVENT_RESPONSE_TYPE(event)) {
      /*case XCB_EXPOSE:
        xcb_poly_rectangle(connection, ball_window, foreground, 1, &rectangle);
        xcb_flush(connection);*/
      case XCB_CLIENT_MESSAGE:
        if (((xcb_client_message_event_t *)event)->data.data32[0] ==
            atom_replies[DELETE_WINDOW_ATOM]->atom) {
          free(event);
          goto disconnect;
        }
        break;
      case XCB_DESTROY_NOTIFY:
        free(event);
        goto disconnect;
      case XCB_KEY_PRESS: {
        xcb_key_press_event_t *const kp = (xcb_key_press_event_t *)event;
        /* TODO: what does the last argument mean exactly? */
        const xcb_keysym_t keysym =
            xcb_key_press_lookup_keysym(key_syms, kp, 0);

        if (paused) {
          switch (keysym) {
          case XK_p:
          case XK_P:
            paused = 0;
            break;
          }
        } else {
          switch (keysym) {
          case XK_w:
          case XK_W:
            left_paddle.yspeed -= 5;
            break;
          case XK_s:
          case XK_S:
            left_paddle.yspeed += 5;
            break;
          case XK_Up:
            right_paddle.yspeed -= 5;
            break;
          case XK_Down:
            right_paddle.yspeed += 5;
            break;
          case XK_p:
          case XK_P:
            paused = 1;
            break;
          }
        }
      } break;
      case XCB_MAPPING_NOTIFY: {
        xcb_mapping_notify_event_t *mn = (xcb_mapping_notify_event_t *)event;
        if (mn->request == XCB_MAPPING_KEYBOARD) {
          xcb_refresh_keyboard_mapping(key_syms, mn);
        }
      } break;
      case XCB_CONFIGURE_NOTIFY: {
        /* Each window move generates this event. This game only cares about
         * external resize events, but receiving only them is impossible. */
        xcb_configure_notify_event_t *cn =
            (xcb_configure_notify_event_t *)event;
        struct moving_window *resized_window = NULL;
        if (cn->window == left_paddle.window) {
          resized_window = &left_paddle;
        } else if (cn->window == right_paddle.window) {
          resized_window = &right_paddle;
          /* TODO: use something better for resizing the right paddle */
          resized_window->x = screen->width_in_pixels - cn->width;
        }
        if (resized_window != NULL) {
          resized_window->width = cn->width;
          resized_window->height = cn->height;
        }
      } break;
      default:
        break;
      }
      free(event);
    }

    if (check_connection_error(connection)) {
      exit_code = EXIT_FAILURE;
      goto free_and_exit;
    }

    moving_window_move(&left_paddle, connection, screen);
    moving_window_move(&right_paddle, connection, screen);

    ball_x += ball_xspeed;
    ball_y += ball_yspeed;

    if (ball_x < left_paddle.x + left_paddle.width) {
      if (!lost && ball_y + 150 > left_paddle.y &&
          ball_y < left_paddle.y + left_paddle.height) {
        collide(&ball_xspeed, &ball_x, left_paddle.x + left_paddle.width,
                INT16_MAX);
        /* Make the game advance faster */
        ball_xspeed += 1;

        ball_yspeed +=
            ((ball_y + 75) - (left_paddle.y + left_paddle.height / 2)) / 5;
        ball_yspeed = clamp(ball_yspeed, -20, 20);

        lost = 0;
      } else {
        lost = 1;
      }
    } else if (ball_x + 150 > right_paddle.x) {
      if (!lost && ball_y + 150 > right_paddle.y &&
          ball_y < right_paddle.y + right_paddle.height) {
        collide(&ball_xspeed, &ball_x, INT16_MIN, right_paddle.x - 150);
        ball_xspeed -= 1;

        ball_yspeed +=
            ((ball_y + 75) - (right_paddle.y + right_paddle.height / 2)) / 5;
        ball_yspeed = clamp(ball_yspeed, -20, 20);

        lost = 0;
      } else {
        lost = 1;
      }
    }
    collide(&ball_yspeed, &ball_y, 0, screen->height_in_pixels - 150);

    if (ball_x < 0) {
      puts("Right wins!");
      goto disconnect;
    } else if (ball_x > screen->width_in_pixels - 150) {
      puts("Left wins!");
      goto disconnect;
    }

    const uint32_t coords[] = {ball_x, ball_y};
    xcb_configure_window(connection, ball_window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
    xcb_flush(connection);

    const struct timespec wait_time = {
        .tv_sec = 0,
        .tv_nsec = 50000000,
    };
    nanosleep(&wait_time, NULL);
  }

disconnect:
  xcb_destroy_window(connection, ball_window);
  xcb_destroy_window(connection, left_paddle.window);
  xcb_destroy_window(connection, right_paddle.window);
  xcb_disconnect(connection);

free_and_exit:
  free(atom_replies[DELETE_WINDOW_ATOM]);
  xcb_key_symbols_free(key_syms);

  return exit_code;
}
