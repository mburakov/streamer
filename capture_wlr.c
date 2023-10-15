/*
 * Copyright (C) 2023 Mikhail Burakov. This file is part of streamer.
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

#ifdef USE_WAYLAND

#include "capture_wlr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "gpu.h"
#include "toolbox/utils.h"
#include "wlr-export-dmabuf-unstable-v1.h"

struct CaptureContextWlr {
  struct IoMuxer* io_muxer;
  struct GpuContext* gpu_context;
  const struct CaptureContextCallbacks* callbacks;
  void* user;

  // Wayland globals
  struct wl_display* wl_display;
  struct wl_registry* wl_registry;
  struct wl_output* wl_output;
  struct zwlr_export_dmabuf_manager_v1* zwlr_export_dmabuf_manager_v1;

  // Wayland dynamics
  struct zwlr_export_dmabuf_frame_v1* zwlr_export_dmabuf_frame_v1;

  // Volatile states
  uint32_t gpu_frame_width;
  uint32_t gpu_frame_height;
  uint32_t gpu_frame_fourcc;
  size_t gpu_frame_nplanes;
  struct GpuFramePlane gpu_frame_planes[4];
};

static bool CaptureOutput(struct CaptureContextWlr* capture_context);

static void OnWlRegistryGlobal(void* data, struct wl_registry* wl_registry,
                               uint32_t name, const char* interface,
                               uint32_t version) {
#define MAYBE_BIND(what)                                                   \
  if (!strcmp(interface, what##_interface.name)) do {                      \
      capture_context->what =                                              \
          wl_registry_bind(wl_registry, name, &what##_interface, version); \
      if (!capture_context->what)                                          \
        LOG("Failed to bind " #what " (%s)", strerror(errno));             \
      return;                                                              \
  } while (false)
  struct CaptureContextWlr* capture_context = data;
  if (!capture_context->wl_output) MAYBE_BIND(wl_output);
  MAYBE_BIND(zwlr_export_dmabuf_manager_v1);
#undef MAYBE_BIND
}

static void OnWlRegistryGlobalRemove(void* data, struct wl_registry* registry,
                                     uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static bool InitWaylandGlobals(struct CaptureContextWlr* capture_context) {
  capture_context->wl_display =
      wl_display_connect(/*NULL*/ "/run/user/1000/wayland-1");
  if (!capture_context->wl_display) {
    LOG("Failed to connect wl_display (%s)", strerror(errno));
    return false;
  }

  capture_context->wl_registry =
      wl_display_get_registry(capture_context->wl_display);
  if (!capture_context->wl_registry) {
    LOG("Failed to get wl_registry (%s)", strerror(errno));
    goto rollback_wl_display;
  }

  static const struct wl_registry_listener wl_registry_listener = {
      .global = OnWlRegistryGlobal,
      .global_remove = OnWlRegistryGlobalRemove,
  };
  if (wl_registry_add_listener(capture_context->wl_registry,
                               &wl_registry_listener, capture_context)) {
    LOG("Failed to add wl_registry listener (%s)", strerror(errno));
    goto rollback_wl_registry;
  }
  if (wl_display_roundtrip(capture_context->wl_display) == -1) {
    LOG("Failed to roundtrip wl_display (%s)", strerror(errno));
    goto rollback_wl_registry;
  }

  if (!capture_context->wl_output ||
      !capture_context->zwlr_export_dmabuf_manager_v1) {
    LOG("Some required wayland globals are missing");
    goto rollback_wayland_globals;
  }
  return true;

rollback_wayland_globals:
  if (capture_context->zwlr_export_dmabuf_manager_v1)
    zwlr_export_dmabuf_manager_v1_destroy(
        capture_context->zwlr_export_dmabuf_manager_v1);
  if (capture_context->wl_output) wl_output_destroy(capture_context->wl_output);
rollback_wl_registry:
  wl_registry_destroy(capture_context->wl_registry);
rollback_wl_display:
  wl_display_disconnect(capture_context->wl_display);
  return false;
}

static void DeinitWaylandGlobals(struct CaptureContextWlr* capture_context) {
  zwlr_export_dmabuf_manager_v1_destroy(
      capture_context->zwlr_export_dmabuf_manager_v1);
  wl_output_destroy(capture_context->wl_output);
  wl_registry_destroy(capture_context->wl_registry);
  wl_display_disconnect(capture_context->wl_display);
}

