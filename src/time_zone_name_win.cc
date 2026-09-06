// Copyright 2025 Google Inc. All Rights Reserved.
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

#if !defined(NOMINMAX)
#define NOMINMAX
#endif  // !defined(NOMINMAX)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace cctz {
namespace {

// Define UChar as wchar_t here because Win32 APIs receive UTF-16 strings as
// wchar_t* instead of char16_t*. Using char16_t would require additional casts.
using UChar = wchar_t;

enum UErrorCode : std::int32_t {
  U_ZERO_ERROR = 0,
  U_BUFFER_OVERFLOW_ERROR = 15,
};

bool U_SUCCESS(UErrorCode error) { return error <= U_ZERO_ERROR; }

using ucal_getTimeZoneIDForWindowsID_func = std::int32_t(__cdecl*)(
    const UChar* winid, std::int32_t len, const char* region, UChar* id,
    std::int32_t id_capacity, UErrorCode* status);
using ucal_getWindowsTimeZoneID_func =
    std::int32_t(__cdecl*)(const UChar* id, std::int32_t len, UChar* winid,
                           std::int32_t winid_capacity, UErrorCode* status);

template <typename T> static T AsProcAddress(HMODULE module, const char* name) {
  static_assert(
      std::is_pointer<T>::value &&
          std::is_function<typename std::remove_pointer<T>::type>::value,
      "T must be a function pointer type");
  const auto proc_address = ::GetProcAddress(module, name);
  return reinterpret_cast<T>(reinterpret_cast<void*>(proc_address));
}

std::wstring GetSystem32Dir() {
  std::wstring result;
  std::uint32_t len = std::max<std::uint32_t>(
      static_cast<std::uint32_t>(std::min<size_t>(
          result.capacity(), std::numeric_limits<std::uint32_t>::max())),
      1);
  do {
    result.resize(len);
    len = ::GetSystemDirectoryW(&result[0], len);
  } while (len > result.size());
  result.resize(len);
  return result;
}

struct IcuDll {
  // Return the module handle of "icu.dll" in the System32 directory, or nullptr
  // if it is not available.
  //
  // This method is intended to be lock free to avoid potential deadlocks with
  // loader-lock taken inside LoadLibraryW. As LoadLibraryW and GetProcAddress
  // are idempotent unless the DLL is unloaded, we just need to make sure the
  // members are read/written atomically, where memory_order_relaxed is also
  // acceptable.
  HMODULE GetOrLoad() {
    if (unavailable.load(std::memory_order_relaxed)) {
      return nullptr;
    }

    {
      const HMODULE cached = dll.load(std::memory_order_relaxed);
      if (cached != nullptr) {
        return cached;
      }
    }

    const std::wstring system32_dir = GetSystem32Dir();
    if (system32_dir.empty()) {
      unavailable.store(true, std::memory_order_relaxed);
      return nullptr;
    }

    // Here LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
    // does return the expected handle if "icu.dll" is already loaded from
    // somewhere other than the system32 directory. Specifying the full path
    // with LoadLibraryW ensures that the handle is from the system32 directory.
    const std::wstring icu_dll_path = system32_dir + L"\\icu.dll";
    const HMODULE icu_dll = ::LoadLibraryW(icu_dll_path.c_str());
    if (icu_dll == nullptr) {
      unavailable.store(true, std::memory_order_relaxed);
      return nullptr;
    }

    dll.store(icu_dll, std::memory_order_relaxed);
    return icu_dll;
  }

  std::atomic<bool> unavailable;
  std::atomic<HMODULE> dll;
};

IcuDll g_icu_dll;

template <typename T>
struct IcuFunction {
  // Returns the entry point for the given function name, or nullptr if it is
  // not available; the result, successful or not, is cached.
  //
  // This method is intended to be lock free to avoid potential deadlocks with
  // loader-lock taken inside LoadLibraryW and GetProcAddress.
  T GetOrLoad(const char* name) {
    if (unavailable.load(std::memory_order_relaxed)) {
      return nullptr;
    }

    {
      const T cached = func.load(std::memory_order_relaxed);
      if (cached != nullptr) {
        return cached;
      }
    }

    const HMODULE icu_dll = g_icu_dll.GetOrLoad();
    if (icu_dll == nullptr) {
      unavailable.store(true, std::memory_order_relaxed);
      return nullptr;
    }

    const T resolved = AsProcAddress<T>(icu_dll, name);
    if (resolved == nullptr) {
      unavailable.store(true, std::memory_order_relaxed);
      return nullptr;
    }
    func.store(resolved, std::memory_order_relaxed);
    return resolved;
  }

