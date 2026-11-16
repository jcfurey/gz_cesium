// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdlib>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>

namespace gz_cesium {
inline std::string ionToken() {
  if (const auto* token = std::getenv("CESIUM_ION_TOKEN"); token && *token) return token;
  const auto* path = std::getenv("CESIUM_ION_TOKEN_FILE");
  if (!path || !*path) throw std::runtime_error("Set CESIUM_ION_TOKEN or CESIUM_ION_TOKEN_FILE for the ion tileset");
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot read CESIUM_ION_TOKEN_FILE");
  std::string text((std::istreambuf_iterator<char>(input)), {});
  if (text.size() > 65536) throw std::runtime_error("Credential file is unexpectedly large");
  // Accept an ordinary token file, including whitespace or a quoted JWT.
  // A unique JWT can also be read from an existing user's short notes file.
  const std::regex jwt("eyJ[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+");
  std::sregex_iterator match(text.begin(), text.end(), jwt), end;
  if (match != end) {
    auto token = match->str();
    if (++match != end) throw std::runtime_error("Credential file contains multiple tokens; select a single-token file");
    return token;
  }
  const auto first = text.find_first_not_of(" \r\n\t\"'");
  const auto last = text.find_last_not_of(" \r\n\t\"'");
  if (first == std::string::npos) throw std::runtime_error("Credential file is empty");
  auto token = text.substr(first, last-first+1);
  if (token.find_first_of(" \r\n\t") != std::string::npos)
    throw std::runtime_error("Cannot identify one token in credential file");
  return token;
}
}  // namespace gz_cesium
