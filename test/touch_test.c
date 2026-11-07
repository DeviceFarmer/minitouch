// Host test for the contact state machine in jni/minitouch/touch.c.
//
// Runs command sequences against a fake device (a temporary file standing in
// for the evdev node), replays the events written to it the way the kernel
// would see them, and after every commit or reset checks that
// state->active_contacts, minitouch's own contact states and what the kernel
// was told all agree, and that BTN_TOUCH is down exactly while contacts are.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include "touch.h"

typedef struct
{
  const char* name;
  int has_mtslot;
  int has_tracking_id;
  int max_contacts;
} config_t;

static const config_t configs[] = {
  {"type B", 1, 1, 10},
  {"type A with tracking IDs", 0, 1, 10},
  {"type A without tracking IDs", 0, 0, 2},
};

typedef struct
{
  internal_state_t state;
  FILE* device;
  off_t read_offset;
  // The kernel's view, rebuilt from the written events.
  int btn_touch;
  int slot;
  int slot_tracking_id[MAX_SUPPORTED_CONTACTS];
  int frame_contacts;  // type A: contacts with a position in the frame so far
  int has_position;    // type A: the current contact had a position
  int reported_down;   // contacts down as of the last SYN_REPORT
  char error[200];
} harness_t;

static void harness_init(harness_t* h, const config_t* config)
{
  int i;

  memset(h, 0, sizeof(*h));
  h->device = tmpfile();
  if (h->device == NULL)
  {
    perror("tmpfile");
    exit(2);
  }
  h->state.fd = fileno(h->device);
  h->state.has_mtslot = config->has_mtslot;
  h->state.has_tracking_id = config->has_tracking_id;
  h->state.has_key_btn_touch = 1;
  h->state.has_pressure = 1;
  h->state.max_contacts = config->max_contacts;
  for (i = 0; i < MAX_SUPPORTED_CONTACTS; ++i)
    h->slot_tracking_id[i] = -1;
}

static void fail(harness_t* h, const char* message)
{
  if (h->error[0] == 0)
    snprintf(h->error, sizeof(h->error), "%s", message);
}

static void replay(harness_t* h)
{
  struct input_event ev;

  while (pread(h->state.fd, &ev, sizeof(ev), h->read_offset) == sizeof(ev))
  {
    h->read_offset += sizeof(ev);

    if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
    {
      if (ev.value == h->btn_touch)
        fail(h, "BTN_TOUCH sent twice with the same value");
      h->btn_touch = ev.value;
    }
    else if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT)
    {
      h->slot = ev.value;
    }
    else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && h->state.has_mtslot)
    {
      h->slot_tracking_id[h->slot] = ev.value;
    }
    else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X)
    {
      h->has_position = 1;
    }
    else if (ev.type == EV_SYN && ev.code == SYN_MT_REPORT)
    {
      h->frame_contacts += h->has_position;
      h->has_position = 0;
    }
    else if (ev.type == EV_SYN && ev.code == SYN_REPORT)
    {
      int i;

      if (h->state.has_mtslot)
      {
        h->reported_down = 0;
        for (i = 0; i < MAX_SUPPORTED_CONTACTS; ++i)
          h->reported_down += h->slot_tracking_id[i] != -1;
      }
      else
      {
        h->reported_down = h->frame_contacts;
        h->frame_contacts = 0;
      }
    }
  }
}

static void check(harness_t* h)
{
  char message[200];
  int contact;
  int own_down = 0;

  for (contact = 0; contact < h->state.max_contacts; ++contact)
  {
    contact_status_t status = h->state.contacts[contact].status;
    // Type A only reports a contact on commit, so a pending down is not down
    // yet and a pending up still is. Type B never has pending states.
    own_down += status == CONTACT_DOWN || status == CONTACT_UP_PENDING;
  }

  if (h->state.active_contacts != own_down)
  {
    snprintf(message, sizeof(message),
      "active_contacts is %d but %d contacts are down",
      h->state.active_contacts, own_down);
    fail(h, message);
  }

  if (h->reported_down != own_down)
  {
    snprintf(message, sizeof(message),
      "%d contacts are down but the device was told %d",
      own_down, h->reported_down);
    fail(h, message);
  }

  if (h->btn_touch != (own_down > 0))
  {
    snprintf(message, sizeof(message),
      "BTN_TOUCH is %d with %d contacts down", h->btn_touch, own_down);
    fail(h, message);
  }
}

