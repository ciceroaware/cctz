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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "cctz/civil_time.h"
#include "cctz/time_zone.h"
#include "cctz/zone_info_source.h"
#include "gtest/gtest.h"
#include "tzfile.h"

namespace cctz {
namespace {

// Encodes a "TZI" registry value (REG_TZI_FORMAT) from its fields, each date
// given as SYSTEMTIME's {wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute,
// wSecond, wMilliseconds}.
WinRegTzi Tzi(std::int32_t bias, std::int32_t standard_bias,
              std::int32_t daylight_bias,
              const std::uint16_t (&standard_date)[8],
              const std::uint16_t (&daylight_date)[8]) {
  WinRegTzi tzi;
  std::size_t i = 0;
  for (std::int32_t v : {bias, standard_bias, daylight_bias}) {
    const std::uint32_t u = static_cast<std::uint32_t>(v);
    for (int shift = 0; shift < 32; shift += 8) {
      tzi[i++] = static_cast<unsigned char>((u >> shift) & 0xff);
    }
  }
  for (const std::uint16_t* date : {standard_date, daylight_date}) {
    for (int j = 0; j < 8; ++j) {
      tzi[i++] = static_cast<unsigned char>(date[j] & 0xff);
      tzi[i++] = static_cast<unsigned char>(date[j] >> 8);
    }
  }
  return tzi;
}

const std::uint16_t kNoDate[8] = {0, 0, 0, 0, 0, 0, 0, 0};
const std::uint16_t kMarch2ndSunday[8] = {0, 3, 0, 2, 2, 0, 0, 0};
const std::uint16_t kNovember1stSunday[8] = {0, 11, 0, 1, 2, 0, 0, 0};

// A zone in DST (UTC-5/-4) from 1970 through `last_dst_year`, and at UTC-6
// after that. For `last_dst_year` >= 1970, it has one transition at the 1970
// year begin, two per year of DST, and one at the year begin after the DST
// years, which is 2 * (last_dst_year - 1969) + 2 transitions in total.
std::unique_ptr<ZoneInfoSource> DstThrough(year_t last_dst_year) {
  return CreateWinZoneInfoSource(
      last_dst_year, {Tzi(300, 0, -60, kNovember1stSunday, kMarch2ndSunday),
                      Tzi(360, 0, 0, kNoDate, kNoDate)});
}

// The last `last_dst_year` for which DstThrough() needs no more than the
// TZ_MAX_TIMES transitions that TimeZoneInfo::Load() accepts.
const year_t kMaxTransitionsLastDstYear = 1969 + (TZ_MAX_TIMES - 2) / 2;

// Whether DstThrough() can be at both sides of the TZ_MAX_TIMES boundary, which
// it cannot for a TZ_MAX_TIMES so small or so large that the boundary is before
// 1970 or beyond SYSTEMTIME's year range.
bool CanTestMaxTransitions() {
  return 1970 <= kMaxTransitionsLastDstYear &&
         kMaxTransitionsLastDstYear + 2 <= kMaxSystemTimeYear;
}

// Returns the source of the test zone `name`, or nullptr if there is none.
std::unique_ptr<ZoneInfoSource> CreateTestZone(const std::string& name) {
  // The zones named "reg:<registry key>" are snapshots of those keys under
  // HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time Zones, captured on
  // Windows 11 (build 26300) with time zone data version 0x07EA0005 - the
  // "TzVersion" value of that key, meaning the fifth 2026 data revision. Each
  // holds exactly what LoadWinZoneInfoSource() passes to
  // CreateWinZoneInfoSource() for the zone: the "Dynamic DST" values for
  // FirstEntry..LastEntry, or, when the zone has no "Dynamic DST" key, the
  // zone's base "TZI" value.
  if (name == "reg:Afghanistan Standard Time") {
    return CreateWinZoneInfoSource(Tzi(-270, 0, -60, kNoDate, kNoDate));
  }
  if (name == "reg:Alberta Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(420, 0, -60, {0, 10, 0, 5, 2, 0, 0, 0}, {0, 4, 0, 1, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(420, 0, -60, {0, 1, 4, 1, 0, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
        Tzi(360, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2006, years);
  }
  if (name == "reg:Cen. Australia Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(-570, 0, -60, {0, 3, 0, 5, 3, 0, 0, 0}, {0, 10, 0, 5, 2, 0, 0, 0}),
        Tzi(-570, 0, -60, {0, 4, 0, 1, 3, 0, 0, 0}, {0, 10, 0, 1, 2, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2007, years);
  }
  if (name == "reg:Eastern Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(300, 0, -60, {0, 10, 0, 5, 2, 0, 0, 0}, {0, 4, 0, 1, 2, 0, 0, 0}),
        Tzi(300, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2006, years);
  }
  if (name == "reg:Hawaiian Standard Time") {
    return CreateWinZoneInfoSource(Tzi(600, 0, -60, kNoDate, kNoDate));
  }
  if (name == "reg:India Standard Time") {
    return CreateWinZoneInfoSource(Tzi(-330, 0, -60, kNoDate, kNoDate));
  }
  if (name == "reg:Lord Howe Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(-630, 0, -30, {0, 3, 0, 5, 2, 0, 0, 0}, {0, 10, 0, 5, 2, 0, 0, 0}),
        Tzi(-630, 0, -30, {0, 4, 0, 1, 2, 0, 0, 0}, {0, 10, 0, 5, 2, 0, 0, 0}),
        Tzi(-630, 0, -30, {0, 3, 0, 5, 2, 0, 0, 0}, {0, 10, 0, 5, 2, 0, 0, 0}),
        Tzi(-630, 0, -30, {0, 4, 0, 1, 2, 0, 0, 0}, {0, 10, 0, 1, 2, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2005, years);
  }
  if (name == "reg:Nepal Standard Time") {
    return CreateWinZoneInfoSource(Tzi(-345, 0, -60, kNoDate, kNoDate));
  }
  if (name == "reg:Newfoundland Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(210, 0, -60, {0, 10, 0, 5, 0, 1, 0, 0}, {0, 4, 0, 1, 0, 1, 0, 0}),
        Tzi(210, 0, -60, {0, 11, 0, 1, 0, 1, 0, 0}, {0, 3, 0, 2, 0, 1, 0, 0}),
        Tzi(210, 0, -60, {0, 11, 0, 1, 0, 1, 0, 0}, {0, 3, 0, 2, 0, 1, 0, 0}),
        Tzi(210, 0, -60, {0, 11, 0, 1, 0, 1, 0, 0}, {0, 3, 0, 2, 0, 1, 0, 0}),
        Tzi(210, 0, -60, {0, 11, 0, 1, 0, 1, 0, 0}, {0, 3, 0, 2, 0, 1, 0, 0}),
        Tzi(210, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 0, 1, 0, 0}),
        Tzi(210, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, {0, 3, 0, 2, 2, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2006, years);
  }
  if (name == "reg:UTC") {
    return CreateWinZoneInfoSource(Tzi(0, 0, 0, kNoDate, kNoDate));
  }
  if (name == "reg:Volgograd Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(-180, 0, -60, {0, 10, 0, 5, 3, 0, 0, 0}, {0, 3, 0, 5, 2, 0, 0, 0}),
        Tzi(-180, 0, -60, {0, 1, 6, 1, 0, 0, 0, 0}, {0, 3, 0, 5, 2, 0, 0, 0}),
        Tzi(-240, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-240, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-180, 0, -60, {0, 10, 0, 5, 2, 0, 0, 0}, {0, 1, 3, 1, 0, 0, 0, 0}),
        Tzi(-180, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-180, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-180, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-240, 0, 60, {0, 10, 0, 5, 2, 0, 0, 0}, {0, 1, 1, 1, 0, 0, 0, 0}),
        Tzi(-240, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-240, 0, -60, {0, 12, 0, 5, 2, 0, 0, 0}, {0, 1, 3, 1, 0, 0, 0, 0}),
        Tzi(-180, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2010, years);
  }
  if (name == "reg:Yakutsk Standard Time") {
    const std::vector<WinRegTzi> years = {
        Tzi(-540, 0, -60, {0, 10, 0, 5, 3, 0, 0, 0}, {0, 3, 0, 5, 2, 0, 0, 0}),
        Tzi(-540, 0, -60, {0, 1, 6, 1, 0, 0, 0, 0}, {0, 3, 0, 5, 2, 0, 0, 0}),
        Tzi(-600, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-600, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
        Tzi(-540, 0, -60, {0, 10, 0, 5, 2, 0, 0, 0}, {0, 1, 3, 1, 0, 0, 0, 0}),
        Tzi(-540, 0, -60, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}),
    };
    return CreateWinZoneInfoSource(2010, years);
  }

  // The zones named "test:<name>" are other synthetic zones used for testing.
  if (name == "test:FixedWithStandardBias") {
    // A fixed-offset zone with a nonzero StandardBias. The Win32 API ignores
    // StandardBias when there are no transition dates, so the offset is
    // -Bias = UTC+9. The proleptic TZ string must use the same rule as the
    // transition table or TimeZoneInfo::Load() rejects the whole zone.
    return CreateWinZoneInfoSource(Tzi(-540, -60, -60, kNoDate, kNoDate));
  }
  if (name == "test:FixedHalfHourEast") {
    // A fixed-offset zone 30 minutes east of UTC (bias=-30). POSIX offsets
    // within the first hour east of UTC need an explicit "-0" hour ("-0:30");
    // truncating integer division would drop the sign and flip the zone to
    // UTC-00:30.
    return CreateWinZoneInfoSource(Tzi(-30, 0, -60, kNoDate, kNoDate));
  }
  if (name == "test:FixedFiveMinutesPastWest") {
    // A fixed-offset zone at UTC-05:05 (bias=305), whose single-digit minutes
    // must still be written as two digits in the POSIX TZ string ("5:05").
    return CreateWinZoneInfoSource(Tzi(305, 0, -60, kNoDate, kNoDate));
  }
  if (name == "test:FixedEastmost") {
    // The eastmost offset a POSIX TZ string can express, UTC+24:59.
    return CreateWinZoneInfoSource(Tzi(-1499, 0, 0, kNoDate, kNoDate));
  }
  if (name == "test:FixedWestmost") {
    // The westmost offset RFC 9636 allows, UTC-24:59.
    return CreateWinZoneInfoSource(Tzi(1499, 0, 0, kNoDate, kNoDate));
  }

  if (name == "test:MaxTransitions") {
    // As many transitions as TZ_MAX_TIMES allows (one fewer if it is odd), e.g.
    // DST through 2968 for the default TZ_MAX_TIMES of 2000.
    return DstThrough(kMaxTransitionsLastDstYear);
  }

  // The zones named "test:Lone<name>" are synthetic zones used to test cases
  // where only one of StandardDate or DaylightDate is set. Windows requires
  // both dates for a zone that observes DST, so such entries never observe DST
  // and must behave as UTC+1 year-round (and must not emit a TZ string with a
  // DST offset but fewer than two rule dates, which would be unparseable and
  // make the zone fail to load). Windows (Win32 APIs,
  // SystemTimeToTzSpecificLocalTime, and .NET alike) also ignores StandardBias
  // for such entries.
  if (name == "test:LoneDaylightDate") {
    return CreateWinZoneInfoSource(
        Tzi(-60, 0, -60, kNoDate, {0, 3, 0, 5, 2, 0, 0, 0}));
  }
  if (name == "test:LoneStandardDate") {
    return CreateWinZoneInfoSource(
        Tzi(-60, 0, -60, {0, 10, 0, 5, 3, 0, 0, 0}, kNoDate));
  }
  if (name == "test:LoneDaylightDateStandardBias") {
    return CreateWinZoneInfoSource(
        Tzi(-60, -30, -60, kNoDate, {0, 3, 0, 5, 2, 0, 0, 0}));
  }
  if (name == "test:LoneStandardDateStandardBias") {
    return CreateWinZoneInfoSource(
        Tzi(-60, -30, -60, {0, 10, 0, 5, 3, 0, 0, 0}, kNoDate));
  }

  if (name == "test:EndOfDayRule") {
    // A zone whose proleptic rule uses Windows' "23:59:59.999" end-of-day
    // convention (as the real West Bank entries did in 2018-2019): DST begins
    // at midnight following the last Thursday of March. In 2022 the last
    // Thursday of March is Mar 31, so DST begins Apr 1 - a fixed week/weekday
    // rule such as "last Friday of March" (Mar 25) would be a week early. The
    // POSIX TZ string therefore uses "M3.5.4/24".
    return CreateWinZoneInfoSource(Tzi(-120, 0, -60, {0, 10, 6, 5, 1, 0, 0, 0},
                                       {0, 3, 4, 5, 23, 59, 59, 999}));
  }
  if (name == "test:AbsoluteEndOfDayRule") {
    // Absolute-dated (wYear != 0) rules with the 23:59:59.999 end-of-day
    // convention: base UTC, DST UTC+1 from the midnight ending Jun 15, 2020
    // until Oct 15, 2020 02:00 local DST. Windows applies the end-of-day
    // convention to absolute rules exactly as to recurring ones (verified
    // against SystemTimeToTzSpecificLocalTime and its Ex variant with
    // registry-backed Dynamic DST data): the last second of Jun 15 still
    // belongs to standard time.
    return CreateWinZoneInfoSource(
        2020, {Tzi(0, 0, -60, {2020, 10, 4, 15, 2, 0, 0, 0},
                   {2020, 6, 1, 15, 23, 59, 59, 999}),
               Tzi(0, 0, -60, kNoDate, kNoDate)});
  }
  if (name == "test:IdenticalYears") {
    // "Dynamic DST" values that are all the same, i.e. a zone that is
    // equivalent to a single "TZI" value with the 2007+ US rules.
    const WinRegTzi us =
        Tzi(300, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, kMarch2ndSunday);
    return CreateWinZoneInfoSource(2010, {us, us, us});
  }
  return nullptr;
}

// Serves the zones of CreateTestZone(), so that the TZif data
// CreateWinZoneInfoSource() produces is validated by exactly the same code path
// as a zone loaded from the registry itself, including the consistency of the
// proleptic TZ string with the transitions.
std::unique_ptr<ZoneInfoSource> TestZoneInfoSourceFactory(
    const std::string& name,
    const std::function<std::unique_ptr<ZoneInfoSource>(const std::string&)>&
        fallback) {
  if (auto z = CreateTestZone(name)) return z;
  EXPECT_NE(name.compare(0, 5, "test:"), 0) << "Unknown test zone: " << name;
  return fallback(name);
}

class WinZoneInfoSourceTest : public ::testing::Test {
 protected:
  WinZoneInfoSourceTest() {
    saved_factory = cctz_extension::zone_info_source_factory;
    cctz_extension::zone_info_source_factory = TestZoneInfoSourceFactory;
  }

  ~WinZoneInfoSourceTest() {
    cctz_extension::zone_info_source_factory = saved_factory;
  }

 private:
  cctz_extension::ZoneInfoSourceFactory saved_factory;
};

const char kTimeFormat[] = "%Y-%m-%d %H:%M:%S";

// Parses "YYYY-MM-DD hh:mm:ss".
civil_second ParseCivil(const std::string& s) {
  const time_zone utc = utc_time_zone();
  time_point<seconds> tp;
  EXPECT_TRUE(parse(kTimeFormat, s, utc, &tp)) << s;
  return convert(tp, utc);
}

// Returns `utc` ("YYYY-MM-DD hh:mm:ss" in UTC) as a local time in `tz`, by
// default as "YYYY-MM-DD hh:mm:ss +hh:mm" followed by " dst" while DST is in
// effect.
std::string Local(const time_zone& tz, const std::string& utc,
                  const std::string& fmt = "%Y-%m-%d %H:%M:%S %Ez") {
  const time_point<seconds> tp = convert(ParseCivil(utc), utc_time_zone());
  return format(fmt, tp, tz) + (tz.lookup(tp).is_dst ? " dst" : "");
}

// Returns the civil_lookup of `local` ("YYYY-MM-DD hh:mm:ss" in `tz`) as
// "<kind> <pre> / <trans> / <post>" with the time points in UTC.
std::string CivilLookup(const time_zone& tz, const std::string& local) {
  const time_zone utc = utc_time_zone();
  const time_zone::civil_lookup cl = tz.lookup(ParseCivil(local));
  const char* kind = cl.kind == time_zone::civil_lookup::UNIQUE    ? "unique"
                     : cl.kind == time_zone::civil_lookup::SKIPPED ? "skipped"
                                                                   : "repeated";
  return std::string(kind) + " " + format(kTimeFormat, cl.pre, utc) + " / " +
         format(kTimeFormat, cl.trans, utc) + " / " +
         format(kTimeFormat, cl.post, utc);
}

// Returns the proleptic TZ string of the test zone `name`, i.e. the TZif footer
// "\n<TZ string>\n" that ends the data.
std::string TzString(const std::string& name) {
  const std::unique_ptr<ZoneInfoSource> zis = CreateTestZone(name);
  if (zis == nullptr) {
    return "<no zone>";
  }
  std::string data;
  char buf[256];
  for (std::size_t n; (n = zis->Read(buf, sizeof(buf))) != 0;) {
    data.append(buf, n);
  }
  const std::size_t begin =
      data.size() < 2 ? std::string::npos : data.rfind('\n', data.size() - 2);
  if (begin == std::string::npos || data.back() != '\n') {
    return "<no footer>";
  }
  return data.substr(begin + 1, data.size() - begin - 2);
}

// ============================================================
// Proleptic TZ strings
// ============================================================

TEST(WinZoneInfoSourceTzStringTest, Format) {
  // POSIX requires two-digit minutes and seconds ("hh[:mm[:ss]]", where only
  // the hour may be a single digit); trailing zero fields are omitted.
  EXPECT_EQ("UTC0", TzString("reg:UTC"));
  EXPECT_EQ("<UTC+0530>-5:30", TzString("reg:India Standard Time"));
  EXPECT_EQ("<UTC+0030>-0:30", TzString("test:FixedHalfHourEast"));
  EXPECT_EQ("<UTC-0505>5:05", TzString("test:FixedFiveMinutesPastWest"));
  EXPECT_EQ("<UTC-05>5<UTC-04>4,M3.2.0/2,M11.1.0/2",
            TzString("reg:Eastern Standard Time"));
  EXPECT_EQ("<UTC+0930>-9:30<UTC+1030>-10:30,M10.1.0/2,M4.1.0/3",
            TzString("reg:Cen. Australia Standard Time"));
  EXPECT_EQ("<UTC+02>-2<UTC+03>-3,M3.5.4/24,M10.5.6/1",
            TzString("test:EndOfDayRule"));
}

// ============================================================
// Fixed-offset zones (no DST)
// ============================================================

TEST_F(WinZoneInfoSourceTest, FixedOffset) {
  const struct {
    const char* name;
    const char* offset;
  } kCases[] = {
      {"reg:Afghanistan Standard Time", "+04:30"},
      {"reg:Hawaiian Standard Time", "-10:00"},
      {"reg:India Standard Time", "+05:30"},
      {"reg:Nepal Standard Time", "+05:45"},
      {"reg:UTC", "+00:00"},
      {"test:FixedWithStandardBias", "+09:00"},
      {"test:FixedHalfHourEast", "+00:30"},
      {"test:FixedFiveMinutesPastWest", "-05:05"},
      {"test:FixedEastmost", "+24:59"},
      {"test:FixedWestmost", "-24:59"},
      {"test:LoneDaylightDate", "+01:00"},
      {"test:LoneStandardDate", "+01:00"},
      {"test:LoneDaylightDateStandardBias", "+01:00"},
      {"test:LoneStandardDateStandardBias", "+01:00"},
  };
  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    time_zone tz;
    ASSERT_TRUE(load_time_zone(c.name, &tz));
    // Before the first transition, in and out of the rule months, and far
    // beyond the transition table (from the proleptic TZ string).
    for (const char* utc : {"1960-01-01 00:00:00", "2024-01-15 00:00:00",
                            "2024-06-15 00:00:00", "2100-06-15 00:00:00"}) {
      EXPECT_EQ(c.offset, Local(tz, utc, "%Ez")) << utc;
    }
  }
}

// ============================================================
// DST zones with recurring (day-of-week) rules
// ============================================================

// America/New_York. 2024 comes from the proleptic TZ string:
//   Spring: 2nd Sunday of March    = Mar 10, 2024 02:00 EST
//   Fall:   1st Sunday of November = Nov  3, 2024 02:00 EDT
TEST_F(WinZoneInfoSourceTest, DST_America_NewYork) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Eastern Standard Time", &tz));

  EXPECT_EQ("2024-03-10 01:59:00 -05:00", Local(tz, "2024-03-10 06:59:00"));
  EXPECT_EQ("2024-03-10 03:00:00 -04:00 dst", Local(tz, "2024-03-10 07:00:00"));
  EXPECT_EQ("2024-11-03 01:59:00 -04:00 dst", Local(tz, "2024-11-03 05:59:00"));
  EXPECT_EQ("2024-11-03 01:00:00 -05:00", Local(tz, "2024-11-03 06:00:00"));

  EXPECT_EQ(
      "skipped 2024-03-10 07:30:00 / 2024-03-10 07:00:00 / 2024-03-10 06:30:00",
      CivilLookup(tz, "2024-03-10 02:30:00"));
  EXPECT_EQ(
      "repeated 2024-11-03 05:30:00 / 2024-11-03 06:00:00 / "
      "2024-11-03 06:30:00",
      CivilLookup(tz, "2024-11-03 01:30:00"));
}

// America/St_Johns - a non-integer-hour standard offset (UTC-3:30) with a
// 60-minute DST.
TEST_F(WinZoneInfoSourceTest, DST_America_StJohns) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Newfoundland Standard Time", &tz));

