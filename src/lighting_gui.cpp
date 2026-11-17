// SPDX-License-Identifier: Apache-2.0
#include "gz_cesium/lighting.hpp"
#include <gz/gui/Application.hh>
#include <gz/gui/GuiEvents.hh>
#include <gz/gui/MainWindow.hh>
#include <gz/gui/Plugin.hh>
#include <gz/plugin/Register.hh>
#include <gz/transport/Node.hh>
#include <gz/msgs/boolean.pb.h>
#include <gz/msgs/empty.pb.h>
#include <QQuickWindow>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace gz_cesium {
class CesiumLighting final : public gz::gui::Plugin {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ Enabled WRITE SetEnabled NOTIFY EnabledChanged)
 public:
  ~CesiumLighting() override {
    std::lock_guard lock(mutex_); lighting_.reset();
  }
  bool Enabled() const { return enabled_; }
  void SetEnabled(bool value) {
    enabled_=value; emit EnabledChanged();
    gz::msgs::Boolean request; request.set_data(value);
    node_.Request<gz::msgs::Boolean,gz::msgs::Boolean>("/cesium/lighting/set_enabled",request,[](const gz::msgs::Boolean&,bool){});
  }
  void LoadConfig(const tinyxml2::XMLElement* element) override {
    if (this->title.empty()) this->title="Lighting";
    LightingOptions options;
    if (const auto* value=element->FirstChildElement("enabled")) value->QueryBoolText(&options.enabled);
    if (const auto* value=element->FirstChildElement("resolution"); value && value->GetText()) {
      std::istringstream text(value->GetText());
      text >> options.resolution[0] >> options.resolution[1] >> options.resolution[2];
    }
    if (const auto* value=element->FirstChildElement("bounce_count")) value->QueryUnsignedText(&options.bounces);
    if (const auto* value=element->FirstChildElement("rebuild_interval")) value->QueryDoubleText(&options.rebuildInterval);
    if (const auto* value=element->FirstChildElement("settle_time")) value->QueryDoubleText(&options.settleTime);
    if (const auto* override=std::getenv("GZ_CESIUM_GI")) options.enabled=std::string(override)!="off";
    std::string status;
    if (const auto* path=std::getenv("GZ_CESIUM_GUI_LIGHTING_STATUS_FILE")) status=path;
    enabled_=options.enabled;
    node_.Subscribe("/cesium/lighting/enabled",&CesiumLighting::receive,this);
    node_.Request("/cesium/lighting/get_enabled",gz::msgs::Empty{},&CesiumLighting::onGetEnabled,this);
    lighting_=std::make_unique<Lighting>(options,"GUI",status);
    auto* window=gz::gui::App()->findChild<gz::gui::MainWindow*>();
    if (!window) return;
    window->installEventFilter(this);
    // Free GL resources with a current context during Qt's renderer teardown.
    QObject::connect(window->QuickWindow(),&QQuickWindow::sceneGraphInvalidated,this,[this] {
      std::lock_guard lock(mutex_); if (lighting_) lighting_->reset();
    },Qt::DirectConnection);
  }
  bool eventFilter(QObject* object,QEvent* event) override {
    if (event->type()==gz::gui::events::Render::kType) {
      std::lock_guard lock(mutex_);
      if (lighting_) lighting_->update(enabled_);
    }
    return QObject::eventFilter(object,event);
  }
 signals:
  void EnabledChanged();
 private:
  void receive(const gz::msgs::Boolean& value) { enabled_=value.data(); emit EnabledChanged(); }
  void onGetEnabled(const gz::msgs::Boolean& value,bool success) { if (success) receive(value); }
  gz::transport::Node node_;
  std::atomic<bool> enabled_{true};
  std::unique_ptr<Lighting> lighting_;
  std::mutex mutex_;
};
}
GZ_ADD_PLUGIN(gz_cesium::CesiumLighting,gz::gui::Plugin)
#include "lighting_gui.moc"
