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

#ifndef STREAMER_CAPTURE_H_
#define STREAMER_CAPTURE_H_

struct CaptureContext;
struct GpuContext;
struct GpuFrame;

struct CaptureContext* CaptureContextCreate(struct GpuContext* gpu_context);
const struct GpuFrame* CaptureContextGetFrame(
    struct CaptureContext* capture_context);
void CaptureContextDestroy(struct CaptureContext* capture_context);

#endif  // STREAMER_CAPTURE_H_
