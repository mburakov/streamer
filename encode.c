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
#include "perf.h"
#include "util.h"

#define AVBufferRefDestroy av_buffer_unref
#define AVFrameDestroy av_frame_free
#define AVPacketDestroy av_packet_free

struct EncodeContext {
  struct GpuContext* gpu_context;
  AVBufferRef* hwdevice_context;
  AVCodecContext* codec_context;

  AVFrame* hw_frame;
  struct GpuFrame* gpu_frame;
};

static bool SetHwFramesContext(struct EncodeContext* encode_context, int width,
                               int height) {
  AUTO(AVBufferRef)* hwframes_context =
      av_hwframe_ctx_alloc(encode_context->hwdevice_context);
  if (!hwframes_context) {
    LOG("Failed to allocate hwframes context");
    return false;
  }

  AVHWFramesContext* hwframes_context_data = (void*)(hwframes_context->data);
  hwframes_context_data->initial_pool_size = 8;
  hwframes_context_data->format = AV_PIX_FMT_VAAPI;
  hwframes_context_data->sw_format = AV_PIX_FMT_NV12;
  hwframes_context_data->width = width;
  hwframes_context_data->height = height;
  int err = av_hwframe_ctx_init(hwframes_context);
  if (err < 0) {
    LOG("Failed to init hwframes context (%s)", av_err2str(err));
    return false;
  }

  encode_context->codec_context->hw_frames_ctx =
      av_buffer_ref(hwframes_context);
  if (!encode_context->codec_context->hw_frames_ctx) {
    LOG("Failed to ref hwframes context");
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
  struct AUTO(EncodeContext)* encode_context =
      malloc(sizeof(struct EncodeContext));
  if (!encode_context) {
    LOG("Failed to allocate encode context (%s)", strerror(errno));
    return NULL;
  }
  *encode_context = (struct EncodeContext){
      .gpu_context = gpu_context,
      .hwdevice_context = NULL,
      .codec_context = NULL,
      .hw_frame = NULL,
      .gpu_frame = NULL,
  };

  int err = av_hwdevice_ctx_create(&encode_context->hwdevice_context,
                                   AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
  if (err < 0) {
    LOG("Failed to create hwdevice context (%s)", av_err2str(err));
    return NULL;
  }

  static const char codec_name[] = "h264_vaapi";
  const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
  if (!codec) {
    LOG("Failed to find %s encoder", codec_name);
    return NULL;
  }

  encode_context->codec_context = avcodec_alloc_context3(codec);
  if (!encode_context->codec_context) {
    LOG("Failed to allocate codec context");
    return NULL;
  }

  encode_context->codec_context->time_base = (AVRational){1, 60};
  encode_context->codec_context->width = (int)width;
  encode_context->codec_context->height = (int)height;
  encode_context->codec_context->pix_fmt = AV_PIX_FMT_VAAPI;
  encode_context->codec_context->max_b_frames = 0;
  encode_context->codec_context->refs = 1;
  encode_context->codec_context->global_quality = 32;
  encode_context->codec_context->colorspace = ConvertColorspace(colrospace);
  encode_context->codec_context->color_range = ConvertRange(range);

  if (!SetHwFramesContext(encode_context, (int)width, (int)height)) {
    LOG("Failed to set hwframes context");
    return NULL;
  }

  err = avcodec_open2(encode_context->codec_context, codec, NULL);
  if (err < 0) {
    LOG("Failed to open codec (%s)", av_err2str(err));
    return NULL;
  }
  return RELEASE(encode_context);
}

const struct GpuFrame* EncodeContextGetFrame(
    struct EncodeContext* encode_context) {
  AUTO(AVFrame)* hw_frame = av_frame_alloc();
  if (!hw_frame) {
    LOG("Failed to allocate hwframe");
    return NULL;
  }

  int err = av_hwframe_get_buffer(encode_context->codec_context->hw_frames_ctx,
                                  hw_frame, 0);
  if (err < 0) {
    LOG("Failed to get hwframe buffer (%s)", av_err2str(err));
    return NULL;
  }
  if (!hw_frame->hw_frames_ctx) {
    LOG("Failed to ref hwframe context");
    return NULL;
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
    return NULL;
  }

  struct GpuFramePlane planes[prime.layers[0].num_planes];
  for (size_t i = 0; i < LENGTH(planes); i++) {
    planes[i] = (struct GpuFramePlane){
        .dmabuf_fd = prime.objects[prime.layers[0].object_index[i]].fd,
        .pitch = prime.layers[0].pitch[i],
        .offset = prime.layers[0].offset[i],
        .modifier =
            prime.objects[prime.layers[0].object_index[i]].drm_format_modifier,
    };
  }
  struct AUTO(GpuFrame)* gpu_frame =
      GpuFrameCreate(encode_context->gpu_context, prime.width, prime.height,
                     prime.fourcc, LENGTH(planes), planes);
  if (!gpu_frame) {
    LOG("Failed to create gpu frame");
    goto release_planes;
  }

  encode_context->hw_frame = RELEASE(hw_frame);
  encode_context->gpu_frame = RELEASE(gpu_frame);

release_planes:
  for (size_t i = prime.num_objects; i; i--) close(prime.objects[i - 1].fd);
  return encode_context->gpu_frame;
}

static bool DrainPacket(const struct AVPacket* packet, int fd) {
  uint32_t size = (uint32_t)packet->size;
  struct iovec iov[] = {
      {.iov_base = &size, .iov_len = sizeof(size)},
      {.iov_base = packet->data, .iov_len = (size_t)packet->size},
  };
  for (;;) {
    ssize_t result = writev(fd, iov, LENGTH(iov));
    switch (result) {
      case -1:
        if (errno == EINTR) continue;
        if (errno == EPIPE) return true;
        LOG("Failed to write packed (%s)", strerror(errno));
        return false;
      case 0:
        LOG("Output file descriptor closed");
        return false;
      default:
        break;
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

bool EncodeContextEncodeFrame(struct EncodeContext* encode_context, int fd,
                              struct TimingStats* encode,
                              struct TimingStats* drain) {
  unsigned long long before_send = MicrosNow();
  GpuFrameDestroy(&encode_context->gpu_frame);
  AUTO(AVFrame)* hw_frame = RELEASE(encode_context->hw_frame);
  AUTO(AVPacket)* packet = av_packet_alloc();
  if (!packet) {
    LOG("Failed to allocate packet (%s)", strerror(errno));
    return false;
  }

  int err = avcodec_send_frame(encode_context->codec_context, hw_frame);
  if (err < 0) {
    LOG("Failed to send frame (%s)", av_err2str(err));
    return false;
  }

  unsigned long long total_send = MicrosNow() - before_send;
  unsigned long long total_receive = 0;
  unsigned long long total_drain = 0;
  for (;;) {
    unsigned long long before_receive = MicrosNow();
    err = avcodec_receive_packet(encode_context->codec_context, packet);
    switch (err) {
      case 0:
        break;
      case AVERROR(EAGAIN):
      case AVERROR_EOF:
        total_receive += MicrosNow() - before_receive;
        if (encode) TimingStatsRecord(encode, total_send + total_receive);
        if (drain) TimingStatsRecord(drain, total_drain);
        return true;
      default:
        LOG("Failed to receive packet (%s)", av_err2str(err));
        return false;
    }

    packet->stream_index = 0;
    unsigned long long before_drain = MicrosNow();
    bool result = DrainPacket(packet, fd);
    av_packet_unref(packet);
    if (!result) {
      LOG("Failed to write full packet (%s)", strerror(errno));
      return false;
    }

    total_receive += before_drain - before_receive;
    total_drain += MicrosNow() - before_drain;
  }
}

void EncodeContextDestroy(struct EncodeContext** encode_context) {
  if (!encode_context || !*encode_context) return;
  if ((*encode_context)->gpu_frame)
    GpuFrameDestroy(&(*encode_context)->gpu_frame);
  if ((*encode_context)->hw_frame) av_frame_free(&(*encode_context)->hw_frame);
  if ((*encode_context)->codec_context)
    avcodec_free_context(&(*encode_context)->codec_context);
  if ((*encode_context)->hwdevice_context)
    av_buffer_unref(&(*encode_context)->hwdevice_context);
}
