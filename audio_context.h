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

#ifndef STREAMER_AUDIO_CONTEXT_H_
#define STREAMER_AUDIO_CONTEXT_H_

struct AudioContext;
struct IoContext;
struct Proto;

struct AudioContext* AudioContextCreate(struct IoContext* io_context,
                                        struct Proto* proto_hello);
void AudioContextDestroy(struct AudioContext* audio_context);

#endif  // STREAMER_AUDIO_CONTEXT_H_
