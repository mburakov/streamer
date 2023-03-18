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
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "encode.h"
#include "gpu.h"
#include "util.h"

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

struct TimingStats {
  unsigned long long min;
  unsigned long long max;
  unsigned long long sum;
};

static void TimingStatsRecord(struct TimingStats* timing_stats,
                              unsigned long long value) {
  timing_stats->min = MIN(timing_stats->min, value);
  timing_stats->max = MAX(timing_stats->max, value);
  timing_stats->sum += value;
}

static void TimingStatsLog(const struct TimingStats* timing_stats,
                           const char* name, unsigned long long counter) {
  LOG("%s min/avg/max: %llu/%llu/%llu", name, timing_stats->min,
      timing_stats->sum / counter, timing_stats->max);
}

static unsigned long long MicrosNow(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000000ull +
         (unsigned long long)ts.tv_nsec / 1000ull;
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

  struct AUTO(GpuContext)* gpu_context = GpuContextCreate();
  if (!gpu_context) {
    LOG("Failed to create gpu context");
    return EXIT_FAILURE;
  }

  struct AUTO(CaptureContext)* capture_context =
      CaptureContextCreate(gpu_context);
  if (!capture_context) {
    LOG("Failed to create capture context");
    return EXIT_FAILURE;
  }

  struct AUTO(EncodeContext)* encode_context = NULL;

  struct TimingStats capture = {.min = ULLONG_MAX};
  struct TimingStats convert = {.min = ULLONG_MAX};
  struct TimingStats encode = {.min = ULLONG_MAX};
  struct TimingStats total = {.min = ULLONG_MAX};
  unsigned long long num_frames = 0;

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
      encode_context = EncodeContextCreate(gpu_context, width, height);
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
    if (!EncodeContextEncodeFrame(encode_context, STDOUT_FILENO)) {
      LOG("Failed to encode frame");
      return EXIT_FAILURE;
    }

    unsigned long long now = MicrosNow();
    if (num_frames++) {
      // mburakov: Do not record stats for first (lazy) frame.
      TimingStatsRecord(&capture, before_convert - before_capture);
      TimingStatsRecord(&convert, before_encode - before_convert);
      TimingStatsRecord(&encode, now - before_encode);
      TimingStatsRecord(&total, now - before_capture);
    }
    unsigned long long micros = now < next ? next - now : 0;
    if (micros) usleep((unsigned)micros);
  }

  if (!EncodeContextEncodeFrame(encode_context, STDOUT_FILENO)) {
    LOG("Failed to drain encoder");
    return EXIT_FAILURE;
  }

  num_frames--;
  TimingStatsLog(&capture, "Capture", num_frames);
  TimingStatsLog(&convert, "Convert", num_frames);
  TimingStatsLog(&encode, "Encode", num_frames);
  TimingStatsLog(&total, "Total", num_frames);
  return EXIT_SUCCESS;
}
