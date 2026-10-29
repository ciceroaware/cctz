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

#if !defined(NOMINMAX)
#define NOMINMAX
#endif  // !defined(NOMINMAX)
#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "time_zone_name_win.h"
#include "zone_info_source_win.h"

namespace cctz {
namespace {

const wchar_t kRegistryPath[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";

using ScopedHKey =
    std::unique_ptr<std::remove_pointer<HKEY>::type, decltype(&::RegCloseKey)>;

ScopedHKey OpenRegistryKey(HKEY root, const wchar_t* sub_key) {
  HKEY hkey = nullptr;
  if (::RegOpenKeyExW(root, sub_key, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
    return ScopedHKey(nullptr, nullptr);
  }
  return ScopedHKey(hkey, ::RegCloseKey);
}

bool ReadDword(HKEY key, const wchar_t* value_name, DWORD& value) {
  DWORD size = sizeof(DWORD);
  DWORD temp_value;
  LSTATUS reg_result =
      ::RegGetValueW(key, nullptr, value_name, RRF_RT_REG_DWORD, nullptr,
                     reinterpret_cast<LPBYTE>(&temp_value), &size);
  if (reg_result != ERROR_SUCCESS || size != sizeof(DWORD)) {
    return false;
  }
  value = temp_value;
  return true;
}

bool ReadTimeZoneInfo(HKEY key, const wchar_t* value_name, WinRegTzi& tzi) {
  static_assert(sizeof(LONG) * 3 + sizeof(SYSTEMTIME) * 2 == kRegTziFormatSize,
                "kRegTziFormatSize must match REG_TZI_FORMAT");
  DWORD size = static_cast<DWORD>(tzi.size());
  return ::RegGetValueW(key, nullptr, value_name, RRF_RT_REG_BINARY, nullptr,
                        tzi.data(), &size) == ERROR_SUCCESS &&
         size == tzi.size();
}

}  // namespace

std::unique_ptr<ZoneInfoSource> LoadWinZoneInfoSource(const std::string& name) {
  const std::wstring key_name = ConvertToWindowsTimeZoneId(name);
  if (key_name.empty()) {
    return nullptr;
  }

  const std::wstring reg_tz_path =
      std::wstring(kRegistryPath) + L"\\" + key_name;

  ScopedHKey hkey_timezone =
      OpenRegistryKey(HKEY_LOCAL_MACHINE, reg_tz_path.c_str());
  if (!hkey_timezone) {
    return nullptr;
  }

  ScopedHKey hkey_dynamic_years =
      OpenRegistryKey(hkey_timezone.get(), L"Dynamic DST");
  if (!hkey_dynamic_years) {
    WinRegTzi tzi;
    if (!ReadTimeZoneInfo(hkey_timezone.get(), L"TZI", tzi)) {
      return nullptr;
    }
    return CreateWinZoneInfoSource(tzi);
  }

  // Windows applies the FirstEntry/LastEntry rules to all earlier/later years,
  // so the base TZI value, which holds only the current year's rule (e.g., a
  // 2026 DST start for a zone that adopts permanent DST in 2027), must not be
  // used.
  DWORD first_year = 0;
  DWORD last_year = 0;
  if (!ReadDword(hkey_dynamic_years.get(), L"FirstEntry", first_year)) {
    return nullptr;
  }
  if (!ReadDword(hkey_dynamic_years.get(), L"LastEntry", last_year)) {
    return nullptr;
  }
  if (first_year > last_year) {
    return nullptr;
  }
  // CreateWinZoneInfoSource() also requires this, but checking it here keeps
  // corrupt or malicious registry data (e.g. FirstEntry == LastEntry ==
  // 4000000000) from making this loop read an absurd number of values.
  if (first_year < kMinSystemTimeYear || last_year > kMaxSystemTimeYear) {
    return nullptr;
  }

  std::vector<WinRegTzi> years;
  years.reserve(static_cast<std::size_t>(last_year - first_year + 1));
  for (DWORD year = first_year; year <= last_year; ++year) {
    WinRegTzi tzi;
    if (!ReadTimeZoneInfo(hkey_dynamic_years.get(),
                          std::to_wstring(year).c_str(), tzi)) {
      return nullptr;
    }
    years.push_back(tzi);
  }
  return CreateWinZoneInfoSource(first_year, years);
}

}  // namespace cctz
