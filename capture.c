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

#include "capture_kms.h"
#include "capture_wlr.h"
#include "toolbox/utils.h"

struct CaptureContext {
  struct CaptureContextKms* kms;
  struct CaptureContextWlr* wlr;
};

struct CaptureContext* CaptureContextCreate(
    struct GpuContext* gpu_context,
    const struct CaptureContextCallbacks* callbacks, void* user) {
  struct CaptureContext* capture_context =
      calloc(1, sizeof(struct CaptureContext));
  if (!capture_context) {
    LOG("Failed to allocate capture context (%s)", strerror(errno));
    return NULL;
  }

  capture_context->kms = CaptureContextKmsCreate(gpu_context, callbacks, user);
  if (capture_context->kms) return capture_context;

  LOG("Failed to create kms capture context");
  capture_context->wlr = CaptureContextWlrCreate(gpu_context, callbacks, user);
  if (capture_context->wlr) return capture_context;

  LOG("Failed to create wlr capture context");
  free(capture_context);
  return NULL;
}

int CaptureContextGetEventsFd(struct CaptureContext* capture_context) {
  if (capture_context->kms)
    return CaptureContextKmsGetEventsFd(capture_context->kms);
  if (capture_context->wlr)
    return CaptureContextWlrGetEventsFd(capture_context->wlr);
  return -1;
}

bool CaptureContextProcessEvents(struct CaptureContext* capture_context) {
  if (capture_context->kms)
    return CaptureContextKmsProcessEvents(capture_context->kms);
  if (capture_context->wlr)
    return CaptureContextWlrProcessEvents(capture_context->wlr);
  return false;
}

void CaptureContextDestroy(struct CaptureContext* capture_context) {
  if (capture_context->wlr) CaptureContextWlrDestroy(capture_context->wlr);
  if (capture_context->kms) CaptureContextKmsDestroy(capture_context->kms);
  free(capture_context);
}
