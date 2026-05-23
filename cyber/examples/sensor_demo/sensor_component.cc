#include "cyber/examples/sensor_demo/sensor_component.h"
#include "cyber/time/time.h"

bool SensorComponent::Init() {
  sensor_writer_ = node_->CreateWriter<SensorData>("channel/sensor_data");
  cmd_reader_ = node_->CreateReader<ControlCommand>(
      "channel/control_cmd",
      [this](const std::shared_ptr<ControlCommand>& cmd) {
        OnCommand(cmd);
      });
  return true;
}

bool SensorComponent::Proc() {
  // 1. 采集传感器数据
  auto msg = std::make_shared<SensorData>();
  msg->set_timestamp(apollo::cyber::Time::Now().ToNanosecond());
  msg->set_data("raw_sensor_bytes");
  sensor_writer_->Write(msg);

  // 2. 自身业务逻辑（天黑检测等）
  // bool is_dark = DetectDarkness(msg);
  // ...

  return true;
}

void SensorComponent::OnCommand(const std::shared_ptr<ControlCommand>& cmd) {
  // 指令回调，与 Proc() 并发，用 atomic 保护共享状态
  target_value_.store(cmd->value());
  AINFO << "Command: " << cmd->cmd() << " = " << cmd->value();
}
