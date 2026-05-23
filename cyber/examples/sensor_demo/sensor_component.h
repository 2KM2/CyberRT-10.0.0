#pragma once
#include <atomic>
#include "cyber/component/timer_component.h"
#include "cyber/examples/sensor_demo/sensor_demo.pb.h"

using apollo::cyber::TimerComponent;
using apollo::cyber::examples::sensor_demo::SensorData;
using apollo::cyber::examples::sensor_demo::ControlCommand;

class SensorComponent : public TimerComponent {
 public:
  bool Init() override;
  bool Proc() override;  // 定时触发：采集 + 业务逻辑 + 发布

 private:
  void OnCommand(const std::shared_ptr<ControlCommand>& cmd);

  std::shared_ptr<apollo::cyber::Writer<SensorData>> sensor_writer_;
  std::shared_ptr<apollo::cyber::Reader<ControlCommand>> cmd_reader_;

  std::atomic<double> target_value_{0.0};  // 来自指令，线程安全
};
CYBER_REGISTER_COMPONENT(SensorComponent)
