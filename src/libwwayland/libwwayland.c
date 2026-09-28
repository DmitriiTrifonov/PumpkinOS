#include "sys.h"
#include "script.h"
#include "thread.h"
#include "media.h"
#include "pwindow.h"
#include "average.h"
#include "debug.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <locale.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <linux/input-event-codes.h>
#ifdef LIBDECOR
#include <libdecor.h>
#endif

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "shm.h"

struct texture_t {
  int width, height;
  uint8_t *buf;
};

typedef struct {
  struct wl_buffer *buffer;
  uint8_t *shm_data;
} buffer_t;

#define MAX_EVENTS 64

// used when no size is given and the compositor does not propose one
#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT 768

typedef struct {
  int type, arg1, arg2;
} window_event_t;

typedef struct {
  int width, height, spixel, margin, inside;
  int x, y, buttons;
  uint32_t key;
  window_event_t events[MAX_EVENTS];
  int first_event, num_events;
  uint32_t modifiers;
  int64_t shift_up;
  struct wl_display *display;
  struct wl_shm *shm;
  struct wl_compositor *compositor;
  struct wl_seat *seat;
  struct xdg_wm_base *xdg_wm_base;
  struct xdg_surface *xdg_surface;
  struct wl_surface *surface;
  struct xdg_toplevel *xdg_toplevel;
  struct wl_surface *cursor_surface;
  struct wl_cursor_image *cursor_image;
  struct wl_buffer *cursor_buffer;
  struct xkb_state *xkb_state;
  struct xkb_keymap *xkb_keymap;
  struct xkb_context *xkb_context;
  struct xkb_compose_state *compose_state;
  struct wl_keyboard *keyboard;
  struct wl_touch *touch;
  int32_t touch_id;
  int touch_active, touch_pending, touch_drag;
  int64_t touch_time;
  int config_width, config_height;
  int scale, zoom, output_scale;
  int buffer_width, buffer_height;
  uint32_t compositor_version;
  struct wl_output *output;
  struct zxdg_decoration_manager_v1 *decoration_manager;
  struct zxdg_toplevel_decoration_v1 *decoration;
  struct xdg_toplevel_icon_manager_v1 *icon_manager;
  struct xdg_toplevel_icon_v1 *icon;
#ifdef LIBDECOR
  struct libdecor *libdecor;
  struct libdecor_frame *frame;
#endif
  buffer_t *buffer;
  buffer_t *icon_buffer;
  int configured;
  int running;
  texture_t *background;
} libwwayland_window_t;

static window_provider_t window_provider;

static void noop() {
}

// Wayland handlers queue events, so a press and release that arrive in the
// same dispatch (a quick tap) are not lost. Consecutive motions are merged.
static void push_event(libwwayland_window_t *window, int type, int arg1, int arg2) {
  window_event_t *last;
  int i;

  if (type == WINDOW_MOTION && window->num_events > 0) {
    last = &window->events[(window->first_event + window->num_events - 1) % MAX_EVENTS];
    if (last->type == WINDOW_MOTION) {
      last->arg1 = arg1;
      last->arg2 = arg2;
      return;
    }
  }

  if (window->num_events == MAX_EVENTS) {
    debug(DEBUG_ERROR, "WAYLAND", "event queue full");
    return;
  }

  i = (window->first_event + window->num_events) % MAX_EVENTS;
  window->events[i].type = type;
  window->events[i].arg1 = arg1;
  window->events[i].arg2 = arg2;
  window->num_events++;
}

static int pop_event(libwwayland_window_t *window, int *arg1, int *arg2) {
  window_event_t *ev;

  if (window->num_events == 0) return 0;
  ev = &window->events[window->first_event];
  window->first_event = (window->first_event + 1) % MAX_EVENTS;
  window->num_events--;
  *arg1 = ev->arg1;
  *arg2 = ev->arg2;

  return ev->type;
}

static int map_button(uint32_t button) {
  switch (button) {
    case BTN_LEFT:  return 1;
    case BTN_RIGHT: return 2;
  }

  return 0;
}

static void pointer_set_position(libwwayland_window_t *window, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  int x, y;

  // surface coordinates are logical, the buffer has scale pixels per unit
  // and each PumpkinOS pixel is zoom x zoom buffer pixels
  x = (int)(wl_fixed_to_double(surface_x) * window->scale) - window->margin;
  y = (int)(wl_fixed_to_double(surface_y) * window->scale) - window->margin;

  if (x >= 0 && y >= 0) {
    x /= window->zoom;
    y /= window->zoom;
    if (x < window->width && y < window->height) {
      window->x = x;
      window->y = y;
      push_event(window, WINDOW_MOTION, window->x, window->y);
    }
  }
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (window->inside) {
    pointer_set_position(window, surface_x, surface_y);
  }
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (window->inside) {
    button = map_button(button);

    if (button) {
      switch (state) {
        case WL_POINTER_BUTTON_STATE_PRESSED:
#ifndef LIBDECOR
          if (button == 2 && (window->modifiers & 1)) {
            // shift right click, move the wayland window and don't pass the event to the app
            xdg_toplevel_move(window->xdg_toplevel, window->seat, serial);
            break;
          }
#endif
          window->buttons |= button;
          push_event(window, WINDOW_BUTTONDOWN, button, 0);
          break;
        case WL_POINTER_BUTTON_STATE_RELEASED:
          window->buttons &= ~button;
          push_event(window, WINDOW_BUTTONUP, button, 0);
          break;
      }
    }
  }
}

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (surface == window->surface) {
    wl_pointer_set_cursor(wl_pointer, serial, window->cursor_surface, window->cursor_image->hotspot_x, window->cursor_image->hotspot_y);
    window->inside = 1;
    // enter carries a position, a click may follow without any motion
    pointer_set_position(window, surface_x, surface_y);
  }
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (surface == window->surface) {
    window->inside = 0;
  }
}

