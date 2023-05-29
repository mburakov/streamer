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

#include "encode.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include "bitstream.h"
#include "gpu.h"
#include "hevc.h"
#include "toolbox/utils.h"

struct EncodeContext {
  struct GpuContext* gpu_context;
  uint32_t width;
  uint32_t height;
  enum YuvColorspace colorspace;
  enum YuvRange range;

  int render_node;
  VADisplay va_display;
  VAConfigID va_config_id;

  uint32_t va_packed_headers;
  VAConfigAttribValEncHEVCFeatures va_hevc_features;
  VAConfigAttribValEncHEVCBlockSizes va_hevc_block_sizes;

  VAContextID va_context_id;
  VASurfaceID input_surface_id;
  struct GpuFrame* gpu_frame;

  VASurfaceID recon_surface_ids[2];
  VABufferID output_buffer_id;

  VAEncSequenceParameterBufferHEVC seq;
  VAEncPictureParameterBufferHEVC pic;
  VAEncSliceParameterBufferHEVC slice;
  size_t frame_counter;
};

static const char* VaErrorString(VAStatus error) {
  static const char* va_error_strings[] = {
      "VA_STATUS_SUCCESS",
      "VA_STATUS_ERROR_OPERATION_FAILED",
      "VA_STATUS_ERROR_ALLOCATION_FAILED",
      "VA_STATUS_ERROR_INVALID_DISPLAY",
      "VA_STATUS_ERROR_INVALID_CONFIG",
      "VA_STATUS_ERROR_INVALID_CONTEXT",
      "VA_STATUS_ERROR_INVALID_SURFACE",
      "VA_STATUS_ERROR_INVALID_BUFFER",
      "VA_STATUS_ERROR_INVALID_IMAGE",
      "VA_STATUS_ERROR_INVALID_SUBPICTURE",
      "VA_STATUS_ERROR_ATTR_NOT_SUPPORTED",
      "VA_STATUS_ERROR_MAX_NUM_EXCEEDED",
      "VA_STATUS_ERROR_UNSUPPORTED_PROFILE",
      "VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT",
      "VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT",
      "VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE",
      "VA_STATUS_ERROR_SURFACE_BUSY",
      "VA_STATUS_ERROR_FLAG_NOT_SUPPORTED",
      "VA_STATUS_ERROR_INVALID_PARAMETER",
      "VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED",
      "VA_STATUS_ERROR_UNIMPLEMENTED",
      "VA_STATUS_ERROR_SURFACE_IN_DISPLAYING",
      "VA_STATUS_ERROR_INVALID_IMAGE_FORMAT",
      "VA_STATUS_ERROR_DECODING_ERROR",
      "VA_STATUS_ERROR_ENCODING_ERROR",
      "VA_STATUS_ERROR_INVALID_VALUE",
      "???",
      "???",
      "???",
      "???",
      "???",
      "???",
      "VA_STATUS_ERROR_UNSUPPORTED_FILTER",
      "VA_STATUS_ERROR_INVALID_FILTER_CHAIN",
      "VA_STATUS_ERROR_HW_BUSY",
      "???",
      "VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE",
      "VA_STATUS_ERROR_NOT_ENOUGH_BUFFER",
      "VA_STATUS_ERROR_TIMEDOUT",
  };
  return VA_STATUS_SUCCESS <= error && error <= VA_STATUS_ERROR_TIMEDOUT
             ? va_error_strings[error - VA_STATUS_SUCCESS]
             : "???";
}

static void OnVaLogMessage(void* context, const char* message) {
  (void)context;
  size_t len = strlen(message);
  while (message[len - 1] == '\n') len--;
  LOG("%.*s", (int)len, message);
}

