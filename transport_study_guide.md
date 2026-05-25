# CyberRT Transport Study Guide

这份笔记的目标不是一次把 `transport` 全部讲透，而是帮你建立一个稳定的学习框架。

如果你第一次看 CyberRT 的 `transport` 模块，最容易出的问题不是“某一行代码看不懂”，而是“看着看着不知道自己现在处在哪一层”。这份指南的核心目的，就是先帮你把层次分清，再逐步回到源码。

## 学习目标

读完这份笔记后，至少要做到下面几件事：

1. 能口头复述 `Writer::Write` 到 `Reader` 回调的大致链路。
2. 能分清 `Writer`、`Transmitter`、`Dispatcher`、`Receiver`、`DataDispatcher`、`Reader` 分别负责什么。
3. 知道 `intra`、`shm`、`rtps` 三条路径的共同点和不同点。
4. 知道第一次读源码时应该按什么顺序看，而不是一头扎进细节。

## 先建立整体心智模型

先不要把 `transport` 当成单纯的“网络通信模块”。它更像是 CyberRT 的一整套“消息投递系统”。

一条消息从发送到接收，大致会经过三层：

1. 发送层：`Writer -> Transmitter`
2. 传输分发层：`Dispatcher -> Receiver`
3. 数据消费层：`DataDispatcher -> Reader`

可以先记住下面这条主链：

```text
Writer::Write
-> transmitter_->Transmit(msg_ptr)
-> HybridTransmitter chooses intra/shm/rtps
-> concrete transport delivers message
-> Dispatcher finds listeners by channel_id
-> ListenerHandler runs registered receiver listener
-> Receiver::OnNewMessage
-> ReceiverManager-registered lambda
-> DataDispatcher::Dispatch(channel_id, msg)
-> ChannelBuffer / Blocker / DataVisitor path
-> Reader enqueue / reader callback
```

如果你现在只能记住这条链，已经够开始复盘了。

## 每层各自负责什么

### 1. Writer

`Writer` 负责“发起发送”，不负责决定底层怎么传。

它最重要的动作只有一个：

- 调 `transmitter_->Transmit(msg_ptr)`

所以你可以把它理解成“业务层发消息的入口”。

### 2. Transmitter

`Transmitter` 负责真正把消息送出去。

它并不是单一实现，而是一套抽象和具体实现组合：

- `Transmitter`：统一接口
- `HybridTransmitter`：调度器，决定走哪条路径
- `IntraTransmitter`：同进程路径
- `ShmTransmitter`：同机跨进程路径
- `RtpsTransmitter`：RTPS 路径

其中 `HybridTransmitter` 最关键，因为它负责选择发送模式。

### 3. Dispatcher

`Dispatcher` 负责“这条消息到了以后，应该交给谁”。

它是接收侧的中枢，不直接关心业务，只关心：

- 这个 `channel_id` 上有哪些 listener
- 该把消息扇出给哪些 receiver

也就是说，`Dispatcher` 解决的是“路由问题”。

### 4. Receiver

`Receiver` 不是最终业务回调，而是 transport 层的接收入口。

它负责接住从 dispatcher 过来的消息，然后把消息继续往上层交。

所以你要把它看成 transport 和 data 层之间的桥，而不是业务层终点。

### 5. DataDispatcher

`DataDispatcher` 负责把收到的消息放入对应 channel 的数据缓冲区，并发出通知。

它做的事情是：

- 找到 `channel_id` 对应的 buffer 集合
- 把消息填进去
- 通知上层可以消费了

这一层的意义是把 transport 层和 reader 层进一步解耦。

### 6. Reader

`Reader` 才是最接近业务使用者的那一层。

它不只是“收到消息就调一下回调”，还带着自己的缓存和消费逻辑。`Blocker`、`Observe()`、历史消息深度这些东西，都属于 reader 侧语义，而不是 transport 侧语义。

## `Writer::Write` 到 `Reader` 回调精读

这部分是你当前最需要反复复盘的主链。

### 第一段：Writer 如何把消息交给 transport

`Writer<MessageT>::Write(const MessageT& msg)` 做的事情很直接：

1. 检查自己是否已经初始化
2. 把消息包装成 `shared_ptr`
3. 调用 `Write(const std::shared_ptr<MessageT>&)`
4. 最终调用 `transmitter_->Transmit(msg_ptr)`

所以 `Writer` 不决定“走哪种通道”，它只负责“把消息交出去”。

一句话记忆：

- `Writer` 只关心“我要发”
- `Transmitter` 决定“怎么发”

### 第二段：Transmitter 如何真正发出去

抽象接口在 `cyber/transport/transmitter/transmitter.h`。

它统一定义了发送能力，并维护一些发送元信息，例如 `seq_num`。

