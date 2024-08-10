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

#include <errno.h>
#include <pipewire/pipewire.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_context.h"
#include "io_context.h"
#include "proto.h"
#include "util.h"

static volatile sig_atomic_t g_signal;
static void OnSignal(int status) { g_signal = status; }

static bool SetupSignalHandler(int sig, void (*func)(int)) {
  struct sigaction sa = {
      .sa_handler = func,
  };
  if (sigemptyset(&sa.sa_mask) || sigaddset(&sa.sa_mask, sig)) {
    LOG("Failed to configure signal set (%s)", strerror(errno));
    return false;
  }
  if (sigaction(sig, &sa, NULL)) {
    LOG("Failed to set signal action (%s)", strerror(errno));
    return false;
  }
  return true;
}

static void HandleClientSession(struct IoContext* io_context) {
  struct Proto* proto = NULL;
  struct AudioContext* audio_context = NULL;
  while (!g_signal) {
    proto = IoContextRead(io_context);
    if (!proto) {
      LOG("Failed to read proto");
      goto leave;
    }

    switch (proto->header->type) {
      case kProtoTypeHello:
        if (audio_context) {
          LOG("Audio reconfiguration prohibited");
          goto rollback_proto;
        }
        audio_context = AudioContextCreate(io_context, proto);
        if (!audio_context) {
          LOG("Failed to create audio context");
          goto leave;
        }
        break;
      case kProtoTypePing:
      case kProtoTypeUhid:
        break;
      default:
        LOG("Unexpected proto received");
        goto rollback_proto;
    }
  }

rollback_proto:
  proto->Destroy(proto);
leave:
  if (audio_context) AudioContextDestroy(audio_context);
}

int main(int argc, char* argv[]) {
  pw_init(&argc, &argv);
  if (argc < 2) {
    LOG("Usage: streamer <port>");
    goto leave;
  }

  int port = atoi(argv[1]);
  if (0 >= port || port > UINT16_MAX) {
    LOG("Invalid port \"%s\"", argv[1]);
    goto leave;
  }

  SetupSignalHandler(SIGINT, OnSignal);
  SetupSignalHandler(SIGPIPE, SIG_IGN);
  SetupSignalHandler(SIGTERM, OnSignal);

  while (!g_signal) {
    struct IoContext* io_context = IoContextCreate((uint16_t)port);
    if (!io_context) {
      LOG("Failed to create io context");
      goto leave;
    }

    HandleClientSession(io_context);
    IoContextDestroy(io_context);
  }

leave:
  pw_deinit();
  bool result = g_signal == SIGINT || g_signal == SIGTERM;
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
