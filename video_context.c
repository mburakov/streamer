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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <pipewire/pipewire.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <wayland-client.h>

#include "encode_context.h"
#include "gpu_context.h"
#include "util.h"
#include "wlr-export-dmabuf-unstable-v1.h"

struct EGLAttribPair {
  EGLAttrib key;
  EGLAttrib value;
};

struct VideoContext {
  struct IoContext* io_context;
  struct EncodeContext* encode_context;

  // Wayland globals
  struct wl_display* display;
  struct wl_registry* registry;
  struct wl_output* output;
  struct zwlr_export_dmabuf_manager_v1* export_dmabuf_manager;

  // Colorspace conversion
  struct GpuContext* gpu_context;
  struct GpuContextImage** imported_images;
  size_t imported_images_count;

  // Threading
  struct pw_thread_loop* thread_loop;
  struct spa_source* source;

  // Volatile state
  struct {
    struct EGLAttribPair height;
    struct EGLAttribPair width;
    struct EGLAttribPair linux_drm_fourcc;
    struct {
      struct EGLAttribPair fd;
      struct EGLAttribPair offset;
      struct EGLAttribPair pitch;
      struct EGLAttribPair modifier_lo;
      struct EGLAttribPair modifier_hi;
    } dma_buf_plane[4];
    EGLAttrib terminator;
  } attrib_list;
};

static void SetEGLAttribPair(struct EGLAttribPair* pair, EGLAttrib key,
                             EGLAttrib value) {
  *pair = (struct EGLAttribPair){
      .key = key,
      .value = value,
  };
}

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

static void OnExportDmabufFrameFrame(
    void* data, struct zwlr_export_dmabuf_frame_v1* export_dmabuf_frame,
    uint32_t width, uint32_t height, uint32_t offset_x, uint32_t offset_y,
    uint32_t buffer_flags, uint32_t flags, uint32_t format, uint32_t mod_high,
    uint32_t mod_low, uint32_t num_objects) {
  (void)export_dmabuf_frame;
  (void)flags;

  struct VideoContext* video_context = data;
  if (!video_context->encode_context) {
    video_context->encode_context =
        EncodeContextCreate(video_context->io_context, width, height);
    if (!video_context->encode_context) {
      LOG("Failed to create encode context");
      // TODO(mburakov): Now what?..
    }
  }

  // TODO(mburakov): Maybe handle those?
  assert(!offset_x && !offset_y && !buffer_flags);
  SetEGLAttribPair(&video_context->attrib_list.height, EGL_HEIGHT, height);
  SetEGLAttribPair(&video_context->attrib_list.width, EGL_WIDTH, width);
  SetEGLAttribPair(&video_context->attrib_list.linux_drm_fourcc,
                   EGL_LINUX_DRM_FOURCC_EXT, format);

  assert(num_objects <= LENGTH(video_context->attrib_list.dma_buf_plane));
  for (EGLAttrib index = 0; index < num_objects; index++) {
    typeof(&video_context->attrib_list.dma_buf_plane[index]) plane =
        &video_context->attrib_list.dma_buf_plane[index];
    SetEGLAttribPair(&plane->modifier_lo,
                     EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT + index * 2, mod_low);
    SetEGLAttribPair(&plane->modifier_hi,
                     EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT + index * 2, mod_high);
  }
}

static void OnExportDmabufFrameObject(
    void* data, struct zwlr_export_dmabuf_frame_v1* export_dmabuf_frame,
    uint32_t index, int32_t fd, uint32_t size, uint32_t offset, uint32_t stride,
    uint32_t plane_index) {
  (void)export_dmabuf_frame;
  (void)size;

  assert(index == plane_index);
  struct VideoContext* video_context = data;

  SetEGLAttribPair(&video_context->attrib_list.dma_buf_plane[index].fd,
                   EGL_DMA_BUF_PLANE0_FD_EXT + (EGLAttrib)index * 3, fd);
  SetEGLAttribPair(&video_context->attrib_list.dma_buf_plane[index].offset,
                   EGL_DMA_BUF_PLANE0_OFFSET_EXT + (EGLAttrib)index * 3,
                   offset);
  SetEGLAttribPair(&video_context->attrib_list.dma_buf_plane[index].pitch,
                   EGL_DMA_BUF_PLANE0_PITCH_EXT + (EGLAttrib)index * 3, stride);
}

