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

#include "zone_info_source_win.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cctz/zone_info_source.h"
#include "time_zone_fixed.h"
#include "time_zone_if.h"
#include "tzfile.h"

namespace cctz {
namespace {

const year_t kTransitionStartYear = 1970;

const weekday kWeekdays[] = {
    weekday::sunday,   weekday::monday, weekday::tuesday, weekday::wednesday,
    weekday::thursday, weekday::friday, weekday::saturday};

// When DST starts or ends, as a local time in the offset in effect before it.
struct TransitionRule {
  TransitionRule()
      : absolute(false), date(), month(0), week(0), weekday(0), time(0) {}

  bool absolute;           // Fires only on `date`, or every year.
  civil_day date;          // Absolute rules only.
  int month;               // Recurring rules only: [1:12].
  int week;                // Recurring rules only: [1:5] (5 == last).
  int weekday;             // Recurring rules only: 0 == Sunday, ..., 6.
  std::int_fast32_t time;  // Seconds after midnight: [0:86400].

  bool operator==(const TransitionRule& o) const {
    if (absolute != o.absolute || time != o.time) {
      return false;
    }
    return absolute
               ? date == o.date
               : month == o.month && week == o.week && weekday == o.weekday;
  }
};

// The meaning of a "TZI" registry value, with Windows' conventions applied.
struct ZoneRule {
  ZoneRule() : std_offset(0), has_dst(false), dst_offset(0) {}

  std::int_fast32_t std_offset;  // Seconds east of UTC.
  bool has_dst;
  std::int_fast32_t dst_offset;  // Seconds east of UTC (has_dst only).
  TransitionRule dst_start;      // has_dst only.
  TransitionRule dst_end;        // has_dst only.

  // Compares only the fields in effect, i.e. whether the rules behave the same.
  bool operator==(const ZoneRule& o) const {
    if (std_offset != o.std_offset || has_dst != o.has_dst) {
      return false;
    }
    return !has_dst || (dst_offset == o.dst_offset &&
                        dst_start == o.dst_start && dst_end == o.dst_end);
  }
};

// rules[i] applies to first_year + i, and the first/last rules to all
// earlier/later years. A single rule applies to all years.
struct ZoneRules {
  std::vector<ZoneRule> rules;
  year_t first_year;
};

struct Transition {
  Transition() : time(0), type_idx(0) {}
  std::int_fast64_t time;      // Unix timestamp
  std::uint_fast8_t type_idx;  // Index into transition types
};

// In the field order of TZif's ttinfo record.
struct TransitionType {
  TransitionType() : offset(0), is_dst(false), abbr_idx(0) {}
  std::int_fast32_t offset;    // Seconds east of UTC
  bool is_dst;                 // Is daylight saving time
  std::uint_fast8_t abbr_idx;  // Index into abbreviation string
};

struct TransitionTable {
  std::vector<Transition> transitions;
  std::vector<TransitionType> transition_types;
  std::string abbreviation_table;
  std::string proleptic_tz_string;
};

// Appends a non-negative number of seconds in the POSIX TZ string time format
// "hh[:mm[:ss]]", where only the hour may be a single digit. Trailing zero
// fields are omitted as zic does, e.g. "2", "5:30" or "24".
void AppendHms(std::string& out, std::int_fast32_t secs) {
  out.append(std::to_string(secs / 3600));
  const std::int_fast32_t fields[] = {(secs / 60) % 60, secs % 60};
  const int num_fields = fields[1] != 0 ? 2 : fields[0] != 0 ? 1 : 0;
  for (int i = 0; i < num_fields; ++i) {
    out.push_back(':');
    out.push_back(static_cast<char>('0' + fields[i] / 10));
    out.push_back(static_cast<char>('0' + fields[i] % 10));
  }
}

// Appends an offset in seconds east of UTC as a POSIX TZ string zone, e.g.
// "<UTC+0530>-5:30".
void AppendTzAbbrAndOffset(std::string& out, std::int_fast32_t offset) {
  if (offset == 0) {
    out.append("UTC0");
    return;
  }
  out.append("<UTC");
  out.append(FixedOffsetToAbbr(seconds(offset)));
  out.push_back('>');
  if (offset > 0) {
    out.push_back('-');
  }
  AppendHms(out, offset < 0 ? -offset : offset);
}

// Appends a recurring rule as a POSIX TZ string rule, e.g. ",M3.2.0/2".
// Windows' "23:59:59.999" end-of-day convention becomes "24" on the rule's own
// day, as the day after "the Nth weekday W" is not expressible as a fixed
// week/weekday pair (https://stackoverflow.com/a/47106207).
void AppendTzRule(std::string& out, const TransitionRule& rule) {
  out.append(",M");
  out.append(std::to_string(rule.month));
  out.push_back('.');
  out.append(std::to_string(rule.week));
  out.push_back('.');
  out.append(std::to_string(rule.weekday));
  out.push_back('/');
  AppendHms(out, rule.time);
}

// Constructs the proleptic TZ string. Absolute rules have no POSIX
// representation, so `rule` must not have them (see CreateWinZoneInfoSource).
std::string ToTzString(const ZoneRule& rule) {
  std::string result;
  AppendTzAbbrAndOffset(result, rule.std_offset);
  if (rule.has_dst) {
    AppendTzAbbrAndOffset(result, rule.dst_offset);
    AppendTzRule(result, rule.dst_start);
    AppendTzRule(result, rule.dst_end);
  }
  return result;
}

class TransitionTableBuilder {
 public:
  static TransitionTable Build(const ZoneRules& zone) {
    TransitionTableBuilder builder;
    builder.Initialize(zone);
    if (builder.failed_) {
      return TransitionTable{};
    }
    return TransitionTable{std::move(builder.transitions_),
                           std::move(builder.transition_types_),
                           std::move(builder.abbreviation_table_),
                           std::move(builder.proleptic_tz_string_)};
  }

