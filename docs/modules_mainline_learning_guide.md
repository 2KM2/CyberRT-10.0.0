# Modules 主链路学习指南（从实际使用出发）

本文不按 `modules/` 目录机械罗列，而是按自动驾驶系统真实运行时的主链路来学习：消息从哪里来，经过哪些模块处理，最终如何落到车辆控制。

适合的阅读目标有两个：

1. 想先建立对 `modules/` 的整体直觉，而不是一上来陷进细节。
2. 想把 `modules/` 里的业务模块和 `cyber/` 里的框架机制对应起来。

---

## 1. 主链路总览

如果只抓最核心的自动驾驶运行链路，可以先压缩成一句话：

`drivers -> localization/perception -> prediction -> planning -> control -> canbus`

这条链路表示的不是单纯的数据传递顺序，而是系统职责的逐层收敛：

- `drivers` 负责把设备原始数据接入 CyberRT。
- `localization` 负责回答“车现在在哪、姿态如何”。
- `perception` 负责回答“周围有什么、道路环境是什么样”。
- `prediction` 负责回答“这些目标接下来可能怎么动”。
- `planning` 负责回答“车应该怎么走”。
- `control` 负责回答“怎么把轨迹变成方向盘、油门、制动命令”。
- `canbus` 负责把控制命令真正送到车辆底盘，并把底盘状态反馈回来。

学习时不要把这些模块当成并列目录去记，而要始终问三个问题：

1. 这个模块吃什么输入。
2. 这个模块产出什么结果。
3. 它在主链里处于哪一段。

---

## 2. 为什么按主链路学比按目录学更有效

`modules/` 下目录很多，里面既有核心自动驾驶模块，也有外围工具、桥接、可视化、统计、第三方适配。第一次学习时，如果直接按目录树平铺阅读，很容易出现两个问题：

- 只记住模块名字，不知道它们如何协作。
- 先掉进实现细节，却不知道为什么这个模块存在。

更有效的方式是沿着“车跑起来时到底发生了什么”来学：

1. 传感器驱动发布原始数据。
2. 定位和感知把原始数据变成可决策的状态信息。
3. 预测和规划把环境状态变成未来轨迹。
4. 控制和 canbus 把轨迹变成执行命令。

这样学的好处是：

- 能快速建立模块之间的上下游关系。
- 更容易读懂 `launch` 和 `dag` 的意义。
- 更容易把业务模块和 CyberRT 的 `Component / Reader / Writer / TimerComponent` 框架对上。

---

## 3. `drivers`：传感器输入层

### 3.1 这一层解决什么问题

`drivers` 是自动驾驶系统的数据入口层。它的核心任务不是做复杂算法，而是把相机、激光雷达、GNSS、IMU、Radar、麦克风、视频设备等硬件数据，转换成 CyberRT 中可以流动的消息。

从实际使用看，这一层最重要的不是“识别了什么”，而是：

- 设备是否正常初始化。
- 原始数据是否稳定发布。
- 消息时间戳、坐标系、频率是否符合下游预期。

如果这一层不稳定，后面的定位、感知、规划、控制都会失真。

### 3.2 在主链里的位置

它是整个主链的起点。没有它，后面的模块都没有输入。

典型下游包括：

- `localization` 读取 GNSS / IMU / 点云等数据。
- `perception` 读取相机图像、点云、雷达目标等数据。
- 部分工具模块直接消费原始传感器流做可视化或录制。

### 3.3 典型目录入口

建议先看这些目录，而不是一次把整个 `drivers/` 全看完：

- `modules/drivers/gnss/`
- `modules/drivers/camera/`
- `modules/drivers/lidar/`
- `modules/drivers/radar/`

这些目录最能代表“原始设备数据怎么接到 Cyber channel”。

### 3.4 常见 `launch` / `dag`

可以先从这些入口看：