static void ResetAttribList(struct VideoContext* video_context) {
  SetEGLAttribPair(&video_context->attrib_list.height, EGL_NONE, EGL_NONE);
  SetEGLAttribPair(&video_context->attrib_list.width, EGL_NONE, EGL_NONE);
  SetEGLAttribPair(&video_context->attrib_list.linux_drm_fourcc, EGL_NONE,
                   EGL_NONE);

  static const EGLAttrib kMaxPlanesCount =
      LENGTH(video_context->attrib_list.dma_buf_plane);
  for (EGLAttrib index = 0; index < kMaxPlanesCount; index++) {
    typeof(&video_context->attrib_list.dma_buf_plane[index]) plane =
        &video_context->attrib_list.dma_buf_plane[index];
    if (plane->fd.value != -1) {
      assert(!close((int)plane->fd.value));
    }
    SetEGLAttribPair(&plane->fd, EGL_NONE, -1);
    SetEGLAttribPair(&plane->offset, EGL_NONE, EGL_NONE);
    SetEGLAttribPair(&plane->pitch, EGL_NONE, EGL_NONE);
    SetEGLAttribPair(&plane->modifier_hi, EGL_NONE, EGL_NONE);
    SetEGLAttribPair(&plane->modifier_lo, EGL_NONE, EGL_NONE);
  }

  video_context->attrib_list.terminator = EGL_NONE;
}

static struct GpuContextImage* ImportEncodeContextFrame(
    struct VideoContext* video_context,
    struct EncodeContextFrame* encode_context_frame) {
  static_assert(LENGTH(encode_context_frame->planes) == 2,
                "Suspicious amount of imported frame planes");
  struct GpuContextImage* imported_frame_planes =
      malloc(2 * sizeof(struct GpuContextImage));
  if (!imported_frame_planes) {
    LOG("Failed to allocate imported frame planes (%s)", strerror(errno));
    return NULL;
  }

  EGLAttrib attrib_list_luma[] = {
#define _(...) __VA_ARGS__
      _(EGL_HEIGHT, video_context->attrib_list.height.value),
      _(EGL_WIDTH, video_context->attrib_list.width.value),
      _(EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8),
      _(EGL_DMA_BUF_PLANE0_FD_EXT, encode_context_frame->planes[0].fd),
      _(EGL_DMA_BUF_PLANE0_OFFSET_EXT, encode_context_frame->planes[0].offset),
      _(EGL_DMA_BUF_PLANE0_PITCH_EXT, encode_context_frame->planes[0].pitch),
      EGL_NONE,
#undef _
  };
  if (GpuContextCreateImage(video_context->gpu_context, attrib_list_luma,
                            &imported_frame_planes[0])) {
    LOG("Failed to import luma frame plane");
    goto rollback_imported_frame_planes;
  }

  EGLAttrib attrib_list_chroma[] = {
#define _(...) __VA_ARGS__
      _(EGL_HEIGHT, video_context->attrib_list.height.value / 2),
      _(EGL_WIDTH, video_context->attrib_list.width.value / 2),
      _(EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88),
      _(EGL_DMA_BUF_PLANE0_FD_EXT, encode_context_frame->planes[1].fd),
      _(EGL_DMA_BUF_PLANE0_OFFSET_EXT, encode_context_frame->planes[1].offset),
      _(EGL_DMA_BUF_PLANE0_PITCH_EXT, encode_context_frame->planes[1].pitch),
      EGL_NONE,
#undef _
  };
  if (GpuContextCreateImage(video_context->gpu_context, attrib_list_chroma,
                            &imported_frame_planes[1])) {
    LOG("Failed to import chroma frame plane");
    goto rollback_luma_plane;
  }

  size_t imported_images_count = video_context->imported_images_count + 1;
  struct GpuContextImage** imported_images =
      realloc(video_context->imported_images,
              imported_images_count * sizeof(struct GpuContextCreateImage*));
  if (!imported_images) {
    LOG("Failed to reallocate imported images list (%s)", strerror(errno));
    goto rollback_chroma_plane;
  }

  imported_images[video_context->imported_images_count] = imported_frame_planes;
  video_context->imported_images = imported_images;
  video_context->imported_images_count = imported_images_count;
  return imported_frame_planes;

rollback_chroma_plane:
  GpuContextDestroyImage(video_context->gpu_context, &imported_frame_planes[1]);
rollback_luma_plane:
  GpuContextDestroyImage(video_context->gpu_context, &imported_frame_planes[0]);
rollback_imported_frame_planes:
  free(imported_frame_planes);
  return NULL;
}

