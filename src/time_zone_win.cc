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

#include "time_zone_win.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cctz/zone_info_source.h"
#include "time_zone_fixed.h"
#include "time_zone_if.h"

namespace cctz {
namespace {

const year_t kTransitionStartYear = 1970;

const cctz::weekday kWeekdays[] = {
    cctz::weekday::sunday,    cctz::weekday::monday,   cctz::weekday::tuesday,
    cctz::weekday::wednesday, cctz::weekday::thursday, cctz::weekday::friday,
    cctz::weekday::saturday};

struct TransitionInfo {
  TransitionInfo() : time(0), offset(0), is_dst(false) {}
  std::int_fast64_t time;    // Unix timestamp
  std::int_fast32_t offset;  // Seconds east of UTC
  bool is_dst;               // Is daylight saving time
};

struct Transition {
  Transition() : time(0), type_idx(0) {}
  std::int_fast64_t time;      // Unix timestamp
  std::uint_fast8_t type_idx;  // Index into transition types
};

struct TransitionType {
  TransitionType() : abbr_idx(0), offset(0), is_dst(false) {}
  std::uint_fast8_t abbr_idx;  // Index into abbreviation string
  std::int_fast32_t offset;    // Seconds east of UTC
  bool is_dst;                 // Is daylight saving time
};

struct TransitionTable {
  std::vector<Transition> transitions;
  std::vector<TransitionType> transition_types;
  std::string abbreviation_table;
  std::string proleptic_tz_string;
};

class TransitionTableBuilder {
 public:
  static TransitionTable Build(const WinTimeZoneRegistryInfo& info) {
    TransitionTableBuilder builder;
    builder.Initialize(info);
    if (builder.overflowed_) {
      return TransitionTable{};
    }
    return TransitionTable{std::move(builder.transitions_),
                           std::move(builder.transition_types_),
                           std::move(builder.abbreviation_table_),
                           std::move(builder.proleptic_tz_string_)};
  }

 private:
  TransitionTableBuilder() : overflowed_(false) {}

  struct OffsetDstPair {
    OffsetDstPair() : offset_seconds(0), dst(false) {}
    OffsetDstPair(std::int_fast32_t offset_seconds_, bool dst_)
        : offset_seconds(offset_seconds_), dst(dst_) {}
    bool operator==(const OffsetDstPair& o) const {
      return offset_seconds == o.offset_seconds && dst == o.dst;
    }
    std::int_fast32_t offset_seconds;
    bool dst;
  };

  void Initialize(const WinTimeZoneRegistryInfo& info) {
    if (info.entries.empty()) {
      return;
    }

    const year_t first_year =
        info.entries.size() == 1
            ? kTransitionStartYear
            : std::min<cctz::year_t>(info.first_year, kTransitionStartYear);

    const auto& first_entry = info.entries[0];
    const bool first_entry_supports_dst =
        first_entry.standard_date.month != 0 &&
        first_entry.daylight_date.month != 0;
    if (!first_entry_supports_dst) {
      // The first entry never transitions (ProcessEntry ignores lone transition
      // dates), so add an initial transition here and seed last_offset_. For
      // entries that never observe DST, the Win32 API ignores StandardBias and
      // uses only Bias.
      civil_second first_civil_second(first_year, 1, 1, 0, 0, 0);
      TransitionInfo initial_info;
      const std::int_fast32_t offset_seconds = -60 * first_entry.bias;
      initial_info.time = seconds(first_civil_second - offset_seconds -
                                  civil_second(1970, 1, 1, 0, 0, 0))
                              .count();
      initial_info.offset = offset_seconds;
      initial_info.is_dst = false;
      AddTransition(initial_info);
      last_offset_ = OffsetDstPair{offset_seconds, false};
    } else {
      // Direct initialization: no warmup call needed.
      // The pre-initial state is always the standard (non-DST) offset.
      last_offset_ = OffsetDstPair{
          -60 * (first_entry.bias + first_entry.standard_bias), false};
    }

    if (info.entries.size() == 1) {
      ProcessEntry(first_entry, first_year);
    } else {
      const year_t last_year =
          info.first_year + static_cast<year_t>(info.entries.size());
      // At most three transitions per year (year begin plus the two rule
      // dates), plus the initial transition.
      transitions_.reserve(
          static_cast<std::size_t>(last_year - first_year) * 3 + 1);
      for (cctz::year_t year = first_year; year < last_year; ++year) {
        ProcessEntry(
            info.entries[year < info.first_year
                             ? 0
                             : static_cast<size_t>(year - info.first_year)],
            year);
        if (overflowed_) {
          return;
        }
      }
    }
    proleptic_tz_string_ = ToTzString(info.entries.back());
  }

