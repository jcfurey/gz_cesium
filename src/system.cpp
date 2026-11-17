// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/mesh.hpp"
#include "gz_cesium/model.hpp"
#include "gz_cesium/task_pool.hpp"
#include "gz_cesium/credentials.hpp"

#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/ITileExcluder.h>
#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <Cesium3DTilesSelection/ViewUpdateResult.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumUtility/CreditSystem.h>
#include <gz/common/Console.hh>
#include <gz/common/MeshManager.hh>
#include <gz/common/SystemPaths.hh>
#include <gz/plugin/Register.hh>
#include <gz/sim/EntityComponentManager.hh>
#include <gz/sim/EventManager.hh>
#include <gz/sim/Events.hh>
#include <gz/sim/Model.hh>
#include <gz/sim/SdfEntityCreator.hh>
#include <gz/sim/System.hh>
#include <gz/sim/Util.hh>
#include <gz/sim/components/Model.hh>
#include <gz/sim/components/Name.hh>
#include <gz/sim/components/Static.hh>
#include <gz/transport/Node.hh>
#include <gz/msgs/stringmsg.pb.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sdf/Collision.hh>
#include <sdf/Geometry.hh>
#include <sdf/Link.hh>
#include <sdf/Mesh.hh>
#include <sdf/Model.hh>
#include <sdf/Visual.hh>
#include <spdlog/sinks/base_sink.h>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <unistd.h>

namespace gz_cesium {
namespace cs = Cesium3DTilesSelection;
using Clock = std::chrono::steady_clock;

std::string redact(std::string message) {
  message = std::regex_replace(message,
      std::regex("([?&](access_token|key|token)=)[^&\\s\\\"'<>]+", std::regex::icase), "$1<redacted>");
  return std::regex_replace(message, std::regex("eyJ[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+"), "<redacted-token>");
}

class LogSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    const auto text = "[gz_cesium] " + redact(std::string(message.payload.data(), message.payload.size()));
    if (message.level >= spdlog::level::err) gzerr << text << '\n';
    else if (message.level >= spdlog::level::warn) gzwarn << text << '\n';
    else gzmsg << text << '\n';
  }
  void flush_() override {}
};

struct Resource { std::shared_ptr<TileMesh> mesh; };

class RangeExcluder final : public cs::ITileExcluder {
 public:
  double radius{150.};
  std::vector<glm::dvec3> anchors;
  bool shouldExclude(const cs::Tile& tile) const noexcept override {
    if (anchors.empty()) return false;
    const auto bounds = cs::getOrientedBoundingBoxFromBoundingVolume(tile.getBoundingVolume());
    return std::all_of(anchors.begin(), anchors.end(), [&](const auto& anchor) {
      return bounds.computeDistanceSquaredToPosition(anchor) > radius*radius;
    });
  }
};

class Resources final : public cs::IPrepareRendererResources {
 public:
  glm::dmat4 ecefToEnu{1.};
  std::filesystem::path directory;
  std::atomic<std::uint64_t> nextId{0};
  std::atomic<unsigned> failures{0};
  std::shared_ptr<spdlog::logger> logger;
  struct Retired { std::shared_ptr<TileMesh> mesh; Clock::time_point time; };
  std::vector<Retired> retired;

