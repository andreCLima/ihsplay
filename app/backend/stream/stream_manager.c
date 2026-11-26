#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stream_manager.h"
#include "stream_manager_internal.h"

#include "app.h"
#include "ui/app_ui.h"
#include "util/listeners_list.h"

#include "ihslib/hid/sdl.h"

#include "ss4s.h"

#include "stream_media.h"
#include "stream_input.h"

#include "backend/input_manager.h"
#include "logging.h"

static void session_initialized(IHS_Session *session, void *context);

static void session_finalized(IHS_Session *session, void *context);

static void session_configuring(IHS_Session *session, IHS_SessionConfig *config, void *context);

static void session_connected(IHS_Session *session, void *context);

static void session_disconnected(IHS_Session *session, void *context);

static void session_show_cursor(IHS_Session *session, float x, float y, void *context);

// Main thread callbacks

static void session_connected_main(app_t *app, void *context);

static void session_disconnected_main(app_t *app, void *context);

static void session_show_cursor_main(app_t *app, void *context);

static void destroy_session_main(app_t *app, void *context);

static void destroy_session(stream_manager_t *manager, IHS_Session *session, stream_media_session_t *media);

static void controller_back_pressed(stream_manager_t *manager);

static void controller_back_released(stream_manager_t *manager);

static Uint32 back_timer_callback(Uint32 duration, void *param);

static void back_timer_progress_main(app_t *app, void *context);

static void back_timer_finish_main(app_t *app, void *context);

static void grab_mouse(stream_manager_t *manager, bool grab);

#define BACK_COUNTER_MAX 100

// Bits of event_context_t.value1 for a disconnect
#define DISCONNECT_REQUESTED 1u
#define DISCONNECT_TIMED_OUT 2u

#define STATS_INTERVAL_MS 5000

static void log_stats(stream_manager_t *manager);

static void stats_timer_cb(lv_timer_t *timer);

typedef struct destroy_context_t {
    IHS_Session *session;
    stream_media_session_t *media;
} destroy_context_t;

typedef struct event_context_t {
    stream_manager_t *manager;
    void *arg1;
    uint32_t value1;
} event_context_t;

static const IHS_StreamSessionCallbacks session_callbacks = {
        .initialized = session_initialized,
        .configuring = session_configuring,
        .connected = session_connected,
        .disconnected = session_disconnected,
        .finalized = session_finalized,
};

static const IHS_StreamInputCallbacks input_callbacks = {
        .showCursor = session_show_cursor,
//        .hideCursor = session_hide_cursor,
//        .setCursor = session_set_cursor,
//        .cursorImage = session_cursor_image,
};

stream_manager_t *stream_manager_create(app_t *app) {
    assert(app->input_manager != NULL);
    stream_manager_t *manager = calloc(1, sizeof(stream_manager_t));
    manager->app = app;
    manager->listeners = listeners_list_create();
    return manager;
}

void stream_manager_destroy(stream_manager_t *manager) {
    if (manager->stats_timer != NULL) {
        lv_timer_del(manager->stats_timer);
        manager->stats_timer = NULL;
    }
    switch (manager->state) {
        case STREAM_MANAGER_STATE_IDLE: {
            break;
        }
        default: {
            destroy_session(manager, manager->session, manager->media);
            break;
        }
    }
    listeners_list_destroy(manager->listeners);
    free(manager);
}

void stream_manager_register_listener(stream_manager_t *manager, const stream_manager_listener_t *listener,
                                      void *context) {
    listeners_list_add(manager->listeners, listener, context);
}

void stream_manager_unregister_listener(stream_manager_t *manager, const stream_manager_listener_t *listener) {
    listeners_list_remove(manager->listeners, listener);
}