  EXPECT_EQ("2024-03-10 01:59:00 -03:30", Local(tz, "2024-03-10 05:29:00"));
  EXPECT_EQ("2024-03-10 03:00:00 -02:30 dst", Local(tz, "2024-03-10 05:30:00"));
  EXPECT_EQ("2024-11-03 01:59:00 -02:30 dst", Local(tz, "2024-11-03 04:29:00"));
  EXPECT_EQ("2024-11-03 01:00:00 -03:30", Local(tz, "2024-11-03 04:30:00"));
}

// Australia/Lord_Howe - UTC+10:30/UTC+11, a 30-minute DST in the southern
// hemisphere.
TEST_F(WinZoneInfoSourceTest, DST_Australia_LordHowe) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Lord Howe Standard Time", &tz));

  // Clocks spring 30 minutes: 02:00 LHST -> 02:30 LHDT.
  EXPECT_EQ("2024-10-06 01:59:00 +10:30", Local(tz, "2024-10-05 15:29:00"));
  EXPECT_EQ("2024-10-06 02:30:00 +11:00 dst", Local(tz, "2024-10-05 15:30:00"));
  // Clocks fall 30 minutes: 02:00 LHDT -> 01:30 LHST.
  EXPECT_EQ("2024-04-07 01:59:00 +11:00 dst", Local(tz, "2024-04-06 14:59:00"));
  EXPECT_EQ("2024-04-07 01:30:00 +10:30", Local(tz, "2024-04-06 15:00:00"));

