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

#include "gpu_context.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

extern const char _binary_vertex_glsl_start[];
extern const char _binary_vertex_glsl_end[];
extern const char _binary_luma_glsl_start[];
extern const char _binary_luma_glsl_end[];
extern const char _binary_chroma_glsl_start[];
extern const char _binary_chroma_glsl_end[];

struct GpuContext {
  // EGL objects
  EGLDisplay egl_display;
  EGLContext egl_context;

  // OpenGL functions
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

  // OpenGL objects
  GLuint program_luma;
  GLuint program_chroma;
  GLint sample_offsets;
  GLuint framebuffer;
  GLuint vertices;
};

static const char* EglErrorString(EGLint error) {
  static const char* const kEglErrorStrings[] = {
      "EGL_SUCCESS",       "EGL_NOT_INITIALIZED",     "EGL_BAD_ACCESS",
      "EGL_BAD_ALLOC",     "EGL_BAD_ATTRIBUTE",       "EGL_BAD_CONFIG",
      "EGL_BAD_CONTEXT",   "EGL_BAD_CURRENT_SURFACE", "EGL_BAD_DISPLAY",
      "EGL_BAD_MATCH",     "EGL_BAD_NATIVE_PIXMAP",   "EGL_BAD_NATIVE_WINDOW",
      "EGL_BAD_PARAMETER", "EGL_BAD_SURFACE",         "EGL_CONTEXT_LOST",
  };
  return EGL_SUCCESS <= error &&
                 error < EGL_SUCCESS + (EGLint)LENGTH(kEglErrorStrings)
             ? kEglErrorStrings[error - EGL_SUCCESS]
             : "???";
}

static const char* GlErrorString(GLenum error) {
  static const char* const kGlErrorStrings[] = {
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
  return GL_INVALID_ENUM <= error &&
                 error < GL_INVALID_ENUM + LENGTH(kGlErrorStrings)
             ? kGlErrorStrings[error - GL_INVALID_ENUM]
             : "???";
}

#define DEFINE_CHECK_BUILDABLE_FUNCTION(what, err, op)             \
  static bool CheckBuildable##what(GLuint buildable) {             \
    GLenum error = glGetError();                                   \
    if (error != GL_NO_ERROR) {                                    \
      LOG("Failed to " err " (%s)", GlErrorString(error));         \
      return false;                                                \
    }                                                              \
    GLint status;                                                  \
    glGet##what##iv(buildable, op, &status);                       \
    if (status != GL_TRUE) {                                       \
      GLint log_length;                                            \
      glGet##what##iv(buildable, GL_INFO_LOG_LENGTH, &log_length); \
      char message[log_length];                                    \
      memset(message, 0, sizeof(message));                         \
      glGet##what##InfoLog(buildable, log_length, NULL, message);  \
      LOG("%s", message);                                          \
      return false;                                                \
    }                                                              \
    return true;                                                   \
  }

DEFINE_CHECK_BUILDABLE_FUNCTION(Shader, "compile shader", GL_COMPILE_STATUS)
DEFINE_CHECK_BUILDABLE_FUNCTION(Program, "link program", GL_LINK_STATUS)

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
  if (!CheckBuildableShader(vertex)) {
    goto delete_vs;
  }

  GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  if (!fragment) {
    LOG("Failed to create fragment shader (%s)", GlErrorString(glGetError()));
    goto delete_vs;
  }
  size = (GLsizei)(fs_end - fs_begin);
  glShaderSource(fragment, 1, &fs_begin, &size);
  glCompileShader(fragment);
  if (!CheckBuildableShader(fragment)) {
    goto delete_fs;
  }

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