  CesiumAsync::Future<cs::TileLoadResultAndRenderResources> prepareInLoadThread(
      const CesiumAsync::AsyncSystem& async, cs::TileLoadResult&& result,
      const glm::dmat4& transform, const std::any&) override {
    Resource* resource = nullptr;
    try {
      if (const auto* model = std::get_if<CesiumGltf::Model>(&result.contentKind)) {
        auto mesh = prepareMesh(*model, transform, ecefToEnu, directory, nextId++);
        if (mesh) resource = new Resource{std::move(mesh)};
      }
    } catch (const std::exception& error) {
      ++failures;
      logger->error("Tile preparation failed: {}", error.what());
      result.state = cs::TileLoadResultState::Failed;
    }
    return async.createResolvedFuture(cs::TileLoadResultAndRenderResources{std::move(result), resource});
  }
  void* prepareInMainThread(cs::Tile&, void* loaded) override { return loaded; }
  void free(cs::Tile&, void* loaded, void* main) noexcept override {
    for (auto* pointer : {static_cast<Resource*>(loaded), static_cast<Resource*>(main)}) {
      if (pointer) {
        retired.push_back({std::move(pointer->mesh), Clock::now()});
        delete pointer;
      }
    }
  }
  void collect() {
    std::erase_if(retired, [](const Retired& entry) {
      if (entry.mesh.use_count() != 1 || Clock::now()-entry.time < std::chrono::seconds(5)) return false;
      // Entity deletion and the renderer have had several frames to release
      // this geometry before its URI and the server's CPU cache are retired.
      gz::common::MeshManager::Instance()->RemoveMesh(entry.mesh->uri.string());
      for (const auto& visual : entry.mesh->visuals)
        gz::common::MeshManager::Instance()->RemoveMesh(visual.uri.string());
      std::error_code error;
      std::filesystem::remove_all(entry.mesh->directory, error);
      return true;
    });
  }
  void* prepareRasterInLoadThread(CesiumImage::ImageAsset&, const std::any&) override { return nullptr; }
  void* prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile&, void*) override { return nullptr; }
  void freeRaster(const CesiumRasterOverlays::RasterOverlayTile&, void*, void*) noexcept override {}
  void attachRasterInMainThread(const cs::Tile&, int32_t,
      const CesiumRasterOverlays::RasterOverlayTile&, void*, const glm::dvec2&, const glm::dvec2&) override {}
  void detachRasterInMainThread(const cs::Tile&, int32_t,
      const CesiumRasterOverlays::RasterOverlayTile&, void*) noexcept override {}
};

