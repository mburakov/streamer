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

#include "audio_context.h"

#include <assert.h>
#include <errno.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_context.h"
#include "proto.h"
#include "util.h"

struct AudioContext {
  struct IoContext* io_context;
  struct pw_thread_loop* thread_loop;
  struct pw_stream* stream;
};

struct ProtoImpl {
  struct Proto proto;
  struct ProtoHeader header;
  uint8_t data[];
};

static void ProtoDestroy(struct Proto* proto) { free(proto); }

static void OnStreamStateChanged(void* arg, enum pw_stream_state old,
                                 enum pw_stream_state state,
                                 const char* error) {
  (void)arg;
  LOG("Stream state change %s->%s, error is %s", pw_stream_state_as_string(old),
      pw_stream_state_as_string(state), error);
}

static void OnStreamParamChanged(void* arg, uint32_t id,
                                 const struct spa_pod* param) {
  (void)arg;
  if (!param || id != SPA_PARAM_Format) return;

  struct spa_audio_info audio_info;
  if (spa_format_parse(param, &audio_info.media_type,
                       &audio_info.media_subtype) < 0) {
    LOG("Failed to parse stream format");
    return;
  }

  if (audio_info.media_type != SPA_MEDIA_TYPE_audio ||
      audio_info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
    LOG("Unexpected stream format");
    return;
  }

  if (spa_format_audio_raw_parse(param, &audio_info.info.raw) < 0) {
    LOG("Failed to parse stream raw format");
    return;
  }

  LOG("Params changed to format=%u, rate=%u, channels=%u",
      audio_info.info.raw.format, audio_info.info.raw.rate,
      audio_info.info.raw.channels);
}

static void OnStreamProcess(void* arg) {
  struct AudioContext* audio_context = arg;
  struct pw_buffer* buffer = pw_stream_dequeue_buffer(audio_context->stream);
  if (!buffer) {
    LOG("Failed to dequeue stream buffer");
    return;
  }

  struct ProtoHeader header = {
      .type = kProtoTypeAudio,
      .timestamp = buffer->time,
  };
  for (size_t index = 0; index < buffer->buffer->n_datas; index++)
    header.size += buffer->buffer->datas[index].chunk->size;

  struct ProtoImpl* proto_impl = malloc(sizeof(struct ProtoImpl) + header.size);
  if (!proto_impl) {
    LOG("Failed to allocate proto (%s)", strerror(errno));
    goto rollback_buffer;
  }

  proto_impl->header = header;
  const struct Proto proto = {
    .Destroy = ProtoDestroy,
    .header = &proto_impl->header,
    .data = proto_impl->data,
  };
  memcpy(proto_impl, &proto, sizeof(proto));
  uint8_t* target = proto_impl->data;
  for (size_t index = 0; index < buffer->buffer->n_datas; index++) {
    const void* source = buffer->buffer->datas[index].data;
    struct spa_chunk* chunk = buffer->buffer->datas[index].chunk;
    memcpy(target, source + chunk->offset, chunk->size);
    target += chunk->size;
  }
  if (!IoContextWrite(audio_context->io_context, &proto_impl->proto)) {
    LOG("Failed to write audio proto");
    goto rollback_buffer;
  }

rollback_buffer:
  assert(!pw_stream_queue_buffer(audio_context->stream, buffer));
}

struct AudioContext* AudioContextCreate(struct IoContext* io_context,
                                        struct Proto* proto_hello) {
  assert(proto_hello->header->type == kProtoTypeHello);
  struct AudioContext* audio_context = malloc(sizeof(struct AudioContext));
  if (!audio_context) {
    LOG("Failed to allocate audio context (%s)", strerror(errno));
    goto rollback_proto_hello;
  }

  *audio_context = (struct AudioContext){
      .io_context = io_context,
      .thread_loop = pw_thread_loop_new("audio-capture", NULL),
  };
  if (!audio_context->thread_loop) {
    LOG("Failed to create thread loop");
    goto rollback_audio_context;
  }

  pw_thread_loop_lock(audio_context->thread_loop);
  if (pw_thread_loop_start(audio_context->thread_loop)) {
    LOG("Failed to start thread loop");
    goto rollback_thread_loop;
  }

  struct pw_properties* properties = pw_properties_new(
#define _(...) __VA_ARGS__
      _(PW_KEY_NODE_NAME, "streamer-sink"), _(PW_KEY_NODE_VIRTUAL, "true"),
      _(PW_KEY_MEDIA_CLASS, "Audio/Sink"), NULL
#undef _
  );
  if (!properties) {
    LOG("Failed to create properties");
    goto rollback_thread_loop;
  }

  static const struct pw_stream_events kStreamEvents = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = OnStreamStateChanged,
      .param_changed = OnStreamParamChanged,
      .process = OnStreamProcess,
  };
  audio_context->stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(audio_context->thread_loop), "audio-capture",
      properties, &kStreamEvents, audio_context);
  if (!audio_context->stream) {
    LOG("Failed to create stream");
    goto rollback_thread_loop;
  }

  uint8_t buffer[1024];
  struct spa_pod_builder builder = {
      .data = buffer,
      .size = sizeof(buffer),
  };
  if (proto_hello->header->size != sizeof(struct spa_audio_info_raw)) {
    LOG("Invalid hello proto");
    goto rollback_stream;
  }

  const struct spa_pod* params[] = {
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
                                 proto_hello->data),
  };
  static const enum pw_stream_flags kStreamFlags = (enum pw_stream_flags)(
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS);
  if (pw_stream_connect(audio_context->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                        kStreamFlags, params, LENGTH(params))) {
    LOG("Failed to connect stream");
    goto rollback_stream;
  }

  pw_thread_loop_unlock(audio_context->thread_loop);
  proto_hello->Destroy(proto_hello);
  return audio_context;

rollback_stream:
  pw_stream_destroy(audio_context->stream);
rollback_thread_loop:
  pw_thread_loop_unlock(audio_context->thread_loop);
  pw_thread_loop_destroy(audio_context->thread_loop);
rollback_audio_context:
  free(audio_context);
rollback_proto_hello:
  proto_hello->Destroy(proto_hello);
  return NULL;
}

void AudioContextDestroy(struct AudioContext* audio_context) {
  pw_thread_loop_lock(audio_context->thread_loop);
  pw_stream_destroy(audio_context->stream);
  pw_thread_loop_unlock(audio_context->thread_loop);
  pw_thread_loop_destroy(audio_context->thread_loop);
  free(audio_context);
}