static bool InitializeCodecCaps(struct EncodeContext* encode_context) {
  VAConfigAttrib attrib_list[] = {
      {.type = VAConfigAttribEncPackedHeaders},
      {.type = VAConfigAttribEncHEVCFeatures},
      {.type = VAConfigAttribEncHEVCBlockSizes},
  };
  VAStatus status = vaGetConfigAttributes(
      encode_context->va_display, VAProfileHEVCMain, VAEntrypointEncSlice,
      attrib_list, LENGTH(attrib_list));
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to get va config attributes (%s)", VaErrorString(status));
    return false;
  }

  if (attrib_list[0].value == VA_ATTRIB_NOT_SUPPORTED) {
    LOG("VAConfigAttribEncPackedHeaders is not supported");
  } else {
    LOG("VAConfigAttribEncPackedHeaders is 0x%08x", attrib_list[0].value);
    encode_context->va_packed_headers = attrib_list[0].value;
  }

  if (attrib_list[1].value == VA_ATTRIB_NOT_SUPPORTED) {
    LOG("VAConfigAttribEncHEVCFeatures is not supported");
    encode_context->va_hevc_features = (VAConfigAttribValEncHEVCFeatures){
        .bits =
            {
                // mburakov: ffmpeg hardcodes these for i965 Skylake driver.
                .separate_colour_planes = 0,     // Table 6-1
                .scaling_lists = 0,              // No scaling lists
                .amp = 1,                        // hardcoded
                .sao = 0,                        // hardcoded
                .pcm = 0,                        // hardcoded
                .temporal_mvp = 0,               // hardcoded
                .strong_intra_smoothing = 0,     // TODO
                .dependent_slices = 0,           // No slice segments
                .sign_data_hiding = 0,           // TODO
                .constrained_intra_pred = 0,     // TODO
                .transform_skip = 0,             // defaulted
                .cu_qp_delta = 0,                // Fixed quality
                .weighted_prediction = 0,        // TODO
                .transquant_bypass = 0,          // TODO
                .deblocking_filter_disable = 0,  // TODO
            },
    };
  } else {
    LOG("VAConfigAttribEncHEVCFeatures is 0x%08x", attrib_list[1].value);
    encode_context->va_hevc_features.value = attrib_list[1].value;
  }

  if (attrib_list[2].value == VA_ATTRIB_NOT_SUPPORTED) {
    LOG("VAConfigAttribEncHEVCBlockSizes is not supported");
    encode_context->va_hevc_block_sizes = (VAConfigAttribValEncHEVCBlockSizes){
        .bits =
            {
                // mburakov: ffmpeg hardcodes these for i965 Skylake driver.
                .log2_max_coding_tree_block_size_minus3 = 2,     // hardcoded
                .log2_min_coding_tree_block_size_minus3 = 0,     // TODO
                .log2_min_luma_coding_block_size_minus3 = 0,     // hardcoded
                .log2_max_luma_transform_block_size_minus2 = 3,  // hardcoded
                .log2_min_luma_transform_block_size_minus2 = 0,  // hardcoded
                .max_max_transform_hierarchy_depth_inter = 3,    // hardcoded
                .min_max_transform_hierarchy_depth_inter = 0,    // defaulted
                .max_max_transform_hierarchy_depth_intra = 3,    // hardcoded
                .min_max_transform_hierarchy_depth_intra = 0,    // defaulted
                .log2_max_pcm_coding_block_size_minus3 = 0,      // TODO
                .log2_min_pcm_coding_block_size_minus3 = 0,      // TODO
            },
    };
  } else {
    LOG("VAConfigAttribEncHEVCBlockSizes is 0x%08x", attrib_list[2].value);
    encode_context->va_hevc_block_sizes.value = attrib_list[2].value;
  }

#ifndef NDEBUG
  const typeof(encode_context->va_hevc_features.bits)* features_bits =
      &encode_context->va_hevc_features.bits;
  const typeof(encode_context->va_hevc_block_sizes.bits)* block_sizes_bits =
      &encode_context->va_hevc_block_sizes.bits;
  LOG("VAConfigAttribEncHEVCFeatures dump:"
      "\n\tseparate_colour_planes = %u"
      "\n\tscaling_lists = %u"
      "\n\tamp = %u"
      "\n\tsao = %u"
      "\n\tpcm = %u"
      "\n\ttemporal_mvp = %u"
      "\n\tstrong_intra_smoothing = %u"
      "\n\tdependent_slices = %u"
      "\n\tsign_data_hiding = %u"
      "\n\tconstrained_intra_pred = %u"
      "\n\ttransform_skip = %u"
      "\n\tcu_qp_delta = %u"
      "\n\tweighted_prediction = %u"
      "\n\ttransquant_bypass = %u"
      "\n\tdeblocking_filter_disable = %u",
      features_bits->separate_colour_planes, features_bits->scaling_lists,
      features_bits->amp, features_bits->sao, features_bits->pcm,
      features_bits->temporal_mvp, features_bits->strong_intra_smoothing,
      features_bits->dependent_slices, features_bits->sign_data_hiding,
      features_bits->constrained_intra_pred, features_bits->transform_skip,
      features_bits->cu_qp_delta, features_bits->weighted_prediction,
      features_bits->transquant_bypass,
      features_bits->deblocking_filter_disable);
  LOG("VAConfigAttribEncHEVCBlockSizes dump:"
      "\n\tlog2_max_coding_tree_block_size_minus3 = %u"
      "\n\tlog2_min_coding_tree_block_size_minus3 = %u"
      "\n\tlog2_min_luma_coding_block_size_minus3 = %u"
      "\n\tlog2_max_luma_transform_block_size_minus2 = %u"
      "\n\tlog2_min_luma_transform_block_size_minus2 = %u"
      "\n\tmax_max_transform_hierarchy_depth_inter = %u"
      "\n\tmin_max_transform_hierarchy_depth_inter = %u"
      "\n\tmax_max_transform_hierarchy_depth_intra = %u"
      "\n\tmin_max_transform_hierarchy_depth_intra = %u"
      "\n\tlog2_max_pcm_coding_block_size_minus3 = %u"
      "\n\tlog2_min_pcm_coding_block_size_minus3 = %u",
      block_sizes_bits->log2_max_coding_tree_block_size_minus3,
      block_sizes_bits->log2_min_coding_tree_block_size_minus3,
      block_sizes_bits->log2_min_luma_coding_block_size_minus3,
      block_sizes_bits->log2_max_luma_transform_block_size_minus2,
      block_sizes_bits->log2_min_luma_transform_block_size_minus2,
      block_sizes_bits->max_max_transform_hierarchy_depth_inter,
      block_sizes_bits->min_max_transform_hierarchy_depth_inter,
      block_sizes_bits->max_max_transform_hierarchy_depth_intra,
      block_sizes_bits->min_max_transform_hierarchy_depth_intra,
      block_sizes_bits->log2_max_pcm_coding_block_size_minus3,
      block_sizes_bits->log2_min_pcm_coding_block_size_minus3);
