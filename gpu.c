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

#include "gpu.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES3/gl32.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Must be included last
#include <GLES2/gl2ext.h>

#include "util.h"

// TODO(mburakov): It should be theoretically possible to do everything in a
// single pass using a compute shader and GLES3. Unfortunately my test machine
// reports the primary framebuffer as multiplane. This is probably the reason
// why texture created from it can not be sampled using imageLoad in a compute
// shader even though it's still RGB. Fallback to GLES2 and per-plane textures
// for now, and figure out details later.

extern const char _binary_vertex_glsl_start[];
extern const char _binary_vertex_glsl_end[];
extern const char _binary_luma_glsl_start[];
extern const char _binary_luma_glsl_end[];
extern const char _binary_chroma_glsl_start[];
extern const char _binary_chroma_glsl_end[];

struct GpuContext {
  EGLDisplay display;
  EGLContext context;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
  GLuint program_luma;
  GLuint program_chroma;
  GLint chroma_offsets;
  GLuint framebuffer;
  GLuint vertices;
};

struct GpuFrame {
  struct GpuContext* gpu_context;
  uint32_t width, height;
  int dmabuf_fds[4];
  EGLImage images[2];
  GLuint textures[2];
};

static const char* EglErrorString(EGLint error) {
  static const char* const egl_error_strings[] = {
      "EGL_SUCCESS",       "EGL_NOT_INITIALIZED",     "EGL_BAD_ACCESS",
      "EGL_BAD_ALLOC",     "EGL_BAD_ATTRIBUTE",       "EGL_BAD_CONFIG",
      "EGL_BAD_CONTEXT",   "EGL_BAD_CURRENT_SURFACE", "EGL_BAD_DISPLAY",
      "EGL_BAD_MATCH",     "EGL_BAD_NATIVE_PIXMAP",   "EGL_BAD_NATIVE_WINDOW",
      "EGL_BAD_PARAMETER", "EGL_BAD_SURFACE",         "EGL_CONTEXT_LOST",
  };
  return EGL_SUCCESS <= error && error <= EGL_CONTEXT_LOST
             ? egl_error_strings[error - EGL_SUCCESS]
             : "???";
}

static const char* GlErrorString(GLenum error) {
  static const char* const gl_error_strings[] = {
      "GL_INVALID_ENUM",
      "GL_INVALID_VALUE",
      "GL_INVALID_OPERATION",
      "GL_STACK_OVERFLOW",
      "GL_STACK_UNDERFLOW",
      "GL_OUT_OF_MEMORY",
      "GL_INVALID_FRAMEBUFFER_OPERATION",
      "GL_CONTEXT_LOST",
  };
  if (error == GL_NO_ERROR) return "GL_NO_ERROR";
  return GL_INVALID_ENUM <= error && error <= GL_CONTEXT_LOST
             ? gl_error_strings[error - GL_INVALID_ENUM]
             : "???";
}

#define DEFINE_CHECK_BUILDABLE_FUNCTION(postfix, err_msg, getter_fn, \
                                        status_enum, logger_fn)      \
  static bool CheckBuildable##postfix(GLuint buildable) {            \
    GLenum error = glGetError();                                     \
    if (error != GL_NO_ERROR) {                                      \
      LOG(err_msg " (%s)", GlErrorString(error));                    \
      return false;                                                  \
    }                                                                \
    GLint status;                                                    \
    getter_fn(buildable, status_enum, &status);                      \
    if (status != GL_TRUE) {                                         \
      GLint log_length;                                              \
      getter_fn(buildable, GL_INFO_LOG_LENGTH, &log_length);         \
      char message[log_length];                                      \
      memset(message, 0, sizeof(message));                           \
      logger_fn(buildable, log_length, NULL, message);               \
      LOG("%s", message);                                            \
      return false;                                                  \
    }                                                                \
    return true;                                                     \
  }

DEFINE_CHECK_BUILDABLE_FUNCTION(Shader, "Failed to compile shader",
                                glGetShaderiv, GL_COMPILE_STATUS,
                                glGetShaderInfoLog)