- `modules/drivers/gnss/launch/gnss.launch`
- `modules/drivers/gnss/dag/gnss.dag`
- `modules/drivers/camera/launch/camera.launch`
- `modules/drivers/camera/dag/camera.dag`
- `modules/drivers/lidar/velodyne/launch/velodyne.launch`
- `modules/drivers/lidar/velodyne/dag/velodyne.dag`
- `modules/drivers/radar/conti_radar/launch/conti_radar.launch`
- `modules/drivers/radar/conti_radar/dag/conti_radar.dag`

这里最值得看的不是算法，而是：

- 一个进程加载哪个 `module_library`
- 起了哪个 `class_name`
- 发布哪些 channel
- 依赖哪些配置文件

### 3.5 典型输入输出

这一层通常直接面向硬件或驱动配置，因此输入往往不是上游 Cyber channel，而是：

- 设备端口
- 网络数据流
- 串口
- 网卡报文
- 本地设备节点

输出则是标准化后的 Cyber 消息，比如：

- 相机图像
- 点云
- IMU 数据
- GNSS 定位观测
- Radar 检测目标

### 3.6 阅读源码时先看什么

建议按这个顺序：

1. 先看 `launch`，确认起了哪个进程。
2. 再看 `dag`，确认进程里加载了哪个组件。
3. 再看组件 `Init()` 里如何打开设备、加载配置、创建 `Writer`。
4. 最后看数据回调里如何把原始数据打包成消息并写入 channel。

看这一层时，不要急着研究设备协议细节，先抓住一个问题：

> 原始设备数据是在哪个组件里，被包装成 Cyber 消息并发布出去的？

### 3.7 为什么先学它

因为它是所有主链数据的根。你先看懂输入层，再看后面的融合和决策，整个系统会自然很多。

---

## 4. `localization`：车辆位姿层

### 4.1 这一层解决什么问题

`localization` 要回答的是：

> 车辆当前在地图或世界坐标系中的位置、姿态、速度、角速度分别是什么？

这是自动驾驶主链里的基础状态层。规划和控制如果拿不到稳定、可信的位姿估计，就没有可执行的参考系。

### 4.2 在主链里的位置

它连接“原始传感器数据”和“带空间语义的决策模块”。

典型上游：

- GNSS
- IMU
- 激光雷达点云

典型下游：

- `planning`
- `control`
- `perception` 中需要自车位姿对齐的部分

### 4.3 典型目录入口

优先看：

- `modules/localization/`

这个目录下会看到多种定位方案，比如 RTK、MSF、NDT。第一次学习不需要全都展开，建议先抓一条主线，比如 MSF 或 RTK。

### 4.4 常见 `launch` / `dag`

建议先看：

- `modules/localization/launch/localization.launch`
- `modules/localization/launch/msf_localization.launch`
- `modules/localization/launch/rtk_localization.launch`
- `modules/localization/dag/dag_streaming_msf_localization.dag`
- `modules/localization/dag/dag_streaming_rtk_localization.dag`

例如在 `dag_streaming_msf_localization.dag` 中可以直接看到 reader 配置里订阅了 IMU 输入，这种信息非常关键，因为它让你立刻知道“这个组件到底依赖什么数据源”。

### 4.5 典型输入输出

典型输入：

- `/apollo/sensor/gnss/imu`
- GNSS 相关观测
- 点云或地图匹配输入

典型输出：

- `/apollo/localization/pose`

这个输出 channel 是后续很多主链模块的重要依赖，尤其是 `planning` 和 `control`。

### 4.6 阅读源码时先看什么

建议按这个顺序：

1. 看 `dag` 中 `module_library` 和 `class_name`。
2. 找到组件 `Init()`，看它加载了哪些定位配置。
3. 看它创建了哪些 `Reader` 和 `Writer`。
4. 看主处理函数里是如何把传感器输入融合成 `LocalizationEstimate` 之类的定位输出。

第一次看定位源码时，不必立刻吃透滤波算法、地图匹配、传感器融合数学细节。先建立这个框架认识：

> `localization` 的本质是把多个原始观测源，变成主链其余模块都能依赖的一份统一自车状态。

