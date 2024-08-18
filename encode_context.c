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

#include "encode_context.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include "io_context.h"
#include "util.h"

struct EncodeContext {
  struct IoContext* io_context;
  size_t width;
  size_t height;
};

struct EncodeContext* EncodeContextCreate(struct IoContext* io_context,
                                          uint32_t width, uint32_t height) {
  LOG("Initializing encoder context for %ux%u resolution", width, height);
  struct EncodeContext* encode_context = malloc(sizeof(struct EncodeContext));
  if (!encode_context) {
    LOG("Failed to allocate encode context (%s)", strerror(errno));
    return NULL;
  }

  *encode_context = (struct EncodeContext){
      .io_context = io_context,
      .width = width,
      .height = height,
  };

  return encode_context;
}

struct EncodeContextFrame* EncodeContextDequeue(
    struct EncodeContext* encode_context) {
  (void)encode_context;
  // TODO(mburakov): Implement this!
  return NULL;
}

bool EncodeContextQueue(struct EncodeContext* encode_context,
                        struct EncodeContextFrame* encode_context_frame,
                        bool encode) {
  (void)encode_context;
  (void)encode_context_frame;
  (void)encode;
  // TODO(mburakov): Implement this!
  return true;
}

void EncodeContextDestroy(struct EncodeContext* encode_context) {
  free(encode_context);
}
