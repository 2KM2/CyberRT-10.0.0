# CyberRT 学习路线图（结合当前已掌握方向）

本文用于梳理 CyberRT 的整体学习方向、每条主线的作用、它们之间的连接关系，以及当前最值得补齐的“突破点”。

你当前已经掌握的方向包括：

1. **SHM 通信链路**
2. **RTPS / FastDDS 通信链路**
3. **拓扑发现 / 有向图机制**
4. **协程调度器**

这些内容已经覆盖了 CyberRT 通信内核的主体框架，但如果想把整体真正打通，还需要补若干关键“连接层”。

---

## 1. 当前学习状态：你已经掌握了什么

### 1.1 SHM 通信链路

你已经知道：

- `ShmTransmitter` 如何把消息写入共享内存
- `Segment` 如何管理共享内存 block
- `ReadableInfo` 如何作为通知描述符传递 block 索引
- `ShmDispatcher` 如何在后台线程里监听 notifier、读取 block、解析 `MessageInfo`
- `ListenerHandler` 如何继续向本进程分发

也就是：你已经看懂了 **同机跨进程共享内存通信** 的完整闭环。

---

### 1.2 RTPS / FastDDS 通信链路

你已经知道：

- `RtpsTransmitter` 会把业务消息封装为 `UnderlayMessage`
- `Publisher` 使用 DDS DataWriter 发送消息
- `MessageInfo` 的部分字段被编码到 `related_sample_identity`
- `SubscriberListener` 如何从 DDS 接收端把这些字段再解出来
- `RtpsDispatcher` 如何把字符串还原成业务消息并继续分发

也就是：你已经看懂了 **基于 FastDDS 的网络通信闭环**。

---

### 1.3 拓扑发现 / 有向图机制

你已经知道：

- `TopologyManager` 是拓扑总控中心
- `NodeManager / ChannelManager / ServiceManager` 负责不同维度的拓扑信息
- `ChannelManager` 内部有 `Graph node_graph_`
- Writer / Reader 通过共享 channel 自动生成有向边
- 系统可以判断 upstream / downstream / unreachable

也就是：你已经看懂了 **CyberRT 如何发现运行时关系，并将其抽象为有向图拓扑**。

---

### 1.4 协程调度器

你已经知道：

- `CRoutine` 是用户态协程对象
- `RoutineContext` 持有私有栈与上下文切换信息
- `Scheduler` 负责创建 / 分发 / 唤醒任务
- `Processor` 是真正执行协程的工作线程
- `DataVisitor` 是 Reader 与调度器之间的桥梁
- Reader 的用户回调不是直接执行，而是包装进循环协程中按需唤醒

也就是：你已经看懂了 **消息进入本进程后如何被挂起 / 唤醒 / 调度执行**。

---

## 2. 为什么现在还不能算“整体打通”

虽然你已经掌握了 4 条核心主线，但还缺少一些关键“连接层”。

这些连接层决定：

- 上层 API 如何把 transport / topology / scheduler 粘起来
- 系统如何自动选择 SHM / RTPS / Intra
- 消息是怎么从 Receiver 进入 DataVisitor 的
- Service / Client 请求响应如何在现有通信体系上实现
- 为什么几乎所有对象最终都依赖 `RoleAttributes`

也就是说，你现在已经懂了“很多重要零件”，下一步要补的是：

> **零件和零件之间的连接关系。**

---

## 3. 还需要重点补充的 5 个方向

---

## 3.1 `Reader / Writer` 抽象层（最高优先级）

### 为什么必须补

你已经学了：

- transport
- topology
- scheduler

但还缺一个“**上层 API 是怎么把这些机制串起来的**”的视角。

而 `Reader / Writer` 正是这个胶水层。

### 它连接了什么

#### Writer
连接：

- `TopologyManager`
- `Transmitter`

#### Reader
连接：

- `TopologyManager`
- `Receiver`
- `DataDispatcher`
- `Scheduler`

### 学完你会明白

- 为什么 Writer 初始化时要 Join 拓扑
- 为什么 Reader 初始化时既要建 Receiver，又要建协程
- 为什么 Reader 监听拓扑变化就能动态 Enable/Disable

### 关键突破点

> **Reader / Writer 不是简单的业务 API 包装层，而是 CyberRT 上层抽象与底层通信 / 调度 / 拓扑的汇合点。**

### 推荐文件

1. `cyber/node/writer.h`
2. `cyber/node/writer_base.h`
3. `cyber/node/reader.h`
4. `cyber/node/reader_base.h`

