#pragma once

#include "cyber/component/component.h"
#include "cyber/component/timer_component.h"
#include "cyber/examples/proto/examples.pb.h"
#include "cyber/service/service.h"
#include "cyber/client/client.h"

using apollo::cyber::Component;
using apollo::cyber::TimerComponent;
using apollo::cyber::Writer;
using apollo::cyber::Service;
using apollo::cyber::Client;
using apollo::cyber::examples::proto::Driver;

// 定时发布传感器数据
class SensorComponent : public TimerComponent {
 public:
  bool Init() override;
  bool Proc() override;

 private:
  std::shared_ptr<Writer<Driver>> writer_;
  int id_ = 0;
};

// 订阅传感器数据，处理后发布结果；每5条通过 Service 切换 Display 开关
class ProcessorComponent : public Component<Driver> {
 public:
  bool Init() override;
  bool Proc(const std::shared_ptr<Driver>& msg) override;

 private:
  std::shared_ptr<Writer<Driver>> writer_;
  std::shared_ptr<Client<Driver, Driver>> ctrl_client_;
  int count_ = 0;
};

// 订阅处理结果并打印；提供 Service 接受开关指令
class DisplayComponent : public Component<Driver> {
 public:
  bool Init() override;
  bool Proc(const std::shared_ptr<Driver>& msg) override;

 private:
  std::shared_ptr<Service<Driver, Driver>> ctrl_service_;
  bool enabled_ = true;
};

CYBER_REGISTER_COMPONENT(SensorComponent)
CYBER_REGISTER_COMPONENT(ProcessorComponent)
CYBER_REGISTER_COMPONENT(DisplayComponent)