  EXPECT_EQ(
      "repeated 2024-04-06 14:30:00 / 2024-04-06 15:00:00 / "
      "2024-04-06 15:00:00",
      CivilLookup(tz, "2024-04-07 01:30:00"));
}

// Australia/Adelaide - UTC+9:30/UTC+10:30. DST spans the year end, so Jan 1
// falls in DST. Dynamic DST 2007-2008, so years up to 2008 come from the
// transition table (not the proleptic TZ string) - a regression test that the
// builder does not force standard time at each year begin.
//   2007 (entry[0]): std = Mar/last-Sun/03:00, dst = Oct/last-Sun/02:00
//   2008 (entry[1]): std = Apr/1st-Sun/03:00,  dst = Oct/1st-Sun/02:00
TEST_F(WinZoneInfoSourceTest, DST_Australia_Adelaide_YearBoundary) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Cen. Australia Standard Time", &tz));

  // Extrapolated year (before first_year): mid-January is DST, but only from
  // 1970, where the transition table begins.
  EXPECT_EQ("1969-01-15 00:00:00 +09:30", Local(tz, "1969-01-14 14:30:00"));
  EXPECT_EQ("1971-01-15 00:00:00 +10:30 dst", Local(tz, "1971-01-14 13:30:00"));

  EXPECT_EQ("2006-12-31 23:59:00 +10:30 dst", Local(tz, "2006-12-31 13:29:00"));
  EXPECT_EQ("2007-01-01 00:00:00 +10:30 dst", Local(tz, "2006-12-31 13:30:00"));
  EXPECT_EQ("2007-03-25 02:59:00 +10:30 dst", Local(tz, "2007-03-24 16:29:00"));
  EXPECT_EQ("2007-03-25 02:00:00 +09:30", Local(tz, "2007-03-24 16:30:00"));

  EXPECT_EQ("2007-12-31 23:59:00 +10:30 dst", Local(tz, "2007-12-31 13:29:00"));
  EXPECT_EQ("2008-01-01 00:00:00 +10:30 dst", Local(tz, "2007-12-31 13:30:00"));
  EXPECT_EQ("2008-04-06 02:59:00 +10:30 dst", Local(tz, "2008-04-05 16:29:00"));
  EXPECT_EQ("2008-04-06 02:00:00 +09:30", Local(tz, "2008-04-05 16:30:00"));
  EXPECT_EQ("2008-10-05 01:59:00 +09:30", Local(tz, "2008-10-04 16:29:00"));
  EXPECT_EQ("2008-10-05 03:00:00 +10:30 dst", Local(tz, "2008-10-04 16:30:00"));
}

