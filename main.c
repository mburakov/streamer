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
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "colorspace.h"
#include "encode.h"
#include "gpu.h"
#include "input.h"
#include "toolbox/io_muxer.h"
#include "toolbox/utils.h"

// TODO(mburakov): Currently zwp_linux_dmabuf_v1 has no way to provide
// colorspace and range information to the compositor. Maybe this would change
// in the future, i.e keep an eye on color-representation Wayland protocol:
// https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/183
static const enum YuvColorspace colorspace = kItuRec601;
static const enum YuvRange range = kNarrowRange;
static const int capture_period = 1000000000 / 60;

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

struct Contexts {
  struct IoMuxer io_muxer;
  int timer_fd;
  int server_fd;
  struct GpuContext* gpu_context;
  struct CaptureContext* capture_context;

  int client_fd;
  struct InputHandler* input_handler;
  struct EncodeContext* encode_context;
};

static int CreateServerSocket(const char* arg) {
  int port = atoi(arg);
  if (0 > port || port > UINT16_MAX) {
    LOG("Invalid port number argument");
    return -1;
  }
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    LOG("Failed to create socket (%s)", strerror(errno));
    return -1;
  }
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))) {
    LOG("Failed to reuse socket address (%s)", strerror(errno));
    goto rollback_sock;
  }
  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)port),
  };
  if (bind(sock, (const struct sockaddr*)&addr, sizeof(addr))) {
    LOG("Failed to bind socket (%s)", strerror(errno));
    goto rollback_sock;
  }
  if (listen(sock, SOMAXCONN)) {
    LOG("Failed to listen socket (%s)", strerror(errno));
    goto rollback_sock;
  }
  return sock;

rollback_sock:
  close(sock);
  return -1;
}

static void MaybeDropClient(struct Contexts* contexts) {
  static const struct itimerspec spec = {0};
  if (contexts->encode_context) {
    EncodeContextDestroy(contexts->encode_context);
    contexts->encode_context = NULL;
  }
  if (contexts->input_handler) {
    IoMuxerForget(&contexts->io_muxer,
                  InputHandlerGetEventsFd(contexts->input_handler));
    InputHandlerDestroy(contexts->input_handler);
    contexts->input_handler = NULL;
  }
  if (contexts->client_fd != -1) {
    IoMuxerForget(&contexts->io_muxer, contexts->client_fd);
    close(contexts->client_fd);
    contexts->client_fd = -1;
  }
}

static void OnTimerExpire(void* user) {
  struct Contexts* contexts = user;
  if (!IoMuxerOnRead(&contexts->io_muxer, contexts->timer_fd, &OnTimerExpire,
                     user)) {
    LOG("Failed to reschedule timer (%s)", strerror(errno));
    g_signal = SIGABRT;
    return;
  }
  uint64_t expirations;
  if (read(contexts->timer_fd, &expirations, sizeof(expirations)) !=
      sizeof(expirations)) {
    LOG("Failed to read timer expirations (%s)", strerror(errno));
    g_signal = SIGABRT;
    return;
  }
  if (contexts->client_fd == -1) {
    // mburakov: Timer must disarm itself AFTER reading.
    static const struct itimerspec spec = {0};
    if (timerfd_settime(contexts->timer_fd, 0, &spec, NULL)) {
      LOG("Failed to disarm timer (%s)", strerror(errno));
      g_signal = SIGABRT;
    }
    return;
  }

  const struct GpuFrame* captured_frame =
      CaptureContextGetFrame(contexts->capture_context);
  if (!captured_frame) {
    LOG("Failed to capture frame");
    goto drop_client;
  }

  if (!contexts->encode_context) {
    contexts->encode_context =
        EncodeContextCreate(contexts->gpu_context, captured_frame->width,
                            captured_frame->height, colorspace, range);
    if (!contexts->encode_context) {
      LOG("Failed to create encode context");
      goto drop_client;
    }
  }

  const struct GpuFrame* encoded_frame =
      EncodeContextGetFrame(contexts->encode_context);
  if (!encoded_frame) {
    LOG("Failed to get encoded frame");
    goto drop_client;
  }
  if (!GpuContextConvertFrame(contexts->gpu_context, captured_frame,
                              encoded_frame)) {
    LOG("Failed to convert frame");
    goto drop_client;
  }
  if (!EncodeContextEncodeFrame(contexts->encode_context,
                                contexts->client_fd)) {
    LOG("Failed to encode frame");
    goto drop_client;
  }
  return;

drop_client:
  MaybeDropClient(contexts);
}

static void OnClientWriting(void* user) {
  struct Contexts* contexts = user;
  if (!IoMuxerOnRead(&contexts->io_muxer, contexts->client_fd, &OnClientWriting,
                     user)) {
    LOG("Failed to reschedule client reading (%s)", strerror(errno));
    goto drop_client;
  }
  if (!InputHandlerHandle(contexts->input_handler, contexts->client_fd)) {
    LOG("Failed to handle client input");
    goto drop_client;
  }
  return;

drop_client:
  MaybeDropClient(contexts);
}

