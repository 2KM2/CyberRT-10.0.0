# CyberRT 基于有向图的拓扑机制学习笔记

本文聚焦 CyberRT 的 **Topology / Service Discovery** 机制，重点解释：

- `TopologyManager` 总体设计
- `NodeManager / ChannelManager / ServiceManager` 的职责划分
- 为什么说它是“基于有向图”的拓扑系统
- `Graph` 如何表达 upstream / downstream 关系
- Reader / Writer / Client / Server 如何参与这套拓扑

---

## 1. 先给结论

CyberRT 把系统中的这些元素抽象成一个**运行时动态维护的有向拓扑图**：

- **Node**：节点
- **Writer / Reader**：消息发布者 / 订阅者
- **Server / Client**：服务提供者 / 请求者
- **Channel / Service**：连接关系

这个设计在 `TopologyManager` 的类注释中已经明确说明：

- `cyber/service_discovery/topology_manager.h:48-62`

核心意思是：

> 可以把整个 Cyber 系统想成一张有向图：
>
> - Node 是容器
> - Writer / Reader / Server / Client 是图中的参与者
> - Channel 是 Writer 指向 Reader 的边
> - Service 是 Server 指向 Client 的边
>
> 因此：
> - Writer / Server 被看作 **Upstream**
> - Reader / Client 被看作 **Downstream**

---

## 2. 拓扑图中的“点”和“边”

## 2.1 点（Vertex）是什么

从系统语义上看，最关键的“点”是 **Node**。

例如：

- Node A 有一个 Writer
- Node B 有一个 Reader
- A 的 Writer 发布 `channel-1`
- B 的 Reader 订阅 `channel-1`

那么拓扑图上就有：

```text
A ---> B
```

也就是：

- A 是 B 的上游（Upstream）
- B 是 A 的下游（Downstream）

这个语义也在 `ChannelManager` 的接口注释里被明确写出：

- `cyber/service_discovery/specific_manager/channel_manager.h:153-183`

---

## 2.2 边（Edge）是什么

### 在消息通信场景中
边是 **Channel**。

也就是说：

> Writer 通过某个 channel，把数据流向 Reader。

可以写成：

```text
NodeA --(channel-1)--> NodeB
```

### 在服务调用场景中
边是 **Service**。

也就是说：

> Server 通过某个 service，面向 Client 提供调用关系。

可以写成：

```text
ServerNode --(service-x)--> ClientNode
```

---

## 3. `TopologyManager`：拓扑总控中心

定义在：

- `cyber/service_discovery/topology_manager.h:64`

它是整套拓扑发现机制的总入口。

---

## 3.1 它管理 3 个子管理器

`TopologyManager` 内部持有：

- `node_manager()`
  `cyber/service_discovery/topology_manager.h:97`
- `channel_manager()`
  `cyber/service_discovery/topology_manager.h:102`
- `service_manager()`
  `cyber/service_discovery/topology_manager.h:107`

所以可以把它理解成：

> **TopologyManager = 拓扑总控中心**
>
> 下分为：
> - NodeManager：管节点
> - ChannelManager：管消息通信拓扑
> - ServiceManager：管服务调用拓扑

---

## 3.2 它如何感知拓扑变化

从注释和接口可看出，TopologyManager 通过 FastRTPS / FastDDS 的 `Participant` 进行拓扑发现与广播：

- `cyber/service_discovery/topology_manager.h:60`
- `cyber/service_discovery/topology_manager.h:116-123`

内部相关接口：

- `CreateParticipant()`
- `OnParticipantChange(...)`
- `Convert(...)`
- `ParseParticipantName(...)`

这说明：

- 进程加入 / 离开
- Writer / Reader / Client / Server 加入 / 离开

最终都会转成统一的 `ChangeMsg` 事件，再交给下面的 manager 处理。

---

## 3.3 它支持监听拓扑变化

接口：

- `AddChangeListener(...)`
  `cyber/service_discovery/topology_manager.h:87`
- `RemoveChangeListener(...)`
  `cyber/service_discovery/topology_manager.h:92`

说明这套系统不仅是“存储拓扑结构”，还是一个**事件驱动的运行时发现系统**。

---

## 4. `Manager` 抽象层：统一 Join / Leave / Notify 机制

3 个具体 manager 的共同基类是：

- `cyber/service_discovery/specific_manager/manager.h:54`

这个基类把拓扑变化抽象成统一模式。

---

## 4.1 基类 `Manager` 的关键能力

### Join
- `cyber/service_discovery/specific_manager/manager.h:100`

```cpp
bool Join(const RoleAttributes& attr, RoleType role,
          bool need_publish = true);
```

