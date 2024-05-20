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
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio.h"
#include "capture.h"
#include "colorspace.h"
#include "encode.h"
#include "gpu.h"
#include "input.h"
#include "proto.h"
#include "toolbox/io_muxer.h"
#include "toolbox/perf.h"
#include "toolbox/utils.h"

// TODO(mburakov): Currently zwp_linux_dmabuf_v1 has no way to provide
// colorspace and range information to the compositor. Maybe this would change
// in the future, i.e keep an eye on color-representation Wayland protocol:
// https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/183
static const enum YuvColorspace colorspace = kItuRec601;
static const enum YuvRange range = kNarrowRange;

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

struct Contexts {
  bool disable_uhid;
  const char* audio_config;
  struct AudioContext* audio_context;
  struct GpuContext* gpu_context;
  struct IoMuxer io_muxer;
  int server_fd;

  int client_fd;
  struct InputHandler* input_handler;
  struct CaptureContext* capture_context;
  struct EncodeContext* encode_context;
  bool drop_client;
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
  if (contexts->encode_context) {
    EncodeContextDestroy(contexts->encode_context);
    contexts->encode_context = NULL;
  }
  if (contexts->capture_context) {
    IoMuxerForget(&contexts->io_muxer,
                  CaptureContextGetEventsFd(contexts->capture_context));
    CaptureContextDestroy(contexts->capture_context);
    contexts->capture_context = NULL;
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

static void OnAudioContextAudioReady(void* user, const void* buffer,
                                     size_t size, size_t latency) {
  struct Contexts* contexts = user;
  if (contexts->client_fd == -1) return;

  struct Proto proto = {
      .size = (uint32_t)size,
      .type = PROTO_TYPE_AUDIO,
      .flags = 0,
      .latency = (uint16_t)MIN(latency, UINT16_MAX),
  };
  if (!WriteProto(contexts->client_fd, &proto, buffer)) {
    LOG("Failed to write audio frame");
    MaybeDropClient(contexts);
  }
}

static void OnCaptureContextFrameReady(void* user,
                                       const struct GpuFrame* captured_frame) {
  struct Contexts* contexts = user;
  unsigned long long timestamp = MicrosNow();

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
  if (!EncodeContextEncodeFrame(contexts->encode_context, contexts->client_fd,
                                timestamp)) {
    LOG("Failed to encode frame");
    goto drop_client;
  }
  return;

drop_client:
  // TODO(mburakov): Can't drop client here, because leftover code in capturing
  // functions would fail in this case. Instead just schedule dropping client
  // here, and execute that in the event loop of the main function.
  contexts->drop_client = true;
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
    LOG("Failed to reschedule input events reading (%s)", strerror(errno));
    goto drop_client;
  }
  if (!InputHandlerProcessEvents(contexts->input_handler)) {
    LOG("Failed to process input events");
    goto drop_client;
  }
  return;

drop_client:
  MaybeDropClient(contexts);
}

static void OnAudioContextEvents(void* user) {
  struct Contexts* contexts = user;
  if (!IoMuxerOnRead(&contexts->io_muxer,
                     AudioContextGetEventsFd(contexts->audio_context),
                     &OnAudioContextEvents, user)) {
    LOG("Failed to reschedule audio io (%s)", strerror(errno));
    g_signal = SIGABRT;
    return;
  }
  if (!AudioContextProcessEvents(contexts->audio_context)) {
    LOG("Failed to process audio events");
    g_signal = SIGABRT;
    return;
  }
}

static void OnCaptureContextEvents(void* user) {
  struct Contexts* contexts = user;
  if (!IoMuxerOnRead(&contexts->io_muxer,
                     CaptureContextGetEventsFd(contexts->capture_context),
                     &OnCaptureContextEvents, user)) {
    LOG("Failed to reschedule capture events reading (%s)", strerror(errno));
    goto drop_client;
  }
  if (!CaptureContextProcessEvents(contexts->capture_context)) {
    LOG("Failed to process capture events");
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

  if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int))) {
    LOG("Failed to set TCP_NODELAY (%s)", strerror(errno));
    goto drop_client;
  }

  contexts->client_fd = client_fd;
  if (!IoMuxerOnRead(&contexts->io_muxer, contexts->client_fd, &OnClientWriting,
                     user)) {
    LOG("Failed to schedule client reading (%s)", strerror(errno));
    goto drop_client;
  }
  contexts->input_handler = InputHandlerCreate(contexts->disable_uhid);
  if (!contexts->input_handler) {
    LOG("Failed to create input handler");
    goto drop_client;
  }
  if (!IoMuxerOnRead(&contexts->io_muxer,
                     InputHandlerGetEventsFd(contexts->input_handler),
                     &OnInputEvents, user)) {
    LOG("Failed to schedule input events reading (%s)", strerror(errno));
    goto drop_client;
  }
  static const struct CaptureContextCallbacks kCaptureContextCallbacks = {
      .OnFrameReady = OnCaptureContextFrameReady,
  };
  contexts->capture_context = CaptureContextCreate(
      contexts->gpu_context, &kCaptureContextCallbacks, user);
  if (!contexts->capture_context) {
    LOG("Failed to create capture context");
    goto drop_client;
  }
  if (!IoMuxerOnRead(&contexts->io_muxer,
                     CaptureContextGetEventsFd(contexts->capture_context),
                     &OnCaptureContextEvents, user)) {
    LOG("Failed to schedule capture events reading (%s)", strerror(errno));
    goto drop_client;
  }

  struct Proto proto = {
      .size = (uint32_t)strlen(contexts->audio_config) + 1,
      .type = PROTO_TYPE_AUDIO,
      .flags = PROTO_FLAG_KEYFRAME,
      .latency = 0,
  };
  if (!WriteProto(contexts->client_fd, &proto, contexts->audio_config)) {
    LOG("Failed to write audio configuration");
    goto drop_client;
  }
  return;

