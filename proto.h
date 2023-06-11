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

#ifndef STREAMER_PROTO_H_
#define STREAMER_PROTO_H_

#include <assert.h>
#include <stdint.h>

#define PROTO_TYPE_MISC 0
#define PROTO_TYPE_VIDEO 1
#define PROTO_TYPE_AUDIO 2

#define PROTO_FLAG_KEYFRAME 1

struct Proto {
  uint32_t size;
  uint8_t type;
  uint8_t flags;
  uint16_t latency;
  uint8_t data[];
};

static_assert(sizeof(struct Proto) == 8 * sizeof(uint8_t),
              "Suspicious proto struct size");

#endif  // STREAMER_PROTO_H_
