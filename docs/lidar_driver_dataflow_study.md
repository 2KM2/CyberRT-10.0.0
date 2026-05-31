# drivers/lidar 详解：从硬件收包到发布 Channel

本文聚焦 `modules/drivers/lidar` 这一层最核心的问题：

> 激光雷达硬件数据是怎样进入 Apollo / CyberRT，经过哪些处理步骤，最后发布到 channel 的？

这份文档不追求一次覆盖所有厂商型号，而是先抓最典型、最能代表整体架构的主链路。当前仓库里最完整、最容易顺着看下去的是 Velodyne 这条链，因此下面主要以 Velodyne 为主线说明，同时补充 `drivers/lidar` 目录级别的通用结构。

---

## 1. 先看结论：这不是“驱动直接发点云”的单段链路

`drivers/lidar` 的真实链路并不是：

`硬件 -> 点云 channel`

而更接近：

`硬件 UDP 包 -> 原始 Scan 消息 -> PointCloud 消息 -> 补偿/融合后的点云`

也就是说，它至少分成两段：

1. **采集段**：从网口收雷达原始包，拼成一帧 `Scan`
2. **转换段**：把 `Scan` 解析成 `PointCloud`

如果再把后处理算上，还会继续串：

3. **补偿段**：对点云做时间补偿、姿态补偿
4. **融合段**：多雷达主副帧融合

所以第一次看 `drivers/lidar` 时，最重要的认知是：

> 驱动层先发布原始扫描消息，点云通常是下一阶段组件再生成的。

---

## 2. `drivers/lidar` 的整体目录结构

从目录组织上看，这里不是一个单一驱动，而是一套“公共骨架 + 厂商实现”的结构。

### 2.1 顶层公共部分

重点目录：

- `modules/drivers/lidar/common/`
- `modules/drivers/lidar/proto/`
- `modules/drivers/lidar/lidar_driver_component.*`
- `modules/drivers/lidar/dag/`
- `modules/drivers/lidar/launch/`

这部分主要负责：

- 定义总配置格式
- 定义通用 component/driver 抽象
- 定义工厂，按品牌选择具体驱动
- 定义进程和组件在 `launch/dag` 里的组织方式

### 2.2 厂商和型号实现

重点目录：

- `modules/drivers/lidar/velodyne/`
- `modules/drivers/lidar/hslidar/`
- `modules/drivers/lidar/lslidar/`
- `modules/drivers/lidar/rslidar/`
- `modules/drivers/lidar/livox/`
- `modules/drivers/lidar/vanjeelidar/`

这些目录一般都会再细分成：

- `driver/`：收包、组帧
- `parser/`：解码和点云生成
- `conf/`：型号配置、端口、channel、标定文件路径
- `dag/` / `launch/`：进程入口

换句话说，Apollo 对 lidar 的建模，不是“每家厂商一个单类”，而是比较稳定地拆成：

- 数据采集
- 原始消息输出
- 点云转换
- 后续补偿/融合

---

## 3. 先从 `launch` 和 `dag` 看运行组织

### 3.1 进程入口

顶层入口是 [driver.launch](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/launch/driver.launch:1)。

这个 `launch` 对应的进程名是 `lidar_driver`，并加载 [velodyne_lidar.dag](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/dag/velodyne_lidar.dag:1)。

### 3.2 `velodyne_lidar.dag` 里串了什么

这个 DAG 里有三段关键模块：

1. `LidarDriverComponent`
2. `VelodyneConvertComponent`
3. `CompensatorComponent`

其中最关键的是前两个。

#### 第一段：驱动组件

见 [velodyne_lidar.dag](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/dag/velodyne_lidar.dag:2)

- 加载库：`liblidar_driver_component.so`
- 组件类：`LidarDriverComponent`
- 配置文件：`/apollo/modules/drivers/lidar/conf/lidar_config.pb.txt`

它负责启动具体 lidar driver。

#### 第二段：转换组件

见 [velodyne_lidar.dag](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/dag/velodyne_lidar.dag:14)

- 加载库：`libvelodyne_convert_component.so`
- 组件类：`VelodyneConvertComponent`
- 配置文件：`velodyne16_front_center_conf.pb.txt`
- reader 输入：`/apollo/sensor/lidar16/Scan`

它负责把原始 `Scan` 转成 `PointCloud2`。

