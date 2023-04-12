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

#include "encode.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

#include "gpu.h"
#include "toolbox/utils.h"

struct EncodeContext {
  struct GpuContext* gpu_context;
  AVBufferRef* hwdevice_context;
  AVCodecContext* codec_context;

  AVFrame* hw_frame;
  struct GpuFrame* gpu_frame;
};

static bool SetHwFramesContext(struct EncodeContext* encode_context, int width,
                               int height) {
  encode_context->codec_context->hw_frames_ctx =
      av_hwframe_ctx_alloc(encode_context->hwdevice_context);
  if (!encode_context->codec_context->hw_frames_ctx) {
    LOG("Failed to allocate hwframes context");
    return false;
  }

  AVHWFramesContext* hwframes_context_data =
      (void*)(encode_context->codec_context->hw_frames_ctx->data);
  hwframes_context_data->initial_pool_size = 8;
  hwframes_context_data->format = AV_PIX_FMT_VAAPI;
  hwframes_context_data->sw_format = AV_PIX_FMT_NV12;
  hwframes_context_data->width = width;
  hwframes_context_data->height = height;
  int err = av_hwframe_ctx_init(encode_context->codec_context->hw_frames_ctx);
  if (err < 0) {
    LOG("Failed to init hwframes context (%s)", av_err2str(err));
    av_buffer_unref(&encode_context->codec_context->hw_frames_ctx);
    return false;
  }
  return true;
}

static enum AVColorSpace ConvertColorspace(enum YuvColorspace colorspace) {
  switch (colorspace) {
    case kItuRec601:
      // TODO(mburakov): No dedicated definition for BT601?
      return AVCOL_SPC_SMPTE170M;
    case kItuRec709:
      return AVCOL_SPC_BT709;
    default:
      __builtin_unreachable();
  }
}

static enum AVColorRange ConvertRange(enum YuvRange range) {
  switch (range) {
    case kNarrowRange:
      return AVCOL_RANGE_MPEG;
    case kFullRange:
      return AVCOL_RANGE_JPEG;
    default:
      __builtin_unreachable();
  }
}

