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

---

## SHM 与 ShmTransmitter/ShmReceiver

### SHM 在整体传输层中的位置

`SHM` 对应的是“跨进程、同机器”的传输路径。上层调用仍然是：

```cpp
node->CreateWriter<T>(channel)
node->CreateReader<T>(channel, callback)
```

真正落到共享内存路径时，调用链是：

```text
Writer<T>::Write(msg)
  -> HybridTransmitter / ShmTransmitter
  -> Segment::AcquireBlockToWrite()
  -> 序列化写入共享内存 block
  -> Notifier::Notify(ReadableInfo)
  -> ShmDispatcher 线程 Listen()
  -> Segment::AcquireBlockToRead()
  -> 反序列化消息
  -> ShmReceiver::OnNewMessage()
  -> Reader 回调
```

其中：

- `ShmTransmitter` 负责把消息写进共享内存并发通知。
- `ShmReceiver` 自己不直接阻塞等消息，而是把监听注册给 `ShmDispatcher`。
- `ShmDispatcher` 是共享内存读端的后台分发线程，负责收通知、读 block、解析消息、回调 listener。

### ShmTransmitter 做了什么

`ShmTransmitter` 在 [cyber/transport/transmitter/shm_transmitter.h](/home/zkm/CyberRT-10.0.0/cyber/transport/transmitter/shm_transmitter.h:46) 定义，核心成员是：

- `segment_`：当前 channel 对应的共享内存段
- `notifier_`：通知器
- `serialized_receiver_count_`：需要普通序列化消息的接收端数量
- `arena_receiver_count_`：需要 arena SHM 消息的接收端数量
- `arena_transmit_`：当前 channel 是否启用 arena SHM

`Enable()` 时它会：

1. 判断当前 channel 是否允许 arena SHM。
2. 如果允许，先启用 `ProtobufArenaManager`。
3. 通过 `SegmentFactory::CreateSegment(channel_id_)` 创建共享内存段。
4. 通过 `NotifierFactory::CreateNotifier()` 选择通知器。

实现见 [cyber/transport/transmitter/shm_transmitter.h](/home/zkm/CyberRT-10.0.0/cyber/transport/transmitter/shm_transmitter.h:160)。

真正发送消息在 `Transmit(const M&, const MessageInfo&)`，见 [cyber/transport/transmitter/shm_transmitter.h](/home/zkm/CyberRT-10.0.0/cyber/transport/transmitter/shm_transmitter.h:201)。它分两条路径。

#### 1. 普通序列化路径

如果没有 arena SHM，流程是：

1. `segment_->AcquireBlockToWrite(msg_size, &wb)` 获取一个可写普通 block。
2. `message::SerializeToArray(msg, wb.buf, msg_size)` 把消息序列化到共享内存。
3. 在消息尾部继续写入 `MessageInfo`。
4. 设置 `wb.block->msg_size()` 和 `msg_info_size()`。
5. `segment_->ReleaseWrittenBlock(wb)` 释放写锁。
6. 通过 `ReadableInfo{host_id, block_index, channel_id}` 通知读端。

这里共享内存里放的是：

```text
[ protobuf bytes ][ MessageInfo bytes ]
```

block 元数据里记录这两段长度。

#### 2. arena SHM 路径

如果 channel 开了 arena SHM，并且消息类型不是 `RawMessage` / `PyMessageWrap`，则走 arena 路径。

这时 `Transmit()` 会先申请一个 `arena block`，见 [cyber/transport/transmitter/shm_transmitter.h](/home/zkm/CyberRT-10.0.0/cyber/transport/transmitter/shm_transmitter.h:216)。

然后它不是直接把 protobuf 对象整体 memcpy 到 shm，而是：

1. 创建 `ArenaMessageWrapper`。
2. `arena_manager->SetMessageChannelId()` 写入 channel id。
3. `message::SerializeToArenaMessageWrapper(msg, wrapper, &msg_p)`。
4. 把 wrapper 的固定长度元信息 `memcpy` 到 arena block，当前代码里是 `1024` 字节。
5. 再把 `MessageInfo` 追加到 wrapper 后面。
6. `ReadableInfo` 里记录 `arena_block_index`。

