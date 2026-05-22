# CyberRT 10.0.0 架构分析

## 整体架构

CyberRT 是百度 Apollo 自动驾驶平台的运行时框架，核心设计目标是**低延迟、高吞吐、确定性调度**。整体分为五层：

```
┌─────────────────────────────────────────────────────┐
│                  应用层 (Application)                 │
│         Component / TimerComponent (用户代码)          │
├─────────────────────────────────────────────────────┤
│                   节点层 (Node)                       │
│         Node / Writer / Reader / Service / Client    │
├─────────────────────────────────────────────────────┤
│                  调度层 (Scheduler)                   │
│            Scheduler / CRoutine / Processor          │
├─────────────────────────────────────────────────────┤
│                  传输层 (Transport)                   │
│    Transmitter / Receiver / INTRA / SHM / RTPS       │
├─────────────────────────────────────────────────────┤
│                  消息层 (Message)                     │
│           Protobuf / MessageTraits / Blocker         │
└─────────────────────────────────────────────────────┘
```

---

## UML 类图

### 1. Component 层

```
┌──────────────────────────────────────────────┐
│              <<abstract>>                    │
│              ComponentBase                   │
├──────────────────────────────────────────────┤
│ # node_            : shared_ptr<Node>        │
│ # readers_         : vector<ReaderBase>      │
│ # config_file_path_: string                  │
│ # is_shutdown_     : atomic<bool>            │
├──────────────────────────────────────────────┤
│ + Initialize(ComponentConfig) : bool         │
│ + Initialize(TimerComponentConfig) : bool    │
│ + Shutdown() : void                          │
│ + GetProtoConfig<T>() : bool                 │
│ # Init() : bool  [pure virtual]              │
│ # Clear() : void                             │
└──────────────────┬───────────────────────────┘
                   │
        ┌──────────┴──────────┐
        │                     │
┌───────▼──────────┐  ┌───────▼──────────────────┐
│ Component<M0..M3>│  │    TimerComponent         │
│  (模板，0~4消息) │  ├──────────────────────────┤
├──────────────────┤  │ - interval_ : uint32_t    │
│                  │  │ - timer_    : unique_ptr  │
├──────────────────┤  ├──────────────────────────┤
│ + Initialize()   │  │ + Initialize()            │
│ + Process()      │  │ + Process()               │
│ # Proc() [纯虚]  │  │ # Proc() [纯虚]           │
└──────────────────┘  │ + GetInterval()           │
                      └──────────────────────────┘

用户继承示例：
  class MyComp : public Component<MsgA, MsgB> {
      bool Init() override;
      bool Proc(shared_ptr<MsgA>, shared_ptr<MsgB>) override;
  };
  CYBER_REGISTER_COMPONENT(MyComp)
```

### 2. Node 层

```
┌──────────────────────────────────────────────────────┐
│                       Node                           │
├──────────────────────────────────────────────────────┤
│ - node_name_         : string                        │
│ - name_space_        : string                        │
│ - readers_           : map<string, ReaderBase>       │
│ - node_channel_impl_ : unique_ptr<NodeChannelImpl>   │
│ - node_service_impl_ : unique_ptr<NodeServiceImpl>   │
├──────────────────────────────────────────────────────┤
│ + CreateWriter<T>(channel) : shared_ptr<Writer<T>>   │
│ + CreateReader<T>(channel, cb) : shared_ptr<Reader<T>>│
│ + CreateService<Req,Resp>(name, cb) : Service        │
│ + CreateClient<Req,Resp>(name) : Client              │
│ + Observe() : void                                   │
│ + ClearData() : void                                 │
└──────┬───────────────────────────────────────────────┘
       │ creates
  ┌────┴──────────────────────────────────────┐
  │                                           │
┌─▼──────────────────┐  ┌────────────────────▼──┐
│    Writer<T>        │  │      Reader<T>         │
├────────────────────┤  ├───────────────────────┤
│ - transmitter_     │  │ - receiver_            │
│   : Transmitter<T> │  │   : Receiver<T>        │
│                    │  │ - blocker_             │
│                    │  │   : Blocker<T>         │
│                    │  │ - reader_func_         │
├────────────────────┤  │   : CallbackFunc<T>    │
│ + Write(msg)       │  ├───────────────────────┤
│ + HasReader()      │  │ + Observe()            │
│ + AcquireMessage() │  │ + GetLatestObserved()  │
└────────────────────┘  │ + HasReceived()        │
                        │ + Empty()              │
                        │ + GetDelaySec()        │
                        └───────────────────────┘

┌──────────────────────────┐  ┌──────────────────────────┐
│    Service<Req, Resp>    │  │    Client<Req, Resp>      │
├──────────────────────────┤  ├──────────────────────────┤
│ - request_receiver_      │  │ - request_transmitter_   │
│ - response_transmitter_  │  │ - response_receiver_     │
│ - service_callback_      │  │ - pending_requests_      │
│ - thread_ : thread       │  │ - sequence_number_       │
├──────────────────────────┤  ├──────────────────────────┤
│ + Init()                 │  │ + SendRequest(req)        │
│ + HandleRequest()        │  │ + AsyncSendRequest()      │
│ + SendResponse()         │  │ + ServiceIsReady()        │
└──────────────────────────┘  │ + WaitForService()        │
                              └──────────────────────────┘
```