static bool GpuContextInitOpenGL(struct GpuContext* gpu_context) {
  // TODO(mburakov): Check extensions?
  gpu_context->glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  if (!gpu_context->glEGLImageTargetTexture2DOES) {
    LOG("Failed to get address of glEGLImageTargetTexture2DOES");
    return false;
  }

  gpu_context->program_luma =
      CreateGlProgram(_binary_vertex_glsl_start, _binary_vertex_glsl_end,
                      _binary_luma_glsl_start, _binary_luma_glsl_end);
  if (!gpu_context->program_luma) {
    LOG("Failed to create luma program");
    return false;
  }

  gpu_context->program_chroma =
      CreateGlProgram(_binary_vertex_glsl_start, _binary_vertex_glsl_end,
                      _binary_chroma_glsl_start, _binary_chroma_glsl_end);
  if (!gpu_context->program_chroma) {
    LOG("Failed to create chroma program");
    goto rollback_program_luma;
  }

  gpu_context->sample_offsets =
      glGetUniformLocation(gpu_context->program_chroma, "sample_offsets");
  if (gpu_context->sample_offsets == -1) {
    LOG("Failed to get sample_offsets uniform location (%s)",
        GlErrorString(glGetError()));
    goto rollback_program_chroma;
  }

  gpu_context->framebuffer = 0;
  glGenFramebuffers(1, &gpu_context->framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, gpu_context->framebuffer);
  if (!gpu_context->framebuffer) {
    LOG("Failed to allocate framebuffer (%s)", GlErrorString(glGetError()));
    goto rollback_program_chroma;
  }

  gpu_context->vertices = 0;
  glGenBuffers(1, &gpu_context->vertices);
  glBindBuffer(GL_ARRAY_BUFFER, gpu_context->vertices);
  if (!gpu_context->vertices) {
    LOG("Failed to allocate buffer (%s)", GlErrorString(glGetError()));
    goto rollback_framebuffer;
  }

  static const GLfloat kVertices[] = {.0f, .0f, 1.f, 0.f, 1.f, 1.f, .0f, 1.f};
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glEnableVertexAttribArray(0);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to initialize array buffer (%s)", GlErrorString(error));
    goto rollback_vertices;
  }
  return true;

rollback_vertices:
  glDeleteBuffers(1, &gpu_context->vertices);
rollback_framebuffer:
  glDeleteFramebuffers(1, &gpu_context->framebuffer);
rollback_program_chroma:
  glDeleteProgram(gpu_context->program_chroma);
rollback_program_luma:
  glDeleteProgram(gpu_context->program_luma);
  return false;
}

struct GpuContext* GpuContextCreate(EGLNativeDisplayType native_display) {
  struct GpuContext* gpu_context = malloc(sizeof(struct GpuContext));
  if (!gpu_context) {
    LOG("Failed to allocate gpu context (%s)", strerror(errno));
    return NULL;
  }

  gpu_context->egl_display =
      eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native_display, NULL);
  if (gpu_context->egl_display == EGL_NO_DISPLAY) {
    LOG("Failed to get platform display (%s)", EglErrorString(eglGetError()));
    goto rollback_gpu_context;
  }

  EGLint major, minor;
  if (!eglInitialize(gpu_context->egl_display, &major, &minor)) {
    LOG("Failed to initialize display (%s)", EglErrorString(eglGetError()));
    goto rollback_egl_display;
  }

  LOG("Initialized EGL %d.%d", major, minor);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    LOG("Failed to bind EGL API (%s)", EglErrorString(eglGetError()));
    goto rollback_egl_display;
  }

  static const EGLint kEglContextAttribs[] = {
#define _(...) __VA_ARGS__
      _(EGL_CONTEXT_MAJOR_VERSION, 3),
      _(EGL_CONTEXT_MINOR_VERSION, 1),
      EGL_NONE,
#undef _
  };
  gpu_context->egl_context =
      eglCreateContext(gpu_context->egl_display, EGL_NO_CONFIG_KHR,
                       EGL_NO_CONTEXT, kEglContextAttribs);
  if (gpu_context->egl_context == EGL_NO_CONTEXT) {
    LOG("Failed to create EGL context (%s)", EglErrorString(eglGetError()));
    goto rollback_egl_display;
  }

  if (!eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      gpu_context->egl_context)) {
    LOG("Failed to make EGL context current (%s)",
        EglErrorString(eglGetError()));
    goto rollback_egl_context;
  }

  bool result = GpuContextInitOpenGL(gpu_context);
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
  if (!result) {
    LOG("Failed to initialize OpenGL objects");
    goto rollback_egl_context;
  }
  return gpu_context;

rollback_egl_context:
  assert(eglDestroyContext(gpu_context->egl_display, gpu_context->egl_context));
rollback_egl_display:
  assert(eglTerminate(gpu_context->egl_display));
rollback_gpu_context:
  free(gpu_context);
  return NULL;
}