bool stream_manager_start_session(stream_manager_t *manager, const IHS_SessionInfo *info) {
    app_assert_main_thread(manager->app);
    if (manager->state != STREAM_MANAGER_STATE_IDLE) {
        return false;
    }
    // Reset all states for last session
    manager->back_counter = 0;
    manager->back_timer = 0;
    manager->overlay_opened = false;
    manager->requested_disconnect = false;

    stream_media_session_t *media = stream_media_create(manager);
    manager->media = media;
    IHS_Session *session = IHS_SessionCreate(&manager->app->client_info.config, info);
    IHS_SessionSetLogFunction(session, app_ihs_log);
    IHS_SessionSetSessionCallbacks(session, &session_callbacks, manager);
    IHS_SessionSetInputCallbacks(session, &input_callbacks, manager);
    IHS_SessionSetAudioCallbacks(session, stream_media_audio_callbacks(), media);
    IHS_SessionSetVideoCallbacks(session, stream_media_video_callbacks(), media);
    IHS_SessionHIDAddProvider(session, input_manager_get_hid_provider(manager->app->input_manager));
    manager->state = STREAM_MANAGER_STATE_CONNECTING;
    commons_log_info("StreamManager", "Change state to CONNECTING");
    manager->session = session;

    stream_media_set_viewport_size(media, manager->viewport_width, manager->viewport_height);
    stream_media_set_overlay_height(media, manager->overlay_height);

    IHS_SessionConnect(session);
    return true;
}

IHS_Session *stream_manager_active_session(const stream_manager_t *manager) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        return NULL;
    }
    return manager->session;
}

void stream_manager_stop_active(stream_manager_t *manager) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        return;
    }
    manager->requested_disconnect = true;
    IHS_SessionDisconnect(manager->session);
}

bool stream_manager_intercept_event(const stream_manager_t *manager, const SDL_Event *event) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        // Ignore events when idle
        return false;
    }
    if (manager->overlay_opened) {
        // No input event should be handled when overlay opened
        return false;
    }
    switch (event->type) {
        // Following events MUST be handled by the app too
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEREMAPPED:
            return false;
    }
    return true;
}

void stream_manager_handle_event(stream_manager_t *manager, const SDL_Event *event) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        return;
    }
    if (manager->overlay_opened) {
        switch (event->type) {
            // Following events be always handled by IHS, even with overlay opened
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
            case SDL_CONTROLLERDEVICEREMAPPED:
                break;
            default:
                return;
        }
    }
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            // Keyboard events
            stream_input_handle_key_event(manager, &event->key);
            break;
        }
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL: {
            stream_input_handle_mouse_event(manager, event);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN: {
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                controller_back_pressed(manager);
            }
            break;
        }
        case SDL_CONTROLLERBUTTONUP: {
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                controller_back_released(manager);
            }
            break;
        }
    }
    if (manager->app->settings->enable_input) {
        IHS_HIDHandleSDLEvent(manager->session, event);
    }
}

void stream_manager_set_viewport_size(stream_manager_t *manager, int width, int height) {
    manager->viewport_width = width;
    manager->viewport_height = height;
    if (manager->media != NULL) {
        stream_media_set_viewport_size(manager->media, width, height);
    }
}

bool stream_manager_is_overlay_opened(const stream_manager_t *manager) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        return false;
    }
    return manager->overlay_opened;
}

void stream_manager_set_overlay_height(stream_manager_t *manager, int height) {
    manager->overlay_height = height;
    if (manager->media != NULL) {
        stream_media_set_overlay_height(manager->media, height);
    }
}

bool stream_manager_set_overlay_opened(stream_manager_t *manager, bool opened) {
    app_assert_main_thread(manager->app);
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        return false;
    }
    manager->overlay_opened = opened;
    stream_media_set_overlay_shown(manager->media, opened);
    if (opened) {
        app_post_event(manager->app, APP_UI_REQUEST_OVERLAY, NULL, NULL);
    } else {
        app_post_event(manager->app, APP_UI_CLOSE_OVERLAY, NULL, NULL);
    }
    grab_mouse(manager, !opened);
    return true;
}

void stream_manager_set_capture_size(stream_manager_t *manager, int width, int height) {
    manager->capture_width = width;
    manager->capture_height = height;
}

bool stream_manager_is_active(const stream_manager_t *manager) {
    return manager->state == STREAM_MANAGER_STATE_STREAMING;
}