### 3. Scheduler / CRoutine 层

```
┌──────────────────────────────────────────────────┐
│             <<singleton, abstract>>              │
│                   Scheduler                      │
├──────────────────────────────────────────────────┤
│ - id_cr_     : unordered_map<uint64, CRoutine>   │
│ - processors_: vector<Processor>                 │
│ - pctxs_     : vector<ProcessorContext>          │
├──────────────────────────────────────────────────┤
│ + CreateTask(func, name) : bool                  │
│ + NotifyTask(crid) : bool                        │
│ + RemoveTask(name) : bool                        │
│ + DispatchTask(CRoutine) : bool                  │
│ + Shutdown()                                     │
└──────────────────┬───────────────────────────────┘
                   │ manages
┌──────────────────▼───────────────────────────────┐
│                  CRoutine                        │
├──────────────────────────────────────────────────┤
│ - name_       : string                           │
│ - func_       : RoutineFunc                      │
│ - state_      : RoutineState                     │
│ - context_    : shared_ptr<RoutineContext>        │
│ - priority_   : uint32_t                         │
│ - wake_time_  : time_point                       │
├──────────────────────────────────────────────────┤
│ + Resume() : RoutineState                        │
│ + Run()                                          │
│ + Stop()                                         │
│ + Wake()                                         │
│ + HangUp()                                       │
│ + Sleep(duration)                                │
│ + Yield()                                        │
└──────────────────────────────────────────────────┘

CRoutine 状态机：
  READY ──→ (Resume) ──→ 运行中
    ↑                      │
    │                  ┌───┴──────────────┐
    │                  ▼                  ▼
  Wake()           SLEEP            DATA_WAIT
  (定时唤醒)      (Sleep调用)      (等待消息到达)
                      │                  │
                      └──────────────────┘
                           FINISHED
```

### 4. Transport 层

```
┌──────────────────────────────────────────────────────┐
│                <<singleton>>                         │
│                  Transport                           │
├──────────────────────────────────────────────────────┤
│ - participant_       : ParticipantPtr (FastDDS)      │
│ - intra_dispatcher_  : IntraDispatcherPtr            │
│ - shm_dispatcher_    : ShmDispatcherPtr              │
│ - rtps_dispatcher_   : RtpsDispatcherPtr             │
├──────────────────────────────────────────────────────┤
│ + CreateTransmitter<M>(attrs, mode) : Transmitter<M> │
│ + CreateReceiver<M>(attrs, cb, mode): Receiver<M>    │
│ + Shutdown()                                         │
└──────────────────────────────────────────────────────┘

传输模式选择（按通信范围自动选择）：

  同进程内  ──→  INTRA  (直接函数调用，零拷贝)
  同主机跨进程 ──→  SHM   (共享内存，零拷贝)
  跨主机    ──→  RTPS  (FastDDS，网络传输)
  混合      ──→  HYBRID (自动选最优)

┌──────────────────┐    ┌──────────────────┐
│  Transmitter<M>  │    │   Receiver<M>    │
│  <<abstract>>    │    │   <<abstract>>   │
├──────────────────┤    ├──────────────────┤
│ + Transmit(msg)  │    │ + Enable()       │
│ + Enable()       │    │ + Disable()      │
│ + Disable()      │    └────────┬─────────┘
└────────┬─────────┘             │
         │ 实现类                 │ 实现类
  ┌──────┴──────┐         ┌──────┴──────┐
  │ Intra       │         │ Intra       │
  │ Shm         │         │ Shm         │
  │ Rtps        │         │ Rtps        │
  │ Hybrid      │         │ Hybrid      │
  └─────────────┘         └─────────────┘
```

---

## 完整数据流

### 消息发布/订阅流程

