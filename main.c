#define _POSIX_C_SOURCE 200809L

#include "window.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
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

static const char *const atom_names[] = {
    [PROTOCOL_ATOM] = "WM_PROTOCOLS",
    [DELETE_WINDOW_ATOM] = "WM_DELETE_WINDOW",
    [WINDOW_TYPE_ATOM] = "_NET_WM_WINDOW_TYPE",
    [DIALOG_ATOM] = "_NET_WM_WINDOW_TYPE_DIALOG"};

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
          "\t[-rc {color}]\n"
          "\t[-fps {number}]\n"
          "\t[-borders]\n"
          "\t[+borders]\n",
          command_name);
}

static uint32_t fps = 30;
static bool start_borders = true;

static int parse_options(int argc, char *argv[]) {
  int return_code = 0;
  for (int i = 1; i < argc; ++i) {
    for (size_t j = 0; j < ARR_LEN(window_color_options); ++j) {
      if (strcmp(argv[i], window_color_options[j]) == 0) {
        if (i == argc - 1) {
          fputs("missing argument from the last option\n", stderr);
          return_code = 1;
        } else {
          /* TODO: is using argv like this guaranteed to be safe? */
          requested_window_colors[j] = argv[++i];
        }
        goto next_arg;
      }
    }

    if (strcmp(argv[i], "-fps") == 0) {
      if (i == argc - 1) {
        fputs("missing argument from the last option\n", stderr);
        return_code = 1;
      } else {
        errno = 0;
        long f = strtol(argv[++i], NULL, 10);
        if (errno) {
          fprintf(
              stderr,
              "Failed to parse fps number: %s; using the default value (30)\n",
              strerror(errno));
          goto next_arg;
        }
        /* 1 is invalid because I don't want to set tv_sec :) */
        if (f <= 1) {
          fputs("Invalid fps value; using the default value (30)\n", stderr);
          goto next_arg;
        }
        fps = f;
      }
      goto next_arg;
    }
    /* These are "swapped" like many xeyes options are */
    if (strcmp(argv[i], "-borders") == 0) {
      start_borders = true;
      goto next_arg;
    } else if (strcmp(argv[i], "+borders") == 0) {
      start_borders = false;
      goto next_arg;
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
  struct moving_window ball =
      moving_window_create(connection, screen, window_colors[BALL],
                           start_borders, ball_startx, ball_starty);
  ball.xspeed = 170;
  ball.yspeed = 170;

  /* The paddles start 1 pixel down from the top because putting the left window
   * at (0, 0) causes it to teleport to center after pressing b twice before
   * moving the window (at least on my machine ¯\_(ツ)_/¯) */
  struct moving_window left_paddle = moving_window_create(
      connection, screen, window_colors[LEFT_PADDLE], start_borders, 0, 1);
  struct moving_window right_paddle =
      moving_window_create(connection, screen, window_colors[RIGHT_PADDLE],
                           start_borders, screen->width_in_pixels - 150, 1);

  moving_window_setup(&ball, connection, atoms, "Xwinpong");
  moving_window_setup(&left_paddle, connection, atoms, "Left paddle");
  moving_window_setup(&right_paddle, connection, atoms, "Right paddle");

  xcb_map_window(connection, ball.window);
  xcb_map_window(connection, left_paddle.window);
  xcb_map_window(connection, right_paddle.window);

  xcb_flush(connection);

  const double delta = 1. / fps;
  bool lost = false;
  bool paused = false;
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
            paused = false;
            break;
          case XK_b:
          case XK_B:
            moving_window_swap(&left_paddle, connection);
            moving_window_swap(&ball, connection);
            moving_window_swap(&right_paddle, connection);
            xcb_flush(connection);
            break;
          }
        } else {
          switch (keysym) {
          case XK_w:
          case XK_W:
            left_paddle.yspeed -= 100;
            break;
          case XK_s:
          case XK_S:
            left_paddle.yspeed += 100;
            break;
          case XK_Up:
            right_paddle.yspeed -= 100;
            break;
          case XK_Down:
            right_paddle.yspeed += 100;
            break;
          case XK_p:
          case XK_P:
            paused = true;
            break;
          case XK_b:
          case XK_B:
            moving_window_swap(&left_paddle, connection);
            moving_window_swap(&ball, connection);
            moving_window_swap(&right_paddle, connection);
            xcb_flush(connection);
            break;
          }
        }
        break;
      }
      case XCB_MAP_NOTIFY: {
        /* This event is received when the game starts and when window
         * decorations are toggled. */
        xcb_map_notify_event_t *mn = (xcb_map_notify_event_t *)event;
        if (mn->window == ball.window && mn->override_redirect) {
          /* It's unexpected for this request to return an X11 error, and such
           * an error is handled in the event loop */
          xcb_grab_keyboard_cookie_t cookie = xcb_grab_keyboard_unchecked(
              connection, false, mn->window, XCB_CURRENT_TIME,
              XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
          xcb_grab_keyboard_reply_t *reply =
              xcb_grab_keyboard_reply(connection, cookie, NULL);
          if (reply != NULL) {
            switch (reply->status) {
            case XCB_GRAB_STATUS_SUCCESS:
              break;
            case XCB_GRAB_STATUS_ALREADY_GRABBED:
            case XCB_GRAB_STATUS_FROZEN:
              /* Shouldn't happen if the player toggled window borders while
               * playing */
              fputs("Keyboard already grabbed by another client! You're on "
                    "your own now!\n",
                    stderr);
              break;
            default:
              /* Shouldn't happen unless there's some weird magic happening */
              fprintf(stderr, "Unexpected keyboard grab status: %" PRIu8 "\n",
                      reply->status);
              break;
            }
            free(reply);
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

    moving_window_move(&left_paddle, screen, delta);
    moving_window_move(&right_paddle, screen, delta);
    moving_window_move(&ball, screen, delta);

    /* TODO: try to deduplicate this code or make it more beautiful */
    if (ball.x < left_paddle.x + left_paddle.width) {
      if (!lost && ball.y + ball.height > left_paddle.y &&
          ball.y < left_paddle.y + left_paddle.height) {
        collide(&ball.xspeed, &ball.x, left_paddle.x + left_paddle.width,
                INT16_MAX);
        /* Make the game advance faster */
        ball.xspeed += 15;

        ball.yspeed += ((ball.y + ball.height / 2) -
                        (left_paddle.y + left_paddle.height / 2)) *
                       4;
        ball.yspeed = clamp(ball.yspeed, -400, 400);
      } else {
        lost = true;
      }
    } else if (ball.x + ball.width > right_paddle.x) {
      if (!lost && ball.y + ball.height > right_paddle.y &&
          ball.y < right_paddle.y + right_paddle.height) {
        collide(&ball.xspeed, &ball.x, INT16_MIN, right_paddle.x - ball.width);
        ball.xspeed -= 15;

        ball.yspeed += ((ball.y + ball.height / 2) -
                        (right_paddle.y + right_paddle.height / 2)) *
                       4;
        ball.yspeed = clamp(ball.yspeed, -400, 400);
      } else {
        lost = true;
      }
    } else {
      lost = false;
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
        .tv_nsec = 1000000000. * delta,
    };
    nanosleep(&wait_time, NULL);
  }

end:
  xcb_disconnect(connection);
  xcb_key_symbols_free(key_syms);
  return exit_code;
}