static void session_initialized(IHS_Session *session, void *context) {
    stream_manager_t *manager = (stream_manager_t *) context;
    assert (manager->state == STREAM_MANAGER_STATE_CONNECTING);
    IHS_SessionConnect(session);
}

static void session_finalized(IHS_Session *session, void *context) {
    stream_manager_t *manager = (stream_manager_t *) context;
    assert(manager->state == STREAM_MANAGER_STATE_DISCONNECTING);
    assert(manager->session == session);
    // Stay DISCONNECTING: this runs on the session thread, and going IDLE here would let the main thread start a new
    // session (and a new media) before the old ones are destroyed. destroy_session_main, on the main thread, destroys
    // both and only then goes IDLE.
    destroy_context_t *dc = calloc(1, sizeof(destroy_context_t));
    dc->session = session;
    dc->media = manager->media;
    app_run_on_main(manager->app, destroy_session_main, dc);
}

/*
 * Bitrate to ask Steam for. A fixed 20 Mbit/s is also tizen-steam-link's default and the setting proven on this TV:
 * keyframes arrive whole. IHS_SESSION_BITRATE_ADAPTIVE lets Steam lower the bitrate on reported loss and raise it
 * again, but up to 30 Mbit/s, the rate whose keyframes lost a packet (a freeze of seconds while keepPlayingAfterLoss
 * is off), and it has not run on this TV yet. To try it without touching the code, configure with
 * -DSTREAM_BITRATE_KBPS=-2 (the CMake cache option of the same name; -2 is IHS_SESSION_BITRATE_ADAPTIVE). It should
 * become a setting once it has proven itself.
 */
#ifndef STREAM_BITRATE_KBPS
#define STREAM_BITRATE_KBPS 20000
#endif

static void session_configuring(IHS_Session *session, IHS_SessionConfig *config, void *context) {
    (void) session;
    stream_manager_t *manager = (stream_manager_t *) context;
    assert (manager->media != NULL);
    config->enableHevc = stream_media_supports_hevc(manager->media);
    // Same defaults as tizen-steam-link. FEC rebuilds one lost packet per video message instead of costing a
    // keyframe, and 20 Mbit/s (Moonlight's default for 1080p60) keeps keyframes small enough to arrive whole on
    // Wi-Fi; the old fixed 30 Mbit/s lost a packet of almost every keyframe.
    config->enableFec = true;
    config->quality = 2;
    config->bitrateKbps = STREAM_BITRATE_KBPS;
    config->maxWidth = 1920;
    config->maxHeight = 1080;
    config->maxFramerate = 60;
    config->keepPlayingAfterLoss = false;
    commons_log_info("StreamManager", "Session config: hevc=%d fec=%d quality=%d bitrate=%d kbps max=%dx%d@%d fps"
                                      " keepPlaying=%d",
                     config->enableHevc, config->enableFec, config->quality, config->bitrateKbps, config->maxWidth,
                     config->maxHeight, config->maxFramerate, config->keepPlayingAfterLoss);
}

static void session_connected(IHS_Session *session, void *context) {
    stream_manager_t *manager = (stream_manager_t *) context;
    assert(manager->state == STREAM_MANAGER_STATE_CONNECTING);
    assert(manager->session == session);
    manager->state = STREAM_MANAGER_STATE_STREAMING;
    commons_log_info("StreamManager", "Change state to STREAMING");
    event_context_t ec = {
            .manager = manager,
            .arg1 = (void *) IHS_SessionGetInfo(session),
    };
    app_run_on_main_sync(manager->app, session_connected_main, &ec);
    if (manager->app->settings->enable_input) {
        IHS_SessionHIDNotifyDeviceChange(session);
    }
}

