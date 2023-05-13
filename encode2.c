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
#include "encode.h"
#include "gpu.h"
#include "toolbox/utils.h"

#define HEVC_SLICE_TYPE_B 0
#define HEVC_SLICE_TYPE_P 1
#define HEVC_SLICE_TYPE_I 2

#define HEVC_NUT_BLA_W_LP 16
#define HEVC_NUT_IDR_W_RADL 19
#define HEVC_NUT_IDR_N_LP 20
#define HEVC_NUT_RSV_IRAP_VCL23 23
#define HEVC_NUT_VPS 32
#define HEVC_NUT_SPS 33
#define HEVC_NUT_PPS 34

struct EncodeContext {
  struct GpuContext* gpu_context;
  uint32_t width;
  uint32_t height;

  VAEncSequenceParameterBufferHEVC seq;
  VAEncPictureParameterBufferHEVC pic;
  VAEncMiscParameterRateControl rc;
  VAEncMiscParameterFrameRate fr;

  int render_node;
  VADisplay va_display;
  VAConfigID va_config_id;
  VAContextID va_context_id;
  VASurfaceID input_surface_id;
  struct GpuFrame* gpu_frame;

  VASurfaceID recon_surface_ids[4];
  VABufferID output_buffer_id;
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

struct EncodeContext* EncodeContextCreate(struct GpuContext* gpu_context,
                                          uint32_t width, uint32_t height,
                                          enum YuvColorspace colorspace,
                                          enum YuvRange range) {
  struct EncodeContext* encode_context = malloc(sizeof(struct EncodeContext));
  if (!encode_context) {
    LOG("Faield to allocate encode context (%s)", strerror(errno));
    return NULL;
  }

  // TODO(mburakov): ffmpeg attempts to deduce this.
  static const uint32_t min_cb_size = 16;
  uint32_t width_in_cb = (width + min_cb_size - 1) / min_cb_size;
  uint32_t height_in_cb = (height + min_cb_size - 1) / min_cb_size;

  // TODO(mburakov): in the same deduction slice block size is set to 32.

  *encode_context = (struct EncodeContext){
      .gpu_context = gpu_context,
      .width = width,
      .height = height,
  };

  // TODO(mburakov): ffmpeg initializes SPS like this.
  encode_context->seq = (VAEncSequenceParameterBufferHEVC){
      .general_profile_idc = 1,  // Main profile
      .general_level_idc = 120,  // Level 4
      .general_tier_flag = 0,    // Main tier

      .intra_period = 120,      // Where this one comes from?
      .intra_idr_period = 120,  // Each I frame is an IDR frame
      .ip_period = 1,           // No B-frames
      .bits_per_second = 0,     // To be configured later?

      .pic_width_in_luma_samples = (uint16_t)(width_in_cb * min_cb_size),
      .pic_height_in_luma_samples = (uint16_t)(height_in_cb * min_cb_size),

      .seq_fields.bits =
          {
              .chroma_format_idc = 1,                    // 4:2:0
              .separate_colour_plane_flag = 0,           // Table 6-1
              .bit_depth_luma_minus8 = 0,                // 8 bpp luma
              .bit_depth_chroma_minus8 = 0,              // 8 bpp chroma
              .scaling_list_enabled_flag = 0,            // ???
              .strong_intra_smoothing_enabled_flag = 0,  // ???

              // mburakov: ffmpeg hardcodes these for i965 Skylake driver.
              .amp_enabled_flag = 1,
              .sample_adaptive_offset_enabled_flag = 0,
              .pcm_enabled_flag = 0,
              .pcm_loop_filter_disabled_flag = 0,  // ???
              .sps_temporal_mvp_enabled_flag = 0,

              // TODO(mburakov): ffmeg does not set below flags.
              // .low_delay_seq = 0,     // Probably should be 1
              // .hierachical_flag = 0,  // ???
          },

      // mburakov: ffmpeg hardcodes these for i965 Skylake driver.
      .log2_min_luma_coding_block_size_minus3 = 0,
      .log2_diff_max_min_luma_coding_block_size = 2,
      .log2_min_transform_block_size_minus2 = 0,
      .log2_diff_max_min_transform_block_size = 3,
      .max_transform_hierarchy_depth_inter = 3,
      .max_transform_hierarchy_depth_intra = 3,

      .pcm_sample_bit_depth_luma_minus1 = 0,            // ???
      .pcm_sample_bit_depth_chroma_minus1 = 0,          // ???
      .log2_min_pcm_luma_coding_block_size_minus3 = 0,  // ???
      .log2_max_pcm_luma_coding_block_size_minus3 = 0,  // ???

      // mburakov: ffmpeg hardcodes this to 0.
      .vui_parameters_present_flag = 0,

      // TODO(mburakov): ffmpeg leaves rest of the structure zero-initialized.
  };