  std::atomic<bool> unavailable;
  std::atomic<T> func;
};

IcuFunction<ucal_getTimeZoneIDForWindowsID_func>
    g_ucal_getTimeZoneIDForWindowsID;
IcuFunction<ucal_getWindowsTimeZoneID_func> g_ucal_getWindowsTimeZoneID;

ucal_getTimeZoneIDForWindowsID_func LoadIcuGetTimeZoneIDForWindowsID() {
  return g_ucal_getTimeZoneIDForWindowsID.GetOrLoad(
      "ucal_getTimeZoneIDForWindowsID");
}

ucal_getWindowsTimeZoneID_func LoadIcuGetWindowsTimeZoneID() {
  return g_ucal_getWindowsTimeZoneID.GetOrLoad("ucal_getWindowsTimeZoneID");
}

// Convert wchar_t array (UTF-16) to UTF-8 string
std::string Utf16ToUtf8(const wchar_t* ptr, size_t size) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return std::string();
  }
  const int chars_len = static_cast<int>(size);
  std::string result;
  std::size_t len = std::max<std::size_t>(
      std::min<size_t>(result.capacity(), std::numeric_limits<int>::max()), 1);
  do {
    result.resize(len);
    len = static_cast<std::size_t>(::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, ptr, chars_len, &result[0],
        static_cast<int>(len), nullptr, nullptr));
  } while (len > result.size());
  result.resize(len);
  return result;
}

}  // namespace

std::string GetWindowsLocalTimeZone() {
  const auto getTimeZoneIDForWindowsID = LoadIcuGetTimeZoneIDForWindowsID();
  if (getTimeZoneIDForWindowsID == nullptr) {
    return std::string();
  }

  DYNAMIC_TIME_ZONE_INFORMATION info = {};
  if (::GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) {
    return std::string();
  }

  std::wstring result;
  std::size_t len = std::max<std::size_t>(
      std::min<size_t>(result.capacity(), std::numeric_limits<int>::max()), 1);
  for (;;) {
    UErrorCode status = U_ZERO_ERROR;
    result.resize(len);
    len = static_cast<std::size_t>(
        getTimeZoneIDForWindowsID(info.TimeZoneKeyName, -1, nullptr, &result[0],
                                  static_cast<int>(len), &status));
    if (U_SUCCESS(status)) {
      return Utf16ToUtf8(result.data(), len);
    }
    if (status != U_BUFFER_OVERFLOW_ERROR) {
      return std::string();
    }
  }
}

std::wstring ConvertToWindowsTimeZoneId(const std::wstring& iana_name) {
  const auto getWindowsTimeZoneID = LoadIcuGetWindowsTimeZoneID();
  if (getWindowsTimeZoneID == nullptr) {
    return std::wstring();
  }
  if (iana_name.size() > std::numeric_limits<std::int32_t>::max()) {
    return std::wstring();
  }
  const std::int32_t iana_name_length =
      static_cast<std::int32_t>(iana_name.size());

  std::wstring result;
  // Windows time zone IDs fit in DYNAMIC_TIME_ZONE_INFORMATION's 128-char
  // TimeZoneKeyName, so the first call virtually always succeeds.  Seeding
  // from result.capacity() (the SSO capacity, 7 chars on MSVC) would instead
  // guarantee a U_BUFFER_OVERFLOW_ERROR round trip for any real ID such as
  // "Tokyo Standard Time".
  std::size_t len = 128;
  for (;;) {
    UErrorCode status = U_ZERO_ERROR;
    result.resize(len);
    len = static_cast<std::size_t>(
        getWindowsTimeZoneID(iana_name.c_str(), iana_name_length, &result[0],
                             static_cast<int>(len), &status));
    if (U_SUCCESS(status)) {
      result.resize(len);
      return result;
    }
    if (status != U_BUFFER_OVERFLOW_ERROR) {
      return std::wstring();
    }
  }
}

}  // namespace cctz
