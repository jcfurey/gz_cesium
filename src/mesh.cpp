// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/mesh.hpp"

#include <CesiumGeospatial/Cartographic.h>
#include <CesiumGeospatial/LocalHorizontalCoordinateSystem.h>
#include <CesiumGltf/AccessorUtility.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionKhrTextureBasisu.h>
#include <CesiumGltf/ExtensionKhrMaterialsUnlit.h>
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumImage/ImageAsset.h>
#include <gz/common/ColladaExporter.hh>
#include <gz/common/Image.hh>
#include <gz/common/Material.hh>
#include <gz/common/Mesh.hh>
#include <gz/common/SubMesh.hh>
#include <glm/gtc/matrix_inverse.hpp>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace gz_cesium {
using CesiumGltfContent::GltfUtilities;

glm::dmat4 enuToEcef(double latitude, double longitude, double height) {
  using namespace CesiumGeospatial;
  LocalHorizontalCoordinateSystem frame(Cartographic::fromDegrees(longitude, latitude, height),
      LocalDirection::East, LocalDirection::North, LocalDirection::Up);
  return frame.getLocalToEcefTransformation();
}

bool TileMesh::near(const glm::dvec3& point, double radius) const {
  const auto closest = glm::clamp(glm::dvec2(point), glm::dvec2(minimum), glm::dvec2(maximum));
  return glm::length(glm::dvec2(point) - closest) <= radius;
}

std::optional<double> TileMesh::heightAt(double x, double y) const {
  if (x < minimum.x - 1e-6 || x > maximum.x + 1e-6 ||
      y < minimum.y - 1e-6 || y > maximum.y + 1e-6) return {};
  std::optional<double> highest;
  for (const auto& face : triangles) {
    const auto& a = vertices[face[0]];
    const auto& b = vertices[face[1]];
    const auto& c = vertices[face[2]];
    const double denominator = (b.y-c.y)*(a.x-c.x) + (c.x-b.x)*(a.y-c.y);
    if (std::abs(denominator) < 1e-12) continue;
    const double u = ((b.y-c.y)*(x-c.x) + (c.x-b.x)*(y-c.y))/denominator;
    const double v = ((c.y-a.y)*(x-c.x) + (a.x-c.x)*(y-c.y))/denominator;
    if (u < -1e-7 || v < -1e-7 || u+v > 1.+1e-7) continue;
    const double z = u*a.z + v*b.z + (1.-u-v)*c.z;
    if (!highest || z > *highest) highest = z;
  }
  return highest;
}

