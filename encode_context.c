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

#include "encode_context.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_wayland.h>

#include "io_context.h"
#include "util.h"

struct EncodeContext {
  struct IoContext* io_context;
  size_t width;
  size_t height;
  VADisplay display;
  VAConfigID config_id;

  uint32_t packed_headers;
  VAConfigAttribValEncHEVCFeatures hevc_features;
  VAConfigAttribValEncHEVCBlockSizes hevc_block_sizes;
};

static const char* VaErrorString(VAStatus error) {
  static const char* kVaErrorStrings[] = {
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
  return VA_STATUS_SUCCESS <= error &&
                 error < VA_STATUS_SUCCESS + (int)LENGTH(kVaErrorStrings)
             ? kVaErrorStrings[error - VA_STATUS_SUCCESS]
             : "???";
}

static bool InitializeCodecCaps(struct EncodeContext* encode_context) {
  VAConfigAttrib attrib_list[] = {
      {.type = VAConfigAttribEncPackedHeaders},
      {.type = VAConfigAttribEncHEVCFeatures},
      {.type = VAConfigAttribEncHEVCBlockSizes},
  };
  VAStatus status = vaGetConfigAttributes(
      encode_context->display, VAProfileHEVCMain, VAEntrypointEncSlice,
      attrib_list, LENGTH(attrib_list));
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to get va config attributes (%s)", VaErrorString(status));
    return false;
  }

  if (attrib_list[0].value == VA_ATTRIB_NOT_SUPPORTED) {
    LOG("VAConfigAttribEncPackedHeaders is not supported");
  } else {
    LOG("VAConfigAttribEncPackedHeaders is 0x%08x", attrib_list[0].value);
    encode_context->packed_headers = attrib_list[0].value;
  }

  if (attrib_list[1].value == VA_ATTRIB_NOT_SUPPORTED) {
    LOG("VAConfigAttribEncHEVCFeatures is not supported");
    encode_context->hevc_features = (VAConfigAttribValEncHEVCFeatures){
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
    encode_context->hevc_features.value = attrib_list[1].value;
  }

  if (attrib_list[2].value == VA_ATTRIB_NOT_SUPPORTED) {
    LOG("VAConfigAttribEncHEVCBlockSizes is not supported");
    encode_context->hevc_block_sizes = (VAConfigAttribValEncHEVCBlockSizes){
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
    encode_context->hevc_block_sizes.value = attrib_list[2].value;
  }

  return true;
}

struct EncodeContext* EncodeContextCreate(struct IoContext* io_context,
                                          uint32_t width, uint32_t height,
                                          struct wl_display* display) {
  LOG("Initializing encoder context for %ux%u resolution", width, height);
  struct EncodeContext* encode_context = malloc(sizeof(struct EncodeContext));
  if (!encode_context) {
    LOG("Failed to allocate encode context (%s)", strerror(errno));
    return NULL;
  }

  *encode_context = (struct EncodeContext){
      .io_context = io_context,
      .width = width,
      .height = height,
      .display = vaGetDisplayWl(display),
  };
  if (!encode_context->display) {
    LOG("Failed to get VA display");
    goto rollback_encode_context;
  }

  int major, minor;
  VAStatus status = vaInitialize(encode_context->display, &major, &minor);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to initialize VA (%s)", VaErrorString(status));
    goto rollback_display;
  }

  LOG("Initialized VA %d.%d", major, minor);
  VAConfigAttrib attrib_list[] = {
      {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420},
  };
  status = vaCreateConfig(encode_context->display, VAProfileHEVCMain,
                          VAEntrypointEncSlice, attrib_list,
                          LENGTH(attrib_list), &encode_context->config_id);
  if (status != VA_STATUS_SUCCESS) {
    LOG("Failed to create VA config (%s)", VaErrorString(status));
    goto rollback_display;
  }

  if (!InitializeCodecCaps(encode_context)) {
    LOG("Failed to initialize codec caps");
    goto rollback_config_id;
  }

  return encode_context;

rollback_config_id:
  assert(vaDestroyConfig(encode_context->display, encode_context->config_id) ==
         VA_STATUS_SUCCESS);
rollback_display:
  assert(vaTerminate(encode_context->display) == VA_STATUS_SUCCESS);
rollback_encode_context:
  free(encode_context);
  return NULL;
}

struct EncodeContextFrame* EncodeContextDequeue(
    struct EncodeContext* encode_context) {
  (void)encode_context;
  // TODO(mburakov): Implement this!
  return NULL;
}

bool EncodeContextQueue(struct EncodeContext* encode_context,
                        struct EncodeContextFrame* encode_context_frame,
                        bool encode) {
  (void)encode_context;
  (void)encode_context_frame;
  (void)encode;
  // TODO(mburakov): Implement this!
  return true;
}

void EncodeContextDestroy(struct EncodeContext* encode_context) {
  assert(vaDestroyConfig(encode_context->display, encode_context->config_id) ==
         VA_STATUS_SUCCESS);
  assert(vaTerminate(encode_context->display) == VA_STATUS_SUCCESS);
  free(encode_context);
}