bool GpuContextCreateImage(const struct GpuContext* gpu_context,
                           const EGLAttrib* attrib_list,
                           struct GpuContextImage* gpu_context_image) {
  EGLImage egl_image = eglCreateImage(gpu_context->egl_display, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT, NULL, attrib_list);
  if (egl_image == EGL_NO_IMAGE) {
    LOG("Failed to create EGL image (%s)", EglErrorString(eglGetError()));
    return false;
  }

  if (!eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      gpu_context->egl_context)) {
    LOG("Failed to make EGL context current (%s)",
        EglErrorString(eglGetError()));
    goto rollback_egl_image;
  }

  GLuint gl_texture = 0;
  glGenTextures(1, &gl_texture);
  if (!gl_texture) {
    LOG("Failed to allocate texture (%s)", GlErrorString(glGetError()));
    goto rollback_egl_make_current;
  }

  glBindTexture(GL_TEXTURE_2D, gl_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gpu_context->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image);
  glBindTexture(GL_TEXTURE_2D, 0);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to initialize texture (%s)", GlErrorString(error));
    goto rollback_gl_texture;
  }

  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
  *gpu_context_image = (struct GpuContextImage){
      .egl_image = egl_image,
      .gl_texture = gl_texture,
  };
  return true;

rollback_gl_texture:
  glDeleteTextures(1, &gl_texture);
rollback_egl_make_current:
  assert(eglMakeCurrent(gpu_context->egl_context, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
rollback_egl_image:
  assert(eglDestroyImage(gpu_context->egl_display, egl_image));
  return false;
}

static bool GpuContextRender(GLuint source, GLuint target) {
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         target, 0);
  GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
    LOG("Framebuffer is incomplete (0x%x)", framebuffer_status);
    return false;
  }

  glBindTexture(GL_TEXTURE_2D, source);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOG("Failed to render (%s)", GlErrorString(error));
    return false;
  }
  return true;
}

bool GpuContextConvertColorspace(const struct GpuContext* gpu_context,
                                 EGLAttrib width, EGLAttrib height,
                                 GLuint source, GLuint luma, GLuint chroma) {
  if (!eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      gpu_context->egl_context)) {
    LOG("Failed to make EGL context current (%s)",
        EglErrorString(eglGetError()));
    return false;
  }

  glUseProgram(gpu_context->program_luma);
  glViewport(0, 0, (GLsizei)width, (GLsizei)height);
  if (!GpuContextRender(source, luma)) {
    LOG("Failed to convert luma plane");
    goto rollback_egl_make_current;
  }

  const GLfloat sample_offsets[] = {
#define _(...) __VA_ARGS__
      _(0.f, 0.f),
      _(1.f / (GLfloat)width, 0.f),
      _(0.f, 1.f / (GLfloat)height),
      _(1.f / (GLfloat)width, 1.f / (GLfloat)height),
#undef _
  };

  glUseProgram(gpu_context->program_chroma);
  glUniform2fv(gpu_context->sample_offsets, 4, sample_offsets);
  glViewport(0, 0, (GLsizei)width / 2, (GLsizei)height / 2);
  if (!GpuContextRender(source, chroma)) {
    LOG("Failed to convert chroma plane");
    goto rollback_egl_make_current;
  }

  EGLSync sync = eglCreateSync(gpu_context->egl_display, EGL_SYNC_FENCE, NULL);
  if (sync == EGL_NO_SYNC) {
    LOG("Failed to create EGL fence sync (%s)", EglErrorString(eglGetError()));
    goto rollback_egl_make_current;
  }

  if (!eglClientWaitSync(gpu_context->egl_display, sync, 0, EGL_FOREVER)) {
    LOG("Failed to wait EGLfence sync (%s)", EglErrorString(eglGetError()));
    goto rollback_sync;
  }

  assert(eglDestroySync(gpu_context->egl_display, sync));
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
  return true;

rollback_sync:
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
rollback_egl_make_current:
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
  return false;
}

void GpuContextDestroyImage(const struct GpuContext* gpu_context,
                            const struct GpuContextImage* gpu_context_image) {
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, gpu_context->egl_context));
  glDeleteTextures(1, &gpu_context_image->gl_texture);
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
  assert(
      eglDestroyImage(gpu_context->egl_display, gpu_context_image->egl_image));
}

void GpuContextDestroy(struct GpuContext* gpu_context) {
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, gpu_context->egl_context));
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &gpu_context->vertices);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &gpu_context->framebuffer);
  glUseProgram(0);
  glDeleteProgram(gpu_context->program_chroma);
  glDeleteProgram(gpu_context->program_luma);
  assert(eglMakeCurrent(gpu_context->egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT));
  assert(eglDestroyContext(gpu_context->egl_display, gpu_context->egl_context));
  assert(eglTerminate(gpu_context->egl_display));
  free(gpu_context);
}
