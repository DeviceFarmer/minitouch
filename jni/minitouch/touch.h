#ifndef MINITOUCH_TOUCH_H
#define MINITOUCH_TOUCH_H

// The contact state machine, kept apart from minitouch.c so that it builds
// without libevdev and can be tested on the host (see test/touch_test.c).

#define MAX_SUPPORTED_CONTACTS 10

struct libevdev;

extern int g_verbose;

typedef struct
{
  int enabled;
  int tracking_id;
  int x;
  int y;
  int pressure;
} contact_t;

typedef struct
{
  int fd;
  int score;
  char path[100];
  struct libevdev* evdev;
  int has_mtslot;
  int has_tracking_id;
  int has_key_btn_touch;
  int has_touch_major;
  int has_width_major;
  int has_pressure;
  int min_pressure;
  int max_pressure;
  int max_x;
  int max_y;
  int max_contacts;
  int max_tracking_id;
  int tracking_id;
  contact_t contacts[MAX_SUPPORTED_CONTACTS];
  int active_contacts;
} internal_state_t;

int touch_down(internal_state_t* state, int contact, int x, int y, int pressure);
int touch_move(internal_state_t* state, int contact, int x, int y, int pressure);
int touch_up(internal_state_t* state, int contact);
int touch_panic_reset_all(internal_state_t* state);
int commit(internal_state_t* state);

#endif
