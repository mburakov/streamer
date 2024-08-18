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

#ifndef STREAMER_GPU_CONTEXT_H_
#define STREAMER_GPU_CONTEXT_H_

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stddef.h>

struct GpuContextImage {
  EGLImage egl_image;
  GLuint gl_texture;
};

struct GpuContext;

struct GpuContext* GpuContextCreate(EGLNativeDisplayType native_display);
bool GpuContextCreateImage(const struct GpuContext* gpu_context,
                           const EGLAttrib* attrib_list,
                           struct GpuContextImage* gpu_context_image);
bool GpuContextConvertColorspace(const struct GpuContext* gpu_context,
                                 EGLAttrib width, EGLAttrib height,
                                 GLuint source, GLuint luma, GLuint chroma);
void GpuContextDestroyImage(const struct GpuContext* gpu_context,
                            const struct GpuContextImage* gpu_context_image);
void GpuContextDestroy(struct GpuContext* gpu_context);

#endif  // STREAMER_GPU_CONTEXT_H_