class CesiumSystem final : public gz::sim::System,
    public gz::sim::ISystemConfigure, public gz::sim::ISystemPreUpdate {
 public:
  ~CesiumSystem() override {
    active_.clear();
    pins_.clear();
    if (!tilesets_.empty()) {
      std::vector<CesiumAsync::SharedFuture<void>> complete;
      for (auto& tileset : tilesets_) complete.push_back(tileset->getAsyncDestructionCompleteEvent());
      tilesets_.clear();
      while (std::any_of(complete.begin(), complete.end(), [](const auto& task) { return !task.isReady(); })) {
        accessor_->tick();
        async_->dispatchMainThreadTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    resources_.reset();
    accessor_.reset();
    async_.reset();
    pool_.reset();
    if (!assetDirectory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(assetDirectory_, error);
    }
  }

  void Configure(const gz::sim::Entity& entity, const std::shared_ptr<const sdf::Element>& sdf,
      gz::sim::EntityComponentManager& ecm, gz::sim::EventManager& events) override {
    world_ = entity;
    events_ = &events;
    creator_ = std::make_unique<gz::sim::SdfEntityCreator>(ecm, events);
    try {
      static std::once_flag registered;
      std::call_once(registered, Cesium3DTilesContent::registerAllTileContentTypes);
      const double latitude = sdf->Get<double>("latitude", 32.3645).first;
      const double longitude = sdf->Get<double>("longitude", -90.8431).first;
      const double height = sdf->Get<double>("height", 100.).first;
      radius_ = sdf->Get<double>("contact_radius", 15.).first;
      interval_ = sdf->Get<double>("update_interval", .2).first;
      waitForTerrain_ = sdf->Get<bool>("wait_for_terrain", true).first;
      placeOnGround_ = sdf->Get<bool>("place_on_ground", true).first;
      if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(height) ||
          latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180 ||
          !std::isfinite(radius_) || radius_ < 1 || radius_ > 1000 ||
          !std::isfinite(interval_) || interval_ < .02 || interval_ > 5.)
        throw std::runtime_error("Invalid Cesium origin, contact radius or update interval");
      enuToEcef_ = enuToEcef(latitude, longitude, height);
      if (sdf->HasElement("follow_model")) {
        auto follow = sdf->GetElementImpl("follow_model");
        while (follow) {
          followNames_.insert(follow->Get<std::string>());
          follow = follow->GetNextElement("follow_model");
        }
      }
      std::string root = environment("GZ_CESIUM_ASSET_DIR");
      if (root.empty()) root = std::filesystem::temp_directory_path().string();
      assetDirectory_ = std::filesystem::path(root) / ("gz_cesium_" + std::to_string(getpid()) +
          "_" + std::to_string(Clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(assetDirectory_);
      std::filesystem::permissions(assetDirectory_, std::filesystem::perms::owner_all);
      statusFile_ = environment("GZ_CESIUM_STATUS_FILE");
      if (statusFile_.empty()) statusFile_ = (assetDirectory_/"status.json").string();
      auto logger = std::make_shared<spdlog::logger>("gz_cesium", std::make_shared<LogSink>());
      pool_ = std::make_shared<TaskPool>(4);
      async_ = std::make_unique<CesiumAsync::AsyncSystem>(pool_);
      resources_ = std::make_shared<Resources>();
      resources_->ecefToEnu = glm::inverse(enuToEcef_);
      resources_->directory = assetDirectory_;
      resources_->logger = logger;
      accessor_ = std::make_shared<CesiumCurl::CurlAssetAccessor>();
      credits_ = std::make_shared<CesiumUtility::CreditSystem>();
      cs::TilesetOptions options;
      range_ = std::make_shared<RangeExcluder>();
      range_->radius = sdf->Get<double>("stream_radius", 150.).first;
      if (!std::isfinite(range_->radius) || range_->radius < radius_*1.5 || range_->radius > 10000.)
        throw std::runtime_error("stream_radius must cover the contact radius and be at most 10 km");
      options.excluders.push_back(range_);
      options.maximumScreenSpaceError = sdf->Get<double>("maximum_screen_space_error", 4.).first;
      options.maximumSimultaneousTileLoads = sdf->Get<unsigned>("maximum_simultaneous_tile_loads", 8).first;
      const auto cacheBytes = sdf->Get<uint64_t>("maximum_cached_bytes", 256ULL*1024*1024).first;
      if (cacheBytes > 8ULL*1024*1024*1024) throw std::runtime_error("Cesium cache budget exceeds 8 GiB");
      options.maximumCachedBytes = static_cast<int64_t>(cacheBytes);
      if (!std::isfinite(options.maximumScreenSpaceError) || options.maximumScreenSpaceError <= 0 ||
          options.maximumSimultaneousTileLoads < 1 || options.maximumSimultaneousTileLoads > 64 ||
          options.maximumCachedBytes < 1024*1024)
        throw std::runtime_error("Invalid Cesium LOD, concurrency or cache budget");
      options.preloadSiblings = false;
      options.preloadAncestors = false;
      options.forbidHoles = true;
      options.enableFogCulling = false;
      options.enableOcclusionCulling = false;
      options.enableLodTransitionPeriod = false;
      options.contentOptions.generateMissingNormalsSmooth = true;
      options.contentOptions.primitiveModeOptions.convertTriangleStrip = true;
      options.contentOptions.primitiveModeOptions.convertTriangleFan = true;
      cs::TilesetExternals externals{accessor_, resources_, *async_, credits_, logger};
      auto url = environment("GZ_CESIUM_TILESET_URL");
      if (url.empty()) url = sdf->Get<std::string>("tileset_url", "").first;
      const auto resolveUrl = [&](std::string value) {
        if (!value.starts_with("https://") && !value.starts_with("http://") && !value.starts_with("file://")) {
          gz::common::SystemPaths paths;
          paths.AddFilePaths(environment("GZ_SIM_RESOURCE_PATH"));
          const auto resolved = paths.FindFile(value);
          if (resolved.empty()) throw std::runtime_error("Cannot resolve Cesium tileset path");
          value = "file://" + std::filesystem::absolute(resolved).string();
        }
        return value;
      };
      if (!url.empty()) {
        tilesets_.push_back(std::make_unique<cs::Tileset>(externals, resolveUrl(url), options));
        source_ = "url";
      } else {
        const auto token = ionToken();
        std::vector<uint64_t> ids;
        auto layer = sdf->GetElementImpl("ion_asset_id");
        while (layer) { ids.push_back(layer->Get<uint64_t>()); layer = layer->GetNextElement("ion_asset_id"); }
        if (ids.empty()) ids.push_back(2275207);
        if (ids.size() > 4) throw std::runtime_error("At most four Cesium ion layers are supported");
        options.maximumCachedBytes /= static_cast<int64_t>(ids.size());
        source_ = "ion";
        for (const auto id : ids) {
          if (id == 0 || id > uint64_t(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error("ion_asset_id must be positive");
          tilesets_.push_back(std::make_unique<cs::Tileset>(externals, static_cast<int64_t>(id), token, options));
          source_ += ":" + std::to_string(id);
        }
      }
      if (sdf->HasElement("overlay_tileset_url")) {
        auto overlay = sdf->GetElementImpl("overlay_tileset_url");
        cs::TilesetOptions overlayOptions = options;
        overlayOptions.maximumCachedBytes = 64LL*1024*1024;
        while (overlay) {
          tilesets_.push_back(std::make_unique<cs::Tileset>(externals,
              resolveUrl(overlay->Get<std::string>()), overlayOptions));
          source_ += ":overlay";
          overlay = overlay->GetNextElement("overlay_tileset_url");
        }
      }
      const auto worldName = ecm.Component<gz::sim::components::Name>(world_)->Data();
      publisher_ = node_.Advertise<gz::msgs::StringMsg>("/world/"+worldName+"/cesium/status");
      gzmsg << "[gz_cesium] Streaming " << source_ << "; status: " << statusFile_ << '\n';
      if (waitForTerrain_) events_->Emit<gz::sim::events::Pause>(true);
    } catch (const std::exception& error) {
      error_ = redact(error.what());
      gzerr << "[gz_cesium] " << error_ << '\n';
      if (waitForTerrain_) events_->Emit<gz::sim::events::Pause>(true);
    }
  }

  void PreUpdate(const gz::sim::UpdateInfo& info, gz::sim::EntityComponentManager& ecm) override {
    if (!ready_ && waitForTerrain_ && !info.paused) events_->Emit<gz::sim::events::Pause>(true);
    const auto now = Clock::now();
    if (now-lastUpdate_ < std::chrono::duration<double>(interval_)) return;
    lastUpdate_ = now;
    if (tilesets_.empty() || !error_.empty()) { publish(); return; }
    try {
      std::map<gz::sim::Entity, glm::dvec3> anchors;
      ecm.Each<gz::sim::components::Model, gz::sim::components::Name, gz::sim::components::Static>(
          [&](const gz::sim::Entity& entity, const auto*, const auto* name, const auto* isStatic) {
        if (!isStatic->Data() && (followNames_.empty() || followNames_.contains(name->Data()))) {
          const auto p = gz::sim::worldPose(entity, ecm).Pos();
          anchors.emplace(entity, glm::dvec3(p.X(), p.Y(), p.Z()));
          initialClearance_.try_emplace(entity, p.Z());
        }
        return true;
      });
      std::vector<cs::ViewState> views;
      range_->anchors.clear();
      const auto addViews = [&](const glm::dvec3& anchor) {
        const glm::dvec3 position = glm::dvec3(enuToEcef_*glm::dvec4(anchor+glm::dvec3(0, 0, 2), 1));
        const auto rotation = glm::dmat3(enuToEcef_);
        range_->anchors.push_back(position);
        const std::array<glm::dvec3, 6> directions{{{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}}};
        for (const auto& direction : directions) {
          const glm::dvec3 up = std::abs(direction.z) > .5 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
          views.emplace_back(position, rotation*direction, rotation*up, glm::dvec2(512, 512),
                             glm::radians(100.), glm::radians(100.));
        }
        // A ground-facing view keeps contact terrain ahead of the vehicle in
        // the selection even when a robot camera is not looking at the road.
        views.emplace_back(glm::dvec3(enuToEcef_*glm::dvec4(anchor+glm::dvec3(0,0,40),1)),
            rotation*glm::dvec3(0,0,-1), rotation*glm::dvec3(0,1,0), glm::dvec2(1024,1024),
            glm::radians(100.), glm::radians(100.));
      };
      if (anchors.empty()) addViews(glm::dvec3(0));
      else for (const auto& [entity, anchor] : anchors) addViews(anchor);
      accessor_->tick();
      async_->dispatchMainThreadTasks();
      std::vector<cs::Tile::ConstPointer> selected;
      progress_ = 100.;
      for (auto& tileset : tilesets_) {
        const auto& update = tileset->updateViewGroup(tileset->getDefaultViewGroup(), views, interval_);
        selected.insert(selected.end(), update.tilesToRenderThisFrame.begin(), update.tilesToRenderThisFrame.end());
        async_->dispatchMainThreadTasks();
        tileset->loadTiles();
        progress_ = std::min(progress_, double(tileset->computeLoadProgress()));
      }
      std::erase_if(pins_, [&](const auto& pin) {
        return std::none_of(anchors.begin(), anchors.end(), [&](const auto& anchor) {
          return pin.second.mesh->near(anchor.second, radius_*1.5);
        });
      });
      std::map<const cs::Tile*, TileRecord> wanted;
      for (const auto& tile : selected) {
        const auto* content = tile->getContent().getRenderContent();
        if (!content || !content->getRenderResources()) continue;
        const auto mesh = static_cast<Resource*>(content->getRenderResources())->mesh;
        if (!mesh) continue;
        bool overlapsPin = false;
        for (const auto& [pointer, pin] : pins_) {
          if (pointer != tile.get() && (ancestor(pointer, tile.get()) || ancestor(tile.get(), pointer))) {
            overlapsPin = true;
            break;
          }
        }
        if (overlapsPin) continue;
        wanted.emplace(tile.get(), TileRecord{tile, mesh, gz::sim::kNullEntity});
      }
      std::set<std::uint64_t> selectedIds;
      for (const auto& [pointer, record] : wanted) selectedIds.insert(record.mesh->id);
      selectionStable_ = progress_ >= 99.99 && selectedIds == previousSelection_ ? selectionStable_+1 : 0;
      previousSelection_ = std::move(selectedIds);
      // Lock local geometry only after refinement settles. Both the visual
      // and collider are retained, avoiding overlapping parent/child surfaces.
      if (selectionStable_ >= 5) {
        for (const auto& [pointer, record] : wanted) {
          const auto extent = record.mesh->maximum-record.mesh->minimum;
          // A continental parent can enclose the rover in XY while its actual
          // content is kilometres away or below the local tangent plane. Such
          // a tile must remain free to refine, never become contact terrain.
          const bool local = extent.x < 250. && extent.y < 250. &&
              pointer->getGeometricError() <= 4.;
          if (local && std::any_of(anchors.begin(), anchors.end(), [&](const auto& anchor) {
                return record.mesh->near(anchor.second, radius_) &&
                    record.mesh->maximum.z >= anchor.second.z-5. &&
                    record.mesh->minimum.z <= anchor.second.z+5.;
              })) pins_.try_emplace(pointer, record);
        }
      }
      for (const auto& [pointer, record] : pins_) wanted.try_emplace(pointer, record);
      for (auto& [pointer, record] : wanted) {
        const auto found = active_.find(pointer);
        if (found != active_.end()) record.entity = found->second.entity;
        else {
          record.entity = createTile(*record.mesh);
          ++created_;
        }
      }
      for (const auto& [pointer, record] : active_)
        if (!wanted.contains(pointer)) { creator_->RequestRemoveEntity(record.entity); ++removed_; }
      active_ = std::move(wanted);
      resources_->collect();
      if (!ready_ && selectionStable_ >= 5 && !active_.empty() && resources_->failures == 0) {
        bool covered = true;
        std::map<gz::sim::Entity, double> heights;
        for (const auto& [entity, anchor] : anchors) {
          auto ground = heightAt(anchor.x, anchor.y);
          for (const auto& offset : {glm::dvec2(-1,-.7), glm::dvec2(-1,.7), glm::dvec2(1,-.7), glm::dvec2(1,.7)})
            covered &= heightAt(anchor.x+offset.x, anchor.y+offset.y).has_value();
          if (!ground) covered = false;
          else heights.emplace(entity, *ground);
        }
        stableFrames_ = covered ? stableFrames_+1 : 0;
        if (stableFrames_ >= 3) {
          if (placeOnGround_) for (const auto& [entity, height] : heights) {
            auto pose = gz::sim::worldPose(entity, ecm);
            pose.Pos().Z(height + initialClearance_.at(entity));
            gz::sim::Model(entity).SetWorldPoseCmd(ecm, pose);
          }
          ready_ = true;
          gzmsg << "[gz_cesium] Terrain READY: " << active_.size() << " mesh tiles; "
                << anchors.size() << " vehicles covered\n";
          if (waitForTerrain_) events_->Emit<gz::sim::events::Pause>(false);
        }
      }
      publish();
    } catch (const std::exception& error) {
      error_ = redact(error.what());
      gzerr << "[gz_cesium] " << error_ << '\n';
      if (waitForTerrain_) events_->Emit<gz::sim::events::Pause>(true);
      publish();
    }
  }

 private:
  struct TileRecord {
    cs::Tile::ConstPointer tile;
    std::shared_ptr<TileMesh> mesh;
    gz::sim::Entity entity;
  };
  static std::string environment(const char* key) {
    const auto* value = std::getenv(key);
    return value ? value : "";
  }
  static bool ancestor(const cs::Tile* parent, const cs::Tile* child) {
    for (auto* tile = child->getParent(); tile; tile = tile->getParent()) if (tile == parent) return true;
    return false;
  }
  std::optional<double> heightAt(double x, double y) const {
    std::optional<double> highest;
    for (const auto& [pointer, record] : active_) {
      const auto value = record.mesh->heightAt(x, y);
      if (value && (!highest || *value > *highest)) highest = value;
    }
    return highest;
  }
  gz::sim::Entity createTile(const TileMesh& mesh) {
    const auto model = tileModel(mesh);
    const auto entity = creator_->CreateEntities(&model);
    creator_->SetParent(entity, world_);
    return entity;
  }
  void publish() {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject();
    out.Key("schema"); out.Int(1);
    out.Key("ready"); out.Bool(ready_);
    out.Key("error"); out.String(error_.c_str());
    out.Key("source"); out.String(source_.c_str());
    out.Key("progress"); out.Double(progress_);
    out.Key("active_tiles"); out.Uint64(active_.size());
    out.Key("pinned_tiles"); out.Uint64(pins_.size());
    out.Key("created_tiles"); out.Uint64(created_);
    out.Key("removed_tiles"); out.Uint64(removed_);
    out.Key("stream_radius"); out.Double(range_ ? range_->radius : 0.);
    out.Key("layers"); out.Uint64(tilesets_.size());
    out.Key("preparation_failures"); out.Uint(resources_ ? unsigned(resources_->failures) : 0);
    std::uint64_t triangles=0, textures=0, exported=0, unlit=0;
    for (const auto& [pointer, record] : active_) {
      triangles += record.mesh->triangles.size();
      textures += record.mesh->textureBytes;
      exported += record.mesh->exportedBytes;
      for (const auto& visual : record.mesh->visuals) unlit += visual.unlit;
    }
    out.Key("active_triangles"); out.Uint64(triangles);
    out.Key("active_texture_bytes"); out.Uint64(textures);
    out.Key("active_exported_bytes"); out.Uint64(exported);
    out.Key("unlit_primitives"); out.Uint64(unlit);
    out.Key("geometry"); out.StartArray();
    for (const auto& [pointer, record] : active_) {
      out.StartObject();
      out.Key("id"); out.Uint64(record.mesh->id);
      out.Key("vertices"); out.Uint64(record.mesh->vertices.size());
      out.Key("triangles"); out.Uint64(record.mesh->triangles.size());
      out.Key("textures"); out.Uint64(record.mesh->textureCount);
      out.Key("uri"); out.String(record.mesh->uri.c_str());
      out.Key("visuals"); out.StartArray();
      for (const auto& visual : record.mesh->visuals) {
        out.StartObject();
        out.Key("uri"); out.String(visual.uri.c_str());
        out.Key("unlit"); out.Bool(visual.unlit);
        out.EndObject();
      }
      out.EndArray();
      out.Key("pinned"); out.Bool(pins_.contains(pointer));
      out.Key("geometric_error"); out.Double(std::isfinite(pointer->getGeometricError()) ? pointer->getGeometricError() : -1.);
      out.Key("children"); out.Uint64(pointer->getChildren().size());
      out.Key("minimum"); out.StartArray(); for (int i=0;i<3;++i) out.Double(record.mesh->minimum[i]); out.EndArray();
      out.Key("maximum"); out.StartArray(); for (int i=0;i<3;++i) out.Double(record.mesh->maximum[i]); out.EndArray();
      out.EndObject();
    }
    out.EndArray();
    out.Key("credits"); out.StartArray();
    if (credits_) for (const auto credit : credits_->getSnapshot().currentCredits) out.String(credits_->getHtml(credit).c_str());
    out.EndArray();
    out.EndObject();
    const auto text = std::string(buffer.GetString(), buffer.GetSize());
    if (publisher_) { gz::msgs::StringMsg message; message.set_data(text); publisher_.Publish(message); }
    if (!statusFile_.empty()) {
      const auto target = std::filesystem::path(statusFile_);
      std::filesystem::create_directories(target.parent_path());
      const auto temporary = statusFile_+".tmp";
      { std::ofstream output(temporary); output << text << '\n'; }
      std::filesystem::rename(temporary, target);
    }
  }
  gz::sim::Entity world_{};
  gz::sim::EventManager* events_{};
  std::unique_ptr<gz::sim::SdfEntityCreator> creator_;
  std::shared_ptr<TaskPool> pool_;
  std::unique_ptr<CesiumAsync::AsyncSystem> async_;
  std::shared_ptr<CesiumCurl::CurlAssetAccessor> accessor_;
  std::shared_ptr<Resources> resources_;
  std::shared_ptr<RangeExcluder> range_;
  std::shared_ptr<CesiumUtility::CreditSystem> credits_;
  std::vector<std::unique_ptr<cs::Tileset>> tilesets_;
  std::map<const cs::Tile*, TileRecord> active_, pins_;
  std::map<gz::sim::Entity, double> initialClearance_;
  std::set<std::string> followNames_;
  std::set<std::uint64_t> previousSelection_;
  glm::dmat4 enuToEcef_{1.};
  std::filesystem::path assetDirectory_;
  std::string source_, error_, statusFile_;
  gz::transport::Node node_;
  gz::transport::Node::Publisher publisher_;
  Clock::time_point lastUpdate_{};
  double radius_{15.}, interval_{.2}, progress_{};
  bool waitForTerrain_{true}, placeOnGround_{true}, ready_{};
  unsigned stableFrames_{}, selectionStable_{};
  std::uint64_t created_{}, removed_{};
};
}  // namespace gz_cesium

GZ_ADD_PLUGIN(gz_cesium::CesiumSystem, gz::sim::System,
              gz_cesium::CesiumSystem::ISystemConfigure,
              gz_cesium::CesiumSystem::ISystemPreUpdate)
GZ_ADD_PLUGIN_ALIAS(gz_cesium::CesiumSystem, "gz_cesium::CesiumSystem")
