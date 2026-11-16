// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/credentials.hpp"
#include "gz_cesium/task_pool.hpp"
#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/SampleHeightResult.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumUtility/CreditSystem.h>
#include <spdlog/sinks/ostream_sink.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <iostream>
#include <sstream>

namespace cs = Cesium3DTilesSelection;
class HeightResources final : public cs::IPrepareRendererResources {
 public:
  CesiumAsync::Future<cs::TileLoadResultAndRenderResources> prepareInLoadThread(
      const CesiumAsync::AsyncSystem& async, cs::TileLoadResult&& result,
      const glm::dmat4&, const std::any&) override {
    return async.createResolvedFuture(cs::TileLoadResultAndRenderResources{std::move(result), nullptr});
  }
  void* prepareInMainThread(cs::Tile&, void*) override { return nullptr; }
  void free(cs::Tile&, void*, void*) noexcept override {}
  void* prepareRasterInLoadThread(CesiumImage::ImageAsset&, const std::any&) override { return nullptr; }
  void* prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile&, void*) override { return nullptr; }
  void freeRaster(const CesiumRasterOverlays::RasterOverlayTile&, void*, void*) noexcept override {}
  void attachRasterInMainThread(const cs::Tile&, int32_t,
      const CesiumRasterOverlays::RasterOverlayTile&, void*, const glm::dvec2&, const glm::dvec2&) override {}
  void detachRasterInMainThread(const cs::Tile&, int32_t,
      const CesiumRasterOverlays::RasterOverlayTile&, void*) noexcept override {}
};

