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
#include "util.h"

struct Framebuffer {
  uint32_t framebuffer_id;
  struct GpuFrame* gpu_frame;
};

struct CaptureContext {
  struct GpuContext* gpu_context;
  int drm_fd;
  uint32_t crtc_id;
  size_t framebuffers_count;
  struct Framebuffer* framebuffers;
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

static void drmModeResPtrDestroy(drmModeResPtr* res) {
  if (res && *res) drmModeFreeResources(*res);
}

static void drmModeCrtcPtrDestroy(drmModeCrtcPtr* crtc) {
  if (crtc && *crtc) drmModeFreeCrtc(*crtc);
}

static void drmModeFB2PtrDestroy(drmModeFB2Ptr* fb2) {
  if (fb2 && *fb2) drmModeFreeFB2(*fb2);
}

static bool IsCrtcComplete(int drm_fd, uint32_t crtc_id) {
  AUTO(drmModeCrtcPtr)
  crtc = drmModeGetCrtc(drm_fd, crtc_id);
  if (!crtc) {
    LOG("Failed to get crtc %u", crtc_id);
    return false;
  }
  if (!crtc->buffer_id) {
    LOG("Crtc %u has no framebuffer", crtc_id);
    return false;
  }

  AUTO(drmModeFB2Ptr)
  fb2 = drmModeGetFB2(drm_fd, crtc->buffer_id);
  if (!fb2) {
    LOG("Failed to get framebuffer %u", crtc->buffer_id);
    return false;
  }
  if (!fb2->handles[0]) {
    LOG("Framebuffer %u has no handles", crtc->buffer_id);
    return false;
  }
  return true;
}

struct CaptureContext* CaptureContextCreate(struct GpuContext* gpu_context) {
  struct AUTO(CaptureContext)* capture_context =
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
  if (capture_context->drm_fd == -1) return NULL;

  AUTO(drmModeResPtr) res = drmModeGetResources(capture_context->drm_fd);
  if (!res) {
    LOG("Failed to get drm mode resources (%s)", strerror(errno));
    return NULL;
  }

  for (int i = 0; i < res->count_crtcs; i++) {
    if (IsCrtcComplete(capture_context->drm_fd, res->crtcs[i])) {
      LOG("Capturing crtc %u", res->crtcs[i]);
      capture_context->crtc_id = res->crtcs[i];
      return RELEASE(capture_context);
    }
  }

  LOG("Nothing to capture");
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
  AUTO(drmModeCrtcPtr)
  crtc = drmModeGetCrtc(capture_context->drm_fd, capture_context->crtc_id);
  if (!crtc) {
    LOG("Failed to get crtc %u", capture_context->crtc_id);
    return NULL;
  }

  for (size_t i = 0; i < capture_context->framebuffers_count; i++) {
    // TODO(mburakov): Verify nothing changed since last frame.
    if (capture_context->framebuffers[i].framebuffer_id == crtc->buffer_id)
      return capture_context->framebuffers[i].gpu_frame;
  }

  AUTO(drmModeFB2Ptr)
  fb2 = drmModeGetFB2(capture_context->drm_fd, crtc->buffer_id);
  if (!fb2) {
    LOG("Failed to get framebuffer %u", crtc->buffer_id);
    return NULL;
  }
  if (!fb2->handles[0]) {
    LOG("Framebuffer %u has no handles", crtc->buffer_id);
    return NULL;
  }

  struct AUTO(GpuFrame)* gpu_frame = WrapFramebuffer(
      capture_context->drm_fd, fb2, capture_context->gpu_context);
  if (!gpu_frame) return NULL;
  size_t framebuffers_count = capture_context->framebuffers_count + 1;
  struct Framebuffer* framebuffers =
      realloc(capture_context->framebuffers,
              framebuffers_count * sizeof(struct Framebuffer));
  if (!framebuffers) {
    LOG("Failed to reallocate framebuffers (%s)", strerror(errno));
    return NULL;
  }

  framebuffers[capture_context->framebuffers_count] = (struct Framebuffer){
      .framebuffer_id = crtc->buffer_id,
      .gpu_frame = gpu_frame,
  };
  capture_context->framebuffers_count = framebuffers_count;
  capture_context->framebuffers = framebuffers;
  return RELEASE(gpu_frame);
}

void CaptureContextDestroy(struct CaptureContext** capture_context) {
  if (!capture_context || !*capture_context) return;
  for (size_t i = 0; i < (*capture_context)->framebuffers_count; i++)
    GpuFrameDestroy(&(*capture_context)->framebuffers[i].gpu_frame);
  if ((*capture_context)->framebuffers) free((*capture_context)->framebuffers);
  if ((*capture_context)->drm_fd != -1) drmClose((*capture_context)->drm_fd);
  free(*capture_context);
  capture_context = NULL;
}