  // TODO(mburakov): ffmpeg initializes PPS like this.
  encode_context->pic = (VAEncPictureParameterBufferHEVC){
      .decoded_curr_pic.picture_id = VA_INVALID_ID,
      .decoded_curr_pic.flags = VA_PICTURE_HEVC_INVALID,

      .coded_buf = VA_INVALID_ID,
      .collocated_ref_pic_index =
          encode_context->seq.seq_fields.bits.sps_temporal_mvp_enabled_flag
              ? 0
              : 0xff,

      .last_picture = 0,

      // mburakov: ffmpeg hardcodes initial value for non-CQP rate control.
      .pic_init_qp = 30,
      .diff_cu_qp_delta_depth = 0,
      .pps_cb_qp_offset = 0,
      .pps_cr_qp_offset = 0,

      .num_tile_columns_minus1 = 0,  // No tiles
      .num_tile_rows_minus1 = 0,     // No tiles

      .log2_parallel_merge_level_minus2 = 0,  // ???
      // mburakov: ffmpeg hardcodes this to 0.
      .ctu_max_bitsize_allowed = 0,

      // mburakov: ffmpeg hardcodes both to 0.
      .num_ref_idx_l0_default_active_minus1 = 0,
      .num_ref_idx_l1_default_active_minus1 = 0,

      // TODO(mburakov): Should this be incremented on IDR?
      .slice_pic_parameter_set_id = 0,

      // TODO(mburakov): ffmeg does not set below value.
      // .nal_unit_type = 0,

      .pic_fields.bits =
          {
              // mburakov: ffmpeg sets the flags below for each picture.
              // .idr_pic_flag = 0,
              // .coding_type = 0,
              // .reference_pic_flag = 0,

              // TODO(mburakov): ffmpeg does not set the flag below.
              // .dependent_slice_segments_enabled_flag = 0,

              .sign_data_hiding_enabled_flag = 0,  // ???
              .constrained_intra_pred_flag = 0,    // ???

              // TODO(mburakov): ffmpeg attempts to deduce the flag below.
              .transform_skip_enabled_flag = 0,

              // mburakov: ffmpeg enables thit for non-CQP rate control.
              .cu_qp_delta_enabled_flag = 1,

              .weighted_pred_flag = 0,                     // ???
              .weighted_bipred_flag = 0,                   // ???
              .transquant_bypass_enabled_flag = 0,         // ???
              .tiles_enabled_flag = 0,                     // No tiles
              .entropy_coding_sync_enabled_flag = 0,       // ???
              .loop_filter_across_tiles_enabled_flag = 0,  // No tiles

              // mburakov: ffmpeg hardcodes the flag below.
              .pps_loop_filter_across_slices_enabled_flag = 1,

              .scaling_list_data_present_flag = 0,  // ???

              // mburakov: ffmpeg hardcodes the flags below.
              .screen_content_flag = 0,
              .enable_gpu_weighted_prediction = 0,
              .no_output_of_prior_pics_flag = 0,
          },

      // TODO(mburakov): ffmpeg does not set values below.
      // .hierarchical_level_plus1 = 0,  // ???
      // .scc_fields.value = 0,          // ???
  };

  // TODO(mburakov): ffmpeg initializes RC like this:
  encode_context->rc = (VAEncMiscParameterRateControl){
      .bits_per_second = 0,      // Hardcoded for non-bitrate
      .target_percentage = 100,  // Hardcoded for non-bitrate
      .window_size = 1000,       // Hardcoded for non-AVBR
      .initial_qp = 0,           // Hardcoded
      .min_qp = 0,               // Comes from context
      .basic_unit_size = 0,      // Hardcoded
      .ICQ_quality_factor = 28,  // Comes from context - clipped [1, 51]
      .max_qp = 0,               // Comes from context
      .quality_factor = 28,      // Comes from context - non-clipped

      // TODO(mburakov): ffmpeg does not set below value.
      // .target_frame_size = 0,
  };

  // TODO(mburakov): ffmpeg initializes FR like this:
  encode_context->fr = (VAEncMiscParameterFrameRate){
      .framerate = (1 << 16) | 60,  // Comes from context

      // TODO(mburakov): ffmpeg does not set below value.
      // .framerate_flags.value = 0,
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

#if 1
  VAConfigAttrib config_attribs[] = {
      {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420},
      {.type = VAConfigAttribRateControl, .value = VA_RC_ICQ},
      {.type = VAConfigAttribEncPackedHeaders,
       .value = VA_ENC_PACKED_HEADER_MISC | VA_ENC_PACKED_HEADER_SLICE |
                VA_ENC_PACKED_HEADER_SEQUENCE},
  };
#else
  VAConfigAttrib config_attribs[] = {
      {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420},
      {.type = VAConfigAttribRateControl, .value = VA_RC_CQP},
  };
#endif
  status = vaCreateConfig(
      encode_context->va_display, VAProfileHEVCMain, VAEntrypointEncSlice,
      config_attribs, LENGTH(config_attribs), &encode_context->va_config_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create va config (%s)", VaErrorString(status));
    goto rollback_va_display;
  }

  status = vaCreateContext(
      encode_context->va_display, encode_context->va_config_id,
      (int)(width_in_cb * min_cb_size), (int)(height_in_cb * min_cb_size),
      VA_PROGRESSIVE, NULL, 0, &encode_context->va_context_id);
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

  status =
      vaCreateSurfaces(encode_context->va_display, VA_RT_FORMAT_YUV420,
                       width_in_cb * min_cb_size, height_in_cb * min_cb_size,
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

static bool UploadMiscBuffer(const struct EncodeContext* encode_context,
                             VAEncMiscParameterType misc_parameter_type,
                             unsigned int size, const void* data,
                             VABufferID** presult) {
  uint8_t stack_allocated_storage[sizeof(VAEncMiscParameterBuffer) + size];
  VAEncMiscParameterBuffer* buffer =
      (VAEncMiscParameterBuffer*)stack_allocated_storage;
  buffer->type = misc_parameter_type;
  memcpy(buffer->data, data, size);
  return UploadBuffer(encode_context, VAEncMiscParameterBufferType,
                      (unsigned int)sizeof(stack_allocated_storage),
                      stack_allocated_storage, presult);
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

static void PackVpsRbsp(struct Bitstream* bitstream,
                        const VAEncSequenceParameterBufferHEVC* seq) {
  char buffer[64];
  struct Bitstream vps_rbsp = {
      .data = buffer,
      .size = 0,
  };

  BitstreamAppend(bitstream, 32, 0x00000001);
  BitstreamAppend(bitstream, 1, 0);  // forbidden_zero_bit
  BitstreamAppend(bitstream, 6, HEVC_NUT_VPS);
  BitstreamAppend(bitstream, 6, 0);  // nuh_layer_id
  BitstreamAppend(bitstream, 3, 1);  // nuh_temporal_id_plus1

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&vps_rbsp, 4, 0);        // vps_video_parameter_set_id
  BitstreamAppend(&vps_rbsp, 1, 1);        // vps_base_layer_internal_flag
  BitstreamAppend(&vps_rbsp, 1, 1);        // vps_base_layer_available_flag
  BitstreamAppend(&vps_rbsp, 6, 0);        // vps_max_layers_minus1
  BitstreamAppend(&vps_rbsp, 3, 0);        // vps_max_sub_layers_minus1
  BitstreamAppend(&vps_rbsp, 1, 1);        // vps_temporal_id_nesting_flag
  BitstreamAppend(&vps_rbsp, 16, 0xffff);  // vps_reserved_0xffff_16bits

  // mburakov: Below is profile_tier_level structure.
  BitstreamAppend(&vps_rbsp, 2, 0);  // general_profile_space
  BitstreamAppend(&vps_rbsp, 1, seq->general_tier_flag);
  BitstreamAppend(&vps_rbsp, 5, seq->general_profile_idc);
  BitstreamAppend(&vps_rbsp, 32, 1 << (31 - seq->general_profile_idc));

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&vps_rbsp, 1, 1);   // general_progressive_source_flag
  BitstreamAppend(&vps_rbsp, 1, 0);   // general_interlaced_source_flag
  BitstreamAppend(&vps_rbsp, 1, 1);   // general_non_packed_constraint_flag
  BitstreamAppend(&vps_rbsp, 1, 1);   // general_frame_only_constraint_flag
  BitstreamAppend(&vps_rbsp, 24, 0);  // general_reserved_zero_43bits
  BitstreamAppend(&vps_rbsp, 19, 0);  // general_reserved_zero_43bits
  BitstreamAppend(&vps_rbsp, 1, 0);   // general_inbld_flag (TODO)

  BitstreamAppend(&vps_rbsp, 8, seq->general_level_idc);
  // mburakov: Above is profile_tier_level structure.

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&vps_rbsp, 1, 0);  // vps_sub_layer_ordering_info_present_flag

  // mburakov: No B-frames.
  BitstreamAppendUE(&vps_rbsp, 1);  // vps_max_dec_pic_buffering_minus1 (TODO)
  BitstreamAppendUE(&vps_rbsp, 0);  // vps_max_num_reorder_pics

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppendUE(&vps_rbsp, 0);   // vps_max_latency_increase_plus1
  BitstreamAppend(&vps_rbsp, 6, 0);  // vps_max_layer_id
  BitstreamAppendUE(&vps_rbsp, 0);   // vps_num_layer_sets_minus1
  BitstreamAppend(&vps_rbsp, 1, 1);  // vps_timing_info_present_flag (TODO)

  // mburakov: 60 frames per second.
  BitstreamAppend(&vps_rbsp, 32, 1);   // vps_num_units_in_tick
  BitstreamAppend(&vps_rbsp, 32, 60);  // vps_time_scale

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&vps_rbsp, 1, 0);  // vps_poc_proportional_to_timing_flag
  BitstreamAppendUE(&vps_rbsp, 0);   // vps_num_hrd_parameters
  BitstreamAppend(&vps_rbsp, 1, 0);  // vps_extension_flag

