#include <stdlib.h>
#include "stream_media.h"

#include "ss4s.h"
#include "stream_manager.h"
#include "app.h"
#include "logging.h"
#include "util/video/sps/include/sps_util.h"

#include <opus_multistream.h>
#include <SDL2/SDL.h>

struct stream_media_session_t {
    stream_manager_t *manager;
    SDL_mutex *lock;
    SS4S_Player *player;
    SS4S_VideoCapabilities video_cap;

    SS4S_VideoInfo video_info;
    /* Guards the audio fields below. Audio callbacks run on the receive thread while stop can come from the
     * session teardown, so a submit must never touch a decoder or buffer that stop is freeing. */
    SDL_mutex *audio_lock;
    OpusMSDecoder *opus_decoder;
    size_t pcm_unit_size;
    int16_t *pcm_buffer;
    /* Samples per channel of the last good packet: the size a concealed (lost) frame should have. */
    int last_frame_size;

    int pcm_buffer_size;
    int viewport_width, viewport_height;
    int overlay_height;
};

static int audio_start(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context);

static void audio_stop(IHS_Session *session, void *context);

static void audio_lost(IHS_Session *session, uint32_t frames, void *context);

static int audio_submit(IHS_Session *session, IHS_Buffer *data, void *context);

static int video_start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context);

static void video_stop(IHS_Session *session, void *context);

static IHS_StreamVideoSubmitResult video_submit(IHS_Session *session, IHS_Buffer *data,
                                                IHS_StreamVideoFrameFlag flags, void *context);

static int video_set_capture_size(IHS_Session *session, int width, int height, void *context);

static const IHS_StreamAudioCallbacks audio_callbacks = {
        .start = audio_start,
        .stop = audio_stop,
        .submit = audio_submit,
        .lost = audio_lost,
};

static const IHS_StreamVideoCallbacks video_callbacks = {
        .start = video_start,
        .stop = video_stop,
        .submit = video_submit,
        .setCaptureSize = video_set_capture_size,
};

stream_media_session_t *stream_media_create(stream_manager_t *manager) {
    stream_media_session_t *media_session = calloc(1, sizeof(stream_media_session_t));
    media_session->manager = manager;
    media_session->lock = SDL_CreateMutex();
    media_session->audio_lock = SDL_CreateMutex();
    media_session->player = SS4S_PlayerOpen();
    SS4S_PlayerSetWaitAudioVideoReady(media_session->player, true);

    SS4S_GetVideoCapabilities(&media_session->video_cap);
    return media_session;
}

void stream_media_destroy(stream_media_session_t *media_session) {
    // In case audio_stop never ran
    if (media_session->opus_decoder != NULL) {
        opus_multistream_decoder_destroy(media_session->opus_decoder);
    }
    free(media_session->pcm_buffer);
    SS4S_PlayerClose(media_session->player);
    SDL_DestroyMutex(media_session->lock);
    SDL_DestroyMutex(media_session->audio_lock);
    free(media_session);
}

void stream_media_set_viewport_size(stream_media_session_t *media_session, int width, int height) {
    SDL_LockMutex(media_session->lock);
    media_session->viewport_width = width;
    media_session->viewport_height = height;
    SDL_UnlockMutex(media_session->lock);
}

void stream_media_set_overlay_height(stream_media_session_t *media_session, int height) {
    SDL_LockMutex(media_session->lock);
    media_session->overlay_height = height;
    SDL_UnlockMutex(media_session->lock);
}

void stream_media_set_overlay_shown(stream_media_session_t *media_session, bool overlay) {
    if (media_session->video_cap.transform & SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING) {
        return;
    }
    if (overlay) {
        SDL_LockMutex(media_session->lock);
        int ui_width = media_session->viewport_width, ui_height = media_session->viewport_height,
                overlay_height = media_session->overlay_height;
        if (ui_width <= 0 || ui_height <= 0 || overlay_height <= 0) {
            SDL_UnlockMutex(media_session->lock);
            return;
        }
        SS4S_VideoRect src = {
                .x = 0, .y = 0,
                .width = media_session->video_info.width,
                .height = media_session->video_info.height * (ui_height - overlay_height) / ui_height
        };
        SDL_UnlockMutex(media_session->lock);

        SS4S_VideoRect dest = {0, 0, ui_width, ui_height - overlay_height};
        SS4S_PlayerVideoSetDisplayArea(media_session->player, &src, &dest);
    } else {
        SS4S_PlayerVideoSetDisplayArea(media_session->player, NULL, NULL);
    }
}

bool stream_media_supports_hevc(stream_media_session_t *media_session) {
    return media_session->video_cap.codecs & SS4S_VIDEO_H265;
}

const IHS_StreamAudioCallbacks *stream_media_audio_callbacks() {
    return &audio_callbacks;
}

const IHS_StreamVideoCallbacks *stream_media_video_callbacks() {
    return &video_callbacks;
}

