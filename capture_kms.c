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

#include "capture_kms.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

#include "gpu.h"
#include "toolbox/utils.h"

static const int kCapturePeriod = 1000000000 / 60;

struct CaptureContextKms {
  struct IoMuxer* io_muxer;
  struct GpuContext* gpu_context;
  const struct CaptureContextCallbacks* callbacks;
  void* user;

  int drm_fd;
  uint32_t crtc_id;
  int timer_fd;
};

static int OpenAnyModule(void) {
  static const char* const modules[] = {
      "i915",
      "amdgpu",
  };
  for (size_t i = 0; i < LENGTH(modules); i++) {
    int drm_fd = drmOpen(modules[i], NULL);
    if (drm_fd >= 0) return drm_fd;
    LOG("Failed to open %s (%s)", modules[i], strerror(errno));
  }
  return -1;
}

static bool GetCrtcFb(int drm_fd, uint32_t crtc_id,
                      struct drm_mode_fb_cmd2* drm_mode_fb_cmd2) {
  struct drm_mode_crtc drm_mode_crtc = {
      .crtc_id = crtc_id,
  };
  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &drm_mode_crtc)) {
    LOG("Failed to get crtc %u (%s)", crtc_id, strerror(errno));
    return false;
  }
  if (!drm_mode_crtc.fb_id) {
    LOG("Crtc %u has no framebuffer", crtc_id);
    return false;
  }

  struct drm_mode_fb_cmd2 result = {
      .fb_id = drm_mode_crtc.fb_id,
  };
  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_GETFB2, &result)) {
    LOG("Failed to get framebuffer %u (%s)", drm_mode_crtc.fb_id,
        strerror(errno));
    return false;
  }
  if (!result.handles[0]) {
    LOG("Framebuffer %u has no handles", drm_mode_crtc.fb_id);
    return false;
  }

  if (drm_mode_fb_cmd2) *drm_mode_fb_cmd2 = result;
  return true;
}

struct CaptureContextKms* CaptureContextKmsCreate(
    struct GpuContext* gpu_context,
    const struct CaptureContextCallbacks* callbacks, void* user) {
  struct CaptureContextKms* capture_context =
      malloc(sizeof(struct CaptureContextKms));
  if (!capture_context) {
    LOG("Failed to allocate capture context (%s)", strerror(errno));
    return NULL;
  }
  *capture_context = (struct CaptureContextKms){
      .gpu_context = gpu_context,
      .callbacks = callbacks,
      .user = user,
      .drm_fd = -1,
      .timer_fd = -1,
  };

  capture_context->drm_fd = OpenAnyModule();
  if (capture_context->drm_fd == -1) {
    LOG("Failed to open any module");
    goto rollback_capture_context;
  }

  uint32_t crtc_ids[16];
  struct drm_mode_card_res drm_mode_card_res = {
      .crtc_id_ptr = (uintptr_t)crtc_ids,
      .count_crtcs = LENGTH(crtc_ids),
  };
  if (drmIoctl(capture_context->drm_fd, DRM_IOCTL_MODE_GETRESOURCES,
               &drm_mode_card_res)) {
    LOG("Failed to get drm mode resources (%s)", strerror(errno));
    goto rollback_drm_fd;
  }
  for (size_t i = 0; i < drm_mode_card_res.count_crtcs; i++) {
    if (GetCrtcFb(capture_context->drm_fd, crtc_ids[i], NULL)) {
      LOG("Capturing crtc %u", crtc_ids[i]);
      capture_context->crtc_id = crtc_ids[i];
      break;
    }
  }
  if (!capture_context->crtc_id) {
    LOG("Nothing to capture");
    goto rollback_drm_fd;
  }

  capture_context->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (capture_context->timer_fd == -1) {
    LOG("Failed to create timer (%s)", strerror(errno));
    goto rollback_drm_fd;
  }
  static const struct itimerspec kTimerSpec = {
      .it_interval.tv_nsec = kCapturePeriod,
      .it_value.tv_nsec = kCapturePeriod,
  };
  if (timerfd_settime(capture_context->timer_fd, 0, &kTimerSpec, NULL)) {
    LOG("Failed to arm timer (%s)", strerror(errno));
    goto rollback_timer_fd;
  }
  return capture_context;

rollback_timer_fd:
  close(capture_context->timer_fd);
rollback_drm_fd:
  drmClose(capture_context->drm_fd);
rollback_capture_context:
  free(capture_context);
  return NULL;
}

int CaptureContextKmsGetEventsFd(struct CaptureContextKms* capture_context) {
  return capture_context->timer_fd;
}

bool CaptureContextKmsProcessEvents(struct CaptureContextKms* capture_context) {
  uint64_t expirations;
  if (read(capture_context->timer_fd, &expirations, sizeof(expirations)) !=
      sizeof(expirations)) {
    LOG("Failed to read timer expirations (%s)", strerror(errno));
    return false;
  }

  struct drm_mode_fb_cmd2 drm_mode_fb_cmd2;
  if (!GetCrtcFb(capture_context->drm_fd, capture_context->crtc_id,
                 &drm_mode_fb_cmd2)) {
    return false;
  }

  struct GpuFramePlane planes[] = {
      {.dmabuf_fd = -1},
      {.dmabuf_fd = -1},
      {.dmabuf_fd = -1},
      {.dmabuf_fd = -1},
  };
  static_assert(LENGTH(planes) == LENGTH(drm_mode_fb_cmd2.handles),
                "Suspicious drm_mode_fb_cmd2 structure");

  size_t nplanes = 0;
  for (; nplanes < LENGTH(planes); nplanes++) {
    if (!drm_mode_fb_cmd2.handles[nplanes]) break;
    int status = drmPrimeHandleToFD(capture_context->drm_fd,
                                    drm_mode_fb_cmd2.handles[nplanes], 0,
                                    &planes[nplanes].dmabuf_fd);
    if (status) {
      LOG("Failed to get dmabuf fd (%d)", status);
      goto release_planes;
    }
    planes[nplanes].offset = drm_mode_fb_cmd2.offsets[nplanes];
    planes[nplanes].pitch = drm_mode_fb_cmd2.pitches[nplanes];
    planes[nplanes].modifier = drm_mode_fb_cmd2.modifier[nplanes];
  }

  struct GpuFrame* gpu_frame = GpuContextCreateFrame(
      capture_context->gpu_context, drm_mode_fb_cmd2.width,
      drm_mode_fb_cmd2.height, drm_mode_fb_cmd2.pixel_format, nplanes, planes);
  if (!gpu_frame) {
    LOG("Failed to create gpu frame");
    goto release_planes;
  }

  // mburakov: Capture context might get destroyed in callback.
  struct GpuContext* gpu_context = capture_context->gpu_context;
  capture_context->callbacks->OnFrameReady(capture_context->user, gpu_frame);
  GpuContextDestroyFrame(gpu_context, gpu_frame);
  return true;

release_planes:
  CloseUniqueFds((int[]){planes[0].dmabuf_fd, planes[1].dmabuf_fd,
                         planes[2].dmabuf_fd, planes[3].dmabuf_fd});
  return false;
}

void CaptureContextKmsDestroy(struct CaptureContextKms* capture_context) {
  close(capture_context->timer_fd);
  drmClose(capture_context->drm_fd);
  free(capture_context);
}
