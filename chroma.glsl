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
uniform mediump vec2 chroma_offsets;

varying mediump vec2 texcoord;

mediump vec2 rgb2chroma(in mediump vec4 rgb) {
  // mburakov: This hardcodes BT.709 full-range.
  mediump float y = rgb.r * 0.2126 + rgb.g * 0.7152 + rgb.b * 0.0722;
  mediump float u = (rgb.b - y) / (2.0 * (1.0 - 0.0722));
  mediump float v = (rgb.r - y) / (2.0 * (1.0 - 0.2126));
  return vec2(u + 0.5, v + 0.5);
}

void main() {
  mediump vec2 sample_points[4];
  sample_points[0] = texcoord;
  sample_points[1] = texcoord + vec2(chroma_offsets.x, 0.0);
  sample_points[2] = texcoord + vec2(0.0, chroma_offsets.y);
  sample_points[3] = texcoord + chroma_offsets;
  mediump vec4 rgb = texture2D(img_input, sample_points[0]) +
                     texture2D(img_input, sample_points[1]) +
                     texture2D(img_input, sample_points[2]) +
                     texture2D(img_input, sample_points[3]);
  gl_FragColor = vec4(rgb2chroma(rgb / 4.0), 0.0, 1.0);
}