DEFINE_CHECK_BUILDABLE_FUNCTION(Program, "Failed to link program",
                                glGetProgramiv, GL_LINK_STATUS,
                                glGetProgramInfoLog)

static bool HasExtension(const char* haystack, const char* needle) {
  bool result = !!strstr(haystack, needle);
  if (!result) LOG("Unsupported extension %s", needle);
  return result;
}

static GLuint CreateGlProgram(const char* vs_begin, const char* vs_end,
                              const char* fs_begin, const char* fs_end) {
  GLuint program = 0;
  GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
  if (!vertex) {
    LOG("Failed to create vertex shader (%s)", GlErrorString(glGetError()));
    goto bail_out;
  }
  GLsizei size = (GLsizei)(vs_end - vs_begin);
  glShaderSource(vertex, 1, &vs_begin, &size);
  glCompileShader(vertex);
  if (!CheckBuildableShader(vertex)) goto delete_vs;

  GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  if (!fragment) {
    LOG("Failed to create fragment shader (%s)", GlErrorString(glGetError()));
    goto delete_vs;
  }
  size = (GLsizei)(fs_end - fs_begin);
  glShaderSource(fragment, 1, &fs_begin, &size);
  glCompileShader(fragment);
  if (!CheckBuildableShader(fragment)) goto delete_fs;

  program = glCreateProgram();
  if (!program) {
    LOG("Failed to create shader program (%s)", GlErrorString(glGetError()));
    goto delete_fs;
  }
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  if (!CheckBuildableProgram(program)) {
    glDeleteProgram(program);
    program = 0;
    goto delete_fs;
  }

delete_fs:
  glDeleteShader(fragment);
delete_vs:
  glDeleteShader(vertex);
bail_out:
  return program;
}

static bool SetupImgInputUniform(GLuint program) {
  GLint img_input = glGetUniformLocation(program, "img_input");
  if (img_input == -1) {
    LOG("Failed to find img_input uniform (%s)", GlErrorString(glGetError()));
    return false;
  }

  glUseProgram(program);
  glUniform1i(img_input, 0);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to set img_input uniform (%s)", GlErrorString(glGetError()));
    return false;
  }
  return true;
}

struct GpuContext* GpuContextCreate(void) {
  struct AUTO(GpuContext)* gpu_context = malloc(sizeof(struct GpuContext));
  if (!gpu_context) {
    LOG("Failed to allocate gpu context (%s)", strerror(errno));
    return NULL;
  }
  *gpu_context = (struct GpuContext){
      .display = EGL_NO_DISPLAY,
      .context = EGL_NO_CONTEXT,
      .glEGLImageTargetTexture2DOES = NULL,
      .program_luma = 0,
      .program_chroma = 0,
      .chroma_offsets = -1,
      .framebuffer = 0,
      .vertices = 0,
  };

