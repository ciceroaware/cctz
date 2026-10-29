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

#include "time_zone_name_win.h"

#include <string>

#include <windows.h>

#include "cctz/time_zone.h"
#include "gtest/gtest.h"

namespace cctz {
namespace {

// Returns whether "icu.dll" is available in System32 (Windows 10 1809+).
// Call this only after the function under test, so that the DLL this loads
// cannot affect it. Note that LOAD_LIBRARY_SEARCH_SYSTEM32 is not sufficient to
// reliably load "icu.dll" in the production code, but it should be OK for
// testing purposes.
bool IsIcuDllAvailable() {
  HMODULE icu_dll =
      ::LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (icu_dll == nullptr) {
    return false;
  }
  ::FreeLibrary(icu_dll);
  return true;
}

}  // namespace

TEST(TimeZoneNameWin, GetWindowsLocalTimeZone) {
  // GetWindowsLocalTimeZone() should return a valid IANA time zone name where
  // "icu.dll" is available.
  const std::string tz = GetWindowsLocalTimeZone();
  if (IsIcuDllAvailable()) {
    EXPECT_FALSE(tz.empty());
  } else {
    EXPECT_TRUE(tz.empty());
  }
}

TEST(TimeZoneNameWin, ConvertToWindowsTimeZoneId) {
  const std::wstring id = ConvertToWindowsTimeZoneId("America/New_York");
  if (IsIcuDllAvailable()) {
    EXPECT_FALSE(id.empty());
  } else {
    EXPECT_TRUE(id.empty());
  }
}

} // namespace cctz