 private:
  TransitionTableBuilder() : last_offset_(), failed_(false) {}

  struct OffsetDstPair {
    std::int_fast32_t offset;  // Seconds east of UTC
    bool is_dst;
    bool operator==(const OffsetDstPair& o) const {
      return offset == o.offset && is_dst == o.is_dst;
    }
  };

  // Windows applies the first/last rule to all earlier/later years.
  static const ZoneRule& RuleForYear(const ZoneRules& zone, year_t year) {
    if (year <= zone.first_year) {
      return zone.rules.front();
    }
    const std::size_t index = static_cast<std::size_t>(year - zone.first_year);
    return index < zone.rules.size() ? zone.rules[index] : zone.rules.back();
  }

  // Covers [min(first_year, 1970), the last rule's year], so trimming `zone`
  // first (see TrimZoneRules) keeps the table short.
  void Initialize(const ZoneRules& zone) {
    const year_t first_year =
        std::min<year_t>(zone.first_year, kTransitionStartYear);
    const year_t last_year =
        zone.first_year + static_cast<year_t>(zone.rules.size());

    // Civil times are converted with the offset in effect before them, which
    // before the first transition is the first rule's standard offset.
    last_offset_ = OffsetDstPair{zone.rules.front().std_offset, false};

    // At most three transitions per year (year begin plus the two rule dates),
    // and no more than TimeZoneInfo::Load() accepts (see AddTransition).
    transitions_.reserve(std::min<std::size_t>(
        static_cast<std::size_t>(last_year - first_year) * 3, TZ_MAX_TIMES));
    for (year_t year = first_year; year < last_year; ++year) {
      ProcessYear(RuleForYear(zone, year), year);
      if (failed_) {
        return;
      }
    }
    proleptic_tz_string_ = ToTzString(zone.rules.back());
  }