static void OnExportDmabufFrameReady(
    void* data, struct zwlr_export_dmabuf_frame_v1* export_dmabuf_frame,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
  struct VideoContext* video_context = data;
  struct GpuContextImage source_image;
  if (!GpuContextCreateImage(video_context->gpu_context,
                             (EGLAttrib*)&video_context->attrib_list,
                             &source_image)) {
    LOG("Failed to import Wayland frame");
    goto rollback_attrib_list;
  }

  struct EncodeContextFrame* encode_context_frame =
      EncodeContextDequeue(video_context->encode_context);
  if (!encode_context_frame) {
    LOG("Failed to dequeue encode context frame");
    // TODO(mburakov): Now what?..
    goto rollback_source_image;
  }

  struct GpuContextImage* target_images = encode_context_frame->user_data;
  if (!target_images) {
    encode_context_frame->user_data =
        ImportEncodeContextFrame(video_context, encode_context_frame);
    target_images = encode_context_frame->user_data;
    if (!target_images) {
      LOG("Failed to import encode context frame");
      // TODO(mburakov): Now what?..
      goto rollback_encode_context_frame;
    }
  }

  if (!GpuContextConvertColorspace(
          video_context->gpu_context, video_context->attrib_list.width.value,
          video_context->attrib_list.height.value, source_image.gl_texture,
          target_images[0].gl_texture, target_images[1].gl_texture)) {
    LOG("Failed to convert Wayland frame colorspace");
    // TODO(mburakov): Now what?..
    goto rollback_encode_context_frame;
  }

  if (!EncodeContextQueue(video_context->encode_context, encode_context_frame,
                          true)) {
    LOG("Failed to encode video frame");
    // TODO(mburakov): Now what?
    goto rollback_encode_context_frame;
  }

  goto rollback_source_image;

rollback_encode_context_frame:
  assert(EncodeContextQueue(video_context->encode_context, encode_context_frame,
                            false));
rollback_source_image:
  GpuContextDestroyImage(video_context->gpu_context, &source_image);
rollback_attrib_list:
  ResetAttribList(video_context);
  zwlr_export_dmabuf_frame_v1_destroy(export_dmabuf_frame);
}

static void OnExportDmabufFrameCancel(
    void* data, struct zwlr_export_dmabuf_frame_v1* export_dmabuf_frame,
    uint32_t reason) {
  (void)data;
  static const char* const kCancelReasons[] = {
      "temporary",
      "permanent",
      "resizing",
  };
  assert(reason < LENGTH(kCancelReasons));
  LOG("Capturing is cancelled (%s)", kCancelReasons[reason]);
  struct VideoContext* video_context = data;
  ResetAttribList(video_context);
  zwlr_export_dmabuf_frame_v1_destroy(export_dmabuf_frame);
}

