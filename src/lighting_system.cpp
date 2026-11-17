// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/lighting.hpp"
#include <gz/common/Console.hh>
#include <gz/plugin/Register.hh>
#include <gz/sim/System.hh>
#include <gz/sim/EventManager.hh>
#include <gz/sim/rendering/Events.hh>
#include <sdf/Element.hh>
#include <gz/transport/Node.hh>
#include <gz/msgs/boolean.pb.h>
#include <gz/msgs/empty.pb.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace gz_cesium {
class LightingSystem final : public gz::sim::System, public gz::sim::ISystemConfigure {
 public:
  ~LightingSystem() override {
    render_.reset(); teardown_.reset();
    std::lock_guard lock(mutex_); lighting_.reset();
  }
  void Configure(const gz::sim::Entity&,const std::shared_ptr<const sdf::Element>& input,
      gz::sim::EntityComponentManager&,gz::sim::EventManager& events) override {
    try {
      auto sdf=input->Clone();
      LightingOptions options;
      options.enabled=sdf->Get<bool>("enabled",true).first;
      const auto resolution=sdf->Get<gz::math::Vector3d>("resolution",{128,128,64}).first;
      for (int i=0;i<3;++i) {
        if (!std::isfinite(resolution[i]) || resolution[i]<16 || resolution[i]>256 || std::floor(resolution[i])!=resolution[i])
          throw std::runtime_error("GI resolution must have finite integer axes from 16 to 256");
        options.resolution[i]=static_cast<std::uint32_t>(resolution[i]);
      }
      options.bounces=sdf->Get<unsigned>("bounce_count",1).first;
      options.rebuildInterval=sdf->Get<double>("rebuild_interval",2.).first;
      options.settleTime=sdf->Get<double>("settle_time",.5).first;
      if (const auto* override=std::getenv("GZ_CESIUM_GI")) options.enabled=std::string(override)!="off";
      enabled_=options.enabled;
      publisher_=node_.Advertise<gz::msgs::Boolean>("/cesium/lighting/enabled");
      node_.Advertise("/cesium/lighting/set_enabled",&LightingSystem::setEnabled,this);
      node_.Advertise("/cesium/lighting/get_enabled",&LightingSystem::getEnabled,this);
      std::string status;
      if (const auto* path=std::getenv("GZ_CESIUM_LIGHTING_STATUS_FILE")) status=path;
      else if (const auto* path=std::getenv("GZ_CESIUM_STATUS_FILE")) status=std::string(path)+".lighting.json";
      lighting_=std::make_unique<Lighting>(options,"sensor",status);
      render_=events.Connect<gz::sim::events::PreRender>([this] {
        std::lock_guard lock(mutex_); if (lighting_) lighting_->update(enabled_);
      });
      teardown_=events.Connect<gz::sim::events::RenderTeardown>([this] {
        std::lock_guard lock(mutex_); if (lighting_) lighting_->reset();
      });
    } catch (const std::exception& error) { gzerr << "[gz_cesium] GI configuration: " << error.what() << '\n'; }
  }
 private:
  bool setEnabled(const gz::msgs::Boolean& request,gz::msgs::Boolean& response) {
    enabled_=request.data(); publisher_.Publish(request); response.set_data(true); return true;
  }
  bool getEnabled(const gz::msgs::Empty&,gz::msgs::Boolean& response) {
    response.set_data(enabled_); return true;
  }
  gz::transport::Node node_;
  gz::transport::Node::Publisher publisher_;
  std::unique_ptr<Lighting> lighting_;
  gz::common::ConnectionPtr render_,teardown_;
  std::mutex mutex_;
  std::atomic<bool> enabled_{true};
};
}
GZ_ADD_PLUGIN(gz_cesium::LightingSystem,gz::sim::System,gz_cesium::LightingSystem::ISystemConfigure)