  // Returns false if the rule does not fire in `year`.
  static bool ResolveRule(const TransitionRule& rule, year_t year,
                          civil_second& result) {
    civil_day day;
    if (rule.absolute) {
      if (rule.date.year() != year) {
        return false;
      }
      day = rule.date;
    } else if (rule.week == 5) {
      day = prev_weekday(civil_day(year, rule.month + 1, 1),
                         kWeekdays[rule.weekday]);
    } else {
      day = next_weekday(civil_day(year, rule.month, 1) - 1,
                         kWeekdays[rule.weekday]) +
            (rule.week - 1) * 7;
    }
    result = civil_second(day) + rule.time;
    return true;
  }

  void ProcessYear(const ZoneRule& rule, year_t year) {
    const civil_second year_begin(year, 1, 1, 0, 0, 0);
    const OffsetDstPair std_offset{rule.std_offset, false};
    const OffsetDstPair dst_offset{rule.dst_offset, true};

    civil_second std_begin;
    civil_second dst_begin;
    const bool has_std_begin =
        rule.has_dst && ResolveRule(rule.dst_end, year, std_begin);
    const bool has_dst_begin =
        rule.has_dst && ResolveRule(rule.dst_start, year, dst_begin);

    // A rule date at the year begin determines the year-begin state itself.
    if (!(has_std_begin && std_begin == year_begin) &&
        !(has_dst_begin && dst_begin == year_begin)) {
      // Windows treats a local time as DST when
      //   DaylightDate <= t < StandardDate   (if DaylightDate < StandardDate)
      //   t >= DaylightDate || t < StandardDate  (otherwise, i.e. when DST
      //                                           wraps the year end as in the
      //                                           southern hemisphere)
      // so Jan 1 falls in DST iff the standard transition both exists and
      // precedes the daylight one within the year.
      const bool year_begins_in_dst =
          has_std_begin && has_dst_begin && std_begin < dst_begin;
      AddTransition(year_begin, year_begins_in_dst ? dst_offset : std_offset);
    }

    // Apply the rule dates in chronological order.
    const bool std_first =
        has_std_begin && (!has_dst_begin || std_begin < dst_begin);
    if (std_first) {
      AddTransition(std_begin, std_offset);
    }
    if (has_dst_begin) {
      AddTransition(dst_begin, dst_offset);
    }
    if (has_std_begin && !std_first) {
      AddTransition(std_begin, std_offset);
    }
  }

  // Adds a transition to `to_offset` at `from_civil_time`, which is in the
  // offset in effect before it, unless the offset stays the same. The first
  // transition is always added so that the table starts at the first year.
  void AddTransition(civil_second from_civil_time, OffsetDstPair to_offset) {
    if (failed_ || (!transitions_.empty() && last_offset_ == to_offset)) {
      return;
    }

    std::size_t type_index = 0;
    while (type_index < transition_types_.size() &&
           !(transition_types_[type_index].offset == to_offset.offset &&
             transition_types_[type_index].is_dst == to_offset.is_dst)) {
      ++type_index;
    }
    if (type_index == transition_types_.size()) {
      TransitionType new_type;
      new_type.offset = to_offset.offset;
      new_type.is_dst = to_offset.is_dst;
      if (type_index > std::numeric_limits<std::uint8_t>::max() ||
          !FindOrAddAbbreviation(new_type.offset, new_type.abbr_idx)) {
        failed_ = true;
        return;
      }
      transition_types_.push_back(new_type);
    }

    if (transitions_.size() >= TZ_MAX_TIMES) {
      failed_ = true;
      return;
    }
    Transition transition;
    transition.time = seconds(from_civil_time - last_offset_.offset -
                              civil_second(1970, 1, 1, 0, 0, 0))
                          .count();
    transition.type_idx = static_cast<std::uint8_t>(type_index);
    transitions_.push_back(transition);
    last_offset_ = to_offset;
  }

  // The abbreviation depends only on the offset, so it is shared by all types
  // with the same offset.
  bool FindOrAddAbbreviation(std::int_fast32_t offset,
                             std::uint_fast8_t& abbr_idx) {
    for (const TransitionType& tt : transition_types_) {
      if (tt.offset == offset) {
        abbr_idx = tt.abbr_idx;
        return true;
      }
    }
    const std::size_t index = abbreviation_table_.size();
    if (index > std::numeric_limits<std::uint8_t>::max()) {
      return false;
    }
    abbreviation_table_.append("UTC");
    if (offset != 0) {
      abbreviation_table_.append(FixedOffsetToAbbr(seconds(offset)));
    }
    // Table entries are stored NUL-terminated.
    abbreviation_table_.push_back('\0');
    abbr_idx = static_cast<std::uint_fast8_t>(index);
    return true;
  }

