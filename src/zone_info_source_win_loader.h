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

#ifndef CCTZ_ZONE_INFO_SOURCE_WIN_LOADER_H_
#define CCTZ_ZONE_INFO_SOURCE_WIN_LOADER_H_

#include <memory>
#include <string>

#include "zone_info_source_win.h"

namespace cctz {

// Loads the registry time zone that the IANA time zone `name` maps to.
// Returns nullptr if there is none or its data is invalid.
std::unique_ptr<ZoneInfoSource> LoadWinZoneInfoSource(const std::string& name);

}  // namespace cctz

#endif  // CCTZ_ZONE_INFO_SOURCE_WIN_LOADER_H_
