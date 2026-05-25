# CyberRT Transport: Writer::Write To Reader Callback

`Writer::Write` 到 `Reader` 回调并不是一条直接函数调用链，中间会经过两次解耦：

1. 发送侧通过 `Transmitter` 把消息送进某种传输通道。
2. 接收侧先进入 `DataDispatcher` 和 `Blocker`，再由 `Reader` 自己触发用户回调。

所以读源码时，不要期待一个简单的 `Write -> ... -> user_callback` 静态调用链。它是“传输层投递 + 数据层分发 + Reader 层消费”三段式。

## 第一段：Writer 如何把消息交给 transport

入口在 `cyber/node/writer.h`。

`Writer<MessageT>::Write(const MessageT& msg)` 做三件事：

1. 检查 `Writer` 是否已经 `Init`
2. 把栈对象包成 `shared_ptr`
3. 调另一个重载 `Write(const std::shared_ptr<MessageT>&)`

真正发消息的是：

- `cyber/node/writer.h:173`
- `return transmitter_->Transmit(msg_ptr);`

也就是说，`Writer` 本身不负责传输细节，它只是调用自己持有的 `transmitter_`。

这个 `transmitter_` 是在 `Writer::Init()` 时创建的。整体结构上可以确定：`Writer` 初始化时会向 `Transport` 要一个对应 channel 的 transmitter。

心智模型：

- `Writer` 只关心“我要发”
- `Transmitter` 决定“怎么发”

## 第二段：Transmitter 如何真正发出去

抽象基类在 `cyber/transport/transmitter/transmitter.h`。

这里最关键的是两个接口：

- `Transmit(const MessagePtr& msg)`
- `Transmit(const MessagePtr& msg, const MessageInfo& msg_info)`

其中基类还维护了 `seq_num`，见 `cyber/transport/transmitter/transmitter.h:55`。这说明 transport 层会给消息附带发送元信息，而不只是裸消息体。

具体走哪条链路，不是 `Writer` 决定，而是 `HybridTransmitter` 决定。它在 `cyber/transport/transmitter/hybrid_transmitter.h:51`，内部维护多种 transmitter：

- `IntraTransmitter`
- `ShmTransmitter`
- `RtpsTransmitter`

对应三种路径：

- 同进程：`Intra`
- 同机跨进程：`SHM`
- 更通用的 RTPS：`RTPS`

最容易读懂的是 `IntraTransmitter`。在 `cyber/transport/transmitter/intra_transmitter.h:99`，它的 `Transmit` 几乎就是：

```cpp
dispatcher_->OnMessage(channel_id_, msg, msg_info);
```

这很关键。它说明同进程路径根本不需要网络、不需要共享内存，通常也不需要序列化，直接把 `shared_ptr<MessageT>` 丢给进程内 dispatcher。

而 `RtpsTransmitter` 则明显不同。在 `cyber/transport/transmitter/rtps_transmitter.h:122`：

1. 先把 `msg` 序列化到 `UnderlayMessage`
2. 再写入 `publisher_->Write(...)`

所以“为什么 RTPS 要序列化、intra 不需要”的答案是：

- `Intra` 传的是进程内对象引用
- `RTPS` 传的是跨边界字节流

## 第三段：消息到达接收侧后先进入 Dispatcher

接收抽象在 `cyber/transport/receiver/receiver.h`。

`Receiver` 本身不处理业务，它只持有一个 `MessageListener`，然后在收到消息时通过 `OnNewMessage` 继续往上交。这个 `OnNewMessage` 是 transport 到上层的桥。

关键问题是：谁调用 `Receiver::OnNewMessage`？

答案是具体 receiver 注册到 dispatcher 里的 listener。

比如 `IntraReceiver::Enable()` 在 `cyber/transport/receiver/intra_receiver.h:59`：

1. 拿到 `IntraDispatcher::Instance()`
2. 调 `dispatcher_->AddListener<M>(...)`
3. 绑定的回调就是 `IntraReceiver<M>::OnNewMessage`

也就是说，receiver 并不是主动拉消息，而是在 dispatcher 上注册了一个“来消息时请调用我”的回调。

`Dispatcher` 的通用结构在 `cyber/transport/dispatcher/dispatcher.h:55`。

最重要的成员是：

- `AtomicHashMap<uint64_t, ListenerHandlerBasePtr> msg_listeners_`

注释已经写明 key 是 `channel_id`，见 `cyber/transport/dispatcher/dispatcher.h:82`。

`AddListener` 的核心逻辑在 `cyber/transport/dispatcher/dispatcher.h:87`：

1. 用 `channel_id` 找 handler
2. 如果没有，就新建 `ListenerHandler<MessageT>`
3. 把当前 reader 对应的 listener 连接进去

这解释了为什么 key 是 `channel_id`。因为 transport 分发首先按“消息属于哪个 channel”来路由，而不是按 reader 实例或 node 名称路由。

## 第四段：ListenerHandler 把消息扇出给 Receiver

真正的多 listener 分发器是 `ListenerHandler`，在 `cyber/transport/message/listener_handler.h:60`。

它内部维护连接表，把多个 reader 的回调挂在同一个 channel handler 上。消息一到，就统一 `Run(...)`。

所以从 `IntraTransmitter::Transmit()` 往后，逻辑可以理解成：

```text
IntraTransmitter::Transmit
  -> IntraDispatcher::OnMessage
  -> ListenerHandler::Run
  -> Receiver::OnNewMessage
```