### 4.7 为什么在这个阶段学它

因为它是规划和控制的共同前提。没有稳定定位，后面“车该怎么走”和“怎么打方向盘”都无从谈起。

---

## 5. `perception`：环境语义层

### 5.1 这一层解决什么问题

`perception` 要回答的是：

- 周围有哪些障碍物。
- 它们在哪里、速度如何、类别是什么。
- 车道线、红绿灯、路面结构、ROI 等环境信息是什么。

这一层把原始传感器流变成可决策的环境语义，是自动驾驶链路里信息量最大的一层之一。

### 5.2 在主链里的位置

它通常和 `localization` 并行地接收 `drivers` 的原始输入，但产出的结果会进一步供给：

- `prediction`
- `planning`
- 某些监控、可视化、故事化模块

因此它的角色可以概括为：

> 从“设备看到的世界”变成“系统理解的世界”。

### 5.3 典型目录入口

`modules/perception/` 目录很大，第一次不要全看。建议先抓代表性主线：

- `modules/perception/launch/`
- `modules/perception/lidar_detection/`
- `modules/perception/lidar_tracking/`
- `modules/perception/multi_sensor_fusion/`
- `modules/perception/camera_detection_multi_stage/`

### 5.4 常见 `launch` / `dag`

建议先看：

- `modules/perception/launch/perception_all.launch`
- `modules/perception/launch/perception_lidar.launch`
- `modules/perception/launch/perception_camera_multi_stage.launch`
- `modules/perception/launch/perception_radar.launch`

以及配套的：

- `modules/perception/lidar_detection/dag/lidar_detection.dag`
- `modules/perception/lidar_tracking/dag/lidar_tracking.dag`
- `modules/perception/multi_sensor_fusion/dag/multi_sensor_fusion.dag`
- `modules/perception/msg_adapter/dag/msg_adapter.dag`

这里要重点观察的是：

- 感知并不是单个组件，而是经常由多个子模块串起来。
- 一个阶段的输出会成为下一个阶段的输入，例如检测、跟踪、融合、输出。
- `msg_adapter` 这类模块说明感知里经常存在消息格式转换或统一适配层。

### 5.5 典型输入输出

典型输入：

- 相机图像
- 激光雷达点云
- Radar 目标
- 定位姿态信息

典型输出：

- 障碍物列表
- 跟踪结果
- 多传感器融合结果
- 车道线或交通灯相关结果

这些输出最终会成为 `prediction` 和 `planning` 的重要输入。

### 5.6 阅读源码时先看什么

建议不要从某个深层算法文件直接开读，而是按流水线走：

1. 先看一个总体 `launch`，比如 `perception_all.launch`。
2. 再看某一条子链对应的 `dag`，比如 lidar detection 或 camera detection。
3. 搞清楚每个子模块的输入输出 channel。
4. 最后再进入具体组件源码，看 `Init()` 和 `Proc()`。

看感知最容易犯的错误，是一开始就扑进模型推理、后处理、跟踪算法实现。更好的做法是先把链路理顺：

> 数据在哪一层被检测，在哪一层被跟踪，在哪一层被融合，最后又以什么消息格式提供给下游。

### 5.7 为什么在这个阶段学它

因为它决定系统“看见了什么”。如果定位告诉系统“我在哪”，那么感知告诉系统“我周围有什么”。这两层一起构成后续预测和规划的基础世界状态。

---

## 6. `prediction`：目标运动预测层

### 6.1 这一层解决什么问题

`prediction` 要回答的是：

> 当前已经被感知到的目标，接下来可能会怎么运动？

感知通常告诉你“此刻是什么样”，预测则试图把这个静态快照向未来推进一段时间。

### 6.2 在主链里的位置

它位于感知之后、规划之前。

典型上游：

- `perception` 输出的障碍物、轨迹、环境信息

典型下游：

- `planning`

所以它本质上是规划的风险前视层。没有预测，规划只能基于“此刻”，很难处理动态交通参与者。