std::shared_ptr<TileMesh> prepareMesh(const CesiumGltf::Model& model,
    const glm::dmat4& tileToEcef, const glm::dmat4& ecefToEnu,
    const std::filesystem::path& directory, std::uint64_t id) {
  auto result = std::make_shared<TileMesh>();
  result->id = id;
  result->directory = directory / ("tile_" + std::to_string(id));
  std::filesystem::create_directories(result->directory/"meshes");
  std::filesystem::create_directories(result->directory/"materials/textures");
  result->uri = result->directory / "meshes/mesh.dae";
  gz::common::Mesh mesh;
  mesh.SetName("cesium_" + std::to_string(id));
  const auto root = ecefToEnu * GltfUtilities::applyGltfUpAxisTransform(
      model, GltfUtilities::applyRtcCenter(model, tileToEcef));
  model.forEachPrimitiveInScene(-1, [&](const CesiumGltf::Model& gltf,
      const CesiumGltf::Node&, const CesiumGltf::Mesh&,
      const CesiumGltf::MeshPrimitive& primitive, const glm::dmat4& node) {
    if (primitive.mode != CesiumGltf::MeshPrimitive::Mode::TRIANGLES) return;
    auto position = primitive.attributes.find("POSITION");
    if (position == primitive.attributes.end()) return;
    CesiumGltf::AccessorView<glm::vec3> positions(gltf, position->second);
    if (positions.status() != CesiumGltf::AccessorViewStatus::Valid || positions.size() == 0)
      throw std::runtime_error("Invalid decoded tile positions");
    const auto transform = root * node;
    const auto normalTransform = glm::inverseTranspose(glm::dmat3(transform));
    std::vector<std::uint32_t> indices;
    if (primitive.indices < 0) {
      indices.resize(positions.size());
      std::iota(indices.begin(), indices.end(), 0);
    } else {
      std::visit([&](const auto& accessor) {
        using T = std::decay_t<decltype(accessor)>;
        if constexpr (!std::is_same_v<T, std::monostate>) {
          if (accessor.status() != CesiumGltf::AccessorViewStatus::Valid)
            throw std::runtime_error("Invalid decoded tile indices");
          for (std::int64_t i = 0; i < accessor.size(); ++i) indices.push_back(accessor[i]);
        }
      }, CesiumGltf::getIndexAccessorView(gltf, primitive));
    }
    if (indices.empty() || indices.size()%3) throw std::runtime_error("Invalid tile triangle list");
    for (const auto index : indices)
      if (index >= positions.size()) throw std::runtime_error("Tile index out of bounds");
    if (glm::determinant(glm::dmat3(transform)) < 0.)
      for (std::size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i+1], indices[i+2]);

    auto material = std::make_shared<gz::common::Material>();
    const auto& sourceMaterial = CesiumGltf::Model::getSafe(gltf.materials, primitive.material);
    const auto& pbr = sourceMaterial.pbrMetallicRoughness;
    TileVisual visual;
    visual.unlit = sourceMaterial.getExtension<CesiumGltf::ExtensionKhrMaterialsUnlit>() != nullptr;
    visual.doubleSided = sourceMaterial.doubleSided;
    if (pbr) { visual.metallic = pbr->metallicFactor; visual.roughness = pbr->roughnessFactor; }
    gz::math::Color color(1., 1., 1., 1.);
    int uvSet = 0;
    if (pbr) {
      const auto& factor = pbr->baseColorFactor;
      if (factor.size() == 4) color.Set(factor[0], factor[1], factor[2], factor[3]);
      if (pbr->baseColorTexture) {
        const auto& textureInfo = *pbr->baseColorTexture;
        uvSet = textureInfo.texCoord;
        const auto& texture = CesiumGltf::Model::getSafe(gltf.textures, textureInfo.index);
        int imageIndex = texture.source;
        if (const auto* basis = texture.getExtension<CesiumGltf::ExtensionKhrTextureBasisu>())
          imageIndex = basis->source;
        const auto& image = CesiumGltf::Model::getSafe(gltf.images, imageIndex);
        if (image.pAsset && image.pAsset->width > 0 && image.pAsset->height > 0) {
          const auto& pixels = *image.pAsset;
          if (pixels.bytesPerChannel != 1 ||
              pixels.compressedPixelFormat != CesiumImage::GpuCompressedPixelFormat::NONE)
            throw std::runtime_error("Tile texture must decode to 8-bit pixels");
          const auto count = std::size_t(pixels.width)*pixels.height;
          if (pixels.channels < 1 || pixels.channels > 4 || pixels.pixelData.size() < count*pixels.channels)
            throw std::runtime_error("Invalid decoded tile texture");
          std::vector<unsigned char> rgba(count*4);
          const auto* data = reinterpret_cast<const unsigned char*>(pixels.pixelData.data());
          for (std::size_t i = 0; i < count; ++i) {
            rgba[i*4] = data[i*pixels.channels];
            rgba[i*4+1] = data[i*pixels.channels + (pixels.channels >= 3 ? 1 : 0)];
            rgba[i*4+2] = data[i*pixels.channels + (pixels.channels >= 3 ? 2 : 0)];
            rgba[i*4+3] = pixels.channels == 4 ? data[i*4+3] :
                (pixels.channels == 2 ? data[i*2+1] : 255);
          }
          gz::common::Image output;
          output.SetFromData(rgba.data(), pixels.width, pixels.height, gz::common::Image::RGBA_INT8);
          // Ogre2 registers image resources by basename, not absolute path.
          // Include both the per-run directory and tile ID so independent
          // tiles / systems cannot accidentally share the first tile's atlas.
          const auto path = result->directory / "materials/textures" /
              (directory.filename().string() + "_tile_" + std::to_string(id) +
               "_texture_" + std::to_string(mesh.SubMeshCount()) + ".png");
          output.SavePNG(path.string());
          material->SetTextureImage(path.string());
          visual.texture = path;
          result->textureBytes += rgba.size();
          ++result->textureCount;
        }
      }
    }
    material->SetDiffuse(color);
    material->SetAmbient(color);
    material->SetSpecular(gz::math::Color(0, 0, 0, 1));
    material->SetTransparency(1.-color.A());
    visual.color = {color.R(), color.G(), color.B(), color.A()};
    auto submesh = std::make_unique<gz::common::SubMesh>();
    submesh->SetName("primitive_" + std::to_string(mesh.SubMeshCount()));
    submesh->SetPrimitiveType(gz::common::SubMesh::TRIANGLES);
    submesh->SetMaterialIndex(mesh.AddMaterial(material));
    CesiumGltf::AccessorView<glm::vec3> normals(gltf,
        primitive.attributes.contains("NORMAL") ? primitive.attributes.at("NORMAL") : -1);
    const auto uvName = "TEXCOORD_" + std::to_string(uvSet);
    CesiumGltf::AccessorView<glm::vec2> uv(gltf,
        primitive.attributes.contains(uvName) ? primitive.attributes.at(uvName) : -1);
    const auto base = std::uint32_t(result->vertices.size());
    for (std::int64_t i = 0; i < positions.size(); ++i) {
      const auto point = glm::dvec3(transform*glm::dvec4(positions[i], 1.));
      if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
        throw std::runtime_error("Non-finite tile vertex");
      result->minimum = glm::min(result->minimum, point);
      result->maximum = glm::max(result->maximum, point);
      result->vertices.push_back(point);
      submesh->AddVertex(point.x, point.y, point.z);
      if (normals.status() == CesiumGltf::AccessorViewStatus::Valid && i < normals.size()) {
        const auto n = glm::normalize(normalTransform*glm::dvec3(normals[i]));
        submesh->AddNormal(n.x, n.y, n.z);
      }
      if (uv.status() == CesiumGltf::AccessorViewStatus::Valid && i < uv.size())
        // Gazebo's exporter and loader each flip V. Their round trip keeps
        // Gazebo's internal, image-origin convention, which matches glTF.
        submesh->AddTexCoord(uv[i].x, uv[i].y);
    }
    for (const auto index : indices) submesh->AddIndex(index);
    for (std::size_t i = 0; i < indices.size(); i += 3)
      result->triangles.push_back({base+indices[i], base+indices[i+1], base+indices[i+2]});
    if (submesh->NormalCount() != submesh->VertexCount()) submesh->RecalculateNormals();
    // SDF material overrides apply to a whole visual. Export one mesh per
    // primitive so different source materials retain independent unlit/PBR
    // settings in both the sensor renderer and an external Gazebo GUI.
    gz::common::Mesh part;
    part.SetName("cesium_part_"+std::to_string(id)+"_"+std::to_string(mesh.SubMeshCount()));
    auto copy = std::make_unique<gz::common::SubMesh>();
    copy->SetName("primitive");
    copy->SetPrimitiveType(gz::common::SubMesh::TRIANGLES);
    copy->SetMaterialIndex(part.AddMaterial(material));
    for (unsigned i=0; i<submesh->VertexCount(); ++i) {
      const auto point = submesh->Vertex(i);
      copy->AddVertex(point.X(), point.Y(), point.Z());
      const auto normal = submesh->Normal(i);
      copy->AddNormal(normal.X(), normal.Y(), normal.Z());
      if (i<submesh->TexCoordCount()) {
        const auto uv = submesh->TexCoord(i);
        copy->AddTexCoord(uv.X(), uv.Y());
      }
    }
    for (unsigned i=0; i<submesh->IndexCount(); ++i) copy->AddIndex(submesh->Index(i));
    part.AddSubMesh(std::move(copy));
    const auto basename = result->directory/"meshes"/("visual_"+std::to_string(mesh.SubMeshCount()));
    gz::common::ColladaExporter partExporter;
    partExporter.Export(&part, basename.string(), false);
    visual.uri = basename.string()+".dae";
    if (!std::filesystem::is_regular_file(visual.uri)) throw std::runtime_error("Tile primitive export failed");
    result->visuals.push_back(std::move(visual));
    mesh.AddSubMesh(std::move(submesh));
  });
  // Empty render content is valid in a hierarchy, including some OSM building
  // parents. It must not poison the terrain readiness gate for other layers.
  if (result->triangles.empty()) {
    std::filesystem::remove_all(result->directory);
    return {};
  }
  gz::common::ColladaExporter exporter;
  exporter.Export(&mesh, (result->directory / "meshes/mesh").string(), false);
  if (!std::filesystem::is_regular_file(result->uri)) throw std::runtime_error("Tile mesh export failed");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(result->directory))
    if (entry.is_regular_file()) result->exportedBytes += entry.file_size();
  return result;
}
}  // namespace gz_cesium
