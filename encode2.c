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
#include "hevc.h"
#include "toolbox/utils.h"

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

  struct {
    bool packed_header_sequence;
    bool packed_header_slice;
  } codec_quirks;

  VAContextID va_context_id;
  VASurfaceID input_surface_id;
  struct GpuFrame* gpu_frame;

  VASurfaceID recon_surface_ids[2];
  VABufferID output_buffer_id;

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

static bool InitializeCodecQuirks(struct EncodeContext* encode_context) {
  bool result = false;
  VAProfile dummy_profile;
  VAEntrypoint dummy_entrypoint;
  int num_attribs = vaMaxNumConfigAttributes(encode_context->va_display);
  VAConfigAttrib* attrib_list =
      malloc((size_t)num_attribs * sizeof(VAConfigAttrib));
  VAStatus status = vaQueryConfigAttributes(
      encode_context->va_display, encode_context->va_config_id, &dummy_profile,
      &dummy_entrypoint, attrib_list, &num_attribs);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to query va config attributes (%s)", VaErrorString(status));
    goto rollback_attrib_list;
  }

  for (int i = 0; i < num_attribs; i++) {
    if (attrib_list[i].type == VAConfigAttribEncPackedHeaders) {
      encode_context->codec_quirks.packed_header_sequence =
          !!(attrib_list[i].value & VA_ENC_PACKED_HEADER_SEQUENCE);
      encode_context->codec_quirks.packed_header_slice =
          !!(attrib_list[i].value & VA_ENC_PACKED_HEADER_SLICE);
    }
  }
  result = true;

