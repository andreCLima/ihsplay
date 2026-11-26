#include "input_stats.h"

static input_stats_t stats;

void input_stats_record(const SDL_Event *event) {
    switch (event->type) {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            stats.pad_buttons++;
            stats.last_pad_ticks = SDL_GetTicks() | 1;
            break;
        case SDL_CONTROLLERAXISMOTION:
            stats.pad_axes++;
            stats.last_pad_ticks = SDL_GetTicks() | 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            stats.keys++;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            stats.controllers_added++;
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            stats.controllers_removed++;
            break;
        case SDL_JOYDEVICEREMOVED:
            // The raw joystick went away; a game controller on top of it reports its own removal as well.
            stats.joystick_lost++;
            break;
        default:
            break;
    }
}

void input_stats_get(input_stats_t *out) {
    *out = stats;
}

bool input_stats_is_mark_key(const SDL_KeyboardEvent *key) {
    // SDL-webOS scancodes SDL_SCANCODE_WEBOS_RED..BLUE (486..489). Numbers rather than the macros, which exist only
    // in the webOS SDL headers. The stream never uses them: the app forwards no coloured key to the host.
    int scancode = (int) key->keysym.scancode;
    return scancode >= 486 && scancode <= 489;
}