#endif

  return true;
}

static struct GpuFrame* VaSurfaceToGpuFrame(VADisplay va_display,
                                            VASurfaceID va_surface_id,
                                            struct GpuContext* gpu_context) {
  VADRMPRIMESurfaceDescriptor prime;
  VAStatus status = vaExportSurfaceHandle(
      va_display, va_surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to export va surface (%s)", VaErrorString(status));
    return NULL;
  }

  struct GpuFramePlane planes[] = {{.dmabuf_fd = -1},
                                   {.dmabuf_fd = -1},
                                   {.dmabuf_fd = -1},
                                   {.dmabuf_fd = -1}};
  static_assert(LENGTH(planes) == LENGTH(prime.layers[0].object_index),
                "Suspicious VADRMPRIMESurfaceDescriptor structure");

  for (size_t i = 0; i < prime.layers[0].num_planes; i++) {
    uint32_t object_index = prime.layers[0].object_index[i];
    planes[i] = (struct GpuFramePlane){
        .dmabuf_fd = prime.objects[object_index].fd,
        .pitch = prime.layers[0].pitch[i],
        .offset = prime.layers[0].offset[i],
        .modifier = prime.objects[object_index].drm_format_modifier,
    };
  }

  struct GpuFrame* gpu_frame =
      GpuContextCreateFrame(gpu_context, prime.width, prime.height,
                            prime.fourcc, prime.layers[0].num_planes, planes);
  if (!gpu_frame) {
    LOG("Failed to create gpu frame");
    goto release_planes;
  }
  return gpu_frame;

release_planes:
  CloseUniqueFds((int[]){planes[0].dmabuf_fd, planes[1].dmabuf_fd,
                         planes[2].dmabuf_fd, planes[3].dmabuf_fd});
  return NULL;
}

static void InitializeSeqHeader(struct EncodeContext* encode_context,
                                uint16_t pic_width_in_luma_samples,
                                uint16_t pic_height_in_luma_samples) {
  const typeof(encode_context->va_hevc_features.bits)* features_bits =
      &encode_context->va_hevc_features.bits;
  const typeof(encode_context->va_hevc_block_sizes.bits)* block_sizes_bits =
      &encode_context->va_hevc_block_sizes.bits;

  uint8_t log2_diff_max_min_luma_coding_block_size =
      block_sizes_bits->log2_max_coding_tree_block_size_minus3 -
      block_sizes_bits->log2_min_luma_coding_block_size_minus3;
  uint8_t log2_diff_max_min_transform_block_size =
      block_sizes_bits->log2_max_luma_transform_block_size_minus2 -
      block_sizes_bits->log2_min_luma_transform_block_size_minus2;

  encode_context->seq = (VAEncSequenceParameterBufferHEVC){
      .general_profile_idc = 1,  // Main profile
      .general_level_idc = 120,  // Level 4
      .general_tier_flag = 0,    // Main tier

      .intra_period = 120,      // Where this one comes from?
      .intra_idr_period = 120,  // Each I frame is an IDR frame
      .ip_period = 1,           // No B-frames
      .bits_per_second = 0,     // TODO

      .pic_width_in_luma_samples = pic_width_in_luma_samples,
      .pic_height_in_luma_samples = pic_height_in_luma_samples,

      .seq_fields.bits =
          {
              .chroma_format_idc = 1,                    // 4:2:0
              .separate_colour_plane_flag = 0,           // Table 6-1
              .bit_depth_luma_minus8 = 0,                // 8 bpp luma
              .bit_depth_chroma_minus8 = 0,              // 8 bpp chroma
              .scaling_list_enabled_flag = 0,            // No scaling lists
              .strong_intra_smoothing_enabled_flag = 0,  // defaulted

              .amp_enabled_flag = features_bits->amp,
              .sample_adaptive_offset_enabled_flag = features_bits->sao,
              .pcm_enabled_flag = features_bits->pcm,
              .pcm_loop_filter_disabled_flag = 0,  // defaulted
              .sps_temporal_mvp_enabled_flag = features_bits->temporal_mvp,

              .low_delay_seq = 1,     // No B-frames
              .hierachical_flag = 0,  // defaulted
          },

      .log2_min_luma_coding_block_size_minus3 =
          block_sizes_bits->log2_min_luma_coding_block_size_minus3,
      .log2_diff_max_min_luma_coding_block_size =
          log2_diff_max_min_luma_coding_block_size,
      .log2_min_transform_block_size_minus2 =
          block_sizes_bits->log2_min_luma_transform_block_size_minus2,
      .log2_diff_max_min_transform_block_size =
          log2_diff_max_min_transform_block_size,
      .max_transform_hierarchy_depth_inter =
          block_sizes_bits->max_max_transform_hierarchy_depth_inter,
      .max_transform_hierarchy_depth_intra =
          block_sizes_bits->max_max_transform_hierarchy_depth_intra,

      .pcm_sample_bit_depth_luma_minus1 = 0,            // defaulted
      .pcm_sample_bit_depth_chroma_minus1 = 0,          // defaulted
      .log2_min_pcm_luma_coding_block_size_minus3 = 0,  // defaulted
      .log2_max_pcm_luma_coding_block_size_minus3 = 0,  // defaulted

      .vui_parameters_present_flag = 1,
      .vui_fields.bits =
          {
              .aspect_ratio_info_present_flag = 0,           // defaulted
              .neutral_chroma_indication_flag = 0,           // defaulted
              .field_seq_flag = 0,                           // defaulted
              .vui_timing_info_present_flag = 0,             // No timing
              .bitstream_restriction_flag = 1,               // hardcoded
              .tiles_fixed_structure_flag = 0,               // defaulted
              .motion_vectors_over_pic_boundaries_flag = 1,  // hardcoded
              .restricted_ref_pic_lists_flag = 1,            // hardcoded
              .log2_max_mv_length_horizontal = 15,           // hardcoded
              .log2_max_mv_length_vertical = 15,             // hardcoded
          },

      .vui_num_units_in_tick = 0,         // No timing
      .vui_time_scale = 0,                // No timing
      .min_spatial_segmentation_idc = 0,  // defaulted
      .max_bytes_per_pic_denom = 0,       // hardcoded
      .max_bits_per_min_cu_denom = 0,     // hardcoded

      .scc_fields.bits =
          {
              .palette_mode_enabled_flag = 0,  // defaulted
          },
  };
}