  std::vector<Transition> transitions_;
  std::vector<TransitionType> transition_types_;
  std::string abbreviation_table_;
  std::string proleptic_tz_string_;
  OffsetDstPair last_offset_;
  // Set when the table exceeds what TimeZoneInfo::Load() accepts: the TZif
  // uint8 indices or TZ_MAX_TIMES transitions. Abbreviations such as
  // "UTC+0530" take up to 9 bytes each, so the former happens only with more
  // than about 28 distinct offsets, far more than any real zone has. The latter
  // happens only when the table spans about a thousand years of DST, e.g. with
  // "Dynamic DST" years starting around 3000, whereas real ones start in the
  // 2000s.
  bool failed_;
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
void PushBackUInt32(std::vector<char>& out, std::uint_fast32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xff));
  out.push_back(static_cast<char>((value >> 16) & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

void PushBackInt32(std::vector<char>& out, std::int_fast32_t value) {
  PushBackUInt32(out, static_cast<std::uint32_t>(value));
}

void PushBackInt64(std::vector<char>& out, std::int_fast64_t value) {
  const std::uint_fast64_t v = static_cast<std::uint64_t>(value);
  PushBackUInt32(out, static_cast<std::uint32_t>(v >> 32));
  PushBackUInt32(out, static_cast<std::uint32_t>(v));
}

void WriteTzHeader(std::vector<char>& out, char tzh_version,
                   std::uint_fast32_t tzh_timecnt,
                   std::uint_fast32_t tzh_typecnt,
                   std::uint_fast32_t tzh_charcnt) {
  out.push_back('T');
  out.push_back('Z');
  out.push_back('i');
  out.push_back('f');

  out.push_back(tzh_version);
  for (std::size_t i = 0; i < 15; ++i) {
    out.push_back(0);
  }

  PushBackInt32(out, 0);  // tzh_ttisutcnt
  PushBackInt32(out, 0);  // tzh_ttisstdcnt
  PushBackInt32(out, 0);  // tzh_leapcnt

  PushBackUInt32(out, tzh_timecnt);
  PushBackUInt32(out, tzh_typecnt);
  PushBackUInt32(out, tzh_charcnt);
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

  // === VERSION 1 SECTION (32-bit) ===
  // Version 2+ readers, including TimeZoneInfo::Load(), skip this section, so
  // write a minimal one: no transitions and a single UTC type with an empty
  // abbreviation (as "zic -b slim" does).
  WriteTzHeader(data, '2', 0, 1, 1);
  PushBackInt32(data, 0);   // tt_utoff
  data.push_back(0);        // tt_isdst
  data.push_back(0);        // tt_desigidx
  data.push_back(0);        // ""

  // === VERSION 2 SECTION (64-bit) ===
  WriteTzHeader(data, '2', static_cast<std::uint_fast32_t>(transitions.size()),
                static_cast<std::uint_fast32_t>(types.size()),
                static_cast<std::uint_fast32_t>(abbr_string.size()));

  for (const auto& trans : transitions) {
    PushBackInt64(data, trans.time);
  }
  for (const auto& trans : transitions) {
    data.push_back(static_cast<char>(trans.type_idx));
  }
  for (const auto& type : types) {
    PushBackInt32(data, type.offset);
    data.push_back(type.is_dst ? 1 : 0);
    data.push_back(static_cast<char>(type.abbr_idx));
  }
  data.insert(data.end(), abbr_string.begin(), abbr_string.end());

  // Append the proleptic TZ string.
  data.push_back('\n');
  data.insert(data.end(), transition_table.proleptic_tz_string.begin(),
              transition_table.proleptic_tz_string.end());
  data.push_back('\n');

  return std::unique_ptr<WinZoneInfoSource>(
      new WinZoneInfoSource(std::move(data)));
}

// Helpers to decode the little-endian integers in REG_TZI_FORMAT.
std::uint_fast16_t DecodeUInt16(const unsigned char* p) {
  return static_cast<std::uint_fast16_t>(p[0] | (p[1] << 8));
}

std::int_fast32_t DecodeInt32(const unsigned char* p) {
  const std::uint_fast32_t v = static_cast<std::uint_fast32_t>(p[0]) |
                               (static_cast<std::uint_fast32_t>(p[1]) << 8) |
                               (static_cast<std::uint_fast32_t>(p[2]) << 16) |
                               (static_cast<std::uint_fast32_t>(p[3]) << 24);
  // Avoid the unsigned-to-signed conversion of values beyond INT32_MAX, which
  // is implementation-defined before C++20.
  return v <= 0x7fffffff ? static_cast<std::int_fast32_t>(v)
                         : -static_cast<std::int_fast32_t>(0xffffffff - v) - 1;
}

// Decodes a SYSTEMTIME used as a transition date. Sets `present` to false
// for the all-zero "no transition" value.
// https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/ns-timezoneapi-time_zone_information#members
bool DecodeTransitionRule(const unsigned char* p, bool& present,
                          TransitionRule& rule) {
  const std::uint_fast16_t year = DecodeUInt16(p);
  const std::uint_fast16_t month = DecodeUInt16(p + 2);
  const std::uint_fast16_t day_of_week = DecodeUInt16(p + 4);
  const std::uint_fast16_t day = DecodeUInt16(p + 6);
  const std::uint_fast16_t hour = DecodeUInt16(p + 8);
  const std::uint_fast16_t minute = DecodeUInt16(p + 10);
  const std::uint_fast16_t second = DecodeUInt16(p + 12);
  const std::uint_fast16_t milliseconds = DecodeUInt16(p + 14);

  if (month == 0) {
    // No transition - all fields must be zero
    present = false;
    return year == 0 && day_of_week == 0 && day == 0 && hour == 0 &&
           minute == 0 && second == 0 && milliseconds == 0;
  }

  if (year == 0) {
    // Recurring rule: the day-th (5 == last) day_of_week of the month.
    if (!(month <= 12 && 1 <= day && day <= 5 && day_of_week < 7)) {
      return false;
    }
  } else {
    // Absolute date
    if (!(kMinSystemTimeYear <= year && year <= kMaxSystemTimeYear &&
          month <= 12 && 1 <= day && day <= 31 && day_of_week < 7)) {
      return false;
    }
  }
  if (24 <= hour || 60 <= minute || 60 <= second || 1000 <= milliseconds) {
    return false;
  }

  present = true;
  rule = TransitionRule();
  rule.absolute = year != 0;
  if (rule.absolute) {
    rule.date = civil_day(static_cast<year_t>(year), static_cast<int>(month),
                          static_cast<int>(day));
  } else {
    rule.month = static_cast<int>(month);
    rule.week = static_cast<int>(day);
    rule.weekday = static_cast<int>(day_of_week);
  }
  // Windows uses "23:59:59.999" to mean midnight at the end of the day. Other
  // milliseconds are dropped.
  rule.time =
      static_cast<std::int_fast32_t>(hour * 3600 + minute * 60 + second);
  if (rule.time == 86399 && milliseconds == 999) {
    rule.time = 86400;
  }
  return true;
}

// Whether `offset` is within the range RFC 9636 allows for a TZif utoff
// (-25h < utoff < 26h), which TimeZoneInfo::Load() enforces.
bool IsValidUtcOffset(std::int_fast64_t offset) {
  return -25 * 60 * 60 < offset && offset < 26 * 60 * 60;
}

// Whether `offset` is expressible in a POSIX TZ string, whose offsets are
// "[+-]hh[:mm[:ss]]" with hh in [0:24].
bool IsValidTzStringOffset(std::int_fast32_t offset) {
  return -25 * 60 * 60 < offset && offset < 25 * 60 * 60;
}

// Decodes and validates a "TZI" registry value.
bool DecodeRegTzi(const WinRegTzi& tzi, ZoneRule& rule) {
  const unsigned char* p = tzi.data();

  const std::int_fast32_t bias = DecodeInt32(p);
  const std::int_fast32_t standard_bias = DecodeInt32(p + 4);
  const std::int_fast32_t daylight_bias = DecodeInt32(p + 8);

  bool has_standard_date;
  bool has_daylight_date;
  if (!DecodeTransitionRule(p + 12, has_standard_date, rule.dst_end) ||
      !DecodeTransitionRule(p + 28, has_daylight_date, rule.dst_start)) {
    return false;
  }

  // Windows observes DST only when both dates are set, and ignores
  // StandardBias otherwise (Win32 APIs and .NET alike).
  rule.has_dst = has_standard_date && has_daylight_date;
  // Computed in 64 bits, which cannot overflow for any 32-bit biases.
  const std::int_fast64_t std_offset =
      -60 * (std::int_fast64_t{bias} + (rule.has_dst ? standard_bias : 0));
  const std::int_fast64_t dst_offset =
      -60 * (std::int_fast64_t{bias} + daylight_bias);
  if (!IsValidUtcOffset(std_offset) ||
      (rule.has_dst && !IsValidUtcOffset(dst_offset))) {
    return false;
  }
  rule.std_offset = static_cast<std::int_fast32_t>(std_offset);
  rule.dst_offset =
      rule.has_dst ? static_cast<std::int_fast32_t>(dst_offset) : 0;
  return true;
}

// Drops the leading/trailing rules that equal their neighbor. As the first/last
// rules apply to all earlier/later years anyway, this does not change the rule
// of any year, but it keeps the transition table from spanning years the
// proleptic TZ string covers. A single remaining rule applies to all years, so
// the table needs only its first year.
void TrimZoneRules(ZoneRules& zone) {
  std::vector<ZoneRule>& rules = zone.rules;
  while (rules.size() > 1 && rules[rules.size() - 2] == rules.back()) {
    rules.pop_back();
  }
  std::size_t leading = 0;
  while (leading + 1 < rules.size() && rules[leading] == rules[leading + 1]) {
    ++leading;
  }
  rules.erase(rules.begin(),
              rules.begin() + static_cast<std::ptrdiff_t>(leading));
  zone.first_year += static_cast<year_t>(leading);
  if (rules.size() == 1) {
    zone.first_year = kTransitionStartYear;
  }
}

}  // namespace

std::unique_ptr<ZoneInfoSource> CreateWinZoneInfoSource(
    year_t first_year, const std::vector<WinRegTzi>& years) {
  if (years.empty() || first_year < kMinSystemTimeYear ||
      kMaxSystemTimeYear - first_year < static_cast<year_t>(years.size()) - 1) {
    return nullptr;
  }
  ZoneRules zone;
  zone.rules.resize(years.size());
  zone.first_year = first_year;
  for (std::size_t i = 0; i < years.size(); ++i) {
    if (!DecodeRegTzi(years[i], zone.rules[i])) {
      return nullptr;
    }
  }
  // The last rule applies to all later years, so it becomes the proleptic TZ
  // string, which cannot express absolute rules or offsets of 25h or more.
  const ZoneRule& last_rule = zone.rules.back();
  if (!IsValidTzStringOffset(last_rule.std_offset) ||
      (last_rule.has_dst &&
       (!IsValidTzStringOffset(last_rule.dst_offset) ||
        last_rule.dst_start.absolute || last_rule.dst_end.absolute))) {
    return nullptr;
  }
  TrimZoneRules(zone);
  return SerializeTransitionTable(TransitionTableBuilder::Build(zone));
}

std::unique_ptr<ZoneInfoSource> CreateWinZoneInfoSource(const WinRegTzi& tzi) {
  return CreateWinZoneInfoSource(kTransitionStartYear, {tzi});
}

}  // namespace cctz