这里的关键点是：

- `arena block` 里放的不是完整 protobuf bytes，而是一个“如何在共享内存里找到 protobuf arena 对象”的 wrapper。
- 真正的 protobuf message 对象是在 `ProtobufArenaManager` 管理的 `ArenaSegment` 共享内存中。

如果当前既有 arena 接收者，又有普通序列化接收者，`ShmTransmitter` 还会同时再写一份普通 block，见 [cyber/transport/transmitter/shm_transmitter.h](/home/zkm/CyberRT-10.0.0/cyber/transport/transmitter/shm_transmitter.h:262)。这说明它支持“同一 channel 不同 reader 能力不一致”的兼容发送。

### ShmReceiver 做了什么

`ShmReceiver` 很薄，定义在 [cyber/transport/receiver/shm_receiver.h](/home/zkm/CyberRT-10.0.0/cyber/transport/receiver/shm_receiver.h:32)。

它本身不负责读共享内存，不负责跑监听线程。它的职责只有两个：

1. 启动时把当前 reader 注册给 `ShmDispatcher`
2. arena SHM 打开时，先确保 `ProtobufArenaManager` 和对应 channel 的 `ArenaSegment` 已启用

`Enable()` 的核心逻辑：

```text
if arena shm enabled:
  ProtobufArenaManager::Enable()
  ProtobufArenaManager::EnableSegment(channel_id)

dispatcher_->AddListener<M>(attr_, OnNewMessage)
```

见 [cyber/transport/receiver/shm_receiver.h](/home/zkm/CyberRT-10.0.0/cyber/transport/receiver/shm_receiver.h:62)。

所以可以把 `ShmReceiver` 理解成：共享内存接收端在 reader 层的注册壳子，真正干活的是 `ShmDispatcher`。

### ShmDispatcher 是共享内存读端主循环

`ShmDispatcher` 在 [cyber/transport/dispatcher/shm_dispatcher.h](/home/zkm/CyberRT-10.0.0/cyber/transport/dispatcher/shm_dispatcher.h:50) 定义。

它持有：

- `segments_`：按 `channel_id` 保存的 `Segment`
- `notifier_`：通知器
- `thread_`：后台监听线程
- `previous_indexes_` / `arena_previous_indexes_`
- listener 表

这说明共享内存读端是“中心化”的：同一进程里不是每个 `ShmReceiver` 各起一个线程，而是所有 shm reader 共享一个 dispatcher 线程。

#### 监听注册

`ShmReceiver::Enable()` 调用 `dispatcher_->AddListener<M>()`。`ShmDispatcher::AddListener()` 会做两件事：

1. 为该 `channel_id` 注册 listener adapter。
2. 调用 `AddSegment(self_attr)`，确保当前 channel 的 segment 已加入 dispatcher 管理。

见 [cyber/transport/dispatcher/shm_dispatcher.h](/home/zkm/CyberRT-10.0.0/cyber/transport/dispatcher/shm_dispatcher.h:159)。

listener adapter 又分两类。

#### 普通 block 的 listener adapter

普通路径下，adapter 会：

1. 从 `rb->buf` 中按 `rb->block->msg_size()` 反序列化出 `MessageT`。
2. 从 `msg_info` 取发送时间、序号。
3. 做统计采样。
4. 调用上层 reader callback。

见 [cyber/transport/dispatcher/shm_dispatcher.h](/home/zkm/CyberRT-10.0.0/cyber/transport/dispatcher/shm_dispatcher.h:238)。

#### arena block 的 listener adapter

arena 路径更复杂，见 [cyber/transport/dispatcher/shm_dispatcher.h](/home/zkm/CyberRT-10.0.0/cyber/transport/dispatcher/shm_dispatcher.h:167)。

它会：

