// Host test for the contact state machine in jni/minitouch/touch.c.
//
// Runs command sequences against a fake device (a temporary file standing in
// for the evdev node) and replays the events written to it the way the
// kernel and Android's InputReader would see them. Alongside, it keeps track
// of which contacts the client asked to be down. After every commit or reset
// it checks that the client's view, minitouch's own contact states,
// state->active_contacts and what the device was told all agree, contact by
// contact where the protocol allows, and that BTN_TOUCH is down exactly
// while contacts are.

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
  int has_key_btn_touch;
} config_t;

static const config_t configs[] = {
  {"type B", 1, 1, 10, 1},
  {"type B without BTN_TOUCH", 1, 1, 10, 0},
  {"type A with tracking IDs", 0, 1, 10, 1},
  {"type A with tracking IDs, without BTN_TOUCH", 0, 1, 10, 0},
  {"type A without tracking IDs", 0, 0, 2, 1},
  {"type A without tracking IDs or BTN_TOUCH", 0, 0, 2, 0},
};

typedef struct
{
  internal_state_t state;
  FILE* device;
  off_t read_offset;
  // The contacts the client asked to be down.
  int client_down[MAX_SUPPORTED_CONTACTS];
  // The device's view, rebuilt from the written events.
  int frame_events;    // events written since the last SYN_REPORT
  int btn_touch;
  int slot;
  int slot_tracking_id[MAX_SUPPORTED_CONTACTS];
  int in_contact;      // type A: an ABS_MT_* event started a contact
  int has_x, has_y;    // type A: the current contact has a position
  int tracking_id;     // type A: the current contact's tracking ID
  int frame_contacts;  // type A: contacts in the frame so far
  int frame_ids[MAX_SUPPORTED_CONTACTS]; // type A: their tracking IDs
  int reported_down;   // contacts down as of the last SYN_REPORT
  int reported_ids[MAX_SUPPORTED_CONTACTS]; // type A: their tracking IDs
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
  h->state.has_key_btn_touch = config->has_key_btn_touch;
  h->state.has_pressure = 1;
  h->state.max_contacts = config->max_contacts;
  for (i = 0; i < MAX_SUPPORTED_CONTACTS; ++i)
    h->slot_tracking_id[i] = -1;
  h->tracking_id = -1;
}

static void fail(harness_t* h, const char* message)
{
  if (h->error[0] == 0)
    snprintf(h->error, sizeof(h->error), "%s", message);
}

static void end_type_a_contact(harness_t* h)
{
  if (!h->in_contact)
    return;

  // Android's MultiTouchMotionAccumulator takes any ABS_MT_* event as a
  // contact, so one without a position would be a touch at 0,0.
  if (!h->has_x || !h->has_y)
    fail(h, "type A contact without a position");

  if (h->state.has_tracking_id && h->tracking_id >= 0
    && h->tracking_id < MAX_SUPPORTED_CONTACTS)
    h->frame_ids[h->tracking_id] = 1;

  h->frame_contacts += 1;
  h->in_contact = h->has_x = h->has_y = 0;
  h->tracking_id = -1;
}

static void replay(harness_t* h)
{
  struct input_event ev;

  while (pread(h->state.fd, &ev, sizeof(ev), h->read_offset) == sizeof(ev))
  {
    h->read_offset += sizeof(ev);

    if (ev.type == EV_SYN && ev.code == SYN_REPORT)
    {
      int i;

      // The input core drops a SYN_REPORT with nothing before it, so such a
      // frame never reaches Android. minitouch should not write one either.
      if (h->frame_events == 0)
      {
        fail(h, "empty frame written");
        continue;
      }
      h->frame_events = 0;

      if (h->state.has_mtslot)
      {
        h->reported_down = 0;
        for (i = 0; i < MAX_SUPPORTED_CONTACTS; ++i)
          h->reported_down += h->slot_tracking_id[i] != -1;
      }
      else
      {
        end_type_a_contact(h);
        h->reported_down = h->frame_contacts;
        h->frame_contacts = 0;
        memcpy(h->reported_ids, h->frame_ids, sizeof(h->reported_ids));
        memset(h->frame_ids, 0, sizeof(h->frame_ids));
      }
      continue;
    }

    h->frame_events += 1;

    if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
    {
      if (!h->state.has_key_btn_touch)
        fail(h, "BTN_TOUCH sent to a device without it");
      if (ev.value == h->btn_touch)
        fail(h, "BTN_TOUCH sent twice with the same value");
      h->btn_touch = ev.value;
    }
    else if (ev.type == EV_SYN && ev.code == SYN_MT_REPORT)
    {
      end_type_a_contact(h);
    }
    else if (ev.type == EV_ABS && h->state.has_mtslot)
    {
      if (ev.code == ABS_MT_SLOT)
        h->slot = ev.value;
      else if (ev.code == ABS_MT_TRACKING_ID)
        h->slot_tracking_id[h->slot] = ev.value;
    }
    else if (ev.type == EV_ABS && ev.code >= ABS_MT_TOUCH_MAJOR
      && ev.code <= ABS_MT_TOOL_Y)
    {
      h->in_contact = 1;
      if (ev.code == ABS_MT_POSITION_X)
        h->has_x = 1;
      else if (ev.code == ABS_MT_POSITION_Y)
        h->has_y = 1;
      else if (ev.code == ABS_MT_TRACKING_ID)
        h->tracking_id = ev.value;
    }
  }
}