  const char* egl_ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!egl_ext) {
    LOG("Failed to query platformless egl extensions (%s)",
        EglErrorString(eglGetError()));
    return NULL;
  }

  LOG("EGL_EXTENSIONS: %s", egl_ext);
  if (!HasExtension(egl_ext, "EGL_MESA_platform_surfaceless")) return NULL;
  gpu_context->display =
      eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
  if (gpu_context->display == EGL_NO_DISPLAY) {
    LOG("Failed to get egl display (%s)", EglErrorString(eglGetError()));
    return NULL;
  }

  EGLint major, minor;
  if (!eglInitialize(gpu_context->display, &major, &minor)) {
    LOG("Failed to initialize egl display (%s)", EglErrorString(eglGetError()));
    return NULL;
  }

  LOG("Initialized EGL %d.%d", major, minor);
  egl_ext = eglQueryString(gpu_context->display, EGL_EXTENSIONS);
  if (!egl_ext) {
    LOG("Failed to query egl extensions (%s)", EglErrorString(eglGetError()));
    return NULL;
  }

  LOG("EGL_EXTENSIONS: %s", egl_ext);
  if (!HasExtension(egl_ext, "EGL_KHR_surfaceless_context") ||
      !HasExtension(egl_ext, "EGL_KHR_no_config_context") ||
      !HasExtension(egl_ext, "EGL_EXT_image_dma_buf_import") ||
      !HasExtension(egl_ext, "EGL_EXT_image_dma_buf_import_modifiers"))
    return NULL;

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    LOG("Failed to bind egl api (%s)", EglErrorString(eglGetError()));
    return NULL;
  }

  static const EGLint context_attribs[] = {
#define _(...) __VA_ARGS__
      _(EGL_CONTEXT_MAJOR_VERSION, 3),
      _(EGL_CONTEXT_MINOR_VERSION, 1),
      EGL_NONE,
#undef _
  };
  gpu_context->context = eglCreateContext(
      gpu_context->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attribs);
  if (gpu_context->context == EGL_NO_CONTEXT) {
    LOG("Failed to create egl context (%s)", EglErrorString(eglGetError()));
    return NULL;
  }

  if (!eglMakeCurrent(gpu_context->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      gpu_context->context)) {
    LOG("Failed to make egl context current (%s)",
        EglErrorString(eglGetError()));
    return NULL;
  }

  const char* gl_ext = (const char*)glGetString(GL_EXTENSIONS);
  if (!gl_ext) {
    LOG("Failed to get gl extensions (%s)", GlErrorString(glGetError()));
    return NULL;
  }

  LOG("GL_EXTENSIONS: %s", gl_ext);
  if (!HasExtension(gl_ext, "GL_OES_EGL_image")) return NULL;
  gpu_context->glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  if (!gpu_context->glEGLImageTargetTexture2DOES) {
    LOG("Failed to lookup glEGLImageTargetTexture2DOES function");
    return NULL;
  }

  gpu_context->program_luma =
      CreateGlProgram(_binary_vertex_glsl_start, _binary_vertex_glsl_end,
                      _binary_luma_glsl_start, _binary_luma_glsl_end);
  if (!gpu_context->program_luma ||
      !SetupImgInputUniform(gpu_context->program_luma)) {
    LOG("Failed to create luma program");
    return NULL;
  }

  gpu_context->program_chroma =
      CreateGlProgram(_binary_vertex_glsl_start, _binary_vertex_glsl_end,
                      _binary_chroma_glsl_start, _binary_chroma_glsl_end);
  if (!gpu_context->program_chroma ||
      !SetupImgInputUniform(gpu_context->program_luma)) {
    LOG("Failed to create chroma program");
    return NULL;
  }
  gpu_context->chroma_offsets =
      glGetUniformLocation(gpu_context->program_chroma, "chroma_offsets");
  if (gpu_context->chroma_offsets == -1) {
    LOG("Failed to find chroma_offsets uniform (%s)",
        GlErrorString(glGetError()));
    return NULL;
  }

  glGenFramebuffers(1, &gpu_context->framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, gpu_context->framebuffer);
  glGenBuffers(1, &gpu_context->vertices);
  glBindBuffer(GL_ARRAY_BUFFER, gpu_context->vertices);
  static const GLfloat vertices[] = {0, 0, 1, 0, 1, 1, 0, 1};
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glEnableVertexAttribArray(0);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to create gl objects (%s)", GlErrorString(glGetError()));
    return NULL;
  }
  return RELEASE(gpu_context);
}

bool GpuContextSync(struct GpuContext* gpu_context) {
  EGLSync sync = eglCreateSync(gpu_context->display, EGL_SYNC_FENCE, NULL);
  if (sync == EGL_NO_SYNC) {
    LOG("Failed to create egl fence sync (%s)", EglErrorString(eglGetError()));
    return false;
  }
  eglClientWaitSync(gpu_context->display, sync, 0, EGL_FOREVER);
  eglDestroySync(gpu_context->display, sync);
  return true;
}

