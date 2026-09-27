// Test helper: clicks at absolute output coordinates using the
// wlr-virtual-pointer protocol, keeping one virtual device for the whole
// sequence (wlrctl creates a new device per command, which loses the position).
// Usage: vclick <output width> <output height> x,y [x,y ...]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  if (strcmp(interface, wl_seat_interface.name) == 0) {
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
  } else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
  }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
  .global = handle_global,
  .global_remove = handle_global_remove,
};

int main(int argc, char *argv[]) {
  struct wl_display *display;
  struct zwlr_virtual_pointer_v1 *pointer;
  uint32_t width, height, t = 0;
  int i, x, y;

  if (argc < 4) {
    fprintf(stderr, "usage: %s <output width> <output height> x,y [x,y ...]\n", argv[0]);
    return 1;
  }
  width = atoi(argv[1]);
  height = atoi(argv[2]);

  if ((display = wl_display_connect(NULL)) == NULL) {
    fprintf(stderr, "cannot connect to the wayland display\n");
    return 1;
  }
  wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!manager) {
    fprintf(stderr, "compositor does not support wlr-virtual-pointer\n");
    return 1;
  }

  pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
  wl_display_roundtrip(display);

  for (i = 3; i < argc; i++) {
    if (sscanf(argv[i], "%d,%d", &x, &y) != 2) continue;
    zwlr_virtual_pointer_v1_motion_absolute(pointer, t++, x, y, width, height);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(200000);
    zwlr_virtual_pointer_v1_button(pointer, t++, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    zwlr_virtual_pointer_v1_button(pointer, t++, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    fprintf(stderr, "clicked %d,%d\n", x, y);
    usleep(1500000);
  }

  // keep the device alive a little so the last events are delivered
  usleep(500000);
  zwlr_virtual_pointer_v1_destroy(pointer);
  wl_display_roundtrip(display);
  wl_display_disconnect(display);

  return 0;
}