static void check(harness_t* h)
{
  char message[200];
  int contact;
  int own_down = 0;
  int client_down = 0;

  for (contact = 0; contact < h->state.max_contacts; ++contact)
  {
    contact_status_t status = h->state.contacts[contact].status;
    // Type A only reports a contact on commit, so a pending down is not down
    // yet and a pending up still is. Type B never has pending states.
    int down = status == CONTACT_DOWN || status == CONTACT_UP_PENDING;

    own_down += down;
    client_down += h->client_down[contact];

    if (status == CONTACT_DOWN_PENDING || status == CONTACT_UP_PENDING)
    {
      snprintf(message, sizeof(message),
        "contact %d is still pending after a commit", contact);
      fail(h, message);
    }

    if (down != h->client_down[contact])
    {
      snprintf(message, sizeof(message),
        "contact %d is %s but the client asked for it to be %s",
        contact, down ? "down" : "up", h->client_down[contact] ? "down" : "up");
      fail(h, message);
    }

    // Type A without tracking IDs can only be checked by count, below.
    if (h->state.has_mtslot || h->state.has_tracking_id)
    {
      int device_down = h->state.has_mtslot
        ? h->slot_tracking_id[contact] != -1
        : h->reported_ids[contact];

      if (device_down != h->client_down[contact])
      {
        snprintf(message, sizeof(message),
          "the device has contact %d %s but the client asked for it to be %s",
          contact, device_down ? "down" : "up",
          h->client_down[contact] ? "down" : "up");
        fail(h, message);
      }
    }
  }

  if (h->state.active_contacts != own_down)
  {
    snprintf(message, sizeof(message),
      "active_contacts is %d but %d contacts are down",
      h->state.active_contacts, own_down);
    fail(h, message);
  }

  if (h->reported_down != client_down)
  {
    snprintf(message, sizeof(message),
      "the client has %d contacts down but the device was told %d",
      client_down, h->reported_down);
    fail(h, message);
  }

  if (h->btn_touch != (h->state.has_key_btn_touch && client_down > 0))
  {
    snprintf(message, sizeof(message),
      "BTN_TOUCH is %d with %d contacts down", h->btn_touch, client_down);
    fail(h, message);
  }
}

// What the client expects a command to do, whatever the protocol: a down on
// a contact that is already down resets everything first, moves and ups on
// a contact that is up are ignored, and so are contacts out of range.
static void update_client_view(harness_t* h, char command, long contact)
{
  int in_range = contact >= 0 && contact < h->state.max_contacts;

  switch (command)
  {
    case 'd':
      if (!in_range)
        break;
      if (h->client_down[contact])
        memset(h->client_down, 0, sizeof(h->client_down));
      h->client_down[contact] = 1;
      break;
    case 'u':
      if (in_range)
        h->client_down[contact] = 0;
      break;
    case 'r':
      memset(h->client_down, 0, sizeof(h->client_down));
      break;
  }
}

static void run_command(harness_t* h, char command, long contact)
{
  update_client_view(h, command, contact);

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
    long contact = 0;

    if (command == ' ')
      continue;

    if (command == 'd' || command == 'm' || command == 'u')
      contact = strtol(p, (char**) &p, 10);

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
  {"up twice", "d0 c u0 u0 c d0 c u0 c"},
  {"lift one of two contacts", "d0 c d1 c u0 c m1 c u1 c"},
  {"lift and touch again in one frame", "d0 c d1 c u0 d0 c m1 c u1 c u0 c"},
  {"lift and touch again in one frame, alone", "d0 c u0 d0 c u0 c"},
  {"reset with nothing down", "r d0 c u0 c r c"},
  {"commit with nothing to send", "c d0 c u0 c c"},
  {"negative contact", "d-1 c m-1 c u-1 c d0 c u0 c"},
  {"contact out of range", "d10 c m10 c u10 c d0 c u0 c"},
  {"contact out of range of an int", "d0 c d4294967296 c u4294967296 c u0 c"},
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

    // Random sequences, mostly over three contacts (one more than type A
    // without tracking IDs supports), with the odd contact that is out of
    // range or only in range once narrowed to an int.
    for (seed = 1; seed <= 2000; ++seed)
    {
      static const char commands[] = "dddmmuuucccr";
      const long contacts[] = {
        0, 1, 2, 0, 1, 2, 0, 1, 2,
        -1, configs[c].max_contacts - 1, configs[c].max_contacts, 4294967296L,
      };
      char script[40 * 14 + 1];
      char* p = script;
      int i;

      srand(seed);
      for (i = 0; i < 40; ++i)
      {
        char command = commands[rand() % (sizeof(commands) - 1)];
        *p++ = command;
        if (command == 'd' || command == 'm' || command == 'u')
          p += sprintf(p, "%ld",
            contacts[rand() % (sizeof(contacts) / sizeof(contacts[0]))]);
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