static const struct wl_pointer_listener pointer_listener = {
  .enter = pointer_handle_enter,
  .leave = pointer_handle_leave,
  .motion = pointer_handle_motion,
  .button = pointer_handle_button,
  .axis = noop,
};

// Touch: the first finger acts as the pen (left button). If a second finger
// touches the screen within TOUCH_HOLD_US of the first one, the first finger
// acts as the right button instead: the window under it becomes the current
// one and follows the finger. To make this possible, the pen down of the first
// finger is delayed by TOUCH_HOLD_US (a tap shorter than that is delivered as
// pen down and up when the finger is lifted).
#define TOUCH_HOLD_US 150000

static void touch_set_position(libwwayland_window_t *window, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  int x, y;

  x = (int)(wl_fixed_to_double(surface_x) * window->scale) - window->margin;
  y = (int)(wl_fixed_to_double(surface_y) * window->scale) - window->margin;
  x = x < 0 ? 0 : x / window->zoom;
  y = y < 0 ? 0 : y / window->zoom;
  if (x >= window->width) x = window->width - 1;
  if (y >= window->height) y = window->height - 1;
  window->x = x;
  window->y = y;
}

// delivers the delayed pen down of the first finger
static void touch_press(libwwayland_window_t *window, int button) {
  window->touch_pending = 0;
  // the press is handled at the last reported position, so motion goes first
  push_event(window, WINDOW_MOTION, window->x, window->y);
  window->buttons |= button;
  push_event(window, WINDOW_BUTTONDOWN, button, 0);
}

static void touch_check_hold(libwwayland_window_t *window) {
  if (window->touch_pending && sys_get_clock() - window->touch_time >= TOUCH_HOLD_US) {
    touch_press(window, 1);
  }
}

static void touch_release(libwwayland_window_t *window) {
  int button;

  if (window->touch_pending) {
    // a quick tap
    touch_press(window, 1);
  }
  button = window->touch_drag ? 2 : 1;
  window->touch_active = 0;
  window->touch_drag = 0;
  window->buttons &= ~button;
  push_event(window, WINDOW_BUTTONUP, button, 0);
}

static void touch_handle_down(void *data, struct wl_touch *wl_touch, uint32_t serial, uint32_t time, struct wl_surface *surface, int32_t id, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (surface != window->surface) return;

  if (!window->touch_active) {
    window->touch_active = 1;
    window->touch_id = id;
    window->touch_pending = 1;
    window->touch_time = sys_get_clock();
    touch_set_position(window, surface_x, surface_y);
  } else if (window->touch_pending) {
    // second finger while the first one is held: move the window under the first one
    debug(DEBUG_TRACE, "WAYLAND", "two finger touch, dragging");
    window->touch_drag = 1;
    touch_press(window, 2);
  }
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch, uint32_t serial, uint32_t time, int32_t id) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (window->touch_active && id == window->touch_id) {
    touch_release(window);
  }
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch, uint32_t time, int32_t id, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (window->touch_active && id == window->touch_id) {
    touch_set_position(window, surface_x, surface_y);
    if (!window->touch_pending) {
      push_event(window, WINDOW_MOTION, window->x, window->y);
    }
  }
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  if (window->touch_active) {
    touch_release(window);
  }
}

static const struct wl_touch_listener touch_listener = {
  .down = touch_handle_down,
  .up = touch_handle_up,
  .motion = touch_handle_motion,
  .frame = noop,
  .cancel = touch_handle_cancel,
  .shape = noop,
  .orientation = noop,
};

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;
  char *map_shm;

  debug(DEBUG_INFO, "WAYLAND", "keyboard keymap format %u fd %d size %u", format, fd, size);
  map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  window->xkb_keymap = xkb_keymap_new_from_string(window->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map_shm, size);
  close(fd);
  window->xkb_state = xkb_state_new(window->xkb_keymap);
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;
  uint32_t *key;
  char buf[128];

  debug(DEBUG_TRACE, "WAYLAND", "keyboard enter");
  wl_array_for_each(key, keys) {
    xkb_keysym_t sym = xkb_state_key_get_one_sym(window->xkb_state, *key + 8);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    debug(DEBUG_TRACE, "WAYLAND", "sym: %-12s (%d), ", buf, sym);
    xkb_state_key_get_utf8(window->xkb_state, *key + 8, buf, sizeof(buf));
    debug(DEBUG_TRACE, "WAYLAND", "utf8: '%s'\n", buf);
  }
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
  debug(DEBUG_TRACE, "WAYLAND", "keyboard leave");
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  debug(DEBUG_TRACE, "WAYLAND", "keyboard modifiers 0x%08X", mods_depressed);
  xkb_state_update_mask(window->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
  window->modifiers = mods_depressed;
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
  debug(DEBUG_TRACE, "WAYLAND", "keyboard repeat info");
}

