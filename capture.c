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
#include <xf86drmMode.h>

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

static bool IsCrtcComplete(int drm_fd, uint32_t crtc_id) {
  drmModeCrtcPtr crtc = drmModeGetCrtc(drm_fd, crtc_id);
  if (!crtc) {
    LOG("Failed to get crtc %u (%s)", crtc_id, strerror(errno));
    return false;
  }
  bool result = false;
  if (!crtc->buffer_id) {
    LOG("Crtc %u has no framebuffer", crtc_id);
    goto rollback_crtc;
  }

  drmModeFB2Ptr fb2 = drmModeGetFB2(drm_fd, crtc->buffer_id);
  if (!fb2) {
    LOG("Failed to get framebuffer %u (%s)", crtc->buffer_id, strerror(errno));
    goto rollback_crtc;
  }
  if (!fb2->handles[0]) {
    LOG("Framebuffer %u has no handles", crtc->buffer_id);
    goto rollback_fb2;
  }
  result = true;

rollback_fb2:
  drmModeFreeFB2(fb2);
rollback_crtc:
  drmModeFreeCrtc(crtc);
  return result;
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

  drmModeResPtr res = drmModeGetResources(capture_context->drm_fd);
  if (!res) {
    LOG("Failed to get drm mode resources (%s)", strerror(errno));
    goto rollback_drm_fd;
  }

  for (int i = 0; i < res->count_crtcs; i++) {
    if (IsCrtcComplete(capture_context->drm_fd, res->crtcs[i])) {
      LOG("Capturing crtc %u", res->crtcs[i]);
      capture_context->crtc_id = res->crtcs[i];
      drmModeFreeResources(res);
      return capture_context;
    }
  }

  LOG("Nothing to capture");
  drmModeFreeResources(res);

rollback_drm_fd:
  drmClose(capture_context->drm_fd);
rollback_capture_context:
  free(capture_context);
  return NULL;
}

static struct GpuFrame* WrapFramebuffer(int drm_fd, drmModeFB2Ptr fb2,
                                        struct GpuContext* gpu_context) {
  size_t nplanes = 0;
  struct GpuFrame* result = NULL;
  struct GpuFramePlane planes[LENGTH(fb2->handles)];
  for (; nplanes < LENGTH(planes) && fb2->handles[nplanes]; nplanes++) {
    int status = drmPrimeHandleToFD(drm_fd, fb2->handles[nplanes], 0,
                                    &planes[nplanes].dmabuf_fd);
    if (status) {
      LOG("Failed to get dmabuf fd (%d)", status);
      goto release_planes;
    }
    planes[nplanes].offset = fb2->offsets[nplanes];
    planes[nplanes].pitch = fb2->pitches[nplanes];
    // TODO(mburakov): Structure of drmModeFB2 implies that all the planes have
    // the same modifier. At the same time, surrounding code supports per-plane
    // modifiers. So right now a drmModeFB2-wide modifier is just copypasted
    // into each plane descriptor.
    planes[nplanes].modifier = fb2->modifier;
  }

  result = GpuFrameCreate(gpu_context, fb2->width, fb2->height,
                          fb2->pixel_format, nplanes, planes);
  if (!result) LOG("Failed to create gpu frame");

release_planes:
  for (; nplanes; nplanes--) close(planes[nplanes - 1].dmabuf_fd);
  return result;
}

const struct GpuFrame* CaptureContextGetFrame(
    struct CaptureContext* capture_context) {
  drmModeCrtcPtr crtc =
      drmModeGetCrtc(capture_context->drm_fd, capture_context->crtc_id);
  if (!crtc) {
    LOG("Failed to get crtc %u", capture_context->crtc_id);
    return NULL;
  }

  struct GpuFrame* gpu_frame = NULL;
  drmModeFB2Ptr fb2 = drmModeGetFB2(capture_context->drm_fd, crtc->buffer_id);
  if (!fb2) {
    LOG("Failed to get framebuffer %u", crtc->buffer_id);
    goto rollback_crtc;
  }
  if (!fb2->handles[0]) {
    LOG("Framebuffer %u has no handles", crtc->buffer_id);
    goto rollback_fb2;
  }

  if (capture_context->gpu_frame) GpuFrameDestroy(capture_context->gpu_frame);
  capture_context->gpu_frame = WrapFramebuffer(capture_context->drm_fd, fb2,
                                               capture_context->gpu_context);
  gpu_frame = capture_context->gpu_frame;

rollback_fb2:
  drmModeFreeFB2(fb2);
rollback_crtc:
  drmModeFreeCrtc(crtc);
  return gpu_frame;
}

void CaptureContextDestroy(struct CaptureContext* capture_context) {
  GpuFrameDestroy(capture_context->gpu_frame);
  drmClose(capture_context->drm_fd);
  free(capture_context);
}
