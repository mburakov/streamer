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

#ifndef STREAMER_PROTO_H_
#define STREAMER_PROTO_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

enum ProtoType {
  kProtoTypeHello,
  kProtoTypePing,
  kProtoTypePong,
  kProtoTypeUhid,
  kProtoTypeVideo,
  kProtoTypeAudio,
};

struct ProtoHeader {
  uint32_t size;
  enum ProtoType type;
  uint64_t timestamp;
};

static_assert(sizeof(struct ProtoHeader) == 16 * sizeof(uint8_t),
              "Suspicious proto header struct size");

struct Proto {
  void (*const Destroy)(struct Proto* self);
  const struct ProtoHeader* const header;
  const void* const data;
};

#endif  // STREAMER_PROTO_H_