### 6.3 典型目录入口

优先看：

- `modules/prediction/`

这个目录相对比感知收敛，适合作为“如何承接感知结果并向规划提供未来意图”的样板。

### 6.4 常见 `launch` / `dag`

建议先看：

- `modules/prediction/launch/prediction.launch`
- `modules/prediction/dag/prediction.dag`

如果后续关心不同运行模式，还可以再看：

- `modules/prediction/dag/prediction_navi.dag`
- `modules/prediction/dag/prediction_lego.dag`

### 6.5 典型输入输出

典型输入：

- 感知障碍物列表
- 自车定位状态
- 可能还包括地图相关上下文

典型输出：

- `/apollo/prediction`

这个输出在 `planning` 的 `dag` 里是非常直观可见的输入之一，因此它是主链中一个很好观察的接口点。

### 6.6 阅读源码时先看什么

建议按这个顺序：

1. 看 `prediction.launch` 和 `prediction.dag`。
2. 找组件类和它的 readers。
3. 看它如何读取感知输出并组织内部预测流程。
4. 看它最终如何发布统一的 prediction 消息给 planning。

第一次学习预测时，不需要马上纠结轨迹生成模型和行为意图分类细节。先抓住它在系统里的定位：

> `prediction` 不是重新理解世界，而是把感知得到的“当前世界状态”外推成“短时未来世界状态”。

### 6.7 为什么在这个阶段学它

因为这是从“感知世界”走向“做出决策”的过渡层。你看懂它，planning 的输入语义会清楚很多。

---

## 7. `planning + control`：决策与执行层

### 7.1 为什么把这两部分放在一起看

从系统分层上说，`planning` 和 `control` 不是一回事：

- `planning` 负责生成要走的轨迹。
- `control` 负责把轨迹变成车辆可执行的控制命令。

但从第一次学习主链路的角度，把它们放在一起最有效，因为它们共同构成“从理解环境到驱动车辆动作”的最后闭环。

### 7.2 `planning` 解决什么问题

`planning` 要回答的是：

> 结合自车状态、环境目标、预测结果和地图约束，车辆接下来应该走哪条轨迹？

在你当前仓库中，`planning.dag` 已经很清楚地展示了它的几个关键输入：

- `/apollo/prediction`
- `/apollo/canbus/chassis`
- `/apollo/localization/pose`

这说明 planning 不是单纯依赖感知，而是同时依赖：

- 未来动态目标信息
- 当前底盘状态
- 当前自车定位

### 7.3 `control` 解决什么问题

`control` 要回答的是：

> 给定规划轨迹和车辆当前状态，应该输出什么方向盘、油门、制动指令？

与很多消息驱动组件不同，`control` 在当前配置中是典型的定时驱动模块。`control.dag` 里使用的是 `timer_components`，并配置了固定 `interval`。这意味着它更像一个周期控制回路，而不是单纯“来一帧消息就处理一次”。

这非常符合控制系统的工程特性：控制通常强调固定周期、稳定时序和连续反馈。

### 7.4 在主链里的位置

这是自动驾驶主链的末端决策与执行层：

- `planning` 把环境与状态转成轨迹。
- `control` 把轨迹转成控制命令。
- `canbus` 再把命令送到底盘，同时把底盘状态反馈回来。

### 7.5 典型目录入口

建议先看：

- `modules/planning/planning_component/`
- `modules/control/control_component/`
- `modules/canbus/`

这三个目录连起来看，能形成最完整的“决策到执行”链路。

### 7.6 常见 `launch` / `dag`

建议先看：

- `modules/planning/planning_component/launch/planning.launch`
- `modules/planning/planning_component/dag/planning.dag`
- `modules/control/control_component/launch/control.launch`
- `modules/control/control_component/dag/control.dag`
- `modules/canbus/launch/canbus.launch`
- `modules/canbus/dag/canbus.dag`

这里要重点看两件事：