TEST_F(WinZoneInfoSourceTest, DST_EndOfDayRule_ProlepticYear) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("test:EndOfDayRule", &tz));

  // Clocks spring 00:00 -> 01:00 on Apr 1.
  EXPECT_EQ("2022-03-31 23:59:00 +02:00", Local(tz, "2022-03-31 21:59:00"));
  EXPECT_EQ("2022-04-01 01:00:00 +03:00 dst", Local(tz, "2022-03-31 22:00:00"));
  // Fall-back: Oct 29, 2022 01:00 local DST.
  EXPECT_EQ("2022-10-29 00:59:00 +03:00 dst", Local(tz, "2022-10-28 21:59:00"));
  EXPECT_EQ("2022-10-29 00:00:00 +02:00", Local(tz, "2022-10-28 22:00:00"));
}

// ============================================================
// Dynamic DST - year-boundary transitions
// ============================================================

// Europe/Volgograd: the 2020 entry begins in DST (UTC+5) and the 2021 entry
// moves the standard offset from UTC+4 to UTC+3 at the year begin.
// https://github.com/dotnet/runtime/issues/118915
TEST_F(WinZoneInfoSourceTest, DynamicDST_Europe_Volgograd) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Volgograd Standard Time", &tz));

  EXPECT_EQ("2019-12-31 23:30:00 +04:00", Local(tz, "2019-12-31 19:30:00"));
  EXPECT_EQ("2020-01-01 01:00:00 +05:00 dst", Local(tz, "2019-12-31 20:00:00"));
  EXPECT_EQ("2020-12-31 23:30:00 +04:00", Local(tz, "2020-12-31 19:30:00"));
  EXPECT_EQ("2020-12-31 23:00:00 +03:00", Local(tz, "2020-12-31 20:00:00"));
  EXPECT_EQ("2021-01-01 00:00:00 +03:00", Local(tz, "2020-12-31 21:00:00"));
}