static void OnInputEvents(void* user) {
  struct Contexts* contexts = user;
  if (!IoMuxerOnRead(&contexts->io_muxer,
                     InputHandlerGetEventsFd(contexts->input_handler),
                     &OnInputEvents, user)) {
    LOG("Failed to reschedule events reading (%s)", strerror(errno));
    goto drop_client;
  }
  if (!InputHandlerProcessEvents(contexts->input_handler)) {
    LOG("Failed to process events");
    goto drop_client;
  }
  return;

drop_client:
  MaybeDropClient(contexts);
}

static void OnClientConnecting(void* user) {
  struct Contexts* contexts = user;
  if (!IoMuxerOnRead(&contexts->io_muxer, contexts->server_fd,
                     &OnClientConnecting, user)) {
    LOG("Failed to reschedule accept (%s)", strerror(errno));
    g_signal = SIGABRT;
    return;
  }
  int client_fd = accept(contexts->server_fd, NULL, NULL);
  if (client_fd < 0) {
    LOG("Failed to accept client (%s)", strerror(errno));
    g_signal = SIGABRT;
    return;
  }

  if (contexts->client_fd != -1) {
    LOG("One client is already connected");
    close(client_fd);
    return;
  }

  contexts->client_fd = client_fd;
  if (!IoMuxerOnRead(&contexts->io_muxer, contexts->client_fd, &OnClientWriting,
                     user)) {
    LOG("Failed to schedule client reading (%s)", strerror(errno));
    goto drop_client;
  }
  contexts->input_handler = InputHandlerCreate();
  if (!contexts->input_handler) {
    LOG("Failed to create input handler");
    goto drop_client;
  }
  if (!IoMuxerOnRead(&contexts->io_muxer,
                     InputHandlerGetEventsFd(contexts->input_handler),
                     &OnInputEvents, user)) {
    LOG("Failed to schedule events reading (%s)", strerror(errno));
    goto drop_client;
  }
  static const struct itimerspec spec = {
      .it_interval.tv_nsec = capture_period,
      .it_value.tv_nsec = capture_period,
  };
  if (timerfd_settime(contexts->timer_fd, 0, &spec, NULL)) {
    LOG("Failed to arm timer (%s)", strerror(errno));
    goto drop_client;
  }
  return;

drop_client:
  MaybeDropClient(contexts);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    LOG("Usage: %s <port>", argv[0]);
    return EXIT_FAILURE;
  }
  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  struct Contexts contexts = {
      .timer_fd = -1,
      .server_fd = -1,
      .client_fd = -1,
  };
  IoMuxerCreate(&contexts.io_muxer);
  contexts.server_fd = CreateServerSocket(argv[1]);
  if (contexts.server_fd == -1) {
    LOG("Failed to create server socket");
    goto rollback_io_muxer;
  }
  contexts.timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (contexts.timer_fd == -1) {
    LOG("Failed to create timer (%s)", strerror(errno));
    goto rollback_server_fd;
  }
  contexts.gpu_context = GpuContextCreate(colorspace, range);
  if (!contexts.gpu_context) {
    LOG("Failed to create gpu context");
    goto rollback_timer_fd;
  }
  contexts.capture_context = CaptureContextCreate(contexts.gpu_context);
  if (!contexts.capture_context) {
    LOG("Failed to create capture context");
    goto rollback_gpu_context;
  }

  if (!IoMuxerOnRead(&contexts.io_muxer, contexts.timer_fd, &OnTimerExpire,
                     &contexts)) {
    LOG("Failed to schedule timer (%s)", strerror(errno));
    goto rollback_capture_context;
  }
  if (!IoMuxerOnRead(&contexts.io_muxer, contexts.server_fd,
                     &OnClientConnecting, &contexts)) {
    LOG("Failed to schedule accept (%s)", strerror(errno));
    goto rollback_capture_context;
  }
  while (!g_signal) {
    if (IoMuxerIterate(&contexts.io_muxer, -1) && errno != EINTR) {
      LOG("Failed to iterate io muxer (%s)", strerror(errno));
      g_signal = SIGABRT;
    }
  }
  MaybeDropClient(&contexts);

rollback_capture_context:
  CaptureContextDestroy(contexts.capture_context);
rollback_gpu_context:
  GpuContextDestroy(contexts.gpu_context);
rollback_timer_fd:
  close(contexts.timer_fd);
rollback_server_fd:
  close(contexts.server_fd);
rollback_io_muxer:
  IoMuxerDestroy(&contexts.io_muxer);
  bool result = g_signal == SIGINT || g_signal == SIGTERM;
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