static void InitializePicHeader(struct EncodeContext* encode_context) {
  const typeof(encode_context->seq.seq_fields.bits)* seq_bits =
      &encode_context->seq.seq_fields.bits;
  const typeof(encode_context->va_hevc_features.bits)* features_bits =
      &encode_context->va_hevc_features.bits;

  uint8_t collocated_ref_pic_index =
      seq_bits->sps_temporal_mvp_enabled_flag ? 0 : 0xff;

  encode_context->pic = (VAEncPictureParameterBufferHEVC){
      .decoded_curr_pic =
          {
              .picture_id = VA_INVALID_ID,       // dynamic
              .flags = VA_PICTURE_HEVC_INVALID,  // dynamic
          },

      // .reference_frames[15],

      .coded_buf = encode_context->output_buffer_id,
      .collocated_ref_pic_index = collocated_ref_pic_index,
      .last_picture = 0,  // hardcoded

      .pic_init_qp = 30,            // Fixed quality
      .diff_cu_qp_delta_depth = 0,  // Fixed quality
      .pps_cb_qp_offset = 0,        // hardcoded
      .pps_cr_qp_offset = 0,        // hardcoded

      .num_tile_columns_minus1 = 0,  // No tiles
      .num_tile_rows_minus1 = 0,     // No tiles
      .column_width_minus1 = {0},    // No tiles
      .row_height_minus1 = {0},      // No tiles

      .log2_parallel_merge_level_minus2 = 0,      // defaulted
      .ctu_max_bitsize_allowed = 0,               // hardcoded
      .num_ref_idx_l0_default_active_minus1 = 0,  // hardcoded
      .num_ref_idx_l1_default_active_minus1 = 0,  // hardcoded
      .slice_pic_parameter_set_id = 0,            // hardcoded
      .nal_unit_type = 0,                         // dynamic

      .pic_fields.bits =
          {
              .idr_pic_flag = 0,        // dynamic
              .coding_type = 0,         // dynamic
              .reference_pic_flag = 1,  // No B-frames

              .dependent_slice_segments_enabled_flag = 0,  // defaulted
              .sign_data_hiding_enabled_flag = 0,          // defaulted
              .constrained_intra_pred_flag = 0,            // defaulted
              .transform_skip_enabled_flag = features_bits->transform_skip,
              .cu_qp_delta_enabled_flag = 0,               // Fixed quality
              .weighted_pred_flag = 0,                     // defaulted
              .weighted_bipred_flag = 0,                   // defaulted
              .transquant_bypass_enabled_flag = 0,         // defaulted
              .tiles_enabled_flag = 0,                     // No tiles
              .entropy_coding_sync_enabled_flag = 0,       // defaulted
              .loop_filter_across_tiles_enabled_flag = 0,  // No tiles

              .pps_loop_filter_across_slices_enabled_flag = 1,  // hardcoded
              .scaling_list_data_present_flag = 0,  // No scaling lists

              .screen_content_flag = 0,             // TODO
              .enable_gpu_weighted_prediction = 0,  // hardcoded
              .no_output_of_prior_pics_flag = 0,    // hardcoded
          },

      .hierarchical_level_plus1 = 0,  // defaulted
      .scc_fields.bits =
          {
              .pps_curr_pic_ref_enabled_flag = 0,  // defaulted
          },
  };

  for (size_t i = 0; i < LENGTH(encode_context->pic.reference_frames); i++) {
    encode_context->pic.reference_frames[i] = (VAPictureHEVC){
        .picture_id = VA_INVALID_ID,
        .flags = VA_PICTURE_HEVC_INVALID,
    };
  }
}