// Asia/Yakutsk - multiple offset changes driven by Russian DST policy.
//   2010-2011: UTC+9 std / UTC+10 DST.
//   2012-2013: UTC+10 permanent ("summer time" relabeled as standard at the
//              Jan 1, 2012 year begin; same offset).
//   2014:      Begins in DST (UTC+10) again, then UTC+9 standard from Oct 26.
//   2015:      UTC+9 permanent, which is also the proleptic TZ string.
TEST_F(WinZoneInfoSourceTest, DynamicDST_Asia_Yakutsk) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Yakutsk Standard Time", &tz));

  EXPECT_EQ("2011-12-31 23:59:00 +10:00 dst", Local(tz, "2011-12-31 13:59:00"));
  EXPECT_EQ("2012-01-01 00:00:00 +10:00", Local(tz, "2011-12-31 14:00:00"));

  EXPECT_EQ("2013-12-31 23:59:00 +10:00", Local(tz, "2013-12-31 13:59:00"));
  EXPECT_EQ("2014-01-01 00:00:00 +10:00 dst", Local(tz, "2013-12-31 14:00:00"));

  EXPECT_EQ("2014-10-26 01:59:00 +10:00 dst", Local(tz, "2014-10-25 15:59:00"));
  EXPECT_EQ("2014-10-26 01:00:00 +09:00", Local(tz, "2014-10-25 16:00:00"));

  EXPECT_EQ("2024-01-01 09:00:00 +09:00", Local(tz, "2024-01-01 00:00:00"));
}