struct EncodeContext* EncodeContextCreate(struct GpuContext* gpu_context,
                                          uint32_t width, uint32_t height,
                                          enum YuvColorspace colrospace,
                                          enum YuvRange range) {
  struct EncodeContext* encode_context = malloc(sizeof(struct EncodeContext));
  if (!encode_context) {
    LOG("Failed to allocate encode context (%s)", strerror(errno));
    return NULL;
  }
  *encode_context = (struct EncodeContext){
      .gpu_context = gpu_context,
  };

  int err = av_hwdevice_ctx_create(&encode_context->hwdevice_context,
                                   AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
  if (err < 0) {
    LOG("Failed to create hwdevice context (%s)", av_err2str(err));
    goto rollback_encode_context;
  }

  static const char codec_name[] = "hevc_vaapi";
  const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
  if (!codec) {
    LOG("Failed to find %s encoder", codec_name);
    goto rollback_hwdevice_context;
  }
  encode_context->codec_context = avcodec_alloc_context3(codec);
  if (!encode_context->codec_context) {
    LOG("Failed to allocate codec context");
    goto rollback_hwdevice_context;
  }

  encode_context->codec_context->time_base = (AVRational){1, 60};
  encode_context->codec_context->width = (int)width;
  encode_context->codec_context->height = (int)height;
  encode_context->codec_context->pix_fmt = AV_PIX_FMT_VAAPI;
  encode_context->codec_context->max_b_frames = 0;
  encode_context->codec_context->refs = 1;
  encode_context->codec_context->global_quality = 28;
  encode_context->codec_context->colorspace = ConvertColorspace(colrospace);
  encode_context->codec_context->color_range = ConvertRange(range);

  if (!SetHwFramesContext(encode_context, (int)width, (int)height)) {
    LOG("Failed to set hwframes context");
    goto rollback_codec_context;
  }
  err = avcodec_open2(encode_context->codec_context, codec, NULL);
  if (err < 0) {
    LOG("Failed to open codec (%s)", av_err2str(err));
    goto rollback_codec_context;
  }
  return encode_context;

rollback_codec_context:
  avcodec_free_context(&encode_context->codec_context);
rollback_hwdevice_context:
  av_buffer_unref(&encode_context->hwdevice_context);
rollback_encode_context:
  free(encode_context);
  return NULL;
}

static struct GpuFrame* PrimeToGpuFrame(
    struct GpuContext* gpu_context, const VADRMPRIMESurfaceDescriptor* prime) {
  struct GpuFramePlane planes[4];
  for (size_t i = 0; i < prime->layers[0].num_planes; i++) {
    uint32_t object_index = prime->layers[0].object_index[i];
    if (prime->objects[object_index].fd == -1) break;
    planes[i] = (struct GpuFramePlane){
        .dmabuf_fd = prime->objects[object_index].fd,
        .pitch = prime->layers[0].pitch[i],
        .offset = prime->layers[0].offset[i],
        .modifier = prime->objects[object_index].drm_format_modifier,
    };
  }
  struct GpuFrame* gpu_frame =
      GpuFrameCreate(gpu_context, prime->width, prime->height, prime->fourcc,
                     prime->layers[0].num_planes, planes);
  for (size_t i = prime->num_objects; i; i--) close(prime->objects[i - 1].fd);
  return gpu_frame;
}

const struct GpuFrame* EncodeContextGetFrame(
    struct EncodeContext* encode_context) {
  AVFrame* hw_frame = av_frame_alloc();
  if (!hw_frame) {
    LOG("Failed to allocate hwframe");
    return NULL;
  }

  int err = av_hwframe_get_buffer(encode_context->codec_context->hw_frames_ctx,
                                  hw_frame, 0);
  if (err < 0) {
    LOG("Failed to get hwframe buffer (%s)", av_err2str(err));
    goto rollback_hw_frame;
  }
  if (!hw_frame->hw_frames_ctx) {
    LOG("Failed to ref hwframe context");
    goto rollback_hw_frame;
  }

  // mburakov: Roughly based on Sunshine code...
  AVVAAPIDeviceContext* vaapi_device_context =
      ((AVHWDeviceContext*)(void*)encode_context->hwdevice_context->data)
          ->hwctx;
  VASurfaceID surface_id = (VASurfaceID)(uintptr_t)hw_frame->data[3];
  VADRMPRIMESurfaceDescriptor prime;
  VAStatus status = vaExportSurfaceHandle(
      vaapi_device_context->display, surface_id,
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to export vaapi surface (%d)", status);
    goto rollback_hw_frame;
  }

  struct GpuFrame* gpu_frame =
      PrimeToGpuFrame(encode_context->gpu_context, &prime);
  if (!gpu_frame) {
    LOG("Failed to create gpu frame");
    goto rollback_hw_frame;
  }

  encode_context->hw_frame = hw_frame;
  encode_context->gpu_frame = gpu_frame;
  return gpu_frame;

rollback_hw_frame:
  av_frame_free(&hw_frame);
  return NULL;
}

static bool DrainPacket(const struct AVPacket* packet, int fd) {
  uint32_t size = (uint32_t)packet->size;
  struct iovec iov[] = {
      {.iov_base = &size, .iov_len = sizeof(size)},
      {.iov_base = packet->data, .iov_len = (size_t)packet->size},
  };
  for (;;) {
    ssize_t result = writev(fd, iov, LENGTH(iov));
    if (result < 0) {
      if (errno == EINTR) continue;
      LOG("Failed to write (%s)", strerror(errno));
      return false;
    }
    for (size_t i = 0; i < LENGTH(iov); i++) {
      size_t delta = MIN((size_t)result, iov[i].iov_len);
      iov[i].iov_base = (uint8_t*)iov[i].iov_base + delta;
      iov[i].iov_len -= delta;
      result -= delta;
    }
    if (!result) return true;
  }
}

bool EncodeContextEncodeFrame(struct EncodeContext* encode_context, int fd) {
  bool result = false;
  if (encode_context->gpu_frame) {
    GpuFrameDestroy(encode_context->gpu_frame);
    encode_context->gpu_frame = NULL;
  }
  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    LOG("Failed to allocate packet (%s)", strerror(errno));
    goto rollback_hw_frame;
  }

  int err = avcodec_send_frame(encode_context->codec_context,
                               encode_context->hw_frame);
  if (err < 0) {
    LOG("Failed to send frame (%s)", av_err2str(err));
    goto rollback_packet;
  }

  err = avcodec_receive_packet(encode_context->codec_context, packet);
  switch (err) {
    case 0:
      break;
    case AVERROR(EAGAIN):
      // TODO(mburakov): This happens only for the very first frame, and
      // effectively introduces an additional latency of 16ms...
      result = true;
      goto rollback_packet;
    default:
      LOG("Failed to receive packet (%s)", av_err2str(err));
      goto rollback_packet;
  }

  result = DrainPacket(packet, fd);
  av_packet_unref(packet);
  if (!result) {
    LOG("Failed to drain packet");
    goto rollback_packet;
  }

rollback_packet:
  av_packet_free(&packet);
rollback_hw_frame:
  av_frame_free(&encode_context->hw_frame);
  return result;
}

void EncodeContextDestroy(struct EncodeContext* encode_context) {
  if (encode_context->gpu_frame) GpuFrameDestroy(encode_context->gpu_frame);
  if (encode_context->hw_frame) av_frame_free(&encode_context->hw_frame);
  avcodec_free_context(&encode_context->codec_context);
  av_buffer_unref(&encode_context->hwdevice_context);
  free(encode_context);
}
