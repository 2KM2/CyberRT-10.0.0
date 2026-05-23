#include "cyber/examples/sensor_demo/control_component.h"
#include "cyber/time/time.h"

bool ControlComponent::Init() {
  // 额外订阅指令 channel（不触发 Proc，独立回调）
  cmd_reader_ = node_->CreateReader<ControlCommand>(
      "channel/control_cmd",
      [this](const std::shared_ptr<ControlCommand>& cmd) {
        OnCommand(cmd);
      });
  AINFO << "ControlComponent init";
  return true;
}

bool ControlComponent::Proc(const std::shared_ptr<SensorData>& sensor_msg) {
  AINFO << "Got sensor seq=" << sensor_msg->seq()
        << " ts=" << sensor_msg->timestamp();
  return true;
}

void ControlComponent::OnCommand(const std::shared_ptr<ControlCommand>& cmd) {
  AINFO << "Got command: " << cmd->cmd() << " value=" << cmd->value();
}