// America/Edmonton (Alberta) - UTC-7/UTC-6, then UTC-6 permanent.
//   2007-2025: Mar/2nd-Sun and Nov/1st-Sun.
//   2026:      DST from Mar 8 with no November fall-back.
//   2027:      UTC-6 permanent, which is also the proleptic TZ string. The
//              is_dst label changes at the Jan 1, 2027 year begin.
TEST_F(WinZoneInfoSourceTest, DynamicDST_America_Edmonton) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("reg:Alberta Standard Time", &tz));

  EXPECT_EQ("2026-03-08 01:59:00 -07:00", Local(tz, "2026-03-08 08:59:00"));
  EXPECT_EQ("2026-03-08 03:00:00 -06:00 dst", Local(tz, "2026-03-08 09:00:00"));

  // No fall-back on Nov 1, 2026, so 01:30 local is not repeated.
  EXPECT_EQ("2026-11-01 02:00:00 -06:00 dst", Local(tz, "2026-11-01 08:00:00"));
  EXPECT_EQ(
      "unique 2026-11-01 07:30:00 / 2026-11-01 07:30:00 / 2026-11-01 07:30:00",
      CivilLookup(tz, "2026-11-01 01:30:00"));

  EXPECT_EQ("2026-12-31 23:59:00 -06:00 dst", Local(tz, "2027-01-01 05:59:00"));
  EXPECT_EQ("2027-01-01 00:00:00 -06:00", Local(tz, "2027-01-01 06:00:00"));
  EXPECT_EQ("2028-07-15 00:00:00 -06:00", Local(tz, "2028-07-15 06:00:00"));
  EXPECT_EQ("2035-01-15 00:00:00 -06:00", Local(tz, "2035-01-15 06:00:00"));
}

TEST_F(WinZoneInfoSourceTest, DynamicDST_AbsoluteDateEndOfDayRule) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("test:AbsoluteEndOfDayRule", &tz));

  EXPECT_EQ("2020-06-15 23:59:59 +00:00", Local(tz, "2020-06-15 23:59:59"));
  EXPECT_EQ("2020-06-16 01:00:00 +01:00 dst", Local(tz, "2020-06-16 00:00:00"));
  EXPECT_EQ("2020-10-15 01:59:59 +01:00 dst", Local(tz, "2020-10-15 00:59:59"));
  EXPECT_EQ("2020-10-15 01:00:00 +00:00", Local(tz, "2020-10-15 01:00:00"));

  // Absolute-dated rules fire only in their own year: no DST in 2021.
  EXPECT_EQ("2021-06-16 00:00:00 +00:00", Local(tz, "2021-06-16 00:00:00"));
}

// Identical "Dynamic DST" values collapse into one rule, which applies to all
// years from 1970, not just from FirstEntry.
TEST_F(WinZoneInfoSourceTest, DynamicDST_IdenticalYears) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("test:IdenticalYears", &tz));

  EXPECT_EQ("1969-07-01 07:00:00 -05:00", Local(tz, "1969-07-01 12:00:00"));
  EXPECT_EQ("1975-07-01 08:00:00 -04:00 dst", Local(tz, "1975-07-01 12:00:00"));
  EXPECT_EQ("2011-07-01 08:00:00 -04:00 dst", Local(tz, "2011-07-01 12:00:00"));
  EXPECT_EQ("2024-03-10 03:00:00 -04:00 dst", Local(tz, "2024-03-10 07:00:00"));
  EXPECT_EQ("2024-11-03 01:00:00 -05:00", Local(tz, "2024-11-03 06:00:00"));
}

// ============================================================
// Decoding of "TZI" registry values
// ============================================================