static int map_key(libwwayland_window_t *window, uint32_t keycode) {
  enum xkb_compose_status compose_status;
  int key;

  //shift = (window->modifiers & 1) ? 1 : 0;
  //ctrl  = (window->modifiers & 4) ? 1 : 0;
  //alt   = (window->modifiers & 8) ? 1 : 0;
  key = xkb_state_key_get_one_sym(window->xkb_state, keycode);

  if (window->compose_state) {
    xkb_compose_state_feed(window->compose_state, key);
    compose_status = xkb_compose_state_get_status(window->compose_state);
    switch (compose_status) {
     case XKB_COMPOSE_NOTHING:
       break;
     case XKB_COMPOSE_COMPOSING:
       key = 0;
       break;
     case XKB_COMPOSE_CANCELLED:
       xkb_compose_state_reset(window->compose_state);
       key = 0;
       break;
     case XKB_COMPOSE_COMPOSED:
       key = xkb_compose_state_get_one_sym(window->compose_state);
       xkb_compose_state_reset(window->compose_state);
       break;
    }
  }

  if (key) {
    switch (key) {
      case XKB_KEY_BackSpace: key = 8;  break;
      case XKB_KEY_Return:    key = 10; break;
      case XKB_KEY_Escape:    key = 27; break;
      case XKB_KEY_Right:     key = WINDOW_KEY_RIGHT; break;
      case XKB_KEY_Left:      key = WINDOW_KEY_LEFT; break;
      case XKB_KEY_Down:      key = WINDOW_KEY_DOWN; break;
      case XKB_KEY_Up:        key = WINDOW_KEY_UP; break;
      case XKB_KEY_Page_Up:   key = WINDOW_KEY_PGUP; break;
      case XKB_KEY_Page_Down: key = WINDOW_KEY_PGDOWN; break;
      case XKB_KEY_Home:      key = WINDOW_KEY_HOME; break;
      case XKB_KEY_End:       key = WINDOW_KEY_END; break;
      case XKB_KEY_Insert:    key = WINDOW_KEY_INS; break;
      case XKB_KEY_Delete:    key = WINDOW_KEY_DEL; break;
      case XKB_KEY_F1:        key = WINDOW_KEY_F1; break;
      case XKB_KEY_F2:        key = WINDOW_KEY_F2; break;
      case XKB_KEY_F3:        key = WINDOW_KEY_F3; break;
      case XKB_KEY_F4:        key = WINDOW_KEY_F4; break;
      case XKB_KEY_F5:        key = WINDOW_KEY_F5; break;
      case XKB_KEY_F6:        key = WINDOW_KEY_F6; break;
      case XKB_KEY_F7:        key = WINDOW_KEY_F7; break;
      case XKB_KEY_F8:        key = WINDOW_KEY_F8; break;
      case XKB_KEY_F9:        key = WINDOW_KEY_F9; break;
      case XKB_KEY_F10:       key = WINDOW_KEY_F10; break;
      case XKB_KEY_F11:       key = WINDOW_KEY_F11; break;
      case XKB_KEY_F12:       key = WINDOW_KEY_F12; break;
      default:
        if (key > 0xff) key = 0;
        break;
    }
  }

  return key;
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  key = map_key(window, key + 8); 
  if (key) {
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      debug(DEBUG_TRACE, "WAYLAND", "key down %d", key);
      window->key = key;
      push_event(window, WINDOW_KEYDOWN, key, 0);
    } else if (window->key) {
      debug(DEBUG_TRACE, "WAYLAND", "key up %d", window->key);
      push_event(window, WINDOW_KEYUP, window->key, 0);
      window->key = 0;
    }
  }
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
  .keymap = wl_keyboard_keymap,
  .enter = wl_keyboard_enter,
  .leave = wl_keyboard_leave,
  .key = wl_keyboard_key,
  .modifiers = wl_keyboard_modifiers,
  .repeat_info = wl_keyboard_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;
  struct wl_pointer *pointer;

  if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointer_listener, window);
  }

  if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
    window->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(window->keyboard, &wl_keyboard_listener, window);
  }

  if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !window->touch) {
    debug(DEBUG_INFO, "WAYLAND", "seat has touch capability");
    window->touch = wl_seat_get_touch(seat);
    wl_touch_add_listener(window->touch, &touch_listener, window);
  }
}

static const struct wl_seat_listener seat_listener = {
  .capabilities = seat_handle_capabilities,
};

static void xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}   

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  .ping = xdg_wm_base_handle_ping,
};

static void output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  debug(DEBUG_INFO, "WAYLAND", "output scale %d", factor);
  window->output_scale = factor;
}