static void run_command(harness_t* h, char command, int contact)
{
  switch (command)
  {
    case 'd':
      touch_down(&h->state, contact, 100 + contact, 200 + contact, 50);
      break;
    case 'm':
      touch_move(&h->state, contact, 300 + contact, 400 + contact, 50);
      break;
    case 'u':
      touch_up(&h->state, contact);
      break;
    case 'c':
      commit(&h->state);
      break;
    case 'r':
      touch_panic_reset_all(&h->state);
      break;
  }

  replay(h);

  if (command == 'c' || command == 'r')
    check(h);
}

// Runs e.g. "d0 d0 c u0 c" and returns 1 on success.
static int run_script(const config_t* config, const char* script, char* error, size_t error_size)
{
  harness_t h;
  const char* p = script;
  int ok;

  harness_init(&h, config);

  while (*p && h.error[0] == 0)
  {
    char command = *p++;
    int contact = 0;

    if (command == ' ')
      continue;

    if (*p == '-' && p[1] >= '0' && p[1] <= '9')
    {
      contact = -(p[1] - '0');
      p += 2;
    }
    else if (*p >= '0' && *p <= '9')
      contact = *p++ - '0';

    run_command(&h, command, contact);
  }

  ok = h.error[0] == 0;
  if (!ok)
    snprintf(error, error_size, "%s (after \"%.*s\")",
      h.error, (int) (p - script), script);

  fclose(h.device);
  return ok;
}

static const struct
{
  const char* name;
  const char* script;
} scenarios[] = {
  {"tap", "d0 c u0 c"},
  {"two finger gesture", "d0 c d1 c m0 m1 c u0 c u1 c"},
  {"down twice before a commit (issue #22)", "d0 d0 c u0 c d0 c u0 c"},
  {"down on a contact that is down", "d0 c d0 c u0 c d0 c u0 c"},
  {"down on a contact that is down, with another one down", "d0 c d1 c d0 c u0 c d1 c u1 c"},
  {"reset with contacts down", "d0 c d1 c r d0 c u0 c"},
  {"reset with an uncommitted down", "d0 c d1 r d0 c u0 c"},
  {"reset with only an uncommitted down", "d0 r d0 c u0 c"},
  {"up before the down was committed", "d0 u0 c d0 c u0 c"},
  {"move before the down was committed", "d0 m0 c u0 c d0 c u0 c"},
  {"move after an uncommitted up", "d0 c u0 m0 c u0 c"},
  {"negative contact", "d-1 c m-1 c u-1 c d0 c u0 c"},
};

int main(void)
{
  char error[300];
  int failures = 0;
  size_t c, s;

  for (c = 0; c < sizeof(configs) / sizeof(configs[0]); ++c)
  {
    int seed;

    for (s = 0; s < sizeof(scenarios) / sizeof(scenarios[0]); ++s)
    {
      if (!run_script(&configs[c], scenarios[s].script, error, sizeof(error)))
      {
        printf("FAIL %s: %s: %s\n", configs[c].name, scenarios[s].name, error);
        failures += 1;
      }
    }

    // Random sequences over three contacts (one more than type A without
    // tracking IDs supports, so out of range contacts are covered too).
    for (seed = 1; seed <= 2000; ++seed)
    {
      static const char commands[] = "dddmmuuucccr";
      char script[3 * 40 + 1];
      char* p = script;
      int i;

      srand(seed);
      for (i = 0; i < 40; ++i)
      {
        char command = commands[rand() % (sizeof(commands) - 1)];
        *p++ = command;
        if (command == 'd' || command == 'm' || command == 'u')
          *p++ = '0' + rand() % 3;
        *p++ = ' ';
      }
      *p = 0;

      if (!run_script(&configs[c], script, error, sizeof(error)))
      {
        printf("FAIL %s: random sequence %d: %s\n", configs[c].name, seed, error);
        failures += 1;
        break;
      }
    }
  }

  if (failures)
  {
    printf("%d failure(s)\n", failures);
    return 1;
  }

  printf("All contact state checks passed\n");
  return 0;
}