// The "TZI" values of "Eastern Standard Time" and "Tokyo Standard Time" as
// stored in the registry. They anchor Tzi(), which builds the inputs of all
// the other tests, to the real layout.
TEST(Tzi, MatchesRegistryValues) {
  const WinRegTzi kEasternTzi = {{
      0x2c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc4, 0xff, 0xff,
      0xff, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
      0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  }};
  const WinRegTzi kTokyoTzi = {{
      0xe4, 0xfd, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xc4, 0xff, 0xff,
      0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  }};
  EXPECT_EQ(kEasternTzi,
            Tzi(300, 0, -60, {0, 11, 0, 1, 2, 0, 0, 0}, kMarch2ndSunday));
  EXPECT_EQ(kTokyoTzi, Tzi(-540, 0, -60, kNoDate, kNoDate));
}

// Whether `tzi` is accepted as the "Dynamic DST" value of 2020, followed by a
// fixed-offset 2021. (The last year is also subject to the proleptic TZ string
// restriction tested in AbsoluteDateInLastYearRejectsZone.)
bool IsAccepted(const WinRegTzi& tzi) {
  return CreateWinZoneInfoSource(
             2020, {tzi, Tzi(-60, 0, -60, kNoDate, kNoDate)}) != nullptr;
}

// The UTC offsets in effect must be within the range RFC 9636 allows, which is
// -25h < utoff < 26h, i.e. -1560 < -bias < 1500 in minutes.
TEST(CreateWinZoneInfoSource, BiasRange) {
  const std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
  const std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
  const struct {
    std::int32_t bias;
    std::int32_t standard_bias;
    std::int32_t daylight_bias;
    bool has_dst;
    bool valid;
  } kCases[] = {
      {1499, 0, 0, false, true},
      {1500, 0, 0, false, false},
      {-1559, 0, 0, false, true},
      {-1560, 0, 0, false, false},
      {kMin, 0, 0, false, false},
      {kMax, 0, 0, false, false},
      // Without DST, StandardBias and DaylightBias are not in effect.
      {0, kMin, kMax, false, true},
      {0, kMax, kMin, false, true},
      // With DST, the offsets are -(Bias + StandardBias) and
      // -(Bias + DaylightBias).
      {0, 1499, 0, true, true},
      {0, 1500, 0, true, false},
      {0, 0, -1559, true, true},
      {0, 0, -1560, true, false},
      {3000, -1501, -1501, true, true},
      {3000, -1500, -1501, true, false},
      {3000, -1501, -1500, true, false},
      {kMin, kMin, 0, true, false},
      {kMax, 0, kMax, true, false},
  };
  for (const auto& c : kCases) {
    SCOPED_TRACE(testing::Message() << c.bias << " " << c.standard_bias << " "
                                    << c.daylight_bias << " " << c.has_dst);
    EXPECT_EQ(c.valid, IsAccepted(Tzi(c.bias, c.standard_bias, c.daylight_bias,
                                      c.has_dst ? kNovember1stSunday : kNoDate,
                                      c.has_dst ? kMarch2ndSunday : kNoDate)));
  }
}

// The last value becomes the proleptic TZ string, whose offsets must be less
// than 25h in magnitude.
TEST(CreateWinZoneInfoSource, BiasRangeInLastYear) {
  EXPECT_NE(nullptr,
            CreateWinZoneInfoSource(Tzi(-1499, 0, 0, kNoDate, kNoDate)));
  EXPECT_EQ(nullptr,
            CreateWinZoneInfoSource(Tzi(-1500, 0, 0, kNoDate, kNoDate)));
  EXPECT_NE(nullptr,
            CreateWinZoneInfoSource(Tzi(1499, 0, 0, kNoDate, kNoDate)));
  EXPECT_NE(nullptr, CreateWinZoneInfoSource(Tzi(
                         0, 0, -1499, kNovember1stSunday, kMarch2ndSunday)));
  EXPECT_EQ(nullptr, CreateWinZoneInfoSource(Tzi(
                         0, 0, -1500, kNovember1stSunday, kMarch2ndSunday)));
  // Not the last value, so only the TZif range applies.
  EXPECT_NE(nullptr, CreateWinZoneInfoSource(
                         2020, {Tzi(-1500, 0, 0, kNoDate, kNoDate),
                                Tzi(-60, 0, -60, kNoDate, kNoDate)}));
}

// The transitions begin in 1970, so a zone whose "Dynamic DST" years start far
// in the future can need more than the TZ_MAX_TIMES transitions that
// TimeZoneInfo::Load() accepts.
TEST_F(WinZoneInfoSourceTest, MaxTransitions) {
  if (!CanTestMaxTransitions()) {
    GTEST_SKIP() << "No boundary year for TZ_MAX_TIMES = " << TZ_MAX_TIMES;
  }
  time_zone tz;
  ASSERT_TRUE(load_time_zone("test:MaxTransitions", &tz));
  const std::string last_dst_year = std::to_string(kMaxTransitionsLastDstYear);
  const std::string next_year = std::to_string(kMaxTransitionsLastDstYear + 1);
  EXPECT_EQ(last_dst_year + "-07-01 08:00:00 -04:00 dst",
            Local(tz, last_dst_year + "-07-01 12:00:00"));
  EXPECT_EQ(next_year + "-07-01 06:00:00 -06:00",
            Local(tz, next_year + "-07-01 12:00:00"));
}