static const struct wl_output_listener output_listener = {
  .geometry = noop,
  .mode = noop,
  .done = noop,
  .scale = output_handle_scale,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  debug(DEBUG_INFO, "WAYLAND", "registry name %u interface \"%s\" version %u", name, interface, version);

  if (sys_strcmp(interface, wl_shm_interface.name) == 0) {
    debug(DEBUG_INFO, "WAYLAND", "binding shm interface");
    window->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);

  } else if (sys_strcmp(interface, wl_seat_interface.name) == 0) {
    debug(DEBUG_INFO, "WAYLAND", "binding seat interface");
    window->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_seat_add_listener(window->seat, &seat_listener, window);

  } else if (sys_strcmp(interface, wl_compositor_interface.name) == 0) {
    debug(DEBUG_INFO, "WAYLAND", "binding compositor interface");
    // version 3 for wl_surface_set_buffer_scale, 4 for wl_surface_damage_buffer
    window->compositor_version = version < 4 ? version : 4;
    window->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, window->compositor_version);

  } else if (sys_strcmp(interface, xdg_wm_base_interface.name) == 0) {
    debug(DEBUG_INFO, "WAYLAND", "binding xdg wm base interface");
    window->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(window->xdg_wm_base, &xdg_wm_base_listener, NULL);

  } else if (sys_strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
    debug(DEBUG_INFO, "WAYLAND", "binding zxdg decoration manager interface");
    window->decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    // the toplevel decoration is created later, after xdg_toplevel exists

  } else if (sys_strcmp(interface, wl_output_interface.name) == 0 && !window->output && version >= 2) {
    // only the first output is used, for its scale
    debug(DEBUG_INFO, "WAYLAND", "binding output interface");
    window->output = wl_registry_bind(registry, name, &wl_output_interface, 2);
    wl_output_add_listener(window->output, &output_listener, window);

  } else if (sys_strcmp(interface, "xdg_toplevel_icon_manager_v1") == 0) {
    debug(DEBUG_INFO, "WAYLAND", "binding xdg toplevel icon interface");
    window->icon_manager = wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
  } 
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
  .global = handle_global,
  .global_remove = handle_global_remove,
};  
  
static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  xdg_surface_ack_configure(xdg_surface, serial);

  if (window->configured) {
    wl_surface_commit(window->surface);
  }

  window->configured = 1;
} 

static const struct xdg_surface_listener xdg_surface_listener = {
  .configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  window->running = 0;
} 

static void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states) {
  libwwayland_window_t *window = (libwwayland_window_t *)data;

  // a size of 0 means the client may choose its own size
  debug(DEBUG_INFO, "WAYLAND", "toplevel configure %dx%d", width, height);
  window->config_width = width;
  window->config_height = height;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  .configure = xdg_toplevel_handle_configure,
  .close = xdg_toplevel_handle_close,
};

#ifdef LIBDECOR
static void libdecor_error(struct libdecor *context, enum libdecor_error error, const char *message) {
  debug(DEBUG_ERROR, "WAYLAND", "libdecor error %d: %s", error, message);
}

static struct libdecor_interface libdecor_interface = {
  libdecor_error
};

static void decoration_frame_configure(struct libdecor_frame *frame, struct libdecor_configuration *configuration, void *user_data) {
  libwwayland_window_t *window = (libwwayland_window_t *)user_data;
  struct libdecor_state *state;

  debug(DEBUG_INFO, "WAYLAND", "libdecor configure");
  window->configured = 1;
  state = libdecor_state_new(window->width, window->height);
  libdecor_frame_commit(frame, state, configuration);
  libdecor_state_free(state);
}

static void decoration_frame_close(struct libdecor_frame *frame, void *user_data) {
  debug(DEBUG_TRACE, "WAYLAND", "libdecor close");
  // SDL_WINDOWEVENT_CLOSE
}

static void decoration_frame_commit(struct libdecor_frame *frame, void *user_data) {
  debug(DEBUG_TRACE, "WAYLAND", "libdecor commit");
  // SDL_WINDOWEVENT_EXPOSED
}

static struct libdecor_frame_interface libdecor_frame_interface = {
  decoration_frame_configure,
  decoration_frame_close,
  decoration_frame_commit
};
#endif

static void libwwayland_status(libwwayland_window_t *window, int *x, int *y, int *buttons) {
  if (window) {
    *x = window->x;
    *y = window->y;
    *buttons = window->buttons;
  }
}

static void libwwayland_title(libwwayland_window_t *window, char *title) {
  if (window && title) {
    debug(DEBUG_TRACE, "WAYLAND", "set title [%s]", title);

#ifdef LIBDECOR
    if (window->frame) {
      libdecor_frame_set_title(window->frame, title);
    } else {
      xdg_toplevel_set_title(window->xdg_toplevel, title);
    }
#else
    xdg_toplevel_set_title(window->xdg_toplevel, title);
#endif
  }
}

static buffer_t *create_buffer(libwwayland_window_t *window, int width, int height) {
  buffer_t *buffer;
  struct wl_shm_pool *pool;
  int stride = width * window->spixel;
  int size = stride * height;
  int fd;

  if ((buffer = sys_malloc(sizeof(buffer_t))) == NULL) {
    return NULL;
  }

  fd = create_shm_file(size);
  if (fd < 0) {
    debug(DEBUG_ERROR, "WAYLAND", "creating a buffer file for %d B failed: %m", size);
    return NULL;
  }

  buffer->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (buffer->shm_data == MAP_FAILED) {
    debug(DEBUG_ERROR, "WAYLAND", "mmap failed: %m");
    close(fd);
    sys_free(buffer);
    return NULL;
  }

  pool = wl_shm_create_pool(window->shm, fd, size);
  buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  return buffer;
}