void GpuContextDestroy(struct GpuContext** gpu_context) {
  if (!gpu_context || !*gpu_context) return;
  if ((*gpu_context)->vertices) glDeleteBuffers(1, &(*gpu_context)->vertices);
  if ((*gpu_context)->framebuffer)
    glDeleteFramebuffers(1, &(*gpu_context)->framebuffer);
  if ((*gpu_context)->program_chroma)
    glDeleteProgram((*gpu_context)->program_chroma);
  if ((*gpu_context)->program_luma)
    glDeleteProgram((*gpu_context)->program_luma);
  if ((*gpu_context)->context != EGL_NO_CONTEXT) {
    eglMakeCurrent((*gpu_context)->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglDestroyContext((*gpu_context)->display, (*gpu_context)->context);
  }
  if ((*gpu_context)->display != EGL_NO_DISPLAY)
    eglTerminate((*gpu_context)->display);
  free(*gpu_context);
  *gpu_context = NULL;
}

static EGLImage CreateEglImage(struct GpuContext* gpu_context, uint32_t width,
                               uint32_t height, uint32_t fourcc, size_t nplanes,
                               const struct GpuFramePlane* planes) {
  static const EGLAttrib attrib_keys[] = {
      EGL_DMA_BUF_PLANE0_FD_EXT,          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,       EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
      EGL_DMA_BUF_PLANE1_OFFSET_EXT,      EGL_DMA_BUF_PLANE1_PITCH_EXT,
      EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
      EGL_DMA_BUF_PLANE2_FD_EXT,          EGL_DMA_BUF_PLANE2_OFFSET_EXT,
      EGL_DMA_BUF_PLANE2_PITCH_EXT,       EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
      EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
      EGL_DMA_BUF_PLANE3_OFFSET_EXT,      EGL_DMA_BUF_PLANE3_PITCH_EXT,
      EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
  };

  EGLAttrib attrib_list[7 + LENGTH(attrib_keys) * 2] = {
#define _(...) __VA_ARGS__
      _(EGL_WIDTH, width),
      _(EGL_HEIGHT, height),
      _(EGL_LINUX_DRM_FOURCC_EXT, fourcc),
#undef _
  };

  EGLAttrib* pairs = &attrib_list[6];
  const EGLAttrib* key = &attrib_keys[0];
  for (size_t i = 0; i < nplanes; i++) {
    if (planes[i].dmabuf_fd == -1) break;
    *pairs++ = *key++;
    *pairs++ = planes[i].dmabuf_fd;
    *pairs++ = *key++;
    *pairs++ = planes[i].offset;
    *pairs++ = *key++;
    *pairs++ = planes[i].pitch;
    *pairs++ = *key++;
    *pairs++ = planes[i].modifier >> 32;
    *pairs++ = *key++;
    *pairs++ = planes[i].modifier & UINT32_MAX;
  }

  *pairs = EGL_NONE;
  EGLImage image = eglCreateImage(gpu_context->display, EGL_NO_CONFIG_KHR,
                                  EGL_LINUX_DMA_BUF_EXT, NULL, attrib_list);
  if (image == EGL_NO_IMAGE)
    LOG("Failed to create egl image (%s)", EglErrorString(eglGetError()));
  return image;
}

static GLuint CreateTexture(struct GpuContext* gpu_context, EGLImage image) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gpu_context->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to create texture (%s)", GlErrorString(error));
    glDeleteTextures(1, &texture);
    return 0;
  }
  return texture;
}

