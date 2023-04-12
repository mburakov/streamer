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

#include "capture.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "gpu.h"
#include "toolbox/utils.h"

struct CaptureContext {
  struct GpuContext* gpu_context;
  int drm_fd;
  uint32_t crtc_id;
  struct GpuFrame* gpu_frame;
};

static int OpenAnyModule(void) {
  static const char* const modules[] = {
      "i915",      "amdgpu",     "radeon",     "nouveau",     "vmwgfx",
      "omapdrm",   "exynos",     "tilcdc",     "msm",         "sti",
      "tegra",     "imx-drm",    "rockchip",   "atmel-hlcdc", "fsl-dcu-drm",
      "vc4",       "virtio_gpu", "mediatek",   "meson",       "pl111",
      "stm",       "sun4i-drm",  "armada-drm", "komeda",      "imx-dcss",
      "mxsfb-drm", "simpledrm",  "imx-lcdif",  "vkms",
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

struct CaptureContext* CaptureContextCreate(struct GpuContext* gpu_context) {
  struct CaptureContext* capture_context =
      malloc(sizeof(struct CaptureContext));
  if (!capture_context) {
    LOG("Failed to allocate capture context (%s)", strerror(errno));
    return NULL;
  }
  *capture_context = (struct CaptureContext){
      .gpu_context = gpu_context,
      .drm_fd = -1,
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
      return capture_context;
    }
  }
  LOG("Nothing to capture");

rollback_drm_fd:
  drmClose(capture_context->drm_fd);
rollback_capture_context:
  free(capture_context);
  return NULL;
}

const struct GpuFrame* CaptureContextGetFrame(
    struct CaptureContext* capture_context) {
  struct drm_mode_fb_cmd2 drm_mode_fb_cmd2;
  if (!GetCrtcFb(capture_context->drm_fd, capture_context->crtc_id,
                 &drm_mode_fb_cmd2))
    return NULL;

  if (capture_context->gpu_frame) {
    GpuFrameDestroy(capture_context->gpu_frame);
    capture_context->gpu_frame = NULL;
  }

  size_t nplanes = 0;
  struct GpuFramePlane planes[LENGTH(drm_mode_fb_cmd2.handles)];
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

  capture_context->gpu_frame = GpuFrameCreate(
      capture_context->gpu_context, drm_mode_fb_cmd2.width,
      drm_mode_fb_cmd2.height, drm_mode_fb_cmd2.pixel_format, nplanes, planes);

release_planes:
  for (; nplanes; nplanes--) close(planes[nplanes - 1].dmabuf_fd);
  return capture_context->gpu_frame;
}

void CaptureContextDestroy(struct CaptureContext* capture_context) {
  if (capture_context->gpu_frame) GpuFrameDestroy(capture_context->gpu_frame);
  drmClose(capture_context->drm_fd);
  free(capture_context);
}
