// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <CesiumGltf/Model.h>
#include <glm/glm.hpp>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <limits>
#include <optional>
#include <vector>

namespace gz_cesium {

struct TileVisual {
  std::filesystem::path uri;
  std::filesystem::path texture;
  glm::dvec4 color{1.};
  double metallic{1.}, roughness{1.};
  bool unlit{}, doubleSided{};
};

struct TileMesh {
  std::uint64_t id{};
  std::filesystem::path directory;
  std::filesystem::path uri;
  glm::dvec3 minimum{std::numeric_limits<double>::max()};
  glm::dvec3 maximum{std::numeric_limits<double>::lowest()};
  std::vector<glm::dvec3> vertices;
  std::vector<std::array<std::uint32_t, 3>> triangles;
  std::size_t textureCount{};
  std::size_t textureBytes{}, exportedBytes{};
  std::vector<TileVisual> visuals;

  bool near(const glm::dvec3& position, double radius) const;
  std::optional<double> heightAt(double x, double y) const;
};

// All transforms, including RTC, glTF up axis and node hierarchy, are applied
// in double precision before writing coordinates in the local ENU frame.
std::shared_ptr<TileMesh> prepareMesh(const CesiumGltf::Model& model,
    const glm::dmat4& tileToEcef, const glm::dmat4& ecefToEnu,
    const std::filesystem::path& directory, std::uint64_t id);

glm::dmat4 enuToEcef(double latitudeDegrees, double longitudeDegrees,
                    double ellipsoidHeight);

}  // namespace gz_cesium
