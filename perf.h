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

#ifndef STREAMER_PERF_H_
#define STREAMER_PERF_H_

struct TimingStats {
  unsigned long long min;
  unsigned long long max;
  unsigned long long sum;
};

unsigned long long MicrosNow(void);

void TimingStatsReset(struct TimingStats* timing_stats);
void TimingStatsRecord(struct TimingStats* timing_stats,
                       unsigned long long value);
void TimingStatsLog(const struct TimingStats* timing_stats, const char* name,
                    unsigned long long counter);

#endif  // STREAMER_PERF_H_
