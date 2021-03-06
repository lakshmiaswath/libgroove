/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "player.h"
#include <groove/queue.h>

#include <libavutil/mem.h>
#include <libavutil/log.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <pthread.h>

struct GroovePlayerPrivate {
    struct GroovePlayer externals;
    struct GrooveBuffer *audio_buf;
    size_t audio_buf_size; // in bytes
    size_t audio_buf_index; // in bytes

    // this mutex applies to the variables in this block
    pthread_mutex_t play_head_mutex;
    char play_head_mutex_inited;
    // pointer to current item where the buffered audio is reaching the device
    struct GroovePlaylistItem *play_head;
    // number of seconds into the play_head song where the buffered audio
    // is reaching the device
    double play_pos;

    SDL_AudioDeviceID device_id;
    struct GrooveSink *sink;

    struct GrooveQueue *eventq;
};

static Uint16 groove_fmt_to_sdl_fmt(enum GrooveSampleFormat fmt) {
    switch (fmt) {
        case GROOVE_SAMPLE_FMT_U8:
            return AUDIO_U8;
        case GROOVE_SAMPLE_FMT_S16:
            return AUDIO_S16SYS;
        case GROOVE_SAMPLE_FMT_S32:
            return AUDIO_S32SYS;
        case GROOVE_SAMPLE_FMT_FLT:
            return AUDIO_F32SYS;
        default:
            av_log(NULL, AV_LOG_ERROR,
                "unable to use selected format. using GROOVE_SAMPLE_FMT_S16 instead.\n");
            return AUDIO_S16SYS;
    }
}

static enum GrooveSampleFormat sdl_fmt_to_groove_fmt(Uint16 sdl_format) {
    switch (sdl_format) {
        case AUDIO_U8:
            return GROOVE_SAMPLE_FMT_U8;
        case AUDIO_S16SYS:
            return GROOVE_SAMPLE_FMT_S16;
        case AUDIO_S32SYS:
            return GROOVE_SAMPLE_FMT_S32;
        case AUDIO_F32SYS:
            return GROOVE_SAMPLE_FMT_FLT;
        default:
            return GROOVE_SAMPLE_FMT_NONE;
    }
}

static void emit_event(struct GrooveQueue *queue, enum GroovePlayerEventType type) {
    union GroovePlayerEvent *evt = av_malloc(sizeof(union GroovePlayerEvent));
    if (!evt) {
        av_log(NULL, AV_LOG_ERROR, "unable to create event: out of memory\n");
        return;
    }
    evt->type = type;
    if (groove_queue_put(queue, evt) < 0)
        av_log(NULL, AV_LOG_ERROR, "unable to put event on queue: out of memory\n");
}


static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    struct GroovePlayerPrivate *p = opaque;

    struct GrooveSink *sink = p->sink;
    struct GroovePlaylist *playlist = sink->playlist;

    double bytes_per_sec = sink->bytes_per_sec;
    int paused = !groove_playlist_playing(playlist);

    pthread_mutex_lock(&p->play_head_mutex);

    while (len > 0) {
        if (!paused && p->audio_buf_index >= p->audio_buf_size) {
            groove_buffer_unref(p->audio_buf);
            p->audio_buf_index = 0;
            p->audio_buf_size = 0;

            int ret = groove_sink_buffer_get(p->sink, &p->audio_buf, 0);
            if (ret == GROOVE_BUFFER_END) {
                emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->play_head = NULL;
                p->play_pos = -1.0;
            } else if (ret == GROOVE_BUFFER_YES) {
                if (p->play_head != p->audio_buf->item)
                    emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);

                p->play_head = p->audio_buf->item;
                p->play_pos = p->audio_buf->pos;
                p->audio_buf_size = p->audio_buf->size;
            } else {
                // errors are treated the same as no buffer ready
                emit_event(p->eventq, GROOVE_EVENT_BUFFERUNDERRUN);
            }
        }
        if (paused || !p->audio_buf) {
            // fill with silence
            memset(stream, 0, len);
            break;
        }
        size_t len1 = p->audio_buf_size - p->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, p->audio_buf->data[0] + p->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        p->audio_buf_index += len1;
        p->play_pos += len1 / bytes_per_sec;
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    if (p->play_head == item) {
        p->play_head = NULL;
        p->play_pos = -1.0;
        groove_buffer_unref(p->audio_buf);
        p->audio_buf = NULL;
        p->audio_buf_index = 0;
        p->audio_buf_size = 0;
        emit_event(p->eventq, GROOVE_EVENT_NOWPLAYING);
    }

    pthread_mutex_unlock(&p->play_head_mutex);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GroovePlayerPrivate *p = sink->userdata;

    pthread_mutex_lock(&p->play_head_mutex);

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;
    p->audio_buf_index = 0;
    p->audio_buf_size = 0;

    pthread_mutex_unlock(&p->play_head_mutex);
}