---

## 3.2 `HybridTransmitter / HybridReceiver`

### 为什么必须补

你已经分别看了：

- SHM
- RTPS

但真实运行时，很多时候不是手动指定“只用 SHM”或“只用 RTPS”，而是系统根据双方关系自动选择：

- 同进程 → INTRA
- 同机不同进程 → SHM
- 跨机 → RTPS

这个决策逻辑就在 Hybrid 层。

### 它连接了什么

- topology 中的 host / process / node 关系
- transport 中的多种具体实现
- receiver / transmitter 的动态 enable/disable

### 学完你会明白

- 系统为什么会自动选择通信模式
- 为什么同一个 channel 在不同对端条件下可能走不同路径
- 为什么拓扑信息不只是展示用，而是直接驱动链路建立

### 关键突破点

> **Hybrid 层把“发现关系”和“选择传输方式”绑定到了一起。**

### 推荐文件

1. `cyber/transport/transmitter/hybrid_transmitter.h`
2. `cyber/transport/receiver/hybrid_receiver.h`

---

## 3.3 `DataDispatcher / DataNotifier / ChannelBuffer`

### 为什么必须补

你已经知道：

- 协程不会在 transport 回调线程里直接执行
- `DataVisitor` 在中间取数据

但消息从 `Receiver` 到 `DataVisitor` 之间的“缓冲层”和“通知层”还没有完全补齐。

### 它连接了什么

- `Receiver::OnNewMessage`
- `DataDispatcher`
- `ChannelBuffer`
- `DataNotifier`
- `DataVisitor`
- `Scheduler`

### 学完你会明白

- 消息进入本进程后先存在哪里
- 谁在通知协程“现在有数据可取”
- 为什么 Reader 协程可以按自己的节奏取消息

### 关键突破点

> **DataDispatcher 这一层是“通信层”和“执行层”之间的解耦缓冲区。**

### 推荐文件

1. `cyber/data/data_dispatcher.*`
2. `cyber/data/data_notifier.*`
3. `cyber/data/channel_buffer.*`
4. `cyber/data/data_visitor_base.h`
5. `cyber/data/data_visitor.h`

---

## 3.4 `Service / Client` 请求响应链路

### 为什么值得补

你已经碰到过：

- `MessageInfo::spare_id`
- request / response 语义

而 service 通信和普通 pub/sub 是另一条重要支线。

### 它连接了什么

- request transmitter / receiver
- service topology
- `spare_id`
- `seq_num`
- pending request 映射

### 学完你会明白

- 为什么 `spare_id` 在 service 场景里特别关键
- response 是怎么精确匹配回原 client 的
- service 通信如何复用 transport 能力但形成 request/response 语义

### 关键突破点

> **Service 通信是在 pub/sub 基础设施上叠加“请求-响应配对语义”的结果。**

### 推荐文件

1. `cyber/service/client.h`
2. `cyber/service/service.h`
3. service 相关 transmitter / receiver 逻辑

---

## 3.5 基础身份模型：`RoleAttributes / Endpoint / Identity`

### 为什么要补

你现在已经反复遇到：

- `RoleAttributes`
- `Identity`
- `Endpoint`
- `channel_id`
- `host_ip`
- `process_id`
- `node_name`

如果不系统梳理，这些概念会一直是零散的。

### 它连接了什么

它其实是所有主线的共同基础：

- topology 用它描述角色
- transport 用它创建 endpoint
- hybrid 用它判断双方关系
- service 用它做请求响应配对

### 学完你会明白

- 一个 Reader / Writer / Client / Server 最小需要哪些身份信息
- `id`、`channel_id`、`node_name` 分别是什么层面的标识
- 为什么几乎所有运行时对象最终都依赖 `RoleAttributes`

### 关键突破点

> **RoleAttributes 是 CyberRT 几乎所有运行时对象的统一身份证。**

### 推荐文件

1. `cyber/proto/role_attributes.proto`（或生成后的使用点）
2. `cyber/transport/common/endpoint.h`
3. `cyber/transport/common/identity.h`

---

## 4. 你当前的知识图谱：已经有的 vs 还缺的

---

## 4.1 你已经掌握的主线

### 1. SHM
知道同机跨进程共享内存通信怎么走

### 2. RTPS
知道跨机 / 网络通信怎么走 DDS / FastDDS

### 3. Topology / Graph
知道系统如何发现谁和谁相连

### 4. Coroutine Scheduler
知道消息不直接执行，而是走调度器

