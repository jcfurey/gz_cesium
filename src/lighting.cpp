// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/lighting.hpp"
#include <gz/common/Console.hh>
#include <gz/rendering/GlobalIlluminationVct.hh>
#include <gz/rendering/RenderingIface.hh>
#include <gz/rendering/RenderEngine.hh>
#include <gz/rendering/Scene.hh>
#include <gz/rendering/Visual.hh>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace gz_cesium {
void LightingOptions::validate() const {
  for (const auto size : resolution)
    if (size<16 || size>256 || (size&(size-1)))
      throw std::runtime_error("GI resolution must use powers of two from 16 to 256");
  if (bounces>4 || !std::isfinite(rebuildInterval) || rebuildInterval<.5 || rebuildInterval>30 ||
      !std::isfinite(settleTime) || settleTime<.1 || settleTime>5)
    throw std::runtime_error("Invalid GI bounce/rebuild settings");
}
Lighting::Lighting(LightingOptions options, std::string label, std::string status)
    : options_(options), label_(std::move(label)), status_(std::move(status)) { options_.validate(); }
Lighting::~Lighting() = default;
void Lighting::reset() {
  if (auto scene=scene_.lock(); scene && gi_) scene->SetActiveGlobalIllumination(nullptr);
  gi_.reset(); scene_.reset(); active_=false;
}
void Lighting::status(bool active, std::size_t visuals) {
  if (status_.empty() || Clock::now()-reported_<std::chrono::seconds(1)) return;
  reported_=Clock::now();
  std::ostringstream json;
  json << "{\"schema\":1,\"active\":" << (active?"true":"false")
       << ",\"failed\":" << (failed_?"true":"false") << ",\"builds\":" << builds_
       << ",\"geometry_signature\":" << signature_ << ",\"built_signature\":" << builtSignature_
       << ",\"visuals\":" << visuals << ",\"last_build_ms\":" << buildMs_
       << ",\"static_bounce_visuals\":" << bounceVisuals_
       << ",\"resolution\":[" << options_.resolution[0] << ',' << options_.resolution[1] << ',' << options_.resolution[2]
       << "],\"bounces\":" << options_.bounces << ",\"static_bounce_geometry\":true}\n";
  try {
    const std::filesystem::path path(status_);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    { std::ofstream out(status_+".tmp"); out << json.str(); }
    std::filesystem::rename(status_+".tmp",status_);
  } catch (const std::exception& error) { gzwarn << "[gz_cesium] GI status: " << error.what() << '\n'; }
}
void Lighting::update(bool enabled) {
  if (failed_) { status(false,0); return; }
  auto* engine=gz::rendering::engine("ogre2");
  if (!engine || engine->SceneCount()==0) return;
  auto scene=engine->SceneByIndex(0);
  if (!scene) return;
  scene_=scene;
  std::uint64_t signature=0;
  std::size_t visuals=0;
  bounceVisuals_=0;
  for (unsigned i=0; i<scene->VisualCount(); ++i) {
    const auto visual=scene->VisualByIndex(i);
    if (!visual) continue;
    // Gazebo's SceneManager currently places SDF-static models in Ogre's
    // dynamic bucket too. Promote only our explicitly static tile hierarchy;
    // otherwise a STATIC_VISUALS-only voxelization contains no terrain.
    if (visual->Name().find("cesium_tile_")!=std::string::npos && !visual->Static()) visual->SetStatic(true);
    if (!visual->GeometryCount()) continue;
    if (visual->Static()) ++bounceVisuals_;
    ++visuals;
    // IDs detect replacements even when the total count stays constant. Pose
    // changes matter for static bounce geometry; robot motion needs no rebuild.
    std::uint64_t hash=std::hash<std::string>{}(visual->Name());
    hash ^= std::uint64_t(visual->Id())*1099511628211ULL;
    hash ^= visual->GeometryCount()*16777619ULL;
    if (visual->Static()) {
      const auto pose=visual->WorldPose();
      for (const auto value : {pose.Pos().X(),pose.Pos().Y(),pose.Pos().Z(),
                              pose.Rot().W(),pose.Rot().X(),pose.Rot().Y(),pose.Rot().Z()})
        hash ^= std::hash<double>{}(value)+0x9e3779b97f4a7c15ULL+(hash<<6)+(hash>>2);
    }
    signature ^= hash;
  }
  const auto now=Clock::now();
  if (signature!=signature_) {
    if (signature_==builtSignature_) dirty_=now;
    signature_=signature; changed_=now;
  }
  if (!enabled) {
    if (active_) { scene->SetActiveGlobalIllumination(nullptr); active_=false; }
    status(false,visuals); return;
  }
  const bool settled=std::chrono::duration<double>(now-changed_).count()>=options_.settleTime;
  const bool overdue=now-dirty_>=std::chrono::seconds(4);
  const bool interval=std::chrono::duration<double>(now-built_).count()>=options_.rebuildInterval;
  if (visuals && interval && (settled || overdue) && (signature_!=builtSignature_ || !active_)) {
    try {
      if (!gi_) {
        gi_=scene->CreateGlobalIlluminationVct();
        if (!gi_) throw std::runtime_error("Ogre2 VCT is unavailable");
        gi_->SetResolution(options_.resolution.data());
        const std::uint32_t octants[3]={1,1,1};
        gi_->SetOctantCount(octants);
        gi_->SetBounceCount(options_.bounces);
        gi_->SetHighQuality(true); gi_->SetAnisotropic(true);
        gi_->SetThinWallCounter(1.5f); gi_->SetConserveMemory(false);
        gi_->SetParticipatingVisuals(gz::rendering::GlobalIlluminationBase::STATIC_VISUALS);
      }
      const auto start=Clock::now();
      gi_->Build();
      scene->SetActiveGlobalIllumination(gi_);
      buildMs_=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
      built_=Clock::now(); builtSignature_=signature_; active_=true; ++builds_;
      gzmsg << "[gz_cesium] " << label_ << " GI build " << builds_ << ": " << visuals
            << " visuals, " << buildMs_ << " ms\n";
    } catch (const std::exception& error) {
      gzerr << "[gz_cesium] " << label_ << " GI failed: " << error.what() << '\n';
      reset(); failed_=true;
    }
  }
  status(active_,visuals);
}
}
