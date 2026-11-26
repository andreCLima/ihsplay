#pragma once

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Counts the input events that reach the app, to tell in the log whether a controller that "froze" stopped producing
 * events (TV, SDL or the pad) or the events stopped at a later stage (app, ihslib, host). Main thread only: the
 * counters are fed from the SDL event loop and read by the stats timer there. Nothing is logged per event.
 */

typedef struct input_stats_t {
    unsigned pad_buttons, pad_axes, keys, controllers_added, controllers_removed, joystick_lost;
    /** SDL_GetTicks() of the last gamepad button or axis event; 0 if none yet. */
    Uint32 last_pad_ticks;
} input_stats_t;

void input_stats_record(const SDL_Event *event);

/** Copy of the counters. */
void input_stats_get(input_stats_t *out);

/** Whether the key is one of the TV remote's coloured keys, used to put a mark in the log. */
bool input_stats_is_mark_key(const SDL_KeyboardEvent *key);
