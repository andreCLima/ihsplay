#pragma once

#include <stdint.h>
#include "ihslib.h"

typedef struct stream_media_session_t stream_media_session_t;
typedef struct stream_manager_t stream_manager_t;

stream_media_session_t *stream_media_create(stream_manager_t *manager);

void stream_media_destroy(stream_media_session_t *media);

void stream_media_set_viewport_size(stream_media_session_t *media_session, int width, int height);

void stream_media_set_overlay_height(stream_media_session_t *media_session, int height);

void stream_media_set_overlay_shown(stream_media_session_t *media_session, bool overlay);
bool stream_media_supports_hevc(stream_media_session_t *media_session);

const IHS_StreamAudioCallbacks *stream_media_audio_callbacks();

const IHS_StreamVideoCallbacks *stream_media_video_callbacks();

typedef struct stream_media_stats_t {
    /** Running totals since the media was created; take differences. Wrap around at 2^32. */
    uint32_t video_frames, video_bytes, video_dropped, audio_packets, audio_lost_frames;
} stream_media_stats_t;

void stream_media_get_stats(stream_media_session_t *media_session, stream_media_stats_t *stats);