static void InitializeSliceHeader(struct EncodeContext* encode_context) {
  const typeof(encode_context->seq.seq_fields.bits)* seq_bits =
      &encode_context->seq.seq_fields.bits;
  const typeof(encode_context->va_hevc_block_sizes.bits)* block_sizes_bits =
      &encode_context->va_hevc_block_sizes.bits;

  uint32_t ctu_size =
      1 << (block_sizes_bits->log2_max_coding_tree_block_size_minus3 + 3);
  uint32_t slice_block_rows =
      (encode_context->height + ctu_size - 1) / ctu_size;
  uint32_t slice_block_cols = (encode_context->width + ctu_size - 1) / ctu_size;
  uint32_t num_ctu_in_slice = slice_block_rows * slice_block_cols;

  encode_context->slice = (VAEncSliceParameterBufferHEVC){
      .slice_segment_address = 0,  // No slice segments
      .num_ctu_in_slice = num_ctu_in_slice,

      .slice_type = 0,  // dynamic
      .slice_pic_parameter_set_id =
          encode_context->pic.slice_pic_parameter_set_id,

      .num_ref_idx_l0_active_minus1 =
          encode_context->pic.num_ref_idx_l0_default_active_minus1,
      .num_ref_idx_l1_active_minus1 =
          encode_context->pic.num_ref_idx_l1_default_active_minus1,

      .luma_log2_weight_denom = 0,          // defaulted
      .delta_chroma_log2_weight_denom = 0,  // defaulted

      // .delta_luma_weight_l0[15],
      // .luma_offset_l0[15],
      // .delta_chroma_weight_l0[15][2],
      // .chroma_offset_l0[15][2],
      // .delta_luma_weight_l1[15],
      // .luma_offset_l1[15],
      // .delta_chroma_weight_l1[15][2],
      // .chroma_offset_l1[15][2],

      .max_num_merge_cand = 5,  // defaulted
      .slice_qp_delta = 0,      // Fixed quality
      .slice_cb_qp_offset = 0,  // defaulted
      .slice_cr_qp_offset = 0,  // defaulted

      .slice_beta_offset_div2 = 0,  // defaulted
      .slice_tc_offset_div2 = 0,    // defaulted

      .slice_fields.bits =
          {
              .last_slice_of_pic_flag = 1,        // No slice segments
              .dependent_slice_segment_flag = 0,  // No slice segments
              .colour_plane_id = 0,               // defaulted
              .slice_temporal_mvp_enabled_flag =
                  seq_bits->sps_temporal_mvp_enabled_flag,
              .slice_sao_luma_flag =
                  seq_bits->sample_adaptive_offset_enabled_flag,
              .slice_sao_chroma_flag =
                  seq_bits->sample_adaptive_offset_enabled_flag,
              .num_ref_idx_active_override_flag = 0,              // hardcoded
              .mvd_l1_zero_flag = 0,                              // defaulted
              .cabac_init_flag = 0,                               // defaulted
              .slice_deblocking_filter_disabled_flag = 0,         // defaulted
              .slice_loop_filter_across_slices_enabled_flag = 0,  // defaulted
              .collocated_from_l0_flag = 0,                       // No B-frames
          },

      .pred_weight_table_bit_offset = 0,  // defaulted
      .pred_weight_table_bit_length = 0,  // defaulted
  };

  for (size_t i = 0; i < LENGTH(encode_context->slice.ref_pic_list0); i++) {
    encode_context->slice.ref_pic_list0[i] = (VAPictureHEVC){
        .picture_id = VA_INVALID_ID,
        .flags = VA_PICTURE_HEVC_INVALID,
    };
  }

  for (size_t i = 0; i < LENGTH(encode_context->slice.ref_pic_list1); i++) {
    encode_context->slice.ref_pic_list1[i] = (VAPictureHEVC){
        .picture_id = VA_INVALID_ID,
        .flags = VA_PICTURE_HEVC_INVALID,
    };
  }
}

