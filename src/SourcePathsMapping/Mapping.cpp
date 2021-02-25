// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "SourcePathsMapping/Mapping.h"

#include <absl/strings/match.h>

#include <filesystem>
#include <system_error>

#include "OrbitBase/Logging.h"

namespace fs = std::filesystem;

namespace orbit_source_paths_mapping {

std::optional<fs::path> MapToFirstMatchingTarget(absl::Span<const Mapping> mappings,
                                                 const fs::path& source_path) {
  for (const auto& mapping : mappings) {
    if (absl::StartsWith(source_path.string(), mapping.source_path.string())) {
      std::string target = mapping.target_path.string();
      target.append(
          std::string_view{source_path.string()}.substr(mapping.source_path.string().size()));
      return fs::path{target};
    }
  }

  return std::nullopt;
}

std::optional<fs::path> MapToFirstExistingTarget(absl::Span<const Mapping> mappings,
                                                 const fs::path& source_path) {
  for (const auto& mapping : mappings) {
    if (absl::StartsWith(source_path.string(), mapping.source_path.string())) {
      std::string target = mapping.target_path.string();
      target.append(
          std::string_view{source_path.string()}.substr(mapping.source_path.string().size()));
      auto target_path = fs::path{target};

      std::error_code error{};
      if (fs::is_regular_file(target_path, error)) return target_path;
      if (error.value() != 0) {
        ERROR("Failed to 'stat' the file \"%s\": %s", target_path.string(), error.message());
      }
    }
  }

  return std::nullopt;
}
}  // namespace orbit_source_paths_mapping