drop_client:
  MaybeDropClient(contexts);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    LOG("Usage: %s <port> [--disable-uhid] [--audio <rate:channels>]", argv[0]);
    return EXIT_FAILURE;
  }
  if (signal(SIGINT, OnSignal) == SIG_ERR ||
      signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
      signal(SIGTERM, OnSignal) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  struct Contexts contexts = {
      .server_fd = -1,
      .client_fd = -1,
  };
  const char* audio_config = NULL;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--disable-uhid")) {
      contexts.disable_uhid = true;
    } else if (!strcmp(argv[i], "--audio")) {
      audio_config = argv[++i];
      if (i == argc) {
        LOG("Audio argument requires a value");
        return EXIT_FAILURE;
      }
    }
  }

  static struct AudioContextCallbacks kAudioContextCallbacks = {
      .OnAudioReady = OnAudioContextAudioReady,
  };
  if (audio_config) {
    contexts.audio_config = audio_config;
    contexts.audio_context =
        AudioContextCreate(audio_config, &kAudioContextCallbacks, &contexts);
    if (!contexts.audio_context) {
      LOG("Failed to create audio context");
      return EXIT_FAILURE;
    }
  }

  contexts.gpu_context = GpuContextCreate(colorspace, range);
  if (!contexts.gpu_context) {
    LOG("Failed to create gpu context");
    goto rollback_audio_context;
  }

  IoMuxerCreate(&contexts.io_muxer);
  contexts.server_fd = CreateServerSocket(argv[1]);
  if (contexts.server_fd == -1) {
    LOG("Failed to create server socket");
    goto rollback_io_muxer;
  }

  if (contexts.audio_context &&
      !IoMuxerOnRead(&contexts.io_muxer,
                     AudioContextGetEventsFd(contexts.audio_context),
                     &OnAudioContextEvents, &contexts)) {
    LOG("Failed to schedule audio io (%s)", strerror(errno));
    goto rollback_server_fd;
  }
  if (!IoMuxerOnRead(&contexts.io_muxer, contexts.server_fd,
                     &OnClientConnecting, &contexts)) {
    LOG("Failed to schedule accept (%s)", strerror(errno));
    goto rollback_server_fd;
  }

  while (!g_signal) {
    if (IoMuxerIterate(&contexts.io_muxer, -1) && errno != EINTR) {
      LOG("Failed to iterate io muxer (%s)", strerror(errno));
      g_signal = SIGABRT;
    }
    if (contexts.drop_client) {
      MaybeDropClient(&contexts);
      contexts.drop_client = false;
    }
  }
  MaybeDropClient(&contexts);

rollback_server_fd:
  close(contexts.server_fd);
rollback_io_muxer:
  IoMuxerDestroy(&contexts.io_muxer);
  GpuContextDestroy(contexts.gpu_context);
rollback_audio_context:
  if (contexts.audio_context) AudioContextDestroy(contexts.audio_context);
  bool result = g_signal == SIGINT || g_signal == SIGTERM;
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