struct EncodeContext* EncodeContextCreate(struct GpuContext* gpu_context,
                                          uint32_t width, uint32_t height,
                                          enum YuvColorspace colorspace,
                                          enum YuvRange range) {
  struct EncodeContext* encode_context = malloc(sizeof(struct EncodeContext));
  if (!encode_context) {
    LOG("Faield to allocate encode context (%s)", strerror(errno));
    return NULL;
  }

  *encode_context = (struct EncodeContext){
      .gpu_context = gpu_context,
      .width = width,
      .height = height,
      .colorspace = colorspace,
      .range = range,
  };

  encode_context->render_node = open("/dev/dri/renderD128", O_RDWR);
  if (encode_context->render_node == -1) {
    LOG("Failed to open render node (%s)", strerror(errno));
    goto rollback_encode_context;
  }

  encode_context->va_display = vaGetDisplayDRM(encode_context->render_node);
  if (!encode_context->va_display) {
    LOG("Failed to get va display (%s)", strerror(errno));
    goto rollback_render_node;
  }

  vaSetErrorCallback(encode_context->va_display, OnVaLogMessage, NULL);
#ifndef NDEBUG
  vaSetInfoCallback(encode_context->va_display, OnVaLogMessage, NULL);
#endif  // NDEBUG

  int major, minor;
  VAStatus status = vaInitialize(encode_context->va_display, &major, &minor);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to initialize va (%s)", VaErrorString(status));
    goto rollback_va_display;
  }

  LOG("Initialized VA %d.%d", major, minor);
  // TODO(mburakov): Check entry points?

  VAConfigAttrib attrib_list[] = {
      {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420},
  };
  status = vaCreateConfig(encode_context->va_display, VAProfileHEVCMain,
                          VAEntrypointEncSlice, attrib_list,
                          LENGTH(attrib_list), &encode_context->va_config_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create va config (%s)", VaErrorString(status));
    goto rollback_va_display;
  }

  if (!InitializeCodecCaps(encode_context)) {
    LOG("Failed to initialize codec caps");
    goto rollback_va_config_id;
  }

  // mburakov: Intel fails badly when min_cb_size value is not set to 16 and
  // log2_min_luma_coding_block_size_minus3 is not set to zero. Judging from
  // ffmpeg code, calculating one from another should work on other platforms,
  // but I hardcoded it instead since AMD is fine with alignment on 16 anyway.
  static const uint32_t min_cb_size = 16;
  uint32_t aligned_width =
      (encode_context->width + min_cb_size - 1) & ~(min_cb_size - 1);
  uint32_t aligned_height =
      (encode_context->height + min_cb_size - 1) & ~(min_cb_size - 1);

  status =
      vaCreateContext(encode_context->va_display, encode_context->va_config_id,
                      (int)aligned_width, (int)aligned_height, VA_PROGRESSIVE,
                      NULL, 0, &encode_context->va_context_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create va context (%s)", VaErrorString(status));
    goto rollback_va_config_id;
  }

  status =
      vaCreateSurfaces(encode_context->va_display, VA_RT_FORMAT_YUV420, width,
                       height, &encode_context->input_surface_id, 1, NULL, 0);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create va input surface (%s)", VaErrorString(status));
    goto rollback_va_context_id;
  }

  encode_context->gpu_frame = VaSurfaceToGpuFrame(
      encode_context->va_display, encode_context->input_surface_id,
      encode_context->gpu_context);
  if (!encode_context->gpu_frame) {
    LOG("Failed to convert va surface to gpu frame");
    goto rollback_input_surface_id;
  }

  status = vaCreateSurfaces(encode_context->va_display, VA_RT_FORMAT_YUV420,
                            aligned_width, aligned_height,
                            encode_context->recon_surface_ids,
                            LENGTH(encode_context->recon_surface_ids), NULL, 0);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create va recon surfaces (%s)", VaErrorString(status));
    goto rollback_gpu_frame;
  }

  unsigned int max_encoded_size =
      encode_context->width * encode_context->height * 3 / 2;
  status =
      vaCreateBuffer(encode_context->va_display, encode_context->va_context_id,
                     VAEncCodedBufferType, max_encoded_size, 1, NULL,
                     &encode_context->output_buffer_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create va output buffer (%s)", VaErrorString(status));
    goto rollback_recon_surface_ids;
  }

  InitializeSeqHeader(encode_context, (uint16_t)aligned_width,
                      (uint16_t)aligned_height);
  InitializePicHeader(encode_context);
  InitializeSliceHeader(encode_context);
  return encode_context;

rollback_recon_surface_ids:
  vaDestroySurfaces(encode_context->va_display,
                    encode_context->recon_surface_ids,
                    LENGTH(encode_context->recon_surface_ids));
rollback_gpu_frame:
  GpuContextDestroyFrame(encode_context->gpu_context,
                         encode_context->gpu_frame);
rollback_input_surface_id:
  vaDestroySurfaces(encode_context->va_display,
                    &encode_context->input_surface_id, 1);
rollback_va_context_id:
  vaDestroyContext(encode_context->va_display, encode_context->va_config_id);
rollback_va_config_id:
  vaDestroyConfig(encode_context->va_display, encode_context->va_config_id);
rollback_va_display:
  vaTerminate(encode_context->va_display);
rollback_render_node:
  close(encode_context->render_node);
rollback_encode_context:
  free(encode_context);
  return NULL;
}

const struct GpuFrame* EncodeContextGetFrame(
    struct EncodeContext* encode_context) {
  return encode_context->gpu_frame;
}

