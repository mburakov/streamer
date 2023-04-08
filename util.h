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

#ifndef STREAMER_UTIL_H_
#define STREAMER_UTIL_H_

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)
#define LOG(fmt, ...) \
  fprintf(stderr, __FILE__ ":" STR(__LINE__) " " fmt "\n", ##__VA_ARGS__)
#define LENGTH(x) (sizeof(x) / sizeof *(x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define _(...) __VA_ARGS__

#endif  // STREAMER_UTIL_H_