```
Publisher 进程                          Subscriber 进程
─────────────────                       ─────────────────
Component::Proc()
    │
    ▼
Writer<T>::Write(msg)
    │
    ▼
Transmitter<T>::Transmit(msg)
    │
    ├─[同进程]──→ IntraTransmitter ──→ IntraReceiver
    ├─[同主机]──→ ShmTransmitter ───→ ShmReceiver
    └─[跨主机]──→ RtpsTransmitter ──→ RtpsReceiver
                                           │
                                           ▼
                                    Receiver 触发回调
                                           │
                                           ▼
                                    Blocker::Enqueue(msg)
                                           │
                                           ▼
                                    Scheduler::NotifyTask()
                                           │
                                           ▼
                                    CRoutine 状态: DATA_WAIT → READY
                                           │
                                           ▼
                                    Processor 调度执行
                                           │
                                           ▼
                                    Reader callback / Component::Proc()
```

### Service 调用流程

```
Client::SendRequest(req)
    │
    ▼
request_transmitter_::Transmit(req)  ──→  Service::HandleRequest(req)
                                               │
                                               ▼
                                          service_callback_(req, resp)
                                               │
                                               ▼
response_transmitter_::Transmit(resp) ←── SendResponse(resp)
    │
    ▼
Client::HandleResponse(resp)
    │
    ▼
Promise::set_value(resp)  →  Future::get()  →  返回给调用方
```

---

## 关键设计决策

### 1. 协程调度 vs 线程

CyberRT 用**协程（CRoutine）**而非线程处理消息回调：

|          | 线程              | CRoutine               |
| -------- | ----------------- | ---------------------- |
| 切换开销 | ~1-10μs（内核态） | ~100ns（用户态）       |
| 内存占用 | ~8MB stack        | ~128KB stack           |
| 并发数   | 受 CPU 核数限制   | 可创建数千个           |
| 调度控制 | OS 抢占式         | 框架协作式，确定性更强 |

### 2. 三种传输模式

```
INTRA  → 同进程，直接指针传递，零拷贝，延迟 < 1μs
SHM    → 同主机，共享内存，零序列化，延迟 ~10μs
RTPS   → 跨主机，FastDDS，支持 QoS，延迟 ~100μs
```

框架根据 Publisher/Subscriber 的位置**自动选择**最优模式（HYBRID）。

### 3. Component 与 Node 的关系

每个 Component 持有一个 Node，Node 是与框架交互的唯一入口：

```
Component
  └── node_  (ComponentBase 成员)
        ├── CreateWriter()   → 发布数据
        ├── CreateReader()   → 订阅数据
        ├── CreateService()  → 提供服务
        └── CreateClient()   → 调用服务
```

### 4. 插件机制

Component 通过 `CYBER_REGISTER_COMPONENT(ClassName)` 宏注册到 ClassLoader，`mainboard` 进程在运行时从 `.so` 动态加载，无需重新编译主程序。

---

## DAG 文件与进程模型

```
mainboard 进程
    │
    ├── 加载 .dag 文件
    │       module_library: "libxxx.so"
    │       timer_components: [...]
    │       components: [...]
    │
    ├── dlopen(libxxx.so)
    │
    ├── ClassLoader::CreateInstance("ClassName")
    │       → Component::Initialize()
    │           → Init()  (用户代码)
    │
    └── Scheduler 开始调度
            → TimerComponent: 按 interval 触发 Proc()
            → Component: 有消息到达时触发 Proc()
```

同一个 `.dag` 文件中的所有 component **运行在同一个 mainboard 进程**中，共享内存空间，进程内通信走 INTRA 模式（零拷贝）。

---

## 目录结构速查

```
cyber/
├── component/          Component / TimerComponent / ComponentBase
├── node/               Node / Writer / Reader
├── service/            Service / Client
├── scheduler/          Scheduler / Processor / ProcessorContext
├── croutine/           CRoutine / RoutineContext (含汇编)
├── transport/          Transport / Transmitter / Receiver
│   ├── transmitter/    Intra / Shm / Rtps / Hybrid
│   ├── receiver/       Intra / Shm / Rtps / Hybrid
│   └── dispatcher/     IntraDispatcher / ShmDispatcher / RtpsDispatcher
├── message/            MessageTraits / Blocker
├── class_loader/       动态库加载
├── plugin_manager/     插件管理
├── timer/              Timer
├── record/             录制/回放
├── tools/              cyber_monitor / cyber_recorder / cyber_channel
└── examples/           示例代码
```

```
https://apollo.baidu.com/docs/apollo/latest/dir_0d6dc3a33fbbb812401bd489d918ef1b.html
```