static void session_disconnected(IHS_Session *session, void *context) {
    stream_manager_t *manager = (stream_manager_t *) context;
    assert(manager->state != STREAM_MANAGER_STATE_DISCONNECTING);
    assert(manager->session == session);
    if (manager->back_timer != 0) {
        SDL_RemoveTimer(manager->back_timer);
        manager->back_timer = 0;
    }
    bool requested = manager->requested_disconnect;
    // ihslib reports no reason, but it flags the one case that matters here: the host went silent.
    IHS_SessionStreamStats stats = {0};
    IHS_SessionGetStreamStats(session, &stats);
    manager->state = STREAM_MANAGER_STATE_DISCONNECTING;
    commons_log_info("StreamManager", "Change state to DISCONNECTING (requested=%d, timedOut=%d)", requested,
                     stats.timedOut);
    event_context_t ec = {
            .manager = manager,
            .arg1 = (void *) IHS_SessionGetInfo(session),
            .value1 = (requested ? DISCONNECT_REQUESTED : 0) | (stats.timedOut ? DISCONNECT_TIMED_OUT : 0)
    };
    app_run_on_main_sync(manager->app, session_disconnected_main, &ec);
}

static void session_show_cursor(IHS_Session *session, float x, float y, void *context) {
    (void) session;
    stream_manager_t *manager = (stream_manager_t *) context;
    if (manager->capture_width <= 0 && manager->capture_height <= 0 || manager->viewport_width <= 0 ||
        manager->viewport_height <= 0) {
        return;
    }
    if (stream_manager_is_overlay_opened(manager)) {
        return;
    }
    float scale = SDL_min((float) manager->viewport_width / manager->capture_width,
                          (float) manager->viewport_height / manager->capture_height);
    float dst_width = (float) manager->capture_width * scale, dst_height = (float) manager->capture_height * scale;
    SDL_Point *point = calloc(1, sizeof(SDL_Point));
    point->x = (int) (((float) manager->viewport_width - dst_width) / 2.0f + dst_width * x);
    point->y = (int) (((float) manager->viewport_height - dst_height) / 2.0f + dst_height * y);
    app_run_on_main(manager->app, session_show_cursor_main, point);
}

static void session_connected_main(app_t *app, void *context) {
    (void) app;
    event_context_t *ec = context;
    stream_manager_t *manager = ec->manager;
    listeners_list_notify(manager->listeners, stream_manager_listener_t, connected, (const IHS_SessionInfo *) ec->arg1);
    memset(&manager->stats_prev, 0, sizeof(manager->stats_prev));
    input_stats_get(&manager->input_prev);
    memset(&manager->media_stats_prev, 0, sizeof(manager->media_stats_prev));
    manager->stats_prev_ticks = SDL_GetTicks();
    if (manager->stats_timer == NULL) {
        manager->stats_timer = lv_timer_create(stats_timer_cb, STATS_INTERVAL_MS, manager);
    }
    grab_mouse(manager, true);
}

static void session_disconnected_main(app_t *app, void *context) {
    (void) app;
    event_context_t *ec = context;
    stream_manager_t *manager = ec->manager;
    if (manager->stats_timer != NULL) {
        lv_timer_del(manager->stats_timer);
        manager->stats_timer = NULL;
    }
    grab_mouse(manager, false);
    listeners_list_notify(manager->listeners, stream_manager_listener_t, disconnected,
                          (const IHS_SessionInfo *) ec->arg1, (ec->value1 & DISCONNECT_REQUESTED) != 0,
                          (ec->value1 & DISCONNECT_TIMED_OUT) != 0);
}

static void session_show_cursor_main(app_t *app, void *context) {
    stream_manager_t *manager = app->stream_manager;
    SDL_Point *point = context;
    input_manager_ignore_next_mouse_movement(manager->app->input_manager);
//    SDL_WarpMouseInWindow(app->ui->window, point->x, point->y);
    free(context);
}

static void destroy_session_main(app_t *app, void *context) {
    destroy_context_t *dc = context;
    stream_manager_t *manager = app->stream_manager;
    destroy_session(manager, dc->session, dc->media);
    free(dc);
    manager->state = STREAM_MANAGER_STATE_IDLE;
    commons_log_info("StreamManager", "Change state to IDLE");
}