static int audio_start(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context) {
    (void) session;
    commons_log_info("Media", "Audio start. codec=%u, channels=%u, sampleRate=%u", config->codec,
                     config->channels, config->frequency);
    if (config->codec != IHS_StreamAudioCodecOpus || config->channels < 1 || config->channels > 2) {
        return -1;
    }
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    int rc = OPUS_OK;
    /* One stream: two channels are a coupled (stereo) pair, one channel is a plain mono stream. */
    unsigned char mapping[2] = {0, 1};
    const int coupled = config->channels == 2 ? 1 : 0;
    const int samples_per_frame = 240;

    SDL_LockMutex(media_session->audio_lock);
    if (media_session->opus_decoder != NULL) {
        opus_multistream_decoder_destroy(media_session->opus_decoder);
        media_session->opus_decoder = NULL;
    }
    free(media_session->pcm_buffer);
    media_session->pcm_buffer = NULL;

    media_session->pcm_buffer_size = samples_per_frame * 64;
    media_session->pcm_unit_size = config->channels * sizeof(int16_t);
    media_session->last_frame_size = (int) (config->frequency / 100); // 10 ms until a packet says otherwise
    media_session->opus_decoder = opus_multistream_decoder_create((opus_int32) config->frequency,
                                                                  (int) config->channels, 1, coupled, mapping, &rc);
    media_session->pcm_buffer = calloc(media_session->pcm_unit_size, media_session->pcm_buffer_size);
    if (media_session->opus_decoder == NULL || rc != OPUS_OK || media_session->pcm_buffer == NULL) {
        commons_log_error("Media", "Can't create Opus decoder: %s", opus_strerror(rc));
        if (media_session->opus_decoder != NULL) {
            opus_multistream_decoder_destroy(media_session->opus_decoder);
            media_session->opus_decoder = NULL;
        }
        free(media_session->pcm_buffer);
        media_session->pcm_buffer = NULL;
        SDL_UnlockMutex(media_session->audio_lock);
        return -1;
    }
    SDL_UnlockMutex(media_session->audio_lock);

    SS4S_AudioInfo info = {
            .codec = SS4S_AUDIO_PCM_S16LE,
            .numOfChannels = (int) config->channels,
            .sampleRate = (int) config->frequency,
            .samplesPerFrame = samples_per_frame,
            .appName = "IHSplay",
            .streamName = "Streaming",
    };
    int open_result = SS4S_PlayerAudioOpen(media_session->player, &info);
    if (open_result != SS4S_AUDIO_OPEN_OK) {
        // The session may not call stop for a start that failed; audio_stop is safe to run twice.
        audio_stop(session, context);
    }
    return open_result;
}

/* Most lost frames in a row to conceal. Past a few, a gap sounds better than repeated guesses. */
#define AUDIO_MAX_CONCEALED_FRAMES 8

static void audio_stop(IHS_Session *session, void *context) {
    (void) session;
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    /* Take the lock first: a submit in flight finishes before the decoder and buffer go away, and any later
     * one sees the NULLs and returns. */
    SDL_LockMutex(media_session->audio_lock);
    SS4S_PlayerAudioClose(media_session->player);
    if (media_session->opus_decoder != NULL) {
        opus_multistream_decoder_destroy(media_session->opus_decoder);
        media_session->opus_decoder = NULL;
    }
    free(media_session->pcm_buffer);
    media_session->pcm_buffer = NULL;
    SDL_UnlockMutex(media_session->audio_lock);
}

static int audio_submit(IHS_Session *session, IHS_Buffer *data, void *context) {
    (void) session;
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    SDL_LockMutex(media_session->audio_lock);
    if (media_session->opus_decoder == NULL || media_session->pcm_buffer == NULL) {
        SDL_UnlockMutex(media_session->audio_lock);
        return 0;
    }
    int decode_len = opus_multistream_decode(media_session->opus_decoder, data->data + data->offset, data->size,
                                             media_session->pcm_buffer, media_session->pcm_buffer_size, 0);
    if (decode_len <= 0) {
        /* A negative value is an Opus error code, not a length: multiplied by the unit size it would hand the
         * player a gigantic size_t. Drop the packet; the next one decodes on its own. */
        if (decode_len < 0) {
            commons_log_warn("Media", "Opus decode failed: %s", opus_strerror(decode_len));
        }
        SDL_UnlockMutex(media_session->audio_lock);
        return 0;
    }
    media_session->last_frame_size = decode_len;
    int result = SS4S_PlayerAudioFeed(media_session->player, (const unsigned char *) media_session->pcm_buffer,
                                      media_session->pcm_unit_size * decode_len);
    SDL_UnlockMutex(media_session->audio_lock);
    return result;
}

