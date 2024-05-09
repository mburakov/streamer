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

#ifndef STREAMER_BUFFER_QUEUE_H_
#define STREAMER_BUFFER_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct BufferQueue;

struct BufferQueueItem {
  size_t size;
  uint8_t data[];
};

struct BufferQueueItem* BufferQueueItemCreate(const void* data, size_t size);
void BufferQueueItemDestroy(struct BufferQueueItem* buffer_queue_item);

struct BufferQueue* BufferQueueCreate(void);
bool BufferQueueQueue(struct BufferQueue* buffer_queue,
                      struct BufferQueueItem* buffer_queue_item);
bool BufferQueueDequeue(struct BufferQueue* buffer_queue,
                        struct BufferQueueItem** buffer_queue_item);
void BufferQueueDestroy(struct BufferQueue* buffer_queue);

#endif  // STREAMER_BUFFER_QUEUE_H_
