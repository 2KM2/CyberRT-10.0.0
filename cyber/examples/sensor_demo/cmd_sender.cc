#include "cyber/cyber.h"
#include "cyber/time/rate.h"
#include "cyber/time/time.h"
#include "cyber/examples/sensor_demo/sensor_demo.pb.h"

using apollo::cyber::examples::sensor_demo::ControlCommand;

int main(int argc, char* argv[]) {
  apollo::cyber::Init(argv[0]);
  auto node = apollo::cyber::CreateNode("cmd_sender");
  auto writer = node->CreateWriter<ControlCommand>("channel/control_cmd");

  apollo::cyber::Rate rate(1.0);
  uint64_t seq = 0;
  while (apollo::cyber::OK()) {
    auto cmd = std::make_shared<ControlCommand>();
    cmd->set_timestamp(apollo::cyber::Time::Now().ToNanosecond());
    cmd->set_cmd("set_speed");
    cmd->set_value(seq * 0.5);
    writer->Write(cmd);
    AINFO << "Sent command seq=" << seq++;
    rate.Sleep();
  }
  return 0;
}
