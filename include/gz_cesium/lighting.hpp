// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gz/rendering/RenderTypes.hh>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace gz_cesium {
struct LightingOptions {
  bool enabled{true};
  std::array<std::uint32_t,3> resolution{128,128,64};
  unsigned bounces{1};
  double rebuildInterval{2.}, settleTime{.5};
  void validate() const;
};

// Used exclusively on the owning rendering thread, by both camera and GUI
// adapters. Static terrain supplies indirect light; moving robots receive it.
class Lighting {
 public:
  Lighting(LightingOptions options, std::string label, std::string status={});
  ~Lighting();
  void update(bool enabled);
  void reset();
 private:
  using Clock = std::chrono::steady_clock;
  void status(bool active, std::size_t visuals);
  LightingOptions options_;
  std::string label_, status_;
  std::weak_ptr<gz::rendering::Scene> scene_;
  gz::rendering::GlobalIlluminationVctPtr gi_;
  std::uint64_t signature_{}, builtSignature_{}, builds_{};
  std::size_t bounceVisuals_{};
  Clock::time_point changed_{Clock::now()}, dirty_{Clock::now()}, built_{}, reported_{};
  bool active_{}, failed_{};
  double buildMs_{};
};
}
