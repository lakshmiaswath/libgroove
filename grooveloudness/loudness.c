/*
 * Copyright (c) 2013 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "loudness.h"
#include <groove/queue.h>

#include <ebur128.h>

#include <libavutil/mem.h>
#include <libavutil/log.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct GrooveLoudnessDetectorPrivate {
    struct GrooveLoudnessDetector externals;

    int state_history_count;
    // index into all_track_states
    int cur_track_index;
    ebur128_state **all_track_states;
    struct GrooveSink *sink;
    struct GrooveQueue *info_queue;
    pthread_t thread_id;

    // info_head_mutex applies to variables inside this block.
    pthread_mutex_t info_head_mutex;
    char info_head_mutex_inited;
    // current playlist item pointer
    struct GroovePlaylistItem *info_head;
    double info_pos;
    // analyze_thread waits on this when the info queue is full
    pthread_cond_t drain_cond;
    char drain_cond_inited;
    // how many items are in the queue
    int info_queue_count;
    double album_peak;
    double track_duration;
    double album_duration;

    // set temporarily
    struct GroovePlaylistItem *purge_item;

    int abort_request;
};

static int emit_track_info(struct GrooveLoudnessDetectorPrivate *d) {
    struct GrooveLoudnessDetectorInfo *info = av_mallocz(sizeof(struct GrooveLoudnessDetectorInfo));
    if (!info) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate loudness detector info\n");
        return -1;
    }
    info->item = d->info_head;
    info->duration = d->track_duration;

    ebur128_state *cur_track_state = d->all_track_states[d->cur_track_index];
    ebur128_loudness_global(cur_track_state, &info->loudness);
    ebur128_sample_peak(cur_track_state, 0, &info->peak);
    double out;
    ebur128_sample_peak(cur_track_state, 1, &out);
    if (out > info->peak) info->peak = out;
    if (info->peak > d->album_peak) d->album_peak = info->peak;

    groove_queue_put(d->info_queue, info);

    return 0;
}

static int resize_state_history(struct GrooveLoudnessDetectorPrivate *d) {
    int new_size = d->state_history_count * 2;
    d->all_track_states = realloc(d->all_track_states, new_size);
    if (!d->all_track_states) {
        av_log(NULL, AV_LOG_ERROR, "unable to reallocate state pointer array\n");
        return -1;
    }
    memset(d->all_track_states + d->state_history_count, 0, new_size - d->state_history_count);
    d->state_history_count = new_size;
    return 0;
}

static void *detect_thread(void *arg) {
    struct GrooveLoudnessDetectorPrivate *d = arg;
    struct GrooveLoudnessDetector *detector = &d->externals;

    struct GrooveBuffer *buffer;
    while (!d->abort_request) {
        pthread_mutex_lock(&d->info_head_mutex);

        if (d->info_queue_count >= detector->info_queue_size) {
            pthread_cond_wait(&d->drain_cond, &d->info_head_mutex);
            pthread_mutex_unlock(&d->info_head_mutex);
            continue;
        }

        // we definitely want to unlock the mutex while we wait for the
        // next buffer. Otherwise there will be a deadlock when sink_flush or
        // sink_purge is called.
        pthread_mutex_unlock(&d->info_head_mutex);

        int result = groove_sink_buffer_get(d->sink, &buffer, 1);

        pthread_mutex_lock(&d->info_head_mutex);

        if (result == GROOVE_BUFFER_END) {
            // last file info
            emit_track_info(d);

            // send album info
            struct GrooveLoudnessDetectorInfo *info = av_mallocz(
                    sizeof(struct GrooveLoudnessDetectorInfo));
            if (info) {
                info->duration = d->album_duration;
                if (!detector->disable_album) {
                    ebur128_loudness_global_multiple(d->all_track_states, d->cur_track_index + 1,
                            &info->loudness);
                }
                info->peak = d->album_peak;
                groove_queue_put(d->info_queue, info);
            } else {
                av_log(NULL, AV_LOG_ERROR, "unable to allocate album loudness info\n");
            }

            if (!detector->disable_album) {
                for (int i = 0; i <= d->cur_track_index; i += 1) {
                    if (d->all_track_states[i])
                        ebur128_destroy(&d->all_track_states[i]);
                }
                d->cur_track_index = 0;
            }

            d->album_peak = 0.0;
            d->album_duration = 0.0;

            d->info_head = NULL;
            d->info_pos = -1.0;

            pthread_mutex_unlock(&d->info_head_mutex);
            continue;
        }

        if (result != GROOVE_BUFFER_YES) {
            pthread_mutex_unlock(&d->info_head_mutex);
            break;
        }

        if (buffer->item != d->info_head) {
            if (d->all_track_states[d->cur_track_index]) {
                emit_track_info(d);
                if (detector->disable_album) {
                    ebur128_destroy(&d->all_track_states[d->cur_track_index]);
                } else {
                    d->cur_track_index += 1;
                    if (d->cur_track_index >= d->state_history_count) {
                        av_log(NULL, AV_LOG_WARNING, "loudness scanner: resizing state history."
                                " Unless you're loudness-scanning very large albums you might"
                                " consider setting disable_album to 1.\n");
                        resize_state_history(d);
                    }
                }
            }
            d->all_track_states[d->cur_track_index] = ebur128_init(2, 44100,
                    EBUR128_MODE_SAMPLE_PEAK|EBUR128_MODE_I);
            if (!d->all_track_states[d->cur_track_index]) {
                av_log(NULL, AV_LOG_ERROR, "unable to allocate EBU R128 track context\n");
            }
            d->track_duration = 0.0;
            d->info_head = buffer->item;
            d->info_pos = buffer->pos;
        }

        double buffer_duration = buffer->frame_count / (double)buffer->format.sample_rate;
        d->track_duration += buffer_duration;
        d->album_duration += buffer_duration;
        ebur128_add_frames_double(d->all_track_states[d->cur_track_index],
                (double*)buffer->data[0], buffer->frame_count);

        pthread_mutex_unlock(&d->info_head_mutex);
        groove_buffer_unref(buffer);
    }

    return NULL;
}

static void info_queue_cleanup(struct GrooveQueue* queue, void *obj) {
    struct GrooveLoudnessDetectorInfo *info = obj;
    struct GrooveLoudnessDetectorPrivate *d = queue->context;
    d->info_queue_count -= 1;
    av_free(info);
}

static void info_queue_put(struct GrooveQueue *queue, void *obj) {
    struct GrooveLoudnessDetectorPrivate *d = queue->context;
    d->info_queue_count += 1;
}

static void info_queue_get(struct GrooveQueue *queue, void *obj) {
    struct GrooveLoudnessDetectorPrivate *d = queue->context;
    struct GrooveLoudnessDetector *detector = &d->externals;

    d->info_queue_count -= 1;

    if (d->info_queue_count < detector->info_queue_size)
        pthread_cond_signal(&d->drain_cond);
}

static int info_queue_purge(struct GrooveQueue* queue, void *obj) {
    struct GrooveLoudnessDetectorInfo *info = obj;
    struct GrooveLoudnessDetectorPrivate *d = queue->context;

    return info->item == d->purge_item;
}

static void sink_purge(struct GrooveSink *sink, struct GroovePlaylistItem *item) {
    struct GrooveLoudnessDetectorPrivate *d = sink->userdata;

    pthread_mutex_lock(&d->info_head_mutex);
    d->purge_item = item;
    groove_queue_purge(d->info_queue);
    d->purge_item = NULL;

    if (d->info_head == item) {
        d->info_head = NULL;
        d->info_pos = -1.0;
    }
    pthread_cond_signal(&d->drain_cond);
    pthread_mutex_unlock(&d->info_head_mutex);
}

static void sink_flush(struct GrooveSink *sink) {
    struct GrooveLoudnessDetectorPrivate *d = sink->userdata;

    pthread_mutex_lock(&d->info_head_mutex);
    groove_queue_flush(d->info_queue);
    for (int i = 0; i <= d->cur_track_index; i += 1) {
        if (d->all_track_states[i])
            ebur128_destroy(&d->all_track_states[i]);
    }
    d->cur_track_index = 0;
    d->track_duration = 0.0;
    d->info_head = NULL;
    d->info_pos = -1.0;

    pthread_cond_signal(&d->drain_cond);
    pthread_mutex_unlock(&d->info_head_mutex);
}

struct GrooveLoudnessDetector *groove_loudness_detector_create(void) {
    struct GrooveLoudnessDetectorPrivate *d = av_mallocz(sizeof(struct GrooveLoudnessDetectorPrivate));
    if (!d) {
        av_log(NULL, AV_LOG_ERROR, "unable to allocate loudness detector\n");
        return NULL;
    }

    struct GrooveLoudnessDetector *detector = &d->externals;

    if (pthread_mutex_init(&d->info_head_mutex, NULL) != 0) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex\n");
        return NULL;
    }
    d->info_head_mutex_inited = 1;

    if (pthread_cond_init(&d->drain_cond, NULL) != 0) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to create mutex condition\n");
        return NULL;
    }
    d->drain_cond_inited = 1;

    d->info_queue = groove_queue_create();
    if (!d->info_queue) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate queue\n");
        return NULL;
    }
    d->info_queue->context = detector;
    d->info_queue->cleanup = info_queue_cleanup;
    d->info_queue->put = info_queue_put;
    d->info_queue->get = info_queue_get;
    d->info_queue->purge = info_queue_purge;

    d->sink = groove_sink_create();
    if (!d->sink) {
        groove_loudness_detector_destroy(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate sink\n");
        return NULL;
    }
    d->sink->audio_format.sample_rate = 44100;
    d->sink->audio_format.channel_layout = GROOVE_CH_LAYOUT_STEREO;
    d->sink->audio_format.sample_fmt = GROOVE_SAMPLE_FMT_DBL;
    d->sink->userdata = detector;
    d->sink->purge = sink_purge;
    d->sink->flush = sink_flush;

    // set some defaults
    detector->info_queue_size = INT_MAX;
    detector->sink_buffer_size = d->sink->buffer_size;

    return detector;
}

void groove_loudness_detector_destroy(struct GrooveLoudnessDetector *detector) {
    if (!detector)
        return;

    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    if (d->sink)
        groove_sink_destroy(d->sink);

    if (d->info_queue)
        groove_queue_destroy(d->info_queue);

    if (d->info_head_mutex_inited)
        pthread_mutex_destroy(&d->info_head_mutex);

    if (d->drain_cond_inited)
        pthread_cond_destroy(&d->drain_cond);

    av_free(d);
}

int groove_loudness_detector_attach(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylist *playlist)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    detector->playlist = playlist;
    groove_queue_reset(d->info_queue);

    // set the initial state history size. if we run out we will realloc later.
    d->state_history_count = detector->disable_album ? 1 : 128;
    d->all_track_states = calloc(d->state_history_count, sizeof(ebur128_state*));
    d->cur_track_index = 0;
    if (!d->all_track_states) {
        groove_loudness_detector_detach(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to allocate ebur128 track state pointers\n");
        return -1;
    }

    if (groove_sink_attach(d->sink, playlist) < 0) {
        groove_loudness_detector_detach(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to attach sink\n");
        return -1;
    }

    if (pthread_create(&d->thread_id, NULL, detect_thread, detector) != 0) {
        groove_loudness_detector_detach(detector);
        av_log(NULL, AV_LOG_ERROR, "unable to create detector thread\n");
        return -1;
    }

    return 0;
}

int groove_loudness_detector_detach(struct GrooveLoudnessDetector *detector) {
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    d->abort_request = 1;
    groove_sink_detach(d->sink);
    groove_queue_flush(d->info_queue);
    groove_queue_abort(d->info_queue);
    pthread_cond_signal(&d->drain_cond);
    pthread_join(d->thread_id, NULL);

    detector->playlist = NULL;

    if (d->all_track_states) {
        for (int i = 0; i <= d->cur_track_index; i += 1) {
            if (d->all_track_states[i])
                ebur128_destroy(&d->all_track_states[i]);
        }
        free(d->all_track_states);
        d->all_track_states = NULL;
    }
    d->cur_track_index = 0;

    d->abort_request = 0;
    d->info_head = NULL;
    d->info_pos = 0;

    return 0;
}

int groove_loudness_detector_info_get(struct GrooveLoudnessDetector *detector,
        struct GrooveLoudnessDetectorInfo *info, int block)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    struct GrooveLoudnessDetectorInfo *info_ptr;
    if (groove_queue_get(d->info_queue, (void**)&info_ptr, block) == 1) {
        *info = *info_ptr;
        av_free(info_ptr);
        return 1;
    }

    return 0;
}

int groove_loudness_detector_info_peek(struct GrooveLoudnessDetector *detector,
        int block)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;
    return groove_queue_peek(d->info_queue, block);
}

void groove_loudness_detector_position(struct GrooveLoudnessDetector *detector,
        struct GroovePlaylistItem **item, double *seconds)
{
    struct GrooveLoudnessDetectorPrivate *d = (struct GrooveLoudnessDetectorPrivate *) detector;

    pthread_mutex_lock(&d->info_head_mutex);

    if (item)
        *item = d->info_head;

    if (seconds)
        *seconds = d->info_pos;

    pthread_mutex_unlock(&d->info_head_mutex);
}
