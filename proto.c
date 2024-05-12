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

#include "proto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "toolbox/utils.h"

#define UNCONST(x) ((void*)(uintptr_t)(x))

static bool DrainBuffers(int fd, struct iovec* iovec, int count) {
  for (;;) {
    ssize_t result = writev(fd, iovec, count);
    if (result < 0) {
      if (errno == EINTR) continue;
      LOG("Failed to write (%s)", strerror(errno));
      return false;
    }
    for (int i = 0; i < count; i++) {
      size_t delta = MIN((size_t)result, iovec[i].iov_len);
      iovec[i].iov_base = (uint8_t*)iovec[i].iov_base + delta;
      iovec[i].iov_len -= delta;
      result -= delta;
    }
    if (!result) return true;
  }
}

bool WriteProto(int fd, const struct Proto* proto, const void* data) {
  struct iovec iovec[] = {
      {.iov_base = UNCONST(proto), .iov_len = sizeof(struct Proto)},
      {.iov_base = UNCONST(data), .iov_len = proto->size},
  };
  if (!DrainBuffers(fd, iovec, LENGTH(iovec))) {
    LOG("Failed to drain buffers");
    return false;
  }
  return true;
}
