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

uniform sampler2D img_input;
uniform mediump vec2 sample_offsets[4];
varying mediump vec2 texcoord;

const mediump mat3 kColorSpace = mat3(
   0.299,     0.587,     0.114,
  -0.168736, -0.331264,  0.5,
   0.5,      -0.418688, -0.081312
);

const mediump vec3 kColorRangeBase = vec3(
  16.0 / 255.0,
  16.0 / 255.0,
  16.0 / 255.0
);

const mediump vec3 kColorRangeScale = vec3(
  (235.0 - 16.0) / 255.0,
  (240.0 - 16.0) / 255.0,
  (240.0 - 16.0) / 255.0
);

mediump vec4 supersample() {
  return texture2D(img_input, texcoord + sample_offsets[0]) +
         texture2D(img_input, texcoord + sample_offsets[1]) +
         texture2D(img_input, texcoord + sample_offsets[2]) +
         texture2D(img_input, texcoord + sample_offsets[3]);
}

mediump vec3 rgb2yuv(in mediump vec3 rgb) {
  mediump vec3 yuv = kColorSpace * rgb.rgb + vec3(0.0, 0.5, 0.5);
  return kColorRangeBase + yuv * kColorRangeScale;
}

void main() {
  mediump vec4 rgb = supersample() / 4.0;
  mediump vec3 yuv = rgb2yuv(rgb.rgb);
  gl_FragColor = vec4(yuv.yz, 0.0, 1.0);
}
