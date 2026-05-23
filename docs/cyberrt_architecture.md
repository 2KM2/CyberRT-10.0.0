# CyberRT 架构总览

## ROS1 vs CyberRT 核心对比

| 概念 | ROS1 | CyberRT |
|------|------|---------|
| 初始化 | `ros::init()` | `apollo::cyber::Init()` |
| 节点 | `ros::NodeHandle` | `CreateNode()` |
| 发布者 | `nh.advertise<T>()` | `node->CreateWriter<T>()` |
| 订阅者 | `nh.subscribe<T>()` | `node->CreateReader<T>()` |
| 消息格式 | `.msg` (rosidl) | `.proto` (Protobuf) |
| 主循环 | `ros::spin()` | `cyber::WaitForShutdown()` |
| 频率控制 | `ros::Rate` | `cyber::Rate` |
| 话题名 | `/chatter` | `channel/chatter` |
| 日志 | `ROS_INFO` | `AINFO` |
| 中间件 | roscore (master) | 无中心节点，基于 FastDDS |

**最大的不同：**
1. **没有 roscore** — 基于 FastDDS 去中心化服务发现
2. **消息用 Protobuf** — 强类型、跨语言、向后兼容
3. **Component 框架** — 插件式加载，由 `mainboard` 统一管理
4. **协程调度** — 用协程替代线程，消息驱动任务调度

---

## 传输层（Transport）

### 三种传输方式

| 场景 | 模式 | 机制 |
|------|------|------|
| 同进程 | INTRA | `shared_ptr` 直传，零拷贝零序列化 |
| 跨进程（同机器） | SHM | 共享内存 + 通知机制 |
| 跨机器 | RTPS | FastDDS 网络传输 |

### HYBRID 模式（默认）

框架根据 publisher/subscriber 位置**自动选择**传输方式，配置在 `cyber/conf/cyber.pb.conf`：

```
same_proc  → INTRA
diff_proc  → SHM
diff_host  → RTPS
```

**指令类小消息和图像大消息写法完全一样**，框架自动决策传输方式。

---

## UML 类图

```
┌──────────────────────────────────────────────────────┐
│                    ComponentBase                     │
│  - node_: shared_ptr<Node>                           │
│  - readers_: vector<ReaderBase>                      │
│  + Init() [virtual]                                  │
└──────────────────────┬───────────────────────────────┘
                       │ 继承
┌──────────────────────▼───────────────────────────────┐
│           Component<M0, M1, M2, M3>                  │
│  + Proc(m0, m1, m2, m3) [virtual]                    │
└──────────────────────┬───────────────────────────────┘
                       │ 拥有
                       ▼
┌──────────────────────────────────────────────────────┐
│                      Node                            │
│  - node_channel_impl_: NodeChannelImpl               │
│  - node_service_impl_: NodeServiceImpl               │
│  - readers_: map<string, ReaderBase>                 │
│  + CreateWriter<M>(channel)  → Writer<M>             │
│  + CreateReader<M>(channel)  → Reader<M>             │
│  + CreateService<Req,Res>()  → Service<Req,Res>      │
│  + CreateClient<Req,Res>()   → Client<Req,Res>       │
└──────┬───────────────┬──────────────────┬────────────┘
       ▼               ▼                  ▼
┌────────────┐  ┌─────────────┐  ┌──────────────────────┐
│  Writer<M> │  │  Reader<M>  │  │  Service<Req,Res>     │
└─────┬──────┘  └──────┬──────┘  │  Client<Req,Res>      │
      │                │         └──────────┬─────────────┘
      └────────────────┴──────────────────┘
                       │ 使用
                       ▼
┌──────────────────────────────────────────────────────┐
│               Transport (singleton)                  │
│                                                      │
│   HybridTransmitter / HybridReceiver                 │
│     ├── SAME_PROC → IntraTransmitter  (shared_ptr)   │
│     ├── DIFF_PROC → ShmTransmitter    (共享内存)      │
│     └── DIFF_HOST → RtpsTransmitter   (FastDDS)      │
│                                                      │
│   Dispatchers:                                       │
│     intra_dispatcher_ / shm_dispatcher_              │
│     rtps_dispatcher_ (participant: FastDDS)          │
└──────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────┐
│               Scheduler (singleton)                  │
│  - processors_: vector<Processor>                    │
│  - id_cr_: map<id, CRoutine>                         │
│  + CreateTask() / DispatchTask() / NotifyTask()      │
│                                                      │
│  ┌─────────────┐    ┌──────────────────────────────┐ │
│  │  Processor  │    │          CRoutine            │ │
│  │  (线程)     │◄───│  - func_: function<void()>   │ │
│  └─────────────┘    │  - state_: READY/SLEEP/...   │ │
│                     │  + Resume() / Yield() / Wake()│ │
│                     └──────────────────────────────┘ │
└──────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────┐
│                  Record (录制/回放)                   │
│  RecordBase                                          │
│    ├── RecordWriter  (owns RecordFileWriter)         │
│    └── RecordReader  (owns RecordFileReader)         │
└──────────────────────────────────────────────────────┘
```

---

## 核心数据流（单机场景）

```
Component::Proc()
    └─► Writer<M>::Write(msg)
            └─► HybridTransmitter
                    ├─► IntraTransmitter  (同进程: shared_ptr 直传)
                    └─► ShmTransmitter    (跨进程: 写共享内存 + 通知)
                                              │
                                         Reader 收到通知
                                              │
                                    Scheduler::NotifyTask()
                                              │
                                    CRoutine: DATA_WAIT → READY
                                              │
                                    Processor 调度执行
                                              │
                                    Component::Proc() 被调用
```

---

## 关键源码位置

| 模块 | 路径 |
|------|------|
| 框架入口 | `cyber/cyber.h`, `cyber/init.h` |
| Node | `cyber/node/node.h` |
| Component | `cyber/component/component.h` |
| Transport | `cyber/transport/transport.h` |
| 传输配置 | `cyber/conf/cyber.pb.conf` |
| Scheduler | `cyber/scheduler/scheduler.h` |
| CRoutine | `cyber/croutine/croutine.h` |
| Service/Client | `cyber/service/` |
| Record | `cyber/record/` |
| 示例 | `cyber/examples/` |
