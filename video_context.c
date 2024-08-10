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

#include "video_context.h"

#include <assert.h>
#include <errno.h>
#include <pipewire/pipewire.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <wayland-client.h>

#include "util.h"
#include "wlr-export-dmabuf-unstable-v1.h"

struct VideoContext {
  struct IoContext* io_context;

  // Wayland globals
  struct wl_display* display;
  struct wl_registry* registry;
  struct wl_output* output;
  struct zwlr_export_dmabuf_manager_v1* export_dmabuf_manager;

  // Threading
  struct pw_thread_loop* thread_loop;
  struct spa_source* source;
};

static void OnRegistryGlobal(void* data, struct wl_registry* wl_registry,
                             uint32_t name, const char* interface,
                             uint32_t version) {
#define MAYBE_BIND(a, b)                                            \
  if (!strcmp(interface, a.name)) do {                              \
      if (!b) b = wl_registry_bind(wl_registry, name, &a, version); \
      if (!b) LOG("Failed to bind " #a " (%s)", strerror(errno));   \
      return;                                                       \
  } while (false)
  struct VideoContext* video_context = data;
  MAYBE_BIND(wl_output_interface, video_context->output);
  MAYBE_BIND(zwlr_export_dmabuf_manager_v1_interface,
             video_context->export_dmabuf_manager);
#undef MAYBE_BIND
}

static void OnRegistryGlobalRemove(void* data, struct wl_registry* registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static bool InitWaylandGlobals(struct VideoContext* video_context) {
  video_context->display = wl_display_connect(NULL);
  if (!video_context->display) {
    LOG("Failed to open display");
    return false;
  }

  video_context->registry = wl_display_get_registry(video_context->display);
  if (!video_context->registry) {
    LOG("Failed to get registry");
    goto rollback_display;
  }

  static const struct wl_registry_listener kRegistryListener = {
      .global = OnRegistryGlobal,
      .global_remove = OnRegistryGlobalRemove,
  };
  if (wl_registry_add_listener(video_context->registry, &kRegistryListener,
                               video_context)) {
    LOG("Failed to add registry listener");
    goto rollback_registry;
  }
  if (wl_display_roundtrip(video_context->display) == -1) {
    LOG("Failed to roundtrip display");
    goto rollback_registry;
  }

  if (!video_context->output || !video_context->export_dmabuf_manager) {
    LOG("Some required globals are missing");
    goto rollback_globals;
  }

  return true;

rollback_globals:
  if (video_context->export_dmabuf_manager)
    zwlr_export_dmabuf_manager_v1_destroy(video_context->export_dmabuf_manager);
  if (video_context->output) wl_output_destroy(video_context->output);
rollback_registry:
  wl_registry_destroy(video_context->registry);
rollback_display:
  wl_display_disconnect(video_context->display);
  return false;
}

static void OnDisplayData(void* arg, int fd, uint32_t mask) {
  (void)fd;
  (void)mask;
  struct VideoContext* video_context = arg;
  if (wl_display_dispatch(video_context->display) == -1) {
    LOG("Failed to dispatch display");
    // TODO(mburakov): Now what?..
  }
}

struct VideoContext* VideoContextCreate(struct IoContext* io_context) {
  struct VideoContext* video_context = malloc(sizeof(struct VideoContext));
  if (!video_context) {
    LOG("Failed to allocate video context (%s)", strerror(errno));
    return NULL;
  }

  *video_context = (struct VideoContext){
      .io_context = io_context,
  };
  if (!InitWaylandGlobals(video_context)) {
    LOG("Failed to init wayland globals");
    goto rollback_video_context;
  }

  video_context->thread_loop = pw_thread_loop_new("video-capture", NULL);
  if (!video_context->thread_loop) {
    LOG("Failed to create thread loop");
    goto rollback_wayland_globals;
  }

  pw_thread_loop_lock(video_context->thread_loop);
  if (pw_thread_loop_start(video_context->thread_loop)) {
    LOG("Failed to start thread loop");
    goto rollback_thread_loop;
  }

  int events_fd = wl_display_get_fd(video_context->display);
  if (events_fd == -1) {
    LOG("Failed to get display fd");
    goto rollback_thread_loop;
  }

  video_context->source =
      pw_loop_add_io(pw_thread_loop_get_loop(video_context->thread_loop),
                     events_fd, SPA_IO_IN, false, OnDisplayData, video_context);
  if (!video_context->source) {
    LOG("Failed to add thread loop io");
    goto rollback_thread_loop;
  }

  pw_thread_loop_unlock(video_context->thread_loop);
  return video_context;

rollback_thread_loop:
  pw_thread_loop_unlock(video_context->thread_loop);
  pw_thread_loop_destroy(video_context->thread_loop);
rollback_wayland_globals:
  zwlr_export_dmabuf_manager_v1_destroy(video_context->export_dmabuf_manager);
  wl_output_destroy(video_context->output);
  wl_registry_destroy(video_context->registry);
  wl_display_disconnect(video_context->display);
rollback_video_context:
  free(video_context);
  return NULL;
}

void VideoContextDestroy(struct VideoContext* video_context) {
  pw_thread_loop_lock(video_context->thread_loop);
  assert(pw_loop_destroy_source(
      pw_thread_loop_get_loop(video_context->thread_loop),
      video_context->source));
  pw_thread_loop_unlock(video_context->thread_loop);
  pw_thread_loop_destroy(video_context->thread_loop);
  zwlr_export_dmabuf_manager_v1_destroy(video_context->export_dmabuf_manager);
  wl_output_release(video_context->output);
  wl_registry_destroy(video_context->registry);
  wl_display_disconnect(video_context->display);
  free(video_context);
}
