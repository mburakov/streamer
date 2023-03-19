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
uniform mediump mat3 colorspace;
uniform mediump vec3 ranges[2];

varying mediump vec2 texcoord;

mediump vec3 rgb2yuv(in mediump vec3 rgb) {
  mediump vec3 yuv = colorspace * rgb.rgb + vec3(0.0, 0.5, 0.5);
  return ranges[0] + yuv * ranges[1];
}

void main() {
  mediump vec4 rgb = texture2D(img_input, texcoord);
  mediump vec3 yuv = rgb2yuv(rgb.rgb);
  gl_FragColor = vec4(yuv.x, 0.0, 0.0, 1.0);
}
