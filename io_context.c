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

#include "io_context.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <threads.h>
#include <unistd.h>

#include "proto.h"
#include "queue.h"
#include "util.h"

struct IoContext {
  int fd;
  atomic_bool running;
  struct Queue prio;
  struct Queue queue;

  mtx_t mutex;
  cnd_t cond;
  thrd_t thread;
};

struct ProtoImpl {
  struct Proto proto;
  struct ProtoHeader header;
  uint8_t data[];
};

static bool IsPrioProto(const struct Proto* proto) {
  return proto->header->type == kProtoTypePing ||
         proto->header->type == kProtoTypePong;
}

static void ProtoDestroy(struct Proto* proto) { free(proto); }

static bool ReadAll(int fd, void* buffer, size_t size) {
  for (uint8_t* ptr = buffer; size;) {
    ssize_t result = read(fd, ptr, size);
    if (result <= 0) {
      LOG("Failed to read socket (%s)", strerror(errno));
      return false;
    }
    size -= (size_t)result;
    ptr += result;
  }
  return true;
}

static bool WriteAll(int fd, struct iovec* iov, size_t count) {
  while (count) {
    int max_count = (int)MIN(count, UIO_MAXIOV);
    ssize_t result = writev(fd, iov, max_count);
    if (result <= 0) {
      LOG("Failed to write socket (%s)", strerror(errno));
      return false;
    }

    for (;;) {
      if ((size_t)result < iov->iov_len) {
        iov->iov_len -= (size_t)result;
        iov->iov_base = (uint8_t*)iov->iov_base + result;
        break;
      }
      result -= (ssize_t)iov->iov_len;
      count--;
      iov++;
    }
  }
  return true;
}

static bool IoContextDequeue(struct IoContext* io_context,
                             struct Proto** pproto) {
  if (mtx_lock(&io_context->mutex) != thrd_success) {
    LOG("Failed to lock mutex (%s)", strerror(errno));
    return false;
  }

  void* item = NULL;
  while (!QueuePop(&io_context->prio, &item) &&
         !QueuePop(&io_context->queue, &item) &&
         atomic_load_explicit(&io_context->running, memory_order_relaxed)) {
    assert(cnd_wait(&io_context->cond, &io_context->mutex) == thrd_success);
  }

  assert(mtx_unlock(&io_context->mutex) == thrd_success);
  *pproto = item;
  return true;
}

static int IoContextThreadProc(void* arg) {
  struct IoContext* io_context = arg;
  for (;;) {
    struct Proto* proto;
    if (!IoContextDequeue(io_context, &proto)) {
      LOG("Failed to dequeue proto");
      goto leave;
    }

    if (!proto) {
      // mburakov: running was set to false externally.
      return 0;
    }

    struct iovec iov[] = {
        {.iov_base = (void*)(uintptr_t)(proto->header),
         .iov_len = sizeof(struct ProtoHeader)},
        {.iov_base = (void*)(uintptr_t)(proto->data),
         .iov_len = proto->header->size},
    };
    bool result = WriteAll(io_context->fd, iov, LENGTH(iov));
    proto->Destroy(proto);
    if (!result) {
      LOG("Failed to write proto");
      goto leave;
    }
  }

leave:
  atomic_store_explicit(&io_context->running, false, memory_order_relaxed);
  return 0;
}