struct GroovePlayer *groove_player_create(void) {
    struct GroovePlayerPrivate *p = av_mallocz(sizeof(struct GroovePlayerPrivate));

    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "unable to create player: out of memory\n");
        return NULL;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        av_free(p);
        av_log(NULL, AV_LOG_ERROR, "unable to init SDL audio subsystem: %s\n",
                SDL_GetError());
        return NULL;
    }

    struct GroovePlayer *player = &p->externals;

    p->sink = groove_sink_create();
    if (!p->sink) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create sink: out of memory\n");
        return NULL;
    }

    p->sink->userdata = player;
    p->sink->purge = sink_purge;
    p->sink->flush = sink_flush;

    if (pthread_mutex_init(&p->play_head_mutex, NULL) != 0) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create play head mutex: out of memory\n");
        return NULL;
    }
    p->play_head_mutex_inited = 1;

    p->eventq = groove_queue_create();
    if (!p->eventq) {
        groove_player_destroy(player);
        av_log(NULL, AV_LOG_ERROR,"unable to create event queue: out of memory\n");
        return NULL;
    }

    // set some nice defaults
    player->target_audio_format.sample_rate = 44100;
    player->target_audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    player->target_audio_format.sample_fmt = GROOVE_SAMPLE_FMT_S16;
    // small because there is no way to clear the buffer.
    player->device_buffer_size = 1024;
    player->sink_buffer_size = 8192;

    return player;
}

void groove_player_destroy(struct GroovePlayer *player) {
    if (!player)
        return;

    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    if (p->play_head_mutex_inited)
        pthread_mutex_destroy(&p->play_head_mutex);

    if (p->eventq)
        groove_queue_destroy(p->eventq);

    groove_sink_destroy(p->sink);

    av_free(p);
}

int groove_player_attach(struct GroovePlayer *player, struct GroovePlaylist *playlist) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.format = groove_fmt_to_sdl_fmt(player->target_audio_format.sample_fmt);
    wanted_spec.freq = player->target_audio_format.sample_rate;
    wanted_spec.channels = groove_channel_layout_count(player->target_audio_format.channel_layout);
    wanted_spec.samples = player->device_buffer_size;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = player;

    p->device_id = SDL_OpenAudioDevice(player->device_name, 0, &wanted_spec,
            &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (p->device_id == 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to open audio device: %s\n", SDL_GetError());
        return -1;
    }

    // save the actual spec back into the struct
    player->actual_audio_format.sample_rate = spec.freq;
    player->actual_audio_format.channel_layout = groove_channel_layout_default(spec.channels);
    player->actual_audio_format.sample_fmt = sdl_fmt_to_groove_fmt(spec.format);

    // based on spec that we got, attach a sink with those properties
    p->sink->buffer_size = player->sink_buffer_size;
    p->sink->audio_format = player->actual_audio_format;

    if (p->sink->audio_format.sample_fmt == GROOVE_SAMPLE_FMT_NONE) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unsupported audio device sample format\n");
        return -1;
    }

    int err = groove_sink_attach(p->sink, playlist);
    if (err < 0) {
        groove_player_detach(player);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return err;
    }

    p->play_pos = -1.0;

    groove_queue_reset(p->eventq);

    SDL_PauseAudioDevice(p->device_id, 0);

    return 0;
}

int groove_player_detach(struct GroovePlayer *player) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    if (p->eventq) {
        groove_queue_flush(p->eventq);
        groove_queue_abort(p->eventq);
    }
    if (p->sink->playlist) {
        groove_sink_detach(p->sink);
    }
    if (p->device_id > 0) {
        SDL_CloseAudioDevice(p->device_id);
        p->device_id = 0;
    }
    player->playlist = NULL;

    groove_buffer_unref(p->audio_buf);
    p->audio_buf = NULL;

    return 0;
}

int groove_device_count(void) {
    return SDL_GetNumAudioDevices(0);
}

const char * groove_device_name(int index) {
    return SDL_GetAudioDeviceName(index, 0);
}

void groove_player_position(struct GroovePlayer *player,
        struct GroovePlaylistItem **item, double *seconds)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;

    pthread_mutex_lock(&p->play_head_mutex);

    if (item)
        *item = p->play_head;

    if (seconds)
        *seconds = p->play_pos;

    pthread_mutex_unlock(&p->play_head_mutex);
}

int groove_player_event_get(struct GroovePlayer *player,
        union GroovePlayerEvent *event, int block)
{
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    union GroovePlayerEvent *tmp;
    int err = groove_queue_get(p->eventq, (void **)&tmp, block);
    if (err > 0) {
        *event = *tmp;
        av_free(tmp);
    }
    return err;
}

int groove_player_event_peek(struct GroovePlayer *player, int block) {
    struct GroovePlayerPrivate *p = (struct GroovePlayerPrivate *) player;
    return groove_queue_peek(p->eventq, block);
}
