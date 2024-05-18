/*
 * Copyright (C) 2024 Mikhail Burakov. This file is part of streamer.
 *
 * streamer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * streamer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with streamer.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef USE_PIPEWIRE

#include "audio.h"

#include <errno.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/utils/result.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer_queue.h"
#include "toolbox/utils.h"

#define STATUS_OK 0
#define STATUS_ERR 1

struct AudioContext {
  const struct AudioContextCallbacks* callbacks;
  void* user;

  int waker[2];
  struct BufferQueue* buffer_queue;
  struct pw_thread_loop* pw_thread_loop;
  struct pw_stream* pw_stream;
};

static void WakeClient(const struct AudioContext* audio_context, char status) {
  if (write(audio_context->waker[1], &status, sizeof(status)) !=
      sizeof(status)) {
    // TODO(mburakov): Then what?..
    abort();
  }
}

static void OnStreamStateChanged(void* data, enum pw_stream_state old,
                                 enum pw_stream_state state,
                                 const char* error) {
  (void)data;
  LOG("Stream state change %s->%s, error is %s", pw_stream_state_as_string(old),
      pw_stream_state_as_string(state), error);
}

static void OnStreamParamChanged(void* data, uint32_t id,
                                 const struct spa_pod* param) {
  struct AudioContext* audio_context = data;
  if (param == NULL || id != SPA_PARAM_Format) return;

  struct spa_audio_info audio_info;
  if (spa_format_parse(param, &audio_info.media_type,
                       &audio_info.media_subtype) < 0) {
    LOG("Failed to parse stream format");
    goto failure;
  }
  if (audio_info.media_type != SPA_MEDIA_TYPE_audio ||
      audio_info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
    LOG("Unexpected stream format");
    goto failure;
  }
  if (spa_format_audio_raw_parse(param, &audio_info.info.raw) < 0) {
    LOG("Faield to parse audio stream format");
    goto failure;
  }

  LOG("Capturing rate: %u, channels: %u", audio_info.info.raw.rate,
      audio_info.info.raw.channels);
  return;

failure:
  pw_thread_loop_stop(audio_context->pw_thread_loop);
  WakeClient(audio_context, STATUS_ERR);
}

static void OnStreamProcess(void* data) {
  struct AudioContext* audio_context = data;
  struct pw_buffer* pw_buffer =
      pw_stream_dequeue_buffer(audio_context->pw_stream);
  if (!pw_buffer) {
    LOG("Failed to dequeue stream buffer");
    goto failure;
  }

  for (uint32_t i = 0; i < pw_buffer->buffer->n_datas; i++) {
    const struct spa_data* spa_data = pw_buffer->buffer->datas + i;
    const void* buffer = (const uint8_t*)spa_data->data +
                         spa_data->chunk->offset % spa_data->maxsize;
    uint32_t size = MIN(spa_data->chunk->size, spa_data->maxsize);
    struct BufferQueueItem* buffer_queue_item =
        BufferQueueItemCreate(buffer, size);
    if (!buffer_queue_item) {
      LOG("Failed to copy stream buffer");
      goto failure;
    }
    if (!BufferQueueQueue(audio_context->buffer_queue, buffer_queue_item)) {
      LOG("Failed to queue stream buffer copy");
      BufferQueueItemDestroy(buffer_queue_item);
      goto failure;
    }
  }

  pw_stream_queue_buffer(audio_context->pw_stream, pw_buffer);
  WakeClient(audio_context, STATUS_OK);
  return;

failure:
  pw_thread_loop_stop(audio_context->pw_thread_loop);
  WakeClient(audio_context, STATUS_ERR);
}

struct AudioContext* AudioContextCreate(
    const struct AudioContextCallbacks* callbacks, void* user) {
  pw_init(0, NULL);
  struct AudioContext* audio_context = malloc(sizeof(struct AudioContext));
  if (!audio_context) {
    LOG("Failed to allocate audio context (%s)", strerror(errno));
    return NULL;
  }
  *audio_context = (struct AudioContext){
      .callbacks = callbacks,
      .user = user,
  };

  if (pipe(audio_context->waker)) {
    LOG("Failed to create pipe (%s)", strerror(errno));
    goto rollback_audio_context;
  }

  audio_context->buffer_queue = BufferQueueCreate();
  if (!audio_context->buffer_queue) {
    LOG("Failed to create buffer queue (%s)", strerror(errno));
    goto rollback_waker;
  }

  audio_context->pw_thread_loop = pw_thread_loop_new("audio-capture", NULL);
  if (!audio_context->pw_thread_loop) {
    LOG("Failed to create pipewire thread loop");
    goto rollback_buffer_queue;
  }

  pw_thread_loop_lock(audio_context->pw_thread_loop);
  int err = pw_thread_loop_start(audio_context->pw_thread_loop);
  if (err) {
    LOG("Failed to start pipewire thread loop (%s)", spa_strerror(err));
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  // TOOD(mburakov): Read these from the commandline?
  struct pw_properties* pw_properties = pw_properties_new(
#define _(...) __VA_ARGS__
      _(PW_KEY_AUDIO_FORMAT, "S16LE"), _(PW_KEY_AUDIO_RATE, "48000"),
      _(PW_KEY_AUDIO_CHANNELS, "2"), _(SPA_KEY_AUDIO_POSITION, "FL,FR"),
      _(PW_KEY_NODE_NAME, "streamer-sink"), _(PW_KEY_NODE_VIRTUAL, "true"),
      _(PW_KEY_MEDIA_CLASS, "Audio/Sink"), NULL
#undef _
  );
  if (!pw_properties) {
    LOG("Failed to create pipewire properties");
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  static const struct pw_stream_events kPwStreamEvents = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = OnStreamStateChanged,
      .param_changed = OnStreamParamChanged,
      .process = OnStreamProcess,
  };
  audio_context->pw_stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(audio_context->pw_thread_loop), "audio-capture",
      pw_properties, &kPwStreamEvents, audio_context);
  if (!audio_context->pw_stream) {
    LOG("Failed to create pipewire stream");
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder spa_pod_builder =
      SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod* params[] = {
      spa_format_audio_raw_build(
          &spa_pod_builder, SPA_PARAM_EnumFormat,
          &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16_LE,
                                   .rate = 48000, .channels = 2,
                                   .position = {SPA_AUDIO_CHANNEL_FL,
                                                SPA_AUDIO_CHANNEL_FR})),
  };
  static const enum pw_stream_flags kPwStreamFlags =
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS;
  if (pw_stream_connect(audio_context->pw_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                        kPwStreamFlags, params, LENGTH(params))) {
    LOG("Failed to connect pipewire stream");
    pw_stream_destroy(audio_context->pw_stream);
    pw_thread_loop_unlock(audio_context->pw_thread_loop);
    goto rollback_thread_loop;
  }

  pw_thread_loop_unlock(audio_context->pw_thread_loop);
  return audio_context;

rollback_thread_loop:
  pw_thread_loop_destroy(audio_context->pw_thread_loop);
rollback_buffer_queue:
  BufferQueueDestroy(audio_context->buffer_queue);
rollback_waker:
  close(audio_context->waker[1]);
  close(audio_context->waker[0]);
rollback_audio_context:
  free(audio_context);
  pw_deinit();
  return NULL;
}

int AudioContextGetEventsFd(struct AudioContext* audio_context) {
  return audio_context->waker[0];
}

bool AudioContextProcessEvents(struct AudioContext* audio_context) {
  char status;
  if (read(audio_context->waker[0], &status, sizeof(status)) !=
      sizeof(status)) {
    // TODO(mburakov): Then what?..
    abort();
  }

  switch (status) {
    case STATUS_OK:
      break;
    case STATUS_ERR:
      LOG("Error reported from audio thread");
      return false;
    default:
      __builtin_unreachable();
  }

  for (;;) {
    struct BufferQueueItem* buffer_queue_item;
    if (!BufferQueueDequeue(audio_context->buffer_queue, &buffer_queue_item)) {
      LOG("Failed to dequeue stream buffer copy");
      return false;
    }
    if (!buffer_queue_item) return true;
    audio_context->callbacks->OnAudioReady(
        audio_context->user, buffer_queue_item->data, buffer_queue_item->size);
    BufferQueueItemDestroy(buffer_queue_item);
  }
}

void AudioContextDestroy(struct AudioContext* audio_context) {
  pw_thread_loop_lock(audio_context->pw_thread_loop);
  pw_stream_destroy(audio_context->pw_stream);
  pw_thread_loop_unlock(audio_context->pw_thread_loop);
  pw_thread_loop_destroy(audio_context->pw_thread_loop);
  BufferQueueDestroy(audio_context->buffer_queue);
  close(audio_context->waker[1]);
  close(audio_context->waker[0]);
  free(audio_context);
  pw_deinit();
}

#endif  // USE_PIPEWIRE