### Leave
- `cyber/service_discovery/specific_manager/manager.h:112`

```cpp
bool Leave(const RoleAttributes& attr, RoleType role);
```

### 监听变化
- `AddChangeListener(...)`
  `cyber/service_discovery/specific_manager/manager.h:121`
- `RemoveChangeListener(...)`
  `cyber/service_discovery/specific_manager/manager.h:128`

### 发布 / 处理远端变化
- `Publish(...)`
  `cyber/service_discovery/specific_manager/manager.h:151`
- `OnRemoteChange(...)`
  `cyber/service_discovery/specific_manager/manager.h:152`

所以这一层的核心思想是：

> 拓扑中的每个元素（Node / Writer / Reader / Server / Client）
> 都通过 **Join / Leave 消息** 进入或退出全局拓扑。

---

## 4.2 `ChangeMsg` 是统一的拓扑变化载体

基类 `Manager` 统一使用：

- `ChangeMsg`
- `RoleAttributes`
- `RoleType`
- `OperateType`

见：

- `cyber/service_discovery/specific_manager/manager.h:43-47`

这说明拓扑变化不是“直接改一块共享状态”，而是先抽象成一条**拓扑变更事件**。

---

## 5. `RoleAttributes`：每个参与者的身份卡

所有参与拓扑的对象最终都靠：

- `proto::RoleAttributes`

来描述自己的身份。

这些 role 又被封装成：

- `RoleBase`
- `RoleWriter`
- `RoleServer`

定义在：

- `cyber/service_discovery/role/role.h:45-84`

---

## 5.1 `RoleBase`

`RoleBase` 保存两类信息：

- `attributes_`
- `timestamp_ns_`

见：

- `cyber/service_discovery/role/role.h:61-63`

也就是：

- 它是谁（属性）
- 它什么时候加入 / 更新时间是多少

---

## 5.2 `RoleWriter` / `RoleReader`

在这套实现里，`RoleReader` 直接复用了 `RoleWriter`：

- `cyber/service_discovery/role/role.h:35-38`

```cpp
using RoleWriterPtr = std::shared_ptr<RoleWriter>;
using RoleReader = RoleWriter;
```

这说明从拓扑匹配的角度看，Reader / Writer 都是“带属性的角色对象”，只是角色语义不同。

---

## 5.3 `RoleServer` / `RoleClient`

同样地：

- `RoleClient = RoleServer`
  `cyber/service_discovery/role/role.h:40-43`

即服务调用拓扑也复用同样的 role 抽象模式。

---

## 6. `NodeManager`：节点集合管理

定义在：

- `cyber/service_discovery/specific_manager/node_manager.h:38`

职责：

- 判断某个 node 是否存在
- 获取所有 nodes
- 处理 node join / leave

核心数据结构：

- `NodeWarehouse nodes_`
  `cyber/service_discovery/specific_manager/node_manager.h:79`

所以 `NodeManager` 的定位比较直接：

> **维护系统里现在有哪些节点。**

它不直接维护图边，只维护节点集合。

---

## 7. `ServiceManager`：服务调用拓扑管理

定义在：

- `cyber/service_discovery/specific_manager/service_manager.h:39`

核心数据结构：

- `ServerWarehouse servers_`
  `cyber/service_discovery/specific_manager/service_manager.h:89`
- `ClientWarehouse clients_`
  `cyber/service_discovery/specific_manager/service_manager.h:90`

它负责：

- 某个 service 是否存在
- 有哪些 server
- 有哪些 client

这也是一种“有向关系”，只不过更偏向服务调用方向：

```text
Server ---> Client
```

不过如果你现在主线是 transport / channel / reader / writer，那么最值得深入的是 **ChannelManager**。

---

## 8. `ChannelManager`：真正把通信关系组织成有向图的核心

定义在：

- `cyber/service_discovery/specific_manager/channel_manager.h:43`

这是整套“基于有向图的拓扑设计”里最关键的类。

---

## 8.1 ChannelManager 管哪些东西

它维护两类索引：

### 节点维度
- `node_writers_`
- `node_readers_`
- `node_graph_`

见：

- `cyber/service_discovery/specific_manager/channel_manager.h:207-210`

```cpp
Graph node_graph_;
WriterWarehouse node_writers_;
ReaderWarehouse node_readers_;
```

### Channel 维度
- `channel_writers_`
- `channel_readers_`

见：

- `cyber/service_discovery/specific_manager/channel_manager.h:212-214`

```cpp
WriterWarehouse channel_writers_;
ReaderWarehouse channel_readers_;
```

所以它同时保留：

1. **按节点查询**
2. **按 channel 查询**
3. **按图关系查询**