  // mburakov: Below is rbsp_trailing_bits structure.
  BitstreamAppend(&vps_rbsp, 1, 1);  // rbsp_stop_one_bit
  BitstreamByteAlign(&vps_rbsp);     // rbsp_alignment_zero_bit

  BitstreamInflate(bitstream, &vps_rbsp);
}

static void PackSpsRbsp(struct Bitstream* bitstream,
                        const VAEncSequenceParameterBufferHEVC* seq,
                        uint32_t crop_width, uint32_t crop_height) {
  char buffer[64];
  struct Bitstream sps_rbsp = {
      .data = buffer,
      .size = 0,
  };

  BitstreamAppend(bitstream, 32, 0x00000001);
  BitstreamAppend(bitstream, 1, 0);  // forbidden_zero_bit
  BitstreamAppend(bitstream, 6, HEVC_NUT_SPS);
  BitstreamAppend(bitstream, 6, 0);  // nuh_layer_id
  BitstreamAppend(bitstream, 3, 1);  // nuh_temporal_id_plus1

  BitstreamAppend(&sps_rbsp, 4, 0);  // sps_video_parameter_set_id
  BitstreamAppend(&sps_rbsp, 3, 0);  // sps_max_sub_layers_minus1
  BitstreamAppend(&sps_rbsp, 1, 1);  // sps_temporal_id_nesting_flag

  // mburakov: Below is profile_tier_level structure.
  BitstreamAppend(&sps_rbsp, 2, 0);  // general_profile_space
  BitstreamAppend(&sps_rbsp, 1, seq->general_tier_flag);
  BitstreamAppend(&sps_rbsp, 5, seq->general_profile_idc);
  BitstreamAppend(&sps_rbsp, 32, 1 << (31 - seq->general_profile_idc));

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&sps_rbsp, 1, 1);   // general_progressive_source_flag
  BitstreamAppend(&sps_rbsp, 1, 0);   // general_interlaced_source_flag
  BitstreamAppend(&sps_rbsp, 1, 1);   // general_non_packed_constraint_flag
  BitstreamAppend(&sps_rbsp, 1, 1);   // general_frame_only_constraint_flag
  BitstreamAppend(&sps_rbsp, 24, 0);  // general_reserved_zero_43bits
  BitstreamAppend(&sps_rbsp, 19, 0);  // general_reserved_zero_43bits
  BitstreamAppend(&sps_rbsp, 1, 0);   // general_inbld_flag (TODO)

  BitstreamAppend(&sps_rbsp, 8, seq->general_level_idc);
  // mburakov: Above is profile_tier_level structure.

  BitstreamAppendUE(&sps_rbsp, 0);  // sps_seq_parameter_set_id
  BitstreamAppendUE(&sps_rbsp, seq->seq_fields.bits.chroma_format_idc);
  BitstreamAppendUE(&sps_rbsp, seq->pic_width_in_luma_samples);
  BitstreamAppendUE(&sps_rbsp, seq->pic_height_in_luma_samples);
  if (crop_width != seq->pic_width_in_luma_samples ||
      crop_height != seq->pic_height_in_luma_samples) {
    uint32_t crop_win_right_offset_in_chroma_samples =
        (seq->pic_width_in_luma_samples - crop_width) / 2;
    uint32_t crop_win_bottom_offset_in_chroma_samples =
        (seq->pic_height_in_luma_samples - crop_height) / 2;
    BitstreamAppend(&sps_rbsp, 1, 1);  // conformance_window_flag
    BitstreamAppendUE(&sps_rbsp, 0);   // conf_win_left_offset
    BitstreamAppendUE(&sps_rbsp, crop_win_right_offset_in_chroma_samples);
    BitstreamAppendUE(&sps_rbsp, 0);  // conf_win_top_offset
    BitstreamAppendUE(&sps_rbsp, crop_win_bottom_offset_in_chroma_samples);
  } else {
    BitstreamAppend(&sps_rbsp, 1, 0);  // conformance_window_flag
  }

