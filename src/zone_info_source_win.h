// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   https://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#ifndef CCTZ_ZONE_INFO_SOURCE_WIN_H_
#define CCTZ_ZONE_INFO_SOURCE_WIN_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "cctz/zone_info_source.h"
#include "time_zone_if.h"

namespace cctz {

// The size of a "TZI" registry value (REG_TZI_FORMAT): three little-endian
// LONGs (Bias, StandardBias, DaylightBias) followed by two SYSTEMTIMEs
// (StandardDate, DaylightDate) of eight little-endian WORDs each.
// https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/ns-timezoneapi-time_zone_information
const std::size_t kRegTziFormatSize = 44;

// A "TZI" registry value exactly as stored in the registry.
using WinRegTzi = std::array<unsigned char, kRegTziFormatSize>;

// The year range of SYSTEMTIME.
const year_t kMinSystemTimeYear = 1601;
const year_t kMaxSystemTimeYear = 30827;

// Decodes and validates "TZI" registry values, then converts them into a
// TZif-compatible ZoneInfoSource, suitable for use with the existing
// TimeZoneInfo parser. years[i] is the value for first_year + i, and the
// first/last values also apply to all earlier/later years, as in Windows'
// "Dynamic DST" keys (whose zone's base "TZI" value is not used). Before 1970,
// however, the zone stays in the first value's standard time, as the generated
// transitions begin in 1970.
//
// Returns nullptr if any value is invalid (e.g. an out-of-range transition
// date, or a UTC offset outside the range RFC 9636 allows, which is
// -25h < utoff < 26h, or -25h < utoff < 25h for the last value as it becomes
// the proleptic TZ string), if `years` is empty or extends beyond
// SYSTEMTIME's year range, or if the resulting zone needs more than
// TZ_MAX_TIMES transitions (as the transitions begin in 1970, this can happen
// when `first_year` is far in the future).
std::unique_ptr<ZoneInfoSource> CreateWinZoneInfoSource(
    year_t first_year, const std::vector<WinRegTzi>& years);

// A zone without a "Dynamic DST" key: `tzi` applies to all years.
std::unique_ptr<ZoneInfoSource> CreateWinZoneInfoSource(const WinRegTzi& tzi);

}  // namespace cctz

#endif  // CCTZ_ZONE_INFO_SOURCE_WIN_H_
