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

#ifndef STREAMER_GPU_H_
#define STREAMER_GPU_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "colorspace.h"

struct GpuFramePlane {
  int dmabuf_fd;
  uint32_t pitch;
  uint32_t offset;
  uint64_t modifier;
};

struct GpuContext* GpuContextCreate(enum YuvColorspace colorspace,
                                    enum YuvRange range);
bool GpuContextSync(struct GpuContext* gpu_context);
void GpuContextDestroy(struct GpuContext** gpu_context);

struct GpuFrame* GpuFrameCreate(struct GpuContext* gpu_context, uint32_t width,
                                uint32_t height, uint32_t fourcc,
                                size_t nplanes,
                                const struct GpuFramePlane* planes);
void GpuFrameGetSize(const struct GpuFrame* gpu_frame, uint32_t* width,
                     uint32_t* height);
bool GpuFrameConvert(const struct GpuFrame* from, const struct GpuFrame* to);
void GpuFrameDestroy(struct GpuFrame** gpu_frame);

#endif  // STREAMER_GPU_H_