- planning 是如何通过多个 readers 接收 prediction、chassis、localization 等输入的。
- control 为什么采用 `TimerComponent` 风格而不是普通消息触发风格。

### 7.7 典型输入输出

`planning` 典型输入：

- `/apollo/prediction`
- `/apollo/canbus/chassis`
- `/apollo/localization/pose`

`planning` 典型输出：

- 规划轨迹相关消息

`control` 典型输入：

- 规划轨迹
- 车辆当前状态
- 定位姿态

`control` 典型输出：

- 转向、油门、制动等控制命令

`canbus` 典型输入输出：

- 输入控制命令
- 输出底盘状态、反馈状态

### 7.8 阅读源码时先看什么

建议把这一段按闭环来读：

1. 先看 `planning.dag`，弄清 readers 和输入 channel。
2. 再看 planning 组件是怎么组织输入并产出轨迹的。
3. 然后看 `control.dag`，确认它是 timer 驱动。
4. 再看 control 组件如何在固定周期内读取状态、计算控制量、发布命令。
5. 最后看 canbus 如何把控制命令下发到底盘，并把底盘状态回流给上游。

这一段最关键的理解不是“公式怎么写”，而是：

> 主链在这里完成了从“理解世界”到“驱动车辆执行”的闭环落地。

### 7.9 为什么它最适合串主链

因为前面的模块即使很复杂，最终都要服务于这里：

- 感知和预测提供环境理解。
- 定位提供自车状态。
- 规划做动作决策。
- 控制做执行落地。
- canbus 负责和真实车辆底盘交互。

把这段看通，整个 `modules/` 的主脉络就会非常清楚。

---

## 8. 第一轮学习时怎么实际走

如果你现在开始沿主链读代码，建议用下面的顺序，不要跳：

1. 先看 `planning.dag` 和 `control.dag`，因为它们最能暴露主链输入输出关系。
2. 反向追到 `prediction`，弄清 planning 为什么要依赖它。
3. 再反向追到 `perception`，看 prediction 的输入从哪里来。
4. 同时补 `localization`，理解 `/apollo/localization/pose` 为什么几乎是主链基础输入。
5. 最后回到 `drivers`，看原始传感器数据从哪里进入系统。

这个顺序和“从下往上读”看起来有点反直觉，但对第一次学习非常有效。原因是：

- 先看末端，你更容易知道上游信息为什么存在。
- 先看关键消费方，再看生产方，问题意识会更强。
- 不容易一开始就淹没在大规模感知和驱动细节里。

如果你只打算先追一条最小可理解路径，可以这样走：

1. `modules/planning/planning_component/launch/planning.launch`
2. `modules/planning/planning_component/dag/planning.dag`
3. `modules/control/control_component/launch/control.launch`
4. `modules/control/control_component/dag/control.dag`
5. `modules/localization/dag/dag_streaming_msf_localization.dag`
6. `modules/prediction/dag/prediction.dag`
7. `modules/perception/launch/perception_all.launch`
8. `modules/drivers/gnss/` 或 `modules/drivers/lidar/velodyne/`

---

## 9. 这一轮学习的目标，不是吃透所有实现

第一次学习 `modules`，目标不应该是把每个目录、每个算法、每个配置都看完。更现实的目标是先建立下面这套整体感：

1. 自动驾驶主链由哪些核心部分组成。
2. 每一层分别回答什么问题。
3. 每一层的输入输出是什么。
4. `launch`、`dag`、组件源码三者之间如何对应。
5. 业务模块如何依托 CyberRT 的 `Component / TimerComponent / Reader / Writer` 跑起来。

等这套主骨架建立起来之后，再进入某一层深挖，效率会高很多。

---

## 10. 一句话总结

如果把 `modules/` 从实际使用角度压缩成一句话，可以这样记：

> `drivers` 把真实世界搬进 CyberRT，`localization` 和 `perception` 解释世界，`prediction` 推演世界，`planning` 决定怎么走，`control` 和 `canbus` 让车真的动起来。
