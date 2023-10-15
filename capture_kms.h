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

#ifndef STREAMER_CAPTURE_KMS_H_
#define STREAMER_CAPTURE_KMS_H_

#include "capture.h"

struct CaptureContextKms;

struct CaptureContextKms* CaptureContextKmsCreate(
    struct GpuContext* gpu_context,
    const struct CaptureContextCallbacks* callbacks, void* user);
int CaptureContextKmsGetEventsFd(struct CaptureContextKms* capture_context);
bool CaptureContextKmsProcessEvents(struct CaptureContextKms* capture_context);
void CaptureContextKmsDestroy(struct CaptureContextKms* capture_context);

#endif  // STREAMER_CAPTURE_KMS_H_
