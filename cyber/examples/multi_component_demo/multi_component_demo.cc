#include "cyber/examples/multi_component_demo/multi_component_demo.h"

// ── SensorComponent ──────────────────────────────────────────────────────────

bool SensorComponent::Init() {
  writer_ = node_->CreateWriter<Driver>("/demo/sensor");
  return true;
}

bool SensorComponent::Proc() {
  auto msg = std::make_shared<Driver>();
  msg->set_msg_id(id_++);
  msg->set_content("sensor_data");
  writer_->Write(msg);
  AINFO << "[Sensor] published id=" << msg->msg_id();
  return true;
}

// ── ProcessorComponent ───────────────────────────────────────────────────────

bool ProcessorComponent::Init() {
  writer_ = node_->CreateWriter<Driver>("/demo/result");
  // 指令交互：通过 Service 控制 DisplayComponent 的开关
  ctrl_client_ = node_->CreateClient<Driver, Driver>("/demo/display_ctrl");
  return true;
}

bool ProcessorComponent::Proc(const std::shared_ptr<Driver>& msg) {
  ++count_;

  // 每处理5条消息，发一次指令切换 Display 的开关状态
  if (count_ % 5 == 0) {
    auto req = std::make_shared<Driver>();
    req->set_content(count_ % 10 == 0 ? "disable" : "enable");
    auto resp = ctrl_client_->SendRequest(req);
    if (resp) {
      AINFO << "[Processor] ctrl resp: " << resp->content();
    }
  }

  auto out = std::make_shared<Driver>();
  out->set_msg_id(msg->msg_id());
  out->set_content("processed[" + std::to_string(count_) + "]:" + msg->content());
  writer_->Write(out);
  AINFO << "[Processor] count=" << count_ << " id=" << out->msg_id();
  return true;
}

// ── DisplayComponent ─────────────────────────────────────────────────────────

bool DisplayComponent::Init() {
  // 指令交互：提供 Service，接受来自 ProcessorComponent 的开关指令
  ctrl_service_ = node_->CreateService<Driver, Driver>(
      "/demo/display_ctrl",
      [this](const std::shared_ptr<Driver>& req, std::shared_ptr<Driver>& resp) {
        enabled_ = (req->content() != "disable");
        resp->set_content(enabled_ ? "display:on" : "display:off");
        AINFO << "[Display] ctrl received: " << req->content()
              << " -> enabled=" << enabled_;
      });
  return true;
}

bool DisplayComponent::Proc(const std::shared_ptr<Driver>& msg) {
  if (!enabled_) return true;
  AINFO << "[Display] id=" << msg->msg_id() << " " << msg->content();
  return true;
}
