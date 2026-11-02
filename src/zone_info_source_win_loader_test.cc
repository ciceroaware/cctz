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

#include "zone_info_source_win_loader.h"

#include <functional>
#include <memory>
#include <string>

#include "cctz/civil_time.h"
#include "cctz/time_zone.h"
#include "cctz/zone_info_source.h"
#include "gtest/gtest.h"
#include "time_zone_name_win.h"

namespace cctz {
namespace {

// Copied from time_zone_lookup_test.cc. This helper is a macro so that failed
// expectations show up with the correct line numbers.
#define ExpectTime(tp, tz, y, m, d, hh, mm, ss, off, isdst, zone) \
  do {                                                            \
    time_zone::absolute_lookup al = tz.lookup(tp);                \
    EXPECT_EQ(y, al.cs.year());                                   \
    EXPECT_EQ(m, al.cs.month());                                  \
    EXPECT_EQ(d, al.cs.day());                                    \
    EXPECT_EQ(hh, al.cs.hour());                                  \
    EXPECT_EQ(mm, al.cs.minute());                                \
    EXPECT_EQ(ss, al.cs.second());                                \
    EXPECT_EQ(off, al.offset);                                    \
    EXPECT_TRUE(isdst == al.is_dst);                              \
    EXPECT_STREQ(zone, al.abbr);                                  \
  } while (0)

// Serves "win:<IANA name>" from LoadWinZoneInfoSource(), regardless of the
// zoneinfo files and CCTZ_USE_WIN_REGISTRY_FALLBACK.
std::unique_ptr<ZoneInfoSource> WinZoneInfoSourceFactory(
    const std::string& name,
    const std::function<std::unique_ptr<ZoneInfoSource>(const std::string&)>&
        fallback) {
  if (name.compare(0, 4, "win:") == 0) {
    return LoadWinZoneInfoSource(name.substr(4));
  }
  return fallback(name);
}

class ZoneInfoSourceWinLoaderTest : public ::testing::Test {
 protected:
  ZoneInfoSourceWinLoaderTest() {
    saved_factory = cctz_extension::zone_info_source_factory;
    cctz_extension::zone_info_source_factory = WinZoneInfoSourceFactory;
  }

  ~ZoneInfoSourceWinLoaderTest() {
    cctz_extension::zone_info_source_factory = saved_factory;
  }

  void SetUp() override {
    // LoadWinZoneInfoSource() maps IANA names to registry keys with
    // ConvertToWindowsTimeZoneId(), which needs "icu.dll" (Windows 10 1809+).
    if (ConvertToWindowsTimeZoneId("America/New_York").empty()) {
      GTEST_SKIP() << "ConvertToWindowsTimeZoneId() is not available";
    }
  }

 private:
  cctz_extension::ZoneInfoSourceFactory saved_factory;
};

// Adapted from TimeZoneEdgeCase.AmericaNewYork in time_zone_lookup_test.cc,
// with abbreviations derived from the offsets. "Eastern Standard Time" has a
// "Dynamic DST" key for 2006-2007, so 2006 comes from the transition table and
// 2013 from the proleptic TZ string.
TEST_F(ZoneInfoSourceWinLoaderTest, AmericaNewYork) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("win:America/New_York", &tz));

  // Spring 1:59:59 -> 3:00:00
  auto tp = convert(civil_second(2006, 4, 2, 1, 59, 59), tz);
  ExpectTime(tp, tz, 2006, 4, 2, 1, 59, 59, -5 * 3600, false, "UTC-05");
  tp += seconds(1);
  ExpectTime(tp, tz, 2006, 4, 2, 3, 0, 0, -4 * 3600, true, "UTC-04");

  // Fall 1:59:59 -> 1:00:00
  tp = convert(civil_second(2006, 10, 29, 1, 59, 59), tz);
  ExpectTime(tp, tz, 2006, 10, 29, 1, 59, 59, -4 * 3600, true, "UTC-04");
  tp += seconds(1);
  ExpectTime(tp, tz, 2006, 10, 29, 1, 0, 0, -5 * 3600, false, "UTC-05");

  // Spring 1:59:59 -> 3:00:00
  tp = convert(civil_second(2013, 3, 10, 1, 59, 59), tz);
  ExpectTime(tp, tz, 2013, 3, 10, 1, 59, 59, -5 * 3600, false, "UTC-05");
  tp += seconds(1);
  ExpectTime(tp, tz, 2013, 3, 10, 3, 0, 0, -4 * 3600, true, "UTC-04");

  // Fall 1:59:59 -> 1:00:00
  tp = convert(civil_second(2013, 11, 3, 1, 59, 59), tz);
  ExpectTime(tp, tz, 2013, 11, 3, 1, 59, 59, -4 * 3600, true, "UTC-04");
  tp += seconds(1);
  ExpectTime(tp, tz, 2013, 11, 3, 1, 0, 0, -5 * 3600, false, "UTC-05");
}

// "GMT Standard Time" has no "Dynamic DST" key, so its base "TZI" value applies
// to all years.
TEST_F(ZoneInfoSourceWinLoaderTest, EuropeLondon) {
  time_zone tz;
  ASSERT_TRUE(load_time_zone("win:Europe/London", &tz));

  // Spring 0:59:59 -> 2:00:00
  auto tp = convert(civil_second(2013, 3, 31, 0, 59, 59), tz);
  ExpectTime(tp, tz, 2013, 3, 31, 0, 59, 59, 0, false, "UTC");
  tp += seconds(1);
  ExpectTime(tp, tz, 2013, 3, 31, 2, 0, 0, 1 * 3600, true, "UTC+01");

  // Fall 1:59:59 -> 1:00:00
  tp = convert(civil_second(2013, 10, 27, 1, 59, 59), tz);
  ExpectTime(tp, tz, 2013, 10, 27, 1, 59, 59, 1 * 3600, true, "UTC+01");
  tp += seconds(1);
  ExpectTime(tp, tz, 2013, 10, 27, 1, 0, 0, 0, false, "UTC");
}

}  // namespace
}  // namespace cctz