---

## 4.2 你还缺的关键连接层

### 5. `Reader / Writer`
把 API、拓扑、传输、调度串起来

### 6. `HybridTransmitter / HybridReceiver`
决定到底选哪种传输方式

### 7. `DataDispatcher / Buffer / Notifier`
把 transport 和协程执行衔接起来

### 8. `Service / Client`
补齐请求响应模型

### 9. `RoleAttributes / Endpoint / Identity`
补齐统一身份模型

---

## 5. 如果按“突破点”来安排学习顺序

---

## 第一突破点：`Reader / Writer`

### 原因

这是从“看底层零件”走向“看整体骨架”的第一步。

### 解决的问题

> CyberRT 上层 API 为什么能自然调用到底层这么多机制？

### 学完后的变化

你会第一次真正把：

- transport
- topology
- scheduler

放到同一个对象模型里理解。

---

## 第二突破点：`HybridTransmitter / HybridReceiver`

### 原因

这是把 SHM 和 RTPS 两条平行线收束成一条“运行时选择逻辑”的关键。

### 解决的问题

> 系统到底什么时候走 SHM，什么时候走 RTPS，什么时候走 Intra？

### 学完后的变化

你会从“知道每种通信方式是什么”，进化到“知道系统如何自动选它们”。

---

## 第三突破点：`DataDispatcher / DataNotifier / ChannelBuffer`

### 原因

这是协程调度器真正接上消息流的地方。

### 解决的问题

> transport 收到消息后，协程到底靠什么被唤醒？

### 学完后的变化

你会把“消息到达”和“消息执行”之间的黑盒完全打开。

---

## 第四突破点：`Service / Client`

### 原因

这是普通 pub/sub 之外另一套非常关键的通信语义。

### 解决的问题

> 请求-响应是怎么在这套基础设施上叠加出来的？

### 学完后的变化

你会更完整地理解 CyberRT 的通信能力边界。

---

## 第五突破点：`RoleAttributes / Identity / Endpoint`

### 原因

这是统一概念层。

### 解决的问题

> 为什么这么多地方都在传 RoleAttributes，它到底是什么？

### 学完后的变化

你会发现很多以前零散的字段 suddenly 全通了。

---

## 6. 推荐的总学习地图

可以把后续学习路径记成这张图：

```text
【基础身份层】
RoleAttributes / Identity / Endpoint
        ↓
【拓扑发现层】
TopologyManager / ChannelManager / Graph
        ↓
【传输选择层】
HybridTransmitter / HybridReceiver
        ↓
【传输实现层】
Intra / SHM / RTPS
        ↓
【进程内分发层】
Dispatcher / ListenerHandler / Receiver
        ↓
【数据缓冲通知层】
DataDispatcher / ChannelBuffer / DataNotifier / DataVisitor
        ↓
【执行调度层】
CRoutine / Scheduler / Processor
        ↓
【用户抽象层】
Reader / Writer / Client / Service
```

这张图的意义很大，因为以后你每看一个文件，都能知道它属于系统的哪一层。

---

## 7. 如果只补“最必要的三块”，推荐这三个

如果你不想一下子铺得太大，建议优先补：

### 必补 1：`Reader / Writer`
因为它是上层 API 与底层机制的汇合点

### 必补 2：`HybridTransmitter / HybridReceiver`
因为它是通信方式选择中枢

### 必补 3：`DataDispatcher / DataNotifier / ChannelBuffer`
因为它是 transport 到 scheduler 的桥梁

这三块补完后，你对 CyberRT 通信主链路会基本形成闭环理解。

---

## 8. 当前最适合的学习策略

你现在最适合做的，不是继续钻某一个底层文件，而是先按路线图建立整体框架：

1. **先知道每条主线的职责**
2. **再知道它们之间如何连接**
3. **再挑突破点逐条深入**

这样后面阅读源码时，你不会陷在局部细节里迷失方向。

---

## 9. 一句话结论

结合你当前已经掌握的方向，**还需要重点补充的不是更多单点实现细节，而是 5 个关键连接层**：

1. `Reader / Writer`
2. `HybridTransmitter / HybridReceiver`
3. `DataDispatcher / DataNotifier / ChannelBuffer`
4. `Service / Client`
5. `RoleAttributes / Identity / Endpoint`

其中最优先补的是：

> **Reader/Writer、Hybrid、DataDispatcher 这三块。**

它们补齐之后，你就能把 SHM、RTPS、Topology、Scheduler 四条主线真正串成一个完整系统。