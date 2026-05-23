#pragma once
#include <atomic>
#include "cyber/component/component.h"
#include "cyber/examples/sensor_demo/sensor_demo.pb.h"

using apollo::cyber::Component;
using apollo::cyber::examples::sensor_demo::SensorData;
using apollo::cyber::examples::sensor_demo::ControlCommand;

// 控制组件：订阅传感器数据，同时接收外部指令
class ControlComponent : public Component<SensorData> {
 public:
  bool Init() override;
  bool Proc(const std::shared_ptr<SensorData>& sensor_msg) override;

 private:
  void OnCommand(const std::shared_ptr<ControlCommand>& cmd);

  std::shared_ptr<apollo::cyber::Reader<ControlCommand>> cmd_reader_;
  std::atomic<std::string*> current_cmd_{nullptr};
};
CYBER_REGISTER_COMPONENT(ControlComponent)
