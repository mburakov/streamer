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

#ifndef STREAMER_BITSTREAM_H_
#define STREAMER_BITSTREAM_H_

#include <stddef.h>
#include <stdint.h>

struct Bitstream {
  void* data;
  size_t size;
};

void BitstreamAppend(struct Bitstream* bitstream, size_t size, uint32_t bits);
void BitstreamAppendUE(struct Bitstream* bitstream, uint32_t bits);
void BitstreamAppendSE(struct Bitstream* bitstream, int32_t bits);
void BitstreamByteAlign(struct Bitstream* bitstream);

#endif  // STREAMER_BITSTREAM_H_
