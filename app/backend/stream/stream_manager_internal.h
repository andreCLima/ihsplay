#pragma once

#include "stream_manager.h"
#include "stream_media.h"

#include "array_list.h"
#include "lvgl.h"
#include "util/input_stats.h"

typedef enum stream_manager_state_t {
    STREAM_MANAGER_STATE_IDLE,
    STREAM_MANAGER_STATE_CONNECTING,
    STREAM_MANAGER_STATE_STREAMING,
    STREAM_MANAGER_STATE_DISCONNECTING,
} stream_manager_state_t;

struct stream_manager_t {
    app_t *app;
    array_list_t *listeners;

    stream_manager_state_t state;

    stream_media_session_t *media;
    IHS_Session *session;
    SDL_TimerID back_timer;
    // Periodic STATS line in the log, and the totals it last saw (all on the main thread)
    lv_timer_t *stats_timer;
    IHS_SessionStreamStats stats_prev;
    input_stats_t input_prev;
    int mark_count;
    stream_media_stats_t media_stats_prev;
    /* SDL_GetTicks() of the previous stats line: a user mark logs in between, so intervals vary */
    uint32_t stats_prev_ticks;
    int back_counter;
    bool overlay_opened;
    bool requested_disconnect;

    int viewport_width, viewport_height;
    int capture_width, capture_height;
    int overlay_height;
};
