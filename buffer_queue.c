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

#include "buffer_queue.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

struct BufferQueue {
  mtx_t mutex;
  struct BufferQueueItem** items;
  size_t size;
  size_t alloc;
};

struct BufferQueueItem* BufferQueueItemCreate(const void* data, size_t size) {
  struct BufferQueueItem* buffer_queue_item =
      malloc(sizeof(struct BufferQueueItem) + size);
  if (!buffer_queue_item) return NULL;
  buffer_queue_item->size = size;
  memcpy(buffer_queue_item->data, data, size);
  return buffer_queue_item;
}

void BufferQueueItemDestroy(struct BufferQueueItem* buffer_queue_item) {
  free(buffer_queue_item);
}

struct BufferQueue* BufferQueueCreate(void) {
  struct BufferQueue* buffer_queue = calloc(1, sizeof(struct BufferQueue));
  if (!buffer_queue) return false;
  if (mtx_init(&buffer_queue->mutex, mtx_plain) != thrd_success)
    goto rollback_buffer_queue;
  return buffer_queue;

rollback_buffer_queue:
  free(buffer_queue);
  return NULL;
}

bool BufferQueueQueue(struct BufferQueue* buffer_queue,
                      struct BufferQueueItem* buffer_queue_item) {
  if (!buffer_queue_item || mtx_lock(&buffer_queue->mutex) != thrd_success)
    return false;

  if (buffer_queue->size == buffer_queue->alloc) {
    size_t alloc = buffer_queue->alloc + 1;
    struct BufferQueueItem** items =
        realloc(buffer_queue->items, sizeof(struct BufferQueueItem*) * alloc);
    if (!items) {
      mtx_unlock(&buffer_queue->mutex);
      return false;
    }
    buffer_queue->items = items;
    buffer_queue->alloc = alloc;
  }

  buffer_queue->items[buffer_queue->size] = buffer_queue_item;
  buffer_queue->size++;
  mtx_unlock(&buffer_queue->mutex);
  return true;
}

bool BufferQueueDequeue(struct BufferQueue* buffer_queue,
                        struct BufferQueueItem** buffer_queue_item) {
  if (mtx_lock(&buffer_queue->mutex) != thrd_success) return false;
  if (!buffer_queue->size) {
    *buffer_queue_item = NULL;
  } else {
    buffer_queue->size--;
    *buffer_queue_item = buffer_queue->items[0];
    memmove(buffer_queue->items, buffer_queue->items + 1,
            sizeof(struct BufferQueueItem*) * buffer_queue->size);
  }
  mtx_unlock(&buffer_queue->mutex);
  return true;
}

void BufferQueueDestroy(struct BufferQueue* buffer_queue) {
  for (size_t i = 0; i < buffer_queue->size; i++)
    BufferQueueItemDestroy(buffer_queue->items[i]);
  free(buffer_queue->items);
  mtx_destroy(&buffer_queue->mutex);
  free(buffer_queue);
}