#### 第三段：补偿组件

见 [velodyne_lidar.dag](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/dag/velodyne_lidar.dag:27)

- reader 输入：`/apollo/sensor/lidar16/PointCloud2`

说明补偿是在点云生成之后再做，不在原始驱动采集阶段做。

所以从 DAG 层面已经可以得到第一版完整链路：

`LidarDriverComponent -> /apollo/sensor/lidar16/Scan -> VelodyneConvertComponent -> /apollo/sensor/lidar16/PointCloud2 -> CompensatorComponent`

---

## 4. 顶层驱动组件：`LidarDriverComponent` 只负责装配

顶层入口类在 [lidar_driver_component.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/lidar_driver_component.cc:25)。

它的职责非常简单，不直接读硬件：

1. 读取总配置 `lidar_config.pb.txt`
2. 创建 Cyber 节点 `drivers_lidar`
3. 让 `LidarDriverFactory` 注册可用品牌驱动
4. 根据配置里的 `brand` 创建具体的 `LidarDriver`
5. 调用具体 driver 的 `Init()`

也就是说：

> `LidarDriverComponent` 是运行入口，不是具体采集实现。

这一层解决的是“如何把一个具体厂商驱动挂进 CyberRT component 框架”，不是“如何收雷达包”。

---

## 5. 工厂层：按品牌选择具体实现

### 5.1 总配置格式

总配置格式定义在 [config.proto](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/proto/config.proto:9)。

它的结构很直接：

- `brand`
- `hesai`
- `velodyne`

品牌枚举在 [lidar_parameter.proto](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/proto/lidar_parameter.proto:5)。

### 5.2 工厂注册逻辑

工厂在 [lidar_driver_factory.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/common/driver_factory/lidar_driver_factory.cc:30)。

这里当前实际注册启用的是：

- `VELODYNE`

而 `HESAI`、`ROBOSENSE` 的注册逻辑是注释状态。

这意味着：

- 顶层框架支持多品牌抽象
- 但当前这份代码里真正连通的主路径是 Velodyne

这也是为什么要以 Velodyne 为主线分析。它不是随便挑的，而是当前实现里最明确的一条完整链路。

### 5.3 一个值得注意的工程细节

在 [lidar_config.pb.txt](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/conf/lidar_config.pb.txt:1) 里，目前第一行是：

`brand: ROBOSENSE`

但工厂里 Robosense 注册被注释掉了。这说明：

- 这份默认配置不一定和当前编译/运行实现严格匹配
- 真正运行时可能还依赖其他 `dag/conf` 组合或具体厂商分支

学习时要注意区分：

- “框架支持的抽象”
- “当前代码真正启用的实现”

---

## 6. Velodyne 典型链路：从硬件收包到发布 `Scan`

现在进入最关键的主链。

### 6.1 具体 driver 类是谁

Velodyne 的核心 driver 在 [driver.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.h:44)。

这里有两个主要类：

- `VelodyneDriver`
- `Velodyne64Driver`

工厂 `VelodyneDriverFactory::CreateDriver()` 会根据不同型号选择不同 driver，并设置不同的 `packet_rate`。
见 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:232)

这说明型号差异首先影响的是：

- 每秒包率
- 每帧收多少包
- 某些特定型号的同步/角度处理逻辑

### 6.2 `VelodyneDriver::Init()` 做了什么

关键初始化在 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:43)。

它做了几件关键事：

1. 根据 `rpm` 和 `packet_rate_` 计算 `npackets`
2. 创建 `SocketInput` 作为 firing 数据输入
3. 创建 `SocketInput` 作为 positioning 数据输入
4. 创建 `VelodyneScan` writer，输出到 `scan_channel`
5. 用配置里的端口初始化两个 UDP socket
6. 起两个线程：
   - `positioning_thread_`
   - `poll_thread_`

这已经把驱动设计暴露得很清楚了：

- 一条线程负责定位时间
- 一条线程负责激光 firing 包
- 两类包合起来组成后续一帧 `VelodyneScan`

### 6.3 `scan_channel` 是在哪里定义的

例如在 [velodyne16_front_center_conf.pb.txt](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/conf/velodyne16_front_center_conf.pb.txt:2) 中：

