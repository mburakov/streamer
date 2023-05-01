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

#include "bitstream.h"

void BitstreamAppend(struct Bitstream* bitstream, size_t size, uint32_t bits) {
  uint8_t* ptr = (uint8_t*)bitstream->data + bitstream->size / 8;
  size_t vacant_bits = 8 - bitstream->size % 8;
  *ptr &= ~0 << vacant_bits;

  if (vacant_bits >= size) {
    *ptr |= bits << (vacant_bits - size);
    bitstream->size += size;
    return;
  }

  *ptr |= bits >> (size - vacant_bits);
  bitstream->size += vacant_bits;
  BitstreamAppend(bitstream, size - vacant_bits, bits);
}

void BitstreamAppendUE(struct Bitstream* bitstream, uint32_t bits) {
  size_t size = 0;
  uint32_t dummy = ++bits;
  while (dummy) {
    dummy >>= 1;
    size++;
  }
  BitstreamAppend(bitstream, size - 1, 0);
  BitstreamAppend(bitstream, size, bits);
}

void BitstreamAppendSE(struct Bitstream* bitstream, int32_t bits) {
  BitstreamAppendUE(
      bitstream, bits <= 0 ? (uint32_t)(-2 * bits) : (uint32_t)(2 * bits - 1));
}

void BitstreamByteAlign(struct Bitstream* bitstream) {
  uint8_t* ptr = (uint8_t*)bitstream->data + bitstream->size / 8;
  size_t vacant_bits = 8 - bitstream->size % 8;
  if (vacant_bits == 8) return;

  *ptr &= ~0 << vacant_bits;
  bitstream->size += vacant_bits;
}