SHM 路径也类似，只是中间先从共享内存块里把消息取出来。可以看 `cyber/transport/dispatcher/shm_dispatcher.cc:113`：

- `ShmDispatcher::OnMessage(...)`
- 找到 `ListenerHandler<ReadableBlock>`
- `handler->Run(rb, msg_info)`

RTPS 路径则在 `cyber/transport/dispatcher/rtps_dispatcher.h:85`。它会先把底层字符串反序列化成 `MessageT`，然后再调用上层 listener。

所以 transport 层到这里为止，做完的是：

1. 把消息送到正确 channel
2. 找到这个 channel 的 receiver listener
3. 调用 `Receiver::OnNewMessage`

但还没到用户 callback。

## 第五段：Receiver 并不直接调用用户回调，而是先投递给 DataDispatcher

这是最容易漏掉的一层，也是理解 `Reader` 的关键。

在 `cyber/node/reader_base.h:199`，`ReceiverManager<MessageT>::GetReceiver()` 第一次为某个 channel 创建 receiver 时，传给 `Transport::CreateReceiver<MessageT>(...)` 的监听器不是用户 callback，而是一个 lambda。

lambda 里面做的事是：

1. 记录 perf event
2. 调 `data::DataDispatcher<MessageT>::Instance()->Dispatch(reader_attr.channel_id(), msg);`
3. 再记一次 perf event

这意味着 transport 层收到消息后，不是直接调 `Reader` 构造时传进来的业务函数，而是统一交给 `DataDispatcher`。

这是整个主链里最重要的第二次解耦。

## 第六段：DataDispatcher 把消息填进 Reader 的缓存，并发出通知

`DataDispatcher` 在 `cyber/data/data_dispatcher.h:38`。

它维护的是：

- `channel_id -> BufferVector`

也就是说，同一个 channel 可以有多个 buffer。每个 `Reader` 都会把自己的 buffer 注册进来。

`Dispatch()` 在 `cyber/data/data_dispatcher.h:73` 做的事很明确：

1. 用 `channel_id` 找所有 buffer
2. 把 `msg` 填进去：`buffer->Fill(msg)`
3. 调 `notifier_->Notify(channel_id)`

这里说明 `Reader` 收消息不是“直接收到一个参数”，而是“所属 buffer 被填充了，同时收到 channel 级通知”。

## 第七段：Reader 自己消费 buffer，并最终触发用户回调

`Reader` 本身在 `cyber/node/reader.h:69`。

它除了持有 `receiver_`，还持有一个很关键的对象：

- `blocker_`，见 `cyber/node/reader.h:224`

`Reader::Enqueue` 在 `cyber/node/reader.h:247`。虽然这次没有把函数体完整展开，但结合 `Blocker` 的结构已经足够推断它的职责：把收到的消息推进 `blocker_` 的 published queue。

`Blocker::Enqueue` 的具体实现能看到，在 `cyber/blocker/blocker.h:265`：

1. 往 `published_msg_queue_` 头部插入消息
2. 超过容量就从尾部淘汰

所以 `Reader` 不是“收到就立刻只调回调”，它默认还维护了一份本地历史缓存。这个缓存对应平时看到的：

- `Observe()`
- `GetLatestObserved()`
- `PendingQueueSize()`

也解释了为什么 CyberRT 的 reader 天然支持“回看最近若干条消息”。

更重要的是，`ReaderBase` 顶部注释已经把流程写得很直白，在 `cyber/node/reader_base.h:159`：

- receiver 收到消息
- push 到 `ChannelBuffer`
- `DataVisitor` fetch 数据
- 再传给 `Reader` 的 callback

所以最终用户回调的触发，不是 transport 线程直接裸调，而是经过 data 层和 reader 自己的消费路径。

## 整条链压成一张图

```text
Writer::Write
  -> transmitter_->Transmit(msg_ptr)
  -> HybridTransmitter 选择具体通道

同进程时：
  -> IntraTransmitter::Transmit
  -> IntraDispatcher::OnMessage
  -> ListenerHandler::Run
  -> Receiver::OnNewMessage
  -> ReceiverManager 里注册的 lambda
  -> DataDispatcher::Dispatch(channel_id, msg)
  -> ChannelBuffer / Blocker / DataVisitor
  -> Reader::Enqueue / Reader callback
```

如果是 RTPS 或 SHM，只是中间“底层送达”的那段不同：

- RTPS 多了序列化/反序列化
- SHM 多了共享内存块读写和 notifier
- 后半段进入 `Receiver -> DataDispatcher -> Reader` 的框架基本一致

## 应该抓住的 4 个认识

1. `Writer` 不直接懂传输，它只调用 `Transmitter`。
2. `Receiver` 不直接等于用户 callback，它只是 transport 层入口。
3. `Dispatcher` 解决的是“按 channel 找到该通知谁”。
4. 用户 callback 前还有一层 `DataDispatcher + Blocker`，这是为了缓存、通知和统一数据流。

## 推荐回读顺序

1. `cyber/node/writer.h:166`
2. `cyber/transport/transmitter/transmitter.h:37`
3. `cyber/transport/transmitter/intra_transmitter.h:99`
4. `cyber/transport/dispatcher/dispatcher.h:87`
5. `cyber/transport/message/listener_handler.h:60`
6. `cyber/transport/receiver/receiver.h:32`
7. `cyber/node/reader_base.h:199`
8. `cyber/data/data_dispatcher.h:73`
9. `cyber/blocker/blocker.h:265`