  BitstreamAppendUE(&sps_rbsp, seq->seq_fields.bits.bit_depth_luma_minus8);
  BitstreamAppendUE(&sps_rbsp, seq->seq_fields.bits.bit_depth_chroma_minus8);

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppendUE(&sps_rbsp, 8);   // log2_max_pic_order_cnt_lsb_minus4
  BitstreamAppend(&sps_rbsp, 1, 0);  // sps_sub_layer_ordering_info_present_flag

  // mburakov: No B-frames.
  BitstreamAppendUE(&sps_rbsp, 1);  // sps_max_dec_pic_buffering_minus1 (TODO)
  BitstreamAppendUE(&sps_rbsp, 0);  // sps_max_num_reorder_pics

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppendUE(&sps_rbsp, 0);  // sps_max_latency_increase_plus1

  BitstreamAppendUE(&sps_rbsp, seq->log2_min_luma_coding_block_size_minus3);
  BitstreamAppendUE(&sps_rbsp, seq->log2_diff_max_min_luma_coding_block_size);
  BitstreamAppendUE(&sps_rbsp, seq->log2_min_transform_block_size_minus2);
  BitstreamAppendUE(&sps_rbsp, seq->log2_diff_max_min_transform_block_size);
  BitstreamAppendUE(&sps_rbsp, seq->max_transform_hierarchy_depth_inter);
  BitstreamAppendUE(&sps_rbsp, seq->max_transform_hierarchy_depth_intra);
  BitstreamAppend(&sps_rbsp, 1, seq->seq_fields.bits.scaling_list_enabled_flag);
  // mburakov: scaling list details are absent because scaling_list_enabled_flag
  // is hardcoded to zero during sps initialization.
  BitstreamAppend(&sps_rbsp, 1, seq->seq_fields.bits.amp_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1,
                  seq->seq_fields.bits.sample_adaptive_offset_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1, seq->seq_fields.bits.pcm_enabled_flag);
  // mburakov: pcm sample details are missing because pcm_enabled_flag is
  // hardcoded to zero during sps initialization.

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppendUE(&sps_rbsp, 0);   // num_short_term_ref_pic_sets
  BitstreamAppend(&sps_rbsp, 1, 0);  // long_term_ref_pics_present_flag

  BitstreamAppend(&sps_rbsp, 1,
                  seq->seq_fields.bits.sps_temporal_mvp_enabled_flag);
  BitstreamAppend(&sps_rbsp, 1,
                  seq->seq_fields.bits.strong_intra_smoothing_enabled_flag);
  // TODO(mburakov): ffmpeg hardcodes vui_parameters_present_flag to zero for
  // unpacked sps, but to one for packed sps. Why???
  BitstreamAppend(&sps_rbsp, 1, 1);  // vui_parameters_present_flag

  // mburakov: Below is vui_parameters structure.
  BitstreamAppend(&sps_rbsp, 1, 0);  // aspect_ratio_info_present_flag
  BitstreamAppend(&sps_rbsp, 1, 0);  // overscan_info_present_flag
  BitstreamAppend(&sps_rbsp, 1, 1);  // video_signal_type_present_flag
  BitstreamAppend(&sps_rbsp, 3, 5);  // video_format
  BitstreamAppend(&sps_rbsp, 1, 0);  // video_full_range_flag (TODO)
  BitstreamAppend(&sps_rbsp, 1, 1);  // colour_description_present_flag
  BitstreamAppend(&sps_rbsp, 8, 2);  // colour_primaries (TODO)
  BitstreamAppend(&sps_rbsp, 8, 2);  // transfer_characteristics (TODO)
  BitstreamAppend(&sps_rbsp, 8, 6);  // matrix_coeffs (TODO)
  BitstreamAppend(&sps_rbsp, 1, 0);  // chroma_loc_info_present_flag

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&sps_rbsp, 1, 0);  // neutral_chroma_indication_flag
  BitstreamAppend(&sps_rbsp, 1, 0);  // field_seq_flag
  BitstreamAppend(&sps_rbsp, 1, 0);  // frame_field_info_present_flag
  BitstreamAppend(&sps_rbsp, 1, 0);  // default_display_window_flag

  BitstreamAppend(&sps_rbsp, 1, 1);  // vui_timing_info_present_flag (TODO)

  // mburakov: 60 frames per second.
  BitstreamAppend(&sps_rbsp, 32, 1);   // vui_num_units_in_tick
  BitstreamAppend(&sps_rbsp, 32, 60);  // vui_time_scale

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&sps_rbsp, 1, 0);  // vui_poc_proportional_to_timing_flag
  BitstreamAppend(&sps_rbsp, 1, 0);  // vui_hrd_parameters_present_flag
  BitstreamAppend(&sps_rbsp, 1, 1);  // bitstream_restriction_flag

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&sps_rbsp, 1, 0);  // tiles_fixed_structure_flag

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&sps_rbsp, 1, 1);  // motion_vectors_over_pic_boundaries_flag
  BitstreamAppend(&sps_rbsp, 1, 1);  // restricted_ref_pic_lists_flag

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppendUE(&sps_rbsp, 0);  // min_spatial_segmentation_idc

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppendUE(&sps_rbsp, 0);   // max_bytes_per_pic_denom
  BitstreamAppendUE(&sps_rbsp, 0);   // max_bits_per_min_cu_denom
  BitstreamAppendUE(&sps_rbsp, 15);  // log2_max_mv_length_horizontal
  BitstreamAppendUE(&sps_rbsp, 15);  // log2_max_mv_length_vertical
  // mburakov: Above is vui_parameters structure.

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppend(&sps_rbsp, 1, 0);  // sps_extension_present_flag (TODO)

  // mburakov: Below is rbsp_trailing_bits structure.
  BitstreamAppend(&sps_rbsp, 1, 1);  // rbsp_stop_one_bit
  BitstreamByteAlign(&sps_rbsp);     // rbsp_alignment_zero_bit

  BitstreamInflate(bitstream, &sps_rbsp);
}

