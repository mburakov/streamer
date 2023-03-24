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

#include "perf.h"

#include <limits.h>
#include <stdio.h>
#include <time.h>

#include "util.h"

unsigned long long MicrosNow(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000000ull +
         (unsigned long long)ts.tv_nsec / 1000ull;
}

void TimingStatsReset(struct TimingStats* timing_stats) {
  *timing_stats = (struct TimingStats){.min = ULLONG_MAX};
}

void TimingStatsRecord(struct TimingStats* timing_stats,
                       unsigned long long value) {
  timing_stats->min = MIN(timing_stats->min, value);
  timing_stats->max = MAX(timing_stats->max, value);
  timing_stats->sum += value;
}

void TimingStatsLog(const struct TimingStats* timing_stats, const char* name,
                    unsigned long long counter) {
  LOG("%s min/avg/max: %llu/%llu/%llu", name, timing_stats->min,
      timing_stats->sum / counter, timing_stats->max);
}