static void OnExportDmabufFrameFrame(
    void* data, struct zwlr_export_dmabuf_frame_v1* zwlr_export_dmabuf_frame_v1,
    uint32_t width, uint32_t height, uint32_t offset_x, uint32_t offset_y,
    uint32_t buffer_flags, uint32_t flags, uint32_t format, uint32_t mod_high,
    uint32_t mod_low, uint32_t num_objects) {
  (void)zwlr_export_dmabuf_frame_v1;
  (void)offset_x;
  (void)offset_y;
  (void)buffer_flags;
  (void)flags;
  struct CaptureContextWlr* capture_context = data;
  capture_context->gpu_frame_width = width;
  capture_context->gpu_frame_height = height;
  capture_context->gpu_frame_fourcc = format;
  capture_context->gpu_frame_nplanes = num_objects;
  for (size_t i = 0; i < LENGTH(capture_context->gpu_frame_planes); i++) {
    capture_context->gpu_frame_planes[i].dmabuf_fd = -1;
    capture_context->gpu_frame_planes[i].modifier =
        (uint64_t)mod_high << 32 | mod_low;
  }
}

static void OnExportDmabufFrameObject(
    void* data, struct zwlr_export_dmabuf_frame_v1* zwlr_export_dmabuf_frame_v1,
    uint32_t index, int32_t fd, uint32_t size, uint32_t offset, uint32_t stride,
    uint32_t plane_index) {
  (void)zwlr_export_dmabuf_frame_v1;
  (void)size;
  (void)index;
  struct CaptureContextWlr* capture_context = data;
  capture_context->gpu_frame_planes[plane_index].dmabuf_fd = fd;
  capture_context->gpu_frame_planes[plane_index].pitch = stride;
  capture_context->gpu_frame_planes[plane_index].offset = offset;
}

static void OnExportDmabufFrameReady(
    void* data, struct zwlr_export_dmabuf_frame_v1* zwlr_export_dmabuf_frame_v1,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
  (void)zwlr_export_dmabuf_frame_v1;
  (void)tv_sec_hi;
  (void)tv_sec_lo;
  (void)tv_nsec;
  struct CaptureContextWlr* capture_context = data;

  struct GpuFrame* gpu_frame = GpuContextCreateFrame(
      capture_context->gpu_context, capture_context->gpu_frame_width,
      capture_context->gpu_frame_height, capture_context->gpu_frame_fourcc,
      capture_context->gpu_frame_nplanes, capture_context->gpu_frame_planes);
  if (!gpu_frame) {
    // TODO(mburakov): ... then what?
    abort();
  }

  // TODO(mburakov): Callee could theoretically drop client, and then below code
  // in this function would probably fail miserably. Do something about this!
  capture_context->callbacks->OnFrameReady(capture_context->user, gpu_frame);

  CloseUniqueFds((int[4]){capture_context->gpu_frame_planes[0].dmabuf_fd,
                          capture_context->gpu_frame_planes[1].dmabuf_fd,
                          capture_context->gpu_frame_planes[2].dmabuf_fd,
                          capture_context->gpu_frame_planes[3].dmabuf_fd});
  zwlr_export_dmabuf_frame_v1_destroy(
      capture_context->zwlr_export_dmabuf_frame_v1);
  capture_context->zwlr_export_dmabuf_frame_v1 = NULL;

  if (!CaptureOutput(capture_context)) {
    // TODO(mburakov): ... then what?
    abort();
  }
}

static void OnExportDmabufFrameCancel(
    void* data, struct zwlr_export_dmabuf_frame_v1* zwlr_export_dmabuf_frame_v1,
    uint32_t reason) {
  (void)zwlr_export_dmabuf_frame_v1;
  struct CaptureContextWlr* capture_context = data;
  CloseUniqueFds((int[4]){capture_context->gpu_frame_planes[0].dmabuf_fd,
                          capture_context->gpu_frame_planes[1].dmabuf_fd,
                          capture_context->gpu_frame_planes[2].dmabuf_fd,
                          capture_context->gpu_frame_planes[3].dmabuf_fd});
  zwlr_export_dmabuf_frame_v1_destroy(
      capture_context->zwlr_export_dmabuf_frame_v1);
  capture_context->zwlr_export_dmabuf_frame_v1 = NULL;
  bool result;
  switch (reason) {
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_TEMPORARY:
      result = CaptureOutput(capture_context);
      break;
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT:
      result = false;
      break;
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_RESIZING:
      result = CaptureOutput(capture_context);
      break;
    default:
      __builtin_unreachable();
  }
  if (!result) {
    // TODO(mburakov): ... then what?
    abort();
  }
}

