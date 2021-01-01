#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
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

/* Sets some ICCCM and EWMH atoms for window managers */
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
                                                 uint32_t color, int16_t x,
                                                 int16_t y) {
  const xcb_window_t window =
      window_create(connection, screen, color, x, y, 150, 150);
  return (struct moving_window){window, x, y, 150, 150, 0, 0};
}

/* Calculates collisions with the top and bottom edges of the screen. Doesn't
 * send X11 events. */
static void moving_window_move(struct moving_window *window,
                               const xcb_screen_t *screen) {
  window->x += window->xspeed;
  window->y += window->yspeed;
  collide(&window->yspeed, &window->y, 0,
          screen->height_in_pixels - window->height);
}

static void moving_window_send_position(const struct moving_window *window,
                                        xcb_connection_t *connection) {
  const uint32_t coords[] = {window->x, window->y};
  xcb_configure_window(connection, window->window,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
}

enum game_window { LEFT_PADDLE, BALL, RIGHT_PADDLE };

static const char *const window_color_options[] = {
    [LEFT_PADDLE] = "-lc", [BALL] = "-bc", [RIGHT_PADDLE] = "-rc"};

static char *requested_window_colors[ARR_LEN(window_color_options)];
static uint32_t window_colors[ARR_LEN(window_color_options)];

static void default_window_colors(const xcb_screen_t *screen) {
  window_colors[LEFT_PADDLE] = window_colors[RIGHT_PADDLE] =
      screen->black_pixel;
  window_colors[BALL] = screen->white_pixel;
}

enum color_type { COLOR, NAMED_COLOR };

/* xcb_alloc_color_cookie_t and xcb_alloc_named_color_cookie_t are basically the
 * same type. They are structs that only contain the sequence number of the
 * request. */
union color_request_union {
  xcb_alloc_color_cookie_t color_cookie;
  xcb_alloc_named_color_cookie_t named_color_cookie;
};

struct color_request {
  enum color_type type;
  union color_request_union cookie;
};

union color_reply_union {
  xcb_alloc_color_reply_t color_reply;
  xcb_alloc_named_color_reply_t named_color_reply;
};

struct color_reply {
  enum color_type type;
  union color_reply_union reply;
};

/* color_name isn't const because older versions of xcb-util are missing const
 * in xcb_aux_parse_color's declaration */
static struct color_request request_color(xcb_connection_t *connection,
                                          xcb_colormap_t colormap,
                                          size_t color_name_len,
                                          char *color_name) {
  uint16_t r, g, b;
  struct color_request request;
  if (xcb_aux_parse_color(color_name, &r, &g, &b)) {
    request.type = COLOR;
    request.cookie.color_cookie =
        xcb_alloc_color(connection, colormap, r, g, b);
  } else {
    request.type = NAMED_COLOR;
    request.cookie.named_color_cookie =
        xcb_alloc_named_color(connection, colormap, color_name_len, color_name);
  }
  return request;
}

static uint32_t read_color_reply(xcb_connection_t *connection,
                                 struct color_request request,
                                 xcb_generic_error_t **error) {
  uint32_t pixel;
  switch (request.type) {
  case COLOR: {
    xcb_alloc_color_reply_t *color_reply =
        xcb_alloc_color_reply(connection, request.cookie.color_cookie, error);
    if (color_reply == NULL) {
      /* The return value shouldn't be used if the error is set */
      return 0;
    }
    pixel = color_reply->pixel;
    free(color_reply);
    break;
  }
  case NAMED_COLOR: {
    xcb_alloc_named_color_reply_t *color_reply = xcb_alloc_named_color_reply(
        connection, request.cookie.named_color_cookie, error);
    if (color_reply == NULL) {
      return 0;
    }
    pixel = color_reply->pixel;
    free(color_reply);
    break;
  }
  }
  return pixel;
}

static void usage(const char *command_name) {
  fprintf(stderr,
          "usage: %s\n"
          "\t[-lc {color}]\n"
          "\t[-bc {color}]\n"
          "\t[-rc {color}]\n",
          command_name);
}

static int parse_options(int argc, char *argv[]) {
  int return_code = 0;
  for (int i = 1; i < argc; ++i) {
    for (size_t j = 0; j < ARR_LEN(window_color_options); ++j) {
      if (strcmp(argv[i], window_color_options[j]) == 0) {
        if (i == argc - 1) {
          fputs("missing argument from the last option\n", stderr);
          return_code = 1;
        } else {
          requested_window_colors[j] = argv[++i];
        }
        goto next_arg;
      }
    }
    fprintf(stderr, "unknown option: %s\n", argv[i]);
    return_code = 1;
  next_arg:;
  }
  return return_code;
}

static int check_connection_error(xcb_connection_t *connection) {
  int error = xcb_connection_has_error(connection);
  if (error) {
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

int main(int argc, char *argv[]) {
  if (parse_options(argc, argv)) {
    usage(argv[0]);
    return EXIT_FAILURE;
  };

  int screen_num;
  xcb_connection_t *const connection = xcb_connect(NULL, &screen_num);
  if (check_connection_error(connection)) {
    return EXIT_FAILURE;
  }

  const xcb_screen_t *const screen = xcb_aux_get_screen(connection, screen_num);
  /* TODO: can this happen? */
  if (screen == NULL) {
    xcb_disconnect(connection);
    fprintf(stderr, "Failed to get the requested screen (screen number %d)\n",
            screen_num);
    return EXIT_FAILURE;
  }

  default_window_colors(screen);

  struct color_request color_requests[ARR_LEN(window_color_options)];
  for (size_t i = 0; i < ARR_LEN(window_color_options); ++i) {
    if (requested_window_colors[i] == NULL) {
      continue;
    }
    /* Just use the screen's default colormap. This game could use a custom
     * colormap, but I expect people to run this game on modern enough hardware.
     */
    color_requests[i] = request_color(connection, screen->default_colormap,
                                      strlen(requested_window_colors[i]),
                                      requested_window_colors[i]);
  }

  xcb_intern_atom_cookie_t atom_requests[ARR_LEN(atom_names)];
  for (size_t i = 0; i < ARR_LEN(atom_names); ++i) {
    atom_requests[i] = xcb_intern_atom_unchecked(
        connection, 1, strlen(atom_names[i]), atom_names[i]);
  }

  xcb_key_symbols_t *const key_syms = xcb_key_symbols_alloc(connection);
  if (key_syms == NULL) {
    xcb_disconnect(connection);
    fputs("Can't allocate space for keyboard map information\n", stderr);
    return EXIT_FAILURE;
  }

  for (size_t i = 0; i < ARR_LEN(window_color_options); ++i) {
    if (requested_window_colors[i] == NULL) {
      continue;
    }
    xcb_generic_error_t *error = NULL;
    uint32_t pixel = read_color_reply(connection, color_requests[i], &error);
    if (error != NULL) {
      fprintf(stderr, "Failed to get color \"%s\": %s; using default color\n",
              requested_window_colors[i],
              xcb_event_get_error_label(error->error_code));
      free(error);
    } else {
      window_colors[i] = pixel;
    }
  }

  xcb_atom_t atoms[ARR_LEN(atom_names)];
  for (size_t i = 0; i < ARR_LEN(atom_names); ++i) {
    xcb_intern_atom_reply_t *atom_reply =
        xcb_intern_atom_reply(connection, atom_requests[i], NULL);
    atoms[i] = atom_reply == NULL ? XCB_ATOM_NONE : atom_reply->atom;
    free(atom_reply);
  }

  /* Create the ball */
  int16_t ball_startx = screen->width_in_pixels / 2 - 150 / 2;
  int16_t ball_starty = screen->height_in_pixels / 2 - 150 / 2;
  struct moving_window ball = moving_window_create(
      connection, screen, window_colors[BALL], ball_startx, ball_starty);
  ball.xspeed = 10;
  ball.yspeed = 10;

  /* Create the paddles */
  struct moving_window left_paddle = moving_window_create(
      connection, screen, window_colors[LEFT_PADDLE], 0, 0);
  struct moving_window right_paddle =
      moving_window_create(connection, screen, window_colors[RIGHT_PADDLE],
                           screen->width_in_pixels - 150, 0);

  window_setup(connection, ball.window, atoms, "XCB pong");
  window_setup(connection, left_paddle.window, atoms, "Left paddle");
  window_setup(connection, right_paddle.window, atoms, "Right paddle");

  xcb_map_window(connection, ball.window);
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
      case XCB_CLIENT_MESSAGE:
        if (((xcb_client_message_event_t *)event)->data.data32[0] ==
            atoms[DELETE_WINDOW_ATOM]) {
          free(event);
          goto end;
        }
        break;
      case XCB_DESTROY_NOTIFY:
        free(event);
        goto end;
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
        /* The game must receive this event if it wants to handle DestroyNotify
         * events. ResizeRedirect could probably be used to reduce useless X11
         * traffic, but I want to handle DestroyNotify properly. */
        xcb_configure_notify_event_t *cn =
            (xcb_configure_notify_event_t *)event;
        struct moving_window *changed_window = NULL;
        if (cn->window == left_paddle.window) {
          changed_window = &left_paddle;
        } else if (cn->window == right_paddle.window) {
          changed_window = &right_paddle;
          /* TODO: use something better for resizing the right paddle */
          changed_window->x = screen->width_in_pixels - cn->width;
        } else if (cn->window == ball.window) {
          changed_window = &ball;
        }
        if (changed_window != NULL) {
          changed_window->width = cn->width;
          changed_window->height = cn->height;
        }
      } break;
      default:
        break;
      }
      free(event);
    }

    if (check_connection_error(connection)) {
      exit_code = EXIT_FAILURE;
      goto end;
    }

    moving_window_move(&left_paddle, screen);
    moving_window_move(&right_paddle, screen);
    moving_window_move(&ball, screen);

    /* TODO: try to deduplicate this code or make it more beautiful */
    if (ball.x < left_paddle.x + left_paddle.width) {
      if (!lost && ball.y + ball.width > left_paddle.y &&
          ball.y < left_paddle.y + left_paddle.height) {
        collide(&ball.xspeed, &ball.x, left_paddle.x + left_paddle.width,
                INT16_MAX);
        /* Make the game advance faster */
        ball.xspeed += 1;

        ball.yspeed += ((ball.y + ball.height / 2) -
                        (left_paddle.y + left_paddle.height / 2)) /
                       5;
        ball.yspeed = clamp(ball.yspeed, -20, 20);
      } else {
        lost = 1;
      }
    } else if (ball.x + ball.width > right_paddle.x) {
      if (!lost && ball.y + ball.height > right_paddle.y &&
          ball.y < right_paddle.y + right_paddle.height) {
        collide(&ball.xspeed, &ball.x, INT16_MIN, right_paddle.x - ball.width);
        ball.xspeed -= 1;

        ball.yspeed += ((ball.y + ball.height / 2) -
                        (right_paddle.y + right_paddle.height / 2)) /
                       5;
        ball.yspeed = clamp(ball.yspeed, -20, 20);
      } else {
        lost = 1;
      }
    } else {
      lost = 0;
    }

    if (ball.x < 0) {
      puts("Right wins!");
      goto end;
    } else if (ball.x > screen->width_in_pixels - ball.width) {
      puts("Left wins!");
      goto end;
    }

    moving_window_send_position(&left_paddle, connection);
    moving_window_send_position(&right_paddle, connection);
    moving_window_send_position(&ball, connection);
    xcb_flush(connection);

    const struct timespec wait_time = {
        .tv_sec = 0,
        .tv_nsec = 50000000,
    };
    nanosleep(&wait_time, NULL);
  }

end:
  xcb_disconnect(connection);
  xcb_key_symbols_free(key_syms);
  return exit_code;
}