- `scan_channel: "/apollo/sensor/lidar16/front/center/Scan"`
- `convert_channel_name: "/apollo/sensor/lidar16/front/center/PointCloud2"`

这说明配置已经把“原始 scan channel”和“转换后点云 channel”明确拆开了。

---

## 7. 最底层硬件接入：`SocketInput` 直接监听 UDP 端口

### 7.1 不是 SDK 回调，而是原始 socket 收包

Velodyne 最底层收包在 [socket_input.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/socket_input.cc:56)。

`SocketInput::init()` 的关键动作是：

1. `socket(AF_INET, SOCK_DGRAM, 0)` 创建 UDP socket
2. `bind()` 到指定端口
3. `fcntl(... O_NONBLOCK | FASYNC)` 设成非阻塞

所以 Apollo 这层对 Velodyne 的接入本质上是：

> 在本机 UDP 端口上直接接收雷达设备发送过来的原始报文。

### 7.2 两种 UDP 包

驱动区分两类包：

1. **firing data packet**
大小定义在 [input.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/input.h:32)

- `FIRING_DATA_PACKET_SIZE = 1206`

2. **positioning data packet**
同文件定义：

- `POSITIONING_DATA_PACKET_SIZE = 512`

所以硬件进来的不是“点云对象”，而是固定长度的 UDP 二进制报文。

### 7.3 firing 数据包怎么读

接口是 [get_firing_data_packet](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/socket_input.cc:93)。

流程是：

1. 先 `poll()` 等待端口可读
2. 再 `recvfrom()` 读 1206 字节
3. 如果字节数正确，就把原始 bytes 直接塞进 `VelodynePacket::data`
4. 同时记录一个中间时间戳 `stamp`

这里有一个很重要的事实：

> 这一层还完全没有做点坐标计算，它只是把原始以太网/UDP 有效载荷搬进 protobuf 消息。

### 7.4 positioning 数据包怎么读

接口是 [get_positioning_data_packet](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/socket_input.cc:128)。

它同样通过 `poll + recvfrom` 收包，但收到的是 512 字节定位数据，然后进一步调用：

- `exract_nmea_time_from_packet()`

实现见 [input.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/input.cc:23)。

这个函数会从 packet 固定偏移附近解析 NMEA/GPRMC 语句，提取：

- 年
- 月
- 日
- 时
- 分
- 秒

这一步的作用不是为了点云本身，而是为了建立雷达数据的时间基准。

---

## 8. 时间同步：为什么驱动要单独收 positioning packet

Velodyne driver 不是只依赖本机系统时间，它会尽量使用 GPS/定位包中的时间信息。

### 8.1 `PollPositioningPacket()` 的职责

见 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:175)

这条线程循环做两种事情之一：

1. 如果配置 `use_gps_time = false`
使用本机时间构造 NMEA 时间

2. 否则
持续从 `positioning_input_` 收定位包并解析 NMEA 时间

### 8.2 `basetime_` 是什么

一旦成功得到 NMEA 时间，就通过 `SetBaseTimeFromNmeaTime()` 生成 `basetime_`。
见 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:66)

`basetime_` 可以理解成：

- 当前 GPS 小时对应的 Unix 微秒基准

后面每帧 `VelodyneScan` 都会把这个 `basetime_` 写进去。

### 8.3 为什么这一步重要

因为 Velodyne 原始 firing 包本身通常只带相对时间或设备时间片段，不足以直接恢复精确绝对时间。

所以 Apollo 的做法是：

- 从 positioning packet 拿更完整的时间基准
- 从 firing packet 拿当前包的设备时间字段
- 在后续解析时拼出更准确的点时间

这说明时间同步不是外围功能，而是驱动主链的一部分。

---

## 9. 组帧：多个 UDP 包怎么变成一帧 `VelodyneScan`

### 9.1 `DevicePoll()` 是主采集循环

见 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:288)

逻辑很简单：

1. 创建一个新的 `VelodyneScan`
2. 调用 `Poll(scan)` 填充它
3. 成功后补 header
4. 通过 writer 发布出去

### 9.2 `Poll()` 做什么

见 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:107)

这里有几个关键动作：

1. 如果 `basetime_ == 0`，说明时间基准还没准备好，先不发布
2. 调 `PollStandard(scan)` 去真正收一整帧包
3. 给 `scan` 补 header、frame_id、model、mode
4. 从第一包里取设备时间字段，更新 GPS top hour
5. 把 `basetime_` 写入 `scan`