static int RequestCapture(struct spa_loop* loop, bool async, uint32_t seq,
                          const void* data, size_t size, void* user_data) {
  (void)loop;
  (void)async;
  (void)seq;
  (void)data;
  (void)size;
  struct VideoContext* video_context = user_data;
  struct zwlr_export_dmabuf_frame_v1* export_dmabuf_frame =
      zwlr_export_dmabuf_manager_v1_capture_output(
          video_context->export_dmabuf_manager, 1, video_context->output);
  if (!export_dmabuf_frame) {
    LOG("Failed to capture output");
    // TODO(mburakov): Now what?..
    return 0;
  }

  static const struct zwlr_export_dmabuf_frame_v1_listener
      kExportDmabufFrameListener = {
          .frame = OnExportDmabufFrameFrame,
          .object = OnExportDmabufFrameObject,
          .ready = OnExportDmabufFrameReady,
          .cancel = OnExportDmabufFrameCancel,
      };
  if (zwlr_export_dmabuf_frame_v1_add_listener(
          export_dmabuf_frame, &kExportDmabufFrameListener, video_context)) {
    LOG("Failed to add frame listener");
    goto rollback_export_dmabuf_frame;
  }

  if (wl_display_flush(video_context->display) == -1) {
    LOG("Failed to flush display");
    goto rollback_export_dmabuf_frame;
  }

  return 0;

rollback_export_dmabuf_frame:
  zwlr_export_dmabuf_frame_v1_destroy(export_dmabuf_frame);
  // TODO(mburakov): Now what?..
  return 0;
}

struct VideoContext* VideoContextCreate(struct IoContext* io_context) {
  struct VideoContext* video_context = malloc(sizeof(struct VideoContext));
  if (!video_context) {
    LOG("Failed to allocate video context (%s)", strerror(errno));
    return NULL;
  }

  *video_context = (struct VideoContext){
      .io_context = io_context,
      .attrib_list.dma_buf_plane[0].fd.value = -1,
      .attrib_list.dma_buf_plane[1].fd.value = -1,
      .attrib_list.dma_buf_plane[2].fd.value = -1,
      .attrib_list.dma_buf_plane[3].fd.value = -1,
  };
  if (!InitWaylandGlobals(video_context)) {
    LOG("Failed to init Wayland globals");
    goto rollback_video_context;
  }

  video_context->gpu_context = GpuContextCreate(video_context->display);
  if (!video_context->gpu_context) {
    LOG("Failed to create gpu context");
    goto rollback_wayland_globals;
  }

  video_context->thread_loop = pw_thread_loop_new("video-capture", NULL);
  if (!video_context->thread_loop) {
    LOG("Failed to create thread loop");
    goto rollback_gpu_context;
  }

  pw_thread_loop_lock(video_context->thread_loop);
  if (pw_thread_loop_start(video_context->thread_loop)) {
    LOG("Failed to start thread loop");
    goto rollback_thread_loop;
  }

  struct pw_loop* loop = pw_thread_loop_get_loop(video_context->thread_loop);
  if (!loop) {
    LOG("Failed to get thread loop");
    goto rollback_thread_loop;
  }

  int events_fd = wl_display_get_fd(video_context->display);
  if (events_fd == -1) {
    LOG("Failed to get display fd");
    goto rollback_thread_loop;
  }

  video_context->source = pw_loop_add_io(loop, events_fd, SPA_IO_IN, false,
                                         OnDisplayData, video_context);
  if (!video_context->source) {
    LOG("Failed to add thread loop io");
    goto rollback_thread_loop;
  }

  if (pw_loop_invoke(loop, RequestCapture, SPA_ID_INVALID, NULL, 0, false,
                     video_context)) {
    LOG("Failed to request capture");
    goto rollback_thread_loop;
  }

  ResetAttribList(video_context);
  pw_thread_loop_unlock(video_context->thread_loop);
  return video_context;

rollback_thread_loop:
  pw_thread_loop_unlock(video_context->thread_loop);
  pw_thread_loop_destroy(video_context->thread_loop);
rollback_gpu_context:
  GpuContextDestroy(video_context->gpu_context);
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
  for (size_t index = 0; index < video_context->imported_images_count;
       index++) {
    GpuContextDestroyImage(video_context->gpu_context,
                           &video_context->imported_images[index][0]);
    GpuContextDestroyImage(video_context->gpu_context,
                           &video_context->imported_images[index][1]);
    free(video_context->imported_images[index]);
  }
  free(video_context->imported_images);
  GpuContextDestroy(video_context->gpu_context);
  zwlr_export_dmabuf_manager_v1_destroy(video_context->export_dmabuf_manager);
  wl_output_release(video_context->output);
  wl_registry_destroy(video_context->registry);
  wl_display_disconnect(video_context->display);
  free(video_context);
}
