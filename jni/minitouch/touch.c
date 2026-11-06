#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <linux/input.h>

#include "touch.h"

int g_verbose = 0;

#define WRITE_EVENT(state, type, code, value) _write_event(state, type, #type, code, #code, value)

static int _write_event(internal_state_t* state,
  uint16_t type, const char* type_name,
  uint16_t code, const char* code_name,
  int32_t value)
{
  // It seems that most devices do not require the event timestamps at all.
  // Left here for reference should such a situation arise.
  //
  //   timespec ts;
  //   clock_gettime(CLOCK_MONOTONIC, &ts);
  //   input_event event = {{ts.tv_sec, ts.tv_nsec / 1000}, type, code, value};

  struct input_event event = {{0, 0}, type, code, value};
  ssize_t result;
  ssize_t length = (ssize_t) sizeof(event);

  if (g_verbose)
    fprintf(stderr, "%-12s %-20s %08x\n", type_name, code_name, value);

  result = write(state->fd, &event, length);
  return result - length;
}

static int next_tracking_id(internal_state_t* state)
{
  if (state->tracking_id < INT_MAX)
  {
    state->tracking_id += 1;
  }
  else
  {
    state->tracking_id = 0;
  }

  return state->tracking_id;
}

static int type_a_commit(internal_state_t* state)
{
  int contact;
  int found_any = 0;

  for (contact = 0; contact < state->max_contacts; ++contact)
  {
    switch (state->contacts[contact].enabled)
    {
      case 1: // WENT_DOWN
        found_any = 1;

        state->active_contacts += 1;

        if (state->has_tracking_id)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        // Send BTN_TOUCH on first contact only.
        if (state->active_contacts == 1 && state->has_key_btn_touch)
          WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 1);

        if (state->has_touch_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

        if (state->has_width_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

        if (state->has_pressure)
          WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

        WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);

        state->contacts[contact].enabled = 2;
        break;
      case 2: // MOVED
        found_any = 1;

        if (state->has_tracking_id)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        if (state->has_touch_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

        if (state->has_width_major)
          WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

        if (state->has_pressure)
          WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
        WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

        WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);
        break;
      case 3: // WENT_UP
        found_any = 1;

        state->active_contacts -= 1;

        if (state->has_tracking_id)
          WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

        // Send BTN_TOUCH only when no contacts remain.
        if (state->active_contacts == 0 && state->has_key_btn_touch)
          WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 0);

        WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);

        state->contacts[contact].enabled = 0;
        break;
    }
  }

  if (found_any)
    WRITE_EVENT(state, EV_SYN, SYN_REPORT, 0);

  return 1;
}

static int type_a_touch_panic_reset_all(internal_state_t* state)
{
  int contact;

  for (contact = 0; contact < state->max_contacts; ++contact)
  {
    switch (state->contacts[contact].enabled)
    {
      case 1: // WENT_DOWN
        // Never reported, so there is nothing to lift. Turning it into
        // WENT_UP would take it off active_contacts without it ever having
        // been added.
        state->contacts[contact].enabled = 0;
        break;
      case 2: // MOVED
        // Force everything that is down to WENT_UP
        state->contacts[contact].enabled = 3;
        break;
    }
  }

  return type_a_commit(state);
}

static int type_a_touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact < 0 || contact >= state->max_contacts)
  {
    return 0;
  }

  if (state->contacts[contact].enabled)
  {
    type_a_touch_panic_reset_all(state);
  }

  state->contacts[contact].enabled = 1;
  state->contacts[contact].x = x;
  state->contacts[contact].y = y;
  state->contacts[contact].pressure = pressure;

  return 1;
}

static int type_a_touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact < 0 || contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  // A contact whose down has not been committed yet stays WENT_DOWN, or the
  // commit would never count it.
  if (state->contacts[contact].enabled != 1)
  {
    state->contacts[contact].enabled = 2;
  }

  state->contacts[contact].x = x;
  state->contacts[contact].y = y;
  state->contacts[contact].pressure = pressure;

  return 1;
}

