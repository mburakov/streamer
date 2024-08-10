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

#include "queue.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

void QueueCreate(struct Queue* queue) {
  memset(queue, 0, sizeof(struct Queue));
}

bool QueuePush(struct Queue* queue, void* item) {
  if (queue->size == queue->alloc) {
    size_t alloc = queue->alloc + 1;
    void** buffer = malloc(alloc * sizeof(void*));
    if (!buffer) return false;

    size_t head_size = queue->read;
    size_t tail_size = queue->size - queue->read;
    memcpy(buffer, queue->buffer + queue->read, tail_size * sizeof(void*));
    memcpy(buffer + tail_size, queue->buffer, head_size * sizeof(void*));
    free(queue->buffer);

    queue->buffer = buffer;
    queue->alloc = alloc;
    queue->read = 0;
    queue->write = queue->size;
  }

  queue->buffer[queue->write] = item;
  queue->write = (queue->write + 1) % queue->alloc;
  queue->size++;
  return true;
}

bool QueuePop(struct Queue* queue, void** pitem) {
  if (!queue->size) return false;
  *pitem = queue->buffer[queue->read];
  queue->read = (queue->read + 1) % queue->alloc;
  queue->size--;
  return true;
}

void QueueDestroy(struct Queue* queue) { free(queue->buffer); }