struct GpuFrame* GpuFrameCreate(struct GpuContext* gpu_context, uint32_t width,
                                uint32_t height, uint32_t fourcc,
                                size_t nplanes,
                                const struct GpuFramePlane* planes) {
  struct AUTO(GpuFrame)* gpu_frame = malloc(sizeof(struct GpuFrame));
  if (!gpu_frame) {
    LOG("Failed to allocate gpu frame (%s)", strerror(errno));
    return NULL;
  }
  *gpu_frame = (struct GpuFrame){
      .gpu_context = gpu_context,
      .width = width,
      .height = height,
      .dmabuf_fds = {-1, -1, -1, -1},
      .images = {EGL_NO_IMAGE, EGL_NO_IMAGE},
      .textures = {0, 0},
  };

  for (size_t i = 0; i < nplanes; i++) {
    gpu_frame->dmabuf_fds[i] = dup(planes[i].dmabuf_fd);
    if (gpu_frame->dmabuf_fds[i] == -1) {
      LOG("Failed to dup dmabuf fd (%s)", strerror(errno));
      return NULL;
    }
  }

  struct GpuFramePlane dummy_planes[nplanes];
  for (size_t i = 0; i < nplanes; i++) {
    dummy_planes[i] = (struct GpuFramePlane){
        .dmabuf_fd = gpu_frame->dmabuf_fds[i],
        .offset = planes[i].offset,
        .pitch = planes[i].pitch,
        .modifier = planes[i].modifier,
    };
  }

  if (fourcc == DRM_FORMAT_NV12) {
    gpu_frame->images[0] = CreateEglImage(gpu_context, width, height,
                                          DRM_FORMAT_R8, 1, &dummy_planes[0]);
    if (gpu_frame->images[0] == EGL_NO_IMAGE) {
      LOG("Failed to create luma plane image");
      return NULL;
    }
    gpu_frame->images[1] = CreateEglImage(gpu_context, width / 2, height / 2,
                                          DRM_FORMAT_GR88, 1, &dummy_planes[1]);
    if (gpu_frame->images[1] == EGL_NO_IMAGE) {
      LOG("Failed to create chroma plane image");
      return NULL;
    }
  } else {
    gpu_frame->images[0] = CreateEglImage(gpu_context, width, height, fourcc,
                                          nplanes, dummy_planes);
    if (gpu_frame->images[0] == EGL_NO_IMAGE) {
      LOG("Failed to create multiplanar image");
      return NULL;
    }
  }

  for (size_t i = 0; i < LENGTH(gpu_frame->images); i++) {
    if (gpu_frame->images[i] == EGL_NO_IMAGE) break;
    gpu_frame->textures[i] = CreateTexture(gpu_context, gpu_frame->images[i]);
    if (!gpu_frame->textures[i]) {
      LOG("Failed to create texture");
      return NULL;
    }
  }

  return RELEASE(gpu_frame);
}

void GpuFrameGetSize(const struct GpuFrame* gpu_frame, uint32_t* width,
                     uint32_t* height) {
  *width = gpu_frame->width;
  *height = gpu_frame->height;
}

static bool GpuFrameConvertImpl(GLuint from, GLuint to) {
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         to, 0);
  GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
    LOG("Framebuffer is incomplete (0x%x)", framebuffer_status);
    return false;
  }

  glBindTexture(GL_TEXTURE_2D, from);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to convert plane (%s)", GlErrorString(error));
    return false;
  }
  return true;
}

bool GpuFrameConvert(const struct GpuFrame* from, const struct GpuFrame* to) {
  glUseProgram(from->gpu_context->program_luma);
  glViewport(0, 0, (GLsizei)to->width, (GLsizei)to->height);
  if (!GpuFrameConvertImpl(from->textures[0], to->textures[0])) {
    LOG("Failed to convert luma plane");
    return false;
  }

  glUseProgram(from->gpu_context->program_chroma);
  glUniform2f(from->gpu_context->chroma_offsets, 1.f / (GLfloat)from->width,
              1.f / (GLfloat)from->height);
  glViewport(0, 0, (GLsizei)to->width / 2, (GLsizei)to->height / 2);
  if (!GpuFrameConvertImpl(from->textures[0], to->textures[1])) {
    LOG("Failed to convert chroma plane");
    return false;
  }
  return true;
}

void GpuFrameDestroy(struct GpuFrame** gpu_frame) {
  if (!gpu_frame || !*gpu_frame) return;
  for (size_t i = LENGTH((*gpu_frame)->textures); i; i--) {
    if ((*gpu_frame)->textures[i])
      glDeleteTextures(1, &(*gpu_frame)->textures[i]);
  }
  for (size_t i = LENGTH((*gpu_frame)->images); i; i--) {
    if ((*gpu_frame)->images[i] != EGL_NO_IMAGE)
      eglDestroyImage((*gpu_frame)->gpu_context->display,
                      (*gpu_frame)->images[i]);
  }
  for (size_t i = LENGTH((*gpu_frame)->dmabuf_fds); i; i--) {
    if ((*gpu_frame)->dmabuf_fds[i] != -1) close((*gpu_frame)->dmabuf_fds[i]);
  }
  free(*gpu_frame);
  *gpu_frame = NULL;
}