static bool UploadBuffer(const struct EncodeContext* encode_context,
                         VABufferType va_buffer_type, unsigned int size,
                         void* data, VABufferID** presult) {
  VAStatus status =
      vaCreateBuffer(encode_context->va_display, encode_context->va_context_id,
                     va_buffer_type, size, 1, data, *presult);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create buffer (%s)", VaErrorString(status));
    return false;
  }
  (*presult)++;
  return true;
}

static bool UploadPackedBuffer(const struct EncodeContext* encode_context,
                               VAEncPackedHeaderType packed_header_type,
                               unsigned int bit_length, void* data,
                               VABufferID** presult) {
  VAEncPackedHeaderParameterBuffer packed_header = {
      .type = packed_header_type,
      .bit_length = bit_length,
      .has_emulation_bytes = 1,
  };
  return UploadBuffer(encode_context, VAEncPackedHeaderParameterBufferType,
                      sizeof(packed_header), &packed_header, presult) &&
         UploadBuffer(encode_context, VAEncPackedHeaderDataBufferType,
                      (bit_length + 7) / 8, data, presult);
}

static void UpdatePicHeader(struct EncodeContext* encode_context, bool idr) {
  encode_context->pic.decoded_curr_pic = (VAPictureHEVC){
      .picture_id =
          encode_context
              ->recon_surface_ids[encode_context->frame_counter %
                                  LENGTH(encode_context->recon_surface_ids)],
      .pic_order_cnt = (int32_t)(encode_context->frame_counter %
                                 encode_context->seq.intra_idr_period),
  };

  if (idr) {
    encode_context->pic.reference_frames[0] = (VAPictureHEVC){
        .picture_id = VA_INVALID_ID,
        .flags = VA_PICTURE_HEVC_INVALID,
    };
    encode_context->pic.nal_unit_type = IDR_W_RADL;
    encode_context->pic.pic_fields.bits.idr_pic_flag = 1;
    encode_context->pic.pic_fields.bits.coding_type = 1;
  } else {
    encode_context->pic.reference_frames[0] = (VAPictureHEVC){
        .picture_id =
            encode_context
                ->recon_surface_ids[(encode_context->frame_counter - 1) %
                                    LENGTH(encode_context->recon_surface_ids)],
        .pic_order_cnt = (int32_t)((encode_context->frame_counter - 1) %
                                   encode_context->seq.intra_idr_period),
    };
    encode_context->pic.nal_unit_type = TRAIL_R;
    encode_context->pic.pic_fields.bits.idr_pic_flag = 0;
    encode_context->pic.pic_fields.bits.coding_type = 2;
  }
}

static bool DrainBuffers(int fd, struct iovec* iovec, int count) {
  for (;;) {
    ssize_t result = writev(fd, iovec, count);
    if (result < 0) {
      if (errno == EINTR) continue;
      LOG("Failed to write (%s)", strerror(errno));
      return false;
    }
    for (int i = 0; i < count; i++) {
      size_t delta = MIN((size_t)result, iovec[i].iov_len);
      iovec[i].iov_base = (uint8_t*)iovec[i].iov_base + delta;
      iovec[i].iov_len -= delta;
      result -= delta;
    }
    if (!result) return true;
  }
}