static void destroy_buffer(buffer_t *buffer) {
  if (buffer) {
    wl_buffer_destroy(buffer->buffer);
    sys_free(buffer);
  }
}

static int libwwayland_icon(libwwayland_window_t *window, uint32_t *raw, int width, int height) {
  struct xdg_toplevel *xdg_toplevel;
  uint32_t *p;
  int i, r = -1;

  if (window && raw && width > 0 && height > 0 && window->icon_manager) {
    debug(DEBUG_TRACE, "WAYLAND", "set icon %dx%d", width, height);
    if ((window->icon = xdg_toplevel_icon_manager_v1_create_icon(window->icon_manager)) != NULL) {
      if ((window->icon_buffer = create_buffer(window, width, height)) == NULL) {
        p = (uint32_t *)window->icon_buffer->shm_data;
        for (i = 0; i < width * height; i++) {
          p[i] = raw[i];
        }
        xdg_toplevel_icon_v1_add_buffer(window->icon, window->icon_buffer->buffer, 1);
#ifdef LIBDECOR
        xdg_toplevel = window->frame ? libdecor_frame_get_xdg_toplevel(window->frame) : window->xdg_toplevel;
#else
        xdg_toplevel = window->xdg_toplevel;
#endif
        xdg_toplevel_icon_manager_v1_set_icon(window->icon_manager, xdg_toplevel, window->icon);
        r = 0;
      }
    }
  }

  return r;
}

static char *libwwayland_clipboard(libwwayland_window_t *_window, char *clipboard, int len) {
  //libwwayland_window_t *window;
  char *r = NULL;

  if (_window) {
    //window = (libwwayland_window_t *)_window;

    if (clipboard && len > 0) {
      // copy to clipboard not implemented
    }

    // paste from clipboard not implemented
  }

  return r;
}

static int libwwayland_event2(libwwayland_window_t *window, int wait, int *arg1, int *arg2) {
  struct pollfd pfd;
  int poll_result, r;

  if (!window) return -1;
  if (!window->running) return -1;
  if (thread_must_end()) return -1;

  // events queued by a previous dispatch are delivered first
  touch_check_hold(window);
  if ((r = pop_event(window, arg1, arg2)) != 0) return r;

  wl_display_flush(window->display);

  pfd.fd = wl_display_get_fd(window->display);
  pfd.events = POLLIN;
  pfd.revents = 0;
  poll_result = poll(&pfd, 1, wait);
  if (poll_result == -1) return -1;
  if (poll_result == 0) return 0;
  if (!(pfd.revents & POLLIN)) return 0;

  if (wl_display_dispatch(window->display) == -1) {
    debug(DEBUG_ERROR, "WAYLAND", "wl_display_dispatch failed");
    return -1;
  }

  touch_check_hold(window);
  return pop_event(window, arg1, arg2);
}

static int libwwayland_window_show_cursor(window_t *_window, int show) {
  //libwwayland_window_t *window;
  int r = -1;

  if (_window) {
    //window = (libwwayland_window_t *)_window;
    r = 0;
  }

  return r;
}