rollback_attrib_list:
  free(attrib_list);
  return result;
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
      .vui_parameters_present_flag = 1,
      .vui_fields.bits =
          {
              .aspect_ratio_info_present_flag = 0,           // defaulted
              .neutral_chroma_indication_flag = 0,           // defaulted
              .field_seq_flag = 0,                           // defaulted
              .vui_timing_info_present_flag = 1,             // hardcoded
              .bitstream_restriction_flag = 1,               // hardcoded
              .tiles_fixed_structure_flag = 0,               // defaulted
              .motion_vectors_over_pic_boundaries_flag = 1,  // hardcoded
              .restricted_ref_pic_lists_flag = 1,            // hardcoded
              .log2_max_mv_length_horizontal = 15,           // hardcoded
              .log2_max_mv_length_vertical = 15,             // hardcoded
          },

      .vui_num_units_in_tick = 1,         // TODO
      .vui_time_scale = 60,               // TODO
      .min_spatial_segmentation_idc = 0,  // defaulted
      .max_bytes_per_pic_denom = 0,       // hardcoded
      .max_bits_per_min_cu_denom = 0,     // hardcoded

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
  // TODO(mburakov): AMD only supports CQP and no packed headers.
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

  if (!InitializeCodecQuirks(encode_context)) {
    LOG("Failed to initialize codec quirks");
    goto rollback_va_config_id;
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

  bool idr =
      encode_context->frame_counter % encode_context->seq.intra_idr_period == 0;
  if (idr && !UploadBuffer(encode_context, VAEncSequenceParameterBufferType,
                           sizeof(encode_context->seq), &encode_context->seq,
                           &buffer_ptr)) {
    LOG("Failed to upload sequence parameter buffer");
    return false;
  }

  bool result = false;
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
      .picture_id =
          encode_context
              ->recon_surface_ids[encode_context->frame_counter %
                                  LENGTH(encode_context->recon_surface_ids)],
      .pic_order_cnt = (int32_t)(encode_context->frame_counter %
                                 encode_context->seq.intra_idr_period),
      .flags = 0,
  };
  for (size_t i = 0; i < LENGTH(encode_context->pic.reference_frames); i++) {
    encode_context->pic.reference_frames[i] = (VAPictureHEVC){
        .picture_id = VA_INVALID_ID,
        .flags = VA_PICTURE_HEVC_INVALID,
    };
  }
  if (!idr) {
    encode_context->pic.reference_frames[0] = (VAPictureHEVC){
        .picture_id =
            encode_context
                ->recon_surface_ids[(encode_context->frame_counter - 1) %
                                    LENGTH(encode_context->recon_surface_ids)],
        .pic_order_cnt = (int32_t)((encode_context->frame_counter - 1) %
                                   encode_context->seq.intra_idr_period),
    };
  }
  encode_context->pic.coded_buf = encode_context->output_buffer_id;
  encode_context->pic.nal_unit_type = idr ? IDR_W_RADL : TRAIL_R;
  encode_context->pic.pic_fields.bits.idr_pic_flag = idr;
  encode_context->pic.pic_fields.bits.coding_type = idr ? 1 : 2;
  encode_context->pic.pic_fields.bits.reference_pic_flag = 1;
  if (!UploadBuffer(encode_context, VAEncPictureParameterBufferType,
                    sizeof(encode_context->pic), &encode_context->pic,
                    &buffer_ptr)) {
    LOG("Failed to upload picture parameter buffer");
    goto rollback_buffers;
  }

  if (encode_context->codec_quirks.packed_header_sequence && idr) {
    char buffer[256];
    struct Bitstream bitstream = {
        .data = buffer,
        .size = 0,
    };
    static const struct MoreVideoParameters mvp = {
        .vps_max_dec_pic_buffering_minus1 = 1,  // No B-frames
        .vps_max_num_reorder_pics = 0,          // No B-frames
    };
    PackVideoParameterSetNalUnit(&bitstream, &encode_context->seq, &mvp);
    const struct MoreSeqParameters msp = {
        .conf_win_left_offset = 0,
        .conf_win_right_offset =
            (encode_context->seq.pic_width_in_luma_samples -
             encode_context->width) /
            2,
        .conf_win_top_offset = 0,
        .conf_win_bottom_offset =
            (encode_context->seq.pic_height_in_luma_samples -
             encode_context->height) /
            2,
        .sps_max_dec_pic_buffering_minus1 = 1,  // No B-frames
        .sps_max_num_reorder_pics = 0,          // No B-frames
        .video_signal_type_present_flag = 1,
        .video_full_range_flag = 0,  // TODO
        .colour_description_present_flag = 1,
        .colour_primaries = 2,          // Unsepcified
        .transfer_characteristics = 2,  // Unspecified
        .matrix_coeffs = 6,             // TODO
    };
    PackSeqParameterSetNalUnit(&bitstream, &encode_context->seq, &msp);
    PackPicParameterSetNalUnit(&bitstream, &encode_context->pic);
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

      .slice_type = idr ? I : P,  // calculated
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
  if (!idr) {
    slice.ref_pic_list0[0] = (VAPictureHEVC){
        .picture_id =
            encode_context
                ->recon_surface_ids[(encode_context->frame_counter - 1) %
                                    LENGTH(encode_context->recon_surface_ids)],
        .pic_order_cnt = (int32_t)((encode_context->frame_counter - 1) %
                                   encode_context->seq.intra_idr_period),
    };
  }

  // TODO(mburakov): ffmpeg assign reference frame for non-I-frames here.

  if (encode_context->codec_quirks.packed_header_slice) {
    char buffer[256];
    struct Bitstream bitstream = {
        .data = buffer,
        .size = 0,
    };
    const struct MoreSliceParamerters msp = {
        .first_slice_segment_in_pic_flag = 1,
        .num_negative_pics = 1,
        .negative_pics = &(struct NegativePics){0, 1},
    };
    PackSliceSegmentHeaderNalUnit(&bitstream, &encode_context->seq,
                                  &encode_context->pic, &slice, &msp);
    if (!UploadPackedBuffer(encode_context, VAEncPackedHeaderSlice,
                            (unsigned int)bitstream.size, bitstream.data,
                            &buffer_ptr)) {
      LOG("Failed to upload packed sequence header");
      goto rollback_buffers;
    }
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
  vaDestroyContext(encode_context->va_display, encode_context->va_config_id);
  vaDestroyConfig(encode_context->va_display, encode_context->va_config_id);
  vaTerminate(encode_context->va_display);
  close(encode_context->render_node);
  free(encode_context);
}