static void destroy_session(stream_manager_t *manager, IHS_Session *session, stream_media_session_t *media) {
    IHS_SessionThreadedJoin(session);
    IHS_SessionDestroy(session);
    if (media != NULL) {
        stream_media_destroy(media);
    }
    manager->media = NULL;
}

static void controller_back_pressed(stream_manager_t *manager) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING || manager->back_timer != 0) {
        return;
    }
    manager->back_counter = 0;
    manager->back_timer = SDL_AddTimer(300, back_timer_callback, manager);
}

static void controller_back_released(stream_manager_t *manager) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING || manager->back_timer == 0) {
        return;
    }
    SDL_RemoveTimer(manager->back_timer);
    manager->back_timer = 0;
    commons_log_info("Streaming", "Overlay timer cancelled");
    // This function is expected to be called in main thread.
    app_assert_main_thread(manager->app);
    listeners_list_notify(manager->listeners, stream_manager_listener_t, overlay_progress_finished, false);
}

static void grab_mouse(stream_manager_t *manager, bool grab) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        return;
    }
    SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
}


static Uint32 back_timer_callback(Uint32 duration, void *param) {
    (void) duration;
    stream_manager_t *manager = (stream_manager_t *) param;
    if (manager->state != STREAM_MANAGER_STATE_STREAMING) {
        return 0;
    }
    manager->back_counter += 1;
    if (manager->back_counter >= BACK_COUNTER_MAX) {
        manager->back_timer = 0;
        manager->back_counter = 0;
        IHS_HIDResetSDLGameControllers(manager->session);
        commons_log_info("Streaming", "Requesting overlay");
        app_run_on_main(manager->app, back_timer_finish_main, manager);
        return 0;
    } else {
        app_run_on_main(manager->app, back_timer_progress_main, manager);
    }
    return 16;
}

static void back_timer_progress_main(app_t *app, void *context) {
    (void) app;
    stream_manager_t *manager = (stream_manager_t *) context;
    listeners_list_notify(manager->listeners, stream_manager_listener_t, overlay_progress, manager->back_counter);
}

static void back_timer_finish_main(app_t *app, void *context) {
    (void) app;
    stream_manager_t *manager = (stream_manager_t *) context;
    stream_manager_set_overlay_opened(manager, true);
    listeners_list_notify(manager->listeners, stream_manager_listener_t, overlay_progress_finished, true);
}

/* One short line every few seconds: enough to see in the log when and how a stream degraded. Totals come from ihslib
 * and from the media callbacks; the rates are differences since the previous line. Runs on the main thread, between
 * connected_main and disconnected_main, so the session and media are alive. */
