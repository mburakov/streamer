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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "capture.h"
#include "colorspace.h"
#include "encode.h"
#include "gpu.h"
#include "perf.h"
#include "util.h"

// TODO(mburakov): Currently zwp_linux_dmabuf_v1 has no way to provide
// colorspace and range information to the compositor. Maybe this would change
// in the future, i.e keep an eye on color-representation Wayland protocol:
// https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/183
static const enum YuvColorspace colorspace = kItuRec601;
static const enum YuvRange range = kNarrowRange;

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

static void GpuContextDtor(struct GpuContext** gpu_context) {
  if (!*gpu_context) return;
  GpuContextDestroy(*gpu_context);
  *gpu_context = NULL;
}

static void CaptureContextDtor(struct CaptureContext** capture_context) {
  if (!*capture_context) return;
  CaptureContextDestroy(*capture_context);
  *capture_context = NULL;
}

static void EncodeContextDtor(struct EncodeContext** encode_context) {
  if (!*encode_context) return;
  EncodeContextDestroy(*encode_context);
  *encode_context = NULL;
}

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGPIPE, OnSignal) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  struct GpuContext __attribute__((cleanup(GpuContextDtor)))* gpu_context =
      GpuContextCreate(colorspace, range);
  if (!gpu_context) {
    LOG("Failed to create gpu context");
    return EXIT_FAILURE;
  }

  struct CaptureContext
      __attribute__((cleanup(CaptureContextDtor)))* capture_context =
          CaptureContextCreate(gpu_context);
  if (!capture_context) {
    LOG("Failed to create capture context");
    return EXIT_FAILURE;
  }

  struct EncodeContext
      __attribute__((cleanup(EncodeContextDtor)))* encode_context = NULL;

  struct TimingStats capture;
  struct TimingStats convert;
  struct TimingStats encode;
  struct TimingStats drain;
  struct TimingStats total;
  TimingStatsReset(&capture);
  TimingStatsReset(&convert);
  TimingStatsReset(&encode);
  TimingStatsReset(&drain);
  TimingStatsReset(&total);
  unsigned long long num_frames = 0;

  unsigned long long recording_started = MicrosNow();
  static const unsigned long long delta = 1000000ull / 60ull;
  for (unsigned long long next = MicrosNow() + delta; !g_signal;
       next += delta) {
    unsigned long long before_capture = MicrosNow();
    const struct GpuFrame* captured_frame =
        CaptureContextGetFrame(capture_context);
    if (!captured_frame) {
      LOG("Failed to capture frame");
      return EXIT_FAILURE;
    }

    if (!encode_context) {
      uint32_t width, height;
      GpuFrameGetSize(captured_frame, &width, &height);
      encode_context =
          EncodeContextCreate(gpu_context, width, height, colorspace, range);
      if (!encode_context) {
        LOG("Failed to create encode context");
        return EXIT_FAILURE;
      }
    }

    const struct GpuFrame* encoded_frame =
        EncodeContextGetFrame(encode_context);
    if (!encoded_frame) {
      LOG("Failed to get encoded frame");
      return EXIT_FAILURE;
    }

    unsigned long long before_convert = MicrosNow();
    if (!GpuFrameConvert(captured_frame, encoded_frame)) {
      LOG("Failed to convert frame");
      return EXIT_FAILURE;
    }

    GpuContextSync(gpu_context);
    unsigned long long before_encode = MicrosNow();
    if (!EncodeContextEncodeFrame(encode_context, STDOUT_FILENO, &encode,
                                  &drain)) {
      LOG("Failed to encode frame");
      return EXIT_FAILURE;
    }

    unsigned long long now = MicrosNow();
    TimingStatsRecord(&capture, before_convert - before_capture);
    TimingStatsRecord(&convert, before_encode - before_convert);
    TimingStatsRecord(&total, now - before_capture);

    unsigned long long period = now - recording_started;
    static const unsigned long long second = 1000000;
    if (period > 10 * second) {
      LOG("---->8-------->8-------->8----");
      TimingStatsLog(&capture, "Capture", num_frames);
      TimingStatsLog(&convert, "Convert", num_frames);
      TimingStatsLog(&encode, "Encode", num_frames);
      TimingStatsLog(&drain, "Drain", num_frames);
      TimingStatsLog(&total, "Total", num_frames);
      TimingStatsReset(&capture);
      TimingStatsReset(&convert);
      TimingStatsReset(&encode);
      TimingStatsReset(&drain);
      TimingStatsReset(&total);
      recording_started = now;
      num_frames = 0;
    }

    now = MicrosNow();
    unsigned long long micros = now < next ? next - now : 0;
    if (micros) usleep((unsigned)micros);
    num_frames++;
  }

  if (!EncodeContextEncodeFrame(encode_context, STDOUT_FILENO, NULL, NULL)) {
    LOG("Failed to drain encoder");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
