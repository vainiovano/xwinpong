#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

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

struct paddle {
  xcb_window_t window;
  int16_t x;
  int16_t y;
};

static struct paddle paddle_create(xcb_connection_t *connection,
                                   const xcb_screen_t *screen, int16_t x,
                                   int16_t y) {
  const xcb_window_t window = xcb_generate_id(connection);
  const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
  const uint32_t values[] = {screen->black_pixel, 1};
  xcb_create_window(connection,                    /* connection */
                    XCB_COPY_FROM_PARENT,          /* depth */
                    window,                        /* window id */
                    screen->root,                  /* parent window */
                    x, y,                          /* x, y */
                    150, 150,                      /* width, height */
                    0,                             /* border width */
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, /* class */
                    screen->root_visual,           /* visual */
                    mask, values                   /* masks */
  );
  return (struct paddle){window, x, y};
}

static void paddle_move(struct paddle *paddle, xcb_connection_t *connection,
                        const xcb_screen_t *screen, int16_t dx, int16_t dy) {
  paddle->x = clamp(paddle->x + dx, 0, screen->width_in_pixels - 150);
  paddle->y = clamp(paddle->y + dy, 0, screen->height_in_pixels - 150);
  const uint32_t coords[] = {paddle->x, paddle->y};
  xcb_configure_window(connection, paddle->window,
                       XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
}

int main(void) {
  int screen_num;
  xcb_connection_t *const connection = xcb_connect(NULL, &screen_num);
  int error = xcb_connection_has_error(connection);
  if (error) {
    xcb_disconnect(connection);
    fprintf(stderr, "Can't connect to an X server (error %d)\n",
            error); /* TODO: print some nicer messages */
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

  /*const xcb_gcontext_t foreground = xcb_generate_id(connection);
  uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
  uint32_t values[2] = {screen->black_pixel, 0};
  xcb_create_gc(connection, foreground, screen->root, mask, values);*/

  const xcb_window_t window = xcb_generate_id(connection);

  const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  const uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_KEY_PRESS};

  /* Create the ball */
  int16_t ball_xspeed = 10;
  int16_t ball_yspeed = 10;
  int16_t ball_x = screen->width_in_pixels / 2 - 150 / 2;
  int16_t ball_y = screen->height_in_pixels / 2 - 150 / 2;
  xcb_create_window(connection,                    /* connection */
                    XCB_COPY_FROM_PARENT,          /* depth */
                    window,                        /* window id */
                    screen->root,                  /* parent window */
                    ball_x, ball_y,                /* x, y */
                    150, 150,                      /* width, height */
                    0,                             /* border width */
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, /* class */
                    screen->root_visual,           /* visual */
                    mask, values                   /* masks */
  );

  const xcb_intern_atom_cookie_t protocol_cookie =
      xcb_intern_atom_unchecked(connection, 0, 12, "WM_PROTOCOLS");
  const xcb_intern_atom_cookie_t delete_cookie =
      xcb_intern_atom_unchecked(connection, 0, 16, "WM_DELETE_WINDOW");
  /* Just use UTF-8 instead of XCB_ATOM_WM_NAME */
  const xcb_intern_atom_cookie_t name_cookie =
      xcb_intern_atom_unchecked(connection, 0, 12, "_NET_WM_NAME");

  xcb_intern_atom_reply_t *const protocol_reply =
      xcb_intern_atom_reply(connection, protocol_cookie, NULL);
  xcb_intern_atom_reply_t *const delete_reply =
      xcb_intern_atom_reply(connection, delete_cookie, NULL);
  xcb_intern_atom_reply_t *const name_reply =
      xcb_intern_atom_reply(connection, name_cookie, NULL);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      protocol_reply->atom, XCB_ATOM_ATOM, 32, 1,
                      &delete_reply->atom);
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      name_reply->atom, XCB_ATOM_STRING, 8, 8, "XCB pong");

  /* delete_reply is used for checking the client messages */
  free(protocol_reply);
  free(name_reply);

  /* Create the paddles here */
  struct paddle left_paddle = paddle_create(connection, screen, 0, 0);
  struct paddle right_paddle =
      paddle_create(connection, screen, screen->width_in_pixels - 150, 0);

  xcb_map_window(connection, window);
  xcb_map_window(connection, left_paddle.window);
  xcb_map_window(connection, right_paddle.window);
  xcb_flush(connection);

  int lost = 0;

  for (;;) {
    xcb_generic_event_t *event = NULL;
    while ((event = xcb_poll_for_event(connection)) != NULL) {
      if (event->response_type == 0) {
        /* TODO: is this cast legal? */
        fprintf(stderr, "Received X11 error %d\n",
                ((xcb_generic_error_t *)event)->error_code);
        free(event);
        continue;
      }

      switch (event->response_type & ~0x80) {
      /*case XCB_EXPOSE:
        xcb_poly_rectangle(connection, window, foreground, 1, &rectangle);
        xcb_flush(connection);*/
      case XCB_CLIENT_MESSAGE:
        if (((xcb_client_message_event_t *)event)->data.data32[0] ==
            delete_reply->atom) {
          free(event);
          goto end;
        }
        break;
      case XCB_KEY_PRESS: {
        xcb_key_press_event_t *kp = (xcb_key_press_event_t *)event;
        switch (kp->detail) {
          /* TODO: find these codes from a header file? */
        case 25:
          paddle_move(&left_paddle, connection, screen, 0, -20);
          xcb_flush(connection);
          break;
        case 39:
          paddle_move(&left_paddle, connection, screen, 0, 20);
          xcb_flush(connection);
          break;
        case 111:
          paddle_move(&right_paddle, connection, screen, 0, -20);
          xcb_flush(connection);
          break;
        case 116:
          paddle_move(&right_paddle, connection, screen, 0, 20);
          xcb_flush(connection);
          break;
        }
      }
      default:
        break;
      }
      free(event);
    }

    ball_x += ball_xspeed;
    ball_y += ball_yspeed;

    if (ball_x < left_paddle.x + 150) {
      if (!lost && ball_y > left_paddle.y - 150 &&
          ball_y < left_paddle.y + 150) {
        collide(&ball_xspeed, &ball_x, left_paddle.x + 150, INT16_MAX);
	/* Make the game advance faster */
        ball_xspeed += 1;

        ball_yspeed += (ball_y - left_paddle.y) / 5;
        ball_yspeed = clamp(ball_yspeed, -20, 20);

        lost = 0;
      } else {
        lost = 1;
      }
    } else if (ball_x > right_paddle.x - 150) {
      if (!lost && ball_y > right_paddle.y - 150 &&
          ball_y < right_paddle.y + 150) {
        collide(&ball_xspeed, &ball_x, INT16_MIN, right_paddle.x - 150);
        ball_xspeed -= 1;

        ball_yspeed += (ball_y - right_paddle.y) / 5;
        ball_yspeed = clamp(ball_yspeed, -20, 20);

        lost = 0;
      } else {
        lost = 1;
      }
    }
    collide(&ball_yspeed, &ball_y, 0, screen->height_in_pixels - 150);

    if (ball_x < 0) {
      puts("Right wins!");
      goto end;
    } else if (ball_x > screen->width_in_pixels - 150) {
      puts("Left wins!");
      goto end;
    }

    const uint32_t coords[] = {ball_x, ball_y};
    xcb_configure_window(connection, window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, coords);
    xcb_flush(connection);

    const struct timespec wait_time = {
        .tv_sec = 0,
        .tv_nsec = 50000000,
    };
    nanosleep(&wait_time, NULL);
  }

end:
  free(delete_reply);
  xcb_destroy_window(connection, window);
  xcb_destroy_window(connection, left_paddle.window);
  xcb_destroy_window(connection, right_paddle.window);
  xcb_disconnect(connection);
}
