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

#include "hevc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bitstream.h"

// mburakov: Below entries are hardcoded by ffmpeg:
static const uint8_t vps_video_parameter_set_id = 0;
static const bool vps_base_layer_internal_flag = 1;
static const bool vps_base_layer_available_flag = 1;
static const uint8_t vps_max_layers_minus1 = 0;
static const uint8_t vps_max_sub_layers_minus1 = 0;
static const bool vps_temporal_id_nesting_flag = 1;
static const uint8_t general_profile_space = 0;
static const bool general_progressive_source_flag = 1;
static const bool general_interlaced_source_flag = 0;
static const bool general_non_packed_constraint_flag = 1;
static const bool general_frame_only_constraint_flag = 1;
static const bool general_one_picture_only_constraint_flag = 0;
static const bool vps_sub_layer_ordering_info_present_flag = 0;
static const uint32_t vps_max_latency_increase_plus1 = 0;
static const uint8_t vps_max_layer_id = 0;
static const uint32_t vps_num_layer_sets_minus1 = 0;
static const bool vps_timing_info_present_flag = 1;
static const bool vps_poc_proportional_to_timing_flag = 0;
static const uint32_t vps_num_hrd_parameters = 0;
static const uint8_t sps_video_parameter_set_id = vps_video_parameter_set_id;
static const uint8_t sps_max_sub_layers_minus1 = vps_max_sub_layers_minus1;
static const bool sps_temporal_id_nesting_flag = vps_temporal_id_nesting_flag;
static const uint32_t sps_seq_parameter_set_id = 0;
static const uint32_t log2_max_pic_order_cnt_lsb_minus4 = 8;
static const bool sps_sub_layer_ordering_info_present_flag =
    vps_sub_layer_ordering_info_present_flag;
static const uint32_t sps_max_latency_increase_plus1 =
    vps_max_latency_increase_plus1;
static const uint32_t num_short_term_ref_pic_sets = 0;
static const bool long_term_ref_pics_present_flag = 0;
static const bool vui_parameters_present_flag = 1;
static const uint8_t video_format = 5;
static const bool vui_timing_info_present_flag = 1;
static const bool vui_poc_proportional_to_timing_flag =
    vps_poc_proportional_to_timing_flag;
static const bool vui_hrd_parameters_present_flag = 0;
static const bool bitstream_restriction_flag = 1;
static const bool motion_vectors_over_pic_boundaries_flag = 1;
static const bool restricted_ref_pic_lists_flag = 1;
static const uint32_t max_bytes_per_pic_denom = 0;
static const uint32_t max_bits_per_min_cu_denom = 0;
static const uint32_t log2_max_mv_length_horizontal = 15;
static const uint32_t log2_max_mv_length_vertical = 15;
static const uint32_t pps_pic_parameter_set_id = 0;
static const uint32_t pps_seq_parameter_set_id = sps_seq_parameter_set_id;
static const bool short_term_ref_pic_set_sps_flag = 0;

// mburakov: Below entries are defaulted by ffmpeg:
static const bool general_inbld_flag = 0;
static const bool vps_extension_flag = 0;
static const bool aspect_ratio_info_present_flag = 0;
static const bool overscan_info_present_flag = 0;
static const bool neutral_chroma_indication_flag = 0;
static const bool field_seq_flag = 0;
static const bool frame_field_info_present_flag = 0;
static const bool default_display_window_flag = 0;
static const bool tiles_fixed_structure_flag = 0;
static const uint32_t min_spatial_segmentation_idc = 0;
static const bool sps_extension_present_flag = 0;
static const bool output_flag_present_flag = 0;
static const uint8_t num_extra_slice_header_bits = 0;
static const bool cabac_init_present_flag = 0;
static const bool pps_slice_chroma_qp_offsets_present_flag = 0;
static const bool deblocking_filter_control_present_flag = 0;
static const bool lists_modification_present_flag = 0;
static const bool slice_segment_header_extension_present_flag = 0;
static const bool pps_extension_present_flag = 0;
static const uint32_t motion_vector_resolution_control_idc = 0;
static const bool pps_slice_act_qp_offsets_present_flag = 0;
static const bool chroma_qp_offset_list_enabled_flag = 0;
static const bool deblocking_filter_override_enabled_flag = 0;
static const bool deblocking_filter_override_flag = 0;
static const uint32_t num_entry_point_offsets = 0;
static const bool inter_ref_pic_set_prediction_flag = 0;