---

## 8.2 为什么需要两套索引 + 一张图

因为系统里有三类不同问题。

### A. 资源视角问题
例如：

- 某个 channel 有哪些 writers？
- 某个 channel 有哪些 readers？

对应接口：

- `GetWritersOfChannel(...)`
  `channel_manager.h:116`
- `GetReadersOfChannel(...)`
  `channel_manager.h:150`

### B. 节点视角问题
例如：

- 某个 node 有哪些 writers？
- 某个 node 有哪些 readers？

对应接口：

- `GetWritersOfNode(...)`
  `channel_manager.h:108`
- `GetReadersOfNode(...)`
  `channel_manager.h:142`

### C. 拓扑视角问题
例如：

- 某个 node 的上游是谁？
- 某个 node 的下游是谁？
- A 到 B 的流向是什么？

对应接口：

- `GetUpstreamOfNode(...)`
  `channel_manager.h:162`
- `GetDownstreamOfNode(...)`
  `channel_manager.h:174`
- `GetFlowDirection(...)`
  `channel_manager.h:182`

前两类靠 warehouse 足够；第三类必须依赖图结构。

---

## 9. `Graph`：有向图容器本体

定义在：

- `cyber/service_discovery/container/graph.h:93`

这个类就是专门为拓扑关系维护准备的图结构。

---

## 9.1 图支持的方向语义

在：

- `cyber/service_discovery/container/graph.h:39-43`

```cpp
enum FlowDirection {
  UNREACHABLE,
  UPSTREAM,
  DOWNSTREAM,
};
```

这说明 Graph 不是一个泛用图算法库，而是一个专门为“流向判断”设计的图容器。

---

## 9.2 图支持的核心操作

- `Insert(const Edge& e)`
  `graph.h:101`
- `Delete(const Edge& e)`
  `graph.h:102`
- `GetNumOfEdge()`
  `graph.h:104`
- `GetDirectionOf(lhs, rhs)`
  `graph.h:105`

所以它承担的核心角色是：

> **维护有向边，并判断两个节点之间的数据流向关系。**

---

## 9.3 图的内部结构

核心成员：

- `EdgeInfo edges_`
- `AdjacencyList list_`

见：

- `cyber/service_discovery/container/graph.h:124-126`

```cpp
EdgeInfo edges_;
AdjacencyList list_;
base::AtomicRWLock rw_lock_;
```

说明内部同时维护：

- 边信息
- 邻接表
- 并发安全读写锁

---

## 10. 从 `graph_test.cc` 看 Graph 的语义最直观

测试文件：

- `cyber/service_discovery/container/graph_test.cc:94-206`

测试里构造了这张图：

```text
a -> b
c -> d
c -> e
a -> c
b -> e
```

然后验证：

- `a -> b` 是 `UPSTREAM`
- `b -> a` 是 `DOWNSTREAM`
- `a -> e` 是 `UPSTREAM`
- `e -> a` 是 `DOWNSTREAM`
- `b -> d` 是 `UNREACHABLE`

这说明：

> `GetDirectionOf(lhs, rhs)` 不只判断直接边，
> 它判断的是**图上的可达关系**。

也就是说如果：

```text
a -> c -> e
```

那么：

- `GetDirectionOf(a, e) == UPSTREAM`
- `GetDirectionOf(e, a) == DOWNSTREAM`

这也是“拓扑图”比单纯索引表更强大的地方。

---

## 11. ChannelManager 如何把 Writer/Reader 关系映射成图边

虽然这次没有展开 `channel_manager.cc` 的实现细节，但从接口注释和成员语义，建图规则已经很清楚。

### 基本规则
如果：

- Node A 有 Writer 发布某个 channel
- Node B 有 Reader 订阅同一个 channel

那么图中会建立：

```text
A ---> B
```

这个规则在：

- `cyber/service_discovery/specific_manager/channel_manager.h:153-169`

已经用注释明确说明。

---

## 11.1 边的含义

- **源点（src）**：Writer 所在节点
- **终点（dst）**：Reader 所在节点
- **边值（edge value）**：某个 channel 语义连接

因此：

- Writer 所在节点 = 上游节点
- Reader 所在节点 = 下游节点

---

## 11.2 边的更新时机

`ChannelManager` 在 Join / Leave 时会更新拓扑：

- `DisposeJoin(...)`
  `channel_manager.h:200`
- `DisposeLeave(...)`
  `channel_manager.h:201`

也就是说，当：

- Writer 加入
- Reader 加入
- Writer 离开
- Reader 离开

系统都会动态修改：

- warehouse 索引
- `node_graph_`

