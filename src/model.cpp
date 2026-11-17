// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/model.hpp"
#include <sdf/Collision.hh>
#include <sdf/Geometry.hh>
#include <sdf/Link.hh>
#include <sdf/Material.hh>
#include <sdf/Mesh.hh>
#include <sdf/Pbr.hh>
#include <sdf/Visual.hh>

namespace gz_cesium {
namespace {
sdf::Geometry geometry(const std::filesystem::path& uri) {
  sdf::Mesh shape;
  shape.SetUri(uri.string());
  sdf::Geometry result;
  result.SetType(sdf::GeometryType::MESH);
  result.SetMeshShape(shape);
  return result;
}
}

sdf::Model tileModel(const TileMesh& mesh) {
  sdf::Link link;
  link.SetName("terrain");
  sdf::Collision collision;
  collision.SetName("collision");
  collision.SetGeom(geometry(mesh.uri));
  link.AddCollision(collision);
  for (std::size_t i=0; i<mesh.visuals.size(); ++i) {
    const auto& source = mesh.visuals[i];
    sdf::Visual visual;
    visual.SetName("visual_"+std::to_string(i));
    visual.SetGeom(geometry(source.uri));
    // Baked photogrammetry stays emissive, but its geometry must still block
    // direct sunlight onto live robots and other normally lit objects.
    visual.SetCastShadows(true);
    sdf::Material material;
    const auto color = gz::math::Color(source.color.x, source.color.y, source.color.z, source.color.w);
    material.SetDoubleSided(source.doubleSided);
    material.SetSpecular(gz::math::Color(0,0,0,1));
    material.SetAmbient(source.unlit ? gz::math::Color(0,0,0,1) : color);
    material.SetDiffuse(source.unlit ? gz::math::Color(0,0,0,source.color.w) : color);
    material.SetEmissive(source.unlit ? color : gz::math::Color(0,0,0,1));
    sdf::PbrWorkflow workflow;
    workflow.SetMetalness(source.unlit ? 0. : source.metallic);
    workflow.SetRoughness(source.unlit ? 1. : source.roughness);
    if (!source.texture.empty()) {
      workflow.SetAlbedoMap(source.texture.string());
      if (source.unlit) workflow.SetEmissiveMap(source.texture.string());
    }
    sdf::Pbr pbr;
    pbr.SetWorkflow(sdf::PbrWorkflowType::METAL, workflow);
    material.SetPbrMaterial(pbr);
    visual.SetMaterial(material);
    link.AddVisual(visual);
  }
  sdf::Model result;
  result.SetName("cesium_tile_"+std::to_string(mesh.id));
  result.SetStatic(true);
  result.AddLink(link);
  return result;
}
}