具体发送时，并不是 `Writer` 直接选择 `intra/shm/rtps`，而是 `HybridTransmitter` 来决定。它内部维护多种 transmitter，然后根据对端关系和模式进行选择。

你现在先只需要知道：

- `Intra`：同进程
- `SHM`：同机不同进程
- `RTPS`：更通用的跨边界路径

### 第三段：为什么 Intra 最容易理解

`IntraTransmitter` 是理解整条链最好的入口。

因为它的发送逻辑非常短，几乎就是：

```cpp
dispatcher_->OnMessage(channel_id_, msg, msg_info);
```

这句话很重要，它说明：

- 同进程路径不需要网络
- 同进程路径通常不需要序列化
- 它本质上是在进程内把一个 `shared_ptr<MessageT>` 直接交给 dispatcher

这也是为什么 `intra` 适合拿来建立第一层理解。

### 第四段：RTPS 为什么不一样

`RtpsTransmitter` 的核心区别在于：

1. 先序列化消息
2. 再交给 RTPS publisher 写出去

所以你可以把 `intra` 和 `rtps` 的差别先记成一句话：

- `Intra` 传对象引用
- `RTPS` 传字节流

这也是为什么 `RTPS` 路径需要 `SerializeToString/ParseFromString`，而 `intra` 路径不需要。

### 第五段：消息进入 Dispatcher 后发生了什么

接收端的关键中枢是 `Dispatcher`。

它维护的核心映射关系可以概括成：

- `channel_id -> ListenerHandler`

这里最重要的一点是：它按 `channel_id` 路由，而不是按 node 名称或 reader 实例路由。

原因很简单：transport 分发面对的是“某个 channel 上来了消息”，所以第一步必须先按 channel 找到对应 listener 集合。

### 第六段：ListenerHandler 负责扇出

一个 channel 上可能挂着多个 listener。`ListenerHandler` 的作用就是统一管理这些回调连接，并在消息到来时统一 `Run(...)`。

所以你可以把这段逻辑理解成：

```text
某条消息进来
-> Dispatcher 找到这个 channel 的 handler
-> ListenerHandler 把消息发给所有相关 listener
```

这一步是“从一条消息到多个订阅者”的关键。

### 第七段：Receiver 接住 transport 层消息

具体 receiver 在 `Enable()` 时，会把自己的 `OnNewMessage` 注册到对应 dispatcher 里。

例如 `IntraReceiver::Enable()` 会：

1. 拿到 `IntraDispatcher`
2. 调 `AddListener`
3. 把 `OnNewMessage` 绑定进去

这意味着：

- receiver 不是主动拉消息
- receiver 是在 dispatcher 上注册回调，等别人来推

所以从 `IntraTransmitter::Transmit()` 往后，主链大致是：

```text
IntraTransmitter::Transmit
-> IntraDispatcher::OnMessage
-> ListenerHandler::Run
-> Receiver::OnNewMessage
```

### 第八段：为什么还没到用户 callback

这是最容易误判的一步。

很多人第一次看会以为：`Receiver::OnNewMessage` 之后就直接调用户 callback 了。实际上不是。

在 `ReaderBase` 的 `ReceiverManager<MessageT>::GetReceiver()` 中，创建 receiver 时传进去的监听器并不是最终业务 callback，而是一个中间 lambda。这个 lambda 会先把消息交给：

- `DataDispatcher<MessageT>::Dispatch(channel_id, msg)`

也就是说，transport 层在这里就结束了，它把消息交给 data 层，后面的 reader 侧处理是另一层职责。

### 第九段：DataDispatcher 做什么

`DataDispatcher` 维护的是：

- `channel_id -> BufferVector`

这表示同一个 channel 可以有多个 reader buffer。

消息到来后它会：

1. 找到这个 channel 对应的所有 buffer
2. 把消息填进每个 buffer
3. 发出通知

所以它不是业务层 callback 调度器，而是“数据投递到 reader 缓冲区”的统一入口。

### 第十段：Reader 为什么还有 Blocker

`Reader` 不只是一个“回调包装器”，它还维护自己的缓冲和观察能力。

`Blocker` 的作用可以先粗略理解为：

- 缓存最近收到的消息
- 限制缓冲深度
- 支持 `Observe()`、`GetLatestObserved()` 之类的读法

所以 reader 侧不是“收到即忘”，而是“收到后进入本地缓存，再由上层以自己的节奏消费”。

这就是为什么 `Reader` 比 `Receiver` 更靠近业务语义。

## 你现在最该记住的 4 个认识

1. `Writer` 不直接懂传输，它只调用 `Transmitter`。
2. `Receiver` 不等于用户 callback，它只是 transport 层接收入口。
3. `Dispatcher` 解决的是“按 channel 找到该通知谁”。
4. 用户 callback 前还有一层 `DataDispatcher + Blocker`，这是为了缓冲、通知和统一数据流。