// 7.3.1.2 NAL unit header syntax
static void PackNalUnitHeader(struct Bitstream* bitstream,
                              uint8_t nal_unit_type) {
  BitstreamAppend(bitstream, 32, 0x00000001);
  BitstreamAppend(bitstream, 1, 0);  // forbidden_zero_bit
  BitstreamAppend(bitstream, 6, nal_unit_type);
  BitstreamAppend(bitstream, 6, 0);  // nuh_layer_id
  BitstreamAppend(bitstream, 3, 1);  // nuh_temporal_id_plus1
}

// 7.3.3 Profile, tier and level syntax
static void PackProfileTierLevel(struct Bitstream* bitstream,
                                 const VAEncSequenceParameterBufferHEVC* seq,
                                 bool profilePresentFlag,
                                 uint8_t maxNumSubLayersMinus1) {
  if (profilePresentFlag) {
    BitstreamAppend(bitstream, 2, general_profile_space);
    BitstreamAppend(bitstream, 1, seq->general_tier_flag);
    BitstreamAppend(bitstream, 5, seq->general_profile_idc);

    // mburakov: ffmpeg deduces general_profile_compatibility_flag.
    bool general_profile_compatibility_flag[32] = {0};
    general_profile_compatibility_flag[seq->general_profile_idc] = 1;
    if (general_profile_compatibility_flag[1])
      general_profile_compatibility_flag[2] = 1;
    if (general_profile_compatibility_flag[3]) {
      general_profile_compatibility_flag[1] = 1;
      general_profile_compatibility_flag[2] = 1;
    }

    for (uint8_t j = 0; j < 32; j++)
      BitstreamAppend(bitstream, 1, general_profile_compatibility_flag[j]);

    BitstreamAppend(bitstream, 1, general_progressive_source_flag);
    BitstreamAppend(bitstream, 1, general_interlaced_source_flag);
    BitstreamAppend(bitstream, 1, general_non_packed_constraint_flag);
    BitstreamAppend(bitstream, 1, general_frame_only_constraint_flag);
    if (seq->general_profile_idc == 4 ||
        general_profile_compatibility_flag[4] ||
        seq->general_profile_idc == 5 ||
        general_profile_compatibility_flag[5] ||
        seq->general_profile_idc == 6 ||
        general_profile_compatibility_flag[6] ||
        seq->general_profile_idc == 7 ||
        general_profile_compatibility_flag[7] ||
        seq->general_profile_idc == 8 ||
        general_profile_compatibility_flag[8] ||
        seq->general_profile_idc == 9 ||
        general_profile_compatibility_flag[9] ||
        seq->general_profile_idc == 10 ||
        general_profile_compatibility_flag[10] ||
        seq->general_profile_idc == 11 ||
        general_profile_compatibility_flag[11]) {
      // TODO(mburakov): Implement this!
      abort();
    } else if (seq->general_profile_idc == 2 ||
               general_profile_compatibility_flag[2]) {
      BitstreamAppend(bitstream, 7, 0);  // general_reserved_zero_7bits
      BitstreamAppend(bitstream, 1, general_one_picture_only_constraint_flag);
      BitstreamAppend(bitstream, 24, 0);  // general_reserved_zero_35bits
      BitstreamAppend(bitstream, 11, 0);  // general_reserved_zero_35bits
    } else {
      BitstreamAppend(bitstream, 24, 0);  // general_reserved_zero_43bits
      BitstreamAppend(bitstream, 19, 0);  // general_reserved_zero_43bits
    }

    if (seq->general_profile_idc == 1 ||
        general_profile_compatibility_flag[1] ||
        seq->general_profile_idc == 2 ||
        general_profile_compatibility_flag[2] ||
        seq->general_profile_idc == 3 ||
        general_profile_compatibility_flag[3] ||
        seq->general_profile_idc == 4 ||
        general_profile_compatibility_flag[4] ||
        seq->general_profile_idc == 5 ||
        general_profile_compatibility_flag[5] ||
        seq->general_profile_idc == 9 ||
        general_profile_compatibility_flag[9] ||
        seq->general_profile_idc == 11 ||
        general_profile_compatibility_flag[11]) {
      BitstreamAppend(bitstream, 1, general_inbld_flag);
    } else {
      BitstreamAppend(bitstream, 1, 0);  // general_reserved_zero_bit
    }
  }

  BitstreamAppend(bitstream, 8, seq->general_level_idc);
  for (uint8_t i = 0; i < maxNumSubLayersMinus1; i++) {
    // TODO(mburakov): Implement this!
    abort();
  }
  if (maxNumSubLayersMinus1 > 0) {
    for (uint8_t i = maxNumSubLayersMinus1; i < 8; i++)
      BitstreamAppend(bitstream, 2, 0);  // reserved_zero_2bits
  }
  for (uint8_t i = 0; i < maxNumSubLayersMinus1; i++) {
    // TODO(mburakov): Implement this!
    abort();
  }
}