static void PackPpsRbsp(struct Bitstream* bitstream,
                        const VAEncPictureParameterBufferHEVC* pic) {
  char buffer[64];
  struct Bitstream pps_rbsp = {
      .data = buffer,
      .size = 0,
  };

  BitstreamAppend(bitstream, 32, 0x00000001);
  BitstreamAppend(bitstream, 1, 0);  // forbidden_zero_bit
  BitstreamAppend(bitstream, 6, HEVC_NUT_PPS);
  BitstreamAppend(bitstream, 6, 0);  // nuh_layer_id
  BitstreamAppend(bitstream, 3, 1);  // nuh_temporal_id_plus1

  // mburakov: ffmpeg hardcodes the parameters below.
  BitstreamAppendUE(&pps_rbsp, 0);  // pps_pic_parameter_set_id
  BitstreamAppendUE(&pps_rbsp, 0);  // pps_seq_parameter_set_id (TODO)

  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.dependent_slice_segments_enabled_flag);

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&pps_rbsp, 1, 0);  // output_flag_present_flag
  BitstreamAppend(&pps_rbsp, 3, 0);  // num_extra_slice_header_bits

  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.sign_data_hiding_enabled_flag);

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&pps_rbsp, 1, 0);  // cabac_init_present_flag

  BitstreamAppendUE(&pps_rbsp, pic->num_ref_idx_l0_default_active_minus1);
  BitstreamAppendUE(&pps_rbsp, pic->num_ref_idx_l1_default_active_minus1);
  BitstreamAppendSE(&pps_rbsp, pic->pic_init_qp - 26);
  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.constrained_intra_pred_flag);
  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.transform_skip_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, pic->pic_fields.bits.cu_qp_delta_enabled_flag);
  if (pic->pic_fields.bits.cu_qp_delta_enabled_flag)
    BitstreamAppendUE(&pps_rbsp, pic->diff_cu_qp_delta_depth);
  BitstreamAppendSE(&pps_rbsp, pic->pps_cb_qp_offset);
  BitstreamAppendSE(&pps_rbsp, pic->pps_cr_qp_offset);

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&pps_rbsp, 1, 0);  // pps_slice_chroma_qp_offsets_present_flag

  BitstreamAppend(&pps_rbsp, 1, pic->pic_fields.bits.weighted_pred_flag);
  BitstreamAppend(&pps_rbsp, 1, pic->pic_fields.bits.weighted_bipred_flag);
  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.transquant_bypass_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1, pic->pic_fields.bits.tiles_enabled_flag);
  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.entropy_coding_sync_enabled_flag);
  if (pic->pic_fields.bits.tiles_enabled_flag) {
    BitstreamAppendUE(&pps_rbsp, pic->num_tile_columns_minus1);
    BitstreamAppendUE(&pps_rbsp, pic->num_tile_rows_minus1);
    // TODO(mburakov): Implement this!!!
    abort();
  }
  BitstreamAppend(
      &pps_rbsp, 1,
      pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&pps_rbsp, 1, 0);  // deblocking_filter_control_present_flag

  BitstreamAppend(&pps_rbsp, 1,
                  pic->pic_fields.bits.scaling_list_data_present_flag);
  if (pic->pic_fields.bits.scaling_list_data_present_flag) {
    // TODO(mburakov): Implement this!!!
    abort();
  }

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&pps_rbsp, 1, 0);  // lists_modification_present_flag

  BitstreamAppendUE(&pps_rbsp, pic->log2_parallel_merge_level_minus2);

  // mburakov: ffmpeg defaults the parameters below.
  BitstreamAppend(&pps_rbsp, 1,
                  0);  // slice_segment_header_extension_present_flag
  BitstreamAppend(&pps_rbsp, 1, 0);  // pps_extension_present_flag

  // mburakov: Below is rbsp_trailing_bits structure.
  BitstreamAppend(&pps_rbsp, 1, 1);  // rbsp_stop_one_bit
  BitstreamByteAlign(&pps_rbsp);     // rbsp_alignment_zero_bit

  BitstreamInflate(bitstream, &pps_rbsp);
}