bool EncodeContextEncodeFrame(struct EncodeContext* encode_context, int fd) {
  bool result = false;
  VABufferID buffers[8];
  VABufferID* buffer_ptr = buffers;

  bool idr =
      !(encode_context->frame_counter % encode_context->seq.intra_idr_period);
  if (idr && !UploadBuffer(encode_context, VAEncSequenceParameterBufferType,
                           sizeof(encode_context->seq), &encode_context->seq,
                           &buffer_ptr)) {
    LOG("Failed to upload sequence parameter buffer");
    goto rollback_buffers;
  }

  if (idr &&
      (encode_context->va_packed_headers & VA_ENC_PACKED_HEADER_SEQUENCE)) {
    char buffer[256];
    struct Bitstream bitstream = {
        .data = buffer,
        .size = 0,
    };

    static const struct MoreVideoParameters mvp = {
        .vps_max_dec_pic_buffering_minus1 = 1,  // No B-frames
        .vps_max_num_reorder_pics = 0,          // No B-frames
    };
    uint32_t conf_win_right_offset_luma =
        encode_context->seq.pic_width_in_luma_samples - encode_context->width;
    uint32_t conf_win_bottom_offset_luma =
        encode_context->seq.pic_height_in_luma_samples - encode_context->height;
    const struct MoreSeqParameters msp = {
        .conf_win_left_offset = 0,
        .conf_win_right_offset = conf_win_right_offset_luma / 2,
        .conf_win_top_offset = 0,
        .conf_win_bottom_offset = conf_win_bottom_offset_luma / 2,
        .sps_max_dec_pic_buffering_minus1 = 1,  // No B-frames
        .sps_max_num_reorder_pics = 0,          // No B-frames
        .video_signal_type_present_flag = 1,
        .video_full_range_flag = encode_context->range == kFullRange,
        .colour_description_present_flag = 1,
        .colour_primaries = 2,          // Unsepcified
        .transfer_characteristics = 2,  // Unspecified
        .matrix_coeffs =
            encode_context->colorspace == kItuRec601 ? 6 : 1,  // Table E.5
    };

    PackVideoParameterSetNalUnit(&bitstream, &encode_context->seq, &mvp);
    PackSeqParameterSetNalUnit(&bitstream, &encode_context->seq, &msp);
    PackPicParameterSetNalUnit(&bitstream, &encode_context->pic);
    if (!UploadPackedBuffer(encode_context, VAEncPackedHeaderSequence,
                            (unsigned int)bitstream.size, bitstream.data,
                            &buffer_ptr)) {
      LOG("Failed to upload packed sequence header");
      goto rollback_buffers;
    }
  }

  UpdatePicHeader(encode_context, idr);
  if (!UploadBuffer(encode_context, VAEncPictureParameterBufferType,
                    sizeof(encode_context->pic), &encode_context->pic,
                    &buffer_ptr)) {
    LOG("Failed to upload picture parameter buffer");
    goto rollback_buffers;
  }

  encode_context->slice.slice_type = idr ? I : P;
  encode_context->slice.ref_pic_list0[0] =
      encode_context->pic.reference_frames[0];
  if (encode_context->va_packed_headers & VA_ENC_PACKED_HEADER_SLICE) {
    char buffer[256];
    struct Bitstream bitstream = {
        .data = buffer,
        .size = 0,
    };
    static const struct NegativePics negative_pics[] = {
        {
            .delta_poc_s0_minus1 = 0,
            .used_by_curr_pic_s0_flag = true,
        },
    };
    const struct MoreSliceParamerters msp = {
        .first_slice_segment_in_pic_flag = 1,
        .num_negative_pics = idr ? 0 : LENGTH(negative_pics),
        .negative_pics = idr ? NULL : negative_pics,
    };
    PackSliceSegmentHeaderNalUnit(&bitstream, &encode_context->seq,
                                  &encode_context->pic, &encode_context->slice,
                                  &msp);
    if (!UploadPackedBuffer(encode_context, VAEncPackedHeaderSlice,
                            (unsigned int)bitstream.size, bitstream.data,
                            &buffer_ptr)) {
      LOG("Failed to upload packed sequence header");
      goto rollback_buffers;
    }
  }

  if (!UploadBuffer(encode_context, VAEncSliceParameterBufferType,
                    sizeof(encode_context->slice), &encode_context->slice,
                    &buffer_ptr)) {
    LOG("Failed to upload slice parameter buffer");
    goto rollback_buffers;
  }

  VAStatus status =
      vaBeginPicture(encode_context->va_display, encode_context->va_context_id,
                     encode_context->input_surface_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to begin va picture (%s)", VaErrorString(status));
    goto rollback_buffers;
  }

  int num_buffers = (int)(buffer_ptr - buffers);
  status = vaRenderPicture(encode_context->va_display,
                           encode_context->va_context_id, buffers, num_buffers);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to render va picture (%s)", VaErrorString(status));
    goto rollback_buffers;
  }

  status =
      vaEndPicture(encode_context->va_display, encode_context->va_context_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to end va picture (%s)", VaErrorString(status));
    goto rollback_buffers;
  }

  status = vaSyncBuffer(encode_context->va_display,
                        encode_context->output_buffer_id, VA_TIMEOUT_INFINITE);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to sync va buffer (%s)", VaErrorString(status));
    goto rollback_buffers;
  }

  VACodedBufferSegment* segment;
  status = vaMapBuffer(encode_context->va_display,
                       encode_context->output_buffer_id, (void**)&segment);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to map va buffer (%s)", VaErrorString(status));
    goto rollback_buffers;
  }
  if (segment->next != NULL) {
    LOG("Next segment non-null!");
    abort();
  }

  struct iovec iovec[] = {
      {.iov_base = &segment->size, .iov_len = sizeof(segment->size)},
      {.iov_base = segment->buf, .iov_len = segment->size},
  };
  if (!DrainBuffers(fd, iovec, LENGTH(iovec))) {
    LOG("Failed to drain encoded frame");
    goto rollback_segment;
  }

  encode_context->frame_counter++;
  result = true;

rollback_segment:
  vaUnmapBuffer(encode_context->va_display, encode_context->output_buffer_id);
rollback_buffers:
  while (buffer_ptr-- > buffers)
    vaDestroyBuffer(encode_context->va_display, *buffer_ptr);
  return result;
}

void EncodeContextDestroy(struct EncodeContext* encode_context) {
  vaDestroyBuffer(encode_context->va_display, encode_context->output_buffer_id);
  GpuContextDestroyFrame(encode_context->gpu_context,
                         encode_context->gpu_frame);
  vaDestroySurfaces(encode_context->va_display,
                    &encode_context->input_surface_id, 1);
  vaDestroyContext(encode_context->va_display, encode_context->va_context_id);
  vaDestroyConfig(encode_context->va_display, encode_context->va_config_id);
  vaTerminate(encode_context->va_display);
  close(encode_context->render_node);
  free(encode_context);
}
