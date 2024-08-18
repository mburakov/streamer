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

#ifndef STREAMER_ENCODE_CONTEXT_H_
#define STREAMER_ENCODE_CONTEXT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct EncodeContext;
struct IoContext;

struct EncodeContextFrame {
  void* user_data;
  struct {
    int fd;
    uint32_t offset;
    uint32_t pitch;
  } const planes[2];
};

struct EncodeContext* EncodeContextCreate(struct IoContext* io_context,
                                          uint32_t width, uint32_t height);
struct EncodeContextFrame* EncodeContextDequeue(
    struct EncodeContext* encode_context);
bool EncodeContextQueue(struct EncodeContext* encode_context,
                        struct EncodeContextFrame* encode_context_frame,
                        bool encode);
void EncodeContextDestroy(struct EncodeContext* encode_context);

#endif  // STREAMER_ENCODE_CONTEXT_H_