## 三种传输路径怎么记

### Intra

场景：同一进程内通信。

特点：

- 最短路径
- 通常不需要序列化
- 直接依赖进程内 dispatcher

适合拿来建立第一层理解。

### SHM

场景：同机不同进程。

特点：

- 用共享内存传递数据
- 需要额外处理共享内存块和通知机制
- 即使有共享内存，仍然需要 dispatcher 做“谁该收到消息”的分发

### RTPS

场景：更通用的跨边界通信。

特点：

- 需要序列化和反序列化
- 更通用
- 适合跨进程、跨主机或更统一的发现与传输模式

## 第一次读源码的正确顺序

不要从 `shm` 内部实现开始读，那样很容易在细节里迷路。

建议顺序：

1. `cyber/node/writer.h`
2. `cyber/transport/transmitter/transmitter.h`
3. `cyber/transport/transmitter/intra_transmitter.h`
4. `cyber/transport/dispatcher/dispatcher.h`
5. `cyber/transport/message/listener_handler.h`
6. `cyber/transport/receiver/receiver.h`
7. `cyber/node/reader_base.h`
8. `cyber/data/data_dispatcher.h`
9. `cyber/blocker/blocker.h`
10. 最后再回头看 `shm` 和 `rtps`

这个顺序的好处是：

- 先建立骨架
- 再理解中枢分发
- 再进入 reader 侧
- 最后才看复杂路径实现

## 快速回忆版

如果你一段时间没看源码，先用下面这段回忆：

```text
Writer::Write
-> Transmitter::Transmit
-> HybridTransmitter 选择发送路径
-> Intra/Shm/Rtps 具体投递
-> Dispatcher 按 channel_id 找 listener
-> ListenerHandler 扇出给 Receiver
-> Receiver::OnNewMessage
-> DataDispatcher::Dispatch
-> Reader buffer / callback
```

如果这一段你能背出来，说明框架还在。

## 复盘题

每次学完一轮，你都可以试着脱离源码回答下面这些问题：

1. `Writer` 是什么时候创建 transmitter 的？
2. `HybridTransmitter` 如何决定走 `intra/shm/rtps`？
3. `Dispatcher` 维护的 key 是什么，为什么是 `channel_id`？
4. 为什么 RTPS 路径需要 `ParseFromString/SerializeToString`，而 `intra` 不需要？
5. SHM 为什么还需要 `Dispatcher`，它不是已经有共享内存了吗？
6. 为什么 `Receiver::OnNewMessage` 之后还不是最终业务 callback？
7. `DataDispatcher` 在整条链里到底解决了什么问题？
8. `Blocker` 对 `Reader` 的意义是什么？

## 推荐复盘方式

### 第一轮复盘

只看主链，不看细节：

- `Writer::Write -> Transmitter -> Dispatcher -> Receiver -> DataDispatcher -> Reader`

### 第二轮复盘

开始区分职责：

- 哪些层负责“传”
- 哪些层负责“路由”
- 哪些层负责“缓存和消费”

### 第三轮复盘

开始比较三条路径：

- 哪些部分在 `intra/shm/rtps` 中不同
- 哪些部分在三条路径中是共通骨架

### 第四轮复盘

尝试自己画一张图：

- 从 `Writer::Write` 画到 `Reader callback`
- 尽量不看源码
- 画完后再回源码核对

## 如果现在理解还不牢，很正常

第一次接触这块源码时，慢是正常的。原因不是你学得慢，而是它本来就跨了三层：

- `node`
- `transport`
- `data`

而且中间又混合了：

- 模板
- 回调
- 单例
- 缓冲区
- 多种传输后端

所以第一次学习不应该追求“全部理解”，而应该追求三件事：

1. 先能背主链
2. 先能分层
3. 先不要钻太深的实现细节

如果你现在只能清楚说出：

- `Writer` 交给 `Transmitter`
- `Dispatcher` 负责按 channel 路由
- `Receiver` 之后还要进 `DataDispatcher`
- `Reader` 这边有缓存和回调

那就已经是有效理解了。

## 下一步建议

当你觉得这份笔记差不多能消化后，推荐按下面顺序继续深入：

1. 精读 `Reader::Init`，把 reader 侧消费路径补完整。
2. 精读 `DataVisitor/ChannelBuffer/Blocker`，弄清 reader 为什么不是直接回调。
3. 再深入 `shm` 路径，理解共享内存传输为什么仍然需要 dispatcher。
4. 最后回头总结 `intra/shm/rtps` 的边界条件和适用场景。

这时候再看 `transport`，就不会再像第一次那样散。