  static bool ResolveSystemTime(const WinSystemTime& system_time, year_t year,
                                civil_second* result) {
    const year_t system_time_year = static_cast<year_t>(system_time.year);
    civil_second cs;
    if (system_time_year == year) {
      cs = civil_second(system_time_year, system_time.month, system_time.day,
                        system_time.hour, system_time.minute,
                        system_time.second);
    } else if (system_time_year != 0) {
      // Absolute-dated rules fire only in their own year.
      return false;
    } else {
      if (system_time.day_of_week > 6) {
        return false;
      }
      const cctz::weekday target_weekday = kWeekdays[system_time.day_of_week];
      cctz::civil_day target_day;
      if (system_time.day == 5) {
        year_t tmp_year = year;
        std::int_fast32_t tmp_month = system_time.month + 1;
        if (tmp_month > 12) {
          tmp_month = 1;
          tmp_year += 1;
        }
        target_day = prev_weekday(cctz::civil_day(tmp_year, tmp_month, 1),
                                  target_weekday);
      } else {
        target_day = next_weekday(
            cctz::civil_day(year, system_time.month, 1) - 1, target_weekday);
        target_day += (system_time.day - 1) * 7;
      }

      cs = civil_second(target_day.year(), target_day.month(), target_day.day(),
                        system_time.hour, system_time.minute,
                        system_time.second);
    }
    // Windows uses "23:59:59.999" to mean midnight at the end of the day.
    if (cs.hour() == 23 && cs.minute() == 59 && cs.second() == 59 &&
        system_time.milliseconds == 999) {
      cs += 1;
    }
    *result = cs;
    return true;
  }

  void ProcessEntry(const WinTimeZoneRegistryEntry& format, year_t year) {
    const civil_second year_begin(year, 1, 1, 0, 0, 0);

    // Intentionally omit entries with only one transition date to be consistent
    // with how Windows APIs handle such entries.
    const bool supports_dst =
        format.standard_date.month != 0 && format.daylight_date.month != 0;

    bool has_std_begin = false;
    civil_second std_begin;
    bool has_dst_begin = false;
    civil_second dst_begin;
    if (supports_dst) {
      has_std_begin = ResolveSystemTime(format.standard_date, year, &std_begin);
      has_dst_begin = ResolveSystemTime(format.daylight_date, year, &dst_begin);
    }

    if ((!has_std_begin || std_begin != year_begin) &&
        (!has_dst_begin || dst_begin != year_begin)) {
      // Windows treats a local time as DST when
      //   DaylightDate <= t < StandardDate   (if DaylightDate < StandardDate)
      //   t >= DaylightDate || t < StandardDate  (otherwise, i.e. when DST
      //                                           wraps the year end as in the
      //                                           southern hemisphere)
      // so Jan 1 falls in DST iff the standard transition both exists and
      // precedes the daylight one within the year.
      const bool year_begins_in_dst =
          has_std_begin && has_dst_begin && std_begin < dst_begin;
      if (year_begins_in_dst) {
        TryAddOffset(
            year_begin,
            OffsetDstPair{-60 * (format.bias + format.daylight_bias), true});
      } else {
        // For entries that never observe DST (no transition dates, or only one
        // of the two), the Win32 API ignores StandardBias and uses only Bias.
        // For DST zones, the standard-time offset is -(Bias + StandardBias).
        const std::int_fast32_t year_begin_bias =
            supports_dst ? (format.bias + format.standard_bias) : format.bias;
        TryAddOffset(year_begin, OffsetDstPair{-60 * year_begin_bias, false});
      }
    }

    if (has_dst_begin) {
      if (has_std_begin) {
        if (std_begin < dst_begin) {
          TryAddOffset(
              std_begin,
              OffsetDstPair{-60 * (format.bias + format.standard_bias), false});
          TryAddOffset(
              dst_begin,
              OffsetDstPair{-60 * (format.bias + format.daylight_bias), true});
        } else {
          TryAddOffset(
              dst_begin,
              OffsetDstPair{-60 * (format.bias + format.daylight_bias), true});
          TryAddOffset(
              std_begin,
              OffsetDstPair{-60 * (format.bias + format.standard_bias), false});
        }
      } else {
        TryAddOffset(
            dst_begin,
            OffsetDstPair{-60 * (format.bias + format.daylight_bias), true});
      }
    } else {
      if (has_std_begin) {
        TryAddOffset(
            std_begin,
            OffsetDstPair{-60 * (format.bias + format.standard_bias), false});
      }
    }
  }