### 9.3 `PollStandard()` 怎么判定一帧收满了

见 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:144)

逻辑本质上是：

- 根据 `npackets` 连续收若干个 firing packet
- 每收到一个完整包，就 `scan->add_firing_pkts()`
- 收到足够数量后，认为这一帧完成

如果打开了 `use_poll_sync()`，还会结合 `is_main_frame` 和全局计数器做主副帧同步。

这说明 `VelodyneScan` 本质上是：

> 某一时刻收集到的一组原始 VelodynePacket 的集合消息。

### 9.4 第一层发布发生在这里

真正第一次写到 Cyber channel 的位置在 [driver.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/driver/driver.cc:294)。

```cpp
writer_->Write(scan);
```

这个 writer 在初始化时创建：

```cpp
writer_ = node_->CreateWriter<VelodyneScan>(config_.scan_channel());
```

所以驱动采集层最终产出的是：

- 消息类型：`VelodyneScan`
- channel：`scan_channel`

这就是“硬件数据到第一个 channel”的完整落点。

---

## 10. 从 `Scan` 到 `PointCloud`：第二阶段转换组件

驱动层只负责收包和组帧，不负责直接生成点云。点云是下一阶段组件 `VelodyneConvertComponent` 做的。

### 10.1 转换组件的输入输出

定义在 [velodyne_convert_component.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/parser/velodyne_convert_component.h:42)。

它是一个：

- 输入：`Component<VelodyneScan>`
- 输出：`Writer<PointCloud>`

所以它是标准的消息驱动组件，而不是直接跟硬件打交道的组件。

### 10.2 `Init()` 做什么

见 [velodyne_convert_component.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/parser/velodyne_convert_component.cc:29)

主要做了这些事：

1. 读取 Velodyne 配置
2. 创建 `Convert` 对象
3. `conv_->init(config)`，里面会创建具体 parser
4. 创建点云 writer，目标 channel 是 `convert_channel_name()`
5. 创建点云对象池，预留点数组容量

这里有一个明显的性能意图：

- 不想每一帧都重新分配大块点云内存
- 通过对象池减少分配开销

### 10.3 `Proc()` 做什么

见 [velodyne_convert_component.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/parser/velodyne_convert_component.cc:54)

处理流程是：

1. 从对象池拿一个 `PointCloud`
2. 清空旧点云内容
3. 调 `conv_->ConvertPacketsToPointcloud(scan_msg, point_cloud_out)`
4. 检查结果是否为空
5. `writer_->Write(point_cloud_out)`

所以第二个关键 channel 落点就是：

- 消息类型：`PointCloud`
- channel：`convert_channel_name`

例如在 [velodyne16_front_center_conf.pb.txt](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/conf/velodyne16_front_center_conf.pb.txt:16)，对应：

`/apollo/sensor/lidar16/front/center/PointCloud2`

---

## 11. `Convert` 和 `Parser`：点云真正在哪生成

### 11.1 `Convert` 只是包装层

定义在 [convert.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/parser/convert.h:35)，实现见 [convert.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/parser/convert.cc:26)。

它本身不直接解析每个字节，而是做两件事：

1. 根据配置创建合适的 `VelodyneParser`
2. 把 `VelodyneScan` 交给 parser 生成点云

### 11.2 真正的坐标解算在 parser

`ConvertPacketsToPointcloud()` 的核心调用是：

```cpp
parser_->GeneratePointcloud(scan_msg, point_cloud);
```

见 [convert.cc](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/velodyne/parser/convert.cc:44)

也就是说，真正把原始 packet 字节解释成：

- x
- y
- z
- intensity
- ring
- 时间相关信息

是在各型号 parser 中完成的，比如：

- `velodyne16_parser.cc`
- `velodyne32_parser.cc`
- `velodyne64_parser.cc`
- `velodyne128_parser.cc`

这也是后面继续深挖时最值得看的下一层。

### 11.3 `organized` 只是点云布局策略

`Convert` 最后还会根据 `organized` 做一次组织方式处理：

- `organized = true` 时调用 `Order()`
- 否则标记为 `is_dense = true`

这说明：

- 点云“是否有序”是转换阶段的职责
- 但它不改变采集链路本身