struct IoContext* IoContextCreate(uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    LOG("Failed to create socket (%s)", strerror(errno));
    return NULL;
  }

  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))) {
    LOG("Failed to reuse socket address (%s)", strerror(errno));
    goto rollback_sock;
  }

  const struct sockaddr_in sa = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = INADDR_ANY,
  };
  if (bind(sock, (const struct sockaddr*)&sa, sizeof(sa))) {
    LOG("Failed to bind socket (%s)", strerror(errno));
    goto rollback_sock;
  }

  if (listen(sock, SOMAXCONN)) {
    LOG("Failed to listen socket (%s)", strerror(errno));
    goto rollback_sock;
  }

  struct IoContext* io_context = malloc(sizeof(struct IoContext));
  if (!io_context) {
    LOG("Failed to allocate io context (%s)", strerror(errno));
    goto rollback_sock;
  }

  io_context->fd = accept(sock, NULL, NULL);
  if (io_context->fd == -1) {
    LOG("Failed to accept socket (%s)", strerror(errno));
    goto rollback_io_context;
  }

  if (setsockopt(io_context->fd, IPPROTO_TCP, TCP_NODELAY, &(int){1},
                 sizeof(int))) {
    LOG("Failed to set TCP_NODELAY (%s)", strerror(errno));
    goto rollback_fd;
  }

  atomic_init(&io_context->running, true);
  QueueCreate(&io_context->prio);
  QueueCreate(&io_context->queue);
  if (mtx_init(&io_context->mutex, mtx_plain) != thrd_success) {
    LOG("Failed to init mutex (%s)", strerror(errno));
    goto rollback_fd;
  }

  if (cnd_init(&io_context->cond) != thrd_success) {
    LOG("Failed to init condition variable (%s)", strerror(errno));
    goto rollback_mutex;
  }

  if (thrd_create(&io_context->thread, &IoContextThreadProc, io_context) !=
      thrd_success) {
    LOG("Failed to create thread (%s)", strerror(errno));
    goto rollback_cond;
  }

  assert(!close(sock));
  return io_context;

rollback_cond:
  cnd_destroy(&io_context->cond);
rollback_mutex:
  mtx_destroy(&io_context->mutex);
rollback_fd:
  assert(!close(io_context->fd));
rollback_io_context:
  free(io_context);
rollback_sock:
  assert(!close(sock));
  return NULL;
}

struct Proto* IoContextRead(struct IoContext* io_context) {
  struct ProtoHeader header;
  if (!ReadAll(io_context->fd, &header, sizeof(header))) {
    LOG("Failed to read proto header");
    return NULL;
  }

  struct ProtoImpl* proto_impl = malloc(sizeof(struct ProtoImpl) + header.size);
  if (!proto_impl) {
    LOG("Failed to allocate proto (%s)", strerror(errno));
    return NULL;
  }

  if (!ReadAll(io_context->fd, &proto_impl->data, header.size)) {
    LOG("Failed to read proto body");
    goto rollback_proto_impl;
  }

  proto_impl->header = header;
  const struct Proto proto = {
      .Destroy = ProtoDestroy,
      .header = &proto_impl->header,
      .data = proto_impl->data,
  };
  memcpy(proto_impl, &proto, sizeof(proto));
  return &proto_impl->proto;

rollback_proto_impl:
  free(proto_impl);
  return NULL;
}

bool IoContextWrite(struct IoContext* io_context, struct Proto* proto) {
  if (!atomic_load_explicit(&io_context->running, memory_order_relaxed)) {
    LOG("Io context is not running");
    goto rollback_proto;
  }

  struct Queue* queue =
      IsPrioProto(proto) ? &io_context->prio : &io_context->queue;
  if (mtx_lock(&io_context->mutex) != thrd_success) {
    LOG("Failed to lock mutex (%s)", strerror(errno));
    goto rollback_proto;
  }

  if (!QueuePush(queue, proto)) {
    LOG("Failed to queue proto");
    goto rollback_lock;
  }

  assert(cnd_broadcast(&io_context->cond) == thrd_success);
  assert(mtx_unlock(&io_context->mutex) == thrd_success);
  return true;

rollback_lock:
  assert(mtx_unlock(&io_context->mutex) == thrd_success);
rollback_proto:
  proto->Destroy(proto);
  return false;
}

void IoContextDestroy(struct IoContext* io_context) {
  atomic_store_explicit(&io_context->running, false, memory_order_relaxed);
  assert(cnd_broadcast(&io_context->cond) == thrd_success);
  assert(thrd_join(io_context->thread, NULL) == thrd_success);
  cnd_destroy(&io_context->cond);
  mtx_destroy(&io_context->mutex);
  for (void* item; QueuePop(&io_context->prio, &item); free(item));
  QueueDestroy(&io_context->prio);
  for (void* item; QueuePop(&io_context->queue, &item); free(item));
  QueueDestroy(&io_context->queue);
  assert(!close(io_context->fd));
  free(io_context);
}