  void TryAddOffset(civil_second from_civil_time, OffsetDstPair to_offset) {
    if (last_offset_ == to_offset) {
      return;
    }
    TransitionInfo new_info;
    new_info.time = seconds(from_civil_time - last_offset_.offset_seconds -
                            civil_second(1970, 1, 1, 0, 0, 0))
                        .count();
    new_info.offset = to_offset.offset_seconds;
    new_info.is_dst = to_offset.dst;
    AddTransition(new_info);
    last_offset_ = to_offset;
  }

  void AddTransition(const TransitionInfo& transition_info) {
    if (overflowed_) {
      return;
    }

    const auto offset_sec = cctz::seconds(transition_info.offset);
    const std::string abbr = (offset_sec.count() == 0)
                                 ? "UTC"
                                 : "UTC" + cctz::FixedOffsetToAbbr(offset_sec);

    // Table entries are stored NUL-terminated, so compare (and append) the
    // terminated form.
    const std::string abbr_z = abbr + '\0';
    bool abbr_found = false;
    size_t abbr_index = 0;
    for (size_t index : abbreviation_indices_) {
      if (abbreviation_table_.compare(index, abbr_z.size(), abbr_z) == 0) {
        abbr_found = true;
        abbr_index = index;
        break;
      }
    }
    if (!abbr_found) {
      abbr_index = abbreviation_table_.size();
      if (abbr_index > std::numeric_limits<uint8_t>::max()) {
        overflowed_ = true;
        return;
      }
      abbreviation_table_ += abbr_z;
      abbreviation_indices_.push_back(abbr_index);
    }

    bool type_found = false;
    size_t type_index = 0;
    for (size_t i = 0; i < transition_types_.size(); ++i) {
      const TransitionType& tt = transition_types_[i];
      if (tt.offset == transition_info.offset &&
          tt.is_dst == transition_info.is_dst && tt.abbr_idx == abbr_index) {
        type_found = true;
        type_index = i;
        break;
      }
    }
    if (!type_found) {
      type_index = transition_types_.size();
      if (type_index > std::numeric_limits<uint8_t>::max()) {
        overflowed_ = true;
        return;
      }
      TransitionType new_type;
      new_type.offset = transition_info.offset;
      new_type.is_dst = transition_info.is_dst;
      new_type.abbr_idx = static_cast<uint8_t>(abbr_index);
      transition_types_.push_back(new_type);
    }

    Transition transition;
    transition.time = transition_info.time;
    transition.type_idx = static_cast<uint8_t>(type_index);
    transitions_.push_back(transition);
  }

  std::vector<Transition> transitions_;
  std::vector<TransitionType> transition_types_;
  std::string abbreviation_table_;
  std::vector<std::size_t> abbreviation_indices_;
  std::string proleptic_tz_string_;
  OffsetDstPair last_offset_;
  bool overflowed_;
};

class WinZoneInfoSource : public ZoneInfoSource {
 public:
  WinZoneInfoSource() = delete;
  WinZoneInfoSource(const WinZoneInfoSource&) = delete;
  WinZoneInfoSource& operator=(const WinZoneInfoSource&) = delete;

  explicit WinZoneInfoSource(std::vector<char>&& data)
      : data_(std::move(data)) {}

  std::size_t Read(void* ptr, std::size_t size) override {
    // pos_ never exceeds data_.size() (see Skip), so no underflow here.
    const std::size_t to_read = std::min(size, data_.size() - pos_);
    std::memcpy(ptr, data_.data() + pos_, to_read);
    pos_ += to_read;
    return to_read;
  }