static window_t *libwwayland_window_create(int encoding, int *width, int *height, int xfactor, int yfactor, int rotate,
     int fullscreen, int software, char *driver, void *data) {

  libwwayland_window_t *window;
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_cursor_theme *cursor_theme;
  struct wl_cursor *cursor;
  struct xkb_compose_table *compose_table;
  uint32_t spixel;
  buffer_t *tmp = NULL;
  char *s;
  int i;
  const char *locale;

  switch (encoding) {
    case ENC_RGBA:
      spixel = sizeof(uint32_t);
      break;
    default:
      debug(DEBUG_ERROR, "WAYLAND", "invalid encoding %s", video_encoding_name(encoding));
      return NULL;
  }

  if ((display = wl_display_connect(NULL)) == NULL) {
    debug(DEBUG_ERROR, "WAYLAND", "wl_display_connect failed");
    return NULL;
  }

  if ((window = sys_calloc(1, sizeof(libwwayland_window_t))) != NULL) {
    window->display = display;
    window->width = *width;
    window->height = *height;
    window->spixel = spixel;
    window->running = 1;
    window->margin = 4;
    window->scale = 1;
    window->zoom = 1;
    window->output_scale = 1;

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, window);
    if (wl_display_roundtrip(display) == -1) {
      debug(DEBUG_ERROR, "WAYLAND", "wl_registry_add_listener failed");
      sys_free(window);
      return NULL;
    } 

    if (window->shm == NULL || window->compositor == NULL || window->xdg_wm_base == NULL) {
      debug(DEBUG_ERROR, "WAYLAND", "no wl_shm, wl_compositor or xdg_wm_base support");
      sys_free(window);
      return NULL;
    }

    window->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    window->compose_state = NULL;
    locale = setlocale(LC_CTYPE, NULL);
    compose_table = xkb_compose_table_new_from_locale(window->xkb_context, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (compose_table) {
      window->compose_state = xkb_compose_state_new(compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    } else { 
      debug(DEBUG_ERROR, "WAYLAND", "xkb_compose_table_new_from_locale failed");
    }

    cursor_theme = wl_cursor_theme_load(NULL, 24, window->shm);
    cursor = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
    window->cursor_image = cursor->images[0];
    window->cursor_buffer = wl_cursor_image_get_buffer(window->cursor_image);
    window->cursor_surface = wl_compositor_create_surface(window->compositor);
    wl_surface_attach(window->cursor_surface, window->cursor_buffer, 0, 0);
    wl_surface_commit(window->cursor_surface);

    window->surface = wl_compositor_create_surface(window->compositor);

    wl_surface_attach(window->surface, NULL, 0, 0);
    wl_surface_commit(window->surface);

    if (fullscreen) {
      window->margin = 0;
    }

#ifdef LIBDECOR
    if (!window->decoration_manager && !fullscreen) {
      window->libdecor = libdecor_new(window->display, &libdecor_interface);
      if (window->libdecor) {
        window->margin = 0;
        window->frame = libdecor_decorate(window->libdecor, window->surface, &libdecor_frame_interface, window);
        //libdecor_frame_set_app_id(window->frame, c->classname);
        libdecor_frame_map(window->frame);
      } else {
        debug(DEBUG_ERROR, "WAYLAND", "libdecor_decorate failed");
      }
    }

    if (!window->frame) {
#endif
      debug(DEBUG_INFO, "WAYLAND", "xdg_toplevel from xdg");
      window->xdg_surface = xdg_wm_base_get_xdg_surface(window->xdg_wm_base, window->surface);
      window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
      xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, window);
      xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window);
      if (window->decoration_manager) {
        window->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(window->decoration_manager, window->xdg_toplevel);
        zxdg_toplevel_decoration_v1_set_mode(window->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
      }
      if (fullscreen) {
        xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
      }
#ifdef LIBDECOR
    }
#endif

    wl_surface_commit(window->surface);
    while (!window->configured) {
      wl_display_flush(window->display);
      wl_display_dispatch(window->display);
      if (thread_must_end()) break;
    }

    // Some compositors (sway) answer the initial commit with a 0x0 configure
    // and send the fullscreen size only after the surface is mapped. Map it
    // with a 1x1 buffer until the size is known.
    if (fullscreen && window->config_width <= 0 && (tmp = create_buffer(window, 1, 1)) != NULL) {
      wl_surface_attach(window->surface, tmp->buffer, 0, 0);
      wl_surface_commit(window->surface);
      for (i = 0; window->config_width <= 0 && i < 10; i++) {
        if (wl_display_roundtrip(window->display) == -1) break;
      }
    }

    // PUMPKIN_WAYLAND_ZOOM=n (fullscreen only): the buffer has the physical
    // resolution of the output (buffer scale = output scale, so the compositor
    // does not scale it) and each PumpkinOS pixel is drawn as n x n physical
    // pixels. On a phone with a large output scale this gives a crisp picture
    // with a PumpkinOS screen of a usable size.
    if (fullscreen && (s = sys_getenv("PUMPKIN_WAYLAND_ZOOM")) != NULL) {
      window->zoom = sys_atoi(s);
      if (window->zoom < 1) window->zoom = 1;
      if (window->zoom > 8) window->zoom = 8;
      if (window->compositor_version >= 3) {
        window->scale = window->output_scale;
      }
      debug(DEBUG_INFO, "WAYLAND", "buffer scale %d, zoom %d", window->scale, window->zoom);
    }

    // in fullscreen, or when no size was given, use the size proposed by the compositor
    if (fullscreen || window->width <= 0 || window->height <= 0) {
      if (window->config_width > 2*window->margin && window->config_height > 2*window->margin) {
        window->buffer_width = window->config_width * window->scale;
        window->buffer_height = window->config_height * window->scale;
        window->width = (window->buffer_width - 2*window->margin) / window->zoom;
        window->height = (window->buffer_height - 2*window->margin) / window->zoom;
      } else if (window->width <= 0 || window->height <= 0) {
        window->width = DEFAULT_WIDTH;
        window->height = DEFAULT_HEIGHT;
      }
      debug(DEBUG_INFO, "WAYLAND", "using window size %dx%d", window->width, window->height);
      *width = window->width;
      *height = window->height;
    }

#ifdef LIBDECOR
    if (window->frame) {
      debug(DEBUG_INFO, "WAYLAND", "xdg_toplevel from libdecor");
      window->xdg_surface = libdecor_frame_get_xdg_surface(window->frame);
      window->xdg_toplevel = libdecor_frame_get_xdg_toplevel(window->frame);
      libdecor_frame_unset_capabilities(window->frame, LIBDECOR_ACTION_RESIZE);
      libdecor_frame_set_min_content_size(window->frame, window->width, window->height);
      libdecor_frame_set_max_content_size(window->frame, window->width, window->height);
      libdecor_frame_set_visibility(window->frame, 1);
    }
#endif

    if (window->buffer_width == 0 || window->buffer_height == 0) {
      window->buffer_width = window->width * window->zoom + 2*window->margin;
      window->buffer_height = window->height * window->zoom + 2*window->margin;
    }

    if ((window->buffer = create_buffer(window, window->buffer_width, window->buffer_height)) == NULL) {
      destroy_buffer(tmp);
      sys_free(window);
      return NULL;
    }

    if (window->scale > 1) {
      wl_surface_set_buffer_scale(window->surface, window->scale);
    }
    wl_surface_attach(window->surface, window->buffer->buffer, 0, 0);
    wl_surface_commit(window->surface);
    destroy_buffer(tmp);

    xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, window->buffer_width / window->scale, window->buffer_height / window->scale);
    wl_surface_commit(window->surface);
  }

  return (window_t *)window;
}