static void audio_lost(IHS_Session *session, uint32_t frames, void *context) {
    (void) session;
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    SDL_LockMutex(media_session->audio_lock);
    if (media_session->opus_decoder == NULL || media_session->pcm_buffer == NULL) {
        SDL_UnlockMutex(media_session->audio_lock);
        return;
    }
    /* Audio isn't resent. Decoding "no data" makes Opus extrapolate the previous frame (packet loss
     * concealment), which keeps the sample clock in step and sounds far better than a hole. */
    for (uint32_t i = 0; i < frames && i < AUDIO_MAX_CONCEALED_FRAMES; i++) {
        int frame_size = media_session->last_frame_size;
        if (frame_size <= 0 || frame_size > media_session->pcm_buffer_size) {
            break;
        }
        int decode_len = opus_multistream_decode(media_session->opus_decoder, NULL, 0, media_session->pcm_buffer,
                                                 frame_size, 0);
        if (decode_len <= 0) {
            break;
        }
        SS4S_PlayerAudioFeed(media_session->player, (const unsigned char *) media_session->pcm_buffer,
                             media_session->pcm_unit_size * decode_len);
    }
    SDL_UnlockMutex(media_session->audio_lock);
}

static int video_start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context) {
    (void) session;
    SS4S_VideoCodec codec;
    switch (config->codec) {
        case IHS_StreamVideoCodecH264:
            codec = SS4S_VIDEO_H264;
            break;
        case IHS_StreamVideoCodecHEVC:
            codec = SS4S_VIDEO_H265;
            break;
        default:
            return -1;
    }
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    SDL_LockMutex(media_session->lock);
    commons_log_info("Media", "Video start. codec=%u, width=%u, height=%u", config->codec, config->width,
                     config->height);
    SS4S_VideoInfo info = {
            .codec = codec,
            .width = (int) config->width,
            .height = (int) config->height,
    };
    media_session->video_info = info;
    SDL_UnlockMutex(media_session->lock);
    return SS4S_PlayerVideoOpen(media_session->player, &info);
}

static void video_stop(IHS_Session *session, void *context) {
    (void) session;
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    SS4S_PlayerVideoClose(media_session->player);
}

static IHS_StreamVideoSubmitResult video_submit(IHS_Session *session, IHS_Buffer *data,
                                                IHS_StreamVideoFrameFlag flags, void *context) {
    (void) session;
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    SS4S_VideoFeedFlags sflgs = 0;
    if (flags & IHS_StreamVideoFrameKeyFrame) {
        SDL_LockMutex(media_session->lock);
        sflgs = SS4S_VIDEO_FEED_DATA_KEYFRAME;
        sps_dimension_t dimension = {0, 0};
        bool dimension_parsed = false;
        switch (media_session->video_info.codec) {
            case SS4S_VIDEO_H264: {
                dimension_parsed = sps_util_parse_dimension_h264(IHS_BufferPointer(data), data->size, &dimension);
                break;
            }
            case SS4S_VIDEO_H265: {
                dimension_parsed = sps_util_parse_dimension_hevc(IHS_BufferPointer(data), data->size, &dimension);
                break;
            }
            default: {
                commons_log_fatal("Media", "Unexpected video codec %s!!",
                                  SS4S_VideoCodecName(media_session->video_info.codec));
                abort();
            }
        }
        if (!dimension_parsed) {
            commons_log_warn("Media", "Can't parse NAL Unit.");
            commons_log_hexdump(COMMONS_LOG_LEVEL_WARN, "Media", IHS_BufferPointer(data), data->size);
        }
        if (dimension_parsed && (dimension.width != media_session->video_info.width ||
                                 dimension.height != media_session->video_info.height)) {
            commons_log_info("Media", "Size change detected by NAL header. (%d*%d)=>(%d*%d)",
                             media_session->video_info.width, media_session->video_info.height, dimension.width,
                             dimension.height);
            media_session->video_info.width = dimension.width;
            media_session->video_info.height = dimension.height;
            SS4S_PlayerVideoSizeChanged(media_session->player, dimension.width, dimension.height);
        }
        SDL_UnlockMutex(media_session->lock);
    }
    SS4S_VideoFeedResult result = SS4S_PlayerVideoFeed(media_session->player, data->data + data->offset, data->size,
                                                       sflgs);
    switch (result) {
        case SS4S_VIDEO_FEED_OK:
            return IHS_StreamVideoSubmitOK;
        case SS4S_VIDEO_FEED_ERROR:
            // ihslib ends the session on IHS_StreamVideoSubmitError; a frame the TV's decoder refused is worth a
            // keyframe, not the whole stream.
            commons_log_warn("Media", "Video feed failed, requesting a keyframe");
            return IHS_StreamVideoSubmitReportLost;
        default:
            // Not ready, or the decoder asks for a keyframe: either way this frame never reached the screen.
            return IHS_StreamVideoSubmitReportLost;
    }
}

static int video_set_capture_size(IHS_Session *session, int width, int height, void *context) {
    (void) session;
    stream_media_session_t *media_session = (stream_media_session_t *) context;
    stream_manager_set_capture_size(media_session->manager, width, height);
    return 0;
}