  int Skip(std::size_t offset) override {
    if (offset > data_.size() - pos_) {
      return -1;  // Would go past EOF (also guards against overflow).
    }
    pos_ += offset;
    return 0;
  }

  std::string Version() const override { return ""; }

 private:
  // The generated TZDATA binary data
  const std::vector<char> data_;

  // Current read position
  std::size_t pos_ = 0;
};

// Helpers to encode big-endian integers. The signed variants delegate to the
// unsigned one through a signed-to-unsigned cast, which is defined to yield the
// two's-complement bit pattern; right-shifting the negative value
// directly would be implementation-defined before C++20.
void PushBackUInt32(std::vector<char>* dst, std::uint_fast32_t value) {
  dst->push_back(static_cast<char>((value >> 24) & 0xff));
  dst->push_back(static_cast<char>((value >> 16) & 0xff));
  dst->push_back(static_cast<char>((value >> 8) & 0xff));
  dst->push_back(static_cast<char>(value & 0xff));
}

void PushBackInt32(std::vector<char>* dst, std::int_fast32_t value) {
  PushBackUInt32(dst, static_cast<std::uint32_t>(value));
}

void PushBackInt64(std::vector<char>* dst, std::int_fast64_t value) {
  const std::uint_fast64_t v = static_cast<std::uint64_t>(value);
  PushBackUInt32(dst, static_cast<std::uint32_t>(v >> 32));
  PushBackUInt32(dst, static_cast<std::uint32_t>(v));
}

void WriteTzHeader(std::vector<char>* dest, char tzh_version,
                   std::uint_fast32_t tzh_timecnt,
                   std::uint_fast32_t tzh_typecnt,
                   std::uint_fast32_t tzh_charcnt) {
  dest->push_back('T');
  dest->push_back('Z');
  dest->push_back('i');
  dest->push_back('f');

  dest->push_back(tzh_version);
  for (size_t i = 0; i < 15; ++i) {
    dest->push_back(0);
  }

  PushBackInt32(dest, 0);  // tzh_ttisutcnt
  PushBackInt32(dest, 0);  // tzh_ttisstdcnt
  PushBackInt32(dest, 0);  // tzh_leapcnt

  PushBackUInt32(dest, tzh_timecnt);
  PushBackUInt32(dest, tzh_typecnt);
  PushBackUInt32(dest, tzh_charcnt);
}

// Serialize a TransitionTable into TZif binary format (v1+v2 sections +
// proleptic TZ string).
std::unique_ptr<WinZoneInfoSource> SerializeTransitionTable(
    const TransitionTable& transition_table) {
  const std::vector<Transition>& transitions = transition_table.transitions;
  const std::vector<TransitionType>& types = transition_table.transition_types;
  const std::string& abbr_string = transition_table.abbreviation_table;

  if (types.empty()) {
    return nullptr;
  }

  std::vector<char> data;

  // For version 2, write 32-bit section first (for compatibility).
  // Limit transitions to those that fit in 32-bit time_t.
  std::vector<Transition> trans32;
  for (const auto& trans : transitions) {
    if (trans.time >= std::numeric_limits<int32_t>::min() &&
        trans.time <= std::numeric_limits<int32_t>::max()) {
      trans32.push_back(trans);
    }
  }

  // === VERSION 1 SECTION (32-bit) ===
  WriteTzHeader(&data, '2', static_cast<std::uint_fast32_t>(trans32.size()),
                static_cast<std::uint_fast32_t>(types.size()),
                static_cast<std::uint_fast32_t>(abbr_string.size()));

  for (const auto& trans : trans32) {
    PushBackInt32(&data, static_cast<std::int_fast32_t>(trans.time));
  }
  for (const auto& trans : trans32) {
    data.push_back(trans.type_idx);
  }
  for (const auto& type : types) {
    PushBackInt32(&data, type.offset);
    data.push_back(type.is_dst ? 1 : 0);
    data.push_back(type.abbr_idx);
  }
  data.insert(data.end(), abbr_string.begin(), abbr_string.end());

  // === VERSION 2 SECTION (64-bit) ===
  WriteTzHeader(&data, '2', static_cast<std::uint_fast32_t>(transitions.size()),
                static_cast<std::uint_fast32_t>(types.size()),
                static_cast<std::uint_fast32_t>(abbr_string.size()));

  for (const auto& trans : transitions) {
    PushBackInt64(&data, trans.time);
  }
  for (const auto& trans : transitions) {
    data.push_back(trans.type_idx);
  }
  for (const auto& type : types) {
    PushBackInt32(&data, type.offset);
    data.push_back(type.is_dst ? 1 : 0);
    data.push_back(type.abbr_idx);
  }
  data.insert(data.end(), abbr_string.begin(), abbr_string.end());

  // Append the proleptic TZ string.
  data.push_back('\n');
  data.insert(data.end(), transition_table.proleptic_tz_string.begin(),
              transition_table.proleptic_tz_string.end());
  data.push_back('\n');

  return std::make_unique<WinZoneInfoSource>(std::move(data));
}

std::string ToTzAbbrAndOffset(cctz::seconds offset) {
  const auto offset_count = offset.count();
  if (offset_count == 0) {
    return "UTC0";
  }
  const auto abs_count = std::abs(offset_count);
  const auto offset_hour = abs_count / 3600;
  const auto offset_min = (abs_count % 3600) / 60;
  return "<UTC" + cctz::FixedOffsetToAbbr(cctz::seconds(-offset_count)) + ">" +
         (offset_count < 0 ? "-" : "") + std::to_string(offset_hour) +
         (offset_min == 0 ? "" : ":" + std::to_string(offset_min));
}

void Format02d(std::string* str, std::uint_fast8_t v) {
  str->push_back('0' + ((v / 10) % 10));
  str->push_back('0' + (v % 10));
}

void Format01d(std::string* str, std::uint_fast8_t v) {
  if (v >= 10) {
    str->push_back('0' + ((v / 10) % 10));
  }
  str->push_back('0' + (v % 10));
}

std::string ToTzTransitionDateTimeStr(const WinSystemTime& datetime) {
  if (datetime.month == 0) {
    return "";
  }

  // Windows uses "23:59:59.999" to represent a transition at midnight at the
  // end of the rule's day (https://stackoverflow.com/a/47106207). The day after
  // "the Nth weekday W" is not expressible as a fixed week/weekday pair (which
  // occurrence of W+1 it is depends on the month's layout in each year), but
  // POSIX TZ strings accept transition times beyond 24:00:00, so emit the
  // rule's own day with a time of 24:00:00.
  const bool end_of_day = datetime.hour == 23 && datetime.minute == 59 &&
                          datetime.second == 59 && datetime.milliseconds == 999;

  std::string result;
  result.append(",M");
  Format01d(&result, datetime.month);
  result.push_back('.');
  Format01d(&result, datetime.day);
  result.push_back('.');
  Format01d(&result, datetime.day_of_week);
  result.push_back('/');
  if (end_of_day) {
    result.append("24:00:00");
  } else {
    Format01d(&result, datetime.hour);
    result.push_back(':');
    Format02d(&result, datetime.minute);
    result.push_back(':');
    Format02d(&result, datetime.second);
  }

  return result;
}

}  // namespace

std::unique_ptr<ZoneInfoSource> CreateWinZoneInfoSource(
    const WinTimeZoneRegistryInfo& info) {
  return SerializeTransitionTable(TransitionTableBuilder::Build(info));
}

// Construct TZ String Extensions
std::string ToTzString(const WinTimeZoneRegistryEntry& entry) {
  if (entry.standard_date.month == 0 || entry.daylight_date.month == 0) {
    // Windows APIs treat entries as non-DST if either the standard or daylight
    // date is missing.
    return ToTzAbbrAndOffset(cctz::seconds(60 * entry.bias));
  }
  const std::string std_tz =
      ToTzAbbrAndOffset(cctz::seconds(60 * (entry.bias + entry.standard_bias)));
  const std::string dst_tz =
      ToTzAbbrAndOffset(cctz::seconds(60 * (entry.bias + entry.daylight_bias)));
  const std::string dst_start = ToTzTransitionDateTimeStr(entry.daylight_date);
  const std::string std_start = ToTzTransitionDateTimeStr(entry.standard_date);
  return std_tz + dst_tz + dst_start + std_start;
}

}  // namespace cctz
