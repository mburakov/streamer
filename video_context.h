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

#ifndef STREAMER_VIDEO_CONTEXT_H_
#define STREAMER_VIDEO_CONTEXT_H_

struct VideoContext;
struct IoContext;

struct VideoContext* VideoContextCreate(struct IoContext* io_context);
void VideoContextDestroy(struct VideoContext* video_context);

#endif  // STREAMER_VIDEO_CONTEXT_H_