int main(int argc, char** argv) {
  try {
    std::string url;
    int64_t asset = 2275207;
    double timeout = 120.;
    std::vector<CesiumGeospatial::Cartographic> points;
    for (int i=1; i<argc; ++i) {
      const std::string option = argv[i];
      if (++i >= argc) throw std::runtime_error("Expected --point lat,lon, --ion-asset ID, --url URL or --timeout SECONDS");
      const std::string value = argv[i];
      if (option == "--url") url = value;
      else if (option == "--ion-asset") asset = std::stoll(value);
      else if (option == "--timeout") timeout = std::stod(value);
      else if (option == "--point") {
        const auto comma = value.find(',');
        if (comma == std::string::npos) throw std::runtime_error("Point requires latitude,longitude");
        const auto lat = std::stod(value.substr(0, comma)), lon = std::stod(value.substr(comma+1));
        if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat)>90 || std::abs(lon)>180)
          throw std::runtime_error("Invalid point coordinates");
        points.push_back(CesiumGeospatial::Cartographic::fromDegrees(lon, lat));
      } else throw std::runtime_error("Unknown probe option");
    }
    if (points.empty() || asset <= 0 || !std::isfinite(timeout) || timeout <= 0)
      throw std::runtime_error("Specify at least one --point lat,lon and a valid asset/timeout");
    Cesium3DTilesContent::registerAllTileContentTypes();
    const auto pool = std::make_shared<gz_cesium::TaskPool>(4);
    CesiumAsync::AsyncSystem async(pool);
    const auto accessor = std::make_shared<CesiumCurl::CurlAssetAccessor>();
    const auto resources = std::make_shared<HeightResources>();
    const auto credits = std::make_shared<CesiumUtility::CreditSystem>();
    // The CLI returns only structured, non-secret diagnostics. Native logs
    // may contain signed URLs, so never forward this private stream.
    std::ostringstream diagnostics;
    auto logger = std::make_shared<spdlog::logger>("cesium_probe",
        std::make_shared<spdlog::sinks::ostream_sink_mt>(diagnostics));
    cs::TilesetExternals externals{accessor, resources, async, credits, logger};
    cs::TilesetOptions options;
    options.maximumSimultaneousTileLoads = 4;
    options.maximumCachedBytes = 64LL*1024*1024;
    options.preloadSiblings = false;
    options.preloadAncestors = false;
    std::unique_ptr<cs::Tileset> tileset;
    if (url.empty()) tileset = std::make_unique<cs::Tileset>(externals, asset, gz_cesium::ionToken(), options);
    else tileset = std::make_unique<cs::Tileset>(externals, url, options);
    auto result = tileset->sampleHeightMostDetailed(points);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::duration<double>(timeout);
    while (!result.isReady() && std::chrono::steady_clock::now() < deadline) {
      accessor->tick();
      async.dispatchMainThreadTasks();
      tileset->updateViewGroup(tileset->getDefaultViewGroup(), {});
      async.dispatchMainThreadTasks();
      tileset->loadTiles();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool timedOut = !result.isReady();
    std::optional<cs::SampleHeightResult> heights;
    if (!timedOut) heights = result.wait();
    std::size_t meshTiles = 0, vertices = 0, triangles = 0, doubleSided = 0;
    std::vector<CesiumGltf::Material> sourceMaterials;
    tileset->forEachLoadedTile([&](const cs::Tile& tile) {
      if (const auto* content = tile.getContent().getRenderContent()) {
        ++meshTiles;
        const auto& model = content->getModel();
        sourceMaterials.insert(sourceMaterials.end(), model.materials.begin(), model.materials.end());
        for (const auto& material : model.materials) doubleSided += material.doubleSided;
        for (const auto& mesh : model.meshes) for (const auto& primitive : mesh.primitives) {
          auto position = primitive.attributes.find("POSITION");
          if (position != primitive.attributes.end()) vertices += model.accessors[position->second].count;
          if (primitive.indices >= 0) triangles += model.accessors[primitive.indices].count/3;
        }
      }
    });
    auto destroyed = tileset->getAsyncDestructionCompleteEvent();
    tileset.reset();
    while (!destroyed.isReady()) {
      accessor->tick();
      async.dispatchMainThreadTasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (timedOut) throw std::runtime_error("Terrain height lookup timed out");
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject();
    out.Key("schema"); out.Int(1);
    out.Key("loaded_mesh_tiles"); out.Uint64(meshTiles);
    out.Key("loaded_vertices"); out.Uint64(vertices);
    out.Key("loaded_triangles"); out.Uint64(triangles);
    out.Key("double_sided_materials"); out.Uint64(doubleSided);
    out.Key("source_materials"); out.StartArray();
    for (const auto& material : sourceMaterials) {
      out.StartObject();
      out.Key("extensions"); out.StartArray();
      for (const auto& [name, value] : material.extensions) out.String(name.c_str());
      out.EndArray();
      if (material.pbrMetallicRoughness) {
        const auto& pbr = *material.pbrMetallicRoughness;
        out.Key("metallic_factor"); out.Double(pbr.metallicFactor);
        out.Key("roughness_factor"); out.Double(pbr.roughnessFactor);
        out.Key("base_color_factor"); out.StartArray();
        for (const auto factor : pbr.baseColorFactor) out.Double(factor);
        out.EndArray();
        out.Key("texture_extensions"); out.StartArray();
        if (pbr.baseColorTexture) for (const auto& [name, value] : pbr.baseColorTexture->extensions)
          out.String(name.c_str());
        out.EndArray();
      }
      out.EndObject();
    }
    out.EndArray();
    out.Key("samples"); out.StartArray();
    bool success = true;
    for (std::size_t i=0; i<heights->positions.size(); ++i) {
      out.StartObject();
      out.Key("latitude"); out.Double(glm::degrees(heights->positions[i].latitude));
      out.Key("longitude"); out.Double(glm::degrees(heights->positions[i].longitude));
      out.Key("height"); out.Double(heights->positions[i].height);
      out.Key("success"); out.Bool(heights->sampleSuccess[i]);
      success &= heights->sampleSuccess[i];
      out.EndObject();
    }
    out.EndArray();
    out.Key("warning_count"); out.Uint64(heights->warnings.size());
    out.EndObject();
    std::cout << buffer.GetString() << '\n';
    return success ? 0 : 2;
  } catch (const std::exception&) {
    std::cerr << "Cesium height probe failed; check the source, credential and network access. No signed URLs are logged.\n";
    return 1;
  }
}
