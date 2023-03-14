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

varying mediump vec2 texcoord;

mediump float rgb2luma(in mediump vec4 rgb) {
  // mburakov: This hardcodes BT.709 full-range.
  return rgb.r * 0.2126 + rgb.g * 0.7152 + rgb.b * 0.0722;
}

void main() {
  mediump vec4 rgb = texture2D(img_input, texcoord);
  gl_FragColor = vec4(rgb2luma(rgb), 0.0, 0.0, 1.0);
}