// 7.3.2.11 RBSP trailing bits syntax
static void PackRbspTrailingBits(struct Bitstream* bitstream) {
  BitstreamAppend(bitstream, 1, 1);  // rbsp_stop_one_bit
  BitstreamByteAlign(bitstream);     // rbsp_alignment_zero_bit
}

// 7.3.2.1 Video parameter set RBSP syntax
void PackVideoParameterSetNalUnit(struct Bitstream* bitstream,
                                  const VAEncSequenceParameterBufferHEVC* seq,
                                  const struct MoreVideoParameters* mvp) {
  PackNalUnitHeader(bitstream, VPS_NUT);

  char buffer_on_the_stack[64];
  struct Bitstream vps_rbsp = {
      .data = buffer_on_the_stack,
      .size = 0,
  };

  BitstreamAppend(&vps_rbsp, 4, vps_video_parameter_set_id);
  BitstreamAppend(&vps_rbsp, 1, vps_base_layer_internal_flag);
  BitstreamAppend(&vps_rbsp, 1, vps_base_layer_available_flag);
  BitstreamAppend(&vps_rbsp, 6, vps_max_layers_minus1);
  BitstreamAppend(&vps_rbsp, 3, vps_max_sub_layers_minus1);
  BitstreamAppend(&vps_rbsp, 1, vps_temporal_id_nesting_flag);
  BitstreamAppend(&vps_rbsp, 16, 0xffff);  // vps_reserved_0xffff_16bits

  PackProfileTierLevel(&vps_rbsp, seq, 1, vps_max_sub_layers_minus1);

  BitstreamAppend(&vps_rbsp, 1, vps_sub_layer_ordering_info_present_flag);
  for (uint8_t i = (vps_sub_layer_ordering_info_present_flag
                        ? 0
                        : vps_max_sub_layers_minus1);
       i <= vps_max_sub_layers_minus1; i++) {
    if (i != vps_max_sub_layers_minus1) {
      // TODO(mburakov): Implement this!
      abort();
    }

    BitstreamAppendUE(
        &vps_rbsp, mvp->max_b_depth + 1);  // vps_max_dec_pic_buffering_minus1
    BitstreamAppendUE(&vps_rbsp, mvp->max_b_depth);  // vps_max_num_reorder_pics
    BitstreamAppendUE(&vps_rbsp, vps_max_latency_increase_plus1);
  }

  BitstreamAppend(&vps_rbsp, 6, vps_max_layer_id);
  BitstreamAppendUE(&vps_rbsp, vps_num_layer_sets_minus1);
  for (uint8_t i = 1; i <= vps_max_layers_minus1; i++) {
    for (uint8_t j = 0; j <= vps_max_layer_id; j++) {
      // TODO(mburakov): Implement this!
      abort();
    }
  }

  BitstreamAppend(&vps_rbsp, 1, vps_timing_info_present_flag);
  if (vps_timing_info_present_flag) {
    // TODO(mburakov): Is this section required?

    BitstreamAppend(&vps_rbsp, 32,
                    mvp->time_base_num);  // vps_num_units_in_tick
    BitstreamAppend(&vps_rbsp, 32, mvp->time_base_den);  // vps_time_scale
    BitstreamAppend(&vps_rbsp, 1, vps_poc_proportional_to_timing_flag);
    if (vps_poc_proportional_to_timing_flag) {
      // TODO(mburakov): Implement this!
      abort();
    }

    BitstreamAppendUE(&vps_rbsp, vps_num_hrd_parameters);
    for (uint32_t i = 0; i < vps_num_hrd_parameters; i++) {
      // TODO(mburakov): Implement this!
      abort();
    }
  }

  BitstreamAppend(&vps_rbsp, 1, vps_extension_flag);
  if (vps_extension_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  PackRbspTrailingBits(&vps_rbsp);
  BitstreamInflate(bitstream, &vps_rbsp);
}

// E.2.1 VUI parameters syntax
static void PackVuiParameters(struct Bitstream* bitstream,
                              const struct MoreVideoParameters* mvp,
                              const struct MoreSeqParameters* msp) {
  BitstreamAppend(bitstream, 1, aspect_ratio_info_present_flag);
  if (aspect_ratio_info_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(bitstream, 1, overscan_info_present_flag);
  if (overscan_info_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(bitstream, 1, msp->video_signal_type_present_flag);
  if (msp->video_signal_type_present_flag) {
    BitstreamAppend(bitstream, 3, video_format);
    BitstreamAppend(bitstream, 1, msp->video_full_range_flag);
    BitstreamAppend(bitstream, 1, msp->colour_description_present_flag);
    if (msp->colour_description_present_flag) {
      BitstreamAppend(bitstream, 8, msp->colour_primaries);
      BitstreamAppend(bitstream, 8, msp->transfer_characteristics);
      BitstreamAppend(bitstream, 8, msp->matrix_coeffs);
    }
  }

  BitstreamAppend(bitstream, 1, msp->chroma_loc_info_present_flag);
  if (msp->chroma_loc_info_present_flag) {
    BitstreamAppendUE(bitstream, msp->chroma_sample_loc_type_top_field);
    BitstreamAppendUE(bitstream, msp->chroma_sample_loc_type_bottom_field);
  }

  BitstreamAppend(bitstream, 1, neutral_chroma_indication_flag);
  if (neutral_chroma_indication_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(bitstream, 1, field_seq_flag);
  BitstreamAppend(bitstream, 1, frame_field_info_present_flag);
  BitstreamAppend(bitstream, 1, default_display_window_flag);
  if (default_display_window_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(bitstream, 1, vui_timing_info_present_flag);
  if (vui_timing_info_present_flag) {
    BitstreamAppend(bitstream, 32,
                    mvp->time_base_num);  // vui_num_units_in_tick
    BitstreamAppend(bitstream, 32, mvp->time_base_den);  // vui_time_scale
    BitstreamAppend(bitstream, 1, vui_poc_proportional_to_timing_flag);
    if (vui_poc_proportional_to_timing_flag) {
      // TODO(mburakov): Implement this!
      abort();
    }

    BitstreamAppend(bitstream, 1, vui_hrd_parameters_present_flag);
    if (vui_hrd_parameters_present_flag) {
      // TODO(mburakov): Implement this!
      abort();
    }
  }

  BitstreamAppend(bitstream, 1, bitstream_restriction_flag);
  if (bitstream_restriction_flag) {
    BitstreamAppend(bitstream, 1, tiles_fixed_structure_flag);
    BitstreamAppend(bitstream, 1, motion_vectors_over_pic_boundaries_flag);
    BitstreamAppend(bitstream, 1, restricted_ref_pic_lists_flag);
    BitstreamAppendUE(bitstream, min_spatial_segmentation_idc);
    BitstreamAppendUE(bitstream, max_bytes_per_pic_denom);
    BitstreamAppendUE(bitstream, max_bits_per_min_cu_denom);
    BitstreamAppendUE(bitstream, log2_max_mv_length_horizontal);
    BitstreamAppendUE(bitstream, log2_max_mv_length_vertical);
  }
}

void PackSeqParameterSetNalUnit(struct Bitstream* bitstream,
                                const VAEncSequenceParameterBufferHEVC* seq,
                                const struct MoreVideoParameters* mvp,
                                const struct MoreSeqParameters* msp) {
  const typeof(seq->seq_fields.bits)* seq_bits = &seq->seq_fields.bits;

  PackNalUnitHeader(bitstream, SPS_NUT);

  char buffer_on_the_stack[64];
  struct Bitstream sps_rbsp = {
      .data = buffer_on_the_stack,
      .size = 0,
  };

  BitstreamAppend(&sps_rbsp, 4, sps_video_parameter_set_id);
  BitstreamAppend(&sps_rbsp, 3, sps_max_sub_layers_minus1);
  BitstreamAppend(&sps_rbsp, 1, sps_temporal_id_nesting_flag);

  PackProfileTierLevel(&sps_rbsp, seq, 1, sps_max_sub_layers_minus1);

  BitstreamAppendUE(&sps_rbsp, sps_seq_parameter_set_id);
  BitstreamAppendUE(&sps_rbsp, seq_bits->chroma_format_idc);
  if (seq_bits->chroma_format_idc == 3) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppendUE(&sps_rbsp, seq->pic_width_in_luma_samples);
  BitstreamAppendUE(&sps_rbsp, seq->pic_height_in_luma_samples);

  bool conformance_window_flag =
      msp->crop_width != seq->pic_width_in_luma_samples ||
      msp->crop_height != seq->pic_height_in_luma_samples;
  BitstreamAppend(&sps_rbsp, 1, conformance_window_flag);

  if (conformance_window_flag) {
    if (seq_bits->chroma_format_idc != 1) {
      // TODO(mburakov): Implement this!
      abort();
    }

    // mburakov: Offsets are in chroma samples.
    uint32_t conf_win_right_offset =
        (seq->pic_width_in_luma_samples - msp->crop_width) / 2;
    uint32_t conf_win_bottom_offset =
        (seq->pic_height_in_luma_samples - msp->crop_height) / 2;
    BitstreamAppendUE(&sps_rbsp, 0);  // conf_win_left_offset
    BitstreamAppendUE(&sps_rbsp, conf_win_right_offset);
    BitstreamAppendUE(&sps_rbsp, 0);  // conf_win_top_offset
    BitstreamAppendUE(&sps_rbsp, conf_win_bottom_offset);
  }

  BitstreamAppendUE(&sps_rbsp, seq_bits->bit_depth_luma_minus8);
  BitstreamAppendUE(&sps_rbsp, seq_bits->bit_depth_chroma_minus8);
  BitstreamAppendUE(&sps_rbsp, log2_max_pic_order_cnt_lsb_minus4);

  BitstreamAppend(&sps_rbsp, 1, sps_sub_layer_ordering_info_present_flag);
  for (uint8_t i = (sps_sub_layer_ordering_info_present_flag
                        ? 0
                        : sps_max_sub_layers_minus1);
       i <= sps_max_sub_layers_minus1; i++) {
    if (i != sps_max_sub_layers_minus1) {
      // TODO(mburakov): Implement this!
      abort();
    }

    BitstreamAppendUE(
        &sps_rbsp, mvp->max_b_depth + 1);  // vps_max_dec_pic_buffering_minus1
    BitstreamAppendUE(&sps_rbsp, mvp->max_b_depth);  // vps_max_num_reorder_pics
    BitstreamAppendUE(&sps_rbsp, sps_max_latency_increase_plus1);
  }

  BitstreamAppendUE(&sps_rbsp, seq->log2_min_luma_coding_block_size_minus3);
  BitstreamAppendUE(&sps_rbsp, seq->log2_diff_max_min_luma_coding_block_size);
  BitstreamAppendUE(&sps_rbsp, seq->log2_min_transform_block_size_minus2);
  BitstreamAppendUE(&sps_rbsp, seq->log2_diff_max_min_transform_block_size);
  BitstreamAppendUE(&sps_rbsp, seq->max_transform_hierarchy_depth_inter);
  BitstreamAppendUE(&sps_rbsp, seq->max_transform_hierarchy_depth_intra);

  BitstreamAppend(&sps_rbsp, 1, seq_bits->scaling_list_enabled_flag);
  if (seq_bits->scaling_list_enabled_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(&sps_rbsp, 1, seq_bits->amp_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1, seq_bits->sample_adaptive_offset_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1, seq_bits->pcm_enabled_flag);
  if (seq_bits->pcm_enabled_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppendUE(&sps_rbsp, num_short_term_ref_pic_sets);
  for (uint32_t i = 0; i < num_short_term_ref_pic_sets; i++) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(&sps_rbsp, 1, long_term_ref_pics_present_flag);
  if (long_term_ref_pics_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(&sps_rbsp, 1, seq_bits->sps_temporal_mvp_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1, seq_bits->strong_intra_smoothing_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1, vui_parameters_present_flag);
  if (vui_parameters_present_flag) PackVuiParameters(&sps_rbsp, mvp, msp);
  BitstreamAppend(&sps_rbsp, 1, sps_extension_present_flag);
  if (sps_extension_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  PackRbspTrailingBits(&sps_rbsp);
  BitstreamInflate(bitstream, &sps_rbsp);
}

// 7.3.2.3.1 General picture parameter set RBSP syntax
void PackPicParameterSetNalUnit(struct Bitstream* bitstream,
                                const VAEncPictureParameterBufferHEVC* pic) {
  const typeof(pic->pic_fields.bits)* pic_bits = &pic->pic_fields.bits;

  PackNalUnitHeader(bitstream, PPS_NUT);

  char buffer_on_the_stack[64];
  struct Bitstream pps_rbsp = {
      .data = buffer_on_the_stack,
      .size = 0,
  };

  BitstreamAppendUE(&pps_rbsp, pps_pic_parameter_set_id);
  BitstreamAppendUE(&pps_rbsp, pps_seq_parameter_set_id);
  BitstreamAppend(&pps_rbsp, 1,
                  pic_bits->dependent_slice_segments_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, output_flag_present_flag);
  BitstreamAppend(&pps_rbsp, 3, num_extra_slice_header_bits);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->sign_data_hiding_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, cabac_init_present_flag);
  BitstreamAppendUE(&pps_rbsp, pic->num_ref_idx_l0_default_active_minus1);
  BitstreamAppendUE(&pps_rbsp, pic->num_ref_idx_l1_default_active_minus1);
  BitstreamAppendSE(&pps_rbsp,
                    pic->pic_init_qp - 26);  // init_qp_minus26
  BitstreamAppend(&pps_rbsp, 1, pic_bits->constrained_intra_pred_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->transform_skip_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->cu_qp_delta_enabled_flag);
  if (pic_bits->cu_qp_delta_enabled_flag)
    BitstreamAppendUE(&pps_rbsp, pic->diff_cu_qp_delta_depth);
  BitstreamAppendSE(&pps_rbsp, pic->pps_cb_qp_offset);
  BitstreamAppendSE(&pps_rbsp, pic->pps_cr_qp_offset);
  BitstreamAppend(&pps_rbsp, 1, pps_slice_chroma_qp_offsets_present_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->weighted_pred_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->weighted_bipred_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->transquant_bypass_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->tiles_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, pic_bits->entropy_coding_sync_enabled_flag);

  if (pic_bits->tiles_enabled_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(&pps_rbsp, 1,
                  pic_bits->pps_loop_filter_across_slices_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, deblocking_filter_control_present_flag);
  if (deblocking_filter_control_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(&pps_rbsp, 1, pic_bits->scaling_list_data_present_flag);
  if (pic_bits->scaling_list_data_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  BitstreamAppend(&pps_rbsp, 1, lists_modification_present_flag);
  BitstreamAppendUE(&pps_rbsp, pic->log2_parallel_merge_level_minus2);
  BitstreamAppend(&pps_rbsp, 1, slice_segment_header_extension_present_flag);
  BitstreamAppend(&pps_rbsp, 1, pps_extension_present_flag);

  if (pps_extension_present_flag) {
    // TODO(mburakov): Implement this!
    abort();
  }

  PackRbspTrailingBits(&pps_rbsp);
  BitstreamInflate(bitstream, &pps_rbsp);
}

// 7.3.7 Short-term reference picture set syntax
static void PackStRefPicSet(struct Bitstream* bitstream, uint32_t stRpsIdx,
                            const struct MoreSliceParamerters* msp) {
  if (stRpsIdx != 0)
    BitstreamAppend(bitstream, 1, inter_ref_pic_set_prediction_flag);
  if (inter_ref_pic_set_prediction_flag) {
    // TODO(mburakov): Implement this!
    abort();
  } else {
    BitstreamAppendUE(bitstream, msp->num_negative_pics);
    BitstreamAppendUE(bitstream, msp->num_positive_pics);
    for (uint32_t i = 0; i < msp->num_negative_pics; i++) {
      BitstreamAppendUE(bitstream, msp->negative_pics[i].delta_poc_s0_minus1);
      BitstreamAppend(bitstream, 1,
                      msp->negative_pics[i].used_by_curr_pic_s0_flag);
    }
    for (uint32_t i = 0; i < msp->num_positive_pics; i++) {
      BitstreamAppendUE(bitstream, msp->positive_pics[i].delta_poc_s1_minus1);
      BitstreamAppend(bitstream, 1,
                      msp->positive_pics[i].used_by_curr_pic_s1_flag);
    }
  }
}

// 7.3.6.1 General slice segment header syntax
void PackSliceSegmentHeaderNalUnit(struct Bitstream* bitstream,
                                   const VAEncSequenceParameterBufferHEVC* seq,
                                   const VAEncPictureParameterBufferHEVC* pic,
                                   const VAEncSliceParameterBufferHEVC* slice,
                                   const struct MoreSliceParamerters* msp) {
  const typeof(seq->seq_fields.bits)* seq_bits = &seq->seq_fields.bits;
  const typeof(pic->pic_fields.bits)* pic_bits = &pic->pic_fields.bits;
  const typeof(slice->slice_fields.bits)* slice_bits =
      &slice->slice_fields.bits;

  PackNalUnitHeader(bitstream, pic->nal_unit_type);

  char buffer_on_the_stack[64];
  struct Bitstream slice_rbsp = {
      .data = buffer_on_the_stack,
      .size = 0,
  };

  BitstreamAppend(&slice_rbsp, 1, msp->first_slice_segment_in_pic_flag);
  if (pic->nal_unit_type >= BLA_W_LP && pic->nal_unit_type <= RSV_IRAP_VCL23)
    BitstreamAppend(&slice_rbsp, 1, pic_bits->no_output_of_prior_pics_flag);
  BitstreamAppendUE(&slice_rbsp, slice->slice_pic_parameter_set_id);
  if (!msp->first_slice_segment_in_pic_flag) {
    if (pic_bits->dependent_slice_segments_enabled_flag)
      BitstreamAppend(&slice_rbsp, 1, slice_bits->dependent_slice_segment_flag);
    // TODO(mburakov): Implement this!!!
    abort();
  }

  if (!slice_bits->dependent_slice_segment_flag) {
    for (uint32_t i = 0; i < num_extra_slice_header_bits; i++) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    BitstreamAppendUE(&slice_rbsp, slice->slice_type);
    if (output_flag_present_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (seq_bits->separate_colour_plane_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (pic->nal_unit_type != IDR_W_RADL && pic->nal_unit_type != IDR_N_LP) {
      uint32_t slice_pic_order_cnt_lsb =
          pic->decoded_curr_pic.pic_order_cnt &
          (1 << (log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1;
      BitstreamAppend(&slice_rbsp, log2_max_pic_order_cnt_lsb_minus4 + 4,
                      slice_pic_order_cnt_lsb);
      BitstreamAppend(&slice_rbsp, 1, short_term_ref_pic_set_sps_flag);
      if (!short_term_ref_pic_set_sps_flag)
        PackStRefPicSet(&slice_rbsp, num_short_term_ref_pic_sets, msp);
      else if (num_short_term_ref_pic_sets > 1) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      if (long_term_ref_pics_present_flag) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      if (seq_bits->sps_temporal_mvp_enabled_flag) {
        BitstreamAppend(&slice_rbsp, 1,
                        slice_bits->slice_temporal_mvp_enabled_flag);
      }
    }
    if (seq_bits->sample_adaptive_offset_enabled_flag) {
      BitstreamAppend(&slice_rbsp, 1, slice_bits->slice_sao_luma_flag);
      uint32_t ChromaArrayType = !seq_bits->separate_colour_plane_flag
                                     ? seq_bits->chroma_format_idc
                                     : 0;
      if (ChromaArrayType != 0)
        BitstreamAppend(&slice_rbsp, 1, slice_bits->slice_sao_chroma_flag);
    }
    if (slice->slice_type == P || slice->slice_type == B) {
      BitstreamAppend(&slice_rbsp, 1,
                      slice_bits->num_ref_idx_active_override_flag);
      if (slice_bits->num_ref_idx_active_override_flag) {
        BitstreamAppendUE(&slice_rbsp, slice->num_ref_idx_l0_active_minus1);
        if (slice->slice_type == B)
          BitstreamAppendUE(&slice_rbsp, slice->num_ref_idx_l1_active_minus1);
      }
      if (lists_modification_present_flag /* && NumPicTotalCurr > 1*/) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      if (slice->slice_type == B)
        BitstreamAppend(&slice_rbsp, 1, slice_bits->mvd_l1_zero_flag);
      if (cabac_init_present_flag)
        BitstreamAppend(&slice_rbsp, 1, slice_bits->cabac_init_flag);
      if (slice_bits->slice_temporal_mvp_enabled_flag) {
        if (slice->slice_type == B)
          BitstreamAppend(&slice_rbsp, 1, slice_bits->collocated_from_l0_flag);
        if ((slice_bits->collocated_from_l0_flag &&
             slice->num_ref_idx_l0_active_minus1 > 0) ||
            (!slice_bits->collocated_from_l0_flag &&
             slice->num_ref_idx_l1_active_minus1 > 0))
          BitstreamAppendUE(&slice_rbsp, pic->collocated_ref_pic_index);
      }
      if ((pic_bits->weighted_pred_flag && slice->slice_type == P) ||
          (pic_bits->weighted_bipred_flag && slice->slice_type == B)) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      BitstreamAppendUE(
          &slice_rbsp,
          5 - slice->max_num_merge_cand);  // five_minus_max_num_merge_cand
      if (motion_vector_resolution_control_idc == 2) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
    }
    BitstreamAppendSE(&slice_rbsp, slice->slice_qp_delta);
    if (pps_slice_chroma_qp_offsets_present_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (pps_slice_act_qp_offsets_present_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (chroma_qp_offset_list_enabled_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (deblocking_filter_override_enabled_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (deblocking_filter_override_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (pic_bits->pps_loop_filter_across_slices_enabled_flag &&
        (slice_bits->slice_sao_luma_flag || slice_bits->slice_sao_chroma_flag ||
         !slice_bits->slice_deblocking_filter_disabled_flag)) {
      BitstreamAppend(&slice_rbsp, 1,
                      slice_bits->slice_loop_filter_across_slices_enabled_flag);
    }
  }
  if (pic_bits->tiles_enabled_flag ||
      pic_bits->entropy_coding_sync_enabled_flag) {
    BitstreamAppendUE(&slice_rbsp, num_entry_point_offsets);
    if (num_entry_point_offsets > 0) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
  }
  if (slice_segment_header_extension_present_flag) {
    // TODO(mburakov): Implement this!!!
    abort();
  }

  PackRbspTrailingBits(&slice_rbsp);
  BitstreamInflate(bitstream, &slice_rbsp);
}