TEST(CreateWinZoneInfoSource, TooManyTransitionsRejectsZone) {
  if (!CanTestMaxTransitions()) {
    GTEST_SKIP() << "No boundary year for TZ_MAX_TIMES = " << TZ_MAX_TIMES;
  }
  // One more year of DST than test:MaxTransitions, and as many as possible.
  EXPECT_EQ(nullptr, DstThrough(kMaxTransitionsLastDstYear + 1));
  EXPECT_EQ(nullptr, DstThrough(kMaxSystemTimeYear - 1));
  // Without DST in the first year, the years before it need no transitions.
  EXPECT_NE(nullptr,
            CreateWinZoneInfoSource(
                kMaxSystemTimeYear - 1,
                {Tzi(360, 0, 0, kNoDate, kNoDate),
                 Tzi(300, 0, -60, kNovember1stSunday, kMarch2ndSunday)}));
}

TEST(CreateWinZoneInfoSource, TransitionDateRange) {
  const struct {
    const char* name;
    std::uint16_t date[8];
    bool valid;
  } kCases[] = {
      {"no transition", {0, 0, 0, 0, 0, 0, 0, 0}, true},
      {"no transition with a stray field", {0, 0, 0, 1, 0, 0, 0, 0}, false},
      {"recurring", {0, 3, 0, 2, 2, 0, 0, 0}, true},
      {"recurring end of day", {0, 3, 6, 5, 23, 59, 59, 999}, true},
      {"recurring month 13", {0, 13, 0, 1, 2, 0, 0, 0}, false},
      {"recurring week 0", {0, 3, 0, 0, 2, 0, 0, 0}, false},
      {"recurring week 6", {0, 3, 0, 6, 2, 0, 0, 0}, false},
      {"recurring weekday 7", {0, 3, 7, 1, 2, 0, 0, 0}, false},
      {"absolute first year", {1601, 1, 0, 1, 0, 0, 0, 0}, true},
      {"absolute last year", {30827, 12, 0, 31, 0, 0, 0, 0}, true},
      {"absolute year 1600", {1600, 1, 0, 1, 0, 0, 0, 0}, false},
      {"absolute year 30828", {30828, 1, 0, 1, 0, 0, 0, 0}, false},
      {"absolute without month", {2020, 0, 0, 1, 0, 0, 0, 0}, false},
      {"absolute day 0", {2020, 3, 0, 0, 0, 0, 0, 0}, false},
      {"absolute day 32", {2020, 3, 0, 32, 0, 0, 0, 0}, false},
      {"absolute weekday 7", {2020, 3, 7, 1, 0, 0, 0, 0}, false},
      {"hour 24", {0, 3, 0, 2, 24, 0, 0, 0}, false},
      {"minute 60", {0, 3, 0, 2, 2, 60, 0, 0}, false},
      {"second 60", {0, 3, 0, 2, 2, 0, 60, 0}, false},
      {"milliseconds 1000", {0, 3, 0, 2, 2, 0, 0, 1000}, false},
  };
  for (const auto& c : kCases) {
    SCOPED_TRACE(c.name);
    EXPECT_EQ(c.valid, IsAccepted(Tzi(-60, 0, -60, c.date,  // StandardDate
                                      kMarch2ndSunday)));
    EXPECT_EQ(c.valid, IsAccepted(Tzi(-60, 0, -60, kMarch2ndSunday,
                                      c.date)));  // DaylightDate
  }
}

// A single invalid "Dynamic DST" year rejects the whole zone.
TEST(CreateWinZoneInfoSource, InvalidYearRejectsZone) {
  const std::uint16_t kInvalidDate[8] = {0, 3, 7, 2, 2, 0, 0, 0};
  EXPECT_EQ(nullptr, CreateWinZoneInfoSource(
                         2020, {Tzi(-60, 0, -60, kNoDate, kNoDate),
                                Tzi(-60, 0, -60, kInvalidDate, kMarch2ndSunday),
                                Tzi(-60, 0, -60, kNoDate, kNoDate)}));
}

// The last rule becomes the proleptic TZ string, which cannot express absolute
// dates. (Before the last year, absolute dates fire only in their own year.)
TEST(CreateWinZoneInfoSource, AbsoluteDateInLastYearRejectsZone) {
  const std::uint16_t kAbsoluteDate[8] = {2021, 10, 0, 15, 2, 0, 0, 0};
  EXPECT_EQ(nullptr, CreateWinZoneInfoSource(
                         Tzi(-60, 0, -60, kAbsoluteDate, kMarch2ndSunday)));
  EXPECT_EQ(nullptr,
            CreateWinZoneInfoSource(
                2020, {Tzi(-60, 0, -60, kNoDate, kNoDate),
                       Tzi(-60, 0, -60, kMarch2ndSunday, kAbsoluteDate)}));
  // A lone absolute date means no DST, so there is nothing to express.
  EXPECT_NE(nullptr,
            CreateWinZoneInfoSource(Tzi(-60, 0, -60, kAbsoluteDate, kNoDate)));
}

// "Dynamic DST" years must be nonempty and within SYSTEMTIME's range.
TEST(CreateWinZoneInfoSource, DynamicDstYearRange) {
  const WinRegTzi kTzi = Tzi(-60, 0, -60, kNoDate, kNoDate);
  EXPECT_EQ(nullptr, CreateWinZoneInfoSource(2020, {}));
  EXPECT_EQ(nullptr, CreateWinZoneInfoSource(1600, {kTzi}));
  EXPECT_NE(nullptr, CreateWinZoneInfoSource(1601, {kTzi}));
  EXPECT_NE(nullptr, CreateWinZoneInfoSource(30826, {kTzi, kTzi}));
  EXPECT_EQ(nullptr, CreateWinZoneInfoSource(30827, {kTzi, kTzi}));
}

}  // namespace
}  // namespace cctz