static int libwwayland_window_render(window_t *_window) {
  return 0;
}

static int libwwayland_window_erase(window_t *_window, uint32_t bg) {
  libwwayland_window_t *window;
  int r = -1;

  if (_window) {
    window = (libwwayland_window_t *)_window;
    sys_memset(window->buffer->shm_data, 0, window->buffer_width * window->buffer_height * window->spixel);

    if (window->background) {
      window_provider.draw_texture(_window, window->background, 0, 0);
    }

    r = 0;
  }

  return r;
}

static texture_t *libwwayland_window_create_texture(window_t *_window, int width, int height) {
  libwwayland_window_t *window;
  uint8_t *buf;
  texture_t *texture = NULL;

  window = (libwwayland_window_t *)_window;

  if (window && width > 0 && height > 0) {
    if ((buf = sys_calloc(width * height, window->spixel)) != NULL) {
      if ((texture = sys_calloc(1, sizeof(texture_t))) != NULL) {
        texture->width = width;
        texture->height = height;
        texture->buf = buf;
      } else {
        sys_free(buf);
      }
    }
  }

  return texture;
}

static int libwwayland_window_destroy_texture(window_t *_window, texture_t *texture) {
  libwwayland_window_t *window;
  int r = -1;

  window = (libwwayland_window_t *)_window;

  if (window && texture) {
    sys_free(texture->buf);
    sys_free(texture);
  }

  return r;
}

static int libwwayland_window_update_texture_rect(window_t *_window, texture_t *texture, uint8_t *src, int tx, int ty, int w, int h) {
  libwwayland_window_t *window;
  uint8_t *s, *d;
  int pitch, len, i, r = -1;

  if (_window && texture && w > 0 && h > 0 && tx >= 0 && ty >= 0 && (tx+w) <= texture->width && (ty+h) <= texture->height) {
    window = (libwwayland_window_t *)_window;
    s = &src[(ty * texture->width + tx) * window->spixel];
    d = &texture->buf[(ty * texture->width + tx) * window->spixel];
    pitch = texture->width * window->spixel;
    len = w * window->spixel;
    for (i = 0; i < h; i++) {
      sys_memcpy(d, s, len);
      s += pitch;
      d += pitch;
    }
    r = 0;
  }

  return r;
}

static int libwwayland_window_update_texture(window_t *_window, texture_t *texture, uint8_t *src) {
  libwwayland_window_t *window;
  int r = -1;

  if (_window && texture && src) {
    window = (libwwayland_window_t *)_window;
    sys_memcpy(texture->buf, src, texture->width * texture->height * window->spixel);
    r = 0;
  }

  return r;
}

static int libwwayland_window_draw_texture_rect(window_t *_window, texture_t *texture, int tx, int ty, int w, int h, int x, int y) {
  libwwayland_window_t *window = (libwwayland_window_t *)_window;
  uint8_t *s, *d;
  uint32_t *s32, *d32;
  int spitch, dpitch, len, i, j, k, r = -1;

  if (window && texture && w > 0 && h > 0 && tx >= 0 && ty >= 0 && tx+w <= texture->width && ty+h <= texture->height &&
      x < window->width && y < window->height && x+w > 0 && y+h > 0) {

    if (x < 0) {
      tx -= x;
      w += x;
      x = 0;
    }

    if (y < 0) {
      ty -= y;
      h += y;
      y = 0;
    }

    if (w > 0 && h > 0) {
      if (x + w > window->width) {
        w = window->width - x;
      }

      if (y + h > window->height) {
        h = window->height - y;
      }

      // from now on, x, y, w and h are in buffer pixels
      x = window->margin + x * window->zoom;
      y = window->margin + y * window->zoom;

      s = &texture->buf[(ty * texture->width + tx) * window->spixel];
      d = &window->buffer->shm_data[(y * window->buffer_width + x) * window->spixel];
      spitch = texture->width * window->spixel;
      dpitch = window->buffer_width * window->spixel;
      len = w * window->zoom * window->spixel;

      if (window->zoom == 1) {
        for (i = 0; i < h; i++) {
          sys_memcpy(d, s, len);
          s += spitch;
          d += dpitch;
        }
      } else {
        // spixel is always 4 (ENC_RGBA)
        for (i = 0; i < h; i++) {
          s32 = (uint32_t *)s;
          d32 = (uint32_t *)d;
          for (j = 0; j < w; j++) {
            for (k = 0; k < window->zoom; k++) {
              *d32++ = s32[j];
            }
          }
          for (k = 1; k < window->zoom; k++) {
            sys_memcpy(d + k * dpitch, d, len);
          }
          s += spitch;
          d += dpitch * window->zoom;
        }
      }
      w *= window->zoom;
      h *= window->zoom;
      wl_surface_attach(window->surface, window->buffer->buffer, 0, 0);
      if (window->compositor_version >= 4) {
        wl_surface_damage_buffer(window->surface, x, y, w, h);
      } else {
        wl_surface_damage(window->surface, x / window->scale, y / window->scale, (w + window->scale - 1) / window->scale + 1, (h + window->scale - 1) / window->scale + 1);
      }
      wl_surface_commit(window->surface);
      r = 0;
    } else {
      debug(DEBUG_ERROR, "WAYLAND", "invalid libwwayland_window_draw_texture_rect %d,%d %dx%d %d,%d", tx, ty, w, h, x, y);
    }
  }

  return r;
}

