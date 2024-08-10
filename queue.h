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

#ifndef STREAMER_QUEUE_H_
#define STREAMER_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>

struct Queue {
  void** buffer;
  size_t alloc;
  size_t size;
  size_t read;
  size_t write;
};

void QueueCreate(struct Queue* queue);
bool QueuePush(struct Queue* queue, void* item);
bool QueuePop(struct Queue* queue, void** pitem);
void QueueDestroy(struct Queue* queue);

#endif  // STREAMER_QUEUE_H_