---

## 12. 公共骨架层在抽象什么

除了 Velodyne 的专有实现，`common/lidar_component_base_impl.h` 还定义了一套更抽象的 lidar component 骨架。
见 [lidar_component_base_impl.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/common/lidar_component_base_impl.h:35)

这套模板骨架其实明确表达了 lidar 通用数据流：

### 12.1 采集侧

`InitPacket()` 会在在线雷达模式下创建 `scan_writer_`。
见 [lidar_component_base_impl.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/common/lidar_component_base_impl.h:109)

表示：

- 从设备拿到原始包后，可以先写 `Scan`

### 12.2 转换侧

`InitConverter()` 会创建 `pcd_writer_`；如果 source 是 raw packet，还会创建 `scan_reader_`。
见 [lidar_component_base_impl.h](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/common/lidar_component_base_impl.h:77)

表示：

- 转换组件可以订阅 `Scan`
- 然后写 `PointCloud`

### 12.3 两个核心写接口

- `WriteScan()`：写原始 scan
- `WritePointCloud()`：写点云并补 header

见：

- [WriteScan](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/common/lidar_component_base_impl.h:131)
- [WritePointCloud](/home/zkm/CyberRT-10.0.0/modules/drivers/lidar/common/lidar_component_base_impl.h:172)

也就是说，这个公共骨架抽象的其实就是：

> `在线设备 -> Scan writer -> Scan reader -> PointCloud writer`

Velodyne 这条链就是这个模式的具体落地版本。

---

## 13. 把整条链压成一张图

如果只看最关键的数据流，可以画成：

```text
激光雷达硬件
  -> 以太网/UDP 报文
  -> SocketInput::recvfrom()
  -> VelodynePacket
  -> VelodyneDriver::PollStandard()
  -> VelodyneScan
  -> writer_->Write(scan)
  -> /apollo/sensor/.../Scan
  -> VelodyneConvertComponent::Proc()
  -> Convert::ConvertPacketsToPointcloud()
  -> VelodyneParser::GeneratePointcloud()
  -> PointCloud
  -> writer_->Write(point_cloud)
  -> /apollo/sensor/.../PointCloud2
  -> CompensatorComponent / 后续感知模块
```

如果再把时间同步并进去，更完整的是：

```text
positioning UDP 包
  -> get_positioning_data_packet()
  -> exract_nmea_time_from_packet()
  -> SetBaseTimeFromNmeaTime()
  -> basetime_

firing UDP 包
  -> get_firing_data_packet()
  -> VelodynePacket
  -> 拼成 VelodyneScan
  -> scan 中写入 basetime
  -> parser 根据包内容和时间基准生成 PointCloud
```

---

## 14. 从“硬件到 channel”这个问题里，最值得抓住的 5 个结论

1. **最底层接入方式是 UDP socket 收包，不是高级 SDK 回调。**
Velodyne 的采集层就是 `poll + recvfrom`。

2. **驱动层第一阶段发布的是 `Scan`，不是点云。**
原始激光包先被组装成一帧 `VelodyneScan`。

3. **点云生成发生在单独的转换组件，而不是驱动主线程里。**
采集和解码是分层的。

4. **定位时间包是主链的一部分，不是可有可无的附属功能。**
`basetime_` 直接参与后续数据时间恢复。

5. **`drivers/lidar` 的通用设计模式是稳定的：采集、scan、转换、pointcloud、补偿。**
厂商差异主要集中在 `driver/input/parser` 的细节实现。

---

## 15. 建议你下一步怎么继续看

如果你现在已经看懂了“从硬件收包到发 channel”这一层，最自然的下一步是二选一：

1. **继续往下钻 parser**
重点看 `velodyne16_parser.cc`、`velodyne_parser.cc`，解决“1206 字节包怎么变成 x/y/z/intensity”。

2. **继续往后钻补偿组件**
重点看 `compensator_component.cc`，解决“为什么点云发出来之后还要再补偿一次”。

如果你的目标是彻底打通 `drivers/lidar`，推荐顺序是：

1. 先看本文这条采集和发布主链
2. 再看 parser 做点云解码
3. 再看 compensator 做时间/运动补偿
4. 最后再看 fusion 组件如何合多雷达帧

这样顺序最自然，也最符合系统真实运行链路。