1. 创建 `ArenaMessageWrapper`。
2. 把 `rb->buf` 里的 1024 字节 wrapper 元信息复制出来。
3. `ParseFromArenaMessageWrapper()` 解析出共享内存中的真实 protobuf 对象地址 `msg_p`。
4. 从 `ProtobufArenaManager` 取出该消息关联的 block 列表。
5. 给这些 block 加读锁，避免消息对象在 listener 执行期间被覆盖。
6. 用自定义 deleter 形式包装 `msg`。
7. 调用用户 listener。
8. listener 返回后释放相关 block 的读锁。

这里的设计重点是：arena 模式下，listener 拿到的消息可能直接指向共享内存中的 protobuf 对象，而不是当前进程重新反序列化出来的一份普通堆对象。

### 普通 SHM 路径端到端链路

普通序列化消息从 writer 到 reader 的完整路径可以画成：

```text
Writer<T>::Write(msg)
  -> ShmTransmitter<T>::Transmit(msg)
  -> Segment::AcquireBlockToWrite()
  -> SerializeToArray(msg, wb.buf)
  -> 追加 MessageInfo
  -> ReleaseWrittenBlock()
  -> notifier_->Notify(ReadableInfo{channel_id, block_index})

ShmDispatcher::ThreadFunc()
  -> notifier_->Listen()
  -> 根据 channel_id 找到 Segment
  -> Segment::AcquireBlockToRead(block_index)
  -> OnMessage(...)
  -> listener adapter 反序列化 MessageT
  -> ShmReceiver::OnNewMessage()
  -> 用户 callback
  -> Segment::ReleaseReadBlock()
```

这条路径里：

- 一次 protobuf 序列化发生在写端。
- 一次 protobuf 反序列化发生在读端。
- 数据本体不经 socket，只在共享内存中传递。
- socket 或共享 notifier 只传“可读索引”，不传大消息本体。

### arena SHM 路径端到端链路

arena SHM 走的是另一条链：

```text
Writer<T>::Write(msg)
  -> ShmTransmitter<T>::Transmit(msg)
  -> ProtobufArenaManager::SetMessage(...)
  -> protobuf message 构造/落在 ArenaSegment 的共享内存中
  -> arena block 中只写入 ArenaMessageWrapper + MessageInfo
  -> notifier_->Notify(ReadableInfo{channel_id, arena_block_index})

ShmDispatcher::ThreadFunc()
  -> notifier_->Listen()
  -> ReadArenaMessage(channel_id, arena_block_index)
  -> OnArenaMessage(...)
  -> listener adapter 从 wrapper 恢复共享内存中的 protobuf 对象地址
  -> 对相关 block 加读锁
  -> 用户 callback 直接消费 arena message
  -> callback 返回后释放读锁
```

这里的收益是：

- 避免把完整 protobuf 再序列化成一大块 byte array 后读端再 parse 一次。
- 大对象消息可以更接近“对象级共享”，而不是“字节级共享”。

代价是：

- 生命周期管理更复杂，需要 `ProtobufArenaManager` 记录 message 对应 block。
- 读端必须在回调期间维护 block 读锁，防止 arena 对象底层内存被覆盖。

### SHM 这一层的真实分工

把这几个类的职责压缩成一句话：

- `ShmTransmitter`：写 block，发 `ReadableInfo`
- `Segment`：管理共享内存段和 block 锁
- `Notifier`：只负责“有新 block 可读了”的事件传播
- `ShmDispatcher`：后台等通知，拿 block，解析并分发
- `ShmReceiver`：把 reader 挂到 `ShmDispatcher`
- `ProtobufArenaManager`：arena 模式下管理共享内存中的 protobuf 对象布局与地址恢复

### 对照源码时最值得看的位置

- 写端入口：`cyber/transport/transmitter/shm_transmitter.h`
- 读端注册：`cyber/transport/receiver/shm_receiver.h`
- 读端后台分发：`cyber/transport/dispatcher/shm_dispatcher.h`
- 共享内存段本体：`cyber/transport/shm/segment.cc`
- 通知机制：`cyber/transport/shm/condition_notifier.cc`
- arena 扩展：`cyber/transport/shm/protobuf_arena_manager.cc`