static bool CaptureOutput(struct CaptureContextWlr* capture_context) {
  capture_context->zwlr_export_dmabuf_frame_v1 =
      zwlr_export_dmabuf_manager_v1_capture_output(
          capture_context->zwlr_export_dmabuf_manager_v1, 1,
          capture_context->wl_output);
  if (!capture_context->zwlr_export_dmabuf_frame_v1) {
    LOG("Failed to capture zwlr_export_dmabuf_manager_v1 (%s)",
        strerror(errno));
    return false;
  }

  static const struct zwlr_export_dmabuf_frame_v1_listener
      zwlr_export_dmabuf_frame_v1_listener = {
          .frame = OnExportDmabufFrameFrame,
          .object = OnExportDmabufFrameObject,
          .ready = OnExportDmabufFrameReady,
          .cancel = OnExportDmabufFrameCancel,
      };
  if (zwlr_export_dmabuf_frame_v1_add_listener(
          capture_context->zwlr_export_dmabuf_frame_v1,
          &zwlr_export_dmabuf_frame_v1_listener, capture_context)) {
    LOG("Failed to add zwlr_export_dmabuf_frame_v1 listener (%s)",
        strerror(errno));
    goto rollback_frame;
  }

  if (wl_display_flush(capture_context->wl_display) == -1) {
    LOG("Failed to flush wl_display (%s)", strerror(errno));
    goto rollback_frame;
  }
  return true;

rollback_frame:
  zwlr_export_dmabuf_frame_v1_destroy(
      capture_context->zwlr_export_dmabuf_frame_v1);
  capture_context->zwlr_export_dmabuf_frame_v1 = NULL;
  return false;
}

struct CaptureContextWlr* CaptureContextWlrCreate(
    struct GpuContext* gpu_context,
    const struct CaptureContextCallbacks* callbacks, void* user) {
  struct CaptureContextWlr* capture_context =
      malloc(sizeof(struct CaptureContextWlr));
  if (!capture_context) {
    LOG("Failed to allocate capture context (%s)", strerror(errno));
    return NULL;
  }
  *capture_context = (struct CaptureContextWlr){
      .gpu_context = gpu_context,
      .callbacks = callbacks,
      .user = user,
  };

  if (!InitWaylandGlobals(capture_context)) {
    LOG("Failed to initialize wayland globals");
    goto rollback_capture_context;
  }
  if (!CaptureOutput(capture_context)) {
    LOG("Failed to capture output");
    goto rollback_wayland_globals;
  }
  return capture_context;

rollback_wayland_globals:
  DeinitWaylandGlobals(capture_context);
rollback_capture_context:
  free(capture_context);
  return NULL;
}

int CaptureContextWlrGetEventsFd(struct CaptureContextWlr* capture_context) {
  int events_fd = wl_display_get_fd(capture_context->wl_display);
  if (events_fd == -1) LOG("Failed to get wl_display fd (%s)", strerror(errno));
  return events_fd;
}

bool CaptureContextWlrProcessEvents(struct CaptureContextWlr* capture_context) {
  bool result = wl_display_dispatch(capture_context->wl_display) != -1;
  if (!result) LOG("Failed to dispatch wl_display (%s)", strerror(errno));
  return result;
}

void CaptureContextWlrDestroy(struct CaptureContextWlr* capture_context) {
  if (capture_context->zwlr_export_dmabuf_frame_v1)
    zwlr_export_dmabuf_frame_v1_destroy(
        capture_context->zwlr_export_dmabuf_frame_v1);
  DeinitWaylandGlobals(capture_context);
  free(capture_context);
}

#endif  // USE_WAYLAND