static int libwwayland_window_draw_texture(window_t *_window, texture_t *texture, int x, int y) {
  int r = -1;

  if (texture) {
    r = libwwayland_window_draw_texture_rect(_window, texture, 0, 0, texture->width, texture->height, x, y);
  }

  return r;
}

static int libwwayland_window_background(window_t *_window, uint32_t *raw, int width, int height) {
  libwwayland_window_t *window;
  int r = -1;

  if (_window && raw) {
    window = (libwwayland_window_t *)_window;
    debug(DEBUG_TRACE, "WAYLAND", "background %dx%d", width, height);
    if (window->background) {
      libwwayland_window_destroy_texture(_window, window->background);
    }
    window->background = libwwayland_window_create_texture(_window, width, height);
    libwwayland_window_update_texture(_window, window->background, (uint8_t *)raw);
    r = 0;
  }

  return r;
}

static void libwwayland_window_status(window_t *window, int *x, int *y, int *buttons) {
  libwwayland_status((libwwayland_window_t *)window, x, y, buttons);
}

static void libwwayland_window_title(window_t *window, char *title) {
  libwwayland_title((libwwayland_window_t *)window, title);
}

static int libwwayland_window_icon(window_t *window, uint32_t *raw, int width, int height) {
  return libwwayland_icon((libwwayland_window_t *)window, raw, width, height);
}

static char *libwwayland_window_clipboard(window_t *window, char *clipboard, int len) {
  return libwwayland_clipboard((libwwayland_window_t *)window, clipboard, len);
}

static int libwwayland_window_event2(window_t *window, int wait, int *arg1, int *arg2) {
  return libwwayland_event2((libwwayland_window_t *)window, wait, arg1, arg2);
}

static int libwwayland_window_update(window_t *_window, int x, int y, int width, int height) {
  return 0;
}

static int libwwayland_window_destroy(window_t *_window) {
  libwwayland_window_t *window;
  int r = -1;

  if (_window) {
    window = (libwwayland_window_t *)_window;
    if (window->background) {
      libwwayland_window_destroy_texture(_window, window->background);
    }
#ifdef LIBDECOR
    if (window->libdecor) {
      libdecor_unref(window->libdecor);
    }
#endif
    if (window->icon_buffer) {
      destroy_buffer(window->icon_buffer);
    }
    xdg_toplevel_destroy(window->xdg_toplevel);
    xdg_surface_destroy(window->xdg_surface);
    wl_surface_destroy(window->surface);
    if (window->buffer) {
      destroy_buffer(window->buffer);
    }
    sys_free(window);
    r = 0;
  }

  return r;
}

static int libwwayland_window_average(window_t *window, int *x, int *y, int ms) {
  return average_click(&window_provider, window, x, y, ms);
}

static int libwwayland_calib(int pe) {
  window_provider.average = libwwayland_window_average;
  return 0;
}

int libwwayland_load(void) {
  sys_memset(&window_provider, 0, sizeof(window_provider));
  window_provider.create = libwwayland_window_create;
  window_provider.destroy = libwwayland_window_destroy;
  window_provider.erase = libwwayland_window_erase;
  window_provider.render = libwwayland_window_render;
  window_provider.background = libwwayland_window_background;
  window_provider.create_texture = libwwayland_window_create_texture;
  window_provider.destroy_texture = libwwayland_window_destroy_texture;
  window_provider.update_texture = libwwayland_window_update_texture;
  window_provider.draw_texture = libwwayland_window_draw_texture;
  window_provider.status = libwwayland_window_status;
  window_provider.title = libwwayland_window_title;
  window_provider.icon = libwwayland_window_icon;
  window_provider.clipboard = libwwayland_window_clipboard;
  window_provider.event2 = libwwayland_window_event2;
  window_provider.update = libwwayland_window_update;
  window_provider.draw_texture_rect = libwwayland_window_draw_texture_rect;
  window_provider.update_texture_rect = libwwayland_window_update_texture_rect;
  window_provider.show_cursor = libwwayland_window_show_cursor;

  return 0;
}

int libwwayland_init(int pe, script_ref_t obj) {
  debug(DEBUG_INFO, "WAYLAND", "registering provider %s", WINDOW_PROVIDER);
  script_set_pointer(pe, WINDOW_PROVIDER, &window_provider);

  script_add_iconst(pe, obj, "motion", WINDOW_MOTION);
  script_add_iconst(pe, obj, "down", WINDOW_BUTTONDOWN);
  script_add_iconst(pe, obj, "up", WINDOW_BUTTONUP);
  script_add_iconst(pe, obj, "hdepth", 32);

  script_add_function(pe, obj, "calib", libwwayland_calib);

  return 0;
}

int libwwayland_unload(void) {
  return 0;
}