static void log_stats(stream_manager_t *manager) {
    if (manager->state != STREAM_MANAGER_STATE_STREAMING || manager->session == NULL || manager->media == NULL) {
        return;
    }
    IHS_SessionStreamStats s;
    memset(&s, 0, sizeof(s));
    IHS_SessionGetStreamStats(manager->session, &s);
    stream_media_stats_t m;
    stream_media_get_stats(manager->media, &m);
    const stream_media_stats_t *pm = &manager->media_stats_prev;
    const IHS_SessionStreamStats *ps = &manager->stats_prev;
    // Rates over the time that really passed: a user mark logs between two periodic lines.
    const uint32_t now_ticks = SDL_GetTicks();
    uint32_t elapsed_ms = now_ticks - manager->stats_prev_ticks;
    if (elapsed_ms == 0) {
        elapsed_ms = 1;
    }
    manager->stats_prev_ticks = now_ticks;
    commons_log_info("STATS", "fps=%u kbps=%u dropped=%u | pkts=%u lost=%u(+%u) lostRep=%u(+%u) fec=%d rec=%u unrec=%u"
                              " | rtt=%.0fms | audio=%u lost=%u | rexmit=%u(+%u) gaveUp=%u maxDelay=%ums"
                              " ackCum=%u(+%u) | hidFull=%u(+%u) hidSlow=%u",
                     (uint32_t) ((uint64_t) (m.video_frames - pm->video_frames) * 1000 / elapsed_ms),
                     (uint32_t) ((uint64_t) (m.video_bytes - pm->video_bytes) * 8 / elapsed_ms),
                     m.video_dropped - pm->video_dropped, s.videoPackets, s.videoPacketsLost,
                     s.videoPacketsLost - ps->videoPacketsLost, s.lostReports, s.lostReports - ps->lostReports,
                     s.fec, s.fecRecovered, s.fecUnrecoverable, (double) s.roundTripMs,
                     m.audio_packets - pm->audio_packets, m.audio_lost_frames - pm->audio_lost_frames,
                     s.retransmissions, s.retransmissions - ps->retransmissions, s.retransmitGaveUp,
                     s.retransmitMaxDelayMs, s.ackCumulativeCancels,
                     s.ackCumulativeCancels - ps->ackCumulativeCancels, s.hidFullReportRequests,
                     s.hidFullReportRequests - ps->hidFullReportRequests, s.hidSlowJobs);
    manager->media_stats_prev = m;

    // What reached the app from the controller, to compare with what the stream then sent to the host.
    input_stats_t in;
    input_stats_get(&in);
    const input_stats_t *pi = &manager->input_prev;
    int last_pad_ms = in.last_pad_ticks == 0 ? -1 : (int) (SDL_GetTicks() - in.last_pad_ticks);
    commons_log_info("INPUT", "pad buttons=%u(+%u) axes=%u(+%u) lastPadEvent=%dms ago | pads=%u added=%u removed=%u"
                              " joyLost=%u | keys=%u(+%u) | hid gen=+%u sent=+%u lockWait=%ums stalls=%u"
                              " | steamInput=%u flips=%u dropped=%u | sendQ depth=%u wait=%ums ctrlSent=+%u timerLag=%ums"
                              " | ctrlAck n=%u avg=%ums max=%ums oldest=%ums stalls=%u",
                     in.pad_buttons, in.pad_buttons - pi->pad_buttons, in.pad_axes, in.pad_axes - pi->pad_axes,
                     last_pad_ms, (unsigned) input_manager_sdl_gamepad_count(manager->app->input_manager),
                     in.controllers_added, in.controllers_removed, in.joystick_lost, in.keys, in.keys - pi->keys,
                     s.hidReportsGenerated - ps->hidReportsGenerated, s.hidReportsSent - ps->hidReportsSent,
                     s.hidUseLockMaxWaitMs, s.hidInputStalls, s.inputEnabled, s.inputTransitions,
                     s.inputDisabledDrops, s.sendQueueDepthMax, s.sendQueueLatencyMaxMs,
                     s.controlPacketsSent - ps->controlPacketsSent, s.timerLagMaxMs, s.controlAckCount,
                     s.controlAckAvgMs, s.controlAckMaxMs, s.controlOldestPendingMs, s.controlAckStalls);
    manager->input_prev = in;
    // Only now: the INPUT line above takes its differences from the previous stats too.
    manager->stats_prev = s;
}

static void stats_timer_cb(lv_timer_t *timer) {
    log_stats(timer->user_data);
}

static void toast_timer_cb(lv_timer_t *timer) {
    lv_obj_t *label = timer->user_data;
    // The timer was created with a repeat count of 1, so LVGL deletes it after this call
    (void) timer;
    lv_obj_del(label);
}

void stream_manager_user_mark(stream_manager_t *manager) {
    app_assert_main_thread(manager->app);
    int n = ++manager->mark_count;
    // Warn: it must stand out, be flushed at once, and never be folded or dropped by the log limiters
    commons_log_warn("MARK", "=== USER MARK %d ===", n);
    log_stats(manager);
    char text[48];
    snprintf(text, sizeof(text), "Mark %d saved in the log", n);
    lv_obj_t *label = lv_label_create(lv_layer_top());
    lv_label_set_text(label, text);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_pad_all(label, LV_DPX(10), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, LV_DPX(20));
    lv_timer_t *timer = lv_timer_create(toast_timer_cb, 2000, label);
    lv_timer_set_repeat_count(timer, 1);
}