static void PackSliceSegmentHeaderRbsp(
    struct Bitstream* bitstream, const VAEncSequenceParameterBufferHEVC* seq,
    const VAEncPictureParameterBufferHEVC* pic,
    const VAEncSliceParameterBufferHEVC* slice) {
  BitstreamAppend(bitstream, 32, 0x00000001);
  BitstreamAppend(bitstream, 1, 0);  // forbidden_zero_bit
  BitstreamAppend(bitstream, 6, pic->nal_unit_type);
  BitstreamAppend(bitstream, 6, 0);  // nuh_layer_id
  BitstreamAppend(bitstream, 3, 1);  // nuh_temporal_id_plus1

  // TODO(mburakov): I have no idea what I'm doing...
  static const uint32_t first_slice_segment_in_pic_flag = 1;

  // mburakov: ffmpeg defaults the parameteres below.
  static const uint32_t num_extra_slice_header_bits = 0;
  static const uint32_t output_flag_present_flag = 0;
  static const uint32_t lists_modification_present_flag = 0;
  static const uint32_t cabac_init_present_flag = 0;
  static const uint32_t motion_vector_resolution_control_idc = 0;
  static const uint32_t pps_slice_chroma_qp_offsets_present_flag = 0;
  static const uint32_t pps_slice_act_qp_offsets_present_flag = 0;
  static const uint32_t chroma_qp_offset_list_enabled_flag = 0;
  static const uint32_t deblocking_filter_override_enabled_flag = 0;
  static const uint32_t deblocking_filter_override_flag = 0;
  static const uint32_t num_entry_point_offsets = 0;
  static const uint32_t slice_segment_header_extension_present_flag = 0;

  // mburakov: ffmpeg hardcodes the parameters below.
  static const uint32_t log2_max_pic_order_cnt_lsb_minus4 = 8;
  static const uint32_t short_term_ref_pic_set_sps_flag = 0;
  static const uint32_t num_short_term_ref_pic_sets = 0;
  static const uint32_t long_term_ref_pics_present_flag = 0;

  BitstreamAppend(bitstream, 1, first_slice_segment_in_pic_flag);
  if (pic->nal_unit_type >= HEVC_NUT_BLA_W_LP &&
      pic->nal_unit_type <= HEVC_NUT_RSV_IRAP_VCL23) {
    BitstreamAppend(bitstream, 1,
                    pic->pic_fields.bits.no_output_of_prior_pics_flag);
  }
  BitstreamAppendUE(bitstream, slice->slice_pic_parameter_set_id);
  if (!first_slice_segment_in_pic_flag) {
    if (pic->pic_fields.bits.dependent_slice_segments_enabled_flag) {
      BitstreamAppend(bitstream, 1,
                      slice->slice_fields.bits.dependent_slice_segment_flag);
    }
    // TODO(mburakov): Implement this!!!
    abort();
  }

  if (!slice->slice_fields.bits.dependent_slice_segment_flag) {
    for (uint32_t i = 0; i < num_extra_slice_header_bits; i++) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    BitstreamAppendUE(bitstream, slice->slice_type);
    if (output_flag_present_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (seq->seq_fields.bits.separate_colour_plane_flag) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
    if (pic->nal_unit_type != HEVC_NUT_IDR_W_RADL &&
        pic->nal_unit_type != HEVC_NUT_IDR_N_LP) {
      uint32_t slice_pic_order_cnt_lsb =
          pic->decoded_curr_pic.pic_order_cnt &
          (1 << (log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1;
      BitstreamAppend(bitstream, log2_max_pic_order_cnt_lsb_minus4 + 4,
                      slice_pic_order_cnt_lsb);
      BitstreamAppend(bitstream, 1, short_term_ref_pic_set_sps_flag);
      if (!short_term_ref_pic_set_sps_flag) {
        // TODO(mburakov): Implement this!!!
        abort();
      } else if (num_short_term_ref_pic_sets > 0) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      if (long_term_ref_pics_present_flag) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      if (seq->seq_fields.bits.sps_temporal_mvp_enabled_flag) {
        BitstreamAppend(
            bitstream, 1,
            slice->slice_fields.bits.slice_temporal_mvp_enabled_flag);
      }
    }
    if (seq->seq_fields.bits.sample_adaptive_offset_enabled_flag) {
      BitstreamAppend(bitstream, 1,
                      slice->slice_fields.bits.slice_sao_luma_flag);
      uint32_t ChromaArrayType =
          !seq->seq_fields.bits.separate_colour_plane_flag
              ? seq->seq_fields.bits.chroma_format_idc
              : 0;
      if (ChromaArrayType != 0) {
        BitstreamAppend(bitstream, 1,
                        slice->slice_fields.bits.slice_sao_chroma_flag);
      }
    }
    if (slice->slice_type == HEVC_SLICE_TYPE_P ||
        slice->slice_type == HEVC_SLICE_TYPE_B) {
      BitstreamAppend(
          bitstream, 1,
          slice->slice_fields.bits.num_ref_idx_active_override_flag);
      if (slice->slice_fields.bits.num_ref_idx_active_override_flag) {
        BitstreamAppendUE(bitstream, slice->num_ref_idx_l0_active_minus1);
        if (slice->slice_type == HEVC_SLICE_TYPE_B)
          BitstreamAppendUE(bitstream, slice->num_ref_idx_l1_active_minus1);
      }
      if (lists_modification_present_flag /* && NumPicTotalCurr > 1*/) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      if (slice->slice_type == HEVC_SLICE_TYPE_B) {
        BitstreamAppend(bitstream, 1,
                        slice->slice_fields.bits.mvd_l1_zero_flag);
      }
      if (cabac_init_present_flag) {
        BitstreamAppend(bitstream, 1, slice->slice_fields.bits.cabac_init_flag);
      }
      if (slice->slice_fields.bits.slice_temporal_mvp_enabled_flag) {
        if (slice->slice_type == HEVC_SLICE_TYPE_B) {
          BitstreamAppend(bitstream, 1,
                          slice->slice_fields.bits.collocated_from_l0_flag);
        }
        if ((slice->slice_fields.bits.collocated_from_l0_flag &&
             slice->num_ref_idx_l0_active_minus1 > 0) ||
            (!slice->slice_fields.bits.collocated_from_l0_flag &&
             slice->num_ref_idx_l1_active_minus1 > 0)) {
          BitstreamAppendUE(bitstream, pic->collocated_ref_pic_index);
        }
      }
      if ((pic->pic_fields.bits.weighted_pred_flag &&
           slice->slice_type == HEVC_SLICE_TYPE_P) ||
          (pic->pic_fields.bits.weighted_bipred_flag &&
           slice->slice_type == HEVC_SLICE_TYPE_B)) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
      BitstreamAppendUE(bitstream, 5 - slice->max_num_merge_cand);
      if (motion_vector_resolution_control_idc == 2) {
        // TODO(mburakov): Implement this!!!
        abort();
      }
    }
    BitstreamAppendSE(bitstream, slice->slice_qp_delta);
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
    if (pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag &&
        (slice->slice_fields.bits.slice_sao_luma_flag ||
         slice->slice_fields.bits.slice_sao_chroma_flag ||
         !slice->slice_fields.bits.slice_deblocking_filter_disabled_flag)) {
      BitstreamAppend(bitstream, 1,
                      slice->slice_fields.bits
                          .slice_loop_filter_across_slices_enabled_flag);
    }
  }
  if (pic->pic_fields.bits.tiles_enabled_flag ||
      pic->pic_fields.bits.entropy_coding_sync_enabled_flag) {
    BitstreamAppendUE(bitstream, num_entry_point_offsets);
    if (num_entry_point_offsets > 0) {
      // TODO(mburakov): Implement this!!!
      abort();
    }
  }
  if (slice_segment_header_extension_present_flag) {
    // TODO(mburakov): Implement this!!!
    abort();
  }

  // mburakov: Below is byte_alignment structure.
  BitstreamAppend(bitstream, 1, 1);  // alignment_bit_equal_to_one
  BitstreamByteAlign(bitstream);     // alignment_bit_equal_to_zero
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
  VABufferID buffers[12];
  VABufferID* buffer_ptr = buffers;
  if (!UploadBuffer(encode_context, VAEncSequenceParameterBufferType,
                    sizeof(encode_context->seq), &encode_context->seq,
                    &buffer_ptr)) {
    LOG("Failed to upload sequence parameter buffer");
    return false;
  }

  bool result = false;
  bool idr = true;
  if (idr) {
    if (!UploadMiscBuffer(encode_context, VAEncMiscParameterTypeRateControl,
                          sizeof(encode_context->rc), &encode_context->rc,
                          &buffer_ptr)) {
      LOG("Failed to upload rate control buffer");
      goto rollback_buffers;
    }
    if (!UploadMiscBuffer(encode_context, VAEncMiscParameterTypeFrameRate,
                          sizeof(encode_context->fr), &encode_context->fr,
                          &buffer_ptr)) {
      LOG("Failed to upload frame rate buffer");
      goto rollback_buffers;
    }
  }

  // TODO(mburakov): Implement this!!!
  encode_context->pic.decoded_curr_pic = (VAPictureHEVC){
      .picture_id = encode_context->recon_surface_ids[0],  // recon
      .pic_order_cnt = 0,  // pic->display_order - hpic->last_idr_frame
      .flags = 0,
  };
  for (size_t i = 0; i < LENGTH(encode_context->pic.reference_frames); i++) {
    encode_context->pic.reference_frames[i] = (VAPictureHEVC){
        .picture_id = VA_INVALID_ID,
        .flags = VA_PICTURE_HEVC_INVALID,
    };
  }
  encode_context->pic.coded_buf = encode_context->output_buffer_id;
  encode_context->pic.nal_unit_type = HEVC_NUT_IDR_W_RADL;
  encode_context->pic.pic_fields.bits.idr_pic_flag = 1;
  encode_context->pic.pic_fields.bits.coding_type = 1;
  encode_context->pic.pic_fields.bits.reference_pic_flag = 1;
  if (!UploadBuffer(encode_context, VAEncPictureParameterBufferType,
                    sizeof(encode_context->pic), &encode_context->pic,
                    &buffer_ptr)) {
    LOG("Failed to upload picture parameter buffer");
    goto rollback_buffers;
  }

  if (idr) {
    char buffer[256];
    struct Bitstream bitstream = {
        .data = buffer,
        .size = 0,
    };
    PackVpsRbsp(&bitstream, &encode_context->seq);
    PackSpsRbsp(&bitstream, &encode_context->seq, encode_context->width,
                encode_context->height);
    PackPpsRbsp(&bitstream, &encode_context->pic);
    if (!UploadPackedBuffer(encode_context, VAEncPackedHeaderSequence,
                            (unsigned int)bitstream.size, bitstream.data,
                            &buffer_ptr)) {
      LOG("Failed to upload packed sequence header");
      goto rollback_buffers;
    }
  }

  // if (IDR) {
  //   VAEncSequenceParameterBufferType
  // }
  // if (IDR) {
  //   VAEncMiscParameterBufferType (VAEncMiscParameterTypeRateControl)
  //   VAEncMiscParameterBufferType (VAEncMiscParameterTypeFrameRate)
  // }
  // VAEncPictureParameterBufferType
  // if (IDR) {
  //   VAEncPackedHeaderParameterBufferType (VAEncPackedHeaderSequence)
  //   VAEncPackedHeaderDataBufferType (VPS+SPS+PPS)
  // }
  // VAEncPackedHeaderParameterBufferType (VAEncPackedHeaderSlice)
  // VAEncPackedHeaderDataBufferType (slice header)
  // VAEncSliceParameterBufferType

#if 0
if (pic->type == PICTURE_TYPE_IDR) {
    av_assert0(pic->display_order == pic->encode_order);

    hpic->last_idr_frame = pic->display_order;

    hpic->slice_nal_unit = HEVC_NAL_IDR_W_RADL;
    hpic->slice_type     = HEVC_SLICE_I;
    hpic->pic_type       = 0;
} else {
    av_assert0(prev);
    hpic->last_idr_frame = hprev->last_idr_frame;

    if (pic->type == PICTURE_TYPE_I) {
        hpic->slice_nal_unit = HEVC_NAL_CRA_NUT;
        hpic->slice_type     = HEVC_SLICE_I;
        hpic->pic_type       = 0;
    } else if (pic->type == PICTURE_TYPE_P) {
        av_assert0(pic->refs[0]);
        hpic->slice_nal_unit = HEVC_NAL_TRAIL_R;
        hpic->slice_type     = HEVC_SLICE_P;
        hpic->pic_type       = 1;
    } else {
#endif

  // hpic->slice_nal_unit = HEVC_NAL_IDR_W_RADL;
  // hpic->slice_type     = HEVC_SLICE_I;
  // hpic->pic_type       = 0;
  // hpic->pic_order_cnt = pic->display_order - hpic->last_idr_frame;
  // priv->sei_needed = 0;
  //
  // priv->sei == 56
  // vpic->decoded_curr_pic = (VAPictureHEVC) {
  //   .picture_id    = pic->recon_surface,
  //   .pic_order_cnt = hpic->pic_order_cnt,
  //   .flags         = 0,
  // };
  //

#if 0
for (i = 0; i < pic->nb_refs; i++) {
    VAAPIEncodePicture      *ref = pic->refs[i];
    VAAPIEncodeH265Picture *href;

    av_assert0(ref && ref->encode_order < pic->encode_order);
    href = ref->priv_data;

    vpic->reference_frames[i] = (VAPictureHEVC) {
        .picture_id    = ref->recon_surface,
        .pic_order_cnt = href->pic_order_cnt,
        .flags = (ref->display_order < pic->display_order ?
                  VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE : 0) |
                 (ref->display_order > pic->display_order ?
                  VA_PICTURE_HEVC_RPS_ST_CURR_AFTER  : 0),
    };
}
for (; i < FF_ARRAY_ELEMS(vpic->reference_frames); i++) {
    vpic->reference_frames[i] = (VAPictureHEVC) {
        .picture_id = VA_INVALID_ID,
        .flags      = VA_PICTURE_HEVC_INVALID,
    };
}
#endif

#if 0
vpic->coded_buf = pic->output_buffer;

vpic->nal_unit_type = hpic->slice_nal_unit;

switch (pic->type) {
case PICTURE_TYPE_IDR:
    vpic->pic_fields.bits.idr_pic_flag       = 1;
    vpic->pic_fields.bits.coding_type        = 1;
    vpic->pic_fields.bits.reference_pic_flag = 1;
    break;
case PICTURE_TYPE_I:
    vpic->pic_fields.bits.idr_pic_flag       = 0;
    vpic->pic_fields.bits.coding_type        = 1;
    vpic->pic_fields.bits.reference_pic_flag = 1;
    break;
case PICTURE_TYPE_P:
    vpic->pic_fields.bits.idr_pic_flag       = 0;
    vpic->pic_fields.bits.coding_type        = 2;
    vpic->pic_fields.bits.reference_pic_flag = 1;
    break;
#endif

  // slice_block_rows(34) = (height + slice_block_size - 1) / slice_block_size;
  // slice_block_cols(60) = (width + slice_block_size - 1) / slice_block_size;

  // TODO(mburakov): see comment in EncodeContextCreate.
  static const uint32_t slice_block_size = 32;
  uint32_t slice_block_rows =
      (encode_context->width + slice_block_size - 1) / slice_block_size;
  uint32_t slice_block_cols =
      (encode_context->height + slice_block_size - 1) / slice_block_size;
  uint32_t block_size = slice_block_rows * slice_block_cols;

  VAEncSliceParameterBufferHEVC slice = {
      .slice_segment_address = 0,  // calculated
      .num_ctu_in_slice = block_size,

      .slice_type = HEVC_SLICE_TYPE_I,  // calculated
      .slice_pic_parameter_set_id =
          encode_context->pic.slice_pic_parameter_set_id,

      .num_ref_idx_l0_active_minus1 =
          encode_context->pic.num_ref_idx_l0_default_active_minus1,
      .num_ref_idx_l1_active_minus1 =
          encode_context->pic.num_ref_idx_l1_default_active_minus1,

      .luma_log2_weight_denom = 0,          // ???
      .delta_chroma_log2_weight_denom = 0,  // ???

      // TODO(mburakov): ffmpeg does not initialize below entries.
      // .delta_luma_weight_l0[15],
      // .luma_offset_l0[15],
      // .delta_chroma_weight_l0[15][2],
      // .chroma_offset_l0[15][2],
      // .delta_luma_weight_l1[15],
      // .luma_offset_l1[15],
      // .delta_chroma_weight_l1[15][2],
      // .chroma_offset_l1[15][2],

      .max_num_merge_cand = 5 - 0,  // ???
      .slice_qp_delta = 0,          // evals to zero for CQP???
      .slice_cb_qp_offset = 0,      // ???
      .slice_cr_qp_offset = 0,      // ???

      .slice_beta_offset_div2 = 0,  // ???
      .slice_tc_offset_div2 = 0,    // ???

      .slice_fields.bits =
          {
              // TODO(mburakov): We only have a single slice?
              .last_slice_of_pic_flag = 1,
              .dependent_slice_segment_flag = 0,  // ???
              .colour_plane_id = 0,               // ???
              .slice_temporal_mvp_enabled_flag =
                  encode_context->seq.seq_fields.bits
                      .sps_temporal_mvp_enabled_flag,
              .slice_sao_luma_flag = encode_context->seq.seq_fields.bits
                                         .sample_adaptive_offset_enabled_flag,
              .slice_sao_chroma_flag = encode_context->seq.seq_fields.bits
                                           .sample_adaptive_offset_enabled_flag,
              .num_ref_idx_active_override_flag = 0,              // ???
              .mvd_l1_zero_flag = 0,                              // ???
              .cabac_init_flag = 0,                               // ???
              .slice_deblocking_filter_disabled_flag = 0,         // ???
              .slice_loop_filter_across_slices_enabled_flag = 0,  // ???
              .collocated_from_l0_flag = 0,                       // ???
          },

      // TODO(mburakov): ffmpeg does not initialize below entries.
      // .pred_weight_table_bit_offset = 0,
      // .pred_weight_table_bit_length = 0,
  };

  for (size_t i = 0; i < LENGTH(slice.ref_pic_list0); i++) {
    slice.ref_pic_list0[i].picture_id = VA_INVALID_ID;
    slice.ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
    slice.ref_pic_list1[i].picture_id = VA_INVALID_ID;
    slice.ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
  }

  // TODO(mburakov): ffmpeg assign reference frame for non-I-frames here.

  char buffer[256];
  struct Bitstream bitstream = {
      .data = buffer,
      .size = 0,
  };
  PackSliceSegmentHeaderRbsp(&bitstream, &encode_context->seq,
                             &encode_context->pic, &slice);
  if (!UploadPackedBuffer(encode_context, VAEncPackedHeaderSlice,
                          (unsigned int)bitstream.size, bitstream.data,
                          &buffer_ptr)) {
    LOG("Failed to upload packed sequence header");
    goto rollback_buffers;
  }

  if (!UploadBuffer(encode_context, VAEncSliceParameterBufferType,
                    sizeof(slice), &slice, &buffer_ptr)) {
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

  struct iovec iovec[] = {
      {.iov_base = &segment->size, .iov_len = sizeof(segment->size)},
      {.iov_base = segment->buf, .iov_len = segment->size},
  };
  if (!DrainBuffers(fd, iovec, LENGTH(iovec))) {
    LOG("Failed to drain encoded frame");
    goto rollback_segment;
  }

  LOG("GOT FRAME!");
  result = false;

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
  vaDestroyContext(encode_context->va_display, encode_context->va_config_id);
  vaDestroyConfig(encode_context->va_display, encode_context->va_config_id);
  vaTerminate(encode_context->va_display);
  close(encode_context->render_node);
  free(encode_context);
}
