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

#ifndef STREAMER_ENCODE_H_
#define STREAMER_ENCODE_H_

#include <stdbool.h>
#include <stdint.h>

#include "colorspace.h"

struct EncodeContext;
struct GpuContext;
struct GpuFrame;
struct TimingStats;

struct EncodeContext* EncodeContextCreate(struct GpuContext* gpu_context,
                                          uint32_t width, uint32_t height,
                                          enum YuvColorspace colorspace,
                                          enum YuvRange range);
const struct GpuFrame* EncodeContextGetFrame(
    struct EncodeContext* encode_context);
bool EncodeContextEncodeFrame(struct EncodeContext* encode_context, int fd,
                              struct TimingStats* encode,
                              struct TimingStats* drain);
void EncodeContextDestroy(struct EncodeContext* encode_context);

#endif  // STREAMER_ENCODE_H_