所以这不是静态配置图，而是**运行时动态维护的图**。

---

## 12. Reader / Writer 如何参与这套拓扑系统

这一点和 transport 主线非常相关。

---

## 12.1 Reader 启动时加入拓扑

在：

- `cyber/node/reader.h:344-345`

```cpp
channel_manager_->Join(this->role_attr_, proto::RoleType::ROLE_READER,
                       message::HasSerializer<MessageT>::value);
```

意思是：

> Reader 初始化时，会向 ChannelManager 声明：
> “我这个节点正在订阅这个 channel。”

---

## 12.2 Reader 还会监听拓扑变化

在：

- `cyber/node/reader.h:334-335`

```cpp
change_conn_ = channel_manager_->AddChangeListener(std::bind(
    &Reader<MessageT>::OnChannelChange, this, std::placeholders::_1));
```

这说明 Reader 不只是初始化时登记一次，而是会持续监听 Writer 的变化：

- Writer 新加入时：`receiver_->Enable(writer)`
- Writer 离开时：`receiver_->Disable(writer_attr)`

相关逻辑在：

- `cyber/node/reader.h:355-370`

因此：

> 拓扑系统是 Reader / Writer 动态协作的基础设施，不只是一个“信息登记表”。

---

## 12.3 Writer 也会加入拓扑

Writer 侧也会向 ChannelManager 注册自己的 `RoleAttributes`，告诉系统：

- 我在哪个 node
- 我发布哪个 channel
- 我的 message type 是什么

这样 Reader 才能在运行时发现 Writer，并建立接收路径。

---

## 13. 这套“基于有向图”的设计到底解决了什么问题

---

## 13.1 如果只有注册表，不够

如果系统只是简单维护：

- 某个 channel 有哪些 writers
- 某个 channel 有哪些 readers

那么它只能回答“资源存在性”问题。

但很多场景还需要回答：

- A 是不是 B 的上游？
- B 的所有上游节点是谁？
- C 的所有下游节点是谁？
- A 和 D 之间是否可达？

这些问题本质上都属于**图上的关系推理**。

---

## 13.2 有向图让系统具备“拓扑推理能力”

通过 `Graph::GetDirectionOf(...)`，系统可以判断：

- 直接相连
- 间接可达
- 不可达

这使得 CyberRT 的拓扑机制不只是“发现组件”，而是能够：

> **推理组件之间的流向关系。**

---

## 13.3 图上的方向代表“数据流向”

要特别注意：

这里的图方向不是：

- 函数调用方向
- 线程依赖方向
- 类继承方向

而是：

> **消息 / 服务从谁流向谁**

因此：

- Writer / Server = upstream
- Reader / Client = downstream

---

## 13.4 图是动态的

随着：

- Join
- Leave
- 进程退出
- Participant 消失

图会不断更新。

这也是它和“静态 DAG 配置图”最大的区别。

---

## 14. 如何用三层结构记住这套机制

建议你把它记成三层：

---

### 第一层：事件层

由 `Manager` 抽象提供：

- Join
- Leave
- Publish
- Notify
- OnRemoteChange

即：**拓扑变化先变成事件消息。**

---

### 第二层：资源层

由三个 manager 分管：

- `NodeManager`：节点集合
- `ChannelManager`：writer/reader 集合
- `ServiceManager`：server/client 集合

即：**维护系统里有哪些实体。**

---

### 第三层：关系层

主要由 `ChannelManager::node_graph_` 体现：

- 节点间建立有向边
- 支持 upstream / downstream / unreachable 判断

即：**维护系统里的数据流向关系。**

---

## 15. 一句话总结

> CyberRT 的拓扑机制本质上是一个**基于运行时发现事件维护的有向图系统**：Node 是图上的实体，Writer/Reader 通过共享 Channel 自动生成有向边，TopologyManager 负责收集和分发拓扑变化，ChannelManager 用 `Graph` 维护节点间的 upstream/downstream 数据流关系。

---

## 16. 建议后续阅读顺序

如果你后面想继续顺源码深入，建议按这个顺序看：

1. `cyber/service_discovery/topology_manager.h`
2. `cyber/service_discovery/specific_manager/manager.h`
3. `cyber/service_discovery/role/role.h`
4. `cyber/service_discovery/specific_manager/node_manager.h`
5. `cyber/service_discovery/specific_manager/service_manager.h`
6. `cyber/service_discovery/specific_manager/channel_manager.h`
7. `cyber/service_discovery/container/graph.h`
8. `cyber/service_discovery/container/graph_test.cc`
9. 再去看 `channel_manager.cc` 里 Join/Leave 如何具体增删图边

这样理解会比较顺。