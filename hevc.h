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

#ifndef STREAMER_HEVC_H_
#define STREAMER_HEVC_H_

#include <stdbool.h>
#include <va/va.h>

// Table 7-1
enum NalUnitType {
  TRAIL_R = 1,
  BLA_W_LP = 16,
  IDR_W_RADL = 19,
  IDR_N_LP = 20,
  RSV_IRAP_VCL23 = 23,
  VPS_NUT = 32,
  SPS_NUT = 33,
  PPS_NUT = 34,
};

// Table 7-7
enum SliceType {
  B = 0,
  P = 1,
  I = 2,
};

struct Bitstream;

struct MoreVideoParameters {
  uint32_t max_b_depth;
  uint32_t time_base_num;
  uint32_t time_base_den;
};

struct MoreSeqParameters {
  uint32_t crop_width;
  uint32_t crop_height;
  // TODO(mburakov): Why ffmpeg ignores vui section in seq?
  bool video_signal_type_present_flag;
  bool video_full_range_flag;
  bool colour_description_present_flag;
  uint8_t colour_primaries;
  uint8_t transfer_characteristics;
  uint8_t matrix_coeffs;
  bool chroma_loc_info_present_flag;
  uint32_t chroma_sample_loc_type_top_field;
  uint32_t chroma_sample_loc_type_bottom_field;
};

struct MoreSliceParamerters {
  bool first_slice_segment_in_pic_flag;
  // TODO(mburakov): Deduce from picture parameter buffer?
  uint32_t num_negative_pics;
  uint32_t num_positive_pics;
  struct NegativePics {
    uint32_t delta_poc_s0_minus1;
    bool used_by_curr_pic_s0_flag;
  }* negative_pics;
  struct PositivePics {
    uint32_t delta_poc_s1_minus1;
    bool used_by_curr_pic_s1_flag;
  }* positive_pics;
};

void PackVideoParameterSetNalUnit(struct Bitstream* bitstream,
                                  const VAEncSequenceParameterBufferHEVC* seq,
                                  const struct MoreVideoParameters* mvp);
void PackSeqParameterSetNalUnit(struct Bitstream* bitstream,
                                const VAEncSequenceParameterBufferHEVC* seq,
                                const struct MoreVideoParameters* mvp,
                                const struct MoreSeqParameters* msp);
void PackPicParameterSetNalUnit(struct Bitstream* bitstream,
                                const VAEncPictureParameterBufferHEVC* pic);
void PackSliceSegmentHeaderNalUnit(struct Bitstream* bitstream,
                                   const VAEncSequenceParameterBufferHEVC* seq,
                                   const VAEncPictureParameterBufferHEVC* pic,
                                   const VAEncSliceParameterBufferHEVC* slice,
                                   const struct MoreSliceParamerters* msp);

#endif  // STREAMER_HEVC_H_