static int type_a_touch_up(internal_state_t* state, int contact)
{
  if (contact < 0 || contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  // Lifting a contact whose down has not been committed yet just drops it,
  // since it was never reported or counted.
  state->contacts[contact].enabled =
    state->contacts[contact].enabled == 1 ? 0 : 3;

  return 1;
}

static int type_b_commit(internal_state_t* state)
{
  WRITE_EVENT(state, EV_SYN, SYN_REPORT, 0);

  return 1;
}

static int type_b_touch_up(internal_state_t* state, int contact);

static int type_b_touch_panic_reset_all(internal_state_t* state)
{
  int contact;

  for (contact = 0; contact < state->max_contacts; ++contact)
  {
    // Lift every contact properly, so that the device gets the slot's
    // tracking ID -1 (and BTN_TOUCH 0 for the last one) and active_contacts
    // goes back down. Just forgetting them would leave them down on the
    // device and active_contacts too high.
    if (state->contacts[contact].enabled)
    {
      type_b_touch_up(state, contact);
    }
  }

  // Commit even if nothing was down any more, as events are written straight
  // away and uncommitted ones (say "u 0" without "c") would otherwise stay
  // pending.
  return type_b_commit(state);
}

static int type_b_touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact < 0 || contact >= state->max_contacts)
  {
    return 0;
  }

  if (state->contacts[contact].enabled)
  {
    type_b_touch_panic_reset_all(state);
  }

  state->contacts[contact].enabled = 1;
  state->contacts[contact].tracking_id = next_tracking_id(state);
  state->active_contacts += 1;

  WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);
  WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID,
    state->contacts[contact].tracking_id);

  // Send BTN_TOUCH on first contact only.
  if (state->active_contacts == 1 && state->has_key_btn_touch)
    WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 1);

  if (state->has_touch_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

  if (state->has_width_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

  if (state->has_pressure)
    WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, pressure);

  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, x);
  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, y);

  return 1;
}

static int type_b_touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (contact < 0 || contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);

  if (state->has_touch_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

  if (state->has_width_major)
    WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

  if (state->has_pressure)
    WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, pressure);

  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, x);
  WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, y);

  return 1;
}

static int type_b_touch_up(internal_state_t* state, int contact)
{
  if (contact < 0 || contact >= state->max_contacts || !state->contacts[contact].enabled)
  {
    return 0;
  }

  state->contacts[contact].enabled = 0;
  state->active_contacts -= 1;

  WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);
  WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, -1);

  // Send BTN_TOUCH only when no contacts remain.
  if (state->active_contacts == 0 && state->has_key_btn_touch)
    WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 0);

  return 1;
}

int touch_down(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (state->has_mtslot)
  {
    return type_b_touch_down(state, contact, x, y, pressure);
  }
  else
  {
    return type_a_touch_down(state, contact, x, y, pressure);
  }
}

int touch_move(internal_state_t* state, int contact, int x, int y, int pressure)
{
  if (state->has_mtslot)
  {
    return type_b_touch_move(state, contact, x, y, pressure);
  }
  else
  {
    return type_a_touch_move(state, contact, x, y, pressure);
  }
}

int touch_up(internal_state_t* state, int contact)
{
  if (state->has_mtslot)
  {
    return type_b_touch_up(state, contact);
  }
  else
  {
    return type_a_touch_up(state, contact);
  }
}

int touch_panic_reset_all(internal_state_t* state)
{
  if (state->has_mtslot)
  {
    return type_b_touch_panic_reset_all(state);
  }
  else
  {
    return type_a_touch_panic_reset_all(state);
  }
}

int commit(internal_state_t* state)
{
  if (state->has_mtslot)
  {
    return type_b_commit(state);
  }
  else
  {
    return type_a_commit(state);
  }
}
