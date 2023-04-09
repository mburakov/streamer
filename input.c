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

#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uhid.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "toolbox/buffer.h"
#include "toolbox/utils.h"

struct InputHandler {
  struct Buffer buffer;
  int uhid_fd;
};

struct InputHandler* InputHandlerCreate(void) {
  struct InputHandler* input_handler = malloc(sizeof(struct InputHandler));
  if (!input_handler) {
    LOG("Failed to allocate input handler (%s)", strerror(errno));
    return NULL;
  }
  *input_handler = (struct InputHandler){
      .uhid_fd = -1,
  };

  BufferCreate(&input_handler->buffer);
  input_handler->uhid_fd = open("/dev/uhid", O_RDWR);
  if (input_handler->uhid_fd == -1) {
    LOG("Failed to open uhid device (%s)", strerror(errno));
    goto rollback_input_handler;
  }
  return input_handler;

rollback_input_handler:
  free(input_handler);
  return NULL;
}

int InputHandlerGetEventsFd(struct InputHandler* input_handler) {
  return input_handler->uhid_fd;
}

bool InputHandlerProcessEvents(struct InputHandler* input_handler) {
  struct uhid_event uhid_event;
  if (read(input_handler->uhid_fd, &uhid_event, sizeof(uhid_event)) < 0) {
    LOG("Failed to process uhid events (%s)", strerror(errno));
    return false;
  }
  // TODO(mburakov): Add logging?
  return true;
}

bool InputHandlerHandle(struct InputHandler* input_handler, int fd) {
  switch (BufferAppendFrom(&input_handler->buffer, fd)) {
    case -1:
      LOG("Failed to append input data to buffer (%s)", strerror(errno));
      return false;
    case 0:
      LOG("Client closed connection");
      return false;
    default:
      break;
  }

  for (;;) {
    struct uhid_event* event = input_handler->buffer.data;
    if (input_handler->buffer.size < sizeof(event->type)) {
      // mburakov: Packet type is not yet available.
      return true;
    }

    size_t size;
    switch (event->type) {
      case UHID_CREATE2:
        if (input_handler->buffer.size <
            offsetof(struct uhid_event, u.create2.rd_size) +
                sizeof(event->u.create2.rd_size)) {
          // mburakov: Report descriptor size is not yet available.
          return true;
        }
        size = offsetof(struct uhid_event, u.create2.rd_data) +
               event->u.create2.rd_size;
        if (input_handler->buffer.size < size) {
          // mburakov: Report descriptor data is not yet available.
          return true;
        }
        break;
      case UHID_INPUT2:
        if (input_handler->buffer.size <
            offsetof(struct uhid_event, u.input2.size) +
                sizeof(event->u.input2.size)) {
          // mburakov: Report size is not yet available.
          return true;
        }
        size =
            offsetof(struct uhid_event, u.input2.data) + event->u.input2.size;
        if (input_handler->buffer.size < size) {
          // mburakov: Report data is not yet available.
          return true;
        }
        break;
      case UHID_DESTROY:
        size = sizeof(event->type);
        break;
      default:
        __builtin_unreachable();
    }

    // mburakov: This write has to be atomic.
    if (write(input_handler->uhid_fd, event, size) != (ssize_t)size) {
      LOG("Failed to write uhid event (%s)", strerror(errno));
      return false;
    }
    BufferDiscard(&input_handler->buffer, size);
  }
}

void InputHandlerDestroy(struct InputHandler* input_handler) {
  close(input_handler->uhid_fd);
  BufferDestroy(&input_handler->buffer);
  free(input_handler);
}
